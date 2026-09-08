#include <contextsnap/browser/bridge.hpp>

#include <contextsnap/browser/session_parsers.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/matching.hpp>

#include "tab_inbox.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace contextsnap::browser {
namespace inbox {
namespace {

struct State {
    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<BrowserWindow> tabs;
    bool tabs_fresh{false};
    bool collection_requested{false};
    std::deque<PendingRestore> restores;
    std::unordered_map<std::string, std::uint32_t> results;
    std::uint64_t counter{0};
};

State& state() {
    static State instance;
    return instance;
}

}  // namespace

void publish_tabs(const std::string& connection_id, std::vector<BrowserWindow> windows) {
    State& s = state();
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        // Windows from other connections (a second browser) are kept; only the
        // entries owned by this connection are replaced.
        s.tabs.erase(std::remove_if(s.tabs.begin(), s.tabs.end(),
                                    [&](const BrowserWindow& window) {
                                        return window.profile == connection_id;
                                    }),
                     s.tabs.end());
        for (BrowserWindow& window : windows) {
            s.tabs.push_back(std::move(window));
        }
        s.tabs_fresh = true;
    }
    s.arrived.notify_all();
}

std::vector<BrowserWindow> take_tabs(std::chrono::milliseconds timeout) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.collection_requested = true;
    if (!s.arrived.wait_for(lock, timeout, [&] { return s.tabs_fresh; })) {
        return {};
    }
    s.tabs_fresh = false;
    return s.tabs;
}

std::string enqueue_restore(std::vector<BrowserWindow> windows, bool lazy_load) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    PendingRestore pending;
    pending.id = "restore-" + std::to_string(++s.counter);
    pending.windows = std::move(windows);
    pending.lazy_load = lazy_load;
    const std::string id = pending.id;
    s.restores.push_back(std::move(pending));
    return id;
}

bool next_restore(PendingRestore& out) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.restores.empty()) {
        return false;
    }
    out = std::move(s.restores.front());
    s.restores.pop_front();
    return true;
}

void acknowledge_restore(const std::string& request_id, std::uint32_t tabs_restored) {
    State& s = state();
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        s.results[request_id] = tabs_restored;
    }
    s.arrived.notify_all();
}

std::uint32_t restore_result(const std::string& request_id, std::chrono::milliseconds timeout) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.arrived.wait_for(lock, timeout,
                            [&] { return s.results.count(request_id) != 0; })) {
        return 0;
    }
    const std::uint32_t value = s.results[request_id];
    s.results.erase(request_id);
    return value;
}

void request_collection() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.collection_requested = true;
}

bool consume_collection_request() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    const bool requested = s.collection_requested;
    s.collection_requested = false;
    return requested;
}

}  // namespace inbox

std::size_t attach_tabs_to_windows(const TabCapture& capture,
                                   std::vector<core::WindowInfo>& windows) {
    std::size_t attached = 0;
    std::vector<bool> consumed(capture.windows.size(), false);

    // Pass 1: browser windows whose geometry matches almost exactly. Chromium
    // and Firefox report their own outer bounds, so this is reliable when the
    // extension is installed.
    for (core::WindowInfo& window : windows) {
        if (window.browser == core::BrowserKind::None) {
            continue;
        }
        double best = 0.55;
        std::size_t best_index = capture.windows.size();
        for (std::size_t i = 0; i < capture.windows.size(); ++i) {
            if (consumed[i] || capture.windows[i].browser != window.browser) {
                continue;
            }
            const double score =
                0.75 * core::matching::geometry_proximity(window.frame, capture.windows[i].frame) +
                0.25 * (capture.windows[i].focused && window.focused ? 1.0 : 0.0);
            if (score > best) {
                best = score;
                best_index = i;
            }
        }
        if (best_index < capture.windows.size()) {
            consumed[best_index] = true;
            window.tabs = capture.windows[best_index].tabs;
            if (!capture.windows[best_index].profile.empty()) {
                window.browser_profile = capture.windows[best_index].profile;
            }
            ++attached;
        }
    }

    // Pass 2: anything left over is assigned by browser + order, which is what
    // the session-file fallback can offer.
    for (core::WindowInfo& window : windows) {
        if (window.browser == core::BrowserKind::None || !window.tabs.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < capture.windows.size(); ++i) {
            if (consumed[i] || capture.windows[i].browser != window.browser) {
                continue;
            }
            consumed[i] = true;
            window.tabs = capture.windows[i].tabs;
            if (!capture.windows[i].profile.empty()) {
                window.browser_profile = capture.windows[i].profile;
            }
            ++attached;
            break;
        }
    }
    return attached;
}

namespace {

struct Connection {
    core::BrowserKind browser{core::BrowserKind::None};
    std::string profile;
};

/// Bridge backed by the native messaging host, with session files as fallback.
class HostBridge final : public Bridge {
public:
    explicit HostBridge(bool allow_session_files) : allow_session_files_(allow_session_files) {}

    [[nodiscard]] bool extension_connected() const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        return !connections_.empty();
    }

    [[nodiscard]] Result<TabCapture> capture_tabs(std::chrono::milliseconds timeout,
                                                  bool allow_session_fallback) override {
        const auto started = std::chrono::steady_clock::now();
        TabCapture capture;
        if (extension_connected()) {
            capture.windows = inbox::take_tabs(timeout);
            if (!capture.windows.empty()) {
                capture.source = Source::Extension;
            } else {
                capture.warnings.emplace_back(
                    "extension did not answer within " + std::to_string(timeout.count()) + "ms");
            }
        }
        if (capture.windows.empty() && allow_session_fallback && allow_session_files_) {
            TabCapture fallback = session::capture_from_session_files();
            capture.windows = std::move(fallback.windows);
            capture.source = fallback.source;
            for (std::string& warning : fallback.warnings) {
                capture.warnings.push_back(std::move(warning));
            }
            if (!capture.windows.empty()) {
                capture.warnings.emplace_back(
                    "tabs came from on-disk session files; they can lag the live browser");
            }
        }
        capture.duration_ms = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        return capture;
    }

    [[nodiscard]] Result<std::uint32_t> restore_tabs(const std::vector<BrowserWindow>& windows,
                                                     bool lazy_load) override {
        if (!extension_connected()) {
            return core::err::unsupported(
                "no browser extension is connected; tabs cannot be reopened", "browser.bridge");
        }
        const std::string request = inbox::enqueue_restore(windows, lazy_load);
        const std::uint32_t restored =
            inbox::restore_result(request, std::chrono::milliseconds{15000});
        if (restored == 0) {
            return core::err::internal("the extension did not acknowledge the restore request",
                                       "browser.bridge");
        }
        return restored;
    }

    void register_host_connection(const std::string& connection_id, core::BrowserKind browser,
                                  const std::string& profile) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        connections_[connection_id] = Connection{browser, profile};
        core::log::info("native messaging host connected",
                        {core::log::field("browser", std::string(core::to_string(browser))),
                         core::log::field("profile", profile)});
    }

    void unregister_host_connection(const std::string& connection_id) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(connection_id);
        core::log::info("native messaging host disconnected",
                        {core::log::field("connection", connection_id)});
    }

    [[nodiscard]] std::vector<core::BrowserKind> connected_browsers() const override {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<core::BrowserKind> browsers;
        for (const auto& entry : connections_) {
            if (std::find(browsers.begin(), browsers.end(), entry.second.browser) ==
                browsers.end()) {
                browsers.push_back(entry.second.browser);
            }
        }
        return browsers;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Connection> connections_;
    bool allow_session_files_{true};
};

/// Bridge that never talks to a browser; used by tests and `--no-tabs`.
class NullBridge final : public Bridge {
public:
    [[nodiscard]] bool extension_connected() const override { return false; }

    [[nodiscard]] Result<TabCapture> capture_tabs(std::chrono::milliseconds,
                                                  bool) override {
        TabCapture capture;
        capture.source = Source::None;
        capture.warnings.emplace_back("browser integration is disabled");
        return capture;
    }

    [[nodiscard]] Result<std::uint32_t> restore_tabs(const std::vector<BrowserWindow>&,
                                                     bool) override {
        return core::err::unsupported("browser integration is disabled", "browser.null");
    }

    void register_host_connection(const std::string&, core::BrowserKind,
                                  const std::string&) override {
        core::log::debug("ignoring host connection: browser integration is disabled");
    }

    void unregister_host_connection(const std::string&) override {}

    [[nodiscard]] std::vector<core::BrowserKind> connected_browsers() const override { return {}; }
};

}  // namespace

std::shared_ptr<Bridge> Bridge::create() { return std::make_shared<HostBridge>(true); }

std::shared_ptr<Bridge> Bridge::create_null() { return std::make_shared<NullBridge>(); }

}  // namespace contextsnap::browser
