// Minimal structured logger. Writes single-line key=value records to stderr (or
// a file), which keeps journald/Console.app/Event Viewer happy and stays
// grep-friendly. No third-party logging framework: the daemon's whole point is
// a small resident set.
//
// Privacy: log records never contain URLs, window titles or file paths above
// `Debug` level. Call sites must use redact() for anything user-derived.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contextsnap::core::log {

enum class Level : std::uint8_t { Trace = 0, Debug, Info, Warn, Error, Off };

[[nodiscard]] Level level_from_string(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(Level level) noexcept;

struct Field {
    std::string key;
    std::string value;
};

/// Redacts user content: keeps scheme+host for URLs, hashes everything else.
[[nodiscard]] std::string redact(std::string_view sensitive);

[[nodiscard]] Field field(std::string key, std::string value);
[[nodiscard]] Field field(std::string key, std::int64_t value);
[[nodiscard]] Field field(std::string key, double value);
[[nodiscard]] Field field(std::string key, bool value);

void set_level(Level level);
[[nodiscard]] Level current_level() noexcept;

/// Redirects output to `path` (rotated at 5 MB, three generations kept).
bool set_log_file(const std::string& path);

/// Adds a field attached to every subsequent record on this process, e.g.
/// component=daemon or pid=1234.
void add_global_field(Field field);

void write(Level level, std::string_view message, std::vector<Field> fields = {});

inline void trace(std::string_view message, std::vector<Field> fields = {}) {
    write(Level::Trace, message, std::move(fields));
}

inline void debug(std::string_view message, std::vector<Field> fields = {}) {
    write(Level::Debug, message, std::move(fields));
}

inline void info(std::string_view message, std::vector<Field> fields = {}) {
    write(Level::Info, message, std::move(fields));
}

inline void warn(std::string_view message, std::vector<Field> fields = {}) {
    write(Level::Warn, message, std::move(fields));
}

inline void error(std::string_view message, std::vector<Field> fields = {}) {
    write(Level::Error, message, std::move(fields));
}

/// Scoped timer that emits `message` with duration_ms on destruction.
class ScopedTimer {
public:
    explicit ScopedTimer(std::string message, Level level = Level::Debug);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    [[nodiscard]] std::uint32_t elapsed_ms() const noexcept;

private:
    std::string message_;
    Level level_;
    std::int64_t start_ns_;
};

}  // namespace contextsnap::core::log
