import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

// Drop guide of the drag between the preset part and the Versions part: a dashed band
// at the bottom of the target part while a drag can go to it, and over the whole part,
// with the result of the drop and the dragged name, while the drag is over it. The
// guide takes no input: the items under it still get clicks, wheel, and drops.
// Fill the target well with it (anchors.fill); it sits above the content of the well.
Item {
    id: root
    objectName: "dropGuideOverlay"

    // A drag can go to this part: show the band.
    property bool shown: false
    // The drag is over this part: cover the part.
    property bool expanded: false
    property string bandText: ""
    property string expandedText: ""
    // The name of the dragged item; empty for none.
    property string detailText: ""

    readonly property bool active: shown || expanded
    readonly property real bandHeight: appTheme.iconButtonHitSizeCompact + appTheme.spaceMd

    visible: active
    z: 50

    Item {
        id: frame
        objectName: "dropGuideFrame"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: root.expanded ? 0 : appTheme.spaceXs
        height: root.expanded
                ? root.height
                : Math.min(root.bandHeight, Math.max(0, root.height - appTheme.spaceXs * 2))

        Rectangle {
            anchors.fill: parent
            radius: appTheme.controlRadiusSmall
            color: root.expanded
                   ? appTheme.overlayColor
                   : Qt.rgba(appTheme.cardSurfaceColor.r, appTheme.cardSurfaceColor.g,
                             appTheme.cardSurfaceColor.b, 0.92)
            border.width: root.expanded ? 1 : 0
            border.color: appTheme.textColor
        }

        // The band outline is dashed; Rectangle draws solid borders only.
        Shape {
            anchors.fill: parent
            visible: !root.expanded
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                fillColor: "transparent"
                strokeColor: appTheme.textMutedColor
                strokeWidth: 1
                strokeStyle: ShapePath.DashLine
                dashPattern: [4, 3]

                PathRectangle {
                    x: 0.5
                    y: 0.5
                    width: Math.max(0, frame.width - 1)
                    height: Math.max(0, frame.height - 1)
                    radius: appTheme.controlRadiusSmall
                }
            }
        }

        component PlusMark: Rectangle {
            id: mark
            property real size: 20
            property color ink: appTheme.textMutedColor
            width: size
            height: size
            radius: Math.round(size / 4)
            color: "transparent"
            border.width: 1.5
            border.color: ink

            Rectangle {
                anchors.centerIn: parent
                width: Math.round(mark.size * 0.45)
                height: 1.5
                color: mark.ink
            }

            Rectangle {
                anchors.centerIn: parent
                width: 1.5
                height: Math.round(mark.size * 0.45)
                color: mark.ink
            }
        }

        Row {
            objectName: "dropGuideBandContent"
            anchors.centerIn: parent
            visible: !root.expanded
            spacing: appTheme.spaceSm

            PlusMark {
                anchors.verticalCenter: parent.verticalCenter
                size: 20
            }

            Label {
                objectName: "dropGuideBandLabel"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, frame.width - 20 - appTheme.spaceSm
                                                - appTheme.spaceMd * 2)
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: root.bandText
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        Column {
            objectName: "dropGuideExpandedContent"
            anchors.centerIn: parent
            width: frame.width - appTheme.spaceMd * 2
            visible: root.expanded
            spacing: appTheme.spaceXs

            PlusMark {
                anchors.horizontalCenter: parent.horizontalCenter
                size: 28
                ink: appTheme.textColor
            }

            Label {
                objectName: "dropGuideExpandedLabel"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: root.expandedText
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }

            Label {
                objectName: "dropGuideDetailLabel"
                width: parent.width
                visible: root.detailText.length > 0
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
                textFormat: Text.PlainText
                text: root.detailText
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }
    }
}
