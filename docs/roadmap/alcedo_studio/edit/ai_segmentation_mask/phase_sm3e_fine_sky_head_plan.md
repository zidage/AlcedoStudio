# SM3.E Sky from the Fine Sky Head — Phase Plan

Date: 2026-10-26.

Status: the Rust part (#536) and the host part (#537) are implemented and verified on Windows
(sections 9 and 10). The Core ML part (#538) is implemented and verified on macOS (section 11);
since #538 every backend has the sky head, and the MVP Sky is removed.

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), decision
D24 and the SM3.E entry of the phase summary.

Prerequisites: SM3.C ([phase plan](phase_sm3c_task_masks_plan.md)) and SM3.D
([phase plan](phase_sm3d_people_masks_from_s3od_plan.md)), merged to `main`.

Issues: #536 (Rust sidecar, this record), #537 (C++ host and the training data notice), #538
(Core ML and macOS).

Source of the model: lab issue zidage/alcedo-ai-mask-lab#10, lab PR zidage/alcedo-ai-mask-lab#11,
[lab report](https://github.com/zidage/alcedo-ai-mask-lab/blob/main/docs/exploration/sky_head_mass13k.md).

---

## 1. Decision record

### 1.1 User request (2026-10-26)

- Bring the new sky head of `alcedo-ai-mask-lab` into Alcedo Studio. One issue tracks the Rust
  side (#536); one issue tracks the C++ side (#537).
- The first step is a Windows MVP. macOS is not verified yet.

### 1.2 Inherited decisions

- Master plan D2 (model set), D5 (model licenses), D12 (the sidecar runs the models), D13 (local
  model directory until SM2), D15 (U3: the edge comes from the model output; U6 memory budget).
- Lab decisions U17 (own sky head with a fine alpha output on the SAM 2.1 features) and U18
  (MaSS13K training data; the Sky function tells the user that the training data may not permit
  commercial use; exception to U2 for Sky only).
- SM3.A A7 (one execution provider, no other device after an error).
- SM3.C C6 (one load of all models), C7 (one image entry per image, results computed once).
- #532 (DirectML memory arena: off for the encoder and S3OD, on for the decoder and the head).

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| S1 | The ONNX Runtime backend (DirectML and CPU) loads `sky_head.onnx` with the other models. A missing or damaged file is a load error, as for the other models. Sky never uses the MVP rule on this backend. | No fallback (AGENTS.md). The sky head is part of the model set. |
| S2 | The sky head runs at the first Sky request of an image, not at the session start. The image entry keeps the encoder input (`[1, 3, 1024, 1024]` float32, 12 MB) for it when the model set has a sky head. | The session start time does not change for the users who do not use Sky. The encoder input cannot be made again from the entry: the entry has no full photo (C8). |
| S3 | Sky is found when at least `SKY_FOUND_SHARE` = 5e-4 (0.05 %) of the 1024 x 1024 alpha values are >= 0.5. `presence` is the share and `presence_threshold` is 5e-4. | The head has no presence output. On the 1,500 MaSS13K test photos the rule gives the most correct decisions of the measured shares (1,453): 906 of 938 photos with sky found (MVP 789), 15 of 562 photos without sky found (MVP 11). Lab report section 10. |
| S4 | The Sky result is `ALPHA`: the 1024 x 1024 alpha of the stretched image, `mask_box` (0, 0, 1, 1), `region_count` 0, `mask_area` the count of values >= 0.5. | The head output is on the stretched encoder input. The host maps each axis with its own scale; the existing alpha grid path of the host accepts a grid of any size with a box (#537 checks the upscale rule). |
| S5 | A backend without a sky head (Core ML, `load_sky_head` returns `None`) keeps the MVP Sky and the `BINARY` result until #538. **Replaced by S11 (#538).** | #536 scope: Windows MVP; the macOS build and tests stay green. |
| S11 | (#538) The Core ML backend loads `sky_head.mlmodelc` (float32, `ComputeUnits::All`; `CpuOnly` on the `cpu` device). `load_sky_head` returns the head on every backend, and the MVP Sky is removed: the engine region rule, the R8 union, the proto `BINARY` mask kind and the fields `mask_r8` and `region_count` (reserved), the client and host R8 output (`AiMaskBinaryR8`), and the MVP Sky port reference. | Section 11.2: float32 differs from ONNX Runtime CPU by at most 1.1e-3 on 54 photos, float16 by up to 0.24. A dead R8 path in the protocol and the host would be a second Sky rule that no backend makes. |
| S6 | The model set id stays `alcedo-ai-mask-v1`. | The model set is not published yet (SM2, SM10). |
| S7 | The DirectML session of the sky head switches off the memory arena (`ep.dml.disable_memory_arena`), as the encoder and S3OD do (#532). | With the arena, the session keeps 1,150 MB of dedicated GPU memory after its first run; without it, 122 MB. A run takes 50 ms instead of 30 ms, once per image (section 9.3). |
| S8 | The license of the sky head is recorded in master plan D5 and section 8.2.1. The user-facing notice is part of #537. | U18. No model directory license file exists yet for any model; SM2 makes the published directory. |
| S9 | The host upscales the Sky alpha of the sky head with the joint bilateral upsampling of D23 and a gain of 1 (`AiMaskAlphaGain::kOne`). Subject, Background, People, and Person keep the D23 gain of half the larger upscale factor. | The D23 gain was made for the almost binary S3OD alpha. At a 4096 x 3072 session image it is 2, so it moves the partial sky alpha at twigs to 0 or 1. Measured on 120 MaSS13K test photos (section 10.2): gain 1 has the lowest error on the partial alpha (0.200 against 0.258) and keeps 81 % of the partial target pixels partial (D23: 42 %); the total error and the edge error are equal within 1 %. |
| S10 | The notice of U18 is a static caption with an info icon under the four task buttons of the AI Mask panel: "Sky: the training data of the Sky model may not permit commercial use. Do not use Sky for commercial work." It is always shown while the panel is open. The Chinese text is a manual entry in `alcedo_main_zh_CN.ts`. | The user must read it before the first Sky run (#537), not only in a tooltip. A static caption next to the Sky button needs no state. |

### 1.4 Rejected options

- Run the sky head at the session start (with the people and sky head). It costs about 50 ms on
  every start on DirectML, also for users who never select Sky.
- Keep the DirectML memory arena of the sky head (20 ms faster per run, 1 GB more GPU memory; S7).
- Keep the people and sky head sky presence as the "found" rule. It misses 149 of the 938 test
  photos with sky (lab report section 5).

---

## 2. Product and data specification

### 2.1 Model

| Item | Value |
| --- | --- |
| File | `sky_head.onnx`, opset 17, float32, 5,821,904 bytes, SHA-256 `a6c555e7dfdbb236d6fa11569b546eab0da9f3945f6462872de0866f675a4788` |
| Inputs | `image_embed` [1, 256, 64, 64], `high_res_feats_0` [1, 32, 256, 256], `high_res_feats_1` [1, 64, 128, 128] (the encoder outputs), `pixel_values` [1, 3, 1024, 1024] (the encoder input) |
| Output | `sky_alpha` [1, 1, 1024, 1024] in [0, 1] |

### 2.2 Sky task with the sky head

1. `run_task(SKY)` reads the `sky` result slot of the image entry. When the slot is empty, the
   engine runs the sky head once on the stored features and the stored encoder input, and stores
   the result. A failure stores nothing; the next request runs the head again.
2. Share = count(alpha >= 0.5) / 1,048,576. Share < 5e-4: `found = false`, the message
   `No sky found: the sky alpha reaches 0.5 on X % of the image, less than 0.050 %.`.
3. Else: `ALPHA` with all 1,048,576 values, `mask_box` (0, 0, 1, 1).

### 2.3 Errors

| Case | Result |
| --- | --- |
| `sky_head.onnx` missing or not loadable at `LoadSegmentationModels` | `ModelNotReady` with the path; no model is loaded |
| Sky head run fails | `Inference` with the runtime message; the slot stays empty |

---

## 3. Scope

Included (#536): `model_set.rs`, `backend.rs`, `ort_backend.rs`, `coreml_backend.rs` (no sky head),
`image_entry.rs`, `task_masks.rs`, `engine.rs`, `test_support.rs`, `server/segmentation.rs` (tests),
`proto/segmentation.proto` (comments), `segmentation_port_check.rs`,
`scripts/ai_models/sam2/make_segmentation_port_reference.py`, this plan, the master plan, and the
lab measurement of S3.

Excluded: the host (#537), Core ML (#538), a presence output trained into the head, the COCO
matting labels of U17.

Included (#537): `sidecar_client/dto/segmentation.hpp` (comments), `ai_mask_coverage_composition`
(`AiMaskAlphaGain`), `ai_mask_model_output_store` (the gain of each task), `EditorAiMaskPanel.qml`
(notice), `alcedo_main_zh_CN.ts`, the tests of section 6.3, this plan, and the master plan.
Excluded from #537: macOS, the other tasks, the prompt input modes, the SM7 misalignment notice, a
saved format change (a committed Sky Mask stays a content-addressed R8 asset).

---

## 4. Source audit (before this phase)

- `engine.rs` `compute_sky`: the people and sky head sky presence, 8-connected regions of the
  64 x 64 sky probability, 3 decoder points per region, and the R8 union at the session image size
  (SM3.C section 2.3). The decoder gives 256 x 256 logits, so the sky between branches is lost.
- `ImageEntry` kept the features, the S3OD input, the head output, and the People list, but not the
  encoder input.
- `SegmentationBackend` had four `load_*` methods; the ONNX Runtime backend kept the DirectML arena
  for the decoder and the head (#532).

---

## 5. Target architecture

| Owner | Change |
| --- | --- |
| `model_set::SKY_HEAD`, `SKY_ALPHA_SHAPE` | The file, the input and output names, and the output shape. |
| `SegmentationModelRole::SkyHead` (`"sky_head"`), `SkyHeadModel::run(features, pixel_values)`, `SegmentationBackend::load_sky_head -> Option` | The backend interface (S1, S5). |
| `OrtSkyHead` | Binds the three stored feature values and the encoder input; warm-up run at load; output shape check. |
| `ImageEntry::encoder_input` | The encoder input (S2). |
| `image_entry::SkyMask::{Alpha, RegionLogits}` in `SkyResult::mask` | The two Sky rules. |
| `task_masks::SKY_FOUND_SHARE`, `sky_share` | S3. |
| `SegmentationEngine::compute_sky_alpha`, `compute_sky_regions`, `sky_task` | S2 to S5. |

### 5.1 Primary success call chain

```text
LoadSegmentationModels -> SegmentationEngine::load -> backend.load_sky_head -> OrtSkyHead (warm-up)
StartSegmentationSession -> start_session -> sam2::preprocess_image -> encoder.encode
  -> ImageEntry::new(..., encoder_input = Some(pixels), ...)
RunSegmentationTask(SKY) -> run_task -> sky_task -> result_of(entry.sky)
  -> compute_sky_alpha -> OrtSkyHead::run(entry.features, encoder_input) -> sky_share
  -> SkyResult{mask: Alpha} -> TaskResult{Alpha{values, FULL_IMAGE}} -> response ALPHA 1024 x 1024
```

### 5.2 Primary failure call chain

```text
load_sky_head fails -> ModelNotReady -> header MODEL_NOT_READY; no models loaded
OrtSkyHead::run fails -> Inference -> header INTERNAL; entry.sky stays empty
share < SKY_FOUND_SHARE -> found = false with the message; no mask
```

---

## 6. Tests

### 6.1 Rust unit tests

| Test | Assertion |
| --- | --- |
| `engine::load_reports_sky_head_only_when_backend_has_one_and_fails_without_thresholds` | Five residencies with a sky head, four without; a failing sky head load leaves no models. |
| `engine::run_task_sky_returns_sky_head_alpha_of_whole_image_without_decoder` | 1024 x 1024 alpha equal to the head output, box (0, 0, 1, 1), `mask_area`, share, threshold, no decoder call, the head reads the encoder input of the image. |
| `engine::run_task_sky_head_not_found_below_sky_found_share` | One value below the share: not found with the message; at the share: found. |
| `engine::run_task_sky_head_runs_once_per_image_entry` | A cached key-only start returns the same mask with `model_ms` 0 and one head run. |
| `engine::sky_head_failure_reports_error_and_keeps_no_sky_result` | The failure is `Inference`; the slot stays empty; the next request runs the head again. |
| `engine::image_entry_keeps_encoder_input_only_with_sky_head` | The entry keeps `preprocess_image` of the pixels with a sky head, nothing without one. |
| `engine::run_task_sky_without_sky_head_*` (2, renamed) | The MVP Sky stays on a backend without a sky head. |
| `server::server_maps_sky_head_alpha_with_whole_image_box` | `ALPHA`, 1024 x 1024, values, box, `mask_area`, `region_count` 0. |
| `server::server_maps_task_result_and_not_found` (changed) | "Not found" with share 0 and threshold `SKY_FOUND_SHARE`. |
| `ort_backend::ort_backend_reports_missing_model_file_path` (changed) | A missing `sky_head.onnx` names the file. |

### 6.2 Port check

`segmentation_port_check_matches_reference_masks`: with a sky head, Sky is compared with the sky
head reference of `make_segmentation_port_reference.py --sky-head-onnx` (ONNX Runtime 1.31 CPU,
torchvision preprocessing): "found" equal (unless the reference share is within 1e-4 of 5e-4); IoU
of the alpha >= 0.5 at least `MIN_SKY_IOU`; largest alpha difference at most
`MAX_SKY_ALPHA_DIFFERENCE` (0.999 and 1e-3; reason in section 9.2).

### 6.3 C++ and QML tests (#537)

| Test | Assertion |
| --- | --- |
| `SegmentationClientTest.RunTaskMapsSkyHeadAlphaWithWholeImageBox` | A Sky response with `ALPHA` maps to `kAlpha` with 1024 x 1024 values, the box (0, 0, 1, 1), the share and its threshold, `region_count` 0. |
| `SegmentationClientTest.RunTaskMapsSkyRegionUnionAsBinaryMask` | A Sky response with `BINARY` maps to `kBinary` with the R8 values and the region count, as before. |
| `AiMaskEditSessionTest.SkyAlphaGainOneKeepsPartialAlphaOfThinTwig` | A twig of one grid cell with alpha 0.25, upscaled 4 x 3 with a guide: gain 1 keeps 60 to 72, the D23 gain gives at most 12. |
| `AiMaskEditSessionTest.SkyAlphaOfStretchedImageConvertsWithGainOne` | A 16 x 16 Sky alpha on the 64 x 48 session image (independent x and y scales) is stored and converts with gain 1; the same grid as Subject keeps the D23 gain; a Sky `BINARY` result stays the R8 mask. |
| Existing `AiMaskEditSessionTest` composition tests | The S3OD conversion does not change (gain 4 example, clamps, guided edge). |
| `EditorAiMaskPanelQmlTest.SkyTrainingDataNoticeIsVisibleBeforeFirstSkyRun` | The production panel without a tool shows the notice under the Sky button, inside the panel. Fails without the notice (checked: "the Sky notice is missing or hidden"). |
| `SimplifiedChineseCatalogTest.CompiledCatalogTranslatesTheSkyTrainingDataNotice` | The compiled catalog translates the notice. |

---

## 7. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab\rust\puerh_mind
cmd /c "script\cargo_msvc.cmd test --release -- --skip live_"
D:\Projects\alcedo-ai-mask-lab\build\tmp\explore\venv\Scripts\python.exe -I `
  D:\Projects\pu-erh_lab\scripts\ai_models\sam2\make_segmentation_port_reference.py `
  --onnx-dir <model directory> --photos <lab input_4096 photos> --prompts <prompts.json> `
  --lab-dir D:\Projects\alcedo-ai-mask-lab --head-onnx <model directory>\people_sky_head.onnx `
  --head-json <model directory>\people_sky_head.json --subject-onnx <model directory>\subject_masks.onnx `
  --sky-head-onnx <model directory>\sky_head.onnx --out D:\Projects\pu-erh_lab\build\tmp\sm3e\port_reference
$env:ALCEDO_SEGMENTATION_MODEL_DIR = "<model directory>"
$env:ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3e\port_reference"
$env:ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE = "directml"   # then "cpu"
cmd /c "script\cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture"
```

---

## 8. Repository rules that apply

Read `AGENTS.md` and the applicable skills again before implementation.

- No fallback: the ONNX Runtime backend never makes Sky with the MVP rule; a sky head failure
  returns the real error.
- The encoder input is owner state of the image entry, written once at its creation (S2). No
  generation or token: the entry lifetime ends with an unload or a model change.
- Problems found during the work become GitHub issues.

---

## 9. Completion record

### 9.1 #536 completion record (2026-10-26)

**Status:** the Rust part is implemented. Windows: all Rust tests pass; the port check passes on
DirectML and on the ONNX Runtime CPU device. macOS: not built and not checked (#538; the Core ML
backend returns no sky head, so the macOS Sky does not change).

**Source revision and branch:** `feature/ai-segmentation-sm3e-fine-sky-head` on `a046ddd3`
(`main`). Lab: `feature/sky-head-found-rule` of `alcedo-ai-mask-lab` on `ff2cf09` (`main`): the
"sky found" measurement (lab report section 10).

**Decisions made during the work:**

| Item | Decision | Reason |
| --- | --- | --- |
| "Sky found" share (S3) | 5e-4. | Measured on the MaSS13K test split before the implementation (lab report section 10); the issue named the rule but not the value. |
| DirectML memory arena (S7) | Off for the sky head. | Measured with onnxruntime-directml 1.24.4 on one sky head session (random inputs, 12 runs, 2 runs per setting): arena on 89 MB after session creation and 1,150 MB after the first run; arena off 89 MB and 122 MB. Run time 28 to 30 ms against 50 to 51 ms. |
| Port reference prompts | `make_segmentation_port_reference.py` read a `prompts.json` that was rebuilt from the prompts of the SM3.C port reference manifest (the lab `sm1/quality/prompts.json` is no longer on the machine). | The rebuilt file has the point sets and boxes of each photo; the script makes the stroke and the subtract prompts from them, as before. The port check compares 190 masks (SM3.D: 201, which included the 11 MVP Sky unions). |
| Port check limits | `MIN_SKY_IOU` 0.999 and `MAX_SKY_ALPHA_DIFFERENCE` 1e-3. | Measured IoU 1.0 on all 12 photos with sky on both devices; largest difference 3.1e-4 (CPU) and 3.9e-4 (DirectML). 1e-3 is the export tolerance that the user accepted for the sky head. |
| Core ML follow-up | Issue #538 opened. | S5 needs a tracked item for the Core ML model. |

**Actual changed lines:** about 1,150 in Alcedo Studio (1,039 added, 109 removed; of these, the
Rust service and its unit tests about 640, the port check 168, the Python reference 29, the
documents about 310), plus 160 in the lab without the result JSON (10,628 lines). Estimate 900 to
1,300.

**Primary call chains:** sections 5.1 and 5.2 (implemented as planned).

### 9.2 Windows verification

Machine: NVIDIA GeForce RTX 3080 Laptop GPU, release build. Models:
`D:/Download/alcedo_models/alcedo-ai-mask-v1` with `sky_head.onnx` (SHA-256 `a6c555e7…4788`).
Reference: `build/tmp/sm3e/port_reference` (14 photos, ONNX Runtime 1.31 CPU).

| Test | Result |
| --- | --- |
| `cargo test --release -- --skip live_` | 378 + 4 + 5 + 1 passed, 0 failed |
| New and changed tests of section 6.1 | all passed |
| `cargo clippy --release --all-targets` | 70 warnings, none in the changed segmentation code (the count of SM3.D) |
| Port check, DirectML, arena off (final) | passed: 190 prompt masks, minimum photo IoU 0.999973; 12 sky alphas, grid IoU 1.000000, max alpha difference 3.9e-4; 2 photos without sky (`DSC_1188`, `DSC_1535`) not found, as in the reference; People, Person, Subject, and Background as before |
| Port check, ONNX Runtime CPU | passed: 12 sky alphas, grid IoU 1.000000, max alpha difference 3.1e-4 |

### 9.3 Measurements (Windows, DirectML)

| Item | Value |
| --- | ---: |
| Sky head load with the warm-up run | 400 ms (arena on: 790 ms) |
| First Sky request of a photo (sky head run and share), port check p50 | 54 ms (arena on: 32 ms); MVP Sky was the decoder runs per region |
| Session start with pixels, port check p50 | 176 ms (no change: the sky head does not run at the start) |
| Dedicated GPU memory of the port check process, 1 sample per 1.3 s | arena off: peak 1,973 MB, 951 MB between model runs; arena on: peak 3,001 MB, 1,979 MB between model runs |
| Host memory per image entry | +12 MB (the encoder input) |

### 9.4 Issues

- #537: host coverage of the sky alpha and the training data notice (open).
- #538: the sky head on Core ML (opened by this phase).

---

## 10. Completion record of #537

### 10.1 Summary (2026-10-26)

**Status:** implemented and verified on Windows. macOS: not built and not checked (#538).

**Source revision and branch:** `feature/ai-segmentation-sm3e-sky-alpha-coverage`, stacked on
`feature/ai-segmentation-sm3e-fine-sky-head` (#539) at `e364f190`.

**Decisions made during the work:** S9 (the Sky upscale rule, section 10.2) and S10 (the notice).
Other details:

| Item | Decision | Reason |
| --- | --- | --- |
| Where the gain rule lives | A field `AiMaskAlphaGrid::gain` (`AiMaskAlphaGain`), set by `AiMaskModelOutputStore::StoreTaskResult` from the task: Sky `kOne`, the other tasks and Person `kHalfUpscaleFactor` (the default). | The model output store owns the outputs and knows the task; `ConvertModelOutputToCoverage` stays a pure function of its input. |
| Client | No code change: `RunTask` maps `ALPHA` and `BINARY` for every task. Only the DTO comments change. | The mapping was already generic. |
| Model directory check | No change. | The host does not check the files of the model set directory; the sidecar does (#536). |
| Translation of the other AI Mask panel texts | Not part of #537. | The panel has no Chinese context yet; #537 asks only for the notice. |

**Actual changed lines:** about 530 added and 29 removed in code and tests (product code 108, tests
about 450), plus the documents. Estimate: no separate estimate in this plan.

**Primary call chain:**

```text
EditorAiMaskTool -> AiMaskEditSession::AddTask(Sky) -> AiMaskModelOutputStore::EnsureTask(Sky)
  -> runner.RunTask(kSky) -> GrpcSegmentationClient::RunTask -> SegmentationTaskResult{kAlpha, 1024 x 1024, box (0, 0, 1, 1)}
  -> StoreTaskResult(key, result, AiMaskAlphaGain::kOne) -> ValidateAiMaskModelOutput
  -> ComponentCoverage -> ConvertModelOutputToCoverage(AiMaskAlphaGrid{gain kOne}, session extent, guide)
     (joint bilateral upsampling, x and y scales of the stretched grid, gain 1) -> R8 coverage
```

Failure: a grid with a wrong value count or box returns `Failed` from `StoreTaskResult`, and the
session state does not change; "not found" returns `NotFound` with the sidecar message, as before.

### 10.2 Sky upscale rule (S9)

Method: the sky head (ONNX Runtime 1.24 CPU of the lab environment) on 120 MaSS13K test photos with
a sky share of at least 2 %: the 24 random and 8 worst photos of the lab sheets (including `36388`
with bare branches) and 88 random photos (seed 537). The photo (2048 x 1536) and its target alpha
were resized to a 4096 x 3072 session image (Lanczos, bilinear), so the grid is upscaled 4 x 3 and
the D23 gain is 2. The rules ran in a numpy port of `ConvertAlpha` with the guide of the session
image. "Partial" pixels have a target alpha in (0.05, 0.95); the edge band is 9 pixels around the
target boundary. Scripts and sheets: `build/tmp/sm3e/upscale/` (local; MaSS13K photos stay local).

| Rule | Mean absolute error | Error on partial pixels | Partial pixels kept partial | Error in the edge band |
| --- | ---: | ---: | ---: | ---: |
| D23 (joint bilateral, gain 2) | 7.60e-3 | 0.258 | 42 % | 0.165 |
| Joint bilateral, gain 1 (selected) | 7.65e-3 | 0.200 | 81 % | 0.164 |
| Bilinear, gain 1 | 8.43e-3 | 0.216 | 82 % | 0.180 |

Gain 1 has the lower partial-pixel error on 117 of 120 photos. On `36388` the partial-pixel error
is 0.129 against 0.195 (D23). By eye: with D23, thin twigs become sky and the sky between close
branches becomes branch; with gain 1 both stay. On hard roof lines (`18457`) D23 is a little
sharper; gain 1 is about 2 session pixels softer and keeps the sky between the tubes of a solar
water heater. Bilinear, without the guide, blurs the branches.

### 10.3 Verification

| Check | Result |
| --- | --- |
| Debug build of `AiMaskEditSessionTest`, `SegmentationClientTest`, `EditorAiMaskPanelQmlTest`, `SimplifiedChineseCatalogTest` | passed |
| `ctest -R "AiMaskEditSessionTest|SegmentationClientTest|EditorAiMaskPanelQmlTest|SimplifiedChineseCatalogTest"` | 44 of 44 passed |
| QML notice test with the panel of `HEAD` (no notice) | failed, as required |
| Release build of `alcedo_main` | passed |
| Full test suite | not run (AGENTS.md) |
| macOS | not built, not checked (#538) |

### 10.4 GUI check on Windows

Release `alcedo_main` and the release sidecar of #536, DirectML on the RTX 3080 Laptop GPU (8 GB),
UI language Chinese. Seven JPEG photos of the lab set (long edge 4096 px) and `36388` of MaSS13K (2048 x 1536): sky behind
leaves (`5761824892`), trees (`DSC00527`, `P1024439`, `36388`), poles, overhead wires, and roof
lines (`DSC00602`), a bridge (`2088064441`), and two photos without sky (`DSC_1188`, `DSC_1535`).

- The notice shows in Chinese under the task buttons when the panel opens, before Sky runs.
- Sky selects the sky between the leaves and branches; wires, poles, a mic stand, and roof lines
  stay outside the coverage.
- The photos without sky give "No sky found: the sky alpha reaches 0.5 on 0.000 % of the image,
  less than 0.050 %." and no step.

| Measurement | Value |
| --- | ---: |
| Sky step in the app (`[AI Mask] step Applied`) | 270 to 306 ms (6 photos with sky) |
| Sidecar Sky task / sky head run | 207 to 251 ms / 207 to 249 ms (port check: 54 ms; #541) |
| Model load at tool open (all five models) | 3.1 to 4.4 s; sky head 203 to 250 ms of it |
| Peak dedicated GPU memory, editor / sidecar / sum | 3,198 MB / 1,473 MB / 4,590 MB |
| Sidecar GPU memory between model runs | 923 MB |

### 10.5 Issues

- #540: `library.import` with a folder imports nothing and reports success (found while preparing
  the GUI check).
- #541: the sky head run in the editor takes about 220 ms against 54 ms in the port check.

---

## 11. Completion record of #538

### 11.1 Summary (2026-10-26)

**Status:** implemented. macOS: the Rust tests, the port check on Core ML `All`, and the C++
tests pass; Sky passes on `CpuOnly` too. Windows: the Rust tests, the port check on DirectML and on
the ONNX Runtime CPU device, and the C++ tests pass. The GUI on macOS was not run (section 11.4).

**Source revision and branch:** `feature/ai-segmentation-sm3e-coreml-sky-head`, stacked on
`feature/ai-segmentation-sm3e-sky-alpha-coverage` (#542) at `178ccab9`. Lab:
`feature/sky-head-coreml` of `alcedo-ai-mask-lab`, stacked on `feature/sky-head-found-rule`
(`279bcfd`): the Core ML conversion of the sky head and its measurement (lab report section 11).

**Decisions made during the work:**

| Item | Decision | Reason |
| --- | --- | --- |
| Precision | float32 (S11). | float16 differs from ONNX Runtime CPU by up to 0.13 (`All`) and 0.24 (`CpuOnly`), IoU down to 0.964, up to 27,042 values more than 1/255 apart (section 11.2). |
| Compute units | `ComputeUnits::All`, as the other heads and S3OD (U15); `CpuOnly` on the `cpu` device. | float32 does not run on the Neural Engine: `All` and `CpuAndGpu` take 68 ms, `CpuOnly` and `CpuAndNeuralEngine` 171 to 173 ms. |
| Encoder input | One copy of the stored 12 MB tensor into a Core ML array per sky head run; the three feature arrays are retained references (no copy). | The sky head runs once per image (S2). |
| Remove the MVP Sky | Everywhere (S11), also in the protocol and the host. | The issue asks for the removal when both backends have the head. Without a producer, the R8 path would be dead code in the proto, the client, and the coverage conversion. |
| Port check limits on Core ML | The same as on Windows (`MIN_SKY_IOU` 0.999, `MAX_SKY_ALPHA_DIFFERENCE` 1e-3). | The 14 lab photos differ by at most 4.4e-4 in the lab probe and 4.1e-4 in the port check. |
| `CpuOnly` port check | Sky passes; the S3OD failures are the accepted deviation of #477 (closed as not planned: the `cpu` device is for development and tests). | The failure list is the same as in #477. |
| Core ML model hash | The table of master plan section 8.2.1 records the directory SHA-256 of the compiled model and the SHA-256 of `model.mil` and `weights/weight.bin`. | `coremldata.bin` and `analytics/coremldata.bin` change at every compile (two conversions of the same checkpoint were compared); `model.mil` and the weights are equal. The directory SHA-256 identifies one compiled copy only. The models of the set were converted again on the Mac for this check (the earlier copies were deleted): the same sizes as in section 8.2.1, other directory SHA-256 values for the same reason. |

**Actual changed lines:** Alcedo Studio 264 added and 939 removed in code, tests, and scripts (Rust
service, server, and unit tests +168 / -601, port check and reference script +48 / -197, proto +10 /
-13, host and C++ tests +38 / -128), plus the documents; lab about 300 lines (conversion, probe,
README, report) plus the result JSON. No estimate in this plan.

**Primary call chain (macOS):**

```text
LoadSegmentationModels -> SegmentationEngine::load -> CoreMlSegmentationBackend::load_sky_head
  -> Model::load_from_url(sky_head.mlmodelc, ComputeUnits::All) -> CoreMlSkyHead::run(zero features) (warm-up)
RunSegmentationTask(SKY) -> sky_task -> compute_sky_alpha -> CoreMlSkyHead::run
  -> CoreMlImageFeatures::insert_into (3 retained arrays) + insert_tensor(pixel_values) -> Model::predict
  -> read_output(sky_alpha, [1, 1, 1024, 1024]) -> sky_share -> ALPHA 1024 x 1024, box (0, 0, 1, 1)
```

Failure: a missing or damaged `sky_head.mlmodelc` or a failed warm-up run is `ModelNotReady` with
the path, and no model is loaded; a failed run is `Inference`, and the Sky slot stays empty.

### 11.2 Core ML conversion (lab)

`convert_models.py --model sky_head` (coremltools 9.0, PyTorch 2.7.0, MLProgram, macOS 15 target)
on the run 1 checkpoint (`best.pt`, epoch 15). `probe_sky_head.py` ran ONNX Runtime 1.31 CPU
(`point_encoder.onnx`, `sky_head.onnx`) as the reference on 54 photos: the 14 lab quality photos
and 40 MaSS13K test photos (32 with at least 2 % sky, 8 without sky; seed 538). "Head": the Core ML
head on the ONNX Runtime encoder outputs; "pipeline": the Core ML encoder and the Core ML head.
Apple M4.

| Conversion, compute units | Head run, median (max) | Largest difference, head / pipeline | Smallest IoU (alpha >= 0.5) | Values > 1/255 apart, worst photo | "Found" differs |
| --- | ---: | ---: | ---: | ---: | ---: |
| float32, `ALL` (selected) | 68 ms (71) | 1.05e-3 / 1.11e-3 | 0.99969 | 0 | 0 |
| float32, `CPU_AND_GPU` | 68 ms (71) | 1.05e-3 / 1.11e-3 | 0.99969 | 0 | 0 |
| float32, `CPU_AND_NE` | 173 ms (230) | 9.5e-4 / 1.11e-3 | 0.99969 | 0 | 0 |
| float32, `CPU_ONLY` | 171 ms (295) | 9.5e-4 / 1.11e-3 | 0.99969 | 0 | 0 |
| float16, `ALL` | 29 ms (34) | 0.129 / 0.129 | 0.96867 | 27,042 | 0 |
| float16, `CPU_AND_GPU` | 51 ms (53) | 0.029 / 0.027 | 0.98986 | 1,937 | 0 |
| float16, `CPU_ONLY` | 83 ms (112) | 0.241 / 0.238 | 0.96401 | 9,671 | 0 |

The largest float32 difference is on one MaSS13K photo (`05568`, 2.9 % sky); the lab photos differ
by at most 4.4e-4 (`DSC00602`). The mean difference is 2.5e-7. Against PyTorch float32 on the
random input of the conversion, float32 differs by 3.3e-5 (`CPU_ONLY`) and 2.7e-5 (`ALL`).

### 11.3 Verification

| Check | Result |
| --- | --- |
| Rust tests, Windows (`cargo test --release -- --skip live_`) | 373 + 4 + 5 + 1 passed, 0 failed (5 MVP Sky tests removed) |
| `cargo clippy --release --all-targets`, Windows | 65 warnings, none in the segmentation code (SM3.E #536: 70) |
| Rust tests, macOS (with the Core ML tests) | 376 + 4 + 5 + 1 passed, 0 failed, no warnings |
| Port check, macOS, Core ML `All` (`coreml`) | passed: 190 prompt masks, minimum photo IoU 0.999964; 12 sky alphas, grid IoU 1.000000, max alpha difference 4.1e-4; `DSC_1188` and `DSC_1535` not found, as in the reference |
| Port check, macOS, `cpu` (`CpuOnly`) | Sky passed (12 alphas, IoU 1.000000, max difference 3.5e-4); failed on the S3OD person crops and the Subject of `Nikon-D800e-raw-00007`, the same list as #477 |
| Port check, Windows, DirectML and ONNX Runtime CPU | passed: 190 prompt masks, minimum photo IoU 0.999973 (DirectML) and 0.999964 (CPU); 12 sky alphas, IoU 1.000000, max difference 3.9e-4 (DirectML) and 3.1e-4 (CPU) |
| Port reference | made again without the MVP Sky (`build/tmp/sm3e_coreml/port_reference`, 14 photos) |
| C++ `ctest -R "AiMaskEditSessionTest|SegmentationClientTest|AiMaskCoverage|AiMaskModelOutput"`, Windows debug | 50 of 50 passed |
| C++ `ctest -R "AiMaskEditSessionTest|SegmentationClientTest"`, macOS debug | 36 of 36 passed |
| Full test suite | not run (AGENTS.md) |

Changed tests: `engine::load_reports_all_five_models_and_fails_without_thresholds` and
`engine::image_entry_keeps_encoder_input_for_sky_head` (no backend without a sky head),
`coreml_backend::coreml_device_values_select_u15_compute_units` (the sky head on `All`),
`AiMaskEditSessionTest.PrepareCommitCropsToCoverageWithBorder` (the square coverage comes from a
fake decoder stroke instead of an R8 Sky), and the "not found" text of `SegmentationClientTest`.
Removed tests: the two MVP Sky engine tests, `sky_regions_use_eight_connectivity_and_minimum_size`,
`region_points_take_three_most_probable_cells`, `union_mask_is_or_of_upsampled_members`, and
`SegmentationClientTest.RunTaskMapsSkyRegionUnionAsBinaryMask`.

### 11.4 Measurements (macOS, Apple M4, release sidecar code in the port check)

| Item | Value |
| --- | ---: |
| Sky head load with the warm-up run (compiled model in the Core ML cache) | 132 ms |
| Sky head load at the first load after the conversion | 977 ms (S3OD 5,061 ms) |
| All five models, warm | 352 ms |
| First Sky request of a photo, port check p50 | 82 ms (`CpuOnly`: 168 ms) |
| Encoder p50 | 239 ms |
| Peak memory footprint of the port check process (`/usr/bin/time -l`) | 4,790 MB (largest resident set 3,065 MB) |

The macOS GUI was not run: the Mac is driven over SSH without a display. The host code of Sky is
the same on both platforms and is covered by the C++ tests; the sidecar is covered by the port check.
