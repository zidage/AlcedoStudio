.pragma library

// Thumbnail tiers of the Library grid. A thumbnail URL names its tier:
// image://alcedo-thumb/<element id>/<max edge>/<revision> (ThumbnailImageStore::MakeUrl).

// Max edge of the thumbnail that @p url shows; 0 when the URL names no thumbnail tier.
function urlMaxEdge(url) {
    const prefix = "image://alcedo-thumb/"
    const text = String(url || "")
    if (!text.startsWith(prefix)) {
        return 0
    }
    const parts = text.substring(prefix.length).split("/")
    if (parts.length < 2) {
        return 0
    }
    const edge = parseInt(parts[1], 10)
    return isNaN(edge) ? 0 : edge
}

// A zoom asks a photo for @p requestedMaxEdge while it pins @p pinnedMaxEdge and shows
// @p shownUrl. True when the photo keeps its pin and requests nothing: the request is a smaller
// tier and the shown thumbnail is already larger than the request, so a render of the smaller
// tier adds no pixels.
function keepsLargerTier(pinnedMaxEdge, requestedMaxEdge, shownUrl, errorText) {
    return requestedMaxEdge > 0
            && requestedMaxEdge < pinnedMaxEdge
            && String(errorText || "").length === 0
            && urlMaxEdge(shownUrl) > requestedMaxEdge
}

// Element id that @p url names; 0 when the URL names no thumbnail.
function urlElementId(url) {
    const prefix = "image://alcedo-thumb/"
    const text = String(url || "")
    if (!text.startsWith(prefix)) {
        return 0
    }
    const id = parseInt(text.substring(prefix.length).split("/")[0], 10)
    return isNaN(id) ? 0 : id
}

// True when @p oldUrl and @p newUrl are thumbnails of the same photo (another tier or revision).
function samePhoto(oldUrl, newUrl) {
    const id = urlElementId(oldUrl)
    return id > 0 && id === urlElementId(newUrl)
}
