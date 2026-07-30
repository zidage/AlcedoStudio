import QtQuick
import QtQuick.Layouts

// Owns workspace layout and lazy loading. Inactive expensive visual trees are
// destroyed by setting Loader.active false so repeated switches do not retain
// inactive objects or grow the QML graph.
Item {
    id: root
    objectName: "workspaceHost"

    property var theme: null
    property var host: null
    property var workspaceRouter: appModules.workspaceRouter

    readonly property string activeWorkspace: workspaceRouter
                                              ? String(workspaceRouter.workspace || "library")
                                              : "library"
    readonly property bool libraryActive: activeWorkspace === "library"
    readonly property bool editorActive: activeWorkspace === "editor"

    // Exposed for tests and diagnostics.
    readonly property Item libraryItem: libraryLoader.item
    readonly property Item editorItem: editorLoader.item
    readonly property int activeLoaderCount: (libraryLoader.active ? 1 : 0)
                                             + (editorLoader.active ? 1 : 0)
    // Create/destroy counters catch leaked trees that activeLoaderCount alone cannot.
    property int libraryCreateCount: 0
    property int libraryDestroyCount: 0
    property int editorCreateCount: 0
    property int editorDestroyCount: 0

    // Lazy-load / teardown rules:
    // - Only the active workspace Loader is active.
    // - Switching away sets active=false, which destroys the visual tree.
    // - Shared backend models remain alive on appModules; only QML trees unload.
    // - Filmstrip collapse preference lives on editorSession (QSettings-backed)
    //   so it survives editor teardown and application restart.
    Loader {
        id: libraryLoader
        objectName: "libraryWorkspaceLoader"
        anchors.fill: parent
        active: root.libraryActive
        asynchronous: false
        visible: status === Loader.Ready && root.libraryActive
        sourceComponent: libraryComponent

        onLoaded: {
            Qt.callLater(function() {
                if (item && item.forceActiveFocus) {
                    item.forceActiveFocus()
                }
            })
        }
    }

    Loader {
        id: editorLoader
        objectName: "editorWorkspaceLoader"
        anchors.fill: parent
        active: root.editorActive
        asynchronous: false
        visible: status === Loader.Ready && root.editorActive
        sourceComponent: editorComponent

        onLoaded: {
            Qt.callLater(function() {
                if (item && item.forceActiveFocus) {
                    item.forceActiveFocus()
                }
            })
        }
    }

    Component {
        id: libraryComponent
        LibraryWorkspace {
            theme: root.theme
            host: root.host
            Component.onCompleted: root.libraryCreateCount += 1
            Component.onDestruction: root.libraryDestroyCount += 1
        }
    }

    Component {
        id: editorComponent
        EditorWorkspace {
            theme: root.theme
            workspaceRouter: root.workspaceRouter
            editorSession: appModules.editorSession
            blurSource: root.host ? root.host.dialogBlurSource : null
            Component.onCompleted: root.editorCreateCount += 1
            Component.onDestruction: root.editorDestroyCount += 1
        }
    }
}
