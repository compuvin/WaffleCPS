import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageDialog {
    id: dialog
    objectName: "satelliteDialog"
    property var rows: []
    property int selected: -1
    property int capacity: 0
    property string filePath: ""
    property string downloadSummary: ""
    property var fieldNames: ["Catalog ID", "Name", "Voice receive (MHz)", "Voice transmit (MHz)",
        "Transmit CTCSS (Hz)", "Arming CTCSS (Hz)", "APRS receive (MHz)", "APRS transmit (MHz)",
        "Other receive (MHz)", "Other transmit (MHz)", "APRS path"]
    function clearWrittenStatus() {
        downloadSummary = ""
        for (let i = 0; i < names.count; ++i) names.setProperty(i, "written", false)
    }
    function showSettings() {
        const data = store.loadSatelliteSettings()
        if (!data.path) return
        downloadSummary = ""
        rows = data.rows
        capacity = data.capacity
        filePath = data.path
        orbitalUrl.text = data.url
        names.clear()
        for (let i = 0; i < rows.length; ++i) names.append({ name: rows[i][1], written: false })
        selected = rows.length ? 0 : -1
        open()
    }
    modal: true
    title: "Satellites"
    width: Math.min(880, parent.width - 32)
    height: Math.min(700, parent.height - 32)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    closePolicy: store.satelliteUpdateBusy ? Popup.NoAutoClose : Popup.CloseOnEscape
    standardButtons: Dialog.NoButton
    Connections {
        target: store
        function onSatelliteUpdateCompleted(catalogIds) {
            for (let i = 0; i < names.count; ++i)
                names.setProperty(i, "written", catalogIds.indexOf(parseInt(dialog.rows[i][0], 10)) >= 0)
            dialog.downloadSummary = "Updated " + catalogIds.length + " of " + names.count
                + " satellites. The radio clock was set to UTC."
        }
        function onSatelliteUpdateFailed() { dialog.downloadSummary = "" }
    }
    ListModel { id: names }
    contentItem: ColumnLayout {
        enabled: !store.satelliteUpdateBusy
        spacing: 12
        Label {
            text: "Choose the satellites you want available on your radio. Frequencies are in MHz; use 0 for unused fields."
            color: "#64776c"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label { text: "Keplerian data URL"; color: "#173d30"; font.bold: true }
        PageField {
            id: orbitalUrl
            objectName: "satelliteUrlField"
            Layout.fillWidth: true
            selectByMouse: true
            Accessible.name: "Keplerian data URL"
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16
            ColumnLayout {
                Layout.minimumWidth: 170
                Layout.preferredWidth: 190
                Layout.maximumWidth: 190
                Layout.fillHeight: true
                Label { text: names.count + " / " + dialog.capacity + " satellites"; color: "#64776c" }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 10
                    color: "#f5f8f5"
                    border.color: "#dce4df"
                    ListView {
                        id: satelliteList
                        objectName: "satelliteList"
                        anchors.fill: parent
                        anchors.margins: 6
                        clip: true
                        spacing: 4
                        model: names
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: ItemDelegate {
                            id: satelliteItem
                            required property int index
                            required property string name
                            required property bool written
                            width: satelliteList.width
                            text: name.length ? name : "New satellite"
                            contentItem: Text {
                                text: satelliteItem.text
                                font: satelliteItem.font
                                color: "#173d30"
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            highlighted: dialog.selected === index
                            onClicked: dialog.selected = index
                            background: Rectangle {
                                radius: 8
                                color: satelliteItem.written ? "#b8dfbf"
                                       : dialog.selected === satelliteItem.index ? "#d8ebe3"
                                       : "transparent"
                                border.width: dialog.selected === satelliteItem.index ? 2 : 0
                                border.color: "#246448"
                            }
                        }
                    }
                }
                RowLayout {
                    PageAction {
                        objectName: "addSatelliteButton"
                        text: "Add"
                        enabled: names.count < dialog.capacity
                        onClicked: {
                            dialog.clearWrittenStatus()
                            dialog.rows.push(["", "", "0", "0", "0", "0", "0", "0", "0", "0", ""])
                            names.append({ name: "", written: false })
                            dialog.selected = names.count - 1
                            satelliteList.positionViewAtEnd()
                        }
                    }
                    PageAction {
                        text: "Remove"
                        enabled: dialog.selected >= 0
                        onClicked: {
                            dialog.clearWrittenStatus()
                            const at = dialog.selected
                            dialog.selected = -1
                            dialog.rows.splice(at, 1)
                            names.remove(at)
                            dialog.selected = names.count ? Math.min(at, names.count - 1) : -1
                        }
                    }
                }
            }
            ScrollView {
                id: editorScroll
                implicitWidth: 1
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                GridLayout {
                    width: editorScroll.availableWidth
                    height: implicitHeight
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 8
                    Repeater {
                        model: dialog.fieldNames
                        delegate: ColumnLayout {
                            required property int index
                            required property string modelData
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            Layout.fillWidth: true
                            Layout.columnSpan: index === 10 ? 2 : 1
                            spacing: 4
                            Label {
                                text: modelData
                                color: "#173d30"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            PageField {
                                implicitWidth: 1
                                Layout.minimumWidth: 0
                                Layout.fillWidth: true
                                enabled: dialog.selected >= 0
                                selectByMouse: true
                                maximumLength: index === 1 ? 8 : 128
                                text: dialog.selected >= 0 ? dialog.rows[dialog.selected][index] : ""
                                Accessible.name: modelData
                                onTextEdited: {
                                    dialog.clearWrittenStatus()
                                    dialog.rows[dialog.selected][index] = text
                                    if (index === 1) names.setProperty(dialog.selected, "name", text)
                                }
                            }
                        }
                    }
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        text: "The radio displays names up to 8 characters. Transmit CTCSS and arming CTCSS are separate tones."
                        color: "#64776c"
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }
                }
            }
        }
        Label {
            text: dialog.filePath
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            color: "#64776c"
            font.pixelSize: 11
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: store.satelliteUpdateBusy ? store.satelliteStatus : dialog.downloadSummary
            color: "#64776c"
            wrapMode: Text.WordWrap
        }
        ProgressBar {
            Layout.fillWidth: true
            visible: store.satelliteUpdateBusy
            value: store.satelliteProgress
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            PageAction { text: "Cancel"; onClicked: dialog.close() }
            PageAction {
                objectName: "saveSatellitesButton"
                text: "Save list"
                onClicked: {
                    if (store.saveSatelliteSettings(dialog.rows, orbitalUrl.text)) dialog.close()
                }
            }
            PageAction {
                objectName: "updateSatelliteRadioButton"
                text: "Update radio"
                primary: true
                enabled: !radioReader.busy && !radioWriter.busy
                onClicked: {
                    dialog.clearWrittenStatus()
                    store.updateSatelliteRadio(dialog.rows, orbitalUrl.text)
                }
            }
        }
    }
}
