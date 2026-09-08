// The daemon RPC contract.
//
// Wire format (v1): 4-byte little-endian length prefix + payload.
// The payload codec is JSON by default and protobuf when built with
// CONTEXTSNAP_WITH_PROTOBUF (proto/contextsnap.proto holds the canonical IDL).
// Both encode the same logical messages defined here, so the transport, the
// server and every client are codec-agnostic. See docs/ipc-protocol.md.
#pragma once

#include <contextsnap/core/json.hpp>
#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::ipc {

using core::Result;

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kLengthPrefixBytes = 4;

/// Every method the daemon exposes. Adding a method is a minor version bump;
/// changing an existing payload shape is a major one.
enum class Method : std::uint16_t {
    Unknown = 0,
    Hello,            ///< Handshake: version negotiation + capability exchange.
    Ping,
    CaptureSnapshot,
    ListSnapshots,
    GetSnapshot,
    DeleteSnapshot,
    RenameSnapshot,
    TagSnapshot,
    FavoriteSnapshot,
    PlanRestore,
    RestoreSnapshot,
    SearchSnapshots,
    ExportSnapshot,
    ImportSnapshot,
    GetConfig,
    SetConfig,
    GetHealth,
    PruneSnapshots,
    Subscribe,        ///< Streams events until the client disconnects.
    Shutdown,
};

[[nodiscard]] std::string_view to_string(Method method) noexcept;
[[nodiscard]] Method method_from_string(std::string_view text) noexcept;

/// Server-initiated notifications delivered on subscribed connections.
enum class EventType : std::uint16_t {
    SnapshotCreated,
    SnapshotDeleted,
    SnapshotUpdated,
    RestoreStarted,
    RestoreProgress,
    RestoreFinished,
    BrowserConnected,
    BrowserDisconnected,
    ConfigChanged,
    DaemonShuttingDown,
};

[[nodiscard]] std::string_view to_string(EventType type) noexcept;

struct Request {
    std::uint64_t id{0};  ///< Client-chosen correlation id; echoed in Response.
    Method method{Method::Unknown};
    core::json::Value params{};

    [[nodiscard]] core::json::Value to_json() const;
    [[nodiscard]] static Result<Request> from_json(const core::json::Value& value);
};

struct Response {
    std::uint64_t id{0};
    bool ok{true};
    core::json::Value result{};
    core::ErrorCode error_code{core::ErrorCode::Ok};
    std::string error_message;

    [[nodiscard]] static Response success(std::uint64_t id, core::json::Value result);
    [[nodiscard]] static Response failure(std::uint64_t id, const core::Error& error);

    [[nodiscard]] core::json::Value to_json() const;
    [[nodiscard]] static Result<Response> from_json(const core::json::Value& value);
};

struct Event {
    EventType type{EventType::SnapshotCreated};
    core::json::Value payload{};

    [[nodiscard]] core::json::Value to_json() const;
    [[nodiscard]] static Result<Event> from_json(const core::json::Value& value);
};

// --- Frame codec -------------------------------------------------------------
// Shared with the browser native messaging host, which uses the identical
// 4-byte-length framing (that is the Chrome/Firefox native messaging rule).

/// Prepends the length prefix. Fails when payload exceeds kMaxFrameBytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(std::string_view payload,
                                                             std::uint32_t max_bytes = kMaxFrameBytes);

struct DecodedFrame {
    std::string payload;
    std::size_t consumed{0};  ///< Bytes taken from the buffer, prefix included.
};

/// Attempts to decode one frame from the front of `buffer`.
/// Returns std::nullopt (not an error) when more bytes are needed.
[[nodiscard]] Result<std::optional<DecodedFrame>> decode_frame(
    std::string_view buffer,
    std::uint32_t max_bytes = kMaxFrameBytes);

/// Incremental reader for stream transports: feed bytes, pop complete frames.
class FrameReader {
public:
    explicit FrameReader(std::uint32_t max_bytes = kMaxFrameBytes) : max_bytes_(max_bytes) {}

    void feed(std::string_view bytes);

    /// Pops the next complete frame, or nullopt when none is buffered yet.
    [[nodiscard]] Result<std::optional<std::string>> next();

    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

    void reset() { buffer_.clear(); }

private:
    std::string buffer_;
    std::uint32_t max_bytes_;
};

}  // namespace contextsnap::ipc
