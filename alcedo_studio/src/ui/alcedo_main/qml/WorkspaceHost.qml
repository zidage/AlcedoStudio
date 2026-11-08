import QtQuick
import QtQuick.Layouts

// Owns workspace layout. Each workspace is created on first visit and then
// kept: switching only flips visibility. Destroying EditorViewportItem,
// the six adjustment panels, and the library MultiEffect grid on every
// switch blocked the GUI thread for a full event.
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

    // Once true, the matching Loader stays active across later switches.
    property bool libraryRetained: false
    property bool editorRetained: false

    // Exposed for tests and diagnostics.
    readonly property Item libraryItem: libraryLoader.item
    readonly property Item editorItem: editorLoader.item
    readonly property bool libraryVisible: libraryLoader.visible
    readonly property bool editorVisible: editorLoader.visible
    readonly property int activeLoaderCount: (libraryLoader.active ? 1 : 0)
                                             + (editorLoader.active ? 1 : 0)
    // Create/destroy counters catch leaked trees that activeLoaderCount alone cannot.
    property int libraryCreateCount: 0
    property int libraryDestroyCount: 0
    property int editorCreateCount: 0
    property int editorDestroyCount: 0

    function focusActiveWorkspace() {
        const item = root.libraryActive ? libraryLoader.item : editorLoader.item
        if (item && item.forceActiveFocus) {
            item.forceActiveFocus()
        }
    }

    // A hidden workspace keeps its last size. Each workspace switch folds or opens the
    // collections sidebar, and its width animation changes this host on every frame; a hidden
    // workspace that followed it would lay out on every frame as well (grid relayout, thumbnail
    // cache size, editor viewport metrics). The shown workspace takes the current size, once
    // when it is shown and then on each change.
    // The workspace is read from the router, not from libraryActive/editorActive: Main.qml
    // folds the sidebar in its own handler of the same route change, which can run before
    // these bindings follow the route.
    function syncWorkspaceSizes() {
        const workspace = root.workspaceRouter
                ? String(root.workspaceRouter.workspace || "library")
                : "library"
        const shown = workspace === "editor" ? editorLoader : libraryLoader
        shown.width = root.layoutWidth > 0 ? root.layoutWidth : root.width
        shown.height = root.height
    }

    // The width of the shown workspace after the collections sidebar fold (Main.qml); -1 uses
    // the host width. While the fold animates the host width, the shown workspace keeps this
    // width and stays aligned to the right edge: the sidebar slides over or out of it, and the
    // workspace lays out once instead of on every frame (the Library grid layout took 30-40 ms
    // a frame, and each editor viewport size started a render).
    property real layoutWidth: -1
    onLayoutWidthChanged: root.syncWorkspaceSizes()

    onWidthChanged: root.syncWorkspaceSizes()
    onHeightChanged: root.syncWorkspaceSizes()
    onActiveWorkspaceChanged: root.syncWorkspaceSizes()
    Component.onCompleted: root.syncWorkspaceSizes()

    Connections {
        target: root.workspaceRouter
        ignoreUnknownSignals: true
        // RouteChanged notifies the workspace property (an upper-case signal has no QML handler).
        function onWorkspaceChanged() { root.syncWorkspaceSizes() }
    }

    onLibraryActiveChanged: {
        if (libraryActive) {
            libraryRetained = true
            Qt.callLater(root.focusActiveWorkspace)
        }
    }
    onEditorActiveChanged: {
        if (editorActive) {
            editorRetained = true
            Qt.callLater(root.focusActiveWorkspace)
        }
    }

    // Lazy-load / retain rules:
    // - First visit creates the tree (synchronous so tests and first paint see it).
    // - Later switches keep the inactive tree and only hide it.
    // - Shared backend models remain alive on appModules.
    // - Filmstrip collapse preference lives on editorSession (QSettings-backed)
    //   so it survives application restart.
    Loader {
        id: libraryLoader
        objectName: "libraryWorkspaceLoader"
        x: root.width - width
        active: root.libraryActive || root.libraryRetained
        asynchronous: false
        visible: status === Loader.Ready && root.libraryActive
        enabled: visible
        sourceComponent: libraryComponent

        onLoaded: {
            root.libraryRetained = true
            Qt.callLater(root.focusActiveWorkspace)
        }
    }

    Loader {
        id: editorLoader
        objectName: "editorWorkspaceLoader"
        x: root.width - width
        active: root.editorActive || root.editorRetained
        asynchronous: false
        visible: status === Loader.Ready && root.editorActive
        enabled: visible
        sourceComponent: editorComponent

        onLoaded: {
            root.editorRetained = true
            Qt.callLater(root.focusActiveWorkspace)
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
            host: root.host
            workspaceRouter: root.workspaceRouter
            editorSession: appModules.editorSession
            blurSource: root.host ? root.host.dialogBlurSource : null
            Component.onCompleted: root.editorCreateCount += 1
            Component.onDestruction: root.editorDestroyCount += 1
        }
    }
}
