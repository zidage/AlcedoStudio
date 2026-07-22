# Phase 6C Mini-Git History and Pipeline Snapshot Plan

Date: 2026-07-22

Status: approved design; 6C-1, 6C-2, 6C-2-Fix, 6C-3, 6C-4, and 6C-5 implemented.
6C-5-Fix is a blocking qualification and maintainability correction package. Do not begin 6C-6
checkout, session switching, or garbage collection until every 6C-5-Fix stage is complete.

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

### Phase 6C-5-Fix - Save checkpoint qualification and maintainability corrections

This is a blocking correction package discovered during the 6C-5 review. The focused tests added in
6C-5 pass, but they do not yet prove the complete production path from editor navigation through
capture, materialization, journal truncation, thumbnail invalidation, and navigation resumption. The
review also found that the purported concurrent coordinator test is single-threaded, the `phase6c`
CTest label selects no tests, newly added files do not pass the repository formatter, and several
changed files exceed 1000 physical lines.

Implement the stages below in order. Each stage must leave its named tests green before the next
stage starts. Do not interpret a passing direct materializer test as proof that the production ports,
QML entrypoints, or asynchronous completion path work.

#### Mandatory change-size and review discipline

- Every lettered Fix stage below (`Fix-1A`, `Fix-1B`, and so on) is an independent implementation,
  verification, and commit unit.
- The hard size limit for one lettered stage is 500 changed lines, calculated as additions plus
  deletions across source, tests, QML, CMake, and documentation relative to that stage's starting
  commit.
- If a stage reaches or is projected to reach more than 500 changed lines, stop and split it again by
  responsibility before continuing. Name the children with another suffix such as `Fix-3B-1` and
  `Fix-3B-2`, give each child its own tests and acceptance items, and keep each child at or below 500
  changed lines.
- Do not evade the limit by postponing tests, mixing unrelated cleanup into another stage, reformatting
  unrelated files, or treating file extraction as outside the count. A detected pure rename may use
  Git's rename accounting, but edits made during the move count normally.
- At the end of every lettered stage, record `git diff --numstat` totals and complete physical LOC for
  every changed file. A changed file above 1000 physical lines requires an immediate split plan in the
  same stage; do not defer that decision to the final cleanup.
- Judge behavior only from executed tests and runtime evidence. Source inspection may produce naming,
  responsibility, performance, documentation, or missing-test findings, but it must not be reported as
  proof of behavioral correctness.

#### Review-skill requirement mapping

Every requirement from `$grill-code-review` has an owning stage:

| Review requirement | Owning Fix stages |
| --- | --- |
| Read scope, design, changed files, acceptance criteria, and complete LOC | Fix-1A and every stage exit |
| Keep observed failures, coverage gaps, and maintainability findings separate | Fix-1A and Fix-6I |
| Basic and invariant unit tests | Fix-1B, Fix-2A, Fix-3A |
| Boundary, malformed-input, stale-state, and safety tests | Fix-2C, Fix-4B, Fix-4D |
| State transitions, retry, cancellation, partial failure, and idempotence | Fix-2B, Fix-3C, Fix-4B, Fix-4C |
| Real persistence, reopen, thread, and production-port integration | Fix-1B, Fix-1C, Fix-4B, Fix-4C |
| End-to-end user actions and externally visible ordering | Fix-5B and Fix-5C |
| Concurrency, bounded work, large inputs, and performance evidence | Fix-2A, Fix-4D, Fix-6H |
| Reusable fixture without repeated environment setup | Fix-1B and Fix-1C |
| Split oversized or mixed-responsibility test files | Fix-1D and Fix-6G |
| Clear established naming without metaphors or duplicate state | Fix-2C, Fix-3A, Fix-6G |
| Small, explicit responsibilities and maintainable asynchronous ownership | Fix-2A through Fix-3C and Fix-6A through Fix-6F |
| Avoid busy waits, broad lock scopes, repeated graph copies, and unbounded work | Fix-2A, Fix-4D, Fix-6H |
| Report per-file LOC and split files above 1000 lines | every stage exit and Fix-6A through Fix-6G |
| Document success/failure call chains | Fix-6H |
| Doxygen-compatible documentation for every changed function | every implementation stage, audited in Fix-6H |
| Record commands, counts, skipped tests, limitations, and residual risk | every stage exit and Fix-6I |

#### Phase 6C-5-Fix-1 - Executable test suite and shared integration fixture

Goal: make the correction suite selectable, deterministic, and reusable before changing runtime
ownership.

Mandatory stage split:

- **Fix-1A - Test discovery and evidence baseline:** correct labels, add non-empty label assertions,
  record the acceptance matrix, and record the initial LOC/diff inventory.
- **Fix-1B - Project and persistence fixture:** add temporary project/database, two image roots,
  deterministic clocks, reopen helpers, and durable-state inspection.
- **Fix-1C - Asynchronous and UI fixture collaborators:** add the controllable executor, failure-point
  controls, thumbnail event spy, production ports, task registry, and interaction policy wiring.
- **Fix-1D - Focused test targets and file split:** move save-checkpoint service tests out of the
  oversized file and register the five focused targets. Do not add new runtime behavior here.

Each sub-stage must stay within the 500-line limit. If the shared fixture cannot fit, split persistence
setup from editor-runtime setup rather than creating one oversized support file.

Implementation:

- Fix `alcedo_studio/tests/CMakeLists.txt` so `ctest -L phase6c` and
  `ctest -L phase6c_5_fix` both select a non-empty set. Do not pass a semicolon-expanded property list
  that CTest interprets as unrelated property/value pairs.
- Add a configure-time or test-time assertion that both label selections contain tests. A mistyped
  label must fail CI instead of silently running zero tests.
- Create one shared `EditorSaveCheckpointIntegrationFixture` that owns:
  - a temporary project database and journal directory;
  - two image elements with distinct roots and default Version refs;
  - deterministic commit timestamps;
  - real `StorageService`, commit-graph persistence, production pipeline/history/journal/task ports,
    `EditorSessionService`, and `InteractionPolicyController`;
  - a controllable asynchronous executor that can stop before commit, after commit, and before
    truncation without wall-clock sleeps;
  - a thumbnail invalidation spy that records element IDs and ordering;
  - helpers to open A, commit an adjustment, request navigation to B, reopen the project, and inspect
    the durable Version/head/hash/serialized state.
- Put the fixture in a small shared test support file. Do not repeat project, graph, pipeline, journal,
  and temporary-directory construction in every test.
- Move save-checkpoint cases out of the 1000-line
  `tests/app/editor_session_service_test.cpp` into a focused
  `tests/app/editor_session_save_checkpoint_test.cpp`. Preserve behavior-oriented test names.
- Add focused test targets instead of adding more unrelated functions to an existing large test
  binary:
  - `EditorSaveCheckpointCoordinatorTest`;
  - `EditorSaveCheckpointCaptureTest`;
  - `EditorMiniGitMaterializerFailureTest`;
  - `EditorSaveCheckpointIntegrationTest`;
  - `EditorSaveCheckpointQmlTest`.
- Format all new and changed C++ files before considering this stage complete.

Acceptance:

- `ctest --test-dir build/debug -L phase6c -N` lists at least one test.
- `ctest --test-dir build/debug -L phase6c_5_fix -N` lists every correction target above.
- Running either label with `--output-on-failure` executes tests rather than reporting
  `No tests were found`.
- The shared fixture can create, close, reopen, and delete its project without leaked tasks, guards,
  files, or threads.
- No test uses timing sleeps to coordinate save completion or failure points.

#### Phase 6C-5-Fix-2 - Project-owned checkpoint lease and journal-prefix ownership

Goal: make one explicit owner cover the complete save interval and make the captured journal prefix
unambiguous.

Mandatory stage split:

- **Fix-2A - Coordinator and lease:** inject the project-owned coordinator, implement blocking RAII
  ownership, remove the busy loop, and add real threaded exclusion/teardown tests.
- **Fix-2B - Journal-prefix rotation:** make capture/append/truncate share one prefix owner and add
  capture-then-edit, failure-retention, and retry tests.
- **Fix-2C - Capture state cleanup:** remove redundant flags, unused fields, dead wrappers, and
  duplicate truncation surfaces; add contradictory/empty capture boundary tests.

Implementation:

- Replace the process-static coordinator and `yield` retry loop with one coordinator owned by the
  open project/editor runtime and injected into the production save path.
- Introduce a movable RAII checkpoint lease. The lease must be acquired before the live snapshot and
  journal prefix are captured and remain owned through:
  1. committed live-state capture;
  2. DuckDB materialization;
  3. journal truncation and flush;
  4. thumbnail invalidation scheduling;
  5. publication of the terminal save result.
- Do not busy-wait. Waiting workers must block on a standard synchronization primitive, and shutdown
  must be able to cancel or join them deterministically.
- Define one explicit state sequence such as `Idle -> Capturing -> Materializing -> Finishing -> Idle`.
  Publish state changes only from the coordinator; do not infer checkpoint ownership from a generic
  busy flag.
- Make the journal prefix an immutable captured value with an explicit sequence range. Capture and
  append must use the same prefix mutex so a finalized edit cannot enter the file being truncated.
- A finalized edit that arrives after capture must wait behind the checkpoint lease and become the
  first record of the next prefix after successful truncation. It must never be silently added to the
  captured prefix or deleted by truncation.
- If materialization or truncation fails, retain the captured prefix for retry. Do not consume or
  discard the capture before the terminal result is known.
- Remove redundant or unused state and APIs while establishing this ownership:
  - derive the empty-prefix case from the captured record range instead of storing a separate
    `no_journal_changes` boolean;
  - either validate `session_generation` as part of capture identity or remove it;
  - remove `MaterializeValidatedGraph`, `SetRecords`, and duplicate truncation helpers unless the new
    call chain gives them one concrete responsibility.

Required tests:

- `TwoImagesCompetingForCheckpointNeverMaterializeConcurrently` uses two real threads and a barrier;
  it asserts the maximum simultaneous materialization count is one.
- `CheckpointLeaseReleasesAfterSuccessFailureExceptionAndMove` covers every RAII exit.
- `SecondImageCannotAcquireCheckpointUntilFirstFinishes` proves project-wide rather than per-image
  serialization.
- `EditFinalizedAfterCaptureStartsNextJournalPrefix` proves exact record membership before and after
  truncation.
- `FailedCheckpointRetainsCapturedPrefixForRetry` retries the same prefix and observes one durable
  commit.
- `ShutdownJoinsCheckpointWaitersWithoutBusyLoop` uses deterministic notification rather than sleeps.

Acceptance:

- Production code contains no `std::this_thread::yield()` acquisition loop for editor saving.
- The checkpoint lease has one documented owner at every asynchronous boundary.
- Threaded tests demonstrate exclusion and forward progress; a sequential `TryAcquire` test is not
  sufficient.
- A captured prefix and a next prefix cannot reference the same journal record sequence.

#### Phase 6C-5-Fix-3 - Typed production handoff and navigation state machine

Goal: connect the real editor session to the mini-Git materializer without hidden side maps, legacy
journal work, positional booleans, or success fallbacks.

Mandatory stage split:

- **Fix-3A - Typed capture and materialization request:** change the history and journal interfaces,
  pass capture ownership explicitly, validate identity, and remove the element-keyed rendezvous.
- **Fix-3B - Production mini-Git routing:** select only the mini-Git path for configured projects,
  reject missing storage/materializer state, and prove the legacy journal receives no records.
- **Fix-3C - Pending navigation state machine:** replace positional booleans, define second-request and
  close/shutdown behavior, and make completion callbacks idempotent.

Implementation:

- Change `IEditorHistoryPort::CaptureSaveCheckpoint` to return a typed result containing either an
  immutable `EditorMiniGitSaveCapture` or an error. Do not store the only copy in a hidden
  element-keyed map followed by a separate `TakeSaveCapture` call.
- Pass that captured value explicitly into the asynchronous materialization request. The request must
  preserve element ID, session generation when retained, Version ID, root ID, working head, chain
  hash, serialized pipeline state, journal sequence range, and journal path.
- When a mini-Git journal resolver is configured, route production save and recovery exclusively
  through the mini-Git path. Do not also commit the previous transaction-array journal. Bootstrap
  harnesses may use explicit no-project adapters, but a configured project must not report success
  when storage, the capture, or the materializer is missing.
- Replace `PendingNavigation` positional booleans with a typed request kind and named data. Support
  exactly one pending transition. A second image, workspace, Version, Paste, Merge, or close request
  while saving must be rejected with the checkpoint reason and must not replace the first request.
- Keep the GUI thread free of DuckDB and file I/O. Only the short live-state capture may run before
  dispatch, and its render-lock duration must be measured in a focused test.
- Make success and failure completion idempotent. Duplicate or stale callbacks must not reopen an
  image, release a newer session's guards, end a task twice, or publish a second terminal result.
- Preserve image A's pipeline/history guards until the complete checkpoint succeeds. Release A only
  immediately before beginning B's recovery/acquisition path.

Required tests:

- `ProductionSessionCapturesAndMaterializesOneMiniGitPrefixBeforeLoadingB` uses the shared fixture and
  no manually assembled capture.
- `CaptureFailureKeepsAInteractiveAndDoesNotStartSaveTaskOrLoadB` asserts guards and visible state.
- `MissingProjectStorageFailsConfiguredMiniGitSave` rejects instead of returning a no-op success.
- `SecondNavigationDuringCheckpointIsRejectedAndOriginalTargetStillResumes` covers request ownership.
- `CloseAndShutdownDuringCheckpointHaveOneDeterministicTerminalPath` covers teardown.
- `DuplicateAndStaleSaveCallbacksCannotResumeNavigationTwice` covers callback identity.
- `ProductionMiniGitSaveDoesNotWriteLegacyTransactionJournal` checks the real selected files and
  services.

Acceptance:

- Repository call-site scans find no production `TakeSaveCapture` rendezvous or configured-project
  success fallback caused by a missing materializer.
- One typed request visibly connects capture to materialization in the call chain.
- Image B recovery, guard acquisition, and first render are all absent until A reports a complete
  checkpoint.
- The previous transaction-array journal receives no records during a mini-Git save.

#### Phase 6C-5-Fix-4 - Transaction, truncation, and recovery failure qualification

Goal: prove every persistence boundary using real reopen checks and deterministic failure injection.

Mandatory stage split:

- **Fix-4A - Failure-point interfaces:** add narrow storage and journal-file seams plus parameterized
  failure identifiers, without changing success behavior.
- **Fix-4B - Pre-commit atomicity:** cover every pre-commit failure and full durable-state comparison
  after recreating storage objects.
- **Fix-4C - Post-commit cleanup and retry:** cover original journal bytes, truncate/open/flush
  failures, typed partial outcomes, retry, and duplicate suppression.
- **Fix-4D - Journal variants and scale:** cover mixed head moves, stale/malformed/duplicate records,
  empty prefixes, large prefixes, strict no-pipeline spies, and recorded resource measurements.

Implementation:

- Add narrow failure-injection seams around the storage transaction and journal file operations.
  Keep them in test adapters or small interfaces; do not add test conditionals throughout production
  logic.
- Exercise failures before transaction start, after commit-object insertion, before Version/state
  update, before DuckDB commit, after DuckDB commit, during journal open-for-truncate, and during
  journal flush.
- For every failure before DuckDB commit, close and recreate `DBController`, then verify that commit
  rows, Version head, materialized head, chain hash, serialized pipeline state, and recovery metadata
  all retain their prior values.
- For failure after DuckDB commit but before successful truncation, return a typed result that
  distinguishes `database_committed` from `checkpoint_completed`. Keep navigation blocked for that
  attempt, retain the journal bytes, and let a retry/reopen recognize the already-materialized prefix,
  truncate it, and complete without a duplicate commit.
- Do not reproduce the crash window by first completing a successful truncation and then rewriting a
  synthetic journal. Stop the real production path at the failure point while the original journal
  remains on disk.
- Validate empty, one-edit, many-edit, edit/head-move mixtures, stale source head, stale chain hash,
  duplicate record, malformed record, missing target commit, and a large journal prefix.
- Keep materialization pipeline-free. Add spies that fail the test if materialization requests
  pipeline replay, changes operator state, builds execution stages, or creates a second executor.
- Report truncation and flush failures; do not ignore their error values.

Required tests:

- `EachPreCommitFailureLeavesAllDurableStateUnchangedAfterReopen` is parameterized by the pre-commit
  failure points.
- `CommitSucceededButTruncateFailedRetriesWithoutDuplicateCommit` uses the original journal bytes.
- `FlushFailureLeavesCheckpointIncompleteAndRecoverable` verifies the typed partial result.
- `MixedEditAndHeadMovePrefixMaterializesToCapturedHeadAndChain` covers non-trivial journal order.
- `MalformedOrStalePrefixWritesNoRows` covers safety checks without making them the whole suite.
- `MaterializerNeverCallsPipelineReplayOrMutation` uses strict spies.
- `EmptyPrefixRefreshesOnlyMatchingSerializedStateWithoutMovingVersionHead` reopens and compares all
  fields.
- `LargePrefixMaterializesWithinRecordedTimeAndMemoryTargets` records a baseline without a fragile
  machine-specific absolute threshold.

Acceptance:

- Every failure point has an assertion on all durable fields, not only commit count.
- Recovery is tested by destroying and recreating the project/storage objects.
- No journal cleanup error is discarded.
- The same already-committed prefix can be presented repeatedly without adding a second commit row or
  moving the Version twice.

#### Phase 6C-5-Fix-5 - User-visible locks, thumbnail ordering, and end-to-end navigation

Goal: prove the five locked user behaviors and the complete A-to-B result through their real UI and
production collaborators.

Mandatory stage split:

- **Fix-5A - Production lock publication:** test the real task port, policy propagation, task
  completion, and per-capability C++ behavior.
- **Fix-5B - QML capability bindings and reasons:** remove session-state permission fallbacks, bind
  each action narrowly, expose localized reasons, and test the five real QML entrypoints.
- **Fix-5C - Thumbnail and A-to-B end-to-end sequence:** add the ordered production event log, failure
  assertions, successful navigation, first-frame presentation, and reopen verification.

Implementation:

- Make `EditorSessionProductionTaskPort::BeginTask` the tested producer of editor-save locks. The
  test must observe those locks through `BackgroundTaskController` and
  `InteractionPolicyController`; do not register equivalent locks directly in the test.
- Keep `InteractionPolicyController` as the sole authority for editor navigation capabilities.
  Remove QML fallbacks that infer permissions from `sessionState == Saving`.
- Bind separate capabilities to the actual actions they guard. Do not use `canCheckoutVersion` to
  disable unrelated history browsing, and do not name a general selection block `saveInProgress`.
- Surface the localized blocking reason through tooltip, accessibility description, or the existing
  snackbar path for filmstrip selection, workspace switch, Version checkout, Paste, and Merge.
- Schedule thumbnail invalidation only after DuckDB commit and successful journal truncation/flush.
  A failed or incomplete checkpoint must not invalidate the thumbnail. A retry that completes must
  invalidate exactly once for image A before navigation resumes.
- Add an end-to-end sequence using two real image elements:
  1. open A;
  2. commit an adjustment;
  3. request B from the user-facing entrypoint;
  4. stop materialization and assert all five capabilities are disabled with a reason;
  5. assert B has no recovery, guard, render, or presentation activity;
  6. finish persistence and truncation;
  7. assert A thumbnail invalidation occurs;
  8. assert locks clear;
  9. assert B opens and presents its first frame;
  10. reopen the project and compare A's Version head, chain hash, serialized state, and adjustment.

Required tests:

- `ProductionEditorSaveTaskPublishesAndClearsAllFiveNavigationLocks` starts and ends the real task.
- `EachBlockedQmlActionShowsTheSaveReasonAndPerformsNoBackendCall` covers all five actions.
- `FailedCheckpointKeepsThumbnailAndPendingTargetUntouched` covers failure ordering.
- `SuccessfulCheckpointInvalidatesAOnceBeforeBStartsLoading` asserts an ordered event log.
- `AToBSaveAndReopenPreservesAHeadChainStateAndAdjustment` is the required end-to-end test.
- `HistoryBrowsingRemainsAvailableWhenOnlyVersionCheckoutIsLocked` prevents over-broad UI gating.

Acceptance:

- The UI tests exercise actual QML action entrypoints, not only C++ capability getters.
- Every disabled action exposes a non-empty localized reason.
- The event log orders DuckDB commit, journal truncate/flush, thumbnail invalidation, lock release,
  B acquisition, and B first render exactly as specified.
- Failure paths make zero B backend calls and zero thumbnail invalidation calls.

#### Phase 6C-5-Fix-6 - File decomposition, documentation, and performance evidence

Goal: leave the corrected save path small enough for an AI agent or human to navigate safely.

Mandatory stage split:

- **Fix-6A - Production task and pipeline port extraction:** move only those two responsibilities and
  preserve behavior with existing tests.
- **Fix-6B - Production history port extraction:** move history and capture code with no behavior
  change.
- **Fix-6C - Production journal/materializer port extraction:** move persistence orchestration and
  keep its tests green.
- **Fix-6D - Production scheduler extraction:** move scheduling/rendering code separately; if the move
  exceeds 500 changed lines after Git rename accounting, split scheduler helpers from worker
  lifecycle.
- **Fix-6E - Session save-state extraction:** separate save/navigation completion from lifecycle and
  render routing.
- **Fix-6F - QML root decomposition:** extract workspace navigation and adjustment transfer in
  separate commits.
- **Fix-6G - CMake, test-file, naming, and dead-API cleanup:** split registries by subsystem, finish
  test-file decomposition, and remove misleading or unused surfaces.
- **Fix-6H - Doxygen, call chains, formatting, and performance evidence:** document every changed
  function, publish success/failure chains, format the complete scope, and record non-trivial
  performance measurements.
- **Fix-6I - Final evidence report:** run the full matrix and record commands, pass/fail/skip counts,
  per-file LOC, per-stage diff totals, environmental limitations, and remaining risks before changing
  the Status line.

File extraction is not permission for a large commit. Each port, QML responsibility, registry, and
documentation pass remains an independent stage subject to the 500-line limit.

Implementation:

- Split `editor_session_production.cpp` by production port responsibility. Put pipeline, history,
  journal/materialization, task, and scheduler implementations in separate files. Keep the mini-Git
  capture/materialization bridge with the journal/history boundary, not with rendering.
- Split the save-checkpoint state machine from `editor_session_service.cpp` so editor lifecycle,
  render routing, and save/navigation completion no longer share one large implementation file.
- Split `Main.qml` workspace navigation and adjustment-transfer actions into focused components or
  controllers. Keep the root window responsible for composition rather than feature logic.
- Split source and test CMake registration into subsystem include files while preserving target names
  and labels.
- Keep every changed implementation and test file below 1000 physical lines unless a generated or
  declarative registry has a written responsibility-based reason to remain larger.
- Remove dead wrappers and misleading state. Prefer names such as `selectionBlocked`,
  `versionCheckoutEnabled`, `capturedPrefix`, and `checkpointLease` when they match the final
  responsibility; do not introduce metaphors or duplicate synonyms.
- Add Doxygen-compatible documentation to every new or changed C++ function in this correction
  package. Each comment must cover purpose, parameters, return value, preconditions, ownership,
  side effects, thread affinity or thread safety, and failure behavior where applicable. QML helper
  functions must carry the equivalent concise documentation in the local convention.
- Document the final success and failure call chains beside the owning service interfaces so later
  work does not have to reconstruct the asynchronous ownership from implementations.
- Add a benchmark or recorded performance test for large graph/prefix materialization. Measure graph
  copies, database-lock duration, capture render-lock duration, elapsed time, and peak memory. Move
  journal folding outside the database lock when the measured/structural dependency allows it.
- Run the repository formatter and remove every changed-line violation.

Acceptance:

- No changed non-generated source or test file exceeds 1000 physical lines without an explicit
  responsibility-based exception in this plan.
- `clang-format --dry-run --Werror --style=file` passes for every changed C++ file.
- No changed function lacks the required Doxygen-compatible description.
- Repository searches find no unused 6C-5-Fix API, positional navigation booleans, hidden capture
  rendezvous, or busy-wait save loop.
- The performance result records inputs and measurements and does not rely on a trivial one-commit
  graph.

#### Phase 6C-5-Fix exit gate

6C-5-Fix is complete only when all of the following are true:

- all Fix-1 through Fix-6 acceptance items pass;
- the production A-to-B test covers capture, DuckDB transaction, original journal truncation,
  thumbnail invalidation, lock release, B acquisition, B first render, and reopen verification;
- threaded tests prove global exclusion and next-prefix behavior;
- every specified failure point preserves or recovers the exact durable state;
- `ctest --test-dir build/debug -L phase6c --output-on-failure` executes a non-empty green suite;
- `ctest --test-dir build/debug -L phase6c_5_fix --output-on-failure` executes a non-empty green suite;
- relevant broader editor, history, storage, interaction-policy, and workspace tests remain green;
- formatting, roadmap terminology, project terminology, test naming, LOC, and documentation checks
  pass; and
- this document's Status line is updated to mark 6C-5-Fix implemented before 6C-6 begins.

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
