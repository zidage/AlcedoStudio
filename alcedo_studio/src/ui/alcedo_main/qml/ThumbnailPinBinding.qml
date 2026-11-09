import QtQuick

// Thumbnail lifetime of one photo delegate of ThumbnailGridView (a classic card or a
// justified-rows tile). While the delegate exists it pins the thumbnail of its photo at the tier
// of the view (view.desiredMaxEdge) through LibraryModule.SetThumbnailVisible, and it keeps the
// thumbnail state that the delegate shows (the live* properties).
//
// During a zoom the view suspends the binding (view.thumbnailBindingSuspended): a new photo of a
// delegate shows its state but pins nothing, and a release is deferred to the view, which pins
// the visible delegates again at the new tier when the zoom settles (view.syncVisibleThumbnailBindings).
QtObject {
    id: pin

    // The ThumbnailGridView that owns the delegate.
    property Item view: null
    // Model roles of the delegate.
    property int elementId: 0
    property int imageId: 0
    property string thumbUrl: ""
    property bool thumbLoading: false
    property bool thumbMissingSource: false
    property string thumbErrorText: ""

    property string liveThumbUrl: thumbUrl
    property bool liveThumbLoading: thumbLoading
    property bool liveThumbMissingSource: thumbMissingSource
    property string liveThumbErrorText: thumbErrorText

    property int pinnedElementId: 0
    property int pinnedImageId: 0
    property int pinnedMaxEdge: 0

    onThumbUrlChanged: liveThumbUrl = thumbUrl
    onThumbLoadingChanged: liveThumbLoading = thumbLoading
    onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
    onThumbErrorTextChanged: liveThumbErrorText = thumbErrorText
    onElementIdChanged: bind(false)
    onImageIdChanged: bind(false)

    function releasePinned() {
        if (pinnedElementId !== 0 && pinnedImageId !== 0) {
            appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, false,
                                                   pinnedMaxEdge > 0 ? pinnedMaxEdge
                                                                     : view.desiredMaxEdge)
        }
        pinnedElementId = 0
        pinnedImageId = 0
        pinnedMaxEdge = 0
    }

    function bind(force) {
        if (!view) {
            return
        }
        liveThumbUrl = thumbUrl
        liveThumbLoading = thumbLoading
        liveThumbMissingSource = thumbMissingSource
        liveThumbErrorText = thumbErrorText

        if (view.thumbnailBindingSuspended && !force) {
            return
        }
        const maxEdge = view.desiredMaxEdge
        if (pinnedElementId === elementId && pinnedImageId === imageId
                && pinnedMaxEdge === maxEdge) {
            return
        }
        if (pinnedElementId === elementId && pinnedImageId === imageId) {
            // The same photo at a new tier: pin the new tier before the old one is released.
            const oldMaxEdge = pinnedMaxEdge
            pinnedMaxEdge = maxEdge
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, true,
                                                       pinnedMaxEdge)
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, false,
                                                       oldMaxEdge)
            }
            return
        }
        releasePinned()
        pinnedElementId = elementId
        pinnedImageId = imageId
        pinnedMaxEdge = maxEdge
        if (pinnedElementId !== 0 && pinnedImageId !== 0) {
            appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, true,
                                                   pinnedMaxEdge)
        }
    }

    function sync() {
        bind(true)
    }

    function release() {
        if (!view) {
            return
        }
        if (view.thumbnailBindingSuspended) {
            view.deferThumbnailRelease(pinnedElementId, pinnedImageId,
                                       pinnedMaxEdge > 0 ? pinnedMaxEdge : view.desiredMaxEdge)
            pinnedElementId = 0
            pinnedImageId = 0
            pinnedMaxEdge = 0
            return
        }
        releasePinned()
    }

    Component.onCompleted: bind(false)
    Component.onDestruction: release()

    property Connections viewConnection: Connections {
        target: pin.view
        function onDesiredMaxEdgeChanged() {
            if (!pin.view.thumbnailBindingSuspended) {
                pin.sync()
            }
        }
    }

    property Connections libraryConnection: Connections {
        target: appModules.library
        ignoreUnknownSignals: true
        function onThumbnailUpdated(updatedElementId, updatedUrl, loading, missingSource,
                                    errorText) {
            if (updatedElementId === pin.elementId) {
                pin.liveThumbUrl = updatedUrl
                pin.liveThumbLoading = loading
                pin.liveThumbMissingSource = missingSource
                pin.liveThumbErrorText = errorText
            }
        }
    }
}
