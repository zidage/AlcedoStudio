import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "DataFormat.js" as DataFormat

// One part of a SplitBar legend: the swatch in the color of the part, its name, and its count
// (grouped digits, Manrope with tabular figures). The swatch only names a bar color.
RowLayout {
    id: legend

    property color swatch: "transparent"
    property string label: ""
    property real count: 0
    readonly property alias countLabel: countText

    spacing: appTheme.spaceSm

    Rectangle {
        Layout.alignment: Qt.AlignVCenter
        implicitWidth: 8
        implicitHeight: 8
        radius: 4
        color: legend.swatch
    }
    Label {
        text: legend.label
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeBody
    }
    Label {
        id: countText
        text: DataFormat.grouped(legend.count, Qt.locale())
        color: appTheme.textColor
        font.family: appTheme.headlineFontFamily
        font.pixelSize: appTheme.fontSizeSection
        font.weight: Font.DemiBold
        font.features: { "tnum": 1 }
    }
}
