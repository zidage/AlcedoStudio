import QtQuick
import QtQuick.Effects

// Content placeholder: a bgBaseColor rectangle with a hoverColor highlight band
// that moves from left to right. One sweep takes appTheme.skeletonCycleMs with
// Easing.InOutSine. DESIGN.md Motion lists this as the one approved perpetual
// animation for content placeholders.
//
// The band moves only while `animated` is true, the block is visible, and
// appTheme.reduceMotion is false. Otherwise the block is a static bgBaseColor
// rectangle. Rounded blocks clip the band to their radius through a mask.
Rectangle {
    id: root

    // False draws a plain static tile (for example, a cover tile without a photo).
    property bool animated: true
    readonly property bool bandRunning: root.animated && root.visible
                                        && !appTheme.reduceMotion
                                        && root.width > 0 && root.height > 0

    color: appTheme.bgBaseColor
    clip: true
    layer.enabled: root.bandRunning && root.radius > 0
    layer.effect: MultiEffect {
        maskEnabled: true
        maskSource: shapeMask
    }

    Rectangle {
        id: shapeMask
        width: root.width
        height: root.height
        radius: root.radius
        visible: false
        layer.enabled: true
    }

    Rectangle {
        id: band
        y: 0
        width: Math.max(root.width * 0.5, root.height)
        height: root.height
        visible: root.bandRunning
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0.0
                color: Qt.rgba(appTheme.hoverColor.r, appTheme.hoverColor.g, appTheme.hoverColor.b, 0)
            }
            GradientStop {
                position: 0.5
                color: appTheme.hoverColor
            }
            GradientStop {
                position: 1.0
                color: Qt.rgba(appTheme.hoverColor.r, appTheme.hoverColor.g, appTheme.hoverColor.b, 0)
            }
        }

        NumberAnimation on x {
            from: -band.width
            to: root.width
            duration: appTheme.skeletonCycleMs
            easing.type: Easing.InOutSine
            loops: Animation.Infinite
            running: root.bandRunning
        }
    }
}
