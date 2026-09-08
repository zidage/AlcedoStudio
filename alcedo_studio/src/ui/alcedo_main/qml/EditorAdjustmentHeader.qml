import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Selected-node name, image-owned EXIF line, and reserved Mask tool buttons
// under the scope slot. Node switching does not reread EXIF; the session
// publishes the EXIF line when the open image identity changes. Brush / Radial /
// Gradient actions are NM7 authoring placeholders: icons only, no command.
Item {
    id: root
    objectName: "editorAdjustmentHeader"

    property var theme: null
    property string nodeName: ""
    property string exifText: "\u2014"

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colIcon: appTheme.iconColor
    readonly property color colIconMuted: theme ? theme.colTextMuted : appTheme.textMutedColor

    implicitHeight: Math.max(appTheme.editorAdjustmentHeaderMinHeight, headerColumn.implicitHeight)
    implicitWidth: 200
    Accessible.role: Accessible.Grouping
    Accessible.name: root.nodeName

    ColumnLayout {
        id: headerColumn
        anchors.fill: parent
        spacing: appTheme.spaceXs

        Label {
            id: exifLabel
            objectName: "editorAdjustmentHeaderExif"
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.minimumWidth: 0
            Layout.preferredHeight: appTheme.lineHeightCaption
            text: root.exifText
            color: root.colText
            font.family: appTheme.monoFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
            lineHeight: appTheme.lineHeightCaption
            lineHeightMode: Text.FixedHeight
            elide: Text.ElideRight
            wrapMode: Text.NoWrap
            maximumLineCount: 1
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.StaticText
            Accessible.name: root.exifText
        }

        RowLayout {
            id: nameRow
            objectName: "editorAdjustmentHeaderNameRow"
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.minimumWidth: 0
            Layout.fillHeight: true
            spacing: appTheme.spaceSm

            Label {
                id: nodeNameLabel
                objectName: "editorAdjustmentHeaderNodeName"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
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

            RowLayout {
                id: maskTools
                objectName: "editorAdjustmentHeaderMaskTools"
                Layout.alignment: Qt.AlignVCenter
                spacing: 0

                IconActionButton {
                    objectName: "editorAdjustmentHeaderBrushButton"
                    compact: true
                    iconSrc: "qrc:/mask_icons/brush.svg"
                    actionName: qsTr("Brush")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                }

                IconActionButton {
                    objectName: "editorAdjustmentHeaderRadialButton"
                    compact: true
                    iconSrc: "qrc:/mask_icons/radial.svg"
                    actionName: qsTr("Radial")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                }

                IconActionButton {
                    objectName: "editorAdjustmentHeaderGradientButton"
                    compact: true
                    iconSrc: "qrc:/mask_icons/gradient.svg"
                    actionName: qsTr("Gradient")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                }
            }
        }
    }
}
