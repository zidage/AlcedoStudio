# Editor Adjustment and Version Comparison Plan

Date: 2026-10-01
Status: Phase 1 complete (2026-10-01, branch `feature/editor-comparison-inputs`); Phase 2 complete on CUDA (2026-10-01, branch `feature/editor-comparison-pair-render`); Phase 3 partial (2026-10-01, branch `feature/editor-comparison-image-presentation`: implemented and tested; the manual pointer and focus check waits for the Phase 4 entry action); Phase 4 partial (2026-10-01, branch `feature/editor-comparison-entry-and-restore`: implemented and tested; the manual check in the real application is open); Phase 5 planned. Phase 5 proves the Phase 2 image job on OpenCL and Metal.
Source revision: `deeb8901881b5aae41680299f75b735ed4d01c3c` on `main`.
Parent plan: none. This is a five-phase feature plan.

## 1. Confirmed product decisions

The editor must show two adjustment states of the same image without changing its working document or active Version.

The user confirmed these requirements:

1. Before/After uses the image's imported root and the working values captured when comparison opens.
2. Version comparison permits two named Versions of the open image.
3. The active Version uses those captured working values. Other Versions use their saved heads.
4. Both inputs use the current demosaic, highlight reconstruction, and lens settings.
5. Each input retains its own white balance. Geometry, Color Grades, Masks, DRT, and post-processing also come from that input.
6. The existing editor executor and its single scheduler worker render A and B consecutively.
7. Each render uses full RAW decode and the Quality Base long-edge limit of 4096 pixels.
8. The pair appears together after both renders succeed and both Qt images load.
9. The UI supports two complete images side by side or stacked. It also supports a draggable left/right or top/bottom reveal.
10. Both images align in the full source reference space. Different crop boundaries leave empty areas.
11. Comparison disables image adjustments, Undo, Redo, Version checkout, zoom, and pan.
12. A/B selection, display mode, divider position, swapping A/B, and closing remain available.
13. Selecting another image or leaving the editor closes comparison automatically.
14. HDR output disables comparison. Do not convert HDR to SDR to make comparison available.
15. The user accepts 8-bit SDR presentation through ordinary Qt Quick `Image` items and an in-memory image provider.
16. The normal viewport remains underneath the comparison surface. Closing comparison reveals it again.
17. Compare controls use the paged adjustment navigation introduced by PR #244.

The fixed sensor settings change the meaning of the two labels. Before means the imported root with current sensor settings. A named Version means its adjustments and white balance with current sensor settings. Show this explanation in plain text in the Compare panel. Do not describe these images as exact historical RAW renditions.

These decisions replace the earlier proposal for a separate temporary executor and native-resolution pair. HDR support and floating-point Qt presentation are outside this feature's scope.

## 2. Related work and repository requirements

Read `AGENTS.md` and applicable skills again before implementation.

| Reference | Relationship |
| --- | --- |
| [Executor ownership refactor](../../../refactor/2026-09-27-executor-ownership-refactor-plan.md) | Keep the document/executor split. The render port remains the sole editor executor owner. |
| [Editor render path simplification](editor_render_path_simplification_plan.md) | Keep one execution pool. Add no private worker thread or blocking future bridge. |
| [Session command queue](editor_session_command_queue_and_lock_simplification_plan.md) | Capture source states and reduce completion on the session owner. Project action decisions from the same policy used for admission. |
| [Single working document and history identity](editor_single_live_pipeline_wal_checkpoint_plan.md) | Retain current HEAD, WAL, checkpoint, and lease rules. Its older executor ownership sections are superseded. |
| [History and Versions UI](phase_7a_history_versions_repair_and_ui_refactor_plan.md) | Reuse Version identity and lists. Comparing a Version does not check it out. |
| [QML visual identity](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md) | Use Basic style, theme tokens, shared controls, and the per-file VI catalog. |
| [PR #244](https://github.com/zidage/AlcedoStudio/pull/244) | Reuse `SlidingIconNav` pages. Compare has its own hidden page. |

Use `alcedo-qml-ui`, `qt-qml`, `qt-cmake-project`, and `alcedo-msvc-cmake` during the relevant implementation phases. Write plan updates in simple English.

Use existing owners for reads and mutations. Do not copy `CommitGraph`, editor state, or complete parameter objects into a second model. The independent comparison documents and image outputs below have a specific lifetime and purpose. They never write back to live state.

Use LF files. Convert an existing CRLF file in a separate commit before its content edit. Include defining headers. Include only required OpenCV modules. Format only changed lines in existing files. Use a functional branch name. Keep temporary work under `build/tmp/editor_comparison/` and remove it after completion.

Run focused tests and direct caller tests only. Do not start the full test suite. Do not use `WorkspaceShellTest` as completion evidence. Verify QML input in the real application when an offscreen harness cannot deliver it.

## 3. Current source audit

These are source observations, not results from a build or test run for this plan.

| Area | Current source | Observed behavior | Required change |
| --- | --- | --- | --- |
| Working values | `app/editor_working_document.*` | `CurrentPreview()` returns an immutable publication. `Freeze()` shares unchanged nodes. | Capture the publication on comparison entry through the session owner. |
| Historical state | `app/pipeline_root_state.*`; `ui/alcedo_main/album_backend/editor_history_state_detail.*` | `BuildDocumentFromRoot()` replays any first-parent head and binds the camera profile. | Add a focused read operation for the held editor history. Do not checkout or copy the graph. |
| Runtime stamps | `edit/operators/models/parameter_revision.*`; `operator_model_base.hpp` | Writes use process-wide stamps. Clone copies values and stamps together. | Preserve current sensor stamps while retaining input-specific white balance. Equal JSON values alone are insufficient. |
| Sensor invalidation | `edit/runtime/runtime_invalidation.cpp` | `SensorFieldMask()` includes Demosaic, Highlights, and Lens. WhiteBalance invalidates from `develop_output`. | Reuse this distinction. Do not suppress real invalidation. |
| Executor binding | `edit/runtime/executor_role.hpp`; `detail/renderer.inl.hpp` | Interactive bindings use lineage and element identity. A different binding releases cached resources. | Give comparison derivatives the captured editor lineage and element identity. |
| Quality Base | `renderer/pipeline_scheduler.cpp`; `edit/runtime/result_persistence.hpp` | Quality Base uses full decode, 4096 pixels, Preview resampling, and `SensorDevelopOnly` persistence. | Reuse these settings for host image requests. Factor the existing request builder to avoid separate quality constants. |
| Host pixels | `edit/pipeline/pipeline_executor.*`; `detail/renderer.inl.hpp` | `require_host_output=true` downloads the result. A null request sink skips presentation. | Add an output operation that returns pixels with the exact resolved geometry and display configuration. |
| Worker | `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.*` | The port owns one Interactive executor and `PipelineScheduler(1)`. Normal dispatch reads the latest current preview and requires a sink. | Add explicit immutable-input image jobs on this same worker. Keep executor access private. |
| Cache retention | `basic_render_workspace.hpp`; `graph_image_cache.hpp` | Quality Base publishes only sensor output. Downstream storage is submission-local. | Retain this behavior for A and B. Download A before starting B. |
| Display pixels | `ui/editor_rhi/editor_viewport_item.cpp`; `shaders/editor_viewport.frag` | The viewport uses RGBA32F and samples pipeline output. | The user-authorized SDR image path quantizes the same output values once. |
| Qt images | `ui/alcedo_main/album_backend/path_utils.cpp`; `thumbnail_image_provider.*` | The RGBA conversion and image-provider pattern already exist. | Reuse the conversion and provider pattern with a comparison-owned store. Do not use the persistent thumbnail store. |
| HDR | `edit/graph/drt_node_model.*` | `IsHdrExportEncoding()` recognizes ST 2084 and HLG through the DRT owner. | Reject comparison for the current document or either selected input when this returns true. |
| UI | `EditorWorkspace.qml`; `EditorAdjustmentStack.qml`; `EditorVersionsPanel.qml`; `SlidingIconNav.qml` | The viewport binds the sink independently of the tool panel. The nav supports hidden pages. | Add an overlay and dedicated Compare page. Retain the underlying viewport item and binding. |

Existing test targets include `PipelineDocumentRenderTest`, `PipelineFrameSinkTest`, `GraphImageCacheRetentionTest`, `EditorSessionHistoryPortTest`, `EditorSessionRenderSchedulerPortTest`, `EditorRenderCoordinatorTest`, `EditorSessionActionPolicyCq3Test`, and `EditorAdjustmentHeaderQmlTest`. `PipelineDocumentRenderTest` is CUDA-dependent. Inspect current registration before building backend-specific tests.

The existing `HostBatchRendersReuseBatchDeviceAndLeaveInteractiveCacheUntouched` test documents a separate Batch executor. This feature uses the Interactive executor instead. Existing Quality Base cache tests provide the closer reference behavior.

## 4. Product specification

### Entry and controls

Add a labeled Compare action to the editor viewport toolbar and a labeled Compare action to the Versions header. Use text actions unless an appropriate approved repository icon is available. A new icon needs the approval required by `DESIGN.md` during implementation.

The viewport action opens Before/After. The Versions action opens Version comparison with A set to the root and B set to the captured current working state. Both selectors can choose Root, Current working state, or a named Version. Restrict the list to the open image.

On entry, retain the current published working document before any comparison rendering. Resolve the input pair after all earlier accepted writes reach the owner. An active parameter input must settle through the existing input-seal path. Do not cancel its value or force a save for comparison. Refuse entry while mask drawing, a Version operation, save recovery, or another incompatible session operation owns input.

Activate a `compare` adjustment panel on a separate nav page, distinct from Mask. Outside comparison, this page stays hidden. Record the previous adjustment-panel key. Restore that key on close when it is still valid.

The panel contains:

- A and B source selectors and separate source labels;
- a Before/After or Versions choice;
- separate controls for Complete images or Divider, and Left/right or Top/bottom;
- a Swap action and a Close action;
- a plain explanation of fixed current sensor settings and input-specific white balance;
- loading, error, retry, and HDR-unavailable text.

Start in Divider mode with a left/right line at the midpoint. Keep the selected mode and orientation during the current comparison. New comparison entry starts from these defaults. Do not persist comparison choices into the pipeline document.

Disable selectors and mode-changing requests that require new documents while a pair is rendering. Close remains available. Changing layout, divider orientation, divider position, or swapping an already-ready pair does not rerender.

### Spatial layout

```text
Complete images, left/right      Complete images, top/bottom
+-------------+-------------+    +---------------------------+
| A canvas    | B canvas    |    | A canvas                  |
| same source | same source |    +---------------------------+
| coordinates | coordinates |    | B canvas                  |
+-------------+-------------+    +---------------------------+

Divider, left/right              Divider, top/bottom
+-------------+-------------+    +---------------------------+
| A revealed  | B revealed  |    | A revealed                |
|             |             |    +------ movable line -------+
|       movable line       |    | B revealed                |
+-------------+-------------+    +---------------------------+
```

Each complete-image region fits the full reference canvas independently. Both use the same reference bounds. Divider mode fits one common reference canvas and clips two identically placed images along the movable line.

Let `F` map reference pixels into the fitted canvas. Place an output image with `F * render_to_reference`. Use its actual render pixel extent as local image size. Qt applies the resulting affine transform. Clip the final canvas to its bounds. Do not stretch a cropped output into the full source rectangle.

For example, a crop of the left half appears only in the left half of the reference canvas. Its right half remains empty. A rotated crop maps to its source-space quadrilateral. This alignment removes the user rotation from the displayed content orientation; it preserves the rotated crop footprint. It is not automatic feature matching or nonlinear image registration.

Use the same neutral theme surface for empty areas. Preserve aspect ratio. Allow labels to wrap. Retain the existing minimum viewport width and side-panel placement. A narrow window must not switch comparison mode automatically.

### Input, loading, and close behavior

Comparison blocks image writes, graph and mask writes, history movement, Version mutation, Paste, and viewport zoom/pan. Enforce this at command admission and in the UI. Do not rely only on an overlay intercepting the pointer.

The divider accepts pointer drag and keyboard arrow steps. Home and End reach its bounds. Tab reaches selectors, layout controls, divider, and Close. Escape closes comparison without consuming an active text-field edit first.

Keep the `EditorViewportItem` alive, bound, and visible beneath an opaque comparison overlay. Cover its photograph and input overlays visually. Do not set its visibility false or destroy its Loader. This lets already-accepted normal frames complete without leaving a hidden sink waiting for scenegraph consumption.

During initial loading, show no partial pair. For a new A/B selection, clear the previous pair from display and show loading. Publish a pair only after both renders succeed. Show the images only when both `Image.status` values are Ready. Keep the normal viewport underneath during failure. Show the actual error and permit Retry or Close.

Close releases documents, pending outputs, provider entries, URLs, and Qt image references. It restores the previous panel and removes the comparison action restriction. Retain the existing view transform. Ask the normal render coordinator to refresh the current document with its current view. This reconciles downstream runtime state without changing the live document or releasing sensor resources.

Select-image and leave-editor paths close comparison before releasing the image lease or viewport. Shutdown cancels accepted comparison work and waits for the scheduler before destroying its owners. Do not block the GUI on a render future.

### HDR and SDR

Disable entry when the captured current DRT uses ST 2084 or HLG. Validate both selected documents before scheduling. A selected HDR Version reports that HDR comparison is unavailable. Do not replace its DRT or tone-map it into SDR.

Quantize finite float RGBA output to `QImage::Format_RGBA8888` once. Preserve output channel order. Clamp at the conversion boundary as required by the user-approved 8-bit SDR format. Do not apply another DRT, gamma operation, gamut conversion, or thumbnail render recipe.

The provider only serves completed memory images. It never decodes or renders. Use `cache: false` and URLs that include the existing comparison operation identity and A/B side. Qt caches provider images by default; changing the URL makes a new pair load. Release item sources as well as store entries on close.

Qt 6.9.3's [default texture factory source](https://raw.githubusercontent.com/qt/qtdeclarative/v6.9.3/src/quick/util/qquickpixmapcache.cpp) converts other QImage formats to ARGB32 for ordinary image loading. The [Qt image-provider documentation](https://doc.qt.io/archives/qt-6.9/qquickimageprovider.html) describes provider loading and cache behavior. The user's explicit SDR decision permits this precision boundary.

## 5. Owners, APIs, and data flow

Names below are proposed. Put implementation in existing module directories. Do not create another editor engine or persistence format.

| Owner or proposed API | Responsibility and lifetime |
| --- | --- |
| Session owner / history port: `BuildComparisonInputs` | Resolve Root, Current, or Version head under current owner access. Capture both together. Return existing immutable `PipelineGraphSnapshot` representations. |
| `PipelineDocument::UseSensorSettingsFrom` | Focused operation on a private comparison document. Preserve its white balance and all downstream state. Adopt current Demosaic, Highlights, and Lens values with their matching field stamps. Validate backbone IDs and image origin before mutation. |
| `EditorComparisonService` | Own source selection, captured current publication, comparison activity, and pair publication on the session owner. Read history through its port. Call an image-render port without seeing GPU internals. |
| `IEditorImageRenderPort` | Application-facing seam for one image or a consecutive pair. Accept immutable inputs and `RenderRequest`; complete with image results or the real error. |
| `EditorSessionRenderSchedulerPort` | Implement the seam with its private current executor and existing scheduler. Keep all Apply calls on its single worker. Track accepted image work for cancel and shutdown. |
| `PipelineExecutor::ApplyImage` and matching renderer operation | Return host pixels, exact `ResolvedRenderGeometry`, and `ViewerDisplayConfig` from the executed plan. Keep existing `Apply` compatible by returning the pixel member. |
| `RenderedPipelineImage` | Minimal result: one existing image-buffer owner, exact geometry, and display configuration. It contains no document, history, or executor state. |
| `EditorComparisonController` | Project service state to QML on the GUI thread. Convert outputs and publish both URLs together. It owns no second copy of session/history state. |
| `ComparisonImageStore` / `ComparisonImageProvider` | Retain one ready pair for the active comparison. Serve `QImage` values under a store lock. Use QImage sharing rather than extra pixel copies. |
| `EditorComparisonCanvas.qml` | Fit reference space, transform an ordinary Image, and leave uncovered areas empty. It does no rendering or history work. |
| `EditorComparisonView.qml` / `EditorComparisonPanel.qml` | Display two canvases, clip divider regions, and offer controls. State survives QML panel recreation through the controller/service. |

### Independent documents and runtime stamps

Independent historical documents are necessary because both states must remain fixed while the executor runs later. Reusing the current mutable document or checking out A then B would change editor state. Use existing replay and Freeze representations. Do not serialize the current document just to capture it.

Capture the current preview once on entry. Retain it until close. Replay a non-current Version from the held root and graph on the owner thread. Do not call `LoadHistorySnapshot` for an image already held by the editor lease.

For a comparison-only document, retain the selected state's WhiteBalance fields: `use_camera_wb`, `user_wb`, `wb_mode`, custom CCT/tint, and as-shot CCT/tint. Keep image-bound camera data valid. Adopt current demosaic, highlights, and all lens/projection fields.

An implementation can clone the current Develop node, which preserves its sensor field stamps. It can then apply only the selected white-balance fields through focused Model operations. Install that node through a validated document/graph owner operation with the same Develop NodeId. Do not copy all revisions from a Model whose values differ. Do not add a mode that ignores invalidation comparisons.

Freeze the completed private document. Wrap it as an editor Preview with the captured current lineage and element identity. It is a derivative of the current loaded image, not a checkout. Do not label it Committed: its pinned sensor settings can differ from the selected historical head. Retain the selected Version/head only as source provenance for labels.

Use a new normal parameter stamp for changed white balance. Preserve sensor field stamps. Different branch topology and downstream adjustments keep their actual stamps. Existing process-wide stamps already distinguish their writes; add no second generation counter.

### Pair execution

Build a host request with these values:

```text
role = Interactive
decode_res = FULL
geometry = existing full-image Quality Base request
geometry.resolution.max_edge = 4096
geometry.resolution.quality = Preview
geometry.document_geometry = ApplyCropAndRotation
submission.metadata.frame_role = QualityBase
submission.metadata.scope_update_allowed = false
require_host_output = true
sink = nullptr
output_color = no override
```

Keep the attached live sink on the executor. The explicit request has a null sink; it must not inherit the executor's sink. This uses `SensorDevelopOnly` persistence and downloads the final 4K output before submission-local results are released.

Submit one composite work item to the existing scheduler. Inside it, run the reusable image operation for A, retain A's host result, then run it for B. Hold the same exclusive executor access throughout. Never enqueue B on that worker and wait for its future from A.

The render port remains the only component that invokes its executor. A reusable synchronous image helper is enough for future single-image requests. The composite pair is a narrow scheduling operation, not another general render queue.

```text
Compare action -> session owner -> comparison activity restriction
  -> capture current + resolve A/B from held history
  -> private sensor-setting derivatives -> freeze with current binding
  -> image-render port -> existing PipelineScheduler(1)
  -> existing Interactive executor: ApplyImage(A), then ApplyImage(B)
  -> owner completion -> GUI conversion -> publish both memory images
  -> both Qt Images Ready -> show comparison overlay
```

```text
Replay / validation / HDR / GPU / download / conversion failure
  -> discard incomplete pair -> owner reports the exact failure
  -> comparison error controls remain usable -> Retry or Close
  -> Close reveals normal viewport and requests a current-document refresh
```

### Cancellation and actual event ordering

Reuse existing session operation identity, image identity, and explicit cancellation support. A pair permits one active render. Disable new source-selection submissions while it runs. Do not add a replacement queue or new epoch protocol.

Test these real asynchronous sequences through production boundaries:

1. A runs on the scheduler worker. Close runs on the session owner before B starts. Cancellation skips B and owner completion cannot reopen comparison.
2. B finishes and queues completion. Image selection closes comparison and replaces the image lease before completion is reduced. The closed operation cannot publish into the new image.
3. The provider starts reading a ready QImage. Close clears its entry. Its shared image value remains valid until that read finishes.
4. Shutdown starts during A. The scheduler owner cancels image work and waits before service, executor, or callback storage is destroyed.

These queue and callback boundaries justify operation correlation and existing cancellation. They do not justify more versioning mechanisms. Add deterministic tests that drive the boundaries, rather than injecting impossible identity values.

### Future Detail reuse

Keep the image operation independent of Compare UI and source-pair selection. It accepts one immutable graph and the existing `RenderRequest`. It returns pixels with exact geometry. A later Detail service can map a source-reference selection into the selected document's edit space and request that region.

Do not implement a Detail panel in these phases. Do not assume a requested region permits partial RAW processing or small neighborhood support. Existing sampling footprints and full-reference processing requirements remain authoritative. Detail images must not replace the main viewport's reference geometry.

## 6. File map

All source paths below are relative to `alcedo_studio/src/`. Proposed files do not exist at plan creation.

| Phase | Current files to inspect or change | Proposed files |
| --- | --- | --- |
| 1 | `app/pipeline_root_state.*`; `include/app/editor_session_ports.hpp`; `ui/alcedo_main/album_backend/editor_session_history_port.*`; `editor_history_state_detail.*`; `edit/graph/pipeline_document.*`; `pipeline_graph.*`; `develop_node_model.*` | `include/app/editor_comparison_inputs.hpp`; `app/editor_comparison_inputs.cpp` |
| 2 | `edit/pipeline/pipeline_executor.*`; `include/edit/runtime/renderer.hpp`; `detail/renderer.inl.hpp`; `renderer/pipeline_scheduler.*`; `include/renderer/pipeline_task.hpp`; `editor_session_render_scheduler_port.*`; module CMake files | `include/edit/pipeline/rendered_pipeline_image.hpp`; `include/app/editor_image_render_port.hpp` |
| 3 | `ui/alcedo_main/album_backend/path_utils.*`; `ui/alcedo_main/CMakeLists.txt`; `DESIGN.md`; `docs/VI/README.md` | `ui/alcedo_main/album_backend/comparison_image_provider.*` and matching include headers; `qml/EditorComparisonCanvas.qml`; `qml/EditorComparisonView.qml`; `qml/EditorComparisonPanel.qml` |
| 4 | `app/editor_session_service.*`; `editor_session_command_queue.*`; `editor_action_policy.*`; `ui/alcedo_main/album_backend/editor_session_controller.*`; `application_module_host.cpp`; `qml/EditorWorkspace.qml`; `EditorAdjustmentStack.qml`; `EditorVersionsPanel.qml`; shortcut and translation registrations | `include/app/editor_comparison_service.hpp`; `app/editor_comparison_service.cpp`; `ui/alcedo_main/album_backend/editor_comparison_controller.*` and matching include header |
| 5 | `tests/ui/editor_session_render_scheduler_port_test.cpp`; `tests/ui/CMakeLists.txt`; `tests/edit/pipeline/pipeline_document_render_test.cpp`; `tests/edit/runtime/opencl_drt_product_test.cpp`; `tests/edit/runtime/metal_renderer_test.cpp`; `tests/edit/CMakeLists.txt`; backend renderer, workspace, or presenter files only when a test finds a defect | `tests/ui/support/editor_render_port_gpu_fixture.hpp`; `tests/ui/editor_session_render_scheduler_port_image_job_test.cpp` |

Register all new QML in `ALCEDO_MAIN_QML_FILES`. Add the `compare` key to `NormalizeAdjustmentPanel`. Keep application-layer interfaces independent of QML and native GPU types. Add dependencies to the owning target directly.

## 7. Phase summary

| Phase | Result | Main modules | Prerequisite | Expected changed lines | Status |
| --- | --- | --- | --- | ---: | --- |
| 1 | Read-only comparison documents with current sensor settings and selected white balance | History port, document/Model owners | Current source audit | 700-1300 | Complete (2026-10-01) |
| 2 | Consecutive one-shot image jobs on the current editor worker and executor | Executor, renderer, scheduler port | Phase 1 | 900-1700 | Complete (2026-10-01) |
| 3 | Memory-image presentation with source alignment and all four layouts | Provider, QML canvas/view/panel | Phase 2 result schema | 850-1600 | Partial (2026-10-01): manual check open |
| 4 | Product entry, Version selection, restrictions, close, and restore | Comparison service/controller, session, workspace | Phases 1-3 | 1000-1900 | Partial (2026-10-01): manual check open |
| 5 | Phase 2 image job proven on OpenCL and Metal | Port test fixture, backend renderer tests, backend corrections | Phase 2 | 500-1100 | Planned |

Each range includes production code, tests, registration, resources, and phase completion documentation. No phase needs splitting at this estimate. Split before implementation if its expected diff can exceed 2000 lines. Split because of actual scope growth, not to omit an approved behavior.

## 8. Detailed phases

### Phase 1: Build independent comparison inputs through their owners

**Objective and deliverables.** Produce A and B without moving history, mutating working values, or rebuilding the executor. Retain actual white balance and downstream differences while sharing current sensor settings.

**Inputs and prerequisites.** Read the held history lease, imported root, selected Version identities, and captured current preview. Recheck `BuildDocumentFromRoot`, model revision rules, and stable Develop IDs.

**Modules and APIs.** Add the focused history read operation and minimal source selectors. Add `UseSensorSettingsFrom` through the document/Model owner. Keep provenance labels separate from Preview/Committed identity. Use the Phase 1 file map.

**Data invariants.** Both inputs belong to the current image and loaded lineage. Sensor values and field stamps equal the captured current publication. White balance remains selected-state data. No new history record, WAL write, save, checkpoint, active-Version move, or dirty-state change occurs.

**Steps.**

1. Resolve Root, Current, and Version selectors on the session owner. Reject a missing Version or a foreign image.
2. Use current immutable publication directly when its requested state needs no sensor derivative.
3. Replay other states from the held graph and root. Keep graph reads on their owner; do not retain graph references in worker callbacks.
4. Validate graph, camera binding, Develop identity, and HDR status before rendering.
5. Build the private Develop variant with current sensor values/stamps and selected white balance. Publish only after the full operation succeeds.
6. Freeze each private result as a Preview under current binding. Document its purpose and release on pair replacement or close.

**Success chain.** Selector -> history owner -> root replay/current publication -> focused sensor application -> Freeze -> immutable input pair.

**Failure chain.** Missing source, invalid backbone, unbound required camera data, or HDR -> error -> discard private results -> original history and document remain visible.

**Tests and observable assertions.**

- `RootAndCurrentInputsKeepHistoryAndWorkingDocumentUnchanged`: compare head, active Version, dirty state, journal sequence, and working values before/after.
- `VersionInputsRetainOwnWhiteBalanceAndUseCurrentSensorSettings`: include different demosaic, highlights, lens, CCT, tint, geometry, and Grade values.
- `SensorSettingsDerivativeKeepsCurrentSensorFieldRevisions`: assert Demosaic/Highlights/Lens stamps exactly match current; changed white balance remains distinguishable.
- `ReplayOfDifferentGradeTopologyKeepsStableDevelopIdentity`: add/remove Grade nodes and Masks; confirm correct target topology.
- `ComparisonInputFailureDoesNotPublishPartialPair`: drive invalid Version and replay failure through the real owner API.
- `HdrCurrentOrSelectedVersionRejectsComparisonWithoutSdrRewrite`: cover PQ and HLG on either source.

Add a proposed `EditorComparisonInputsTest` in `tests/app/`. Extend existing document/history tests when their ownership fixtures already exercise the changed API.

**Build/run.** Use the common Windows commands below with `EditorComparisonInputsTest` and `EditorSessionHistoryPortTest`. Inspect and run direct document-owner tests that registration includes. Use matching filtered CTest prefixes.

**Exit criteria.**

- [x] All source selectors produce the specified state.
- [x] Sensor values and field stamps match current; white balance remains source-specific.
- [x] Failure and successful reads leave live and persistent state unchanged.
- [x] No independent CommitGraph or whole parameter mirror was added.

**Expected diff.** 700-1300 lines.

**Completion record.** See the Phase 1 record below.

##### Phase 1 completion record (2026-10-01)

**Status:** complete. Both comparison inputs are built from the held history through the history port. Each input has the current sensor settings and stamps and keeps its own white balance and downstream state. History, the working document, and persistent state stay unchanged.

**Source revision and branch:** based on `deeb8901881b5aae41680299f75b735ed4d01c3c`; branch `feature/editor-comparison-inputs`. Not committed when this record was written.

**Implemented behavior and APIs:**

| Owner | Change |
| --- | --- |
| `OperatorModelBase::TakeFieldsFromThenMutate` (protected) | Copies the payload and all field stamps of a source Model, then applies one focused write-back, under both Model locks. Changed fields get one new stamp. |
| `DevelopParamsModel::UseSensorSettingsFrom` | Takes demosaic, highlights, lens/projection, and camera profile with their stamps from current. Keeps `use_camera_wb`, `user_wb`, `wb_mode`, and custom/as-shot CCT/tint. Equal white balance keeps current's stamp; different white balance gets one new stamp. |
| `PipelineDocument::UseSensorSettingsFrom` | Checks Develop presence, Develop NodeId, and equal camera profile (same image) before it writes. Changes no geometry, Grade, Mask, DRT, or topology. |
| `EditorComparisonSource`, `EditorComparisonInput`, `EditorComparisonInputPair` (`app/editor_comparison_types.hpp`) | Root, Current, or Version selectors. Provenance (`source`, `source_head`) is separate from the preview identity. |
| `BuildEditorComparisonInputs` (`app/editor_comparison_inputs.*`, library `EditorComparisonInputs`) | Current, and the active Version, return the captured preview unchanged. Root and other Versions replay with `BuildDocumentFromRoot`, validate the product graph, reject HDR, apply current sensor settings, require a bound DNG profile, and freeze as a Preview with the captured lineage and image. Returns both inputs or none. |
| `IEditorHistoryPort::BuildComparisonInputs` / `EditorSessionHistoryPort` | Reads the held graph and root under the port lock. Rejects an invalid guard, a missing state, or a preview captured from an earlier lineage. Writes nothing. |

**Deviation from the file map:** the selector and result types are in `include/app/editor_comparison_types.hpp`. `editor_comparison_inputs.hpp` includes `pipeline_root_state.hpp`, which includes Storage/DuckDB headers; the separate types header keeps `editor_session_ports.hpp` free of them. The `EditorComparisonInputsTest` source is in `tests/app/`, but it is registered in `tests/ui/CMakeLists.txt` because it compiles the history port sources directly. Both history port test binaries share the `ALCEDO_EDITOR_HISTORY_PORT_SOURCES` list.

**Primary success call chain:**

```text
IEditorHistoryPort::BuildComparisonInputs(guard, captured_current, a, b)
  -> EditorSessionHistoryPort (port lock) -> EditorHistoryState::EnsureWorkingState
  -> lineage check: captured_current.Lineage == working document Lineage
  -> BuildEditorComparisonInputs(graph, root, captured_current, element_id, a, b)
       -> reject HDR / unbound DNG profile on captured_current
       -> per side: ResolveSource (Root | Current | Version head; active Version -> captured)
            Current/active Version -> captured preview, unchanged
            otherwise -> BuildDocumentFromRoot -> ValidateProductDocument -> RejectHdr
                      -> PipelineDocument::UseSensorSettingsFrom
                           -> DevelopParamsModel::UseSensorSettingsFrom
                           -> OperatorModelBase::TakeFieldsFromThenMutate
                      -> RequireBound -> Freeze -> PipelineGraphSnapshot::Preview(current lineage)
  -> EditorComparisonInputPair written to the caller only when both sides succeed
```

**Primary failure call chain:**

```text
missing/foreign Version | replay failure | invalid graph | camera profile mismatch
| unbound DNG profile | ST 2084/HLG on current or selected | earlier-lineage capture
  -> exception inside BuildEditorComparisonInputs, or port check -> error text names the side
  -> private replay documents are dropped; the output pair is not written
  -> history head, active Version, commits, WAL, dirty state, working document,
     lineage, and published preview stay unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `RootAndCurrentInputsKeepHistoryAndWorkingDocumentUnchanged` | `EditorComparisonInputsTest` | PASS |
| `VersionInputsRetainOwnWhiteBalanceAndUseCurrentSensorSettings` | `EditorComparisonInputsTest` | PASS |
| `SensorSettingsDerivativeKeepsCurrentSensorFieldRevisions` | `EditorComparisonInputsTest` | PASS |
| `ReplayOfDifferentGradeTopologyKeepsStableDevelopIdentity` | `EditorComparisonInputsTest` | PASS |
| `ComparisonInputFailureDoesNotPublishPartialPair` (missing Version, replay failure, earlier-lineage capture) | `EditorComparisonInputsTest` | PASS |
| `HdrCurrentOrSelectedVersionRejectsComparisonWithoutSdrRewrite` (ST 2084 and HLG; current and selected) | `EditorComparisonInputsTest` | PASS |
| `ActiveVersionUsesCapturedWorkingValues` (added) | `EditorComparisonInputsTest` | PASS |
| `DevelopModelTakesSensorStampsAndKeepsOwnWhiteBalance` (added; equal and different white balance, self-source rejection) | `EditorComparisonInputsTest` | PASS |
| `DocumentOfAnotherImageRejectsSensorSettingsUnchanged` (added) | `EditorComparisonInputsTest` | PASS |

The unchanged-state assertions compare the active head, active Version, commit count, Version count, unmaterialized flag, WAL file size, canonical working JSON, `DocumentRevisionFingerprint`, lineage, and the identity of the published preview pointer.

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagModelGraphTest EditorSessionHistoryPortTest EditorComparisonInputsTest AlbumBackendLib --parallel 4
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -R '^(EditorComparisonInputsTest|EditorSessionHistoryPortTest|GpuDagModelGraphTest)\.' --output-on-failure -j 1
```

The build exited with code 0. CTest exited with code 8 because of one failure that predates this phase:

| Suite | Discovered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| `EditorComparisonInputsTest` | 9 | 9 | 0 | 0 |
| `EditorSessionHistoryPortTest` | 107 | 106 | 1 | 0 |
| `GpuDagModelGraphTest` (document/Model owners, header rules) | 115 | 115 | 0 | 0 |

The failure is `EditorHistoryCommitPresentationTest.FormatsNumericBooleanPathEnumAndCompoundAdjustments`: a crop commit label gives `"Crop"` where the test expects `"+12°"`. It also fails on clean `deeb89018` (tracked changes stashed, target rebuilt, test run directly), so this phase did not cause it.

**Manual verification:** none needed; Phase 1 has no UI or render path.

**Cache counters, resources, and timing:** not applicable to Phase 1. Equal sensor stamps are the precondition for cache reuse, and the tests check them directly. Renderer counters are recorded in Phase 2.

**Checklist / exit condition:** all four boxes are checked.

**LOC note:** about 1,060 changed lines: 223 added and 8 removed in tracked files, plus 839 lines in new files (577 of them in the test). The largest touched production file is `pipeline_document.cpp` at 483 lines. No touched file is over 1,000 lines.

**Remaining gaps:**

- The full test suite was not run. Per `AGENTS.md`, only the user starts a full run.
- `alcedo_main` was not built. `AlbumBackendLib` built successfully; it compiles the production history port and links the new library.
- Nothing in production calls `BuildComparisonInputs` yet. The Phase 4 comparison service will call it.

### Phase 2: Render a pair on the existing editor worker

**Objective and deliverables.** Run A then B on the same existing Interactive executor. Return host pixels and exact geometry without calling the viewport sink.

**Inputs and prerequisites.** Use Phase 1 frozen inputs. Recheck Quality Base geometry, persistence, host-download order, and port shutdown.

**Modules and APIs.** Add `RenderedPipelineImage`, the compatible `ApplyImage` operation, the application image-render seam, and one composite pair job in the existing scheduler port. Factor the shared full-image Quality Base request builder. Use the Phase 2 file map.

**Data invariants.** The current executor, backend, source identity, Develop NodeId, and binding stay the same. All renders use the existing single worker. Comparison requests have a null sink and disabled scope updates. A's host buffer survives B. Transient GPU output does not become another persistent result cache.

**Steps.**

1. Return image metadata from the actual executed plan. Keep the existing Apply return behavior for existing callers.
2. Build the comparison host request from the common Quality Base builder. Use full decode, 4096 pixels, Preview resampling, and `FrameRole::QualityBase`.
3. Resolve the port's already-bound input once. Reuse its buffer; do not load source bytes separately for A and B.
4. Submit A/B as one consecutive worker operation. Download A before B begins. Do not expose an executor getter for feature code.
5. Publish only a complete pair. Forward GPU and download errors without alternate backend, decode, operator, or resolution.
6. Extend explicit cancel and shutdown to cover accepted image jobs, including queued jobs. Preserve callbacks and input lifetimes until the worker stops.
7. Verify that downstream invalidation is still observed when normal rendering resumes. Never restore an old document copy onto live state.

**Success chain.** Frozen inputs -> existing scheduler worker -> current executor A download -> current executor B download -> one complete pair -> owner callback.

**Failure/restore chain.** A or B failure/cancel -> release submission-local resources and incomplete host outputs -> report failure -> normal sink and current binding remain owned by the editor -> normal current-document render can proceed.

**Tests and observable assertions.**

- `WarmComparisonPairReusesEditorSensorResult`: warm a real RAW editor frame; assert the pair adds zero LibRaw unpack and zero sensor-develop executions.
- `WhiteBalanceDifferenceChangesPairPixelsWithoutSensorExecution`: assert image differences and zero sensor work after warm-up.
- `ComparisonHostRequestsKeepEditorExecutorAndQueueIdentity`: assert one executor/device/queue, unchanged binding, and no sink-ready notification for the pair.
- `ComparisonPairDoesNotInterleaveAAndBWithNormalFrames`: submit work before and after the composite item and record actual worker order.
- `DifferentVersionTopologyRendersCorrectPixelsWithSharedSensor`: compare each side against an independent fresh execution of its derived document.
- `AResultRemainsValidAfterBAndTransientRelease`: verify A pixels after B completes and submission-local storage is released.
- `ComparisonCloseDuringARenderSkipsBAndCannotPublish`: drive real explicit cancellation.
- `ComparisonFailureAllowsNormalCurrentDocumentRender`: fail a download or GPU pass, then run the normal request and verify expected pixels.

Extend `PipelineDocumentRenderTest` and `EditorSessionRenderSchedulerPortTest`. Use the existing Quality Base cache fixtures where applicable. Declare float pixel tolerance per fixture; use `2e-5` only when the current fixture already supports it. Do not substitute a fake renderer for cache-reuse evidence.

**Build/run.** Build those two targets plus `PipelineFrameSinkTest` and `GraphImageCacheRetentionTest`. Run filtered tests with GPU concurrency set to one. Record unavailable backends separately.

**Exit criteria.**

- [x] One existing executor/worker runs A/B consecutively and never presents them to the sink.
- [x] Warm real-RAW evidence shows no new unpack or sensor-develop execution.
- [x] Pixel comparisons cover different white balance and topology.
- [x] Cancel, failure, and shutdown preserve ownership and permit normal rendering.

**Expected diff.** 900-1700 lines.

**Completion record.** See the Phase 2 record below.

##### Phase 2 completion record (2026-10-01)

**Status:** complete. The editor render port renders one image, or A then B, as host pixels on its existing Interactive executor and single worker. It uses the shared full-image Quality Base request with no sink. A job publishes all of its images or none. Close and shutdown cancel it, and a normal frame renders correctly after the job.

**Source revision and branch:** based on `bf74bf8ac` (Phase 1); branch `feature/editor-comparison-pair-render`. Not committed when this record was written.

**Implemented behavior and APIs:**

| Owner | Change |
| --- | --- |
| `RenderedPipelineImage` (`include/edit/pipeline/rendered_pipeline_image.hpp`) | Host pixels, the exact `ResolvedRenderGeometry` of the executed plan, and its `ViewerDisplayConfig`. No document, history, or executor state. |
| `Renderer<Backend>::RenderImage` | The former `Render` body. It returns the pixels with `plan.geometry` and the display configuration. `Render` returns its `pixels` member, so existing callers are unchanged. |
| `PipelineExecutor::ApplyImage` | Requires `require_host_output`. `Apply` and `ApplyImage` share one private `Render` that selects the backend and role renderer. The request's own sink is used; a null sink presents nothing, although the viewport sink stays attached. |
| `MakeQualityBaseApplyRequest` and `kQualityBaseMaxLongEdge` (`pipeline_apply_request.hpp`) | The one source of the full-image Quality Base values: FULL decode, Interactive role, 4096 long edge, Preview resampling, frame role QualityBase (`SensorDevelopOnly`). `PipelineTask::MakeApplyRequest` (QUALITY_BASE_PREVIEW) now uses it. The scheduler's private 4096 constant is removed. |
| `IEditorImageRenderPort`, `EditorImageRenderRequest`, `EditorImageRenderResult` (`include/app/editor_image_render_port.hpp`) | Application seam: the held image identity, one or two immutable snapshots, and the geometry (default: full-image Quality Base). Status Completed, Failed, or Cancelled. Images are published all together or not at all. No executor or GPU types. |
| `EditorSessionRenderSchedulerPort::ScheduleImages` / `CancelImages` | Accepts one image job at a time. It requires the bound context of the requested image, a held lease, and the binding (lineage and element) of the current preview on every snapshot. It reuses the bound encoded input. The job is one `ScheduleWork` item that holds the executor render lock for all of its documents. It checks cancellation before each document and calls `ApplyImage` with a null sink, host output, and `scope_update_allowed = false`. A failure discards earlier images. The completion runs before the job is cleared, so `Shutdown` waits for it. |
| `EditorSessionRenderSchedulerPort::Shutdown` | Also cancels the accepted image job and waits until its completion has returned. |
| `EnsureContext` / `ContextMatches` | The context lookup of `EnsureContextForRequest`, factored to take identity values so that the image job and frames share it. |

**Deviation from the plan:** the plan asked for a "composite pair" operation. `ScheduleImages` accepts one or two documents (`kMaxEditorImagesPerJob = 2`), so the same narrow operation serves a later single-image request. It is not a general render queue: one job is accepted at a time.

**Primary success call chain:**

```text
IEditorImageRenderPort::ScheduleImages(request{element, image, epoch, [A, B]}, on_complete)
  -> EditorSessionRenderSchedulerPort (caller thread)
       -> bound context == request identity; CurrentPreview(element) held
       -> RenderBindingKey::Of(A) == Of(B) == Of(current preview)
       -> EnsureContext (bound encoded input, no reload) -> EnsureExecutor (the port's executor)
       -> image_job_ = {id}; PipelineScheduler(1)::ScheduleWork(one item)
  -> editor worker: RunImageJob
       -> lock the executor render lock for the whole job
       -> MakeQualityBaseApplyRequest + host output, sink = nullptr, no scope update
       -> PipelineExecutor::ApplyImage(A) -> Renderer::RenderImage
            -> prepared source hit, sensor_linear lookup hit (SensorDevelopOnly)
            -> Download(A) -> DiscardUnpublished -> {pixels, plan.geometry, display}
       -> cancellation check -> ApplyImage(B) -> Download(B)
       -> unlock the render lock
  -> FinishImageJob: on_complete(Completed, [A, B]) -> clear image_job_
```

**Primary failure and restore call chain:**

```text
B render error (example: corrupted CUBE in the root's LUT)
  -> PlanExecutor failure cleanup -> Renderer catch path -> exception to RunImageJob
  -> status Failed, real error text, A's host pixels released -> on_complete(Failed, [])
  -> editor results, binding, and viewport sink unchanged -> next frame renders as before

CancelImages(id) while queued or during A | Shutdown during A
  -> image_job_.cancelled -> a document that has not started is skipped (B never runs)
  -> the finished A is dropped in FinishImageJob -> on_complete(Cancelled, [])
  -> Shutdown returns only after that completion returned
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `WarmComparisonPairReusesEditorSensorResult` (0 LibRaw unpacks, 0 prepared-source misses, 0 sensor develops, 2 sensor skips, no new published result; the next frame equals the frame before, with no sensor work) | `EditorSessionRenderSchedulerPortTest` (GPU, CUDA) | PASS |
| `WhiteBalanceDifferenceChangesPairPixelsWithoutSensorExecution` (mean channel difference > 0.01; each side within `2e-5` of a fresh executor) | same | PASS |
| `ComparisonHostRequestsKeepEditorExecutorAndQueueIdentity` (same executor, renderer, device, queue, and binding; no batch renderer; 0 sink presentations; viewport sink still attached; pixel size equals `render_extent`; long edge at most 4096; display equals the document DRT) | same | PASS |
| `ComparisonPairDoesNotInterleaveAAndBWithNormalFrames` (observed order frame-1, pair, frame-2; one presentation before the pair completed) | same | PASS |
| `DifferentVersionTopologyRendersCorrectPixelsWithSharedSensor` (extra Grade, crop, rotation, white balance, highlights; each side within `2e-5` of a fresh executor; 0 sensor develops; same full reference extent) | same | PASS |
| `AResultRemainsValidAfterBAndTransientRelease` (published count, value ids, and prepared-source entries unchanged; 0 unpublished results; A within `2e-5` of a fresh render after B) | same | PASS |
| `ComparisonCloseDuringARenderSkipsBAndCannotPublish` (cancel while A's LUT lookup holds the worker; Cancelled with no image; one prepared-source acquire; the next job completes) | same | PASS |
| `ComparisonFailureAllowsNormalCurrentDocumentRender` (corrupted CUBE on the root side; Failed with the error and no image; 0 unpublished results; the next frame equals the frame before, with no sensor work and the same binding) | same | PASS |
| `ShutdownDuringAImageCancelsAndWaitsForTheJob` (added; Shutdown blocks while A renders and returns after the Cancelled completion; B never starts; later jobs are rejected) | same | PASS |
| `QualityBaseFrameAndImageJobShareOneRequestBuilder` (added) | same (no GPU) | PASS |
| `ImageJobRejectsDocumentsThatWouldRebindOrOverlap` (added; unbound image, wrong epoch, 0 or 3 documents, foreign lineage, second job, after shutdown; rejected jobs never complete) | same (no GPU) | PASS |
| `QueuedImageJobCancelledBeforeItStartsRendersNothing` (added; no renderer was created) | same (no GPU) | PASS |
| `ImageJobFailureReportsTheRenderErrorWithoutImages` (added) | same (no GPU) | PASS |
| `ApplyImageReturnsExecutedGeometryWithoutPresentingToTheSink` (added; geometry equals the presented frame's geometry; pixels within `2e-5` of the presented pixels and of `Apply`; no host output throws) | `PipelineDocumentRenderTest` (GPU, CUDA) | PASS |

The close-during-A and shutdown-during-A tests use a real production callback on the worker: the executor's `LutResourceResolver`. The test resolver is given to `PipelineMgmtService`. It holds the worker inside A's LUT lookup until the test thread has acted. The production code has no test branch.

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionRenderSchedulerPortTest PipelineDocumentRenderTest PipelineFrameSinkTest GraphImageCacheRetentionTest GpuDagCudaDrtProductTest --parallel 4
$env:PATH = "D:/Projects/pu-erh_lab/vcpkg/installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -R '^(EditorSessionRenderSchedulerPortTest|PipelineDocumentRenderTest)\.' --output-on-failure -j 1
ctest --test-dir build/debug -R '^(PipelineDocumentRenderTest|PipelineFrameSinkTest|GraphImageCacheRetentionTest|GpuDagCudaDrtProductTest)\.' --output-on-failure -j 1
```

The build exited with code 0. The first CTest command exited with code 8 because of the 5 failures below. The second exited with code 0.

| Suite | Discovered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| `EditorSessionRenderSchedulerPortTest` | 34 | 29 | 5 | 0 |
| `PipelineDocumentRenderTest` (CUDA) | 13 | 13 | 0 | 0 |
| `PipelineFrameSinkTest` | 23 | 23 | 0 | 0 |
| `GraphImageCacheRetentionTest` | 17 | 17 | 0 | 0 |
| `GpuDagCudaDrtProductTest` (renderer and Quality Base cache tests) | 85 | 85 | 0 | 0 |

The 5 port failures predate this phase: `ProductionPipelinePathSchedulesInstalledContextWithoutAdapterBind`, `ViewDrivenReasonsDisableScopeFrameReplacement`, `ScopeRefreshMarksFrameAsRequestedScopeInput`, `SessionDoesNotStampPreviewGenerationFromIntent`, and `InstalledContextAllowsScheduleWithoutImagePoolService`. Each expects a sink presentation from a fixture context that has no encoded bytes. They also fail on clean `bf74bf8ac` with only the link fix below (Phase 2 changes stashed, target rebuilt, CTest run).

**Test build fix:** `EditorSessionRenderSchedulerPortTest` had not linked in this build tree. Its GPU fixture calls `LibRaw::unpack`. The test found the vcpkg `libraw/libraw.h` before the bundled LibRaw headers, so it imported a symbol that the bundled static library does not export. `tests/ui/CMakeLists.txt` now links `libraw::raw_r` directly and puts the bundled LibRaw include directory first (only when the `puerhlab_libraw` target exists).

**Manual verification:** none needed; Phase 2 has no UI.

**Cache counters, resources, and timing:** sample `raw/linear_dng/mfzoty.dng` (a linear DNG read through LibRaw), CUDA, Debug build. After one warm viewport frame, a pair adds 0 LibRaw unpacks, 0 prepared-source misses, 2 prepared-source hits, 0 sensor-develop executions, and 2 sensor-develop skips. It adds no published result, no session value id, and no unpublished result. Wall times were not measured separately. Each GPU test case takes 9-18 s in this Debug build, including device creation and the fresh reference renders. Large-RAW pair timing is a Phase 4 exit criterion.

**Checklist / exit condition:** all four boxes are checked. The warm-cache evidence uses a linear DNG, the real RAW sample of the existing GPU fixtures. It is not a Bayer CFA file.

**LOC note:** 1,379 lines added and 54 removed in 13 files, including two new headers of 146 lines. The test file `editor_session_render_scheduler_port_test.cpp` grew from 819 to 1,609 lines, so it is now over 1,000 lines. Before Phase 4 adds tests to it, move the image job tests and their fixture helpers into a separate test file with a shared fixture header. The largest production file is `editor_session_render_scheduler_port.cpp` at 811 lines.

**Remaining gaps:**

- The full test suite was not run. Per `AGENTS.md`, only the user starts a full run.
- `alcedo_main` was not built. `AlbumBackendLib`, which compiles the port, was built through the port test.
- Nothing in production calls `ScheduleImages` yet. The Phase 4 comparison service will call it.
- OpenCL: `win_debug` compiles it (`HAVE_OPENCL`), so `Renderer<OpenClBackend>::RenderImage` and the OpenCL branch of `PipelineExecutor::Render` were compiled and linked. No Phase 2 test ran on OpenCL; every GPU test selects CUDA.
- Metal: not compiled. `ALCEDO_ENABLE_METAL` is OFF on Windows; only the macOS presets compile it.
- Phase 5 runs the Phase 2 tests on OpenCL and Metal.

### Phase 3: Display SDR pairs in aligned Qt image canvases

**Objective and deliverables.** Show completed in-memory pairs with source-coordinate alignment, complete-image layouts, and both divider orientations.

**Inputs and prerequisites.** Use Phase 2 result metadata. Use real production QML and AppTheme. Read the VI rules and the existing conversion/provider pattern.

**Modules and APIs.** Add the pair image store/provider, the canvas, view, and controls panel. Register QML and the provider. Use the Phase 3 file map. A focused test fixture supplies completed images without adding test branches to production code.

**Data invariants.** One ready pair owns two QImages and their actual geometry. Both URLs publish together. Images and documents stay independent. Mode/divider movement does not call the renderer. QML uses one reference transform per canvas and preserves each crop footprint.

**Steps.**

1. Validate nonempty finite float pixels and matching dimensions against render geometry. Reuse the RGBA conversion for the approved SDR output.
2. Publish both QImages into the comparison store in one focused operation. Use existing operation identity in URLs and disable QML image caching.
3. Implement the reference-to-canvas fit transform and compose it with each `render_to_reference` matrix. Do not guess reference extent from texture dimensions.
4. Implement complete-image and divider layouts with ordinary Image items, parent clipping, and affine transforms.
5. Add accessible divider input, orientation, Swap, loading, and error presentation. Display only a pair whose two Image items are Ready.
6. Clear source references and store entries on close/replacement. Verify QImage values already handed to provider callers remain valid.
7. Record per-file VI decisions. Add AppTheme/DESIGN values only if an existing token cannot express the approved geometry.

**Success chain.** Complete rendered pair -> validated SDR conversion -> atomic store publication -> both Images Ready -> fitted and aligned comparison view.

**Failure/restore chain.** Invalid pixels, geometry, conversion, or image load -> pair stays hidden -> real error -> Retry/Close -> release references and reveal normal viewport.

**Tests and observable assertions.**

- `SdrPairConversionMatchesRoundedClampedRgbaValues`: cover channel order, alpha, fractional values, and output-buffer lifetime.
- `PairProviderNeverPublishesOneNewSideWithOneOldSide`: inspect publication through the real store API.
- `SourceAlignmentPlacesHalfCropInHalfOfReferenceCanvas`: assert mapped corners and empty-area coverage.
- `RotatedCropCornersMatchRendererReferenceGeometry`: use actual resolved geometry with nonzero crop offsets and rotation.
- `BothDividerOrientationsRevealTheSameReferencePoint`: verify both source transforms stay equal while clipping changes.
- `LayoutAndDividerChangesDoNotSubmitRenderJobs`: assert zero render calls for ready-pair view operations.
- `PairRemainsHiddenUntilBothImagesAreReady`: drive differing image-load completion order.
- `ClosingComparisonClearsProviderAndItemReferences`: check entry count and released image ownership after pending reads finish.

Add proposed `EditorComparisonImageProviderTest`, `EditorComparisonGeometryTest`, and `EditorComparisonViewQmlTest`. Keep input checks small. Use manual application testing for divider drag and focus when offscreen delivery is unreliable.

**Build/run.** Build the three proposed targets and `alcedo_main`. Run their filtered suites. Check both themes, narrow windows, and representative display scaling in the real application.

**Exit criteria.**

- [x] All four layouts display a completed pair.
- [x] Original reference alignment preserves crop footprints and empty areas.
- [x] SDR quantization, channel order, and lifetime assertions pass.
- [ ] Pointer/keyboard divider behavior and focus are manually verified.

**Expected diff.** 850-1600 lines.

**Completion record.** See the Phase 3 record below.

##### Phase 3 completion record (2026-10-01)

**Status:** partial. All Phase 3 code and tests are complete and pass. The last exit item, a manual check of divider drag and focus in the real application, is open: no production path opens the comparison view until Phase 4 adds the entry action. Keyboard steps, Home, and End were tested through the production QML item.

**Source revision and branch:** based on `56e172bfe` (Phase 2); branch `feature/editor-comparison-image-presentation`. Not committed when this record was written.

**Implemented behavior and APIs:**

| Owner | Change |
| --- | --- |
| `ComparisonImagePlacement`, `ComparisonPlacementFromGeometry` (`comparison_presentation_image.*`) | Reads the full reference extent, render extent, and `render_to_reference` of the executed render. Rejects an empty extent and a non-finite, projective, or singular map. `ToVariantMap` gives QML the extents and the six affine terms. |
| `ConvertRenderedImageForComparison` | Requires host RGBA32F pixels whose size equals `render_extent`, a valid placement, a non-HDR output encoding, and finite values. Then quantizes once with the existing `album_util::MatRgba32fToQImageCopy` (saturating round to nearest, RGBA order kept, deep copy) to `Format_RGBA8888`. No DRT, gamma, or gamut operation. |
| `ComparisonImageStore` (`comparison_image_provider.*`) | Holds one pair (operation id, A, B) under one mutex. `PublishPair` replaces both sides in one locked write; `Clear` releases both. URL `image://alcedo-comparison/<operation>/<a or b>`; a request for another operation returns a null image. Released images are dropped outside the lock. |
| `PublishComparisonPair` | Converts A and B before the store changes, requires equal reference extents, then publishes both. A failure names the side and leaves the previous pair in the store. |
| `ComparisonImageProvider`, `SharedComparisonImageStore` | Serves stored QImages only (no decode or render) and ignores the requested size. Registered in `ApplicationModuleHost::AttachQmlEngine`. |
| `EditorComparisonCanvas.qml` | Fits the reference extent into the canvas (aspect ratio kept, centered). Draws an ordinary `Image` whose local size is the render extent, with one `Matrix4x4` transform: fit * `render_to_reference`. `cache: false`, asynchronous. Uncovered areas stay empty. |
| `EditorComparisonView.qml` | Opaque `cardSurfaceColor` surface that takes all pointer and wheel input. One canvas per side for the life of a pair; complete left/right, complete top/bottom, divider left/right, and divider top/bottom only move and clip the two regions. The divider position is relative to the fitted reference canvas; arrow keys step 0.01 (Shift: 0.1), Home and End reach the bounds. The view reports `dividerPositionRequested` and owns no state. The pair shows only when `status` is ready and both `Image` items are Ready. |
| `EditorComparisonPanel.qml` | Compare title, Before/After or Versions, A and B selectors (`AdjustmentCombo`), display mode, orientation, Swap, Close, Retry, status and HDR text, and the fixed-sensor-settings explanation. Every control reports a request signal. Source and kind choices are disabled while a pair renders or for HDR; view choices and Close stay available. |
| `DESIGN.md`, `docs/VI/README.md` | Product copy rows and per-file VI entries for the three QML files. No new AppTheme token. |

**Deviations from the plan:**

- `path_utils.*` is unchanged. The existing `MatRgba32fToQImageCopy` already gives the approved quantization; validation is in the new conversion function.
- The conversion and placement code is in `comparison_presentation_image.*`, separate from the store and provider in `comparison_image_provider.*`.
- The Compare page is not yet in `EditorAdjustmentStack.qml`, and `compare` is not yet in `NormalizeAdjustmentPanel`. Both belong to Phase 4 step 5, with the session owner that opens the page.
- Escape handling and translations are Phase 4 items (steps 7 and 9). The new strings use `qsTr`; the `.ts` catalog is unchanged.

**Primary success call chain:**

```text
(Phase 4 owner, GUI thread) completed IEditorImageRenderPort result [A, B]
  -> PublishComparisonPair(SharedComparisonImageStore(), operation_id, A, B)
       -> ConvertRenderedImageForComparison(A), then (B)
            -> validate pixels / render_extent / placement / SDR encoding / finite values
            -> MatRgba32fToQImageCopy -> RGBA8888 QImage (own pixels)
       -> equal reference extents -> ComparisonImageStore::PublishPair (one locked write)
  -> ComparisonPairPublication::ToVariantMap -> EditorComparisonView.pair, status "ready"
  -> two EditorComparisonCanvas Images load image://alcedo-comparison/<op>/a and /b
       -> ComparisonImageProvider::requestImage -> ComparisonImageStore::Get (Qt reader thread)
  -> both Image.status Ready -> pairReady -> regions visible
  -> fit * render_to_reference places each image in the common reference canvas
```

**Primary failure and restore call chain:**

```text
invalid pixels | size != render_extent | bad placement | ST 2084/HLG | NaN/Inf | different reference extents
  -> std::invalid_argument that names the side -> the store keeps its previous pair; nothing is published
  -> (Phase 4 owner) status "failed", errorText -> the view shows the error and no images;
     the panel shows Retry and Close

Image load error -> view status text; imageLoadFailed(message) to the owner

Close or replacement
  -> ComparisonImageStore::Clear (pixels dropped outside the lock); view pair = null
  -> both Image sources empty, status Null; a provider read in progress keeps a valid image
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `SdrPairConversionMatchesRoundedClampedRgbaValues` (channel order in raw bytes, alpha, fractions, clamp below 0 and above 1, float buffer released after conversion) | `EditorComparisonImageProviderTest` | PASS |
| `ConversionRejectsInvalidPixelsGeometryAndHdrOutput` (added; no pixels, RGB32F, size mismatch, NaN, Inf, ST 2084, HLG, singular map, empty reference) | `EditorComparisonImageProviderTest` | PASS |
| `PairProviderNeverPublishesOneNewSideWithOneOldSide` (invalid B keeps the old pair; different reference extents rejected; a replacement replaces both; a concurrent reader during 600 replacements sees no image of another operation) | `EditorComparisonImageProviderTest` | PASS |
| `ProviderServesOnlyTheStoredOperationAtItsRenderExtent` (added) | `EditorComparisonImageProviderTest` | PASS |
| `ClearedStoreKeepsImagesAlreadyReadByTheProvider` (added; shared before Clear, sole owner and intact after) | `EditorComparisonImageProviderTest` | PASS |
| `SourceAlignmentPlacesHalfCropInHalfOfReferenceCanvas` (left and right half crop; 8000 x 6000 source with a 1024 long-edge limit) | `EditorComparisonGeometryTest` | PASS |
| `RotatedCropCornersMatchRendererReferenceGeometry` (real `ResolveRenderGeometry`, crop offset, 10 degrees; canvas corners equal fit * renderer map within 1e-3 px) | `EditorComparisonGeometryTest` | PASS |
| `PlacementRejectsEmptyOrNonInvertibleGeometry` (added) | `EditorComparisonGeometryTest` | PASS |
| `PairRemainsHiddenUntilBothImagesAreReady` (both completion orders, held through the provider; failed status) | `EditorComparisonViewQmlTest` | PASS |
| `LayoutAndDividerChangesDoNotSubmitRenderJobs` (2 modes x 2 orientations x swap x 3 divider positions; provider reads stay at 2, sources unchanged, regions do not overlap) | `EditorComparisonViewQmlTest` | PASS |
| `BothDividerOrientationsRevealTheSameReferencePoint` (A and B draw one reference point at one position; the region clip and the divider follow the position; keyboard steps, Home, End) | `EditorComparisonViewQmlTest` | PASS |
| `ClosingComparisonClearsProviderAndItemReferences` (close during a held B read; the read image stays valid; sources and status cleared; no new read; store empty) | `EditorComparisonViewQmlTest` | PASS |
| `PanelReportsRequestsAndDisablesSelectionWhileRendering` (added) | `EditorComparisonViewQmlTest` | PASS |

The QML tests load the production QML from the source tree and use the production store and provider. The test provider wraps `ComparisonImageProvider` as an asynchronous provider so that it can count reads and hold one side. Qt's pixmap reader serves synchronous provider reads one at a time, so a held synchronous read would also block the other side.

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorComparisonImageProviderTest EditorComparisonGeometryTest EditorComparisonViewQmlTest ApplicationModuleHostLifecycleTest ApplicationModuleHostShutdownTest alcedo_main --parallel 4
$env:PATH = "D:/Projects/pu-erh_lab/vcpkg/installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -R '^(EditorComparisonImageProviderTest|EditorComparisonGeometryTest|EditorComparisonViewQmlTest|ApplicationModuleHostLifecycleTest|ApplicationModuleHostShutdownTest)\.' --output-on-failure -j 1
```

The build exited with code 0; qmlcachegen compiled the three QML files into `alcedo_main`. CTest exited with code 0.

| Suite | Discovered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| `EditorComparisonImageProviderTest` | 5 | 5 | 0 | 0 |
| `EditorComparisonGeometryTest` | 3 | 3 | 0 | 0 |
| `EditorComparisonViewQmlTest` | 5 | 5 | 0 | 0 |
| `ApplicationModuleHostLifecycleTest` (caller that registers the provider) | 2 | 2 | 0 | 0 |
| `ApplicationModuleHostShutdownTest` | 8 | 8 | 0 | 0 |

**Manual verification:** not done. The view and panel have no production entry until Phase 4. Check divider drag, focus order, both themes, narrow windows, and display scaling in the application after Phase 4 adds the overlay (see the manual review list in `docs/VI/README.md`).

**Cache counters, resources, and timing:** not applicable; Phase 3 renders nothing. One pair holds two RGBA8 images (about 21.3 MiB for two uncropped 4096 x 2731 images).

**Checklist / exit condition:** three of four boxes are checked. The manual pointer and focus check is open.

**LOC note:** about 2,430 lines: 153 added in 5 tracked files and 2,277 lines in 11 new files. Production: 456 C++ lines and 650 QML lines. Tests: 1,171 lines. This is above the 850-1600 estimate, mostly because of tests. No file is over 1,000 lines; the largest is `editor_comparison_view_qml_test.cpp` at 486 lines.

**Remaining gaps:**

- Manual pointer, focus, theme, and scaling check (Phase 4 provides the entry).
- Compare nav page, `compare` panel key, Escape, and translations: Phase 4.
- The full test suite was not run. Per `AGENTS.md`, only the user starts a full run. `WorkspaceShellTest` was not used.

### Phase 4: Add source selection, entry, restrictions, and restore

**Objective and deliverables.** Complete the user workflow from editor entry to close. Connect the first three phases without changing normal Version checkout behavior.

**Inputs and prerequisites.** Use Phase 1 inputs, Phase 2 image jobs, and Phase 3 presentation. Recheck session admission, pending input, navigation, panel ownership, and shutdown order.

**Modules and APIs.** Add the comparison service and GUI controller. Use existing queue-owned action restriction representation and completion identities. Extend workspace, Versions header, and adjustment nav. Use the Phase 4 file map.

**Data invariants.** Current working values capture once per open comparison. All later A/B selections use that capture for Current/active Version. Only the session owner controls comparison activity and admission. Ordinary checkout still applies every stored RAW and white-balance field.

**Steps.**

1. Add entry actions and HDR-unavailable reasons. Seal earlier accepted parameter input before capture; refuse conflicting mask/session operations.
2. Activate a comparison activity restriction through existing policy evaluation. Include adjustments, graph/mask writes, history movement, Version writes, Paste, and view changes.
3. Supply selectors from the existing Version list through its owner. Do not create another persistent Version catalog.
4. Resolve the selected pair, submit one image job, and reduce its result on the session owner. Convert/publish on the GUI boundary without worker calls into QML.
5. Add the hidden Compare nav page and panel key. Restore the previous panel after close.
6. Cover the viewport and editing overlays while keeping the real viewport item alive underneath. Keep ready-pair view controls independent of session edit availability.
7. Add Close, Escape, Retry, image-switch, workspace-exit, and shutdown paths. Correlate completion with the existing operation identity and closed activity.
8. Remove the restriction on close. Submit the normal current-document/view render to reconcile downstream runtime state. Preserve zoom/pan and active Version.
9. Add translated UI copy and shortcut registration through existing systems. Test normal checkout with differing sensor parameters to prove the temporary policy did not spread into it.

**Success chain.** Entry action -> owner capture/restriction -> source selection -> render pair -> Compare panel/view -> Close -> release -> prior panel and current viewport refresh.

**Failure/restore chain.** Source/render/presentation error -> owner error state -> Retry retains fixed capture or Close releases it -> original working document/Version stay unchanged. Image switch or shutdown closes first and ignores any completed closed operation.

**Tests and observable assertions.**

- `ComparisonAdmissionBlocksEditsAndHistoryButAllowsCloseAndImageSelection`: test command admission and projected action decisions together.
- `CurrentVersionComparisonIncludesCapturedWorkingValuesWithoutSaving`: verify pending-value inclusion, no extra commit, and fixed capture across selector changes.
- `ComparingVersionDoesNotCheckoutAndCheckoutStillAppliesItsSensorSettings`: inspect both production operations.
- `CompareNavPageRestoresPreviousPanelAfterClose`: reuse the PR #244 nav fixture.
- `ClosePreservesViewTransformAndRefreshesCurrentDocument`: verify original zoom/pan, active Version, and normal render request.
- `QueuedPairCompletionAfterImageSwitchCannotReopenComparison`: drive worker completion and owner/navigation ordering.
- `HdrDocumentDisablesEntryAndSelectedHdrVersionReportsReason`: cover current and non-current Version inputs.
- `RepeatedOpenSelectCloseKeepsOneExecutorAndReleasesTemporaryImages`: assert executor/device identity, store size, and bounded ownership.

Add proposed `EditorComparisonServiceTest` and `EditorComparisonControllerTest`. Extend `EditorSessionActionPolicyCq3Test`, `EditorSessionHistoryPortTest`, and `EditorAdjustmentHeaderQmlTest`. Run focused direct caller tests; skip `WorkspaceShellTest` explicitly.

**Build/run.** Build those proposed and existing targets plus `alcedo_main`. Run only their filtered suites. Manually exercise Before/After, A/B selection, all layouts, close during render, image switching, editor re-entry, and application shutdown.

**Exit criteria.**

- [x] Both entry actions and all selectors follow the confirmed state semantics.
- [x] Edit/history restrictions work at the owner and QML boundaries.
- [x] Normal checkout still applies historical sensor settings.
- [x] Close, image switch, exit, and shutdown release temporary state without stale publication.
- [x] Real large-RAW warm-pair resource and timing evidence exists.

**Expected diff.** 1000-1900 lines.

**Completion record.** See the Phase 4 record below.

##### Phase 4 completion record (2026-10-01)

**Status:** partial. All Phase 4 code and tests are complete and pass. The manual check in the real application (the build/run list above, and the open Phase 3 pointer and focus item) is not done: this session could not operate the desktop application.

**Source revision and branch:** based on `d842aa0ce` (Phase 3); branch `feature/editor-comparison-entry-and-restore`, stacked on `feature/editor-comparison-image-presentation`. Not committed when this record was written.

**Implemented behavior and APIs:**

| Owner | Change |
| --- | --- |
| `EditorComparisonKind`, `EditorComparisonStatus`, `EditorComparisonState` (`editor_comparison_types.hpp`) | Kind (Before/After, Versions), status (Inactive, Rendering, Ready, Failed), and the GUI read of the open comparison: sources, opening command id, pair id (the image job id), and the real error. |
| `EditorActionPolicy` | New action `OpenComparison` (Interactive image; denied for HDR output with "Comparison is unavailable for HDR output.", and while a Mask edit owns input). New lease `Comparison`: blocks adjustments, commits, Undo, Redo, head moves, discard, every Version write, Paste, view changes, and a second comparison; Select Image, Close Editor, and Shutdown stay admissible. Every other lease also blocks `OpenComparison`. New inputs `current_output_is_hdr` (DRT of the published working preview) and `mask_input_open`. |
| `EditorComparisonService` (new library `EditorComparisonService`) | Owner-thread collaborator of the session. Holds the working preview captured once at entry, the kind and sources, the image job of the selected pair, and the Ready pair until the GUI takes it. Builds inputs only through `IEditorHistoryPort::BuildComparisonInputs` and renders only through `IEditorImageRenderPort`. A completion is reduced on the owner and is ignored unless its job id is the job of the selected pair, so a closed or replaced comparison never publishes. A failure keeps the comparison open with the real error. |
| `EditorSessionService` | Commands `OpenComparison`, `SelectComparisonSources`, `RetryComparison`, `CloseComparison`. Open settles pending parameter input through the existing seal, captures `CurrentPreview`, takes the `Comparison` lease, and saves nothing. Open, Switch, editor Close, and Shutdown close the comparison before they release the image. Close removes the lease and routes one `EditorRenderReason::ComparisonClosed` Quality render with the viewport region. Queued slider and Mask input is refused at its own admission while comparing. `PostOwnerTask` reduces image-job completions inside one publication. |
| `EditorSessionRuntime::CreateWithPorts`, `ApplicationModuleHost` | New `image_render` port argument; production passes the editor render scheduler port, so pairs render on the editor executor and worker. |
| `EditorRenderReason::ComparisonClosed` | Quality, normal priority, no frame reuse; coordinator name `ComparisonClosed`. |
| `EditorActionAvailabilityModel` | `canOpenComparison`, `openComparisonReason`. |
| `EditorSessionController` | `compare` panel key. The Compare page opens with the comparison and the earlier panel returns on close (Mask falls back to Tone). `compare` is never stored as the startup panel. `CloseComparison(refresh)` sends the viewport region. |
| `WorkspaceRouter::OpenLibrary` | Closes the comparison (no refresh) before it persists the image. |
| `EditorComparisonController` (QML `appModules.editorComparison`) | Projects the backend state, builds source choices (Root, Current, named Versions from the projection of the history owner), and routes Compare actions. On the GUI thread it takes a Ready pair, converts and publishes it with `PublishComparisonPair`, and drops the float images. Display mode, orientation, divider, and swap are local view state; a new comparison resets them. Conversion and image-load failures show their reason; Retry renders again. |
| QML | Viewport text action `Compare` (Before/After) and Versions header `Compare` (Version comparison); the comparison view covers the viewport at z 20 while the viewport item stays alive and visible; the Compare page is nav page 2 (`panel_icons/compare.svg`, Tabler `columns-2`, user-approved); Escape (`comparison.close` in the exclusive `editor.comparison` scope) closes. |
| Translations, VI | `en` and `zh_CN` catalog entries for the comparison panel, view, controller, and entry actions (added by hand; lupdate was not run). DESIGN.md product copy and icon approval; `docs/VI/README.md` Phase 4 entries and manual-review items. |

**Deviations from the plan:**

- The editor has no viewport toolbar. The Before/After action is a text button at the top left of the viewport, hidden while comparing.
- The plan placed the policy assertions in `EditorSessionActionPolicyCq3Test` and history assertions in `EditorSessionHistoryPortTest`. The pure policy test is in Cq3; the owner tests run the production session facade over the real history port in the new `EditorComparisonServiceTest` (`tests/app/`, registered in `tests/ui/CMakeLists.txt` with the history port sources, as `EditorComparisonInputsTest` is). `EditorSessionHistoryPortTest` is unchanged because the history port is unchanged.
- `CompareNavPageRestoresPreviousPanelAfterClose` tests the production `EditorSessionController`; the QML nav fixture of PR #244 has the added `CompareNavPageAppearsOnlyWhileComparingAndRoutesPanelRequests`.
- `RepeatedOpenSelectCloseKeepsOneExecutorAndReleasesTemporaryImages` proves the GUI side (one backend for every pair, store emptied, float buffers released). Executor, device, and queue identity of the pair job is the Phase 2 test `ComparisonHostRequestsKeepEditorExecutorAndQueueIdentity`.
- The timing evidence is a new target `EditorComparisonLargeRawPairTest`, so `editor_session_render_scheduler_port_test.cpp` (1,609 lines) gets no new test before the Phase 5 split. Its small CUDA fixture repeats part of the port test fixture; Phase 5 step 1 can move both onto the shared fixture header.
- Shortcut registry labels are not in the `.ts` catalogs (no existing registry label is); the new command follows that.

**Primary success call chain:**

```text
Viewport "Compare" | Versions header "Compare"
  -> EditorComparisonController::openBeforeAfter / openVersions
  -> EditorSessionService::OpenComparison (command, session owner)
       -> EditorActionPolicy::Evaluate(OpenComparison): Interactive, SDR, no Mask edit, no lease
       -> SettlePendingParameterInputForBoundary (normal seal; no save)
       -> IEditorPipelinePort::CurrentPreview (captured once) -> AcquireLease(Comparison)
       -> EditorComparisonService::Open -> RenderSelectedPair
            -> IEditorHistoryPort::BuildComparisonInputs (Phase 1)
            -> IEditorImageRenderPort::ScheduleImages (Phase 2) -> state Rendering
  -> editor worker: ApplyImage(A), ApplyImage(B) -> completion -> PostOwnerTask
  -> owner: HandleImagesFinished(job id of the selected pair) -> state Ready
  -> change notification -> EditorSessionController::OnBackendChanged
       -> SyncComparisonAdjustmentPanel (Compare page; earlier panel kept)
       -> StateChanged -> EditorComparisonController::Refresh
            -> TakeComparisonImages -> PublishComparisonPair (Phase 3 SDR) -> pair
  -> EditorComparisonView over the viewport -> both Images Ready -> pair shown
Close (panel Close | Escape)
  -> EditorSessionController::CloseComparison(true, viewport region)
  -> owner: CloseComparisonOnOwner -> EditorComparisonService::Close (cancel job, release
     capture, documents, and images) -> release Comparison lease
     -> RouteViewChange(ComparisonClosed, region) -> one Quality render of the current document
  -> GUI: earlier panel restored, store cleared, overlay hidden, zoom/pan unchanged
```

**Primary failure and restore call chain:**

```text
HDR current output -> OpenComparison denied by policy -> actions disabled; tooltip states why
Missing Version | selected HDR Version | replay failure | ScheduleImages rejected | job Failed
  -> EditorComparisonService state Failed (real error); restriction stays
  -> Compare panel error, Retry or Close; working document and active Version unchanged
SDR conversion failure | Image load error -> controller marks that pair failed -> Retry
Image switch | Open | editor Close | Shutdown -> CloseComparisonOnOwner first (CancelImages)
  -> a completion queued behind it carries an old job id -> ignored; no pair is published
Leave the editor (WorkspaceRouter::OpenLibrary) -> CloseComparison(false) before persist
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `ComparisonAdmissionBlocksEditsAndHistoryButAllowsCloseAndImageSelection` (projected decisions, command admission, queued slider and Mask input, no commit; image selection closes the comparison) | `EditorComparisonServiceTest` | PASS |
| `CurrentVersionComparisonIncludesCapturedWorkingValuesWithoutSaving` (unreleased drag value in B, one commit of the user edit, 0 materializations, the same capture on a later selection) | `EditorComparisonServiceTest` | PASS |
| `ComparingVersionDoesNotCheckoutAndCheckoutStillAppliesItsSensorSettings` | `EditorComparisonServiceTest` | PASS |
| `ClosePreservesViewTransformAndRefreshesCurrentDocument` (region of the current view, `ComparisonClosed` Quality render, active Version unchanged; no render when nothing is open) | `EditorComparisonServiceTest` | PASS |
| `QueuedPairCompletionAfterImageSwitchCannotReopenComparison` | `EditorComparisonServiceTest` | PASS |
| `HdrDocumentDisablesEntryAndSelectedHdrVersionReportsReason` (ST 2084 current; HLG Version selected) | `EditorComparisonServiceTest` | PASS |
| `ShutdownDuringPairCancelsTheJobAndIgnoresItsCompletion` (added) | `EditorComparisonServiceTest` | PASS |
| `PairPublishesOnceAndRenderFailureKeepsComparisonOpen` (added) | `EditorComparisonServiceTest` | PASS |
| `ComparisonLeaseBlocksWritesHistoryAndViewButKeepsSelectionCloseAndShutdown` (added) | `EditorSessionActionPolicyCq3Test` | PASS |
| `CompareNavPageRestoresPreviousPanelAfterClose` (production controller; compare not stored) | `EditorComparisonControllerTest` | PASS |
| `RepeatedOpenSelectCloseKeepsOneExecutorAndReleasesTemporaryImages` (3 cycles; see deviations) | `EditorComparisonControllerTest` | PASS |
| `LeavingTheEditorClosesTheComparisonWithoutARefresh` (added) | `EditorComparisonControllerTest` | PASS |
| `ConversionAndImageLoadFailuresShowTheErrorAndRetryRenders` (added) | `EditorComparisonControllerTest` | PASS |
| `SourceChoicesComeFromTheVersionListAndMapToBackendSources` (added) | `EditorComparisonControllerTest` | PASS |
| `CompareNavPageAppearsOnlyWhileComparingAndRoutesPanelRequests` (added) | `EditorAdjustmentHeaderQmlTest` | PASS |
| `ComparisonCloseOwnsEscapeOnlyInsideTheComparisonScope` (added) | `ShortcutRegistryTest` | PASS |
| `LargeRawPairReusesTheSensorResultAndRecordsTiming` (added) | `EditorComparisonLargeRawPairTest` (CUDA) | PASS |

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorComparisonServiceTest EditorSessionActionPolicyCq3Test EditorRenderCoordinatorTest EditorComparisonControllerTest EditorAdjustmentHeaderQmlTest ShortcutRegistryTest ApplicationModuleHostLifecycleTest EditorComparisonLargeRawPairTest EditorSessionControllerPhase5ATest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main EditorComparisonViewQmlTest EditorComparisonImageProviderTest ApplicationModuleHostShutdownTest --parallel 4
$env:PATH = "D:/Projects/pu-erh_lab/vcpkg/installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -R '^(EditorComparisonServiceTest|EditorSessionActionPolicyCq3Test|EditorRenderCoordinatorTest|EditorComparisonControllerTest|EditorAdjustmentHeaderQmlTest|ShortcutRegistryTest|ApplicationModuleHostLifecycleTest|EditorSessionControllerPhase5ATest)\.' --output-on-failure -j 1
ctest --test-dir build/debug -R '^(EditorComparisonViewQmlTest|EditorComparisonImageProviderTest|ApplicationModuleHostShutdownTest|EditorComparisonLargeRawPairTest)\.' --output-on-failure -j 1
```

Both builds exited with code 0; qmlcachegen compiled the changed QML into `alcedo_main`. The first CTest command exited with code 8 because of one failure that predates this phase; the second exited with code 0.

| Suite | Discovered | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: | ---: |
| `EditorComparisonServiceTest` | 8 | 8 | 0 | 0 |
| `EditorComparisonControllerTest` | 5 | 5 | 0 | 0 |
| `EditorSessionActionPolicyCq3Test` | 13 | 12 | 1 | 0 |
| `EditorRenderCoordinatorTest` | 33 | 33 | 0 | 0 |
| `EditorAdjustmentHeaderQmlTest` | 24 | 24 | 0 | 0 |
| `ShortcutRegistryTest` | 18 | 18 | 0 | 0 |
| `ApplicationModuleHostLifecycleTest` | 2 | 2 | 0 | 0 |
| `EditorSessionControllerPhase5ATest` | 52 | 52 | 0 | 0 |
| `EditorComparisonViewQmlTest`, `EditorComparisonImageProviderTest`, `ApplicationModuleHostShutdownTest` | 18 | 18 | 0 | 0 |
| `EditorComparisonLargeRawPairTest` (CUDA) | 1 | 1 | 0 | 0 |

The failure is `EditorSessionActionPolicyCq3Test.AdjustmentPanelsReloadOnlyWhenCommittedContentChanges` (`history_revision` stays 1 after a settled commit through the fake history port). It also fails on clean `d842aa0ce` (Phase 4 changes stashed, target rebuilt, test run directly), so this phase did not cause it.

**Manual verification:** not done. Exercise in the real application: Before/After and Version selection, all layouts, divider drag and focus (the open Phase 3 item), close during render, Escape, image switching, editor re-entry, both themes, display scaling, and application shutdown during a render.

**Cache counters, resources, and timing** (`EditorComparisonLargeRawPairTest`): `raw/camera/sony/a7rv/DSC00064.ARW`, 9728 x 6656 raw (Bayer), reference 9496 x 6328, output 4096 x 2730, CUDA, Debug build, current white balance different from the root.

| Measurement | Value |
| --- | --- |
| Cold pair (first work on the binding) | 4,581 ms; 1 LibRaw unpack, 1 sensor develop |
| Warm pair (after one viewport frame) | 842 ms; 0 unpacks, 0 prepared-source misses, 0 sensor develops, 2 sensor skips |
| SDR conversion of both images (GUI) | 461 ms |
| Live comparison images (RGBA8, two) | 89,456,640 bytes (85.3 MiB) |
| Prepared sources / published results / transient bytes after the warm pair | 1 (129,499,136 host bytes) / 4, unchanged by the pair / 0 |

The 21.3 MiB figure for two RGBA8 images in section 9 and in the Phase 3 record is wrong: two uncropped 4096 x 2731 RGBA8 images use about 85.3 MiB (two RGBA32F images about 341 MiB). Separate A and B GPU and download times and the QML image-ready time were not measured; the pair time includes both renders and both downloads.

**Checklist / exit condition:** all five boxes are checked from executed tests. The manual application check of the build/run list is open, so the status is partial.

**LOC note:** about 3,300 changed lines: about 1,200 added and 20 removed in 34 tracked files (258 of them translations), plus about 2,100 lines in 8 new files (1,211 of them tests). This is above the 1000-1900 estimate and the 2000-line split guideline, mostly because of tests and translations; production code is about 1,300 lines. New files are below 400 lines. Existing files over 1,000 lines grew: `editor_session_service.cpp` (2,527 to 2,703), `editor_session_controller.cpp` (1,881 to 1,936), and `editor_adjustment_header_qml_test.cpp` (1,183 to 1,286). The comparison logic is in its own owner; splitting those files is not part of this phase.

**Remaining gaps:**

- Manual application check (above).
- The full test suite was not run. Per `AGENTS.md`, only the user starts a full run. `WorkspaceShellTest` was not used.
- Phase 5: OpenCL and Metal for the image job, and the port test split.

### Phase 5: Prove the image job on the OpenCL and Metal backends

**Objective and deliverables.** Make the Phase 2 image job a proven behavior of every product backend, not only CUDA. Run the Phase 2 acceptance tests on OpenCL (Windows) and Metal (macOS). Correct each backend defect that they find.

**Inputs and prerequisites.** Phase 2 is complete. The image operation (`Renderer<Backend>::RenderImage`, `PipelineExecutor::ApplyImage`) is one backend-independent template, and `PipelineExecutor` dispatches all three backends to it. Phase 2 evidence has these limits:

| Backend | Compiled in the Phase 2 build | Phase 2 tests executed |
| --- | --- | --- |
| CUDA | Yes (`win_debug`) | Yes: all GPU tests in `EditorSessionRenderSchedulerPortTest` and `PipelineDocumentRenderTest` |
| OpenCL | Yes (`win_debug` defines `HAVE_OPENCL`; `Renderer<OpenClBackend>::RenderImage` is compiled into the executor) | No. Every Phase 2 GPU test selects CUDA. |
| Metal | No (`ALCEDO_ENABLE_METAL` is OFF on Windows; only `macos_debug` and `macos_release` compile it) | No |

Phase 5 does not depend on Phases 3 and 4. It can run before or after them. It must complete before the feature is reported as available on Metal or OpenCL.

**Modules and APIs.** No new production API. Test changes, and the backend corrections that the tests require. Use the Phase 5 file map.

**Data invariants.** Each Phase 2 invariant holds on each backend:

- The port's one Interactive executor, device, queue, and binding render A and B consecutively on the single worker.
- A null request sink presents nothing; the attached viewport sink stays attached.
- A warm pair causes 0 LibRaw unpacks, 0 prepared-source misses, and 0 sensor-develop executions. It adds no published or unpublished result.
- `RenderedPipelineImage::geometry` equals the geometry of the presented frame for the same request. The pixel extent equals `render_extent`.
- A failure or cancellation publishes no image and leaves the editor results usable.

No backend gets another decode, resolution, operator, or backend in place of the requested path. A backend that cannot satisfy an invariant fails with its real error, and the defect is corrected in that backend.

**Steps.**

1. Split the port test file first. `editor_session_render_scheduler_port_test.cpp` has 1,609 lines (Phase 2 LOC note). Move the GPU fixture, `BlockingLutResolver`, and the image-job helpers into `tests/ui/support/editor_render_port_gpu_fixture.hpp`. Move the image-job tests into `tests/ui/editor_session_render_scheduler_port_image_job_test.cpp`. Register the new source in the same `EditorSessionRenderSchedulerPortTest` target. Keep test names unchanged.
2. Make the GPU fixture backend-parameterized. Use a gtest value parameter of `AcceleratorBackendPreference` with the compiled backends (`HAVE_CUDA`, `HAVE_OPENCL`, `HAVE_METAL`). Skip a backend only when its device is unavailable, and report that skip by name. Give `PipelineMgmtService` the parameter backend. Replace the CUDA-only reads (`DebugCudaRenderer`, `InteractiveCudaBinding`) with a test helper that reads `Stats`, `Resources`, `Binding`, `DebugDeviceIdentity`, `DebugQueueIdentity`, and the unpublished-result count from the executor's interactive renderer of the selected backend. Use only the const `Device()` accessor; the test target does not link the backend runtime libraries.
3. Build `FreshImage` from a new `PipelineExecutor` with the parameter backend. Each backend's pixels must match a fresh render on the same backend. Declare the float tolerance per backend at the fixture. Start from `2e-5`. A larger tolerance needs a stated reason from the existing backend tests (for example, the tolerance that `GpuDagOpenClDrtProductTest` already uses for the same output).
4. Run the nine Phase 2 GPU port tests on each backend, without changes to their assertions.
5. Add the executor-level check to each backend's existing renderer target. In `GpuDagOpenClDrtProductTest` (`opencl_drt_product_test.cpp`) and `GpuDagMetalRendererTest` (`metal_renderer_test.cpp`), add `RenderImageReturnsExecutedGeometryWithoutPresentingToTheSink`. Use their existing Quality Base fixtures. Assert the same items as the CUDA `ApplyImageReturnsExecutedGeometryWithoutPresentingToTheSink`.
6. Make `PipelineDocumentRenderTest` cover OpenCL as well. Today it is registered only under `ALCEDO_CUDA_ENABLED` and selects CUDA. Either parameterize it by backend and register it when any GPU backend is enabled, or add the same `ApplyImage` case to the OpenCL and Metal targets in step 5. Record the choice in the completion record.
7. Build Metal on macOS with `macos_debug` and `-DALCEDO_BUILD_TESTS=ON`. Run the same filtered suites there. `AlbumBackendLib` and the port test must build on macOS; correct any platform build defect in the changed files.
8. For each failed invariant, find the backend cause, correct it in that backend's renderer, workspace, or presenter, and add or keep a test that shows the failure before the correction.

**Success chain.** Backend parameter -> port executor with that backend -> warm frame -> `ScheduleImages(A, B)` -> `ApplyImage(A)` and `ApplyImage(B)` on that backend's interactive renderer -> both images -> each matches a fresh render on that backend; sensor result reused.

**Failure chain.** A backend invariant fails -> the test reports the backend name and the real error or counter -> the backend defect is corrected, or the phase stays partial with that backend listed as not supported for comparison. No backend runs another backend's path in its place.

**Tests and observable assertions.**

- The nine Phase 2 GPU port tests, unchanged in name and assertion, for each compiled and available backend: `WarmComparisonPairReusesEditorSensorResult`, `WhiteBalanceDifferenceChangesPairPixelsWithoutSensorExecution`, `ComparisonHostRequestsKeepEditorExecutorAndQueueIdentity`, `ComparisonPairDoesNotInterleaveAAndBWithNormalFrames`, `DifferentVersionTopologyRendersCorrectPixelsWithSharedSensor`, `AResultRemainsValidAfterBAndTransientRelease`, `ComparisonCloseDuringARenderSkipsBAndCannotPublish`, `ComparisonFailureAllowsNormalCurrentDocumentRender`, and `ShutdownDuringAImageCancelsAndWaitsForTheJob`.
- `RenderImageReturnsExecutedGeometryWithoutPresentingToTheSink` in `GpuDagOpenClDrtProductTest` and `GpuDagMetalRendererTest`.
- The existing Quality Base cache tests of each backend target stay green: `QualityBaseBypassesEveryResultCacheAfterSensorDevelop` and `QualityBasePixelsMatchFreshExecutionWithinDeclaredTolerance`.

**Build/run.**

Windows (`win_debug`, CUDA and OpenCL):

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionRenderSchedulerPortTest PipelineDocumentRenderTest GpuDagOpenClDrtProductTest GpuDagCudaDrtProductTest --parallel 4
$env:PATH = "D:/Projects/pu-erh_lab/vcpkg/installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -R '^(EditorSessionRenderSchedulerPortTest|PipelineDocumentRenderTest|GpuDagOpenClDrtProductTest|GpuDagCudaDrtProductTest)\.' --output-on-failure -j 1
```

macOS (`macos_debug`, Metal):

```bash
cmake --preset macos_debug -DALCEDO_BUILD_TESTS=ON
cmake --build --preset macos_debug --target EditorSessionRenderSchedulerPortTest GpuDagMetalRendererTest
ctest --test-dir build/macos-debug -R '^(EditorSessionRenderSchedulerPortTest|GpuDagMetalRendererTest)\.' --output-on-failure -j 1
```

Report the counts per backend. A backend skipped because no device was found counts as not verified.

**Exit criteria.**

- [ ] The nine Phase 2 GPU port tests pass on OpenCL.
- [ ] The nine Phase 2 GPU port tests pass on Metal.
- [ ] `RenderImageReturnsExecutedGeometryWithoutPresentingToTheSink` passes on OpenCL and Metal.
- [ ] Each backend's warm pair shows 0 LibRaw unpacks and 0 sensor-develop executions.
- [ ] The port test file is split, and no touched test file is over 1,000 lines.
- [ ] Each backend defect found is corrected with a test; no backend substitution was added.

**Expected diff.** 500-1100 lines, mostly the test split and backend parameterization. Backend corrections add to this. If they would take the phase over 2000 lines, split them into a separate phase before implementation.

**Completion record.** Not started. Fill section 11 after implementation.

## 9. Acceptance and resource evidence

| Behavior | Required evidence |
| --- | --- |
| Root/current and two Versions | Correct white balance/downstream parameters with current sensor values and stamps; unchanged live history |
| Warm cache | Zero additional LibRaw unpack and sensor-develop executions for A/B after warming the current image |
| Cold cache | At most one successful sensor computation for a pair on an otherwise valid cold current binding; never lower decode quality |
| Different topology | Each side matches a fresh execution of its derived document within its declared float tolerance |
| Quality Base | FULL decode, 4096 long edge, Preview resample, SensorDevelopOnly, null sink, and no scope publication |
| Spatial alignment | Common full reference bounds, exact crop/rotation mapping, empty uncovered areas |
| Temporary images | One ready pair; close clears provider/items; incomplete results release on failure |
| Runtime restore | Normal current-document render produces correct pixels after A/B without sensor recomputation when valid |
| HDR | PQ/HLG disables or rejects comparison; no SDR substitute |
| SDR | Explicit 8-bit quantization assertion; no second color transform |
| Session behavior | No save, commit, checkout, dirty-state change, or persistence schema change from comparison |
| Async behavior | Tested close, queued completion, switch, and shutdown through actual worker/owner boundaries |
| Qt UI | Both themes, all layouts, divider endpoints/keyboard, narrow window, and focus verified in the application |

Measure a real large RAW with the configured RAW method and lens settings. Record cold and warm pair wall time. Separate A/B GPU execution, downloads, SDR conversion, and image-ready time. Record source dimensions, backend, build mode, and output dimensions. A 4K limit reduces downstream work; it does not prove that every operator is fast.

Use the same already-landed renderer counters for cache evidence. Do not add a profiler framework. Record prepared-source entries, sensor pass count, GPU transient bytes, and live comparison image bytes before, during, and after repeated open/close cycles.

For an uncropped 4096 x 2731 output, two RGBA32F host images use about 85.3 MiB. Two RGBA8 images use about 21.3 MiB. Process sides consecutively and release float outputs after conversion. Qt upload storage is additional. Do not retain floats merely to support divider movement or mode switching.

## 10. Build and verification commands

These are commands for implementation. Plan creation runs no build or product test.

Use PowerShell at the repository root. Read actual CMake target registration before running a command. Proposed targets must be added in their phase. Use one build at a time. Start each build with at least a 20-minute allowance and poll the same process without restarting healthy compiler/linker work.

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/misc/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target <phase-targets> --parallel 4
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -N -R '<phase-prefixes>'
ctest --test-dir build/debug -R '<phase-prefixes>' --output-on-failure -j 1
```

Use the phase-specific targets listed above. Put logs under `build/tmp/editor_comparison/`. Refresh stale test-runtime DLL copies if a changed first-party DLL did not relink its caller. Do not run CTest while a build copies DLLs.

For release application integration:

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_release --target alcedo_main --parallel 4
```

On macOS, use `macos_debug` in `build/macos-debug` with `-DALCEDO_BUILD_TESTS=ON` when tests are needed. Build the corresponding registered targets. Record unavailable platform/backend results rather than inferring them from CUDA. Keep shader and RAW algorithms unchanged unless source evidence requires a reviewed change.

Search touched source and all roadmap filenames for prohibited terminology. Search roadmap content and review matches against current rules. Keep unrelated historical wording outside the feature diff. Verify source links, new-file LF endings, QML registrations, direct dependencies, and final diff scope.

Full test suite: not run unless the user explicitly requests it. Offscreen workspace input coverage: skipped where unreliable, with manual evidence stated separately.

## 11. Completion records and stop conditions

The Phase 1, Phase 2, Phase 3, and Phase 4 records are under their phases in section 8.

Fill one record for each phase after implementation:

```text
Phase / date / status:
Source revision and branch:
Actual changed modules and changed-line count:
Implemented behavior:
Unimplemented required items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual verification and exact build:
Cache counters, resource measurements, and timing:
Evidence location:
Unavailable platforms or remaining defects:
```

Stop and revise the design when sensor stamps or binding identity cannot remain consistent. Do not fake cache hits. Stop when derived graphs have different source/Develop identities or camera bindings. Reject the input with the real reason.

Stop when the port exposes its executor to another owner, adds another worker, or changes normal checkout behavior. Correct the ownership boundary before continuing.

Stop when a phase can exceed 2000 changed lines. Split it before implementation. Stop when a new consistency mechanism has no executable production ordering test. Stop when a failure path needs an unapproved backend, quality, or SDR substitution.

Keep evidence of real failures and skipped coverage. Do not mark a phase complete from source inspection alone.
