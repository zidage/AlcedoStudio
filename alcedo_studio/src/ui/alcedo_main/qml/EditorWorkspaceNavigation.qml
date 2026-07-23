import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shared Library/Editor navigation for the application header. The router,
// policy, and editor re-entry lookup are explicit inputs so this component
// owns only workspace navigation and its permission gate.
Item {
    id: root
    objectName: "editorWorkspaceNavigation"

    property var theme: null
    property var workspaceRouter: null
    property var interactionPolicy: null
    property var editorSession: null
    property var editorImageExists: null
    property bool navigationEnabled: true

    readonly property string currentWorkspace: workspaceRouter
                                               ? String(workspaceRouter.workspace || "library")
                                               : "library"
    readonly property bool switchWorkspaceEnabled: navigationEnabled
                                                   && (!interactionPolicy
                                                       || interactionPolicy.canSwitchWorkspace)
    readonly property string switchWorkspaceDisabledReason: interactionPolicy
                                                            ? String(interactionPolicy.switchWorkspaceReason || "")
                                                            : ""
    readonly property int opticalIconSize: appTheme.iconOpticalSizeCompact

    implicitWidth: 112
    implicitHeight: 40

    readonly property color colBgBase: theme ? theme.colBgBase : "#242424"
    readonly property color colDivider: theme ? theme.colDivider : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#E0E0E0"
    readonly property color colTextMuted: theme ? theme.colTextMuted : "#888888"
    readonly property color colAccentPrimary: theme ? theme.colAccentPrimary : "#6892B9"
    readonly property color colAccentSecondary: theme ? theme.colAccentSecondary : "#76A0C7"

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    // Returns whether the last editor image can be used for re-entry.
    // A missing callback means the caller has no existence restriction.
    function editorImageIsAvailable(elementId) {
        return !editorImageExists || Boolean(editorImageExists(elementId))
    }

    // Requests the library workspace after applying the authoritative policy.
    // A blocked request has no router side effect.
    function openLibrary() {
        if (!switchWorkspaceEnabled || !workspaceRouter
                || currentWorkspace === "library") {
            return
        }
        workspaceRouter.openLibrary()
    }

    // Requests the editor workspace and restores the last valid image when one
    // is available. A blocked request has no router side effect.
    function openEditor() {
        if (!switchWorkspaceEnabled || !workspaceRouter
                || currentWorkspace === "editor") {
            return
        }

        const lastElementId = editorSession ? Number(editorSession.lastElementId || 0) : 0
        const lastImageId = editorSession ? Number(editorSession.lastImageId || 0) : 0
        if (lastElementId > 0 && lastImageId > 0
                && editorImageIsAvailable(lastElementId)) {
            workspaceRouter.openEditor(lastElementId, lastImageId)
        } else {
            workspaceRouter.openEditor(0, 0)
        }
    }

    Item {
        id: workspaceSwitch
        objectName: "workspaceSwitch"
        anchors.fill: parent

        Rectangle {
            id: wsTrack
            objectName: "workspaceSwitchTrack"
            anchors.centerIn: parent
            width: parent.width - 4
            height: 32
            radius: appTheme.controlRadiusSmall
            color: root.colBgBase
            border.width: 1
            border.color: root.colDivider
            opacity: root.navigationEnabled ? 1.0 : 0.45
        }

        Rectangle {
            id: wsThumb
            objectName: "workspaceSwitchThumb"
            width: wsTrack.width / 2 - 2
            height: wsTrack.height - 4
            y: wsTrack.y + 2
            x: root.currentWorkspace === "library"
               ? wsTrack.x + 2
               : wsTrack.x + wsTrack.width - width - 2
            radius: appTheme.controlRadiusSmall - 2
            color: root.colAccentPrimary
            border.width: 1
            border.color: root.colAccentSecondary
            opacity: root.navigationEnabled ? 1.0 : 0.45

            Behavior on x {
                NumberAnimation {
                    duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
                    easing.type: Easing.OutCubic
                }
            }
        }

        Row {
            anchors.fill: parent
            spacing: 0

            Button {
                id: libraryNavButton
                objectName: "libraryNavButton"
                width: parent.width / 2
                height: parent.height
                flat: true
                padding: 0
                display: AbstractButton.IconOnly
                enabled: root.switchWorkspaceEnabled
                activeFocusOnTab: true
                readonly property bool isActive: root.currentWorkspace === "library"
                readonly property string actionName: qsTr("Library")

                HoverHandler { id: libraryNavHover }

                icon.source: "qrc:/panel_icons/layout-grid.svg"
                icon.width: root.opticalIconSize
                icon.height: root.opticalIconSize
                icon.color: !enabled
                            ? root.withAlpha(root.colText, 0.30)
                            : (isActive || hovered ? root.colText : root.colTextMuted)
                Material.foreground: icon.color
                background: Rectangle {
                    color: "transparent"
                    radius: 4
                }
                ToolTip.visible: libraryNavHover.hovered
                ToolTip.text: libraryNavButton.actionName
                Accessible.name: libraryNavButton.actionName
                Accessible.description: root.switchWorkspaceDisabledReason
                Accessible.role: Accessible.Button
                onClicked: root.openLibrary()
            }

            Button {
                id: editorNavButton
                objectName: "editorNavButton"
                width: parent.width / 2
                height: parent.height
                flat: true
                padding: 0
                display: AbstractButton.IconOnly
                enabled: root.switchWorkspaceEnabled
                activeFocusOnTab: true
                readonly property bool isActive: root.currentWorkspace === "editor"
                readonly property string actionName: qsTr("Editor")

                HoverHandler { id: editorNavHover }

                icon.source: "qrc:/panel_icons/adjustments.svg"
                icon.width: root.opticalIconSize
                icon.height: root.opticalIconSize
                icon.color: !enabled
                            ? root.withAlpha(root.colText, 0.30)
                            : (isActive || hovered ? root.colText : root.colTextMuted)
                Material.foreground: icon.color
                background: Rectangle {
                    color: "transparent"
                    radius: 4
                }
                ToolTip.visible: editorNavHover.hovered
                ToolTip.text: editorNavButton.actionName
                Accessible.name: editorNavButton.actionName
                Accessible.description: root.switchWorkspaceDisabledReason
                Accessible.role: Accessible.Button
                onClicked: root.openEditor()
            }
        }
    }
}
