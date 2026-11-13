import QtQuick
import QtQml

// Non-visual data source of the welcome surface. It reads the previewed
// project from appModules.project and the cover rows from the Library
// thumbnail model, so the welcome view binds to this object only.
//
// Cover rows are rows 0-2 of the Library thumbnail model in the Library
// order. The adapter pins their thumbnails with the Library grid tier
// (thumbnailMaxEdge) so it shares the grid pins. It takes at most three pins
// and releases every pin it took when a cover row changes, when it becomes
// inactive, and on destruction.
QtObject {
    id: root

    property var projectModule: appModules.project
    property var thumbnailModel: appModules.library ? appModules.library.thumbnailModel : null
    // Thumbnail tier of the Library grid; 0 disables the pins.
    property int thumbnailMaxEdge: 0
    // False while the welcome surface is closed.
    property bool active: true

    readonly property string welcomeProjectPath: root.projectModule ? String(root.projectModule.welcomeProjectPath || "") : ""
    // Path that the upper area describes. Before the startup preview starts, the
    // first recent entry (the project that the preview will load).
    readonly property string projectPath: root.welcomeProjectPath.length > 0
                                          ? root.welcomeProjectPath
                                          : (root.recentProjects && root.recentProjects.length > 0
                                             ? String(root.recentProjects[0].path || "") : "")
    readonly property string errorText: root.projectModule ? String(root.projectModule.previewErrorMessage || "") : ""
    readonly property var recentProjects: root.projectModule ? root.projectModule.recentProjects : []
    readonly property bool loadRunning: !!root.projectModule && root.projectModule.projectLoading
    readonly property var overview: root.projectModule ? root.projectModule.projectOverview : ({})

    // "empty": no recent project and nothing to preview.
    // "loading": a preview load runs, or the startup preview has not started yet.
    // "ready": the previewed project is loaded.
    // "failed": the last preview load failed; errorText holds the real error.
    readonly property string state: {
        if (root.errorText.length > 0)
            return "failed"
        if (root.welcomeProjectPath.length === 0)
            return root.projectPath.length > 0 ? "loading" : "empty"
        if (root.loadRunning)
            return "loading"
        if (root.projectModule && root.projectModule.serviceReady
                && root.overview && root.overview.photoCount !== undefined)
            return "ready"
        return "loading"
    }

    readonly property string projectName: root.nameForPath(root.projectPath)
    readonly property int photoCount: root.state === "ready" ? Number(root.overview.photoCount) : 0
    readonly property int editedPhotoCount: root.state === "ready" ? Number(root.overview.editedPhotoCount) : 0
    readonly property string captureRangeText: root.state === "ready"
                                                ? root.formatCaptureRange(root.overview.earliestCaptureDate,
                                                                          root.overview.latestCaptureDate)
                                                : ""
    readonly property bool hasCaptureRange: root.state === "ready" && root.isValidDate(root.overview.earliestCaptureDate)

    // Up to 3 entries {elementId, imageId, thumbUrl, thumbLoading}.
    property var coverItems: []

    // "继续编辑" is available only for a ready preview. It is disabled while any
    // project load runs and in the failed state.
    readonly property bool continueEnabled: root.state === "ready" && !root.loadRunning
    // One load at a time: recent rows, open and create wait for the running load.
    readonly property bool selectionEnabled: !root.loadRunning

    property var pinnedCovers: []

    function nameForPath(path) {
        if (!path || path.length === 0)
            return ""
        const recent = root.recentProjects || []
        for (let i = 0; i < recent.length; ++i) {
            if (String(recent[i].path) === path)
                return String(recent[i].name || "")
        }
        const fileName = path.substring(path.lastIndexOf("/") + 1)
        const dot = fileName.lastIndexOf(".")
        return dot > 0 ? fileName.substring(0, dot) : fileName
    }

    function isValidDate(value) {
        return value instanceof Date && !isNaN(value.getTime())
    }

    function isoDate(value) {
        return Qt.formatDate(value, "yyyy-MM-dd")
    }

    // Section 3.2 of the welcome overview plan.
    function formatCaptureRange(earliest, latest) {
        if (!root.isValidDate(earliest) || !root.isValidDate(latest))
            return qsTr("No capture dates")
        const first = root.isoDate(earliest)
        const last = root.isoDate(latest)
        if (first === last)
            return first
        if (earliest.getFullYear() === latest.getFullYear())
            return qsTr("%1 to %2").arg(first).arg(Qt.formatDate(latest, "MM-dd"))
        return qsTr("%1 to %2").arg(first).arg(last)
    }

    function refreshCoverItems() {
        if (root.state !== "ready" || !root.thumbnailModel
                || root.thumbnailModel.rowCount() === 0) {
            root.coverItems = []
        } else {
            root.coverItems = root.thumbnailModel.getThumbnailStatesInRange(0, 2)
        }
        root.syncPins()
    }

    function pinKey(cover) {
        return cover.elementId + ":" + cover.imageId + ":" + cover.maxEdge
    }

    function syncPins() {
        const wanted = []
        if (root.active && root.state === "ready" && root.thumbnailMaxEdge > 0) {
            for (let i = 0; i < root.coverItems.length && i < 3; ++i) {
                const item = root.coverItems[i]
                if (item.elementId !== 0 && item.imageId !== 0) {
                    wanted.push({ elementId: item.elementId, imageId: item.imageId,
                                  maxEdge: root.thumbnailMaxEdge })
                }
            }
        }
        const wantedKeys = wanted.map(root.pinKey)
        const pinnedKeys = root.pinnedCovers.map(root.pinKey)
        // Pin the new rows before the release so a shared thumbnail never drops to zero pins.
        for (let i = 0; i < wanted.length; ++i) {
            if (pinnedKeys.indexOf(wantedKeys[i]) < 0) {
                appModules.library.SetThumbnailVisible(wanted[i].elementId, wanted[i].imageId,
                                                       true, wanted[i].maxEdge)
            }
        }
        for (let i = 0; i < root.pinnedCovers.length; ++i) {
            if (wantedKeys.indexOf(pinnedKeys[i]) < 0) {
                const pinned = root.pinnedCovers[i]
                appModules.library.SetThumbnailVisible(pinned.elementId, pinned.imageId,
                                                       false, pinned.maxEdge)
            }
        }
        root.pinnedCovers = wanted
    }

    function releaseAllPins() {
        for (let i = 0; i < root.pinnedCovers.length; ++i) {
            const pinned = root.pinnedCovers[i]
            appModules.library.SetThumbnailVisible(pinned.elementId, pinned.imageId,
                                                   false, pinned.maxEdge)
        }
        root.pinnedCovers = []
    }

    onStateChanged: root.refreshCoverItems()
    onActiveChanged: root.syncPins()
    onThumbnailMaxEdgeChanged: root.syncPins()
    onThumbnailModelChanged: root.refreshCoverItems()

    property Connections thumbnailModelConnections: Connections {
        target: root.thumbnailModel
        ignoreUnknownSignals: true
        function onModelReset() { root.refreshCoverItems() }
        function onRowsInserted(parent, first, last) {
            if (first <= 2)
                root.refreshCoverItems()
        }
        function onRowsRemoved(parent, first, last) {
            if (first <= 2)
                root.refreshCoverItems()
        }
        function onDataChanged(topLeft, bottomRight, roles) {
            if (topLeft.row <= 2)
                root.refreshCoverItems()
        }
    }

    Component.onCompleted: root.refreshCoverItems()
    Component.onDestruction: root.releaseAllPins()
}
