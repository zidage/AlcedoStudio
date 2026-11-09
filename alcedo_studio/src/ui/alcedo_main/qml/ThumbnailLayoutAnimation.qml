import QtQuick

// Move of one Library photo delegate from its old rectangle to its new one (a zoom or an
// in-place change). The card has zoomTranslateX/Y, zoomScaleX/Y, and zoomOpacity, applied as a
// Scale from its top-left corner and then a Translate. start() puts the card at its old
// rectangle (the offset and the scale from the new rectangle) and animates to the new one; all
// values use one easing curve, so a point of the card at a fixed fraction of its size moves on a
// straight line. finish() puts the card at its new rectangle at once.
ParallelAnimation {
    id: animation

    property Item card: null
    property int duration: 280
    readonly property int easingType: Easing.OutCubic

    function start(fromX, fromY, fromScaleX, fromScaleY, fromOpacity) {
        stop()
        card.zoomTranslateX = fromX
        card.zoomTranslateY = fromY
        card.zoomScaleX = fromScaleX
        card.zoomScaleY = fromScaleY
        card.zoomOpacity = fromOpacity
        restart()
    }

    function finish() {
        stop()
        card.zoomTranslateX = 0
        card.zoomTranslateY = 0
        card.zoomScaleX = 1
        card.zoomScaleY = 1
        card.zoomOpacity = 1
    }

    NumberAnimation {
        target: animation.card
        property: "zoomTranslateX"
        to: 0
        duration: animation.duration
        easing.type: animation.easingType
    }
    NumberAnimation {
        target: animation.card
        property: "zoomTranslateY"
        to: 0
        duration: animation.duration
        easing.type: animation.easingType
    }
    NumberAnimation {
        target: animation.card
        property: "zoomScaleX"
        to: 1
        duration: animation.duration
        easing.type: animation.easingType
    }
    NumberAnimation {
        target: animation.card
        property: "zoomScaleY"
        to: 1
        duration: animation.duration
        easing.type: animation.easingType
    }
    NumberAnimation {
        target: animation.card
        property: "zoomOpacity"
        to: 1
        duration: animation.duration
        easing.type: animation.easingType
    }
}
