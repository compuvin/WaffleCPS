import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: editor
    required property var group
    signal orderRequested()
    readonly property var channels: (group.zones || []).length ? group.zones[0].channels : []
    Connections {
        target: store
        function onAboutToResetProject() { renameDialog.close(); channelDialog.close() }
    }
    spacing: 12
    RowLayout {
        Layout.fillWidth: true
        Label { text: "ZONE · " + editor.channels.length + " / 16 channels"; color: "#64776c"; Layout.fillWidth: true }
        Button { text: "Rename"; onClicked: { renameField.text = editor.group.name; renameDialog.open() } }
    }
    Label {
        Layout.fillWidth: true
        text: "A zone is an ordered group of channels you select together on your radio. Use a custom zone to combine channels from different repeaters, simplex frequencies, or analog and digital systems—for example, everything you need for a trip or local event. The Repeaters view organizes channels that belong to one repeater."
        wrapMode: Text.WordWrap
        color: "#64776c"
    }
    Label { visible: editor.channels.length === 0; text: "No channels in this zone. Add an existing channel or create one."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
    Column {
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        Layout.minimumWidth: 0
        visible: editor.channels.length > 0
        spacing: 12
        Repeater {
            model: editor.channels
            delegate: Frame {
                required property var modelData
                width: parent.width
                padding: 12
                background: Rectangle { color: "white"; radius: 10; border.color: "#dce4df" }
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: modelData.zonePosition + ". " + modelData.name; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { text: modelData.power; color: "#64776c" }
                    }
                    Label { text: "RX " + modelData.receiveFrequency + " · TX " + modelData.transmitFrequency + " MHz"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#405b4b" }
                    Label {
                        text: modelData.mode === "Digital"
                            ? "Digital · CC " + modelData.colorCode + " · TS " + modelData.timeSlot
                              + (modelData.contactName ? " · " + modelData.contactName + " (" + modelData.contactId + ")" : " · No contact")
                            : modelData.mode
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#64776c"
                    }
                    RowLayout {
                        Item { Layout.fillWidth: true }
                        Button {
                            objectName: "zoneEditChannelButton"
                            property int zonePosition: modelData.zonePosition
                            text: "Edit"
                            onClicked: channelDialog.openForEdit(modelData)
                        }
                        Button {
                            objectName: "zoneRemoveChannelButton"
                            property int zonePosition: modelData.zonePosition
                            text: "Remove"
                            onClicked: store.removeSelectedZoneChannel(modelData.zonePosition)
                        }
                    }
                }
            }
        }
    }
    RowLayout {
        Button {
            objectName: "zoneAddChannelButton"
            text: "+ Add channel"
            enabled: editor.channels.length < 16
            onClicked: channelDialog.openForAdd()
        }
        Button { objectName: "zoneOrderChannelsButton"; text: "Order channels"; enabled: editor.channels.length > 1; onClicked: editor.orderRequested() }
    }

    Dialog {
        id: renameDialog
        title: "Rename zone"
        modal: true
        width: Math.min(400, editor.Window.window.width - 32)
        parent: Overlay.overlay
        anchors.centerIn: parent
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label { text: "Zone name" }
            TextField { id: renameField; maximumLength: 16; Layout.fillWidth: true }
            RowLayout {
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; onClicked: renameDialog.close() }
                Button { text: "Save"; enabled: renameField.text.trim().length > 0; onClicked: { if (store.renameSelectedZone(renameField.text)) renameDialog.close() } }
            }
        }
    }
    Dialog {
        id: channelDialog
        objectName: "zoneChannelDialog"
        property bool editing: false
        property int position: 0
        property int originalMode: 0
        property int selectedChannelNumber: 0
        property int selectedContactIndex: 0
        property var contactChoices: []
        readonly property var filteredContacts: {
            if (!visible || choosingExisting || channelMode.currentIndex !== 0) return []
            const terms = contactSearch.text.trim().toLowerCase().split(/\s+/).filter(t => t.length > 0)
            return contactChoices.filter(contact => {
                const text = (contact.name + " " + contact.id).toLowerCase()
                return terms.every(term => text.includes(term))
            })
        }
        readonly property string selectedContactText: {
            if (selectedContactIndex === 0) return "Select a contact"
            const selected = contactChoices.find(c => c.number === selectedContactIndex)
            return selected ? selected.name + " · " + selected.id + (selected.groupCall ? " · Group" : " · Private") : "Select a contact"
        }
        readonly property bool choosingExisting: !editing && addTabs.currentIndex === 0
        readonly property var filteredChannels: {
            if (!visible || !choosingExisting) return []
            const terms = channelSearch.text.trim().toLowerCase().split(/\s+/).filter(t => t.length > 0)
            return store.availableZoneChannels.filter(channel => {
                const text = [channel.name, channel.contactName || "", channel.contactId || "",
                    channel.receiveFrequency, channel.transmitFrequency].join(" ").toLowerCase()
                return terms.every(term => text.includes(term))
            })
        }
        function openForAdd() {
            editing = false
            position = 0
            channelError.text = ""
            selectedChannelNumber = 0
            channelSearch.text = ""
            existingChannels.currentIndex = -1
            addTabs.currentIndex = store.availableZoneChannels.length ? 0 : 1
            channelName.text = ""
            channelRx.text = ""
            channelTx.text = ""
            channelMode.currentIndex = 0
            channelCc.value = 1
            channelSlot.currentIndex = 0
            channelPower.currentIndex = 0
            contactChoices = store.zoneContactChoices()
            selectedContactIndex = 0
            contactSearch.text = ""
            open()
            if (choosingExisting) channelSearch.forceActiveFocus()
        }
        function openForEdit(channel) {
            editing = true
            position = channel.zonePosition
            channelError.text = ""
            channelName.text = channel.name
            channelRx.text = channel.receiveFrequency
            channelTx.text = channel.transmitFrequency
            originalMode = channel.mode === "Digital" ? 0 : 1
            channelMode.currentIndex = originalMode
            channelCc.value = channel.colorCode || 0
            channelSlot.currentIndex = channel.timeSlot === 2 ? 1 : 0
            channelPower.currentIndex = store.powerChoices.indexOf(channel.power)
            contactChoices = store.zoneContactChoices()
            selectedContactIndex = channel.contactIndex
            contactSearch.text = ""
            open()
        }
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, editor.Window.window.width - 32)
        height: Math.min(650, editor.Window.window.height - 40)
        modal: true
        title: editing ? "Edit channel" : "Add channel to zone"
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            spacing: 10
            TabBar {
                id: addTabs
                objectName: "zoneChannelTabs"
                visible: !channelDialog.editing
                Layout.fillWidth: true
                TabButton { text: "Existing channel" }
                TabButton { text: "New channel" }
            }
            ColumnLayout {
                visible: channelDialog.choosingExisting
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextField {
                    id: channelSearch
                    objectName: "zoneChannelSearch"
                    Layout.fillWidth: true
                    placeholderText: "Search by channel name, contact, ID, or frequency"
                    Accessible.name: "Search existing channels"
                    onTextChanged: { channelDialog.selectedChannelNumber = 0; existingChannels.currentIndex = -1 }
                    Keys.onDownPressed: { existingChannels.forceActiveFocus(); existingChannels.currentIndex = existingChannels.count ? 0 : -1 }
                }
                Label { text: channelDialog.filteredChannels.length + " channels"; color: "#64776c" }
                ListView {
                    id: existingChannels
                    objectName: "zoneExistingChannels"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    currentIndex: -1
                    model: channelDialog.filteredChannels
                    ScrollBar.vertical: ScrollBar {}
                    Keys.onReturnPressed: {
                        if (currentIndex >= 0) channelDialog.selectedChannelNumber = model[currentIndex].number
                    }
                    Keys.onSpacePressed: {
                        if (currentIndex >= 0) channelDialog.selectedChannelNumber = model[currentIndex].number
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        objectName: "zoneExistingChannelChoice"
                        property int channelNumber: modelData.number
                        width: existingChannels.width
                        height: 88
                        highlighted: channelDialog.selectedChannelNumber === modelData.number
                        Accessible.name: modelData.name + ", RX " + modelData.receiveFrequency + ", TX " + modelData.transmitFrequency
                        onClicked: { channelDialog.selectedChannelNumber = modelData.number; existingChannels.currentIndex = index }
                        contentItem: Column {
                            spacing: 3
                            Label { width: parent.width; text: modelData.name; font.bold: true; elide: Text.ElideRight }
                            Label { width: parent.width; text: "RX " + modelData.receiveFrequency + " · TX " + modelData.transmitFrequency + " MHz"; color: "#405b4b"; elide: Text.ElideRight }
                            Label {
                                width: parent.width
                                text: modelData.mode === "Digital"
                                    ? "Digital · CC " + modelData.colorCode + " · TS " + modelData.timeSlot
                                      + (modelData.contactName ? " · " + modelData.contactName + " (" + modelData.contactId + ")" : " · No contact")
                                    : modelData.mode
                                color: "#64776c"
                                elide: Text.ElideRight
                            }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        width: parent.width
                        visible: existingChannels.count === 0
                        text: channelSearch.text.trim().length ? "No matching channels." : "No additional existing channels. Use New channel."
                        wrapMode: Text.WordWrap
                        color: "#64776c"
                    }
                }
            }
            ScrollView {
                visible: !channelDialog.choosingExisting
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    spacing: 8
                    Label { text: "Channel name" }
                    TextField { id: channelName; objectName: "zoneChannelName"; maximumLength: 16; Layout.fillWidth: true }
                    Label { text: "Receive frequency (MHz)" }
                    TextField { id: channelRx; objectName: "zoneChannelRx"; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                    Label { text: "Transmit frequency (MHz)" }
                    TextField { id: channelTx; objectName: "zoneChannelTx"; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
                    RowLayout {
                        Label { text: "Mode" }
                        ComboBox { id: channelMode; objectName: "zoneChannelMode"; model: ["Digital", "Analog"]; Layout.fillWidth: true }
                        Label { text: "Power" }
                        ComboBox {
                            id: channelPower
                            objectName: "zoneChannelPower"
                            model: store.powerChoices
                            Layout.fillWidth: true
                            displayText: currentIndex < 0 ? "Unknown (unchanged)" : currentText
                        }
                    }
                    RowLayout {
                        visible: channelMode.currentIndex === 0
                        Label { text: "Color code" }
                        SpinBox { id: channelCc; objectName: "zoneChannelColorCode"; from: 0; to: 15 }
                        Label { text: "Time slot" }
                        ComboBox { id: channelSlot; objectName: "zoneChannelTimeSlot"; model: ["1", "2"] }
                    }
                    Label { visible: channelMode.currentIndex === 0; text: "Contact" }
                    ColumnLayout {
                        visible: channelMode.currentIndex === 0
                        Layout.fillWidth: true
                        Label {
                            text: channelDialog.selectedContactIndex > 0
                                  && channelDialog.selectedContactIndex <= channelDialog.contactChoices.length
                                  ? "Selected: " + channelDialog.selectedContactText : "Select a contact"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: "#405b4b"
                        }
                        TextField {
                            id: contactSearch
                            objectName: "zoneContactSearch"
                            Layout.fillWidth: true
                            placeholderText: "Search contacts by name or ID"
                            Accessible.name: "Search channel contacts"
                            onTextChanged: contactList.currentIndex = -1
                            Keys.onDownPressed: { contactList.forceActiveFocus(); contactList.currentIndex = contactList.count ? 0 : -1 }
                        }
                        ListView {
                            id: contactList
                            objectName: "zoneContactChoices"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 190
                            clip: true
                            currentIndex: -1
                            model: channelDialog.filteredContacts
                            ScrollBar.vertical: ScrollBar {}
                            Keys.onReturnPressed: {
                                if (currentIndex >= 0) channelDialog.selectedContactIndex = model[currentIndex].number
                            }
                            Keys.onSpacePressed: {
                                if (currentIndex >= 0) channelDialog.selectedContactIndex = model[currentIndex].number
                            }
                            delegate: ItemDelegate {
                                required property var modelData
                                required property int index
                                objectName: "zoneContactChoice"
                                property int contactNumber: modelData.number
                                width: contactList.width
                                height: 54
                                text: modelData.name + " · " + modelData.id + (modelData.groupCall ? " · Group" : " · Private")
                                highlighted: channelDialog.selectedContactIndex === modelData.number
                                onClicked: { channelDialog.selectedContactIndex = modelData.number; contactList.currentIndex = index }
                            }
                            Label {
                                anchors.centerIn: parent
                                visible: contactList.count === 0
                                width: parent.width
                                text: channelDialog.contactChoices.length === 0
                                      ? "Add a contact in Contacts to use a digital channel." : "No matching contacts."
                                wrapMode: Text.WordWrap
                                color: "#64776c"
                            }
                        }
                    }
                    Label {
                        visible: channelDialog.editing && channelMode.currentIndex !== channelDialog.originalMode
                        text: "Switching mode uses the radio defaults for settings specific to the new mode."
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        color: "#64776c"
                    }
                }
            }
            Label { id: channelError; visible: text.length > 0; color: "#a33a33"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Item { Layout.fillWidth: true }
                Button { objectName: "zoneChannelCancel"; text: "Cancel"; onClicked: channelDialog.close() }
                Button {
                    objectName: "zoneChannelSave"
                    text: channelDialog.editing ? "Save" : "Add channel"
                    enabled: channelDialog.choosingExisting ? channelDialog.selectedChannelNumber > 0
                             : channelMode.currentIndex !== 0
                               || (channelDialog.selectedContactIndex > 0
                                   && channelDialog.selectedContactIndex <= channelDialog.contactChoices.length)
                    onClicked: {
                        const saved = channelDialog.editing
                            ? store.updateSelectedZoneChannel(channelDialog.position, channelName.text, channelRx.text, channelTx.text,
                                channelMode.currentIndex === 0, channelCc.value, channelSlot.currentIndex + 1,
                                channelDialog.selectedContactIndex, channelPower.currentIndex < 0 ? -1 : store.powerChoice(channelPower.currentText))
                            : channelDialog.choosingExisting
                              ? store.addExistingChannelToSelectedZone(channelDialog.selectedChannelNumber)
                              : store.createChannelInSelectedZone(channelName.text, channelRx.text, channelTx.text,
                                  channelMode.currentIndex === 0, channelCc.value, channelSlot.currentIndex + 1,
                                  channelDialog.selectedContactIndex, store.powerChoice(channelPower.currentText))
                        if (saved) channelDialog.close()
                        else channelError.text = "Could not save this channel. Check its name, frequencies, and available capacity."
                    }
                }
            }
        }
    }
}
