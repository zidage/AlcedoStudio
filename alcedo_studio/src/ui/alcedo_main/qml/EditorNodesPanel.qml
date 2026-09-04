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

    onLayoutIdentityKeyChanged: activateLayoutKey()

    function addColorGrade() {
        if (!root.nodeController || !root.nodeController.canAddColorGrade) {
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

    function applyLayout() {
        if (!root.nodeController || !root.nodeLayoutStore || !qanAdapter) {
            return
        }
        root.nodeLayoutStore.ensureDefaultsFrom(root.nodeController)
        const ids = root.nodeController.backboneNodeIds
        for (var i = 0; i < ids.length; ++i) {
            const id = ids[i]
            if (root.nodeLayoutStore.hasNodePosition(id)) {
                const pos = root.nodeLayoutStore.nodePosition(id)
                qanAdapter.setNodePosition(id, pos.x, pos.y)
            }
            qanAdapter.setDrawerOpen(id, root.nodeLayoutStore.drawerOpen(id))
        }
        if (graphView) {
            graphView.zoom = root.nodeLayoutStore.zoom
            if (graphView.containerItem) {
                graphView.containerItem.x = root.nodeLayoutStore.viewPosition.x
                graphView.containerItem.y = root.nodeLayoutStore.viewPosition.y
            }
        }
        qanAdapter.applyProductSelection(root.nodeController.selectedNodeId)
    }

    function bindControllerAdapter() {
        if (!root.nodeController) {
            return
        }
        if (qanAdapter && qanAdapter.graph) {
            root.nodeController.graphAdapter = qanAdapter
        }
    }

    function clearControllerAdapter() {
        if (root.nodeController) {
            root.nodeController.graphAdapter = null
        }
    }

    Binding {
        target: root.nodeController
        property: "graphAdapter"
        value: (qanAdapter && qanAdapter.graph) ? qanAdapter : null
        when: root.nodeController !== null && root.nodeController !== undefined
    }

    function bindGraph() {
        bindControllerAdapter()
        if (!root.nodeController || !qanAdapter || !qanAdapter.graph) {
            return
        }
        if (!root.nodeController.applyToGraph(qanAdapter)) {
            return
        }
        applyLayout()
    }

    function fitGraph() {
        if (!graphView) {
            return
        }
        graphView.fitContentInView()
        captureView()
    }

    function activateLayoutKey() {
        if (!root.nodeLayoutStore || !root.nodeController) {
            return
        }
        root.nodeLayoutStore.activate("", root.nodeController.elementId,
                                      root.nodeController.imageId,
                                      root.nodeController.versionId)
        const stored = root.nodeLayoutStore.selectedNodeId
        if (stored && stored.length > 0) {
            root.nodeController.selectNode(stored)
        }
    }

    Connections {
        target: root.nodeController
        function onSnapshotChanged() {
            Qt.callLater(function () { root.bindGraph() })
        }
        function onSelectionChanged() {
            if (qanAdapter) {
                qanAdapter.applyProductSelection(root.nodeController.selectedNodeId)
            }
            if (root.nodeLayoutStore) {
                root.nodeLayoutStore.selectedNodeId = root.nodeController.selectedNodeId
            }
            if (root.renameVisible
                    && root.renameNodeId !== root.nodeController.selectedNodeId) {
                root.cancelRename()
            }
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
            root.bindControllerAdapter()
            Qt.callLater(function () { root.bindGraph() })
        }
    }

    Component.onCompleted: {
        activateLayoutKey()
        bindControllerAdapter()
        bindGraph()
        if (graphView.originCross) {
            graphView.originCross.visible = false
        }
        graphView.vScrollBar.policy = ScrollBar.AlwaysOff
        graphView.hScrollBar.policy = ScrollBar.AlwaysOff
    }

    Component.onDestruction: {
        captureView()
        clearControllerAdapter()
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
            }

            IconActionButton {
                objectName: "editorNodesAddButton"
                compact: true
                enabled: root.nodeController ? root.nodeController.canAddColorGrade : false
                iconSrc: "qrc:/panel_icons/plus.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Add Color Grade")
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
            objectName: "editorNodesEditingGuidance"
            Layout.fillWidth: true
            visible: root.nodeController && root.nodeController.incompleteDraft
            text: root.nodeController ? root.nodeController.incompleteDraftInstruction : ""
            color: root.colMuted
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
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
        }

        Rectangle {
            id: canvasHost
            objectName: "editorNodesCanvasHost"
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: appTheme.controlRadiusSmall
            color: appTheme.graphCanvasColor
            border.width: 1
            border.color: root.colCardBorder
            clip: true

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
                focus: true
                activeFocusOnTab: true
                Accessible.role: Accessible.Canvas
                Accessible.name: qsTr("Nodes graph")

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
                    if (!root.nodeController) {
                        return
                    }
                    if (event.key === Qt.Key_F2) {
                        root.beginRename()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Delete) {
                        root.deleteSelectedColorGrade()
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Plus || event.key === Qt.Key_Equal)
                               && (event.modifiers & Qt.ControlModifier)) {
                        root.addColorGrade()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up) {
                        root.nodeController.selectPreviousBackboneNode()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down) {
                        root.nodeController.selectNextBackboneNode()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Home) {
                        root.nodeController.selectDevelop()
                        event.accepted = true
                    } else if (event.key === Qt.Key_End) {
                        root.nodeController.selectDrt()
                        event.accepted = true
                    } else if (event.key === Qt.Key_0
                               && (event.modifiers & Qt.ControlModifier)) {
                        root.fitGraph()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Escape) {
                        graphView.forceActiveFocus()
                        event.accepted = true
                    }
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
                    onTriggered: root.fitGraph()
                }
            }

            AppContextMenu {
                id: nodeMenu
                objectName: "editorNodesNodeMenu"

                AppMenuItem {
                    objectName: "editorNodesRenameMenuItem"
                    text: qsTr("Rename Color Grade")
                    enabled: root.nodeController
                             ? root.nodeController.canRenameSelectedColorGrade : false
                    onTriggered: root.beginRename()
                }

                AppMenuItem {
                    objectName: "editorNodesDeleteMenuItem"
                    text: qsTr("Delete Color Grade")
                    enabled: root.nodeController
                             ? root.nodeController.canDeleteSelectedColorGrade : false
                    onTriggered: root.deleteSelectedColorGrade()
                }
            }
        }
    }
}
