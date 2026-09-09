import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// One Mask source-type row inside a Color Grade drawer. Selection is
// NodeId/MaskId owned; click does not rebuild the list. Delete is a compact
// IconActionButton and does not propagate into row selection.
Item {
    id: root
    objectName: "editorNodeMaskTypeRow"

    property string sourceKind: ""
    property string maskId: ""
    property bool selected: false

    signal clicked
    signal deleteClicked

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
    implicitHeight: appTheme.graphMaskRowHeight
    height: implicitHeight
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: root.typeLabel
    Accessible.ignored: !root.visible || root.typeLabel.length === 0
    Accessible.onPressAction: root.clicked()

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
            root.clicked()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            root.deleteClicked()
            event.accepted = true
        }
    }

    Rectangle {
        objectName: "editorNodeMaskTypeRowWash"
        anchors.fill: parent
        anchors.leftMargin: appTheme.graphSelectionOutlineWidth
        anchors.rightMargin: appTheme.graphSelectionOutlineWidth
        color: root.selected ? appTheme.selectedTintColor
                            : (rowMouse.containsMouse || root.activeFocus
                               ? appTheme.hoverColor : "transparent")
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceSm
        anchors.rightMargin: appTheme.spaceXs
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

        IconActionButton {
            id: deleteButton
            objectName: "editorNodeMaskTypeRowDelete"
            compact: true
            width: appTheme.graphMaskRowHeight
            height: appTheme.graphMaskRowHeight
            Layout.preferredWidth: appTheme.graphMaskRowHeight
            Layout.preferredHeight: appTheme.graphMaskRowHeight
            Layout.alignment: Qt.AlignVCenter
            iconSrc: "qrc:/panel_icons/trash.svg"
            actionName: qsTr("Delete %1").arg(root.typeLabel)
            focusOnPointerPress: false
            onClicked: root.deleteClicked()
        }
    }

    MouseArea {
        id: rowMouse
        anchors.fill: parent
        anchors.rightMargin: appTheme.graphMaskRowHeight
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
