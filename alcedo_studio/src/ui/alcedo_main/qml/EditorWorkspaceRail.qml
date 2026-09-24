import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Persistent left rail for editor-wide tools plus the independent transaction
// and Version panels. Only the active panel body is loaded; scroll offsets live
// on the rail so Loader teardown/recreation restores the prior viewport.
//
// Fold motion matches the filmstrip: panelOpenProgress interpolates the rail's
// layout width so the center viewport grows and shrinks with the panel. The
// icon column stays put; the panel host clips a full-width body.
Item {
    id: root
    objectName: "editorWorkspaceRail"

    property var theme: null
    property var host: null
    property var editorSession: null
    property var interactionPolicy: null
    property var adjustmentTransfer: (typeof appModules !== "undefined" && appModules)
                                      ? appModules.adjustmentTransfer : null
    property bool recoveryPending: false
    property var historyModel: internalHistoryModel
    property Item blurSource: null

    // Scroll offsets stored outside Loader-owned panel bodies.
    property real historyListContentY: 0
    property real versionsListContentY: 0
    property real maskGroupsListContentY: 0
    property string _lastBodyPage: ""

    readonly property bool versionCheckoutEnabled: editorSession
                                                  ? !root.recoveryPending
                                                    && Boolean(editorSession.actions.canCheckoutVersion)
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
                                         ? String(editorSession.editorToolPanelPage || "")
                                         : ""
    readonly property bool panelExpanded: activePage === "history"
                                          || activePage === "versions"
                                          || activePage === "nodes"
                                          || activePage === "maskgroups"
    readonly property int railWidth: 68
    readonly property int expandedPanelWidth: {
        if (activePage === "nodes" || activePage === "maskgroups")
            return nodesLayoutStore.preferredPanelWidth
        return appTheme.editorSidePanelWidth
    }
    readonly property int panelGap: appTheme.spaceSm

    // Visual fold progress (0 closed → 1 open). Drives layout width + body
    // opacity, the same contract as EditorFilmstrip.dockExpandProgress.
    property real panelOpenProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    readonly property bool layoutExpanded: root.panelOpenProgress > 0.001
    readonly property real panelRevealWidth: (panelGap + expandedPanelWidth) * panelOpenProgress
    readonly property real totalWidth: railWidth + panelRevealWidth
    readonly property string bodyPage: {
        if (activePage === "history" || activePage === "versions" || activePage === "nodes"
                || activePage === "maskgroups")
            return activePage
        return _lastBodyPage
    }

    readonly property string statusMessage: {
        var body = panelBodyLoader.item
        if (body && body.statusMessage !== undefined)
            return String(body.statusMessage || "")
        return ""
    }

    // Diagnostics / tests: body loader and delegate presence.
    readonly property Item panelBodyItem: panelBodyLoader.item
    readonly property bool panelBodyActive: panelBodyLoader.active
    readonly property int panelBodyCreateCount: _panelBodyCreateCount
    readonly property int panelBodyDestroyCount: _panelBodyDestroyCount
    property int _panelBodyCreateCount: 0
    property int _panelBodyDestroyCount: 0

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

    EditorNodeLayoutStore {
        id: nodesLayoutStore
    }

    EditorNodeController {
        id: nodesController
        objectName: "editorNodeController"
        editorSession: root.editorSession
        layoutStore: nodesLayoutStore
    }

    readonly property alias nodeController: nodesController

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
        if (String(editorSession.editorToolPanelPage || "") === page) {
            editorSession.editorToolPanelPage = ""
        } else {
            editorSession.editorToolPanelPage = page
        }
    }

    function captureBodyScroll() {
        var body = panelBodyLoader.item
        if (!body)
            return
        var y = 0
        if (body.listContentY !== undefined)
            y = Number(body.listContentY || 0)
        if (_lastBodyPage === "history")
            historyListContentY = y
        else if (_lastBodyPage === "versions")
            versionsListContentY = y
        else if (_lastBodyPage === "maskgroups")
            maskGroupsListContentY = y
    }

    function applyBodyScrollRestore() {
        var body = panelBodyLoader.item
        if (!body || body.restoreListContentY === undefined)
            return
        var y = 0
        if (bodyPage === "history")
            y = historyListContentY
        else if (bodyPage === "versions")
            y = versionsListContentY
        else if (bodyPage === "maskgroups")
            y = maskGroupsListContentY
        body.restoreListContentY(y)
    }

    onActivePageChanged: {
        // Capture scroll of the body that is about to be destroyed or hidden.
        // Keep the last non-empty page so a closing fold can clip that body
        // until panelOpenProgress reaches 0.
        captureBodyScroll()
        if (activePage === "history" || activePage === "versions" || activePage === "nodes"
                || activePage === "maskgroups")
            _lastBodyPage = activePage
    }

    onPanelExpandedChanged: {
        _foldDuration = panelExpanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive)
            panelOpenProgress = panelExpanded ? 1 : 0
    }

    Component.onCompleted: {
        panelOpenProgress = panelExpanded ? 1 : 0
        _lastBodyPage = activePage
        _motionArmed = true
    }

    Behavior on panelOpenProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: appTheme.motionEasing
        }
    }

    Rectangle {
        id: rail
        objectName: "editorToolRail"
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
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: appTheme.spaceXs
            spacing: appTheme.spaceXs

            component RailButton: EditorRailButton {
                width: parent ? parent.width : implicitWidth
                textColor: root.colText
                mutedColor: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
            }

            RailButton {
                id: historyRailButton
                objectName: "editorHistoryRailButton"
                selected: root.activePage === "history"
                iconSrc: "qrc:/history_icons/git-commit-horizontal.svg"
                label: qsTr("History")
                actionName: selected ? qsTr("Hide Edit History") : qsTr("Show Edit History")
                onClicked: root.selectPage("history")
            }

            RailButton {
                id: versionsRailButton
                objectName: "editorVersionsRailButton"
                enabled: root.versionCheckoutEnabled
                selected: root.activePage === "versions"
                iconSrc: "qrc:/panel_icons/versions.svg"
                label: qsTr("Versions")
                actionName: selected ? qsTr("Hide Versions") : qsTr("Show Versions")
                onClicked: root.selectPage("versions")
            }

            RailButton {
                id: nodesRailButton
                objectName: "editorNodesRailButton"
                selected: root.activePage === "nodes"
                iconSrc: "qrc:/panel_icons/pipeline.svg"
                label: qsTr("Nodes")
                actionName: selected ? qsTr("Hide Nodes") : qsTr("Show Nodes")
                onClicked: root.selectPage("nodes")
            }

            RailButton {
                id: maskGroupsRailButton
                objectName: "editorMaskGroupsRailButton"
                selected: root.activePage === "maskgroups"
                iconSrc: "qrc:/panel_icons/nodes.svg"
                label: qsTr("Mask Groups")
                actionName: selected ? qsTr("Hide Mask Groups") : qsTr("Show Mask Groups")
                onClicked: root.selectPage("maskgroups")
            }

            // Separates the panel toggles above from the dialog launcher below.
            Rectangle {
                width: parent.width
                height: 1
                color: root.colCardBorder
            }

            RailButton {
                id: backgroundTasksRailButton
                objectName: "editorBackgroundTasksRailButton"
                enabled: root.host !== null
                iconSrc: "qrc:/panel_icons/clock-play.svg"
                label: qsTr("Tasks")
                actionName: qsTr("Background Tasks")
                onClicked: root.host.openBackgroundTasksDialog()
            }
        }
    }

    // Panel shell: host width tracks progress; the full-width body is clipped
    // so the viewport reflows with the fold (filmstrip mode).
    Item {
        id: historyPanelHost
        objectName: "editorToolPanelHost"
        anchors.left: rail.right
        anchors.leftMargin: root.panelGap * root.panelOpenProgress
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.expandedPanelWidth * root.panelOpenProgress
        visible: root.layoutExpanded
        opacity: root.panelOpenProgress
        clip: true

        Rectangle {
            id: historyPanel
            objectName: "editorToolPanel"
            width: root.expandedPanelWidth
            height: parent.height
            x: 0
            radius: root.panelRadius
            color: root.colCardSurface
            border.width: 1
            border.color: root.colCardBorder

            Loader {
                id: panelBodyLoader
                objectName: "editorToolBodyLoader"
                anchors.fill: parent
                // Keep the last body mounted while a close fold is in flight.
                // A fully closed rail (progress ≈ 0) owns no list or graph delegates.
                active: root.layoutExpanded
                        && (root.bodyPage === "history" || root.bodyPage === "versions"
                            || root.bodyPage === "nodes" || root.bodyPage === "maskgroups")
                asynchronous: false
                sourceComponent: {
                    if (root.bodyPage === "history")
                        return historyBodyComponent
                    if (root.bodyPage === "versions")
                        return versionsBodyComponent
                    if (root.bodyPage === "nodes")
                        return nodesBodyComponent
                    if (root.bodyPage === "maskgroups")
                        return maskGroupsBodyComponent
                    return null
                }

                onLoaded: {
                    root._panelBodyCreateCount += 1
                    root.applyBodyScrollRestore()
                }
            }
        }
    }

    Component {
        id: historyBodyComponent
        EditorHistoryTransactionsPanel {
            theme: root.theme
            editorSession: root.editorSession
            adjustmentTransfer: root.adjustmentTransfer
            historyModel: root.historyModel
            blurSource: root.blurSource
            Component.onDestruction: {
                // Capture final scroll if the rail still owns this page key.
                if (root._lastBodyPage === "history" && listContentY !== undefined)
                    root.historyListContentY = Number(listContentY || 0)
                root._panelBodyDestroyCount += 1
            }
        }
    }

    Component {
        id: versionsBodyComponent
        EditorVersionsPanel {
            theme: root.theme
            editorSession: root.editorSession
            historyModel: root.historyModel
            versionCheckoutEnabled: root.versionCheckoutEnabled
            versionCheckoutDisabledReason: root.versionCheckoutDisabledReason
            Component.onDestruction: {
                if (root._lastBodyPage === "versions" && listContentY !== undefined)
                    root.versionsListContentY = Number(listContentY || 0)
                root._panelBodyDestroyCount += 1
            }
        }
    }

    Component {
        id: nodesBodyComponent
        EditorNodesPanel {
            theme: root.theme
            editorSession: root.editorSession
            nodeController: nodesController
            nodeLayoutStore: nodesLayoutStore
            Component.onDestruction: {
                root._panelBodyDestroyCount += 1
            }
        }
    }

    Component {
        id: maskGroupsBodyComponent
        // Shares nodesController / nodesLayoutStore with the Nodes page: one
        // owner, two projections of the same PipelineDocument DAG.
        EditorMaskGroupsPanel {
            theme: root.theme
            editorSession: root.editorSession
            nodeController: nodesController
            nodeLayoutStore: nodesLayoutStore
            Component.onDestruction: {
                if (root._lastBodyPage === "maskgroups" && listContentY !== undefined)
                    root.maskGroupsListContentY = Number(listContentY || 0)
                root._panelBodyDestroyCount += 1
            }
        }
    }
}
