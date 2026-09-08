#include <contextsnap/core/snapshot_manager.hpp>

#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/ulid.hpp>
#include <contextsnap/storage/serialization.hpp>
#include <contextsnap/version.hpp>

#include <chrono>
#include <cstdlib>
#include <utility>

namespace contextsnap::core {
namespace {

std::string host_name() {
    for (const char* variable : {"CONTEXTSNAP_HOST", "COMPUTERNAME", "HOSTNAME", "HOST"}) {
        if (const char* value = std::getenv(variable); value != nullptr && *value != '\0') {
            return value;
        }
    }
    return "unknown-host";
}

std::uint32_t elapsed_ms(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start)
            .count());
}

}  // namespace

SnapshotManager::SnapshotManager(std::shared_ptr<hal::Platform> platform,
                                 std::shared_ptr<storage::SnapshotRepository> repository,
                                 std::shared_ptr<browser::Bridge> browser_bridge,
                                 std::shared_ptr<privacy::Sanitizer> sanitizer, Config config)
    : platform_(std::move(platform)),
      repository_(std::move(repository)),
      browser_bridge_(std::move(browser_bridge)),
      sanitizer_(std::move(sanitizer)),
      config_(std::move(config)) {}

SnapshotManager::~SnapshotManager() = default;

void SnapshotManager::update_config(Config config) { config_ = std::move(config); }

Result<std::vector<WindowInfo>> SnapshotManager::capture_windows(const CaptureOptions& options) {
    auto enumerated = platform_->windows().enumerate(options);
    if (!enumerated) {
        return enumerated.error();
    }
    std::vector<WindowInfo> windows;
    windows.reserve(enumerated.value().size());
    for (WindowInfo& window : enumerated.value()) {
        if (is_excluded(window, options)) {
            continue;
        }
        if (window.id.empty()) {
            window.id = generate_ulid();
        }
        if (window.restored_frame.empty()) {
            window.restored_frame = window.frame;
        }
        windows.push_back(std::move(window));
    }
    return windows;
}

bool SnapshotManager::is_excluded(const WindowInfo& window, const CaptureOptions& options) const {
    if (!options.include_minimized && window.state == WindowState::Minimized) {
        return true;
    }
    for (const std::string& pattern : options.excluded_app_ids) {
        if (privacy::glob_match(pattern, window.process.app_id) ||
            privacy::glob_match(pattern, window.process.executable_path)) {
            return true;
        }
    }
    return false;
}

Status SnapshotManager::attach_browser_tabs(std::vector<WindowInfo>& windows,
                                            const CaptureOptions& options) {
    if (browser_bridge_ == nullptr) {
        return Status::success();
    }
    const bool has_browser_window = std::any_of(
        windows.begin(), windows.end(),
        [](const WindowInfo& window) { return window.browser != BrowserKind::None; });
    if (!has_browser_window) {
        return Status::success();
    }

    // A slow or missing extension must never block a capture: the bridge is
    // given a hard deadline and we keep whatever it returned.
    auto captured = browser_bridge_->capture_tabs(options.browser_timeout, true);
    if (!captured) {
        log::warn("tab capture failed; windows are stored without tabs",
                  {log::field("error", captured.error().to_string())});
        return Status::success();
    }
    for (const std::string& warning : captured.value().warnings) {
        log::warn("browser bridge warning", {log::field("warning", warning)});
    }
    if (!options.include_incognito) {
        for (browser::BrowserWindow& browser_window : captured.value().windows) {
            if (browser_window.incognito) {
                browser_window.tabs.clear();
            }
        }
    }
    const std::size_t attached = browser::attach_tabs_to_windows(captured.value(), windows);
    log::debug("attached browser tabs",
               {log::field("windows", static_cast<std::int64_t>(attached))});
    return Status::success();
}

void SnapshotManager::sanitize(Snapshot& snapshot, const CaptureOptions& options) const {
    if (!options.sanitize_urls || sanitizer_ == nullptr) {
        return;
    }
    sanitizer_->sanitize_snapshot(snapshot);
    for (WindowInfo& window : snapshot.windows) {
        std::vector<TabInfo> kept;
        kept.reserve(window.tabs.size());
        for (TabInfo& tab : window.tabs) {
            bool excluded = false;
            for (const std::string& pattern : options.excluded_url_patterns) {
                if (privacy::glob_match(pattern, tab.url)) {
                    excluded = true;
                    break;
                }
            }
            if (!excluded) {
                kept.push_back(std::move(tab));
            }
        }
        window.tabs = std::move(kept);
    }
}

Result<Snapshot> SnapshotManager::capture(const CaptureOptions& options) {
    const auto started = std::chrono::steady_clock::now();

    auto monitors = platform_->displays().enumerate();
    if (!monitors) {
        return monitors.error();
    }
    auto windows = capture_windows(options);
    if (!windows) {
        return windows.error();
    }

    Snapshot snapshot;
    snapshot.monitors = std::move(monitors.value());
    snapshot.windows = std::move(windows.value());

    if (options.include_tabs) {
        if (const Status tabs = attach_browser_tabs(snapshot.windows, options); !tabs) {
            return tabs.error();
        }
    }
    if (platform_->capabilities().cursor_query) {
        if (auto cursor = platform_->cursor().query(); cursor) {
            snapshot.cursor = cursor.value();
        }
    }
    if (platform_->capabilities().virtual_desktops) {
        if (auto workspace = platform_->workspaces().current(); workspace) {
            snapshot.active_workspace_id = workspace.value();
        }
    }

    SnapshotMetadata& metadata = snapshot.metadata;
    metadata.id = generate_ulid();
    metadata.created_at = Clock::now();
    metadata.updated_at = metadata.created_at;
    metadata.platform = platform_->kind();
    metadata.session_type = platform_->session_type();
    metadata.host_name = host_name();
    metadata.os_version = platform_->os_version();
    metadata.app_version = std::string(kVersion);
    metadata.schema_version = kSnapshotSchemaVersion;

    sanitize(snapshot, options);
    metadata.capture_duration_ms = elapsed_ms(started);

    log::info("captured snapshot",
              {log::field("id", metadata.id),
               log::field("windows", static_cast<std::int64_t>(snapshot.windows.size())),
               log::field("tabs", static_cast<std::int64_t>(snapshot.tab_count())),
               log::field("duration_ms", static_cast<std::int64_t>(metadata.capture_duration_ms))});
    return snapshot;
}

Result<Snapshot> SnapshotManager::capture_and_store(const CaptureOptions& options) {
    auto snapshot = capture(options);
    if (!snapshot) {
        return snapshot.error();
    }
    auto inserted = repository_->insert(snapshot.value());
    if (!inserted) {
        return inserted.error();
    }
    // Retention runs after the insert so a capture is never lost to pruning.
    if (auto pruned = prune(); pruned && pruned.value() > 0) {
        log::info("pruned old snapshots",
                  {log::field("removed", static_cast<std::int64_t>(pruned.value()))});
    }
    return snapshot;
}

Result<std::vector<SnapshotSummary>> SnapshotManager::list(std::size_t limit, std::size_t offset,
                                                           const std::string& tag_filter) {
    storage::ListQuery query;
    query.limit = limit;
    query.offset = offset;
    if (!tag_filter.empty()) {
        query.tag = tag_filter;
    }
    return repository_->list(query);
}

Result<Snapshot> SnapshotManager::get(const std::string& snapshot_id) {
    auto resolved = repository_->resolve_id_prefix(snapshot_id);
    if (!resolved) {
        return resolved.error();
    }
    return repository_->load(resolved.value());
}

Result<Snapshot> SnapshotManager::latest() {
    auto summary = repository_->latest();
    if (!summary) {
        return summary.error();
    }
    if (!summary.value().has_value()) {
        return err::not_found("no snapshots have been captured yet", "core.manager");
    }
    return repository_->load(summary.value()->id);
}

Status SnapshotManager::remove(const std::string& snapshot_id) {
    auto resolved = repository_->resolve_id_prefix(snapshot_id);
    if (!resolved) {
        return resolved.error();
    }
    return repository_->remove(resolved.value());
}

namespace {

/// Metadata edits are read-modify-write: the repository replaces the whole
/// mutable metadata row, so we load the current values first.
Result<SnapshotMetadata> current_metadata(storage::SnapshotRepository& repository,
                                          const std::string& snapshot_id) {
    auto resolved = repository.resolve_id_prefix(snapshot_id);
    if (!resolved) {
        return resolved.error();
    }
    auto snapshot = repository.load(resolved.value());
    if (!snapshot) {
        return snapshot.error();
    }
    return snapshot.value().metadata;
}

}  // namespace

Status SnapshotManager::rename(const std::string& snapshot_id, const std::string& name) {
    auto metadata = current_metadata(*repository_, snapshot_id);
    if (!metadata) {
        return metadata.error();
    }
    metadata.value().name = name;
    metadata.value().updated_at = Clock::now();
    return repository_->update_metadata(metadata.value());
}

Status SnapshotManager::set_favorite(const std::string& snapshot_id, bool favorite) {
    auto metadata = current_metadata(*repository_, snapshot_id);
    if (!metadata) {
        return metadata.error();
    }
    metadata.value().favorite = favorite;
    metadata.value().updated_at = Clock::now();
    return repository_->update_metadata(metadata.value());
}

Status SnapshotManager::set_tags(const std::string& snapshot_id,
                                 const std::vector<std::string>& tags) {
    auto metadata = current_metadata(*repository_, snapshot_id);
    if (!metadata) {
        return metadata.error();
    }
    metadata.value().tags = tags;
    metadata.value().updated_at = Clock::now();
    return repository_->update_metadata(metadata.value());
}

Result<std::vector<SnapshotSummary>> SnapshotManager::search(const std::string& query,
                                                             std::size_t limit) {
    return repository_->search(query, limit);
}

Result<std::uint32_t> SnapshotManager::prune() {
    storage::PruneQuery query;
    query.retention_days = config_.storage.retention_days;
    query.max_snapshots = config_.storage.max_snapshots;
    query.keep_favorites = true;
    return repository_->prune(query);
}

Result<std::string> SnapshotManager::export_snapshot(const std::string& snapshot_id,
                                                     const std::string& format) {
    auto parsed = storage::format_from_string(format);
    if (!parsed) {
        return parsed.error();
    }
    auto snapshot = get(snapshot_id);
    if (!snapshot) {
        return snapshot.error();
    }
    if (parsed.value() == storage::Format::FlatBuffers) {
        return storage::serialize(snapshot.value(), parsed.value());
    }
    // JSON exports carry the envelope so importers can check format/version.
    return storage::wrap_document(storage::to_json(snapshot.value()));
}

Result<Snapshot> SnapshotManager::import_snapshot(const std::string& serialized,
                                                  const std::string& format,
                                                  const std::string& rename_to) {
    auto parsed = storage::format_from_string(format);
    if (!parsed) {
        return parsed.error();
    }
    auto snapshot = storage::deserialize(serialized, parsed.value());
    if (!snapshot) {
        return snapshot.error();
    }
    Snapshot imported = std::move(snapshot.value());
    if (!rename_to.empty()) {
        imported.metadata.name = rename_to;
    }
    // Imported ids may collide with local ones; mint a fresh ULID in that case.
    if (auto existing = repository_->load(imported.metadata.id); existing) {
        const std::string previous = imported.metadata.id;
        imported.metadata.id = generate_ulid();
        log::info("imported snapshot id collided; assigned a new id",
                  {log::field("previous", previous), log::field("id", imported.metadata.id)});
    }
    imported.metadata.updated_at = Clock::now();
    auto stored = repository_->insert(imported);
    if (!stored) {
        return stored.error();
    }
    return imported;
}

SnapshotManager::HealthReport SnapshotManager::health() {
    HealthReport report;
    report.platform_name = platform_->name();
    report.session_type = std::string(to_string(platform_->session_type()));
    report.platform_ok = platform_->capabilities().enumerate_windows;
    report.missing_permissions = platform_->missing_permissions();
    report.browser_bridge_connected =
        browser_bridge_ != nullptr && browser_bridge_->extension_connected();

    if (auto integrity = repository_->database().integrity_check(); integrity) {
        report.database_ok = integrity.value();
    } else {
        report.warnings.push_back("integrity check failed: " + integrity.error().to_string());
    }
    if (auto size = repository_->database().file_size_bytes(); size) {
        report.database_bytes = size.value();
    }
    if (auto count = repository_->count(); count) {
        report.snapshot_count = count.value();
    }
    for (const std::string& limitation : platform_->capabilities().limitations) {
        report.warnings.push_back(limitation);
    }
    if (!report.browser_bridge_connected) {
        report.warnings.emplace_back(
            "no browser extension is connected; tabs fall back to session files");
    }
    return report;
}

}  // namespace contextsnap::core
