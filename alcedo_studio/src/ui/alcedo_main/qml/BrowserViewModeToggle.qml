import QtQuick
import QtQuick.Controls

// Grid / list switch of the browser toolbars (LUT browser, preset browser): the
// monochrome segmented family (sunken track, light well under the current segment),
// sized to the toolbar row. The owner keeps the mode; a click asks for the new one.
Rectangle {
    id: track

    // "grid" or "list".
    property string viewMode: "grid"
    property int chromeHeight: Math.max(appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
                                        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    // Object name prefix of the parts: <prefix>ViewModeThumb, <prefix>GridViewButton,
    // <prefix>ListViewButton.
    property string namePrefix: ""

    signal viewModeRequested(string viewMode)

    readonly property int inset: appTheme.spaceXs / 2
    readonly property int segment: chromeHeight - inset * 2

    implicitWidth: segment * 2 + inset * 2
    implicitHeight: chromeHeight
    radius: appTheme.controlRadiusSmall
    color: appTheme.bgBaseColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    Rectangle {
        objectName: track.namePrefix + "ViewModeThumb"
        x: track.inset + (track.viewMode === "list" ? track.segment : 0)
        y: track.inset
        width: track.segment
        height: track.segment
        radius: Math.max(0, appTheme.controlRadiusSmall - track.inset)
        color: appTheme.editorListSelectedFillColor
        Behavior on x {
            enabled: !appTheme.reduceMotion
            NumberAnimation {
                duration: appTheme.motionFoldOpenMs
                easing.type: appTheme.motionEasing
            }
        }
    }

    Row {
        x: track.inset
        y: track.inset

        Repeater {
            model: [
                { key: "grid", icon: "qrc:/panel_icons/view-grid.svg",
                  label: qsTr("Grid view"), name: "GridViewButton" },
                { key: "list", icon: "qrc:/panel_icons/view-list.svg",
                  label: qsTr("List view"), name: "ListViewButton" }
            ]
            delegate: IconActionButton {
                required property var modelData
                objectName: track.namePrefix + modelData.name
                width: track.segment
                height: track.segment
                compact: true
                selected: track.viewMode === modelData.key
                showHoverFill: !selected
                showFocusRing: false
                iconSrc: modelData.icon
                actionName: modelData.label
                iconColorDefault: selected ? appTheme.editorListSelectedInkColor
                                           : appTheme.textMutedColor
                iconColorMuted: appTheme.textMutedColor
                iconColorSelected: appTheme.editorListSelectedInkColor
                fillIdle: "transparent"
                fillSelected: "transparent"
                fillHover: appTheme.buttonHoveredFillColor
                onClicked: track.viewModeRequested(modelData.key)
            }
        }
    }
}
