#include <contextsnap/ipc/protocol.hpp>

#include <array>
#include <utility>

namespace contextsnap::ipc {
namespace {

constexpr std::array<std::pair<Method, std::string_view>, 21> kMethodNames{{
    {Method::Unknown, "unknown"},
    {Method::Hello, "hello"},
    {Method::Ping, "ping"},
    {Method::CaptureSnapshot, "capture_snapshot"},
    {Method::ListSnapshots, "list_snapshots"},
    {Method::GetSnapshot, "get_snapshot"},
    {Method::DeleteSnapshot, "delete_snapshot"},
    {Method::RenameSnapshot, "rename_snapshot"},
    {Method::TagSnapshot, "tag_snapshot"},
    {Method::FavoriteSnapshot, "favorite_snapshot"},
    {Method::PlanRestore, "plan_restore"},
    {Method::RestoreSnapshot, "restore_snapshot"},
    {Method::SearchSnapshots, "search_snapshots"},
    {Method::ExportSnapshot, "export_snapshot"},
    {Method::ImportSnapshot, "import_snapshot"},
    {Method::GetConfig, "get_config"},
    {Method::SetConfig, "set_config"},
    {Method::GetHealth, "get_health"},
    {Method::PruneSnapshots, "prune_snapshots"},
    {Method::Subscribe, "subscribe"},
    {Method::Shutdown, "shutdown"},
}};

}  // namespace

std::string_view to_string(Method method) noexcept {
    for (const auto& entry : kMethodNames) {
        if (entry.first == method) {
            return entry.second;
        }
    }
    return "unknown";
}

Method method_from_string(std::string_view text) noexcept {
    for (const auto& entry : kMethodNames) {
        if (entry.second == text) {
            return entry.first;
        }
    }
    return Method::Unknown;
}

std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::SnapshotCreated:
            return "snapshot_created";
        case EventType::SnapshotDeleted:
            return "snapshot_deleted";
        case EventType::SnapshotUpdated:
            return "snapshot_updated";
        case EventType::RestoreStarted:
            return "restore_started";
        case EventType::RestoreProgress:
            return "restore_progress";
        case EventType::RestoreFinished:
            return "restore_finished";
        case EventType::BrowserConnected:
            return "browser_connected";
        case EventType::BrowserDisconnected:
            return "browser_disconnected";
        case EventType::ConfigChanged:
            return "config_changed";
        case EventType::DaemonShuttingDown:
            break;
    }
    return "daemon_shutting_down";
}

core::json::Value Request::to_json() const {
    core::json::Object object;
    object.emplace_back("v", core::json::Value(static_cast<std::int64_t>(kProtocolVersion)));
    object.emplace_back("id", core::json::Value(static_cast<std::int64_t>(id)));
    object.emplace_back("method", core::json::Value(std::string(to_string(method))));
    object.emplace_back("params", params);
    return core::json::Value(std::move(object));
}

Result<Request> Request::from_json(const core::json::Value& value) {
    if (!value.is_object()) {
        return core::err::protocol("request must be a JSON object", "ipc.request");
    }
    Request request;
    if (const core::json::Value* version = value.find("v");
        version != nullptr && version->as_int(1) > static_cast<std::int64_t>(kProtocolVersion)) {
        return core::err::protocol("client protocol version is newer than the daemon's",
                                   "ipc.request");
    }
    if (const core::json::Value* id = value.find("id"); id != nullptr) {
        request.id = static_cast<std::uint64_t>(id->as_int(0));
    }
    const core::json::Value* method = value.find("method");
    if (method == nullptr || !method->is_string()) {
        return core::err::protocol("request is missing a method", "ipc.request");
    }
    request.method = method_from_string(method->as_string());
    if (request.method == Method::Unknown) {
        return core::err::protocol("unknown method: " + method->as_string(), "ipc.request");
    }
    if (const core::json::Value* params = value.find("params"); params != nullptr) {
        request.params = *params;
    }
    return request;
}

Response Response::success(std::uint64_t id, core::json::Value result) {
    Response response;
    response.id = id;
    response.ok = true;
    response.result = std::move(result);
    return response;
}

Response Response::failure(std::uint64_t id, const core::Error& error) {
    Response response;
    response.id = id;
    response.ok = false;
    response.error_code = error.code;
    response.error_message = error.to_string();
    return response;
}

core::json::Value Response::to_json() const {
    core::json::Object object;
    object.emplace_back("v", core::json::Value(static_cast<std::int64_t>(kProtocolVersion)));
    object.emplace_back("id", core::json::Value(static_cast<std::int64_t>(id)));
    object.emplace_back("ok", core::json::Value(ok));
    if (ok) {
        object.emplace_back("result", result);
    } else {
        core::json::Object error;
        error.emplace_back("code", core::json::Value(std::string(core::to_string(error_code))));
        error.emplace_back("message", core::json::Value(error_message));
        object.emplace_back("error", core::json::Value(std::move(error)));
    }
    return core::json::Value(std::move(object));
}

Result<Response> Response::from_json(const core::json::Value& value) {
    if (!value.is_object()) {
        return core::err::protocol("response must be a JSON object", "ipc.response");
    }
    Response response;
    if (const core::json::Value* id = value.find("id"); id != nullptr) {
        response.id = static_cast<std::uint64_t>(id->as_int(0));
    }
    const core::json::Value* ok = value.find("ok");
    response.ok = ok != nullptr && ok->as_bool(false);
    if (response.ok) {
        if (const core::json::Value* result = value.find("result"); result != nullptr) {
            response.result = *result;
        }
        return response;
    }
    if (const core::json::Value* error = value.find("error"); error != nullptr) {
        if (const core::json::Value* message = error->find("message"); message != nullptr) {
            response.error_message = message->as_string();
        }
        if (const core::json::Value* code = error->find("code"); code != nullptr) {
            const std::string text = code->as_string();
            for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(core::ErrorCode::Internal);
                 ++i) {
                const auto candidate = static_cast<core::ErrorCode>(i);
                if (core::to_string(candidate) == text) {
                    response.error_code = candidate;
                    break;
                }
            }
        }
    }
    return response;
}

core::json::Value Event::to_json() const {
    core::json::Object object;
    object.emplace_back("v", core::json::Value(static_cast<std::int64_t>(kProtocolVersion)));
    object.emplace_back("event", core::json::Value(std::string(to_string(type))));
    object.emplace_back("payload", payload);
    return core::json::Value(std::move(object));
}

Result<Event> Event::from_json(const core::json::Value& value) {
    const core::json::Value* name = value.find("event");
    if (name == nullptr || !name->is_string()) {
        return core::err::protocol("event is missing a type", "ipc.event");
    }
    Event event;
    const std::string text = name->as_string();
    for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(EventType::DaemonShuttingDown); ++i) {
        const auto candidate = static_cast<EventType>(i);
        if (to_string(candidate) == text) {
            event.type = candidate;
            break;
        }
    }
    if (const core::json::Value* payload = value.find("payload"); payload != nullptr) {
        event.payload = *payload;
    }
    return event;
}

Result<std::vector<std::uint8_t>> encode_frame(std::string_view payload, std::uint32_t max_bytes) {
    if (payload.size() > max_bytes) {
        return core::err::protocol("payload exceeds the frame limit", "ipc.encode_frame");
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame;
    frame.reserve(kLengthPrefixBytes + payload.size());
    frame.push_back(static_cast<std::uint8_t>(length & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

Result<std::optional<DecodedFrame>> decode_frame(std::string_view buffer, std::uint32_t max_bytes) {
    if (buffer.size() < kLengthPrefixBytes) {
        return std::optional<DecodedFrame>{};
    }
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[index]));
    };
    const std::uint32_t length = byte(0) | (byte(1) << 8) | (byte(2) << 16) | (byte(3) << 24);
    if (length > max_bytes) {
        return core::err::protocol("frame length exceeds the limit", "ipc.decode_frame");
    }
    if (buffer.size() < kLengthPrefixBytes + length) {
        return std::optional<DecodedFrame>{};
    }
    DecodedFrame frame;
    frame.payload = std::string(buffer.substr(kLengthPrefixBytes, length));
    frame.consumed = kLengthPrefixBytes + length;
    return std::optional<DecodedFrame>{std::move(frame)};
}

void FrameReader::feed(std::string_view bytes) { buffer_.append(bytes); }

Result<std::optional<std::string>> FrameReader::next() {
    auto decoded = decode_frame(buffer_, max_bytes_);
    if (!decoded) {
        return decoded.error();
    }
    if (!decoded.value().has_value()) {
        return std::optional<std::string>{};
    }
    std::string payload = decoded.value()->payload;
    buffer_.erase(0, decoded.value()->consumed);
    return std::optional<std::string>{std::move(payload)};
}

}  // namespace contextsnap::ipc
