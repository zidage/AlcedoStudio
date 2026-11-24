//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

// The steps of Import Folder (#1078): the system folder dialog, then the Import Folder dialog,
// or the remembered choice of library/importFolderAction (#1077) without the dialog.
//
// Entries:
// - begin("", ...): Import Folder... of the Import menu. The dialog opens with Import into
//   Collection selected; a remembered choice runs that choice.
// - begin("mount", ...): the + of the Disk section and the Mount... button of a volume card.
// - begin("import", ...): the Import... button of a volume card.
//   An entry that names its action opens the dialog with that card; with a remembered choice it
//   runs its own action without the dialog.
// - beginDrop(urls, folderId): a Library drop that holds a folder always opens the dialog with
//   the drop target as the destination.
//
// The Import destination of an entry is the selected collection, or Library when a mounted row
// or All Photos is selected.
Item {
    id: flow

    /// FolderController (appModules.folders).
    property var folders: null
    /// ImportExportHandler (appModules.importExport).
    property var importExport: null
    /// LibraryBehaviorPreferences (appModules.libraryBehavior).
    property var libraryBehavior: null
    property var theme: null
    property Item blurSource: null
    property alias dialog: dialogObj
    property alias folderDialog: folderDialogObj
    readonly property bool opened: dialogObj.opened

    // The action of the entry that opened the system folder dialog.
    property string pendingAction: ""

    function rememberedAction() {
        const value = libraryBehavior ? String(libraryBehavior.importFolderAction) : "ask"
        return value === "import" || value === "mount" ? value : ""
    }

    function defaultDestinationFolderId() {
        if (!folders) {
            return 0
        }
        const current = Number(folders.currentFolderId)
        const rows = folders.folders ? folders.folders : []
        for (let i = 0; i < rows.length; ++i) {
            if (Number(rows[i].folderId) === current) {
                return Number(rows[i].mountId || 0) === 0 ? current : 0
            }
        }
        return 0
    }

    /// @p folderUrl: the folder, or empty to ask with the system folder dialog, which opens at
    /// @p startFolderUrl when it is set.
    function begin(action, folderUrl, startFolderUrl) {
        pendingAction = action === "import" || action === "mount" ? action : ""
        const url = folderUrl ? String(folderUrl) : ""
        if (url.length > 0) {
            proceed(url)
            return
        }
        const start = startFolderUrl ? String(startFolderUrl) : ""
        if (start.length > 0) {
            folderDialogObj.currentFolder = start
        }
        folderDialogObj.open()
    }

    function proceed(folderUrl) {
        const remembered = rememberedAction()
        const action = pendingAction.length > 0 ? pendingAction : remembered
        if (remembered.length > 0) {
            run(action, folderUrl)
            return
        }
        dialogObj.openForFolder(folderUrl, action.length > 0 ? action : "import",
                                defaultDestinationFolderId())
    }

    function run(action, folderUrl) {
        if (action === "mount") {
            if (folders) {
                folders.MountFolder(folderUrl)
            }
        } else if (importExport) {
            importExport.StartFolderImportOfFolderAsUser(folderUrl, defaultDestinationFolderId())
        }
    }

    function beginDrop(urls, folderId) {
        const singleFolder = urls.length === 1 && importExport
                             && importExport.DropHoldsFolder(urls)
        dialogObj.openForDrop(urls, Number(folderId), singleFolder)
    }

    function remember(action) {
        if (libraryBehavior && libraryBehavior.importFolderAction !== action) {
            libraryBehavior.setImportFolderAction(action)
        }
    }

    FolderDialog {
        id: folderDialogObj
        objectName: "importFolderSystemDialog"
        title: flow.pendingAction === "mount" ? qsTr("Select Folder to Mount")
                                              : qsTr("Select Folder to Import")
        onAccepted: flow.proceed(selectedFolder.toString())
    }

    ImportFolderDialog {
        id: dialogObj
        parent: Overlay.overlay
        theme: flow.theme
        blurSource: flow.blurSource
        scan: flow.importExport ? flow.importExport.folderScan : null
        folderController: flow.folders
        onImportConfirmed: function(folderId, rememberChoice) {
            if (rememberChoice) {
                flow.remember("import")
            }
            if (flow.importExport) {
                flow.importExport.StartFolderImportToFolderAsUser(folderId)
            }
        }
        onMountConfirmed: function(folderUrl, rememberChoice) {
            if (rememberChoice) {
                flow.remember("mount")
            }
            if (scan) {
                scan.Cancel()
            }
            if (flow.folders) {
                flow.folders.MountFolder(folderUrl)
            }
        }
    }
}
