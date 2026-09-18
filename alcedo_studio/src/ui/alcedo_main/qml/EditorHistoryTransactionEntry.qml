import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One transaction timeline entry (Git-graph row): the commit-graph cell on the
// left, then a flat two-line commit row — title + relative time, then hash
// chip + delta summary. Only the checked-out row carries the card
// surface with the quiet 1 px text outline; other rows stay flat on the
// sunken timeline well (DESIGN.md, Git-graph timeline language).
Item {
    id: root
    objectName: "editorHistoryTransactionDelegate"

    property var theme: null
    property var editorSession: null
    property var historyModel: null

    required property int index
    required property bool current
    required property string commitId
    required property string displayName
    required property string deltaText
    required property string timelinePosition
    required property var createdAtNs

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor

    // Airy Git-graph row metrics: title line + caption chip line + top/bottom
    // air (was the dense LUT-density well: body + caption + spaceSm).
    readonly property int rowTopPadding: appTheme.spaceXs
    readonly property int rowLineGap: appTheme.spaceXs
    readonly property int rowBottomPadding: appTheme.spaceSm
    readonly property int titleRowHeight: appTheme.lineHeightTitle
    readonly property int metaRowHeight: appTheme.lineHeightCaption + appTheme.spaceXs
    readonly property int listRowGap: ListView.view ? ListView.view.spacing : 0
    readonly property int rowCount: ListView.view ? ListView.view.count : 0

    property bool currentTransaction: current
    property string transactionId: String(commitId || "")
    property string transactionDisplayName: String(displayName || "")
    property string transactionDelta: String(deltaText || "")
    property string transactionPosition: String(timelinePosition || "applied")
    property string transactionTime: relativeTime(createdAtNs)
    property string shortCommitId: transactionId.length > 0
                                   ? transactionId.slice(0, 8) : ""
    property bool futureRow: transactionPosition === "future"
    // Model order is newest-first: the last row is the graph root commit.
    property bool firstRow: index === 0
    property bool lastRow: rowCount > 0 && index === rowCount - 1
    property bool canMove: root.editorSession
                           && root.editorSession.actions.canMoveHead
                           && transactionId.length > 0 && !currentTransaction

    function relativeTime(createdAtNs) {
        var timestamp = Number(createdAtNs || 0)
        if (timestamp <= 0) return qsTr("Earlier")

        var ageSeconds = Math.max(0, Math.floor((Date.now() * 1000000 - timestamp) / 1000000000))
        if (ageSeconds < 60) return qsTr("Just now")
        if (ageSeconds < 3600) return qsTr("%1 min ago").arg(Math.floor(ageSeconds / 60))
        if (ageSeconds < 86400) return qsTr("%1 hr ago").arg(Math.floor(ageSeconds / 3600))
        return qsTr("%1 days ago").arg(Math.floor(ageSeconds / 86400))
    }

    function activateMove() {
        if (root.canMove && root.historyModel)
            root.historyModel.moveHeadToCommit(root.transactionId)
    }

    width: ListView.view ? ListView.view.width : 0
    height: rowTopPadding + titleRowHeight + rowLineGap + metaRowHeight + rowBottomPadding

    ListView.onPooled: {}
    ListView.onReused: {}

    EditorHistoryCommitGraphNode {
        id: graphCell
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        current: root.currentTransaction
        rootCommit: root.lastRow
        future: root.futureRow
        connectAbove: !root.firstRow
        connectBelow: !root.lastRow
        linkExtendBelow: root.lastRow ? 0 : root.listRowGap
        nodeCenterY: root.rowTopPadding + root.titleRowHeight / 2
        lineColor: root.colCardBorder
        inkColor: root.colText
        mutedInkColor: root.colMuted
    }

    Rectangle {
        id: entryCard
        objectName: "editorHistoryCard"
        anchors.left: graphCell.right
        anchors.leftMargin: appTheme.spaceXs
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        radius: appTheme.controlRadiusSmall
        color: root.currentTransaction
               ? root.colCardSurface
               : (cardMouseArea.containsMouse && root.canMove
                  ? appTheme.hoverColor : "transparent")
        property color selectionOutlineColor: root.currentTransaction
                                              ? root.colText : "transparent"
        border.width: 1
        border.color: selectionOutlineColor
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: root.transactionDisplayName
        Accessible.description: root.transactionDelta.length > 0
                                ? root.transactionDelta : root.transactionId
        ToolTip.text: root.transactionId.length > 0
                      ? qsTr("Commit %1").arg(root.transactionId) : ""
        ToolTip.visible: cardMouseArea.containsMouse
        ToolTip.delay: 500
        Keys.onEnterPressed: {
            root.activateMove()
            event.accepted = true
        }
        Keys.onReturnPressed: {
            root.activateMove()
            event.accepted = true
        }
        Keys.onSpacePressed: {
            root.activateMove()
            event.accepted = true
        }

        MouseArea {
            id: cardMouseArea
            objectName: "editorHistoryCardMouseArea"
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: root.canMove ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: root.activateMove()
        }

        RowLayout {
            id: titleRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: appTheme.spaceSm
            anchors.rightMargin: appTheme.spaceSm
            anchors.topMargin: root.rowTopPadding
            height: root.titleRowHeight
            spacing: appTheme.spaceSm

            Label {
                objectName: "editorHistoryCommitTitle"
                Layout.fillWidth: true
                text: root.transactionDisplayName.length > 0
                      ? root.transactionDisplayName : qsTr("Adjustment")
                color: root.futureRow ? root.colMuted : root.colText
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightStrong
            }

            Label {
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                text: root.transactionTime
                color: root.colMuted
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        RowLayout {
            id: metaRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleRow.bottom
            anchors.leftMargin: appTheme.spaceSm
            anchors.rightMargin: appTheme.spaceSm
            anchors.topMargin: root.rowLineGap
            height: root.metaRowHeight
            spacing: appTheme.spaceXs

            Rectangle {
                id: hashChip
                visible: root.shortCommitId.length > 0
                Layout.preferredWidth: hashLabel.implicitWidth + appTheme.spaceSm * 2
                Layout.preferredHeight: root.metaRowHeight
                radius: appTheme.badgeRadius
                color: root.currentTransaction ? appTheme.bgBaseColor : root.colCardSurface
                border.width: 1
                border.color: root.colCardBorder

                Label {
                    id: hashLabel
                    objectName: "editorHistoryCommitHash"
                    anchors.centerIn: parent
                    text: root.shortCommitId
                    color: root.colMuted
                    font.family: appTheme.monoFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            Label {
                objectName: "editorHistoryCommitValue"
                Layout.fillWidth: true
                text: root.transactionDelta
                color: root.colMuted
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.monoFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }
    }
}
