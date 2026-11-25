import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0
import "FileDrop.js" as FileDrop

// Named Version refs and their stable graph identities. This panel is kept
// independent from the transaction timeline so each rail destination owns its
// own layout and actions.
//
// Naming is inline (Enter commits, Escape cancels, focus-loss commits only
// non-empty changed text). Create / branch use a draft row under the Versions
// header; rename edits the card title in place. There is no modal naming
// dialog. Active selection is outline-only. A right click on a card opens its menu:
// Rename, Export Version (a preset file at a path from the system save dialog), and
// Remove.
//
// List scroll is restorable from the rail across Loader teardown (Phase 7A R6).
//
// Presets (edit presets plan 2.1 and 2.2): the preset part sits above the Versions
// part, with a splitter whose height PresetController stores. A drag of a Version
// row onto the preset part saves a preset; a drag of a preset tile onto the Versions
// list applies it as a new Version. Escape cancels the drag; a drop outside the
// accepting part does nothing. A .alcpreset file dropped on the Versions list is
// applied one time. A drop guide (DropGuideOverlay) shows where a drop goes: on the
// Versions list while a preset is selected or dragged, on the preset part while a
// Version is dragged.
Item {
    id: root
    objectName: "editorVersionsPageBody"

    property var theme: null
    property var editorSession: null
    property var historyModel: null
    // AdjustmentTransferController: pastes the copied adjustments as a new Version.
    property var adjustmentTransfer: null
    // PresetController: the preset part and its commands.
    property var presets: typeof appModules !== "undefined" && appModules
                          ? appModules.presets : null
    // PresetPreviewController: the preset tile previews of the open photo.
    property var presetPreview: typeof appModules !== "undefined" && appModules
                                ? appModules.presetPreview : null
    property bool versionCheckoutEnabled: true
    property string versionCheckoutDisabledReason: ""
    // EditorComparisonController; the header Compare action opens Version comparison.
    property var comparison: typeof appModules !== "undefined" && appModules
                             ? appModules.editorComparison : null

    // Inline draft state shared by create branch / create fork / rename. The
    // header field edits create drafts; the target card's title field edits a
    // rename draft. draftText is the text of whichever field is active.
    property bool draftVisible: false
    // "branchHead" | "forkRoot" | "rename" — selects the draft's submit action.
    property string draftMode: ""
    property string draftVersionId: ""
    property string draftOriginalText: ""
    property string draftText: ""
    // Bumped to ask the inline rename field on the target card to take focus.
    property int draftFocusRequest: 0
    readonly property bool headerDraftVisible: root.draftVisible && root.draftMode !== "rename"
    // Name accepted on the last submit; used to detect model-projected success
    // when a terminal HistoryOperationFinished is delayed or missed.
    property string draftSubmittedName: ""
    property bool draftSubmitPending: false
    // Correlated operation id for the in-flight create/rename. Only the matching
    // terminal HistoryOperationFinished may close or fail this draft (R4).
    property var draftPendingOperationId: null
    property string draftError: ""
    // Rejection text of the last paste-as-Version request; empty after success.
    property string pasteError: ""
    property real _preservedContentY: 0
    property bool _restoringContentY: false
    // Active Version's working head commit; empty when the active Version sits at
    // the image root. "Branch from current" is disabled while this is empty.
    readonly property string activeVersionHeadCommit:
        root.historyModel ? String(root.historyModel.activeVersionHeadCommit || "") : ""
    readonly property string activeVersionDisplayName:
        root.historyModel ? String(root.historyModel.activeVersionDisplayName || "") : ""
    readonly property string activeVersionId:
        root.historyModel ? String(root.historyModel.activeVersionId || "") : ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property bool canMutateVersions: root.versionCheckoutEnabled && root.editorSession
                                              && root.editorSession.actions.canCheckoutVersion
                                              && !root.draftSubmitPending
    // Same condition as editorForkFromRootButton: the Ctrl+A command shares the
    // fork-from-root action's availability so a disabled action never consumes
    // the key.
    readonly property bool canCreateDefaultRootVersion:
        root.canMutateVersions && root.editorSession
        && root.editorSession.actions.canCreateRootVersion
    readonly property real listContentY: versionList ? versionList.contentY : 0

    // Presets: an apply makes a Version of the open photo; a save reads one of its Versions.
    readonly property bool canApplyPresets: !!root.presets && root.versionCheckoutEnabled
                                            && !!root.editorSession && root.editorSession.hasImage
    readonly property bool canSavePresets: !!root.presets && !!root.editorSession
                                           && root.editorSession.hasImage
    readonly property real presetsMinHeight: presetsSection.headerHeight
    readonly property real presetsMaxHeight:
        Math.max(root.presetsMinHeight, root.height - appTheme.spaceLg * 2 - splitter.height
                                        - appTheme.editorVersionsPartMinHeight)
    readonly property real presetsHeight: {
        const stored = root.presets ? Number(root.presets.browserHeight || 0) : 0
        const wanted = stored > 0 ? stored : appTheme.editorPresetPaneHeight
        return Math.max(root.presetsMinHeight, Math.min(root.presetsMaxHeight, wanted))
    }

    // Drag between the two parts: "version" (save a preset) or "preset" (apply it).
    property string dragKind: ""
    property string dragId: ""
    property string dragName: ""
    property point dragPoint: Qt.point(0, 0)
    readonly property bool dragOverPresets: root.dragKind === "version"
                                            && root.pointInside(presetsSection, root.dragPoint)
    readonly property bool dragOverVersions: root.dragKind === "preset"
                                             && root.pointInside(versionsListWell, root.dragPoint)

    // Drop guide of the Versions list: a selected preset shows its band until an apply
    // of that preset succeeds; a preset drag shows it until the drop. A Version drag
    // shows the band of the preset part instead.
    readonly property string selectedPresetId: presetsSection.browser.selectedPresetId
    property string dropGuideDismissedPresetId: ""
    readonly property bool versionsDropGuideShown:
        root.canApplyPresets
        && (root.dragKind === "preset"
            || (root.dragKind.length === 0 && root.selectedPresetId.length > 0
                && root.selectedPresetId !== root.dropGuideDismissedPresetId))
    onSelectedPresetIdChanged: root.dropGuideDismissedPresetId = ""

    function pointInside(item, point) {
        const local = item.mapFromItem(root, point.x, point.y)
        return local.x >= 0 && local.y >= 0 && local.x < item.width && local.y < item.height
    }

    function beginDrag(kind, id, name, sceneX, sceneY) {
        root.dragKind = kind
        root.dragId = id
        root.dragName = name
        root.forceActiveFocus()
        root.moveDrag(sceneX, sceneY)
    }

    function moveDrag(sceneX, sceneY) {
        if (root.dragKind.length > 0)
            root.dragPoint = root.mapFromItem(null, sceneX, sceneY)
    }

    function finishDrag(sceneX, sceneY) {
        if (root.dragKind.length === 0)
            return
        root.moveDrag(sceneX, sceneY)
        const kind = root.dragKind
        const id = root.dragId
        const savePreset = root.dragOverPresets
        const applyPreset = root.dragOverVersions
        root.cancelDrag()
        if (kind === "version" && savePreset)
            root.saveVersionAsPreset(id)
        else if (kind === "preset" && applyPreset)
            root.applyPreset(id)
    }

    function cancelDrag() {
        root.dragKind = ""
        root.dragId = ""
        root.dragName = ""
    }

    function saveVersionAsPreset(versionId) {
        if (root.canSavePresets)
            presetsSection.reportSave(root.presets.SaveVersionAsPreset(versionId))
    }

    function applyPreset(presetId) {
        if (!root.canApplyPresets)
            return
        root.captureListScroll()
        const result = root.presets.ApplyToEditor(presetId)
        presetsSection.report(result)
        if (result && result.success === true)
            root.dropGuideDismissedPresetId = presetId
        root.restoreListScroll()
    }

    function applyPresetFile(url) {
        if (!root.canApplyPresets)
            return
        root.captureListScroll()
        presetsSection.report(root.presets.ApplyFileToEditor(url))
        root.restoreListScroll()
    }

    focus: true

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape && root.dragKind.length > 0) {
            root.cancelDrag()
            event.accepted = true
            return
        }
        if (ShortcutRegistry.commandIdForKey("editor.versions", event.key,
                                             event.modifiers)
                === "versions.createDefaultFromRoot"
                && root.canCreateDefaultRootVersion) {
            root.createDefaultRootVersion()
            event.accepted = true
        }
    }

    // Freeze scroll capture BEFORE any model mutation. ListView modelReset can
    // jump contentY to 0 synchronously; if _restoringContentY is still false,
    // onContentYChanged would overwrite _preservedContentY with 0.
    function pasteAdjustmentsAsVersion() {
        if (!root.adjustmentTransfer || !root.editorSession)
            return
        const result = root.adjustmentTransfer.PasteIntoEditor(root.editorSession)
        root.pasteError = result && result.success !== true
                          ? String(result.message || qsTr("Paste failed"))
                          : ""
    }

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

    function setDraftText(text) {
        root.draftText = text
        if (root.draftMode !== "rename")
            versionNameField.text = text
    }

    function clearDraftFields() {
        root.draftMode = ""
        root.draftVersionId = ""
        root.draftOriginalText = ""
        root.draftSubmittedName = ""
        root.draftPendingOperationId = null
        root.draftError = ""
        root.draftText = ""
        versionNameField.text = ""
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
        root.draftSubmittedName = ""
        root.draftPendingOperationId = null
        root.draftOriginalText = root.historyModel.defaultVersionName(qsTr("Version %1"))
        root.setDraftText(root.draftOriginalText)
        root.draftVisible = true
        draftFocusTimer.restart()
    }

    // Rename edits the card title in place; the header draft row stays closed.
    function openRenameVersion(versionId, displayName) {
        if (root.draftSubmitPending)
            return
        root.draftMode = "rename"
        root.draftVersionId = versionId
        root.draftError = ""
        root.draftSubmittedName = ""
        root.draftPendingOperationId = null
        root.draftOriginalText = displayName
        versionNameField.text = ""
        root.setDraftText(displayName)
        root.draftVisible = true
        draftFocusTimer.restart()
    }

    function cancelDraft() {
        if (root.draftSubmitPending)
            return
        root.draftVisible = false
        root.draftSubmitPending = false
        root.clearDraftFields()
    }

    function closeDraftAfterSuccess() {
        root.draftSubmitPending = false
        root.draftVisible = false
        root.clearDraftFields()
        root.restoreListScroll()
    }

    function keepDraftAfterFailure(message) {
        root.draftSubmitPending = false
        root.draftPendingOperationId = null
        root.draftError = String(message || qsTr("Version operation failed"))
        // Re-show the naming row if SaveStarted already hid it.
        root.draftVisible = true
        if (root.draftSubmittedName.length > 0)
            root.setDraftText(root.draftSubmittedName)
        root.restoreListScroll()
        draftFocusTimer.restart()
    }

    function isVersionDraftAction(action) {
        return action === "createRootVersion" || action === "renameVersion"
               || action === "branchFromCommit"
    }

    // True when the projected history already shows the submitted create/branch
    // checkout or active rename. Used when terminal HistoryOperationFinished is
    // delayed/missed so the naming row and mutation locks cannot stick open.
    function historyModelReflectsDraftSuccess() {
        if (!root.historyModel || root.draftSubmittedName.length === 0)
            return false
        var activeName = root.activeVersionDisplayName.trim()
        if (activeName !== root.draftSubmittedName)
            return false
        if (root.draftMode === "rename") {
            if (root.draftVersionId.length === 0)
                return true
            return root.activeVersionId === root.draftVersionId
        }
        // forkRoot / branchHead always check out the new Version on success.
        return true
    }

    // Apply the latest correlated history result to the inline draft. Create and
    // branch already checkout the new Version on the backend before the terminal
    // event is published; closing the draft returns the list to normal control.
    function applyHistoryResultToDraft() {
        if (!root.draftSubmitPending || !root.editorSession)
            return false
        var result = root.editorSession.lastHistoryResult || {}
        var action = String(result.action || "")
        if (!root.isVersionDraftAction(action))
            return false
        // String compare: QVariantMap may surface operationId as double/string.
        if (root.draftPendingOperationId !== null
                && result.operationId !== undefined
                && String(result.operationId) !== String(root.draftPendingOperationId))
            return false
        var kind = Number(result.kind !== undefined ? result.kind : -1)
        var terminal = result.terminal === true
        // SaveStarted = 4. Hide the naming row while the checkpoint runs, but
        // keep draftSubmitPending until a terminal event or model success so
        // duplicate submits stay blocked. Version checkout stays blocked by the
        // save-checkpoint lease until the backend releases it.
        if (!terminal && kind === 4) {
            if (result.operationId !== undefined)
                root.draftPendingOperationId = result.operationId
            root.draftVisible = false
            root.restoreListScroll()
            return true
        }
        if (kind === 6 || kind === 7) {
            root.keepDraftAfterFailure(result.message || root.editorSession.lastHistoryMessage)
            return true
        }
        // Terminal success (Accepted / RenderRouted / SaveFinished / …).
        root.closeDraftAfterSuccess()
        return true
    }

    // Terminal result first; if still pending, accept a model projection that
    // already shows the submitted Version (create/branch/active rename).
    function tryClosePendingDraft() {
        if (!root.draftSubmitPending)
            return
        root.applyHistoryResultToDraft()
        if (root.draftSubmitPending && root.historyModelReflectsDraftSuccess())
            root.closeDraftAfterSuccess()
    }

    function commitDraft(requireChanged) {
        if (!root.draftVisible || !root.historyModel || root.draftSubmitPending)
            return
        var name = root.historyModel.normalizeVersionName(root.draftText)
        if (name.length === 0) {
            cancelDraft()
            return
        }
        if (requireChanged
                && root.historyModel.isUnchangedVersionName(root.draftOriginalText, name)) {
            cancelDraft()
            return
        }

        // Pending-save lock: block duplicate submit until terminal/model success.
        root.draftSubmitPending = true
        root.draftSubmittedName = name
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
        // Synchronous paths finish immediately. Async SaveStarted hides the
        // naming row but keeps draftSubmitPending until terminal/model success.
        if (!root.editorSession) {
            root.closeDraftAfterSuccess()
            return
        }
        if (root.applyHistoryResultToDraft()) {
            // Model may already project the new Version on the same stack.
            if (root.draftSubmitPending && root.historyModelReflectsDraftSuccess())
                root.closeDraftAfterSuccess()
            return
        }
        // Unrelated history result: release the submit lock but keep the draft.
        root.draftSubmitPending = false
        root.draftSubmittedName = ""
        root.restoreListScroll()
    }

    // Direct create with the same default-name calculation as the inline
    // forkRoot draft; skips the naming row. The caller checks
    // canCreateDefaultRootVersion.
    function createDefaultRootVersion() {
        if (!root.historyModel)
            return
        root.captureListScroll()
        root.historyModel.createRootVersion(
            root.historyModel.defaultVersionName(qsTr("Version %1")))
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

    // Export Version: the system save dialog, then the Version as a preset file (#1124).
    // A failure shows in the error badge of the preset part.
    function exportVersion(versionId) {
        if (root.canSavePresets)
            presetsSection.report(root.presets.ExportVersion(versionId))
    }

    function openVersionMenu(versionId, displayName, active, sceneX, sceneY) {
        versionMenu.versionId = versionId
        versionMenu.displayName = displayName
        versionMenu.versionActive = active
        versionMenu.parent = Overlay.overlay
        versionMenu.openAt(sceneX, sceneY)
    }

    Timer {
        id: draftFocusTimer
        interval: 0
        onTriggered: {
            if (!root.draftVisible)
                return
            if (root.draftMode === "rename") {
                root.draftFocusRequest += 1
                return
            }
            versionNameField.forceActiveFocus()
            versionNameField.selectAll()
        }
    }

    // Focus-loss commit, deferred so a click on another control runs first.
    // @p draftKey pins the draft that lost focus: if the user already opened a
    // different draft (e.g. Rename on another card), this one must not commit
    // or cancel the new draft.
    function commitDraftAfterFocusLoss(draftKey) {
        Qt.callLater(function () {
            Qt.callLater(function () {
                if (!root.draftVisible || root.draftSubmitPending)
                    return
                if (root.draftMode + ":" + root.draftVersionId !== draftKey)
                    return
                root.commitDraft(true)
            })
        })
    }

    // The C++ signals HistoryOperationFinished and StateChanged start with an
    // upper-case letter, so QML handlers for them do not run. These handlers use
    // properties that the signals notify.
    Connections {
        target: root.editorSession
        ignoreUnknownSignals: true
        // lastHistoryResult NOTIFY HistoryOperationFinished: the correlated result
        // of a Version operation, including the terminal result of an async save.
        function onLastHistoryResultChanged() {
            root.tryClosePendingDraft()
        }
    }

    // activeVersionId NOTIFY StateChanged, so this handler runs on every
    // EditorHistoryModel StateChanged. The model emits it after the list
    // projection applies: a missed terminal result still closes the draft when
    // the model shows the submitted Version.
    Connections {
        target: root.historyModel
        ignoreUnknownSignals: true
        function onActiveVersionIdChanged() {
            root.tryClosePendingDraft()
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

    EditorPresetsSection {
        id: presetsSection
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: appTheme.spaceLg
        anchors.rightMargin: appTheme.spaceLg
        anchors.topMargin: appTheme.spaceLg
        height: root.presetsHeight
        clip: true
        presets: root.presets
        presetPreview: root.presetPreview
        canApply: root.canApplyPresets
        versionDropHighlight: root.dragOverPresets
        versionDragActive: root.dragKind === "version" && root.canSavePresets
        versionDragName: root.dragKind === "version" ? root.dragName : ""
        onApplyRequested: function (presetId) { root.applyPreset(presetId) }
        onApplyFileRequested: function (url) { root.applyPresetFile(url) }
        onPresetDragStarted: function (presetId, name, sceneX, sceneY) {
            if (root.canApplyPresets)
                root.beginDrag("preset", presetId, name, sceneX, sceneY)
        }
        onPresetDragMoved: function (sceneX, sceneY) { root.moveDrag(sceneX, sceneY) }
        onPresetDragFinished: function (sceneX, sceneY) { root.finishDrag(sceneX, sceneY) }
    }

    // Splitter between the two parts. A drag sets the preset part height (stored by
    // PresetController); the preset part can shrink to its header. Double click
    // restores the default height.
    Item {
        id: splitter
        objectName: "editorPresetsSplitter"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: presetsSection.bottom
        height: appTheme.spaceMd
        property real startHeight: 0

        Rectangle {
            anchors.centerIn: parent
            width: parent.width - appTheme.spaceLg * 2
            height: 1
            color: splitterHover.hovered || splitterDrag.active ? appTheme.textMutedColor
                                                                : appTheme.cardBorderColor
        }

        HoverHandler {
            id: splitterHover
            cursorShape: Qt.SizeVerCursor
        }

        DragHandler {
            id: splitterDrag
            target: null
            xAxis.enabled: false
            cursorShape: Qt.SizeVerCursor
            onActiveChanged: {
                if (active)
                    splitter.startHeight = root.presetsHeight
            }
            onTranslationChanged: {
                if (active && root.presets)
                    root.presets.browserHeight = Math.max(
                        root.presetsMinHeight,
                        Math.min(root.presetsMaxHeight, splitter.startHeight + translation.y))
            }
        }

        TapHandler {
            onDoubleTapped: {
                if (root.presets)
                    root.presets.browserHeight = appTheme.editorPresetPaneHeight
            }
        }
    }

    // Right click on a Version card. Rename and Export start when the menu has closed: on
    // close the menu gives the focus back to the item that had it before, which would end an
    // inline rename at once, and the save dialog must not open under a closing menu.
    AppContextMenu {
        id: versionMenu
        objectName: "editorVersionMenu"
        property string versionId: ""
        property string displayName: ""
        property bool versionActive: false
        property string actionAfterClose: ""
        onClosed: {
            const action = actionAfterClose
            actionAfterClose = ""
            if (action === "rename")
                root.openRenameVersion(versionId, displayName)
            else if (action === "export")
                root.exportVersion(versionId)
        }
        AppMenuItem {
            objectName: "editorVersionMenuRenameItem"
            text: qsTr("Rename Version")
            enabled: root.canMutateVersions && root.editorSession.actions.canRenameVersion
            onTriggered: versionMenu.actionAfterClose = "rename"
        }
        AppMenuItem {
            objectName: "editorVersionMenuExportItem"
            text: qsTr("Export Version...")
            enabled: root.canSavePresets
            onTriggered: versionMenu.actionAfterClose = "export"
        }
        AppMenuItem {
            objectName: "editorVersionMenuRemoveItem"
            text: qsTr("Remove Version")
            enabled: root.versionCheckoutEnabled && !versionMenu.versionActive
                     && !!root.historyModel && root.historyModel.versions.count > 1
                     && !!root.editorSession && root.editorSession.actions.canRemoveVersion
            onTriggered: root.removeVersionPreservingScroll(versionMenu.versionId)
        }
    }

    // The name of the dragged Version or preset follows the pointer.
    Rectangle {
        objectName: "editorPresetsDragLabel"
        visible: root.dragKind.length > 0
        z: 100
        x: root.dragPoint.x + appTheme.spaceSm
        y: root.dragPoint.y + appTheme.spaceSm
        width: dragLabel.implicitWidth + appTheme.spaceMd * 2
        height: dragLabel.implicitHeight + appTheme.spaceXs * 2
        radius: appTheme.controlRadiusSmall
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: root.dragOverPresets || root.dragOverVersions ? appTheme.textColor
                                                                    : appTheme.cardBorderColor

        Label {
            id: dragLabel
            anchors.centerIn: parent
            textFormat: Text.PlainText
            text: root.dragName
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: splitter.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: appTheme.spaceLg
        anchors.rightMargin: appTheme.spaceLg
        anchors.bottomMargin: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            objectName: "editorVersionsPanelHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                objectName: "editorHistoryVersionsPanelTitle"
                Layout.fillWidth: true
                text: qsTr("Versions")
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
            }

            DialogActionButton {
                objectName: "editorVersionsCompareButton"
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonWidth: contentItem.implicitWidth + 2 * appTheme.spaceMd
                font.pixelSize: appTheme.fontSizeBody
                text: qsTr("Compare")
                enabled: !!(root.comparison && root.comparison.canOpen)
                Accessible.name: qsTr("Compare Versions")
                Accessible.description: root.comparison
                                        ? String(root.comparison.openUnavailableReason || "") : ""
                onClicked: root.comparison.openVersions()
            }

            IconActionButton {
                objectName: "editorBranchFromCurrentButton"
                compact: true
                enabled: root.canMutateVersions && root.activeVersionHeadCommit.length > 0
                         && root.editorSession.actions.canBranchVersion
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
                enabled: root.canMutateVersions && root.editorSession.actions.canCreateRootVersion
                iconSrc: "qrc:/panel_icons/fork-root.svg"
                iconColorDefault: root.colText
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Fork new version from root")
                toolTipText: ShortcutRegistry.decorateTooltip(
                    qsTr("Fork new version from root"), "versions.createDefaultFromRoot")
                onClicked: root.openCreateVersion("forkRoot")
            }

            IconActionButton {
                objectName: "editorVersionsPasteButton"
                compact: true
                enabled: root.adjustmentTransfer !== null && root.editorSession
                         && root.editorSession.actions.canPaste === true
                iconSrc: "qrc:/panel_icons/clipboard.svg"
                iconColorDefault: root.colText
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Paste adjustments as a new Version")
                onClicked: root.pasteAdjustmentsAsVersion()
            }
        }

        Label {
            objectName: "editorVersionsPasteError"
            Layout.fillWidth: true
            visible: root.pasteError.length > 0
            text: root.pasteError
            color: appTheme.dangerColor
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        // Inline create draft row under the Versions header (branch / fork).
        // Rename edits the card title instead. Height snaps; no opacity
        // animation on this subtree (R6).
        Item {
            objectName: "editorVersionDraftRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.headerDraftVisible
                                    ? (appTheme.spaceXl * 2 + appTheme.spaceSm
                                       + appTheme.spaceMd + appTheme.fontSizeCaption
                                       + (root.draftError.length > 0
                                          ? (appTheme.spaceXs + appTheme.fontSizeCaption * 2)
                                          : 0))
                                    : 0
            visible: root.headerDraftVisible

            ColumnLayout {
                anchors.fill: parent
                spacing: appTheme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: root.draftMode === "branchHead" ? qsTr("Branch from current")
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
                        enabled: root.headerDraftVisible && !root.draftSubmitPending
                        color: root.colText
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        placeholderText: qsTr("Version name")
                        selectByMouse: true
                        leftPadding: appTheme.spaceSm
                        rightPadding: appTheme.spaceSm
                        selectionColor: appTheme.editorListSelectedFillColor
                        selectedTextColor: appTheme.editorListSelectedInkColor
                        Accessible.name: root.draftMode === "branchHead"
                                         ? qsTr("Branch from current")
                                         : qsTr("Fork from root")
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

                        onTextChanged: {
                            if (root.headerDraftVisible)
                                root.draftText = text
                        }
                        onAccepted: root.commitDraft(false)
                        Keys.onEscapePressed: function (event) {
                            root.cancelDraft()
                            event.accepted = true
                        }
                        onEditingFinished: {
                            if (root.headerDraftVisible)
                                root.commitDraftAfterFocusLoss(root.draftMode + ":")
                        }
                    }

                    IconActionButton {
                        objectName: "editorVersionAcceptButton"
                        compact: true
                        // Keep focus in the name field: taking it would end
                        // editing, and focus-loss cancels an unchanged default
                        // name before this click lands.
                        focusOnPointerPress: false
                        enabled: root.headerDraftVisible && !root.draftSubmitPending
                                 && versionNameField.text.trim().length > 0
                        iconSrc: "qrc:/panel_icons/plus.svg"
                        iconColorDefault: root.colText
                        iconColorMuted: root.colMuted
                        fillIdle: root.colCardSurface
                        fillHover: appTheme.buttonHoveredFillColor
                        fillPressed: appTheme.buttonPressedFillColor
                        fillSelected: appTheme.buttonSelectedFillColor
                        focusRingColor: root.colText
                        actionName: root.draftMode === "branchHead"
                                    ? qsTr("Accept branch from current")
                                    : qsTr("Accept fork from root")
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

        // Checkout-disabled reason only (no "Named looks" chrome when enabled).
        Label {
            Layout.fillWidth: true
            visible: !root.versionCheckoutEnabled
            text: root.versionCheckoutDisabledReason.length > 0
                  ? root.versionCheckoutDisabledReason
                  : qsTr("Version checkout is unavailable")
            color: root.colMuted
            wrapMode: Text.Wrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        Item {
            id: versionsListWell
            objectName: "editorVersionsListWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.dragOverVersions || presetFileDrop.url.length > 0
                              ? root.colText : root.colCardBorder
            }

            // One .alcpreset file from the system: apply it one time (plan 2.2, R8).
            DropArea {
                id: presetFileDrop
                objectName: "editorVersionsPresetFileDropArea"
                anchors.fill: parent
                keys: ["text/uri-list"]
                property string url: ""
                function presetFileUrl(dragEvent) {
                    const urls = FileDrop.localFileUrls(dragEvent)
                    return urls.length === 1 && urls[0].toLowerCase().endsWith(".alcpreset")
                           ? urls[0] : ""
                }
                onEntered: function (drag) {
                    url = root.canApplyPresets ? presetFileUrl(drag) : ""
                    if (url.length > 0)
                        drag.accept(Qt.CopyAction)
                    else
                        drag.accepted = false
                }
                onExited: url = ""
                onDropped: function (drop) {
                    const dropped = url.length > 0 ? url : presetFileUrl(drop)
                    url = ""
                    if (dropped.length === 0 || !root.canApplyPresets) {
                        drop.accepted = false
                        return
                    }
                    drop.accept(Qt.CopyAction)
                    root.applyPresetFile(dropped)
                }
            }

            // A selected preset or a preset drag: the band says that a drop here applies the
            // preset. It sits above the list (z) and takes no input.
            DropGuideOverlay {
                objectName: "editorVersionsDropGuide"
                anchors.fill: parent
                shown: root.versionsDropGuideShown
                expanded: root.dragOverVersions || presetFileDrop.url.length > 0
                bandText: qsTr("Drag here to apply this preset")
                expandedText: qsTr("Release to apply this preset")
                detailText: root.dragKind === "preset" ? root.dragName : ""
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
                    readonly property bool renaming: root.draftVisible
                                                     && root.draftMode === "rename"
                                                     && root.draftVersionId === versionId
                    // Outline-only active chrome: card surface stays cardSurface;
                    // the 1 px border is the selection signal (white/text color).
                    // No detached HEAD — the outline alone marks the checked-out
                    // Version; no "CURRENT HEAD" pill or "Checked out" copy.
                    property color selectionOutlineColor: versionActive
                                                          ? root.colText : root.colCardBorder
                    width: ListView.view ? ListView.view.width : 0
                    height: Math.max(appTheme.spaceXl * 4,
                                     versionCardColumn.implicitHeight + appTheme.spaceMd)
                    radius: appTheme.controlRadiusSmall
                    color: root.colCardSurface
                    border.width: 1
                    border.color: selectionOutlineColor
                    Accessible.role: Accessible.ListItem
                    Accessible.name: displayName
                    Accessible.description: versionActive
                                            ? qsTr("Active Version")
                                            : qsTr("Named Version")

                    ListView.onPooled: {}
                    ListView.onReused: {}

                    MouseArea {
                        objectName: "editorVersionCardMouseArea"
                        anchors.fill: parent
                        z: 0
                        enabled: !versionCard.renaming
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        // A drag of the row saves a preset; the list must not flick instead.
                        preventStealing: true
                        property point pressPoint: Qt.point(0, 0)
                        property bool dragged: false
                        onPressed: function (mouse) {
                            pressPoint = Qt.point(mouse.x, mouse.y)
                            dragged = false
                            if (mouse.button === Qt.RightButton) {
                                const scene = mapToItem(null, mouse.x, mouse.y)
                                root.openVersionMenu(versionId, displayName, versionActive,
                                                     scene.x, scene.y)
                            }
                        }
                        onPositionChanged: function (mouse) {
                            if (!(mouse.buttons & Qt.LeftButton) || !root.canSavePresets)
                                return
                            const scene = mapToItem(null, mouse.x, mouse.y)
                            if (!dragged) {
                                const dx = mouse.x - pressPoint.x
                                const dy = mouse.y - pressPoint.y
                                if (Math.sqrt(dx * dx + dy * dy) < Qt.styleHints.startDragDistance)
                                    return
                                dragged = true
                                root.beginDrag("version", versionId, displayName, scene.x, scene.y)
                            }
                            root.moveDrag(scene.x, scene.y)
                        }
                        onReleased: function (mouse) {
                            if (dragged) {
                                const scene = mapToItem(null, mouse.x, mouse.y)
                                root.finishDrag(scene.x, scene.y)
                            }
                        }
                        onCanceled: {
                            if (dragged)
                                root.cancelDrag()
                        }
                        // Do not block checkout on draftSubmitPending: a stuck
                        // pending bit after async create must not freeze the list.
                        // Save-checkpoint leases already disable canCheckoutVersion.
                        onClicked: function (mouse) {
                            if (mouse.button === Qt.LeftButton && !dragged
                                    && root.versionCheckoutEnabled && !versionActive)
                                root.checkoutVersionPreservingScroll(versionId)
                        }
                    }

                    ColumnLayout {
                        id: versionCardColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: appTheme.spaceMd
                        anchors.rightMargin: appTheme.spaceMd
                        spacing: appTheme.spaceXs

                        Label {
                            objectName: "editorVersionTitle"
                            Layout.fillWidth: true
                            visible: !versionCard.renaming
                            text: displayName
                            color: root.colText
                            wrapMode: Text.Wrap
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: appTheme.fontWeightStrong
                        }

                        // In-place rename: replaces the title while this card
                        // is the rename target. Enter commits, Escape cancels,
                        // focus loss commits only changed non-empty text.
                        TextField {
                            id: inlineRenameField
                            objectName: "editorVersionInlineRenameField"
                            Layout.fillWidth: true
                            visible: versionCard.renaming
                            enabled: versionCard.renaming && !root.draftSubmitPending
                            color: root.colText
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: appTheme.fontWeightStrong
                            placeholderText: qsTr("Version name")
                            selectByMouse: true
                            leftPadding: appTheme.spaceXs
                            rightPadding: appTheme.spaceXs
                            topPadding: 0
                            bottomPadding: 0
                            selectionColor: appTheme.editorListSelectedFillColor
                            selectedTextColor: appTheme.editorListSelectedInkColor
                            Accessible.name: qsTr("Rename Version")
                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: appTheme.bgBaseColor
                                border.width: 1
                                border.color: inlineRenameField.activeFocus
                                              ? root.colText
                                              : root.colCardBorder
                            }

                            function loadDraftText() {
                                if (versionCard.renaming && text !== root.draftText)
                                    text = root.draftText
                            }

                            Component.onCompleted: loadDraftText()
                            onVisibleChanged: loadDraftText()
                            onTextChanged: {
                                if (versionCard.renaming)
                                    root.draftText = text
                            }
                            onAccepted: root.commitDraft(false)
                            Keys.onEscapePressed: function (event) {
                                root.cancelDraft()
                                event.accepted = true
                            }
                            onEditingFinished: {
                                if (versionCard.renaming)
                                    root.commitDraftAfterFocusLoss("rename:" + versionCard.versionId)
                            }

                            Connections {
                                target: root
                                function onDraftTextChanged() {
                                    inlineRenameField.loadDraftText()
                                }
                                function onDraftFocusRequestChanged() {
                                    if (!versionCard.renaming)
                                        return
                                    inlineRenameField.loadDraftText()
                                    inlineRenameField.forceActiveFocus()
                                    inlineRenameField.selectAll()
                                }
                            }
                        }

                        Label {
                            objectName: "editorVersionInlineRenameError"
                            Layout.fillWidth: true
                            visible: versionCard.renaming && root.draftError.length > 0
                            text: root.draftError
                            color: appTheme.dangerColor
                            wrapMode: Text.WordWrap
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        // Single identity line: a HEAD badge, then the
                        // Version's head commit hash (or the image root). The
                        // badge and hash are code identifiers, not prose: they
                        // stay untranslated in the mono face. Only the image-
                        // root wording is translated, in the UI face, because
                        // the mono face has no CJK glyphs.
                        RowLayout {
                            objectName: "editorVersionHeadRow"
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs

                            Rectangle {
                                objectName: "editorVersionHeadBadge"
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth: headBadgeLabel.implicitWidth + appTheme.spaceXs * 2
                                implicitHeight: headBadgeLabel.implicitHeight + 2
                                radius: appTheme.badgeRadius
                                color: "transparent"
                                border.width: 1
                                border.color: root.colMuted

                                Label {
                                    id: headBadgeLabel
                                    anchors.centerIn: parent
                                    text: "HEAD"
                                    color: root.colMuted
                                    font.family: appTheme.monoFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }

                            Label {
                                objectName: "editorVersionSubtitle"
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                text: versionHead.length > 0 ? versionHead.slice(0, 8)
                                                             : qsTr("Image root")
                                color: root.colMuted
                                elide: Text.ElideRight
                                font.family: versionHead.length > 0 ? appTheme.monoFontFamily
                                                                    : appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                            }
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
