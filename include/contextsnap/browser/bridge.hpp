// Browser integration front door.
//
// Two acquisition strategies, tried in order:
//   1. ACTIVE  — the WebExtension answers a `captureTabs` request over the
//                native messaging host. Complete, live, includes tab groups,
//                pinned/audible state and containers.
//   2. PASSIVE — no extension installed (or it did not answer within the
//                timeout): parse the browser's own session-restore files.
//                Read-only, slightly stale, no restore capability.
//
// The bridge owns the mapping between OS windows and browser windows, which is
// the genuinely hard part: extensions see `windowId`, the HAL sees an HWND.
// Correlation strategy is documented in docs/browser-integration.md#correlation.
#pragma once

#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace contextsnap::browser {

using core::BrowserKind;
using core::Result;
using core::Status;
using core::TabInfo;

/// One browser window as reported by an extension or a session file.
struct BrowserWindow {
    std::string extension_window_id;
    BrowserKind browser{BrowserKind::None};
    std::string profile;
    core::Rect frame;         ///< Extension-reported geometry, used for correlation.
    bool focused{false};
    bool incognito{false};
    std::string state;        ///< normal | minimized | maximized | fullscreen
    std::vector<TabInfo> tabs;
};

enum class Source : std::uint8_t { Extension, SessionFile, None };

struct TabCapture {
    std::vector<BrowserWindow> windows;
    Source source{Source::None};
    std::vector<std::string> warnings;
    std::uint32_t duration_ms{0};
};

/// Correlates extension windows with OS windows and copies tabs into them.
/// Returns the number of OS windows that received tabs.
[[nodiscard]] std::size_t attach_tabs_to_windows(const TabCapture& capture,
                                                 std::vector<core::WindowInfo>& windows);

class Bridge {
public:
    virtual ~Bridge() = default;

    /// True when at least one extension has an open native messaging port.
    [[nodiscard]] virtual bool extension_connected() const = 0;

    /// Requests tabs from every connected browser, falling back to session file
    /// parsing when `allow_session_fallback` is set.
    [[nodiscard]] virtual Result<TabCapture> capture_tabs(std::chrono::milliseconds timeout,
                                                          bool allow_session_fallback = true) = 0;

    /// Asks the extension to recreate `windows`. Requires an active extension;
    /// returns Unsupported when only the passive path is available.
    [[nodiscard]] virtual Result<std::uint32_t> restore_tabs(
        const std::vector<BrowserWindow>& windows,
        bool lazy_load) = 0;

    /// Registers a host connection created by contextsnap-native-host.
    virtual void register_host_connection(const std::string& connection_id,
                                          BrowserKind browser,
                                          const std::string& profile) = 0;
    virtual void unregister_host_connection(const std::string& connection_id) = 0;

    [[nodiscard]] virtual std::vector<BrowserKind> connected_browsers() const = 0;

    /// Default implementation backed by the native messaging host plus the
    /// passive session parsers.
    [[nodiscard]] static std::shared_ptr<Bridge> create();

    /// No-op bridge for headless tests and for `--no-tabs` captures.
    [[nodiscard]] static std::shared_ptr<Bridge> create_null();
};

}  // namespace contextsnap::browser
