// SnapshotManager is the orchestrator: it owns the HAL, the repository, the
// browser bridge and the sanitizer, and exposes the operations the daemon's IPC
// surface maps onto one-to-one.
//
// Capture pipeline
//   monitors -> windows -> processes -> (browser tabs in parallel) -> cursor
//   -> sanitize -> persist
//
// Restore pipeline (staged, see docs/architecture.md#restore-state-machine)
//   plan -> launch missing apps -> wait for windows -> match -> place geometry
//   -> workspaces -> z-order -> tabs -> cursor -> focus
#pragma once

#include <contextsnap/browser/bridge.hpp>
#include <contextsnap/core/config.hpp>
#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>
#include <contextsnap/hal/platform.hpp>
#include <contextsnap/privacy/sanitizer.hpp>
#include <contextsnap/storage/snapshot_repository.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace contextsnap::core {

/// Progress callback for long-running restores (GUI progress bar, CLI spinner).
using ProgressCallback = std::function<void(std::string_view stage, int percent)>;

class SnapshotManager {
public:
    SnapshotManager(std::shared_ptr<hal::Platform> platform,
                    std::shared_ptr<storage::SnapshotRepository> repository,
                    std::shared_ptr<browser::Bridge> browser_bridge,
                    std::shared_ptr<privacy::Sanitizer> sanitizer,
                    Config config);

    ~SnapshotManager();

    SnapshotManager(const SnapshotManager&) = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;

    // --- Capture -----------------------------------------------------------

    /// Captures the live desktop state without persisting it.
    [[nodiscard]] Result<Snapshot> capture(const CaptureOptions& options);

    /// Captures and stores in one transaction; returns the persisted snapshot.
    [[nodiscard]] Result<Snapshot> capture_and_store(const CaptureOptions& options);

    // --- Restore -----------------------------------------------------------

    /// Computes the ordered step list without touching the desktop.
    [[nodiscard]] Result<RestorePlan> plan_restore(const std::string& snapshot_id,
                                                   const RestoreOptions& options);

    /// Executes a restore. Honours options.dry_run by returning an empty report
    /// with the plan's warnings attached.
    [[nodiscard]] Result<RestoreReport> restore(const std::string& snapshot_id,
                                                const RestoreOptions& options,
                                                ProgressCallback progress = nullptr);

    // --- Library management ------------------------------------------------

    [[nodiscard]] Result<std::vector<SnapshotSummary>> list(std::size_t limit = 50,
                                                            std::size_t offset = 0,
                                                            const std::string& tag_filter = {});
    [[nodiscard]] Result<Snapshot> get(const std::string& snapshot_id);
    [[nodiscard]] Result<Snapshot> latest();
    [[nodiscard]] Status remove(const std::string& snapshot_id);
    [[nodiscard]] Status rename(const std::string& snapshot_id, const std::string& name);
    [[nodiscard]] Status set_favorite(const std::string& snapshot_id, bool favorite);
    [[nodiscard]] Status set_tags(const std::string& snapshot_id,
                                  const std::vector<std::string>& tags);

    /// Full-text search over snapshot names, window titles and tab titles.
    [[nodiscard]] Result<std::vector<SnapshotSummary>> search(const std::string& query,
                                                              std::size_t limit = 25);

    /// Applies retention_days / max_snapshots and reclaims favicon cache space.
    [[nodiscard]] Result<std::uint32_t> prune();

    // --- Import / export ---------------------------------------------------

    [[nodiscard]] Result<std::string> export_snapshot(const std::string& snapshot_id,
                                                      const std::string& format);
    [[nodiscard]] Result<Snapshot> import_snapshot(const std::string& serialized,
                                                   const std::string& format,
                                                   const std::string& rename_to = {});

    // --- Diagnostics -------------------------------------------------------

    struct HealthReport {
        bool database_ok{false};
        bool platform_ok{false};
        bool browser_bridge_connected{false};
        std::vector<std::string> missing_permissions;
        std::vector<std::string> warnings;
        std::uint64_t database_bytes{0};
        std::uint32_t snapshot_count{0};
        std::string platform_name;
        std::string session_type;
    };

    [[nodiscard]] HealthReport health();

    [[nodiscard]] const Config& config() const noexcept { return config_; }

    void update_config(Config config);

private:
    struct CaptureContext;

    [[nodiscard]] Result<std::vector<WindowInfo>> capture_windows(const CaptureOptions& options);
    [[nodiscard]] Status attach_browser_tabs(std::vector<WindowInfo>& windows,
                                             const CaptureOptions& options);
    void sanitize(Snapshot& snapshot, const CaptureOptions& options) const;
    [[nodiscard]] bool is_excluded(const WindowInfo& window, const CaptureOptions& options) const;

    [[nodiscard]] Result<RestoreReport> execute_plan(const Snapshot& snapshot,
                                                     const RestorePlan& plan,
                                                     const RestoreOptions& options,
                                                     const ProgressCallback& progress);

    std::shared_ptr<hal::Platform> platform_;
    std::shared_ptr<storage::SnapshotRepository> repository_;
    std::shared_ptr<browser::Bridge> browser_bridge_;
    std::shared_ptr<privacy::Sanitizer> sanitizer_;
    Config config_;
};

/// True when `selectors` allow this window to participate in a restore.
[[nodiscard]] bool selector_matches(const WindowInfo& window,
                                    const std::vector<RestoreSelector>& selectors);

/// Parses "app:code", "tab:*.figma.com", "window:01H..." into a selector.
[[nodiscard]] Result<RestoreSelector> parse_selector(std::string_view text);

}  // namespace contextsnap::core
