# Mask Luminance Range and Hue Range - Phase Plan

Date: 2026-10-26. Status: MR1 complete (2026-10-26, PR #556, branch
`feature/mask-ranges-mr1-model`); MR2 complete (2026-10-26, branch
`feature/mask-ranges-mr2-gpu-eval`, stacked on MR1); MR3 complete (2026-10-27, three stacked
branches `feature/mask-ranges-mr3-*` on MR2); MR4 complete (2026-10-27, branch
`feature/mask-ranges-mr4-range-bars`, stacked on MR3). Follow-up issue #564 (precise range
controls, shared Range section, AI Mask edit on demand): section 10, phases RP1-RP4, complete
(2026-10-27, four stacked branches `feature/mask-range-precision-rp*`). Follow-up issue #604
(ranges of a new AI Mask during its edit): section 11, complete (2026-10-29, branch
`feature/ai-mask-new-candidate-ranges`).

Parent: [Node-aware Pipeline Editing and Mask Creation Master Plan](../node_mask_editor_master_plan.md),
sections 8.3 (Color Range and Luminance Range), 8.4 (Union), and 10.3-10.4 (content-dependent
coverage). Display: [SM8 GPU Coverage and One Display Composite](../ai_segmentation_mask/phase_sm8_gpu_coverage_and_display_plan.md).
Source audit: `5111bd47`, branch `fix/radial-mask-drag-layers-closed` (same mask and coverage code
as `main` `c86b1fbb`).
Tracking: [#552](https://github.com/zidage/AlcedoStudio/issues/552).

## 1. Required result

Every Mask of a Color Grade gets two qualifiers:

- **Luminance Range**: a two-handle range on a black-to-white bar.
- **Hue Range**: a two-handle range on a hue scan bar. The range can wrap through 0/360 degrees.

Both ranges default to the full range (everything selected). A full range is the identity: it
does not change coverage, rendered pixels, cache keys, or the serialized document.

The two range bars sit at the top of each Mask panel. While the user edits a range, the viewport
shows which pixels the Mask selects, with the existing SM8 coverage overlay. The overlay shows the
coverage of the selected Mask of the selected node only, evaluated on the image that enters that
node.

Range edits are Mask field edits: Interactive renders during a drag, one history commit on
release, Undo/Redo, Version, Paste, and reopen restore them.

## 2. Current source facts

Paths are relative to `alcedo_studio/src/`.

### 2.1 Already in place

- `MaskModel` already has `std::optional<ColorRangeModel> color_range` and
  `std::optional<LuminanceRangeModel> luminance_range` (`include/edit/mask/mask_model.hpp:136-168`).
  Both types hold only `bool enabled = false`. Validation rejects `enabled == true`
  (`edit/mask/mask_model.cpp:212-228`) and any other key (`ValidateRangeObject`, :102-115).
  `MaskModelToJson` writes both keys; an empty optional is JSON `null` (:281-293, :531-532).
- The compiler already gives every `MaskEvaluate` pass a second input `range`, bound to the Grade's
  incoming scene edge, never to its output (`edit/runtime/graph_compiler.cpp:269-314`,
  `include/edit/runtime/compiled_mask_stack.hpp:47-56`, checked in
  `edit/runtime/execution_plan.cpp:117`). No kernel reads it yet.
- The mask result key already mixes in the Grade input scene key
  (`edit/runtime/result_content_key.cpp:187-196`). The plan hash mixes only `has_value()` of both
  ranges (`edit/runtime/graph_compiler.cpp:175-176`).
- Per-Grade execution order (`include/edit/runtime/plan_executor.hpp:203-253`): for each Grade in
  backbone order, every enabled `MaskEvaluate`, then `MaskUnion`, then `PrimaryColorGrade` on the
  running `scene` binding. When the masks run, `scene` still holds the output of the previous
  Grade (the first Grade reads `develop_output`). This is the image the ranges must read.
- The image between Grades is ACEScc-encoded AP1 (`include/edit/runtime/execution_plan.hpp:176`).
- Coverage per Mask is `invert -> opacity -> clamp`, stored as R8 (`FinishEffectiveCoverage`,
  `edit/runtime/cuda/cuda_mask_pass.cu:32-35`; OpenCL `mask.cl`, Metal `mask.metal`). The
  `color_grade_node_model.hpp:186-190` comment already states that invert is applied before the
  range fields.
- SM8 coverage display: `IMaskCoverageSink` second present queue, RGBA32F with coverage in red,
  `ViewportMaskCoverageLayer`, one composite in `ui/editor_rhi/shaders/editor_viewport.frag`
  after base/detail selection, preview owner `EditorMaskCoveragePreview` behind
  `IEditorMaskCoveragePort`.

### 2.2 Not in place

- **The HLS operator does not use OkLab.** `ApplyHls` (`edit/runtime/cuda/cuda_primary_grade_pass.cu:166-213`,
  OpenCL `primary_grade.cl:115-159`, Metal `primary_grade.metal:126`) uses an HSV max/min hue of
  the ACEScc RGB values and a YIQ rotation. The only live OkLab code is the Contrast operator
  (`LinearAp1ToOkLab` / `OkLabToLinearAp1`, CUDA :84-122, OpenCL `primary_grade.cl:28-64`, Metal
  `primary_grade.metal:38-75`, reference `tests/edit/runtime/oklab_contrast_reference.hpp`). The
  OpenCL helpers in `common.cl:192-256` have no callers and use different matrices.
  So the Mask pass needs its own decode and OkLab conversion. It reuses the Contrast functions; it
  does not need a new color transform.
- `PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode` does not receive the running `scene`
  binding (`plan_executor.hpp:219`). The `range` input names a graph value, but the running
  scene can be a frame-local `SceneWorkImagePair` member (`include/edit/runtime/frame_scene_binding.hpp:25`).
- The SM8 coverage operation compiles no DAG and reads only a stored AI raster
  (`include/edit/runtime/mask_coverage_presentation.hpp:33-65`). `MaskCoverageSelection` is a
  candidate or an `AiSegmentationMaskSource` only. It cannot show content-dependent coverage.
- No two-handle range control and no continuous hue bar exist in QML. The HLS hue strip is eight
  swatches (`ui/alcedo_main/qml/EditorLookPanel.qml:675-697`).
- Mask field edits allow only `enabled`, `invert`, `opacity`, `display_name`
  (`app/editor_mask_creation_controller.cpp:90-131`, `edit/history/pipeline_edit_change_validate.cpp:230-263`).

## 3. Decisions

| ID | Decision |
| --- | --- |
| D1 | Keep the field names `color_range` / `luminance_range` and the types `ColorRangeModel` / `LuminanceRangeModel`. Renaming the JSON key would change the bytes of every saved Mask. The UI calls the Color Range "Hue Range". |
| D2 | A full range is stored as an empty optional (JSON `null`). Only a restricted range is an object. Old documents keep byte-identical JSON, root id, and history byte comparisons. No pipeline document format version change. A document with a restricted range fails to load in an older build with the existing validation error ("extra fields rejected"). |
| D3 | Evaluation space: decode the Grade input from ACEScc to linear AP1, convert with the existing `LinearAp1ToOkLab`. Luminance = OkLab `L`, clamped to [0, 1]. Hue = OkLCh `h = atan2(b, a)` in degrees [0, 360). Chroma `C = hypot(a, b)`. |
| D4 | Ranges read the owning Grade's input (`range` input), never its output, so a range never follows its own adjustment. The source shape (Radial, Linear Gradient, AI) still reads `image` as before. |
| D5 | Per-Mask coverage: `clamp(invert(source) × hue_weight × luminance_weight × opacity, 0, 1)`. Invert applies to the source only, as the model comment states. Union across Masks is unchanged. |
| D6 | Each restricted range stores its soft transition (`softness`) with a fixed default. This plan exposes only the two handles; the stored value lets a later Smoothness control change it without a format change. Defaults: luminance 0.05 (OkLab L units), hue 12 degrees. |
| D7 | Hue weight is multiplied by a chroma ramp `smoothstep(0.015, 0.045, C)` when the hue range is restricted, so near-neutral pixels (undefined hue) are not selected by a hue range. A full hue range skips the ramp. |
| D8 | The coverage overlay of a range edit is produced inside the photo render, from the R8 that `MaskEvaluate` already writes for the selected Mask. It is not a second evaluator and not a coverage-only operation (SM8 C4 does not apply: this coverage depends on photo content). |
| D9 | The overlay is a render request option (`MaskCoverageProbe{grade_id, mask_id}`), presentation state only. It does not change the photo DAG key, exported pixels, scopes, or history (SM8 C10). |
| D10 | The probe publishes for the base render and for the detail ROI render, so the zoomed overlay has the detail resolution. The composite gets a detail coverage binding selected with the same ROI test as the detail photo. |
| D11 | One coverage source at a time (SM8 C8): selecting the range probe clears an AI candidate/committed preview and the reverse. While the AI Mask tool edits a new candidate, its range bars are enabled (#604, replaces the earlier rule that disabled them): the ranges are kept by the AI Mask edit, the probe of the candidate covers the candidate, and Confirm writes them with the new Mask; see section 11. MR4 (user decision): during the AI Mask edit of a stored Mask, the probe of that Mask covers the candidate and its ranges are editable; see the MR4 completion record. |
| D12 | Overlay visibility: shown while a range handle is dragged, and while the "Show selection" toggle of the range section is on. Starting any adjustment input hides it (master plan 10.4). |

## 4. Model and persistence (MR1)

```cpp
struct LuminanceRangeModel {
  float low      = 0.0f;   // OkLab L, [0, 1]
  float high     = 1.0f;   // OkLab L, [0, 1], low < high
  float softness = 0.05f;  // (0, 0.5]
};

struct ColorRangeModel {     // UI: Hue Range
  float hue_start = 0.0f;    // degrees [0, 360)
  float hue_end   = 360.0f;  // degrees (0, 360]; hue_start > hue_end wraps through 0
  float softness  = 12.0f;   // degrees (0, 90]
};
```

- `bool enabled` is removed. An object means "restricted"; `null` means full.
- `MaskModelFromJson` accepts `null`, the legacy `{"enabled": false}` object (read as empty), and
  the new object. Any other key or `"enabled": true` fails as today.
- A range equal to the full range after normalization is stored as empty (D2). The UI writes
  empty when both handles reach the ends.
- Validation: finite values, the bounds above, nonzero selected width (`high - low >= 0.01`,
  circular hue width >= 1 degree).
- Plan hash and result content key mix the range values, not only `has_value()`.
  (Changed in MR1: the plan hash keeps `has_value()` only; see the completion record.)
- Mask field edit keys `luminance_range` and `color_range`: value is the range object or `null`.
  Add them to `MaskFieldEditKeyIsValid`, `ValidateMaskFieldValue`, `ApplyLiveMaskField`,
  `CurrentMaskField` / `ApplySetMaskField`, and `EditorHistoryMutation::SetMaskField`.
- Remove the hard-coded `"color_range": nullptr` / `"luminance_range": nullptr` copies in
  `editor_mask_creation_controller.cpp:73-74`, `pipeline_history_applier.cpp:415-416`, and
  `pipeline_edit_json.cpp:280-282`; serialize through `MaskModelToJson` / the range helpers.

Tests: JSON round trip; an unedited Mask serializes to the same bytes as before (stored
`expected_mask_json_without_ranges.json` from the current serializer); legacy `{"enabled": false}`
loads as empty; invalid values fail; one drag gives one history commit; Undo/Redo, Version, Paste,
and reopen restore the range; full-range edit stores `null`.

##### MR1 completion record (2026-10-26)

**Status:** complete. Model, JSON, validation, content key, Mask field edit keys, and history for
both ranges. Work is on branch `feature/mask-ranges-mr1-model` (from `main` `18c86370`), not
committed. Issue [#548](https://github.com/zidage/AlcedoStudio/issues/548).

**Changes from the plan and details that the plan did not give:**

- **Plan hash keeps `has_value()` only (changed).** `HashGraphTopology`
  (`edit/runtime/graph_compiler.cpp`) is the plan topology key: `AlignParameterLayout` is a no-op
  for the same hash, so parameter edits keep their offsets. Opacity and source values are not in
  it either. If range values went into it, each frame of a range drag would compile a new plan and
  a new parameter layout. Range presence stays in the topology hash (whether a Mask reads the
  `range` input is plan structure). Range values go into the Mask result key
  (`MixMaskSourceModel`, `edit/runtime/result_content_key.cpp`). Test:
  `MaskRangeValuesInvalidateMaskKeyButKeepThePlan`.
- **JSON keys.** Luminance: `{"high", "low", "softness"}`. Hue: `{"hue_end", "hue_start",
  "softness"}`. All three are required; other keys fail. The legacy `{"enabled": false}` reads as
  empty; `"enabled": true`, a non-boolean `enabled`, or `enabled` with another key fails.
  Production code never wrote the legacy object (only tests built `{false}`), so no stored
  document changes bytes.
- **One canonical form.** `ValidateColorRange` / `ValidateLuminanceRange` reject a stored full
  range, so a Mask holds an object only for a restricted range. `ColorRangeFromJson` /
  `LuminanceRangeFromJson` read a valid full-range object as empty. A history batch must hold the
  serializer form (`CanonicalColorRangeJson` / `CanonicalLuminanceRangeJson`,
  `edit/history/pipeline_edit_json.cpp`): a full-range object, the legacy object, or a number in
  double precision fails batch validation. The two writers that take values from the UI
  (`EditorMaskCreationController::ApplyMaskFieldValue`, `EditorHistoryMutation::SetMaskField`)
  read and write the value back first, so a full-range edit stores `null` and the stored numbers
  have float precision (the Undo comparison of `CurrentMaskField` is exact).
- **Hue width.** `hue_start == hue_end` is width 0 and fails; it does not mean the full circle.
  The only full hue range is `[0, 360]`. Width uses float arithmetic: `high - low >= 0.01` and
  hue width `>= 1` degree are checked on the float values, so MR4 must keep its minimum handle
  distance a little above these limits.
- **Probe Masks removed.** The three hard-coded `"color_range": nullptr` probe Masks only parsed a
  source. They now call the new public `MaskSourceFromJson` / `MaskSourceToJson` /
  `ValidateMaskSource` (`edit/mask/mask_model.hpp`): `editor_mask_creation_controller.cpp`
  (`RestoreLive`), `pipeline_history_applier.cpp` (ReplaceMaskSource apply), and
  `pipeline_edit_json.cpp` (`CanonicalMaskSourceJson`).
- **Owner writes.** New `ColorGradeNodeModel::SetMaskColorRange` / `SetMaskLuminanceRange`
  (content writes through `StoreMask`, new content revision, same-value write is a no-op).
- **Stored expected bytes.** No new `expected_mask_json_without_ranges.json` file. The existing
  `ExistingMaskJsonBytesAreUnchanged` holds the bytes that the `main` serializer writes for a
  Radial and a Linear Gradient Mask without ranges, and `pipeline_document_multi_grade.json`,
  `pipeline_root.json`, and `pipeline_checkpoint.json` hold whole documents with a Mask. All still
  pass unchanged.

**Primary success call chain (range drag):**

```text
BeginMaskFieldEdit("luminance_range")            EditorMaskCreationController, Selected Mask
  -> ApplyMaskFieldValue(key, {low, high, softness})  per drag step
     -> MaskFieldValueIsValid -> LuminanceRangeFromJson (full object -> empty)
     -> ApplyLiveMaskField -> ColorGradeNodeModel::SetMaskLuminanceRange -> StoreMask
        (ValidateMaskModel, new content revision) -> RequestInteractive
  -> FinishMaskInput -> PublishMaskFieldEdit
     -> MaskFieldValueJson (LuminanceRangeToJson) before/after
     -> MakeSetMaskFieldBatch -> PipelineEditBatch::Make
        -> ValidateMaskFieldValue -> CanonicalLuminanceRangeJson
     -> PublishSettledBatch -> one history commit, Quality render
Undo / Redo
  -> ApplyPipelineEditBatch -> ApplySetMaskField
     -> CurrentMaskField == expected side -> SetMaskLuminanceRange(LuminanceRangeFromJson(next))
Reopen / Version / Paste
  -> MaskModelFromJson -> LuminanceRangeFromJson / ColorRangeFromJson
Render cache
  -> MixMaskSourceModel mixes the range values -> new Mask and Grade result keys, same plan
```

**Primary failure call chain:**

```text
Invalid value (bounds, NaN, width, softness, extra key, enabled: true)
  -> controller: MaskFieldValueIsValid false -> Reject, no live change, no commit
  -> EditorHistoryMutation::SetMaskField: FromJson throws -> false + error, no commit
  -> stored batch: ValidateMaskFieldValue throws -> batch rejected
  -> document load: MaskModelFromJson throws -> document rejected
Live apply succeeded but the history publish failed
  -> ApplyLiveMaskField(before) or RestoreLive -> Mask returns to the before value
```

**What was proven (executed tests):**

| Required criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| JSON round trip, values in serialized form | `MaskListRoundTripPreservesSourcesOrderAndRangeFields` | `GpuDagModelGraphTest` | PASS |
| Unedited Mask bytes unchanged | `ExistingMaskJsonBytesAreUnchanged` (unchanged test) | `GpuDagModelGraphTest` | PASS |
| Legacy object reads as empty; key and type rules; full object reads as empty | `RangeJsonReadsNullLegacyPlaceholderAndRangeObjects` | `GpuDagModelGraphTest` | PASS |
| Invalid values fail before mutation | `InvalidRangeValuesFailBeforeDocumentMutation` | `GpuDagModelGraphTest` | PASS |
| Owner setters, content revision | `RangeSettersStoreRangeAndTakeNewContentRevision` | `GpuDagModelGraphTest` | PASS |
| Batch accepts only canonical range values | `MaskRangeFieldsAcceptOnlyCanonicalRangeValues` | `PipelineEditBatchTest` | PASS |
| One drag gives one commit; Undo/Redo restore | `LuminanceRangeDragCommitsOnceAndUndoRedoRestoreTheRange` | `AnalyticMaskCreationTest` | PASS |
| Full-range edit stores `null`; wrapping hue range | `FullHueRangeEditStoresNullAndUndoRestoresTheRange` | `AnalyticMaskCreationTest` | PASS |
| Invalid controller value: no change, no history | `InvalidRangeValueIsRejectedWithoutHistoryOrMaskChange` | `AnalyticMaskCreationTest` | PASS |
| `EditorHistoryMutation::SetMaskField` keys, render reason, full -> `null`, Undo/Redo | `MaskRangeFieldsCommitRenderAndRoundTripAcrossUndoAndRedo` | `EditorSessionHistoryPortTest` | PASS |
| Reopen (root state, root id) and Version checkpoint restore | `RootAndCheckpointRoundTripRestoreMaskRanges` | `PipelineDocumentCheckpointTest` | PASS |
| Paste restores the ranges | `PasteCarriesMaskLuminanceAndHueRanges` | `DocumentTransferTest` | PASS |
| Content key mixes values; plan key unchanged | `MaskRangeValuesInvalidateMaskKeyButKeepThePlan` | `GpuDagRawInputTest` | PASS |

Commands (Windows, `win_debug`, `build/debug`):
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest
PipelineEditBatchTest PipelineDocumentCheckpointTest AnalyticMaskCreationTest DocumentTransferTest
GpuDagRawInputTest EditorSessionHistoryPortTest AlbumBackendLib`, then each test binary run
directly from its `*_runtime` directory.

Suite totals: `GpuDagModelGraphTest` 147/147, `PipelineEditBatchTest` 22/22,
`AnalyticMaskCreationTest` 30/30, `GpuDagRawInputTest` 197/197, `PipelineDocumentCheckpointTest`
18/20, `DocumentTransferTest` 34/35, `EditorSessionHistoryPortTest` 115/116. The 4 failures also
fail on clean `main` `18c86370` (checked by stash and rebuild): #553
(`ExistingRawRoot...`, `ExistingLmtRoot...`), #554
(`TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches`), #555
(`CheckoutAndPasteHashesAreUnchanged`). The full suite was not run. `AlbumBackendLib` builds;
`alcedo_main` was not linked and the app was not started (MR1 has no UI).

**Checklist / exit condition:** all section 4 items done, with the plan hash change above.

**LOC note:** `editor_mask_creation_controller.cpp` is 1113 lines (+58 in MR1) and
`analytic_mask_creation_test.cpp` 1206 lines (+125); both were above 1000 before MR1. No split
in MR1.

**Residual gaps:** a restricted range does not change coverage or pixels yet; no kernel reads the
ranges (MR2). A range commit uses the one `history.operation.set_mask_field` presentation key, as
opacity and invert do.

## 5. GPU evaluation (MR2)

- `PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode` receives the running `scene` binding
  of the plan executor. `plan_executor.hpp` passes it; the encoder resolves cached, work, or
  display images the same way as `PrimaryColorGrade`.
- One kernel per backend computes source coverage, then, only when a range is restricted, reads the
  scene pixel, decodes ACEScc, converts to OkLab, and multiplies the weights before invert,
  opacity, and R8 quantization. Masks without restricted ranges do not read the scene image.
- Weights:

  ```text
  luminance_weight = smoothstep(low - s, low, L) × (1 - smoothstep(high, high + s, L))
  d                = signed circular distance of h outside [hue_start, hue_end] (0 inside)
  hue_weight       = (1 - smoothstep(0, s, d)) × smoothstep(0.015, 0.045, C)
  ```

  A range that touches an end of the luminance bar has no falloff on that side (L below 0 or
  above 1 stays selected).
- Move `LinearAp1ToOkLab` and the ACEScc decode into a per-backend shared header (CUDA `.cuh`,
  OpenCL `.cl` include, Metal header) so Contrast and Mask call the same function. Delete the
  unused OpenCL helpers in `common.cl:192-256`.
- Mask result key: when a range is restricted, the key keeps the Grade input scene key (already
  present). A change of an upstream Grade invalidates the Mask, as now.

Tests: a CPU reference in `tests/edit/runtime/` (same equations, double precision); CUDA, OpenCL,
and Metal R8 output equals the reference within 1 code value on a synthetic ACEScc ramp + hue
wheel input; wrap-around hue range; full range output is byte-equal to the current kernel output;
two Grades where Grade 1 shifts hue, and the Grade 2 hue range follows Grade 1 output, not the
source.

##### MR2 completion record (2026-10-26)

**Status:** complete. `MaskEvaluate` multiplies the source coverage by the Luminance Range and Hue
Range weights of the owning Grade's input on CUDA, OpenCL, and Metal. Branch
`feature/mask-ranges-mr2-gpu-eval`, stacked on MR1 (PR #556). Issue
[#549](https://github.com/zidage/AlcedoStudio/issues/549).

**Changes from the plan and details that the plan did not give:**

- **Separate ranged kernel entry points.** A Mask with a restricted range dispatches a ranged
  kernel: CUDA `AnalyticMaskKernel<true>` / `RasterMaskKernel<true>`, OpenCL
  `mask_analytic_ranged_r8` / `mask_raster_ranged_r8`, Metal `mask_analytic_ranged` /
  `mask_raster_ranged`. Each computes the source coverage and the range weights in one dispatch
  (the plan's "one kernel"). A Mask without ranges dispatches the existing kernel, so it binds and
  reads no scene image, and its output stays byte-identical (checked by a byte dump, below).
  The source coverage code is shared by both entry points (`AnalyticSourceCoverage`,
  `RasterMaskMean`).
- **Packed parameters.** `MaskRangeParams` (`include/edit/mask/mask_model.hpp`, next to
  `RadialMaskParams`) holds both ranges in eight 4-byte fields with a `*_restricted` flag each.
  CUDA takes it by value; `mask.cl` and `mask.metal` declare the same layout, and the hosts check
  `sizeof == 32`. `MaskRangeParamsFromMask` and `MaskHasRestrictedRange` read the live
  `MaskModel`. The equations are documented at `MaskRangeParams`.
- **Weight details.** `weight = hue_weight * luminance_weight` (each 1 when its range is
  empty), then `clamp(invert(source) * weight * opacity, 0, 1)`. The end-of-bar rule (no falloff
  at `low = 0` or `high = 1`) follows from the clamp of L to [0, 1]; there is no special case.
  Hue distance: `offset = (h - hue_start) mod 360`; inside when `offset <= width` (width as
  `ColorRangeWidthDegrees`), else `d = min(offset - width, 360 - offset)`. Chroma is
  `sqrt(a * a + b * b)` on every backend; Metal uses `precise::atan2` and `precise::sqrt`.
- **Scene binding.** `PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode` and the per-source
  `Execute*Mask` take the running `FrameSceneBinding` (cached image or scene-work member). The
  backends resolve it with `CudaSceneTexture` / `OpenClBindScene` / `MetalSceneTexture` and throw
  when a ranged Mask has no RGBA32F image of the render extent. The Grade-level and document-level
  `Execute*Mask` overloads (tests only) read `CompiledMaskSource::range_input` as a cached image,
  so a range on a later Grade needs the plan executor path.
- **Shared OkLab code.** `LinearAp1ToOkLab`, `OkLabToLinearAp1`, `AcesccToLinearAp1`, and
  `LinearAp1ToAcescc` moved to `edit/runtime/cuda/cuda_oklab_math.cuh`,
  `edit/runtime/opencl/shader/oklab.cl` (listed in the primary-grade and mask program manifests
  after `color_encoding_math.h`), and `edit/runtime/metal/shader/oklab_math.h`. Contrast and the
  Mask ranges call the same functions. The whole HLS helper block of `common.cl` is deleted
  (lines 192-285: also `opencl_fit_ap1_lower_gamut` and `opencl_hue2rgb`, which have no callers
  either; only the deprecated legacy pipeline has its own copies).
- **No implementation version change.** `kMaskImplementationVersion` stays 3: Masks without
  ranges write the same bytes, and the range values are in the Mask result key since MR1. The
  key already mixes the Grade input scene key, so an upstream Grade edit invalidates a ranged Mask.
- **Warm-up.** OpenCL and Metal warm the analytic ranged kernel with the other mask kernels.
- **Test input.** The colored ramp of the test scene has constant saturation (chroma
  `min(0.1 L, 0.08)`). With fixed chroma 0.08 at L = 0.009, CUDA and OpenCL agreed with each other
  but differed from the double reference by up to 123 codes: the hue of a dark, strongly
  saturated color comes from the cube root of a near-zero LMS sum and is not stable in float. Such
  pixels are below the chroma ramp in real images.
- **Two-Grade test.** The test reads the Grade 1 output from a render of Grade 1 alone: in a
  two-Grade render the DiffusionFilter writes into the free scene-work member, which is the
  Grade 1 output (#496).

**Primary success call chain (render of a Mask with a range):**

```text
PlanExecutor<Backend>::Execute                      plan_executor.hpp, per Grade in backbone order
  -> scene = running FrameSceneBinding (Develop cached image, or the previous Grade's work member)
  -> BindOrMiss(effective_output)                   Mask result key: Grade input key + range values
  -> PassEncoder<Backend, MaskEvaluate>::Encode(..., source, scene)
     -> Execute{Cuda,OpenCl,Metal}Mask(..., range_scene = scene)
        -> RequireMaskModel -> MaskHasRestrictedRange
        -> Range scene resolve (RGBA32F, render extent)
        -> ranged kernel: source coverage -> ACEScc decode -> LinearAp1ToOkLab
           -> MaskRangeWeight -> invert * weight * opacity -> clamp -> R8 effective_output
  -> MaskUnion -> PrimaryColorGrade(scene)          the Grade mixes by the Union coverage
```

**Primary failure call chain:**

```text
Ranged Mask without a usable Grade input (missing binding, wrong format or extent)
  -> Range scene resolve throws std::runtime_error naming the Mask range
  -> no kernel dispatch; the render fails with that error (no unranged substitute)
Mask without ranges
  -> the existing kernel; the scene binding is not resolved, so a missing image does not fail
```

**What was proven (executed tests):**

| Required criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| R8 equals the double reference within 1 code on an ACEScc ramp and hue wheel (Luminance only, Hue only, both on an inverted Radial with opacity) | `LuminanceAndHueRangesMatchTheDoublePrecisionReference` | `GpuDagCudaMaskTest`, `GpuDagOpenClGradeTest`, `GpuDagMetalGradeTest` | PASS (3 backends) |
| Wrap-around hue range | `WrappingHueRangeSelectsHuesOnBothSidesOfZero` | same | PASS (3 backends) |
| Full-range output byte-equal to the current kernel | `FullySelectingRangeMatchesTheMaskWithoutRangesByteForByte` (a range with weight exactly 1 writes the bytes of the Mask without ranges); byte dump below | same | PASS (3 backends) |
| Masks without a restricted range do not read the scene image | `OnlyRangedMasksReadTheGradeInput` | same | PASS (3 backends) |
| Raster (AI) Mask with ranges | `AiMaskRangeMatchesReference` | same | PASS (3 backends) |
| Grade 2 hue range follows Grade 1 output, not the source | `SecondGradeHueRangeReadsFirstGradeOutput` (fails when the executor passes the Develop image: checked on CUDA) | same | PASS (3 backends) |
| Contrast unchanged after the OkLab move | `MetalContrastMatchesOkLabLightnessReferenceForEveryPixel` and the CUDA/OpenCL Contrast tests | `GpuDagMetalGradeTest`, `GpuDagCudaPrimaryGradeTest`, `GpuDagOpenClGradeTest` | PASS |

Byte dump (one-time, not committed): a Radial (inverted, opacity 0.8), a Linear Gradient
(opacity 0.65), and an AI Mask (inverted, opacity 0.7), each at render scale 1 and 0.5, written by
`ea55087e` and by MR2. CUDA and OpenCL: 12 of 12 files are byte-identical. Metal was not dumped.

Commands (Windows, `win_debug`, `build/debug`):
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 6 --target GpuDagCudaMaskTest
GpuDagOpenClGradeTest GpuDagCudaPrimaryGradeTest GpuDagModelGraphTest GpuDagRawInputTest
OkLabContrastReferenceTest AlbumBackendLib`, then each binary from its `*_runtime` directory.
macOS (Apple silicon, `build/macos-debug`): `cmake --build build/macos-debug --target
GpuDagMetalGradeTest -j 8`, then the binary.

Suite totals: `GpuDagCudaMaskTest` 29/29, `GpuDagOpenClGradeTest` 86/92,
`GpuDagCudaPrimaryGradeTest` 75/85, `GpuDagModelGraphTest` 147/147, `GpuDagRawInputTest` 197/197,
`OkLabContrastReferenceTest` 12/12, `GpuDagMetalGradeTest` 71/82. Every failure also fails without
MR2: the six OpenCL, six CUDA, and six Metal multi-Grade tests, the two CUDA scene-work pair tests,
and the CUDA two-LUT timing test are #496 (CUDA checked at `ea55087e` by stash and rebuild); the
CUDA Neighbor Grade stored-pixel test is #557; the five Metal LLF and Mask Mix tests are #497.

**Checklist / exit condition:** all section 5 items done.

**LOC note:** the largest changed files are `metal_mask_test.cpp` 867, `mask_model.cpp` 777, and
`opencl_mask_test.cpp` 734 lines; all are below 1000. The shared checks are in
`tests/edit/runtime/mask_range_test_support.hpp` (303) and the reference in
`mask_range_reference.hpp` (232).

**Residual gaps:** Metal unranged output was not byte-dumped against the old kernel (the
fully selecting range test is the Metal evidence). No UI exposes the ranges yet (MR4), and the
viewport shows no range coverage (MR3).

## 6. Node-scoped coverage overlay (MR3)

- `MaskCoverageProbe{NodeId grade_id; MaskId mask_id;}` is an optional field of the interactive
  render request. It is not part of the DAG key.
- In `plan_executor.hpp`, after the probed Mask's `effective_output` is bound (executed or cache
  hit), the executor records a display write: R8 on the render grid to the RGBA32F coverage
  display target (red = coverage). This is a new small per-backend kernel
  `RecordMaskValueDisplay` (same grid, no resampling), next to `RecordMaskCoverageDisplay`.
- The frame presenter publishes it through `IMaskCoverageSink` with the geometry of the same
  render, together with the photo frame. Base and detail renders publish to two coverage slots
  (`LeaseFrameLayer::MaskCoverage` base and a new detail slot) (D10).
- `editor_viewport.frag` gets `detailCoverageTexture` (binding 4). Inside the detail ROI it samples
  the detail coverage; elsewhere the base coverage. The tint equation does not change.
- `EditorMaskCoveragePreview` gets the source kind `RangeProbe(grade_id, mask_id)`. The facade
  adds `ShowMaskRangeCoverage(grade_id, mask_id)` / `HideMaskRangeCoverage()`. While the probe is
  active, every Interactive and Quality render request of the editor carries it; the coverage
  updates with each frame, so a range drag shows the selection live.
- Hide clears the coverage layer without a photo render.

Tests: the presented coverage equals the probed Mask R8 for CUDA, OpenCL, and Metal; the probe
does not change the photo frame bytes or the DAG key; the coverage of Grade 2 is computed from
Grade 1 output; detail ROI coverage samples the detail slot; AI preview and probe replace each
other.


##### MR3 completion record (2026-10-27)

**Status:** complete. The viewport shows the coverage of one probed Mask of one Color Grade,
taken from the photo render, on CUDA, OpenCL, and Metal. Issue
[#550](https://github.com/zidage/AlcedoStudio/issues/550). Three stacked pull requests on MR2:
runtime probe (branch `feature/mask-ranges-mr3-coverage-probe`), detail coverage channel
(`feature/mask-ranges-mr3-detail-coverage`), session facade (`feature/mask-ranges-mr3-session-probe`).

**Changes from the plan and details that the plan did not give:**

- **Probe record, not workspace state.** `PlanExecutor::Execute` (and both device `Execute`
  functions) take an optional `MaskCoverageProbeRecord<Backend>*`. The executor records
  `RecordMaskValueDisplay` right after the probed Mask's `effective_output` is bound (executed or a
  valid result) and sets `record.target`. The record lives for one render call; the probe is not
  stored on the workspace or the renderer. `BasicRenderWorkspace::AcquireMaskCoverageDisplayTarget`
  now also accepts an open render, not only coverage-only work.
- **Header split.** `edit/runtime/mask_coverage_probe.hpp` holds only `MaskCoverageProbe` and
  `MaskCoverageProbeOutcome` (the app layer and `PipelineApplyRequest` include it).
  `mask_coverage_probe_record.hpp` holds the record and `PublishMaskCoverageProbe`.
- **Publication order and failure.** `Renderer::RenderImage` publishes the probed coverage after
  the photo frame, the host download, and the result publication, with the identity
  (`presentation_request_id`, session epoch, image identity) and the geometry of the photo frame.
  `RenderedPipelineImage::mask_coverage` reports `Published`, `NotEvaluated`, or
  `DiscardedForImageChange`. A failed coverage publication fails the render with the real error;
  the photo frame and its results are already published then.
- **Mask not evaluated.** When the render does not evaluate the probed Mask (the Mask or its Grade
  is disabled, the Mask is removed), the renderer calls `IMaskCoverageSink::ClearCoverage`, so no
  older coverage stays displayed (`NotEvaluated`). The render does not fail.
- **Value display kernels.** CUDA `MaskValueDisplayKernel`, OpenCL `mask_value_display_rgba32f`,
  Metal `mask_value_display` (warmed with the mask kernels): red = R8 code / 255. OpenCL and Metal
  quantize the read value again (`QuantizeR8`). The OpenCL division is not correctly rounded, so a
  presented value can differ from `code / 255.0f` by 1 ulp; the tests compare R8 codes (same code,
  within 1e-6).
- **Detail slot = second coverage channel.** The detail slot is a second
  `DirectPresentQueue` + `ViewportMaskCoverageSink` + `ViewportMaskCoverageLayer` of the viewport
  (`detailMaskCoverageSink()`), with the lease layer label `LeaseFrameLayer::DetailMaskCoverage`.
  It follows the same session, suspension, invalidation, and teardown rules as the base channel;
  `EditorViewportItem::clearMaskCoverage` clears both.
- **Composite rule inside the detail ROI.** `editor_viewport.frag` binding 4 samples the detail
  coverage frame only inside a visible DetailPatch ROI and only where that frame has texels;
  elsewhere it samples the base coverage frame. `ViewportCompositeUniforms` has
  `detail_coverage_u/v` (photo UV to detail coverage UV through the reference space, as the base
  map). Without a visible DetailPatch the detail frame is not used.
- **Stale detail coverage.** A base (non-detail) frame with a probe clears the detail channel on
  the render worker before it renders (port `prepare_`), so a detail coverage frame of an earlier
  probe or document state is not composited with a newer base frame. Inside the ROI the base
  coverage shows until the next detail frame. A coverage-only AI job also clears the detail
  channel first (AI coverage has no detail frame). The port `ClearMaskCoverage` clears both
  channels.
- **Session probe source.** `EditorMaskCoveragePreview` has the source `RangeProbe` and publishes
  the probe in `EditorMaskCoveragePreviewState::range_probe`. `EditorSessionRenderController`
  reads it through `SetMaskCoverageProbeSource` each time it builds a render intent, so every
  Interactive, Quality, and Detail request carries it (`EditorRenderIntent::mask_coverage_probe`).
  The port copies it into `RenderDesc` with the coverage sink of the frame role
  (`SetDetailMaskCoverageSinkResolver` for detail frames) and `PipelineTask::MakeApplyRequest`
  into `PipelineApplyRequest` for interactive renders. Without a bound channel the frame renders
  without the probe.
- **Facade.** `ShowMaskRangeCoverage(grade_id, mask_id)` accepts any Mask kind of the working
  document, refuses during an AI Mask edit and without an interactive session, and routes one
  Quality render with the new reason `MaskCoverageProbeChanged` (no scope update) when the probe
  changes. `HideMaskRangeCoverage()` clears the channels through the port without a render.
  Status is `Current` while the probe is shown: each frame carries the probe, and a failed
  coverage publication fails that frame.
- **One source at a time (D11).** Showing the probe ends a committed AI preview (its jobs end as
  Discarded; the clear runs after them on the worker). A committed AI preview or a candidate
  replaces the probe. Opening an AI Mask edit ends the probe. An image change ends it
  (`EditorMaskCoveragePreview::Clear`).
- **Metal coverage publication after a completed render.**
  `MetalFramePresenter::PresentMaskCoverage` required a recorded command buffer. After the photo
  download (and the `WaitIdle` of a QualityBase frame) the command context has no buffer, so the
  probe publication threw. The presenter now throws only for a missing target texture and waits for
  the buffer only when one exists: no buffer means the recorded work is complete.
- **Tests reuse.** The SM8.6 composite fixture moved to
  `tests/ui/viewport_coverage_composite_test_support.hpp`; the coverage harness has a detail layer
  and `RenderPhotoWithProbe` (the order of `Renderer::RenderImage`). The SM8.7 session fixture
  moved to `tests/app/editor_mask_coverage_preview_test_support.hpp`.

**Primary success call chain (range probe shown, one frame):**

```text
EditorSessionService::ShowMaskRangeCoverage           owner thread, validates Grade and Mask
  -> EditorMaskCoveragePreview::ShowRangeProbe         source RangeProbe, state.range_probe
  -> EditorSessionRenderController::RouteViewChange(MaskCoverageProbeChanged)
     -> MakeRenderIntent: intent.mask_coverage_probe = probe source()
EditorSessionRenderSchedulerPort::DispatchPipelineFrame
  -> coverage sink of the frame role (base or detail resolver); RenderDesc probe + sink
  -> prepare_ (worker): base frame clears the detail channel
PipelineTask::MakeApplyRequest -> PipelineApplyRequest{mask_coverage_probe, mask_coverage_sink}
Renderer<Backend>::RenderImage
  -> device.Execute(..., &MaskCoverageProbeRecord)
     -> PlanExecutor: probed Mask effective_output bound
        -> AcquireMaskCoverageDisplayTarget(render extent) -> RecordMaskValueDisplay
  -> FramePresenter::Present (photo) -> PublishResults
  -> PublishMaskCoverageProbe -> FramePresenter::PresentMaskCoverage(target, identity, geometry)
ViewportMaskCoverageLayer (base or detail) -> ViewportCompositePass (binding 3 or 4)
```

**Primary failure call chains:**

```text
Probed Mask not evaluated (disabled, removed, Grade disabled)
  -> record.target == nullptr -> IMaskCoverageSink::ClearCoverage -> outcome NotEvaluated
Coverage publication fails (no writable target, copy failure)
  -> std::runtime_error from FramePresenter::PresentMaskCoverage -> RenderImage reports and
     rethrows; photo frame and results already published; the frame job reports the error
Show during an AI Mask edit / missing Grade or Mask / no interactive session
  -> Reject with the reason; preview and render requests unchanged
```

**What was proven (executed tests):**

| Required criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Presented coverage equals the probed Mask R8 | `PresentedCoverageEqualsTheProbedMaskR8` | `MaskRangeCoverageProbeTest` | CUDA, OpenCL: PASS; Metal: PASS |
| Photo bytes and result keys unchanged by the probe | `ProbeChangesNoPhotoPixelAndNoResultKey` (byte-identical display output; the unprobed render after a probed one skips every Mask and DRT pass; a probe of valid results still publishes) | same | CUDA, OpenCL: PASS; Metal: PASS |
| Grade 2 coverage computed from Grade 1 output | `SecondGradeCoverageIsComputedFromFirstGradeOutput` | same | CUDA, OpenCL: PASS; Metal: PASS |
| Detail ROI render publishes to the detail slot | `DetailRoiRenderPublishesToTheDetailChannel` | same | CUDA, OpenCL: PASS; Metal: PASS |
| Probe of a Mask that the render skips clears the coverage | `ProbeOfAMaskThatTheRenderSkipsClearsTheCoverage` | same | CUDA, OpenCL: PASS; Metal: PASS |
| Detail ROI samples the detail slot | `DetailCoverageIsSampledInsideTheDetailRoi` | `ViewportCoverageCompositeTest` | CUDA, OpenCL: PASS; Metal: PASS |
| Probe through the production port and renderer, base and detail channel | `FrameWithAMaskCoverageProbePresentsTheMaskToTheChannelOfItsRole` | `EditorSessionRenderSchedulerPortTest` | CUDA: PASS |
| Every frame carries the probe; Hide clears without a render | `ProbeRidesOnEveryEditorFrameUntilHidden` | `EditorMaskCoveragePreviewTest` | PASS |
| AI preview and probe replace each other | `ProbeAndAiCoveragePreviewReplaceEachOther` | same | PASS |
| Missing Mask or Grade refused; image change ends the probe | `ProbeOfAMissingMaskOrGradeIsRefused`, `ImageChangeEndsTheProbe` | same | PASS |

Regression suites (Windows, `win_debug`): `ViewportCoverageCompositeTest` 9/9 and
`MaskCoveragePresentationTest` 7/7 on CUDA and OpenCL, `MaskRangeCoverageProbeTest` 5/5 on CUDA
and OpenCL, `EditorMaskCoveragePreviewTest` 16/16, `EditorSessionRenderSchedulerPortTest` 44/44,
`EditorRenderCoordinatorTest` 33/33, `EditorSessionRenderControllerTest` 21/21,
`EditorSessionServiceFacadeTest` 4/4, `EditorComparisonServiceTest` 22/22,
`EditorViewportSuspensionTest` 3/3. macOS (`macos-debug`, Apple M4): `MaskRangeCoverageProbeTest`
5/5, `ViewportCoverageCompositeTest` 9/9, `MaskCoveragePresentationTest` 7/7, `GpuDagMetalGradeTest`
71/82 (the 11 failures are pre-existing: #496, #497).

Commands (Windows, `build/debug`): `cmd /c scripts\msvc_env.cmd --build --preset win_debug
--parallel 6 --target MaskRangeCoverageProbeTest ViewportCoverageCompositeTest
MaskCoveragePresentationTest EditorViewportSuspensionTest EditorMaskCoveragePreviewTest
EditorSessionRenderSchedulerPortTest EditorRenderCoordinatorTest EditorSessionRenderControllerTest
EditorSessionServiceFacadeTest EditorComparisonServiceTest alcedo_main`, then each binary from its
`*_runtime` directory (`ALCEDO_TEST_EDITOR_BACKEND=cuda` or `opencl` for the coverage binaries).

**Checklist / exit condition:** all section 6 items done.

**Residual gaps:** no UI calls the facade yet (MR4). The detail channel shows the base coverage
inside the ROI after a base frame until the next detail frame. On Windows, the OpenCL runs of the
coverage harness binaries sometimes do not exit after all tests pass; this is pre-existing (#559).

## 7. UI (MR4)

- New reusable `RangeSelectorBar.qml`: a track with a gradient or image fill, two handles, the
  selected interval framed, outside dimmed. Drag a handle, drag the interval body to move both,
  double-click to reset to full. `wraps: true` lets the hue handles cross 0/360 and draws the
  wrapped interval in two parts.
- New `EditorMaskRangeSection.qml`: "Luminance" bar (black-to-white) and "Hue" bar, plus the
  "Show selection" toggle. It is placed at the top of `EditorMasksContextPanel.qml` (above the
  name field) and of `EditorAiMaskPanel.qml` (above the task row; disabled for a new candidate,
  D11).
- The hue bar fill comes from C++: `EditorMaskCreationAdapter` exposes gradient stops computed
  from OkLCh at fixed `L = 0.75, C = 0.12`, converted to display sRGB and clipped, so a hue
  position on the bar is the hue that the kernel tests.
- Adapter properties: `maskLuminanceLow/High`, `maskHueStart/End`, `maskRangeCoverageVisible`;
  invokables `beginRangeEdit(kind)`, `updateRangeEdit(kind, a, b)`, `finishRangeEdit(kind)`,
  `resetRange(kind)`, `setRangeCoverageVisible(bool)`. They enqueue the MR1 Mask field commands
  and call the MR3 facade.
- Strings in `.ts` files are added by hand (no `lupdate` run), with Simplified Chinese
  translations: 明度范围, 色相范围, 显示选区.

Tests: adapter tests through the C++ owner (drag sequence produces one commit; reset writes
`null`; the probe is shown during drag and hidden on adjustment input). QML harness tests are not
used as evidence (AGENTS.md, Qt UI/QML tests).

##### MR4 completion record (2026-10-27)

**Status:** complete. Issue [#551](https://github.com/zidage/AlcedoStudio/issues/551). Branch
`feature/mask-ranges-mr4-range-bars`, stacked on the MR3 session branch
`feature/mask-ranges-mr3-session-probe`.

**Changes from the plan and details that the plan did not give:**

- **Stored AI Mask ranges are editable during its AI Mask edit (user decision, 2026-10-27).** An
  AI Mask row opens the AI Mask edit, and SM9 refused every Mask command and (MR3) the range probe
  during that edit, so D11 alone left AI Masks without range bars. The user chose to make them
  editable. During the AI Mask edit of a stored Mask, the session admits Cancel and the range edit
  commands (BeginMaskField and SetMaskField of `luminance_range` / `color_range`, and Finish);
  the owner rejects them for any other Mask (`EditorSessionService::ApplyMaskCreationCommand`).
  `EditorSessionController::EnqueueMaskCreation` passes them although `can_edit()` is false (the
  AI Mask edit lease holds CommitAdjustment). One shared predicate decides this:
  `IsMaskRangeEditCommand` (`app/editor_mask_creation_controller.hpp`). The AI Mask commit
  replaces only the source, so a range committed during the edit stays.
- **Range probe over the candidate.** `ShowMaskRangeCoverage` accepts the stored Mask of the open
  AI Mask edit (no other Mask, and not while the candidate is in the Previous status).
  `EditorMaskCoveragePreview::ShowRangeProbe` then covers the candidate: candidate jobs end as
  Discarded, the newest candidate request is kept, and the published source is RangeProbe.
  `HideRangeProbe(request_id)` presents that candidate request again (same shared pixels, no
  model call). A candidate update replaces the probe; the end of the edit (Commit, Cancel) ends
  it.
- **Range edit on an explicit Mask, not on the selection.** The controller's field edit works only
  on the selected Mask, and the controller cannot select an AI Mask. New
  `EditorMaskCreationController::BeginMaskRangeEdit(grade_id, mask_id, key)` opens a range edit
  of any Mask kind without a selection or mode change. While it is open, the matching explicit
  `ApplyMaskFieldValue` writes live values (Interactive), `FinishMaskInput` publishes one
  SetMaskField, `CancelMaskInput` restores the before value, and every other operation is refused.
  `HasOpenOperation` includes it, so Undo, Redo, and head moves wait for the release. The session
  routes every BeginMaskField of a range key to it, also for the Mask panel.
- **Range target.** The adapter targets the stored Mask of the open AI Mask edit
  (`SetAiMaskEditRangeTarget`, called by `EditorAiMaskTool` on Open, Opened, and Close), else the
  selected Mask. A new candidate has no target, so the bars are disabled (D11) with the notice
  "Confirm the new AI Mask to set its ranges.". Replaced by #604 (section 11): the candidate of a
  new Mask is the target, and the notice is removed.
- **Toggle rules (D12).** A drag shows the coverage from press to release when the toggle is off.
  The toggle follows a new selection; it turns off when the target is gone, when the AI Mask edit
  opens or closes, when an AI candidate replaces the probe (`OnRangeCoverageReplaced`), and at the
  start of any adjustment input (`EditorSessionController::EnqueueAdjustmentPatch` calls
  `OnAdjustmentInputStarted`). The coverage is not shown again by itself (master plan 10.4).
- **Handle values.** Handles keep a distance of 1.2 times the stored minimum widths (0.012 in L,
  1.2 degrees), so float rounding never writes an invalid range. Both handles at the ends write
  `null`. A Hue start of 360 is written as 0, an end of 0 as 360. A new restricted range uses the
  stored softness of the Mask, else the D6 defaults. Double-click on a bar writes `null` with one
  commit (`resetRange`, a SetMaskField without an open edit).
- **Bar fills from C++.** `edit/mask/mask_range_display.hpp`: `MaskHueBarColor` (OkLCh
  L = 0.75, C = 0.12 through the inverse of the kernel `LinearAp1ToOkLab`, AP1 to Rec.709, sRGB)
  and `MaskLuminanceBarGray` (neutral with OkLab L, luminance L^3). All 37 Hue stops are inside
  the sRGB gamut, so no clipping moves a hue. QML draws the stops in a Canvas gradient.
- **QML.** `RangeSelectorBar.qml` (handles, body drag, double-click reset after the second
  press settles, wrapped interval in two parts, outside dimmed with `appTheme.overlayColor`) and
  `EditorMaskRangeSection.qml` (both bars and "Show selection"). The section is at the top of
  `EditorMasksContextPanel.qml` (above Name) and of `EditorAiMaskPanel.qml` (above the tasks).
  Strings were added to both `.ts` files by hand (context `EditorMaskRangeSection`): 明度范围,
  色相范围, 显示选区, 蒙版明度范围, 蒙版色相范围, 确认新的 AI 蒙版后即可设置其范围。

**Primary success call chain (range drag in the Mask panel or the AI Mask panel):**

```text
RangeSelectorBar press -> EditorMaskCreationAdapter::beginRangeEdit(kind)
  -> EditorSessionController::EnqueueMaskCreation (can_edit, or AI edit + IsMaskRangeEditCommand)
     -> EditorSessionService::EnqueueMaskCreation -> ConsumePendingMaskCommands (owner)
        -> ApplyMaskCreationCommand: AI edit target check
           -> EditorMaskCreationController::BeginMaskRangeEdit(grade, mask, key)
  -> (toggle off) IEditorSessionBackend::ShowMaskRangeCoverage(grade, mask)
     -> EditorMaskCoveragePreview::ShowRangeProbe (covers a candidate during the AI edit)
RangeSelectorBar move -> updateRangeEdit -> RangeJson (clamp, minimum width, full -> null)
  -> SetMaskField -> ApplyMaskFieldValue (open range edit) -> live value, Interactive render
     (the frame carries the probe -> coverage of the Mask on the Grade input)
RangeSelectorBar release -> finishRangeEdit -> Finish
  -> FinishMaskInput -> PublishMaskRangeEdit -> one SetMaskField commit, Quality render
  -> (toggle off) HideMaskRangeCoverage -> HideRangeProbe (candidate presented again)
```

**Primary failure call chains:**

```text
Range command for another Mask during an AI Mask edit
  -> owner rejects ("Commit or cancel the AI Mask edit before editing Masks"); no write
Any other Mask command during an AI Mask edit -> rejected at enqueue, as before
New AI Mask candidate -> no range target -> bars disabled, beginRangeEdit returns false
Escape / CancelMode during a drag -> CancelMaskRangeEdit restores the before value, Quality render
History publish fails at release -> the before value is restored live, Quality render, error
Probe during a candidate in the Previous status -> refused (retry the AI preview first)
```

**What was proven (executed tests):**

| Required criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| One drag gives one commit; coverage shown only during the drag; wrapped hue | `RangeDragCommitsOnceAndShowsTheCoverageOnlyDuringTheDrag` | `EditorMaskRangeAdapterTest` | PASS |
| Minimum handle width; both ends write the full range (`null`) | `HandlesKeepTheMinimumWidthAndBothEndsWriteTheFullRange` | same | PASS |
| Reset writes `null` with one commit; Undo restores | `ResetWritesNullWithOneCommit` | same | PASS |
| Toggle keeps the coverage; an adjustment input hides it and turns the toggle off | `ShowSelectionKeepsTheCoverageUntilAnAdjustmentInput` | same | PASS |
| AI Mask edit targets its stored Mask (one commit, probe over the candidate); a new candidate has none | `AiMaskEditTargetsItsStoredMaskAndANewCandidateHasNone` | same | PASS |
| Hue bar shows the kernel hue; Luminance bar shows OkLab L | `HueBarStopsShowTheHueThatTheKernelTests`, `LuminanceBarStopsShowTheOkLabLightnessOfTheirPosition` | same | PASS |
| Probe of the stored Mask covers the candidate; Hide shows it again without a model call; a candidate update and the edit end replace it; another Mask refused | `ProbeOfTheStoredMaskCoversItsCandidateUntilHidden` | `EditorMaskCoveragePreviewTest` | PASS |
| Range drag of the stored AI Mask commits once during its edit; other Masks and fields refused; the AI commit keeps the range | `RangeDragOfTheStoredAiMaskCommitsOnceDuringItsAiMaskEdit` | same | PASS |

Commands (Windows, `win_debug`, `build/debug`): `cmd /c scripts\msvc_env.cmd --build --preset
win_debug --parallel 6 --target EditorMaskCoveragePreviewTest EditorMaskRangeAdapterTest
AnalyticMaskCreationTest alcedo_main`, then each binary from its `*_runtime` directory.

Suite totals: `EditorMaskRangeAdapterTest` 7/7, `EditorMaskCoveragePreviewTest` 18/18,
`AnalyticMaskCreationTest` 30/30. Regression suites: `EditorComparisonServiceTest` 22/22 (AI Mask edit Open, Commit, Cancel),
`EditorHistoryMoveDuringInputTest` 3/3, `EditorPendingInputSessionTest` 15/15,
`EditorSessionServiceFacadeTest` 4/4, `EditorSessionHistoryPortTest` 115/116 (the failure,
`CheckoutAndPasteHashesAreUnchanged`, is pre-existing: #555), `EditorAiMaskPanelQmlTest` 1/1 (the
panel loads with the new section; not used as evidence). The full suite was not run.
`alcedo_main` links (QML compiled by qmlcachegen); the app was not started, and the bars were not
checked by hand.

**Checklist / exit condition:** all section 7 items done, with the AI Mask edit change above.

**Residual gaps:** no QML test drives the bars (AGENTS.md: QML harness tests are not evidence).
The Smoothness control of D6 is not exposed.

## 8. Phases

| Phase | Content | Depends on |
| --- | --- | --- |
| MR1 | Model, JSON, validation, keys, Mask field commands, history (section 4) | - |
| MR2 | GPU range evaluation on CUDA, OpenCL, Metal (section 5) | MR1 |
| MR3 | Node-scoped coverage probe and detail coverage slot (section 6) | MR2 |
| MR4 | Range bars, adapter, overlay control, translations (section 7) | MR1, MR3 |

MR3 can start once the MR2 encoder change (scene binding) is in, in parallel with the MR2 kernels.
Each phase writes its completion record and call chains into its section.

## 9. Tracking

- Tracking: [#552](https://github.com/zidage/AlcedoStudio/issues/552)
- MR1: [#548](https://github.com/zidage/AlcedoStudio/issues/548)
- MR2: [#549](https://github.com/zidage/AlcedoStudio/issues/549)
- MR3: [#550](https://github.com/zidage/AlcedoStudio/issues/550)
- MR4: [#551](https://github.com/zidage/AlcedoStudio/issues/551)
- RP1-RP4 (section 10): [#564](https://github.com/zidage/AlcedoStudio/issues/564)
- Ranges of a new AI Mask (section 11): [#604](https://github.com/zidage/AlcedoStudio/issues/604)
- Range section polish (section 12): [#605](https://github.com/zidage/AlcedoStudio/issues/605),
  [#606](https://github.com/zidage/AlcedoStudio/issues/606),
  [#607](https://github.com/zidage/AlcedoStudio/issues/607)

## 10. Precise range controls and AI Mask edit on demand (#564)

Issue [#564](https://github.com/zidage/AlcedoStudio/issues/564) holds the approved scope
(2026-10-27): A (handle drag = press value + cursor delta), B (Shift fine drag, key and wheel
steps), C (value fields), D (Softness sliders and the softness ramp on the bar), G (saturation
threshold `ColorRangeModel::min_chroma`), the shared Range section above the Mask-specific
controls, and an AI Mask edit that starts only on a user action. Not in scope: sampler,
histograms, hue bar offset.

| Phase | Content | Depends on |
| --- | --- | --- |
| RP1 | `min_chroma` in the model, JSON, validation, `MaskRangeParams`, CUDA, OpenCL, Metal | - |
| RP2 | Bar precision (A, B, softness ramp drawing) and adapter key and wheel steps | RP1 |
| RP3 | Shared Range section, value fields (C), Softness sliders (D), threshold slider (G) | RP2 |
| RP4 | AI Mask edit on demand: row selection selects only, "Edit AI Mask" button | RP3 |

### 10.1 RP1: saturation threshold in the model and kernels

##### RP1 completion record (2026-10-27)

**Status:** complete. Branch `feature/mask-range-precision-rp1-min-chroma` (from `main`
`68b77fdb`).

**Changes from the issue and details that it did not give:**

- **Gate formula.** The issue gives `smoothstep(min_chroma - 0.015, min_chroma + 0.015, C)`. In
  float, `0.03f + 0.015f` is `0.044999998`, one ulp below the old edge `0.045f`. The kernels add
  the offset `t = min_chroma - 0.03` to the fixed edges instead:
  `smoothstep(0.015 + t, 0.045 + t, C)`. This is the same gate for every threshold, and at the
  default `t` is exactly 0, so the edges are the old float constants. A one-time byte dump on CUDA
  and OpenCL (three ranged Masks on the ramp scene and the hue wheel, `main` against this branch)
  was identical; the dump test was not committed.
- **Constants.** `kColorRangeDefaultMinChroma = 0.03`, `kColorRangeMinimumMinChroma = 0.015`,
  `kColorRangeMaximumMinChroma = 0.20` (`mask_model.hpp`).
- **JSON.** `ColorRangeToJson` writes `min_chroma` only when it is not the default. The reader
  accepts the key as optional (absent = default). A full-range object with an out-of-bounds
  threshold is rejected before it reads as empty, like `softness`. History canonical form follows
  from the writer: a stored `"min_chroma": 0.03` is not canonical and the batch rejects it.
- **Keys.** `MaskRangeParams` is 36 bytes (`hue_min_chroma` last); `static_assert`s in
  `mask_model.hpp`, `opencl_mask_pass.cpp`, and `metal_mask_pass.mm`. The Mask result content key
  mixes `min_chroma`. The plan topology hash keeps `has_value()` only (MR1 rule). No other key or
  fingerprint hashes range values.
- **Adapter.** `EditorMaskCreationAdapter::RangeView` keeps `hue_min_chroma`, so a Hue drag keeps
  a stored threshold (before RP1 the adapter rebuilt the range from start, end, and softness).

**Primary call chain:**

```text
MaskModel.color_range.min_chroma -> MaskRangeParamsFromMask -> hue_min_chroma
  -> CUDA MaskRangeWeight / mask.cl MaskRangeWeight / mask.metal MaskRangeWeight
     -> gate = smoothstep(0.015 + t, 0.045 + t, C), t = hue_min_chroma - 0.03
JSON: ColorRangeFromJson (optional key, bounds) <-> ColorRangeToJson (omitted at the default)
```

**Failure chain:** `min_chroma` not finite, below 0.015, or above 0.2 -> `ValidateColorRange` /
`ColorRangeFromJson` throw "color_range min_chroma must be in [0.015, 0.2]"; the document, the
Mask setter, and the history batch are not changed.

**What was proven (executed tests, Windows `win_debug`):**

| Criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Default range writes the same bytes; non-default value round-trips (Mask JSON and document) | `HueRangeMinChromaIsOmittedAtTheDefaultAndRoundTripsOtherwise` | `GpuDagModelGraphTest` | PASS |
| Bounds rejected (JSON, validation, setter; full-range object too) | `HueRangeMinChromaOutsideItsBoundsIsRejected` | same | PASS |
| Packed for the kernels; default edges are the old float constants | `HueRangeMinChromaIsPackedForTheKernels` | same | PASS |
| History accepts a non-default threshold and rejects a written default or out-of-bounds value | `MaskRangeFieldsAcceptOnlyCanonicalRangeValues` | `PipelineEditBatchTest` | PASS |
| Default threshold gives the old fixed gate (reference with literal 0.015 / 0.045) | `DefaultSaturationThresholdKeepsTheFixedChromaGate` | `GpuDagCudaMaskTest`, `GpuDagOpenClGradeTest` | PASS |
| Raised threshold drops low-chroma pixels and keeps saturated ones | `RaisedSaturationThresholdDropsLowChromaPixels` | same | PASS |

Suite totals: `GpuDagCudaMaskTest` 31/31, `GpuDagOpenClGradeTest` `*Mask*` 32/32,
`GpuDagModelGraphTest` 150/150, `PipelineEditBatchTest` 22/22, `EditorMaskRangeAdapterTest` 7/7,
`DocumentTransferTest` 34/35 (the failure, `TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches`,
is pre-existing: #554).

### 10.2 RP2: bar precision

##### RP2 completion record (2026-10-27)

**Status:** complete. Branch `feature/mask-range-precision-rp2-bar-precision`, stacked on RP1.

**Changes from the issue and details that it did not give:**

- **A. Handle drag.** `RangeSelectorBar.dragTo` moves a handle to its press value plus the cursor
  delta, clamped to the bar; the wrap and minimum-width rules of MR4 are unchanged. A move that
  leaves both values unchanged reports nothing, so a press alone reports no value.
- **B. Shift fine drag.** The delta is multiplied by 0.1 while Shift is held. When Shift changes
  during a drag, the bar anchors at the values of the previous cursor position, so the handle does
  not jump and the move since then runs at the new speed.
- **B. Keys and wheel.** The bar takes focus on Tab or on a press (a press on a handle focuses that
  handle; Tab moves from the first handle to the second, then out). Left/Right report
  `stepped(part, ±1, shift)`; the wheel over a handle reports `stepped(part, n, shift)` per 120
  angle units (horizontal delta used when Shift turns the wheel horizontal). The wheel elsewhere
  is not accepted, so the panel still scrolls. The focused handle has a 2 px text-color border.
- **Step owner.** New `EditorMaskCreationAdapter::stepRangeHandle(kind, handle, steps, coarse)`
  sizes the steps (0.005 L, 0.5 degrees, ×10 coarse), keeps the minimum handle distance, and
  keeps hue handles on the bar (a step does not wrap a handle across an end, as in a drag). The
  first step opens a normal range edit (BeginMaskField, coverage shown as in a drag); each step
  is one live SetMaskField; a single-shot timer (180 ms, the `AdjustmentSlider` key debounce)
  sends Finish, so one burst is one history commit. A pointer drag (`beginRangeEdit`) or a reset
  (`resetRange`) during a burst settles the burst first; a step during a pointer drag is refused.
  The burst is dropped with the other range state on image close and on AI Mask edit open/close.
- **D. Softness ramp.** The two dim rectangles are replaced by a Canvas that dims each pixel
  column by `1 - weight`, with the kernel weight of the shown handles and softness (luminance
  smoothsteps, hue circular distance). New adapter properties `maskLuminanceSoftness` and
  `maskHueSoftness` give the stored softness (the default for a full range).

**Primary call chains:**

```text
Handle drag: RangeSelectorBar press (no value) -> began -> beginRangeEdit
  move -> dragTo: press value + delta (x 0.1 with Shift) -> moved -> updateRangeEdit
  release -> finished -> finishRangeEdit -> one SetMaskField commit (MR4 chain)
Key / wheel: RangeSelectorBar Keys / onWheel -> stepped(part, steps, coarse)
  -> EditorMaskCreationAdapter::stepRangeHandle
     -> (first step) beginRangeEdit -> BeginMaskField, coverage shown
     -> updateRangeEdit -> live SetMaskField (Interactive)
     -> range_step_timer_ (180 ms after the last step) -> SettleRangeSteps -> finishRangeEdit
        -> one SetMaskField commit
```

**Failure chains:** a step during a pointer drag, a step with no range target, or a step that
cannot move (bar end, minimum distance) returns false and writes nothing new. A burst of steps
that ends on the stored value commits nothing.

**What was proven (executed tests, Windows `win_debug`):**

| Criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| No change without a value change; one commit per drag | `PressAndMoveBackWithoutAChangeCommitsNothing`, `RangeDragCommitsOnceAndShowsTheCoverageOnlyDuringTheDrag` | `EditorMaskRangeAdapterTest` | PASS |
| One commit per key burst; one handle moves; coarse steps; minimum distance; one Undo | `KeyStepBurstMovesOneHandleAndCommitsOnce` | same | PASS |
| Hue steps keep handles on the bar; a drag and a reset settle the burst first; a step during a drag is refused | `HueStepsKeepHandlesOnTheBarAndADragSettlesTheBurstFirst` | same | PASS |
| Bar: handle moves by the delta from the press, a press reports nothing, Shift = 1/10 | `HandleDragMovesByTheCursorDeltaFromThePress` | `RangeSelectorBarQmlTest` | PASS (fails on the MR4 bar: 8 degree jump) |
| Bar: keys step the focused handle, the wheel steps the handle under it only | `KeysStepTheFocusedHandleAndTheWheelStepsTheHandleUnderIt` | same | PASS (fails on the MR4 bar) |

The QML harness tests support the adapter tests; they are not the evidence of the commits
(AGENTS.md, #526). Suite totals: `EditorMaskRangeAdapterTest` 10/10, `RangeSelectorBarQmlTest`
2/2. The softness ramp drawing is not checked by a test.

### 10.3 RP3: shared Range section, value fields, Softness, saturation threshold

##### RP3 completion record (2026-10-27)

**Status:** complete. Branch `feature/mask-range-precision-rp3-range-section`, stacked on RP2.

**Changes from the issue and details that it did not give:**

- **Panel split.** `EditorMasksContextPanel` is a column: the "Mask" title, the empty-state text,
  the Range section in a `CollapsibleSection` titled "Range" (范围; the shared fold component, so
  the header behaves like the other panel groups), then the lower part (`editorMasksLowerPart`).
  The lower part holds the Mask-field Flickable, or the `EditorAiMaskPanel` while the AI Mask
  edit is open. The Range section is not inside the Flickable. The fold state is not persisted.
- **AI Mask panel placement.** `EditorAdjustmentStack` no longer lays the AI Mask panel over the
  whole stack. While the AI Mask edit is open the stack shows the Mask page (index 7), whose
  lower part shows the AI Mask panel. The copy of `EditorMaskRangeSection` in `EditorAiMaskPanel`
  is removed. During the edit the Range controls are enabled while the tool is ready and not
  busy (the MR4 rule, `canStep`), and the D11 notice shows for a new candidate.
- **Value fields (C).** `RangeValueField` (inline in the section): Luminance Low/High as L × 100
  and Hue Start/End in degrees, one decimal, `DoubleValidator`. Enter or focus-out calls
  `editRangeValue(kind, "first" | "second", value)` and `commitRangeEdits()`: one SetMaskField
  commit. The adapter clamps a Luminance value to the minimum handle distance; a Hue value wraps
  into the bar (Start 360 = 0, End 0 = 360), Start > End wraps, and a Hue value that breaks the
  minimum distance is moved to it.
- **Softness and threshold (D, G).** `AdjustmentSlider` with an inline `RangeParameterModel`:
  Luminance Softness 0.5-50 (L × 100, default 5), Hue Softness 0.5-90 degrees (default 12),
  Saturation Threshold (饱和度门槛) 1.5-20 (chroma × 100, default 3). A slider drag is one range
  edit (`beginRangeEdit`, live `editRangeValue`, `finishRangeEdit`: coverage shown, one commit);
  keys and typed values go into the step burst of RP2 (one commit; Enter settles at once);
  double-click resets to the default with one commit. The three sliders are disabled while their
  range is full, and the adapter refuses them too. A reset to full writes `null` and the adapter
  view returns to the default softness and threshold, so the next restriction starts there.
- **Adapter.** New `editRangeValue(kind, field, value)` (fields `first`, `second`, `softness`,
  `minChroma`), `commitRangeEdits()`, properties `maskHueMinChroma` and `rangeParameterLimits`
  (bounds and defaults from the model constants). The open edit carries its softness and
  threshold (`range_softness_`, `range_min_chroma_`), so a handle drag keeps them and a slider
  drag keeps the handles. `stepRangeHandle` and `editRangeValue` share `OpenRangeInput` (join
  the open edit or open a burst) and `WriteRangeEdit`.
- **Strings.** Added to `alcedo_main_zh_CN.ts` by hand (no `lupdate`): context
  `EditorMaskRangeSection`: Softness 柔和度, Saturation Threshold 饱和度门槛, Luminance low 明度下限,
  Luminance high 明度上限, Hue start 色相起点, Hue end 色相终点; context `EditorMasksContextPanel`:
  Range 范围. `lrelease` compiles the file (1507 finished).

**Primary call chains:**

```text
Typed value: RangeValueField editingFinished -> editRangeValue(kind, first|second, v)
  -> OpenRangeInput (burst: BeginMaskField, coverage) -> clamp / wrap -> WriteRangeEdit
  -> commitRangeEdits -> SettleRangeSteps -> finishRangeEdit -> one SetMaskField commit
Softness / threshold drag: AdjustmentSlider beginDrag -> beginRangeEdit(kind)
  updateDrag -> editRangeValue(kind, softness|minChroma, v / scale) -> joins the open edit
  -> WriteRangeEdit (handles kept, Interactive render)
  finishDrag -> finishRangeEdit -> one commit
AI Mask edit open: EditorAdjustmentStack currentIndex 7 -> EditorMasksContextPanel
  -> Range section (target = stored Mask of the edit) + EditorAiMaskPanel in the lower part
```

**Failure chains:** Softness or threshold of a full range, threshold of the Luminance Range, a
non-finite value, or an unchanged value -> `editRangeValue` returns false and writes nothing
new; a value of one range while a drag of the other range is open -> refused.

**What was proven (executed tests, Windows `win_debug`):**

| Criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Typed values clamp to the minimum distance, wrap, and commit once | `TypedRangeValuesAreClampedAndCommitOnce` | `EditorMaskRangeAdapterTest` | PASS |
| Softness drag is one commit with handles kept; threshold clamped, stored, kept by a drag; refused while full; reset gives the defaults; slider bounds | `SoftnessAndSaturationThresholdBelongToARestrictedRange` | same | PASS |
| One Range section, above and outside the scrolling lower part; the AI Mask panel only in the lower part; the D11 notice | `RangeSectionIsOneFixedUpperPartAndTheAiPanelReplacesTheLowerPart` | `EditorMasksPanelQmlTest` | PASS (fails on the RP2 panel) |

Suite totals: `EditorMaskRangeAdapterTest` 12/12, `EditorMasksPanelQmlTest` 1/1,
`EditorAiMaskPanelQmlTest` 1/1, `RangeSelectorBarQmlTest` 2/2. `alcedo_main` builds (QML
compiled by qmlcachegen). The panel test uses fake owners and checks only the layout; it is not
the evidence of the edits (#526).

### 10.4 RP4: AI Mask edit on demand

##### RP4 completion record (2026-10-27)

**Status:** complete. Branch `feature/mask-range-precision-rp4-ai-edit-on-demand`, stacked on
RP3.

**Changes from the issue and details that it did not give:**

- **The owner selects an AI Mask.** Before RP4 the session refused `SelectMask` for an AI Mask
  ("Mask source kind is not selectable"), and a row click worked only because the QML then opened
  the AI Mask edit. `EditorMaskCreationController::SelectMask` now accepts an AI Mask. Its
  selection has no handles (`HandleMatchesKind` refuses every move, the adapter does not own the
  left button, and `maskNudgeAvailable` is false), and only the AI Mask edit changes its source.
  The Mask value edits (name, enabled, invert, opacity) and the range edits work through the
  normal owner path. `OpenAiMaskEdit` already aborts the Mask selection (`AbortMaskCreation`), so
  the AI Mask edit starts from the same state as before.
- **Row selection only selects.** `EditorMaskGroupsPanel.selectMaskFromGroup` and
  `EditorNodesPanel.selectMaskFromDrawer` no longer call the tool. `openSelectedAiMask` (and its
  helper `IsAiMask`) is removed from `EditorAiMaskTool`; `openSelectedMask` is the only way to
  open a stored AI Mask. The #517 QML tests that expected a row click to open the edit are
  replaced by tests that expect only the selection.
- **"Edit AI Mask" (编辑 AI 蒙版).** A full-width button at the top of the lower part, shown for a
  selected AI Mask while no AI Mask edit is open; it calls `openSelectedMask()`. The header AI Mask
  button already called `openSelectedMask()` for a selected AI Mask and `open()` otherwise, so it
  needed no change. Below the button the lower part shows the Mask value fields (Name, Enabled,
  Invert, Opacity) of the AI Mask; the analytic source fields stay hidden.
- **Range edits and "Show selection" without the edit.** The selected AI Mask is the range target
  (`RangeTargetNow`), so range edits use `BeginMaskRangeEdit` and one SetMaskField. The MR3 probe
  takes any Mask kind; MR3's `ProbeRidesOnEveryEditorFrameUntilHidden` already uses a committed AI
  Mask with no edit open, and the MR2 `AiMaskRangeMatchesReference` kernels evaluate the stored
  asset with ranges. RP4 adds the same check through the adapter selection.
- **Strings.** `alcedo_main_zh_CN.ts` context `EditorMasksContextPanel`: Edit AI Mask 编辑 AI 蒙版.
  The audited count of that context in `SimplifiedChineseCatalogTest` is 29 (RP3 made it 28 for
  Range).

**Primary call chains:**

```text
AI Mask row click: EditorMaskGroupsPanel / EditorNodesPanel -> maskCreation.selectMask
  -> SelectMask command -> EditorMaskCreationController::SelectMask (AI kind: Selected)
  -> adapter SyncOwnerSelection -> ApplyOwnerSource (toolKind aiSegmentation, no handles)
  -> EditorSessionController::SyncMaskAdjustmentPanel -> Mask page: Range section + lower part
     ("Edit AI Mask", Mask value fields); no lease, no analysis render, no sidecar
Range edit / Show selection of that AI Mask: the RP2/RP3 chains on the selected Mask
"Edit AI Mask" or the header AI Mask button -> EditorAiMaskTool::openSelectedMask -> OpenFor
  -> runtime request -> OpenAiMaskEdit (AbortMaskCreation, lease) -> analysis render -> sidecar
New AI Mask (header button, no AI Mask selected) -> EditorAiMaskTool::open -> OpenFor(nullopt)
```

**Failure chains:** unchanged. `openSelectedMask` without a selection shows "Select an AI Mask in
the Mask list first."; the session refuses a non-AI Mask ("The Mask is not an AI Mask").

**What was proven (executed tests, Windows `win_debug`):**

| Criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Selecting a stored AI Mask starts no sidecar request and no edit; a range edit commits once; the probe rides the frames; Mask values edit; the Edit action opens the edit | `SelectingAStoredAiMaskStartsNoAiMaskEditAndItsRangesCommit` | `EditorMaskRangeAdapterTest` | PASS (fails without the controller change) |
| A new AI Mask opens its edit at once, with no range target | `NewAiMaskOpensItsEditAtOnce` | same | PASS |
| A row click on an AI Mask only selects it (Mask group panel, Node drawer) | `MaskGroupAiMaskRowClickOnlySelectsTheMask`, `NodeDrawerAiMaskRowClickOnlySelectsTheMask` | `EditorNodesPanelQmlTest` | PASS |
| The lower part shows "Edit AI Mask" for a selected AI Mask; only a click opens the edit | `SelectedAiMaskShowsTheEditAiMaskButtonThatOpensItsEdit` | `EditorMasksPanelQmlTest` | PASS |

Suite totals (all four phases on this branch): `EditorMaskRangeAdapterTest` 14/14,
`EditorMasksPanelQmlTest` 2/2, `RangeSelectorBarQmlTest` 2/2, `EditorAiMaskPanelQmlTest` 1/1,
`EditorNodesPanelQmlTest` 85/85, `EditorAdjustmentHeaderQmlTest` 25/25,
`EditorAdjustmentSnapshotQmlTest` 4/4, `SimplifiedChineseCatalogTest` 6/6,
`AnalyticMaskCreationTest` 30/30, `EditorMaskCoveragePreviewTest` 18/18,
`EditorComparisonServiceTest` 22/22, `EditorHistoryMoveDuringInputTest` 3/3,
`EditorPendingInputSessionTest` 15/15, `EditorSessionServiceFacadeTest` 4/4,
`EditorSessionHistoryPortTest` 115/116 (pre-existing #555), `GpuDagModelGraphTest` 150/150,
`PipelineEditBatchTest` 22/22, `GpuDagCudaMaskTest` 31/31, `GpuDagOpenClGradeTest` 88/94
(the six `OpenClMultiGradeFixture` failures are pre-existing: #496), `DocumentTransferTest`
34/35 (pre-existing #554). `alcedo_main` builds.

## 11. Ranges of a new AI Mask during its edit (#604)

Issue [#604](https://github.com/zidage/AlcedoStudio/issues/604) replaces the new-candidate part of
D11 (maintainer decision, 2026-10-29). During the edit of a new AI Mask, the Luminance Range and
Hue Range are enabled (not while an AI operation runs). The range probe of the candidate covers
the candidate, as for a stored AI Mask in its edit (MR4). The values set during the edit are
written with the new Mask at Confirm, in the same commit. Close without saving discards them. The
notice "Confirm the new AI Mask to set its ranges." and its `.ts` entries are removed.

Change by the maintainer at the start of the work (2026-10-29): requirement 3 of the issue (after
Confirm the new Mask is selected and the Range section targets it) is not done. After Confirm the
range does not need more edits: the Mask panel closes. The maintainer also chose the full probe of
the candidate from the photo render (not only values without a display).

##### #604 completion record (2026-10-29)

**Status:** complete. Branch `feature/ai-mask-new-candidate-ranges`.

**Implementation and the details that the issue did not give:**

- **Candidate ranges are kept by the AI Mask edit (session owner).**
  `EditorSessionService::AiMaskEditTarget` holds `candidate_luminance_range`,
  `candidate_color_range`, and the open range edit of the candidate (field key, value before the
  edit). A range command of the new candidate is a normal Mask range command
  (`IsMaskRangeEditCommand`) for the Grade of the edit with an empty Mask id.
  `ApplyMaskCreationCommand` sends it, and Cancel and CancelMode, to
  `ApplyAiMaskCandidateRangeCommand`: BeginMaskField opens the edit, SetMaskField parses the
  range JSON (`LuminanceRangeFromJson` / `ColorRangeFromJson`, full range = empty), Finish closes
  the edit, and Cancel restores the value before it. No command writes the document or the
  history. The end of the edit (Commit, Cancel, image change) ends `ai_mask_edit_`, so Close
  without saving discards the values.
- **Commit.** `CommitAiMaskEdit` first applies the queued Mask commands
  (`SettleMaskInputForBoundary`), so a key or wheel step burst that has not settled at the click
  is part of the commit, and it ends an open candidate range edit with its current value. Then
  `EditorMaskCreationController::CommitAiMaskSource` takes the candidate ranges: the provisional
  Mask gets them before `PublishAddMask`, so the AddMask batch (one commit) holds the source and
  the ranges. A stored Mask keeps its own ranges (non-empty ranges with a Mask id are refused).
  `EditorAiMaskTool::commit` calls `commitRangeEdits()` first, so the settle of a burst is
  queued before the commit.
- **Range probe of the candidate.** `MaskCoverageProbe` has an optional
  `MaskCoverageProbeCandidate` (the newest candidate selection: shared pixels, analysis size, edge
  values; and the ranges). `ShowMaskRangeCoverage(grade, empty Mask id)` during the edit of a new
  Mask makes this probe from `EditorMaskCoveragePreview::NewestCandidate()`; it is refused without
  a candidate coverage or while the candidate is in the Previous status. A shown probe of the
  candidate takes each range write (`ShowRangeProbe` with the new ranges) and routes a render:
  Interactive inside an open edit, Quality for a settled write. A candidate update replaces the
  probe (MR4 rule).
- **Render.** The candidate is not a Mask of the document, so `PlanExecutor` evaluates it for the
  display only: before the Grade pass of the probed Grade, `RecordProbedCandidate` acquires the
  candidate coverage (`AcquireAiMaskCandidateCoverage`, now allowed in a render too) and records
  `RecordMaskCandidateRangeDisplay` into the coverage display target. The value is the raster
  MaskEvaluate of the candidate with the ranges on the Grade input, no invert, opacity 1: the R8
  of the same Mask after the commit. No R8 image is bound, so no result, result key, or photo
  pass changes. CUDA has a new kernel (`RasterMaskRangeDisplayKernel`); OpenCL and Metal use their
  MaskEvaluate raster kernels with the RGBA32F output (they quantize and write (value, 0, 0, 1)).
- **Adapter.** `EditorMaskCreationAdapter::RangeTarget` has a `candidate` flag. After the session
  opens the edit (`SetAiMaskEditRangeTarget(true, grade, nullopt)`), the candidate is the target.
  The candidate is not in the document, so the adapter shows the values that it wrote (only it
  writes them; the session keeps them for the probe and the commit); a new candidate starts with
  full ranges.
- **After Confirm the Mask panel closes (maintainer change).** The AI Mask edit ends the Mask
  mode at its Open (`AbortMaskCreation`), so the editor is already back on the panel from before
  the Mask panel, and the stack shows the Mask page only while the tool is active. The commit
  selects no Mask. So after Confirm the tool closes and the Mask panel with it; no Range section
  stays open. A test keeps this behavior.
- **QML and translations.** The candidate notice and the `newAiMaskCandidate` property of
  `EditorMaskRangeSection.qml` are removed, and the entry is removed from both `.ts` files by hand
  (no `lupdate` run).

**Primary success call chain (range drag during the edit of a new AI Mask, then Confirm):**

```text
EditorAiMaskTool::OnOpened -> SetAiMaskEditRangeTarget(true, grade, nullopt) (candidate target)
RangeSelectorBar press -> EditorMaskCreationAdapter::beginRangeEdit
  -> EditorSessionController::EnqueueMaskCreation (AI edit + IsMaskRangeEditCommand)
     -> EditorSessionService::ApplyMaskCreationCommand -> ApplyAiMaskCandidateRangeCommand
  -> ShowMaskRangeCoverage(grade, empty Mask id) -> probe with the newest candidate + ranges
     -> EditorMaskCoveragePreview::ShowRangeProbe (covers the candidate), Quality render
RangeSelectorBar move -> SetMaskField -> candidate range -> ShowRangeProbe, Interactive render
  -> PlanExecutor::RecordProbedCandidate -> RecordMaskCandidateRangeDisplay -> coverage frame
RangeSelectorBar release -> Finish; HideMaskRangeCoverage (candidate presented again)
Confirm -> EditorAiMaskTool::commit -> commitRangeEdits -> PrepareCommit (worker)
  -> CommitAiMaskEdit -> SettleMaskInputForBoundary -> CommitAiMaskSource(..., ranges)
     -> AddMask batch with source and ranges (one commit) -> edit ends -> tool Close
```

**Primary failure call chains:**

```text
Probe of the candidate before it has coverage -> refused ("has no coverage yet")
Probe of the candidate in the Previous status -> refused (retry the AI preview first)
Range command for a stored Mask during the edit of a new Mask -> refused by the owner
Invalid range JSON -> refused; the candidate value does not change
Close without saving -> CancelAiMaskEdit -> ai_mask_edit_ ends; the ranges are discarded
```

**What was proven (executed tests, Windows `win_debug` unless noted):**

| Criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| The probe of the candidate shows the R8 of the same Mask after its commit, runs no Mask pass, keeps the photo byte-identical; its ranges change the coverage | `CandidateProbeShowsTheR8OfTheCommittedMaskAndKeepsThePhoto` | `MaskRangeCoverageProbeTest` | PASS on CUDA, OpenCL, and Metal (Apple Silicon, `macos-debug`) |
| Range commands of the candidate write no document; Cancel restores; the probe needs a coverage, carries the ranges, and follows each write; a stored Mask id is refused; the commit writes the ranges in one commit | `RangeCommandsOfTheNewCandidateWriteNoDocumentUntilTheCommit` | `EditorMaskCoveragePreviewTest` | PASS |
| A drag and an unsettled key step burst of the candidate write no history; the frames carry the candidate probe; Confirm adds the Mask with both ranges in one commit | `NewAiMaskCandidateRangesCommitWithTheNewMaskInOneCommit` | `EditorMaskRangeAdapterTest` | PASS |
| Close without saving discards the ranges; the next candidate starts with full ranges and commits none | `ClosingANewAiMaskEditWithoutSavingDiscardsItsRanges` | same | PASS |
| "Show selection" of the candidate follows each range write; a candidate update replaces it | `ShowSelectionOfANewCandidateFollowsItsRangesUntilACandidateUpdate` | same | PASS |
| After Confirm the Mask panel is closed: no Mask mode, no selection, no range target, the previous panel | `ConfirmOfANewAiMaskLeavesTheMaskPanelClosed` | same | PASS |
| The candidate is the target after the session opens the edit, not before | `AiMaskEditTargetsItsStoredMaskOrItsNewCandidate`, `NewAiMaskOpensItsEditAtOnce` | same | PASS |
| The Range controls of a new candidate are enabled while the tool is ready, disabled while busy; no candidate notice | `RangeSectionIsOneFixedUpperPartAndTheAiPanelReplacesTheLowerPart` | `EditorMasksPanelQmlTest` | PASS (fails on the old QML: the notice exists) |

Suite totals: `EditorMaskCoveragePreviewTest` 20/20, `EditorMaskRangeAdapterTest` 18/18,
`AnalyticMaskCreationTest` 30/30, `EditorMasksPanelQmlTest` 2/2, `EditorAiMaskPanelQmlTest` 2/2,
`SimplifiedChineseCatalogTest` 8/8, `MaskRangeCoverageProbeTest` 6/6 on CUDA, OpenCL (the process
did not exit after PASSED: the known #559), and Metal. `alcedo_main` builds (QML compiled by
qmlcachegen). The full suite was not run. The app was not started: the range bars of a new AI
Mask were not checked by hand in the GUI.

**Residual gaps:** the QML panel test uses fake owners (AGENTS.md: QML harness tests are not
evidence); the session, adapter, and render tests are the evidence.

## 12. Range section polish (#605, #606, #607)

Three issues change the Range section of the Mask panel. The persisted Mask format does not
change.

- [#605](https://github.com/zidage/AlcedoStudio/issues/605): the section is collapsed by default.
  The user's choice stays for the rest of the session. Selection changes and an AI Mask edit do
  not change it.
- [#606](https://github.com/zidage/AlcedoStudio/issues/606): the section title is
  "Luminance/Hue Range" (zh-CN "明度/色相范围").
- [#607](https://github.com/zidage/AlcedoStudio/issues/607): the Softness sliders and the
  Saturation Threshold slider are enabled also for a full range. This replaces the "refused while
  full" rule of RP3 (section 10.3) and the "next restriction starts at the default values" rule
  of MR4 D6 for the current target.

##### #605-#607 completion record (2026-10-29)

**Status:** complete. Branch `feature/mask-range-section-polish`, stacked on
`feature/ai-mask-new-candidate-ranges` (#604, PR #610).

**Implementation and the details that the issues did not give:**

- **Session state of the section (#605).** The editor workspace is in a Loader, so the panel is
  created again after each Library to Editor route. The state is on the application window
  (`Main.qml` `editorMaskRangeSectionExpanded`, false at the start), as
  `editorAdjustmentStackExpanded`. The panel binds `expanded` to it; a header click writes it
  and binds again. Without a host (QML tests) the panel keeps a local value. No other code writes
  the value, so a selection change or an AI Mask edit does not expand the section.
- **Title (#606).** `alcedo_main_zh_CN.ts` is edited by hand: the `Range` message of context
  `EditorMasksContextPanel` gets the new source and translation. The message count (29) does not
  change. No `lupdate` run.
- **Kept values (#607).** `EditorMaskCreationAdapter::RangeView` is the store. For a full range,
  its Softness and threshold values are the kept values of the target. `SyncRangeView` keeps
  them while the target (Grade, Mask or candidate) stays and the Mask is present. A new target
  starts at the defaults. A change of the selected Mask is a new target. An image change clears
  the selection (`OnImageClosed` or the owner selection of the new image), which is also a new
  target.
- **Commands (#607).** `editRangeValue` with "softness" or "minChroma" for a full range and no
  open edit of that range sends no command. It clamps the value to the slider bounds, keeps it,
  and publishes it. `beginRangeEdit` already seeds the edit from `RangeView`, so the first write
  of a restricted range (bar drag, value field, key step) writes the kept values in the same
  commit. `finishRangeEdit` and `resetRange` keep the values when the range becomes full.
- **Slider drag (#607).** A new `beginRangeParameterEdit(kind)` opens a range edit only for a
  restricted range. For a full range it opens nothing, so a slider drag shows no range coverage
  and writes no Mask field. `EditorMaskRangeSection.qml` uses it for the Softness and threshold
  sliders, and enables them whenever the section is available.
- **AI Mask edit.** The kept values of a stored Mask stay when its AI Mask edit opens (same
  target). A new candidate is a new target: it starts at the defaults.

**Primary success call chain (Softness at a full range, then a range drag):**

```text
AdjustmentSlider press -> RangeParameterModel.beginDrag -> beginRangeParameterEdit (full: false)
AdjustmentSlider move -> editRangeValue("hue", "softness") -> RangeIsFull -> keep in RangeView
  -> maskRangeChanged (no command, no commit)
RangeSelectorBar press -> beginRangeEdit -> range_softness_ / range_min_chroma_ from RangeView
RangeSelectorBar move -> WriteRangeEdit -> RangeJson with the kept values -> SetMaskField
RangeSelectorBar release -> finishRangeEdit -> Finish (one commit with range and kept values)
```

**Primary failure call chains:**

```text
editRangeValue("luminance", "minChroma") -> refused (the Luminance Range has no threshold)
Kept value equal to the current value -> false, no publish
Range section not available -> kept value refused
Change of the selected Mask or of the image -> new target -> defaults
```

**What was proven (executed tests, Windows `win_debug`):**

| Criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Collapsed when the panel first shows a selected Mask and when an AI Mask edit opens; the user's choice stays through selection changes, an AI Mask edit, and a new panel; the title is "Luminance/Hue Range" | `RangeSectionIsCollapsedByDefaultAndKeepsTheUserChoice`, `RangeSectionIsCollapsedWhenAnAiMaskEditOpens` | `EditorMasksPanelQmlTest` | PASS (both fail on the old QML) |
| The three sliders are enabled for a full range, not while the section is unavailable | `RangeParameterSlidersAreEnabledForAFullRange` | same | PASS (fails on the old QML) |
| Values set at a full range write no commit and no document change; a slider drag at a full range opens no edit and no coverage; the next bar drag and key step write the kept values in one commit; full again keeps the slider values; another Mask or image discards them | `ValuesSetAtAFullRangeAreKeptAndWrittenWithTheNextRangeDrag` | `EditorMaskRangeAdapterTest` | PASS |
| A reset keeps the Softness and threshold for the next restriction | `SoftnessAndSaturationThresholdBelongToARestrictedRange` (updated) | same | PASS |
| The zh-CN catalog has the new entry | `SimplifiedChineseCatalogTest` | `SimplifiedChineseCatalogTest` | PASS |

Suite totals: `EditorMasksPanelQmlTest` 5/5, `EditorMaskRangeAdapterTest` 19/19,
`EditorMaskCoveragePreviewTest` 20/20, `AnalyticMaskCreationTest` 30/30,
`EditorAiMaskPanelQmlTest` 2/2, `SimplifiedChineseCatalogTest` 8/8. `alcedo_main` builds (QML
compiled by qmlcachegen). The full suite was not run. The app was not started.

**Residual gaps:** the QML panel tests use fake owners; the adapter test is the evidence for the
kept values.
