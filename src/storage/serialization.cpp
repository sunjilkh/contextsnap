#include <contextsnap/storage/serialization.hpp>

#include <contextsnap/core/ulid.hpp>
#include <contextsnap/version.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace contextsnap::storage {
namespace {

using core::json::Array;
using core::json::Object;
using core::json::Value;

Value rect_to_json(const core::Rect& rect) {
    Object object;
    object.emplace_back("x", Value(static_cast<std::int64_t>(rect.x)));
    object.emplace_back("y", Value(static_cast<std::int64_t>(rect.y)));
    object.emplace_back("width", Value(static_cast<std::int64_t>(rect.width)));
    object.emplace_back("height", Value(static_cast<std::int64_t>(rect.height)));
    return Value(std::move(object));
}

core::Rect rect_from_json(const Value* value) {
    core::Rect rect;
    if (value == nullptr || !value->is_object()) {
        return rect;
    }
    const auto read = [&](const char* key) {
        const Value* field = value->find(key);
        return field == nullptr ? 0 : static_cast<std::int32_t>(field->as_int(0));
    };
    rect.x = read("x");
    rect.y = read("y");
    rect.width = read("width");
    rect.height = read("height");
    return rect;
}

Value strings_to_json(const std::vector<std::string>& values) {
    Array array;
    array.reserve(values.size());
    for (const std::string& value : values) {
        array.emplace_back(Value(value));
    }
    return Value(std::move(array));
}

std::vector<std::string> strings_from_json(const Value* value) {
    std::vector<std::string> out;
    if (value == nullptr || !value->is_array()) {
        return out;
    }
    for (const Value& item : value->as_array()) {
        out.push_back(item.as_string());
    }
    return out;
}

void put_optional(Object& object, const char* key, const std::optional<std::string>& value) {
    if (value.has_value()) {
        object.emplace_back(key, Value(*value));
    }
}

std::optional<std::string> optional_string(const Value& parent, const char* key) {
    const Value* value = parent.find(key);
    if (value == nullptr || value->is_null() || value->as_string().empty()) {
        return std::nullopt;
    }
    return value->as_string();
}

bool read_bool(const Value& parent, const char* key, bool fallback = false) {
    const Value* value = parent.find(key);
    return value == nullptr ? fallback : value->as_bool(fallback);
}

std::int64_t read_int(const Value& parent, const char* key, std::int64_t fallback = 0) {
    const Value* value = parent.find(key);
    return value == nullptr ? fallback : value->as_int(fallback);
}

std::string read_string(const Value& parent, const char* key) {
    const Value* value = parent.find(key);
    return value == nullptr ? std::string() : value->as_string();
}

core::PlatformKind platform_from_text(std::string_view text) {
    for (const auto kind : {core::PlatformKind::Windows, core::PlatformKind::MacOS,
                            core::PlatformKind::Linux}) {
        if (core::to_string(kind) == text) {
            return kind;
        }
    }
    return core::PlatformKind::Unknown;
}

core::SessionType session_from_text(std::string_view text) {
    for (const auto type : {core::SessionType::Win32, core::SessionType::Quartz,
                            core::SessionType::X11, core::SessionType::Wayland}) {
        if (core::to_string(type) == text) {
            return type;
        }
    }
    return core::SessionType::Unknown;
}

}  // namespace

Result<Format> format_from_string(std::string_view text) {
    if (text == "json") return Format::Json;
    if (text == "json-pretty" || text == "pretty") return Format::JsonPretty;
    if (text == "flatbuffers" || text == "fb" || text == "binary") return Format::FlatBuffers;
    return core::err::invalid("unknown format: " + std::string(text), "storage.format");
}

std::string_view to_string(Format format) noexcept {
    switch (format) {
        case Format::Json:
            return "json";
        case Format::JsonPretty:
            return "json-pretty";
        case Format::FlatBuffers:
            break;
    }
    return "flatbuffers";
}

std::string format_timestamp(core::Timestamp timestamp) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(timestamp);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - seconds).count();
    const std::time_t tt = core::Clock::to_time_t(seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << millis << 'Z';
    return out.str();
}

Result<core::Timestamp> parse_timestamp(std::string_view text) {
    if (text.size() < 19) {
        return core::err::invalid("timestamp too short", "storage.time");
    }
    std::tm tm{};
    int millis = 0;
    const std::string copy(text);
    const int matched = std::sscanf(copy.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d", &tm.tm_year,
                                    &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                                    &millis);
    if (matched < 6) {
        return core::err::invalid("malformed RFC 3339 timestamp", "storage.time");
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
#if defined(_WIN32)
    const std::time_t tt = _mkgmtime(&tm);
#else
    const std::time_t tt = timegm(&tm);
#endif
    return core::Clock::from_time_t(tt) + std::chrono::milliseconds{millis};
}

std::int64_t to_unix_millis(core::Timestamp timestamp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())
        .count();
}

core::Timestamp from_unix_millis(std::int64_t millis) {
    return core::Timestamp{std::chrono::milliseconds{millis}};
}

Value to_json(const core::TabInfo& tab) {
    Object object;
    object.emplace_back("id", Value(tab.id));
    object.emplace_back("index", Value(static_cast<std::int64_t>(tab.index)));
    object.emplace_back("url", Value(tab.url));
    object.emplace_back("title", Value(tab.title));
    put_optional(object, "favicon_path", tab.favicon_path);
    put_optional(object, "group_id", tab.group_id);
    put_optional(object, "group_title", tab.group_title);
    put_optional(object, "cookie_store_id", tab.cookie_store_id);
    if (tab.opener_index.has_value()) {
        object.emplace_back("opener_index", Value(static_cast<std::int64_t>(*tab.opener_index)));
    }
    object.emplace_back("scroll_y", Value(static_cast<std::int64_t>(tab.scroll_y)));
    object.emplace_back("pinned", Value(tab.pinned));
    object.emplace_back("active", Value(tab.active));
    object.emplace_back("audible", Value(tab.audible));
    object.emplace_back("muted", Value(tab.muted));
    object.emplace_back("discarded", Value(tab.discarded));
    object.emplace_back("incognito", Value(tab.incognito));
    object.emplace_back("sanitized", Value(tab.sanitized));
    return Value(std::move(object));
}

Value to_json(const core::MonitorInfo& monitor) {
    Object object;
    object.emplace_back("id", Value(monitor.id));
    object.emplace_back("name", Value(monitor.name));
    object.emplace_back("bounds", rect_to_json(monitor.bounds));
    object.emplace_back("work_area", rect_to_json(monitor.work_area));
    object.emplace_back("dpi", Value(static_cast<std::int64_t>(monitor.dpi)));
    object.emplace_back("scale_factor", Value(monitor.scale_factor));
    object.emplace_back("refresh_hz", Value(static_cast<std::int64_t>(monitor.refresh_hz)));
    object.emplace_back("orientation", Value(monitor.orientation));
    object.emplace_back("primary", Value(monitor.primary));
    put_optional(object, "edid_hash", monitor.edid_hash);
    return Value(std::move(object));
}

Value to_json(const core::WindowInfo& window) {
    Object process;
    process.emplace_back("pid", Value(static_cast<std::int64_t>(window.process.pid)));
    process.emplace_back("executable_path", Value(window.process.executable_path));
    process.emplace_back("command_line", strings_to_json(window.process.command_line));
    process.emplace_back("working_directory", Value(window.process.working_directory));
    process.emplace_back("app_id", Value(window.process.app_id));
    process.emplace_back("user", Value(window.process.user));
    process.emplace_back("single_instance", Value(window.process.single_instance));
    process.emplace_back("elevated", Value(window.process.elevated));

    Array tabs;
    tabs.reserve(window.tabs.size());
    for (const core::TabInfo& tab : window.tabs) {
        tabs.push_back(to_json(tab));
    }

    Object object;
    object.emplace_back("id", Value(window.id));
    object.emplace_back("native_handle", Value(static_cast<std::int64_t>(window.native_handle)));
    object.emplace_back("title", Value(window.title));
    object.emplace_back("window_class", Value(window.window_class));
    object.emplace_back("frame", rect_to_json(window.frame));
    object.emplace_back("client_area", rect_to_json(window.client_area));
    object.emplace_back("restored_frame", rect_to_json(window.restored_frame));
    object.emplace_back("state", Value(std::string(core::to_string(window.state))));
    object.emplace_back("z_order", Value(static_cast<std::int64_t>(window.z_order)));
    object.emplace_back("monitor_id", Value(window.monitor_id));
    put_optional(object, "workspace_id", window.workspace_id);
    put_optional(object, "workspace_name", window.workspace_name);
    object.emplace_back("opacity", Value(window.opacity));
    object.emplace_back("focused", Value(window.focused));
    object.emplace_back("always_on_top", Value(window.always_on_top));
    object.emplace_back("minimizable", Value(window.minimizable));
    object.emplace_back("resizable", Value(window.resizable));
    object.emplace_back("captured_geometry_reliable", Value(window.captured_geometry_reliable));
    object.emplace_back("process", Value(std::move(process)));
    object.emplace_back("browser", Value(std::string(core::to_string(window.browser))));
    put_optional(object, "browser_profile", window.browser_profile);
    object.emplace_back("tabs", Value(std::move(tabs)));
    return Value(std::move(object));
}

Value to_json(const core::SnapshotSummary& summary) {
    Object object;
    object.emplace_back("id", Value(summary.id));
    object.emplace_back("name", Value(summary.name));
    object.emplace_back("tags", strings_to_json(summary.tags));
    object.emplace_back("created_at", Value(format_timestamp(summary.created_at)));
    object.emplace_back("window_count", Value(static_cast<std::int64_t>(summary.window_count)));
    object.emplace_back("tab_count", Value(static_cast<std::int64_t>(summary.tab_count)));
    object.emplace_back("monitor_count", Value(static_cast<std::int64_t>(summary.monitor_count)));
    object.emplace_back("favorite", Value(summary.favorite));
    object.emplace_back("automatic", Value(summary.automatic));
    return Value(std::move(object));
}

Value to_json(const core::RestorePlan& plan) {
    Array steps;
    steps.reserve(plan.steps.size());
    for (const core::RestoreStep& step : plan.steps) {
        Object object;
        object.emplace_back("kind", Value(static_cast<std::int64_t>(step.kind)));
        object.emplace_back("target_id", Value(step.target_id));
        object.emplace_back("description", Value(step.description));
        object.emplace_back("skipped", Value(step.skipped));
        if (!step.skip_reason.empty()) {
            object.emplace_back("skip_reason", Value(step.skip_reason));
        }
        steps.emplace_back(Value(std::move(object)));
    }
    Object object;
    object.emplace_back("snapshot_id", Value(plan.snapshot_id));
    object.emplace_back("steps", Value(std::move(steps)));
    object.emplace_back("warnings", strings_to_json(plan.warnings));
    object.emplace_back("estimated_duration_ms",
                        Value(static_cast<std::int64_t>(plan.estimated_duration_ms)));
    return Value(std::move(object));
}

Value to_json(const core::RestoreReport& report) {
    Object object;
    object.emplace_back("snapshot_id", Value(report.snapshot_id));
    object.emplace_back("windows_restored",
                        Value(static_cast<std::int64_t>(report.windows_restored)));
    object.emplace_back("windows_failed", Value(static_cast<std::int64_t>(report.windows_failed)));
    object.emplace_back("apps_launched", Value(static_cast<std::int64_t>(report.apps_launched)));
    object.emplace_back("tabs_restored", Value(static_cast<std::int64_t>(report.tabs_restored)));
    object.emplace_back("duration_ms", Value(static_cast<std::int64_t>(report.duration_ms)));
    object.emplace_back("warnings", strings_to_json(report.warnings));
    object.emplace_back("errors", strings_to_json(report.errors));
    return Value(std::move(object));
}

Value to_json(const Snapshot& snapshot) {
    Object metadata;
    metadata.emplace_back("id", Value(snapshot.metadata.id));
    metadata.emplace_back("name", Value(snapshot.metadata.name));
    metadata.emplace_back("description", Value(snapshot.metadata.description));
    metadata.emplace_back("tags", strings_to_json(snapshot.metadata.tags));
    metadata.emplace_back("created_at", Value(format_timestamp(snapshot.metadata.created_at)));
    metadata.emplace_back("updated_at", Value(format_timestamp(snapshot.metadata.updated_at)));
    metadata.emplace_back("platform",
                          Value(std::string(core::to_string(snapshot.metadata.platform))));
    metadata.emplace_back("session_type",
                          Value(std::string(core::to_string(snapshot.metadata.session_type))));
    metadata.emplace_back("host_name", Value(snapshot.metadata.host_name));
    metadata.emplace_back("os_version", Value(snapshot.metadata.os_version));
    metadata.emplace_back("app_version", Value(snapshot.metadata.app_version));
    metadata.emplace_back("schema_version",
                          Value(static_cast<std::int64_t>(snapshot.metadata.schema_version)));
    metadata.emplace_back("capture_duration_ms",
                          Value(static_cast<std::int64_t>(snapshot.metadata.capture_duration_ms)));
    metadata.emplace_back("favorite", Value(snapshot.metadata.favorite));
    metadata.emplace_back("automatic", Value(snapshot.metadata.automatic));

    Array monitors;
    for (const core::MonitorInfo& monitor : snapshot.monitors) {
        monitors.push_back(to_json(monitor));
    }
    Array windows;
    for (const core::WindowInfo& window : snapshot.windows) {
        windows.push_back(to_json(window));
    }

    Object cursor;
    cursor.emplace_back("x", Value(static_cast<std::int64_t>(snapshot.cursor.x)));
    cursor.emplace_back("y", Value(static_cast<std::int64_t>(snapshot.cursor.y)));
    cursor.emplace_back("monitor_id", Value(snapshot.cursor.monitor_id));
    put_optional(cursor, "focused_window_id", snapshot.cursor.focused_window_id);

    Object object;
    object.emplace_back("metadata", Value(std::move(metadata)));
    object.emplace_back("monitors", Value(std::move(monitors)));
    object.emplace_back("windows", Value(std::move(windows)));
    object.emplace_back("cursor", Value(std::move(cursor)));
    put_optional(object, "active_workspace_id", snapshot.active_workspace_id);
    return Value(std::move(object));
}

Result<core::TabInfo> tab_from_json(const Value& value) {
    if (!value.is_object()) {
        return core::err::invalid("tab must be an object", "storage.deserialize");
    }
    core::TabInfo tab;
    tab.id = read_string(value, "id");
    tab.index = static_cast<std::int32_t>(read_int(value, "index"));
    tab.url = read_string(value, "url");
    tab.title = read_string(value, "title");
    tab.favicon_path = optional_string(value, "favicon_path");
    tab.group_id = optional_string(value, "group_id");
    tab.group_title = optional_string(value, "group_title");
    tab.cookie_store_id = optional_string(value, "cookie_store_id");
    if (const Value* opener = value.find("opener_index"); opener != nullptr && !opener->is_null()) {
        tab.opener_index = static_cast<std::int32_t>(opener->as_int(-1));
    }
    tab.scroll_y = static_cast<std::int32_t>(read_int(value, "scroll_y"));
    tab.pinned = read_bool(value, "pinned");
    tab.active = read_bool(value, "active");
    tab.audible = read_bool(value, "audible");
    tab.muted = read_bool(value, "muted");
    tab.discarded = read_bool(value, "discarded");
    tab.incognito = read_bool(value, "incognito");
    tab.sanitized = read_bool(value, "sanitized");
    return tab;
}

namespace {

core::MonitorInfo monitor_from_json(const Value& value) {
    core::MonitorInfo monitor;
    monitor.id = read_string(value, "id");
    monitor.name = read_string(value, "name");
    monitor.bounds = rect_from_json(value.find("bounds"));
    monitor.work_area = rect_from_json(value.find("work_area"));
    monitor.dpi = static_cast<std::uint32_t>(read_int(value, "dpi", 96));
    if (const Value* scale = value.find("scale_factor"); scale != nullptr) {
        monitor.scale_factor = scale->as_double(1.0);
    }
    monitor.refresh_hz = static_cast<std::uint32_t>(read_int(value, "refresh_hz"));
    monitor.orientation = read_string(value, "orientation");
    monitor.primary = read_bool(value, "primary");
    monitor.edid_hash = optional_string(value, "edid_hash");
    if (monitor.work_area.empty()) {
        monitor.work_area = monitor.bounds;
    }
    return monitor;
}

Result<core::WindowInfo> window_from_json(const Value& value) {
    if (!value.is_object()) {
        return core::err::invalid("window must be an object", "storage.deserialize");
    }
    core::WindowInfo window;
    window.id = read_string(value, "id");
    window.native_handle = static_cast<std::uint64_t>(read_int(value, "native_handle"));
    window.title = read_string(value, "title");
    window.window_class = read_string(value, "window_class");
    window.frame = rect_from_json(value.find("frame"));
    window.client_area = rect_from_json(value.find("client_area"));
    window.restored_frame = rect_from_json(value.find("restored_frame"));
    window.state = core::window_state_from_string(read_string(value, "state"));
    window.z_order = static_cast<std::int32_t>(read_int(value, "z_order"));
    window.monitor_id = read_string(value, "monitor_id");
    window.workspace_id = optional_string(value, "workspace_id");
    window.workspace_name = optional_string(value, "workspace_name");
    if (const Value* opacity = value.find("opacity"); opacity != nullptr) {
        window.opacity = opacity->as_double(1.0);
    }
    window.focused = read_bool(value, "focused");
    window.always_on_top = read_bool(value, "always_on_top");
    window.minimizable = read_bool(value, "minimizable", true);
    window.resizable = read_bool(value, "resizable", true);
    window.captured_geometry_reliable = read_bool(value, "captured_geometry_reliable", true);
    window.browser = core::browser_kind_from_string(read_string(value, "browser"));
    window.browser_profile = optional_string(value, "browser_profile");

    if (const Value* process = value.find("process"); process != nullptr && process->is_object()) {
        window.process.pid = static_cast<std::uint64_t>(read_int(*process, "pid"));
        window.process.executable_path = read_string(*process, "executable_path");
        window.process.command_line = strings_from_json(process->find("command_line"));
        window.process.working_directory = read_string(*process, "working_directory");
        window.process.app_id = read_string(*process, "app_id");
        window.process.user = read_string(*process, "user");
        window.process.single_instance = read_bool(*process, "single_instance");
        window.process.elevated = read_bool(*process, "elevated");
    }
    if (const Value* tabs = value.find("tabs"); tabs != nullptr && tabs->is_array()) {
        for (const Value& item : tabs->as_array()) {
            auto tab = tab_from_json(item);
            if (!tab) {
                return tab.error();
            }
            window.tabs.push_back(tab.value());
        }
    }
    if (window.restored_frame.empty()) {
        window.restored_frame = window.frame;
    }
    return window;
}

}  // namespace

Result<Snapshot> snapshot_from_json(const Value& value) {
    if (!value.is_object()) {
        return core::err::invalid("snapshot must be an object", "storage.deserialize");
    }
    Snapshot snapshot;
    const Value* metadata = value.find("metadata");
    if (metadata == nullptr || !metadata->is_object()) {
        return core::err::invalid("snapshot is missing metadata", "storage.deserialize");
    }
    snapshot.metadata.id = read_string(*metadata, "id");
    if (snapshot.metadata.id.empty()) {
        snapshot.metadata.id = core::generate_ulid();
    } else if (!core::is_valid_ulid(snapshot.metadata.id)) {
        return core::err::invalid("snapshot id is not a ULID", "storage.deserialize");
    }
    snapshot.metadata.name = read_string(*metadata, "name");
    snapshot.metadata.description = read_string(*metadata, "description");
    snapshot.metadata.tags = strings_from_json(metadata->find("tags"));
    if (auto created = parse_timestamp(read_string(*metadata, "created_at")); created) {
        snapshot.metadata.created_at = created.value();
    }
    if (auto updated = parse_timestamp(read_string(*metadata, "updated_at")); updated) {
        snapshot.metadata.updated_at = updated.value();
    }
    snapshot.metadata.platform = platform_from_text(read_string(*metadata, "platform"));
    snapshot.metadata.session_type = session_from_text(read_string(*metadata, "session_type"));
    snapshot.metadata.host_name = read_string(*metadata, "host_name");
    snapshot.metadata.os_version = read_string(*metadata, "os_version");
    snapshot.metadata.app_version = read_string(*metadata, "app_version");
    snapshot.metadata.schema_version =
        static_cast<std::uint32_t>(read_int(*metadata, "schema_version", 1));
    if (snapshot.metadata.schema_version > kSnapshotSchemaVersion) {
        return core::err::unsupported("snapshot schema is newer than this build supports",
                                      "storage.deserialize");
    }
    snapshot.metadata.capture_duration_ms =
        static_cast<std::uint32_t>(read_int(*metadata, "capture_duration_ms"));
    snapshot.metadata.favorite = read_bool(*metadata, "favorite");
    snapshot.metadata.automatic = read_bool(*metadata, "automatic");

    if (const Value* monitors = value.find("monitors");
        monitors != nullptr && monitors->is_array()) {
        for (const Value& item : monitors->as_array()) {
            snapshot.monitors.push_back(monitor_from_json(item));
        }
    }
    if (const Value* windows = value.find("windows"); windows != nullptr && windows->is_array()) {
        for (const Value& item : windows->as_array()) {
            auto window = window_from_json(item);
            if (!window) {
                return window.error();
            }
            snapshot.windows.push_back(window.value());
        }
    }
    if (const Value* cursor = value.find("cursor"); cursor != nullptr && cursor->is_object()) {
        snapshot.cursor.x = static_cast<std::int32_t>(read_int(*cursor, "x"));
        snapshot.cursor.y = static_cast<std::int32_t>(read_int(*cursor, "y"));
        snapshot.cursor.monitor_id = read_string(*cursor, "monitor_id");
        snapshot.cursor.focused_window_id = optional_string(*cursor, "focused_window_id");
    }
    snapshot.active_workspace_id = optional_string(value, "active_workspace_id");
    return snapshot;
}

Result<std::string> serialize(const Snapshot& snapshot, Format format) {
    if (format == Format::Json) {
        return to_json(snapshot).dump();
    }
    if (format == Format::JsonPretty) {
        return to_json(snapshot).dump(2);
    }
#if defined(CONTEXTSNAP_WITH_FLATBUFFERS)
    auto buffer = to_flatbuffer(snapshot);
    if (!buffer) {
        return buffer.error();
    }
    return std::string(buffer.value().begin(), buffer.value().end());
#else
    return core::err::unsupported("this build has no FlatBuffers support", "storage.serialize");
#endif
}

Result<Snapshot> deserialize(std::string_view payload, Format format) {
    if (format == Format::FlatBuffers) {
#if defined(CONTEXTSNAP_WITH_FLATBUFFERS)
        return from_flatbuffer(std::vector<std::uint8_t>(payload.begin(), payload.end()));
#else
        return core::err::unsupported("this build has no FlatBuffers support",
                                      "storage.deserialize");
#endif
    }
    auto parsed = core::json::parse(payload);
    if (!parsed) {
        return parsed.error();
    }
    // Accept both a bare snapshot object and a full export envelope.
    if (parsed.value().contains("snapshot")) {
        auto unwrapped = unwrap_document(payload);
        if (!unwrapped) {
            return unwrapped.error();
        }
        return snapshot_from_json(unwrapped.value());
    }
    return snapshot_from_json(parsed.value());
}

std::string wrap_document(const core::json::Value& snapshot_json) {
    Object envelope;
    envelope.emplace_back("format", Value(std::string("contextsnap.snapshot")));
    envelope.emplace_back("version", Value(static_cast<std::int64_t>(kSnapshotSchemaVersion)));
    envelope.emplace_back("exported_at", Value(format_timestamp(core::Clock::now())));
    envelope.emplace_back("producer",
                          Value(std::string(kProjectName) + " " + std::string(kVersion)));
    envelope.emplace_back("snapshot", snapshot_json);
    return Value(std::move(envelope)).dump(2);
}

Result<core::json::Value> unwrap_document(std::string_view text) {
    auto parsed = core::json::parse(text);
    if (!parsed) {
        return parsed.error();
    }
    const Value& document = parsed.value();
    const Value* format = document.find("format");
    if (format == nullptr || format->as_string() != "contextsnap.snapshot") {
        return core::err::invalid("not a ContextSnap snapshot document", "storage.unwrap");
    }
    if (const Value* version = document.find("version");
        version != nullptr && version->as_int(1) > static_cast<std::int64_t>(kSnapshotSchemaVersion)) {
        return core::err::unsupported("document version is newer than this build supports",
                                      "storage.unwrap");
    }
    const Value* snapshot = document.find("snapshot");
    if (snapshot == nullptr) {
        return core::err::invalid("document has no snapshot member", "storage.unwrap");
    }
    return *snapshot;
}

}  // namespace contextsnap::storage
