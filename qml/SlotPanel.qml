import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: panel

    required property int slotNumber
    required property var groups
    property bool importedChannels: false
    property bool canAdd: true
    property bool expanded: true
    readonly property var channelCards: {
        const cards = []
        for (const group of groups) {
            for (const channel of (group.channels || [])) {
                cards.push({
                    channelName: channel.name,
                    talkgroupName: group.name,
                    id: group.id,
                    groupCall: group.groupCall,
                    contactIndex: group.contactIndex,
                    zonePosition: channel.zonePosition
                })
            }
        }
        if (panel.importedChannels)
            cards.sort((a, b) => a.zonePosition - b.zonePosition)
        return cards
    }
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
                text: panel.channelCards.length + " talkgroup"
                      + (panel.channelCards.length === 1 ? "" : "s")
                color: "#64776c"
                font.pixelSize: 12
            }

            PageAction {
                text: panel.expanded ? "−" : "+"
                quiet: true
                compact: true
                Accessible.name: panel.expanded ? "Collapse time slot" : "Expand time slot"
                onClicked: panel.expanded = !panel.expanded
            }
        }

        Column {
            visible: panel.expanded
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.minimumWidth: 0
            spacing: 8

            Repeater {
                model: panel.channelCards

                delegate: Rectangle {
                    id: groupRow
                    required property var modelData
                    width: parent.width
                    height: 68
                    radius: 10
                    color: "#f5f8f5"
                    border.color: "#e1e9e2"

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 8
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    text: modelData.channelName
                                    font.bold: true
                                    color: "#23372c"
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: modelData.talkgroupName
                                    color: "#405b4b"
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: panel.importedChannels && modelData.id === 0
                                          ? "No contact reference"
                                          : (panel.importedChannels && !modelData.groupCall ? "Private ID " : "TG ") + modelData.id
                                    color: "#66776d"
                                    font.pixelSize: 12
                                }
                            }

                            PageAction {
                                objectName: "removeSlotChannelButton"
                                property int zonePosition: modelData.zonePosition || 0
                                text: "Remove"
                                quiet: true
                                Accessible.name: "Remove " + modelData.channelName + " from time slot " + panel.slotNumber
                                onClicked: {
                                    if (panel.importedChannels)
                                        store.removeImportedTalkgroup(panel.slotNumber, modelData.contactIndex, modelData.zonePosition)
                                    else
                                        store.removeTalkgroup(panel.slotNumber, modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            Label {
                visible: panel.channelCards.length === 0
                text: "No talkgroups assigned"
                color: "#66776d"
                width: parent.width
            }

            PageAction {
                objectName: "addTalkgroupButton"
                text: "+ Add talkgroup"
                primary: true
                width: parent.width
                enabled: panel.canAdd
                onClicked: panel.addRequested(panel.slotNumber)
            }
        }
    }
}
