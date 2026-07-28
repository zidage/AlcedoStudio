import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Library Inspector show/hide toggle on the application top toolbar.
Button {
    id: root
    objectName: "libraryInspectorToggle"
    property var theme: null
    property var host: null
    checkable: false
    flat: true
    Layout.preferredWidth: 52
    Layout.preferredHeight: 42
    display: AbstractButton.IconOnly
    visible: host && appModules.workspaceRouter.workspace === "library"
    activeFocusOnTab: true
    property real iconRotationTarget: host && host.libraryInspectorVisible ? 180 : 0
    icon.source: "qrc:/panel_icons/inspector-expand.svg"
    icon.width: appTheme.iconOpticalSize
    icon.height: appTheme.iconOpticalSize
    icon.color: host && host.libraryInspectorVisible
                ? (theme ? theme.colAccentPrimary : appTheme.accentColor)
                : (root.hovered ? (theme ? theme.colText : appTheme.textColor)
                                : (theme ? theme.colTextMuted : appTheme.textMutedColor))
    Material.foreground: root.icon.color
    ToolTip.visible: root.hovered
    ToolTip.text: host && host.libraryInspectorVisible ? qsTr("Collapse Inspector") : qsTr("Expand Inspector")
    Accessible.name: ToolTip.text
    background: Rectangle {
        radius: theme ? theme.controlRadius : 10
        color: "transparent"
        border.width: 0
    }
    onContentItemChanged: {
        inspectorIconRotate.target = contentItem
        if (contentItem) {
            contentItem.transformOrigin = Item.Center
            contentItem.rotation = root.iconRotationTarget
        }
    }
    onIconRotationTargetChanged: {
        if (contentItem) {
            inspectorIconRotate.stop()
            inspectorIconRotate.to = root.iconRotationTarget
            inspectorIconRotate.start()
        }
    }
    Component.onCompleted: {
        if (contentItem) {
            contentItem.transformOrigin = Item.Center
            contentItem.rotation = root.iconRotationTarget
        }
    }
    NumberAnimation {
        id: inspectorIconRotate
        property: "rotation"
        duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldCloseMs
        easing.type: Easing.OutCubic
    }
    onClicked: if (host) host.libraryInspectorVisible = !host.libraryInspectorVisible
}