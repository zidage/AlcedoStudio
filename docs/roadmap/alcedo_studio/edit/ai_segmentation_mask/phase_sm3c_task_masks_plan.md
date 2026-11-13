# SM3.C Task Masks in the Segmentation Service — Phase Plan

Date: 2026-10-23.

Status: implemented and verified on Windows on 2026-10-23 (`feature/ai-segmentation-sm3c`). The
macOS port check with the Core ML models passed on 2026-10-23 with SM3.D (section 13.2).

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), section
9.2.3.

Prerequisites: SM3.A ([phase plan](phase_sm3a_segmentation_service_plan.md)) and SM3.B
([phase plan](phase_sm3b_embedding_cache_plan.md)), merged to `main` in `61d3f705` (#463).

---

## 1. Decision record

### 1.1 User request (2026-10-23)

- Merge #463 (SM3.B) and complete SM3.C: the task masks Subject, Background, Sky, and People in
  the sidecar segmentation service, on Windows and macOS.
- The user closed the open decisions of the master plan outline (F1, F4a, the response form of
  the People list) in this conversation (C1 to C4).

### 1.2 Inherited decisions

- D2 (model set), D12 (the sidecar runs the models; ONNX Runtime with DirectML on Windows, native
  Core ML on macOS), D13 (local model directory until SM2), D15 (U3 no edge post-processing, U6
  memory budget 3 GB on Windows and 3.5 GB on macOS).
- SM3.A decisions A1 to A9, in particular A3 (the host keeps the prompt selection), A6 (no
  cancellation protocol), and A7 (one execution provider, no other device after an error).
- SM3.B decisions B1 to B6: the opaque image key, the cache of 4 images, unload clears the cache,
  and the native residency of the encoder outputs.
- Master plan section 8.2: the lab MVP pipeline (`scripts/poc/ai_mask_mvp/mask_pipeline.py` of
  `alcedo-ai-mask-lab`, commit `2a33061`) is the reference for the host rules of the task masks.

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| C1 | User decision (F1). Subject is "not found" only when no value of the selected S3OD alpha is at least 0.5. The response always carries the S3OD IoU score and the area of the alpha that is at least 0.5. | A small real subject is never rejected. The quality set has no photo without a subject, so a score or area threshold has no evidence. SM10 can tune the rule with the reported values. |
| C2 | User decision (F4a). The People list holds the person boxes with a score of at least the list threshold of `people_sky_head.json` (0.3132). When the person presence is at least its threshold and no box reaches the list threshold, the list holds the single box with the best score. | The lab MVP loses the paddle boarder of `2088064441` (presence 0.999, best box 0.31267) by 0.0005. |
| C3 | User decision (F4a). The People list is sorted from left to right by the box center x. Equal centers are sorted by the score, highest first. The list index is the position in this order. | The list entries then match the positions of the people in the photo. |
| C4 | User decision. The sidecar makes the union masks. Sky returns one mask: the union of all sky regions. People returns the union of the people that the request selects; an empty selection selects all listed people. | The AI Mask tool shows the task masks in a panel. A host union would need one panel round trip per added person. |
| C5 | The union of SAM 2.1 masks is the MVP union: the OR of the member masks at the session image size, each the bilinear upsampling of its 256 x 256 logits with threshold 0. The sidecar returns it as an R8 mask (255 inside, 0 outside) at the session image size. Each member is evaluated only on the pixels of the bounding box of its positive cells. | The MVP unions boolean masks at the photo size (master plan 8.2.3), and the Mask system stores R8 coverage (D6). A maximum of the logits per cell is not the same mask: the bilinear upsampling of the maximum mixes the positive corners of two adjacent members and fills the gap between them. The first port check measured it: People unions of 6 to 20 people had a photo IoU of 0.988 to 0.998 against the MVP union, and the 4-region Sky union of `P1024439` 0.9935. |
| C6 | The load request loads all four models: the encoder, the decoder, the head, and S3OD. A failure of any model is the load error, and no model stays loaded. The head thresholds come from `people_sky_head.json` in the model directory; a missing file or a missing threshold is a load error. | Master plan 9.2.3. One load keeps one memory state for U6. |
| C7 | Sessions and cache entries of one image share one image entry. The entry holds the image embedding, the S3OD input image, and the task results. Each task result is computed once per entry, at the first request that needs it: the head outputs, the Sky union, the person masks, and the S3OD alpha. Unload and a model change drop all entries. | A key-only start (B1) has no pixels, so the S3OD input must stay with the entry. The results depend only on the image and the model set; the entry has the same lifetime as both. |
| C8 | The S3OD input image is the photo resized to a long edge of 1024 (the letterbox content, without the padding), RGB8, made at the session start from the pixels. | 3 MB or less per entry instead of the full photo (up to 50 MB). The padding is added at the S3OD run. |
| C9 | Background is 1 minus the Subject alpha. Background is "not found" when Subject is "not found", or when no value of the Background alpha is at least 0.5. | F2: the inverted F1 mask. Without a subject the photo has no separation into subject and background. |
| C10 | The head runs once at load on zero features (a warm-up run). S3OD has no warm-up run; it runs at the first Subject or Background request of an image entry. | Master plan 9.2.3. A head warm-up costs a few milliseconds. An S3OD warm-up would keep about 1.3 GB of DirectML working memory from the load on, also for users who never select Subject. The first-call time is measured (section 9). |

### 1.4 Rejected options

- A host union of per-region and per-person masks (C4).
- A Subject "not found" rule from the IoU score or from an area fraction (C1).
- A second transfer of the pixels with each Subject request (C7, C8).
- A union as the maximum of the 256 x 256 logits per cell (C5: the port check measured a photo
  IoU down to 0.988 against the MVP union).

---

## 2. Product and data specification

SM3.C has no user-visible change. SM9 calls the task masks; SM7 makes the committed coverage from
the returned masks.

### 2.1 Task request

`RunSegmentationTask(session_id, task, person_indices)`:

| Field | Rule |
| --- | --- |
| `session_id` | A started session. |
| `task` | `SUBJECT`, `BACKGROUND`, `SKY`, or `PEOPLE`. `UNSPECIFIED` is invalid input. |
| `person_indices` | `PEOPLE` only: indices into the People list (C3). Empty selects all listed people. An index outside the list is invalid input. Duplicate indices select a person once. For other tasks the field must be empty. |

### 2.2 Task response

| Field | Meaning |
| --- | --- |
| `found` | True when the response has a mask. False is a successful result with a message; the header status is OK. |
| `message` | The "not found" text with the measured value and the threshold, for example `No sky found: sky presence 0.312 is below 0.423.` Empty when `found` is true. |
| `mask_kind` | `BINARY` (Sky, People: the R8 union of C5 in `mask_r8`, 255 inside and 0 outside) or `ALPHA` (Subject, Background: values in [0, 1] in `alpha_values`, inside where >= 0.5). |
| `mask_width`, `mask_height`, `mask_r8`, `alpha_values` | The mask, row-major. Binary: the session image size. Alpha: the unpadded S3OD grid, long edge 1024 (C8). Both value fields are empty when `found` is false. |
| `presence`, `presence_threshold` | Sky and People: the head presence (sigmoid) and its threshold. |
| `region_count` | Sky: number of decoded regions. |
| `people` | People: the full People list (index, normalized box, score), also when the request selects a subset. |
| `subject_score`, `mask_area` | Subject, Background: the selected S3OD IoU score. All tasks: the number of mask values inside. |
| `model_ms` | Time of the model runs of this request (head, decoder, S3OD). 0 when all results come from the image entry. |

### 2.3 Sky rule (lab P1 section 3.3, MVP `select_sky`)

1. Head outputs of the entry (C7). Presence = sigmoid(`presence_logits[0]`). Presence below the
   sky threshold: not found.
2. Sky probability = sigmoid(`sky_logits`) on the 64 x 64 grid. Cells with probability >= 0.5 form
   8-connected regions. Keep regions of at least 3 cells, largest first; equal sizes keep the
   raster order of their first cell. No region: not found.
3. For each region, take the 3 cells with the highest probability inside the region (ties: the
   higher raster index first, as `np.argsort(kind="stable")[::-1]`). The points are the cell
   centers `((column + 0.5) * 16, (row + 0.5) * 16)` in model pixels, label add.
4. One decoder call per region, without a box and without a mask input; select one mask with the
   rule for several points (SM3.A plan 2.4).
5. Union of the region masks (C5).

### 2.4 People rule (lab `head_model.decode_people` with `nms_iou = 0.5`, MVP `select_people`)

1. Presence = sigmoid(`presence_logits[1]`). Presence below the person threshold: not found.
2. Heatmap = sigmoid(`center_logits`). A cell is a peak when it equals the maximum of its 3 x 3
   neighborhood (cells outside the grid do not count). Take the 20 peaks with the highest values
   (non-peaks count as 0), highest first, and keep the peaks with a value >= 0.05.
3. Box of a peak at (row, column): center = (column + `center_offset[0]`, row +
   `center_offset[1]`), size = exp(`log_size`); corners in cells.
4. Suppression: in score order, remove a box whose IoU with a kept box of higher score is >= 0.5.
5. People list: the boxes with score >= the list threshold; else, the best box when presence
   passed (C2); no box at all: not found. Sort by C3.
6. Person mask: the box clipped to [0, 64] cells, times 16 model pixels, as a box prompt (labels 2
   and 3), no mask input, one mask by the rule for a box. One decoder call per listed person, at
   the first People request of the entry (C7).
7. The response mask is the union (C5) of the selected person masks.

The normalized box of a list entry is the clipped box divided by 64.

### 2.5 Subject and Background rule (lab `s3od_graphs`, MVP `select_subject`)

1. S3OD input image (C8): scale = 1024 / max(width, height); resized size = (round(height *
   scale), round(width * scale)); OpenCV `INTER_LINEAR` (half-pixel centers, no antialiasing) on
   RGB8, values rounded to RGB8.
2. At the S3OD run: put the resized image in the middle of a black 1024 x 1024 image (padding
   `(1024 - side) // 2` on the top and the left), `(value / 255 - mean) / std` with the ImageNet
   mean and standard deviation, NCHW float32.
3. S3OD outputs `masks` [1, 3, 1024, 1024] and `iou_scores` [1, 3], both after the sigmoid. Take the
   mask with the largest score (first on ties) and remove the padding: the alpha on the resized
   grid.
4. Subject: the alpha, not found by C1. Background: 1 - alpha, not found by C9.

### 2.6 Errors

| Case | Result |
| --- | --- |
| Models not loaded | `MODEL_NOT_READY`. |
| Unknown session | `SESSION_NOT_FOUND`. |
| `UNSPECIFIED` task, indices with another task | `INVALID_INPUT`; no model run. |
| Index outside the People list | `INVALID_INPUT` (the list exists only after the head and the person decodes). |
| A model run fails | `INTERNAL` with the runtime message; the entry keeps no partial result. |

---

## 3. Scope

### 3.1 Included

- Rust: head and S3OD model roles, ONNX Runtime and Core ML adapters, the task rules module, the
  image entry, the engine task operation, the proto messages, and the server handler.
- The port check: a reference generator for the task masks and their comparison in
  `segmentation_port_check.rs`.
- C++: the task DTOs, `SegmentationClient::RunTask`, `AiMaskSegmentationService::RunTask`.
- The macOS build, unit tests, and port check with the Core ML models.

### 3.2 Excluded

- The AI Mask tool UI and the People panel (SM9).
- Feather and Edge, the photo-size coverage, and the commit (SM7).
- A combination of a task mask with strokes (F7, after a user decision).
- The model download (SM2).

---

## 4. Current source audit

Source revision: `61d3f705` (`main`).

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Model roles | `service/segmentation/backend.rs` | `SegmentationModelRole` has `Encoder` and `Decoder`. `SegmentationBackend` loads these two. | Add `Head` and `Subject` roles, traits, and loads. |
| Model set | `model_set.rs` | Specifications of the encoder and the decoder. | Add the head, its JSON file, and S3OD. |
| ONNX Runtime adapter | `ort_backend.rs` | `OrtImageFeatures` keeps the encoder outputs as ONNX Runtime values in CPU memory. | The head reads `image_embed` from these values. Add the S3OD session. |
| Core ML adapter | `coreml_backend.rs` | `CoreMlImageFeatures` keeps Float32 arrays. | The head adds the stored `image_embed` array to its input. Add the S3OD model. |
| Engine | `engine.rs` | Sessions and the cache map to `Arc<dyn ImageFeatures>`. | Map to a shared image entry (C7) and add `run_task`. |
| Server | `server/segmentation.rs` | Six RPCs. | Add `RunSegmentationTask`. |
| Proto | `proto/segmentation.proto` | No task messages. | Add the task request and response. |
| C++ client | `sidecar_client/segmentation_client.{hpp,cpp}` | Load, unload, status, start, decode, end. | Add `RunTask`. |
| App service | `app/ai_mask_segmentation_service.{hpp,cpp}` | Owns the current session id. | Add `RunTask` on the current session. |
| Port check | `tests/segmentation_port_check.rs`, `scripts/ai_models/sam2/make_segmentation_port_reference.py` | Prompt masks only. | Add the task masks. |

---

## 5. Target architecture

### 5.1 Rust owners

- `backend.rs`: `SegmentationHead::run(&Arc<dyn ImageFeatures>) -> HeadOutput` and
  `SubjectModel::run(&TensorF32) -> SubjectOutput`; `SegmentationBackend::load_head` and
  `load_subject_model`.
- `task_masks.rs` (new): pure functions of section 2: `head_thresholds_from_json`,
  `sky_regions`, `region_points`, `decode_people`, `people_list`, `union_mask`,
  `subject_input_image`, `subject_pixel_values`, and `select_subject_alpha`.
- `image_entry.rs` (new): `ImageEntry` (image size, features, S3OD input image, once-computed
  task results). The Sky result keeps the region logits; the People result keeps the person
  logits. A request makes the union of the selected members (C5).
- `engine.rs`: `run_task(session_id, TaskRequest) -> TaskResult`. Task requests hold the read
  lock of the model set for the whole run, as decode does. The head and S3OD each have their own
  mutex. Each once-computed result has its own mutex in the entry; the engine holds it while it
  computes the result, so two requests on one entry compute it once.

### 5.2 C++ owners

- `SegmentationClient::RunTask(session_id, task, person_indices, timeout) -> SegmentationTaskResult`.
- `AiMaskSegmentationService::RunTask(task, person_indices)`: fails without a sidecar call when no
  session exists or the service is closed.

### 5.3 Primary success call chain (People, two people of five)

```text
AiMaskSegmentationService::RunTask(PEOPLE, {1, 3})
  -> SegmentationClient::RunTask -> gRPC RunSegmentationTask
  -> SegmentationServiceImpl::run_segmentation_task -> spawn_blocking
  -> SegmentationEngine::run_task(session, People{[1, 3]})
       -> session entry -> head outputs (first request: head.run(features))
       -> person masks (first request: decode_people -> people_list -> decoder per person)
       -> union_mask(masks 1 and 3, image size) -> TaskResult{found, BINARY, R8, people list}
```

### 5.4 Primary failure call chain

```text
RunTask(SUBJECT) on an entry -> S3OD run fails (DirectML error)
  -> SegmentationError::Inference(message) -> header INTERNAL with the message
  -> the entry keeps no alpha; the next Subject request runs S3OD again
  -> C++: SegmentationTaskResult.status.ok = false, error = message
```

---

## 6. File and API map

| File | Change |
| --- | --- |
| `rust/puerh_mind/proto/segmentation.proto` | `RunSegmentationTask`, `SegmentationTask`, `SegmentationTaskMaskKind`, `SegmentationPerson`, request and response. |
| `rust/puerh_mind/src/service/segmentation/{backend,model_set,ort_backend,coreml_backend,engine,mod}.rs` | Section 5.1. |
| `rust/puerh_mind/src/service/segmentation/{task_masks,image_entry}.rs` | New. |
| `rust/puerh_mind/src/service/segmentation/test_support.rs` | Fake head and fake S3OD with call counts. |
| `rust/puerh_mind/src/server/segmentation.rs` | Handler and tests. |
| `rust/puerh_mind/tests/segmentation_port_check.rs` | Task mask comparison. |
| `scripts/ai_models/sam2/make_segmentation_port_reference.py` | Task mask reference (`--head-onnx`, `--head-json`, `--subject-onnx`). |
| `alcedo_studio/src/include/sidecar_client/dto/segmentation.hpp` | `SegmentationTask`, `SegmentationPerson`, `SegmentationTaskResult`. |
| `alcedo_studio/src/{include/,}sidecar_client/segmentation_client.{hpp,cpp}` | `RunTask`. |
| `alcedo_studio/src/{include/,}app/ai_mask_segmentation_service.{hpp,cpp}` | `RunTask`. |
| `alcedo_studio/tests/app/{segmentation_client_test,ai_mask_segmentation_service_test}.cpp` | Tests. |

Model directory additions (developer setup until SM2): Windows `people_sky_head.onnx`,
`people_sky_head.json`, `subject_masks.onnx` (lab `build/tmp/explore/p1/colab_run2_coco_openimages/export/`
and `build/tmp/explore/s3od/onnx_mha_chunk8/float16/`); macOS `people_sky_head.mlmodelc`,
`people_sky_head.json`, `s3od.mlmodelc` (lab `build/tmp/explore/coreml/float32/` and
`build/tmp/explore/coreml/float16_embeddings_attention_float32/`).

---

## 7. Implementation phases

### 7.1 SM3.C.1 — Rust task masks and port check

1. Add the head and S3OD specifications to `model_set.rs`, and the roles and traits to
   `backend.rs`.
2. Implement the ONNX Runtime head (input `image_embed` from `OrtImageFeatures`, warm-up run at
   load) and the S3OD session.
3. Implement the Core ML head and S3OD (compute units `All`; `cpu` device: `CpuOnly`).
4. Write `task_masks.rs` with the rules of section 2 and unit tests.
5. Write `image_entry.rs`; change the engine sessions and cache to `Arc<ImageEntry>`; make the S3OD
   input image at the start.
6. Load all four models and the thresholds (C6); report four residencies.
7. Add `SegmentationEngine::run_task`.
8. Add the proto messages and the server handler.
9. Extend the reference generator and the port check.
10. Measure (section 9).

Expected diff: 1,300-1,600 lines (Rust 1,000-1,200 with tests, Python 150, port check 150-250).

### 7.2 SM3.C.2 — C++ client, app service, and macOS

1. Add the DTOs, `SegmentationClient::RunTask`, and `AiMaskSegmentationService::RunTask` with
   tests.
2. Build and test on macOS; run the port check with the Core ML models.
3. Write the completion record.

Expected diff: 300-450 lines (C++ 250-350 with tests, documents 100).

---

## 8. Tests

| Name | Observable assertion |
| --- | --- |
| Rust `head_thresholds_read_from_lab_json` | Thresholds 0.4232, 0.5332, 0.3132 from a JSON with the lab structure; a JSON without `person_list_best_f1_nms` is an error. |
| Rust `sky_regions_use_eight_connectivity_and_minimum_size` | A diagonal pair joins one region; a 2-cell region is dropped; order largest first. |
| Rust `region_points_take_three_most_probable_cells` | Cell centers in model pixels; ties take the higher raster index. |
| Rust `decode_people_finds_peaks_and_suppresses_overlaps` | Two peaks with IoU >= 0.5: one box; offsets and sizes give the expected corners. |
| Rust `people_list_orders_left_to_right_and_keeps_best_box` | Order by center x; below the list threshold with presence passed: the best box only. |
| Rust `union_mask_is_or_of_upsampled_members` | Every pixel of a 1000 x 700 union equals the OR of the two members upsampled over the whole grid; a per-cell maximum gives a larger mask for two members that touch at a corner. |
| Rust `subject_input_image_matches_letterbox_size` | 4096 x 2730 gives 1024 x 683; values of a constant image unchanged. |
| Rust `subject_pixel_values_pad_center_and_normalize` | Padding rows are the normalized zero; content rows the normalized values. |
| Rust `select_subject_alpha_takes_best_score_and_removes_padding` | Alpha size equals the resized size; the second mask selected when its score is largest. |
| Rust `subject_not_found_when_no_alpha_reaches_half` | All values 0.49: not found with the score and area 0. |
| Rust `run_task_computes_head_and_people_once_per_entry` | Two People requests with other selections: head runs 1, decoder runs = listed people; second `model_ms` 0. |
| Rust `run_task_people_selection_unions_selected_masks` | Selection {0} and {0, 1} with the fake decoder: union values. |
| Rust `run_task_rejects_invalid_task_and_index` | Indices with SKY: `InvalidInput` before any model run. Index 5 of 2 people: `InvalidInput`. Unknown session: `SessionNotFound`. After unload: `ModelNotReady`. |
| Rust `server_maps_task_result_and_not_found` | Sky not found: header OK, `found` false, the message; `UNSPECIFIED`: `INVALID_ARGUMENT`; unknown session: `SESSION_NOT_FOUND`. |
| Rust `server_maps_people_list_and_binary_mask` | A found People result: `BINARY`, the image size, the R8 values, and the People list box. |
| Rust `cached_start_runs_subject_without_pixels` | Start with key and pixels, end, key-only start, Subject: S3OD runs once with the stored input. |
| Rust `subject_failure_keeps_no_alpha` | Fake S3OD fails once: `Inference`; the next request runs S3OD again and succeeds. |
| Rust `load_reports_four_models_and_fails_without_thresholds` | Four residencies; a directory without `people_sky_head.json` fails with `ModelNotReady` and nothing loaded. |
| Rust `segmentation_port_check_matches_reference_masks` (extended) | Per photo: Sky and People presence decisions, region count, People list order and boxes (within 1e-3 cell), each person mask, the Sky union, and the People union with IoU >= 0.999 at the photo size against the OR of the reference masks, Subject alpha >= 0.5 IoU >= 0.998 on the S3OD grid, Background the same. A key-only start of a cached photo reads the stored Subject result. |
| C++ `SegmentationClientTest.RunTaskCarriesTaskAndSelection` | The server sees the task and indices; the DTO receives the R8 mask, kind, size, and People list. |
| C++ `SegmentationClientTest.RunTaskNotFoundIsSuccessWithMessage` | `ok` true, `found` false, the message. |
| C++ `AiMaskSegmentationServiceTest.RunTaskWithoutSessionFailsWithoutCall` | `ok` false; the fake client records no call. |
| C++ `AiMaskSegmentationServiceTest.RunTaskUsesCurrentSession` | The fake client receives the current session id, the task, and the indices. |

---

## 9. Measurements

Windows, RTX 3080 Laptop, DirectML, release build; macOS, Apple Silicon, native Core ML.

| Item | Method |
| --- | --- |
| Load time per model (four models) | Port check. |
| Head time; Sky and People request time (first and from the entry) | Port check. |
| S3OD time: first call after load, later calls | Port check. |
| Start time with pixels (encoder and S3OD input image) | Port check `encoder_ms`, against the SM3.B record. |
| Dedicated GPU memory after load and after Subject (Windows) | `Get-Counter "\GPU Process Memory(pid_<pid>*)\Dedicated Usage"` during the port check. Compare with U6 (3 GB). |

---

## 10. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab\rust\puerh_mind
cmd /c "script\cargo_msvc.cmd test --release -- --skip live_"
# Reference (lab environment, ONNX Runtime 1.31 CPU)
D:\Projects\alcedo-ai-mask-lab\build\tmp\explore\venv\Scripts\python.exe -I `
  D:\Projects\pu-erh_lab\scripts\ai_models\sam2\make_segmentation_port_reference.py `
  --onnx-dir <model directory> --photos <lab input_4096 photos> --prompts <lab prompts.json> `
  --head-onnx <model directory>\people_sky_head.onnx --head-json <model directory>\people_sky_head.json `
  --subject-onnx <model directory>\subject_masks.onnx --out D:\Projects\pu-erh_lab\build\tmp\sm3c\port_reference
$env:ALCEDO_SEGMENTATION_MODEL_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3c\models\alcedo-ai-mask-v1"
$env:ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3c\port_reference"
$env:ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE = "directml"
cmd /c "script\cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture"
```

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SegmentationClientTest AiMaskSegmentationServiceTest --parallel 4
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;" + $env:PATH
ctest --test-dir build/debug -R "SegmentationClientTest|AiMaskSegmentationServiceTest" --output-on-failure
```

```bash
# macOS
cd rust/puerh_mind && cargo test --release -- --skip live_
ALCEDO_SEGMENTATION_MODEL_DIR=<directory with the .mlmodelc models and people_sky_head.json> \
ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR=<copy of the port reference> \
cargo test --release --test segmentation_port_check -- --nocapture
```

## 11. Repository rules that apply

Read `AGENTS.md` and the applicable skills again before implementation.

- No fallback: a failed head or S3OD load is a load error; no other device (A7). A failed run
  returns the real error; no substitute mask.
- The once-computed results of an image entry are owner state of the entry, not a copy of other
  state. No generation or token: the entry lifetime equals the image and the model set.
- Run MSVC builds through PowerShell with `scripts\msvc_env.cmd`; run `cargo` through
  `script\cargo_msvc.cmd`.
- Temporary files under `build/tmp/sm3c/`.
- Only targeted test runs; no full `ctest` run.
- Prohibited terms: check the new code and documents before completion.

## 12. Expected diff

1,600-2,050 lines in two parts (section 7). Each part stays under 2,000 lines.

## 13. Completion record

### 13.1 SM3.C completion record (2026-10-23)

**Status:** SM3.C.1 and SM3.C.2 are implemented and verified on Windows. macOS: the Core ML
adapter type-checks for `aarch64-apple-darwin`; the macOS build and the C++ tests run in the
`macOS arm64 Metal` CI job of the pull request. The macOS port check with the Core ML models is
not done: the Mac was not reachable on 2026-10-23 (section 13.3).

**Source revision and branch:** `be162dd8` (plan) and `cc7c7d31` (implementation) on `61d3f705`
(`main` after #463), `feature/ai-segmentation-sm3c`.

**Actual changed modules:** segmentation protocol (`RunSegmentationTask`); Rust model set,
backend interface, ONNX Runtime and Core ML adapters (head and S3OD), task mask rules
(`task_masks.rs`), image entry (`image_entry.rs`), engine (`run_task`), server handler, test
fakes, port check; the port reference script; the C++ task DTOs, `SegmentationClient::RunTask`,
and `AiMaskSegmentationService::RunTask`.

**Actual diff:** about 3,250 code lines and 400 document lines: Rust production about 1,520,
Rust unit tests and the port check about 1,180, C++ about 160 and its tests about 120, Python
130. The estimate (1,600 to 2,050 in total) was too low: the unit tests of the task rules and of
the engine are larger than planned. The phase stays one pull request, because the C++ part
(about 280 lines) depends on the protocol of the Rust part.

**Change of decision C5 during the implementation:** the first implementation made the union as
the maximum of the 256 x 256 logits per cell. The first DirectML port check failed on five
unions: People unions of 6 to 20 people had a photo IoU of 0.988 to 0.998 against the MVP union,
and the 4-region Sky union of `P1024439` 0.9935; every member mask alone passed. The bilinear
upsampling of the maximum mixes the positive corners of two adjacent members. C5 now returns the
MVP union (the OR of the upsampled member masks) as an R8 mask at the session image size; all
unions then pass with a photo IoU of at least 0.99998.

**Primary call chains:** as in sections 5.3 and 5.4. Success: `AiMaskSegmentationService::RunTask`
-> `SegmentationClient::RunTask` -> `RunSegmentationTask` -> `SegmentationEngine::run_task` ->
image entry results (head, Sky regions, person masks, Subject alpha; each computed once) ->
`task_masks::union_mask` or the alpha -> response. Failure: a model error ->
`SegmentationError::Inference` -> header `INTERNAL` with the runtime message; the entry keeps no
partial result.

**Tests (Windows, release unless noted):**

| Test | Result |
| --- | --- |
| `cargo test --release -- --skip live_` | 346 + 4 + 5 + 1 passed, 0 failed (`main`: 324 + 4 + 5 + 1). The port check skips without its environment. |
| New Rust unit tests (`task_masks`: 12, `engine`: 9, `server`: 2, adapter missing-file case) | all passed; names in section 8 |
| Port check, DirectML (`segmentation_port_check_matches_reference_masks`) | passed in 6 runs: 213 masks; minimum photo IoU 0.999591 (`DSC00562` person 5); Sky and People unions at least 0.999981; Subject grid IoU at least 0.998102 (`DSC00527`); Background at least 0.999834; Sky region counts, People list lengths, order, and boxes equal on all 14 photos |
| Port check, ONNX Runtime CPU | passed: 213 masks; minimum photo IoU 0.999964; unions at least 0.999995; Subject grid IoU at least 0.998423; Background at least 0.999857 (S3OD float16 on the CPU: about 19 s per photo) |
| C++ `SegmentationClientTest`, `AiMaskSegmentationServiceTest` (`win_debug`) | 21 of 21 passed (4 new) |
| Real sidecar, Python gRPC client (`build/tmp/sm3c/e2e_client.py`) | passed: four models load; Sky, People, Subject, Background, a People selection, an index outside the list (`INVALID_INPUT`), an unspecified task (`INVALID_INPUT`), "not found" results with the presence values, a key-only start that reads the stored Subject result, unload |
| `cargo clippy --release --all-targets` | no warning in the segmentation files |
| Core ML adapter type check for `aarch64-apple-darwin` (shim crate of SM3.B) | passed, 0 warnings |

The reference (`scripts/ai_models/sam2/make_segmentation_port_reference.py`, ONNX Runtime 1.31
CPU, the functions of the lab repository) gives the Sky and People results of the MVP report on
all 14 photos; the only difference is `2088064441`, which lists 1 person by C2 (the MVP list is
empty).

**Measurements (Windows, RTX 3080 Laptop 8 GB, DirectML, release; milliseconds):**

| Item | Value |
| --- | ---: |
| Load: encoder, decoder, head, S3OD (port check, warm files) | 1,020 to 1,180; 920 to 1,090; 91 to 109; 795 to 1,005 |
| Load round trip, first process after the build (real sidecar) | 5,888 |
| First Sky request of a photo (head, region decodes, union), p50 | 15 to 23 |
| First People request of a photo (person decodes, union), p50; `DSC00527` with 20 people | 11 to 19; 117 to 203 |
| First Subject request of a photo (S3OD with pre- and postprocessing), p50; first in the process | 210 to 218; 250 to 361 |
| Request from the stored results, p50 (engine, with the union at 4096 px) | 3.0 to 3.5 |
| Real sidecar round trip: People selection change on `DSC00527` (10.7 MB R8 mask) | 36 to 58 |
| Real sidecar round trip: Background after Subject (2.7 MB alpha) | 13 to 16 |
| CPU work of a start: S3OD input image; SAM 2.1 preprocessing (for comparison) | 6.5; 60 |
| Union of one full-image member at 4096 x 2730 | 6.5 |

Dedicated GPU memory of the process (peak, port check; `Get-Counter "\GPU Process Memory"`):

| State | MB |
| --- | ---: |
| Encoder, decoder, and head (S3OD not loaded; temporary local build) | 1,664 |
| All four models, S3OD never run | 1,894 |
| All four models after Subject requests | 2,955 |
| Real sidecar: after load / after the task requests / after unload | 838 / 2,955 / 0 |

The peak of 2,955 MB is inside the 3 GB budget of U6 (the SM3.B plan expected about 2,950 MB).

**Effect of the resident S3OD session on SAM 2.1 (same day, back to back, port check p50):**

| Build | Encoder (with preprocessing) | Decode, 1 / 2 / 3 / 4 points |
| --- | ---: | --- |
| `main` (SM3.B), 2 runs | 143, 144 | 8.3-8.8 / 8.1 / 8.0-8.6 / 8.4-8.5 |
| SM3.C without S3OD loaded (temporary local build, prompts only), 2 runs | 145, 151 | 8.6-8.7 / 8.3-8.4 / 8.5-8.6 / 8.5-8.7 |
| SM3.C, S3OD loaded and never run (prompts only), 3 runs | 169 to 222 | 8.9-10.2 / 9.0-11.3 / 8.4-10.8 / 10.0-11.3 |
| SM3.C with the task requests, 5 runs | 206 to 215 | 8.7-9.7 / 11.3-11.9 / 9.8-11.2 / 10.7-11.5 |

With the S3OD DirectML session resident, the SAM 2.1 encoder takes about 60 ms longer and a
decode about 2 to 3 ms longer. The CPU work of SM3.C adds only about 7 ms to a start. The cause
inside DirectML is not known. The other choice is to load S3OD at the first Subject request of an
image and unload it after the run (about 0.9 s of load and a first run for each new image). The
user moved this decision to SM9 (section 13.3).

### 13.2 macOS record

First attempt (2026-10-23, SM3.C): not done. `yurunmac.local` did not resolve and the known
addresses of the Mac did not answer.

Done with SM3.D on 2026-10-23 (SM3.D plan, section 12.3): Apple Silicon, macOS 27.0.1, the SM3.D
branch on `2722a740`, the Core ML models `point_encoder.mlmodelc`, `point_decoder.mlmodelc`,
`people_sky_head.mlmodelc` with its JSON file, and `s3od.mlmodelc`
(`float16_embeddings_attention_float32`). The SM3.D port check includes all SM3.C tasks.

| Test | Result |
| --- | --- |
| `cargo test --release -- --skip live_` | 368 + 4 + 5 + 1 passed |
| C++ `SegmentationClientTest`, `AiMaskSegmentationServiceTest` | 22 of 22 passed |
| Port check, Core ML `All` (device `""`) | passed: prompt masks minimum photo IoU 0.999964; Sky unions IoU at least 0.999999; Subject grid IoU at least 0.998243; Background at least 0.999960; encoder p50 424 to 430 ms; decoder p50 41 to 44 ms |
| Port check, Core ML `CPUOnly` (device `cpu`) | failed: `Nikon-D800e-raw-00007` Subject grid IoU 0.996392 (and SM3.D person crops); prompt masks, Sky, and Background pass. Issue #477. The app uses Core ML `All`. |

### 13.3 Remaining items

- The macOS port check with the Core ML models: done with SM3.D (section 13.2). It passes on Core
  ML `All`; the `CPUOnly` device fails (issue #477).
- S3OD residency: keep S3OD resident from load to unload (current, C6 and C10), or load it for
  each Subject computation (section 13.1, last table). The user decided on 2026-10-23 that SM9
  makes this decision together with the AI Mask tool interaction (when the tool requests Subject
  and Background, and how long it keeps a session). SM3.C keeps C6 and C10 until then.
