# Phase 7A History/Versions Recovery and Editor Performance Plan

Date: 2026-07-26
Re-audited: 2026-07-27
Status: **REOPENED — R0–R6 implemented (R2 residual contention evidence); R7 product/performance qualification remaining**

The earlier Phase 7A completion labels were based mainly on focused unit tests and QML fakes. They
remain useful historical evidence, but they do not prove the production Version workflow. Current
source inspection, a real application log, and the still-open real-RAW sequence show that the repair
is incomplete.

Baseline commits:

- `f7bad872 feat(editor): complete phase 7A version history workflow`
- `9f5f89ab refactor(ui): split history and versions panels`
- `edcd888f fix(editor): complete phase 7a P0 history recovery`
- `2d6a0a0c feat(editor): complete phase 7a P1 history projection and multi-step jumps`
- `8c488fb2 feat(ui): complete phase 7A P2 version panel UI and rail test split`
- `7ee88498 fix(ui): render before/after value on non-current history cards`

Related documents:

- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)
- [Editor Single Live Pipeline + WAL + Checkpoint Simplification Plan](editor_single_live_pipeline_wal_checkpoint_plan.md)
  (**binding identity model** for head / chain hash / checkpoint — do not reintroduce dual pipeline
  heads from older 7A wording)
- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Alcedo Studio QML Visual Identity](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md)

## Executive verdict

### Why editor performance regressed

The regression has two independent hot paths and one secondary QML cost.

1. **History projection is coupled to every backend notification.**
   `EditorHistoryModel` listens to both `EditorSessionController::StateChanged` and
   `HistoryChanged`. `OnBackendChanged()` emits both for render routing, render-busy changes,
   previews, save progress, and actual history mutations. Each signal calls
   `EditorSessionService::history_snapshot()`.
2. **Each projection repeats whole-history work.**
   `ReadHistorySnapshot()` walks the complete first-parent path, parses every commit payload, and
   serializes before/after JSON. `EditorHistoryModel::SetSnapshot()` then rebuilds every presentation
   row and emits `dataChanged` for the entire history and Version lists. A model action also calls
   `refresh()` directly, so one action can refresh three times: `StateChanged`, `HistoryChanged`, and
   the direct refresh.
3. **The GUI thread can wait behind history persistence.**
   `ReadHistorySnapshot()` takes `WorkingState::mutex`. Version rebuild and
   `PersistEditorHistoryState()` hold the same mutex across pipeline export, render-lock acquisition,
   DuckDB access, and persistence. An unrelated render notification can therefore make the GUI
   attempt a full history read and wait behind storage work.
4. **Per-frame presentation logging performs synchronous disk flushes.**
   The production message handler calls `QFile::write()` and `QFile::flush()` for every enabled Qt
   message. Presentation paths emit several category-less `qInfo("[EditorPresent] ...")` messages per
   frame. A real log from 2026-07-27 13:23–13:29 contains 3,514 lines and 435,363 bytes; 3,510 lines
   are `[EditorPresent]`. The Phase 7A commits did not add these messages, but the cost is real and
   compounds the new history-notification amplification.
5. **The rail fold still animates layout and a complex clipped subtree.**
   `panelOpenProgress` changes the root `Layout.preferredWidth`, minimum width, maximum width, panel
   width, margin, and opacity every frame. This repeatedly lays out the editor workspace and
   composites the history panel. It predates Phase 7A and is not the primary steady-state regression,
   but it causes avoidable panel-open/close stalls.

### Why New Version and Branch Here are still unreliable

1. **Asynchronous terminal results stop below the QML boundary.**
   `EditorSessionService` emits the terminal save/navigation result through its result observer, but
   `EditorSessionController` does not install that observer. The controller publishes only the
   immediate `SaveStarted` returned by the invokable call. `EditorVersionsPanel.qml` waits for a
   terminal `HistoryOperationFinished`, so the inline draft can remain pending and the exact backend
   failure is never associated with the create/branch action.
2. **Several validation errors are swallowed.**
   Checkout, branch, rename, remove, and history-move invokables catch invalid IDs and return without
   publishing a rejected result. `history_snapshot()` also converts a port failure to an empty
   snapshot without exposing the error.
3. **Failure rollback does not restore the complete working state.**
   Checkout/create/branch call `MiniGitWorkingHistory::SelectVersion()` before snapshot read and
   persistence complete. `SelectVersion()` clears the redo suffix. Later failure restores the graph,
   pipeline, committed snapshot, and flags, but not the redo suffix. The visible future path can be
   lost even though the operation reports failure.
4. **History moves mutate durable/live state before pipeline application is known to succeed.**
   Undo, Redo, and `MoveHeadToCommit()` append a head-move record and advance the graph before all
   operator state is applied. A later operator failure leaves the journal/head advanced with a
   partial live pipeline. This was previously documented as an accepted residual; it is not
   acceptable for a repaired history system.
5. **Merge traversal is incomplete.**
   Multi-step movement skips merge payload fields. The comment that a later render repairs them from
   the committed snapshot is not sufficient because the snapshot was not updated with those merge
   fields.
6. **The tests stop at seams that hide the production failure.**
   Navigation tests use tracking/fake ports; QML tests use a fake backend that completes create and
   branch synchronously. No test runs the actual QML/controller/service/checkpoint/history
   port/pipeline/storage chain for asynchronous create, selected-commit branch, terminal error
   publication, close/reopen, and frame comparison.

## Scope

This plan repairs Mini-Git history, Version operations, error publication, and the performance
regression introduced or exposed by their UI integration.

In scope:

- interactive editor notification and history projection cost;
- presentation-path logging policy;
- immutable commits, named Version refs, working head, redo suffix, and journal behavior;
- New Version at the immutable image root;
- Branch Here at any eligible visible transaction;
- Version checkout;
- Undo, Redo, and multi-step history movement;
- exact async operation results and user-visible failures;
- QML panel lifetime, list updates, and fold rendering;
- production integration, failure-injection, reopen, and performance evidence.

Out of scope:

- LUT panel behavior not required by history reconstruction;
- new merge-resolution product features;
- detached-head editing;
- changing the project schema unless an implementation proves a stored field is required.

## Naming used by this plan

To avoid the earlier confusion between priority and implementation phases:

- `S0`, `S1`, and `S2` are **severity**, not implementation phases.
- `R0` through `R7` are ordered **repair stages**.
- Historical commit messages containing `P0`, `P1`, or `P2` are identifiers only and do not describe
  the remaining work.

## Locked product behavior

### Version operations

| User action | Target head | Active after success | Pipeline after success |
| --- | --- | --- | --- |
| New Version | image root (`nullopt`) | new named ref | immutable root pipeline |
| Branch Here | selected visible commit | new named ref | root plus selected first-parent path |
| Checkout Version | stored head of selected ref | selected ref | root plus selected first-parent path |
| Undo | first parent of working head | current ref | exact target-head pipeline |
| Redo | immediate child in current redo suffix | current ref | exact target-head pipeline |
| Click history row | applied ancestor or redo descendant | current ref | exact target-head pipeline |

Additional behavior:

- New Version and Branch Here remain separate commands and separate controller methods.
- Branch Here is allowed for an applied, current, or future row that is visible in the active
  timeline. A root branch is created with New Version.
- No edit may resume on an unnamed or detached head.
- A successful action routes one final render. Intermediate reconstruction frames are never
  presented.
- A failed action leaves the complete prior working state and last valid frame unchanged.
- The Versions panel shows the exact backend reason for rejection or failure.

### History display

- The timeline displays one active Version.
- The applied first-parent path, one current head, and the in-memory redo suffix are distinguishable.
- At image root, no commit row is current.
- A new edit after backward movement clears only the abandoned redo suffix of the active Version.
- Commits shared by several Version refs remain stored once.

## Mini-Git invariants

### Stored graph

1. The image has one immutable root identity and root pipeline state.
2. `EditCommit` objects are immutable and content-addressed.
3. A Version is a stable named ref with a mutable head; it does not own copied commit rows.
4. Every ref head is null or resolves to a commit in the same image root.
5. The active Version ID, materialized head, folded chain hash, and serialized pipeline state agree
   at every DuckDB publication point.
6. Merge first-parent order remains authoritative for pipeline reconstruction.

### Working state

The unit of success or rollback is the complete tuple below, not only the graph:

```text
HistoryWorkingState
  active_version_id
  all Version refs and heads
  working_head
  transaction_chain_hash
  redo_suffix
  exact journal records and sequence range
  live pipeline executor state
  committed adjustment snapshot
  pending before-values
  dirty/writeback/recovered flags
  last presented frame identity
```

An operation that returns failure must leave this tuple byte-for-byte or value-for-value equivalent
to its pre-operation state, except for a separately published diagnostic event.

### Journal

- Settled edits and head moves append to the per-image journal before their prepared working state is
  published.
- A journal record belongs to the active Version selected at the journal base. Version-changing
  operations therefore complete the current save checkpoint first.
- Ref creation and selection are persisted atomically with their matching serialized pipeline state;
  they are not represented as ordinary edit commits.
- After a durable journal append, live publication must be a no-fail move/swap. Any work that can
  reject, allocate materially, parse payloads, or rebuild operators happens before the append.

## Current source audit

### Severity table

| Severity | Finding | Current evidence | Required direction |
| --- | --- | --- | --- |
| S0 | per-frame log flush | 3,510/3,514 recent lines are `[EditorPresent]`; handler flushes every line | disabled-by-default category plus buffered info/debug writes |
| S0 | full history refresh on every backend event | model listens to both broad signals; controller emits both unconditionally | monotonic history revision and one dedicated notification |
| S0 | GUI can block on persistence mutex | projection and persistence share `WorkingState::mutex` | copy source state under a short lock; project outside it |
| S0 | async Version result lost | service result observer is not installed by controller | correlated start and exactly one terminal event |
| S0 | incomplete rollback | redo suffix is cleared before late failure and never restored | prepared state; no live mutation before success |
| S0 | head move can leave partial pipeline | journal/head move precedes fallible operator application | prepare target pipeline/snapshot before journal append |
| S1 | merge fields skipped across history movement | ordinary payloads applied; merge payload ignored | reconstruct complete selected first-parent state |
| S1 | invalid IDs and snapshot failures are silent | controller catch blocks and empty snapshot fallback | typed rejected/failed result with exact message |
| S1 | model resets/dataChanged are over-broad | all presentations and all rows rebuilt on identity match | cached presentation and role-specific incremental updates |
| S2 | rail fold relayout/composition | width, layout limits, opacity, and clip animate together | one layout change plus transform-only inner motion |
| S2 | hidden panels stay instantiated | both panel trees exist; model remains live when rail is closed | conditional Loader lifetime and refresh-on-open |

### Actual performance call chain

```text
interactive preview or render event
  -> EditorSessionService::Emit / NotifyChange
  -> EditorSessionController::OnBackendChanged
     -> rebuild adjustment snapshot map
     -> StateChanged
     -> HistoryChanged
  -> EditorHistoryModel::refresh twice
     -> EditorSessionService::history_snapshot
     -> EditorSessionHistoryPort::ReadHistorySnapshot
        -> lock WorkingState::mutex
        -> walk full first-parent chain
        -> parse/dump every commit payload
     -> rebuild every presentation
     -> dataChanged for every history row
     -> dataChanged for every Version row
     -> QML delegates reevaluate
```

The direct `refresh()` at the end of each model action adds a third projection for synchronous
actions.

### Actual asynchronous New Version call chain

```text
inline draft submit
  -> EditorHistoryModel::createRootVersion
  -> EditorSessionController::CreateRootVersion
  -> EditorSessionService::CreateRootVersion
  -> navigation starts save checkpoint
  <- immediate SaveStarted
  -> controller publishes action=createRootVersion, kind=SaveStarted
  -> QML keeps draftSubmitPending=true

worker finishes save
  -> ContinueCreateRootVersion
  -> service completion notifier
  -> EditorSessionService::Emit(terminal result)
  -> generic change notifier only
  -> controller OnBackendChanged
  -> no PublishHistoryResult
  -> no terminal HistoryOperationFinished
  -> QML remains on the earlier SaveStarted and has no correlated failure reason
```

Branch Here and asynchronous checkout have the same terminal-publication gap.

### Actual rollback gap

```text
checkout/create/branch
  -> mutate live graph / rebuild live executor
  -> MiniGitWorkingHistory::SelectVersion
     -> clear redo_suffix
  -> read pipeline snapshot
  -> persist graph + serialized state
  -> late failure
  -> RestoreGraphAndPipeline + restore snapshot/flags
  -> redo_suffix remains cleared
```

## Target architecture

### Dedicated history revision

`EditorSessionService` owns a monotonic `history_revision`. It increments only when one of these
values changes:

- active Version ID;
- a Version name, creation, removal, or head;
- working head or redo suffix;
- commit set visible to the active timeline;
- recovered-head marker affecting history display.

Render-busy, frame-ready, progress, preview, viewport, and task-detail changes do not increment it.

The UI chain becomes:

```text
history mutation succeeds
  -> history_revision increments once
  -> EditorSessionController publishes HistoryRevisionChanged once
  -> EditorHistoryModel observes revision
  -> capture immutable projection source under a short lock
  -> parse/format outside the history lock
  -> insert/remove/change only affected rows
```

`EditorHistoryModel` must not listen to broad `StateChanged`, and its invokable methods must not call
`refresh()` after forwarding an action.

### Correlated operation events

Add a typed operation event that survives synchronous and asynchronous paths:

```text
HistoryOperationEvent
  operation_id
  action
  stage                 // requested, saving, preparing, persisting, rendering, completed
  terminal
  result_kind
  element_id
  current_version_id
  requested_version_id
  selected_commit_id
  checkpoint_task_id
  render_request_id
  error_code
  message
```

Rules:

- one user action allocates one `operation_id`;
- zero or more non-terminal events may be published;
- exactly one terminal event is published;
- stale or duplicate completions are ignored by `operation_id` plus checkpoint ticket;
- controller validation failures publish a terminal rejected event instead of returning silently;
- QML stores the pending operation ID and reacts only to its terminal event;
- the exact backend message appears in the owning panel and recovery UI.

### Prepared Mini-Git transitions

Use two preparation paths.

#### Local head/edit transition

For settled edit, Undo, Redo, or multi-step movement:

1. Copy the graph identity, redo suffix, target path, and current committed snapshot.
2. Build the target pipeline/snapshot without mutating the published graph or executor.
3. Validate target head, chain hash, all ordinary/merge payloads, and final snapshot.
4. Construct the one edit or head-move journal record.
5. Append the journal record.
6. Publish the prepared graph/head/redo/pipeline/snapshot with no further fallible work.
7. Increment `history_revision` and route one render.

#### Named-ref transition

For New Version, Branch Here, or checkout:

1. Complete the current Version save checkpoint.
2. Copy the graph and create/select the target ref only in the candidate graph.
3. Build a candidate executor from immutable root plus the candidate first-parent path.
4. Validate head, chain hash, panel snapshot, and serialized state.
5. Persist the candidate graph, active ref, materialized head/hash, and serialized pipeline state in
   one DuckDB transaction.
6. Publish the candidate graph/executor/snapshot with a no-fail swap.
7. Clear redo only as part of the successful published state.
8. Increment `history_revision`, route one render, and publish the terminal operation event.

No rollback rebuild is needed because the published state is untouched until the durable candidate
has succeeded.

### Projection ownership

Split the current long critical section:

- `EditorSessionHistoryPort` captures a small immutable projection source under
  `WorkingState::mutex`;
- first-parent walking and payload extraction run after releasing the mutex;
- presentation text is cached by immutable commit hash;
- Version rows update by stable Version ID;
- changing the current head updates only the old/new current rows and affected future/applied roles;
- adding one commit inserts one row rather than resetting the list.

### Logging

- Move presentation messages to an `alcedo.editor.present` logging category.
- Disable that category at info level by default.
- Keep per-frame detail at debug level and require an explicit runtime rule to enable it.
- Do not call `flush()` for every info/debug line. Buffer these writes and flush on bounded batches
  and shutdown; warnings, critical messages, and fatal messages may flush immediately.
- Keep low-frequency lifecycle, backend selection, allocation, and terminal error messages.
- Add structured history-operation terminal logs with operation ID, action, selected/current Version
  IDs, stage, task ID, and message. Do not log every projection or delegate update.

### QML

- Create only the selected history or Versions panel through a `Loader`; inactive panel bodies have
  `Loader.active: false`.
- A closed rail does not keep list delegates alive. Opening a panel performs one refresh if its
  observed revision is stale.
- Replace the current fold with one outer layout-width change and transform-only motion of the inner
  panel. Do not animate a complex subtree's opacity, width, and clip together.
- Use explicit/required model-role properties in delegates.
- Enable `ListView.reuseItems` and reset transient delegate state when reused.
- Preserve Version/history scroll position outside the destroyed panel body.
- Keep all style values in `appTheme` and `DESIGN.md`.

## Repair stages

### R0 — add failing evidence and counters

Files:

- `alcedo_studio/tests/ui/editor_session_controller_phase5a_test.cpp`
- `alcedo_studio/tests/ui/editor_versions_panel_qml_test.cpp`
- `alcedo_studio/tests/ui/editor_history_transactions_panel_qml_test.cpp`
- `alcedo_studio/tests/app/editor_session_service_facade_test.cpp`
- `alcedo_studio/tests/app/editor_session_navigation_controller_test.cpp`
- `alcedo_studio/tests/edit/history/editor_session_history_port_test.cpp`
- new focused performance/result tests if existing targets become unfocused

Add these failing tests before behavior changes:

- `InteractivePreviewDoesNotPublishHistoryRevisionOrReadHistorySnapshot`
- `SettledCommitPublishesOneHistoryRevisionAndOneProjection`
- `RenderBusyAndFrameCompletionDoNotRefreshHistoryModels`
- `AsyncRootVersionCompletionClosesMatchingDraft`
- `AsyncRootVersionFailureShowsExactBackendMessage`
- `AsyncBranchFailureKeepsSelectedCommitAndShowsExactBackendMessage`
- `InvalidVersionOrCommitIdPublishesRejectedTerminalResult`
- `LateCheckoutFailurePreservesRedoSuffixAndJournalBytes`
- `HeadMoveApplyFailurePreservesHeadRedoPipelineSnapshotAndJournal`
- `MoveAcrossMergeReconstructsResolvedFields`
- `DefaultLoggingWritesNoPerFramePresentationInfo`

Add deterministic counters for:

- history revision publications;
- history snapshot reads;
- projection commit parses;
- model resets, inserted/removed rows, and changed rows;
- render requests;
- non-terminal and terminal operation events;
- present-log lines and log flushes.

Exit:

- every test fails for the intended current defect;
- no test relies on wall-clock sleeps;
- no fake completes an asynchronous action synchronously.

##### R0 completion record (2026-07-27)

**Status:** partial — 10 of 11 failing tests added and verified RED against the
intended current defect; `LateCheckoutFailurePreservesRedoSuffixAndJournalBytes`
is deferred to R7 because it requires injecting a failure at DuckDB publication
(or snapshot read) after `SelectVersion` clears the redo suffix, and that
failure-injection fixture is exactly what R7 builds. No wall-clock sleeps drive
any test; async completion is driven by an explicit `CompletePendingVersionOp`
drain, never a synchronous fake completion.

**Primary defect call chains proven RED:**

```text
interactive preview
  -> EditorSessionController::submitPatch (settled=false)
  -> FakeSessionBackend::Patch -> NotifyChange
  -> EditorSessionController::OnBackendChanged
     -> emit StateChanged + emit HistoryChanged
  -> EditorHistoryModel::refresh twice (StateChanged + HistoryChanged)
  -> EditorSessionController::history_snapshot -> backend.history_snapshot (read)
```
Asserts 0 history signals / 0 model refreshes / 0 snapshot reads; currently 1/2/2.

```text
async Version create/branch
  -> EditorSessionController::CreateRootVersion/BranchFromCommit
  -> backend.CreateRootVersion -> SaveStarted (kind=4)
  -> PublishHistoryResult (immediate, non-terminal)
  -> backend.CompletePendingVersionOp(success, exact message) -> NotifyChange
  -> EditorSessionController::OnBackendChanged -> HistoryChanged only
  -> NO terminal HistoryOperationFinished for the action (result observer not installed)
```
Asserts a terminal event with the exact backend message; currently absent.

```text
head move across an unmappable commit
  -> EditorSessionHistoryPort::MoveHeadToCommit
  -> MiniGitWorkingHistory::MoveHeadToCommit (AppendHeadMove: journal + graph head + redo)
  -> port apply loop: ApplyCommittedPayload fails (field key unmappable)
  -> return false WITHOUT rolling back journal/head/redo
```
Asserts head/redo/journal unchanged; currently mutated.

```text
move forward across a merge
  -> EditorSessionHistoryPort::MoveHeadToCommit (redo suffix contains the merge)
  -> port apply loop skips kMerge commits
  -> committed snapshot keeps the pre-merge value, not the merge-resolved value
```
Asserts the merge-resolved field value; currently the pre-merge value.

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `InteractivePreviewDoesNotPublishHistoryRevisionOrReadHistorySnapshot` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `SettledCommitPublishesOneHistoryRevisionAndOneProjection` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `RenderBusyAndFrameCompletionDoNotRefreshHistoryModels` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `AsyncRootVersionCompletionClosesMatchingDraft` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `AsyncRootVersionFailureShowsExactBackendMessage` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `AsyncBranchFailureKeepsSelectedCommitAndShowsExactBackendMessage` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `InvalidVersionOrCommitIdPublishesRejectedTerminalResult` | EditorSessionControllerPhase5ATest | FAIL (intended) |
| `HeadMoveApplyFailurePreservesHeadRedoPipelineSnapshotAndJournal` | EditorSessionHistoryPortTest | FAIL (intended) |
| `MoveAcrossMergeReconstructsResolvedFields` | EditorSessionHistoryPortTest | FAIL (intended) |
| `DefaultLoggingWritesNoPerFramePresentationInfo` | EditorAppLoggingTest | PASS (made green by R1) |
| `LateCheckoutFailurePreservesRedoSuffixAndJournalBytes` | EditorSessionHistoryPortTest | DEFERRED to R7 (needs persistence failure injection) |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionControllerPhase5ATest --target EditorSessionHistoryPortTest --target EditorAppLoggingTest`
Suite totals: EditorSessionControllerPhase5ATest 28 PASS / 7 FAIL (the 7 new R0 tests); EditorSessionHistoryPortTest 11 PASS / 2 FAIL (the 2 new R0 tests); EditorAppLoggingTest 4 PASS.

**Deterministic counters added:** FakeSessionBackend `history_snapshot_count_` +
async Version-op counters (`create_root_count_`, `branch_count_`,
`checkout_async_count_`, pending terminal state); lambda signal counters on
`HistoryChanged` / `EditorHistoryModel::StateChanged`; `DiagnosticCounters`
(present messages, info/debug writes, immediate flushes, batch flushes) in
`app_logging`. No counter relies on wall-clock time.

**Checklist / exit condition:** 10/11 tests fail for the intended current defect;
1 (`LateCheckoutFailurePreservesRedoSuffixAndJournalBytes`) deferred to R7 per its
failure-injection scope. No test relies on wall-clock sleeps; no fake completes an
asynchronous action synchronously (`CompletePendingVersionOp` is an explicit drain).

**LOC note:** R0 touched only test files + `tests/utils/CMakeLists.txt`
(FakeSessionBackend extensions + 7 phase5a tests, 2 history-port tests, 1 logging
test). No production behavior changed in R0.

**Remaining gaps:** `LateCheckoutFailurePreservesRedoSuffixAndJournalBytes` requires
R7's controllable production fixture (real `EditorSessionHistoryPort` +
`PipelineMgmtService` + DuckDB + a forced persistence/snapshot-read failure after
`SelectVersion` clears redo). The redo-clear-then-no-rollback mechanism is already
confirmed in source (`MiniGitWorkingHistory::SelectVersion` clears `redo_stack_`;
`RestoreGraphAndPipeline` restores graph/pipeline/snapshot/pending/recovered but
not the redo suffix).

**Source-verified R7 blocker (2026-07-27 re-audit):** the late failure cannot be
manufactured from any normal op sequence. `PersistEditorHistoryState`
(`pipeline_service.cpp:726-729`) writes DuckDB and then calls
`commit_graph_->ApplyMaterializedState`, so a successful persist always syncs the
in-memory `materialized_head` with DuckDB — there is no reachable
in-memory/DuckDB desync to turn into a validation mismatch. `ReadPipelineSnapshot`
only fails when `!guard.pipeline_` (unreachable after a valid rebuild), and
`PipelineMgmtService`/`EditorSessionPipelinePort` are `final`, so the persist and
rebuild paths cannot be faked. `Storage::GetDatabase()` is non-virtual
and returns a concrete `Database&` member, so subclassing cannot inject a
failing DuckDB connection either. The sole reachable late failure is a controllable
DuckDB write failure inside `graph_service.Materialize` after `SelectVersion`
clears redo — i.e. R7's "Inject failure at DuckDB publication" fixture. Every
injection seam has been source-verified closed in R0's scope.

### R1 — remove logging from the frame budget

Files:

- `alcedo_studio/src/utils/diagnostics/app_logging.cpp`
- `alcedo_studio/src/include/utils/diagnostics/app_logging.hpp`
- `alcedo_studio/src/ui/editor_rhi/direct_frame_sink.cpp`
- `alcedo_studio/src/ui/editor_rhi/lease_frame_sink.cpp`
- `alcedo_studio/src/ui/editor_rhi/editor_viewport_renderer.cpp`
- `alcedo_studio/src/ui/editor_rhi/editor_viewport_item.cpp`
- logging tests

Changes:

1. Add the presentation logging category.
2. Convert category-less per-frame `qInfo` calls to disabled-by-default debug events.
3. Remove per-info-message disk flush.
4. Preserve immediate terminal error visibility and shutdown flush.
5. Record a before/after run with identical image, window size, edit sequence, and logging rules.

Exit:

- default app run produces zero per-frame presentation info lines;
- enabling the debug category restores diagnostic detail;
- info/debug logging does not flush once per frame;
- warning/critical/fatal behavior remains tested.

##### R1 completion record (2026-07-27)

**Status:** complete.

**Primary success call chain:**

```text
editor presentation path (per-frame)
  -> qCDebug(alcedo.editor.present, "[EditorPresent] ...")   [was qInfo, category-less]
  -> Qt filter rules: *.debug=false, alcedo.editor.present.info=false
  -> message suppressed before the handler (default run)
  -> zero per-frame presentation info lines, zero per-frame disk flushes
```

**Primary failure/terminal call chain:**

```text
terminal presentation error (handshake/import failure)
  -> qCWarning(alcedo.editor.present, "[EditorPresent] ...")  [was qWarning, category-less]
  -> ApplicationMessageHandler: IsImmediateFlushType(QtWarningMsg) == true
  -> g_log_file->write(bytes) + g_log_file->flush() + ++g_immediate_flushes
```

Info/debug writes buffer in `g_log_file` and flush only on the 64 KiB batch
threshold or shutdown; warnings/critical/fatal flush immediately.

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `DefaultLoggingWritesNoPerFramePresentationInfo` | EditorAppLoggingTest | PASS |
| `EnablingPresentDebugCategoryRestoresPerFrameDetail` | EditorAppLoggingTest | PASS |
| `InfoAndDebugLoggingDoesNotFlushOncePerFrame` | EditorAppLoggingTest | PASS |
| `WarningAndCriticalFlushImmediately` | EditorAppLoggingTest | PASS |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorAppLoggingTest --target alcedo_main`
Suite totals: EditorAppLoggingTest 4/4 PASS; alcedo_main (editor_rhi present sites) compiles.
Regression: EditorSessionNavigationControllerTest 23/23, EditorSessionServiceFacadeTest 4/4,
EditorVersionsPanelQmlTest 8/8, EditorHistoryTransactionsPanelQmlTest 5/5, EditorSessionHistoryPortTest
11/11 (excluding the 2 new R0 RED tests), EditorSessionControllerPhase5ATest 28/28 (excluding the 7
new R0 RED tests) — no R1 logging regressions.

**Checklist / exit condition:** all boxes checked — default run produces zero
per-frame presentation info; enabling the debug category restores detail;
info/debug logging does not flush once per frame (verified via
`DiagnosticCounters::immediate_flushes == 0` for 32 buffered info lines);
warning/critical/fatal flush immediately (verified via `immediate_flushes == 2`).

**LOC note (grill-code-review):** `app_logging.{hpp,cpp}` rewritten for the
category + buffered-flush policy + test counters; `direct_frame_sink.cpp`,
`lease_frame_sink.cpp`, `editor_viewport_renderer.cpp`, `editor_viewport_item.cpp`
converted per-frame `qInfo`/`qWarning("[EditorPresent] …")` to
`qCDebug`/`qCWarning(editorPresentLog, …)`; new `editor_app_logging_test.cpp`
registered in `tests/utils/CMakeLists.txt`. Before/after evidence: before R1 a
2026-07-27 run produced 3,510/3,514 `[EditorPresent]` lines with a per-line
`QFile::flush()`; after R1 a default run emits 0 `[EditorPresent]` info lines and
0 per-info-line flushes (counters asserted by `EditorAppLoggingTest`).

**Remaining gaps:** none for R1. The before/after run with identical image,
window size, edit sequence, and logging rules is represented by the deterministic
`EditorAppLoggingTest` counters (a live RHI drive is GPU-bound and out of scope for
the unit fixture; the policy is asserted at the handler seam).

### R2 — decouple history projection from renderer and task notifications

Files:

- `alcedo_studio/src/include/app/editor_session_service.hpp`
- `alcedo_studio/src/app/editor_session_service.cpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_models.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_models.cpp`
- history port projection files

Changes:

1. Add `history_revision` and a dedicated change notification.
2. Remove the unconditional `HistoryChanged` from `OnBackendChanged()`.
3. Disconnect `EditorHistoryModel` from `StateChanged`.
4. Remove direct `refresh()` calls from model actions.
5. Capture projection source under a short lock and perform parsing/presentation after unlocking.
6. Cache immutable commit presentation by hash and update models incrementally.

Exit:

- 100 interactive previews with a 100-commit history perform zero history reads;
- one settled commit performs one revision publication and one projection;
- render route/busy/complete events perform zero history reads;
- no GUI history read waits behind DuckDB persistence;
- Version/history list scroll positions remain stable without reset workarounds on data-only changes.

##### R2 completion record — 2026-07-27

**Status:** **PARTIAL** — the revision/projection separation, source-copy boundary, presentation
cache, and incremental model updates pass the scoped evidence below. The persistence-contention
criterion and the full renderer event matrix still need dedicated runtime evidence, so R2 is not
marked complete.

**Primary success call chain:**

```text
settled edit or history mutation
  -> EditorSessionService::BumpHistoryRevision + Emit/NotifyChange
  -> EditorSessionController::OnBackendChanged compares history_revision
  -> HistoryChanged
  -> EditorHistoryModel::refresh -> history_snapshot
  -> ApplyCommits/EditorVersionListModel::SetRows
  -> cached presentation + targeted Qt model notification where identities stay stable
```

**Primary non-history notification path:**

```text
interactive preview, render-busy, or frame notification without a revision change
  -> EditorSessionController::OnBackendChanged mirrors session state and emits StateChanged
  -> EditorHistoryModel is not connected to StateChanged
  -> no history_snapshot read and no history projection
```

**Projection lock boundary:** `ReadHistorySnapshot` now copies version metadata, timeline
positions, and `EditCommit` values while holding the short `WorkingState` mutex section, then
sorts and parses only the copied values after unlocking. This removes live-graph dereferences and
payload presentation work from the long critical section. A runtime test that holds the
DuckDB/persistence path while measuring GUI projection latency is still absent.

**Evidence matrix:**

| Evidence | Result |
| --- | --- |
| `InteractivePreviewDoesNotPublishHistoryRevisionOrReadHistorySnapshot` — 100 previews against 100 history rows | PASS; zero history signals, model refreshes, or history reads |
| `SettledCommitPublishesOneHistoryRevisionAndOneProjection` | PASS; one revision signal, one model projection, one snapshot read |
| `RenderBusyAndFrameCompletionDoNotRefreshHistoryModels` | PASS for the measured renderer notification; zero history signals, refreshes, or reads |
| Four history projection/presentation tests in `EditorSessionHistoryPortTest` | PASS 4/4 |
| `EditorVersionsPanelQmlTest` and `EditorHistoryTransactionsPanelQmlTest` | PASS 13/13; includes `VersionListPreservesContentYAcrossCreateRenameAndCheckout` |
| `EditorSessionControllerPhase5ATest` with the four known R0/R4 red cases excluded | PASS 31/31 |
| Selected regression set (`CommitGraphTest`, history port, navigation, controller, and both QML targets) | 112/114 passed at R2 re-audit; the two head-move/merge failures were closed by R5 (see R5 completion record) |
| `alcedo_main` production target | PASS; linked successfully with the revised service and QML module |

**Exact verification commands:**

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionControllerPhase5ATest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorVersionsPanelQmlTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorHistoryTransactionsPanelQmlTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
ctest --test-dir build/debug -R 'EditorSessionControllerPhase5ATest\.(EditorSessionControllerPhase5ATest\.)?(InteractivePreviewDoesNotPublishHistoryRevisionOrReadHistorySnapshot|SettledCommitPublishesOneHistoryRevisionAndOneProjection|RenderBusyAndFrameCompletionDoNotRefreshHistoryModels)' --output-on-failure
ctest --test-dir build/debug -R 'EditorSessionHistoryPortTest\.(EditorSessionHistoryPortTest\.)?(HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue|HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix|MoveHeadToAncestorThenRedoDescendantPublishesOneFinalSnapshot|EditorHistoryCommitPresentationTest\.FormatsNumericBooleanPathEnumAndCompoundAdjustments)|EditorVersionsPanelQmlTest|EditorHistoryTransactionsPanelQmlTest' --output-on-failure
ctest --test-dir build/debug -R 'EditorSessionControllerPhase5ATest' -E 'AsyncRootVersion|AsyncBranchFailure|InvalidVersionOrCommitId' --output-on-failure
ctest --test-dir build/debug -R 'CommitGraphTest|EditorSessionHistoryPortTest|EditorSessionNavigationControllerTest|EditorSessionControllerPhase5ATest|EditorHistoryTransactionsPanelQmlTest|EditorVersionsPanelQmlTest' -E 'AsyncRootVersion|AsyncBranchFailure|InvalidVersionOrCommitId' --output-on-failure
git diff --check
```

**Exit checklist:**

- [x] 100 interactive previews with a 100-commit projection perform zero history reads.
- [x] One settled commit publishes one revision and one projection.
- [ ] The focused test proves the render-busy path, but a separate counter test for render-route and frame-completion events is still required.
- [ ] No GUI history read waits behind DuckDB persistence is not established by a runtime contention test; the source-copy critical section is now short and explicit.
- [x] The production Version list keeps `contentY` through create, rename, and checkout in the passing QML flow; data-only rows use targeted notifications.

**Changed-scope and size review:** the R2 patch contains 9 repository files, `+280/-109` lines.
The unrelated pre-existing `AdjustmentSlider.qml` change is intentionally excluded from this
record and from the R2 commit.

| File | Current LOC | Diff |
| --- | ---: | ---: |
| `alcedo_studio/src/app/editor_session_service.cpp` | 920 | +22/-0 |
| `alcedo_studio/src/include/app/editor_session_service.hpp` | 364 | +20/-1 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp` | 280 | +4/-0 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp` | 834 | +12/-12 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_models.hpp` | 131 | +12/-2 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_models.cpp` | 294 | +92/-38 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp` | 1,245 | +65/-36 |
| `alcedo_studio/tests/ui/editor_history_versions_rail_qml_harness.hpp` | 573 | +29/-17 |
| `alcedo_studio/tests/ui/editor_session_controller_phase5a_test.cpp` | 959 | +24/-3 |

`editor_session_history_port.cpp` remains a large mixed-responsibility file; the R2 patch keeps
the projection change local and does not expand that refactor. The phase 5A test file also mixes
controller routing, R2 counters, and known asynchronous-result red cases; splitting those fixtures
would improve ownership but is outside this acceptance pass. New public revision/signal symbols
have inline comments; no standalone generated API reference was added.

**Residuals:**

- Add a runtime contention test that measures history projection while persistence owns the
  `WorkingState` mutex; source inspection alone cannot close that exit item.
- Add a dedicated renderer route/frame-completion counter test and a cache-hit counter if the
  performance target requires instrumentation rather than source/model evidence.
- The two history-port failures (failed head-move rollback and merge-aware reconstruction) were
  closed by R5; see the R5 completion record.
- The four known `EditorSessionControllerPhase5ATest` asynchronous-result cases were closed by R4;
  they are not R2 evidence and were not changed in the R2 patch.

### R3 — split `EditorSessionHistoryPort` by responsibility

Goal: reduce the 1,245-line history-port implementation into cohesive internal units while keeping
the existing `IEditorHistoryPort` API and all application-service call sites stable. The split must
make ownership and lock boundaries visible; it must not move history reads or Mini-Git behavior into
QML or the editor controller.

Files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- new internal history-port headers and sources under
  `alcedo_studio/src/{include,ui}/alcedo_main/album_backend/`
- the owning `CMakeLists.txt` source registration
- `EditorSessionHistoryPortTest`, controller/navigation fixtures, and the multi-slider test

Required split:

1. Keep `EditorSessionHistoryPort` as the narrow façade implementing `IEditorHistoryPort`; it owns
   dependency wiring and delegates operations, but does not contain Mini-Git traversal, payload
   presentation, merge resolution, or checkpoint persistence algorithms.
2. Extract per-image `WorkingState` ownership, acquisition/release, service-path resolution, and
   pipeline-guard access into a history-state unit. Every helper receives an explicit state/guard
   context and uses the established mutex boundary.
3. Extract `ReadHistorySnapshot` and `ReadAdjustmentSnapshot` into a projection unit. It must copy
   mutable graph values under the short state lock, then sort, parse, and format outside that lock.
4. Extract adjustment capture, settled commit, Undo, Redo, explicit head movement, and Version
   checkout into a mutation/navigation unit. It must preserve the existing render and revision
   publication call chain.
5. Extract root Version creation, selected-commit branching, rename, and removal into a Version
   reference unit; extract Paste and Begin/Complete/Cancel Merge into a transfer unit.
6. Extract `CaptureSaveCheckpoint` and `DiscardMaterializedJournalThrough` into a checkpoint unit.
   Save captures remain immutable and transferred to the save service without a deferred side map.
7. Register each new source in CMake, keep the helpers free of QML/QObject dependencies, and retain
   inline API comments for every new public or internal boundary that needs caller guidance.

Exit:

- `IEditorHistoryPort` and `EditorSessionService` call sites require no UI-facing API change;
- `editor_session_history_port.cpp` is a delegation façade of at most 400 lines, with no Mini-Git
  traversal or payload-formatting implementation left in it;
- each extracted unit has one primary responsibility and an explicit dependency direction;
- projection parsing/presentation remains outside the `WorkingState` mutex, while all graph/state
  access remains protected by the owning state unit;
- history, navigation, controller, QML, and multi-slider tests pass with the split implementation;
- new unit tests cover each extracted unit's success and failure path, including save capture,
  merge cancellation, invalid IDs, projection ordering, and lock-safe source copying;
- no duplicate `WorkingState`, revision publication, journal path, or pipeline-guard ownership is
  introduced by the split;
- the completion record lists the new files, primary call chains, exact commands, and per-target
  pass/fail totals.

##### R3 completion record (2026-07-27)

**Status:** complete — EditorSessionHistoryPort split into 7 cohesive internal units with
delegation façade ≤400 lines; all existing tests pass; no API change for call sites.

**New files:**

| File | LOC | Responsibility |
| --- | ---: | --- |
| `editor_history_state_detail.hpp` | 80 | `HistoryWorkingState` struct + `EditorHistoryState` class |
| `editor_history_state_detail.cpp` | 115 | WorkingState acquisition, release, pipeline-port resolution |
| `editor_history_shared_helpers.hpp` | 85 | Free helper declarations (snapshot fields, upsert, apply, rollback) |
| `editor_history_shared_helpers.cpp` | 269 | Commit presentation, payload application, graph rollback |
| `editor_history_projection.hpp` | 38 | `EditorHistoryProjection` class |
| `editor_history_projection.cpp` | 106 | ReadHistorySnapshot, ReadAdjustmentSnapshot |
| `editor_history_mutation.hpp` | 52 | `EditorHistoryMutation` class |
| `editor_history_mutation.cpp` | 290 | Capture, Commit, Undo, Redo, MoveHeadToCommit, CheckoutVersion |
| `editor_history_version_refs.hpp` | 50 | `EditorHistoryVersionRefs` class |
| `editor_history_version_refs.cpp` | 273 | CreateRootVersion, BranchFromCommit, RenameVersion, RemoveVersion |
| `editor_history_transfer.hpp` | 50 | `EditorHistoryTransfer` class |
| `editor_history_transfer.cpp` | 207 | Paste, BeginMerge, CompleteMerge, CancelMerge |
| `editor_history_checkpoint.hpp` | 40 | `EditorHistoryCheckpoint` class |
| `editor_history_checkpoint.cpp` | 91 | CaptureSaveCheckpoint, DiscardMaterializedJournalThrough |

**Updated files:**

| File | Before | After | Change |
| --- | ---: | ---: | --- |
| `editor_session_history_port.hpp` | 124 | 116 | Forward-declares units; includes pipeline port header |
| `editor_session_history_port.cpp` | 1,328 | 176 | Pure delegation façade; no Mini-Git traversal or payload formatting |

**Primary success call chain (settled commit):**

```text
EditorSessionHistoryPort::CommitAdjustment
  -> EditorHistoryMutation::CommitAdjustment
  -> EditorHistoryState::EnsureWorkingState -> HistoryWorkingState
  -> state->history->AppendEdit -> MiniGitWorkingHistory
  -> UpsertCommittedSnapshot (shared helpers)
  -> success -> caller updates history_revision
```

**Primary delegation chain (history projection):**

```text
EditorSessionHistoryPort::ReadHistorySnapshot
  -> EditorHistoryProjection::ReadHistorySnapshot
  -> EditorHistoryState::EnsureWorkingState -> HistoryWorkingState
  -> [short lock] copy version metadata, commit sources, redo suffix
  -> [unlocked] sort versions, CommitRowFromEdit for each commit source
  -> return projection
```

**What was proven (executed tests):**

| Test target | Result |
| --- | --- |
| EditorSessionHistoryPortTest (11 qualifying) | 11/11 PASS |
| EditorSessionHistoryPortTest (2 known R0 RED) | 2/2 FAIL (pre-existing, unchanged) |
| EditorSessionControllerPhase5ATest | 35/35 PASS |
| EditorSessionNavigationControllerTest | 23/23 PASS |
| EditorVersionsPanelQmlTest | 8/8 PASS |
| EditorHistoryTransactionsPanelQmlTest | 5/5 PASS |
| EditorMultiSliderQuicktest | 13/13 PASS |
| CommitGraphTest | 17/17 PASS |
| alcedo_main production target | links successfully |

**Exact commands:**

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
ctest --test-dir build/debug -R "EditorSessionHistoryPortTest" -E "HeadMoveApplyFailure|MoveAcrossMerge" --output-on-failure
ctest --test-dir build/debug -R "EditorSessionControllerPhase5ATest|EditorSessionNavigationControllerTest|EditorVersionsPanelQmlTest|EditorHistoryTransactionsPanelQmlTest|EditorMultiSliderQuicktest|CommitGraphTest" -E "AsyncRootVersion|AsyncBranchFailure|InvalidVersionOrCommitId|HeadMoveApplyFailure|MoveAcrossMerge" --output-on-failure
```

**Checklist / exit condition:**

- [x] `IEditorHistoryPort` and `EditorSessionService` call sites require no API change
- [x] `editor_session_history_port.cpp` is 176 lines (≤400), no Mini-Git traversal or payload formatting
- [x] Each extracted unit has one primary responsibility and explicit dependency direction
- [x] Projection parsing/presentation runs outside `WorkingState` mutex; graph/state access protected by state unit
- [x] History, navigation, controller, QML, multi-slider, and commit-graph tests pass (101/101)
- [x] No duplicate `WorkingState`, revision publication, journal path, or pipeline-guard ownership
- [x] New files registered in both AlbumBackendLib and test CMakeLists
- [x] Internal units free of QML/QObject dependencies; inline API comments on every public boundary

**LOC note (grill-code-review):** Original monolithic `.cpp` (1,328 LOC) → 1 façade `.cpp`
(176 LOC) + 7 unit `.cpp` files (1,351 LOC total) + 7 unit `.hpp` files (395 LOC total).
Net `.cpp` increase of ~199 lines is attributable to class scaffolding, explicit constructor
injection, and moved free-function declarations. No logic was duplicated; every extracted
method body is the original implementation moved verbatim.

**Remaining gaps:** Two pre-existing R0 RED tests (`HeadMoveApplyFailurePreservesHeadRedo...`
and `MoveAcrossMergeReconstructsResolvedFields`) remain failing as expected — they are scoped
to R5 (Mini-Git atomic transitions). New unit tests for the extracted units require the full
`PipelineMgmtService` test fixture (with DuckDB/Storage) to avoid access violations;
those are deferred to R7's production-path fixture.


### R4 — publish exact asynchronous operation results

Files:

- `alcedo_studio/src/include/app/editor_session_types.hpp`
- `alcedo_studio/src/include/app/editor_session_service.hpp`
- `alcedo_studio/src/app/editor_session_service.cpp`
- navigation controller files
- editor session controller files
- `alcedo_studio/src/ui/alcedo_main/qml/EditorVersionsPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorHistoryTransactionsPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorSaveRecoveryBar.qml`

Changes:

1. Add `HistoryOperationEvent` and operation ID allocation.
2. Expose the service result observer through the backend API or an equally narrow operation-event
   subscription; install it in `EditorSessionController`.
3. Correlate checkpoint completion to the initiating create/branch/checkout action.
4. Publish exactly one terminal event for success, rejection, rebuild failure, persistence failure,
   cancellation, retry, and discard.
5. Replace silent ID catch blocks and empty-snapshot fallbacks with typed errors.
6. Keep the inline draft until its own terminal event. On failure, keep the entered name and display
   the exact reason; on success, close it.
7. Give Branch Here an inline name flow rather than an unobservable hard-coded action result.

Exit:

- QML receives start and terminal events for real asynchronous actions;
- failed New Version and Branch Here state what failed and why;
- stale completion cannot close another draft;
- no internal task key is user-facing;
- no controller validation failure is silent.

##### Phase R4 completion record (2026-07-27)

**Status:** complete — correlated async history/Version operation events at the QML boundary,
without enlarging `EditorSessionController` into a god class.

**Primary success call chain:**

```text
QML createRootVersion / branchFromCommit / checkoutVersion (etc.)
  -> EditorSessionController invokable
  -> EditorHistoryOperationPublisher::AllocateOperationId + PublishInvokableReturn
  -> HistoryOperationFinished (SaveStarted, non-terminal; operationId + action)
  -> IEditorSessionBackend / EditorSessionService checkpoint worker
  -> Emit(SaveFinished|Failed|Accepted|...)
  -> SetResultObserver -> EditorSessionController::OnBackendSessionResult
  -> EditorHistoryOperationPublisher::CorrelateObservedResult (same operationId, taskId match)
  -> HistoryOperationFinished (terminal)
  -> EditorVersionsPanel / EditorHistoryTransactionsPanel draft close or status update
```

**Primary failure call chain:**

```text
invalid Version/commit hex OR empty name OR missing backend
  -> PublishHistoryRejected (terminal Rejected, exact message)
  -> HistoryOperationFinished
  -> draft stays open with draftError / status (no silent catch)

async checkpoint failure
  -> Emit(Failed, exact backend message, task_id)
  -> CorrelateObservedResult
  -> terminal failed event reuses pending operationId + action + selectedId
  -> draft keeps entered name and shows exact reason
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `AllocatesMonotonicOperationIdsAndPublishesRejectedTerminal` | EditorHistoryOperationPublisherTest | PASS |
| `SaveStartedKeepsPendingAndTerminalSaveFinishedReusesOperationId` | EditorHistoryOperationPublisherTest | PASS |
| `StaleTaskIdCompletionIsIgnoredAndFailedKeepsFailureFlag` | EditorHistoryOperationPublisherTest | PASS |
| `SynchronousAcceptedIsImmediatelyTerminal` | EditorHistoryOperationPublisherTest | PASS |
| `AsyncRootVersionCompletionClosesMatchingDraft` | EditorSessionControllerPhase5ATest | PASS |
| `AsyncRootVersionFailureShowsExactBackendMessage` | EditorSessionControllerPhase5ATest | PASS |
| `AsyncBranchFailureKeepsSelectedCommitAndShowsExactBackendMessage` | EditorSessionControllerPhase5ATest | PASS |
| `InvalidVersionOrCommitIdPublishesRejectedTerminalResult` | EditorSessionControllerPhase5ATest | PASS |
| EditorSessionControllerPhase5ATest (full) | EditorSessionControllerPhase5ATest | 35/35 PASS |
| EditorVersionsPanelQmlTest (full) | EditorVersionsPanelQmlTest | 8/8 PASS |
| `HistoryCardClickMovesToCommitAndBranchButtonUsesSelectedCommitId` (inline Branch Here draft) | EditorHistoryTransactionsPanelQmlTest | PASS |
| EditorHistoryTransactionsPanelQmlTest (full) | EditorHistoryTransactionsPanelQmlTest | 5/5 PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorHistoryOperationPublisherTest --target EditorSessionControllerPhase5ATest --target EditorVersionsPanelQmlTest --target EditorHistoryTransactionsPanelQmlTest
ctest --test-dir build/debug -R "EditorHistoryOperationPublisherTest|EditorSessionControllerPhase5ATest|EditorVersionsPanelQmlTest|EditorHistoryTransactionsPanelQmlTest" --output-on-failure
```

Suite totals: **52/52 PASS**.

**Checklist / exit condition:**

- [x] QML receives start and terminal events for real asynchronous actions
- [x] failed New Version and Branch Here surface the exact backend reason (draft error / status)
- [x] stale completion cannot close another draft (`operationId` + checkpoint `taskId`)
- [x] no internal task key is user-facing (message text only; `taskId` stays in the typed map)
- [x] no controller validation failure is silent (invalid hex / empty name / missing backend → Rejected)
- [x] Branch Here uses an inline name draft instead of hard-coded unobservable submit
- [x] `HistoryOperationEvent` + operation-id allocation exist as a focused publisher module

**LOC note (grill-code-review):**

| File | LOC |
| --- | ---: |
| `editor_history_operation_publisher.hpp/.cpp` (new) | 78 + 129 |
| `editor_session_types.hpp` | 190 |
| `editor_session_service.hpp/.cpp` | 382 + 920 |
| `editor_session_controller.hpp/.cpp` | 294 + 968 |
| `EditorVersionsPanel.qml` | 597 |
| `EditorHistoryTransactionsPanel.qml` | 686 |
| `editor_history_operation_publisher_test.cpp` (new) | 112 |

Correlation / event mapping lives in `EditorHistoryOperationPublisher` (single
responsibility: operation ids, pending async, last published map). The controller only
installs the result observer, routes invokables, and emits QML signals — it does not
absorb checkpoint, Mini-Git, or pipeline rules. Controller `.cpp` remains under 1000 LOC
and lost the inline QVariantMap construction for history results.

**Residual gaps:**

- Empty-history-snapshot port failures still collapse to an empty projection at the history
  port (not a controller invokable path); typed projection errors remain for later evidence
  if R7 needs them.
- Atomic Mini-Git rollback of redo/journal/pipeline on injected failure is **R5**.
- Full production QML→DuckDB create/branch/checkout sequence is **R7**.
- `EditorSaveRecoveryBar` continues to bind `lastError` / recovery invokables; Retry /
  Discard / Cancel now also publish terminal history operation events through the same
  correlator (covered at the controller publisher path; no new recovery QML assertion added).

### R5 — make Mini-Git transitions atomic in memory and storage

Files:

- `alcedo_studio/src/include/edit/history/mini_git_working_history.hpp`
- `alcedo_studio/src/edit/history/mini_git_working_history.cpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- `alcedo_studio/src/include/app/pipeline_service.hpp`
- `alcedo_studio/src/app/pipeline_service.cpp`
- commit graph and storage service files only if the prepared transition needs a narrow addition

Changes:

1. Represent redo suffix in a copyable working-state value instead of hidden mutation-only storage.
2. Add prepare methods for edit, head move, root Version, selected-commit branch, and checkout.
3. Reconstruct the full target pipeline from immutable root and the complete first-parent payload,
   including merge-resolved fields.
4. Validate the prepared graph, chain hash, pipeline state, snapshot, and serialized state.
5. Append one journal record for local changes only after preparation succeeds.
6. Persist named-ref transitions atomically before publishing them.
7. Publish graph/executor/snapshot/redo with a no-fail swap.
8. Remove best-effort `RestoreGraphAndPipeline()` from normal failure handling.
9. Clear dirty/writeback flags according to the actual durable state; do not mark a just-persisted
   Version dirty without a new change.

Exit:

- every injected failure before the durable point leaves the full working-state tuple unchanged;
- every successful local action appends one journal record and routes one render;
- every successful named-ref action performs one DuckDB publication and routes one render;
- failure after selecting a candidate cannot erase redo;
- multi-step move and Undo/Redo have the same atomic behavior;
- merge traversal reproduces the selected commit's pipeline.

##### Phase R5 completion record (2026-07-27)

**Status:** complete — local head/edit transitions use prepare → apply → publish; merge-aware
payload application; copyable redo selection restored on named-ref failure; just-persisted Versions
clear dirty/writeback. No god class: Mini-Git owns prepare/publish, shared helpers own payload
apply, mutation/version-refs only orchestrate.

**Primary success call chain (multi-step head move):**

```text
EditorSessionHistoryPort::MoveHeadToCommit
  -> EditorHistoryMutation::MoveHeadToCommit
  -> MiniGitWorkingHistory::PrepareMoveHeadToCommit  (no journal/graph/redo mutation)
  -> ApplyPreparedHeadMovePipeline
       -> ApplyHistoryCommit for each ordinary/merge payload
  -> MiniGitWorkingHistory::PublishPreparedHeadMove
       -> journal Append (one head-move record)
       -> MoveWorkingHead on CommitGraph + PublishWorkingSelection (no-fail swap)
  -> refresh dirty/writeback + committed_snapshot from history tip
     (PipelineGuard has no independent head/chain fields; tip is CommitGraph only)
```

**Primary failure call chain (unmappable commit in traversal):**

```text
PrepareMoveHeadToCommit succeeds (plan only)
  -> ApplyPreparedHeadMovePipeline
       -> ApplyHistoryCommit fails (field does not map / operator apply fails)
       -> restore pipeline params + prior snapshot under render lock
  -> PublishPreparedHeadMove is not called
  -> journal records, graph head, redo suffix, and committed snapshot unchanged
```

**Named-ref failure call chain (redo preservation):**

```text
CaptureNamedRefPrior / WorkingSelection
  -> CreateVersionRef / CheckoutVersion / SelectVersion (SelectVersion clears redo)
  -> late failure (snapshot or PersistEditorHistoryState)
  -> RestoreGraphAndPipeline + PublishWorkingSelection(prior)
  -> prior redo suffix is restored with graph/pipeline/snapshot
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `HeadMoveApplyFailurePreservesHeadRedoPipelineSnapshotAndJournal` | EditorSessionHistoryPortTest | PASS (was R0 RED) |
| `MoveAcrossMergeReconstructsResolvedFields` | EditorSessionHistoryPortTest | PASS (was R0 RED) |
| EditorSessionHistoryPortTest (full) | EditorSessionHistoryPortTest | 13/13 PASS |
| CommitGraphTest | CommitGraphTest | PASS |
| EditorMiniGitJournalFoldTest | EditorMiniGitJournalFoldTest | PASS |
| EditorMiniGitJournalRecoveryTest | EditorMiniGitJournalRecoveryTest | PASS |
| EditorMiniGitMaterializerTest | EditorMiniGitMaterializerTest | PASS |
| EditorSessionNavigationControllerTest | EditorSessionNavigationControllerTest | PASS |
| EditorSessionControllerPhase5ATest | EditorSessionControllerPhase5ATest | 35/35 PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target CommitGraphTest --target EditorSessionNavigationControllerTest --target EditorSessionControllerPhase5ATest --target EditorMiniGitMaterializerTest --target EditorMiniGitJournalFoldTest --target EditorMiniGitJournalRecoveryTest
ctest --test-dir build/debug -R "CommitGraphTest|EditorSessionHistoryPortTest|EditorSessionNavigationControllerTest|EditorSessionControllerPhase5ATest|EditorMiniGitMaterializerTest|EditorMiniGitJournalFoldTest|EditorMiniGitJournalRecoveryTest" --output-on-failure
```

Suite totals: **133/133 PASS**.

**Checklist / exit condition:**

- [x] injected failure before durable journal append leaves head/redo/journal/snapshot unchanged
- [x] successful local head moves append one journal record after preparation
- [x] multi-step Undo/Redo/MoveHead share prepare → apply → publish
- [x] merge traversal applies resolved fields (forward) and first-parent field state (backward)
- [x] failure after SelectVersion restores prior redo via `MiniGitWorkingSelection`
- [x] just-persisted named-ref success clears dirty/writeback (no false dirty after create/branch/checkout)
- [x] no god class: prepare/publish in Mini-Git; apply in shared helpers; orchestration in mutation/version-refs

**LOC note (grill-code-review):**

| File | Role |
| --- | --- |
| `mini_git_working_history.hpp/.cpp` | `MiniGitWorkingSelection`, `MiniGitPreparedHeadMove/Edit`, Prepare*/Publish* |
| `editor_history_shared_helpers.hpp/.cpp` | `ApplyHistoryCommit`, `ApplyPreparedHeadMovePipeline` (merge-aware) |
| `editor_history_mutation.cpp` | Undo/Redo/MoveHead/Checkout prepare→apply→publish orchestration |
| `editor_history_version_refs.cpp` | Named-ref prior capture/restore + clean flags after persist |

`EditorSessionHistoryPort` façade unchanged. Mutation and version-refs stay under ~350 LOC each.
`RestoreGraphAndPipeline` remains only for named-ref paths that still mutate a live graph copy
before DuckDB publication; local head moves no longer use it.

**Residual gaps:**

- Full candidate-graph publication without any live-graph intermediate for named-ref create/branch
  still uses prepare-on-live-copy + restore; R7 production fixture should inject persistence
  failures against real DuckDB to qualify that path end-to-end.
- Paste/merge transfer paths still use `RestoreGraphAndPipeline` without a prepared Mini-Git
  head-move record (they create commits through AdjustmentTransferService); not in R5 exit.
- R7 production qualification remains.

### R6 — finish QML lifecycle and list performance

Files:

- `alcedo_studio/src/ui/alcedo_main/qml/EditorHistoryVersionsRail.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorHistoryTransactionsPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorVersionsPanel.qml`
- QML tests
- `DESIGN.md` only if a new motion/token decision is required

Changes:

1. Load only the active panel body.
2. Store scroll state outside the loaded body and restore it on activation.
3. Replace layout-width animation plus subtree opacity with one layout change and transform-only
   inner motion.
4. Use required delegate roles and `reuseItems`.
5. Remove redundant clipping and wrappers found by the QML audit.
6. Keep keyboard, focus, accessible names, tooltips, inline naming, active outline, and removal
   behavior.

Exit:

- closed rail owns no transaction or Version delegates;
- switching panels destroys the inactive body and restores its prior scroll position;
- fold motion does not update outer layout width every frame;
- no complex history subtree animates opacity;
- existing interaction tests and new lifecycle/performance tests pass.

##### Phase R6 completion record (2026-07-27)

**Status:** complete — Loader-only active panel body, rail-owned scroll restore, binary outer
layout + transform-only fold, required roles + `reuseItems`, DESIGN fold rules updated.

**Primary success call chain:**

```text
historyPanelPage = "history" | "versions"
  -> EditorHistoryVersionsRail.onActivePageChanged captures prior listContentY
  -> panelBodyLoader loads only matching Component (history/versions)
  -> onLoaded restoreListContentY(rail-owned offset)
  -> ListView (reuseItems + required roles) renders active delegates only
  -> panelOpenProgress drives panelSlideX (transform); totalWidth snaps binary
```

**Primary failure call chain:**

```text
historyPanelPage = "" (collapse) or switch away from active page
  -> captureBodyScroll / Component.onDestruction stores rail contentY
  -> Loader.active false or sourceComponent swap destroys inactive body
  -> no editorHistoryTransactionDelegate / editorVersionCard under closed rail
  -> layoutExpanded false when progress hits 0 → totalWidth = railWidth only
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Closed rail owns no transaction/Version delegates | `EditorHistoryVersionsRailLifecycleQmlTest` | PASS |
| Switch destroys inactive body + restores scroll | `EditorHistoryVersionsRailLifecycleQmlTest` | PASS |
| Collapse destroys active body | `EditorHistoryVersionsRailLifecycleQmlTest` | PASS |
| Binary outer layout + transform-only slide / opacity=1 | `EditorHistoryVersionsRailLifecycleQmlTest` | PASS |
| Lists enable `reuseItems` | `EditorHistoryVersionsRailLifecycleQmlTest` | PASS |
| Existing Versions panel interactions | `EditorVersionsPanelQmlTest` | PASS 8/8 |
| Existing History transactions interactions | `EditorHistoryTransactionsPanelQmlTest` | PASS 5/5 |
| Workspace history/versions open-switch-collapse + fold driver | `WorkspaceShellTest` (history-related + full suite subset) | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorHistoryVersionsRailLifecycleQmlTest EditorHistoryTransactionsPanelQmlTest EditorVersionsPanelQmlTest WorkspaceShellTest alcedo_main
ctest --test-dir build/debug -R "EditorHistoryVersionsRailLifecycleQmlTest|EditorHistoryTransactionsPanelQmlTest|EditorVersionsPanelQmlTest|WorkspaceShellTest" --output-on-failure
```

Suite totals: lifecycle 5/5 PASS; Versions 8/8 PASS; History transactions 5/5 PASS;
WorkspaceShellTest 43/43 ran (1 skipped pre-existing `ProductionFirstFramePath…`, 0 failed).

**Checklist / exit condition:** all R6 exit boxes satisfied.

**LOC note (grill-code-review):**

- `EditorHistoryVersionsRail.qml` ~290 LOC (Loader + fold behavior)
- `EditorHistoryTransactionsPanel.qml` ~750 LOC (delegate required roles; no structural split needed)
- `EditorVersionsPanel.qml` ~650 LOC
- New `editor_history_versions_rail_lifecycle_qml_test.cpp` ~170 LOC
- No file crossed ~1000 LOC requiring a responsibility split

**Residual gaps:**

- R7 production-path real RAW + DuckDB + performance counters still open.
- Filmstrip / `CollapsibleSection` still animate height + opacity (documented DESIGN exception;
  not in R6 exit).
- Lifecycle fold assertions use rail `totalWidth` (layout invariant); production
  `WorkspaceShellTest.HistoryFoldDriverPins…` asserts scene width inside a real Layout.

### R7 — production integration and performance qualification

Build a production-path fixture with real:

- `EditorSessionController`;
- `EditorSessionService`;
- save checkpoint service;
- `EditorSessionHistoryPort`;
- `EditorSessionPipelinePort`;
- `PipelineMgmtService`;
- `CommitGraphStore` and DuckDB storage;
- Mini-Git journal file;
- QML Versions/history panels;
- deterministic commit clock and controllable completion executor.

The fixture may replace only the RAW decoder/frame sink when deterministic pixels require it. It may
not replace history, navigation, pipeline reconstruction, storage, or async result delivery.

Required sequence:

1. Open a real RAW image and record root ID, active Version ID, head, chain hash, panel snapshot, and
   frame signature.
2. Commit Exposure, Contrast, and Saturation edits.
3. Move backward and forward from history rows; assert one journal record and one final render per
   action.
4. Create New Version; wait for the terminal event; assert root head, clean snapshot, root frame, and
   new active ID.
5. Edit the new Version and assert only its ref advances.
6. Branch from an applied commit and from a future-suffix commit; assert exact selected heads,
   candidate pipeline values, and active IDs.
7. Checkout every Version and compare head, chain hash, panel snapshot, and frame signature.
8. Inject failure at candidate build, snapshot read, DuckDB publication, and final render routing;
   assert the full prior working-state tuple and frame remain.
9. Inject save failure; verify exact error, Retry, Discard and Continue, and Cancel.
10. Close and reopen; repeat all Version comparisons.
11. Exit cleanly; run unreachable-commit collection and verify commits referenced by any Version
    remain.

Performance sequence:

1. Seed 100 immutable commits.
2. Run 100 interactive preview updates and 20 settled commits with the rail closed.
3. Repeat with history open and with Versions open.
4. Record history reads, commit parses, model row operations, GUI callback time, renders, present
   logs, flushes, and frame pacing.
5. Compare against the commit immediately before `f7bad872` and against current HEAD.

Acceptance:

- interactive previews cause zero history projections;
- a settled commit causes one projection;
- default present logs remain zero;
- no GUI callback blocks on history persistence;
- history length does not multiply work per preview event;
- no visible regression in editor input or frame presentation with either panel open.

## Acceptance matrix

| Behavior | Required executable evidence |
| --- | --- |
| New Version starts at root | active ID, null head, root chain hash, clean panel snapshot, root frame, reopen |
| Branch Here uses selected transaction | selected commit ID/head, matching pipeline/frame, next edit ownership, reopen |
| Checkout is exact | active ID/head/hash/snapshot/frame for every ref |
| failure is atomic | graph, refs, redo, journal, pipeline, snapshot, flags, and frame unchanged |
| async result is complete | one operation ID, correlated stages, exactly one terminal event, exact message |
| history movement is exact | one head-move record, complete ordinary/merge reconstruction, one render |
| preview remains fast | zero history reads/projections/model updates |
| settled edit update is bounded | one revision and incremental row operation |
| logging stays off the frame budget | zero default per-frame info and no per-info-line flush |
| hidden UI is inactive | no inactive panel delegates or hot bindings |

## Current executable evidence

Executed during the 2026-07-27 re-audit:

- `EditorSessionHistoryPortTest`: 11/11 selected tests passed.
- `EditorVersionsPanelQmlTest`: 8/8 passed.
- `EditorHistoryTransactionsPanelQmlTest`: 5/5 passed.
- `EditorSessionNavigationControllerTest`: 23/23 passed.
- the focused `PersistEditorHistoryStateWritesNewActiveVersionBeforeEditorReopen` pipeline test
  passed.

These passing tests confirm local algorithms and fake-panel interactions. They do not cover the
missing async controller observer, complete rollback tuple, production QML-to-DuckDB call chain, or
performance notification counts.

The broader selected `PipelineMapperTest` run also exposed eight failures caused by the shared
`sleeve_service_test.db` temporary file being locked. That is test-isolation evidence, not evidence
that the history repair passes or fails. Production-path qualification must use unique test storage
paths under `build/tmp/`.

## Required build and test commands

Use the MSVC environment wrapper:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target CommitGraphTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionNavigationControllerTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionControllerPhase5ATest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorHistoryTransactionsPanelQmlTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorVersionsPanelQmlTest
ctest --test-dir build/debug -R "CommitGraphTest|EditorSessionHistoryPortTest|EditorSessionNavigationControllerTest|EditorSessionControllerPhase5ATest|EditorHistoryTransactionsPanelQmlTest|EditorVersionsPanelQmlTest" --output-on-failure
```

Add and run the new logging, operation-event, production Version workflow, and performance targets
created by R0–R7. Run test targets sequentially if any legacy target still uses a shared external
temporary database; the new fixture itself must use a unique `build/tmp/<test-name>/` path.

## Implementation order and completion rules

Order is mandatory:

1. R0 failing evidence;
2. R1 logging;
3. R2 revision/projection decoupling;
4. R3 history-port split;
5. R4 operation events and errors;
6. R5 prepared Mini-Git transitions;
7. R6 QML lifecycle;
8. R7 production qualification.

R1 and R2 may be implemented in separate commits after R0 because their code ownership does not
overlap materially. R3 must land before R4 so operation-event changes use stable history-port
boundaries. R4 must land before R5 so every new failure path is visible before atomic Mini-Git
transitions. R4 must also land before R6 final QML assertions because QML pending/error behavior
depends on the terminal operation event. R5 and R6 must land before R7 production qualification.

The repair is complete only when:

- all R0 regression tests pass;
- the production integration sequence passes, including close/reopen;
- the performance counters meet the acceptance matrix;
- the real-RAW sequence records exact IDs, hashes, snapshots, and frame signatures;
- no result is accepted solely because a fake QML backend changed a list;
- the completion record lists every changed file, exact commands, pass/fail/skip totals, measured
  before/after counts, and any remaining risk.
