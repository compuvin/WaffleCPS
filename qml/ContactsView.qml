import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "contactsView"
    required property var contacts
    property var selectedContact: null
    property bool editingContact: false
    Connections {
        target: store
        function onAboutToResetProject() {
            contactDialog.close()
            root.selectedContact = null
            root.editingContact = false
        }
    }
    function openContactEditor(editing) {
        editingContact = editing
        nameField.text = editing && selectedContact ? selectedContact.name : ""
        idField.text = editing && selectedContact ? String(selectedContact.id) : ""
        formError.text = ""
        contactDialog.open()
        nameField.forceActiveFocus()
    }
    property var visibleContacts: {
        const wantGroup = typeTabs.currentIndex === 1
        const query = searchField.text.trim().toLocaleLowerCase()
        return contacts.filter(contact => contact.groupCall === wantGroup
                               && (query.length === 0
                                   || contact.name.toLocaleLowerCase().includes(query)
                                   || String(contact.id).includes(query)))
                       .sort((a, b) => a.name.localeCompare(b.name))
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 3
                Label { text: "Contacts"; font.pixelSize: 30; font.bold: true; color: "#183b2d" }
                Label { text: "Find people and talkgroups by name or ID."; color: "#64776c" }
            }
            Item { Layout.fillWidth: true }
            PageAction {
                text: typeTabs.currentIndex === 0 ? "Add person" : "Add talkgroup"
                primary: true
                onClicked: root.openContactEditor(false)
            }
        }

        TabBar {
            id: typeTabs
            Layout.fillWidth: true
            background: Rectangle { color: "#f7faf7"; radius: 10; border.color: "#dce4df" }
            onCurrentIndexChanged: root.selectedContact = null
            PageTabButton {
                text: "People (" + root.contacts.filter(contact => !contact.groupCall).length + ")"
                Accessible.name: "People contacts"
            }
            PageTabButton {
                text: "Talkgroups (" + root.contacts.filter(contact => contact.groupCall).length + ")"
                Accessible.name: "Talkgroup contacts"
            }
        }

        PageField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: typeTabs.currentIndex === 0 ? "Search people by name or ID" : "Search talkgroups by name or ID"
            Accessible.name: "Search contacts"
            onTextChanged: root.selectedContact = null
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            Frame {
                Layout.preferredWidth: 380
                Layout.fillHeight: true
                padding: 0
                background: Rectangle { color: "#ffffff"; radius: 14; border.color: "#dce4df" }

                ListView {
                    objectName: "contactsList"
                    anchors.fill: parent
                    anchors.margins: 8
                    clip: true
                    spacing: 4
                    model: root.visibleContacts

                    delegate: ItemDelegate {
                        objectName: "contactChoice"
                        required property var modelData
                        width: ListView.view.width
                        height: 64
                        highlighted: root.selectedContact !== null
                                     && root.selectedContact.id === modelData.id
                                     && root.selectedContact.name === modelData.name
                        onClicked: root.selectedContact = modelData
                        Accessible.name: modelData.name + ", ID " + modelData.id
                        background: Rectangle {
                            radius: 10
                            color: parent.highlighted ? "#e5f1e9" : (parent.hovered ? "#f5f8f5" : "transparent")
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Rectangle {
                                width: 38
                                height: 38
                                radius: 19
                                color: typeTabs.currentIndex === 0 ? "#e5e4f4" : "#d9ede5"
                                Label {
                                    anchors.centerIn: parent
                                    text: modelData.name.slice(0, 1).toUpperCase()
                                    font.bold: true
                                    color: typeTabs.currentIndex === 0 ? "#4f4d86" : "#166046"
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label { text: modelData.name; font.bold: true; color: "#20372e"; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label { text: "ID " + modelData.id; color: "#64776c"; font.pixelSize: 12 }
                            }
                        }
                    }
                }
            }

            Frame {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 24
                background: Rectangle { color: "#ffffff"; radius: 14; border.color: "#dce4df" }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 14
                    Label {
                        text: root.selectedContact ? root.selectedContact.name : "Select a contact"
                        font.pixelSize: 24
                        font.bold: true
                        color: "#20372e"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        text: root.selectedContact ? (root.selectedContact.groupCall ? "Talkgroup" : "Person") : "Choose someone from the list to see their details."
                        color: "#64776c"
                    }
                    Label {
                        visible: root.selectedContact !== null
                        text: "DMR ID"
                        font.bold: true
                        color: "#697c71"
                    }
                    Label {
                        visible: root.selectedContact !== null
                        text: root.selectedContact ? String(root.selectedContact.id) : ""
                        font.pixelSize: 20
                        color: "#20372e"
                    }
                    PageAction {
                        visible: root.selectedContact !== null
                        text: "Edit contact"
                        onClicked: root.openContactEditor(true)
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Label {
            visible: root.visibleContacts.length === 0
            text: searchField.text.length > 0 ? "No matching contacts." : (typeTabs.currentIndex === 0 ? "No people contacts yet." : "No talkgroups yet.")
            color: "#64776c"
        }
    }

    PageDialog {
        id: contactDialog
        title: root.editingContact ? "Edit contact" : (typeTabs.currentIndex === 0 ? "Add person" : "Add talkgroup")
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, root.width - 32)
        standardButtons: Dialog.NoButton
        footer: RowLayout {
            spacing: 8
            Item { Layout.fillWidth: true }
            PageAction {
                text: "Cancel"
                onClicked: contactDialog.reject()
            }
            PageAction {
                text: "Save"
                highlighted: true
                onClicked: contactDialog.accept()
            }
        }
        onAccepted: {
            const name = nameField.text.trim()
            const id = Number(idField.text)
            if (!name || name.length > 16
                || !/^\d+$/.test(idField.text) || id < 1 || id > 16777215) {
                formError.text = "Enter a name of up to 16"
                               + " characters and a DMR ID from 1 to 16777215."
                open()
                return
            }
            const oldIndex = root.editingContact && root.selectedContact ? root.selectedContact.index : -1
            const oldId = root.editingContact && root.selectedContact ? root.selectedContact.id : -1
            if (id !== oldId && root.contacts.some(contact => contact.index !== oldIndex && contact.id === id)) {
                formError.text = "A person or talkgroup already uses that DMR ID. Choose a different ID."
                open()
                return
            }
            const success = root.editingContact
                ? store.updateContact(oldIndex, name, id)
                : store.addContact(name, id, typeTabs.currentIndex === 1)
            if (!success) {
                formError.text = "That DMR ID is already in use, or this contact cannot be saved."
                open()
                return
            }
            root.selectedContact = null
            for (const contact of root.contacts) {
                if (contact.id === id && contact.groupCall === (typeTabs.currentIndex === 1)) {
                    root.selectedContact = contact
                    break
                }
            }
        }
        ColumnLayout {
            width: parent.width
            spacing: 10
            Label { text: typeTabs.currentIndex === 0 ? "Person · private call" : "Talkgroup · group call"; color: "#64776c" }
            Label { text: "Name or call sign" }
            PageField { id: nameField; Layout.fillWidth: true; Accessible.name: "Contact name or call sign" }
            Label { text: "DMR ID" }
            PageField {
                id: idField
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: 16777215 }
                Accessible.name: "Contact DMR ID"
            }
            Label { id: formError; Layout.fillWidth: true; color: "#b34335"; wrapMode: Text.WordWrap }
        }
    }
}
