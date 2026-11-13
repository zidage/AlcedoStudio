import QtQuick
import QtQuick.Effects

// Cover block of the welcome overview: one large tile (2 parts of the width,
// both rows) and two small tiles (1 part), spaceXs apart. The outer corners
// are rounded with controlRadiusSmall through one mask over all tiles.
//
// Tile rules (welcome overview plan, section 3.3):
//   "loading"            - animated skeleton tiles.
//   "ready", cover row   - the thumbnail (PreserveAspectCrop); an animated
//                          skeleton shows until the image is ready.
//   "ready", no row      - plain static bgBaseColor tile (fewer than 3 photos).
//   "failed" / "empty"   - plain static bgBaseColor tiles.
// The tiles read only `previewState` and `coverItems` from WelcomeProjectPreviewAdapter.
Item {
    id: root

    // WelcomeProjectPreviewAdapter.state.
    property string previewState: "loading"
    // Up to 3 entries {elementId, imageId, thumbUrl, thumbLoading}.
    property var coverItems: []

    readonly property real largeTileWidth: Math.round((root.width - appTheme.spaceXs) * 2 / 3)
    readonly property real smallTileWidth: root.width - appTheme.spaceXs - root.largeTileWidth
    readonly property real smallTileHeight: (root.height - appTheme.spaceXs) / 2

    Item {
        id: tiles
        anchors.fill: parent
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: coverMask
        }

        CoverTile {
            x: 0
            y: 0
            width: root.largeTileWidth
            height: root.height
            mosaicState: root.previewState
            cover: root.coverItems && root.coverItems.length > 0 ? root.coverItems[0] : null
        }

        CoverTile {
            x: root.largeTileWidth + appTheme.spaceXs
            y: 0
            width: root.smallTileWidth
            height: root.smallTileHeight
            mosaicState: root.previewState
            cover: root.coverItems && root.coverItems.length > 1 ? root.coverItems[1] : null
        }

        CoverTile {
            x: root.largeTileWidth + appTheme.spaceXs
            y: root.smallTileHeight + appTheme.spaceXs
            width: root.smallTileWidth
            height: root.height - y
            mosaicState: root.previewState
            cover: root.coverItems && root.coverItems.length > 2 ? root.coverItems[2] : null
        }
    }

    Rectangle {
        id: coverMask
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        visible: false
        layer.enabled: true
    }

    component CoverTile: Item {
        id: tile

        // Inline components do not see the ids of the enclosing file.
        property string mosaicState: "loading"
        property var cover: null
        readonly property string thumbUrl: tile.cover && tile.cover.thumbUrl ? String(tile.cover.thumbUrl) : ""
        readonly property bool imageReady: tile.thumbUrl.length > 0 && coverImage.status === Image.Ready

        Accessible.role: Accessible.Graphic
        Accessible.name: qsTr("Project cover")

        SkeletonBlock {
            anchors.fill: parent
            visible: !tile.imageReady
            animated: tile.mosaicState === "loading"
                      || (tile.mosaicState === "ready" && tile.cover !== null)
        }

        Image {
            id: coverImage
            anchors.fill: parent
            visible: tile.imageReady
            source: tile.thumbUrl
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            smooth: true
        }
    }
}
