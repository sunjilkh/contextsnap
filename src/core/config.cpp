#include <contextsnap/core/config.hpp>

#include <contextsnap/version.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace contextsnap::core {
namespace {

namespace fs = std::filesystem;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string home_dir() {
#if defined(_WIN32)
    const std::string profile = env("USERPROFILE");
    if (!profile.empty()) {
        return profile;
    }
    return env("HOMEDRIVE") + env("HOMEPATH");
#else
    const std::string home = env("HOME");
    return home.empty() ? std::string("/tmp") : home;
#endif
}

std::string join(const std::string& base, const std::string& leaf) {
    return (fs::path(base) / leaf).string();
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string unquote(const std::string& text) {
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front()) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

std::vector<std::string> parse_array(const std::string& text) {
    std::vector<std::string> values;
    if (text.size() < 2 || text.front() != '[') {
        return values;
    }
    std::string inner = text.substr(1, text.find_last_of(']') - 1);
    std::stringstream stream(inner);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const std::string value = unquote(trim(item));
        if (!value.empty()) {
            values.push_back(value);
        }
    }
    return values;
}

bool parse_bool(const std::string& text, bool fallback) {
    const std::string value = unquote(trim(text));
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

std::uint32_t parse_u32(const std::string& text, std::uint32_t fallback) {
    try {
        return static_cast<std::uint32_t>(std::stoul(unquote(trim(text))));
    } catch (const std::exception&) {
        return fallback;
    }
}

/// Applies one `section.key = value` pair. Unknown keys are ignored so that a
/// config written by a newer build still loads on an older one.
void apply(Config& config, const std::string& section, const std::string& key,
           const std::string& raw) {
    const std::string value = unquote(trim(raw));
    if (section == "storage") {
        if (key == "database_path") config.storage.database_path = value;
        else if (key == "encrypt") config.storage.encrypt = parse_bool(raw, config.storage.encrypt);
        else if (key == "retention_days") config.storage.retention_days = parse_u32(raw, 90);
        else if (key == "max_snapshots") config.storage.max_snapshots = parse_u32(raw, 500);
        else if (key == "vacuum_interval_days") config.storage.vacuum_interval_days = parse_u32(raw, 14);
        else if (key == "store_favicons") config.storage.store_favicons = parse_bool(raw, true);
        else if (key == "favicon_cache_mb") config.storage.favicon_cache_mb = parse_u32(raw, 32);
    } else if (section == "capture") {
        if (key == "auto_capture") config.capture.auto_capture = parse_bool(raw, false);
        else if (key == "auto_capture_interval_minutes") config.capture.auto_capture_interval_minutes = parse_u32(raw, 30);
        else if (key == "capture_on_idle") config.capture.capture_on_idle = parse_bool(raw, true);
        else if (key == "idle_threshold_minutes") config.capture.idle_threshold_minutes = parse_u32(raw, 10);
        else if (key == "capture_before_shutdown") config.capture.capture_before_shutdown = parse_bool(raw, true);
        else if (key == "include_minimized") config.capture.include_minimized = parse_bool(raw, true);
        else if (key == "include_incognito") config.capture.include_incognito = parse_bool(raw, false);
        else if (key == "excluded_app_ids") config.capture.excluded_app_ids = parse_array(raw);
        else if (key == "excluded_url_patterns") config.capture.excluded_url_patterns = parse_array(raw);
        else if (key == "browser_timeout_ms") config.capture.browser_timeout_ms = parse_u32(raw, 700);
    } else if (section == "restore") {
        if (key == "launch_missing_apps") config.restore.launch_missing_apps = parse_bool(raw, true);
        else if (key == "lazy_load_tabs") config.restore.lazy_load_tabs = parse_bool(raw, true);
        else if (key == "restore_cursor") config.restore.restore_cursor = parse_bool(raw, true);
        else if (key == "restore_z_order") config.restore.restore_z_order = parse_bool(raw, true);
        else if (key == "app_launch_timeout_ms") config.restore.app_launch_timeout_ms = parse_u32(raw, 8000);
        else if (key == "window_settle_timeout_ms") config.restore.window_settle_timeout_ms = parse_u32(raw, 2500);
        else if (key == "max_parallel_launches") config.restore.max_parallel_launches = parse_u32(raw, 4);
    } else if (section == "privacy") {
        if (key == "sanitize_urls") config.privacy.sanitize_urls = parse_bool(raw, true);
        else if (key == "strip_query_strings") config.privacy.strip_query_strings = parse_bool(raw, false);
        else if (key == "hash_excluded_urls") config.privacy.hash_excluded_urls = parse_bool(raw, true);
        else if (key == "extra_sensitive_params") config.privacy.extra_sensitive_params = parse_array(raw);
        else if (key == "domain_denylist") config.privacy.domain_denylist = parse_array(raw);
        else if (key == "domain_allowlist") config.privacy.domain_allowlist = parse_array(raw);
    } else if (section == "hotkeys") {
        if (key == "quick_switcher") config.hotkeys.quick_switcher = value;
        else if (key == "save_snapshot") config.hotkeys.save_snapshot = value;
        else if (key == "restore_last") config.hotkeys.restore_last = value;
        else if (key == "enabled") config.hotkeys.enabled = parse_bool(raw, true);
    } else if (section == "ipc") {
        if (key == "socket_path") config.ipc.socket_path = value;
        else if (key == "max_frame_bytes") config.ipc.max_frame_bytes = parse_u32(raw, 16u * 1024u * 1024u);
        else if (key == "request_timeout_ms") config.ipc.request_timeout_ms = parse_u32(raw, 5000);
        else if (key == "max_clients") config.ipc.max_clients = parse_u32(raw, 16);
    } else {  // Top-level keys.
        if (key == "log_level") config.log_level = log::level_from_string(value);
        else if (key == "log_file") config.log_file = value;
    }
}

void apply_environment(Config& config) {
    const struct {
        const char* name;
        const char* section;
        const char* key;
    } kOverrides[] = {
        {"CONTEXTSNAP_DB", "storage", "database_path"},
        {"CONTEXTSNAP_SOCKET", "ipc", "socket_path"},
        {"CONTEXTSNAP_LOG_LEVEL", "", "log_level"},
        {"CONTEXTSNAP_LOG_FILE", "", "log_file"},
        {"CONTEXTSNAP_AUTO_CAPTURE", "capture", "auto_capture"},
        {"CONTEXTSNAP_SANITIZE_URLS", "privacy", "sanitize_urls"},
        {"CONTEXTSNAP_ENCRYPT", "storage", "encrypt"},
    };
    for (const auto& override_entry : kOverrides) {
        const std::string value = env(override_entry.name);
        if (!value.empty()) {
            apply(config, override_entry.section, override_entry.key, value);
        }
    }
}

}  // namespace

namespace paths {

std::string config_dir() {
#if defined(_WIN32)
    const std::string appdata = env("APPDATA");
    return join(appdata.empty() ? home_dir() : appdata, "ContextSnap");
#elif defined(__APPLE__)
    return join(join(home_dir(), "Library/Application Support"), "ContextSnap");
#else
    const std::string xdg = env("XDG_CONFIG_HOME");
    return join(xdg.empty() ? join(home_dir(), ".config") : xdg, "contextsnap");
#endif
}

std::string data_dir() {
#if defined(_WIN32)
    const std::string local = env("LOCALAPPDATA");
    return join(local.empty() ? home_dir() : local, "ContextSnap");
#elif defined(__APPLE__)
    return join(join(home_dir(), "Library/Application Support"), "ContextSnap");
#else
    const std::string xdg = env("XDG_DATA_HOME");
    return join(xdg.empty() ? join(home_dir(), ".local/share") : xdg, "contextsnap");
#endif
}

std::string cache_dir() {
#if defined(_WIN32)
    return join(data_dir(), "cache");
#elif defined(__APPLE__)
    return join(join(home_dir(), "Library/Caches"), "ContextSnap");
#else
    const std::string xdg = env("XDG_CACHE_HOME");
    return join(xdg.empty() ? join(home_dir(), ".cache") : xdg, "contextsnap");
#endif
}

std::string runtime_dir() {
#if defined(_WIN32)
    return data_dir();
#else
    const std::string xdg = env("XDG_RUNTIME_DIR");
    return xdg.empty() ? data_dir() : join(xdg, "contextsnap");
#endif
}

std::string default_database_path() { return join(data_dir(), "snapshots.db"); }

std::string default_socket_path() {
#if defined(_WIN32)
    return std::string(kDefaultPipeName);
#else
    return join(runtime_dir(), std::string(kDefaultSocketName));
#endif
}

std::string default_log_path() { return join(data_dir(), "contextsnapd.log"); }

std::string favicon_cache_dir() { return join(cache_dir(), "favicons"); }

Status ensure_directory(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec && !fs::exists(path)) {
        return err::io("cannot create directory: " + path, "config.paths", ec.value());
    }
    return Status::success();
}

}  // namespace paths

Config Config::with_defaults() {
    Config config;
    config.storage.database_path = paths::default_database_path();
    config.ipc.socket_path = paths::default_socket_path();
    config.log_file = paths::default_log_path();
    return config;
}

Result<Config> Config::load(const std::string& explicit_path) {
    Config config = Config::with_defaults();
    const std::string path =
        explicit_path.empty() ? join(paths::config_dir(), "config.toml") : explicit_path;

    std::ifstream file(path);
    if (file.is_open()) {
        std::string section;
        std::string line;
        while (std::getline(file, line)) {
            const std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                section = trimmed.substr(1, trimmed.size() - 2);
                continue;
            }
            const std::size_t equals = trimmed.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            apply(config, section, trim(trimmed.substr(0, equals)), trim(trimmed.substr(equals + 1)));
        }
    } else if (!explicit_path.empty()) {
        return err::not_found("config file not found: " + explicit_path, "config.load");
    }

    apply_environment(config);
    const Status valid = config.validate();
    if (!valid) {
        return valid.error();
    }
    return config;
}

Status Config::save(const std::string& path) const {
    const Status dir = paths::ensure_directory(fs::path(path).parent_path().string());
    if (!dir) {
        return dir;
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return err::io("cannot write config: " + path, "config.save");
    }

    const auto array = [](const std::vector<std::string>& values) {
        std::string out = "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            out += (i == 0 ? "\"" : ", \"") + values[i] + "\"";
        }
        return out + "]";
    };

    file << "# ContextSnap configuration (" << kProjectName << ' ' << kVersion << ")\n"
         << "log_level = \"" << log::to_string(log_level) << "\"\n"
         << "log_file = \"" << log_file << "\"\n\n"
         << "[storage]\n"
         << "database_path = \"" << storage.database_path << "\"\n"
         << "encrypt = " << (storage.encrypt ? "true" : "false") << "\n"
         << "retention_days = " << storage.retention_days << "\n"
         << "max_snapshots = " << storage.max_snapshots << "\n"
         << "vacuum_interval_days = " << storage.vacuum_interval_days << "\n"
         << "store_favicons = " << (storage.store_favicons ? "true" : "false") << "\n"
         << "favicon_cache_mb = " << storage.favicon_cache_mb << "\n\n"
         << "[capture]\n"
         << "auto_capture = " << (capture.auto_capture ? "true" : "false") << "\n"
         << "auto_capture_interval_minutes = " << capture.auto_capture_interval_minutes << "\n"
         << "capture_on_idle = " << (capture.capture_on_idle ? "true" : "false") << "\n"
         << "idle_threshold_minutes = " << capture.idle_threshold_minutes << "\n"
         << "capture_before_shutdown = " << (capture.capture_before_shutdown ? "true" : "false")
         << "\n"
         << "include_minimized = " << (capture.include_minimized ? "true" : "false") << "\n"
         << "include_incognito = " << (capture.include_incognito ? "true" : "false") << "\n"
         << "excluded_app_ids = " << array(capture.excluded_app_ids) << "\n"
         << "excluded_url_patterns = " << array(capture.excluded_url_patterns) << "\n"
         << "browser_timeout_ms = " << capture.browser_timeout_ms << "\n\n"
         << "[restore]\n"
         << "launch_missing_apps = " << (restore.launch_missing_apps ? "true" : "false") << "\n"
         << "lazy_load_tabs = " << (restore.lazy_load_tabs ? "true" : "false") << "\n"
         << "restore_cursor = " << (restore.restore_cursor ? "true" : "false") << "\n"
         << "restore_z_order = " << (restore.restore_z_order ? "true" : "false") << "\n"
         << "app_launch_timeout_ms = " << restore.app_launch_timeout_ms << "\n"
         << "window_settle_timeout_ms = " << restore.window_settle_timeout_ms << "\n"
         << "max_parallel_launches = " << restore.max_parallel_launches << "\n\n"
         << "[privacy]\n"
         << "sanitize_urls = " << (privacy.sanitize_urls ? "true" : "false") << "\n"
         << "strip_query_strings = " << (privacy.strip_query_strings ? "true" : "false") << "\n"
         << "hash_excluded_urls = " << (privacy.hash_excluded_urls ? "true" : "false") << "\n"
         << "extra_sensitive_params = " << array(privacy.extra_sensitive_params) << "\n"
         << "domain_denylist = " << array(privacy.domain_denylist) << "\n"
         << "domain_allowlist = " << array(privacy.domain_allowlist) << "\n\n"
         << "[hotkeys]\n"
         << "enabled = " << (hotkeys.enabled ? "true" : "false") << "\n"
         << "quick_switcher = \"" << hotkeys.quick_switcher << "\"\n"
         << "save_snapshot = \"" << hotkeys.save_snapshot << "\"\n"
         << "restore_last = \"" << hotkeys.restore_last << "\"\n\n"
         << "[ipc]\n"
         << "socket_path = \"" << ipc.socket_path << "\"\n"
         << "max_frame_bytes = " << ipc.max_frame_bytes << "\n"
         << "request_timeout_ms = " << ipc.request_timeout_ms << "\n"
         << "max_clients = " << ipc.max_clients << "\n";
    return Status::success();
}

CaptureOptions Config::capture_options() const {
    CaptureOptions options;
    options.include_minimized = capture.include_minimized;
    options.include_incognito = capture.include_incognito;
    options.sanitize_urls = privacy.sanitize_urls;
    options.browser_timeout = Milliseconds{static_cast<std::int64_t>(capture.browser_timeout_ms)};
    options.excluded_app_ids = capture.excluded_app_ids;
    options.excluded_url_patterns = capture.excluded_url_patterns;
    return options;
}

RestoreOptions Config::restore_options() const {
    RestoreOptions options;
    options.launch_missing_apps = restore.launch_missing_apps;
    options.lazy_load_tabs = restore.lazy_load_tabs;
    options.restore_cursor = restore.restore_cursor;
    options.restore_z_order = restore.restore_z_order;
    options.app_launch_timeout = Milliseconds{static_cast<std::int64_t>(restore.app_launch_timeout_ms)};
    options.window_settle_timeout =
        Milliseconds{static_cast<std::int64_t>(restore.window_settle_timeout_ms)};
    options.max_parallel_launches = std::max(1u, restore.max_parallel_launches);
    return options;
}

Status Config::validate() const {
    if (storage.database_path.empty()) {
        return err::invalid("storage.database_path must not be empty", "config.validate");
    }
#if !defined(CONTEXTSNAP_WITH_SQLCIPHER)
    if (storage.encrypt) {
        return err::unsupported(
            "storage.encrypt requires a build with -DCONTEXTSNAP_WITH_SQLCIPHER=ON",
            "config.validate");
    }
#endif
    if (ipc.max_frame_bytes < 64u * 1024u) {
        return err::invalid("ipc.max_frame_bytes must be at least 65536", "config.validate");
    }
    if (capture.auto_capture && capture.auto_capture_interval_minutes == 0) {
        return err::invalid("capture.auto_capture_interval_minutes must be > 0 when auto capture is on",
                            "config.validate");
    }
    if (!privacy.domain_allowlist.empty() && !privacy.domain_denylist.empty()) {
        return err::invalid("privacy.domain_allowlist and domain_denylist are mutually exclusive",
                            "config.validate");
    }
    return Status::success();
}

}  // namespace contextsnap::core
