import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Selected-node name, image-owned EXIF tokens, and Mask tool buttons under the
// scope slot. Node switching does not reread EXIF; the session publishes the
// four tokens when the open image identity changes. Radial / Gradient arm
// analytic creation on the selected Color Grade. Brush remains disabled until
// accumulating-stroke UI exists.
Item {
    id: root
    objectName: "editorAdjustmentHeader"

    property var theme: null
    property string nodeName: ""
    property string focalText: "\u2014"
    property string apertureText: "\u2014"
    property string shutterText: "\u2014"
    property string isoText: "\u2014"
    property var maskCreation: null
    property string selectedNodeKind: ""
    property bool cropOverlayVisible: false
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colIcon: appTheme.iconColor
    readonly property color colIconMuted: theme ? theme.colTextMuted : appTheme.textMutedColor

    implicitHeight: Math.max(appTheme.editorAdjustmentHeaderMinHeight, headerColumn.implicitHeight)
    implicitWidth: 200
    Accessible.role: Accessible.Grouping
    Accessible.name: root.nodeName

    component ExifToken: Label {
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        Layout.minimumWidth: 0
        Layout.preferredHeight: appTheme.lineHeightCaption
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
        Accessible.name: text
    }

    ColumnLayout {
        id: headerColumn
        anchors.fill: parent
        spacing: appTheme.spaceXs

        RowLayout {
            id: exifRow
            objectName: "editorAdjustmentHeaderExif"
            Layout.fillWidth: true
            Layout.preferredWidth: 0
            Layout.minimumWidth: 0
            Layout.preferredHeight: appTheme.lineHeightCaption
            spacing: appTheme.spaceXs

            ExifToken {
                objectName: "editorAdjustmentHeaderFocal"
                text: root.focalText
                horizontalAlignment: Text.AlignLeft
            }
            ExifToken {
                objectName: "editorAdjustmentHeaderAperture"
                text: root.apertureText
                horizontalAlignment: Text.AlignHCenter
            }
            ExifToken {
                objectName: "editorAdjustmentHeaderShutter"
                text: root.shutterText
                horizontalAlignment: Text.AlignHCenter
            }
            ExifToken {
                objectName: "editorAdjustmentHeaderIso"
                text: root.isoText
                horizontalAlignment: Text.AlignRight
            }
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
                    enabled: false
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
                    id: radialButton
                    objectName: "editorAdjustmentHeaderRadialButton"
                    compact: true
                    enabled: root.controlsEnabled
                             && root.selectedNodeKind === "colorGrade"
                             && !root.cropOverlayVisible
                    selected: root.maskCreation
                              && String(root.maskCreation.toolKind || "") === "radial"
                    iconSrc: "qrc:/mask_icons/radial.svg"
                    actionName: qsTr("Radial")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                    onClicked: {
                        if (root.maskCreation && radialButton.selected) {
                            root.maskCreation.cancel()
                        } else if (root.maskCreation) {
                            root.maskCreation.beginRadial()
                        }
                    }
                }

                IconActionButton {
                    id: gradientButton
                    objectName: "editorAdjustmentHeaderGradientButton"
                    compact: true
                    enabled: root.controlsEnabled
                             && root.selectedNodeKind === "colorGrade"
                             && !root.cropOverlayVisible
                    selected: root.maskCreation
                              && String(root.maskCreation.toolKind || "") === "linear"
                    iconSrc: "qrc:/mask_icons/gradient.svg"
                    actionName: qsTr("Gradient")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                    onClicked: {
                        if (root.maskCreation && gradientButton.selected) {
                            root.maskCreation.cancel()
                        } else if (root.maskCreation) {
                            root.maskCreation.beginLinear()
                        }
                    }
                }
            }
        }
    }
}
