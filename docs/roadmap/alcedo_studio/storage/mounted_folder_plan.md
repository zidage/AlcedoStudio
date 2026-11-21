# Mounted Folder Plan

Date: 2026-11-19

Status: **planned**. No phase has implementation evidence.

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

1. The command opens the system folder dialog.
2. The application creates a collection with the name of the directory, below the row where the
   user opened the menu. An ordinary row and the library root are valid parents.
3. The first scan starts as a background task with the kind `MountScan`.
4. Photos appear in the collection while the scan imports them.

The command reports a named error, with no change to the project, when:

- the directory is inside a mounted directory, or contains one;
- the parent row is mounted;
- the directory does not exist.

### 2.2 Collections panel rows

```text
v  Trips                      1.2K      ordinary collection
v  [=] 2026 Japan              842      mount root, folder mark
      [=] Day 1                310      mounted folder
      [=] Day 2                532      mounted folder
>  [=] Archive             Offline      offline mount, dimmed
```

- A mounted row shows the folder mark before the name. An ordinary row has no mark.
- The count of a mounted folder counts present and missing photos.
- An offline mount shows a dimmed name and the label **Offline** in the count position.
- The row menu of a mount root has: **Synchronize**, **Show in File Manager**, **Unmount**, and,
  for an offline mount, **Locate Folder**. When the mount has missing photos, it also has
  **Remove Missing Photos**.
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
- A missing photo shows the current tile state "Source file was moved or deleted". Its ratings
  and labels stay editable. The editor does not open it.
- The menu of a missing photo has **Locate**.

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
| MF1 #983 | Mount record, element mount id, source file record | Storage, sleeve element | none | 900 to 1300 | planned |
| MF2 #984 | Fingerprint, volume identity, directory listing | Source file utilities | none | 800 to 1200 | planned |
| MF3 #985 | Scan change list | Scan change list | MF1, MF2 | 900 to 1400 | planned |
| MF4 #986 | Mount service: create, scan, adopt, apply, unmount | Mount service, sleeve service | MF3 | 1400 to 1900 | planned |
| MF5 #987 | Read-only structure | Sleeve service, import, automation | MF1 | 600 to 1000 | planned |
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

- [ ] The tests above pass in the debug build.
- [ ] A project of version 0.10.0 and one of 0.11.0 open with no error.
- [ ] No SQL text exists outside the storage module.

**Expected diff.** 900 to 1300 lines.

**Completion record.** Section 13 template. Empty.

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

- [ ] The tests pass on Windows in the debug build.
- [ ] The macOS and Linux sources compile in CI.
- [ ] No function of this phase opens a file for writing.

**Expected diff.** 800 to 1200 lines.

**Completion record.** Section 13 template. Empty.

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

- [ ] Each test compares the complete returned change list with the expected list.
- [ ] The function has no include of a storage, sleeve service, or Qt header.

**Expected diff.** 900 to 1400 lines.

**Completion record.** Section 13 template. Empty.

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

- [ ] The tests pass in the debug build.
- [ ] The second scan of an unchanged directory of 2 000 files calls the fingerprint function 0
      times. The test records the count.
- [ ] No code path of the service opens a user file for writing.

**Expected diff.** 1400 to 1900 lines. If the sleeve operations of step 3 pass 400 lines, move
them to a separate pull request before the service.

**Completion record.** Section 13 template. Empty.

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

- [ ] Each operation in the list has one test.
- [ ] The check exists in one function.

**Expected diff.** 600 to 1000 lines.

**Completion record.** Section 13 template. Empty.

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
- `BackgroundTaskKind::MountScan` and its text.
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
2. Add the background task kind and register the scan as a task with a stop callback.
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

**Completion record.** Section 13 template. Empty.

### MF7. Scan triggers

**Objective and deliverables.** A mount follows its directory with no user action. A reviewer can
add a file to the directory and see the photo.

**Inputs and prerequisites.** MF6.

**Modules, files, and APIs.**

- Proposed `MountScanCoordinator` in the album backend.
- `application_module_host.cpp`: start after `project_opened`, stop before project close.
- `QFileSystemWatcher` on each directory of each online mount.

**Data rules and invariants.**

- Triggers: project open; application activation after 60 seconds or more in the background; a
  directory notification, 5 seconds after the last notification.
- One scan at a time for one mount. A request during a scan sets one flag. The coordinator starts
  one more scan when the current scan ends.
- The watcher holds at most 4 096 directories for all mounts. A mount that passes the limit, or
  whose registration fails, runs with the other two triggers.
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

- [ ] The tests pass in the debug build.
- [ ] A scan of an unchanged directory of 10 000 files completes in less than 2 seconds on a
      local SSD in a Release build. The completion record holds the measured time.

**Expected diff.** 700 to 1100 lines.

**Completion record.** Section 13 template. Empty.

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

**Completion record.** Section 13 template. Empty.

---

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
