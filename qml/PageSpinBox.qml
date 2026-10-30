import QtQuick
import QtQuick.Controls

SpinBox {
    id: number
    implicitWidth: 108
    implicitHeight: 40
    leftPadding: 10
    rightPadding: 36
    up.indicator: Rectangle {
        x: number.width - width - 5
        y: 4
        width: 28
        height: 15
        radius: 4
        color: number.up.pressed ? "#d9ede5" : "#f2f7f2"
        border.color: "#cbded0"
        Text {
            anchors.centerIn: parent
            text: "▲"
            font.pixelSize: 10
            color: number.up.indicator.enabled ? "#173d30" : "#839188"
        }
    }
    down.indicator: Rectangle {
        x: number.width - width - 5
        y: number.height - height - 4
        width: 28
        height: 15
        radius: 4
        color: number.down.pressed ? "#d9ede5" : "#f2f7f2"
        border.color: "#cbded0"
        Text {
            anchors.centerIn: parent
            text: "▼"
            font.pixelSize: 10
            color: number.down.indicator.enabled ? "#173d30" : "#839188"
        }
    }
    background: Rectangle {
        radius: 9
        color: number.enabled ? "#ffffff" : "#f5f8f5"
        border.color: number.activeFocus ? "#548d66" : "#cbded0"
    }
}
