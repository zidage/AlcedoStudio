import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// RAW Decode panel. Import already asked LibRaw whether the file is
// decodable; this panel only presents controls and submits complete operator
// parameter objects through the typed adjustment models.
Item {
    id: root
    objectName: "editorAdjustmentPanel_raw"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true
    property bool restoring: false
    property var rawParams: ({})

    // ponytail: fixed method list; no per-image capability map.
    readonly property var rawMethodEntries: [
        { value: "default", label: qsTr("Default") },
        { value: "legacy", label: qsTr("Legacy") },
        { value: "neural_engine", label: qsTr("Neural Engine") }
    ]

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor

    function buildDefaultRawParams() {
        return {
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

        CollapsibleSection {
            id: rawSection
            objectName: "editorAdjustmentGroupShell_raw_decode"
            Layout.fillWidth: true
            title: qsTr("RAW Decode")
            expanded: true
            controlsEnabled: root.controlsEnabled
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
        enabled: root.controlsEnabled
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
        enabled: root.controlsEnabled
        submitter: root.editorSession
        paramsBuilder: root.buildRawParams
    }

    onEditorSessionChanged: {
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    Component.onCompleted: {
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }
}
