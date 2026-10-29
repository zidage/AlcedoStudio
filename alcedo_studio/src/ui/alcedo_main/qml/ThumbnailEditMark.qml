import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

// Edit mark of a Library card and a filmstrip tile (DESIGN.md "Library thumbnail cards"): a
// small chip on the top-right corner of the photo, the adjustments icon on a
// thumbnailEditMarkFillColor fill. It shows only for an edited photo, that is, when the
// active Version has a head commit. The tooltip has two lines: the relative edit time and the
// name of the active Version.
//
// The owner sets image: the chip sits on the top-right corner of the painted image of a
// PreserveAspectFit Image, or of the image item while nothing is painted yet.
//
// The tooltip opens on hover of the mark (an internal HoverHandler), or when the owner view
// sets pointerOver (ThumbnailGridView hit-tests through its input overlay).
Rectangle {
    id: root
    objectName: "thumbnailEditMark"

    property bool edited: false
    // Local time of the head commit (a Date), or undefined.
    property var editedAt: undefined
    property string versionName: ""
    property bool compact: false
    property bool pointerOver: false
    // Image whose painted top-right corner holds the chip, and the insets from that corner.
    property Image image: null
    property real inset: compact ? 3 : appTheme.spaceXs
    property real rightInset: inset
    readonly property int iconSize: compact ? 10 : 12
    readonly property bool hovered: markHover.hovered || pointerOver

    readonly property real paintedWidth: image && image.paintedWidth > 0 ? image.paintedWidth
                                                                          : (image ? image.width : 0)
    readonly property real paintedHeight: image && image.paintedHeight > 0
                                          ? image.paintedHeight : (image ? image.height : 0)

    visible: edited
    width: iconSize + (compact ? 4 : 6)
    height: width
    radius: compact ? 3 : 4
    color: appTheme.thumbnailEditMarkFillColor
    x: image ? image.x + (image.width + paintedWidth) / 2 - width - rightInset : 0
    y: image ? image.y + (image.height - paintedHeight) / 2 + inset : 0
    z: 4

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Edited")
    Accessible.description: tooltipText

    function sameDay(a, b) {
        return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth()
                && a.getDate() === b.getDate()
    }

    // "Edited 2 hours ago", "Edited yesterday", then the date.
    function relativeEditTimeText(when, now) {
        if (!(when instanceof Date) || isNaN(when.getTime())) {
            return qsTr("Edited")
        }
        const elapsedMs = Math.max(0, now.getTime() - when.getTime())
        const minuteMs = 60 * 1000
        const hourMs = 60 * minuteMs
        if (elapsedMs < minuteMs) {
            return qsTr("Edited just now")
        }
        if (elapsedMs < hourMs) {
            return qsTr("Edited %n minute(s) ago", "", Math.floor(elapsedMs / minuteMs))
        }
        if (sameDay(when, now)) {
            return qsTr("Edited %n hour(s) ago", "", Math.floor(elapsedMs / hourMs))
        }
        const yesterday = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1)
        if (sameDay(when, yesterday)) {
            return qsTr("Edited yesterday")
        }
        return qsTr("Edited %1").arg(Qt.formatDate(when, Locale.ShortFormat))
    }

    // Read when the tooltip opens, so the relative time is current.
    property string tooltipText: ""
    function updateTooltipText() {
        const lines = [relativeEditTimeText(editedAt, new Date())]
        if (versionName.length > 0) {
            lines.push(versionName)
        }
        tooltipText = lines.join("\n")
    }
    onHoveredChanged: if (hovered) updateTooltipText()
    onEditedAtChanged: updateTooltipText()
    onVersionNameChanged: updateTooltipText()
    Component.onCompleted: updateTooltipText()

    ColorImage {
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        sourceSize.width: root.iconSize
        sourceSize.height: root.iconSize
        source: "qrc:/panel_icons/adjustments.svg"
        color: appTheme.textColor
    }

    HoverHandler {
        id: markHover
    }

    ToolTip.visible: root.edited && root.hovered
    ToolTip.text: root.tooltipText
    ToolTip.delay: 400
}
