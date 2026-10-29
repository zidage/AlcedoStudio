import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// One filmstrip tile (DESIGN.md "Editor filmstrip tiles"). The photo keeps its own aspect ratio,
// fits the photo slot, and has rounded corners. There is no well or frame around it. The tile
// state is one ring that follows the photo outline at a fixed offset, so the ring and the photo
// corners are concentric:
// - editing: the editor shows this image (2 px textColor ring, the strongest state);
// - selected: the image is in the shared Library selection (2 px textMutedColor ring);
// - keyboard focus without a state: 1 px textMutedColor ring.
// Hover is a hoverColor wash in the ring area. The file name is plain text below the photo. The
// edit mark is a chip on the top-right corner of the photo. The rating is display only: the
// filled stars on the photo, bottom right, over a short bottom shade. An unrated image has no
// stars and no shade.
Item {
    id: root
    objectName: "editorFilmstripTileCard"

    property string fileName: ""
    property int imageIndex: 0
    property string liveThumbUrl: ""
    property bool liveThumbLoading: false
    property bool thumbnailReady: liveThumbUrl.length > 0
    property bool thumbnailProblemState: false
    property string thumbnailProblemText: ""
    property bool editing: false
    property bool selected: false
    property bool hovered: false
    property bool hasFocusFrame: false
    property bool saving: false
    property bool renderBusy: false
    property int displayRating: 0
    property bool edited: false
    property var editedAt: undefined
    property string editVersionName: ""
    property int thumbnailMaxEdge: 512

    readonly property string tileState: editing ? "editing" : (selected ? "selected" : "idle")

    // Ring geometry: the ring outer edge is ringOffset outside the photo, so the gap between the
    // photo and the inner edge of a 2 px ring is ringOffset - 2.
    readonly property real ringOffset: appTheme.spaceXs
    readonly property real photoRadius: appTheme.controlRadiusSmall
    readonly property real captionAreaHeight: appTheme.lineHeightCaption + appTheme.spaceXs
    // Every slot has the 3:2 shape; the photo fits into it.
    readonly property real slotAspect: 1.5
    readonly property real slotHeight: Math.max(1, height - ringOffset * 2 - captionAreaHeight)
    implicitWidth: slotHeight * slotAspect + ringOffset * 2

    readonly property bool photoPainted: thumbnailReady && thumbnailImage.status === Image.Ready
                                         && thumbnailImage.implicitWidth > 0
                                         && thumbnailImage.implicitHeight > 0
    readonly property real ringWidth: editing || selected ? 2 : (hasFocusFrame ? 1 : 0)
    readonly property color ringColor: editing ? appTheme.textColor : appTheme.textMutedColor

    Accessible.ignored: true

    Item {
        id: photoSlot
        objectName: "editorFilmstripPhotoSlot"
        x: root.ringOffset
        y: root.ringOffset
        width: Math.max(1, root.width - root.ringOffset * 2)
        height: root.slotHeight
    }

    // Hover wash in the ring area, behind the photo.
    Rectangle {
        objectName: "editorFilmstripHoverWash"
        x: photo.x - root.ringOffset
        y: photo.y - root.ringOffset
        width: photo.width + root.ringOffset * 2
        height: photo.height + root.ringOffset * 2
        radius: root.photoRadius + root.ringOffset
        color: appTheme.hoverColor
        opacity: root.hovered ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: appTheme.motionFadeMs } }
    }

    Item {
        id: photo
        objectName: "editorFilmstripPhoto"
        // Fit the painted thumbnail into the slot; before it paints, the photo is the slot.
        readonly property real fitScale: root.photoPainted
                                         ? Math.min(photoSlot.width / thumbnailImage.implicitWidth,
                                                    photoSlot.height / thumbnailImage.implicitHeight)
                                         : 1
        width: root.photoPainted ? Math.round(thumbnailImage.implicitWidth * fitScale)
                                 : photoSlot.width
        height: root.photoPainted ? Math.round(thumbnailImage.implicitHeight * fitScale)
                                  : photoSlot.height
        x: photoSlot.x + Math.round((photoSlot.width - width) / 2)
        y: photoSlot.y + Math.round((photoSlot.height - height) / 2)

        Rectangle {
            id: photoMask
            anchors.fill: parent
            radius: root.photoRadius
            visible: false
            layer.enabled: true
        }

        // The photo and everything painted on it are one layer with one rounded mask, so no part
        // needs its own corner radius and no part can show outside the photo corners.
        Item {
            id: photoContent
            objectName: "editorFilmstripPhotoContent"
            anchors.fill: parent
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: photoMask
            }

            // Sunken placeholder while the thumbnail loads or when it is missing.
            Rectangle {
                objectName: "editorFilmstripPhotoPlaceholder"
                anchors.fill: parent
                color: appTheme.bgBaseColor
                visible: !root.photoPainted
            }

            Image {
                id: thumbnailImage
                objectName: "editorFilmstripThumbnailImage"
                anchors.fill: parent
                source: root.liveThumbUrl
                sourceSize.width: root.thumbnailMaxEdge
                sourceSize.height: root.thumbnailMaxEdge
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }

            // Rating overlay: the filled stars in white on a short bgDeepColor shade over the
            // bottom part of the photo only. No container around the stars.
            Item {
                id: ratingOverlay
                objectName: "editorFilmstripRatingOverlay"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Math.min(parent.height / 2 - 1,
                                 appTheme.lineHeightCaption + appTheme.spaceSm * 2)
                visible: root.displayRating > 0

                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop {
                            position: 0.0
                            color: Qt.rgba(appTheme.bgDeepColor.r, appTheme.bgDeepColor.g,
                                           appTheme.bgDeepColor.b, 0)
                        }
                        GradientStop {
                            position: 1.0
                            color: Qt.rgba(appTheme.bgDeepColor.r, appTheme.bgDeepColor.g,
                                           appTheme.bgDeepColor.b, 0.78)
                        }
                    }
                }

                ThumbnailStarRating {
                    id: ratingStars
                    objectName: "editorFilmstripRatingStars"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: appTheme.spaceSm
                    anchors.bottomMargin: appTheme.spaceXs
                    height: appTheme.lineHeightCaption
                    rating: root.displayRating
                    interactive: false
                    filledColor: "#FFFFFF"
                }
            }

            // Edit mark: a chip on the top-right corner of the photo.
            ThumbnailEditMark {
                id: editMark
                objectName: "editorFilmstripEditMark"
                image: thumbnailImage
                edited: root.edited
                editedAt: root.editedAt
                versionName: root.editVersionName
            }
        }

        BusyIndicator {
            anchors.centerIn: parent
            width: appTheme.iconOpticalSize
            height: appTheme.iconOpticalSize
            visible: root.liveThumbLoading
            running: visible
        }

        Label {
            anchors.centerIn: parent
            width: Math.max(1, parent.width - appTheme.spaceLg)
            text: root.thumbnailProblemState
                  ? root.thumbnailProblemText : qsTr("No thumbnail")
            visible: !root.thumbnailReady && !root.liveThumbLoading
            color: root.thumbnailProblemState
                   ? appTheme.dangerColor : appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeCaption
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Row {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: appTheme.spaceXs
            spacing: appTheme.spaceXs
            z: 3
            visible: root.editing && (root.saving || root.renderBusy)

            Rectangle {
                objectName: "editorFilmstripSavingBadge"
                visible: root.saving
                width: savingLabel.implicitWidth + appTheme.spaceSm
                height: savingLabel.implicitHeight + appTheme.spaceXs
                radius: appTheme.badgeRadius
                color: appTheme.editorListSelectedInkColor
                Label {
                    id: savingLabel
                    anchors.centerIn: parent
                    text: qsTr("Saving")
                    color: appTheme.editorListSelectedFillColor
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }

            Rectangle {
                objectName: "editorFilmstripRenderBadge"
                visible: root.renderBusy
                width: renderLabel.implicitWidth + appTheme.spaceSm
                height: renderLabel.implicitHeight + appTheme.spaceXs
                radius: appTheme.badgeRadius
                color: appTheme.editorListSelectedInkColor
                Label {
                    id: renderLabel
                    anchors.centerIn: parent
                    text: qsTr("Rendering")
                    color: appTheme.editorListSelectedFillColor
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }
        }
    }

    // State ring: concentric with the photo corners (radius = photo radius + offset).
    Rectangle {
        id: selectionRing
        objectName: "editorFilmstripSelectionRing"
        readonly property real ringWidth: root.ringWidth
        readonly property color ringColor: root.ringColor
        x: photo.x - root.ringOffset
        y: photo.y - root.ringOffset
        width: photo.width + root.ringOffset * 2
        height: photo.height + root.ringOffset * 2
        radius: root.photoRadius + root.ringOffset
        color: "transparent"
        antialiasing: true
        border.width: root.ringWidth
        border.color: root.ringColor
        visible: root.ringWidth > 0
        Behavior on border.color { ColorAnimation { duration: appTheme.motionFadeMs } }
    }

    // File name below the photo: plain text, middle elision. Every tile has the same caption
    // height, so every slot has the same size.
    Label {
        id: fileNameLabel
        objectName: "editorFilmstripFileNameLabel"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.ringOffset
        anchors.rightMargin: root.ringOffset
        height: appTheme.lineHeightCaption
        text: root.fileName.length > 0
              ? root.fileName : qsTr("Image %1").arg(root.imageIndex + 1)
        color: root.editing || root.selected || root.hovered
               ? appTheme.textColor : appTheme.textMutedColor
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: root.editing ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideMiddle
        Behavior on color { ColorAnimation { duration: appTheme.motionFadeMs } }
    }
}
