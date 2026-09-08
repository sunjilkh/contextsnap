#pragma once

/// @file null_platform.hpp
/// Headless HAL backend. It keeps window state in memory so unit tests, CI
/// runners and `contextsnap doctor` work without a display server.

#include <contextsnap/hal/platform.hpp>

#include <memory>
#include <vector>

namespace contextsnap::hal {

/// Seed state for the null platform; mutations are applied in place so tests
/// can assert that restore moved the right windows.
struct NullPlatformState {
    std::vector<MonitorInfo> monitors;
    std::vector<WindowInfo> windows;
    CursorState cursor;
    std::vector<WorkspaceProvider::Workspace> workspaces;
    /// When false, every mutating call fails with ErrorCode::Unsupported, which
    /// mirrors a locked-down Wayland compositor.
    bool allow_mutation{true};
};

/// A single 1920x1080 monitor, one workspace, no windows.
[[nodiscard]] NullPlatformState default_null_state();

/// Two monitors with mixed DPI plus an editor and a browser window; used by the
/// restore-planner tests.
[[nodiscard]] NullPlatformState sample_null_state();

[[nodiscard]] std::shared_ptr<Platform> create_null_platform(NullPlatformState state);

inline std::shared_ptr<Platform> create_null_platform() {
    return create_null_platform(default_null_state());
}

}  // namespace contextsnap::hal
