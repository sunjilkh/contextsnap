// The daemon's RPC server: accept loop, per-connection worker threads, request
// dispatch and the event fan-out used by `Subscribe`.
#pragma once

#include <contextsnap/core/result.hpp>
#include <contextsnap/ipc/protocol.hpp>
#include <contextsnap/ipc/transport.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace contextsnap::ipc {

/// Handler signature: params in, result out. Throwing is a bug; return an Error.
using Handler = std::function<Result<core::json::Value>(const core::json::Value& params,
                                                        const PeerIdentity& peer)>;

struct ServerOptions {
    std::string endpoint;
    std::uint32_t max_clients{16};
    std::uint32_t max_frame_bytes{kMaxFrameBytes};
    std::chrono::milliseconds idle_timeout{std::chrono::minutes{30}};
    bool require_same_user{true};
};

class Server {
public:
    explicit Server(ServerOptions options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Registers the handler for `method`. Replaces any previous handler.
    void on(Method method, Handler handler);

    /// Binds the endpoint and starts the accept loop on a background thread.
    [[nodiscard]] Status start();

    /// Stops accepting, closes live connections and joins every worker.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /// Broadcasts an event to every subscribed connection. Never blocks the
    /// caller: slow clients are dropped after their queue exceeds 256 events.
    void broadcast(const Event& event);

    [[nodiscard]] std::size_t client_count() const;
    [[nodiscard]] const std::string& endpoint() const noexcept { return options_.endpoint; }

private:
    struct ClientSession;

    void accept_loop();
    void serve_client(std::shared_ptr<ClientSession> session);
    [[nodiscard]] Response dispatch(const Request& request, const PeerIdentity& peer);

    ServerOptions options_;
    std::unique_ptr<Listener> listener_;
    std::unordered_map<Method, Handler> handlers_;
    mutable std::mutex handlers_mutex_;

    std::thread accept_thread_;
    std::vector<std::thread> worker_threads_;
    std::vector<std::shared_ptr<ClientSession>> sessions_;
    mutable std::mutex sessions_mutex_;

    std::atomic<bool> running_{false};
};

}  // namespace contextsnap::ipc
