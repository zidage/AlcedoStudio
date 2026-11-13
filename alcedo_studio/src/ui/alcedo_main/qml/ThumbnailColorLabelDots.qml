import QtQuick
import "ColorLabelNames.js" as ColorLabelNames

// Color label dots of a Library photo (#771, DESIGN.md "Library color labels"): one dot per
// code the photo has, in code order (0 Red ... 6 Gray), after the file name of the caption.
// A dot is 9 px (7 px when compact), overlaps the previous dot by 3 px, and has a 1.5 px ring
// of the surface behind it, so the overlap reads as separate dots. A later code is drawn over
// the earlier one. The classic cards, the justified tiles, and the grouped cells share it.
//
// The dots for every code exist at all times; a change of the mask only shows or hides dots,
// so the caption updates in place.
Row {
    id: root
    objectName: "thumbnailColorLabelDots"

    // Bit set of the codes (`Image.color_labels`): bit k is code k.
    property int labels: 0
    property bool compact: false
    // Surface behind the dots (the card fill or the view background).
    property color ringColor: appTheme.cardSurfaceColor
    property var colors: appTheme.colorLabelColors

    readonly property real dotSize: compact ? 7 : 9
    readonly property real ringWidth: 1.5
    readonly property real overlap: 3
    // Space between the file name and the first dot color (6 px, 4 px when compact); the ring
    // of the first dot lies in it.
    readonly property real leadingGap: (compact ? 4 : 6) - ringWidth
    readonly property int shownCount: {
        let count = 0
        for (let code = 0; code < codeCount; ++code) {
            if (hasCode(code)) {
                ++count
            }
        }
        return count
    }
    readonly property int codeCount: colors ? colors.length : 0

    function hasCode(code) {
        return (labels & (1 << code)) !== 0
    }

    // Names of the shown codes (the user name from Settings or the color name).
    function shownNames() {
        return ColorLabelNames.shownNames(labels, appModules.libraryView
                                                  ? appModules.libraryView.colorLabelNames : [],
                                          codeCount)
    }

    visible: shownCount > 0
    // Each ring box is dotSize + 2 * ringWidth wide; the dots overlap by `overlap`.
    spacing: -(overlap + ringWidth * 2)

    Accessible.role: Accessible.Graphic
    Accessible.name: shownNames().join(", ")

    Repeater {
        model: root.codeCount

        Rectangle {
            required property int index

            objectName: "thumbnailColorLabelDot_" + index
            visible: root.hasCode(index)
            width: root.dotSize + root.ringWidth * 2
            height: width
            radius: width / 2
            color: root.ringColor

            Rectangle {
                anchors.centerIn: parent
                width: root.dotSize
                height: width
                radius: width / 2
                color: root.colors[index]
            }
        }
    }
}
