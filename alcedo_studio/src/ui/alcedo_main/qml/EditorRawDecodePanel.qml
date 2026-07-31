import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6G RAW Decode panel.  The session supplies capability decisions and
// the current operator snapshot; this panel only presents controls and sends
// complete operator parameter objects through the typed adjustment models.
Item {
    id: root
    objectName: "editorAdjustmentPanel_raw"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true
    property bool restoring: false
    property var rawParams: ({})
    property var rawMethodEntries: []

    readonly property var fallbackCapabilities: ({
        available: true,
        rawSource: true,
        metadataAvailable: true,
        neuralEngineAvailable: false,
        highlightsAvailable: true,
        methodValues: ["default", "legacy"],
        rawDefaultParamsJson: ""
    })
    readonly property var capabilities: root.readCapabilities()
    readonly property bool rawAvailable: root.controlsEnabled
                                       && Boolean(root.capabilities.available)
    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
    readonly property string capabilityStatus: root.statusText()

    function readCapabilities() {
        if (!root.editorSession)
            return root.fallbackCapabilities
        const value = root.editorSession.rawDecodeCapabilities
        return value === undefined || value === null ? root.fallbackCapabilities : value
    }

    function capabilityEnabled(key) {
        if (!root.capabilities || root.capabilities[key] === undefined)
            return true
        return Boolean(root.capabilities[key])
    }

    function statusText() {
        if (!root.controlsEnabled)
            return qsTr("Select an image to enable RAW Decode")
        if (!root.capabilities.available) {
            const reason = root.capabilities.unavailableReason
            return reason && String(reason).length > 0
                    ? String(reason)
                    : qsTr("RAW Decode is unavailable for this image.")
        }
        if (!root.capabilityEnabled("metadataAvailable"))
            return qsTr("RAW metadata is unavailable; decoder defaults are active.")
        return ""
    }

    function methodLabel(value) {
        switch (String(value)) {
        case "legacy": return qsTr("Legacy")
        case "neural_engine": return qsTr("Neural Engine")
        default: return qsTr("Default")
        }
    }

    function refreshMethodEntries() {
        var values = root.capabilities ? root.capabilities.methodValues : []
        if (!values || values.length === 0)
            values = ["default", "legacy"]
        var entries = []
        for (var i = 0; i < values.length; ++i) {
            const value = String(values[i])
            var found = false
            for (var j = 0; j < entries.length; ++j) {
                if (entries[j].value === value) {
                    found = true
                    break
                }
            }
            if (!found)
                entries.push({ value: value, label: root.methodLabel(value) })
        }
        root.rawMethodEntries = entries
    }

    function buildDefaultRawParams() {
        var params = {
            raw: {
                // No accelerator backend: the decode backend is a runtime
                // property of the pipeline (user setting), not an edit param.
                method: "default",
                highlights_reconstruct: true,
                use_camera_wb: true,
                user_wb: 7600.0,
                backend: "alcedo"
            }
        }
        const json = root.capabilities ? root.capabilities.rawDefaultParamsJson : ""
        if (json && String(json).length > 0) {
            try {
                params = JSON.parse(String(json))
            } catch (error) {
                params = params
            }
        }
        if (!params.raw)
            params.raw = {}
        return params
    }

    function mergeRawParams(rawEntry) {
        var params = root.buildDefaultRawParams()
        if (rawEntry) {
            for (var key in rawEntry)
                params.raw[key] = rawEntry[key]
        }
        return params
    }

    function setEnumValue(model, value, fallbackIndex) {
        if (!model)
            return
        var index = fallbackIndex === undefined ? 0 : fallbackIndex
        for (var i = 0; i < model.entries.length; ++i) {
            if (String(model.entries[i].value) === String(value)) {
                index = i
                break
            }
        }
        model.currentIndex = index
    }

    function buildRawParams() {
        // Pure builder: do not write root.rawParams during submit (avoids
        // binding churn while the patch is already enqueued).
        var base = root.rawParams && root.rawParams.raw
                ? root.rawParams
                : root.buildDefaultRawParams()
        var raw = {}
        if (base.raw) {
            var src = base.raw
            for (var k in src) {
                if (Object.prototype.hasOwnProperty.call(src, k))
                    raw[k] = src[k]
            }
        }
        raw.method = String(rawMethodModel.currentValue || "default")
        raw.highlights_reconstruct = Boolean(rawHighlightsModel.value)
        return JSON.stringify({ raw: raw })
    }

    function loadFromSnapshot(snapshot) {
        root.restoring = true
        var rawWrapper = snapshot ? snapshot["raw_decode"] : undefined
        var rawEntry = rawWrapper && rawWrapper["raw"] !== undefined
                ? rawWrapper["raw"] : rawWrapper
        root.rawParams = root.mergeRawParams(rawEntry)

        const method = rawEntry && rawEntry.method !== undefined
                ? String(rawEntry.method) : "default"
        root.setEnumValue(rawMethodModel, method, rawMethodModel.defaultIndex)
        rawHighlightsModel.value = rawEntry && rawEntry.highlights_reconstruct !== undefined
                ? Boolean(rawEntry.highlights_reconstruct) : rawHighlightsModel.defaultValue
        root.restoring = false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            objectName: "rawDecodeTitle"
            Layout.fillWidth: true
            text: qsTr("RAW Decode")
            color: root.colText
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightHeading
        }

        Label {
            objectName: "rawDecodeStatus"
            Layout.fillWidth: true
            text: root.capabilityStatus
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.WordWrap
            visible: text.length > 0
        }

        CollapsibleSection {
            id: rawSection
            objectName: "editorAdjustmentGroupShell_raw_decode"
            Layout.fillWidth: true
            title: qsTr("RAW Decode")
            expanded: true
            controlsEnabled: root.rawAvailable
            surfaceColor: root.colCardSurface
            disabledSurfaceColor: root.colCardSurface
            borderColor: root.colCardBorder
            textColor: root.colText
            mutedColor: root.colMuted
            hoverColor: root.colHover
            accentColor: root.colAccent
            bodyContentHeight: rawBody.implicitHeight + appTheme.spaceSm

            ColumnLayout {
                id: rawBody
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: appTheme.spaceXs
                spacing: appTheme.spaceSm

                AdjustmentCombo {
                    objectName: "rawDemosaicMethodControl"
                    controlObjectName: "rawDemosaicMethodCombo"
                    Layout.fillWidth: true
                    model: rawMethodModel
                }

                AdjustmentToggle {
                    objectName: "rawHighlightsControl"
                    model: rawHighlightsModel
                }
            }
        }

        Item { Layout.fillHeight: true }
    }

    EditorAdjustmentEnumModel {
        id: rawMethodModel
        objectName: "rawDemosaicMethodModel"
        fieldKey: "raw_decode"
        label: qsTr("Method")
        entries: root.rawMethodEntries
        defaultIndex: 0
        enabled: root.rawAvailable && entries.length > 0
        submitter: root.editorSession
        paramsBuilder: root.buildRawParams
    }

    EditorAdjustmentToggleModel {
        id: rawHighlightsModel
        objectName: "rawHighlightsModel"
        fieldKey: "raw_decode"
        label: qsTr("Enable Highlight Reconstruction")
        defaultValue: true
        value: true
        enabled: root.rawAvailable && root.capabilityEnabled("highlightsAvailable")
        submitter: root.editorSession
        paramsBuilder: root.buildRawParams
    }

    Connections {
        target: root.editorSession
        function onRawDecodeCapabilitiesChanged() {
            // Capability maps are independent of AdjustmentSnapshotChanged.
            root.refreshMethodEntries()
            root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        }
    }

    onEditorSessionChanged: {
        root.refreshMethodEntries()
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    Component.onCompleted: {
        root.refreshMethodEntries()
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }
}
