import QtQuick
import QtQuick.Controls

TabButton {
    id: tab
    implicitHeight: 40
    contentItem: Text {
        text: tab.text
        color: tab.checked ? "#173d30" : "#52695d"
        font: tab.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 9
        color: tab.checked ? "#d9ede5" : tab.hovered ? "#f2f7f2" : "transparent"
        border.color: tab.activeFocus ? "#548d66" : tab.checked ? "#b8d9c1" : "transparent"
    }
}
