// Snapshot <-> portable document conversion.
//
// JSON is the always-available interchange format (stable, diffable, documented
// by schemas/snapshot.schema.json). FlatBuffers is an optional zero-copy binary
// format for large libraries and for the IPC fast path; both encode exactly the
// same field set so `export --format json` and `--format fb` round-trip to
// identical Snapshot values (enforced by tests/unit/test_serialization.cpp).
#pragma once

#include <contextsnap/core/json.hpp>
#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::storage {

using core::Result;
using core::Snapshot;
using core::Status;

enum class Format : std::uint8_t { Json, JsonPretty, FlatBuffers };

[[nodiscard]] Result<Format> format_from_string(std::string_view text);
[[nodiscard]] std::string_view to_string(Format format) noexcept;

// --- JSON --------------------------------------------------------------------

[[nodiscard]] core::json::Value to_json(const Snapshot& snapshot);
[[nodiscard]] core::json::Value to_json(const core::WindowInfo& window);
[[nodiscard]] core::json::Value to_json(const core::TabInfo& tab);
[[nodiscard]] core::json::Value to_json(const core::MonitorInfo& monitor);
[[nodiscard]] core::json::Value to_json(const core::SnapshotSummary& summary);
[[nodiscard]] core::json::Value to_json(const core::RestorePlan& plan);
[[nodiscard]] core::json::Value to_json(const core::RestoreReport& report);

[[nodiscard]] Result<Snapshot> snapshot_from_json(const core::json::Value& value);
[[nodiscard]] Result<core::TabInfo> tab_from_json(const core::json::Value& value);

/// Convenience wrappers used by the CLI and the IPC layer.
[[nodiscard]] Result<std::string> serialize(const Snapshot& snapshot, Format format);
[[nodiscard]] Result<Snapshot> deserialize(std::string_view payload, Format format);

/// Document envelope written by `contextsnap export`:
///   { "format": "contextsnap.snapshot", "version": 1, "snapshot": { ... } }
[[nodiscard]] std::string wrap_document(const core::json::Value& snapshot_json);
[[nodiscard]] Result<core::json::Value> unwrap_document(std::string_view text);

// --- Timestamps ---------------------------------------------------------------

/// RFC 3339 in UTC with millisecond precision — the only textual time format
/// used anywhere in the project.
[[nodiscard]] std::string format_timestamp(core::Timestamp timestamp);
[[nodiscard]] Result<core::Timestamp> parse_timestamp(std::string_view text);
[[nodiscard]] std::int64_t to_unix_millis(core::Timestamp timestamp);
[[nodiscard]] core::Timestamp from_unix_millis(std::int64_t millis);

#if defined(CONTEXTSNAP_WITH_FLATBUFFERS)
[[nodiscard]] Result<std::vector<std::uint8_t>> to_flatbuffer(const Snapshot& snapshot);
[[nodiscard]] Result<Snapshot> from_flatbuffer(const std::vector<std::uint8_t>& buffer);
#endif

}  // namespace contextsnap::storage
