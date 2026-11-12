.pragma library

// Library file drop (issue #757): the drop areas of the Library grid and the Collections rows
// take only drags of local files and folders from the system file manager. The internal
// Collections drag carries no URLs, so the two never meet.

// The local file URLs of a DropArea drag or drop event, in drop order; empty when the event
// carries no URL or any URL that is not a local file (a web link).
function localFileUrls(dragEvent) {
    if (!dragEvent || !dragEvent.hasUrls) {
        return []
    }
    const urls = []
    for (let i = 0; i < dragEvent.urls.length; ++i) {
        const text = dragEvent.urls[i].toString()
        if (!text.startsWith("file:")) {
            return []
        }
        urls.push(text)
    }
    return urls
}
