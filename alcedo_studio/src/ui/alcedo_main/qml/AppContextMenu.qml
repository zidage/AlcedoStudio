import QtQuick
import QtQuick.Controls

// Shared dark popup menu for Alcedo chrome (Library grid, editor filmstrip,
// toolbar dropdowns). Visual identity: DESIGN.md "Context menus" —
// bgBaseColor surface, panelRadius shell, dividerColor hairline, hoverColor
// row wash via the AppMenuItem default delegate (also styles sub-menu rows).
Menu {
    id: control

    delegate: AppMenuItem {}
    padding: appTheme.spaceXs

    function openAt(sceneX, sceneY) {
        x = Math.max(0, sceneX)
        y = Math.max(0, sceneY)
        open()
    }

    background: Rectangle {
        implicitWidth: 180
        radius: appTheme.panelRadius
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: appTheme.dividerColor
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0.0
            to: 1.0
            duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
        }
    }

    exit: Transition {
        NumberAnimation {
            property: "opacity"
            from: 1.0
            to: 0.0
            duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
        }
    }
}
