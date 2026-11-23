import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageAction {
    id: helpButton
    property string helpTitle: "Help"
    property string helpText: ""

    text: "?"
    compact: true
    Accessible.name: helpTitle + " help"
    onClicked: helpDialog.open()

    PageDialog {
        id: helpDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 32)
        modal: true
        title: helpButton.helpTitle
        standardButtons: Dialog.NoButton

        contentItem: ColumnLayout {
            spacing: 14
            Label {
                text: helpButton.helpText
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            PageAction {
                text: "Close"
                Layout.alignment: Qt.AlignRight
                onClicked: helpDialog.close()
            }
        }
    }
}
