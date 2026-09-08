// Byte-stream transports for the daemon RPC.
//
//   POSIX   : Unix domain socket in $XDG_RUNTIME_DIR, mode 0600, peer credentials
//             verified with SO_PEERCRED / LOCAL_PEERCRED.
//   Windows : named pipe with a DACL restricted to the interactive user SID.
//
// Both refuse connections from a different uid/SID: the daemon can move windows
// and launch processes, so its control channel is a privilege boundary.
#pragma once

#include <contextsnap/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::ipc {

using core::Result;
using core::Status;

struct PeerIdentity {
    std::uint64_t pid{0};
    std::string user;      ///< uid as text (POSIX) or SID string (Windows).
    bool same_user{false};
};

/// A connected client seen from the server side, or the server seen from a
/// client. Implementations are blocking; the server runs one thread per
/// connection (max_clients is small by design).
class Connection {
public:
    virtual ~Connection() = default;

    [[nodiscard]] virtual Result<std::size_t> write(std::string_view bytes) = 0;

    /// Reads up to `max_bytes`; returns an empty string on clean EOF.
    [[nodiscard]] virtual Result<std::string> read_some(std::size_t max_bytes = 64 * 1024) = 0;

    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual PeerIdentity peer() const = 0;
    [[nodiscard]] virtual Status set_timeout(std::chrono::milliseconds timeout) = 0;
};

class Listener {
public:
    virtual ~Listener() = default;

    /// Blocks until a client connects or the listener is closed.
    [[nodiscard]] virtual Result<std::unique_ptr<Connection>> accept() = 0;

    virtual void close() = 0;
    [[nodiscard]] virtual std::string endpoint() const = 0;
};

/// Creates the platform listener at `endpoint` (socket path or pipe name),
/// removing a stale socket file if no live daemon owns it.
[[nodiscard]] Result<std::unique_ptr<Listener>> listen(const std::string& endpoint);

/// Connects to a running daemon.
[[nodiscard]] Result<std::unique_ptr<Connection>> connect(
    const std::string& endpoint,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

/// True when something is listening on `endpoint` right now.
[[nodiscard]] bool daemon_running(const std::string& endpoint);

/// stdin/stdout transport used by the browser native messaging host.
[[nodiscard]] std::unique_ptr<Connection> stdio_connection();

}  // namespace contextsnap::ipc
