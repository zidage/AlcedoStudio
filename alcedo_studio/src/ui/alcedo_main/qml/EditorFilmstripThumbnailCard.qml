import QtQuick
import QtQuick.Controls

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
    property bool selected: false
    property bool hovered: false
    property bool hasFocusFrame: false
    property bool saving: false
    property bool renderBusy: false
    property int displayRating: 0
    property int thumbnailMaxEdge: 512

    readonly property real fileNameLabelHeight: appTheme.lineHeightCaption
    readonly property real thumbnailAreaHeight: Math.max(
        1, height - fileNameLabelHeight - appTheme.spaceXs * 2)

    Accessible.ignored: true

    function ratingStars(value) {
        const rating = Math.max(0, Math.min(5, Math.round(Number(value))))
        let text = ""
        for (let i = 1; i <= 5; ++i) {
            text += i <= rating ? "★" : "☆"
        }
        return text
    }

    // Borderless card shell (matches ThumbnailGridView delegate): the card has no
    // outline of its own; the thumbnailFrame is the only frame, and selection is
    // carried by the fill plus the frame's ink border.
    Rectangle {
        id: tile
        objectName: "editorFilmstripTileSurface"
        anchors.fill: parent
        radius: appTheme.panelRadius
        color: root.selected
               ? appTheme.editorListSelectedFillColor
               : (root.hovered ? appTheme.buttonHoveredFillColor : appTheme.cardSurfaceColor)
        Behavior on color { ColorAnimation { duration: 120 } }

        Column {
            id: tileContent
            anchors.fill: parent
            anchors.margins: appTheme.spaceXs
            spacing: 0

            Rectangle {
                id: thumbnailFrame
                objectName: "editorFilmstripThumbnailFrame"
                width: parent.width
                height: root.thumbnailAreaHeight
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.selected
                             ? appTheme.editorListSelectedInkColor : appTheme.dividerColor

                Image {
                    id: thumbnailImage
                    anchors.fill: parent
                    anchors.margins: appTheme.spaceXs
                    source: root.liveThumbUrl
                    sourceSize.width: root.thumbnailMaxEdge
                    sourceSize.height: root.thumbnailMaxEdge
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    visible: root.thumbnailReady
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
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceXs
                    visible: root.selected && (root.saving || root.renderBusy)

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

                Rectangle {
                    id: ratingOverlay
                    objectName: "editorFilmstripRatingOverlay"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: appTheme.spaceXs
                    width: ratingLabel.implicitWidth + appTheme.spaceXs * 2
                    height: ratingLabel.implicitHeight + appTheme.spaceXs
                    radius: appTheme.badgeRadius
                    // Monochrome badge: dark well + light stars (no gold accent).
                    color: appTheme.editorListSelectedInkColor
                    visible: root.displayRating > 0
                    z: 3

                    Label {
                        id: ratingLabel
                        objectName: "editorFilmstripRatingLabel"
                        anchors.centerIn: parent
                        text: root.ratingStars(root.displayRating)
                        color: appTheme.editorListSelectedFillColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            Label {
                id: fileNameLabel
                objectName: "editorFilmstripFileNameLabel"
                width: parent.width
                height: root.fileNameLabelHeight
                text: root.fileName.length > 0
                      ? root.fileName : qsTr("Image %1").arg(root.imageIndex + 1)
                color: root.selected ? appTheme.editorListSelectedInkColor : appTheme.textMutedColor
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: root.selected
                             ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: appTheme.controlRadiusSmall
            color: "transparent"
            border.width: 2
            border.color: root.selected
                         ? appTheme.editorListSelectedInkColor : appTheme.editorListSelectedFillColor
            visible: root.hasFocusFrame
        }
    }
}
