import QtQuick

// Paste/Merge entry actions for the adjustment-transfer dialog. The dialog and
// backend are explicit dependencies; this object owns permission checks and
// does not know workspace, filmstrip, history, or persistence details.
QtObject {
    id: root
    objectName: "editorAdjustmentTransferActions"

    property var adjustmentTransfer: null
    property var adjustmentTransferDialog: null
    property var interactionPolicy: null
    property var pendingTargets: []

    readonly property bool hasTargets: pendingTargets && pendingTargets.length > 0
    readonly property bool pasteEnabled: interactionPolicy
                                        ? Boolean(interactionPolicy.canPasteAdjustments)
                                        : true
    readonly property bool mergeEnabled: interactionPolicy
                                        ? Boolean(interactionPolicy.canMergeAdjustments)
                                        : true
    readonly property string pasteDisabledReason: interactionPolicy
                                                 ? String(interactionPolicy.pasteAdjustmentsReason || "")
                                                 : ""
    readonly property string mergeDisabledReason: interactionPolicy
                                                 ? String(interactionPolicy.mergeAdjustmentsReason || "")
                                                 : ""

    signal messageRequested(string message)

    function notifyBlocked(reason) {
        const text = String(reason || "")
        if (text.length > 0) {
            messageRequested(text)
        }
    }

    // Opens the paste dialog when a package and target list are available.
    // A blocked request leaves the dialog closed and emits its policy reason.
    function requestPasteAdjustments() {
        if (!adjustmentTransfer || !adjustmentTransferDialog
                || !adjustmentTransfer.packageAvailable || !hasTargets) {
            return
        }
        if (!pasteEnabled) {
            notifyBlocked(pasteDisabledReason)
            return
        }

        adjustmentTransferDialog.mode = "paste"
        adjustmentTransferDialog.pasteStrategy = mergeEnabled ? "merge" : "paste"
        adjustmentTransferDialog.sourceTitle = adjustmentTransfer.packageSourceTitle
        adjustmentTransferDialog.targetCount = pendingTargets.length
        adjustmentTransferDialog.adjustmentRows = adjustmentTransfer.packageSummary
        adjustmentTransferDialog.open()
    }

    // Applies the selected strategy only while its current policy gate allows
    // it. Returns false without a backend call when the strategy is blocked.
    function applyPaste(strategy) {
        if (!adjustmentTransfer || !hasTargets) {
            return false
        }

        const selectedStrategy = String(strategy || "")
        if (selectedStrategy !== "merge" && selectedStrategy !== "paste") {
            return false
        }
        if (!pasteEnabled) {
            notifyBlocked(pasteDisabledReason)
            return false
        }
        if (selectedStrategy === "merge" && !mergeEnabled) {
            notifyBlocked(mergeDisabledReason)
            return false
        }

        const result = adjustmentTransfer.Paste(pendingTargets, selectedStrategy)
        if (result && result.message) {
            messageRequested(String(result.message))
        }
        return Boolean(result && result.success === true)
    }
}
