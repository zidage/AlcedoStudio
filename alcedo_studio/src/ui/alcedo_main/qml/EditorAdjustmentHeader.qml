import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Selected-node name plus image-owned EXIF rows under the scope slot.
// Node switching does not reread EXIF; the session publishes those four rows
// when the open image identity changes.
Item {
    id: root
    objectName: "editorAdjustmentHeader"

    property var theme: null
    property string nodeName: ""
    property string shutterText: "\u2014"
    property string isoText: "\u2014"
    property string apertureText: "\u2014"
    property string focalText: "\u2014"

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colDivider: appTheme.dividerColor

    implicitHeight: Math.max(appTheme.editorAdjustmentHeaderMinHeight, headerRow.implicitHeight)
    implicitWidth: 200
    Accessible.role: Accessible.Grouping
    Accessible.name: root.nodeName

    RowLayout {
        id: headerRow
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            id: nodeNameLabel
            objectName: "editorAdjustmentHeaderNodeName"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignVCenter
            text: root.nodeName
            color: root.colText
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
            lineHeight: appTheme.lineHeightTitle
            lineHeightMode: Text.FixedHeight
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            Accessible.name: root.nodeName
            ToolTip.visible: truncated && hoverHandler.hovered
            ToolTip.delay: 400
            ToolTip.text: root.nodeName

            HoverHandler {
                id: hoverHandler
            }
        }

        Rectangle {
            objectName: "editorAdjustmentHeaderDivider"
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.topMargin: appTheme.spaceXs
            Layout.bottomMargin: appTheme.spaceXs
            color: root.colDivider
        }

        ColumnLayout {
            objectName: "editorAdjustmentHeaderExif"
            Layout.alignment: Qt.AlignVCenter
            Layout.fillHeight: true
            Layout.preferredWidth: implicitWidth
            Layout.maximumWidth: implicitWidth
            spacing: 0

            ExifRow {
                objectName: "editorAdjustmentHeaderShutter"
                label: qsTr("Shutter")
                value: root.shutterText
            }
            ExifRow {
                objectName: "editorAdjustmentHeaderIso"
                label: qsTr("ISO")
                value: root.isoText
            }
            ExifRow {
                objectName: "editorAdjustmentHeaderAperture"
                label: qsTr("Aperture")
                value: root.apertureText
            }
            ExifRow {
                objectName: "editorAdjustmentHeaderFocal"
                label: qsTr("Focal length")
                value: root.focalText
            }
        }
    }

    component ExifRow: RowLayout {
        id: row
        required property string label
        required property string value
        Layout.fillWidth: true
        spacing: appTheme.spaceMd

        Label {
            Layout.alignment: Qt.AlignVCenter
            text: row.label
            color: root.colMuted
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
            elide: Text.ElideRight
        }
        Label {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: row.value
            color: root.colText
            font.family: appTheme.dataFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
        }
    }
}
