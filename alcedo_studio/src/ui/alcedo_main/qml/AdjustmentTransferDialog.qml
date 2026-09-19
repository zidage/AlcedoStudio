pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "adjustmentTransferDialog"
    font.family: appTheme.uiFontFamily

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2
                           : appTheme.editorSidePanelWidthMin
                             + appTheme.editorSidePanelWidth
                             + appTheme.editorSidePanelWidthMax,
                    appTheme.editorSidePanelWidthMin
                    + appTheme.editorSidePanelWidth
                    + appTheme.editorSidePanelWidthMax)
    height: Math.min(parent ? parent.height - appTheme.spaceXl * 2
                            : appTheme.editorSidePanelWidthMax,
                     appTheme.editorSidePanelWidthMax)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    property string mode: "copy"
    property string pasteStrategy: "paste"
    property string sourceTitle: ""
    // Read-only package summary rows for Paste mode. Never edited in QML.
    // Each row carries key, section (node display name), label, value,
    // checked, plus node (row group index), itemSection, and itemKind.
    property var adjustmentRows: []
    // AdjustmentTransferDialogModel owned by AdjustmentTransferController.
    // C++ owns every checked state; QML sends commands with stable identities.
    property var dialogModel: null
    property Item blurSource: null
    property real cornerRadius: 0

    signal copyAccepted()
    signal pasteAccepted(string strategy)
    signal pasteDiscarded()

    readonly property bool copyMode: mode === "copy"
    readonly property string selectedSourceVersionId:
        dialogModel ? String(dialogModel.selectedVersionId || "") : ""

    // Paste-mode read-only node/item panes. `focusedPasteNodeIndex` tracks the
    // paste node whose items fill the item pane; no selection mutation exists.
    property int focusedPasteNodeIndex: 0
    property ListModel pasteNodeModel: ListModel {}
    property ListModel pasteItemModel: ListModel {}

    function rebuildPasteModels() {
        pasteNodeModel.clear()
        pasteItemModel.clear()
        const rows = adjustmentRows || []
        const nodeKeys = []
        const nodeNames = ({})
        for (let index = 0; index < rows.length; ++index) {
            const row = rows[index] || ({})
            const key = row.node !== undefined
                        ? "n" + row.node
                        : "s:" + String(row.section || "")
            if (!(key in nodeNames)) {
                nodeNames[key] = String(row.section || qsTr("Other"))
                nodeKeys.push(key)
            }
        }
        if (focusedPasteNodeIndex >= nodeKeys.length) {
            focusedPasteNodeIndex = 0
        }
        for (let index = 0; index < nodeKeys.length; ++index) {
            pasteNodeModel.append({
                nodeId: nodeKeys[index],
                displayName: nodeNames[nodeKeys[index]],
                nodeKind: 0,
                defaultGrade: false,
                checkState: Qt.Unchecked,
                focused: index === focusedPasteNodeIndex
            })
        }
        rebuildPasteItems()
    }

    function rebuildPasteItems() {
        pasteItemModel.clear()
        if (pasteNodeModel.count === 0 || focusedPasteNodeIndex >= pasteNodeModel.count) {
            return
        }
        const focusedKey = String(pasteNodeModel.get(focusedPasteNodeIndex).nodeId)
        const rows = adjustmentRows || []
        for (let index = 0; index < rows.length; ++index) {
            const row = rows[index] || ({})
            const key = row.node !== undefined
                        ? "n" + row.node
                        : "s:" + String(row.section || "")
            if (key !== focusedKey) {
                continue
            }
            pasteItemModel.append({
                itemKey: String(row.key || ""),
                displayName: String(row.label || ""),
                displayValue: String(row.value || ""),
                itemSection: row.itemSection !== undefined ? Number(row.itemSection) : 0,
                itemKind: row.itemKind !== undefined ? Number(row.itemKind) : 0,
                checked: true,
                enabled: true
            })
        }
    }

    function focusPasteNode(nodeKey) {
        for (let index = 0; index < pasteNodeModel.count; ++index) {
            if (String(pasteNodeModel.get(index).nodeId) !== String(nodeKey)) {
                continue
            }
            if (index === focusedPasteNodeIndex) {
                return
            }
            pasteNodeModel.setProperty(focusedPasteNodeIndex, "focused", false)
            focusedPasteNodeIndex = index
            pasteNodeModel.setProperty(index, "focused", true)
            rebuildPasteItems()
            return
        }
    }

    function titleText() {
        return copyMode ? qsTr("Copy Adjustments") : qsTr("Paste Adjustments")
    }

    function acceptText() {
        return copyMode ? qsTr("Copy Adjustments") : qsTr("Paste Adjustments")
    }

    onAdjustmentRowsChanged: rebuildPasteModels()

    onOpened: {
        focusedPasteNodeIndex = 0
        rebuildPasteModels()
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: dialog.cornerRadius
            color: appTheme.textColor
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: dialog.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: dialog.blurSource
                blurEnabled: dialog.blurSource !== null
                blur: 0.72
                blurMax: 72
                saturation: -0.24
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: appTheme.overlayColor
            }
        }
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    contentItem: Rectangle {
        color: "transparent"
        radius: appTheme.panelRadius
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Header bar ──────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceMd
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: dialog.titleText()
                            color: appTheme.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: appTheme.fontWeightHeading
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: dialog.sourceTitle.length > 0
                            text: dialog.sourceTitle
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            elide: Text.ElideMiddle
                        }
                    }

                    Item {
                        id: closeButton
                        objectName: "adjustmentTransferCloseButton"
                        Layout.preferredWidth: appTheme.iconButtonHitSize
                        Layout.preferredHeight: appTheme.iconButtonHitSize
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Close")
                        Accessible.onPressAction: dialog.reject()

                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) {
                                dialog.reject()
                                event.accepted = true
                            }
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: appTheme.iconButtonHitSize - appTheme.spaceSm
                            height: width
                            radius: appTheme.controlRadiusSmall
                            color: closeMouse.containsMouse
                                   ? appTheme.buttonHoveredFillColor
                                   : appTheme.buttonIdleFillColor
                            border.width: closeButton.activeFocus ? 1 : 0
                            border.color: Qt.rgba(appTheme.accentColor.r,
                                                  appTheme.accentColor.g,
                                                  appTheme.accentColor.b, 0.60)
                        }

                        Label {
                            anchors.centerIn: parent
                            text: "×"
                            color: closeMouse.containsMouse
                                   ? appTheme.textColor : appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dialog.reject()
                        }
                    }
                }
            }

            // ── Central workspace ─────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                AdjustmentTransferVersionPane {
                    Layout.preferredWidth: appTheme.editorSidePanelWidthMin
                    Layout.fillHeight: true
                    visible: dialog.copyMode
                    model: dialog.copyMode && dialog.dialogModel
                           ? dialog.dialogModel.versions : null
                    onVersionActivated: function(versionId) {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.SelectVersion(versionId)
                        }
                    }
                }

                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    visible: dialog.copyMode
                    color: appTheme.dividerColor
                }

                AdjustmentTransferNodePane {
                    Layout.preferredWidth: appTheme.editorSidePanelWidth
                    Layout.fillHeight: true
                    readOnly: !dialog.copyMode
                    model: dialog.copyMode
                           ? (dialog.dialogModel ? dialog.dialogModel.nodes : null)
                           : dialog.pasteNodeModel
                    allCheckState: dialog.copyMode && dialog.dialogModel
                                   ? dialog.dialogModel.allNodesCheckState
                                   : Qt.Unchecked
                    onNodeFocused: function(nodeId) {
                        if (dialog.copyMode) {
                            if (dialog.dialogModel) {
                                dialog.dialogModel.FocusNode(nodeId)
                            }
                        } else {
                            dialog.focusPasteNode(nodeId)
                        }
                    }
                    onNodeCheckRequested: function(nodeId, checked) {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.SetNodeChecked(nodeId, checked)
                        }
                    }
                    onSelectAllRequested: function(checked) {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.SetAllNodesChecked(checked)
                        }
                    }
                    onClearRequested: {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.ClearAll()
                        }
                    }
                }

                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    color: appTheme.dividerColor
                }

                AdjustmentTransferItemPane {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    readOnly: !dialog.copyMode
                    model: dialog.copyMode
                           ? (dialog.dialogModel ? dialog.dialogModel.items : null)
                           : dialog.pasteItemModel
                    title: {
                        if (dialog.copyMode) {
                            return dialog.dialogModel
                                    && String(dialog.dialogModel.focusedNodeName || "").length > 0
                                   ? String(dialog.dialogModel.focusedNodeName)
                                   : qsTr("Items")
                        }
                        return dialog.pasteNodeModel.count > 0
                               && dialog.focusedPasteNodeIndex < dialog.pasteNodeModel.count
                               ? String(dialog.pasteNodeModel.get(
                                            dialog.focusedPasteNodeIndex).displayName)
                               : qsTr("Parameters to Paste")
                    }
                    bulkCheckState: dialog.copyMode && dialog.dialogModel
                                    ? dialog.dialogModel.focusedItemsCheckState
                                    : Qt.Unchecked
                    errorText: dialog.copyMode && dialog.dialogModel
                               ? String(dialog.dialogModel.errorText || "")
                               : ""
                    onItemCheckRequested: function(itemKey, checked) {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.SetItemChecked(
                                dialog.dialogModel.focusedNodeId, itemKey, checked)
                        }
                    }
                    onSelectAllRequested: function(checked) {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.SetAllFocusedNodeItemsChecked(checked)
                        }
                    }
                    onClearRequested: {
                        if (dialog.dialogModel) {
                            dialog.dialogModel.ClearFocusedNode()
                        }
                    }
                }
            }

            // ── Footer bar ───────────────────────────────────────────────────
            Rectangle {
                objectName: "adjustmentTransferFooter"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceLg
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceLg
                    spacing: appTheme.spaceSm

                    Item {
                        Layout.fillWidth: true
                    }

                    DialogActionButton {
                        objectName: "adjustmentTransferCancelButton"
                        buttonWidth: appTheme.spaceXl * 5
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: qsTr("Cancel")
                        onClicked: {
                            if (!dialog.copyMode) {
                                dialog.pasteDiscarded()
                            }
                            dialog.close()
                        }
                    }

                    DialogActionButton {
                        objectName: "adjustmentTransferAcceptButton"
                        kind: "accent"
                        buttonWidth: appTheme.spaceXl * 7
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: dialog.acceptText()
                        enabled: dialog.copyMode
                                 ? (dialog.dialogModel && dialog.dialogModel.canCopy === true)
                                 : true
                        onClicked: {
                            if (dialog.copyMode) {
                                dialog.copyAccepted()
                            } else {
                                dialog.pasteAccepted(dialog.pasteStrategy)
                            }
                            dialog.close()
                        }
                    }
                }
            }
        }
    }
}
