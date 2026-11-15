import QtQuick

// "AI" lettermark tile for preview wells. A Mask Group shows it when any of its
// Masks comes from AI segmentation; an AI Mask row shows it in its own well.
// Sized by the parent well; the lettermark scales with the tile.
Rectangle {
    id: root
    objectName: "editorAiMaskTile"

    property color inkColor: appTheme.textColor
    property color borderTint: appTheme.cardBorderColor

    radius: appTheme.controlRadiusSmall
    color: appTheme.bgBaseColor
    border.width: 1
    border.color: root.borderTint
    Accessible.ignored: true

    Text {
        objectName: "editorAiMaskTileText"
        anchors.centerIn: parent
        text: "AI"
        color: root.inkColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: Math.max(8, Math.round(root.height * 0.38))
        font.weight: appTheme.fontWeightStrong
        font.letterSpacing: 0.4
        Accessible.ignored: true
    }
}
