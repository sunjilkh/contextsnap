// ULID identifiers (Crockford base32, 26 chars: 48-bit timestamp + 80-bit
// randomness). Chosen over UUIDv4 because snapshot ids sort chronologically as
// text, which makes `ORDER BY id` in SQLite equivalent to `ORDER BY created_at`
// and keeps b-tree inserts append-only.
#pragma once

#include <contextsnap/core/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace contextsnap::core {

/// Generates a new ULID from the current wall clock.
[[nodiscard]] std::string generate_ulid();

/// Deterministic variant used by tests and by import/merge code paths.
[[nodiscard]] std::string generate_ulid(Timestamp when, std::uint64_t random_hi,
                                        std::uint64_t random_lo);

/// Validates the character set and length without allocating.
[[nodiscard]] bool is_valid_ulid(std::string_view text) noexcept;

/// Extracts the embedded millisecond timestamp; returns epoch on malformed input.
[[nodiscard]] Timestamp ulid_timestamp(std::string_view text) noexcept;

/// Short, human-typable prefix used by the CLI (`contextsnap show 01J8Z2`).
[[nodiscard]] std::string short_id(std::string_view ulid, std::size_t length = 8);

}  // namespace contextsnap::core
