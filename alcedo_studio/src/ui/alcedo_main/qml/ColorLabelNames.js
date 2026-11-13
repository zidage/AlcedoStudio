.pragma library

// Color labels of a photo: a bit set, bit `code` for color label `code`
// (ThumbnailColorLabelDots, the Library cards and tiles).

// Names of the color labels in @p labels, in code order. @p names are the names of the codes
// (LibraryViewPreferences.colorLabelNames); @p codeCount is the number of codes.
function shownNames(labels, names, codeCount) {
    const out = []
    for (let code = 0; code < codeCount; ++code) {
        if ((labels & (1 << code)) !== 0 && names && code < names.length) {
            out.push(String(names[code]))
        }
    }
    return out
}
