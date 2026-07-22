import QtQuick

// Phase 6C-5: thin QML adapter over InteractionPolicyController for editor
// navigation gates (filmstrip, workspace, Version checkout, Paste, Merge).
// Keeps EditorWorkspace free of lock-reason plumbing; consumers bind to the
// readonly properties and optional reason strings.
QtObject {
    id: root

    property var interactionPolicy: null
    property var editorSession: null

    readonly property bool saveCheckpointActive: editorSession
                                                 && String(editorSession.sessionState || "") === "Saving"

    readonly property bool canSelectEditorImage: interactionPolicy
                                                 ? interactionPolicy.canSelectEditorImage
                                                 : !saveCheckpointActive
    readonly property string selectEditorImageReason: interactionPolicy
                                                      ? String(interactionPolicy.selectEditorImageReason || "")
                                                      : (saveCheckpointActive ? qsTr("Saving editor changes") : "")

    readonly property bool canSwitchWorkspace: interactionPolicy
                                               ? interactionPolicy.canSwitchWorkspace
                                               : !saveCheckpointActive
    readonly property string switchWorkspaceReason: interactionPolicy
                                                    ? String(interactionPolicy.switchWorkspaceReason || "")
                                                    : (saveCheckpointActive ? qsTr("Saving editor changes") : "")

    readonly property bool canCheckoutVersion: interactionPolicy
                                               ? interactionPolicy.canCheckoutVersion
                                               : !saveCheckpointActive
    readonly property string checkoutVersionReason: interactionPolicy
                                                    ? String(interactionPolicy.checkoutVersionReason || "")
                                                    : (saveCheckpointActive ? qsTr("Saving editor changes") : "")

    readonly property bool canPasteAdjustments: interactionPolicy
                                                ? interactionPolicy.canPasteAdjustments
                                                : !saveCheckpointActive
    readonly property string pasteAdjustmentsReason: interactionPolicy
                                                     ? String(interactionPolicy.pasteAdjustmentsReason || "")
                                                     : (saveCheckpointActive ? qsTr("Saving editor changes") : "")

    readonly property bool canMergeAdjustments: interactionPolicy
                                                ? interactionPolicy.canMergeAdjustments
                                                : !saveCheckpointActive
    readonly property string mergeAdjustmentsReason: interactionPolicy
                                                     ? String(interactionPolicy.mergeAdjustmentsReason || "")
                                                     : (saveCheckpointActive ? qsTr("Saving editor changes") : "")
}
