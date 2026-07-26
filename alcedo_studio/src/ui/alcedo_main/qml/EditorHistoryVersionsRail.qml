import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Left History / Versions rail for the editor desktop. The rail stays present
// while its typed graph-backed panel folds beside it.
Item {
    id: root
    objectName: "editorHistoryVersionsRail"

    property var theme: null
    property var editorSession: null
    property var interactionPolicy: null
    property var adjustmentTransfer: (typeof appModules !== "undefined" && appModules)
                                      ? appModules.adjustmentTransfer : null
    property var historyModel: internalHistoryModel
    readonly property bool versionCheckoutEnabled: interactionPolicy
                                                  ? Boolean(interactionPolicy.canCheckoutVersion)
                                                  : true
    readonly property string versionCheckoutDisabledReason: interactionPolicy
                                                            ? String(interactionPolicy.checkoutVersionReason || "")
                                                            : ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colSelectedFill: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor
    readonly property int panelRadius: theme ? theme.panelRadius : appTheme.panelRadius

    readonly property string activePage: editorSession
                                         ? String(editorSession.historyPanelPage || "")
                                         : ""
    readonly property bool panelExpanded: activePage === "history" || activePage === "versions"
    readonly property int railWidth: 48
    readonly property int expandedPanelWidth: appTheme.editorSidePanelWidth
    readonly property int panelGap: appTheme.spaceSm
    property real panelOpenProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property real totalWidth: railWidth + (panelGap + expandedPanelWidth) * panelOpenProgress
    property string statusMessage: ""

    implicitWidth: totalWidth
    implicitHeight: 200
    Layout.preferredWidth: totalWidth
    Layout.minimumWidth: totalWidth
    Layout.maximumWidth: totalWidth
    Layout.fillHeight: true

    EditorHistoryModel {
        id: internalHistoryModel
        editorSession: root.editorSession
    }

    EditorMergeDialog {
        id: mergeDialog
        textColor: root.colText
        mutedColor: root.colMuted
        surfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        onMergeRequested: function(resolutions) {
            if (!root.adjustmentTransfer) {
                root.statusMessage = qsTr("Adjustment transfer is unavailable")
                return
            }
            var result = root.adjustmentTransfer.CompleteMergeIntoEditor(root.editorSession,
                                                                          resolutions)
            root.statusMessage = result.message || qsTr("Merge completed")
        }
        onCancelled: {
            if (root.adjustmentTransfer) {
                var result = root.adjustmentTransfer.CancelMergeIntoEditor(root.editorSession)
                root.statusMessage = result.message || qsTr("Merge cancelled")
            }
        }
    }

    Dialog {
        id: versionNameDialog
        objectName: "editorVersionNameDialog"
        property string editVersionId: ""
        property bool renameMode: false
        modal: true
        title: renameMode ? qsTr("Rename Version") : qsTr("Create Version")
        width: appTheme.editorSidePanelWidth

        footer: RowLayout {
            width: parent.width
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            DialogActionButton {
                objectName: "editorVersionCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                onClicked: versionNameDialog.reject()
            }

            DialogActionButton {
                objectName: "editorVersionAcceptButton"
                text: qsTr("OK")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                onClicked: {
                    if (versionNameField.text.trim().length === 0) {
                        versionNameField.forceActiveFocus()
                        return
                    }
                    versionNameDialog.accept()
                }
            }
        }

        onAccepted: {
            var name = versionNameField.text.trim()
            if (renameMode) {
                root.historyModel.renameVersion(editVersionId, name)
            } else {
                root.historyModel.createVersion(name)
            }
        }

        contentItem: ColumnLayout {
            spacing: appTheme.spaceSm
            Label {
                Layout.fillWidth: true
                text: qsTr("Use a stable name for this editable look.")
                color: root.colMuted
                wrapMode: Text.WordWrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }
            TextField {
                id: versionNameField
                objectName: "editorVersionNameField"
                Layout.fillWidth: true
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                placeholderText: qsTr("Version name")
                selectByMouse: true
                leftPadding: appTheme.spaceSm
                rightPadding: appTheme.spaceSm
                selectionColor: appTheme.editorListSelectedFillColor
                selectedTextColor: appTheme.editorListSelectedInkColor
                background: Rectangle {
                    implicitHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: versionNameField.activeFocus
                                  ? appTheme.editorListSelectedInkColor
                                  : root.colCardBorder
                }
            }
        }

        onOpened: {
            versionNameField.selectAll()
            versionNameField.forceActiveFocus()
        }
    }

    function driveFoldProgress(value) {
        foldManualDrive = true
        panelOpenProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        panelOpenProgress = panelExpanded ? 1 : 0
    }

    function selectPage(page) {
        if (!editorSession) return
        if (String(editorSession.historyPanelPage || "") === page) {
            editorSession.historyPanelPage = ""
        } else {
            editorSession.historyPanelPage = page
        }
    }

    function openCreateVersion() {
        versionNameDialog.renameMode = false
        versionNameDialog.editVersionId = ""
        versionNameField.text = qsTr("Version %1").arg(root.historyModel.versions.count + 1)
        versionNameDialog.open()
    }

    function openRenameVersion(versionId, displayName) {
        versionNameDialog.renameMode = true
        versionNameDialog.editVersionId = versionId
        versionNameField.text = displayName
        versionNameDialog.open()
    }

    function applyPaste() {
        if (!adjustmentTransfer || !editorSession) return
        var result = adjustmentTransfer.PasteIntoEditor(editorSession)
        statusMessage = result.message || qsTr("Adjustments pasted")
    }

    function beginMerge() {
        if (!adjustmentTransfer || !editorSession) return
        var result = adjustmentTransfer.BeginMergeIntoEditor(editorSession)
        if (!result.success) {
            statusMessage = result.message || qsTr("Merge could not start")
            return
        }
        if (result.hasConflicts) {
            mergeDialog.openPreview(result)
            return
        }
        var completed = adjustmentTransfer.CompleteMergeIntoEditor(editorSession, [])
        statusMessage = completed.message || qsTr("Merge completed")
    }

    onPanelExpandedChanged: {
        _foldDuration = panelExpanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive) panelOpenProgress = panelExpanded ? 1 : 0
    }

    Component.onCompleted: {
        panelOpenProgress = panelExpanded ? 1 : 0
        _motionArmed = true
    }

    Behavior on panelOpenProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: Easing.OutCubic
        }
    }

    Rectangle {
        id: rail
        objectName: "editorHistoryRail"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.railWidth
        radius: root.panelRadius
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder

        Column {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: appTheme.spaceSm
            spacing: appTheme.spaceXs

            IconActionButton {
                id: historyRailButton
                objectName: "editorHistoryRailButton"
                compact: true
                enabled: true
                selected: root.activePage === "history"
                iconSrc: "qrc:/history_icons/git-commit-horizontal.svg"
                iconColorDefault: selected ? root.colSelectedInk : root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: root.colSelectedFill
                fillPressed: root.colSelectedFill
                fillSelected: root.colSelectedFill
                focusRingColor: root.colSelectedInk
                actionName: selected ? qsTr("Hide Edit History") : qsTr("Show Edit History")
                onClicked: root.selectPage("history")
            }

            IconActionButton {
                id: versionsRailButton
                objectName: "editorVersionsRailButton"
                compact: true
                enabled: root.versionCheckoutEnabled
                selected: root.activePage === "versions"
                iconSrc: "qrc:/panel_icons/versions.svg"
                iconColorDefault: selected ? root.colSelectedInk : root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: root.colSelectedFill
                fillPressed: root.colSelectedFill
                fillSelected: root.colSelectedFill
                focusRingColor: root.colSelectedInk
                actionName: selected ? qsTr("Hide Versions") : qsTr("Show Versions")
                onClicked: root.selectPage("versions")
            }
        }
    }

    Rectangle {
        id: historyPanel
        objectName: "editorHistoryVersionsPanel"
        anchors.left: rail.right
        anchors.leftMargin: root.panelGap * root.panelOpenProgress
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.expandedPanelWidth * root.panelOpenProgress
        visible: root.panelOpenProgress > 0.001
        radius: root.panelRadius
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        opacity: root.panelOpenProgress
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: appTheme.spaceLg
            spacing: appTheme.spaceMd

            Label {
                objectName: "editorHistoryVersionsPanelTitle"
                Layout.fillWidth: true
                text: root.activePage === "versions" ? qsTr("Versions") : qsTr("Edit History")
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
            }

            Item {
                objectName: "editorHistoryPageBody"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.activePage === "history"

                ColumnLayout {
                    anchors.fill: parent
                    spacing: appTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs

                        IconActionButton {
                            objectName: "editorHistoryUndoButton"
                            compact: true
                            enabled: root.historyModel.canUndo && root.editorSession && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/reset.svg"
                            iconColorDefault: root.colMuted
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: root.colSelectedFill
                            fillPressed: root.colSelectedFill
                            fillSelected: root.colSelectedFill
                            focusRingColor: root.colSelectedInk
                            actionName: qsTr("Undo edit")
                            onClicked: root.historyModel.undo()
                        }
                        IconActionButton {
                            objectName: "editorHistoryRedoButton"
                            compact: true
                            enabled: root.historyModel.canRedo && root.editorSession && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/retry.svg"
                            iconColorDefault: root.colMuted
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: root.colSelectedFill
                            fillPressed: root.colSelectedFill
                            fillSelected: root.colSelectedFill
                            focusRingColor: root.colSelectedInk
                            actionName: qsTr("Redo edit")
                            onClicked: root.historyModel.redo()
                        }
                        Item { Layout.fillWidth: true }
                        IconActionButton {
                            objectName: "editorHistoryPasteButton"
                            compact: true
                            enabled: root.adjustmentTransfer && root.adjustmentTransfer.packageAvailable
                                     && root.editorSession && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/to_bg.svg"
                            iconColorDefault: root.colMuted
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: root.colSelectedFill
                            fillPressed: root.colSelectedFill
                            fillSelected: root.colSelectedFill
                            focusRingColor: root.colSelectedInk
                            actionName: qsTr("Paste adjustments as a new Version")
                            onClicked: root.applyPaste()
                        }
                        IconActionButton {
                            objectName: "editorHistoryMergeButton"
                            compact: true
                            enabled: root.adjustmentTransfer && root.adjustmentTransfer.packageAvailable
                                     && root.editorSession && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/git-branch.svg"
                            iconColorDefault: root.colMuted
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: root.colSelectedFill
                            fillPressed: root.colSelectedFill
                            fillSelected: root.colSelectedFill
                            focusRingColor: root.colSelectedInk
                            actionName: qsTr("Merge adjustments into this Version")
                            onClicked: root.beginMerge()
                        }
                    }

                    Label {
                        objectName: "editorHistoryRecoveryNotice"
                        Layout.fillWidth: true
                        visible: root.historyModel.recoveredHead
                        text: qsTr("Recovered edits are available in this Version.")
                        color: root.colMuted
                        wrapMode: Text.WordWrap
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    Label {
                        objectName: "editorHistoryStatus"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.statusMessage
                        color: root.colMuted
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    ListView {
                        id: historyList
                        objectName: "editorHistoryList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: appTheme.spaceXs
                        model: root.historyModel

                        delegate: Rectangle {
                            objectName: "editorHistoryCard"
                            width: historyList.width
                            height: appTheme.spaceXl * 2
                            radius: appTheme.controlRadiusSmall
                            color: current ? root.colSelectedFill : root.colCardSurface
                            border.width: 1
                            border.color: current ? root.colSelectedInk : root.colCardBorder

                            Column {
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceSm
                                spacing: appTheme.spaceXs
                                Label {
                                    objectName: "editorHistoryCommitTitle"
                                    width: parent.width
                                    text: commitKind === "merge"
                                          ? qsTr("Merge · second parent %1").arg(secondParentId.slice(0, 8))
                                          : label
                                    color: current ? root.colSelectedInk : root.colText
                                    elide: Text.ElideRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeBody
                                    font.weight: appTheme.fontWeightStrong
                                }
                                Label {
                                    width: parent.width
                                    text: fieldKey
                                    color: current ? root.colSelectedInk : root.colMuted
                                    elide: Text.ElideRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - appTheme.spaceMd
                            visible: root.historyModel.count === 0
                            text: qsTr("No edit history yet")
                            color: root.colMuted
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeBody
                        }
                    }
                }
            }

            Item {
                objectName: "editorVersionsPageBody"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.activePage === "versions"

                ColumnLayout {
                    anchors.fill: parent
                    spacing: appTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: root.versionCheckoutEnabled ? qsTr("Named looks")
                                                              : (root.versionCheckoutDisabledReason.length > 0
                                                                 ? root.versionCheckoutDisabledReason
                                                                 : qsTr("Version checkout is unavailable"))
                            color: root.colMuted
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                        IconActionButton {
                            objectName: "editorCreateVersionButton"
                            compact: true
                            enabled: root.versionCheckoutEnabled && root.editorSession && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/plus.svg"
                            iconColorDefault: root.colMuted
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: root.colSelectedFill
                            fillPressed: root.colSelectedFill
                            fillSelected: root.colSelectedFill
                            focusRingColor: root.colSelectedInk
                            actionName: qsTr("Create Version")
                            onClicked: root.openCreateVersion()
                        }
                    }

                    ListView {
                        id: versionList
                        objectName: "editorVersionsList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: appTheme.spaceXs
                        model: root.historyModel.versions

                        delegate: Rectangle {
                            objectName: "editorVersionCard"
                            property string versionId: model.versionId
                            property string displayName: model.displayName
                            property bool versionActive: Boolean(model.active)
                            width: versionList.width
                            height: appTheme.spaceXl * 2 + appTheme.spaceSm
                            radius: appTheme.controlRadiusSmall
                            color: versionActive ? root.colSelectedFill : root.colCardSurface
                            border.width: 1
                            border.color: versionActive ? root.colSelectedInk : root.colCardBorder

                            MouseArea {
                                anchors.fill: parent
                                z: 0
                                enabled: root.versionCheckoutEnabled && !versionActive
                                onClicked: root.historyModel.checkoutVersion(versionId)
                            }

                            Column {
                                anchors.left: parent.left
                                anchors.right: actionRow.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: appTheme.spaceSm
                                spacing: appTheme.spaceXs
                                Label {
                                    objectName: "editorVersionTitle"
                                    width: parent.width
                                    text: displayName
                                    color: versionActive ? root.colSelectedInk : root.colText
                                    elide: Text.ElideRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeBody
                                    font.weight: appTheme.fontWeightStrong
                                }
                                Label {
                                    objectName: "editorVersionSubtitle"
                                    width: parent.width
                                    text: versionActive ? qsTr("Checked out")
                                                         : qsTr("Head %1").arg(headCommitHash.length > 0
                                                                                 ? headCommitHash.slice(0, 8)
                                                                                 : qsTr("image root"))
                                    color: versionActive ? root.colSelectedInk : root.colMuted
                                    elide: Text.ElideRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }

                            Row {
                                id: actionRow
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.rightMargin: appTheme.spaceXs
                                spacing: appTheme.spaceXs
                                z: 2
                                IconActionButton {
                                    objectName: "editorRenameVersionButton"
                                    compact: true
                                    enabled: root.versionCheckoutEnabled && root.editorSession && root.editorSession.canEdit
                                    iconSrc: "qrc:/panel_icons/edit.svg"
                                    iconColorDefault: versionActive ? root.colSelectedInk : root.colMuted
                                    iconColorMuted: root.colMuted
                                    fillIdle: versionActive ? root.colSelectedFill : root.colCardSurface
                                    fillHover: root.colSelectedFill
                                    fillPressed: root.colSelectedFill
                                    fillSelected: root.colSelectedFill
                                    focusRingColor: root.colSelectedInk
                                    actionName: qsTr("Rename Version")
                                    onClicked: root.openRenameVersion(versionId, displayName)
                                }
                                IconActionButton {
                                    objectName: "editorRemoveVersionButton"
                                    compact: true
                                    enabled: root.versionCheckoutEnabled && !versionActive
                                             && root.historyModel.versions.count > 1
                                             && root.editorSession && root.editorSession.canEdit
                                    iconSrc: "qrc:/panel_icons/stop.svg"
                                    iconColorDefault: versionActive ? root.colSelectedInk : root.colMuted
                                    iconColorMuted: root.colMuted
                                    fillIdle: versionActive ? root.colSelectedFill : root.colCardSurface
                                    fillHover: root.colSelectedFill
                                    fillPressed: root.colSelectedFill
                                    fillSelected: root.colSelectedFill
                                    focusRingColor: root.colSelectedInk
                                    actionName: qsTr("Remove Version")
                                    onClicked: root.historyModel.removeVersion(versionId)
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - appTheme.spaceMd
                            visible: root.historyModel.versions.count === 0
                            text: root.versionCheckoutEnabled
                                  ? qsTr("No versions yet")
                                  : (root.versionCheckoutDisabledReason.length > 0
                                     ? root.versionCheckoutDisabledReason
                                     : qsTr("Version checkout is unavailable"))
                            color: root.colMuted
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeBody
                        }
                    }
                }
            }
        }
    }
}
