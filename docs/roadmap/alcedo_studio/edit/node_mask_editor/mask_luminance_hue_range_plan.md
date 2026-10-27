# Mask Luminance Range and Hue Range - Phase Plan

Date: 2026-10-26. Status: MR1 complete (2026-10-26, uncommitted on branch
`feature/mask-ranges-mr1-model`); MR2-MR4 planned.

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
| D11 | One coverage source at a time (SM8 C8): selecting the range probe clears an AI candidate/committed preview and the reverse. While the AI Mask tool edits a new candidate, the range bars are disabled (the candidate is not a `MaskModel` yet). |
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
