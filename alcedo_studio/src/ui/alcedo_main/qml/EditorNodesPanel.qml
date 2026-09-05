import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuickQanava 2.0 as Qan
import Alcedo.Main 1.0

// Nodes page: header plus a navigable QuickQanava canvas. Product selection
// lives on EditorNodeController. Positions, zoom, view, and Mask drawers live
// on EditorNodeLayoutStore. Product changes route through EditorNodeController.
Item {
    id: root
    objectName: "editorNodesPageBody"

    property var theme: null
    property var editorSession: null
    property var nodeController: null
    property var nodeLayoutStore: null

    property bool renameVisible: false
    property string renameNodeId: ""
    property string renameOriginalName: ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property string layoutIdentityKey: {
        if (!root.nodeController) {
            return ""
        }
        return String(root.nodeController.elementId) + ":"
                + String(root.nodeController.imageId) + ":"
                + String(root.nodeController.versionId)
    }
    readonly property string sessionStateName: root.editorSession
            ? String(root.editorSession.sessionState || "")
            : ""
    readonly property bool graphReady: root.nodeController && root.nodeController.hasSnapshot
    readonly property bool graphLoading: !root.graphReady
            && (root.sessionStateName === "Loading"
                || root.sessionStateName === "Acquiring"
                || root.sessionStateName === "Switching")
    readonly property bool noImage: !root.graphReady && !root.graphLoading
    readonly property bool pendingCommand: root.nodeController
            && root.nodeController.commandActive
    readonly property bool keyboardConnectActive: qanAdapter
            && qanAdapter.keyboardConnectActive

    function addColorGrade() {
        if (!root.graphReady || !root.nodeController || !root.nodeController.canAddColorGrade) {
            return
        }
        root.nodeController.addCleanColorGrade()
    }

    function beginRename() {
        if (!root.nodeController
                || !root.nodeController.canRenameSelectedColorGrade) {
            return
        }
        root.renameNodeId = root.nodeController.selectedNodeId
        root.renameOriginalName = root.nodeController.selectedNodeName
        renameField.text = root.renameOriginalName
        root.renameVisible = true
        Qt.callLater(function () {
            if (root.renameVisible) {
                renameField.forceActiveFocus()
                renameField.selectAll()
            }
        })
    }

    function cancelRename() {
        root.renameVisible = false
        root.renameNodeId = ""
        root.renameOriginalName = ""
        renameField.text = ""
        graphView.forceActiveFocus()
    }

    function commitRename() {
        if (!root.renameVisible || !root.nodeController) {
            return
        }
        const name = renameField.text.trim()
        if (name.length === 0) {
            return
        }
        if (name === root.renameOriginalName) {
            cancelRename()
            return
        }
        if (root.nodeController.renameColorGrade(root.renameNodeId, name)) {
            cancelRename()
        }
    }

    function deleteSelectedColorGrade() {
        if (!root.nodeController
                || !root.nodeController.canDeleteSelectedColorGrade) {
            return
        }
        if (root.renameVisible) {
            cancelRename()
        }
        root.nodeController.deleteColorGrade(root.nodeController.selectedNodeId)
        graphView.forceActiveFocus()
    }

    function captureView() {
        if (!graphView || !root.nodeLayoutStore) {
            return
        }
        root.nodeLayoutStore.zoom = graphView.zoom
        if (graphView.containerItem) {
            root.nodeLayoutStore.viewPosition = Qt.point(graphView.containerItem.x,
                                                         graphView.containerItem.y)
        }
    }

    function restoreGraphView() {
        if (!graphView || !root.nodeLayoutStore) {
            return
        }
        graphView.zoom = root.nodeLayoutStore.zoom
        if (graphView.containerItem) {
            graphView.containerItem.x = root.nodeLayoutStore.viewPosition.x
            graphView.containerItem.y = root.nodeLayoutStore.viewPosition.y
        }
    }

    function attachAdapter() {
        if (!root.nodeController || !qanAdapter) {
            return
        }
        root.nodeController.graphAdapter = qanAdapter
    }

    function detachAdapter() {
        if (root.nodeController) {
            root.nodeController.graphAdapter = null
        }
    }

    function fitGraph() {
        if (!graphView || !root.graphReady) {
            return
        }
        graphView.fitContentInView()
        captureView()
    }

    function startKeyboardConnect() {
        if (!root.graphReady || !root.nodeController || !qanAdapter) {
            return
        }
        if (root.renameVisible) {
            return
        }
        qanAdapter.beginKeyboardConnect(root.nodeController.selectedNodeId)
    }

    function completeKeyboardConnect() {
        if (!root.keyboardConnectActive || !root.nodeController || !qanAdapter) {
            return
        }
        const source = qanAdapter.keyboardConnectSourceId
        const destination = root.nodeController.selectedNodeId
        qanAdapter.cancelKeyboardConnect()
        if (source.length > 0 && destination.length > 0) {
            root.nodeController.requestConnect(source, destination)
        }
        graphView.forceActiveFocus()
    }

    function cancelConnectorOrRename() {
        if (root.renameVisible) {
            root.cancelRename()
            return
        }
        if (qanAdapter && qanAdapter.keyboardConnectActive) {
            qanAdapter.cancelKeyboardConnect()
        } else if (qanAdapter) {
            qanAdapter.hideConnectorPreview()
        }
        graphView.forceActiveFocus()
    }

    // Graph-scoped product keys. Command ids live on ShortcutRegistry.
    // Masks header Enter/Space stay on EditorNodeMaskDrawer. Add Enter/Space
    // stay on IconActionButton. Tab order is KeyNavigation on Add and GraphView.
    function handleGraphKey(event) {
        if (!root.nodeController || !root.graphReady) {
            return
        }
        const id = ShortcutRegistry.commandIdForKey(event.key, event.modifiers)
        if (id === "nodes.addColorGrade") {
            root.addColorGrade()
        } else if (id === "nodes.fitGraph") {
            root.fitGraph()
        } else if (id === "nodes.renameColorGrade") {
            root.beginRename()
        } else if (id === "nodes.deleteColorGrade") {
            root.deleteSelectedColorGrade()
        } else if (id === "nodes.beginConnect") {
            root.startKeyboardConnect()
        } else if (id === "nodes.completeConnect") {
            if (!root.keyboardConnectActive) {
                return
            }
            root.completeKeyboardConnect()
        } else if (id === "nodes.selectPrevious") {
            root.nodeController.selectPreviousBackboneNode()
        } else if (id === "nodes.selectNext") {
            root.nodeController.selectNextBackboneNode()
        } else if (id === "nodes.selectDevelop") {
            root.nodeController.selectDevelop()
        } else if (id === "nodes.selectDrt") {
            root.nodeController.selectDrt()
        } else if (id === "nodes.cancel") {
            root.cancelConnectorOrRename()
        } else {
            return
        }
        event.accepted = true
    }

    onLayoutIdentityKeyChanged: restoreGraphView()

    Connections {
        target: root.nodeController
        function onSelectionChanged() {
            if (root.renameVisible
                    && root.renameNodeId !== root.nodeController.selectedNodeId) {
                root.cancelRename()
            }
        }
        function onSnapshotChanged() {
            root.restoreGraphView()
        }
    }

    Connections {
        target: qanAdapter
        function onNodeDrawerOpenChanged(nodeId, open) {
            if (root.nodeLayoutStore) {
                root.nodeLayoutStore.setDrawerOpen(nodeId, open)
            }
        }
        function onGraphChanged() {
            root.attachAdapter()
        }
    }

    Component.onCompleted: {
        attachAdapter()
        restoreGraphView()
        if (graphView.originCross) {
            graphView.originCross.visible = false
        }
        graphView.vScrollBar.policy = ScrollBar.AlwaysOff
        graphView.hScrollBar.policy = ScrollBar.AlwaysOff
    }

    Component.onDestruction: {
        captureView()
        detachAdapter()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            objectName: "editorNodesPanelHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                objectName: "editorNodesPanelTitle"
                Layout.fillWidth: true
                text: qsTr("Nodes")
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Nodes")
            }

            IconActionButton {
                id: addButton
                objectName: "editorNodesAddButton"
                compact: true
                enabled: root.graphReady && root.nodeController
                         && root.nodeController.canAddColorGrade
                iconSrc: "qrc:/panel_icons/plus.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Add Color Grade")
                toolTipText: ShortcutRegistry.decorateTooltip(qsTr("Add Color Grade"),
                                                             "nodes.addColorGrade")
                KeyNavigation.tab: graphView
                onClicked: root.addColorGrade()
            }
        }

        RowLayout {
            objectName: "editorNodeRenameRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.renameVisible ? appTheme.iconButtonHitSizeCompact : 0
            spacing: appTheme.spaceSm
            visible: root.renameVisible

            TextField {
                id: renameField
                objectName: "editorNodeRenameField"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                enabled: root.renameVisible && root.nodeController
                         && !root.nodeController.commandActive
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                placeholderText: qsTr("Color Grade name")
                selectByMouse: true
                leftPadding: appTheme.spaceSm
                rightPadding: appTheme.spaceSm
                Accessible.name: qsTr("Rename Color Grade")
                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: renameField.activeFocus ? root.colText : root.colCardBorder
                }
                onAccepted: root.commitRename()
                Keys.onEscapePressed: function (event) {
                    root.cancelRename()
                    event.accepted = true
                }
            }

            IconActionButton {
                objectName: "editorNodeRenameAcceptButton"
                compact: true
                enabled: root.renameVisible && root.nodeController
                         && !root.nodeController.commandActive
                         && renameField.text.trim().length > 0
                iconSrc: "qrc:/panel_icons/edit.svg"
                iconColorDefault: root.colText
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Accept Rename")
                onClicked: root.commitRename()
            }
        }

        Label {
            objectName: "editorNodesPendingCommand"
            Layout.fillWidth: true
            visible: root.pendingCommand
            text: qsTr("Updating node graph")
            color: root.colMuted
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Updating node graph")
        }

        Label {
            objectName: "editorNodesEditingGuidance"
            Layout.fillWidth: true
            visible: (root.nodeController && root.nodeController.incompleteDraft)
                     || root.keyboardConnectActive
            text: {
                if (root.keyboardConnectActive) {
                    return qsTr("Select a destination node and press Enter")
                }
                return root.nodeController ? root.nodeController.incompleteDraftInstruction : ""
            }
            color: root.colMuted
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Label {
            objectName: "editorNodesCommandError"
            Layout.fillWidth: true
            visible: root.nodeController && root.nodeController.lastError.length > 0
            text: root.nodeController ? root.nodeController.lastError : ""
            color: appTheme.dangerColor
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Rectangle {
            id: canvasHost
            objectName: "editorNodesCanvasHost"
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: appTheme.controlRadiusSmall
            color: appTheme.graphCanvasColor
            border.width: 1
            border.color: graphView.activeFocus ? root.colText : root.colCardBorder
            clip: true

            Label {
                objectName: "editorNodesEmptyState"
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                visible: root.noImage
                text: qsTr("Select an image to edit nodes")
                color: root.colMuted
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Select an image to edit nodes")
            }

            Label {
                objectName: "editorNodesLoadingState"
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                visible: root.graphLoading
                text: qsTr("Loading node graph")
                color: root.colMuted
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Loading node graph")
            }

            Qan.GraphView {
                id: graphView
                objectName: "editorNodesGraphView"
                anchors.fill: parent
                anchors.margins: 1
                navigable: true
                selectionRectEnabled: false
                // Uniform canvas, no grid: assigning null swaps in QuickQanava's
                // empty default grid (Navigable::setGrid), so nothing is painted
                // behind the nodes.
                grid: null
                visible: root.graphReady
                enabled: root.graphReady
                focus: root.graphReady
                activeFocusOnTab: root.graphReady
                Accessible.role: Accessible.Canvas
                Accessible.name: root.keyboardConnectActive
                                 ? qsTr("Nodes graph, connecting")
                                 : qsTr("Nodes graph")
                Accessible.description: root.pendingCommand
                                        ? qsTr("Updating node graph")
                                        : ""
                Accessible.ignored: !root.graphReady
                KeyNavigation.backtab: addButton
                Keys.priority: Keys.BeforeItem

                graph: Qan.Graph {
                    id: graphTopology
                    objectName: "editorNodesQanGraph"
                    multipleSelectionEnabled: false
                    connectorEnabled: true
                    connectorCreateDefaultEdge: false
                    connectorEdgeColor: appTheme.graphCandidateEdgeColor
                    connectorColor: appTheme.graphPortBorderColor
                }

                Keys.onPressed: function (event) {
                    root.handleGraphKey(event)
                }

                onNavigated: root.captureView()
                onNodeClicked: function (node) {
                    if (!root.nodeController || !qanAdapter) {
                        return
                    }
                    const id = qanAdapter.liveNodeId(node)
                    if (id.length > 0) {
                        root.nodeController.selectNode(id)
                    }
                }
                onNodeRightClicked: function (node, position) {
                    if (!root.nodeController || !qanAdapter || !node || !node.item) {
                        return
                    }
                    const id = qanAdapter.liveNodeId(node)
                    if (id.length === 0) {
                        return
                    }
                    root.nodeController.selectNode(id)
                    const menuPosition = node.item.mapToItem(canvasHost,
                                                              position.x, position.y)
                    nodeMenu.openAt(menuPosition.x, menuPosition.y)
                }
                onRightClicked: function () {
                    canvasMenu.popup()
                }
            }

            Connections {
                target: graphView.graph
                enabled: graphView.graph !== null
                function onNodeMoved(node) {
                    if (!root.nodeLayoutStore || !qanAdapter || !node || !node.item) {
                        return
                    }
                    const id = qanAdapter.liveNodeId(node)
                    if (id.length > 0) {
                        root.nodeLayoutStore.setNodePosition(id, node.item.x, node.item.y)
                    }
                }
            }

            AlcedoQanGraph {
                id: qanAdapter
                objectName: "editorNodesQanAdapter"
                graph: graphView.graph
                colorGradeDelegateUrl: Qt.resolvedUrl("EditorNodeDelegate.qml")
                endpointDelegateUrl: Qt.resolvedUrl("EditorEndpointNodeDelegate.qml")
                portDelegateUrl: Qt.resolvedUrl("EditorNodePortDelegate.qml")
                portDockDelegateUrl: Qt.resolvedUrl("EditorNodePortDock.qml")
                edgeDelegateUrl: Qt.resolvedUrl("EditorNodeEdgeDelegate.qml")
            }

            AppContextMenu {
                id: canvasMenu
                objectName: "editorNodesCanvasMenu"

                AppMenuItem {
                    objectName: "editorNodesFitMenuItem"
                    text: qsTr("Fit")
                    Accessible.name: qsTr("Fit")
                    onTriggered: root.fitGraph()
                }
            }

            AppContextMenu {
                id: nodeMenu
                objectName: "editorNodesNodeMenu"

                AppMenuItem {
                    objectName: "editorNodesRenameMenuItem"
                    text: qsTr("Rename Color Grade")
                    Accessible.name: qsTr("Rename Color Grade")
                    enabled: root.nodeController
                             ? root.nodeController.canRenameSelectedColorGrade : false
                    onTriggered: root.beginRename()
                }

                AppMenuItem {
                    objectName: "editorNodesDeleteMenuItem"
                    text: qsTr("Delete Color Grade")
                    Accessible.name: qsTr("Delete Color Grade")
                    enabled: root.nodeController
                             ? root.nodeController.canDeleteSelectedColorGrade : false
                    onTriggered: root.deleteSelectedColorGrade()
                }
            }
        }
    }
}
