import QtQuick
import QtQuick.Controls

// Hairline group divider for AppContextMenu. 1 px dividerColor, spaceXs air.
MenuSeparator {
    id: control

    topPadding: appTheme.spaceXs
    bottomPadding: appTheme.spaceXs

    contentItem: Rectangle {
        implicitWidth: 1
        implicitHeight: 1
        color: appTheme.dividerColor
    }
}
