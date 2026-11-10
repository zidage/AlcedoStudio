import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// One tile of the justified-rows layout of ThumbnailGridView (a delegate of its
// JustifiedLayoutModel). The photo fills the photo area of the tile (the tile width follows the
// aspect ratio of the photo); the caption below it shows the file name and the color label dots
// (ThumbnailColorLabelDots). While the pointer is over the tile, a details panel on the bottom
// of the photo shows the camera, ISO, aperture, capture date, labels, and the star rating; at
// the zoom levels where the stars are a control, a click on a star rates the photo. The edit mark, the missing-source mark, and the Delete mark
// (ThumbnailDeleteMark) sit on the photo as on the classic cards.
//
// Pointer input reaches the tile through the input overlay of ThumbnailGridView: the overlay
// hit-tests with starAtOverlay and editMarkAtOverlay, and the view starts the zoom animation
// with startZoomLayoutAnim.
Item {
    id: tile
    objectName: "thumbnailGridView_tile_" + photoIndex

    // The ThumbnailGridView that owns the tile.
    required property Item view
    required property int photoIndex
    required property real tileX
    required property real tileY
    required property real tileWidth
    required property real tileHeight
    required property int elementId
    required property int imageId
    required property string fileName
    required property string cameraModel
    required property int iso
    required property string aperture
    required property string captureDate
    required property int rating
    required property int colorLabels
    required property bool deleteMark
    required property bool isHdr
    required property bool isEdited
    required property var editedAt
    required property string editVersionName
    required property string tags
    required property string thumbUrl
    required property bool thumbLoading
    required property bool thumbMissingSource
    required property string thumbErrorText

    // Photo index of the tile; the view reads it from every delegate kind.
    readonly property int cardIndex: photoIndex
    readonly property alias liveThumbUrl: thumbPin.liveThumbUrl
    readonly property bool isSelected: view.isImageSelected(elementId)
    readonly property bool isHovered: view.hoveredCardIndex === photoIndex
    readonly property bool detailsShown: isHovered && view.justifiedDetailsEnabled
    readonly property real captionHeight: view.justifiedCaptionHeight
    readonly property int tileRadius: view.columns >= 11 ? 4 : 6
    readonly property string displayTags: view.semanticTagsDisplayText(tags)

    readonly property bool thumbnailReady: thumbPin.liveThumbUrl.length > 0
    readonly property bool thumbnailLoadingState: thumbPin.liveThumbLoading
    readonly property bool thumbnailMissingState: !thumbnailReady && !thumbnailLoadingState
                                                  && thumbPin.liveThumbMissingSource
    readonly property bool thumbnailErrorState: !thumbnailReady && !thumbnailLoadingState
                                                && thumbPin.liveThumbErrorText.length > 0
    readonly property bool thumbnailProblemState: thumbnailMissingState || thumbnailErrorState
    readonly property string thumbnailProblemText: thumbPin.liveThumbErrorText.length > 0
                                                   ? thumbPin.liveThumbErrorText
                                                   : qsTr("Source file was moved or deleted")

    x: tileX
    y: tileY
    width: tileWidth
    height: tileHeight

    ThumbnailPinBinding {
        id: thumbPin
        view: tile.view
        elementId: tile.elementId
        imageId: tile.imageId
        thumbUrl: tile.thumbUrl
        thumbLoading: tile.thumbLoading
        thumbMissingSource: tile.thumbMissingSource
        thumbErrorText: tile.thumbErrorText
    }

    function syncThumbnailBinding() {
        thumbPin.sync()
    }

    // Star (1 to 5) under the overlay point @p x, @p y, or 0 (see ThumbnailStarRating).
    function starAtOverlay(x, y) {
        if (!detailsShown || !tileStars.visible) {
            return 0
        }
        const p = tile.view.inputOverlay.mapToItem(tileStars, x, y)
        return tileStars.starAt(p.x, p.y)
    }

    function ratingForStar(star) {
        return tileStars.ratingForStar(star)
    }

    function editMarkAtOverlay(x, y) {
        if (!editMark.visible) {
            return false
        }
        const p = tile.view.inputOverlay.mapToItem(editMark, x, y)
        return p.x >= 0 && p.y >= 0 && p.x < editMark.width && p.y < editMark.height
    }

    // ── Zoom and change animation (see ThumbnailGridView.applyZoomTransition) ──
    // The tile is drawn at (x + zoomTranslateX, y + zoomTranslateY) with the size scaled by
    // zoomScaleX and zoomScaleY from its top-left corner; the animation moves these to 0 and 1.
    property real zoomTranslateX: 0
    property real zoomTranslateY: 0
    property real zoomScaleX: 1
    property real zoomScaleY: 1
    property real zoomOpacity: 1
    opacity: zoomOpacity
    transform: [
        Scale {
            xScale: tile.zoomScaleX
            yScale: tile.zoomScaleY
        },
        Translate {
            x: tile.zoomTranslateX
            y: tile.zoomTranslateY
        }
    ]

    ThumbnailLayoutAnimation {
        id: layoutAnimation
        card: tile
        duration: tile.view.delegateReflowDuration
    }

    function startZoomLayoutAnim(fromX, fromY, fromScaleX, fromScaleY, fromOpacity) {
        layoutAnimation.start(fromX, fromY, fromScaleX, fromScaleY, fromOpacity)
    }

    function finishZoomLayoutAnim() {
        layoutAnimation.finish()
    }

    Accessible.role: Accessible.ListItem
    Accessible.name: {
        const parts = [fileName,
                       rating > 0 ? qsTr("%n star(s)", "", rating) : qsTr("Unrated")]
        if (isEdited) {
            parts.push(qsTr("Edited"))
        }
        if (colorLabels !== 0) {
            parts.push(labelDots.Accessible.name)
        }
        if (deleteMark) {
            parts.push(deleteMarkItem.Accessible.name)
        }
        return parts.join(", ")
    }
    Accessible.selected: isSelected

    Item {
        id: photoArea
        objectName: "justifiedThumbnailTile_photo"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.max(1, tile.height - tile.captionHeight)

        Rectangle {
            anchors.fill: parent
            radius: tile.tileRadius
            color: appTheme.bgBaseColor
        }
        ThumbnailLoadingSpinner {
            anchors.centerIn: parent
            width: 28
            height: 28
            visible: tile.thumbnailLoadingState
        }
        Image {
            id: thumbImage
            anchors.fill: parent
            source: thumbPin.liveThumbUrl
            visible: false
            asynchronous: true
            // A new tier keeps the shown thumbnail until the new image is ready.
            retainWhileLoading: true
            fillMode: Image.PreserveAspectCrop
        }
        Rectangle {
            id: thumbMask
            anchors.fill: parent
            radius: tile.tileRadius
            visible: false
            layer.enabled: true
        }
        MultiEffect {
            anchors.fill: parent
            source: thumbImage
            maskEnabled: true
            maskSource: thumbMask
            saturation: deleteMarkItem.photoSaturation
            visible: tile.thumbnailReady
        }
        ThumbnailDeleteMark {
            id: deleteMarkItem
            objectName: "justifiedThumbnailTile_deleteMark"
            anchors.fill: parent
            radius: tile.tileRadius
            marked: tile.deleteMark
            compact: tile.view.columns >= 8
        }

        // Edit mark: a chip on the top-right corner of the photo.
        ThumbnailEditMark {
            id: editMark
            compact: tile.view.columns >= 8
            x: photoArea.width - width
               - (tile.thumbnailProblemState ? 30 : editMark.inset)
            y: editMark.inset
            edited: tile.isEdited
            editedAt: tile.editedAt
            versionName: tile.editVersionName
            pointerOver: tile.view.hoveredEditMarkIndex === tile.photoIndex
        }
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            width: 18
            height: 18
            radius: 9
            visible: tile.thumbnailProblemState
            color: appTheme.dangerTintColor
            border.width: 1
            border.color: appTheme.dangerColor
        }
        Column {
            anchors.centerIn: parent
            width: parent.width - 12
            visible: tile.thumbnailProblemState
            spacing: 2
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "!"
                color: appTheme.dangerColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: tile.view.columns >= 11 ? 24 : 30
                font.weight: 700
            }
            Label {
                width: parent.width
                text: tile.thumbnailProblemText
                color: appTheme.dangerColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: tile.view.columns >= 11 ? 8 : 10
                font.weight: tile.view.dataFontWeight
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
        }

        // Hover details: the camera line, the date line, and the stars.
        Rectangle {
            id: detailsPanel
            objectName: "justifiedThumbnailTile_details"
            z: 4
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.min(parent.height, detailsColumn.implicitHeight + appTheme.spaceSm * 2)
            visible: opacity > 0
            opacity: tile.detailsShown ? 1 : 0
            Behavior on opacity {
                enabled: !appTheme.reduceMotion
                NumberAnimation { duration: appTheme.motionFadeMs; easing.type: Easing.OutCubic }
            }
            color: appTheme.thumbnailDetailsScrimColor
            bottomLeftRadius: tile.tileRadius
            bottomRightRadius: tile.tileRadius
            clip: true

            Column {
                id: detailsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: appTheme.spaceSm
                spacing: 2

                Label {
                    objectName: "justifiedThumbnailTile_camera"
                    width: parent.width
                    text: qsTr("%1 | ISO %2 | f/%3").arg(tile.cameraModel).arg(tile.iso)
                                                     .arg(tile.aperture)
                    color: appTheme.textColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: tile.view.metadataFontSize
                    font.weight: tile.view.dataFontWeight
                    font.letterSpacing: tile.view.dataLetterSpacing
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "justifiedThumbnailTile_date"
                    width: parent.width
                    text: tile.displayTags.length > 0
                          ? qsTr("%1 | %2").arg(tile.captureDate).arg(tile.displayTags)
                          : tile.captureDate
                    color: appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: tile.view.metadataFontSize
                    font.weight: tile.view.dataFontWeight
                    font.letterSpacing: tile.view.dataLetterSpacing
                    elide: Text.ElideRight
                }
                ThumbnailStarRating {
                    id: tileStars
                    objectName: "justifiedThumbnailTile_stars"
                    photoId: tile.elementId
                    rating: tile.rating
                    interactive: tile.view.starsInteractive
                    cardHovered: tile.isHovered
                    hoveredStar: tile.isHovered ? tile.view.hoveredCardStar : 0
                    pixelSize: tile.view.titleFontSize
                }
            }
        }
    }

    // Caption: the file name and the color label dots. The file name elides first.
    Item {
        objectName: "justifiedThumbnailTile_caption"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: tile.captionHeight
        visible: tile.captionHeight > 0

        Label {
            id: captionName
            objectName: "justifiedThumbnailTile_name"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, parent.width
                            - (labelDots.visible ? labelDots.width + labelDots.leadingGap : 0)))
            text: tile.fileName
            color: tile.deleteMark ? appTheme.textMutedColor : appTheme.textColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: tile.view.titleFontSize
            font.weight: tile.view.dataFontWeight
            font.letterSpacing: tile.view.dataLetterSpacing
            elide: Text.ElideRight
        }
        ThumbnailColorLabelDots {
            id: labelDots
            objectName: "justifiedThumbnailTile_labelDots"
            anchors.left: captionName.right
            anchors.leftMargin: leadingGap
            anchors.verticalCenter: parent.verticalCenter
            labels: tile.colorLabels
            compact: tile.view.compactText
            ringColor: tile.view.justifiedCanvasColor
        }
    }

    // Monochrome selection: a 2 px text-color outline around the photo, as on the classic cards.
    Rectangle {
        anchors.fill: photoArea
        z: 5
        color: "transparent"
        radius: tile.tileRadius
        border.width: tile.isSelected ? 2 : 0
        border.color: appTheme.textColor
        Behavior on border.width { NumberAnimation { duration: 150 } }
    }
}
