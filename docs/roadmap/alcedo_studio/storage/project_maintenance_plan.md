# Project Maintenance Feature Plan

Date: 2026-11-09

Updated: 2026-11-27. Ordinary-entry review uses the source records from PR #798.
Source-change marks persist in the project.

Status: Sections 4–12 record PM1–PM5 and their Windows qualification on the previous base.
Section 13 specifies and tracks the current revision.

Source audit: `7bcac45d`.

Related designs: [Mounted Folder Plan](mounted_folder_plan.md) and [Import duplicate skipping](https://github.com/zidage/AlcedoStudio/pull/798).

## 1. Approved product decisions

Add Project Maintenance as a separate application feature.
Its workflow is **Scan -> Review -> Remove selected**.
Users review existing project entries before any removal.

- Add a **Project Maintenance** text button immediately below **Background Tasks** in the top-left
  Collections sidebar. Use one shared dialog and controller. The user-approved UI revision adds the Tabler database-cog icon in the same utility-row layout as Search and Background Tasks.
- Inspect every ordinary photo entry once, including photos with no collection. Use the element's
  empty mount ID. An ordinary collection can link to a mounted photo. The mount owns that photo.
- Offer duplicate removal only when the ordinary entries share a normalized path and their
  recorded fingerprints match the current source. Leave entries with an unknown or changed
  source history for manual review.
- Check the current fingerprint once for each path that has multiple ordinary entries or at
  least one saved source-change mark.
- The review panel has a default-on **Save source-change icons on Library thumbnails** checkbox.
  A completed scan with marking enabled saves confirmed changes. A verified match clears its mark.
- Use the crossed-photo icon on the Library thumbnail and the tooltip **Source changed since
  import**. Show the thumbnail and the icon together. Show saved icons when the project reopens.
- A scan with marking disabled shows the findings. It leaves the saved icons visible.
- Keep every rated or edited entry. This includes inactive Versions, undone history, and additional
  or renamed Versions. A missing source does not remove this protection.
- Identify missing sources separately from permission errors, unreadable paths, and broken project
  references. Do not convert an access failure into a missing-file finding.
- Show full source paths, entry IDs, ratings, editing evidence, and reasons in the review.
  Users select eligible entries and can clear that selection before removal.
- Back up the complete current project before deletion. Verify that backup before proceeding.
- Remove project entries only. Never delete original source images.
- Publish a successful result only after checked removal, package writing, and project reopening.
  Report real failures. Do not continue through a substitute deletion or recovery algorithm.

The user authorized PM1-PM5 implementation on 2026-11-09. This authorization includes targeted
builds, tests, and checks on disposable project copies. It does not authorize another cleanup of Photos.

The earlier proposal for CLI-only cleanup is superseded. CLI support now uses the same application
service as the UI. The earlier Photos operation is completed operational work, not this feature.

## 2. Product specification

### 2.1 Entry point and dialog

The Collections utility rows appear in this order:

```text
Search
Background Tasks
Project Maintenance
```

The button opens the following shared dialog:

```text
Project Maintenance                                      X
Current project name

Scan project
[x] Save source-change icons on Library thumbnails
Scan progress and current phase, when running

Findings: All / Duplicates / Missing sources / Source problems / Project problems
Search full source path or entry ID

Select  Full source path  Entry ID  Rating  Editing work  Finding  Action/reason
[ ]     ...              ...       ...     ...           ...      ...

Select removable duplicates   Select removable missing entries   Clear selection
Selected: N entries

Backup location and status, when a removal is prepared or completed
Cancel / Remove selected
```

Use a text utility row with the same alignment and spacing as the existing utility rows.
Preserve the collection tree. The user approved the Tabler database-cog icon on 2026-11-09, at the existing white 1.5 px source stroke. Add no status dot or decorative badge.
Expose the same dialog from the editor rail below its Background Tasks action so the entry point
remains available when switching workspaces. Both launchers use the same controller and findings.

Use `AppTheme` tokens and existing shared controls. Use `ThemeCheckBox` for row selection and
`DialogActionButton` for actions. Use `AlertBadge` for actual errors. Keep neutral results as plain
text. Give selection a neutral outline or surface treatment from the existing design rules.
Add English and Simplified Chinese strings. Register new QML files in the application CMake list.
Update the applicable `docs/VI/README.md` entries during UI implementation.

### 2.2 States and selection

| State | Required behavior |
| --- | --- |
| No loaded project | Disable the launcher and explain that a project must be opened |
| Project loading or retiring | Disable Scan and Remove selected; show the existing owner reason |
| Ready, no scan | Show Scan project; no implicit scan or deletion |
| Scanning | Show progress and Cancel scan; removal is disabled |
| Scan canceled | Show Canceled; partial findings are informational and cannot be applied |
| No findings | Show No duplicate or invalid entries found; removal stays disabled |
| Review | Show completed findings; only eligible entries have enabled selection controls |
| Preparing removal | Settle conflicting writers; finalize editor work; save and back up |
| Applying removal | Disable selection changes and cancellation; retain the operation task |
| Complete | Show actual removed and retained counts, the backup path, and Refresh scan |
| Failed | Show the failed stage and recovery status; never show a successful removal count |

Start every completed scan with no entries selected. The two explicit selection actions select only
currently eligible findings. Users can clear individual rows or all rows. The service revalidates
selection before mutation. The UI cannot override a rated, edited, or uncertain entry's protection.

For a verified duplicate group, retain every protected entry. When no entry is protected, retain
the earliest import by `Element.added_time`, then the lowest element ID. Show the retained entry
beside removable extras. That retained entry cannot be selected as a removal target. A user can
leave additional extras by clearing their selection. Multiple edited copies stay separate.

A missing, unrated entry with complete proof of no editing work is removable without a survivor.
A missing source with rating or editing work is shown as protected. Permission failures, an
unavailable volume, non-file paths, and ambiguous broken references remain unselected and blocked.
Show their exact error and affected full path. The user can fix the source or project and rescan.

Close hides the dialog during a scan; the task continues in Background Tasks. Reopening returns to
the same task. Cancel scan stops further reads and releases its resources. Closing the dialog does
not approve removal. Once apply begins, closing the dialog hides it but does not interrupt the
backup/publication operation. Project close waits through the existing task shutdown handling.

Tab reaches the launcher, filters, path search, enabled checkboxes, selection actions, and footer
actions in native order. Space toggles focused enabled checkboxes. Enter invokes the focused
button. Escape hides the dialog during scan/review and cancels an unstarted confirmation. It does
not terminate an applying operation. Announce stage and count changes through accessible text.
All paths wrap in an expanded row; a narrow window changes rows to stacked field/value layouts.
Virtualized rows remain bounded. Search and view filters never remove hidden selected IDs.
The confirmation always states the complete selected count, including hidden rows.

### 2.3 Removal confirmation and recovery

Remove selected opens a confirmation containing the actual selected counts by finding kind.
State that original image files remain untouched. State that rated and edited entries are protected.
Confirming starts backup and validation; Cancel changes nothing.

Choose a unique side-by-side backup name such as
`Photos.backup-before-maintenance-2026-11-09-001.alcd`.
Never overwrite an earlier backup. Show its full path when verified.
If the package directory cannot hold a backup, fail explicitly before deletion and let the user
choose another backup directory. Do not silently move the backup into an unrelated cache.

The feature does not add library deletion to image edit Undo/Redo. Existing Undo/Redo and Versions
remain unchanged for retained entries. Recovery of removed entries uses the verified backup through
the normal project-open workflow. A successful removal does not remove that backup automatically.

## 3. Findings, identity, and preservation rules

### 3.1 Identity and first-release scan scope

Use `SourcePathKey` from the source-file utility. Group ordinary entries by their normalized
absolute source path. Use `SourceFile` for the recorded fingerprint and fingerprint version.
Compare these values with the fingerprint of the current file. Size is part of the fingerprint.
A different path gives an independent group.

| Finding | Evidence | Removal eligibility |
| --- | --- | --- |
| Duplicate | Multiple ordinary entries have the same path and each recorded fingerprint matches the current file | Pristine extras only. Keep the designated survivor and protected entries. |
| Source changed | The current fingerprint differs from a comparable recorded fingerprint | Review only. Show each verified mismatch. |
| Source history unknown | A repeated path has an entry without a comparable record, or the fingerprint read failed | Review only. Show what the scan can verify. |
| Missing source | The path is absent on a reachable source volume | Pristine entries only. Recheck the path during removal. |
| Source problem | Access error, unavailable volume, or a path to an unsupported filesystem object | Review only. |
| Project problem | Broken catalog reference or invalid stored history | Review only. |

When a path has several recorded fingerprints, protect the whole group from duplicate removal.
Show the matching row and every changed row. If the file differs from all comparable records,
mark all those entries. A canceled scan and a failed source read give no new evidence for marks.

### 3.2 Proof that an entry has no editing work

Check through the existing data owners. The library's edited indicator is insufficient because it
reads only the active Version's head. A removable entry must satisfy every condition below:

1. Image and element bindings exist and belong to the requested entry.
2. Both `Image.rating` and the metadata `Rating` are zero and agree.
   `Image.color_labels` must be zero and `Image.delete_mark` must be false.
   A color label or deletion mark protects the entry, including when added after scan review.
   Recheck these fields through the same storage inspection before any removal transaction.
3. `CommitGraphStore::LoadGraph` succeeds. Its root has zero stored commits, including unreachable
   or undone commits. Any stored commit protects the entry.
4. The entry has exactly its original Default Version at the empty root head. Additional or
   renamed Versions protect the entry. A stored root head is `''`, not necessarily SQL NULL.
5. Root and checkpoint decoding succeeds through the existing pipeline-document APIs.
   Root element ID, active Version, materialized head, and chain labels agree.
6. Checkpoint and `PipelineParam` documents equal this entry's immutable import root document.
   A difference is editing evidence. Compare each entry with its own root, not another entry's
   root ID. Root identity includes the element ID.
7. No active editor lease, provisional edit, pending journal, or unresolved save can affect it.
8. Removal cannot discard a manual description, rating reason, or other owner-managed annotation.
   Treat uncertain annotation ownership as protected.

An intentional library duplicate can share its Image ID with another element. Keep these bindings
protected during automatic candidate selection. The storage helper must still respect reference
counts if another valid caller removes an unshared entry. Missing or inconsistent evidence always
produces a protected finding; the UI never converts an inspection failure into deletion permission.

### 3.3 Minimal results and lifetime

Add a derived finding representation containing entry ID, source path for display, finding kind,
preservation reasons, rating, commit/Version counts, and an optional retained entry ID.
These are bounded read results. Do not include copied Image, CommitGraph, pipeline documents,
image buffers, or asset blobs. Read those through owners while validating.

`ProjectMaintenanceService` owns scan results until refresh, project change, or service destruction.
The QML model reads those results and owns row selection. Applying produces only selected IDs and
remove/retain ID pairs. It does not write an old report over current state. The report is advisory;
the apply owner rereads every selected entry before mutation.

## 4. Current implementation audit

Paths in this table are relative to `alcedo_studio/`.

| Area | Current owner and source | Observed behavior | Planned change |
| --- | --- | --- | --- |
| Sidebar | `src/ui/alcedo_main/qml/CollectionsPanel.qml:20` | Search and Background Tasks utility rows | Add the icon/text action below Background Tasks |
| Dialog routing | `src/ui/alcedo_main/qml/Main.qml:680`, `src/ui/alcedo_main/qml/AppDialogs.qml` | Main forwards utility signals to shared dialogs | Add one maintenance launcher/dialog |
| Editor rail | `src/ui/alcedo_main/qml/EditorWorkspaceRail.qml:302` | Background Tasks action exists | Add matching text entry to the same shared dialog |
| Background tasks | `src/include/ui/alcedo_main/album_backend/background_task_controller.hpp` | UI-thread RegisterTask/UpdateTask/FinishTask, cancellation, and locks | Add scan/apply kinds; reuse lifecycle |
| Interaction policy | `src/include/ui/alcedo_main/album_backend/interaction_policy_controller.hpp` | Reads existing task locks and explains disabled controls | Extend admission to maintenance conflicts across UI and automation |
| Mutation adapter | `src/include/ui/alcedo_main/album_backend/library_mutation_operations.hpp` | DeleteScope follows the selected folder | Add explicit whole-project maintenance operation |
| Delete owner | `src/ui/alcedo_main/album_backend/image_controller.cpp:873` | Deletes library bindings, history, image data, then saves; several errors are swallowed | Use checked owner helpers for one staged storage transaction |
| Storage | `src/storage/store/sleeve/element_store.cpp` | Owns memberships and element/file persistence | Add focused batch validation/removal with caller transaction |
| History | `src/storage/store/edit_history/commit_graph_store.cpp:555` | DeleteGraphForElement starts its own transaction | Expose transaction-neutral deletion helper for the batch owner |
| Edited SQL | `src/include/storage/store/sleeve/album_edit_state_sql.hpp:29` | Active Version head only | Keep this presentation rule separate from maintenance preservation proof |
| Masks | `src/include/storage/store/mask_asset/mask_asset_store.hpp` | Verifies content-addressed mask rows; caller owns connection | Preserve every asset referenced by retained history |
| Package API | `src/include/app/project_package_service.hpp` | Exports a live database and reads/writes checked packages | Add verified backup and checked publication operations |
| Package publication | `src/app/project_package_backend.cpp:1187` | Writes a temporary package, removes the target, then renames | Add an atomic replacement operation with explicit restore result |
| Project lifecycle | `src/include/ui/alcedo_main/album_backend/project_handler.hpp` | Save/package jobs and close/load owner boundaries | Settle without history GC; activate or restore through these owners |

`PersistProjectForClose` performs unreachable-history cleanup. Do not invoke that cleanup as an
incidental maintenance preparation step. This feature promises to preserve complete retained
history rows. Materialize/save current work through a checked operation that does not run history
garbage collection. After staging succeeds, close the old runtime without saving it again before
opening the published result. Otherwise an ordinary close-save could overwrite the cleaned package.

Relevant registered targets are `SleeveServiceTest`, `PipelineMapperTest`,
`ProjectPackageBackendTest`, `AlbumBackendImageDeleteTest`, `LibraryMutationOperationsTest`,
`AlbumBackendBackgroundTaskTest`, and `AlbumBackendInteractionPolicyTest`.
New owner/controller tests are specified below. Confirm discovery before selecting their cases.

Related plans:

- [Background Tasks and declarative UI state](../ui/background_tasks_ui_state_plan.md): reuse current
  registry/policy owners; confirm source rather than relying on its historical type lists.
- [Album sorting and import time](../ui/album_sort_group_and_import_time_plan.md): preserve survivor
  import time and use full project scope independently of album queries.
- [Library search and project size](library_search_and_project_size_plan.md): preserve the compact
  Image mapping and current storage boundaries.
- [CLI and shared-session automation](../automation/cli_mcp_automation_plan.md): use one application
  operation from UI and commands; expose explicit selection rather than implicit delete-all.

## 5. Target ownership and failure boundaries

### 5.1 Service, controller, and scan

`ApplicationModuleHost` owns both the new `ProjectMaintenanceService` application facade and the
new `ProjectMaintenanceController`. Both survive replacement of the current project. The service
depends on application/storage types, not UI types. ProjectService supplies scoped owner access;
it does not own the maintenance service's lifetime. The service exposes scoped reads, scan
cancellation, and staged apply operations. It owns no independent live Image/history model and
acquires Database connections at operation boundaries.

The controller publishes task state and a paged `ProjectMaintenanceModel` on the UI thread.
Service project bindings and job references exist only while inspecting or staging the current
project. Release those references and close staged connections before activation. The controller
orchestrates ProjectHandler/ProjectLaunchCoordinator activation, then binds the service to the
newly installed owners. Only the controller publishes whole-operation success after activation.

Use bounded database pages for scan enumeration. Release database locks before slow filesystem
reads. Check active editor ownership on its owning thread. Active entries are protected during
scan; apply later finalizes and rechecks them. Scan completion is not a global immutable database
capture. The report records derived findings from scoped reads, and apply performs fresh checks.

Scan success chain:

```text
Collections/Editor rail -> AppDialogs -> ProjectMaintenanceController
  -> BackgroundTaskController registration -> ProjectMaintenanceService worker
  -> scoped storage/history reads and source status -> completed findings
  -> UI-thread model publication -> review selection
```

Scan failure chain:

```text
entry read/status failure -> protected finding with exact reason
fatal owner failure or cancel -> incomplete task -> no apply eligibility -> release scan resources
```

### 5.2 Apply, backup, staged storage, and activation

The apply operation owns one complete maintenance lifecycle:

1. Validate a nonempty selection of completed findings. Refuse a second apply.
2. Reserve conflicting project mutations through the existing task/admission owners. Stop new
   UI and automation writes. Settle already admitted rating, import, delete, export, editor-save,
   semantic/annotation, and package-save jobs before capture. Do not cancel user work silently.
3. Finalize the editor and materialize its journal through its owner. Check every result.
   Save the complete baseline without running history garbage collection.
4. Create a unique complete project backup and verify package checksum, project identity, and a
   content digest. A backup failure stops before any removal.
5. Export/unpack a working package artifact through `ProjectPackageService`. Use Database and its
   storage owners against that staged artifact. Do not start a second live ProjectService or copy
   in-memory Image/history objects. The staged database exists specifically so a failed removal
   or package write cannot partly mutate the active project.
6. Revalidate every selected ID and preservation rule in the staged database. Recheck source
   statuses and retained targets. Validate all pairs before mutation.
7. Run one checked storage transaction for all selected removals and membership changes.
8. Write a new checked package from the staged database. Independently reopen it and check retained
   IDs, ratings, complete histories, masks, reference integrity, and summaries.
9. Under the existing package owner write exclusion, ensure that the original still represents
   the captured baseline. A changed baseline stops publication and leaves the current original
   untouched. Atomically replace the original package with the verified result. Record whether
   this operation actually installed that result and its verified content digest.
10. Close the old runtime without an additional save. Reopen through ProjectHandler and
    ProjectLaunchCoordinator. Release old references before installing the new project owners.
11. Publish actual counts and the backup path only after activation succeeds. Release mutation
    reservation and task locks only after success or completed restore.

The backup and staged package are necessary independent artifacts. An owner operation on the live
database alone cannot undo a later package-write or activation failure. Capture the saved metadata,
database, and required embedded assets only. The verified backup is immutable recovery data and
remains until the user removes it. The working artifact is independently editable only by the
maintenance storage owner, for this operation's lifetime. Discard it after success or completed
restore. Neither artifact is copied back as an old mutable Image/history representation.

Database or validation failure rolls back the whole staged transaction. Backup, staged write,
prepublication verification failure, changed-baseline refusal, or failed atomic replacement leaves
the current original untouched. These outcomes must not restore an older backup over that file.

Restore is permitted only after this operation is known to have installed its verified cleaned
package. Before restoring for an activation failure, the package owner verifies that the current
original still has this operation's published digest. If another writer has changed it, leave that
externally changed file untouched and report recovery required. Keep the backup and staged evidence.
An unknown publication outcome also requires explicit recovery rather than an assumed restore.

When the current original is still the package installed by this operation, the package owner's
explicit restore operation can restore the verified backup, then the controller reopens that
baseline. A restore/reopen failure reports both errors, keeps conflicting writes disabled, and
exposes the backup path. It must not publish success or open another project as a substitute.
Retain required recovery artifacts and report the real failure. Release task/admission locks after
success or completed restore; keep conflicting writes disabled while recovery remains unresolved.

### 5.3 Storage operation and batch invariants

Add a focused operation accepting only removal IDs and optional retained IDs. It owns one
connection, the Database lock, and one transaction in the staged database.

- Removal IDs are unique and nonzero. No retained ID appears anywhere in the complete removal set.
- Every duplicate pair belongs to one normalized full-path group. Missing-source removals have
  no retained target. Reject self-pairs, chains, cycles, missing targets, and protected entries.
- Validate the complete batch before changing any row. A bad selection removes nothing.
- Union removed duplicate entries' album memberships into the nominated survivor before deletion.
  The `(folder_id, element_id)` key prevents duplicate memberships. Recompute affected reference
  counts through the owning mapper. Missing-entry removal has no membership transfer.
- Remove dependent Element, FileImage, PipelineParam, VersionRef, ImageEditState, and PipelineRoot
  rows through transaction-neutral owner helpers. Handle EditCommitAttribution with its commits.
- Keep every retained root, Version, commit, checkpoint, rating, annotation, and mask reference.
  Delete an Image only after its last FileImage reference is gone. Keep shared mask assets unless
  their owner proves that no retained pipeline or history refers to them.
- Remove semantic/AI rows for removed entries through their owners. If the import feature's
  optional ImageSourceSize table exists, the last-Image-reference removal owner also removes
  its unused size row. That integration belongs to the import feature's storage owner; maintenance calls
  the ordinary Image removal operation and acquires no import feature dependency. Do not add
  independent SQL copies of those owners.
- Publish no notifications until the transaction and package activation are complete.

### 5.4 Executable concurrency cases

Do not add generation counters, report epochs, or a second cancellation protocol.
Use existing cancellation for user-requested scan cancellation and existing operation admission.
Cover these real current-path orderings:

1. Scan reads pristine entry E. After the worker queues the finding to the UI, the editor or rating
   owner changes E. Apply must finalize current work and reject E on fresh owner validation.
2. A rating worker is already admitted when Apply begins. If Apply captures before that worker
   commits and the worker writes afterward, the backup or deletion would omit current work.
   Apply must wait for that admitted writer and refuse new writes through both UI and automation.
3. An asynchronous package-save job is already running through MakeSaveAndPackageJob. Maintenance
   must settle that job and share package-owner write exclusion before capture/publication.
4. Another saved package replaces the captured source before publication. Test a real package writer
   at that boundary. Detect changed baseline before replacement and fail explicitly, keeping the
   backup and staged result. Never overwrite a newly observed package with an older staged one.

Drive tests through these owner/executor boundaries, not by injecting impossible revision values.
Project close cancels/waits for scan and waits for applying tasks through the existing shutdown
owners, so a queued result cannot publish into a destroyed project controller.

## 6. File and API map

All source paths below are relative to `alcedo_studio/`. The completion records map the proposed APIs to implemented owners.

| Area | Current files that change | Proposed files and responsibility |
| --- | --- | --- |
| Full path identity | Domain CMake files | `src/include/utils/file/source_image_path.hpp`, `src/utils/file/source_image_path.cpp`; shared comparison utility |
| Application service | `src/include/app/project_service.hpp`, `src/app/project_service.cpp` for focused owner access if required; app CMake | `src/include/app/project_maintenance_service.hpp`, `src/app/project_maintenance_service.cpp`; host-owned facade with scoped project bindings |
| Library storage | `src/include/storage/store/sleeve/element_store.hpp`, `src/storage/store/sleeve/element_store.cpp`, associated mappers | Focused removal operation and transaction-neutral helpers |
| History/assets | CommitGraphStore, MaskAssetStore, annotation and image stores | Reuse existing owners; add caller-transaction helpers where required |
| Package/lifecycle | `src/app/project_package_service.cpp`, `src/app/project_package_backend.cpp`, defining headers; ProjectHandler/ProjectLaunchCoordinator | Verified backup, staged publication, checked restore, and activation |
| UI owner | `src/ui/alcedo_main/album_backend/application_module_host.cpp` and header; task/policy controllers | `src/include/ui/alcedo_main/album_backend/project_maintenance_controller.hpp` and implementation; host owns service/controller and coordinates activation |
| Review model | UI CMake | `project_maintenance_model.hpp/.cpp`; derived finding rows and selection |
| QML | CollectionsPanel, EditorWorkspaceRail, Main, AppDialogs, application CMake | `ProjectMaintenanceDialog.qml`; shared review workflow |
| UI documentation | `src/ui/alcedo_main/DESIGN.md`, `docs/VI/README.md`, translations | Existing tokens and per-file decisions; no new palette |
| Automation | `automation/commands/project_commands.cpp`, task tracker and command tests | `project.maintenance.scan`, paged results, and explicit remove-selected command |

Proposed APIs:

| API | Input and result | Owner and mutation behavior |
| --- | --- | --- |
| `ScanProjectMaintenance` | Current project owner, cancellation/progress callbacks -> completed findings or explicit error | Application service; read-only project inspection |
| `VisitMaintenanceFindings` | Offset/count/filter -> const derived result page | Service/model; bounded result lifetime until refresh/project change |
| `InspectMaintenancePreservation` | Element ID -> protection reasons and minimal counts | Existing live/history owners; no mutable internals exposed |
| `RemoveSelectedProjectEntries` | Span of removal IDs and optional retained IDs -> staged operation outcome | Application service; validates, backs up, and prepares/publishes through package owner; controller completes activation and final task result |
| `RemovePristineProjectEntries` | Minimal validated change descriptions -> row counts | ElementStore; same transaction as related owner helpers |
| `CreateVerifiedProjectBackup` | Saved package path and selected backup directory -> backup path/digest | Package owner; never overwrites an existing backup |
| `PublishVerifiedMaintenancePackage` | Checked staged package and baseline/backup identities -> explicit publication outcome and installed digest | Package owner; no UI types or activation. Controller publishes final success after lifecycle activation |

Use defining headers rather than new forward declarations. Value types contain only new derived
results or the minimal change descriptions. Include span/optional and defining owner headers at
their actual use sites. Add no long-lived DuckDB connections to new storage controllers.

## 7. Phase summary

| Phase | Result | Main modules | Dependency | Expected changed lines | Status |
| --- | --- | --- | --- | ---: | --- |
| PM1 | Full-path scan and preservation classification | Shared utility, app service, storage/history reads | None | 900-1,500 | complete on Windows |
| PM2 | Sidebar launcher, background scan, paged review and selection | Controller/model/QML/task policy | PM1 | 1,100-1,800 | complete on Windows |
| PM3 | Checked whole-project storage removal transaction | Element/Image/history/asset stores | PM1 | 1,100-1,900 | complete on Windows |
| PM4 | Verified backup, staged publication, activation and restore | Package/service/project lifecycle | PM2, PM3 | 1,000-1,800 | complete on Windows |
| PM5 | UI/automation parity and product qualification | Commands, caller tests, translations, docs | PM4 | 700-1,300 | complete on Windows |

Each estimate includes production source, tests, build registration, resources, and documentation.
PM3 is a storage capability tested on isolated fixtures. The product Remove selected action remains
disabled until PM4 supplies the complete indivisible backup/publication lifecycle. Split a phase
before implementation if its expected diff can exceed 2,000 lines. The features share only neutral
path comparison; import detection is not a prerequisite for any maintenance phase.

Before implementation, reread AGENTS.md, CONTRIBUTING.md, `alcedo-create-plan`, `alcedo-qml-ui`, and
`alcedo-msvc-cmake`. The execution records below use the current source and the parent plan under the subsequent implementation authorization.

## 8. Detailed phases

### PM1 - Full-path scan and preservation classification

**Objective and deliverables:** Add a read-only service that returns complete, bounded findings.
Prove the distinction between repeated full paths, missing sources, and protected work.

**Inputs and prerequisites:** Current source; shared path utility if already delivered by import.
Otherwise introduce that utility here with the same API. No source-size database is required.

**Modules, files, and APIs:** Proposed ProjectMaintenanceService and result definitions; shared path
utility; ElementStore scoped reads; CommitGraphStore reads; source-status utility; app CMake.

**Data rules and invariants:** Apply sections 3 and 5. Read complete graph preservation evidence.
Keep filesystem I/O outside database locks. Store derived findings only. Scan writes no project row.

**Implementation steps:**

1. Add/reuse NormalizeSourceImagePath with platform-aware full-path tests.
2. Enumerate live project bindings in bounded pages through storage owners.
3. Classify source status without treating permission or volume errors as file-not-found.
4. Inspect every preservation rule through live and persisted owners.
5. Choose survivors and removable extras deterministically. Validate all result IDs.
6. Return completed findings with exact protected/error reasons and bounded paging.

**Primary success call chain:** service -> scoped owners -> full-path groups/status -> findings.

**Primary failure and restore call chain:** invalid per-entry evidence -> protected finding;
fatal owner failure/cancel -> incomplete scan with no apply result. Existing state is unchanged.

**Tests and evidence:** Add proposed `SourceImagePathTest` and `ProjectMaintenanceServiceTest`.
`DifferentFoldersWithEqualNamesRemainSeparate` asserts separate groups, including equal-byte files.
`EquivalentWindowsSlashSpellingsShareOnePath` asserts one group without dropping directories.
`RatedUneditedEntryIsProtected`, `InactiveVersionAndUndoneCommitsProtectEntry`, and
`RenamedVersionProtectsEntry` assert disabled removal eligibility.
`MissingSourceWithEditingWorkRemainsProtected` verifies intersecting reasons.
`PermissionAndUnavailableVolumeAreNotMissingSources` asserts report-only results.
`BrokenReferenceProtectsItsEntry` and `PipelineDifferenceProtectsEntry` assert no removable ID.
`ScanLeavesAllProjectRowsUnchanged` compares ordered cell digests before/after.

**Build and run commands:** Use section 10; build SourceImagePathTest and
ProjectMaintenanceServiceTest. Discover and run their cases only.

**Exit criteria:** [x] Every finding has an exact reason. [x] All preservation/path/error tests pass.
[x] Scan creates no database, history, or source-file mutation. [x] Result paging stays bounded.

**Expected diff:** 900-1,500 lines, including tests and source registration.

**Completion record (2026-11-09):** Implemented on Windows.

- `ProjectMaintenanceService::Scan` -> `ProjectMaintenanceStore::ListEntryIds/Inspect` ->
  normalized full-path groups and source checks -> completed advisory findings. Storage reads use
  64-entry pages; the UI and command output use 100-row pages. No RAW decode is required.
- Invalid references and uncertain source status become protected findings. Cancellation leaves
  `Complete()` false, so no partial selection can reach package work.
- `ProjectMaintenanceServiceTest`: 12 cases cover deterministic survivors, directory identity,
  rating, renamed/inactive Versions, undone commits, missing/error paths, pipeline differences,
  invalid references, unchanged table digests, cancellation, and bounded enumeration.
  The existing `SourceImagePathTest` supplies platform path identity coverage.
- The existing full-path utility is reused. Source availability has a separate focused utility;
  maintenance inspection belongs to `ProjectMaintenanceStore`, rather than the large ElementStore.

### PM2 - Background scan, launchers, review, and selection

**Objective and deliverables:** Expose the approved UI and selection flow. Keep removal unavailable
until PM4 is complete. Users can scan, inspect, cancel, hide, reopen, and select eligible findings.

**Inputs and prerequisites:** PM1 completed findings and owner errors; current QML design rules.

**Modules, files, and APIs:** Proposed ProjectMaintenanceController/Model/Dialog; ApplicationModuleHost;
CollectionsPanel; EditorWorkspaceRail; Main/AppDialogs; BackgroundTaskController; interaction policy.

**Data rules and invariants:** The host owns the controller across project reloads. The service owns
findings; the model owns selection. Rows cannot enable protected entries. Partial results cannot apply.

**Implementation steps:**

1. Add task kinds and route scan progress/cancel callbacks to the existing registry on the UI thread.
2. Add the two launchers and one shared dialog route. The subsequent approved UI revision uses the Tabler database-cog SVG.
3. Implement the paged model's display roles, filters, selected IDs, and protected reasons.
4. Implement the explicit selection actions and total selected count across filters/pages.
5. Implement empty/error/canceled states and hide/reopen behavior.
6. Add keyboard/accessibility names, wrapping, theme tokens, translations, QML registration, and VI records.

**Primary success call chain:** launcher -> shared controller -> service/task -> model -> review.

**Primary failure and restore call chain:** owner failure/cancel -> task terminal state and disabled
apply; project change clears results/selection after pending scan callbacks have settled.

**Tests and evidence:** Add proposed `ProjectMaintenanceControllerTest` and `ProjectMaintenanceModelTest`.
`MaintenanceLauncherFollowsBackgroundTasks` asserts action order and shared dialog routing.
`ReviewCannotSelectRatedOrEditedRows` asserts disabled selection through model APIs.
`HiddenSelectedRowsRemainInConfirmationCount` asserts selection survives filtering/paging.
`ScanCancelLeavesApplyDisabled` and `DialogReopenShowsCurrentScan` assert task continuity.
`ProjectChangeReleasesOldResults` checks owner lifetime and no late publication.
Manually check both themes, keyboard navigation, long paths, and narrow-window row reflow.

**Build and run commands:** Build the proposed controller/model targets plus
AlbumBackendBackgroundTaskTest and AlbumBackendInteractionPolicyTest. Use targeted discovered cases.

**Exit criteria:** [x] All approved review states work. [x] Protected selection is rejected in C++.
[x] Background task cancellation/close behavior passes. [x] Manual VI/accessibility record exists.
[x] Remove selected requires the complete PM4 operation.

**Expected diff:** 1,100-1,800 lines.

**Completion record (2026-11-09):** Implemented on Windows.

- Collections or editor rail -> shared `ProjectMaintenanceDialog` -> application-owned controller
  -> background scan -> model reset and bounded review. Hiding the dialog does not own or cancel
  the worker. Selection is stored as IDs and survives filtering and paging.
- Protected/incomplete/read-only rows reject selection in C++, including direct calls. Project close
  cancels and waits for a scan through the existing close coordinator. Errors have no removal count.
- `ProjectMaintenanceModelTest` covers protected rows, hidden selection, incomplete scans,
  unsigned 32-bit IDs, and independent command paging. Controller tests cover no project,
  scan lifetime, deterministic cancellation, and close during scan.
- Native appearance and keyboard evidence is recorded with PM5 in section 11.

### PM3 - Whole-project checked storage removal

**Objective and deliverables:** Add a focused atomic storage operation for eligible IDs, independently
of the currently selected album. Exercise it only through isolated storage fixtures at this phase.

**Inputs and prerequisites:** PM1 preservation rules and minimal change descriptions.
The staged package lifecycle does not exist yet, so the UI cannot call this mutator directly.

**Modules, files, and APIs:** ElementStore, related mappers, ImageStore, CommitGraphStore, annotation
owners, MaskAssetStore, source-size storage owner; proposed RemovePristineProjectEntries.

**Data rules and invariants:** One operation-boundary connection/lock and one transaction.
Validate every pair before mutation. Preserve all protected rows and survivor-owned data.

**Implementation steps:**

1. Validate unique removals, same-path pairs, retained targets, complete preservation evidence, and source status.
2. Refactor existing transaction-owning deletion helpers to provide caller-transaction operations.
3. Union duplicate album memberships and recompute affected reference counts.
4. Delete only the reviewed bindings and dependent owner rows.
5. Preserve shared Images/assets and every retained root, Version, commit, checkpoint, and annotation.
6. Commit once and return exact removed IDs/counts; propagate the first real owner error after rollback.

**Primary success call chain:** staged service -> ElementStore transaction -> owner helpers -> exact IDs.

**Primary failure and restore call chain:** invalid batch -> zero mutations; injected owner failure
after partial statements -> transaction rollback -> unchanged isolated database.

**Tests and evidence:** Add proposed `ProjectMaintenanceStorageTest`.
`SelectedAlbumDoesNotLimitProjectRemoval` checks whole-project removal and all memberships.
`DuplicateRemovalTransfersMembershipsWithoutDuplicates` asserts membership union and ref counts.
`RatedOrEditedSelectionRejectsWholeBatch` asserts no other selected row is deleted.
`RemovalChainsCyclesAndRepeatedIdsChangeNothing` covers A->B->C and A->B->A.
`LastImageReferenceControlsImageRemoval` verifies shared ownership.
`RetainedHistoryAndMasksMatchBeforeRemoval` compares complete serialized rows and asset bytes.
`InjectedOwnerFailureRollsBackAllRelatedTables` injects failure after memberships/history change.
`MissingEntryRemovalRequiresPristineProof` checks no survivor is needed but all protection remains.

**Build and run commands:** Build ProjectMaintenanceStorageTest, SleeveServiceTest, PipelineMapperTest,
and AlbumBackendImageDeleteTest. Run targeted affected cases and direct callers using section 10.

**Exit criteria:** [x] Batch validation is complete before mutation. [x] All rollback/ref-count tests
pass. [x] No history or image state is copied back over a live owner. [x] The storage operation alone does not enable a UI mutator.

**Expected diff:** 1,100-1,900 lines.

**Completion record (2026-11-09):** Implemented on Windows.

- Explicit ID/retained-ID pairs -> complete source/preservation validation -> one storage transaction
  -> union album memberships -> existing history/annotation/image owners -> nominated entry removal.
- Any bad ID, protected entry, invalid survivor, chain, cycle, or dependent-owner failure rejects
  or rolls back the whole batch. Only nominated unreferenced Images are removed. Mask assets stay.
- `ProjectMaintenanceStorageTest`: 9 cases cover project scope, membership union, protected batch
  rejection, malformed pairs, invalid retained references, actual dependent-row failure rollback,
  missing-source proof, last Image reference, and complete retained history/mask row digests.
- Transaction-neutral history and Image removal helpers remain with those owners. The annotation
  owner is called once per batch because its full-text index rebuild cannot run twice in one
  transaction. No persisted schema change is introduced.

### PM4 - Verified backup, staged publication, activation, and restore

**Objective and deliverables:** Enable one complete user removal operation with checked backup,
staged database mutation, atomic package publication, and owner replacement or verified restore.

**Inputs and prerequisites:** PM2 review/controller and PM3 storage operation. Required live owner
admission and checked save APIs must be added within this phase before enabling Remove selected.

**Modules, files, and APIs:** ProjectMaintenanceService/Controller; ProjectPackageService/backend;
ProjectHandler/ProjectLaunchCoordinator; existing task/admission and shutdown owners.

**Data rules and invariants:** Apply section 5.2 in order. Preserve complete history without cleanup
GC. Independent artifacts have the documented purpose/lifetime. No second live owner model exists.

**Implementation steps:**

1. Extend shared owner admission for both UI and automation; settle existing conflicting jobs.
2. Finalize and save current work without history GC; verify a unique immutable backup.
3. Create the staged artifact through package APIs and use its Database/storage owners for PM3.
4. Revalidate selected rows/statuses after capture; fail the entire batch if protection changed.
5. Write and independently reopen the result; compare retained rows and reference integrity.
6. Add checked atomic publication instead of remove-target-then-rename behavior.
7. Close the old runtime without resaving it; activate through the existing project lifecycle.
8. Restore/reopen only if this operation installed the current verified package. Leave a changed
   baseline or externally changed published file untouched; hold recovery when ownership is uncertain.
9. Publish actual counts and release locks only after the complete outcome is known.

**Primary success call chain:** confirmation -> settle/save -> verified backup -> staged transaction
-> verified package -> atomic replacement -> old-owner release -> reopen -> result/unlock.

**Primary failure and restore call chain:** backup/staging/prepublication failure -> current original
unchanged and no restore; verified installation followed by activation failure -> confirm current
installed digest -> restore -> baseline reopen -> error/unlock. An externally changed current file
or unknown installation result -> leave current file untouched -> recovery required. Restore failure
-> explicit recovery error, retained artifacts, disabled writes, and backup path. Never report
backup-only success.

**Tests and evidence:** Add proposed `ProjectMaintenanceApplyTest` and extend ProjectPackageBackendTest.
`BackupFailurePerformsNoRemoval` asserts all package and database bytes remain.
`ExistingBackupIsNeverOverwritten` checks unique names and verified contents.
`AlreadyAdmittedRatingFinishesBeforeCapture` drives the actual worker/admission ordering.
`NewAutomationWriteIsRejectedDuringApply` checks shared admission rather than only QML state.
`ReviewThenEditPreventsSelectedRemoval` checks revalidation after a real settled edit.
`StagedWriteFailureLeavesOriginalPackageIntact` and `AtomicReplaceFailureRetainsOriginal` verify failure boundaries.
`ChangedBaselineNeverTriggersBackupRestore` asserts the external writer's bytes remain unchanged.
`ExternallyChangedPublishedPackageIsNotOverwrittenDuringRecovery` changes the installed package
before injected activation failure and asserts no backup restore touches the changed file.
`ActivationFailureRestoresVerifiedBackup` checks project identity and preserved rows after reopening.
`RestoreFailureReportsBothErrorsAndKeepsWritesDisabled` asserts no false success.
`OldRuntimeCannotResaveOverPublishedMaintenance` covers the close/open save ordering.
`ConcurrentPackageSaveIsSettledBeforeMaintenance` and `ChangedBaselineStopsPublication` drive real writer boundaries.
`SuccessfulMaintenancePreservesCompleteHistoryAndReopens` compares retained rows, including undone work on protected entries.

**Build and run commands:** Build ProjectMaintenanceApplyTest, ProjectPackageBackendTest, and the
affected project lifecycle/task targets. Run targeted discovered cases. Run package/filesystem
replacement cases on Windows and macOS; report an unavailable platform honestly.

**Exit criteria:** [x] Backup exists and verifies before every mutation. [x] Failure injection covers
each publication/activation boundary. [x] Retained-row and reopen evidence passes. [x] Shared admission
blocks conflicting UI/automation writes. [x] The UI enables only the complete operation.

**Expected diff:** 1,000-1,800 lines. Split before execution if lifecycle changes can exceed 2,000.

**Completion record (2026-11-09):** Implemented on Windows.

- Confirmation -> controller reserves writes and settles existing tasks/editor -> handler package
  mutex -> save current work -> verified unique backup -> independent working database transaction
  -> retained-row verification -> independently reopened result -> atomic replacement -> close old
  owners without saving -> normal project launch -> UUID/digest verification -> final counts/unlock.
- Before publication, failure leaves the captured saved baseline intact. After verified installation,
  activation failure -> confirm installed digest -> restore verified backup -> reopen baseline ->
  explicit failed outcome. Externally changed bytes or failed restore leave writes disabled and
  expose the backup, without overwriting the observed file.
- `ProjectMaintenanceApplyTest`: 9 cases cover success/reopen/repeat, backup failure, result-write
  failure, unique backup, changed baseline, restoration, externally changed installed package,
  review-then-rating rejection, and a real Windows file-sharing replacement failure.
- Controller tests drive actual activation, restoration failure, already-admitted rating work,
  shared UI/automation write refusal, and external replacement during activation. The package owner
  serializes normal save jobs and maintenance under the same mutex. Activation is the success boundary.
- Windows filesystem behavior is executed. The equivalent macOS code is not executed here.

### PM5 - Commands, caller coverage, and real product qualification

**Objective and deliverables:** Complete UI and command parity. Qualify selected removal on copied
real projects and record persistence, cancellation, accessibility, and performance evidence.

**Inputs and prerequisites:** PM4 complete operation and all earlier owner tests passing.

**Modules, files, and APIs:** Automation project commands/task tracker; maintenance model/controller;
translations, CMake registrations, VI documentation, and direct caller test fixtures.

**Data rules and invariants:** Commands use the same application service and explicit selected IDs.
No command implicitly selects/removes all findings or bypasses preservation/backup.

**Implementation steps:**

1. Add scan, paged result, and remove-selected command schemas with the shared service.
2. Connect task results and exact cancellation/error states to UI and command clients.
3. Exercise scan-only, user selection changes, empty selection, protected selection, and repeat scans.
4. Verify retained history, masks, album memberships, and package counters after reopen.
5. Review UI appearance, keyboard access, long paths, both themes, and narrow windows.
6. Record bounded pages/memory and scan/apply stage durations on a representative library.

**Primary success call chain:** UI or command -> shared service -> completed outcome -> consistent result.

**Primary failure and restore call chain:** schema/selection/owner failure -> exact rejection or PM4
restore result; application and command task states agree.

**Tests and evidence:** Extend AutomationProjectCommandsTest and the maintenance owner/model targets.
`MaintenanceCommandsCannotBypassProtectedRows` asserts the same protection result as UI calls.
`ScanCommandDoesNotMutateProject` checks ordered row digests.
`RemoveSelectedCommandRequiresExplicitIds` rejects empty and implicit-all requests.
`SecondMaintenanceScanFindsNoRemovedDuplicateOrMissingEntries` checks a successful repeat run.
`TaskResultCountsEqualActualRemovedRows` compares published counts with reopened storage.
`ProjectCloseWaitsForApplyAndCancelsScan` exercises both shutdown policies.
Manual checks must include selecting while a different album is open and keeping hidden selected
rows visible in the final confirmation count. Use copies of user data; never alter Photos for a test.

**Build and run commands:** Build alcedo_main, alcedo_cli, AutomationProjectCommandsTest, and the
maintenance targets. Run targeted command/owner/caller cases and the manual checks in section 10.

**Exit criteria:** [x] UI and commands have identical preservation/removal behavior. [x] Empty,
cancel, failure, recovery, and repeat-run checks pass. [x] Manual VI record and actual counts exist.
[x] No imported detection feature or full test suite was required to qualify maintenance.

**Expected diff:** 700-1,300 lines.

**Completion record (2026-11-09):** Implementation and qualification are recorded in section 11.

- `project.maintenance.scan`, `project.maintenance.findings`, and
  `project.maintenance.remove_selected` use the same service/controller as the dialog. Results have
  explicit schemas. Removal requires reviewed IDs; paging is bounded and does not change UI state.
- Schema/selection failure -> explicit rejection before backup; owner failure -> PM4's verified
  recovery outcome. No command can select protected findings or implicitly remove all findings.
- The in-process command integration imports real raster fixtures, scans without row changes,
  rejects protected/empty requests, removes one explicit entry, reopens, saves, and scans again.
  Invalid numeric IDs are rejected by the controller as well as command validation.
- English and Simplified Chinese dialog/controller strings, QML/CMake registration, and VI notes
  are included. Detailed owner diagnostics retain their original error text.

## 9. Acceptance matrix

| Behavior | Expected observable result |
| --- | --- |
| Equal filenames in different folders | Separate groups; neither entry is removed for that reason |
| Equivalent full-path spelling | One group with full directory identity preserved |
| Unrated repeated import with no work | Eligible extra; designated existing-path survivor stays |
| Rated-only, edited-only, or rated-and-edited entry | Protected in both UI and service |
| Inactive Version or undone commit | Protected; complete retained history survives reopen |
| Missing source with protected work | Shown as missing and protected; cannot be selected |
| Missing pristine source | Eligible only after completed scan and explicit user selection |
| Permission failure or disconnected volume | Report-only source problem; not a missing-removal candidate |
| Broken DB/history/asset reference | Exact finding and protection; no guessed ownership repair |
| Selected album differs from project root | Whole-project scope and all relevant memberships are used |
| Source/status/rating/history changes after review | Fresh validation rejects unsafe selected operation |
| Batch contains chains, cycles, or a bad ID | Zero removal across the entire batch |
| Owner transaction failure | Staged rollback; active package and backup remain intact |
| Backup, prepublication, or failed atomic-replacement operation | No success; current original unchanged and no backup restore |
| Changed baseline before publication | Refuse publication; never restore the older backup over the new file |
| Published file changed externally before recovery | Leave current file untouched; preserve backup and report recovery required |
| Activation/restore failure | Exact error, preserved backup, and disabled conflicting writes |
| Existing/overlapping task or automation writer | Shared admission settles/refuses it deterministically |
| User cancels scan or hides dialog | No deletion; task/result behavior matches section 2 |
| Successful removal and reopen | Exact selected IDs gone; retained rows/assets unchanged |
| Repeat scan and removal | No removed IDs reappear; empty selection changes nothing |

## 10. Build and evidence rules

Plan creation runs no product build, test suite, or project operation.
The commands below are for implementation. Proposed test targets must be registered first.

```powershell
Set-Location C:\Users\Haidong\Data\Photography\AlcedoStudio
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ProjectMaintenanceServiceTest ProjectMaintenanceStorageTest ProjectMaintenanceApplyTest --parallel 4
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -N -R 'ProjectMaintenance|SourceImagePathTest'
ctest --test-dir build/debug --output-on-failure -j 1 -R 'ProjectMaintenance|SourceImagePathTest'
```

Each phase also builds its named direct-caller targets. Select actual discovered cases when a
target's CTest prefix differs from its target name. Use only win_debug/win_release on Windows and
the repository wrapper. Allow at least 10-20 minutes per build invocation; retain healthy builds.
Do not run a build and test concurrently in one build directory. Add the vcpkg runtime bin path.
Check copied runtime DLLs if an unchanged test executable appears to load old owner behavior.

On macOS use the fixed macos_debug directory and configure ALCEDO_BUILD_TESTS=ON when needed.
Verify source path comparison, filesystem errors, package replacement, and restore on that platform.
These checks require no GPU decode or render. Report unavailable platforms and skipped checks as such.
Never start the full test suite unless the user explicitly requests it. Do not use unreliable
WorkspaceShellTest synthetic input as the acceptance basis. Prefer C++ owner/model/command tests
and record manual QML behavior and appearance separately.

Use build/tmp/project_maintenance/<phase>/ for temporary logs and evidence. New files use LF.
Apply the separate line-ending conversion rule before editing existing CRLF source files. Include
the defining headers, preserve direct owner mutation, and format only changed existing lines.
Review all new/touched terminology and the full roadmap filename set before phase completion.

Before implementation, search existing GitHub items and record required issues under CONTRIBUTING.md.
The parent coordination plan records current issue/authentication status. An absent issue link is
not implementation evidence. Do not post AI follow-up conversations on existing issue threads.

### Completed Photos operation: evidence context only

The user separately approved and completed the one-time Photos cleanup on 2026-11-09:

- Removed 4,778 reviewed entries; 2,547 remained.
- Preserved all 70 rated and 97 edited entries, 4,080 commits, 2,645 Versions, and 47 mask assets.
- Compared complete retained row/asset digests and independently checked references and full paths.
- Reopened the cleaned package in Alcedo before publishing it.
- Preserved `C:\Users\Haidong\Documents\Alcedo\Photos.backup-before-cleanup-2026-11-09.alcd`.

That operation used the existing application deletion API on an isolated working package, followed
by independent validation and atomic replacement. It required operational orchestration that the
new feature must own. It does not mark PM1-PM5 implemented, and it does not replace failure-injection
or UI coverage. Do not rerun destructive operations on Photos to qualify this feature.

## 11. Completion record

### Implementation decisions (2026-11-09)

The implementation starts at `e128d588` on `de_duplicate`.

- `ProjectMaintenanceService` owns derived scan findings and validates explicit selection.
  `ProjectMaintenanceStore` owns the scoped storage inspection and the atomic removal operation.
  This keeps the existing large ElementStore file from absorbing maintenance inspection rules.
- `ProjectMaintenancePackage` owns the independent backup, working database, verified result,
  publication digests, and checked restoration. ProjectHandler supplies the existing package
  write exclusion. The application-owned controller completes activation before it reports success.
- The saved baseline starts after current work is saved without history cleanup. A new DuckDB
  export can change package bytes without changing logical rows. Failure tests compare the package
  with this captured baseline. They also compare live rows and verified backup contents.
- The annotation owner rebuilds its full-text index. The storage transaction calls it once for
  the complete removal batch. Rebuilding its stopword table twice in one transaction failed in
  the executed multi-entry removal test.
- Invalid retained references reject the complete removal batch. A group with an invalid
  protected copy retains an additional valid entry when needed. All protected copies remain.
- Activation checks the project UUID and the installed or restored package digest. An external
  package replacement between publication and activation produces recovery-required state.
  Restoration never overwrites that externally changed package.
- The permitted fork, `ClearSights/AlcedoStudio`, reports administrator permission and
  `has_issues=false`. Issue tracking is unavailable there. No upstream write or AI issue
  conversation is used as a substitute.

### Executed Windows verification (2026-11-09)

Source: uncommitted implementation on `de_duplicate`, based on `e128d588`.
The existing `win_release` build has tests enabled. No new build tree was created.
MSVC 14.44 matches the existing cache; Qt 6.12.0 is installed on this machine.

```powershell
Set-Location C:/Users/Haidong/Data/Photography/AlcedoStudio
$env:VCVARS_USER_VERSION = '14.44'
cmd /c scripts\msvc_env.cmd --build --preset win_release --target ProjectMaintenanceServiceTest ProjectMaintenanceStorageTest ProjectMaintenanceApplyTest ProjectMaintenanceModelTest ProjectMaintenanceControllerTest alcedo_main alcedo_cli alcedo_studio_test_host --parallel 4
$env:PATH = "$PWD/build/release/vcpkg_installed/x64-windows/bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
& build/release/alcedo_studio/tests/app/ProjectMaintenanceServiceTest_runtime/ProjectMaintenanceServiceTest.exe
& build/release/alcedo_studio/tests/app/ProjectMaintenanceStorageTest_runtime/ProjectMaintenanceStorageTest.exe
& build/release/alcedo_studio/tests/app/ProjectMaintenanceApplyTest_runtime/ProjectMaintenanceApplyTest.exe
& build/release/alcedo_studio/tests/ui/ProjectMaintenanceModelTest_runtime/ProjectMaintenanceModelTest.exe
& build/release/alcedo_studio/tests/ui/ProjectMaintenanceControllerTest_runtime/ProjectMaintenanceControllerTest.exe
```

The build and all five binaries exited 0. The final maintenance run passed **47/47** cases.
The controller's 12 cases ran together, including activation restoration, failed restoration,
command integration, numeric ID rejection, and external replacement during activation.

| Executed target | Passed | Skipped | Evidence goal |
| --- | ---: | ---: | --- |
| ProjectMaintenanceServiceTest | 12 | 0 | Full-project read-only classification and preservation |
| ProjectMaintenanceStorageTest | 9 | 0 | Atomic batch removal and retained rows |
| ProjectMaintenanceApplyTest | 9 | 0 | Backup, publication, replacement failure, restoration, reopen |
| ProjectMaintenanceModelTest | 5 | 0 | Protection, paging, hidden selection, full ID range |
| ProjectMaintenanceControllerTest | 12 | 0 | Async lifecycle, admission, commands, actual activation/recovery |
| ProjectPackageBackendTest | 7 | 0 | Existing package callers and persisted package contents |
| SourceImagePathTest | 3 | 1 | Platform full-path identity; symlink privilege unavailable |
| AlbumBackendBackgroundTaskTest | 18 | 0 | Existing task policies and lifecycle |
| AlbumBackendInteractionPolicyTest | 15 | 0 | Existing mutation restrictions |
| ApplicationCloseCoordinatorTest | 5 | 0 | Existing project/application close behavior |
| LibraryMutationOperationsTest | 5 | 0 | Existing mutation owners |
| SleeveServiceTest (selected) | 11 | 0 | Project/album file identity and persistence |
| PipelineMapperTest (selected) | 8 | 0 | Root, checkpoint, history, mask persistence and deletion |
| AlbumBackendImageDeleteTest | 9 | 3 | Existing deletion owner; RAW fixture absent for three cases |
| AutomationProjectCommandsTest | 6 | 0 | Existing project command behavior |
| **Total** | **134** | **4** | **0 failures in the recorded qualification runs** |

Direct callers were built and executed before the final model/query and QML-only adjustments.
Their source was unchanged by those adjustments. Whole caller binaries ran unless the table says
selected. The exact selected filters were:

```text
SleeveServiceTest --gtest_filter=*SaveLoadTest:*CreateFileInLibraryAndLinkToAlbumKeepsSingleFileIdentity:*SharedAlbumFileEditIsVisibleFromRootAndOtherAlbums:*DeletingAlbumFolderKeepsLinkedLibraryFiles:*ExplicitDuplicateClonesStateAndKeepsPipelineIndependent:*ReloadedRootMembershipKeepsLibraryFile:*ReloadedAlbumMembershipKeepsSharedFileIdentity:*ReloadedAlbumUnlinkKeepsRootMembershipOnly:*DuplicateLinkIsIdempotentAndPersistsAsSingleMembership:*DeletingFromAlbumOnlyUnlinksMembership:*DeletingFromRootDeletesFileEverywhereAndPersists
PipelineMapperTest --gtest_filter=*NewImageRootHoldsTheDefaultThreeNodeDocument:*EditorPersistedEditSurvivesProjectReopen:*CheckpointReloadPreservesNodesEdgesAndParameters:*HeldEditorLeaseRefusesStorageHistoryUsersAndReleaseKeepsThePublishedState:*DeletePipelinesRemovesTheDeletedImagesMiniGitGraphOnly:*RootStateRejectsDifferentImageOwner:*PersistEditorHistoryWritesHistoryAndCheckpointInOneTransaction:*MaskAssetsReaderReadsStoredAssetAndReportsMissingKey
```

No full test suite was run. WorkspaceShell synthetic-input tests were not used. macOS path,
filesystem replacement, and native UI checks were not executable on this Windows machine.
No GPU quality policy changed.

### Disposable-project qualification

The original `C:/Users/Haidong/Documents/Alcedo/Photos.alcd` was read and copied only.
Its SHA-256 before and after qualification was:
`8AB7F9C8524858E8587BEC9AD534B7681ACA76818071A6A74FB1C09829EBFB4C`.

- Initial read-only scan: 2,575 entries, no findings, 46.2 seconds; process private memory
  850.8 MiB and peak working set 478.9 MiB. The scan does not decode full RAW images.
- On the disposable copy only, import two copies of one test raster path with duplicate skipping
  explicitly disabled. Scan 2,577 entries: exactly two duplicate findings, one removable extra,
  30.2 seconds. The 100-row output limit remains enforced by the model and command schema.
- Remove that one explicit extra through `project.maintenance.remove_selected`: verified backup,
  retained-row digest comparison, independent result reopening, atomic publication, and normal
  activation all succeeded. Actual result: one entry removed, 2,576 retained; 38.6 seconds including
  reopen. No source image was deleted.
- Repeat scan: no duplicate or invalid findings. `library.list` independently reported 2,576 entries.
  The test session was then stopped. These timings include warmed filesystem and database caches;
  they are observations, not a throughput requirement.
- A separate disposable project contains 106 entries: 105 uses of one long Unicode source path,
  one rated survivor, and one deliberately missing source. Native review verified 104 eligible
  duplicate extras and one eligible missing entry, with 105 selected across a search hiding all rows.
  The explicit confirmation reported 104 duplicates and one missing entry. Cancel and Escape
  performed no removal. Space toggled an individual checkbox. The editor launcher reopened the
  same selection and search after switching from Library.

### Native visual review and limitations

The QML-only rebuild of `alcedo_main` and `alcedo_studio_test_host` exited 0. Native review
used the real Main QML module in the isolated test host, with the disposable project only.

- Reviewed the ready, scan, duplicate, missing-source, filtered, selected, and confirmation states.
  The final dialog and menu were inspected in both Alcedo and Classic themes at 1200 x 760.
- The filter now reuses `AdjustmentCombo`; the search uses explicit theme text/placeholder/selection
  colors. This corrects the Basic style's light filter and unreadable placeholder seen during review.
  Both controls have accessible names. The rail button has no default padding, so Project and
  Maintenance each fit on one line at the existing caption size.
- Rescanning with an empty collection selected still returned all 106 project findings. Protected
  rows remained unselected. Canceling confirmation preserved the source files and project entries.
- Scan cancellation, close during scan, backup failure, activation failure, and recovery-required
  state are proven by owner/controller tests. They were not all repeated as native visual states.
- Attempts to resize the native test host left it at 1200 x 760. The 960 x 640 appearance therefore
  remains unverified; this is a coverage limit, not a claim that the narrow layout passed.
- The 12 controller cases passed three additional consecutive runs (`--gtest_repeat=3`), 36 case
  executions with zero failures. These repeats are not added to the unique-case total above.
- Entering the empty editor emitted `Cannot find member data` from the untouched
  `EditorTonePanel.qml:227` and `:290`. No maintenance QML loading or binding error was observed.
  The permitted fork has issues disabled, so an issue could not be filed for this separate warning.
  No upstream issue or AI follow-up comment was sent.
- All local test sessions were stopped. Temporary logs, copied packages, test backups, and synthetic
  sources under `build/tmp/project_maintenance/` were removed after recording this evidence.
  The original Photos checksum remained unchanged.

The new source modules remain separate by owner: scan service, storage inspector/removal operation,
package operation, UI controller/model, and command registration. No new source file exceeds 419
lines before the final documentation update. Existing large CMake, translation, Collections, and
package-backend files received focused edits; no unrelated large-file split was required. Shared
files contain work from several phases and are counted once in the overall diff.
Initial PM1-PM5 recorded delta: 4,380 lines (1,066 tracked additions, 77 tracked deletions, and 3,237 lines in 20 new files), across 56 files. The largest new source file is 419 lines.

The permitted fork has issue tracking disabled, so no issue number could be created for the
multi-entry annotation-index defect fixed during implementation. The failure and its correction
are recorded above. There is no unresolved observed owner-test failure. Temporary qualification
artifacts belong under `build/tmp/project_maintenance/` and are removed after test sessions stop.

### UI revision requested after manual testing (2026-11-09)

The user approved the specific Tabler database-cog asset, its 24 x 24 viewBox, white stroke, and
1.5 px normalization. The path geometry and MIT notice remain with the source asset.

- The Collections actions now share one delegate, so icon/text edges and row centers cannot drift.
  The editor launcher uses the existing icon rail control.
- The dialog close action reuses Search's compact transparent X. Header/body/footer margins,
  filter/search heights, metadata flow, selection columns, paging, and final action alignment
  now follow the existing dialog controls.
- Removal shows the same ring and working outline as Import. Store progress reports scalar
  processed/total values before commit; package code forwards them only after verified backup.
  The controller queues bounded UI updates without publishing a final removal count. Package and
  activation stages use a waiting ring. Success still requires verified activation; errors retain
  the existing restoration and write-exclusion behavior.
- New storage tests exercise progress counts with real removal, and rollback when an observer
  fails. Existing package/controller success tests now assert backup-before-progress and no final
  removed count before activation. Build, targeted test, and native review results follow below.


**UI revision verification:** The existing `win_release` build completed with exit 0 for
`ProjectMaintenanceServiceTest`, `ProjectMaintenanceStorageTest`, `ProjectMaintenanceApplyTest`,
`ProjectMaintenanceModelTest`, `ProjectMaintenanceControllerTest`, `alcedo_main`, `alcedo_cli`,
and `alcedo_studio_test_host`, through `scripts/msvc_env.cmd` with MSVC 14.44 and parallelism 4.
The same five direct test binaries were then executed with the release vcpkg bin directory on PATH
and `QT_QPA_PLATFORM=offscreen`: 12 service, 11 storage, 9 package, 5 model, and 12 controller cases
passed, **49/49**, with no failures or skips. No full test suite was run.
The storage additions verify actual processed counts and rollback on observer failure. The package
and controller assertions verify a checked backup before progress and no final removal count until
activation. Resource/translation XML parsed; approved SVG geometry matches upstream exactly.

A separate disposable project was opened in the native test host. The Windows desktop was locked,
so app input stopped before visual review. At that point native appearance and live progress inspection
were pending desktop unlock; the completed review is recorded below. The provided `maintenance_manual_test` project files were not changed
by this verification. The disposable native test host was closed and temporary build logs/trial copies were removed after recording this evidence. The provided synthetic projects and launcher remain available for user testing.


**Native review resumed after unlock (2026-11-09):** The rebuilt test host opened a new disposable
`build/tmp/maintenance_ui_native/VisualMaintenance.alcd` copy. At 1200 x 760 in Alcedo theme,
Search, Background Tasks, and Project Maintenance share the same icon column, label edge, row
height, and vertical center. The approved icon rendered correctly. The compact X closed the review;
reopening retained the completed 106-finding scan. Full paths, the metadata flow, filter/search
fields, three equal selection actions, paging, selected count, backup location, and confirmation
footer rendered without clipping or new QML errors. Selecting eligible duplicate and missing
entries produced 105 selected entries; the four-star survivor remained unselected. The final
confirmation reports 104 duplicates and one missing entry on the disposable copy only.
The user confirmed the disposable removal. The native progress card displayed the backup stage,
working outline, waiting ring, and `0 / 105` processed count. The operation then reopened the project
and displayed 105 removed, one retained, and a verified backup path. The four-star survivor remained
visible in the Library. A repeat scan found no duplicate or invalid entries. The Classic theme's
empty-result controls and alignment were also inspected at 1200 x 760. No new maintenance QML
error was observed. The narrow-window limit recorded above is unchanged.

Native editor review found that the `Maintenance` caption split inside the fixed rail. The caption
was shortened to `Maintain`, with `Project Maintenance` kept as the accessible name and tooltip;
the approved icon remains the standard rail icon. The fork's disabled issue tracker prevents an
issue for this visual defect; its evidence and correction are recorded here. The final caption
build and native confirmation are recorded below.


**Final native completion (2026-11-09):** The QML/translation-only caption rebuild of `alcedo_main`
and `alcedo_studio_test_host` exited 0. Native inspection confirmed that `Maintain` fits on one rail
line and the accessible action remains `Project Maintenance`. The already-cleaned project reopened
with its four-star survivor. No C++ behavior changed after the recorded 49-case pass, so those
owner tests were not repeated for a caption change. The test host was closed normally.

The provided project files stayed unchanged throughout native qualification:
`SyntheticMaintenance.alcd` SHA-256 `E8A77725C5B94082ADF34DE4826D7C780A568496712C4C2F950B40342B7DED80`;
`SyntheticMaintenance.initial.alcd` SHA-256 `CECA00B07A3F6EE0E7F8CFDF100924312D2D0948D921FDBA5D9E5C444152014A`.
Only the disposable copy received the 105 removals. Its backup and temporary logs/copies were
removed after recording the results. The supplied synthetic projects and launcher were retained.

## 12. Mark preservation qualification (2026-11-10)

Color labels and deletion marks were added after the initial maintenance implementation.
`ProjectMaintenanceStore::Inspect` now reads both fields in its existing evidence query.
Either mark protects an entry. Scan, staged-package revalidation, and direct transactional removal
use that same owner operation. No extra per-entry query or copy of Image state was introduced.

New evidence passed on Windows:

- `ColorLabelsAndDeletionMarksProtectDuplicateAndMissingEntries`: neither marked duplicate nor
  marked missing-source entry is removable or selectable.
- `MarkAddedAfterReviewRejectsTheWholeRemovalBatch`: either mark added after scan rejects all
  requested removals; Image, Element and pipeline-root rows remain unchanged.
- `ReviewThenColorLabelPreventsSelectedRemoval` and
  `ReviewThenDeletionMarkPreventsSelectedRemoval`: package revalidation refuses the changed selection.
- `SuccessfulMaintenancePreservesCompleteHistoryAndReopens`: the retained Image keeps its exact
  color-label mask and deletion mark after removal, package publication and reopen.

All 53 selected maintenance cases passed: service 13, storage 12, package 11, model 5, controller 12.
The Release app rebuilt successfully. Shared import/rendering checks and exact commands are in
[ordinary import verification in PR #798](https://github.com/zidage/AlcedoStudio/pull/798).
The full suite and macOS execution were not run. No PR or commit was created for this revision.

## 13. Ordinary entries and saved source-change marks

**Prerequisites.** Merge upstream main and PR #798. Read `AGENTS.md` and the plan, QML, and
Windows build skills before implementation. Audit base `f758bf5a7`.

**Source audit.** The original maintenance service grouped entries by path and checked whether
the path was readable. It held findings in memory. The mounted-folder scan now owns mounted
reachability and the stored missing state. PR #798 supplies the shared source measurements and
the path normalization utility. The Library already draws the crossed-photo icon for a missing
thumbnail source.

**PM6A. Ordinary-entry classification and checked removal.** Estimate 850 changed lines.

1. Restrict enumeration and removal to ordinary entries in `ProjectMaintenanceStore`. Keep
   report-only diagnostics for broken catalog references. Reject a mounted entry at apply.
2. Read the recorded fingerprint from the source owner. Measure each qualifying path once
   through `ComputeSourceFileFingerprint`. Keep access errors and missing paths distinct.
3. Use the match rules in section 3.1. Pass only explicit IDs into staged removal. Recheck the
   path, comparable fingerprints, survivor, and preservation evidence before deletion.
4. Remove the source row and its saved mark in the deletion transaction. Include them in the
   retained-row verification.

**PM6B. Save and display source-change marks.** Estimate 750 changed lines.

1. Store the mark as project data keyed by the ordinary element ID. Keep the source measurement
   and the mounted missing state in `SourceFile`. A project without saved marks starts empty.
2. The maintenance worker collects verified mark updates during the scan. Publish one storage
   operation after a complete, enabled scan. Use the existing project save and package operation.
   Report a storage or package failure. Show the saved marks after the project opens.
3. Use `ThemeCheckBox` in the review panel. The Library uses the existing crossed-photo asset
   with a distinct tooltip and accessible description. The thumbnail stays visible. A disabled
   scan still computes the review findings.
4. Refresh the Library marks after the storage operation. The update keeps the existing
   thumbnail request path.

**Call chains.** Scan action → controller worker → maintenance service → ordinary entry query,
source checks, fingerprint read → findings → optional mark update → project save → Library
refresh. Remove selected → verified backup → staged storage validation → deletion and retained
row verification → package activation. A canceled scan emits no mark updates. A failed apply
uses the existing restore path.

**Verification.** Exercise ordinary scope, mounted refusal, matching and unknown histories,
different fingerprints at one path, a replaced source shared by two entries, and removal after
a source changes. Verify mark persistence across reopen, review-only scans, and mark clearing
after recheck of a single marked entry. Build the five Project Maintenance test targets and
the Windows Release app with `scripts/msvc_env.cmd --build --preset win_release`. Run the
targeted Maintenance, import classification, and mount service cases. Record results and
manual verification separately.

**Completion record (2026-11-27).** Merged upstream main at `728947418` and PR #798 at
`65127f0ac` before implementation. The `SourceFileChanged` table stores only element IDs.
`SourceFileStore` checks the baseline and ordinary ownership before it writes a mark. It
removes marks when it replaces or deletes the baseline. The controller saves the project
when an enabled scan changes a mark. The Library reads the saved IDs when the project opens
and when verified results change. The automation scan remains a read-only review.

The Windows Release app built successfully. All 110 targeted tests passed. The suites were
`ProjectMaintenanceServiceTest` (19), `ProjectMaintenanceStorageTest` (13),
`ProjectMaintenanceApplyTest` (11), `ProjectMaintenanceModelTest` (5),
`ProjectMaintenanceControllerTest` (13), `ImportContentClassificationTest` (20), and
`MountServiceTest` (29). The controller test verified the marks in the saved package,
review-only behavior, and clearing a mark after a verified match. The storage boundary and
C++ thread API checks passed. Native visual verification remains pending.

The Windows recovery test exposed an intermittent replacement error. The same atomic operation
now retries that error after it verifies both file digests. The restore case passed six
consecutive reruns. [Restore issue](https://github.com/ClearSights/AlcedoStudio/issues/7)
