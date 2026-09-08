#include <contextsnap/core/result.hpp>

namespace contextsnap::core {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return "ok";
        case ErrorCode::NotFound:
            return "not_found";
        case ErrorCode::AlreadyExists:
            return "already_exists";
        case ErrorCode::InvalidArgument:
            return "invalid_argument";
        case ErrorCode::PermissionDenied:
            return "permission_denied";
        case ErrorCode::Unsupported:
            return "unsupported";
        case ErrorCode::IoError:
            return "io_error";
        case ErrorCode::DatabaseError:
            return "database_error";
        case ErrorCode::SerializationError:
            return "serialization_error";
        case ErrorCode::ProtocolError:
            return "protocol_error";
        case ErrorCode::Timeout:
            return "timeout";
        case ErrorCode::Cancelled:
            return "cancelled";
        case ErrorCode::Conflict:
            return "conflict";
        case ErrorCode::PartialFailure:
            return "partial_failure";
        case ErrorCode::Internal:
            break;
    }
    return "internal";
}

std::string Error::to_string() const {
    // Stable, greppable rendering: "[not_found] no such snapshot (repo.load) [native=1]".
    std::string out;
    out.reserve(message.size() + context.size() + 32);
    out.push_back('[');
    out.append(contextsnap::core::to_string(code));
    out.append("] ");
    out.append(message.empty() ? "unspecified error" : message);
    if (!context.empty()) {
        out.append(" (");
        out.append(context);
        out.push_back(')');
    }
    if (native_code != 0) {
        out.append(" [native=");
        out.append(std::to_string(native_code));
        out.push_back(']');
    }
    return out;
}

}  // namespace contextsnap::core
