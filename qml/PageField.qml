import QtQuick
import QtQuick.Controls

TextField {
    id: field
    implicitHeight: 40
    leftPadding: 12
    rightPadding: 12
    color: "#173d30"
    selectionColor: "#b8d9c1"
    background: Rectangle {
        radius: 9
        color: field.enabled ? "#ffffff" : "#f5f8f5"
        border.color: field.activeFocus ? "#548d66" : "#cbded0"
    }
}
