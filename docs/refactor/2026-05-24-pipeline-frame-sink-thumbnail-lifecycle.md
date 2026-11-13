# Pipeline Frame Sink and Thumbnail Lifecycle Refactor

Date: 2026-05-24

## Background

Commit `cf659da` fixed a crash observed around
`CPUPipelineExecutor::SetNextFramePresentationMode()` by detaching the editor frame sink when
`EditorFrameManager` is destroyed. This addresses one concrete dangling-pointer path, but the
underlying ownership model is still awkward:

- `CPUPipelineExecutor` instances are cached by `PipelineMgmtService` per sleeve element.
- `CPUPipelineExecutor` also stores a raw `IFrameSink*`.
- The editor frame sink is only valid while an editor session is open.
- Thumbnail and export tasks may reuse the same cached or pinned pipeline executor.

That means a cached image pipeline can retain UI output state from an editor session, and thumbnail
rendering can accidentally interact with an editor-owned sink. This is inconsistent with the
intended architecture.

Separately, thumbnail zoom-tier behavior currently mixes two different identities:

- Cache and pending entries are keyed by `(element_id, resolution)`.
- Cancellation, active flags, pin tracking, release, and invalidation are mostly keyed only by
  `element_id`.

This coarse cancellation model can cancel or invalidate work for a thumbnail request that is still
visible after a zoom-level change.

## Target Semantics

The desired album behavior is:

- The editor preview output target is session-scoped, not a durable part of a per-image pipeline.
- Thumbnail rendering is independent from editor frame presentation.
- Thumbnail cache entries are keyed by the actual thumbnail request identity.
- Zoom-level changes may release old tiers and request new tiers, but must not cancel the currently
  visible request.
- Cache pressure should evict unpinned thumbnail entries first.
- Pinned visible entries must survive cache resizing and LRU eviction.
- Content changes, import/delete, or edit commits may invalidate all tiers for an element.
- View recycling or zoom changes should only cancel/release the specific stale request.

## Phase 1: Decouple Frame Sink From Cached Pipelines

### Problem

`CPUPipelineExecutor` currently owns a raw `frame_sink_` pointer and forwards presentation metadata
through it. Since executors are cached by `PipelineMgmtService`, this makes a UI object lifetime
depend on a cached per-image pipeline lifetime.

This is especially fragile when:

- An editor opens and attaches a frame sink to a pipeline.
- The same element's pipeline remains pinned or cached.
- A thumbnail task loads or reuses that pipeline.
- The thumbnail path calls `SetNextFramePresentationMode()` or preview metadata setters.
- The editor frame sink has already been destroyed or replaced.

### Intended Design

Treat `IFrameSink` as an output target for a render invocation or editor session, not as persistent
pipeline state.

Preferred direction:

- Remove durable frame sink ownership from `CPUPipelineExecutor`.
- Pass an optional frame sink through render task options or an editor-preview render context.
- Thumbnail and export tasks should explicitly have no frame sink.
- Editor preview tasks should explicitly provide the current editor sink.
- GPU stage frame-sink attachment should be derived from the current render invocation, not from
  cached executor state.

Short-term hardening if the full decoupling is too large:

- Add a single lock-protected attach/detach path for frame sinks.
- Require all attach, detach, viewer replacement, scope-panel replacement, and executor replacement
  paths to hold `CPUPipelineExecutor::GetRenderLock()`.
- Detach any previous executor before attaching a new one.
- Ensure thumbnail tasks detach or bypass any existing sink before rendering.
- Keep `PipelineMgmtService::SavePipeline()` sink reset as a fallback, not as the primary lifetime
  mechanism.

### Acceptance Criteria

- Thumbnail rendering cannot call into an editor-owned `IFrameSink`.
- Closing the editor while preview work is in flight cannot leave a dangling sink pointer.
- Reopening the editor or importing history cannot mutate execution stages concurrently with render.
- A cached pipeline can be reused for thumbnail/export/editor without carrying stale UI output state.

## Phase 2: Make Thumbnail Lifecycle Request-Key Based

### Problem

The service already models cache entries and pending callbacks with `ThumbnailCacheKey`:

```cpp
struct ThumbnailCacheKey {
  sl_element_id_t element_id;
  ThumbnailResolution resolution;
};
```

However, several lifecycle controls still operate at `element_id` granularity:

- `CancelPending(element_id)` cancels all resolution tiers.
- `ReleaseThumbnail(element_id)` cancels all pending tiers and releases all cached tiers.
- `InvalidateThumbnail(element_id)` removes all tiers.
- `ThumbnailManager::thumbnail_pins_` tracks one pin state per element.
- `thumbnail_active_flags_` tracks one active flag per element.

This is too coarse for zoomable thumbnail grids. When zoom changes, a delegate may release an old
tier and request a new tier for the same element. Element-wide cancellation can invalidate the new
request even though it is the one currently visible.

### Intended Design

Promote the request identity through the whole thumbnail lifecycle.

At minimum, use:

```cpp
using ThumbnailRequestKey = ThumbnailCacheKey;  // element_id + resolution
```

If exact display size matters within a tier, extend the UI-side request identity to include
`image_id` and `max_edge`:

```cpp
struct ThumbnailRequestIdentity {
  sl_element_id_t element_id;
  image_id_t image_id;
  ThumbnailResolution resolution;
  uint32_t requested_max_edge;
};
```

Recommended service API changes:

- Add `CancelPending(ThumbnailCacheKey key)`.
- Add `ReleaseThumbnail(ThumbnailCacheKey key)`.
- Keep `InvalidateThumbnail(element_id)` for content-level invalidation across all tiers.
- Keep an explicit `CancelAllPendingForElement(element_id)` only for delete/import/editor-reset
  scenarios where all tiers are truly stale.
- Track generation tokens per `ThumbnailCacheKey`, not only per `element_id`.
- Track active flags per request identity, not only per `element_id`.
- Track visible pins per request identity, or at least per `(element_id, resolution)`.

### Zoom Behavior

On zoom-level change:

1. Release the old visible request key.
2. Mark only the old request key inactive.
3. Request the new visible request key.
4. Keep UI loading state for the new key until it either succeeds or fails for a real reason.
5. Do not map cancellation to "missing source".

When a delegate is recycled:

1. Release only the request key previously pinned by that delegate.
2. Do not invalidate all tiers for the element.
3. Do not cancel a newer request for the same element and a different tier.

When an edit is committed, the source file changes, or the item is deleted:

1. Invalidate all tiers for that element.
2. Cancel all pending requests for that element.
3. Clear UI thumbnail state for that element.

### Error-State Cleanup

The current UI red exclamation state is driven by `thumbMissingSource`. Cancellation should not
surface as missing source. The thumbnail state should distinguish at least:

- `loading`
- `ready`
- `cancelled/stale`
- `missing_source`
- `render_error`

Until detailed error reporting is available, cancellation should normally return to an idle/loading
state for the current request, not to a missing-source state.

### Acceptance Criteria

- Changing zoom level does not produce red missing-source indicators for still-visible files.
- Releasing an old tier cannot cancel a newer tier for the same element.
- Cache eviction preserves pinned request keys.
- `InvalidateThumbnail(element_id)` remains available for real content invalidation.
- Tests cover rapid zoom changes, delegate recycling, and concurrent old/new tier requests for the
  same element.

## Suggested Implementation Order

1. Harden the current frame sink attach/detach paths under `render_lock_`.
2. Add request-key overloads to thumbnail cancel/release APIs.
3. Move thumbnail active flags and pin tracking from element-level to request-key-level.
4. Update QML/C++ delegate release paths to pass the exact old request key.
5. Split cancellation from missing-source UI state.
6. Remove durable `frame_sink_` state from cached pipelines, or restrict it to an invocation-scoped
   render context.

## Phase 1 Review: 2026-05-24

Reviewed commit `63258c0b` (`feat: Enhance pipeline frame sink management and add unit tests`).

### Current Status

Phase 1 is partially complete. The implementation follows the short-term hardening path rather
than the preferred full decoupling path.

Improvements that are in place:

- `EditorFrameManager` detaches the current frame sink under `render_lock_` during destruction.
- `SetViewer()` and `SetScopePanel()` detach the current sink before replacing the viewer or scope
  panel.
- `AttachExecutionStages()` detaches any previous executor before attaching a new one.
- Editor-side attach now happens under `render_lock_`.
- Thumbnail and export tasks temporarily detach any attached frame sink before rendering, so their
  render parameter setup does not call into an editor-owned sink.
- Unit tests were added around detached sinks, reset behavior, and basic lock/deadlock scenarios.

This is a meaningful stabilization pass and should reduce the original dangling-sink crash risk.
However, it does not yet make frame sink ownership clean.

### Findings

1. `PipelineScheduler::restore_frame_sink()` is not exception-safe.

   In `alcedo_studio/src/renderer/pipeline_scheduler.cpp`, thumbnail/export tasks save the current
   sink, detach it, render, then restore it on normal exits. If `SetExecutorRenderParams()`,
   `Apply()`, `GetCPUData()`, or `apply_state_transition_after_render()` throws after detaching,
   the outer catch path does not restore the sink. An open editor can silently lose its frame sink
   after a thumbnail/export failure on the same pinned executor.

   Recommended fix: use an RAII scope guard or otherwise guarantee restoration from every exit path
   after a temporary detach.

2. The implementation still mutates cached pipeline state for thumbnail/export rendering.

   Thumbnail/export tasks temporarily call `DetachFrameSink()` and later
   `SetExecutionStages(saved_frame_sink)`. This prevents those tasks from calling the editor sink
   during the render, but the cached or pinned `CPUPipelineExecutor` still carries UI output state
   before and after the task.

   This satisfies part of the short-term hardening goal, but it does not satisfy the stronger
   target that "a cached pipeline can be reused for thumbnail/export/editor without carrying stale
   UI output state."

3. `CPUPipelineExecutor::GetFrameSink()` exposes the raw sink pointer without encoding the locking
   requirement.

   The current scheduler use is under `render_lock_`, but the public API makes future unlocked
   reads easy. If this accessor stays, it should be documented as requiring `render_lock_`, or
   replaced by a locked helper that performs the specific temporary-detach operation needed by
   thumbnail/export.

### Phase 1 Acceptance Assessment

- Thumbnail rendering cannot call into an editor-owned `IFrameSink`: mostly satisfied for normal
  thumbnail/export render paths, but implemented through temporary mutation of cached executor
  state.
- Closing the editor while preview work is in flight cannot leave a dangling sink pointer:
  materially improved by lock-protected detach paths.
- Reopening the editor or importing history cannot mutate execution stages concurrently with
  render: improved for editor attach paths, but the lower-level `SetExecutionStages()` API remains
  publicly callable without lock enforcement.
- A cached pipeline can be reused for thumbnail/export/editor without carrying stale UI output
  state: not fully satisfied. The executor still stores `frame_sink_`; thumbnail/export now bypass
  it temporarily rather than eliminating the cached UI state.

### Next Steps

1. Make scheduler temporary sink restoration exception-safe.
2. Avoid exposing unlocked raw sink access where possible.
3. Treat the current implementation as a hardening patch, not as the final Phase 1 design.
4. Complete Phase 1 by moving frame sink selection to render invocation/editor session state, or by
   adding a narrow executor API that temporarily renders without a sink without rebuilding execution
   stages or exposing raw sink pointers.

## Phase 2 Implementation Note: 2026-05-24

Phase 2 was implemented with request-key scoped thumbnail lifecycle management.

Implemented changes:

- `ThumbnailService` now has key-scoped `CancelPending(ThumbnailCacheKey)` and
  `ReleaseThumbnail(ThumbnailCacheKey)` APIs.
- Thumbnail generation tokens are keyed by `(element_id, resolution)` instead of only
  `element_id`, so cancelling one zoom tier no longer invalidates another tier for the same
  element.
- Element-scoped `CancelPending(element_id)`, `ReleaseThumbnail(element_id)`, and
  `InvalidateThumbnail(element_id)` remain available for content-level teardown, delete, import, or
  edit-commit invalidation.
- `ThumbnailManager` now tracks visible pins and active async flags by `ThumbnailCacheKey`.
- Zoom/recycle paths release only the stale request key instead of cancelling and invalidating all
  tiers for the element.
- Content replacement paths still invalidate all tiers for the affected element.
- A service regression test verifies that releasing one resolution key does not release another
  cached/pinned tier for the same element.

Verification run:

- `git diff --check`
- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ThumbnailServiceTest --parallel 4`
- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumBackendThumbnailTest --parallel 4`
- `build\debug\alcedo_studio\tests\ThumbnailServiceTest.exe --gtest_filter=ThumbnailServiceTests.CacheKeySeparatesResolutions`
- `ctest --test-dir build/debug --output-on-failure -R "AlbumBackendThumbnailTest.*VisibleThumbnailRerequestsWhenMaxEdgeChanges"`

## Phase 1 Follow-up: 2026-05-24 (same day)

### Completed

1. **Exception-safe sink restoration** (Finding 1):
   - Added `CPUPipelineExecutor::AttachFrameSink(IFrameSink*)` — lightweight inverse of
     `DetachFrameSink()` that sets the sink without rebuilding execution stages.
   - Replaced the `restore_frame_sink` call to use `AttachFrameSink()` instead of
     `SetExecutionStages()` (which was unnecessarily expensive).
   - Added an RAII scope guard in `PipelineScheduler::ScheduleTask()` that guarantees sink
     restoration on every exit path, including the outer `catch(...)` block.
   - The scope guard uses `std::unique_ptr` with a non-null sentinel to ensure the deleter is
     always invoked.

2. **Locked raw sink access** (Finding 3):
   - Documented `GetFrameSink()` as requiring `render_lock_` in the header.
   - Documented `AttachFrameSink()` as requiring `render_lock_`.

3. **Narrow executor API** (Next Step 4, partial):
   - `AttachFrameSink()` provides a targeted API for re-attaching a sink without rebuilding
     stages, replacing scheduler calls to `SetExecutionStages(saved_frame_sink)`.
   - This is a meaningful improvement: `SetExecutionStages()` rebuilds the entire execution stage
     vector; `AttachFrameSink()` just sets two pointers.

4. **New tests** (6 new test cases in `pipeline_frame_sink_test.cpp`):
   - `AttachFrameSinkSetsPointerWithoutRebuildingStages`
   - `AttachDetachRoundTripWithoutStageRebuild`
   - `DetachThenAttachPreservesSinkCalls`
   - `SinkIsRestoredAfterExceptionDuringRender`
   - `SinkIsRestoredAfterExceptionBeforeRender`
   - `SinkIsNotRestoredIfNeverDetached`

### Updated Phase 1 Acceptance Assessment

- **Thumbnail rendering cannot call into an editor-owned `IFrameSink`**: satisfied via temporary
  detach; now also exception-safe.
- **Closing the editor while preview work is in flight cannot leave a dangling sink pointer**:
  improved by lock-protected detach paths; unchanged from prior assessment.
- **Reopening the editor or importing history cannot mutate execution stages concurrently with
  render**: unchanged from prior assessment (`SetExecutionStages()` remains publicly callable
  without lock enforcement).
- **A cached pipeline can be reused for thumbnail/export/editor without carrying stale UI output
  state**: improved — `AttachFrameSink` provides a lightweight sink swap without rebuilding
  stages, but `frame_sink_` remains durable executor state. The preferred full decoupling
  (making `IFrameSink` invocation-scoped) is deferred.

### Remaining

The `frame_sink_` member still persists on cached `CPUPipelineExecutor` instances. Full Phase 1
completion would move frame sink binding from cached executor state to per-invocation render
context (e.g., passing an optional `IFrameSink*` through `PipelineTask::options_` or a dedicated
render context struct). The current hardening is sufficient to prevent the concrete crash paths,
but future thumbnail/editor interactions may benefit from the cleaner model.

## Resolution: 2026-09-29

**Superseded by 2026-09-27 executor ownership refactor**
([plan](2026-09-27-executor-ownership-refactor-plan.md), P3–P7). The remaining item above is
closed. The problem it tracked was that executors cached per image by `PipelineMgmtService` kept an
editor sink that thumbnail and export renders could reach. That cannot happen any more:

- `PipelineMgmtService` owns no executor. `PipelineGuard` and the per-image executor cache are
  deleted.
- Thumbnail, analysis, and export render on batch executors that their services own
  (`ThumbnailService` pool of 2, `ExportService` 1). Nothing attaches a frame sink to them, and a
  batch executor rejects interactive requests.
- Only the editor's interactive executor, owned by `EditorSessionRenderSchedulerPort`, has a
  `frame_sink_`. The port attaches the viewport sink in each task's `prepare_` on the single
  editor worker, and changes it only when the viewport item is replaced.
- Execution stages no longer exist. Each render receives an immutable `PipelineGraphSnapshot`, so
  reopening the editor or importing history cannot change what a render in progress reads.

`frame_sink_` stays a member of the executor instead of moving into a per-invocation render
context. With one owner and one worker per executor, per-invocation binding would add no safety.
