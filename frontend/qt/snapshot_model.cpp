#include "snapshot_model.hpp"

#include <contextsnap/core/config.hpp>
#include <contextsnap/storage/serialization.hpp>

#include <QDateTime>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace contextsnap::gui {
namespace {

QString human_age(const core::Timestamp& when) {
    const auto millis = static_cast<qint64>(storage::to_unix_millis(when));
    const QDateTime stamp = QDateTime::fromMSecsSinceEpoch(millis);
    const qint64 seconds = stamp.secsTo(QDateTime::currentDateTime());
    if (seconds < 90) {
        return QObject::tr("just now");
    }
    if (seconds < 3600) {
        return QObject::tr("%1 min ago").arg(seconds / 60);
    }
    if (seconds < 86400) {
        return QObject::tr("%1 h ago").arg(seconds / 3600);
    }
    return stamp.toString(QStringLiteral("MMM d, HH:mm"));
}

QStringList to_string_list(const std::vector<std::string>& values) {
    QStringList list;
    list.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string& value : values) {
        list.append(QString::fromStdString(value));
    }
    return list;
}

}  // namespace

SnapshotModel::SnapshotModel(QObject* parent) : QAbstractListModel(parent) {}

int SnapshotModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(snapshots_.size());
}

QVariant SnapshotModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(snapshots_.size())) {
        return {};
    }
    const core::SnapshotSummary& summary = snapshots_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case IdRole:
            return QString::fromStdString(summary.id);
        case NameRole:
            return QString::fromStdString(summary.name.empty() ? summary.id : summary.name);
        case SubtitleRole:
            return tr("%1 windows - %2 tabs - %3")
                .arg(summary.window_count)
                .arg(summary.tab_count)
                .arg(human_age(summary.created_at));
        case TagsRole:
            return to_string_list(summary.tags);
        case CreatedRole:
            return human_age(summary.created_at);
        case WindowCountRole:
            return static_cast<int>(summary.window_count);
        case TabCountRole:
            return static_cast<int>(summary.tab_count);
        case MonitorCountRole:
            return static_cast<int>(summary.monitor_count);
        case FavoriteRole:
            return summary.favorite;
        case AutomaticRole:
            return summary.automatic;
        default:
            return {};
    }
}

QHash<int, QByteArray> SnapshotModel::roleNames() const {
    return {
        {IdRole, "snapshotId"},        {NameRole, "name"},
        {SubtitleRole, "subtitle"},    {TagsRole, "tags"},
        {CreatedRole, "created"},      {WindowCountRole, "windowCount"},
        {TabCountRole, "tabCount"},    {MonitorCountRole, "monitorCount"},
        {FavoriteRole, "favorite"},    {AutomaticRole, "automatic"},
    };
}

void SnapshotModel::setSnapshots(std::vector<core::SnapshotSummary> snapshots) {
    beginResetModel();
    snapshots_ = std::move(snapshots);
    endResetModel();
}

QString SnapshotModel::idAt(int row) const {
    if (row < 0 || row >= static_cast<int>(snapshots_.size())) {
        return {};
    }
    return QString::fromStdString(snapshots_[static_cast<std::size_t>(row)].id);
}

Controller::Controller(QObject* parent)
    : QObject(parent), status_(tr("Connecting to contextsnapd...")) {
    refresh();
}

Controller::~Controller() = default;

void Controller::setStatus(const QString& text, bool ok) {
    status_ = text;
    connected_ = ok;
    emit stateChanged();
}

bool Controller::ensureClient() {
    if (client_ && client_->connected()) {
        return true;
    }
    // Spawns the daemon on first use so launching the GUI is enough.
    auto client = ipc::connect_or_spawn_daemon(core::Config::with_defaults().ipc.socket_path);
    if (!client) {
        setStatus(tr("Daemon unreachable: %1")
                      .arg(QString::fromStdString(client.error().message)),
                  false);
        return false;
    }
    client_ = std::move(client.value());
    auto health = client_->health();
    if (health) {
        const core::json::Value* version = health.value().find("version");
        daemon_version_ =
            version == nullptr ? QString() : QString::fromStdString(std::string(version->as_string()));
    }
    setStatus(tr("Connected"), true);
    return true;
}

void Controller::refresh() {
    if (!ensureClient()) {
        model_.setSnapshots({});
        return;
    }
    auto snapshots = client_->list(200, 0, {});
    if (!snapshots) {
        setStatus(tr("List failed: %1").arg(QString::fromStdString(snapshots.error().message)),
                  false);
        return;
    }
    all_ = std::move(snapshots.value());
    applyFilter();
    setStatus(tr("%1 snapshots").arg(all_.size()), true);
}

void Controller::applyFilter() {
    if (filter_.trimmed().isEmpty()) {
        model_.setSnapshots(all_);
        return;
    }
    const std::string needle = filter_.trimmed().toLower().toStdString();
    std::vector<core::SnapshotSummary> filtered;
    for (const core::SnapshotSummary& summary : all_) {
        std::string haystack = summary.name + " " + summary.id;
        for (const std::string& tag : summary.tags) {
            haystack += " " + tag;
        }
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (haystack.find(needle) != std::string::npos) {
            filtered.push_back(summary);
        }
    }
    model_.setSnapshots(std::move(filtered));
}

void Controller::setFilter(const QString& value) {
    if (filter_ == value) {
        return;
    }
    filter_ = value;
    applyFilter();
    emit filterChanged();
}

void Controller::capture(const QString& name, bool includeTabs) {
    if (!ensureClient()) {
        return;
    }
    core::CaptureOptions options = core::Config::with_defaults().capture_options();
    options.include_tabs = includeTabs;
    if (!name.trimmed().isEmpty()) {
        options.name = name.trimmed().toStdString();
    }
    auto snapshot = client_->capture(options);
    if (!snapshot) {
        emit toast(tr("Capture failed: %1")
                       .arg(QString::fromStdString(snapshot.error().message)));
        return;
    }
    emit toast(tr("Captured %1 windows").arg(snapshot.value().windows.size()));
    refresh();
}

void Controller::restore(const QString& snapshotId, bool dryRun) {
    if (!ensureClient()) {
        return;
    }
    core::RestoreOptions options = core::Config::with_defaults().restore_options();
    options.dry_run = dryRun;
    auto report = client_->restore(snapshotId.toStdString(), options);
    if (!report) {
        emit toast(tr("Restore failed: %1").arg(QString::fromStdString(report.error().message)));
        return;
    }
    emit toast(dryRun ? tr("Dry run: %1 windows would be restored")
                            .arg(report.value().windows_restored)
                      : tr("Restored %1 windows, %2 tabs")
                            .arg(report.value().windows_restored)
                            .arg(report.value().tabs_restored));
}

void Controller::remove(const QString& snapshotId) {
    if (!ensureClient()) {
        return;
    }
    if (const core::Status removed = client_->remove(snapshotId.toStdString()); !removed) {
        emit toast(tr("Delete failed: %1").arg(QString::fromStdString(removed.error().message)));
        return;
    }
    emit toast(tr("Snapshot deleted"));
    refresh();
}

void Controller::toggleFavorite(const QString& snapshotId, bool favorite) {
    if (!ensureClient()) {
        return;
    }
    core::json::Value params = core::json::Value::object();
    params.set("snapshot_id", core::json::Value(snapshotId.toStdString()));
    params.set("favorite", core::json::Value(favorite));
    if (auto result = client_->call(ipc::Method::FavoriteSnapshot, params); !result) {
        emit toast(tr("Update failed: %1").arg(QString::fromStdString(result.error().message)));
        return;
    }
    refresh();
}

}  // namespace contextsnap::gui
