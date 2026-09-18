import QtQuick

import QuickQanava 2.0 as Qan

// Backbone edge. Thin AppTheme stroke, no destination arrow, flow animation,
// glow, or decorative ending. Each edge owns a style instance so the shared
// QuickQanava default style is not mutated.
Qan.EdgeItem {
    id: edgeItem
    objectName: "editorNodeEdge"
    property bool candidate: false

    Qan.EdgeStyle {
        id: alcedoEdgeStyle
        lineWidth: appTheme.graphEdgeWidth
        lineColor: edgeItem.candidate ? appTheme.graphCandidateEdgeColor : appTheme.graphEdgeColor
        lineType: Qan.EdgeStyle.Straight
        srcShape: Qan.EdgeStyle.None
        dstShape: Qan.EdgeStyle.None
    }

    Component.onCompleted: {
        style = alcedoEdgeStyle
        srcShape = Qan.EdgeStyle.None
        dstShape = Qan.EdgeStyle.None
    }

    Qan.EdgeTemplate {
        anchors.fill: parent
        edgeItem: edgeItem
        color: edgeItem.candidate ? appTheme.graphCandidateEdgeColor : appTheme.graphEdgeColor
    }
}
