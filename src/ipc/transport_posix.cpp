// POSIX transport: Unix domain sockets with peer credential checks.
#if !defined(_WIN32)

#include <contextsnap/ipc/transport.hpp>

#include <contextsnap/core/logging.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

namespace contextsnap::ipc {
namespace {

core::Error errno_error(const std::string& what) {
    core::Error error;
    error.code = errno == EACCES || errno == EPERM ? core::ErrorCode::PermissionDenied
                                                   : core::ErrorCode::IoError;
    error.message = what + ": " + std::strerror(errno);
    error.context = "ipc.posix";
    error.native_code = errno;
    return error;
}

Status set_socket_timeout(int fd, std::chrono::milliseconds timeout) {
    timeval value{};
    value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0) {
        return errno_error("cannot set socket timeout");
    }
    return Status::success();
}

PeerIdentity read_peer(int fd) {
    PeerIdentity peer;
#if defined(SO_PEERCRED) && !defined(__APPLE__)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0) {
        peer.pid = static_cast<std::uint64_t>(credentials.pid);
        peer.user = std::to_string(credentials.uid);
        peer.same_user = credentials.uid == ::getuid();
    }
#elif defined(LOCAL_PEERCRED)
    xucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &length) == 0 &&
        credentials.cr_version == XUCRED_VERSION) {
        peer.user = std::to_string(credentials.cr_uid);
        peer.same_user = credentials.cr_uid == ::getuid();
    }
    pid_t remote_pid = 0;
    socklen_t pid_length = sizeof(remote_pid);
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &remote_pid, &pid_length) == 0) {
        peer.pid = static_cast<std::uint64_t>(remote_pid);
    }
#endif
    return peer;
}

class PosixConnection final : public Connection {
public:
    PosixConnection(int fd, bool owns_fd, PeerIdentity peer)
        : fd_(fd), owns_fd_(owns_fd), peer_(std::move(peer)) {}

    ~PosixConnection() override { close(); }

    Result<std::size_t> write(std::string_view bytes) override {
        if (fd_ < 0) {
            return core::err::io("connection is closed", "ipc.posix");
        }
        while (true) {
            const ssize_t written = ::send(fd_, bytes.data(), bytes.size(), kNoSignal);
            if (written >= 0) {
                return static_cast<std::size_t>(written);
            }
            if (errno == EINTR) {
                continue;
            }
            return errno_error("write failed");
        }
    }

    Result<std::string> read_some(std::size_t max_bytes) override {
        if (fd_ < 0) {
            return core::err::io("connection is closed", "ipc.posix");
        }
        std::string buffer;
        buffer.resize(max_bytes);
        while (true) {
            const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (received > 0) {
                buffer.resize(static_cast<std::size_t>(received));
                return buffer;
            }
            if (received == 0) {
                return std::string{};  // Orderly shutdown.
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return core::Error{core::ErrorCode::Timeout, "read timed out", "ipc.posix", errno};
            }
            return errno_error("read failed");
        }
    }

    void close() override {
        if (fd_ >= 0 && owns_fd_) {
            ::close(fd_);
        }
        fd_ = -1;
    }

    [[nodiscard]] bool is_open() const noexcept override { return fd_ >= 0; }
    [[nodiscard]] PeerIdentity peer() const override { return peer_; }

    Status set_timeout(std::chrono::milliseconds timeout) override {
        if (fd_ < 0) {
            return core::err::io("connection is closed", "ipc.posix");
        }
        return set_socket_timeout(fd_, timeout);
    }

private:
#if defined(MSG_NOSIGNAL)
    static constexpr int kNoSignal = MSG_NOSIGNAL;
#else
    static constexpr int kNoSignal = 0;  // macOS uses SO_NOSIGPIPE instead.
#endif

    int fd_{-1};
    bool owns_fd_{true};
    PeerIdentity peer_;
};

/// Reads from stdin and writes to stdout as one duplex "connection".
class StdioConnection final : public Connection {
public:
    Result<std::size_t> write(std::string_view bytes) override {
        std::size_t total = 0;
        while (total < bytes.size()) {
            const ssize_t written =
                ::write(STDOUT_FILENO, bytes.data() + total, bytes.size() - total);
            if (written < 0) {
                if (errno == EINTR) continue;
                return errno_error("stdout write failed");
            }
            total += static_cast<std::size_t>(written);
        }
        return total;
    }

    Result<std::string> read_some(std::size_t max_bytes) override {
        std::string buffer;
        buffer.resize(max_bytes);
        while (true) {
            const ssize_t received = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (received > 0) {
                buffer.resize(static_cast<std::size_t>(received));
                return buffer;
            }
            if (received == 0) {
                open_ = false;
                return std::string{};
            }
            if (errno == EINTR) continue;
            return errno_error("stdin read failed");
        }
    }

    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] PeerIdentity peer() const override {
        PeerIdentity peer;
        peer.pid = static_cast<std::uint64_t>(::getppid());
        peer.user = std::to_string(::getuid());
        peer.same_user = true;
        return peer;
    }

    Status set_timeout(std::chrono::milliseconds) override { return Status::success(); }

private:
    bool open_{true};
};

class PosixListener final : public Listener {
public:
    PosixListener(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~PosixListener() override { close(); }

    Result<std::unique_ptr<Connection>> accept() override {
        if (fd_ < 0) {
            return core::err::io("listener is closed", "ipc.posix");
        }
        while (true) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client >= 0) {
                return std::unique_ptr<Connection>(
                    new PosixConnection(client, true, read_peer(client)));
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return core::Error{core::ErrorCode::Timeout, "accept timed out", "ipc.posix",
                                   errno};
            }
            return errno_error("accept failed");
        }
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    [[nodiscard]] std::string endpoint() const override { return path_; }

private:
    int fd_{-1};
    std::string path_;
};

bool fill_address(sockaddr_un& address, const std::string& path, std::string& error) {
    if (path.size() >= sizeof(address.sun_path)) {
        error = "socket path is too long (" + std::to_string(path.size()) + " bytes)";
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

Result<std::unique_ptr<Listener>> listen(const std::string& endpoint) {
    sockaddr_un address{};
    std::string error;
    if (!fill_address(address, endpoint, error)) {
        return core::err::invalid(error, "ipc.posix");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(endpoint).parent_path(), ec);
    // A leftover socket from a crashed daemon would block bind().
    if (!daemon_running(endpoint)) {
        std::filesystem::remove(endpoint, ec);
    } else {
        return core::Error{core::ErrorCode::AlreadyExists, "another daemon owns " + endpoint,
                          "ipc.posix", 0};
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return errno_error("cannot create socket");
    }
    // 0700 on the directory and 0600 on the socket keep other users out.
    const mode_t previous = ::umask(0077);
    const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::umask(previous);
    if (bound != 0) {
        const core::Error failure = errno_error("cannot bind " + endpoint);
        ::close(fd);
        return failure;
    }
    if (::listen(fd, 16) != 0) {
        const core::Error failure = errno_error("cannot listen on " + endpoint);
        ::close(fd);
        return failure;
    }
    ::chmod(endpoint.c_str(), S_IRUSR | S_IWUSR);
    return std::unique_ptr<Listener>(new PosixListener(fd, endpoint));
}

Result<std::unique_ptr<Connection>> connect(const std::string& endpoint,
                                            std::chrono::milliseconds timeout) {
    sockaddr_un address{};
    std::string error;
    if (!fill_address(address, endpoint, error)) {
        return core::err::invalid(error, "ipc.posix");
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return errno_error("cannot create socket");
    }
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const core::Error failure = errno_error("cannot connect to " + endpoint);
        ::close(fd);
        return failure;
    }
    if (const Status configured = set_socket_timeout(fd, timeout); !configured) {
        ::close(fd);
        return configured.error();
    }
    return std::unique_ptr<Connection>(new PosixConnection(fd, true, read_peer(fd)));
}

bool daemon_running(const std::string& endpoint) {
    std::error_code ec;
    if (!std::filesystem::exists(endpoint, ec)) {
        return false;
    }
    auto probe = connect(endpoint, std::chrono::milliseconds{250});
    return static_cast<bool>(probe);
}

std::unique_ptr<Connection> stdio_connection() {
    return std::unique_ptr<Connection>(new StdioConnection());
}

}  // namespace contextsnap::ipc

#endif  // !_WIN32
