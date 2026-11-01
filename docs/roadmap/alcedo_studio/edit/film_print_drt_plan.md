# Film Print DRT Plan

Date: 2026-11-01

Status: **Planned.** No phase has started.

Parent: [Roadmap](../../roadmap.md), section "Still Planned".

Research source: [alcedo-film-lab](https://github.com/zidage/alcedo-film-lab). The decisions are ROADMAP
U68 to U72 of that repository. The reference evaluator and the measurements are its reports P6 and P7.

Direct prerequisites:

- [GPU DAG Pipeline Rebuild](gpu_dag_pipeline_rebuild_phase_plan.md).
- [Black Pro-Mist Diffusion Filter](black_pro_mist_diffusion_plan.md): the canonical canvas, the
  published result, and the order CUDA, then OpenCL and Metal.

This plan only depends on those plans. It does not replace them.

Source audit revision: `627bd06c` on `main`. All source paths are relative to `alcedo_studio/src/` unless the
path starts with `alcedo_studio/` or `docs/`.

Before each phase, read `AGENTS.md` and each applicable skill again. These rules directly affect this work:
no fallback without user authorization, a real error on failure, no phase id in a pull request title, and the
prohibited terminology.

---

## 1. Decision record

These are user decisions of 2026-10-31 and 2026-11-01.

| Item | Decision |
| --- | --- |
| Goal | Add a spectral film print to the editor as one DRT method. |
| Route | Work directly in Alcedo Studio. The standalone GPU MVP (lab steps P8 and MVP) is cancelled. |
| Priority | Speed comes before numeric accuracy. The appearance of the print stays the target. |
| Method | The film print. The user removed the lab APD scan method because its look is not good. |
| Selections | Film, print material, and scene light. |
| Sliders | Light spread, Halation (strength only, fixed length), and Enlarger flare. |
| Fixed values | All other lab controls stay at their defaults. The spectral prior is `light_like` only. |
| Output | Rec. 709 SDR only. There is no HDR path. |
| Existing Halation | The film print skips the existing Halation operator. The film model takes its colour from the film layers. |
| Grade | The existing product grade stays in front of the method. |
| Default scene light | `reference`. |
| Default DRT of a new document | OpenDRT stays. A user selects the film print. |
| Stock data | 13 stocks have rule `grey_anchor` from the repair of lab U71. The print setup resource holds the result. |
| Look | The user reviewed the lab viewer with the speed rules and the stock repair, and accepts the look (lab U72). |

Rejected work that must not return: a 3x3 matrix in place of the spectral input, a look LUT fitted to other renders, one Gaussian in place of the
two Gaussians of the light spread, and neutralization of a stock cast.

---

## 2. Product design specification

### 2.1 Display Transform panel

The method list of `EditorDisplayTransformPanel.qml` gets the item "Film Print". When it is selected:

```text
Display Transform
  Method          [Film Print      v]
  Film            [Kodak Vision3 250D 5207 v]
  Print material  [Kodak Vision 2383       v]
  Scene light     [Reference       v]
  Light spread    ----o----------   1.00
  Halation        ---o-----------   1.00
  Enlarger flare  -o-------------   0.010
```

- The panel hides the controls of OpenDRT and ACES, the encoding space, and the peak luminance.
- The Post Processing panel hides its Halation slider while the film print is selected. The stored Halation value does not change. It returns when the user selects another method.
- All other editor behaviors (undo, history, reset) follow the `odt` field.

### 2.2 Data

| Field | JSON key in `params.film_print` of the DRT node | Default | Range |
| --- | --- | --- | --- |
| Film | `negative` | `kodak_5207` | Identifier of the stock bundle |
| Print material | `print_material` | `kodak_2383` | Identifier of the stock bundle |
| Scene light | `scene_light` | `reference` | `reference`, 5 lights, 5 scenes of the lab |
| Light spread | `light_spread` | 1.0 | 0 to 3 |
| Halation | `halation` | 1.0 | 0 to 8 |
| Enlarger flare | `enlarger_flare` | 0.01 | 0 to 0.1 |

- `DrtParamsModel` owns these fields. There is no second data model.
- `ToJson` writes the object only when the method is `film_print`, so current documents and expected serialized files do not change.
- `LoadJson` rejects a non-finite value, a value outside its range, and an unknown identifier with the real error.
- `MethodToString` adds `film_print`. An older application does not read a new document with this method; this is the same rule as other new method values.

### 2.3 Failure behavior

- A missing or damaged resource stops the render with an error that names the file. There is no substitute method.
- A film print with an encoding space other than Rec. 709, or with a linear, ST 2084, or HLG encoding, stops in `DrtOutputResolver` with an error. An export with `output_color_override` follows the same rule.
- A GPU allocation failure reports the real error through the catch of `PlanExecutor` (`device.CancelRender()`).
- Non-finite output in a test is a failure. Product code does not clamp NaN to hide it.

---

## 3. Target chain

The input is linear AP1 from the DiffusionFilter pass, the same as the other DRT methods.

| Step | Calculation | Kind | Lab source |
| --- | --- | --- | --- |
| FP1 | AP1 to XYZ, rule `spectral_locus_projection`, bilinear read of the u'v' table (192^2), layer gain: exposure Q | Point | `fast_kernels.layer_exposure` |
| FP2 | Rule `spread_per_layer_pass`: two passes, each with one Gaussian per layer | Blur | `fast_kernels.gaussian_blur_per_layer` |
| FP3 | Halation: read of the canonical Q canvas, mix per layer | Canvas and point | `fast_kernels.mix_exposure` |
| FP4 | First amounts from the curves; a Gaussian of them, on a half-size image when the sigma is at least 4 pixels | Point and blur | `negative_density`, `gaussian_blur_half_size` |
| FP5 | Rule `inhibition_two_steps`: a = F(log H - K * inhibitor), and FP4 and FP5 once more from these amounts | Point and blur | `fast_kernels.inhibited_step` |
| FP6 | Transmittance, Enlarger flare from the canonical canvas, density, print table (65^3), cube, display matrix, gamut compression, display encoding | Fused point | `flare_density`, `print_display`, `finish_display` |

This chain has 6 blur passes. Python step P6 had 12. The point stages between blurs cannot fuse, because each one
needs a blur result. FP6 fuses all stages after the Enlarger flare.

There is no reference gamut compression at the entry of this method. The current `DrtKernel` applies it to the other
methods. The lab chain has no such step, and rule `spectral_locus_projection` holds the out-of-locus colours. This is an inference; the review
of phase F5 confirms it.

### 3.1 Sizes

These are calculations. Phase F4 measures them.

- Peak full-size images: 3 (input, output, and the temporary image of a separable blur in FP2). After FP2 the chain
  uses 2 full-size images for exposure and amounts, with half-size buffers for FP4 at large sizes. At a small size the inhibitor blur needs a third full-size image. At 24 MP, one `float4` image is 384 MB.
- Blur sizes at a 6000 pixel long edge, for a 36 mm frame (6 um per pixel): spread sigma at most about 1.2 pixels at
  slider 1, inhibitor 5.8 pixels before the half-size reduction.
- Halation (sigma 20 pixels at this size) and Enlarger flare (sigma 10 % of the long edge) exceed
  `kGradeNeighborMaxTapCount` (64). They use the canonical canvas of 2048 pixels of `DiffusionCanvasExtent`.

### 3.2 Viewport

Halation and Enlarger flare read published canonical results, as `DiffusionScatterId` does, so a zoom or a viewport does not
change them. They are `film.halation.0` and `film.flare.0`. FP2 and FP4 run on the pixels of the render, with a size in pixels
from the frame and `RenderAxisScale`. So a light spread is not visible in a fit-to-window preview and is visible at full size,
as on film (lab measurement: mean difference 0.00 at about 2000 pixels and about 3.8 at 6000 pixels when spread is off). The audit of phase F4 finds the margin that an ROI edge needs.

---

## 4. Resources

The hidden controls are constants, so each combination of film, print material, and scene light has fixed scalar values. The lab
calculates them; the product does not need the logic of `_camera_filter`, `_printing_gain`, and `_prepare_inhibition`.

| Resource | Content | Size (calculated) | Made by |
| --- | --- | --- | --- |
| Stock bundle | Per negative: base, dyes, layer weights, dye amount curves, F and K of the inhibition, spread lengths. Per paper: base, dyes, printing, Status A, and viewing weights, curves. Scene light spectra. | Under 10 MB | Lab script |
| Print setup | Per film, print material, and light: printer balance, printing gain, layer gain, aim, display matrix | About 200 KB | Lab script |
| Reflectance table | `reflectance_over_uv_light`, 192^2 x 481 float32 | 71 MB | Lab script |
| u'v' table | 192^2, from the reflectance table, the layer weights, and the light | 0.6 MB, in memory | `film_tables` |
| Stage A | Negative to paper exposure (65^3) | 6.6 MB, in memory | `film_tables` |
| Stage B | Paper Status A to XYZ (65^3, axis power 2) | 6.6 MB, in memory | `film_tables` |
| Print table | Composed from A and B; RGBA32F for the GPU | 4.4 MB | `film_tables`, `AcquireLut` |

- No table except the shipped resources goes to disk (lab U68).
- All three sliders act on the image, so a slider change builds no table. A new film, print material, or scene light takes about 1 s on the host
  (lab P7, 20 threads).
- Each resource has a schema version and a SHA-256 in a manifest.
- The 71 MB table does not go into Git. Phase F1 fixes the delivery route with the release packaging.
- Before a release: license record of the training data of the reflectance table (ISSA and others, CC BY 4.0), spectral_film_lut (MIT), and the digitized
  Kodak values. This does not stop development.

---

## 5. Scope and source audit

Included: the method, its resources and host tables, CUDA, OpenCL, and Metal passes, the editor controls, and history.

Excluded, with the owner that keeps the work: Film Grain stays with its operator; the lab keeps the APD scan and the other lab controls in its viewer. HDR output and a second spectral prior are not part of the product decision.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Method and fields | `DrtParamsModel`, `include/edit/graph/drt_node_model.hpp` | `DrtMethod` has `Aces20` and `OpenDrt`; `ApplyUpdate(DrtParameterUpdate)`; `DrtDirty` has 4 groups | Add the method, the fields, and `DrtDirty::FilmPrint` |
| JSON | `edit/graph/drt_node_model.cpp` | `OutputTransformJson`, `LoadJson`; `MethodToString` maps two values | Add `film_print` and the object of section 2.2 |
| Output resolve | `edit/runtime/drt/drt_output_resolver.cpp` | `ToMethod` maps to `ColorUtils::ODTMethod` and fails on another value | Add a Rec. 709 SDR resolve for the film print |
| Compile | `CompileDrt`, `edit/runtime/graph_compiler.cpp` | Pushes `DiffusionFilter` and `Drt`, with a `DisplayTransform` step and Post neighborhood steps | Keep the passes. The film print is the display transform step of the `Drt` pass |
| Execute | `DrtPostExecutor`, `include/edit/runtime/drt_post_executor.hpp` | `Ops::BindDisplayParams`, `MakeDrtPostSchedule`, `Ops::DispatchDisplayTransform` | Add film print dispatch in each `Ops`; remove Halation from the schedule input |
| CUDA kernel | `DrtKernel`, `edit/runtime/cuda/cuda_drt_pass.cu` | One fused point kernel with the entry RGC | Add `cuda_film_print_pass.cu` |
| LUT storage | `CudaBackend::AcquireLut`, and the same on Metal and OpenCL | `float4` buffer with an LRU limit in bytes; `SampleLut3d` is trilinear in device code. No hardware texture | Use it for the print table. Device-code trilinear matches the lab |
| Small blur | `edit/runtime/cuda/cuda_neighbor_grade.cuh` | At most 64 taps (`kGradeNeighborMaxTapCount`). The current Halation kernel is an exponential, one kernel for all channels | Add a per-layer Gaussian pass. Do not change this file's operators |
| Large blur | `include/edit/runtime/diffusion_filter_plan.hpp` | Canonical canvas at most 2048 pixels, `DecideDiffusionScatter`, a published result | Use the same pattern for two new results |
| Halation operator | `edit/runtime/adjustment_runtime.cpp` | Fixed sigma 7 and fixed colour `{1.0, 0.05, 0.02}`. `RequireCompleteDrtPostTypes` requires the type | Keep the type. Skip it at schedule time with the film print |
| Invalidation | `edit/runtime/runtime_invalidation.cpp` | `DrtDirty::Diffusion` marks the scatter stale | Add origins of the two results |
| Content key | `result_content_key.cpp`, `content_key.hpp` | Hashes the DRT params JSON; `kDrtImplementationVersion` is 6 | Raise the version only when the existing methods change their result; add table keys |
| Editor | `app/editor_parameter_write*.cpp`, `editor_panel_projection.cpp`, `editor_adjustment_context.cpp`, `editor_parameter_catalog.cpp` | Field `odt` writes the whole display transform; `diffusion` is one scalar | Add the fields to `odt` |
| UI | `ui/alcedo_main/qml/EditorDisplayTransformPanel.qml`, `EditorPostProcessPanel.qml` | Panel-level `paramsBuilder`; a Halation slider | Section 2.1 |
| Tests | `alcedo_studio/tests/edit/CMakeLists.txt` | `GpuDagCudaDrtProductTest` holds the CUDA DRT cases, label `gpu` | Add cases and new host targets |

---

## 6. Phases

Limits for the numeric phases: the 8 bit difference of final sRGB against the lab evaluator, per pixel the largest channel, on one AP1 array, because the RAW paths differ. Estimates are planning values.

| Phase | Result | Estimated diff | Split reason |
| --- | --- | --- | --- |
| F1 | Resources and host tables, no GPU | 1500 lines | Host work has its own tests |
| F2 | Model, JSON, compile, and invalidation | 900 lines | No GPU kernel |
| F3 | CUDA point kernels, on a ramp with no image plane | 1000 lines | FP3, FP2, and blur add risk; F3 proves the math first |
| F4 | CUDA image-plane steps, canonical results, and timing | 1500 lines | Together with F3 it can pass 2000 lines |
| F5 | Editor controls and user review | 700 lines | UI only |
| F6 | OpenCL passes | 1500 lines | One backend per phase |
| F7 | Metal passes | 1500 lines | One backend per phase |

Commands for all phases, from the repository root:

```text
cmd /c scripts\msvc_env.cmd --preset win_release
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4
ctest --test-dir build/release -R <test name> --output-on-failure
```

Performance values come from a Release build only.

### F1: resources and host tables

1. **Objective and deliverables.** A table for a selection, with no document and no GPU. The lab adds `scripts/poc/p7_spectral_acceleration/build_product_resources.py`, which makes the resources and the expected values.
2. **Inputs.** Lab resources; `film_tables` at the commit that the plan record names.
3. **Modules (proposed).** `include/edit/runtime/film/film_tables.hpp`, `edit/runtime/film/film_tables.cpp` (copy of the lab library, which becomes the source); `film_stock_bundle.hpp`, `.cpp`: `FilmStockBundle::Open(path)`; `film_print_tables.hpp`, `.cpp`: `FilmPrintTables::Resolve(negative, print_material, scene_light)`, returning immutable `PackedGradeLut`-style data with a `ContentKey`.
4. **Data rules.** Resources are immutable after `Open`. A table result is shared and immutable. Stage A and stage B stay in memory by content key, with a limit in bytes. No table goes to disk.
5. **Steps.** (a) Write the lab script. (b) Add the reader with schema and SHA-256 checks. (c) Copy `film_tables`. (d) Add the u'v' integral. (e) Add `Resolve`. (f) Fix the delivery route of the reflectance table.
6. **Success chain.** `Resolve` -> `FilmStockBundle` read -> u'v' table -> stage A -> stage B -> `ComposePrintTable` -> immutable result.
7. **Failure chain.** A missing file or a wrong digest -> `Open` throws with the path -> no partial result stays in memory.
8. **Tests (proposed).** `FilmPrintTablesTest.PrintTableMatchesLab` and `.UvTableMatchesLab`: largest relative difference 1e-9 against lab values for 3 selections. `.StageTablesAreReused`: a second selection with one paper builds no new stage B. `.DamagedResourceThrows`: a changed byte gives an error that names the file.
9. **Exit criteria.** All tests pass. The record states stage times on the host.
10. **Expected diff.** 1500 lines.
11. **Completion record.** Pending.

### F2: model, JSON, compile, and invalidation

1. **Objective.** The document holds the method. The plan compiles. The render stops with a named "not implemented on this backend" error until F3.
2. **Inputs.** Section 2.2. No F1 dependency in the tests of the model.
3. **Modules.** Rows Method and fields, JSON, Output resolve, Compile, Execute, Halation operator, Invalidation, and Content key of section 5.
4. **Data rules.** Existing methods keep their JSON bytes and their expected output. The Halation value stays in the document. A slider change marks `DrtDirty::FilmPrint`; it builds no table.
5. **Steps.** (a) Add the enum, fields, and update. (b) Add JSON and validation. (c) Add the output check. (d) Add the schedule skip. (e) Add the invalidation origins and the keys.
6. **Success chain.** `DrtParamsModel::ApplyUpdate` -> revision -> `GraphCompiler::CompileDrt` -> `PlanExecutor` -> `PassEncoder<Backend, GpuPassKind::Drt>` -> `DrtPostExecutor::Execute`.
7. **Failure chain.** An invalid update -> `ApplyUpdate` throws and changes no field. A film print with HDR -> `DrtOutputResolver::ResolveNode` throws -> `PlanExecutor` catch -> `device.CancelRender()`.
8. **Tests.** `drt_diffusion_filter_model_test` in its form, new `DrtFilmPrintModelTest`: round trip, omission for other methods, rejection of out-of-range values. `graph_compiler_test`: the plan keeps its passes. `drt_output_resolver_test`: HDR is an error. `drt_post_executor_test`: the film print schedule has no Halation, and the other methods keep it.
9. **Exit criteria.** All existing expected values pass with no change.
10. **Expected diff.** 900 lines.
11. **Completion record.** Pending.

### F3: CUDA point kernels

1. **Objective.** FP1, FP4 and FP5 with no blur, and FP6 with no flare, on CUDA.
2. **Inputs.** F1, F2.
3. **Modules (proposed).** `include/edit/runtime/film/film_print_math.h`, shared by the host, CUDA, OpenCL C, and Metal in the form of `aces_reference_gamut_compression.h`; `edit/runtime/cuda/cuda_film_print_pass.cu`; `alcedo_studio/tests/edit/runtime/film_print_reference.hpp`, a double-precision host form on `film_print_math.h`.
4. **Data rules.** Arithmetic is float32. The print table uses `AcquireLut`; the u'v' table and curves use content-keyed device buffers. All sizes of 3.1 hold.
5. **Steps.** (a) Write the math header. (b) Write the reference. (c) Write the kernels. (d) Add dispatch to `CudaDrtOps`.
6. **Success chain.** `CudaDrtOps::BindDisplayParams` -> `FilmPrintTables::Resolve` -> `AcquireLut` -> `DispatchDisplayTransform` -> film print kernels -> display image.
7. **Failure chain.** A resolve error or a kernel launch error -> throw -> `CheckAfterEncode` or the `PlanExecutor` catch. The display result stays invalid.
8. **Tests.** New cases in `GpuDagCudaDrtProductTest`: `FilmPrintRampMatchesLab`, neutral ramp largest at most 0.5; `FilmPrintChartMatchesReference`, largest at most 1.0; `FilmPrintOutputIsFinite`, an AP1 image with negative values and values above the curve end.
9. **Exit criteria.** All pass on the development GPU.
10. **Expected diff.** 1000 lines.
11. **Completion record.** Pending.

### F4: CUDA image-plane steps and timing

1. **Objective.** The complete chain on CUDA, with its cost record.
2. **Inputs.** F3.
3. **Modules.** `cuda_film_print_pass.cu`; a per-layer Gaussian pass; a plan header in the form of `diffusion_filter_plan.hpp` for `film.halation.0` and `film.flare.0`; `runtime_invalidation.cpp`.
4. **Data rules.** Sizes in pixels come from a 36 mm frame and `RenderAxisScale`. Boundary rule: reflect, as in the lab. Enlarger flare and Halation at an ROI read the published results.
5. **Steps.** (a) Audit how a viewport ROI margin works for neighborhood steps, and record the source fact. (b) Add FP2. (c) Add the half-size FP4. (d) Add the canonical results. (e) Measure.
6. **Success chain.** Full-frame render builds and publishes canvases -> ROI render samples them -> display image.
7. **Failure chain.** An allocation failure -> real error -> no published partial result.
8. **Tests.** `FilmPrintImageMatchesLab`, on one AP1 array at full size for 3 stocks: p99.9 at most 1.0, largest at most 4.0. `FilmPrintViewportKeepsCanvas`: a zoom does not change a pixel of Halation and flare beyond the limit of the Diffusion test. `FilmPrintSliderBuildsNoTable`: a slider change has no table build.
9. **Evidence.** Release build, RTX 3080 Laptop, 24 MP: time per pass, first and warm renders, GPU memory peak. Proposed performance target: at most 150 ms for the film print passes. If the target is not met, report the pass times and stop for a user decision.
10. **Expected diff.** 1500 lines.
11. **Completion record.** Pending.

### F5: editor controls and user review

1. **Objective.** Section 2.1, on CUDA.
2. **Inputs.** F4.
3. **Modules.** Rows Editor and UI of section 5; `ui/alcedo_main/i18n/*.ts`. Use the skills `alcedo-qml-ui` and the design rules.
4. **Data rules.** The fields go through `odt`. History rows use `OutputTransformJson`. Film and print lists come from the stock bundle, with the catalog names.
5. **Steps.** (a) Extend the catalog and `paramsBuilder`. (b) Add parse and write. (c) Add the panel states. (d) Add translations.
6. **Success chain.** QML control -> `parameterCatalog.modelParamsJson("odt", ...)` -> `editor_parameter_write` -> `DrtParamsModel::ApplyUpdate` -> render.
7. **Failure chain.** A render error -> the existing editor error path. The controls keep their values.
8. **Tests.** Parameter write tests: each field reaches the model, and undo restores it. Projection test: the panel reads the values. QML test: the Halation slider hides and returns.
9. **User review.** RAW files by day and by night, against the lab viewer. The review confirms that the entry has no reference gamut compression. Record it as user-confirmed manual verification.
10. **Expected diff.** 700 lines.
11. **Completion record.** Pending.

### F6: OpenCL passes

1. **Objective.** The chain of F3 and F4 on OpenCL 1.2, with its program registration. Use the skill `opencl-program-registry`.
2. **Inputs.** F4.
3. **Modules.** `edit/runtime/opencl/`, `shader/`, `include/edit/runtime/opencl/opencl_pass_encoder.hpp`.
4. **Data rules.** `film_print_math.h` is the same source. Scene images keep the dual-storage binding.
5. **Steps.** (a) Register the kernels. (b) Add dispatch to the OpenCL `Ops`. (c) Add the canonical results.
6. **Success chain.** The chain of F3 and F4, through the OpenCL backend.
7. **Failure chain.** A build or enqueue error stops with the real error. There is no change of backend.
8. **Tests.** The cases of F3 and F4 in `GpuDagOpenClGradeTest`, against the same host reference. An unavailable device is unverified support, not a pass.
9. **Exit criteria.** Pass on the development GPU, with the same limits.
10. **Expected diff.** 1500 lines.
11. **Completion record.** Pending.

### F7: Metal passes

1. **Objective.** The chain of F3 and F4 on Metal.
2. **Inputs.** F4; a macOS machine.
3. **Modules.** `edit/runtime/metal/`, `shader/`, the DRT metallib, `include/edit/runtime/metal/metal_pass_encoder.hpp`.
4. **Data rules.** `film_print_math.h` is the same source. Every dispatch goes into the serial compute encoder of the frame, and pipeline states come from the Metal pipeline cache.
5. **Steps.** (a) Add the shader. (b) Add dispatch to the Metal `Ops`. (c) Add the canonical results.
6. **Success chain.** The chain of F3 and F4, through the Metal backend.
7. **Failure chain.** A pipeline or an encoder error stops with the real error.
8. **Tests.** The cases of F3 and F4 in `GpuDagMetalDrtTest`, against the same host reference. A build with no device is unverified support, not a pass.
9. **Exit criteria.** Pass on a macOS machine, with the same limits.
10. **Expected diff.** 1500 lines.
11. **Completion record.** Pending.

---

## 7. Risks

| Risk | Fact | Response |
| --- | --- | --- |
| Input differs from the lab | The lab decodes with LibRaw; the product has its own camera colour path and RGC. | Numeric tests use one AP1 array. The appearance is decided in F5. |
| Strong stocks | Lab P7: 24 of 30 negatives stay inside the limits (mean 0.5, p99.9 2.0, largest 5.0) against 4 full-size steps. `kodak_5277`, Eterna 500 and Vivid, `kodak_5203`, Aerocolor, and Ektar 100 do not; the worst is 3.11 mean and 14.3 largest. | The user reviewed this form in the lab viewer and accepts it (lab U72). The reference of the tests here is the lab with the same rules. |
| Blur at a large size | A long edge above 6000 pixels raises the FP2 and FP4 sizes. | F4 measures it. |
| Memory | 3 full-size images on top of the current frame, on a GPU of 8 GB. | F4 measures it. A failure reports the real error. |
| Table cache | `AcquireLut` has an LRU limit in bytes, and the print table is 4.4 MB. | F1 records the limit. |
