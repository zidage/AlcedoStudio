# SM6 Raster Mask Evaluate on CUDA, OpenCL, and Metal — Phase Plan

Replan note (2026-10-24): [SM8 GPU Coverage and One Display Composite](phase_sm8_gpu_coverage_and_display_plan.md)
prospectively replaces host edge processing and edge-keyed interactive retention from R3/R6.
It adds one viewport composite shared by candidate and committed coverage.
SM6 completion evidence below still describes the implemented host path.
The former statement that session edge changes do not render does not apply to the new SM8 preview design.
Performance work remains tracked by #494. No SM8 implementation is claimed here.

Date: 2026-10-24.

Status: complete on Windows and macOS on 2026-10-24. SM6.1 section 14.1, SM6.2 section 14.2.

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), section
9.5. Issues: SM6.1 #492, SM6.2 #493. Tracking issue: #491.

Prerequisites:

- SM5.1 ([phase plan](phase_sm5_ai_mask_source_and_session_plan.md), merged in `9a35280d`): the
  `AiSegmentationMaskSource`, the `MaskAsset` table, and `PipelineMgmtService::ReadMaskAsset`.
- SM5.3: the editor Commit writes the asset before the history publish (M12), so each document
  that a renderer receives refers only to stored assets.

Source revision used for the audit: `7eb94016` (`main`, PR #490 merged).

---

## 1. Decision record

### 1.1 User request (2026-10-24)

- Open issues for the SM6 tasks of the master plan and start SM6.
- SM6 makes a committed AI Mask render. Until SM6, each render of a document with an AI Mask
  stops with the error "AI Segmentation Mask raster evaluation is not available in this build"
  (SM5 M21).

### 1.2 Inherited decisions

| ID | Decision |
| --- | --- |
| D6 | One R8 coverage per Mask, Union by per-pixel maximum, Grade Mix. |
| D7 | The model reads the whole source image. A crop or rotation change must not make an AI Mask incomplete. |
| D17 | The stored asset is the composed model coverage before Feather and Expand, over its bounding rectangle (P3). |
| D20 | Feather and Expand apply when the runtime makes the coverage texture. Export uses the same function, so preview and export agree. |
| M8 | `feather_px` in [0, 50] is a Gaussian sigma; `expand_px` in [-50, 50] dilates (> 0) or erodes (< 0). Both are pixels of the analysis image. The semantics are lab function F8 (`adjust_mask` in the lab `mask_pipeline.py`). |
| M9 | The asset covers the bounding rectangle of the values > 0, widened by 1 pixel on each side and clipped to the image. |
| M21 | A path that cannot evaluate an AI Mask reports a real error. No path draws an empty or substitute Mask. |
| Master 3.3 | A corrupt R8 asset makes the render report the real error. It does not render an empty Mask. |

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| R1 | A new render port `MaskAssetReader` (`include/edit/runtime/mask_asset_reader.hpp`) reads a verified `MaskAsset` by key. It travels like `LutResourceResolver`: the executor owner gives it to `PipelineExecutor`, which gives it to each `Renderer`, which sets it on the device workspace (`BasicRenderWorkspace::SetMaskAssets`). The default reader throws "Mask asset <key> cannot be read: this renderer has no project mask asset store". | The asset bytes live in the project database (D17). The runtime has no storage access. The LUT port is the existing pattern for a render input that lives outside the document. |
| R2 | Reader implementations: `ProjectMaskAssetReader` (app) reads through the project `Storage` with `MaskAssetStore`, one connection guard per read. `PipelineMgmtService` owns one instance; `ReadMaskAsset` uses it, and `MaskAssets()` returns it to the thumbnail and export executor pools. The editor executor lives for the application, so it gets `CurrentProjectMaskAssetReader` (album backend), which resolves the pipeline service of the open project at each read, as `EditorSessionMaskAssetPort` does. | Thumbnail and export services exist per project. The editor render port exists once per application and must read the assets of the project that is open now. |
| R3 | `MakeAiMaskCoverageTexture(asset, analysis_extent, edge)` (new library `EditMaskRaster`, OpenCV core and imgproc) makes the coverage texture on the host: place the asset in analysis pixels (the rule of `PlaceAiMaskAssetInCoverage`, moved to `AiMaskAssetPixelRect` in `EditMaskCore`), widen the rectangle by the edge margin and clip it to the analysis image, apply F8 in float (`expand_px` steps of 3 x 3 cross then 3 x 3 square, `BORDER_REPLICATE`; then `GaussianBlur` with sigma `feather_px`, `BORDER_REPLICATE`), and store `round(clamp(v) * 255)` as R8. The result has a `MaskAssetDescriptor` (texture extent and `reference_bounds`) and the pixels. | One host function serves all three backends, the editor, thumbnails, and export, so their coverages are the same bytes (D20). F8 runs once per asset and edge, not per frame. |
| R4 | Edge margin: `max(expand_px, 0) + (feather_px > 0 ? ceil(4 * feather_px) + 1 : 0)` analysis pixels on each side. A side of the widened rectangle that reaches the image edge is clipped there; `BORDER_REPLICATE` then applies at the image edge, as in the lab, which works on the whole image. | The OpenCV float Gaussian kernel radius is at most `4 * sigma + 1`, and each expand step grows the coverage by 1 pixel, so the texture holds every value > 0 of the whole-image F8 result, and each texel equals the whole-image result. |
| R5 | `expand_px = 0` and `feather_px = 0` make a texture that equals the asset bytes and bounds; F8 does not run. | No cost for the common case. |
| R6 | `MaskCoverageTextureCache<Backend>` in `BasicRenderWorkspace` keeps one R8 texture for each (asset key, `feather_px` bits, `expand_px`). A miss reads the asset (R1), builds the texture (R3), and uploads it once. The cache has a byte budget of 256 MiB; a new entry evicts the least recently used entries that no in-flight submission uses. `ReleaseSessionResources` clears it: the editor at an image change and each batch task at its end. | A frame must not read the database or run F8. A Feather change makes a new key; the old entry leaves by the budget or at the image change. |
| R7 | Sampling rule of the raster `MaskEvaluate` pass. For each render pixel, `MakeRasterMaskSamplingPlan(plan.geometry, texture bounds, texture extent)` gives `render_to_texture_uv`. The pass takes `S x S` samples at the offsets `((i + 0.5) / S - 0.5, (j + 0.5) / S - 0.5)` around the pixel center, `S = clamp(ceil(2^mip_level), 1, 8)`. Each sample maps to UV; a sample with UV outside `[0, 1]` on an axis gives 0; otherwise it reads the texture bilinearly at texel centers with clamp-to-edge indices. The mean of the samples, divided by 255, is the source coverage. Then the pass applies the Mask `invert` and `opacity` and quantizes to R8 as the analytic pass does. | Bilinear magnification at zoom; an area mean at preview scales, so a hard model edge does not alias. No mip chain to store. The same float operations in the same order on each backend. |
| R8 | The compiler compiles an AI Mask to the same `MaskEvaluate` pass as an analytic Mask. The M21 compile error is removed. The thumbnail spec, the host `GradeMaskCoverage`, the analytic edit, and the overlay keep their M21 errors; SM9 owns the Mask thumbnail from R8. | The pass reads the source kind from the live Mask model, as the analytic branches do. |
| R9 | A failed read, F8, or upload fails the render with the real message. The cache keeps no entry for a failed key, so the next render tries again. | Master plan 3.3. |
| R10 | No new consistency mechanism. The cache key is the content (asset key and edge). The workspace runs on one render thread per device, as today. | AGENTS.md: no race exists without a real interleaving. |
| R11 | The result content key and the runtime invalidation do not change: SM5.1 already mixes the asset key, the descriptor, `feather_px`, and `expand_px` into the Mask content key. | — |

### 1.4 Rejected options

- Feather and Expand on the GPU. Three implementations of F8 would need three agreement checks,
  and the coverage would change only once per commit.
- A mip chain per texture. R7 needs no stored levels.
- Reading the asset in each frame. A read inflates up to 16 MB and recomputes the key.
- Zero coverage for a missing or corrupt asset (M21, master plan 3.3).
- A reader bound to one project for the editor executor (R2).

---

## 2. Product specification

SM6 has no new control. After SM6, a document with an AI Mask renders in the editor, in
thumbnails, and in export. The Mask follows crop, rotation, zoom, and the region-of-interest
render, because each render pixel maps to ReferenceSpace (D7). Feather and Expand of the source
change the coverage. Invert, opacity, and enabled of the Mask apply as for the analytic Masks.

| Case | Result |
| --- | --- |
| Render pixel outside the texture bounds | Source coverage 0 (before invert). |
| Preview scale (render smaller than the texture) | Area mean of up to 8 x 8 bilinear samples. |
| Zoom (render larger than the texture) | Bilinear magnification. |
| Asset missing from the project | The render fails with "Mask asset <key> is missing from the project". |
| Asset corrupt | The render fails with "Mask asset <key> is corrupt: <reason>". |
| Renderer without a project store (tools, tests) | The render fails with the default reader message (R1). |
| Erode removes all coverage | Zero coverage. This is the requested result, not an error. |

---

## 3. Scope

### 3.1 Included

| Module | Responsibility |
| --- | --- |
| `edit/mask` | `AiMaskAssetPixelRect` (EditMaskCore); `MakeAiMaskCoverageTexture` (new EditMaskRaster). |
| `edit/runtime` | `MaskAssetReader` port; workspace reader and texture cache; compiler change. |
| `edit/runtime/{cuda,opencl,metal}` | Raster branch of the `MaskEvaluate` pass and its kernel. |
| `edit/pipeline` | `PipelineExecutor` takes and passes the reader. |
| `app` | `ProjectMaskAssetReader`; `PipelineMgmtService::MaskAssets`; `BatchExecutorPool`, `ThumbnailService`, `ExportService` pass it. |
| `app/ai_mask_coverage_composition` | `PlaceAiMaskAssetInCoverage` uses `AiMaskAssetPixelRect`. |
| `ui/alcedo_main/album_backend` | `CurrentProjectMaskAssetReader` for the editor executor. |

### 3.2 Excluded

- The D9 notice (SM7), the coverage preview request (SM8), the tool UI and the Mask thumbnail from
  R8 (SM9).
- The Brush Mask raster path (Brush master plan). R7 and the cache can serve it later; this plan
  does not change Brush code.

---

## 4. Current source audit

All paths are under `alcedo_studio/src/` unless they start with `alcedo_studio/tests/` or `docs/`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Compile | `edit/runtime/graph_compiler.cpp:268-317` (`CompileGradeOwnedMaskStack`) | One `MaskEvaluate` per Mask; throws for `AiSegmentation` (M21). | R8. |
| CUDA pass | `edit/runtime/cuda/cuda_mask_pass.cu:104-127` | `AnalyticMaskKernel` for Radial and Linear Gradient; throws for AI. Output R8 at `render_extent` through `AcquireImageForWrite`. | Raster kernel (R7). |
| OpenCL pass | `edit/runtime/opencl/opencl_mask_pass.cpp:160-185`, `shader/mask.cl`, `include/edit/runtime/opencl/opencl_dag_programs.hpp:47-49` | Kernels `mask_analytic_r8`, `mask_fill_zero_r8`, `mask_union_max_r8` in program `opencl_dag_mask`; throws for AI. | Kernel `mask_raster_r8` (R7). |
| Metal pass | `edit/runtime/metal/metal_mask_pass.mm:160-180`, `shader/mask.metal` | Analytic kernel; throws for AI. | Raster kernel (R7). |
| Sampling plan | `include/edit/geometry/texture_sampling_plan.hpp`, `edit/geometry/texture_sampling_plan.cpp` | `MakeRasterMaskSamplingPlan` gives `render_to_texture_uv`, `uv_dx`, `uv_dy`, `mip_level`. No production caller. | Used by the three passes. |
| LUT port (pattern) | `include/edit/runtime/lut_resource_resolver.hpp`; `PipelineMgmtService::LutResources` → `BatchExecutorPool` (`include/app/batch_executor_pool.hpp:38-57`), `ThumbnailService` (`app/thumbnail_service.cpp:264-267`), `ExportService` (`app/export_service.cpp:139-142`), editor (`ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp:476-499`) → `PipelineExecutor` → `Renderer` (`include/edit/runtime/renderer.hpp:211-233`) → `BasicRenderWorkspace::SetLutResources` | The render resolves an input that lives outside the document. | Same path for `MaskAssetReader` (R1, R2). |
| GPU LUT cache (pattern) | `CudaBackend::AcquireLut` (`include/edit/runtime/cuda/cuda_backend.hpp:243-247`, `lut_cache_`) | Content-keyed device cache with an LRU byte budget and busy checks. | R6 follows its eviction rule in the workspace. |
| Session release | `BasicRenderWorkspace::ReleaseSessionResources`; callers `Renderer::ReleaseDeviceResources` (`renderer.hpp:280-289`) and the batch release (`detail/renderer.inl.hpp:220-227`) | Drops results and textures at an image change and after each batch task. | Also clears the coverage cache (R6). |
| Asset store | `PipelineMgmtService::ReadMaskAsset` (`app/pipeline_service.cpp:351-356`), `MaskAssetStore::Read` | Verified read, one guard per call. | Delegates to `ProjectMaskAssetReader` (R2). |
| Asset placement | `app/ai_mask_coverage_composition.cpp:204-226` (`PlaceAiMaskAssetInCoverage`) | `lround` of the bounds, size check within 0.5 px. | Uses `AiMaskAssetPixelRect`. |
| Lab F8 | `alcedo-ai-mask-lab` `scripts/poc/ai_mask_mvp/mask_pipeline.py:1121-1140` (`adjust_mask`) | Float mask; `round(|edge|)` steps of `cv2.dilate`/`cv2.erode` with 3 x 3 cross and square kernels in turn, `BORDER_REPLICATE`; then `cv2.GaussianBlur(sigma)`, `BORDER_REPLICATE`; clip to [0, 1]. | R3 reproduces it. |
| Mask tests | `alcedo_studio/tests/edit/runtime/{cuda,opencl,metal}_mask_test.cpp`, `multi_mask_runtime_test_support.hpp`; targets `GpuDagCudaMaskTest`, `GpuDagOpenClGradeTest`, `GpuDagMetalGradeTest` | Analytic coverage against a host reference. | AI Mask cases. |

---

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Changes | Lifetime | Errors |
| --- | --- | --- | --- | --- | --- |
| `MaskAssetReader` implementation | key | verified `MaskAsset` | nothing | executor | `std::runtime_error`: missing, corrupt, no store |
| `MakeAiMaskCoverageTexture` | asset, analysis extent, edge | `AiMaskCoverageTexture` | nothing | call | `std::runtime_error`: asset does not fit, invalid edge |
| `MaskCoverageTextureCache<Backend>` | AI source, reader, backend, command context | texture and its descriptor | its entries | render workspace | the reader, F8, and upload errors |
| `ExecuteCudaMask` / `ExecuteOpenClMask` / `ExecuteMetalMask` | plan, document, compiled Mask | R8 effective coverage | the output image | one render | as above |

### 5.2 Success chain (editor render)

```text
EditorSessionRenderSchedulerPort::EnsureExecutor
  -> PipelineExecutor(Interactive, lut_resources, CurrentProjectMaskAssetReader)
  -> Renderer::EnsureDevice -> Workspace().SetMaskAssets(reader)
render of a document with an AI Mask
  -> GraphCompiler: MaskEvaluate for the AI Mask (R8)
  -> ExecuteCudaMask(compiled source) -> AiSegmentationMaskSource
     -> Workspace().MaskCoverage().Acquire(source, Workspace().MaskAssets(), device, context)
        hit: texture
        miss: reader.Read(key) -> PipelineMgmtService (open project) -> ProjectMaskAssetReader
              -> MaskAssetStore::Read (verified)
              -> MakeAiMaskCoverageTexture(asset, analysis_extent, edge)   (R3, R4, R5)
              -> CreateTexture2D(R8) -> UploadTexture2D -> insert, evict over budget (R6)
     -> MakeRasterMaskSamplingPlan(geometry, texture bounds, texture extent)
     -> raster kernel (R7) -> R8 effective coverage -> MaskUnion -> Grade Mix
```

Thumbnail and export use the same chain with `ProjectMaskAssetReader` from
`PipelineMgmtService::MaskAssets()`.

### 5.3 Failure chain

```text
reader.Read throws (missing, corrupt, no store) | MakeAiMaskCoverageTexture throws | upload throws
  -> cache inserts nothing -> ExecuteXMask throws -> the render fails with the message
  -> the editor keeps its last published frame and reports the render error, as for other
     render failures; a batch task fails and releases its resources
```

### 5.4 Copies

The coverage texture is an output buffer of R3, not a copy of owner state. The cache owns the
device texture; the host bytes are released after the upload.

---

## 6. File and API map

### 6.1 Proposed files

| File | API |
| --- | --- |
| `include/edit/runtime/mask_asset_reader.hpp`, `edit/runtime/mask_asset_reader.cpp` | `class MaskAssetReader { virtual auto Read(const MaskAssetKey&) const -> MaskAsset = 0; }`; `DefaultMaskAssetReader()` (shared reader that throws R1). Thread: `Read` can run on several render threads at the same time. |
| `include/edit/mask/ai_mask_coverage_texture.hpp`, `edit/mask/ai_mask_coverage_texture.cpp` | `struct AiMaskCoverageTexture { MaskAssetDescriptor descriptor; std::vector<std::uint8_t> pixels; }`; `MakeAiMaskCoverageTexture(const MaskAsset&, Extent2D analysis_extent, const AiMaskEdgeAdjustment&) -> AiMaskCoverageTexture`. Pure function. |
| `include/edit/runtime/mask_coverage_texture_cache.hpp` | `template <class Backend> class MaskCoverageTextureCache` with `Acquire(...) -> const Entry&`, `Clear()`, `EntryCount()`, `UploadCount()`, `UsedBytes()`. |
| `include/app/project_mask_asset_reader.hpp`, `app/project_mask_asset_reader.cpp` | `class ProjectMaskAssetReader final : public MaskAssetReader { explicit ProjectMaskAssetReader(std::shared_ptr<Storage>); }`. |
| `ui/alcedo_main/album_backend/current_project_mask_asset_reader.{hpp,cpp}` | `class CurrentProjectMaskAssetReader final : public MaskAssetReader` over `std::function<std::shared_ptr<PipelineMgmtService>()>`; "No project is open to read Mask asset <key>" when no service exists. |
| `alcedo_studio/tests/edit/mask/ai_mask_coverage_texture_test.cpp` | SM6.1 tests. |
| `alcedo_studio/tests/edit/runtime/ai_mask_raster_reference.hpp` | Independent host reference of R7 for the backend tests. |

### 6.2 Current files that change

| File | Change |
| --- | --- |
| `include/edit/mask/mask_asset.hpp`, `edit/mask/mask_asset.cpp` | `AiMaskAssetPixelRect(const MaskAssetDescriptor&, Extent2D analysis_extent) -> RectI` (throws when the asset does not fit). |
| `app/ai_mask_coverage_composition.cpp` | `PlaceAiMaskAssetInCoverage` calls `AiMaskAssetPixelRect`. |
| `edit/CMakeLists.txt` | Library `EditMaskRaster`; `EditRuntime` links it; `mask_asset_reader.cpp` in `EditRuntime`. |
| `include/edit/runtime/basic_render_workspace.hpp` | `SetMaskAssets`, `MaskAssets`, `MaskCoverage`; `ReleaseSessionResources` clears the cache. |
| `include/edit/runtime/renderer.hpp`, `include/edit/pipeline/pipeline_executor.hpp`, `edit/pipeline/pipeline_executor.cpp` | Reader parameter after `lut_resources`. |
| `include/app/batch_executor_pool.hpp`, `app/thumbnail_service.cpp`, `app/export_service.cpp` | Pass `MaskAssets()`. |
| `include/app/pipeline_service.hpp`, `app/pipeline_service.cpp` | `mask_assets_`, `MaskAssets()`; `ReadMaskAsset` delegates. |
| `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp` | Editor executor gets `CurrentProjectMaskAssetReader`. |
| `edit/runtime/graph_compiler.cpp` | R8. |
| `edit/runtime/cuda/cuda_mask_pass.cu`, `edit/runtime/opencl/opencl_mask_pass.cpp`, `edit/runtime/opencl/shader/mask.cl`, `include/edit/runtime/opencl/opencl_dag_programs.hpp`, `edit/runtime/metal/metal_mask_pass.mm`, `edit/runtime/metal/shader/mask.metal` | Raster branch and kernel. |

---

## 7. Implementation phases

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| SM6.1 | Coverage texture function (F8), reader port, and its path from the project to the render workspace | `edit/mask`, `edit/runtime` port, `edit/pipeline`, `app`, album backend | SM5.1 | 900-1,300 | complete (14.1) |
| SM6.2 | Workspace texture cache, compiler change, raster `MaskEvaluate` on CUDA, OpenCL, and Metal | `edit/runtime`, three backends | SM6.1 | 1,100-1,600 | complete (14.2) |

SM6 is split in two parts because one part with the host function, the port path, and three
backend kernels with their tests can pass 2,000 lines. SM6.1 changes no render result: the passes
still report the M21 error until SM6.2.

### 7.1 SM6.1 — Coverage texture and asset reader port

**Objective and deliverables.** The host can make the coverage texture of an AI Mask with F8. Each
renderer of the application has a reader for the assets of its project.

**Inputs.** `main` with SM5.

**Data rules and invariants.** R1 to R5. The texture equals the whole-image F8 result inside its
rectangle. The reader returns only verified assets.

**Implementation steps.**

1. Add `AiMaskAssetPixelRect` to `mask_asset`: `left = lround(x * W)`, `top = lround(y * H)`; the
   asset must fit in the image, and `|w * W - width|` and `|h * H - height|` must be below 0.5.
   Change `PlaceAiMaskAssetInCoverage` to call it.
2. Add `MakeAiMaskCoverageTexture` in `EditMaskRaster` (R3 to R5). Validate the edge with the M8
   ranges. Use `cv::Mat` over the float rectangle; `cv::dilate`/`cv::erode` with
   `cv::getStructuringElement(MORPH_CROSS | MORPH_RECT, {3, 3})` in turn; `cv::GaussianBlur(src,
   dst, {0, 0}, feather_px, feather_px, BORDER_REPLICATE)`. Include only `opencv2/core.hpp` and
   `opencv2/imgproc.hpp`.
3. Add `MaskAssetReader` and `DefaultMaskAssetReader`.
4. Add `ProjectMaskAssetReader`; give `PipelineMgmtService` one instance; `ReadMaskAsset`
   delegates; add `MaskAssets()`.
5. Add the reader parameter to `PipelineExecutor`, `Renderer`, `BatchExecutorPool`; add
   `BasicRenderWorkspace::SetMaskAssets` and `MaskAssets` (not while rendering, as
   `SetLutResources`). Pass `MaskAssets()` from `ThumbnailService` and `ExportService`.
6. Add `CurrentProjectMaskAssetReader`; give it to the editor executor in `EnsureExecutor`.

**Primary success call chain.** Section 5.2, the reader and texture parts.

**Primary failure chain.** An asset that does not fit gives "AI Mask asset <key> does not fit the
analysis image W x H"; an edge outside M8 gives the range message; the default reader gives R1.

**Tests and evidence.** Section 8.1.

**Exit criteria.**

- [x] The tests of section 8.1 pass.
- [x] The existing tests of the changed targets pass, or each failure also fails on `main`.
- [x] `AlbumBackendLib` and `alcedo_main` build.
- [x] The F8 time of section 9 is recorded.

**Expected diff.** 900-1,300 lines (production 450-600, tests 350-550, build 30, plan 80).

### 7.2 SM6.2 — Raster MaskEvaluate on three backends

**Objective and deliverables.** A document with an AI Mask renders on CUDA, OpenCL, and Metal with
the coverage of R7.

**Inputs.** SM6.1.

**Data rules and invariants.** R6 to R11. One read, one F8, and one upload per cache key while the
entry stays. The pass output has `render_extent` and R8 format, as the analytic pass.

**Implementation steps.**

1. Add `MaskCoverageTextureCache<Backend>` (R6) and its workspace member; clear it in
   `ReleaseSessionResources`. Eviction skips entries whose last submission is busy
   (`Backend::IsResourceBusy`) or that the current frame uses.
2. Remove the M21 throw for AI Masks from `CompileGradeOwnedMaskStack` (R8).
3. CUDA: add `RasterMaskKernel` (R7) and the AI branch of `ExecuteCudaMask`.
4. OpenCL: add `mask_raster_r8` to `mask.cl` and its name to `opencl_dag_programs.hpp`; add the AI
   branch of `ExecuteOpenClMask` with a packed parameter struct and a `static_assert` on its size.
5. Metal: add the raster kernel to `mask.metal` and the AI branch of `ExecuteMetalMask`.
6. Add the host reference of R7 to the tests (double precision, separate code).

**Primary success call chain.** Section 5.2.

**Primary failure chain.** Section 5.3.

**Tests and evidence.** Section 8.2.

**Exit criteria.**

- [x] The tests of section 8.2 pass on CUDA and OpenCL (Windows) and on Metal (macOS).
- [x] The existing mask, grade, and geometry tests of the changed targets pass, or each failure
      also fails on `main`.
- [x] `AlbumBackendLib` and `alcedo_main` build on Windows; the Metal targets build on macOS.

**Expected diff.** 1,100-1,600 lines (production 550-750, tests 500-750, plan 80).

---

## 8. Tests

### 8.1 SM6.1

| Test | Target | Assertion |
| --- | --- | --- |
| `CoverageTextureWithoutEdgeEqualsAssetBytes` | `AiMaskCoverageTextureTest` (new) | Same descriptor and bytes as the asset (R5). |
| `ExpandDilatesByAlternatingCrossAndSquare` | same | One 255 pixel, `expand_px = 2`: the 5 x 5 square without its 4 corners is 255, the rest 0; the texture grew by 2 pixels on each side. |
| `NegativeExpandErodes` | same | A 7 x 7 block, `expand_px = -2`: a 3 x 3 block stays after the cross and square steps; the texture has the asset rectangle. |
| `FeatherMatchesIndependentGaussian` | same | `feather_px = 3` on a square: each texel within 1 of a double-precision normalized Gaussian convolution (radius `ceil(4 sigma)`) of the zero-padded mask. |
| `TextureClipsAtImageEdgeAndReplicatesThere` | same | Asset at the left image edge, `feather_px = 2`: the texture starts at x = 0, and the left column equals the whole-image result with replicated edges (independent reference). |
| `TextureHoldsAllNonZeroValuesOfTheWholeImageResult` | same | Asset in the middle, `expand_px = 3`, `feather_px = 2.5`: the whole-image F8 result (reference) is 0 outside the texture rectangle and equal inside. |
| `CoverageTextureRejectsAssetThatDoesNotFit` | same | Bounds outside the analysis image, or a size mismatch of 0.5 px or more: `std::runtime_error`. |
| `AssetPixelRectMatchesSessionPlacement` | `AiMaskEditSessionTest` | `PlaceAiMaskAssetInCoverage` keeps its results (existing tests pass). |
| `DefaultMaskAssetReaderReportsMissingStore` | `AiMaskCoverageTextureTest` | The message names the key and "no project mask asset store". |
| `ProjectMaskAssetReaderReadsStoredAsset` | `MaskAssetStoreTest` | Put through `PipelineMgmtService`, then `MaskAssets()->Read` returns the same pixels; an unknown key gives the missing error. |
| `CurrentProjectMaskAssetReaderReportsNoOpenProject` | `AlbumBackend` test of the editor ports (name confirmed at implementation) | Without a pipeline service, `Read` throws "No project is open". |

### 8.2 SM6.2

Each backend file (`cuda_mask_test.cpp`, `opencl_mask_test.cpp`, `metal_mask_test.cpp`) gets the
same cases with its prefix:

| Test | Assertion |
| --- | --- |
| `AiMaskCoverageMatchesIndependentReferenceAtFullAndPreviewScale` | Render scale 1 and 0.5: each R8 value within 1 of the host reference of R7. |
| `AiMaskCoverageFollowsCropAndRotation` | Crop and rotation 30 degrees: within 1 of the reference with the same geometry. |
| `AiMaskOutsideTextureBoundsIsZero` | Pixels that map outside the bounds are 0; with invert they are 255. |
| `AiMaskInvertAndOpacityApplyBeforeUnion` | AI Mask with opacity 0.5 and invert, and a Radial Mask: Union = per-pixel maximum of the two references, within 1. |
| `AiMaskFeatherAndExpandChangeCoverage` | The reference over `MakeAiMaskCoverageTexture` output matches; the coverage differs from the edge-free render. |
| `AiMaskCoverageTextureUploadsOncePerAssetAndEdge` | Two renders: one cache upload. A Feather change: a second upload. `ReleaseSessionResources`: the cache is empty. |
| `AiMaskMissingAssetFailsRender` | A reader without the key: the render throws with "Mask asset <key> is missing"; the cache is empty. |

Also: `GpuDagRawInputTest.AiSegmentationMaskCompileReportsMissingRasterEvaluation` changes to
`AiSegmentationMaskCompilesToMaskEvaluate` (one `MaskEvaluate` pass for the AI Mask).

---

## 9. Measurements

- F8 time of `MakeAiMaskCoverageTexture` for a 4096 x 3072 analysis image and a full-image asset
  with `feather_px = 50` and `expand_px = 50`, and for `feather_px = 5`, `expand_px = 2`
  (release build, Windows). Record the values; no target exists. A value above 1 s is a GitHub
  issue for SM9 (Feather and Expand change during the session do not render, so this time is paid
  once per commit or reopen).
- Raster pass GPU time is not measured separately; the analytic pass has no measurement either.

---

## 10. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target <targets of the part>
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(<targets>)\."
```

Targets: SM6.1 `AiMaskCoverageTextureTest MaskAssetStoreTest AiMaskEditSessionTest
GpuDagGeometryTest AlbumBackendLib alcedo_main`; SM6.2 `GpuDagCudaMaskTest GpuDagOpenClGradeTest
GpuDagRawInputTest GpuDagModelGraphTest AlbumBackendLib alcedo_main` on Windows, and
`GpuDagMetalGradeTest` on macOS (`macos_debug` with `-DALCEDO_BUILD_TESTS=ON`). Confirm the target
names in the test `CMakeLists.txt` files. Logs go under `build/tmp/sm6/`.

---

## 11. Repository rules that apply

- Read `AGENTS.md` and the applicable skills again before implementation (`opencl-program-registry`
  for the OpenCL kernel).
- No fallback: a missing or corrupt asset and a failed upload fail the render.
- No new consistency mechanism (R10).
- Include the defining header; include only the needed OpenCV module headers.
- LF line endings. Format new files completely; format only the changed lines of existing files.
- Run only the targets of section 10. Only the user starts a full test suite. Run GPU tests with
  `-j 1`.
- Make implementation decisions and record them in section 14; open a GitHub issue for each problem
  found during the work.

---

## 12. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| F8 is slow for large Feather values. | Section 9 measurement above 1 s. | Record it and open an issue; do not lower the quality (no fallback). |
| A backend upload is asynchronous from a host buffer that the cache frees. | Wrong or random coverage in the backend test. | Keep the host bytes until the upload completes, as the backend requires. |
| The Mac is not available. | SSH failure. | Report the Metal build and tests as not run; open an issue. |
| Backends differ by more than 1. | Section 8.2 tests. | Find the float operation order difference; do not widen the tolerance. |

Stop implementation under the conditions of the master plan section 12.

---

## 13. Issues

Tracking issue #491 with the sub-issues #492 (SM6.1) and #493 (SM6.2).

---

## 14. Completion record

### 14.1 SM6.1

```text
Phase / date / status: SM6.1 / 2026-10-24 / complete.
Source revision and branch: base 7eb94016 (main); branch
  feature/ai-segmentation-sm6-1-coverage-texture.
Actual changed modules: edit/mask (AiMaskAssetPixelRect, MakeAiMaskCoverageTexture, new library
  EditMaskRaster), edit/runtime (MaskAssetReader port, workspace reader), edit/pipeline
  (PipelineExecutor), app (ProjectMaskAssetReader, PipelineMgmtService, BatchExecutorPool,
  ThumbnailService, ExportService, PlaceAiMaskAssetInCoverage), album backend
  (CurrentProjectMaskAssetReader, editor executor), tests.
Implemented behavior: R1 to R5; the steps 1 to 6 of section 7.1.
Explicitly unimplemented items: none of SM6.1. The texture cache and the raster pass are SM6.2;
  until then the passes report the M21 error.
Primary success call chain: section 14.1.1.
Primary failure and restore call chain: section 14.1.1.
Build and test commands with exit codes: section 14.1.2.
Discovered / passed / failed / skipped counts: section 14.1.2.
Manual verification: none; no render result changes in SM6.1.
Performance or resource evidence, if required: section 14.1.3.
Evidence path: build/tmp/sm6/ (removed at completion).
Remaining defects or unavailable platforms: #494 (Feather 50 on a full 4096 x 3072 image takes
  1.5 s). SM6.1 has no platform code; the macOS build runs with SM6.2.
```

#### 14.1.1 Call chains

```text
PipelineMgmtService(storage) -> mask_assets_ = ProjectMaskAssetReader(storage)
ThumbnailService / ExportService -> BatchExecutorPool(count, preference, LutResources(),
  MaskAssets()) -> PipelineExecutor(Batch, lut, reader) -> Renderer(role, unpack, lut, reader)
  -> EnsureDevice -> Workspace().SetMaskAssets(reader)
EditorSessionRenderSchedulerPort::EnsureExecutor
  -> PipelineExecutor(Interactive, lut, CurrentProjectMaskAssetReader(pipeline_port))
CurrentProjectMaskAssetReader::Read(key) -> pipeline_port->PipelineMapper() (open project)
  -> PipelineMgmtService::ReadMaskAsset -> ProjectMaskAssetReader::Read
  -> GetConnectionGuard -> Lock -> MaskAssetStore::Read (verified)
MakeAiMaskCoverageTexture(asset, analysis_extent, edge)
  -> ValidateMaskAssetPixels -> AiMaskEdgeMargin (ValidateAiMaskEdgeAdjustment)
  -> AiMaskAssetPixelRect -> edge 0: the asset
  -> R8 matrix over the widened, clipped rectangle; asset copied in
  -> |expand_px| steps of dilate or erode (cross, square, ...) over row bands, R8
  -> feather: R8 / 255 -> horizontal pass, then vertical pass, over row bands -> R8 (rounded)
```

```text
No project open (editor reader) -> "No project is open to read Mask asset <key>".
Renderer without a reader -> DefaultMaskAssetReader -> "Mask asset <key> cannot be read: this
  renderer has no project mask asset store".
Missing or corrupt row -> the MaskAssetStore errors of SM5.1.
Asset outside the analysis image or size mismatch -> "AI Mask asset <key> does not fit the
  analysis image W x H". Edge outside M8 -> the ValidateAiMaskEdgeAdjustment message.
```

#### 14.1.2 Evidence

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"   # exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AiMaskCoverageTextureTest PipelineMapperTest AiMaskEditSessionTest EditorSessionRenderSchedulerPortTest GpuDagGeometryTest GpuDagCudaMaskTest AlbumBackendLib alcedo_main   # exit 0 (after formatting)
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(AiMaskCoverageTextureTest|PipelineMapperTest|AiMaskEditSessionTest|EditorSessionRenderSchedulerPortTest|GpuDagGeometryTest|GpuDagCudaMaskTest)\."   # exit 0, 133/133 passed
```

| Test (section 8.1) | Target | Result |
| --- | --- | --- |
| `CoverageTextureWithoutEdgeEqualsAssetBytes` | `AiMaskCoverageTextureTest` | PASS |
| `ExpandDilatesByAlternatingCrossAndSquare` | `AiMaskCoverageTextureTest` | PASS |
| `NegativeExpandErodes` | `AiMaskCoverageTextureTest` | PASS |
| `FeatherMatchesIndependentGaussian` | `AiMaskCoverageTextureTest` | PASS |
| `TextureClipsAtImageEdgeAndReplicatesThere` | `AiMaskCoverageTextureTest` | PASS |
| `TextureHoldsAllNonZeroValuesOfTheWholeImageResult` | `AiMaskCoverageTextureTest` | PASS |
| `CoverageTextureRejectsAssetThatDoesNotFit` | `AiMaskCoverageTextureTest` | PASS |
| `DefaultMaskAssetReaderReportsMissingStore` | `AiMaskCoverageTextureTest` | PASS |
| `MaskAssetsReaderReadsStoredAssetAndReportsMissingKey` (plan name `ProjectMaskAssetReaderReadsStoredAsset`) | `PipelineMapperTest` | PASS |
| `CurrentProjectMaskAssetReaderReadsTheOpenProject` | `EditorSessionRenderSchedulerPortTest` | PASS |
| Existing session tests (placement through `AiMaskAssetPixelRect`) | `AiMaskEditSessionTest` | PASS (17/17) |

Suite totals: `AiMaskCoverageTextureTest` 8/8, `PipelineMapperTest` 35/35, `AiMaskEditSessionTest`
17/17, `EditorSessionRenderSchedulerPortTest` 38/38, `GpuDagGeometryTest` 25/25,
`GpuDagCudaMaskTest` 10/10 (the renderer and workspace headers changed). `AlbumBackendLib` and
`alcedo_main` build. The full suite was not run (only the user starts it).

#### 14.1.3 Measurements (section 9)

Release build (`win_release`, only `EditMaskRaster` built; a benchmark program compiled with `cl
/O2` against the release libraries), Windows, OpenCV with TBB and 20 threads, 4096 x 3072
analysis image, an ellipse asset over most of the image, best of 3:

| `feather_px` | `expand_px` | First implementation | Final implementation |
| ---: | ---: | ---: | ---: |
| 50 | 50 | 4240 ms | 1543 ms |
| 50 | 0 | 3605 ms | 1350 ms |
| 0 | -50 | 571 ms | 125 ms |
| 5 | 2 | 279 ms | 79 ms |

The worst case is above 1 s: issue #494.

#### 14.1.4 Implementation decisions

- `AiMaskAssetPixelRect` returns `std::optional<RectI>`; each caller writes its own message, so
  `PlaceAiMaskAssetInCoverage` keeps its SM5.2 error text.
- `mask_asset_reader.cpp` is in `EditMaskRaster`, not in `EditRuntime`: the app reader needs the
  port without the render runtime. `EditRuntime` links `EditMaskRaster` publicly.
- `ProjectMaskAssetReader` accepts a null storage and fails each read; a constructor error would
  break `PipelineMgmtService` construction without storage.
- `CurrentProjectMaskAssetReader` lives in `editor_session_mask_asset_port.{hpp,cpp}` beside the
  SM5.3 editor asset port, which resolves the open project the same way.
- Speed without a result change: Expand runs on the R8 values (a maximum or minimum commutes with
  `v / 255`), and each morphology step and each Gaussian pass runs on row bands with
  `cv::parallel_for_`. A band is a region of the whole matrix, so OpenCV reads the real neighbor
  rows and replicates only at the image edge. The banded results equal the single OpenCV calls
  (benchmark check: maximum difference 0). A single `cv::GaussianBlur` ran on one thread.
- The final conversion uses `convertTo(CV_8U, 255)`, which rounds half to even; the lab uses
  `np.round`, which also rounds half to even.

### 14.2 SM6.2

```text
Phase / date / status: SM6.2 / 2026-10-24 / complete.
Source revision and branch: base 96d2ef25 (SM6.1); branch
  feature/ai-segmentation-sm6-2-raster-mask-evaluate.
Actual changed modules: edit/runtime (MaskCoverageTextureCache, workspace member, compiler),
  CUDA, OpenCL, and Metal mask passes and kernels, tests.
Implemented behavior: R6 to R11; the steps 1 to 6 of section 7.2.
Explicitly unimplemented items: none of SM6.2. The Mask thumbnail from R8 stays an M21 error until
  SM9.
Primary success call chain: section 14.2.1.
Primary failure and restore call chain: section 14.2.1.
Build and test commands with exit codes: section 14.2.2.
Discovered / passed / failed / skipped counts: section 14.2.2.
Manual verification: none; SM9 is the first producer of AI Masks in the product.
Performance or resource evidence, if required: not required (section 9); the texture cost is
  in 14.1.3.
Evidence path: build/tmp/sm6/ on Windows and on the Mac (removed at completion).
Remaining defects or unavailable platforms: #496 (six multi-grade tests fail on main on OpenCL
  and Metal) and #497 (five Metal LLF and Mask Mix tests fail on main). Both fail the same way on
  clean 7eb94016.
```

#### 14.2.1 Call chains

```text
GraphCompiler::Compile -> CompileGradeOwnedMaskStack -> MaskEvaluate for each Mask (AI included)
ExecuteCudaMask / ExecuteOpenClMask / ExecuteMetalMask (compiled AI source)
  -> RequireMaskModel -> AiSegmentationMaskSource
  -> Workspace().MaskCoverage().Acquire(source, Workspace().MaskAssets(), Device(), context)
     key = asset key / analysis size / feather bits / expand
     hit -> entry (last used submission, LRU tick)
     miss -> MaskAssetReader::Read -> MakeAiMaskCoverageTexture -> EvictFor (LRU, not used by
             this frame or an in-flight submission, budget 256 MiB) -> CreateTexture2D(R8)
             -> UploadTexture2D -> entry
  -> MakeRasterMaskSamplingPlan(geometry, texture bounds, texture extent)
  -> RasterMaskSamplesPerAxis -> raster kernel: S x S bilinear samples, 0 outside UV [0, 1],
     mean, invert and opacity of the Mask, R8 -> MaskUnion -> Grade Mix
BasicRenderWorkspace::ReleaseSessionResources -> MaskCoverage().Clear()
```

```text
Reader error (missing, corrupt, no store), texture error, or upload error
  -> no cache entry -> ExecuteXMask throws -> the render fails with the message; the fixture
     cancels the render (BasicRenderDevice::CancelRender), as a renderer does.
```

#### 14.2.2 Evidence

Windows (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaMaskTest GpuDagOpenClGradeTest GpuDagRawInputTest GpuDagModelGraphTest AiMaskCoverageTextureTest AlbumBackendLib alcedo_main   # exit 0 (after formatting)
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(GpuDagCudaMaskTest|GpuDagOpenClGradeTest|GpuDagRawInputTest|GpuDagModelGraphTest|AiMaskCoverageTextureTest)\."   # exit 8, 439/445 passed; the six failures are #496
```

macOS (`macos_debug` with `-DALCEDO_BUILD_TESTS=ON`, `build/macos-debug`, Apple silicon):

```sh
cmake --preset macos_debug -DALCEDO_BUILD_TESTS=ON   # exit 0
cmake --build build/macos-debug --target GpuDagMetalGradeTest AiMaskCoverageTextureTest GpuDagRawInputTest -j 8   # exit 0
ctest -j 1 --output-on-failure -R "^(GpuDagMetalGradeTest|AiMaskCoverageTextureTest|GpuDagRawInputTest)\."   # exit 8, 263/274 passed
```

| Test (section 8.2) | CUDA | OpenCL | Metal |
| --- | --- | --- | --- |
| `AiMaskCoverageMatchesIndependentReferenceAtFullAndPreviewScale` | PASS | PASS | PASS |
| `AiMaskCoverageFollowsCropAndRotation` | PASS | PASS | PASS |
| `AiMaskOutsideTextureBoundsIsZero` | PASS | PASS | PASS |
| `AiMaskInvertAndOpacityApplyBeforeUnion` | PASS | PASS | PASS |
| `AiMaskFeatherAndExpandChangeCoverage` | PASS | PASS | PASS |
| `AiMaskCoverageTextureUploadsOncePerAssetAndEdge` | PASS | PASS | PASS |
| `AiMaskMissingAssetFailsRender` | PASS | PASS | PASS |

The 11 macOS failures are six multi-grade tests (#496) and five Metal LLF and Mask Mix tests
(#497). Clean `main` (`7eb94016`, only `GpuDagMetalGradeTest` rebuilt in the same build directory)
fails the same 11 tests (51/62). On Windows, clean `main` fails the same six OpenCL multi-grade
tests (9/15 of `OpenClMultiGradeFixture`).

`GpuDagGraphCompiler.AiSegmentationMaskCompilesToMaskEvaluate` (`GpuDagRawInputTest`) passes on
Windows and macOS. `AiMaskCoverageTextureTest` passes 8/8 on Windows and macOS. Suite totals on
Windows: `GpuDagCudaMaskTest` 17/17, `GpuDagOpenClGradeTest` 73/79, `GpuDagRawInputTest` 196/196, `GpuDagModelGraphTest` 145/145. `AlbumBackendLib` and `alcedo_main` build. The full
suite was not run (only the user starts it).

#### 14.2.3 Implementation decisions

- The shared checks of section 8.2 live in `ai_mask_raster_test_support.hpp` with the host
  reference of R7 and an in-memory reader. Each backend test file adapts its fixture to
  `AiMaskBackendHarness`, so the three backends run the same assertions. The test analysis image
  is 64 x 48 (the full reference of the test source); render scales 1, 0.5, and 0.25 give 1, 2,
  and 4 samples per axis.
- `RasterMaskSamplesPerAxis` is `clamp(ceil(2^mip_level - 1e-3), 1, 8)`; the allowance keeps an
  exact footprint of N texels at N samples.
- OpenCL reads the four texels with the nearest clamp-to-edge sampler at integer coordinates and
  interpolates in the kernel, as CUDA and Metal do, so no backend uses hardware filtering.
- On Metal the cache upload records a blit before the pass opens its compute encoder.
- The cache keeps its entries in `std::unique_ptr` so a returned entry stays valid while the same
  frame acquires the texture of another AI Mask.
