import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Application top toolbar: brand, workspace switch, File menu, Settings,
// Inspector toggle, and frameless window caption buttons.
Rectangle {
    id: root
    property var theme: null
    property var host: null
    Layout.fillWidth: true
    Layout.preferredHeight: 56
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
        anchors.leftMargin: 20
        anchors.rightMargin: 0
        spacing: 10
        Row {
            spacing: 0
            Label { text: qsTr("Alcedo"); font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily; font.pixelSize: 19; font.weight: 700; color: root.theme ? root.theme.colAccentPrimary : appTheme.accentColor }
            Label { text: " "; font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily; font.pixelSize: 19; font.weight: 700 }
            Label { text: qsTr("Studio"); font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily; font.pixelSize: 19; font.weight: 700; color: root.theme ? root.theme.colText : appTheme.textColor }
        }
        Item { Layout.preferredWidth: 12 }

        EditorWorkspaceNavigation {
            id: workspaceSwitch
            objectName: "workspaceSwitch"
            Layout.preferredWidth: 112
            Layout.preferredHeight: 40
            theme: root.host
            workspaceRouter: appModules.workspaceRouter
            interactionPolicy: appModules.interactionPolicy
            editorSession: appModules.editorSession
            editorImageExists: root.host ? root.host.editorImageStillExists : null
            navigationEnabled: appModules.project.serviceReady
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 22
            color: root.theme ? root.theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
        }

        // ── File menu ──
        Button {
            id: fileMenuButton
            text: qsTr("File")
            flat: true
            Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
            onClicked: fileMenu.open()

            Menu {
                id: fileMenu
                x: 0
                y: fileMenuButton.height + 4

                MenuItem {
                    text: qsTr("Load Project")
                    enabled: root.host && !root.host.projectLaunchBusy && !appModules.project.acceleratorPreparing
                    onTriggered: root.host.beginProjectLaunch(function() {
                        return appModules.project.PromptAndLoadProject()
                    })
                }
                MenuItem {
                    text: qsTr("Create Project")
                    enabled: root.host && !root.host.projectLaunchBusy && !appModules.project.acceleratorPreparing
                    onTriggered: root.host.beginProjectLaunch(function() {
                        return appModules.project.PromptAndCreateProject()
                    })
                }
                MenuSeparator {
                }
                MenuItem {
                    text: qsTr("Save Project")
                    enabled: root.host && root.host.backendInteractive
                    onTriggered: root.host.requestSaveProject()
                }
            }
        }

        Button {
            id: settingsPopoutButton
            text: qsTr("Settings")
            flat: true
            Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
            onClicked: if (root.host) root.host.openSettingsDialog()
        }

        Item { Layout.fillWidth: true }

        // Inspector toggle lives on the application top toolbar (52×42,
        // icon 24×24) — same placement and size as before workspace extraction.
        InspectorToggleButton {
            theme: root.theme
            host: root.host
        }

        // ── Frameless window caption buttons ──
        Item { Layout.preferredWidth: 8 }

        WindowCaptionButtons {
            theme: root.theme
            host: root.host
        }
    }
}