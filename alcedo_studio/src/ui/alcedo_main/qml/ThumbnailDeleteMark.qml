import QtQuick
import QtQuick.Controls.impl

// Delete mark of a Library photo (#771, DESIGN.md "Library color labels"): a
// rgba(6, 6, 6, 0.72) scrim over the photo area with a round trash badge in the centre. The
// owner fills the photo area with it, desaturates the photo under it, and keeps the edit mark
// above it. The mark takes no pointer input, so the photo stays selectable and editable.
Rectangle {
    id: root
    objectName: "thumbnailDeleteMark"

    property bool marked: false
    property bool compact: false

    readonly property int badgeSize: compact ? 24 : 34
    // MultiEffect saturation of the photo under the mark (CSS saturate(0.35)).
    readonly property real photoSaturation: marked ? -0.65 : 0

    visible: marked
    color: Qt.rgba(6 / 255, 6 / 255, 6 / 255, 0.72)
    z: 3

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Marked for deletion")

    Rectangle {
        objectName: "thumbnailDeleteMarkBadge"
        anchors.centerIn: parent
        width: root.badgeSize
        height: width
        radius: width / 2
        color: appTheme.bgDeepColor
        border.width: 1
        border.color: appTheme.controlBorderColor

        ColorImage {
            anchors.centerIn: parent
            width: root.compact ? 12 : 16
            height: width
            sourceSize.width: width
            sourceSize.height: height
            source: "qrc:/panel_icons/trash.svg"
            color: appTheme.textColor
        }
    }
}
