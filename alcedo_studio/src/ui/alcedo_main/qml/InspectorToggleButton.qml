import QtQuick

// Right-sidebar toggle on the application top toolbar.
// Library: inspector. Editor: adjustment stack. Each panel keeps its own
// expanded state; only the target changes with the workspace.
IconActionButton {
    id: root
    objectName: "libraryInspectorToggle"
    property var theme: null
    property var host: null

    compact: true
    focusOnPointerPress: false
    showFocusRing: false
    fillIdle: activeFocus
              ? appTheme.buttonHoveredFillColor
              : appTheme.buttonIdleFillColor
    visible: host
    readonly property bool editorWorkspaceActive: host
                                                  && String(host.activeWorkspace || "") === "editor"
    readonly property bool panelExpanded: host && host.activeRightSidebarExpanded !== undefined
                                          ? host.activeRightSidebarExpanded === true
                                          : (host && host.libraryInspectorVisible === true)
    actionName: root.editorWorkspaceActive
                ? (root.panelExpanded
                   ? qsTr("Hide editor panels")
                   : qsTr("Show editor panels"))
                : (root.panelExpanded
                   ? qsTr("Collapse Inspector")
                   : qsTr("Expand Inspector"))
    iconSrc: root.panelExpanded
             ? "qrc:/panel_icons/layout-sidebar-right.svg"
             : "qrc:/panel_icons/layout-sidebar-right-inactive.svg"
    onClicked: if (host) host.toggleActiveRightSidebar()
}
