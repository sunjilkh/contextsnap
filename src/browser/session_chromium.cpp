// Passive fallback: read Chromium/Chrome/Edge/Brave "Current Session" SNSS
// files when no extension is connected. This is best-effort by design; the
// extension path is always preferred.
#include <chrono>
#include <contextsnap/browser/session_parsers.hpp>

#include <contextsnap/core/logging.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace contextsnap::browser::session {
namespace {

namespace fs = std::filesystem;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

std::string home_dir() {
#if defined(_WIN32)
    const std::string profile = env("USERPROFILE");
    return profile.empty() ? env("HOMEDRIVE") + env("HOMEPATH") : profile;
#else
    const std::string home = env("HOME");
    return home;
#endif
}

/// Root directories that may contain Chromium-family user data.
std::vector<std::pair<core::BrowserKind, std::string>> chromium_roots() {
    const std::string home = home_dir();
    std::vector<std::pair<core::BrowserKind, std::string>> roots;
    if (home.empty()) {
        return roots;
    }
#if defined(_WIN32)
    const std::string local = env("LOCALAPPDATA");
    roots.emplace_back(core::BrowserKind::Chrome, local + "\\Google\\Chrome\\User Data");
    roots.emplace_back(core::BrowserKind::Edge, local + "\\Microsoft\\Edge\\User Data");
    roots.emplace_back(core::BrowserKind::Brave,
                       local + "\\BraveSoftware\\Brave-Browser\\User Data");
    roots.emplace_back(core::BrowserKind::Chromium, local + "\\Chromium\\User Data");
#elif defined(__APPLE__)
    const std::string support = home + "/Library/Application Support";
    roots.emplace_back(core::BrowserKind::Chrome, support + "/Google/Chrome");
    roots.emplace_back(core::BrowserKind::Edge, support + "/Microsoft Edge");
    roots.emplace_back(core::BrowserKind::Brave, support + "/BraveSoftware/Brave-Browser");
    roots.emplace_back(core::BrowserKind::Chromium, support + "/Chromium");
#else
    const std::string config = env("XDG_CONFIG_HOME").empty() ? home + "/.config" : env("XDG_CONFIG_HOME");
    roots.emplace_back(core::BrowserKind::Chrome, config + "/google-chrome");
    roots.emplace_back(core::BrowserKind::Chromium, config + "/chromium");
    roots.emplace_back(core::BrowserKind::Edge, config + "/microsoft-edge");
    roots.emplace_back(core::BrowserKind::Brave, config + "/BraveSoftware/Brave-Browser");
    // Flatpak installs keep their own copy of the profile tree.
    roots.emplace_back(core::BrowserKind::Chromium,
                       home + "/.var/app/org.chromium.Chromium/config/chromium");
#endif
    return roots;
}

std::vector<std::string> firefox_roots() {
    const std::string home = home_dir();
    if (home.empty()) {
        return {};
    }
#if defined(_WIN32)
    return {env("APPDATA") + "\\Mozilla\\Firefox\\Profiles"};
#elif defined(__APPLE__)
    return {home + "/Library/Application Support/Firefox/Profiles"};
#else
    return {home + "/.mozilla/firefox", home + "/snap/firefox/common/.mozilla/firefox",
            home + "/.var/app/org.mozilla.firefox/.mozilla/firefox"};
#endif
}

bool is_profile_dir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) &&
           (fs::exists(path / "Preferences", ec) || fs::exists(path / "Sessions", ec));
}

/// Little-endian readers over a byte buffer with bounds checks.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] bool read_u32(std::uint32_t& out) {
        if (offset_ + 4 > size_) return false;
        out = static_cast<std::uint32_t>(data_[offset_]) |
              (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8) |
              (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16) |
              (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool read_i32(std::int32_t& out) {
        std::uint32_t value = 0;
        if (!read_u32(value)) return false;
        out = static_cast<std::int32_t>(value);
        return true;
    }

    /// Pickle strings are length-prefixed and padded to a 4-byte boundary.
    [[nodiscard]] bool read_string(std::string& out) {
        std::uint32_t length = 0;
        if (!read_u32(length) || length > size_ - offset_) return false;
        out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        align();
        return true;
    }

    /// UTF-16LE string, converted to UTF-8 (BMP subset plus surrogate pairs).
    [[nodiscard]] bool read_string16(std::string& out) {
        std::uint32_t units = 0;
        if (!read_u32(units) || units * 2 > size_ - offset_) return false;
        out.clear();
        out.reserve(units);
        for (std::uint32_t i = 0; i < units; ++i) {
            std::uint32_t code = static_cast<std::uint32_t>(data_[offset_]) |
                                 (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8);
            offset_ += 2;
            if (code >= 0xD800 && code <= 0xDBFF && i + 1 < units) {
                const std::uint32_t low = static_cast<std::uint32_t>(data_[offset_]) |
                                          (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    offset_ += 2;
                    ++i;
                }
            }
            if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }
        align();
        return true;
    }

    void align() { offset_ = (offset_ + 3) & ~static_cast<std::size_t>(3); }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
};

constexpr std::uint8_t kCommandUpdateTabNavigation = 6;
constexpr std::uint8_t kCommandSetSelectedNavigationIndex = 7;
constexpr std::uint8_t kCommandSetActiveWindow = 20;
constexpr std::uint8_t kCommandSetTabWindow = 0;

Result<std::vector<std::uint8_t>> read_file_bytes(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::err::io("cannot open session file: " + path, "browser.session");
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

Result<std::vector<SnssCommand>> parse_snss(const std::vector<std::uint8_t>& data) {
    // Header: "SNSS" magic + int32 version, then <uint16 size><uint8 id><payload>.
    if (data.size() < 8 || std::memcmp(data.data(), "SNSS", 4) != 0) {
        return core::err::protocol("not an SNSS session file", "browser.session");
    }
    std::vector<SnssCommand> commands;
    std::size_t offset = 8;
    while (offset + 3 <= data.size()) {
        const std::size_t size = static_cast<std::size_t>(data[offset]) |
                                 (static_cast<std::size_t>(data[offset + 1]) << 8);
        offset += 2;
        if (size == 0 || offset + size > data.size()) {
            break;
        }
        SnssCommand command;
        command.id = data[offset];
        command.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(offset + 1),
                               data.begin() + static_cast<std::ptrdiff_t>(offset + size));
        commands.push_back(std::move(command));
        offset += size;
    }
    if (commands.empty()) {
        return core::err::protocol("session file contained no commands", "browser.session");
    }
    return commands;
}

Result<std::string> newest_chromium_session_file(const std::string& profile_path) {
    std::error_code ec;
    const fs::path sessions = fs::path(profile_path) / "Sessions";
    if (!fs::is_directory(sessions, ec)) {
        return core::err::not_found("no Sessions directory in " + profile_path, "browser.session");
    }
    fs::path newest;
    fs::file_time_type newest_time{};
    for (const fs::directory_entry& entry : fs::directory_iterator(sessions, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("Session_", 0) != 0 && name != "Current Session") {
            continue;
        }
        const auto written = entry.last_write_time(ec);
        if (!ec && (newest.empty() || written > newest_time)) {
            newest = entry.path();
            newest_time = written;
        }
    }
    if (newest.empty()) {
        return core::err::not_found("no session file in " + sessions.string(), "browser.session");
    }
    return newest.string();
}

Result<std::vector<BrowserWindow>> read_chromium_session(const std::string& path,
                                                         core::BrowserKind browser,
                                                         const std::string& profile) {
    auto bytes = read_file_bytes(path);
    if (!bytes) {
        return bytes.error();
    }
    auto commands = parse_snss(bytes.value());
    if (!commands) {
        return commands.error();
    }

    struct TabState {
        std::int32_t window_id{0};
        std::int32_t selected_index{0};
        std::map<std::int32_t, std::pair<std::string, std::string>> entries;  // index -> url,title
    };
    std::map<std::int32_t, TabState> tabs;
    std::int32_t active_window = 0;

    for (const SnssCommand& command : commands.value()) {
        Reader reader(command.payload.data(), command.payload.size());
        if (command.id == kCommandUpdateTabNavigation) {
            std::uint32_t pickle_size = 0;
            std::int32_t tab_id = 0;
            std::int32_t index = 0;
            std::string url;
            std::string title;
            if (!reader.read_u32(pickle_size) || !reader.read_i32(tab_id) ||
                !reader.read_i32(index) || !reader.read_string(url)) {
                continue;
            }
            (void)reader.read_string16(title);
            if (url.rfind("http", 0) != 0 && url.rfind("file", 0) != 0 &&
                url.rfind("chrome", 0) != 0) {
                continue;
            }
            tabs[tab_id].entries[index] = {url, title};
        } else if (command.id == kCommandSetSelectedNavigationIndex) {
            std::int32_t tab_id = 0;
            std::int32_t index = 0;
            if (reader.read_i32(tab_id) && reader.read_i32(index)) {
                tabs[tab_id].selected_index = index;
            }
        } else if (command.id == kCommandSetTabWindow) {
            std::int32_t window_id = 0;
            std::int32_t tab_id = 0;
            if (reader.read_i32(window_id) && reader.read_i32(tab_id)) {
                tabs[tab_id].window_id = window_id;
            }
        } else if (command.id == kCommandSetActiveWindow) {
            (void)reader.read_i32(active_window);
        }
    }

    std::map<std::int32_t, BrowserWindow> windows;
    for (const auto& entry : tabs) {
        const TabState& state = entry.second;
        if (state.entries.empty()) {
            continue;
        }
        // Prefer the selected navigation entry, else the newest one.
        auto navigation = state.entries.find(state.selected_index);
        if (navigation == state.entries.end()) {
            navigation = std::prev(state.entries.end());
        }
        BrowserWindow& window = windows[state.window_id];
        window.browser = browser;
        window.profile = profile;
        window.extension_window_id = state.window_id;
        window.focused = state.window_id == active_window;

        core::TabInfo tab;
        tab.id = static_cast<std::int64_t>(entry.first);
        tab.index = static_cast<std::int32_t>(window.tabs.size());
        tab.url = navigation->second.first;
        tab.title = navigation->second.second;
        tab.active = false;
        window.tabs.push_back(std::move(tab));
    }

    std::vector<BrowserWindow> result;
    result.reserve(windows.size());
    for (auto& entry : windows) {
        if (!entry.second.tabs.empty()) {
            entry.second.tabs.front().active = true;
            result.push_back(std::move(entry.second));
        }
    }
    if (result.empty()) {
        return core::err::not_found("session file had no restorable tabs", "browser.session");
    }
    return result;
}

std::vector<ProfileLocation> discover_profiles() {
    std::error_code ec;
    std::vector<ProfileLocation> locations;

    for (const auto& root : chromium_roots()) {
        if (!fs::is_directory(root.second, ec)) {
            continue;
        }
        for (const fs::directory_entry& entry : fs::directory_iterator(root.second, ec)) {
            if (!is_profile_dir(entry.path())) {
                continue;
            }
            ProfileLocation location;
            location.browser = root.first;
            location.profile_name = entry.path().filename().string();
            location.profile_path = entry.path().string();
            if (auto session = newest_chromium_session_file(location.profile_path); session) {
                location.session_file = session.value();
                location.readable = true;
            }
            locations.push_back(std::move(location));
        }
    }

    for (const std::string& root : firefox_roots()) {
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)) {
            if (entry.path().filename() != "recovery.jsonlz4") {
                continue;
            }
            ProfileLocation location;
            location.browser = core::BrowserKind::Firefox;
            location.profile_path = entry.path().parent_path().parent_path().string();
            location.profile_name = fs::path(location.profile_path).filename().string();
            location.session_file = entry.path().string();
            location.readable = true;
            locations.push_back(std::move(location));
        }
    }
    return locations;
}

TabCapture capture_from_session_files() {
    TabCapture capture;
    capture.source = Source::SessionFile;
    const auto started = std::chrono::steady_clock::now();

    for (const ProfileLocation& location : discover_profiles()) {
        if (!location.readable || location.session_file.empty()) {
            continue;
        }
        Result<std::vector<BrowserWindow>> windows =
            location.browser == core::BrowserKind::Firefox
                ? read_firefox_session(location.session_file, location.profile_name)
                : read_chromium_session(location.session_file, location.browser,
                                        location.profile_name);
        if (!windows) {
            capture.warnings.push_back(location.session_file + ": " +
                                       windows.error().message);
            continue;
        }
        for (BrowserWindow& window : windows.value()) {
            capture.windows.push_back(std::move(window));
        }
    }
    if (capture.windows.empty()) {
        capture.source = Source::None;
    }
    capture.duration_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started)
            .count());
    return capture;
}

}  // namespace contextsnap::browser::session
