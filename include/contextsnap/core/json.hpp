// A small, dependency-free JSON value type.
//
// Why not nlohmann/json? The daemon must stay under a 30 MB RSS budget and ship
// with a single mandatory dependency (SQLite). This implementation is ~600 SLOC,
// allocation-frugal, preserves key insertion order (deterministic exports, clean
// diffs) and is exercised by tests/unit/test_json.cpp.
//
// Scope: RFC 8259 with the usual pragmatics — no comments, no trailing commas,
// UTF-8 in/out, \u escapes decoded to UTF-8 (surrogate pairs included).
#pragma once

#include <contextsnap/core/result.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace contextsnap::core::json {

class Value;

/// Insertion-ordered object. std::vector keeps incomplete-type support legal
/// and gives us byte-stable serialization across platforms.
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };

class Value {
public:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Array,
                                 Object>;

    Value() = default;
    Value(std::nullptr_t) {}                                     // NOLINT
    Value(bool v) : storage_(v) {}                               // NOLINT
    Value(int v) : storage_(static_cast<std::int64_t>(v)) {}     // NOLINT
    Value(std::int64_t v) : storage_(v) {}                       // NOLINT
    Value(std::uint32_t v) : storage_(static_cast<std::int64_t>(v)) {}  // NOLINT
    Value(std::uint64_t v) : storage_(static_cast<std::int64_t>(v)) {}  // NOLINT
    Value(double v) : storage_(v) {}                             // NOLINT
    Value(const char* v) : storage_(std::string(v)) {}           // NOLINT
    Value(std::string v) : storage_(std::move(v)) {}             // NOLINT
    Value(std::string_view v) : storage_(std::string(v)) {}      // NOLINT
    Value(Array v) : storage_(std::move(v)) {}                   // NOLINT
    Value(Object v) : storage_(std::move(v)) {}                  // NOLINT

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] bool is_bool() const noexcept { return storage_.index() == 1; }
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept { return storage_.index() == 4; }
    [[nodiscard]] bool is_array() const noexcept { return storage_.index() == 5; }
    [[nodiscard]] bool is_object() const noexcept { return storage_.index() == 6; }

    // Typed accessors returning a default instead of throwing: JSON coming from
    // a browser extension or an imported file is untrusted input, and defensive
    // call sites would otherwise be littered with checks.
    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
    [[nodiscard]] std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] double as_double(double fallback = 0.0) const noexcept;
    [[nodiscard]] std::string as_string(std::string_view fallback = {}) const;
    [[nodiscard]] const Array& as_array() const noexcept;
    [[nodiscard]] const Object& as_object() const noexcept;

    /// Object member lookup; returns nullptr when absent or not an object.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }

    /// Dotted path lookup: value.at("metadata.tags").
    [[nodiscard]] const Value* at_path(std::string_view dotted_path) const noexcept;

    /// Object mutation. Replaces an existing key in place, preserving order.
    Value& set(std::string key, Value value);
    Value& push_back(Value value);

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::string dump(int indent = -1) const;

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    static Value object(std::initializer_list<std::pair<const char*, Value>> entries);

private:
    Storage storage_{};
};

/// Parses `text`. On failure the error message carries a byte offset.
[[nodiscard]] Result<Value> parse(std::string_view text);

/// Serializes `value`. indent < 0 -> compact, otherwise pretty-printed.
[[nodiscard]] std::string serialize(const Value& value, int indent = -1);

/// Escapes `input` as a JSON string literal body (no surrounding quotes).
[[nodiscard]] std::string escape(std::string_view input);

}  // namespace contextsnap::core::json
