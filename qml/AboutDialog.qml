import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageDialog {
    id: about
    objectName: "aboutDialog"
    property var definitions: []
    property var contributors: []
    property var platformCredits: [
        { name: "Windows", contributors: [], testers: ["KD1AHF"] },
        { name: "macOS", contributors: [], testers: ["KC1UXW"] },
        { name: "Linux", contributors: [], testers: [] }
    ]
    property var definitionCredits: ({
        "dm32uv.wcp": {
            contributors: ["KD1AHF"],
            testers: ["KD1AHF"]
        }
    })
    function creditText(names) {
        return names && names.length ? names.join(", ") : "—"
    }
    function highlightedCredit(name) {
        const escaped = String(name).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        return '<b><font color="#173d30">' + escaped + '</font></b>'
    }
    modal: true
    title: "About WaffleCPS"
    width: Math.min(640, parent.width - 32)
    height: Math.min(640, parent.height - 32)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    standardButtons: Dialog.NoButton

    component CreditRow: Rectangle {
        required property string creditName
        property string contributors: "—"
        property string testers: "—"
        Layout.fillWidth: true
        implicitHeight: creditColumn.implicitHeight + 16
        radius: 12
        color: "#f7faf7"
        border.color: "#dce4df"
        ColumnLayout {
            id: creditColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            spacing: 2
            Label {
                text: parent.parent.creditName
                font.bold: true
                color: "#173d30"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Contributors: " + about.highlightedCredit(parent.parent.contributors)
                      + " &nbsp;&nbsp; Testers: " + about.highlightedCredit(parent.parent.testers)
                textFormat: Text.RichText
                color: "#405b4b"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 16
        ScrollView {
            id: creditsScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: creditsScroll.availableWidth
                spacing: 12
                RowLayout {
                    spacing: 12
                    Image {
                        source: "qrc:/assets/waffle-radio.png"
                        Layout.preferredWidth: 48
                        Layout.preferredHeight: 48
                        fillMode: Image.PreserveAspectFit
                    }
                    ColumnLayout {
                        spacing: 2
                        Label { text: "WaffleCPS"; font.pixelSize: 24; font.bold: true; color: "#173d30" }
                        RowLayout {
                            spacing: 8
                            Label { text: Qt.application.version; color: "#64776c" }
                            PageAction {
                                objectName: "copyVersionButton"
                                text: "Copy version"
                                flat: true
                                implicitHeight: 28
                                font.pixelSize: 12
                                leftPadding: 8
                                rightPadding: 8
                                Accessible.name: "Copy WaffleCPS version and build"
                                onClicked: store.copyApplicationVersion()
                            }
                        }
                    }
                }
                Label {
                    text: 'Author: KB1JDX (<a href="https://github.com/compuvin">compuvin</a>)'
                    textFormat: Text.RichText
                    linkColor: "#1c6e9a"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    onLinkActivated: link => Qt.openUrlExternally(link)
                }
                Label {
                    text: '<a href="https://github.com/compuvin/WaffleCPS">WaffleCPS on GitHub</a>'
                    textFormat: Text.RichText
                    linkColor: "#1c6e9a"
                    onLinkActivated: link => Qt.openUrlExternally(link)
                }
                Label { text: "Contributors"; font.pixelSize: 18; font.bold: true; color: "#173d30" }
                Label {
                    text: about.creditText(about.contributors)
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    color: "#173d30"
                    font.bold: true
                }
                Label { text: "Platform credits"; font.pixelSize: 18; font.bold: true; color: "#173d30" }
                Repeater {
                    model: about.platformCredits
                    delegate: CreditRow {
                        required property var modelData
                        creditName: modelData.name
                        contributors: about.creditText(modelData.contributors)
                        testers: about.creditText(modelData.testers)
                    }
                }
                Label { text: "Radio definition credits"; font.pixelSize: 18; font.bold: true; color: "#173d30" }
                Label {
                    text: "Only the initial contributors and testers are listed for each definition."
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    color: "#64776c"
                    font.pixelSize: 12
                }
                Repeater {
                    model: about.definitions
                    delegate: CreditRow {
                        required property var modelData
                        creditName: modelData.model
                        contributors: about.creditText((about.definitionCredits[modelData.fileName] || {}).contributors)
                        testers: about.creditText((about.definitionCredits[modelData.fileName] || {}).testers)
                    }
                }
            }
        }
        PageAction {
            text: "Close"
            Layout.alignment: Qt.AlignRight
            onClicked: about.close()
        }
    }
}
