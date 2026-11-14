import QtQuick
import "ThumbnailTier.js" as ThumbnailTier

// Thumbnail lifetime of one photo delegate of ThumbnailGridView (a classic card or a
// justified-rows tile). While the delegate exists it pins the thumbnail of its photo at the tier
// of the view (view.desiredMaxEdge) through LibraryModule.SetThumbnailVisible, and it keeps the
// thumbnail state that the delegate shows (the live* properties).
//
// A zoom to a smaller tier keeps the larger pin of a photo that already shows a thumbnail larger
// than the new tier (ThumbnailTier.keepsLargerTier): the photo requests no render.
//
// During a zoom the view suspends the binding (view.thumbnailBindingSuspended): a new photo of a
// delegate shows its state but pins nothing, and a release is deferred to the view, which pins
// the visible delegates again at the new tier when the zoom settles (view.syncVisibleThumbnailBindings).
//
// A thumbnail state change reaches the delegate through the thumbUrl, thumbLoading,
// thumbMissingSource, and thumbErrorText roles of its model row (dataChanged). The binding does
// not listen to LibraryModule.thumbnailUpdated: every created delegate would run a handler for
// every change of every photo.
//
// A delegate can get another photo (a justified-rows slot, a reused grid card): the model writes
// elementId and imageId one after the other, so the binding asks the view to pin it once, after
// both are set (view.schedulePinBind), and never pins the new photo with the image of the old
// one. The view, not the binding, defers the call: a deferred call on a delegate that is destroyed
// meanwhile would run without its context. A delegate that the grid puts in its pool calls
// release(), and bind(false) when the grid reuses it.
QtObject {
    id: pin
    objectName: "thumbnailPinBinding"

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
    // False when liveThumbUrl names another photo than the URL before it: the Image of the
    // delegate keeps the shown pixels while a new tier of the same photo loads, never the pixels
    // of another photo.
    property bool retainShownImage: true
    property bool liveThumbLoading: thumbLoading
    property bool liveThumbMissingSource: thumbMissingSource
    property string liveThumbErrorText: thumbErrorText

    property int pinnedElementId: 0
    property int pinnedImageId: 0
    property int pinnedMaxEdge: 0

    onThumbUrlChanged: showThumbUrl(thumbUrl)
    onThumbLoadingChanged: liveThumbLoading = thumbLoading
    onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
    onThumbErrorTextChanged: liveThumbErrorText = thumbErrorText
    onElementIdChanged: schedulePhotoBind()
    onImageIdChanged: schedulePhotoBind()

    function schedulePhotoBind() {
        if (view) {
            view.schedulePinBind(pin)
        }
    }

    function showThumbUrl(url) {
        retainShownImage = ThumbnailTier.samePhoto(liveThumbUrl, url)
        liveThumbUrl = url
    }

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
        showThumbUrl(thumbUrl)
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
            if (ThumbnailTier.keepsLargerTier(pinnedMaxEdge, maxEdge, liveThumbUrl,
                                              liveThumbErrorText)) {
                return
            }
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
    Component.onDestruction: {
        if (view) {
            view.unschedulePinBind(pin)
        }
        release()
    }

    property Connections viewConnection: Connections {
        target: pin.view
        function onDesiredMaxEdgeChanged() {
            if (!pin.view.thumbnailBindingSuspended) {
                pin.sync()
            }
        }
    }
}
