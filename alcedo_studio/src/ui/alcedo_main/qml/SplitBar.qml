import QtQuick

// The split of a count into two parts on one track (Settings > LUTs categories, Settings >
// Local Content Recognition label coverage): the first part from the left, a 3 px gap when both
// parts hold items, then the second part. The legend beside it uses the same two colors
// (LegendItem).
Rectangle {
    id: bar

    property real firstCount: 0
    property real secondCount: 0
    property color firstColor: appTheme.textColor
    property color secondColor: Qt.rgba(appTheme.textMutedColor.r, appTheme.textMutedColor.g,
                                        appTheme.textMutedColor.b, 0.45)

    readonly property real totalCount: Math.max(0, firstCount) + Math.max(0, secondCount)
    readonly property real gap: firstCount > 0 && secondCount > 0 ? 3 : 0

    implicitHeight: 10
    radius: height / 2
    color: appTheme.bgBaseColor
    clip: true

    Rectangle {
        objectName: "splitBarFirst"
        height: parent.height
        width: bar.totalCount > 0 ? (bar.width - bar.gap) * Math.max(0, bar.firstCount) / bar.totalCount
                                  : 0
        radius: height / 2
        color: bar.firstColor
    }

    Rectangle {
        objectName: "splitBarSecond"
        x: bar.width - width
        height: parent.height
        width: bar.totalCount > 0 ? (bar.width - bar.gap) * Math.max(0, bar.secondCount) / bar.totalCount
                                  : 0
        radius: height / 2
        color: bar.secondColor
    }
}
