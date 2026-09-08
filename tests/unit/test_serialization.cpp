// Snapshots are the project's file format: exports must survive a round-trip
// and timestamps must stay millisecond-exact across the JSON boundary.
#include "framework/microtest.hpp"

#include <contextsnap/browser/bridge.hpp>
#include <contextsnap/core/config.hpp>
#include <contextsnap/core/snapshot_manager.hpp>
#include <contextsnap/hal/null_platform.hpp>
#include <contextsnap/privacy/sanitizer.hpp>
#include <contextsnap/storage/serialization.hpp>
#include <contextsnap/storage/snapshot_repository.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace core = contextsnap::core;
namespace hal = contextsnap::hal;
namespace storage = contextsnap::storage;
namespace browser = contextsnap::browser;
namespace privacy = contextsnap::privacy;
using contextsnap::core::json::Value;

namespace {

core::Snapshot sample_snapshot() {
    auto platform = hal::create_null_platform(hal::sample_null_state());

    storage::DatabaseOptions database;
    database.path = ":memory:";
    auto repository = storage::SnapshotRepository::open(database);
    CHECK_OK(repository);

    core::SnapshotManager manager(platform, repository.value(), browser::Bridge::create_null(),
                                  std::make_shared<privacy::Sanitizer>(),
                                  core::Config::with_defaults());

    core::CaptureOptions options;
    options.name = "serialisation fixture";
    options.tags = {"focus", "test"};
    options.include_tabs = false;
    auto snapshot = manager.capture(options);
    CHECK_OK(snapshot);
    return snapshot.value();
}

}  // namespace

TEST("snapshots round-trip through JSON without losing structure") {
    const core::Snapshot original = sample_snapshot();
    const Value document = storage::to_json(original);

    auto restored = storage::snapshot_from_json(document);
    CHECK_OK(restored);
    CHECK_EQ(restored.value().metadata.name, original.metadata.name);
    CHECK_EQ(restored.value().metadata.tags.size(), original.metadata.tags.size());
    CHECK_EQ(restored.value().windows.size(), original.windows.size());
    CHECK_EQ(restored.value().monitors.size(), original.monitors.size());
    CHECK_EQ(restored.value().tab_count(), original.tab_count());
    CHECK_EQ(restored.value().metadata.platform, original.metadata.platform);
    CHECK_EQ(restored.value().metadata.session_type, original.metadata.session_type);
}

TEST("window geometry and state are preserved exactly") {
    const core::Snapshot original = sample_snapshot();
    auto restored = storage::snapshot_from_json(storage::to_json(original));
    CHECK_OK(restored);

    const core::WindowInfo& before = original.windows.front();
    const core::WindowInfo& after = restored.value().windows.front();
    CHECK_EQ(after.title, before.title);
    CHECK_EQ(after.frame.x, before.frame.x);
    CHECK_EQ(after.frame.y, before.frame.y);
    CHECK_EQ(after.frame.width, before.frame.width);
    CHECK_EQ(after.frame.height, before.frame.height);
    CHECK_EQ(after.state, before.state);
    CHECK_EQ(after.monitor_id, before.monitor_id);
    CHECK_EQ(after.process.app_id, before.process.app_id);
}

TEST("serialize and deserialize agree on the JSON format") {
    const core::Snapshot original = sample_snapshot();

    auto compact = storage::serialize(original, storage::Format::Json);
    CHECK_OK(compact);
    auto pretty = storage::serialize(original, storage::Format::JsonPretty);
    CHECK_OK(pretty);
    CHECK(pretty.value().size() >= compact.value().size());

    auto parsed = storage::deserialize(compact.value(), storage::Format::Json);
    CHECK_OK(parsed);
    CHECK_EQ(parsed.value().windows.size(), original.windows.size());
}

TEST("format names map to the enum") {
    auto json_format = storage::format_from_string("json");
    CHECK_OK(json_format);
    CHECK_EQ(json_format.value(), storage::Format::Json);
    CHECK_EQ(storage::to_string(storage::Format::JsonPretty), std::string("json-pretty"));
    CHECK_ERR(storage::format_from_string("yaml"));
}

TEST("export documents carry a format tag and a version") {
    const core::Snapshot original = sample_snapshot();
    const std::string document = storage::wrap_document(storage::to_json(original));
    CHECK(document.find("contextsnap.snapshot") != std::string::npos);

    auto unwrapped = storage::unwrap_document(document);
    CHECK_OK(unwrapped);
    auto restored = storage::snapshot_from_json(unwrapped.value());
    CHECK_OK(restored);
    CHECK_EQ(restored.value().metadata.name, original.metadata.name);

    CHECK_ERR(storage::unwrap_document("{\"format\":\"something.else\",\"version\":1}"));
    CHECK_ERR(storage::unwrap_document("not json at all"));
}

TEST("timestamps are RFC 3339 and millisecond-exact") {
    const core::Timestamp now = core::Clock::now();
    const std::string text = storage::format_timestamp(now);
    CHECK(text.find('T') != std::string::npos);
    CHECK_EQ(text.back(), 'Z');

    auto parsed = storage::parse_timestamp(text);
    CHECK_OK(parsed);
    CHECK_EQ(storage::to_unix_millis(parsed.value()), storage::to_unix_millis(now));

    CHECK_EQ(storage::to_unix_millis(storage::from_unix_millis(1757310000123LL)), 1757310000123LL);
    CHECK_ERR(storage::parse_timestamp("yesterday"));
}

TEST("tabs deserialise from the browser payload shape") {
    auto document = contextsnap::core::json::parse(
        "{\"id\":\"7\",\"index\":2,\"url\":\"https://example.com/docs\",\"title\":\"Docs\","
        "\"pinned\":true,\"active\":false,\"muted\":false,\"discarded\":true,\"scroll_y\":120}");
    CHECK_OK(document);

    auto tab = storage::tab_from_json(document.value());
    CHECK_OK(tab);
    CHECK_EQ(tab.value().url, std::string("https://example.com/docs"));
    CHECK_EQ(tab.value().title, std::string("Docs"));
    CHECK_EQ(tab.value().index, std::uint32_t(2));
    CHECK(tab.value().pinned);
    CHECK(tab.value().discarded);
    CHECK_FALSE(tab.value().active);
}

MICROTEST_MAIN()
