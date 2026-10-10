import QtQml

// Album/image selection. The selection and its rules live in C++
// (appModules.library.selection, LibrarySelection); this object forwards to it so
// children keep binding through `selectionState`.
QtObject {
    id: root

    readonly property var selection: appModules.library.selection
    readonly property var selectedImagesById: root.selection.selectedImagesById
    readonly property int selectedCount: root.selection.selectedCount

    function setImageSelected(elementId, imageId, fileName, isHdr, selected) {
        root.selection.SetImageSelected(Number(elementId), Number(imageId),
                                        fileName ? String(fileName) : "", isHdr === true,
                                        selected === true)
    }

    function clearSelectedImages() {
        root.selection.Clear()
    }

    function replaceSelectedImages(items) {
        root.selection.Replace(items)
    }

    function currentSelectedItems() {
        return root.selection.SelectedItems()
    }

    function pruneDeletedElements(elementIds) {
        if (!elementIds || elementIds.length === 0) {
            return
        }
        root.selection.PruneElements(elementIds)
    }
}
