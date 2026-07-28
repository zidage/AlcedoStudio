import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Right-side editor tools: histogram/waveform scope slot, adjustment navbar,
// and stacked panel bodies for Tone / Look / LUT / Display Transform / Geometry /
// RAW Decode.
// section shell that proves the shared fold motion contract.
// Phase 4D: every surface, button fill, and disabled state uses opaque named
// theme colors (alpha 255). No parent-shell opacity, withAlpha(…), Qt.rgba(…,
// alpha), or "transparent" surface fills remain.
//
// Surface family: the outer shell always uses the shared card surface so the
// right column matches History/Versions, the viewport placeholder, and the
// filmstrip. Disabled state mutes text/icons and disables controls — it does
// not recolor the shell to a second panel tone.
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

    // Phase 6C-7: last applied snapshot revision for idempotent reload.
    property int lastAppliedRevision: -1

    // LUT catalog model shared between EditorLookPanel and LUTPanel.
    EditorLutCatalogModel {
        id: lutModel
        objectName: "adjustmentStackLutModel"
        submitter: root.editorSession
    }


    /// Load panel values from the editor session adjustment snapshot.
    /// Idempotent: re-applying the same revision has no effect. Each panel
    /// extracts its owned field keys from the snapshot map.
    function loadFromSnapshot(snapshot) {
        if (!editorSession) return
        const rev = editorSession.snapshotRevision
        if (rev === root.lastAppliedRevision) return
        root.lastAppliedRevision = rev
        if (snapshot === undefined || snapshot === null) return
        // Each panel owns its snapshot fields (Tone / Look / LUT). Do not
        // special-case LUT only at the stack — that path is easy to skip on
        // workspace re-entry and leaves the list without a selected row.
        if (typeof tonePanel.loadFromSnapshot === "function") {
            tonePanel.loadFromSnapshot(snapshot)
        }
        if (typeof lookPanel.loadFromSnapshot === "function") {
            lookPanel.loadFromSnapshot(snapshot)
        }
        if (typeof lutPanel.loadFromSnapshot === "function") {
            lutPanel.loadFromSnapshot(snapshot)
        }
        if (typeof displayPanel.loadFromSnapshot === "function") {
            displayPanel.loadFromSnapshot(snapshot)
        }
        if (typeof geometryPanel.loadFromSnapshot === "function") {
            geometryPanel.loadFromSnapshot(snapshot)
        }
        if (typeof rawPanel.loadFromSnapshot === "function") {
            rawPanel.loadFromSnapshot(snapshot)
        }
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

                // Phase 6B: production Tone panel.
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

                component EmptyAdjustmentPage: Item {
                    property string panelKey: "tone"

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: appTheme.spaceSm

                        Label {
                            Layout.fillWidth: true
                            text: root.panelTitle(panelKey)
                            color: root.colText
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: appTheme.fontWeightHeading
                        }

                        // Shared fold reference for adjustment groups (Phase 4C).
                        // Real controls replace the empty body in Phase 6.
                        // Surface stays the card family; section border provides
                        // hierarchy without a second panel tone.
                        CollapsibleSection {
                            id: groupShell
                            objectName: "editorAdjustmentGroupShell_" + panelKey
                            Layout.fillWidth: true
                            title: root.panelTitle(panelKey)
                            expanded: true
                            controlsEnabled: root.controlsEnabled
                            surfaceColor: root.colCardSurface
                            disabledSurfaceColor: root.colCardSurface
                            borderColor: root.colCardBorder
                            textColor: root.colText
                            mutedColor: root.colMuted
                            hoverColor: theme ? theme.colHover : appTheme.hoverColor
                            accentColor: root.colAccent
                            bodyContentHeight: 96

                            Label {
                                anchors.centerIn: parent
                                width: parent.width - 8
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                                text: root.panelEmptyHint(panelKey)
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeBody
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }
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

    // Phase 6C-7: auto-load panel state when the backend publishes a snapshot.
    // Interactive submitPatch no longer emits this on every pointer move (session
    // controller suppresses the echo). Settled / undo / image-switch still publish.
    Connections {
        target: root.editorSession
        function onAdjustmentSnapshotChanged() {
            root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        }
    }
    // Also load on initial binding when editorSession changes.
    onEditorSessionChanged: {
        root.lastAppliedRevision = -1
        if (root.editorSession) {
            root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
        }
    }
    // createWithInitialProperties / first frame: session may already be set
    // without a change signal. Match Tone/Look panels and always attempt load.
    Component.onCompleted: {
        if (root.editorSession) {
            root.lastAppliedRevision = -1
            root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
        }
    }
}
