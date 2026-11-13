import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "DataFormat.js" as DataFormat

// A bento tile with one large count (Settings > LUTs Total, Settings > Local Content Recognition
// Photos): the name in muted Manrope, the count in Manrope ExtraLight 72 px with tabular figures,
// centered so the gaps left and right are equal. With compactLargeCounts a count of 10,000 or
// more shows in its short form (102K, 10.2万) with the exact count below it. A non-empty valueText
// shows in place of the count, and a non-empty detailText stands below it in place of the exact
// count (Settings > About: the version and its build).
BentoTile {
    id: tile

    property string label: ""
    property real count: 0
    property bool compactLargeCounts: false
    property string languageCode: "en"
    property string valueText: ""
    property string detailText: ""
    readonly property alias countLabel: countText
    readonly property alias exactCountLabel: exactText
    readonly property bool showsExactCount: valueText.length === 0 && compactLargeCounts
                                            && count >= 10000

    Layout.preferredWidth: 190
    contentHeight: tileColumn.implicitHeight

    ColumnLayout {
        id: tileColumn
        anchors.fill: parent
        spacing: 0

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: tile.label
            color: appTheme.textMutedColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: Font.Medium
        }
        Item {
            Layout.fillHeight: true
        }
        Label {
            id: countText
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: tile.valueText.length > 0
                  ? tile.valueText
                  : (tile.compactLargeCounts ? DataFormat.compact(tile.count, tile.languageCode, Qt.locale())
                                             : DataFormat.grouped(tile.count, Qt.locale()))
            color: appTheme.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeDisplay
            font.weight: Font.ExtraLight
            font.features: { "tnum": 1 }
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 32
            elide: Text.ElideNone
        }
        Label {
            id: exactText
            Layout.fillWidth: true
            visible: tile.showsExactCount || tile.detailText.length > 0
            horizontalAlignment: Text.AlignHCenter
            text: tile.detailText.length > 0 ? tile.detailText
                                             : DataFormat.grouped(tile.count, Qt.locale())
            color: appTheme.textMutedColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: Font.Medium
            font.features: { "tnum": 1 }
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
