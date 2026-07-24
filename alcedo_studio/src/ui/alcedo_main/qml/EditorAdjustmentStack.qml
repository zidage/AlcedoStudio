import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Right-side editor tools: histogram/waveform scope slot, adjustment navbar,
// and stacked panel bodies for Tone / Look / Display Transform / Geometry /
// RAW Decode. Phase 4B finalizes navigation, ordering, selection, collapse,
// and sizing; real controls arrive in Phase 6. Phase 4C adds a collapsible
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
    property bool controlsEnabled: true

    // ── Opaque semantic colors (no alpha derivations) ─────────────────────
    // Card surface family (DESIGN.md): same as Library cards and left rail.
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
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
    property int lastAppliedRevision: 0

    /// Load panel values from the editor session adjustment snapshot.
    /// Idempotent: re-applying the same revision has no effect. Each panel
    /// extracts its owned field keys from the snapshot map.
    function loadFromSnapshot(snapshot) {
        if (!editorSession) return
        const rev = editorSession.snapshotRevision
        if (rev === root.lastAppliedRevision) return
        root.lastAppliedRevision = rev
        if (snapshot === undefined || snapshot === null) return
        // Distribute to each panel body. Only Tone is wired today;
        // Look / Display / Geometry / RAW join in their Phase 6 ports.
        if (typeof tonePanel.loadFromSnapshot === "function") {
            tonePanel.loadFromSnapshot(snapshot)
        }
    }


    function selectPanel(panel) {
        if (!editorSession) {
            return
        }
        editorSession.activeAdjustmentPanel = panel
    }

    function panelTitle(key) {
        switch (key) {
        case "look": return qsTr("Look")
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
            Rectangle {
                id: scopeSlot
                objectName: "editorScopeSlot"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.editorScopeHeight
                Layout.minimumHeight: appTheme.editorScopeHeightMin
                radius: appTheme.controlRadiusSmall
                // Sunken inset (not a nested card of the same fill).
                color: root.colBase
                border.width: 1
                border.color: root.colCardBorder

                Label {
                    anchors.centerIn: parent
                    text: qsTr("Histogram / Waveform")
                    color: root.colMuted
                    font.pixelSize: appTheme.fontSizeBody
                }
            }

            // Compact adjustment navbar: sunken track + sliding selection window
            // (same family as Main.qml workspaceSwitchThumb). 40 px hits, 18 px
            // SVGs; the thumb — not per-button fill — is the selected surface.
            Rectangle {
                id: adjustmentNav
                objectName: "editorAdjustmentNav"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact + appTheme.spaceXs
                radius: appTheme.controlRadiusSmall
                color: root.colBase
                border.width: 1
                border.color: root.colCardBorder

                readonly property int navHit: appTheme.iconButtonHitSizeCompact
                readonly property int navSpacing: 2
                readonly property int navIndex: {
                    switch (root.activePanel) {
                    case "look": return 1
                    case "display": return 2
                    case "geometry": return 3
                    case "raw": return 4
                    default: return 0
                    }
                }
                // Chrome size mirrors IconActionButton compact wells (optical+8 / hit-8).
                readonly property int thumbSize: Math.min(
                    navHit,
                    Math.max(appTheme.iconOpticalSizeCompact + 8, navHit - 8))

                Item {
                    id: navHost
                    anchors.centerIn: parent
                    width: navRow.width
                    height: adjustmentNav.navHit
                    opacity: root.controlsEnabled ? 1.0 : 0.55

                    // Sliding selection window under the icons. Buttons must use
                    // transparent chrome — an opaque fillIdle (even track-matched)
                    // completely covers this thumb so you only see a blue slab
                    // sliding in the 2 px gaps between segments.
                    Rectangle {
                        id: navThumb
                        objectName: "editorAdjustmentNavThumb"
                        z: 0
                        width: adjustmentNav.thumbSize
                        height: adjustmentNav.thumbSize
                        radius: Math.max(4, appTheme.controlRadiusSmall - 2)
                        color: root.colAccent
                        border.width: 1
                        border.color: {
                            const s = theme && theme.colAccentSecondary
                                      ? theme.colAccentSecondary
                                      : appTheme.accentSecondaryColor
                            return Qt.rgba(s.r, s.g, s.b, 0.52)
                        }
                        y: (parent.height - height) / 2
                        x: adjustmentNav.navIndex
                           * (adjustmentNav.navHit + adjustmentNav.navSpacing)
                           + (adjustmentNav.navHit - width) / 2

                        Behavior on x {
                            enabled: !appTheme.reduceMotion
                            NumberAnimation {
                                duration: Math.max(appTheme.motionFoldOpenMs, 240)
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.18
                            }
                        }

                        // Soft land pulse so the window feels mechanical, not
                        // just a translating rect. Skipped under reduceMotion.
                        scale: 1.0
                        transformOrigin: Item.Center

                        Connections {
                            target: adjustmentNav
                            function onNavIndexChanged() {
                                if (appTheme.reduceMotion) {
                                    navThumb.scale = 1.0
                                    return
                                }
                                thumbLandAnim.restart()
                            }
                        }

                        SequentialAnimation {
                            id: thumbLandAnim
                            NumberAnimation {
                                target: navThumb
                                property: "scale"
                                to: 0.90
                                duration: 70
                                easing.type: Easing.OutQuad
                            }
                            NumberAnimation {
                                target: navThumb
                                property: "scale"
                                to: 1.0
                                duration: 200
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.4
                            }
                        }
                    }

                    Row {
                        id: navRow
                        z: 1
                        spacing: adjustmentNav.navSpacing

                        // Capsule rule (workspace switch): no per-button fill.
                        // Transparent wells let the accent thumb read as the
                        // selected surface under the SVG; only the icon sits above.
                        component NavIconButton: IconActionButton {
                            compact: true
                            enabled: root.controlsEnabled
                            showHoverFill: false
                            showFocusRing: true
                            iconColorDefault: selected ? "#FFFFFF" : root.colMuted
                            iconColorMuted: root.colMuted
                            fillIdle: "transparent"
                            fillSelected: "transparent"
                        }

                        NavIconButton {
                            objectName: "editorAdjustmentNav_tone"
                            selected: root.activePanel === "tone"
                            iconSrc: "qrc:/panel_icons/adjustments.svg"
                            actionName: qsTr("Tone")
                            onClicked: root.selectPanel("tone")
                        }
                        NavIconButton {
                            objectName: "editorAdjustmentNav_look"
                            selected: root.activePanel === "look"
                            iconSrc: "qrc:/panel_icons/palette.svg"
                            actionName: qsTr("Look")
                            onClicked: root.selectPanel("look")
                        }
                        NavIconButton {
                            objectName: "editorAdjustmentNav_display"
                            selected: root.activePanel === "display"
                            iconSrc: "qrc:/panel_icons/color-filter.svg"
                            actionName: qsTr("Display Transform")
                            onClicked: root.selectPanel("display")
                        }
                        NavIconButton {
                            objectName: "editorAdjustmentNav_geometry"
                            selected: root.activePanel === "geometry"
                            iconSrc: "qrc:/panel_icons/crop.svg"
                            actionName: qsTr("Geometry")
                            onClicked: root.selectPanel("geometry")
                        }
                        NavIconButton {
                            objectName: "editorAdjustmentNav_raw"
                            selected: root.activePanel === "raw"
                            iconSrc: "qrc:/panel_icons/aperture.svg"
                            actionName: qsTr("RAW Decode")
                            onClicked: root.selectPanel("raw")
                        }
                    }
                }
            }

            // Stacked panel bodies. Navigation and sizing are final; content is
            // intentionally empty until each panel is ported. Explicit children
            // (not Repeater) keep StackLayout indices stable for tests.
            StackLayout {
                id: panelStack
                objectName: "editorAdjustmentPanelStack"
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: {
                    switch (root.activePanel) {
                    case "look": return 1
                    case "display": return 2
                    case "geometry": return 3
                    case "raw": return 4
                    default: return 0
                    }
                }

                // Phase 6B: production Tone panel. Other panels keep the empty
                // shell until their Phase 6 ports land.
                EditorTonePanel {
                    id: tonePanel
                    objectName: "editorAdjustmentPanel_tone"
                    theme: root.theme
                    editorSession: root.editorSession
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

                EmptyAdjustmentPage {
                    objectName: "editorAdjustmentPanel_look"
                    panelKey: "look"
                }
                EmptyAdjustmentPage {
                    objectName: "editorAdjustmentPanel_display"
                    panelKey: "display"
                }
                EmptyAdjustmentPage {
                    objectName: "editorAdjustmentPanel_geometry"
                    panelKey: "geometry"
                }
                EmptyAdjustmentPage {
                    objectName: "editorAdjustmentPanel_raw"
                    panelKey: "raw"
                }
            }
        }
    }

    // Phase 6C-7: auto-load panel state when the backend publishes a snapshot.
    Connections {
        target: root.editorSession
        function onAdjustmentSnapshotChanged() {
            root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        }
    }
    // Also load on initial binding when editorSession changes.
    onEditorSessionChanged: {
        root.lastAppliedRevision = 0
        if (root.editorSession) {
            root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
        }
    }
}
