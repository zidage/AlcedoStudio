import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Right-side editor tools: histogram/waveform scope slot, adjustment navbar,
// and stacked panel bodies for Tone / Look / LUT / Display Transform / Geometry /
// RAW Decode.
//
// Phase 4D: every surface, button fill, and disabled state uses opaque named
// theme colors (alpha 255). No parent-shell opacity, withAlpha(…), Qt.rgba(…,
// alpha), or "transparent" surface fills remain.
//
// Surface family: the outer shell always uses the shared card surface so the
// right column matches History/Versions, the viewport placeholder, and the
// filmstrip. Disabled state mutes text/icons and disables controls — it does
// not recolor the shell to a second panel tone.
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
    property bool controlsEnabled: true

    // ── Opaque semantic colors (no alpha derivations) ─────────────────────
    // Card surface family (DESIGN.md): same as Library cards and left rail.
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    // Sunken inset for scope + nav track (interactive well, not a second card).
    readonly property color colBase: theme ? theme.colBgBase : "#161719"
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property int controlRadius: theme ? theme.controlRadius : 10

    // Bound to editorSession so selection survives Loader teardown.
    readonly property string activePanel: editorSession
                                          ? String(editorSession.activeAdjustmentPanel || "tone")
                                          : "tone"

    // Final sizing contract — editor side-panel tokens (DESIGN.md). The
    // preferred width matches the History/Versions expanded panel so the two
    // side columns read as one family.
    readonly property int preferredPanelWidth: appTheme.editorSidePanelWidth
    readonly property int minimumPanelWidth: appTheme.editorSidePanelWidthMin
    readonly property int maximumPanelWidth: appTheme.editorSidePanelWidthMax

    implicitWidth: preferredPanelWidth
    implicitHeight: 400
    Layout.preferredWidth: preferredPanelWidth
    Layout.minimumWidth: minimumPanelWidth
    Layout.maximumWidth: maximumPanelWidth
    Layout.fillHeight: true

    // Counts successful fan-outs for tests/diagnostics. Content equality is
    // gated by the controller: AdjustmentSnapshotChanged only fires when the
    // cached map actually changes (interactive submit suppresses the emit).
    // Do not JSON.stringify the full snapshot for idempotency — that freezes
    // the GUI on large maps (curve/HLS/ODT/crop) every settled commit.
    property int lastAppliedRevision: -1

    // LUT catalog model shared between EditorLookPanel and LUTPanel.
    EditorLutCatalogModel {
        id: lutModel
        objectName: "adjustmentStackLutModel"
        submitter: root.editorSession
    }

    /// Load panel values from the editor session adjustment snapshot.
    /// Fan-out only: each panel extracts its owned field keys. Panel loaders are
    /// idempotent (equal values no-op), so a second apply from a panel bootstrap
    /// path is cheap.
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

    /// Defer until child panels finish construction (createWithInitialProperties
    /// can assign editorSession before StackLayout children exist).
    function scheduleLoadFromSession() {
        if (!root.editorSession)
            return
        Qt.callLater(function () {
            if (root.editorSession)
                root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
        })
    }

    function selectPanel(panel) {
        if (!editorSession) {
            return
        }
        // Leaving Geometry must commit the draft crop before the session turns
        // off geometry_overlay_only and requests the bake refresh.
        if (root.activePanel === "geometry" && String(panel).toLowerCase() !== "geometry") {
            if (typeof geometryPanel.confirmPendingCrop === "function")
                geometryPanel.confirmPendingCrop()
        }
        editorSession.activeAdjustmentPanel = panel
    }

    /// Enter / Return while Geometry is active: commit draft crop and return to Tone.
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
        // Product empty state — no developer/placeholder phrasing.
        return qsTr("No adjustments yet")
    }

    Rectangle {
        id: panelShell
        objectName: "editorRightPanelSlot"
        anchors.fill: parent
        radius: root.panelRadius
        // Always the shared card surface — matches left rail, viewport, filmstrip.
        // Disabled is expressed through control enablement and muted copy, not a
        // second shell fill that breaks the editor card family.
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: appTheme.spaceMd
            spacing: appTheme.spaceMd

            // Histogram / waveform placement stays with the right-side tools.
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
                items: [
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
                      label: qsTr("Geometry"),
                      itemObjectName: "editorAdjustmentNav_geometry" },
                    { key: "raw", icon: "qrc:/panel_icons/aperture.svg",
                      label: qsTr("RAW Decode"), itemObjectName: "editorAdjustmentNav_raw" }
                ]
                onActivated: key => root.selectPanel(key)
            }

            // Stacked panel bodies. Explicit children keep StackLayout indices
            // stable while each panel owns its snapshot fields.
            StackLayout {
                id: panelStack
                objectName: "editorAdjustmentPanelStack"
                Layout.fillWidth: true
                Layout.fillHeight: true
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
        }
    }

    // Settled / undo / image-switch publish. Interactive submitPatch suppresses
    // the emit so pointer moves do not re-enter this fan-out.
    Connections {
        target: root.editorSession
        function onAdjustmentSnapshotChanged() {
            root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        }
    }
    // Session rebind: reset apply counter and project after children are ready.
    onEditorSessionChanged: {
        root.lastAppliedRevision = -1
        root.scheduleLoadFromSession()
    }
    // createWithInitialProperties / first frame: session may already be set
    // without a change signal.
    Component.onCompleted: {
        root.lastAppliedRevision = -1
        root.scheduleLoadFromSession()
    }
}
