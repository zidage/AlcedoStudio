# Editor Render Path Simplification Plan

Date: 2026-08-06

Status: Phase R1 complete on branch `feature/editor_render_simplify`. Phases R2–R4 pending.
Delivery is one feature branch with sequential commits (not one GitHub PR per phase).

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
   established when the job is submitted (prefer `PipelineTask::on_complete_`). Long term, remove
   the port’s stored reverse `weak_ptr` to the coordinator if the submit interface can carry the
   callback instead.
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

Still deferred in R1:

- reverse `SetCoordinator` / `NotifySchedulerCompleted` (works; remove when submit API carries
  completion);
- test-producer `BindFrameSubmission` branch (kept behind `SetTestFrameProducer` until R4);
- per-frame image pool reads (R3).

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

- Replace `pending_` deque + `replacement_key` string equality with three optional slots:
  interactive, quality, detail (enum / quality-derived index).
- Same-slot submit overwrites and emits `Replaced`.
- `ScheduleNext` picks interactive > quality > detail in constant time.
- Epoch mismatch clears all slots and cancels in-flight via token; no full-container rescans for
  coalesce.
- Keep single-flight (`Submit` then schedule when idle; complete then schedule next).
- Keep `ReasonReusesCurrentFrame` (ZoomPan / Resize do not schedule pipeline work).

### Phase R3 — Session render context

**Goal:** bind stable inputs at open/switch; hot path does not re-resolve the image pool.

```text
EditorRenderSessionContext
  epoch, element_id, image_id
  Image, ImageBuffer (or lazy-once load), PipelineGuard handle
  presentation sink identity / resolver
```

- Populate on successful image open / switch.
- Adapter reads context only; image switch replaces the whole context under the new epoch.
- Existing `cached_input_` becomes part of this context, not a separate half-cache.

### Phase R4 — Test seams without production branches

**Goal:** remove `SetTestFrameProducer` special-casing from the production adapter.

- Coordinator unit tests keep recording `IEditorPipelineSchedulerPort` fakes.
- Frame metadata / ready-frame tests target sink or pipeline completion without rewriting
  production `Dispatch` branches.
- Production adapter no longer calls `BindFrameSubmission` / `EnsureSize` for a test-only path.

### Optional follow-up — Raw pointer ownership notes

Document owner lifetimes for `EditorSessionController` bare pointers; keep `QPointer` for QML
viewport objects. No mass ownership redesign unless a concrete use-after-free appears.

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
| Completion direction | Task `on_complete_` forward | Avoids a second control plane; reverse weak_ptr is transitional |
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
