import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Effects

// Left History / Versions rail for the editor desktop.
// The narrow rail always stays present; selecting History or Versions opens a
// panel beside the rail. Selecting the active action again collapses it.
// Panel content for Phase 4B is empty/disabled until later history port phases.
Item {
    id: root
    objectName: "editorHistoryVersionsRail"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true

    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property color colDeep: theme ? theme.colBgDeep : "#0C0D0F"
    readonly property color colBase: theme ? theme.colBgBase : "#161719"
    // Card surface family — shared with the Library grid (ThumbnailGridView) so
    // the rail and expanded panel never introduce locally darker/lighter fills.
    // See src/ui/alcedo_main/DESIGN.md.
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property int controlRadius: theme ? theme.controlRadius : 10

    readonly property string activePage: editorSession
                                         ? String(editorSession.historyPanelPage || "")
                                         : ""
    readonly property bool panelExpanded: activePage === "history" || activePage === "versions"
    readonly property int railWidth: 60
    readonly property int expandedPanelWidth: 300
    readonly property int panelGap: appTheme.spaceSm
    // panelOpenProgress drives the fold (0 collapsed -> 1 expanded). The
    // logical panelExpanded flips immediately so session-state assertions hold;
    // only the visual progress animates. _motionArmed suppresses the initial
    // snap so a recreated rail (workspace round-trip) appears in its persisted
    // state without replaying the open animation. reduceMotion snaps it.
    property real panelOpenProgress: 0
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property int totalWidth: railWidth + (panelGap + expandedPanelWidth) * panelOpenProgress

    implicitWidth: totalWidth
    implicitHeight: 200
    Layout.preferredWidth: totalWidth
    Layout.minimumWidth: totalWidth
    Layout.maximumWidth: totalWidth
    Layout.fillHeight: true

    onPanelExpandedChanged: {
        _foldDuration = panelExpanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        panelOpenProgress = panelExpanded ? 1 : 0
    }
    Component.onCompleted: {
        // Snap to the persisted expand state on load (no open animation).
        panelOpenProgress = panelExpanded ? 1 : 0
        _motionArmed = true
    }
    Behavior on panelOpenProgress {
        enabled: root._motionArmed
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: Easing.OutCubic
        }
    }

    function selectPage(page) {
        if (!editorSession) {
            return
        }
        if (String(editorSession.historyPanelPage || "") === page) {
            editorSession.historyPanelPage = ""
        } else {
            editorSession.historyPanelPage = page
        }
    }

    function withAlpha(c, a) {
        return Qt.rgba(c.r, c.g, c.b, a)
    }

    RowLayout {
        anchors.fill: parent
        spacing: root.panelGap * root.panelOpenProgress

        // Persistent narrow rail — never collapses away.
        Rectangle {
            id: rail
            objectName: "editorHistoryRail"
            Layout.preferredWidth: root.railWidth
            Layout.minimumWidth: root.railWidth
            Layout.maximumWidth: root.railWidth
            Layout.fillHeight: true
            radius: root.panelRadius
            color: root.colCardSurface
            border.width: 1
            border.color: root.colCardBorder
            opacity: root.controlsEnabled ? 1.0 : 0.55

            Column {
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: appTheme.spaceMd
                spacing: appTheme.spaceSm

                // History
                Button {
                    id: historyRailButton
                    objectName: "editorHistoryRailButton"
                    width: 46
                    height: 46
                    flat: true
                    padding: 0
                    display: AbstractButton.IconOnly
                    enabled: root.controlsEnabled
                    activeFocusOnTab: true
                    readonly property bool isActive: root.activePage === "history"
                    HoverHandler { id: historyHover }
                    readonly property int highlightLevel: !enabled ? 0
                                                          : (down ? 2
                                                          : (historyHover.hovered ? 1 : 0))
                    readonly property bool focusRingVisible: enabled && activeFocus
                    icon.source: "qrc:/history_icons/git-commit-horizontal.svg"
                    icon.width: appTheme.iconOpticalSize
                    icon.height: appTheme.iconOpticalSize
                    icon.color: !enabled ? root.withAlpha(root.colMuted, 0.55)
                               : (isActive ? root.colText : root.colMuted)
                    Material.foreground: icon.color
                    background: Rectangle {
                        radius: root.controlRadius
                        color: historyRailButton.isActive
                               ? root.colBase
                               : historyRailButton.highlightLevel === 2
                                 ? root.withAlpha(root.colHover, 0.55)
                                 : historyRailButton.highlightLevel === 1
                                   ? root.withAlpha(root.colHover, 0.30)
                                   : root.withAlpha(root.colBase, 0.55)
                        border.width: historyRailButton.focusRingVisible ? 1 : 0
                        border.color: root.withAlpha(root.colAccent, 0.60)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: historyRailButton.isActive
                                  ? qsTr("Hide Edit History")
                                  : qsTr("Show Edit History")
                    Accessible.name: ToolTip.text
                    Accessible.role: Accessible.Button
                    onClicked: root.selectPage("history")
                }

                // Versions
                Button {
                    id: versionsRailButton
                    objectName: "editorVersionsRailButton"
                    width: 46
                    height: 46
                    flat: true
                    padding: 0
                    display: AbstractButton.IconOnly
                    enabled: root.controlsEnabled
                    activeFocusOnTab: true
                    readonly property bool isActive: root.activePage === "versions"
                    HoverHandler { id: versionsHover }
                    readonly property int highlightLevel: !enabled ? 0
                                                          : (down ? 2
                                                          : (versionsHover.hovered ? 1 : 0))
                    readonly property bool focusRingVisible: enabled && activeFocus
                    icon.source: "qrc:/panel_icons/palette.svg"
                    icon.width: appTheme.iconOpticalSize
                    icon.height: appTheme.iconOpticalSize
                    icon.color: !enabled ? root.withAlpha(root.colMuted, 0.55)
                               : (isActive ? root.colText : root.colMuted)
                    Material.foreground: icon.color
                    background: Rectangle {
                        radius: root.controlRadius
                        color: versionsRailButton.isActive
                               ? root.colBase
                               : versionsRailButton.highlightLevel === 2
                                 ? root.withAlpha(root.colHover, 0.55)
                                 : versionsRailButton.highlightLevel === 1
                                   ? root.withAlpha(root.colHover, 0.30)
                                   : root.withAlpha(root.colBase, 0.55)
                        border.width: versionsRailButton.focusRingVisible ? 1 : 0
                        border.color: root.withAlpha(root.colAccent, 0.60)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: versionsRailButton.isActive
                                  ? qsTr("Hide Versions")
                                  : qsTr("Show Versions")
                    Accessible.name: ToolTip.text
                    Accessible.role: Accessible.Button
                    onClicked: root.selectPage("versions")
                }
            }
        }

        // Expanded panel beside the rail (takes space from the viewport). Width
        // and opacity track panelOpenProgress so the panel folds quietly; the
        // rail stays stationary and content is clipped mid-transition.
        Rectangle {
            id: historyPanel
            objectName: "editorHistoryVersionsPanel"
            visible: root.panelOpenProgress > 0.001
            Layout.preferredWidth: root.expandedPanelWidth * root.panelOpenProgress
            Layout.minimumWidth: 0
            Layout.maximumWidth: root.expandedPanelWidth
            Layout.fillHeight: true
            radius: root.panelRadius
            color: root.colCardSurface
            border.width: 1
            border.color: root.colCardBorder
            opacity: root.panelOpenProgress * (root.controlsEnabled ? 1.0 : 0.55)
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                spacing: appTheme.spaceMd

                Label {
                    objectName: "editorHistoryVersionsPanelTitle"
                    Layout.fillWidth: true
                    text: root.activePage === "versions" ? qsTr("Versions") : qsTr("Edit History")
                    color: root.colText
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                }

                // History page body (empty until history port).
                Item {
                    objectName: "editorHistoryPageBody"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.activePage === "history"

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 16
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        text: root.controlsEnabled
                              ? qsTr("No edit history yet")
                              : qsTr("Select an image to view history")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeBody
                    }
                }

                // Versions page body (empty until versioning port).
                Item {
                    objectName: "editorVersionsPageBody"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.activePage === "versions"

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 16
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        text: root.controlsEnabled
                              ? qsTr("No versions yet")
                              : qsTr("Select an image to view versions")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeBody
                    }
                }
            }
        }
    }
}
