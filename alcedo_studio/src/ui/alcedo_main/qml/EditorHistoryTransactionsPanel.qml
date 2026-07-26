import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Transaction timeline for the editor's active Version. The rail owns the
// typed model and only supplies the panel with the session-facing actions.
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
                    Layout.fillWidth: true
                    text: qsTr("%1 transactions").arg(root.historyModel ? root.historyModel.count : 0)
                    color: root.colMuted
                    font.family: appTheme.dataFontFamily
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
            text: root.statusMessage
            color: root.colMuted
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
                spacing: 0

                delegate: Item {
                    id: transactionDelegate
                    objectName: "editorHistoryTransactionDelegate"
                    width: historyList.width
                    property bool currentTransaction: Boolean(current)
                    property bool mergeTransaction: String(commitKind || "") === "merge"
                    property string transactionField: String(fieldKey || "")
                    property string transactionLabel: String(label || "")
                    property string transactionId: String(commitId || "")
                    property string secondParent: String(secondParentId || "")
                    property string transactionTime: root.relativeTime(createdAtNs)
                    height: transactionCard.height + appTheme.spaceSm

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: appTheme.spaceMd
                        width: 1
                        color: root.colCardBorder
                    }

                    Rectangle {
                        id: transactionMarker
                        x: appTheme.spaceMd - width / 2
                        y: appTheme.spaceMd
                        width: transactionDelegate.mergeTransaction
                               ? appTheme.spaceMd
                               : appTheme.spaceSm
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
                        anchors.leftMargin: appTheme.spaceXl
                        anchors.right: parent.right
                        height: transactionDelegate.mergeTransaction
                                ? appTheme.spaceXl * 7
                                : (transactionDelegate.transactionField.length > 0
                                   ? appTheme.spaceXl * 5
                                   : appTheme.spaceXl * 4)
                        radius: appTheme.controlRadiusSmall
                        color: root.colCardSurface
                        property color selectionOutlineColor: transactionDelegate.currentTransaction
                                                              ? root.colText : root.colCardBorder
                        border.width: 1
                        border.color: selectionOutlineColor
                        clip: true

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: appTheme.spaceMd
                            spacing: appTheme.spaceXs

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceSm

                                Label {
                                    objectName: "editorHistoryCommitTitle"
                                    Layout.fillWidth: true
                                    text: transactionDelegate.mergeTransaction
                                          ? qsTr("Merge · second parent %1")
                                            .arg(transactionDelegate.secondParent.slice(0, 8))
                                          : (transactionDelegate.transactionLabel.length > 0
                                             ? transactionDelegate.transactionLabel
                                             : qsTr("Adjustment"))
                                    color: root.colText
                                    elide: Text.ElideRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeTitle
                                    font.weight: appTheme.fontWeightStrong
                                }

                                Label {
                                    Layout.maximumWidth: appTheme.spaceXl * 4
                                    text: transactionDelegate.transactionTime
                                    color: root.colMuted
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignRight
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: transactionDelegate.mergeTransaction
                                      ? qsTr("Resolved changes from the incoming branch.")
                                      : (transactionDelegate.transactionField.length > 0
                                         ? qsTr("Applied %1 adjustment.")
                                           .arg(transactionDelegate.transactionField)
                                         : qsTr("Committed editor changes."))
                                color: root.colMuted
                                wrapMode: Text.WordWrap
                                elide: Text.ElideRight
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.spaceXl * 3
                                visible: transactionDelegate.mergeTransaction
                                radius: appTheme.badgeRadius
                                color: appTheme.bgBaseColor
                                border.width: 1
                                border.color: root.colCardBorder

                                Column {
                                    anchors.fill: parent
                                    anchors.margins: appTheme.spaceSm
                                    spacing: appTheme.spaceXs

                                    Label {
                                        width: parent.width
                                        text: transactionDelegate.transactionField.length > 0
                                              ? qsTr("Changed field · %1")
                                                .arg(transactionDelegate.transactionField)
                                              : qsTr("Changed fields")
                                        color: root.colText
                                        elide: Text.ElideRight
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                    }

                                    Label {
                                        width: parent.width
                                        text: qsTr("Parent %1")
                                              .arg(transactionDelegate.secondParent.slice(0, 8))
                                        color: root.colMuted
                                        elide: Text.ElideRight
                                        font.family: appTheme.dataFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: transactionDelegate.transactionId.length > 0
                                      ? qsTr("Commit %1")
                                        .arg(transactionDelegate.transactionId.slice(0, 8))
                                      : ""
                                visible: text.length > 0
                                color: root.colMuted
                                elide: Text.ElideRight
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
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
