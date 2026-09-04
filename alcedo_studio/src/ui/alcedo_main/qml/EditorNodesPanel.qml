import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuickQanava 2.0 as Qan
import Alcedo.Main 1.0

// Nodes page: header plus a navigable QuickQanava canvas. Product selection
// lives on EditorNodeController. Positions, zoom, view, and Mask drawers live
// on EditorNodeLayoutStore. This page does not mutate PipelineDocument.
Item {
    id: root
    objectName: "editorNodesPageBody"

    property var theme: null
    property var editorSession: null
    property var nodeController: null
    property var nodeLayoutStore: null

    property string statusMessage: ""

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

    function bindGraph() {
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
            bindGraph()
        }
        function onSelectionChanged() {
            if (qanAdapter) {
                qanAdapter.applyProductSelection(root.nodeController.selectedNodeId)
            }
            if (root.nodeLayoutStore) {
                root.nodeLayoutStore.selectedNodeId = root.nodeController.selectedNodeId
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
            bindGraph()
        }
    }

    Component.onCompleted: {
        activateLayoutKey()
        bindGraph()
        if (graphView.originCross) {
            graphView.originCross.visible = false
        }
        graphView.vScrollBar.policy = ScrollBar.AlwaysOff
        graphView.hScrollBar.policy = ScrollBar.AlwaysOff
    }

    Component.onDestruction: captureView()

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
            }
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
                gridThickColor: appTheme.graphGridColor
                focus: true
                activeFocusOnTab: true
                Accessible.role: Accessible.Canvas
                Accessible.name: qsTr("Nodes graph")

                graph: Qan.Graph {
                    id: graphTopology
                    objectName: "editorNodesQanGraph"
                    multipleSelectionEnabled: false
                    selectionDelegate: null
                }

                Keys.onPressed: function (event) {
                    if (!root.nodeController) {
                        return
                    }
                    if (event.key === Qt.Key_Up) {
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
        }
    }
}
