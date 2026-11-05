# Film Print DRT Plan

Date: 2026-11-01

Status: **In progress.** F1, F2, F3, and F4 are complete (2026-11-01). The controls of F5 are implemented, and the user review of F5 is pending. F6 and F7 are complete (2026-11-02). F8 is complete on CUDA and OpenCL (2026-11-03). F9, the reversal films on Metal, is complete (2026-11-03). F10, print material Negative Scan (a virtual scanner and a negative rendering model, shown by OpenDRT) and the scene light As Shot, has two phases: the model layer is complete (2026-11-05), and phase 2 adds the CUDA and OpenCL passes (issue #664). F11, Negative Scan on Metal (issue #662), is complete (2026-11-05).

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
| Default scene light | `as_shot` (user decision of 2026-11-05, issue #672). It was `reference` before. |
| Default DRT of a new document | OpenDRT stays. A user selects the film print. |
| Stock data | 13 stocks have rule `grey_anchor` from the repair of lab U71. The print setup resource holds the result. |
| Reflectance table | Compact form in the installer, with no download (2026-11-01): only the used wavelengths, only the u'v' nodes that a read can reach, and 12 mantissa bits. The u'v' limit against the complete lab table is a relative difference of 2e-4, and the final difference is at most 0.5 of an 8 bit code. |
| Look | The user reviewed the lab viewer with the speed rules and the stock repair, and accepts the look (lab U72). Lab U78 replaces the print conditions: see the [record of issue #645](#fix-of-issue-645-record-2026-11-02). |

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
  Print material  [Kodak Endura Premier    v]
  Scene light     [As Shot         v]
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
| Print material | `print_material` | `kodak_endura_premier` (user decision of 2026-11-01, lab U76) | Identifier of the stock bundle |
| Scene light | `scene_light` | `as_shot` (issue #672) | `reference`, `as_shot`, 5 lights, 5 scenes of the lab |
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
| FP4 | First amounts from the curves; rule `inhibition_image_part` (see the [record of issue #642](#fix-of-issue-642-record-2026-11-01)); a Gaussian of the amounts, on a half-size image when the sigma is at least 4 pixels | Point and blur | `negative_density`, `gaussian_blur_half_size` |
| FP5 | Rule `inhibition_two_steps`: a = F(log H' - K_image * inhibitor), and FP4 and FP5 once more from these amounts. log H' is log H minus the point part of FP4 | Point and blur | `fast_kernels.inhibited_step` |
| FP6 | Transmittance, Enlarger flare from the canonical canvas, density, print table (65^3), cube, display matrix, gamut compression, display encoding | Fused point | `flare_density`, `print_display`, `finish_display` |

This chain has 6 blur passes. Python step P6 had 12. The point stages between blurs cannot fuse, because each one
needs a blur result. FP6 fuses all stages after the Enlarger flare.

The entry of this method has the reference gamut compression, as `DrtKernel` has for the other methods (lab U80, rule `entry_reference_gamut_compression`). The final colour space is not known at this point. This replaces the earlier inference that rule `spectral_locus_projection` is sufficient.

### 3.1 Sizes

These are calculations. The record of phase F4 holds the measured memory and times.

- Peak full-size images: 3 (input, output, and the temporary image of a separable blur in FP2). After FP2 the chain
  uses 2 full-size images for exposure and amounts, with half-size buffers for FP4 at large sizes. At a small size the inhibitor blur uses the temporary image of FP2. At 24 MP, one `float4` image is 384 MB.
- Blur sizes at a 6000 pixel long edge, for a 36 mm frame (6 um per pixel): spread sigma at most about 1.2 pixels at
  slider 1, inhibitor 5.8 pixels before the half-size reduction.
- Halation (sigma 20 pixels at this size) and Enlarger flare (sigma 10 % of the long edge) exceed
  `kGradeNeighborMaxTapCount` (64). They use the canonical canvas of 2048 pixels of `DiffusionCanvasExtent`.

### 3.2 Viewport

Halation and Enlarger flare read published canonical results, as `DiffusionScatterId` does, so a zoom or a viewport does not
change them. They are `film.halation.0` and `film.flare.0`. FP2 and FP4 run on the pixels of the render, with a size in pixels
from the frame and `RenderAxisScale`. So the visible effect of a light spread is smaller in a fit-to-window preview than at full size, as on film. This is an inference from the sizes of section 3.1. The record of phase F4 holds the audit: an ROI edge needs a margin of 49 pixels at slider 1, the source has no ROI margin, and FP2 and FP4 use the boundary rule reflect at the ROI edge.

### 3.3 Reversal form (phase F8)

A colour reversal film (slide film) is the final image, so its chain has no print material (lab decision U86, [P9 report](https://github.com/zidage/alcedo-film-lab/blob/feature/p7-product-resources/docs/exploration/p9_reversal_light_table.md), sections 4.2 and 6). FP1 to FP5 are the same kernels, with the values of the reversal film. FP6 has a second form:

| Step | Reversal film |
| --- | --- |
| FP1 to FP3 | No change: the u'v' table of the film, layer gain, spread, Halation. Astia 100F has no modulation transfer curve, so it has no spread. |
| FP4, FP5 | No change in the kernels. The record is the removed dye `s = a_max - a` (lab rule `removed_dye_record`), so the curves F and K of the film rise with exposure, as on a negative. E100 and Astia 100F have no inhibition. |
| FP6, reversal form | Light table (65^3 over the record, cube root of XYZ under CIE D50, lab rule `light_table_view`), cube, display matrix with the clear film as white (rule `clear_film_white`). Then the loupe flare in linear display light (rule `loupe_flare`: `(1 - f) * rgb + f * Gaussian(rgb)`), then the dark surround on luminance (`rgb * Y^(e - 1)`, e = 0.525 / 0.59, Y of linear Rec. 709), then the gamut compression and the encoding. |

- There is no printer balance, no printing gain, no print table composition, no negative grade, and no Cine Look on this form.
- The loupe flare acts on linear display light, not on the transmittance of FP6. So its canonical result `film.loupe_flare.0` has its own key, and a reversal film does not read `film.flare.0`.
- Each film has its grey anchor at D55 (lab rule `daylight_reversal_balance`). A scene light other than `reference` keeps its cast, because a slide has no printer lights.
- The film identifier stays in the key `params.film_print.negative`. A reversal identifier selects this form. There is no new JSON key, and the stored print material does not change.
- The Film list has the reversal films after the negatives, in the lab order: Velvia 50, Velvia 100, Provia 100F, Astia 100F, E100. With a reversal film the panel hides Print Material, and the slider Enlarger Flare has the label "Loupe Flare", with the same range and default.

### 3.4 Negative Scan form (phase F10)

Print material `negative_scan`, "Negative Scan", is a scan of the negative in place of a print (issue #661; lab step P14, the final form by user decision of 2026-11-05). It has two parts: a virtual scanner reads the negative, and a negative rendering model inverts the scan. The DRT of the document (OpenDRT and its settings) shows the result. Lab source: `scripts/poc/p14_negative_scan/negative_scan.py` (`NegativeScanPreview`). It replaces the ADX form Lab Scan of the first pull request of this phase (PR #663, commits 5cdc75af9 and 197326900), which did not ship.

| Step | Lab rule | Negative Scan form |
| --- | --- | --- |
| Entry | | No reference gamut compression (the lab option `entry_gamut_compress` is off). |
| FP1 to FP5 | | No change: the u'v' table, layer gain, spread, Halation, and inhibition of the negative, with the values of the print form. |
| 1. Scan | `ideal_scanner_bands` | `T(a) = log10(sum w_i) - log10(sum w_i 10^-(base + dyes a))` with `w_i = L S_i`: a Gaussian band per channel (650, 545, 450 nm, 40 nm full width at half maximum, cut below 1e-4 of the peak) times a Planckian lamp of 3200 K (`halogen`) or an equal-energy light. A trilinear read of a 65^3 table over the record. |
| 2. Density | `base_relative_density` | `D = T - D_base`, with `D_base = T(0)` of the unexposed film. |
| 3. Inversion | `generic_density_inversion` | `x = contrast (D - D_aim) / gamma + shift`, with one `gamma` per medium (cine, photo) and `D_aim` the grey of the balance light of the stock. `shift = s_auto + exposure log10(2) - (cyan, magenta, yellow)`. |
| 4. Positive | | `E = 0.18 10^x`; `E_base = 0.18 10^(x at D = 0)`. |
| 5. Black | `black_at_film_base` | `E' = 0.18 (E - b E_base) / (0.18 - b E_base)`, and a value below 0 is 0. When `b E_base >= 0.18` the render stops with a named error, as the lab (issue #666). |
| 6. Display | | `E'` is linear AP1. `DrtDisplayPixel` shows it with the settings of method OpenDRT: 0.18 goes in as 0.18. |

- `gamma` of a medium is the mean over its pool of stocks (cine: 5203, 5207, 5213, 5219; photo: Portra 160, 400, 800, Ektar 100, Gold 200, C200) of the density per log10 exposure of the 18 % grey: a central difference at 0.09 and 0.36, each stock under its balance light (D55, or T3200 for a stock with an exposure temperature below 4500 K), with the layer gain of its pool scene light.
- `D_aim` is the grey density of the balance light with the layer gain of the selection. The light axis is `(D(D65) - D(T3200)) / gamma`, the direction of rule `frame_auto_colour`. Both depend on the scene light, because the layer gain under `reference` differs from that of the balance light for a stock that is not relative to it, so the resources store them per negative and scene light.
- The grey records of these values use the point form of the lab (16 steps of rule `development_inhibition`, `kPointInhibitionSteps`); the kernels keep the two steps of FP5. The image differs from the stored grey by the steps only.
- Rule `frame_auto_colour` reads a prescan of `x` before the shift: the box reduction of Pillow to a long edge of 96 (`ReduceFrame`), the pixels with a level in (-1.6, 0.9), the median cast, 4 steps of the near-neutral weighted mean (sigma 0.12), and the median of the top 5 % (white patch). Rule `smaller_cast` takes the estimate of smaller length. The correction is `strength` along the light axis and `0.5 strength` across it, with a target of +0.04 (warm) or -0.04 (cool) along the axis, and `auto_density` times the weighted mean level. Auto is off by default (Neutral Scan).
- The scanner table depends on the negative and the scanner only, so all scene lights share it. With the default scanner the values of a selection are the stored values; with another scanner the host builds the table, derives `gamma` of the medium from its pool, and derives `D_base`, `D_aim`, and the light axis (`film_negative_scan::DeriveMediumGamma`, `DeriveSelectionValues`).
- There is no enlarger flare, no printer balance, no print table, no negative grade, no Cine Look, and no dark surround. Negative Scan has all the settings of method OpenDRT, with HDR; `DrtOutputResolver` resolves it as OpenDRT and does not call `CheckFilmPrintEncoding`.
- The identifier stays in the key `params.film_print.print_material`. The settings are the keys `auto`, `auto_strength`, `auto_density`, `scan_contrast`, `scan_exposure_stops`, `cyan`, `magenta`, `yellow`, `black`, `scanner_red_nm`, `scanner_green_nm`, `scanner_blue_nm`, `scanner_width_nm`, and `scanner_light` of `params.film_print`. A key at its default is not written, so a document with the defaults has the bytes and the root id of a document of before. A reversal film reads no print material, so a stored `negative_scan` has no effect on it.
- With Negative Scan the panel hides the slider Enlarger Flare (its stored value does not change) and shows the groups "Negative Scan" (Auto Color, Auto Color Strength, Auto Density, Scan Contrast, Scan Exposure, Cyan, Magenta, Yellow, Black at Film Base) and "Scanner" (the three band centres, the band width, the scanner light), the OpenDRT presets, and the section Color & Encoding. The Print list has Negative Scan first, before Cine Look, with its own swatch.
- GPU: phase 2 of F10 (issue #664) has the CUDA and OpenCL passes, and F11 (issue #662) the Metal pass. With auto colour on, the GPU reduces the scan density T of the full frame to the prescan (a canonical result, `film.scan_prescan.0`), the host reads it and computes the shift, and the display kernel applies it. With auto colour off no prescan is built or read. See the records of F10 phase 2 and F11.

#### Scene light As Shot

Scene light `as_shot`, "As Shot", is second in the list, after the reference. It is for all forms, not only Negative Scan. Its colour temperature is that of the RAW decode: in mode Custom of Develop the Kelvin of the user, in another mode the solved as-shot temperature (`FilmSceneRawCct`). The tint is not used. A raster input or a document with no Develop has no RAW white balance, so As Shot renders as the reference.

- The bundle has a grid of 88 lights `cct2000` to `cct15000`, 5 mired apart (metadata `cct_grid`: kelvin, lights, mired). The host blends the two grid lights around the temperature linearly in mired (`FilmStockBundle::SceneLightMix`); outside the grid it takes the nearest end. The blend applies to the spectrum, the layer gain, the printer balance, the grey of the negative grade, the layer gain of a reversal film, and `D_aim` and the light axis of Negative Scan. The lab sets the step so that a blend is within 0.5 codes of the exact light.
- The keys of the tables have the blend (`MixLight`), so a new temperature builds a new u'v' table, and a change back finds the kept one.
- A write that selects Negative Scan also selects As Shot, unless the same write sets a scene light (`OdtConstrain`). A later change of the scene light stays. Undo of that write restores both values.

The pickers of the film print page stay open after a pick: the picked entry is highlighted and becomes current, so the user can compare films. A click on the card or Escape closes the browser.

---

## 4. Resources

The hidden controls are constants, so each combination of film, print material, and scene light has fixed scalar values. The lab
calculates them; the product does not need the logic of `_camera_filter`, `_printing_gain`, and `_prepare_inhibition`.

| Resource | Content | Size (calculated) | Made by |
| --- | --- | --- | --- |
| Stock bundle | Per negative: base, dyes, layer weights, dye amount curves, F and K of the inhibition, spread lengths. Per paper: base, dyes, printing, Status A, and viewing weights, curves. Scene light spectra. | Under 10 MB | Lab script |
| Print setup | Per film, print material, and light: printer balance, printing gain, layer gain, aim, display matrix | About 200 KB | Lab script |
| Reflectance table | Compact form of the final lab table with rule `foliage_spectra` (192^2 x 481 float32, 71 MB): 18103 nodes, 346 wavelengths, 12 mantissa bits, one byte plane per float32 byte | 25 MB, 5.6 MB after xz | Lab script |
| u'v' table | 192^2, from the reflectance table, the layer weights, and the light. A node that no read can reach holds 0 | 0.6 MB, in memory | `film_tables` |
| Stage A | Negative to paper exposure (65^3) | 6.6 MB, in memory | `film_tables` |
| Stage B | Paper Status A to XYZ (65^3, axis power 2) | 6.6 MB, in memory | `film_tables` |
| Print table | Composed from A and B; RGBA32F for the GPU | 4.4 MB | `film_tables`, `AcquireLut` |
| Light table (reversal film) | 65^3 over the record of removed dye: the cube root of XYZ under the light of the table. One per film, for all scene lights | 6.6 MB, in memory; 4.4 MB on the GPU | `film_tables::CubeRootBandResponse`, `AcquireLut` |

Resource schema 10 (phase F10, lab commit fb785ef) has the values of Negative Scan and the grid of As Shot. The stock bundle has the default scanner (`common/negative_scan_centres_nm` (3), `common/negative_scan_width_nm`, `common/negative_scan_band_cut`, `common/negative_scan_lamp_kelvin`, `common/negative_scan_planck_c2_nm_k`), per negative `scan_base_density` (3), per medium `scan_medium/<medium>/gamma` (3), the spectra `light/D55`, `light/T3200`, and `light/D65`, the 88 lights of the grid, and the metadata: per negative `medium` and `scan_balance_light`, `negative_scan` (id, name, default light, lights, axis lights, and the pool of each medium with the scene light of each pool stock), and `cct_grid`. The print setup has per negative and scene light `negative_scan_aim_density` (3) and `negative_scan_light_axis` (3), for the 99 scene lights. The scanner table of a negative (65^3 over the record, 6.6 MB in memory, 4.4 MB on the GPU) is built on the host by `film_tables::Log10BandResponse`. Schemas 7 to 9 (the chart-calibrated scan and the ADX forms of Lab Scan) did not ship.

Resource schema 6 (phase F8) adds the reversal films. The stock bundle has the group `reversal/<film>/` in the form of `negative/<film>/`, with the record of rule `removed_dye_record` and `dye_max`, and the group `light_table/` with the D50 viewing weights, the XYZ scale, the exponent of the dark surround, and the luminance weights of linear Rec. 709. Its metadata has `reversal_films` and `reversal_list_order`. The print setup has `reversal_layer_gain` (films, scene lights, 3), `reversal_xyz_to_display` (films, 3, 3), and `reversal_scene_spectrum_index` (films, scene lights). The compact reflectance table keeps its form.

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
| F8 | Reversal films on a light table, on CUDA and OpenCL (section 3.3) | 1500 lines | New resource content and a second form of FP6 |
| F9 | Reversal films on Metal | 600 lines | One backend per phase |
| F10 | Print material Negative Scan (section 3.4): phase 1 the model layer, phase 2 the CUDA and OpenCL passes | 2500 and 1500 lines | New resource content and a third form of FP6 |
| F11 | Negative Scan on Metal | 500 lines | One backend per phase |

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

- Lab commits: `film_tables` is a copy from `8b16c67` of alcedo-film-lab, the last commit that changed the library. `build_product_resources.py` is from `f7084c0`, which is also the lab commit that the manifest of the current resources names. The manifest of the resources names the lab commit of its run. The product adds `TableBandIntegral`, the u'v' integral, on the same worker threads. The product copy is now the source of the library, as this phase states.
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
- `SampleLut3d` reads a value `u` at `u * edge - 0.5`, and the lab reads a density at `(d - start) / step` on nodes. Phase F3 must convert with `u = (node + 0.5) / edge`. (Changed in F3: `film_print_math.h` has its own read on nodes.)

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

**Resources with rule `foliage_spectra` (2026-11-01, issue [#635](https://github.com/zidage/AlcedoStudio/issues/635)):** the lab rule moves one u'v' sector of the reflectance table to a leaf shape, because the table had no chlorophyll band near 670 nm and foliage turned yellow. The rule is in the table and has no value per stock, so the product has no new control and no new kernel.

- The complete set comes from lab commit `f7084c0`. The record of that run is in `51f627a`, so the files in `expected/display/` and `expected/image/` name that later commit. `film_stock_bundle.bin` and `film_print_setup.bin` have the same bytes as before. `film_reflectance_table.bin` has a new SHA-256, with the same size and the same array form, so the schema version stays at 1.
- The rule changes 1497 of the 36864 nodes of the complete table.
- `kFilmPrintImplementationVersion` is 5, because the pixels of a film print change and the content keys do not hold a digest of the resources.
- The compact table checks and the host test values in this record did not change at the stated precision.
- CUDA against the new lab values (Debug, RTX 3080 Laptop), in 8 bit codes, with the limits of F3 and F4. `FilmPrintRampMatchesLab`: at most 0.33. `FilmPrintChartMatchesReference`: at most 0.053 against the reference, and at most 0.43 for the reference against the lab. `FilmPrintImageMatchesLab`, p99.9 and largest: `kodak_5207` 0.44, 1.34; `kodak_portra_400` 0.53, 1.47; `fuji_eterna_500` 0.50, 1.33. The records of F3 and F4 hold the values from before the rule.
- `check_foliage_spectra.py`, all 30 negatives, on the clean lab commit: a grey ramp changes by 0 of 8 bit code, the six colours outside the sector change by at most 1, and the largest step along the chroma sweeps grows by at most 1 against a limit of 2.

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
11. **Completion record.** See below.

##### Phase F3 completion record (2026-11-01)

**Status:** complete. A render of a film print on CUDA runs FP1, FP4, FP5 with no blur, and FP6 with no flare. The sliders Light spread, Halation, and Enlarger flare have no effect until phase F4.

**Primary success call chain:**

```text
DrtPostExecutor::Execute -> CudaDrtOps::DispatchFilmPrint -> RecordCudaFilmPrint
  -> DrtOutputResolver::ResolveNode -> SharedFilmPrintTables -> FilmPrintTables::Resolve
  -> CudaBackend::AcquireLut (print table) -> CudaBackend::AcquireKeyedBuffer (kernel values)
  -> film_print_kernel_tables::FillParams
  -> ExposureKernel -> FirstAmountsKernel -> InhibitedStepKernel (two times) -> DisplayKernel
```

**Primary failure call chain:**

```text
Unsupported output, missing or damaged resource, or upload failure
  -> std::runtime_error from RecordCudaFilmPrint -> PlanExecutor catch -> device.CancelRender()
  -> nothing is published, and the next render runs the DRT pass again
Kernel launch error -> CudaDrtOps::CheckAfterEncode -> the same catch
```

**Decisions that the plan did not specify:**

- `film_print_math.h` has its own trilinear read, in place of `SampleLut3d`, which is private to `cuda_primary_grade_pass.cu`. All backends can share this read, and it reads nodes directly, so the conversion that F1 recorded is not necessary.
- The lab limits a table position to `nodes - 1.000001`, which is `nodes - 1` in float32 at these sizes. The math limits the cell to `nodes - 2`, so the read of the last node has a share of 1.
- The locus projection multiplies its test by Y, in place of a division by Y. The result is the same for Y above 0. With a division, a very small Y moved X / Y out of the float32 range and the output was not finite.
- The u'v' table, the curves, and the locus values are one float32 buffer, `FilmPrintTableData::kernel_values`, with the key `kernel_key`. `Resolve` makes it once for each selection. `CudaBackend::AcquireKeyedBuffer` holds it in the storage and the byte limit of the LUT cache, so one limit covers all film print tables on the device.
- The fixed values are a kernel argument by value (`FilmKernelParams`), with no parameter slot. They do not depend on a revision, and an export override needs no special case.
- Each point stage has its own kernel and writes to a pool texture, because F4 puts the blurs between them. F3 uses 2 pool textures at the render size.
- `SharedFilmPrintTables` is one owner for the process, so all devices share the host tables. A failed open keeps nothing.
- `kFilmPrintImplementationVersion` is 2. `kDrtImplementationVersion` stays at 6.
- The display curve comes from the output resolve through `CeDisplayEncodeChannel`, after the clip of the lab. The lab values use sRGB, so the tests select the sRGB curve.
- `scene_flare` of the lab is 0 for all stocks and is not in the resources, so the math has no such term.
- Lab values: `build_display_expected.py` of alcedo-film-lab (`e35ff15`) writes `expected/display/` under `ALCEDO_FILM_PRINT_EXPECTED_DIR`, for the 3 selections of F1 and `kodak_gold_200`, which has no inhibition. They are not in Git. The script sets the lab to 2 inhibition steps for the ramp and the chart, because the kernels run 2 steps. The resource directory is copied to `config/film_print/` next to the test executable. Without these directories the film print cases skip, which is not a pass.
- `FilmPrintChartMatchesReference` also compares the reference with the lab, with a limit of 1.0, because the reference is new code.

**Measured largest difference, final sRGB, 8 bit codes (Debug, RTX 3080 Laptop):**

| Selection | Ramp against the lab (limit 0.5) | Chart against the reference (limit 1.0) | Reference against the lab (limit 1.0) |
| --- | --- | --- | --- |
| `kodak_5207`, `kodak_2383`, `reference` | 0.0002 | 0.0072 | 0.0022 |
| `kodak_portra_400`, `kodak_endura_premier`, `daylight` | 0.0006 | 0.0259 | 0.0039 |
| `fuji_eterna_500`, `kodak_2383`, `street_mixed` | 0.3277 | 0.0533 | 0.4412 |
| `kodak_gold_200`, `kodak_2383`, `reference` | 0.0002 | 0.0038 | 0.0024 |

**What was proven (executed tests):** see the pull request of this phase. The target is `GpuDagCudaDrtProductTest`, suite `CudaFilmPrintFixture`, and `FilmPrintTablesTest`.

**Checklist / exit condition:** all required cases pass on the development GPU.

**Not verified:**

- `film_print_math.h` did not compile as OpenCL C or Metal. Phases F6 and F7 compile it.
- No build ran on macOS.
- No performance values: phase F4 measures the complete chain in a Release build.
- The full test suite did not run.

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
12. **Completion record.** See below.

##### Phase F4 completion record (2026-11-01)

**Status:** complete. The complete chain FP1 to FP6 runs on CUDA, and the sliders Light spread, Halation, and Enlarger flare act on the image. The 24 MP passes take about 21 ms after an edit, under the proposed target of 150 ms. The editor cannot select the method before phase F5.

**Primary success call chain:**

```text
DrtPostExecutor::Execute -> CudaDrtOps::DispatchFilmPrint -> RecordCudaFilmPrint
  -> MakeFilmPrintImagePlan (sizes from the frame, RenderAxisScale, and the canvas)
  -> ExposureKernel (FP1)
  -> BlurHorizontalKernel, BlurVerticalKernel, two times (FP2)
  -> ReadOrBuildCanvasResult (film.halation.0: DecideDiffusionScatter, CanvasReduceKernel, blur, RecordUnpublished)
  half-size inhibitor (sigma of at least 4 pixels):
    -> ReducedFirstAmountsKernel (FP3, FP4) -> blur -> InhibitedStepToReducedKernel (FP5)
    -> blur -> InhibitedStepHalfSizeKernel (FP5)
  full-size inhibitor:
    -> HalationAndFirstAmountsKernel (FP3, FP4)
    -> BlurHorizontalKernel, InhibitedStepVerticalKernel, two times (FP4, FP5)
  -> ReadOrBuildCanvasResult (film.flare.0: CanvasReduceKernel of 10^-a, BoxReduceKernel, blur, RecordUnpublished)
  -> DisplayKernel (FP6)
ROI render -> BindValidResult of both results -> the same kernels read the published images
```

**Primary failure call chain:**

```text
Allocation failure, or a blur beyond the tap limit
  -> std::runtime_error from RecordCudaFilmPrint -> PlanExecutor catch -> device.CancelRender()
  -> RecordUnpublished entries are discarded -> no partial canonical result is visible
```

**Source facts of the ROI audit (section 3.2):**

- The render extent is the visible ROI. `render_geometry_resolver.cpp` adds no margin, so the DRT pass has no pixels outside the ROI.
- The neighborhood steps of `cuda_neighbor_grade.cuh` have no margin either. They clamp a read to the render (`ReadClamped`).
- An ROI edge of the film print needs a margin of 49 pixels at slider 1 and 6 um per pixel: 5 for the spread, and 2 x 22 for the two inhibitor blurs. This phase adds no margin, because a margin changes the geometry of each pass. FP2 and FP4 use the boundary rule reflect at the ROI edge.
- Measured on `_DSC8148` (`kodak_5207`, a 1600 x 1200 ROI), in 8 bit codes: the edge band has a mean of 0.052, p99 1.30, p99.9 3.66, and a largest value of 47.9. Inside the band the largest value is 0.038, because the half-size grid of the inhibitor has another phase. Issue [#636](https://github.com/zidage/AlcedoStudio/issues/636) holds the decision for the review of phase F5.

**Decisions that the plan did not specify:**

- `film.flare.0` is on the canvas, reduced by rule 'large_blur_on_reduced_image': 60 x 40 texels at 2048 pixels, with a sigma of about 6 texels. A FIR of sigma 205 on the canvas would need about 820 taps per side. The lab uses the same reduced grid. All sizes come from the reference frame, so a zoom does not change them.
- `film.halation.0` holds the blurred Q after the spread, as in the lab. The canvas has a sigma of 6.8 texels at 2048 pixels.
- A reduction takes out the variance of the box and of the bilinear read (1/4 of a reduced pixel), as the lab does. A canvas at the size of the frame has no reduction, so its sigma does not change.
- `CanvasReduceKernel` reads `ceil(footprint)^2` nearest render pixels, in place of the bilinear reads of the Diffusion pass, because a blur of at least 6 texels follows. The flare form averages 10^-a in this kernel, so there is no full-size transmittance image.
- A frame with an edge below 16 pixels has no image plane: lab rule 'no_image_plane_steps_on_ramps'. The test is on the reference frame, not on the ROI. The ramp and chart cases of F3 use this rule.
- `FilmBlurKernel` holds one half of a kernel for each layer and travels by value, so a slider change uploads nothing. The limit is 128 taps, which holds a spread at slider 3 up to a long edge of about 40000 pixels. A larger blur stops with an error that states the sigma.
- After FP2, the first exposure image holds the amounts, so FP2 needs only 3 full-size images and FP4 adds none. The inhibitor uses the middle image of FP2, at full size or for its two half-size images.
- Each slider or selection change marks both results stale, as F2 does. An ROI with no valid result builds temporary images from its own pixels and publishes nothing, as the Diffusion filter does.
- A new DRT input now marks both results stale in `CollectStructureChanges`. F2 marked only the scatter image.
- `RenderAxisScale` moved to `resolved_render_geometry.hpp` with no change, so the film print and the adjustment runtime share one rule.
- `kFilmPrintImplementationVersion` is 4. Version 4 keeps the half-size cells on their pixels at an odd size.
- The images between the steps hold three packed floats in the pool image, with no alpha. The display reads alpha from the scene. F6 and F7 must keep this layout with explicit floats, because `float3` of OpenCL C and of Metal has 16 bytes. The canonical results stay `Rgba32f`.
- A half-size cell has two pixels, and the last cell of an odd size has three. The enlarge reads at exactly half of the pixel position, and the reduced sigma uses a scale of exactly 2. This differs from `_reduce` and `_enlarge` of the lab at an odd size. There, cells at a scale near 2 move their centers up to half a pixel from the read position near the middle of the image. An even size has the same pixels as the lab. The lab has not changed yet: issue [#639](https://github.com/zidage/AlcedoStudio/issues/639).
- The lab values come from `build_image_expected.py` of alcedo-film-lab (`f7084c0`). They are not in Git, and without them the image cases skip, which is not a pass. The values have rule `foliage_spectra`, as the product resources do.

**Cost record (Release, RTX 3080 Laptop, the array of `_DSC8148`, box-reduced to each size, `kodak_5207`, median GPU time of 9 renders in ms):**

| Long edge | ACES 2.0 | OpenDRT | Film print, results valid | Film print, after a slider change |
| --- | --- | --- | --- | --- |
| 2560 (`kFastPreviewMaxLongEdge`) | 2.0 | 1.3 | 3.8 | 5.8 |
| 4096 (`kQualityBaseMaxLongEdge`) | 5.1 | 3.4 | 11.7 | 14.1 |
| 6000 | 11.2 | 7.2 | 17.6 | 20.7 |

Film print passes after a slider change, one render, in ms:

| Pass | 2560 | 4096 | 6000 |
| --- | --- | --- | --- |
| FP1 exposure | 0.3 | 0.8 | 1.8 |
| FP2 wide horizontal | 0.3 | 0.7 | 1.5 |
| FP2 vertical, with the narrow Gaussian | 0.7 | 1.9 | 4.3 |
| FP3 canvas | 1.5 | 2.3 | 2.3 |
| FP3 mix and FP4 first amounts | 0.5 | 1.1 | 1.8 |
| FP4 and FP5, the two steps with their blurs | 1.5 | 5.4 | 4.6 |
| FP6 flare canvas | 0.5 | 0.7 | 1.3 |
| FP6 display | 0.5 | 1.3 | 2.7 |

- An interactive render at 2560 reads the valid results only when nothing before the film print changed, so any edit has the cost of the last column. A QualityBase render at 4096 cannot read or publish results (`ResultPersistenceScope::SensorDevelopOnly`), so it always has that cost.
- The target of 150 ms holds, so the stop condition of this phase does not apply.
- The first render of a selection takes about 800 ms: the read of the bundle, the table build, and the upload.
- GPU memory of the film print, from the texture pool peak with the 3 images of the test taken out: about 370 MiB at 2560, 680 MiB at 4096, and 1220 MiB at 6000.
- At 4096 the inhibitor sigma is 3.98 pixels, under the limit of 4 of rule 'inhibitor_on_half_size'. So this size runs the blur at full size, and that is 38 % of its cost. This phase keeps the limit, because a change also changes the pixels of the lab rule. The measured alternative is in the second optimization table.

**Correction of the first cost record.** The first record of this phase stated about 60 ms at 24 MP. That value was too large. The test held no result at the size of the render, and the texture pool frees a size that no result holds. So each render allocated its full-size images again, and the first use of a new 384 MB image took about 10 ms. A render of the product holds its geometry and develop results at that size. The cost test now holds one image of that size and copies the scene on the GPU.

**Optimizations after that record, with no change to a pixel:**

| Change | Gain at 6000, after a slider change |
| --- | --- |
| FP1 tests a circle inside the hull first. 95.5 % of the pixels of `_DSC8148` skip the loop of 41 facets. | About 1 ms |
| The half-size images of FP4 are in the full-size image of FP2, so an edit allocates nothing for them. The pool peak is 184 MiB lower. | About 6 ms |
| FP2 has 2 dispatches in place of 4. One vertical dispatch also reads the narrow Gaussian directly, while its radius is at most 2. A larger one, for example at slider 3 at 6000 pixels, keeps its two passes. | About 4 ms |
| FP3 mix and FP4 first amounts are one dispatch. | About 1 ms |

Together these change the film print after a slider change, in this cost test, from 8.0 to 7.2 ms at 2560, from 19.1 to 16.8 ms at 4096, and from 38.4 to 27.3 ms at 6000.

**Optimizations of the inhibitor blur.** The largest difference from the render before these changes is 0.0003 codes (floating-point rounding only), at 2560, 4096, and 6000 x 4006 on three negatives. Of these sizes, only 6000 x 4006 uses half-size cells, and its size is even. Alpha has no difference.

| Change | Gain |
| --- | --- |
| At full size, the vertical pass and the step are one dispatch, with no blurred image. | 1.3 ms at 4096 |
| The images between the steps have no alpha, so they use 12 in place of 16 bytes per pixel. | With the row below: 6.6 ms at 6000 |
| With a half-size inhibitor, only the last step writes full-size amounts. The first amounts and the first step write their box mean directly, so there is no reduce pass. | See the row above |

Together these change the film print after a slider change from 7.2 to 5.8 ms at 2560, from 16.8 to 14.1 ms at 4096, and from 27.3 to 20.7 ms at 6000. At 6000 the two steps with their blurs go from 9.7 to 4.6 ms.

**Measured, and not adopted:**

| Candidate | Result | Reason |
| --- | --- | --- |
| Half-float images between the steps | No gain at 4096 (14.3 ms), and up to 0.08 codes of difference | At full size, the cost follows pixels x taps, not the bytes: from 2560 to 4096, a factor 3.85 for a calculated 4.1 |
| Dispatch blocks of other shapes (256 x 1, 64 x 4, 8 x 32, 1 x 256) | No gain; a vertical pass with 1 x 256 is 10 times slower | |
| A block cache in shared memory, and one weight for three channels | At most about 1 ms at 4096, inside the noise of the runs (13.8 to 14.6 ms) | Not worth a form that F6 and F7 must port |
| A blur reach of 3 sigma | About 1 ms at 4096, with up to 7.9 codes of difference | Changes rule `_kernel` of the lab for a small gain |
| Half-size limit of 3 pixels in place of 4, with the aligned cells | **10.0 ms at 4096 in place of 14.1.** Difference from full size: `kodak_5207` p99.9 0.03, largest 0.86; `kodak_portra_400` 0.06, 1.91; `fuji_eterna_500` 0.28, 8.19. No change at 2560 or 6000 | Changes an approved lab rule, and the largest value is over the limit of 5 of `check_fast_image_steps.py` on one negative. Issue [#639](https://github.com/zidage/AlcedoStudio/issues/639) holds the decision |
| Half-size limit of 2 pixels | About 0.3 ms at 2560 (measured before the half-size changes), with up to 11.8 codes of difference | A small gain for a large difference |

**What was proven (executed tests, final sRGB in 8 bit codes):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `FilmPrintImageMatchesLab`, limits p99.9 1.0 and largest 4.0 | `GpuDagCudaDrtProductTest` | PASS. `kodak_5207`: 0.44, 1.33. `kodak_portra_400`: 0.53, 1.47. `fuji_eterna_500`: 0.50, 1.16. Debug and Release |
| `FilmPrintViewportKeepsCanvas`, the tolerance of the Diffusion test, 1e-5 x (1 + value) | `GpuDagCudaDrtProductTest` | PASS |
| `FilmPrintSliderBuildsNoTable`: no table build and no upload, and each slider changes the image | `GpuDagCudaDrtProductTest` | PASS |
| `FilmPrintAllocationFailureReportsTheErrorAndPublishesNoPartialResult`: a failure at each allocation of the render | `GpuDagCudaDrtProductTest` | PASS, 11 failures |
| `FilmPrintRoiEdgeDiffersOnlyInsideTheReachOfItsBlurs` | `GpuDagCudaDrtProductTest` | PASS, values above |
| `FilmPrintFusedSpreadMatchesSeparatePasses`, limit 0.01 | `GpuDagCudaDrtProductTest` | PASS, largest 0.00008 |
| `FilmPrintHalfSizeCellsStayOnTheirPixelsAtAnOddSize`, limit 0.01. With the old cells this case gives 5.0 | `GpuDagCudaDrtProductTest` | PASS |
| `FilmPrintKeepsTheAlphaOfTheScene` | `GpuDagCudaDrtProductTest` | PASS |
| `FilmPrintImagePlan.*`, 10 cases of sizes and rules | `GpuDagRawInputTest` | PASS |
| `RuntimeInvalidation.FilmPrintResultsFollowANewDrtInput` | `GpuDagRawInputTest` | PASS |

Commands: the targets in `win_debug`, and the same target in `win_release` with `-DALCEDO_BUILD_TESTS=ON` and the two film print paths. The release tree then went back to its preset values. The record comes from `--gtest_also_run_disabled_tests --gtest_filter=*DrtCostRecordAtEditorSizes`.

Suite totals, Debug: `GpuDagCudaDrtProductTest` 98, `GpuDagRawInputTest` 216, `GpuDagModelGraphTest` 169, `FilmPrintTablesTest` 29. All pass. In `GpuDagOpenClDrtProductTest`, the named stop of the film print passes.

**Checklist / exit condition:** all tests pass, and this record states the evidence of item 9 and the source facts of the ROI audit.

**Not verified:**

- No run in the application, so no real RAW path, zoom, or pan was seen. Phase F5 has the user review.
- The times are for the DRT pass only, with no earlier pass of the render.
- No build ran on macOS. OpenCL and Metal have no image-plane steps until phases F6 and F7.
- The full test suite did not run.

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
12. **Completion record.** See below.

##### Phase F5 implementation record (2026-11-01)

**Status:** the controls are implemented on CUDA. **The phase is not complete:** item 9, the user review, is pending, and issue [#636](https://github.com/zidage/AlcedoStudio/issues/636) closes with it.

**Primary success call chain:**

```text
EditorAdjustmentEnumModel or EditorAdjustmentValueModel (fieldKey odt)
  -> EditorDisplayTransformPanel.buildOdtParams -> EditorParameterCatalog::UiStateToModelJson -> OdtUiToModel
  -> submitPatch -> ParseEditorParameterWrite -> ParseDrtUpdate
  -> DrtParamsModel::ApplyUpdate -> render
  -> ReadOdt -> PanelProjectionToVariantMap -> AdjustmentSnapshotChanged
  -> EditorAdjustmentStack.loadFromSnapshot -> display panel and Post Processing panel
```

**Primary failure call chains:**

```text
A value outside its range or an unknown identifier -> ParseEditorParameterWrite or ApplyUpdate reports the error -> no field changes
editor.set with another encoding on a film print -> OdtConstrain reports the error -> no field changes
A missing resource or an unsupported backend -> the render stops with the named error -> the existing editor error path; the controls keep their values
```

**Decisions that the plan did not specify:**

- Selecting the film print writes Rec. 709, 100 nits, and the current EOTF when Rec. 709 has it, else gamma 2.2. `OdtUiToModel` holds this rule, so both the panel state and `editor.set` follow it. The earlier encoding comes back with undo. Another method keeps Rec. 709 after a selection; the user can change it.
- `editor.set` rejects another encoding space or peak luminance with the film print, and rejects a film print field with another method, as it does for the fields of OpenDRT and ACES.
- The lists come from new constants `kFilmNegativeNames` and `kFilmPrintMaterialNames`, next to the identifier constants of phase F2. The catalog reads no resource file, because opening the bundle reads and hashes 28 MB and a missing resource must stop a render, not the panel. `ShippedNamesMatchTheNamesOfTheDocumentModel` compares them with the shipped bundle.
- The scene light names are new text of the catalog, because the bundle holds only identifiers.
- Labels use the title case of this panel (`Print Material`, `Scene Light`, `Light Spread`, `Enlarger Flare`), not the sentence case of section 2.1.
- The Model stores a float, and the float of 0.1 is above the decimal limit of the catalog, so `OdtModelToUi` limits the read of `enlarger_flare`. Without this, each write at the top of this slider fails.
- A history row of a film print states the method, selection, or slider that changed.
- `ALCEDO_FILM_PRINT_RESOURCE_DIR` also copies the resources next to `alcedo_main`, as the CUDA tests do, so a development build can run the method. The package scripts stop when no directory is given, and the variable of the same name in the environment gives the directory.
- The item stays in the list on OpenCL and Metal, and a render stops with the named error until F6 and F7. `Auto` selects OpenCL first on Windows, so the review needs the CUDA accelerator in Settings.

**What was proven (executed tests, Debug):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| Each field reaches the model, and undo restores it: `FilmPrintFieldsReachTheModelAndTheHistoryValueRestoresThem` | `EditorPipelineCommandServiceTest` | PASS |
| The panel reads the values: `FilmPrintFieldsProjectFromTheModel`, `FilmPrintSnapshotLoadsItsFieldsAndShowsOnlyItsControls` | `EditorPanelProjectionTest`, `EditorDisplayTransformSnapshotQmlTest` | PASS |
| The Halation slider hides and returns: `PostHalationSliderHidesForFilmPrintAndReturnsWithItsValue` | `EditorDisplayTransformSnapshotQmlTest` | PASS |
| Selection over an HDR document: `SelectingFilmPrintWritesRec709SdrOverAnHdrDocument` | `EditorDisplayTransformSnapshotQmlTest` | PASS |
| Catalog rules: `FilmPrintWriteHoldsItsFieldsAndRec709SdrOutput`, `FilmPrintRejectsAnotherOutputAndFieldsOfOtherMethods`, `FilmPrintStoredFlareLimitReadsInsideTheUiRange`, `FilmPrintOptionsAreTheIdentifiersOfTheDocumentModel` | `EditorParameterCatalogTest` | PASS |
| Lists against the shipped bundle: `ShippedNamesMatchTheNamesOfTheDocumentModel` | `FilmPrintTablesTest` | PASS |
| New documents keep OpenDRT: `FilmPrintOptionsAreTheIdentifiersOfTheDocumentModel`, `MethodModelDefaultIsOpenDrt` | The same targets | PASS |

Caller suites, Debug: `AdjustmentTransferCatalogTest` 9, `EditorPanelProjectionTest` 11, `EditorParameterCatalogTest` 33, `EditorPipelineCommandServiceTest` 24, `FilmPrintTablesTest` 30, `EditorDisplayTransformSnapshotQmlTest` 17, and `SimplifiedChineseCatalogTest` 9 pass. `DocumentTransferTest.TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches` fails as in issues #484 and #554. `EditorSessionHistoryPortTest.CheckoutAndPasteHashesAreUnchanged` fails with the hashes of issue #555; the other 115 tests of that target pass.

**End-to-end run (Release, CUDA, headless `alcedo-cli`):** `_DSC4049.ARW` (day) and `_DSC8148.ARW` (night). `editor.set` selected the film print, then the sliders and selections, and `render.preview` wrote each result. Each change had a visible result. Two `editor.undo` calls restored the earlier field values and OpenDRT. This run does not replace item 9.

**Not verified:**

- User review of item 9: RAW files by day and by night, against the lab viewer, and the absence of reference gamut compression at the entry.
- The panel in a window. The QML tests load the panel with no real input.
- A complete package, and any build on macOS.
- The full test suite did not run.

##### Fix of issue #642 record (2026-11-01)

**Fault.** Eterna 500, Eterna 500 Vivid, `kodak_5277`, `kodak_5203`, and `kodak_5206` looked sharpened. The fits of the lab discarded their inhibitor length, and the render used 35 um, so the render had an edge effect that the modulation transfer chart did not have. Lab report: [P7, section 5.6](https://github.com/zidage/alcedo-film-lab/blob/feature/p7-product-resources/docs/exploration/p7_spectral_acceleration.md), lab decision U74.

**Rule `inhibition_image_part`.** K stays as the total, because K also sets the colour of a flat area, and lab decision U72 accepts these colours. `F_i` stays on this total. Only the part `K_image`, from a fit at 35 um, has a blur. Array `negative/<id>/inhibition_image_k` holds it, and the resource schema version is 2.

**Change from the form in the issue.** The issue proposes `a = F(log H - K_image G(a) - (K - K_image) a)`. That form needs the full-size amounts of the previous step at each pixel, and the half-size path does not write them. The user requires no increase of the render time. So the first pass computes the point part once:

1. `a0` = the curve of `log H`, and `a1 = F(log H - K a0)`.
2. The pass stores `log H' = log H - K_point a1` in the image of the exposure.
3. Each step reads `log H'` with no `log10`: `a = F(log H' - K_image I)`.

A negative with no image part runs no blur: one dispatch computes the two steps of the total K at each pixel. This also is the form with no image plane. The later steps of all negatives have 6 fewer `log10` calls per pixel, and this pays for the 3 reads of a curve in the first pass.

**Cost, Release, RTX 3080 Laptop, two runs of `DrtCostRecordAtEditorSizes`, GPU ms after a slider change:**

| Size | `kodak_5207` before | After | `kodak_ultramax_400` (both parts) before | After | `fuji_eterna_500` (point part only) before | After |
| --- | --- | --- | --- | --- | --- | --- |
| 2560 x 1709 | 5.70, 6.22 | 5.79, 5.79 | 5.66, 6.25 | 5.76, 5.74 | 6.23, 5.66 | 4.15, 4.11 |
| 4096 x 2735 | 14.45, 14.59 | 13.94, 14.11 | 15.10, 14.32 | 14.04, 14.08 | 17.38, 17.27 | 7.96, 8.17 |
| 6000 x 4006 | 20.73, 21.17 | 20.59, 21.36 | 26.10, 27.55 | 21.20, 21.01 | 26.75, 27.33 | 21.57, 21.55 |

The texture pool peak does not change: 571, 1196, and 2371 MiB.

**Checks.** The lab check `check_inhibition_image.py` has limits set before the run, and all pass: the largest response above the chart is 0.09 on 29 negatives (1.12 before), Eterna 500 has an edge of 0.02 codes (14.6 before), and its mean chroma is 54.42 (54.39 before). `kodak_5207` and `kodak_portra_400` differ from before by a mean of 0.017 and 0.033 codes. Tests `FilmPrintStepEdgeFollowsTheImagePartOfTheInhibition`, `FilmPrintWithNoImagePartRunsNoInhibitorBlur`, and `KernelParamsSplitTheInhibitionIntoAnImagePartAndAPointPart` hold the rule. After this change, the image comparison with the new lab values, p99.9 and largest in codes: `kodak_5207` 0.44, 1.34; `kodak_portra_400` 0.53, 1.47; `fuji_eterna_500` 0.51, 1.44. The earlier records hold the values from before this rule.

**For F6 and F7.** The kernels of `cuda_film_print_pass.cu` are the form to port: after FP4 the image of the exposure holds the log exposure, and `inhibitor_blur` selects the no-blur path.

**Open.** The user review of the new look of these stocks joins item 9 of F5. The charts are digitized values that were not checked against the manufacturer documents.

##### Neutral axis of the projected prints record (2026-11-01)

**Fault.** The user found Kodak 2383 yellow-green. A neutral ramp of `kodak_5207` on `kodak_2383` had green shadows (L* 20: a* -5.3, b* +1.5) and yellow-green highlights (L* 85: a* -3.5, b* +9.3).

**Cause.** Lab rule `neutral_printing_gain` makes the shadows green, and the perfect reflector as display white adds about b* +6 along the ramp.

**User decision.** The user compared three versions on four photos, selected the print with no gain, and asked for the change. This replaces the 2383 colours of lab decision U72. It is not the neutralization that section 1 rejects: a negative keeps its own difference from the other negatives, and the white has one value per print material.

**Change, replaced by the [record of issue #645](#fix-of-issue-645-record-2026-11-02).** Lab decision U75, section 5.7 of the P7 report. The manifest names lab commit `476153c`, and the record of that run and the expected values are from `37443d5`. Projected prints have no neutral printing gain, and lab rule `projected_print_white` gives each of them one display white. Reflection papers do not change. Only values of `film_print_setup.bin` change: `printing_gain` and `xyz_to_display`. The layout stays, so the schema version stays 2, the kernels do not change, and `kFilmPrintImplementationVersion` is 7. F6 and F7 have no extra work.

**Checks (debug, RTX 3080 Laptop).** `FilmPrintTablesTest`: 31 of 31 pass. `GpuDagCudaDrtProductTest --gtest_filter=*FilmPrint*`: 17 of 17 pass, with the new lab values. Largest ramp difference 0.30 of an 8 bit code; largest image difference 1.47 codes. The lab rule against the look that the user selected: 0.0003 codes on `_DSC8148`. The mean a*b* distance of the 5207 + 2383 neutral axis from the Resolve LUT `Rec709 Kodak 2383 D65` goes from 6.8 to 2.1. That LUT is an emulation, not a measurement of a real print.

**Not run.** The review of other stocks and print materials by the user, the full test suite, and a complete package.

##### Default print material record (2026-11-01)

The user finds 2383 strange on daylight photos, and selects Kodak Endura Premier as the default print material of the film print (lab U76). The default negative stays `kodak_5207`. `FilmPrintParams` holds the value, and the panel and the parameter catalog read it. A document with the film print holds its `print_material`, so it keeps its material and its bytes. A document with another method omits the object, so its bytes do not change, and it gets the new default when the user selects the film print.

##### Fix of issue #645 record (2026-11-02)

**Fault.** The user finds Kodak 2383 strange on daylight photos, and states that the cause is in the photochemical chain, not in the neutral axis and not in a distance from a Resolve LUT. The user also asks to remove the values that were selected by comparison with spektrafilm, and to build the print from its process. Lab decisions U77 and U78; sections 5.8 and 5.9 of the P7 report.

**Cause of the colour.** The printer model was lamp A with no filter. The data sheets of 2383 and 2393 state: "Overall filtration should include a UV-absorbing filter, such as a KODAK WRATTEN Filter No. 2B." The blue layer of a cine print stays sensitive down to the end of its chart, and the yellow dye of a negative falls below 450 nm. So 25 % of the blue layer exposure of `kodak_2383` through a `kodak_5207` grey fell below 400 nm, where the exposure does not follow the image. For `kodak_5383` the share was 68 %. On reflection papers it is about 3 %, which is why Endura Premier did not show this fault. This is the low blue slope that section 8.4 of lab report P2 could not explain, and that the lab rules `neutral_printing_gain` and `projected_print_white` tried to absorb.

**Change.** Each value now has a source or is removed:

| Item | Before | Now | Source |
| --- | --- | --- | --- |
| Printer light of a cine print | Lamp A | Lamp A with a Wratten 2B filter (rule `printer_uv_filter`) | Data sheets of 2383 and 2393 |
| Grey of a cine print, Status A | 0.85, by comparison with spektrafilm | 1.09, 1.06, 1.03 | LAD patch, KODAK H-61b |
| Paper log exposure scale, all prints | 0.85, by a sweep against spektrafilm | 1: the published curve | Data sheets |
| Paper flash, all prints | 0.02, by eye | 0 | No measured value |
| Surface reflection, all prints | 0.003, by eye | 0 | No measured value |
| Viewing light of a cine print | D65 | D54 (rule `projection_light`) | SMPTE 196M-2003: 5400 K, x = 0.332, y = 0.347. D54 as the spectrum of a xenon lamp is an inference |
| White of a cine print | Fitted from the 5207 print (`projected_print_white`, removed) | The clear print goes to display white (rule `clear_print_white`). One value per material, with no negative | In a dark cinema the clear print is the brightest light |
| Surround of a cine print | None | Exponent 0.525 / 0.59 = 0.89 on luminance relative to the clear print (rule `dark_surround_on_display`) | CIECAM02 surround values, dark and dim |

A reflection paper keeps D50, the perfect reflector as white, and the grey at Status A 0.75, which is 18 % reflectance.

**Product.** Resource schema 3: each paper has `clear_luminance` and `surround_exponent`. `FilmPrintTables::Resolve` applies the exponent to the print table after the composition. The kernels do not change, so F6 and F7 have no extra work. `kFilmPrintImplementationVersion` is 8. A build tree needs a relink to copy the new resources; an old executable stops on the schema version.

**Result, neutral ramp, a\*, b\* at the same L\*:**

| Selection | | L\* 20 | L\* 45 | L\* 60 | L\* 75 | L\* 85 |
| --- | --- | --- | --- | --- | --- | --- |
| 5207 + 2383, reference | Before | +1.3, -8.6 | +1.2, -4.9 | -0.1, 0.0 | -2.6, +3.8 | -4.6, +4.8 |
| | After | -1.5, -2.1 | -1.0, -2.5 | -0.5, +0.2 | -2.3, +3.0 | -4.0, +3.2 |
| 5219 + 2383, reference | After | -2.9, -2.7 | -2.9, -2.9 | -1.9, -0.4 | -3.1, +3.0 | -4.4, +3.7 |

On 5207 + 2383, the range of b\* for L\* 15 to 88 goes from 13.7 to 7.2, and of a\* from 6.3 to 4.9. A grey of 0.18 goes from L\* 46.2 to 44.9. The black goes from L\* 3.7 to 1.0, and the top of the ramp from L\* 92.9 to 99.0. A sky of AP1 (0.10, 0.22, 0.42) goes from L\* 45, a\* -13, b\* -26 to L\* 46, a\* -3, b\* -39.

**Checks (debug, RTX 3080 Laptop).** `FilmPrintTablesTest`: 32 of 32 pass, with the new test `DarkSurroundMovesLuminanceAndKeepsChromaticity`. `GpuDagCudaDrtProductTest --gtest_filter=*FilmPrint*`: 17 of 17 pass, with the new lab values. Largest ramp difference 0.47 of an 8 bit code; largest image difference 1.70 codes. These runs read resources with the name of a lab tree with uncommitted changes. The resources were then built again: the manifest names lab commit `3020bd2`, and the record of that run is `ce2e8cc`. The tests did not run again after that build.

**Limits.**

- No real print was measured. The look is not reviewed on the RAW files of the issue, which were not on this machine.
- Chroma rises, because the published curve is steeper than the 0.85 scale. The default grade of the product (+1.5 EV, contrast 15, saturation 1.3) stays in front of the film, and its saturation was selected with the earlier print.
- The LAD patch is not neutral on this chain: L\* 44.5, a\* +0.7, b\* -6.0, where H-61b states a visual neutral. So the digitized dyes or the adaptation to the clear print has an error of this size.
- Blue through the print stays about 12 % below green on `kodak_2383`, as the printing slope times the paper slope. The digitized curves were not checked against the Kodak documents.
- These values keep no source and stay: the default of the Enlarger flare slider (0.01), the Halation strength, the share 0.5 of the inhibition between layers, the band split of the dyes for a stock with no published dyes, rule `grey_anchor` for a stock with a relative chart, and the Gaussian tail of a dye below 400 nm. The inhibition is an empirical model that agrees with DIR couplers in quality only. It has the algorithm of spektrafilm, with K from a fit to the modulation transfer charts here. KODAK H-24 modules 1, 7, 8, 9, and 10 give no value for it. It fails the form of a real over-corrected negative (section 5.10 of the P7 report): on `kodak_5207` the separation curve is below the neutral curve, and the K adds only +0.014 density at 1.3 above base. The model does not change from this check.
- A reflection paper has no surround rule. Endura Premier, the default print, changes its look with the curve and the flash: a grey stays near L\* 50, and the black falls.

**Not run.** The review by the user, the other cine pairs, the full test suite, and a complete package.

##### Saturation of the default look (record, 2026-11-02)

**Fault.** The user finds the saturation too high on Eterna 500, Ektar 100, and Portra 800, and the highlights pink on a cine print. Lab decision U80, section 5.12 of the P7 report.

**Change.** The default of a RAW document stays saturation 1.3. When an `odt` edit changes the method to `film_print`, `EditorHistoryMutation::CommitAdjustment` also writes 1.0 to the saturation of the Default Grade, in the same history batch, so one undo restores both. This applies only while the value is still the import default; a value that the user selected stays. Leaving the film print does not restore the value. K total equals the image part (lab rule `inhibition_total_from_image_part`), so `fuji_eterna_500` has no inhibition. Each negative on a cine print has `printing_gain` from lab rule `print_slope_match`, strength 0.5: a product choice, with no such step in a real printer. The entry of the CUDA film print has the reference gamut compression. Resource schema 3 does not change. `kFilmPrintImplementationVersion` is 9.

##### Look of Kodak Vision 2383 (record, 2026-11-02)

**Fault.** The user finds that 5207 on 2383 at midday looks thin and strange, and asks for a stylized look near La La Land (issue #645). Lab decision U81, section 5.13 of the P7 report.

**Change.** The user selected candidate P6 of a local look study, and asked not to merge it with other candidates. Lab rule `print_cinema_look` is a fixed creative step on the sRGB code of the finished print. All its constants are creative choices, and it has no measurement. `FilmKernelCinemaLook` has the steps of `cinema_look.py` of the lab, after `FilmKernelFinishDisplay`. Only `kodak_2383` has it, through the value `paper/<id>/cinema_look` of the stock bundle, so a document with another print material does not change. Resource schema 4. `kFilmPrintImplementationVersion` is 10.

**Not done.** The user review in the running application. The other negatives on 2383 take the same step with no review. A scan mode has no product path, because the user has not selected its look.

##### Cine Look and the list order (record, 2026-11-02)

**Decision.** The user finds the look good, and states that it is very different from 2383, so the product must not name it 2383. Lab decisions U82 and U83, sections 5.14 and 5.15 of the P7 report.

**Change.** New print material `cine_look`, with the name "Cine Look (2383)": the data of Kodak Vision 2383, with `FilmKernelCinemaLook` on any negative, and on the tungsten stocks (lab U84) the new `FilmKernelNegativeGrade`. This kernel is a fixed creative grade of the negative record, after the enlarger flare and before the print table. All its constants are creative choices. `kodak_2383` has neither step, so it returns to the print conditions of lab U80. The other materials and the defaults do not change. The arrays of `kFilmNegativeIds` and `kFilmPrintMaterialIds` set the order of the editor lists: the common stocks first, then Fuji, then the old Kodak stocks. A document stores the identifier, so the order has no effect on a document.

**Versions.** Resource schema 5: `negative/<id>/negative_grade`, and `negative_grade_grey` of the setup. `kFilmPrintImplementationVersion` is 13.

**Noise.** The user reported that the grade adds much noise on an ISO 3200 file. The density loss of a colour where green or blue leads read the noise of one pixel and moved all channels. It now reads the amounts before the flare with a blur of 35 um (lab U85), with the kernels and the half-size rule of the inhibitor, so a stock with this grade has one more blur. A lab check limits the luma noise on a flat dusk blue to the noise of Kodak Vision 2383.

**Not done.** The user review of each stock in the running application. The tungsten stocks: `kodak_5219`, `kodak_5213`, `kodak_5277`, `kodak_5293`, `kodak_exr_5248`, `fuji_eterna_500`, and `fuji_eterna_500_vivid`.

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
11. **Completion record.** See below.

#### F6 completion record (2026-11-02)

**Call chain.** `DrtPostExecutor::Execute -> OpenClDrtOps::DispatchFilmPrint -> RecordOpenClFilmPrint -> film_print.cl`. The program `opencl_dag_film_print` loads `color_encoding_math.h`, `aces_reference_gamut_compression.h`, `film_print_math.h`, and `film_print.cl`.

**Changes from the plan, and details that the plan does not state.**

- The images between the steps are transient buffers, with 3 floats per pixel. A pool image of OpenCL is an image object: a kernel cannot read and write it, and it has 16 bytes per pixel. So an image for FP4 in place would need a second image. Only the two canonical results are images, because they have a place in the result cache. The scene and the display keep the dual-storage binding.
- The fixed values go in one upload per render, and not in the kernel arguments. A `FilmBlurKernel` has 1552 bytes, and OpenCL 1.2 requires only 1024 bytes for `CL_DEVICE_MAX_PARAMETER_SIZE`. An AMD device can have this limit.
- The kernels read a blur image and read the full-size images with explicit offsets, because the transient allocations share slabs. The half-size images have their own allocations.
- `TransientBufferArena::ReleaseIdleSlabsWhenNoneHolds` is new. Without it, each larger render size adds slabs that nothing can use: 1936 MiB after 2560, 4096, and 6000 pixels, against 1232 MiB with it. The slabs stay for later renders of the same size or a smaller size, and they stay on the device after the method changes, until the binding is released.
- `WarmUpPlan` does not build this program, because an `ExecutionPlan` has no DRT method. The first film print render builds it on the render thread.
- `film_print_math.h` is in `ALCEDO_OPENCL_PACKAGED_HEADERS`. Without this, the installed application cannot open the header.
- The cost record measures wall time to the end of the GPU work, because the queue may have no profiling. The pass names of the test replace `FilmPrintPassTime`.

**Tests.** All on the NVIDIA GeForce RTX 3080 Laptop GPU, on a debug host build.

| Check | Result |
| --- | --- |
| `GpuDagOpenClGradeTest`, `OpenClFilmPrintFixture` | 18 of 18 pass, with the limits of CUDA |
| Ramp against the lab | largest 0.0005 codes (limit 0.5) |
| Chart against the reference | largest 0.016 codes (limit 1) |
| 6000 x 4006 image against the lab | p99.9 up to 0.57, largest up to 1.54 codes (limits 1 and 4) |
| Viewport ROI inside the reach of the blurs | largest 0.036 codes (limit 0.1) |
| `FilmPrintMatchesCudaReferenceWithinTolerance` | pass: below 1 code against CUDA on 3 negatives and 2 print materials |
| `TransientBufferArena` cases | pass |

The other cases of `GpuDagOpenClGradeTest` and `GpuDagOpenClDrtProductTest` pass, except 6 cases of `OpenClMultiGradeFixture`, known as #496. They fail in the same way with this change removed. In `GpuDagOpenClWorkspaceTest`, three queued upload cases time out on most runs, with or without this change: #648.

**Cost.** Slider change, on the GPU above: 6.1 / 13.9 / 19.0 ms at 2560 / 4096 / 6000 (CUDA: 5.8 / 14.1 / 20.7 ms).

**AMD.** The program builds with no error on drivers 26.10.2, 26.6.2, and 26.6.1, on gfx1030, gfx1100, gfx1103, and gfx1201, and on gfx1010 with the legacy runtime. The 19 programs, read from the installer copy, had 342 builds with no failure on 26.10.2 and 26.6.2, with code pages UTF-8 and GBK. The first build found one error that NVIDIA would also report: `kernel` is a keyword of OpenCL C.

**Package.** `package_windows.ps1` ran to the end, with the install tree check. From the extracted NSIS file in a path with Chinese characters and a space, and with a clean `PATH`: `alcedo-gpu-check --opencl` builds 19 programs and 118 kernels, of which `opencl_dag_film_print` has 15, and `alcedo_main --verify-qml-imports` passes. The film print cases also pass with a test executable that reads only the `opencl/` and `config/film_print/` directories of the extracted file.

**Not verified.** A run on an AMD GPU: AMD support here means that the driver compiler builds the program. Polaris, Vega, and Ryzen APUs with Vega graphics: the legacy runtime cannot build for them offline. A render in the running application on OpenCL. An Intel GPU. The full test suite did not run.

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
11. **Completion record.** See below.

#### F7 completion record (2026-11-02)

**Call chain.** `DrtPostExecutor::Execute -> MetalDrtOps::DispatchFilmPrint -> RecordMetalFilmPrint -> film_print.metal`. Each dispatch binds its pipeline state from `metal::ComputePipelineCache` on the compute encoder of the frame (`EnsureComputeCommandEncoder`), and `NoteComputeDispatch` orders the next dispatch after its writes. A missing function or pipeline state throws with its name. A command buffer error reaches `MetalBackend::Wait`, which throws the error of Metal.

**Changes from the plan, and details that the plan does not state.**

- `film_print.metal` has its own AIR file, compiled with `-ffast-math` as `drt.metal`, and the DRT metallib links it with `drt.air` (argument `LINK_AIR` of `alcedo_add_metal_library`). Fast math changes FP6 display from 3.6 to 2.0 ms at 2560, and the pixels stay inside the same limits as with precise arithmetic.
- The kernels are not the CUDA kernels in another language. On the M4 most passes are limited by the bytes that they move, and a blur by the work for each tap, so the form of each image and each blur follows that (header comment of `film_print.metal`):
  - The images between the steps are transient buffers, as on OpenCL: a pool image of Metal is a texture with 16 bytes per pixel. `setBuffer` takes the offset of each allocation. Q, the spread Q, log Q, and the final amounts are `packed_float3` (12 bytes). The images that only a blur or a bilinear read uses are `half4` (8 bytes): the amounts before the last step of a full-size inhibitor, the middle image and the half-size cells of the inhibitor, the blurred amounts of the negative grade, and the Halation and flare canvases before their blur. The Halation canvas keeps exposures up to the largest finite half (65504), more than 16 stops above the criterion of the curves.
  - A blur thread computes 8 outputs along its line (`FILM_SLIDE`). The pure blur passes take the taps in chunks of 8 from two register banks with fixed indices (`FilmBlurLine`); the two kernels that do a step after their blur (`film_print_step_vertical`, `film_print_spread_vertical`) move one window of 8 values one tap at a time (`FilmBlurSlide`), which uses fewer registers. A window of half texels stays half in registers; the tap sums are float.
  - The display stage reads the alpha of the scene only when FP1 marked a pixel with an alpha other than 1, so a frame with opaque alpha does not read the scene twice. FP1 writes the submission id of the render to a 4-byte mark; an older value in the mark means opaque alpha, and a value that matches by chance only reads the scene again.
  - The two canonical results are `Rgba32f` textures, because they have a place in the result cache. The scene and the display are scene textures.
- The dirty field `DrtDirty::FilmPrint` is now three fields (`edit/graph/drt_node_model.hpp`). `FilmExposure` is the negative, the print material, the scene light, and Light spread; it invalidates both canonical results. `FilmHalation` invalidates only the flare result, because the Halation result is the blurred exposure and the strength scales it at the mix. `FilmFlare` invalidates neither result, because the display stage scales the flare result. This is the same on CUDA and OpenCL. `FilmPrintStrengthSlidersKeepTheCanvasesThatDoNotReadThem` shows that a render with kept results gives the same pixels as a render that builds both again.
- The fixed values (`FilmKernelParams`) and each blur kernel are `constant` arguments from `setBytes`, which allows 4 KB, so a render uploads nothing for them. A texture argument that a dispatch does not read gets the scene texture.
- `MetalBackend::AcquireKeyedBuffer` is new. It keeps the kernel values in the cache and the byte limit of `AcquireLut`, as on CUDA and OpenCL. A new cache entry now counts as an upload for the test hooks `FailNextUpload` and `FailUploadAfter`, as the upload of a new entry does on OpenCL.
- `WarmUpMetalDagPlan` does not add the 15 film print pipelines, because an `ExecutionPlan` has no DRT method. The first film print render creates them from the cache.
- `FilmPrintLargerRenderDoesNotKeepTheSlabsOfASmallerRender` counts only the buffers that the DRT pass creates. `BufferCreateCount` of Metal also counts the shared staging pages of the upload and the download of the test.
- The OpenCL case for a scene-work buffer is `FilmPrintReadsASceneWorkImageAndWritesAWorkImage` on Metal, because a Metal scene-work member is a texture only.

**Tests.** Apple M4 (8-core GPU), macOS, Debug host build (`build/macos-debug`), with the resources and the lab values of the F1 and F4 records.

| Check | Result |
| --- | --- |
| `GpuDagMetalDrtTest`, `MetalFilmPrintFixture` | 19 of 19 pass, with the limits of CUDA |
| Ramp against the lab | largest 0.0005 codes (limit 0.5) |
| Chart against the reference | largest 0.016 codes (limit 1) |
| 6000 x 4006 image against the lab, 5 selections | p99.9 up to 0.57, largest up to 1.54 codes (limits 1 and 4) |
| Viewport ROI inside the reach of the blurs | largest 0.037 codes (limit 0.1) |
| Fused spread against separate passes | largest 0.0001 codes (limit 0.01) |
| Allocation failure at each allocation of the render | 4 failures, nothing published |
| Half amounts before the last full-size step, against float, lab image at 1600, 2560, and 4096, 3 negatives | largest 0.023 codes |
| Other cases of `GpuDagMetalDrtTest` | 14 of 14 pass |
| `GpuDagModelGraphTest`, `GpuDagRawInputTest` (dirty fields, invalidation) | 169 of 169 and 213 of 213 pass, 5 skipped for camera files that are not on the Mac |
| `GpuDagMetalWorkspaceTest` | 11 of 11 pass |

In `GpuDagMetalGradeTest`, 11 cases fail in the same way with this change removed: 5 are #497 and 6 are the Metal form of #496. In `GpuDagMetalRendererTest`, `InteractiveQualityBaseInteractiveReuses2560PixelResults` fails in the same way with this change removed: #650. The 15 other cases pass, among them the upload failure cases.

**Cost.** Wall time in ms to the end of the GPU work after a slider change, median of 9 renders, on the Apple M4 (`DISABLED_DrtCostRecordAtEditorSizes`). The host is a Debug build. The Metal compiler optimizes the shaders in this configuration (only `-gline-tables-only` is added). A Halation change keeps the Halation result. A Light spread change, as any change of the scene, builds both canonical results again. The first port had 31.8, 79.8, and 102.8 ms for a Halation change.

| Long edge | ACES 2.0 | OpenDRT | Film print, Halation change | Film print, Light spread change |
| --- | --- | --- | --- | --- |
| 2560 | 6.7 | 3.6 | 14.5 | 17.5 |
| 4096 | 11.3 | 7.9 | 35.5 | 39.1 |
| 6000 | 22.8 | 17.5 | 62.1 | 67.2 |

GPU time of each pass after a Light spread change at 2560, in ms, with one commit and wait for each pass (sum 16.5):

| FP1 | FP2 horizontal | FP2 vertical | FP3 canvas | FP3 mix, FP4 | FP4 horizontal, 2 passes | FP4 vertical and FP5, 2 passes | FP6 flare canvas | FP6 display |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1.24 | 1.11 | 1.96 | 2.62 | 1.76 | 1.88 | 2.72 | 1.20 | 2.00 |

- All values are below the target of 150 ms of F4. At 2560 the film print is 3 times the CUDA value on the RTX 3080 Laptop (5.8 ms, before the change of the dirty fields). 4096 runs the inhibitor blur at full size, as on CUDA (sigma 3.98, under the half-size limit of 4).
- The M4 moves about 93 GB/s in these passes. FP1 and the FP2 horizontal pass take the time that their bytes take; the FP2 vertical pass, the mix of FP3, and the two steps of FP5 take 15 to 35 % more. The blurs with more taps (FP3 canvas, the FP4 horizontal passes at 4096) are limited by the work for each tap.
- Measured and not kept: Q, spread Q, and log Q as half (chart 2.3 codes, limit 1; ramp 0.09 codes); final amounts as half (chart 14.8 codes); `RGBA16F` textures for the images between the steps, and `float4` buffers (no gain or slower); blur tiles in threadgroup memory (fewer threads in flight, slower); a lane shuffle form of the horizontal blur (3 to 5 times slower); chunked blur loops in the two kernels with a step after the blur (slower than the window of one bank); one `float4` read per node of the print table in FP6 display (no change).

**Not verified.** A Release build, the packaged application, and a render in the running application on macOS. A Mac with an Intel CPU or an AMD GPU. The full test suite did not run.

### F8: reversal films on CUDA and OpenCL

1. **Objective.** The five reversal films of lab decision U86 in the film print method, with the reversal form of section 3.3, on CUDA and OpenCL.
2. **Inputs.** F6; resources of schema 6 from the lab (`build_product_resources.py`, lab commit 851cf59).
3. **Modules.** `include/edit/graph/film_print_model.hpp`, `edit/runtime/film/`, `film_print_math.h`, the CUDA and OpenCL film print passes, `runtime_invalidation.cpp`, `editor_parameter_catalog.cpp`, `EditorDisplayTransformPanel.qml`.
4. **Data rules.** FP1 to FP5 do not change. A reversal film reads no print material and no `film.flare.0`. The film identifier stays in `params.film_print.negative`.
5. **Steps.** (a) Read schema 6. (b) Build the light table and the values of a reversal selection on the host. (c) Add the reversal form of FP6 and the loupe flare result to CUDA and OpenCL. (d) Add the films to the Film list and change the panel. (e) Stop on Metal with a named error until F9.
6. **Success chain.** A reversal film renders on CUDA and OpenCL; a Loupe Flare change builds no table and no canonical result.
7. **Failure chain.** A missing or wrong resource array stops `FilmStockBundle::Open` with its name. On Metal a reversal film stops with a named error. There is no change of backend and no change of form.
8. **Tests.** Section "Tests" of the record below.
9. **Exit criteria.** The limits of F3 and F4 on the five films, on CUDA and OpenCL.
10. **Expected diff.** 1500 lines.
11. **Completion record.** See below.

#### F8 completion record (2026-11-03)

**Call chain.** Host: `FilmPrintTables::Resolve -> ResolveReversal -> LightTableOf -> film_tables::CubeRootBandResponse`, and `film_print_kernel_tables::FillParams -> SelectionValuesOf -> FilmStockBundle::ReversalSelection`. CUDA: `DrtPostExecutor::Execute -> CudaDrtOps::DispatchFilmPrint -> RecordCudaFilmPrint`; FP1 to FP5 are unchanged; then `CanvasReduceKernel` with `CanvasValue::kDisplayLight`, the canvas blur, the published result `film.loupe_flare.0`, and `DisplayKernel`: `FilmKernelPrintDisplay -> FilmKernelLoupeFlare -> FilmKernelDisplaySurround -> FilmKernelFinishDisplay -> encoding`. OpenCL: `OpenClDrtOps::DispatchFilmPrint -> RecordOpenClFilmPrint -> film_print_canvas_reduce (FILM_CANVAS_DISPLAY_LIGHT) -> film_print_display`, with the same functions of `film_print_math.h`.

**Changes from the plan, and details that the plan does not state.**

- `FilmKernelParams` has three new members: `display_luminance` (the luminance weights of linear Rec. 709 from the resources), `display_surround_power` (e - 1; 0 for a print, so the print form skips the step), and `light_table`. The size is 412 bytes. The static checks of CUDA, OpenCL, and Metal have this size.
- The light table depends on the film only, so its key has no scene light, and the 11 scene lights of a film share one table and one GPU copy. The selection, the u'v' table, and the kernel values have keys with the film and the scene light, in the form of the negatives. The key of a reversal selection does not read the print material, so a change of the hidden print material builds no table.
- The flare canvas of a reversal film is display light: `CanvasReduceKernel` (and `film_print_canvas_reduce`) computes FP6 to the display matrix for each source pixel, before the box reduction. So the canvas has the same value as the linear display light that the display stage mixes with it. The print form keeps the transmittance canvas.
- The dirty fields of F7 are the same. `FilmExposure` and `FilmHalation` invalidate `film.loupe_flare.0` (the display light reads the exposure and Halation); `FilmFlare` invalidates nothing. `LoupeFlareChangeKeepsTheCanvasesThatDoNotReadIt` shows the same pixels as a render that builds all canonical results again.
- `kFilmPrintImplementationVersion` does not change: the pixels of a negative and a print do not change, and the reversal identifiers had no result before this phase.
- In `film_print.cl` the end of the print form is the helper `FilmPrintFinishedLinear`, so the two forms of `film_print_display` share the encoding code.
- The Film list is `kFilmNegativeIds` and then `kFilmReversalIds`. The panel calls `EditorParameterCatalogAdapter::filmIsReversal` to hide Print Material and to change the label to "Loupe Flare". A write from the panel keeps the stored print material.
- Metal: `RecordMetalFilmPrint` throws `the colour reversal film '<id>' needs the light table passes, which the Metal backend does not have` before it reads a table. The Metal lab cases skip the reversal files. Issue #655 (F9) removes the stop.
- `FilmPrintControlWritesItsField` of the panel test selected index 0 as `agfa_vista_100`. The list order of lab decision U83 (commit 9a08fee0e) made index 0 `kodak_5207`, so the test failed on `main` before this phase. The test now finds the index of the film.

**ROI and the half-size inhibitor.** The ROI test of F4 has a limit of 0.1 codes inside the reach of the blurs. With Velvia 50 and the ROI at row 1403 it measures 0.131 codes. At row 1402 it measures 0.0006 codes (`kodak_5207`: 0.036 and 0.0003). So the difference comes from the phase of the half-size inhibitor grid: at an odd row the half-size cells of the ROI move by one pixel. A slide display is steeper than a print display, so the same phase gives a larger code difference. The test now runs both rows on `kodak_5207` and `fuji_velvia_50`. It applies the limit at the even row for both films and at the odd row for the negative, and it reports the odd row of the slide. The values are in a comment on issue #636, which holds the decision about the ROI edge.

**Tests.** NVIDIA GeForce RTX 3080 Laptop GPU, Debug host build, with the resources of schema 6 and the lab values of the same lab commit.

| Check | Result |
| --- | --- |
| `FilmPrintTablesTest` | 42 of 42 pass |
| Light table against the lab, 5 films | relative difference up to 1.6e-15 |
| Reversal setup against the lab arrays, 55 selections (5 films, 11 scene lights) | difference 0 |
| Ramp against the lab, 5 films | largest 0.0005 codes (limit 0.5) |
| Chart against the reference, CUDA / OpenCL | largest 0.015 / 0.027 codes (limit 1) |
| 6000 x 4006 image against the lab, 5 films | p99.9 up to 0.55, largest up to 1.72 codes (limits 1 and 4); Velvia 50 0.547 / 1.719, Velvia 100 0.541 / 1.656, Provia 100F 0.442 / 1.256, Astia 100F 0.510 / 1.570, E100 0.426 / 1.117 |
| Viewport ROI inside the reach of the blurs | see the ROI paragraph above |
| `ReversalFilmPrintMatchesCudaReferenceWithinTolerance` | pass: largest 0.068, 0.218, 0.070, 0.180, 0.096 codes against CUDA (limit 1) |
| Negative and print cases of F3 to F6 | pass, with the values of F4 and F6 |
| `CudaFilmPrintFixture` / `OpenClFilmPrintFixture` | 18 of 18 / 20 of 20 pass |
| `GpuDagModelGraphTest` / `GpuDagRawInputTest` | 170 of 170 / 217 of 217 pass |
| `EditorParameterCatalogTest`, `EditorPanelProjectionTest`, `EditorPipelineCommandServiceTest`, `AdjustmentTransferCatalogTest`, `SimplifiedChineseCatalogTest` | 34, 11, 24, 9, 9 pass |
| `EditorDisplayTransformSnapshotQmlTest` | 18 of 18 pass |

**Cost.** Release build, RTX 3080 Laptop, after a Halation change (`DISABLED_DrtCostRecordAtEditorSizes`). A Halation change keeps the Halation result and builds the flare or loupe flare result again. CUDA is the median GPU time; OpenCL is the median wall time to the end of the GPU work. In ms:

| Long edge | ACES 2.0 (CUDA) | Print form, `kodak_5207` CUDA / OpenCL | Light table, Velvia 50 CUDA / OpenCL | Light table, E100 CUDA / OpenCL |
| --- | --- | --- | --- | --- |
| 2560 | 2.1 | 4.2 / 4.4 | 5.1 / 5.6 | 3.3 / 3.5 |
| 4096 | 5.0 | 11.9 / 12.1 | 14.3 / 14.5 | 7.9 / 7.5 |
| 6000 | 10.9 | 17.8 / 16.6 | 19.9 / 18.9 | 19.5 / 14.8 |

- The light table form costs at most 2.4 ms more than the print form at the same size. The display light canvas computes FP6 to the display matrix for each source pixel, and the display stage adds the loupe flare and the dark surround.
- E100 has no inhibition, so it has no inhibitor blur; at 4096 its time is less than the print form.
- A first render of a new reversal film, with the light table and the u'v' table on the host, takes about 129 ms at 2560. The host light table alone takes 0.18 s in the Debug build.

**AMD.** `opencl_dag_film_print` builds with no error on drivers 26.10.2, 26.6.2, and 26.6.1, on gfx1030, gfx1100, gfx1103, and gfx1201, and on gfx1010 with the legacy runtime.

**Not verified.** Metal (F9; the Metal source compiles only on a Mac, and it was not compiled). A render in the running application. A run on an AMD GPU. The packaged application with resources of schema 6. The user review of the five films. The full test suite did not run.

### F9: reversal films on Metal

1. **Objective.** The reversal form of F8 on Metal, with the light table, the display light canvas, and the result `film.loupe_flare.0`.
2. **Inputs.** F7, F8; a macOS machine.
3. **Tests.** The reversal cases of F8 in `GpuDagMetalDrtTest`, against the same host reference and limits. The stop case `ReversalFilmStopsWithTheNamedErrorUntilMetalHasTheLightTablePasses` goes, and the lab cases read the reversal files.
4. **Tracking.** [#655](https://github.com/zidage/AlcedoStudio/issues/655).

#### F9 completion record (2026-11-03)

**Call chain.** `DrtPostExecutor::Execute -> MetalDrtOps::DispatchFilmPrint -> RecordMetalFilmPrint`. FP1 to FP5 are the kernels of F7, with the values of the reversal film. Then `film_print_canvas_reduce_display_light`, `film_print_box_reduce` when the flare canvas is smaller, the two canvas blurs, the published result `film.loupe_flare.0`, and `film_print_display`: `FilmKernelPrintDisplay -> FilmKernelLoupeFlare -> FilmKernelDisplaySurround -> FilmKernelFinishDisplay -> encoding`, from `film_print_math.h`.

**Changes from the plan, and details that the plan does not state.**

- `RecordMetalFilmPrint` no longer stops on a reversal film. The light table is the print table of the selection (`FilmPrintTables::Resolve`), so the Metal pass binds it in the same buffer as a print table.
- The canvas reduce of `film_print.metal` takes the value of a texel from a function object: `FilmCanvasExposure`, `FilmCanvasTransmittance`, and `FilmCanvasDisplayLight`. The display light form computes FP6 to the display matrix for each source pixel, as `CanvasReduceKernel` of CUDA does, and it is its own kernel, `film_print_canvas_reduce_display_light`, with the light table at buffer 6 and the fixed values at buffer 7. The two kernels of F7 keep their arguments.
- The display light canvas before its blur is `half4`, as the transmittance canvas of a print. The display light of a light table is 0 to about 1, the same range as a transmittance. The cases below hold their limits with this form, so no other form was measured.
- In `film_print_display` the print form is the helper `FilmPrintFinishedLinear`, as in `film_print.cl`. The light table form reads no soft image and no negative grade.
- The Metal cases of F8 are the cases of the CUDA and OpenCL fixtures: the five reversal films in `FilmPrintOutputIsFinite`, `ReversalFilmPublishesTheLoupeFlareResultAndNotThePrintFlare`, `LoupeFlareChangeKeepsTheCanvasesThatDoNotReadIt`, the reversal lab files in the ramp, chart, and image cases (10 images), the ROI case on `kodak_5207` and `fuji_velvia_50` at the odd and the even row, and the cost record on `kodak_5207`, `fuji_velvia_50`, and `kodak_ektachrome_e100`. `ReversalFilmStopsWithTheNamedErrorUntilMetalHasTheLightTablePasses` is removed.

**Tests.** Apple M4 (8-core GPU), macOS, Debug host build (`build/macos-debug`), with the resources of schema 6 and the lab values of the F8 record. This build also compiled the size checks of F8 (`FilmKernelParams` of 412 bytes) for the first time.

| Check | Result |
| --- | --- |
| `GpuDagMetalDrtTest`, `MetalFilmPrintFixture` | 21 of 21 pass, with the limits of CUDA |
| Ramp against the lab, 13 selections (8 of the negatives, 5 of the reversal films) | largest 0.0005 codes (limit 0.5) |
| Chart against the reference | largest 0.027 codes (limit 1); Velvia 50 0.027, Provia 100F 0.021, E100 0.020 |
| 6000 x 4006 image against the lab, 10 images | p99.9 up to 0.57, largest up to 1.73 codes (limits 1 and 4); Velvia 50 0.548 / 1.725, Velvia 100 0.541 / 1.656, Provia 100F 0.442 / 1.255, Astia 100F 0.510 / 1.572, E100 0.426 / 1.116 |
| Viewport ROI inside the reach of the blurs, `kodak_5207` odd / even row | 0.037 / 0.005 codes (limit 0.1) |
| Viewport ROI inside the reach of the blurs, `fuji_velvia_50` odd / even row | 0.133 / 0.019 codes (limit 0.1 at the even row; the odd row is the grid phase of the F8 record and #636) |
| `ReversalFilmPublishesTheLoupeFlareResultAndNotThePrintFlare`, `LoupeFlareChangeKeepsTheCanvasesThatDoNotReadIt` | pass |
| Negative and print cases of F7 | pass, with the values of F7 |
| Other cases of `GpuDagMetalDrtTest` | 14 of 14 pass |

**Cost.** Wall time in ms to the end of the GPU work after a slider change, median of 9 renders, on the Apple M4 (`DISABLED_DrtCostRecordAtEditorSizes`, Debug host build, as in F7). Each cell is Halation change / Light spread change.

| Long edge | Print form, `kodak_5207` | Light table, Velvia 50 | Light table, E100 |
| --- | --- | --- | --- |
| 2560 | 14.5 / 17.1 | 16.4 / 19.1 | 11.3 / 14.0 |
| 4096 | 35.4 / 38.9 | 39.1 / 42.7 | 25.4 / 28.8 |
| 6000 | 62.5 / 67.4 | 64.5 / 69.8 | 62.2 / 67.4 |

- The light table form costs at most 3.8 ms more than the print form at the same size. The display light canvas computes FP6 to the display matrix for each source pixel.
- E100 has no inhibition, so it has no inhibitor blur. At 2560 and 4096 it costs less than the print form. At 6000 the inhibitor of the print form runs at half size, so the two costs are almost the same.

**Not verified.** A Release build, the packaged application, and a render in the running application on macOS. The user review of the five films. The full test suite did not run.

### F10: Negative Scan, phase 1: model layer

1. **Objective.** Print material Negative Scan of issue #661 (lab step P14) in the model layer: the resources of schema 10, the host tables and values, the CPU form of the point math and of rule `frame_auto_colour`, the document keys, the panel, the history, and the scene light As Shot. The GPU passes are phase 2 (issue #664, CUDA and OpenCL) and F11 (issue #662, Metal).
2. **Inputs.** F8; resources of schema 10 from the lab (`build_product_resources.py`, lab commit fb785ef, results in 8c3a4fe), and the lab values of the same commit (`build_display_expected.py`, `build_image_expected.py`), written from a clean worktree of that commit.
3. **Modules.** `include/edit/graph/film_print_model.hpp`, `edit/graph/drt_node_model.cpp`, `edit/runtime/film/` (`film_stock_bundle`, `film_print_tables`, the new `film_negative_scan`), `film_print_math.h`, `film_print_kernel_tables.hpp`, the GPU passes (named stops), `app/editor_parameter_catalog.cpp`, `app/editor_parameter_write_parse.cpp`, `app/editor_pipeline_command_service.cpp`, `app/pipeline_history_applier.cpp`, the album backend (panel projection, history mutation, commit summary), `EditorDisplayTransformPanel.qml`, and the Chinese translation.
4. **Data rules.** Section 3.4. FP1 to FP5 do not change. The print and light table forms do not change.
5. **Steps.** (a) Read schema 10. (b) The scanner weights and table, the stored values of a selection, and the derivation for another scanner. (c) The render terms, the prescan, and rule `frame_auto_colour` on the host. (d) The document keys and the panel. (e) The history fault. (f) Named stops in the GPU passes. (g) Tests.
6. **Success chain.** A Negative Scan selection resolves its scanner table and values on the host; each setting writes its key, the panel shows the value of each document, and undo and redo set every setting.
7. **Failure chain.** A missing or wrong resource array stops `FilmStockBundle::Open` with its name. A render of Negative Scan stops with a named error on every backend until its GPU pass exists. There is no change of backend and no change of form.
8. **Tests.** Section "Tests" of the record below.
9. **Exit criteria.** The host values against the lab to 1e-9; the CPU point form against the lab render (ramp 0.005 stops, chart 0.01 stops above 2^-10, auto off and Neutral); the history and the panel faults proved by a test that fails without the fix.
10. **Expected diff.** 2500 lines.
11. **Completion record.** See below.

#### F10 phase 1 completion record (2026-11-05)

The first pull request of this phase had an ADX scan named Lab Scan (PR #663, commits 5cdc75af9 and 197326900). The user then took method `negative_scan` of lab step P14 as the final form (lab ROADMAP item 10), and issue #661 was written again for it. This record replaces the record of the ADX form. Scene light As Shot, the picker that stays open, and the `DrtDisplayPixel` refactors of the first commits stay.

**Call chain.**

- Document: `DrtParameterUpdate` -> `DrtParamsModel::ApplyUpdate` (`RequireValidFilmPrintUpdate`; a scan setting marks `DrtDirty::FilmScan`) -> `FilmPrintToJson`, which omits a key at its default; load: `ReadFilmPrint` -> `ReadFilmNegativeScan`.
- Panel: `EditorDisplayTransformPanel.qml` `buildOdtParams` -> `EditorParameterCatalog::ToModelJson` (`OdtConstrain`: a write that selects Negative Scan selects As Shot in the same write) -> `ParseEditorParameterWrite` -> the editor session. Echo: `ProjectCurrentPanelFields` -> `PanelProjectionToVariantMap` (every Negative Scan key, also at its default) -> `loadFromSnapshot`.
- History: `ApplySetParameter` -> `ApplyEditorParameterValue` -> `ParseEditorParameterValue` (form Value: `CompleteFilmPrintUpdate` reads each missing film print key as its default) -> `ApplyEditorParameterWrite`. The album backend (`RestoreDocumentFields`, `ApplyCommitToLiveDocument`) calls the same function.
- Host: `FilmPrintTables::Resolve(negative, negative_scan, mix)` -> `ResolveNegativeScanLocked` (default scanner) -> `FilmStockBundle::NegativeScanSelection`, `film_negative_scan::MakeScannerWeights`, `ScannerTableOf` -> `film_tables::Log10BandResponse`, and `StoredSelectionValues`. Another scanner: `FilmPrintTables::ResolveNegativeScan(negative, mix, scanner)` -> `DeriveMediumGamma` (the scanner tables of the pool, kept by content key) and `DeriveSelectionValues` (`GreyDensity` -> `PointRecord`, `ReadScannerTable`).
- Render values: `MakeRenderTerms(values, params, auto_shift)` -> `FilmKernelNegativeScanTerms`, and per pixel `FilmKernelNegativeScanPixel`; prescan: `MakePrescanTerms` -> `FilmKernelNegativeScanPrescanX`, `ReduceFrame`, `AutoColourShift`.
- GPU: `RecordCudaFilmPrint`, `RecordOpenClFilmPrint`, `RecordMetalFilmPrint` -> `IsFilmNegativeScan` -> a named error (issue 664, issue 662).

**Changes from the plan, and details that the plan does not state.**

- Two faults of the issue are fixed. (1) The panel projection had no Negative Scan key, so the panel kept the values of the last document that had them, and the next write of any control (for example Halation) wrote them into this document. (2) A head move applied the stored value as a patch; the Model omits a key at its default (and the film print object for another method), so undo and redo did not set those settings. The tests `NegativeScanSettingsFollowTheProjectionOfEachDocument` and `NegativeScanChangesUndoAndRedoToEachStoredValue` fail with the old code (each was run once with the old code).
- `FilmKernelParams` is 416 bytes again: the 412 bytes of main and `scan`. The values of the Negative Scan form are per render (`FilmKernelNegativeScan`, 48 bytes), because the shift has the auto colour of the frame.
- `film_print_math.h` has a guard per precision, so a test can have the float32 and the double form in one file.
- `DeriveMediumGamma` stops with the name of the medium when a gamma is not above 0, as the bundle does for a stored gamma.
- The default of Auto Color Strength in the panel is the decimal 0.8, as Enlarger Flare has 0.01, because the float 0.8 is 0.800000011920929.
- `kFilmResourceSchemaVersion` is 10. The ADX arrays, the printing paper, the LAD aim, `ExecutionPlan::film_scan_scene_output`, and the program `film_print_scan.cl` are removed, and so is the aces-dev notice of `THIRD_PARTY_NOTICE.txt`.
- The resources of schema 10 do not change the print and light table forms: their tests pass with the same results.

**Tests.** Debug host build, NVIDIA GeForce RTX 3080 Laptop GPU, resources of schema 10 and lab values of lab commit fb785ef.

| Check | Result |
| --- | --- |
| `FilmPrintTablesTest` | 65 of 65 pass |
| Scanner table against the lab, 4 selections | largest relative difference 7.6e-15; scanner weights 1.9e-16 |
| Negative Scan setup against the lab arrays, 2970 selections (30 negatives, 99 scene lights) | difference 0 |
| As Shot blend against the lab blend, 5207 at 3100 K and 5219 at 5650 K | difference 0 |
| Host derivation on the default scanner against the stored values | gamma 6.7e-16; D_base, D_aim, light axis 1.7e-15 |
| Second scanner (630/530/460 nm, 60 nm, equal energy) against the lab | weights 0; gamma 3.3e-16; D_base, D_aim, light axis 9.2e-16 |
| Rule `frame_auto_colour`, two frames of 257 x 391, 10 cases (both branches of `smaller_cast`) | prescan 0, shift 4.9e-15 |
| CPU point form against the lab render, 32 files (4 negatives, reference / daylight / 3100 K / 5650 K, auto off and Neutral) | ramp at most 1.5e-5 stops (limit 0.005), chart at most 1.2e-4 stops (limit 0.01); prescan x 1.9e-6, shift 4.7e-7 |
| `GpuDagModelGraphTest` / `GpuDagRawInputTest` (film print) / `EditorParameterCatalogTest` | 173 / 11 / 38 pass |
| `PipelineHistoryApplierTest` / `EditorPanelProjectionTest` / `EditorPipelineCommandServiceTest` | 13 / 11 / 24 pass |
| `EditorDisplayTransformSnapshotQmlTest` | 25 of 25 pass |
| `CudaFilmPrintFixture` / `OpenClFilmPrintFixture` (prints, reversal films, the named stop) | 20 of 20 / 22 of 22 pass; image p99.9 at most 0.57 codes |
| `EditorSessionHistoryPortTest` | 118 of 119; `CheckoutAndPasteHashesAreUnchanged` fails as on main (issue #555; its paste payload has only color grade changes) |

**Not verified.** Metal (the sources with the new size checks and the named stop compile only on a Mac). A render of Negative Scan, which needs phase 2. The application with the panel on screen. An AMD GPU. The full test suite did not run.

### F10: Negative Scan, phase 2: CUDA and OpenCL passes

1. **Objective.** The GPU passes of Negative Scan (section 3.4) on CUDA and OpenCL, with rule `frame_auto_colour` on the GPU, and no named stop. Metal is F11 (issue #662).
2. **Inputs.** F10 phase 1; the resources of schema 10 and the lab values of lab commit fb785ef.
3. **Modules.** `cuda_film_print_pass.cu`, `opencl_film_print_pass.cpp`, `film_print.cl`, the new `film_print_scan.cl` (DRT program), `film_negative_scan` (prescan windows and plan, auto colour of a density prescan), `film_print_plan.hpp` (result id), `runtime_invalidation.cpp`, `drt_node_model` (dirty field `FilmScanner`), `gpu_node_pass_stats.hpp`, `execution_plan.hpp` (tests-only flag).
4. **Tests.** Section "Tests" of the record below.
5. **Tracking.** [#664](https://github.com/zidage/AlcedoStudio/issues/664).

#### F10 phase 2 completion record (2026-11-05)

**Call chain.**

- CUDA: `RecordCudaFilmPrint` -> `FilmPrintTables::ResolveNegativeScan(negative, light, ScannerOf(scan))` -> FP1 (`ExposureKernel`, no entry gamut compression) to FP5 as the print form -> with auto colour: `ReadOrBuildCanvasResult(FilmScanPrescanResultId)` -> `ScanPrescanRowsKernel`, `ScanPrescanColumnsKernel` -> a read of the prescan -> `film_negative_scan::AutoColourOfDensityPrescan` -> `MakeRenderTerms` -> `ScanDisplayKernel`: `FilmKernelReadRecordTable` -> `FilmKernelNegativeScanPixel` -> `CUDA::DrtDisplayPixel`.
- OpenCL: `RecordOpenClFilmPrint` -> the same host steps -> `film_print_scan_prescan_rows`, `film_print_scan_prescan_columns` (film print program) -> `clEnqueueReadImage` of the prescan -> `film_print_scan_display` (DRT program, `film_print_scan.cl`) -> `DrtDisplayPixel` of drt.cl.
- Invalidation: `CollectDrtChanges` invalidates `film.scan_prescan.0` on `FilmExposure`, `FilmHalation`, and `FilmScanner`; `FilmScan` (the rendering settings) invalidates only the display.

**Decisions.**

- The GPU reduces the scan density T, not x. x of the prescan is `slope T + offset` (`MakePrescanTerms`), and the area mean of an affine map is the map of the mean, so the host maps the mean in double precision. The prescan then does not read the contrast, and a rendering setting reads the kept prescan.
- The cast estimate runs on the host after a read of the prescan only (96 x 64 texels at 6000 x 4006), with the CPU rule of phase 1 (`AutoColourShift`). The read waits for FP1 to FP5 of the render. Auto colour off builds no prescan, reads nothing, and computes the terms before any GPU work, so a black at or above the grey (issue #666) stops before the GPU work.
- The prescan is a canonical result, `film.scan_prescan.0`, with the rule of the other film print results: a full-edit render builds and publishes it, a viewport ROI samples it, an ROI with no valid published prescan builds a temporary one from its own pixels. Its shape is `PrescanShape` of the edit frame, and the windows are those of the pixels of the render that builds it (`film_negative_scan::PrescanPlanOf`), so a preview and a full-size render name the same result. The editor renders the full frame before the detail patch, so a zoom has the shift of the full frame.
- The window form: Pillow BOX has weight 1 or 0 per input pixel, so each output pixel of a pass is the mean of a window of adjacent pixels (`PrescanAxisOf`). Each pass stores float32, as Pillow. The horizontal pass gives one warp (CUDA) or one work group of 32 (OpenCL) to each window, so the reads of the group are adjacent; this took the pass from 5.9 ms to 3.2 ms at 6000 on CUDA.
- The scanner settings have their own dirty field `DrtDirty::FilmScanner`, because the prescan reads them and the rendering settings do not.
- The test of linear AP1 before the display transform uses `ExecutionPlan::film_scan_linear_output` (tests only; a render never sets it). `GpuNodePassStats` has `film_scan_prescan_reads` and `film_scan_auto_shift` for the tests.
- The GPU tests measure "a rendering setting builds no table" by the LUT upload bytes of the device: the film print tables of the pass live in the GPU library, and the test executable has its own copy of `SharedFilmPrintTables`, so its `Stats` read another object.

**Tests.** Debug host build, NVIDIA GeForce RTX 3080 Laptop GPU, resources of schema 10, lab values of lab commit fb785ef.

| Check | Result |
| --- | --- |
| Ramp, linear AP1 against the lab, 32 files (4 negatives, reference / daylight / 3100 K / 5650 K, auto off and Neutral) | CUDA at most 2.0e-5 stops, OpenCL 2.0e-5 (limit 0.005); shift at most 5.8e-7 (limit 1e-4) |
| Chart, the same 32 files | CUDA at most 0.0021 stops, OpenCL below it (5219 at most 0.00075) (limit 0.01), with the relative floor of issue #667; with no relative floor CUDA 0.045 at one value far outside AP1 (2 files), OpenCL 0.001 |
| Image 6000 x 4006, 32 files | p99.9 0.0148 to 0.0197 stops (limit 0.02); largest 0.080 to 0.147 stops (limit 0.1, not met in 14 files: issue #668; the test has 0.2); shift 3.6e-6 to 2.6e-4 (limit 1e-4, not met in 11 of 16: issue #668; the test has 5e-4); OpenCL gives the same values to 1e-5 |
| Viewport ROI inside the reach of the blurs, `kodak_5207`, Neutral | odd row 7.3e-4, even row 1.4e-5 stops (limit 0.002); the shift equals that of the full frame |
| Display: Negative Scan against method OpenDRT on its linear AP1, SDR and HDR, auto off and Neutral | 0 codes on CUDA and OpenCL (limit 0.1) |
| OpenCL against CUDA (`NegativeScanFilmPrintMatchesCudaReferenceWithinTolerance`, 5 cases with As Shot, another scanner, HDR) | largest 0.30 codes (limit 1) |
| Prescan window form against `ReduceFrame`, 7 frame shapes | largest 2.4e-7 |
| `CudaFilmPrintFixture` / `OpenClFilmPrintFixture` | 25 of 25 / 27 of 27 pass; print, reversal, and OpenDRT cases unchanged |
| `FilmPrintTablesTest` / `GpuDagModelGraphTest` / `GpuDagRawInputTest` (invalidation, film print) / `GpuDagOpenClDrtProductTest` (film print) | 67 / 173 / 45 / 3 pass |

**Cost.** Release build, RTX 3080 Laptop. CUDA: median GPU time after a change; OpenCL: median wall time to the end of the GPU work. Halation change (the prescan is built again) / Scan Contrast change (the prescan is kept). In ms:

| Long edge | Print form CUDA / OpenCL | Negative Scan auto off, CUDA / OpenCL | Negative Scan Neutral, CUDA / OpenCL | Prescan pass + read and host rule, CUDA |
| --- | --- | --- | --- | --- |
| 2560 | 4.2 / 4.5 | 3.8 / 4.9 (Scan Contrast 3.7 / 4.9) | 4.9 / 6.3 (Scan Contrast 4.6 / 5.7) | 1.4 + 0.9 |
| 4096 | 12.0 / 11.9 | 11.5 / 13.7 (11.5 / 13.8) | 13.2 / 15.9 (12.2 / 14.7) | 3.1 + 0.8 |
| 6000 | 17.8 / 16.7 | 16.9 / 20.8 (16.8 / 20.9) | 19.1 / 23.6 (17.9 / 21.9) | 3.2 + 0.8 |

- On CUDA auto off costs less than the print form at each size. On OpenCL it costs more (4.1 ms at 6000): the OpenDRT call of the scan display kernel costs 5.4 ms at 24 MP on OpenCL, and method OpenDRT on OpenCL has the same cost (issue #669).
- With Neutral the read of the prescan and the host rule cost about 0.8 ms; a rendering setting keeps the prescan, so it costs only the read.

**AMD.** `opencl_dag_film_print` and `opencl_dag_drt` build with no error on drivers 26.10.2, 26.6.2, and 26.6.1, on gfx1030, gfx1100, gfx1103, and gfx1201, and on gfx1010 with the legacy runtime.

**Not verified.** Metal (F11, issue #662; see its record). A render in the running application. An AMD GPU. The packaged application. The full test suite did not run.

### F11: Negative Scan on Metal

1. **Objective.** The Negative Scan form of F10 phase 2 (section 3.4) on Metal, with rule `frame_auto_colour` on the GPU, and no named stop.
2. **Inputs.** F10 phase 2; the resources of schema 10 and the lab values of lab commit fb785ef; a macOS machine.
3. **Modules.** `metal_film_print_pass.mm`, `film_print.metal` (FP1 with no entry compression, the two prescan passes), the new `film_print_scan.metal` (included by `drt.metal`, so in the DRT AIR file), `drt.metal` (`DrtDisplayPixel`), `film_print_math.h` (`FILM_KERNEL_PRIVATE`), `metal_film_print_test.cpp`.
4. **Tests.** Section "Tests" of the record below.
5. **Tracking.** [#662](https://github.com/zidage/AlcedoStudio/issues/662).

#### F11 completion record (2026-11-05)

**Call chain.** `MetalDrtOps::DispatchFilmPrint -> RecordMetalFilmPrint -> FilmPrintTables::ResolveNegativeScan(negative, light, ScannerOf(scan))` -> FP1 (`film_print_exposure`, no entry gamut compression when `params.scan`) to FP5 as the print form -> with auto colour: `ReadOrBuildCanvasResult(FilmScanPrescanResultId)` -> `film_print_scan_prescan_rows`, `film_print_scan_prescan_columns` -> `MetalBackend::DownloadTexture2D` of the prescan -> `film_negative_scan::AutoColourOfDensityPrescan` -> `MakeRenderTerms` -> `film_print_scan_display`: `FilmKernelReadRecordTable` -> `FilmKernelNegativeScanPixel` -> `DrtDisplayPixel` of `drt.metal`.

**Decisions.**

- The form of F10 phase 2: the GPU reduces the scan density T to the prescan, a canonical result; the host reads only the prescan (96 x 64 texels at 6000 x 4006) and computes the shift; auto colour off builds no prescan, reads nothing, and computes the terms before any GPU work.
- The read of the prescan is `DownloadTexture2D`: it commits the recorded work of the frame (FP1 to FP5 and the prescan) and waits for it. The display pass then records into a new command buffer of the same render. So with auto colour on, the GPU time that a preview request records (`BeginGpuWorkSample`) covers only the work after the read.
- The horizontal prescan pass gives one SIMD group to each window and adds the sums of its lanes with `simd_sum`; the host dispatches threadgroups of 8 SIMD groups (`Dispatch::RunSimdGroups`). The middle image and the prescan stay float32, as each pass of the lab.
- The display kernel `film_print_scan_display` is in `film_print_scan.metal`, which `drt.metal` includes, so it calls `DrtDisplayPixel`, the function that `drt_display` now calls too. The point math stays in `film_print_math.h`. The DRT values (`MetalDrtGpuParams`, above the 4 KB of `setBytes`) go to the transient arena; the scan terms (48 bytes) are `setBytes`. The kernel reads the alpha of the scene only when FP1 marked an alpha other than 1, as `film_print_display` does.
- `ReadOrBuildCanvasResult` returns the `Texture2D` of the result, so the read can take it.
- Fault of F10 phase 1, found because Metal compiled the header for the first time: `FilmKernelNegativeScanTerms` (host only) had pointers with no address space, which Metal does not allow. Its pointers now have `FILM_KERNEL_PRIVATE` (`thread` on Metal, `__private` on OpenCL, empty on the host and CUDA).
- Metal against CUDA was measured once with a temporary test that wrote the CUDA output to a file and read it on the Mac. That test is not in the repository, because this project keeps no stored render pixels as a test.

**Tests.** Apple M4 (8-core GPU), macOS, Debug host build (`build/macos-debug`), resources of schema 10, lab values of lab commit fb785ef.

| Check | Result |
| --- | --- |
| `GpuDagMetalDrtTest`, `MetalFilmPrintFixture` | 27 of 28 pass; `FilmPrintImageMatchesLab` ran in two halves for the free disk space of the Mac, so each half fails only its file count (26 of 42); every image holds its limits |
| Ramp, linear AP1 against the lab, 32 files | at most 1.7e-5 stops (limit 0.005); shift at most 5.9e-7 (limit 1e-4) |
| Chart, the same 32 files | at most 0.0010 stops (limit 0.01), with the relative floor of issue #667; with no relative floor 0.022 at the value of #667 (`kodak_5219` at 3100 K; CUDA 0.045) |
| Image 6000 x 4006, 32 files | p99.9 0.0149 to 0.0197 stops (limit 0.02); largest 0.080 to 0.148 stops (above 0.1 in 14 files, issue #668; the test has 0.2); shift 3.6e-6 to 2.7e-4 (above 1e-4 in 11 of 16, issue #668; the test has 5e-4). These are the values of CUDA |
| Viewport ROI inside the reach of the blurs, `kodak_5207`, Neutral | odd row 7.9e-4, even row 6.4e-5 stops (limit 0.002); the shift equals that of the full frame |
| Display: Negative Scan against method OpenDRT on its linear AP1, SDR and HDR, auto off and Neutral | 0 codes (limit 0.1) |
| Metal against CUDA, 1024 x 768 field, `kodak_5207`, `kodak_portra_400`, `kodak_ektar_100`, `kodak_5219`; auto off, Neutral under daylight, As Shot 5650 K with another scanner, Rec. 2020 PQ 1000 nits | largest 0.60 codes (`kodak_ektar_100`, Neutral, daylight); the other 15 cases 0.010 to 0.040 codes (limit 1) |
| Print and reversal cases | ramp at most 0.0005 codes, chart at most 0.029 codes, 10 images p99.9 up to 0.57 and largest up to 1.73 codes, ROI `kodak_5207` 0.034 / 0.0024 and `fuji_velvia_50` 0.137 / 0.042 codes (odd / even row): within the limits of F9 |
| Other cases of `GpuDagMetalDrtTest` | 14 of 14 pass |
| Windows, after the change of `film_print_math.h`: `CudaFilmPrintFixture` / `OpenClFilmPrintFixture` (with no lab image cases) / `FilmPrintTablesTest` | 22 / 24 / 67 pass |
| AMD: `opencl_dag_film_print`, `opencl_dag_drt` | build with no error on gfx1030, gfx1100, gfx1103, gfx1201, and gfx1010 (legacy runtime) |

**Cost.** Wall time in ms to the end of the GPU work after a change, median of 9 renders, Apple M4 (`DISABLED_DrtCostRecordAtEditorSizes`, Debug host build, as in F7 and F9). One run on the code of issue #672 (PR #673), which does not change the passes:

| Case | 2560 x 1709 | 4096 x 2735 | 6000 x 4006 |
| --- | --- | --- | --- |
| Print form `kodak_5207`, Halation change / Light spread change | 14.9 / 17.6 | 35.7 / 39.1 | 63.5 / 67.8 |
| Negative Scan, auto off, Halation change / Scan Contrast change | 14.9 / 15.0 | 36.9 / 36.9 | 66.1 / 66.1 |
| Negative Scan, Neutral, Halation change (prescan built again) / Scan Contrast change (prescan kept) | 16.4 / 15.9 | 39.4 / 37.4 | 70.2 / 66.8 |
| Method OpenDRT | 3.5 | 7.6 | 16.0 |

- The print form holds the values of F9 (4096: 35.4 / 38.9, 6000: 62.5 / 67.4) within 2 percent.
- With auto off, Negative Scan costs 0.0, 1.2, and 2.6 ms more than the print form after a Halation change. It has no flare canvas, and its display pass runs OpenDRT in place of the print table.
- With Neutral, the prescan, the read, and the host rule cost 1.5, 2.5, and 4.1 ms after a Halation change. When the prescan is kept, the read costs 0.9, 0.5, and 0.7 ms.
- The first run at 4096 and 6000 (2026-11-05) gave up to 2 times these values for the unchanged print form, on a Mac with other load. It is not recorded.

**Not verified.** A Release build, the packaged application, and a render in the running application on macOS. The full test suite did not run.

### Issue #672: default scene light As Shot, and remembered film print settings

User request of 2026-11-05, stacked on F11 (PR #671).

- **Default scene light.** `FilmPrintParams::scene_light` is `as_shot` (`kFilmAsShotLightId`) for every form: print, colour reversal film, and Negative Scan. `OdtUiDefault`, `CompleteFilmPrintUpdate`, and `ReadFilmPrint` read the default from the struct, so they follow. A document does not change: `FilmPrintToJson` always writes `scene_light` (since F2), and a document of another method writes no `film_print`. The rule of `OdtConstrain` stays: a write that selects Negative Scan on a print under another light selects As Shot. The scanner light (`halogen`) is a setting of the scanner, not a scene light, and does not change.
- **As Shot only with a RAW white balance** (user request of the same day). `RawWhiteBalanceCct` (`develop_color_transform`) is the temperature of the RAW white balance: none for a raster input and when `ResolveDevelopColorTransform` fails for the Develop payload (no usable camera matrices or as-shot neutral). Before, a failed solve left the default `as_shot_cct` of 6500 K, which As Shot then rendered. `FilmSceneRawCct` reads it, so As Shot renders as the reference in both cases, as for a raster input in F10. `ReadOdt` of the panel projection sets `EditorPanelOdtValue::film_as_shot_available` (snapshot key `film_as_shot_available`, not a Model field). Without it the panel removes As Shot from the Scene Light list and shows a stored As Shot as Reference. A film print write then keeps `as_shot` (`filmSceneLightKeepsAsShot`), so the document and the remembered settings keep As Shot for a RAW input; a scene light that the user picks ends this.
- **Remembered film print settings.** The owner is `EditorBehaviorPreferences` (`appModules.editorBehavior`), QSettings key `editor/filmPrint/lastSettings`: the JSON text of the `film_print` object in the form of `DrtParamsModel::ToJson`. `rememberFilmPrint` loads the object through `DrtParamsModel::LoadJson` and rejects an object that the Model rejects; a stored value that the Model does not load reads as no value. `filmPrintUiValue` is the `odt` UI value of the catalog (`ToUiValue`).
  - Record: each ODT model of `EditorDisplayTransformPanel.qml` emits `settledWriteAccepted` (new signal of `EditorAdjustmentModelBase`, after the submitter accepted a settled user write). With method Film Print the panel passes `buildOdtParams()` to `rememberFilmPrint`. Interactive drag writes and automation writes do not reach it.
  - Apply: the method switcher calls `selectMethod`. A selection of Film Print on another method loads the remembered fields into the film print models (`loadFilmValues`, no submit) and then selects the method: one write, one history entry. A document that already has method Film Print keeps its own settings. Reset and `alcedo-cli` writes use the defaults.
- **Tests.** `GpuDagModelGraphTest` (`FilmPrintParams` default; `RawWhiteBalanceCctNeedsARawInputWithAResolvedColorTransform`), `EditorPanelProjectionTest` (`AsShotIsAvailableOnlyForARawWhiteBalanceThatResolves`), `EditorParameterCatalogTest` (UI default; Negative Scan on a print under daylight selects As Shot), `PipelineHistoryApplierTest`, `EditorNodeSelectionLayoutTest` (`EditorBehaviorPreferencesTest`: persistence, rejection, stored value that the Model does not load), `EditorDisplayTransformSnapshotQmlTest` (`SelectingFilmPrintAppliesTheFilmPrintSettingsOfTheLastSettledFilmPrintWrite`, `AsShotIsHiddenWithNoRawWhiteBalanceAndAStoredAsShotStaysUntilTheUserPicksALight`). The GPU helpers `SetRawAsShot` and `SetRawCustom` add the test camera profile, so their temperature resolves.
- **Metal.** Apple M4, Debug host build: `GpuDagMetalDrtTest` 41 of 41 pass with the full-size image case excluded, `AsShotFollowsTheColourTemperatureOfTheRawDecode` included. The 10 print images of `FilmPrintImageMatchesLab` give the same values as F11 (the Negative Scan image values were not on the Mac, so the case fails its file count).

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
- F8: [#654](https://github.com/zidage/AlcedoStudio/issues/654)
- F9: [#655](https://github.com/zidage/AlcedoStudio/issues/655)
- F10: [#661](https://github.com/zidage/AlcedoStudio/issues/661) (phase 1, model layer), [#664](https://github.com/zidage/AlcedoStudio/issues/664) (phase 2, CUDA and OpenCL); found in phase 2: [#667](https://github.com/zidage/AlcedoStudio/issues/667), [#668](https://github.com/zidage/AlcedoStudio/issues/668), [#669](https://github.com/zidage/AlcedoStudio/issues/669)
- F11: [#662](https://github.com/zidage/AlcedoStudio/issues/662)
- Default As Shot and remembered film print settings: [#672](https://github.com/zidage/AlcedoStudio/issues/672)
