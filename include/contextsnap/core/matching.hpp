// Window/process matching heuristics.
//
// Restoration is a bipartite matching problem: N windows in the snapshot, M
// windows alive right now. Native handles are useless across reboots, so we
// score candidates on stable attributes and greedily assign the best pairs
// above a confidence threshold. Everything here is pure and unit tested
// (tests/unit/test_matching.cpp) — no OS calls.
#pragma once

#include <contextsnap/core/types.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::core::matching {

/// Relative contribution of each signal. Weights sum to 1.0.
struct Weights {
    double app_id{0.40};
    double executable_path{0.20};
    double window_class{0.15};
    double title_similarity{0.15};
    double geometry_proximity{0.05};
    double workspace{0.05};
};

inline constexpr double kDefaultThreshold = 0.55;

/// Case- and separator-insensitive normalization of an application identifier:
/// "C:\\Program Files\\Mozilla Firefox\\firefox.exe" -> "firefox",
/// "/Applications/Visual Studio Code.app" -> "visual studio code".
[[nodiscard]] std::string normalize_app_id(std::string_view raw);

/// Strips volatile decorations that browsers and editors add to titles:
/// "● main.cpp — contextsnap" -> "main.cpp contextsnap",
/// "(3) Inbox — Gmail" -> "inbox gmail".
[[nodiscard]] std::string normalize_title(std::string_view raw);

/// Token-set similarity in [0,1]; order-insensitive, robust to counters and
/// document-modified markers.
[[nodiscard]] double title_similarity(std::string_view a, std::string_view b);

/// Normalized Damerau-Levenshtein similarity in [0,1], used as a tiebreak.
[[nodiscard]] double string_similarity(std::string_view a, std::string_view b);

/// 1.0 when the frames coincide, decaying with centre distance and size delta.
[[nodiscard]] double geometry_proximity(const Rect& a, const Rect& b) noexcept;

/// Composite score in [0,1] for "is `candidate` the same window as `target`".
[[nodiscard]] double score(const WindowInfo& target,
                           const WindowInfo& candidate,
                           const Weights& weights = {});

struct Match {
    std::size_t snapshot_index{0};
    std::size_t live_index{0};
    double score{0.0};
};

struct MatchResult {
    std::vector<Match> matches;
    std::vector<std::size_t> unmatched_snapshot;  ///< Need launching or a new window.
    std::vector<std::size_t> unmatched_live;      ///< Extra windows; left alone by default.
};

/// Greedy maximum-score assignment. O(n*m log(n*m)); n,m are window counts in
/// the tens, so the simplicity beats Hungarian-algorithm optimality here.
[[nodiscard]] MatchResult match_windows(const std::vector<WindowInfo>& snapshot_windows,
                                        const std::vector<WindowInfo>& live_windows,
                                        double threshold = kDefaultThreshold,
                                        const Weights& weights = {});

/// Applications that focus an existing instance instead of spawning a new
/// window when launched again (browsers, Finder/Explorer, most Electron apps).
[[nodiscard]] bool is_single_instance_app(std::string_view app_id) noexcept;

/// Maps an executable/app id onto a BrowserKind for tab-capture routing.
[[nodiscard]] BrowserKind detect_browser(std::string_view app_id,
                                         std::string_view window_class) noexcept;

/// Best-effort command line for relaunching a process, with per-platform
/// quirks applied (e.g. `open -na` on macOS, `--new-window` for browsers).
[[nodiscard]] std::vector<std::string> build_launch_command(const ProcessInfo& process,
                                                            PlatformKind platform);

}  // namespace contextsnap::core::matching
