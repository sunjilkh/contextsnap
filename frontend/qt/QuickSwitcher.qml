import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Keyboard-first snapshot list. Rendering stays cheap (no thumbnails yet) so
// the HUD appears within one frame of the hotkey.
FocusScope {
    id: control

    property alias model: list.model
    property alias currentIndex: list.currentIndex

    signal inspectRequested(string snapshotId, string name)

    function incrementCurrentIndex() { list.incrementCurrentIndex() }
    function decrementCurrentIndex() { list.decrementCurrentIndex() }

    function restoreCurrent() {
        if (list.currentIndex < 0 || list.count === 0)
            return
        const item = list.model.data(list.model.index(list.currentIndex, 0), 0x0101)
        controller.restore(item, false)
    }

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: "#161a21"
        border.color: "#242a35"

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 6
            clip: true
            focus: true
            currentIndex: 0
            keyNavigationWraps: true
            highlightMoveDuration: 90
            ScrollBar.vertical: ScrollBar {}

            highlight: Rectangle {
                radius: 8
                color: "#1f2937"
                border.color: "#4c7dff"
            }

            delegate: ItemDelegate {
                width: list.width
                height: 62
                highlighted: ListView.isCurrentItem

                onClicked: list.currentIndex = index
                onDoubleClicked: controller.restore(snapshotId, false)

                contentItem: RowLayout {
                    spacing: 12

                    Rectangle {
                        width: 34
                        height: 34
                        radius: 8
                        color: favorite ? "#3b2f16" : "#1d2330"
                        Label {
                            anchors.centerIn: parent
                            text: favorite ? "*" : String(windowCount)
                            color: favorite ? "#f0c674" : "#8ea0bb"
                            font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: name
                            color: "#eef1f6"
                            font.pixelSize: 14
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Label {
                            text: subtitle + (automatic ? qsTr(" - automatic") : "")
                            color: "#7e8899"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Row {
                        spacing: 4
                        Repeater {
                            model: tags
                            delegate: Rectangle {
                                radius: 4
                                color: "#212836"
                                height: 18
                                width: tagLabel.implicitWidth + 12
                                Label {
                                    id: tagLabel
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: "#93a4bf"
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }

                    ToolButton {
                        text: qsTr("Inspect")
                        onClicked: control.inspectRequested(snapshotId, name)
                    }

                    ToolButton {
                        text: qsTr("Restore")
                        onClicked: controller.restore(snapshotId, false)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                color: "#5d6675"
                text: controller.connected
                    ? qsTr("No snapshots yet.\nPress Ctrl+N or run: contextsnap save")
                    : qsTr("Waiting for contextsnapd...")
            }
        }
    }
}
