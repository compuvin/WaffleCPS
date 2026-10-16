import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
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

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 66
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

                Item { Layout.fillWidth: true }

                Button { text: "Open"; onClicked: store.openProjectDialog() }
                Button { text: "Import codeplug"; onClicked: store.importCodeplugDialog() }
                Button { text: "Save"; onClicked: store.saveProjectDialog(false) }
                Button { text: "Save As"; onClicked: store.saveProjectDialog(true) }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                visible: !store.hasImportedCodeplug
                Layout.preferredWidth: 242
                Layout.fillHeight: true
                color: "#ffffff"
                border.color: "#e0e8e0"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10

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
                            onClicked: store.selectRepeater(index)
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

                    Label {
                        text: "Zone editor is planned next"
                        color: "#839188"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.pixelSize: 12
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

            Rectangle {
                visible: store.hasImportedCodeplug
                Layout.preferredWidth: 260
                Layout.fillHeight: true
                color: "#ffffff"
                border.color: "#e0e8e0"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 8

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
                                    onClicked: store.selectImportedGroup(index)
                                }
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
                                    onClicked: store.selectImportedGroup(index + store.importedRepeaterNames.length)
                                }
                            }
                        }
                    }
                    Label {
                        text: store.importedCodeplugSummary
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#64776c"
                        font.pixelSize: 11
                    }
                }
            }

            ScrollView {
                id: workspace
                visible: !store.hasImportedCodeplug
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    width: workspace.availableWidth - 40
                    x: 20
                    spacing: 20

                    Item { Layout.preferredHeight: 10 }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            text: store.selectedName
                            font.pixelSize: 30
                            font.bold: true
                            color: "#183b2d"
                        }

                        Label {
                            text: store.projectDisplayName === "Untitled project"
                                  ? "Example data · Edit and save as a project"
                                  : "Repeater overview"
                            color: "#718379"
                        }
                    }

                    Frame {
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
                            }
                        }
                    }

                    Label {
                        text: "TIME SLOTS"
                        color: "#697c71"
                        font.pixelSize: 11
                        font.bold: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 16

                        SlotPanel {
                            slotNumber: 1
                            groups: store.slot1Talkgroups
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            onAddRequested: slot => addDialog.openForSlot(slot)
                        }

                        SlotPanel {
                            slotNumber: 2
                            groups: store.slot2Talkgroups
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            onAddRequested: slot => addDialog.openForSlot(slot)
                        }
                    }

                    Item { Layout.preferredHeight: 10 }
                }
            }

            ScrollView {
                id: importedWorkspace
                visible: store.hasImportedCodeplug
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    property var group: store.selectedImportedGroup
                    width: importedWorkspace.availableWidth - 40
                    x: 20
                    spacing: 14

                    Item { Layout.preferredHeight: 10 }
                    Label {
                        text: parent.group.name || "Imported codeplug"
                        font.pixelSize: 28
                        font.bold: true
                        color: "#183b2d"
                    }
                    Label {
                        text: parent.group.classification || ""
                        color: "#64776c"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: "Frequency A and B follow the codeplug's stored order; RX/TX direction is still being verified."
                        color: "#64776c"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Repeater {
                        model: parent.group.zones || []
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            padding: 16
                            background: Rectangle { color: "white"; radius: 12; border.color: "#dce4df" }
                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 8
                                Label {
                                    text: modelData.name + " · " + modelData.channels.length + " channels"
                                    font.bold: true
                                    font.pixelSize: 17
                                    color: "#20372e"
                                }
                                Repeater {
                                    model: modelData.channels
                                    delegate: Rectangle {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 48
                                        radius: 8
                                        color: "#f5f8f5"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            Label { text: modelData.name; Layout.fillWidth: true; elide: Text.ElideRight }
                                            Label { text: modelData.frequencyA + " / " + modelData.frequencyB + " MHz"; color: "#64776c" }
                                            Label {
                                                text: modelData.mode === "Digital"
                                                      ? "CC " + modelData.colorCode + " · TS " + modelData.timeSlot
                                                      : modelData.mode
                                                color: "#64776c"
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
        }
    }

    Dialog {
        id: editDialog

        function openForCurrent() {
            repeaterNameField.text = store.selectedName
            rxField.text = store.rxFrequency
            txField.text = store.txFrequency
            colorCodeField.value = store.colorCode
            editError.text = ""
            open()
        }

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: 380
        modal: true
        title: "Edit repeater"
        standardButtons: Dialog.NoButton

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label { text: "Name" }
            TextField { id: repeaterNameField; Layout.fillWidth: true; maximumLength: 32 }
            Label { text: "Receive frequency (MHz)" }
            TextField { id: rxField; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
            Label { text: "Transmit frequency (MHz)" }
            TextField { id: txField; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly }
            Label { text: "Color code" }
            SpinBox { id: colorCodeField; from: 0; to: 15 }

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
                    text: "Save changes"
                    highlighted: true
                    onClicked: {
                        if (store.updateSelectedRepeater(repeaterNameField.text, rxField.text,
                                                         txField.text, colorCodeField.value)) {
                            editDialog.close()
                        } else {
                            editError.text = "Enter a name and valid MHz frequencies. Color code must be 0–15."
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: addDialog
        property int targetSlot: 1

        function openForSlot(slot) {
            targetSlot = slot
            nameField.text = ""
            idField.text = ""
            errorLabel.text = ""
            open()
            nameField.forceActiveFocus()
        }

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: 360
        modal: true
        title: "Add talkgroup to Time Slot " + targetSlot
        standardButtons: Dialog.NoButton

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label { text: "Name" }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "For example, Local"
                maximumLength: 32
            }

            Label { text: "Talkgroup ID" }
            TextField {
                id: idField
                Layout.fillWidth: true
                placeholderText: "1–16777215"
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 16777215 }
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
                    text: "Add"
                    highlighted: true
                    onClicked: {
                        const id = Number(idField.text)
                        if (!nameField.text.trim() || !idField.acceptableInput) {
                            errorLabel.text = "Enter a name and a valid talkgroup ID."
                        } else if (!store.addTalkgroup(addDialog.targetSlot, nameField.text, id)) {
                            errorLabel.text = "That talkgroup ID is already in this time slot."
                        } else {
                            addDialog.close()
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: operationError
        property string message: ""
        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        modal: true
        title: "Project error"
        standardButtons: Dialog.NoButton
        ColumnLayout {
            width: 320
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
