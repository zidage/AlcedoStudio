# SM3.E Sky from the Fine Sky Head — Phase Plan

Date: 2026-10-26.

Status: the Rust part (#536) is implemented and verified on Windows (section 9). The host part
(#537) and the Core ML part (#538) are open.

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
| S5 | A backend without a sky head (Core ML, `load_sky_head` returns `None`) keeps the MVP Sky and the `BINARY` result until #538. | #536 scope: Windows MVP; the macOS build and tests stay green. |
| S6 | The model set id stays `alcedo-ai-mask-v1`. | The model set is not published yet (SM2, SM10). |
| S7 | The DirectML session of the sky head switches off the memory arena (`ep.dml.disable_memory_arena`), as the encoder and S3OD do (#532). | With the arena, the session keeps 1,150 MB of dedicated GPU memory after its first run; without it, 122 MB. A run takes 50 ms instead of 30 ms, once per image (section 9.3). |
| S8 | The license of the sky head is recorded in master plan D5 and section 8.2.1. The user-facing notice is part of #537. | U18. No model directory license file exists yet for any model; SM2 makes the published directory. |

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
