import QtQuick
import QtQuick.Controls

ItemDelegate {
    id: choice
    background: Rectangle {
        radius: 9
        color: choice.highlighted ? "#d9ede5" : choice.hovered ? "#f2f7f2" : "transparent"
        border.color: choice.activeFocus ? "#548d66"
                      : choice.highlighted ? "#b8d9c1" : "transparent"
    }
}
