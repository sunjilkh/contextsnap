// Thin Qt adapter over the daemon IPC client. Everything expensive lives in
// the daemon; this class only marshals results into roles QML can bind to.
#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

#include <contextsnap/core/types.hpp>
#include <contextsnap/ipc/client.hpp>

#include <memory>
#include <vector>

namespace contextsnap::gui {

/// List model backing both the quick switcher and the inspector.
class SnapshotModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        SubtitleRole,
        TagsRole,
        CreatedRole,
        WindowCountRole,
        TabCountRole,
        MonitorCountRole,
        FavoriteRole,
        AutomaticRole,
    };

    explicit SnapshotModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setSnapshots(std::vector<core::SnapshotSummary> snapshots);
    [[nodiscard]] QString idAt(int row) const;

private:
    std::vector<core::SnapshotSummary> snapshots_;
};

/// Facade exposed to QML as the `controller` context property.
class Controller final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString daemonVersion READ daemonVersion NOTIFY stateChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(contextsnap::gui::SnapshotModel* model READ model CONSTANT)

public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;

    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString daemonVersion() const { return daemon_version_; }
    [[nodiscard]] QString filter() const { return filter_; }
    void setFilter(const QString& value);
    [[nodiscard]] SnapshotModel* model() { return &model_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void capture(const QString& name, bool includeTabs = true);
    Q_INVOKABLE void restore(const QString& snapshotId, bool dryRun = false);
    Q_INVOKABLE void remove(const QString& snapshotId);
    Q_INVOKABLE void toggleFavorite(const QString& snapshotId, bool favorite);

signals:
    void stateChanged();
    void filterChanged();
    void toast(const QString& message);
    void activateRequested();

private:
    bool ensureClient();
    void applyFilter();
    void setStatus(const QString& text, bool ok);

    SnapshotModel model_;
    std::unique_ptr<ipc::Client> client_;
    std::vector<core::SnapshotSummary> all_;
    QString status_;
    QString daemon_version_;
    QString filter_;
    bool connected_{false};
};

}  // namespace contextsnap::gui
