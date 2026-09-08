#include <contextsnap/core/types.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace contextsnap::core {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

std::string_view to_string(WindowState state) noexcept {
    switch (state) {
        case WindowState::Normal:
            return "normal";
        case WindowState::Minimized:
            return "minimized";
        case WindowState::Maximized:
            return "maximized";
        case WindowState::Fullscreen:
            return "fullscreen";
        case WindowState::Hidden:
            break;
    }
    return "hidden";
}

std::string_view to_string(PlatformKind kind) noexcept {
    switch (kind) {
        case PlatformKind::Windows:
            return "windows";
        case PlatformKind::MacOS:
            return "macos";
        case PlatformKind::Linux:
            return "linux";
        case PlatformKind::Unknown:
            break;
    }
    return "unknown";
}

std::string_view to_string(SessionType type) noexcept {
    switch (type) {
        case SessionType::Win32:
            return "win32";
        case SessionType::Quartz:
            return "quartz";
        case SessionType::X11:
            return "x11";
        case SessionType::Wayland:
            return "wayland";
        case SessionType::Unknown:
            break;
    }
    return "unknown";
}

std::string_view to_string(BrowserKind kind) noexcept {
    switch (kind) {
        case BrowserKind::None:
            return "none";
        case BrowserKind::Chrome:
            return "chrome";
        case BrowserKind::Chromium:
            return "chromium";
        case BrowserKind::Edge:
            return "edge";
        case BrowserKind::Brave:
            return "brave";
        case BrowserKind::Vivaldi:
            return "vivaldi";
        case BrowserKind::Opera:
            return "opera";
        case BrowserKind::Firefox:
            return "firefox";
        case BrowserKind::LibreWolf:
            return "librewolf";
        case BrowserKind::Safari:
            return "safari";
        case BrowserKind::Other:
            break;
    }
    return "other";
}

WindowState window_state_from_string(std::string_view text) noexcept {
    const std::string value = lower(text);
    if (value == "minimized" || value == "iconic") {
        return WindowState::Minimized;
    }
    if (value == "maximized" || value == "zoomed") {
        return WindowState::Maximized;
    }
    if (value == "fullscreen") {
        return WindowState::Fullscreen;
    }
    if (value == "hidden") {
        return WindowState::Hidden;
    }
    return WindowState::Normal;
}

BrowserKind browser_kind_from_string(std::string_view text) noexcept {
    const std::string value = lower(text);
    if (value.empty() || value == "none") {
        return BrowserKind::None;
    }
    if (value == "chrome" || value == "google chrome") {
        return BrowserKind::Chrome;
    }
    if (value == "chromium") {
        return BrowserKind::Chromium;
    }
    if (value == "edge" || value == "msedge" || value == "microsoft edge") {
        return BrowserKind::Edge;
    }
    if (value == "brave" || value == "brave-browser") {
        return BrowserKind::Brave;
    }
    if (value == "vivaldi") {
        return BrowserKind::Vivaldi;
    }
    if (value == "opera") {
        return BrowserKind::Opera;
    }
    if (value == "firefox" || value == "mozilla firefox") {
        return BrowserKind::Firefox;
    }
    if (value == "librewolf") {
        return BrowserKind::LibreWolf;
    }
    if (value == "safari") {
        return BrowserKind::Safari;
    }
    return BrowserKind::Other;
}

}  // namespace contextsnap::core
