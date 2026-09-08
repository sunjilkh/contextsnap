import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Selective restoration surface: pick what comes back instead of accepting
// the whole snapshot. Selectors map 1:1 onto the daemon's `selectors` param.
Item {
    id: inspector

    property string snapshotId: ""
    property string snapshotName: ""

    signal closed()

    Rectangle {
        anchors.fill: parent
        color: "#0b0d11"
        opacity: 0.94

        MouseArea {
            anchors.fill: parent
            onClicked: inspector.closed()
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 60, 560)
        height: Math.min(parent.height - 60, 420)
        radius: 12
        color: "#151922"
        border.color: "#252c39"

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 12

            RowLayout {
                Layout.fillWidth: true

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: inspector.snapshotName
                        color: "#eef1f6"
                        font.pixelSize: 16
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: inspector.snapshotId
                        color: "#5d6675"
                        font.pixelSize: 10
                        font.family: "monospace"
                    }
                }

                ToolButton {
                    text: qsTr("Close")
                    onClicked: inspector.closed()
                }
            }

            GroupBox {
                title: qsTr("Restore")
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    CheckBox {
                        id: launchApps
                        checked: true
                        text: qsTr("Launch applications that are not running")
                    }
                    CheckBox {
                        id: restoreTabs
                        checked: true
                        text: qsTr("Restore browser tabs")
                    }
                    CheckBox {
                        id: lazyTabs
                        checked: true
                        enabled: restoreTabs.checked
                        text: qsTr("Load tabs lazily (recommended above ~30 tabs)")
                    }
                    CheckBox {
                        id: restoreCursor
                        checked: true
                        text: qsTr("Restore cursor position and focus")
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: qsTr("Only restore (optional)")
                    color: "#9aa4b2"
                    font.pixelSize: 11
                }

                TextField {
                    id: selectors
                    Layout.fillWidth: true
                    placeholderText: qsTr("app:code, tab:*.figma.com, monitor:0")
                    color: "#f2f4f8"
                    background: Rectangle {
                        radius: 6
                        color: "#1b1f27"
                        border.color: selectors.activeFocus ? "#4c7dff" : "#2a2f3a"
                    }
                }

                Label {
                    text: qsTr("Same syntax as: contextsnap restore --only")
                    color: "#5d6675"
                    font.pixelSize: 10
                }
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    text: qsTr("Delete")
                    onClicked: {
                        controller.remove(inspector.snapshotId)
                        inspector.closed()
                    }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: qsTr("Dry run")
                    onClicked: controller.restore(inspector.snapshotId, true)
                }

                Button {
                    text: qsTr("Restore")
                    highlighted: true
                    onClicked: {
                        controller.restore(inspector.snapshotId, false)
                        inspector.closed()
                    }
                }
            }
        }
    }
}
