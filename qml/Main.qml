import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    property bool clearingProject: false
    property string currentSection: "repeaters"
    onClosing: function(event) {
        if (radioReader.busy) {
            event.accepted = false
            radioReader.cancel()
        }
    }
    Connections {
        target: store
        function onAboutToResetProject() {
            createZoneDialog.close()
            channelOrderDialog.close()
            window.clearingProject = true
            window.currentSection = "repeaters"
        }
        function onProjectChanged() {
            if (window.clearingProject)
                Qt.callLater(() => window.clearingProject = false)
        }
    }
    width: 1080
    height: 720
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: store.projectDisplayName + (store.modified ? " *" : "") + " — WaffleCPS"
    color: "#f2f5f1"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        enabled: !radioReader.busy

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(66, headerActions.implicitHeight + 24)
            color: "#173d30"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                spacing: 14

                Image {
                    source: "qrc:/assets/waffle-radio.png"
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 42
                    fillMode: Image.PreserveAspectFit
                    Accessible.name: "Waffle and portable radio"
                }

                Label {
                    text: "WaffleCPS"
                    color: "white"
                    font.pixelSize: 22
                    font.bold: true
                }

                Rectangle {
                    width: 92
                    height: 24
                    radius: 12
                    color: "#315d4b"

                    Label {
                        anchors.centerIn: parent
                        text: "PROTOTYPE"
                        color: "#d7eee0"
                        font.pixelSize: 10
                        font.bold: true
                    }
                }

                Flow {
                    id: headerActions
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: implicitHeight
                    spacing: 6
                    Button { text: "New"; onClicked: store.newProjectDialog() }
                    Button { text: "Open"; onClicked: store.openProjectDialog() }
                    Button { text: "Import codeplug"; onClicked: store.importCodeplugDialog() }
                    Button {
                        objectName: "readRadioButton"
                        text: "Read from radio"
                        onClicked: {
                            if (store.modified)
                                radioReadConfirmation.open()
                            else
                                radioReader.start()
                        }
                    }
                    Button { text: "Save"; onClicked: store.saveProjectDialog(false) }
                    Button { text: "Save As"; onClicked: store.saveProjectDialog(true) }
                    Button {
                        text: "Export"
                        ToolTip.text: "Export CS7000 codeplug"
                        ToolTip.visible: hovered
                        onClicked: store.exportCodeplugDialog()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Loader {
                active: !store.hasImportedCodeplug && !window.clearingProject
                visible: active
                Layout.preferredWidth: 242
                Layout.fillHeight: true
                sourceComponent: Component { Rectangle {
                color: "#ffffff"
                border.color: "#e0e8e0"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10

                    Button {
                        text: "Contacts"
                        Layout.fillWidth: true
                        highlighted: window.currentSection === "contacts"
                        onClicked: { store.leaveZoneEditor(); window.currentSection = "contacts" }
                    }

                    Button {
                        objectName: "radioSettingsNav"
                        text: "Radio Settings"
                        Layout.fillWidth: true
                        highlighted: window.currentSection === "settings"
                        onClicked: { store.leaveZoneEditor(); window.currentSection = "settings" }
                    }

                    Label {
                        text: "REPEATERS"
                        color: "#697c71"
                        font.pixelSize: 11
                        font.bold: true
                        Layout.bottomMargin: 4
                    }

                    Repeater {
                        model: store.repeaterNames

                        delegate: ItemDelegate {
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            text: modelData
                            highlighted: index === store.selectedIndex
                            onClicked: {
                                store.selectRepeater(index)
                                window.currentSection = "repeaters"
                            }
                        }
                    }

                    Button {
                        text: "+ Add repeater"
                        Layout.fillWidth: true
                        onClicked: {
                            window.currentSection = "repeaters"
                            editDialog.openForNew()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        Layout.topMargin: 10
                        color: "#e1e8e1"
                    }

                    Label {
                        text: "ZONES"
                        color: "#697c71"
                        font.pixelSize: 11
                        font.bold: true
                        Layout.topMargin: 8
                    }

                    Button {
                        objectName: "createZoneButton"
                        text: "+ Add zone"
                        Layout.fillWidth: true
                        onClicked: createZoneDialog.openForNew()
                    }

                    Item { Layout.fillHeight: true }

                    Button {
                        text: "Attach original codeplug"
                        Layout.fillWidth: true
                        onClicked: store.attachSourceDialog()
                    }

                    Button {
                        visible: store.sourceCodeplugName.length > 0
                        text: "Import attached codeplug"
                        Layout.fillWidth: true
                        onClicked: store.importAttachedCodeplug()
                    }

                    Label {
                        text: store.sourceCodeplugName.length > 0
                              ? "Archived source: " + store.sourceCodeplugName
                              : "No source codeplug attached"
                        color: "#64776c"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.pixelSize: 11
                    }

                    Label {
                        text: store.projectDisplayName + (store.modified ? " · Unsaved changes" : "")
                        color: "#839188"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.pixelSize: 11
                    }
                }
            }

            } }

            Loader {
                active: store.hasImportedCodeplug && !window.clearingProject
                visible: active
                Layout.preferredWidth: 260
                Layout.fillHeight: true
                sourceComponent: Component { Rectangle {
                color: "#ffffff"
                border.color: "#e0e8e0"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 8

                    Button {
                        text: "Contacts"
                        Layout.fillWidth: true
                        highlighted: window.currentSection === "contacts"
                        onClicked: { store.leaveZoneEditor(); window.currentSection = "contacts" }
                    }
                    Button {
                        objectName: "radioSettingsNav"
                        text: "Radio Settings"
                        Layout.fillWidth: true
                        highlighted: window.currentSection === "settings"
                        onClicked: { store.leaveZoneEditor(); window.currentSection = "settings" }
                    }
                    Label { text: "REPEATERS"; color: "#697c71"; font.bold: true; font.pixelSize: 11 }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        ColumnLayout {
                            width: 220
                            spacing: 3
                            Repeater {
                                model: store.importedRepeaterNames
                                delegate: ItemDelegate {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    Layout.fillWidth: true
                                    highlighted: store.selectedImportedIndex === index
                                    onClicked: {
                                        store.selectImportedGroup(index)
                                        window.currentSection = "repeaters"
                                    }
                                }
                            }
                            Button {
                                text: "+ Add repeater"
                                Layout.fillWidth: true
                                onClicked: { store.leaveZoneEditor(); editDialog.openForNew() }
                            }
                            Label { text: "ZONES"; color: "#697c71"; font.bold: true; font.pixelSize: 11; Layout.topMargin: 12 }
                            Repeater {
                                model: store.importedZoneNames
                                delegate: ItemDelegate {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    Layout.fillWidth: true
                                    highlighted: store.selectedImportedIndex === index + store.importedRepeaterNames.length
                                    onClicked: {
                                        store.selectImportedGroup(index + store.importedRepeaterNames.length)
                                        window.currentSection = "repeaters"
                                    }
                                }
                            }
                        }
                    }
                    Button {
                        objectName: "createZoneButton"
                        text: "+ Add zone"
                        Layout.fillWidth: true
                        onClicked: createZoneDialog.openForNew()
                    }
                    Label {
                        text: store.importedCodeplugSummary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#64776c"
                        font.pixelSize: 11
                    }
                }
            } } }

            Loader {
                active: !store.hasImportedCodeplug && !window.clearingProject
                visible: active && window.currentSection === "repeaters"
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: Component { ScrollView {
                id: workspace
                clip: true

                ColumnLayout {
                    width: Math.max(0, workspace.availableWidth - 40)
                    x: 20
                    spacing: 20

                    Item { Layout.preferredHeight: 10 }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            text: store.repeaterNames.length > 0 ? store.selectedName : "New project"
                            font.pixelSize: 30
                            font.bold: true
                            color: "#183b2d"
                        }

                        Label {
                            text: store.repeaterNames.length > 0 ? "Repeater overview" : "Add a repeater to get started."
                            color: "#718379"
                        }
                        Button {
                            visible: store.repeaterNames.length === 0
                            text: "+ Add repeater"
                            onClicked: editDialog.openForNew()
                        }
                    }

                    Frame {
                        visible: store.repeaterNames.length > 0
                        Layout.fillWidth: true
                        padding: 20
                        background: Rectangle {
                            color: "#ffffff"
                            radius: 16
                            border.color: "#dce4df"
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10

                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "REPEATER DETAILS"; color: "#6f8175"; font.pixelSize: 11; font.bold: true; Layout.fillWidth: true }
                                Button { text: "Edit"; onClicked: editDialog.openForCurrent() }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 30

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "RECEIVE"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: store.rxFrequency + " MHz"; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "TRANSMIT"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: store.txFrequency + " MHz"; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "COLOR CODE"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: store.colorCode; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "TRANSMIT POWER"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: store.selectedPower; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }
                            }
                        }
                    }

                    Label {
                        visible: store.repeaterNames.length > 0
                        text: "TIME SLOTS"
                        color: "#697c71"
                        font.pixelSize: 11
                        font.bold: true
                    }

                    RowLayout {
                        visible: store.repeaterNames.length > 0
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 16

                        SlotPanel {
                            objectName: "manualSlot1"
                            slotNumber: 1
                            groups: store.slot1Talkgroups
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            onAddRequested: slot => addDialog.openForSlot(slot)
                        }

                        SlotPanel {
                            objectName: "manualSlot2"
                            slotNumber: 2
                            groups: store.slot2Talkgroups
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            onAddRequested: slot => addDialog.openForSlot(slot)
                        }
                    }

                    RowLayout {
                        visible: store.repeaterNames.length > 0
                        Button {
                            objectName: "manualOrderChannelsButton"
                            text: "Order channels"
                            onClicked: channelOrderDialog.openForSelectedRepeater()
                        }
                        Button {
                            objectName: "manualEditAsZoneButton"
                            text: "Edit as zone (advanced)"
                            onClicked: store.editSelectedRepeaterAsZone()
                        }
                    }

                    Frame {
                        visible: store.repeaterNames.length > 0
                        Layout.fillWidth: true
                        padding: 16
                        background: Rectangle { color: "white"; radius: 12; border.color: "#dce4df" }
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: "ADDITIONAL SIMPLEX FREQUENCIES"
                                    color: "#697c71"
                                    font.bold: true
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Button { text: "+ Add simplex"; onClicked: simplexDialog.openForCurrent(false) }
                            }
                            Label {
                                visible: store.selectedSimplexFrequencies.length === 0
                                text: "No additional simplex frequencies."
                                color: "#64776c"
                            }
                            Column {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 0
                                Layout.minimumWidth: 0
                                visible: store.selectedSimplexFrequencies.length > 0
                                spacing: 8
                                Repeater {
                                    model: store.selectedSimplexFrequencies
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        width: parent.width
                                        height: 44
                                        radius: 8
                                        color: "#f5f8f5"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            Label { text: modelData.name; Layout.fillWidth: true }
                                            Label { text: modelData.frequency + " MHz"; color: "#64776c" }
                                            Label {
                                                text: modelData.mode === "Digital"
                                                      ? "CC " + modelData.colorCode + " · TS " + modelData.timeSlot
                                                      : modelData.mode
                                                color: "#64776c"
                                            }
                                            ToolButton {
                                                text: "Remove"
                                                Accessible.name: "Remove " + modelData.name + " from additional simplex frequencies"
                                                onClicked: store.removeSimplexFromSelectedRepeater(index)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item { Layout.preferredHeight: 10 }
                }
            }

            } }

            Loader {
                active: store.hasImportedCodeplug && !window.clearingProject
                visible: active && window.currentSection === "repeaters"
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: Component { Item {
                ScrollView {
                id: importedWorkspace
                anchors.fill: parent
                clip: true

                ColumnLayout {
                    id: importedContent
                    property var group: store.selectedImportedGroup
                    readonly property bool showZoneEditor: store.editingAsZone || !(group.isRepeater || false)
                    width: Math.max(0, importedWorkspace.availableWidth - 40)
                    x: 20
                    spacing: 20

                    Item { Layout.preferredHeight: 10 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            text: importedContent.group.name || "Imported codeplug"
                            font.pixelSize: 30
                            font.bold: true
                            color: "#183b2d"
                        }
                        Label {
                            text: !importedContent.showZoneEditor
                                  ? "Repeater overview"
                                  : (importedContent.group.classification || "Zone overview")
                            color: "#718379"
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    Label {
                        visible: !importedContent.showZoneEditor
                        text: "ZONE · " + importedContent.group.zoneChannelCount + " / 16 channels"
                        color: "#64776c"
                    }
                    Frame {
                        visible: !importedContent.showZoneEditor
                        Layout.fillWidth: true
                        padding: 20
                        background: Rectangle { color: "white"; radius: 16; border.color: "#dce4df" }
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: "REPEATER DETAILS"; color: "#6f8175"; font.pixelSize: 11; font.bold: true; Layout.fillWidth: true }
                                Button { text: "Edit"; onClicked: editDialog.openForImported() }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 30
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "RECEIVE"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: importedContent.group.receiveFrequency + " MHz"; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "TRANSMIT"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: importedContent.group.transmitFrequency + " MHz"; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "COLOR CODE"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: importedContent.group.colorCode ?? ""; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: "TRANSMIT POWER"; color: "#6f8175"; font.pixelSize: 11; font.bold: true }
                                    Label { text: importedContent.group.power || ""; color: "#1e3c2d"; font.pixelSize: 19; font.bold: true }
                                }
                            }
                        }
                    }
                    Label {
                        visible: !importedContent.showZoneEditor
                        text: "TIME SLOTS"
                        color: "#697c71"
                        font.pixelSize: 11
                        font.bold: true
                    }
                    RowLayout {
                        visible: !importedContent.showZoneEditor
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 16
                        SlotPanel {
                            objectName: "importedSlot1"
                            slotNumber: 1
                            groups: importedContent.group.slot1Talkgroups || []
                            importedChannels: true
                            canAdd: importedContent.group.zoneChannelCount < 16
                                    && importedContent.group.power !== "Unknown"
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            onAddRequested: slot => addDialog.openForSlot(slot, true)
                        }
                        SlotPanel {
                            objectName: "importedSlot2"
                            slotNumber: 2
                            groups: importedContent.group.slot2Talkgroups || []
                            importedChannels: true
                            canAdd: importedContent.group.zoneChannelCount < 16
                                    && importedContent.group.power !== "Unknown"
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            onAddRequested: slot => addDialog.openForSlot(slot, true)
                        }
                    }
                    Label {
                        visible: !importedContent.showZoneEditor
                                 && importedContent.group.zoneChannelCount < 16
                                 && importedContent.group.power === "Unknown"
                        text: "Choose High or Low transmit power in Repeater details before adding a talkgroup."
                        color: "#64776c"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        visible: !importedContent.showZoneEditor
                        Button {
                            objectName: "orderChannelsButton"
                            text: "Order channels"
                            enabled: (importedContent.group.zones || []).length === 1
                            onClicked: channelOrderDialog.openForSelectedZone()
                            ToolTip.visible: hovered
                            ToolTip.text: "Arrange the radio's channel order for this zone"
                        }
                        Button {
                            objectName: "importedEditAsZoneButton"
                            text: "Edit as zone (advanced)"
                            onClicked: store.editSelectedRepeaterAsZone()
                        }
                    }
                    ZoneEditor {
                        Layout.fillWidth: true
                        visible: importedContent.showZoneEditor
                        group: importedContent.group
                        onOrderRequested: channelOrderDialog.openForSelectedZone()
                    }
                    Frame {
                        visible: !importedContent.showZoneEditor
                        Layout.fillWidth: true
                        padding: 16
                        background: Rectangle { color: "white"; radius: 12; border.color: "#dce4df" }
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: "ADDITIONAL SIMPLEX FREQUENCIES"
                                    color: "#697c71"
                                    font.bold: true
                                    font.pixelSize: 12
                                    Layout.fillWidth: true
                                }
                                Button {
                                    text: "+ Add simplex"
                                    enabled: importedContent.group.zoneChannelCount < 16
                                    onClicked: simplexDialog.openForCurrent(true)
                                }
                            }
                            Label {
                                visible: (importedContent.group.simplexChannels || []).length === 0
                                text: "No additional simplex frequencies."
                                color: "#64776c"
                            }
                            Column {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 0
                                Layout.minimumWidth: 0
                                visible: (importedContent.group.simplexChannels || []).length > 0
                                spacing: 8
                                Repeater {
                                    model: importedContent.group.simplexChannels || []
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: parent.width
                                        height: 44
                                        radius: 8
                                        color: "#f5f8f5"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            Label { text: modelData.name; Layout.fillWidth: true; elide: Text.ElideRight }
                                            Label { text: modelData.receiveFrequency + " MHz"; color: "#64776c" }
                                            Label {
                                                text: modelData.mode === "Digital"
                                                      ? "CC " + modelData.colorCode + " · TS " + modelData.timeSlot
                                                      : modelData.mode
                                                color: "#64776c"
                                            }
                                            ToolButton {
                                                text: "Remove"
                                                Accessible.name: "Remove " + modelData.name + " from additional simplex frequencies"
                                                onClicked: store.removeSimplexFromSelectedImportedRepeater(
                                                               modelData.zoneInGroup, modelData.zonePosition)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Item { Layout.preferredHeight: 10 }
                }
                }
            } } }

            Loader {
                visible: !window.clearingProject && window.currentSection === "contacts"
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: ContactsView { contacts: store.contacts }
            }

            Loader {
                active: !window.clearingProject
                visible: active && window.currentSection === "settings"
                Layout.fillWidth: true
                Layout.fillHeight: true
                sourceComponent: ScrollView {
                    clip: true
                    ColumnLayout {
                        width: Math.max(0, parent.width - 48)
                        x: 24
                        spacing: 16
                        Item { Layout.preferredHeight: 12 }
                        Label { text: "Radio Settings"; font.pixelSize: 26; font.bold: true }
                        Label {
                            text: "Set the radio identity for this project. These fields are saved in the WaffleCPS project."
                            color: "#64776c"
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Label {
                            text: store.hasImportedCodeplug
                                  ? "Imported radio: " + store.radioModel + " · CPS " + store.radioCpsVersion
                                  : "Radio model: not selected"
                            color: "#64776c"
                            Layout.fillWidth: true
                        }
                        Label { text: "Radio name / call sign" }
                        TextField {
                            id: radioNameField
                            objectName: "radioNameField"
                            Layout.fillWidth: true
                            maximumLength: 16
                            text: store.radioName
                            placeholderText: "Enter your call sign"
                            Accessible.name: "Radio name or call sign"
                        }
                        Label { text: "DMR radio ID" }
                        TextField {
                            id: radioIdField
                            objectName: "radioIdField"
                            Layout.fillWidth: true
                            text: store.radioId > 0 ? String(store.radioId) : ""
                            placeholderText: "Enter your DMR radio ID"
                            inputMethodHints: Qt.ImhDigitsOnly
                            Accessible.name: "DMR radio ID"
                        }
                        Button {
                            objectName: "saveRadioSettingsButton"
                            text: "Save radio settings"
                            onClicked: store.updateRadioSettings(radioNameField.text, radioIdField.text)
                        }
                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
    }

    Dialog {
        id: editDialog
        property bool creating: false
        property bool forImported: false

        function openForCurrent() {
            creating = false
            forImported = false
            repeaterNameField.text = store.selectedName
            rxField.text = store.rxFrequency
            txField.text = store.txFrequency
            colorCodeField.value = store.colorCode
            powerField.currentIndex = store.selectedPower === "Low" ? 1 : 0
            editError.text = ""
            open()
        }

        function openForNew() {
            creating = true
            forImported = false
            repeaterNameField.text = ""
            rxField.text = ""
            txField.text = ""
            colorCodeField.value = 1
            powerField.currentIndex = 0
            editError.text = ""
            open()
            repeaterNameField.forceActiveFocus()
        }

        function openForImported() {
            creating = false
            forImported = true
            const group = store.selectedImportedGroup
            repeaterNameField.text = group.name
            rxField.text = group.receiveFrequency
            txField.text = group.transmitFrequency
            colorCodeField.value = group.colorCode
            powerField.currentIndex = group.power === "High" ? 0
                                      : group.power === "Low" ? 1 : -1
            editError.text = ""
            open()
        }

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: 380
        modal: true
        title: creating ? "Add repeater" : "Edit repeater"
        standardButtons: Dialog.NoButton

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label { text: "Name" }
            TextField { id: repeaterNameField; Layout.fillWidth: true; maximumLength: editDialog.forImported ? 16 : 32 }
            Label { text: "Receive frequency (MHz)" }
            TextField { id: rxField; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
            Label { text: "Transmit frequency (MHz)" }
            TextField { id: txField; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
            Label { text: "Color code" }
            SpinBox { id: colorCodeField; from: 0; to: 15 }
            Label { text: "Transmit power" }
            ComboBox {
                id: powerField
                objectName: "repeaterPowerField"
                Layout.fillWidth: true
                model: ["High", "Low"]
                displayText: currentIndex < 0
                             ? (store.selectedImportedGroup.power || "Unknown") + " (unchanged)"
                             : currentText
            }
            Label {
                text: "Applies to this zone's repeater channels. Additional simplex frequencies are unchanged."
                color: "#64776c"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                id: editError
                visible: text.length > 0
                color: "#a33a33"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; onClicked: editDialog.close() }
                Button {
                    text: editDialog.creating ? "Add repeater" : "Save changes"
                    highlighted: true
                    onClicked: {
                        const saved = editDialog.creating
                                      ? store.addRepeater(repeaterNameField.text, rxField.text,
                                                          txField.text, colorCodeField.value,
                                                          powerField.currentIndex === 0)
                                      : editDialog.forImported
                                        ? store.updateSelectedImportedRepeater(repeaterNameField.text, rxField.text,
                                                                              txField.text, colorCodeField.value,
                                                                              powerField.currentIndex < 0 ? -1
                                                                                : powerField.currentIndex === 0 ? 1 : 0)
                                        : store.updateSelectedRepeater(repeaterNameField.text, rxField.text,
                                                                       txField.text, colorCodeField.value,
                                                                       powerField.currentIndex === 0)
                        if (saved) {
                            editDialog.close()
                        } else {
                            editError.text = editDialog.forImported
                                             ? "Enter a zone name of up to 16 characters, different valid RX/TX frequencies, and a color code from 0–15."
                                             : "Enter a name and valid MHz frequencies. Color code must be 0–15."
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: simplexDialog
        property bool forImported: false

        function openForCurrent(importedMode) {
            forImported = importedMode
            simplexName.text = ""
            simplexFrequency.text = ""
            simplexDigital.checked = true
            simplexColorCode.value = importedMode ? store.selectedImportedGroup.colorCode : store.colorCode
            simplexTimeSlot.value = 1
            simplexZone.currentIndex = 0
            simplexError.text = ""
            open()
            simplexName.forceActiveFocus()
        }

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: 390
        modal: true
        title: "Add simplex frequency"
        standardButtons: Dialog.NoButton

        ColumnLayout {
            anchors.fill: parent
            spacing: 9
            Label { text: "Name" }
            TextField { id: simplexName; Layout.fillWidth: true; maximumLength: 16 }
            Label { text: "Frequency (MHz)" }
            TextField {
                id: simplexFrequency
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                placeholderText: "446.00000"
            }
            CheckBox { id: simplexDigital; text: "Digital channel" }
            RowLayout {
                visible: simplexDigital.checked
                Label { text: "Color code" }
                SpinBox { id: simplexColorCode; from: 0; to: 15 }
                Label { text: "Time slot" }
                SpinBox { id: simplexTimeSlot; from: 1; to: 2 }
            }
            Label { visible: simplexDialog.forImported && simplexZone.count > 1; text: "Add to zone" }
            ComboBox {
                id: simplexZone
                visible: simplexDialog.forImported && count > 1
                Layout.fillWidth: true
                model: (store.selectedImportedGroup.zones || []).map(zone => zone.name)
            }
            Label {
                id: simplexError
                visible: text.length > 0
                text: ""
                color: "#a33a33"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; onClicked: simplexDialog.close() }
                Button {
                    text: "Add"
                    highlighted: true
                    onClicked: {
                        const saved = simplexDialog.forImported
                                      ? store.addSimplexToSelectedImportedRepeater(simplexZone.currentIndex,
                                            simplexName.text, simplexFrequency.text, simplexDigital.checked,
                                            simplexColorCode.value, simplexTimeSlot.value)
                                      : store.addSimplexToSelectedRepeater(simplexName.text, simplexFrequency.text,
                                            simplexDigital.checked, simplexColorCode.value,
                                            simplexTimeSlot.value)
                        if (saved)
                            simplexDialog.close()
                        else
                            simplexError.text = "Enter a name and a valid MHz frequency. The selected zone must have room for another channel."
                    }
                }
            }
        }
    }

    Dialog {
        id: addDialog
        objectName: "addTalkgroupDialog"
        property int targetSlot: 1
        property bool importedMode: false
        property bool creatingTalkgroup: false
        property bool channelNameCustomized: false
        property var selectedTalkgroup: null
        property var matchingTalkgroups: {
            const query = talkgroupSearch.text.trim().toLocaleLowerCase()
            return store.contacts.filter(contact => contact.groupCall
                                         && (query.length === 0
                                             || contact.name.toLocaleLowerCase().includes(query)
                                             || String(contact.id).includes(query)))
                                 .sort((a, b) => a.name.localeCompare(b.name))
        }
        function checkChannelName() {
            const available = store.availableChannelName(channelNameField.text)
            if (available && available !== channelNameField.text.trim()) {
                if (!duplicateChannelDialog.visible) {
                    duplicateChannelDialog.requestedName = channelNameField.text.trim()
                    duplicateChannelDialog.suggestion = available
                    duplicateChannelDialog.open()
                }
                return false
            }
            return true
        }

        function openForSlot(slot, imported = false) {
            targetSlot = slot
            importedMode = imported
            creatingTalkgroup = false
            channelNameCustomized = false
            selectedTalkgroup = null
            talkgroupSearch.text = ""
            newTalkgroupName.text = ""
            newTalkgroupId.text = ""
            channelNameField.text = ""
            channelNameTimer.stop()
            errorLabel.text = ""
            open()
            talkgroupSearch.forceActiveFocus()
        }

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: 460
        modal: true
        title: "Add talkgroup to Time Slot " + targetSlot
        standardButtons: Dialog.NoButton
        Timer {
            id: channelNameTimer
            interval: 500
            onTriggered: addDialog.checkChannelName()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                text: "Talkgroup"
                font.bold: true
            }
            TextField {
                id: talkgroupSearch
                objectName: "talkgroupSearch"
                Layout.fillWidth: true
                visible: !addDialog.creatingTalkgroup
                placeholderText: "Search by name or ID"
                Accessible.name: "Find a talkgroup"
                onTextChanged: {
                    if (addDialog.selectedTalkgroup && text !== addDialog.selectedTalkgroup.name)
                        addDialog.selectedTalkgroup = null
                }
            }
            ListView {
                id: talkgroupChoices
                objectName: "talkgroupChoices"
                visible: !addDialog.creatingTalkgroup
                Layout.fillWidth: true
                Layout.preferredHeight: 172
                clip: true
                model: addDialog.matchingTalkgroups
                delegate: ItemDelegate {
                    objectName: "talkgroupChoice"
                    required property var modelData
                    width: ListView.view.width
                    text: modelData.name + "  ·  ID " + modelData.id
                    highlighted: addDialog.selectedTalkgroup !== null
                                 && addDialog.selectedTalkgroup.index === modelData.index
                    onClicked: {
                        addDialog.selectedTalkgroup = modelData
                        talkgroupSearch.text = modelData.name
                        if (!addDialog.channelNameCustomized)
                            channelNameField.text = store.suggestedChannelName(modelData.name)
                        errorLabel.text = ""
                    }
                }
            }
            Label {
                visible: !addDialog.creatingTalkgroup && addDialog.matchingTalkgroups.length === 0
                text: "No matching talkgroups. Create one below."
                color: "#66776d"
            }
            Label {
                visible: !addDialog.creatingTalkgroup && addDialog.selectedTalkgroup !== null
                text: addDialog.selectedTalkgroup
                      ? "Selected: " + addDialog.selectedTalkgroup.name + " · ID " + addDialog.selectedTalkgroup.id
                      : ""
                color: "#52695d"
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Button {
                objectName: "createTalkgroupButton"
                text: addDialog.creatingTalkgroup ? "Choose an existing talkgroup" : "+ Create new talkgroup"
                onClicked: {
                    if (addDialog.creatingTalkgroup) {
                        addDialog.creatingTalkgroup = false
                        addDialog.selectedTalkgroup = null
                        talkgroupSearch.forceActiveFocus()
                    } else {
                        const query = talkgroupSearch.text.trim()
                        addDialog.creatingTalkgroup = true
                        addDialog.selectedTalkgroup = null
                        newTalkgroupName.text = /^\d+$/.test(query) ? "" : query
                        newTalkgroupId.text = /^\d+$/.test(query) ? query : ""
                        if (!addDialog.channelNameCustomized)
                            channelNameField.text = store.suggestedChannelName(newTalkgroupName.text)
                        newTalkgroupName.forceActiveFocus()
                    }
                    errorLabel.text = ""
                }
            }
            Label { visible: addDialog.creatingTalkgroup; text: "Talkgroup display name" }
            TextField {
                id: newTalkgroupName
                objectName: "newTalkgroupName"
                visible: addDialog.creatingTalkgroup
                Layout.fillWidth: true
                maximumLength: 16
                placeholderText: "For example, CT Statewide"
                Accessible.name: "New talkgroup display name"
                onTextChanged: {
                    if (addDialog.creatingTalkgroup && !addDialog.channelNameCustomized)
                        channelNameField.text = store.suggestedChannelName(text)
                }
            }
            Label { visible: addDialog.creatingTalkgroup; text: "Talkgroup ID" }
            TextField {
                id: newTalkgroupId
                objectName: "newTalkgroupId"
                visible: addDialog.creatingTalkgroup
                Layout.fillWidth: true
                placeholderText: "1–16777215"
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 16777215 }
                Accessible.name: "New talkgroup ID"
            }
            Label {
                text: "Channel name"
                font.bold: true
            }
            TextField {
                id: channelNameField
                objectName: "channelNameField"
                Layout.fillWidth: true
                maximumLength: 16
                placeholderText: "Shown on the radio when idle"
                Accessible.name: "Channel name"
                onTextEdited: {
                    addDialog.channelNameCustomized = true
                    channelNameTimer.restart()
                }
                onEditingFinished: addDialog.checkChannelName()
            }
            Label {
                text: "The radio shows the talkgroup name while receiving or transmitting."
                color: "#66776d"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Uses this repeater's frequencies, color code, and Time Slot " + addDialog.targetSlot + "."
                color: "#66776d"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                visible: addDialog.importedMode && store.selectedImportedGroup.power === "Mixed"
                text: "This repeater has mixed power. New talkgroups use "
                      + store.selectedImportedGroup.newChannelPower + ", the most common setting."
                color: "#66776d"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                id: errorLabel
                visible: text.length > 0
                color: "#a33a33"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; onClicked: addDialog.close() }
                Button {
                    objectName: "confirmAddTalkgroup"
                    text: "Add"
                    highlighted: true
                    onClicked: {
                        const contact = addDialog.creatingTalkgroup
                            ? {name: newTalkgroupName.text.trim(), id: Number(newTalkgroupId.text)}
                            : addDialog.selectedTalkgroup
                        if (!contact || !contact.name || !Number.isInteger(contact.id)
                            || contact.id < 1 || contact.id > 16777215
                            || (addDialog.creatingTalkgroup && !newTalkgroupId.acceptableInput)) {
                            errorLabel.text = "Choose a talkgroup or enter its name and valid ID."
                        } else if (!channelNameField.text.trim()) {
                            errorLabel.text = "Enter a channel name."
                        } else if (store.availableChannelName(channelNameField.text)
                                   !== channelNameField.text.trim()) {
                            addDialog.checkChannelName()
                        } else if (!(addDialog.importedMode
                                     ? store.addImportedTalkgroup(addDialog.targetSlot, contact.name, contact.id, channelNameField.text)
                                     : store.addTalkgroup(addDialog.targetSlot, contact.name, contact.id, channelNameField.text))) {
                            errorLabel.text = addDialog.importedMode
                                    ? "Could not add this channel. Check zone capacity and whether the ID is already in use."
                                    : "Could not add this channel. Check whether the ID is already in this slot or used by a person."
                        } else {
                            addDialog.close()
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: duplicateChannelDialog
        property string requestedName: ""
        property string suggestion: ""
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: 400
        modal: true
        title: "Channel name already used"
        standardButtons: Dialog.NoButton
        ColumnLayout {
            width: 350
            spacing: 14
            Label {
                text: "A channel named “" + duplicateChannelDialog.requestedName
                      + "” is already in this codeplug. Each channel needs a different name so you can tell them apart on the radio."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Suggested name: " + duplicateChannelDialog.suggestion
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: "Edit name"
                    onClicked: {
                        duplicateChannelDialog.close()
                        channelNameField.forceActiveFocus()
                    }
                }
                Button {
                    text: "Use suggested name"
                    highlighted: true
                    onClicked: {
                        channelNameField.text = duplicateChannelDialog.suggestion
                        addDialog.channelNameCustomized = true
                        duplicateChannelDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: radioReadConfirmation
        objectName: "radioReadConfirmation"
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        modal: true
        title: "Discard unsaved changes?"
        standardButtons: Dialog.NoButton
        ColumnLayout {
            width: 360
            Label {
                text: "A successful radio read will replace this project and discard unsaved changes."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: "Cancel"; onClicked: radioReadConfirmation.close() }
                Button {
                    text: "Read from radio"
                    onClicked: {
                        radioReadConfirmation.close()
                        radioReader.start()
                    }
                }
            }
        }
    }

    Dialog {
        id: radioReadProgress
        objectName: "radioReadProgress"
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        title: "Reading radio"
        width: Math.min(420, window.width - 32)
        modal: true
        closePolicy: Popup.NoAutoClose
        visible: radioReader.busy
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: radioReader.status
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            ProgressBar {
                Layout.fillWidth: true
                value: radioReader.progress
            }
            Button {
                objectName: "cancelRadioReadButton"
                text: "Cancel"
                Layout.alignment: Qt.AlignRight
                onClicked: radioReader.cancel()
            }
        }
    }

    Connections {
        target: radioReader
        function onFailed(message) {
            operationError.message = message
            Qt.callLater(() => operationError.open())
        }
    }

    Dialog {
        id: createZoneDialog
        objectName: "createZoneDialog"
        modal: true
        width: Math.min(400, window.width - 32)
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        title: "Create custom zone"
        standardButtons: Dialog.NoButton
        function openForNew() { createZoneName.text = ""; createZoneError.text = ""; open() }
        contentItem: ColumnLayout {
            spacing: 10
            Label { text: "Zone name" }
            TextField { id: createZoneName; objectName: "createZoneName"; maximumLength: 16; Layout.fillWidth: true }
            Label { id: createZoneError; visible: text.length > 0; color: "#a33a33"; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            RowLayout {
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; onClicked: createZoneDialog.close() }
                Button {
                    objectName: "createZoneSave"
                    text: "Create zone"
                    enabled: createZoneName.text.trim().length > 0
                    onClicked: {
                        if (store.createCustomZone(createZoneName.text)) {
                            window.currentSection = "repeaters"
                            createZoneDialog.close()
                        } else createZoneError.text = "Could not create this zone. Check the name and radio capacity."
                    }
                }
            }
        }
    }

    ListModel { id: channelOrderModel; objectName: "channelOrderModel" }

    Dialog {
        id: channelOrderDialog
        objectName: "channelOrderDialog"
        property string zoneName: ""
        property bool manualProject: false
        width: Math.min(540, window.width - 32)
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        title: "Order channels — " + zoneName
        modal: true
        standardButtons: Dialog.NoButton

        function openForSelectedRepeater() {
            manualProject = true
            zoneName = store.selectedName
            channelOrderModel.clear()
            for (const channel of store.selectedRepeaterChannels) {
                channelOrderModel.append({
                    channelName: channel.name,
                    channelNumber: channel.number,
                    kind: channel.simplex ? "Simplex" + (channel.timeSlot ? " · TS " + channel.timeSlot : "")
                                          : "TS " + channel.timeSlot
                })
            }
            channelOrderError.text = ""
            open()
        }

        function openForSelectedZone() {
            manualProject = false
            const zones = store.selectedImportedGroup.zones || []
            if (zones.length !== 1)
                return
            zoneName = zones[0].name
            channelOrderModel.clear()
            for (const channel of zones[0].channels) {
                const simplex = channel.receiveFrequency === channel.transmitFrequency
                channelOrderModel.append({
                    channelName: channel.name,
                    channelNumber: channel.number,
                    kind: simplex ? "Simplex" + (channel.timeSlot ? " · TS " + channel.timeSlot : "")
                                  : (channel.timeSlot ? "TS " + channel.timeSlot : channel.mode)
                })
            }
            channelOrderError.text = ""
            open()
        }

        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: "Arrange all channels in radio order. Each time-slot box follows this order, filtered to its slot."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            ListView {
                id: channelOrderList
                objectName: "channelOrderList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(360, channelOrderModel.count * 54)
                clip: true
                spacing: 4
                model: channelOrderModel
                ScrollBar.vertical: ScrollBar {}
                delegate: Frame {
                    required property int index
                    required property string channelName
                    required property string kind
                    width: channelOrderList.width
                    height: 50
                    padding: 6
                    RowLayout {
                        anchors.fill: parent
                        Label { text: index + 1; Layout.preferredWidth: 24 }
                        Label {
                            text: channelName
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label { text: kind; color: "#64776c" }
                        ToolButton {
                            objectName: "channelOrderUp"
                            property int rowIndex: index
                            text: "Up"
                            enabled: index > 0
                            Accessible.name: "Move " + channelName + " up"
                            onClicked: channelOrderModel.move(index, index - 1, 1)
                        }
                        ToolButton {
                            objectName: "channelOrderDown"
                            property int rowIndex: index
                            text: "Down"
                            enabled: index < channelOrderModel.count - 1
                            Accessible.name: "Move " + channelName + " down"
                            onClicked: channelOrderModel.move(index, index + 1, 1)
                        }
                    }
                }
            }
            Label {
                id: channelOrderError
                color: "#b33a32"
                visible: text.length > 0
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    objectName: "channelOrderCancel"
                    text: "Cancel"
                    onClicked: channelOrderDialog.close()
                }
                Button {
                    objectName: "channelOrderSave"
                    text: "Save"
                    onClicked: {
                        const numbers = []
                        for (let i = 0; i < channelOrderModel.count; ++i)
                            numbers.push(channelOrderModel.get(i).channelNumber)
                        if (channelOrderDialog.manualProject ? store.reorderSelectedRepeaterChannels(numbers)
                                                            : store.reorderSelectedZone(numbers))
                            channelOrderDialog.close()
                        else
                            channelOrderError.text = "Could not apply this order. Close and reopen the dialog to refresh the zone."
                    }
                }
            }
        }
    }

    Dialog {
        id: operationError
        width: Math.min(420, window.width - 32)
        property string message: ""
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        modal: true
        title: "Project error"
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            Label {
                text: operationError.message
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Button {
                text: "OK"
                Layout.alignment: Qt.AlignRight
                onClicked: operationError.close()
            }
        }
    }

    Connections {
        target: store
        function onErrorOccurred(message) {
            operationError.message = message
            operationError.open()
        }
    }
}
