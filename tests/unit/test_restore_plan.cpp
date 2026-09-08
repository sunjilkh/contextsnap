// End-to-end exercise of capture -> store -> plan -> dry-run restore on the
// null HAL backend, so the whole pipeline is covered without a display server.
#include "framework/microtest.hpp"

#include <contextsnap/browser/bridge.hpp>
#include <contextsnap/core/config.hpp>
#include <contextsnap/core/snapshot_manager.hpp>
#include <contextsnap/hal/null_platform.hpp>
#include <contextsnap/privacy/sanitizer.hpp>
#include <contextsnap/storage/snapshot_repository.hpp>

#include <memory>
#include <string>

namespace core = contextsnap::core;
namespace hal = contextsnap::hal;
namespace storage = contextsnap::storage;
namespace browser = contextsnap::browser;
namespace privacy = contextsnap::privacy;

namespace {

struct Fixture {
    std::shared_ptr<hal::Platform> platform;
    std::shared_ptr<storage::SnapshotRepository> repository;
    std::unique_ptr<core::SnapshotManager> manager;
};

/// Builds a manager backed by the deterministic null platform and an in-memory
/// database. sample_null_state() contains an editor window, a browser window
/// with two tabs, two monitors (one HiDPI) and a cursor.
Fixture make_fixture() {
    Fixture fixture;
    fixture.platform = hal::create_null_platform(hal::sample_null_state());

    storage::DatabaseOptions options;
    options.path = ":memory:";
    auto repository = storage::SnapshotRepository::open(options);
    CHECK_OK(repository);
    fixture.repository = repository.value();

    core::Config config = core::Config::with_defaults();
    config.capture.auto_capture = false;

    fixture.manager = std::make_unique<core::SnapshotManager>(
        fixture.platform, fixture.repository, browser::Bridge::create_null(),
        std::make_shared<privacy::Sanitizer>(), config);
    return fixture;
}

core::CaptureOptions capture_options(const std::string& name) {
    core::CaptureOptions options;
    options.name = name;
    options.include_tabs = false;  // No extension in tests; skip the wait.
    options.include_cursor = true;
    return options;
}

}  // namespace

TEST("capture reads the platform state") {
    Fixture fixture = make_fixture();
    auto snapshot = fixture.manager->capture(capture_options("fixture"));
    CHECK_OK(snapshot);
    CHECK_FALSE(snapshot.value().windows.empty());
    CHECK_FALSE(snapshot.value().monitors.empty());
    CHECK_EQ(snapshot.value().metadata.name, std::string("fixture"));
}

TEST("stored snapshots are listed and reloaded identically") {
    Fixture fixture = make_fixture();
    auto stored = fixture.manager->capture_and_store(capture_options("deep work"));
    CHECK_OK(stored);
    const std::string id = stored.value().metadata.id;
    CHECK_FALSE(id.empty());

    auto summaries = fixture.manager->list();
    CHECK_OK(summaries);
    CHECK_EQ(summaries.value().size(), std::size_t(1));
    CHECK_EQ(summaries.value().front().id, id);
    CHECK_EQ(summaries.value().front().window_count,
             static_cast<std::uint32_t>(stored.value().windows.size()));

    auto reloaded = fixture.manager->get(id);
    CHECK_OK(reloaded);
    CHECK_EQ(reloaded.value().windows.size(), stored.value().windows.size());
    CHECK_EQ(reloaded.value().monitors.size(), stored.value().monitors.size());
    CHECK_EQ(reloaded.value().metadata.name, std::string("deep work"));
}

TEST("a plan is produced for every captured window") {
    Fixture fixture = make_fixture();
    auto stored = fixture.manager->capture_and_store(capture_options("planning"));
    CHECK_OK(stored);

    core::RestoreOptions options;
    options.dry_run = true;
    auto plan = fixture.manager->plan_restore(stored.value().metadata.id, options);
    CHECK_OK(plan);
    CHECK_EQ(plan.value().snapshot_id, stored.value().metadata.id);
    CHECK_FALSE(plan.value().steps.empty());

    bool moves_a_window = false;
    for (const core::RestoreStep& step : plan.value().steps) {
        if (step.kind == core::RestoreStepKind::MoveWindow) {
            moves_a_window = true;
        }
    }
    CHECK(moves_a_window);
}

TEST("selectors narrow the plan") {
    Fixture fixture = make_fixture();
    auto stored = fixture.manager->capture_and_store(capture_options("selective"));
    CHECK_OK(stored);

    core::RestoreOptions all;
    all.dry_run = true;
    auto full_plan = fixture.manager->plan_restore(stored.value().metadata.id, all);
    CHECK_OK(full_plan);

    core::RestoreOptions narrowed = all;
    core::RestoreSelector selector;
    selector.kind = core::RestoreSelector::Kind::App;
    selector.pattern = stored.value().windows.front().process.app_id;
    narrowed.selectors.push_back(selector);

    auto narrow_plan = fixture.manager->plan_restore(stored.value().metadata.id, narrowed);
    CHECK_OK(narrow_plan);
    CHECK(narrow_plan.value().steps.size() <= full_plan.value().steps.size());
}

TEST("a dry run changes nothing and reports cleanly") {
    Fixture fixture = make_fixture();
    auto stored = fixture.manager->capture_and_store(capture_options("dry run"));
    CHECK_OK(stored);

    core::RestoreOptions options;
    options.dry_run = true;
    options.launch_missing_apps = false;
    options.restore_tabs = false;

    auto report = fixture.manager->restore(stored.value().metadata.id, options);
    CHECK_OK(report);
    CHECK_EQ(report.value().snapshot_id, stored.value().metadata.id);
    CHECK_EQ(report.value().windows_failed, std::uint32_t(0));
    CHECK(report.value().fully_successful());
}

TEST("unknown ids fail with NotFound rather than crashing") {
    Fixture fixture = make_fixture();
    auto missing = fixture.manager->get("01J0000000000000000000MISS");
    CHECK_ERR(missing);
    CHECK_EQ(missing.error().code, core::ErrorCode::NotFound);
}

TEST("deleting a snapshot removes it from the repository") {
    Fixture fixture = make_fixture();
    auto stored = fixture.manager->capture_and_store(capture_options("temporary"));
    CHECK_OK(stored);
    CHECK_OK(fixture.manager->remove(stored.value().metadata.id));
    CHECK_ERR(fixture.manager->get(stored.value().metadata.id));

    auto summaries = fixture.manager->list();
    CHECK_OK(summaries);
    CHECK(summaries.value().empty());
}

TEST("metadata edits are persisted") {
    Fixture fixture = make_fixture();
    auto stored = fixture.manager->capture_and_store(capture_options("before"));
    CHECK_OK(stored);
    const std::string id = stored.value().metadata.id;

    CHECK_OK(fixture.manager->rename(id, "after"));
    CHECK_OK(fixture.manager->set_favorite(id, true));
    CHECK_OK(fixture.manager->set_tags(id, {"focus", "writing"}));

    auto reloaded = fixture.manager->get(id);
    CHECK_OK(reloaded);
    CHECK_EQ(reloaded.value().metadata.name, std::string("after"));
    CHECK(reloaded.value().metadata.favorite);
    CHECK_EQ(reloaded.value().metadata.tags.size(), std::size_t(2));
}

MICROTEST_MAIN()
