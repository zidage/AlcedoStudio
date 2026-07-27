import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Persistent left rail for the editor's independent transaction and Version
// panels. The rail controls which panel is open; each panel owns its content.
Item {
    id: root
    objectName: "editorHistoryVersionsRail"

    property var theme: null
    property var editorSession: null
    property var interactionPolicy: null
    property var adjustmentTransfer: (typeof appModules !== "undefined" && appModules)
                                      ? appModules.adjustmentTransfer : null
    property bool recoveryPending: false
    property var historyModel: internalHistoryModel

    readonly property bool versionCheckoutEnabled: interactionPolicy
                                                  ? !root.recoveryPending
                                                    && Boolean(interactionPolicy.canCheckoutVersion)
                                                  : !root.recoveryPending
    readonly property string versionCheckoutDisabledReason: root.recoveryPending
                                                            ? qsTr("Resolve the editor save first")
                                                            : (interactionPolicy
                                                               ? String(interactionPolicy.checkoutVersionReason || "")
                                                               : "")

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property int panelRadius: theme ? theme.panelRadius : appTheme.panelRadius

    readonly property string activePage: editorSession
                                         ? String(editorSession.historyPanelPage || "")
                                         : ""
    readonly property bool panelExpanded: activePage === "history" || activePage === "versions"
    readonly property int railWidth: 48
    readonly property int expandedPanelWidth: appTheme.editorSidePanelWidth
    readonly property int panelGap: appTheme.spaceSm
    property real panelOpenProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property real totalWidth: railWidth + (panelGap + expandedPanelWidth) * panelOpenProgress
    readonly property string statusMessage: historyTransactionsPanel.statusMessage

    implicitWidth: totalWidth
    implicitHeight: 200
    Layout.preferredWidth: totalWidth
    Layout.minimumWidth: totalWidth
    Layout.maximumWidth: totalWidth
    Layout.fillHeight: true

    EditorHistoryModel {
        id: internalHistoryModel
        editorSession: root.editorSession
    }

    function driveFoldProgress(value) {
        foldManualDrive = true
        panelOpenProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        panelOpenProgress = panelExpanded ? 1 : 0
    }

    function selectPage(page) {
        if (!editorSession) return
        if (String(editorSession.historyPanelPage || "") === page) {
            editorSession.historyPanelPage = ""
        } else {
            editorSession.historyPanelPage = page
        }
    }

    onPanelExpandedChanged: {
        _foldDuration = panelExpanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive) panelOpenProgress = panelExpanded ? 1 : 0
    }

    Component.onCompleted: {
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

    Rectangle {
        id: rail
        objectName: "editorHistoryRail"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.railWidth
        radius: root.panelRadius
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
                enabled: true
                selected: root.activePage === "history"
                selectedOutline: true
                selectedOutlineColor: root.colText
                iconSrc: "qrc:/history_icons/git-commit-horizontal.svg"
                iconColorDefault: selected ? root.colText : root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: root.colCardSurface
                focusRingColor: root.colText
                actionName: selected ? qsTr("Hide Edit History") : qsTr("Show Edit History")
                onClicked: root.selectPage("history")
            }

            IconActionButton {
                id: versionsRailButton
                objectName: "editorVersionsRailButton"
                compact: true
                enabled: root.versionCheckoutEnabled
                selected: root.activePage === "versions"
                selectedOutline: true
                selectedOutlineColor: root.colText
                iconSrc: "qrc:/panel_icons/versions.svg"
                iconColorDefault: selected ? root.colText : root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: root.colCardSurface
                focusRingColor: root.colText
                actionName: selected ? qsTr("Hide Versions") : qsTr("Show Versions")
                onClicked: root.selectPage("versions")
            }
        }
    }

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
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        opacity: root.panelOpenProgress
        clip: true

        EditorHistoryTransactionsPanel {
            id: historyTransactionsPanel
            anchors.fill: parent
            theme: root.theme
            editorSession: root.editorSession
            adjustmentTransfer: root.adjustmentTransfer
            historyModel: root.historyModel
            visible: root.activePage === "history"
        }

        EditorVersionsPanel {
            id: versionsPanel
            anchors.fill: parent
            theme: root.theme
            editorSession: root.editorSession
            historyModel: root.historyModel
            versionCheckoutEnabled: root.versionCheckoutEnabled
            versionCheckoutDisabledReason: root.versionCheckoutDisabledReason
            visible: root.activePage === "versions"
        }
    }
}
