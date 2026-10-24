# SM8 GPU Coverage and One Display Composite - Phase Plan

Date: 2026-10-24. Status: SM8.1 complete on 2026-10-24 on Windows and macOS (section 7.1.1).
SM8.2 complete on 2026-10-24 on Windows CUDA (section 7.2.1).
SM8.3 complete on 2026-10-24 on Windows OpenCL (section 7.3.1).
SM8.4 complete on 2026-10-24 on macOS Metal (section 7.4.1).
SM8.5 complete on 2026-10-24 on Windows and macOS (section 7.5.1).
SM8.6 complete on 2026-10-24 on Windows and macOS (section 7.6.1).
SM8.7 and SM8.8 are planned; no implementation evidence exists for them.

Parent: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), sections 1.5 and 9.7.
Prerequisites: [SM4](phase_sm4_analysis_render_plan.md), [SM5](phase_sm5_ai_mask_source_and_session_plan.md),
and [SM6](phase_sm6_raster_mask_evaluate_plan.md). Source audit: `b3e1d479f60caa70b47205504348287b30114b47`, `main`.
Performance defect: [#494](https://github.com/zidage/AlcedoStudio/issues/494).
Tracking: [#499](https://github.com/zidage/AlcedoStudio/issues/499).
Display transport: #500. Composite: #501. Session integration: #502.

## 1. Product decisions and scope

The user requested an earlier SM8 and a later SM7 on 2026-10-24.
SM8 owns GPU edge processing, coverage presentation, and one display composite.
SM9 adds the AI Mask controls to these operations.
SM7 adds the misalignment notice after SM9.

The temporary edit stack must retain earlier model results.
A new model request must not erase the displayed result.
The candidate and the committed Mask use the same display composite.
The photo document stays unchanged until Commit.
The project asset remains the coverage before Feather and Expand.

These implementation choices replace the old SM8 outline:

| ID | Selected design |
| --- | --- |
| C1 | Keep history in the existing `AiMaskEditSession` and `AiMaskModelOutputStore`. Do not add GPU history. |
| C2 | Give `BasicRenderWorkspace` the current source R8, processed R8, and necessary algorithm work textures. Use its backend and submission lifetime. |
| C3 | Run Expand and Feather on the selected CUDA, OpenCL, or Metal backend. Use the same operations for candidate display, committed rendering, thumbnails, and export. |
| C4 | Add a coverage-only worker operation. It uses the current render device without compiling or executing the photo DAG. |
| C5 | Carry coverage to the viewport with the existing GPU presentation adapters. Use a dedicated coverage channel and the existing RGBA32F format. The red channel stores coverage. |
| C6 | Composite coverage once in `editor_viewport.frag`, after base/detail photo selection. Do not add another overlay composite to the photo DAG or a candidate-only QSG effect. |
| C7 | Keep the previous displayed coverage while a model or GPU request runs. Publish a replacement only after its GPU work completes. Release the previous display resource after its reader finishes. |
| C8 | Select one preview source at a time: a candidate, one committed Mask, or a Grade Mask Union. Selection changes the input, not the composite implementation. |
| C9 | Retain current full-resolution processing and the F8 equations. Do not reduce resolution, replace the Gaussian, or switch backend on failure. |
| C10 | Put coverage visibility and tint in presentation state. They do not change the photo DAG key, exported pixels, scope input, or main history. |

GPU residency covers the edge-processing and display path after an R8 upload.
CPU model outputs and the stored asset remain available for history and persistence.
This plan does not move the sidecar RPC, model-output conversion, or ordered component fold to the GPU.
Those operations already have owners in SM3 and SM5.
There is no second implementation of them in SM8.

Luminance Range and Color Range Masks remain excluded by the master plan.
They are not prerequisites for coverage display.
SM8 must not add their controls, persistence fields, or evaluation.

## 2. Current source audit

Paths below are relative to `alcedo_studio/src/`, unless stated otherwise.

| Owner and file | Verified current behavior | Required change |
| --- | --- | --- |
| `app/ai_mask_edit_session.cpp` | `states_` stores recipe, edge, and input mode. `Undo` and `Redo` compose from retained outputs. `stored_` preserves the opened Mask. `Coverage()` returns a span into mutable owner storage. | Keep these history rules. Give asynchronous upload a stable read lifetime. |
| `app/ai_mask_model_output_store.cpp` | Native-grid task outputs and decoder levels stay for the store lifetime. Calls block on one owner thread. | Keep earlier outputs when a new result arrives. No new history store. |
| `include/edit/runtime/mask_coverage_texture_cache.hpp` | A miss reads the asset, runs host F8, and uploads the processed R8. Keys include asset, analysis size, and edge. The budget is 256 MiB. | Separate source upload from GPU edge processing. Do not cache each slider position as permanent interactive state. |
| `include/edit/runtime/basic_render_workspace.hpp` | The workspace owns the cache, texture pool, command resources, and session release. | Keep the same resource owner. Add coverage-only work without resetting photo results. |
| `include/edit/runtime/basic_render_device.hpp` | The device is not thread-safe. It owns submission context and serial execution. | Run coverage-only work on the same executor as photo work. |
| `edit/runtime/{cuda,opencl,metal}/*mask_pass*` | Committed AI Masks sample cached coverage through `MakeRasterMaskSamplingPlan`. Invert and opacity precede Union. | Acquire GPU-processed coverage from the common owner operation. |
| `include/app/editor_session_service.hpp`, `app/editor_session_service.cpp` | Open, Commit, and Cancel exist. The AI edit lease blocks document writes. Commit stores the asset before history publication. | Bind candidate presentation to the existing session lifecycle. |
| `include/ui/editor_rhi/frame_presentation_lease.hpp` | Presentation supports RGBA32F and three photo layers. Native adapters synchronize producer and reader lifetimes. | Add a separate coverage channel. Do not reuse a photo or detail slot for it. |
| `ui/editor_rhi/shaders/editor_viewport.frag` | One draw selects the base photo and optional detail patch. | Add one coverage sample and tint operation to this draw. |
| `ui/editor_rhi/editor_overlay_item.cpp` | QSG nodes draw existing controls and input markers. | Keep controls above the photo and coverage. Do not duplicate texture processing here. |
| `tests/app/ai_mask_edit_session_test.cpp` | Session Undo, Redo, stored-source editing, and no-model replay have coverage. | Extend lifetime and model-request retention assertions. |
| `tests/edit/runtime/ai_mask_raster_test_support.hpp` | Shared fixtures check raster sampling on the three backends. | Add independent edge-processing comparisons. |

The host measurement in #494 is historical evidence, not a new measurement of this plan.
SM6 completed its stated host design. SM8 replaces that design prospectively.
Keep the SM6 completion record and its measured failures intact.

## 3. Ownership and retained data

| Data | Owner | Retention and release |
| --- | --- | --- |
| Committed original R8 | Project `MaskAssetStore` through `PipelineMgmtService` | Existing content-addressed asset and history reachability. A new candidate never overwrites it. |
| Opened Mask source and coverage | Existing `AiMaskEditSession::stored_` | Until edit-session destruction. Undo can return to it without inference. |
| Earlier model outputs | Existing `AiMaskModelOutputStore` | Native grids for the session lifetime. Preserve the current SM5 M7 behavior. |
| Session stack | Existing `AiMaskEditSession::states_` | Recipes and scalar parameters. No full R8 or GPU texture per entry. |
| Current composed original coverage | `AiMaskEditSession` | One immutable algorithm output. Replace it when the recipe changes. Edge-only edits share it. |
| Upload input still in use | A scoped shared read of that immutable output | Keep the same allocation until upload completes. Do not copy the image just to cross a queue. |
| Current source and processed GPU coverage | Workspace Mask coverage owner | Retain current work only. A replacement can coexist with the displayed predecessor until publication completes. |
| GPU work textures | Workspace texture pool | Necessary morphology and float Gaussian outputs. Reuse after GPU completion. No historical outputs. |
| Coverage display target | Existing presentation queue and native adapter | Existing producer/reader release rules. A pending target cannot overwrite a displayed target. |

Change the current composed buffer to shared immutable storage if asynchronous upload needs it.
Keep `Coverage()` as a scoped view for existing synchronous callers.
Add `AcquireCoveragePixels()` to return a shared const read of the same allocation.
Do not make another full-frame copy in that accessor.
This is an algorithm output with an extended read lifetime, not a new session-state representation.
Document the owner, immutability, and release condition at this accessor.

Historical GPU residency is unnecessary: Undo reconstructs the current original R8 from retained model outputs.
It uploads that R8 and reruns the GPU edge operation.
Undo does not call the model, write an asset, or append main history.
The stored source, model outputs, current coverage, and displayed coverage have different purposes.
Do not collapse their lifetimes into one mutable texture.

## 4. Requests, publication, and errors

### 4.1 Owner APIs

Names below are proposed APIs. They are not claims about existing functions.

| Operation | Input and output | Owner and validation |
| --- | --- | --- |
| `AiMaskEditSession::AcquireCoveragePixels()` | Shared const read of current R8; no mutation | Session worker. Validate the session extent before publication. |
| `UpdateAiMaskCandidateCoverage(pixels, edge)` | Existing shared pixel allocation and scalar edge values; completion or real error | App facade checks the active AI edit lease and current image. Render worker owns all GPU changes. |
| `SelectMaskCoveragePreview(grade_id, optional_mask_id)` | One Mask or Grade Union selection; completion or real error | App resolves live data through its owner. Do not copy the document for a preview. |
| `ClearMaskCoveragePreview()` | Clears the selected preview and retires its display target | App session and display owner. Idempotent close operation. |
| Workspace `AcquireAiMaskCoverage(...)` | Raw source, analysis extent, edge; a scoped processed texture read | Existing coverage owner. Validate bounds, extents, and parameter ranges before allocation. |
| Coverage-only render operation | Selected coverage and existing geometry; GPU presentation completion | Current render executor. No RAW prepare, DAG compile, photo draw, or history mutation. |

Keep request parameters narrow. Reuse `AiMaskEdgeAdjustment`, `MaskAssetDescriptor`, and geometry types.
Use an existing owner read for a committed source or Union.
New request types may describe an operation. They must not mirror a complete session or document.

### 4.2 Queue policy

The AI Mask tool controller owns one `AiMaskEditSession` on one serial worker.
Blocking inference runs there, not on the GUI thread or the render worker.
The current GPU device remains exclusive to its existing render executor.
The tool submits candidate uploads to that executor through the app facade.

Allow one model request at a time. Disable prompt submission, session Undo/Redo, and Commit while it runs.
Keep view navigation and Cancel available.
Keep one edge computation active and only the newest pending scalar edge setting.
Do not drop an accepted prompt or a stored Undo step when merging display work.
SM9 records one edge edit at pointer release or keyboard acceptance.
Intermediate slider values request display work without appending stack entries.

Reuse existing operation IDs, image-load IDs, ordered owner commands, and presentation completion rules.
Do not add an independent revision counter or cancellation protocol.
Before extending an existing stale-result check, prove the actual queue interleaving required by `AGENTS.md`.

An upload cannot retain `Coverage()` as a bare span after its owner changes coverage.
The shared immutable read in section 3 supplies that concrete lifetime requirement.
Cancel or image navigation closes the presentation through existing image/session identity checks.
Let running work finish safely. Release its input and result on their owning executors.
Do not destroy a blocking model session from another thread.

### 4.3 Publication rules

1. Keep the current stack and displayed coverage while inference runs.
2. Validate a successful model result before the existing session accepts a step.
3. Acquire a stable read of the newly composed original coverage.
4. Upload it and compute edge processing into unpublished GPU outputs.
5. Produce a coverage display target and complete its existing presentation synchronization.
6. Replace the displayed target. Retire the prior target after the reader releases it.

Inference failure or `NotFound` adds no stack entry and keeps the previous coverage.
A GPU failure can occur after the session accepts a step.
Keep that desired session state, report the real display error, and keep the previous displayed target.
Mark it explicitly as the previous result. Disable Commit until Retry displays the current state successfully.
Retry uses the retained model outputs. It must not run inference again.
The error must not be reported as a successful preview of the current parameters.

Commit uses `PrepareCommit()` for the original R8, then the existing asset-first history publication.
Do not read adjusted GPU pixels back to create the project asset.
Keep the candidate visible until the committed replacement frame is ready.
A failed asset write or history publication leaves the session open under the existing SM5 rule.
Cancel retires candidate display and restores the selected committed preview, or the photo alone when none was selected.

## 5. GPU processing and the single display composite

### 5.1 Edge computation

Preserve SM5 M8 and SM6 R3-R5: analysis pixels, margin, clipping, and replicated image borders.
Apply the alternating 3x3 cross and square dilate/erode operations before Feather.
Normalize the R8 values to float for the two separable Gaussian passes.
Use the same finite Gaussian coefficients and support as the host implementation.
Keep the horizontal intermediate in float. Quantize to R8 only after the vertical pass.
With both edge fields zero, use the uploaded original R8 without an edge dispatch.

Do not use repeated box blur, a recursive approximation, or a reduced-resolution raster.
The host implementation remains a test reference. It is not a runtime substitute on GPU failure.
Each backend must pass independent equation checks and the existing R8 tolerance.
Integer morphology must match exactly. Compare Gaussian R8 against the independent reference within one code value.
Measure the maximum backend-to-host difference separately; do not claim byte equality from a one-code comparison.

Separate upload identity from edge identity.
For a candidate, reuse the immutable current allocation while its read owner remains alive.
For a committed Mask, use the existing asset key and analysis extent.
Do not add a generation counter solely to identify current pixels.
A Feather or Expand change reuses source R8 and performs no upload.
Interactive updates keep one current processed result, plus results with actual GPU/display readers.
Committed reuse can retain bounded entries under the existing 256 MiB policy.
Charge retained sources, processed results, and peak work textures in measurements.
Do not present the existing cache budget as a hard total device-memory limit.
Do not allocate a result for every historical parameter value.

### 5.2 Presentation transport

Use a dedicated coverage presentation channel with the existing fixed-slot queue and native adapters.
Preserve the existing three photo-layer meanings.
Do not make coverage look like `DetailPatch` or replace the current photo frame.
Reuse RGBA32F target allocation and synchronization. Write coverage in red, zero in green/blue, and one in alpha.
This transport is an explicit GPU representation conversion, not CPU readback.

The coverage-only operation maps processed coverage to the current full photo-frame grid.
Use `MakeRasterMaskSamplingPlan` and the same sampling rule as committed `MaskEvaluate`.
Candidate preview uses the effective source coverage with its selected edge settings.
For a new Mask, use default invert and opacity. For an edited Mask, read its unchanged fields through its owner.
Committed preview includes the selected Mask's invert and opacity, or the Grade Union as requested.
Do not tint effective coverage twice.
Reuse a valid published Union when available.
Otherwise run only the existing Mask evaluation and Union operations needed by the selection.
Range Masks are excluded, so this operation has no photo-content dependency.
Do not trigger a full photo render to obtain analytic or AI coverage.

Attach the existing frame geometry and image identity to coverage presentation.
The viewport samples the photo and coverage with the same geometry and clipping.
Handle crop, rotation, full-frame preview, and detail ROI without independent coordinate rounding.
Zoom and pan resample current coverage. They do not repeat edge processing or source upload.
A display-grid resize can regenerate the presentation target from resident processed R8.
The full analysis grid used for edge processing never changes for display size.

### 5.3 One composite

Extend the existing viewport draw. First select the base/detail photo color.
Then compute `a = clamp(coverage, 0, 1) * overlay_opacity`.
Set the display RGB to `photo_rgb * (1 - a) + overlay_rgb * a`.
Keep photo alpha and HDR values outside covered pixels unchanged.
Do not clamp the full photo to [0, 1].

Use the same shader branch for candidate, committed single Mask, and Grade Union.
The controls remain in the existing QSG overlay above this draw.
Coverage is decorative and accepts no pointer or keyboard input.
The final display frame supplied to scopes remains the photo without the coverage tint.
Export and thumbnails apply the Mask to the photo, but never include this display tint.

Add named `AppTheme` roles for coverage color and opacity in the same change as `DESIGN.md`.
Use the existing semantic accent for the default tint and opacity 0.4.
SM9 exposes the existing-style visibility control; SM8 does not create another settings panel.
There is no second runtime composite after DRT. This viewport draw is the SM8 display composite.

## 6. File and build map

Existing files to change:

- `include/app/ai_mask_edit_session.hpp`, `app/ai_mask_edit_session.cpp`: immutable read lifetime.
- `include/app/editor_session_service.hpp`, `app/editor_session_service.cpp`: focused preview facade and lifecycle.
- `include/app/editor_session_ports.hpp`, `include/app/editor_session_command_queue.hpp`: typed owner operations.
- `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp`: render-executor submission.
- `include/edit/runtime/basic_render_workspace.hpp`, `basic_render_device.hpp`, `mask_coverage_texture_cache.hpp`: coverage resource owner.
- `edit/runtime/cuda/cuda_mask_pass.cu`, `opencl/opencl_mask_pass.cpp`, `metal/metal_mask_pass.mm`: shared acquisition.
- `include/ui/editor_rhi/frame_presentation_lease.hpp`, `direct_present_queue.hpp`, `lease_target_adapters.hpp`: dedicated coverage channel.
- `ui/editor_rhi/direct_present_queue.cpp`, `lease_target_adapters.cpp`, `editor_viewport_renderer.cpp`: coverage transport and consumption.
- `ui/editor_rhi/shaders/editor_viewport.frag`: the one composite.
- `ui/alcedo_main/app_theme.cpp`, `include/ui/alcedo_main/app_theme.hpp`, `ui/alcedo_main/DESIGN.md`: coverage display roles.
- `tests/app/CMakeLists.txt`, `tests/edit/CMakeLists.txt`, `tests/ui/CMakeLists.txt`: named test registration.

Proposed files:

- `include/edit/runtime/ai_mask_coverage_work.hpp`: narrow backend-neutral operation inputs and validation.
- `edit/runtime/cuda/cuda_ai_mask_coverage.cu`: CUDA edge kernels and dispatch.
- `edit/runtime/opencl/opencl_ai_mask_coverage.cpp`, `edit/runtime/opencl/shader/ai_mask_coverage.cl`: OpenCL implementation.
- `edit/runtime/metal/metal_ai_mask_coverage.mm`: Metal dispatch; add kernels to the existing Metal shader compilation unit.
- `tests/edit/runtime/ai_mask_gpu_coverage_test_support.hpp`: independent equation and readback assertions.
- `tests/app/editor_mask_coverage_preview_test.cpp`: app request and lifecycle assertions.
- `tests/ui/mask_coverage_presentation_test.cpp`: real native-resource transfer and composite output assertions.

Keep OpenCL program names in the owning manifest. Register through `OpenClBackendProgramRegistry`.
Use existing CUDA and Metal targets and shader packaging. Do not add an inference runtime dependency to UI.
Use defining includes. Do not prescribe new forward declarations for convenience.

## 7. Implementation assignments

Each row is a separately reviewable implementation phase.
The estimate includes production code, tests, build changes, and documentation.
The backend split keeps each kernel port and its evidence below 2000 changed lines.
The transport/composite split separates resource lifetime evidence from pixel composition evidence.

| Phase | Result | Dependency | Expected diff | Status |
| --- | --- | --- | ---: | --- |
| SM8.1 | Source lifetime and workspace operation boundary | SM5, SM6 | 900-1500 | complete 2026-10-24 (7.1.1) |
| SM8.2 | CUDA edge computation and committed-render reuse | SM8.1 | 1200-1900 | complete 2026-10-24 (7.2.1) |
| SM8.3 | OpenCL edge computation and committed-render reuse | SM8.2 equations and fixtures | 1000-1800 | complete 2026-10-24 (7.3.1) |
| SM8.4 | Metal edge computation and committed-render reuse | SM8.2 equations and fixtures | 1000-1800 | complete 2026-10-24 (7.4.1) |
| SM8.5 | Dedicated GPU coverage presentation channel | SM8.1 | 1200-1900 | complete 2026-10-24 (7.5.1) |
| SM8.6 | One viewport composite for candidate and committed coverage | SM8.2-SM8.5 | 1000-1700 | complete 2026-10-24 (7.6.1) |
| SM8.7 | Session preview, replacement, Undo/Redo, Commit, and Cancel | SM8.6 | 1100-1800 | planned |
| SM8.8 | Cross-backend resource and latency evidence | SM8.7 | 500-1000 | planned |

### 7.1 SM8.1 - Source lifetime and workspace operations

**Objective and prerequisites.** Establish the shared read and GPU owner operations from sections 3 and 4.
Read SM5 M7, M15, and M16 before changing any owner API.

**Modules and invariants.** Change `AiMaskEditSession`, the coverage cache, and `BasicRenderWorkspace`.
Keep the asset format, serialized recipe, stored source, and main history unchanged.

**Steps.**

1. Add the shared immutable read of the current composed coverage without a full-frame copy.
2. Define raw-source acquisition separately from edge-result acquisition.
3. Add candidate-result replacement that retains a predecessor only for actual readers.
4. Add coverage-only begin/end work on the existing device submission path without photo invalidation.
5. Add explicit errors for bad extents, invalid edge parameters, and allocation failure.

**Success chain.** Session owner -> shared current pixels -> render executor -> workspace source acquisition.
**Failure chain.** Validation/allocation error -> no published replacement -> prior source and photo values remain valid -> caller error.

**Tests.** `QueuedCoverageReadSurvivesNextModelResult` checks immutable old bytes until the read is released.
`EdgeChangeSharesOriginalPixels` checks the allocation identity and unchanged stack model-call count.
`CoverageOnlyWorkKeepsPhotoResults` checks existing photo result identities before and after coverage work.
Use `AiMaskEditSessionTest` and proposed `EditorMaskCoveragePreviewTest` plus the affected workspace tests.

**Commands.** Use command groups A and B in section 9. Filter to these named behaviors.
**Exit.** All three behaviors pass; the shared read and release purpose are documented.
**Expected diff.** 900-1500 lines. **Completion record.** Section 7.1.1.

#### 7.1.1 SM8.1 completion record

```text
Phase / date / status: SM8.1 / 2026-10-24 / complete on Windows (CUDA, OpenCL) and macOS (Metal).
Source revision and branch: base b3e1d479 on main; branch
  feature/ai-segmentation-sm8-1-coverage-source-lifetime (first pull request of the SM8 stack).
Actual changed modules and diff size: 21 files, +1450 / -138 (1588 changed lines; estimate
  900-1500). AiMaskEditSession, the new ai_mask_coverage_work, the host coverage function,
  MaskCoverageTextureCache, BasicRenderWorkspace, BasicRenderDevice, CudaRenderDevice, the three
  MaskEvaluate callers, and tests. Largest changed files: mask_coverage_texture_cache.hpp 440
  lines, basic_render_workspace.hpp 491 lines, mask_coverage_texture_cache_test.cpp 449 lines.
Implemented behavior:
  1. AiMaskEditSession stores the composed coverage in shared immutable storage.
     AcquireCoveragePixels() returns a read of the same allocation (no copy). Coverage() stays a
     scoped view. A recipe change composes a new allocation; edge and input-mode steps keep it.
     Undo and Redo compose only when the recipe changes, so they keep the allocation across edge
     steps. The opened stored coverage is shared, not copied.
  2. AiMaskCoverageWork (include/edit/runtime/ai_mask_coverage_work.hpp) validates the analysis
     size, the source rectangle, and the edge values, and gives the source and output texel
     rectangles. The host F8 function uses the same rectangles.
  3. MaskCoverageTextureCache separates the original-coverage upload from the edge result.
     AcquireStored keeps committed coverage by content key under the 256 MiB policy.
     AcquireCandidate keeps one current original upload and one current edge result per
     candidate allocation; identity is the allocation (weak owner identity), not a counter.
     A replaced texture stays in the retired list only while a read (MaskCoverageRead) or an
     in-flight submission uses it. The original upload holds the session read until its
     submission completes. With no edge adjustment, the read is the source texture.
  4. BasicRenderWorkspace: AcquireAiMaskCoverage (render or coverage work),
     AcquireAiMaskCandidateCoverage (coverage work only), BeginCoverageWork, EndCoverageWork,
     CancelCoverageWork. Coverage work waits for the previous submission and takes a new
     submission id; it does not touch graph images, parameters, transients, scene work, or the
     texture pool. BasicRenderDevice and CudaRenderDevice forward the three calls.
  5. Errors: "AI Mask coverage:" messages for a null candidate, a wrong pixel count, an analysis
     size outside [1, 4096], a source outside the analysis image, edge values out of range, a
     failed texture allocation, and a failed upload. No current entry changes on failure.
Primary success call chain:
  AiMaskEditSession::AcquireCoveragePixels (session worker)
    -> shared const read crosses to the render executor
    -> BasicRenderDevice/CudaRenderDevice::BeginCoverageWork
    -> BasicRenderWorkspace::AcquireAiMaskCandidateCoverage
    -> MaskCoverageTextureCache::AcquireCandidate -> MakeAiMaskCoverageWork
    -> original upload (no edge) or host edge result + upload
    -> MaskCoverageRead (scoped processed texture read)
    -> EndCoverageWork (Submit) -> WaitIdle -> ReleaseIdle drops the held session read
  Committed render: ExecuteCudaMask / EncodeRaster (OpenCL, Metal)
    -> BasicRenderWorkspace::AcquireAiMaskCoverage -> AcquireStored -> raster sampling.
Primary failure and restore call chain:
  invalid input / allocation / upload error in AcquireCandidate or AcquireStored
    -> exception before Install -> current candidate entries, earlier reads, and photo results
       unchanged -> caller cancels (CancelCoverageWork / CancelRender) -> error to caller.
  cancelled render or coverage work -> DiscardUploadsOf(open submission) -> entries whose
    upload was recorded in it are retired -> next acquisition uploads again (#503).
Commands and exit codes:
  cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH=... (0)
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
    MaskCoverageTextureCacheTest AiMaskCoverageTextureTest AiMaskEditSessionTest
    GpuDagCudaMaskTest GpuDagOpenClGradeTest GpuDagCudaWorkspaceTest alcedo_main (0)
  ctest --test-dir build/debug -j 1 -R
    '^(MaskCoverageTextureCacheTest|AiMaskCoverageTextureTest|AiMaskEditSessionTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest)\.'
  ctest --test-dir build/debug -j 1 -R '^GpuDagOpenClGradeTest\.OpenClMaskFixture\.'
  macOS: cmake --build build/macos-debug --target GpuDagMetalGradeTest (0);
    ctest --test-dir build/macos-debug -j 1 -R '^GpuDagMetalGradeTest\.MetalMaskFixture\.'
Discovered / passed / failed / skipped counts:
  Windows host and CUDA: 90 discovered, 90 passed (GpuDagCudaMaskTest after the
    control-render change below)
    (MaskCoverageTextureCacheTest 8/8, AiMaskEditSessionTest 19/19, AiMaskCoverageTextureTest
    8/8, GpuDagCudaMaskTest 18/18, GpuDagCudaWorkspaceTest 37/37).
  Windows OpenCL: OpenClMaskFixture 17/17 passed.
  macOS Metal: MetalMaskFixture 20 discovered, 18 passed, 2 failed. The failures are
    MetalDisconnectedMaskUsesConstantOneCoverage and
    MetalNormalMixMatchesCudaReferenceWithinTolerance: the null result lease and the
    192-versus-0 value of open issue #497, unchanged by SM8.1.
  Named SM8.1 behaviors: QueuedCoverageReadSurvivesNextModelResult and
    EdgeChangeSharesOriginalPixels (AiMaskEditSessionTest), CoverageOnlyWorkKeepsPhotoResults
    (GpuDagCudaMaskTest, GpuDagOpenClGradeTest, GpuDagMetalGradeTest): all pass.
Manual verification: none; SM8.1 has no user-visible change.
Numeric differences, latency, and resource measurements: none in this assignment. The candidate
  read of CoverageOnlyWorkKeepsPhotoResults equals the session pixels byte for byte, and its edge
  result equals the host function byte for byte, on all three backends.
Remaining issues or unavailable platforms: #497 (Metal, existing). #503 found during this work
  and fixed here.
```

Decisions made during SM8.1 (AGENTS.md "Make implementation decisions"):

- The edge result stays the host F8 function plus an upload in SM8.1. It is the SM6 computation,
  selected per backend at compile time, not a runtime substitute after a GPU failure. SM8.2,
  SM8.3, and SM8.4 replace it on CUDA, OpenCL, and Metal; the last of them removes the host step
  from the runtime. Until then a nonzero edge does not upload the original coverage, because the
  host step reads the CPU pixels directly.
- `EditorMaskCoveragePreviewTest` is not created in SM8.1. SM8.1 adds no app facade operation; the
  facade (`UpdateAiMaskCandidateCoverage`, `SelectMaskCoveragePreview`) belongs to SM8.5 and
  SM8.7. The three named behaviors are in `AiMaskEditSessionTest` and in the shared GPU mask
  harness (`ai_mask_raster_test_support.hpp`), plus the new host `MaskCoverageTextureCacheTest`.
- `CoverageOnlyWorkKeepsPhotoResults` compares the next render with a control render of the same
  document. The Color Grade result lives in the scene-work pair and re-executes on every render,
  so the check compares pass counters with the control instead of asserting zero executions.
- Only coverage work can acquire the candidate, because the candidate is presentation input (C4,
  C10). `BasicRenderWorkspace::MaskCoverage()` is now read-only; acquisition goes through the
  workspace methods that check for an open submission.
- A cancelled render or cancelled coverage work retires the coverage uploads of its submission.
  On Metal, a cancel discards the recorded command buffer, so the cache entry had no content
  (#503). The previous candidate is not restored after a cancel; earlier reads stay valid and the
  next acquisition uploads again.
- OpenCL coverage work records no kernel in SM8.1, so it does not bind the thread queue that
  `BasicRenderDevice::Execute` binds. SM8.3 must bind it when it adds the OpenCL edge kernels.

### 7.2 SM8.2 - CUDA edge computation

**Objective and prerequisites.** Implement section 5.1 on CUDA after SM8.1.
**Modules and invariants.** Use `EditRuntimeCuda`, its existing device, and the workspace coverage owner.
Preserve margin, replicated borders, and exact integer morphology.

**Steps.**

1. Implement alternating morphology and separable finite Gaussian kernels.
2. Keep float intermediates and quantize only the final Gaussian result.
3. Make CUDA `MaskEvaluate` acquire this same processed result.
4. Reuse source uploads across edge changes and preserve old results until readers release them.
5. Report CUDA errors without invoking host edge processing.

**Success chain.** Raw R8 -> workspace upload -> CUDA edge work -> processed R8 -> raster sampling.
**Failure chain.** Kernel/allocation error -> discard unpublished output -> keep previous display -> report error.

**Tests.** Add `GpuExpandMatchesAlternatingMorphologyExactly`, `GpuFeatherMatchesIndependentGaussianWithinOneCode`,
`GpuCoverageIncludesFullMarginAtImageBorders`, `EdgeChangeDoesNotUploadOriginalAgain`, and
`FailedEdgeWorkKeepsPublishedCoverage` to `GpuDagCudaMaskTest` with shared independent fixtures.
Test sigma 0, 0.5, 5, and 50; Expand -50, -2, 0, 2, and 50; odd sizes and image-edge masks.

**Commands.** Groups A and B, target `GpuDagCudaMaskTest`, `ctest -j 1` with named behavior filters.
**Exit.** Pixel assertions pass and the production CUDA caller uses GPU edge processing.
**Expected diff.** 1200-1900 lines. **Completion record.** Section 7.2.1.

#### 7.2.1 SM8.2 completion record

```text
Phase / date / status: SM8.2 / 2026-10-24 / complete on Windows (CUDA).
Source revision and branch: base f3ac3db5 (SM8.1, pull request #504); branch
  feature/ai-segmentation-sm8-2-cuda-edge-coverage (second pull request of the SM8 stack).
Actual changed modules and diff size: 17 files, +1455 / -142 (1597 changed lines; estimate
  1200-1900). Host Feather coefficients, AiMaskEdgeDispatch (ai_mask_coverage_work),
  MaskCoverageTextureCache, BasicRenderWorkspace (discard of cancelled writes), CudaBackend, the
  new cuda_ai_mask_coverage.cu, and tests. Largest changed files: mask_coverage_texture_cache.hpp
  645 lines, mask_coverage_texture_cache_test.cpp 557 lines, ai_mask_gpu_coverage_test_support.hpp
  364 lines.
Implemented behavior:
  1. AiMaskEdgeDispatch gives the device passes of one AiMaskCoverageWork: the source placed in
     the output rectangle (zero outside it, replicated output border), |expand_px| alternating
     cross/square 3 x 3 steps, then the two Feather passes with the half kernel of
     AiMaskFeatherWeights. AiMaskFeatherWeights reads cv::getGaussianKernel(round(8 sigma + 1) | 1,
     sigma, CV_32F), the kernel of the host function, so host and device use the same finite
     coefficients and support (radius at most 200).
  2. CudaBackend::RecordAiMaskEdge records MorphologyStepKernel steps (R8, exact), then
     FeatherRowsKernel (R8 -> float rows, `u * (1 / 255)`) and FeatherColumnsKernel (float ->
     R8, round half to even, clamp). The horizontal intermediate stays float. It validates every
     texture size and format and reports launch errors; there is no host edge step on CUDA.
  3. MaskCoverageTextureCache selects the device edge work at compile time
     (RecordsAiMaskEdgeOnDevice). Stored coverage: one original upload per asset key and
     analysis size, and one edge result per exact edge value, both under the 256 MiB policy; an
     edge change reads no asset and uploads nothing. Candidate: the original upload of the
     current allocation stays current during edge changes; one current edge result. Work
     textures (R8 morphology x 0-2, R32f Feather rows x 0-1, output size) belong to the cache,
     are reused for the next edge work of the same size, and are destroyed when idle and of
     another size, and at Clear.
  4. CUDA MaskEvaluate already acquires through BasicRenderWorkspace::AcquireAiMaskCoverage, so
     committed renders, thumbnails, and export use the same device edge result.
  5. A failed output or work-texture allocation or a failed recording throws an
     "AI Mask coverage:" error and installs nothing; the cancelled submission's writes (uploads
     and edge results) are retired (DiscardWritesOf, renamed from DiscardUploadsOf).
Primary success call chain:
  Committed: ExecuteCudaMask -> BasicRenderWorkspace::AcquireAiMaskCoverage
    -> MaskCoverageTextureCache::AcquireStored -> original upload hit (or reader + upload)
    -> RecordEdge -> MakeAiMaskEdgeDispatch -> AcquireWorkTextures
    -> CudaBackend::RecordAiMaskEdge (morphology, Feather rows, Feather columns on the stream)
    -> processed R8 entry -> RasterMaskKernel samples it.
  Candidate: BeginCoverageWork -> AcquireAiMaskCandidateCoverage -> AcquireCandidate
    -> original upload of the session allocation (once) -> RecordEdge -> EndCoverageWork.
Primary failure and restore call chain:
  allocation / validation / launch error in RecordEdge -> exception before Install -> the
    unpublished output is destroyed, the current edge result and its reads are unchanged
    -> caller cancels (CancelRender / CancelCoverageWork -> DiscardWritesOf) -> error to caller
    -> the next acquisition of the published edge value is a hit; a retry records new work
       from the resident original upload.
Commands and exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
    MaskCoverageTextureCacheTest GpuDagCudaMaskTest AiMaskCoverageTextureTest
    GpuDagCudaWorkspaceTest GpuDagOpenClGradeTest AiMaskEditSessionTest alcedo_main (0)
  ctest --test-dir build/debug -j 1 --output-on-failure -R
    '^(MaskCoverageTextureCacheTest|AiMaskCoverageTextureTest|AiMaskEditSessionTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest)\.|^GpuDagOpenClGradeTest\.OpenClMaskFixture\.'
    (0)
Discovered / passed / failed / skipped counts: 114 discovered, 114 passed, 0 failed, 0 skipped
  (MaskCoverageTextureCacheTest 10/10, AiMaskCoverageTextureTest 8/8, AiMaskEditSessionTest
  19/19, GpuDagCudaMaskTest 23/23, GpuDagCudaWorkspaceTest 37/37, OpenClMaskFixture 17/17 on the
  unchanged host edge path).
  Named SM8.2 behaviors in GpuDagCudaMaskTest: GpuExpandMatchesAlternatingMorphologyExactly
  (Expand -50, -2, -1, 0, 1, 2, 3, 50; 67 x 45; candidate and stored),
  GpuFeatherMatchesIndependentGaussianWithinOneCode (sigma 0.5, 5, 50 with Expand 0, -2, 2),
  GpuCoverageIncludesFullMarginAtImageBorders (corner, side, and interior assets),
  EdgeChangeDoesNotUploadOriginalAgain, FailedEdgeWorkKeepsPublishedCoverage: all pass.
  Sigma 0 is the Expand-only case of the first test.
  Mutation check: with the cross/square order swapped in the CUDA kernel,
  GpuExpandMatchesAlternatingMorphologyExactly fails (restored before the final run).
Manual verification: none; SM8.2 has no user-visible change before SM8.6.
Numeric differences, latency, and resource measurements:
  Largest difference of the CUDA result to the double-precision reference: 0 code; to the host
    function MakeAiMaskCoverageTexture: 0 code (all cases of the Feather test). The plan
    tolerance stays one code.
  Kernel time, RTX 3080 Laptop GPU, driver 610.62, kernels of cuda_ai_mask_coverage.cu built
    by nvcc -O3 -arch=sm_86 in a temporary harness, 4096 x 3072, 4 samples each: Feather sigma
    50 rows 11.9-13.5 ms + columns 5.8-6.4 ms; sigma 20 4.8 + 2.3-3.3 ms; sigma 5 1.3 + 0.6 ms;
    Expand 50 (50 steps) 12.3-12.5 ms. This is kernel time only, not request-to-present, and
    not the SM8.8 qualification (no p95, no production Release build). The debug build compiles
    CUDA with -G, so its timings are not representative.
  Work textures at 4096 x 3072 with Expand >= 2 and Feather: 2 x 12 MiB R8 + 48 MiB R32f.
Remaining issues or unavailable platforms: OpenCL and Metal still use the host edge function
  until SM8.3 and SM8.4.
```

Decisions made during SM8.2 (AGENTS.md "Make implementation decisions"):

- The morphology and Feather work textures belong to `MaskCoverageTextureCache`, not to the
  workspace `TexturePool`. The pool's frame state (`BeginFrame`, `MarkSubmitted` of every leased
  entry, `ReleaseUnused` at the end of a photo render) belongs to photo renders, and SM8.1 requires
  that coverage-only work leave the pool unchanged (`CoverageOnlyWorkKeepsPhotoResults`). The cache
  keeps one set per output size, reuses it on the ordered device queue, and reports
  `WorkTextureBytes()` outside the 256 MiB coverage budget.
- The device operation is a backend member, `RecordAiMaskEdge(dispatch, textures, context)`, and
  the cache selects it at compile time with the `RecordsAiMaskEdgeOnDevice` concept. A backend
  without it keeps the SM8.1 host function. This is per-backend selection, not a runtime
  substitute; SM8.4 removes the host path when Metal has the operation.
- `AiMaskFeatherWeights` gives the half kernel; the passes sum symmetric pairs
  (`w_k * (left + right)`). The host function keeps `cv::sepFilter2D`.
- `FailedEdgeWorkKeepsPublishedCoverage` fails a real texture allocation through the new test
  hook `CudaBackend::FailTextureAllocationAfter`, the pattern of `FailUploadAfter`: first the
  output, then the first morphology work texture after the output succeeded.
- The SM6 check `AiMaskCoverageTextureUploadsOncePerAssetAndEdge` is renamed
  `AiMaskCoverageTextureUploadsOncePerAsset`: on a backend with device edge work, an edge change
  reads no asset and uploads nothing. `CheckCoverageOnlyWorkKeepsPhotoResults` compares the edge
  result with the host function within one code instead of byte equality (section 5.1).

### 7.3 SM8.3 - OpenCL edge computation

**Objective and prerequisites.** Port the exact SM8.2 equations and fixtures to OpenCL.
**Modules and invariants.** Use `EditRuntimeOpenCl`, the workspace queue, and the central program registry.
Register the program once. Do not register or compile a new program for each slider update.

**Steps.**

1. Add the kernels and module manifest constants.
2. Dispatch through the workspace queue with tracked completion events.
3. Change OpenCL `MaskEvaluate` to the shared GPU coverage acquisition.
4. Add upload, allocation, dispatch, and release counters for the named assertions.
5. Report OpenCL errors and keep failed results unpublished.

**Success chain.** Current source -> OpenCL edge kernels -> workspace processed coverage -> existing raster pass.
**Failure chain.** Program/build/dispatch error -> prior published coverage remains -> explicit caller error.

**Tests.** Run the five SM8.2 behaviors in `GpuDagOpenClGradeTest`.
Add `OpenClCoverageProgramIsNotRegisteredPerUpdate` and verify stable registration counts over repeated updates.
Keep known unrelated failures #496 separate from these results.

**Commands.** Groups A and B, target `GpuDagOpenClGradeTest`, named GPU tests with `-j 1`.
**Exit.** Pixel and lifecycle evidence passes; no host processing or backend substitution occurs.
**Expected diff.** 1000-1800 lines. **Completion record.** Section 7.3.1.

#### 7.3.1 SM8.3 completion record

```text
Phase / date / status: SM8.3 / 2026-10-24 / complete on Windows (OpenCL, NVIDIA driver).
Source revision and branch: base e99b36de (SM8.2, pull request #505); branch
  feature/ai-segmentation-sm8-3-opencl-edge-coverage (third pull request of the SM8 stack).
Actual changed modules and diff size: 9 files, +379 / -6 (385 changed lines; estimate
  1000-1800). The estimate assumed new fixtures and owner changes; SM8.2 already added the
  backend-neutral dispatch, the coverage owner operation, and the shared checks, so SM8.3 adds
  only the OpenCL program, its dispatch, the manifest entry, and the test wiring. Largest files:
  opencl_ai_mask_coverage.cpp 158 lines, ai_mask_coverage.cl 101 lines.
Implemented behavior:
  1. shader/ai_mask_coverage.cl: ai_mask_morphology_r8, ai_mask_feather_rows_r32f, and
     ai_mask_feather_columns_r8, the passes of AiMaskEdgeDispatch. R8 images are
     CL_UNORM_INT8, so each read converts back to the integer value (convert_uint_sat_rte of
     v * 255) and each R8 write stores an integer / 255; morphology stays exact.
  2. The program opencl_dag_ai_mask_coverage is one descriptor of the gpu_dag manifest
     (RegisterOpenClGpuDagPrograms, once per process), with its path constant in
     src/opencl/CMakeLists.txt and its names in opencl_dag_programs.hpp. The shader directory is
     installed whole, so packaging needs no new rule.
  3. OpenClBackend::RecordAiMaskEdge gets the kernels from the per-thread OpenClKernelCache,
     enqueues on the device queue, and tracks each event in the command context, like the raster
     Mask pass. The half kernel is passed by value (808 bytes; with the grid and two images the
     arguments stay below the 1024-byte minimum CL_DEVICE_MAX_PARAMETER_SIZE).
  4. MaskCoverageTextureCache<OpenClBackend> now records the edge work on the device; OpenCL
     MaskEvaluate (EncodeRaster) acquires through the same owner operation.
  5. Errors: missing or wrong textures, kernel lookup, argument, and enqueue failures throw;
     the owner installs nothing and the caller cancels. OpenClBackend gains the test hook
     FailTextureAllocationAfter.
Primary success call chain:
  EncodeRaster / AcquireAiMaskCandidateCoverage -> MaskCoverageTextureCache (resident original
    upload) -> RecordEdge -> OpenClBackend::RecordAiMaskEdge -> OpenClKernelCache::GetKernel
    (program built once) -> clEnqueueNDRangeKernel x (steps + 2) on the device queue
    -> TrackKernelEvent -> Submit marker -> Wait -> processed R8 sampled by mask_raster_r8.
Primary failure and restore call chain:
  program build / argument / enqueue / allocation error -> exception before Install
    -> CancelRender or CancelCoverageWork -> DiscardWritesOf -> the next Begin waits on the
    tracked events of the cancelled work (OpenClBackend::Wait drains live events) -> the
    published edge result stays current -> error to caller.
Commands and exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
    MaskCoverageTextureCacheTest GpuDagCudaMaskTest GpuDagOpenClGradeTest alcedo_main (0)
  ctest --test-dir build/debug -j 1 --output-on-failure -R
    '^(MaskCoverageTextureCacheTest|GpuDagCudaMaskTest)\.|^GpuDagOpenClGradeTest\.OpenClMaskFixture\.' (0)
  ctest --test-dir build/debug -j 1 -R '^GpuDagOpenClGradeTest\.' (8: the six #496 tests)
Discovered / passed / failed / skipped counts:
  Targeted: 56 discovered, 56 passed (MaskCoverageTextureCacheTest 10/10, GpuDagCudaMaskTest
    23/23, OpenClMaskFixture 23/23).
  Whole GpuDagOpenClGradeTest: 86 discovered, 80 passed, 6 failed. The failures are exactly
    the six OpenClMultiGradeFixture tests of open issue #496 (ThreeGradesComposeInEdgeOrder,
    SameAdjustmentTypeUsesDistinctNodeSlots, EachGradeMixesAgainstItsOwnInput,
    DisabledGradeAliasesInputUntilFinalReader, ZeroMixGradeAliasesInputUntilFinalReader,
    TwoLutGradesKeepIndependentCubeState); no Mask test fails.
  Named SM8.3 behaviors in OpenClMaskFixture: GpuExpandMatchesAlternatingMorphologyExactly,
    GpuFeatherMatchesIndependentGaussianWithinOneCode, GpuCoverageIncludesFullMarginAtImageBorders,
    EdgeChangeDoesNotUploadOriginalAgain, FailedEdgeWorkKeepsPublishedCoverage, and
    OpenClCoverageProgramIsNotRegisteredPerUpdate (12 edge updates: program build count, kernel
    create count, and registered program names unchanged; the program appears once): all pass.
Manual verification: none; SM8.3 has no user-visible change before SM8.6.
Numeric differences, latency, and resource measurements:
  Largest difference of the OpenCL result to the double-precision reference: 0 code; to the
    host function: 0 code.
  Coverage-only work wall time, NVIDIA GeForce RTX 3080 Laptop GPU OpenCL (driver 610.62),
    4096 x 3072 candidate with a resident original upload, one sample each, including the result
    allocation and a 12 MiB readback in a temporary test (debug host build; the driver compiles
    the kernels with optimization): Feather sigma 50 48-69 ms, sigma 5 16 ms, Expand 50 38 ms,
    Feather 50 with Expand 50 96 ms. Not the SM8.8 qualification.
Remaining issues or unavailable platforms: no other OpenCL device (AMD or Intel) was available.
  #496 (existing, unrelated).
```

Decisions made during SM8.3 (AGENTS.md "Make implementation decisions"):

- Coverage work does not bind the thread queue (`BindThreadQueue`). The SM8.1 note required it
  when coverage work records kernels. `RecordAiMaskEdge` and the R8 upload enqueue directly on
  the backend's own queue (`queue_`), like `ExecuteOpenClMask`, and resolve no queue through
  `OpenClContext`, so the binding has no effect on this work. Kernels come from the per-thread
  cache, so argument binding stays on the calling thread.
- The Feather half kernel is a by-value kernel argument, not a buffer upload, so an edge update
  allocates and uploads nothing besides its output texture.
- `OpenClCoverageProgramIsNotRegisteredPerUpdate` reads `OpenClBackend::ProgramBuildCount`,
  `KernelCreateCount`, and `OpenClProgramLibrary::RegisteredProgramNames`; the plan's
  "registration counts" are these existing counters, so no new counter was added (plan step 4).

### 7.4 SM8.4 - Metal edge computation

**Objective and prerequisites.** Port the exact SM8.2 equations and fixtures to Metal.
**Modules and invariants.** Use `EditRuntimeMetal`, existing command buffers, and Metal shader packaging.
Keep CPU upload storage alive until its blit completes.

**Steps.**

1. Add morphology and Gaussian kernels to the existing shader build.
2. Order source blit before compute and compute before display sampling.
3. Change Metal `MaskEvaluate` to the same processed coverage acquisition.
4. Reuse work textures only after command completion.
5. Surface Metal allocation and command errors through the current caller.

**Success chain.** Source blit -> morphology -> horizontal/vertical Gaussian -> processed R8 -> raster sampling.
**Failure chain.** Allocation/command error -> no display publication -> prior target retained -> error.

**Tests.** Run the five SM8.2 behaviors in `GpuDagMetalGradeTest`.
Add `MetalCoverageUploadOwnerSurvivesUntilBlitCompletes` with a delayed real completion.
Keep the existing #496 and #497 failures separate.

**Commands.** Group C and targeted Metal tests with `-j 1`.
**Exit.** Native macOS results exist for pixels, upload lifetime, and repeated resource release.
**Expected diff.** 1000-1800 lines. **Completion record.** Section 7.4.1.

#### 7.4.1 SM8.4 completion record

```text
Phase / date / status: SM8.4 / 2026-10-24 / complete on macOS (Metal, Apple M4); the shared
  owner change is also verified on Windows (CUDA, OpenCL).
Source revision and branch: base 8d106742 (SM8.3, pull request #506); branch
  feature/ai-segmentation-sm8-4-metal-edge-coverage (fourth pull request of the SM8 stack).
Actual changed modules and diff size: 13 files, +456 / -91 (547 changed lines; estimate
  1000-1800, smaller for the reason given in 7.3.1). Metal kernels in mask.metal,
  metal_ai_mask_coverage.mm, MetalBackend, the Mask warm-up list, MaskCoverageTextureCache (host
  edge path removed), and tests. Largest files: metal_ai_mask_coverage.mm 163 lines, the mask.metal
  addition 104 lines.
Implemented behavior:
  1. mask.metal gains ai_mask_morphology, ai_mask_feather_rows, and ai_mask_feather_columns (the
     passes of AiMaskEdgeDispatch, in the existing Mask shader library). R8Unorm reads convert
     back to the integer value; R8 writes store an integer / 255; morphology stays exact.
  2. MetalBackend::RecordAiMaskEdge encodes the passes in the compute encoder of the current
     command buffer with pipelines from ComputePipelineCache, setBytes for the grid and the
     808-byte half kernel, and NoteComputeDispatch (a texture and buffer memory barrier) after
     each dispatch. A source upload is a blit encoder that ends before the compute encoder starts,
     so the order is source blit -> morphology -> Feather rows -> Feather columns -> raster
     sampling. The three pipelines join AppendMetalMaskWarmup.
  3. All three render backends now record the edge work on the device. MaskCoverageTextureCache
     requires RecordsAiMaskEdgeOnDevice (static_assert) and has no host edge path; the host
     function MakeAiMaskCoverageTexture is a test reference only. The kRecordsEdgeOnDevice
     constant and the harness flag records_edge_on_device are removed.
  4. Errors: missing or wrong textures, a missing pipeline or encoder, and a failed allocation
     throw; the owner installs nothing. MetalBackend gains the test hook
     FailTextureAllocationAfter.
Primary success call chain:
  EncodeRaster / AcquireAiMaskCandidateCoverage -> MaskCoverageTextureCache (resident original
    upload; UploadTexture2D copies the bytes into command-buffer staging and encodes a blit)
    -> RecordEdge -> MetalBackend::RecordAiMaskEdge -> EnsureComputeCommandEncoder
    -> dispatches with barriers -> Submit (commit) -> Wait -> processed R8 sampled by mask_raster.
Primary failure and restore call chain:
  pipeline / encoder / allocation error -> exception before Install -> CancelRender or
    CancelCoverageWork -> DiscardWritesOf retires the uploads and edge results of the discarded
    command buffer (#503 rule) -> the published edge result stays current -> error to caller.
Commands and exit codes:
  macOS (yurunmac.local, build/macos-debug with ALCEDO_BUILD_TESTS=ON):
    cmake --build build/macos-debug --target GpuDagMetalGradeTest (0)
    ctest --test-dir build/macos-debug -j 1 --output-on-failure -R
      '^GpuDagMetalGradeTest\.MetalMaskFixture\.' (8: the two #497 tests)
  Windows: cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
    MaskCoverageTextureCacheTest GpuDagCudaMaskTest GpuDagOpenClGradeTest
    AiMaskCoverageTextureTest GpuDagCudaWorkspaceTest AiMaskEditSessionTest alcedo_main (0);
    ctest --test-dir build/debug -j 1 --output-on-failure -R
      '^(MaskCoverageTextureCacheTest|AiMaskCoverageTextureTest|AiMaskEditSessionTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest)\.|^GpuDagOpenClGradeTest\.OpenClMaskFixture\.' (0)
Discovered / passed / failed / skipped counts:
  macOS MetalMaskFixture: 26 discovered, 24 passed, 2 failed. The failures are
    MetalDisconnectedMaskUsesConstantOneCoverage and
    MetalNormalMixMatchesCudaReferenceWithinTolerance of open issue #497 (unchanged since
    SM8.1). Named SM8.4 behaviors: GpuExpandMatchesAlternatingMorphologyExactly,
    GpuFeatherMatchesIndependentGaussianWithinOneCode, GpuCoverageIncludesFullMarginAtImageBorders,
    EdgeChangeDoesNotUploadOriginalAgain, FailedEdgeWorkKeepsPublishedCoverage, and
    MetalCoverageUploadOwnerSurvivesUntilBlitCompletes: all pass.
    MetalMaskWarmupCachesFillZeroAndUnionMax passes with the three added pipelines.
  Windows: 120 discovered, 120 passed (the 114 of 7.2.1 plus the six OpenCL tests of 7.3.1).
Manual verification: none; SM8.4 has no user-visible change before SM8.6.
Numeric differences, latency, and resource measurements:
  Largest difference of the Metal result to the double-precision reference: 0 code; to the host
    function: 0 code.
  Coverage-only work wall time, Apple M4 (macOS 27.0.1, Metal 4), 4096 x 3072 candidate with a
    resident original upload, one sample each, including the result allocation and a 12 MiB
    readback, in a temporary test of the debug build (the Metal library is compiled the same way
    in every build type): Feather sigma 50 145-175 ms, sigma 5 16 ms, Expand 50 80 ms, Feather 50
    with Expand 50 236 ms. These are not the SM8.8 qualification, but they show that the SM8.8
    target (resident edge work below 100 ms at p95) is not met on this device by the direct
    401-tap passes; see "Remaining issues".
  MetalCoverageUploadOwnerSurvivesUntilBlitCompletes: in each of two cycles (two uploads and one
    edge result in one command buffer, then a session release) every created texture is destroyed;
    cache entries, work textures, and bytes return to 0.
Remaining issues or unavailable platforms: #497 (existing). The Metal Feather time at sigma 50 is
  recorded on #494 for SM8.8; the planned response is the exact finite convolution with
  threadgroup-memory row and column tiles (each texel read once per tile instead of once per
  tap), with no change of kernel, support, or resolution (section 10, first risk).
```

Decisions made during SM8.4 (AGENTS.md "Make implementation decisions"):

- "Keep CPU upload storage alive until its blit completes": on Metal the upload storage is the
  command-buffer staging slice that `UploadTexture2D` fills when the upload is recorded; the
  backend recycles it only after `Wait`, `SynchronizeRecordedWork`, or a download completes. The
  session allocation is not read after the upload is recorded. The named test therefore records
  two uploads and an edge result in one command buffer, drops the session allocations before the
  commit, and checks each read's bytes after completion.
- "Reuse work textures only after command completion": across submissions, the coverage owner
  reuses a work texture only when it is idle (every `Begin*` waits for the previous submission).
  Within one command buffer, a second edge work reuses the same work textures after the barrier of
  `NoteComputeDispatch`, which orders the reads of the first work before the writes of the second.
- The resource-release check counts textures (`TextureCreateCount` and `FreeCount`), because the
  Metal backend counts a staging-page creation as an allocation but not its release.
- The three kernels are in `mask.metal` (the existing Mask shader compilation unit), as the plan
  states, so no new metallib or packaging rule is needed.

### 7.5 SM8.5 - Coverage presentation channel

**Objective and prerequisites.** Transfer coverage independently of photo presentation after SM8.1.
**Modules and invariants.** Extend existing presentation queue, adapters, frame metadata, and viewport consumption.
Reuse RGBA32F. Do not alter the meanings of existing photo layers or send coverage to scopes.

**Steps.**

1. Add the coverage channel and its existing-format native target requests.
2. Add a GPU sampling operation that writes effective coverage into the red channel.
3. Use existing geometry and image identity at publication and consumption.
4. Keep the displayed target until a completed replacement arrives.
5. Retire resources safely on hide, image change, device recreation, and close.

**Success chain.** Processed R8 -> GPU display target -> native adapter completion -> viewport coverage input.
**Failure chain.** Target acquisition or transfer error -> no channel replacement -> visible prior target plus caller error.

**Tests.** Proposed `MaskCoveragePresentationTest` checks `CoverageTransferLeavesPhotoSlotsUnchanged`,
`OldCoverageLivesUntilRendererRelease`, `CoverageTransferDoesNotReadBackToHost`,
`CoverageChannelClosesBeforeImageReplacement`, and `CoverageDoesNotReplaceScopePhotoInput`.
Use real native adapters on CUDA, OpenCL, and Metal. Do not infer transfer correctness from fake handles.

**Commands.** Groups A-C and the proposed presentation target; discover its tests before naming them in results.
**Exit.** Real transfers and dual-owner release pass on all backends.
**Expected diff.** 1200-1900 lines. **Completion record.** Section 7.5.1.

#### 7.5.1 SM8.5 completion record

```text
Phase / date / status: SM8.5 / 2026-10-24 / complete on Windows (CUDA, OpenCL) and macOS (Metal).
Source revision and branch: base 36dc153a (SM8.4, pull request #507); branch
  feature/ai-segmentation-sm8-5-coverage-presentation-channel (fifth pull request of the SM8
  stack).
Actual changed modules and diff size: 36 files, +2348 / -51 (2399 changed lines; estimate
  1200-1900). The end-to-end test is 719 lines: it needs its own render thread, a QRhi per
  backend on the device of the editor startup, the real lease adapter, and a test-only readback.
  Production code is about 1630 changed lines. Largest files: mask_coverage_presentation_test.cpp
  719, viewport_mask_coverage_sink.cpp 315, viewport_mask_coverage_layer.cpp 214,
  mask_coverage_display.hpp 161.
Implemented behavior:
  1. Coverage display write: RecordMaskCoverageDisplay (CUDA, OpenCL, Metal) samples the processed
     R8 coverage on the grid of the presented photo frame (MaskCoverageDisplayRequest: the
     ResolveRenderGeometry result, invert, opacity) into an RGBA32F target: red is the R8 value
     / 255, green and blue are 0, alpha is 1. It is the raster sampling of committed MaskEvaluate:
     OpenCL and Metal dispatch the same mask_raster kernel with an RGBA32F output (it already
     writes (value, 0, 0, 1)); CUDA shares RasterMaskCode and MakeRasterMaskParams between
     RasterMaskKernel and RasterMaskDisplayKernel.
  2. MaskCoverageDisplayTargets (workspace owner, not the texture pool): one RGBA32F target per
     size that no presentation reader holds. CUDA and OpenCL keep one target; Metal keeps one more
     per frame that the viewport still displays. A failed allocation leaves the targets unchanged.
     Cleared by ReleaseSessionResources.
  3. PresentMaskCoverage(device, selection, request, identity, sink): coverage-only work
     (BeginCoverageWork, candidate or stored acquisition, display write, EndCoverageWork), then
     FramePresenter<Backend>::PresentMaskCoverage. CUDA: cudaMemcpy2DToArrayAsync into the mapped
     CUDA array and stream completion. OpenCL: clEnqueueCopyImage into the shared image, release
     to OpenGL and clFinish, FinalizePresentation. Metal: wait for the command buffer, then hand
     the MTLTexture and the reader identity of its display target to the viewport. No host
     readback, no photo DAG, no photo result change.
  4. IMaskCoverageSink (ui/edit_viewer/mask_coverage_sink.hpp): the producer side, separate from
     IFrameSink, so a coverage frame cannot reach SubmitFinalDisplayFrame (scope input).
     MaskCoveragePresentation carries the image session (MaskCoverageFrameIdentity) and the photo
     frame geometry. MaskCoveragePublication reports Published or DiscardedForImageChange.
  5. Viewport channel: EditorViewportItem owns a second DirectPresentQueue (coverage queue) and
     ViewportMaskCoverageSink. The coverage queue follows the photo queue for session changes,
     cancel, suspension, renderer invalidation, scene-graph invalidation, and shutdown. Native
     targets come from the same lease adapters with the new LeaseFrameLayer::MaskCoverage.
     ViewportMaskCoverageLayer (owned by EditorViewportRenderer) creates the requested targets,
     imports the newest frame, and releases the previous frame only after the replacement is
     imported (OpenCL: glFinish before the slot returns). It releases the frame on
     clearMaskCoverage, an image session change, and QRhi recreation or teardown. The renderer
     does not bind the coverage texture yet (SM8.6).
  6. The sink drops a frame of another image session at map and at publication; the layer checks
     the identity that the frame carries before import.
  7. Backends count host downloads (DeviceToHostCopyCount) for the transfer assertions.
Primary success call chain:
  render thread: PresentMaskCoverage -> BasicRenderDevice/CudaRenderDevice::BeginCoverageWork
    -> AcquireAiMaskCandidateCoverage | AcquireAiMaskCoverage (resident upload, device edge work)
    -> AcquireMaskCoverageDisplayTarget -> RecordMaskCoverageDisplay (raster sampling, RGBA32F)
    -> EndCoverageWork (Submit) -> FramePresenter<Backend>::PresentMaskCoverage
    -> ViewportMaskCoverageSink::MapCoverageTarget (PrepareWrite, size request, WaitForWritableSlot,
       BeginWrite, ProducerAcquireWritable) -> device copy -> completion
    -> UnmapCoverageTarget (ProducerReleaseWritable, OpenCL clFinish, EndWrite)
    -> PublishCoverage (identity check, NotifyReady) -> requestPresentUpdate
  scene-graph thread: EditorViewportRenderer::render
    -> ViewportMaskCoverageLayer::FulfillTargetRequests (adapter CreateTarget, PublishCreatedSlot)
    -> ConsumeNewest (ConsumeNewestReady, identity check, QRhiTexture::createFrom)
    -> Install: Release of the previous frame (CompleteRendererRead or owner reset)
    -> Texture() is the viewport coverage input.
  Metal: FramePresenter<MetalBackend>::PresentMaskCoverage -> SubmitMetalCoverage
    -> TakePendingMetalCoverage -> import with the frame owner.
Primary failure and restore call chain:
  validation / acquisition / display allocation / encode error -> CancelCoverageWork -> exception
    to the caller; the sink is not touched; the displayed coverage stays.
  no native target (adapter failure -> FailSizeRequest) or a failed copy -> AbandonCoverageTarget
    -> std::runtime_error ("presentation suspended" prefix when minimized) -> displayed coverage
    stays; the next frame recovers.
  image session changed before the map or the publication -> DiscardedForImageChange, slot
    abandoned; the session change already invalidated the queue and released the layer.
  import failure -> the slot returns to the queue; the displayed coverage stays.
Commands and exit codes:
  Windows: cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH=... (0)
    cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
      MaskCoveragePresentationTest MaskCoverageTextureCacheTest GpuDagCudaMaskTest
      GpuDagOpenClGradeTest AiMaskCoverageTextureTest GpuDagCudaWorkspaceTest
      AiMaskEditSessionTest EditorViewportSuspensionTest alcedo_main (0)
    cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
      GpuDagCudaDrtProductTest GpuDagOpenClDrtProductTest HeadlessFrameSinkTest
      EditorSessionRenderSchedulerPortTest EditorRealRawGpuE2eTest (0)
    ctest --test-dir build/debug -j 1 --output-on-failure -R
      '^(MaskCoveragePresentationTest|MaskCoverageTextureCacheTest|AiMaskCoverageTextureTest|AiMaskEditSessionTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest|EditorViewportSuspensionTest)\.|^GpuDagOpenClGradeTest\.OpenClMaskFixture\.' (0)
    ctest --test-dir build/debug -j 1 --output-on-failure -R
      '^(GpuDagCudaDrtProductTest|GpuDagOpenClDrtProductTest|HeadlessFrameSinkTest|EditorSessionRenderSchedulerPortTest|EditorRealRawGpuE2eTest)' (0)
    ctest --test-dir build/debug -j 1 --output-on-failure -R '^MaskCoveragePresentationTest\.'
      (0, after the last change)
  macOS (yurunmac.local, build/macos-debug with ALCEDO_BUILD_TESTS=ON):
    cmake --build build/macos-debug --target MaskCoveragePresentationTest GpuDagMetalGradeTest (0)
    ctest --test-dir build/macos-debug -j 1 --output-on-failure -R '^MaskCoveragePresentationTest\.' (0)
    ctest --test-dir build/macos-debug -j 1 --output-on-failure -R
      '^GpuDagMetalGradeTest\.MetalMaskFixture\.' (8: the two #497 tests)
Discovered / passed / failed / skipped counts:
  MaskCoveragePresentationTest: Windows CUDA 6/6, Windows OpenCL 6/6 (NVIDIA RTX 3080 Laptop GPU,
    D3D11 and OpenGL QRhi), macOS Metal 6/6 (Apple M4): CoverageTransferLeavesPhotoSlotsUnchanged,
    OldCoverageLivesUntilRendererRelease, CoverageTransferDoesNotReadBackToHost,
    CoverageChannelClosesBeforeImageReplacement, CoverageDoesNotReplaceScopePhotoInput,
    FailedTargetCreationKeepsDisplayedCoverage.
  Windows regression group 1: 135 discovered, 135 passed (the 12 above, MaskCoverageTextureCacheTest,
    AiMaskCoverageTextureTest, AiMaskEditSessionTest, GpuDagCudaMaskTest, GpuDagCudaWorkspaceTest,
    EditorViewportSuspensionTest, OpenClMaskFixture).
  Windows regression group 2: 159 discovered, 159 reported passed, 0 failed. The two
    EditorRealRawGpuE2eTest entries end in GTEST_SKIP (disabled in the repository unless
    ALCEDO_RUN_DEADLOCKING_RAW_GPU_E2E is set), so they give no evidence.
  macOS MetalMaskFixture: 26 discovered, 24 passed, 2 failed (#497, unchanged since SM8.1).
  Mutation check: with invert dropped from the display request in PresentMaskCoverage,
    CoverageTransferDoesNotReadBackToHost fails on CUDA (texel 0 red 2 / 255, expected 126 / 255);
    restored before the final run.
Manual verification: none; the coverage texture is not composited before SM8.6.
Numeric differences, latency, and resource measurements:
  Every displayed texel equals the double-precision host reference of the raster Mask rule
    (EvaluateAiMaskR8) within one code on all three backends, with a crop of (0.1, 0.15, 0.8, 0.7),
    12 degrees of rotation, render scales 1 and 0.5, and invert with opacity 0.5. Green and blue
    are exactly 0 and alpha is exactly 1.
  Host transfers: the first presentation of a candidate uploads its R8 once; a presentation with
    another invert and opacity uploads nothing; no presentation downloads to the host.
  Display memory: one RGBA32F target of the photo frame size per producer on CUDA and OpenCL (16
    bytes per photo pixel), plus the native coverage targets of the viewport (at most three slots of
    the same size); Metal keeps one display target per frame that the viewport still holds (two
    during a replacement). No latency measurement in this assignment (SM8.8).
Remaining issues or unavailable platforms: #497 (existing). #508: the photo present queue accepts a
  frame that was written across an image session change of the same image; the coverage channel
  checks the frame identity itself. No AMD or Intel OpenCL device was available. The scene-graph
  glue in EditorViewportRenderer (synchronize and render calls of the coverage layer) is built and
  is exercised only by SM8.6 and later window tests: the real-window E2E test is disabled in the
  repository.
```

Decisions made during SM8.5 (AGENTS.md "Make implementation decisions"):

- The coverage channel is a second `DirectPresentQueue` owned by `EditorViewportItem`, not a fourth
  layer of the photo queue. The photo queue has three slots for three photo layers; a displayed
  coverage frame would hold one of them and stall the next photo write. The second queue reuses
  the slot states, the size handshake, suspension, and the session invalidation unchanged.
- The display write is the committed raster MaskEvaluate with an RGBA32F output, not a new
  sampling kernel. OpenCL and Metal already write `(value, 0, 0, 1)`; CUDA shares the per-pixel
  function. The same Mask therefore has the same effective values in the photo and in the display,
  which SM8.6 needs for `CandidateAndCommittedCoverageUseSameComposite`.
- CUDA and OpenCL write into a workspace display target and copy it into the native target, as the
  photo presenter does with the DRT output. Writing directly into the CUDA array would need the
  surface load/store flag on the shared photo targets. The copy is device-to-device.
- The frame carries its image session in `FramePreviewMetadata` (`session_epoch`,
  `image_identity`), and the sink and the layer compare it. The queue alone does not drop a frame
  that was written across a session change of the same image (#508).
- The SM8.5 surface ends at the render-thread coverage input (`ViewportMaskCoverageLayer::Texture`
  and `Presentation`). The app facade (`UpdateAiMaskCandidateCoverage`,
  `SelectMaskCoveragePreview`, `ClearMaskCoveragePreview`) belongs to SM8.7, where the session and
  the render executor submit this operation; `EditorViewportItem::clearMaskCoverage` is the
  viewport half of the clear operation.
- `EditorMaskCoveragePreviewTest` is not created: SM8.5 adds no app operation. The named behaviors
  are in `MaskCoveragePresentationTest`, which runs each test once per backend
  (`ALCEDO_TEST_EDITOR_BACKEND`, one process per test).
- The failure test also fails the display target allocation on the device, so the failure chain
  has evidence on Metal, where the viewport creates no coverage target.
- The diff exceeds the 2000-line threshold of section 10 by the end-to-end test. Splitting the
  runtime operation from the viewport channel would leave the first part without a test that can
  observe it, so the assignment stays one pull request; #500 records the size.

### 7.6 SM8.6 - One viewport composite

**Objective and prerequisites.** Add the shared display composite after SM8.2-SM8.5.
**Modules and invariants.** Change `editor_viewport_renderer.cpp`, its shader, AppTheme, and `DESIGN.md`.
Use one shader path for all selected coverage sources.

**Steps.**

1. Bind the independent coverage input and visibility/tint uniforms.
2. Apply the equation in section 5.3 after base/detail selection.
3. Use the existing photo geometry for coverage clipping, zoom, pan, crop, and rotation.
4. Keep QSG controls above the composite and retain their existing input owner.
5. Make visibility and tint changes redraw without a photo DAG request.

**Success chain.** Current photo base/detail + selected coverage -> one viewport draw -> existing controls.
**Failure chain.** Shader or binding error -> explicit display error -> no claim of a successful current preview.

**Tests.** `CandidateAndCommittedCoverageUseSameComposite` compares pixels for identical coverage.
`OverlayToggleDoesNotExecutePhotoDag` checks photo dispatch counters.
`CoverageFollowsCropRotationZoomAndDetailRoi` checks independent mapped sample positions.
`CoverageTintNeverAppearsInExportOrScopes` checks those production outputs.
`UncoveredHdrPixelsStayUnchanged` checks negative and above-one values.

**Commands.** Groups A-C; build `alcedo_main` and the proposed presentation test target.
**Exit.** Pixel, mapping, and dispatch assertions pass. Manual appearance is recorded separately.
**Expected diff.** 1000-1700 lines. **Completion record.** Section 7.6.1.

#### 7.6.1 SM8.6 completion record

```text
Phase / date / status: SM8.6 / 2026-10-24 / complete on Windows (CUDA, OpenCL) and macOS (Metal).
Source revision and branch: base b04e5a01 (SM8.5, pull request #509); branch
  feature/ai-segmentation-sm8-6-viewport-coverage-composite (sixth pull request of the SM8
  stack), commits be16a279 and 7c15a508.
Actual changed modules and diff size: 23 files, +2289 / -661 (2950 changed lines; estimate
  1000-1700). About 830 changed lines move the SM8.5 test harness into a shared support header
  without a change of behavior; the new test file is 912 lines. Production code (including
  shaders, theme, QML, DESIGN.md, and CMake) is about 1050 changed lines. Largest files:
  viewport_coverage_composite_test.cpp 912, mask_coverage_presentation_test_support.hpp 513,
  viewport_composite_pass.cpp 375, viewport_composite_pass.hpp 199. editor_viewport_renderer.cpp
  went from 1114 to 950 lines.
Implemented behavior:
  1. The one composite: editor_viewport.frag samples the coverage channel (binding 3) after the
     base/detail photo selection: a = clamp(coverage, 0, 1) * opacity,
     rgb = photo_rgb * (1 - a) + tint_rgb * a, only where a > 0. Alpha is the photo alpha. Pixels
     with a = 0 keep their photo value, including negative and above-one values. Candidate,
     committed Mask, and Grade Union coverage use the same draw; the coverage frame has no texels
     outside its photo frame, so coverage there is 0.
  2. ViewportCompositePass (new module) owns the QRhi resources of the viewport draw (samplers,
     uniform and vertex buffers, bindings, pipeline) and records the pass. Outcome Drawn, NoPhoto,
     or Failed with LastError. EditorViewportRenderer routes the selected photo layer, the visible
     DetailPatch, and the displayed coverage through it, and drops the bindings before a photo or
     coverage texture is destroyed (ViewportMaskCoverageLayer::SetTextureReleaseObserver).
  3. Coverage follows the photo through the common reference space: MapPhotoUvToCoverageUv
     composes photo UV -> photo render -> render_to_reference -> coverage reference_to_render ->
     coverage UV from the two ResolveRenderGeometry results. Photo frames now carry their geometry
     to the render thread: DirectPresentQueue::NotifyReady stores it in the slot (CUDA, OpenCL)
     and DirectFrameSink copies the bound submission geometry into the Metal import. Crop,
     rotation, render scale, RoiFrame primaries, zoom, pan, and the DetailPatch (sampled at the
     base UV) need no separate coordinate rounding. Frames of different reference extents give no
     map and no tint.
  4. Presentation state: EditorViewportItem::maskCoverageVisible, maskCoverageColor, and
     maskCoverageOpacity (GUI thread, read in synchronize). A change emits
     MaskCoverageStyleChanged and calls update() only. EditorWorkspace.qml binds the color and
     opacity to the new AppTheme roles maskCoverageTintColor (accentColor) and
     maskCoverageTintOpacity (0.4); DESIGN.md has the section "Editor viewport Mask coverage
     display" and limits the old "no coverage-area color" rule to the QSG overlay.
  5. The QSG Mask controls stay in EditorOverlayItem (z 2) above the viewport draw and keep their
     input; the composite takes no input.
Primary success call chain:
  scene-graph thread: EditorViewportRenderer::synchronize (copies maskCoverageStyle)
    -> render: consumeDirectFrames / consumeImportedGpuFrames (photo texture + slot geometry)
    -> ViewportMaskCoverageLayer::ConsumeNewest (coverage texture + presentation geometry)
    -> BuildViewportCompositeUniforms (letterbox, zoom, pan, detail ROI,
       MapPhotoUvToCoverageUv, tint) -> ViewportCompositePass::Record (bindings: photo, detail,
       coverage) -> one draw -> QSG controls above it.
  style change: EditorViewportItem::setMaskCoverage* -> MaskCoverageStyleChanged + update()
    -> next synchronize/render; no frame sink, render scheduler, or device call.
Primary failure and restore call chain:
  shader load, binding, or pipeline error -> ViewportCompositePass::Record clears the target and
    returns Failed with LastError -> the renderer logs it and sets the viewport status text to the
    error; it does not report "presented" or note a composed frame.
  no common reference space (different reference extents or empty geometry) -> no coverage map ->
    the photo is drawn without tint.
  coverage texture release (clear, image change, QRhi teardown, replacement) -> observer drops the
    bindings first -> the next draw rebinds the placeholder or the replacement.
Commands and exit codes:
  Windows: cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
      ViewportCoverageCompositeTest MaskCoveragePresentationTest alcedo_main (0)
    cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
      EditorViewportSuspensionTest HeadlessFrameSinkTest EditorSessionRenderSchedulerPortTest
      PipelineSchedulerRequestIdTest MaskCoverageTextureCacheTest AiMaskCoverageTextureTest (0)
    ctest --test-dir build/debug -j 1 --output-on-failure -R
      '^(ViewportCoverageCompositeTest|MaskCoveragePresentationTest)\.' (0, after 7c15a508)
    ctest --test-dir build/debug -j 1 --output-on-failure -R
      '^(EditorViewportSuspensionTest|HeadlessFrameSinkTest|EditorSessionRenderSchedulerPortTest|PipelineSchedulerRequestIdTest|MaskCoverageTextureCacheTest|AiMaskCoverageTextureTest)\.' (0)
  macOS (yurunmac.local, build/macos-debug with ALCEDO_BUILD_TESTS=ON):
    cmake --build build/macos-debug --target ViewportCoverageCompositeTest
      MaskCoveragePresentationTest GpuDagMetalGradeTest alcedo_main (0)
    ctest --test-dir build/macos-debug -j 1 --output-on-failure -R
      '^(ViewportCoverageCompositeTest|MaskCoveragePresentationTest)\.' (0)
    ctest --test-dir build/macos-debug -j 1 -R '^GpuDagMetalGradeTest\.MetalMaskFixture\.'
      (8: the two #497 tests)
Discovered / passed / failed / skipped counts:
  ViewportCoverageCompositeTest: Windows CUDA 7/7, Windows OpenCL 7/7 (NVIDIA RTX 3080 Laptop GPU,
    D3D11 and OpenGL QRhi), macOS Metal 7/7 (Apple M4), each run with gtest [ OK ] output (no
    skip): PhotoUvMapsToCoverageUvThroughTheReferenceSpace,
    CandidateAndCommittedCoverageUseSameComposite, OverlayToggleDoesNotExecutePhotoDag,
    CoverageFollowsCropRotationZoomAndDetailRoi, CoverageTintNeverAppearsInExportOrScopes,
    UncoveredHdrPixelsStayUnchanged, ShaderLoadFailureReportsDisplayError.
  MaskCoveragePresentationTest on the shared harness: Windows CUDA 6/6, OpenCL 6/6, macOS Metal 6/6.
  Windows callers: 71 discovered, 71 passed (EditorViewportSuspensionTest, HeadlessFrameSinkTest,
    EditorSessionRenderSchedulerPortTest, PipelineSchedulerRequestIdTest,
    MaskCoverageTextureCacheTest, AiMaskCoverageTextureTest).
  macOS MetalMaskFixture: 26 discovered, 24 passed, 2 failed (#497, unchanged since SM8.1).
  First macOS run: UncoveredHdrPixelsStayUnchanged failed because the Metal draw returns +0 for a
    -0 photo texel and the check compared bytes; 7c15a508 compares each channel by value.
  Mutation check (Windows CUDA): an identity coverage map made
    PhotoUvMapsToCoverageUvThroughTheReferenceSpace and
    CoverageFollowsCropRotationZoomAndDetailRoi fail; a tint that also blends alpha made
    UncoveredHdrPixelsStayUnchanged fail. Both were restored before the final runs.
Manual verification: none. The renderer glue (synchronize/render calls of the pass) is built
  with alcedo_main; no window test drives it, because the real-window E2E test is disabled in the
  repository. No coverage is presented in the application before SM8.7, so the appearance is not
  visible yet.
Numeric differences, latency, and resource measurements:
  1:1 draws: every pixel equals the composite equation of the read-back coverage texel within
    1e-5 (relative for values above one); uncovered pixels keep their exact photo value, with
    values -0.75, 0, 0.5, 1, 2.5, and 16 and alpha 0.5. Candidate and committed coverage of the
    same R8 and edge (Feather 1.5, Expand 1) give byte-identical composites.
  Mapped draws: a half-scale crop-and-rotation frame at zoom 1.6 with pan and a DetailPatch, and a
    viewport ROI frame, match the independent mapping of the test within 0.01 at more than 1000
    pixels each, including covered and uncovered pixels.
  Export and scope: the export download is byte-identical before and after a tinted coverage
    display on the same device; the scope tap receives a new frame of the same size and image;
    the photo frame of the viewport is byte-identical to the export download; the Mask changes
    the export pixels.
  Style changes: no photo DAG pass counter, host transfer, display target allocation, photo frame,
    adjustment frame request, or size request.
  Resources: the pass adds one 1x1 RGBA32F placeholder, two samplers, one uniform buffer, and one
    vertex buffer, as before; the coverage texture is the SM8.5 channel texture. No latency
    measurement in this assignment (SM8.8).
Remaining issues or unavailable platforms: #497 (existing). No AMD or Intel OpenCL device was
  available. The scope check reads the frame identity of the tap, not its pixels; the pixels
  are the display output that the export check compares.
```

Decisions made during SM8.6 (AGENTS.md "Make implementation decisions"):

- The draw moved into `ViewportCompositePass`. The renderer file was above 1000 lines, and the
  pass needs a real-QRhi test without a scene graph. The renderer keeps layer selection and
  consumption; the pass owns the draw resources.
- Coverage is mapped through the reference space with the geometry of each photo layer, not
  sampled at the photo UV. The photo and coverage grids differ for a RoiFrame primary, a render
  scale change, and a crop change before SM8.7 presents new coverage. The photo queue slot and
  the Metal import now carry the frame geometry that the renderer already stamps.
- The tint color is used as a display-encoded value of the photo display encoding, without a
  color conversion. The tint is decorative and never reaches export or scopes.
- Coverage is visible by default; SM9 adds the visibility control (`maskCoverageVisible`).
- A composite failure clears the target and replaces the status text with the error. SM8.7 adds
  the app-facing completion signal when it presents coverage.
- The tests are in a new `ViewportCoverageCompositeTest` target instead of
  `mask_coverage_presentation_test.cpp`. Both use the harness of
  `mask_coverage_presentation_test_support.hpp`, which also renders a photo on the producer
  device (photo DAG, FramePresenter::Present, and FramePresenter::Download, the export path).
- The diff exceeds the 2000-line threshold of section 10 because of the harness move and the
  real-resource test. Without the move, the new test could not render a photo on the coverage
  device, so the assignment stays one pull request; #501 records the size.

### 7.7 SM8.7 - Session replacement and temporary history

**Objective and prerequisites.** Connect the existing session to the display path after SM8.6.
**Modules and invariants.** Add the focused app operations of section 4 and controller-facing completion signals.
Keep SM5's stack, output cache, write lease, asset-first Commit, and one main history operation.

**Steps.**

1. Open a stored Mask with its saved coverage; make no inference call for that display.
2. Keep its source and current display while a later model request runs.
3. On a successful step, submit the shared original R8 and its current edge parameters.
4. On Undo/Redo, reconstruct from retained outputs and submit the same coverage-only operation.
5. Implement the GPU-error, Retry, Commit, Cancel, and image-navigation rules of section 4.3.
6. Keep display-only slider updates separate from the existing accepted session edge step.

**Success chain.** Model output -> session step -> shared R8 -> GPU edge -> same coverage channel and composite.
**Failure chain.** Inference error -> no step; GPU error -> desired step retained, previous result labelled, Commit disabled;
Retry -> same retained outputs -> successful display -> Commit enabled.

**Tests.** `NewModelRequestKeepsOpenedMaskAndDisplayedCoverage`, `FailedModelRequestKeepsStackAndPreview`,
`UndoAfterNewModelResultRestoresOpenedMaskWithoutInference`, `RedoUsesRetainedModelOutputs`,
`GpuPreviewFailureKeepsDesiredStateAndBlocksCommit`, `CancelRestoresCommittedPreview`, and
`CommitStoresOriginalCoverageAndPublishesOneHistoryOperation`.
Use `AiMaskEditSessionTest`, proposed `EditorMaskCoveragePreviewTest`, and the existing AI edit service tests.
Their registration is in `tests/app/CMakeLists.txt` and the existing editor session test target in `tests/ui/CMakeLists.txt`.

**Commands.** Groups A and B, plus relevant existing service tests discovered with `ctest -N -R`.
**Exit.** The retained-output and temporary-history cases pass through the production facade.
**Expected diff.** 1100-1800 lines. **Completion record.** Section 11; unfilled.

### 7.8 SM8.8 - Latency and resource qualification

**Objective and prerequisites.** Prove the completed GPU/display path after SM8.7.
**Modules and invariants.** Use the production coverage requests and display adapters.
Add only measurement support needed to observe the required behavior.

**Steps.**

1. Repeat the four #494 cases on the same 4096x3072 input in Release on each available backend.
2. Report cold upload, resident-source edge work, display transfer, and request-to-present latency separately.
3. Record median, p95, maximum, GPU model, driver, build, and sample count; use at least 30 resident-source samples.
4. Exercise 100 model-result/edge/Undo/Redo replacements. Record retained textures and peak device bytes.
5. Release the session and verify that Mask allocations return to their measured pre-session count after reader completion.

**Success chain.** Real user operation -> measured GPU work -> real display completion -> recorded evidence.
**Failure chain.** Missed requirement -> keep #494 open with measured facts; do not lower quality or add a substitute.

**Tests and targets.** `RepeatedPreviewReplacementKeepsBoundedGpuResources` and
`SessionCloseReleasesCoverageAfterReadersFinish` must pass.
For the #494 full-image case, require resident-source edge processing below 100 ms at p95 on each qualification device.
Require request-to-present below 150 ms at p95 when the render executor is otherwise idle.
These are completion targets, not measured claims or a promise of 60 fps at sigma 50.
Report the cold path separately and require it below the original 1 s reporting threshold.
If a target fails, the implementation phase stays incomplete; record a concrete optimization in this plan before continuing.

**Commands.** Release builds through groups A/C. Run the named resource tests with `-j 1`.
**Exit.** Numeric, lifecycle, and measured latency evidence exists for CUDA, OpenCL, and Metal.
Unavailable platforms stay explicitly unverified. Do not close #494 from kernel timing alone.
**Expected diff.** 500-1000 lines. **Completion record.** Section 11; unfilled.

## 8. Acceptance matrix

| Behavior | Observable requirement |
| --- | --- |
| New model result B after displayed A | A stays visible while B runs. Original stored coverage and prior model outputs remain accessible. |
| Model failure while A is visible | Error shown; no new stack entry; A remains current. |
| GPU failure after B is accepted | B remains the desired session state; previous display is labelled; Commit disabled until Retry succeeds. |
| Session Undo/Redo | No inference, asset write, or main-history mutation. One current GPU result is rebuilt. |
| Feather drag | Source upload count stays fixed. Display updates; one accepted edge step is recorded at release. |
| Candidate/committed display | Identical effective coverage produces the same overlay pixels through the same composite. |
| Pan/zoom and resize | No repeated source upload or edge processing. Geometry remains aligned. |
| Commit/Cancel | Existing SM5 history behavior holds. Cancel restores the prior committed preview selection. |
| Image change or export resource release | Existing owner ordering ends preview and releases resources safely. No independent GPU cache survives it. |
| Export, thumbnail, scopes | Mask affects intended photo processing. Display tint never enters these outputs. |
| Resource budget | Current and actually-read outputs only for interaction; committed reuse remains bounded by its policy. |

## 9. Build and evidence commands

Before each phase, reread `AGENTS.md` and applicable skills.
Use `alcedo-msvc-cmake`, `opencl-program-registry`, and `alcedo-qml-ui` for their scopes.
Use repository Qt/QML rules for SM9. Do not run the full test suite.
Do not use unreliable offscreen input delivery as the only evidence of product behavior.

Group A, Windows build. Select only targets for the phase:

```powershell
Set-Location D:/Projects/pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AiMaskEditSessionTest GpuDagCudaMaskTest GpuDagOpenClGradeTest alcedo_main
```

Build a proposed test target only after adding and configuring its CMake registration.
Use the existing `win_debug` preset if reconfiguration is required.
For Release measurements, use `win_release` and the same required target names.
Start build time allowances at 20 minutes. Poll the same process and retain healthy progress.

Group B, Windows discovery and targeted GPU run:

```powershell
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -N -R '^(AiMaskEditSessionTest|GpuDagCudaMaskTest|GpuDagOpenClGradeTest)\.'
ctest --test-dir build/debug -j 1 --output-on-failure -R '^GpuDagCudaMaskTest\..*(GpuExpand|GpuFeather|EdgeChange|FailedEdgeWork)'
```

Select the matching named behaviors for other phases. Discovery alone is not execution evidence.
Run only tests for changed code and their direct callers.

Group C, macOS:

```sh
cmake --preset macos_debug -DALCEDO_BUILD_TESTS=ON
cmake --build --preset macos_debug --target GpuDagMetalGradeTest AiMaskEditSessionTest alcedo_main
ctest --test-dir build/macos-debug -j 1 --output-on-failure -R '^GpuDagMetalGradeTest\..*(GpuExpand|GpuFeather|Coverage|EdgeChange)'
```

Put temporary evidence under `build/tmp/sm8/`. Remove task-created temporary files when the task ends.
Keep durable command results and measurements in the completion record.
Report passed, failed, skipped, and unavailable separately. No builds or tests run during this planning edit.

## 10. Risks and required responses

| Risk | Detection | Response |
| --- | --- | --- |
| Gaussian remains too slow | SM8.8 targets fail | Keep the phase incomplete and optimize the exact finite convolution. No quality change. |
| RGBA32F display target costs too much memory | Measured transport bytes and peak allocation | Count it explicitly. Do not create an analysis-sized RGBA32F copy when the displayed photo grid is smaller. |
| A bare span outlives current coverage | Delayed upload after a session step | Use the same immutable shared allocation through completion. |
| Previous display is overwritten early | Delayed real reader during replacement | Fix existing lease use; do not add GPU history. |
| App/render queue ownership changes | Source differs from this audit | Re-audit the actual calls before adding IDs, tokens, or concurrent mutation. |
| Existing unrelated GPU tests fail | #496 or #497 reproduction | Record those issues separately. Do not weaken coverage assertions. |
| Phase estimate can exceed 2000 lines | Reviewed scope before implementation | Split the assignment and update its issue before continuing. |

## 11. Completion records

Create one record per assignment. Leave every assignment planned until its evidence exists.

```text
Phase / date / status:
Source revision and branch:
Actual changed modules and diff size:
Implemented behavior:
Primary success call chain:
Primary failure and restore call chain:
Commands and exit codes:
Discovered / passed / failed / skipped counts:
Manual verification:
Numeric differences, latency, and resource measurements:
Remaining issues or unavailable platforms:
```
