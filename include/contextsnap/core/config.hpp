// Runtime configuration: a small TOML-subset file plus environment overrides.
//
// Precedence (highest first):
//   1. CLI flags
//   2. CONTEXTSNAP_* environment variables
//   3. $XDG_CONFIG_HOME/contextsnap/config.toml   (macOS: ~/Library/Application Support,
//      Windows: %APPDATA%\ContextSnap)
//   4. Compiled-in defaults below
#pragma once

#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace contextsnap::core {

struct StorageConfig {
    std::string database_path;         ///< Default: <data_dir>/snapshots.db
    bool encrypt{false};               ///< SQLCipher; requires the sqlcipher build option.
    std::uint32_t retention_days{90};  ///< 0 disables age-based pruning.
    std::uint32_t max_snapshots{500};  ///< 0 disables count-based pruning.
    std::uint32_t vacuum_interval_days{14};
    bool store_favicons{true};
    std::uint32_t favicon_cache_mb{32};
};

struct CaptureConfig {
    bool auto_capture{false};
    std::uint32_t auto_capture_interval_minutes{30};
    bool capture_on_idle{true};
    std::uint32_t idle_threshold_minutes{10};
    bool capture_before_shutdown{true};
    bool include_minimized{true};
    bool include_incognito{false};
    std::vector<std::string> excluded_app_ids;
    std::vector<std::string> excluded_url_patterns;
    std::uint32_t browser_timeout_ms{700};
};

struct RestoreConfig {
    bool launch_missing_apps{true};
    bool lazy_load_tabs{true};
    bool restore_cursor{true};
    bool restore_z_order{true};
    std::uint32_t app_launch_timeout_ms{8000};
    std::uint32_t window_settle_timeout_ms{2500};
    std::uint32_t max_parallel_launches{4};
};

struct PrivacyConfig {
    bool sanitize_urls{true};
    bool strip_query_strings{false};   ///< Aggressive mode: drop every query param.
    bool hash_excluded_urls{true};     ///< Keep a hash so diffs still work.
    std::vector<std::string> extra_sensitive_params;
    std::vector<std::string> domain_denylist;
    std::vector<std::string> domain_allowlist;  ///< Non-empty == deny everything else.
};

struct HotkeyConfig {
    std::string quick_switcher{"CommandOrControl+Shift+Space"};
    std::string save_snapshot{"CommandOrControl+Shift+S"};
    std::string restore_last{"CommandOrControl+Shift+R"};
    bool enabled{true};
};

struct IpcConfig {
    std::string socket_path;             ///< Unix socket / named pipe override.
    std::uint32_t max_frame_bytes{16u * 1024u * 1024u};
    std::uint32_t request_timeout_ms{5000};
    std::uint32_t max_clients{16};
};

struct Config {
    StorageConfig storage;
    CaptureConfig capture;
    RestoreConfig restore;
    PrivacyConfig privacy;
    HotkeyConfig hotkeys;
    IpcConfig ipc;
    log::Level log_level{log::Level::Info};
    std::string log_file;

    /// Fills every path field with platform-appropriate defaults.
    static Config with_defaults();

    /// Loads config.toml if present, then applies CONTEXTSNAP_* env overrides.
    static Result<Config> load(const std::string& explicit_path = {});

    [[nodiscard]] Status save(const std::string& path) const;

    [[nodiscard]] CaptureOptions capture_options() const;
    [[nodiscard]] RestoreOptions restore_options() const;

    /// Rejects nonsensical combinations (e.g. encrypt without SQLCipher support).
    [[nodiscard]] Status validate() const;
};

/// Platform directory helpers (XDG on Linux, Application Support on macOS,
/// %APPDATA%/%LOCALAPPDATA% on Windows).
namespace paths {

[[nodiscard]] std::string config_dir();
[[nodiscard]] std::string data_dir();
[[nodiscard]] std::string cache_dir();
[[nodiscard]] std::string runtime_dir();
[[nodiscard]] std::string default_database_path();
[[nodiscard]] std::string default_socket_path();
[[nodiscard]] std::string default_log_path();
[[nodiscard]] std::string favicon_cache_dir();
[[nodiscard]] Status ensure_directory(const std::string& path);

}  // namespace paths

}  // namespace contextsnap::core
