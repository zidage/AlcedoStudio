import QtQuick
import QtQuick.Controls

// One filmstrip tile. Two independent states:
// - editing: the editor shows this image (bone well + ink frame, the strongest state);
// - selected: the image is in the shared Library selection (light gray well).
// The file name and the edit mark sit in a fixed one-line caption below the image. The
// rating is display only: the filled stars on the photo, bottom right, over a short
// bottom shade. An unrated image has no stars and no shade.
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

    Accessible.ignored: true

    // Borderless card shell (matches ThumbnailGridView delegate): the card has no
    // outline of its own; the thumbnailFrame is the only frame. Editing is carried
    // by the bone fill plus the frame's ink border; Library selection by the light
    // gray fill only.
    Rectangle {
        id: tile
        objectName: "editorFilmstripTileSurface"
        anchors.fill: parent
        radius: appTheme.panelRadius
        color: root.editing
               ? appTheme.editorListSelectedFillColor
               : (root.selected
                  ? appTheme.filmstripSelectedFillColor
                  : (root.hovered ? appTheme.buttonHoveredFillColor : appTheme.cardSurfaceColor))
        Behavior on color { ColorAnimation { duration: 120 } }

        Rectangle {
            id: thumbnailFrame
            objectName: "editorFilmstripThumbnailFrame"
            anchors.fill: parent
            anchors.margins: appTheme.spaceXs
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: root.editing
                         ? appTheme.editorListSelectedInkColor : appTheme.dividerColor
            clip: true

            Image {
                id: thumbnailImage
                objectName: "editorFilmstripThumbnailImage"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: captionBar.top
                anchors.margins: 1
                source: root.liveThumbUrl
                sourceSize.width: root.thumbnailMaxEdge
                sourceSize.height: root.thumbnailMaxEdge
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                visible: root.thumbnailReady
            }

            // Rating overlay: the filled stars in white on a short bgDeepColor shade over
            // the bottom part of the image only. No container around the stars.
            Item {
                id: ratingOverlay
                objectName: "editorFilmstripRatingOverlay"
                anchors.left: thumbnailImage.left
                anchors.right: thumbnailImage.right
                anchors.bottom: thumbnailImage.bottom
                height: Math.min(thumbnailImage.height,
                                 appTheme.lineHeightCaption + appTheme.spaceSm * 2)
                visible: root.displayRating > 0
                z: 2

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

            BusyIndicator {
                anchors.centerIn: thumbnailImage
                width: appTheme.iconOpticalSize
                height: appTheme.iconOpticalSize
                visible: root.liveThumbLoading
                running: visible
            }

            Label {
                anchors.centerIn: thumbnailImage
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
                anchors.margins: appTheme.spaceSm
                spacing: appTheme.spaceXs
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

            // Fixed one-line caption on the bottom edge of the frame, below the
            // image: the file name (middle elision) and the edit mark at the end of
            // the line. Every image in the strip has the same size. On the editing
            // tile the caption uses the ink of the light well.
            Rectangle {
                id: captionBar
                objectName: "editorFilmstripCaptionBar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 1
                height: appTheme.lineHeightCaption + appTheme.spaceXs * 2
                color: root.editing ? appTheme.editorListSelectedFillColor
                                    : appTheme.thumbnailCaptionBarColor
                z: 3

                Label {
                    id: fileNameLabel
                    objectName: "editorFilmstripFileNameLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth,
                                    parent.width - appTheme.spaceSm * 2
                                    - (editMark.visible ? editMark.width + appTheme.spaceXs : 0))
                    height: appTheme.lineHeightCaption
                    text: root.fileName.length > 0
                          ? root.fileName : qsTr("Image %1").arg(root.imageIndex + 1)
                    color: root.editing ? appTheme.editorListSelectedInkColor : appTheme.textColor
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: root.editing
                                 ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                }

                ThumbnailEditMark {
                    id: editMark
                    objectName: "editorFilmstripEditMark"
                    anchors.left: fileNameLabel.right
                    anchors.leftMargin: appTheme.spaceXs
                    anchors.verticalCenter: parent.verticalCenter
                    edited: root.edited
                    editedAt: root.editedAt
                    versionName: root.editVersionName
                    inkColor: root.editing ? appTheme.editorListSelectedInkColor
                                           : appTheme.textMutedColor
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: appTheme.controlRadiusSmall
            color: "transparent"
            border.width: 2
            border.color: root.editing
                         ? appTheme.editorListSelectedInkColor : appTheme.editorListSelectedFillColor
            visible: root.hasFocusFrame
        }
    }
}
