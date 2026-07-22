import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Left History / Versions rail for the editor desktop.
// The narrow rail always stays present; selecting History or Versions opens a
// panel beside the rail. Selecting the active action again collapses it.
// Panel content for Phase 4B is empty/disabled until later history port phases.
// Motion / surface tokens: DESIGN.md.
// Phase 4D: every surface fill is opaque (alpha 255); no parent-opacity,
// withAlpha(…), Qt.rgba(…, alpha), or "transparent" surface derivations.
Item {
    id: root
    objectName: "editorHistoryVersionsRail"

    property var theme: null
    property var editorSession: null
    // Phase 6C-5: Version checkout is disabled while a save checkpoint holds locks.
    property var navigationPolicy: null
    property bool controlsEnabled: navigationPolicy ? navigationPolicy.canCheckoutVersion : true
    readonly property string checkoutDisabledReason: navigationPolicy
                                                     ? String(navigationPolicy.checkoutVersionReason || "")
                                                     : ""

    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    // Card surface family — shared with the Library grid, adjustment shell,
    // viewport placeholder, and filmstrip. Disabled does not recolor the shell.
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property int panelRadius: theme ? theme.panelRadius : 12

    readonly property string activePage: editorSession
                                         ? String(editorSession.historyPanelPage || "")
                                         : ""
    readonly property bool panelExpanded: activePage === "history" || activePage === "versions"
    readonly property int railWidth: 48
    readonly property int expandedPanelWidth: appTheme.editorSidePanelWidth
    readonly property int panelGap: appTheme.spaceSm
    // panelOpenProgress drives the fold (0 collapsed -> 1 expanded). The
    // logical panelExpanded flips immediately so session-state assertions hold;
    // only the visual progress animates. _motionArmed suppresses the initial
    // snap so a recreated rail (workspace round-trip) appears in its persisted
    // state without replaying the open animation. reduceMotion snaps it.
    // foldManualDrive + driveFoldProgress() let tests pin intermediate geometry
    // without wall-clock sleeps (Phase 4C motion contract).
    property real panelOpenProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property int totalWidth: railWidth + (panelGap + expandedPanelWidth) * panelOpenProgress

    implicitWidth: totalWidth
    implicitHeight: 200
    Layout.preferredWidth: totalWidth
    Layout.minimumWidth: totalWidth
    Layout.maximumWidth: totalWidth
    Layout.fillHeight: true

    function driveFoldProgress(value) {
        foldManualDrive = true
        panelOpenProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        panelOpenProgress = panelExpanded ? 1 : 0
    }

    onPanelExpandedChanged: {
        _foldDuration = panelExpanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive) {
            panelOpenProgress = panelExpanded ? 1 : 0
        }
    }
    Component.onCompleted: {
        // Snap to the persisted expand state on load (no open animation).
        panelOpenProgress = panelExpanded ? 1 : 0
        _motionArmed = true
    }
    Behavior on panelOpenProgress {
        enabled: root._motionArmed && !root.foldManualDrive
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

    // Persistent narrow rail — never collapses away. Anchor-positioned at a
    // fixed width so it never participates in the fold's layout churn; only the
    // outer editor row relayouts as the root item width animates.
    Rectangle {
        id: rail
        objectName: "editorHistoryRail"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.railWidth
        radius: root.panelRadius
        // Phase 4D: opaque surface; disabled is a concrete color, not opacity.
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder

        Column {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: appTheme.spaceSm
            spacing: appTheme.spaceXs

            IconActionButton {
                id: historyRailButton
                objectName: "editorHistoryRailButton"
                compact: true
                enabled: root.controlsEnabled
                selected: root.activePage === "history"
                iconSrc: "qrc:/history_icons/git-commit-horizontal.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillSelected: appTheme.buttonSelectedFillColor
                actionName: selected ? qsTr("Hide Edit History") : qsTr("Show Edit History")
                onClicked: root.selectPage("history")
            }

            IconActionButton {
                id: versionsRailButton
                objectName: "editorVersionsRailButton"
                compact: true
                enabled: root.controlsEnabled
                selected: root.activePage === "versions"
                // Phase 4D: Tabler versions icon replaces palette.svg.
                iconSrc: "qrc:/panel_icons/versions.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillSelected: appTheme.buttonSelectedFillColor
                actionName: selected ? qsTr("Hide Versions") : qsTr("Show Versions")
                onClicked: root.selectPage("versions")
            }
        }
    }

    // Expanded panel beside the rail (takes space from the viewport). Width and
    // left margin track panelOpenProgress as Item geometry, not Layout
    // properties, so the fold animates without relayouting the rail's inner
    // row (the cause of the prior jank). The outer editor row still relayouts
    // as the root item width animates — the tested "takes space" contract —
    // and the panel's right edge stays flush with the root right edge at every
    // step: rail.right + gap*progress + panelWidth*progress == totalWidth.
    Rectangle {
        id: historyPanel
        objectName: "editorHistoryVersionsPanel"
        anchors.left: rail.right
        anchors.leftMargin: root.panelGap * root.panelOpenProgress
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.expandedPanelWidth * root.panelOpenProgress
        visible: root.panelOpenProgress > 0.001
        radius: root.panelRadius
        // Phase 4D: same card surface as the rail / adjustment shell / viewport.
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        // Fold opacity still rides on progress for the open/close animation;
        // the surface behind it is the same opaque color so no bleed-through.
        opacity: root.panelOpenProgress
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
