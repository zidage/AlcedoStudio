import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// One read-only Mask source-type row inside a Color Grade drawer.
// Shows only the approved type icon and localized type name. MaskId stays on
// the projection for later selection; it is never UI text.
Item {
    id: root
    objectName: "editorNodeMaskTypeRow"

    property string sourceKind: ""
    property string maskId: ""

    readonly property string typeLabel: {
        if (root.sourceKind === "linearGradient") {
            return qsTr("Gradient")
        }
        if (root.sourceKind === "radial") {
            return qsTr("Radial")
        }
        if (root.sourceKind === "brush") {
            return qsTr("Brush")
        }
        return ""
    }

    readonly property url iconSrc: {
        if (root.sourceKind === "linearGradient") {
            return "qrc:/mask_icons/gradient.svg"
        }
        if (root.sourceKind === "radial") {
            return "qrc:/mask_icons/radial.svg"
        }
        if (root.sourceKind === "brush") {
            return "qrc:/mask_icons/brush.svg"
        }
        return ""
    }

    implicitWidth: appTheme.graphNodeWidth
    implicitHeight: Math.max(appTheme.graphMaskRowHeight, typeName.implicitHeight + appTheme.spaceXs)
    height: implicitHeight
    activeFocusOnTab: false

    Accessible.role: Accessible.StaticText
    Accessible.name: root.typeLabel
    Accessible.ignored: !root.visible || root.typeLabel.length === 0

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceSm
        anchors.rightMargin: appTheme.spaceSm
        spacing: appTheme.spaceSm

        ColorImage {
            id: typeIcon
            objectName: "editorNodeMaskTypeIcon"
            Layout.preferredWidth: appTheme.iconOpticalSizeCompact
            Layout.preferredHeight: appTheme.iconOpticalSizeCompact
            Layout.alignment: Qt.AlignVCenter
            width: appTheme.iconOpticalSizeCompact
            height: appTheme.iconOpticalSizeCompact
            source: root.iconSrc
            sourceSize.width: appTheme.iconSourceSizeCompact
            sourceSize.height: appTheme.iconSourceSizeCompact
            fillMode: Image.PreserveAspectFit
            smooth: true
            color: appTheme.iconColor
            visible: root.iconSrc.toString().length > 0
        }

        Label {
            id: typeName
            objectName: "editorNodeMaskTypeLabel"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: root.typeLabel
            color: appTheme.textColor
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightRegular
            elide: Text.ElideRight
            wrapMode: Text.NoWrap
            Accessible.ignored: true
        }
    }
}
