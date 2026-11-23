import QtQuick

// The AI Denoise error of the open photo over the viewport (AI Denoise plan, DN7, section 2.4;
// #1056). `error` is EditorSessionController.aiDenoiseError: {kind, text}, empty without an
// error. This file maps the kind to the translated title and message of the shared
// ViewportRenderAlert and keeps the raw text as its detail. Each kind has Turn Off AI Denoise; a
// missing model also has Manage (AI Models page) as the main action.
ViewportRenderAlert {
    id: root
    objectName: "editorAiDenoiseErrorCard"

    property var error: ({})
    property bool controlsEnabled: true

    signal manageRequested()
    signal turnOffRequested()

    readonly property string kind: error && error.kind !== undefined ? String(error.kind) : ""
    readonly property bool modelMissing: root.kind === "model_not_installed"

    function titleText() {
        switch (root.kind) {
        case "model_not_installed":
            return qsTr("AI Denoise model not installed")
        case "device_too_slow":
            return qsTr("This GPU is too slow for AI Denoise")
        case "sensor_not_supported":
            return qsTr("AI Denoise does not support this sensor")
        default:
            return qsTr("AI Denoise failed")
        }
    }

    function messageText() {
        switch (root.kind) {
        case "model_not_installed":
            return qsTr("The model file is not in the AI model folder.")
        case "device_too_slow":
            return qsTr("This photo would take too long on this GPU.")
        case "sensor_not_supported":
            return qsTr("AI Denoise supports Bayer RAW photos only.")
        default:
            return qsTr("The model stopped while processing this photo.")
        }
    }

    title: root.kind.length > 0 ? root.titleText() : ""
    message: root.kind.length > 0 ? root.messageText() : ""
    detail: root.error && root.error.text !== undefined ? String(root.error.text) : ""
    primaryText: root.modelMissing ? qsTr("Manage") : qsTr("Turn Off AI Denoise")
    primaryEnabled: root.modelMissing || root.controlsEnabled
    secondaryText: root.modelMissing ? qsTr("Turn Off AI Denoise") : ""
    secondaryEnabled: root.controlsEnabled

    onPrimaryClicked: {
        if (root.modelMissing)
            root.manageRequested()
        else
            root.turnOffRequested()
    }
    onSecondaryClicked: root.turnOffRequested()
}
