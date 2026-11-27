# Sleeve Node Cache Plan

Date: 2026-11-26

Status: **in progress**. SN1, SN2, SN3, and SN4 are complete (2026-11-26). SN5 to SN8 are
planned.

Parent: [Roadmap](../../README.md).

Direct prerequisites: the storage boundary of the
[Storage Query Boundary Plan](storage_query_boundary_plan.md) and the mount records of the
[Mounted Folder Plan](mounted_folder_plan.md). This plan changes the cache rules of the sleeve file
system inside that boundary. It keeps both plans.

Tracking issue: [#1152](https://github.com/zidage/AlcedoStudio/issues/1152). Cause and required
behavior: [#1148](https://github.com/zidage/AlcedoStudio/issues/1148).

Source audit revision: `f4d364746` on `main`.

Unless a path starts with `alcedo_studio/` or `docs/`, it is relative to `alcedo_studio/src/`.
Test paths are relative to `alcedo_studio/`.

Section 10 defines the terms and the parameters that this plan uses.

---

## 1. Decision record

### 1.1 User goal

A project with more than 100,000 photos and a deep folder tree must open, show its sidebar tree,
and accept writes with a cost that depends on the folders and on the changed elements. Today the
cost depends on every photo of the project.

The sleeve file system keeps its design: the database is the persistent store and the query
engine, and `FileSystem` holds the in-memory nodes with their structure, path resolution, and
copy-on-write rules. This plan changes the cache rules only.

### 1.2 Approved decisions

| Id | Decision |
| --- | --- |
| D1 | `FileSystem`, `PathResolver`, `NodeStorageHandler`, the copy-on-write rules, and the `SleeveService` API stay. |
| D2 | A link stays the membership model: one file has one element id, and each folder that holds the file has one `FolderContent` row. The row of the library root (folder 0) for each photo stays. Old projects open with no data change. |
| D3 | The rule that a file name is unique in the library root can change. The root was the link target of each photo only to make the count of all photos simple. |
| D4 | Node eviction (issue step 4) is decided after a measurement. The measurement is a phase of this plan. |
| D5 | The sidebar tree read can move off the GUI thread. The risk of defects must stay low. |
| D6 | The node container can be a page table indexed by element id. |
| D7 | New code goes into new files. `sleeve/sleeve_filesystem.cpp` and `storage/store/sleeve/element_store.cpp` get smaller or keep their size. |
| D8 | A file name is a label. Only folder names are unique among the child folders of one parent. A file that holds a name with `@` today keeps that name. |
| D9 | The project file version becomes 0.15.0. Version 0.10.0 stays the minimum. The versions 0.13.0 and 0.14.0 stay unused. |

### 1.3 Decisions made in this plan

| Id | Decision | Reason |
| --- | --- | --- |
| P1 | All folders are resident. One query loads all folder elements and all folder-to-folder edges when the project opens. | A project has at most some thousands of folders. A deep tree then costs one query, and path resolution of a folder reads memory only. |
| P2 | A folder holds its child folders in memory. File membership is the `FolderContent` rows plus the pending changes of the folder (`ContentAddedSinceSync`, `ContentRemovedSinceSync`). | The two sets exist today. They are the difference between memory and the database before a sync. |
| P3 | SN4 removes the file name map and all file name checks (D8). | One element has one name. A name that is unique in each holder folder makes a link fail when two cameras gave the same name. The root rule prevented that failure at the cost of one name entry for each photo. |
| P4 | The node container is a page table: a vector of pages, and each page holds 4096 `std::shared_ptr<SleeveElement>` slots. The index is the element id. | Element ids are `uint32_t` values from one increasing generator. A lookup has no hash and no node allocation. |
| P5 | Each element has one intrusive list hook. An element is in the dirty list, in the eviction list, or in no list (a resident folder, or a clean element before SN7). | One hook has no allocation. An element in the dirty list cannot be evicted. |
| P6 | `SleeveElement::SetSyncFlag` is the only writer of the sync flag. It moves the element between the lists. | 62 lines in 12 source files read or write `sync_flag_` today, and some write the field directly. One writer makes the dirty list complete. |
| P7 | `Sync()` writes the inserts, the updates, and the deletes in one transaction. It marks the elements `SYNCED` after the commit. | Today the three groups commit separately. A failure between two groups leaves a part of the change in the database. |
| P8 | Bulk reads return rows (ids, names, folder ids) from a store query. They do not load elements into the cache. | A scan of a large mount then leaves the cache unchanged, so strict LRU order is sufficient. |
| P9 | The sidebar tree read uses no stored copy of the tree and no new consistency mechanism without the evidence that `AGENTS.md` requires. SN8 states the rule. | `AGENTS.md` prohibits a copy of owner state for convenience, and a generation without an executable interleaving. The first proposal in the discussion (a published tree copy with a counter) does not satisfy these rules. |
| P10 | The container type is private to `NodeCache`. `FileSystem` reads and writes nodes through `NodeCache` operations. | A later container change touches one file. |

| P11 | SN4 writes the new project file version (D9). | A build before this plan loads the root with `SleeveFolder::AddElementToMap`, which throws on the second equal name. Its tree is then empty with no message. The new version makes that build refuse the project with the version message. |

### 1.4 Rejected options that must not return

- A file name that is unique in the library root, or in each folder that holds the file.

- The database as the only source of truth with no in-memory node tree.
- A published copy of the folder tree with a change counter for the GUI thread.
- A cache of file directory entries (name to id). Under P3 no reader needs one.
- An adaptive eviction policy (ARC, 2Q, W-TinyLFU). P8 removes the scan problem that such a policy
  solves.
- A new hash map dependency. P4 uses the properties of the element id.

---

## 2. Current source audit

Each row is a verified source fact at the audit revision unless the row says "estimate".

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Node map | `FileSystem::storage_`, `include/sleeve/sleeve_filesystem.hpp` | `std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>`. `FileSystem` writes `storage_[id]` in 4 places and scans all entries in `DeleteFileEverywhere`, `DeleteFilesEverywhere`, `RenameFile`, and the three `Get*Elements` functions. | `NodeCache` owns the container (SN2). |
| Node loader | `NodeStorageHandler`, `include/sleeve/storage.hpp`, `sleeve/storage.cpp` | Shares one header with `Storage`. `GetElement` does `contains` and then `at`. | Own files (SN2). |
| Folder load | `NodeStorageHandler::EnsureChildrenLoaded` | Two queries for each folder. It loads each child element, files included. A `std::exception` ends the load with no log line (`// TODO: LOG`). | Child folders only (SN3, SN4). Log (SN1). |
| Folder content | `SleeveFolder`, `sleeve/sleeve_element/sleeve_folder.cpp` | `ContainsElementId` is a linear search in a vector. `AddElementToMap` calls it for each child, so the load of a folder with n children costs n squared comparisons. `RemoveElementById` scans the name map by value. | File entries leave the folder (SN4). |
| Root links | `FileSystem::Create`, `DuplicateFileToFolder`, `RenameFile` | Each file is a child of root 0. A new name gets `@` until it is unique in the root and in the target folder. `RenameFile` also scans all loaded folders. | P3 (SN4). |
| Sidebar tree | `FolderController::ReloadTree`, `LoadSubtree`, `LoadChildren`, `ui/alcedo_main/album_backend/folder_controller.cpp` | Recursive. One `AlbumBrowseService::ListFolders` call for each folder on the GUI thread. `LoadChildren` returns on any exception, and the folder stays unloaded. | One tree read (SN3). |
| Folder listing | `AlbumBrowseService::ListFolders`, `app/album_browse_service.cpp` | Calls `SleeveServiceImpl::ListFolderEntries`, which loads all entries of the folder, and keeps the folders. Returns `{}` on any exception. | Folder query and log (SN1). |
| Sync | `SleeveServiceImpl::Sync`, `app/sleeve_service.cpp` | Three scans of the node map. Two id sets. `AddElements`, `UpdateElements`, and `RemoveElements` commit separately. | Dirty list and one transaction (SN2). |
| Writes with no sync | `SleeveServiceImpl::Write_NoSync`, used in `app/import_service.cpp` | Import leaves dirty elements in memory until a later sync. The statement of #1148 that no dirty element exists outside a write is incorrect for import. | The dirty list holds them (SN2). |
| Mount tree read | `ReadMountTree`, `app/mount_tree.cpp` | Lists each folder of the mount and calls `fs.Get` for each child, so it loads each file element of the mount. | One store query (SN5). |
| Path access to files | `AlbumBrowseService::ListFilesInFolder(path)`, `AlbumBrowseService::DeleteFiles(paths)`, `SleeveServiceImpl::ResolveFile` | Callers are tests only. Production code addresses a file by its id. | Archive (SN4). |
| Callers that pass elements | `sleeve/*.cpp`, `app/mount_tree.cpp` | Many functions take `const std::shared_ptr<T>` by value where they do not keep the pointer. | `T&` in touched functions. |
| Existing LRU type | `LRUCache<K, V>`, `include/utils/cache/lru_cache.hpp` | A `std::list` plus a map of iterators: two allocations for each entry. `Evict` removes the last entry with no check. `access_count_` never increases. | Stays for its current users. `NodeCache` does not use it. |
| Async sidebar read | `FolderController`, `library_->query_worker()`, `SearchRequestKind::kFolderCounts` | The folder counts and the mount summaries already run on the query worker. | SN8 can use the same worker. |
| Tests | `tests/sleeve/sleeve_fs_test.cpp` (`SleeveFSTest`), `tests/app/sleeve_service_test.cpp` (`SleeveServiceTest`), `tests/app/mount_tree_test.cpp` (`MountTreeTest`), `tests/app/album_browse_service_mount_test.cpp` (`AlbumBrowseServiceMountTest`), `tests/app/project_open_benchmark.cpp` | 5 lines in 2 test files read `sync_flag_`. Some tests address files by path. | Update with each phase. |

Memory estimate (inference from the field layout, not measured): one file element with its
control block, its name, its map node, and its name entry in the root uses 400 to 450 bytes.
80,000 photos then use about 35 MB. The 4 GB of the 80,000-photo project has another owner. SN6
measures it.

---

## 3. Target architecture

### 3.1 Owners

| Owner | Holds | Changes | Error surface |
| --- | --- | --- | --- |
| `NodeCache` (proposed) | The page table, the dirty list, the eviction list, the size limit | Inserts a node, removes a deleted node, moves a node between lists, evicts | None. Its operations do not read the database. |
| `NodeStorageHandler` | A reference to `NodeCache` and to the stores | Loads a missing element by id. Loads the folder tree once. | Throws the store error. The caller logs the folder id and the text. |
| `FolderEntryStore` (proposed) | No state. duckorm statements on `Element` and `FolderContent`. | Reads child folders, folder edges, file membership, and mount file locations as rows | Throws the duckorm error. |
| `SleeveFolder` | The names and ids of its child folders. The pending membership changes. | Through `FileSystem` operations | Throws on a duplicate folder name. |
| `FileSystem` | `NodeCache`, `NodeStorageHandler`, `PathResolver`, the id generator | All structure operations | `std::runtime_error`, as today. |
| `SleeveServiceImpl` | `fs_lock_`, the sync operation | One transaction for each sync | `SyncResult`, as today. |

### 3.2 Node states

```text
                 SetSyncFlag(UNSYNC | MODIFIED | DELETED)
   clean  ------------------------------------------------>  dirty list
     ^                                                           |
     |        Sync commit: SetSyncFlag(SYNCED), or erase         |
     +-----------------------------------------------------------+

   clean folder : in no list (resident)
   clean file   : in no list before SN7, in the eviction list after SN7
```

An element has a back pointer to its `NodeCache` from the insert to the removal. `SetSyncFlag`
calls `NodeCache::OnSyncFlagChanged` through that pointer. An element with no cache (a row that a
store just read) changes its flag only.

### 3.3 File membership

```text
FolderHoldsFile(folder, file)
  = file in folder.ContentAddedSinceSync
    or (FolderEntryStore row (folder, file) exists
        and file not in folder.ContentRemovedSinceSync)
```

A batch operation sends one query for all its ids. A link inserts its row with a statement that
ignores an existing `(folder_id, element_id)` row, so a repeated link needs no read.

### 3.4 Primary call chains

Project open:

```text
SleeveServiceImpl ctor -> FileSystem::InitRoot
  -> NodeStorageHandler::LoadFolderTree
       -> FolderEntryStore::ReadFolderTree          (one query: folders and edges)
       -> NodeCache::Insert for each folder
       -> SleeveFolder child map for each edge      (explicit stack, visited set)
```

Write:

```text
SleeveServiceImpl::Write -> operation(FileSystem&)
  -> element.SetSyncFlag(...) -> NodeCache: element enters the dirty list
SleeveServiceImpl::Sync
  -> NodeCache::DirtyElements()                     (list walk, no table scan)
  -> ElementStore::WriteSync(inserts, updates, deletes)   (one transaction)
  -> commit -> SetSyncFlag(SYNCED) or NodeCache::Erase
```

Sync failure:

```text
ElementStore::WriteSync throws -> transaction rollback
  -> each element keeps its flag and its place in the dirty list
  -> SyncResult{success_ = false, message_}
  -> the next sync writes the same elements
```

---

## 4. File and API map

Proposed files:

| File | Content |
| --- | --- |
| `include/utils/intrusive_list.hpp` | `IntrusiveListHook` and `IntrusiveList<T, &T::hook>`: push back, erase, move to back, front, iteration. No allocation. |
| `include/sleeve/node_cache.hpp`, `sleeve/node_cache.cpp` | `NodeCache`: `Find(id)`, `Insert(element)`, `Erase(id)`, `OnSyncFlagChanged(element, old, new)`, `DirtyElements()`, `ForEachFolder(fn)`, `Size()`, and from SN7 `SetLimit(n)` and `EvictToLimit()`. |
| `include/sleeve/node_storage_handler.hpp`, `sleeve/node_storage_handler.cpp` | `NodeStorageHandler`, moved out of `include/sleeve/storage.hpp` and `sleeve/storage.cpp`. Adds `LoadFolderTree()`. |
| `include/storage/store/sleeve/folder_entry_store.hpp`, `storage/store/sleeve/folder_entry_store.cpp` | `FolderEntryStore`: `ReadChildFolders(folder_id)`, `ReadFolderTree()`, `ReadHeldFiles(folder_id, file_ids)`, `ReadMountFileFolders(mount_id)`. All statements use duckorm. `Storage` gives the store through `GetFolderEntryStore()`. |
| `tests/sleeve/node_cache_test.cpp`, `tests/storage/folder_entry_store_test.cpp` | Tests of the two new owners. |

Current files that change: `include/sleeve/sleeve_filesystem.hpp`, `sleeve/sleeve_filesystem.cpp`,
`include/sleeve/sleeve_element/sleeve_element.hpp`, `sleeve/sleeve_element/sleeve_element.cpp`,
`include/sleeve/sleeve_element/sleeve_folder.hpp`, `sleeve/sleeve_element/sleeve_folder.cpp`,
`sleeve/path_resolver.cpp`, `include/sleeve/storage.hpp`, `sleeve/storage.cpp`,
`storage/store/sleeve/element_store.cpp`, `app/sleeve_service.cpp`,
`app/album_browse_service.cpp`, `app/mount_tree.cpp`, `app/import_service.cpp`,
`ui/alcedo_main/album_backend/folder_controller.cpp`, and the CMake lists of `sleeve`, `storage`,
and the tests.

---

## 5. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| SN1 | The folder listing reads folders only. A failed listing writes a log line. | `FolderEntryStore`, `AlbumBrowseService`, `FolderController` | None | 350 to 500 | complete |
| SN2 | `NodeCache` with the page table and the dirty list. One sync transaction. | `NodeCache`, `SleeveElement`, `SleeveServiceImpl::Sync` | SN1 | 1100 to 1500 | complete |
| SN3 | All folders load with one query. One tree read for the sidebar. | `NodeStorageHandler`, `PathResolver`, `FolderController` | SN2 | 700 to 1000 | complete |
| SN4 | File entries leave the folders. File names are labels. Project file 0.15.0. | `SleeveFolder`, `FileSystem`, project version | SN3 | 1200 to 1700 | complete |
| SN5 | The mount tree read and the import use row queries. | `mount_tree.cpp`, `import_service.cpp` | SN4 | 600 to 900 | planned |
| SN6 | Measured memory and time of the 80,000-photo project. Decision for SN7 and SN8. | `project_open_benchmark.cpp` | SN5 | 150 to 300 | planned |
| SN7 | Eviction of clean file elements under a limit. Runs when SN6 requires it. | `NodeCache` | SN6 | 400 to 600 | planned |
| SN8 | The sidebar tree read runs on the query worker. Runs when SN6 requires it. | `FolderController` | SN6 | 300 to 500 | planned |

Each phase is one pull request. Before each phase, read `AGENTS.md` and the applicable skills
again.

---

## 6. Detailed phases

### SN1: Folder-only listing and visible failures

**Objective and deliverables.** The sidebar tree load reads no file element. One bad row in a
folder keeps the other folders listed. Each failed listing writes one warning.

**Modules, files, and APIs.**

- `FolderEntryStore::ReadChildFolders(sl_element_id_t folder_id) -> std::vector<std::shared_ptr<SleeveElement>>`
  (proposed). One statement: `Element` joined with `FolderContent` where `folder_id` is the
  parameter and `type` is FOLDER.
- `FileSystem::ListChildFolders(path)` and `SleeveServiceImpl::ListChildFolders(path)`. They put
  the folders into the node map when the map does not hold them, and return the live folders.
- `AlbumBrowseService::ListFolders` calls `ListChildFolders`.

**Implementation steps.**

1. Add `FolderEntryStore` with `ReadChildFolders` and register it in `Storage`.
2. Add `FileSystem::ListChildFolders`. An element that is in the node map keeps its live state.
3. Change `AlbumBrowseService::ListFolders` to the new call. Its catch logs the folder path and
   the exception text with `ALCEDO_LOG_WARN(Library)` and returns the folders that it has.
4. `NodeStorageHandler::EnsureChildrenLoaded` logs the folder id and the exception text.
5. `FolderController::LoadChildren` logs with `qCWarning(diag::libraryLog)` and leaves the folder
   unloaded, so the next expand reads it again.

**Tests and evidence.**

- `SleeveServiceTest.ListChildFoldersLeavesFileElementsUnloaded`: a project with files in the
  root and one mount. After the listing of the root, a `Read` finds no file element in memory.
- `SleeveServiceTest.ListChildFoldersKeepsFoldersWhenOneRowHasNoElement`: a `FolderContent` row
  of the root with no `Element` row. The listing returns the other folders.

**Exit criteria.**

- [x] Both tests pass in the debug build.
- [x] `SleeveFSTest`, `SleeveServiceTest`, and `AlbumBrowseServiceMountTest` pass.

**Completion record.**

```text
Phase / date / status: SN1 / 2026-11-26 / complete. Issue #1153 (sub-issue of #1152).
Source revision and branch: c00ad5afc (plan PR #1151) on refact/sleeve-folder-only-listing.
Actual changed modules: FolderEntryStore (new), Storage, NodeStorageHandler, PathResolver,
  FileSystem, SleeveServiceImpl, AlbumBrowseService, FolderController, SleeveServiceTest.
Implemented behavior:
  - FolderEntryStore::ReadChildFolders: one duckorm statement, FolderContent joined with Element
    where type is FOLDER. A FolderContent row with no Element row is not in the result.
  - NodeStorageHandler::ChildFolders: a folder with loaded children answers from memory (the
    changes of the current write are in the result); otherwise the store read, where a folder
    that the node map holds keeps its live object and each other folder goes into the map.
  - PathResolver::ResolveFolder: a path walk through ChildFolders at each part. It loads no file
    element and no children of a folder on the path.
  - FileSystem::ListChildFolders and SleeveServiceImpl::ListChildFolders (read lock).
  - AlbumBrowseService::ListFolders uses ListChildFolders. A failure writes
    ALCEDO_LOG_WARN(Library) with the path and the text, then throws.
  - NodeStorageHandler::EnsureChildrenLoaded writes ALCEDO_LOG_WARN(Library) with the folder id
    and the text.
  - FolderController::LoadChildren writes qCWarning(diag::libraryLog) and leaves the folder
    unloaded.
Decisions made during the work:
  - PathResolver::ResolveFolder is new. The plan did not name it: Resolve calls
    EnsureChildrenLoaded for each folder on the path and for the target, so a listing of
    /Trip through Resolve loads every file of the root.
  - ListFolders throws after the log line instead of returning the folders that it has. The
    listing is one store read, so a failure has no partial result; an empty result would make
    FolderController mark the folder loaded, and the next expand would not read it again. The
    automation command library.folders returns the error through the command registry.
  - ReadChildFolders returns std::shared_ptr<SleeveFolder> instead of SleeveElement.
  - FileSystem::IsInNodeMap(id) (memory read) gives the tests their observation. SN2 changes
    it to NodeCache::Find.
  - Test names: the plan fixture SleeveServiceTest is SleeveServiceTests in the source, and the
    first test uses its subclass MountedFolderStructureTest, which already builds files in the
    root and one mount.
Explicitly unimplemented items: none of SN1. The sidebar still sends one ListFolders call for
  each folder, and each call walks the path from the root; SN3 makes the tree resident.
Primary success call chain:
  FolderController::LoadChildren(path)
    -> AlbumBrowseService::ListFolders(path)
    -> SleeveServiceImpl::ListChildFolders(path)                   (Read, fs_lock_)
    -> FileSystem::ListChildFolders(path)
    -> PathResolver::ResolveFolder(path)
         -> NodeStorageHandler::ChildFolders(folder) at each part
              -> memory (children loaded) or FolderEntryStore::ReadChildFolders (one statement)
    -> NodeStorageHandler::ChildFolders(target)
    -> AlbumFolderView values -> child_keys_by_path_, loaded_paths_
Primary failure and restore call chain:
  FolderEntryStore::ReadChildFolders throws (DuckDB error, refused row)
    -> no folder changed; the folders read before stay in the node map as SYNCED elements
    -> AlbumBrowseService::ListFolders: ALCEDO_LOG_WARN(Library) path + text, rethrow
    -> FolderController::LoadChildren: qCWarning(libraryLog), loaded_paths_ unchanged
    -> the next expand of the folder reads it again
Build and test commands with exit codes:
  cmd /c "scripts\msvc_env.cmd --build build\debug --target SleeveServiceTest SleeveFSTest
    AlbumBrowseServiceMountTest --parallel 8"                                   exit 0
  cmd /c "scripts\msvc_env.cmd --build build\debug --target <folder_controller.cpp.obj>
    <library_commands.cpp.obj>"                                                 exit 0
  SleeveServiceTest.exe --gtest_brief=1                                         exit 0
  SleeveFSTest.exe --gtest_brief=1                                              exit 0
  AlbumBrowseServiceMountTest.exe --gtest_brief=1                               exit 0
  python scripts\check_storage_boundary.py                                      exit 0
Discovered / passed / failed / skipped counts:
  SleeveServiceTest 32 / 32 / 0 / 0, with
    SleeveServiceTests.ListChildFoldersKeepsFoldersWhenOneRowHasNoElement and
    MountedFolderStructureTest.ListChildFoldersLeavesFileElementsUnloaded
  SleeveFSTest 13 / 13 / 0 / 0
  AlbumBrowseServiceMountTest 3 / 3 / 0 / 0
Manual verification: none. The application was not started.
Performance or resource evidence, if required: not required for SN1.
Evidence path: build/tmp/sn1/ (build.log, build2.log, build_ui.log, suite logs).
Remaining defects or unavailable platforms: macOS and Linux builds not run.
  sleeve/sleeve_filesystem.cpp grows from 785 to 790 lines (ListChildFolders); SN2 makes it
  smaller (D7).
```

### SN2: NodeCache, dirty list, one sync transaction

**Objective and deliverables.** `NodeCache` owns all nodes. `Sync()` visits the dirty elements
only and commits once.

**Data rules and invariants.**

- An element with flag `UNSYNC`, `MODIFIED`, or `DELETED` that is in the cache is in the dirty
  list. An element with flag `SYNCED` is not in the dirty list.
- `sync_flag_` is private. `SleeveElement::SyncState()` reads it. `SetSyncFlag` writes it.
- The page table allocates a page at the first insert into its id range and releases a page when
  its last slot empties.
- `NodeCache::Erase` requires an element that is in no list or in the dirty list with flag
  `DELETED`.

**Implementation steps.**

1. Add `include/utils/intrusive_list.hpp`.
2. Add `NodeCache` with the page table (P4), `Find`, `Insert`, `Erase`, `ForEachFolder`, and
   `Size`. `Insert` sets the back pointer of the element and puts a dirty element into the dirty
   list.
3. Move `NodeStorageHandler` to its own files. It holds `NodeCache&`. `GetElement` does one
   `Find`.
4. Replace `FileSystem::storage_` with `NodeCache`. Replace the 4 direct writes with `Insert`.
   Replace the scans of `DeleteFileEverywhere`, `DeleteFilesEverywhere`, and `RenameFile` with
   `ForEachFolder`.
5. Make `sync_flag_` private. Change each reader to `SyncState()` and each direct write to
   `SetSyncFlag`.
6. Remove `GetModifiedElements`, `GetUnsyncedElements`, `GetDeletedElements`, and
   `GarbageCollect` from `FileSystem`. Add `NodeCache::DirtyElements()`.
7. Add `ElementStore::WriteSync(inserts, updates, deletes)` in `element_store.cpp`. It opens one
   `duckorm::Transaction`, calls the row functions that `AddElements`, `UpdateElements`, and
   `RemoveElements` use today, and commits. Remove the three separate commits from the sync path.
8. Rewrite `SleeveServiceImpl::Sync`: split the dirty list by flag (a `DELETED` element that was
   `UNSYNC` has no database row and is erased only), call `WriteSync`, then mark and erase.
9. Change touched functions that take `const std::shared_ptr<T>` by value and do not keep the
   pointer to `T&`.

**Primary failure and restore call chain.** Section 3.4, "Sync failure".

**Tests and evidence.**

- `NodeCacheTest.InsertFindEraseAcrossPageBoundaries`: ids 1, 4095, 4096, 4097, and 1,000,000.
- `NodeCacheTest.PageIsReleasedWhenItsLastElementIsErased`.
- `NodeCacheTest.FlagChangeMovesElementIntoAndOutOfDirtyList`: each flag transition.
- `SleeveServiceTest.SyncAfterOneRenameWritesOneElementWithTenThousandInMemory`: 10,000 synced
  elements, one folder rename, `SyncResult::elements_synced_` equals the changed elements.
- `SleeveServiceTest.FailedSyncLeavesDatabaseUnchangedAndRetrySucceeds`: a sync with one insert,
  one update, and one delete, where the delete step fails (a row that `StorageTestAccess` makes
  invalid). The database holds none of the three changes. The next sync after the repair writes
  all three.

**Exit criteria.**

- [x] The tests above pass. `SleeveFSTest`, `SleeveServiceTest`, `MountTreeTest`,
      `MountServiceTest`, `ImportServiceTest`, and `AlbumQueryTest` pass. In
      `ImportServiceTest`, `BatchCancelTest` fails before SN2 too (#1156).
- [x] No first-party file outside `sleeve/sleeve_element/` writes the sync flag field.
- [x] `sleeve/sleeve_filesystem.cpp` has fewer lines than at the audit revision.

**Completion record.**

```text
Phase / date / status: SN2 / 2026-11-26 / complete. Issue #1155 (sub-issue of #1152).
Source revision and branch: d00438089 (SN1 PR #1154 plus an LF conversion commit) on
  refact/sleeve-node-cache.
Actual changed modules: IntrusiveList (new), NodeCache (new), NodeStorageHandler (own files),
  SleeveElement, SleeveFolder, PathResolver, FileSystem, Storage, ElementStore,
  SleeveServiceImpl, mount_tree.cpp, mount_service.cpp, AlbumBrowseService, NodeCacheTest (new),
  SleeveServiceTest, SleeveFSTest, ImportServiceTest, AlbumQueryTest, LibraryBuildMemoryTest.
Implemented behavior:
  - include/utils/intrusive_list.hpp: IntrusiveListHook<T> and IntrusiveList<T, &T::hook>:
    PushBack, Erase, MoveToBack, Front, Clear, iteration. No allocation. A copied hook is
    unlinked.
  - NodeCache (include/sleeve/node_cache.hpp, sleeve/node_cache.cpp): page table of
    4096 std::shared_ptr<SleeveElement> slots per page, indexed by the element id. A page is
    allocated at the first insert into its range and released when its last slot empties.
    Find, Contains, Insert, Erase, ForEachFolder, DirtyElements, DirtyCount, Size, PageCount.
    The destructor clears the back pointer of each held element.
  - The dirty list: each element has one IntrusiveListHook and a back pointer to its cache.
    SleeveElement::SetSyncFlag is the only writer of sync_flag_ (now private) and calls
    NodeCache::OnSyncFlagChanged, which moves the element into the list (UNSYNC, MODIFIED,
    DELETED) or out of it (SYNCED). Insert puts a dirty element into the list.
  - Erase refuses a dirty element whose flag is not DELETED (std::logic_error), so a change
    that no sync wrote cannot leave the cache. Insert refuses an element of another cache and a
    different element in an occupied slot; the element that the slot holds is a no-op.
  - NodeStorageHandler moved to include/sleeve/node_storage_handler.hpp and
    sleeve/node_storage_handler.cpp. It holds NodeCache&. GetElement does one Find.
    GarbageCollect is removed.
  - FileSystem::storage_ is NodeCache nodes_. The 4 direct writes are Insert calls. The scans of
    DeleteFileEverywhere, DeleteFilesEverywhere, and RenameFile use ForEachFolder.
    GetModifiedElements, GetUnsyncedElements, GetDeletedElements, and GarbageCollect are removed;
    FileSystem::DirtyElements, FileSystem::MarkWritten, and FileSystem::Nodes (const) are new.
    SyncToDB (deprecated, SleeveManager) walks the dirty list.
  - ElementStore::WriteSync(inserts, updates, deletes): one duckorm::Transaction around
    InsertElementBatchRows, UpdateElementRows, and RemoveElementBatchRows (the row functions of
    AddElements, UpdateElements, and RemoveElements, now private static). It changes no element.
    RemoveElements now also runs in one transaction.
  - SleeveServiceImpl::Sync: splits the dirty list by flag, calls WriteSync once, and then
    FileSystem::MarkWritten: a DELETED element leaves the cache, each other element becomes
    SYNCED and a folder clears its pending content changes. elements_synced_ is the size of the
    dirty list.
Decisions made during the work:
  - NodeCache::OnSyncFlagChanged takes the element only. The list membership is the old state,
    so the old flag is not needed.
  - SleeveElement::MarkModified() (SYNCED becomes MODIFIED) replaces each copy of that check in
    SleeveFolder, PathResolver, FileSystem, mount_tree.cpp, and AlbumQueryTest.
    SleeveElement::IsLive() replaces the reads of sync_flag_ != DELETED.
  - The copy constructor and the copy assignment of SleeveElement are deleted: the cache keeps
    the address of each element. No code copied an element by value.
  - NodeCache keeps the ids of its folders in a std::unordered_set, so ForEachFolder reads no
    file slot. A project has some thousands of folders.
  - node_cache.cpp is in the SleeveElement library: SleeveElement::SetSyncFlag calls the cache,
    and SleeveFS links SleeveElement. sleeve_element.hpp forward-declares NodeCache, because
    node_cache.hpp includes sleeve_element.hpp (an include cycle; the element holds a pointer
    only, and sleeve_element.cpp includes node_cache.hpp).
  - Sync calls FileSystem::DirtyElements and FileSystem::MarkWritten instead of a NodeCache
    reference, so FileSystem stays the only writer of its cache (P10).
  - FileSystem::IsInNodeMap (SN1) is FileSystem::Nodes().Contains: a const reference to the
    cache, for reads only.
  - A DELETED element that never had a row (created and deleted before a sync) goes into the
    delete group, as before SN2. The element has no record of a stored row, and the delete
    statements remove nothing for it. Plan step 8 said "erased only"; that needs a new field,
    for no measured cost.
  - The page entry member is `elements`, not `slots`: Qt defines `slots` as an empty macro, and
    the member disappeared in the Qt translation units (clang -Wmissing-declarations in the
    first Mac build of alcedo_main). The struct then had two layouts.
  - SyncAfterOneRenameWritesOneElementWithTenThousandInMemory expects 2 written elements: the
    folder and the root. ResolveForWrite marks the parent MODIFIED, and the remove and the add
    of the folder id cancel in the pending sets, so the update of the root writes its Element
    row only. The test name keeps the plan name.
  - FailedSyncLeavesDatabaseUnchangedAndRetrySucceeds makes the delete step fail with
    ALTER TABLE SourceFile RENAME TO SourceFileAside on the project connection
    (StorageTestAccess), and the repair renames it back. A DuckDB DELETE does not fail on a
    row value, so "a row that StorageTestAccess makes invalid" was not possible.
  - Test names: the fixture of the plan name SleeveServiceTest is SleeveServiceTests in the
    source.
Explicitly unimplemented items: none of SN2. ElementStore::AddElements, UpdateElements,
  RemoveElements, AddElement, and UpdateElement stay for their test callers and SyncToDB.
Primary success call chain:
  SleeveServiceImpl::Write(operation)                                  (fs_lock_)
    -> operation(FileSystem&) -> element.SetSyncFlag / MarkModified
         -> NodeCache::OnSyncFlagChanged -> dirty_.PushBack            (no allocation)
    -> SleeveServiceImpl::Sync
         -> FileSystem::DirtyElements -> NodeCache::DirtyElements      (list walk)
         -> split by SyncState: inserts, updates, deletes
         -> ElementStore::WriteSync                                    (one transaction)
              -> InsertElementBatchRows, UpdateElementRows, RemoveElementBatchRows, commit
         -> FileSystem::MarkWritten
              -> DELETED: NodeCache::Erase (slot, folder id set, page release)
              -> other: SleeveFolder::MarkContentSynced, SetSyncFlag(SYNCED) -> dirty_.Erase
    -> SyncResult{success_ = true, elements_synced_ = dirty list size}
Primary failure and restore call chain:
  ElementStore::WriteSync: a statement or the commit throws
    -> duckorm::Transaction destructor: ROLLBACK (no row of the three groups stays)
    -> FileSystem::MarkWritten does not run: each element keeps its flag, its pending content
       changes, and its place in the dirty list; no element leaves the cache
    -> SyncResult{success_ = false, message_ = DuckDB text}
    -> the next Write (or Sync) writes the same elements
Build and test commands with exit codes (macOS arm64, build/macos-debug, Ninja):
  cmake build/macos-debug                                                      exit 0
  cmake --build build/macos-debug --target NodeCacheTest SleeveFSTest SleeveServiceTest
    MountTreeTest MountServiceTest AlbumBrowseServiceMountTest ImportServiceTest AlbumQueryTest
    SleeveFilesystemCiTest MountStoreTest AiStorageControllerTest alcedo_main -j 8 -- -k 0
                                                                               exit 0
  <Target> --gtest_brief=1 for each target below                               exit 0
Discovered / passed / failed / skipped counts:
  NodeCacheTest 7 / 7 / 0 / 0
  SleeveFSTest 13 / 13 / 0 / 0
  SleeveServiceTest 34 / 34 / 0 / 0, with
    SleeveServiceTests.SyncAfterOneRenameWritesOneElementWithTenThousandInMemory and
    SleeveServiceTests.FailedSyncLeavesDatabaseUnchangedAndRetrySucceeds
  MountTreeTest 9 / 9 / 0 / 0
  MountServiceTest 29 / 29 / 0 / 0
  AlbumBrowseServiceMountTest 3 / 3 / 0 / 0
  ImportServiceTest 16 / 15 / 1 / 0: ImportServiceTests.BatchCancelTest fails at the source
    revision before SN2 too (5 of 5 runs; issue #1156). The Mac had no batch_import folder; the
    run used 70 of its 164 files and raw/airplane/_DSC1704.NEF, copied for the run.
  AlbumQueryTest 39 / 38 / 0 / 1 (the timing profile skips without ALCEDO_ALBUM_QUERY_PROFILE)
  SleeveFilesystemCiTest 2 / 2 / 0 / 0
  MountStoreTest 17 / 17 / 0 / 0
  AiStorageControllerTest 20 / 20 / 0 / 0
Manual verification: none. The application was not started.
Performance or resource evidence, if required: not required for SN2. The sync of one rename
  with 10,000 clean elements in memory writes 2 elements (the test above).
Evidence path: the counts above. The Mac logs under build/tmp/sn2/ were removed at the end of
  the task.
Remaining defects or unavailable platforms: Windows (MSVC) not built: the Windows machine was
  in use. LibraryBuildMemoryTest is Windows-only and was not built; its change uses only the
  APIs that the Mac build compiled. Linux not built.
  The Mac checkout had no alcedo_studio/tests/resources/temp_folder (gitignored), so
  SleeveFSTest could not create its database until the folder was made.
  storage/store/sleeve/element_store.cpp grows from 1415 to 1448 lines (WriteSync and the two
  row functions, plan step 7); D7 holds for sleeve/sleeve_filesystem.cpp (785 to 749 lines).
```

### SN3: Resident folder tree with one query

**Objective and deliverables.** The project open loads all folders with one query. The sidebar
reads the tree with one service call.

**Data rules and invariants.**

- After `InitRoot`, each live folder element is in `NodeCache` and each parent holds the names
  and ids of its child folders.
- A folder can have more than one parent (the copy-on-write reference count). The load adds one
  child entry for each edge and does not change `ref_count_`.
- The tree walk uses an explicit stack and a visited set of folder ids. An edge to a visited
  folder on the current path is a cycle: the load skips the edge and writes one warning with both
  ids.

**Implementation steps.**

1. Add `FolderEntryStore::ReadFolderTree()`: the folder elements and the edges
   `(parent_id, child_id)` where both ends are folders.
2. Add `NodeStorageHandler::LoadFolderTree()` and call it from `FileSystem::InitRoot`.
3. Give `SleeveFolder` a child folder map that is complete after the load. `PathResolver::Resolve`
   and `ResolveForWrite` read it for each path part.
4. Keep `EnsureChildrenLoaded` for file entries until SN4. It no longer loads folders.
5. Add `SleeveServiceImpl::ListFolderTree()` and `AlbumBrowseService::ListFolderTree()`. The
   result is a vector of `AlbumFolderView` values in parent-before-child order.
6. `FolderController::ReloadTree` calls `ListFolderTree` once and fills `nodes_by_path_` and
   `child_keys_by_path_`. `LoadSubtree` and `LoadChildren` then read these maps.
7. `library.folders` of `ui/alcedo_main/automation/commands/library_commands.cpp` uses the same
   call.

**Tests and evidence.**

- `SleeveServiceTest.OpenLoadsNestedFoldersWithOneTreeRead`: a tree of depth 40 and 2,000
  folders. After open, each folder path resolves with no store read (a counter in the test store
  wrapper stays at the value after open).
- `SleeveServiceTest.FolderWithTwoParentsAppearsUnderBoth`.
- `SleeveServiceTest.FolderEdgeCycleIsSkippedWithWarning`: rows from `StorageTestAccess`.
- `AlbumBrowseServiceMountTest`: the existing mount tree assertions pass with `ListFolderTree`.

**Exit criteria.**

- [x] The tests above pass. `CollectionsPanelQmlTest` and `LibraryFolderSectionQmlTest` pass.
- [x] `project_open_benchmark` reports the tree read time. The value goes into the record.

**Completion record.**

```text
Phase / date / status: SN3 / 2026-11-26 / complete. Issue #1158 (sub-issue of #1152).
Source revision and branch: a36d1ce96 on refact/sleeve-folder-tree, stacked on 26387aae2
  (SN2 PR #1157).
Actual changed modules: FolderEntryStore, NodeStorageHandler, PathResolver, FileSystem,
  SleeveServiceImpl, AlbumBrowseService, FolderController, library.folders
  (library_commands.cpp), SleeveServiceTest, AlbumBrowseServiceMountTest,
  AutomationLibraryCommandsTest, ProjectOpenBenchmark.
Implemented behavior:
  - FolderEntryStore::ReadFolderTree: every Element row of type FOLDER (id order) and every
    FolderContent row whose two ends are folders (parent, then child order), under one database
    lock. The result type is FolderTreeRows. ReadChildFolders (SN1) has no caller and is removed.
  - NodeStorageHandler::LoadFolderTree, called by FileSystem::InitRoot for a stored root: puts
    each folder into the NodeCache (the root keeps its object) and adds one child entry for each
    link with AddElementToMap(child, false, false): no reference count change, no pending content
    change, no MODIFIED flag. The walk starts at the root and then at each folder that the root
    does not reach, with an explicit stack and a state per folder (on the walk path, done). A link
    to a folder on the walk path closes a cycle: skipped, one warning
    "the folder link <parent> -> <child> closes a cycle of folders". A link whose child name the
    parent holds for another folder is skipped with one warning. A failed read writes one error
    line and throws, so the project open fails with the store message.
  - NodeStorageHandler::EnsureChildrenLoaded adds file entries only: a stored link to a folder
    adds nothing (the folder entries are resident and current), and a file in
    ContentRemovedSinceSync stays removed.
  - NodeStorageHandler::ChildFolders reads memory only (folder entries and NodeCache::Find).
    NodeStorageHandler::FolderTree(root): every live folder at each of its paths, parents before
    children, in the order of the folder entries; a folder with two parents is listed under
    each. FolderTreeEntry{path_, folder_} is the entry type.
  - PathResolver::Resolve reads the folder entries for each part; only a part that names no
    child folder loads the file entries of its folder. The result folder no longer loads its
    file entries. PathResolver::ResolveFolder reads memory only.
  - PathResolver::ResolveForWrite loads the file entries of each folder on the path (as before)
    and now also of a folder result, so the name checks of a write see every entry while file
    entries stay in the name map (until SN4).
  - PathResolver::CoWHandler loads the file entries of the source folder before the copy.
  - FileSystem::ListFolderTree, SleeveServiceImpl::ListFolderTree (read lock), and
    AlbumBrowseService::ListFolderTree (AlbumFolderView values; logs and rethrows a failure).
  - FolderController: LoadTree calls ListFolderTree once and fills nodes_by_path_ and
    child_keys_by_path_. ReloadTree marks every node expanded (the old LoadSubtree result).
    EnsurePathExpanded only marks the nodes of the path expanded; it calls LoadTree when the last
    read failed (tree_loaded_ false), so a failed read is retried at the next selection.
    LoadChildren, LoadSubtree, and loaded_paths_ are removed.
  - library.folders: one ListFolderTree call; the parent id and the depth come from the listed
    parent path. A failed read answers Failed, "The folder tree cannot be read."
Decisions made during the work:
  - "One query" (P1) is one store call: two statements under one database lock, the folders and
    the links. One statement with the link column would need a new mapper for an
    element-plus-parent row.
  - CoWHandler loads the source first. The bug: after an open, the first write below a shared
    folder copied it while its file entries were not loaded; the old code then loaded the
    entries of the new id (none) and marked the copy loaded, so the copy lost its files.
    CopyOnWriteOfSharedFolderAfterOpenKeepsItsFiles covers it, and fails with the load removed
    (Mac probe run: "{ Inner, New }" against "{ Inner, New, Photo.ARW }").
  - ResolveForWrite keeps the file entry loads of SN2 and adds the result folder. Before SN3 the
    final Resolve of a folder loaded its entries, and write callers (Create, Copy) relied on it.
  - FolderTree also skips a folder that is on its own walk path, with one warning, so a cycle
    that a later write made cannot loop the sidebar read.
  - OpenLoadsNestedFoldersWithOneTreeRead has no counting store wrapper: the test renames the
    Element table aside after the open. Each element read joins Element, so a store read throws,
    and a file entry load writes a "did not load" warning; the test resolves the 2,000 paths and
    lists the tree with no throw and no such warning. FolderContent cannot be renamed (DuckDB
    refuses: its two indexes depend on it). The open itself reads the tree with one
    ReadFolderTree call (source; no counter).
  - FolderWithTwoParentsAppearsUnderBoth writes the stored reference count 2 with SQL: a copy
    with FileSystem::Copy increments the count in memory and does not mark the folder modified,
    so the count of 2 is not stored by the copy (as before SN3).
  - Tests added beyond the plan: CopyOnWriteOfSharedFolderAfterOpenKeepsItsFiles,
    AlbumBrowseServiceMountTest.FolderTreeGivesEachFolderItsMountAfterReopen,
    AutomationLibraryCommandsTest.FoldersListsEachFolderAfterItsParentWithParentIdAndDepth.
  - Test names: the fixture of the plan name SleeveServiceTest is SleeveServiceTests.
Explicitly unimplemented items: none of SN3. File entries stay in the folder name map until SN4;
  EnsureChildrenLoaded still reads all rows of the folder (GetFolderContent and
  GetFolderChildren) and keeps the file rows.
Primary success call chain:
  ProjectService open -> SleeveServiceImpl ctor -> FileSystem::InitRoot
    -> NodeCache::Insert(root)
    -> NodeStorageHandler::LoadFolderTree
         -> FolderEntryStore::ReadFolderTree           (one lock: folders, folder links)
         -> NodeCache::Insert for each folder
         -> walk from the root, then from each unreached folder (explicit stack)
              -> SleeveFolder::AddElementToMap(child, false, false) for each link
  FolderController::ReloadTree -> LoadTree
    -> AlbumBrowseService::ListFolderTree -> SleeveServiceImpl::ListFolderTree (fs_lock_)
         -> FileSystem::ListFolderTree -> NodeStorageHandler::FolderTree   (memory only)
    -> nodes_by_path_, child_keys_by_path_ -> RebuildFolderView
  SleeveServiceImpl::ResolveFolder(path) -> PathResolver::Resolve
    -> SleeveFolder::GetElementIdByName per part -> NodeCache::Find        (memory only)
Primary failure and restore call chain:
  FolderEntryStore::ReadFolderTree throws (DuckDB error)
    -> NodeStorageHandler::LoadFolderTree: one error log line, rethrow
    -> FileSystem::InitRoot -> SleeveServiceImpl ctor -> the project open fails with the message
  A stored link closes a cycle
    -> LoadFolderTree skips the link, one warning with both ids; the other links load
  ListFolderTree throws in the sidebar
    -> AlbumBrowseService: one warning, rethrow -> FolderController::LoadTree: one warning, root
       node only, tree_loaded_ false -> the next selection (EnsurePathExpanded) reads it again
Build and test commands with exit codes (macOS arm64, build/macos-debug, Ninja):
  cmake --build build/macos-debug --target SleeveServiceTest AlbumBrowseServiceMountTest
    NodeCacheTest SleeveFSTest MountTreeTest MountServiceTest ImportServiceTest AlbumQueryTest
    SleeveFilesystemCiTest MountStoreTest AiStorageControllerTest ProjectOpenBenchmark
    CollectionsPanelQmlTest LibraryFolderSectionQmlTest AlbumBackendFolderTest
    FolderControllerMountTest AutomationLibraryCommandsTest alcedo_main -j 8 -- -k 0   exit 0
  <Target> --gtest_brief=1 for each target below (QT_QPA_PLATFORM=offscreen)     exit 0,
    ImportServiceTest exit 1 (BatchCancelTest, #1156)
Discovered / passed / failed / skipped counts:
  SleeveServiceTest 38 / 38 / 0 / 0, with SleeveServiceTests.OpenLoadsNestedFoldersWithOneTreeRead,
    SleeveServiceTests.FolderWithTwoParentsAppearsUnderBoth,
    SleeveServiceTests.CopyOnWriteOfSharedFolderAfterOpenKeepsItsFiles, and
    SleeveServiceTests.FolderEdgeCycleIsSkippedWithWarning
  AlbumBrowseServiceMountTest 4 / 4 / 0 / 0, with FolderTreeGivesEachFolderItsMountAfterReopen
  AutomationLibraryCommandsTest 18 / 18 / 0 / 0, with
    FoldersListsEachFolderAfterItsParentWithParentIdAndDepth
  CollectionsPanelQmlTest 30 / 30 / 0 / 0
  LibraryFolderSectionQmlTest 8 / 8 / 0 / 0
  AlbumBackendFolderTest 16 / 16 / 0 / 0
  FolderControllerMountTest 11 / 11 / 0 / 0
  NodeCacheTest 7 / 7 / 0 / 0
  SleeveFSTest 13 / 13 / 0 / 0
  MountTreeTest 9 / 9 / 0 / 0
  MountServiceTest 29 / 29 / 0 / 0
  AlbumQueryTest 39 / 38 / 0 / 1 (the timing profile skips without ALCEDO_ALBUM_QUERY_PROFILE)
  SleeveFilesystemCiTest 2 / 2 / 0 / 0
  MountStoreTest 17 / 17 / 0 / 0
  AiStorageControllerTest 20 / 20 / 0 / 0
  ImportServiceTest 16 / 15 / 1 / 0: ImportServiceTests.BatchCancelTest fails before SN2 too
    (#1156). The run used raw/airplane/_DSC1704.NEF and 70 of the 164 batch_import files,
    copied to the Mac for the run.
  The QML and controller logs have no "FAIL!" and no QtTest compare failure (#526).
Manual verification: none. The application was not started.
Performance or resource evidence, if required: ProjectOpenBenchmark on a copy of a user project
  (v31.alcd: 3 folders, 86 photos), 3 rounds, Mac debug build:
    folder_tree_query (FolderEntryStore::ReadFolderTree, the open's store read) 0.72 to 0.77 ms
    folder_tree (AlbumBrowseService::ListFolderTree, the sidebar read)        0.004 to 0.014 ms
    project_service (the open, tree load included)                            17 to 37 ms
  The 80,000-photo project is on the Windows machine; SN6 measures it.
Evidence path: the counts above. The Mac logs under build/tmp/sn3/ were removed at the end of
  the task.
Remaining defects or unavailable platforms: Windows (MSVC) and Linux not built: the Windows
  machine was in use. AutomationLibraryE2ETest was not run: it starts alcedo_main processes.
  The Mac checkout had no tests/resources/temp_folder and no RAW test files; both were made for
  the run and removed after it.
  sleeve/sleeve_filesystem.cpp: 749 to 747 lines (D7).
```

### SN4: File entries on demand, file names as labels

**Objective and deliverables.** A folder holds no file entry in memory. The library accepts equal
file names. The project file version is 0.15.0.

**Inputs and prerequisites.** SN3 is merged. D8 and D9 apply.

**Data rules and invariants.**

- File membership follows section 3.3.
- A folder name is unique among the child folders of its parent. A file name has no uniqueness
  rule. A file and a folder in one parent can have the same name.
- `ContentAddedSinceSync` and `ContentRemovedSinceSync` hold file ids and folder ids, as today.
- A project of version 0.10.0 to 0.12.0 opens with no data change. The next save writes 0.15.0.

**Implementation steps.**

1. Add `FolderEntryStore::ReadHeldFiles(folder_id, file_ids)`: the ids of `file_ids` that have a
   row in the folder.
2. Remove file entries from `SleeveFolder::contents_` and from the default index.
   `AddElementToMap` for a file records the pending add only. Remove `ContainsElementId` for
   files, `RemoveElementById`, and `RemoveElementsById`; add `RecordFileLinked(id)` and
   `RecordFileUnlinked(id)`.
3. `FileSystem::LinkFileToFolder`: record the link. The sync inserts the row with a statement that
   ignores an existing row. When duckorm has no such statement form, add it to duckorm in this
   phase with a test in `DuckormStatementTest`.
4. `UnlinkFileFromFolder` and `UnlinkFilesFromFolder`: one `ReadHeldFiles` call plus the pending
   sets give the members. The single form throws for a file that is not a member, as today.
5. `Create`, `CreateFileInLibrary`, `DuplicateFileToFolder`, `RenameFile`: remove the name loops
   for files. `RenameFile` sets the name and the flag.
6. `DeleteFileEverywhere` and `DeleteFilesEverywhere`: remove the ids from the pending sets of
   each folder with `ForEachFolder`, then set `DELETED`. `ElementStore::RemoveElements` already
   removes the rows.
7. `ListFolderContent` returns child folder ids. Add `FileSystem::ListFileIds(folder_id)`, which
   reads the ids from the store and applies the pending sets. Change each caller to the call that
   it needs.
8. Remove `EnsureChildrenLoaded` and `SleeveFolder::children_loaded_`.
9. Move `AlbumBrowseService::ListFilesInFolder(path)`, `AlbumBrowseService::DeleteFiles(paths)`,
   `SleeveServiceImpl::ResolveFile`, and the file branch of `FileSystem::Delete(path)` to the
   archive location that the legacy code rule of the repository names. Change their tests to the
   id operations.
10. Remove `FreeNameForFolder` from `app/mount_tree.cpp`.
11. Set `kProjectFileVersion` and `kMaxSupportedProjectFileVersion` to `0.15.0` in
    `include/edit/history/pipeline_history_format.hpp` and extend the version comment.

**Primary failure and restore call chain.**

```text
FileSystem::UnlinkFileFromFolder -> FolderEntryStore::ReadHeldFiles throws
  -> no pending set changed, no flag changed
  -> SleeveServiceImpl::Write returns the exception to the caller, as today
```

**Tests and evidence.**

- `SleeveServiceTest.TwoFilesWithEqualNamesLinkIntoOneFolder`.
- `SleeveServiceTest.LinkAndUnlinkInOneWriteLeaveNoRow` and
  `SleeveServiceTest.UnlinkOfAFileLinkedInTheSameWriteNeedsNoRow`: the pending sets cancel.
- `SleeveServiceTest.RepeatedLinkKeepsOneRow`.
- `SleeveServiceTest.LinkIntoFolderWithTenThousandFilesLoadsNoFileElement`.
- `SleeveServiceTest.ProjectWithEqualRootNamesReopensWithAllFiles`.
- A project file of version 0.12.0 from the test data opens, and its saved metadata reads 0.15.0.

**Exit criteria.**

- [x] The tests above pass. `SleeveFSTest`, `SleeveServiceTest`, `MountTreeTest`,
      `MountServiceTest`, `ImportServiceTest`, `AlbumQueryTest`, and the project package tests
      pass. In `ImportServiceTest`, `BatchCancelTest` fails before SN2 too (#1156).
- [x] `sleeve/sleeve_filesystem.cpp` and `sleeve/sleeve_element/sleeve_folder.cpp` have fewer
      lines than after SN3.

**Completion record.**

```text
Phase / date / status: SN4 / 2026-11-26 / complete. Issue #1160 (sub-issue of #1152).
Source revision and branch: cd3703e95 on refact/sleeve-file-entries, stacked on c68736175
  (SN3 PR #1159).
Actual changed modules: FolderEntryStore, SleeveFolder, NodeStorageHandler, PathResolver,
  FileSystem, ElementStore (new folder rows), SleeveServiceImpl, AlbumBrowseService,
  mount_tree.cpp, pipeline_history_format.hpp, and the tests SleeveServiceTest, SleeveFSTest,
  SleeveFilesystemCiTest, MountTreeTest, MountStoreTest, FilterServiceTest,
  LibrarySearchColumnsTest, ProjectServiceTest, and the version tests of ProjectCompatibilityTest,
  CommitGraphTest, PipelineDocumentCheckpointTest, PipelineEditBatchTest,
  EditorMiniGitMaterializerTest, and BrushSourceFormatBoundaryTest.
Implemented behavior:
  - FolderEntryStore::ReadFolderFiles(folder_id) and ReadHeldFiles(folder_id, file_ids): one
    duckorm statement each, FolderContent joined with Element where type is FILE, id order.
    ReadHeldFiles with no ids sends no statement.
  - SleeveFolder holds its child folders only (contents_ and the default index). AddElementToMap
    takes a folder and throws std::invalid_argument for a file. RecordFileLinked and
    RecordFileUnlinked write the pending sets; PendingMembership reads them; DropPendingChanges
    forgets the ids of deleted files. HoldsChildFolder replaces ContainsElementId.
    RemoveElementById, RemoveElementsById, ContentSize, children_loaded_, ChildrenLoaded, and
    MarkChildrenLoaded are removed.
  - Pending set rule: for a child folder, an add and a remove of one id cancel (memory holds every
    child folder). For a file, the last link or unlink wins (the id moves to its set): the insert
    ignores an existing row and the delete ignores a missing row, so no link or unlink needs a
    read to keep the sets correct.
  - NodeStorageHandler::EnsureChildrenLoaded is removed. FileIds(folder): the stored file rows,
    minus the pending unlinks, plus the pending links that are not child folders, sorted; one
    read, none for an UNSYNC folder. HeldFiles(folder, ids): a pending change decides an id from
    memory; the other ids go to one ReadHeldFiles read. Both drop an id that the cache holds as
    DELETED.
  - PathResolver: a path names folders only. Resolve reads the resident child folders and throws
    for a part that names no live child folder. ResolveForWrite loads nothing. CoWHandler reads
    FileIds of the source before any change and records each id as a pending link of the copy.
    Tree lists the folders and the files (FileIds and an element load per file; diagnostics).
  - FileSystem: Create gives a folder the first free name among the child folders; a file keeps
    its name and gets the root link and the destination link as pending links. LinkFileToFolder
    records the link only. UnlinkFileFromFolder uses HeldFiles and throws for a file that is not a
    member. UnlinkFilesFromFolder sends at most one read. DuplicateFileToFolder keeps the name.
    DeleteFileEverywhere and DeleteFilesEverywhere call DropPendingChanges on each folder and set
    DELETED. RenameFile(file_id, name) sets the name and the flag. ListFolderContent(folder_id)
    gives the child folder ids and then the file ids; ListFileIds(folder_id) gives the file ids.
    LiveFile is a new private check.
  - ElementStore: the insert of a new folder writes its child folders and its pending links
    (NewFolderContentIds). The update writes the pending sets, as before.
  - SleeveServiceImpl::ResolveFile, the file branch of DeletePath, AlbumBrowseService::
    ListFilesInFolder(path), AlbumBrowseService::DeleteFiles(paths), AlbumDeleteResult::
    failed_paths_, FileSystem::ListFolderContent(path, write), the file branches of
    FileSystem::Delete(path) and FileSystem::Copy, and the dead declaration
    ElementStore::EnsureChildrenLoaded are removed.
  - mount_tree.cpp: FreeNameForFolder is removed. RemoveFolder reads ListFileIds and unlinks the
    mounted files with one UnlinkFilesFromFolder call. CreateMountRootFolder reads the resident
    child folders of the root.
  - kProjectFileVersion and kMaxSupportedProjectFileVersion are 0.15.0; the format comment
    records the reason.
Decisions made during the work:
  - Step 3 needed no duckorm change: FolderMapper::InsertFolderContents already writes
    `ON CONFLICT DO NOTHING` (primary key (folder_id, element_id)).
  - The cancel rule of the pending sets cannot apply to files: a link of a file that has a row,
    and then an unlink in the same write, would cancel and keep the row. The last-change rule
    above writes the correct rows with no read.
  - ListFolderContent(folder_id) gives the child folders and the file ids, not the child folders
    only (step 7): ReadMountTree needs both kinds, and the ids come with no element load. The
    path form had one caller (ListFolderEntries) and is removed.
  - SleeveServiceImpl::ListFolderEntries(path) stays: it resolves the folder and loads the file
    elements of that folder. Its production caller ListFilesInFolder(path) is removed; tests use
    it to find a file by its folder and name.
  - Step 9 says "archive". The legacy code rule archives whole files; these functions were parts
    of files that stay, so they are deleted. DeletePath and FileSystem::Delete(path) now name a
    folder only; a file leaves by its id (DeleteElement, DeleteFileEverywhere).
  - RenameFile has no folder parameter: with no name rule, the folder had no use.
  - A new file and a duplicate get one reference count increment, as the root link gave before,
    so the stored ref_count of a file stays 1.
  - FileIds and HeldFiles send no statement for an UNSYNC folder: element ids are not reused, so
    a folder with no row has no FolderContent row.
  - The legacy SleeveBase (sleeve_base.cpp, compiled into SleeveFS, no production caller, no
    registered test) still compiles. Its file creation calls AddElementToMap with a file, which
    now throws std::invalid_argument.
  - PlaceFile of mount_tree.cpp unlinks a moved file from each other home folder with
    UnlinkFileFromFolder: one membership read for each moved file. A scan places only moved,
    adopted, and returned files; SN5 changes the mount scan to row queries.
  - The project version range is [0.10.0, 0.15.0], so ProjectVersionIsSupported("0.13.0") is now
    true; no build wrote 0.13.0 or 0.14.0. The tests of a refused newer version use 0.16.0.
  - The test data has no 0.12.0 project file. Version012ProjectOpensAndTheNextSaveWritesVersion015
    saves a project with this build and writes 0.12.0 into its metadata: SN4 changes no table, so
    the database is the one that a 0.12.0 build writes.
  - Tests added beyond the plan: FailedMembershipReadLeavesTheUnlinkUndone (the failure chain),
    MountTreeTest.PlacedFileTakesItsDiskNameWhenTheLibraryHoldsIt and
    NewFolderTakesTheNameOfAFileInItsParent (replace the two free-name tests),
    SleeveFSTest.CopyRefusesAFilePathAndLinksNothing (replaces the file path copy test).
    FolderCopyWriteKeepsNestedFileIdentity now checks that a copy-on-write copy keeps its file.
  - Test names: the fixture of the plan name SleeveServiceTest is SleeveServiceTests.
Explicitly unimplemented items: none of SN4. ReadMountTree still loads each file element of the
  mount (SN5).
Primary success call chain:
  SleeveServiceImpl::LinkFileToFolder(file_id, folder_id)                    (Write, fs_lock_)
    -> FileSystem::LinkFileToFolder -> LiveFile (NodeCache or one element read), LiveFolder
    -> SleeveFolder::RecordFileLinked -> pending link, MODIFIED -> dirty list
    -> SleeveServiceImpl::Sync -> ElementStore::WriteSync -> UpdateElementRows
         -> FolderMapper::InsertFolderContents (ON CONFLICT DO NOTHING)       (one transaction)
  SleeveServiceImpl::DeleteFileFromFolder(file_id, folder_id)
    -> FileSystem::UnlinkFileFromFolder
    -> NodeStorageHandler::HeldFiles -> pending set, else FolderEntryStore::ReadHeldFiles
    -> SleeveFolder::RecordFileUnlinked -> Sync -> FolderMapper::RemoveFolderContents
  FileSystem::ListFileIds(folder_id) -> NodeStorageHandler::FileIds
    -> FolderEntryStore::ReadFolderFiles (one statement) + pending sets      (no element load)
Primary failure and restore call chain:
  FileSystem::UnlinkFileFromFolder -> NodeStorageHandler::HeldFiles
    -> FolderEntryStore::ReadHeldFiles throws (DuckDB error)
    -> no pending set and no flag changed (the read runs before any change)
    -> SleeveServiceImpl::Write returns the exception to the caller; the dirty list is empty
  A build that accepts 0.10.0 to 0.12.0 opens a 0.15.0 project
    -> ProjectVersionIsSupported is false -> the open fails with the version message, so the
       build never loads a root with equal file names
Build and test commands with exit codes (macOS arm64, build/macos-debug, Ninja):
  cmake --build build/macos-debug --target SleeveFSTest SleeveServiceTest NodeCacheTest
    MountTreeTest MountServiceTest AlbumBrowseServiceMountTest ImportServiceTest AlbumQueryTest
    SleeveFilesystemCiTest MountStoreTest AiStorageControllerTest ProjectOpenBenchmark
    ProjectServiceTest ProjectCompatibilityTest ProjectPackageBackendTest CommitGraphTest
    PipelineDocumentCheckpointTest PipelineEditBatchTest EditorMiniGitMaterializerTest
    FilterServiceTest LibrarySearchColumnsTest CiRawWorkflowTest AutomationLibraryCommandsTest
    CollectionsPanelQmlTest LibraryFolderSectionQmlTest AlbumBackendFolderTest
    FolderControllerMountTest alcedo_main -j 8 -- -k 0                         exit 0
  <Target> --gtest_brief=1 for each target below (QT_QPA_PLATFORM=offscreen)     exit 0,
    ImportServiceTest exit 1 (#1156), PipelineDocumentCheckpointTest exit 1 (#553)
  python scripts/check_storage_boundary.py (Windows)                           exit 0
Discovered / passed / failed / skipped counts:
  SleeveServiceTest 45 / 45 / 0 / 0, with SleeveServiceTests.TwoFilesWithEqualNamesLinkIntoOneFolder,
    LinkAndUnlinkInOneWriteLeaveNoRow, UnlinkOfAFileLinkedInTheSameWriteNeedsNoRow,
    RepeatedLinkKeepsOneRow, LinkIntoFolderWithTenThousandFilesLoadsNoFileElement,
    FailedMembershipReadLeavesTheUnlinkUndone, and ProjectWithEqualRootNamesReopensWithAllFiles
  ProjectServiceTest 11 / 11 / 0 / 0, with
    ProjectServiceUUIDTests.Version012ProjectOpensAndTheNextSaveWritesVersion015
  ProjectCompatibilityTest 6 / 6 / 0 / 0 (0.10.0 and 0.11.0 projects save as 0.15.0)
  ProjectPackageBackendTest 7 / 7 / 0 / 0
  SleeveFSTest 13 / 13 / 0 / 0
  MountTreeTest 9 / 9 / 0 / 0
  MountServiceTest 29 / 29 / 0 / 0
  AlbumBrowseServiceMountTest 4 / 4 / 0 / 0
  AlbumQueryTest 39 / 38 / 0 / 1 (the timing profile skips without ALCEDO_ALBUM_QUERY_PROFILE)
  SleeveFilesystemCiTest 2 / 2 / 0 / 0
  MountStoreTest 17 / 17 / 0 / 0
  NodeCacheTest 7 / 7 / 0 / 0
  AiStorageControllerTest 20 / 20 / 0 / 0
  AutomationLibraryCommandsTest 18 / 18 / 0 / 0
  CommitGraphTest 52 / 52 / 0 / 0
  PipelineEditBatchTest 24 / 24 / 0 / 0
  EditorMiniGitMaterializerTest 17 / 17 / 0 / 0
  FilterServiceTest 44 / 33 / 0 / 11 (the RAW batch fixtures are not on the Mac)
  LibrarySearchColumnsTest 15 / 15 / 0 / 0
  CiRawWorkflowTest 3 / 3 / 0 / 0
  CollectionsPanelQmlTest 30 / 30 / 0 / 0, LibraryFolderSectionQmlTest 8 / 8 / 0 / 0,
    AlbumBackendFolderTest 16 / 16 / 0 / 0, FolderControllerMountTest 11 / 11 / 0 / 0; the logs
    have no "FAIL!" and no QtTest compare failure (#526)
  ImportServiceTest 16 / 15 / 1 / 0: ImportServiceTests.BatchCancelTest fails before SN2 too
    (#1156). The run used raw/airplane/_DSC1704.NEF and 70 of the batch_import files, copied to
    the Mac for the run.
  PipelineDocumentCheckpointTest 24 / 21 / 3 / 0: the three Existing*Root* tests fail at
    c68736175 without this change too (#553, comment of 2026-11-26).
  The UI suites, CommitGraphTest, PipelineEditBatchTest, EditorMiniGitMaterializerTest,
    FilterServiceTest, LibrarySearchColumnsTest, CiRawWorkflowTest, ProjectPackageBackendTest,
    AiStorageControllerTest, and ImportServiceTest ran before the final formatting and the batch
    unlink of RemoveFolder; the suites of the changed code ran again after it.
Manual verification: none. The application was not started.
Performance or resource evidence, if required: not required for SN4. A link into a folder with
  10,000 files and the unlink of one of them leave 4 elements in the node cache (the test above).
Evidence path: the counts above. The Mac logs under build/tmp/sn4/ were removed at the end of
  the task.
Remaining defects or unavailable platforms: Windows (MSVC) and Linux not built: the Windows
  machine was in use. BrushSourceFormatBoundaryTest needs ALCEDO_ENABLE_BRUSH_MASK, which is off
  on the Mac; only its version literals changed. AutomationLibraryE2ETest was not run.
  sleeve/sleeve_filesystem.cpp: 747 to 600 lines; sleeve/sleeve_element/sleeve_folder.cpp: 287 to
  213 lines (D7).
```

### SN5: Row queries for the mount scan and the import

**Objective and deliverables.** A scan of a mount with 80,000 files loads no file element that it
does not change.

**Implementation steps.**

1. Add `FolderEntryStore::ReadMountFileFolders(mount_id)`: rows `(file_id, folder_id)` for the
   files of the mount in the folders of the mount.
2. `ReadMountTree` takes its folders from the resident tree and `home_folders` from the query plus
   the pending sets of the mount folders.
3. `PlaceFile`, `RemoveFolder`, and `RemoveMountTree` call `fs.Get` only for a file that they
   change.
4. In `app/import_service.cpp`, keep the two `Write_NoSync` sites. Confirm with a test that a
   batch of n files sends a number of statements that does not grow with the files of the library.

**Tests and evidence.**

- `MountTreeTest.ReadMountTreeLoadsNoFileElement`.
- `MountTreeTest.PlacementOfOneFileLoadsOneFileElement` in a mount with 10,000 files.
- The existing `MountTreeTest` and `MountServiceTest` cases pass.

**Exit criteria.**

- [ ] The tests above pass.

**Completion record.** Use the template of section 9.

### SN6: Measurement

**Objective and deliverables.** Numbers that decide SN7 and SN8, and the owner of the 4 GB.

**Implementation steps.**

1. Extend `tests/app/project_open_benchmark.cpp` with: the open time, the tree read time,
   `NodeCache::Size()` and the resident set size after open, after a browse of 50 folders, and
   after a scan of the mount.
2. Run it on the 80,000-photo project of the user in the debug build and in the release build.
   Follow the measurement rules of the repository for other jobs on the machine.
3. Record the wait of the GUI thread on `fs_lock_` during a mount scan (the time around each
   sidebar read).
4. Record the table sizes of the 1.6 GB project file with `StorageMaintenance`.

**Decision rules.**

- SN7 runs when the nodes use more than 64 MB after the mount scan.
- SN8 runs when one sidebar read waits more than 50 ms.
- A finding about the 4 GB or the 1.6 GB that has another owner becomes its own issue.

**Exit criteria.**

- [ ] The numbers and the two decisions are in the completion record and in #1148.

**Completion record.** Use the template of section 9.

### SN7: Eviction of clean file elements (conditional)

**Objective and deliverables.** `NodeCache` keeps at most the limit of clean file elements.

**Data rules and invariants.**

- A clean file element is in the eviction list. `Find` moves it to the back.
- A folder, a dirty element, and an element with `use_count() > 1` stay. The eviction walk moves a
  referenced element to the back.
- The check of `use_count()` runs under `fs_lock_`. A caller gets a new reference to a cached
  element only under that lock, so a count of 1 stays 1 during the walk.
- `EvictToLimit` runs at the end of `Read`, `Write`, and `Write_NoSync`.
- The limit comes from SN6. It is a constant in `node_cache.hpp`.

**Tests and evidence.**

- `NodeCacheTest.ReadingMoreFilesThanTheLimitKeepsTheCacheAtTheLimit`.
- `NodeCacheTest.ElementHeldByACallerStaysAndKeepsItsIdentity`: the pointer from a second `Find`
  equals the held pointer.
- `NodeCacheTest.ModifiedElementStaysUntilSync`.
- `NodeCacheTest.FoldersStayResident`.
- The memory of the SN6 benchmark before and after goes into the pull request description.

**Completion record.** Use the template of section 9.

### SN8: Sidebar tree read on the query worker (conditional)

**Objective and deliverables.** The GUI thread does not wait on `fs_lock_` for the sidebar tree.

**Rules.**

- The read stays `AlbumBrowseService::ListFolderTree()` through the owner. Its result is the value
  that the function returns. No type stores a second copy of the tree.
- The job runs on `library_->query_worker()`, as the folder counts do. The result goes to the GUI
  thread with a queued call and `FolderController` applies it there.
- A stale-result check (`IsCurrent` with a new `SearchRequestKind`) needs the evidence of
  `AGENTS.md`: the two operations, the two events, and a test that drives the real path. Without
  that evidence the controller applies each result in arrival order.

**Tests and evidence.**

- `CollectionsPanelQmlTest` and `LibraryFolderSectionQmlTest` pass.
- The SN6 wait measurement after the change goes into the pull request description.
- The user confirms in the application that the tree stays usable during a mount scan.

**Completion record.** Use the template of section 9.

---

## 7. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Open a project with 80,000 photos in the root | The tree shows. No file element is in memory. | SN1, SN3 |
| A folder row with no element | The other folders show. One warning names the folder. | SN1 |
| Rename one folder in a large project | The sync writes the changed elements only. | SN2 |
| A sync fails in its last step | The database holds no part of the change. The retry writes all of it. | SN2 |
| A tree of depth 40 | One query at open. Each path resolves from memory. | SN3 |
| A folder edge that closes a cycle | The load skips the edge. One warning names both folders. | SN3 |
| Two photos with one name in one collection | Both are members. | SN4 |
| A project of version 0.12.0 | Opens. The save writes 0.15.0. | SN4 |
| A build before this plan opens a 0.15.0 project | The build refuses the project with the version message. | SN4 |
| Scan of a mount with no change | No file element is loaded. | SN5 |
| An element that a caller holds | It keeps its identity while the caller holds it. | SN7 |

---

## 8. Build, evidence, risks

**Build and run.** Use the debug build and the MSVC wrapper of the `alcedo-msvc-cmake` skill:
`cmd /c "scripts\msvc_env.cmd --build build\debug --target <Target>"` from PowerShell. Run the
named targets of each phase. Do not run the full suite. Confirm that the test list contains each
new test name before a pass is recorded. Put temporary logs under the build directory.

**Risks and stop conditions.**

| Risk | Detection | Response |
| --- | --- | --- |
| A reader of `sync_flag_` or a writer outside `SetSyncFlag` remains | The build fails after the field is private | Change the site in SN2. |
| A caller needs the file entries of a folder as elements | A compile error or a failing test in SN4 | Give the caller `ListFileIds` or a row query. Do not load all elements. |
| duckorm has no insert that ignores an existing row | SN4 step 3 | Add the statement form to duckorm in SN4. Stop when that pushes SN4 over 2000 lines, and split it. |
| The copy-on-write count changes with the tree load | `SleeveFSTest.CoWTest1` and `CoWTest2` fail | The load must not change `ref_count_`. |
| A phase estimate passes 2000 lines | During the work | Stop and split the phase in this plan. |
| SN8 needs a stale-result check with no interleaving evidence | SN8 | Apply results in arrival order. |

---

## 9. Completion record template

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

## 10. Terms and parameters

| Term | Meaning |
| --- | --- |
| Element, node | One `SleeveElement`: a file or a folder with an element id. |
| Link | One `FolderContent` row `(folder_id, element_id)`. |
| Resident | In `NodeCache` for the lifetime of the open project. |
| Dirty | Sync flag `UNSYNC`, `MODIFIED`, or `DELETED`. |
| Clean | Sync flag `SYNCED`. |
| Pending sets | `ContentAddedSinceSync` and `ContentRemovedSinceSync` of a folder. |
| Row query | A store read that returns ids and values and loads no element. |

| Parameter | Meaning | Value | Source |
| --- | --- | --- | --- |
| Page size | Slots in one page of the page table | 4096 slots, 64 KiB | This plan (P4) |
| Node size | Memory of one file element at the audit revision | 400 to 450 bytes | Estimate from the field layout |
| Eviction limit | Largest number of clean file elements in the cache | Set in SN6 | Measurement |
| SN7 threshold | Node memory after the mount scan that starts SN7 | 64 MB | This plan |
| SN8 threshold | Wait of one sidebar read on `fs_lock_` that starts SN8 | 50 ms | This plan |
| Project file version | Version that SN4 writes | 0.15.0, minimum 0.10.0 | User (D9) |
