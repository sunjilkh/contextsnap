// ContextSnap Qt 6 / QML client.
//
// The GUI is deliberately a thin shell: a tray icon, a global-hotkey HUD and
// an inspector. All state lives in the daemon, so the window can be closed and
// reopened without losing anything, and the process idles under ~12 MB RSS.
#include "snapshot_model.hpp"

#include <contextsnap/core/config.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/version.hpp>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QMenu>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>

namespace {

/// Builds the tray menu. Keep the entries short: the tray is the primary
/// surface for users who never open the window.
std::unique_ptr<QMenu> build_tray_menu(contextsnap::gui::Controller& controller,
                                       QQmlApplicationEngine& engine) {
    auto menu = std::make_unique<QMenu>();

    QAction* capture = menu->addAction(QObject::tr("Capture context now"));
    QObject::connect(capture, &QAction::triggered, &controller,
                     [&controller] { controller.capture(QString(), true); });

    QAction* switcher = menu->addAction(QObject::tr("Quick switcher..."));
    QObject::connect(switcher, &QAction::triggered, &controller, [&controller] {
        controller.refresh();
        emit controller.activateRequested();
    });

    menu->addSeparator();

    QAction* refresh = menu->addAction(QObject::tr("Refresh"));
    QObject::connect(refresh, &QAction::triggered, &controller,
                     [&controller] { controller.refresh(); });

    QAction* quit = menu->addAction(QObject::tr("Quit ContextSnap"));
    QObject::connect(quit, &QAction::triggered, &engine, [] { QCoreApplication::quit(); });

    return menu;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ContextSnap"));
    QApplication::setApplicationVersion(
        QString::fromUtf8(contextsnap::kVersionString.data(),
                          static_cast<qsizetype>(contextsnap::kVersionString.size())));
    QApplication::setOrganizationName(QStringLiteral("ContextSnap"));
    QApplication::setDesktopFileName(QStringLiteral("contextsnap"));
    QApplication::setQuitOnLastWindowClosed(false);  // The tray keeps us alive.

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QObject::tr("Capture and restore your desktop context. Local-first."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption hidden(QStringList{QStringLiteral("hidden")},
                                    QObject::tr("Start in the tray without showing the window."));
    parser.addOption(hidden);
    parser.process(app);

    contextsnap::core::log::set_level(contextsnap::core::log::Level::Info);
    contextsnap::core::log::add_global_field(
        contextsnap::core::log::field("component", std::string("gui")));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    contextsnap::gui::Controller controller;
    engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
                                             QApplication::applicationVersion());
    engine.loadFromModule("ContextSnap", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    std::unique_ptr<QSystemTrayIcon> tray;
    std::unique_ptr<QMenu> menu;
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        menu = build_tray_menu(controller, engine);
        tray = std::make_unique<QSystemTrayIcon>(QIcon(QStringLiteral(":/icons/contextsnap.png")));
        tray->setToolTip(QObject::tr("ContextSnap"));
        tray->setContextMenu(menu.get());
        QObject::connect(tray.get(), &QSystemTrayIcon::activated, &controller,
                         [&controller](QSystemTrayIcon::ActivationReason reason) {
                             if (reason == QSystemTrayIcon::Trigger ||
                                 reason == QSystemTrayIcon::DoubleClick) {
                                 controller.refresh();
                                 emit controller.activateRequested();
                             }
                         });
        tray->show();

        QObject::connect(&controller, &contextsnap::gui::Controller::toast, tray.get(),
                         [&tray](const QString& message) {
                             tray->showMessage(QObject::tr("ContextSnap"), message,
                                               QSystemTrayIcon::Information, 4000);
                         });
    } else {
        contextsnap::core::log::warn("no system tray available; window mode only");
    }

    // Poll the daemon so the list stays fresh without a persistent event
    // subscription; a subscription is used once the HUD is visible.
    auto* poll = new QTimer(&app);
    QObject::connect(poll, &QTimer::timeout, &controller, [&controller] { controller.refresh(); });
    poll->start(30000);

    if (!parser.isSet(hidden)) {
        emit controller.activateRequested();
    }

    return QApplication::exec();
}
