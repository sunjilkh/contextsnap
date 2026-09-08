// Geometry normalization: the part of ContextSnap that decides where a window
// goes when the monitor layout at restore time differs from capture time.
//
// Rules implemented here (see docs/architecture.md#geometry-normalization):
//   1. Everything is stored in virtual-desktop coordinates, primary at (0,0).
//   2. A window belongs to the monitor with the largest intersection area.
//   3. Geometry is persisted *relative* to that monitor's work area, as
//      normalized [0,1] fractions, so a 4K -> 1080p change scales gracefully.
//   4. When the owning monitor is gone at restore time, the window is remapped
//      onto the best surviving monitor and clamped into its work area.
#pragma once

#include <contextsnap/core/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace contextsnap::core::geometry {

/// Geometry expressed as fractions of a monitor work area.
struct RelativeRect {
    double x{0.0};
    double y{0.0};
    double width{0.0};
    double height{0.0};
};

[[nodiscard]] Rect intersection(const Rect& a, const Rect& b) noexcept;
[[nodiscard]] Rect union_of(const Rect& a, const Rect& b) noexcept;
[[nodiscard]] std::int64_t intersection_area(const Rect& a, const Rect& b) noexcept;
[[nodiscard]] bool intersects(const Rect& a, const Rect& b) noexcept;

/// Smallest rect containing every monitor — the virtual desktop extent.
[[nodiscard]] Rect virtual_bounds(const std::vector<MonitorInfo>& monitors) noexcept;

/// Monitor owning `frame` (largest intersection; nearest centre as a tiebreak).
/// Returns nullptr only when `monitors` is empty.
[[nodiscard]] const MonitorInfo* owning_monitor(const Rect& frame,
                                                const std::vector<MonitorInfo>& monitors) noexcept;

[[nodiscard]] const MonitorInfo* primary_monitor(const std::vector<MonitorInfo>& monitors) noexcept;

/// Converts an absolute rect into monitor-relative fractions (clamped to sane
/// bounds so a window dragged half off-screen does not produce absurd values).
[[nodiscard]] RelativeRect to_relative(const Rect& frame, const MonitorInfo& monitor) noexcept;

/// Inverse of to_relative().
[[nodiscard]] Rect from_relative(const RelativeRect& relative, const MonitorInfo& monitor) noexcept;

/// Moves `frame` fully inside `work_area`, shrinking it only if it cannot fit.
[[nodiscard]] Rect clamp_into(const Rect& frame, const Rect& work_area) noexcept;

/// Rescales a rect captured at `from_dpi` for a display running at `to_dpi`.
[[nodiscard]] Rect scale_for_dpi(const Rect& frame,
                                 std::uint32_t from_dpi,
                                 std::uint32_t to_dpi) noexcept;

/// Chooses the monitor in `target` that best matches `source`: exact id, then
/// matching resolution + primary flag, then largest area.
[[nodiscard]] const MonitorInfo* best_match(const MonitorInfo& source,
                                            const std::vector<MonitorInfo>& target) noexcept;

/// Full remap of a captured window frame onto the current monitor layout.
///
/// `captured_monitors` is the layout stored in the snapshot, `current_monitors`
/// the layout observed now. The result is always inside a visible work area.
struct RemapResult {
    Rect frame;
    std::string monitor_id;
    bool monitor_changed{false};
    bool clamped{false};
    bool rescaled{false};
};

[[nodiscard]] RemapResult remap_frame(const Rect& frame,
                                      const std::string& captured_monitor_id,
                                      const std::vector<MonitorInfo>& captured_monitors,
                                      const std::vector<MonitorInfo>& current_monitors) noexcept;

/// True when the two layouts are equivalent for restoration purposes.
[[nodiscard]] bool layouts_equivalent(const std::vector<MonitorInfo>& a,
                                      const std::vector<MonitorInfo>& b) noexcept;

}  // namespace contextsnap::core::geometry
