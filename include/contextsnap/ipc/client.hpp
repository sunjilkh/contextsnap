// Synchronous RPC client shared by the CLI, the Qt GUI and the native host.
#pragma once

#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>
#include <contextsnap/ipc/protocol.hpp>
#include <contextsnap/ipc/transport.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace contextsnap::ipc {

class Client {
public:
    explicit Client(std::string endpoint = {});
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Connects and performs the `hello` handshake (version negotiation).
    [[nodiscard]] Status connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});
    void disconnect();
    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] Result<core::json::Value> call(Method method,
                                                 core::json::Value params = {},
                                                 std::chrono::milliseconds timeout =
                                                     std::chrono::milliseconds{15000});

    // Typed conveniences over call().
    [[nodiscard]] Result<core::Snapshot> capture(const core::CaptureOptions& options);
    [[nodiscard]] Result<std::vector<core::SnapshotSummary>> list(std::size_t limit,
                                                                  std::size_t offset,
                                                                  const std::string& tag = {});
    [[nodiscard]] Result<core::Snapshot> get(const std::string& snapshot_id);
    [[nodiscard]] Result<core::RestorePlan> plan_restore(const std::string& snapshot_id,
                                                         const core::RestoreOptions& options);
    [[nodiscard]] Result<core::RestoreReport> restore(const std::string& snapshot_id,
                                                      const core::RestoreOptions& options);
    [[nodiscard]] Status remove(const std::string& snapshot_id);
    [[nodiscard]] Result<core::json::Value> health();

    /// Subscribes to daemon events; `callback` runs on a background thread
    /// until unsubscribe() or disconnect().
    using EventCallback = std::function<void(const Event&)>;
    [[nodiscard]] Status subscribe(EventCallback callback);
    void unsubscribe();

    [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }

    [[nodiscard]] std::uint32_t server_protocol_version() const noexcept { return server_version_; }

private:
    [[nodiscard]] Result<Response> round_trip(const Request& request,
                                              std::chrono::milliseconds timeout);

    std::string endpoint_;
    std::unique_ptr<Connection> connection_;
    FrameReader reader_;
    std::atomic<std::uint64_t> next_request_id_{1};
    std::uint32_t server_version_{0};

    struct Subscription;
    std::unique_ptr<Subscription> subscription_;
};

/// Starts contextsnapd if it is not already listening, then returns a connected
/// client. Used by the CLI so `contextsnap save` works from a cold start.
[[nodiscard]] Result<std::unique_ptr<Client>> connect_or_spawn_daemon(
    const std::string& endpoint,
    const std::string& daemon_executable = {});

}  // namespace contextsnap::ipc
