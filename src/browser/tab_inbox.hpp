// Internal (non-installed) header shared by the browser bridge, the IPC
// service handlers, and the native messaging host.
//
// The native host is a short-lived process started by the browser. It connects
// to the daemon over IPC and *pushes* tab payloads here; the bridge then hands
// them to whichever capture is currently waiting.
#pragma once

#include <contextsnap/browser/bridge.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace contextsnap::browser::inbox {

/// A restore request waiting to be delivered to a connected extension.
struct PendingRestore {
    std::string id;
    std::vector<BrowserWindow> windows;
    bool lazy_load{true};
};

/// Stores tabs pushed by a native host connection and wakes any waiter.
void publish_tabs(const std::string& connection_id, std::vector<BrowserWindow> windows);

/// Blocks until fresh tabs arrive or the deadline elapses.
/// Returns an empty vector on timeout.
[[nodiscard]] std::vector<BrowserWindow> take_tabs(std::chrono::milliseconds timeout);

/// Requests a browser-side restore; returns the queued request id.
std::string enqueue_restore(std::vector<BrowserWindow> windows, bool lazy_load);

/// Pops the next queued restore request for a connected host, if any.
[[nodiscard]] bool next_restore(PendingRestore& out);

/// Marks a queued restore as acknowledged by the extension.
void acknowledge_restore(const std::string& request_id, std::uint32_t tabs_restored);

/// Number of tabs the extension reported for `request_id` (0 when unknown).
[[nodiscard]] std::uint32_t restore_result(const std::string& request_id,
                                           std::chrono::milliseconds timeout);

/// Asks every connected host to send a fresh tab snapshot.
void request_collection();

/// True when a collection request is outstanding (polled by the host loop).
[[nodiscard]] bool consume_collection_request();

}  // namespace contextsnap::browser::inbox
