//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Shapes

// Loading mark of a thumbnail card: an arc in the accent colour that turns while the thumbnail
// loads. A GUI-thread RotationAnimation turns it. The Material BusyIndicator and the rotation
// animators run on the render thread and request a render-only frame on every vsync, which
// halves the frame rate of the GUI thread (issues 750 and 751: the cards that a Library switch
// or a sidebar fold shows load their thumbnails together). With reduced motion the arc stands
// still.
Item {
    id: root

    property bool running: true
    property color color: appTheme.accentColor
    property real lineWidth: Math.max(2, Math.round(width / 12))

    implicitWidth: 28
    implicitHeight: 28
    Accessible.role: Accessible.Animation
    Accessible.name: qsTr("Loading")

    Shape {
        id: arc
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: "transparent"
            strokeColor: root.color
            strokeWidth: root.lineWidth
            capStyle: ShapePath.RoundCap

            PathAngleArc {
                centerX: arc.width / 2
                centerY: arc.height / 2
                radiusX: Math.max(1, arc.width / 2 - root.lineWidth)
                radiusY: Math.max(1, arc.height / 2 - root.lineWidth)
                startAngle: -90
                sweepAngle: 270
            }
        }

        RotationAnimation on rotation {
            running: root.running && root.visible && !appTheme.reduceMotion
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
        }
    }
}
