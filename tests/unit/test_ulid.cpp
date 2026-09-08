// Snapshot ids are ULIDs so that lexicographic order equals chronological
// order. Several SQL queries depend on that property.
#include "framework/microtest.hpp"

#include <contextsnap/core/ulid.hpp>

#include <chrono>
#include <set>
#include <string>

namespace core = contextsnap::core;

TEST("generated ids are 26 valid Crockford base32 characters") {
    const std::string id = core::generate_ulid();
    CHECK_EQ(id.size(), std::size_t(26));
    CHECK(core::is_valid_ulid(id));
}

TEST("ambiguous characters are rejected") {
    CHECK_FALSE(core::is_valid_ulid(""));
    CHECK_FALSE(core::is_valid_ulid("01J8Z2"));                        // too short
    CHECK_FALSE(core::is_valid_ulid("01J8Z2ABCDEFGHIJKLMNOPQRSTU"));   // too long
    CHECK_FALSE(core::is_valid_ulid("01J8Z2ILOU00000000000000AB"));    // I, L, O, U
}

TEST("lexicographic order follows time order") {
    const core::Timestamp base = core::Clock::now();
    const std::string earlier = core::generate_ulid(base, 0, 0);
    const std::string later =
        core::generate_ulid(base + std::chrono::milliseconds{5}, 0, 0);
    CHECK(earlier < later);
}

TEST("the embedded timestamp is recoverable to the millisecond") {
    const core::Timestamp when = core::Clock::now();
    const std::string id = core::generate_ulid(when, 1, 2);

    const auto expected =
        std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
    const auto actual = std::chrono::duration_cast<std::chrono::milliseconds>(
                            core::ulid_timestamp(id).time_since_epoch())
                            .count();
    CHECK_EQ(actual, expected);
}

TEST("randomness makes collisions unlikely within a millisecond") {
    std::set<std::string> ids;
    for (int index = 0; index < 512; ++index) {
        ids.insert(core::generate_ulid());
    }
    CHECK_EQ(ids.size(), std::size_t(512));
}

TEST("short ids stay unique enough to type") {
    const std::string id = core::generate_ulid();
    CHECK_EQ(core::short_id(id).size(), std::size_t(8));
    CHECK_EQ(core::short_id(id, 4).size(), std::size_t(4));
    // The suffix carries the entropy, so short ids are taken from the tail.
    CHECK_EQ(core::short_id(id, 4), id.substr(id.size() - 4));
}

MICROTEST_MAIN()
