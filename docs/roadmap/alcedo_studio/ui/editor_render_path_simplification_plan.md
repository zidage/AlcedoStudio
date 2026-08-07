# Editor Render Path Simplification Plan

Date: 2026-08-06

Status: Phases R1–R4 complete; optional residual cleanup (completion direction +
`replacement_key`) complete on branch `feature/editor_render_simplify`. Interactive-app
verification follow-up intentionally deferred. Delivery is one feature branch with
sequential commits (not one GitHub PR per phase).

Primary owner: Alcedo Studio editor session render path.

Affected areas:

- `EditorRenderCoordinator` request coalescing and single-flight scheduling;
- `EditorSessionRenderSchedulerPort` production adapter;
- `PipelineScheduler` / `PipelineTask` completion signaling;
- editor session open/switch image context for render input;
- focused unit tests for coordinator, scheduler port, and pipeline request identity.

Related roadmaps:

- [Editor Session Command Queue and Lock Simplification Plan](editor_session_command_queue_and_lock_simplification_plan.md)
- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)

This plan does not replace CQ0–CQ5 command-queue ownership. It reduces layered scheduling that
grew on top of the render submit port after the session/render split.

## Review findings (source diagnosis)

These findings come from a code review that started at
`alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp` and followed the
production render path into the coordinator and scheduler port. They are the product requirement
for this plan; implementation must shrink complexity against this list, not invent a parallel
framework.

### Lifetime and ownership

- `EditorSessionController` holds several raw pointers (`editor_`, `session_backend_`,
  `interaction_policy_`) whose ownership is only implied by host construction order. Viewport
  handles already use `QPointer`; the rest of the graph should document owner/borrower rules rather
  than a blanket `shared_ptr` rewrite of QObject trees.

### Coordinator queue and cancel

- `EditorRenderCoordinator` cancel paths walk the entire pending `deque` more than once (obsolete
  image-load cleanup, session cancel, pre-schedule token/epoch scrub), then `SelectNextIndex`
  walks the remaining deque again to pick a frame role / priority winner.
- Submit accepts work into a pending queue and only then calls `ScheduleNext`. That single-flight
  pattern is sound, but the surrounding state machine grew large around it (terminal-id sets,
  delivery re-entrancy guards, string-keyed replacement).

### Scheduler port is a second scheduler

- `EditorSessionRenderSchedulerPort` reverse-references `EditorRenderCoordinator` via
  `weak_ptr` and calls `NotifySchedulerCompleted`. Control flow goes coordinator → port →
  coordinator.
- The port owned a private worker thread and a second job slot (`queued_job_` / `running_job_`)
  even though `PipelineScheduler` already runs work on a thread pool. The worker’s main job was
  blocking on `future.get()` for preview tasks that only completed through the blocking promise
  path—so the port became a thin wrapper around an already-async executor, without relieving
  pressure on the render queue.
- `BindFrameSubmission` was invoked from the port (and specially for test producers via
  `EnsureSize`) even though production pipeline execution already binds submission under
  `PipelineTask::SetExecutorRenderParams`. Frame-sink binding should stay with the pipeline
  stage that knows output geometry.

### Test seams rewrote production logic

- `SetTestFrameProducer` forced production code paths to branch on “test vs real” and changed
  upper-layer behavior (`BindFrameSubmission`, size setup) so tests could pass. Test producers
  should simulate the production producer boundary, not force the production adapter to grow
  alternate control flow.

### Per-frame image pool lookup

- `TryProducePipelineFrame` re-resolved `EditorSessionSchedulerServices` (including image pool)
  and re-read image / input buffer identity on every frame instead of packaging a stable session
  render context when the image is opened or switched.

### Redundant cancel surfaces

- Cancel and replace exist at multiple layers: coordinator pending queue, string
  `replacement_key` comparison, intent cancellation tokens, and the port’s own scheduled/running
  job flags. The same request can be “cancelled” by several independent mechanisms.

### Overall assessment

The design stacked queues, threads, and reverse callbacks on top of an already-async pipeline
scheduler. Maintainability is poor; several paths are inefficient relative to the real requirements
(single-flight, quality ladder coalesce, epoch supersession, present handoff).

## Decision

Collapse to one coalesce owner and one execution owner:

```text
UI / session
  → EditorRenderCoordinator     // only coalesce + single-flight
  → thin production adapter     // build PipelineTask from session context
  → PipelineScheduler           // only execution pool
  → IFrameSink
```

Invariants:

1. **One pending-merge layer.** The coordinator is the only place that replaces/coalesces
   interactive / quality / detail work.
2. **One execution layer.** `PipelineScheduler` is the only thread pool for editor preview work.
   The session port must not own a second worker or second FIFO of jobs.
3. **Forward completion.** Task completion notifies the coordinator through a completion path
   established when the job is submitted (`EditorPipelineScheduleCompletion` on
   `IEditorPipelineSchedulerPort::Schedule`, fed by `PipelineTask::on_complete_`). The production
   adapter does not store a reverse `weak_ptr` to the coordinator.
4. **Pipeline owns frame binding.** Production `BindFrameSubmission` stays inside pipeline task
   setup / apply. Upper layers do not pre-bind “for convenience.”
5. **Session-scoped render inputs.** Image, input buffer, pipeline guard, and sink identity for
   the open image are bound when the image is acquired or switched—not re-fetched from the image
   pool on every interactive frame.
6. **Tests mock the seam; they do not rewrite production branches.** Prefer recording fakes for
   `IEditorPipelineSchedulerPort` or sink-level producers that look like production completion.

Explicit non-goals (YAGNI until measured need appears):

- a new general render “engine” or multi-inflight GPU dispatcher;
- merging the app-layer coordinator into `PipelineScheduler` (session tests still need a thin
  submit seam);
- rewriting the legacy QWidget `editor_dialog` render coordinator in the same change set;
- blanket smart-pointer conversion of QObject ownership graphs.

## Target call chain

Interactive adjustment:

```text
QML / models
  → EditorSessionService / EditorSessionRenderController
  → EditorRenderCoordinator::Submit
       · accept or replace slot
       · if idle, hand one request to the adapter
  → adapter builds PipelineTask (session context already holds image/pipeline/sink)
  → PipelineScheduler::ScheduleTask
  → configure under render lock + Apply + BindFrameSubmission + present
  → on_complete_(success)
  → Coordinator publishes FrameReady / Failed / Cancelled and schedules the next slot
```

Image switch:

```text
SetActiveImageLoadRequest(new_epoch)
  → drop pending slots for the old epoch
  → cancel in-flight via token
  → replace EditorRenderSessionContext
  → InitialFrame into the interactive slot
```

Cancel surfaces after simplification: **slot replace**, **cancellation token**, **image-load
epoch**. No string-key scans and no second port queue.

## Phases (one branch, sequential commits)

Work lands on `feature/editor_render_simplify` as sequential commits. Do not open one remote PR
per phase unless a later review asks for a stack.

### Phase R1 — Remove the port worker; complete via PipelineScheduler

**Goal:** delete the private worker thread and blocking `future.get()` bridge.

Delivered:

- `PipelineTask::on_complete_` invoked once on every terminal path in `ScheduleTask`
  (success, cancel, empty result, exception);
- `PipelineScheduler::ScheduleWork` for non-pipeline pool work (test producer, async setup
  failures);
- `EditorSessionRenderSchedulerPort` rewritten as a thin adapter: at most one `running_job_`,
  no `std::thread`, production path uses non-blocking `ScheduleTask` + `on_complete_`;
- destructor and `WaitForSessionIdle` still drain in-flight pool work and may pump GUI events
  during present handoff.

Still deferred after R1 (recorded as later-phase tasks):

- reverse `SetCoordinator` / `NotifySchedulerCompleted` → **Optional follow-up (completion
  direction)**;
- test-producer `BindFrameSubmission` branch → **Phase R4**;
- per-frame image pool reads → **Phase R3** (done 2026-08-07).

**Focused verification (R1):**

```text
EditorSessionRenderSchedulerPortTest
EditorRenderCoordinatorTest
PipelineSchedulerRequestIdTest
EditorSessionRenderControllerTest
```

Avoid relying on large UI e2e binaries known to hang in harness-only conditions that do not
reproduce in the interactive app.

### Phase R2 — Coordinator: fixed quality slots instead of string-keyed deque

**Goal:** O(1) replace and schedule selection for the three real ladder roles.

- [x] Replace `pending_` deque + `replacement_key` string equality with three optional slots:
  interactive, quality, detail (enum / quality-derived index).
- [x] Same-slot submit overwrites and emits `Replaced`.
- [x] `ScheduleNext` picks interactive > quality > detail in constant time.
- [x] Epoch mismatch clears all slots and cancels in-flight via token; no full-container rescans for
  coalesce.
- [x] Keep single-flight (`Submit` then schedule when idle; complete then schedule next).
- [x] Keep `ReasonReusesCurrentFrame` (ZoomPan / Resize do not schedule pipeline work).

##### Phase R2 completion record (2026-08-06)

**Status:** complete — fixed quality slots replace string-keyed pending deque in
`EditorRenderCoordinator`.

**Primary success call chain:**

```text
Submit(intent)
  -> FillRenderIntentDefaults / AcceptOrReject / ReasonReusesCurrentFrame
  -> PlaceInSlot(SlotIndexForQuality)  // overwrite same ladder slot + Replaced
  -> ScheduleNext when idle
       · ScrubPendingSlots (token / epoch, O(3))
       · SelectNextSlotIndex: interactive > quality > detail
  -> IEditorPipelineSchedulerPort::Schedule
  -> NotifySchedulerCompleted -> FrameReady -> ScheduleNext next slot
```

**Primary failure call chain:**

```text
SetActiveImageLoadRequest(new_epoch)
  -> CancelObsoleteForImageLoadMismatch
       · clear every occupied slot with epoch mismatch (Cancelled)
       · cancel in-flight token + emit Cancelled
  -> scheduler_->Cancel(job) outside mutex
  -> DeliverPendingResults
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `SameQualitySlotSubmitReplacesPriorPending` | `EditorRenderCoordinatorTest` | PASS |
| `ThreeQualitySlotsCanCoexistAndScheduleInteractiveFirst` | `EditorRenderCoordinatorTest` | PASS |
| `BurstOfReplaceableIntentsKeepsNewestInteractiveOnly` | `EditorRenderCoordinatorTest` | PASS |
| `SetActiveImageLoadRequestCancelsObsoletePendingAndInflight` | `EditorRenderCoordinatorTest` | PASS |
| `ZoomPanIntentIsReusedWithoutScheduling` / `ResizeIntentIsReusedWithoutScheduling` | `EditorRenderCoordinatorTest` | PASS |
| `SlotIndexMatchesQualityLadderOrder` | `EditorRenderCoordinatorTest` | PASS |
| Full coordinator suite | `EditorRenderCoordinatorTest` | PASS 30/30 |
| Render controller regression | `EditorSessionRenderControllerTest` | PASS 13/13 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorRenderCoordinatorTest
ctest --test-dir build/debug -R EditorRenderCoordinatorTest --output-on-failure
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionRenderControllerTest
ctest --test-dir build/debug -R EditorSessionRenderControllerTest --output-on-failure
```

Suite totals: `EditorRenderCoordinatorTest` 30/30; `EditorSessionRenderControllerTest` 13/13.

**Checklist / exit condition:** all R2 boxes checked.

**LOC note (grill-code-review):**

| File | LOC |
| --- | --- |
| `alcedo_studio/src/app/editor_render_coordinator.cpp` | ~491 |
| `alcedo_studio/src/include/app/editor_render_coordinator.hpp` | ~173 |
| `alcedo_studio/tests/app/editor_render_coordinator_test.cpp` | ~752 |

No responsibility split needed; coordinator still owns only coalesce + single-flight.

**Residual gaps (carried forward as later-phase tasks):**

- `replacement_key` still on `EditorRenderIntent` for producers / diagnostics; coalesce no longer
  uses string equality → **Optional follow-up (intent field cleanup)**.
- Session render context → **Phase R3** (done 2026-08-07).
- Test-producer production branches → **Phase R4**.

### Phase R3 — Session render context

**Goal:** bind stable inputs at open/switch; hot path does not re-resolve the image pool.

```text
EditorRenderSessionContext
  epoch, element_id, image_id
  Image, ImageBuffer (or lazy-once load), PipelineGuard handle
  presentation sink identity / resolver
```

- [x] Populate on successful image open / switch.
- [x] Adapter reads context only; image switch replaces the whole context under the new epoch.
- [x] Existing `cached_input_` becomes part of this context, not a separate half-cache.

##### Phase R3 completion record (2026-08-07)

**Status:** complete — session render context binds identity at open/switch; payload
loads once; hot path no longer re-resolves the image pool.

**Primary success call chain:**

```text
Open/Switch → ContinueToTarget
  -> ResetForNewImage → ClearSessionRenderContext
  -> RouteInitialRender
       · BindSessionRenderContext(epoch, element_id, image_id)
       · SetActiveImageLoadRequest(epoch)
       · Submit(intent)
  -> Coordinator → EditorSessionRenderSchedulerPort::Schedule
  -> EnsureContextForRequest
       · match bound identity
       · lazy-once load Image + ImageBuffer + PipelineGuard into context
  -> DispatchPipelineFrame builds PipelineTask from context only
  -> PipelineScheduler::ScheduleTask → on_complete_ → FrameReady
```

**Primary failure call chain:**

```text
Schedule without bind / with missing pool after identity-only bind
  -> EnsureContextForRequest fails (no pipeline / no pool / empty path)
  -> FinishJob(false, error) → NotifySchedulerCompleted
  -> Coordinator publishes Failed; next slot may schedule
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `BindSessionContextRecordsIdentityWithoutPoolRead` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `ClearSessionContextDropsBoundIdentity` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `ImageSwitchBindReplacesPriorContextPayload` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `InstalledContextAllowsScheduleWithoutImagePoolService` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `HotPathAfterInstalledContextDoesNotInvokeImagePoolResolver` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `BoundIdentityWithoutPayloadStillRejectsWhenPoolUnavailable` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `RouteInitialRenderBindsSessionContextAtOpen` | `EditorSessionRenderControllerTest` | PASS |
| `ResetForNewImageClearsSessionRenderContext` | `EditorSessionRenderControllerTest` | PASS |
| `BindAndClearSessionRenderContextForwardToScheduler` | `EditorRenderCoordinatorTest` | PASS |
| Full port suite | `EditorSessionRenderSchedulerPortTest` | PASS 11/11 |
| Full coordinator suite | `EditorRenderCoordinatorTest` | PASS 31/31 |
| Full render controller suite | `EditorSessionRenderControllerTest` | PASS 15/15 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionRenderSchedulerPortTest EditorRenderCoordinatorTest EditorSessionRenderControllerTest
ctest --test-dir build/debug -R "EditorSessionRenderSchedulerPortTest|EditorRenderCoordinatorTest|EditorSessionRenderControllerTest" --output-on-failure
```

Suite totals: `EditorSessionRenderSchedulerPortTest` 11/11; `EditorRenderCoordinatorTest` 31/31;
`EditorSessionRenderControllerTest` 15/15 (57/57 combined).

**Checklist / exit condition:** all R3 boxes checked. Acceptance criterion 6 (hot interactive
frames do not call image-pool after context bind) covered by
`HotPathAfterInstalledContextDoesNotInvokeImagePoolResolver`.

**LOC note (grill-code-review):**

| File | LOC (approx) |
| --- | --- |
| `editor_session_render_scheduler_port.hpp` | ~160 |
| `editor_session_render_scheduler_port.cpp` | ~560 |
| `editor_session_render_scheduler_port_test.cpp` | ~300 |
| `editor_session_render_controller.cpp` | ~480 |
| `editor_render_coordinator.cpp` / `.hpp` | modest Bind/Clear forwarders |

No responsibility split needed; context lives on the production adapter only.

**Residual gaps (carried forward as later-phase tasks — do not drop):**

| Residual | Owner phase / section |
| --- | --- |
| Test-producer `BindFrameSubmission` / `EnsureSize` still rewrites production `Dispatch` | **Phase R4** |
| Presentation sink still resolved every schedule via `sink_resolver_` (not session-context identity) | **Phase R4** (sink identity on context) or **Optional follow-up** if R4 scope is already full |
| Reverse `SetCoordinator` / `NotifySchedulerCompleted` still the completion plane | **Optional follow-up (completion direction)** (from R1) |
| `replacement_key` field still on `EditorRenderIntent` after slot coalesce | **Optional follow-up (intent field cleanup)** (from R2) |
| No interactive-app / real-RAW open-switch latency evidence for context bind + lazy-once load | **Optional follow-up (verification)** |
| First frame after bind still pays one payload load (lazy-once) | Accepted R3 design; only re-open if open latency measurement fails in verification follow-up |

##### Phase R3 re-land completion record (2026-08-07)

**Status:** complete — R3 re-applied after a temporary full revert used while fixing vsync /
frame-production desync. Restored commit content is the final R3 edition: session context bind
plus `ImageBuffer` shared encoded payload (`ShareEncodedBuffer`) so quality-ladder frames do not
deep-copy multi-MB RAW bytes.

**Why re-land:** R3 was reverted (`da95f574`) to isolate a present/vsync overlap issue. That
issue is addressed separately by deferring `ScheduleNext` on pipeline-pool completion
(`NotifySchedulerCompleted(..., schedule_next_from_pool=false)` + `Pump`), kept in the working
tree alongside this re-land. R3 itself is restored as commit `2d5aef7b` (revert of the revert of
`072d3024`).

**Primary success call chain:** unchanged from the 2026-08-07 R3 record above
(`RouteInitialRender` → bind → `EnsureContextForRequest` lazy-once →
`ShareEncodedBuffer` on Apply).

**Primary failure call chain:** unchanged (missing bind/pool → `FinishJob(false)` → Failed).

**What was proven (re-run after re-land):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `BindSessionContextRecordsIdentityWithoutPoolRead` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `ClearSessionContextDropsBoundIdentity` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `ImageSwitchBindReplacesPriorContextPayload` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `RebindSameImageIdentityKeepsPayloadWithoutReload` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `InstalledContextAllowsScheduleWithoutImagePoolService` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `HotPathAfterInstalledContextDoesNotInvokeImagePoolResolver` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `BoundIdentityWithoutPayloadStillRejectsWhenPoolUnavailable` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `RouteInitialRenderBindsSessionContextAtOpen` | `EditorSessionRenderControllerTest` | PASS |
| `ResetForNewImageClearsSessionRenderContext` | `EditorSessionRenderControllerTest` | PASS |
| `BindAndClearSessionRenderContextForwardToScheduler` | `EditorRenderCoordinatorTest` | PASS |
| Full port suite | `EditorSessionRenderSchedulerPortTest` | PASS 12/12 |
| Full coordinator suite | `EditorRenderCoordinatorTest` | PASS 31/31 |
| Full render controller suite | `EditorSessionRenderControllerTest` | PASS 15/15 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionRenderSchedulerPortTest EditorRenderCoordinatorTest EditorSessionRenderControllerTest
ctest --test-dir build/debug -R "EditorSessionRenderSchedulerPortTest|EditorRenderCoordinatorTest|EditorSessionRenderControllerTest" --output-on-failure
```

Suite totals: `EditorSessionRenderSchedulerPortTest` 12/12; `EditorRenderCoordinatorTest` 31/31;
`EditorSessionRenderControllerTest` 15/15 (58/58 combined).

**Checklist / exit condition:** all R3 boxes remain checked; acceptance criterion 6 re-proven.

**Residual gaps:** same as the original R3 residual table (R4 + optional follow-ups). Vsync /
present handoff deferral is outside R3 scope and remains local WIP diagnostics until committed
separately.

### Phase R4 — Test seams without production branches

**Goal:** remove `SetTestFrameProducer` special-casing from the production adapter, and close
residuals that force production branches or re-resolve presentation inputs on the hot path.

**Checklist (includes residuals carried from R1–R3):**

- [x] Remove `SetTestFrameProducer` and any production `if (test_producer_)` branch from
  `EditorSessionRenderSchedulerPort`.
- [x] Production adapter no longer calls `BindFrameSubmission` / `EnsureSize` for a test-only path
  (R1 residual; R3 residual).
- [x] Coordinator unit tests keep recording `IEditorPipelineSchedulerPort` fakes (no test producer
  on the production adapter).
- [x] Frame metadata / ready-frame tests target sink or pipeline completion without rewriting
  production `Dispatch` branches.
- [x] Session context (or bind path) records **presentation sink identity** so schedule does not
  re-resolve a live sink pointer as a substitute for session-scoped sink identity (R3 residual:
  today `sink_resolver_()` runs every `DispatchJob`). Prefer stamping `PresentationSinkId` at
  open/switch and resolving the pointer only when submitting to the pipeline / present path if a
  live `IFrameSink*` is still required.
- [x] Focused tests prove: (1) production `DispatchPipelineFrame` has no test-producer branch;
  (2) ready-frame metadata still correct without adapter-side `BindFrameSubmission`; (3) sink
  identity is stable across interactive frames for one bound session context.

**Exit condition:** all R4 boxes checked; acceptance criterion 5 satisfied.

##### Phase R4 completion record (2026-08-07)

**Status:** complete — removed production test-producer branches; stamped
`PresentationSinkId` on session context; shell/e2e harness moved to a test-only scheduler port.

**Primary success call chain:**

```text
RouteInitialRender
  -> BindSessionRenderContext(epoch, element, image, presentation_sink_id)
  -> EditorSessionRenderSchedulerPort::BindSessionContext stamps context.presentation_sink_id
  -> Coordinator::Submit -> Schedule
  -> DispatchJob
       · reject if intent sink id mismatches bound context
       · resolve live IFrameSink* only at submit (sink_resolve_count++)
  -> DispatchPipelineFrame (only path)
  -> PipelineTask::SetExecutorRenderParams -> BindFrameSubmission
  -> on_complete_ -> FrameReady
```

**Primary failure call chain:**

```text
Schedule with mismatched presentation_sink_id
  -> DispatchJob FinishJob(false, sink identity mismatch)
  -> no EnsureSize / no adapter BindFrameSubmission
  -> NotifySchedulerCompleted(Failed)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ProductionPipelinePathSchedulesInstalledContextWithoutAdapterBind` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `ScopeRefreshMarksFrameAsRequestedScopeInput` (metadata via pipeline bind) | `EditorSessionRenderSchedulerPortTest` | PASS |
| `SinkIdentityStableAcrossInteractiveFramesForBoundContext` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `MismatchedPresentationSinkIdentityFailsWithoutAdapterEnsureSize` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `BindAndClearSessionRenderContextForwardToScheduler` (sink id forwarded) | `EditorRenderCoordinatorTest` | PASS |
| `RouteInitialRenderBindsSessionContextAtOpen` (sink id stamped) | `EditorSessionRenderControllerTest` | PASS |
| Full port suite | `EditorSessionRenderSchedulerPortTest` | PASS 14/14 |
| Full coordinator suite | `EditorRenderCoordinatorTest` | PASS 31/31 |
| Full render controller suite | `EditorSessionRenderControllerTest` | PASS 15/15 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionRenderSchedulerPortTest EditorRenderCoordinatorTest EditorSessionRenderControllerTest
ctest --test-dir build/debug -R "EditorSessionRenderSchedulerPortTest|EditorRenderCoordinatorTest|EditorSessionRenderControllerTest" --output-on-failure
```

Suite totals: `EditorSessionRenderSchedulerPortTest` 14/14; `EditorRenderCoordinatorTest` 31/31;
`EditorSessionRenderControllerTest` 15/15 (60/60 combined).

**Checklist / exit condition:** all R4 boxes checked; acceptance criterion 5 satisfied
(production path does not pre-bind frame submission outside pipeline task setup).

**LOC note (grill-code-review):**

| File | LOC (approx) |
| --- | --- |
| `editor_session_render_scheduler_port.hpp` | ~127 |
| `editor_session_render_scheduler_port.cpp` | ~607 |
| `editor_session_render_scheduler_port_test.cpp` | ~359 |
| `harness_completing_pipeline_scheduler_port.hpp` (tests/support) | ~226 |
| `editor_render_coordinator.hpp` / `.cpp` | modest SetPipelineSchedulerPort + sink-id forward |

No responsibility split needed; harness lives only under `tests/support/`.

**Remaining gaps:** interactive-app / real-RAW open-switch latency evidence remains under
**Optional follow-up (verification)** (deferred). Completion direction and `replacement_key`
cleanup closed 2026-08-07.

#### Session context / sink (from R3, if not closed in R4)

- [x] Closed in R4: `PresentationSinkId` stamped on `EditorRenderSessionContext` at
  open/switch; live `IFrameSink*` resolved only at submit.

### Optional follow-up — residuals and ownership notes

Work below is sequenced after R4. Completion direction and intent-field cleanup are done
(2026-08-07). Interactive-app verification remains deferred by request.

#### Completion direction (from R1)

- [x] Carry completion on the submit path (`PipelineTask::on_complete_` / submit callback) so the
  production adapter no longer needs reverse `SetCoordinator` +
  `NotifySchedulerCompleted` via `weak_ptr` to the coordinator.
- [x] Delete reverse coordinator pointer from `EditorSessionRenderSchedulerPort` once the forward
  path is the only control plane.

#### Intent field cleanup (from R2)

- [x] Remove `EditorRenderIntent::replacement_key` (and `DefaultReplacementKey` / fill path) if no
  producer/diagnostic consumer still requires the string; slots already own coalesce.

#### Verification (from R3)

- [ ] Interactive-app or focused integration proof: open / switch a real RAW, confirm first-frame
  path binds context once, subsequent interactive frames do not hit image-pool `Read`, and
  open/switch latency stays acceptable with lazy-once payload load.
- [ ] If lazy-once first-frame load is too slow, promote payload load into
  `BindSessionContext` (eager bind) and re-measure; keep the single-load invariant.

**Deferred by request (2026-08-07 residual cleanup):** interactive-app / real-RAW open-switch
latency evidence is not part of this cleanup pass.

#### Raw pointer ownership notes (pre-existing)

- [ ] Document owner lifetimes for `EditorSessionController` bare pointers (`editor_`,
  `session_backend_`, `interaction_policy_`); keep `QPointer` for QML viewport objects.
- [ ] No mass ownership redesign unless a concrete use-after-free appears.

##### Optional residual cleanup completion record (2026-08-07)

**Status:** complete — forward Schedule completion + `replacement_key` removal; verification
intentionally skipped.

**Primary success call chain:**

```text
Coordinator::ScheduleNext
  -> IEditorPipelineSchedulerPort::Schedule(request, on_complete)
       · on_complete captures NotifySchedulerCompleted(..., schedule_next_from_pool=false) + Pump
  -> EditorSessionRenderSchedulerPort stores on_complete on Job (no coordinator weak_ptr)
  -> PipelineScheduler::ScheduleTask -> PipelineTask::on_complete_
  -> FinishJob -> CompleteJob invokes Schedule on_complete
  -> FrameReady / Failed / Cancelled + Pump next slot
```

**Primary failure call chain:**

```text
Schedule reject / pipeline empty / cancel
  -> FinishJob(success=false, message)
  -> on_complete(false, message)  // still forward-only
  -> NotifySchedulerCompleted -> Failed/Cancelled; Pump may start next slot
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ForwardScheduleCompletionDrivesFrameReadyWithoutReversePort` | `EditorRenderCoordinatorTest` | PASS |
| `ForwardScheduleCompletionInvokedWithoutReverseCoordinator` | `EditorSessionRenderSchedulerPortTest` | PASS |
| `SubmitDoesNotMutateStoredIntentAfterAccept` (no replacement_key) | `EditorRenderCoordinatorTest` | PASS |
| Full port suite | `EditorSessionRenderSchedulerPortTest` | PASS 15/15 |
| Full coordinator suite | `EditorRenderCoordinatorTest` | PASS 32/32 |
| Full render controller suite | `EditorSessionRenderControllerTest` | PASS 15/15 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionRenderSchedulerPortTest EditorRenderCoordinatorTest EditorSessionRenderControllerTest
ctest --test-dir build/debug -R "EditorSessionRenderSchedulerPortTest|EditorRenderCoordinatorTest|EditorSessionRenderControllerTest" --output-on-failure
```

Suite totals: port 15/15; coordinator 32/32; render controller 15/15 (62/62 combined).

**Checklist / exit condition:** completion-direction and intent-field boxes checked; verification
left unchecked by explicit request.

**LOC note (grill-code-review):**

| File | LOC (approx) |
| --- | --- |
| `editor_render_coordinator.hpp` | ~201 |
| `editor_render_coordinator.cpp` | ~600 |
| `editor_render_intent.hpp` | ~228 |
| `editor_session_render_scheduler_port.hpp` | ~130 |
| `editor_session_render_scheduler_port.cpp` | ~599 |
| `harness_completing_pipeline_scheduler_port.hpp` | ~224 |

No responsibility split needed; harness no longer owns a reverse notifier plane.

**Remaining gaps:** interactive-app verification (deferred); raw-pointer ownership notes
(documentation-only, pre-existing).

## Acceptance criteria

1. Interactive burst: many submits while one job is in flight leave at most one pending
   interactive slot and one in-flight job; the final ready frame matches the latest accepted
   interactive intent.
2. Image-load epoch change: stale `FrameReady` does not advance first-frame or interactive state.
3. ZoomPan / Resize: `Reused` result, zero `ScheduleTask` for that intent.
4. No private `std::thread` inside the session render scheduler port.
5. Production path does not pre-bind frame submission outside pipeline task setup (after R4).
6. Hot interactive frames do not call image-pool `Read` after context bind (after R3).
7. History head-move `WaitForSessionIdle` still completes without deadlocking present handoff on
   the GUI thread (processEvents remains allowed at that boundary).

## Key decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| How many queues? | One coalesce layer (coordinator) | Second queue was only a blocking adapter artifact |
| Who runs pipeline work? | `PipelineScheduler` only | Already owns the pool and cancel checks |
| Completion direction | Task `on_complete_` + Schedule callback forward | Avoids a second control plane; reverse weak_ptr removed |
| Coalesce key | Fixed interactive / quality / detail slots | Matches real ladder; string keys add nothing |
| Image handles | Session context at open/switch | Avoid per-frame pool lookups |
| Tests | Mock port or sink boundary | Do not grow production if-test branches |
| Delivery shape | One feature branch, sequential commits | Easier to land and bisect than four parallel PRs |

## Progress log

### 2026-08-06 — Phase R1 complete

Branch: `feature/editor_render_simplify`.

Code:

- `alcedo_studio/src/include/renderer/pipeline_task.hpp` — `on_complete_`;
- `alcedo_studio/src/include/renderer/pipeline_scheduler.hpp` / `.cpp` — `on_complete_` on all
  terminal paths; `ScheduleWork`;
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp`
  and `.cpp` — worker and second queue removed; async completion.

Focused tests (all passed):

- `EditorSessionRenderSchedulerPortTest` 5/5
- `EditorRenderCoordinatorTest` 28/28
- `PipelineSchedulerRequestIdTest` 4/4
- `EditorSessionRenderControllerTest` 13/13

Next: Phase R2 (fixed slots).

### 2026-08-06 — Phase R2 complete

Branch: `feature/editor_render_simplify`.

Code:

- `alcedo_studio/src/include/app/editor_render_coordinator.hpp` — `QualitySlot` / `slots_` array;
  `SlotIndexForQuality`, `PlaceInSlot`, `SelectNextSlotIndex`, `ScrubPendingSlots`;
- `alcedo_studio/src/app/editor_render_coordinator.cpp` — deque + `ReplacePendingWithKey` /
  `SelectNextIndex` removed; cancel/schedule walk three fixed slots only.

Focused tests (all passed):

- `EditorRenderCoordinatorTest` 30/30 (includes three-slot order + 100-burst + slot index)
- `EditorSessionRenderControllerTest` 13/13

Next: Phase R3 (session render context).

### 2026-08-07 — Phase R3 complete

Branch: `feature/editor_render_simplify`.

Code:

- `EditorRenderSessionContext` on the production scheduler port (epoch, element, image,
  Image, ImageBuffer, PipelineGuard); replaces `cached_input_` half-cache;
- `BindSessionContext` / `ClearSessionContext` on `IEditorPipelineSchedulerPort` and
  `BindSessionRenderContext` / `ClearSessionRenderContext` on `IEditorRenderSubmitPort`;
- `EditorSessionRenderController::RouteInitialRender` binds at open/switch;
  `ResetForNewImage` clears;
- `DispatchPipelineFrame` / `CanProduceFrame` use bound context; payload loads once via
  `EnsureContextForRequest`;
- `ImageBuffer` encoded payload held as `shared_ptr`; `ShareEncodedBuffer()` aliases multi-MB
  RAW bytes across session source and per-Apply working buffers (no deep copy on ladder frames).

Focused tests (all passed):

- `EditorSessionRenderSchedulerPortTest` 11/11 (later 12/12 after rebind-identity case)
- `EditorRenderCoordinatorTest` 31/31
- `EditorSessionRenderControllerTest` 15/15

### 2026-08-07 — Phase R3 temporarily reverted, then re-landed

Branch: `feature/editor_render_simplify`.

- Temporary full revert of R3 (`da95f574`) while isolating vsync / frame-production desync.
- R3 final edition restored (`2d5aef7b`, revert of the revert of `072d3024`): session context +
  shared encoded RAW bytes.
- Present/vsync handoff deferral (`schedule_next_from_pool=false` on pool completion) kept as
  local WIP with R3, not part of the R3 commit itself.

Re-verified focused suites: port 12/12, coordinator 31/31, render controller 15/15 (58/58).

### 2026-08-07 — Phase R4 complete

Branch: `feature/editor_render_simplify`.

Code:

- Removed `SetTestFrameProducer` / `DispatchTestProducer` from
  `EditorSessionRenderSchedulerPort`; production `Dispatch` is pipeline-only;
- `EditorRenderSessionContext::presentation_sink_id` stamped at
  `BindSessionContext` / `RouteInitialRender`; live sink resolved only at submit;
- Test-only `HarnessCompletingPipelineSchedulerPort` under `tests/support/` for
  shell/e2e frame completion; coordinator `SetPipelineSchedulerPort` swaps the seam;
- Port metadata tests assert pipeline `BindFrameSubmission` without adapter
  `EnsureSize`.

Focused tests (all passed):

- `EditorSessionRenderSchedulerPortTest` 14/14
- `EditorRenderCoordinatorTest` 31/31
- `EditorSessionRenderControllerTest` 15/15

Next: Optional follow-ups (completion direction, `replacement_key` cleanup,
interactive-app verification).

### 2026-08-07 — Optional residual cleanup complete

Branch: `feature/editor_render_simplify`.

Code:

- `IEditorPipelineSchedulerPort::Schedule` carries `EditorPipelineScheduleCompletion`;
  coordinator installs forward completion at `ScheduleNext` (no reverse port pointer);
- Removed `EditorSessionRenderSchedulerPort::SetCoordinator` / `coordinator_` weak_ptr;
- Harness uses Schedule `on_complete` only (no `SetCompletionNotifier`);
- Removed `EditorRenderIntent::replacement_key` / `DefaultReplacementKey` / fill path.

Focused tests (all passed):

- `EditorSessionRenderSchedulerPortTest` 15/15
- `EditorRenderCoordinatorTest` 32/32
- `EditorSessionRenderControllerTest` 15/15

Deferred: interactive-app / real-RAW verification follow-up (by request).
