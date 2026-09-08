#include "daemon.hpp"

#include "../browser/tab_inbox.hpp"

#include <contextsnap/browser/native_host.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/hal/null_platform.hpp>
#include <contextsnap/privacy/crypto.hpp>
#include <contextsnap/storage/serialization.hpp>
#include <contextsnap/version.hpp>

#include <chrono>
#include <utility>

namespace contextsnap::daemon {
namespace {

using core::json::Value;
using core::log::field;

const Value* member(const Value& params, const char* key) { return params.find(key); }

std::string string_param(const Value& params, const char* key, const std::string& fallback = {}) {
    const Value* found = member(params, key);
    return found == nullptr ? fallback : found->as_string();
}

bool bool_param(const Value& params, const char* key, bool fallback) {
    const Value* found = member(params, key);
    return found == nullptr ? fallback : found->as_bool();
}

std::int64_t int_param(const Value& params, const char* key, std::int64_t fallback) {
    const Value* found = member(params, key);
    return found == nullptr ? fallback : found->as_int();
}

std::vector<std::string> string_list(const Value& params, const char* key) {
    std::vector<std::string> values;
    const Value* found = member(params, key);
    if (found == nullptr || !found->is_array()) {
        return values;
    }
    for (const Value& entry : found->as_array()) {
        values.push_back(entry.as_string());
    }
    return values;
}

Value to_value(const std::vector<std::string>& values) {
    core::json::Array array;
    for (const std::string& value : values) {
        array.emplace_back(Value(value));
    }
    return Value(std::move(array));
}

core::CaptureOptions capture_options_from(const Value& params, const core::Config& config) {
    core::CaptureOptions options = config.capture_options();
    options.include_tabs = bool_param(params, "include_tabs", options.include_tabs);
    options.include_minimized = bool_param(params, "include_minimized", options.include_minimized);
    options.include_cursor = bool_param(params, "include_cursor", options.include_cursor);
    options.include_incognito = bool_param(params, "include_incognito", options.include_incognito);
    options.include_all_workspaces =
        bool_param(params, "include_all_workspaces", options.include_all_workspaces);
    options.sanitize_urls = bool_param(params, "sanitize_urls", options.sanitize_urls);
    options.automatic = bool_param(params, "automatic", options.automatic);
    options.name = string_param(params, "name", options.name);
    if (member(params, "tags") != nullptr) {
        options.tags = string_list(params, "tags");
    }
    if (member(params, "excluded_app_ids") != nullptr) {
        options.excluded_app_ids = string_list(params, "excluded_app_ids");
    }
    if (member(params, "excluded_url_patterns") != nullptr) {
        options.excluded_url_patterns = string_list(params, "excluded_url_patterns");
    }
    if (const std::int64_t timeout = int_param(params, "browser_timeout_ms", 0); timeout > 0) {
        options.browser_timeout = core::Milliseconds{timeout};
    }
    return options;
}

core::RestoreOptions restore_options_from(const Value& params, const core::Config& config) {
    core::RestoreOptions options = config.restore_options();
    options.launch_missing_apps =
        bool_param(params, "launch_missing_apps", options.launch_missing_apps);
    options.restore_tabs = bool_param(params, "restore_tabs", options.restore_tabs);
    options.lazy_load_tabs = bool_param(params, "lazy_load_tabs", options.lazy_load_tabs);
    options.restore_cursor = bool_param(params, "restore_cursor", options.restore_cursor);
    options.restore_focus = bool_param(params, "restore_focus", options.restore_focus);
    options.restore_z_order = bool_param(params, "restore_z_order", options.restore_z_order);
    options.restore_workspaces =
        bool_param(params, "restore_workspaces", options.restore_workspaces);
    options.close_conflicting_windows =
        bool_param(params, "close_conflicting_windows", options.close_conflicting_windows);
    options.dry_run = bool_param(params, "dry_run", options.dry_run);
    if (const std::int64_t value = int_param(params, "app_launch_timeout_ms", 0); value > 0) {
        options.app_launch_timeout = core::Milliseconds{value};
    }
    if (const std::int64_t value = int_param(params, "window_settle_timeout_ms", 0); value > 0) {
        options.window_settle_timeout = core::Milliseconds{value};
    }
    if (const std::int64_t value = int_param(params, "max_parallel_launches", 0); value > 0) {
        options.max_parallel_launches = static_cast<std::uint32_t>(value);
    }

    const Value* selectors = member(params, "selectors");
    if (selectors != nullptr && selectors->is_array()) {
        options.selectors.clear();
        for (const Value& entry : selectors->as_array()) {
            // Accept both the structured form and the textual "app:code" form.
            if (entry.is_string()) {
                // Unparsable selectors are ignored rather than failing the whole
                // restore: a typo should not abort a 40-window recovery.
                if (auto parsed = core::parse_selector(entry.as_string()); parsed) {
                    options.selectors.push_back(parsed.value());
                }
                continue;
            }
            core::RestoreSelector selector;
            selector.kind = static_cast<core::RestoreSelector::Kind>(
                entry.find("kind") == nullptr ? 0 : entry.find("kind")->as_int());
            selector.pattern =
                entry.find("pattern") == nullptr ? "" : entry.find("pattern")->as_string();
            options.selectors.push_back(std::move(selector));
        }
    }
    return options;
}

Value config_to_json(const core::Config& config) {
    Value storage = Value::object();
    storage.set("database_path", Value(config.storage.database_path));
    storage.set("encrypt", Value(config.storage.encrypt));
    storage.set("retention_days", Value(static_cast<std::int64_t>(config.storage.retention_days)));
    storage.set("max_snapshots", Value(static_cast<std::int64_t>(config.storage.max_snapshots)));

    Value capture = Value::object();
    capture.set("auto_capture", Value(config.capture.auto_capture));
    capture.set("auto_capture_interval_minutes",
                Value(static_cast<std::int64_t>(config.capture.auto_capture_interval_minutes)));
    capture.set("include_minimized", Value(config.capture.include_minimized));
    capture.set("include_incognito", Value(config.capture.include_incognito));
    capture.set("browser_timeout_ms",
                Value(static_cast<std::int64_t>(config.capture.browser_timeout_ms)));

    Value restore = Value::object();
    restore.set("launch_missing_apps", Value(config.restore.launch_missing_apps));
    restore.set("lazy_load_tabs", Value(config.restore.lazy_load_tabs));
    restore.set("restore_cursor", Value(config.restore.restore_cursor));
    restore.set("restore_z_order", Value(config.restore.restore_z_order));

    Value privacy = Value::object();
    privacy.set("sanitize_urls", Value(config.privacy.sanitize_urls));

    Value ipc_config = Value::object();
    ipc_config.set("socket_path", Value(config.ipc.socket_path));
    ipc_config.set("request_timeout_ms",
                   Value(static_cast<std::int64_t>(config.ipc.request_timeout_ms)));

    Value document = Value::object();
    document.set("storage", std::move(storage));
    document.set("capture", std::move(capture));
    document.set("restore", std::move(restore));
    document.set("privacy", std::move(privacy));
    document.set("ipc", std::move(ipc_config));
    document.set("log_level", Value(std::string(core::log::to_string(config.log_level))));
    return document;
}

}  // namespace

std::string browser_endpoint_for(const std::string& endpoint) {
    return endpoint + "-browser";
}

Daemon::Daemon(DaemonOptions options) : options_(std::move(options)) {
    endpoint_ = options_.endpoint.empty() ? options_.config.ipc.socket_path : options_.endpoint;
    browser_endpoint_ = options_.browser_endpoint.empty() ? browser_endpoint_for(endpoint_)
                                                          : options_.browser_endpoint;
}

Daemon::~Daemon() { stop(); }

core::Status Daemon::start() {
    // 1. Storage first: a broken database must fail before we take the socket.
    storage::DatabaseOptions database;
    database.path = options_.config.storage.database_path;
    database.create_if_missing = true;
    if (options_.config.storage.encrypt) {
        auto keyring = privacy::Keyring::create();
        if (!keyring) {
            return keyring.error();
        }
        auto key = privacy::get_or_create_database_key(*keyring.value());
        if (!key) {
            return key.error();
        }
        const std::vector<std::uint8_t> bytes(key.value().begin(), key.value().end());
        database.encryption_key = privacy::to_hex(bytes);
    }
    auto repository = storage::SnapshotRepository::open(database);
    if (!repository) {
        return repository.error();
    }
    repository_ = repository.value();

    // 2. Platform backend. A missing display is not fatal: the daemon still
    //    serves the library so `list`/`export` keep working over SSH.
    auto platform = hal::Platform::create();
    if (!platform) {
        core::log::warn("no window backend available; capture is disabled",
                        {field("reason", platform.error().message)});
    } else {
        platform_ = platform.value();
    }

    bridge_ = options_.headless ? browser::Bridge::create_null() : browser::Bridge::create();
    sanitizer_ = std::make_shared<privacy::Sanitizer>();

    if (platform_ == nullptr) {
        // Library-only mode: capture is unavailable but list/export keep working.
        platform_ = hal::create_null_platform(hal::default_null_state());
    }

    manager_ = std::make_unique<core::SnapshotManager>(platform_, repository_, bridge_,
                                                        sanitizer_, options_.config);

    // 3. IPC listeners.
    ipc::ServerOptions server_options;
    server_options.endpoint = endpoint_;
    server_options.max_clients = options_.config.ipc.max_clients;
    server_options.max_frame_bytes = options_.config.ipc.max_frame_bytes;
    server_ = std::make_unique<ipc::Server>(server_options);
    register_handlers();
    if (const core::Status status = server_->start(); !status) {
        return status;
    }

    auto browser_listener = ipc::listen(browser_endpoint_);
    if (!browser_listener) {
        core::log::warn("browser endpoint unavailable; tabs fall back to session files",
                        {field("endpoint", browser_endpoint_),
                         field("reason", browser_listener.error().message)});
    } else {
        browser_listener_ = std::move(browser_listener.value());
    }

    running_.store(true);
    if (browser_listener_ != nullptr) {
        browser_thread_ = std::thread(&Daemon::browser_loop, this);
    }
    if (options_.scheduler) {
        scheduler_thread_ = std::thread(&Daemon::scheduler_loop, this);
    }

    core::log::info("contextsnapd ready",
                    {field("version", std::string(kVersion)), field("endpoint", endpoint_),
                     field("platform", platform_->name())});
    return core::Status::success();
}

void Daemon::run() {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_.wait(lock, [this] { return !running_.load(); });
}

void Daemon::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    publish(ipc::EventType::DaemonShuttingDown, Value::object());

    if (browser_listener_ != nullptr) {
        browser_listener_->close();
    }
    if (server_ != nullptr) {
        server_->stop();
    }
    wake_.notify_all();

    if (browser_thread_.joinable()) {
        browser_thread_.join();
    }
    {
        std::lock_guard<std::mutex> guard(browser_mutex_);
        for (std::thread& worker : browser_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        browser_workers_.clear();
    }
    if (scheduler_thread_.joinable()) {
        scheduler_thread_.join();
    }
    core::log::info("contextsnapd stopped");
}

void Daemon::publish(ipc::EventType type, Value payload) {
    if (server_ == nullptr) {
        return;
    }
    ipc::Event event;
    event.type = type;
    event.payload = std::move(payload);
    server_->broadcast(event);
}

void Daemon::register_handlers() {
    ipc::Server& server = *server_;

    server.on(ipc::Method::Hello, [this](const Value& params, const ipc::PeerIdentity& peer) {
        core::log::debug("client connected",
                         {field("client", string_param(params, "client", "unknown")),
                          field("pid", static_cast<std::int64_t>(peer.pid))});
        Value result = Value::object();
        result.set("daemon", Value(std::string(kProjectName)));
        result.set("version", Value(std::string(kVersion)));
        result.set("protocol", Value(static_cast<std::int64_t>(ipc::kProtocolVersion)));
        result.set("schema_version", Value(static_cast<std::int64_t>(kSnapshotSchemaVersion)));
        result.set("platform", Value(platform_->name()));
        result.set("session_type",
                   Value(std::string(core::to_string(platform_->session_type()))));
        return core::Result<Value>(std::move(result));
    });

    server.on(ipc::Method::Ping, [](const Value&, const ipc::PeerIdentity&) {
        Value result = Value::object();
        result.set("pong", Value(true));
        return core::Result<Value>(std::move(result));
    });

    server.on(ipc::Method::CaptureSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  auto snapshot =
                      manager_->capture_and_store(capture_options_from(params, manager_->config()));
                  if (!snapshot) {
                      return snapshot.error();
                  }
                  last_capture_ = core::Clock::now();
                  Value created = Value::object();
                  created.set("snapshot_id", Value(snapshot.value().metadata.id));
                  created.set("name", Value(snapshot.value().metadata.name));
                  publish(ipc::EventType::SnapshotCreated, std::move(created));

                  Value result = Value::object();
                  result.set("snapshot", storage::to_json(snapshot.value()));
                  return result;
              });

    server.on(ipc::Method::ListSnapshots,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  auto summaries = manager_->list(
                      static_cast<std::size_t>(int_param(params, "limit", 50)),
                      static_cast<std::size_t>(int_param(params, "offset", 0)),
                      string_param(params, "tag"));
                  if (!summaries) {
                      return summaries.error();
                  }
                  core::json::Array array;
                  for (const core::SnapshotSummary& summary : summaries.value()) {
                      array.emplace_back(storage::to_json(summary));
                  }
                  Value result = Value::object();
                  result.set("snapshots", Value(std::move(array)));
                  return result;
              });

    server.on(ipc::Method::GetSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string id = string_param(params, "snapshot_id");
                  auto snapshot = id.empty() ? manager_->latest() : manager_->get(id);
                  if (!snapshot) {
                      return snapshot.error();
                  }
                  Value result = Value::object();
                  result.set("snapshot", storage::to_json(snapshot.value()));
                  return result;
              });

    server.on(ipc::Method::DeleteSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string id = string_param(params, "snapshot_id");
                  if (const core::Status status = manager_->remove(id); !status) {
                      return status.error();
                  }
                  Value deleted = Value::object();
                  deleted.set("snapshot_id", Value(id));
                  publish(ipc::EventType::SnapshotDeleted, deleted);
                  Value result = Value::object();
                  result.set("deleted", Value(true));
                  return result;
              });

    server.on(ipc::Method::RenameSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string id = string_param(params, "snapshot_id");
                  if (const core::Status status =
                          manager_->rename(id, string_param(params, "name"));
                      !status) {
                      return status.error();
                  }
                  Value updated = Value::object();
                  updated.set("snapshot_id", Value(id));
                  publish(ipc::EventType::SnapshotUpdated, updated);
                  Value result = Value::object();
                  result.set("renamed", Value(true));
                  return result;
              });

    server.on(ipc::Method::TagSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string id = string_param(params, "snapshot_id");
                  if (const core::Status status = manager_->set_tags(id, string_list(params, "tags"));
                      !status) {
                      return status.error();
                  }
                  Value updated = Value::object();
                  updated.set("snapshot_id", Value(id));
                  publish(ipc::EventType::SnapshotUpdated, updated);
                  Value result = Value::object();
                  result.set("tagged", Value(true));
                  return result;
              });

    server.on(ipc::Method::FavoriteSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string id = string_param(params, "snapshot_id");
                  if (const core::Status status =
                          manager_->set_favorite(id, bool_param(params, "favorite", true));
                      !status) {
                      return status.error();
                  }
                  Value updated = Value::object();
                  updated.set("snapshot_id", Value(id));
                  publish(ipc::EventType::SnapshotUpdated, updated);
                  Value result = Value::object();
                  result.set("favorite", Value(bool_param(params, "favorite", true)));
                  return result;
              });

    server.on(ipc::Method::PlanRestore,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  auto plan = manager_->plan_restore(
                      string_param(params, "snapshot_id"),
                      restore_options_from(params, manager_->config()));
                  if (!plan) {
                      return plan.error();
                  }
                  Value result = Value::object();
                  result.set("plan", storage::to_json(plan.value()));
                  return result;
              });

    server.on(ipc::Method::RestoreSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string id = string_param(params, "snapshot_id");
                  Value started = Value::object();
                  started.set("snapshot_id", Value(id));
                  publish(ipc::EventType::RestoreStarted, started);

                  auto report = manager_->restore(
                      id, restore_options_from(params, manager_->config()),
                      [this, &id](std::string_view stage, int percent) {
                          Value progress = Value::object();
                          progress.set("snapshot_id", Value(id));
                          progress.set("stage", Value(std::string(stage)));
                          progress.set("percent", Value(static_cast<std::int64_t>(percent)));
                          publish(ipc::EventType::RestoreProgress, std::move(progress));
                      });
                  if (!report) {
                      return report.error();
                  }
                  publish(ipc::EventType::RestoreFinished,
                          storage::to_json(report.value()));
                  Value result = Value::object();
                  result.set("report", storage::to_json(report.value()));
                  return result;
              });

    server.on(ipc::Method::SearchSnapshots,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  auto summaries =
                      manager_->search(string_param(params, "query"),
                                       static_cast<std::size_t>(int_param(params, "limit", 25)));
                  if (!summaries) {
                      return summaries.error();
                  }
                  core::json::Array array;
                  for (const core::SnapshotSummary& summary : summaries.value()) {
                      array.emplace_back(storage::to_json(summary));
                  }
                  Value result = Value::object();
                  result.set("snapshots", Value(std::move(array)));
                  return result;
              });

    server.on(ipc::Method::ExportSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  const std::string format = string_param(params, "format", "json");
                  auto document =
                      manager_->export_snapshot(string_param(params, "snapshot_id"), format);
                  if (!document) {
                      return document.error();
                  }
                  Value result = Value::object();
                  result.set("format", Value(format));
                  result.set("document", Value(document.value()));
                  return result;
              });

    server.on(ipc::Method::ImportSnapshot,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  auto snapshot = manager_->import_snapshot(
                      string_param(params, "document"), string_param(params, "format", "json"),
                      string_param(params, "rename_to"));
                  if (!snapshot) {
                      return snapshot.error();
                  }
                  Value created = Value::object();
                  created.set("snapshot_id", Value(snapshot.value().metadata.id));
                  publish(ipc::EventType::SnapshotCreated, created);
                  Value result = Value::object();
                  result.set("snapshot_id", Value(snapshot.value().metadata.id));
                  return result;
              });

    server.on(ipc::Method::GetConfig, [this](const Value&, const ipc::PeerIdentity&) {
        return core::Result<Value>(config_to_json(manager_->config()));
    });

    server.on(ipc::Method::SetConfig,
              [this](const Value& params, const ipc::PeerIdentity&) -> core::Result<Value> {
                  core::Config config = manager_->config();
                  if (const Value* storage = member(params, "storage"); storage != nullptr) {
                      config.storage.retention_days = static_cast<std::uint32_t>(int_param(
                          *storage, "retention_days", config.storage.retention_days));
                      config.storage.max_snapshots = static_cast<std::uint32_t>(
                          int_param(*storage, "max_snapshots", config.storage.max_snapshots));
                  }
                  if (const Value* capture = member(params, "capture"); capture != nullptr) {
                      config.capture.auto_capture =
                          bool_param(*capture, "auto_capture", config.capture.auto_capture);
                      config.capture.auto_capture_interval_minutes =
                          static_cast<std::uint32_t>(
                              int_param(*capture, "auto_capture_interval_minutes",
                                        config.capture.auto_capture_interval_minutes));
                      config.capture.include_incognito = bool_param(
                          *capture, "include_incognito", config.capture.include_incognito);
                  }
                  if (const Value* restore = member(params, "restore"); restore != nullptr) {
                      config.restore.lazy_load_tabs =
                          bool_param(*restore, "lazy_load_tabs", config.restore.lazy_load_tabs);
                      config.restore.launch_missing_apps = bool_param(
                          *restore, "launch_missing_apps", config.restore.launch_missing_apps);
                  }
                  if (const Value* privacy = member(params, "privacy"); privacy != nullptr) {
                      config.privacy.sanitize_urls =
                          bool_param(*privacy, "sanitize_urls", config.privacy.sanitize_urls);
                  }
                  if (member(params, "log_level") != nullptr) {
                      config.log_level =
                          core::log::level_from_string(string_param(params, "log_level"));
                      core::log::set_level(config.log_level);
                  }
                  if (const core::Status valid = config.validate(); !valid) {
                      return valid.error();
                  }
                  manager_->update_config(config);
                  if (const core::Status saved = config.save({}); !saved) {
                      core::log::warn("config could not be persisted",
                                      {field("reason", saved.error().message)});
                  }
                  publish(ipc::EventType::ConfigChanged, config_to_json(config));
                  return core::Result<Value>(config_to_json(config));
              });

    server.on(ipc::Method::GetHealth, [this](const Value&, const ipc::PeerIdentity&) {
        const core::SnapshotManager::HealthReport health = manager_->health();
        Value result = Value::object();
        result.set("version", Value(std::string(kVersion)));
        result.set("endpoint", Value(endpoint_));
        result.set("browser_endpoint", Value(browser_endpoint_));
        result.set("database_ok", Value(health.database_ok));
        result.set("platform_ok", Value(health.platform_ok));
        result.set("browser_bridge_connected", Value(health.browser_bridge_connected));
        result.set("database_bytes", Value(static_cast<std::int64_t>(health.database_bytes)));
        result.set("snapshot_count", Value(static_cast<std::int64_t>(health.snapshot_count)));
        result.set("platform", Value(health.platform_name));
        result.set("session_type", Value(health.session_type));
        result.set("missing_permissions", to_value(health.missing_permissions));
        result.set("warnings", to_value(health.warnings));
        result.set("limitations", to_value(platform_->capabilities().limitations));
        result.set("clients", Value(static_cast<std::int64_t>(server_->client_count())));
        return core::Result<Value>(std::move(result));
    });

    server.on(ipc::Method::PruneSnapshots,
              [this](const Value&, const ipc::PeerIdentity&) -> core::Result<Value> {
                  auto removed = manager_->prune();
                  if (!removed) {
                      return removed.error();
                  }
                  Value result = Value::object();
                  result.set("removed", Value(static_cast<std::int64_t>(removed.value())));
                  return result;
              });

    server.on(ipc::Method::Shutdown,
              [this](const Value&, const ipc::PeerIdentity& peer) -> core::Result<Value> {
                  core::log::info("shutdown requested",
                                  {field("pid", static_cast<std::int64_t>(peer.pid))});
                  // Reply first, then unblock run() from a detached thread.
                  std::thread([this] {
                      std::this_thread::sleep_for(std::chrono::milliseconds{50});
                      stop();
                  }).detach();
                  Value result = Value::object();
                  result.set("stopping", Value(true));
                  return result;
              });
}

void Daemon::scheduler_loop() {
    core::log::debug("scheduler started");
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_for(lock, std::chrono::seconds{60}, [this] { return !running_.load(); });
        }
        if (!running_.load()) {
            break;
        }
        const core::Config& config = manager_->config();
        if (!config.capture.auto_capture) {
            continue;
        }
        const auto interval = std::chrono::minutes{config.capture.auto_capture_interval_minutes};
        if (core::Clock::now() - last_capture_ < interval) {
            continue;
        }
        core::CaptureOptions options = config.capture_options();
        options.automatic = true;
        options.name = "Automatic";
        auto snapshot = manager_->capture_and_store(options);
        if (!snapshot) {
            core::log::warn("automatic capture failed",
                            {field("reason", snapshot.error().message)});
            continue;
        }
        last_capture_ = core::Clock::now();
        Value created = Value::object();
        created.set("snapshot_id", Value(snapshot.value().metadata.id));
        created.set("automatic", Value(true));
        publish(ipc::EventType::SnapshotCreated, std::move(created));

        if (auto removed = manager_->prune(); removed && removed.value() > 0) {
            core::log::info("pruned old snapshots",
                            {field("count", static_cast<std::int64_t>(removed.value()))});
        }
    }
}

void Daemon::browser_loop() {
    while (running_.load()) {
        auto connection = browser_listener_->accept();
        if (!connection) {
            if (connection.error().code == core::ErrorCode::Timeout) {
                continue;
            }
            if (!running_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            continue;
        }
        const std::string id = "host-" + std::to_string(host_counter_.fetch_add(1));
        std::lock_guard<std::mutex> guard(browser_mutex_);
        browser_workers_.emplace_back(&Daemon::serve_browser_host, this,
                                       std::move(connection.value()), id);
    }
}

void Daemon::serve_browser_host(std::unique_ptr<ipc::Connection> connection,
                                const std::string& connection_id) {
    // Short read timeout: the loop doubles as the outbound pump for restore
    // requests, so it must come back even when the browser is quiet.
    (void)connection->set_timeout(std::chrono::milliseconds{200});
    ipc::FrameReader reader(static_cast<std::uint32_t>(options_.config.ipc.max_frame_bytes));
    core::BrowserKind browser = core::BrowserKind::None;
    std::string profile;
    bool registered = false;

    const auto send = [&connection](const std::string& type, Value payload) {
        browser::HostMessage message;
        message.type = type;
        message.payload = std::move(payload);
        auto frame = ipc::encode_frame(message.to_json().dump());
        if (!frame) {
            return false;
        }
        std::string_view bytes(reinterpret_cast<const char*>(frame.value().data()),
                               frame.value().size());
        while (!bytes.empty()) {
            auto written = connection->write(bytes);
            if (!written || written.value() == 0) {
                return false;
            }
            bytes.remove_prefix(written.value());
        }
        return true;
    };

    Value hello = Value::object();
    hello.set("daemon", Value(std::string(kProjectName)));
    hello.set("version", Value(std::string(kVersion)));
    hello.set("protocol", Value(static_cast<std::int64_t>(kNativeMessagingProtocolVersion)));
    (void)send("hello", std::move(hello));

    while (running_.load() && connection->is_open()) {
        auto chunk = connection->read_some();
        if (!chunk) {
            if (chunk.error().code != core::ErrorCode::Timeout) {
                break;
            }
        } else if (chunk.value().empty()) {
            break;  // The host exited, which happens whenever the browser closes.
        } else {
            reader.feed(chunk.value());
        }

        bool fatal = false;
        while (!fatal) {
            auto next = reader.next();
            if (!next) {
                core::log::warn("malformed browser frame",
                                {field("reason", next.error().message)});
                fatal = true;
                break;
            }
            if (!next.value().has_value()) {
                break;
            }
            auto document = core::json::parse(*next.value());
            if (!document) {
                continue;
            }
            auto message = browser::ExtensionMessage::from_json(document.value());
            if (!message) {
                continue;
            }
            const std::string& type = message.value().type;
            const Value& payload = message.value().payload;

            if (type == "hello" || type == "browser_info") {
                browser = core::browser_kind_from_string(
                    payload.find("browser") == nullptr ? "" : payload.find("browser")->as_string());
                profile = payload.find("profile") == nullptr ? ""
                                                             : payload.find("profile")->as_string();
                if (!registered) {
                    bridge_->register_host_connection(connection_id, browser, profile);
                    registered = true;
                    Value connected = Value::object();
                    connected.set("browser", Value(std::string(core::to_string(browser))));
                    connected.set("profile", Value(profile));
                    publish(ipc::EventType::BrowserConnected, std::move(connected));
                }
            } else if (type == "tabs") {
                auto windows = browser::parse_tabs_message(payload);
                if (!windows) {
                    core::log::warn("unusable tab payload",
                                    {field("reason", windows.error().message)});
                    continue;
                }
                browser::inbox::publish_tabs(connection_id, windows.value());
            } else if (type == "restoreResult" || type == "restore_ack") {
                browser::inbox::acknowledge_restore(
                    payload.find("requestId") == nullptr
                        ? ""
                        : payload.find("requestId")->as_string(),
                    static_cast<std::uint32_t>(payload.find("tabsRestored") == nullptr
                                                   ? 0
                                                   : payload.find("tabsRestored")->as_int()));
            } else if (type == "error") {
                core::log::warn("extension reported an error",
                                {field("message", payload.find("message") == nullptr
                                                      ? ""
                                                      : payload.find("message")->as_string())});
            }
        }
        if (fatal) {
            break;
        }

        // Outbound: capture requests and queued restores.
        if (browser::inbox::consume_collection_request()) {
            Value request = Value::object();
            request.set("includeIncognito", Value(manager_->config().capture.include_incognito));
            if (!send("collect_tabs", std::move(request))) {
                break;
            }
        }
        browser::inbox::PendingRestore pending;
        if (browser::inbox::next_restore(pending)) {
            Value payload = browser::build_restore_message(pending.windows, pending.lazy_load);
            payload.set("requestId", Value(pending.id));
            if (!send("restore_tabs", std::move(payload))) {
                break;
            }
        }
    }

    if (registered) {
        bridge_->unregister_host_connection(connection_id);
        Value disconnected = Value::object();
        disconnected.set("browser", Value(std::string(core::to_string(browser))));
        publish(ipc::EventType::BrowserDisconnected, std::move(disconnected));
    }
    connection->close();
}

}  // namespace contextsnap::daemon
