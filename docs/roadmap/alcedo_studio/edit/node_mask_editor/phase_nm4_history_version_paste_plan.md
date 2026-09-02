# Phase NM4 — History, Version, Recovery, and Paste

Date: 2026-09-01

Status: NM4.1 complete; NM4.2–4.6 planned

Prerequisite: NM3 complete. NM1.4R and NM1.5 behavior remains required.

## 1. Purpose

Replace stage and operator history payloads with typed `PipelineEditBatch` payloads.
Record complete forward and inverse data for document edits.
Replay each Version from one immutable image root.

Store one checkpoint label with the saved `PipelineDocument`.
Use the history head as the only authority for the current Version.
Use the checkpoint only when its root, head, and chain label match history.

Release one new project, document, history, root, and checkpoint format.
Reject older project formats at the project-open boundary.
Do not convert old stage data or old commit payloads.

Keep Adjustment Transfer Paste.
Paste creates and checks out a new Version on the target image.
Remove product paths that create a pipeline merge commit.

NM4 does not add the Nodes panel.
NM4 does not add node-aware adjustment panels.
NM4 does not add Mask drawing or QSG overlays.
It supplies the durable editing behavior that NM5, NM6, and NM7 require.

## 2. Required context

Read the applicable sections before each sub-phase.
Follow a link when it defines an owner, order, or failure rule.
Check current callers and tests before a change.
Do not infer a component's purpose from its name.

| Source | Required context |
| --- | --- |
| [Node and mask master plan](../node_mask_editor_master_plan.md) | Sections 3, 6, 9, 11–14, 19–21, 23, and 25 define identity, history, Version, Paste, formats, and risks. |
| [NM1 execution plan](phase_nm1_pipeline_document_editing_plan.md) | Sections 3–5 define the one live document. Sections 10–15 define access, save, and later history work. |
| [NM2 execution plan](phase_nm2_multi_grade_runtime_plan.md) | Sections 3–4 define Grade ownership, ordering, and content identity. |
| [NM3 execution plan](phase_nm3_multi_mask_runtime_plan.md) | Sections 3–5 define Mask identity, immutable assets, and the boundary to NM4. |
| [Single live pipeline plan](../../ui/editor_single_live_pipeline_wal_checkpoint_plan.md) | Defines one live editing object, history authority, WAL order, and checkpoint identity. |
| [Mini-Git history plan](../../ui/phase_6c_mini_git_history_and_pipeline_snapshot_plan.md) | Defines the current commit graph, Version refs, journal, checkpoint, checkout, recovery, and transfer paths. |
| [History and Versions repair plan](../../ui/phase_7a_history_versions_repair_and_ui_refactor_plan.md) | Defines current History and Versions presentation behavior. |

The master plan defines current product behavior.
NM1 defines the live document and document commands.
NM2 defines the ordered multi-Grade runtime.
NM3 defines the final Mask model and raster asset identity.

This plan replaces history payloads and saved state.
It reuses the current commit graph, Version refs, journal, and save coordinator where their behavior remains valid.
It does not restore `EditHistory`, `WorkingVersion`, a candidate document, or a snapshot executor.

### 2.1 Current source map

The paths below show the implementation on 2026-09-01.
Check them again at the implementation revision.

| Component | Source and current responsibility |
| --- | --- |
| Commit payload | [edit_commit.hpp](../../../../../alcedo_studio/src/include/edit/history/edit_commit.hpp) stores `OrdinaryEditPayload` by operator, stage, and field. It also stores merge payload types. |
| Commit graph | [commit_graph.hpp](../../../../../alcedo_studio/src/include/edit/history/commit_graph.hpp) owns immutable commits, Version refs, active Version, and materialized state. |
| Working history | [mini_git_working_history.hpp](../../../../../alcedo_studio/src/include/edit/history/mini_git_working_history.hpp) prepares edits and head moves. It appends to the mini-Git journal before it publishes a working head. |
| Version state | [version_ref.hpp](../../../../../alcedo_studio/src/include/edit/history/version_ref.hpp) stores `VersionRef` and `ImageEditState`. |
| Legacy history | [edit_transaction.hpp](../../../../../alcedo_studio/src/include/edit/history/edit_transaction.hpp), [version.hpp](../../../../../alcedo_studio/src/include/edit/history/version.hpp), and [edit_history.hpp](../../../../../alcedo_studio/src/include/edit/history/edit_history.hpp) still describe stage and operator timelines. |
| Live guard | [pipeline_service.hpp](../../../../../alcedo_studio/src/include/app/pipeline_service.hpp) owns one `PipelineDocument`, one executor, and one `CommitGraph` pointer for an image. |
| Parameter commands | [editor_pipeline_command_service.hpp](../../../../../alcedo_studio/src/include/app/editor_pipeline_command_service.hpp) reads and applies complete `EditorParameterTarget` values. |
| Graph commands | [pipeline_graph_commands.hpp](../../../../../alcedo_studio/src/include/edit/graph/pipeline_graph_commands.hpp) adds, removes, reconnects, renames, and enables Color Grades. |
| Mask model operations | [color_grade_node_model.hpp](../../../../../alcedo_studio/src/include/edit/graph/color_grade_node_model.hpp) adds, removes, and changes Grade-owned Masks. |
| Document I/O | [pipeline_document.hpp](../../../../../alcedo_studio/src/include/edit/graph/pipeline_document.hpp) defines format 3 and the current full DAG JSON. |
| Parameter history bridge | [editor_history_mutation.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp) captures preview state and commits ordinary parameter edits. |
| History state and projection | [editor_history_state_detail.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_state_detail.cpp) and [editor_history_projection.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_projection.cpp) load working state and publish rows. |
| Session history port | [editor_session_history_port.hpp](../../../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp) exposes commit, Undo, Redo, Version, transfer, and checkpoint operations. |
| Pipeline checkout | [editor_session_pipeline_port.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_pipeline_port.cpp) routes Version checkout into `PipelineMgmtService`. |
| Checkpoint capture | [editor_history_checkpoint.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_checkpoint.cpp) captures parameter JSON with root, head, and chain labels. |
| Checkpoint materialization | [editor_mini_git_materializer.hpp](../../../../../alcedo_studio/src/include/app/editor_mini_git_materializer.hpp) writes a captured graph and checkpoint in one storage operation. |
| Journal recovery | [editor_mini_git_journal_recovery.hpp](../../../../../alcedo_studio/src/include/app/editor_mini_git_journal_recovery.hpp) compares WAL records with durable history. |
| Storage | [commit_graph_store.cpp](../../../../../alcedo_studio/src/storage/store/edit_history/commit_graph_store.cpp) stores commits, refs, image state, and root state in DuckDB. |
| Database schema | [database.hpp](../../../../../alcedo_studio/src/include/storage/store/database.hpp) defines current history and root tables. |
| Paste and merge | [adjustment_transfer_service.hpp](../../../../../alcedo_studio/src/include/app/adjustment_transfer_service.hpp) captures operator packages and creates Paste or merge commits. |
| Live transfer | [editor_history_transfer.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp) applies live Paste and merge operations. |
| Project format | [project_package_backend.hpp](../../../../../alcedo_studio/src/include/app/project_package_backend.hpp) accepts project version `0.3.0`. |
| Mask assets | [mask_store.hpp](../../../../../alcedo_studio/src/include/edit/mask/mask_store.hpp) writes and reads immutable content-addressed R8 assets. |

### 2.2 Current behavior

The active QML editor already uses a mini-Git commit graph.
One `VersionRef` owns the active working head.
The live guard reads that head from `CommitGraph`.

Current settled parameter history uses `OrdinaryEditPayload`.
The payload identifies one operator, one legacy stage, and one field.
It cannot identify an arbitrary Grade, adjustment instance, Mask, or graph edge.

Current Undo and Redo apply before and after parameter JSON.
Current Version checkout rebuilds a CPU parameter table from an immutable root and first-parent commits.
The current checkpoint also stores CPU parameter JSON.

The live guard now owns a full `PipelineDocument`.
NM2 and NM3 made that document able to store multiple Grades and multiple Masks.
The checkpoint and root path do not yet use that full document as their replay value.

Current transfer code captures an operator list.
Paste builds one commit per operator.
Merge code can create a two-parent commit and conflict payload.

The project entry already rejects versions outside its accepted version range.
NM4 must advance that version range again.
The new range must not accept `0.3.0` projects.

### 2.3 Existing limits

- History cannot identify arbitrary `NodeId`, `AdjustmentInstanceId`, or `MaskId` values.
- One graph command cannot store all data that its inverse requires.
- Node deletion history cannot restore the exact node, Masks, adjustments, or edges.
- Reconnect history cannot restore the exact prior neighbors.
- Mask asset replacement cannot restore the prior immutable key.
- The immutable root stores CPU parameter JSON instead of the full default document.
- The checkpoint stores CPU parameter JSON instead of the full document.
- Replay can rebuild only the legacy parameter table.
- The current commit hash format does not identify typed pipeline batches.
- Current history rows derive meaning from operator and stage fields.
- The current transfer package does not contain the complete transferable DAG data.
- Product merge APIs and QML state still exist.
- Mask disk assets have no complete reachability scan.
- Project version `0.3.0` does not identify the final node and Mask history format.

## 3. Terms and scope

| Term | Meaning |
| --- | --- |
| Pipeline edit batch | One user action with one or more ordered typed changes. It creates one commit. |
| Typed change | One change variant with explicit identity and complete before and after data. |
| Forward apply | Apply the batch from its stored before state to its stored after state. |
| Inverse apply | Apply the batch from its stored after state to its stored before state. |
| Root document | The immutable default `PipelineDocument` after image-specific Develop data enters it. |
| Working head | The active Version tip in `CommitGraph`. It is the only history head. |
| Checkpoint document | A saved document with the root, head, and chain label that it represents. |
| WAL record | A durable mini-Git journal record for one commit or head move. |
| First-parent replay | Apply commits from the image root to one Version head in order. |
| Transfer document | The portable Grade chain, Masks, and DRT/Post values used by Paste. |
| Reachable asset | A Mask asset referenced by a live, durable, recovery, or active-authoring owner. |
| Published state | The document, head, projections, and render intent visible after one successful action. |

### 3.1 Included work

- Define typed `PipelineEditBatch` and typed change variants.
- Store complete forward and inverse data.
- Apply parameter, graph, node, Mask, and raster-key changes to one live document.
- Replace `OrdinaryEditPayload` on the new project path.
- Preserve one commit for one user action.
- Update commit canonical encoding and hashing.
- Update the mini-Git WAL for typed batch commits and head moves.
- Store the full immutable root document.
- Store the full checkpoint document with root, head, and chain labels.
- Rebuild a Version by first-parent replay.
- Restore the prior Version after a failed checkout.
- Support branch creation, Undo, Redo, checkout, recovery, and reopen.
- Advance all incompatible format versions together.
- Reject old project, document, history, root, checkpoint, and WAL formats.
- Replace transfer packages with portable document data.
- Paste into a new target Version with complete ID remapping.
- Keep target Develop and image-specific RAW data.
- Remove product merge creation and merge presentation paths.
- Scan Mask asset reachability for clean-exit or maintenance deletion.
- Publish typed, localizable history rows.
- Add service integration tests without production Nodes or Mask UI.

### 3.2 Excluded work

- NM5 QuickQanava projection and commands.
- NM6 selected-node adjustment routing.
- NM7 viewer input, active Brush raster creation, and QSG overlays.
- Color Range or Luminance Range algorithms.
- Product image branches or a branch mixer.
- Detached HEAD.
- A new pipeline merge operation.
- Old project conversion.
- Old stage commit conversion.
- Old merge commit conversion.
- A second live document or candidate document.
- A second executor for replay or checkout.
- Per-Version resident writable documents.
- Disk asset deletion during normal rendering.
- CPU or other-backend substitution after a failure.

## 4. Design requirements

### 4.1 Authority and object lifetime

Keep these ownership rules:

```text
CommitGraph active Version head
  = only working history head

PipelineGuard::document_
  = only writable document for the image

PipelineGuard::pipeline_
  = executor and current parameter projection

root document
  = immutable replay start

checkpoint document
  = saved acceleration value with an exact history label
```

Do not store another working head on `PipelineGuard`.
Do not store a writable document on each Version.
Do not publish a candidate document after each operation.

Hold the existing render lock for a complete document change.
This includes forward apply, inverse apply, checkout replay, and failure restoration.
UI projection and render scheduling happen after the lock-protected state is valid.

Background tasks keep using the shared document and executor rules from NM1.4R.
They must not move history heads or save a checkpoint.

### 4.2 Immutable root document

Create the root after image-specific Develop values are known.
The root must contain the full default three-node document.

The root contains:

- document geometry defaults;
- the Develop endpoint;
- image-specific RAW metadata and color data;
- the Default Color Grade with the product baseline;
- the DRT/Post endpoint and its default values;
- all stable node and adjustment instance identities;
- an empty Mask list on the Default Color Grade;
- the two scene-image edges.

The root does not contain user edits.
It does not change when product defaults change later.
It does not change after the image enters history.

Calculate `root_id` from canonical root identity data.
The calculation must bind the root to its image owner.
It must also bind the exact root document format and content.

Store the root document once.
If the root already exists, compare its canonical identity.
Reject a different image owner or different canonical root.
Do not replace it.

### 4.3 `PipelineEditBatch` shape

Use one immutable batch payload per commit.
The exact C++ names can follow existing naming style.
The data must have this meaning:

```text
PipelineEditBatch
  batch_format_version
  operation_kind
  ordered changes[]
  presentation_key
  presentation_args
```

`operation_kind` identifies the user action.
It does not replace the typed changes.
`presentation_key` is a stable localization key.
It is not final user text.

Each change must contain all required identity.
Each change must contain complete before and after data.
Replay must not inspect a current selection.
Replay must not infer an ID from a field key.
Replay must not reconstruct deleted data from later commits.

Keep change order significant.
Forward apply uses stored order.
Inverse apply uses reverse order.

Reject an empty batch.
Reject a batch with incompatible change kinds.
Reject unknown fields and unknown enum values.
Reject non-canonical values before a commit enters the graph.

### 4.4 Typed change variants

The first NM4 payload supports these variants:

| Variant | Required identity | Required before and after data |
| --- | --- | --- |
| `SetParameter` | Complete `EditorParameterTarget` | Model JSON and enabled state. |
| `SetNodeEnabled` | `NodeId` and node kind | Boolean values. |
| `SetNodeMix` | Color Grade `NodeId` | Normalized finite float values. |
| `RenameColorGrade` | Color Grade `NodeId` | Old and new display names. |
| `AddColorGrade` | New `NodeId` | Full node, insertion neighbors, and exact scene edges. |
| `RemoveColorGrade` | Removed `NodeId` | Full node, prior neighbors, removed edges, and bridge edge. |
| `ReconnectColorGrade` | Grade `NodeId` | Prior and next predecessor, successor, and exact edges. |
| `AddMask` | Grade `NodeId` and `MaskId` | Full `MaskModel` and display index. |
| `RemoveMask` | Grade `NodeId` and `MaskId` | Full `MaskModel` and display index. |
| `ReplaceMaskSource` | Grade `NodeId` and `MaskId` | Complete old and new `MaskSource`. |
| `ReplaceMaskAsset` | Grade `NodeId` and `MaskId` | Complete old and new Brush source data. |
| `SetMaskField` | Grade `NodeId`, `MaskId`, and field key | Old and new typed field values. |

`SetParameter` covers Document, Develop, Color Grade adjustment, and DRT/Post targets.
It can also cover a future supported Mask parameter target.
The dedicated Mask variants remain required for structural source and asset changes.

`AddColorGrade` stores the complete Clean node created for that action.
Redo must restore the same `NodeId` and `AdjustmentInstanceId` values.

`RemoveColorGrade` stores the full removed node.
This includes its display name, enabled value, mix, adjustments, Masks, and asset keys.
Undo must restore the exact node and exact edge positions.

`ReplaceMaskAsset` stores complete Brush source values.
It does not store raster bytes in the commit.
The old and new `MaskAssetKey` values must both resolve before the head move publishes.

Mask display reorder remains session and presentation state in NM4.
It does not create a photo edit commit.

### 4.5 Canonical encoding and commit identity

Define one canonical JSON shape for every variant.
Use stable lowercase kind names.
Write every required field, including explicit null values.

Use canonical number rules.
Reject non-finite numbers.
Normalize model values before the batch is created.
Hash the normalized stored value.

Preserve ordered arrays where order changes meaning.
This includes batch changes, adjustment lists, Mask lists, and scene edges.
Sort only data that the model defines as unordered.

Do not hash localized text.
Do not hash QML state.
Do not hash GPU state, dirty bits, request data, or cache data.

Advance the commit format version for typed batch commits.
Advance the chain format version when the commit hash input changes.
Do not accept old ordinary or merge payloads in the new project format.

Canonical tests must use fixed golden bytes and fixed hashes.
The test must not calculate its expected bytes through the encoder under test.

### 4.6 Batch validation

Validate a batch at these boundaries:

1. Before commit creation.
2. During commit JSON load.
3. During WAL load.
4. During DuckDB graph load.
5. Before forward or inverse apply.

Validation checks stored data and current preconditions.
It must check:

- root identity;
- supported payload version;
- complete target identity;
- owner and node kind;
- node, adjustment, and Mask identity;
- model field names and types;
- finite and normalized values;
- graph neighbors and exact edges;
- full node and Mask JSON;
- asset key syntax and asset descriptor;
- operation and change compatibility.

Load validation must reject malformed stored data.
It must not repair owner placement or convert an old payload.

Apply validation checks the expected current side.
Forward apply requires the stored before state.
Inverse apply requires the stored after state.
A mismatch is an error.
It is not a request to overwrite newer data.

### 4.7 Domain apply and inverse apply

Create one application-layer batch applier.
It operates on `PipelineDocument` under the existing render lock.
It does not own UI state, storage, or rendering.

Use existing domain operations where they have the required meaning:

- `ApplyEditorParameterPatch` for model parameters;
- `AddCleanColorGrade` or an exact-node insertion helper;
- `RemoveColorGradeAndBridge`;
- `ReconnectColorGrade`;
- `RenameColorGrade`;
- `SetColorGradeEnabled`;
- `ColorGradeNodeModel` Mask operations.

Add exact restoration helpers where current commands create new IDs.
These helpers must remain in the edit model or application layer.
Do not place them in QML or the history projection.

Before mutation, keep only the local values needed for restoration.
Do not clone the full document.
If change `n` fails, inverse-apply changes `n-1` through `1`.

After a structural batch, run graph and backbone validation once.
After a parameter-only batch, validate its owners and values.
Do not run full graph validation for every slider value.

A successful forward then inverse apply must restore the canonical document hash.
A successful inverse then forward apply must restore it again.

### 4.8 Settled edits and WAL order

Keep provisional input separate from committed history.

For a parameter input sequence:

```text
first valid provisional patch
  -> lock the complete target
  -> capture the typed before value
  -> apply provisional values to the live document
  -> Interactive render

settled patch
  -> normalize and read the final after value
  -> build one PipelineEditBatch
  -> validate the batch
  -> append one WAL commit record
  -> publish one commit and working head
  -> publish committed document state
  -> Quality render
```

The live document can already contain the after value when the pointer input settles.
The commit still stores both before and after values.

For a non-preview command:

```text
prepare typed change and local before data
  -> apply the change under the render lock
  -> validate the resulting document
  -> append WAL and publish the commit
  -> publish projections and render intent
```

If the WAL append or head publish fails, inverse-apply the live change.
Keep the prior head and prior projection revision.
Do not schedule a render for the failed action.

The WAL record stores the complete immutable commit.
It also stores expected source and target head and chain values.
The record must use the same canonical payload as DuckDB.

One user action advances the chain once.
A batch with several typed changes still creates one commit.

### 4.9 Undo, Redo, and direct head moves

Undo applies the current commit's batch in the inverse direction.
Redo applies the selected child commit's batch in the forward direction.

Prepare the complete move before the WAL append.
Validate all required assets and stored objects before publication.
Append the WAL head-move record before the working head becomes visible.

Apply traversed batches in this order:

- backward move: newest commit to oldest commit, using inverse apply;
- forward move: oldest commit to newest commit, using forward apply.

If live apply fails, revoke the new WAL tail.
Restore the prior head and redo selection.
Restore the prior document through the already-applied batch directions.

Do not use stored checkpoint JSON to implement ordinary Undo or Redo.
Do not move a head when the expected current side does not match.

Undo and Redo schedule one Quality render after success.
Rename-only Undo and Redo do not schedule a render.

### 4.10 Version behavior

Each Version is a stable named ref.
Its head can be the root or one commit.
Several Versions can share the same head and ancestry.

Create a root Version with a null head.
Create a branch Version at an explicit existing commit.
Do not copy a writable document into the Version.

Version checkout uses this order:

1. Finish or cancel provisional input.
2. Complete the existing save checkpoint barrier.
3. Capture the prior Version ID, head, and redo selection.
4. Resolve and validate the target first-parent chain.
5. Verify every referenced persistent Mask asset.
6. Reset the same live document to the immutable root.
7. Apply target batches in first-parent order.
8. Validate the final document.
9. Publish the active Version and working selection.
10. Publish document, history, and adjustment projections.
11. Schedule one `VersionDocumentChanged` Quality render.

No other thread can read the document during steps 6–8.
Do not allocate a second live document for the target Version.

If target replay fails, reset the same live document to the root again.
Replay the prior first-parent chain.
Restore the prior active Version and redo selection.

If prior-state restoration also fails, report a fatal editor-session error.
Do not continue with another backend, stage table, or reduced feature set.

### 4.11 Checkpoint and reopen

The checkpoint stores one full `PipelineDocument` with these labels:

```text
checkpoint_state_format_version
project_schema_version
pipeline_document_format_version
root_id
head_commit_hash
transaction_chain_hash
pipeline_document
```

Build all label fields from one `CommitGraphMaterialization` capture.
Do not read the head twice from separate owners.

Use a checkpoint only when all labels match the loaded history and root.
Validate the document before it becomes live.
Verify every referenced persistent Mask asset.

When the labels match:

```text
load CommitGraph and active Version
  -> validate root and checkpoint labels
  -> load PipelineDocument from checkpoint
  -> bind it to the existing executor
  -> skip first-parent replay
```

When any label does not match:

```text
history head remains authoritative
  -> load immutable root document
  -> replay first-parent typed batches
  -> validate final document and assets
  -> mark checkpoint writeback required
```

Do not compare every parameter with history after a valid label match.
Do not move the history head to match a checkpoint.
Do not accept a checkpoint with the wrong root.

Normal save captures the already-valid live graph and document.
The worker writes history, refs, image state, root identity, and checkpoint in one DuckDB transaction.
Clear the saved WAL range only after the DuckDB transaction commits.

### 4.12 Recovery

Recovery starts from durable DuckDB state and the mini-Git WAL.
It does not use a legacy stage timeline.

For a fully materialized WAL prefix, clear only the covered records.
For a valid missing suffix, apply its typed commits and head moves to the live graph and document.
For an incomplete tail, keep the valid prefix and isolate the invalid tail as current policy requires.

Each recovery record must prove this sequence:

```text
expected source head and chain
  -> one valid typed commit or head move
  -> expected target head and chain
```

Validate the first-parent relation and commit hash before apply.
Validate the typed payload before document mutation.
Validate Mask assets before a head becomes visible.

After recovery, one head, one chain, and one document must agree.
Write a new checkpoint only through the normal materialization path.

Never delete assets while unmaterialized recovery records can reference them.

### 4.13 Project and schema cutover

NM4 is one incompatible format release.
Advance these version identities together:

- project metadata version;
- packed project acceptance range;
- `PipelineDocument` format version;
- image edit schema version;
- commit payload format version;
- commit hash input version;
- transaction chain format version;
- root state format version;
- checkpoint state format version;
- mini-Git WAL record or payload version;
- transfer package schema version.

Choose the exact new constants in NM4.3.
Record them in one format table near the owning code.
Do not use a range that also accepts the old values.

Project open must check metadata before history or pipeline load.
Return one clear unsupported-format error for an old package.
Do not run a database migration for stage history or old commit payloads.

Fresh databases must create only the new tables and required columns.
Existing old databases are outside the accepted project format.
Do not keep a production reader for old history rows.

Golden tests must cover:

- project metadata;
- full document JSON;
- root JSON;
- commit JSON and hash bytes;
- WAL bytes;
- checkpoint JSON;
- transfer package JSON.

### 4.14 Paste-only transfer

Define a new portable transfer package.
It contains only transferable edit data.

The package contains:

- ordered Color Grade nodes;
- each Grade's adjustments, enabled value, mix, and Masks;
- immutable Mask asset descriptors and source keys;
- DRT/Post transferable parameters;
- source package schema and document format;
- a canonical package fingerprint.

The package does not contain:

- source Develop data;
- source RAW metadata;
- source camera profile or lens identity;
- source document geometry;
- source root ID;
- source Version IDs or commit IDs;
- source active Version;
- source history rows;
- source UI selection or graph layout.

Paste uses this order:

1. Validate the source package and all source assets.
2. Read the target immutable root document.
3. Keep the target Develop endpoint and image-specific values.
4. Keep the target document geometry.
5. Import the Color Grade chain and DRT/Post transferable values.
6. Remap every `NodeId`, `AdjustmentInstanceId`, and `MaskId`.
7. Copy or reuse each immutable Mask asset in the target asset store.
8. Validate all new IDs, owners, edges, and asset references.
9. Create one new named Version at the target root.
10. Apply one typed Paste batch to the target live document.
11. Append WAL and publish the new Version head.
12. Set the new Version active.
13. Save through the normal checkpoint path.
14. Schedule one `PastedPipelineDocument` Quality render.

The Paste batch can contain several typed changes.
It still creates one commit for the user action.
Do not create one commit per imported operator.

Build the complete Paste ID map before live mutation.
Use an injectable ID source for deterministic tests.
Repeated Paste must create different target IDs.
The map must not reuse a source or target identity.
It must reject a collision before live mutation.

If Paste fails, remove no prior target data.
Do not leave an empty Version.
Do not move the active Version.
Do not leave a commit or WAL tail.

Content-addressed asset publication can finish before the Version publish.
An unreferenced equal asset is harmless.
A new unreferenced asset can remain until audited maintenance removes it.

### 4.15 Remove product merge paths

Remove all product actions that begin or complete a pipeline merge.
Remove merge conflict state from Adjustment Transfer UI and controllers.
Remove service entry points that create new two-parent pipeline commits.

New project loaders reject old merge payloads.
The new transfer package has no merge mode.

Keep Version branching and shared ancestors.
These features do not require merge commits.

The storage column for a second parent can remain only if database cleanup is unsafe in this phase.
No new product operation can write it.
Document any retained unused column in the completion record.

Do not add a hidden automatic merge.
Do not convert Merge into Apply-on-current-Version.
Paste always creates a new Version.

### 4.16 Mask asset reachability

Create one GPU-free reachability scanner.
It returns the set of referenced `MaskAssetKey` values.

The scan includes:

- every immutable root document;
- every commit reachable from every Version head;
- the active working head and redo suffix;
- the current live document;
- every valid unmaterialized WAL record;
- every checkpoint that storage can still select;
- active Mask authoring values when NM7 supplies them.

NM4 must provide the extension point for NM7 active authoring values.
NM4 tests can use an empty active-authoring set.

Do not infer asset use from GPU or host caches.
Do not delete an asset because it is absent from the active Version only.

Asset deletion is a separate clean-exit or maintenance operation.
It runs only after a successful final checkpoint.
It waits until no active editor or recovery operation can add a reference.

For each deletion candidate:

1. Derive the key from its file name and validate the asset file.
2. Confirm that the final reachable set does not contain the key.
3. Remove only that exact asset path.
4. Report a deletion failure.

Do not recursively delete the Mask store root.
Do not delete corrupt files silently.

### 4.17 History projection and render behavior

Project history rows from the typed batch.
Do not parse an operator stage to find the target.

The projection exposes stable data:

- operation kind;
- node ID and saved display name where applicable;
- adjustment instance ID;
- Mask ID and saved display name where applicable;
- stable field key;
- typed before and after display values;
- localization key and arguments;
- commit and timeline position.

Do not store final localized strings in the commit.
Do not resolve a deleted node name from the current document.
Use the saved presentation data from the batch.

Increment the history revision once per successful head change.
Do not rebuild history rows for render progress or busy state.

Render rules:

| Successful action | Render behavior |
| --- | --- |
| Parameter commit | One Quality render after the settled action. |
| Add, remove, or reconnect Grade | One `GraphTopologyChanged` Quality render. |
| Add, remove, or change Mask | One `SettledMaskEdit` Quality render. |
| Replace Mask asset | One `SettledMaskEdit` Quality render. |
| Undo or Redo of pixel data | One Quality render. |
| Rename-only action | No render. |
| Version checkout | One `VersionDocumentChanged` Quality render. |
| Paste | One `PastedPipelineDocument` Quality render. |
| Failed action | No render intent. |

### 4.18 Concurrency and failure rules

Use the existing image-scoped save lock and render lock.
Do not add a global history lock across images.

Lock order must be explicit.
No path can hold the render lock while it waits for a save worker that needs that lock.
No path can hold the journal mutex during DuckDB I/O.

Capture immutable save data on the caller thread.
Move the capture into the worker.
Do not let the worker read the live document again.

Use a session generation on async completion.
Ignore a stale completion after an image or Version switch.

Fail with the actual error for:

- malformed typed payload;
- wrong owner or missing identity;
- graph invariant failure;
- missing or corrupt Mask asset;
- WAL append or truncate failure;
- DuckDB write failure;
- checkpoint label mismatch that cannot replay;
- Version restore failure;
- transfer package or ID-remap failure.

Do not continue on a stage table.
Do not use a prior checkpoint as a hidden replacement for a failed replay.
Do not switch backend or quality.

### 4.19 Code naming rule

Do not put `NM4`, `Phase 4`, or another phase identifier in code.
This rule applies to production and test code.
It includes filenames, targets, identifiers, comments, strings, and generated files.

Name each artifact for its behavior.
Examples include `pipeline_edit_batch`, `pipeline_history_applier`, `document_checkpoint`, `document_transfer`, and `mask_asset_reachability`.

## 5. Transition boundaries

### 5.1 Boundary from NM1

Keep one live `PipelineDocument` and one shared executor.
Keep the existing render lock and image access rules.
Keep current graph commands and local restoration behavior.

Replace NM1.3 same-session parameter history with typed batches.
Do not restore a whole-document candidate or a stage write path.

### 5.2 Boundary from NM2

Keep ordered multi-Grade ownership and execution.
History changes node and adjustment values only through stable IDs.

NM4 does not change native Grade execution.
It can trigger static-plan rebuilds through existing topology dirty rules.

### 5.3 Boundary from NM3

Use the final `MaskId`, `MaskSource`, `MaskAssetKey`, and `MaskModel` values.
Store them directly in typed changes.

NM4 owns asset references, Undo, Redo, recovery, Paste, and reachability.
NM4 does not create provisional raster pixels.

### 5.4 Boundary to NM5

NM4 supplies application commands with durable typed history behavior.
NM5 connects QuickQanava actions to those commands.

NM5 must not create commits from QML.
NM5 must not edit `PipelineGraph` containers directly.

### 5.5 Boundary to NM6

NM4 accepts complete `EditorParameterTarget` values for any valid node owner.
NM6 supplies the selected node and adjustment instance.

NM4 history replay never reads current selection.

### 5.6 Boundary to NM7

NM4 supplies typed Mask changes and `ReplaceMaskAsset`.
NM7 supplies pointer samples, active raster data, and the settled asset.

NM7 calls one NM4 command after `MaskStore::Put()` succeeds.
Escape produces no batch.

### 5.7 Boundary to NM8

NM4 proves model, service, storage, recovery, and project reopen behavior.
NM8 proves the full packaged product path with native rendering and real RAW files.

NM4 does not claim package or real-RAW UI qualification.

## 6. Sub-phase sequence

| Phase | Status | Result |
| --- | --- | --- |
| NM4.1 | complete | Typed batch schema, canonical encoding, commit identity, and presentation data. |
| NM4.2 | planned | Reversible live-document apply, WAL publication, Undo, Redo, and head moves. |
| NM4.3 | planned | Immutable root document, unified format cutover, and full-document checkpoint. |
| NM4.4 | planned | Version branch, checkout, recovery, materialization, and reopen. |
| NM4.5 | planned | Paste-only transfer, merge-path removal, and Mask asset reachability. |
| NM4.6 | planned | Failure, concurrency, service, storage, and project qualification. |

Implement these phases in order.
Keep each sub-phase buildable and testable.
Do not expose new product UI during NM4.

### 6.1 NM4.1 — Typed batch and commit identity

**Required context**

Read Sections 2, 3, 4.3–4.6, and 4.17.
Read the NM3 model types before defining Mask payload fields.

This sub-phase changes stored commit meaning.
It does not connect the new payload to the live editor yet.

**Work**

1. Define `PipelineEditBatch` and all first-release change variants.
2. Define stable operation kinds and localization keys.
3. Define canonical JSON for the batch and each variant.
4. Define strict `FromJson` validation.
5. Define forward and inverse direction helpers without document mutation.
6. Replace commit creation input with one typed batch on the new path.
7. Advance commit and chain hash input versions.
8. Reject ordinary stage payloads and merge payloads in the new format.
9. Update commit graph validation and persistence mapping.
10. Update history projection data to carry typed targets and display values.
11. Add golden JSON, golden byte, golden hash, fuzz, and malformed-input tests.

**Primary files**

- [edit_commit.hpp](../../../../../alcedo_studio/src/include/edit/history/edit_commit.hpp)
- [edit_commit.cpp](../../../../../alcedo_studio/src/edit/history/edit_commit.cpp)
- [commit_types.hpp](../../../../../alcedo_studio/src/include/edit/history/commit_types.hpp)
- [commit_graph.hpp](../../../../../alcedo_studio/src/include/edit/history/commit_graph.hpp)
- [commit_graph_store.cpp](../../../../../alcedo_studio/src/storage/store/edit_history/commit_graph_store.cpp)
- [editor_history_types.hpp](../../../../../alcedo_studio/src/include/app/editor_history_types.hpp)
- [editor_history_projection.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_projection.cpp)
- [editor_history_commit_presentation.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_commit_presentation.cpp)

Create focused payload files if `edit_commit.hpp` becomes multi-purpose.
Keep serialization separate from UI presentation.

**Primary success call chain**

```text
typed user-action fixture
  -> normalize complete typed changes
  -> PipelineEditBatch::Validate
  -> canonical JSON and canonical hash bytes
  -> EditCommit::MakePipelineEdit
  -> finalized commit hash
  -> CommitGraph::InsertCommit
  -> DuckDB row mapping
  -> load and compare identical typed batch
```

**Primary failure call chain**

```text
missing ID / unknown kind / wrong owner / non-finite value / malformed node or Mask
  -> typed batch validation fails
  -> no commit hash is finalized
  -> no graph or DuckDB row changes
```

**Tests and evidence**

- Fixed golden for one parameter batch.
- Fixed golden for one node deletion batch with Masks.
- Fixed golden for one reconnect batch.
- Fixed golden for one Brush asset replacement batch.
- Round-trip for every change variant.
- Unknown and duplicate field rejection.
- Mutation-order hash sensitivity.
- Locale-independent hash equality.
- Commit and chain hash change from the old format.
- Fuzz parse with no accepted non-canonical payload.

**Exit conditions**

- [x] Every first-release action has one typed change representation.
- [x] Each change has complete identity and before/after data.
- [x] Canonical JSON and hash bytes have independent golden values.
- [x] Change order is preserved and affects the commit hash.
- [x] Old ordinary and merge payloads fail in the new format.
- [x] History projection does not require a stage or operator identity.
- [x] No live document mutation uses the new payload yet.

##### NM4.1 completion record (2026-09-01)

**Status:** complete — typed `PipelineEditBatch` schema, canonical JSON, commit/chain format v2, history projection data; live apply still uses ordinary payloads.

**Primary success call chain:**

```text
typed user-action fixture
  -> PipelineEditBatch::Make / Validate
  -> CanonicalJSON dump (frozen golden files)
  -> EditCommit::MakePipelineEdit
  -> CanonicalHashInput + Hash128 (kCommitFormatVersion = 2)
  -> CommitGraph::InsertCommit
  -> CommitGraphStore::Materialize
  -> LoadGraph + PipelineEditBatch::FromJSON identical dump
```

**Primary failure call chain:**

```text
missing ID / unknown kind / extra key / non-finite / ordinary or merge payload
  -> PipelineEditBatch::Make or FromJSON throws
  -> EditCommit::MakePipelineEdit does not finalize a hash
  -> no CommitGraph or DuckDB row is written from the failed batch
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `TypedBatchGoldenBytesAndHashRemainStable` | `PipelineEditBatchTest` | PASS |
| `RemoveColorGradeGoldenBytesRemainStable` (node deletion with Masks) | `PipelineEditBatchTest` | PASS |
| `ReconnectGoldenBytesRemainStable` | `PipelineEditBatchTest` | PASS |
| `BrushAssetReplacementGoldenBytesRemainStable` | `PipelineEditBatchTest` | PASS |
| `RoundTripForEveryChangeVariant` | `PipelineEditBatchTest` | PASS |
| `ChangingTypedChangeOrderChangesCommitIdentity` | `PipelineEditBatchTest` | PASS |
| `UnknownOrMissingTypedPayloadFieldsAreRejected` | `PipelineEditBatchTest` | PASS |
| `ParameterHistoryRequiresCompleteOwnerNodeAndInstance` | `PipelineEditBatchTest` | PASS |
| `OrdinaryAndMergePayloadsAreRejected` | `PipelineEditBatchTest` | PASS |
| `LocaleIndependentHashEquality` | `PipelineEditBatchTest` | PASS |
| `FuzzParseRejectsNonCanonicalPayloads` | `PipelineEditBatchTest` | PASS |
| `TypedHistoryRowsUseSavedIdentityAndLocalizationData` | `PipelineEditBatchTest` | PASS |
| `GraphInsertAndOrdinaryHashFormatChanged` | `PipelineEditBatchTest` | PASS |
| `EditCommitHashing.FixedHashVectorsAreStable` | `CommitGraphTest` | PASS |
| `CommitGraphPersistenceTests.TypedPipelineEditBatchRoundTripsThroughStore` | `CommitGraphTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target PipelineEditBatchTest CommitGraphTest --parallel 4
.\alcedo_studio\tests\edit\PipelineEditBatchTest_runtime\PipelineEditBatchTest.exe
.\alcedo_studio\tests\edit\CommitGraphTest_runtime\CommitGraphTest.exe
```

Suite totals: PipelineEditBatchTest 16/16 PASS; CommitGraphTest 35/35 PASS (includes MiniGit ordinary-path regression under format v2).

**Checklist / exit condition:** all NM4.1 boxes checked from the tests above. Live mutation remains ordinary (`editor_history_mutation.cpp`); `OrderedChangesForApply` only reorders stored changes.

**LOC note (grill-code-review):** `pipeline_edit_batch.cpp` is 1245 lines (schema, per-variant JSON, validation). That is one payload module; splitting change codecs into files without owned state would be a method split. The next owned-state module is the NM4.2 applier. Header 337 lines. Test 610 lines. `commit_graph_test.cpp` 967 lines.

**Remaining gaps:** NM4.2 live apply / WAL / Undo. NM4.3 project and `kImageEditSchemaVersion` cutover. NM4.5 merge removal and paste product. Dual path remains: `EditCommit` still accepts `OrdinaryEditPayload` when `batch_format_version` is absent. `CommitRowFromEdit` is wired for typed batches but was not executed in a UI-port binary this phase; domain `ProjectPipelineEditHistory` was. nlohmann last-wins duplicate object keys; extra keys are rejected. QML history roles for the new `EditorHistoryCommit` fields are not added.

### 6.2 NM4.2 — Reversible live-document history

**Required context**

Read Sections 4.1, 4.6–4.9, 4.17, and 4.18.
Read the current preview capture and head-move code before editing it.

This sub-phase makes typed batches authoritative in memory and WAL.
It still uses the current development project format until NM4.3.

**Work**

1. Add the application-layer batch applier.
2. Add exact graph and Mask restoration helpers.
3. Validate the expected current side before every change.
4. Roll back earlier changes when a later change fails.
5. Route settled parameter history through `PipelineEditBatch`.
6. Add service commands for graph and Mask actions without product UI.
7. Change mini-Git edit records to carry typed batch commits.
8. Apply Undo, Redo, and direct head moves through typed batches.
9. Keep provisional preview outside history.
10. Publish one history revision and one render intent after success.
11. Remove active-editor use of `OrdinaryEditPayload`.
12. Add lock, rollback, WAL-failure, and projection tests.

**Primary files**

- New application-layer batch applier and command service files.
- [editor_history_mutation.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp)
- [mini_git_working_history.hpp](../../../../../alcedo_studio/src/include/edit/history/mini_git_working_history.hpp)
- [mini_git_working_history.cpp](../../../../../alcedo_studio/src/edit/history/mini_git_working_history.cpp)
- [pipeline_graph_commands.cpp](../../../../../alcedo_studio/src/edit/graph/pipeline_graph_commands.cpp)
- [editor_pipeline_command_service.cpp](../../../../../alcedo_studio/src/app/editor_pipeline_command_service.cpp)
- [editor_session_history_port.hpp](../../../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp)
- [editor_session_history_port.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp)

Keep model mutation outside the journal and projection classes.

**Primary success call chain**

```text
service command or settled parameter input
  -> capture complete typed before and after data
  -> apply batch to the one live document under render lock
  -> validate the result
  -> MiniGitWorkingHistory prepares commit
  -> journal appends complete commit record
  -> CommitGraph publishes commit and active head
  -> committed snapshot and typed history projection update
  -> one Quality render intent
```

**Undo and Redo call chain**

```text
Undo or Redo
  -> prepare head move and traversed typed commits
  -> validate current side and asset references
  -> append head-move WAL record
  -> apply inverse or forward batches under render lock
  -> publish head, redo selection, projections, and one Quality render
```

**Primary failure call chain**

```text
domain apply / graph validation / WAL append / head publication fails
  -> inverse-apply changed local values
  -> revoke only the failed WAL tail when required
  -> restore prior head and redo selection
  -> preserve document hash and projection revision
  -> no render intent
  -> report the actual error
```

**Tests and evidence**

- Parameter preview settles to one typed commit.
- Add Grade creates one commit and Undo removes it.
- Remove Grade Undo restores exact node JSON and edges.
- Reconnect Undo restores exact order.
- Add and remove Mask restore exact display index and data.
- Replace Mask asset switches only immutable references.
- Multi-change batch failure rolls back earlier changes.
- WAL append failure restores document and head.
- Head publish failure revokes the WAL tail.
- Rename commit creates no render.
- Pixel-changing batch creates one Quality render.
- Render lock blocks concurrent read until batch completion.

**Exit conditions**

- [ ] Active editor commits use typed batches only.
- [ ] Forward then inverse restores the canonical document hash.
- [ ] Inverse then forward restores the same hash.
- [ ] Add, remove, reconnect, parameter, and Mask changes Undo and Redo correctly.
- [ ] One user action creates one commit and one chain fold.
- [ ] Failed changes preserve document, head, redo, projection, and render state.
- [ ] Preview does not enter WAL or history.

### 6.3 NM4.3 — Root, formats, and checkpoint document

**Required context**

Read Sections 4.2, 4.10, 4.11, 4.13, and 4.18.
Read the current project-open checks and DuckDB history tables.

This sub-phase performs the incompatible format cutover.
After this sub-phase, old development projects must fail at open.

**Work**

1. Fix the exact new format constants in one table.
2. Advance project metadata and packed-project acceptance values.
3. Advance the full document format.
4. Advance image edit, root, checkpoint, WAL, and transfer schema values.
5. Store the complete immutable root document.
6. Bind `root_id` to the canonical root document and image owner.
7. Store the complete checkpoint document with one materialization label.
8. Replace checkpoint parameter snapshots with full document JSON.
9. Update DuckDB schema, mappers, and materialization capture.
10. Check project metadata before storage history load.
11. Reject every old format without conversion.
12. Delete active product reads of the old pipeline parameter checkpoint.
13. Add format golden files and open-boundary tests.

**Primary files**

- [project_package_backend.hpp](../../../../../alcedo_studio/src/include/app/project_package_backend.hpp)
- [project_package_backend.cpp](../../../../../alcedo_studio/src/app/project_package_backend.cpp)
- [project_service.cpp](../../../../../alcedo_studio/src/app/project_service.cpp)
- [pipeline_document.hpp](../../../../../alcedo_studio/src/include/edit/graph/pipeline_document.hpp)
- [pipeline_document.cpp](../../../../../alcedo_studio/src/edit/graph/pipeline_document.cpp)
- [commit_types.hpp](../../../../../alcedo_studio/src/include/edit/history/commit_types.hpp)
- [database.hpp](../../../../../alcedo_studio/src/include/storage/store/database.hpp)
- [commit_graph_store.cpp](../../../../../alcedo_studio/src/storage/store/edit_history/commit_graph_store.cpp)
- [editor_history_checkpoint.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_checkpoint.cpp)
- [editor_mini_git_materializer.cpp](../../../../../alcedo_studio/src/app/editor_mini_git_materializer.cpp)
- [pipeline_service.cpp](../../../../../alcedo_studio/src/app/pipeline_service.cpp)

**Primary success call chain**

```text
new project and imported image
  -> create full default document with image Develop data
  -> canonical root document and root_id
  -> persist immutable root
  -> typed edits change the live document and history head
  -> checkpoint capture reads one graph materialization and document
  -> one DuckDB transaction stores graph, refs, image state, and document checkpoint
  -> WAL truncates only after commit
```

**Matching checkpoint load chain**

```text
supported project metadata
  -> load history and immutable root
  -> checkpoint root/head/chain labels match active Version
  -> validate and load full PipelineDocument
  -> skip replay
```

**Primary failure call chain**

```text
old project or old document/history/root/checkpoint/WAL version
  -> reject at the earliest owning boundary
  -> no conversion and no partial database writes

checkpoint label or document validation failure
  -> history remains authoritative
  -> later replay path handles the supported data
  -> actual error remains visible if replay also fails
```

**Tests and evidence**

- New project metadata golden.
- New full document golden with multiple Grades and Masks.
- Root golden with image-specific Develop data.
- Checkpoint golden with root, head, chain, and document.
- Matching checkpoint skips replay.
- Stale checkpoint marks writeback after replay.
- Wrong-root checkpoint is never loaded.
- Old `0.3.0` metadata fails before history load.
- Old document, commit, root, checkpoint, and WAL values fail.
- DuckDB failure leaves WAL and prior durable state.
- WAL truncate failure leaves recoverable durable state.

**Exit conditions**

- [ ] One published format table identifies every new version constant.
- [ ] Root and checkpoint store full documents, not CPU parameter tables.
- [ ] One capture provides checkpoint head and chain labels.
- [ ] New projects create only the new history shape.
- [ ] Old projects fail before history or pipeline load.
- [ ] No old payload or stage conversion path remains active.
- [ ] Save failure preserves the prior durable state and WAL.

### 6.4 NM4.4 — Version, recovery, and reopen

**Required context**

Read Sections 4.9–4.12 and 4.18.
Read the current navigation save barrier and recovery implementation.

This sub-phase proves that every Version is one replayable DAG.
It does not add new Versions UI.

**Work**

1. Rebuild active Version from root and first-parent typed batches.
2. Use a matching full-document checkpoint when valid.
3. Update root Version creation and branch creation.
4. Update Version checkout to replay the same live document.
5. Restore the prior Version after a target replay failure.
6. Update WAL recovery for typed commits and head moves.
7. Validate every referenced Mask asset before head publication.
8. Materialize recovered state through the normal checkpoint path.
9. Reopen a saved project and compare document, head, chain, refs, and assets.
10. Preserve branch sharing and redo behavior.
11. Add crash-window, missing-commit, missing-asset, and stale-session tests.

**Primary files**

- [pipeline_service.cpp](../../../../../alcedo_studio/src/app/pipeline_service.cpp)
- [editor_session_pipeline_port.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_pipeline_port.cpp)
- [editor_history_version_refs.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_version_refs.cpp)
- [editor_history_mutation.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp)
- [editor_mini_git_journal_recovery.cpp](../../../../../alcedo_studio/src/app/editor_mini_git_journal_recovery.cpp)
- [editor_mini_git_materializer.cpp](../../../../../alcedo_studio/src/app/editor_mini_git_materializer.cpp)
- [editor_session_navigation_controller.cpp](../../../../../alcedo_studio/src/app/editor_session_navigation_controller.cpp)

**Primary success call chain**

```text
Version checkout request
  -> finish or cancel provisional input
  -> save checkpoint barrier
  -> resolve target Version and typed first-parent chain
  -> verify persistent Mask assets
  -> reset the same live document to immutable root
  -> apply batches in order under render lock
  -> validate final DAG
  -> publish active Version, history, document, and adjustment snapshots
  -> one VersionDocumentChanged Quality render
```

**Recovery call chain**

```text
supported project open with non-empty WAL
  -> load durable graph, root, and checkpoint
  -> validate WAL commit and head-move chain
  -> skip durable covered prefix
  -> apply valid missing suffix to graph and live document
  -> one agreeing head, chain, and document
  -> normal checkpoint materialization
  -> clear only materialized WAL records
```

**Primary failure call chain**

```text
target commit / batch / asset / graph validation fails
  -> reset same live document to immutable root
  -> replay prior Version chain
  -> restore prior active Version and redo selection
  -> keep prior projections and displayed frame
  -> no target render
  -> report the actual target error
```

**Tests and evidence**

- Root Version always loads the exact default root document.
- Two Versions keep different Grade chains and Masks.
- Branch from a commit shares ancestry without copying commits.
- Checkout A to B to A restores exact document hashes.
- Matching checkpoint skips replay after reopen.
- Stale checkpoint replays and writes a new checkpoint.
- Undo, branch, checkout, and Redo preserve intended selection.
- Missing reachable commit fails closed.
- Missing or corrupt reachable Mask asset fails closed.
- Crash after WAL append replays once.
- Crash after DuckDB commit and before WAL truncate does not duplicate a commit.
- Stale async completion does not change the current image or Version.

**Exit conditions**

- [ ] Every Version reconstructs one full DAG from root and typed commits.
- [ ] Root Version always equals the immutable root document.
- [ ] Checkout uses one live document and no candidate document.
- [ ] Checkout failure restores the prior Version and document.
- [ ] Recovery produces one agreeing head, chain, and document.
- [ ] Reopen preserves nodes, edges, parameters, Masks, refs, and asset keys.
- [ ] Missing data fails with no stage or checkpoint substitution.

### 6.5 NM4.5 — Paste-only and asset reachability

**Required context**

Read Sections 4.14–4.16.
Read current Adjustment Transfer capture, controller, and QML actions.

This sub-phase removes new pipeline merge behavior.
It keeps one explicit Paste action.

**Work**

1. Define the new transfer document schema.
2. Capture complete transferable Grade, Mask, and DRT/Post data.
3. Exclude Develop, RAW data, geometry, history, and UI state.
4. Validate package format, owners, graph order, and asset references.
5. Implement complete target ID remapping.
6. Copy or reuse content-addressed Mask assets.
7. Build one typed Paste batch.
8. Create and activate one new target Version only after validation.
9. Route Paste through WAL, history, checkpoint, and render paths.
10. Remove live and batch merge creation APIs.
11. Remove merge conflict controller and QML paths.
12. Add the Mask asset reachability scanner.
13. Add safe clean-exit or maintenance deletion.
14. Add Paste rollback and asset reachability tests.

**Primary files**

- [adjustment_transfer_types.hpp](../../../../../alcedo_studio/src/include/app/adjustment_transfer_types.hpp)
- [adjustment_transfer_service.hpp](../../../../../alcedo_studio/src/include/app/adjustment_transfer_service.hpp)
- [adjustment_transfer_service.cpp](../../../../../alcedo_studio/src/app/adjustment_transfer_service.cpp)
- [editor_history_transfer.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp)
- [adjustment_transfer_controller.cpp](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/adjustment_transfer_controller.cpp)
- Transfer QML actions and dialogs found by the implementation audit.
- New GPU-free Mask asset reachability files.
- [mask_store.hpp](../../../../../alcedo_studio/src/include/edit/mask/mask_store.hpp)

**Primary success call chain**

```text
Paste request with validated transfer document
  -> read target root and target image data
  -> map every node, adjustment, and Mask identity
  -> publish or reuse immutable Mask assets
  -> build and validate target document changes
  -> create new Version at root
  -> apply one typed Paste batch to the same live document
  -> append WAL and publish one commit
  -> activate new Version
  -> normal checkpoint save
  -> one PastedPipelineDocument Quality render
```

**Asset scan call chain**

```text
successful final checkpoint and clean-exit maintenance
  -> collect root, Version, live, redo, WAL, checkpoint, and active-authoring references
  -> validate asset files
  -> compare exact keys with the reachable set
  -> remove only unreachable exact files
  -> report every failure
```

**Primary failure call chain**

```text
invalid package / missing asset / ID collision / graph failure / WAL failure / save failure
  -> restore prior target document, head, refs, and active Version
  -> remove no prior assets or commits
  -> leave no empty Paste Version
  -> leave no published render intent
  -> report the actual error
```

**Tests and evidence**

- Paste preserves target Develop and RAW metadata.
- Paste preserves target geometry.
- Paste imports ordered Grades, adjustments, Masks, and DRT/Post values.
- Every imported stable ID differs from the source and target existing IDs.
- Equal Mask bytes reuse one content key.
- Different Mask bytes remain distinct.
- Paste creates one Version and one commit.
- Paste does not inherit the prior active Version ancestry.
- Paste failure creates no Version, commit, head move, or render.
- No product API creates a two-parent commit.
- No QML merge action or conflict state remains.
- Reachability retains assets from inactive Versions and WAL.
- Reachability removes only an unreferenced exact file.
- Corrupt asset deletion candidates report an error.

**Exit conditions**

- [ ] Transfer packages contain complete portable DAG data.
- [ ] Paste creates one new Version and one typed batch commit.
- [ ] Target Develop, RAW data, and geometry remain unchanged.
- [ ] All imported IDs are remapped before publication.
- [ ] Paste failure leaves no partial Version or head move.
- [ ] No product merge creation or conflict UI remains.
- [ ] Asset reachability includes every durable and recovery owner.
- [ ] Asset deletion is separate from rendering and reports failures.

### 6.6 NM4.6 — Qualification

**Required context**

Read Sections 7–9.
Check all current target names before the build.

This sub-phase changes no product behavior unless a test finds a defect.
It records complete NM4 evidence.

**Work**

1. Run the full acceptance matrix in Section 7.
2. Run focused model, history, service, storage, and UI-port tests.
3. Run format and project-open tests.
4. Run crash-window and recovery tests.
5. Run Paste and Mask asset reachability tests.
6. Run concurrent save, checkout, and edit tests.
7. Run full affected suites after focused tests pass.
8. Record source revision, commands, counts, timings, and failures.
9. Record main success and failure call chains.
10. Scan production code for old payload, merge creation, and stage replay paths.
11. Scan code names for phase identifiers and banned project terms.
12. Leave unavailable package or real-RAW UI evidence for NM8.

**Primary files**

Use the test targets in Section 7.3.
Add focused test support files by behavior.
Do not add one large phase-named test helper.

**Primary success call chain**

```text
fresh supported project
  -> immutable root document
  -> typed parameter, graph, and Mask commits
  -> Undo, Redo, branch, and Version checkout
  -> checkpoint and close
  -> reopen with exact DAG and assets
  -> Paste into a second image Version
  -> final save and clean-exit asset scan
```

**Primary failure call chain**

```text
injected apply / WAL / DuckDB / truncate / replay / asset / Paste failure
  -> one explicit error
  -> prior document, head, refs, assets, and displayed frame remain valid
  -> no legacy or reduced replacement path
```

**Exit conditions**

- [ ] Every required behavior has executed evidence on the current host.
- [ ] Storage crash windows do not duplicate or lose a committed edit.
- [ ] Reopen produces the same canonical document and history labels.
- [ ] Paste and asset reachability pass complete service integration tests.
- [ ] Old formats fail at their owning boundaries.
- [ ] Repository scans find no active old payload or product merge creation path.
- [ ] Missing package, native render, or real-RAW UI evidence remains explicit for NM8.

## 7. Acceptance matrix

The names below specify assertion goals.
Use these names or equally precise behavior names.
Do not put a phase identifier in a test name, target, file, or fixture.

| Required behavior | Evidence and assertion goal |
| --- | --- |
| Batch canonical form | `TypedBatchGoldenBytesAndHashRemainStable`: compare independent fixed JSON, bytes, commit hash, and chain hash. |
| Batch order | `ChangingTypedChangeOrderChangesCommitIdentity`: use noncommuting changes and compare hashes. |
| Strict parse | `UnknownOrMissingTypedPayloadFieldsAreRejected`: cover every variant and version field. |
| Target identity | `ParameterHistoryRequiresCompleteOwnerNodeAndInstance`: do not infer from field or selection. |
| Parameter inverse | `ParameterForwardInverseRestoresDocumentHash`: cover Document, Develop, Grade, and DRT/Post. |
| Add Grade | `AddGradeUndoRedoPreservesStableIdsAndCleanValues`: compare node and adjustment IDs. |
| Delete Grade | `DeleteGradeUndoRestoresNodeMasksAndExactEdges`: compare full node JSON and edge order. |
| Reconnect Grade | `ReconnectUndoRedoRestoresBackboneOrder`: use noncommuting Grade parameters. |
| Rename Grade | `RenameCreatesHistoryWithoutRenderIntent`: compare document metadata and render count. |
| Add and remove Mask | `MaskAddRemoveUndoRestoresValueAndDisplayIndex`: cover three source kinds. |
| Mask source | `MaskSourceUndoRestoresExactVariantValues`: cover Radial and Linear Gradient values. |
| Raster key | `BrushAssetUndoSwitchesImmutableKeysWithoutChangingFiles`: load both old and new assets. |
| One action | `MultiChangeActionCreatesOneCommitAndOneChainFold`: use node deletion or Paste. |
| Local rollback | `LaterChangeFailureReversesEarlierBatchChanges`: inject a failure after one valid change. |
| WAL failure | `JournalAppendFailureRestoresDocumentHeadAndProjection`: compare all state revisions. |
| Head publish failure | `HeadPublishFailureRevokesOnlyNewJournalTail`: preserve earlier records. |
| Undo and Redo | `UndoRedoAppliesTypedBatchesInRequiredOrder`: traverse more than one commit. |
| Direct move | `MoveToAncestorAndRedoChildUsesStoredDirections`: compare document hashes at each tip. |
| Immutable root | `ImageRootStoresCompleteDefaultDocumentAndDevelopData`: change product defaults after fixture creation. |
| Root identity | `DifferentImageDevelopDataProducesDifferentRootIdentity`: keep all other values equal. |
| Checkpoint match | `MatchingDocumentCheckpointSkipsFirstParentReplay`: inspect replay count. |
| Checkpoint mismatch | `StaleDocumentCheckpointReplaysHistoryAndNeedsWriteback`: compare final document. |
| Wrong root | `CheckpointForAnotherImageNeverLoads`: preserve target state. |
| Root Version | `RootVersionAlwaysRebuildsExactImmutableDocument`: edit every supported owner first. |
| Branch | `BranchVersionSharesCommitsAndKeepsIndependentHead`: compare graph object count and heads. |
| Checkout | `VersionCheckoutReplacesTheDagOnTheSameLiveGuard`: compare guard identity and document hash. |
| Checkout failure | `FailedCheckoutRestoresPriorVersionAndDocument`: inject missing asset and invalid batch cases. |
| Recovery suffix | `RecoveryAppliesCommittedTypedSuffixExactlyOnce`: stop after WAL append. |
| Commit-before-truncate crash | `CoveredWalAfterDatabaseCommitDoesNotDuplicateHistory`: reopen twice. |
| Missing commit | `MissingReachableTypedCommitFailsClosed`: no checkpoint substitution. |
| Missing asset | `MissingReachableMaskAssetFailsBeforeHeadPublication`: preserve active Version. |
| Project cutover | `OldProjectMetadataFailsBeforeHistoryLoad`: prove no DuckDB history query. |
| Old subformats | `OldDocumentCommitRootCheckpointAndWalFormatsFail`: test each owning reader. |
| Reopen | `ProjectReopenPreservesDagVersionsHistoryAndMaskAssets`: compare canonical values. |
| Paste owner | `PasteKeepsTargetDevelopRawDataAndGeometry`: compare target values before and after. |
| Paste identity | `PasteRemapsEveryNodeAdjustmentAndMaskId`: compare source and target sets. |
| Paste history | `PasteCreatesOneRootRelativeVersionAndOneTypedCommit`: inspect ancestry and commit count. |
| Paste failure | `FailedPasteCreatesNoVersionCommitHeadMoveOrRender`: inject each publication boundary. |
| No merge | `TransferSurfaceHasNoPipelineMergeOperation`: compile and API scan plus service test. |
| Asset reachability | `InactiveVersionAndWalKeepReferencedMaskAssets`: run the clean-exit scanner. |
| Asset deletion | `MaintenanceRemovesOnlyUnreferencedExactMaskAssetFiles`: preserve reachable and corrupt files. |
| Projection | `TypedHistoryRowsUseSavedIdentityAndLocalizationData`: delete the live node before projection. |
| Locking | `ConcurrentRenderCannotObservePartialTypedBatch`: block after the first change. |
| Stale completion | `OldSessionCheckpointCompletionCannotPublishIntoNewVersion`: change session generation. |

### 7.1 Golden and fixture requirements

Use deterministic IDs and timestamps for golden tests.
Do not obtain expected bytes from the encoder under test.

Keep permanent golden files in the established test resource directories.
Store temporary logs under `build/tmp/node_history/`.

The main document fixture must include:

1. One image-specific Develop endpoint.
2. Three Color Grades with noncommuting adjustments.
3. One renamed Grade.
4. One disabled Grade and one non-default mix.
5. Brush, Radial, and Linear Gradient Masks.
6. Two immutable Brush assets.
7. Non-default DRT/Post values.
8. A branch with shared ancestry.
9. One stale checkpoint case.
10. One valid unmaterialized WAL suffix.

Use fixed Mask bytes.
Verify their content keys independently.

### 7.2 Failure and crash matrix

Run each failure at the named boundary.
Check document, history, refs, WAL, storage, assets, projections, and render count.

| Boundary | Required result |
| --- | --- |
| Typed parse | Reject before commit creation. |
| Domain change 1 | No document change. |
| Domain change after an earlier success | Reverse the earlier local change. |
| Graph final validation | Restore exact nodes and edges. |
| Mask asset load | Keep the prior head and document. |
| WAL append | Restore live data and preserve earlier WAL records. |
| Commit graph publish | Revoke only the new WAL tail and restore live data. |
| Save capture | Start no worker and change no materialized state. |
| DuckDB write before commit | Keep prior durable state and all WAL data. |
| DuckDB commit before WAL truncate | Reopen without duplicate apply. |
| WAL truncate | Keep recoverable state and report failure. |
| Target Version replay | Restore prior Version and document. |
| Prior Version restoration | Enter fatal session error and publish no replacement path. |
| Paste asset publication | Create no Version or commit. |
| Paste Version creation | Remove the new ref if later publication fails. |
| Paste WAL publish | Restore prior active Version and document. |
| Asset maintenance deletion | Preserve the file and report failure. |

### 7.3 Existing targets and commands

Check target names after source changes.
Register new tests in the closest existing behavior target.
Create a new target only when ownership is clear.

| Existing target | Relevant coverage |
| --- | --- |
| `CommitGraphTest` | Commit canonical form, graph structure, Version refs, reachability, and persistence. |
| `EditorSessionHistoryPortTest` | Includes live document history fixtures. Covers rollback, Undo, Redo, typed projection, Version actions, transfer, recovery, and checkpoint capture. |
| `EditorMiniGitJournalRecoveryTest` | WAL validation, covered prefixes, missing suffixes, and corrupt tails. |
| `EditorMiniGitMaterializerTest` | DuckDB transaction, checkpoint, truncate windows, roots, and reopen. |
| `EditorSaveCheckpointServiceTest` | Async save lifecycle and stale completion. |
| `EditorSessionNavigationControllerTest` | Save-before-checkout and failure restoration. |
| `PipelineMapperTest` | Root, full document checkpoint, matching load, stale replay, and invalid storage. |
| `AdjustmentTransferServiceMiniGitTest` | Replace old operator Paste and merge tests with document Paste behavior. |
| `ProjectServiceTest` | Project metadata cutover and unsupported-format errors. |
| `GpuDagModelGraphTest` | Full document round-trip and domain graph commands. |
| `GpuDagMaskStoreTest` | Immutable asset load, validation, and exact file behavior. |

Run focused Windows checks from the repository root:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target CommitGraphTest EditorSessionHistoryPortTest EditorMiniGitJournalRecoveryTest EditorMiniGitMaterializerTest EditorSaveCheckpointServiceTest EditorSessionNavigationControllerTest PipelineMapperTest AdjustmentTransferServiceMiniGitTest ProjectServiceTest GpuDagModelGraphTest GpuDagMaskStoreTest
ctest --test-dir build/debug -R "CommitGraphTest|EditorSessionHistoryPortTest|EditorMiniGitJournalRecoveryTest|EditorMiniGitMaterializerTest|EditorSaveCheckpointServiceTest|EditorSessionNavigationControllerTest|PipelineMapperTest|AdjustmentTransferServiceMiniGitTest|ProjectServiceTest|GpuDagModelGraphTest|GpuDagMaskStoreTest" --output-on-failure
```

Run affected UI-port and project package suites after focused tests pass.
Run the complete core test set before NM4 completion.

NM4 has no native CUDA, OpenCL, or Metal algorithm change.
Run one existing render integration case for topology, parameter, Mask, checkout, and Paste invalidation.
NM8 retains full native and real-RAW product qualification.

Do not mark a skipped test as passed.
Record unavailable environments as gaps.

## 8. Required call chains

### 8.1 Settled parameter commit

```text
complete EditorParameterTarget + settled value
  -> captured normalized before and after model JSON
  -> one SetParameter typed change
  -> PipelineEditBatch validation
  -> live document already contains or applies after value under render lock
  -> mini-Git WAL append
  -> commit and active Version head publish
  -> typed history row and committed snapshot
  -> one Quality render
```

### 8.2 Graph command commit

```text
service Add / Remove / Reconnect request
  -> prepare exact node and edge data
  -> apply existing domain command under render lock
  -> graph and backbone validation
  -> one typed batch with complete inverse data
  -> WAL and commit publish
  -> graph/history projection revision
  -> one GraphTopologyChanged Quality render
```

### 8.3 Brush asset replacement

```text
settled complete R8 pixels from NM7
  -> MaskStore::Put
  -> immutable new MaskAssetKey
  -> ReplaceMaskAsset typed change with old and new Brush source
  -> verify both stored assets
  -> apply document reference change
  -> WAL and commit publish
  -> one SettledMaskEdit Quality render
```

### 8.4 Undo and Redo

```text
Undo / Redo request
  -> prepare typed head move
  -> validate expected document side and assets
  -> append WAL head-move record
  -> inverse / forward apply under render lock
  -> publish head and redo selection
  -> typed projections
  -> one Quality render when pixels changed
```

### 8.5 Checkpoint save and matching reopen

```text
save checkpoint request
  -> capture one CommitGraph materialization
  -> capture canonical full PipelineDocument from the same valid state
  -> write refs, commits, image state, and checkpoint in one DuckDB transaction
  -> verify durable root/head/chain/document labels
  -> truncate covered WAL

reopen
  -> project format check
  -> history and root load
  -> matching checkpoint validation
  -> install full document on the existing guard
  -> no first-parent replay
```

### 8.6 Recovery replay

```text
durable graph + root + checkpoint + non-empty WAL
  -> validate WAL source head and chain
  -> skip covered durable prefix
  -> replay missing typed commits and head moves
  -> validate final document and assets
  -> one agreeing head, chain, and document
  -> normal materialization and covered WAL truncation
```

### 8.7 Version checkout

```text
Version selected
  -> finish or cancel provisional input
  -> save checkpoint barrier
  -> resolve target first-parent typed chain
  -> reset same live document to immutable root
  -> apply target chain under render lock
  -> validate DAG and assets
  -> publish active Version and projections
  -> one VersionDocumentChanged Quality render
```

### 8.8 Paste

```text
portable transfer document
  -> validate source data and assets
  -> read target root and keep target Develop + geometry
  -> remap node, adjustment, and Mask IDs
  -> copy or reuse immutable assets
  -> create one typed Paste batch
  -> create new Version at target root
  -> apply batch, WAL, and commit
  -> activate Version and save checkpoint
  -> one PastedPipelineDocument Quality render
```

### 8.9 Failure restoration

```text
typed apply / WAL / replay / storage / asset / Paste failure
  -> stop before external publication when possible
  -> inverse-apply local completed changes
  -> revoke only the failed WAL tail when required
  -> restore prior head, refs, redo, document, and projections
  -> keep prior displayed frame
  -> no render intent
  -> report the actual error
```

## 9. NM4 completion criteria

- [ ] `PipelineEditBatch` is the only new-project edit commit payload.
- [ ] Every typed change stores complete identity and before/after data.
- [ ] Canonical payload, commit, and chain hashes have independent golden evidence.
- [ ] Parameter, node, graph, Mask, and raster-key changes support exact Undo and Redo.
- [ ] One user action creates one commit and one chain fold.
- [ ] The active Version head remains the only working history head.
- [ ] The live guard keeps one writable document and one shared executor.
- [ ] The immutable root stores the complete image-specific default document.
- [ ] Checkpoints store a full document with matching root, head, and chain labels.
- [ ] Matching checkpoints skip replay; stale checkpoints rebuild from history.
- [ ] Every Version rebuilds its own DAG from root and first-parent typed commits.
- [ ] Checkout failure restores the prior Version and document.
- [ ] WAL recovery applies each missing typed commit exactly once.
- [ ] Reopen preserves nodes, edges, parameters, Masks, refs, heads, and assets.
- [ ] The new project format rejects all older project and history shapes.
- [ ] No old stage or ordinary payload conversion path remains active.
- [ ] Paste creates one new Version and preserves target Develop, RAW data, and geometry.
- [ ] Paste remaps all node, adjustment, and Mask identities.
- [ ] Paste failure creates no partial Version, commit, head move, or render.
- [ ] No product path creates or presents a pipeline merge operation.
- [ ] Mask asset reachability covers roots, Versions, live state, redo, WAL, checkpoints, and active authoring input.
- [ ] Asset maintenance removes only exact unreachable files and reports failures.
- [ ] Typed history rows use saved identity and localization data.
- [ ] Failure and concurrency evidence preserves prior valid state with no substitute path.
- [ ] NM5 retains QuickQanava UI work.
- [ ] NM6 retains selected-node panel routing.
- [ ] NM7 retains viewer input and provisional raster work.
- [ ] NM8 retains full packaged native and real-RAW qualification.

Record implementation results under the corresponding sub-phase.
Include source revision, commands, test results, and main success and failure call chains.
Leave a criterion unchecked when evidence is unavailable.
