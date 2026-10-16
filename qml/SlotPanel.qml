import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: panel

    required property int slotNumber
    required property var groups
    property bool expanded: true
    signal addRequested(int slotNumber)

    padding: 18
    background: Rectangle {
        color: "#ffffff"
        radius: 16
        border.color: "#dce4df"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                width: 34
                height: 34
                radius: 10
                color: panel.slotNumber === 1 ? "#d9ede5" : "#e5e4f4"

                Label {
                    anchors.centerIn: parent
                    text: panel.slotNumber
                    font.bold: true
                    color: panel.slotNumber === 1 ? "#166046" : "#4f4d86"
                }
            }

            Label {
                text: "Time Slot " + panel.slotNumber
                font.pixelSize: 17
                font.bold: true
                color: "#20372e"
                Layout.fillWidth: true
            }

            Label {
                text: panel.groups.length + " talkgroup" + (panel.groups.length === 1 ? "" : "s")
                color: "#64776c"
                font.pixelSize: 12
            }

            ToolButton {
                text: panel.expanded ? "−" : "+"
                Accessible.name: panel.expanded ? "Collapse time slot" : "Expand time slot"
                onClicked: panel.expanded = !panel.expanded
            }
        }

        ColumnLayout {
            visible: panel.expanded
            Layout.fillWidth: true
            spacing: 8

            Repeater {
                model: panel.groups

                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    radius: 10
                    color: "#f5f8f5"
                    border.color: "#e1e9e2"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Label {
                                text: modelData.name
                                font.bold: true
                                color: "#23372c"
                            }

                            Label {
                                text: "TG " + modelData.id
                                color: "#66776d"
                                font.pixelSize: 12
                            }
                        }

                        ToolButton {
                            text: "Remove"
                            Accessible.name: "Remove " + modelData.name + " from time slot " + panel.slotNumber
                            onClicked: store.removeTalkgroup(panel.slotNumber, modelData.id)
                        }
                    }
                }
            }

            Label {
                visible: panel.groups.length === 0
                text: "No talkgroups assigned"
                color: "#66776d"
                Layout.fillWidth: true
            }

            Button {
                text: "+ Add talkgroup"
                Layout.fillWidth: true
                onClicked: panel.addRequested(panel.slotNumber)
            }
        }
    }
}
