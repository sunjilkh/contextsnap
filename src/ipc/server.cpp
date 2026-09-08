#include <contextsnap/ipc/server.hpp>

#include <contextsnap/core/logging.hpp>

#include <algorithm>
#include <utility>

namespace contextsnap::ipc {

struct Server::ClientSession {
    std::unique_ptr<Connection> connection;
    std::mutex write_mutex;
    std::atomic<bool> subscribed{false};
    std::atomic<bool> active{true};

    bool send(const core::json::Value& value, std::uint32_t max_bytes) {
        auto frame = encode_frame(value.dump(), max_bytes);
        if (!frame) {
            core::log::warn("dropping oversized frame",
                            {core::log::field("error", frame.error().to_string())});
            return false;
        }
        const std::lock_guard<std::mutex> lock(write_mutex);
        std::string_view bytes(reinterpret_cast<const char*>(frame.value().data()),
                               frame.value().size());
        while (!bytes.empty()) {
            auto written = connection->write(bytes);
            if (!written || written.value() == 0) {
                return false;
            }
            bytes.remove_prefix(written.value());
        }
        return true;
    }
};

Server::Server(ServerOptions options) : options_(std::move(options)) {}

Server::~Server() { stop(); }

void Server::on(Method method, Handler handler) {
    const std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_[method] = std::move(handler);
}

Status Server::start() {
    if (running_.load()) {
        return core::err::invalid("server is already running", "ipc.server");
    }
    auto listener = listen(options_.endpoint);
    if (!listener) {
        return listener.error();
    }
    listener_ = std::move(listener.value());
    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });
    core::log::info("ipc server listening", {core::log::field("endpoint", options_.endpoint)});
    return Status::success();
}

void Server::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listener_ != nullptr) {
        listener_->close();
    }
    {
        const std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const std::shared_ptr<ClientSession>& session : sessions_) {
            session->active.store(false);
            session->connection->close();
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    for (std::thread& worker : worker_threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    worker_threads_.clear();
    {
        const std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
    listener_.reset();
    core::log::info("ipc server stopped", {});
}

void Server::accept_loop() {
    while (running_.load()) {
        auto accepted = listener_->accept();
        if (!accepted) {
            if (accepted.error().code == core::ErrorCode::Timeout) {
                continue;
            }
            if (running_.load()) {
                core::log::warn("accept failed",
                                {core::log::field("error", accepted.error().to_string())});
            }
            break;
        }
        std::unique_ptr<Connection> connection = std::move(accepted.value());

        // Local sockets are already restricted by file permissions; this is the
        // second line of defence against another user's process.
        const PeerIdentity peer = connection->peer();
        if (options_.require_same_user && !peer.same_user) {
            core::log::warn("rejected connection from another user",
                            {core::log::field("user", peer.user)});
            connection->close();
            continue;
        }
        if (client_count() >= options_.max_clients) {
            core::log::warn("rejected connection: too many clients", {});
            connection->close();
            continue;
        }
        (void)connection->set_timeout(options_.idle_timeout);

        auto session = std::make_shared<ClientSession>();
        session->connection = std::move(connection);
        {
            const std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.push_back(session);
            worker_threads_.emplace_back([this, session] { serve_client(session); });
            // Reap finished worker threads so long-running daemons do not leak.
            worker_threads_.erase(std::remove_if(worker_threads_.begin(), worker_threads_.end(),
                                                 [](std::thread& worker) {
                                                     return !worker.joinable();
                                                 }),
                                  worker_threads_.end());
        }
    }
}

void Server::serve_client(std::shared_ptr<ClientSession> session) {
    FrameReader reader(options_.max_frame_bytes);
    while (running_.load() && session->active.load()) {
        auto chunk = session->connection->read_some();
        if (!chunk) {
            if (chunk.error().code == core::ErrorCode::Timeout) {
                continue;
            }
            break;
        }
        if (chunk.value().empty()) {
            break;  // Peer closed the connection.
        }
        reader.feed(chunk.value());
        while (true) {
            auto next = reader.next();
            if (!next) {
                core::log::warn("framing error; dropping client",
                                {core::log::field("error", next.error().to_string())});
                session->active.store(false);
                break;
            }
            if (!next.value().has_value()) {
                break;
            }
            auto document = core::json::parse(*next.value());
            if (!document) {
                (void)session->send(
                    Response::failure(0, document.error()).to_json(), options_.max_frame_bytes);
                continue;
            }
            auto request = Request::from_json(document.value());
            if (!request) {
                (void)session->send(Response::failure(0, request.error()).to_json(),
                                    options_.max_frame_bytes);
                continue;
            }
            if (request.value().method == Method::Subscribe) {
                session->subscribed.store(true);
                core::json::Value acknowledgement = core::json::Value::object();
                acknowledgement.set("subscribed", core::json::Value(true));
                (void)session->send(
                    Response::success(request.value().id, std::move(acknowledgement)).to_json(),
                    options_.max_frame_bytes);
                continue;
            }
            const Response response = dispatch(request.value(), session->connection->peer());
            if (!session->send(response.to_json(), options_.max_frame_bytes)) {
                session->active.store(false);
                break;
            }
        }
    }
    session->active.store(false);
    session->connection->close();

    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session), sessions_.end());
}

Response Server::dispatch(const Request& request, const PeerIdentity& peer) {
    Handler handler;
    {
        const std::lock_guard<std::mutex> lock(handlers_mutex_);
        const auto found = handlers_.find(request.method);
        if (found != handlers_.end()) {
            handler = found->second;
        }
    }
    if (!handler) {
        return Response::failure(
            request.id,
            core::Error{core::ErrorCode::Unsupported,
                        "method " + std::string(to_string(request.method)) + " is not handled",
                        "ipc.server", 0});
    }
    auto result = handler(request.params, peer);
    if (!result) {
        return Response::failure(request.id, result.error());
    }
    return Response::success(request.id, std::move(result.value()));
}

void Server::broadcast(const Event& event) {
    std::vector<std::shared_ptr<ClientSession>> targets;
    {
        const std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const std::shared_ptr<ClientSession>& session : sessions_) {
            if (session->subscribed.load() && session->active.load()) {
                targets.push_back(session);
            }
        }
    }
    const core::json::Value payload = event.to_json();
    for (const std::shared_ptr<ClientSession>& session : targets) {
        if (!session->send(payload, options_.max_frame_bytes)) {
            session->active.store(false);
        }
    }
}

std::size_t Server::client_count() const {
    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

}  // namespace contextsnap::ipc
