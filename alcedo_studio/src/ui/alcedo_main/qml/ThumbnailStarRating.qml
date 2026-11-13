import QtQuick

// Star rating of a Library card (DESIGN.md "Library thumbnail cards").
//
// interactive (2 to 6 columns): the five stars are a control. A rated photo shows five stars
// (filled and empty); an unrated photo shows none at rest, and the five empty stars fade in
// while the card is hovered. The star under the pointer previews the rating up to it, and a
// click asks for that rating (ratingRequested). A click on the star of the current rating asks
// for 0. Display only (8 and 11 columns): only the filled stars show, with no hover preview.
//
// The pointer can come from the internal handler (handlesPointer) or from the owner view,
// which sets hoveredStar and calls starAt itself (ThumbnailGridView hit-tests through its
// input overlay).
Item {
    id: root

    property int photoId: 0
    property int rating: 0
    property bool interactive: false
    property bool cardHovered: false
    property bool handlesPointer: false
    // Star under the pointer (1 to 5), or 0, from the owner view (handlesPointer false).
    property int hoveredStar: 0
    // Star under the pointer from the internal handler (handlesPointer true).
    property int pointerStar: 0
    property int pixelSize: appTheme.fontSizeCaption
    property color filledColor: appTheme.textColor
    property color emptyColor: appTheme.ratingStarEmptyColor
    property color previewColor: appTheme.textMutedColor

    signal ratingRequested(int rating)

    readonly property int clampedRating: Math.max(0, Math.min(5, Math.round(Number(rating))))
    readonly property int previewRating: interactive && cardHovered
                                         ? (handlesPointer ? pointerStar : hoveredStar) : 0
    // Filled stars that show now: the hover preview wins over the stored rating.
    readonly property int shownRating: previewRating > 0 ? previewRating : clampedRating
    readonly property bool starsShown: interactive ? (clampedRating > 0 || cardHovered)
                                                   : clampedRating > 0
    // The pointer is on the stars (internal handler only). The owner keeps the card hovered
    // while this is true, so the stars of an unrated photo stay shown under the pointer.
    readonly property bool pointerInside: starPointer.containsMouse

    implicitWidth: starRow.implicitWidth
    implicitHeight: starRow.implicitHeight
    opacity: starsShown ? 1 : 0
    Behavior on opacity {
        enabled: !appTheme.reduceMotion
        NumberAnimation { duration: appTheme.motionFadeMs; easing.type: Easing.OutCubic }
    }

    Accessible.role: interactive ? Accessible.Grouping : Accessible.StaticText
    Accessible.name: clampedRating > 0 ? qsTr("%n star(s)", "", clampedRating) : qsTr("Unrated")

    // Rating of the click on star @p star: that rating, or 0 for the star of the current one.
    function ratingForStar(star) {
        return star === clampedRating ? 0 : star
    }

    // Star (1 to 5) at @p x, @p y of this item, or 0. Gaps between stars belong to the star on
    // their left, so the pointer never falls between two stars.
    function starAt(x, y) {
        if (!interactive || !starsShown || y < -2 || y > height + 2) {
            return 0
        }
        for (let i = 0; i < starRepeater.count; ++i) {
            const star = starRepeater.itemAt(i)
            if (star && x >= star.x - starRow.spacing && x < star.x + star.width) {
                return i + 1
            }
        }
        return 0
    }

    // After a rating change of the same photo, the filled stars appear one after another.
    property int settledPhotoId: 0
    property int settledRating: 0
    function settle() {
        if (photoId !== 0 && photoId === settledPhotoId && clampedRating !== settledRating
                && !appTheme.reduceMotion) {
            for (let i = 0; i < Math.min(clampedRating, starRepeater.count); ++i) {
                const star = starRepeater.itemAt(i)
                if (star) {
                    star.playSettle()
                }
            }
        }
        settledPhotoId = photoId
        settledRating = clampedRating
    }
    onClampedRatingChanged: Qt.callLater(root.settle)
    onPhotoIdChanged: Qt.callLater(root.settle)
    Component.onCompleted: {
        settledPhotoId = photoId
        settledRating = clampedRating
    }

    Row {
        id: starRow
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        spacing: 1

        Repeater {
            id: starRepeater
            model: 5

            Text {
                id: star
                required property int index
                readonly property bool filled: index < root.shownRating
                property real settleProgress: 1

                function playSettle() {
                    settleAnimation.restart()
                }

                objectName: "thumbnailStar_" + (index + 1)
                visible: root.interactive || index < root.clampedRating
                text: filled ? "★" : "☆"
                color: !filled ? root.emptyColor
                               : (root.previewRating > 0 ? root.previewColor : root.filledColor)
                font.family: appTheme.dataFontFamily
                font.pixelSize: root.pixelSize
                verticalAlignment: Text.AlignVCenter
                opacity: settleProgress
                scale: 0.6 + 0.4 * settleProgress

                SequentialAnimation {
                    id: settleAnimation
                    ScriptAction { script: star.settleProgress = 0 }
                    PauseAnimation { duration: star.index * 30 }
                    NumberAnimation {
                        target: star
                        property: "settleProgress"
                        to: 1
                        duration: appTheme.motionFadeMs
                        easing.type: Easing.OutBack
                    }
                }
            }
        }
    }

    MouseArea {
        id: starPointer
        objectName: "thumbnailStarPointer"
        anchors.fill: starRow
        anchors.leftMargin: -starRow.spacing
        enabled: root.interactive && root.handlesPointer && root.starsShown
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPositionChanged: function(mouse) {
            const p = mapToItem(root, mouse.x, mouse.y)
            root.pointerStar = root.starAt(p.x, p.y)
        }
        onExited: root.pointerStar = 0
        onClicked: function(mouse) {
            const p = mapToItem(root, mouse.x, mouse.y)
            const star = root.starAt(p.x, p.y)
            if (star > 0) {
                root.ratingRequested(root.ratingForStar(star))
            }
        }
        onEnabledChanged: if (!enabled) root.pointerStar = 0
    }
}
