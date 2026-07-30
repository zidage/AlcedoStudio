import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Persistent left rail for the editor's independent transaction and Version
// panels. Only the active panel body is loaded; scroll offsets live on the rail
// so Loader teardown/recreation restores the prior viewport (Phase 7A R6).
//
// Fold motion: outer layout width snaps once (open or closed). Inner reveal is
// transform-only (x) driven by panelOpenProgress — never per-frame Layout width
// and never opacity on the history/Versions subtree.
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
    property Item blurSource: null

    // Scroll offsets stored outside Loader-owned panel bodies.
    property real historyListContentY: 0
    property real versionsListContentY: 0
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
                                         ? String(editorSession.historyPanelPage || "")
                                         : ""
    readonly property bool panelExpanded: activePage === "history" || activePage === "versions"
    readonly property int railWidth: 48
    readonly property int expandedPanelWidth: appTheme.editorSidePanelWidth
    readonly property int panelGap: appTheme.spaceSm

    // Visual fold progress (0 closed → 1 open). Drives transform only.
    property real panelOpenProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    // Layout width is binary: full while open or while a close animation still
    // holds intermediate progress; never interpolated every animation frame.
    readonly property bool layoutExpanded: root.panelExpanded || root.panelOpenProgress > 0.001
    readonly property real totalWidth: railWidth
                                       + (layoutExpanded ? (panelGap + expandedPanelWidth) : 0)
    readonly property real panelSlideX: (1.0 - panelOpenProgress) * (-expandedPanelWidth)

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
    }

    function applyBodyScrollRestore() {
        var body = panelBodyLoader.item
        if (!body || body.restoreListContentY === undefined)
            return
        var y = 0
        if (activePage === "history")
            y = historyListContentY
        else if (activePage === "versions")
            y = versionsListContentY
        body.restoreListContentY(y)
    }

    onActivePageChanged: {
        // Capture scroll of the body that is about to be destroyed, then track
        // which page the next Loader instance represents.
        captureBodyScroll()
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

    // Panel shell: full terminal width when layoutExpanded; content slides on x.
    Item {
        id: historyPanelHost
        objectName: "editorHistoryVersionsPanelHost"
        anchors.left: rail.right
        anchors.leftMargin: root.layoutExpanded ? root.panelGap : 0
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.layoutExpanded ? root.expandedPanelWidth : 0
        visible: root.layoutExpanded
        clip: true

        Rectangle {
            id: historyPanel
            objectName: "editorHistoryVersionsPanel"
            width: root.expandedPanelWidth
            height: parent.height
            x: root.panelSlideX
            radius: root.panelRadius
            color: root.colCardSurface
            border.width: 1
            border.color: root.colCardBorder
            // No opacity animation — transform-only reveal (R6).

            Loader {
                id: panelBodyLoader
                objectName: "editorHistoryVersionsBodyLoader"
                anchors.fill: parent
                // Load only while the session page names a body; closed rail owns
                // no transaction or Version delegates.
                active: root.activePage === "history" || root.activePage === "versions"
                asynchronous: false
                sourceComponent: root.activePage === "history" ? historyBodyComponent
                                 : (root.activePage === "versions" ? versionsBodyComponent
                                                                   : null)

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
}
