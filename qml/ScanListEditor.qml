import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: editor
    required property int scanIndex
    property var scanData: ({ valid: false, channels: [], repeaters: [], members: [] })
    signal renameRequested(int index)
    signal deleted()
    function refresh() { scanData = store.scanListEditorData(scanIndex) }
    onScanIndexChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: store
        function onImportedCodeplugChanged() { editor.refresh() }
        function onAboutToResetProject() { addDialog.close(); deleteDialog.close(); replaceDialog.close() }
    }
    spacing: 12
    RowLayout {
        Layout.fillWidth: true
        Label {
            text: editor.scanData.valid ? editor.scanData.name : "Scan lists"
            font.pixelSize: 26
            font.bold: true
            color: "#173d30"
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        PageAction { text: "Rename"; visible: editor.scanData.valid; onClicked: editor.renameRequested(editor.scanIndex) }
        PageAction { text: "Delete"; visible: editor.scanData.valid; onClicked: deleteDialog.open() }
    }
    Label {
        text: "A scan list tells your radio which channels to check while scanning."
        color: "#64776c"
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        visible: !editor.scanData.valid
        text: "Choose a scan list on the left, or add a new one."
        color: "#64776c"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
    TabBar {
        id: tabs
        visible: editor.scanData.valid
        Layout.fillWidth: true
        PageTabButton { text: "Channels to scan" }
        PageTabButton { text: "Use from these channels" }
    }
    Label {
        visible: editor.scanData.valid
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: "#64776c"
        text: tabs.currentIndex === 0
            ? (editor.scanData.members || []).length + " / " + editor.scanData.memberCapacity
              + " entries. Current channel follows the channel selected on your radio."
            : "Your radio needs to know which scan list to use. Select the channels where starting a scan should use this list. Each channel can use only one scan list."
    }
    Label {
        visible: editor.scanData.valid && tabs.currentIndex === 0
                 && !(editor.scanData.channels || []).some(channel => channel.scanListIndex === editor.scanIndex)
        text: "No channels use this scan list yet. Choose them in “Use from these channels” before writing to your radio."
        color: "#805715"
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    PageField {
        id: assignmentSearch
        visible: editor.scanData.valid && tabs.currentIndex === 1
        Layout.fillWidth: true
        placeholderText: "Search channels"
    }
    ListView {
        objectName: "scanListMembers"
        visible: editor.scanData.valid && tabs.currentIndex === 0
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: 6
        model: editor.scanData.members || []
        ScrollBar.vertical: ScrollBar {}
        delegate: Frame {
            required property var modelData
            width: ListView.view.width
            padding: 10
            background: Rectangle { color: "white"; radius: 10; border.color: "#dce4df" }
            contentItem: RowLayout {
                Label {
                    text: (modelData.position + 1) + ". " + modelData.name
                    Layout.fillWidth: true
                    color: "#173d30"
                    elide: Text.ElideRight
                }
                PageAction {
                    text: "Up"
                    enabled: modelData.position > 0
                    onClicked: store.moveScanListMember(editor.scanIndex, modelData.position, modelData.position - 1)
                }
                PageAction {
                    text: "Down"
                    enabled: modelData.position + 1 < editor.scanData.members.length
                    onClicked: store.moveScanListMember(editor.scanIndex, modelData.position, modelData.position + 1)
                }
                PageAction {
                    text: "Remove"
                    onClicked: store.removeScanListMember(editor.scanIndex, modelData.position)
                }
            }
        }
    }
    ListView {
        objectName: "scanListAssignments"
        visible: editor.scanData.valid && tabs.currentIndex === 1
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: (editor.scanData.channels || []).filter(channel => channel.name.toLowerCase().includes(assignmentSearch.text.toLowerCase()))
        ScrollBar.vertical: ScrollBar {}
        delegate: Item {
            required property var modelData
            width: ListView.view.width
            implicitHeight: channelCheck.implicitHeight
            PageCheckBox {
                id: channelCheck
                width: parent.width
                text: modelData.name + (modelData.scanListIndex >= 0 && modelData.scanListIndex !== editor.scanIndex
                                      ? " · Uses " + modelData.scanListName : "")
                checked: modelData.scanListIndex === editor.scanIndex
                onClicked: {
                    if (modelData.scanListIndex >= 0 && modelData.scanListIndex !== editor.scanIndex) {
                        replaceDialog.channelNumber = modelData.number
                        replaceDialog.channelName = modelData.name
                        replaceDialog.previousName = modelData.scanListName
                        replaceDialog.open()
                    } else store.assignChannelScanList(modelData.number,
                                modelData.scanListIndex === editor.scanIndex ? -1 : editor.scanIndex)
                    checked = Qt.binding(() => modelData.scanListIndex === editor.scanIndex)
                }
            }
        }
    }
    RowLayout {
        visible: editor.scanData.valid && tabs.currentIndex === 0
        PageAction { text: "+ Add channels"; primary: true; onClicked: addDialog.showChannels(false) }
        PageAction { text: "+ Add repeater"; onClicked: addDialog.showChannels(true) }
        PageAction { text: "+ Current channel"; onClicked: store.addScanListChannels(editor.scanIndex, [0]) }
    }
    Item { visible: !editor.scanData.valid; Layout.fillHeight: true }

    PageDialog {
        id: addDialog
        property bool repeaters: false
        property var selectedNumbers: []
        function showChannels(fromRepeaters) {
            repeaters = fromRepeaters
            selectedNumbers = []
            search.text = ""
            open()
        }
        title: repeaters ? "Add channels from a repeater" : "Add channels"
        modal: true
        width: Math.min(620, editor.Window.window.width - 40)
        height: Math.min(540, editor.Window.window.height - 40)
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            PageField { id: search; placeholderText: "Search"; Layout.fillWidth: true }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: (addDialog.repeaters ? editor.scanData.repeaters || [] : editor.scanData.channels || [])
                       .filter(item => item.name.toLowerCase().includes(search.text.toLowerCase()))
                ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: channelCheck.implicitHeight
                    PageCheckBox {
                        id: channelCheck
                        width: parent.width
                        text: modelData.name
                        checked: addDialog.repeaters
                                 ? (modelData.numbers || []).every(number => addDialog.selectedNumbers.includes(number))
                                 : addDialog.selectedNumbers.includes(modelData.number)
                        onClicked: {
                            const numbers = addDialog.repeaters ? modelData.numbers : [modelData.number]
                            let selected = addDialog.selectedNumbers.slice()
                            const remove = numbers.every(number => selected.includes(number))
                            for (const number of numbers) {
                                if (remove) selected = selected.filter(value => value !== number)
                                else if (!selected.includes(number)) selected.push(number)
                            }
                            addDialog.selectedNumbers = selected
                            checked = Qt.binding(() => addDialog.repeaters
                                ? (modelData.numbers || []).every(number => addDialog.selectedNumbers.includes(number))
                                : addDialog.selectedNumbers.includes(modelData.number))
                        }
                    }
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                PageAction { text: "Cancel"; onClicked: addDialog.close() }
                PageAction {
                    text: "Add selected"
                    primary: true
                    enabled: addDialog.selectedNumbers.length > 0
                    onClicked: {
                        if (store.addScanListChannels(editor.scanIndex, addDialog.selectedNumbers)) addDialog.close()
                    }
                }
            }
        }
    }
    PageDialog {
        id: deleteDialog
        title: "Delete scan list"
        modal: true
        width: Math.min(460, editor.Window.window.width - 40)
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: "Delete this scan list? Channels using it will be left without an assigned scan list. The channels themselves will remain."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                PageAction { text: "Cancel"; onClicked: deleteDialog.close() }
                PageAction {
                    text: "Delete"
                    primary: true
                    onClicked: {
                        if (store.deleteScanList(editor.scanIndex)) { deleteDialog.close(); editor.deleted() }
                    }
                }
            }
        }
    }
    PageDialog {
        id: replaceDialog
        property int channelNumber: 0
        property string channelName: ""
        property string previousName: ""
        title: "Replace scan-list assignment"
        modal: true
        width: Math.min(460, editor.Window.window.width - 40)
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: replaceDialog.channelName + " currently uses “" + replaceDialog.previousName
                      + "” when scanning starts. Use “" + editor.scanData.name + "” instead?"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                PageAction { text: "Cancel"; onClicked: replaceDialog.close() }
                PageAction {
                    text: "Replace"
                    primary: true
                    onClicked: {
                        if (store.assignChannelScanList(replaceDialog.channelNumber, editor.scanIndex, true)) replaceDialog.close()
                    }
                }
            }
        }
    }
}
