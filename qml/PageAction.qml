import QtQuick
import QtQuick.Controls

Button {
    id: action
    property bool primary: false
    property bool quiet: false
    property bool compact: false
    readonly property bool filled: primary || highlighted

    implicitWidth: compact ? 34 : contentItem.implicitWidth + leftPadding + rightPadding
    implicitHeight: compact ? 34 : 38
    leftPadding: compact ? 6 : 12
    rightPadding: compact ? 6 : 12
    font.pixelSize: 14
    font.bold: true

    contentItem: Text {
        text: action.text
        font: action.font
        color: !action.enabled ? "#839188"
              : action.filled ? "#ffffff" : action.flat ? "#1c6e9a" : "#173d30"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 9
        color: action.filled
               ? (action.enabled ? (action.hovered ? "#1f563e" : "#246448") : "#dce4df")
               : action.hovered ? "#e7f2e9" : action.quiet || action.flat ? "transparent" : "#f7faf7"
        border.color: action.activeFocus ? "#548d66"
                      : action.filled || action.quiet || action.flat ? "transparent" : "#dce4df"
    }
}
