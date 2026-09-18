# Move thumbnail and analysis disk writes into the disk cache service

Date: 2026-08-30

Status: Open; design discussion pending.

GitHub issue: [#113](https://github.com/zidage/AlcedoStudio/issues/113).

Repository: `zidage/AlcedoStudio`.

## Purpose and scope

The disk cache service must control disk writes for thumbnail and analysis images.
The scheduler callback must not read the live pipeline to decide whether to write an image.
The callback must supply the result and the required request data.

This issue concerns final images stored on disk.
It does not concern GPU textures, intermediate image results, or scratch memory.

**Discuss and implement this change separately from NM1.4R.**
This issue does not block NM1.5.
The service API and its data model remain open for discussion.

## Terms

- **Analysis image:** a rendered image that the application supplies to image analysis.
- **Live pipeline:** the shared document and executor for one image.
- **HEAD:** the current committed position in the edit history.
- **Commit label:** the identifier for the committed edit state that corresponds to an image result.
- **Cache key:** the identifier that the disk cache uses to store and find an image.
- **Pin:** a count that keeps a pipeline available while a task uses it.
- **Callback:** a function that receives a task result or a completion event.
- **Invalidation:** an operation that marks a cached result or pending write as no longer valid.
- **Scratch memory:** temporary memory that an image operation uses.

## Background

The NM1.4C review on 2026-08-30 found disk write decisions in callbacks that ThumbnailService registers with PipelineScheduler.

The uncommitted code adds `EnqueueDiskWriteIfCommitLabelMatches`.
This function reads HEAD, `dirty_`, and `unsettled_preview_` from the live pipeline.
It then decides whether to call `EnqueueWrite`.

These checks attempt to prevent image pixels from using the wrong commit label.
However, the callback reads the state after rendering.
That state does not identify the state that produced the pixels.

The user confirmed these operating conditions:

- Editor changes do not occur during thumbnail updates or exports.
- Export tasks and thumbnail tasks can run at the same time.

Do not add support for concurrent editing to solve this issue.
Do not add frozen document copies or exports from arbitrary history positions.

## Required responsibility

The disk cache service must determine whether a result can be written.
It must also control invalidation and completion of disk writes.

Moving the existing checks into a service function is insufficient if that function still reads the live executor or editor state.
Define the required input at the service boundary instead.

Keep the existing cancellation and invalidation mechanisms.
Do not add document copies, publication tokens, a second history model, or a general coordination framework.

## Questions for the design

1. Which application entry point supplies the correct commit label for each result?
2. How does that label work with existing cache keys, cancellation, and invalidation?
3. How does the service prevent an invalid result from returning to the disk index during a write?
4. Can thumbnails and analysis images use the same mechanism while keeping separate cache namespaces and cancellation?
5. Should the service remove `dirty_` from the conditions that permit disk writes?
6. Which fields and helper functions become unnecessary after the service owns this mechanism?

The `dirty_` field indicates changes that storage does not yet contain.
It does not, by itself, identify an uncommitted preview.

The change added `unsettled_preview_` and `SyncUnsettledPreviewFlag` to copy preview state for these checks.
Remove this copied state if no other operation needs it after the design changes.

A database label can be out of date.
Do not use it instead of the label that corresponds to the rendered pixels.

## Existing test evidence

The review ran 57 focused tests on 2026-08-30.

| Result | Count |
| --- | ---: |
| Passed | 54 |
| Failed | 2 |
| Timed out | 1 |

The two failed tests check pin counts.
Both passed when run separately again.
NM1.4R tracks their completion synchronization problems.

This issue tracks `PipelineSharedUseTest.QueuedRenderDoesNotStorePixelsUnderStaleCommitLabel`.
That test timed out after 90 seconds.

The test requests the same thumbnail key twice.
It does not invalidate the first image in the memory cache before the second request.
The second request can return that cached image without adding a render task.
The test then waits without a time limit for the pipeline pin count to increase.
It also reads that non-atomic count without the required synchronization.

Thus, this test does not prove the intended ordering or correct use of commit labels.

Test the new behavior at the disk cache service boundary.
Use barriers or a controllable executor to establish the execution order.
Do not use sleeps, unlimited polling, or a longer timeout as the fix.

## Acceptance criteria

- [ ] The disk cache service controls permission to write, invalidation, and completion of disk writes.
- [ ] Scheduler callbacks do not read live HEAD, dirty state, or preview state to decide whether to write.
- [ ] The service writes valid results under the correct keys and can read them again.
- [ ] The service does not reject all writes as a substitute for correct behavior.
- [ ] Canceled, invalid, or obsolete results do not enter the disk index.
- [ ] Tests cover separate thumbnail and analysis namespaces, concurrent writes, write failures, and resource release.
- [ ] Tests use a controlled execution order.
- [ ] The change removes unnecessary copied state, helper functions, and tests that only repeat implementation details.
- [ ] The change preserves internal GPU cache behavior, RAW quality, and the number of parallel background tasks.

## Related code and plans

- `alcedo_studio/src/app/thumbnail_service.cpp`: `BuildDiskCacheKey`, `RenderedCommitLabel`, `EnqueueDiskWriteIfCommitLabelMatches`, and result callbacks.
- `alcedo_studio/src/app/thumbnail_disk_cache_service.cpp`, its header, and its tests.
- `alcedo_studio/src/include/app/thumbnail_types.hpp`: `ThumbnailDiskCacheWriteAllowed`.
- `alcedo_studio/src/include/app/pipeline_service.hpp`: `unsettled_preview_`.
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp`: `SyncUnsettledPreviewFlag`.
- `alcedo_studio/tests/app/pipeline_shared_use_test.cpp`: the existing test for commit labels.
- `docs/roadmap/alcedo_studio/edit/node_mask_editor/phase_nm1_pipeline_document_editing_plan.md`.
- `docs/roadmap/alcedo_studio/edit/node_mask_editor_master_plan.md`.
