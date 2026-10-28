import QtQuick

// A bento tile of the settings pages (LUTs, AI Models): a card surface whose children are
// placed in `content`, inset by the tile padding. Set `contentHeight` to the content height.
Rectangle {
    default property alias content: tileContent.data
    property real contentHeight: 0

    implicitHeight: contentHeight + 22 * 2
    radius: 14
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    Item {
        id: tileContent
        anchors.fill: parent
        anchors.margins: 22
    }
}
