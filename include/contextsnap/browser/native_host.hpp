// contextsnap-native-host: the process browsers spawn for native messaging.
//
// It is deliberately tiny — it owns no state and makes no OS calls beyond stdio
// and one IPC socket. Browsers execute it with the extension's privileges, so
// keeping it a dumb relay minimises the attack surface.
//
//   browser  <-- stdio frames -->  native host  <-- unix socket -->  contextsnapd
//
// Framing on both sides is identical: 4-byte little-endian length + UTF-8 JSON.
// The browser side additionally caps messages at 64 MB (Chrome) / 1 MB per
// message from the extension.
#pragma once

#include <contextsnap/browser/bridge.hpp>
#include <contextsnap/core/json.hpp>
#include <contextsnap/core/result.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace contextsnap::browser {

using core::Result;
using core::Status;

inline constexpr std::uint32_t kNativeMessagingVersion = 1;
inline constexpr std::uint32_t kMaxNativeMessageBytes = 64u * 1024u * 1024u;

/// Messages the extension sends to the host.
struct ExtensionMessage {
    std::string type;  ///< hello | tabs | restoreResult | error | pong
    core::json::Value payload;

    [[nodiscard]] static Result<ExtensionMessage> from_json(const core::json::Value& value);
};

/// Messages the host sends to the extension.
struct HostMessage {
    std::string type;  ///< welcome | captureTabs | restoreTabs | ping | error
    core::json::Value payload;

    [[nodiscard]] core::json::Value to_json() const;
};

/// Parses the extension's `tabs` payload into the internal model.
[[nodiscard]] Result<std::vector<BrowserWindow>> parse_tabs_message(
    const core::json::Value& payload);

/// Builds the `restoreTabs` payload for the extension.
[[nodiscard]] core::json::Value build_restore_message(const std::vector<BrowserWindow>& windows,
                                                      bool lazy_load);

struct NativeHostOptions {
    std::string daemon_endpoint;
    std::chrono::milliseconds daemon_timeout{3000};
    std::uint32_t max_message_bytes{kMaxNativeMessageBytes};
    bool verbose{false};
};

/// Runs the relay loop until stdin closes. Returns a process exit code.
[[nodiscard]] int run_native_host(const NativeHostOptions& options);

/// Generates the host manifest JSON for a browser family. `allowed_origins`
/// carries chrome-extension:// ids, `allowed_extensions` the Firefox ids.
[[nodiscard]] std::string build_host_manifest(const std::string& host_path,
                                              const std::vector<std::string>& allowed_origins,
                                              const std::vector<std::string>& allowed_extensions);

}  // namespace contextsnap::browser
