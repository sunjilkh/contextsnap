#include <contextsnap/core/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace contextsnap::core::geometry {
namespace {

/// Windows may be dragged partly off-screen; allow half a screen of slack in
/// relative space rather than clamping to [0,1] and "teleporting" them.
constexpr double kRelativeSlack = 0.5;

double centre_x(const Rect& r) { return r.x + r.width / 2.0; }

double centre_y(const Rect& r) { return r.y + r.height / 2.0; }

double centre_distance(const Rect& a, const Rect& b) {
    const double dx = centre_x(a) - centre_x(b);
    const double dy = centre_y(a) - centre_y(b);
    return std::sqrt(dx * dx + dy * dy);
}

const Rect& usable_area(const MonitorInfo& monitor) {
    return monitor.work_area.empty() ? monitor.bounds : monitor.work_area;
}

}  // namespace

Rect intersection(const Rect& a, const Rect& b) noexcept {
    const std::int32_t left = std::max(a.left(), b.left());
    const std::int32_t top = std::max(a.top(), b.top());
    const std::int32_t right = std::min(a.right(), b.right());
    const std::int32_t bottom = std::min(a.bottom(), b.bottom());
    if (right <= left || bottom <= top) {
        return Rect{};
    }
    return Rect{left, top, right - left, bottom - top};
}

Rect union_of(const Rect& a, const Rect& b) noexcept {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const std::int32_t left = std::min(a.left(), b.left());
    const std::int32_t top = std::min(a.top(), b.top());
    const std::int32_t right = std::max(a.right(), b.right());
    const std::int32_t bottom = std::max(a.bottom(), b.bottom());
    return Rect{left, top, right - left, bottom - top};
}

std::int64_t intersection_area(const Rect& a, const Rect& b) noexcept {
    return intersection(a, b).area();
}

bool intersects(const Rect& a, const Rect& b) noexcept { return !intersection(a, b).empty(); }

Rect virtual_bounds(const std::vector<MonitorInfo>& monitors) noexcept {
    Rect bounds{};
    for (const auto& monitor : monitors) {
        bounds = union_of(bounds, monitor.bounds);
    }
    return bounds;
}

const MonitorInfo* owning_monitor(const Rect& frame,
                                  const std::vector<MonitorInfo>& monitors) noexcept {
    const MonitorInfo* best = nullptr;
    std::int64_t best_area = 0;
    double best_distance = std::numeric_limits<double>::max();

    for (const auto& monitor : monitors) {
        const std::int64_t area = intersection_area(frame, monitor.bounds);
        if (area > best_area) {
            best_area = area;
            best = &monitor;
            best_distance = centre_distance(frame, monitor.bounds);
            continue;
        }
        // Fully off-screen window: fall back to the nearest monitor centre.
        if (best_area == 0) {
            const double distance = centre_distance(frame, monitor.bounds);
            if (distance < best_distance) {
                best_distance = distance;
                best = &monitor;
            }
        }
    }
    return best;
}

const MonitorInfo* primary_monitor(const std::vector<MonitorInfo>& monitors) noexcept {
    for (const auto& monitor : monitors) {
        if (monitor.primary) {
            return &monitor;
        }
    }
    return monitors.empty() ? nullptr : &monitors.front();
}

RelativeRect to_relative(const Rect& frame, const MonitorInfo& monitor) noexcept {
    const Rect& area = usable_area(monitor);
    if (area.empty()) {
        return RelativeRect{0.0, 0.0, 1.0, 1.0};
    }
    const double w = static_cast<double>(area.width);
    const double h = static_cast<double>(area.height);
    RelativeRect relative{
        (frame.x - area.x) / w,
        (frame.y - area.y) / h,
        frame.width / w,
        frame.height / h,
    };
    relative.x = std::clamp(relative.x, -kRelativeSlack, 1.0 + kRelativeSlack);
    relative.y = std::clamp(relative.y, -kRelativeSlack, 1.0 + kRelativeSlack);
    relative.width = std::clamp(relative.width, 0.01, 1.0 + kRelativeSlack);
    relative.height = std::clamp(relative.height, 0.01, 1.0 + kRelativeSlack);
    return relative;
}

Rect from_relative(const RelativeRect& relative, const MonitorInfo& monitor) noexcept {
    const Rect& area = usable_area(monitor);
    const double w = static_cast<double>(area.width);
    const double h = static_cast<double>(area.height);
    return Rect{
        area.x + static_cast<std::int32_t>(std::lround(relative.x * w)),
        area.y + static_cast<std::int32_t>(std::lround(relative.y * h)),
        std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(relative.width * w))),
        std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(relative.height * h))),
    };
}

Rect clamp_into(const Rect& frame, const Rect& work_area) noexcept {
    if (work_area.empty()) {
        return frame;
    }
    Rect out = frame;
    out.width = std::min(out.width, work_area.width);
    out.height = std::min(out.height, work_area.height);
    out.width = std::max(out.width, 1);
    out.height = std::max(out.height, 1);
    out.x = std::clamp(out.x, work_area.left(), work_area.right() - out.width);
    out.y = std::clamp(out.y, work_area.top(), work_area.bottom() - out.height);
    return out;
}

Rect scale_for_dpi(const Rect& frame, std::uint32_t from_dpi, std::uint32_t to_dpi) noexcept {
    if (from_dpi == 0 || to_dpi == 0 || from_dpi == to_dpi) {
        return frame;
    }
    const double factor = static_cast<double>(to_dpi) / static_cast<double>(from_dpi);
    return Rect{
        static_cast<std::int32_t>(std::lround(frame.x * factor)),
        static_cast<std::int32_t>(std::lround(frame.y * factor)),
        std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(frame.width * factor))),
        std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(frame.height * factor))),
    };
}

const MonitorInfo* best_match(const MonitorInfo& source,
                              const std::vector<MonitorInfo>& target) noexcept {
    if (target.empty()) {
        return nullptr;
    }
    // 1. Same stable id (same physical panel, possibly rearranged).
    for (const auto& monitor : target) {
        if (monitor.id == source.id) {
            return &monitor;
        }
    }
    // 2. Same EDID hash (same panel, different connector/name).
    if (source.edid_hash.has_value()) {
        for (const auto& monitor : target) {
            if (monitor.edid_hash == source.edid_hash) {
                return &monitor;
            }
        }
    }
    // 3. Same resolution and primary flag.
    for (const auto& monitor : target) {
        if (monitor.bounds.width == source.bounds.width &&
            monitor.bounds.height == source.bounds.height && monitor.primary == source.primary) {
            return &monitor;
        }
    }
    // 4. Same resolution regardless of role.
    for (const auto& monitor : target) {
        if (monitor.bounds.width == source.bounds.width &&
            monitor.bounds.height == source.bounds.height) {
            return &monitor;
        }
    }
    // 5. Primary, else the largest surviving display.
    if (const MonitorInfo* primary = primary_monitor(target)) {
        return primary;
    }
    return &*std::max_element(target.begin(), target.end(),
                              [](const MonitorInfo& a, const MonitorInfo& b) {
                                  return a.bounds.area() < b.bounds.area();
                              });
}

RemapResult remap_frame(const Rect& frame,
                        const std::string& captured_monitor_id,
                        const std::vector<MonitorInfo>& captured_monitors,
                        const std::vector<MonitorInfo>& current_monitors) noexcept {
    RemapResult result{frame, captured_monitor_id, false, false, false};
    if (current_monitors.empty()) {
        return result;
    }

    // Locate the captured monitor; fall back to geometric ownership when the
    // snapshot predates monitor ids or the id was lost.
    const MonitorInfo* source = nullptr;
    for (const auto& monitor : captured_monitors) {
        if (monitor.id == captured_monitor_id) {
            source = &monitor;
            break;
        }
    }
    if (source == nullptr) {
        source = owning_monitor(frame, captured_monitors);
    }
    if (source == nullptr) {
        // No captured layout at all: clamp into the primary monitor.
        const MonitorInfo* primary = primary_monitor(current_monitors);
        result.frame = clamp_into(frame, usable_area(*primary));
        result.monitor_id = primary->id;
        result.monitor_changed = true;
        result.clamped = result.frame != frame;
        return result;
    }

    const MonitorInfo* target = best_match(*source, current_monitors);
    if (target == nullptr) {
        return result;
    }

    result.monitor_id = target->id;
    result.monitor_changed = target->id != source->id;

    const bool same_geometry = source->bounds.width == target->bounds.width &&
                               source->bounds.height == target->bounds.height &&
                               source->work_area == target->work_area;
    if (!result.monitor_changed && same_geometry && source->dpi == target->dpi) {
        return result;  // Identical display: keep pixel-exact placement.
    }

    // Proportional remap through relative coordinates, then DPI correction for
    // the residual difference in logical pixel density.
    const RelativeRect relative = to_relative(frame, *source);
    Rect remapped = from_relative(relative, *target);
    if (source->dpi != target->dpi && source->bounds.width == target->bounds.width &&
        source->bounds.height == target->bounds.height) {
        remapped = scale_for_dpi(frame, source->dpi, target->dpi);
        result.rescaled = true;
    }

    const Rect clamped = clamp_into(remapped, usable_area(*target));
    result.clamped = !(clamped == remapped);
    result.frame = clamped;
    return result;
}

bool layouts_equivalent(const std::vector<MonitorInfo>& a,
                        const std::vector<MonitorInfo>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (const auto& monitor : a) {
        const MonitorInfo* match = best_match(monitor, b);
        if (match == nullptr || !(match->bounds == monitor.bounds) ||
            !(match->work_area == monitor.work_area) || match->dpi != monitor.dpi) {
            return false;
        }
    }
    return true;
}

}  // namespace contextsnap::core::geometry
