import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Full-width application toolbar: native traffic-light reserve, collections
// toggle, workspace switch, File, Settings, update, inspector, and caption
// actions. The wordmark remains on the collections card below this bar.
Rectangle {
    id: root
    objectName: "topToolbar"
    property var theme: null
    property var host: null
    readonly property bool nativeTrafficLightsEnabled: host
                                                       && host.nativeTrafficLightsEnabled === true
    readonly property bool collectionsSidebarExpanded: !host
                                                       || host.collectionsSidebarExpanded === true
    Layout.fillWidth: true
    // One compact 40 px action band plus 4 px breathing room above and below.
    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact + appTheme.spaceSm
    radius: theme ? theme.panelRadius : 12
    color: theme ? theme.colGlassPanel : "#1C1C1D"
    border.width: 1
    border.color: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    z: 1

    // Drag the window from any empty area of the toolbar; double-click toggles maximize.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onDoubleTapped: if (root.host) root.host.toggleMaximizeAnimated()
    }
    DragHandler {
        target: null
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onActiveChanged: if (active && root.host) root.host.startSystemMove()
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: appTheme.spaceXl
        anchors.rightMargin: 0
        spacing: 10

        Item {
            Layout.preferredWidth: appTheme.iconButtonHitSizeCompact + appTheme.spaceXl
            Layout.preferredHeight: 1
            visible: root.nativeTrafficLightsEnabled
        }

        IconActionButton {
            id: sidebarToggle
            objectName: "collectionsSidebarToggle"
            compact: true
            focusOnPointerPress: false
            showFocusRing: false
            fillIdle: activeFocus
                      ? appTheme.buttonHoveredFillColor
                      : appTheme.buttonIdleFillColor
            actionName: root.collectionsSidebarExpanded
                        ? qsTr("Hide collections sidebar")
                        : qsTr("Show collections sidebar")
            iconSrc: root.collectionsSidebarExpanded
                     ? "qrc:/panel_icons/layout-sidebar.svg"
                     : "qrc:/panel_icons/layout-sidebar-inactive.svg"
            onClicked: if (root.host) root.host.toggleCollectionsSidebar()
        }

        EditorWorkspaceNavigation {
            id: workspaceSwitch
            Layout.preferredWidth: 112
            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
            theme: root.host
            workspaceRouter: appModules.workspaceRouter
            interactionPolicy: appModules.interactionPolicy
            editorSession: appModules.editorSession
            editorImageExists: root.host ? root.host.editorImageStillExists : null
            firstEditorImage: root.host ? root.host.firstLibraryImage : null
            navigationEnabled: appModules.project.serviceReady
        }

        // ── File menu ──
        Button {
            id: fileMenuButton
            objectName: "fileMenuButton"
            text: qsTr("File")
            flat: true
            Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
            onClicked: fileMenu.open()

            AppContextMenu {
                id: fileMenu
                objectName: "fileMenu"
                x: 0
                y: fileMenuButton.height + 4

                AppMenuItem {
                    objectName: "fileMenuLoadProject"
                    text: qsTr("Load Project")
                    enabled: root.host && !root.host.projectLaunchBusy && !appModules.project.acceleratorPreparing
                    onTriggered: root.host.beginProjectLaunch(function() {
                        return appModules.project.PromptAndLoadProject()
                    })
                }
                AppMenuItem {
                    objectName: "fileMenuCreateProject"
                    text: qsTr("Create Project")
                    enabled: root.host && !root.host.projectLaunchBusy && !appModules.project.acceleratorPreparing
                    onTriggered: root.host.beginProjectLaunch(function() {
                        return appModules.project.PromptAndCreateProject()
                    })
                }
                AppMenuSeparator {
                }
                AppMenuItem {
                    objectName: "fileMenuSaveProject"
                    text: qsTr("Save Project")
                    enabled: root.host && root.host.backendInteractive
                    onTriggered: root.host.requestSaveProject()
                }
            }
        }

        Button {
            id: settingsPopoutButton
            objectName: "settingsPopoutButton"
            text: qsTr("Settings")
            flat: true
            Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
            onClicked: if (root.host) root.host.openSettingsDialog()
        }

        Item {
            id: updateEntry
            Layout.preferredWidth: updateButton.hitSize
            Layout.preferredHeight: updateButton.hitSize
            visible: appModules.updates
                     && (appModules.updates.updateDeferred || appModules.updates.updateAvailable)
            Accessible.name: qsTr("Update available")
            Accessible.role: Accessible.Button

            IconActionButton {
                id: updateButton
                anchors.centerIn: parent
                compact: true
                actionName: qsTr("Update available")
                iconSrc: "qrc:/panel_icons/update.svg"
                onClicked: {
                    if (root.host)
                        root.host.openUpdateSettings()
                }
            }

            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: 4
                anchors.topMargin: 4
                width: 8
                height: 8
                radius: 4
                color: appTheme.backgroundTaskFinishedColor
                Accessible.ignored: true
            }
        }

        Item { Layout.fillWidth: true }

        // Inspector toggle: compact IconActionButton, same hit as the
        // collections sidebar control. Visible only in the Library workspace.
        InspectorToggleButton {
            theme: root.theme
            host: root.host
        }

        // Drawn caption buttons stay off macOS so they do not sit next to the
        // system traffic lights.
        Item {
            Layout.preferredWidth: 8
            visible: captionButtons.visible
        }

        WindowCaptionButtons {
            id: captionButtons
            visible: !root.nativeTrafficLightsEnabled
            theme: root.theme
            host: root.host
        }
    }
}
