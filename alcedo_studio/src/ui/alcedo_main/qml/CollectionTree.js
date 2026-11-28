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

// The rows without a mount: the root row and the ordinary collections. A mounted folder takes
// no user change to its content (Mounted Folder Plan, D6), and no ordinary collection lies below
// a mounted row, so the result is still one connected tree.
function ordinaryRows(source) {
    const out = []
    const count = source ? source.length : 0
    for (let i = 0; i < count; ++i) {
        const row = source[i]
        if (row && Number(row.mountId || 0) === 0) {
            out.push(row)
        }
    }
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

// Tree search of the Import Folder destination tree (#1078). `ordered` are rows in tree order
// (orderRows); `nameOf(row)` gives the shown name. A row whose name holds `text`
// (case-insensitive) matches and keeps every ancestor visible. Returns `{ visible, matched,
// matchCount }`: `visible` and `matched` map folderId -> true. An empty `text` returns null: the
// tree shows its own expansion state.
function searchRows(ordered, text, nameOf) {
    const needle = String(text || "").trim().toLowerCase()
    if (needle.length === 0) {
        return null
    }
    const byFolderId = rowsByFolderId(ordered)
    const visible = {}
    const matched = {}
    let matchCount = 0
    for (let i = 0; i < ordered.length; ++i) {
        const row = ordered[i]
        const name = nameOf ? nameOf(row) : String(row.name || "")
        if (String(name).toLowerCase().indexOf(needle) < 0) {
            continue
        }
        matched[String(Number(row.folderId))] = true
        ++matchCount
        let current = row
        let guard = 0
        while (current && !visible[String(Number(current.folderId))] && guard < 4096) {
            visible[String(Number(current.folderId))] = true
            if (Number(current.folderId) === 0) {
                break
            }
            current = byFolderId[String(Number(current.parentFolderId))]
            ++guard
        }
    }
    return { visible: visible, matched: matched, matchCount: matchCount }
}

function escapeHtml(text) {
    return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                       .replace(/"/g, "&quot;")
}

// `name` as StyledText with the first case-insensitive match of `text` in bold; plain escaped
// text when `text` is empty or does not match.
function highlightedName(name, text) {
    const source = String(name || "")
    const needle = String(text || "").trim().toLowerCase()
    const at = needle.length > 0 ? source.toLowerCase().indexOf(needle) : -1
    if (at < 0) {
        return escapeHtml(source)
    }
    return escapeHtml(source.slice(0, at)) + "<b>" + escapeHtml(source.slice(at, at + needle.length))
           + "</b>" + escapeHtml(source.slice(at + needle.length))
}
// The rows of the Disk section of the Collections sidebar (#1078): for each volume, one volume
// row and then its mount roots with their folders in tree order. Mount rows are the rows with a
// mountId; a mount root has mountRoot true. The mounts of one volume share mountVolumeKey (the
// stored volume identity, else the mount point). A volume row is { rowKind: "volume", volumeKey,
// volumeName, mountPoint, volumeRoot, offline, treeDepth: 0 }; the volume is offline when every
// mount of it is. Each mount row is a copy that adds treeDepth (mount root 1) and hasChildren.
// Volumes sort by their shown name; mounts of a volume sort like collections.
function diskRows(source, descending) {
    const count = source ? source.length : 0
    const children = childrenByParent(source)
    const groups = {}
    const keys = []
    for (let i = 0; i < count; ++i) {
        const row = source[i]
        if (!row || Number(row.mountId || 0) === 0 || row.mountRoot !== true) {
            continue
        }
        const key = String(row.mountVolumeKey || row.mountPoint || row.mountVolumeRoot
                           || ("mount:" + Number(row.mountId)))
        if (!groups[key]) {
            groups[key] = []
            keys.push(key)
        }
        groups[key].push(row)
    }
    const volumes = keys.map(function(key) {
        const mounts = groups[key].slice().sort(compareByName)
        if (descending) {
            mounts.reverse()
        }
        let name = ""
        let mountPoint = ""
        for (let i = 0; i < mounts.length; ++i) {
            if (name.length === 0 && mounts[i].mountVolumeName) {
                name = String(mounts[i].mountVolumeName)
            }
            if (mountPoint.length === 0 && mounts[i].mountPoint) {
                mountPoint = String(mounts[i].mountPoint)
            }
        }
        return {
            rowKind: "volume",
            volumeKey: key,
            volumeName: name,
            mountPoint: mountPoint,
            volumeRoot: String(mounts[0].mountVolumeRoot || ""),
            offline: mounts.every(function(row) { return row.mountOffline === true }),
            treeDepth: 0,
            hasChildren: true,
            mounts: mounts
        }
    })
    const shownName = function(volume) {
        return (volume.volumeName || volume.mountPoint || volume.volumeRoot || volume.volumeKey)
                .toLowerCase()
    }
    volumes.sort(function(left, right) {
        return shownName(left).localeCompare(shownName(right))
    })
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
            out.push(Object.assign({}, list[i], {
                treeDepth: depth,
                hasChildren: hasChildren(list[i].folderId)
            }))
            visit(list[i].folderId, depth + 1)
        }
    }
    for (let v = 0; v < volumes.length; ++v) {
        const volume = volumes[v]
        const mounts = volume.mounts
        delete volume.mounts
        out.push(volume)
        for (let i = 0; i < mounts.length; ++i) {
            out.push(Object.assign({}, mounts[i], {
                treeDepth: 1,
                hasChildren: hasChildren(mounts[i].folderId)
            }))
            visit(mounts[i].folderId, 2)
        }
    }
    return out
}