//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Effects
import QtQuick.Shapes

// Working outline of a background task: the whole rounded border carries a
// conical gradient with a bright head and a fading tail, and the gradient turns
// around the outline while the task runs. A render-thread animator turns the
// gradient; a ring mask keeps only the border. With reduced motion the
// gradient stands still.
Item {
    id: root

    property real radius: 0
    property real borderWidth: 1.5
    property color color: appTheme.backgroundTaskWorkingColor
    property bool running: true

    Accessible.ignored: true

    Item {
        id: sweepSource
        anchors.fill: parent
        visible: false
        layer.enabled: true

        Shape {
            id: sweep
            readonly property real side: Math.ceil(Math.hypot(root.width, root.height))
            width: side
            height: side
            anchors.centerIn: parent

            ShapePath {
                strokeColor: "transparent"
                strokeWidth: 0
                fillGradient: ConicalGradient {
                    centerX: sweep.side / 2
                    centerY: sweep.side / 2
                    angle: 0
                    GradientStop { position: 0.0; color: root.color }
                    GradientStop {
                        position: 0.3
                        color: Qt.rgba(root.color.r, root.color.g, root.color.b, 0.22)
                    }
                    GradientStop {
                        position: 0.94
                        color: Qt.rgba(root.color.r, root.color.g, root.color.b, 0.22)
                    }
                    GradientStop { position: 1.0; color: root.color }
                }
                startX: 0
                startY: 0
                PathLine { x: sweep.side; y: 0 }
                PathLine { x: sweep.side; y: sweep.side }
                PathLine { x: 0; y: sweep.side }
                PathLine { x: 0; y: 0 }
            }

            RotationAnimator on rotation {
                running: root.running && root.visible && !appTheme.reduceMotion
                loops: Animation.Infinite
                from: 0
                to: 360
                duration: appTheme.motionFoldOpenMs * 10
            }
        }
    }

    Rectangle {
        id: ringMask
        anchors.fill: parent
        visible: false
        layer.enabled: true
        radius: root.radius
        color: "transparent"
        border.width: root.borderWidth
        border.color: "white"
    }

    MultiEffect {
        anchors.fill: parent
        source: sweepSource
        maskEnabled: true
        maskSource: ringMask
    }
}
