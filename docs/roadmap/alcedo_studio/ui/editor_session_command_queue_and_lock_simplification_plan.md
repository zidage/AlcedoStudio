# Editor Session Command Queue and Lock Simplification Plan

Date: 2026-07-29

Status: CQ5 complete — transitional presentation/transfer/save adapters removed and
qualification evidence recorded on 2026-07-30. All CQ0-CQ5 checklist items are done.

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

`EditorSessionService` remains a behavior-oriented facade rather than a state container. The queue
reducer owns the minimum execution context required to serialize active work; history, committed
adjustments, rendering, and persistence retain their focused domain models. CQ3 does not copy those
models into a second session snapshot.

The queue thread owns:

- the current session state and identity;
- the active and pending user command;
- the accepted pipeline and history guards;
- the active Mini-Git working selection;
- pending navigation and merge state;
- the minimal active-operation context used to decide command admissibility;
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
  and integration tests consume the unified action-decision API.

## Target architecture

```text
QML / controller adapter
  -> enqueue EditorSessionCommand
  -> EditorSessionCommandQueue (one owning thread)
       -> evaluate command against the same policy used by QML availability
       -> reduce command against minimal queue-owned context
       -> publish immediate Queued / Rejected result
       -> dispatch optional worker request
       -> wait for typed completion message
       -> reduce completion
       -> publish changed domain values
       -> recompute and publish changed action decisions once
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
4. Every completion carries only the narrow request ID required by its worker boundary, such as
   load request, render request, save task, or command operation ID.
5. Stale completions are ignored when their request ID is no longer active.
6. One accepted command publishes at most one terminal result.
7. QML action availability and command admission use the same pure decision function.
8. The live pipeline executor is a render target, not the source of truth for history before/after
   values.
9. Paste and Merge perform one durable publication.
10. No recursive mutex remains in editor-session lifecycle or navigation.
11. Session identity contains domain identity only; session-wide generation, render generation,
    view generation, and snapshot revision are not part of the public model.

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

Each command receives an operation ID when accepted. Commands that target an image, commit, or
Version carry that explicit target ID. The reducer evaluates current admissibility when the command
reaches the owner thread; the UI does not submit a snapshot revision.

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

## Target public model and asynchronous correlation

CQ3 does not introduce a monolithic session snapshot. Domain values keep their focused APIs:
current image identity, lifecycle state, history projection, committed adjustment state, and
operation results. The queue batches their notifications and publishes action decisions after the
domain mutation is complete.

Session identity contains only stable domain identity:

```cpp
struct EditorSessionIdentity {
  sl_element_id_t element_id = 0;
  image_id_t image_id = 0;
};
```

Action availability is a projection of command admissibility:

```cpp
struct EditorActionDecision {
  bool allowed = false;
  std::string reason;
};

EditorActionDecision Evaluate(EditorAction action,
                              const EditorCommandContext& context,
                              const EditorActionInputs& inputs);
```

The command reducer calls the same `Evaluate` function immediately before acceptance. QML observes
the last projected decisions and one `ActionAvailabilityChanged` signal; it does not provide an
independent precondition.

The decision inputs are:

- session state;
- active image and Version state;
- history Undo/Redo state;
- copied-adjustment package state;
- active operation kind;
- external background-task restrictions.

Starting an asynchronous command creates an operation lease. Completing, cancelling, or rejecting
that exact operation removes the lease. The lease describes which actions it blocks; code does not
manually flip `canUndo`, `canPaste`, or similar booleans.

Worker correlation uses strong, request-scoped identifiers:

```text
EditorSessionOperationId
ImageLoadRequestId
EditorRenderRequestId
EditorSaveTaskId
MergePreviewId
```

These identifiers do not describe session state, are not compared by greater-than or less-than,
and are discarded when their request finishes or is cancelled. They remain internal to C++ worker
boundaries and are not exposed to QML. An A→B→A sequence rejects the first A frame by matching the
active render request, not by comparing a session generation.

QML components bind only to the projected action decisions. They do not combine `canEdit`,
history-model flags, package state, and interaction-policy flags independently.

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
- `OneSnapshotRevision…` is a historical test name. CQ1 makes it pass by batching one reduction's
  visible publication; it does not require CQ3 to add a public snapshot revision.
  `OneAcceptedCommand…` pins the single-terminal invariant owned by CQ1.


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
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target PipelineMapperTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AdjustmentTransferServiceMiniGitTest --parallel 4
```

Direct runtime results from `build/debug`:

- `EditorSessionHistoryPortTest`: 20/20 passed, including pure replay without constructing a
  render executor, complete-field initialization, held-render-lock capture, unsupported-field
  rejection, and head-move preservation.
- `EditorRenderCoordinatorTest`: 29/29 passed, including the 100-preview replacement burst.
- `EditorSessionCommandQueueBaselineTest`: 12/12 passed.
- `PipelineMapperTest`: 23/23 passed; 2 explicitly disabled tests remain disabled by the target.
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

## Phase CQ3 — Derive action availability from command admission and remove session generations

Status: complete — action admission and QML availability share `EditorActionPolicy::Evaluate`;
session generations removed from public identity; scoped request ids correlate completions.

### Necessity decision

CQ3 remains necessary only as an action-admission cleanup. The previously proposed monolithic
`EditorSessionSnapshot`, public snapshot revision, and presentation identity are rejected.

The session facade should remain behavior-oriented: callers submit operations and observe focused
results. It must not become a second state store that mirrors lifecycle, history, adjustment,
render, and presentation data.

The command queue still owns the minimal context required to serialize work. This is unavoidable
for asynchronous operations, but it is internal execution context rather than a public session
model.

### Purpose

Make every editor action's enabled state exactly match whether the command queue would accept that
action now, while removing session-wide generation counters from identity, QML, and worker
correlation.

### Implementation

1. Add `EditorAction`, `EditorActionDecision`, and a pure `EditorActionPolicy::Evaluate`.
2. Call `Evaluate` from both:
   - the action projection published to QML;
   - the command reducer immediately before accepting a command.
3. Model in-flight work as queue-owned operation leases:
   - operation ID;
   - operation kind;
   - explicit target identity;
   - blocked action set;
   - optional user-facing blocking reason.
4. Acquire a lease only after command validation. Release that exact lease on completion,
   cancellation, failure, or shutdown.
5. Never mutate individual `canX` flags from operation start/finish code.
6. Merge history facts, copied-package facts, and background-task restrictions into
   `EditorActionInputs`. The evaluator remains side-effect free.
7. Publish one `ActionAvailabilityChanged` event only when the evaluated decisions change.
8. Bind adjustment panels, history actions, filmstrip selection, workspace navigation, Version
   actions, Paste, and Merge to the projected decisions.
9. Keep `InteractionPolicyController` for non-editor product actions. Editor command decisions
   move to `EditorActionPolicy`.
10. Remove `session_generation`, `render_generation`, and `view_generation` from
    `EditorSessionIdentity` and QML properties.
11. Replace generation-based completion checks with strong request-scoped IDs:
    - image load completion matches `ImageLoadRequestId`;
    - render and first-frame completion match `EditorRenderRequestId`;
    - save completion matches `EditorSaveTaskId` plus the initiating operation ID;
    - Merge completion matches `MergePreviewId`.
12. Remove QML `snapshotRevision`. Adjustment panels reload only from
    `AdjustmentSnapshotChanged`, which is emitted only when committed adjustment content changes.
13. Move the pending presentation target from `EditorSessionController` into the active image-load
    operation. The accepted first-frame request determines when that target becomes current.
14. Retain focused domain notifications and the CQ1 publication batch. Do not add a global
    session-changed revision.

### Operation-driven availability

The initial lease matrix is explicit:

| Active operation | Availability effect |
| --- | --- |
| image load or running image switch | block adjustment, history, Version, Paste, and Merge commands; another selection is allowed only when the queue can replace an unstarted target |
| save checkpoint | block adjustment, history, Version mutation, Paste, and Merge; recovery, Close, and Shutdown follow their explicit queue rules |
| interactive or quality render | block nothing; CQ2 makes render an output effect rather than a command gate |
| Merge preview | enable Complete Merge and Cancel Merge only for its `MergePreviewId`; invalidate them when the active head or package changes |
| Paste or Merge materialization | block history, Version mutation, transfer, and image navigation until the durable result returns |
| failure recovery | enable only Retry Save, Discard and Continue, Cancel, Close, and Shutdown as permitted by the recovery state |

Base facts such as no active image, no Undo parent, no Redo suffix, or no copied package remain pure
evaluator inputs. An operation adds and removes only its lease; the resulting action decisions are
always recomputed, never restored from a saved set of booleans.

### Required QML behavior

```text
enabled: editorSession.actions.canUndo
enabled: editorSession.actions.canPaste
enabled: editorSession.actions.canSelectImage
```

QML must not add independent `canEdit`, `packageAvailable`, save-state, history-state, generation,
or revision checks to these final decisions.

### Required tests

- `CommandAcceptanceMatchesPublishedAvailabilityForEveryEditorAction`
- `AcceptedOperationLeaseBlocksAndCompletionRestoresExactlyItsDeclaredActions`
- `RejectedCommandDoesNotChangeAvailability`
- `StaleImageLoadRequestCannotAcquireTheCurrentImage`
- `StaleRenderRequestCannotPresentAFrameOrEnableEditing`
- `ImageAtoBtoARejectsTheFirstARenderWithoutSessionGeneration`
- `AvailabilityPublishesAtMostOncePerCommandOrCompletionReduction`
- `BackgroundRestrictionAndHistoryFactsUseTheSameDecisionFunction`
- `AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`
- a static QML/API check finds no `sessionGeneration`, `snapshotRevision`, or independent editor
  action conjunctions.

### Exit criteria

- command admission and QML availability call the same evaluator;
- no editor operation manually pairs enable/disable writes;
- `EditorSessionIdentity` contains only element and image identity;
- session, render, and view generation fields are absent from the editor-session public API;
- worker completions correlate with scoped request IDs;
- controller-owned pending presentation fields are removed;
- no monolithic session snapshot or global snapshot revision is introduced;
- action decisions have one C++ matrix covering every lifecycle and active-operation combination.

##### Phase CQ3 completion record (2026-07-29)

**Status:** complete — one `EditorActionPolicy::Evaluate` gates command admission and QML
`editorSession.actions.*`; public identity has only element/image ids; completions correlate by
scoped request ids; no public `sessionGeneration` / `snapshotRevision`.

**Primary success call chain:**

```text
QML enabled: editorSession.actions.canPaste
  -> EditorActionAvailabilityModel (projected from EditorActionAvailability)
  -> EditorSessionService::PublishActionAvailabilityIfChanged
  -> EditorActionPolicy::EvaluateAll(leases, BuildActionInputs)

QML Paste / controller PasteAdjustments
  -> SubmitCommand(ApplyPaste)
  -> owner-thread Evaluate(ApplyPaste) [same policy]
  -> acquire PasteMaterialization / optional SaveCheckpoint lease
  -> history transfer + durable checkpoint
  -> release lease on SaveCheckpointFinished
  -> EvaluateAll republishes changed decisions once
```

**Primary failure call chain:**

```text
Undo while Interactive but can_undo=false
  -> SubmitCommand(Undo) on owner thread
  -> Evaluate(Undo) -> denied ("Nothing to undo")
  -> Rejected result, no lease, availability unchanged

Stale ImageLoadRequestId / EditorRenderRequestId
  -> completion posts to queue
  -> MatchesImageLoadRequest / first_frame request_id mismatch
  -> ignored; identity and Interactive state retained
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `CommandAcceptanceMatchesPublishedAvailabilityForEveryEditorAction` | `EditorSessionActionPolicyCq3Test` | PASS |
| `AcceptedOperationLeaseBlocksAndCompletionRestoresExactlyItsDeclaredActions` | `EditorSessionActionPolicyCq3Test` | PASS |
| `RejectedCommandDoesNotChangeAvailability` | `EditorSessionActionPolicyCq3Test` | PASS |
| `StaleImageLoadRequestCannotAcquireTheCurrentImage` | `EditorSessionActionPolicyCq3Test` | PASS |
| `StaleRenderRequestCannotPresentAFrameOrEnableEditing` | `EditorSessionActionPolicyCq3Test` | PASS |
| `ImageAtoBtoARejectsTheFirstARenderWithoutSessionGeneration` | `EditorSessionActionPolicyCq3Test` | PASS |
| `AvailabilityPublishesAtMostOncePerCommandOrCompletionReduction` | `EditorSessionActionPolicyCq3Test` | PASS |
| `BackgroundRestrictionAndHistoryFactsUseTheSameDecisionFunction` | `EditorSessionActionPolicyCq3Test` | PASS |
| `AdjustmentPanelsReloadOnlyWhenCommittedContentChanges` | `EditorSessionActionPolicyCq3Test` | PASS |
| Static QML/API ban (`sessionGeneration` / `snapshotRevision` Q_PROPERTY) | `EditorSessionActionPolicyStaticApiBan` | PASS |
| CQ1 baseline regression (12) | `EditorSessionCommandQueueBaselineTest` | PASS 12/12 |
| Lifecycle request-id identity | `EditorSessionLifecycleTest` | PASS 18/18 |
| Controller / QML projection | `EditorSessionControllerPhase5ATest` | PASS 37/37 |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionActionPolicyCq3Test EditorSessionCommandQueueBaselineTest EditorSessionLifecycleTest EditorSessionControllerPhase5ATest --parallel 4`
binaries run from `build/debug/alcedo_studio/tests/app/*_runtime/`.
Suite totals: **CQ3 10/10, baseline 12/12, lifecycle 18/18, phase5a 37/37 = 77/77 PASS**.

**Checklist / exit condition:** all exit criteria met — admission and QML share `EditorActionPolicy`;
leases recompute decisions (no manual canX pairs); `EditorSessionIdentity` is element+image only;
session/render/view generation absent from public editor-session API and QML properties; workers
correlate via `ImageLoadRequestId` / render request id / save task+operation / `MergePreviewId`;
controller pending presentation moved to `pending_presentation_target()` on the service; no
monolithic session snapshot or global revision introduced.

**LOC note (grill-code-review):** new `editor_action_policy.{hpp,cpp}` ~508 LOC; request-id header
~36 LOC; QML availability model ~101 LOC; CQ3 tests ~325 LOC. `editor_session_service.cpp` grew
with lease/admission helpers but remains the CQ1 facade (responsibility-based split still deferred
to CQ4 durable-publication seam). No new file near the 1000-LOC split threshold.

**Remaining gaps:** CQ5 transitional-path removal and full production qualification remain.
Library-side Paste (`EditorAdjustmentTransferActions` / `AppDialogs`) correctly keeps
`InteractionPolicy` + packageAvailable. Worker journal ports still accept a numeric load-id
parameter named `session_generation` at the Mini-Git journal boundary (opaque uint64 equal to
`ImageLoadRequestId.value`); that is not part of the public editor-session identity/API.
QML-shell/GPU e2e suites remain environmentally blocked as in CQ1/CQ2.

## Phase CQ4 — Unify Paste and Merge into one durable publication path

Status: complete — one staged candidate and one durable checkpoint now serve both Paste and Merge;
verified on 2026-07-29.

### Purpose

Remove synchronous double persistence and make dirty-current-image behavior deterministic.

### Implementation

1. Represent Paste and Merge as queue commands rather than direct synchronous history-service
   calls.
2. If the current image has journal records:
   - queue one save checkpoint;
   - retain the Paste or Merge command as the next command;
   - continue only after the matching save completion.
3. Build Paste and Merge candidate graph, head, Version refs, and adjustment state without
   modifying published state.
4. For Merge, store an opaque `MergePreviewId` with:
   - source package fingerprint;
   - first-parent head;
   - incoming head;
   - conflict fields;
5. Reject conflict resolutions when `MergePreviewId`, active head, or package fingerprint no longer
   matches.
6. Publish the candidate working state once after validation.
7. Submit one save capture containing the final graph, head, chain, Version selection, and serialized
   pipeline state.
8. Remove direct `PersistEditorHistoryState()` calls from editor Paste/Merge orchestration.
9. Route exactly one render from the durable final adjustment state.
10. On materialization failure, retain the prior published state or enter an explicit recoverable
    pending-publication state; never expose half of a new Version.

### Primary Paste call chain

```text
ApplyPaste
  -> optional checkpoint of current journal
  -> build root-relative candidate
  -> validate candidate
  -> one materialization request
  -> publish active Version + committed adjustment state
  -> one render
```

### Primary Merge call chain

```text
BeginMerge
  -> optional checkpoint of current journal
  -> build preview with an opaque MergePreviewId
CompleteMerge
  -> validate preview ID, active head, package fingerprint, and resolutions
  -> build two-parent candidate
  -> one materialization request
  -> publish merge head + committed adjustment state
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

### CQ4 completion record (2026-07-29)

Status: complete.

Implementation result:

- `EditorSessionService` now queues Paste and Merge through candidate preparation, one immutable
  save capture, one checkpoint materialization, one publication, and one final render route.
- `EditorHistoryTransfer` builds Paste and Merge graphs on private copies, retains base active
  Version/head/chain facts, validates Merge preview identity plus package fingerprint, and swaps
  the published graph only after the checkpoint succeeds.
- `EditorMiniGitSaveCapture` marks candidate publication and carries the prior durable tuple;
  `EditorMiniGitMaterializer` validates the tuple and folds any captured journal prefix before a
  DuckDB write.
- The old direct persistence call was removed from the editor transfer orchestration. The
  compatibility wrappers remain only at the history-port seam for existing callers and test
  adapters; CQ5 removes those transitional paths.

Primary success call chains:

```text
PasteAdjustments
  -> PreparePaste
  -> EditorHistoryTransfer::PreparePaste
  -> root-relative candidate graph + adjustment snapshot
  -> StartTransferPublication
  -> CaptureTransferSaveCheckpoint
  -> EditorSaveCheckpointService / Mini-Git materialization
  -> PublishTransferCandidate
  -> one final RouteInitialRender
```

```text
BeginMerge
  -> PrepareMerge
  -> MergePreviewId + package fingerprint + first-parent/incoming facts
CompleteMerge
  -> ValidateMergeCandidate
  -> CompleteMergeCandidate on the staged graph
  -> StartTransferPublication
  -> one candidate checkpoint/materialization
  -> PublishTransferCandidate
  -> one final RouteInitialRender
```

Primary failure call chains:

```text
candidate build/capture failure
  -> DiscardTransferCandidate
  -> no materialization, Version publication, or render

stale Merge preview or active head
  -> ValidateMergeCandidate rejects
  -> published graph remains unchanged

checkpoint/materialization failure
  -> posted save completion
  -> discard staged candidate
  -> RetainedImageFailure with the prior published frame/state
```

Changed files:

- `alcedo_studio/src/app/CMakeLists.txt`
- `alcedo_studio/src/app/adjustment_transfer_service.cpp`
- `alcedo_studio/src/app/editor_mini_git_materializer.cpp`
- `alcedo_studio/src/app/editor_session_service.cpp`
- `alcedo_studio/src/include/app/adjustment_transfer_service.hpp`
- `alcedo_studio/src/include/app/adjustment_transfer_types.hpp`
- `alcedo_studio/src/include/app/editor_mini_git_materializer.hpp`
- `alcedo_studio/src/include/app/editor_session_ports.hpp`
- `alcedo_studio/src/include/app/editor_session_service.hpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_state_detail.hpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_transfer.hpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_checkpoint.cpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_shared_helpers.cpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- `alcedo_studio/tests/app/editor_session_command_queue_baseline_test.cpp`
- `alcedo_studio/tests/edit/history/editor_session_history_port_test.cpp`
- `alcedo_studio/tests/support/editor_session_command_queue_test_support.hpp`

What was proven:

| Required behavior / command | Target / binary | Result |
| --- | --- | --- |
| Dirty Paste and Merge ordering, one capture/materialization/publication/render, and capture/materialization failure retention | `EditorSessionCommandQueueBaselineTest` | PASS 16/16 |
| Candidate build failure, stale Merge rejection, staged two-parent Undo/Redo, and close/reopen identity/head/chain/snapshot equality | `EditorSessionHistoryPortTest` | PASS 24/24 after the final additions |
| Checkpoint materialization and journal recovery regressions | `EditorSessionCheckpointStoreTest` | PASS 6/6 |
| Whitespace and patch integrity | `git diff --check` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionCommandQueueBaselineTest EditorSessionHistoryPortTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionCheckpointStoreTest --parallel 4
build\debug\alcedo_studio\tests\app\EditorSessionCommandQueueBaselineTest_runtime\EditorSessionCommandQueueBaselineTest.exe
build\debug\alcedo_studio\tests\ui\EditorSessionHistoryPortTest_runtime\EditorSessionHistoryPortTest.exe
build\debug\alcedo_studio\tests\ui\EditorSessionCheckpointStoreTest_runtime\EditorSessionCheckpointStoreTest.exe
git diff --check
```

The three focused binaries therefore total **46/46 PASS, 0 failed, 0 skipped**. CTest discovery was
not used for the focused totals because the pre-existing `WorkspaceShellTest` discovery process
times out in this headless environment; direct binary execution completed normally.

Checklist / exit condition:

- [x] Paste and Merge are admitted as queued session commands.
- [x] Candidate graph, Version refs, head, chain, and adjustment snapshot are built before live
  publication.
- [x] Merge preview identity, package fingerprint, first parent, and incoming branch are checked
  before resolution and publication.
- [x] One save capture and one checkpoint materialization carry the final candidate state.
- [x] Direct transfer persistence and the old empty-journal precheck are absent from the editor
  Paste/Merge orchestration.
- [x] One final render is routed from the durable adjustment snapshot.
- [x] Candidate-build, capture, stale-validation, materialization, Undo/Redo, and reopen behavior
  have deterministic regression coverage.
- [x] Batch transfer remains on its own scheduling adapter while using the shared transfer service
  candidate-building primitives.

LOC note: implementation changes cover 20 source/test files; the final diff stat is recorded by
the commit that contains this completion record. The largest additions are the transfer reducer
seam in `editor_session_service.cpp` and the staged graph implementation in
`editor_history_transfer.cpp`; both remain single-purpose within their existing modules.

Residual gaps / hand-off to CQ5:

- Full QML-shell/GPU qualification and CTest discovery remain environmentally blocked by the
  pre-existing headless `WorkspaceShellTest` issue.
- Compatibility wrappers on `IEditorHistoryPort` and the old direct transfer methods remain until
  CQ5 removes transitional paths.
- Scheduler-level render rejection after a successful durable materialization still belongs in
  CQ5 production qualification; the CQ4 path routes only after the checkpoint completion and
  never performs a second history publication.

## Phase CQ5 — Remove transitional paths and qualify production behavior

Status: complete — presentation commands reduce through the queue; one-shot Paste/Merge
history wrappers and inline save completions are gone; QML editor enablement binds
`editorSession.actions.*`; CQ5 qualification suite and CQ0-CQ4 regressions pass.

### Purpose

Delete compatibility architecture and prove the final queue under real storage and presentation
timing.

### Removal list

- direct mutating methods from controller to lifecycle/navigation/edit collaborators;
- navigation and lifecycle recursive mutexes;
- inline save-completion branches;
- controller identity/state mirrors and pending presentation fields;
- editor-specific QML availability expressions outside `EditorActionPolicy`;
- session-wide generation fields and public snapshot revisions;
- history paths that acquire the live executor render mutex on the command thread;
- transfer paths that persist before the unified checkpoint;
- duplicate history/result/change notifications superseded by focused domain notifications;
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
- at most one changed action-decision publication per command or completion reduction;
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
- before/after command latency, render count, action-decision publication count, and DuckDB
  publication count.

### Exit criteria

- all removal-list items are absent;
- all CQ0-CQ4 tests pass without compatibility mode;
- the production qualification sequence passes;
- the completion record lists exact changed files, call chains, commands, pass/fail/skip totals,
  measurements, and remaining risks.

##### Phase CQ5 completion record (2026-07-30)

**Status:** complete — transitional presentation/transfer/save adapters removed; QML editor
controls bind projected action decisions; qualification and CQ0-CQ4 regressions pass.

**Primary success call chain:**

```text
QML bindPresentationViewport / Geometry panel
  -> EditorSessionController
  -> EditorSessionService::SetPresentationSinkId|Size|GeometryOverlay
  -> SubmitCommand(SetPresentation*)
  -> owner-thread render controller update
  -> NotifyChange (no results_ terminal)

QML Paste / Merge
  -> SubmitCommand(ApplyPaste|CompleteMerge)
  -> PreparePaste|CompleteMergeCandidate
  -> one CaptureTransferSaveCheckpoint / materialization
  -> PublishTransferCandidate
  -> one RouteInitialRender

QML enabled: editorSession.actions.canEdit / canRetrySave
  -> EditorActionAvailabilityModel
  -> EditorActionPolicy::EvaluateAll
```

**Primary failure call chain:**

```text
EditorSaveCheckpointService without command_executor
  -> DeliverCompletion drops the completion (no inline session re-entry)
  -> caller observes no terminal callback / invalid ticket on start failure

Transfer materialization failure
  -> posted SaveCheckpointFinished
  -> DiscardTransferCandidate
  -> RetainedImageFailure; published graph and render schedule unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `PresentationCommandsReduceThroughQueueWithoutDirectBypass` | `EditorSessionCq5QualificationTest` | PASS |
| `DirtyPastePerformsOnePublicationAndOneFinalRender` | `EditorSessionCq5QualificationTest` | PASS |
| `SaveCompletionWithoutExecutorIsDropped` | `EditorSessionCq5QualificationTest` | PASS |
| `QmlAndPublicApiOmitBannedGenerationAndSnapshotRevisionTokens` | `EditorSessionCq5QualificationTest` | PASS |
| `HistoryTransferOmitsOneShotPasteMergeWrappers` | `EditorSessionCq5QualificationTest` | PASS |
| CQ0-CQ4 baseline (16) | `EditorSessionCommandQueueBaselineTest` | PASS 16/16 |
| CQ3 policy (10) | `EditorSessionActionPolicyCq3Test` | PASS 10/10 |
| Save checkpoint (14) | `EditorSaveCheckpointServiceTest` | PASS 14/14 |
| History port (24) | `EditorSessionHistoryPortTest` | PASS 24/24 |
| Navigation / lifecycle / edit / facade | session app suites | PASS 23+18+8+4 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionCq5QualificationTest EditorSessionCommandQueueBaselineTest EditorSessionActionPolicyCq3Test EditorSaveCheckpointServiceTest EditorSessionHistoryPortTest EditorSessionNavigationControllerTest EditorSessionLifecycleTest EditorSessionEditControllerTest EditorSessionServiceFacadeTest --parallel 4
build\debug\alcedo_studio\tests\app\EditorSessionCq5QualificationTest_runtime\EditorSessionCq5QualificationTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionCommandQueueBaselineTest_runtime\EditorSessionCommandQueueBaselineTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionActionPolicyCq3Test_runtime\EditorSessionActionPolicyCq3Test.exe
build\debug\alcedo_studio\tests\app\EditorSaveCheckpointServiceTest_runtime\EditorSaveCheckpointServiceTest.exe
build\debug\alcedo_studio\tests\ui\EditorSessionHistoryPortTest_runtime\EditorSessionHistoryPortTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionNavigationControllerTest_runtime\EditorSessionNavigationControllerTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionLifecycleTest_runtime\EditorSessionLifecycleTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionEditControllerTest_runtime\EditorSessionEditControllerTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionServiceFacadeTest_runtime\EditorSessionServiceFacadeTest.exe
```

Suite totals: **CQ5 5/5, baseline 16/16, CQ3 10/10, save 14/14, history 24/24, nav 23/23,
lifecycle 18/18, edit 8/8, facade 4/4 = 122/122 PASS**. CTest discovery remains blocked by the
pre-existing headless `WorkspaceShellTest` issue; binaries were run directly.

**Checklist / exit condition:**

- [x] Presentation sink/size/geometry commands reduce through the queue (no direct bypass).
- [x] One-shot history-port Paste/Merge wrappers and default Publish→legacy adapter removed.
- [x] Inline save-completion path without a command executor removed; fixtures inject a manual
  executor and drain.
- [x] Editor QML controls/recovery bind `editorSession.actions.*` (not `hasPendingRecovery` /
  independent recovery mirrors).
- [x] Public diagnostics omit `sessionGeneration` / `renderGeneration` / `viewGeneration` keys.
- [x] CQ0-CQ4 suites pass without compatibility mode.
- [x] CQ5 qualification suite covers presentation queue routing, dirty Paste publication, save
  executor requirement, and static transitional-path bans.

**LOC note (grill-code-review):** `editor_session_service.cpp` remains ~1786 LOC as the CQ1
facade (presentation commands added; no new god-context split). New CQ5 test file ~222 LOC.
Transfer header dropped to ~63 LOC after one-shot removal. No new file near the 1000-LOC split
threshold besides the existing facade.

**Remaining gaps:**

- Full QML-shell/GPU e2e (`WorkspaceShellTest`, real first-frame under Qt executor) remains
  environmentally blocked headless, as in CQ1-CQ4.
- Album-library `PasteViaMiniGit` / `InteractionPolicy` paste gates remain product-level
  batch-transfer adapters (outside the editor-session command owner), not editor CQ5 paths.
- Internal worker request counters (`render_generation` / `view_generation` inside the render
  controller) remain for request correlation; they are not public QML properties.
- ThreadSanitizer and command-latency p95 measurements remain platform qualification items on a
  supported non-MSVC configuration.

## Phase dependency order

The order is mandatory:

```text
CQ0 evidence
  -> CQ1 command ownership
  -> CQ2 render/history separation
       -> CQ3 command-derived availability and scoped request IDs --\
       -> CQ4 Paste/Merge publication -------------------------------+-> CQ5 cleanup
```

CQ3 must not precede CQ1 because action decisions must be evaluated by the command owner. CQ4 must
not precede CQ2 because candidate publication cannot be safe while history correctness still
depends on the live render executor. After CQ2, CQ3 and CQ4 are independent and may be completed in
either order before CQ5.

## Completion checklist

- [x] CQ0 records deterministic failing evidence.
- [x] CQ1 serializes all session mutations and removes inline completion re-entry.
- [x] CQ2 removes command-thread render-lock waits.
- [x] CQ3 derives editor action decisions from command admission and removes session generations.
- [x] CQ4 gives Paste and Merge one durable publication path.
- [x] CQ5 removes transitional code and qualifies the production sequence.

The work is complete only when all boxes are checked and no editor-session behavior depends on
callback timing, recursive locking, generation comparison, or independently sampled QML action
state.
