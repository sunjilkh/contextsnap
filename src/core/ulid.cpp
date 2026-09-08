#include <contextsnap/core/ulid.hpp>

#include <array>
#include <chrono>
#include <random>

namespace contextsnap::core {
namespace {

// Crockford base32: no I, L, O or U, so ids survive being read aloud or typed.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t kTimestampChars = 10;
constexpr std::size_t kRandomChars = 16;
constexpr std::size_t kUlidLength = kTimestampChars + kRandomChars;

int decode_char(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    // Accept lowercase and the ambiguous characters Crockford maps back.
    switch (c) {
        case 'i':
        case 'I':
        case 'l':
        case 'L':
            return 1;
        case 'o':
        case 'O':
            return 0;
        default:
            break;
    }
    const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    for (int i = 10; i < 32; ++i) {
        if (kAlphabet[i] == upper) {
            return i;
        }
    }
    return -1;
}

std::uint64_t random_word() {
    static thread_local std::mt19937_64 engine{[] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        std::mt19937_64 seeded;
        seeded.seed(seed);
        return seeded;
    }()};
    return engine();
}

}  // namespace

std::string generate_ulid() {
    return generate_ulid(Clock::now(), random_word(), random_word());
}

std::string generate_ulid(Timestamp when, std::uint64_t random_hi, std::uint64_t random_lo) {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            when.time_since_epoch())
                            .count();
    auto timestamp = static_cast<std::uint64_t>(millis < 0 ? 0 : millis) & 0x0000FFFFFFFFFFFFULL;

    std::string out(kUlidLength, '0');
    for (std::size_t i = kTimestampChars; i-- > 0;) {
        out[i] = kAlphabet[timestamp & 0x1FULL];
        timestamp >>= 5;
    }

    // 80 random bits = 16 base32 characters: 8 from each word's low 40 bits.
    std::uint64_t hi = random_hi & 0x000000FFFFFFFFFFULL;
    std::uint64_t lo = random_lo & 0x000000FFFFFFFFFFULL;
    for (std::size_t i = 8; i-- > 0;) {
        out[kTimestampChars + i] = kAlphabet[hi & 0x1FULL];
        hi >>= 5;
    }
    for (std::size_t i = 8; i-- > 0;) {
        out[kTimestampChars + 8 + i] = kAlphabet[lo & 0x1FULL];
        lo >>= 5;
    }
    return out;
}

bool is_valid_ulid(std::string_view text) noexcept {
    if (text.size() != kUlidLength) {
        return false;
    }
    for (const char c : text) {
        if (decode_char(c) < 0) {
            return false;
        }
    }
    // The first character encodes the top 3 bits of a 48-bit timestamp: values
    // above '7' would overflow the year 10889 boundary.
    return decode_char(text[0]) <= 7;
}

Timestamp ulid_timestamp(std::string_view text) noexcept {
    if (text.size() < kTimestampChars) {
        return Timestamp{};
    }
    std::uint64_t millis = 0;
    for (std::size_t i = 0; i < kTimestampChars; ++i) {
        const int value = decode_char(text[i]);
        if (value < 0) {
            return Timestamp{};
        }
        millis = (millis << 5) | static_cast<std::uint64_t>(value);
    }
    return Timestamp{std::chrono::milliseconds{static_cast<std::int64_t>(millis)}};
}

std::string short_id(std::string_view ulid, std::size_t length) {
    if (length == 0 || ulid.empty()) {
        return std::string{};
    }
    // The tail carries the randomness; leading characters are near-identical for
    // snapshots taken within the same day, so they make poor short ids.
    const std::size_t take = std::min(length, ulid.size());
    return std::string(ulid.substr(ulid.size() - take));
}

}  // namespace contextsnap::core
