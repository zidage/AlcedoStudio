# Library Photo Stacking Plan

Date: 2026-11-11

Status: PS1, PS2A, and PS2B complete (2026-11-11). All other phases remain planned.
The latest Windows Release verification passed 93 targeted tests, skipped three, and failed none.
One GPU rendering comparison was excluded. See the PS2A and PS2B completion records.

Feature request: [#486](https://github.com/zidage/AlcedoStudio/issues/486), stacking section only.

Parent plan: none. This file is the master plan and the detailed phase plan.

Last source review: 2026-11-11.

Source audit: upstream `main` at `e59529c66d2d0bfdc53c5c5cf2d88e6e68d9138d`.
A fresh upstream fetch confirmed this revision. Local `main` and the planning branch
`feature/library-photo-stacking` have the same base commit.
The plan and its two roadmap entries were committed as `4830980d` before PS1 implementation.
The source audit below describes the implementation base before PS1.

This plan has no dependency on `feature/project-maintenance` or
`feature/import-duplicate-skipping`. Their unmerged APIs, documents, and test targets are absent
from this audit. Implement stacking directly on the audited main revision.

Primary owners: project storage, `PhotoStackService` (proposed), `AlbumBrowseService`,
`LibraryModule`, and the Library presentation models.

## 1. Decision record

### 1.1 Product goal and approved behavior

Let users organize related Library photos into a persistent stack.
A collapsed stack occupies one photo position. Users can expand it to see its matching members.
The feature supports burst photography and other sets of related photos.
It does not combine image pixels.

The user approved these rules on 2026-11-11:

1. Apply all photo filters before stack presentation.
2. Show a stack when at least one member matches the complete filter.
3. Show only matching members when that stack expands.
4. Use a temporary cover when the saved cover is outside the matching portion.
5. Let one saved stack span several Library groups.
6. Show each group's matching portion inside that group.

The discussion also established these requirements:

- A temporary cover must not replace the saved cover in storage.
- Clearing a filter restores the full eligible stack and its saved cover.
- Expansion state survives a temporary filter change.
- Counts must distinguish matching photos from the full stack.
- Ordinary photo actions must not silently include non-matching members.

### 1.2 Design choices selected by this plan

These choices fill details that the issue does not specify. They are part of this proposed design.
They are not claims of separate user approval.

- Stack membership belongs to the project. It uses logical File IDs, not source paths or Image IDs.
- A File belongs to at most one saved stack. Stacks cannot contain other stacks.
- A saved stack has at least two members, one saved cover, and a stable member order.
- An album shows the intersection of its own files and the stack. Album links do not copy stacks.
- A displayed portion is called a **stack fragment**. It is the matching members in one group.
- Expansion is a session setting for each stack fragment. It is not project data.
- New stacks start collapsed. Each occurrence of a stack in a different group expands independently.
- A single matching member still shows its stack control and full-stack count.
- All three Library layouts support stacks: classic grid, justified rows, and grouped classic cells.
- The editor filmstrip and existing automation photo lists retain their complete matching photo set.
- Stack changes are Library operations. They do not add image-edit commits or change image pixels.
- Automatic stacking, image merging, and new public stacking automation commands are outside this request.

### 1.3 Rejected behavior

Do not filter only the saved cover. Do not admit every member because one member matches.
Do not flatten all stacks whenever a filter is active.
Do not move a whole stack into the saved cover's group.
Do not change the saved cover, member order, album membership, or image metadata during a read.

Adobe Bridge documents cover-dependent filtering for collapsed stacks.
Lightroom Classic users report the same search limitation.
Those products explain the comparison; they do not define Alcedo's selected behavior.
Sources: [Bridge filtering](https://helpx.adobe.com/bridge/desktop/organize-and-find-files/organize-files-and-folders/sort-and-filter-files-and-folders.html),
[Lightroom Classic report](https://community.adobe.com/questions-675/lightroom-classic-not-finding-photos-in-closed-stacks-977686).

## 2. Product specification

### 2.1 Filter, group, and stack order

Let `F` contain the unique live Files in the current root or album scope.
Let `P(f)` be the complete existing search and Inspector predicate for File `f`.
Let `M = {f in F | P(f)}`.
Use the current field-search and natural-language relevance rules to compute `M`.
An unqualified natural-language result must not enter a stack fragment.

For saved stack `S` and Library group `g`, define:

```text
matching stack members:       M(S)    = members(S) intersect M
matching fragment members:    M(S,g)  = M(S) intersect members(g)
flat mode:                    one group containing M
display the fragment:         M(S,g) is not empty
displayed cover:              saved cover, if it belongs to M(S,g)
                              otherwise the earliest member of M(S,g) in saved member order
temporary cover:              displayed cover differs from saved cover
```

Each File must satisfy the complete predicate itself.
A Monday photo that uses Lens A and a Tuesday photo that uses Lens B do not make the stack match
`Monday AND Lens B`.
Keep the existing OR rules within one filter category and AND rules across categories.

The complete read order is:

```text
root or album scope
  -> complete photo predicate and semantic relevance restriction
  -> unique matching Files and Inspector statistics
  -> group membership, including repeated semantic-label occurrences
  -> stack fragments inside each group
  -> fragment cover and item order
  -> expansion projection
  -> bounded display page
```

Compute the fragment before applying page limits.
A page must not decide whether a stack exists or which photo becomes its temporary cover.

### 2.2 Temporary covers and multiple groups

The following example is required acceptance data:

| File | Saved order | Saved cover | Capture day | Camera | Lens | Labels |
| --- | ---: | --- | --- | --- | --- | --- |
| A | 0 | Yes | Monday | Canon | Lens A | bird |
| B | 1 | No | Tuesday | Nikon | Lens B | bird, flight |
| C | 2 | No | Tuesday | Canon | Lens A | flight |
| D | 3 | No | Wednesday | Nikon | Lens B | none |

| View | Required stack presentation |
| --- | --- |
| No filter, flat | One fragment, A as saved cover, four members. |
| Tuesday filter, flat | One fragment, B as temporary cover, B and C on expansion. |
| Monday AND Lens B | No matching fragment. |
| Capture-day grouping | Monday: A; Tuesday: B and C with temporary cover B; Wednesday: D with temporary cover D. |
| Camera grouping | Canon: A and C with cover A; Nikon: B and D with temporary cover B. |
| Label grouping | bird: A and B; flight: B and C; unknown: D. Each fragment uses only its own members. |
| Album containing C and D | One flat fragment with temporary cover C. A and B remain members of the saved stack. |

The same File can appear in several label groups.
Those occurrences share photo selection and image state. They have separate fragment identities.
Do not count their repeated appearances as additional project photos.

Changing the filter, grouping, semantic model, sort direction, or album can change the displayed cover.
It must not issue a stack write.
An explicit `Set as stack cover` command changes the saved cover project-wide.
Other fragments then compute their own displayed cover again.

### 2.3 Sorting and expansion

Keep the existing group order and unknown-group placement.
Order stack fragments and unstacked photos with the existing photo comparator applied to their
displayed photo. This includes explicit sort direction and natural-language relevance order.
Use the displayed File ID as the final tie breaker.
The displayed photo belongs to the group, so the comparator cannot read a cover from another group.

An expanded stack stays at the same item position as its collapsed fragment.
Show its displayed cover first. Show its other matching members in saved member order.
Expansion therefore keeps stack members adjacent. It does not promise a global photo sort within
an expanded stack. The sort tooltip must state this behavior.

Key fragment expansion by project session, album scope, grouping field, typed group key, and stack ID.
Use a tagged flat key. Do not confuse flat mode with the unknown group.
Keep absent keys while filters hide them. Release all keys on project close.
Returning to a previous album or grouping in the same session restores its expansion choices.
`Expand all stacks` and `Collapse all stacks` affect fragments in the current matching result only.
They do not expand or collapse Library group headers.

### 2.4 Counts

Keep these meanings separate:

| Value | Meaning |
| --- | --- |
| Library total | Unique matching Files in `M`. |
| Group photo count | Matching photo occurrences in that group, before stack collapse. |
| Stack total | All live members of the saved project stack. |
| Stack query match count | Unique members in `M(S)`, before repeated label membership. |
| Fragment match count | Members in `M(S,g)`. |
| Display item count | Cards after applying stack expansion. Used for layout and display paging only. |

Collapse must not change Inspector statistics, collection counts, export totals, or photo totals.
Do not replace a photo count with a card count.

The disclosure control shows the fragment match count.
Its tooltip states the full stack total and the query match count when they differ.
At normal thumbnail sizes, use separate caption roles for `2 matching here` and `8 in stack`.
Do not join independent counts with a slash or a centered dot.
At compact sizes, keep the disclosure count and expose the other values through its tooltip and
accessible description.

### 2.5 Selection, actions, and editor navigation

- A normal card click selects the displayed File only. It does not select the entire stack.
- Clicking the disclosure changes expansion only. It does not change photo selection or open the editor.
- Ctrl-click changes one photo's selection. Shift selection follows displayed cards and deduplicates File IDs.
- Ctrl+A selects every unique matching File, including members of collapsed stacks and collapsed groups.
- `Select matching photos in this stack` selects `M(S,g)` for the clicked fragment.
- `Select entire stack` is an explicit project-wide action. It can include files outside this result.
  Show its total before application. Show the count outside the current result in the selection summary.
- A filter or scope replacement intersects the existing selection with the new match set.
  Ordinary collapse does not clear an explicit photo selection.
- Rating, labels, deletion, export, adjustment paste, and collection linking use the explicit selection.
  They never expand a selected cover into all stack members.
- Deduplicate targets by logical File ID, including repeated label-group occurrences.
- Double-click and Enter open the actual displayed photo, including a temporary cover.
- The editor filmstrip lists all unique matching photos in the existing photo-query order.
  Library collapse does not hide filmstrip photos or change existing automation list counts.
- Returning from the editor reveals the actual File in its preferred group.
  If that matching File is hidden by stack collapse, expand that fragment before revealing it.
  If it is outside the filter, report that fact. Do not substitute another member or clear the filter.

Visible selected counts must include selected members hidden by collapse.
Keep that count separate from selected photos outside the query after explicit whole-stack selection.
Selection remains owned by `LibrarySelection`.

### 2.6 Stack commands

Put these text actions in the existing Library image context menu:

| Command | Preconditions | Effect |
| --- | --- | --- |
| Group selected photos | At least two unique selected Files; a valid selected cover | Create one stack from exactly those Files, in their current displayed order. |
| Add selected photos to this stack | Valid clicked target stack; at least one selected File outside it | Append the selected Files in displayed order. Keep the target stack's saved cover. |
| Remove selected photos from stacks | At least one selected stack member | Remove only the selected memberships. Keep the photos in the project and albums. |
| Ungroup entire stack | Valid clicked stack | Remove that complete stack's organization. Keep every photo and album link. |
| Set as stack cover | Clicked File belongs to the saved stack | Persist that File as the saved cover. |
| Move earlier in stack / Move later in stack | Selected single member has an adjacent saved member | Swap their member positions. Filtering does not limit the saved neighbor search. |
| Select matching photos in this stack | Clicked fragment has matches | Read and select its matching unique Files. |
| Select entire stack | Valid clicked stack | Read and explicitly select every live member. |
| Expand / Collapse stack | Valid clicked fragment | Change only that fragment's session expansion. |

Choose the clicked selected File as the cover of a new stack.
If no clicked selected File exists, use the first selected File in displayed order.
Deduplicate selected Files by their first displayed occurrence.
Place explicitly selected Files outside the result after those displayed Files.
Order those additional Files by their saved stack ID and member position, then by File ID.
Place unstacked additional Files last, in File ID order.
If the selection has no displayed File, use the first File in this defined order as its cover.
If all targets already form exactly one stack, Group selected photos changes nothing.
Otherwise, moving selected members into a new or existing stack repairs each source stack in the
same transaction. It does not move unselected members.
Treat duplicate target IDs as one target. Reject a missing File or invalid cover before any write.

After removal, dissolve a saved stack with fewer than two members.
If its cover was removed, choose the earliest remaining saved member as the new saved cover.
That promotion is a persistent membership repair, not a temporary display cover.

Ungroup entire stack requires confirmation when the current result hides some members.
The confirmation states the full member count and that it changes every fragment of that stack.
Set as stack cover and member-order actions have project-wide meaning in their tooltips.
Do not imply that they affect only the current group.

These commands do not use image-edit Undo or Redo. Do not insert Library state into Mini-Git.
The inverse organization commands remain available. The UI must not advertise an unavailable undo.

### 2.7 Visual and input design

Use the existing Basic style and Library monochrome design.
Reuse the existing disclosure text glyphs and standard text actions. No new SVG is required.
Use a compact standard disclosure control, not a new rounded count badge.
It stays reachable at every zoom level, including the caption-free level.

```text
Tuesday                                      2 photos

+----------------------+       expanded:
| [>] 2                |       +----------------------+ +----------------------+
|                      |       | [v] 2                | |                      |
|  thumbnail B         |       |  thumbnail B         | |  thumbnail C         |
|                      |       |                      | |                      |
+----------------------+       +----------------------+ +----------------------+
B                              B                        C
2 matching here                2 matching here
4 in stack                     4 in stack
```

Map dimensions to existing card spacing, `inspectorHeaderActionSize`, and typography tokens.
Use `uiFontFamily` for labels and the current numeric typography rule for counts.
Map all fills, outlines, focus, and animation durations to `appTheme`.
If a new token is necessary, add it to AppTheme and DESIGN.md together.
Register a shared proposed `PhotoStackDisclosure.qml` in `ALCEDO_MAIN_QML_FILES`.
Use it on classic cards, justified tiles, and grouped cells.
Keep the count visible without overlapping the existing edit, HDR, missing-source, and Delete marks.

The disclosure has an accessible name such as `Expand stack, 2 matching photos`.
Its accessible description gives the total and temporary-cover state.
Space and Enter activate a focused disclosure. A focused photo retains existing Enter behavior.
Expose stack commands through the existing context-menu keyboard path.
Do not assign new global key chords without checking the shortcut registry.
Do not add a separate toolbar or a second selection owner.

Preserve the first shown occurrence during expand, collapse, zoom, and temporary-cover replacement.
Use real item positions when restoring scroll. Do not introduce new uses of estimated ListView
row offsets. Existing scroll work in [#612](https://github.com/zidage/AlcedoStudio/issues/612) remains relevant.
Use existing motion durations. `reduceMotion` makes the transition immediate.

### 2.8 Loading, failure, and save behavior

Keep the last accepted result while a query updates.
Use the current Updating and No Matching Photos states.
A query failure keeps the accepted result and offers Retry through `LibraryModule`.
It must not display a flat substitute or a successful empty result.

Validate commands through the interaction policy and again inside the storage operation.
Do not update membership optimistically in QML or in a copied stack object.
Publish the complete storage result, then refresh the accepted Library query.

A database transaction failure leaves all affected stack rows unchanged.
Return the actual validation or storage error.
A later metadata-save or package-save failure has a different result: the database operation
has committed, but the package has not saved the change.
Requery and show the committed working state. Report `Stack changed, but project save failed`
with the owner's diagnostic and a Retry Save action.
Add a pending-stack-save condition to ProjectModule. This condition does not exist on audited main.
Keep it set until a successful project save or an explicit project discard.
Do not roll back a committed stack by writing an old copy over current rows.
Close must warn about this unsaved state and offer Retry Save or explicit discard.
Never report that the package saved when only the working database changed.

## 3. Source audit and dependencies

The audit confirms source structure, not runtime correctness. No new test evidence is claimed here.
Paths below are relative to the repository root.

| Area | Current owner and path | Verified source behavior | Required change |
| --- | --- | --- | --- |
| Project tables | `src/include/storage/store/database.hpp`, `src/storage/store/database.cpp` under `alcedo_studio/` | Image, Element, FileImage, and FolderContent have no photo-stack relation. | Add stack tables and migration. |
| Project compatibility | `src/include/edit/history/pipeline_history_format.hpp` | Project metadata writes 0.11.0 and accepts 0.10.0 through 0.11.0. | Introduce project metadata 0.12.0 without changing image-edit formats. |
| Storage boundary | `src/include/sleeve/storage.hpp`, `src/include/storage/store/storage_session.hpp` | Stores take operation-scoped connections under the database lock. Sessions support shared operations. | Add one stack owner and transaction-local deletion repair. |
| Matching and paging | `src/storage/store/sleeve/element_store.cpp` | `ReadAlbumQuery` evaluates SearchMatchSet once under a transaction. Position and ID reads share the photo order. | Add a display projection after matching and group membership. |
| Group definitions | `src/storage/store/sleeve/album_query.cpp`, `src/include/sleeve/album_query.hpp` | Typed group keys include unknown values. Label grouping repeats File occurrences. | Preserve these meanings when forming fragments. |
| Application reads | `src/app/album_browse_service.cpp` | `AlbumBrowseService` owns the UI-facing read API and explicit errors. | Expose display, fragment-member, and display-position reads. |
| Query publication | `src/ui/alcedo_main/album_backend/library_module.cpp` | One SearchRequestWorker runs reads. Publication installs photo and section owners before notifications. | Publish display pages without mixing raw photo positions and display positions. |
| Photo metadata | `src/include/ui/alcedo_main/album_backend/album_types.hpp`, `album_thumbnail_model.hpp` in that directory | AlbumItem and AlbumThumbnailModel hold photo values keyed by File ID. | Keep one photo-data owner. Do not attach a complete stack to each photo. |
| Group geometry | `src/ui/alcedo_main/album_backend/album_section_model.cpp` | Group descriptors, prefix rows, collapse choices, and loaded occurrence IDs drive grouped layout. | Use display counts for geometry and photo counts for captions. |
| Selection and mutations | `library_selection.cpp`, `library_mutation_operations.cpp`, `image_controller.cpp` in that directory | Selection uses element IDs. Shared action owners resolve explicit targets. | Add fragment selection reads and preserve explicit target semantics. |
| QML | `src/ui/alcedo_main/qml/ThumbnailGridView.qml`, `JustifiedThumbnailTile.qml`, `AlbumSectionView.qml` | Flat and grouped views consume photo rows. Justified geometry uses photo aspect ratios. | Consume display identities and show stack disclosures. |
| Save and package | `project_handler.cpp`, `project_module.cpp` in album_backend | `MakeSaveAndPackageJob` saves metadata, then takes the package mutex. It catches metadata-save errors without preserving their text. | Add explicit stack-save outcomes and retain error details. Do not assume the mutex covers metadata save. |
| Application and project close | `application_close_coordinator.{hpp,cpp}`, `project_module.{hpp,cpp}` in album_backend | Close confirmation observes editor state. ProjectModule has no pending-stack-save property. | Add that state and extend the existing close/save flow in PS5C. |
| File deletion and sync | `src/app/sleeve_service.cpp`, `src/storage/store/sleeve/element_store.cpp` | Sync calls AddElements, UpdateElements, then RemoveElements. Folder changes can commit before File removal. RemoveElements holds a connection lock but opens no transaction. | Add one atomic element-change operation that includes stack repair. A lock alone is insufficient. |
| Database packaging and diagnostics | `src/storage/store/storage_maintenance.cpp`, `src/app/project_package_service.cpp` | StorageMaintenance supplies database copies and a fixed diagnostic table list. It is not a user-facing cleanup feature. | Retain this existing owner. Add stack table summaries and package preservation tests. |
| Existing caller test registration | `tests/app/CMakeLists.txt`, `tests/cmake/AlcedoTestRegistration.cmake` under `alcedo_studio/` | ImportServiceTest and SleeveServiceTest are build targets without CTest discovery. Category registration does not discover cases. | Run those binaries directly with verified GoogleTest filters. Do not claim their CTest prefixes exist. |

The `src/` paths in this table all start at `alcedo_studio/src/`.
Short names inside one row share that row's stated directory.

Read these related plans before implementation:

1. [Album Sorting, Grouping, and Import Time](album_sort_group_and_import_time_plan.md).
   Extend its presentation. Replace its exclusion of custom photo grouping only for this stack feature.
   Keep its filter, group, and sort field meanings.
2. [Storage Query Boundary](../storage/storage_query_boundary_plan.md).
   Preserve store-owned SQL, typed rows, prepared binds, and operation-scoped connections.
3. [Library Search and Project Size](../storage/library_search_and_project_size_plan.md).
   Preserve one match-set evaluation and bounded metadata pages.
4. [Global Search Redesign](global_search_redesign_plan.md).
   Preserve relevance admission and raw photo-list behavior.

### 3.1 Main-branch boundary

The source comparison with the former planning branch established these facts:

| Former assumption | Audited main | Plan correction |
| --- | --- | --- |
| ProjectMaintenanceStore and its scanner can protect stack members | These production files and APIs are absent. | Remove them from phases and commands. Future cleanup must call the stack-aware File owner. |
| SourceFileStore and identity-preserving import replacement are available | These APIs are absent. | Do not require source-replacement rendering tests or implement that import feature. Test ordinary imports and existing metadata writes. |
| Maintenance/import plans can serve as prerequisites | Both local plan links are absent on main. | Use only the four existing prerequisite plans above. |
| A deletion transaction already encloses folder unlink and File removal | Current Sync uses separate store calls. | PS2A adds one connection and transaction for those element changes and stack repair. |
| Pending stack saves already participate in close confirmation | No such state or close rule exists. | PS5C adds it to ProjectModule and ApplicationCloseCoordinator. |

Do not merge or copy the maintenance and duplicate-import branches to satisfy this plan.
Stacking remains independently implementable on main.
If those features land before implementation, re-audit their File-removal and File-identity paths.
Preserve existing stack membership when an operation keeps a logical File ID.
Require stack-aware deletion when it removes that ID.
These are integration rules for that future source state, not prerequisites or unfinished stacking phases.

Design sources: [DESIGN.md](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md) and
[VI catalog](../../../VI/README.md).
Before every phase, read AGENTS.md and the current `alcedo-create-plan`, `alcedo-qml-ui`,
`alcedo-msvc-cmake`, and applicable implementation/review skills again.

## 4. Target ownership and stored data

### 4.1 Stored schema

The following tables are proposed. They are the sole persistent stack representation.

```sql
CREATE TABLE PhotoStack (
  stack_id VARCHAR PRIMARY KEY,
  cover_file_id BIGINT NOT NULL
);
CREATE TABLE PhotoStackMember (
  file_id BIGINT PRIMARY KEY,
  stack_id VARCHAR NOT NULL,
  member_order BIGINT NOT NULL CHECK (member_order >= 0),
  UNIQUE (stack_id, member_order)
);
CREATE INDEX idx_photo_stack_member_stack ON PhotoStackMember(stack_id);
```

Use a UUID string for stack_id. Generate it in the application service with the existing Qt UUID
facility. Storage validates its form and uniqueness. Do not add Qt to storage for UUID generation.
Use the existing File ID type at service boundaries. Validate its full range before conversion.

The store validates these conditions inside the transaction:

- Every member refers to a live Element of File type with a valid FileImage binding.
- Every member has exactly one stack and one position in that stack.
- Every saved stack has at least two members.
- The saved cover is one of those members.
- Member positions are unique, non-negative, and deterministic.

Validate stored stack relations during project open and affected-stack reads.
A saved cover excluded by the query permits a temporary cover.
A saved cover missing from its stored membership is invalid data and reports an error.
Do not treat a damaged stack as unstacked photos or silently select a replacement saved cover.

The relation between cover and member is cyclic across these two tables.
Enforce it through the owner transaction and the storage invariant checker.
Do not introduce a partially valid intermediate state visible to another connection.
Use delete-and-insert within the transaction when ordinal changes would violate immediate uniqueness.

Set project metadata version and maximum supported version to 0.12.0.
Keep the lower supported version at 0.10.0.
Opening 0.10.0 or 0.11.0 adds empty stack tables; it does not invent stacks.
The next save writes 0.12.0. Earlier builds reject that version before writing project data.
Keep the packed header, pipeline document, edit history, root, checkpoint, and transfer versions unchanged.
If the implementation base has advanced this metadata version, use its next unused version and
record that exact choice before implementing migration.

Package the tables through the existing database package owner.
Do not add a stack JSON file, image-metadata copy, or external sidecar.

### 4.2 Owner boundaries

| Owner | Responsibility | Lifetime and error surface |
| --- | --- | --- |
| Proposed `PhotoStackStore` | Read and atomically change saved membership, order, and cover. Validate all row invariants. | Project storage lifetime; connection per operation; typed validation result or storage exception. |
| Proposed stack table Mappers | Typed single-table writes used inside the store's transaction. | Connection scope; no independent commit. |
| Proposed `PhotoStackService` | Application API for explicit stack commands and save-independent outcomes. Generate stack IDs. | ProjectService owns it; no duplicate membership cache. |
| `ElementStore` and proposed `album_stack_query` module | Read matching display occurrences and fragment members from the same match set. | One read transaction; move query output to its caller; no retained stack state. |
| `AlbumBrowseService` | UI-facing display and photo reads. Keep filters and semantic inputs consistent. | Project service lifetime; explicit read failure. |
| `LibraryModule` | Accepted query, existing worker, publication, statistics, and raw photo navigation. | GUI owner plus existing queued worker messages. |
| Proposed `LibraryDisplayModel` | Ordered display-page identities, fragment expansion choices, and flat-view roles. | GUI thread; no Image, AlbumItem, pixel, or membership copies. |
| `AlbumSectionModel` | Group headers and prefix row geometry. Read display pages from LibraryDisplayModel. | GUI thread; do not keep a second display-page cache. |
| `AlbumThumbnailModel` | Photo metadata and thumbnail state by File ID. | GUI thread; shared by all occurrences of the same photo. |
| Proposed `PhotoStackController` | UI command validation, background task, save outcome, and completion notification. | ApplicationModuleHost owns it; uses services only. |
| `LibrarySelection` | Selected logical File IDs and explicit whole-stack selection status. | GUI thread; all bulk photo actions read this owner. |

Keep the new responsibilities in their own types.
Do not add another collection of membership to LibraryModule or ImageController.
Do not split large member functions across files and claim that this creates a new owner.

### 4.3 Query and presentation data

Extend the existing `AlbumQueryRead`, `AlbumQueryRow`, `AlbumQueryResult`, and
`AlbumGroupDescriptor` representations. Do not create a parallel result that copies them wholesale.

- Add an explicit photo/display read mode. Existing callers default to the current photo mode.
- Keep raw photo counts and raw photo occurrence offsets unchanged.
- Add named display counts and display offsets. Never use one position type for both meanings.
- A display row carries the existing photo columns for its displayed File and minimal stack scalars:
  stack ID, saved cover File ID, fragment count, query match count, total count, and temporary-cover flag.
- A display identity contains the typed group key and either an unstacked File ID, a stack
  representative identity, or a member File ID within that fragment.
- A representative identity remains stable when its temporary cover changes.
- Expanded member rows refer to the same stack identity. They do not repeat a member list.
- Add a display-position result type rather than changing `AlbumFilePosition` silently.

Compute the global stack total from PhotoStackMember.
Compute the query match count from unique matching Files before label expansion.
Compute the fragment count after label expansion, grouped by stack ID and typed group key.
Use the same SQL comparison for display pages, display positions, and display range IDs.

Build the display query in these steps:

1. Left-join the unique match set to PhotoStackMember and PhotoStack.
2. Distinguish an unstacked File key from a stack UUID. Do not coerce them into one numeric key.
3. Compute total and query-match counts once for participating stacks.
4. Expand the existing group membership relation, including each canonical label occurrence.
5. Partition by typed group key and stack ID. Keep unstacked Files as independent units.
6. Rank candidate covers by saved-cover membership first, then saved member order and File ID.
7. Join the captured expansion choices by the exact fragment key.
8. Produce one representative for a collapsed fragment, or its representative and matching members
   for an expanded fragment. Rank units with the existing representative-photo comparator.
9. Assign display positions, compute display group counts, then apply the requested page bounds.

Compare unknown group keys with null-safe equality.
Use prepared values for expansion keys. Do not concatenate translated titles into SQL or identity keys.
Use the same active semantic model for matching, labels, statistics, and group membership.
Do not compute a temporary cover from the photos retained in a UI page.

`LibraryDisplayModel` owns display pages once the result is published.
`AlbumSectionModel` reads those pages through a scoped const API.
Keep raw photo metadata in AlbumThumbnailModel once per File.
The existing in-place refresh's ID-only read can carry minimal display identities instead of
duplicating photo objects. The model must retain its bounded metadata pages.

### 4.4 Required asynchronous request values

Extend the existing documented `LibraryQueryInput`; do not add another view-state object.
A display read needs immutable fragment-expansion choices for its row counts and page offsets.
Capture only the current scope's typed fragment keys and expansion values needed by that request.

This independent request value is necessary because one queued read must use one expansion choice
throughout counts, ordering, and paging. A mutable GUI-owned reference cannot supply that guarantee.
A synchronous read under a GUI lock would block input for the database operation.
The existing worker request owns these minimal values until completion.
It captures no group descriptors, photo containers, membership arrays, or pixels.
Release it with the request. Never write it back over live expansion choices.
Document these captured fields and this lifetime at the input definition.

Reuse SearchRequestWorker's existing replacement and publication behavior.
Do not add a stack generation, epoch, cancellation scheme, or second query worker.
If an existing mechanism cannot support a demonstrated production interleaving, first record the
two operations, queue boundary, executable reproduction, and required correction.

### 4.5 Persistence and File lifecycle

Every stack mutation uses one storage transaction under the database lock.
Validation and writes use the same connection.
Source-stack repair and destination-stack changes commit together.
Return minimal change facts, such as affected stack IDs and affected File IDs.
Do not return editable copies of complete stored stacks for callers to write back.

Project File deletion removes its memberships and repairs affected stacks in the same transaction
as the File binding, Element, and affected FolderContent changes.
This shared transaction is new work. It is not present in the current Sleeve sync chain.

Add proposed `ElementStore::ApplyElementChanges(additions, modifications, deletions)`.
Its inputs are spans of the existing owner-held SleeveElement references for this sync.
It opens one connection, takes the database lock, and starts one transaction.
Reuse InsertElementRows and UpdateElementRows on that connection.
Add a transaction-neutral removal helper that calls the stack owner's repair operation.
Do not call the public connection-opening AddElements or UpdateElements inside this transaction.

Change SleeveServiceImpl::Sync to submit its existing change lists to that one operation.
Clear sync flags and pending folder changes only after commit.
Run FileSystem garbage collection only after successful sync.
On failure, restore the affected FileSystem entries from committed storage through proposed
FileSystem::RestoreAfterFailedSync(affected_ids). Use the existing Sleeve owner synchronization.
Keep live object identities for restored entries. Remove only newly created entries whose rows did not commit.
Restore affected folder links and flags through that owner operation.
Do not replace the complete FileSystem or read an old copied object over newer state.
The affected IDs come from the current change lists. They are the minimal description needed for reload.
Do not retain whole element copies for rollback. Do not rewind monotonically allocated Element IDs.
Report failure after owner restoration; an explicit user retry starts a new operation.
If restoration itself fails, report both errors and retain the owner error state instead of reporting success.
Standalone full-element removal APIs must use the same connection-local removal helper.
Keep RemoveElement(id) as the documented primitive Element-row operation; do not route live File
deletion through that primitive. Test production deletion entry points through the service.

Album unlink must not remove project stack membership.
Ordinary import and Image metadata writes must leave existing stack rows unchanged.
No main-branch stacking phase requires replacing source bytes while retaining File identity.

Extend StorageMaintenance::ReadDataSummary with PhotoStack and PhotoStackMember.
Keep its existing summary schema and key-conversion rules.
Its row counts are diagnostics, not proof of stack integrity.
Verify membership, order, and covers through owner reads and the storage invariant checker.
Use the existing whole-database package-copy path; do not add another database-copy implementation.

## 5. Proposed APIs and file map

All names in this section that do not exist are proposed.
API parameters carry IDs or focused changes. Services do not expose mutable store internals.

| API | Input and result | Mutation and validation |
| --- | --- | --- |
| `PhotoStackService::CreateStack` | Ordered unique File IDs and cover File ID -> created stack ID and changed IDs | Store validates every File and repairs source stacks atomically. |
| `PhotoStackService::AddMembers` | Target stack ID and ordered File IDs -> changed IDs | Keep existing target order and cover; append only new members. |
| `PhotoStackService::RemoveMembers` | Explicit File IDs -> changed IDs | Remove only those memberships; repair or dissolve source stacks. |
| `PhotoStackService::UngroupStack` | Explicit stack ID -> changed IDs | Remove all memberships of that stack, without deleting photos. |
| `PhotoStackService::SetCover` | Stack ID and member File ID -> changed IDs | Reject non-members; no-op for the current saved cover. |
| `PhotoStackService::MoveMember` | Member File ID and direction -> changed IDs | Resolve the saved adjacent member under the same lock. |
| `PhotoStackService::ReadMemberIds` | Stack ID -> ordered IDs | Read-only explicit full-stack selection; no metadata or pixels. |
| `ElementStore::ApplyElementChanges` | Spans of existing additions, modifications, and deletions -> void | New storage-owned transaction; commits element rows, folder links, and stack repair together. Throws on failure; flags clear only after commit. |
| `FileSystem::RestoreAfterFailedSync` | Minimal affected Element IDs -> void | Proposed focused owner reload from committed storage. Restore affected File and Folder entries and links; preserve live identities. Does not copy the whole FileSystem. |
| `AlbumBrowseService::ReadAlbumQuery` extension | Existing query input plus display mode and expansion choices -> extended existing result | Read-only; one match-set interpretation and explicit failure. |
| `AlbumBrowseService::ReadAlbumDisplayPosition` | Existing query input, File ID, preferred group -> display location and hidden-by-collapse flag | Read-only; distinguish absent File from collapsed member. |
| `AlbumBrowseService::ReadAlbumDisplayFileIds` | Existing query input and display ranges -> ordered unique File IDs | Read-only; return represented photos, not an implicit stack expansion. |
| `AlbumBrowseService::ReadMatchingStackFileIds` | Existing query input and typed fragment key -> ordered unique matching IDs | Read-only; use complete filter and exact group membership. |
| `LibraryDisplayModel::SetStackExpanded` | Typed fragment key and bool | GUI presentation mutation only; request display refresh. |
| `PhotoStackController` command methods | User targets and explicit command -> started/task result, then completion | Apply interaction policy; dispatch to application service and existing save owner. |
| `ProjectModule::RetryStackSave` and `hasUnsavedStackChanges` | No mutation request -> save outcome and pending-state notification | Proposed additions. Retry only persistence; do not repeat the stack command. Clear pending state on successful save or explicit discard. |

Each public read returns a value whose lifetime belongs to its caller.
Each write validates inputs before mutation.
Malformed IDs, unknown stacks, missing Files, and invalid covers have explicit messages.
Database exceptions retain the actual diagnostic. No failed read becomes an empty success.

Existing files that change, grouped by responsibility:

- Schema/format: `alcedo_studio/src/include/storage/store/database.hpp`,
  `alcedo_studio/src/storage/store/database.cpp`,
  `alcedo_studio/src/include/edit/history/pipeline_history_format.hpp`,
  `alcedo_studio/src/include/app/project_package_backend.hpp`.
- Storage ownership: `alcedo_studio/src/include/sleeve/storage.hpp`,
  `alcedo_studio/src/sleeve/storage.cpp`,
  `alcedo_studio/src/sleeve/sleeve_filesystem.cpp` and its defining header,
  `alcedo_studio/src/storage/store/sleeve/element_store.cpp`,
  `alcedo_studio/src/include/storage/store/sleeve/element_store.hpp`,
  `alcedo_studio/src/storage/store/sleeve/album_query.cpp`,
  `alcedo_studio/src/include/storage/store/sleeve/album_query_sql.hpp`,
  `alcedo_studio/src/include/sleeve/album_query.hpp`,
  `alcedo_studio/src/storage/store/storage_maintenance.cpp` and its defining header.
- Services: `alcedo_studio/src/app/album_browse_service.cpp` and its defining header;
  `alcedo_studio/src/app/project_service.cpp` and its defining header;
  `alcedo_studio/src/app/sleeve_service.cpp` and its defining header.
- UI owners: `library_module`, `album_section_model`, `library_selection`,
  `library_mutation_operations`, `project_handler`, `project_module`, `background_task_controller`,
  `interaction_policy_controller`, `application_close_coordinator`, and `application_module_host`, with their defining headers
  under `alcedo_studio/src/include/ui/alcedo_main/album_backend/`.
- QML: `LibraryWorkspace.qml`, `ThumbnailGridView.qml`, `JustifiedThumbnailTile.qml`,
  `AlbumSectionView.qml`, `ImageActionsController.qml`, and the existing image context menu
  in `AppDialogs.qml`, under `alcedo_studio/src/ui/alcedo_main/qml/`.
  Add Main.qml, ShellSignals.qml, and EditorCloseConfirmDialog.qml to the close/save integration scope.
- Justified layout: the existing `justified_layout_model` owner in album_backend and its header.
- Registration: `alcedo_studio/src/storage/CMakeLists.txt`,
  `alcedo_studio/src/app/CMakeLists.txt`, `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt`,
  `alcedo_studio/tests/app/CMakeLists.txt`, `alcedo_studio/tests/ui/CMakeLists.txt`, and
  `alcedo_studio/tests/sleeve/CMakeLists.txt` for the existing StorageMaintenanceTest registration.
- Design and localization: DESIGN.md, AppTheme if required, `docs/VI/README.md`, and the existing
  `alcedo_main_en` and `alcedo_main_zh_CN` translation catalogs.
- Verification: `alcedo_studio/tests/support/storage_invariants.{hpp,cpp}`,
  existing `project_compatibility_test.cpp`, `sleeve_service_test.cpp`, library owner tests,
  `alcedo_studio/tests/storage/storage_maintenance_test.cpp`, and
  `alcedo_studio/tests/ui/application_close_coordinator_test.cpp`.

Proposed new files:

- `alcedo_studio/src/include/sleeve/photo_stack.hpp`: IDs and minimal operation results.
- `alcedo_studio/src/include/storage/mapper/sleeve/photo_stack_mapper.hpp` and
  `alcedo_studio/src/storage/mapper/sleeve/photo_stack_mapper.cpp`: typed stack-table writes.
- `alcedo_studio/src/include/storage/store/sleeve/photo_stack_store.hpp` and
  `alcedo_studio/src/storage/store/sleeve/photo_stack_store.cpp`: persistent owner.
- `alcedo_studio/src/include/storage/store/sleeve/album_stack_query.hpp` and
  `alcedo_studio/src/storage/store/sleeve/album_stack_query.cpp`: display SQL definitions.
- `alcedo_studio/src/include/app/photo_stack_service.hpp` and
  `alcedo_studio/src/app/photo_stack_service.cpp`: application command API.
- `library_display_model.{hpp,cpp}` and `photo_stack_controller.{hpp,cpp}` in the existing
  album_backend header/source directories.
- `alcedo_studio/src/ui/alcedo_main/qml/PhotoStackDisclosure.qml`: shared presentation control.
- Focused proposed test files and targets listed in the phases below.

Proposed test registration map:

| Target | Source under `alcedo_studio/tests/` | Manifest |
| --- | --- | --- |
| PhotoStackStoreTest | `app/photo_stack_store_test.cpp` | `app/CMakeLists.txt` |
| PhotoStackLifecycleTest | `app/photo_stack_lifecycle_test.cpp` | `app/CMakeLists.txt` |
| AlbumStackQueryTest | `app/album_stack_query_test.cpp` | `app/CMakeLists.txt` |
| AlbumStackPagingTest | `app/album_stack_paging_test.cpp` | `app/CMakeLists.txt` |
| LibraryDisplayModelTest | `ui/library_display_model_test.cpp` | `ui/CMakeLists.txt` |
| PhotoStackControllerTest | `ui/photo_stack_controller_test.cpp` | `ui/CMakeLists.txt` |
| ProjectStackSaveTest | `ui/project_stack_save_test.cpp` | `ui/CMakeLists.txt` |
| LibraryStackActionsTest | `ui/library_stack_actions_test.cpp` | `ui/CMakeLists.txt` |
| LibraryStackLayoutTest | `ui/library_stack_layout_test.cpp` | `ui/CMakeLists.txt` |
| LibraryPhotoStackIntegrationTest | `ui/library_photo_stack_integration_test.cpp` | `ui/CMakeLists.txt` |

Reuse the current project/import test builders.
If these suites need shared stack inputs, add proposed `support/photo_stack_test_library.hpp`.
It defines fixture inputs and creates saved stacks through the service.
It must not load live owner data into a parallel editable representation.

Keep headers complete with their defining includes.
Keep SQL and DuckDB connections inside storage.
Do not add image buffers, cloned Images, copied whole-stack objects, or new package representations.

## 6. Primary call chains

### 6.1 Display read and temporary cover

```text
Inspector/search/album or stack expansion change
  -> LibraryModule schedules the existing query worker
  -> AlbumBrowseService::ReadAlbumQuery(display mode)
  -> ElementStore takes connection lock and starts read transaction
  -> SearchMatchSet -> group membership -> fragment query -> display page
  -> counts, groups, and requested photo columns from that read
  -> worker completion on GUI thread
  -> install thumbnail metadata, display identities, and section counts
  -> publish notifications after every owner holds the accepted result
```

Failure: query throws -> worker carries the error -> keep last accepted result -> AlertBadge and Retry.
No cover or membership write occurs on either path.

### 6.2 Stack command and save

```text
context-menu command -> PhotoStackController -> interaction policy
  -> existing background task, with project-close and target-delete protection
  -> PhotoStackService -> PhotoStackStore transaction
  -> validate current Files, memberships, and cover -> commit focused changes
  -> existing save-and-package job
  -> GUI completion -> refresh matching presentation -> finish task
```

Validation or transaction failure: roll back -> report actual error -> keep accepted membership.
Save failure after commit: publish committed working state -> mark pending save in Project owner
-> AlertBadge and Retry Save -> clear pending state only after the package save succeeds.
Keep the operation's project services alive until its worker completes.
Do not make a queued callback read a closed project's mutable UI objects.

### 6.3 Delete and album unlink

```text
existing explicit delete targets -> LibraryMutationOperations -> ImageController worker
  -> AlbumBrowseService -> SleeveServiceImpl::Write and Sync
  -> proposed ElementStore::ApplyElementChanges on one connection and transaction
  -> pending folder writes and File deletion with transaction-local stack repair -> commit
  -> mark owner changes synced and collect deleted FileSystem entries
  -> existing save -> selection prune -> Library refresh
```

Any statement failure rolls back the entire database removal and stack repair.
An album unlink follows its existing FolderContent operation and never calls stack-member removal.
Failure rolls back storage, reloads affected FileSystem entries through their owner, and reports
no successful removal. An explicit retry starts a new operation from the restored state.
PS2A implements the atomic element sync and owner restoration in this chain.

### 6.4 Pending stack save and close

```text
committed stack command -> save job reports failure -> GUI ProjectModule marks unsaved stack changes
  -> existing close coordinator checks editor state and proposed pending-stack-save state
  -> Save: finalize editor if needed -> save/package the current project -> close only on success
  -> Discard: explicit confirmation -> close the working project without saving it
  -> Cancel: keep the project and its pending save state
```

Main's ApplicationCloseCoordinator::BeginSave currently finalizes the editor; it does not itself
guarantee a project package save before application exit. PS5C adds that step for pending stack changes.
Update the current close dialog text when stack changes are pending, including Library-only sessions.
A failed retry aborts close and keeps the working project loaded with the actual error.
Do not add a second close coordinator or copy stack membership for a discard operation.

## 7. Phase summary

Every estimate includes production code, tests, build registration, localization, and phase write-back.
Each phase stays below 2000 changed lines. Split again before implementation if its upper bound grows.
Do not place the complete implementation in one PR if its added diff can exceed the repository limit.

| Phase | Result | Dependency | Expected changed lines | Split reason | Status |
| --- | --- | --- | ---: | --- | --- |
| PS1 | Stack schema, store, and application operations | Source audit | 1300-1850 | Keep persistent invariants in one reviewed unit. | complete |
| PS2A | Atomic element sync and stack-aware project deletion | PS1 | 1200-1850 | Main needs a complete sync transaction and focused owner restore. | complete |
| PS2B | Migration, package preservation, and diagnostics | PS1 | 750-1250 | Keep format and package evidence separate from deletion changes. | complete |
| PS3 | Filter-first fragments, covers, and collapsed display reads | PS1 | 1200-1800 | Isolate query semantics from expansion and UI. | planned |
| PS4 | Expansion, bounded pages, positions, and matching-member reads | PS3 | 1100-1750 | Paging and three position spaces need dedicated tests. | planned |
| PS5A | C++ display owner and complete query publication | PS4 | 800-1300 | Separate display state from command and save state. | planned |
| PS5B | Command controller, task policy, and save outcomes | PS2A, PS2B, PS5A | 700-1200 | Keep command task behavior independent from close decisions. | planned |
| PS5C | Pending stack saves and application/project close | PS5B | 900-1500 | Add the lifecycle behavior that audited main does not provide. | planned |
| PS6 | Selection, bulk actions, and editor navigation | PS5C | 1000-1600 | Verify target semantics through existing callers. | planned |
| PS7 | Classic/grid and grouped QML presentation | PS6 | 1050-1700 | Keep the first visual integration below the line limit. | planned |
| PS8 | Justified rows, localization, and complete visual specification | PS7 | 900-1550 | The second layout has separate geometry and anchor behavior. | planned |
| PS9 | Integrated persistence, resource, and performance acceptance | PS8 | 800-1400 | Keep final evidence separate from feature construction. | planned |

No phase is complete because a type compiles or a constructor succeeds.
PS1 through PS6, including all PS2 and PS5 sub-phases, must not expose an incomplete stack feature
in the production UI.
PS7 and PS8 form one product delivery: do not release a build with a layout that ignores stacks.

### 7.1 Pull request delivery

The user selected five sequential pull requests on 2026-11-11.
The twelve phases remain the implementation and verification units. They do not require twelve PRs.
Keep a focused, tested commit for each phase. Use functional commit and PR titles without phase IDs.
Put the phase IDs and this plan link in the PR body's Plan section.

| PR | Phases | Review scope | Estimated changed lines |
| --- | --- | --- | ---: |
| 1 | PS1, PS2A, PS2B | Stack persistence, atomic deletion, compatibility, and packaging | 3250-4950 |
| 2 | PS3, PS4 | Filter-first fragments, temporary covers, expansion, and paging | 2300-3550 |
| 3 | PS5A | Display models and complete query publication | 800-1300 |
| 4 | PS5B, PS5C, PS6 | Commands, save and close handling, selection, and editor navigation | 2600-4300 |
| 5 | PS7, PS8, PS9 | All Library layouts, localization, and integrated acceptance | 2750-4650 |

The estimates exclude the initial plan-document commit. Include that commit in PR 1.
Keep PR 1's persistence, deletion repair, and format compatibility together before the first merge.
Keep all layouts and integrated acceptance together in PR 5 before product release.
PS1 through PS6 must not expose stack commands or incomplete stack presentation in the production UI.

Use `feature/library-photo-stacking` for PR 1. After each PR merges, create the next functional
branch from updated main. Open one PR in this series at a time. Search for overlapping work first.
Use the current alcedo-create-pr skill and its Plan, Why, Changes, and Verification sections.
Report the targeted test evidence collected during implementation.
Each PR must stay within CONTRIBUTING.md's limit of 10,000 added lines.
The 2000 changed-line phase limit still applies separately to each phase.

Use `Refs #486` in partial PRs and commits. Use a closing keyword only when a PR satisfies the
entire issue. This plan implements only its stacking section. Human authors handle review replies
and subsequent discussion, as CONTRIBUTING.md requires.

## 8. Detailed phases

### PS1 - Persisted stacks and focused owner operations

**Objective and prerequisites.** Add the persistent owner and its complete command semantics.
Start from the audited source. Review the chosen project-wide scope before changing a stored format.

**Modules and deliverables.** Add proposed `photo_stack.hpp`, table Mappers, PhotoStackStore,
PhotoStackService, and their registrations. Connect service lifetime to ProjectService.
Add empty stack tables to Database. Add proposed `PhotoStackStoreTest` in `tests/app/`.

**Data rules.** Enforce Section 4.1 in every operation. Keep cover and membership in storage only.
Use ordered IDs as an explicit command, not an editable stored-stack copy.

**Steps.**

1. Add typed table writes and prepared reads. Keep transactions in PhotoStackStore.
2. Implement create, add, remove, ungroup, cover, and adjacent-order operations.
3. Validate the complete batch before changing a row.
4. Repair every source stack in the same transaction as a member move.
5. Define no-op results for duplicate additions and unchanged covers.
6. Add transaction failure injection through test storage support, without production alternate paths.
7. Add stack checks to the existing storage invariant test support.

**Success chain.** Service -> store lock/transaction -> validate -> all affected writes -> commit -> changed IDs.
**Failure chain.** Invalid input or injected statement failure -> rollback -> unchanged stored rows and diagnostic.

**Tests and assertions.**

- `CreateStackPersistsMembersOrderAndSelectedCover`: real project reopen returns exact IDs and order.
- `MovingSelectedMembersRepairsSourceStacksAtomically`: unselected members stay; one-member sources dissolve.
- `InvalidCoverOrMissingFileLeavesEveryStackUnchanged`: compare both source and destination rows.
- `FailureAfterSourceRemovalRestoresAllMemberships`: fail before destination completion and reopen the database.
- `CoverAndMemberMovePreserveImageHistoryMarksAndAlbumLinks`: compare owner reads before and after.
- `DuplicateAdditionAndUnchangedCoverPerformNoMembershipChange`: verify returned no-op and stable order.

**Build and run.** Run command group V1 in Section 10. Proposed target: PhotoStackStoreTest.
**Exit criteria.** [x] All write invariants pass. [x] Injected failure changes no rows.
[x] No SQL or long-lived connection exists in the service. [x] Public APIs have Doxygen ownership notes.
**Expected diff.** 1300-1850 lines.

#### PS1 completion record (2026-11-11)

**Status:** complete. Implemented only persisted stacks and application commands.
Branch: `feature/library-photo-stacking`. Source base: `e59529c66d2d0bfdc53c5c5cf2d88e6e68d9138d`.
The plan commit is `4830980d`. PS1 and the five-PR split were committed as `a5503149`.

**Changed modules.** Added `PhotoStackChange`, `PhotoStackMove`, two typed table Mappers,
PhotoStackStore, and PhotoStackService. Storage owns the store. ProjectService creates the service
for new and loaded projects. The service retains project storage for its callers.
Database creates the two empty tables and their index with idempotent statements.
Added PhotoStackStoreTest and stack rules in StorageInvariantsTestSupport.
Registered the sources, service, and test target in the existing CMake manifests.

**Implemented behavior and selected details.**

- Create, append, remove, ungroup, saved-cover change, adjacent-member move, and ordered ID reads
  use one connection, database lock, and transaction per operation.
- Commands deduplicate input IDs by first occurrence. A complete existing stack, repeated additions,
  an unchanged cover, and a move beyond an endpoint return no changed IDs.
- Moving selected Files repairs every source in the destination transaction. Cover removal promotes
  the earliest remaining saved member. A source with fewer than two members dissolves.
- Removal retains gaps in saved positions. Append uses the largest saved position plus one.
  Adjacent moves use saved neighbors across those gaps. Position overflow reports an explicit error.
- Mappers accept focused scalar values and run prepared writes. They do not commit independently.
  The service generates canonical UUIDs with Qt. Storage checks the UUID text and uniqueness.
- Validation rejects missing Files, folders, absent or duplicate image bindings, dangling image
  bindings, invalid covers, unknown stacks, and damaged affected relations before writes.
- Results contain only affected stack IDs and File IDs. There is no membership cache, editable stack
  copy, production failure switch, new consistency identifier, or connection retained by the service.

**Primary success call chain:**

```text
ProjectService::GetPhotoStackService -> explicit command and File IDs
  -> PhotoStackStore -> operation connection and database lock -> transaction
  -> validate all targets and affected stacks -> typed Mapper writes and source repair
  -> commit -> caller-owned changed IDs
```

**Primary failure call chain:**

```text
invalid input or destination INSERT constraint failure
  -> exception -> transaction rollback before releasing the database lock
  -> unchanged source membership, source cover, dissolved source, and destination rows
  -> original diagnostic reaches the caller -> reopened project reads the committed prior rows
```

**Executed evidence.** All six required tests ran through the real application service and project
storage. The rollback test inserts one destination member before rejecting another. It verifies
that rollback restores a promoted cover and a dissolved second source, then reopens the project.
It also verifies that a later valid command succeeds.

| Required test | Target | Result |
| --- | --- | --- |
| `CreateStackPersistsMembersOrderAndSelectedCover` | PhotoStackStoreTest | PASS |
| `MovingSelectedMembersRepairsSourceStacksAtomically` | PhotoStackStoreTest | PASS |
| `InvalidCoverOrMissingFileLeavesEveryStackUnchanged` | PhotoStackStoreTest | PASS |
| `FailureAfterSourceRemovalRestoresAllMemberships` | PhotoStackStoreTest | PASS |
| `CoverAndMemberMovePreserveImageHistoryMarksAndAlbumLinks` | PhotoStackStoreTest | PASS |
| `DuplicateAdditionAndUnchangedCoverPerformNoMembershipChange` | PhotoStackStoreTest | PASS |

The other eight stack cases verify partial-source selection, removal and position gaps, malformed
and duplicate IDs, damaged covers, missing bindings and folder targets, position overflow, damaged
member bindings, and schema constraints. Each successful case checks the stored invariants on reopen.
The corruption cases check the error and unchanged stack rows, then restore only their test damage.
The history test compares root and head IDs. It also compares image bindings, ratings, color labels,
Delete marks, and album links before and after cover and order changes.

**Commands and environment.** Used the existing `win_release` build with tests already enabled.
This keeps the supported preset and reuses the installed dependencies. No second build tree was made.
The configured Qt prefix is `C:/Qt/6.12.0/msvc2022_64/lib/cmake`.
Run from the repository root:

```powershell
$env:VCVARS_USER_VERSION = '14.44'
$env:PATH = "$PWD/build/release/vcpkg_installed/x64-windows/debug/bin;$PWD/vcpkg/installed/x64-windows/debug/bin;$PWD/vcpkg/installed/x64-windows/bin;C:/Qt/6.12.0/msvc2022_64/bin;$PWD/build/dependencies/duckdb-v1.5.6;$env:PATH"
cmd /c scripts\msvc_env.cmd --build --preset win_release --target PhotoStackStoreTest ProjectServiceTest --parallel 4
ctest --test-dir build/release -N -R '^(PhotoStackStoreTest|ProjectServiceTest)\.'
ctest --test-dir build/release -j 1 -R '^(PhotoStackStoreTest|ProjectServiceTest)\.' --output-on-failure
python scripts/check_storage_boundary.py
git diff --check --ignore-submodules=all
```

Build and discovery exit codes: 0. Final test exit code: 0.
Discovered: 22. Passed: 22 (14 stack cases and 8 ProjectService cases). Failed: 0. Skipped: 0.
Final test duration: 13.82 seconds. Storage boundary check: no findings.
The first execution passed 20 cases and failed two new test-fixture cases. The fixture now removes
the dependent index before renaming its table and uses Statement for its expected exception type.
The final execution above includes both corrected cases. No existing test failed.

**Review and LOC.** The new store has 373 lines. The stack test file has 418 lines.
The other new C++ files each have at most 55 lines. Code and registration changes total 1151 lines.
PS1 changed 1272 lines with its original completion record. The existing app test manifest has
1034 lines; this phase adds only one focused target registration there. It contains no new business
logic that needs a separate owner. No changed C++ source or header exceeds 1000 lines.
Public APIs document ownership, transaction scope, synchronous calls, and errors.
New files were formatted in full. Existing C++ files were formatted only at changed lines.
The source, test, and documentation terminology searches found no prohibited usage in touched files
and no prohibited roadmap filename. All changed files use LF.

**Scope and unavailable evidence at PS1 completion.** PS1 left project version 0.12, old-project
compatibility validation, package diagnostics, and File-deletion integration to PS2A/PS2B.
Display queries, temporary covers, groups, selection, save tasks, and QML remain in their listed phases.
No full test suite, Debug build, macOS build, UI check, or performance benchmark ran for PS1.
There is no unresolved failure in the targeted evidence. Task logs under
`build/tmp/photo_stacking/` were removed after these results were recorded.

### PS2A - Atomic element sync and stack-aware project deletion

**Objective and prerequisites.** Make project File deletion and stack repair one database change.
Requires PS1. Do not assume the current Sleeve sync already has that transaction.

**Modules and deliverables.** Add ElementStore::ApplyElementChanges and its connection-local removal helper.
Adapt SleeveServiceImpl::Sync and the existing element-removal APIs.
Add FileSystem::RestoreAfterFailedSync in sleeve_filesystem.cpp and its defining header.
Add proposed `PhotoStackLifecycleTest` in `tests/app/` and extend SleeveServiceTest.

**Data rules.** Commit FolderContent, Element, FileImage, and stack repair together.
Album unlink preserves saved stack membership.
Mark owner changes synced only after commit. Restore affected owner entries from storage on failure.

**Steps.**

1. Add the focused store operation over the existing additions, modifications, and deletion lists.
2. Reuse transaction-neutral row helpers on one connection. Do not nest public store transactions.
3. Call the stack repair helper before completing the affected File removals on that connection.
4. Replace Sync's separate AddElements/UpdateElements/RemoveElements calls with the atomic operation.
5. Move flag changes, pending-folder cleanup, and garbage collection after successful commit.
6. Route standalone full-element removal through the shared removal helper under a transaction.
7. Preserve the low-level RemoveElement(id) meaning and keep it out of live File deletion call chains.
8. Inject failure after folder-link writes, after stack repair, and before final commit.
9. Reload only affected FileSystem entries through the new owner operation after a failed transaction.
10. Check stored and live-owner membership after each successful operation and failed attempt.

**Success chain.** Service Write/Sync -> ApplyElementChanges -> one transaction -> folder/File/stack writes
-> commit -> mark owner changes synced -> garbage collection -> existing delete completion.
**Failure chain.** Any statement failure -> rollback all those rows -> focused FileSystem owner reload
-> Sync reports failure -> no successful delete notification; explicit retry starts from restored entries.

**Tests and assertions.**

- `DeletingSavedCoverPromotesFirstRemainingMemberInTheSameCommit`: verify no dangling cover.
- `DeletingDownToOneMemberDissolvesTheSavedStack`: remaining File and album links survive.
- `AlbumUnlinkKeepsProjectStackMembership`: verify exact stored membership after the unlink.
- `FailureAfterFolderUnlinkRestoresFolderFileAndStackRows`: fail on the real service sync path.
- `FailedSyncRestoresFileSystemAndAllowsExplicitRetry`: inspect live File resolution, links, flags, and the later retry result.
- `StandaloneElementRemovalUsesTheSameStackRepair`: exercise each supported full-element removal entry.

**Build and run.** V2A: PhotoStackLifecycleTest and AlbumBackendImageDeleteTest through CTest.
Build SleeveServiceTest and ImportServiceTest, then run their direct filters from Section 10.1.
**Exit criteria.** [x] One connection and transaction cover element changes and stack repair.
[x] Failed sync preserves the database and restores affected owner entries. [x] Existing import sync still passes.
**Expected diff.** 1200-1850 lines.

#### PS2A completion record (2026-11-11)

**Status:** complete. Atomic element sync, stack-aware File removal, and failed-sync recovery.
Branch: `feature/library-photo-stacking`. Implementation base: PS1 commit `a5503149`.
PS2A is committed as `6261e0a7`. PS2B follows in a separate functional commit. No PR was opened.

**Changed modules and selected details.** ElementStore owns one transaction over additions,
modifications, and deletions. Its existing multi-row insertion and folder-delta helpers remain in use.
Standalone full-element removal uses the same connection-local removal helper. That helper repairs
stacks before it removes File bindings. The ID-only Element-row removal primitive keeps its meaning.
Image/history cleanup remains with its existing caller; this phase does not move that ownership.

SleeveService serializes public Sync with Read and Write through its existing FileSystem mutex.
Write and DuplicateFileToFolder call SyncLocked while holding that mutex. Sync reports a count and
collects deleted entries only after the transaction commits. Album unlink changes folder links only.

FileSystem::RestoreAfterFailedSync reads affected entries through ElementStore and restores the
same live objects under the existing owner lock and storage session. ElementMapper applies its
existing persisted row fields. Folder membership is then reloaded without incrementing references.
The operation invalidates path resolution, clears pending folder deltas, discards only uncommitted
new entries, and never rewinds ID allocation. If the first root insert rolled back, the same empty
root remains unsynced and usable. No editable owner copy or new consistency identifier was added.

If owner restoration itself fails, the service retains both the original SQL error and restoration
error and rejects subsequent reads and writes. Reopening the project creates a usable owner from
committed data. This explicit error state prevents access to a partly restored FileSystem.

**Primary success call chain:**

```text
DeletePath / DeleteFileEverywhere / DeleteElement / public Sync
  -> SleeveServiceImpl owner lock -> SyncLocked
  -> ElementStore::ApplyElementChanges -> one connection, database lock, and transaction
  -> folder writes -> PhotoStackStore::RemoveFileMemberships -> File and Element removal
  -> commit -> mark additions/modifications synced and clear pending folder deltas
  -> FileSystem::GarbageCollect -> successful SyncResult and stored repaired cover/membership
```

**Primary failure call chain:**

```text
constraint failure after folder writes or stack repair, before the final Element delete commits
  -> transaction rollback -> zero successful sync count
  -> FileSystem::RestoreAfterFailedSync -> restore committed fields and folder membership in place
  -> failed SyncResult with actual diagnostic -> explicit retry uses restored live objects
restoration failure
  -> retain original and restoration diagnostics -> reject service access until project reopen
```

**Executed evidence.** The new lifecycle cases use real ProjectService, SleeveService, and DuckDB.
Test-only foreign keys reject real statement boundaries. Exact Element, FolderContent, FileImage,
PhotoStack, and PhotoStackMember rows are compared across rollback. Recovery cases also assert live
pointer identity, resolution, sync flags, pending folder changes, and successful explicit retries.

| Required test or additional behavior | Target | Result |
| --- | --- | --- |
| `DeletingSavedCoverPromotesFirstRemainingMemberInTheSameCommit` | PhotoStackLifecycleTest | PASS; saved order, cover, deleted rows, and reopen |
| `DeletingDownToOneMemberDissolvesTheSavedStack` | PhotoStackLifecycleTest | PASS; remaining File and album link retained |
| `AlbumUnlinkKeepsProjectStackMembership` | PhotoStackLifecycleTest | PASS; exact stack rows and reopen |
| `FailureAfterFolderUnlinkRestoresFolderFileAndStackRows` | PhotoStackLifecycleTest | PASS; transaction rollback and live folder membership |
| `FailedSyncRestoresFileSystemAndAllowsExplicitRetry` | PhotoStackLifecycleTest | PASS; failure after stack repair, same live owners, and retry |
| `StandaloneElementRemovalUsesTheSameStackRepair` | PhotoStackLifecycleTest | PASS; shared-pointer and span entry points |
| `FailedSyncDiscardsOnlyNewEntriesAndDoesNotReuseTheirIds` | PhotoStackLifecycleTest | PASS; mixed create/delete rollback |
| `FailedOwnerRestoreReportsBothErrorsAndRequiresProjectReopen` | PhotoStackLifecycleTest | PASS; both diagnostic parts, rejected access, and reopen |
| `FailedAlbumUnlinkRestoresTheSameFolderAndKeepsTheStack` | PhotoStackLifecycleTest | PASS; same folder, cleared pending changes, and retry |
| `PublicSyncCommitsPendingFolderChangesAndPreservesIdentity` | SleeveServiceTest | PASS; committed child link, same owners, then zero pending changes |
| `FailedFirstSyncKeepsRootUsableAndDoesNotReuseDiscardedIds` | SleeveServiceTest | PASS; no committed root/File after failure, same root, and later successful write |

**Commands, discovery, and totals.** PS2A and PS2B share the final build and test run below. It ran
on their combined working tree before the changes were separated into commits. It used
the existing supported `win_release` tree with tests enabled and Qt 6.12.0. No new build tree was made.
All commands ran from the repository root in PowerShell. Build, discovery, and final test exits: 0.

```powershell
$env:VCVARS_USER_VERSION = '14.44'
$env:PATH = "C:/Qt/6.12.0/msvc2022_64/bin;$PWD/build/dependencies/duckdb-v1.5.6;$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$PWD/vcpkg/installed/x64-windows/debug/bin;$PWD/vcpkg/installed/x64-windows/bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
cmd /c scripts\msvc_env.cmd --build --preset win_release --target PhotoStackLifecycleTest PhotoStackStoreTest ProjectCompatibilityTest StorageMaintenanceTest ProjectServiceTest SleeveServiceTest ImportServiceTest AlbumBackendImageDeleteTest --parallel 4
$testPattern = '^(PhotoStackLifecycleTest|PhotoStackStoreTest|ProjectCompatibilityTest|StorageMaintenanceTest|ProjectServiceTest|AlbumBackendImageDeleteTest)\.'
ctest --test-dir build/release -N -R $testPattern -E 'ProjectWrittenByCurrentMainOpensAndRendersUnchanged'
ctest --test-dir build/release -j 1 -R $testPattern -E 'ProjectWrittenByCurrentMainOpensAndRendersUnchanged' --output-on-failure
& ./build/release/alcedo_studio/tests/app/SleeveServiceTest_runtime/SleeveServiceTest.exe '--gtest_filter=SleeveServiceTests.*:ProjectVersionTests.*'
& ./build/release/alcedo_studio/tests/app/ImportServiceTest_runtime/ImportServiceTest.exe '--gtest_filter=ImportServiceTests.ImportEmptyTest:ImportServiceTests.ImportWithNonExistentFiles:ImportServiceTests.ImportedElementNameMatchesFileName:ImportServiceTests.ImportToSubfolder_KeepsUnsyncedAndModifiedSetsDisjoint'
```

| Target | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| PhotoStackStoreTest | 14 | 0 | 0 |
| PhotoStackLifecycleTest, including package cases | 18 | 0 | 0 |
| ProjectCompatibilityTest | 5 | 0 | 0 |
| StorageMaintenanceTest | 6 | 0 | 0 |
| ProjectServiceTest | 8 | 0 | 0 |
| AlbumBackendImageDeleteTest | 9 | 0 | 3 |
| SleeveServiceTest, direct filter | 29 | 0 | 0 |
| ImportServiceTest, direct filter | 4 | 0 | 0 |
| Total | 93 | 0 | 3 |

CTest discovered 63 selected cases; 60 passed and three skipped in 87.82 seconds.
The final direct Sleeve run passed 29 cases in 3.684 seconds; the direct Import run passed four in
0.607 seconds. After strengthening the two-part restoration diagnostic assertions, the lifecycle
target was rebuilt and that exact CTest case passed again in 0.67 seconds. After adding the first-sync
root test, SleeveServiceTest was rebuilt and the final 29-case direct run above passed.

**Failures found and resolved.** The first Import run passed two cases and failed two because they
required the absent private `_DSC1704.NEF`. Reported this dependency in
[#826](https://github.com/zidage/AlcedoStudio/issues/826). Both tests now assert the presence of the
tracked real RAW `sample_images/ci_rawfiles/credit @ryanbreitkreutz _DSC0027.ARW` and keep their
original behavior assertions. All four import cases passed. The initial first-sync failure test
could not drop Element because of schema dependencies; its corrected test setup rejects the
FileImage insert instead. The corrected test exercises the same first-sync rollback and passed.

**Checklist and scope.** All PS2A exit criteria are satisfied. The storage boundary check found no
violations. Review of copied values, includes, ownership, and terminology found no new prohibited
state copy, unsupported forward declaration, or changed-file terminology usage. The shared LOC
review is recorded under PS2B. No PS3 or later behavior or production stack UI was implemented.

**Unavailable evidence.** Three existing deletion cases skipped because private RAW samples are
absent: `DeleteImages_RemovesImageAndRelatedRows`,
`DeleteTargetsRejectsUnacknowledgedImportAtDirectEntryPoint`, and
`DeleteImages_BestEffortPartialFailure`. The nine other deletion caller cases passed.
`ProjectWrittenByCurrentMainOpensAndRendersUnchanged` was explicitly excluded because its GPU
pixel-hash comparison is unrelated to these persistence changes. No full suite, Debug or macOS
build, manual UI check, sanitizer, or performance benchmark ran. This phase adds no UI or layout.
Task evidence under `build/tmp/photo_stack_first_pr/` was removed after recording the final results.

### PS2B - Project compatibility, packaging, and diagnostics

**Objective and prerequisites.** Preserve stacks through supported project open, save, and package operations.
Requires PS1. No maintenance or source-replacement branch is required.

**Modules and deliverables.** Update version constants, Database migration, package comments, and
StorageMaintenance's diagnostic table list. Extend ProjectCompatibilityTest, StorageMaintenanceTest,
and the package cases of PhotoStackLifecycleTest. Keep those package cases in
`tests/app/photo_stack_package_test.cpp`, separate from the File-lifecycle cases.
Both sources share the focused `tests/support/photo_stack_test_library.hpp` fixture.
Keep the historical 0.11 version reader in `tests/resources/compat/project_version_reader_011.hpp`
as a fixed test fixture. It must not become a production alternate read path.

**Data rules.** Old projects gain empty stack tables. Image-edit format identities stay unchanged.
The existing database-copy owner preserves all stack rows. Diagnostic counts do not prove integrity.

**Steps.**

1. Set the selected metadata version and acceptance range together.
2. Add idempotent empty-table migration for 0.10.0 and 0.11.0 projects.
3. Validate stored stack relations at project open. Report invalid stored covers explicitly.
4. Add PhotoStack and PhotoStackMember to ReadDataSummary without changing its schema.
5. Use the existing package path and compare exact membership, order, and cover after reopen.
6. Verify ordinary imports and existing Image metadata writes leave saved stack rows unchanged.
7. Add old-version rejection fixtures and package-write failure evidence.
8. Update the existing project-version assertions in sleeve_service_test.cpp for the selected metadata version.

**Success chain.** Open supported project -> migrate/validate -> save metadata -> existing database copy
-> packed project -> reopen exact stack rows.
**Failure chain.** Migration or stored-data validation fails -> explicit open failure.
Package-copy/write failure leaves the working database valid and reports a failed save.

**Tests and assertions.**

- `Version011ProjectGainsEmptyStackTablesAndKeepsRootIds`: verify existing marks, edits, and IDs after save.
- `PackagedProjectReopensWithExactStackMembershipAndCover`: use the production package path.
- `DataSummaryIncludesBothStackTablesWithExactRowCounts`: keep existing key conversion semantics.
- `PackageDatabaseCopyPreservesStackRowsAndFailureKeepsSource`: use StorageMaintenance and its real database copy.
- `ImportAndMetadataWritesPreserveExistingStackMembership`: call current main services only.

**Build and run.** V2B: PhotoStackLifecycleTest, ProjectCompatibilityTest, StorageMaintenanceTest,
and ProjectServiceTest through CTest. Run the direct ImportServiceTest and SleeveServiceTest filters too.
**Exit criteria.** [x] Both supported old versions open. [x] New format has rejection evidence for old readers.
[x] Package reopen preserves exact stack state. [x] Every required existing target is registered on main.
**Expected diff.** 750-1250 lines.

#### PS2B completion record (2026-11-11)

**Status:** complete. Supported-version migration, stored-stack validation, package preservation,
and diagnostic table counts. Branch and audited source base are the same as the PS2A record.
The prerequisite PS2A commit is `6261e0a7`.

**Changed modules and selected details.** The writer and maximum metadata version are 0.12.0;
the minimum remains 0.10.0. Packed project, pipeline, image-edit, commit, chain, root, checkpoint,
WAL record, and adjustment-transfer format identities are unchanged. The WAL documentation now
states the existing value 7. PS1's idempotent Database table creation also performs the empty-table
migration; no parallel migration owner was added.

ProjectService validates every persisted stack immediately after Storage opens and before services
become available. PhotoStackStore::ValidateAll uses one read transaction, an orphan check, canonical
UUID validation, and one grouped relation query. It checks covers, member count/order, live File
identity, and exactly one valid image binding. It neither repairs nor discards damaged relations.
This avoids one query per stack at open. No performance benchmark was run.

StorageMaintenance::ReadDataSummary includes PhotoStack and PhotoStackMember with the existing
summary schema and key conversion. The existing database-copy and packed-project paths preserve
the whole database. They need no stack sidecar or second persisted representation.

The fixed historical 0.11 reader fixture comes from the audited upstream revision, independently
of current version constants. Only tests include it. Package tests use the real copy, pack, unpack,
and load operations. Their assertion-row copies belong only to tests and are never written back.

**Primary success call chain:**

```text
ProjectService load -> Storage/Database idempotent table creation
  -> PhotoStackStore::ValidateAll -> expose existing application services
  -> SaveProject writes 0.12 metadata and stack table counts
  -> existing StorageMaintenance database copy -> WritePackedProject
  -> UnpackProjectToWorkspace -> ProjectService load -> exact cover, member IDs, and saved order
```

**Primary failure call chain:**

```text
invalid persisted cover / orphan member / empty stack / malformed stack ID
  -> project-open validation throws the actual diagnostic -> stored stack rows stay unchanged
database-copy or package-replacement failure
  -> existing copy/package owner reports failure -> source stack rows stay unchanged
  -> later explicit copy or project reopen succeeds after the test obstruction is removed
```

**Executed evidence:**

| Required test or additional behavior | Target | Result |
| --- | --- | --- |
| `Version011ProjectGainsEmptyStackTablesAndKeepsRootIds` | PhotoStackLifecycleTest | PASS; absent tables, marks, roots, 0.12 save, and repeat open |
| `Version010ProjectGainsLibraryMarksAndKeepsRootIds` | ProjectCompatibilityTest | PASS; existing 0.10 fixture, empty stacks, marks, edits, and root IDs |
| `Version012MetadataIsRejectedByTheVersion011Reader` | ProjectCompatibilityTest | PASS; historical bounds reject 0.12 and accept 0.10/0.11; other format IDs unchanged |
| `PackagedProjectReopensWithExactStackMembershipAndCover` | PhotoStackLifecycleTest | PASS; two stacks, order gaps, cover changes, exact rows, roots, and invariants |
| `DataSummaryIncludesBothStackTablesWithExactRowCounts` | StorageMaintenanceTest | PASS; existing summary schema and numeric key conversion |
| `PackageDatabaseCopyPreservesStackRowsAndFailureKeepsSource` | PhotoStackLifecycleTest | PASS; real copied database, failed copy, detach cleanup, and later copy |
| `ImportAndMetadataWritesPreserveExistingStackMembership` | PhotoStackLifecycleTest | PASS; real import with SyncImports, marks, exact stack rows, reopen, and invariants |
| `PackageWriteFailureReportsErrorAndKeepsWorkingStackRows` | PhotoStackLifecycleTest | PASS; failed destination replacement, diagnostic, source rows, and reopen |
| Invalid cover, orphan membership, empty stack, malformed saved UUID | PhotoStackLifecycleTest, four named cases | PASS; explicit open failure and exact unchanged stack rows |

**Commands and totals.** The PS2A record contains the exact combined commands, target counts,
follow-up runs, and environment. All final build and test exits were 0. All required targets exist
and were built on this branch. The new lifecycle target registers both focused test sources with
CTest. Sleeve and Import use direct filters because audited main does not discover their cases.
The combined 93-pass result includes all nine package cases, five compatibility cases, six storage
maintenance cases, eight ProjectService cases, and the required direct filters.

**Failures found and resolved.** The first combined run passed 58 cases, skipped three, and failed
two. The package-write test used an empty destination directory that the package owner could
replace; it now uses a nonempty directory and asserts that its marker remains. The existing
ProjectService version assertion still expected 0.11; it now checks the selected 0.12 version.
Both corrected cases passed in the final 60-pass, three-skip CTest run. No unresolved failure
remains in the executed targeted evidence. The import fixture issue is recorded as #826 under PS2A.

**Ownership, file size, and review.** PS2A and PS2B together change about 1700 lines, including tests
and these records. This combined count is below the 2000-line limit for either individual phase.
The persistence store, FileSystem owner, application orchestration, and package owner retain their
existing boundaries. New code adds no retained connection, owner mirror, or speculative race state.

| Changed code or registration file | Total lines |
| --- | ---: |
| `src/app/project_service.cpp` | 641 |
| `src/app/sleeve_service.cpp` | 314 |
| `src/include/app/project_package_backend.hpp` | 148 |
| `src/include/app/sleeve_service.hpp` | 155 |
| `src/include/edit/history/pipeline_history_format.hpp` | 90 |
| `src/include/sleeve/sleeve_filesystem.hpp` | 112 |
| `src/include/storage/mapper/sleeve/element/element_mapper.hpp` | 69 |
| `src/include/storage/store/database.hpp` | 338 |
| `src/include/storage/store/sleeve/element_store.hpp` | 407 |
| `src/include/storage/store/sleeve/photo_stack_store.hpp` | 59 |
| `src/sleeve/sleeve_filesystem.cpp` | 706 |
| `src/storage/mapper/sleeve/element/element_mapper.cpp` | 155 |
| `src/storage/store/sleeve/element_store.cpp` | 1371 |
| `src/storage/store/sleeve/photo_stack_store.cpp` | 420 |
| `src/storage/store/storage_maintenance.cpp` | 231 |
| `tests/app/CMakeLists.txt` | 1046 |
| `tests/app/import_service_test.cpp` | 771 |
| `tests/app/photo_stack_store_test.cpp` | 417 |
| `tests/app/project_compatibility_test.cpp` | 454 |
| `tests/app/project_service_test.cpp` | 263 |
| `tests/app/sleeve_service_test.cpp` | 1155 |
| `tests/storage/storage_maintenance_test.cpp` | 304 |
| `tests/app/photo_stack_lifecycle_test.cpp` | 195 |
| `tests/app/photo_stack_package_test.cpp` | 228 |
| `tests/resources/compat/project_version_reader_011.hpp` | 61 |
| `tests/support/photo_stack_test_library.hpp` | 144 |

ElementStore already exceeded 1000 lines (1369 before this change). It owns only a Database
reference; this phase consolidates existing persistence operations into its focused atomic API.
Album query helpers remain separate modules, stack invariants remain in PhotoStackStore, and
restoration of live state remains in FileSystem. Moving these transaction helpers into a second
method file would not establish a new owner. No new mutable responsibility was added to the store.
The existing 1155-line SleeveServiceTest keeps its FileSystem behavior fixture; only two focused
sync cases and version assertions were added. The new lifecycle and package files stay separate
and share a small six-photo project builder. The 1046-line CMake manifest gains one registration
and no runtime responsibility. The PS1 fixture cleanup only removes trailing underscores from
protected members; it changes no assertions or tested behavior.

Public API comments state lock/transaction requirements, ownership, side effects, and errors.
New files were formatted in full; existing files only at changed lines. Changed files use LF.
Storage boundary, whitespace, roadmap filename, and touched-file terminology checks passed.

**Checklist and unavailable evidence.** All PS2B exit criteria are satisfied. The PS2A record lists
the three missing-sample skips and excluded GPU rendering comparison. No full suite, Debug/macOS
build, manual layout/theme/language check, sanitizer, or resource/performance measurement ran.
PS3 and later work is unchanged. No production Library stack command or presentation is exposed.
Task logs under `build/tmp/photo_stack_first_pr/` were removed after results were recorded.

### PS3 - Matching fragments and temporary covers

**Objective and prerequisites.** Return correct collapsed stack results for every query and group.
Requires PS1. Read the existing album SQL comparison and label rules again.

**Modules and deliverables.** Add proposed album_stack_query module.
Extend the existing album query types and ElementStore read path.
Add proposed `AlbumStackQueryTest` in `tests/app/` with an independent expected-result evaluator.

**Data rules.** Match each File before grouping. Partition fragments by typed group key and stack ID.
Temporary covers are read results. A saved stack with one match remains a saved stack.

**Steps.**

1. Add explicit display read mode without changing the default photo read mode.
2. Derive membership only from the existing SearchMatchSet and group relations.
3. Compute total, query-match, and fragment counts at their stated levels.
4. Choose saved or temporary cover before LIMIT/OFFSET.
5. Apply the current comparator to displayed representatives and unstacked photos.
6. Return group photo counts and display counts separately in the existing result.
7. Assert that display reads never write PhotoStack or PhotoStackMember.

**Success chain.** MatchSet -> group occurrences -> fragment partition -> cover -> collapsed display page.
**Failure chain.** Invalid options or query failure -> exception -> no result publication and no stack write.

**Tests and assertions.**

- `NonCoverMatchShowsStackAndChoosesTemporaryCover`: Tuesday returns B, with two matches and total four.
- `DifferentMembersCannotSatisfyDifferentAndConditions`: Monday AND Lens B returns no fragment.
- `OneStackSpanningDaysHasOneFragmentInEachMatchingDay`: assert exact members, counts, and covers.
- `LabelOccurrencesUseOneSavedStackWithoutDuplicatingPhotoTotals`: B appears in two groups; total stays four.
- `AlbumScopeCanUseATemporaryCoverWithoutChangingSavedCover`: C/D album displays C; stored cover stays A.
- `ExplicitSortAndSemanticRelevanceOrderUseTheDisplayedCover`: verify each supported comparator.
- `UnknownGroupAndFlatModeHaveDifferentFragmentKeys`: preserve typed unknown values.
- `PhotoModeReturnsEveryMatchingFileRegardlessOfStackCollapse`: preserve old read APIs and statistics.
- `InvalidStoredCoverReportsErrorInsteadOfChoosingATemporaryCover`: distinguish damaged data from excluded data.

**Build and run.** V3: AlbumStackQueryTest and AlbumQueryTest.
**Exit criteria.** [ ] Section 2.2 table matches exact expected outputs.
[ ] Predicate evaluates once per read. [ ] Saved rows stay byte-for-byte unchanged after reads.
**Expected diff.** 1200-1800 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS4 - Expanded display pages and position reads

**Objective and prerequisites.** Page and navigate stack display items without loading a whole stack.
Requires PS3. Keep raw photo, display occurrence, and visual row positions distinct.

**Modules and deliverables.** Extend album_stack_query and AlbumBrowseService with display positions,
display range IDs, and matching-fragment IDs. Add proposed `AlbumStackPagingTest` in `tests/app/`.

**Data rules.** Expansion changes display counts only.
Display ID ranges select represented photos; Ctrl+A continues to use the raw matching query.

**Steps.**

1. Add minimal immutable expansion input to the existing worker request representation.
2. Derive the expanded display stream with the representative first and other members in saved order.
3. Apply display page bounds after that derivation.
4. Use the same display stream for positions and display-range selection IDs.
5. Return a hidden-by-collapse location for a matching member that needs reveal.
6. Read exact fragment IDs without trusting a page's loaded members.
7. Keep the existing 1000-row page limit and typed result checks.

**Success chain.** Accepted query/expansion input -> display rank -> page or position -> bounded result.
**Failure chain.** Invalid range or failed statement -> explicit error; retain accepted display pages.

**Tests and assertions.**

- `StackCrossingPageBoundariesKeepsOneRepresentativeAndExactMemberOrder`: concatenate pages and compare all IDs.
- `LargeCollapsedStackLoadsOnlyItsRepresentativeMetadata`: a 10,000-member stack returns one photo row.
- `DisplayPositionAndDisplayRangeIdsAgreeWithPagedRows`: verify first, middle, and last positions.
- `MatchingMemberRevealLocatesItsCollapsedFragment`: distinguish hidden member from non-matching member.
- `ExpandOneDayFragmentDoesNotExpandOtherDays`: counts and other fragment state remain unchanged.
- `FragmentSelectionIncludesUnloadedMatchesAndExcludesOtherGroups`: verify independent SQL ID reads.
- `RawSelectionAndStatisticsDoNotDependOnExpansion`: compare collapse, expand, and partial expansion.

**Build and run.** V4: AlbumStackPagingTest, AlbumStackQueryTest, and AlbumQueryTest.
**Exit criteria.** [ ] All pages concatenate without omission or duplication.
[ ] No expansion requires all member metadata. [ ] All three position spaces have explicit names.
**Expected diff.** 1100-1750 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS5A - Presentation owner and complete query publication

**Objective and prerequisites.** Publish stack display results through one C++ presentation owner.
Requires PS4. This sub-phase separates presentation work from command and package-save work.

**Modules and deliverables.** Add LibraryDisplayModel and its ApplicationModuleHost registration.
Adapt LibraryModule publication and AlbumSectionModel geometry.
Add proposed `LibraryDisplayModelTest` through the existing UI test manifest.

**Data rules.** Display model owns display pages; thumbnail model owns photo values.
Section model reads display pages rather than copying them.
Expansion choices are session data. Query output never replaces newer expansion choices.

**Steps.**

1. Install counts, metadata, display pages, and section descriptors before publishing notifications.
2. Transfer each result component into its existing or new owner exactly once.
3. Replace the section model's separate page cache with scoped display-owner reads.
4. Use display counts for geometry and matching photo counts for captions.
5. Preserve representative identity when its temporary cover changes.
6. Preserve expansion choices while a filter temporarily hides their fragment.
7. Reuse the existing query worker's replacement behavior and error publication.

**Success chain.** Worker result -> GUI installation in each owner -> complete accepted state -> notifications.
**Failure chain.** Query error -> retain accepted owners -> current query error and Retry.
An expansion choice made after submission remains the live choice for its requested refresh.

**Tests and assertions.**

- `PublicationObserversSeeOneCompletePhotoAndDisplayResult`: inspect every relevant notification.
- `TemporaryCoverChangeKeepsFragmentIdentityWithoutSelectingTheNewCover`: identity survives; selection stays File-based.
- `ClearingFilterRestoresSavedCoverAndPreviousExpansion`: test flat and multi-group states.
- `SectionGeometryReadsDisplayCountsButReportsMatchingPhotoCounts`: verify row count and caption separately.
- `DisplayPagesHaveOneOwnerAndReleaseHiddenThumbnailPins`: verify loaded counts and owner notifications.

**Build and run.** V5A: LibraryDisplayModelTest, AlbumSectionModelTest, and AlbumBackendLibraryQueryTest.
**Exit criteria.** [ ] One owner per state category. [ ] QAbstractItemModelTester accepts every transition.
[ ] GUI publication performs no SQL. [ ] No duplicate display-page cache remains.
**Expected diff.** 800-1300 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS5B - Command tasks and save outcomes

**Objective and prerequisites.** Connect the complete service behavior to testable C++ UI owners.
Requires PS2A, PS2B, and PS5A. Keep the production QML feature unavailable until its layouts are complete.

**Modules and deliverables.** Add PhotoStackController and its ApplicationModuleHost registration.
Extend the background-task policy and preserve save diagnostics through the existing save job.
Add proposed `PhotoStackControllerTest` through the existing UI test manifest.

**Data rules.** Command requests hold IDs and focused changes only.
Storage owns membership. Saved package state and committed working state have distinct outcomes.
PS5C owns the pending-save and close integration that consumes these results.

**Steps.**

1. Add a background task kind and `EditPhotoStacks` interaction capability.
2. Protect project close and affected File deletion while a command/save job runs.
3. Dispatch validated command IDs through PhotoStackService; do not pass QObjects to the worker.
4. Return distinct unchanged, committed-and-saved, and committed-but-unsaved outcomes.
5. Preserve actual save diagnostics through the existing save job.
6. Use existing query replacement behavior after mutations. Add no stack-specific consistency counter.

**Success chain.** Controller -> policy -> task -> service/store -> save job -> GUI refresh and task completion.
**Failure chain.** Storage failure -> unchanged membership; post-commit save failure -> accepted working state
and an explicit unsaved outcome for PS5C. Do not repeat a committed command.

**Tests and assertions.**

- `CommandUsesWorkerAndRefusesConflictingDeleteOrProjectClose`: use the real task and policy owners.
- `DatabaseFailurePublishesNoOptimisticMembership`: command fails and accepted view remains valid.
- `PackageSaveFailureReportsCommittedButUnsavedStack`: verify working rows and unchanged saved package.
- `MetadataSaveFailureRetainsItsDiagnostic`: inject the error at the existing metadata save owner.

**Build and run.** V5B: PhotoStackControllerTest, LibraryDisplayModelTest, and ProjectCompatibilityTest.
**Exit criteria.** [ ] Command policy and task lifetimes have owner-level evidence.
[ ] Save failure is distinguishable from mutation failure. [ ] No command outcome claims a save that failed.
**Expected diff.** 700-1200 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS5C - Pending stack saves and close integration

**Objective and prerequisites.** Preserve committed stack changes when save or close fails.
Requires PS5B. These pending-state APIs and close rules are proposed additions to main.

**Modules and deliverables.** Extend ProjectModule, ProjectHandler, ApplicationCloseCoordinator,
their defining headers, and the existing ProjectLifecycleHooks where needed.
Update Main.qml, ShellSignals.qml, AppDialogs.qml, and EditorCloseConfirmDialog.qml.
Add proposed ProjectStackSaveTest and extend ApplicationCloseCoordinatorTest.

**Data rules.** ProjectModule owns only the pending-save condition and diagnostic, not stack rows.
Controller completion marks it after a committed-but-unsaved result.
Successful project/package save or explicit project discard clears it.
An active stack command refuses another stack command or explicit save retry until completion.
The save action must not clear pending state while a stack command remains active.
Use the existing task's busy state and project lifecycle hook. Do not add a revision counter.

**Steps.**

1. Add hasUnsavedStackChanges and its change notification to ProjectModule.
2. Add RetryStackSave, which saves the current working project without rerunning a stack mutation.
3. Preserve pending state and the actual error on every failed save attempt.
4. Extend CloseNeedsConfirmation to include pending stack changes in Library-only sessions.
5. After editor finalization, require successful project/package save before application close with Save.
6. Apply the same rule to project close and project replacement, using the existing lifecycle owner.
7. Abort close on save failure. Keep the project and current working stack rows loaded.
8. Use explicit discard to abandon unsaved working-project changes; do not promise stack-only rollback.
9. Update close-dialog text for pending stack changes without adding a second dialog/state owner.
10. Keep the existing editor-only close behavior when no stack save is pending.

**Success chain.** Controller result -> ProjectModule pending condition -> Retry Save or close with Save
-> existing save/package owner -> successful persistence -> clear pending condition -> finish requested close.
**Failure chain.** Save error -> keep pending condition and working project -> show diagnostic -> abort close.
Cancel changes no state. Explicit discard closes without saving the working project.

**Tests and assertions.**

- `RetryStackSavePersistsWithoutRepeatingCommittedMutation`: exact rows and package are checked after retry.
- `LibraryOnlyApplicationCloseRequiresPendingStackSaveConfirmation`: no editor image is needed.
- `ApplicationCloseWaitsForProjectPackageSaveAfterEditorFinalize`: assert package success before close notification.
- `FailedStackSaveAbortsCloseAndKeepsProjectLoaded`: inspect pending condition and live membership.
- `ProjectReplacementDoesNotDiscardPendingStacksWithoutAnExplicitChoice`: check the lifecycle entry surface.
- `ExplicitDiscardLeavesTheLastSavedPackageUnchanged`: reopen the package and verify its prior stack rows.
- `EditorOnlyCloseBehaviorStaysUnchangedWithoutPendingStacks`: retain the current coordinator tests.

**Build and run.** V5C: ProjectStackSaveTest, ApplicationCloseCoordinatorTest, PhotoStackControllerTest,
and ProjectCompatibilityTest. Verify dialog bindings declaratively and through real application review.
**Exit criteria.** [ ] Every close/save entry reads the same pending state.
[ ] Save failure cannot report successful close. [ ] Discard never writes an old stack copy over live rows.
**Expected diff.** 900-1500 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS6 - Selection, existing bulk actions, and editor navigation

**Objective and prerequisites.** Make every existing photo action use the intended explicit Files.
Requires PS5C. This phase adds no stack controls to QML.

**Modules and deliverables.** Extend LibrarySelection, LibraryMutationOperations, LibraryModule's
selection/reveal APIs, and the existing editor-list integration.
Add proposed `LibraryStackActionsTest` in `tests/ui/`.

**Data rules.** A cover click means one File. Ctrl+A means the full matching photo set.
Explicit whole-stack selection is the only selection action that admits non-matching members.
Repeated label occurrences must not duplicate mutation or export targets.

**Steps.**

1. Map displayed card ranges to File IDs through the new display read API.
2. Implement matching-fragment and explicit whole-stack selection reads.
3. Keep hidden-by-collapse selected counts and outside-result selected counts distinct.
4. Intersect selection on filter/scope replacement without clearing it on ordinary collapse.
5. Route rating, marks, delete, export, paste, and album linking through existing explicit target owners.
6. Keep editor filmstrip and automation photo lists on raw photo reads.
7. Reveal a matching hidden member by expanding its exact preferred fragment.

**Success chain.** User selection request -> worker ID read -> LibrarySelection -> existing action owner -> exact IDs.
**Failure chain.** ID read or policy refusal -> unchanged selection/action state and explicit message.
Missing reveal target -> report outside-result state, without changing the filter.

**Tests and assertions.**

- `CollapsedCoverRatingChangesOnlyTheExplicitlySelectedFile`: persisted non-cover ratings stay unchanged.
- `SelectAllIncludesCollapsedMatchingMembersButExcludesNonMatches`: verify complete ID set beyond loaded pages.
- `WholeStackSelectionIsExplicitAndReportsOutsideResultCount`: verify subsequent action target IDs.
- `RepeatedLabelOccurrencesProduceOneExportAndOneMutationPerFile`: inspect real export enqueue and owner calls.
- `FilteredDeleteLeavesNonMatchingStackMembersAndRepairsCover`: use production delete and reopen.
- `FilterChangePrunesSelectionWithoutReplacingSelectedFileWithTemporaryCover`: verify photo identity.
- `EditorFilmstripKeepsAllMatchingFilesWhenLibraryStacksCollapse`: compare its exact IDs and positions.
- `ReturningToHiddenMatchingMemberExpandsOnlyItsPreferredFragment`: assert revealed File and group.

**Build and run.** V6: LibraryStackActionsTest, LibrarySelectionTest, LibraryMutationOperationsTest,
AlbumBackendLibraryQueryTest, and the direct automation library caller tests.
**Exit criteria.** [ ] Every bulk action has exact-target evidence.
[ ] No hidden member enters an action through cover selection. [ ] Filmstrip/photo APIs preserve raw counts.
**Expected diff.** 1000-1600 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS7 - Classic grid and grouped cells

**Objective and prerequisites.** Add the real Library controls for classic and grouped layouts.
Requires PS6. PS8 must complete before product release.

**Modules and deliverables.** Add PhotoStackDisclosure.qml.
Update LibraryWorkspace, ThumbnailGridView, AlbumSectionView, ImageActionsController, and AppDialogs.
Register QML and document token use in DESIGN.md and VI.

**Data rules.** QML reads C++ occurrence identities and scalar roles.
It does not construct stacks or cache member lists.
Group-header disclosure and stack disclosure remain independent.

**Steps.**

1. Bind classic cards and grouped cells to display occurrences and shared photo metadata.
2. Add the compact disclosure, separate count roles, and temporary-cover accessible description.
3. Add context actions, valid disabled states, and the hidden-member ungroup confirmation.
4. Keep pointer, keyboard, rating, rubber-band, and context-menu hit areas separate.
5. Preserve real scroll anchors during disclosure changes and zoom.
6. Show errors with AlertBadge and save retry through the owner.
7. Add declarative role/accessibility assertions. Do not depend on WorkspaceShellTest input delivery.

**Success chain.** QML action -> C++ owner -> accepted display publication -> correct card/control state.
**Failure chain.** Owner refusal -> same accepted result and visible error; QML does not invent a substitute.

**Tests and evidence.**

- `DisclosureTogglesOnlyTheClickedFragmentAndKeepsPhotoSelection`: C++ owner assertions plus declarative bindings.
- `TemporaryCoverOpensItsOwnFileAndReportsMatchingCounts`: assert the actual editor-open target.
- `GroupHeaderCollapseDoesNotChangeStackExpansionChoices`: restore header and verify fragment expansion.
- `CountAndDisclosureRemainAccessibleAtEveryClassicZoomLevel`: include caption-free geometry.
- Manual: both themes, narrow window, all zoom levels, multiple day/camera/label groups, real keyboard and pointer.

**Build and run.** V7: alcedo_main, LibraryDisplayModelTest, PhotoStackControllerTest,
and LibraryStackActionsTest. Run focused declarative tests only if they are reliable on the host.
**Exit criteria.** [ ] Real application matches Section 2.7.
[ ] Every action reaches its intended C++ owner. [ ] QML and new tokens are registered and documented.
**Expected diff.** 1050-1700 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS8 - Justified layout, localization, and complete visual delivery

**Objective and prerequisites.** Finish the same feature in justified rows and every supported UI language.
Requires PS7. Do not switch users to classic grid to hide missing justified-stack behavior.

**Modules and deliverables.** Update JustifiedLayoutModel, JustifiedThumbnailTile, and shared grid geometry.
Complete translations, VI entries, tooltips, error strings, and proposed `LibraryStackLayoutTest`.

**Data rules.** A justified tile uses its displayed cover's oriented aspect ratio.
Replacing a temporary cover may change its geometry, but not its fragment identity.

**Steps.**

1. Feed display-item IDs and displayed-photo aspect ratios to justified layout.
2. Support expand/collapse and temporary-cover replacement without duplicating photo metadata.
3. Preserve the first shown fragment using actual layout rectangles.
4. Keep disclosure hit targets and existing marks readable at compact sizes.
5. Add English and Chinese strings by hand to the current catalogs.
6. Verify language change does not reset membership, expansion, selection, or typed group keys.
7. Complete real-application visual review for all layouts and save/error states.

**Success chain.** Accepted display roles -> justified geometry -> shared tile/disclosure -> existing input owner.
**Failure chain.** Missing source/thumbnail -> existing photo error presentation; query failure -> accepted view and Retry.
Do not select a different cover because thumbnail loading fails.

**Tests and evidence.**

- `JustifiedTemporaryCoverChangeUsesItsAspectRatioAndKeepsFragmentIdentity`: verify rectangles and occurrence identity.
- `JustifiedExpansionPreservesTopFragmentAndShowsOnlyMatchingMembers`: assert member IDs and real anchor.
- `LanguageChangeKeepsTypedFragmentKeysAndSelection`: compare C++ state and translated control text.
- Manual: classic, justified, grouped classic; both themes; English and Chinese; narrow view; reduceMotion.

**Build and run.** V8: alcedo_main, LibraryStackLayoutTest, and AlbumBackendI18nTest.
**Exit criteria.** [ ] Every Library layout implements the same matching policy.
[ ] Every new string translates. [ ] Manual review records actual states and unresolved defects honestly.
**Expected diff.** 900-1550 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

### PS9 - Integrated acceptance and resource evidence

**Objective and prerequisites.** Demonstrate the complete requested capability through production owners.
Requires PS8. This phase closes evidence, not missing product behavior.

**Modules and deliverables.** Add proposed `LibraryPhotoStackIntegrationTest` in `tests/ui/` and focused
performance cases in AlbumStackPagingTest. Update this plan's completion records and roadmap status.

**Data rules.** Expected results come from an independent set/order evaluator over fixture inputs.
Do not compute expected covers by calling the production fragment helper.

**Steps.**

1. Create a real project, import fixture photos, create a stack through the command owner, and save.
2. Apply dates, camera, lens, rating, marks, labels, and semantic search through their production owners.
3. Verify temporary covers, multiple-group fragments, explicit actions, and restored expansion.
4. Delete the saved cover, remove an album link, import another photo, save/package, and reopen.
5. Inject a transaction failure and a package-save failure through the real command paths.
6. Measure 50,000 photos, many small stacks, and one 10,000-member stack.
7. Record targeted test discovery, results, skips, platform limits, and manual evidence.

**Success chain.** ApplicationModuleHost -> Library commands/query -> services -> real storage/package -> reopen.
**Failure chain.** Injected owner failure -> defined rollback or committed-unsaved state -> successful explicit retry.

**Tests and assertions.**

- `FilterGroupSaveAndReopenPreserveSavedCoverAndExactStackMembership`.
- `TemporaryCoverAndExpansionNeverWriteStackRows`.
- `MultipleGroupFragmentsSharePhotoSelectionWithoutDuplicatingActions`.
- `StackCommandsAndDeletionLeaveStorageInvariantsValidAfterEveryOperation`.
- `LargeCollapsedStackKeepsMetadataPagesAndThumbnailPinsBounded`.
- `UnstackedLibraryReadsStayWithinTheMeasuredQueryCostTarget`.

**Build and run.** V9: the proposed integration target and the directly affected suites in Section 10.
**Exit criteria.** [ ] Every acceptance row in Section 9 has evidence.
[ ] Targeted caller suites pass or have linked existing issues. [ ] No layout silently ignores stacks.
[ ] No phase is marked complete with missing required product behavior.
**Expected diff.** 800-1400 lines.
**Completion record.** Not started. Fill the Section 12 template here after implementation.

## 9. Cross-phase acceptance matrix

| Behavior | Primary evidence | Required observable result |
| --- | --- | --- |
| Create/add/remove/order/cover | PS1 store tests | Exact persisted IDs, order, cover, and source repair. |
| Invalid inputs and transaction failure | PS1, PS2A | No partial rows or dangling File references. |
| Filter on a non-cover | PS3 example and integration | Matching fragment appears with a temporary cover. |
| Combined predicates | PS3 independent evaluator | One File must satisfy the full predicate. |
| Cross-day/camera/lens/rating/edit groups | PS3, PS9 | Correct fragment in every applicable group. |
| Multiple semantic labels | PS3, PS6 | Repeated appearances, unique photo counts and action targets. |
| Album excludes saved cover | PS3, PS9 | Temporary cover; global membership unchanged. |
| Single or zero matching member | PS3 | One-member fragment keeps stack identity; zero members hide it. |
| Paging through a large expanded stack | PS4 | Exact order, bounded returned metadata, no duplicate representative. |
| Expansion and filter clearing | PS5A | Session expansion returns; saved cover is unchanged. |
| Group collapse versus stack collapse | PS5A, PS7 | Independent state and correct counts. |
| Bulk photo actions | PS6 | Exact explicit unique targets, including unloaded selected photos. |
| Save/package/reopen | PS2B, PS5C, PS9 | Saved membership persists; temporary covers and expansion do not persist. |
| Failed package save and close | PS5B, PS5C | Working change remains visible; retry does not repeat mutation; failed save aborts close. |
| Delete/unlink/import/metadata writes | PS2A, PS2B, PS9 | Delete repairs; unlink, other imports, and metadata writes preserve membership. |
| Editor and automation callers | PS6 | All matching photo IDs remain available independent of Library collapse. |
| Every layout and language | PS7, PS8 | Same behavior, accessible controls, correct real-item anchors. |
| Performance and resources | PS4, PS9 | No whole-stack metadata load and no per-card SQL. |

## 10. Build and verification commands

### 10.1 Execution rules

These commands are requirements for implementation. They were not run during plan creation.
All new targets below are proposed until their phase registers them.
Read each test manifest before building a target.

Use one build at a time. Do not run tests while a build links or copies DLLs.
Allow at least 10 minutes per Windows build invocation; use 20 minutes for the application.
Poll the same healthy build process instead of restarting it after a short wait.
Use PowerShell and the MSVC wrapper. Do not create another build directory.

Run this setup from the repository root when new targets or QML files require reconfiguration:

```powershell
Set-Location C:\Users\Haidong\Data\Photography\AlcedoStudio
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake" -DALCEDO_BUILD_TESTS=ON
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
```

For each V group below, build its targets and select only its test-name pattern:

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target <targets from the table> --parallel 4
ctest --test-dir build/debug -N -R '<pattern from the table>'
ctest --test-dir build/debug -j 1 -R '<pattern from the table>' --output-on-failure
```

| Group | Build targets | CTest name pattern |
| --- | --- | --- |
| V1 | `PhotoStackStoreTest` | `^PhotoStackStoreTest\.` |
| V2A | `PhotoStackLifecycleTest SleeveServiceTest AlbumBackendImageDeleteTest ImportServiceTest` | `^(PhotoStackLifecycleTest|AlbumBackendImageDeleteTest)\.` |
| V2B | `PhotoStackLifecycleTest ProjectCompatibilityTest StorageMaintenanceTest ProjectServiceTest ImportServiceTest SleeveServiceTest` | `^(PhotoStackLifecycleTest|ProjectCompatibilityTest|StorageMaintenanceTest|ProjectServiceTest)\.` |
| V3 | `AlbumStackQueryTest AlbumQueryTest` | `^(AlbumStackQueryTest|AlbumQueryTest)\.` |
| V4 | `AlbumStackPagingTest AlbumStackQueryTest AlbumQueryTest` | `^(AlbumStackPagingTest|AlbumStackQueryTest|AlbumQueryTest)\.` |
| V5A | `LibraryDisplayModelTest AlbumSectionModelTest AlbumBackendLibraryQueryTest` | `^(LibraryDisplayModelTest|AlbumSectionModelTest|AlbumBackendLibraryQueryTest)\.` |
| V5B | `PhotoStackControllerTest LibraryDisplayModelTest ProjectCompatibilityTest` | `^(PhotoStackControllerTest|LibraryDisplayModelTest|ProjectCompatibilityTest)\.` |
| V5C | `ProjectStackSaveTest ApplicationCloseCoordinatorTest PhotoStackControllerTest ProjectCompatibilityTest` | `^(ProjectStackSaveTest|ApplicationCloseCoordinatorTest|PhotoStackControllerTest|ProjectCompatibilityTest)\.` |
| V6 | `LibraryStackActionsTest LibrarySelectionTest LibraryMutationOperationsTest AlbumBackendLibraryQueryTest AutomationLibraryCommandsTest` | `^(LibraryStackActionsTest|LibrarySelectionTest|LibraryMutationOperationsTest|AlbumBackendLibraryQueryTest|AutomationLibraryCommandsTest)\.` |
| V7 | `alcedo_main LibraryDisplayModelTest PhotoStackControllerTest LibraryStackActionsTest` | `^(LibraryDisplayModelTest|PhotoStackControllerTest|LibraryStackActionsTest)\.` |
| V8 | `alcedo_main LibraryStackLayoutTest AlbumBackendI18nTest` | `^(LibraryStackLayoutTest|AlbumBackendI18nTest)\.` |
| V9 | `LibraryPhotoStackIntegrationTest PhotoStackStoreTest PhotoStackLifecycleTest AlbumStackQueryTest AlbumStackPagingTest LibraryDisplayModelTest PhotoStackControllerTest ProjectStackSaveTest LibraryStackActionsTest LibraryStackLayoutTest` | `^(LibraryPhotoStackIntegrationTest|PhotoStackStoreTest|PhotoStackLifecycleTest|AlbumStackQueryTest|AlbumStackPagingTest|LibraryDisplayModelTest|PhotoStackControllerTest|ProjectStackSaveTest|LibraryStackActionsTest|LibraryStackLayoutTest)\.` |

StorageMaintenanceTest is the existing database summary/copy suite registered in tests/sleeve/CMakeLists.txt.
It does not require the unmerged Project Maintenance feature or any ProjectMaintenance test target.
ImportServiceTest and SleeveServiceTest have no CTest discovery on audited main.
After building V2A and V2B, run these direct Windows commands with the PATH setup above:

```powershell
$sleeveBinary = 'build/debug/alcedo_studio/tests/app/SleeveServiceTest_runtime/SleeveServiceTest.exe'
$sleeveFilter = 'SleeveServiceTests.*:ProjectVersionTests.*'
& $sleeveBinary --gtest_list_tests "--gtest_filter=$sleeveFilter"
& $sleeveBinary "--gtest_filter=$sleeveFilter"
$importBinary = 'build/debug/alcedo_studio/tests/app/ImportServiceTest_runtime/ImportServiceTest.exe'
$importFilter = 'ImportServiceTests.ImportEmptyTest:ImportServiceTests.ImportWithNonExistentFiles:ImportServiceTests.ImportedElementNameMatchesFileName:ImportServiceTests.ImportToSubfolder_KeepsUnsyncedAndModifiedSetsDisjoint'
& $importBinary --gtest_list_tests "--gtest_filter=$importFilter"
& $importBinary "--gtest_filter=$importFilter"
```

Record every direct command's exit code and actual case count.
A successful process with zero selected cases is not evidence.
On macOS, run those same filters against the built target files without the Windows runtime-directory suffix.
PS9 runs the direct callers changed by the final diff, including rating, marks, deletion, import,
project compatibility, and library automation. Select them by exact target prefixes.
Confirm discovery before claiming execution. A missing executable or `_NOT_BUILT` is not a pass.
Check stale runtime DLL copies when observed behavior does not match the linked sources.

On macOS, use `macos_debug` in its existing build directory.
Configure with `-DALCEDO_BUILD_TESTS=ON` and build only the same relevant targets.
Run the same targeted CTest patterns with `--test-dir build/macos-debug`.
No image-processing backend changes are planned. Record unavailable platform coverage explicitly.

Only the user can request a full test suite. Never replace these commands with unfiltered CTest.
Do not spend effort repairing WorkspaceShellTest or synthetic offscreen pointer/focus delivery.
Use C++ owner/service assertions and record actual manual application checks.

### 10.2 Performance and evidence

Use identical fixture data, build configuration, and hardware for before/after measurements.
Before PS3 changes the query, run its benchmark fixture against the current photo-read implementation.
Record those unstacked query timings in the PS3 completion record for the later comparison.
Record first read and the median and p95 of at least ten warm reads.
Measure no-stack, many-small-stack, and one-large-stack libraries separately.
Include first display page, deep page, display position, matching-fragment IDs, and expand/collapse.

Acceptance requirements:

- Each display page returns at most `kMaxAlbumQueryPageRows` photo rows.
- A collapsed 10,000-member stack loads one representative's metadata for its visible card.
- Expanded paging loads requested pages, not every earlier member's metadata.
- No per-card database query or source decode runs on the GUI thread.
- Thumbnail pins track visible photo occurrences and release on pooling, collapse, and teardown.
- The no-stack warm-read median must not exceed the pre-change median by more than 25%.
  Investigate and correct a larger regression before PS9 completes. Record absolute times too.
- Stack presentation never lowers decode resolution or changes render quality.

Store transient logs under `build/tmp/library_photo_stacking/<phase>/`.
Write stable commands, results, and resource measurements into the phase completion record.
Remove that phase's transient files after recording evidence, as required by AGENTS.md.
Do not commit temporary logs or test packages.

## 11. Risks and implementation checks

| Risk | Detection signal | Required response |
| --- | --- | --- |
| Photo and display positions become mixed | Page/position/selection IDs disagree | Correct the shared projection and use separate position types. |
| Global counts are computed after label expansion | Repeated labels inflate total or matching counts | Count unique Files before occurrence expansion. |
| A cover from another group affects this group's order | Temporary representative sorts with excluded metadata | Apply the comparator to the actual displayed File. |
| Deletion leaves a cover or membership behind | Storage invariant check fails after a real delete path | Move cleanup into the shared deletion transaction. |
| Package failure is reported as a successful save | Reopened package lacks the shown stack change | Preserve committed-unsaved status and retry through the Project owner. |
| Branch-only API or test slips into a phase | An input file or target is absent from audited main | Remove that prerequisite; use the explicit main-branch APIs and proposed additions in this plan. |
| Large stacks defeat paging | Metadata count scales with complete stack size | Move fragment/rank computation into storage; keep result metadata bounded. |
| One layout ignores the feature | Switching layout reveals all hidden members | Complete the layout before product release; do not change user layout preferences. |
| Source branch differs from this audit | Owners, schema version, or save operations have changed | Re-audit and update this plan before implementing affected work. |
| A phase approaches its size limit | Updated upper estimate exceeds 2000 lines | Split the phase before writing more implementation. |

Stop an affected phase if its required owner API has changed or its invariants cannot hold.
Update the plan with the concrete implementation path. Do not add a weaker substitute.
Do not add a consistency mechanism for an imagined race.
Report proved defects through the repository issue rules. Search existing issues first.
The known scroll item #612 remains tracked there; this plan must not claim to have fixed it.

Before completion, search first-party source, tests, docs, and the entire roadmap tree for prohibited
terminology. Check roadmap filenames too. Rename violations in touched files and update callers/links.
Review new data copies, async request values, and forward declarations against AGENTS.md.
Use LF. Do not reformat complete existing source files.

## 12. Completion records

Use this template under each phase. Keep actual failures, skips, and unavailable results.
Do not fill implementation evidence during plan creation.

```text
Phase / date / status:
Source revision and branch:
Actual changed modules and total diff:
Implemented product behavior:
Design details selected or revised, with reason:
Primary success call chain:
Primary failure and save-retry call chain:
Build commands and exit codes:
Test discovery / passed / failed / skipped:
Persistence and storage invariant evidence:
Resource and performance evidence:
Manual layouts / themes / languages / input states:
Unavailable coverage and linked issues:
Temporary evidence removed after recording:
```

Original plan verification: source and documentation audit only. The 2026-11-11 review checked the latest
upstream main revision, existing owner APIs, caller chains, and CMake test registration.
All maintenance-branch prerequisites were removed. The approved product matching rules are unchanged.
Product code was unchanged during that plan review. No build, automated product test, or manual
application verification ran during that review. Phase records above contain later execution evidence.
