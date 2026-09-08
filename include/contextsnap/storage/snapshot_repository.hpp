// Persistence for the snapshot aggregate.
//
// A snapshot is written in a single IMMEDIATE transaction across five tables
// (snapshots, monitors, windows, tabs, cursor_states) so a crash mid-capture can
// never leave a half-snapshot behind. Reads are assembled with three queries and
// an in-memory join rather than a wide join, which keeps the row size small and
// avoids duplicating window columns per tab.
#pragma once

#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>
#include <contextsnap/storage/database.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace contextsnap::storage {

using core::Result;
using core::Snapshot;
using core::SnapshotSummary;
using core::Status;

struct ListQuery {
    std::size_t limit{50};
    std::size_t offset{0};
    std::optional<std::string> tag;
    std::optional<bool> favorite_only;
    std::optional<bool> include_automatic;
    std::optional<core::Timestamp> created_after;
    std::optional<core::Timestamp> created_before;
    enum class Order : std::uint8_t { NewestFirst, OldestFirst, NameAsc } order{Order::NewestFirst};
};

struct PruneQuery {
    std::uint32_t retention_days{0};  ///< 0 = no age limit.
    std::uint32_t max_snapshots{0};   ///< 0 = no count limit.
    bool keep_favorites{true};
};

class SnapshotRepository {
public:
    explicit SnapshotRepository(Database database);
    ~SnapshotRepository();

    SnapshotRepository(const SnapshotRepository&) = delete;
    SnapshotRepository& operator=(const SnapshotRepository&) = delete;

    /// Opens the database at `path`, runs migrations and returns the repository.
    [[nodiscard]] static Result<std::shared_ptr<SnapshotRepository>> open(
        const DatabaseOptions& options);

    /// Inserts a snapshot. Assigns metadata.id when empty.
    [[nodiscard]] Result<std::string> insert(const Snapshot& snapshot);

    /// Replaces the mutable metadata of an existing snapshot.
    [[nodiscard]] Status update_metadata(const core::SnapshotMetadata& metadata);

    [[nodiscard]] Result<Snapshot> load(const std::string& snapshot_id);

    /// Resolves a unique id prefix (`contextsnap show 01J8Z2`).
    [[nodiscard]] Result<std::string> resolve_id_prefix(const std::string& prefix);

    [[nodiscard]] Result<std::vector<SnapshotSummary>> list(const ListQuery& query);
    [[nodiscard]] Result<std::optional<SnapshotSummary>> latest();
    [[nodiscard]] Status remove(const std::string& snapshot_id);
    [[nodiscard]] Result<std::uint32_t> prune(const PruneQuery& query);
    [[nodiscard]] Result<std::uint32_t> count();

    /// FTS5 search over snapshot name, window title and tab title/url host.
    [[nodiscard]] Result<std::vector<SnapshotSummary>> search(const std::string& query,
                                                              std::size_t limit);

    /// Distinct tags with usage counts, for the GUI filter bar.
    [[nodiscard]] Result<std::vector<std::pair<std::string, std::uint32_t>>> tags();

    [[nodiscard]] Database& database() noexcept { return database_; }

    [[nodiscard]] Status maintenance();  ///< checkpoint + optional vacuum

private:
    [[nodiscard]] Status insert_monitors(const Snapshot& snapshot);
    [[nodiscard]] Status insert_windows(const Snapshot& snapshot);
    [[nodiscard]] Status insert_tabs(const std::string& snapshot_id, const core::WindowInfo& window);
    [[nodiscard]] Status insert_cursor(const Snapshot& snapshot);
    [[nodiscard]] Status index_for_search(const Snapshot& snapshot);

    [[nodiscard]] Result<std::vector<core::MonitorInfo>> load_monitors(const std::string& id);
    [[nodiscard]] Result<std::vector<core::WindowInfo>> load_windows(const std::string& id);
    [[nodiscard]] Status load_tabs(const std::string& snapshot_id,
                                   std::vector<core::WindowInfo>& windows);
    [[nodiscard]] Result<core::CursorState> load_cursor(const std::string& id);

    Database database_;
    mutable std::mutex mutex_;  ///< SQLite is serialized; this guards our own state.
};

}  // namespace contextsnap::storage
