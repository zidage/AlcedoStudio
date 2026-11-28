//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

Rectangle {
    id: mark
    width: appTheme.iconButtonHitSizeCompact
    height: width
    radius: appTheme.controlRadiusSmall
    color: appTheme.thumbnailDetailsScrimColor

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Source changed since import")
    ToolTip.visible: visible && pointer.hovered
    ToolTip.text: mark.Accessible.name

    IconImage {
        anchors.centerIn: parent
        width: appTheme.iconOpticalSizeCompact
        height: width
        source: "qrc:/panel_icons/photo-off.svg"
        color: appTheme.textColor
    }
    HoverHandler { id: pointer }
}
