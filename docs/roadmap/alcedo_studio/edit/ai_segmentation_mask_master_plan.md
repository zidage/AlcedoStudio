# AI Segmentation Mask Master Plan

Date: 2026-10-19. Revised: 2026-10-24.

Status: SM1 complete on 2026-10-22. The EfficientSAM3 quality review failed (section 8.1). The
re-evaluation in the `alcedo-ai-mask-lab` repository accepted the model set SAM 2.1 Hiera tiny, the
people and sky head, and S3OD `s3od-synth` (section 8.2). Alcedo Studio does not use EfficientSAM3
or SAM3. SM3 is split into SM3.A, SM3.B, SM3.C, and SM3.D. SM3.A is complete on 2026-10-22
([phase plan](ai_segmentation_mask/phase_sm3a_segmentation_service_plan.md)); its macOS port check
ran with the SM3.B macOS run. SM3.B is complete on Windows and macOS on 2026-10-22
([phase plan](ai_segmentation_mask/phase_sm3b_embedding_cache_plan.md)). SM4 is complete on Windows on
2026-10-23 ([phase plan](ai_segmentation_mask/phase_sm4_analysis_render_plan.md)). SM3.C is complete
on Windows and macOS on 2026-10-23 ([phase plan](ai_segmentation_mask/phase_sm3c_task_masks_plan.md));
its macOS port check ran with SM3.D. On 2026-10-23 the
user approved the SM5 design (D16 to D22): an AI Mask edit session with its own history stack, R8
assets in the project database, and People masks from S3OD. SM3.D is complete on Windows and macOS
on 2026-10-23 ([phase plan](ai_segmentation_mask/phase_sm3d_people_masks_from_s3od_plan.md)). In SM5
([phase plan](ai_segmentation_mask/phase_sm5_ai_mask_source_and_session_plan.md)), SM5.1, SM5.2,
and SM5.3 are complete on 2026-10-23. SM6 is complete on Windows and macOS on 2026-10-24
([phase plan](ai_segmentation_mask/phase_sm6_raster_mask_evaluate_plan.md)). SM2 runs
after SM9 (section 7). On 2026-10-24 the user requested the new order SM8, SM9, then SM7.
SM8 now includes GPU edge processing, retained candidate display, and one shared viewport composite
([detailed plan](ai_segmentation_mask/phase_sm8_gpu_coverage_and_display_plan.md)).
SM9 and the later SM7 have a [detailed follow-on plan](ai_segmentation_mask/phase_sm9_tool_and_sm7_notice_plan.md).
SM8.1 is complete on 2026-10-24; the later SM8 phases, SM9, and SM7 are planned. The SM6 host
implementation and its completion evidence remain historical facts.

Target release: Alcedo Studio 0.3.4. The AI Segmentation Mask is the main feature of that release.

Parent context:

- [Node-aware Pipeline Editing and Mask Authoring Master Plan](node_mask_editor_master_plan.md)
- [NM7 Viewer Mask Creation and Editing](node_mask_editor/phase_nm7_viewer_mask_creation_plan.md)
- [Brush Mask Architecture, History, and Raster Materialization Master Plan](brush_mask_architecture_master_plan.md)
- [AI Sidecar Backend Integration Plan](../ai/ai_sidecar_backend_plan.md)
- [AI Sidecar Runtime Residency Plan](../ai/ai_sidecar_runtime_residency_plan.md)
- [Semantic Generation and Search Integration Plan](../ai/semantic_generation_search_plan.md)
- [Alcedo AI Mask Lab](https://github.com/zidage/alcedo-ai-mask-lab): the model re-evaluation,
  the accepted model set, the export scripts, and a Python MVP of the AI Mask tool (section 8.2).

Other affected areas: AI sidecar (`rust/puerh_mind`), model download (`app/model_*`), editor
render requests, GPU mask passes on CUDA, OpenCL, and Metal, and the editor QML workspace.

Source revision used for the initial audit: `e1effd87` on `fix/color-wheel-tonal-lgg`.
The SM8 replan audits `b3e1d479f60caa70b47205504348287b30114b47` on `main`.

---

## 1. Decision record

### 1.1 Product goal

The user selects an object in the editor viewport with an AI model. The model makes a Mask that
follows the object edges. The Mask joins the existing Mask system of a Color Grade node. It uses the
existing R8 coverage, Union, and Grade Mix behavior. The interaction must be visually rich, because
this feature is the main item of the 0.3.4 release.

### 1.2 Approved decisions

These decisions come from the user. An implementer must not change them without a new user
decision.

| ID | Decision |
| --- | --- |
| D1 | The 0.3.4 main feature is the AI Segmentation Mask. CFA-domain neural denoise and DAG node blending by luminance or color are not the main feature of this release. |
| D2 | Changed on 2026-10-22 by the user. The model set is SAM 2.1 Hiera tiny (stroke, click, box, add and subtract, and the final masks of Sky and People), the people and sky head on the SAM 2.1 image embedding (Sky and People localization), and S3OD `s3od-synth` (Subject; Background is the inverted Subject). EfficientSAM3 and SAM3 are not used. No model of the set takes text, so text prompts are not part of this plan. Evidence: section 8.2. Changed on 2026-10-26 by D24: Sky uses the fine sky head on the ONNX Runtime backend. |
| D3 | The interaction is visually rich. The exact interaction set is decided in the UI phase. The Python MVP of `alcedo-ai-mask-lab` is a reference for SM9, not a specification: SM9 decides each change to it (D15). |
| D4 | The installer does not contain model weights. Alcedo Studio supplies a built-in downloader and the inference runtime. The weights come from the user-owned Cloudflare R2 bucket or from a Hugging Face repository. |
| D5 | Changed on 2026-10-22 with D2. Each published model directory contains the license text of its models: Apache-2.0 for SAM 2.1, MIT and the DINOv3 License for S3OD. The people and sky head is an own model (training data COCO 2017 and Open Images V7). Model licenses must permit commercial use (lab decision U2). Exception (lab decision U18, 2026-10-25): the sky head is an own model trained on MaSS13K, whose license is not certain; its model directory entry says so, and the Sky function tells the user that the training data may not permit commercial use (#537). |
| D6 | The AI Segmentation Mask reuses the current R8 Mask logic: one R8 coverage per Mask, Union by per-pixel maximum, and Grade Mix. |
| D7 | The model always reads the whole source image. It does not read the image after the document crop and rotation. A later crop or rotation change must not make an existing AI Mask incomplete. |
| D8 | The analysis render disables the document crop in the same way that the Geometry panel does. See the source finding in section 5.3: the current Geometry panel mode keeps the rotation. |
| D9 | A change to Lens correction or to RAW decode parameters can move image content relative to an existing AI Mask. Alcedo Studio does not correct the Mask. The AI Mask UI tells the user that such a change can misalign a Mask. The user deletes the Mask and makes a new one. |
| D10 | This release does not include a color picker. The viewport pointer only supplies the prompt coordinates for the model. The coordinate rule must also be usable by a future color picker. |
| D11 | The first phase is a feasibility and measurement phase. Later design decisions are made as the plan advances. |
| D12 | 2026-10-22. The `alcedo_mind` sidecar runs the models, as it runs the CLIP encoders. Windows uses ONNX Runtime with DirectML. macOS uses compiled native Core ML models (`.mlmodelc`) through the `coreml` crate, as the SigLIP2 encoders do; it does not use the ONNX Runtime Core ML execution provider. The two paths share one service and one set of host rules. ONNX Runtime must be 1.28 or later (section 8.2.3). |
| D13 | 2026-10-22. SM2 (model download) runs after SM9. Until SM2, the sidecar reads the model files from a local model directory that the developer fills. SM3, SM7, SM8, and SM9 do not depend on SM2. |
| D14 | 2026-10-22. SM3 is split by function, not by language: SM3.A is the service architecture (Rust and C++), SM3.B is the image embedding cache and the first optimizations, and SM3.C adds the task masks (Subject, Background, Sky, People) to the service. |
| D15 | 2026-10-22. The lab decisions U1 (interaction not worse than Lightroom and Capture One), U3 (no edge post-processing: the edge comes from the model output), U6 (model memory budget 3 GB on Windows, 3.5 GB on macOS; models unload when the user goes back to adjusting), U7 (no interaction that follows the pointer), and U8 (float16 models are acceptable) apply to this plan. The lab MVP UI has known defects; SM9 replans the interaction instead of porting the MVP UI as it is. |
| D16 | 2026-10-23. Two-level history. An AI Mask edit session keeps its own in-memory history stack: strokes, add and subtract points, boxes, task selections, a change of the selection mode, Feather, and Expand are session steps. Undo and Redo go to the session stack while the session is open. The live document does not change during the session. Commit publishes one `AddMask` or `ReplaceMaskSource` history operation. Cancel publishes nothing. The main history Undo and Redo do not change. |
| D17 | 2026-10-23. Only Commit writes R8 bytes: one content-addressed `MaskAsset` per Commit, in a table of the project database. The asset is the composed model coverage before Feather and Expand, over its bounding rectangle (P3). A session step writes nothing. A reachability cleanup with the commit cleanup removes the assets that no stored commit, Version, root or checkpoint document, or open editor journal refers to. |
| D18 | 2026-10-23. Paste to another image does not copy AI Masks. The paste result names the Masks that it skipped. An AI Mask describes the content of its own image. |
| D19 | 2026-10-23. The format change is additive: one new project table and one new Mask source kind. The project metadata version and the Pipeline document JSON version do not change, so older projects open without change. An older build reports the real "unknown Mask source kind" error for an image with an AI Mask. |
| D20 | 2026-10-23. Feather and Expand are fields of the AI Mask source. They apply when the runtime makes the coverage texture (SM6). The stored R8 does not contain them, so a Feather or Expand change writes no asset. |
| D21 | 2026-10-23. People uses S3OD. The People task starts with the full-image S3OD alpha (the Subject result), because S3OD usually selects all people of a group. The person boxes of the head show at the same time. A click on a box adds or removes one person: the person alpha comes from S3OD on the crop of the person box plus a margin. The session applies the clicks as ordered Add and Subtract steps, with no special rule for overlapping neighbors. People does not use SAM 2.1 box prompts. Sky does not change. (SM3.D) |
| D22 | 2026-10-23. S3OD stays loaded from model load to unload, because Subject, Background, and People all use it. This closes the S3OD residency item of section 8.2.6. |
| D23 | 2026-10-24 ([#522](https://github.com/zidage/AlcedoStudio/issues/522)). The host upsamples an S3OD alpha grid to the session image with a joint bilateral filter guided by the analysis image. The S3OD alpha is almost binary, and a bilinear upscale of about 4x kept the grid steps; a smoothing of the grid alone did not remove the long steps of near-vertical and near-horizontal edges. A pixel near an edge takes the mean of its 4 x 4 nearest cells, weighted by the distance (sigma 1 cell) and by the color difference to the cell in the guide (sigma 0.7 times the color standard deviation of the window, so low-contrast edges also separate). The guide is the analysis image smoothed by 1 pixel. The result is steepened around 0.5 by half the upscale factor. The guide moves the edge by at most about 1 grid cell, inside the grid uncertainty; this is not the edge refinement that U3 removed. Release cost on a 2729 x 4096 analysis image: about 20 ms per conversion, plus about 50 ms once per session for the guide. Changed on 2026-10-26 by SM3.E decision S9 (#537): the Sky alpha of the sky head uses the same upsampling with a gain of 1, so its partial alpha at fine structures stays. |
| D24 | 2026-10-26 ([#536](https://github.com/zidage/AlcedoStudio/issues/536), [#537](https://github.com/zidage/AlcedoStudio/issues/537), [#538](https://github.com/zidage/AlcedoStudio/issues/538)). Sky uses the fine sky head of the lab (lab issue 10, U17, U18) on every backend: `sky_head.onnx` (ONNX Runtime) or `sky_head.mlmodelc` (Core ML, float32, since #538) reads the three encoder outputs and the encoder input and gives a 1024 x 1024 sky alpha of the stretched image. Sky is found when at least 0.05 % of the alpha values are >= 0.5. Sky returns this alpha with the whole-image box. On the MaSS13K test split the head has IoU 0.875 against 0.642 of the MVP Sky and keeps the sky between branches. The MVP Sky (people and sky head regions, SAM 2.1 decoder, R8 union) and its `BINARY` result are removed (#538). Plan: [SM3.E](ai_segmentation_mask/phase_sm3e_fine_sky_head_plan.md). |

### 1.3 Proposed decisions that need user approval

These items came from the planning discussion. They are not approved. Each item names the phase that
must close it.

| ID | Proposal | Closed by |
| --- | --- | --- |
| P1 | Closed by D12 (2026-10-22): the sidecar runs inference; the C++ process has no ONNX Runtime. | SM1, closed |
| P2 | Approved 2026-10-23: store prompts and Mask coverage in normalized ReferenceSpace. | SM5, closed |
| P3 | Approved 2026-10-23: store the R8 coverage only over the bounding rectangle of the Mask content, plus a margin. The maximum axis is `kMaximumRasterMaskAxis` (4096). | SM5, closed |
| P4 | Closed by U3 (D15): no edge refinement. D17 and D20 place the stored model coverage before Feather and Expand. SM7 only shows the misalignment notice. | SM1 and SM5, closed |
| P5 | Replaced by the 2026-10-24 design in section 1.5: candidate and committed coverage use one viewport composite. QSG keeps input controls only. | SM8, closed in design |
| P6 | Approved 2026-10-23: store a geometry fingerprint with each AI Mask. Mark only the Masks whose fingerprint differs from the current Lens and RAW decode geometry. | SM5 (field), SM7 (notice), closed |
| P7 | Use one persistent gRPC channel per segmentation session. SM3.A implements the channel (Ping evidence in section 8.1). The second part ("keep only the newest pending decode request while the pointer moves") is dropped: U7 removed decoding during pointer movement, and the tool decodes once per stroke, click, or box. | SM3.A |
| P8 | Separate scope proposal: Luminance Range and Color Range Masks remain excluded. They do not block SM8 and are not added by this replan. | Separate user request if scope changes |

### 1.4 Rejected options

- Do not encode only the cropped and rotated image (rejected by D7).
- Do not set `reference_bounds` to the document crop rectangle (rejected by D7).
- Do not move crop and rotation to a post-processing stage of the pipeline. The current mask passes
  already follow geometry through `render_to_reference` (section 5.2).
- Do not export a full image and read one pixel from it to implement a future color picker. A
  future picker reads a small region through a pipeline readback request.
- Do not bundle model weights in the installer (D4).

### 1.5 GPU coverage and display replan (2026-10-24)

The user requested an earlier SM8, a later SM7, shared composition, and preservation of old Masks
during new model requests. The following implementation design serves those requirements.
It does not change D16-D20 or the stored project format.

| Item | Selected design and reason |
| --- | --- |
| Order | Complete SM8, then SM9, then SM7, then SM2 and SM10. Misalignment notices use the completed tool surface. |
| History | Keep the existing session recipes, parameters, stored coverage, and native model-output cache. Undo/Redo reconstruct current coverage without inference. No GPU history is added. |
| Source lifetime | Keep original R8 separate from adjusted GPU coverage. A shared immutable read of the current algorithm output stays alive until upload finishes. Do not retain a mutable span across a queue. |
| GPU owner | Extend the existing workspace coverage owner. Use the current backend, render executor, submission tracking, and resource release. A coverage-only operation does not execute the photo DAG. |
| New model request | Keep the previous displayed result and the opened stored Mask. Accept valid model output through the session owner. Publish its GPU replacement only after completion. Release old display resources after readers finish. |
| GPU failure | Retain the desired session state and report the real error. Show the previous result as previous, with Commit disabled until Retry displays the desired state. Retry uses retained outputs. |
| Processing | Use the same GPU Expand/Feather operation for candidates, committed Masks, thumbnails, and export. Preserve the full analysis grid and existing equations. The host function remains test evidence, not a runtime substitute. |
| Composite | Add coverage to the existing viewport draw after photo base/detail selection. Candidate, committed single Mask, and Grade Union use this one composite. No second photo-DAG or candidate-only composite exists. |
| Transport | Reuse current GPU presentation adapters and RGBA32F in a dedicated coverage channel. Store coverage in red. Do not overwrite a photo/detail slot or read coverage back through the CPU. |
| Presentation state | Visibility, tint, and preview selection do not change document history, photo cache identity, export pixels, or scope inputs. |
| Controls | QSG retains points, boxes, strokes, and existing controls above the coverage. SM9 adds the dialog actions without a second renderer or temporary stack. |

See the SM8 plan for exact operations, error chains, resource lifetimes, and measurements.
See the SM9/SM7 plan for control behavior and the later notice task.
Tracking: [#499](https://github.com/zidage/AlcedoStudio/issues/499).
GPU processing/performance: #494. Display transport: #500. Shared composite: #501. Session integration: #502.

---

## 2. Terms

| Term | Meaning |
| --- | --- |
| ReferenceSpace | The uncropped and unrotated source image after the sensor active area. Normalized ReferenceSpace is `[0, 1]` on each axis of `full_reference_extent`. |
| Analysis render | A pipeline render that makes an 8-bit RGB image for the model. It does not apply the document crop or rotation. |
| Image embedding | The output of the SAM 2.1 image encoder for one analysis render: `image_embed`, `high_res_feats_0`, and `high_res_feats_1`. |
| Prompt | One user input to the SAM 2.1 decoder: a click (one point), a stroke (points sampled along the pointer path), or a box. Each point adds (label 1) or subtracts (label 0). |
| Task mask | A mask of a one-click task: Subject, Background, Sky, or People. |
| Model set | The accepted models of section 8.2 in the platform form: ONNX files on Windows, compiled Core ML models on macOS. |
| Candidate Mask | The current session coverage composed from retained model outputs, before Commit. It is not part of the document. Its original R8 and adjusted GPU display are separate resources. |
| Committed Mask | A Mask that the document owns. It has a stored R8 coverage. |
| Segmentation session | The sidecar state for one analysis render: the loaded models and one image embedding. |

---

## 3. Product design specification

This section states the required behavior. The UI phase adds the detailed visual design after D3 is
refined.

### 3.1 Creation flow

1. The user selects a Color Grade node.
2. The user selects the AI Mask tool.
3. If the model is not installed, the tool shows a download panel. The panel shows the model name,
   the download size, and the license name. The user starts the download from that panel.
4. When the model is installed, Alcedo Studio starts a segmentation session for the current image.
   The viewport shows a busy state until the image embedding exists.
5. The user adds prompts and task masks. Each change is one step of the AI Mask edit session (D16)
   and produces a new Candidate Mask. The viewport shows the Candidate Mask over the photo. Undo
   and Redo move through the session steps.
6. The user commits the Candidate Mask. Alcedo Studio stores the composed R8 coverage as one
   project asset (D17) and adds one Mask, or replaces the source of the edited Mask, in one history
   operation. Feather and Expand are fields of the Mask (D20). There is no edge refinement stage
   (D15, U3).
7. The user can cancel. Cancel leaves the document unchanged.

During each new model request, the previous candidate stays visible.
Earlier model outputs and the opened Mask stay available to the temporary stack.
Feather and Expand preview through SM8 without a photo render.
An accepted slider edit creates one temporary step; intermediate drag values create display requests only.
Candidate and committed coverage use the same display composite.

### 3.2 Coordinate behavior

- A pointer position maps to normalized ReferenceSpace through the current viewport geometry.
- The user can add prompts only inside the visible photo. The resulting Mask can extend outside the
  current crop.
- A later crop, rotation, zoom, or region-of-interest render shows the same Mask on the same image
  content.

### 3.3 Failure behavior

| Failure | Required result |
| --- | --- |
| The model is not installed. | The tool shows the download panel. It does not start a session. |
| The download fails or a file checksum is wrong. | The download panel shows the real error. The installed model state does not change. |
| The sidecar does not start. | The tool shows the start error. The document does not change. |
| The model load or inference fails. | The tool shows the runtime error. It keeps the current prompts. It does not select another model, device, or resolution. |
| The user leaves the image during a session. | The session ends. The document does not change. |
| A stored AI Mask object is missing or corrupt. | Rendering reports the real error. It does not render an empty Mask or rebuild the Mask through another path. |

### 3.4 Exclusions

- A color picker is excluded from this release (D10).
- CFA-domain neural denoise is excluded from this plan.
- Blend modes and blending between DAG nodes are excluded from this plan.
- Luminance Range and Color Range Masks are excluded until the user closes P8.
- Automatic Mask correction after a Lens or RAW decode change is excluded (D9).
- Paste of an AI Mask to another image is excluded (D18).

---

## 4. Scope by module

| Module | Responsibility in this plan |
| --- | --- |
| `alcedo-ai-mask-lab` repository | Export the model set to ONNX and Core ML. Check numeric agreement with PyTorch. The export scripts stay in that repository. |
| `rust/puerh_mind` | Load the model set: ONNX Runtime sessions on Windows, native Core ML models on macOS. Keep segmentation sessions. Serve encode, decode, and task mask requests. |
| `rust/puerh_mind/proto` | Define the segmentation RPC messages. |
| `alcedo_studio/src/sidecar_client`, `app/ai_sidecar_runtime_service` | Call the segmentation RPCs with a persistent channel. |
| `app/model_asset_catalog`, `app/model_download_service`, Rust `model_assets.rs` | Download and verify the segmentation model files. |
| `edit/geometry`, `renderer/` | Make the analysis render in ReferenceSpace. |
| `edit/mask` | Own the AI Mask source model, validation, serialization, and the R8 asset. |
| `edit/runtime/{cuda,opencl,metal}` | Process original R8 with GPU Expand/Feather, sample raster Masks, and prepare coverage display targets. |
| `ui/editor_rhi` | Own one viewport coverage composite and existing native-resource display lifetimes. |
| `app/ai_mask_edit_session`, app tool controller, `ui/alcedo_main` | Keep temporary history and model results; expose tool actions and existing viewport controls. |

Related plans:

- This plan extends the Mask model of the Node-aware Mask master plan.
- This plan shares the raster Mask storage and the three-backend raster sampling with the Brush Mask
  master plan. SM5 and SM6 must decide which parts this plan builds first. The Brush plan keeps its
  tiled copy-on-write storage design.
- This plan extends the AI sidecar with a local inference task. It does not change remote provider
  behavior.

---

## 5. Initial source audit and current replan

Sections 5.1-5.3 preserve the initial pre-SM5 audit. They are not a current implementation inventory.
SM3-SM6 completion records describe the landed changes.
The current SM8 audit is in [SM8 section 2](ai_segmentation_mask/phase_sm8_gpu_coverage_and_display_plan.md#2-current-source-audit).

### 5.1 Mask model and execution

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Mask source | `include/edit/mask/mask_model.hpp` | `MaskSource` is a variant of `RadialMaskSource` and `LinearGradientMaskSource`. `BrushMaskSource` exists only with `ALCEDO_ENABLE_BRUSH_MASK`. Analytic parameters are in normalized ReferenceSpace. | Add an AI Mask source variant (SM5). |
| Range fields | `mask_model.hpp:104-141` | `ColorRangeModel` and `LuminanceRangeModel` hold only `enabled = false`. The loader rejects other values. | No change unless the user closes P8. |
| Raster asset | `include/edit/mask/mask_asset.hpp` | `MaskAsset` holds packed R8 pixels, an extent, and `reference_bounds`. `MakeMaskAssetKey` hashes canonical bytes with 128-bit xxHash. No production code stores or uploads a `MaskAsset`. | Add persistence (SM5) and GPU upload (SM6). |
| Mask compile | `edit/runtime/graph_compiler.cpp:266-311` | `CompileGradeOwnedMaskStack` adds one `MaskEvaluate` pass per Mask and one `MaskUnion` pass. Each `MaskEvaluate` already receives a `range` input from the Grade scene input. Brush throws "Brush mask rasterization is not available in this build". | Compile the AI Mask source to a raster `MaskEvaluate` (SM6). |
| Native evaluation | `edit/runtime/cuda/cuda_mask_pass.cu:40-120`, `opencl/opencl_mask_pass.cpp`, `metal/metal_mask_pass.mm` | `AnalyticMaskKernel` maps each render pixel to ReferenceSpace with `render_to_reference` and evaluates the analytic formula. No backend samples a raster. | Add a raster sampling path on all three backends (SM6). |
| Raster sampling plan | `include/edit/geometry/texture_sampling_plan.hpp` | `MakeRasterMaskSamplingPlan(geometry, reference_bounds, texture_extent)` returns `render_to_texture_uv`. Production masks do not use it yet. | Use it for the AI Mask (SM6). |
| Mask thumbnail | `app/mask_thumbnail_evaluate.cpp:69-130` | A CPU loop draws analytic Masks at 128 x 128. It ignores image content. | Draw the AI Mask from its R8 asset (SM9). |

### 5.2 Geometry and coordinates

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Pointer mapping | `ui/edit_viewer/mask_edit_geometry.cpp:284-318` | `MaskEditGeometry::MapItemToReference` maps an item point to ReferenceSpace pixels and normalized ReferenceSpace. It handles a region-of-interest frame. | SM4 maps its result to prompt coordinates of the analysis image (SM4 plan S6 to S8); SM9 calls it. |
| Frame geometry | `include/edit/geometry/resolved_render_geometry.hpp:32-50` | `ResolvedRenderGeometry` carries the decoded, reference, edit, and render transforms. | No change. |
| Pass order | `graph_compiler.cpp:417-508` | Develop runs Lens, then `GeometryResample`, then color conversion. Mask passes run per Color Grade at `render_extent`. | No change. Crop and rotation stay upstream. |

### 5.3 Analysis render

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Document geometry use | `include/edit/geometry/render_request.hpp`, `graph_compiler.cpp:72-87` | `DocumentGeometryUse` has `ApplyCropAndRotation` and `RotatedUncroppedSource`. `RotatedUncroppedSource` keeps the document rotation and frames the bounding box of the rotated source. Only the Geometry panel uses it. | SM4 adds `ReferenceSource` (no crop, no rotation). |
| Host readback | `editor_image_render_port.hpp`, `EditorSessionRenderSchedulerPort::ScheduleImages` | The editor image render port renders documents of the held image to host RGBA32F on the editor executor. The comparison and `render.preview` use it. | SM4 renders the analysis document through it. |

### 5.4 Sidecar and model download

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| ONNX Runtime | `rust/puerh_mind/Cargo.toml`, `src/service/ort_runtime.rs` | `ort` 2.0.0-rc.12 (ONNX Runtime 1.24) with DirectML and CoreML. The `auto` device lists DirectML then CPU on Windows, and CoreML then CPU on macOS. CUDA is rejected. The C++ process has no ONNX Runtime. | SM3.A raises `ort` to 2.0.0-rc.13 (ONNX Runtime 1.28) because of microsoft/onnxruntime#29071 (section 8.2.3). The segmentation sessions do not use the CLIP execution provider list with its CPU entry. |
| Session lifetime | `src/service/ort_clip.rs`, `coreml_clip.rs`, `encoder_residency.rs` | Encoders load on first use and unload after an idle time. One mutex serializes one encoder. | Add segmentation residency with an explicit unload (SM3.A). |
| Transport | `sidecar_client/client.cpp:57-58` | Plain gRPC over loopback TCP. Each RPC makes a new channel and stub. The client uses the default 4 MB receive limit. | Use a persistent channel for segmentation (SM3). |
| Model catalog | `include/app/model_asset_catalog.hpp`, Rust `model_assets.rs` | The catalog lists semantic (CLIP) profiles only. `BuildAssetUrl` makes `{endpoint}/{repo_id}/resolve/{revision}/{path}`. Assets have size and an optional SHA-256. | Add a segmentation profile kind and a per-profile download location (SM2, after SM9). |
| Download | `app/model_download_service.cpp` | aria2c downloads a profile into a staging directory, verifies it, and promotes it. `StartDownload` accepts only semantic profiles. | Accept segmentation profiles (SM2). |

### 5.5 Project format

`include/edit/history/pipeline_history_format.hpp` states that loaders accept only the current
format identities. The current project metadata version is `0.10.0`, and the Pipeline document JSON
version is 7. D19 closes the format change: one additive project table and one new Mask source
kind, with no version change. Older projects open without change.

### 5.6 External model facts

The facts of the first audit (EfficientSAM3 and `facebook/sam3` on Hugging Face) are history; the
SM1 history keeps them (section 8.1). The accepted model set, its files, licenses, and
measurements are in section 8.2.

---

## 6. Target architecture (initial)

This section shows the intended owners. Each later phase confirms or changes its part before
implementation.

```text
Viewport pointer (QML)
  -> EditorMaskCreationAdapter
  -> MaskEditGeometry::MapItemToReference          normalized ReferenceSpace
  -> AI Mask tool controller (app layer)
       ├─ open: sidecar LoadSegmentationModels (SAM 2.1 tiny; SM3.C adds the head and S3OD)
       ├─ start: analysis render (ReferenceSpace, no crop, no rotation)
       │    -> sidecar StartSegmentationSession(image) -> image embedding stays in the sidecar
       ├─ stroke, click, or box: sidecar DecodeSegmentationPrompt(session, prompts, mask_input)
       │    -> Candidate Mask logits -> QSG presentation over the photo
       ├─ Subject, Background, Sky (SM3.C), People (SM3.D): task mask request -> Candidate Mask
       ├─ session steps: in-memory session history stack, host composition (D16)
       └─ commit: MaskAsset (R8, reference_bounds) -> project mask asset table (D17)
            -> AddMask or ReplaceMaskSource -> one history operation
  -> close (back to adjusting): sidecar UnloadSegmentationModels (U6)
Sidecar model execution
  Windows: ONNX Runtime 1.28+, DirectML    macOS: native Core ML (.mlmodelc), compute units per model
Render
  -> CompileGradeOwnedMaskStack -> raster MaskEvaluate (CUDA / OpenCL / Metal)
  -> MaskUnion -> Grade Mix
```

The sidecar owns the model sessions and the image embedding. The document owns the committed Mask.
The project storage owns the R8 bytes. No owner keeps a copy of another owner's state.

---

## 7. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| SM1 | Model selection, export, runtime feasibility, latency, and quality evidence | `alcedo-ai-mask-lab` | none | 900-1500 | complete 2026-10-22 (section 8) |
| SM3.A | Segmentation service architecture: proto, Rust service with the Windows ONNX Runtime and macOS Core ML adapters, SAM 2.1 encode and decode, C++ client with a persistent channel, app session | proto, Rust service, `sidecar_client`, app | SM1 | 2400-2950 estimated, in two parts SM3.A.1 and SM3.A.2; actual about 4,500 | complete 2026-10-22; macOS port check passed with the SM3.B macOS run ([phase plan](ai_segmentation_mask/phase_sm3a_segmentation_service_plan.md)) |
| SM3.B | Image embedding cache and first optimizations | Rust service, app session | SM3.A | 800-1400; actual about 1,800 code lines | complete 2026-10-22 on Windows and macOS ([phase plan](ai_segmentation_mask/phase_sm3b_embedding_cache_plan.md)) |
| SM3.C | Task masks in the service: Subject (S3OD), Background, Sky and People (head) | Rust service, proto, C++ client | SM3.A | 1600-2050 in two parts SM3.C.1 and SM3.C.2; actual about 3,250 code lines | complete 2026-10-23 on Windows and macOS (macOS port check with SM3.D) ([phase plan](ai_segmentation_mask/phase_sm3c_task_masks_plan.md)) |
| SM4 | Analysis render in ReferenceSpace, image key, and prompt coordinates | geometry, editor session, app, viewer geometry | none | 1600 in two parts SM4.1 and SM4.2; actual about 1,350 | complete 2026-10-23 on Windows; measurements with SM9 ([phase plan](ai_segmentation_mask/phase_sm4_analysis_render_plan.md)) |
| SM3.D | People masks from S3OD on person crops (D21, D22) | lab pipeline, Rust service, proto, C++ client | SM3.C | 1,300-1,750 in two parts SM3.D.1 and SM3.D.2, plus 300-450 in the lab; actual about 2,230 lines, plus 820 in the lab without the result JSON | complete 2026-10-23 on Windows and macOS ([phase plan](ai_segmentation_mask/phase_sm3d_people_masks_from_s3od_plan.md)) |
| SM3.E | Sky from the fine sky head (D24): Rust service and port check (#536); host coverage and notice (#537); Core ML (#538) | Rust service, proto comments, port reference; C++ app and QML (#537) | SM3.C | Rust part 900-1,300 | Rust part (#536) and host part (#537) complete 2026-10-26 on Windows; Core ML and the removal of the MVP Sky (#538) complete 2026-10-26 on macOS and Windows ([phase plan](ai_segmentation_mask/phase_sm3e_fine_sky_head_plan.md)) |
| SM5.1 | AI Mask source kind, project mask asset table, read verification, reachability cleanup | `edit/mask`, storage, history, app | SM4 | 1,500-1,900 | complete ([phase plan](ai_segmentation_mask/phase_sm5_ai_mask_source_and_session_plan.md)) |
| SM5.2 | AI Mask edit session: session history stack, composition, People toggles, edit of a stored AI Mask, Commit payload | app layer, segmentation service, proto | SM5.1, SM3.D | 1,300-1,700 | complete (same plan) |
| SM5.3 | Editor integration: Open, Commit, Cancel, asset write; Paste skip and notice | editor session, Mask controller, transfer, album backend | SM5.1, SM5.2 | 1,000-1,400 | complete (same plan) |
| SM6 | Raster `MaskEvaluate` on CUDA, OpenCL, and Metal; Feather and Expand at texture creation (D20) | `edit/mask`, `edit/runtime`, three backends, app | SM5.1 | 2,000-2,900 in two parts SM6.1 and SM6.2 | complete 2026-10-24 on Windows and macOS ([phase plan](ai_segmentation_mask/phase_sm6_raster_mask_evaluate_plan.md)) |
| SM8.1 | Stable coverage reads and workspace operation boundary | session, GPU workspace | SM5, SM6 | 900-1500 | complete 2026-10-24 on Windows and macOS ([SM8 plan](ai_segmentation_mask/phase_sm8_gpu_coverage_and_display_plan.md#711-sm81-completion-record)) |
| SM8.2 | CUDA Expand/Feather and committed-render reuse | CUDA runtime | SM8.1 | 1200-1900 | planned |
| SM8.3 | OpenCL Expand/Feather and committed-render reuse | OpenCL runtime and registry | SM8.2 equations | 1000-1800 | planned |
| SM8.4 | Metal Expand/Feather and committed-render reuse | Metal runtime | SM8.2 equations | 1000-1800 | planned |
| SM8.5 | Independent GPU coverage presentation channel | native adapters, presentation queue | SM8.1 | 1200-1900 | planned |
| SM8.6 | One viewport composite for all coverage sources | viewport shader, theme | SM8.2-SM8.5 | 1000-1700 | planned |
| SM8.7 | Candidate replacement and temporary history integration | app/session facade | SM8.6 | 1100-1800 | planned |
| SM8.8 | Pixel, memory, and request-to-present evidence | targeted qualification | SM8.7 | 500-1000 | planned |
| SM9.1 | AI tool controller, dialog model, and lifecycle | app, QML | SM8 | 1100-1800 | planned ([SM9/SM7 plan](ai_segmentation_mask/phase_sm9_tool_and_sm7_notice_plan.md)) |
| SM9.2 | Prompt/task/People controls and input markers | controller, existing overlay | SM9.1 | 1200-1900 | planned |
| SM9.3 | Edge editing, local history, Commit/Cancel, thumbnails | controller, QML, thumbnail owner | SM9.2 | 1100-1900 | planned |
| SM7 | Per-Mask misalignment notice from the geometry fingerprint (D9, P6) | app, existing tool surface | SM9.3 | 400-800 | planned, after SM9 |
| SM2 | Segmentation model download from R2 or Hugging Face | model catalog, download service, settings | SM9, SM7 in execution order | 800-1400 | outline (after the tool and notice) |
| SM10 | Product qualification | all | SM1-SM9, SM2 | 300-800 | outline |

Order of work: SM3.A, SM3.B, SM3.C, SM4, SM5.1, SM3.D, SM5.2, SM5.3, SM6, SM8, SM9, SM7, SM2, SM10. SM5.1
does not depend on SM3 and can start before SM3.D. Until SM2, the developer fills the local model directory by
hand (D13); the phase plans state the directory and the files.

"Outline" means that the phase has a scope and an estimate only. Each outline phase receives a
detailed phase plan with the alcedo-create-plan skill before it starts. That plan records the user
decisions from section 1.3 that the phase closes.

---

## 8. Phase SM1 — Model selection and runtime feasibility (complete 2026-10-22)

### 8.1 History

SM1 started on 2026-10-19 with EfficientSAM3 and continued with a model re-evaluation. The export
scripts, the runtime measurement program, and the step reports of this work are in the
[`alcedo-ai-mask-lab`](https://github.com/zidage/alcedo-ai-mask-lab) repository. Alcedo Studio does not keep
them. Issues: #448 (SM1), #449 (quality review), #458 (re-evaluation), all closed.

- **EfficientSAM3 proof of concept** (2026-10-19, lab
  [`docs/background/sm1_efficientsam3_record.md`](https://github.com/zidage/alcedo-ai-mask-lab/blob/main/docs/background/sm1_efficientsam3_record.md)):
  the checkpoints export to ONNX and run in the `alcedo_mind` runtime on DirectML and Core ML. The
  full checkpoints contain SAM3 components; only the image encoder trunk is an EfficientSAM3
  student.
- **Quality review failed** (lab
  [`docs/background/model_re_evaluation.md`](https://github.com/zidage/alcedo-ai-mask-lab/blob/main/docs/background/model_re_evaluation.md)): the
  point and box path makes regular grid holes on large regions and fails on close-up portraits.
  The cause is the student image encoder: its native 32 x 32 output is resized to 72 x 72, and the
  decoder is never trained on student features. The ONNX graphs match PyTorch, so the export is
  not the cause.
- **gRPC round trip** (Rust tonic client, 1000 `HealthService.Ping` calls, Windows loopback):
  persistent channel p50 0.20, p95 0.32, maximum 0.60 ms; new channel per call p50 0.52, p95 13.4,
  maximum 26.6 ms. This is the evidence for P7.
- **Re-evaluation steps E1 to E5** (2026-10-19 to 2026-10-20, lab
  [`docs/exploration/`](https://github.com/zidage/alcedo-ai-mask-lab/blob/main/docs/exploration/README.md)): SAM 2.1 removes the grid holes in all
  sizes (E1); BiRefNet is too slow for the Subject task (E2); a short stroke of 3 to 4 points
  selects like a click, and add and subtract strokes work with the previous mask as `mask_input`
  (E3); a live point decoder reaches the intended mask more often than precomputed candidates
  (E4); an edge stage helps only with a second encoder pass (E5). The lab then accepted the model
  set of section 8.2 and decided U3: no edge post-processing.

### 8.2 Accepted model set (2026-10-22)

**Status:** SM1 complete. The re-evaluation continued in the
[`alcedo-ai-mask-lab`](https://github.com/zidage/alcedo-ai-mask-lab) repository (lab commit
`2a33061`). The lab README section "Final selection and MVP architecture", the acceptance record
(`docs/exploration/acceptance_ai_mask_model_set.md`), the MVP report
(`docs/exploration/mvp_ai_mask.md`), and the macOS Core ML report
(`docs/exploration/macos_coreml_verification.md`) are the evidence. This section copies the facts
that later phases need. The lab is the reference for the numbers.

#### 8.2.1 Model set

| Function | Model | Windows file (ONNX Runtime, DirectML) | macOS file (native Core ML) | License |
| --- | --- | --- | --- | --- |
| Stroke, click, box, add and subtract; final masks of Sky and People | SAM 2.1 Hiera tiny image encoder | `point_encoder.onnx`, float32, 109,381,695 bytes, SHA-256 `17a554b420d3c51a109567f784ff3792b354c78dc8c2c09c585403329b412d61` | `point_encoder.mlmodelc`, float32, compute units ALL, 134,260,018 bytes, directory SHA-256 `d3f66d7bcc2a81b2245f22e2ff073b3b1bd180720ea49c091bb0ea401e420285` | Apache-2.0 |
| as above | SAM 2.1 tiny prompt encoder and mask decoder | `point_decoder.onnx`, float32, 21,188,206 bytes, SHA-256 `6f9a0ef2c217a0abccc8b0c01e631c42c6a78a2038a7944cc7fcb4f536439278` | `point_decoder.mlmodelc`, float32, compute units CPU_ONLY, 1 to 64 points, 21,098,274 bytes, directory SHA-256 `f2efed0420fba10b641afa384a58a872cc878231472711e9d86219bf4440fd70` | Apache-2.0 |
| Sky (D24) | Sky head, lab issue 10 run 1, epoch 15 (`SkyAlphaHead`, 1,435,523 parameters) | `sky_head.onnx`, float32, opset 17, 5,821,904 bytes, SHA-256 `a6c555e7dfdbb236d6fa11569b546eab0da9f3945f6462872de0866f675a4788` | `sky_head.mlmodelc` (#538), float32, compute units ALL, 5,860,515 bytes, directory SHA-256 `f15ed22c46edeb5f7863c861717f95d30a52b3e089fde7f99b98733e56f888b9`; `model.mil` SHA-256 `8a09045be0114277df36032400cc055571a0b84da004ec2aba2c5beb7485bce0`, `weights/weight.bin` SHA-256 `d15e41f754acff09d43395d5010f53c2a15e4a9c72990c155c6cc8b42557da4d` | Own model; training data MaSS13K, license not certain (D5 exception, lab U18) |
| Sky and People localization | People and sky head, P1 run 2, epoch 15 | `people_sky_head.onnx`, 1,119,131 bytes, SHA-256 `e02f2a1eaace94e0efe2b3c8db0a0391be210b477a95e49063e80758f12abf35`; thresholds `people_sky_head.json`, SHA-256 `43981e9ed1336ee97bcd3b7a7f07318523a56d43a24631c981d29d27c4fb2486` | `people_sky_head.mlmodelc`, compute units ALL, 1,138,418 bytes, directory SHA-256 `ccb5c48d19d8785ca9fc485d01b3b2ef94989de480fb2eba6c9471b0728540c9` | Own model (COCO 2017, Open Images V7) |
| Subject; Background = 1 - Subject | S3OD `s3od-synth` | `subject_masks.onnx`, float16 with the encoder layer 0 attention in float32, attention in 8 chained query chunks, 219,301,899 bytes, SHA-256 `0f070b8641676d68faaafc0b70816be1458fcc976d8d6d119e67038527cbc288` | `s3od.mlmodelc` (`float16_embeddings_attention_float32`), compute units ALL, 271,166,829 bytes, directory SHA-256 `2c6a24e61ca5dc062ed80a5163974e0e58500fd94fc02b0e078eba7205c0424d` | MIT; DINOv3 License for the encoder |

The directory SHA-256 of a `.mlmodelc` is the lab rule: SHA-256 over the sorted relative paths and
the file hashes (`convert_models.directory_sha256`). SM2 decides the published packaging of the
Core ML directories. The directory SHA-256 identifies one compiled copy only: `coremldata.bin` and
`analytics/coremldata.bin` change at every compile, while `model.mil` and `weights/weight.bin`
stay equal (SM3.E plan, section 11.1). SM2 pins the published files.

Model input and output names (the Core ML models use the ONNX input names; the decoder output names
differ):

| Model | Inputs | Outputs |
| --- | --- | --- |
| Encoder | `pixel_values` [1, 3, 1024, 1024]: the photo stretched to 1024 x 1024 (bilinear with antialias, no padding), ImageNet mean and standard deviation | `image_embed` [1, 256, 64, 64], `high_res_feats_0` [1, 32, 256, 256], `high_res_feats_1` [1, 64, 128, 128] |
| Decoder | the three encoder outputs, `point_coords` [1, N, 2] (normalized photo coordinates x 1024 on each axis), `point_labels` [1, N] (1 add, 0 subtract, 2 and 3 box corners, box first), `mask_input` [1, 1, 256, 256], `has_mask_input` [1] | ONNX: `mask_logits` [1, 4, 256, 256], `iou_scores` [1, 4], `object_score_logits` [1, 1]. Core ML: `masks`, `iou_predictions`, `object_score_logits` with the same shapes |

#### 8.2.2 Decisions for the items that SM1 prepared

| Item | Decision |
| --- | --- |
| Checkpoint variant | SAM 2.1 Hiera tiny (E1, E3, E5; lab README section 4). EfficientSAM3 is not used. |
| Text prompt support in 0.3.4 | None (D2). No model of the set takes text. |
| P1: sidecar or C++ process | Sidecar (D12). |
| Core ML use on macOS | Native Core ML models, not the ONNX Runtime Core ML execution provider (lab U15). The provider runs S3OD on the CPU (7.9 s, 10.3 GB) and stops the process with the decoder in the MLProgram format. |
| Acceptable CPU operator placement | Windows DirectML runs the set with the MVP session settings (one execution provider, no CPU entry). macOS runs the decoder on CPU_ONLY by decision (about 30 ms per call, no cost for a change of the point count; U15). |
| P4: edge refinement method | None (U3). The edge comes from the model output. |
| Model input size | SAM 2.1: 1024 x 1024 stretch. S3OD: letterbox to a 1024 long edge. The photo enters at a long edge of at most 4096 px. |
| Published file list | Section 8.2.1. |

#### 8.2.3 Runtime facts that later phases must keep

- ONNX Runtime 1.24.4 to 1.27.0 fold the two `If` nodes of the SAM 2.1 tiny encoder into freed
  memory (microsoft/onnxruntime#29071, fixed in 1.28.0). On the Mac the encoder output was wrong in
  every process; on Windows the result depends on the memory reuse. Alcedo Studio used ONNX Runtime
  1.24 (`ort` 2.0.0-rc.12, `api-24`). SM3.A raises it to 1.28 (`ort` 2.0.0-rc.13, `api-28`).
  `ort` 2.0.0-rc.14 and later need Rust 1.92.
- MVP session settings on ONNX Runtime: one execution provider per session and no other provider
  after an error; 4 intra-op CPU threads, 1 inter-op thread; CPU memory arena off; DirectML with
  sequential execution and memory patterns off.
- On DirectML, a decoder session runs only the point count of its first run at about 10.5 ms; other
  counts take 22 to 34 ms. Padding to a fixed point count breaks the masks. One session per point
  count gives 12 ms at about 90 MB each (E3). SM3.B implements one session for each of the
  counts 1, 2, and 4 and one general session, each run at load (SM3.B plan, decision B3).
- Core ML prepares the decoder again for each new point count on the GPU (about 52 ms); on the CPU
  the decoder takes about 30 ms per call. Keep all Core ML calls of one model on one thread.
- Decoder mask selection: one point without a box (a click) returns the token 1 to 3 with the best
  `iou_scores`, and the three tokens sorted by area are the levels of a level change without a model
  run. A box or several points use token 0 when its stability (area of logits > 0.05 / area of
  logits > -0.05) is at least 0.98, else the token 1 to 3 with the best score. Logits are clipped to
  +-32. A mask at the photo size is the bilinear upsampling of the 256 x 256 logits, threshold 0.
- Each stroke, click, or box decodes all points and the box of the selection in one decoder call,
  with the previous selected logits as `mask_input` (`has_mask_input` = 1). A stroke is 4 points at
  equal spacing along the pointer path; a click is 1 point.

#### 8.2.4 Cost of the accepted set

All models loaded in one process, warm p50 (lab README, section "Cost of the MVP pipeline"):

| Stage | Windows, RTX 3080 Laptop, DirectML | macOS, Apple M4, native Core ML |
| --- | ---: | ---: |
| Session load: encoder, decoder, head, S3OD | 2,623, 249, 112, 910 ms | 1,197, 69, 9, 43 ms |
| Encoder (once per photo) | 70.5 ms | 191 ms |
| Head (once per photo) | 2.0 ms | 2.1 ms |
| Decoder: box; 10 points with `mask_input` | 23.2; 22.2 ms | 33.5; 34.1 ms |
| S3OD (once per photo) | 191.1 ms | 531 ms |
| Sky head (once per photo, at the first Sky request; SM3.E port check, Alcedo Studio sidecar) | 54 ms | 82 ms |
| Memory with all models loaded | 2,666 MB dedicated GPU memory | 3,190 MB peak footprint of the Python process |

#### 8.2.5 Lab MVP

The lab contains a PySide6 MVP of the AI Mask tool (`scripts/poc/ai_mask_mvp/`): the pipeline
`mask_pipeline.py` (no UI) and the UI `ai_mask_app.py`. The pipeline is the reference for the host
rules of SM3.A and SM3.C: the port is correct when it gives the MVP results on the 14 quality photos
(lab README, section "Port checks"). The MVP UI is a reference for SM9 only (D15).

#### 8.2.6 Open items and their owner phases

| Item | Owner phase |
| --- | --- |
| The user check of the interaction by eye on Windows and macOS (U1) | SM9 |
| Subject "not found" rule (F1); People list threshold and order (F4a) | SM3.C; closed by C1 to C3 of the SM3.C plan |
| Decoder point limit of a long selection (the Core ML decoder takes 1 to 64 points; each stroke adds 4) | SM3.A rejects more than 64 points; SM9 decides the tool behavior at the limit |
| Combination across mask types (F7) and recipe storage (F9), lab decision O1 | Closed 2026-10-23 by D16 and the SM5 plan: ordered Add and Subtract components, recipe stored in the AI Mask source |
| Training data rule U13 and the S3OD float32 tolerance (lab acceptance record, section 6) | User decision before SM2 publishes files |
| Model unload when the user goes back to adjusting (U6) | SM3.A provides the operation; SM9 calls it |
| S3OD residency | Closed 2026-10-23 by D22: resident from load to unload |

---

## 9. Outline phases

Each outline phase lists its scope and its open decisions. A detailed phase plan replaces the outline
before the phase starts.

### 9.1 SM2 — Segmentation model download (replaced)

The [AI Models page plan](../ai/external_ai_model_download_plan.md) (tracking
[#579](https://github.com/zidage/AlcedoStudio/issues/579)) replaces SM2. The AI Mask model set
(SAM 2.1 tiny encoder and decoder, people and sky head, S3OD, sky head) downloads as one signed
package per platform from R2, as LUT packages and application updates do (AM1, AM2). The AI Models
settings page shows and installs it (AM4), and the AI Mask tool checks the installed set before it
loads models (AM5). The decisions A1-A10 and P1-P13 of that plan close the open decisions of the
former SM2 outline (download location, settings page placement, training data rule U13). The
installation directory stays `<model directory>/alcedo-ai-mask-v<version>`, the directory that
SM3.A reads.

### 9.2 SM3 — Sidecar segmentation service and C++ client

SM3 is split by function (D14). Each part changes Rust and C++ code as its function needs.

#### 9.2.1 SM3.A — Service architecture

Detailed plan: [SM3.A Segmentation service architecture](ai_segmentation_mask/phase_sm3a_segmentation_service_plan.md). Issues: SM3.A.1 #460,
SM3.A.2 #461.

Scope: `segmentation.proto`; the Rust segmentation service with one backend interface and two
adapters (ONNX Runtime with DirectML on Windows, native Core ML on macOS); explicit model load and
unload; segmentation sessions with one image embedding each; SAM 2.1 encode and decode with the host
rules of section 8.2.3; the C++ segmentation client with a persistent channel and a receive limit
that fits the largest message; the app-layer segmentation session; the ONNX Runtime 1.28 upgrade.

#### 9.2.2 SM3.B — Image embedding cache and first optimizations

Detailed plan: [SM3.B Image embedding cache and first optimizations](ai_segmentation_mask/phase_sm3b_embedding_cache_plan.md).
The user closed the open decisions on 2026-10-22: the cache key is an opaque host image key with
a key-only start (B1; SM4 defines its content), the cache keeps 4 embeddings and unload clears it
(B2), and DirectML loads decoder sessions for the point counts 1, 2, and 4 and one general
session (B3).

Scope:

- Keep the image embedding of recent analysis renders, so that a new session on the same image
  content does not run the encoder again. Define the cache key from the analysis render identity
  of SM4 and the model set identity. Define the memory limit against U6.
- Keep the encoder outputs in the native form of the backend (ONNX Runtime values, Core ML
  multi-arrays) between decoder calls, so that a decoder call does not copy 16 MB of features.
- DirectML: one decoder session per point count (section 8.2.3), with a limit on the number of
  sessions and their memory.
- Measure each change against the SM3.A latency record on Windows DirectML and macOS Core ML.

Open decisions: none (closed by B1 to B3 of the SM3.B plan).

#### 9.2.3 SM3.C — Task masks in the service

Detailed plan: [SM3.C Task masks in the segmentation service](ai_segmentation_mask/phase_sm3c_task_masks_plan.md).
The user closed the open decisions on 2026-10-23: Subject is "not found" only when no alpha value
reaches 0.5 (C1, F1); the People list uses the list threshold of the head and keeps the best box
when the person presence passes and no box reaches the threshold (C2, F4a); the list is sorted
from left to right (C3, F4a); the sidecar makes the Sky union and the union of the selected people
(C4). The union is the OR of the member masks at the session image size, returned as an R8 mask
(C5).

Scope:

- Load the people and sky head and S3OD with the SAM 2.1 models (section 8.2.1), on both
  platforms.
- Run the head once per session on `image_embed`. Run S3OD once per session at the first Subject
  or Background request.
- Add the task mask requests: Subject (the S3OD mask with the largest IoU score, unpadded),
  Background (1 - Subject), Sky (presence threshold, 8-connected regions, 3 points per region, one
  decoder call per region, union), and People (presence threshold, person boxes, one box prompt per
  person, People list).
- Decide the Subject "not found" rule (F1) and the People list threshold and order (F4a).
- Verify the port against the lab MVP results on the 14 quality photos (section 8.2.5).

Open decisions: none (closed by C1 to C4 of the SM3.C plan).

#### 9.2.4 SM3.D — People masks from S3OD on person crops

Detailed plan: [SM3.D People masks from S3OD on person crops](ai_segmentation_mask/phase_sm3d_people_masks_from_s3od_plan.md).
The user decided D21 and D22 on 2026-10-23. Issue: #469. Tracking issue: #468. Status: complete on
Windows and macOS on 2026-10-23 (phase plan, section 12; issues #474 to #478).

Scope:

- Change the lab MVP pipeline first. The user checks the people photos by eye.
- Run the head at session start and keep one crop per listed person in the image entry.
- Return the full-image S3OD alpha as the start of the People task, with the People list.
- Return the alpha of one person: S3OD on the person crop, with the crop rectangle. The host
  composes the person alphas (SM5.2).
- Remove the SAM 2.1 box prompts and the sidecar union from People. Sky does not change.
- Update the port check reference and run the port check on Windows and macOS.

### 9.3 SM4 — Analysis render in ReferenceSpace

Detailed plan: [SM4 Analysis render in ReferenceSpace](ai_segmentation_mask/phase_sm4_analysis_render_plan.md).
The user closed the open decisions on 2026-10-23: the analysis document is the import root
document (its Default Color Grade and DRT) with every Develop field of the working document (S1),
and the image key names only the analysis render content, without a model set identity (S5; the
sidecar clears its cache on a model change, B2). The user added a requirement: prompt coordinates
come from the viewport after crop and rotation and must map back to the analysis image (S6 to S8).

Scope:

- Add `DocumentGeometryUse::ReferenceSource`: no crop and no rotation, so EditSpace is
  ReferenceSpace. `RotatedUncroppedSource` keeps the rotation (section 5.3).
- Render the analysis document on the editor executor at a long edge of at most 4096 and read it
  back as RGB8. The edge refinement rectangle of the outline is dropped (U3).
- Make the image key of the SM3.B embedding cache.
- Map viewport points and boxes to normalized analysis image coordinates, and analysis image
  coordinates back to the viewport.

Open decisions: none (closed by S1 and S5 of the SM4 plan).

### 9.4 SM5 — AI Mask source model, R8 persistence, history, and project format

Detailed plan: [SM5 AI Mask source, asset storage, and edit session](ai_segmentation_mask/phase_sm5_ai_mask_source_and_session_plan.md).
The user decided D16 to D20 and approved P2, P3, and P6 on 2026-10-23. Issues: SM5.1 #470, SM5.2
#471, SM5.3 #472. Tracking issue: #468.

Scope:

- SM5.1: the AI Mask source kind (asset key, component recipe, Feather, Expand, model set
  identity, geometry fingerprint), its JSON and validation, the project mask asset table with
  compression and read verification, the read path for the editor, export, and thumbnails, and the
  reachability cleanup.
- SM5.2: the AI Mask edit session (session history stack, host composition, People toggles, edit
  of an existing AI Mask, the Commit payload).
- SM5.3: the editor integration (Open, Commit, Cancel, asset write), Version, reopen, export, and
  the Paste skip of D18.

Open decisions: none (closed by D16 to D20, P2, P3, and P6).

### 9.5 SM6 — Raster `MaskEvaluate` on three backends

This section records the completed SM6 scope. SM8 prospectively replaces host edge processing
with GPU processing through the same resource owner. Keep the SM6 evidence unchanged.

Detailed plan: [SM6 Raster Mask Evaluate](ai_segmentation_mask/phase_sm6_raster_mask_evaluate_plan.md).
SM6.1 makes the coverage texture (Feather and Expand on the host) and brings a mask asset reader
from the project to each render workspace. SM6.2 adds the texture cache and the raster pass on the
three backends. Issues: SM6.1 #492, SM6.2 #493. Tracking issue: #491.

Scope:

- Compile the AI Mask source to a raster `MaskEvaluate` pass.
- Sample the R8 texture with `MakeRasterMaskSamplingPlan` on CUDA, OpenCL, and Metal. Coverage
  outside `reference_bounds` is zero.
- Upload the R8 asset into the render workspace of the open image and release it when the image
  closes.
- Apply Feather and Expand of the source when the runtime makes the coverage texture (D20). Export
  uses the same function, so the preview coverage and the export coverage agree.
- Compare each backend with independent expected R8 values.

### 9.6 SM7 — Misalignment notice

Run SM7 after SM9.3, before SM2 and release qualification.
The [detailed assignment](ai_segmentation_mask/phase_sm9_tool_and_sm7_notice_plan.md#44-sm7---misalignment-notice-after-the-tool)
uses the completed tool surface and existing fingerprint.
It compares each committed Mask with current Lens/RAW geometry and marks only mismatched Masks.
It does not regenerate coverage, change stored pixels, or add persisted warning state.

### 9.7 SM8 — Coverage preview request

Replanned as GPU coverage and one display composite. Run it immediately after SM6.
The [detailed plan](ai_segmentation_mask/phase_sm8_gpu_coverage_and_display_plan.md) replaces the old outline.

- SM8.1 defines stable source reads and workspace-owned GPU coverage operations.
- SM8.2-SM8.4 implement exact Expand/Feather on CUDA, OpenCL, and Metal for every production consumer.
- SM8.5 adds a dedicated coverage channel through existing GPU presentation adapters.
- SM8.6 adds the one viewport composite shared by candidate, committed Mask, and Grade Union.
- SM8.7 connects candidate replacement, retained outputs, temporary Undo/Redo, Commit, and Cancel.
- SM8.8 records full-path latency, numeric agreement, and resource release evidence for #494.

The display composite runs in the existing viewport draw after photo base/detail selection.
No duplicate composite runs in the photo DAG or candidate QSG layer.
Coverage visibility and tint do not change photo cache identity.
Old displayed coverage survives a new model request and retires only after safe replacement.
Historical model outputs stay in the existing session store, not in a GPU history cache.
P5 is replaced by this design. P8 remains excluded and does not block implementation.

### 9.8 SM9 — AI Mask tool UI and interaction

Run after SM8 and before SM7. The
[detailed plan](ai_segmentation_mask/phase_sm9_tool_and_sm7_notice_plan.md) splits this work:

- SM9.1 adds the app controller, dialog model, model lifecycle, and error/busy presentation.
- SM9.2 adds stroke, click, box, Add/Subtract, tasks, People boxes, and valid level selection.
- SM9.3 adds edge previews, local Undo/Redo, Commit/Cancel, and stored-source thumbnails.

Use one dialog bound to the main viewport and the existing temporary session stack.
Run inference after input release, never during pointer movement.
Reject a prompt above 64 points and preserve the previous valid state.
One accepted edge edit adds one local step; intermediate slider updates only request display.
Models unload on return to adjustment (U6/D22).
Until SM2, missing models report the expected local directory and the real error.
The lab UI remains a reference, not another implementation to copy unchanged.

### 9.9 SM10 — Product qualification

Scope:

- Real RAW photos on CUDA, OpenCL, and Metal.
- Crop, rotation, zoom, region-of-interest render, and export with AI Masks.
- History, Version, Paste, reopen, and package.
- Installed-application download, license files, and a clean-machine start.
- Latency and memory compared with the targets that the user approves after SM1.

---

## 10. Cross-phase acceptance matrix

| Behavior | Expected result |
| --- | --- |
| Crop after Mask creation | The Mask covers the same image content. Content outside the old crop keeps its coverage. |
| Rotation after Mask creation | The Mask rotates with the image content. |
| Zoom and region-of-interest render | The Mask edge stays on the same image content at each zoom level. |
| Export | The exported Mask coverage matches the preview coverage on the same image content. |
| Undo and Redo of Mask creation | The Mask and its coverage appear and disappear with the history operation. |
| Undo and Redo in an open AI Mask session | The Candidate Mask moves through the session steps. The main history does not change and no model runs. |
| New model request during an open session | The prior coverage stays visible. Stored coverage and earlier model outputs remain available for Undo. |
| Shared coverage display | Candidate, committed single Mask, and Grade Union use the same viewport composite. |
| Feather and Expand preview | GPU processing uses the full analysis grid. Source upload count stays fixed for edge-only edits. |
| GPU preview error | Real error; desired state retained; previous display identified; Commit disabled until Retry succeeds. |
| Coverage visibility or tint | Viewport redraw only. No photo DAG work, history edit, scope tint, or exported overlay. |
| Cancel of an AI Mask session | The document and the project mask asset table do not change. |
| Paste to another image | The AI Masks are not pasted. The paste result names them. |
| Reopen | A reopened project shows the same coverage. It does not run the model. |
| Missing model | The tool shows the download panel. The document does not change. |
| Sidecar failure | The tool shows the real error. The document does not change. |
| Corrupt R8 asset | Rendering reports the real error. It does not render an empty Mask. |
| Lens or RAW decode change | The Mask stays as stored. The UI shows the notice of D9. |

---

## 11. Build and evidence rules

- Use only the `win_debug`, `win_release`, `macos_debug`, and `macos_release` presets.
- Run MSVC commands through the PowerShell tool with `scripts\msvc_env.cmd`.
- Put all temporary files, weights, photos, and reports under `build/tmp/<phase>/`.
- Run only the test binaries for the changed code and their direct callers. Only the user starts a
  full test suite.
- Report pass, fail, skip, and unavailable results separately. Do not report an unavailable
  measurement as zero.
- Record user visual reviews as manual verification. Keep them separate from automated evidence.

---

## 12. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| ONNX Runtime before 1.28 computes the SAM 2.1 tiny encoder wrongly. | A build with `ort` older than 2.0.0-rc.13, or a port check failure on Windows. | Keep ONNX Runtime 1.28 or later. Do not add a workaround in the graph or the session settings. |
| The Core ML adapter cannot be built or run while the Mac is not available. | SM3.A completion record. | Report the macOS build and run as not done. Run them on the Mac before SM3.B is complete. |
| DirectML runs operators on the CPU provider. | SM1 operator placement report; SM3.A sessions have no CPU entry, so a failed placement is a load error. | Report the error. The user decides. Do not change the device without a user decision. |
| Whole-image input is too coarse for small subjects. | SM1 cases 5 and 6. | Report the IoU. D7 stays. A new refinement design needs a separate user decision; SM7 only shows misalignment. |
| The edge quality is low at full resolution. | SM9 user check by eye. | Report it. U3 rejects edge post-processing; a change needs a new user decision. |
| The project format change rejects older projects. | SM5 format analysis. | Stop SM5 until the user approves the format change and its result. |
| The license terms change. | Meta SAM License page. | Report the change before SM2 publishes files. |

Stop implementation when:

- a required owner API does not exist;
- the source audit no longer matches the branch;
- a phase can pass the 2000-line limit;
- a new persistence format lacks a rejection or migration decision;
- an operation needs a fallback that the user did not authorize;
- a proposed consistency mechanism lacks a real production interleaving.

Before each phase, read `AGENTS.md` and each applicable skill again. Repository rules can change after
this plan was written.
