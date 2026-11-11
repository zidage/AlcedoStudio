//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick

// Compact staggered grid loader: 3 x 3 tiles that grow and light up as the wave passes
// (DESIGN.md "Library loading"). The loading mark of a photo: a Library card or tile while its
// thumbnail loads, an Editor filmstrip card, and the Library card over a kept result while a
// replacing read runs. Spacing and tile radius follow the width, so one mark serves every size.
StaggeredGridLoader {
    id: root

    implicitWidth: 24
    implicitHeight: 24
    columns: 3
    rows: 3
    spacing: Math.max(2, Math.round(root.width / 9))
    tileRadius: Math.max(1, Math.round(root.width / 18))
    tileColor: appTheme.bgBaseColor
    restLevel: 0.15
    scaleDepth: 0.3
    waveStrength: 0.85
    cycleMs: 1200
    accessibleName: qsTr("Loading")
}
