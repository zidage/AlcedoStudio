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
    // True while the host width follows an animation (the manual fold of the collections
    // sidebar). The shown workspace follows the width on every frame and defers the work that
    // only its final size needs (thumbnail cache size, editor render size).
    property bool geometryAnimating: false

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
    readonly property bool revealRunning: revealAnimation.running

    // The workspace that this host sizes; "" before the first sync.
    property string _shownWorkspace: ""
    // Horizontal travel of the fade-in after a switch, toward the side of the toolbar button
    // (the editor comes from the right, the Library from the left).
    readonly property real revealTravel: 10

    // The router has published a new workspace. Main.qml applies the collections sidebar
    // state of @p workspace at once in its handler, so the host width is final before the
    // shown workspace is sized.
    signal workspaceShowing(string workspace)

    function focusActiveWorkspace() {
        const item = root.libraryActive ? libraryLoader.item : editorLoader.item
        if (item && item.forceActiveFocus) {
            item.forceActiveFocus()
        }
    }

    function routerWorkspace() {
        return root.workspaceRouter ? String(root.workspaceRouter.workspace || "library")
                                    : "library"
    }

    // Sizes the shown workspace. A hidden workspace keeps its last size: a manual fold of the
    // collections sidebar changes this host on every frame, and a hidden workspace that
    // followed it would lay out on every frame as well (grid relayout, thumbnail cache size,
    // editor viewport metrics).
    // The workspace is read from the router, not from libraryActive/editorActive, so that the
    // first handler of a route change sees it. On a switch, workspaceShowing runs first: the
    // sidebar takes its new state, the host width changes in the same call (anchors), and the
    // shown workspace takes its final width once. It then fades in.
    function syncWorkspaceSizes() {
        const workspace = root.routerWorkspace()
        if (workspace === root._shownWorkspace) {
            root.sizeShownWorkspace()
            return
        }
        const switched = root._shownWorkspace.length > 0
        root._shownWorkspace = workspace
        root.workspaceShowing(workspace)
        root.sizeShownWorkspace()
        if (switched) {
            root.revealShownWorkspace()
        }
    }

    function sizeShownWorkspace() {
        const shown = root._shownWorkspace === "editor" ? editorLoader : libraryLoader
        shown.width = root.width
        shown.height = root.height
    }

    // Applies the pending layouts of the shown workspace now (issue 754). A RowLayout or
    // ColumnLayout can apply a new width in its next polish, so after the last frame of a fold
    // the content can still have the previous width. Each layout is polished before the layouts
    // inside it, so every layout works with the final size of its parent. Hidden items and the
    // content of views (Flickable) are skipped: their delegates do not use these layouts.
    function settleShownWorkspaceLayouts() {
        const shown = root._shownWorkspace === "editor" ? editorLoader : libraryLoader
        root.polishLayoutsIn(shown.item)
    }

    function polishLayoutsIn(item) {
        if (!item || !item.visible) {
            return
        }
        if (item instanceof Layout) {
            item.ensurePolished()
        }
        if (item instanceof Flickable) {
            return
        }
        const children = item.children
        for (let i = 0; i < children.length; i++) {
            root.polishLayoutsIn(children[i])
        }
    }

    // The new workspace fades in at its final size with a short slide. Its layout does not
    // change: only opacity and x move.
    function revealShownWorkspace() {
        revealAnimation.stop()
        libraryLoader.opacity = 1
        libraryLoader.x = 0
        editorLoader.opacity = 1
        editorLoader.x = 0
        if (appTheme.reduceMotion) {
            return
        }
        const editor = root._shownWorkspace === "editor"
        revealAnimation.target = editor ? editorLoader : libraryLoader
        revealAnimation.fromX = editor ? root.revealTravel : -root.revealTravel
        revealAnimation.start()
    }

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

    ParallelAnimation {
        id: revealAnimation
        property Item target: null
        property real fromX: 0
        NumberAnimation {
            target: revealAnimation.target
            property: "opacity"
            from: 0
            to: 1
            duration: appTheme.motionFadeMs
            easing.type: appTheme.motionEasing
        }
        NumberAnimation {
            target: revealAnimation.target
            property: "x"
            from: revealAnimation.fromX
            to: 0
            duration: appTheme.motionFadeMs
            easing.type: appTheme.motionEasing
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
            geometryAnimating: root.geometryAnimating && root.libraryActive
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
            geometryAnimating: root.geometryAnimating && root.editorActive
            Component.onCompleted: root.editorCreateCount += 1
            Component.onDestruction: root.editorDestroyCount += 1
        }
    }
}
