import QtQuick
import QtQuick.Controls

ComboBox {
    id: choice
    implicitHeight: 40
    leftPadding: 12
    rightPadding: 30
    background: Rectangle {
        radius: 9
        color: choice.enabled ? "#ffffff" : "#f5f8f5"
        border.color: choice.activeFocus ? "#548d66" : "#cbded0"
    }
}
