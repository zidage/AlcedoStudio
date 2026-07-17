import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Right-side editor tools: histogram/waveform scope slot, adjustment navbar,
// and stacked panel bodies for Tone / Look / Display Transform / Geometry /
// RAW Decode. Phase 4B finalizes navigation, ordering, selection, collapse,
// and sizing; panel bodies may stay empty until later port phases.
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
            return qsTr("Select an image to enable adjustment controls")
        }
        return qsTr("%1 controls will appear here").arg(panelTitle(key))
    }

    Rectangle {
        id: panelShell
        objectName: "editorRightPanelSlot"
        anchors.fill: parent
        radius: root.panelRadius
        color: root.colPanel
        border.width: 1
        border.color: root.colStroke
        opacity: root.controlsEnabled ? 1.0 : 0.55
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            // Histogram / waveform placement stays with the right-side tools.
            Rectangle {
                id: scopeSlot
                objectName: "editorScopeSlot"
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                Layout.minimumHeight: 96
                radius: 8
                color: root.colDeep
                border.width: 1
                border.color: root.colStroke

                Label {
                    anchors.centerIn: parent
                    text: qsTr("Histogram / Waveform")
                    color: root.colMuted
                    font.pixelSize: 12
                }
            }

            // Segmented adjustment navbar (icon-only; tooltips carry the names).
            // Explicit buttons keep stable objectNames for tests (no Repeater).
            Rectangle {
                id: adjustmentNav
                objectName: "editorAdjustmentNav"
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: root.controlRadius
                color: root.colBase
                border.width: 1
                border.color: root.colStroke

                component AdjustmentNavButton: Button {
                    id: navBtn
                    property string panelKey: "tone"
                    property string iconSrc: ""
                    property string tip: ""

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    flat: true
                    padding: 0
                    display: AbstractButton.IconOnly
                    enabled: true
                    activeFocusOnTab: true
                    readonly property bool isActive: root.activePanel === panelKey
                    HoverHandler { id: navHover }
                    readonly property int highlightLevel: !enabled ? 0
                                                          : (down ? 2
                                                          : (navHover.hovered ? 1 : 0))
                    readonly property bool focusRingVisible: enabled && activeFocus
                    icon.source: iconSrc
                    icon.width: 16
                    icon.height: 16
                    icon.color: !enabled ? root.withAlpha(root.colMuted, 0.55)
                               : (isActive ? root.colText : root.colMuted)
                    Material.foreground: icon.color
                    background: Rectangle {
                        radius: Math.max(4, root.controlRadius - 2)
                        color: navBtn.isActive
                               ? root.withAlpha(root.colHover, 0.55)
                               : navBtn.highlightLevel === 2
                                 ? root.withAlpha(root.colHover, 0.40)
                                 : navBtn.highlightLevel === 1
                                   ? root.withAlpha(root.colHover, 0.22)
                                   : "transparent"
                        border.width: navBtn.focusRingVisible ? 1 : 0
                        border.color: root.withAlpha(root.colAccent, 0.60)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: tip
                    Accessible.name: tip
                    Accessible.role: Accessible.Button
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
                        tip: qsTr("Tone")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_look"
                        panelKey: "look"
                        iconSrc: "qrc:/panel_icons/palette.svg"
                        tip: qsTr("Look")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_display"
                        panelKey: "display"
                        iconSrc: "qrc:/panel_icons/color-filter.svg"
                        tip: qsTr("Display Transform")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_geometry"
                        panelKey: "geometry"
                        iconSrc: "qrc:/panel_icons/crop.svg"
                        tip: qsTr("Geometry")
                    }
                    AdjustmentNavButton {
                        objectName: "editorAdjustmentNav_raw"
                        panelKey: "raw"
                        iconSrc: "qrc:/panel_icons/aperture.svg"
                        tip: qsTr("RAW Decode")
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
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: root.panelTitle(panelKey)
                            color: root.colText
                            font.pixelSize: 13
                            font.weight: 700
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 8
                            color: "transparent"
                            border.width: 1
                            border.color: root.colStroke

                            Label {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                                text: root.panelEmptyHint(panelKey)
                                color: root.colMuted
                                font.pixelSize: 12
                            }
                        }
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
