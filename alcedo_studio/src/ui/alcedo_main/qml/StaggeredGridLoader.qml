//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick

// Staggered grid loader: a grid of rounded tiles in which a light wave runs along the
// diagonals, top left to bottom right. Tile (row, column) pulses after the tiles of the
// diagonals before it, so the grid reads as photos that arrive one after another.
//
// Two uses (DESIGN.md "Library loading"):
// - Placeholder grid: `rows: 0` fills the item with as many rows as fit, at the column count
//   and card proportion of the photo grid that will replace it.
// - Compact mark: a fixed 3 x 3 grid with `scaleDepth` > 0, so each tile also grows a little
//   as the wave passes.
//
// One GUI-thread NumberAnimation drives the wave for every tile; the render-thread animators
// cost a render-only frame on every vsync (issues 750 and 751). The wave runs only while
// `running` is true, the item is visible, and appTheme.reduceMotion is false; otherwise every
// tile draws at `restLevel`.
Item {
    id: root

    property bool running: true
    property int columns: 3
    // 0: as many rows as fit the height; the last row is clipped.
    property int rows: 3
    property real spacing: appTheme.spaceXs
    // Tile width / tile height.
    property real tileAspect: 1
    property real tileRadius: appTheme.controlRadiusSmall
    property color tileColor: appTheme.bgBaseColor
    property color waveColor: appTheme.textColor
    // Opacity of waveColor over a tile at the crest of the wave.
    property real waveStrength: 0.12
    // Wave level of a tile that the wave does not reach (and of every tile when still).
    property real restLevel: 0
    // A tile at rest scales to 1 - scaleDepth; at the crest of the wave it scales to 1.
    property real scaleDepth: 0
    // One pass of the wave over the grid, including the rest before the next pass.
    property int cycleMs: 1400
    // Accessible name; the owner supplies the translated text.
    property string accessibleName: ""

    readonly property bool waveRunning: root.running && root.visible && !appTheme.reduceMotion
                                        && root.width > 0 && root.height > 0
    readonly property real tileWidth: root.columns > 0
                                      ? Math.max(0, (root.width - root.spacing * (root.columns - 1))
                                                 / root.columns)
                                      : 0
    readonly property real tileHeight: root.tileAspect > 0 ? root.tileWidth / root.tileAspect : 0
    readonly property int rowCount: root.rows > 0
                                    ? root.rows
                                    : (root.tileHeight > 0
                                       ? Math.max(1, Math.ceil((root.height + root.spacing)
                                                               / (root.tileHeight + root.spacing)))
                                       : 0)
    readonly property int diagonalCount: Math.max(1, root.columns + root.rowCount - 1)

    // Wave clock, 0 to 1 over cycleMs.
    property real phase: 0

    implicitWidth: 48
    implicitHeight: root.rows > 0 && root.tileAspect > 0
                    ? (root.implicitWidth - root.spacing * (root.columns - 1)) / root.columns
                      / root.tileAspect * root.rows + root.spacing * (root.rows - 1)
                    : 48
    clip: root.rows <= 0

    Accessible.role: Accessible.Animation
    Accessible.name: root.accessibleName

    // Wave level of the tiles on diagonal @p diagonal at the current phase: 0 at rest, 1 at
    // the crest. The crest takes the first 45 % of the cycle to cross the grid; a tile glows
    // for `glow` of the cycle as it passes.
    function waveLevel(diagonal) {
        if (!root.waveRunning) {
            return root.restLevel
        }
        const travel = 0.45
        const glow = 0.4
        const start = root.diagonalCount > 1 ? diagonal / (root.diagonalCount - 1) * travel : 0
        let t = root.phase - start
        if (t < 0) {
            t += 1
        }
        if (t >= glow) {
            return root.restLevel
        }
        const crest = Math.sin(Math.PI * t / glow)
        return root.restLevel + (1 - root.restLevel) * crest * crest
    }

    NumberAnimation on phase {
        from: 0
        to: 1
        duration: root.cycleMs
        loops: Animation.Infinite
        running: root.waveRunning
    }

    onWaveRunningChanged: {
        if (!root.waveRunning) {
            root.phase = 0
        }
    }

    Grid {
        columns: Math.max(1, root.columns)
        spacing: root.spacing

        Repeater {
            model: root.columns > 0 ? root.columns * root.rowCount : 0

            Rectangle {
                id: tile

                readonly property int column: index % root.columns
                readonly property int row: Math.floor(index / root.columns)
                readonly property real level: root.waveLevel(tile.row + tile.column)

                width: root.tileWidth
                height: root.tileHeight
                radius: Math.min(root.tileRadius, root.tileWidth / 2, root.tileHeight / 2)
                color: root.tileColor
                scale: 1 - root.scaleDepth * (1 - tile.level)

                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: root.waveColor
                    opacity: root.waveStrength * tile.level
                    visible: opacity > 0
                }
            }
        }
    }
}
