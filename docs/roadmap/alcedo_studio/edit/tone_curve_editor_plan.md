# Tone Curve Editor Plan

Date: 2026-11-13

Status: TC1 to TC6 complete.

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

### TC3: histogram of the Curve input

**Objective.** The editor renders give the histogram of the red, green, and blue values at the
input of the Curve adjustment of one Color Grade, on the axis of the curve, to a QML-facing owner.
The scope analyzer cannot give it: it reads the final display frame, after the curve and the
display transform.

**Data rules.**

- 256 bins for each channel on the curve axis [0, 1]. `CurveInputHistogramBin(v)` is
  `floor(v * 256)` limited to [0, 255]: a value below 0 counts in bin 0, a value at or above 1 in
  bin 255, a value that is not a number in bin 0. The three kernels use the same float operations.
- The counted values are the grade input after the compiled adjustments before the Curve in
  `ColorGradeCompileOrder` (White Balance, Exposure, Contrast, Whites, Blacks, as far as the Grade
  has them). All of them are pointwise. The Curve and the adjustments after it are not applied.
- Sample grid: the pixels `(i * step, j * step)` of the grade input, with
  `step = ceil(long edge / 1024)` (`CurveInputHistogramSampleStep`). A Quality Base frame of 4096
  pixels is sampled every 4 pixels.
- Only a render of the whole edit frame counts the histogram: the Interactive full frame, the
  Quality Base frame, and a Detail render of the whole frame. A viewport region frame and a Detail
  patch do not (the histogram of a region is not the histogram of the image). While the user works
  zoomed in, the histogram follows the settled Quality Base frames.

**Files.**

- New: `edit/runtime/curve_input_histogram.hpp` (request, record, result, sink, bin and sample
  rules, `ReadCurveInputHistogramCounts`), `ui/alcedo_main/album_backend/
  editor_curve_histogram_controller.{hpp,cpp}`.
- Changed: `GradeExecutor` (record step), the three grade passes and kernels
  (`CurveInputHistogramKernel` in `cuda_primary_grade_pass.cu`,
  `primary_grade_curve_input_histogram` in `primary_grade.cl` and `primary_grade.metal`),
  `PlanExecutor`, the render devices, `PassEncoder<PrimaryColorGrade>` of the three backends,
  `Renderer::RenderImage`, `PipelineApplyRequest`, `RenderDesc` and `PipelineTask::
  MakeApplyRequest`, `EditorRenderIntent`, `EditorSessionRenderController`, `EditorSessionService`
  (`ShowCurveInputHistogram`, `HideCurveInputHistogram`), `MakeEditorRenderDesc`,
  `EditorSessionController` (`curveHistogram` property), `GpuNodePassStats`
  (`curve_input_histogram_execute`).

**Place in the render.** The film negative scan reduces its prescan inside the film print pass and
reads it on the host before the pass continues, because the result changes the pass. The curve
input histogram changes no pixel, so the grade pass records it on the device next to the grade
work (`GradeExecutor::RecordCurveInputHistogram`), and the renderer reads the 768 counts after the
photo frame was presented, as it publishes the Mask coverage probe. The grade pass applies the
adjustments before the Curve inside one fused kernel, so the histogram kernel evaluates those
adjustments again on the sample grid (`ApplyAdjustment` of the same shader) and adds the bins in
group-local counts, then into the device counts with atomics. The pass writes no extra image.

**Success call chain (consumer to sink).**

```text
QML Tone Curve section sets EditorCurveHistogramController.active and gradeNodeId
  -> RequestChanged -> EditorSessionController::ApplyCurveHistogramRequest
  -> IEditorSessionBackend::ShowCurveInputHistogram(grade_id, sink)        (owner command)
  -> EditorSessionRenderController::SetCurveInputHistogramRequest; one Quality render
     (EditorRenderReason::CurveInputHistogramRequested); later intents carry the request
  -> MakeEditorRenderDesc -> RenderDesc::curve_input_histogram_
  -> PipelineTask::MakeApplyRequest (whole frame only) -> PipelineApplyRequest
  -> Renderer::RenderImage -> PlanExecutor::Execute(..., CurveInputHistogramRecord*)
  -> PassEncoder<PrimaryColorGrade> -> Execute{Cuda,OpenCl,Metal}PrimaryGrade
  -> GradeExecutor::RecordCurveInputHistogram -> Ops::DispatchCurveInputHistogram (kernel)
  -> Renderer: present photo frame -> ReadCurveInputHistogramCounts
  -> ICurveInputHistogramSink::PublishCurveInputHistogram (render thread)
  -> queued to EditorCurveHistogramController::Accept (identity, epoch, Grade check)
  -> BuildScopeHistogramRenderData -> HistogramChanged
```

**Failure and edge chains.**

- The plan has no Grade with the requested id (removed Grade, another image): the record stays
  not recorded -> the sink receives a histogram with no counts -> the owner clears its histogram.
- A result of another image identity, session epoch, or Grade, or a result that arrives while the
  owner is not active -> `Accept` drops it.
- The owner is destroyed while a render runs -> its destructor detaches the sink; Qt drops a call
  that was queued before.
- A batch request or a request without a sink -> `Renderer::RenderImage` throws
  `std::invalid_argument`.
- A device download error after the frame -> the render reports the error; the photo frame and its
  results are already published.

**Tests.**

- `GpuDagCudaPrimaryGradeTest`, `GpuDagOpenClGradeTest`, `GpuDagMetalGradeTest`:
  `*CurveInputHistogramCountsTheGradeInputOnTheCurveAxis` (exact counts against the CPU reference
  of the downloaded grade input, with a curve that is not the identity),
  `*CurveInputHistogramSamplesEveryStepPixelsOfALargeInput` (2100 x 5 input, step 3, exact counts),
  `*CurveInputHistogramIgnoresCurveEditsAndFollowsExposure` (equal after a curve edit; after
  Exposure +1 each bin is within the range of the shifted CPU reference),
  `*RenderWithoutAHistogramRequestRecordsNoHistogram` (no kernel, no counts buffer; a request for
  a Grade that the plan does not have records nothing). CPU reference:
  `tests/edit/runtime/curve_input_histogram_test_support.hpp`.
- `ExecutorSnapshotRenderTest.InteractiveRenderGivesTheCurveInputHistogramToItsSink` (real RAW:
  frame identity in the result, one sample count per channel, equal after a curve edit, changed
  after an Exposure edit, no counts for a missing Grade, nothing without a request).
- `PipelineFrameSinkTest.CurveInputHistogramRidesOnlyOnRendersOfTheWholeFrame`.
- `EditorMaskCoveragePreviewTest.EditorCurveInputHistogramTest.*` (session facade: no request
  before Show; Show routes one Quality render and every frame carries the request until Hide;
  the same request routes no second render; refusal without a Grade or a sink; Show before an
  image applies to its frames).
- `EditorCurveHistogramControllerTest` (request only while active with a Grade; result of the
  current image published as `ScopeHistogramRenderData`; results of another image, epoch, or Grade
  dropped; image change and a result without counts clear the histogram; a result from another
  thread is accepted on the owner thread; the sink outlives the owner).

### TC4: reusable plot pane

**Objective.** One plot item that the Tone Curve section (TC5) draws on: a view transform with
zoom and pan, an inset for labels, ordered layers, and pointer and key routing to the layers in
data coordinates. No other panel uses it in this work.

**Parts** (library `EditorPlotPane`, `ui/alcedo_main/plot_pane/`, headers in
`include/ui/alcedo_main/plot_pane/`):

- `PlotViewTransform` (value type, Qt Core geometry only): limits, view, maximum zoom (32),
  `ToItem`, `ToData`, `ZoomAt` (the anchor keeps its item position), `Pan` (the data follows the
  pointer), `SetView`, `ResetView`. The view always stays inside the limits; its span is between
  the limits span divided by the maximum zoom and the limits span. One zoom factor applies to both
  axes. Invalid limits throw `std::invalid_argument`.
- `PlotLayer` (QObject): `visible`, `interactive`, `contentChanged`, `Build(view, geometry)`, and
  the input calls `PointerPress`, `PointerMove`, `PointerRelease`, `PointerDoubleClick`, `Hover`,
  `HoverLeave`, `Cursor`, `Key`. A layer builds premultiplied triangles and labels in item
  coordinates (`PlotLayerGeometry`) with the antialiased triangle primitives of the viewport
  overlays (`overlay_triangles`, through `plot_primitives`).
- Layers: `PlotGridLayer` (ticks `{value, label, strong}` on each axis, lines and labels),
  `PlotHistogramLayer` (filled areas and edge from `ScopeHistogramRenderData`, all channels
  additive or one channel in its own tint, the break across a cut peak), `PlotPolylineLayer`
  (solid or dashed line, hit test of the nearest point on the line), `PlotHandleLayer` (rings, the
  selected handle filled, a hovered or pressed handle 1 px larger; hit test with a radius in item
  pixels).
- `PlotPane` (QQuickItem): `limits`, `maximumZoom`, `view`, `zoomed`, `zoom`, `plotRect`, the four
  insets, `backgroundColor`, `surfaceColor`, `wheelZoomStep`, the default property `layers`, and
  `setView`, `resetView`, `zoomAt`, `mapToData`, `mapFromData`.
- `PlotHistogramSource` (QObject with `histogram()` and `HistogramChanged`): the owner of the
  histogram. `EditorCurveHistogramController` is one, so TC5 sets it as the `source` of the
  histogram layer. The layer reads the owner's data when it builds and keeps no copy.

**Drawing.** A layer that emits `contentChanged` is marked dirty; a view, size, or inset change
marks every layer dirty. `updatePolish` (GUI thread) builds only the dirty layers and their text
layouts; `updatePaintNode` uploads only those. Each layer has a container node, so the layer order
stays fixed: it holds one geometry node (`QSGVertexColorMaterial`, premultiplied colors) while the
layer has triangles, and one text node (`QQuickWindow::createTextNode`) for its labels. The
histogram channels add up with premultiplied vertices of alpha 0 (source plus target), as the
`CompositionMode_Plus` of the scope histogram.

**Input routing.** Press: the top visible, interactive layer whose `PointerPress` accepts gets the
pointer grab (`setKeepMouseGrab`) and the moves and the release; the pane calls `PointerRelease`
with the last position when it loses the grab. A press that no layer accepts pans a zoomed view.
Hover goes to the top layer that takes it; the others get `HoverLeave`. Keys go to the top layer
that accepts them. Ctrl + wheel zooms at the pointer by `wheelZoomStep` per notch; a plain wheel
event is ignored, so the parent Flickable scrolls. The handle layer keeps the press offset, so a
point moves 1:1 with no jump; the owner moves it by setting `points` from `moved`.

**Call chain (handle drag).**

```text
QQuickWindow -> PlotPane::mousePressEvent (item -> data through PlotViewTransform)
  -> top layer PlotHandleLayer::PointerPress (HitTest, radius in item px) -> pressed(index, data)
  -> PlotPane::mouseMoveEvent -> PlotHandleLayer::PointerMove -> moved(index, data)
  -> owner sets PlotHandleLayer.points -> contentChanged -> PlotPane::MarkLayerDirty
  -> PlotPane::updatePolish -> BuildDirtyLayers -> PlotHandleLayer::Build
  -> PlotPane::updatePaintNode (uploads that layer only)
  -> PlotPane::mouseReleaseEvent -> PlotHandleLayer::PointerRelease -> released(index, data)
```

**Tokens.** `AppTheme` `plotSurfaceColor`, `plotGridColor`, `plotGridStrongColor`,
`plotLabelColor`, `plotLineColor`, `plotReferenceLineColor`, `plotHandleColor`,
`plotHandleOutlineColor`, `plotGridLineWidth`, `plotLineWidth`, `plotHandleRadius`,
`plotHandleRingWidth`, `plotHandleOutlineWidth`, `plotHitRadius`; DESIGN.md "Plot pane". The C++
defaults of the pane and the layers are the dark theme values of these tokens.

**Tests** (`EditorPlotPaneTest`): `PlotViewTransformTest.*` (identity of data to item and back,
zoom at a point, limits of pan and zoom), `PlotPolylineLayerTest.PolylineItemPointsFollowTheZoomedView`,
`PlotHandleLayerTest.PointerSevenPixelsFromAHandleHitsItAtRadiusEightAtEachZoom`,
`PlotHandleLayerTest.HandleOutsideThePlotRectangleIsNotHitOrDrawn`,
`PlotGridLayerTest.TicksInsideTheViewDrawLinesAndLabels`, `PlotPaneInputTest.*` (press, drag,
and release reach the top layer in data coordinates; plain wheel unaccepted and Ctrl + wheel zoom
at the pointer; drag in an empty area pans a zoomed view; keys only with a selected handle; double
click), `PlotPanePaintTest.*` (each layer in the rendered image; only the changed layer builds
again; two panes in one window keep their own views).

### TC5: Tone Curve section on the plot pane

**Objective.** The Tone Curve section of the Tone panel edits the four curves of TC2 on the plot
pane of TC4, over the curve input histogram of TC3, with the drawing, input, and controls of #890.

**Layout** (`EditorToneCurveSection.qml`, inside the `CollapsibleSection` "Tone Curve"):

- Header: title, then the header actions Histogram (toggle), Fit (toggle), and Reset. The
  `CollapsibleSection` gets a `headerActions` component slot at the right end of the header; a
  click on an action does not fold the section.
- Row 1: channel selector `RGB`, `R`, `G`, `B` (`SegmentedCardSwitcher`). A segment of a curve
  that is not the identity shows a 4 px dot before its title, in the ink of the segment. TC6 adds
  the preset menu to this row.
- Plot: a square `PlotPane` (limits [0, 1] x [0, 1]). Layers from the bottom: grid, histogram,
  dashed identity line, the faint curves of the other channels, the pointer line, the curve of
  the selected channel, the handles. EV labels under the plot (bottom inset).
- Pointer readout: while the pointer is over the plot, a vertical line at the pointer and, at the
  top left of the plot, `In` and `Out` with the EV of the pointer input and of the curve output.
- Row 2: `In` and `Out` fields of the selected point (plain `TextInput` in a hairline box, value
  on the curve axis with 4 decimals), each followed by its EV value. Without a selection the
  fields are empty and read-only.
- The caption sentence under the plot is removed.

**Data rules of the panel.**

- Grid: lines at `0.4135 + 2 k * 0.0571` inside [0, 1] (-6 EV to +10 EV from 18% grey, every
  2 EV) on both axes; the line at 18% grey is strong. Labels only under the plot, as signed EV
  (`−6` to `+10`). The exact axis values come from ACEScc: 18% grey is
  `(log2(0.18) + 9.72) / 17.52`, one stop is `1 / 17.52` (`curve::kCurveAxisGrey`,
  `curve::kCurveAxisStop`).
- Fit: off shows the full range [0, 1]. On shows -7 EV to +5 EV from 18% grey on both axes
  (`curve::kCurveFitMinimumEv`, `curve::kCurveFitMaximumEv`): the shadows of a photo down to the
  sensor floor and the highlights to +5 EV, where the histogram of a usual photo ends. Ctrl +
  wheel still zooms; Fit sets the view again on each change.
- Curve lines: the spline of TC1 sampled at 512 equal steps of x on [0, 1] plus each control
  point (`curve::SampleCurveLine`), drawn with `PlotPolylineLayer`.
- Histogram: `EditorSessionController.curveHistogram` is the `source` of the histogram layer.
  `RGB` shows the three channels; `R`, `G`, `B` show their channel. The section sets `active`
  while the Histogram toggle is on, the section is open, and the Tone panel is visible.
  `EditorSessionController` sets `gradeNodeId` itself: it is the node of the `curve` field of the
  current panel projection (the Color Grade whose curve the panel shows and writes), so QML does
  not resolve node ids.

**Input** (owner: `EditorToneCurveModel`; the layers only report positions):

- Handle press: `PlotHandleLayer.pressed` -> `beginDrag(index)`; `moved` -> `updateDrag(x, y,
  modifiers)`; `released` -> `finishDrag()`. The handle layer keeps the press offset, so the
  point follows the pointer 1:1. `kPointerGain` is gone with `EditorToneCurveItem`.
- Shift + drag keeps one axis: the axis with the larger motion from the press position moves, the
  other keeps the press value.
- A press on the curve line within 6 px (`plotCurveHitRadius`) of the selected curve:
  `PlotPolylineLayer.pressed` gives the nearest point of the line; `insertPointOnCurve(x)` adds a
  point at `(x, curve(x))` and starts a drag. The polyline layer keeps the press offset like the
  handle layer (changed in TC5), so the new point follows the pointer 1:1.
- A press that no layer takes: `PlotPane.backgroundPressed` (new) -> `clearSelection()`. The pane
  pans a zoomed view as before.
- Keys (`PlotHandleLayer.keyPressed`): arrows move the selected point by 0.001, with Shift by
  0.01; Delete and Backspace remove it.
- Double click on an inner point removes it. A double click elsewhere does nothing.
- Reset sets the selected curve to the identity.

**History.** Each change below gives exactly one settled write of the `curve` field; a write has
only the key of the edited curve (`EditorCurveWrite` with one list), so the other curves stay:

| Change | Interactive writes | Settled write |
| --- | --- | --- |
| Drag of a handle or of a new point | one for each move that changes the point | at release, only when the point moved or was added |
| Key move sequence | one for each key | 500 ms after the last key (`keySettleIntervalMs`), or at once when the selection, the channel, or a drag starts |
| In or Out field edit | none | at Enter or focus out, when the value changed |
| Removal, Reset | none | at once (none when nothing changes) |

A press on a handle with no motion writes nothing, so a click that only selects a point and the
first click of a double click add no history entry.

**Load.** `loadFromSnapshotEntry` reads `points`, `red`, `green`, and `blue` of the projected
`curve` map (an absent channel list is the identity) without a write, and only while no drag
runs. The selected channel is panel state: a load does not change it. The editor workspace is
retained after its first creation (`WorkspaceHost.editorRetained`), so the channel stays during a
workspace re-entry. A load that changes the points of the selected curve clears the selection.

**Files.**

- Changed: `EditorToneCurveModel` (four curves, selected channel, selection, key and field
  edits), `curve.hpp` (axis constants, `SampleCurveLine`), `PlotPane` (`pointerInside`,
  `pointerPosition`, `backgroundPressed`), `PlotPolylineLayer` (press offset),
  `EditorSessionController` (curve histogram Grade from the projection), `CollapsibleSection.qml`
  (`headerActions`), `EditorTonePanel.qml`, `AppTheme` and `DESIGN.md` (tokens of the section),
  `resource.qrc` (`fit-range.svg`).
- New: `EditorToneCurveSection.qml`, `panel_icons/fit-range.svg` (Lucide `scan`).
- Archived (unchanged, outside the build): `EditorToneCurveItem` and
  `editor_tone_curve_geometry_test.cpp` to `alcedo_studio/deprecated/tone_curve_item/`.

**Call chain (handle drag).**

```text
PlotPane::mousePressEvent -> PlotHandleLayer::PointerPress -> pressed(index)
  -> QML -> EditorToneCurveModel::beginDrag(index)        (selects, records the press point)
PlotPane::mouseMoveEvent -> PlotHandleLayer::PointerMove -> moved(index, data, modifiers)
  -> EditorToneCurveModel::updateDrag(x, y, modifiers)    (axis lock, neighbor limits)
  -> submitNow(EditorCurveWrite{<channel>: points}, settled=false) -> pointsChanged
  -> QML sets PlotHandleLayer.points and PlotPolylineLayer.points
PlotPane::mouseReleaseEvent -> released -> EditorToneCurveModel::finishDrag()
  -> submitNow(..., settled=true) -> settledCommitted
```

**Call chain (histogram).** `EditorSessionController::NotifyChange` (panel projection current) ->
`SyncCurveHistogramGrade` -> `EditorCurveHistogramController::set_grade_node_id` ->
`RequestChanged` -> `ApplyCurveHistogramRequest` (TC3 chain) -> `HistogramChanged` ->
`PlotHistogramLayer` builds again.

**Failure behavior.** A write that the submitter refuses (no editable session) leaves the model
points as the user moved them; the next projection load sets the document points again. An
insertion at a curve of 16 points or within 0.001 of an end point does nothing and starts no
drag. A field text that is not a number keeps the point and shows the point value again.

**Tests.**

- `EditorToneCurveModelTest`: `HandleDragToAPanePositionGivesExactlyThatDataPosition` (a
  `PlotHandleLayer` press at a handle, a move to an item position: the point equals
  `PlotViewTransform::ToData` of that position), `PointAddedOnTheCurveKeepsTheCurveValuesAt33Inputs`
  (a point at x 0.6 of a five-point S curve: the curve moves by at most 0.003 on the axis, 0.05
  stop, at the 33 inputs i / 32; the new slopes cause the change),
  `ShiftDragMovesOnlyTheAxisWithTheLargerMotion`, `ArrowKeysMoveTheSelectedPointAndSettleOnceAfterTheSequence`,
  `FieldEditsWriteOneSettledEntryEach`, `RemovalAndResetWriteOneSettledEntryForTheSelectedCurve`,
  `ChannelChangeSettlesThePendingKeyMovesAndEditsTheNewCurve`,
  `PressWithoutMotionWritesNothing`, `SnapshotLoadReadsTheFourCurvesWithoutAWrite`.
- `EditorAdjustmentSnapshotQmlTest.ToneCurveSectionLoadsFourCurvesAndMarksTheChangedChannels`.
- `EditorPlotPaneTest`: `PlotPanePaintTest.CurveLineIsOnePointTwoFivePixelsWideAtItsPosition`
  (coverage of a column across a horizontal curve line), `PlotPaneInputTest.
  PressThatNoLayerTakesReportsTheBackgroundPress`, `PlotPaneInputTest.PointerPositionFollowsTheMovesAndLeavesWithTheHover`,
  `PlotPaneInputTest.PolylineDragKeepsThePressOffset`.
- `EditorCurveHistogramControllerTest` and `EditorSessionRenderSchedulerPortTest` (callers).

### TC6: Tone Curve presets

**Objective.** A preset menu in the Tone Curve section sets the points of the selected curve:
Linear, Soft, Medium, and Strong Contrast, Film-like, Filmic, Kodak 2383, Velvia 50, and
Ektachrome E100. `alcedo-cli` lists the presets and applies one by id. Film-like is the RGB curve
of the RAW import look (user decision of 2026-11-13: "The default look will be Film-like").

**Study.** [tone_curve_preset_study.md](tone_curve_preset_study.md) and
`scripts/fit_tone_curve_presets.py` give the source, the points, and the fit error of each preset.
The fit target is the source tone divided by the neutral response of ACES 2.0 on the curve axis,
with 18% grey fixed at (0.41359, 0.41359) by an exposure shift of the source (study section 4.1).
Each preset has at most 15 points, and each fit error is at most 0.51 display codes.

**Data** (`edit` layer, Qt-free):

- `tone_curve_presets.hpp`: `ToneCurvePreset` (id, English display name, menu group, points),
  `ToneCurvePresets()` in menu order, `FindToneCurvePreset(id)`, `MatchToneCurvePreset(points)`
  (exact float equality), `kFilmLikeToneCurvePresetId`.
- `tone_curve_preset_points.inc`: the fitted points, written by the script.
- The document stores only points. No Model key, no format change.

**Apply.**

- Panel: `EditorToneCurveModel::applyPreset(id)` replaces the points of the selected curve and
  writes one settled entry with only that curve (none when the points are already the preset).
  `presetId` is the id of the preset whose points equal the selected curve (empty for Custom);
  it notifies with `pointsChanged`, so it follows edits, loads, and the channel.
- Parameter write: in a write of `curve`, the value of `points`, `red`, `green`, or `blue` can be a
  preset id instead of a list (`{"curve": {"points": "film_like"}}`); `ParseCurveWrite` writes the
  points of the preset. A stored value (history replay) holds points only.
- `alcedo-cli`: `curve.presets` lists id, name, group, points, and `editor_value`
  (`{"points": "<id>"}`) for `editor.set` on `curve`.

**Menu** (`EditorToneCurveSection.qml`): Row 1 holds the channel selector and the preset button
(equal widths). The button shows the translated name of `presetId`, or "Custom". The menu is the
shared `AppContextMenu` with the presets in four groups split by separators; a check marks the
preset of the selected curve. QML translates the names by id (`presetLabel`).

**RAW import look.** `CreateDefaultPipelineDocument` sets the RGB curve of Color Grade 1 to the
Film-like points (`kDefaultPipelineToneCurvePresetId`), with exposure +1.5 EV and saturation 1.3;
contrast stays 0 (the curve carries the contrast; study section 6). `kDefaultPipelineContrast` is
gone; `kImportLookContrastBeforeFilmLike = 15` names the contrast of documents imported before.
Entry resets: a profile tone entry and a Film Print entry each write the identity RGB curve while
the RGB curve still has the Film-like points (`FilmPrintEntryCurveTarget`,
`ProfileToneEntryResetTargets`); contrast 15 of an older document still resets on a profile tone
entry.

**Call chain (menu).**

```text
AppMenuItem.triggered -> EditorToneCurveModel::applyPreset(id)
  -> FindToneCurvePreset(id) -> ReplaceChannelPoints(channel, preset points) -> pointsChanged
  -> CommitSettled -> submitNow(EditorCurveWrite{<channel>: points}, settled=true)
```

**Call chain (CLI).** `editor.set {"field":"curve","value":{"points":"film_like"}}` ->
`PrepareWrite` -> `ParseEditorParameterWrite` -> `ParseCurveWrite` -> `ParseCurvePresetPoints`
-> `EnqueueFieldWrite(settled)`.

**Tests.**

- `ToneCurvePresetTest`: `MenuListsTheNinePresetsWithUniqueIds`,
  `EachPresetHasTwoToSixteenPointsWithRisingXInTheUnitSquare`, `EachPresetKeepsGreyAndIsMonotone`
  (|curve(g) - g| <= 1e-4), `MatchFindsThePresetOfExactlyItsPoints`,
  `StudyToneScaleIsTheProductAces2NeutralResponse`,
  `RenderedNeutralAxisAgreesWithTheStudySourceToneWithinTheFitError` (product spline and product
  ACES 2.0 neutral response at 257 axis samples, fit error + 0.05 codes).
- `ToneCurvePresetFilmSourceTest.NeutralToneOfEachFilmIsTheToneOfTheStudy` (film resources).
- `EditorParameterCatalogTest`: `EachToneCurvePresetPassesTheCurveWrite`,
  `CurveWriteAppliesAPresetById`.
- `EditorToneCurveModelTest`: `PresetListIsTheMenuOrderOfTheEditLayer`,
  `ApplyPresetWritesThePointsOfTheSelectedCurveInOneSettledEntry`,
  `MenuNameFollowsThePointsPresetAfterApplyCustomAfterAnEdit`.
- `AutomationEditorCommandsTest.CurvePresetsListsThePresetsWithTheirPointsAndEditorValue`.
- Import look: `GpuDagModelGraphTest`
  `DefaultPipelineDocumentBakesOnePointFiveEvSaturationOnePointThreeAndTheFilmLikeCurve`,
  `EditorSessionHistoryPortTest` (profile tone entry cases with the curve, and
  `DocumentImportedBeforeFilmLikeResetsContrastFifteen`; the pinned hash cases open a root with
  the earlier look), `DocumentTransferTest`, `ImportPipelineDocumentTest`.

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

### TC3 (2026-11-13)

Status: complete. Branch `feature/tone-curve-input-histogram`, stacked on TC2. The design, call
chains, and test list are in the TC3 section of section 5.

Decisions made during the work:

- The request is presentation state of a render request (`CurveInputHistogramRequest` on
  `EditorRenderIntent`, `RenderDesc`, and `PipelineApplyRequest`), as the Mask coverage probe is.
  It is in no result key, so a curve edit or a histogram toggle invalidates nothing.
- Only a render of the whole edit frame carries it (`PipelineTask::MakeApplyRequest`). A viewport
  region frame and a Detail patch do not.
- The grade pass records the histogram with a separate kernel that applies the adjustments before
  the Curve again on a sample grid (step `ceil(long edge / 1024)`). The fused grade kernel writes
  no intermediate image, and the prescan pattern of the film negative scan (read before the pass
  continues) is not needed, because the histogram changes no pixel. The renderer reads the counts
  after the photo frame.
- A zero-mix Grade still records the histogram of its curve input.
- A render of a document without the requested Grade gives the sink a histogram with no counts, so
  the owner shows no older histogram. The request stays across an image change.
- The session API is `ShowCurveInputHistogram` and `HideCurveInputHistogram` (owner commands); a
  changed request routes one Quality render (`EditorRenderReason::CurveInputHistogramRequested`).
- The QML-facing owner is `EditorCurveHistogramController`, the `curveHistogram` property of
  `EditorSessionController`. TC5 binds `active` and `gradeNodeId` and draws `histogram()`.

Verification (Windows, debug build, RTX 3080 Laptop, CUDA and OpenCL):

| Binary and filter | Result |
| --- | --- |
| `GpuDagCudaPrimaryGradeTest --gtest_filter=CudaPrimaryGradeFixture.*` (includes the three `CudaCurveInputHistogram*` tests and `CudaRenderWithoutAHistogramRequestRecordsNoHistogram`) | 36 passed, 1 failed: `CudaNeighborGradeDetailPixelsAreUnchanged`, stored detail pixels ([#690](https://github.com/zidage/AlcedoStudio/issues/690)) |
| `GpuDagOpenClGradeTest --gtest_filter=OpenClGradeFixture.*` (includes the four OpenCL tests) | 37 passed |
| `ExecutorSnapshotRenderTest --gtest_filter=*CurveInputHistogram*:ExecutorSnapshotRenderTest.InteractiveRendersOfOneLineageReuseSourceAndPlan:ExecutorRoleTest.*` | 4 passed |
| `PipelineFrameSinkTest` (all) | 24 passed |
| `EditorMaskCoveragePreviewTest` (all, includes `EditorCurveInputHistogramTest.*`) | 25 passed |
| `EditorCurveHistogramControllerTest` (all) | 6 passed |
| `EditorSessionRenderSchedulerPortTest` (all) | 46 passed |
| `alcedo_main` | builds |

The three `GpuDagMetalGradeTest.MetalCurveInputHistogram*` tests,
`MetalRenderWithoutAHistogramRequestRecordsNoHistogram`, and the Metal shader compile were not
run locally: the Mac was not reachable. The macOS CI builds the Metal targets. The full test suite
was not run.

### TC4 (2026-11-13)

Status: complete. Branch `feature/tone-curve-plot-pane`, stacked on TC3. The design, call chain,
and test list are in the TC4 section of section 5.

Decisions made during the work:

- The layers build in `updatePolish` on the GUI thread and `updatePaintNode` only uploads, so a
  layer reads its owner's data (points, the histogram of `PlotHistogramSource`) on the thread of
  the owner and keeps no copy.
- The layers draw with the antialiased triangle primitives of the viewport overlays, so the pane
  needs no custom material. A width is the visible width with the antialiased edge included.
- A geometry node exists only while its layer has triangles: the batch renderer did not draw a
  geometry node that it first received without vertices, even after the vertices arrived.
- The histogram layer reads a `PlotHistogramSource`; `EditorCurveHistogramController` now derives
  from it (its `HistogramChanged` signal is the one of the interface). The scope item and the
  histogram layer share the additive channel colors (`plot_primitives::AdditiveChannelChroma`);
  the scope item draws as before.
- A press that no layer accepts pans a zoomed view (the drag in an empty area of the panel
  design); Ctrl + wheel zooms by 1.25 per notch.
- `EditorPlotPaneTest` has its own `main` on the native platform plugin: the offscreen plugin
  gives the software scene graph, which draws no geometry nodes. The painting tests select the
  RHI API of the platform and grab a window that is never shown (rendered offscreen); no window
  appears.

Verification (Windows, debug build, RTX 3080 Laptop, D3D11 scene graph):

| Binary and filter | Result |
| --- | --- |
| `EditorPlotPaneTest` (all) | 14 passed |
| `EditorScopeItemPaintTest` (all) | 3 passed |
| `EditorCurveHistogramControllerTest` (all) | 6 passed |
| `alcedo_main` | builds |

The painting tests were not run on macOS (Metal): the Mac was not reachable. The full test suite
was not run.

### TC5 (2026-11-13)

Status: complete. Branch `feature/tone-curve-section`, stacked on TC4. The design, call chains,
and test list are in the TC5 section of section 5.

Decisions made during the work:

- The session sets the histogram Grade (`EditorSessionController::SyncCurveHistogramGrade`, from
  the node of the projected `curve` field); the section only sets `active`. QML resolves no node
  ids, and the histogram follows the Grade that the panel shows after a node change or an undo.
- Fit shows -7 EV to +5 EV from 18% grey; off is the full range. The view is a square, because
  the pane has one zoom factor for both axes.
- The polyline layer keeps its press offset (a change of TC4), so a point added on the curve
  follows the pointer 1:1 like a handle.
- A press with no motion writes nothing. A key move sequence settles 500 ms after the last key,
  or at once when the selection, the channel, or a drag changes.
- A load is skipped during a drag and during a key move sequence, so a projection cannot move the
  point back while the user moves it.
- The model keeps the loaded points as the document floats; a drag and the fields set doubles,
  and the write converts them to float.
- The point added on the curve is at the exact spline value. The new slopes move the curve by at
  most 0.003 on the axis (0.05 stop) at the 33 test inputs of an S curve.
- The selected channel stays in the panel model: the editor workspace is retained after its first
  creation, so it stays during a workspace re-entry.
- `EditorToneCurveItem` and `EditorToneCurveGeometryTest` moved to
  `alcedo_studio/deprecated/tone_curve_item/` (with `kPointerGain`). The stale check #892 left
  with the archived test.

Verification (Windows, debug build, RTX 3080 Laptop, D3D11 scene graph):

| Binary and filter | Result |
| --- | --- |
| `EditorToneCurveModelTest` (all) | 15 passed |
| `EditorPlotPaneTest` (all, includes the four new tests) | 18 passed |
| `EditorAdjustmentSnapshotQmlTest` (all, includes `ToneCurveSectionLoadsFourCurvesAndMarksTheChangedChannels`) | 5 passed; no QML warning from the Tone panel, the section, or `CollapsibleSection` |
| `EditorCurveHistogramControllerTest` (all) | 6 passed |
| `EditorSerialInputBoundaryTest` (all) | 7 passed |
| `EditorSessionRenderSchedulerPortTest` (all) | 46 passed |
| `EditorMaskCoveragePreviewTest` (all) | 25 passed |
| `alcedo_main` | builds (qmlcachegen of the new QML included) |

The section was not driven in the running application (no input is sent to a real window). The
painting tests were not run on macOS (Metal): the Mac was not reachable. The full test suite was
not run.

### TC6 (2026-11-13)

Status: complete. Branch `feature/tone-curve-presets`, stacked on TC5. The design, call chains, and
test list are in the TC6 section of section 5; the study is
[tone_curve_preset_study.md](tone_curve_preset_study.md). The `alcedo-cli` documents:
zidage/alcedo-skills#25 (stacked on #23).

Decisions made during the work:

- Default look (user, 2026-11-13): Film-like is the RGB curve of the RAW import look. The curve
  carries the contrast, so the import look has contrast 0 (contrast +15 on top would add a second
  S). Documents imported before keep their bytes and root ids.
- The profile tone entry and the Film Print entry reset a Film-like RGB curve to the identity, as
  they reset the other parts of the import look; contrast 15 of an older document still resets on
  a profile tone entry.
- 18% grey stays fixed by an exposure shift of the source (the grey exposure of the study), so a
  preset keeps the mid-grey brightness and the source white reaches the display peak.
- Fitted points are at least 0.02 (1/3 stop) apart, so no two handles overlap in the panel.
- The film sources come from the point stages of the product film render
  (`FilmPrintPointReference`, scene light `reference`), read through
  `ToneCurvePresetFilmSourceTest`, which also checks that the resources still give that tone.
- The CLI applies a preset by id in the `curve` write (`{"points": "film_like"}`) instead of a
  separate apply command, so the apply is one `editor.set` commit; `curve.presets` lists them.
- The menu names are translated in QML by id; the edit layer keeps the English names.
- The pinned commit hashes of `EditorSessionHistoryPortTest` and the stored files of
  `PipelineDocumentCheckpointTest` were recorded with the earlier look: these tests now open a root
  with that look (`ApplyImportLookBeforeFilmLike`), and no stored value was recorded again.

Verification (Windows, debug build, RTX 3080 Laptop):

| Binary and filter | Result |
| --- | --- |
| `ToneCurvePresetTest` (all) | 6 passed; rendered neutral axis within the fit error of each preset (largest 0.51 codes, Velvia 50); study tone scale within 0.0001 codes of the product ACES 2.0 response |
| `ToneCurvePresetFilmSourceTest` (all, film resources schema 11) | 1 passed |
| `EditorParameterCatalogTest` (all) | 51 passed |
| `EditorToneCurveModelTest` (all) | 18 passed |
| `EditorAdjustmentSnapshotQmlTest` (all, includes `ToneCurvePresetMenuNamesThePresetOfTheCurveAndAppliesOne`) | 6 passed |
| `AutomationEditorCommandsTest` (all) | 9 passed |
| `GpuDagModelGraphTest` (all) | 195 passed |
| `ProfileToneReferenceTest` (all) | 11 passed |
| `ImportPipelineDocumentTest` (all) | 2 passed |
| `EditorSessionHistoryPortTest` (all, includes the profile tone entry cases) | 130 passed, 1 failed: `CheckoutAndPasteHashesAreUnchanged`, the same hash with the earlier look restored in the product ([#555](https://github.com/zidage/AlcedoStudio/issues/555)) |
| `DocumentTransferTest` (all) | 36 passed, 1 failed: `TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches` ([#484](https://github.com/zidage/AlcedoStudio/issues/484), [#554](https://github.com/zidage/AlcedoStudio/issues/554)) |
| `PipelineDocumentCheckpointTest` (all) | 20 passed, 3 failed: the `Existing{Raw,Lmt,FilmGrain}Root*` checks ([#553](https://github.com/zidage/AlcedoStudio/issues/553)) |
| `AutomationReferenceDocsTest` | wrote the new schemas; the skills branch takes them. `generate_cli_reference.py --check` reports only the `labels.*` pages that the skills repository lacks ([alcedo-skills#24](https://github.com/zidage/alcedo-skills/issues/24)) |
| `alcedo_main` | builds (qmlcachegen of the section included) |

The preset menu was not driven in the running application (no input is sent to a real window).
The Metal render was not run: the presets are points of the TC1 spline, whose Metal test runs on
the macOS CI. The full test suite was not run.
