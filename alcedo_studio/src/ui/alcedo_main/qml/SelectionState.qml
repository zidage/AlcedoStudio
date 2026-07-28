import QtQml

// Album/image selection state. Mirrors the ExportQueueState pattern: a plain
// QtObject owning the selected-image map and the mutate/prune helpers. Main
// exposes the instance via an alias so children bind through `selectionState`.
QtObject {
    id: root

    property var selectedImagesById: ({})
    readonly property int selectedCount: Object.keys(selectedImagesById).length

    function keyForElement(elementId) {
        return String(Number(elementId))
    }

    function setImageSelected(elementId, imageId, fileName, isHdr, selected) {
        const key = keyForElement(elementId)
        const already = Object.prototype.hasOwnProperty.call(selectedImagesById, key)
        if (selected === already) {
            return
        }

        const next = Object.assign({}, selectedImagesById)
        if (selected) {
            next[key] = {
                elementId: Number(elementId),
                fileId: Number(elementId),
                imageId: Number(imageId),
                fileName: fileName ? fileName : qsTr("(unnamed)"),
                isHdr: isHdr === true
            }
        } else {
            delete next[key]
        }
        selectedImagesById = next
    }

    function clearSelectedImages() {
        selectedImagesById = ({})
    }

    function replaceSelectedImages(items) {
        const next = {}
        for (let i = 0; i < items.length; ++i) {
            const item = items[i]
            const key = keyForElement(item.elementId)
            next[key] = {
                elementId: Number(item.elementId),
                fileId: Number(item.fileId || item.elementId),
                imageId: Number(item.imageId),
                fileName: item.fileName ? item.fileName : qsTr("(unnamed)"),
                isHdr: item.isHdr === true
            }
        }
        selectedImagesById = next
    }

    function currentSelectedItems() {
        const rows = Object.values(selectedImagesById)
        const items = []
        for (let i = 0; i < rows.length; ++i) {
            const item = rows[i]
            const rowIndex = appModules.library.thumbnailModel.rowByElementId(Number(item.elementId))
            if (rowIndex >= 0) {
                const current = appModules.library.thumbnailModel.getItemAt(rowIndex)
                if (current && Number(current.elementId) === Number(item.elementId)) {
                    items.push({
                        elementId: Number(current.elementId),
                        fileId: Number(current.fileId || current.elementId),
                        imageId: Number(current.imageId),
                        fileName: current.fileName ? current.fileName : qsTr("(unnamed)"),
                        isHdr: current.isHdr === true
                    })
                    continue
                }
            }
            items.push({
                elementId: Number(item.elementId),
                fileId: Number(item.fileId || item.elementId),
                imageId: Number(item.imageId),
                fileName: item.fileName ? item.fileName : qsTr("(unnamed)"),
                isHdr: item.isHdr === true
            })
        }
        return items
    }

    function pruneDeletedElements(elementIds) {
        if (!elementIds || elementIds.length === 0) {
            return
        }

        const deleted = {}
        for (let i = 0; i < elementIds.length; ++i) {
            deleted[keyForElement(elementIds[i])] = true
        }

        const nextSelected = {}
        const selectedRows = Object.values(selectedImagesById)
        for (let i = 0; i < selectedRows.length; ++i) {
            const row = selectedRows[i]
            const key = keyForElement(row.elementId)
            if (!deleted[key]) {
                nextSelected[key] = row
            }
        }
        selectedImagesById = nextSelected
    }
}