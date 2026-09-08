// Restore planning and execution. Split from snapshot_manager.cpp because the
// ordering rules here are the trickiest part of the project.
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/matching.hpp>
#include <contextsnap/core/snapshot_manager.hpp>
#include <contextsnap/storage/serialization.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace contextsnap::core {
namespace {

RestoreStep step(RestoreStepKind kind, std::string target_id, std::string description,
                 bool skipped = false, std::string skip_reason = {}) {
    RestoreStep out;
    out.kind = kind;
    out.target_id = std::move(target_id);
    out.description = std::move(description);
    out.skipped = skipped;
    out.skip_reason = std::move(skip_reason);
    return out;
}

/// Correlates one saved window with the windows currently on screen. Kept local
/// so the planner does not depend on the full scoring matcher.
const WindowInfo* best_live_match(const WindowInfo& saved,
                                  const std::vector<WindowInfo>& live,
                                  const std::vector<std::uint64_t>& already_used) {
    const std::string saved_app = matching::normalize_app_id(saved.process.app_id);
    const WindowInfo* best = nullptr;
    double best_score = 0.0;
    for (const WindowInfo& candidate : live) {
        if (std::find(already_used.begin(), already_used.end(), candidate.native_handle) !=
            already_used.end()) {
            continue;
        }
        if (matching::normalize_app_id(candidate.process.app_id) != saved_app) {
            continue;
        }
        // Same app: prefer the closest title, then the closest geometry.
        const double score = 0.7 * matching::title_similarity(saved.title, candidate.title) +
                             0.3 * matching::geometry_proximity(saved.frame, candidate.frame);
        if (score > best_score) {
            best_score = score;
            best = &candidate;
        }
    }
    return best_score >= 0.35 ? best : nullptr;
}

Rect clamp_to_monitors(const Rect& frame, const std::vector<MonitorInfo>& monitors) {
    if (monitors.empty()) {
        return frame;
    }
    // Keep at least the title bar reachable when the display layout shrank.
    const MonitorInfo* target = &monitors.front();
    std::int64_t best_overlap = -1;
    for (const MonitorInfo& monitor : monitors) {
        const std::int32_t x = std::max(frame.left(), monitor.work_area.left());
        const std::int32_t y = std::max(frame.top(), monitor.work_area.top());
        const std::int32_t right = std::min(frame.right(), monitor.work_area.right());
        const std::int32_t bottom = std::min(frame.bottom(), monitor.work_area.bottom());
        const std::int64_t overlap = static_cast<std::int64_t>(std::max(0, right - x)) *
                                     static_cast<std::int64_t>(std::max(0, bottom - y));
        if (overlap > best_overlap) {
            best_overlap = overlap;
            target = &monitor;
        }
    }
    Rect out = frame;
    out.width = std::min(out.width, target->work_area.width);
    out.height = std::min(out.height, target->work_area.height);
    out.x = std::min(std::max(out.x, target->work_area.left()),
                     target->work_area.right() - out.width);
    out.y = std::min(std::max(out.y, target->work_area.top()),
                     target->work_area.bottom() - out.height);
    return out;
}

}  // namespace

bool selector_matches(const WindowInfo& window, const std::vector<RestoreSelector>& selectors) {
    if (selectors.empty()) {
        return true;
    }
    for (const RestoreSelector& selector : selectors) {
        switch (selector.kind) {
            case RestoreSelector::Kind::All:
                return true;
            case RestoreSelector::Kind::App:
                if (privacy::glob_match(selector.pattern,
                                        matching::normalize_app_id(window.process.app_id))) {
                    return true;
                }
                break;
            case RestoreSelector::Kind::Window:
                if (window.id.rfind(selector.pattern, 0) == 0) {
                    return true;
                }
                break;
            case RestoreSelector::Kind::Tab:
                for (const TabInfo& tab : window.tabs) {
                    if (privacy::glob_match(selector.pattern, tab.url) ||
                        privacy::glob_match(selector.pattern, tab.title)) {
                        return true;
                    }
                }
                break;
            case RestoreSelector::Kind::Monitor:
                if (window.monitor_id == selector.pattern) {
                    return true;
                }
                break;
            case RestoreSelector::Kind::Workspace:
                if (window.workspace_id.value_or("") == selector.pattern) {
                    return true;
                }
                break;
        }
    }
    return false;
}

Result<RestoreSelector> parse_selector(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        if (text == "all" || text == "*") {
            return RestoreSelector{RestoreSelector::Kind::All, "*"};
        }
        // Bare values are treated as app ids, which is what users expect from
        // `--only code,chromium`.
        return RestoreSelector{RestoreSelector::Kind::App, std::string(text)};
    }
    const std::string_view prefix = text.substr(0, colon);
    const std::string pattern(text.substr(colon + 1));
    if (pattern.empty()) {
        return err::invalid("selector has an empty pattern", "core.selector");
    }
    if (prefix == "app") return RestoreSelector{RestoreSelector::Kind::App, pattern};
    if (prefix == "window") return RestoreSelector{RestoreSelector::Kind::Window, pattern};
    if (prefix == "tab") return RestoreSelector{RestoreSelector::Kind::Tab, pattern};
    if (prefix == "monitor") return RestoreSelector{RestoreSelector::Kind::Monitor, pattern};
    if (prefix == "workspace") return RestoreSelector{RestoreSelector::Kind::Workspace, pattern};
    return err::invalid("unknown selector prefix: " + std::string(prefix), "core.selector");
}

Result<RestorePlan> SnapshotManager::plan_restore(const std::string& snapshot_id,
                                                  const RestoreOptions& options) {
    auto snapshot = get(snapshot_id);
    if (!snapshot) {
        return snapshot.error();
    }
    const Snapshot& saved = snapshot.value();
    const hal::Capabilities& capabilities = platform_->capabilities();

    RestorePlan plan;
    plan.snapshot_id = saved.metadata.id;

    auto live_monitors = platform_->displays().enumerate();
    if (live_monitors && live_monitors.value().size() != saved.monitors.size()) {
        plan.warnings.push_back("display layout changed (" +
                                std::to_string(saved.monitors.size()) + " saved vs " +
                                std::to_string(live_monitors.value().size()) +
                                " now); windows will be clamped to visible work areas");
    }
    if (saved.metadata.platform != platform_->kind()) {
        plan.warnings.push_back(
            "snapshot was captured on a different platform; app paths may not resolve");
    }

    CaptureOptions probe;
    probe.include_tabs = false;
    auto live = platform_->windows().enumerate(probe);
    const std::vector<WindowInfo> live_windows =
        live ? live.value() : std::vector<WindowInfo>{};
    if (!live) {
        plan.warnings.push_back("cannot enumerate live windows: " + live.error().to_string());
    }

    std::vector<std::uint64_t> used;
    std::uint32_t estimate = 0;
    for (const WindowInfo& window : saved.windows) {
        if (!selector_matches(window, options.selectors)) {
            continue;
        }
        const WindowInfo* match = best_live_match(window, live_windows, used);
        if (match != nullptr) {
            used.push_back(match->native_handle);
        } else if (options.launch_missing_apps) {
            const bool can_launch = capabilities.launch_processes;
            plan.steps.push_back(step(RestoreStepKind::LaunchProcess, window.id,
                                      "launch " + window.process.app_id, !can_launch,
                                      can_launch ? "" : "platform cannot launch processes"));
            plan.steps.push_back(step(RestoreStepKind::WaitForWindow, window.id,
                                      "wait for a window from " + window.process.app_id,
                                      !can_launch));
            estimate += can_launch ? 900 : 0;
        } else {
            plan.steps.push_back(step(RestoreStepKind::MoveWindow, window.id,
                                      "skip " + window.title, true,
                                      "app is not running and launching is disabled"));
            continue;
        }

        const bool can_move = capabilities.set_window_geometry;
        plan.steps.push_back(step(RestoreStepKind::MoveWindow, window.id,
                                  "place \"" + window.title + "\" at " +
                                      std::to_string(window.restored_frame.x) + "," +
                                      std::to_string(window.restored_frame.y),
                                  !can_move,
                                  can_move ? "" : "compositor does not allow window placement"));
        estimate += 20;

        if (window.state != WindowState::Normal) {
            plan.steps.push_back(step(RestoreStepKind::SetWindowState, window.id,
                                      std::string("set state ") + std::string(to_string(window.state)),
                                      !capabilities.set_window_state));
        }
        if (options.restore_workspaces && window.workspace_id.has_value()) {
            plan.steps.push_back(step(RestoreStepKind::SetWorkspace, window.id,
                                      "move to workspace " + *window.workspace_id,
                                      !capabilities.move_between_desktops));
        }
        if (options.restore_tabs && !window.tabs.empty()) {
            plan.steps.push_back(step(RestoreStepKind::RestoreTabs, window.id,
                                      "restore " + std::to_string(window.tabs.size()) +
                                          " tabs in " + std::string(to_string(window.browser))));
            estimate += static_cast<std::uint32_t>(window.tabs.size()) * 12;
        }
    }

    if (options.restore_z_order) {
        std::vector<const WindowInfo*> ordered;
        for (const WindowInfo& window : saved.windows) {
            if (selector_matches(window, options.selectors)) {
                ordered.push_back(&window);
            }
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const WindowInfo* a, const WindowInfo* b) { return a->z_order < b->z_order; });
        for (const WindowInfo* window : ordered) {
            plan.steps.push_back(step(RestoreStepKind::RaiseWindow, window->id,
                                      "raise \"" + window->title + "\"", !capabilities.z_order));
        }
    }
    if (options.restore_cursor) {
        plan.steps.push_back(step(RestoreStepKind::WarpCursor, "cursor",
                                  "warp cursor to " + std::to_string(saved.cursor.x) + "," +
                                      std::to_string(saved.cursor.y),
                                  !capabilities.cursor_warp));
    }
    if (options.restore_focus && saved.cursor.focused_window_id.has_value()) {
        plan.steps.push_back(step(RestoreStepKind::FocusWindow, *saved.cursor.focused_window_id,
                                  "focus the previously active window",
                                  !capabilities.focus_control));
    }

    plan.estimated_duration_ms = estimate;
    return plan;
}

Result<RestoreReport> SnapshotManager::execute_plan(const Snapshot& snapshot,
                                                    const RestorePlan& plan,
                                                    const RestoreOptions& options,
                                                    const ProgressCallback& progress) {
    const auto started = std::chrono::steady_clock::now();
    RestoreReport report;
    report.snapshot_id = snapshot.metadata.id;

    auto live_monitors = platform_->displays().enumerate();
    const std::vector<MonitorInfo> monitors =
        live_monitors ? live_monitors.value() : snapshot.monitors;

    CaptureOptions probe;
    probe.include_tabs = false;
    auto live = platform_->windows().enumerate(probe);
    std::vector<WindowInfo> live_windows = live ? live.value() : std::vector<WindowInfo>{};

    // Snapshot window id -> native handle of the window we are driving.
    std::unordered_map<std::string, std::uint64_t> handles;
    std::vector<std::uint64_t> used;
    for (const WindowInfo& window : snapshot.windows) {
        if (const WindowInfo* match = best_live_match(window, live_windows, used);
            match != nullptr) {
            handles[window.id] = match->native_handle;
            used.push_back(match->native_handle);
        }
    }

    const std::size_t total = plan.steps.empty() ? 1 : plan.steps.size();
    std::size_t index = 0;
    for (const RestoreStep& current : plan.steps) {
        ++index;
        if (progress) {
            progress(current.description, static_cast<int>((index * 100) / total));
        }
        if (current.skipped) {
            report.warnings.push_back("skipped: " + current.description +
                                      (current.skip_reason.empty() ? ""
                                                                   : " (" + current.skip_reason + ")"));
            continue;
        }
        const WindowInfo* saved = snapshot.find_window(current.target_id);
        switch (current.kind) {
            case RestoreStepKind::LaunchProcess: {
                if (saved == nullptr) {
                    break;
                }
                auto launched = platform_->processes().launch(saved->process);
                if (!launched) {
                    ++report.windows_failed;
                    report.errors.push_back("launch failed for " + saved->process.app_id + ": " +
                                            launched.error().to_string());
                    break;
                }
                ++report.apps_launched;
                handles[current.target_id + ":pid"] = launched.value();
                break;
            }
            case RestoreStepKind::WaitForWindow: {
                if (saved == nullptr) {
                    break;
                }
                const auto pid_entry = handles.find(current.target_id + ":pid");
                const std::uint64_t pid =
                    pid_entry == handles.end() ? 0 : pid_entry->second;
                auto appeared = platform_->windows().wait_for_window(
                    pid, saved->title, options.window_settle_timeout);
                if (!appeared) {
                    ++report.windows_failed;
                    report.errors.push_back("no window appeared for " + saved->process.app_id);
                    break;
                }
                handles[current.target_id] = appeared.value().native_handle;
                break;
            }
            case RestoreStepKind::MoveWindow: {
                const auto handle = handles.find(current.target_id);
                if (saved == nullptr || handle == handles.end()) {
                    report.warnings.push_back("no live window for " + current.description);
                    break;
                }
                const Rect target = clamp_to_monitors(saved->restored_frame, monitors);
                if (const Status moved = platform_->windows().set_frame(handle->second, target);
                    moved) {
                    ++report.windows_restored;
                } else {
                    ++report.windows_failed;
                    report.errors.push_back("placement failed: " + moved.error().to_string());
                }
                break;
            }
            case RestoreStepKind::SetWindowState: {
                const auto handle = handles.find(current.target_id);
                if (saved != nullptr && handle != handles.end()) {
                    if (const Status applied =
                            platform_->windows().set_state(handle->second, saved->state);
                        !applied) {
                        report.warnings.push_back("state change failed: " +
                                                  applied.error().to_string());
                    }
                }
                break;
            }
            case RestoreStepKind::SetWorkspace: {
                const auto handle = handles.find(current.target_id);
                if (saved != nullptr && handle != handles.end() &&
                    saved->workspace_id.has_value()) {
                    if (const Status moved = platform_->workspaces().move_window(
                            handle->second, *saved->workspace_id);
                        !moved) {
                        report.warnings.push_back("workspace move failed: " +
                                                  moved.error().to_string());
                    }
                }
                break;
            }
            case RestoreStepKind::RaiseWindow: {
                const auto handle = handles.find(current.target_id);
                if (handle != handles.end()) {
                    if (const Status raised = platform_->windows().raise(handle->second); !raised) {
                        report.warnings.push_back("raise failed: " + raised.error().to_string());
                    }
                }
                break;
            }
            case RestoreStepKind::RestoreTabs: {
                if (saved == nullptr || browser_bridge_ == nullptr) {
                    break;
                }
                browser::BrowserWindow request;
                request.browser = saved->browser;
                request.profile = saved->browser_profile.value_or("");
                request.frame = saved->restored_frame;
                request.state = std::string(to_string(saved->state));
                request.tabs = saved->tabs;
                auto restored = browser_bridge_->restore_tabs({request}, options.lazy_load_tabs);
                if (!restored) {
                    report.warnings.push_back("tab restore failed: " +
                                              restored.error().to_string());
                    break;
                }
                report.tabs_restored += restored.value();
                break;
            }
            case RestoreStepKind::WarpCursor: {
                if (const Status warped =
                        platform_->cursor().warp(snapshot.cursor.x, snapshot.cursor.y);
                    !warped) {
                    report.warnings.push_back("cursor warp failed: " + warped.error().to_string());
                }
                break;
            }
            case RestoreStepKind::FocusWindow: {
                const auto handle = handles.find(current.target_id);
                if (handle != handles.end()) {
                    if (const Status focused = platform_->windows().focus(handle->second);
                        !focused) {
                        report.warnings.push_back("focus failed: " + focused.error().to_string());
                    }
                }
                break;
            }
        }
    }

    report.duration_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started)
            .count());
    return report;
}

Result<RestoreReport> SnapshotManager::restore(const std::string& snapshot_id,
                                               const RestoreOptions& options,
                                               ProgressCallback progress) {
    auto snapshot = get(snapshot_id);
    if (!snapshot) {
        return snapshot.error();
    }
    auto plan = plan_restore(snapshot.value().metadata.id, options);
    if (!plan) {
        return plan.error();
    }
    if (options.dry_run) {
        RestoreReport report;
        report.snapshot_id = snapshot.value().metadata.id;
        report.warnings = plan.value().warnings;
        for (const RestoreStep& current : plan.value().steps) {
            report.warnings.push_back((current.skipped ? "skip: " : "plan: ") +
                                      current.description);
        }
        return report;
    }

    auto report = execute_plan(snapshot.value(), plan.value(), options, progress);
    if (!report) {
        return report.error();
    }
    for (const std::string& warning : plan.value().warnings) {
        report.value().warnings.push_back(warning);
    }

    // Best-effort audit trail; a failure here must not fail the restore.
    if (auto statement = repository_->database().prepare(
            "INSERT INTO restore_events (snapshot_id, started_at, duration_ms, windows_restored, "
            "windows_failed, apps_launched, tabs_restored, dry_run, warnings, errors) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)");
        statement) {
        json::Array warnings;
        for (const std::string& warning : report.value().warnings) {
            warnings.emplace_back(json::Value(warning));
        }
        json::Array errors;
        for (const std::string& error : report.value().errors) {
            errors.emplace_back(json::Value(error));
        }
        statement.value()
            .bind(1, report.value().snapshot_id)
            .bind(2, storage::to_unix_millis(Clock::now()))
            .bind(3, static_cast<std::int64_t>(report.value().duration_ms))
            .bind(4, static_cast<std::int64_t>(report.value().windows_restored))
            .bind(5, static_cast<std::int64_t>(report.value().windows_failed))
            .bind(6, static_cast<std::int64_t>(report.value().apps_launched))
            .bind(7, static_cast<std::int64_t>(report.value().tabs_restored))
            .bind(8, false)
            .bind(9, json::Value(std::move(warnings)).dump())
            .bind(10, json::Value(std::move(errors)).dump());
        if (const Status recorded = statement.value().execute(); !recorded) {
            log::debug("restore event not recorded",
                       {log::field("error", recorded.error().to_string())});
        }
    }

    log::info("restore finished",
              {log::field("id", report.value().snapshot_id),
               log::field("windows", static_cast<std::int64_t>(report.value().windows_restored)),
               log::field("tabs", static_cast<std::int64_t>(report.value().tabs_restored)),
               log::field("duration_ms", static_cast<std::int64_t>(report.value().duration_ms))});
    return report;
}

}  // namespace contextsnap::core
