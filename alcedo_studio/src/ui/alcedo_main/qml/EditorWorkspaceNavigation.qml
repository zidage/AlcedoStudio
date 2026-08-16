import QtQuick
import QtQuick.Controls

// Shared Library/Editor navigation for the application toolbar. The router,
// policy, and editor re-entry lookup are explicit inputs so this component
// owns only workspace navigation and its permission gate.
//
// VI (DESIGN.md): monochrome capsule — sunken bgBase track, sliding light
// selected well (editorListSelectedFill), dark ink on the active segment.
// No Material style; no accent blue thumb.
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
                                                   && (editorSession && editorSession.active
                                                       ? Boolean(editorSession.actions.canSwitchWorkspace)
                                                       : (!interactionPolicy
                                                          || interactionPolicy.canSwitchWorkspace))
    readonly property string switchWorkspaceDisabledReason: interactionPolicy
                                                            ? String(interactionPolicy.switchWorkspaceReason || "")
                                                            : ""
    readonly property int opticalIconSize: appTheme.iconOpticalSizeCompact

    implicitWidth: 112
    implicitHeight: appTheme.iconButtonHitSizeCompact

    // Track / chrome (theme mirror falls back to appTheme tokens).
    readonly property color colBgBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colDivider: theme ? theme.colDivider : appTheme.dividerColor
    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colTextMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colIcon: appTheme.iconColor
    // Monochrome selected well (same family as LUT rows / method segments).
    readonly property color colSelectedFill: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor

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
            width: parent.width - appTheme.spaceXs
            height: appTheme.iconButtonHitSizeCompact - appTheme.spaceSm
            radius: appTheme.controlRadiusSmall
            color: root.colBgBase
            border.width: 1
            border.color: root.colDivider
            opacity: root.navigationEnabled ? 1.0 : 0.45
        }

        // Sliding monochrome selected well — only selected-workspace surface.
        Rectangle {
            id: wsThumb
            objectName: "workspaceSwitchThumb"
            width: wsTrack.width / 2 - appTheme.spaceXs
            height: wsTrack.height - appTheme.spaceXs
            y: wsTrack.y + appTheme.spaceXs / 2
            x: root.currentWorkspace === "library"
               ? wsTrack.x + appTheme.spaceXs / 2
               : wsTrack.x + wsTrack.width - width - appTheme.spaceXs / 2
            radius: Math.max(2, appTheme.controlRadiusSmall - 2)
            color: root.colSelectedFill
            border.width: 0
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
                // Active icon sits on the light well → dark ink; idle → muted.
                readonly property color glyphColor: !enabled
                    ? root.withAlpha(root.colTextMuted, 0.45)
                    : (isActive ? root.colSelectedInk
                                : (hovered ? root.colText : root.colIcon))

                HoverHandler { id: libraryNavHover }

                icon.source: "qrc:/panel_icons/layout-grid.svg"
                icon.width: root.opticalIconSize
                icon.height: root.opticalIconSize
                icon.color: libraryNavButton.glyphColor
                // Capsule exception (DESIGN.md): no hover/press/focus fill —
                // the sliding thumb is the only selected surface.
                background: Item {}
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
                readonly property color glyphColor: !enabled
                    ? root.withAlpha(root.colTextMuted, 0.45)
                    : (isActive ? root.colSelectedInk
                                : (hovered ? root.colText : root.colIcon))

                HoverHandler { id: editorNavHover }

                icon.source: "qrc:/panel_icons/adjustments.svg"
                icon.width: root.opticalIconSize
                icon.height: root.opticalIconSize
                icon.color: editorNavButton.glyphColor
                background: Item {}
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
