// Synchronous RPC client.
//
// Framing is identical to the native messaging protocol (4-byte little-endian
// length + UTF-8 JSON), so the same FrameReader serves both. Requests are
// correlated by id; events that arrive while a call is in flight are ignored on
// the request connection because subscribers use a dedicated one.
#include <contextsnap/ipc/client.hpp>

#include <contextsnap/core/config.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/storage/serialization.hpp>
#include <contextsnap/version.hpp>

#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace contextsnap::ipc {
namespace {

using core::json::Value;

constexpr const char* kContext = "ipc client";

core::Error error_from(const Response& response) {
    const std::string& message =
        response.error_message.empty() ? std::string("daemon reported a failure")
                                       : response.error_message;
    switch (response.error_code) {
        case core::ErrorCode::NotFound:
            return core::err::not_found(message, kContext);
        case core::ErrorCode::InvalidArgument:
            return core::err::invalid(message, kContext);
        case core::ErrorCode::Unsupported:
            return core::err::unsupported(message, kContext);
        case core::ErrorCode::PermissionDenied:
            return core::err::denied(message, kContext);
        case core::ErrorCode::ProtocolError:
            return core::err::protocol(message, kContext);
        case core::ErrorCode::IoError:
            return core::err::io(message, kContext);
        case core::ErrorCode::DatabaseError:
            return core::err::database(message, kContext);
        default:
            return core::err::internal(message, kContext);
    }
}

Value to_value(const std::vector<std::string>& values) {
    core::json::Array array;
    for (const std::string& value : values) {
        array.emplace_back(Value(value));
    }
    return Value(std::move(array));
}

std::vector<std::string> string_list(const Value& value, const char* key) {
    std::vector<std::string> values;
    const Value* found = value.find(key);
    if (found == nullptr || !found->is_array()) {
        return values;
    }
    for (const Value& entry : found->as_array()) {
        values.push_back(entry.as_string());
    }
    return values;
}

std::string string_field(const Value& value, const char* key) {
    const Value* found = value.find(key);
    return found == nullptr ? std::string{} : found->as_string();
}

std::uint32_t unsigned_field(const Value& value, const char* key) {
    const Value* found = value.find(key);
    return found == nullptr ? 0u : static_cast<std::uint32_t>(found->as_int());
}

bool bool_field(const Value& value, const char* key) {
    const Value* found = value.find(key);
    return found != nullptr && found->as_bool();
}

/// Timestamps are RFC 3339 strings, but tolerate epoch millis for robustness.
core::Timestamp timestamp_field(const Value& value, const char* key) {
    const Value* found = value.find(key);
    if (found == nullptr) {
        return core::Timestamp{};
    }
    if (found->is_string()) {
        auto parsed = storage::parse_timestamp(found->as_string());
        return parsed ? parsed.value() : core::Timestamp{};
    }
    return storage::from_unix_millis(found->as_int());
}

core::SnapshotSummary summary_from_json(const Value& value) {
    core::SnapshotSummary summary;
    summary.id = string_field(value, "id");
    summary.name = string_field(value, "name");
    summary.tags = string_list(value, "tags");
    summary.created_at = timestamp_field(value, "created_at");
    summary.window_count = unsigned_field(value, "window_count");
    summary.tab_count = unsigned_field(value, "tab_count");
    summary.monitor_count = unsigned_field(value, "monitor_count");
    summary.favorite = bool_field(value, "favorite");
    summary.automatic = bool_field(value, "automatic");
    return summary;
}

core::RestorePlan plan_from_json(const Value& value) {
    core::RestorePlan plan;
    plan.snapshot_id = string_field(value, "snapshot_id");
    plan.estimated_duration_ms = unsigned_field(value, "estimated_duration_ms");
    plan.warnings = string_list(value, "warnings");
    const Value* steps = value.find("steps");
    if (steps != nullptr && steps->is_array()) {
        for (const Value& entry : steps->as_array()) {
            core::RestoreStep step;
            // `kind` is serialised as the enum's integer value (see
            // storage/serialization.cpp) so old clients keep parsing new steps.
            const Value* kind = entry.find("kind");
            step.kind = static_cast<core::RestoreStepKind>(kind == nullptr ? 0 : kind->as_int());
            step.target_id = string_field(entry, "target_id");
            step.description = string_field(entry, "description");
            step.skipped = bool_field(entry, "skipped");
            step.skip_reason = string_field(entry, "skip_reason");
            plan.steps.push_back(std::move(step));
        }
    }
    return plan;
}

core::RestoreReport report_from_json(const Value& value) {
    core::RestoreReport report;
    report.snapshot_id = string_field(value, "snapshot_id");
    report.windows_restored = unsigned_field(value, "windows_restored");
    report.windows_failed = unsigned_field(value, "windows_failed");
    report.apps_launched = unsigned_field(value, "apps_launched");
    report.tabs_restored = unsigned_field(value, "tabs_restored");
    report.duration_ms = unsigned_field(value, "duration_ms");
    report.warnings = string_list(value, "warnings");
    report.errors = string_list(value, "errors");
    return report;
}

Value capture_params(const core::CaptureOptions& options) {
    Value params = Value::object();
    params.set("include_tabs", Value(options.include_tabs));
    params.set("include_minimized", Value(options.include_minimized));
    params.set("include_cursor", Value(options.include_cursor));
    params.set("include_incognito", Value(options.include_incognito));
    params.set("include_all_workspaces", Value(options.include_all_workspaces));
    params.set("sanitize_urls", Value(options.sanitize_urls));
    params.set("automatic", Value(options.automatic));
    params.set("browser_timeout_ms",
               Value(static_cast<std::int64_t>(options.browser_timeout.count())));
    if (!options.name.empty()) {
        params.set("name", Value(options.name));
    }
    if (!options.tags.empty()) {
        params.set("tags", to_value(options.tags));
    }
    if (!options.excluded_app_ids.empty()) {
        params.set("excluded_app_ids", to_value(options.excluded_app_ids));
    }
    if (!options.excluded_url_patterns.empty()) {
        params.set("excluded_url_patterns", to_value(options.excluded_url_patterns));
    }
    return params;
}

Value restore_params(const std::string& snapshot_id, const core::RestoreOptions& options) {
    Value params = Value::object();
    params.set("snapshot_id", Value(snapshot_id));
    params.set("launch_missing_apps", Value(options.launch_missing_apps));
    params.set("restore_tabs", Value(options.restore_tabs));
    params.set("lazy_load_tabs", Value(options.lazy_load_tabs));
    params.set("restore_cursor", Value(options.restore_cursor));
    params.set("restore_focus", Value(options.restore_focus));
    params.set("restore_z_order", Value(options.restore_z_order));
    params.set("restore_workspaces", Value(options.restore_workspaces));
    params.set("close_conflicting_windows", Value(options.close_conflicting_windows));
    params.set("dry_run", Value(options.dry_run));
    params.set("app_launch_timeout_ms",
               Value(static_cast<std::int64_t>(options.app_launch_timeout.count())));
    params.set("window_settle_timeout_ms",
               Value(static_cast<std::int64_t>(options.window_settle_timeout.count())));
    params.set("max_parallel_launches",
               Value(static_cast<std::int64_t>(options.max_parallel_launches)));
    if (!options.selectors.empty()) {
        core::json::Array selectors;
        for (const core::RestoreSelector& selector : options.selectors) {
            Value entry = Value::object();
            entry.set("kind", Value(static_cast<std::int64_t>(selector.kind)));
            entry.set("pattern", Value(selector.pattern));
            selectors.emplace_back(std::move(entry));
        }
        params.set("selectors", Value(std::move(selectors)));
    }
    return params;
}

}  // namespace

struct Client::Subscription {
    std::unique_ptr<Connection> connection;
    std::thread worker;
    std::atomic<bool> running{true};
};

Client::Client(std::string endpoint)
    : endpoint_(endpoint.empty() ? core::Config::with_defaults().ipc.socket_path
                                 : std::move(endpoint)) {}

Client::~Client() { disconnect(); }

Status Client::connect(std::chrono::milliseconds timeout) {
    if (connected()) {
        return Status::success();
    }
    auto connection = ipc::connect(endpoint_, timeout);
    if (!connection) {
        return connection.error();
    }
    connection_ = std::move(connection.value());
    (void)connection_->set_timeout(timeout);
    reader_.reset();

    Request hello;
    hello.id = next_request_id_.fetch_add(1);
    hello.method = Method::Hello;
    hello.params = Value::object();
    hello.params.set("client", Value(std::string("contextsnap")));
    hello.params.set("version", Value(std::string(kVersion)));
    hello.params.set("protocol", Value(static_cast<std::int64_t>(kProtocolVersion)));

    auto response = round_trip(hello, timeout);
    if (!response) {
        connection_.reset();
        return response.error();
    }
    if (!response.value().ok) {
        connection_.reset();
        return error_from(response.value());
    }
    server_version_ = unsigned_field(response.value().result, "protocol");
    if (server_version_ != kProtocolVersion) {
        core::log::warn("daemon speaks a different protocol version",
                        {core::log::field("daemon", static_cast<std::int64_t>(server_version_)),
                         core::log::field("client", static_cast<std::int64_t>(kProtocolVersion))});
    }
    return Status::success();
}

void Client::disconnect() {
    unsubscribe();
    if (connection_ != nullptr) {
        connection_->close();
        connection_.reset();
    }
    reader_.reset();
}

bool Client::connected() const noexcept {
    return connection_ != nullptr && connection_->is_open();
}

Result<Response> Client::round_trip(const Request& request, std::chrono::milliseconds timeout) {
    if (connection_ == nullptr) {
        return core::err::io("not connected to contextsnapd", kContext);
    }
    auto frame = encode_frame(request.to_json().dump());
    if (!frame) {
        return frame.error();
    }
    std::string_view bytes(reinterpret_cast<const char*>(frame.value().data()),
                           frame.value().size());
    while (!bytes.empty()) {
        auto written = connection_->write(bytes);
        if (!written) {
            return written.error();
        }
        if (written.value() == 0) {
            return core::err::io("the daemon closed the connection while writing", kContext);
        }
        bytes.remove_prefix(written.value());
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        // Frames already buffered by an earlier read come first.
        auto next = reader_.next();
        if (!next) {
            return next.error();
        }
        if (next.value().has_value()) {
            auto document = core::json::parse(*next.value());
            if (!document) {
                return document.error();
            }
            if (document.value().contains("event")) {
                continue;  // Events belong to the subscription connection.
            }
            auto response = Response::from_json(document.value());
            if (!response) {
                return response.error();
            }
            if (response.value().id != request.id) {
                continue;  // Stale reply from a cancelled call.
            }
            return response.value();
        }

        auto chunk = connection_->read_some();
        if (!chunk) {
            if (chunk.error().code == core::ErrorCode::Timeout) {
                continue;
            }
            return chunk.error();
        }
        if (chunk.value().empty()) {
            return core::err::io("the daemon closed the connection", kContext);
        }
        reader_.feed(chunk.value());
    }
    return core::err::io("timed out waiting for the daemon to reply", kContext);
}

Result<core::json::Value> Client::call(Method method, core::json::Value params,
                                       std::chrono::milliseconds timeout) {
    if (!connected()) {
        if (const Status status = connect(); !status) {
            return status.error();
        }
    }
    Request request;
    request.id = next_request_id_.fetch_add(1);
    request.method = method;
    request.params = std::move(params);

    auto response = round_trip(request, timeout);
    if (!response) {
        return response.error();
    }
    if (!response.value().ok) {
        return error_from(response.value());
    }
    return response.value().result;
}

Result<core::Snapshot> Client::capture(const core::CaptureOptions& options) {
    // Capture waits for the browser round-trip, so allow more than the default.
    auto result = call(Method::CaptureSnapshot, capture_params(options),
                       std::chrono::milliseconds{30000});
    if (!result) {
        return result.error();
    }
    const Value* snapshot = result.value().find("snapshot");
    if (snapshot == nullptr) {
        return core::err::protocol("capture response has no \"snapshot\"", kContext);
    }
    return storage::snapshot_from_json(*snapshot);
}

Result<std::vector<core::SnapshotSummary>> Client::list(std::size_t limit, std::size_t offset,
                                                        const std::string& tag) {
    Value params = Value::object();
    params.set("limit", Value(static_cast<std::int64_t>(limit)));
    params.set("offset", Value(static_cast<std::int64_t>(offset)));
    if (!tag.empty()) {
        params.set("tag", Value(tag));
    }
    auto result = call(Method::ListSnapshots, std::move(params));
    if (!result) {
        return result.error();
    }
    std::vector<core::SnapshotSummary> summaries;
    const Value* snapshots = result.value().find("snapshots");
    if (snapshots == nullptr || !snapshots->is_array()) {
        return summaries;
    }
    for (const Value& entry : snapshots->as_array()) {
        summaries.push_back(summary_from_json(entry));
    }
    return summaries;
}

Result<core::Snapshot> Client::get(const std::string& snapshot_id) {
    Value params = Value::object();
    params.set("snapshot_id", Value(snapshot_id));
    auto result = call(Method::GetSnapshot, std::move(params));
    if (!result) {
        return result.error();
    }
    const Value* snapshot = result.value().find("snapshot");
    if (snapshot == nullptr) {
        return core::err::protocol("get response has no \"snapshot\"", kContext);
    }
    return storage::snapshot_from_json(*snapshot);
}

Result<core::RestorePlan> Client::plan_restore(const std::string& snapshot_id,
                                               const core::RestoreOptions& options) {
    auto result = call(Method::PlanRestore, restore_params(snapshot_id, options));
    if (!result) {
        return result.error();
    }
    const Value* plan = result.value().find("plan");
    if (plan == nullptr) {
        return core::err::protocol("plan response has no \"plan\"", kContext);
    }
    return plan_from_json(*plan);
}

Result<core::RestoreReport> Client::restore(const std::string& snapshot_id,
                                            const core::RestoreOptions& options) {
    // Restores launch applications and wait for their windows.
    auto result = call(Method::RestoreSnapshot, restore_params(snapshot_id, options),
                       std::chrono::milliseconds{180000});
    if (!result) {
        return result.error();
    }
    const Value* report = result.value().find("report");
    if (report == nullptr) {
        return core::err::protocol("restore response has no \"report\"", kContext);
    }
    return report_from_json(*report);
}

Status Client::remove(const std::string& snapshot_id) {
    Value params = Value::object();
    params.set("snapshot_id", Value(snapshot_id));
    auto result = call(Method::DeleteSnapshot, std::move(params));
    if (!result) {
        return result.error();
    }
    return Status::success();
}

Result<core::json::Value> Client::health() { return call(Method::GetHealth); }

Status Client::subscribe(EventCallback callback) {
    if (callback == nullptr) {
        return core::err::invalid("subscribe needs a callback", kContext);
    }
    unsubscribe();

    // Events use a dedicated connection so a long-running subscription never
    // interleaves with request/response traffic.
    auto connection = ipc::connect(endpoint_, std::chrono::milliseconds{2000});
    if (!connection) {
        return connection.error();
    }
    auto subscription = std::make_unique<Subscription>();
    subscription->connection = std::move(connection.value());
    (void)subscription->connection->set_timeout(std::chrono::milliseconds{250});

    Request request;
    request.id = next_request_id_.fetch_add(1);
    request.method = Method::Subscribe;
    request.params = Value::object();
    auto frame = encode_frame(request.to_json().dump());
    if (!frame) {
        return frame.error();
    }
    std::string_view bytes(reinterpret_cast<const char*>(frame.value().data()),
                           frame.value().size());
    while (!bytes.empty()) {
        auto written = subscription->connection->write(bytes);
        if (!written) {
            return written.error();
        }
        bytes.remove_prefix(written.value());
    }

    Subscription* raw = subscription.get();
    subscription->worker = std::thread([raw, callback = std::move(callback)] {
        FrameReader reader;
        while (raw->running.load()) {
            auto chunk = raw->connection->read_some();
            if (!chunk) {
                if (chunk.error().code == core::ErrorCode::Timeout) {
                    continue;
                }
                break;
            }
            if (chunk.value().empty()) {
                break;
            }
            reader.feed(chunk.value());
            while (true) {
                auto next = reader.next();
                if (!next || !next.value().has_value()) {
                    break;
                }
                auto document = core::json::parse(*next.value());
                if (!document) {
                    continue;
                }
                auto event = Event::from_json(document.value());
                if (event) {
                    callback(event.value());
                }
            }
        }
    });
    subscription_ = std::move(subscription);
    return Status::success();
}

void Client::unsubscribe() {
    if (subscription_ == nullptr) {
        return;
    }
    subscription_->running.store(false);
    if (subscription_->connection != nullptr) {
        subscription_->connection->close();
    }
    if (subscription_->worker.joinable()) {
        subscription_->worker.join();
    }
    subscription_.reset();
}

Result<std::unique_ptr<Client>> connect_or_spawn_daemon(const std::string& endpoint,
                                                        const std::string& daemon_executable) {
    const std::string target =
        endpoint.empty() ? core::Config::with_defaults().ipc.socket_path : endpoint;

    auto client = std::make_unique<Client>(target);
    if (const Status status = client->connect(std::chrono::milliseconds{500}); status) {
        return client;
    }

    const std::string executable = daemon_executable.empty() ? "contextsnapd" : daemon_executable;
    core::log::info("starting contextsnapd", {core::log::field("endpoint", target)});

#if defined(_WIN32)
    std::string command = executable + " --detached --endpoint \"" + target + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                       nullptr, &startup, &process) == 0) {
        return core::err::io("could not start contextsnapd", kContext);
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
#else
    const pid_t child = ::fork();
    if (child < 0) {
        return core::err::io("fork failed while starting contextsnapd", kContext);
    }
    if (child == 0) {
        ::setsid();
        ::execlp(executable.c_str(), executable.c_str(), "--detached", "--endpoint", target.c_str(),
                 static_cast<char*>(nullptr));
        ::_exit(127);  // execlp only returns on failure.
    }
#endif

    // The daemon needs a moment to open the database and bind the socket.
    for (int attempt = 0; attempt < 30; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        if (const Status status = client->connect(std::chrono::milliseconds{300}); status) {
            return client;
        }
    }
    return core::err::io("contextsnapd did not start within 3 seconds", kContext);
}

}  // namespace contextsnap::ipc
