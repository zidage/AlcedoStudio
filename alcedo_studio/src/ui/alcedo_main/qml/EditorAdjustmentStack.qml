import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Right-side editor tools: histogram/waveform scope slot, selected-node
// name/EXIF header, a stable adjustment navbar, and stacked panel bodies.
// The navbar does not hide pages when the selected node changes; write
// targeting still rejects fields the current node does not own.
//
// Surfaces use opaque named theme colors. The outer shell always uses the
// shared card surface so the right column matches History/Versions, the
// viewport placeholder, and the filmstrip. Disabled state mutes text/icons
// and disables controls — it does not recolor the shell to a second panel tone.
//
// Snapshot loading: stack fans out on AdjustmentSnapshotChanged / session bind.
// Child panels still bootstrap once on construction so first-frame projection
// works if session is assigned before StackLayout children exist.
Item {
    id: root
    objectName: "editorAdjustmentStack"

    property var theme: null
    property var editorSession: null
    property var interaction: null
    property var nodeController: null
    property bool controlsEnabled: true

    // ── Opaque semantic colors (no alpha derivations) ─────────────────────
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colBase: theme ? theme.colBgBase : "#161719"
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property int controlRadius: theme ? theme.controlRadius : 10

    readonly property var maskCreation: editorSession ? editorSession.maskCreation : null
    readonly property string activePanel: editorSession
                                          ? String(editorSession.activeAdjustmentPanel || "tone")
                                          : "tone"

    readonly property var navItems: [
        { key: "tone", icon: "qrc:/panel_icons/adjustments.svg",
          label: qsTr("Tone"), itemObjectName: "editorAdjustmentNav_tone" },
        { key: "look", icon: "qrc:/panel_icons/palette.svg",
          label: qsTr("Look"), itemObjectName: "editorAdjustmentNav_look" },
        { key: "lut", icon: "qrc:/panel_icons/box.svg",
          label: qsTr("LUT"), itemObjectName: "editorAdjustmentNav_lut" },
        { key: "display", icon: "qrc:/panel_icons/color-filter.svg",
          label: qsTr("Display Transform"),
          itemObjectName: "editorAdjustmentNav_display" },
        { key: "geometry", icon: "qrc:/panel_icons/crop.svg",
          label: qsTr("Geometry"), itemObjectName: "editorAdjustmentNav_geometry" },
        { key: "raw", icon: "qrc:/panel_icons/aperture.svg",
          label: qsTr("RAW Decode"), itemObjectName: "editorAdjustmentNav_raw" }
    ]

    readonly property int preferredPanelWidth: appTheme.editorSidePanelWidth
    readonly property int minimumPanelWidth: appTheme.editorSidePanelWidthMin
    readonly property int maximumPanelWidth: appTheme.editorSidePanelWidthMax
    property bool expanded: true

    property real stackExpandProgress: 1
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property real stackWidth: preferredPanelWidth * stackExpandProgress
    readonly property bool foldAtRestExpanded: expanded && stackExpandProgress >= 0.999

    implicitWidth: stackWidth
    implicitHeight: 400
    Layout.preferredWidth: stackWidth
    Layout.minimumWidth: foldAtRestExpanded ? minimumPanelWidth : 0
    Layout.maximumWidth: foldAtRestExpanded ? maximumPanelWidth : preferredPanelWidth
    Layout.fillHeight: true
    opacity: stackExpandProgress
    enabled: expanded
    clip: true

    function driveFoldProgress(value) {
        foldManualDrive = true
        stackExpandProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        stackExpandProgress = expanded ? 1 : 0
    }

    onExpandedChanged: {
        _foldDuration = expanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive)
            stackExpandProgress = expanded ? 1 : 0
    }

    Behavior on stackExpandProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: appTheme.motionEasing
        }
    }

    property int lastAppliedRevision: -1

    EditorLutCatalogModel {
        id: lutModel
        objectName: "adjustmentStackLutModel"
        submitter: root.editorSession
    }

    function loadFromSnapshot(snapshot) {
        if (!editorSession)
            return
        if (snapshot === undefined || snapshot === null)
            return
        root.lastAppliedRevision += 1
        if (typeof tonePanel.loadFromSnapshot === "function")
            tonePanel.loadFromSnapshot(snapshot)
        if (typeof lookPanel.loadFromSnapshot === "function")
            lookPanel.loadFromSnapshot(snapshot)
        if (typeof lutPanel.loadFromSnapshot === "function")
            lutPanel.loadFromSnapshot(snapshot)
        if (typeof displayPanel.loadFromSnapshot === "function")
            displayPanel.loadFromSnapshot(snapshot)
        if (typeof geometryPanel.loadFromSnapshot === "function")
            geometryPanel.loadFromSnapshot(snapshot)
        if (typeof rawPanel.loadFromSnapshot === "function")
            rawPanel.loadFromSnapshot(snapshot)
    }

    function scheduleLoadFromSession() {
        if (!root.editorSession)
            return
        Qt.callLater(function () {
            if (root.editorSession)
                root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
        })
    }

    function selectPanel(panel) {
        if (!editorSession)
            return
        if (root.maskCreation && root.maskCreation.bodyVisible)
            root.maskCreation.hideBody()
        editorSession.activeAdjustmentPanel = panel
    }

    function confirmGeometryAndReturnToTone() {
        if (root.activePanel !== "geometry")
            return false
        if (typeof geometryPanel.confirmAndReturnToTone === "function") {
            geometryPanel.confirmAndReturnToTone()
            return true
        }
        return false
    }

    function panelTitle(key) {
        switch (key) {
        case "look": return qsTr("Look")
        case "lut": return qsTr("LUT")
        case "display": return qsTr("Display Transform")
        case "geometry": return qsTr("Geometry")
        case "raw": return qsTr("RAW Decode")
        default: return qsTr("Tone")
        }
    }

    function panelEmptyHint(key) {
        if (!root.controlsEnabled) {
            return qsTr("Select an image to enable adjustments")
        }
        return qsTr("No adjustments yet")
    }

    Rectangle {
        id: panelShell
        objectName: "editorRightPanelSlot"
        anchors.fill: parent
        radius: root.panelRadius
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: appTheme.spaceMd
            spacing: appTheme.spaceMd

            EditorScopePanel {
                id: scopeSlot
                objectName: "editorScopeSlot"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.editorScopeHeight
                Layout.minimumHeight: appTheme.editorScopeHeightMin
                theme: root.theme
                editorSession: root.editorSession
                controlsEnabled: root.controlsEnabled
            }

            EditorAdjustmentHeader {
                id: adjustmentHeader
                Layout.fillWidth: true
                theme: root.theme
                nodeName: root.nodeController
                          ? String(root.nodeController.selectedNodeName || "")
                          : ""
                focalText: root.editorSession
                            ? String(root.editorSession.exifFocalText || "\u2014")
                            : "\u2014"
                apertureText: root.editorSession
                              ? String(root.editorSession.exifApertureText || "\u2014")
                              : "\u2014"
                shutterText: root.editorSession
                              ? String(root.editorSession.exifShutterText || "\u2014")
                              : "\u2014"
                isoText: root.editorSession
                         ? String(root.editorSession.exifIsoText || "\u2014")
                         : "\u2014"
                maskCreation: root.editorSession ? root.editorSession.maskCreation : null
                selectedNodeKind: root.nodeController
                                  ? String(root.nodeController.selectedNodeKind || "")
                                  : ""
                cropOverlayVisible: root.interaction
                                    ? !!root.interaction.cropOverlayVisible
                                    : false
                controlsEnabled: root.controlsEnabled
            }

            SlidingIconNav {
                id: adjustmentNav
                objectName: "editorAdjustmentNav"
                Layout.fillWidth: true
                currentKey: root.activePanel
                controlsEnabled: root.controlsEnabled
                trackColor: root.colBase
                trackBorderColor: root.colCardBorder
                idleIconColor: root.colMuted
                thumbObjectName: "editorAdjustmentNavThumb"
                items: root.navItems
                onActivated: key => root.selectPanel(key)
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                StackLayout {
                    id: panelStack
                    objectName: "editorAdjustmentPanelStack"
                    anchors.fill: parent
                    currentIndex: {
                        switch (root.activePanel) {
                        case "look": return 1
                        case "lut": return 2
                        case "display": return 3
                        case "geometry": return 4
                        case "raw": return 5
                        default: return 0
                        }
                    }

                    EditorTonePanel {
                        id: tonePanel
                        objectName: "editorAdjustmentPanel_tone"
                        theme: root.theme
                        editorSession: root.editorSession
                        controlsEnabled: root.controlsEnabled
                    }

                    EditorLookPanel {
                        id: lookPanel
                        objectName: "editorAdjustmentPanel_look"
                        theme: root.theme
                        editorSession: root.editorSession
                        controlsEnabled: root.controlsEnabled
                        lutModel: lutModel
                    }

                    LUTPanel {
                        id: lutPanel
                        objectName: "editorAdjustmentPanel_lut"
                        theme: root.theme
                        editorSession: root.editorSession
                        lutModel: lutModel
                        controlsEnabled: root.controlsEnabled
                    }

                    EditorDisplayTransformPanel {
                        id: displayPanel
                        objectName: "editorAdjustmentPanel_display"
                        theme: root.theme
                        editorSession: root.editorSession
                        controlsEnabled: root.controlsEnabled
                    }

                    EditorGeometryPanel {
                        id: geometryPanel
                        objectName: "editorAdjustmentPanel_geometry"
                        theme: root.theme
                        editorSession: root.editorSession
                        interaction: root.interaction
                        controlsEnabled: root.controlsEnabled
                        panelActive: root.activePanel === "geometry"
                    }

                    EditorRawDecodePanel {
                        id: rawPanel
                        objectName: "editorAdjustmentPanel_raw"
                        theme: root.theme
                        editorSession: root.editorSession
                        controlsEnabled: root.controlsEnabled
                    }
                }

                EditorMasksContextPanel {
                    id: masksPanel
                    objectName: "editorAdjustmentPanel_masksOverlay"
                    anchors.fill: parent
                    visible: root.maskCreation && root.maskCreation.bodyVisible
                    theme: root.theme
                    editorSession: root.editorSession
                    nodeController: root.nodeController
                    maskCreation: root.maskCreation
                    controlsEnabled: root.controlsEnabled
                    z: 1
                }
            }
        }
    }

    Connections {
        target: root.editorSession
        function onAdjustmentSnapshotChanged() {
            root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        }
    }
    onEditorSessionChanged: {
        root.lastAppliedRevision = -1
        root.scheduleLoadFromSession()
    }
    Component.onCompleted: {
        stackExpandProgress = expanded ? 1 : 0
        _motionArmed = true
        root.lastAppliedRevision = -1
        root.scheduleLoadFromSession()
    }
}
