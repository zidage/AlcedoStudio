import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Selected-node name, image-owned EXIF tokens, and Mask tool buttons under the
// scope slot. Node switching does not reread EXIF; the session publishes the
// four tokens when the open image identity changes. Add Radial / Add Gradient
// arm creation on the selected Color Grade. On a locked (deletion-protected)
// Color Grade the stored Default Behavior decides: ask, insert a new layer
// first, or draw on the locked node.
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
    /// Primary selection is a deletion-protected Color Grade.
    property bool selectedNodeLocked: false
    /// EditorNodeController; inserts the new layer for the "newLayer" choice.
    property var nodeController: null
    /// EditorBehaviorPreferences; null reads as "ask".
    property var editorBehavior: null
    property bool controlsEnabled: true

    // Tool waiting for the inserted layer to become the selected node. The
    // session commits the insert on its worker thread, so the new layer is
    // usually selected after insertMaskGroupAtTop returns; starting the tool
    // before that would target the locked node that is still selected.
    property string pendingMaskToolKind: ""
    property string pendingMaskToolNodeId: ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colIcon: appTheme.iconColor
    readonly property color colIconMuted: theme ? theme.colTextMuted : appTheme.textMutedColor

    function beginMaskTool(kind) {
        if (!root.maskCreation)
            return
        if (kind === "radial")
            root.maskCreation.beginRadial()
        else
            root.maskCreation.beginLinear()
    }

    function beginMaskToolOnNewLayer(kind) {
        // No fallback to the locked node: a failed insert leaves lastError on
        // the node controller and starts no Mask.
        if (!root.nodeController || !root.nodeController.insertMaskGroupAtTop())
            return
        const insertedId = String(root.nodeController.lastInsertedMaskGroupId || "")
        if (insertedId.length === 0)
            return
        if (String(root.nodeController.selectedNodeId || "") === insertedId) {
            root.beginMaskTool(kind)
            return
        }
        root.pendingMaskToolKind = kind
        root.pendingMaskToolNodeId = insertedId
    }

    function startPendingMaskToolIfSelected() {
        if (root.pendingMaskToolKind.length === 0 || !root.nodeController)
            return
        if (String(root.nodeController.selectedNodeId || "") !== root.pendingMaskToolNodeId)
            return
        const kind = root.pendingMaskToolKind
        root.pendingMaskToolKind = ""
        root.pendingMaskToolNodeId = ""
        root.beginMaskTool(kind)
    }

    function requestMaskTool(kind) {
        root.pendingMaskToolKind = ""
        root.pendingMaskToolNodeId = ""
        if (!root.maskCreation)
            return
        const action = root.editorBehavior
                       ? String(root.editorBehavior.lockedNodeMaskAction || "ask")
                       : "ask"
        if (!root.selectedNodeLocked || action === "currentNode") {
            root.beginMaskTool(kind)
        } else if (action === "newLayer") {
            root.beginMaskToolOnNewLayer(kind)
        } else {
            lockedNodeMaskPrompt.openForTool(kind)
        }
    }

    Connections {
        target: root.nodeController
        ignoreUnknownSignals: true
        function onSelectionChanged() { root.startPendingMaskToolIfSelected() }
    }

    LockedNodeMaskPromptDialog {
        id: lockedNodeMaskPrompt
        theme: root.theme
        editorBehavior: root.editorBehavior
        canCreateLayer: root.nodeController
                        && root.nodeController.canEditMaskGroupStructure === true
        onNewLayerRequested: function(kind) { root.beginMaskToolOnNewLayer(kind) }
        onCurrentNodeRequested: function(kind) { root.beginMaskTool(kind) }
    }

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
                    id: radialButton
                    objectName: "editorAdjustmentHeaderRadialButton"
                    compact: true
                    enabled: root.controlsEnabled
                             && root.selectedNodeKind === "colorGrade"
                    selected: root.maskCreation
                              && String(root.maskCreation.toolKind || "") === "radial"
                    iconSrc: "qrc:/mask_icons/radial-add.svg"
                    actionName: qsTr("Add Radial Mask")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                    onClicked: {
                        if (root.maskCreation && radialButton.selected) {
                            root.maskCreation.finishBody()
                        } else {
                            root.requestMaskTool("radial")
                        }
                    }
                }

                IconActionButton {
                    id: gradientButton
                    objectName: "editorAdjustmentHeaderGradientButton"
                    compact: true
                    enabled: root.controlsEnabled
                             && root.selectedNodeKind === "colorGrade"
                    selected: root.maskCreation
                              && String(root.maskCreation.toolKind || "") === "linear"
                    iconSrc: "qrc:/mask_icons/gradient-add.svg"
                    actionName: qsTr("Add Gradient Mask")
                    iconColorDefault: root.colIcon
                    iconColorMuted: root.colIconMuted
                    fillIdle: appTheme.buttonIdleFillColor
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                    onClicked: {
                        if (root.maskCreation && gradientButton.selected) {
                            root.maskCreation.finishBody()
                        } else {
                            root.requestMaskTool("linear")
                        }
                    }
                }
            }
        }
    }
}
