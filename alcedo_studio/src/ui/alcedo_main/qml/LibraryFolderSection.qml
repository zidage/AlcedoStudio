import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

// Folder section of the Library view (issue #1009): one tile per direct child folder of the
// shown folder, above the photos, in the order of the Collections tree. The photo views host
// it at the top of their scrolled content; it scrolls with the photos and stays outside the
// photo sort and the photo groups.
//
// A tile shows the folder icon, the name, and the photo count of the folder subtree. A mounted
// folder shows the link icon after its name; an offline mount shows a dimmed name and Offline
// in place of the count. A click on a tile asks the owner to show that folder.
//
// The section takes the presses in its area, so a press there does not reach the photo input
// (selection, rubber band, context menu). It takes no wheel event: the wheel scrolls the view.
Item {
    id: section
    objectName: "libraryFolderSection"

    // Rows of FolderController.folders: folderId, elementId, name, imageCount, mountId,
    // mountOffline.
    property var folders: []
    // Width of one photo cell at the current zoom level. A tile cell has this width, but at
    // least minimumTileCellWidth, and the tiles fill the section width.
    property real photoCellWidth: 160
    // One-line tiles (8 columns and more).
    property bool compact: false
    // Space around each tile, as around the photo cards.
    property real cellPadding: 8

    readonly property real minimumTileCellWidth: 132
    readonly property int columns: Math.max(1, Math.floor(width / Math.max(photoCellWidth,
                                                                         minimumTileCellWidth)))
    readonly property real tileCellWidth: width / columns
    readonly property real tileHeight: compact ? 36 : 52
    readonly property real tileCellHeight: tileHeight + cellPadding * 2
    readonly property real headerHeight: 28
    // The hairline rule at the end of the section and the space below it.
    readonly property real footerHeight: cellPadding + 1
    readonly property int rowCount: Math.ceil(folders.length / columns)

    signal folderActivated(int folderId)

    height: folders.length > 0 ? headerHeight + rowCount * tileCellHeight + footerHeight : 0
    visible: folders.length > 0

    function countText(row) {
        if (row.mountOffline === true) {
            return qsTr("Offline")
        }
        const count = Number(row.imageCount)
        return count >= 0 ? count.toLocaleString(Qt.locale(), "f", 0) : ""
    }

    // Takes the presses of the empty parts of the section; the tiles lie above it.
    MouseArea {
        objectName: "libraryFolderSectionBlocker"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    Row {
        id: header
        x: section.cellPadding
        height: section.headerHeight
        spacing: appTheme.spaceSm

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Folders")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: section.folders.length.toLocaleString(Qt.locale(), "f", 0)
            color: appTheme.textMutedColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }

    Repeater {
        model: section.folders

        delegate: Item {
            id: cell
            required property var modelData
            required property int index
            readonly property bool mounted: Number(modelData.mountId) > 0
            readonly property bool offline: mounted && modelData.mountOffline === true

            objectName: "libraryFolderTile_" + String(modelData.elementId)
            x: (index % section.columns) * section.tileCellWidth
            y: section.headerHeight + Math.floor(index / section.columns) * section.tileCellHeight
            width: section.tileCellWidth
            height: section.tileCellHeight

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Show folder %1").arg(String(modelData.name))
            Accessible.onPressAction: section.folderActivated(Number(modelData.folderId))

            Rectangle {
                id: tile
                anchors.fill: parent
                anchors.margins: section.cellPadding
                radius: section.compact ? appTheme.controlRadiusSmall : appTheme.controlRadius
                color: tileHit.containsMouse ? appTheme.hoverColor : appTheme.bgBaseColor
                border.width: 1
                border.color: tileHit.containsMouse ? appTheme.controlBorderColor
                                                    : appTheme.glassStrokeColor

                IconImage {
                    id: folderIcon
                    readonly property real size: section.compact ? 16 : 20
                    anchors.left: parent.left
                    anchors.leftMargin: section.compact ? 9 : 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: size
                    height: size
                    sourceSize: Qt.size(size, size)
                    source: "qrc:/panel_icons/folder.svg"
                    color: appTheme.textColor
                    opacity: cell.offline ? 0.45 : 1.0
                }

                // Two lines (name, count), or one line with the count at the right end.
                Item {
                    id: textArea
                    anchors.left: folderIcon.right
                    anchors.leftMargin: section.compact ? 7 : 10
                    anchors.right: parent.right
                    anchors.rightMargin: section.compact ? 9 : 12
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom

                    Row {
                        id: nameRow
                        y: section.compact ? (textArea.height - height) / 2
                                           : textArea.height / 2 - height - 1
                        width: section.compact ? textArea.width - countLabel.implicitWidth
                                                 - appTheme.spaceSm
                                               : textArea.width
                        spacing: 5

                        Label {
                            id: nameLabel
                            objectName: "libraryFolderTileName"
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, nameRow.width
                                            - (cell.mounted ? linkIcon.width + nameRow.spacing : 0))
                            text: String(cell.modelData.name)
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            color: appTheme.textColor
                            opacity: cell.offline ? 0.45 : 1.0
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: section.compact ? appTheme.fontSizeCaption
                                                            : appTheme.fontSizeBody
                        }

                        IconImage {
                            id: linkIcon
                            objectName: "libraryFolderTileLink"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: cell.mounted
                            width: 12
                            height: 12
                            sourceSize: Qt.size(12, 12)
                            source: "qrc:/panel_icons/link.svg"
                            color: appTheme.textMutedColor
                            opacity: cell.offline ? 0.45 : 1.0
                        }
                    }

                    Label {
                        id: countLabel
                        objectName: "libraryFolderTileCount"
                        x: section.compact ? textArea.width - width : 0
                        y: section.compact ? (textArea.height - height) / 2 : textArea.height / 2 + 1
                        text: section.countText(cell.modelData)
                        color: appTheme.textMutedColor
                        font.family: cell.offline ? appTheme.uiFontFamily : appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

                MouseArea {
                    id: tileHit
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: section.folderActivated(Number(cell.modelData.folderId))
                }
            }
        }
    }

    Rectangle {
        x: section.cellPadding
        width: Math.max(0, section.width - section.cellPadding * 2)
        y: section.height - section.footerHeight
        height: 1
        color: appTheme.dividerColor
    }
}
