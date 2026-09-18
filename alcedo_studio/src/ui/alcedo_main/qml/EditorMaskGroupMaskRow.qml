import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// One Mask row inside an EditorMaskGroupDelegate body. The row carries its own
// (NodeId, MaskId): selection, lock, and delete commands target this Mask even
// when another Mask is currently selected for editing.
// Selection follows docs/VI/README.md: the row keeps its fill, text, and SVG
// colors and changes only its neutral outer outline.
Item {
    id: root
    objectName: "editorMaskGroupMaskRow"

    property string nodeId: ""
    property string maskId: ""
    property string sourceKind: ""
    property string displayName: ""
    property bool maskEnabled: true
    property real opacityValue: 1.0
    property bool deletionProtected: false
    property bool selected: false
    // Structure commands (lock/delete) share the group action gate: the session
    // must be editable, no node command may be in flight, and no incomplete
    // draft may own the graph.
    property bool actionsEnabled: true
    property string actionsDisabledReason: ""
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color hoverColor: appTheme.hoverColor
    property color selectionOutlineColor: appTheme.graphSelectionOutlineColor
    property real selectionOutlineWidth: appTheme.graphSelectionOutlineWidth
    property var maskThumbnails: null
    property string maskThumbUrl: ""

    signal clicked()
    signal lockClicked()
    signal deleteClicked()
    signal navigateUp()
    signal navigateDown()

    readonly property string typeLabel: {
        if (root.sourceKind === "linearGradient") {
            return qsTr("Gradient")
        }
        if (root.sourceKind === "radial") {
            return qsTr("Radial")
        }
        return ""
    }
    readonly property string rowName: root.displayName.length > 0
                                      ? root.displayName
                                      : root.typeLabel
    readonly property url typeIconSrc: {
        if (root.sourceKind === "linearGradient") {
            return "qrc:/mask_icons/gradient.svg"
        }
        if (root.sourceKind === "radial") {
            return "qrc:/mask_icons/radial.svg"
        }
        return ""
    }
    readonly property int opacityPercent: Math.round(root.opacityValue * 100)
    readonly property color inkColor: root.maskEnabled ? root.textColor : root.mutedColor
    readonly property color iconTint: root.mutedColor

    implicitWidth: 240
    implicitHeight: Math.max(appTheme.iconButtonHitSizeCompact,
                             appTheme.maskGroupMaskPreviewSize + appTheme.spaceXs * 2)
    height: implicitHeight
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: {
        var name = root.rowName.length > 0 ? root.rowName : qsTr("Mask")
        var parts = [name, qsTr("%1% opacity").arg(root.opacityPercent)]
        if (root.deletionProtected) {
            parts.push(qsTr("deletion locked"))
        }
        if (!root.maskEnabled) {
            parts.push(qsTr("disabled"))
        }
        if (root.selected) {
            parts.push(qsTr("selected"))
        }
        return parts.join(", ")
    }
    Accessible.description: {
        if (!root.actionsEnabled) {
            return root.actionsDisabledReason
        }
        if (root.deletionProtected) {
            return qsTr("Unlock this Mask before deleting it")
        }
        return ""
    }
    Accessible.onPressAction: root.clicked()

    function refreshMaskThumb() {
        maskThumbUrl = (root.maskThumbnails && root.nodeId.length > 0 && root.maskId.length > 0)
                ? String(root.maskThumbnails.thumbnailUrl(root.nodeId, root.maskId) || "")
                : ""
    }

    onNodeIdChanged: refreshMaskThumb()
    onMaskIdChanged: refreshMaskThumb()
    onMaskThumbnailsChanged: refreshMaskThumb()

    Connections {
        target: root.maskThumbnails
        function onThumbnailUrlChanged(changedNodeId, changedMaskId) {
            if (changedNodeId === root.nodeId && changedMaskId === root.maskId) {
                root.refreshMaskThumb()
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
            root.clicked()
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            root.navigateUp()
            event.accepted = true
        } else if (event.key === Qt.Key_Down) {
            root.navigateDown()
            event.accepted = true
        } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                   && root.actionsEnabled && !root.deletionProtected) {
            root.deleteClicked()
            event.accepted = true
        }
    }

    Rectangle {
        objectName: "editorMaskGroupMaskRowWash"
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: rowMouse.containsMouse || root.activeFocus ? root.hoverColor : "transparent"
        border.width: root.selected ? root.selectionOutlineWidth
                                    : (root.activeFocus ? 1 : 0)
        border.color: root.selected ? root.selectionOutlineColor : root.inkColor
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceSm
        anchors.rightMargin: appTheme.spaceXs
        spacing: appTheme.spaceSm

        // Preview well: NM9.3 shows the Mask type glyph as an honest
        // placeholder; the NM9.4 coverage thumbnail lands in the same slot.
        Rectangle {
            objectName: "editorMaskGroupMaskPreview"
            Layout.preferredWidth: appTheme.maskGroupMaskPreviewSize
            Layout.preferredHeight: appTheme.maskGroupMaskPreviewSize
            Layout.alignment: Qt.AlignVCenter
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: "transparent"
            Accessible.ignored: true

            ColorImage {
                anchors.centerIn: parent
                width: appTheme.iconOpticalSizeCompact
                height: appTheme.iconOpticalSizeCompact
                source: root.typeIconSrc
                sourceSize.width: appTheme.iconSourceSizeCompact
                sourceSize.height: appTheme.iconSourceSizeCompact
                fillMode: Image.PreserveAspectFit
                smooth: true
                color: root.iconTint
                visible: root.typeIconSrc.toString().length > 0
                         && maskThumb.source.toString().length === 0
            }

            Image {
                id: maskThumb
                objectName: "editorMaskGroupMaskPreviewImage"
                anchors.fill: parent
                source: root.maskThumbUrl
                cache: false
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                smooth: true
                visible: source.toString().length > 0
            }
        }

        Column {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            Text {
                objectName: "editorMaskGroupMaskName"
                width: parent.width
                text: root.rowName
                color: root.inkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: root.selected ? appTheme.fontWeightStrong
                                           : appTheme.fontWeightRegular
                elide: Text.ElideRight
                wrapMode: Text.NoWrap
                Accessible.ignored: true
            }

            Text {
                objectName: "editorMaskGroupMaskSummary"
                width: parent.width
                text: root.maskEnabled
                      ? qsTr("%1% opacity").arg(root.opacityPercent)
                      : qsTr("Off") + "  " + qsTr("%1% opacity").arg(root.opacityPercent)
                color: root.mutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideRight
                wrapMode: Text.NoWrap
                Accessible.ignored: true
            }
        }

        Item {
            Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
            Layout.alignment: Qt.AlignVCenter

            IconActionButton {
                id: lockButton
                objectName: "editorMaskGroupMaskLockButton"
                anchors.fill: parent
                compact: true
                stretchInLayout: true
                enabled: root.actionsEnabled
                selected: root.deletionProtected
                iconSrc: root.deletionProtected ? "qrc:/panel_icons/lock.svg"
                                                : "qrc:/panel_icons/lock-open.svg"
                iconColorDefault: root.deletionProtected ? root.textColor : root.mutedColor
                iconColorMuted: root.mutedColor
                fillIdle: "transparent"
                fillHover: root.hoverColor
                fillPressed: appTheme.buttonPressedFillColor
                focusRingColor: root.inkColor
                actionName: root.deletionProtected ? qsTr("Unlock %1").arg(root.rowName)
                                                   : qsTr("Lock %1 against deletion").arg(root.rowName)
                toolTipText: enabled ? actionName : root.actionsDisabledReason
                focusOnPointerPress: false
                onClicked: root.lockClicked()
            }
        }

        Item {
            Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
            Layout.alignment: Qt.AlignVCenter

            IconActionButton {
                id: deleteButton
                objectName: "editorMaskGroupMaskDeleteButton"
                anchors.fill: parent
                compact: true
                stretchInLayout: true
                enabled: root.actionsEnabled && !root.deletionProtected
                iconSrc: "qrc:/panel_icons/trash.svg"
                iconColorDefault: root.mutedColor
                iconColorMuted: root.mutedColor
                fillIdle: "transparent"
                fillHover: root.hoverColor
                fillPressed: appTheme.buttonPressedFillColor
                focusRingColor: root.inkColor
                actionName: qsTr("Delete %1").arg(root.rowName)
                toolTipText: {
                    if (!root.actionsEnabled) {
                        return root.actionsDisabledReason
                    }
                    if (root.deletionProtected) {
                        return qsTr("Unlock %1 before deleting").arg(root.rowName)
                    }
                    return actionName
                }
                focusOnPointerPress: false
                onClicked: root.deleteClicked()
            }
        }
    }

    MouseArea {
        id: rowMouse
        anchors.fill: parent
        // Keep the two action buttons' hit targets exclusive: a press on lock
        // or delete must not also select the row.
        anchors.rightMargin: (appTheme.iconButtonHitSizeCompact * 2) + appTheme.spaceXs
                             + appTheme.spaceSm
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.PointingHandCursor
        onPressed: function (mouse) {
            mouse.accepted = true
            root.forceActiveFocus(Qt.MouseFocusReason)
            root.clicked()
        }
    }
}
