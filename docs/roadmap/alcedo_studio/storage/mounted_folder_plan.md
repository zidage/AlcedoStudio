# Mounted Folder Plan

Date: 2026-11-19

Status: **implemented, pull requests open**. MF1 (#983) to MF8 (#990) are implemented. The user
checks of MF6 and MF8 in the application are open.

Parent: [Roadmap](../../README.md).

Direct prerequisite: the storage boundary that the
[Storage Query Boundary Plan](storage_query_boundary_plan.md) describes. This plan adds tables and
store operations inside that boundary. It does not replace it.

Tracking issue: [#991](https://github.com/zidage/AlcedoStudio/issues/991). Phase issues: MF1 #983,
MF2 #984, MF3 #985, MF4 #986, MF5 #987, MF6 #988, MF7 #989, MF8 #990.

Source audit revision: `016c3535b` on `main`.

Unless a path starts with `alcedo_studio/` or `docs/`, it is relative to `alcedo_studio/src/`.
Test paths are relative to `alcedo_studio/`.

Section 14 defines the terms and parameters that this plan uses.

---

## 1. Decision record

### 1.1 User goal

Several users asked for a library that shows the real folder structure of the disk. The user
mounts a disk directory as a collection. The collection shows the directory tree and the
supported image files of that directory. A scan in one direction, from the disk to the library,
keeps the collection equal to the directory.

The project database stays the only owner of edit history, Versions, ratings, labels, and
collection links. Alcedo Studio writes no sidecar file.

### 1.2 Approved decisions

| Id | Decision |
| --- | --- |
| D1 | The scan has one direction: from the disk to the library. Alcedo Studio never writes, renames, moves, or deletes a file in a mounted directory. Deletion in the application is the Delete mark. |
| D2 | The element id stays the identity of a photo. A source fingerprint matches a file that moved or got a new name to its element. A file that moved is the same photo. |
| D3 | No sidecar file. The fingerprint and the source record live in the project database. |
| D4 | A file that the scan cannot find becomes missing. The element and all its data stay. Only an explicit user command removes missing photos. |
| D5 | An unreachable root directory marks the mount as offline. No comparison runs for an offline mount. |
| D6 | The structure of a mounted folder is read-only in the application. The user cannot create a collection, rename, move, or delete below a mount. The check reads the mounted mark of the target folder only. |
| D7 | Import into a mounted folder is refused. This includes a file drop. The user puts the file into the directory with the file manager, and the scan finds it. |
| D8 | A mounted photo can be linked into an ordinary collection. Ordinary collections stay free. |
| D9 | A nullable `mount_id` on the element identifies a mounted element. A null value identifies an ordinary element. Files and folders use the same field. |
| D10 | One file has one element. When a directory is mounted, a file that the project already holds by reference becomes the mounted element of that path. |
| D11 | Unmount converts each mounted photo to an ordinary photo with its current path. No photo data is removed. |
| D12 | A mounted directory cannot be inside another mounted directory, and cannot contain one. |

### 1.3 Decisions made in this plan

| Id | Decision | Reason |
| --- | --- | --- |
| P1 | The fingerprint is XXH3-128 of the byte size, the first 128 KiB, and the last 128 KiB. | A RAW header holds the camera serial and the capture time. The end of the file holds sensor data. Two reads for each file keep a scan of a large directory short. |
| P2 | `Image.image_path` stays the one location that every reader opens. The scan is the only writer of that field for a mounted file. | Eight readers open this field today (section 4). One writer keeps them unchanged. |
| P3 | The relative path of a mounted element comes from the element names between the mount root and the element. No table stores it. | The sleeve tree already holds the names. A second copy can disagree with the tree. |
| P4 | A mount creates a new collection. The parent is the library root or an ordinary collection. | An existing collection can hold links that have no file in the directory. |
| P5 | A copy of a mounted file (equal fingerprint, the first file still present) is a new photo with no edits. | The user made a second file. The application does not decide that the two files share edits. |
| P6 | A changed file at a recorded path keeps its element. The scan updates the source record and invalidates the thumbnail. | The path identifies the file when the path is stable. This is the rule of pull request #798 for a changed size. |
| P7 | Remove from Library is unavailable for a mounted photo that is present. | The next scan finds the file again. The Delete mark records the intent of the user. |
| P8 | The directory watcher only starts a scan. The directory listing is the only source of the scan result. | Change notifications can be lost, and a network volume can send none. |
| P9 | The project file version becomes 0.12.0. Version 0.10.0 stays the minimum. | A build that does not know the `mount_id` column must refuse the project. |
| P10 | The copy of a dropped file into a mounted directory is not part of this plan. | It is the only operation that writes to the user directory. It needs its own design after the scan is stable. |
| P11 | On Windows the watcher holds one change notification handle on each root directory, which also reports the directories below it. It closes the handles of a volume that the user ejects. Other platforms use `QFileSystemWatcher` on each directory. | Windows refuses to rename a directory while a handle is open in a directory below it (measured 2026-11-20). `QFileSystemWatcher` holds one handle for each directory on Windows, so the user could not rename a mounted directory that has subdirectories. |
| P12 | An automatic scan shows its background task only when it changes the library: from the start of its import of new files, or at its end when it moved, changed, returned, or lost photos. An offline result shows when the mount was not offline before. A Synchronize scan shows from its start. | Activation and the watcher start many scans that change nothing. A task for each would push the results of other work out of the task list. |

### 1.4 Rejected options that must not return

- A content hash as the primary key of a photo. Two copies of one file must be able to exist.
- The NTFS file id or an inode number as the identity. It is stable only inside one volume.
- Import by reference into a mounted folder. The tree would show a file that the directory does
  not hold.
- Automatic removal of a photo that the scan cannot find.

---

## 2. Product design specification

### 2.1 Mount Folder

The Collections panel has the command **Mount Folder** in the same menu as **New Collection**.

Issue #1008 (review of the mock, 2026-11-20) keeps the name **Mount Folder** and adds
an entry point: the primary button at the foot of the Collections panel mounts the directory.
The row menu of the library root has **Mount Folder Here...**. Since #1076 (2026-11-23) the
parent of a mount is always the library root, so an ordinary row has no such item. The two imports share one two-segment button below it (**Import
Images**, **Import Folder**), so "Mount" names only the mount and "Import" only the one-time
imports. The three actions are disabled while a mounted row is selected. **Add to** lists the
root anchor and the ordinary collections only.

1. The command opens the system folder dialog.
2. The application creates a collection with the name of the directory, below the library root
   (#1076). An ordinary collection never holds a mount: the Collections section of the sidebar
   holds the rows that the user arranges, and the Disk section (#1078) holds the mounts, grouped
   by volume.
3. The first scan starts as a background task with the kind `MountScan`.
4. Photos appear in the collection while the scan imports them.

The command reports a named error, with no change to the project, when:

- the directory is inside a mounted directory, or contains one;
- the directory does not exist.

### 2.2 Collections panel rows

```text
v  Trips                      1.2K      ordinary collection
v  2026 Japan (link)           842      mount root, link icon
      Day 1 (link)             310      mounted folder, link icon
      Day 2 (link)             532      mounted folder, link icon
>  Archive (link)          Offline      offline mount, dimmed
```

- Each mounted row, the mount root and each mounted folder below it, shows the link icon
  (`panel_icons/link.svg`, the Tabler `link` outline with a stroke width of 1.5) after the name,
  in `textMutedColor`. The name keeps the alignment of the ordinary rows. An ordinary row has no
  icon.
- The count of a mounted folder counts present and missing photos.
- An offline mount shows a dimmed name and the label **Offline** in the count position.
- The row menu of a mount root has, in this order: **Synchronize**, **Locate Folder...**,
  **Show in File Manager**, a separator, **Remove Missing Photos**, a separator, and **Unmount**.
  The items stay in place and are disabled when they do not apply (review of the MF8 mock,
  2026-11-20): Synchronize and Show in File Manager for an offline mount, Locate Folder for a
  mount that is online, Remove Missing Photos for a mount with no missing photo.
- The row menu of a mounted folder below the root has: **Show in File Manager**.
- **New Collection**, **Rename**, **Delete**, and row drag are disabled for a mounted row.
- An ordinary row cannot be dropped on a mounted row.

The panel follows the button and text rules of `.agents/skills/alcedo-qml-ui/SKILL.md`. It adds
no explanatory text.

### 2.3 Photos of a mounted folder

- The grid, the filter, the search, the editor, the export, ratings, color labels, and the Delete
  mark work as they do for an ordinary photo.
- **Add to collection** and a drag to an ordinary row link a mounted photo into that collection.
- A file drop on a mounted row or on the grid of a mounted folder shows the "not allowed" cursor.
  No import starts.
- **Remove from Library** is disabled when the selection holds a mounted photo that is present.
- A missing photo shows one icon on its tile (Tabler `photo-off`, `panel_icons/photo-off.svg`, in
  `textMutedColor`) and no text (review of the MF8 mock, 2026-11-20). The rule applies to every
  tile whose source is missing, mounted or ordinary: the grid, the justified rows, and the editor
  filmstrip. Another thumbnail problem keeps its text. Its ratings and labels stay editable. The
  editor does not open it.
- The menu of a missing photo has **Locate Photo...**.

### 2.4 Scan rules

The scan compares the stored records of one mount with the listing of its directory. Section 14
defines each term.

| Case | Condition | Result |
| --- | --- | --- |
| Unchanged | A file is at the recorded path. Byte size and modification time are equal to the record. | No change. No file read. |
| Changed in place | A file is at the recorded path. Byte size or modification time is different. | The element stays. The source record gets the new values. The thumbnail is invalidated. |
| Moved | A recorded path has no file. A new path has a file with the fingerprint of the record. | The element stays. Its name, home link, and `image_path` change to the new location. |
| New | A new path has a file that matches no record without a file. | A new photo is imported by reference. |
| Missing | A recorded path has no file and no new path matches. | The source record becomes `missing`. All other data stays. |
| Returned | A missing record has a file at its recorded path again. | The source record becomes `present` with the values of the file. |
| Adopted | A new path equals the `image_path` of an ordinary photo of the project. | That photo becomes the mounted element of the path. |
| Offline | The root directory is unreachable. | The mount becomes offline. No other change. |

Match order when several records or paths have one fingerprint: the same file name first, then
the sorted relative path. The order is the same on every run.

Adoption when several ordinary photos have the path of one file: the scan adopts the photo with
the lowest element id. The other photos stay ordinary photos with their data.

Folder rules: the scan creates a mounted folder for each directory that holds a supported image
file at any depth. A mounted folder whose directory left the listing is removed after its
elements moved or became missing. A missing photo keeps its home link in the nearest mounted
ancestor that still exists.

The listing does not follow a symbolic link or a junction. It does not enter a directory whose
name starts with a period. The file type rule is `is_supported_file`
(`include/type/supported_file_type.hpp`).

### 2.5 Data rules

- **Identity.** The element id (`sl_element_id_t`) and the image id (`image_id_t`) stay counters.
  The scan never changes them for an existing photo.
- **Mounted mark.** `Element.mount_id` is null for an ordinary element. Every folder and file of
  a mount holds the id of that mount.
- **Home link.** A mounted file has exactly one `FolderContent` link to a folder with the same
  `mount_id`. Only the scan writes this link. Links to ordinary folders are free.
- **Source record.** A mounted file always has a `SourceFile` row. An ordinary file has a row
  only after an operation recorded one.
- **Location.** `Image.image_path` holds the absolute path of the file. For a mounted file it
  equals the root directory plus the relative path.
- **Names.** Element names of a mount use Unicode normalization form C. The comparison of
  relative paths is exact.
- **Parent.** The parent of each mount root is the library root (#1076). There is no migration:
  only development test projects hold a mount below an ordinary collection, and they are
  created again.
- **Volume name.** `MountRoot.volume_name` holds the display name of the volume as
  `CreateMount` or the last scan that reached the volume read it (`ReadVolumeLabel`). An offline
  mount keeps it. A scan that stores a new name reports `MountScanResult::volume_name_updated`,
  and the coordinator saves the project file for it (#1088). The mount point of the volume is session state of `MountService`, like the
  reachability.
- **Counts.** The library root row counts the photos that no folder other than the root holds
  and the photos of the ordinary collections; a mounted photo counts only through an ordinary
  collection. All Photos counts every photo (#1076).
- **Compatibility.** A project of file version 0.10.0 or 0.11.0 opens. Each element has a null
  `mount_id`. The edit history root id of each image is unchanged, because the root id uses the
  element id, the pipeline document, and the RAW color context only
  (`edit/history/pipeline_document_checkpoint.cpp`).

### 2.6 Asynchronous behavior

- A scan runs on a worker task. The listing, the fingerprints, and the metadata read of new files
  run with no sleeve lock.
- The apply step runs through `SleeveServiceImpl` under its `fs_lock_`, in batches.
- The first batch holds folder changes, moves, changes in place, missing, and returned records.
  Each later batch holds 500 new files.
- Each batch is complete in the project database before the next batch starts. A canceled scan
  keeps the completed batches. The next scan finds the remaining new files, because they have no
  record.
- One scan runs at a time for one mount. A request during a scan sets one "scan again" flag.
- The comparison reads the fingerprint of a new file only when a lost record has its byte size
  (the fingerprint holds the byte size). Each other new file is read in the batch that imports
  it, so the first scan of a directory stores its first photos after one batch of reads (#1014).
- The scan reads the stop request for each directory of the listing, before each fingerprint
  read, and before each new file batch. A batch that started its import completes: a stop
  leaves each batch stored in full or not started (#1014).
- The UI thread does not wait for a scan. A project close, a project load, and the application
  exit request the stop, and they save the project after the scan worker has ended: the close
  waits in the event loop, the load waits in its loader thread, and the exit waits in its event
  wait (#1014).
- A user import and a delete hold the scans for their whole length, the finalize step of an
  import included (`MountScanCoordinator::Hold`). A hold requests the stop of the running scan,
  and no scan or mount operation starts while a hold exists. The import job or the delete worker
  starts when no scan worker writes the project. The stopped scan runs again after the last
  hold, with its trigger, its waiting callbacks, and its task (#1016).
- A user import leaves each file that the listing of a mount lists to the scan of that mount
  (`IsPathOfAMountScan`; decision D10). The Library snackbar gives the count of these files:
  on a line under the import result, or alone when the import leaves every file and does not
  run (`ImportExportHandler::importLeftToMounts`, `importLeftAllToMounts`; #1089). The
  synchronous delete of the Nikon HE recovery (`ImageController::DeleteTargets`) refuses while a scan worker writes (#1016).

---

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility |
| --- | --- |
| Storage | `MountRoot` and `SourceFile` tables, `Element.mount_id`, stores, mappers, project file version. |
| Source file utilities | Fingerprint, volume identity, directory listing. |
| Scan change list | The comparison of records and listing. No storage access. |
| Mount service | Create, scan, adopt, apply, unmount, locate, remove missing photos. |
| Sleeve service | The structure check and the scan entry. |
| Import | Refusal of a mounted destination. |
| Library panel | Mount Folder, row states, menus, drop refusal, missing state from the stored record. |
| Scan triggers | Project open, application activation, directory watcher. |
| Automation | `library.mount`, `library.mounts`, `library.sync`, `library.unmount`. |

### 3.2 Explicit exclusions

- Copy of a dropped file into a mounted directory (P10).
- Rename, move, or delete of a user file from the application (D1).
- Sidecar files (D3).
- Reload of an open editor session when its source file changed in place. Pull request #798 holds
  an implementation of that reload. This plan invalidates the thumbnail only.
- Duplicate import rules for ordinary collections. Pull request #798 owns that work.

### 3.3 Related plans

| Plan | Relation |
| --- | --- |
| [Storage Query Boundary Plan](storage_query_boundary_plan.md) | Depends on. Every new SQL statement goes through duckorm inside storage. |
| [Library Search and Project Size Plan](library_search_and_project_size_plan.md) | Depends on. The search columns of `Image` stay as they are. |
| [Album Sort, Group, and Import Time Plan](../ui/album_sort_group_and_import_time_plan.md) | Depends on. A photo that the scan imports gets its import time through the same path. |

---

## 4. Current source audit

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Identity | `include/type/type.hpp`, `include/utils/id/id_generator.hpp` | `image_id_t` and `sl_element_id_t` are `uint32_t` counters. | None. |
| Element model | `include/sleeve/sleeve_element/sleeve_element.hpp` | `ElementType { FILE, FOLDER }`. No mounted mark. | Add `mount_id_`. |
| Sleeve tree | `FileSystem`, `include/sleeve/sleeve_filesystem.hpp` | Purely virtual tree. A file can be linked into several folders. | Scan operations and the structure check. |
| App service | `SleeveServiceImpl`, `include/app/sleeve_service.hpp` | One `fs_lock_`. `Write` runs the operation and then `Sync()`. | The structure check and the scan entry. |
| Schema | `include/storage/store/database.hpp`, `storage/store/database.cpp` | `init_table_query` for a new file. `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and `CREATE TABLE IF NOT EXISTS` on every open. No numbered migration. | New column and two tables in the same pattern. |
| Element mapper | `include/storage/mapper/sleeve/element/element_mapper.hpp` | `field_count_ = 6`. The projection is the six `Element` columns in table order. | Seventh field, last. |
| Source location | `Image.image_path`, `storage/mapper/image/image_mapper.cpp` | Absolute path. No byte size, modification time, or content hash. | `SourceFile` table. |
| Import | `ImportService::ImportToFolder`, `app/import_service.cpp` | References the file in place. Checks `is_regular_file` only. | Refuse a mounted destination. The scan reuses the import of one file. |
| Import UI | `ImportExportHandler`, `ui/alcedo_main/album_backend/import_export.cpp`; `CollectionsPanel.qml` `canTakeFileDrop`; `LibraryWorkspace.qml` grid drop | A drop imports into the row or the current folder. | Refuse a mounted target. |
| Folder tree UI | `FolderController`, `ui/alcedo_main/album_backend/folder_controller.cpp`; `CollectionsPanel.qml` | Row fields: ids, name, depth, path, `deletable`, counts. Menu: New collection, Rename, Delete. | Mounted fields, menus, disabled states. |
| Missing display | `ThumbnailManager`, `thumbnail_manager.cpp`; `thumbMissingSource` role | A failed thumbnail read checks `PathExists` and shows "Source file was moved or deleted". The state is in memory only. | Read the stored state for a mounted photo. |
| Path readers | Thumbnail, pipeline render, export, metadata, semantic input, DNG profile binding, editor image controller, legacy decoder scheduler | Each opens `image_path_`. A missing file throws from `ByteBufferLoader`. | None (P2). |
| Delete | `ImageController::StartDeleteImages`, `AlbumBrowseService::DeleteFilesInFolderByElementIds` | Root delete removes the element, the Image row, the pipelines, and the history. A folder delete unlinks. `delete_mark` is a flag. | Refuse a present mounted photo. |
| Background tasks | `BackgroundTaskController`, `BackgroundTaskKind` | No scan kind. | `MountScan` kind. |
| Project open | `ProjectService::LoadProject`; `lifecycle_hooks.project_opened` in `application_module_host.cpp` | Checks `project_file_version` (current 0.11.0, minimum 0.10.0). | Version 0.12.0. Start the scans after open. |
| File hash | `Hash128::Compute` (`include/type/hash_type.hpp`), `project_pack::ComputeFileChecksum` | XXH3 is available. No partial file hash. | Fingerprint function. |
| Volume and watcher | none | No `QStorageInfo`, `GetVolumeInformation`, `QFileSystemWatcher`, or platform helper directory. | Volume identity and watcher. |
| Automation | `ui/alcedo_main/automation/commands/library_commands.cpp` | `library.import` imports into the current folder. `library.folders` lists folders. | Four mount commands. |
| Tests | `tests/app/sleeve_service_test.cpp`, `tests/app/import_service_test.cpp`, `tests/app/project_compatibility_test.cpp`, `tests/sleeve/sleeve_fs_test.cpp`, `tests/storage/` | Registered with `alcedo_register_test_target`. | New tests in each phase. |

Not verified in the audit: how the editor shows a render failure for a file that does not exist.
MF6 records this behavior before it disables the editor for a missing photo.

---

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Can change | Can only read |
| --- | --- | --- | --- | --- |
| `MountStore` (proposed) | Mount and source records | Stored rows | `MountRoot`, `SourceFile` | none |
| `ElementStore` | Elements | `Element`, `FolderContent` rows | `Element.mount_id` with the other element fields | none |
| Source file utilities (proposed) | A path | Fingerprint, volume identity, listing | nothing | the user directory |
| `BuildMountScanChangeList` (proposed) | Records, listing, fingerprint function | Change list | nothing | its arguments |
| `MountService` (proposed) | Mount commands | Mount states, scan results | Mount records, and the sleeve tree through `SleeveServiceImpl` | the user directory |
| `SleeveServiceImpl` | Operations | The tree | The tree and its storage | none |
| `MountScanCoordinator` (proposed, UI layer) | Triggers | Scan requests, background task state | Its timers and watcher | mount list |

`MountService` belongs to the project, like `SleeveServiceImpl`. `ProjectService` creates it and
ends it with the project.

### 5.2 New value types

| Type | Purpose | Fields |
| --- | --- | --- |
| `SourceFileFingerprint` | The match key of a file. An existing hash type holds the value; the version is new. | 128-bit hash, version. |
| `MountDirectoryListing` | The result of one listing. It lives for one scan. | Reachable flag; directories (relative path); files (relative path, byte size, modification time). |
| `MountScanChangeList` | The change description that the apply step executes. It lives for one scan. | Lists for the cases of section 2.4 and for folder changes. |

Each type is a point-in-time value of one scan. No owner keeps it after the scan.

### 5.3 Data flow

```text
trigger (open | activation | watcher | Synchronize | library.sync)
  -> MountScanCoordinator: one scan per mount, register MountScan task
  -> MountService::ScanMount(mount_id, progress, stop request)
       -> ListMountDirectory(root)                      [worker, no lock]
       -> MountStore: read source records of the mount  [storage]
       -> BuildMountScanChangeList(records, listing, fingerprint fn)
       -> SleeveServiceImpl::ApplyMountChanges(batch 1) [fs_lock_, storage]
       -> for each 500 new files:
            read metadata                               [worker, no lock]
            SleeveServiceImpl::ApplyMountChanges(batch) [fs_lock_, storage]
       -> MountStore: write last scan time
  -> coordinator: finish task, refresh folder rows and thumbnails
```

### 5.4 Failure flow

```text
root unreachable
  -> listing.reachable = false
  -> MountService sets the mount offline, returns MountScanResult::Offline
  -> no record, element, or link changes

storage write fails in a batch
  -> the batch transaction rolls back
  -> the service restores the in-memory tree of the mount from storage
  -> ScanMount returns MountScanResult::StorageFailure with the batch number
  -> completed batches stay; the next scan computes the remaining changes again

stop request
  -> the current batch completes
  -> ScanMount returns MountScanResult::Canceled
```

---

## 6. File and API map

### 6.1 Proposed files

| File | Owner responsibility |
| --- | --- |
| `include/storage/store/mount/mount_store.hpp`, `storage/store/mount/mount_store.cpp` | `MountRoot` and `SourceFile` rows. |
| `include/storage/mapper/mount/mount_root_mapper.hpp`, `source_file_mapper.hpp` | duckorm field descriptions. |
| `include/utils/file/source_file_fingerprint.hpp`, `utils/file/source_file_fingerprint.cpp` | Fingerprint. |
| `include/utils/file/volume_identity.hpp`, `utils/file/volume_identity_windows.cpp`, `volume_identity_macos.mm`, `volume_identity_linux.cpp` | Volume identity. |
| `include/utils/file/mount_directory_listing.hpp`, `utils/file/mount_directory_listing.cpp` | Listing. |
| `include/app/mount_scan_change_list.hpp`, `app/mount_scan_change_list.cpp` | Comparison. |
| `include/app/mount_service.hpp`, `app/mount_service.cpp` | Mount operations. |
| `include/ui/alcedo_main/album_backend/mount_scan_coordinator.hpp`, `.cpp` | Triggers and task state. |
| Tests | `tests/storage/mount_store_test.cpp`, `tests/utils/source_file_fingerprint_test.cpp`, `tests/utils/mount_directory_listing_test.cpp`, `tests/app/mount_scan_change_list_test.cpp`, `tests/app/mount_service_test.cpp`, `tests/ui/mount_scan_coordinator_test.cpp` |

### 6.2 Proposed APIs

| API | Input | Output | Changes state | Error result |
| --- | --- | --- | --- | --- |
| `ComputeSourceFileFingerprint` | path | `SourceFileFingerprint` | No | Named error when the file cannot be read. |
| `ReadVolumeIdentity` | directory | identity string, can be empty | No | Empty identity. |
| `ListMountDirectory` | root, stop request | `MountDirectoryListing` | No | `reachable = false`. |
| `BuildMountScanChangeList` | records, listing, fingerprint function | `MountScanChangeList` | No | None. A fingerprint error makes the file "unreadable"; the scan skips it and reports the count. |
| `MountService::CreateMount` | parent folder id, directory | mount id | Yes | `MountError::NestedMount`, `ParentIsMounted`, `DirectoryUnreachable`. |
| `MountService::ScanMount` | mount id, progress, stop request | `MountScanResult` with counts | Yes | `Offline`, `StorageFailure`, `Canceled`. |
| `MountService::RemoveMount` | mount id | none | Yes | `StorageFailure`. |
| `MountService::ListMounts` | none | mount id, root folder id, directory, state, counts | No | None. |
| `MountService::RelocateRoot` | mount id, directory | none | Yes | `NestedMount`, `DirectoryUnreachable`. |
| `MountService::LocateMissingFile` | element id, file | none | Yes | `FileOutsideMount`, `FileAlreadyMounted`. |
| `MountService::RemoveMissingPhotos` | mount id | removed count | Yes | `StorageFailure`. |
| `SleeveServiceImpl::CanEditFolderStructure` | folder id | bool | No | None. |
| `SleeveServiceImpl::ApplyMountChanges` | mount id, one batch | none | Yes | Storage error. |

---

## 7. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| MF1 #983 | Mount record, element mount id, source file record | Storage, sleeve element | none | 900 to 1300 | implemented |
| MF2 #984 | Fingerprint, volume identity, directory listing | Source file utilities | none | 800 to 1200 | implemented |
| MF3 #985 | Scan change list | Scan change list | MF1, MF2 | 900 to 1400 | implemented |
| MF4 #986 | Mount service: create, scan, adopt, apply, unmount | Mount service, sleeve service | MF3 | 1400 to 1900 | implemented |
| MF5 #987 | Read-only structure | Sleeve service, import, automation | MF1 | 600 to 1000 | implemented |
| MF6 #988 | Library panel and thumbnail states | Library panel, folder controller | MF4, MF5 | 1300 to 1800 | planned |
| MF7 #989 | Scan triggers | Scan coordinator | MF6 | 700 to 1100 | planned |
| MF8 #990 | Missing photos, root relocation, automation commands | Mount service, panel, automation, documents | MF6 | 1300 to 1800 | planned |

The split puts each state change in one phase. MF4 holds the complete apply step, because a
partial apply step cannot keep the home link rule.

---

## 8. Detailed phases

Before each phase, the executor reads `AGENTS.md`, each applicable skill, and this plan again.

### MF1. Mount record, element mount id, and source file record

**Objective and deliverables.** The project database can store a mount, the mounted mark of an
element, and the source record of a file. A reviewer can read each record after a reopen.

**Inputs and prerequisites.** `main` at or after the audit revision.

**Modules, files, and APIs.**

- `include/storage/store/database.hpp`, `storage/store/database.cpp`: DDL.
- `include/storage/mapper/sleeve/element/element_mapper.hpp` and its source: seventh field.
- `include/sleeve/sleeve_element/sleeve_element.hpp`: `mount_id_`.
- `include/storage/store/sleeve/element_store.hpp`: write and read of `mount_id`.
- Proposed: `MountStore`, `MountRootMapper`, `SourceFileMapper`.
- `include/edit/history/pipeline_history_format.hpp`: project file version.

**Data rules and invariants.**

```text
MountRoot(id BIGINT PRIMARY KEY, root_folder_id BIGINT NOT NULL, volume_identity VARCHAR,
          root_path TEXT NOT NULL, last_scan_at TIMESTAMP)
Element.mount_id BIGINT            -- last column, NULL for an ordinary element
SourceFile(element_id BIGINT PRIMARY KEY, byte_size BIGINT NOT NULL, modified_ns BIGINT NOT NULL,
           fingerprint VARCHAR NOT NULL, fingerprint_version INTEGER NOT NULL,
           state UTINYINT NOT NULL, missing_since TIMESTAMP)
```

- `mount_id` is the last `Element` column in `init_table_query` and in the mapper.
- An old database gets the column through `ALTER TABLE Element ADD COLUMN IF NOT EXISTS`.
- The mount id counter starts at 1. Its next value is the maximum stored id plus 1.
- `state` is 0 for `present` and 1 for `missing`.
- The removal of an element removes its `SourceFile` row in the same operation.

**Implementation steps.**

1. Add the DDL for a new database and the statements for an old database. Follow the
   `image_marks_migration_query` precedent.
2. Add the seventh mapper field and `SleeveElement::mount_id_` as `std::optional<uint32_t>`.
3. Add `MountStore` with: insert, read all, update root path and volume identity, update last scan
   time, remove for `MountRoot`; upsert, read by mount, read by element, update state, remove for
   `SourceFile`. Each statement goes through duckorm.
4. Extend `ElementStore::RemoveElements` to remove the `SourceFile` rows.
5. Set `kProjectFileVersion` and `kMaxSupportedProjectFileVersion` to `0.12.0`. Keep the minimum.
   Follow each step of the 0.11.0 change, including the compatibility test fixture.
6. Update the storage documents that list the tables.

**Primary success call chain.**

```text
ProjectService::LoadProject -> Database::InitializeDB -> mount_schema_query
MountStore::InsertMountRoot -> duckorm insert -> MountRoot row
ElementStore sync -> ElementMapper (7 fields) -> Element row with mount_id
```

**Primary failure and restore call chain.**

```text
ALTER TABLE fails on open -> InitializeDB throws -> LoadProject reports the open error
-> the project file is unchanged, because the statement did not complete
```

**Tests and evidence.**

- `MountStoreTest.MountRootRowSurvivesProjectReopen`: all fields are equal after a reopen.
- `MountStoreTest.SourceFileStateChangeIsStored`: `present` to `missing` with `missing_since`.
- `MountStoreTest.RemovedElementHasNoSourceFileRow`.
- `MapperCrtpRoundtripTest` case for the seven-field element.
- `ProjectCompatibilityTest.Version011ProjectGainsMountColumnAndKeepsRootIds`: each element has a
  null `mount_id`, and the root id of each image is equal to the stored value. The test follows
  `Version010ProjectGainsLibraryMarksAndKeepsRootIds`.
- A test shows that a project with a file version above 0.12.0 is refused on open.

**Build and run commands.** Section 11.

**Exit criteria.**

- [x] The tests above pass in the debug build.
- [x] A project of version 0.10.0 and one of 0.11.0 open with no error.
- [x] No SQL text exists outside the storage module (`scripts/check_storage_boundary.py`: no
      findings).

**Expected diff.** 900 to 1300 lines. Actual: about 1 400 added and 80 removed lines in 39
files, of which about 550 are tests.

**Decisions made during the work.** The plan did not specify these details. Each one agrees with
the approved decisions.

- The value types `MountRootRecord`, `SourceFileRecord`, and `SourceFileState` are in
  `include/type/mount_records.hpp`, outside storage. MF3 takes them as input and has no storage
  include. `mount_id_t` is in `include/type/type.hpp` beside `sl_element_id_t`.
- `MountStore` follows `ElementStore`: it holds `Database&`, and each operation takes its own
  connection and the database lock. `Storage::GetMountStore()` returns it.
- The operations of step 3: `InsertMountRoot`, `ReadMountRoots`, `ReadNextMountId`,
  `UpdateMountRootLocation`, `UpdateMountLastScanTime`, `RemoveMountRoot`; `UpsertSourceFiles`,
  `ReadSourceFile`, `ReadSourceFilesOfMount`, `UpdateSourceFileState`, `RemoveSourceFiles`. The
  source record writes take a list, because MF4 writes them in batches. `UpdateSourceFileState`
  writes nothing when one element has no record. An update or removal of an unknown mount
  throws.
- `SourceFile.fingerprint` holds `Hash128::ToString` (32 hexadecimal digits). MF2 builds
  `SourceFileFingerprint` from `Hash128` and the version.
- `last_scan_at` and `missing_since` are UTC seconds (`std::time_t`). The text conversion of
  `Element.added_time` moved to `storage/mapper/utc_timestamp_text`, and the three mappers use it.
- `SleeveFile::Copy` and `SleeveFolder::Copy` keep `mount_id_`, because a copy-on-write copy
  replaces the element at the same path. `FileSystem::DuplicateFileToFolder` clears it, because
  the duplicate is a new photo (P5).
- MF1 adds no index on `Element.mount_id`. `ReadSourceFilesOfMount` is one scan of `Element` for
  each mount scan. The MF4 and MF7 time criteria measure it.
- The open error for an unsupported version names the range: `(supported 0.10.0 to 0.12.0)`.
- The 0.11.0 fixture `tests/resources/compat/raw_project_016c3535b.alcd` (Git LFS) was written
  by build `016c3535b`: it opened `raw_project_abdb000c8.alcd`, set the Green label on image 1,
  saved (project file 0.11.0), and packed the project.
- `tests/support/storage_invariants.cpp` has four new rules: a mounted element names a stored
  mount; the root folder of a mount is a folder of that mount; a mounted file has a source
  record; a source record names a file element.
- Step 6: no tracked document lists the tables. `docs/technical/` is not tracked; its local
  `sleeve_filesystem_api_and_storage.md` has the column and the two tables.

**Completion record.**

```text
Phase / date / status: MF1 / 2026-11-19 / implemented, pull request open
Source revision and branch: fb0ed60f5 (main 016c3535b + this plan), feature/mount-records-element-mount-id
Actual changed modules: Storage (Database DDL, ElementMapper, ElementStore, MountRootMapper,
  SourceFileMapper, MountStore, Storage), sleeve element (mount_id_, Copy, duplicate), project
  file version (pipeline_history_format.hpp, ProjectService open error), tests
Implemented behavior: Element.mount_id (last column, NULL = ordinary); MountRoot and SourceFile
  tables on new and old databases; SleeveElement::mount_id_ read and written with each element;
  MountStore write, read, update, remove; SourceFile row removed with its file element; project
  file 0.12.0 (minimum 0.10.0)
Explicitly unimplemented items: none of MF1. Mount operations, fingerprint, and scan are MF2 to MF4.
Primary success call chain:
  ProjectService -> Storage -> Database::InitializeDB -> mount_schema_query
  MountStore::InsertMountRoot -> MountRootMapper::Insert -> duckorm::insert -> MountRoot row
  ElementStore::AddElements / UpdateElement -> ElementMapper (7 fields) -> Element.mount_id
  ElementStore::RemoveElements -> SourceFileMapper::RemoveByIds -> no SourceFile row
Primary failure and restore call chain:
  ALTER TABLE or CREATE TABLE fails -> RunDdl throws -> Storage construction throws -> the open
  reports the error; the database file holds no part of the failed statement
  UpdateSourceFileState with an unrecorded element -> changed rows < ids -> throw before commit
  -> Transaction rolls back
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MountStoreTest
    MapperCrtpRoundtripTest ProjectCompatibilityTest ProjectServiceTest CommitGraphTest
    EditorMiniGitMaterializerTest PipelineEditBatchTest PipelineDocumentCheckpointTest SleeveFSTest
    SleeveServiceTest StorageSessionTest LibraryReferenceModelTest AlbumQueryTest
    LibrarySearchBenchmarkTest --parallel 4 -> 0
  Each binary from its _runtime folder (build/tmp/mf1/run_mf1_tests.ps1)
Discovered / passed / failed / skipped counts:
  MountStoreTest 14/14/0/0; MapperCrtpRoundtripTest 9/9/0/0; ProjectServiceTest 9/9/0/0;
  ProjectCompatibilityTest 6/5/1/0; CommitGraphTest 49/49/0/0; StorageSessionTest 4/4/0/0;
  SleeveFSTest 13/13/0/0; SleeveServiceTest 27/27/0/0; PipelineEditBatchTest 22/22/0/0;
  EditorMiniGitMaterializerTest 17/17/0/0; AlbumQueryTest 36/35/0/1;
  LibraryReferenceModelTest 5/5/0/0; PipelineDocumentCheckpointTest 23/20/3/0;
  LibrarySearchBenchmarkTest.DISABLED_RelevanceSelectOfFolderDoesNotGrowWithProjectSize 1/1/0/0
Manual verification: none required by MF1
Performance or resource evidence, if required: not required
Evidence path: build/tmp/mf1/test_logs/
Remaining defects or unavailable platforms:
  ProjectWrittenByCurrentMainOpensAndRendersUnchanged: render hash of the Intel Iris Xe OpenCL
    device differs from the recorded NVIDIA hash (#993); the open and render checks pass
  PipelineDocumentCheckpointFormat.Existing*Root*: fail on main (#553)
  ElementStore::UpdateElement of a file element throws on its FileImage row (#795); the
    MountStoreTest update case uses a folder
  macOS and Linux builds not run locally
```

### MF2. Source file fingerprint, volume identity, and directory listing

**Objective and deliverables.** Three read-only functions that the scan uses. A reviewer can run
each on a temporary directory.

**Inputs and prerequisites.** None. This phase can run in parallel with MF1.

**Modules, files, and APIs.** The proposed `utils/file` files of section 6.1.

**Data rules and invariants.**

- Fingerprint version 1: XXH3-128 of the 8-byte little-endian byte size, then the first 128 KiB,
  then the last 128 KiB. A file of 256 KiB or less contributes all its bytes one time.
- Volume identity: Windows uses the volume GUID path of `GetVolumeNameForVolumeMountPointW`. A UNC
  path uses the server and share name. macOS uses `NSURLVolumeUUIDStringKey`. Linux uses the file
  system UUID that `/dev/disk/by-uuid` maps to the device of the directory. A volume with no value
  has an empty identity.
- Listing: relative paths with `/` as the separator, names in normalization form C, sorted.
- The listing uses `is_supported_file`. It reads directory entries and file attributes only.
- The listing checks the stop request for each directory.
- The listing reports `reachable = false` when the root does not exist or cannot be opened. A
  directory below the root that cannot be opened is in a list of unreadable directories.

**Implementation steps.**

1. Add the fingerprint function. Open the file for shared read. Return a named error for a file
   that cannot be opened or that changes its size during the read.
2. Add the volume identity function with one source file for each platform. Register the files in
   CMake by platform.
3. Add the listing function with `std::filesystem::directory_iterator` and an explicit stack. Skip
   symbolic links, junctions (reparse points), and names that start with a period.
4. Add the tests.

**Primary success call chain.**

```text
ListMountDirectory(root) -> directories, files(relative path, byte size, modified_ns)
ComputeSourceFileFingerprint(path) -> SourceFileFingerprint{hash, 1}
```

**Primary failure and restore call chain.**

```text
root removed -> ListMountDirectory -> reachable = false -> caller makes no comparison
file locked by another process -> fingerprint error -> caller counts the file as unreadable
```

**Tests and evidence.**

- `SourceFileFingerprintTest.EqualContentAtTwoPathsHasEqualFingerprint`.
- `SourceFileFingerprintTest.ChangeInFirstOrLastBlockChangesFingerprint`.
- `SourceFileFingerprintTest.ChangeInTheMiddleOfALargeFileKeepsFingerprint`: this records the
  limit of version 1.
- `SourceFileFingerprintTest.SmallFileIsHashedInFull`.
- `MountDirectoryListingTest.MissingRootIsUnreachableAndEmptyRootIsEmpty`.
- `MountDirectoryListingTest.DirectoryLinkCycleEnds`: skipped with a stated reason when the
  platform account cannot create a link.
- `MountDirectoryListingTest.UnsupportedFilesAndPeriodDirectoriesAreAbsent`.
- `VolumeIdentityTest.TwoDirectoriesOfOneVolumeHaveOneIdentity`.

**Build and run commands.** Section 11.

**Exit criteria.**

- [x] The tests pass on Windows in the debug build.
- [x] The macOS and Linux sources compile. The repository has no Linux CI job, so the Linux
      build ran in WSL (Arch Linux, GCC 16). The macOS build ran on an Apple Silicon Mac (Apple
      clang 17). The macOS CI job builds and runs `SourceFileUtilsTest` through
      `alcedo_tests_ci_core`.
- [x] No function of this phase opens a file for writing. The fingerprint opens with
      `GENERIC_READ` or `O_RDONLY`; the listing and the volume identity open no file.

**Expected diff.** 800 to 1200 lines. Actual: 1 314 added lines in 13 files, of which 571 are
tests.

**Decisions made during the work.** The plan did not specify these details. Each one agrees with
the approved decisions.

- File type rule: a listed file is a regular file whose extension has an import category
  (`CategoryForPath` is not `ImportFileCategory::Other`): RAW, JPEG, TIFF, PNG, and OpenEXR. This
  is the rule of the folder import (`FolderImportScanModel`, `library.import`). The plan named
  `is_supported_file`, but that function accepts RAW extensions only and runs one `stat` for each
  file.
- The names of section 6.1 are kept. The functions are in the static library `SourceFileUtils`
  (`utils/CMakeLists.txt`). Qt Core is a private dependency for the NFC conversion
  (`QString::normalized`); the headers have no Qt include. A name with ASCII characters only is
  not converted.
- `MountDirectoryListing.status` replaces the reachable flag of section 5.2:
  `kComplete`, `kRootUnreachable`, or `kStopped`. A stopped listing is empty, so a caller cannot
  compare a part of a tree.
- The stop request is `const std::atomic<bool>*` (null for no request), the type of the folder
  import scan.
- A relative path is `std::wstring` with `/`, the type of the element names (`file_name_t`). The
  root directory is not in `directories`. `unreadable_directories` holds each directory that
  could not be opened or that holds an entry whose type or attributes could not be read; an
  empty path names the root.
- The period rule applies to files too. This removes the macOS AppleDouble files (`._name.ARW`).
- Skipped link types: `file_type::symlink` and the MSVC `file_type::junction`, read without a
  follow of the link. Other reparse points (cloud placeholder files, deduplicated files) report
  their target type and are listed.
- `modified_ns`: on Windows the directory entry holds the size and the time from the directory
  read; the MSVC file clock counts 100 ns ticks from 1601-01-01. On POSIX one `lstat` reads both
  values, because the directory entry runs one `stat` for each value.
- Fingerprint errors: `kCannotOpen`, `kReadFailed`, `kSizeChangedDuringRead`. A short read with
  a different size after the read reports `kSizeChangedDuringRead`. The fingerprint opens the
  file with the platform API and shares read, write, and delete access, so the user can rename or
  delete the file in the file manager during a scan (D1). A standard stream on Windows does not
  share delete access.
- Volume identity on Windows: `GetVolumePathNameW`, then the volume GUID path for a local volume.
  A network volume (`GetDriveTypeW` is `DRIVE_REMOTE`) gives `\\server\share` in lower case,
  from the UNC path or from `WNetGetConnectionW` for a mapped drive. A mapped drive and its UNC
  path thus have one identity.
- Test target: one executable `SourceFileUtilsTest` with the three suites, labels `ci_core_flow`
  and `mounted_folder`, categories `core` and `ci_core`.
- Tests added to the plan list: `MissingFileOrDirectoryReportsCannotOpen`,
  `ReadOnlyFileHasFingerprintAndKeepsItsModificationTime`,
  `FileLockedByAnotherHandleReportsCannotOpen` (Windows),
  `ModificationTimeIsNanosecondsSinceTheUnixEpoch`, `NamesAreInNormalizationFormC`,
  `SymbolicLinksToFilesAndDirectoriesAreAbsent`, `StopRequestEndsTheListingWithNoEntries`,
  `UnreadableSubdirectoryIsReportedAndTheRestIsListed` (POSIX),
  `TenThousandFilesAreListedWhileNoFileCanBeOpened`, `MissingDirectoryHasEmptyIdentity`.
- `DirectoryLinkCycleEnds` uses an NTFS junction on Windows, which needs no privilege, and a
  symbolic link elsewhere. The 10 000-file test locks each file against every other open
  (Windows: a handle with no share flags; POSIX: no read permission), and the listing still
  returns each file with its byte size.

**Completion record.**

```text
Phase / date / status: MF2 / 2026-11-20 / implemented, pull request open
Source revision and branch: 0d1d37412 (MF1, pull request #994) + this phase,
  feature/mount-source-file-utilities
Actual changed modules: source file utilities (new library SourceFileUtils under utils/file),
  utility tests (SourceFileUtilsTest)
Implemented behavior: ComputeSourceFileFingerprint (version 1, XXH3-128 of the byte size, the
  first 128 KiB, and the last 128 KiB; a file of 256 KiB or less in full); ReadVolumeIdentity
  (Windows volume GUID path or network share, macOS volume UUID, Linux /dev/disk/by-uuid name;
  empty when the volume has none); ListMountDirectory (directories and supported image files,
  byte size and modification time, NFC names, sorted; no link, junction, or period entry; a named
  status for an unreachable root and for a stop request)
Explicitly unimplemented items: none of MF2. The comparison is MF3.
Primary success call chain:
  ListMountDirectory(root, stop) -> is_directory(root) -> explicit directory stack
    -> directory_iterator -> symlink_status (no follow) -> CategoryForPath
    -> ReadFileAttributes (cached entry on Windows, one lstat on POSIX) -> NfcName
    -> sorted directories[] and files[](relative path, byte size, modified_ns), kComplete
  ComputeSourceFileFingerprint(path) -> ReadOnlySourceFile (shared read) -> size
    -> read head block and tail block -> size check -> Hash128::Compute
    -> SourceFileFingerprint{hash, 1}
  ReadVolumeIdentity(dir) -> GetVolumePathNameW -> GetVolumeNameForVolumeMountPointW
    | NSURLVolumeUUIDStringKey | /dev/disk/by-uuid st_rdev == st_dev -> identity text
Primary failure and restore call chain:
  root removed or not a directory -> ListMountDirectory -> kRootUnreachable, empty lists
    -> the caller makes no comparison (D5)
  subdirectory cannot be opened -> unreadable_directories[] -> the rest of the tree is listed
  stop request -> kStopped, empty lists
  file locked or removed -> kCannotOpen; file shortened during the read ->
    kSizeChangedDuringRead -> the caller counts the file as unreadable (MF3)
  volume without identity or missing directory -> empty identity
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SourceFileUtilsTest
    --parallel 4 -> 0
  ctest --test-dir build/debug -R "SourceFileFingerprintTest|MountDirectoryListingTest|
    VolumeIdentityTest" -> 0
  Linux (WSL alcedo-arch, GCC 16.2.1, Qt 6.12, gtest 1.18): g++ -std=c++20 -Wall -Wextra of
    the three sources and the three test files -> 0; test binary -> 0
  macOS (Apple clang 17, Homebrew Qt and xxHash, gtest of build/macos-debug): clang++ of the
    sources, the .mm with -fobjc-arc, and the tests -> 0; test binary -> 0
Discovered / passed / failed / skipped counts:
  Windows debug: 17 / 16 / 0 / 1 (SymbolicLinksToFilesAndDirectoriesAreAbsent: this account
    cannot create a symbolic link; the junction cycle test ran)
  Linux: 17 / 17 / 0 / 0; macOS: 17 / 17 / 0 / 0
Manual verification: none required by MF2
Performance or resource evidence, if required: not required. The 10 000-file listing test ran in
  7.9 s on Windows debug, most of it file creation and locks.
Evidence path: build/tmp/mf2/ (removed at the end of the task)
Remaining defects or unavailable platforms: no Linux CI job exists; the macOS CI result is on the
  pull request. A network share, a mapped drive, and a Windows mount folder were not tested,
  because the test account has no share and no second volume.
```

### MF3. Scan change list

**Objective and deliverables.** `BuildMountScanChangeList` returns the changes of section 2.4 for
one mount. A reviewer can read each rule in one test.

**Inputs and prerequisites.** The record types of MF1 and the listing type of MF2.

**Modules, files, and APIs.** Proposed `include/app/mount_scan_change_list.hpp` and its source.

**Data rules and invariants.**

- Input records: for each mounted file, the element id, the relative path, the source record. For
  each mounted folder, the element id and the relative path. For adoption, the map from
  `image_path` to element id of the ordinary files below the root directory.
- The function calls the fingerprint function only for a listed file with no record at its path,
  and it reads the stored fingerprint of a record with no file.
- The output lists are sorted by relative path.
- An element id is in one output list at most.
- A reachable listing with zero files makes each present record missing. The function has no
  count limit, because a missing record changes no user data (D4).

**Implementation steps.**

1. Join records and listing by relative path. Emit unchanged (count only), changed in place, and
   returned.
2. Collect records with no file and listed files with no record.
3. Take adoption candidates out of the listed files by `image_path`.
4. Fingerprint the remaining listed files. Group by fingerprint. Match with the order of
   section 2.4.
5. Emit moved, new, and missing.
6. Compute folder changes: directories to add (parents first) and mounted folders to remove
   (children first). Keep a folder that will hold a missing photo.

**Primary success call chain.**

```text
BuildMountScanChangeList(records, listing, fingerprint)
  -> changed_in_place[], moved[], new_files[], missing[], returned[], adopted[],
     folders_to_add[], folders_to_remove[], unreadable_count
```

**Primary failure and restore call chain.**

```text
fingerprint error for a listed file -> the file is in no list -> unreadable_count + 1
-> the next scan reads the file again
```

**Tests and evidence.** `MountScanChangeListTest`:

- `UnchangedFileNeedsNoFingerprint`: the fingerprint function call count is 0.
- `FileWithNewModificationTimeIsChangedInPlace`.
- `RenamedFileKeepsItsElement`.
- `RenamedDirectoryMovesEachFileAndReplacesTheFolder`.
- `FileMovedBetweenDirectoriesKeepsItsElement`.
- `CopyOfAPresentFileIsANewFile`.
- `TwoRecordsWithOneFingerprintMatchBySameNameFirst`.
- `CaseOnlyRenameIsAMove`.
- `RecordWithNoFileAndNoMatchBecomesMissing`.
- `MissingRecordWithFileAtItsPathReturns`.
- `OrdinaryFileBelowTheRootIsAdopted`.
- `LowestElementIdIsAdoptedWhenTwoOrdinaryFilesHaveOnePath`.
- `ReachableRootWithZeroFilesMakesEachRecordMissing`.
- `FolderOfAMissingPhotoStays`.

**Build and run commands.** Section 11.

**Exit criteria.**

- [x] Each test compares the complete returned change list with the expected list.
- [x] The function has no include of a storage, sleeve service, or Qt header. The header includes
      `type/mount_records.hpp`, `type/type.hpp`, and the two MF2 headers; the source includes
      the standard library only.

**Expected diff.** 900 to 1400 lines. Actual: 977 added lines of code in 5 files, of which 493 are tests.

**Decisions made during the work.** The plan did not specify these details. Each one agrees with
the approved decisions.

- Fingerprint reads: the function also reads the fingerprint of a listed file whose byte size or
  modification time differs from its record (changed in place, and returned with new values).
  The source record gets the values of the file (section 2.4), and a stored fingerprint of the
  old content would make a later move of that file unmatchable (D2). An unchanged file and a
  returned file with its recorded values are not read. A lost record contributes its stored
  fingerprint.
- A record whose `fingerprint_version` is not `kSourceFileFingerprintVersion` matches no new
  path. A match key is the hash and the version.
- A missing record takes part in the match like a present record: its file at a new path is a
  move, and the source record becomes present. A missing record with no match is counted in
  `still_missing_count` and is in no list, because it needs no write.
- Adoption input: `MountScanOrdinaryFile` holds the element id and the relative path below the
  root, with `/` and names in NFC. MountService (MF4) converts `Image.image_path`, because the
  conversion needs the root directory and Qt. An adopted file is fingerprinted, because the apply
  step writes its source record. Adoption runs before the fingerprint match, so the path
  identifies an ordinary photo before a lost record can claim the file.
- Name match: a record takes the first unmatched path, in sorted order, with an equal last
  element name. The comparison is exact, so a case-only rename is a move and not a same-name
  match.
- A listed file whose fingerprint cannot be read is in no list and is counted in
  `unreadable_count`. Its record, if any, stays as it is.
- Unreadable directories (`MountDirectoryListing::unreadable_directories`): a record with no
  listed file below such a directory is unverified (`unverified_count`). It does not become
  missing and it matches no path. A new path with the fingerprint of an unverified record is
  deferred (`deferred_count`) and is in no list, so a file that the listing could not see does
  not become a second photo (P5 applies only when the first file is known to be present). A
  folder at or below an unreadable directory stays.
- Folder rule: `folders_to_add` holds each directory above a listed file that has no folder
  record. `MountDirectoryListing::directories` is not read, because a directory with no
  supported file at any depth gets no folder (section 2.4). A folder record is removed when it
  is above no listed file, no missing photo, and no unverified record. This is the MF3 step 6
  rule "keep a folder that will hold a missing photo". It takes precedence over the sentence of
  section 2.4 that removes a folder after its elements became missing; the nearest-ancestor rule
  then applies only when MF4 finds no folder for a missing photo.
- Sort order: each file list by relative path (the new path of a move), `missing` then by element
  id. `folders_to_add` in ascending order, so a parent precedes its children;
  `folders_to_remove` in descending order, so a child precedes its parent.
- Two records at one relative path: the lower element id keeps the path, and the other record is
  lost. The tree does not produce this state; the rule keeps the output deterministic.
- A listing that is not complete (`kRootUnreachable`, `kStopped`) gives an empty change list and
  no fingerprint read (D5).
- The library is `MountScanChangeList` (`app/CMakeLists.txt`), with `SourceFileUtils` as its
  only dependency. The test target is `MountScanChangeListTest`, labels `ci_core_flow` and
  `mounted_folder`, categories `app` and `ci_core`.
- Tests added to the plan list: `FileWithNewModificationTimeIsChangedInPlace` also covers a new
  byte size; `RecordsWithOneFingerprintAndNoSameNameMatchInSortedPathOrder`,
  `MissingRecordWithItsFileAtANewPathIsMoved`, `UnreadableFileIsInNoListAndKeepsItsRecord`,
  `RecordBelowAnUnreadableDirectoryIsUnverifiedAndMatchesNoNewPath`,
  `RecordWithAnotherFingerprintVersionMatchesNoNewPath`,
  `IncompleteListingGivesNoChangesAndReadsNoFile`, and
  `FileMatchesOneRuleOnlyAndListsAreSortedByPath`. `MissingRecordWithFileAtItsPathReturns`
  covers a return with the recorded values (no read) and with new values (one read). The copy
  tests use two and three files with equal content.

**Completion record.**

```text
Phase / date / status: MF3 / 2026-11-20 / implemented, pull request open
Source revision and branch: 5fe3507c0 (MF2, pull request #995) + this phase,
  feature/mount-scan-change-list
Actual changed modules: scan change list (new library MountScanChangeList under app),
  application tests (MountScanChangeListTest)
Implemented behavior: BuildMountScanChangeList(records, listing, fingerprint) returns
  changed_in_place, moved, new_files, missing, returned, adopted, folders_to_add,
  folders_to_remove, and the counts unchanged, still missing, unreadable, unverified, and
  deferred, by the rules of section 2.4 and the decisions above
Explicitly unimplemented items: none of MF3. The apply step, the read of the records from
  storage, and the conversion of image_path to a relative path are MF4.
Primary success call chain:
  BuildMountScanChangeList -> sort records by (relative path, element id) -> record_at_path
    -> join listing.files by relative path -> unchanged | changed_in_place (fingerprint)
       | returned (stored fingerprint or fingerprint)
    -> lost records -> unverified (below an unreadable directory) | fingerprint groups
    -> new paths -> fingerprint -> adopted (ordinary_at_path, lowest id) | fingerprint groups
    -> MatchGroup: same file name, then sorted path -> moved; rest -> new_files | deferred;
       records -> missing | still missing
    -> folders: parents of listed files vs folder records, protected parents of missing and
       unverified records -> folders_to_add (ascending), folders_to_remove (descending)
    -> sort each file list by relative path
Primary failure and restore call chain:
  listing not complete -> empty change list, no fingerprint read -> caller makes no change (D5)
  fingerprint error for a listed file -> the file is in no list -> unreadable_count + 1
    -> the record stays -> the next scan reads the file again
  directory below the root unreadable -> its lost records are unverified, its folders stay
    -> a new path with their fingerprint is deferred -> the next complete listing decides
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MountScanChangeListTest
    --parallel 4 -> 0
  ctest --test-dir build/debug -R "MountScanChangeListTest" -> 0
  Linux (WSL alcedo-arch, GCC 16.2.1, gtest 1.18): g++ -std=c++20 -Wall -Wextra -Werror of the
    source and the test file -> 0; test binary -> 0
  Mutation check: the same-name pass of MatchGroup disabled -> TwoRecordsWithOneFingerprint
    MatchBySameNameFirst fails with the two moves swapped; source restored
Discovered / passed / failed / skipped counts:
  Windows debug: 21 / 21 / 0 / 0; Linux: 21 / 21 / 0 / 0
Manual verification: none required by MF3
Performance or resource evidence, if required: not required
Evidence path: none kept; the Linux build ran in /tmp/mf3_build and was removed
Remaining defects or unavailable platforms: no Linux CI job exists; the macOS result is the macOS
  CI job of the pull request (alcedo_tests_ci_core)
```

### MF4. Mount service

**Objective and deliverables.** A project can hold a mount that follows its directory. A reviewer
can move a file in a temporary directory, scan, and see the same element at the new path.

**Inputs and prerequisites.** MF1, MF2, MF3.

**Modules, files, and APIs.**

- Proposed `MountService` (section 6.2).
- `include/app/sleeve_service.hpp`: `ApplyMountChanges`.
- `include/sleeve/sleeve_filesystem.hpp`: operations that the apply step needs and that the
  current API does not have (rename of a file element, move of a home link). The executor adds the
  minimum set and lists it in the completion record.
- `app/import_service.cpp`: the import of one file by reference becomes callable by the scan with
  a mounted destination. The public `ImportToFolder` keeps its behavior.
- `include/app/project_service.hpp`: `GetMountService()`.

**Data rules and invariants.**

- After each batch, section 2.5 holds: each mounted file has one home link, a `SourceFile` row,
  and an `image_path` that equals the root directory plus the relative path.
- A moved element keeps its element id, image id, pipeline rows, edit history, Versions, rating,
  labels, Delete mark, import time, and links to ordinary folders.
- An adopted element keeps the same data. It gets `mount_id`, a home link, and a `SourceFile` row.
- A new file gets the import time of the scan and the import defaults of the project.
- `RemoveMount` sets `mount_id` to null for each file, removes the home links, keeps the
  `SourceFile` rows and `image_path`, and removes the mounted folder elements and the `MountRoot`
  row. The photos stay in the library.
- `CreateMount` compares normalized absolute paths with a separator at the end, so that
  `D:/Photos2` is outside `D:/Photos`.

**Implementation steps.**

1. Add `CreateMount`: validate (D12, P4), read the volume identity, create the root folder element
   with `mount_id`, insert the `MountRoot` row, in one storage transaction.
2. Add the record read for one mount: mounted files and folders with relative paths (P3), and the
   ordinary files below the root directory.
3. Add `ApplyMountChanges` for the first batch: folders to add, adopted, moved, changed in place,
   returned, missing, folders to remove. One transaction. One `fs_lock_` scope.
4. Add the new file batches: read metadata with no lock, then create elements, Image rows, home
   links, and `SourceFile` rows in one transaction for each batch of 500.
5. Invalidate the thumbnail of each changed-in-place and moved element through the current
   thumbnail owner.
6. Add the offline result and the stop request.
7. Add `RemoveMount` and `ListMounts`.
8. When a storage write fails, restore the in-memory tree of the mount from storage with the
   operation that loads folder children. If the current code has no such operation for a loaded
   subtree, stop, open an issue, and update this plan.

**Primary success call chain.** Section 5.3.

**Primary failure and restore call chain.** Section 5.4.

**Tests and evidence.** `MountServiceTest`, with real temporary directories and a real project
database:

- `FirstScanBuildsFoldersAndPhotosOfTheDirectory`.
- `MovedFileKeepsElementIdAndEditHistoryRootId`.
- `RenamedDirectoryKeepsEachElement`.
- `DeletedFileBecomesMissingAndKeepsItsRating`.
- `FileThatReturnsBecomesPresent`.
- `OrdinaryPhotoBelowTheRootIsAdoptedWithItsCollectionLinks`.
- `UnreachableRootSetsOfflineAndChangesNoRecord`.
- `NestedDirectoryIsRefusedInBothDirections`.
- `SiblingDirectoryWithCommonNamePrefixIsAccepted`.
- `CanceledScanIsCompletedByTheNextScan`.
- `UnmountKeepsPhotosAsOrdinaryPhotos`.
- `StateIsEqualAfterProjectReopen`.
- `ScanLeavesTheDirectoryUnchanged`: names, sizes, and modification times of the directory are
  equal before and after each test scan.

**Build and run commands.** Section 11.

**Exit criteria.**

- [x] The tests pass in the debug build.
- [x] The second scan of an unchanged directory of 2 000 files calls the fingerprint function 0
      times. The test asserts the count and records it as the gtest property
      `second_scan_fingerprint_reads`.
- [x] No code path of the service opens a user file for writing. The service opens a file only
      through `ComputeSourceFileFingerprint` (shared read, MF2); the import reads metadata. Each
      test scan compares the directory content before and after the scan.

**Expected diff.** 1400 to 1900 lines. If the sleeve operations of step 3 pass 400 lines, move
them to a separate pull request before the service. Actual: about 2 600 added lines, of which
about 1 100 are tests. The sleeve operations are about 900 lines with their tests, so they are a
separate pull request: (A) the #795 fix, the `FileSystem` operations, the mount tree, and
`MountTreeTest`; (B), on top of (A), the service, the importer port, and `MountServiceTest`.

**Decisions made during the work.** The plan did not specify these details, or a better approach
appeared during the work. Each one agrees with the approved decisions.

- Relative path of a mounted file. The library root holds each file under a name that no other
  file has (`FileSystem::Create` appends `@`), so two files with one name in two directories
  cannot both have the disk name as their element name. The relative path of a file therefore
  comes from `Image.image_path` below the root directory (P2: the scan is its only writer). P3
  applies to folders: a folder name is the directory name. A file element takes the disk name or
  its first free form `name@`, `name@@`, ... (`FileSystem::RenameFile`); the element name is a
  display name.
- No shared transaction. Each store opens its own connection, so the tree (ElementStore through
  `SleeveServiceImpl::Sync`), `Image.image_path` (ImagePoolService), and the source records
  (MountStore) cannot share one transaction. Each batch writes them in this order: tree, then
  `image_path`, then the source records. Each interrupted state is one that the next scan
  completes:
  - tree written, `image_path` not: the record is at the old path, so the file is moved again; a
    placement that is already done changes nothing.
  - tree and `image_path` written, source record not (or a new file with no record): the scan
    reads a mounted file with no record as a record with zero values, so the file is changed in
    place (its record is written) or missing. `MountServiceTest.FileWithoutASourceRecordGets
    OneFromTheNextScan` drives this state.
  This replaces the one transaction of steps 3 and 4 and the restore of step 8. A failed sync
  keeps the changes in memory with their sync flags, and the next sync writes them (the model of
  `SleeveServiceImpl`), so no reload of a loaded subtree is necessary and no issue was opened.
- `CreateMount` writes the folder element, then the `MountRoot` row; when the row write fails,
  the folder is removed again. `RemoveMount` syncs the tree, then removes the row; a removal
  after an interrupted removal converts the stored files only.
- Importer port. `ProjectService` owns `MountService`, and `ImportService` depends on
  `ProjectService` through `PipelineMgmtService`. The scan therefore imports through
  `MountFileImporter` (`app/mount_file_importer.hpp`), which `ImportServiceImpl` implements with
  the import of `ImportToFolder` (one placement function for each import; `ImportToFolder` keeps
  its behavior). The caller passes its import service in `MountScanRequest`, so a new file gets
  the import defaults of the application.
- Thumbnail invalidation. `ThumbnailService` is created by the user interface layer, so
  `MountScanRequest::invalidate_thumbnail` is a callback. It runs after batch 1 for each file
  that changed in place, moved, or returned (the tile of a returned photo showed the missing
  state).
- Offline state. `MountReachability` (`kNotScanned`, `kOnline`, `kOffline`) is held in memory by
  `MountService`. It is not stored: no schema change; after a reopen a mount is `kNotScanned`
  until its next scan.
- Root directory. `CreateMount` stores `std::filesystem::canonical` of the directory (links
  resolved, the real case on Windows). The nested check and the adoption input compare whole
  path elements (`IsPathAtOrBelow`, `MountRelativePathOf` in `SourceFileUtils`), without case on
  Windows; the names below the root keep their spelling.
- Errors. `MountOperationError` holds a `MountError`: `kNestedMount`,
  `kDirectoryUnreachable`, `kUnknownMount`, and `kStorageFailure`. #1076 removed
  `kParentIsMounted` and `kParentIsNotAFolder` with the parent parameter of `CreateMount`.
  `ScanMount` returns `MountScanResult` with `kComplete`, `kOffline`, `kCanceled`, or
  `kStorageFailure`, the failed batch number, the counts, and `fingerprint_read_count`.
- `MountScanRequest::new_file_batch_size` (default 500) lets a test use small batches. The stop
  request is read before each new file batch; `last_scan_at` is written only by a complete scan.
- One mutex serializes `CreateMount`, `ScanMount`, and `RemoveMount`: MF6 calls them from the user
  interface while a scan runs on a worker. `ListMounts` does not wait for a scan.
- A missing record is written with `UpsertSourceFiles` and the stored values, so a file with no
  record can become missing.
- A new file that the import refuses has no element and no record; the next scan tries it again
  (`import_failed_count`).
- `FileSystem` operations added (the minimum set): `CreateFolderIn`, `RenameFile`,
  `RemoveFolderFrom`; `Delete(id)` and `RemoveFolderFrom` share `ReleaseChildFolders`. The move of
  a home link uses `UnlinkFileFromFolder` and `LinkFileToFolder`. The tree code is
  `app/mount_tree` (`ReadMountTree`, `ApplyMountTreeChanges`, `CreateMountRootFolder`,
  `RemoveMountTree`), with `SleeveServiceImpl` entry points of the same names.
- A new directory whose name a mounted file of the same folder holds (`name@`) makes that file
  take the next free name, so the folder keeps the directory name.
- #795 is fixed here: a moved or adopted file element is a modified element, and its sync threw
  on the `FileImage` row. `FileMapper::WriteBinding` updates the row, or inserts it when the file
  has none.
- Tests added to the plan list: `FailedBatchEndsTheScanAndTheNextScanCompletesIt` (an importer
  that fails), `FileWithoutASourceRecordGetsOneFromTheNextScan`, `MountTreeTest` (9 cases),
  `MountStoreTest.FileElementUpdateWritesItsFieldsAndKeepsOneImageBinding` (#795), and the two
  path helper cases of `MountDirectoryListingTest`. The 2 000-file criterion is
  `SecondScanOfAnUnchangedDirectoryReadsNoFile`.

**Completion record.**

```text
Phase / date / status: MF4 / 2026-11-20 / implemented, pull requests open
Source revision and branch: 3fb1652b5 (MF3, pull request #996) + this phase,
  feature/mount-tree-operations (A), feature/mount-service (B, on A)
Actual changed modules: element store (#795), sleeve file system (CreateFolderIn, RenameFile,
  RemoveFolderFrom), mount tree (app/mount_tree, SleeveServiceImpl entry points), mount store
  (file location reads), source file utilities (IsPathAtOrBelow, MountRelativePathOf), import
  service (placement function, MountFileImporter), mount service (new library MountService),
  project service (GetMountService), tests
Implemented behavior: CreateMount (canonical root, D12, P4, volume identity, root folder with the
  mount id, MountRoot row); ScanMount (listing, record read, change list, batch 1: folders,
  moves, adoption, changes in place, returns, missing; batches of new files through the importer;
  offline; stop request; storage failure with the batch number); RemoveMount (D11); ListMounts
Explicitly unimplemented items: none of MF4. The structure check is MF5, the panel MF6, the
  triggers MF7, locate and removal of missing photos MF8.
Primary success call chain:
  MountService::ScanMount -> ListMountDirectory (no lock)
    -> MountScanRun::ReadRecords: SleeveServiceImpl::ReadMountTree (fs_lock_),
       MountStore::ReadFileLocationsOfMount / ReadSourceFilesOfMount / ReadOrdinaryFileLocations
       -> MountRelativePathOf
    -> BuildMountScanChangeList (fingerprint: ComputeSourceFileFingerprint)
    -> batch 1: SleeveServiceImpl::ApplyMountTreeChanges (fs_lock_: CreateFolderIn, PlaceFile:
       Unlink/RenameFile/LinkFileToFolder, RemoveFolder) -> Sync
       -> ImagePoolService::Write_NoSync (image_path, file name) -> SyncWithStorage
       -> MountStore::UpsertSourceFiles -> invalidate_thumbnail
    -> each 500 new files: MountFileImporter::ImportMountedFiles (ImportServiceImpl: StartImport
       with the mount placement -> metadata and roots -> SyncImports) -> UpsertSourceFiles
    -> MountStore::UpdateMountLastScanTime -> kComplete
  MountService::CreateMount -> canonical -> parent and nested checks -> ReadVolumeIdentity
    -> SleeveServiceImpl::CreateMountRootFolder -> MountStore::InsertMountRoot
  MountService::RemoveMount -> SleeveServiceImpl::RemoveMountTree (mount id cleared, folders
    deleted, files stay) -> MountStore::RemoveMountRoot
Primary failure and restore call chain:
  root unreachable -> kRootUnreachable -> kOffline, MountReachability::kOffline, no write
  stop request -> before the next new file batch -> kCanceled; stored batches stay; no
    last_scan_at; the next scan imports the files with no record
  storage write fails -> exception in the batch -> kStorageFailure, failed_batch, error; the
    earlier batches stay; the next scan computes the rest (write order above)
  CreateMount: refused -> MountOperationError, nothing written; InsertMountRoot fails ->
    RemoveMountTree of the new folder -> kStorageFailure
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MountServiceTest MountTreeTest
    MountStoreTest SourceFileUtilsTest MountScanChangeListTest SleeveServiceTest SleeveFSTest
    ImportServiceTest ImportContentClassificationTest ProjectServiceTest
    ProjectCompatibilityTest MapperCrtpRoundtripTest LibraryReferenceModelTest --parallel 8 -> 0
  Each binary from its _runtime folder (build/tmp/mf4/final_run.ps1)
  python scripts/check_storage_boundary.py -> no findings
  Mutation check: the image_path write of batch 1 disabled -> MovedFileKeepsElementIdAndEdit
    HistoryRootId and RenamedDirectoryKeepsEachElement fail; source restored
Discovered / passed / failed / skipped counts:
  MountServiceTest 16/16/0/0; MountTreeTest 9/9/0/0; MountStoreTest 15/15/0/0;
  SourceFileUtilsTest 19/18/0/1 (SymbolicLinksToFilesAndDirectoriesAreAbsent: this account
    cannot create a symbolic link); MountScanChangeListTest 21/21/0/0; SleeveServiceTest
  27/27/0/0; SleeveFSTest 13/13/0/0; ImportServiceTest 15/15/0/0;
  ImportContentClassificationTest 11/11/0/0; ProjectServiceTest 9/9/0/0;
  ProjectCompatibilityTest 6/5/1/0; MapperCrtpRoundtripTest 9/9/0/0;
  LibraryReferenceModelTest 5/5/0/0
Manual verification: none required by MF4
Performance or resource evidence, if required: the second scan of 2 000 unchanged files read 0
  fingerprints. The whole 2 000-file test (2 000 JPEG writes, the import, two scans) ran in
  about 74 s in the debug build.
Evidence path: build/tmp/mf4/final_logs/
Remaining defects or unavailable platforms:
  ProjectWrittenByCurrentMainOpensAndRendersUnchanged: render hash of the Intel Iris Xe OpenCL
    device (#993), as in MF1
  ImagePoolService::Write locks its mutex twice (#811); the tests use Write_NoSync and a sync
  macOS and Linux builds not run locally; the macOS CI job builds the libraries
```

### MF5. Read-only structure of a mounted folder

**Objective and deliverables.** Each user operation that changes the structure of a mounted
folder returns a named error. A reviewer can call each operation and see an unchanged project.

**Inputs and prerequisites.** MF1. The tests set `mount_id` through the stores, so this phase
does not need MF4.

**Modules, files, and APIs.**

- `include/app/sleeve_service.hpp`, `app/sleeve_service.cpp`: `CanEditFolderStructure` and the
  check in `CreateFolder`, `RenameFolder`, `MoveFolder`, `DeletePath`, `DeleteElement`,
  `LinkFileToFolder`, `DuplicateFileToFolder`, `DeleteFile(s)FromFolder`, `DeleteFile(s)Everywhere`.
- `app/import_service.cpp`: `ImportToFolder` refuses a mounted destination.
- `app/album_browse_service.cpp`: `DeleteFilesInFolderByElementIds`.
- `ui/alcedo_main/automation/commands/library_commands.cpp`: `library.import`, `library.delete`.

**Data rules and invariants.**

- `CanEditFolderStructure(folder)` is `folder.mount_id_` has no value. It reads one field.
- `MoveFolder` checks the folder that moves and the destination.
- Delete everywhere is refused for a mounted file whose source record is `present`. It is accepted
  for a mounted file whose record is `missing` only through `MountService::RemoveMissingPhotos`
  (MF8).
- The error names the rule: `SleeveError::MountedFolderStructureIsReadOnly` or
  `SleeveError::MountedPhotoIsPresent`. Each caller reports the error text.
- `ApplyMountChanges` does not use these checks. It is the scan entry.

**Implementation steps.**

1. Add the check function and the two errors.
2. Add the check to each operation in the list above, before any change.
3. Return the error from `ImportToFolder` before it reads a source file.
4. Map the errors to automation error results.
5. Add the tests.

**Primary success call chain.**

```text
FolderController::CreateFolderIn(ordinary) -> SleeveServiceImpl::CreateFolder -> check passes
SleeveServiceImpl::LinkFileToFolder(mounted file, ordinary folder) -> check passes -> link
```

**Primary failure and restore call chain.**

```text
SleeveServiceImpl::CreateFolder(mounted parent) -> MountedFolderStructureIsReadOnly
-> no element, no storage write -> the caller shows the error
```

**Tests and evidence.**

- `SleeveServiceTest.EachStructureOperationOnAMountedFolderReturnsTheNamedError`: one case for
  each operation; the folder listing is equal before and after.
- `SleeveServiceTest.MountedFileLinksIntoAnOrdinaryCollection`.
- `SleeveServiceTest.OrdinaryFolderCannotMoveIntoAMountedFolder`.
- `ImportServiceTest.ImportIntoAMountedFolderIsRefusedBeforeAnySourceRead`.
- `AutomationLibraryCommandsTest.ImportWithMountedCurrentFolderReturnsTheNamedError`.
- `AlbumBrowseServiceMountTest.DeleteOfAPresentMountedPhotoIsRefused` (proposed test file
  `tests/app/album_browse_service_mount_test.cpp`).

**Build and run commands.** Section 11.

**Exit criteria.**

- [x] Each operation in the list has one test. `EachStructureOperationOnAMountedFolderReturns
      TheNamedError` has one case for each operation and reads the tree before, after, and after
      a reopen.
- [x] The check exists in one function: `SleeveServiceImpl::CanEditFolderStructure`.

**Expected diff.** 600 to 1000 lines. Actual: about 760 added lines, of which about 410 are
tests.

**Decisions made during the work.** The plan did not specify these details. Each one agrees with
the approved decisions.

- Errors. `SleeveOperationError` holds a `SleeveError` (`app/sleeve_error.hpp`, a small header so
  that user interface headers do not include the sleeve service). A third code,
  `kMountedPhotoIsMissing`, names the refusal of Remove from Library for a mounted photo whose
  record is `missing`: the data rule accepts that removal only through `RemoveMissingPhotos`
  (MF8), and `kMountedPhotoIsPresent` would name the wrong rule. `SleeveErrorName` gives the
  stable names `mounted_folder_structure_is_read_only`, `mounted_photo_is_present`, and
  `mounted_photo_is_missing`.
- The folder checks read one field (`CanEditFolderStructure`). Remove from Library reads the
  source record of each mounted file in the request; a mounted file with no record counts as
  present, because a failed later batch of a scan can leave the record of an adopted file
  unwritten (MF4 completion record).
- A request with several files is refused as a whole when one file is refused, so the refusal
  never leaves a partial change. `AlbumDeleteResult::refusal_` then holds the code, and each id
  of the request is failed.
- `DeletePath` follows `FileSystem::Delete`: a folder checks the folder, a file of the root checks
  Remove from Library, and a file of another folder checks that folder (the home link).
- `ImportToFolder` calls `CheckFolderStructureEditable(dest)` on the caller thread before
  `StartImport`. A destination that names no folder passes, so `ImportToNonExistentDestination`
  keeps its per-file failure.
- Callers. `AlbumBrowseService::RenameFolder` and `MoveFolder` return the new status
  `StructureIsReadOnly`; `CreateFolder` and `DeleteFolder` pass the `SleeveOperationError` on.
  `FolderController` shows one status text for each. `ImageController` shows the text of the code
  for a refused delete or link, and its result maps carry `refusal` with the error name.
- Automation. `library.import` checks the current folder before it starts the import and answers
  -32002 (rejected) with `data.error` set to the error name; `library.delete` answers the same
  form from the `refusal` of `DeleteTargetsFinished`. The CLI reference of the alcedo-skills
  repository is regenerated with the mount commands of MF8.
- `MountTreeTest` links a file into a second mounted folder through the tree in its setup, as a
  scan does; the user entry now refuses that link.
- An ordinary folder that holds a mount root can still be deleted, and the delete removes the
  mount tree with it. The check of D6 reads the target folder only, so the fix changes an
  approved decision: #999.

**Completion record.**

```text
Phase / date / status: MF5 / 2026-11-20 / implemented, pull request open
Source revision and branch: b686ee193 (MF4, pull request #998) + this phase,
  feature/mount-read-only-structure
Actual changed modules: sleeve service (SleeveError, CanEditFolderStructure, the checks), import
  service (ImportToFolder), album browse service (refusal result, edit status), folder and image
  controllers (status texts, refusal in the result maps), automation library commands
  (library.import, library.delete), tests
Implemented behavior: the named error for new collection, rename, move (folder and destination),
  delete, import, link, duplicate, and removal of the home link in a mounted folder; the named
  error for Remove from Library of a mounted photo (present or missing); a mounted photo links
  into and leaves an ordinary collection; library.import with a mounted current folder answers
  -32002 with data.error; the scan entry (ApplyMountTreeChanges, CreateMountRootFolder,
  RemoveMountTree) does not use the checks
Explicitly unimplemented items: the disabled menu entries and the drop cursor (MF6); Remove
  Missing Photos (MF8); the delete of an ordinary folder that holds a mount root (#999)
Primary success call chain:
  FolderController::CreateFolderIn(ordinary) -> AlbumBrowseService::CreateFolder
    -> SleeveServiceImpl::CreateFolder (fs_lock_: RequireEditableStructure(parent) passes
    -> FileSystem::Create) -> Sync
  SleeveServiceImpl::LinkFileToFolder(mounted file, ordinary folder) -> RequireEditableFolder
    passes -> FileSystem::LinkFileToFolder -> Sync
Primary failure and restore call chain:
  SleeveServiceImpl::CreateFolder(mounted parent) -> CanEditFolderStructure false
    -> SleeveOperationError(kMountedFolderStructureIsReadOnly) before FileSystem::Create; Write
    does not sync -> no element, no storage write -> FolderController status text
  ImageController delete job -> AlbumBrowseService::DeleteFilesInFolderByElementIds(0, ids)
    -> SleeveServiceImpl::DeleteFilesEverywhere -> RequireRemovableFromLibrary
    -> MountStore::ReadSourceFile -> SleeveOperationError(kMountedPhotoIsPresent) -> refusal_
    -> DeleteTargetsFinished {refusal, message} -> library.delete -32002 with data.error
  library.import -> SleeveServiceImpl::CheckFolderStructureEditable(current folder)
    -> -32002 with data.error; no folder scan and no import job
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SleeveServiceTest
    ImportServiceTest AlbumBrowseServiceMountTest MountTreeTest MountServiceTest
    AutomationLibraryCommandsTest AlbumBackendFolderTest AlbumBackendImageDeleteTest
    LibraryMutationOperationsTest LibraryReferenceModelTest SleeveFSTest --parallel 8 -> 0
  Each binary from its _runtime folder; UI and automation tests with QT_QPA_PLATFORM=offscreen
  python scripts/check_storage_boundary.py -> no findings
  Mutation check: CanEditFolderStructure returns true -> the two folder cases of
    MountedFolderStructureTest and both AlbumBrowseServiceMountTest cases fail; source restored
Discovered / passed / failed / skipped counts:
  SleeveServiceTest 30/30/0/0; ImportServiceTest 16/16/0/0; AlbumBrowseServiceMountTest
  2/2/0/0; MountTreeTest 9/9/0/0; MountServiceTest 16/16/0/0; AutomationLibraryCommandsTest
  15/15/0/0; AlbumBackendFolderTest 16/16/0/0; AlbumBackendImageDeleteTest 12/12/0/0;
  LibraryMutationOperationsTest 5/5/0/0; LibraryReferenceModelTest 5/5/0/0; SleeveFSTest
  13/13/0/0
Manual verification: none required by MF5
Performance or resource evidence, if required: not required
Evidence path: test output of the commands above (local)
Remaining defects or unavailable platforms:
  #999: deleting an ordinary folder that holds a mount root deletes the mount tree
  macOS and Linux builds not run locally; the macOS CI job builds the libraries
```

### MF6. Library panel and thumbnail states

**Objective and deliverables.** The user can mount a folder, see mounted rows, synchronize, and
unmount in the Library. Section 2.1 to 2.3 define the result.

**Inputs and prerequisites.** MF4, MF5. The executor loads `.agents/skills/alcedo-qml-ui/SKILL.md`.

**Modules, files, and APIs.**

- `FolderController`: row fields `mounted`, `mountRoot`, `mountOffline`, `mountMissingCount`,
  `structureEditable`; commands `MountFolder`, `SynchronizeMount`, `Unmount`,
  `ShowFolderInFileManager`.
- `CollectionsPanel.qml`, `CollectionTree.js`: mark, menus, disabled states, drop refusal in
  `canTakeFileDrop` and in the row drag rules.
- `LibraryWorkspace.qml`: grid drop refusal for a mounted current folder.
- `ImportExportHandler`: no import start for a mounted target.
- `BackgroundTaskKind::MountScan` and its text (implemented in MF7).
- Thumbnail model: `thumbMissingSource` is true when the stored state of a mounted photo is
  `missing`. The in-memory check stays for an ordinary photo.
- `ImageController`: delete command availability for the selection.
- Translation sources for the new text. Do not run `lupdate` on a partial scan.

**Data rules and invariants.**

- The panel reads the mounted state from the row fields only. It holds no second mount list.
- The scan task reports the counts of new, moved, changed, and missing photos.
- A scan that ends refreshes the folder rows, the image counts, and the thumbnails of changed
  elements.

**Implementation steps.**

1. Add the row fields and commands to `FolderController`.
2. Call `MountScanCoordinator::RequestScan` with `kSynchronize` for Synchronize, and
   `ReloadMounts` after Mount Folder and Unmount. The task kind and the scan task are in MF7.
3. Add the Mount Folder menu item and the folder dialog.
4. Add the mark, the Offline label, and the menus.
5. Disable the structure commands and the drops from the row fields.
6. Read the missing state for the tile.
7. Record how the editor reports a render failure for a file that does not exist. Then disable
   the editor open for a missing photo.
8. Add the tests.

**Primary success call chain.**

```text
CollectionsPanel "Mount Folder" -> FolderController::MountFolder(parent, url)
  -> MountService::CreateMount -> RegisterTask(MountScan) -> worker: ScanMount
  -> FinishTask -> folder rows reload -> counts and thumbnails refresh
```

**Primary failure and restore call chain.**

```text
CreateMount returns NestedMount -> FolderController reports the error text -> no row is added
ScanMount returns Offline -> FinishTask(failed, "Offline") -> row shows Offline
```

**Tests and evidence.**

- `FolderControllerMountTest.MountedRowReportsMountedAndNotStructureEditable` (proposed test
  file `tests/ui/folder_controller_mount_test.cpp`).
- `FolderControllerMountTest.OfflineMountRowReportsOffline`.
- QML test: the menu of a mounted row has no enabled New Collection, Rename, or Delete.
- QML test: `canTakeFileDrop` is false for a mounted row and true for an ordinary row.
- QML test: the tile of a stored missing photo shows the missing state.
- The QML tests assert the result with an explicit check after each `QTRY`, so that a timeout
  fails the test (#526).
- User check in the application: mount, synchronize, unmount, drop refusal.

**Build and run commands.** Section 11. QML tests run offscreen.

**Exit criteria.**

- [ ] The tests pass in the debug build.
- [ ] The user confirms the panel.
- [ ] No explanatory text was added to the panel.

**Expected diff.** 1300 to 1800 lines.

**Implementation decisions (2026-11-20, built with MF8 in the pull request of MF8 part B).**

- The mount states of the rows come from the photo count read: the job on the library query
  worker reads `MountService::ListMounts` with `ReadFolderImageCounts`, so the UI thread reads no
  mount table. `MountSummary` carries the ids of the present and the missing photos of each mount.
  Each scan end starts a count read, because an offline result changes the rows with no photo
  change.
- The row fields are `mountId` (0 for an ordinary row), `mountRoot`, `mountOffline`,
  `mountMissingCount` (on the root row), and `structureEditable`. The plan field `mountRoot` (the
  root path) is not a row field; Show in File Manager reads the path from the mount summary.
- The mount commands run through `MountScanCoordinator::RunMountOperation` on the scan worker.
  `MountService` holds its operation lock for the whole length of a scan, so a command on the UI
  thread would wait for the scan. An operation runs after the running scan and before the queued
  scans. `RequestScan` takes an optional callback with the result of the requested scan.
- The stored missing state reaches the tiles through `ThumbnailManager::SetStoredMissingSources`:
  a photo in the set shows the missing state and requests no render, so a cached thumbnail of a
  file that is gone does not show. A photo that leaves the set renders again.
- Remove from Library (the Delete item at the library root) is disabled when the selection holds
  a mounted photo, present or missing: a present photo stays, and a missing photo leaves through
  Remove Missing Photos. In a mounted folder, Remove from Album is disabled, because it would
  remove the home link.
- The editor open of a missing photo is not changed in this phase. The thumbnail state shows the
  missing file before the open, and the editor reports the render failure as it does for an
  ordinary missing file.

**Completion record.**

```text
Phase / date / status: MF6 / 2026-11-20 / implemented, pull request open, user check open
Source revision and branch: 2cd412e7c (MF8 part A, pull request #1004) + this phase, built with
  the Library part of MF8 in feature/mount-library-panel (pull request #1005)
Actual changed modules: MountSummary (photo ids), MountScanCoordinator (RunMountOperation,
  scan callbacks), FolderController (mount fields, mount states, commands),
  ThumbnailManager (stored missing state), CollectionsPanel.qml, LibraryWorkspace.qml,
  AppDialogs.qml, the three tile kinds, panel icons link and photo-off, zh_CN translations,
  tests
Implemented behavior: Mount Folder in the menu of an ordinary row and the library root; the
  link icon after the name of each mounted row; Offline in the count position of an offline
  mount; the mount root menu and the mounted folder menu of section 2.2; no drag, no dragged
  row, no file drop, no rename or delete for a mounted row; the grid of a mounted folder
  refuses a file drop; the missing tile icon; Delete at the library root and Remove from Album
  in a mounted folder are disabled for mounted photos
Explicitly unimplemented items: the editor open of a missing photo is unchanged (MF6
  implementation decisions)
Primary success call chain:
  CollectionsPanel "Mount Folder..." -> FolderDialog -> FolderController::MountFolder
    -> StartCreateMount -> MountScanCoordinator::RunMountOperation -> worker:
    MountService::CreateMount -> HandleOperationEnded -> ReloadMounts, ReloadFolderTree,
    SaveProjectState, RequestScan(kSynchronize) -> MountScan task -> scan end
    -> RefreshImageCounts -> worker: ReadFolderImageCounts + ListMounts -> ApplyMountSummaries
    (rows, ThumbnailManager::SetStoredMissingSources) -> RebuildFolderView
Primary failure and restore call chain:
  CreateMount throws MountOperationError(kNestedMount) -> done(error) -> ReportMountCommand
    -> status line "The folder is inside a mounted folder or holds one." -> no row
  scan offline -> on_scan_ended -> RefreshImageCounts -> mountOffline on each row of the mount
Build and test commands with exit codes: msvc_env.cmd --build --preset win_debug --target
  alcedo_main FolderControllerMountTest CollectionsPanelQmlTest MountScanCoordinatorTest
  MountServiceTest: 0; FolderControllerMountTest.exe: 0 (3 runs);
  CollectionsPanelQmlTest.exe (offscreen): 0 (2 runs)
Discovered / passed / failed / skipped counts: FolderControllerMountTest 6/6/0/0;
  CollectionsPanelQmlTest 18/18/0/0; MountScanCoordinatorTest 12/12/0/0
Manual verification: open (the user checks the panel in the application)
Performance or resource evidence, if required: not required
Evidence path: pull request #1005
Remaining defects or unavailable platforms: macOS and Linux builds of the panel not run
```

### MF7. Scan triggers

**Objective and deliverables.** A mount follows its directory with no user action. A reviewer can
add a file to the directory and see the photo.

**Inputs and prerequisites.** MF4, MF5. MF7 also holds the scan task of MF6 (the
`MountScan` task kind, the scan on a worker thread, and the Library refresh after each batch),
because the triggers need it before the panel exists. MF6 adds the panel commands on top of
`MountScanCoordinator::RequestScan` and `ReloadMounts`.

**Modules, files, and APIs.**

- `MountScanCoordinator` (library `MountScanCoordinator`, album backend): the triggers, the
  one-scan rule, the task, and the watch of each mount. `MountScanPort` keeps it testable without
  a project; `ProjectMountScanPort` calls `MountService::ScanMount` with the import service.
- `application_module_host.cpp`: start for an entered project that is not loading; stop through
  `ProjectLifecycleHooks::stop_mount_scans` before a project load and before a project close.
- `MountDirectoryWatcher`: on Windows one recursive change notification handle on each root
  directory with eject handling (P11); elsewhere `QFileSystemWatcher` on each directory.
- `MountScanResult::listed_directories`: the directories of the listing of the scan.

**Data rules and invariants.**

- Triggers: project open; application activation after 60 seconds or more in the background; a
  directory notification, 5 seconds after the last notification.
- One scan at a time for one mount. A request during a scan sets one flag. The coordinator starts
  one more scan when the current scan ends.
- The per-directory watcher holds at most 4 096 directories for all mounts. A mount that passes
  the limit, or whose registration fails, runs with the other two triggers. The Windows watcher
  holds one handle for each mount and has no directory limit.
- The task of a scan follows P12. A user cancel ends the scan and drops its "scan again" flag.
- A scan that ends with changes refreshes the Library in place; a scan that added or removed
  folders also reloads the folder tree. A scan with no change refreshes nothing.
- After each scan, the coordinator registers the directories of the new listing.
- The coordinator stops its timers and its watcher before the project closes, and it waits for
  the running scan to end through the stop request.

The "scan again" flag has this production interleaving: the user copies 500 files into the
directory. The watcher starts a scan after the first 200 files. The remaining 300 files arrive
during that scan and after its listing. The flag starts the second scan that finds them.

**Implementation steps.**

1. Add the coordinator with the mount list from `MountService::ListMounts`.
2. Add the three triggers.
3. Add the one-scan rule and the flag.
4. Register and remove watcher directories after each scan.
5. Add the tests with a controllable clock for the two time values.

**Primary success call chain.**

```text
directory change -> QFileSystemWatcher -> coordinator restarts the 5 s timer
timer -> coordinator: no scan runs for the mount -> scan task of MF6
```

**Primary failure and restore call chain.**

```text
addPath fails -> the mount is marked "not watched" in the coordinator
-> open, activation, and Synchronize still scan the mount
```

**Tests and evidence.**

- `MountScanCoordinatorTest.FileAddedToTheDirectoryAppearsInTheLibrary`.
- `MountScanCoordinatorTest.TenNotificationsInTwoSecondsStartOneScan`.
- `MountScanCoordinatorTest.RequestDuringAScanStartsExactlyOneMoreScan`.
- `MountScanCoordinatorTest.WatcherFailureKeepsSynchronizeWorking`.
- `MountScanCoordinatorTest.ProjectCloseEndsTheRunningScan`.

**Build and run commands.** Section 11.

**Exit criteria.**

- [x] The tests pass in the debug build.
- [x] A scan of an unchanged directory of 10 000 files completes in less than 2 seconds on a
      local SSD in a Release build. The completion record holds the measured time.

**Expected diff.** 700 to 1100 lines.

**Completion record.**

```text
Phase / date / status: MF7 / 2026-11-20 / implemented, pull requests open
Source revision and branch: 8cc0b80fb (MF5, pull request #1000) + this phase, in two pull
  requests by the 2000-line rule: feature/mount-directory-watcher (the watchers and
  MountScanResult::listed_directories) and feature/mount-scan-triggers (the coordinator)
Actual changed modules: mount service (listed_directories), MountDirectoryWatcher library (Qt
  and Windows watchers), MountScanCoordinator library (triggers, one-scan rule, task, watch),
  background task kind MountScan, application module host and project module (start, stop
  hooks, Library refresh), task island and task dialog kind label, zh_CN translations, tests
Implemented behavior: a kProjectOpen scan of each mount when an entered project is not
  loading; a kActivation scan of each mount after 60 s or more in the background; a
  kDirectoryChange scan 5 s after the last notification of a mount; RequestScan for
  Synchronize (MF6 calls it); one scan at a time with one "scan again" flag for the mount of
  the running scan; the task rules of P12; the watch of each mount after each scan that read
  the listing; the stop before a project load, before a project close, and at shutdown, with a
  wait for the running scan; IsIdle is false while a scan runs or waits
Explicitly unimplemented items: the panel commands that call RequestScan and ReloadMounts
  (MF6); the link icon of the mounted rows (MF6, section 2.2); the middle dot of the task
  island text (#1001)
Primary success call chain:
  file copied into a mounted directory -> Windows: FindFirstChangeNotification handle of the
    root -> QWinEventNotifier::activated -> WinMountDirectoryWatcher::HandleChange
    (FindNextChangeNotification) -> MountScanCoordinator::HandleDirectoryChanged -> clock timer
    restarts (5 s) -> RequestScan(kDirectoryChange) -> StartNextScan -> worker thread:
    ProjectMountScanPort::ScanMount -> MountService::ScanMount -> on_progress (queued)
    -> HandleScanProgress: RegisterTask(MountScan) when new files wait, on_batch_stored
    -> Library RequestLibraryContentRefresh, FolderController::RefreshImageCounts
    -> HandleScanEnded: FinishTask(Succeeded, "1 new"), watcher Watch(listed_directories),
    on_scan_ended -> folder tree reload when folders changed
Primary failure and restore call chain:
  Watch fails (registration error, eject, or the directory limit) -> MountState.watched false
    -> project open, activation, and RequestScan(kSynchronize) still scan the mount; the next
    scan that reads the listing tries the watch again
  project close -> ProjectModule::CloseProject -> StopMountScans -> MountScanCoordinator::Stop:
    timers and watches end, stop request, join of the worker (the current batch completes),
    the task ends Canceled -> PersistProjectForClose; a failed save -> resume_mount_scans
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build build\debug --target MountDirectoryWatcherTest
    MountScanCoordinatorTest MountServiceTest alcedo_main ApplicationModuleHostLifecycleTest
    ApplicationModuleHostShutdownTest ApplicationModuleHostIdleTest
    AlbumBackendBackgroundTaskTest ProjectModuleTest --parallel 8 -> 0
  ctest -R "MountDirectoryWatcherTest|MountScanCoordinatorTest|MountServiceTest" -> 0
  ctest -R "ApplicationModuleHost(Lifecycle|Shutdown|Idle)Test|AlbumBackendBackgroundTaskTest|
    ProjectModuleTest" -> 0
  MountDirectoryWatcherTest --gtest_repeat=20, MountScanCoordinatorTest --gtest_repeat=10 -> 0
Discovered / passed / failed / skipped counts: mounted_folder label 31 / 30 / 0 / 1 (the
  disabled timing test); caller suites 36 / 36 / 0 / 0
Manual verification: none in the application yet; MF6 adds the panel commands
Performance or resource evidence, if required: win_release_test build, MountServiceTest
  --gtest_also_run_disabled_tests --gtest_filter=*TenThousand*: an unchanged scan of 10 000
  files took 91 ms and 95 ms (two runs; WD_BLACK SN770 NVMe SSD, system temporary directory)
Evidence path: this record
Remaining defects or unavailable platforms: Linux and macOS were not built or run. The Qt
  watcher passes g++ 16 -std=c++20 -Wall -Wextra -fsyntax-only with Qt 6 in WSL alcedo-arch
  with no diagnostic; the coordinator was not checked there (its includes need OpenCV, which
  that distribution lacks). The eject of a removable volume with a watched mount was not
  tested (the Windows watcher closes its handles on DBT_DEVICEQUERYREMOVE)
```

Measured fact for P11 (2026-11-20, Windows 11, NTFS): with a `FindFirstChangeNotification`
handle open in `Root\Day1`, the rename of `Root` fails with access denied. With one recursive
handle on `Root`, the rename of `Root\A` (which has a subdirectory) and of `Root` itself
succeed; the rename of the parent of `Root` fails.

**Change after the phase (#1014, 2026-11-20).** The stop of a scan replaces the rules of MF3,
MF4, and MF7 that are named below. Section 2.6 holds the current rules.

```text
Problem: the first scan read the fingerprint of each file before its first import batch, with
  no read of the stop request; MountScanCoordinator::Stop joined the scan worker on the UI
  thread at project close, project load, and exit
MF3: BuildMountScanChangeList has a stop request parameter, read before each fingerprint read; a
  stop gives an empty change list. A new path is read only when an ordinary file adopts it or a
  lost record of the current fingerprint version has its byte size; each other new path goes to
  MountScanChangeList::unread_new_files without a read
MF4: MountScanRun reads the fingerprint of the unread new files in the batch that imports them
  and reads the stop request before each of these reads, which is before the first write of the
  batch. A batch that started its import completes. The progress shows the total of the new
  files before the first read
MF7: MountScanCoordinator::BeginStop requests the stop and returns; the worker stays in
  stopped_worker_ until its queued end (HandleStoppedWorkerEnded -> StoppedWorkerEnded), and no
  scan or operation starts before that. Stop = BeginStop + join (destructor, last step of exit)
Call chains:
  project close -> ApplicationCloseCoordinator::Finish -> BeginStop -> HasStoppedWorker: wait
    -> StoppedWorkerEnded (queued) -> EvaluateEditorClose -> Finish -> ProjectModule::
    CloseProject -> StopMountScans (no worker) -> PersistProjectForClose; an abort, a cancel, or
    a refused close -> ProjectModule::ResumeMountScans
  project load -> ProjectHandler::StartProjectLoad -> ProjectModule::BeginStopMountScans ->
    loader thread: wait for the future of the worker -> save of the old project -> load
  exit -> ApplicationModuleHost::ShutdownModules -> BeginStop -> event wait until IsIdle (15 s)
    -> Stop (join of a worker that is still in its batch) -> PersistProjectForClose
Tests: MountScanChangeListTest 25/25 (FirstScanReadsNoFile, NewPathIsReadOnlyWhenALostRecord
  HasItsByteSize, StopRequestEndsTheFingerprintReadsWithAnEmptyChangeList, StopRequestEndsThe
  ReadsOfChangedFiles); MountServiceTest 25/25 (StoppedFirstScanReadsOnlyTheFilesOfItsStored
  Batch); MountScanCoordinatorTest 15/15, 20 repeats (BeginStopReturnsWhileTheWorkerCompletes
  ItsBatch, StartAfterBeginStopWaitsForTheStoppedWorker, BeginStopLetsTheRunningOperation
  Complete); ApplicationCloseCoordinatorTest 6/6 (ProjectCloseWaitsForTheStoppedScanWorker);
  callers: FolderControllerMountTest 6/6, ApplicationModuleHostLifecycleTest 2/2,
  ApplicationModuleHostShutdownTest 11/11, ApplicationModuleHostIdleTest 1/1,
  ProjectModuleTest 4/4. Windows debug build; alcedo_main links
Not covered: a slow network share was not measured; Linux and macOS were not built
```

### MF8. Missing photos, root relocation, and automation commands

**Objective and deliverables.** The user can repair an offline mount and a missing photo, and can
remove missing photos. Automation can mount and synchronize.

**Inputs and prerequisites.** MF6.

**Modules, files, and APIs.**

- `MountService::RelocateRoot`, `LocateMissingFile`, `RemoveMissingPhotos`, and the volume search
  on open.
- `FolderController` and `CollectionsPanel.qml`: Locate Folder, Remove Missing Photos with a
  count confirmation.
- Thumbnail menu: Locate.
- `library_commands.cpp`: `library.mount`, `library.mounts`, `library.sync`, `library.unmount`.
- Documents: a user document for mounted folders under `docs/`, and the `alcedo-cli` documents in
  the `alcedo-skills` repository.

**Data rules and invariants.**

- `RelocateRoot` changes `root_path`, the volume identity, and the prefix of `image_path` for
  each file of the mount in one transaction. It applies D12 to the new directory. The next scan
  compares the new location.
- Volume search on open: when `root_path` is unreachable and the volume identity has a value, the
  service looks for a mounted volume with that identity and tests the same path below its mount
  point. A reachable result runs `RelocateRoot`.
- `LocateMissingFile` accepts a file inside the root directory that has no present record. The
  element takes the path, the new home link, and the source values of the file. The fingerprint
  can be different from the record.
- `RemoveMissingPhotos` removes each missing element of the mount with the current delete
  operation of the library: element, Image row, pipelines, edit history, and `SourceFile` row. It
  removes no file from the disk, because no file exists.
- The automation commands call the same `MountService` operations as the panel. `library.sync`
  with no mount id scans each mount.

**Implementation steps.**

1. Add `RelocateRoot` and the volume search.
2. Add `LocateMissingFile`.
3. Add `RemoveMissingPhotos` on the delete operation of `AlbumBrowseService` and
   `PipelineService`.
4. Add the menu items and the confirmation dialog with the count.
5. Add the four automation commands and their parameter descriptions.
6. Write the documents.

**Primary success call chain.**

```text
row menu "Locate Folder" -> FolderController -> MountService::RelocateRoot
  -> one transaction (MountRoot, Image.image_path prefix) -> scan request
row menu "Remove Missing Photos" -> confirm(count) -> MountService::RemoveMissingPhotos
```

**Primary failure and restore call chain.**

```text
RelocateRoot(new directory contains another mount) -> NestedMount -> no change
RemoveMissingPhotos storage failure -> transaction rolls back -> StorageFailure -> counts unchanged
```

**Tests and evidence.**

- `MountServiceTest.RelocatedRootKeepsEachElementId`.
- `MountServiceTest.VolumeWithRecordedIdentityIsFoundOnOpen`: with an injected volume list.
- `MountServiceTest.LocatedMissingPhotoKeepsEditHistory`.
- `MountServiceTest.FileOutsideTheMountIsRefusedForLocate`.
- `MountServiceTest.RemoveMissingPhotosRemovesExactlyTheMissingPhotos`.
- `AutomationLibraryCommandsTest.MountSyncAndUnmountReturnTheServiceResults`.

**Build and run commands.** Section 11. The CLI tests use the Release build.

**Exit criteria.**

- [ ] The tests pass.
- [ ] The user confirms Locate Folder and Remove Missing Photos in the application.
- [ ] The documents describe each command and each state.

**Expected diff.** 1300 to 1800 lines.

**Implementation decisions (2026-11-20).**

- Write order of `RelocateRoot`: the `MountRoot` row, then `Image.image_path` through
  `ImagePoolService`. The two owners have no common transaction. When the second write fails, the
  next scan finds each file below the new root by its fingerprint (a move), so the relocation
  completes. The opposite order would leave the mount at an unreachable root with the files below
  the new one.
- The volume search runs in `ScanMount` when the request sets `find_moved_volume`. The scan
  coordinator sets it for the scan of the project open only, as the plan states. The search tries
  each end of the old root path below each mount point of the recorded volume, the longest end
  first, and accepts a directory whose volume identity equals the record. The volume mount point
  itself is a candidate only for a mount of a whole volume. `ListVolumeMountPoints` lists the
  volumes; a test injects the list through the `MountService` constructor.
- The Library commands: Locate Folder and Remove Missing Photos are items of the mount root
  menu (section 2.2); Remove Missing Photos asks for confirmation in the delete dialog with the
  count of the mount. Locate Photo... is an item of the image menu for a missing mounted photo; its
  file dialog opens in the last directory of the photo (`Image.image_path`), else in the mount
  root. A refusal shows its text in the status line.
- The automation commands wait for their operation through the FolderController command
  entries; `library.mount` and `library.sync` also wait for the scan, and `timeout_ms` bounds the
  wait. A refusal of `MountService` answers -32002 with `data.error`, the snake case name of the
  `MountError` code.
- `LocateMissingFile` applies the listing rules to the one file (`ReadMountDirectoryFile`): a file
  that a scan would not list is refused (`kFileIsNotListed`), so the next scan cannot report the
  located photo missing again. A directory that the last scan did not see gets its folders in the
  same tree write.
- `RemoveMissingPhotos` takes the pipeline and history removal as a callback
  (`PipelineMgmtService::DeletePipelines` in production): `MountService` cannot link
  `PipelineMgmtService`, which depends on `ProjectService`. The sleeve removal is
  `SleeveServiceImpl::DeleteMissingMountedFiles`, which checks that each id is a missing photo of
  the mount; the MF5 rule refuses the same files on the ordinary delete.

**Completion record.**

```text
Phase / date / status: MF8 / 2026-11-20 / implemented, pull requests open, user check open
Source revision and branch: a98d64098 (MF7, pull request #1003) + this phase, in three parts:
  feature/mount-missing-photos-service (#1004, service), feature/mount-library-panel (#1005,
  Library with MF6), feature/mount-automation-commands (#1006, automation commands and
  documents);
  CLI documents in zidage/alcedo-skills#28
Actual changed modules: volume identity (ListVolumeMountPoints), mount directory listing
  (ReadMountDirectoryFile), MountService (RelocateRoot, volume search, LocateMissingFile,
  RemoveMissingPhotos), SleeveServiceImpl (DeleteMissingMountedFiles), scan coordinator
  (find_moved_volume on open, relocated root watch), FolderController and QML (part B),
  automation mount commands, docs/mounted_folders.md, the alcedo-skills library reference
Implemented behavior: Locate Folder binds a mount to the new location of its root and keeps
  each element; the project open scan finds a volume with the recorded identity under a new
  mount point; Locate Photo takes a file below the mount root as the source of a missing photo
  and keeps its edit history; a file outside the mount, a file that a scan does not list, and
  the file of another photo are refused; Remove Missing Photos removes exactly the missing
  photos of one mount after a count confirmation; library.mounts, library.mount, library.sync,
  and library.unmount run the FolderController commands of the panel and answer with the
  results of the service operations
Explicitly unimplemented items: none of the plan
Primary success call chain:
  mount root menu "Locate Folder..." -> FolderDialog -> FolderController::LocateMountFolder
    -> StartRelocateMountRoot -> RunMountOperation -> worker: MountService::RelocateRoot
    (MountRoot row, then Image.image_path below the new root) -> ReloadMounts,
    RequestScan(kSynchronize) -> scan compares the new location
  mount root menu "Remove Missing Photos" -> DeleteConfirmDialog (count) ->
    FolderController::RemoveMissingPhotos -> RunMountOperation -> worker:
    MountService::RemoveMissingPhotos -> SleeveServiceImpl::DeleteMissingMountedFiles,
    PipelineMgmtService::DeletePipelines, ImagePoolService::RemoveBatch, ExportService
    RemoveExportTasks -> thumbnail state, selection prune, Library refresh, counts
  alcedo-cli library.sync -> FolderController::StartSynchronize(callback) -> RequestScan
    -> scan end -> callback -> answer with the scan counts
Primary failure and restore call chain:
  RelocateRoot(directory inside another mount) -> kNestedMount -> status line, no change
  LocateMissingFile(file outside the root) -> kFileOutsideMount -> status line, photo missing
  library.mount(nested path) -> -32002 with data.error nested_mount
Build and test commands with exit codes: msvc_env.cmd --build --preset win_debug (targets
  SourceFileUtilsTest MountServiceTest MountScanCoordinatorTest FolderControllerMountTest
  AutomationLibraryCommandsTest alcedo_main): 0; each test binary: 0
Discovered / passed / failed / skipped counts: MountServiceTest 24/24/0/0 (1 disabled timing
  test); SourceFileUtilsTest 21/20/0/1 (symbolic link privilege);
  AutomationLibraryCommandsTest 16/16/0/0; FolderControllerMountTest 6/6/0/0
Manual verification: open (the user checks Locate Folder and Remove Missing Photos)
Performance or resource evidence, if required: not required
Evidence path: pull requests #1004, #1005, and #1006; alcedo-skills#28
Remaining defects or unavailable platforms: ListVolumeMountPoints was compiled and run on
  macOS and Linux (WSL) as single files; the full macOS and Linux builds were not run
```

**Change after the phase (#1020, #1021, 2026-11-21).** The volume check of a scan and the volume
events of the watcher replace the rules of MF4, MF7, and P11 that are named below.

```text
Problem (#1021): a scan treated each root directory that it could list as online. On Linux and
  macOS the empty mount point of an unmounted volume stays, so the scan made each photo of the
  mount missing, and Remove Missing Photos then removed them with their edit history
Problem (#1020): the Windows watcher closed its handles on an eject and reported nothing, so the
  mount stayed online until the next scan, and a canceled eject left the mount without a watch.
  A connected drive left its mount offline until an activation scan or Synchronize
MF4: MountService::ScanMount reads the volume identity of the root before the listing
  (ListRootOnRecordedVolume). A mount with a recorded identity whose root has another identity,
  or none, is offline (D5) and gets the moved volume search when the request asks for it. A
  mount without a recorded identity has no check. MountService takes a VolumeIdentityReader
  (ReadVolumeIdentity in production) next to the VolumeMountPointList
MF7: MountDirectoryWatcher::SetVolumeCallbacks reports the removal of the volume of a watched
  mount (the watch has ended) and the arrival of a volume. MountScanCoordinator requests a
  kVolumeChange scan of the mount at once for a removal, and of each offline mount after
  kMountScanVolumeArrivalDelay (2 s) for an arrival. A kVolumeChange scan searches for a moved
  volume, as the scan of the project open does
P11: the Windows watcher closes the handles on DBT_DEVICEQUERYREMOVE and DBT_DEVICEREMOVEPENDING
  and keeps the device notification; DBT_DEVICEREMOVECOMPLETE and DBT_DEVICEQUERYREMOVEFAILED end
  the watch and report the removal. The arrival is the device interface notification of
  GUID_DEVINTERFACE_VOLUME. The Qt watcher reads the mount table (ListMountPointPaths, no file
  system access) every kMountTablePollInterval (2 s): a new mount point is an arrival, and a
  mount point that is gone removes each watched mount at or below it
Call chains:
  eject -> WM_DEVICECHANGE DBT_DEVICEQUERYREMOVE -> CloseHandles -> DBT_DEVICEREMOVECOMPLETE ->
    Unwatch -> (queued) MountScanCoordinator::HandleVolumeRemoved -> RequestScan(kVolumeChange)
    -> ScanMount: kOffline -> task "Offline", FolderController::RefreshImageCounts
  canceled eject -> DBT_DEVICEQUERYREMOVEFAILED -> same chain -> kComplete -> Watch again
  drive connected -> DBT_DEVICEARRIVAL (DBT_DEVTYP_DEVICEINTERFACE) -> (queued)
    HandleVolumeArrived -> timer of each offline mount (2 s) -> RequestScan(kVolumeChange) ->
    ScanMount: online, missing photos return
  Linux unmount -> QtMountDirectoryWatcher::PollMountTable -> mount point gone -> Unwatch ->
    HandleVolumeRemoved -> ScanMount: the empty mount point has another identity -> kOffline
Tests: MountServiceTest 27/27 (RootOnAnotherVolumeSetsOfflineAndChangesNoRecord,
  RootOnAnotherVolumeSearchesForTheRecordedVolume); MountScanCoordinatorTest 27/27, 10 repeats
  (VolumeRemovalScansTheMountAtOnceAndShowsItOffline, CanceledVolumeRemovalWatchesTheMountAgain,
  VolumeArrivalScansAnOfflineMountAfterTheDelay); MountDirectoryWatcherTest 5/5, 20 repeats
  (QtWatcherReportsTheVolumeEventsOfTheMountTable); SourceFileUtilsTest 21/22, 1 skipped
  (MountTableHoldsAMountPointOfADirectory passes; the symbolic link test needs a privilege);
  ApplicationCloseCoordinatorTest 6/6. Windows debug build
Linux: volume_identity_linux.cpp and mount_directory_watcher_qt.cpp pass g++ 16 -std=c++20 -Wall
  -Wextra -fsyntax-only with Qt 6 in WSL alcedo-arch with no diagnostic
Not covered: a real eject, a canceled eject, and a connected drive were not run (the device
  notifications come from the system); macOS ListMountPointPaths (getfsstat) was not compiled
```

---

### Change record: mounts at the library root, volume names, root-only photos (#1076)

Date: 2026-11-23. Issue #1076; closes #999. Parent of the sidebar work #1077 and #1078.

**Product rules.**

- The parent of a mount is always the library root. `MountService::CreateMount(directory)`,
  `FolderController::MountFolder(const QUrl&)`, `FolderController::StartCreateMount(directory,
  ...)`, and `SleeveServiceImpl::CreateMountRootFolder(name, mount_id)` have no parent
  parameter. `library.mount` has no `parent_id`. An ordinary collection cannot hold a mount
  root, so the delete of #999 cannot occur.
- `FolderImageCounts::library_root_photo_count_` is the count of the root row;
  `project_photo_count_` stays the All Photos total.
- `AlbumScope` (`type/album_scope.hpp`) is the scope of the library reads: `AlbumScope(0)` is
  All Photos as before, `AlbumScope(id)` is a folder, and `AlbumScope::LibraryRoot()` is the
  photos that no folder other than the root holds. Each photo has a link in the root, so "home
  folder is the root" is "no other link". `ReadAlbumQuery`, `ReadAlbumFilePosition`,
  `ReadAlbumFileIds`, `BuildScopedFileQuery`, and `BuildSemanticRelevanceSelect` take it; a
  folder id converts to it, so the other callers keep their folder id.
- `MountRootRecord::volume_name` and the column `MountRoot.volume_name`, added through
  `mount_schema_query` (`ALTER TABLE ... ADD COLUMN IF NOT EXISTS`), so a project file that
  exists opens with an empty name. `MountSummary::volume_mount_point` is the mount point of the
  last scan of this session; it is empty while the mount is not online.
- `ReadVolumeLabel` (`utils/file/volume_identity.hpp`): `GetVolumeInformationW` and
  `GetVolumePathNameW` on Windows, `NSURLVolumeNameKey` and `statfs` on macOS, the
  `/dev/disk/by-label` entry (or the last component of the mount point) and the longest
  `/proc/self/mounts` entry of the device on Linux. `MountService` takes it as the injectable
  `VolumeLabelReader`.
- Arrival of a volume with no mount: `SetVolumeCallbacks` has a third callback,
  `MountPointArrivedCallback`, that reports each new mount point with `ArrivedVolume` (mount
  point, name, identity, `DCIM` flag). The Windows watcher compares the drive list at each
  device interface notification and every `kMountTablePollInterval`, because the drive letter
  can come after the notification. `MountScanCoordinator` emits `VolumeWithoutMountArrived`
  when no mount has the identity and no mount root is at or below the mount point.

**Decisions made in the work.**

- The issue text places the "no mount" check in the watcher. The watcher knows only the watched
  mounts, and the mounts of an arriving volume are offline and unwatched, so the check is in
  `MountScanCoordinator`, which holds the stored root and identity of each mount. The watcher
  test checks the report of the arrival with its name and `DCIM` flag; the coordinator test
  checks the decision.
- The mount point is not stored. The issue asks for the stored name only; the mount point of an
  online mount comes from the scan of this session, and the Disk section groups an offline mount
  by its identity.
- A Windows volume without a label has an empty name; the Disk section shows the mount point
  then. The Linux reader uses the last component of the mount point, as the issue states.
- A scan writes the name only when it differs from the stored one. A failed name write ends the
  scan with `kStorageFailure` and `failed_batch` 0, before batch 1.
- The row menu item **Mount Folder Here...** shows only on the library root row. The sidebar
  sections of #1078 replace the menus.

**Call chains.**

```text
Mount: Import menu or root row menu -> FolderDialog -> FolderController::MountFolder(url)
  -> StartCreateMount -> MountScanCoordinator::RunMountOperation (worker)
  -> MountService::CreateMount: canonical, nested check, ReadVolumeIdentity, ReadVolumeLabel
  -> SleeveServiceImpl::CreateMountRootFolder(name, id) -> alcedo::CreateMountRootFolder
     (kLibraryRootFolderId) -> MountStore::InsertMountRoot (volume_name)
Scan that reaches the volume: MountService::ScanMount -> ListRootOnRecordedVolume
  -> RecordVolumeReached: ReadVolumeLabel -> SetOnline(mount point)
     -> MountStore::UpdateMountVolumeName when the name changed -> MountScanRun::Execute
Counts: FolderController::RefreshImageCounts (worker) -> AlbumBrowseService::ReadFolderImageCounts
  -> ElementStore::ReadFolderImageCounts: subtree rows, the root row (id 0), the project row
Root row photos: AlbumBrowseService::ReadAlbumQuery(AlbumScope::LibraryRoot(), ...)
  -> ElementStore::ReadAlbumQuery -> BuildScopedFileQuery: fc.folder_id = 0 AND no other link
New mount point: QtMountDirectoryWatcher::PollMountTable or WinMountDirectoryWatcher
  (timer, DBT_DEVICEARRIVAL) -> MountTableComparison::Poll -> ReadArrivedVolume
  -> MountScanCoordinator::HandleMountPointArrived -> VolumeWithoutMountArrived
```

**Tests.** MountServiceTest (NewMountIsAChildOfTheLibraryRoot,
VolumeNameIsStoredAndStaysWhileTheVolumeIsOffline), MountStoreTest
(MountVolumeNameUpdateSurvivesProjectReopen, MountRootTableWithoutTheVolumeNameColumnGainsIt),
AlbumQueryTest (LibraryRootCountLeavesOutTheMountTrees,
LibraryRootScopeListsOnlyThePhotosThatNoOtherFolderHolds), MountDirectoryWatcherTest
(NewMountPointIsReportedWithItsNameAndTheDcimFlag, ArrivedVolumeHasTheDcimFlagOfItsRoot),
MountScanCoordinatorTest (VolumeWithoutAMountIsReportedAndTheVolumeOfAMountIsNot),
AutomationLibraryCommandsTest (MountCommandSchemaHasNoParentId).

### Change record: the arrival card follows its volume (#1087)

Status: done on 2026-11-24.

- Arrival decision: a mount hides the card of an arriving volume when the two identities are
  equal. A mount root at or below the mount point hides it only when the mount or the volume has
  an empty identity. A card that replaces another volume at the same mount point therefore gets
  its own card.
- Eject: `SetVolumeCallbacks` has a fourth callback, `MountPointRemovedCallback`, for each mount
  point that is gone from the mount table. The Windows watcher also reads the drive list at the
  device interface removal (`DBT_DEVICEREMOVECOMPLETE`). `MountScanCoordinator` emits
  `MountPointRemoved`, and `FolderController` closes the card of that mount point.
- Card action: `VolumeWithoutMountArrived` carries the identity, and `FolderController` keeps it
  with the mount point. **Import...** and **Mount...** call `FolderController::TakeArrivedVolume`,
  which closes the card and asks `MountScanCoordinator::IsArrivedVolumeAt`: the mount point is a
  directory and its identity equals the card's. On a mismatch the status line shows
  "%1 is no longer connected." and the action stops. This also covers a swap between two reads of
  the mount table, where the same mount point is in both reads and neither change is reported.

```text
Eject: WinMountDirectoryWatcher (DBT_DEVICEREMOVECOMPLETE, timer) or
  QtMountDirectoryWatcher::PollMountTable -> MountTableComparison::Poll (removed)
  -> MountScanCoordinator::HandleMountPointRemoved -> MountPointRemoved
  -> FolderController::HandleMountPointRemoved -> DismissArrivedVolume
Card button: CollectionsPanel.openVolumeCard -> FolderController::TakeArrivedVolume
  -> MountScanCoordinator::IsArrivedVolumeAt (is_directory, ReadVolumeIdentity)
  -> importFolderRequested -> ImportFolderFlow.begin
```

**Tests.** MountScanCoordinatorTest (ReplacementVolumeAtTheSameMountPointGetsItsOwnCard),
MountDirectoryWatcherTest (QtWatcherReportsTheVolumeEventsOfTheMountTable),
CollectionsPanelQmlTest (VolumeCardOpensTheImportFolderSteps). A physical card swap is
untested.

## 9. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Mount a directory of 3 levels | The collection tree equals the directory tree | MF4, MF6 |
| Rename a file in the file manager | Same element, new name, edits kept | MF3, MF4 |
| Move a file to another directory of the mount | Same element, new folder, edits kept | MF3, MF4 |
| Rename a directory | Each photo keeps its element | MF3, MF4 |
| Copy a file inside the mount | Second photo with no edits | MF3, MF4 |
| Delete a file in the file manager | Photo is missing, data kept | MF4, MF6 |
| Put the file back | Photo is present | MF4 |
| Unplug the drive | Mount is offline, no photo becomes missing | MF4, MF6 |
| Drive gets a new letter | Mount finds the volume on open | MF8 |
| Drop a file on a mounted row | Refused, no import | MF5, MF6 |
| New collection below a mount | Refused | MF5, MF6 |
| Link a mounted photo into an ordinary collection | Accepted | MF5 |
| Mount a directory whose photos are in the project | Photos are adopted, no second entry | MF4 |
| Unmount | Photos stay as ordinary photos | MF4 |
| Reopen the project | Mount, states, and counts are equal | MF1, MF4 |
| Open a project of version 0.10.0 or 0.11.0 | Opens, root ids equal | MF1 |
| Any scan | The user directory is unchanged | MF4 |

---

## 10. Repository rules that apply

- SQL goes through duckorm inside storage. A store keeps SQL text only in storage.
- Every image has an edit history root. A missing `ImageEditState` is a defect.
- A change of the pipeline document JSON changes the root id. This plan adds no field to the
  pipeline document.
- No duplicate data model (P3). No new consistency mechanism without a production interleaving
  (MF7 gives one for the flag).
- Tests state the behavior that they verify. No stored expected-pixel data.
- An agent runs targeted tests only. The user requests a full suite run.
- The terminology rules of `AGENTS.md` apply to identifiers, tests, and text.
- A problem found during the work becomes a GitHub issue.

---

## 11. Build and evidence rules

- Configure: `cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"`
  in PowerShell.
- Build: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4`.
- Tests: `ctest -R '<pattern>'` or the test binary. Confirm that CTest discovered the tests
  before a pass is reported.
- Register each new test with `alcedo_register_test_target`.
- QML tests run offscreen with `QT_FORCE_STDERR_LOGGING=1`.
- The time measurement of MF7 uses the Release build.
- Record a skipped test as skipped with its reason. Record an unavailable platform as unavailable.

---

## 12. Risks and stop conditions

| Risk | Detection signal | Response |
| --- | --- | --- |
| Another application rewrites the header of a DNG, JPEG, or TIFF and the user also moves the file | A photo becomes missing and a new photo appears | Locate (MF8) joins them. Record the case in the user document. |
| A network volume reports a reachable root with an incomplete listing | Many photos become missing in one scan | No data is lost (D4). The next complete scan returns them. Record the volume type in an issue. |
| The modification time has a coarse resolution (FAT) | A changed file with equal size and time is "unchanged" | Accepted limit. Record it in the user document. |
| The element mapper change breaks a full `SELECT` in another query | A storage test fails with a column count error | Fix each projection in MF1. |
| `FileSystem` lacks an operation for the apply step | MF4 step 3 cannot keep the home link rule | Stop. Add the operation in a separate pull request. Update this plan. |
| A phase passes 2000 lines | The diff count during the work | Stop. Split the phase. Update this plan. |

Stop implementation also when the source audit of section 4 no longer matches the branch, or
when an operation needs a write to the user directory.

---

## 13. Completion record template

```text
Phase / date / status:
Source revision and branch:
Actual changed modules:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual verification:
Performance or resource evidence, if required:
Evidence path:
Remaining defects or unavailable platforms:
```

---

## 14. Terms and parameters

| Term or parameter | Definition |
| --- | --- |
| Mount | The binding of one collection to one disk directory. |
| Mount root | The collection that is bound to the root directory. |
| Root directory | The disk directory of a mount. |
| Mounted folder | A folder element with a `mount_id`. It represents one directory. |
| Mounted file, mounted photo | A file element with a `mount_id`. It represents one file of the directory. |
| Ordinary element | An element with a null `mount_id`. |
| Home link | The one link of a mounted file to a mounted folder of the same mount. |
| Relative path | The element names from the mount root to the element, joined with `/`. |
| Source record | The `SourceFile` row of a file element. |
| Fingerprint | XXH3-128 of the byte size, the first 128 KiB, and the last 128 KiB of a file. |
| Fingerprint version | 1 for the definition above. |
| Fingerprint block size | 128 KiB. |
| Present, missing | The two states of a source record. |
| Offline | The state of a mount whose root directory is unreachable. |
| Listing | The directories and supported image files below the root directory at one time. |
| Scan | Listing, comparison, and apply for one mount. |
| Change list | The result of the comparison. |
| Adoption | An ordinary photo of the project becomes the mounted element of its path. |
| New file batch size | 500 files. |
| Watcher delay | 5 seconds after the last notification. |
| Activation interval | 60 seconds in the background before an activation starts a scan. |
| Watcher directory limit | 4 096 directories for all mounts. |
| Project file version | 0.12.0 after MF1. Minimum 0.10.0. |
