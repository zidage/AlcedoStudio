# Folder import confirm dialog (archived)

This directory keeps `FolderImportConfirmDialog.qml`, the confirmation dialog of Import Folder and
of a Library drop that held a folder, kept for reference only. `ImportFolderDialog.qml` replaces
it: the user chooses Import into Collection or Mount Folder there, picks the destination
collection from a tree with search, and the folder scan (`ImportExportHandler.folderScan`) stays
the same ([#1078](https://github.com/zidage/AlcedoStudio/issues/1078)).

- The file is not built, installed, or packaged. No CMake QML file list, resource, or QML file
  refers to this directory.
- The file keeps its original path relative to `alcedo_studio/`:
  `alcedo_studio/src/ui/alcedo_main/qml/FolderImportConfirmDialog.qml` is now
  `alcedo_studio/deprecated/folder_import_confirm_dialog/src/ui/alcedo_main/qml/FolderImportConfirmDialog.qml`.
  It moved with `git mv` and no content change, so `git log --follow` shows its history.
- Do not add a `CMakeLists.txt` here.

| Archived path | What it held |
| --- | --- |
| `src/ui/alcedo_main/qml/FolderImportConfirmDialog.qml` | The scan summary, one check box per file type, the list of the scanned files, and Import |
