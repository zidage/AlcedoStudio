# Film Print DRT Plan

Date: 2026-11-01

Status: **In progress.** F1 and F2 are complete (2026-11-01). F3 to F7 have not started.

Parent: [Roadmap](../../roadmap.md), section "Still Planned".

Tracking: [#631](https://github.com/zidage/AlcedoStudio/issues/631). Section 8 lists the phase issues.

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
| Reflectance table | Compact form in the installer, with no download (2026-11-01): only the used wavelengths, only the u'v' nodes that a read can reach, and 12 mantissa bits. The u'v' limit against the complete lab table is a relative difference of 2e-4, and the final difference is at most 0.5 of an 8 bit code. |
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
from the frame and `RenderAxisScale`. So the visible effect of a light spread is smaller in a fit-to-window preview than at full size, as on film. This is an inference from the sizes of section 3.1; phase F4 measures it. The audit of phase F4 finds the margin that an ROI edge needs.

---

## 4. Resources

The hidden controls are constants, so each combination of film, print material, and scene light has fixed scalar values. The lab
calculates them; the product does not need the logic of `_camera_filter`, `_printing_gain`, and `_prepare_inhibition`.

| Resource | Content | Size (calculated) | Made by |
| --- | --- | --- | --- |
| Stock bundle | Per negative: base, dyes, layer weights, dye amount curves, F and K of the inhibition, spread lengths. Per paper: base, dyes, printing, Status A, and viewing weights, curves. Scene light spectra. | Under 10 MB | Lab script |
| Print setup | Per film, print material, and light: printer balance, printing gain, layer gain, aim, display matrix | About 200 KB | Lab script |
| Reflectance table | Compact form of `reflectance_over_uv_light` (192^2 x 481 float32, 71 MB): 18103 nodes, 346 wavelengths, 12 mantissa bits, one byte plane per float32 byte | 25 MB, 5.5 MB after xz | Lab script |
| u'v' table | 192^2, from the reflectance table, the layer weights, and the light. A node that no read can reach holds 0 | 0.6 MB, in memory | `film_tables` |
| Stage A | Negative to paper exposure (65^3) | 6.6 MB, in memory | `film_tables` |
| Stage B | Paper Status A to XYZ (65^3, axis power 2) | 6.6 MB, in memory | `film_tables` |
| Print table | Composed from A and B; RGBA32F for the GPU | 4.4 MB | `film_tables`, `AcquireLut` |

- No table except the shipped resources goes to disk (lab U68).
- All three sliders act on the image, so a slider change builds no table. A new film, print material, or scene light takes about 1 s on the host
  (lab P7, 20 threads).
- Each resource has a schema version and a SHA-256 in a manifest.
- The resources (28 MB) do not go into Git. Delivery route, from phase F1: the release installer ships the resources. `build_product_resources.py` of the lab makes the directory. `package_windows.ps1 -FilmPrintResourceDir DIR` and `package_macos.sh --film-print-resource-dir DIR` set `ALCEDO_FILM_PRINT_RESOURCE_DIR`, the install rule copies the unchanged files to `config/film_print/`, and the install tree check compares each file with the SHA-256 of the manifest. A release machine gets the directory from the lab script or from a copy of its output. There is no download at run time.
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

### 5.1 Target ownership and data flow

| Data | Owner | Reader |
| --- | --- | --- |
| Selections and slider values | `DrtParamsModel` | Compile step, editor projection |
| Stock bundle and print setup | `FilmStockBundle` (proposed), read-only after `Open` | `FilmPrintTables` |
| Stage tables and print table | `FilmPrintTables` (proposed), in memory by content key | Backend `Ops` |
| GPU copy of the print table | Backend LUT cache, `AcquireLut` | Film print kernels |
| Canonical Halation and flare results | Backend published results, in the form of `DiffusionScatterId` | FP3 and FP6 at a viewport |

Data flow: AP1 from the DiffusionFilter pass -> FP1 to FP6 in the `Drt` pass -> display image. A slider change
reaches only the kernels and the published results. A new selection also reaches `FilmPrintTables`.

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
8. **Tests (proposed).** `FilmPrintTablesTest.PrintTableMatchesLab` and `.UvTableMatchesLab`: for 3 selections, the print table has a largest relative difference of 1e-9 against the lab. The u'v' table has 1e-9 against the same integral of the lab on the compact table, and 2e-4 against the complete lab table on the nodes that a read can reach. `.StageTablesAreReused`: a second selection with one paper builds no new stage B. `.DamagedResourceThrows`: a changed byte gives an error that names the file.
9. **Exit criteria.** All tests pass. The record states stage times on the host.
10. **Expected diff.** 1500 lines.
11. **Completion record.** See below.

##### Phase F1 completion record (2026-11-01)

**Status:** complete. Host tables and resources, with no document and no GPU. No product code calls `FilmPrintTables` until phase F3.

**Primary success call chain:**

```text
FilmStockBundle::Open(directory)
  -> ReadManifest -> ReadResource (size, SHA-256) -> FilmArrayFile::Parse (schema version, array bounds)
  -> FilmStockBundle constructor (lists, shape of each array)
FilmPrintTables::Resolve(negative, print_material, scene_light)
  -> FilmStockBundle::Selection
  -> film_tables::TableBandIntegral (kept u'v' nodes, then the 192^2 table)
  -> NegativeStageOf -> film_tables::Log10BandResponse (stage A, kept by negative and paper)
  -> PaperStageOf -> film_tables::BuildPaperTable (stage B, kept by paper)
  -> film_tables::ComposePrintTable -> PackPrintRgba
  -> immutable FilmPrintTableData, kept by ContentKey
```

**Primary failure call chain:**

```text
Missing file, wrong size, wrong SHA-256, other schema version, or wrong array shape
  -> std::runtime_error with the file name, from Open
  -> no bundle object exists
Unknown identifier -> std::runtime_error with the identifier, from Resolve -> no table is built
Non-finite table value -> std::runtime_error with the selection, from Resolve -> no result is kept
```

**Decisions that the plan did not specify:**

- Lab commits: `film_tables` is a copy from `8b16c67` of alcedo-film-lab, the last commit that changed the library. `build_product_resources.py` is from `2c6c14c`. The manifest of the resources names the lab commit of its run. The product adds `TableBandIntegral`, the u'v' integral, on the same worker threads. The product copy is now the source of the library, as this phase states.
- Each resource uses one array file (`film_array_file.hpp`): a JSON header, then raw arrays. The stock names are in the header of the stock bundle.
- User decision of 2026-11-01: the installer ships a compact reflectance table, in place of the complete table of 71 MB. Each byte of a float32 is in its own plane, so that the compression of the installer finds the repeated high bytes. `Open` restores the rows and keeps them, about 25 MB. `Open` stops with an error if a negative has a weight outside the wavelength band.
- The bundle holds shared values once: per negative, per paper, and per pair. The script stops if a value differs between two selections that use it. The print setup is 167 KB and the stock bundle is 2.7 MB.
- The bundle also holds the values of phase F3 (curves, inhibition, locus projection, gamut limits, and fixed strengths), so F3 needs no new schema version. F1 reads only the values of its tables.
- One byte limit covers stage A, stage B, and complete results: 128 MiB, about 5 selections. `Resolve` keeps complete results because the render path of phase F3 calls it on each render. Stage A takes 6.6 MB, stage B takes 6.9 MB, and a complete result takes 11.9 MB.
- One lock serializes `Resolve`, so each table builds one time. A new selection takes about 0.55 s.
- `print_rgba` has red varying fastest, as `LutIndex` reads it. The lab table has red varying slowest.
- Stage A depends on the negative and the paper, because the printing weights belong to the paper. A new scene light changes only the u'v' table and the composition.
- The expected values are local output of the lab script and are not in Git. Without `ALCEDO_FILM_PRINT_RESOURCE_DIR` and `ALCEDO_FILM_PRINT_EXPECTED_DIR`, one test skips with a message in place of the comparison. This is not a pass. The other tests use small resources that the test writes.

**Source facts for later phases:**

- `AcquireLut` limit: 64 MiB on CUDA, OpenCL, and Metal (`lut_byte_budget_`). No production code calls `SetLutByteBudget`. The limit holds about 14 print tables of 4.4 MB.
- `SampleLut3d` reads a value `u` at `u * edge - 0.5`, and the lab reads a density at `(d - start) / step` on nodes. Phase F3 must convert with `u = (node + 0.5) / edge`.

**Host stage times (Release, Intel Core i7-12700H, 20 threads, 3 runs, a new selection):**

| Stage | Time |
| --- | --- |
| `FilmStockBundle::Open`, with all SHA-256 checks | 0.13 to 0.14 s |
| u'v' table | 0.003 to 0.004 s |
| Stage A | 0.073 to 0.081 s |
| Stage B | 0.48 to 0.50 s |
| Composition | 0.013 to 0.014 s |

**Resource sizes (measured):** 28.0 MB in the install tree. These files alone add 6.63 MB to an NSIS file with `SetCompressor lzma`, the CPack default. A ZIP at deflate level 6 takes 9.31 MB, as an estimate for the macOS update ZIP. With the complete table, a 7-Zip archive took 39.8 MB.

**Compact table checks (lab script, limits written before the first run):** on 11 selections, the largest relative u'v' difference against the complete table is 1.2e-4 (limit 2e-4). The removed nodes held NaN, and the layer exposure stayed finite on 3 photos, a ramp, 512^2 random colours, and the edge of the hull. On 6 selections, the final sRGB differs by at most 0.0054 of an 8 bit code (limit 0.5).

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `FilmPrintTablesLabTest.PrintTableMatchesLab`, 3 selections, limit 1e-9 | `FilmPrintTablesTest` | PASS, largest 2.0e-15 |
| `FilmPrintTablesLabTest.UvTableMatchesLab`, 3 selections, limit 1e-9 against the compact lab table | `FilmPrintTablesTest` | PASS, largest 2.0e-15 |
| The same test, limit 2e-4 against the complete lab table, on the nodes that a read can reach | `FilmPrintTablesTest` | PASS, largest 1.1e-4 |
| `FilmPrintTablesTest.StageTablesAreReused` | `FilmPrintTablesTest` | PASS |
| `FilmPrintTablesTest.DamagedResourceThrows`, each of 3 files | `FilmPrintTablesTest` | PASS |

The plan names these four tests in one suite. The two lab comparisons are in the suite `FilmPrintTablesLabTest`, because they need the lab files.

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target FilmPrintTablesTest`, then the executable in `FilmPrintTablesTest_runtime/`, and `ctest --test-dir build/debug -R "^FilmPrintTablesTest\." -j 1`. For the times, the same target in `win_release` with `-DALCEDO_BUILD_TESTS=ON`. The release tree then went back to its preset values.

Suite totals: 27/27 on Debug and on Release. The lab script: 11 selections against the complete `FilmPreview`, largest difference 1.8e-15, limit 1e-9.

**Checklist / exit condition:** all tests pass, and this record states the stage times and the `AcquireLut` limit.

**LOC note:** all new files are under 1000 lines.

**Not verified:**

- No complete package ran, so the sizes above are for the resources alone. The generated `cmake_install.cmake` of the Windows release tree holds the copy to `bin/config/film_print`. The SHA-256 logic of the Windows and macOS install tree checks ran alone on the resource directory.
- No build ran on macOS.
- The product has no GPU render of the film print before phase F3, so the display check of the compact table ran on the lab kernels only.
- The license record of section 4 stays a requirement before a release.

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
11. **Completion record.** See below.

##### Phase F2 completion record (2026-11-01)

**Status:** complete. The document holds the method and the plan compiles. A render of a film print stops with a named error on CUDA, OpenCL, and Metal until the kernels of that backend exist.

**Primary success call chain:**

```text
DrtParamsModel::ApplyUpdate -> RequireValidFilmPrintUpdate -> MutateWithDirtyFields (DrtDirty::FilmPrint)
  -> RuntimeInvalidationState::CollectDrtChanges (film.halation.0, film.flare.0, display)
  -> GraphCompiler::CompileDrt (same passes and steps as the other methods)
  -> BuildFrameResultContentKeys (film_uv_table, film_print_table, drt_display)
  -> PlanExecutor -> PassEncoder<Backend, GpuPassKind::Drt> -> DrtPostExecutor::Execute
```

**Primary failure call chains:**

```text
Invalid update -> ApplyUpdate throws std::invalid_argument with the field -> no field changes
Invalid document -> LoadJson throws std::runtime_error with the key -> no field changes
Film print with HDR, another encoding space, or a linear encoding
  -> DrtOutputResolver::ResolveNode throws -> PlanExecutor catch -> device.CancelRender()
Film print with a supported encoding, until F3
  -> DrtPostExecutor::Execute throws "<prefix>: the film print is not implemented on this backend"
```

**Decisions that the plan did not specify:**

- The identifier lists are constants in `film_print_model.hpp`, because a Model reads no resource file and the model tests do not need F1. `FilmPrintTablesLabTest.ShippedListsMatchTheIdentifiersOfTheDocumentModel` compares them with the shipped bundle. A change of the stock list must change these constants.
- `OutputTransformJson` holds `film_print`, so `ToJson` and the `odt` history value both have it. Only the method `film_print` writes it. The fields stay in memory while another method is selected, but a saved document of another method does not hold them.
- A document with no `film_print` object, or with a missing key, reads the defaults. `LoadJson` now reads `diffusion` and `film_print` before it changes a field, so a rejected document changes nothing.
- An export to a space that other methods encode as Rec. 709 (for example ProPhoto) stops for a film print. The check reads the requested space, not the result of `ExportSpaceToDrt`. All SDR curves of the catalog pass: gamma 1.8, 2.2, 2.6, BT.1886, and sRGB.
- `MethodFromString` still reads an unknown method as OpenDRT, as it did before this phase. The rejection of unknown identifiers applies to the fields of `film_print`.
- `Ops::kImplementsFilmPrint` selects the stop in `DrtPostExecutor::Execute`. Phase F3 sets it to true for CUDA, F6 for OpenCL, and F7 for Metal. The executor resolves the output first, so that an unsupported encoding reports its own error.
- `DrtPostNeighborhoodRuns` is the one rule for the schedule and for the works of `Execute`. The parameter slot of Halation stays, as `CollectParameterSlotKeys` lists all compiled adjustments.
- `kDrtImplementationVersion` stays at 6. New `kFilmPrintImplementationVersion` covers film print pixel rules, and only a film print mixes it. Phases F3 and later raise this value, not the DRT version.
- The frame keys `film_uv_table` and `film_print_table` follow the selections only and are empty for other methods. They do not hold the digest of the bundle; `FilmPrintTableData` holds its own keys with the digest.
- The canonical results have edges from the scene input and the scatter image, because the film print reads the scene after the diffusion filter, and they use the canonical identity of the scatter image.
- `ColorUtils::ODTMethod` has `FILM_PRINT`. `CudaDrtMethod` and the Pack functions do not change, because no film print reaches them before F3.

**Source facts for phase F5:**

- `ParseDrtUpdate` still rejects the method `film_print` and the key `film_print`, so history replay and adjustment transfer of a film print fail with the real error until F5 adds them. The editor cannot select the method before F5.
- A raster document can have a P3 D65 encoding, and the resolver stops that with a film print. The panel of F5 hides the encoding controls, so selecting the film print must also write Rec. 709 and an SDR curve.

**What was proven (executed tests, Debug):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `DrtFilmPrintModelTest`: round trip, omission for other methods, rejection of out-of-range values | `GpuDagModelGraphTest` | PASS |
| `GpuDagGraphCompiler.FilmPrintKeepsThePassesAndStepsOfOtherMethods` | `GpuDagRawInputTest` | PASS |
| `DrtOutputResolver.FilmPrintRejectsHdrAndLinearEncodings`, and the cases of encoding space and export | `GpuDagModelGraphTest` | PASS |
| `DrtPostSchedule.FilmPrintScheduleHasNoHalationAndOtherMethodsKeepIt` | `GpuDagRawInputTest` | PASS |
| `RuntimeInvalidation.FilmPrintSliderInvalidatesItsResultsAndDisplayAndKeepsTheScene` | `GpuDagRawInputTest` | PASS |
| `GpuDagResultContentKey.FilmPrintSliderChangesDisplayKeyAndKeepsTableKeys` | `GpuDagRawInputTest` | PASS |
| `FilmPrintRenderStopsWithNamedErrorUntilItsKernelsExist`, CUDA and OpenCL | `GpuDagCudaDrtProductTest`, `GpuDagOpenClDrtProductTest` | PASS |
| All existing expected values, with no change | The same targets | PASS |

**Checklist / exit condition:** all existing expected values pass with no change.

Caller suites: `EditorPanelProjectionTest`, `AdjustmentTransferCatalogTest`, `DocumentTransferTest`, `EditorPipelineCommandServiceTest`, and `ProjectCompatibilityTest` ran 81 tests. 80 pass. `DocumentTransferTest.TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches` fails on the `version` key of the Color Wheel. Issues #484 and #554 report this failure, and this phase adds no such key.

**Not verified:** no build ran on macOS, so the one-constant change of `metal_drt_pass.mm` did not compile and the Metal stop did not run. The full test suite did not run.

### F3: CUDA point kernels

1. **Objective.** FP1, FP4 and FP5 with no blur, and FP6 with no flare, on CUDA.
2. **Inputs.** F1, F2.
3. **Modules (proposed).** `include/edit/runtime/film/film_print_math.h`, shared by the host, CUDA, OpenCL C, and Metal in the form of `aces_reference_gamut_compression.h`; `edit/runtime/cuda/cuda_film_print_pass.cu`; `alcedo_studio/tests/edit/runtime/film_print_reference.hpp`, a double-precision host form on `film_print_math.h`.
4. **Data rules.** Arithmetic is float32. The print table uses `AcquireLut`; the u'v' table and curves use content-keyed device buffers. All sizes of 3.1 hold. The first caller opens the bundle from `config/film_print/` next to the executable; F1 gives `Open` only a path. A read must follow rule `spectral_locus_projection`, because a node that it cannot reach holds 0.
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
10. **Exit criteria.** All tests pass. The record states the evidence of item 9 and the source facts of the ROI audit.
11. **Expected diff.** 1500 lines.
12. **Completion record.** Pending.

### F5: editor controls and user review

1. **Objective.** Section 2.1, on CUDA.
2. **Inputs.** F4.
3. **Modules.** Rows Editor and UI of section 5; `ui/alcedo_main/i18n/*.ts`. Use the skills `alcedo-qml-ui` and the design rules.
4. **Data rules.** The fields go through `odt`. History rows use `OutputTransformJson`. Film and print lists come from the stock bundle, with the catalog names. This phase makes the film print selectable, so it also makes `-FilmPrintResourceDir` and `--film-print-resource-dir` required in the package scripts.
5. **Steps.** (a) Extend the catalog and `paramsBuilder`. (b) Add parse and write. (c) Add the panel states. (d) Add translations.
6. **Success chain.** QML control -> `parameterCatalog.modelParamsJson("odt", ...)` -> `editor_parameter_write` -> `DrtParamsModel::ApplyUpdate` -> render.
7. **Failure chain.** A render error -> the existing editor error path. The controls keep their values.
8. **Tests.** Parameter write tests: each field reaches the model, and undo restores it. Projection test: the panel reads the values. QML test: the Halation slider hides and returns.
9. **User review.** RAW files by day and by night, against the lab viewer. The review confirms that the entry has no reference gamut compression. Record it as user-confirmed manual verification.
10. **Exit criteria.** All tests pass and the user accepts the review of item 9.
11. **Expected diff.** 700 lines.
12. **Completion record.** Pending.

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

---

## 8. Tracking

- Tracking: [#631](https://github.com/zidage/AlcedoStudio/issues/631)
- F1: [#624](https://github.com/zidage/AlcedoStudio/issues/624)
- F2: [#625](https://github.com/zidage/AlcedoStudio/issues/625)
- F3: [#626](https://github.com/zidage/AlcedoStudio/issues/626)
- F4: [#627](https://github.com/zidage/AlcedoStudio/issues/627)
- F5: [#628](https://github.com/zidage/AlcedoStudio/issues/628)
- F6: [#629](https://github.com/zidage/AlcedoStudio/issues/629)
- F7: [#630](https://github.com/zidage/AlcedoStudio/issues/630)
