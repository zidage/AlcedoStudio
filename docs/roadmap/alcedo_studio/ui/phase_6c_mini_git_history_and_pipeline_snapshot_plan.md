# Phase 6C Mini-Git History and Pipeline Snapshot Plan

Date: 2026-07-22

Status: approved design; 6C-1, 6C-2, 6C-2-Fix, and 6C-3 implemented.
6C-4 production journal cutover may proceed on the validated serialized pipeline state API.

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
