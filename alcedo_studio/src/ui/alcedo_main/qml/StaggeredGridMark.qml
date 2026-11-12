//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick

// Compact staggered grid loader: 3 x 3 tiles that grow and light up as the wave passes
// (DESIGN.md "Library loading"). The loading mark of a photo: a Library card or tile while its
// thumbnail loads, an Editor filmstrip card, and the Library card over a kept result while a
// replacing read runs. Gap and tile radius follow the width, so one mark serves every size.
//
// A dense grid shows many marks at once, so a mark costs one quad: the fragment shader
// shaders/staggered_grid_mark.frag draws the nine tiles. Every mark reads one shared wave clock,
// appTheme.loadingWavePhase, which ticks every appTheme.loadingWaveFrameMs while at least one
// mark holds it; the window then repaints at that rate, not at the display rate.
//
// The Qt Quick software renderer (offscreen QML tests) does not draw ShaderEffect; the app always
// renders through QRhi (D3D11, Metal), where the mark draws.
ShaderEffect {
    id: root

    property bool running: true
    property string accessibleName: qsTr("Loading")
    property color tileColor: appTheme.bgBaseColor
    property color waveColor: appTheme.textColor
    property real restLevel: 0.15
    property real scaleDepth: 0.3
    property real waveStrength: 0.85

    // The wave runs only while the mark is shown and motion is allowed; otherwise every tile
    // draws at restLevel.
    readonly property bool waveRunning: root.running && root.visible && root.width > 0
                                        && !appTheme.reduceMotion

    // Shader inputs (staggered_grid_mark.frag).
    readonly property real phase: root.waveRunning ? appTheme.loadingWavePhase : 0
    readonly property real animated: root.waveRunning ? 1 : 0
    readonly property real markSize: root.width
    readonly property real gap: Math.max(2, Math.round(root.width / 9))
    readonly property real tileRadius: Math.max(1, Math.round(root.width / 18))

    implicitWidth: 24
    implicitHeight: 24
    blending: true
    fragmentShader: Qt.resolvedUrl("shaders/staggered_grid_mark.frag.qsb")

    Accessible.role: Accessible.Animation
    Accessible.name: root.accessibleName

    // AppTheme lets go of a destroyed mark by itself.
    onWaveRunningChanged: appTheme.setLoadingWaveHolder(root, root.waveRunning)
    Component.onCompleted: appTheme.setLoadingWaveHolder(root, root.waveRunning)
}
