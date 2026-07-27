import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Named Version refs and their stable graph identities. This panel is kept
// independent from the transaction timeline so each rail destination owns its
// own layout and actions.
//
// Naming is an inline draft under the Versions header (Enter commits, Escape
// cancels, focus-loss commits only non-empty changed text). There is no modal
// naming dialog. Active selection is outline-only; removal uses a labeled
// trash action, never a stop-playback glyph.
//
// List scroll is restorable from the rail across Loader teardown (Phase 7A R6).
Item {
    id: root
    objectName: "editorVersionsPageBody"

    property var theme: null
    property var editorSession: null
    property var historyModel: null
    property bool versionCheckoutEnabled: true
    property string versionCheckoutDisabledReason: ""

    // Inline draft state (create branch / create fork / rename share one field).
    property bool draftVisible: false
    // "branchHead" | "forkRoot" | "rename" — selects the draft's submit action.
    property string draftMode: ""
    property string draftVersionId: ""
    property string draftOriginalText: ""
    property bool draftSubmitPending: false
    // Correlated operation id for the in-flight create/rename. Only the matching
    // terminal HistoryOperationFinished may close or fail this draft (R4).
    property var draftPendingOperationId: null
    property string draftError: ""
    property real _preservedContentY: 0
    property bool _restoringContentY: false
    // Active Version's working head commit; empty when the active Version sits at
    // the image root. "Branch from current" is disabled while this is empty.
    readonly property string activeVersionHeadCommit:
        root.historyModel ? String(root.historyModel.activeVersionHeadCommit || "") : ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property bool canMutateVersions: root.versionCheckoutEnabled && root.editorSession
                                              && root.editorSession.canEdit
                                              && !root.draftSubmitPending
    readonly property real listContentY: versionList ? versionList.contentY : 0

    // Freeze scroll capture BEFORE any model mutation. ListView modelReset can
    // jump contentY to 0 synchronously; if _restoringContentY is still false,
    // onContentYChanged would overwrite _preservedContentY with 0.
    function captureListScroll() {
        if (!versionList)
            return
        if (!root._restoringContentY)
            root._preservedContentY = versionList.contentY
        root._restoringContentY = true
    }

    function restoreListScroll() {
        if (!versionList)
            return
        root._restoringContentY = true
        Qt.callLater(function () {
            if (!versionList)
                return
            var maxY = Math.max(0, versionList.contentHeight - versionList.height)
            versionList.contentY = Math.max(0, Math.min(root._preservedContentY, maxY))
            root._restoringContentY = false
        })
    }

    // Rail-owned restore after Loader reactivation (R6).
    function restoreListContentY(y) {
        root._preservedContentY = Math.max(0, Number(y || 0))
        root.restoreListScroll()
    }

    function openCreateVersion(mode) {
        if (!root.historyModel || root.draftSubmitPending)
            return
        if (mode === "branchHead" && root.activeVersionHeadCommit.length === 0)
            return
        if (root.draftVisible && root.draftMode === mode) {
            draftFocusTimer.restart()
            return
        }
        root.draftMode = mode
        root.draftVersionId = ""
        root.draftError = ""
        root.draftPendingOperationId = null
        root.draftOriginalText = qsTr("Version %1").arg(root.historyModel.versions.count + 1)
        versionNameField.text = root.draftOriginalText
        root.draftVisible = true
        draftFocusTimer.restart()
    }

    function openRenameVersion(versionId, displayName) {
        if (root.draftSubmitPending)
            return
        root.draftMode = "rename"
        root.draftVersionId = versionId
        root.draftError = ""
        root.draftPendingOperationId = null
        root.draftOriginalText = displayName
        versionNameField.text = displayName
        root.draftVisible = true
        draftFocusTimer.restart()
    }

    function cancelDraft() {
        if (root.draftSubmitPending)
            return
        root.draftVisible = false
        root.draftMode = ""
        root.draftVersionId = ""
        root.draftOriginalText = ""
        root.draftPendingOperationId = null
        root.draftError = ""
        versionNameField.text = ""
    }

    function closeDraftAfterSuccess() {
        root.draftSubmitPending = false
        root.draftVisible = false
        root.draftMode = ""
        root.draftVersionId = ""
        root.draftOriginalText = ""
        root.draftPendingOperationId = null
        root.draftError = ""
        versionNameField.text = ""
        root.restoreListScroll()
    }

    function keepDraftAfterFailure(message) {
        root.draftSubmitPending = false
        root.draftPendingOperationId = null
        root.draftError = String(message || qsTr("Version operation failed"))
        root.restoreListScroll()
        draftFocusTimer.restart()
    }

    function commitDraft(requireChanged) {
        if (!root.draftVisible || !root.historyModel || root.draftSubmitPending)
            return
        var name = versionNameField.text.trim()
        if (name.length === 0) {
            cancelDraft()
            return
        }
        if (requireChanged && name === root.draftOriginalText.trim()) {
            cancelDraft()
            return
        }

        // Pending-save lock: keep the field, block duplicate submit.
        root.draftSubmitPending = true
        root.draftError = ""
        root.draftPendingOperationId = null
        root.captureListScroll()
        if (root.draftMode === "rename") {
            root.historyModel.renameVersion(root.draftVersionId, name)
        } else if (root.draftMode === "branchHead") {
            if (root.activeVersionHeadCommit.length > 0)
                root.historyModel.branchFromCommit(root.activeVersionHeadCommit, name)
            else
                root.keepDraftAfterFailure(qsTr("Current version has no commit to branch from"))
        } else {
            root.historyModel.createRootVersion(name)
        }
        root.finishDraftAfterSubmit()
    }

    function finishDraftAfterSubmit() {
        // Synchronous paths finish immediately. Async SaveStarted keeps the
        // field open with draftSubmitPending until HistoryOperationFinished.
        if (!root.editorSession) {
            root.closeDraftAfterSuccess()
            return
        }
        var result = root.editorSession.lastHistoryResult || {}
        var action = String(result.action || "")
        var kind = Number(result.kind !== undefined ? result.kind : -1)
        var isVersionAction = action === "createRootVersion" || action === "renameVersion"
                              || action === "branchFromCommit"
        var operationId = result.operationId
        // SaveStarted = 4 (EditorSessionResultKind::SaveStarted). Field stays
        // until a terminal HistoryOperationFinished for this operation id.
        if (isVersionAction && kind === 4) {
            root.draftPendingOperationId = operationId
            root.restoreListScroll()
            return
        }
        if (isVersionAction && (kind === 6 || kind === 7)) {
            // Failed / Rejected: keep the entered name and show the reason.
            root.keepDraftAfterFailure(result.message || root.editorSession.lastHistoryMessage)
            return
        }
        if (isVersionAction) {
            root.closeDraftAfterSuccess()
            return
        }
        // Unrelated history result: release the submit lock but keep the draft.
        root.draftSubmitPending = false
        root.restoreListScroll()
    }

    function checkoutVersionPreservingScroll(versionId) {
        if (!root.historyModel)
            return
        root.captureListScroll()
        root.historyModel.checkoutVersion(versionId)
        root.restoreListScroll()
    }

    function removeVersionPreservingScroll(versionId) {
        if (!root.historyModel)
            return
        root.captureListScroll()
        root.historyModel.removeVersion(versionId)
        root.restoreListScroll()
    }

    Timer {
        id: draftFocusTimer
        interval: 0
        onTriggered: {
            if (!root.draftVisible)
                return
            versionNameField.forceActiveFocus()
            versionNameField.selectAll()
        }
    }

    Connections {
        target: root.editorSession
        ignoreUnknownSignals: true
        function onHistoryOperationFinished() {
            if (!root.draftSubmitPending)
                return
            var result = root.editorSession.lastHistoryResult || {}
            var action = String(result.action || "")
            if (action !== "createRootVersion" && action !== "renameVersion"
                    && action !== "branchFromCommit")
                return
            // Stale completion for another draft must not close this one.
            if (root.draftPendingOperationId !== null
                    && result.operationId !== undefined
                    && result.operationId !== root.draftPendingOperationId)
                return
            var kind = Number(result.kind !== undefined ? result.kind : -1)
            // Keep waiting while a save checkpoint is still running.
            if (kind === 4)
                return
            if (kind === 6 || kind === 7) {
                root.keepDraftAfterFailure(result.message || root.editorSession.lastHistoryMessage)
                return
            }
            root.closeDraftAfterSuccess()
        }
    }

    Connections {
        target: root.historyModel && root.historyModel.versions
                ? root.historyModel.versions : null
        ignoreUnknownSignals: true
        function onModelAboutToBeReset() {
            root.captureListScroll()
        }
        function onModelReset() {
            root.restoreListScroll()
        }
        function onCountChanged() {
            if (root._restoringContentY)
                root.restoreListScroll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            objectName: "editorVersionsPanelHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "editorHistoryVersionsPanelTitle"
                    Layout.fillWidth: true
                    text: qsTr("Versions")
                    color: root.colText
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 named looks").arg(root.historyModel && root.historyModel.versions
                                                       ? root.historyModel.versions.count : 0)
                    color: root.colMuted
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            IconActionButton {
                objectName: "editorBranchFromCurrentButton"
                compact: true
                enabled: root.canMutateVersions && root.activeVersionHeadCommit.length > 0
                iconSrc: "qrc:/panel_icons/branch-current.svg"
                iconColorDefault: root.colText
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Branch from current commit")
                onClicked: root.openCreateVersion("branchHead")
            }

            IconActionButton {
                objectName: "editorForkFromRootButton"
                compact: true
                enabled: root.canMutateVersions
                iconSrc: "qrc:/panel_icons/fork-root.svg"
                iconColorDefault: root.colText
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Fork new version from root")
                onClicked: root.openCreateVersion("forkRoot")
            }
        }

        // Inline draft row under the Versions header (create + rename).
        // Height snaps; no opacity animation on this subtree (R6).
        Item {
            objectName: "editorVersionDraftRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.draftVisible
                                    ? (appTheme.spaceXl * 2 + appTheme.spaceSm
                                       + appTheme.spaceMd + appTheme.fontSizeCaption
                                       + (root.draftError.length > 0
                                          ? (appTheme.spaceXs + appTheme.fontSizeCaption * 2)
                                          : 0))
                                    : 0
            visible: root.draftVisible

            ColumnLayout {
                anchors.fill: parent
                spacing: appTheme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: root.draftMode === "rename" ? qsTr("Rename Version")
                          : root.draftMode === "branchHead" ? qsTr("Branch from current")
                          : qsTr("Fork from root")
                    color: root.colMuted
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    TextField {
                        id: versionNameField
                        objectName: "editorVersionNameField"
                        Layout.fillWidth: true
                        Layout.preferredHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                        enabled: root.draftVisible && !root.draftSubmitPending
                        color: root.colText
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        placeholderText: qsTr("Version name")
                        selectByMouse: true
                        leftPadding: appTheme.spaceSm
                        rightPadding: appTheme.spaceSm
                        selectionColor: appTheme.editorListSelectedFillColor
                        selectedTextColor: appTheme.editorListSelectedInkColor
                        Accessible.name: root.draftMode === "rename"
                                         ? qsTr("Rename Version")
                                         : (root.draftMode === "branchHead"
                                            ? qsTr("Branch from current")
                                            : qsTr("Fork from root"))
                        background: Rectangle {
                            implicitHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: versionNameField.activeFocus
                                          ? root.colText
                                          : root.colCardBorder
                            opacity: versionNameField.enabled ? 1.0 : 0.55
                        }

                        onAccepted: root.commitDraft(false)
                        Keys.onEscapePressed: function (event) {
                            root.cancelDraft()
                            event.accepted = true
                        }
                        onEditingFinished: {
                            Qt.callLater(function () {
                                Qt.callLater(function () {
                                    if (!root.draftVisible || root.draftSubmitPending)
                                        return
                                    root.commitDraft(true)
                                })
                            })
                        }
                    }

                    IconActionButton {
                        objectName: "editorVersionAcceptButton"
                        compact: true
                        enabled: root.draftVisible && !root.draftSubmitPending
                                 && versionNameField.text.trim().length > 0
                        iconSrc: "qrc:/panel_icons/plus.svg"
                        iconColorDefault: root.colText
                        iconColorMuted: root.colMuted
                        fillIdle: root.colCardSurface
                        fillHover: appTheme.buttonHoveredFillColor
                        fillPressed: appTheme.buttonPressedFillColor
                        fillSelected: appTheme.buttonSelectedFillColor
                        focusRingColor: root.colText
                        actionName: root.draftMode === "rename"
                                    ? qsTr("Accept Rename")
                                    : (root.draftMode === "branchHead"
                                       ? qsTr("Accept branch from current")
                                       : qsTr("Accept fork from root"))
                        onClicked: root.commitDraft(false)
                    }
                }

                Label {
                    objectName: "editorVersionDraftError"
                    Layout.fillWidth: true
                    visible: root.draftError.length > 0
                    text: root.draftError
                    color: appTheme.dangerColor
                    wrapMode: Text.WordWrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: root.versionCheckoutEnabled
                      ? qsTr("Named looks")
                      : (root.versionCheckoutDisabledReason.length > 0
                         ? root.versionCheckoutDisabledReason
                         : qsTr("Version checkout is unavailable"))
                color: root.colMuted
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        Item {
            objectName: "editorVersionsListWell"
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
                id: versionList
                objectName: "editorVersionsList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs
                clip: true
                spacing: appTheme.spaceSm
                model: root.historyModel ? root.historyModel.versions : null
                reuseItems: true
                // Avoid re-anchoring the viewport to a selected index on data-only
                // updates; outline selection never requires positionViewAtIndex.
                boundsBehavior: Flickable.StopAtBounds

                onContentYChanged: {
                    if (!root._restoringContentY)
                        root._preservedContentY = contentY
                }

                delegate: Rectangle {
                    id: versionCard
                    objectName: "editorVersionCard"

                    required property string versionId
                    required property string displayName
                    required property string headCommitHash
                    required property bool active

                    property string versionHead: headCommitHash
                    property bool versionActive: active
                    // Outline-only active chrome: card surface stays cardSurface;
                    // the 1 px border is the selection signal (white/text color).
                    property color selectionOutlineColor: versionActive
                                                          ? root.colText : root.colCardBorder
                    width: ListView.view ? ListView.view.width : 0
                    height: versionActive ? appTheme.spaceXl * 5 : appTheme.spaceXl * 4
                    radius: appTheme.controlRadiusSmall
                    color: root.colCardSurface
                    border.width: 1
                    border.color: selectionOutlineColor
                    Accessible.role: Accessible.ListItem
                    Accessible.name: displayName
                    Accessible.description: versionActive
                                            ? qsTr("Current head Version")
                                            : qsTr("Named Version")

                    ListView.onPooled: {}
                    ListView.onReused: {}

                    MouseArea {
                        anchors.fill: parent
                        z: 0
                        enabled: root.versionCheckoutEnabled && !versionActive
                                 && !root.draftSubmitPending
                        onClicked: root.checkoutVersionPreservingScroll(versionId)
                    }

                    ColumnLayout {
                        anchors.left: parent.left
                        anchors.right: actionRow.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: appTheme.spaceMd
                        anchors.rightMargin: appTheme.spaceXs
                        spacing: appTheme.spaceXs

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm

                            Label {
                                objectName: "editorVersionTitle"
                                Layout.fillWidth: true
                                text: displayName
                                color: root.colText
                                elide: Text.ElideRight
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeTitle
                                font.weight: appTheme.fontWeightStrong
                            }

                            Rectangle {
                                visible: versionActive
                                implicitWidth: currentBadgeLabel.implicitWidth + appTheme.spaceSm
                                implicitHeight: currentBadgeLabel.implicitHeight + appTheme.spaceXs
                                radius: appTheme.badgeRadius
                                color: "transparent"
                                border.width: 1
                                border.color: root.colText

                                Label {
                                    id: currentBadgeLabel
                                    anchors.centerIn: parent
                                    text: qsTr("CURRENT HEAD")
                                    color: root.colText
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            objectName: "editorVersionSubtitle"
                            text: versionActive
                                  ? qsTr("Checked out")
                                  : qsTr("Head %1").arg(versionHead.length > 0
                                                         ? versionHead.slice(0, 8)
                                                         : qsTr("image root"))
                            color: root.colMuted
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: versionHead.length > 0
                            text: qsTr("Commit %1").arg(versionHead.slice(0, 8))
                            color: root.colMuted
                            elide: Text.ElideRight
                            font.family: appTheme.dataFontFamily
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
                            enabled: root.canMutateVersions
                            iconSrc: "qrc:/panel_icons/edit.svg"
                            iconColorDefault: root.colText
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: appTheme.buttonHoveredFillColor
                            fillPressed: appTheme.buttonPressedFillColor
                            fillSelected: appTheme.buttonSelectedFillColor
                            focusRingColor: root.colText
                            actionName: qsTr("Rename Version")
                            onClicked: root.openRenameVersion(versionId, displayName)
                        }

                        IconActionButton {
                            objectName: "editorRemoveVersionButton"
                            compact: true
                            enabled: root.versionCheckoutEnabled && !versionActive
                                     && root.historyModel && root.historyModel.versions.count > 1
                                     && root.editorSession && root.editorSession.canEdit
                                     && !root.draftSubmitPending
                            iconSrc: "qrc:/panel_icons/trash.svg"
                            iconColorDefault: root.colText
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: appTheme.buttonHoveredFillColor
                            fillPressed: appTheme.buttonPressedFillColor
                            fillSelected: appTheme.buttonSelectedFillColor
                            focusRingColor: root.colText
                            actionName: qsTr("Remove Version")
                            onClicked: root.removeVersionPreservingScroll(versionId)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - appTheme.spaceMd
                    visible: root.historyModel && root.historyModel.versions
                              && root.historyModel.versions.count === 0
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
