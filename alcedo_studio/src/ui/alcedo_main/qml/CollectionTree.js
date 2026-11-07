.pragma library

// Tree helpers for the rows of FolderController.folders (one row per folder, the root row has
// folderId 0). Used by the Collections sidebar and by the "Add to" image menu.
// Fields read: folderId, elementId, parentFolderId, name.

function compareByName(left, right) {
    const a = String(left.name || "").toLowerCase()
    const b = String(right.name || "").toLowerCase()
    const byName = a.localeCompare(b)
    return byName !== 0 ? byName : Number(left.folderId) - Number(right.folderId)
}

function childrenByParent(source) {
    const map = {}
    const count = source ? source.length : 0
    for (let i = 0; i < count; ++i) {
        const row = source[i]
        if (!row || Number(row.folderId) === 0) {
            continue
        }
        const key = String(Number(row.parentFolderId))
        if (!map[key]) {
            map[key] = []
        }
        map[key].push(row)
    }
    return map
}

// Rows in tree order: the root row, then each folder followed by its subtree. Siblings sort by
// name, in reverse when `descending` is true. Each result row is a copy that adds `treeDepth`
// (root 0, top-level folders 1) and `hasChildren`.
function orderRows(source, descending) {
    const count = source ? source.length : 0
    let root = null
    for (let i = 0; i < count; ++i) {
        if (source[i] && Number(source[i].folderId) === 0) {
            root = source[i]
            break
        }
    }
    const children = childrenByParent(source)
    const out = []
    const hasChildren = function(folderId) {
        const list = children[String(Number(folderId))]
        return !!list && list.length > 0
    }
    const visit = function(parentId, depth) {
        const list = (children[String(Number(parentId))] || []).slice()
        list.sort(compareByName)
        if (descending) {
            list.reverse()
        }
        for (let i = 0; i < list.length; ++i) {
            const row = list[i]
            out.push(Object.assign({}, row, {
                treeDepth: depth,
                hasChildren: hasChildren(row.folderId)
            }))
            visit(row.folderId, depth + 1)
        }
    }
    if (root) {
        out.push(Object.assign({}, root, { treeDepth: 0, hasChildren: hasChildren(0) }))
    }
    visit(0, 1)
    return out
}

function rowsByFolderId(rows) {
    const map = {}
    for (let i = 0; i < rows.length; ++i) {
        map[String(Number(rows[i].folderId))] = rows[i]
    }
    return map
}

// True when `folderId` is `ancestorId` or lies below it.
function isAtOrBelow(byFolderId, folderId, ancestorId) {
    let current = byFolderId[String(Number(folderId))]
    const target = Number(ancestorId)
    let guard = 0
    while (current && guard < 4096) {
        if (Number(current.folderId) === target) {
            return true
        }
        if (Number(current.folderId) === 0) {
            return false
        }
        current = byFolderId[String(Number(current.parentFolderId))]
        ++guard
    }
    return false
}

// Ordered rows whose name holds `text` (case-insensitive), with their ancestors so the tree
// stays readable. An empty `text` keeps every row.
function filterRows(ordered, text) {
    const needle = String(text || "").trim().toLowerCase()
    if (needle.length === 0) {
        return ordered
    }
    const byFolderId = rowsByFolderId(ordered)
    const keep = {}
    for (let i = 0; i < ordered.length; ++i) {
        const row = ordered[i]
        if (Number(row.folderId) === 0
                || String(row.name || "").toLowerCase().indexOf(needle) < 0) {
            continue
        }
        let current = row
        let guard = 0
        while (current && !keep[String(Number(current.folderId))] && guard < 4096) {
            keep[String(Number(current.folderId))] = true
            if (Number(current.folderId) === 0) {
                break
            }
            current = byFolderId[String(Number(current.parentFolderId))]
            ++guard
        }
    }
    return ordered.filter(function(row) { return keep[String(Number(row.folderId))] === true })
}
