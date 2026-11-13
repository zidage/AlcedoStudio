# SM4 Analysis Render in ReferenceSpace — Phase Plan

Date: 2026-10-23.

Status: implemented and verified on Windows on 2026-10-23
(`feature/ai-segmentation-sm4`, #466). The measurements of section 9 are not
taken (section 13.2).

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), section
9.3.

Prerequisites: SM3.A and SM3.B (merged to `main` in `61d3f705`): the `AiMaskSegmentationService`
start calls with an image key. SM3.C ([phase plan](phase_sm3c_task_masks_plan.md), merged in
`99f4ad7a`, #465) is not a dependency.

Source revision used for the audit: `4ec7230a` on `feature/ai-segmentation-sm3c`.

---

## 1. Decision record

### 1.1 User request (2026-10-23)

- Open an issue that sets the scope of SM4 and implement SM4.
- The user adds one requirement that the master plan did not state. The user places points,
  strokes, and boxes on the photo as the viewport shows it: after the document crop and rotation.
  The model reads the analysis image: the whole source without crop and rotation (D7). Each
  prompt coordinate must therefore map from the viewport back to the analysis image that SM4
  gives to the sidecar.

### 1.2 Inherited decisions

- D6 (R8 coverage), D7 (the model reads the whole source image), D8 (the analysis render disables
  the document crop), D9 (Lens and RAW decode changes can misalign a Mask), D10 (the coordinate
  rule must also serve a future color picker).
- SM3.B B1: the host supplies an opaque image key; SM4 defines its content. B2: the sidecar cache
  keeps 4 embeddings; unload and a model change clear it.
- Session start limits (`segmentation.proto`): RGB8 row-major, long edge at most 4096, key at most
  256 bytes. Prompt points and boxes are normalized image coordinates in `[0, 1]`.

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| S1 | User decision. The analysis document is the import root document of the image (its Default Color Grade and its DRT) with every Develop field of the current working document: RAW decode, white balance, Lens, and the camera profile. No user Color Grade and no user DRT change reaches the model. | User grading never changes the model input or the image key, so grading never runs the encoder again. For a RAW image the Default look (+1.5 EV, contrast 15, saturation 1.3) is in the root Color Grade; without it the model reads an image about 1.5 EV darker. The root DRT of a RAW image is SDR. The lab evaluated display-referred photos. |
| S2 | A new value `DocumentGeometryUse::ReferenceSource` binds a full-frame crop, rotation 0, and the crop frame output. EditSpace is then ReferenceSpace, and `render_to_reference` is a pure scale. The document is not changed. | D7 and D8. `RotatedUncroppedSource` keeps the rotation (master plan section 5.3). With crop `{0, 0, 1, 1}` and rotation 0, `ResolveRenderGeometry` gives `reference_to_edit` = identity and `edit_extent` = `full_reference_extent`. |
| S3 | The analysis render uses the editor image render port (`IEditorImageRenderPort::ScheduleImages`) on the editor executor: FULL decode, Preview resampling, the whole frame, long edge at most 4096 (`kAiMaskAnalysisMaxLongEdge`). | The port already renders documents of the held image to host pixels and reuses the prepared source and the sensor result of the viewport. A Batch thumbnail path cannot render uncommitted Develop values and stops at 2048 px. |
| S4 | The RGBA32F display output becomes RGB8 with the SDR rule of the editor comparison: saturate cast with rounding to 0 to 255, alpha dropped. An ST 2084 or HLG output, a non-finite value, a pixel size that differs from `render_extent`, or a long edge above 4096 is a failure with the real error. | The model input is 8-bit display-referred RGB (lab `read_rgb8`). No silent tone mapping of HDR output. |
| S5 | User decision. The image key names only the analysis render content: `am1-` and the 32 hexadecimal digits of the XXH3 128-bit hash of the key version, the element id, the image id, the geometry use, the long edge limit, and the canonical JSON of the analysis document. The key does not contain a model set identity. | B2 clears the sidecar cache on unload and on a model change, so a cached embedding cannot outlive its model set. The key needs no render, so a cache hit skips the render (B1). |
| S6 | A prompt point is the normalized ReferenceSpace position of the pointer: `MaskEditGeometry::MapItemToReference` (the presented crop, rotation, Geometry panel frame, zoom, pan, and region-of-interest patch) followed by the check that the position is inside `[0, 1]` on both axes. A point outside the source gives no prompt point. | By S2, normalized ReferenceSpace is the normalized coordinate of the analysis image. The Geometry panel frame shows border corners outside the source; the model has no pixels there. |
| S7 | A prompt box comes from two opposite corners in item coordinates. The four item corners map to ReferenceSpace; the box is their axis-aligned bounding box, clipped to `[0, 1]`. A box without area after clipping gives no box. | With a document rotation, the screen rectangle is a rotated rectangle in the source. The decoder takes only an axis-aligned box. The bounding box contains every pixel that the user enclosed. SM9 can replace this rule with a user decision. |
| S8 | The inverse mapping from normalized analysis image coordinates to item coordinates is one affine transform, built from the current view mapping. | SM9 draws Candidate Masks over the photo with it. The same transform serves a future color picker in the other direction (D10). |
| S9 | No new cancellation protocol. The analysis render is one image job of the existing port; the port cancels it at shutdown, and the session refuses a new job while one runs (Busy). Changed by issue #598: the port runs accepted jobs in order, so the analysis render waits for the job before it and `Busy` was removed. | AGENTS.md: a consistency mechanism needs a real interleaving. The tool waits for the image before it starts a session. |

### 1.4 Rejected options

- The current look with only crop and rotation removed (S1: each grading edit changes the key).
- Develop and DRT without any Color Grade (S1: a RAW image reaches the model about 1.5 EV darker
  and without the Default contrast and saturation).
- A model set identity in the key (S5).
- Prompt coordinates in pixels of the displayed frame or of the analysis image (S6: normalized
  ReferenceSpace is independent of both sizes).

---

## 2. Product and data specification

SM4 has no user-visible change. Its users are SM9 (the tool start sequence and the prompt input)
and SM7 (the commit). The tool start sequence of SM9 is:

1. `RenderAiMaskAnalysisImage(KeyOnly)` gives the image key.
2. `AiMaskSegmentationService::StartCachedImage(key)`. A hit ends the start.
3. On `image_not_cached`: `RenderAiMaskAnalysisImage(KeyAndPixels)` gives the RGB8 image and the
   same key; `StartImage(image, key)`.

### 2.1 Analysis render result

| Field | Content |
| --- | --- |
| `status` | `Completed`, `Busy` (the port runs another image job; removed by issue #598), `Rejected` (no interactive session, no history, no render port), `Failed` (document, render, or conversion error). |
| `message` | The real error; empty when Completed. |
| `image_key` | S5 key; set when Completed. |
| `image` | `SegmentationRgb8Image`; empty for KeyOnly. |
| `full_reference_extent` | ReferenceSpace size of the render; zero for KeyOnly. |

### 2.2 Coordinates

- Analysis image coordinate `(u, v)` in `[0, 1]` = ReferenceSpace pixel `(u * W, v * H)` with
  `W x H` = `full_reference_extent`. The analysis image of `w x h` pixels covers it with the scale
  `w / W`, `h / H` (S2).
- Prompt points and boxes use `(u, v)` directly (`SegmentationPoint`, `SegmentationBox`).
- Pixels outside the analysis image do not exist. A point outside gives no prompt; a box is clipped.

---

## 3. Scope

### 3.1 Included

| Module | Responsibility |
| --- | --- |
| `edit/geometry`, `edit/runtime/graph_compiler` | `DocumentGeometryUse::ReferenceSource` and its frame binding. |
| `edit/graph` | `DevelopParamsModel::UseAllSettingsFrom`, `PipelineDocument::UseDevelopSettingsFrom`: the analysis document takes every Develop field. |
| `app/ai_mask_analysis_image` (new) | Analysis document, image key, render request, RGB8 conversion. |
| `app/editor_session_ports`, `album_backend/editor_session_history_port` | `BuildAiMaskAnalysisInput`: the analysis document from the held history. |
| `app/editor_session_service` | `RenderAiMaskAnalysisImage`: owner command, image job, result. |
| `ui/edit_viewer/ai_mask_prompt_geometry` (new) | Item to prompt point and box; analysis image to item transform. |

### 3.2 Excluded

- The AI Mask tool, its controller, and the start sequence of section 2 (SM9).
- Edge refinement renders of a ReferenceSpace rectangle: U3 removed edge refinement (master plan
  P4), so the master plan outline item "a ReferenceSpace rectangle at up to 4096 for edge
  refinement" is dropped.
- The resize to the model input size (1024 x 1024): the sidecar does it (SM3.A).

---

## 4. Current source audit

| Area | Current owner and path | Current behavior |
| --- | --- | --- |
| Frame binding | `graph_compiler.cpp` `ImageParamsForRequest` | The only branch on `DocumentGeometryUse`. `RotatedUncroppedSource` keeps the rotation and frames the rotated source bounds. |
| Geometry resolver | `render_geometry_resolver.cpp` `ResolveRenderGeometry` | Crop `{0,0,1,1}`, rotation 0, `CropFrame`: `reference_to_edit` = identity, `edit_extent` = `full_reference_extent`. |
| Host images of the held image | `editor_image_render_port.hpp`, `EditorSessionRenderSchedulerPort::ScheduleImages` / `RunImageJob` | One job at a time on the editor worker and the Interactive executor; `require_host_output`; result `RenderedPipelineImage` (RGBA32F, the resolved geometry, the display encoding). |
| Preview images | `EditorSessionService::RenderPreviewImages` | Owner command; builds Root and Current through `IEditorHistoryPort::BuildComparisonInputs`; Busy while another job runs. |
| Root replay | `editor_comparison_inputs.cpp` `BuildInput` | `BuildDocumentFromRoot`, `ValidateProductDocument`, HDR rejection, `UseSensorSettingsFrom` (every Develop field except white balance), frozen with the lineage of the working preview. |
| SDR conversion | `comparison_presentation_image.cpp` `ConvertRenderedImageForComparison` | Validates and converts RGBA32F to RGBA8 with saturate cast; rejects ST 2084 and HLG. UI layer, QImage output. |
| Pointer mapping | `MaskEditGeometry::MapItemToReference`, `EditorInteractionController::maskEditViewMapping` | Item to ReferenceSpace through the presented frame geometry (crop, rotation, Geometry panel frame), zoom, pan, and the region-of-interest patch. `inside_photograph` checks the displayed frame only, not the source. |
| Key hashing | `mask_asset.cpp` | XXH3 128-bit over canonical bytes. |

No production code builds a `SegmentationRgb8Image` or an image key yet.

---

## 5. Target architecture

### 5.1 Primary success call chain (KeyAndPixels)

```text
SM9 caller (any thread)
  -> EditorSessionService::RenderAiMaskAnalysisImage(KeyAndPixels, completion)
     -> SubmitCommand(RenderAiMaskAnalysisImage)                     session owner thread
     -> pipeline->CurrentPreview(element)                             working values
     -> history->BuildAiMaskAnalysisInput(guard, current)
        -> BuildAiMaskAnalysisDocument(graph, root, current, element)
           BuildDocumentFromRoot(root) -> ValidateProductDocument -> HDR rejection
           -> UseDevelopSettingsFrom(current) -> Freeze -> Preview(lineage of current)
        -> MakeAiMaskAnalysisImageKey(element, image, analysis document)
     -> images->ScheduleImages({analysis snapshot}, MakeAiMaskAnalysisRenderRequest())
        -> RunImageJob -> PipelineExecutor::ApplyImage                 editor render worker
           -> BindFrameGeometry(ReferenceSource) -> RenderedPipelineImage
     -> completion on the render worker
        -> ConvertAiMaskAnalysisImage(rendered) -> SegmentationRgb8Image
        -> on_complete({Completed, key, image, full_reference_extent})
```

### 5.2 Primary failure call chains

| Failure | Result |
| --- | --- |
| Session not interactive, no history guard, no render port | `Rejected` with the reason; no job. |
| The port runs another job (comparison, preview images) | `Busy`; that job continues. Changed by issue #598: the analysis render runs after that job. |
| Root replay, product validation, HDR root DRT, Develop copy | `Failed` with the real error; no job. |
| Render or download error, cancellation at shutdown | `Failed` with the port message. |
| Conversion error (HDR output, non-finite value, size) | `Failed` with the real error; no image. |

### 5.3 Prompt mapping

```text
pointer item point
  -> EditorInteractionController::maskEditViewMapping()
  -> MapItemToAiMaskPromptPoint(mapping, item) -> optional normalized (u, v)     S6
  -> MapItemRectToAiMaskPromptBox(mapping, a, b) -> optional NormalizedRect        S7
analysis (u, v) -> AiMaskAnalysisToItemTransform(mapping) -> item point            S8
```

---

## 6. File and API map

| File | Change |
| --- | --- |
| `include/edit/geometry/render_request.hpp` | `DocumentGeometryUse::ReferenceSource`. |
| `edit/runtime/graph_compiler.cpp`, `include/edit/runtime/graph_compiler.hpp` | `ImageParamsForRequest` returns default `ImageGeometryParams` for `ReferenceSource`. |
| `include/edit/graph/develop_node_model.hpp`, `edit/graph/develop_node_model.cpp` | `DevelopParamsModel::UseAllSettingsFrom(const DevelopParamsModel&)`. |
| `include/edit/graph/pipeline_document.hpp`, `edit/graph/pipeline_document.cpp` | `PipelineDocument::UseDevelopSettingsFrom(const PipelineDocument&)`, with the checks of `UseSensorSettingsFrom`. |
| `include/app/ai_mask_analysis_image.hpp`, `app/ai_mask_analysis_image.cpp` (new; library `AiMaskAnalysisImage`, linked by `EditorSessionService`, `AlbumBackendLib`, and the test binaries that compile the history port) | `kAiMaskAnalysisMaxLongEdge`, `BuildAiMaskAnalysisDocument`, `MakeAiMaskAnalysisImageKey`, `MakeAiMaskAnalysisRenderRequest`, `ConvertAiMaskAnalysisImage`. |
| `include/app/editor_session_ports.hpp` | `IEditorHistoryPort::BuildAiMaskAnalysisInput` (default rejects). |
| `album_backend/editor_session_history_port.{hpp,cpp}` | Implementation under the history lock. |
| `include/app/editor_session_service.hpp`, `app/editor_session_service.cpp`, `editor_session_command_queue.hpp`, `editor_action_policy.cpp` | `EditorAiMaskAnalysisContent`, `EditorAiMaskAnalysisStatus`, `EditorAiMaskAnalysisResult`, `RenderAiMaskAnalysisImage`, command kind. |
| `include/ui/edit_viewer/ai_mask_prompt_geometry.hpp`, `ui/edit_viewer/ai_mask_prompt_geometry.cpp` (new, in `EditorViewerLogic`) | `MapItemToAiMaskPromptPoint`, `MapItemRectToAiMaskPromptBox`, `AiMaskAnalysisToItemTransform`. |

---

## 7. Implementation phases

### 7.1 SM4.1 — Analysis render and image key

Objective: render the analysis image of the held image and give its key.

Steps:

1. Add `ReferenceSource` and its binding. Update the enum comments of `render_request.hpp`,
   `graph_compiler.hpp`, and `pipeline_task.hpp`.
2. Add `DevelopParamsModel::UseAllSettingsFrom` (`TakeFieldsFromThenMutate` that keeps no field)
   and `PipelineDocument::UseDevelopSettingsFrom`. Share the precondition checks with
   `UseSensorSettingsFrom`.
3. Add `ai_mask_analysis_image`: the analysis document (S1), the key (S5), the render request
   (S2, S3), and the conversion (S4).
4. Add `IEditorHistoryPort::BuildAiMaskAnalysisInput` and its implementation: the same lineage
   check as `BuildComparisonInputs`; returns the frozen snapshot and the key.
5. Add `EditorSessionService::RenderAiMaskAnalysisImage(content, completion)` after the pattern of
   `RenderPreviewImages`. KeyOnly completes on the owner thread without a job.

Exit criteria: the tests of section 8.1 pass; the CUDA test runs on the RTX 3080 Laptop.

Expected diff: about 1,150 lines (production 550, tests 600).

### 7.2 SM4.2 — Prompt coordinates

Objective: map viewport input to prompt coordinates of the analysis image and back.

Steps:

1. Add `ai_mask_prompt_geometry` with the functions of section 5.3 and the rules S6 to S8.
2. Register the test file in `MaskEditGeometryTest`.

Exit criteria: the tests of section 8.2 pass.

Expected diff: about 450 lines (production 150, tests 300).

---

## 8. Tests

### 8.1 SM4.1

| Test | Target | Assertion |
| --- | --- | --- |
| `ReferenceSourceRequestIgnoresCropAndRotation` | `GpuDagGeometryTest` | A document with crop and rotation 7 degrees binds `edit_extent` = `full_reference_extent`, `reference_to_edit` = identity, and a pure-scale `render_to_reference`; a 4096 long-edge request keeps the aspect ratio. |
| `ReferenceSourceFrameMatchesTheUncroppedUnrotatedDocument` | `GpuDagCudaDocumentGeometryRequestTest` | The `ReferenceSource` render of the cropped and rotated fixture has the size of the uncropped source, equals the render of the same source without crop and rotation within 1/1024, and reuses the sensor result. |
| `UseAllSettingsTakesWhiteBalanceAndSensorSettings` | `EditorComparisonInputsTest` (`AiMaskAnalysisDevelopTest` suite) | After the call, every Develop field and stamp equals the current Model; a self call throws and changes nothing. |
| `AiMaskAnalysisDocumentHasTheRootLookAndTheCurrentDevelop` | `EditorComparisonInputsTest` | Exposure of the analysis grade equals the root; white balance and Lens equal the current document; the snapshot has the current lineage and element. |
| `AiMaskAnalysisImageKeyIgnoresGradingAndTracksDevelop` | `EditorComparisonInputsTest` | Equal states give equal keys; a Color Grade change keeps the key; a white balance change, another element id, or another image id changes it; the key has `am1-` and 32 hex digits. |
| `RenderRequestIsTheWholeReferenceSourceFrameAt4096` | `EditorComparisonInputsTest` (`AiMaskAnalysisImageTest` suite) | The request uses `ReferenceSource`, the whole frame, Preview resampling, and max edge 4096. |
| `ConversionRoundsToRgb8AndDropsAlpha` | `EditorComparisonInputsTest` (`AiMaskAnalysisImageTest` suite) | Known RGBA32F values give the expected RGB8 bytes, row-major. |
| `ConversionRejectsHdrNonFiniteAndSizeErrors` | `EditorComparisonInputsTest` (`AiMaskAnalysisImageTest` suite) | ST 2084, HLG, NaN, a size mismatch, and a long edge of 4097 throw with the reason. |
| `AiMaskAnalysisImageRendersTheAnalysisDocumentWithoutCropOrRotation` | `EditorComparisonServiceTest` | One job with one snapshot, the `ReferenceSource` request, max edge 4096; the completion has Completed, the key, and RGB8 pixels; nothing is committed. |
| `AiMaskAnalysisKeyOnlyStartsNoJob` | `EditorComparisonServiceTest` | KeyOnly completes with the same key as KeyAndPixels and starts no job. |
| `AiMaskAnalysisReportsBusyWhileTheComparisonRenders` | `EditorComparisonServiceTest` | Busy; the comparison job continues; a render failure reaches the completion as Failed with the port message. |

### 8.2 SM4.2

| Test | Target | Assertion |
| --- | --- | --- |
| `PromptPointOnCroppedRotatedViewMapsToTheSourcePosition` | `MaskEditGeometryTest` | For a crop and rotation 7 degrees, the item position of a known ReferenceSpace point maps back to that point divided by the reference extent within 1e-5. |
| `PromptPointOutsideTheSourceIsRejected` | `MaskEditGeometryTest` | A border corner of the Geometry panel frame and a point off the photo give no prompt point. |
| `PromptPointInRegionOfInterestPatchMapsLikeTheFullFrame` | `MaskEditGeometryTest` | A RoiFrame mapping gives the same prompt point as the full frame for the same image content. |
| `PromptBoxOnRotatedViewIsTheBoundingBoxOfTheMappedCorners` | `MaskEditGeometryTest` | Rotation 7 degrees: the box contains the four mapped corners and touches each side; a box partly outside is clipped; a box fully outside gives none. |
| `AnalysisToItemTransformInvertsThePromptMapping` | `MaskEditGeometryTest` | Item to prompt point to item returns the item point within 0.25 logical px. |

---

## 9. Measurements

Record on Windows (RTX 3080 Laptop, CUDA, release build) for one RAW photo of the SM1 quality
set: the analysis render time and the conversion time at 4096 px. Report a measurement that was
not taken as not taken.

---

## 10. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagGeometryTest GpuDagCudaDocumentGeometryRequestTest EditorComparisonInputsTest EditorComparisonServiceTest MaskEditGeometryTest EditorSessionHistoryPortTest AlbumBackendLib
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -j 1 --output-on-failure -R "GpuDagGeometryTest|GpuDagCudaDocumentGeometryRequestTest|EditorComparisonInputsTest|EditorComparisonServiceTest|MaskEditGeometryTest|EditorSessionHistoryPortTest"
```

---

## 11. Repository rules that apply

- Read `AGENTS.md` and the applicable skills again before implementation.
- No fallback: an HDR root DRT, a failed render, or a failed conversion is an error, not a
  substitute image.
- Update data through its owner: the analysis document is a private frozen document, as in the
  editor comparison; the working document does not change.
- No new consistency mechanism without an executable interleaving (S9).
- Include the defining header; include only the needed OpenCV module headers.
- Format new files completely; format only changed lines of existing files.
- Run only the test targets of section 10. Only the user starts a full test suite.

---

## 12. Expected diff

SM4.1 about 1,150 lines, SM4.2 about 450 lines, this plan and the master plan about 450 lines.

---

## 13. Completion record

### 13.1 SM4 completion record (2026-10-23)

Implemented on `feature/ai-segmentation-sm4` on top of the plan commit `0b4dca93` (`main` at
`99f4ad7a` with this plan). Changed lines: about 1,350 added (production about 680, tests about 660); SM4.2 is
about 385 of them. Plan decisions S1 to S9 are implemented as written.

Implementation notes:

- `ReferenceSource` returns default `ImageGeometryParams` in `ImageParamsForRequest`: crop
  `{0, 0, 1, 1}`, rotation 0, `CropFrame`. No other code branches on the value.
- `PipelineDocument::RequireDevelopOfSameImage` holds the precondition checks that
  `UseSensorSettingsFrom` and `UseDevelopSettingsFrom` share. The error text names Develop
  settings instead of sensor settings.
- The canonical text of the key also has the render quality, so a change of the resampling filter
  of the analysis render changes the key.
- The job completion converts the render result on the editor render worker, so the RGBA32F
  download does not leave the worker.
- The conversion tests and the analysis document tests are in the `EditorComparisonInputsTest`
  binary, which already compiles the real history port. No separate test binary was added.

Primary call chains:

```text
EditorSessionService::RenderAiMaskAnalysisImage(KeyAndPixels)          session owner
  -> EditorSessionPipelinePort::CurrentPreview
  -> EditorSessionHistoryPort::BuildAiMaskAnalysisDocument             history lock
     -> BuildAiMaskAnalysisDocument -> BuildDocumentFromRoot -> ValidateProductDocument
        -> IsHdrExportEncoding -> PipelineDocument::UseDevelopSettingsFrom
        -> DevelopParamsModel::UseAllSettingsFrom -> Freeze -> PipelineGraphSnapshot::Preview
  -> MakeAiMaskAnalysisImageKey
  -> IEditorImageRenderPort::ScheduleImages(MakeAiMaskAnalysisRenderRequest)
     -> RunImageJob -> PipelineExecutor::ApplyImage                    editor render worker
        -> GraphCompiler::BindFrameGeometry -> ImageParamsForRequest(ReferenceSource)
     -> completion -> ConvertAiMaskAnalysisImage -> on_complete(Completed)
EditorInteractionController::maskEditViewMapping
  -> MapItemToAiMaskPromptPoint -> MaskEditGeometry::MapItemToReference -> [0, 1] check
  -> MapItemRectToAiMaskPromptBox -> four corners -> bounding box -> clip
  -> AiMaskAnalysisToItemTransform -> MaskEditGeometry::MapReferenceToItem (three points)
```

Automated evidence (Windows, `win_debug`, RTX 3080 Laptop for the CUDA test):

- `ctest -j 1` over `GpuDagGeometryTest`, `GpuDagCudaDocumentGeometryRequestTest`,
  `EditorComparisonInputsTest`, `EditorComparisonServiceTest`, `MaskEditGeometryTest`, and
  `EditorSessionHistoryPortTest`: 186 of 187 passed. The 16 new tests of section 8 passed.
- The one failure, `EditorSessionHistoryPortTest.CheckoutAndPasteHashesAreUnchanged` (paste head
  and chain hashes), also fails on the plan commit without the SM4 code (the same tree as
  `0b4dca93`) after a rebuild of that target. SM4 does not cause it.
- `AlbumBackendLib` builds with the production history port implementation.

Not run: the full test suite (only the user starts it), the macOS build (SM4 has no
platform-specific code; the Metal and OpenCL backends use the same `BindFrameGeometry`), and the
QML application.

### 13.2 Measurements

Not taken. Section 9 needs a release build and a RAW photo in the running editor. SM9 adds the
first caller in the application, so the measurement is taken with SM9.
