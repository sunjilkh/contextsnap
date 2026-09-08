#include <contextsnap/core/matching.hpp>

#include <contextsnap/core/geometry.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <unordered_set>

namespace contextsnap::core::matching {
namespace {

bool is_separator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '-' || c == '_' || c == '.' || c == ':' || c == '/' ||
           c == '\\' || c == '|' || c == ',' || c == '(' || c == ')' || c == '[' || c == ']';
}

std::string collapse_spaces(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char c : text) {
        if (c == ' ') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : text) {
        if (c == ' ') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string normalize_app_id(std::string_view raw) {
    if (raw.empty()) {
        return {};
    }
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // Reverse-DNS bundle ids ("com.microsoft.vscode") identify by their last
    // component; anything else is treated as a path.
    const bool reverse_dns = value.find('/') == std::string::npos &&
                             value.find('\\') == std::string::npos &&
                             std::count(value.begin(), value.end(), '.') >= 2 &&
                             !ends_with(value, ".exe");
    if (reverse_dns) {
        const std::size_t dot = value.find_last_of('.');
        value = value.substr(dot + 1);
    } else {
        const std::size_t slash = value.find_last_of("/\\");
        if (slash != std::string::npos) {
            // "/applications/visual studio code.app/contents/macos/electron"
            // should normalize to the bundle, not the helper binary.
            const std::size_t app = value.find(".app/");
            if (app != std::string::npos) {
                const std::size_t start = value.find_last_of("/\\", app);
                value = value.substr(start == std::string::npos ? 0 : start + 1,
                                     app - (start == std::string::npos ? 0 : start + 1));
            } else {
                value = value.substr(slash + 1);
            }
        }
        for (const std::string_view suffix : {".exe", ".app", ".desktop", ".bin"}) {
            if (ends_with(value, suffix)) {
                value.resize(value.size() - suffix.size());
                break;
            }
        }
    }

    std::string cleaned;
    cleaned.reserve(value.size());
    for (const char c : value) {
        cleaned.push_back(is_separator(c) ? ' ' : c);
    }
    return collapse_spaces(cleaned);
}

std::string normalize_title(std::string_view raw) {
    std::string value;
    value.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x80) {
            value.push_back(static_cast<char>(std::tolower(c)));
        } else {
            // Multi-byte UTF-8 (em dash, bullet, CJK): keep the bytes, they are
            // compared verbatim. Only the common separators are folded below.
            value.push_back(raw[i]);
        }
    }

    // Drop leading unread-counters like "(3) " and modified markers.
    for (const std::string_view marker : {"\xe2\x97\x8f", "\xe2\x80\xa2", "\xe2\x80\x94",
                                          "\xe2\x80\x93"}) {
        std::size_t pos = value.find(marker);
        while (pos != std::string::npos) {
            value.replace(pos, marker.size(), " ");
            pos = value.find(marker, pos + 1);
        }
    }
    if (!value.empty() && value.front() == '(') {
        const std::size_t close = value.find(')');
        if (close != std::string::npos && close <= 4) {
            value = value.substr(close + 1);
        }
    }

    std::string cleaned;
    cleaned.reserve(value.size());
    for (const char c : value) {
        if (c == '*' || c == '\xe2') {
            continue;
        }
        cleaned.push_back(is_separator(c) ? ' ' : c);
    }
    return collapse_spaces(cleaned);
}

double title_similarity(std::string_view a, std::string_view b) {
    const std::vector<std::string> left = tokenize(normalize_title(a));
    const std::vector<std::string> right = tokenize(normalize_title(b));
    if (left.empty() && right.empty()) {
        return 1.0;
    }
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    const std::set<std::string> left_set(left.begin(), left.end());
    const std::set<std::string> right_set(right.begin(), right.end());
    std::size_t intersection = 0;
    for (const auto& token : left_set) {
        if (right_set.count(token) != 0) {
            ++intersection;
        }
    }
    const std::size_t union_size = left_set.size() + right_set.size() - intersection;
    const double jaccard = static_cast<double>(intersection) / static_cast<double>(union_size);
    // A shared suffix ("... - Visual Studio Code") is a strong app-level hint,
    // so blend in character similarity to avoid punishing long documents.
    return 0.75 * jaccard + 0.25 * string_similarity(normalize_title(a), normalize_title(b));
}

double string_similarity(std::string_view a, std::string_view b) {
    if (a == b) {
        return 1.0;
    }
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    // Optimal string alignment (Damerau-Levenshtein without full transposition
    // support) over two rolling rows: O(min(n,m)) memory.
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    std::vector<std::size_t> previous(m + 1);
    std::vector<std::size_t> current(m + 1);
    std::vector<std::size_t> before_previous(m + 1);
    for (std::size_t j = 0; j <= m; ++j) {
        previous[j] = j;
    }
    for (std::size_t i = 1; i <= n; ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            std::size_t value = std::min({current[j - 1] + 1, previous[j] + 1, previous[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                value = std::min(value, before_previous[j - 2] + 1);
            }
            current[j] = value;
        }
        before_previous = previous;
        previous = current;
    }
    const double distance = static_cast<double>(previous[m]);
    const double longest = static_cast<double>(std::max(n, m));
    return 1.0 - (distance / longest);
}

double geometry_proximity(const Rect& a, const Rect& b) noexcept {
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    const double overlap = static_cast<double>(geometry::intersection_area(a, b));
    const double union_area = static_cast<double>(a.area() + b.area()) - overlap;
    if (union_area <= 0.0) {
        return 0.0;
    }
    const double iou = overlap / union_area;

    const double dx = (a.x + a.width / 2.0) - (b.x + b.width / 2.0);
    const double dy = (a.y + a.height / 2.0) - (b.y + b.height / 2.0);
    const double distance = std::sqrt(dx * dx + dy * dy);
    const double diagonal = std::sqrt(static_cast<double>(a.width * a.width + a.height * a.height));
    const double closeness = diagonal > 0.0 ? std::max(0.0, 1.0 - distance / diagonal) : 0.0;
    return 0.6 * iou + 0.4 * closeness;
}

double score(const WindowInfo& target, const WindowInfo& candidate, const Weights& weights) {
    const std::string target_app = normalize_app_id(
        target.process.app_id.empty() ? target.process.executable_path : target.process.app_id);
    const std::string candidate_app = normalize_app_id(candidate.process.app_id.empty()
                                                           ? candidate.process.executable_path
                                                           : candidate.process.app_id);

    // Different applications are never the same window, whatever else matches.
    const bool same_app = !target_app.empty() && target_app == candidate_app;
    if (!same_app && !target_app.empty() && !candidate_app.empty() &&
        string_similarity(target_app, candidate_app) < 0.8) {
        return 0.0;
    }

    double total = 0.0;
    total += weights.app_id * (same_app ? 1.0 : string_similarity(target_app, candidate_app));
    total += weights.executable_path *
             (target.process.executable_path == candidate.process.executable_path
                  ? 1.0
                  : string_similarity(normalize_app_id(target.process.executable_path),
                                      normalize_app_id(candidate.process.executable_path)));
    total += weights.window_class *
             (target.window_class == candidate.window_class
                  ? 1.0
                  : string_similarity(target.window_class, candidate.window_class));
    total += weights.title_similarity * title_similarity(target.title, candidate.title);
    total += weights.geometry_proximity * geometry_proximity(target.frame, candidate.frame);

    const bool same_workspace = target.workspace_id.value_or("") == candidate.workspace_id.value_or("");
    total += weights.workspace * (same_workspace ? 1.0 : 0.0);

    // Identical application plus identical title is a certainty; identical app
    // alone should still clear the default threshold for single-window apps.
    if (same_app && target.title == candidate.title && !target.title.empty()) {
        return 1.0;
    }
    if (same_app) {
        total = std::max(total, 0.60);
    }
    return std::clamp(total, 0.0, 1.0);
}

MatchResult match_windows(const std::vector<WindowInfo>& snapshot_windows,
                          const std::vector<WindowInfo>& live_windows,
                          double threshold,
                          const Weights& weights) {
    struct Candidate {
        std::size_t snapshot_index;
        std::size_t live_index;
        double value;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(snapshot_windows.size() * live_windows.size());
    for (std::size_t i = 0; i < snapshot_windows.size(); ++i) {
        for (std::size_t j = 0; j < live_windows.size(); ++j) {
            const double value = score(snapshot_windows[i], live_windows[j], weights);
            if (value >= threshold) {
                candidates.push_back(Candidate{i, j, value});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.value != b.value) {
            return a.value > b.value;
        }
        if (a.snapshot_index != b.snapshot_index) {
            return a.snapshot_index < b.snapshot_index;  // Deterministic ordering.
        }
        return a.live_index < b.live_index;
    });

    MatchResult result;
    std::vector<bool> snapshot_taken(snapshot_windows.size(), false);
    std::vector<bool> live_taken(live_windows.size(), false);
    for (const auto& candidate : candidates) {
        if (snapshot_taken[candidate.snapshot_index] || live_taken[candidate.live_index]) {
            continue;
        }
        snapshot_taken[candidate.snapshot_index] = true;
        live_taken[candidate.live_index] = true;
        result.matches.push_back(Match{candidate.snapshot_index, candidate.live_index, candidate.value});
    }
    for (std::size_t i = 0; i < snapshot_taken.size(); ++i) {
        if (!snapshot_taken[i]) {
            result.unmatched_snapshot.push_back(i);
        }
    }
    for (std::size_t j = 0; j < live_taken.size(); ++j) {
        if (!live_taken[j]) {
            result.unmatched_live.push_back(j);
        }
    }
    return result;
}

bool is_single_instance_app(std::string_view app_id) noexcept {
    static const std::unordered_set<std::string> kSingleInstance{
        "chrome",  "chromium", "msedge",   "edge",     "brave",   "brave browser",
        "google chrome", "google-chrome", "google-chrome-stable", "microsoft edge",
        "mozilla firefox", "firefox-esr",
        "vivaldi", "opera",    "firefox",  "librewolf", "safari", "finder",
        "explorer", "nautilus", "dolphin", "thunar",   "code",    "visual studio code",
        "slack",   "discord",  "spotify", "obsidian", "notion",  "figma",
        "postman", "docker desktop", "steam",
    };
    return kSingleInstance.count(normalize_app_id(app_id)) != 0;
}

BrowserKind detect_browser(std::string_view app_id, std::string_view window_class) noexcept {
    const std::string normalized = normalize_app_id(app_id);
    const BrowserKind direct = browser_kind_from_string(normalized);
    if (direct != BrowserKind::None && direct != BrowserKind::Other) {
        return direct;
    }
    if (normalized == "brave browser" || normalized == "brave browser beta") {
        return BrowserKind::Brave;
    }
    if (normalized == "google chrome" || normalized == "google chrome canary") {
        return BrowserKind::Chrome;
    }
    if (normalized == "microsoft edge") {
        return BrowserKind::Edge;
    }
    const std::string klass = normalize_app_id(window_class);
    if (!klass.empty() && klass != normalized) {
        const BrowserKind by_class = browser_kind_from_string(klass);
        if (by_class != BrowserKind::None && by_class != BrowserKind::Other) {
            return by_class;
        }
        if (klass == "navigator" || klass == "mozilla firefox") {
            return BrowserKind::Firefox;
        }
    }
    return BrowserKind::None;
}

std::vector<std::string> build_launch_command(const ProcessInfo& process, PlatformKind platform) {
    std::vector<std::string> command;
    if (process.executable_path.empty() && process.app_id.empty()) {
        return command;
    }

    const std::string normalized = normalize_app_id(
        process.app_id.empty() ? process.executable_path : process.app_id);
    const bool browser = detect_browser(normalized, {}) != BrowserKind::None;

    if (platform == PlatformKind::MacOS) {
        // `open -na` starts a *new* instance of a bundle and detaches it, which
        // is what restoring a second window of a single-instance app needs.
        command.push_back("open");
        command.push_back("-na");
        command.push_back(process.executable_path.empty() ? process.app_id
                                                          : process.executable_path);
        if (!process.command_line.empty() || browser) {
            command.push_back("--args");
        }
    } else {
        command.push_back(process.executable_path.empty() ? process.app_id
                                                          : process.executable_path);
    }

    for (const auto& argument : process.command_line) {
        // Never replay one-shot arguments that would reopen a crash dialog or
        // attach to a dead debugger session.
        if (argument.rfind("--crash", 0) == 0 || argument.rfind("--remote-debugging", 0) == 0) {
            continue;
        }
        command.push_back(argument);
    }

    if (browser) {
        const bool has_new_window =
            std::find(command.begin(), command.end(), "--new-window") != command.end();
        if (!has_new_window) {
            command.push_back("--new-window");
        }
    }
    return command;
}

}  // namespace contextsnap::core::matching
