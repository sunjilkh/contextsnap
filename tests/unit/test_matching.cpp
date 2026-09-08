// Matching heuristics decide whether a live window is "the same" window that
// was captured. These are the tests that keep restoration from moving the
// wrong window.
#include "framework/microtest.hpp"

#include <contextsnap/core/matching.hpp>

#include <string>
#include <vector>

namespace core = contextsnap::core;
namespace matching = contextsnap::core::matching;

namespace {

core::WindowInfo make_window(const std::string& app_id, const std::string& title,
                             const core::Rect& frame, const std::string& window_class = {}) {
    core::WindowInfo window;
    window.id = app_id + "|" + title;
    window.title = title;
    window.window_class = window_class.empty() ? app_id : window_class;
    window.frame = frame;
    window.client_area = frame;
    window.restored_frame = frame;
    window.monitor_id = "monitor-0";
    window.process.app_id = app_id;
    window.process.executable_path = "/usr/bin/" + app_id;
    return window;
}

}  // namespace

TEST("app ids normalise across platforms") {
    CHECK_EQ(matching::normalize_app_id("C:\\Program Files\\Mozilla Firefox\\firefox.exe"),
             std::string("firefox"));
    CHECK_EQ(matching::normalize_app_id("/usr/lib/firefox/firefox"), std::string("firefox"));
    CHECK_EQ(matching::normalize_app_id("/Applications/Visual Studio Code.app"),
             std::string("visual studio code"));
}

TEST("titles lose volatile decorations") {
    const double similar =
        matching::title_similarity("\u25cf main.cpp \u2014 contextsnap", "main.cpp - contextsnap");
    CHECK(similar > 0.8);

    const double unrelated = matching::title_similarity("Inbox \u2014 Gmail", "Terminal");
    CHECK(unrelated < 0.3);

    // Unread counters must not break the match.
    CHECK(matching::title_similarity("(3) Inbox - Gmail", "Inbox - Gmail") > 0.75);
}

TEST("geometry proximity decays with distance") {
    const core::Rect a{0, 0, 1280, 800};
    CHECK_NEAR(matching::geometry_proximity(a, a), 1.0, 1e-9);

    const double nearby = matching::geometry_proximity(a, core::Rect{16, 16, 1280, 800});
    const double far = matching::geometry_proximity(a, core::Rect{2400, 900, 400, 300});
    CHECK(nearby > far);
    CHECK(far >= 0.0);
    CHECK(nearby <= 1.0);
}

TEST("identical windows score higher than merely similar ones") {
    const core::WindowInfo target = make_window("code", "main.cpp - contextsnap", {0, 0, 1280, 800});
    const core::WindowInfo same = make_window("code", "main.cpp - contextsnap", {0, 0, 1280, 800});
    const core::WindowInfo other = make_window("code", "README.md - notes", {900, 100, 600, 400});
    const core::WindowInfo different = make_window("terminal", "zsh", {0, 0, 900, 600});

    CHECK(matching::score(target, same) > matching::score(target, other));
    CHECK(matching::score(target, other) > matching::score(target, different));
    CHECK(matching::score(target, same) > matching::kDefaultThreshold);
}

TEST("greedy assignment pairs each window at most once") {
    const std::vector<core::WindowInfo> snapshot{
        make_window("code", "main.cpp - contextsnap", {0, 0, 1280, 800}),
        make_window("firefox", "Docs - Mozilla Firefox", {1280, 0, 1280, 800}),
        make_window("slack", "Slack | general", {200, 200, 900, 700}),
    };
    const std::vector<core::WindowInfo> live{
        make_window("firefox", "Docs - Mozilla Firefox", {1290, 10, 1280, 800}),
        make_window("code", "main.cpp - contextsnap", {0, 0, 1280, 800}),
    };

    const matching::MatchResult result = matching::match_windows(snapshot, live);
    CHECK_EQ(result.matches.size(), std::size_t(2));
    CHECK_EQ(result.unmatched_snapshot.size(), std::size_t(1));
    CHECK(result.unmatched_live.empty());

    std::vector<std::size_t> live_indices;
    for (const matching::Match& match : result.matches) {
        live_indices.push_back(match.live_index);
        CHECK(match.score >= matching::kDefaultThreshold);
    }
    CHECK_NE(live_indices[0], live_indices[1]);
}

TEST("single-instance applications are recognised") {
    CHECK(matching::is_single_instance_app("firefox"));
    CHECK(matching::is_single_instance_app("google chrome"));
    CHECK_FALSE(matching::is_single_instance_app("alacritty"));
}

TEST("browser detection routes tab capture") {
    CHECK_EQ(matching::detect_browser("firefox", "Navigator"), core::BrowserKind::Firefox);
    CHECK_EQ(matching::detect_browser("/usr/bin/chromium", "chromium-browser"),
             core::BrowserKind::Chromium);
    CHECK_EQ(matching::detect_browser("alacritty", "Alacritty"), core::BrowserKind::None);
}

TEST("launch commands keep the executable first") {
    core::ProcessInfo process;
    process.executable_path = "/usr/bin/code";
    process.app_id = "code";
    process.command_line = {"/usr/bin/code", "--new-window"};
    process.working_directory = "/home/user/project";

    const std::vector<std::string> command =
        matching::build_launch_command(process, core::PlatformKind::Linux);
    CHECK_FALSE(command.empty());
    CHECK(command.front().find("code") != std::string::npos);
}

MICROTEST_MAIN()
