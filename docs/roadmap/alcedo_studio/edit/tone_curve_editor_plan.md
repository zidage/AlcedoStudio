# Tone Curve Editor Plan

Date: 2026-11-13

Status: TC1 and TC2 complete. TC3 to TC6 planned.

Tracking issue: [#885](https://github.com/zidage/AlcedoStudio/issues/885). Items: TC1
[#886](https://github.com/zidage/AlcedoStudio/issues/886), TC2
[#887](https://github.com/zidage/AlcedoStudio/issues/887), TC3
[#888](https://github.com/zidage/AlcedoStudio/issues/888), TC4
[#889](https://github.com/zidage/AlcedoStudio/issues/889), TC5
[#890](https://github.com/zidage/AlcedoStudio/issues/890), TC6
[#891](https://github.com/zidage/AlcedoStudio/issues/891).

Related plans: [color_wheel_tonal_lgg_plan.md](color_wheel_tonal_lgg_plan.md) (same grade stage,
same ACEScc axis, same compatibility rule for old documents).

Before work on a phase starts, read `AGENTS.md`, this plan, the phase issue, and the skills that
apply (`alcedo-qml-ui` for TC4 and TC5, `alcedo-msvc-cmake` for builds) again. The rules can change
after this plan.

## 1. Decision record

The user wants the Tone Curve section of the Tone panel to become a precise curve editor, with
the control of the RawTherapee curve editor and a modern look:

- A thin curve line and small ring handles. The pointer moves a control point 1:1, with no
  slow-down gain.
- The histogram of the pixels at the input of the curve shows behind the curve.
- Four curves: RGB (the current curve), R, G, and B.
- A preset menu: contrast shapes, the Adobe default tone curve ("Film-like"), the darktable filmic
  shape, and the tone of Kodak 2383, Velvia 50, and Ektachrome E100.
- The render applies the same spline that the panel draws, on CUDA, OpenCL, and Metal.
- A new reusable plot pane draws the plot. The Tone Curve section is its only user in this work.

Approved decisions (tracking issue #885, "Decisions"):

1. **Render of an existing curve.** Confirmed by the user on 2026-11-13: every curve renders as the
   spline that the panel draws. A document with a curve of three or more points gets slightly
   different pixels. The identity curve and a two-point curve render as before. The stored JSON
   does not change. There is no `interpolation` key.
2. **Pointer.** 1:1 with no gain. Precision comes from the arrow keys (0.001, with Shift 0.01), the
   In and Out fields, Shift + drag (one axis), and the pane zoom.
3. **Curve order.** The RGB curve applies first, then the R, G, and B curves, per channel on ACEScc
   values.
4. **Limits.** 16 points for each curve. Outside the end points the curve continues along the end
   slope.
5. **Histogram.** The pixels at the input of the Curve stage, on the axis of the curve.
6. **Presets.** A preset is a list of control points. The document stores only points.
7. **Preset shapes.** Fitted on the curve axis with 18% grey fixed.
8. **Film-like.** The shape of the Adobe default tone curve. The application rule stays per
   channel.
9. **Out of scope.** Parametric curves, a point picker in the viewport, and other users of the plot
   pane.

Rejected: a sampled table in the Grade slot for the render (see 4.2). Do not reintroduce it.

## 2. Product design

The panel design is in the "Panel design" section of #885. TC5 implements it and records the
final layout here.

Data rules for all phases:

- The curve axis is the ACEScc value of the grade stage. 18% grey is at 0.4135. One stop is
  0.0571.
- A curve has 2 to 16 control points. The x values strictly increase. The parameter write also
  requires x and y in [0, 1].
- The stored JSON of the RGB curve is `{"points":[{"x":..,"y":..}, ...]}`, as before this work.
  `CurveModel::ToJson` writes new keys only for a channel curve that is not the identity (TC2). A
  document from before this work encodes to the same bytes and keeps its root id.
- `alcedo-cli` accepts the current `{"curve":{"size":N,"points":[...]}}` write with no change.

## 3. Current source audit (before TC1)

| Area | Owner and path | Behavior before TC1 |
| --- | --- | --- |
| Panel drawing | `EditorToneCurveItem` (`ui/alcedo_main/album_backend/editor_tone_curve_item.cpp`) | Monotone cubic Hermite from `curve::BuildCurveHermiteCache` and `curve::EvaluateCurveHermite` (`ui/alcedo_main/editor_support/modules/curve.cpp`). Output clamped to [0, 1], flat outside the end points. Up to 12 points (`kCurveMaxControlPoints`). `kPointerGain = 0.32`. |
| Model | `CurveModel` (`edit/operators/models/curve_model.cpp`) | One list of points. `LoadJson` keeps the stored points with no check. |
| Parameter write | `ParseCurveWrite` (`app/editor_parameter_write_parse.cpp`) | At least two points, x and y in [0, 1], x strictly increases. No upper limit. |
| Render packing | `MakeGradeRuntimeParams` (`edit/runtime/adjustment_runtime.cpp`) | `kMaxCurvePoints = 8`: points 9 to 12 had no effect on the render. Layout `values[2i] = x`, `values[2i+1] = y`. |
| Kernels | `ApplyCurve` in `cuda_primary_grade_pass.cu`, `primary_grade.cl`, `primary_grade.metal` | Straight segments between the points, straight continuation outside them. |
| Order | `ColorGradeCompileOrder` (`edit/graph/adjustment_ownership.cpp`) | Curve after White Balance, Exposure, Contrast, Whites, and Blacks. |

## 4. Target architecture

### 4.1 One spline evaluator

`alcedo::tone_curve` (`edit/operators/models/tone_curve_spline.hpp`, Qt-free) owns the spline:

- `FindToneCurvePointsError(points)`: the reason why the points are not a valid spline input, or
  nullopt. Valid: 2 to 16 points, finite coordinates, strictly increasing x.
- `SolveToneCurveSpline(points) -> ToneCurveSpline`: the slope at each point. Interior slope: the
  weighted harmonic mean of the two secant slopes, `(w1 + w2) / (w1 / d0 + w2 / d1)` with
  `w1 = 2 h1 + h0` and `w2 = h1 + 2 h0`, and 0 when the secant slopes `d0` and `d1` do not have the
  same sign (the rule of the panel before TC1). End slope: the secant slope of the end segment.
  Throws `std::invalid_argument` with the text of `FindToneCurvePointsError`.
- `EvaluateToneCurveSpline(spline, x)`: inside the points
  `y0 + t * dy + h * t * (1 - t) * ((1 - t) * (m0 - d) - t * (m1 - d))`, outside them
  `y_end + m_end * (x - x_end)`.

The Hermite form above is algebraically equal to the standard basis `h00, h10, h01, h11`. Its
cubic term is exactly 0 on a straight segment, so a two-point curve gives the same float value as
the straight segment `y0 + t * (y1 - y0)` of the kernels before TC1, and the identity curve is an
exact identity for every input.

The panel solves its normalized points with `curve::SolveCurveSpline` and evaluates them with
`EvaluateToneCurveSpline`. The kernels evaluate the same formula with the same float operations.

### 4.2 GPU layout of a Curve slot

The host solves each curve one time per Model revision (`MakeGradeRuntimeParams`, through
`BindOrRefreshGradeRuntimeSlot`). The 48 floats of `GradeAdjustmentParams` hold the solved spline
of 16 points exactly:

| Field | Content |
| --- | --- |
| `count` | Number of control points (2 to 16) |
| `values[0..15]` | x of each point |
| `values[16..31]` | y of each point |
| `values[32..47]` | slope at each point |

Reason for this layout instead of a sampled table (the first text of #886): a table inside the
48-float slot has at most 46 samples, and linear interpolation between them changes the curve by
up to `h^2 / 8 * |f''|` (about 0.003 on the axis, 0.05 stop, for a strong S curve), with a break
of the slope at each sample. A table in a separate GPU resource needs a new binding on three
backends. The slot layout gives the exact spline for a search of at most 15 steps per channel.

TC2 extends the layout for the R, G, and B curves: the Curve slot becomes four consecutive
`GradeAdjustmentParams` records (RGB, R, G, B), and `ApplyCurve` applies the first record to the
three channels and each other record to its channel. The binding size rule is in the TC2 section
(`GradeRuntimeParamBytes`: 832 bytes for Curve, 208 bytes otherwise).

### 4.3 Failure behavior

- A parameter write with more than 16 points fails with `curve.points must contain at most 16
  points`. The Model does not change.
- A stored Model with points that the spline does not accept (more than 16 points, x that does
  not strictly increase, a value that is not finite) loads with no error and keeps its bytes. The
  render of that document fails with the text of `FindToneCurvePointsError` (for example
  `Tone curve has 17 control points; a curve has 2 to 16 points`). The panel and the parameter
  write never produced such points, and the render does not drop points without an error.

## 5. Phases

| Phase | Issue | Scope | Prerequisites | Expected diff |
| --- | --- | --- | --- | --- |
| TC1 | #886 | Shared spline evaluator, kernels, 16-point write limit | none | ~600 lines |
| TC2 | #887 | R, G, and B curves in Model, write, catalog, history, kernels | TC1 | ~1200 lines |
| TC3 | #888 | Histogram of the Curve stage input on three backends | none | ~1500 lines |
| TC4 | #889 | Reusable plot pane (view transform, layers, input) | none | ~1800 lines |
| TC5 | #890 | Tone Curve section on the plot pane | TC2, TC3, TC4 | ~1800 lines |
| TC6 | #891 | Preset study, preset data, preset menu, CLI | TC5 | ~1200 lines |

Each later phase writes its detailed section (files, call chains, tests, completion record) in this
plan before its work starts. The required behavior and the tests of each phase are in its issue.

### TC1: one spline evaluator, rendered by the three kernels

**Objective.** The render applies the spline that the panel draws, for 2 to 16 points.

**Files.**

- New: `edit/operators/models/tone_curve_spline.hpp`, `tone_curve_spline.cpp` (in
  `EDIT_GRAPH_SRCS`).
- Changed: `edit/runtime/adjustment_runtime.cpp` (packing),
  `edit/runtime/cuda/cuda_primary_grade_pass.cu`, `edit/runtime/opencl/shader/primary_grade.cl`,
  `edit/runtime/metal/shader/primary_grade.metal` (`ApplyCurve`),
  `app/editor_parameter_write_parse.cpp` (limit),
  `ui/alcedo_main/editor_support/modules/curve.cpp` and `curve.hpp` (`SolveCurveSpline`, limit 16),
  `ui/alcedo_main/album_backend/editor_tone_curve_item.cpp` (drawing samples).
- Removed: `curve::CurveHermiteCache`, `curve::BuildCurveHermiteCache`,
  `curve::EvaluateCurveHermite`, the kernel `ExtrapolateCurve`.

**Success call chain (render).**

1. `CurveModel` holds the points (`SetPoints` from `ApplyEditorParameterWrite` or `LoadJson`).
2. The grade pass of the backend calls `BindOrRefreshGradeRuntimeSlot`, which calls
   `MakeGradeRuntimeParams(model, AdjustmentBehavior::Curve)` when the Model revision changed.
3. `MakeGradeRuntimeParams` reads the points under the Model lock and calls
   `tone_curve::SolveToneCurveSpline`. It writes `count`, x, y, and slope into the slot.
4. `ParameterArena::UploadDirty` uploads the slot. The kernel `ApplyCurve` evaluates the spline
   for each channel.

**Success call chain (panel).** `EditorToneCurveItem::updatePaintNode` ->
`BuildToneCurveSceneGeometry` -> `curve::SolveCurveSpline` -> `tone_curve::EvaluateToneCurveSpline`
for each drawing sample.

**Failure call chain.** `ParseCurveWrite` -> `std::invalid_argument("curve.points must contain at
most 16 points")` -> `ParseEditorParameterWrite` returns nullopt with the error text. For a
stored Model with invalid points: `MakeGradeRuntimeParams` -> `SolveToneCurveSpline` throws
`std::invalid_argument` -> the render fails with that text.

**Tests.**

- `GpuDagModelGraphTest.ToneCurveSplineTest.*`: identity is exact also below 0 and above 1; a
  two-point curve equals the straight segment; the spline equals the standard Hermite basis of
  the panel rule within 1e-6 for 3 and 16 points; the spline passes through every point and stays
  between the values of each segment; the end-slope continuation; named errors for 1 point, 17
  points, equal x, and a value that is not finite; stored curve JSON encodes to the same bytes.
- `GpuDagRawInputTest.GpuDagAdjustmentRuntime.*`: the packed layout equals the solved spline; 16
  points pack completely; 17 points fail with the named error.
- `EditorParameterCatalogTest.CurveWriteAcceptsSixteenPointsAndRejectsSeventeen`.
- `EditorToneCurveGeometryTest.DrawingSamplesEqualTheRenderSplineAtTheSameInputs`.
- `GpuDagOpenClGradeTest`, `GpuDagCudaPrimaryGradeTest`, `GpuDagMetalGradeTest`:
  `*CurveMatchesSplineReferenceForThreeAndSixteenPoints`. Every pixel of a ramp agrees with
  `EvaluateToneCurveSpline` within 1e-5 on the ACEScc axis. The control points cover the middle
  80% of the input values, so the test covers the cubic and the continuation on both sides. The
  CPU references of the OpenCL and Metal grade tests read the packed slot through
  `tone_curve_test::ApplyPackedCurve`.

**Completion record.** See section 6.

### TC2: R, G, and B curves

**Objective.** The Curve adjustment holds four curves: RGB (the `points` from before TC2), red,
green, and blue. The render applies the RGB curve to the three channels, then each channel curve
to its channel.

**Model.** `CurvePayload` has `points` (RGB), `red`, `green`, and `blue`; each is the identity
`(0, 0) - (1, 1)` by default. `CurveChannel` (`Rgb`, `Red`, `Green`, `Blue`) names a curve, and
`CurveChannelKey` gives its JSON key (`points`, `red`, `green`, `blue`). `CurveUpdate` has an
optional list of points for each curve; `CurveModel::ApplyUpdate(CurveUpdate)` sets the curves
that the update has in one Model change (one revision, one dirty bit for each changed curve). A curve with
fewer than two points becomes the identity, as `SetPoints` did before. `IsDefault` is true only
when the four curves are the identity.

**JSON.** `ToJson` writes `"points"` always and `"red"`, `"green"`, and `"blue"` only for a curve
that is not exactly the identity. A channel curve is a list of `{"x", "y"}` objects, the same form
as `points`. `LoadJson` reads the complete state: a channel key that the JSON does not have is the
identity. A document from before TC2 encodes to the same bytes and keeps its root id.

**Parameter write.** `EditorCurveWrite` is `CurveUpdate` (as `EditorLutWrite` is `LmtUpdate`).
`ParseCurveWrite` accepts `points`, `size`, `red`, `green`, and `blue`; another key fails with
`Unknown parameter: curve.<key>`. Each curve has the rules of section 2 (2 to 16 points, unit
square, x strictly increases), with the key in the error text (`curve.red[1].x must be in
[0, 1]`). `size` counts the RGB points and needs `points`. The two parse forms differ:

- A write (`ParseEditorParameterWrite`: `alcedo-cli`, the panel) has one or more of the four keys
  and leaves the other curves as they are. A write with no key fails with `curve requires points,
  red, green, or blue`. The current `{"curve":{"size":N,"points":[...]}}` write keeps the channel
  curves.
- A complete value (`ParseEditorParameterValue`: history replay, `RestoreDocumentFields`) is the
  Model JSON and needs `points`. A channel key that it does not have is the identity, because the
  Model omits an identity channel curve. Without this rule, the undo of the first blue edit (stored
  `before` value without `"blue"`) would keep the blue curve.

The catalog reset value of `curve` has the four keys with the identity, so a reset through
`editor.set` resets the four curves. `model_shape` describes the four keys and the write rule.

**GPU layout and binding size.** The Curve slot is `CurveGradeRuntimeParams`: four consecutive
`GradeAdjustmentParams` records (832 bytes) in `CurveChannel` order, each in the layout of 4.2.
`GradeRuntimeParamBytes(behavior)` gives the slot size: 832 bytes for Curve, 208 bytes for every
other behavior. `BindOrRefreshGradeRuntimeSlot` packs with `MakeCurveGradeRuntimeParams` for
Curve and binds the slot at that size; the command offset is the offset of the first record.
`MakeGradeRuntimeParams` fails for Curve with a named error, so no caller packs one curve of four.
The RGB record always has its points (an identity RGB curve has count 2, as in TC1). A red, green,
or blue curve that is the identity has count 0, and `ApplyCurve` returns the value unchanged for
count 0, so an identity channel costs one comparison. The kernels apply
`ApplyCurve(ApplyCurve(c, record[0]), record[1 + channel])` on CUDA, OpenCL, and Metal.

**Panel projection and presentation.** `EditorPanelCurveValue` has the four lists, and the QML map
of `curve` has `points`, `red`, `green`, and `blue`. The current panel edits `points` only, and its
write leaves the channel curves as they are. TC5 adds the channel selector.

**Adjustment transfer.** The transfer copies the Model JSON and loads it with `LoadJson`, so the
four curves transfer. The row text is the RGB point count and the point count of each channel
curve that is not the identity, for example `4 points, R 3, B 5`.

**Success call chain (write).** `alcedo-cli editor.set curve` -> `EditorParameterCatalog::
ToModelJson` -> `ParseEditorParameterWrite` -> `ParseCurveWrite(params, ParseForm::Write)` ->
`ApplyEditorParameterWrite` -> `CurveModel::ApplyUpdate`.

**Success call chain (undo).** `ApplySetParameter` -> `ApplyEditorParameterValue` ->
`ParseEditorParameterValue` -> `ParseCurveWrite(value, ParseForm::Value)` (absent channel keys are
the identity) -> `CurveModel::ApplyUpdate`.

**Success call chain (render).** `BindAndScheduleGrade` -> `BindOrRefreshGradeRuntimeSlot` ->
`MakeCurveGradeRuntimeParams` (four solved splines) -> `ParameterArena::WritePackedSlot` (832
bytes) -> kernel `ApplyAdjustment` reads the records at the command offset.

**Failure call chain.** A write with an invalid channel curve -> `ParseCurvePoints` throws
`std::invalid_argument` with the key in the text -> `ParseEditorParameterWrite` returns nullopt;
the Model does not change. A stored channel curve that the spline does not accept ->
`MakeCurveGradeRuntimeParams` throws the text of `FindToneCurvePointsError` -> the render fails.

**Tests.**

- `GpuDagModelGraphTest.CurveModelTest.*`: identity channel curves encode without the new keys; a
  payload with a blue curve decodes and encodes to the same bytes; `LoadJson` without a channel key
  sets the identity; `IsDefault` only for four identity curves; `Apply` changes only the given
  curves in one revision.
- `PipelineDocumentCheckpointTest.PipelineDocumentCheckpointFormat.
  ExistingCurveRootDocumentsSerializeByteIdenticalAfterChannelCurves`: a stored root with a
  four-point RGB curve in the JSON from before TC2 decodes and encodes to the same bytes.
- `EditorParameterCatalogTest`: `CurveWriteWithPointsOnlyLeavesTheChannelCurvesAsTheyAre`,
  `CurveWriteWithOneChannelKeySetsOnlyThatCurve`,
  `CurveWriteRejectsUnknownKeysAndInvalidChannelPointsWithNamedErrors`,
  `CurveValueWithoutAChannelKeyReadsThatChannelAsIdentity`, `CurveResetValueResetsTheFourCurves`.
- `PipelineHistoryApplierTest.UndoOfABlueCurveEditRestoresTheEarlierBlueCurveAndKeepsTheRgbCurve`.
- `EditorPanelProjectionTest` (the projection has the four curves) and
  `AdjustmentTransferCatalogTest.CurveItemSummaryNamesTheChannelCurves`.
- `GpuDagRawInputTest.GpuDagAdjustmentRuntime.*`: four records with count 0 for identity channel
  curves; the slot binds 832 bytes and repacks after a channel edit; the 17-point error also for a
  channel curve.
- `GpuDagOpenClGradeTest`, `GpuDagCudaPrimaryGradeTest`, `GpuDagMetalGradeTest`:
  `*ChannelCurvesMatchSplineReferenceForRedAloneAndRgbWithBlue`. Every pixel agrees with
  `EvaluateToneCurveSpline` (RGB curve, then the channel curve) within 1e-5 for a red curve alone
  and for the RGB curve with a blue curve. The CPU references of the OpenCL and Metal grade tests
  read the four records through `tone_curve_test::ApplyPackedCurveSlot`.

### TC3 to TC6

See the issues. TC5 removes `kPointerGain` and moves `EditorToneCurveItem` to the archive of
deprecated code when the section no longer uses it.

## 6. Completion record

### TC1 (2026-11-13)

Status: complete. Branch `fix/tone-curve-render-spline`.

Decisions made during the work:

- GPU layout: solved spline in the slot (4.2), not a sampled table.
- The panel draws the end-slope continuation outside the end points (before TC1 it drew a flat
  line there, and the kernels already continued along the end segment).
- The panel accepts 16 points (`kCurveMaxControlPoints` follows `tone_curve::kMaxControlPoints`).
- A stored Model with points that the spline does not accept fails at render with a named error
  (4.3).

Verification (Windows, debug build, RTX 3080 Laptop, CUDA and OpenCL):

| Binary and filter | Result |
| --- | --- |
| `GpuDagModelGraphTest` (all, includes `ToneCurveSplineTest.*` and the header check) | 190 passed |
| `GpuDagRawInputTest --gtest_filter=GpuDagAdjustmentRuntime.*` | 14 passed |
| `EditorParameterCatalogTest` (all) | 44 passed |
| `EditorToneCurveGeometryTest` (all) | 5 passed, 1 failed: `PlotRectUsesLegacyPadding`, a stale inset check on `main` ([#892](https://github.com/zidage/AlcedoStudio/issues/892)) |
| `GpuDagOpenClGradeTest --gtest_filter=OpenClGradeFixture.*` | 32 passed |
| `GpuDagCudaPrimaryGradeTest --gtest_filter=CudaPrimaryGradeFixture.*` | 31 passed, 1 failed: `CudaNeighborGradeDetailPixelsAreUnchanged`, stored detail pixels ([#690](https://github.com/zidage/AlcedoStudio/issues/690)) |

`GpuDagMetalGradeTest.MetalCurveMatchesSplineReferenceForThreeAndSixteenPoints` and the Metal
shader compile were not run locally: the Mac was not reachable. The macOS CI runs the
`ci_metal` targets. The full test suite was not run.

### TC2 (2026-11-13)

Status: complete. Branch `feature/tone-curve-channel-curves`, stacked on TC1. The `alcedo-cli`
documents: zidage/alcedo-skills#23.

Decisions made during the work (details in the TC2 section of section 5):

- `EditorCurveWrite` is `CurveUpdate`: an optional list of points for each curve.
- A write leaves the curves that it does not have as they are; a complete value (history replay)
  reads an absent channel key as the identity. This is the rule that makes the undo of a first
  channel edit work, because the stored `before` value has no channel key.
- The catalog reset value of `curve` has the four keys.
- Slot size by behavior (`GradeRuntimeParamBytes`): 832 bytes for Curve. Identity channel curves
  pack count 0. `MakeGradeRuntimeParams` refuses Curve with a named error.
- The transfer row text names the channel curves (`4 points, R 3, B 5`).

Verification (Windows, debug build, RTX 3080 Laptop, CUDA and OpenCL):

| Binary and filter | Result |
| --- | --- |
| `GpuDagModelGraphTest` (all, includes `CurveModelTest.*`, `ToneCurveSplineTest.*`, and the header check) | 195 passed |
| `GpuDagRawInputTest --gtest_filter=GpuDagAdjustmentRuntime.*` | 16 passed |
| `EditorParameterCatalogTest` (all) | 49 passed |
| `PipelineHistoryApplierTest` (all) | 15 passed |
| `EditorPanelProjectionTest` (all) | 14 passed |
| `AdjustmentTransferCatalogTest` (all) | 13 passed |
| `EditorPendingInputTest` (all) | 13 passed |
| `EditorPipelineCommandServiceTest` (all) | 24 passed |
| `EditorToneCurveModelTest` (all) | 8 passed |
| `EditorToneCurveGeometryTest` (all) | 5 passed, 1 failed: `PlotRectUsesLegacyPadding` ([#892](https://github.com/zidage/AlcedoStudio/issues/892)) |
| `PipelineDocumentCopyCostTest` (all) | 5 passed |
| `PipelineDocumentCheckpointTest` (all) | 20 passed, 3 failed: the `Existing{Raw,Lmt,FilmGrain}Root*` checks of a fresh document against stored HLS defaults ([#553](https://github.com/zidage/AlcedoStudio/issues/553)). The new `ExistingCurveRootDocumentsSerializeByteIdenticalAfterChannelCurves` passes. |
| `GpuDagOpenClGradeTest --gtest_filter=OpenClGradeFixture.*` | 33 passed |
| `GpuDagCudaPrimaryGradeTest --gtest_filter=CudaPrimaryGradeFixture.*` | 32 passed, 1 failed: `CudaNeighborGradeDetailPixelsAreUnchanged` ([#690](https://github.com/zidage/AlcedoStudio/issues/690)) |
| `AutomationReferenceDocsTest` | wrote the new `editor.catalog` schemas; alcedo-skills#23 takes them, and `generate_cli_reference.py --check` passes there |

`GpuDagMetalGradeTest.MetalChannelCurvesMatchSplineReferenceForRedAloneAndRgbWithBlue` and the
Metal shader compile were not run locally: the Mac was not reachable. The full test suite was not
run.
