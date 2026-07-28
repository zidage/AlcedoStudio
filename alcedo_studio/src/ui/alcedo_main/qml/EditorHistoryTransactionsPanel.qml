import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Transaction timeline for the editor's active Version. The rail owns the
// typed model and only supplies the panel with the session-facing actions.
// List scroll is restorable from the rail across Loader teardown (Phase 7A R6).
Item {
    id: root
    objectName: "editorHistoryPageBody"

    property var theme: null
    property var editorSession: null
    property var adjustmentTransfer: null
    property var historyModel: null
    property string statusMessage: ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor

    // Dense list row (same formula as LUTPanel entry wells): body + caption + gap.
    readonly property int entryRowHeight: appTheme.lineHeightBody
                                          + appTheme.lineHeightCaption
                                          + appTheme.spaceSm
    readonly property int listRowGap: appTheme.spaceXs
    // Timeline gutter for the rail + marker (compact, not spaceXl card inset).
    readonly property int timelineGutter: appTheme.spaceMd + appTheme.spaceXs

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

    function relativeTime(createdAtNs) {
        var timestamp = Number(createdAtNs || 0)
        if (timestamp <= 0) return qsTr("Earlier")

        var ageSeconds = Math.max(0, Math.floor((Date.now() * 1000000 - timestamp) / 1000000000))
        if (ageSeconds < 60) return qsTr("Just now")
        if (ageSeconds < 3600) return qsTr("%1 min ago").arg(Math.floor(ageSeconds / 60))
        if (ageSeconds < 86400) return qsTr("%1 hr ago").arg(Math.floor(ageSeconds / 3600))
        return qsTr("%1 days ago").arg(Math.floor(ageSeconds / 86400))
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
                enabled: root.historyModel && root.historyModel.canUndo
                         && root.editorSession && root.editorSession.canEdit
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
                enabled: root.historyModel && root.historyModel.canRedo
                         && root.editorSession && root.editorSession.canEdit
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
                enabled: root.adjustmentTransfer && root.adjustmentTransfer.packageAvailable
                         && root.editorSession && root.editorSession.canEdit
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
                enabled: root.adjustmentTransfer && root.adjustmentTransfer.packageAvailable
                         && root.editorSession && root.editorSession.canEdit
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
                anchors.margins: appTheme.spaceXs
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

                // Dense two-line wells (LUT-style). Timeline rail + marker sit
                // in a narrow gutter; the well carries title/time then hash|delta.
                delegate: Item {
                    id: transactionDelegate
                    objectName: "editorHistoryTransactionDelegate"
                    width: ListView.view ? ListView.view.width : 0
                    height: root.entryRowHeight

                    required property bool current
                    required property bool isMerge
                    required property string commitId
                    required property string displayName
                    required property string beforeText
                    required property string afterText
                    required property string deltaText
                    required property string timelinePosition
                    required property string mergeSummary
                    required property string secondParentId
                    required property var createdAtNs

                    property bool currentTransaction: current
                    property bool mergeTransaction: isMerge
                    property string transactionId: String(commitId || "")
                    property string transactionDisplayName: String(displayName || "")
                    property string transactionBefore: String(beforeText || "")
                    property string transactionAfter: String(afterText || "")
                    property string transactionDelta: String(deltaText || "")
                    property string transactionPosition: String(timelinePosition || "applied")
                    property string transactionMergeSummary: String(mergeSummary || "")
                    property string secondParent: String(secondParentId || "")
                    property string transactionTime: root.relativeTime(createdAtNs)
                    property string shortCommitId: transactionId.length > 0
                                                  ? transactionId.slice(0, 8) : ""
                    property bool futureRow: transactionPosition === "future"
                    property bool canMove: root.editorSession && root.editorSession.canEdit
                                           && transactionId.length > 0 && !currentTransaction

                    function activateMove() {
                        if (transactionDelegate.canMove && root.historyModel) {
                            root.historyModel.moveHeadToCommit(transactionDelegate.transactionId)
                        }
                    }

                    ListView.onPooled: {}
                    ListView.onReused: {}

                    // Vertical timeline rail through the list well.
                    Rectangle {
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.leftMargin: appTheme.spaceSm + (appTheme.spaceXs / 2)
                                            - 0.5
                        width: 1
                        color: root.colCardBorder
                    }

                    Rectangle {
                        id: transactionMarker
                        anchors.left: parent.left
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        width: transactionDelegate.mergeTransaction
                               ? appTheme.spaceSm + 2
                               : appTheme.spaceXs + 2
                        height: width
                        radius: width / 2
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: transactionDelegate.currentTransaction
                                      ? root.colText : root.colMuted
                        z: 2
                    }

                    Rectangle {
                        id: transactionCard
                        objectName: "editorHistoryCard"
                        anchors.left: parent.left
                        anchors.leftMargin: root.timelineGutter
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: root.entryRowHeight
                        radius: appTheme.controlRadiusSmall
                        color: root.colCardSurface
                        property color selectionOutlineColor: transactionDelegate.currentTransaction
                                                              ? root.colText : root.colCardBorder
                        border.width: 1
                        border.color: selectionOutlineColor
                        activeFocusOnTab: true
                        Accessible.role: Accessible.ListItem
                        Accessible.name: transactionDelegate.transactionDisplayName
                        Accessible.description: transactionDelegate.transactionDelta.length > 0
                                                 ? transactionDelegate.transactionDelta
                                                 : transactionDelegate.transactionId
                        ToolTip.text: transactionDelegate.transactionId.length > 0
                                      ? qsTr("Commit %1").arg(transactionDelegate.transactionId)
                                      : ""
                        ToolTip.visible: cardMouseArea.containsMouse
                        ToolTip.delay: 500
                        Keys.onEnterPressed: {
                            transactionDelegate.activateMove()
                            event.accepted = true
                        }
                        Keys.onReturnPressed: {
                            transactionDelegate.activateMove()
                            event.accepted = true
                        }
                        Keys.onSpacePressed: {
                            transactionDelegate.activateMove()
                            event.accepted = true
                        }

                        MouseArea {
                            id: cardMouseArea
                            objectName: "editorHistoryCardMouseArea"
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: transactionDelegate.canMove
                                        ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: transactionDelegate.activateMove()
                        }

                        // Two-line dense layout (LUT entry well pattern):
                        //   row 1 — title ……………………… relative time
                        //   row 2 — commit hash ……… delta / merge summary
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: appTheme.spaceSm
                            anchors.rightMargin: appTheme.spaceSm
                            anchors.topMargin: appTheme.spaceXs / 2
                            anchors.bottomMargin: appTheme.spaceXs / 2
                            spacing: 0

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.lineHeightBody
                                spacing: appTheme.spaceXs

                                Label {
                                    objectName: "editorHistoryCommitTitle"
                                    Layout.fillWidth: true
                                    text: transactionDelegate.mergeTransaction
                                          ? qsTr("Merge")
                                          : (transactionDelegate.transactionDisplayName.length > 0
                                             ? transactionDelegate.transactionDisplayName
                                             : qsTr("Adjustment"))
                                    color: transactionDelegate.futureRow ? root.colMuted
                                                                         : root.colText
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeBody
                                    font.weight: appTheme.fontWeightRegular
                                }

                                Label {
                                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                    text: transactionDelegate.transactionTime
                                    color: root.colMuted
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.lineHeightCaption
                                spacing: appTheme.spaceXs

                                // Left half: commit identity (minigit mono).
                                Label {
                                    objectName: "editorHistoryCommitHash"
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    visible: transactionDelegate.shortCommitId.length > 0
                                             || transactionDelegate.mergeTransaction
                                    text: {
                                        if (transactionDelegate.mergeTransaction
                                                && transactionDelegate.secondParent.length > 0)
                                            return qsTr("2nd %1")
                                                   .arg(transactionDelegate.secondParent.slice(0, 8))
                                        if (transactionDelegate.shortCommitId.length > 0)
                                            return transactionDelegate.shortCommitId
                                        return ""
                                    }
                                    color: root.colMuted
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                    font.family: appTheme.monoFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }

                                // Right half: adjustment delta or merge summary.
                                Label {
                                    objectName: "editorHistoryCommitValue"
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 1
                                    text: transactionDelegate.mergeTransaction
                                          ? (transactionDelegate.transactionMergeSummary.length > 0
                                             ? transactionDelegate.transactionMergeSummary
                                             : qsTr("Resolved adjustments"))
                                          : transactionDelegate.transactionDelta
                                    color: root.colMuted
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignRight
                                    verticalAlignment: Text.AlignVCenter
                                    font.family: appTheme.monoFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }
                        }
                    }
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
