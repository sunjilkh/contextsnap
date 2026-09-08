// Windows transport: named pipes with a SID-based peer check.
#if defined(_WIN32)

#include <contextsnap/ipc/transport.hpp>

#include <windows.h>

#include <sddl.h>
#include <aclapi.h>

#include <string>

namespace contextsnap::ipc {
namespace {

core::Error last_error(const std::string& what) {
    const DWORD code = ::GetLastError();
    core::Error error;
    error.code = code == ERROR_ACCESS_DENIED ? core::ErrorCode::PermissionDenied
                                             : core::ErrorCode::IoError;
    error.message = what + " (win32 error " + std::to_string(code) + ")";
    error.context = "ipc.windows";
    error.native_code = static_cast<std::int32_t>(code);
    return error;
}

std::string current_user_sid() {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    std::string sid_text;
    if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        LPSTR text = nullptr;
        if (::ConvertSidToStringSidA(user->User.Sid, &text) != 0) {
            sid_text = text;
            ::LocalFree(text);
        }
    }
    ::CloseHandle(token);
    return sid_text;
}

class PipeConnection final : public Connection {
public:
    explicit PipeConnection(HANDLE handle, bool server_side)
        : handle_(handle), server_side_(server_side) {}
    ~PipeConnection() override { close(); }

    Result<std::size_t> write(std::string_view bytes) override {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return core::err::io("pipe is closed", "ipc.windows");
        }
        DWORD written = 0;
        if (::WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                        nullptr) == 0) {
            return last_error("pipe write failed");
        }
        return static_cast<std::size_t>(written);
    }

    Result<std::string> read_some(std::size_t max_bytes) override {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return core::err::io("pipe is closed", "ipc.windows");
        }
        std::string buffer;
        buffer.resize(max_bytes);
        DWORD read = 0;
        if (::ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                       nullptr) == 0) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED) {
                return std::string{};
            }
            return last_error("pipe read failed");
        }
        buffer.resize(read);
        return buffer;
    }

    void close() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (server_side_) {
                ::FlushFileBuffers(handle_);
                ::DisconnectNamedPipe(handle_);
            }
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] PeerIdentity peer() const override {
        PeerIdentity identity;
        ULONG pid = 0;
        if (server_side_ && ::GetNamedPipeClientProcessId(handle_, &pid) != 0) {
            identity.pid = pid;
        }
        // Impersonation tells us which user opened the client end.
        if (server_side_ && ::ImpersonateNamedPipeClient(handle_) != 0) {
            identity.user = current_user_sid();
            ::RevertToSelf();
        } else {
            identity.user = current_user_sid();
        }
        identity.same_user = identity.user == current_user_sid();
        return identity;
    }

    Status set_timeout(std::chrono::milliseconds timeout) override {
        COMMTIMEOUTS timeouts{};
        timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(timeout.count());
        timeouts.WriteTotalTimeoutConstant = static_cast<DWORD>(timeout.count());
        // Named pipes ignore COMMTIMEOUTS; the value is kept for parity with the
        // POSIX transport and used by the blocking connect path.
        (void)timeouts;
        return Status::success();
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool server_side_{false};
};

class StdioConnection final : public Connection {
public:
    Result<std::size_t> write(std::string_view bytes) override {
        DWORD written = 0;
        if (::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(),
                        static_cast<DWORD>(bytes.size()), &written, nullptr) == 0) {
            return last_error("stdout write failed");
        }
        return static_cast<std::size_t>(written);
    }

    Result<std::string> read_some(std::size_t max_bytes) override {
        std::string buffer;
        buffer.resize(max_bytes);
        DWORD read = 0;
        if (::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buffer.data(),
                       static_cast<DWORD>(buffer.size()), &read, nullptr) == 0 || read == 0) {
            open_ = false;
            return std::string{};
        }
        buffer.resize(read);
        return buffer;
    }

    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] PeerIdentity peer() const override {
        PeerIdentity identity;
        identity.pid = ::GetCurrentProcessId();
        identity.user = current_user_sid();
        identity.same_user = true;
        return identity;
    }

    Status set_timeout(std::chrono::milliseconds) override { return Status::success(); }

private:
    bool open_{true};
};

class PipeListener final : public Listener {
public:
    explicit PipeListener(std::string name) : name_(std::move(name)) {}
    ~PipeListener() override { close(); }

    Result<std::unique_ptr<Connection>> accept() override {
        if (closed_) {
            return core::err::io("listener is closed", "ipc.windows");
        }
        // "D:P(A;;GA;;;OW)" -- only the owner of the pipe may open it.
        SECURITY_ATTRIBUTES attributes{};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorA(
                "D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr) != 0) {
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = descriptor;
            attributes.bInheritHandle = FALSE;
        }
        const HANDLE pipe = ::CreateNamedPipeA(
            name_.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 64 * 1024,
            64 * 1024, 0, descriptor != nullptr ? &attributes : nullptr);
        if (descriptor != nullptr) {
            ::LocalFree(descriptor);
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            return last_error("cannot create named pipe " + name_);
        }
        if (::ConnectNamedPipe(pipe, nullptr) == 0 &&
            ::GetLastError() != ERROR_PIPE_CONNECTED) {
            const core::Error failure = last_error("ConnectNamedPipe failed");
            ::CloseHandle(pipe);
            return failure;
        }
        return std::unique_ptr<Connection>(new PipeConnection(pipe, true));
    }

    void close() override { closed_ = true; }
    [[nodiscard]] std::string endpoint() const override { return name_; }

private:
    std::string name_;
    bool closed_{false};
};

std::string pipe_name(const std::string& endpoint) {
    return endpoint.rfind("\\\\", 0) == 0 ? endpoint : "\\\\.\\pipe\\" + endpoint;
}

}  // namespace

Result<std::unique_ptr<Listener>> listen(const std::string& endpoint) {
    const std::string name = pipe_name(endpoint);
    if (::WaitNamedPipeA(name.c_str(), 1) != 0) {
        return core::Error{core::ErrorCode::AlreadyExists, "another daemon owns " + name,
                          "ipc.windows", 0};
    }
    return std::unique_ptr<Listener>(new PipeListener(name));
}

Result<std::unique_ptr<Connection>> connect(const std::string& endpoint,
                                            std::chrono::milliseconds timeout) {
    const std::string name = pipe_name(endpoint);
    const DWORD deadline = static_cast<DWORD>(timeout.count());
    if (::WaitNamedPipeA(name.c_str(), deadline == 0 ? NMPWAIT_USE_DEFAULT_WAIT : deadline) == 0) {
        return last_error("no daemon is listening on " + name);
    }
    const HANDLE pipe = ::CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return last_error("cannot open " + name);
    }
    DWORD mode = PIPE_READMODE_BYTE;
    ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    return std::unique_ptr<Connection>(new PipeConnection(pipe, false));
}

bool daemon_running(const std::string& endpoint) {
    return ::WaitNamedPipeA(pipe_name(endpoint).c_str(), 50) != 0;
}

std::unique_ptr<Connection> stdio_connection() {
    return std::unique_ptr<Connection>(new StdioConnection());
}

}  // namespace contextsnap::ipc

#endif  // _WIN32
