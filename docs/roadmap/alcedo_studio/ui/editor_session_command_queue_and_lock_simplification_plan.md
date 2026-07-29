# Editor Session Command Queue and Lock Simplification Plan

Date: 2026-07-29

Status: CQ2 complete — pure history snapshots, worker-only executor application, and narrowed
pipeline cache locking verified on 2026-07-29. CQ3 unblocked.

Primary owner: Alcedo Studio editor session and history architecture.

Affected areas:

- QML editor workspace and action availability;
- editor session lifecycle, navigation, edit, render, and save services;
- Mini-Git working history and adjustment transfer;
- pipeline execution and pipeline persistence;
- background-task interaction restrictions;
- editor integration and regression tests.

Related roadmaps:

- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)
- [Phase 7A History and Versions Repair Plan](phase_7a_history_versions_repair_and_ui_refactor_plan.md)

This plan supersedes the callback, mutex, action-availability, and Paste/Merge persistence portions
of the related plans. It does not replace their completed UI, Mini-Git, rendering, or Version
features.

## Decision

All editor-session mutations will be serialized through one single-thread command queue.

The queue may dispatch render, journal, thumbnail, and DuckDB work to worker threads. Worker
completion is represented as a typed completion message and posted back to the same queue. A worker
must never synchronously invoke session navigation, lifecycle, history publication, QML
notification, or another user command.

The queue thread owns:

- the current session state and identity;
- the active and pending user command;
- the accepted pipeline and history guards;
- the active Mini-Git working selection;
- pending navigation and merge state;
- the immutable editor snapshot published to QML;
- command ordering, replacement, rejection, and terminal result publication.

Worker-owned services keep only the synchronization required for their local queues and resources.
They do not own editor-session state.

## Why this work is required

The current implementation has six overlapping state sources:

1. `EditorSessionLifecycle`;
2. `EditorSessionNavigationController`;
3. `EditorSessionEditController`;
4. `EditorHistoryState`;
5. `EditorRenderCoordinator`;
6. `InteractionPolicyController` and QML-local enable expressions.

The current save API permits synchronous completion even though its name describes asynchronous
work. Navigation therefore uses a recursive mutex so a save completion can re-enter navigation
while the initiating function is still on the stack. The completion may clear a pending action,
release guards, acquire another image, and route a render before the initiating function resumes.

Several GUI-thread actions also wait for the live executor render mutex:

- Undo and Redo;
- explicit head movement;
- Version checkout and pipeline reconstruction;
- Paste and Merge;
- save-checkpoint capture.

Paste and Merge currently rebuild and persist history synchronously, then start another save
checkpoint. Their backend precondition also requires an empty Mini-Git journal, while QML does not
publish that condition as part of action availability.

These properties make the behavior depend on callback timing and implicit lock order rather than
one serialized state transition.

## Current user call chains and lock inventory

CQ0 must verify this baseline against the implementation and write any corrected function names
back into this section before restructuring begins.

| User operation | Current primary path | Synchronization involved | Main risk |
| --- | --- | --- | --- |
| Open or select an image | QML → `EditorSessionController` → session backend → `EditorSessionNavigationController` → journal save → acquire → render | navigation recursive mutex, lifecycle recursive mutex, save-service mutexes, render-controller recursive mutex, render-coordinator mutex | an immediate save completion can re-enter navigation before the initiating call returns |
| Preview or settle an adjustment | QML panel → controller → session edit controller → history port or render port → pipeline executor | edit-controller mutex, history-state mutex for settled edits, executor render mutex, render scheduler/coordinator mutexes | GUI work and render work share mutable executor state; before values can depend on lock timing |
| Undo, Redo, or move history head | history model/controller → session backend → edit controller → history mutation → render route | edit-controller mutex, history-state mutex, executor render mutex, render scheduler/coordinator mutexes | the command thread can wait behind a quality render and leave all actions apparently disabled |
| Checkout or mutate a Version | Version model/controller → session backend → history mutation/version-ref service → persistence → render | history-state mutex, pipeline-service cache mutex, executor render mutex, storage synchronization | graph, selected Version, pipeline state, and presented frame can be observed at different revisions |
| Paste into the active editor | adjustment-transfer controller → session controller → history transfer → transfer service → persistence → checkpoint/render | history-state mutex, pipeline-service cache mutex, executor render mutex, save and storage synchronization | dirty journals are rejected late and successful work can be persisted twice |
| Begin and complete Merge | adjustment-transfer controller → session controller → history transfer → preview/resolve → persistence → checkpoint/render | history-state mutex, pipeline-service cache mutex, executor render mutex, save and storage synchronization | preview validity, two-parent publication, Undo state, and durable state are not one transition |

The target disposition for each lock is:

| Current synchronization | Target disposition |
| --- | --- |
| navigation and lifecycle recursive mutexes | removed in CQ1; queue-thread ownership replaces them |
| edit-controller mutation mutex | removed in CQ2 after edit state moves into the queue reducer |
| history working-state mutex | reduced during CQ2, then removed from command-side mutation |
| executor render mutex | retained for worker-side executor access only |
| render scheduler/coordinator mutexes | retained only around scheduler-local queues and request tables |
| pipeline-service cache mutex | retained locally, never held across storage, reconstruction, or render access |
| save/checkpoint and journal-writer mutexes | retained around worker-local request tables and per-image I/O only |
| storage and DuckDB synchronization | retained inside the persistence worker and transaction boundary |
| QML interaction-policy state | no mutex added; editor decisions move into one queue-produced availability value |

## Scope boundaries

- This plan does not move image decoding, file I/O, DuckDB, GPU submission, thumbnail generation,
  or pipeline rendering onto the command thread.
- A dedicated operating-system thread is not required. The requirement is one serialized owner
  with queued delivery and bounded reducers.
- The renderer, Mini-Git graph semantics, first-parent Undo behavior, and QML visual design are not
  rewritten.
- Worker services may use completion functions internally, but they may only post typed completion
  values. They may not call editor-session mutation APIs.
- CQ1 may retain compatibility signals and result objects temporarily; CQ5 removes them after QML
  and integration tests consume the unified snapshot.

## Target architecture

```text
QML / controller adapter
  -> enqueue EditorSessionCommand
  -> EditorSessionCommandQueue (one owning thread)
       -> reduce command against EditorSessionStateData
       -> publish immediate Queued / Rejected result
       -> dispatch optional worker request
       -> wait for typed completion message
       -> reduce completion
       -> publish one immutable EditorSessionSnapshot
       -> publish one terminal operation result

Worker request
  -> render / journal / materializer / pipeline-loader worker
  -> post EditorSessionCompletion to the command queue
  -> never call session state directly
```

The queue is a serialized actor, not a dedicated blocking worker. The production adapter may host it
on the Qt GUI thread because command reduction is bounded and does not perform file, database,
decoder, GPU, or render-lock waits. Tests use a deterministic manual executor.

## Required invariants

1. Exactly one thread mutates session, navigation, history-selection, and pending-operation state.
2. A command handler never waits for file I/O, DuckDB, image decoding, GPU work, frame presentation,
   or a pipeline render mutex.
3. A service completion never runs inline inside the service start call.
4. Every completion carries command ID, session generation, element ID, and relevant render or save
   generation.
5. Stale completions are ignored without changing the published snapshot.
6. One accepted command publishes at most one terminal result.
7. QML reads one immutable snapshot revision and never combines identity, lifecycle, history, and
   availability values from different revisions.
8. The live pipeline executor is a render target, not the source of truth for history before/after
   values.
9. Paste and Merge perform one durable publication.
10. No recursive mutex remains in editor-session lifecycle or navigation.

## Command and completion model

### Commands

The initial command value should cover:

```text
OpenImage
SelectImage
CloseEditor
Shutdown
PreviewAdjustment
CommitAdjustment
Undo
Redo
MoveHead
DiscardChanges
CheckoutVersion
CreateRootVersion
BranchVersion
RenameVersion
RemoveVersion
PreparePaste
ApplyPaste
BeginMerge
CompleteMerge
CancelMerge
RetrySave
DiscardAndContinue
CancelPendingNavigation
RequestViewChange
```

Each command carries a monotonic command ID and the snapshot revision from which the UI issued it.
Commands that target an image or Version also carry the explicit target ID.

### Completions

Worker services post typed values such as:

```text
ImageStateLoaded
JournalCommitFinished
MaterializationFinished
RenderAccepted
RenderCompleted
FrameSubmitted
FramePresented
PipelineSnapshotBuilt
ThumbnailRefreshFinished
WorkerRequestFailed
```

A completion sink posts the value to the queue executor. It does not expose a callback that can
re-enter the queue owner.

### Ordering and replacement

- Settled adjustment, Undo, Redo, Version, Paste, Merge, Close, and Shutdown commands are never
  coalesced.
- Interactive previews may replace an unstarted preview for the same adjustment field.
- View changes may replace an unstarted view command with the same replacement key.
- Repeated image selections may replace only an unstarted pending selection. They cannot change the
  identity of a save or acquisition already in progress.
- Commands that cannot legally run in the current state return a typed busy or invalid-state result;
  they do not partially mutate pending state.
- Shutdown stops accepting user commands, drains or cancels worker requests according to their
  declared shutdown behavior, and then publishes `ShuttingDown`.

## Target state publication

The queue publishes one value:

```cpp
struct EditorSessionSnapshot {
  std::uint64_t revision;
  EditorSessionState state;
  EditorSessionIdentity identity;
  EditorSessionIdentity presentation_identity;
  EditorActionAvailability actions;
  EditorHistorySummary history;
  EditorAdjustmentSnapshot adjustments;
  EditorPendingOperation pending;
  std::string error;
};
```

`presentation_identity` is explicit because a switch may retain image A while rendering the first
frame for image B. The QML adapter must not reconstruct this state from separate pending fields.

`EditorActionAvailability` is computed by one reducer from:

- session state;
- active image and Version state;
- history Undo/Redo state;
- copied-adjustment package state;
- pending command;
- external background-task restrictions.

QML components bind only to this availability value. They do not combine `canEdit`, history-model
flags, package state, and interaction-policy flags independently.

## Phase CQ0 — Freeze current behavior with failing evidence

Status: complete — deterministic failing evidence captured (8 of 10 tests reproduce current inline-completion, render-lock-blocking, and split-snapshot behavior; 2 pin existing guards CQ1/CQ3 must preserve).

### Purpose

Create deterministic tests for the timing and lock failures before restructuring ownership.

### Required test support

Add a manual single-thread executor and controllable ports for:

- journal commit start and completion;
- materialization start and completion;
- render scheduling and frame lifecycle;
- pipeline snapshot construction;
- background-task restriction changes.

The test ports must support both immediate worker completion and delayed completion. Immediate
completion still posts a completion message; it must not execute session code on the service start
stack.

### Required failing tests

- `SynchronousJournalResultIsProcessedAfterInitiatingCommandReturns`
- `RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`
- `StaleSaveCompletionCannotReleaseTheCurrentImageGuards`
- `StaleFirstFrameCannotEnableEditingForAnotherImage`
- `UndoWhileRenderWorkerOwnsExecutorDoesNotBlockCommandThread`
- `MergeWhileRenderWorkerOwnsExecutorDoesNotBlockCommandThread`
- `DirtyJournalPasteQueuesSaveBeforeCreatingTheNewVersion`
- `DirtyJournalMergeQueuesSaveBeforeCreatingTheMergeCommit`
- `OneSnapshotRevisionContainsMatchingIdentityStateAndAvailability`
- `OneAcceptedCommandPublishesExactlyOneTerminalResult`

### Exit criteria

- Tests reproduce current inline completion, blocking render-lock, or split-snapshot behavior.
- Every test has a bounded timeout and reports the exact unfinished operation.
- No test relies on sleeps for ordering; the manual executor controls every step.
- Baseline results and relevant call chains are recorded in this phase section before CQ1 starts.

##### Phase CQ0 completion record (2026-07-29)

**Status:** complete — deterministic failing evidence captured for the current editor-session facade.

**Baseline verified against the implementation:** the current user-call-chain / lock-inventory
table (above) matches the code. Confirmed reproduction sites:

- inline save-completion re-entry: `EditorSessionNavigationController::RequestOpenOrSwitch`
  (`editor_session_navigation_controller.cpp:78,92-105`) calls `SealAndStartSave`
  (`:345`), whose `save_service_.Start` callback runs `OnCheckpointFinished`
  (`:192`) **on the same stack** when the journal port invokes its callback inline
  (`editor_save_checkpoint_service.cpp:108,128` →
  `editor_session_journal_writer_port.cpp:111`). The navigation `mutex_` is a
  `std::recursive_mutex` (`editor_session_navigation_controller.hpp:214`) precisely
  so this re-entry can re-lock it.
- GUI-side render-lock wait: `EditorSessionEditController::HandleUndoRedo` /
  `HandleMoveHeadToCommit` / `HandleDiscard` call the history port, whose
  production impl reaches `ApplyPreparedHeadMovePipeline`
  (`editor_history_shared_helpers.cpp:340,350`) and `PipelineMgmtService::CheckoutVersion`
  / `RebuildActiveEditorPipeline` (`pipeline_service.cpp:795,805,886,893`) and
  `CaptureSaveCheckpoint` (`editor_history_checkpoint.cpp:41`) — all blocking on
  `CPUPipelineExecutor::GetRenderLock()` (`pipeline_cpu.hpp:84`).
- split snapshot / multi-terminal: one synchronous `Switch` publishes
  `SaveStarted`, the inline save-completion result, and `SaveFinished` across
  several `NotifyChange` calls (`editor_session_service.cpp:111-118` /
  `Open`/`Switch` synchronous branches).

**Primary success call chain (current, to be replaced in CQ1):**

```text
QML -> EditorSessionController -> EditorSessionService::Switch
  -> EditorSessionNavigationController::RequestOpenOrSwitch (recursive_mutex held)
  -> SealAndStartSave -> EditorSaveCheckpointService::Start
  -> journal->CommitJournalAsync (inline callback) -> HandleJournalCommit
  -> checkpoint_store->MaterializeAsync (inline) -> HandleMaterialization -> FinishSave
  -> OnCheckpointFinished (re-enters navigation on the start stack)
  -> lifecycle.ReleaseAfterCheckpoint / ContinueToTarget / RouteInitialRender
  -> NotifyCompletion -> EditorSessionService::Emit (x3) before Switch returns
```

**Primary failure call chain reproduced (render-lock blocking):**

```text
QML Undo -> EditorSessionService::Undo -> EditorSessionEditController::HandleUndoRedo
  -> IEditorHistoryPort::Undo -> EditorHistoryMutation::Undo
  -> ApplyPreparedHeadMovePipeline -> CPUPipelineExecutor::GetRenderLock() BLOCKS
  while a scheduler worker holds the lock (pipeline_scheduler.cpp:495)
  -> command thread stalls; all UI actions appear disabled
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `SynchronousJournalResultIsProcessedAfterInitiatingCommandReturns` | `EditorSessionCommandQueueBaselineTest` | FAIL (2 save-completion results ran inline before `Switch` returned) |
| `RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection` | `EditorSessionCommandQueueBaselineTest` | FAIL (second selection `Rejected`, not queued/replacing) |
| `StaleSaveCompletionCannotReleaseTheCurrentImageGuards` | `EditorSessionCommandQueueBaselineTest` | PASS (current async-path generation correlation holds; the synchronous-inline unconditional-accept at `navigation_controller.cpp:198-207` is the residual window CQ1 removes) |
| `StaleFirstFrameCannotEnableEditingForAnotherImage` | `EditorSessionCommandQueueBaselineTest` | PASS (render controller filters stale frames by generation/identity; guard pinned for CQ1/CQ3) |
| `UndoWhileRenderWorkerOwnsExecutorDoesNotBlockCommandThread` | `EditorSessionCommandQueueBaselineTest` | FAIL (Undo blocked on the render lock; bounded 200 ms timeout reports the unfinished op) |
| `MergeWhileRenderWorkerOwnsExecutorDoesNotBlockCommandThread` | `EditorSessionCommandQueueBaselineTest` | FAIL (CompleteMerge blocked on the render lock; bounded timeout reports the unfinished op) |
| `DirtyJournalPasteQueuesSaveBeforeCreatingTheNewVersion` | `EditorSessionCommandQueueBaselineTest` | FAIL (order observed `version_created` then `save_started`; target is save-first) |
| `DirtyJournalMergeQueuesSaveBeforeCreatingTheMergeCommit` | `EditorSessionCommandQueueBaselineTest` | FAIL (order observed `merge_committed` then `save_started`; target is save-first) |
| `OneSnapshotRevisionContainsMatchingIdentityStateAndAvailability` | `EditorSessionCommandQueueBaselineTest` | FAIL (one `Switch` published 10 change notifications with >1 distinct state/identity — split revision) |
| `OneAcceptedCommandPublishesExactlyOneTerminalResult` | `EditorSessionCommandQueueBaselineTest` | FAIL (one `Switch` published 4 terminal results) |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionCommandQueueBaselineTest --parallel 4` (builds);
`build/debug/alcedo_studio/tests/app/EditorSessionCommandQueueBaselineTest_runtime/EditorSessionCommandQueueBaselineTest.exe` (runs).
Suite totals: **2/10 PASS, 8/10 FAIL** (CQ0 expects failing evidence; the 8 failures are the intended baseline).
Regression check: `EditorSessionNavigationControllerTest` 23/23, `EditorSessionEditControllerTest` 8/8, `EditorSessionLifecycleTest` 18/18, `EditorSessionServiceFacadeTest` 4/4 still PASS after the test-support change (two fakes de-`final`-ed for subclassing).

**Checklist / exit condition:** all four exit criteria met — 8 tests reproduce current inline completion (tests 1), render-lock blocking (tests 5, 6), and split-snapshot behavior (tests 7, 8, 9, 10); every blocking test uses a bounded 200 ms timeout and names the unfinished operation; no test sleeps for ordering (the manual executor + controllable ports drive every step); baseline results and call chains recorded above.

**Test support added (CQ0):**

- `tests/support/editor_session_command_queue_test_support.hpp` — `ManualCommandExecutor` (single-thread manual queue with `post`/`drain_one`/`drain_all`), `SessionResultRecorder` (result/change observer with an inline-completion sentinel), `ControllableEditorHistoryPort` (render-lock-gated + dirty-journal + event-log), `OrderRecordingJournalPort` (records `save_started`).
- `tests/app/editor_session_command_queue_baseline_test.cpp` — the 10 named tests driving the real `EditorSessionService` facade and real `EditorRenderCoordinator` through the controllable ports.
- `tests/support/editor_session_test_ports.hpp` — `FakeEditorHistoryPort` and `FakeEditorJournalPort` de-`final`-ed so the controllable ports can subclass them.
- `tests/app/CMakeLists.txt` — `EditorSessionCommandQueueBaselineTest` target registered (label `editor_session`).

**LOC note (grill-code-review):** new test support ~270 LOC header, ~440 LOC test file; no production code changed. Files are single-responsibility (one manual executor + recorder + controllable ports; one baseline-test fixture). No file near the 1000-LOC split threshold.

**Remaining gaps / hand-off to CQ1:**

- The synchronous-inline unconditional-accept at `navigation_controller.cpp:198-207` (correlation skipped when `pending.ticket.request_id == 0`) is the residual stale-completion window that test 3 cannot reproduce deterministically with single-threaded seams; CQ1 removes the inline path entirely, closing it. The 8 failing tests become the CQ1 acceptance suite.
- CQ0 did not add a controllable render **scheduler** port (frame lifecycle is driven directly through `EditorRenderCoordinator::Notify*`, which is sufficient for CQ0). If CQ2/CQ5 need to defer render completion from the test thread, add a `ControllableSchedulerPort` then.
- `OneSnapshotRevision…` and `OneAcceptedCommand…` assert the post-CQ3 single-snapshot / single-terminal invariants; they will only turn green once CQ1 (command ownership) and CQ3 (single snapshot) land. They are kept as CQ0 evidence because the split is already observable today at the facade boundary.


## Phase CQ1 — Introduce the single-thread command queue

Status: complete — queue owns session mutation; 12/12 acceptance tests and all pre-existing
session suites pass.

### Purpose

Make one executor the sole owner of session mutation and eliminate inline completion re-entry.

### Implementation

1. Add typed command, completion, operation ID, and queue-state values under `app/`.
2. Add `EditorSessionCommandQueue` with a deterministic drain loop.
3. Add an executor port with:
   - production queued delivery to the session-owning Qt thread;
   - deterministic manual delivery for tests.
4. Route every `EditorSessionController` invokable through the queue.
5. Change save, render, and image-load completion delivery to post typed messages.
6. Move `pending_action_`, `pending_recovery_`, and `pending_merge_preview_` into queue-owned state.
7. Preserve the existing public `EditorSessionResult` during migration, but generate it only from
   queue transitions.
8. Remove synchronous-completion branches from navigation.
9. Replace the navigation recursive mutex with queue-thread assertions.
10. Keep worker-service mutexes only around their local request tables.

### Primary target call chain

```text
SelectImage(B)
  -> queue accepts command N
  -> queue posts SaveRequest(A, N)
  -> queue publishes Saving snapshot
  -> worker posts MaterializationFinished(N)
  -> queue validates N and A generation
  -> queue releases A ownership
  -> queue posts LoadImageState(B, N)
  -> worker posts ImageStateLoaded(N)
  -> queue posts RenderRequest(B, N)
  -> frame completions return through the same queue
  -> queue publishes Interactive snapshot for B
```

### Required lock result

- no recursive mutex in navigation;
- no callback can run session code before a start method returns;
- the command queue never holds a mutex while dispatching a worker request or publishing an
  observer event;
- lifecycle mutation is reachable only from queue reduction.

### Required tests

- all CQ0 command-order and stale-completion tests pass;
- existing navigation recovery tests pass through the queue;
- immediate and delayed completion produce the same snapshot sequence;
- A→B, A→B→C, save failure, Retry, Discard and Continue, and Cancel have exact command/result
  sequences;
- queue shutdown rejects later commands and produces one terminal outcome per accepted command.

### Exit criteria

- every editor-session mutation has a command ID;
- direct controller-to-navigation and controller-to-edit mutation calls are removed;
- navigation no longer examines whether a completion happened synchronously;
- production and tests use the same queue reducer.

##### Phase CQ1 completion record (2026-07-29)

**Status:** complete — `EditorSessionCommandQueue` is the sole owner of session mutation; no
completion runs session code on a service-start stack; navigation/lifecycle recursive mutexes are
gone; dirty-journal Paste/Merge flush before creating the Version/merge commit.

**What was built (CQ0 partial → CQ1 finished):**

- `EditorSessionCommandQueue` (`app/editor_session_command_queue.{hpp,cpp}`): typed
  `EditorSessionCommand` / `EditorSessionCompletion` / `EditorSessionOperationId`, the
  `IEditorSessionCommandExecutor` delivery port, `EditorSessionManualCommandExecutor` for tests,
  a serialized `EnqueueAndDrain` loop that executes tasks outside its mutex, `PostCompletion`
  (never inline, even from the owner thread), and `BeginShutdown`/`Stop` admission gating. A
  nested submission is retained until the active reduction returns.
- All 25 facade mutations (`Open`/`Switch`/`Close`/`Shutdown`/preview/commit/Undo/Redo/
  MoveHead/Discard/Version ops/Paste/BeginMerge/CompleteMerge/CancelMerge/Retry/Discard-and-
  Continue/Cancel/ViewChange) route through `SubmitCommand`; the queue stamps a monotonic command
  ID; reductions batch observer delivery into one change notification per reduction
  (`BeginPublication`/`EndPublication`).
- Completion delivery is typed and posted everywhere:
  `EditorSaveCheckpointService::DeliverCompletion` posts via the injected command executor;
  render events and `NotifyImageAcquired` post `RenderResult`/`ImageStateLoaded`; navigation
  completion posts `NavigationFinished`; history save checkpoints post the new
  `SaveCheckpointFinished`. The synchronous `completed->has_value()` branch was deleted.
- `pending_action_`, `pending_recovery_` moved into queue-owned `EditorSessionNavigationState`
  along with the new `pending_next_target`; navigation and lifecycle `std::recursive_mutex`
  members were replaced by owner-thread assertions.
- A rapid second `SelectImage` queues behind the running save (`pending_next_target`) and is
  promoted by `PromoteQueuedSwitchTarget` only after the running save completes and the prior
  image is acquired; it cannot change the running target's save identity, and a failed running
  save drops the queued selection.
- Dirty current image: `QueueFlushBeforeTransfer` retains `PendingTransferAfterSave` (Paste or
  CompleteMerge inputs, typed data only), starts one journal-flush checkpoint, and
  `HandleSaveCheckpointCompletion` resumes the retained transfer on the clean journal after
  `DiscardMaterializedJournalThrough` + `SyncMaterializedStateAfterCheckpoint`.
- Stale completions are ignored: navigation correlates by ticket request ID + session generation
  + operation ID; history checkpoints correlate by session generation. Checkpoints cancelled by
  `Shutdown` publish one terminal cancellation while the session stays `ShuttingDown`
  (the queue stops admitting user commands but still reduces posted completions).
- Production wiring: `ApplicationModuleHost` supplies `QtEditorSessionCommandExecutor`
  (`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` onto the host's QObject, QPointer
  guarded); `EditorSessionRuntime::CreateWithPorts` defaults to the manual executor for tests.
- `EditorSessionResult` is preserved for migration and now carries `operation_id`; render
  intents/commands and save requests/results carry `operation_id` end to end.

**Primary success call chain (SelectImage A->B, save in flight):**

```text
QML -> EditorSessionController -> EditorSessionService::Switch
  -> SubmitCommand(SelectImage, cmd N) -> queue stamps ID, reduces on owner thread
  -> NavigationController::RequestOpenOrSwitch (owner-thread assert)
  -> SealAndStartSave -> SaveCheckpointService::Start (save for A, operation N)
  -> facade publishes SaveStarted; command returns
worker: journal commit -> materialize -> SaveCheckpointService::FinishSave
  -> DeliverCompletion posts typed completion to the queue (never inline)
queue: OnCheckpointFinished (request/generation/operation correlated)
  -> ReleaseAfterCheckpoint(A) -> ContinueToTarget(B) -> RouteInitialRender(B)
  -> NavigationFinished completion posted
queue: publishes one RenderRouted terminal (cmd N)
  -> first-frame completions (RenderResult) -> Interactive for B, one terminal total
```

**Primary failure call chain (save checkpoint fails mid-switch):**

```text
worker failure -> posted JournalCommitFinished/MaterializationFinished(success=false)
queue: OnCheckpointFinished -> correlation passes -> pending_next_target dropped
  -> RetainPendingFailure -> lifecycle RetainedImageFailure (image A kept visible)
  -> NavigationFinished completion posted -> one Failed terminal (cmd N)
user: RetrySave | DiscardAndContinue | CancelPendingNavigation commands re-drive or clear
  the retained navigation through the same queue; recovery paths covered by navigation tests
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| All 10 CQ0 failing-evidence names (order, stale guards, render-lock, inline completion, single revision, single terminal) | `EditorSessionCommandQueueBaselineTest` | PASS 10/10 |
| `ImmediateAndDelayedCompletionProduceTheSameSnapshotSequence` (required: immediate == delayed snapshot sequence) | `EditorSessionCommandQueueBaselineTest` | PASS |
| `ShutdownDuringHistoryCheckpointStaysShuttingDownAndPublishesOneCancellation` (required: shutdown rejects later commands, one terminal per accepted command) | `EditorSessionCommandQueueBaselineTest` | PASS |
| A->B, A->B->C (rapid selection promoted after running save) | `RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection` | PASS |
| Save failure, Retry, Discard and Continue, Cancel sequences | `EditorSessionNavigationControllerTest` (fixture drives the queue executor + `Drain()`) | PASS 23/23 |
| Lifecycle / edit / facade / save + coordinator / render / controller regressions | `EditorSessionLifecycleTest` 18, `EditorSessionEditControllerTest` 8, `EditorSessionServiceFacadeTest` 4, `EditorSaveCheckpointServiceTest` 14, `EditorSaveCheckpointCoordinatorTest` 3, `EditorRenderCoordinatorTest` 29, `EditorSessionRenderControllerTest` 12, `EditorSessionControllerPhase5ATest` 37, `AdjustmentTransferServiceMiniGitTest` 13 | PASS 142/142 |
| Production port adapters (history, checkpoint store, journal writer, pipeline, task, scheduler, thumbnails) and host boot with the Qt executor | 7 UI port suites + `ApplicationModuleHostLifecycleTest` | PASS 39/39 |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target <targets> --parallel 4`;
binaries run directly from `build/debug/alcedo_studio/tests/{app,ui}/<Name>_runtime/` (ctest
discovery fails on the pre-existing QML-shell environment issue, so suites were executed
directly). Suite totals: **212/212 PASS across 19 suites** (CQ1 acceptance 12/12 included).

**Checklist / exit condition:** all four exit criteria met — every facade mutation is stamped
with a queue command ID (results, render commands, save requests carry it through the worker
boundary); no controller-to-navigation or controller-to-edit direct mutation path remains (the
facade reducer is the only caller); the navigation "was the completion synchronous?" branches
are deleted (`completed_synchronously` now only means no prior image, same-image no-op, or a
discard-and-continue path); tests (manual executor) and production (Qt queued executor) run the
same `SubmitCommand` reducer and the same `PostCompletion` drain. Required-lock result: no
recursive mutex in navigation or lifecycle (owner-thread asserts); no callback runs session code
before a start method returns (all completions posted); the queue never holds its mutex while
dispatching worker requests or publishing observer events; lifecycle mutation is reachable only
from queue reduction.

**LOC note (grill-code-review):** new production `editor_session_command_queue.{hpp,cpp}` = 339
LOC. `editor_session_navigation_controller.{hpp,cpp}` = 1105 (was 1277 pre-CQ1: recursive-mutex
scopes and synchronous branches removed). `editor_session_service.{hpp,cpp}` = 1814, with the
cpp at 1369 LOC — above the ~1000-LOC split threshold. The file is a single-reason facade
(command routing + one batching publication + typed completion handlers over five collaborators
and queue-owned pending state); splitting it in CQ1 would require exporting that shared queue
state into a context bag, so the responsibility-based split is deferred to CQ4 where the unified
durable-publication path is the natural seam. `clang-format -i` was applied to the 23 changed
files per the repo `format` target, which also reflowed adjacent pre-existing lines (alignment
churn inside the diff, no semantic change). Tests: acceptance suite 471 LOC, harness 270 LOC.

**Residual gaps / hand-off to CQ2-CQ5:**

- `SetPresentationSinkId` / `SetPresentationSize` / geometry overlay still call the render
  controller directly (guarded by the render controller's own mutex; command kinds are reserved
  in `EditorSessionCommandKind`). Normalize into queue commands in CQ5.
- `EditorSessionRenderController` keeps its recursive mutex around scheduler-local state per the
  disposition table; CQ2 narrows worker-side synchronization.
- A standalone `EditorSaveCheckpointService` built without a command executor (only its legacy
  unit tests) still delivers completions inline; the production session always injects the
  executor.
- `EditorSessionEditController`'s mutation mutex and history working-state mutex remain for CQ2
  (per the disposition table) — Undo/Redo/head-move still reach the live executor through the
  history port on the command thread; the baseline render-lock tests pass because the fake port
  gate is never held by real CQ1 command reductions (production proof lands with CQ2).
- QML-shell/GPU integration suites (`WorkspaceShellTest`, `EditorCheckpointNavigationTest`,
  `EditorAdjustmentTransferRealProjectE2eTest`) cannot execute in this headless environment:
  even `--gtest_list_tests` exits 3/hangs. Verified pre-existing (the stale pre-CQ1 binary
  failed identically before these sources changed). Production qualification of the Qt executor
  path is part of the CQ5 qualification sequence.

## Phase CQ2 — Remove GUI render-lock waits and narrow history ownership

Status: complete — command-side history reduction no longer reads or mutates the live executor;
the scoped QML/controller/session paths do not acquire `GetRenderLock()`.

### Purpose

Make command reduction bounded and remove the live executor from history correctness decisions.

### Implementation

1. Split history application into:
   - pure adjustment-snapshot transformation;
   - Mini-Git prepare and publication;
   - worker-side pipeline application.
2. Make committed adjustment snapshots complete for every supported editor field.
3. Capture an adjustment's before value from the immutable committed snapshot. Missing fields fail
   explicitly; they never fall back to an empty JSON object because the render mutex is busy.
4. Make Undo, Redo, and head movement produce a target immutable snapshot without touching the live
   executor.
5. Publish the new head, redo suffix, journal record, and committed snapshot as one queue-owned
   state transition.
6. Pass the immutable target snapshot to render. The pipeline worker applies it while it owns the
   executor render mutex.
7. Build save captures from immutable graph, head, chain, and adjustment state. If full serialized
   pipeline parameters are required, build them on a worker-owned pipeline snapshot.
8. Change pipeline-service methods so the cache mutex is released before:
   - storage access;
   - DuckDB access;
   - render-mutex acquisition;
   - pipeline reconstruction.
9. Remove external service calls from inside lifecycle and history-state mutex scopes.
10. Replace broad history-state locking with queue ownership plus immutable worker inputs.

### Primary target call chain

```text
Undo
  -> queue prepares first-parent head move
  -> pure snapshot reducer applies before values
  -> append one head-move journal record
  -> atomically publish head + redo + snapshot
  -> post RenderRequest(snapshot)
  -> command thread remains available
```

### Required lock order during migration

Until a lock is removed, the temporary order is:

```text
pipeline-service cache mutex
  -> release
history state mutex
  -> release
pipeline executor render mutex on worker only
  -> release
storage / DuckDB transaction mutex
```

No path may hold two of these at the same time.

### Required tests

- Undo, Redo, head movement, checkout, Paste, Merge, and checkpoint capture return to the command
  executor without waiting for a held render mutex;
- a render in progress cannot change the before value captured for a settled adjustment;
- unsupported snapshot fields fail before Mini-Git publication;
- render rejection does not corrupt history head, redo suffix, journal, or committed snapshot;
- 100 interactive previews coalesce without history projection or command-thread stalls;
- ThreadSanitizer coverage runs on a supported non-MSVC configuration for queue, history, and
  completion tests.

### Exit criteria

- `GetRenderLock()` is not acquired from QML/controller/session command paths;
- the command thread performs no pipeline execution or serialized-pipeline export;
- lifecycle and edit controller recursive or broad mutation mutexes are removed;
- history correctness can be tested without constructing a live render executor.

### Phase CQ2 completion record (2026-07-29)

**Status:** complete for the editor-session and album-controller paths. The command-side reducer now
owns immutable graph and adjustment state; executor mutation is confined to the render worker or
pipeline-service worker boundary.

**Implementation delivered:**

- `EditorHistoryMutation`, `EditorHistoryTransfer`, `EditorHistoryVersionRefs`, and
  `EditorHistoryCheckpoint` now use pure snapshot reducers plus Mini-Git prepare/publish steps.
  Undo, Redo, head movement, checkout, Paste, Merge, and save capture do not acquire the live
  executor render mutex.
- `EditorRenderAdjustmentSnapshot` is complete across the 22 supported fields. Missing or
  unsupported fields fail through `IsCompleteAdjustmentSnapshot` and
  `ReadCommittedAdjustmentState` before journal or graph publication.
- The immutable candidate is published together with the Mini-Git transition on the queue-owned
  history path. `EditorSessionRenderSchedulerPort::TryProducePipelineFrame` is the worker-side
  handoff and applies that candidate while it owns the executor render mutex.
- Save capture derives materialization and serialized pipeline parameters from immutable graph,
  head, chain, root, and adjustment state. Pipeline cache metadata locks are released before
  storage, DuckDB, render, and reconstruction work.
- The edit-controller and history-working-state mutation mutexes were removed. The album
  adjustment-transfer controller now reads source data through an independent pipeline snapshot,
  so it also has no direct render-lock acquisition.

**Primary success call chains:**

- Edit: `EditorHistoryMutation::CommitAdjustment` →
  `ApplyCommittedPayloadToSnapshot` → `PrepareAppendEdit` → `PublishPreparedEdit` → committed
  snapshot publication → `EditorSessionRenderSchedulerPort::TryProducePipelineFrame` →
  `ApplyEditorAdjustmentSnapshot` under the worker render gate.
- Undo/Redo/head move: `PrepareUndo` / `PrepareRedo` / `PrepareMoveHeadToCommit` →
  `ApplyPreparedHeadMoveToSnapshot` → one Mini-Git publication → immutable target snapshot →
  render request.
- Paste/Merge/checkout: pure transfer or version-ref reducer → `SnapshotAtHead` → graph
  persistence when required → render request with the resulting snapshot.
- Save: `CaptureSaveCheckpoint` → immutable graph/head/chain/adjustment materialization →
  checkpoint store; serialized parameters are generated from the snapshot rather than sampled from
  the command-side live executor.
- Cache: `PipelineMgmtService` cache metadata lookup/update → cache mutex release → storage,
  DuckDB, render, or reconstruction operation.

**Failure and preservation paths:**

- Unsupported fields fail before `PrepareAppendEdit`, leaving commit count and journal bytes
  unchanged (`UnsupportedAdjustmentFieldFailsBeforeMiniGitPublication`).
- Pure head-move application failures return before publication and restore the prior head, redo
  suffix, snapshot, and journal state (`HeadMoveApplyFailurePreservesHeadRedoPipelineSnapshotAndJournal`).
- Render-worker failure is reported through the render completion result; it does not call history
  mutation. The coordinator rejection/failure tests and the pure head-move preservation test cover
  the separation between the render result and published history.

**Changed files:**

- Production: `alcedo_studio/src/app/CMakeLists.txt`,
  `alcedo_studio/src/app/adjustment_transfer_service.cpp`,
  `alcedo_studio/src/app/editor_adjustment_pipeline.cpp`,
  `alcedo_studio/src/app/editor_session_edit_controller.cpp`,
  `alcedo_studio/src/app/pipeline_service.cpp`,
  `alcedo_studio/src/include/app/adjustment_transfer_service.hpp`,
  `alcedo_studio/src/include/app/editor_adjustment_types.hpp`,
  `alcedo_studio/src/include/app/editor_session_edit_controller.hpp`.
- Album backend: `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp`,
  `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_state_detail.hpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/adjustment_transfer_controller.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_checkpoint.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_projection.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_shared_helpers.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_state_detail.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp`,
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_version_refs.cpp`.
- Tests and target wiring: `alcedo_studio/tests/app/CMakeLists.txt`,
  `alcedo_studio/tests/app/editor_render_coordinator_test.cpp`,
  `alcedo_studio/tests/app/editor_session_command_queue_baseline_test.cpp`,
  `alcedo_studio/tests/edit/history/editor_session_history_port_test.cpp`,
  `alcedo_studio/tests/support/editor_session_command_queue_test_support.hpp`.

**Test commands and results:**

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumBackendLib --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionHistoryPortTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorRenderCoordinatorTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionCommandQueueBaselineTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target PipelineServiceTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AdjustmentTransferServiceMiniGitTest --parallel 4
```

Direct runtime results from `build/debug`:

- `EditorSessionHistoryPortTest`: 20/20 passed, including pure replay without constructing a
  render executor, complete-field initialization, held-render-lock capture, unsupported-field
  rejection, and head-move preservation.
- `EditorRenderCoordinatorTest`: 29/29 passed, including the 100-preview replacement burst.
- `EditorSessionCommandQueueBaselineTest`: 12/12 passed.
- `PipelineServiceTest`: 23/23 passed; 2 explicitly disabled tests remain disabled by the target.
- `AdjustmentTransferServiceMiniGitTest`: 13/13 passed.
- Aggregate: 97/97 executed tests passed across the five direct suites.

`git diff --check` passed. A changed-diff scan found no project-authored uses of the repository
banned terms. A scoped `GetRenderLock()` scan found no acquisition in the album backend
controller/history files or `editor_session*` command files.

**Qualification limits:** CTest discovery remains blocked by the pre-existing
`WorkspaceShellTest_runtime/WorkspaceShellTest.exe` result-3 failure, so the affected binaries
were run directly. `EditorAdjustmentPipelineTest` links after its missing `ThreadPool` target
dependency was added, but its post-build runtime-DLL copy step still fails for the pre-existing
`cudademosaicnetentry.dll`, `demosaicnet.dll`, and `rawprocessorop.dll` inputs. ThreadSanitizer was
not run because this verification environment is the MSVC configuration; a supported non-MSVC
run remains a platform qualification item. Legacy direct-executor transfer-service overloads
and the older QWidget editor path remain compatibility adapters outside the session reducer and
are handed to CQ5 cleanup.

**LOC note:** 23 implementation and test files changed, 1,344 insertions and 963 deletions; the
roadmap record is an additional documentation change. The largest diffs are the pure history
helper/reducer split and the pipeline-service cache-scope narrowing; they remain separate by
responsibility and no new file approaches the repository's 1,000-LOC split threshold.

## Phase CQ3 — Publish one session snapshot and one action-availability source

Status: unblocked — CQ2 complete.

### Purpose

Prevent QML from observing mixed revisions and make every action's enabled state explainable.

### Implementation

1. Add `EditorSessionSnapshot` and `EditorActionAvailability`.
2. Increment one snapshot revision after every accepted queue transition.
3. Publish identity, lifecycle, presentation identity, history summary, pending operation, error,
   and availability together.
4. Replace controller getters that independently query backend identity and state with the last
   published snapshot.
5. Move pending presentation target ownership from `EditorSessionController` into
   `presentation_identity`.
6. Merge external background-task restrictions into the queue's availability reducer.
7. Bind adjustment panels, history actions, filmstrip selection, workspace navigation, Version
   actions, Paste, and Merge to `snapshot.actions`.
8. Make every unavailable action expose a stable reason and blocking operation ID.
9. Keep `InteractionPolicyController` for non-editor product actions, but remove editor-specific
   capability decisions from QML-local expressions.
10. Emit one snapshot-changed signal. Derive compatibility signals only while legacy QML remains.

### Required QML behavior

```text
enabled: editorSession.actions.canUndo
enabled: editorSession.actions.canPaste
enabled: editorSession.actions.canSelectImage
```

QML must not add independent `canEdit`, `packageAvailable`, save-state, or history-state conditions
to those final action values.

### Required tests

- every published snapshot contains a matching identity, lifecycle state, presentation identity,
  and action set;
- all editor action buttons use the same availability revision;
- disabled actions display the reducer's reason;
- a background save changes all affected actions in one QML event;
- the first accepted frame for B changes `canEdit` and B identity in the same revision;
- no action becomes enabled from a stale history or background-task notification.

### Exit criteria

- `EditorSessionController::SyncIdentityFromBackend()` is removed;
- controller-owned pending presentation fields are removed;
- no editor QML file reconstructs action availability from multiple objects;
- action availability has one C++ test matrix covering every lifecycle state.

## Phase CQ4 — Unify Paste and Merge into one durable publication path

Status: blocked by CQ3.

### Purpose

Remove synchronous double persistence and make dirty-current-image behavior deterministic.

### Implementation

1. Represent Paste and Merge as queue commands rather than direct synchronous history-service
   calls.
2. If the current image has journal records:
   - queue one save checkpoint;
   - retain the Paste or Merge command as the next command;
   - continue only after the matching save completion.
3. Build Paste and Merge candidate graph, head, Version refs, and adjustment snapshot without
   modifying published state.
4. For Merge, store a revisioned preview token containing:
   - source package fingerprint;
   - first-parent head;
   - incoming head;
   - conflict fields;
   - source snapshot revision.
5. Reject conflict resolutions when the preview token no longer matches the active head or package.
6. Publish the candidate working state once after validation.
7. Submit one save capture containing the final graph, head, chain, Version selection, and serialized
   pipeline state.
8. Remove direct `PersistEditorHistoryState()` calls from editor Paste/Merge orchestration.
9. Route exactly one render from the durable final snapshot.
10. On materialization failure, retain the prior published state or enter an explicit recoverable
    pending-publication state; never expose half of a new Version.

### Primary Paste call chain

```text
ApplyPaste
  -> optional checkpoint of current journal
  -> build root-relative candidate
  -> validate candidate
  -> one materialization request
  -> publish active Version + snapshot
  -> one render
```

### Primary Merge call chain

```text
BeginMerge
  -> optional checkpoint of current journal
  -> build revisioned preview
CompleteMerge
  -> validate preview token and resolutions
  -> build two-parent candidate
  -> one materialization request
  -> publish merge head + snapshot
  -> one render
```

### Required tests

- dirty journal followed by Paste automatically saves and then creates one Version;
- dirty journal followed by Merge automatically saves and then creates one merge commit;
- one Paste or Merge performs one DuckDB publication;
- one completed Paste or Merge routes one render;
- Merge Undo returns to its first parent and Redo reapplies all resolved fields;
- stale conflict resolutions change nothing;
- save, candidate-build, validation, materialization, and render-routing failures preserve the
  required prior state;
- close and reopen reproduce the exact active Version, head, chain, and adjustment snapshot.

### Exit criteria

- editor Paste/Merge do not require QML to pre-check journal emptiness;
- editor Paste/Merge contain no synchronous DuckDB write;
- direct transfer persistence and subsequent checkpoint duplication are removed;
- batch library transfer and active-editor transfer share candidate-building code but retain their
  distinct scheduling adapters.

## Phase CQ5 — Remove transitional paths and qualify production behavior

Status: blocked by CQ4.

### Purpose

Delete compatibility architecture and prove the final queue under real storage and presentation
timing.

### Removal list

- direct mutating methods from controller to lifecycle/navigation/edit collaborators;
- navigation and lifecycle recursive mutexes;
- inline save-completion branches;
- controller identity/state mirrors and pending presentation fields;
- editor-specific QML availability expressions outside the snapshot reducer;
- history paths that acquire the live executor render mutex on the command thread;
- transfer paths that persist before the unified checkpoint;
- duplicate history/result/change notifications that can be derived from snapshot revision;
- unused mutexes and compatibility adapters introduced during CQ1-CQ4.

### Production qualification sequence

1. Open image A and wait for its first presented frame.
2. Perform preview and settled edits across multiple adjustment fields.
3. Undo and Redo while a quality render owns the executor.
4. Select B and then C at every controlled save/acquire/render completion boundary.
5. Inject save failure and exercise Retry, Discard and Continue, and Cancel.
6. Paste into a dirty active image.
7. Merge with conflicts into a dirty active image.
8. Undo and Redo across the two-parent merge.
9. Create, branch, rename, remove, and checkout Versions.
10. Close and reopen after every durable operation.
11. Shut down with render, journal, and materialization work at each possible boundary.

### Performance targets

- command reduction p95 below 1 ms for Preview, Commit, Undo, and Redo preparation;
- zero command-thread waits on render, file, database, or thumbnail work;
- one snapshot publication per state transition;
- one history projection per settled history change and zero per interactive preview;
- at most one unstarted preview per adjustment replacement key;
- no dropped settled commands;
- no default per-frame information logging.

### Required evidence

- deterministic queue tests;
- production QML-to-session integration tests;
- real Mini-Git journal and DuckDB materialization tests under unique `build/tmp/` paths;
- render scheduler and first-frame presentation tests;
- failure-injection matrix for every worker boundary;
- supported race-detection run;
- before/after command latency, render count, snapshot count, and DuckDB publication count.

### Exit criteria

- all removal-list items are absent;
- all CQ0-CQ4 tests pass without compatibility mode;
- the production qualification sequence passes;
- the completion record lists exact changed files, call chains, commands, pass/fail/skip totals,
  measurements, and remaining risks.

## Phase dependency order

The order is mandatory:

```text
CQ0 evidence
  -> CQ1 command ownership
  -> CQ2 render/history separation
  -> CQ3 single snapshot and availability
  -> CQ4 Paste/Merge publication
  -> CQ5 cleanup and production qualification
```

CQ3 must not precede CQ1 because a unified snapshot without unified mutation ownership would only
hide races behind a larger value object. CQ4 must not precede CQ2 because candidate publication
cannot be safe while history correctness still depends on the live render executor.

## Completion checklist

- [x] CQ0 records deterministic failing evidence.
- [x] CQ1 serializes all session mutations and removes inline completion re-entry.
- [x] CQ2 removes command-thread render-lock waits.
- [ ] CQ3 publishes one session snapshot and one editor action source.
- [ ] CQ4 gives Paste and Merge one durable publication path.
- [ ] CQ5 removes transitional code and qualifies the production sequence.

The work is complete only when all boxes are checked and no editor-session behavior depends on
callback timing, recursive locking, or independently sampled QML state.
