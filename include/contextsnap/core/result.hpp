// Lightweight error-propagation type. C++20 has no std::expected, and we do not
// want a third-party dependency in the core, so Result<T> fills that role.
//
// Conventions:
//   * Every fallible API returns Result<T>; exceptions are reserved for
//     programming errors (contract violations) and never cross a HAL boundary.
//   * Error carries a machine-readable code plus human context so the CLI and
//     the GUI can render actionable messages without string matching.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace contextsnap::core {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    NotFound,
    AlreadyExists,
    InvalidArgument,
    PermissionDenied,   ///< macOS TCC, Windows UIPI, Polkit, portal denial.
    Unsupported,        ///< Capability missing on this platform/compositor.
    IoError,
    DatabaseError,
    SerializationError,
    ProtocolError,      ///< Malformed IPC / native messaging frame.
    Timeout,
    Cancelled,
    Conflict,           ///< Optimistic concurrency failure.
    PartialFailure,     ///< Restore completed with per-item errors.
    Internal,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string message;
    std::string context;  ///< Subsystem or operation, e.g. "hal.windows.enumerate".
    int native_code{0};   ///< errno / GetLastError() / OSStatus / sqlite rc.

    Error() = default;

    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    Error(ErrorCode c, std::string msg, std::string ctx)
        : code(c), message(std::move(msg)), context(std::move(ctx)) {}

    Error(ErrorCode c, std::string msg, std::string ctx, int native)
        : code(c), message(std::move(msg)), context(std::move(ctx)), native_code(native) {}

    [[nodiscard]] std::string to_string() const;
};

/// Convenience factories keep call sites terse: `return err::not_found("id")`.
namespace err {

inline Error not_found(std::string what, std::string ctx = {}) {
    return Error{ErrorCode::NotFound, std::move(what), std::move(ctx)};
}

inline Error invalid(std::string what, std::string ctx = {}) {
    return Error{ErrorCode::InvalidArgument, std::move(what), std::move(ctx)};
}

inline Error unsupported(std::string what, std::string ctx = {}) {
    return Error{ErrorCode::Unsupported, std::move(what), std::move(ctx)};
}

inline Error denied(std::string what, std::string ctx = {}) {
    return Error{ErrorCode::PermissionDenied, std::move(what), std::move(ctx)};
}

inline Error protocol(std::string what, std::string ctx = {}) {
    return Error{ErrorCode::ProtocolError, std::move(what), std::move(ctx)};
}

inline Error io(std::string what, std::string ctx = {}, int native = 0) {
    return Error{ErrorCode::IoError, std::move(what), std::move(ctx), native};
}

inline Error database(std::string what, std::string ctx = {}, int native = 0) {
    return Error{ErrorCode::DatabaseError, std::move(what), std::move(ctx), native};
}

inline Error internal(std::string what, std::string ctx = {}) {
    return Error{ErrorCode::Internal, std::move(what), std::move(ctx)};
}

}  // namespace err

template <typename T>
class [[nodiscard]] Result {
public:
    using value_type = T;

    Result(T value) : storage_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

    Result(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }

    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        throw_if_error();
        return std::get<0>(storage_);
    }

    const T& value() const& {
        throw_if_error();
        return std::get<0>(storage_);
    }

    T&& value() && {
        throw_if_error();
        return std::get<0>(std::move(storage_));
    }

    T value_or(T fallback) const& {
        return has_value() ? std::get<0>(storage_) : std::move(fallback);
    }

    const Error& error() const& {
        if (has_value()) {
            throw std::logic_error("Result::error() called on a value");
        }
        return std::get<1>(storage_);
    }

    T* operator->() { return &value(); }

    const T* operator->() const { return &value(); }

    T& operator*() & { return value(); }

    const T& operator*() const& { return value(); }

    /// Applies `fn` to the contained value, forwarding the error otherwise.
    template <typename F>
    auto map(F&& fn) const& -> Result<std::invoke_result_t<F, const T&>> {
        using U = std::invoke_result_t<F, const T&>;
        if (!has_value()) {
            return Result<U>(error());
        }
        return Result<U>(std::forward<F>(fn)(std::get<0>(storage_)));
    }

    /// Chains another fallible operation.
    template <typename F>
    auto and_then(F&& fn) const& -> std::invoke_result_t<F, const T&> {
        using R = std::invoke_result_t<F, const T&>;
        if (!has_value()) {
            return R(error());
        }
        return std::forward<F>(fn)(std::get<0>(storage_));
    }

private:
    void throw_if_error() const {
        if (!has_value()) {
            throw std::runtime_error(std::get<1>(storage_).to_string());
        }
    }

    std::variant<T, Error> storage_;
};

/// Result<void> for fallible operations without a payload.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;

    Result(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT

    [[nodiscard]] bool has_value() const noexcept { return ok_; }

    explicit operator bool() const noexcept { return ok_; }

    const Error& error() const& {
        if (ok_) {
            throw std::logic_error("Result<void>::error() called on success");
        }
        return error_;
    }

    static Result<void> success() { return Result<void>{}; }

private:
    Error error_{};
    bool ok_{true};
};

using Status = Result<void>;

}  // namespace contextsnap::core
