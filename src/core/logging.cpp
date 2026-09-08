#include <contextsnap/core/logging.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>

namespace contextsnap::core::log {
namespace {

constexpr std::uintmax_t kMaxLogBytes = 5u * 1024u * 1024u;
constexpr int kRotatedGenerations = 3;

struct State {
    std::mutex mutex;
    std::atomic<Level> level{Level::Info};
    std::FILE* sink{nullptr};  ///< nullptr == stderr
    std::string sink_path;
    std::vector<Field> globals;
};

State& state() {
    static State instance;
    return instance;
}

/// logfmt requires quoting whenever a value contains spaces or quotes.
std::string quote_if_needed(const std::string& value) {
    const bool needs_quotes =
        value.empty() || value.find_first_of(" \t\"=\n") != std::string::npos;
    if (!needs_quotes) {
        return value;
    }
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out.append("\\n");
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string iso8601_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - secs).count();
    const std::time_t tt = system_clock::to_time_t(secs);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << millis << 'Z';
    return out.str();
}

/// Rotates sink_path when it exceeds kMaxLogBytes. Caller holds the mutex.
void rotate_if_needed(State& s) {
    if (s.sink == nullptr || s.sink_path.empty()) {
        return;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(s.sink_path, ec);
    if (ec || size < kMaxLogBytes) {
        return;
    }
    std::fclose(s.sink);
    s.sink = nullptr;
    for (int i = kRotatedGenerations - 1; i >= 1; --i) {
        const std::string from = s.sink_path + "." + std::to_string(i);
        const std::string to = s.sink_path + "." + std::to_string(i + 1);
        std::filesystem::rename(from, to, ec);
    }
    std::filesystem::rename(s.sink_path, s.sink_path + ".1", ec);
    s.sink = std::fopen(s.sink_path.c_str(), "ae");
    if (s.sink == nullptr) {
        s.sink = std::fopen(s.sink_path.c_str(), "a");
    }
}

std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

Level level_from_string(std::string_view text) noexcept {
    if (text == "trace") {
        return Level::Trace;
    }
    if (text == "debug") {
        return Level::Debug;
    }
    if (text == "warn" || text == "warning") {
        return Level::Warn;
    }
    if (text == "error") {
        return Level::Error;
    }
    if (text == "off" || text == "none") {
        return Level::Off;
    }
    return Level::Info;
}

std::string_view to_string(Level level) noexcept {
    switch (level) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warn";
        case Level::Error:
            return "error";
        case Level::Off:
            break;
    }
    return "off";
}

std::string redact(std::string_view sensitive) {
    if (sensitive.empty()) {
        return "<empty>";
    }
    // Keep scheme://host so operators can still reason about which site failed,
    // and hash the rest: paths and query strings are the sensitive part.
    const std::size_t scheme = sensitive.find("://");
    std::ostringstream out;
    if (scheme != std::string_view::npos) {
        const std::size_t host_start = scheme + 3;
        const std::size_t host_end = sensitive.find('/', host_start);
        out << sensitive.substr(0, host_end == std::string_view::npos ? sensitive.size()
                                                                     : host_end);
        if (host_end != std::string_view::npos && host_end + 1 < sensitive.size()) {
            out << "/#" << std::hex << fnv1a(sensitive.substr(host_end));
        }
        return out.str();
    }
    out << "#" << std::hex << fnv1a(sensitive) << "/" << std::dec << sensitive.size();
    return out.str();
}

Field field(std::string key, std::string value) { return Field{std::move(key), std::move(value)}; }

Field field(std::string key, std::int64_t value) {
    return Field{std::move(key), std::to_string(value)};
}

Field field(std::string key, double value) {
    std::ostringstream out;
    out << value;
    return Field{std::move(key), out.str()};
}

Field field(std::string key, bool value) {
    return Field{std::move(key), value ? "true" : "false"};
}

void set_level(Level level) { state().level.store(level, std::memory_order_relaxed); }

Level current_level() noexcept { return state().level.load(std::memory_order_relaxed); }

bool set_log_file(const std::string& path) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    std::FILE* handle = std::fopen(path.c_str(), "a");
    if (handle == nullptr) {
        return false;
    }
    if (s.sink != nullptr) {
        std::fclose(s.sink);
    }
    s.sink = handle;
    s.sink_path = path;
    return true;
}

void add_global_field(Field f) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.globals.push_back(std::move(f));
}

void write(Level level, std::string_view message, std::vector<Field> fields) {
    State& s = state();
    if (level < s.level.load(std::memory_order_relaxed) || level == Level::Off) {
        return;
    }

    std::ostringstream line;
    line << iso8601_now() << " level=" << to_string(level) << " msg="
         << quote_if_needed(std::string(message));

    const std::lock_guard<std::mutex> lock(s.mutex);
    for (const auto& f : s.globals) {
        line << ' ' << f.key << '=' << quote_if_needed(f.value);
    }
    for (const auto& f : fields) {
        line << ' ' << f.key << '=' << quote_if_needed(f.value);
    }
    line << '\n';

    rotate_if_needed(s);
    std::FILE* sink = s.sink != nullptr ? s.sink : stderr;
    const std::string text = line.str();
    std::fwrite(text.data(), 1, text.size(), sink);
    std::fflush(sink);
}

ScopedTimer::ScopedTimer(std::string message, Level level)
    : message_(std::move(message)),
      level_(level),
      start_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()) {}

ScopedTimer::~ScopedTimer() {
    write(level_, message_, {field("duration_ms", static_cast<std::int64_t>(elapsed_ms()))});
}

std::uint32_t ScopedTimer::elapsed_ms() const noexcept {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    const std::int64_t delta = (now - start_ns_) / 1000000;
    return static_cast<std::uint32_t>(delta < 0 ? 0 : delta);
}

}  // namespace contextsnap::core::log
