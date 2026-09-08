import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Frameless HUD window. It behaves like a launcher: Esc hides it, Enter
// restores the highlighted snapshot, typing filters the list.
ApplicationWindow {
    id: root

    property bool inspectorOpen: false

    width: 720
    height: 480
    minimumWidth: 520
    minimumHeight: 320
    visible: false
    title: qsTr("ContextSnap")
    flags: Qt.Dialog | Qt.WindowStaysOnTopHint
    color: "#111318"

    Connections {
        target: controller
        function onActivateRequested() {
            root.show()
            root.raise()
            root.requestActivate()
            search.forceActiveFocus()
            search.selectAll()
        }
        function onToast(message) {
            toast.text = message
            toast.visible = true
            toastTimer.restart()
        }
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: root.hide()
    }

    Shortcut {
        sequence: "Ctrl+N"
        onActivated: controller.capture("", true)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            TextField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search snapshots by name or tag")
                text: controller.filter
                onTextChanged: controller.filter = text
                background: Rectangle {
                    radius: 8
                    color: "#1b1f27"
                    border.color: search.activeFocus ? "#4c7dff" : "#2a2f3a"
                }
                color: "#f2f4f8"
                Keys.onDownPressed: switcher.incrementCurrentIndex()
                Keys.onUpPressed: switcher.decrementCurrentIndex()
                Keys.onReturnPressed: switcher.restoreCurrent()
            }

            Button {
                text: qsTr("Capture")
                onClicked: controller.capture(search.text, true)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Capture the current context (Ctrl+N)")
            }
        }

        QuickSwitcher {
            id: switcher
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: controller.model
            onInspectRequested: function (snapshotId, name) {
                inspector.snapshotId = snapshotId
                inspector.snapshotName = name
                root.inspectorOpen = true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: controller.connected ? "#27ae60" : "#c0392b"
            }

            Label {
                text: controller.status
                color: "#9aa4b2"
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                text: controller.daemonVersion.length > 0
                    ? qsTr("daemon %1").arg(controller.daemonVersion)
                    : qsTr("v%1").arg(appVersion)
                color: "#5d6675"
            }
        }
    }

    SnapshotInspector {
        id: inspector
        anchors.fill: parent
        visible: root.inspectorOpen
        onClosed: root.inspectorOpen = false
    }

    Label {
        id: toast
        visible: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        padding: 10
        color: "#f2f4f8"
        background: Rectangle {
            radius: 8
            color: "#232936"
        }
    }

    Timer {
        id: toastTimer
        interval: 3500
        onTriggered: toast.visible = false
    }
}
