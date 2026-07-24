# Phase 6C Mini-Git History and Pipeline Snapshot Plan

Date: 2026-07-22

Status: approved design; 6C-1, 6C-2, 6C-2-Fix, 6C-3, 6C-4, and 6C-5 implemented.
Phase 6C-5 qualification Phase 1, Phase 2A, Phase 2B, and Phase 3A are implemented. Phase 3B is
the next build-configuration ownership stage (move app/UI source target declarations); the former
typed-value Phase 3 begins at Phase 4. Do not begin 6C-6 checkout, session switching,
or garbage collection until every qualification phase is complete.

Related documents:

- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Editor History Durability and Version Transfer Design](editor_history_durability_and_version_transfer_design.md)

This document is the authoritative Phase 6C plan. It replaces the array-and-cursor `Version` model,
the `WorkingVersion` timeline model, `RewriteTimeline`, image-scoped overlapping saves, and the old
Paste/Merge copy semantics described by the completed Phase 5 implementation. The checksummed journal
framing and DuckDB atomic-write lessons from Phase 5 remain useful implementation input, but the
target data model and product behavior are defined here.

## Outcome

Replace the current history implementation with a small Git-like commit graph and make the pipeline
snapshot, active Version, recovery journal, serialized pipeline state, adjustment panels, and
rendered frame agree on one checked-out head.

The editor continues to keep one live pipeline snapshot for the focused image. A slider drag changes
that live pipeline provisionally. Pointer release creates one immutable edit commit and appends it to
the recovery journal. Autosave, image switch, Version checkout, workspace switch, and orderly shutdown
materialize the journal into DuckDB before the requested transition continues.

Phase 6C is a destructive project-format cutover. It does not read or migrate old history, pipeline,
or journal layouts.

## Locked terminology

- **Edit commit**: an immutable commit object produced by one finalized edit command. It contains one
  or two parent hashes, a monotonic high-resolution creation timestamp, and the complete edit payload
  required for forward replay.
- **Version**: a named branch/ref with a stable Version ID and a mutable `head_commit_hash`.
- **HEAD**: the currently checked-out Version. Detached-HEAD editing is not supported.
- **Working head**: the in-memory head after journaled edits that have not yet been materialized into
  DuckDB.
- **Head-move record**: a recovery-journal record that moves the working head between existing
  commits for Undo or Redo. It is analogous to a reflog entry and is not an edit commit.
- **Root**: the immutable, image-specific base pipeline created after import metadata such as lens,
  dimensions, and color matrices has been resolved.
- **First-parent chain**: the replay path from root to a Version head. Ordinary edit commits have one
  parent. A merge commit has the current branch as first parent and the incoming branch as second
  parent. Pipeline reconstruction follows first parents and applies the merge commit's resolved field
  payload; it does not replay the second-parent branch into the pipeline.
- **Transaction-chain hash**: the incremental hash folded from root through the ordered first-parent
  commits applied to the pipeline. This is intentionally not called a Merkle root.
- **Pipeline snapshot**: the complete live runtime aggregate managed by `PipelineMgmtService`,
  including operator state, execution structure, kernel launchers, and current transient cache state.
- **Serialized pipeline state**: the operator state stored in DuckDB and used to rebuild
  a live pipeline snapshot. Runtime GPU handles, launchers, cache contents, and scheduler-selected
  cache policy are not persisted as history and are not hashed.
- **Save checkpoint**: the short-lived global editor save state that prevents image selection, Version
  checkout, and workspace switching until the current committed journal prefix has been materialized.
- **Materialize**: insert journaled commit objects and atomically advance the checked-out Version ref,
  serialized pipeline state, and recovery metadata in DuckDB.

Use `save checkpoint` consistently for this workflow: the current committed state is stored before
the requested navigation continues.

## Architecture decisions

### 1. Version is a branch, not a transaction container

`Version` no longer owns a transaction vector, cursor, last transaction, or copied materialized
params. Its stable identity is independent of its current head.

Minimum stored fields:

```text
VersionRef
  version_id
  element_id
  display_name
  head_commit_hash       // null means the image root
  created_at
  updated_at
```

The image edit state stores the root identity and the checked-out Version:

```text
ImageEditState
  element_id
  root_id
  active_version_id
  materialized_head_commit_hash
  materialized_transaction_chain_hash
  serialized_pipeline_state
  project_schema_version
```

Multiple Version refs may point to the same commit object. Shared ancestry is stored once.

### 2. Edit commits are immutable content-addressed objects

The project stores commit objects in one table keyed by `commit_hash`:

```text
EditCommit
  commit_hash
  root_id
  first_parent_hash
  second_parent_hash      // null for ordinary edits
  created_at_ns
  kind                    // Edit or Merge
  edit_payload
```

`created_at_ns` is part of the hashed object. The commit clock is normalized to be strictly
increasing within the process (`max(clock_now, previous_timestamp + 1)`). Two user actions with the
same parents and adjustment values therefore remain distinct commits. Common ancestry and an
already-existing exact commit hash are still shared by all refs that reach it.

Forking a Version never copies its ancestor rows: it creates one new ref, and only later unique
commits add rows. A commit reachable from several Version heads exists once in the commit table.
This is the primary database-growth control; unreachable unique commits are reclaimed on clean
project exit instead of duplicating or rewriting complete chains.

An ordinary edit payload contains the operator/stage/field identity, before and after values, and
before and after enabled state. A merge payload contains the complete UI-resolved field delta needed
to transform the first-parent pipeline into the merged result. Reconstruction never needs to infer a
conflict decision again.

The stable hash input is versioned and canonical:

```text
commit_hash = H(
  commit_format_version,
  root_id,
  ordered_parent_hashes,
  created_at_ns,
  commit_kind,
  canonical_edit_payload)
```

Parent order is significant. For a merge, parent 1 is the checked-out branch and parent 2 is the
incoming root-relative branch.

### 3. Pipeline and Version use the same incremental chain hash

The root receives a stable `root_id` when import metadata has produced the immutable base pipeline.
The hash does not serialize the live pipeline or hash its params JSON.

```text
root_chain_hash = H(chain_format_version, root_id)
next_chain_hash = H(previous_chain_hash, commit_hash)
```

On pointer release:

1. Build and hash the edit commit.
2. Append the complete commit plus expected previous and resulting chain hashes to the journal.
3. Advance the live pipeline snapshot's `working_head_commit_hash`.
4. Fold the live pipeline snapshot's `transaction_chain_hash` once.

Undo and Redo append head-move records containing the expected source head, target head, and target
chain hash. The live pipeline restores the chain hash associated with that existing target; a
reverse hash operation is never attempted.

During materialization, the save path starts from the stored materialized head/hash, validates each
new commit or head move in journal order, and obtains the final working head/hash. That result must
equal the captured pipeline head/hash. This simulates the same history operations that already
changed the live pipeline without hashing runtime pipeline state.

For a merge, the second parent participates in `commit_hash`; the incremental fold still advances
from the current first-parent chain hash exactly once.

### 4. The base pipeline is immutable

Each imported image owns one immutable root pipeline produced after required metadata has been
resolved. Changing code defaults later must not silently rewrite an existing image's root.

Checkout and repair always:

1. load the image root;
2. walk first parents from the selected Version head to the root;
3. reverse that list;
4. replay each commit forward into a clean pipeline;
5. build the runtime execution stages for the selected backend.

Merge second-parent ancestry is retained for history and garbage collection, but replay uses the
merge commit's resolved payload on top of its first parent.

### 5. Pipeline snapshots are runtime objects, not params bags

`PipelineMgmtService` owns the focused image's live pipeline snapshot. That snapshot includes the
executor graph, operator instances, launchers, and the transient cache state selected by
`pipeline_scheduler.cpp`. Cache policy, resize/crop scheduling state, GPU handles, and cache contents
remain runtime concerns and are excluded from the commit and chain hashes.

The serialized pipeline state is an acceleration cache. On editor open or Version checkout,
`PipelineMgmtService` validates:

- root ID;
- materialized head commit hash;
- materialized transaction-chain hash.

If all three match the checked-out Version, the service rebuilds the live runtime snapshot from the
serialized state. If they do not match, it reconstructs from root and first-parent commits, replaces
the stale serialized state on the next save, and emits a diagnostic. Missing or invalid reachable
commit objects are data corruption and must fail the open instead of silently selecting another
Version.

`thumbnail_service.cpp` does not perform this validation. Thumbnail work consumes the stored
serialized state selected by the save path; the editor open/checkout path owns history validation.

## Edit, save, and navigation flow

### Provisional edit and edit commit

- Slider movement updates the live pipeline and schedules preview rendering.
- No Version row or DuckDB commit object is modified while the input remains provisional.
- Pointer release or the defined idle boundary creates exactly one edit commit.
- The journal append and live chain-hash advance are one logical operation.
- If journal append fails, the UI reports the edit as unsaved and does not claim that the working
  head advanced.

An ordinary timed autosave is deferred while an edit remains provisional; it must not persist the
live pipeline while that pipeline contains a value with no edit commit. Image switch, Version
checkout, workspace switch, and orderly shutdown finalize the current value before starting the
save checkpoint.

### Global save checkpoint

Use one project-wide editor save mutex/coordinator. This deliberately trades parallel image saving
for a smaller and auditable state machine.

While a save checkpoint owns the global save lock:

- filmstrip selection, workspace switching, Version checkout, Paste, and Merge are disabled through
  `interaction_policy_controller.cpp` with a localized reason;
- a requested image/workspace transition remains pending and resumes only after save success;
- the GUI thread does not wait on DuckDB or file I/O;
- the service captures the committed serialized pipeline state, working head, and chain hash together
  before starting DuckDB I/O;
- current-image preview may continue after that capture, but a finalized edit commit is queued behind
  the global save lock and becomes the first record of the next journal prefix;
- no second image begins loading until the current image save has completed.

The save task must publish explicit interaction capabilities for editor image selection, workspace
selection, Version checkout, and adjustment transfer. Do not infer disabled state from a generic
busy flag.

### DuckDB materialization

Materialization does not replay commits into or otherwise manipulate a pipeline. At save-checkpoint
start, `PipelineMgmtService` exports one immutable serialized state from the committed live
snapshot together with its working head and transaction-chain hash. This is persistence input, not a
second live executor. The materializer recomputes commit hashes, validates head moves, and advances
the journal state only; the final journal head/hash must match the captured pipeline head/hash.

One DuckDB transaction performs all of the following:

1. Validate project schema, root ID, active Version ID, stored head, stored chain hash, and expected
   journal base.
2. Recompute and validate every journaled commit hash and every expected/target head move.
3. Insert immutable commit objects, ignoring an already-present identical hash.
4. Advance the journal head/hash through commits and head moves in record order.
5. Compare the computed head/hash with the captured committed pipeline head/hash.
6. Move the checked-out Version ref to the final commit hash.
7. Store the captured matching serialized pipeline state.
8. Advance recovery metadata and commit.

No save path may update history and pipeline through separate DuckDB transactions.

After the DuckDB transaction succeeds, truncate the materialized journal prefix and flush the file.
The global save lock prevents a later commit from being appended into the file being truncated. If
the process stops after DuckDB commit but before truncation completes, recovery uses the stored
materialized head and sequence to ignore already-materialized records, then truncates them on the
next successful save. Do not add atomic journal replacement or journal generations solely for this
truncation step.

After materialization and journal truncation, schedule thumbnail invalidation/regeneration for the
saved filmstrip element. Only then finish the save checkpoint, release the global save lock, and
resume a pending navigation request.

### Image switch and workspace switch

```text
Focused image A
  -> finalize A's open edit command if required
  -> start a save checkpoint and acquire the global save lock
  -> capture A's committed serialized pipeline state, head, and chain hash
  -> materialize A journal into DuckDB
  -> truncate A journal
  -> return A's live pipeline snapshot to PipelineMgmtService
  -> invalidate/regenerate A thumbnail
  -> finish the save checkpoint and release the global save lock
  -> acquire or rebuild image B pipeline snapshot
  -> validate B root/head/chain
  -> publish B panel snapshot and request B's first render
```

This intentionally removes the earlier Phase 5 goal of overlapping A save with B load/render.

## Undo, redo, checkout, and garbage collection

- Undo appends a head-move record, moves the working head to its first parent, restores that commit's
  chain hash, and applies the stored before state to the live pipeline. The abandoned child path
  remains available to the in-memory redo stack.
- Redo appends a head-move record and follows the in-memory child selected by that stack.
- Editing after undo clears the redo stack and creates a new child commit on the same Version. The
  Version ref moves to the new commit; no automatic Version is created.
- Detached-HEAD editing is not supported.
- Checkout is allowed only for a Version ref and always completes a save checkpoint first.
- Project-exit garbage collection marks every Version head and walks both parents. Commit objects not
  reached from any Version are deleted after the final save. This collects abandoned redo paths.
- A failed or abnormal shutdown does not run garbage collection. The next clean project exit may
  remove the previously unreachable objects.

## Paste and Merge

Library and Editor continue to call one `AdjustmentTransferService`; QML does not construct commits
or move Version refs.

### Paste

Paste is branch replacement, not cherry-pick:

1. Complete a save checkpoint and materialize the current Version.
2. Start an independent target-local branch at the target image root.
3. Convert the incoming adjustment package into a forward-replayable commit chain against that root.
4. Create a new Version ref pointing to that branch head.
5. Atomically set it as the active Version and store its matching serialized pipeline state.
6. Checkout the new Version.

The pasted branch never inherits commits from the previously active Version. If an existing Version
already points to the exact same target-local commit objects, refs may share them; Paste still creates
a distinct Version ref.

### Merge

Merge preserves both branches:

1. Complete a save checkpoint and materialize the current Version.
2. Represent the incoming adjustments as a target-local root branch without checking it out.
3. Present per-field conflicts to the UI.
4. Require the UI to provide one resolved value for every conflicting field.
5. Create one merge commit whose first parent is the current Version head and whose second parent is
   the incoming branch head.
6. Store the complete resolved field delta in that merge commit.
7. Advance the current Version ref to the merge commit and store the matching serialized state in one
   DuckDB transaction.

Canceling conflict resolution creates no merge commit and moves no ref. Reconstruction follows the
first parent and applies the resolved merge payload; it never re-runs the UI conflict algorithm.

## Adjustment panel state distribution

The live pipeline snapshot is authoritative for the focused editor session. QML receives a read-only
field-oriented copy through `EditorSessionController`.

- Every adjustment panel exposes `loadFromSnapshot(snapshot)`.
- The method uses no-submit setters only and is idempotent.
- It never writes the journal, moves HEAD, schedules rendering, changes focus, or starts a debounce
  timer.
- `EditorAdjustmentStack` owns the registered panel list and distributes snapshots after image open,
  Version checkout, undo, redo, recovery, Paste, Merge, and deferred panel creation.
- Controls remain disabled until root/head/chain validation and the first state publication finish.
- Snapshot shape is independent of panel layout; unknown fields remain available to later panels.

## Phase 6C work packages

The packages are ordered. Each package must leave its focused tests green before the next begins.

### Phase 6C-1 - Destructive schema boundary and Git vocabulary

Deliverables:

- Bump the project schema version and reject old project files with a clear incompatible-format
  error. Add no migration, fallback reader, or legacy history adapter.
- Introduce stable `VersionRef`, immutable root identity, commit-hash, and head types without yet
  routing production editing through them.
- Remove ambiguous new API names based on timeline arrays or cursors from the target interface.

Acceptance:

- A current-format empty project creates one root and one default Version ref.
- An old project fails before partially loading history or pipeline state.
- Version ID stays unchanged when its head moves.

### Phase 6C-2 - Immutable commit graph and incremental hashes

Deliverables:

- Add the commit-object table and parent indexes.
- Implement canonical Edit and Merge payloads, strictly increasing `created_at_ns`, commit hashing,
  first-parent traversal, and incremental transaction-chain hashing.
- Support one-parent edit commits and ordered two-parent merge commits.

Acceptance:

- Equal adjustment values committed at different timestamps produce different commit hashes.
- Two Version refs can share one head and ancestry without duplicating commit rows.
- Parent order changes a merge commit hash.
- First-parent traversal and chain folding are deterministic across reopen.

### Phase 6C-2-Fix - Identity, materialized-state, and graph-invariant corrections

This is a blocking correction package discovered during the 6C-1/2 review. It is ordered after
6C-2 and before 6C-3. No 6C-3 production pipeline work may build on the current graph API until
these corrections and their focused tests are complete.

Deliverables:

- Remove the overloaded meaning of `std::nullopt` in Version creation. A null
  `head_commit_hash` always means the image root. Provide separate, explicit operations for
  creating a Version at a supplied head and creating one at the active Version head, so a caller
  can create a root Version while the active Version is non-root.
- Separate working-head movement from materialized database state. Moving an in-memory working
  head must not silently claim that DuckDB has advanced, and the persistence API must not write a
  moved `VersionRef` together with stale `ImageEditState.materialized_head_commit_hash` or
  `materialized_transaction_chain_hash`.
- Replace or restrict the generic full-graph save entrypoint with a materialization input that
  captures the checked-out Version ID, final materialized head, recomputed transaction-chain hash,
  and serialized pipeline state as one immutable value. Validate their agreement before starting
  DuckDB writes, then store them in one transaction.
- Make graph load validate the complete reachable structure before returning a usable graph:
  every first and second parent must exist, belong to the same root, and satisfy commit-kind parent
  rules. An Edit commit has no second parent; a Merge commit has exactly one ordered second parent.
- Validate stored state during load: the active Version belongs to the requested element, its head
  equals the materialized head, and folding its first-parent chain equals the materialized
  transaction-chain hash. Reject mismatches as corruption instead of deferring failure until later
  traversal or pipeline reconstruction.
- Make the strictly increasing commit timestamp guarantee process-wide and thread-safe rather than
  local to an independently constructible `CommitClock`. Detect timestamp exhaustion instead of
  wrapping `UINT64_MAX` to zero.
- Define commit and chain hash byte encoding independently of host endianness and add stable hash
  vectors. Canonicalize Merge field deltas by field identity and reject duplicate field identities;
  ordered parent hashes remain significant.
- Validate deserialized commit kind, payload kind, payload shape, and parent cardinality before
  accepting a recomputed hash. An unknown enum value or extra/missing required structure is an
  incompatible or corrupt commit object, not an Edit fallback.
- Clarify the 6C-1 empty-state boundary in tests: the infrastructure test creates one empty image
  edit state, root identity, and default Version ref. Production creation of the immutable root
  after import metadata resolution remains part of 6C-3 and must not be claimed by a helper-only
  test.

Acceptance:

- With the active Version at a non-root commit, an explicit root-Version creation produces a new
  stable Version ID whose head is null; creating at the active head produces a ref to the active
  commit. Neither operation is ambiguous.
- Attempting to persist a Version head that disagrees with the materialized head or chain hash
  fails before the DuckDB transaction commits and leaves all prior rows unchanged.
- A valid materialization advances the commit rows, checked-out Version head, image materialized
  head, transaction-chain hash, and serialized pipeline state together; close and recreate `DBController`,
  then load and verify the same values.
- Loading fails immediately when any reachable first parent or merge second parent is missing,
  belongs to another root, or violates the Edit/Merge parent rules.
- Separate and concurrent timestamp callers still receive one strictly increasing process-wide
  sequence. Exhaustion produces an explicit failure.
- Fixed hash-vector tests produce the same commit, root-chain, and folded-chain hashes from the
  canonical bytes. Reordering equivalent Merge field inputs does not change the payload/hash, while
  swapping merge parents does.
- Repository call-site scans find no persistence path that can atomically store mutually
  inconsistent Version head, materialized head, chain hash, or serialized state values.

### Phase 6C-3 - Immutable root and pipeline snapshot validation

Deliverables:

- Persist the image-specific root produced after import metadata resolution.
- Extend the live pipeline snapshot with root ID, working head, and transaction-chain hash.
- Teach `PipelineMgmtService` to accept matching serialized state or rebuild from root and commit
  graph on mismatch.
- Keep scheduler cache policy, temporary resize/crop state, GPU handles, and cache contents outside
  history hashing.

Acceptance:

- Matching root/head/chain opens without full replay.
- Stale serialized state rebuilds to the checked-out Version and is replaced when the guard returns.
- A missing reachable commit fails with a diagnostic.
- Thumbnail generation does not duplicate editor history validation.

### Phase 6C-4 - Pointer-release commit and journal cutover

Deliverables:

- Replace `WorkingVersion`, cursor records, and `RewriteTimeline` production use with immutable edit
  commits, reflog-like head-move journal records, working-head state, and an in-memory redo stack.
- On pointer release, create one edit commit, append it with expected/result chain hashes, then advance
  the live working head and chain hash.
- Keep provisional slider updates pipeline-only.

Acceptance:

- A drag with any number of preview samples produces one edit commit.
- Failed journal append does not report an advanced working head.
- Undo, redo, and edit-after-undo select the required commits without rewriting old commit objects;
  recovery replays their head moves.

### Phase 6C-5 - Global save checkpoint, materialization, and log truncation

Deliverables:

- Add the project-wide editor save coordinator.
- Publish editor navigation locks through `InteractionPolicyController`.
- Capture the committed live serialized pipeline state/head/hash without constructing a second live
  executor, then materialize commits, Version head, that state, and recovery metadata in one
  DuckDB transaction.
- Truncate the materialized journal after DuckDB success and recover safely from the DB-commit/
  truncate window.

Acceptance:

- Filmstrip, workspace, Version checkout, Paste, and Merge remain disabled with a reason while saving.
- Image B does not begin loading before image A's save finishes.
- Materialization performs no pipeline replay or pipeline mutation; it only verifies the journal
  fold against the captured pipeline head/hash.
- Failure before DuckDB commit leaves the prior Version head and serialized pipeline state.
- Failure after DuckDB commit but before truncation does not replay a commit twice.
- Saving with no journal changes succeeds without moving the Version head.

### Phase 6C-5 qualification plan

This section is a work order, not a design essay. An implementer should be able to take one checkbox,
open the named files, make the stated change, and run the named tests without guessing what
"cleanup", "fixture", or "responsibility" means.

#### The user behavior being protected

Use image A as the image currently open in the editor and image B as the requested next image.

1. The user changes an adjustment on A. Preview updates may happen continuously, but releasing the
   control appends one Mini-Git commit to A's journal.
2. The user selects B, switches workspace, checks out another Version, pastes adjustments, or merges
   adjustments.
3. The editor starts one global save checkpoint. Those five actions are temporarily disabled and show
   the reason `Saving editor changes`.
4. The checkpoint captures A's working head, transaction-chain hash, serialized pipeline state, and
   the exact journal records being saved. It does not replay or modify the live pipeline.
5. A worker writes the commits, current Version head, chain hash, and serialized state in one DuckDB
   transaction. After that transaction succeeds, it truncates the saved journal records.
6. The editor invalidates A's thumbnail, finishes the background task, and only then starts loading B.
7. If any save step fails, A remains the active image, B is not loaded, and the journal remains usable
   for retry or reopen recovery.

The required success call chain is:

`QML action -> EditorSessionController::Open -> EditorSessionService facade ->`
`EditorSessionNavigationController -> EditorSaveCheckpointService ->`
`EditorSessionHistoryPort::CaptureSaveCheckpoint(A) ->`
`EditorSessionCheckpointStore -> EditorMiniGitMaterializer -> DuckDB transaction ->`
`journal truncate -> EditorSessionThumbnailPort::Invalidate(A) -> finish editor-save task ->`
`EditorSessionNavigationController -> EditorSessionLifecycle::ReleaseImage(A) -> AcquireImage(B)`.

The required failure call chain is:

`capture/write/truncate failure -> EditorSaveCheckpointService returns failure -> finish editor-save`
`task as failed -> EditorSessionNavigationController keeps A in EditorSessionLifecycle -> do not`
`invalidate A -> do not call B acquire/recovery/render -> retain the journal for retry`.

#### What the current tests prove, and what they do not prove

The review baseline executed 75 related discovered tests and all passed. That is useful evidence, but
only for their current assertions.

| Existing target | Current useful evidence | Missing evidence that this qualification must add |
| --- | --- | --- |
| `EditorMiniGitMaterializerTest` | Empty journal, one edit, one rejected capture, one recovery attempt, and basic lock acquisition. | Its lock test is sequential rather than concurrent; the recovery test recreates journal bytes after a successful save; no production port or reopen-wide field comparison. |
| `EditorSessionServiceTest` | Fake ports show that B waits for A's asynchronous materialization and that failure keeps A. | No real Mini-Git capture, DuckDB transaction, journal file, thumbnail service, or QML entrypoint. |
| `AlbumBackendInteractionPolicyTest` | A manually registered task disables five C++ capability getters and returns reasons. | It does not start the task through `EditorSessionTaskPort` and does not click the five QML actions. |
| `WorkspaceShellTest` | Existing workspace and editor shell behaviors remain green. | It has no checkpoint lock test and is already large, so new checkpoint cases belong in a new module test file. |

Passing the first column may not be used as a substitute for the missing evidence in the last column.

#### Scope: what may be decomposed now

Only code added or directly expanded for the 6C-5 save checkpoint belongs to this qualification. All paths in
this section are relative to `alcedo_studio/` unless explicitly stated otherwise.

| Current file | Current LOC | Work allowed in this qualification |
| --- | ---: | --- |
| `src/app/editor_mini_git_materializer.cpp` | 337 | Keep the completed coordinator split, then separate journal folding, DuckDB writing, and recovery into collaborating types. |
| `src/ui/alcedo_main/album_backend/editor_session_{history,journal_writer,thumbnail,pipeline,task,render_scheduler}_port.cpp` plus `editor_session_checkpoint_store.cpp` | 1387 total | Responsibility-named adapters replace the retired 1835-line mixed source; each implementation remains below 500 lines. |
| `src/app/editor_session_service.cpp` | 1287 | Replace the god class with lifecycle, navigation, save-checkpoint, edit, and render modules; leave a thin `IEditorSessionBackend` facade. |
| `src/ui/alcedo_main/album_backend/interaction_policy_controller.cpp` | 365 | Keep one file; clean only the five checkpoint-related capabilities. |
| `src/ui/alcedo_main/qml/EditorNavigationPolicy.qml` | 50 | Remove duplicated policy/fallback state; do not create another wrapper with the same data. |
| `src/ui/alcedo_main/qml/Main.qml` | 2206 | Extract the Phase 6 workspace and adjustment-transfer actions into real QML components; do not split unrelated window behavior. |
| `tests/app/editor_session_service_test.cpp` | 1229 | Divide all service tests by the new lifecycle/navigation/checkpoint/edit/render/facade modules. |
| `tests/ui/workspace_shell_test.cpp` | 2426 | Move only its Main-QML loading setup into a reusable fixture; do not reorganize its existing UI cases. |
| `tests/CMakeLists.txt` | 2063 | Register the new module test targets; do not reorganize unrelated targets. |

This qualification fully decomposes `EditorSessionService` because its responsibilities cannot be separated by
moving member definitions alone. For other 1000+ line files, decompose the Phase 6 types and behaviors
named above. Record unrelated old responsibilities as follow-up work instead of expanding this qualification into
a repository-wide rewrite.

#### Rules for every checklist stage

- Complete stages in order. Every stage is independently buildable and reviewable.
- Aim for about 500 changed lines per stage. This is a review target, not a hard cap. Keep a larger
  atomic change together when splitting would separate an implementation from its tests; record why.
- Record `git diff --numstat` and full LOC for every file touched by the stage.
- A module split must create a type with its own state and public API. Moving `GodClass::Method` into a
  second `.cpp`, adding a `friend`, or passing a pointer to the parent class does not count.
- Each mutable field has one owning module. Do not replace one god class with a shared mutable
  `Context`/`State` struct accessed by every module, and do not share the parent mutex across modules.
- Components communicate through typed requests, results, and completion callbacks. A facade may own
  the components and publish results, but it must not reimplement their business rules.
- Use module names, never development-phase names, for tests and CTest labels: `editor_history`,
  `editor_session`, `interaction_policy`, and `workspace_qml`. Remove the `phase6c` label.
- Judge behavior from executed tests. Source inspection is used only for naming, file ownership,
  documentation, and performance review.
- In each stage report, separate `Observed failure` (an executed check failed), `Coverage gap` (the
  named behavior has no test yet), and `Style/maintainability` (a source-structure finding).
- Add or update Doxygen comments in the same stage as a function changes. A useful comment explains
  purpose, inputs, result, side effects, owner/lifetime, thread or callback context, and failure result;
  it does not repeat the function name.

#### Phase 1 - Replace god classes with modules that own their own state

Phase 1 changes file ownership, names, comments, and test setup. It must not intentionally change save
behavior. Run the existing tests after every move so later failures can be attributed to later behavior
changes.

##### Phase 1A - Separate checkpoint locking from Mini-Git persistence (implementation present)

The current workspace already contains this extraction. Keep it as the baseline for the remaining
Phase 1 work. Before committing it, record the named test result; do not treat file presence as test
evidence.

Files:

- `src/include/app/editor_mini_git_materializer.hpp`
- `src/app/editor_mini_git_materializer.cpp`
- new `src/include/app/editor_save_checkpoint_coordinator.hpp`
- new `src/app/editor_save_checkpoint_coordinator.cpp`
- `src/CMakeLists.txt`

Checklist:

- [ ] Move `EditorSaveCheckpointCoordinator` and its move-only lock object into the new coordinator
      files. This module has one job: allow only one project save checkpoint at a time and release that
      ownership on every return, exception, move, and shutdown path.
- [ ] Leave `EditorMiniGitSaveCapture`, `EditorMiniGitMaterializeResult`, journal folding, DuckDB writes,
      recovery, and journal truncation in the materializer files. That module has one job: turn one
      already-captured Mini-Git journal prefix into durable history state.
- [ ] Keep `FoldMiniGitJournalFromMaterializedBase` private to the materializer unless another production
      caller exists. Do not create a third file for one helper.
- [ ] Rename `ScopedLock` to `SaveCheckpointLock`; `ScopedLock` does not say what it protects.
- [ ] Remove `MaterializeValidatedGraph` if call-site search still shows that it only calls
      `Materialize`. A second name for the same operation adds no information.
- [ ] Add Doxygen comments to the coordinator, lock, capture, result, `Materialize`, and
      `RecoverAndMaterialize`. State which thread may call them and whether the journal has been
      truncated when they return.
- [ ] Run `EditorMiniGitMaterializerTest` before and after the move. The same five tests must pass.

##### Phase 1B - Decompose `EditorSessionService` into collaborating types

The untracked `src/app/editor_session_checkpoint.cpp` is not the target design: it still implements
`EditorSessionService::...` and reaches every private field of the parent class. Replace that temporary
split with the modules below. Each numbered item is a separate review unit, normally near the 500-line
target.

###### Phase 1B-1 - `EditorSaveCheckpointService` owns save work

Files:

- new `src/include/app/editor_save_checkpoint_service.hpp`
- new `src/app/editor_save_checkpoint_service.cpp`
- `src/include/app/editor_session_ports.hpp`
- `src/include/app/editor_session_service.hpp`
- remove temporary `src/app/editor_session_checkpoint.cpp` after its logic has moved
- `src/CMakeLists.txt`

State owned only by `EditorSaveCheckpointService`:

- the callback shutdown gate;
- active save request ID and session generation;
- background task ID;
- the immutable capture passed to persistence;
- save completion/cancellation state.

Public API:

- `Start(SaveCheckpointRequest, SaveCheckpointCompletion)` starts capture and persistence;
- `CancelAndWait()` stops new callbacks and joins/cancels outstanding work;
- `active()` is diagnostics only and does not grant ownership.

Checklist:

- [ ] Move the behavior currently in `BeginSaveForSession`, `HandleJournalCommit`, and
      `HandleMaterialization` into this new class, not merely into a new file.
- [ ] The class depends on history capture, checkpoint persistence, task publication, thumbnail
      invalidation, and the project-owned save coordinator through narrow ports.
- [ ] It does not know image B, pending navigation, session guards, render generations, adjustment
      state, or `EditorSessionService`.
- [ ] The completion callback returns a typed `SaveCheckpointResult` containing request ID, source
      session generation, database/truncation outcome, and error.
- [ ] Add `EditorSaveCheckpointServiceTest` for start failure, asynchronous success, asynchronous
      failure, stale completion, duplicate completion, and `CancelAndWait`.

###### Phase 1B-2 - `EditorSessionLifecycle` owns the current image and guards

Files:

- new `src/include/app/editor_session_lifecycle.hpp`
- new `src/app/editor_session_lifecycle.cpp`
- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- `src/CMakeLists.txt`

State owned only by `EditorSessionLifecycle`:

- `EditorSessionState` and `EditorSessionIdentity`;
- pipeline and history guard handles;
- current error;
- acquisition/release state for the current image.

Checklist:

- [ ] Move `AcquireGuards`, `ReleaseGuards`, `ResetActiveImageState`, image-acquired handling, and the
      guard portion of open/close/shutdown into this type.
- [ ] Expose named operations such as `AcquireImage`, `MarkImageReady`, `ReleaseImage`, and
      `KeepCurrentImageAfterFailure`; do not expose mutable fields or the lifecycle mutex.
- [ ] This type does not save, publish background tasks, submit renders, apply edits, or remember B.
- [ ] Add `EditorSessionLifecycleTest` for acquire success/failure, release exactly once, same-image
      reopen, failed switch retaining A, and shutdown.

###### Phase 1B-3 - `EditorSessionNavigationController` owns pending A-to-B/close actions

Files:

- new `src/include/app/editor_session_navigation_controller.hpp`
- new `src/app/editor_session_navigation_controller.cpp`
- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- `src/CMakeLists.txt`

State owned only by `EditorSessionNavigationController`:

- `PendingEditorAction` with a named kind (`SwitchImage` or `CloseEditor`) and target data;
- the checkpoint request ID associated with that action.

Checklist:

- [ ] Move the orchestration currently spread across `HandleOpenOrSwitch`, `HandleClose`,
      `SealCurrentSession`, `ContinueOpenOrSwitch`, and `ResumePendingNavigationAfterSave` into this
      controller.
- [ ] It calls `EditorSaveCheckpointService` and `EditorSessionLifecycle` through their public APIs.
      It never accesses their fields or mutexes.
- [ ] It owns the rule "A remains acquired until its checkpoint succeeds; then release A and acquire
      B" and the rule "failure keeps A and discards the pending transition".
- [ ] It rejects a second pending action without replacing the original target.
- [ ] Add `EditorSessionNavigationControllerTest` for open, A-to-B success, save failure, second
      request, close, shutdown, and stale completion.

###### Phase 1B-4 - `EditorSessionRenderController` owns render and first-frame state

Files:

- new `src/include/app/editor_session_render_controller.hpp`
- new `src/app/editor_session_render_controller.cpp`
- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- `src/CMakeLists.txt`

State owned only by `EditorSessionRenderController`:

- presentation sink and dimensions;
- first-frame and quality-base request IDs;
- acquired/completed/submitted/presented flags;
- pending initial render reason;
- render-busy notification state and first-frame timing.

Checklist:

- [ ] Move `MakeRenderIntent`, initial/quality render routing, view-change routing, render diagnostics,
      first-frame matching, and `NotifyRenderResult` behavior into this type.
- [ ] Pass an immutable `EditorSessionIdentity` snapshot into render requests. Do not let this module
      mutate lifecycle or edit state.
- [ ] Return typed render events (`FirstFramePresented`, `RenderFailed`, and so on) to the facade or
      navigation controller.
- [ ] Add `EditorSessionRenderControllerTest` by moving the current first-frame, view-change,
      supersession, stale-render, and timing cases out of `editor_session_service_test.cpp`.

###### Phase 1B-5 - `EditorSessionEditController` owns adjustment/history operations

Files:

- new `src/include/app/editor_session_edit_controller.hpp`
- new `src/app/editor_session_edit_controller.cpp`
- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- `src/CMakeLists.txt`

State owned only by `EditorSessionEditController`:

- current `EditorRenderAdjustmentSnapshot`;
- provisional/settled adjustment data required between input and history commit.

Checklist:

- [ ] Move patch, settled commit, undo, redo, discard, finalized edit, head-move, and timeline-rewrite
      routing into this type.
- [ ] It receives the active history guard as a value/handle from lifecycle and requests render through
      a typed callback; it does not own session identity, guards, or render state.
- [ ] Add `EditorSessionEditControllerTest` by moving adjustment, undo/redo, discard, and history-record
      cases out of `editor_session_service_test.cpp`.

###### Phase 1B-6 - Reduce `EditorSessionService` to a facade

Files:

- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- `tests/app/editor_session_service_facade_test.cpp`
- `tests/CMakeLists.txt`

Checklist:

- [ ] `EditorSessionService` continues to implement `IEditorSessionBackend`, owns the five components,
      routes public calls, and publishes observer/change notifications.
- [ ] Remove component-owned fields and methods from the facade header. In particular, it must not own
      guards, pending saves/actions, callback gates, adjustment snapshots, or first-frame flags.
- [ ] Do not add `friend` declarations, a shared mutable `EditorSessionContext`, or callbacks that take
      `EditorSessionService*`.
- [ ] Keep facade tests limited to routing and externally visible result ordering. Component behavior
      belongs in the five module test targets above.
- [ ] Record final LOC and public/private method counts for every new type. Any type with more than one
      of lifecycle, save, navigation, edit, and render ownership fails this stage even if its file is
      under 1000 lines.

##### Phase 1C - Decompose Mini-Git persistence and production adapters into types

The same rule applies here: checkpoint materialization must not remain a file of member definitions
for several pre-existing large classes.

###### Phase 1C-1 - Split fold, DuckDB write, and recovery

Files:

- new `src/include/app/editor_mini_git_journal_fold.hpp`
- new `src/app/editor_mini_git_journal_fold.cpp`
- new `src/include/app/editor_mini_git_commit_writer.hpp`
- new `src/app/editor_mini_git_commit_writer.cpp`
- new `src/include/app/editor_mini_git_journal_recovery.hpp`
- new `src/app/editor_mini_git_journal_recovery.cpp`
- `src/include/app/editor_mini_git_materializer.hpp`
- `src/app/editor_mini_git_materializer.cpp`
- `src/CMakeLists.txt`

Responsibilities:

- `FoldMiniGitJournalFromMaterializedBase` is a pure algorithm: validate/replay records into a graph
  value and return the folded head/hash. It performs no database or file I/O.
- `EditorMiniGitCommitWriter` owns one DuckDB transaction that inserts commit objects and updates the
  Version head, chain hash, serialized pipeline state, and recovery metadata.
- `EditorMiniGitJournalRecovery` loads an existing journal, detects an already-written prefix, invokes
  the writer when needed, and truncates/flushes only the saved records.
- `EditorMiniGitMaterializer` may remain only as a thin facade composing those three operations. It
  must not own their mutable state or duplicate their logic.

Checklist:

- [ ] Give each type its own constructor dependencies and tests. Do not give them access to the
      materializer's private members through `friend`.
- [ ] Keep journal file handles in recovery, DuckDB connection/transaction state in the writer, and
      fold inputs/outputs as ordinary immutable values.
- [ ] Add `EditorMiniGitJournalFoldTest`, `EditorMiniGitCommitWriterTest`, and
      `EditorMiniGitJournalRecoveryTest`; keep `EditorMiniGitMaterializerTest` for facade integration.

###### Phase 1C-2 - Replace the mixed editor-session composition with narrow adapters

Files:

- new `src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- new `src/include/ui/alcedo_main/album_backend/editor_session_journal_writer_port.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_journal_writer_port.cpp`
- new `src/include/ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_checkpoint_store.cpp`
- new `src/include/ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_thumbnail_port.cpp`
- new `src/include/ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_pipeline_port.cpp`
- new `src/include/ui/alcedo_main/album_backend/editor_session_task_port.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_task_port.cpp`
- new `src/include/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp`
- new `src/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp`
- `src/include/app/editor_session_ports.hpp`
- `src/CMakeLists.txt`
- `tests/CMakeLists.txt`

Responsibilities:

The permanent modules are named after their responsibilities. The original 1,835-line file had
pipeline loading, task publication, journal writing, Mini-Git history, checkpoint materialization,
thumbnail invalidation, and render scheduling in one compilation unit. Each extracted type below
owns its own state and has a narrow dependency surface; no class is retained under a phase or
environment label.

- `EditorSessionHistoryPort` owns `MiniGitWorkingHistory`, history guards, adjustment
  commits, undo/redo, and immutable checkpoint capture. Capture returns the value directly; it stores
  no side-map for another class to take later.
- `EditorSessionJournalWriterPort` owns journal lookup, per-image journal synchronization,
  finalized edit/head-move append, and discard of uncommitted records. It performs no DuckDB
  materialization, recovery, thumbnail invalidation, or task publication.
- `EditorSessionCheckpointStore` implements a new `IEditorCheckpointStore`. It accepts an
  immutable capture and calls the materializer/recovery facade. It owns no live history or QML task.
- `EditorSessionThumbnailPort` implements a narrow thumbnail invalidation port. It contains
  the only call from this save path to `ThumbnailService::InvalidateThumbnail`.
- `EditorSessionPipelinePort` owns loaded pipeline guards and pipeline-service save/release calls.
- `EditorSessionTaskPort` owns task IDs and BackgroundTaskController publication only.
- `EditorSessionRenderSchedulerPort` owns render jobs, presentation acknowledgements, and worker
  lifetime only.

Checklist:

- [x] Split the current broad `IEditorJournalPort` into the writer API required by edit finalization
      and `IEditorCheckpointStore` required by save/recovery. Do not leave default successful
      materialization methods on the writer interface.
- [x] Move writer lookup, image locks, finalize/append/head-move/discard methods to
      `EditorSessionJournalWriterPort`.
- [x] Delete the old combined journal port after its call sites move. The old
      transaction-array materialization/compaction route is not part of the new project format; remove
      it when call-site search confirms no target-architecture caller. If a historical test still needs
      it, keep a test-only legacy adapter rather than wiring it into the production session.
- [x] Move still-relevant cases from the old combined journal-port test to the history, journal
      writer, or checkpoint-store target that owns the behavior. Remove obsolete legacy cases with an
      explicit old-schema reason and record the before/after discovered test counts.
- [x] Remove `TakeSaveCapture` and `save_captures_`; capture ownership travels in function arguments.
- [x] Move pipeline, task, and scheduler behavior into responsibility-named modules without changing
      their ownership or execution path; record their focused tests in the module targets.
- [x] Add `EditorSessionHistoryPortTest`, `EditorSessionJournalWriterPortTest`,
      `EditorSessionCheckpointStoreTest`, and `EditorSessionThumbnailPortTest` with real behavior per
      adapter.
- [x] Add direct focused coverage for `EditorSessionPipelinePort`, `EditorSessionTaskPort`, and
      `EditorSessionRenderSchedulerPort`; their tests verify guard caching/release, all five save
      locks/task completion, and frame submission/identity-checked presentation acknowledgement.

Implementation record (2026-07-23): the method counts below exclude constructors, destructors, and
inherited virtual declarations; they count the public operations and private helpers declared by each
new type. The old combined journal/thumbnail tests discovered 16 cases (12 + 4); the seven
responsibility targets now discover 18 cases (3 + 4 + 2 + 3 + 2 + 2 + 2), with the additional two
cases covering the newly isolated pipeline/task/scheduler state.

| Type | Implementation LOC | Public operations | Private helpers | Focused target |
| --- | ---: | ---: | ---: | --- |
| `EditorSessionHistoryPort` | 408 | 10 | 1 | `EditorSessionHistoryPortTest` (4) |
| `EditorSessionJournalWriterPort` | 196 | 8 | 3 | `EditorSessionJournalWriterPortTest` (3) |
| `EditorSessionCheckpointStore` | 111 | 4 | 2 | `EditorSessionCheckpointStoreTest` (2) |
| `EditorSessionThumbnailPort` | 25 | 1 | 0 | `EditorSessionThumbnailPortTest` (3) |
| `EditorSessionPipelinePort` | 93 | 5 | 0 | `EditorSessionPipelinePortTest` (2) |
| `EditorSessionTaskPort` | 93 | 3 | 0 | `EditorSessionTaskPortTest` (2) |
| `EditorSessionRenderSchedulerPort` | 461 | 11 | 4 | `EditorSessionRenderSchedulerPortTest` (2) |
| **Total** | **1387** | **42** | **10** | **18 discovered cases** |

##### Phase 1D - Extract the Phase 6 QML actions into components

Files:

- new `src/ui/alcedo_main/qml/EditorWorkspaceNavigation.qml`
- new `src/ui/alcedo_main/qml/EditorAdjustmentTransferActions.qml`
- deleted `src/ui/alcedo_main/qml/EditorNavigationPolicy.qml`
- `src/ui/alcedo_main/qml/EditorWorkspace.qml`
- `src/ui/alcedo_main/qml/EditorFilmstrip.qml`
- `src/ui/alcedo_main/qml/EditorHistoryVersionsRail.qml`
- `src/ui/alcedo_main/qml/Main.qml`
- `src/CMakeLists.txt`

Responsibilities:

- `InteractionPolicyController` remains one cohesive C++ type: it translates active task locks into
  capability booleans and reasons. Its 365 lines do not justify splitting one responsibility into five
  tiny controllers.
- `EditorWorkspaceNavigation.qml` owns the Library/Editor buttons, current-workspace presentation, and
  the `SwitchWorkspace` permission check. It does not know Paste, Merge, history, or filmstrip state.
- `EditorAdjustmentTransferActions.qml` owns Paste/Merge permission checks and opening the existing
  adjustment-transfer dialog. It does not own workspace navigation or persistence.
- `EditorFilmstrip.qml` owns image-selection presentation and emits an activation request only when
  `canSelectEditorImage` is true.
- `EditorHistoryVersionsRail.qml` keeps history browsing separate from the future Version checkout
  capability.

Checklist:

- [x] Delete `EditorNavigationPolicy.qml` after its consumers bind to the authoritative
      `InteractionPolicyController`; a QML mirror of the same five values is not a module.
- [x] Move only the Phase 6 workspace button block from `Main.qml` into
      `EditorWorkspaceNavigation.qml`. Leave unrelated global window/navigation behavior in `Main.qml`.
- [x] Move only `requestPasteAdjustments` and the Merge permission branch into
      `EditorAdjustmentTransferActions.qml`; pass the existing dialog/backend as explicit properties.
- [x] Do not create one new QML controller containing workspace, filmstrip, Version, Paste, and Merge.
      That would reproduce the god component under a new name.
- [x] Add component tests `EditorWorkspaceNavigationQmlTest` and
      `EditorAdjustmentTransferActionsQmlTest`; keep the later end-to-end test for their composition in
      `Main.qml`.

Verification record (2026-07-23): `EditorWorkspaceNavigationQmlTest` discovers and passes 3 cases;
`EditorAdjustmentTransferActionsQmlTest` discovers and passes 4 cases. The focused CTest run passes
7/7 cases, `MainQmlWorkflowTest` passes its production window case, and the `alcedo_main` target
builds successfully. The final full `WorkspaceShellTest` run discovers 44 cases: 41 pass, 2 fail,
and 1 is skipped by the offscreen platform. The remaining failures are
`EditorSessionControllerTracksSessionWithoutLegacyModal` (backend close state) and
`ProductionFrameSinkAcceptsThreeLayerFrameSubmissions` (production frame image identity); the run
emits no QML warnings.

##### Phase 1E - Create module-specific fixtures and test targets

This stage creates test infrastructure only. It does not add failure policies or change production
behavior.

Files to add:

- `tests/support/editor_mini_git_project_fixture.hpp/.cpp`
- `tests/support/editor_save_checkpoint_fixture.hpp/.cpp`
- `tests/support/editor_session_navigation_fixture.hpp/.cpp`
- `tests/support/editor_session_test_ports.hpp`
- `tests/ui/main_qml_test_fixture.hpp/.cpp`
- `tests/app/editor_save_checkpoint_service_test.cpp`
- `tests/app/editor_session_lifecycle_test.cpp`
- `tests/app/editor_session_navigation_controller_test.cpp`
- `tests/app/editor_session_edit_controller_test.cpp`
- `tests/app/editor_session_render_controller_test.cpp`
- `tests/app/editor_session_service_facade_test.cpp`
- `tests/CMakeLists.txt`

`EditorMiniGitProjectFixture` must provide:

- [x] one unique temporary directory containing a project database, project metadata file, and
      per-image Mini-Git journals;
- [x] real `ProjectService`, `StorageService`, `CommitGraphService`, and persisted default Versions for
      image A and image B, with different element IDs and root IDs;
- [x] deterministic commit timestamps through `CommitClockAccess`, not wall-clock sleeps;
- [x] helpers named `AppendExposureEdit`, `CaptureWorkingState`, `CloseAndReopenProject`,
      `LoadStoredGraph`, `ReadJournalRecords`, and `CountStoredCommits`;
- [x] teardown that destroys materializer/storage/project objects before removing files.

`EditorSaveCheckpointFixture` must provide only:

- [x] history capture, checkpoint store, task, thumbnail, and coordinator test doubles;
- [x] helpers named `StartCheckpoint`, `CompleteDatabaseWrite`, `CompleteJournalTruncate`, and
      `CancelAndWait`;
- [x] switches for capture, task-start, journal-commit, and materialization failure.

`EditorSessionNavigationFixture` must provide only:

- [x] a real `EditorSessionNavigationController` with lifecycle and save-checkpoint collaborators;
- [x] fixed identities for A and B and helpers named `OpenA`, `RequestSwitchToB`,
      `CompleteCheckpoint`, and `FailCheckpoint`;
- [x] an ordered event vector containing `checkpoint_a`, `release_a`, and `acquire_b`;
- [x] no render port, adjustment snapshot, DuckDB project, or QML engine.

`editor_session_test_ports.hpp` contains small reusable fake port types, not a fixture and not shared
mutable scenario state. Each component fixture constructs only the fakes its module requires.

`MainQmlTestFixture` must provide:

- [x] the `LoadedMainWindow`, `MainQmlUrl`, and `LoadMainWindow` setup currently repeated or private in
      `workspace_shell_test.cpp`;
- [x] a real `ApplicationModuleHost`, `BackgroundTaskController`, and
      `InteractionPolicyController`, plus helpers to find the five guarded QML entrypoints;
- [x] no production behavior changes and no copied second implementation of application startup.

Test-file split and registration:

- [x] Move every test from `editor_session_service_test.cpp` to the module that owns the asserted
      behavior: lifecycle/open/guard tests, navigation/save-order tests, edit/history tests,
      render/first-frame/view-change tests, or facade routing tests.
- [x] Delete the old monolithic test file once its test-count inventory reaches zero. Do not keep it as
      an unsorted destination for future cases.
- [x] Register the six module targets named above. Target names describe software modules, not roadmap
      phases.
- [x] Use CTest labels `editor_history`, `editor_session`, `interaction_policy`, and `workspace_qml`.
      Remove `phase6c`. Labels are only a convenient module filter; stage acceptance also runs the
      named executables directly, so an empty label cannot be mistaken for passing tests.
- [x] Record a source-test-to-destination-test table and compare discovered test counts before/after.
      No test may disappear or be weakened during the split.
- [x] Reject a fixture that constructs lifecycle, checkpoint, edit, render, QML, and DuckDB together.
      That is a test-side god object; only the later integration fixture may compose the full path.

Implementation record (2026-07-23): the monolithic `editor_session_service_test.cpp` is absent (already
split in 1B). Phase 1E adds focused fixtures under `tests/support/` and `MainQmlTestFixture`, rewires
module tests to those fixtures, and registers `EditorSessionTestSupport`. Source-to-destination inventory
for the former session-service cases:

| Destination target | Discovered cases (1E) | Ownership |
| --- | ---: | --- |
| `EditorSessionLifecycleTest` | 17 | open/guard/state transitions |
| `EditorSessionNavigationControllerTest` | 13 | A-to-B save order, close, stale tickets |
| `EditorSessionEditControllerTest` | 10 | patch/undo/discard/journal record routing |
| `EditorSessionRenderControllerTest` | 11 | first-frame, quality base, view change |
| `EditorSessionServiceFacadeTest` | 4 | facade Open/Shutdown/result ordering |
| `EditorSaveCheckpointServiceTest` | 8 | save task/journal/materialize completion |
| **Total module cases** | **63** | no monolithic residual file |

Additional fixture consumers: `EditorMiniGitMaterializerTest` now uses `EditorMiniGitProjectFixture`
(6 cases including dual-root isolation). Labels applied: `editor_session`, `editor_history`,
`interaction_policy`, `workspace_qml`. No `phase6c` label remains on these targets.

Phase 1 is complete only when all five sub-stages build, the existing behavior tests remain green, all
changed C++ files pass `clang-format --dry-run --Werror --style=file`, every changed function has the
required Doxygen description, and no new module reaches into another module's private state.

#### Phase 2 - Make the global save lock and captured journal range unambiguous

This stage fixes only ownership and concurrency. It does not yet change DuckDB failure behavior or QML.

##### Phase 2A - One project-owned global save lock

Files:

- `src/include/app/editor_save_checkpoint_coordinator.hpp`
- `src/app/editor_save_checkpoint_coordinator.cpp`
- `src/include/app/editor_save_checkpoint_service.hpp`
- `src/app/editor_save_checkpoint_service.cpp`
- `src/ui/alcedo_main/album_backend/application_module_host.cpp`
- `tests/app/editor_save_checkpoint_coordinator_test.cpp`
- `tests/app/editor_save_checkpoint_service_test.cpp`
- `tests/CMakeLists.txt`

Checklist:

- [x] Remove the function-static coordinator currently returned by `SharedCoordinator()`.
- [x] Construct one `EditorSaveCheckpointCoordinator` with the open editor/project services in
      `ApplicationModuleHost`, then inject that shared instance into `EditorSaveCheckpointService`.
- [x] Acquire a `SaveCheckpointLock` before capturing A. Keep it until DuckDB write, journal
      truncation, thumbnail invalidation scheduling, and the terminal callback have all finished.
- [x] Wait with `std::condition_variable` or an equivalent blocking primitive. Delete the
      `std::this_thread::yield()` loop. Shutdown must wake and join/cancel waiting work.
- [x] Do not expose a second boolean such as `isSaving` as ownership. The move-only lock object is the
      owner; `active_element_id()` is diagnostics only.

Add target `EditorSaveCheckpointCoordinatorTest` with these tests:

- [x] `TwoThreadsCannotOwnTheGlobalSaveLockAtTheSameTime`: use two real threads, a barrier, and an
      atomic active-owner count; assert the maximum is exactly one.
- [x] `SaveCheckpointLockReleasesAfterSuccessFailureExceptionAndMove`: exercise normal destruction,
      explicit failure return, thrown exception, move construction, and move assignment.
- [x] `WaitingSaveStopsCleanlyWhenProjectShutsDown`: no sleep; use a condition/barrier to prove the
      waiter exits and no thread remains joinable.

##### Phase 2B - Capture one exact journal range

Files:

- `src/include/app/editor_mini_git_materializer.hpp`
- `src/include/app/editor_session_ports.hpp`
- `src/edit/history/mini_git_working_history.cpp`
- `src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- `tests/edit/history/editor_save_checkpoint_capture_test.cpp`
- `tests/CMakeLists.txt`

Checklist:

- [x] Give `EditorMiniGitSaveCapture` an explicit first and last journal sequence number, plus element
      ID, Version ID, root ID, working head, transaction-chain hash, serialized pipeline state, and
      journal path.
- [x] Remove `no_journal_changes` / `journal_already_materialized`; `journal_records.empty()` already
      answers that question.
- [x] Capture the records and sequence numbers while holding the same journal mutex used by append and
      truncate. Copy the immutable capture into the worker request, then release live history access.
- [x] If a finalized edit arrives after capture, it must not appear in the captured range and must not
      be removed by truncating that range. Because the global save lock also blocks navigation, the
      edit either waits or becomes the first record after the checkpoint completes.
- [x] On any failure, keep the captured journal bytes and sequence range available for retry.

Add target `EditorSaveCheckpointCaptureTest` with these tests:

- [x] `EmptyJournalCaptureHasNoSequenceRangeAndDoesNotNeedAnotherFlag`.
- [x] `CaptureContainsElementVersionRootHeadHashStateAndExactRecords`.
- [x] `EditAppendedAfterCaptureIsNotDeletedWithCapturedRecords`.
- [x] `FailedCheckpointKeepsTheCapturedRecordsForRetry`.
- [x] `MismatchedElementVersionRootOrSequenceRangeStartsNoMaterialization` as a parameterized test.

Phase 2 is complete when both new targets pass repeatedly, Thread Sanitizer is run where available, no
busy-wait remains, and the updated call-chain comment names the owner of the save lock and capture at
each callback boundary.

#### Phase 3 - Give CMake files domain ownership

This is a build-graph refactor only. It changes neither Mini-Git behavior nor C++/QML ownership,
target names, public headers, test names, CTest labels, compiler flags, generated artifacts, or
link directions. Its purpose is to stop Phase 6C work from extending two mixed root manifests.
The former typed-value work is Phase 4 after this stage.

##### Baseline and ownership boundary

First-party manifests live under `alcedo_studio/` (top-level CMake adds
`alcedo_studio/src` and `alcedo_studio/tests`). Phase 3A freezes that tree before relocation.
LOC after Phase 3A helper extraction (still above the review threshold):

| File | LOC after 3A | Mixed responsibilities that must still be separated |
| --- | ---: | --- |
| `alcedo_studio/src/CMakeLists.txt` | 2266 | platform/backend switches, source lists, generated protobuf/shader work, app services, UI libraries, QML module, executable packaging (helper macro extracted) |
| `alcedo_studio/tests/CMakeLists.txt` | 2156 | test switches, category aggregate targets, every domain's executable declarations, discovery, resource copy rules, and trailing category membership lists (registration functions extracted) |

`alcedo_studio/src/third_party/` already has its own build manifests, but no first-party
`alcedo_studio/src/app`, `alcedo_studio/src/ui`, `alcedo_studio/tests/app`, or
`alcedo_studio/tests/ui` manifest currently owns its local targets. Moving lines into a second
large root-adjacent file would not fix the ownership problem. Each new manifest owns the target
declarations for one source domain; the root files only assemble the graph.

| Owner | New or retained file | Owns | Must not own |
| --- | --- | --- | --- |
| Source build root | `alcedo_studio/src/CMakeLists.txt` | immutable source-root paths, common helper inclusion, platform-wide feature values, and ordered `add_subdirectory` calls | first-party library/executable source lists or their target-link rules |
| Source target helper | `alcedo_studio/src/cmake/AlcedoTargetHelpers.cmake` | `def_library` implementation and common include-directory behavior | a concrete app/UI target or a platform feature decision |
| Application services | `alcedo_studio/src/app/CMakeLists.txt` | every target whose primary implementation is under `src/app/` or public API under `src/include/app/`, including the editor-session and Mini-Git service targets | UI target declarations or a dependency from app to a UI target |
| UI composition root | `alcedo_studio/src/ui/CMakeLists.txt` | ordering of UI subdomains only | long source/QML lists |
| Editor RHI | `alcedo_studio/src/ui/editor_rhi/CMakeLists.txt` | RHI viewport, harness, shaders, backend-specific RHI setup, and developer harness targets | album-shell or QML application target details |
| Alcedo UI shell | `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt` | `UiLocalization`, `BackgroundTaskController`, `AlbumBackendLib`, editor-dialog source selection, `alcedo_main`, QML files, translations, and platform packaging rules | app-service source lists |
| Test build root | `alcedo_studio/tests/CMakeLists.txt` | test-root paths, test feature options, aggregate category targets, common helper inclusion, and ordered `add_subdirectory` calls | individual test executable declarations, `gtest_discover_tests`, or per-target copy commands |
| Test registration helper | `alcedo_studio/tests/cmake/AlcedoTestRegistration.cmake` | category registration, DuckDB extension copy helpers, and shared test-target setup | lists of application/UI test source files |
| Application tests | `alcedo_studio/tests/app/CMakeLists.txt` | all targets with test sources under `tests/app/`, their discovery, labels, fixtures, resource copies, and category registration | UI/QML test executable definitions |
| UI tests | `alcedo_studio/tests/ui/CMakeLists.txt` | all targets with test sources under `tests/ui/`, the album-backend helper, QML resources, discovery, labels, and category registration | app-service test executable definitions |
| Edit/history tests | `alcedo_studio/tests/edit/CMakeLists.txt` | Mini-Git persistence, capture, and edit-pipeline test declarations under `tests/edit/` | app/UI target declarations |

The remaining first-party source and test domains follow the same boundary in Phase 3D:
`concurrency`, `cuda`, `decoders`, `edit`, `image`, `io`, `metal`, `nn`, `opencl`, `renderer`,
`sidecar_client`, `sleeve`, `storage`, and `utils` under `src`; and the existing direct test domains
under `tests` (`ci`, `cuda`, `decoders`, `gui_pocs`, `image`, `io`, `metal`, `ml_ops`, `opencl`,
`perf`, `raw`, `resources`, `sleeve`, `storage`, `support`, and `utils`). A `CMakeLists.txt` is added
only where that domain actually declares a target; source files and fixtures stay where they are.

Build ownership and observable result must be traceable through this chain:

`top-level CMake -> src root -> owning source manifest -> named library/executable -> tests root -> owning test manifest -> gtest discovery/manual CTest entry -> CTest category aggregate`.

##### Build-context rules

- [x] Before the first source `add_subdirectory`, define `ALCEDO_SRC_ROOT`,
      `ALCEDO_INCLUDE_ROOT`, and `ALCEDO_BINARY_ROOT`. Before the first test `add_subdirectory`,
      define `ALCEDO_TEST_ROOT` and `ALCEDO_TEST_SUPPORT_ROOT`. Child manifests may read these
      values but may not overwrite them. *(Phase 3A: defined at the top of the source and test
      roots; no first-party `add_subdirectory` yet.)*
- [x] Move the current `def_library` macro into `alcedo_studio/src/cmake/AlcedoTargetHelpers.cmake`.
      Its public include path must use `${ALCEDO_INCLUDE_ROOT}`, never a relative `include` path that
      changes meaning in a child directory.
- [x] Move `alcedo_assign_test_category`, DuckDB copy helpers, and reusable test setup into
      `alcedo_studio/tests/cmake/AlcedoTestRegistration.cmake`. *(Phase 3A: those functions left the
      root. `def_ui_test` remains in the test root until Phase 3C moves it with UI tests.)*
- [ ] Replace the trailing hand-maintained category target lists with one
      `alcedo_register_test_target(<target> <category>...)` call adjacent to each test declaration.
      The helper applies the existing build option, `EXCLUDE_FROM_ALL` behavior, and dependencies on
      `alcedo_tests_all` and the named category aggregate. A target in more than one category is
      registered once per category at that same declaration. *(Phase 3A: helper exists; trailing
      lists still call `alcedo_assign_test_category`. Phase 3C converts declarations as they move.)*
- [x] Keep all existing `gtest_discover_tests`, explicit `add_test`, labels, working directories,
      timeouts, resource copies, platform guards, and target names unchanged. Category registration
      changes build aggregation only; it must not rewrite CTest names or labels. *(Verified by
      Phase 3A pre/post CTest identity compare.)*
- [x] Do not pass target lists through `PARENT_SCOPE`, a directory property, or a mutable global list.
      CMake targets are global by design; their source list, link rules, discovery, and category
      membership remain adjacent in the owning manifest.
- [ ] Use `${ALCEDO_SRC_ROOT}`, `${ALCEDO_TEST_ROOT}`, or a path local to the owning manifest for
      every moved path. Audit every use of `CMAKE_CURRENT_SOURCE_DIR`, `CMAKE_CURRENT_BINARY_DIR`,
      and `CMAKE_CURRENT_LIST_DIR`; retain a local value only when that directory is intentionally
      the source of the path. *(Phase 3B–3D when targets relocate.)*

##### Phase 3A - Freeze the build graph before relocation

Files:

- `alcedo_studio/src/CMakeLists.txt`
- `alcedo_studio/tests/CMakeLists.txt`
- new `alcedo_studio/src/cmake/AlcedoTargetHelpers.cmake`
- new `alcedo_studio/tests/cmake/AlcedoTestRegistration.cmake`

Checklist:

- [x] Configure the existing `win_debug` build before changing a manifest. Record the exact CMake
      version, generator, configured feature values, and the current values of every
      `ALCEDO_BUILD_*` option.
- [x] Capture the pre-change target manifest with
      `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target help` and the pre-change CTest
      manifest with `ctest --test-dir build/debug --show-only=json-v1`. Store both comparison outputs
      outside source control with the review evidence; do not add machine-specific build output to
      the repository.
- [x] Build a declaration inventory before moving code. For every existing first-party target, record
      target name, owning source/test domain, source paths, public/private dependencies, platform
      guard, generated inputs, post-build work, CTest discovery style, labels, and category
      membership. A target may have exactly one owning CMake manifest.
- [x] Derive the `add_subdirectory` order from the current declaration order and `if(TARGET ...)`
      checks. Do not choose alphabetical order. Every target referenced by an existing conditional
      must be defined at the same point relative to that conditional after the split.
- [x] Establish the shared helper files and root path values without moving a target. Configure once
      and compare the target and CTest manifests to the baseline before continuing.

Required evidence:

| Acceptance criterion | Required proof | Reject the stage when |
| --- | --- | --- |
| Existing developer target names remain usable | Pre/post target manifests have the same first-party target set | any production, test, aggregate, or harness target disappears or is renamed |
| CTest behavior remains discoverable | Pre/post JSON manifests have the same test names, labels, working directories, and timeouts | discovery changes, a manual CTest entry disappears, or a label changes |
| Global setup has one owner | Root manifests contain only setup and composition after the final substage | a root manifest still declares a first-party library/test executable or a shared helper function |

###### Phase 3A frozen results (2026-07-24)

Machine-local review evidence (not committed): `build/tmp/phase3a_baseline/` (pre-change) and
`build/tmp/phase3a_post/` (after helper extraction + reconfigure). Full target help, CTest JSON,
option dumps, declaration CSVs, and diffs live there.

**Configure baseline (`win_debug`)**

| Field | Frozen value |
| --- | --- |
| CMake | 3.26.3 |
| Generator | Ninja |
| `CMAKE_BUILD_TYPE` | Debug |
| Compiler | MSVC 14.44 (`cl.exe` Hostx64/x64) |
| Qt prefix (configure) | `D:/Qt/6.9.3/msvc2022_64/lib/cmake` (cache may also resolve `D:/misc/Qt/6.9.3/...`) |
| `ALCEDO_ENABLE_CUDA` | ON |
| `ALCEDO_ENABLE_OPENCL` | ON |
| `ALCEDO_ENABLE_METAL` | OFF |
| `ALCEDO_ENABLE_WEBGPU` | OFF |
| `ALCEDO_REAL_WIDGET_EDITOR` | ON on this Windows Qt build (set in top-level CMake, not a cache option) |
| `ALCEDO_BUILD_TESTS` | ON |
| `ALCEDO_BUILD_TESTS_BY_DEFAULT` | ON |
| `ALCEDO_BUILD_CORE_TESTS` | ON |
| `ALCEDO_BUILD_IMAGE_TESTS` | ON |
| `ALCEDO_BUILD_IO_TESTS` | ON |
| `ALCEDO_BUILD_RAW_TESTS` | ON |
| `ALCEDO_BUILD_GPU_TESTS` | ON |
| `ALCEDO_BUILD_EDIT_TESTS` | ON |
| `ALCEDO_BUILD_UI_TESTS` | ON |
| `ALCEDO_BUILD_APP_TESTS` | ON |
| `ALCEDO_BUILD_STORAGE_TESTS` | ON |
| `ALCEDO_BUILD_DEMO_TARGETS` | OFF |
| `ALCEDO_BUILD_CI_TESTS` | OFF |
| `ALCEDO_BUILD_SEMANTIC_SIDECAR` | ON |
| `ALCEDO_BUILD_ARIA2C_FETCH` | ON |

**Manifest compare (pre helper extract → post helper extract, same `win_debug` tree)**

| Artifact | Pre | Post | Diff |
| --- | ---: | ---: | --- |
| Ninja simple phony target names | 1173 | 1173 | 0 missing, 0 added |
| CTest discovered test names | 1300 | 1300 | 0 missing, 0 added |
| CTest labels / working directory / timeout identity | 1300 | 1300 | 0 after normalizing label separators |
| `ALCEDO_BUILD_*` cache options | — | — | 0 changed |

Key targets present after reconfigure: `alcedo_main`, `AlbumBackendLib`, `EditViewer`,
`EditorRhiViewport`, `EditorRhiHarness`, `EditorSessionService`, `EditorMiniGitMaterializer`,
`EditorSaveCheckpointCoordinatorTest`, `alcedo_tests_app`, `alcedo_tests_ui`.

**Helper and root-path freeze (no target moved)**

| Item | Location / value |
| --- | --- |
| `ALCEDO_SRC_ROOT` | `${CMAKE_CURRENT_SOURCE_DIR}` of `alcedo_studio/src` |
| `ALCEDO_INCLUDE_ROOT` | `${ALCEDO_SRC_ROOT}/include` |
| `ALCEDO_BINARY_ROOT` | `${CMAKE_CURRENT_BINARY_DIR}` of `alcedo_studio/src` |
| `ALCEDO_TEST_ROOT` | `${CMAKE_CURRENT_SOURCE_DIR}` of `alcedo_studio/tests` |
| `ALCEDO_TEST_SUPPORT_ROOT` | `${ALCEDO_TEST_ROOT}/support` |
| `def_library` | `alcedo_studio/src/cmake/AlcedoTargetHelpers.cmake` (public includes use `ALCEDO_INCLUDE_ROOT`) |
| `alcedo_assign_test_category`, DuckDB copy helpers, `alcedo_register_test_target` | `alcedo_studio/tests/cmake/AlcedoTestRegistration.cmake` |
| Legacy test path aliases | `ALCEDO_ROOT_DIR`, `ALCEDO_SRC_DIR`, `ALCEDO_INCLUDE_DIR`, `ALCEDO_TEST_DIR` kept equal to the new roots for existing path lines |

**Declaration inventory (CSV under `build/tmp/phase3a_baseline/`)**

| Kind | Count | Owner today |
| --- | ---: | --- |
| First-party source library/executable declarations inventoried | 91 | `alcedo_studio/src/CMakeLists.txt` |
| Test library/executable/custom declarations inventoried | 152 | `alcedo_studio/tests/CMakeLists.txt` |

Domain first-appearance order in the source root (not alphabetical; early `UiLocalization` makes
`ui/alcedo_main` appear before mid-file app targets):

`concurrency → opencl → utils → nn → cuda → ui/alcedo_main → metal → image → decoders → storage → edit → app → sidecar_client → ui/editor_rhi`

Frozen **composition order for Phase 3B/3D `add_subdirectory`** (dependency-preserving; UI shell
last among first-party code):

1. `concurrency`, `utils`, `cuda`, `opencl`, `metal`, `nn`, `image`
2. `decoders`, `edit`, `sleeve`, `storage`, `io`, `sidecar_client`
3. `app` (after storage/edit deps exist)
4. `ui/editor_rhi` then `ui/alcedo_main` (plan-required RHI-before-shell order)
5. packaging / install only after `alcedo_main` exists

`if(TARGET ...)` guards that fix relative order after the split:

- `if(TARGET puerhlab_lensfun_build)` around Operators lensfun wiring
- `if(TARGET EditViewer)` before Editor RHI / widget-editor paths that link it
- `if(TARGET alcedo_main AND ALCEDO_DUCKDB_...)` for DuckDB extension post-build copy

Test domain first-appearance order (category aggregates stay in the test root):

`ui → raw → app → cuda → image → metal → sleeve → edit → io → utils → opencl → ci`

Phase 6C-focused owner map for later moves (single owner each):

| Target | Owning domain / future manifest |
| --- | --- |
| `EditorSaveCheckpointCoordinator`, `EditorSaveCheckpointService`, `EditorSession*`, Mini-Git service libs, other `src/app/*` services | `app` → `alcedo_studio/src/app/CMakeLists.txt` |
| `EditorRhiContracts`, `EditorRhiViewport`, `EditorRhiHarnessLib`, `EditorRhiHarness` | `ui/editor_rhi` |
| `UiLocalization`, `BackgroundTaskController`, `AlbumBackendLib`, `EditViewer`, `alcedo_main` | `ui/alcedo_main` |
| `EditorSaveCheckpointCoordinatorTest`, `EditorSaveCheckpointServiceTest`, `EditorSession*Test` (app folder) | `tests/app` |
| Album-backend / QML / RHI UI tests under `tests/ui/` | `tests/ui` |
| `EditorSaveCheckpointCaptureTest`, `EditorMiniGit*Test`, other `tests/edit/history/*` | `tests/edit` |

Phase 3A is complete: baseline and post-extract target/CTest sets match, helpers and root paths are
in place, and no production target declaration moved. Phase 3B may begin moving app/UI source
declarations into domain manifests.

##### Phase 3B - Move application and UI source target declarations intact

Files:

- new `src/app/CMakeLists.txt`
- new `src/ui/CMakeLists.txt`
- new `src/ui/editor_rhi/CMakeLists.txt`
- new `src/ui/alcedo_main/CMakeLists.txt`
- `src/CMakeLists.txt`
- `src/cmake/AlcedoTargetHelpers.cmake`

Checklist:

- [ ] Move each complete app target declaration, including its source/header list, compile
      definitions, platform branches, generated protobuf inputs, post-build work, and link rules,
      into `src/app/CMakeLists.txt`. This includes the editor-session, save-checkpoint, Mini-Git,
      project, import/export, thumbnail, semantic, image-analysis, and AI service targets whose
      implementation/API ownership is app. `ProjectService` may continue to compile the existing
      `ui/alcedo_main/album_backend/path_utils.cpp` until a separate source-ownership change; this
      phase neither moves that file nor introduces an app-to-UI target dependency.
- [ ] Keep `src/ui/CMakeLists.txt` as an assembly file that adds `editor_rhi` before `alcedo_main`.
      Move all `EditorRhi*` target declarations and shader setup into the former. Move
      `UiLocalization`, `BackgroundTaskController`, `AlbumBackendLib`, dialog source selection,
      `alcedo_main`, QML-module registration, translations, application icons, and finalization into
      the latter.
- [ ] Preserve the exact `ALCEDO_REAL_WIDGET_EDITOR`, CUDA, OpenCL, Metal, Windows, and Apple
      branches around the targets they currently affect. Do not centralize feature branches merely
      because their source files now live in different manifests.
- [ ] Make every moved path explicit and stable in its new directory scope. Generated protobuf and
      shader paths must continue to use the same binary directory and must not be duplicated.
- [ ] Build `alcedo_main`, `AlbumBackendLib`, `EditorRhiViewport`, `EditorRhiHarness`, and
      `EditorSessionService` after this substage. A successful configure alone is not sufficient.

##### Phase 3C - Put Phase 6C test registration beside the owning tests

Files:

- new `tests/app/CMakeLists.txt`
- new `tests/ui/CMakeLists.txt`
- new `tests/edit/CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/cmake/AlcedoTestRegistration.cmake`

Checklist:

- [ ] Move the app test declarations and their fixture/resource setup into `tests/app/CMakeLists.txt`.
      This includes `EditorSaveCheckpointCoordinatorTest`, `EditorSaveCheckpointServiceTest`,
      `EditorSessionLifecycleTest`, `EditorSessionNavigationControllerTest`,
      `EditorSessionRenderControllerTest`, `EditorSessionEditControllerTest`, and
      `EditorSessionServiceFacadeTest`, together with the existing app service tests in that folder.
- [ ] Move the UI test declarations into `tests/ui/CMakeLists.txt`. This includes the album-backend,
      session-port, QML action, workspace-shell, RHI, and editor adjustment targets. Keep the
      `def_ui_test` helper local to this manifest or make it a focused helper in the test CMake module;
      it must not return to `tests/CMakeLists.txt`.
- [ ] Move Mini-Git history/capture declarations such as `EditorSaveCheckpointCaptureTest`,
      `EditorMiniGitJournalFoldTest`, `EditorMiniGitCommitWriterTest`,
      `EditorMiniGitJournalRecoveryTest`, and `EditorMiniGitMaterializerTest` into
      `tests/edit/CMakeLists.txt`; retain their current test-source paths and focused fixtures.
- [ ] Register every moved target with `alcedo_register_test_target` immediately after its discovery
      and any post-build copy rule. Preserve targets that belong to a second aggregate such as a CI
      category by registering that additional category at the same location.
- [ ] Verify that no test target becomes hidden by default unintentionally: the old option value must
      produce the same `EXCLUDE_FROM_ALL`, `EXCLUDE_FROM_DEFAULT_BUILD`, category aggregate, and
      `alcedo_tests_all` membership as before the split.

##### Phase 3D - Complete the first-party root split

Files:

- new or retained first-party `src/<domain>/CMakeLists.txt` files for every remaining target-owning
      domain named in the ownership boundary above
- new or retained `tests/<domain>/CMakeLists.txt` files for every remaining target-owning test domain
      named in the ownership boundary above
- `src/CMakeLists.txt`
- `tests/CMakeLists.txt`

Checklist:

- [ ] Move the remaining first-party source target declarations one domain at a time. Each change
      moves whole target declarations and leaves third-party build files untouched.
- [ ] Move the remaining test target declarations one test domain at a time. A target whose source
      file is outside its behavioral domain keeps the owner documented in the declaration inventory;
      do not duplicate it in two manifests.
- [ ] Keep a module CMake file below 500 lines where responsibility permits. A file may exceed that
      guide only for one coherent target with a generated-file list or a complete QML resource list;
      record why it cannot be divided without scattering that target's build definition.
- [ ] Do not split a single target's source list across arbitrary phase-named files. When a target is
      genuinely too large, split it only by a stable target boundary such as editor RHI versus the
      Alcedo UI shell, not by physical line count.
- [ ] Reduce both root manifests to path/setup values, helper inclusion, aggregate target creation,
      and ordered `add_subdirectory` calls. The source root has no direct first-party
      `add_library`/`add_executable`; the test root has no direct first-party
      `add_library`/`add_executable` or `gtest_discover_tests`.

##### Phase 3E - Prove configuration, build, discovery, and category behavior

Run the named targets with the MSVC wrapper. Configure first, then build each target rather than
claiming success from a configure-only result:

- [ ] `cmd /c scripts\msvc_env.cmd --preset win_debug`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSaveCheckpointCoordinatorTest`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSaveCheckpointServiceTest`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSaveCheckpointCaptureTest`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionNavigationControllerTest`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorMiniGitMaterializerTest`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorWorkspaceNavigationQmlTest`.
- [ ] `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_tests_app` and
      `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_tests_ui`.
- [ ] Run the corresponding focused tests with
      `ctest --test-dir build/debug --output-on-failure -R "EditorSaveCheckpoint|EditorSession|EditorMiniGit|EditorWorkspaceNavigation"`
      and record test count, passed/failed/skipped count, and any environment-dependent skips.
- [ ] Re-capture `--target help` and `ctest --show-only=json-v1`; compare both to the Phase 3A
      baseline. Diff only source/build paths that necessarily reflect the new CMake directory, not
      target or test identity.
- [ ] On macOS or the macOS CI worker, configure `macos_debug` and build `alcedo_main` plus one
      affected UI test target. Record that platform separately; Windows evidence does not prove the
      Apple icon, bundle, Metal, or finalization paths.

Phase 3 is complete only when the manifest comparison is clean, the named production and test targets
build, the focused tests execute successfully, all category aggregate memberships match their baseline,
and the root manifests have the restricted responsibilities above. No C++/QML behavior change may be
claimed as part of this phase.

#### Phase 4 - Connect the new modules with typed values

Phase 1 establishes ownership. Phase 4 changes the behavior between those modules without putting the
logic back into the facade.

##### Phase 4A - Pass capture directly from history to checkpoint storage

Files:

- `src/include/app/editor_session_ports.hpp`
- `src/include/app/editor_save_checkpoint_service.hpp`
- `src/app/editor_save_checkpoint_service.cpp`
- `src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp`
- `src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- `src/include/ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp`
- `src/ui/alcedo_main/album_backend/editor_session_checkpoint_store.cpp`
- `tests/edit/history/editor_session_checkpoint_store_test.cpp`
- `tests/CMakeLists.txt`

Checklist:

- [ ] `IEditorHistoryPort::CaptureSaveCheckpoint` returns either an immutable
      `EditorMiniGitSaveCapture` or an error.
- [ ] `IEditorCheckpointStore::SaveAsync` takes that capture by value and owns it until completion.
- [ ] `EditorSaveCheckpointService` is the only orchestrator between capture, store, thumbnail, and
      task completion.
- [ ] Delete `TakeSaveCapture`, `save_captures_`, and every element-ID rendezvous map.
- [ ] A configured project with missing history/store/storage fails; no module reports a successful
      no-op.
- [ ] The Mini-Git path does not call the legacy transaction-array materializer.

Required tests:

- [ ] `ProductionCaptureValueReachesCheckpointStoreWithoutSideMap`.
- [ ] `ConfiguredProjectWithoutHistoryStoreOrStorageFails`.
- [ ] `MiniGitCheckpointDoesNotInvokeLegacyMaterializer`.
- [ ] `CaptureFailureWritesNothingAndLeavesJournalBytes`.

##### Phase 4B - Qualify navigation behavior through its owning controller

Files:

- `src/include/app/editor_session_navigation_controller.hpp`
- `src/app/editor_session_navigation_controller.cpp`
- `src/include/app/editor_save_checkpoint_service.hpp`
- `src/app/editor_save_checkpoint_service.cpp`
- `tests/app/editor_session_navigation_controller_test.cpp`
- `tests/app/editor_save_checkpoint_service_test.cpp`

Checklist:

- [ ] One `PendingEditorAction` stores a named kind and target. A second request cannot replace it.
- [ ] Navigation keeps A's lifecycle acquisition until `SaveCheckpointResult::checkpoint_completed`
      is true, then releases A and acquires B.
- [ ] Capture, write, or truncate failure keeps A and clears the failed pending action without any B
      recovery/render request.
- [ ] The save service ignores duplicate/stale storage callbacks by checkpoint request ID; navigation
      ignores results for an older session generation.
- [ ] Close waits for its checkpoint. Shutdown calls `CancelAndWait` and publishes one terminal result.

Required tests:

- [ ] `SwitchToBWaitsForACommitTruncateAndThumbnailCompletion`.
- [ ] `CheckpointFailureKeepsAAndNeverAcquiresB`.
- [ ] `SecondActionDoesNotReplaceOriginalTargetB`.
- [ ] `DuplicateOrStaleCompletionCannotResumeBOrFinishTaskTwice`.
- [ ] `CloseAndShutdownEachProduceOneTerminalResult`.

Phase 4 is complete when call-site search finds no `TakeSaveCapture`, side map, positional pending-action
initializer, or checkpoint business rule inside `EditorSessionService`.

#### Phase 5 - Grill DuckDB commit, journal truncation, and reopen recovery

Use real project files from `EditorMiniGitProjectFixture`. A test that only checks an in-memory graph
does not prove persistence.

##### Phase 5A - Basic and non-trivial successful saves

Files:

- `tests/edit/history/editor_mini_git_materializer_test.cpp`
- `tests/support/editor_mini_git_project_fixture.hpp/.cpp`

Checklist:

- [ ] Keep `EmptyJournalSucceedsWithoutMovingVersionHead`, but also assert the stored chain hash and
      serialized pipeline state after closing and reopening the project.
- [ ] Keep one-edit coverage with `OneEditWritesCommitAdvancesVersionStoresStateAndTruncatesJournal`.
- [ ] Add `EditHeadMoveAndEditMaterializeInOrderToCapturedHeadAndHash`; this is the ordinary
      non-trivial history case and prevents the suite from testing only empty input and rejection.
- [ ] Add `ManyEditsWithRepeatedFieldsPreserveEveryCommitIdentityAndFinalState`; repeated exposure
      values must remain distinct commits because their timestamps differ.
- [ ] Add a strict pipeline spy and `MaterializationDoesNotReplayOrModifyTheLivePipeline`.

##### Phase 5B - Fail before DuckDB commit

Files:

- `src/include/app/editor_mini_git_commit_writer.hpp`
- `src/app/editor_mini_git_commit_writer.cpp`
- `tests/edit/history/editor_mini_git_commit_writer_test.cpp`
- `tests/CMakeLists.txt`

Checklist:

- [ ] Register `EditorMiniGitCommitWriterTest` with the `editor_history` CTest label and reuse
      `EditorMiniGitProjectFixture`; do not duplicate project/database setup from the basic test file.
- [ ] Add one narrow failure-injection interface around these steps: begin transaction, insert commit
      objects, update Version head/state, and commit the DuckDB transaction. Production code must not
      contain test-only `if` branches.
- [ ] Add parameterized test
      `FailureBeforeDuckDbCommitLeavesEveryDurableFieldUnchangedAfterReopen`.
- [ ] For each failure point, close and reopen the project, then compare commit count, Version head,
      materialized head, transaction-chain hash, serialized pipeline state, and journal bytes with the
      values from before the attempt.
- [ ] Assert that no thumbnail invalidation and no B load event occurred.

##### Phase 5C - Fail after DuckDB commit but before journal cleanup

Files:

- `src/include/app/editor_mini_git_journal_recovery.hpp`
- `src/app/editor_mini_git_journal_recovery.cpp`
- `tests/edit/history/editor_mini_git_journal_recovery_test.cpp`

Checklist:

- [ ] Make `EditorMiniGitMaterializeResult` distinguish `database_committed` from
      `checkpoint_completed`.
- [ ] Add failure points for opening the journal for truncation, truncating the captured range, and
      flushing the file.
- [ ] Stop the actual save at those points. Do not simulate the state by completing a save and writing
      journal records back afterward.
- [ ] Add `DuckDbCommittedButTruncateFailedRetriesWithoutDuplicateCommit` using the original journal
      bytes.
- [ ] Add `JournalFlushFailureRemainsIncompleteAndRecoversOnReopen`.
- [ ] Run the same recovery twice and assert the commit count and Version head move only once.

##### Phase 5D - Invalid and large inputs

Add stale/malformed fold cases to `EditorMiniGitJournalFoldTest` and missing-target/retry/scale cases to
`EditorMiniGitJournalRecoveryTest`:

- [ ] `StaleSourceHeadOrChainHashWritesNothing`.
- [ ] `MalformedDuplicateOrOutOfOrderRecordWritesNothing`.
- [ ] `MissingTargetCommitWritesNothing`.
- [ ] `LargeJournalPrefixHasLinearRecordVisitsAndBoundedCopies` using instrumented counters. Record
      elapsed time and peak test-process memory as diagnostic output, but do not use a machine-specific
      time limit as the only assertion.

Phase 5 is complete when fold, commit-writer, recovery, and materializer-facade targets pass; every
persistence assertion is made after a real reopen; no journal-file error is ignored; and presenting an
already committed prefix repeatedly does not create another commit or move the Version again.

#### Phase 6 - Prove the real UI locks and the complete A-to-B workflow

##### Phase 6A - The real editor-save task publishes the five locks

Files:

- `src/ui/alcedo_main/album_backend/editor_session_task_port.cpp`
- `src/ui/alcedo_main/album_backend/background_task_controller.cpp`
- `src/ui/alcedo_main/album_backend/interaction_policy_controller.cpp`
- `tests/ui/album_backend_interaction_policy_test.cpp`

Checklist:

- [ ] Start a checkpoint through `EditorSaveCheckpointService`; verify it calls
      `EditorSessionTaskPort::BeginTask("editor_save", A)`. The test must not register a
      hand-built equivalent task directly in `BackgroundTaskController`.
- [ ] Observe `SelectEditorImage`, `SwitchWorkspace`, `CheckoutVersion`, `PasteAdjustments`, and
      `MergeAdjustments` through the real `InteractionPolicyController` getters and reason getters.
- [ ] Finish the task as success, failure, and cancellation; all five locks must clear exactly once.
- [ ] Add `ProductionEditorSaveTaskPublishesAndClearsFiveCheckpointLocks` to
      `AlbumBackendInteractionPolicyTest`.
- [ ] Keep history browsing enabled when only `CheckoutVersion` is locked; add
      `VersionCheckoutLockDoesNotDisableHistoryBrowsing`.

##### Phase 6B - Exercise the actual QML entrypoints

Files:

- `src/ui/alcedo_main/qml/EditorWorkspaceNavigation.qml`
- `src/ui/alcedo_main/qml/EditorAdjustmentTransferActions.qml`
- `src/ui/alcedo_main/qml/EditorFilmstrip.qml`
- `src/ui/alcedo_main/qml/EditorHistoryVersionsRail.qml`
- `src/ui/alcedo_main/qml/EditorWorkspace.qml`
- `src/ui/alcedo_main/qml/Main.qml`
- new `tests/ui/editor_workspace_navigation_qml_test.cpp`
- new `tests/ui/editor_adjustment_transfer_actions_qml_test.cpp`
- new `tests/ui/editor_checkpoint_qml_integration_test.cpp`
- `tests/ui/main_qml_test_fixture.hpp/.cpp`
- `tests/CMakeLists.txt`

Run the focused component targets first, then `EditorCheckpointQmlIntegrationTest`. For each action
below, start a real editor-save task, invoke the named QML entrypoint, and assert both a non-empty reason
and zero backend calls:

- [ ] filmstrip `activateImage(index)` does not emit `imageActivated`;
- [ ] Library/Editor workspace button click does not call `WorkspaceRouter`;
- [ ] `EditorHistoryVersionsRail.versionCheckoutEnabled` is false while the History panel can still
      open. Version checkout UI belongs to 6C-6; do not invent that control in this qualification;
- [ ] `requestPasteAdjustments()` does not open the dialog or call the adjustment-transfer backend;
- [ ] Merge strategy does not call the adjustment-transfer backend.

Then finish the task and repeat one allowed action from each component to prove the controls recover.
Checking only `InteractionPolicyController` getters is not enough for this stage.

##### Phase 6C - One production-style A-to-B integration test

Files:

- new `tests/integration/editor_checkpoint_navigation_test.cpp`
- `tests/support/editor_mini_git_project_fixture.hpp/.cpp`
- `tests/ui/main_qml_test_fixture.hpp/.cpp`
- `tests/CMakeLists.txt`

Add target `EditorCheckpointNavigationTest` and one readable test,
`SwitchFromAToBAfterCheckpointPersistsAAndPresentsB`, with this checklist:

- [ ] Create a real temporary project with image A and image B.
- [ ] Open A and present its first frame.
- [ ] Commit one exposure adjustment on A and verify one journal record exists.
- [ ] Request B through the existing `EditorSessionController::Open(B)` entrypoint. Full filmstrip
      population belongs to 6C-6; do not add it here merely to make this test possible.
- [ ] Pause the controllable worker before DuckDB commit. Assert five UI actions are blocked and B has
      no acquire, recovery, render, or presentation event.
- [ ] Continue the worker. Assert event order:
      `duckdb_commit_a -> journal_truncate_a -> thumbnail_invalidate_a -> task_finish_a ->`
      `acquire_b -> first_frame_b`.
- [ ] Close and reopen the project. Assert A's Version head, transaction-chain hash, serialized
      pipeline state, and exposure value match the state captured before switching.

Add the paired failure test `FailedCheckpointKeepsAOpenAndDoesNotTouchBOrThumbnail`, stopping before
DuckDB commit and asserting zero B and thumbnail events.

Phase 6 is complete only when the C++ policy tests, QML action tests, and production-style integration
test all pass. A controller-only test cannot substitute for QML coverage, and a direct materializer
test cannot substitute for the A-to-B workflow.

#### Phase 7 - Run the final evidence checklist

Phase 7 is verification, not a place to postpone cleanup. If a large rename, fixture, or file move is
still needed, return it to Phase 1 and review it there.

##### Build and run the named module targets

- [ ] Build each target with the repository MSVC wrapper, replacing `<TargetName>` with the exact name
      from the list below:
      `cmd /c scripts\msvc_env.cmd --build build\debug --target <TargetName>`.
- [ ] `EditorSaveCheckpointCoordinatorTest`.
- [ ] `EditorSaveCheckpointServiceTest`.
- [ ] `EditorSaveCheckpointCaptureTest`.
- [ ] `EditorSessionLifecycleTest`.
- [ ] `EditorSessionNavigationControllerTest`.
- [ ] `EditorSessionEditControllerTest`.
- [ ] `EditorSessionRenderControllerTest`.
- [ ] `EditorSessionServiceFacadeTest`.
- [ ] `EditorMiniGitJournalFoldTest`.
- [ ] `EditorMiniGitCommitWriterTest`.
- [ ] `EditorMiniGitJournalRecoveryTest`.
- [ ] `EditorMiniGitMaterializerTest`.
- [ ] `EditorSessionHistoryPortTest`.
- [ ] `EditorSessionJournalWriterPortTest`.
- [ ] `EditorSessionCheckpointStoreTest`.
- [ ] `EditorSessionThumbnailPortTest`.
- [ ] `AlbumBackendInteractionPolicyTest`.
- [ ] `EditorWorkspaceNavigationQmlTest`.
- [ ] `EditorAdjustmentTransferActionsQmlTest`.
- [ ] `EditorCheckpointQmlIntegrationTest`.
- [ ] `EditorCheckpointNavigationTest`.
- [ ] Run the pre-existing `WorkspaceShellTest` regression target after its shared QML fixture move.

Record the exact command, discovered test count, passed/failed/skipped count, and any environmental
limitation for every target. Do not report a compiled but unexecuted target as passing.

##### Review the final code shape

- [ ] Run `clang-format --dry-run --Werror --style=file` on every C++ file changed by Phase 1 through
      Phase 6.
- [ ] Search for removed names: `ScopedLock`, `MaterializeValidatedGraph`, `TakeSaveCapture`,
      `no_journal_changes`, `saveInProgress`, and checkpoint-related `controlsEnabled`.
- [ ] Verify temporary same-class split files `editor_session_checkpoint.cpp` and
      the retired combined checkpoint source no longer exists. Its behavior must live in the
      owning component types named in Phase 1.
- [ ] Search for `std::this_thread::yield()` in the save-checkpoint path.
- [ ] Record total LOC and diff LOC for every changed file. For a file above 1000 lines, state which
      responsibilities remain. Do not split old out-of-scope responsibilities merely to close this qualification.
- [ ] For every changed class, list the mutable fields it owns and its single reason to change. Fail
      the audit if two modules share mutable state, a facade owns component state, or a new class mixes
      lifecycle, save, navigation, edit, render, persistence, or QML policy responsibilities.
- [ ] Verify every changed C++ function has a useful Doxygen comment and every changed QML function has
      a concise purpose/input/blocked-result comment.
- [ ] Verify the call chain at the start of this section matches the final function names.

##### Performance evidence

- [ ] Run the large-prefix test with a realistic count such as 10,000 records.
- [ ] Record record visits, graph copies, database-lock duration, capture duration, elapsed time, peak
      memory, and maximum waiting workers.
- [ ] Fix only measured problems: repeated full-graph copies, more than one pass over each journal
      record without a documented reason, broad database-lock scope, or unbounded worker creation.
- [ ] Put each optimization and its before/after measurement in the same review unit.

#### Phase 6C-5 qualification completion checklist

- [ ] A real threaded test proves that only one global save checkpoint runs at a time.
- [ ] A captured journal range cannot delete an edit outside that range.
- [ ] The production path passes one typed capture from history, through
      `EditorSaveCheckpointService`, to `IEditorCheckpointStore`; no side map remains.
- [ ] Basic, mixed-history, invalid-input, pre-commit failure, post-commit failure, retry, reopen, and
      large-prefix tests pass.
- [ ] The five real QML actions are blocked with a reason during save and recover afterward.
- [ ] The A-to-B integration test proves DuckDB commit, journal truncation, A thumbnail invalidation,
      task completion, B acquisition, B first frame, and A state after reopen.
- [ ] The failure integration test proves no B call and no thumbnail invalidation.
- [ ] Test files are divided by module. Focused fixtures create only their module collaborators; only
      the integration fixture composes lifecycle, checkpoint, edit, render, QML, and persistence.
- [ ] `EditorSessionService` is a facade, not the owner of guards, pending save/navigation, callback
      shutdown, adjustment snapshots, or render/first-frame state.
- [ ] Mini-Git folding, DuckDB writing, recovery, production history capture, checkpoint storage, and
      thumbnail invalidation have separate types with no private-state reach-through.
- [ ] Naming, LOC analysis, Doxygen comments, formatter, terminology scans, and recorded performance
      evidence are complete.
- [ ] Update this document's Status only after every box above has executable evidence.

### Phase 6C-6 - Checkout, session switching, and garbage collection

Deliverables:

- Rebuild checkout from root plus first-parent replay.
- Route image switch, workspace switch, Version checkout, and shutdown through the save checkpoint.
- Return the existing live snapshot to `PipelineMgmtService` when leaving an image; do not add a
  second editor-specific snapshot copy.
- Mark from all Version heads through both parents and delete unreachable commits on clean project
  exit.

Acceptance:

- A -> B -> A restores the correct Version, root, head, chain hash, pipeline state, and rendered frame.
- Edit after undo abandons the old redo path, and clean project exit collects it when no Version or
  merge parent reaches it.
- Checkout never exposes a partially reconstructed pipeline.

### Phase 6C-7 - Panel state publication

Deliverables:

- Publish the authoritative read-only field snapshot and monotonically increasing revision through
  `EditorSessionController`.
- Implement idempotent `loadFromSnapshot` for Tone and the explicit distribution list in
  `EditorAdjustmentStack`.
- Publish state after open, checkout, undo, redo, recovery, Paste, and Merge.

Acceptance:

- Opening or switching to an edited image fills Tone values and curve points before controls enable.
- Reapplying a snapshot produces no commit, render request, timer restart, or focus change.
- Pipeline values, rendered frame, panel values, working head, and chain hash remain equal after every
  state-changing operation.

### Phase 6C-8 - Paste, Merge, and history integration

Deliverables:

- Implement root-relative Paste branch creation and checkout.
- Implement target-local incoming branches, two-parent merge commits, and a typed per-field conflict
  resolution request/result surface for the Phase 7A UI.
- Make Library and Editor use the same service path.

Acceptance:

- Paste never inherits the previously active Version's commits.
- Merge moves the current Version only after every conflict has a UI-provided resolution.
- Canceling Merge writes no commit or ref change.
- Merge reconstruction uses the stored resolved payload and produces the same pipeline after reopen.

### Phase 6C-9 - Recovery, thumbnail, and destructive-cutover qualification

Deliverables:

- Update the recovery simulator and forced-termination cases for commit objects, head-move records,
  Version refs, incremental hashes, save-checkpoint serialization, and direct truncation.
- Schedule thumbnail refresh only after successful materialization.
- Delete production dependencies on array-owned Version transactions, cursor persistence,
  `WorkingVersion`, and `RewriteTimeline`.

Acceptance:

- Forced termination at journal append, DuckDB commit, and truncate points selects one valid head and
  never duplicates a commit.
- The focused filmstrip thumbnail refreshes after save and never before a failed save.
- Repository scans find no production path that opens the old project schema or reconstructs a
  Version from a stored transaction array.

## End-to-end acceptance sequence

Use one real RAW image and deterministic clocks:

1. import the image and record its immutable root;
2. drag exposure through multiple preview values and release once;
3. verify one journaled edit commit and one live chain-hash advance;
4. autosave and verify the commit row, Version head, serialized pipeline state, recovery metadata, and
   truncated journal;
5. undo, redo, undo, then edit contrast and verify the old redo child becomes unreachable;
6. create a second Version, checkout it, and verify reconstruction from root;
7. Paste a root-relative adjustment branch and verify HEAD switches to the new Version;
8. Merge another branch, resolve conflicting fields through the typed UI result, and verify the
   ordered two-parent commit;
9. switch A -> B -> A and verify saving completes before B loads;
10. reopen the project and compare panel values, pipeline result, Version head, commit ancestry, and
    transaction-chain hash;
11. exit cleanly and verify unreachable commits are collected;
12. attempt to open an old project and verify the incompatible-format error.

## Definition of done

Phase 6C is complete only when:

- Version is a stable branch/ref and no longer owns a copied transaction array;
- edit and merge commits are immutable objects with one or two ordered parents and timestamped hashes;
- the live pipeline and materialized Version advance the same incremental transaction-chain hash;
- editor open and checkout validate root/head/chain, while thumbnail generation does not duplicate
  that work;
- save, image selection, Version checkout, workspace switching, Paste, and Merge follow the global
  save checkpoint;
- successful materialization truncates the saved log prefix without duplicate recovery;
- Paste checks out a new root-relative branch and Merge stores UI-resolved fields in a two-parent
  commit;
- clean project exit removes commits unreachable from every Version and merge parent;
- panel loading is read-only and idempotent; and
- old project files are rejected with no migration or compatibility path.
