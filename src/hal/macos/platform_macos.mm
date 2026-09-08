// macOS HAL: CoreGraphics window list for capture, Accessibility for control.
//
// Two frameworks are needed because they answer different questions:
//   * CGWindowListCopyWindowInfo enumerates every on-screen window cheaply and
//     needs no permission for geometry (titles need Screen Recording).
//   * AXUIElement is the only public way to *move* another app's window, and
//     that requires the Accessibility (TCC) grant.
#if defined(__APPLE__)

#include <contextsnap/core/geometry.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/matching.hpp>
#include <contextsnap/hal/platform.hpp>

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <libproc.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstring>

#include <algorithm>
#include <thread>

namespace contextsnap::hal {
namespace {

using core::CaptureOptions;
using core::CursorState;
using core::MonitorInfo;
using core::ProcessInfo;
using core::Rect;
using core::Result;
using core::Status;
using core::WindowInfo;
using core::WindowState;

std::string to_std(NSString* text) {
    return text == nil ? std::string{} : std::string([text UTF8String]);
}

/// CoreGraphics uses a top-left origin on the primary display, while AppKit
/// uses bottom-left. Everything inside ContextSnap stores CG (top-left)
/// coordinates; this converts an AppKit rect into that space.
Rect from_appkit(NSRect rect, CGFloat primary_height) {
    return Rect{static_cast<std::int32_t>(rect.origin.x),
                static_cast<std::int32_t>(primary_height - rect.origin.y - rect.size.height),
                static_cast<std::int32_t>(rect.size.width),
                static_cast<std::int32_t>(rect.size.height)};
}

CGFloat primary_display_height() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count == 0 ? 0 : screens[0].frame.size.height;
}

bool accessibility_trusted() { return AXIsProcessTrusted() == TRUE; }

ProcessInfo read_process(pid_t pid) {
    ProcessInfo info;
    info.pid = static_cast<std::uint64_t>(pid);

    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath(pid, path, sizeof(path)) > 0) {
        info.executable_path = path;
    }

    // KERN_PROCARGS2 returns argc followed by NUL-separated argv and the
    // environment; only same-user processes are readable, which is what we want.
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    std::size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) == 0 && size > sizeof(int)) {
        std::string buffer(size, '\0');
        if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) == 0) {
            int argc = 0;
            std::memcpy(&argc, buffer.data(), sizeof(argc));
            std::size_t cursor = sizeof(argc);
            while (cursor < size && buffer[cursor] != '\0') ++cursor;   // exec path
            while (cursor < size && buffer[cursor] == '\0') ++cursor;   // padding
            for (int index = 0; index < argc && cursor < size; ++index) {
                const std::string argument(buffer.c_str() + cursor);
                info.command_line.push_back(argument);
                cursor += argument.size() + 1;
            }
        }
    }

    NSRunningApplication* application =
        [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (application != nil) {
        info.app_id = to_std(application.bundleIdentifier);
    }
    if (info.app_id.empty()) {
        info.app_id = core::matching::normalize_app_id(info.executable_path);
    }
    info.single_instance = core::matching::is_single_instance_app(info.app_id);
    info.user = to_std(NSUserName());
    info.elevated = geteuid() == 0;
    return info;
}

AXUIElementRef window_element(std::uint64_t window_number, pid_t pid) {
    AXUIElementRef application = AXUIElementCreateApplication(pid);
    if (application == nullptr) {
        return nullptr;
    }
    CFArrayRef windows = nullptr;
    if (AXUIElementCopyAttributeValue(application, kAXWindowsAttribute,
                                      reinterpret_cast<CFTypeRef*>(&windows)) != kAXErrorSuccess ||
        windows == nullptr) {
        CFRelease(application);
        return nullptr;
    }
    AXUIElementRef found = nullptr;
    const CFIndex count = CFArrayGetCount(windows);
    for (CFIndex index = 0; index < count; ++index) {
        auto candidate = static_cast<AXUIElementRef>(
            const_cast<void*>(CFArrayGetValueAtIndex(windows, index)));
        // _AXUIElementGetWindow is private, so match on geometry instead: the
        // CG window number is not exposed through the public AX API.
        CGWindowID identifier = 0;
        if (AXUIElementGetWindow != nullptr &&
            AXUIElementGetWindow(candidate, &identifier) == kAXErrorSuccess &&
            identifier == static_cast<CGWindowID>(window_number)) {
            found = static_cast<AXUIElementRef>(CFRetain(candidate));
            break;
        }
    }
    CFRelease(windows);
    CFRelease(application);
    return found;
}

class MacWindows final : public WindowProvider {
public:
    Result<std::vector<WindowInfo>> enumerate(const CaptureOptions& options) override {
        CFArrayRef list = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID);
        if (list == nullptr) {
            return core::err::denied(
                "CGWindowListCopyWindowInfo returned nothing; grant Screen Recording in "
                "System Settings > Privacy & Security",
                "hal.macos");
        }
        const pid_t focused_pid =
            [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier;

        std::vector<WindowInfo> windows;
        std::int32_t z = 0;
        const CFIndex count = CFArrayGetCount(list);
        for (CFIndex index = 0; index < count; ++index) {
            auto entry = static_cast<NSDictionary*>(
                const_cast<void*>(CFArrayGetValueAtIndex(list, index)));

            const NSNumber* layer = entry[(__bridge NSString*)kCGWindowLayer];
            if (layer != nil && layer.intValue != 0) {
                continue;  // Menu bar, dock and overlays are not app windows.
            }
            const NSNumber* alpha = entry[(__bridge NSString*)kCGWindowAlpha];
            if (alpha != nil && alpha.doubleValue < 0.05) {
                continue;
            }

            WindowInfo info;
            const NSNumber* number = entry[(__bridge NSString*)kCGWindowNumber];
            info.native_handle = number == nil ? 0 : number.unsignedLongLongValue;
            info.title = to_std(entry[(__bridge NSString*)kCGWindowName]);
            info.window_class = to_std(entry[(__bridge NSString*)kCGWindowOwnerName]);

            const NSDictionary* bounds = entry[(__bridge NSString*)kCGWindowBounds];
            CGRect rect = CGRectZero;
            if (bounds != nil) {
                CGRectMakeWithDictionaryRepresentation(
                    static_cast<CFDictionaryRef>(bounds), &rect);
            }
            info.frame = Rect{static_cast<std::int32_t>(rect.origin.x),
                              static_cast<std::int32_t>(rect.origin.y),
                              static_cast<std::int32_t>(rect.size.width),
                              static_cast<std::int32_t>(rect.size.height)};
            info.client_area = info.frame;
            info.restored_frame = info.frame;
            info.captured_geometry_reliable = true;
            info.opacity = alpha == nil ? 1.0 : alpha.doubleValue;
            info.z_order = z++;

            const NSNumber* owner = entry[(__bridge NSString*)kCGWindowOwnerPID];
            const pid_t pid = owner == nil ? 0 : owner.intValue;
            info.process = read_process(pid);
            info.focused = pid == focused_pid;
            info.browser =
                core::matching::detect_browser(info.process.app_id, info.window_class);

            const NSNumber* on_screen = entry[(__bridge NSString*)kCGWindowIsOnscreen];
            const bool visible = on_screen == nil || on_screen.boolValue;
            info.state = visible ? WindowState::Normal : WindowState::Minimized;
            if (!options.include_minimized && info.state == WindowState::Minimized) {
                continue;
            }

            // Fullscreen windows exactly cover a display.
            for (NSScreen* screen in [NSScreen screens]) {
                const Rect screen_rect = from_appkit(screen.frame, primary_display_height());
                if (info.frame.width >= screen_rect.width &&
                    info.frame.height >= screen_rect.height) {
                    info.state = WindowState::Fullscreen;
                }
            }
            windows.push_back(std::move(info));
        }
        CFRelease(list);
        return windows;
    }

    Result<WindowInfo> get(std::uint64_t handle) override {
        CaptureOptions options;
        options.include_minimized = true;
        auto all = enumerate(options);
        if (!all) {
            return all.error();
        }
        for (WindowInfo& window : all.value()) {
            if (window.native_handle == handle) {
                return window;
            }
        }
        return core::err::not_found("window is gone", "hal.macos");
    }

    Status set_frame(std::uint64_t handle, const Rect& frame) override {
        if (!accessibility_trusted()) {
            return core::err::denied("Accessibility permission is required to move windows",
                                     "hal.macos");
        }
        auto window = get(handle);
        if (!window) {
            return window.error();
        }
        AXUIElementRef element =
            window_element(handle, static_cast<pid_t>(window.value().process.pid));
        if (element == nullptr) {
            return core::err::not_found("no accessibility element for this window", "hal.macos");
        }
        CGPoint position{static_cast<CGFloat>(frame.x), static_cast<CGFloat>(frame.y)};
        CGSize size{static_cast<CGFloat>(frame.width), static_cast<CGFloat>(frame.height)};
        AXValueRef position_value = AXValueCreate(kAXValueCGPointType, &position);
        AXValueRef size_value = AXValueCreate(kAXValueCGSizeType, &size);
        const AXError moved =
            AXUIElementSetAttributeValue(element, kAXPositionAttribute, position_value);
        const AXError resized =
            AXUIElementSetAttributeValue(element, kAXSizeAttribute, size_value);
        CFRelease(position_value);
        CFRelease(size_value);
        CFRelease(element);
        if (moved != kAXErrorSuccess || resized != kAXErrorSuccess) {
            return core::err::internal("the app refused the geometry change", "hal.macos");
        }
        return Status::success();
    }

    Status set_state(std::uint64_t handle, WindowState state) override {
        if (!accessibility_trusted()) {
            return core::err::denied("Accessibility permission is required", "hal.macos");
        }
        auto window = get(handle);
        if (!window) {
            return window.error();
        }
        AXUIElementRef element =
            window_element(handle, static_cast<pid_t>(window.value().process.pid));
        if (element == nullptr) {
            return core::err::not_found("no accessibility element", "hal.macos");
        }
        Status result = Status::success();
        switch (state) {
            case WindowState::Minimized:
                AXUIElementSetAttributeValue(element, kAXMinimizedAttribute, kCFBooleanTrue);
                break;
            case WindowState::Fullscreen:
                AXUIElementSetAttributeValue(element, CFSTR("AXFullScreen"), kCFBooleanTrue);
                break;
            case WindowState::Maximized:
                // macOS has no "maximize"; zoom is the closest equivalent.
                AXUIElementPerformAction(element, CFSTR("AXZoomWindow"));
                break;
            case WindowState::Normal:
                AXUIElementSetAttributeValue(element, kAXMinimizedAttribute, kCFBooleanFalse);
                AXUIElementSetAttributeValue(element, CFSTR("AXFullScreen"), kCFBooleanFalse);
                break;
            case WindowState::Hidden:
                result = core::err::unsupported("macOS cannot hide a single window",
                                                "hal.macos");
                break;
        }
        CFRelease(element);
        return result;
    }

    Status raise(std::uint64_t handle) override {
        if (!accessibility_trusted()) {
            return core::err::denied("Accessibility permission is required", "hal.macos");
        }
        auto window = get(handle);
        if (!window) {
            return window.error();
        }
        AXUIElementRef element =
            window_element(handle, static_cast<pid_t>(window.value().process.pid));
        if (element == nullptr) {
            return core::err::not_found("no accessibility element", "hal.macos");
        }
        AXUIElementPerformAction(element, kAXRaiseAction);
        CFRelease(element);
        return Status::success();
    }

    Status focus(std::uint64_t handle) override {
        auto window = get(handle);
        if (!window) {
            return window.error();
        }
        NSRunningApplication* application = [NSRunningApplication
            runningApplicationWithProcessIdentifier:static_cast<pid_t>(
                                                        window.value().process.pid)];
        if (application == nil) {
            return core::err::not_found("application is gone", "hal.macos");
        }
        [application activateWithOptions:NSApplicationActivateIgnoringOtherApps];
        return raise(handle);
    }

    Status set_always_on_top(std::uint64_t, bool) override {
        return core::err::unsupported(
            "macOS does not let one app change another app's window level", "hal.macos");
    }

    Status close(std::uint64_t handle) override {
        if (!accessibility_trusted()) {
            return core::err::denied("Accessibility permission is required", "hal.macos");
        }
        auto window = get(handle);
        if (!window) {
            return window.error();
        }
        AXUIElementRef element =
            window_element(handle, static_cast<pid_t>(window.value().process.pid));
        if (element == nullptr) {
            return core::err::not_found("no accessibility element", "hal.macos");
        }
        CFTypeRef button = nullptr;
        if (AXUIElementCopyAttributeValue(element, kAXCloseButtonAttribute, &button) ==
                kAXErrorSuccess &&
            button != nullptr) {
            AXUIElementPerformAction(static_cast<AXUIElementRef>(button), kAXPressAction);
            CFRelease(button);
        }
        CFRelease(element);
        return Status::success();
    }

    Result<WindowInfo> wait_for_window(std::uint64_t pid, std::string_view title_hint,
                                       std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        CaptureOptions options;
        options.include_minimized = true;
        while (std::chrono::steady_clock::now() < deadline) {
            auto windows = enumerate(options);
            if (windows) {
                const WindowInfo* best = nullptr;
                double best_score = 0.0;
                for (const WindowInfo& window : windows.value()) {
                    if (pid != 0 && window.process.pid != pid) {
                        continue;
                    }
                    const double score =
                        title_hint.empty()
                            ? 1.0
                            : core::matching::title_similarity(window.title, title_hint);
                    if (score > best_score) {
                        best_score = score;
                        best = &window;
                    }
                }
                if (best != nullptr) {
                    return *best;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
        }
        return core::Error{core::ErrorCode::Timeout, "no window appeared in time", "hal.macos", 0};
    }
};

class MacDisplays final : public DisplayProvider {
public:
    Result<std::vector<MonitorInfo>> enumerate() override {
        std::vector<MonitorInfo> monitors;
        const CGFloat height = primary_display_height();
        int index = 0;
        for (NSScreen* screen in [NSScreen screens]) {
            MonitorInfo monitor;
            const NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
            const CGDirectDisplayID identifier =
                number == nil ? 0 : number.unsignedIntValue;
            monitor.id = "display-" + std::to_string(identifier);
            monitor.name = to_std(screen.localizedName);
            monitor.bounds = from_appkit(screen.frame, height);
            monitor.work_area = from_appkit(screen.visibleFrame, height);
            monitor.scale_factor = screen.backingScaleFactor;
            monitor.dpi = static_cast<std::int32_t>(72.0 * screen.backingScaleFactor);
            monitor.primary = index == 0;
            monitor.refresh_hz = static_cast<std::int32_t>(
                CGDisplayModeGetRefreshRate(CGDisplayCopyDisplayMode(identifier)));
            monitors.push_back(std::move(monitor));
            ++index;
        }
        if (monitors.empty()) {
            return core::err::unsupported("no displays are attached", "hal.macos");
        }
        return monitors;
    }

    Result<Rect> virtual_bounds() override {
        auto monitors = enumerate();
        if (!monitors) {
            return monitors.error();
        }
        return core::virtual_bounds(monitors.value());
    }
};

class MacCursor final : public CursorProvider {
public:
    Result<CursorState> query() override {
        const CGEventRef event = CGEventCreate(nullptr);
        if (event == nullptr) {
            return core::err::internal("CGEventCreate failed", "hal.macos");
        }
        const CGPoint point = CGEventGetLocation(event);
        CFRelease(event);
        CursorState cursor;
        cursor.x = static_cast<std::int32_t>(point.x);
        cursor.y = static_cast<std::int32_t>(point.y);
        return cursor;
    }

    Status warp(std::int32_t x, std::int32_t y) override {
        if (CGWarpMouseCursorPosition(CGPointMake(x, y)) != kCGErrorSuccess) {
            return core::err::internal("CGWarpMouseCursorPosition failed", "hal.macos");
        }
        CGAssociateMouseAndMouseCursorPosition(true);
        return Status::success();
    }
};

class MacProcesses final : public ProcessProvider {
public:
    Result<ProcessInfo> info(std::uint64_t pid) override {
        ProcessInfo process = read_process(static_cast<pid_t>(pid));
        if (process.executable_path.empty()) {
            return core::err::not_found("no process " + std::to_string(pid), "hal.macos");
        }
        return process;
    }

    Result<std::uint64_t> launch(const ProcessInfo& process) override {
        // Prefer the bundle: launching the executable inside a .app directly
        // breaks app services such as the dock icon and Apple events.
        NSString* bundle_id = process.app_id.empty()
                                  ? nil
                                  : [NSString stringWithUTF8String:process.app_id.c_str()];
        NSURL* url = nil;
        if (bundle_id != nil) {
            url = [[NSWorkspace sharedWorkspace]
                URLForApplicationWithBundleIdentifier:bundle_id];
        }
        if (url == nil && !process.executable_path.empty()) {
            NSString* path =
                [NSString stringWithUTF8String:process.executable_path.c_str()];
            const NSRange app = [path rangeOfString:@".app/"];
            url = [NSURL fileURLWithPath:app.location == NSNotFound
                                             ? path
                                             : [path substringToIndex:app.location + 4]];
        }
        if (url == nil) {
            return core::err::invalid("cannot resolve an application to launch", "hal.macos");
        }

        NSError* error = nil;
        NSRunningApplication* application =
            [[NSWorkspace sharedWorkspace] launchApplicationAtURL:url
                                                          options:NSWorkspaceLaunchDefault
                                                    configuration:@{}
                                                            error:&error];
        if (application == nil) {
            return core::err::io("launch failed: " + to_std(error.localizedDescription),
                                 "hal.macos");
        }
        return static_cast<std::uint64_t>(application.processIdentifier);
    }

    bool is_running(std::uint64_t pid) override {
        return [NSRunningApplication
                   runningApplicationWithProcessIdentifier:static_cast<pid_t>(pid)] != nil ||
               kill(static_cast<pid_t>(pid), 0) == 0;
    }

    Result<std::vector<std::uint64_t>> find_by_app_id(std::string_view app_id) override {
        std::vector<std::uint64_t> pids;
        NSString* identifier = [NSString stringWithUTF8String:std::string(app_id).c_str()];
        for (NSRunningApplication* application in
             [[NSWorkspace sharedWorkspace] runningApplications]) {
            if ([application.bundleIdentifier isEqualToString:identifier]) {
                pids.push_back(static_cast<std::uint64_t>(application.processIdentifier));
            }
        }
        return pids;
    }
};

/// Mission Control spaces are not scriptable without private APIs, so ContextSnap
/// records the space a window was on but never moves windows between spaces.
class MacWorkspaces final : public WorkspaceProvider {
public:
    Result<std::vector<Workspace>> enumerate() override {
        return core::err::unsupported(
            "macOS exposes no public API for Mission Control spaces", "hal.macos");
    }
    Result<std::string> current() override {
        return core::err::unsupported("no public spaces API", "hal.macos");
    }
    Status activate(std::string_view) override {
        return core::err::unsupported("no public spaces API", "hal.macos");
    }
    Status move_window(std::uint64_t, std::string_view) override {
        return core::err::unsupported("no public spaces API", "hal.macos");
    }
};

class MacPlatform final : public Platform {
public:
    MacPlatform() {
        const bool trusted = accessibility_trusted();
        capabilities_.enumerate_windows = true;
        capabilities_.window_geometry = true;
        capabilities_.set_window_geometry = trusted;
        capabilities_.set_window_state = trusted;
        capabilities_.z_order = trusted;
        capabilities_.focus_control = true;
        capabilities_.cursor_query = true;
        capabilities_.cursor_warp = true;
        capabilities_.process_command_line = true;
        capabilities_.process_working_directory = false;
        capabilities_.launch_processes = true;
        capabilities_.window_thumbnails = true;
        capabilities_.virtual_desktops = false;
        capabilities_.move_between_desktops = false;
        if (!trusted) {
            capabilities_.limitations.emplace_back(
                "Accessibility permission missing: windows can be captured but not moved");
        }
        capabilities_.limitations.emplace_back(
            "Window titles require the Screen Recording permission on macOS 10.15+");
        capabilities_.limitations.emplace_back(
            "Mission Control spaces are recorded but cannot be restored (no public API)");
        capabilities_.limitations.emplace_back(
            "Working directories of other processes are not readable on macOS");
    }

    [[nodiscard]] core::PlatformKind kind() const noexcept override {
        return core::PlatformKind::MacOS;
    }

    [[nodiscard]] core::SessionType session_type() const noexcept override {
        return core::SessionType::Quartz;
    }

    [[nodiscard]] std::string name() const override { return "macOS (Quartz/Accessibility)"; }

    [[nodiscard]] std::string os_version() const override {
        NSProcessInfo* process = [NSProcessInfo processInfo];
        return "macOS " + to_std(process.operatingSystemVersionString);
    }

    [[nodiscard]] const Capabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    WindowProvider& windows() override { return windows_; }
    DisplayProvider& displays() override { return displays_; }
    CursorProvider& cursor() override { return cursor_; }
    ProcessProvider& processes() override { return processes_; }
    WorkspaceProvider& workspaces() override { return workspaces_; }

    [[nodiscard]] std::vector<std::string> missing_permissions() override {
        std::vector<std::string> missing;
        if (!accessibility_trusted()) {
            missing.emplace_back("macos.accessibility");
        }
        if (CGPreflightScreenCaptureAccess() == false) {
            missing.emplace_back("macos.screen-recording");
        }
        return missing;
    }

    [[nodiscard]] Status request_permissions() override {
        // Both calls surface the system prompt exactly once per app bundle.
        const void* keys[] = {kAXTrustedCheckOptionPrompt};
        const void* values[] = {kCFBooleanTrue};
        CFDictionaryRef options = CFDictionaryCreate(
            nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
        CGRequestScreenCaptureAccess();
        return Status::success();
    }

private:
    Capabilities capabilities_;
    MacWindows windows_;
    MacDisplays displays_;
    MacCursor cursor_;
    MacProcesses processes_;
    MacWorkspaces workspaces_;
};

}  // namespace

Result<std::shared_ptr<Platform>> create_macos_platform() {
    return std::static_pointer_cast<Platform>(std::make_shared<MacPlatform>());
}

}  // namespace contextsnap::hal

#endif  // __APPLE__
