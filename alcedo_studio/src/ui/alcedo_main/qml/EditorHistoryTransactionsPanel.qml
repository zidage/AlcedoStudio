import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Transaction timeline for the editor's active Version, drawn as a Git graph:
// a continuous commit-graph rail with state-driven nodes and flat, airy rows
// (EditorHistoryTransactionEntry + EditorHistoryCommitGraphNode). The rail
// owns the typed model and only supplies the panel with the session-facing
// actions. List scroll is restorable from the rail across Loader teardown
// (Phase 7A R6).
Item {
    id: root
    objectName: "editorHistoryPageBody"

    property var theme: null
    property var editorSession: null
    property var adjustmentTransfer: null
    property var historyModel: null
    property string statusMessage: ""
    property Item blurSource: null

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor

    // Git-graph rows breathe: entries own their height; the list supplies the
    // inter-row gap the graph rail paints through.
    readonly property int listRowGap: appTheme.spaceMd

    // Exposed for the rail: scroll lives on the ListView; the rail stores the
    // value outside this body so reactivation restores the prior viewport.
    readonly property real listContentY: historyList ? historyList.contentY : 0

    function restoreListContentY(y) {
        if (!historyList)
            return
        historyList.restoringContentY = true
        historyList.preservedContentY = Math.max(0, Number(y || 0))
        Qt.callLater(function () {
            if (!historyList)
                return
            var maxY = Math.max(0, historyList.contentHeight - historyList.height)
            historyList.contentY = Math.max(0, Math.min(historyList.preservedContentY, maxY))
            historyList.restoringContentY = false
        })
    }

    EditorMergeDialog {
        id: mergeDialog
        textColor: root.colText
        mutedColor: root.colMuted
        surfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        blurSource: root.blurSource
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

    function applyPaste() {
        if (!root.adjustmentTransfer || !root.editorSession) return
        var result = root.adjustmentTransfer.PasteIntoEditor(root.editorSession)
        root.statusMessage = result.message || qsTr("Adjustments pasted")
    }

    function beginMerge() {
        if (!root.adjustmentTransfer || !root.editorSession) return
        var result = root.adjustmentTransfer.BeginMergeIntoEditor(root.editorSession)
        if (!result.success) {
            root.statusMessage = result.message || qsTr("Merge could not start")
            return
        }
        if (result.hasConflicts) {
            mergeDialog.openPreview(result)
            return
        }
        var completed = root.adjustmentTransfer.CompleteMergeIntoEditor(root.editorSession, [])
        root.statusMessage = completed.message || qsTr("Merge completed")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            objectName: "editorHistoryPanelHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "editorHistoryVersionsPanelTitle"
                    Layout.fillWidth: true
                    text: qsTr("Edit History")
                    color: root.colText
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                }

                Label {
                    objectName: "editorHistoryActiveVersionLabel"
                    Layout.fillWidth: true
                    // Checked-out Version identity only — no transaction count.
                    text: {
                        var name = root.historyModel
                                   ? String(root.historyModel.activeVersionDisplayName || "")
                                   : ""
                        return name.length > 0 ? qsTr("Version: %1").arg(name)
                                               : qsTr("Version: —")
                    }
                    color: root.colMuted
                    elide: Text.ElideRight
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }

        RowLayout {
            objectName: "editorHistoryToolbar"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            IconActionButton {
                objectName: "editorHistoryUndoButton"
                compact: true
                enabled: root.editorSession && root.editorSession.actions.canUndo
                iconSrc: "qrc:/panel_icons/reset.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Undo edit")
                onClicked: root.historyModel.undo()
            }

            IconActionButton {
                objectName: "editorHistoryRedoButton"
                compact: true
                enabled: root.editorSession && root.editorSession.actions.canRedo
                iconSrc: "qrc:/panel_icons/retry.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Redo edit")
                onClicked: root.historyModel.redo()
            }

            Item { Layout.fillWidth: true }

            IconActionButton {
                objectName: "editorHistoryPasteButton"
                compact: true
                enabled: root.editorSession && root.editorSession.actions.canPaste
                iconSrc: "qrc:/panel_icons/to_bg.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Paste adjustments as a new Version")
                onClicked: root.applyPaste()
            }

            IconActionButton {
                objectName: "editorHistoryMergeButton"
                compact: true
                enabled: root.editorSession && root.editorSession.actions.canBeginMerge
                iconSrc: "qrc:/panel_icons/git-branch.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Merge adjustments into this Version")
                onClicked: root.beginMerge()
            }
        }

        Label {
            objectName: "editorHistoryRecoveryNotice"
            Layout.fillWidth: true
            visible: root.historyModel && root.historyModel.recoveredHead
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
            text: {
                if (root.statusMessage.length > 0) return root.statusMessage
                if (root.editorSession && root.editorSession.lastHistoryMessage
                        && root.editorSession.lastHistoryMessage.length > 0) {
                    return root.editorSession.lastHistoryMessage
                }
                return ""
            }
            color: (root.editorSession && root.editorSession.lastHistoryFailed)
                   ? appTheme.dangerColor : root.colMuted
            elide: Text.ElideRight
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        Item {
            objectName: "editorHistoryTimelineWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.colCardBorder
            }

            ListView {
                id: historyList
                objectName: "editorHistoryList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceSm
                clip: true
                model: root.historyModel
                spacing: root.listRowGap
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                // Data-only head moves and checkout-driven model refreshes must
                // not pin the viewport to index 0 when the selection is still
                // on-screen. Capture contentY *before* reset (modelAboutToBeReset)
                // and freeze so a synchronous contentY jump cannot clobber it.
                property real preservedContentY: 0
                property bool restoringContentY: false

                onContentYChanged: {
                    if (!restoringContentY)
                        preservedContentY = contentY
                }

                function captureScrollBeforeReset() {
                    if (!restoringContentY)
                        preservedContentY = contentY
                    restoringContentY = true
                }

                function restoreScrollAfterReset() {
                    restoringContentY = true
                    Qt.callLater(function () {
                        if (!historyList)
                            return
                        var maxY = Math.max(0, historyList.contentHeight - historyList.height)
                        historyList.contentY = Math.max(
                            0, Math.min(historyList.preservedContentY, maxY))
                        historyList.restoringContentY = false
                    })
                }

                Connections {
                    target: root.historyModel
                    ignoreUnknownSignals: true
                    function onModelAboutToBeReset() {
                        historyList.captureScrollBeforeReset()
                    }
                    function onModelReset() {
                        historyList.restoreScrollAfterReset()
                    }
                    function onStateChanged() {
                        if (historyList.restoringContentY)
                            historyList.restoreScrollAfterReset()
                    }
                }

                // Git-graph timeline: each entry draws its graph cell and flat
                // commit row; stacked cells form the continuous rail.
                delegate: EditorHistoryTransactionEntry {
                    theme: root.theme
                    editorSession: root.editorSession
                    historyModel: root.historyModel
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - appTheme.spaceMd
                visible: root.historyModel && root.historyModel.count === 0
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
