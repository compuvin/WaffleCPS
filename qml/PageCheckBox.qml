import QtQuick
import QtQuick.Controls

CheckBox {
    id: toggle
    spacing: 8
    indicator: Rectangle {
        implicitWidth: 20
        implicitHeight: 20
        x: toggle.leftPadding
        y: (toggle.height - height) / 2
        radius: 5
        color: toggle.checked ? "#246448" : "#ffffff"
        border.color: toggle.activeFocus ? "#548d66" : "#b8d9c1"
        Text {
            anchors.centerIn: parent
            text: "✓"
            color: "#ffffff"
            visible: toggle.checked
            font.bold: true
        }
    }
}
