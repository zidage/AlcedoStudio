import QtQuick

// One cell of the history commit graph: the link segments above and below the
// row plus the node glyph. Stacked cells form the continuous Git-graph rail.
// The host delegate supplies row state (current / merge / root / future) and
// link connectivity; all glyph geometry derives from AppTheme spacing tokens.
//
// Glyph language (DESIGN.md, Git-graph timeline):
//   applied edit  — small solid disc
//   undone (redo) — small hollow ring
//   graph root    — large hollow ring (also used for merge commits)
//   checked out   — large double ring (text-ink outline + inner dot)
Item {
    id: root

    property bool current: false
    property bool merge: false
    property bool rootCommit: false
    property bool future: false
    // Link continuity: the newest row has no segment above; the graph-root
    // row has none below.
    property bool connectAbove: true
    property bool connectBelow: true
    // Extra link painted into the list spacing gap below this row so the rail
    // reads as one unbroken line across delegates.
    property real linkExtendBelow: 0
    // Node glyph aligns to the title line's vertical center, not the row
    // center, so the graph column tracks the commit identity line.
    property real nodeCenterY: height / 2

    property color lineColor: appTheme.cardBorderColor
    property color inkColor: appTheme.textColor
    property color mutedInkColor: appTheme.textMutedColor
    // Hollow glyphs cut the link with the sunken well color behind them.
    property color cutColor: appTheme.bgBaseColor

    readonly property int nodeSmall: appTheme.spaceSm
    readonly property int nodeLarge: appTheme.spaceLg
    readonly property int nodeDot: appTheme.spaceXs + 2
    readonly property bool largeNode: root.current
                                      || (!root.future && (root.rootCommit || root.merge))
    readonly property bool hollowNode: root.current || root.future
                                       || root.rootCommit || root.merge

    implicitWidth: appTheme.spaceLg + appTheme.spaceSm

    Rectangle {
        visible: root.connectAbove
        x: root.width / 2 - width / 2
        y: 0
        width: 1
        height: root.nodeCenterY
        color: root.lineColor
    }

    Rectangle {
        visible: root.connectBelow
        x: root.width / 2 - width / 2
        y: root.nodeCenterY
        width: 1
        height: root.height + root.linkExtendBelow - root.nodeCenterY
        color: root.lineColor
    }

    Rectangle {
        x: root.width / 2 - width / 2
        y: root.nodeCenterY - height / 2
        width: root.largeNode ? root.nodeLarge : root.nodeSmall
        height: width
        radius: width / 2
        color: root.hollowNode ? root.cutColor : root.mutedInkColor
        border.width: root.hollowNode ? 1 : 0
        border.color: root.current ? root.inkColor : root.mutedInkColor
        z: 2

        Rectangle {
            visible: root.current
            anchors.centerIn: parent
            width: root.nodeDot
            height: width
            radius: width / 2
            color: root.inkColor
        }
    }
}
