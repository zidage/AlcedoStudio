import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Right-side editor tools: histogram/waveform scope slot, adjustment navbar,
// and stacked panel bodies for Tone / Look / Display Transform / Geometry /
// RAW Decode. Phase 4B finalizes navigation, ordering, selection, collapse,
// and sizing; real controls arrive in Phase 6. Phase 4C adds a collapsible
// section shell that proves the shared fold motion contract.
Item {
    id: root
    objectName: "editorAdjustmentStack"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true

    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property color colBase: theme ? theme.colBgBase : "#161719"
    readonly property color colDeep: theme ? theme.colBgDeep : "#0C0D0F"
    // Card surface family — shared with the Library grid (see DESIGN.md).
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property int controlRadius: theme ? theme.controlRadius : 10

    // Bound to editorSession so selection survives Loader teardown.
    readonly property string activePanel: editorSession
                                          ? String(editorSession.activeAdjustmentPanel || "tone")
                                          : "tone"

    // Final sizing contract for Phase 4B.
    readonly property int preferredPanelWidth: 300
    readonly property int minimumPanelWidth: 260
    readonly property int maximumPanelWidth: 420

    implicitWidth: preferredPanelWidth
    implicitHeight: 400
    Layout.preferredWidth: preferredPanelWidth
    Layout.minimumWidth: minimumPanelWidth
    Layout.maximumWidth: maximumPanelWidth
    Layout.fillHeight: true

    function selectPanel(panel) {
        if (!editorSession) {
            return
        }
        editorSession.activeAdjustmentPanel = panel
    }

    function withAlpha(c, a) {
        return Qt.rgba(c.r, c.g, c.b, a)
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
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        opacity: root.controlsEnabled ? 1.0 : 0.55
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
                Layout.preferredHeight: 120
                Layout.minimumHeight: 96
                radius: appTheme.controlRadiusSmall
                color: "transparent"
                border.width: 1
                border.color: root.colCardBorder

                Label {
                    anchors.centerIn: parent
                    text: qsTr("Histogram / Waveform")
                    color: root.colMuted
                    font.pixelSize: appTheme.fontSizeBody
                }
            }

            // Segmented adjustment navbar (icon-only; tooltips carry the names).
            // Explicit buttons keep stable objectNames for tests (no Repeater).
            Rectangle {
                id: adjustmentNav
                objectName: "editorAdjustmentNav"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                radius: root.controlRadius
                color: root.colCardSurface
                border.width: 1
                border.color: root.colCardBorder

                component AdjustmentNavButton: IconActionButton {
                    property string panelKey: "tone"

                    // Compact optical size + stretch across the segmented row.
                    compact: true
                    stretchInLayout: true
                    selected: root.activePanel === panelKey
                    iconColorDefault: root.colMuted
                    iconColorSelected: root.colText
                    iconColorMuted: root.colMuted
                    fillIdle: "transparent"
                    fillHover: root.colHover
                    fillSelected: root.withAlpha(root.colHover, 0.55)
                    focusRingColor: root.colAccent
                    onClicked: root.selectPanel(panelKey)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 2
                    spacing: 0

                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_tone"
                        panelKey: "tone"
                        iconSrc: "qrc:/panel_icons/adjustments.svg"
                        actionName: qsTr("Tone")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_look"
                        panelKey: "look"
                        iconSrc: "qrc:/panel_icons/palette.svg"
                        actionName: qsTr("Look")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_display"
                        panelKey: "display"
                        iconSrc: "qrc:/panel_icons/color-filter.svg"
                        actionName: qsTr("Display Transform")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_geometry"
                        panelKey: "geometry"
                        iconSrc: "qrc:/panel_icons/crop.svg"
                        actionName: qsTr("Geometry")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_raw"
                        panelKey: "raw"
                        iconSrc: "qrc:/panel_icons/aperture.svg"
                        actionName: qsTr("RAW Decode")
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
                        CollapsibleSection {
                            id: groupShell
                            objectName: "editorAdjustmentGroupShell_" + panelKey
                            Layout.fillWidth: true
                            title: root.panelTitle(panelKey)
                            expanded: true
                            controlsEnabled: root.controlsEnabled
                            surfaceColor: "transparent"
                            borderColor: root.colCardBorder
                            textColor: root.colText
                            mutedColor: root.colMuted
                            hoverColor: root.colHover
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
                    objectName: "editorAdjustmentPanel_tone"
                    panelKey: "tone"
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
}
