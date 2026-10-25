# SM3.D People Masks from S3OD on Person Crops — Phase Plan

Date: 2026-10-23.

Status: implemented and verified on Windows and macOS on 2026-10-23
(`feature/ai-segmentation-sm3d`, section 12).

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), decisions
D21 and D22, and the SM3.D entry of the phase summary.

Prerequisites: SM3.C ([phase plan](phase_sm3c_task_masks_plan.md)), merged to `main`. The SM3.C
macOS port check is open (SM3.C plan section 13.2); SM3.D runs it together with its own macOS
check (section 10.3).

Issue: #469. Tracking issue: #468.

---

## 1. Decision record

### 1.1 User request (2026-10-23)

- People masks use S3OD, as Subject does, and not one SAM 2.1 box prompt per person. S3OD gives
  better person edges.
- The People task starts with the full-image S3OD alpha (the Subject result). S3OD usually selects
  all people of a group photo in one run.
- The person boxes of the people and sky head show at the same time. When the Subject alpha has
  too few or too many people, the user clicks a box to add that person or to remove that person.
  Removal is as important as addition.
- The alpha of one person comes from S3OD on the crop of the person box, taken from the session
  image. The session image has a long edge of up to 4096, so a person crop is near the 1024 model
  input.
- Overlapping neighbors get no special rule. The host applies Add and Subtract steps in the order
  of the clicks (master plan D16, SM5.2).
- Sky does not change.

### 1.2 Inherited decisions

- Master plan D2 (model set), D12 (the sidecar runs the models), D13 (local model directory until
  SM2), D15 (U3 no edge post-processing, U6 memory budget), D21 (People from S3OD), and D22 (S3OD
  stays resident from load to unload).
- SM3.A A3 (the host keeps the selection), A6 (no cancellation protocol), A7 (one execution
  provider, no other device after an error).
- SM3.B B1 to B6 (opaque image key, cache of 4 images, unload clears the cache).
- SM3.C C1 (Subject "not found" rule), C2 and C3 (People list threshold and order), C6 (one load
  of four models), C7 (one image entry per image, results computed once), C8 (no full photo in the
  entry), C9 (Background), C10 (no S3OD warm-up run).

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| E1 | The People request returns the People list and the Subject alpha (the same once-computed result as Subject and Background). It does not run SAM 2.1. | D21. One S3OD run serves Subject, Background, and People of one image. |
| E2 | A new task `PERSON` with one `person_index` returns the alpha of one person: S3OD on the crop of that person, on the crop grid, with the crop rectangle. The sidecar computes it once per person and image entry. | The host composes the steps (D16). One request per click keeps the response small (at most 4 MB). |
| E3 | Person crop rectangle: the clipped person box (`PersonBox::prompt_box`), widened by 10 % of the box width on the left and on the right and by 10 % of the box height on the top and on the bottom, clipped to the image. In session pixels: `x0 = floor(left * width)`, `x1 = ceil(right * width)`, the same for y; at least 1 pixel on each axis. | The head boxes are tight bounding boxes (COCO). S3OD needs a small context ring around the person to find the edge. The lab check (section 7.1) shows the result by eye; a change of the margin after the check is recorded in section 12. |
| E4 | The image entry keeps one crop image per listed person, made at the session start from the pixels: the crop pixels when the long edge of the crop is at most 1024, else the crop resized with the S3OD resize (`subject_input_image`). At the person request, `subject_input_image` of the stored crop gives the S3OD input. | A key-only start (B1) has no pixels, so the crops must stay with the entry (C7). The entry keeps at most 3 MB per person and no full photo (C8). The resize of a 1024 crop is the identity, so the two steps give the same input as one resize of a large crop. |
| E5 | The head runs in `start_session` after the encoder, and the People list is made there. A head failure is a start failure; the entry is not cached. Sky and People read the stored head output and People list. | E4 needs the boxes at the start. The head takes about 2 ms. A start that cannot make the crops cannot serve People later. |
| E6 | People "not found" (presence below its threshold, or no box) is a success with `found = false` and the SM3.C message. It runs no S3OD. | C2 and C3 stay. Without people, the Subject alpha is not a People result. |
| E7 | When the People list has people, the People result is found and carries the Subject alpha, also when no Subject value reaches 0.5 (`mask_area` 0). | The user then adds people with the boxes. The Subject "not found" message is a Subject result, not a People result. |
| E8 | Each People list entry carries `subject_fraction`: the share of the Subject grid pixels inside the person box (not widened) whose Subject alpha is at least 0.5. It also carries `in_subject`: `subject_fraction >= PERSON_IN_SUBJECT_FRACTION`. The start value is 0.3. The lab step (section 7.1) sets the final value: the value in steps of 0.05 that agrees most often with the lab rule "at least half of the person alpha >= 0.5 is inside the Subject alpha >= 0.5" over all listed people of the check photos; on equal counts the smaller value. | The host shows each box as included or not included without a person S3OD run. A click on an included box subtracts the person; a click on another box adds the person (SM5.2). The sidecar owns the rules that read model outputs, as for C1 and C2. |
| E9 | A person whose crop alpha has no value >= 0.5 is a success with `found = false` and the message `No person found in the crop of person <i>: no alpha value reaches 0.5 (S3OD score <s>).` The result is stored like a found result. | The same rule as C1. The host makes no step for this click and shows the message. |
| E10 | Every `ALPHA` result carries `mask_box`, the normalized rectangle of the session image that its grid covers: (0, 0, 1, 1) for Subject, Background, and People; the crop rectangle for `PERSON`. `BINARY` results (Sky) carry no box. | One host rule maps every alpha grid to the session image. |
| E11 | The request field `person_indices` (field 4) is removed and reserved. The new field `optional uint32 person_index = 5` is required for `PERSON` and must be absent for the other tasks. | No production caller uses `person_indices` (SM9 is not implemented). The People union of C4 and C5 for People ends with E1. |
| E12 | C4 and C5 stay for Sky only. | Sky does not change (D21). |

### 1.4 Rejected options

- One SAM 2.1 box prompt per person (the SM3.C People rule).
- A sidecar union of the selected people (C4 for People). The host composes the ordered steps
  (D16).
- Keeping the full photo in the image entry to cut crops at the request (C8).
- A person crop from the S3OD input image of long edge 1024 (C8). A small person would become a
  few hundred pixels enlarged to 1024.
- A special rule for an overlapping neighbor in a crop (user decision, section 1.1).
- An S3OD run for each listed person at the People request. The cost grows with the people count
  (about 200 ms each on DirectML), and E8 shows the included state without it.

---

## 2. Product and data specification

SM3.D has no user-visible change. SM5.2 composes the People steps; SM9 shows the boxes and the
messages.

### 2.1 Task request

`RunSegmentationTask(session_id, task, person_index)`:

| Field | Rule |
| --- | --- |
| `session_id` | A started session. |
| `task` | `SUBJECT`, `BACKGROUND`, `SKY`, `PEOPLE`, or `PERSON`. `UNSPECIFIED` is invalid input. |
| `person_index` | `PERSON` only, required: an index into the People list (C3). Absent with `PERSON`, present with another task, or outside the list: invalid input. |

### 2.2 Task response

Changes against SM3.C plan section 2.2:

| Field | Meaning |
| --- | --- |
| `found` | Subject, Background, Sky: as SM3.C. People: the People list has people (E6, E7). Person: the crop alpha has a value >= 0.5 (E9). |
| `mask_kind` | `BINARY`: Sky only. `ALPHA`: Subject, Background, People (the Subject alpha), Person (the crop alpha). |
| `mask_width`, `mask_height`, `alpha_values` | Subject, Background, People: the unpadded S3OD grid of the photo (C8). Person: the unpadded S3OD grid of the crop (the crop resized to a long edge of 1024). |
| `mask_box` (new, field 16) | `ALPHA` results: the normalized rectangle of the session image that the grid covers (E10). |
| `people` | People and Person: the full People list. Each entry adds `subject_fraction` (field 4) and `in_subject` (field 5) (E8). |
| `subject_score` | Subject, Background, People: the S3OD IoU score of the photo run. Person: the S3OD IoU score of the crop run. |
| `presence`, `presence_threshold` | Sky, People, and Person: the head presence and its threshold. |
| `model_ms` | Time of the model runs of this request. People: the photo S3OD run at the first Subject, Background, or People request. Person: the crop S3OD run at the first request for that person. |

### 2.3 People rule

1. At the session start (E5): head outputs, presence, `decode_people`, and `people_list` by the
   SM3.C rules 2.4 steps 1 to 5. Presence below the threshold or no box: the entry keeps the
   "not found" message and no crops.
2. For each listed person, the crop rectangle of E3 and the stored crop image of E4.
3. People request: not found when step 1 found nothing (E6), with no S3OD run. Else the Subject
   alpha of the entry (computed once, SM3.C rule 2.5), `subject_fraction` and `in_subject` of
   each person (E8), and `found = true` (E7).

`subject_fraction` on the Subject grid of width `w` and height `h`: the box pixels are the columns
`floor(x0 * w)` to `ceil(x1 * w) - 1` and the rows `floor(y0 * h)` to `ceil(y1 * h) - 1` of the
normalized clipped box, at least one pixel on each axis.

### 2.4 Person rule

1. Valid index (2.1). The person result of the entry, computed once (C7).
2. S3OD input: `subject_input_image` of the stored crop image, then `subject_pixel_values`
   (letterbox to 1024 x 1024).
3. S3OD run with the resident model (D22). `select_subject_alpha` with the crop input: the mask
   with the largest IoU score, unpadded.
4. Not found by E9. Else the alpha, its grid size, and `mask_box` = the crop rectangle divided by
   the session image size.

### 2.5 Host composition (owned by SM5.2)

For information only: SM5.2 maps an alpha grid to the session image with `mask_box` by bilinear
resampling with half-pixel centers (anti-aliased since master plan D23). It adds a person with the
maximum and subtracts a person with the minimum of the current coverage and (1 - person alpha), in
click order. Pixels outside `mask_box` do not change.

### 2.6 Errors

| Case | Result |
| --- | --- |
| Models not loaded | `MODEL_NOT_READY`. |
| Unknown session | `SESSION_NOT_FOUND`. |
| `UNSPECIFIED` task; `person_index` absent with `PERSON` or present with another task | `INVALID_INPUT`; no model run. |
| `person_index` outside the People list (also when People found nothing) | `INVALID_INPUT`. |
| The head fails at the start | `INTERNAL` with the runtime message; no session, no cache entry (E5). |
| An S3OD run fails | `INTERNAL` with the runtime message; the entry keeps no partial result; the next request runs S3OD again. |

---

## 3. Scope

### 3.1 Included

- Lab repository `alcedo-ai-mask-lab`: the People rule of this plan in
  `scripts/poc/ai_mask_mvp/mask_pipeline.py`, the People panel of `ai_mask_app.py` (boxes that add
  or remove a person), a check script that writes the check pages, the value of E8, and a report.
- Rust: the image entry (head, People list, crops), `start_session`, the People and Person tasks,
  the proto, the server mapping, unit tests, and the port check.
- The port reference script `scripts/ai_models/sam2/make_segmentation_port_reference.py`.
- C++: the DTO fields, `SegmentationClient::RunTask`, `AiMaskSegmentationService::RunTask`, and
  their tests.
- The macOS build, unit tests, and port check with the Core ML models, together with the open
  SM3.C macOS check.

### 3.2 Excluded

- The host composition of People steps and the session history (SM5.2, D16).
- The People panel of the product UI (SM9).
- Sky (no change).
- The model download (SM2).

---

## 4. Current source audit

Source revision: `1015ff4e` (`main`).

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Image entry | `rust/puerh_mind/src/service/segmentation/image_entry.rs:52-62` | `ImageEntry` holds the size, `features`, `subject_input` (long edge 1024), and the slots `head`, `sky`, `people`, `subject`. `PersonMask` (:31-35) holds a person box and its SAM 2.1 logits. | Head output and People list as start fields; one crop image per person; one person alpha slot per person. Remove `PersonMask.logits`. |
| Start | `engine.rs:487-553` | `start_session` runs `sam2::preprocess_image`, `subject_input_image`, and the encoder. No head run. | Run the head, make the People list and the crops (E4, E5). |
| Task dispatch | `engine.rs:621-646` | `run_task` rejects indices for tasks other than People and dispatches Sky, People, Subject, Background. | Add `Person`; check `person_index` (E11). |
| Head | `engine.rs:649-662` | `head_outputs` computes the head once in the `head` slot at the first Sky or People request. | Read the stored head output. |
| People | `engine.rs:727-817` | `people_task` computes the person masks once (one SAM 2.1 box decode per listed person, `compute_people`), checks the indices, and returns the C5 union as `BINARY`. | People rule 2.3: People list and Subject alpha, no decoder. |
| Subject | `engine.rs:820-865` | `subject_task` computes the S3OD alpha once in the `subject` slot and applies C1 and C9. | Share the slot computation with People; make the S3OD run a function of the input image, for the photo and for a crop. |
| Task rules | `task_masks.rs:158-195`, `:202-286`, `:387-555` | `PersonBox::prompt_box`, `decode_people`, `people_list`, `subject_input_image`, `subject_pixel_values`, `select_subject_alpha`. | Add the crop rectangle, the crop image, and `subject_fraction` functions. |
| Task types | `engine.rs:127-187` | `SegmentationTask` (Subject, Background, Sky, People), `TaskRequest{task, person_indices}`, `TaskMask`, `PersonListEntry{x0, y0, x1, y1, score}`, `TaskResult`. | Add `Person`; `person_index: Option<u32>`; `TaskMask.mask_box`; `PersonListEntry.subject_fraction`, `in_subject`. |
| Server | `rust/puerh_mind/src/server/segmentation.rs:365-425` | Maps the task result; `task_from_request` reads `person_indices`. | Map the new fields; read `person_index`. |
| Proto | `rust/puerh_mind/proto/segmentation.proto:159-212` | `SegmentationTask` 0-4; `RunSegmentationTaskRequest.person_indices = 4`; `SegmentationPerson{index, box, score}`; response fields 1-15. | E10, E11, E8 fields (section 6). |
| Message limit | `rust/puerh_mind/src/service/registry.rs:28`, `alcedo_studio/src/sidecar_client/segmentation_client.cpp:25` | 64 MB in both directions. | No change: the largest new response is 4 MB plus the People list. |
| C++ DTO | `alcedo_studio/src/include/sidecar_client/dto/segmentation.hpp:90-146` | `SegmentationTask` 1-4, `SegmentationPerson{index, box, score}`, `SegmentationTaskResult` without a box. | `kPerson = 5`; person `subject_fraction`, `in_subject`; result `mask_box`. |
| C++ client | `alcedo_studio/src/include/sidecar_client/segmentation_client.hpp:47-49`, `alcedo_studio/src/sidecar_client/segmentation_client.cpp:225-262` | `RunTask(session_id, task, person_indices, timeout)`. | `RunTask(session_id, task, std::optional<uint32_t> person_index, timeout)`. |
| App service | `alcedo_studio/src/include/app/ai_mask_segmentation_service.hpp:78`, `alcedo_studio/src/app/ai_mask_segmentation_service.cpp:90-102` | `RunTask(task, person_indices = {})` on the current session. | `RunTask(task, std::optional<uint32_t> person_index = std::nullopt)`. |
| Rust tests | `engine.rs:1439` `run_task_computes_head_and_people_once_per_entry`, `:1479` `run_task_people_selection_unions_selected_masks`, `:1524` `run_task_people_keeps_best_box_below_list_threshold`, `:1541` `run_task_rejects_invalid_task_and_index`; `server/segmentation.rs:736-777` `server_maps_people_list_and_binary_mask` | SAM 2.1 People masks and index selections. | Replace with the tests of section 8. |
| Fakes | `rust/puerh_mind/src/service/segmentation/test_support.rs:50-171` | `FakeCalls` counts encoder, decoder, head, and S3OD runs; the fake S3OD returns a constant alpha. | Record the first input value of each S3OD run so a test can tell a crop run from a photo run. |
| Port check | `rust/puerh_mind/tests/segmentation_port_check.rs:362-433`; `scripts/ai_models/sam2/make_segmentation_port_reference.py:184-204` | People: person logits and unions against the reference with IoU >= 0.999. | Person crop alphas, `subject_fraction`, `in_subject` (section 8.3). |
| C++ tests | `alcedo_studio/tests/app/segmentation_client_test.cpp:119-153`, `:331-358`; `alcedo_studio/tests/app/ai_mask_segmentation_service_test.cpp:85`, `:260-266` | `kPeople` with indices; `kBinary` People mask. | Section 8.2. |
| Lab | `alcedo-ai-mask-lab` `scripts/poc/ai_mask_mvp/mask_pipeline.py:628-662` (`select_people`), `:425-447` (`PeopleResult.mask`); `ai_mask_app.py:502-520` (People list panel) | One SAM 2.1 box decode per listed person; the panel selects one person or all people. | Section 7.1. |

---

## 5. Target architecture

### 5.1 Rust owners

- `task_masks.rs`:
  - `person_crop_rect(person: &PersonBox, width: usize, height: usize) -> PixelRect` (E3).
  - `person_crop_image(rgb8: &[u8], width: usize, height: usize, rect: PixelRect) ->
    Result<SubjectInputImage>` (E4). The type `SubjectInputImage` holds any RGB8 image of a long
    edge of at most 1024.
  - `subject_fraction(alpha: &SubjectAlpha, person: &PersonBox) -> f32` (2.3).
  - `PERSON_IN_SUBJECT_FRACTION: f32` (E8) and `PERSON_CROP_MARGIN: f32 = 0.1` (E3).
- `image_entry.rs`:
  - `ImageEntry` fields: `width`, `height`, `features`, `subject_input`, `head:
    Arc<HeadOutput>`, `people: PeopleList`, `person_alphas: Vec<ResultSlot<SubjectAlpha>>`, and the
    slots `sky` and `subject`.
  - `PeopleList { presence, presence_threshold, persons: Vec<PersonCrop>, message }`.
  - `PersonCrop { person: PersonBox, rect: PixelRect, image: SubjectInputImage }`.
  - The head and the People list are written once in `ImageEntry::new` and are read only. The
    slot rule of C7 stays for `sky`, `subject`, and each person alpha. The engine does not hold two
    slot mutexes at the same time.
- `engine.rs`:
  - `start_session`: after the encoder, `head.run(&features)`, `people_list_of(head)`, the crops,
    then `ImageEntry::new`.
  - `subject_alpha_of(loaded, input: &SubjectInputImage) -> SubjectAlpha`: one S3OD run, used by
    the Subject slot and by each person slot.
  - `people_task(loaded, entry)` and `person_task(loaded, entry, index)`.
- The server maps `TaskResult` to the response (section 6).

### 5.2 C++ owners

- `SegmentationClient::RunTask(session_id, task, person_index, timeout) -> SegmentationTaskResult`
  sets `person_index` only when it has a value. It maps `mask_box`, `subject_fraction`, and
  `in_subject`.
- `AiMaskSegmentationService::RunTask(task, person_index)`: fails without a sidecar call when no
  session exists or the service is closed (as SM3.C).

### 5.3 Primary success call chain (People, then add person 2)

```text
AiMaskSegmentationService::StartImage (pixels, key)
  -> StartSegmentationSession -> SegmentationEngine::start_session
       -> encoder -> head.run -> decode_people -> people_list
       -> person_crop_rect + person_crop_image per person -> ImageEntry::new -> cache insert
AiMaskSegmentationService::RunTask(kPeople)
  -> RunSegmentationTask(PEOPLE) -> SegmentationEngine::run_task -> people_task
       -> result_of(subject slot, subject_alpha_of(photo input))   one S3OD run per entry
       -> subject_fraction, in_subject per person
       -> TaskResult{found, ALPHA, Subject alpha, mask_box (0,0,1,1), People list}
AiMaskSegmentationService::RunTask(kPerson, 2)
  -> RunSegmentationTask(PERSON, 2) -> person_task
       -> result_of(person_alphas[2], subject_alpha_of(crop 2 input))   one S3OD run per person
       -> TaskResult{found, ALPHA, crop alpha, mask_box = crop rectangle, People list}
SM5.2 session: step Add person 2 = max(current coverage, pasted crop alpha)
```

### 5.4 Primary failure call chain

```text
RunTask(kPerson, 2) -> person_task -> S3OD run fails (DirectML error)
  -> SegmentationError::Inference(message) -> header INTERNAL with the message
  -> person_alphas[2] stays empty; the next request for person 2 runs S3OD again
  -> C++: SegmentationTaskResult.status.ok = false, error = message
  -> SM5.2 makes no step; the session state does not change
```

---

## 6. File and API map

| File | Change |
| --- | --- |
| `rust/puerh_mind/proto/segmentation.proto` | `SEGMENTATION_TASK_PERSON = 5`. `RunSegmentationTaskRequest`: `reserved 4; reserved "person_indices"; optional uint32 person_index = 5;`. `SegmentationPerson`: `float subject_fraction = 4; bool in_subject = 5;`. `RunSegmentationTaskResponse`: `SegmentationBox mask_box = 16;`. Comments of `SegmentationTaskMaskKind`: `BINARY` Sky only; `ALPHA` with `mask_box`. |
| `rust/puerh_mind/src/service/segmentation/task_masks.rs` | `PixelRect`, `person_crop_rect`, `person_crop_image`, `subject_fraction`, the two constants, unit tests. |
| `rust/puerh_mind/src/service/segmentation/image_entry.rs` | Section 5.1. |
| `rust/puerh_mind/src/service/segmentation/engine.rs` | Section 5.1; `TaskRequest.person_index`; `SegmentationTask::Person`; `TaskMask.mask_box`; `PersonListEntry` fields; remove `compute_people` and the People union. |
| `rust/puerh_mind/src/service/segmentation/test_support.rs` | `FakeCalls.subject_first_inputs: Mutex<Vec<f32>>`. |
| `rust/puerh_mind/src/server/segmentation.rs` | `task_from_request` with `person_index`; response mapping; tests. |
| `rust/puerh_mind/tests/segmentation_port_check.rs` | People section of section 8.3. |
| `scripts/ai_models/sam2/make_segmentation_port_reference.py` | People reference: Subject alpha, crop rectangles, crop alphas, fractions. |
| `alcedo_studio/src/include/sidecar_client/dto/segmentation.hpp` | `kPerson`, `SegmentationPerson::subject_fraction`, `in_subject`, `SegmentationTaskResult::mask_box`. |
| `alcedo_studio/src/{include/,}sidecar_client/segmentation_client.{hpp,cpp}` | `RunTask` with `std::optional<uint32_t>`. |
| `alcedo_studio/src/{include/,}app/ai_mask_segmentation_service.{hpp,cpp}` | `RunTask` with `std::optional<uint32_t>`. |
| `alcedo_studio/tests/app/{segmentation_client_test,ai_mask_segmentation_service_test}.cpp` | Section 8.2. |
| Lab `scripts/poc/ai_mask_mvp/{mask_pipeline,ai_mask_app}.py`, new `scripts/poc/ai_mask_mvp/check_people_from_s3od.py`, new `docs/exploration/sm3d_people_from_s3od.md` | Section 7.1. |

No model file changes. The model directory of SM3.C serves SM3.D.

---

## 7. Implementation phases

SM3.D is one pull request in the Alcedo Studio repository and one in the lab repository. The proto
change removes `person_indices`, so the Rust part and the C++ part must change together.

### 7.1 SM3.D.1 — Lab rule, user check, Rust service, and port check

**Objective and deliverables.** The lab MVP gives People from S3OD; the user accepts the result by
eye; the sidecar serves People and Person by sections 2.3 and 2.4; the port check passes on
DirectML and on the CPU.

**Inputs.** SM3.C on `main`; the lab repository at `D:\Projects\alcedo-ai-mask-lab` (commit
`2a33061`); the SM3.C model directory and the 14 quality photos (`input_4096`).

**Steps.**

1. Lab: in `mask_pipeline.py`, add `person_crop_rect`, `person_crop_image`, `person_alpha`
   (S3OD on the crop with `s3od.letterbox`, `select_mask`, `unpad`), and `subject_fraction` with
   the arithmetic of E3, E4, and 2.3. Change `select_people` to the rule of section 2.3: no
   decoder call; keep the People list rules C2 and C3 (the C2 best-box rule is added to the lab).
   Add `PeopleSteps`, an ordered list of (person rank, Add or Subtract), and `PeopleSteps.mask`,
   which composes by section 2.5 from the Subject alpha at the photo size.
2. Lab: in `ai_mask_app.py`, the People panel shows the Subject alpha and the boxes; a box shows
   included when `in_subject`; a click on a box adds or subtracts that person by E8 and
   section 2.5. Undo removes the last step.
3. Lab: `check_people_from_s3od.py` writes, for each quality photo with a non-empty People list,
   one page under `build/tmp/explore/sm3d/` of the lab: the photo with the Subject alpha overlay
   and the boxes (green included, white not included), each person crop alpha pasted on the photo,
   the old SAM 2.1 box mask for comparison, and a table of `subject_fraction`, the lab rule of E8,
   and the S3OD times. It computes the value of E8 over all listed people of these photos. The
   photos include the group photo `DSC00527` and the paddle boarder `2088064441`.
4. User check by eye (manual verification): the user reviews the pages. Record the result in the
   lab report `docs/exploration/sm3d_people_from_s3od.md` and in section 12. If the user rejects
   the person edges, stop and open an issue with the pages; do not port the rule.
5. Rust: `task_masks.rs` functions and constants (section 5.1) with unit tests; set
   `PERSON_IN_SUBJECT_FRACTION` to the value of step 3.
6. Rust: `image_entry.rs` and `start_session` (E4, E5). Sky reads `entry.head`.
7. Rust: `subject_alpha_of`, `people_task`, `person_task`; `run_task` validation (E11); remove
   `compute_people`, `PersonMask`, and the People branch of `union_mask`.
8. Proto and server mapping (section 6); server tests.
9. Port reference: the People part writes the Subject alpha (already written for Subject), and
   per listed person the crop rectangle, the crop alpha (`.f32` on the crop grid), the score,
   `subject_fraction`, and `in_subject`. It uses the lab functions of step 1.
10. Port check: section 8.3 on DirectML and on the ONNX Runtime CPU device.
11. Measurements (section 9).

**Exit criteria.** Step 4 accepted by the user; all tests of section 8.1 and 8.3 pass; the
measurements are recorded.

**Expected diff.** Alcedo Studio: 1,050-1,350 lines (Rust production 300-400, Rust tests
450-550, port check 120-180, Python reference 80-120, documents 100). Lab: 300-450 lines in the lab
repository.

### 7.2 SM3.D.2 — C++ client, app service, and macOS

**Objective and deliverables.** The C++ client and the app service carry the Person task and the
new fields; the macOS checks of SM3.C and SM3.D pass on the Mac.

**Steps.**

1. DTO, `SegmentationClient::RunTask`, `AiMaskSegmentationService::RunTask` (section 5.2); update
   the fake server and the tests (section 8.2).
2. macOS: section 10.3.
3. Completion record (section 12); master plan status.

**Exit criteria.** C++ tests pass on Windows; the macOS Rust tests and the macOS port check pass
with the Core ML models on the devices `""` and `cpu`; the SM3.C macOS record is written in the
SM3.C plan section 13.2.

**Expected diff.** 250-400 lines (C++ 120-180, C++ tests 100-150, documents 50).

---

## 8. Tests

### 8.1 Rust

| Name | Observable assertion |
| --- | --- |
| `person_crop_rect_adds_margin_and_clips_to_image` | Box (0.2, 0.3)-(0.4, 0.7) on 1000 x 500 gives pixels x 180-420, y 130-370; a box at the left edge starts at x 0; a box past the right edge ends at the width. |
| `person_crop_rect_is_at_least_one_pixel` | A box of width 0 gives a rectangle of width 1. |
| `person_crop_image_keeps_small_crops_and_resizes_large_crops` | A 300 x 600 crop keeps its pixels; a 2000 x 3000 crop equals `subject_input_image` of the cut pixels (683 x 1024). |
| `subject_input_of_stored_crop_equals_one_resize` | For a 2000 x 3000 crop, `subject_input_image` of the stored crop equals `subject_input_image` of the cut pixels, byte for byte. |
| `subject_fraction_counts_alpha_inside_box` | An alpha with a 10 x 10 inside square and a box over 10 x 20 pixels gives 0.5; `in_subject` follows the constant. |
| `start_session_runs_head_and_cuts_person_crops` | Two listed people: head runs 1 at the start; the entry has 2 crops with the rectangles of E3; S3OD runs 0. |
| `start_session_without_people_keeps_no_crops` | Presence below the threshold: no crops; People returns not found with the SM3.C message; S3OD runs 0. |
| `head_failure_at_start_fails_start_and_caches_nothing` | Fake head fails: `Inference`; a key-only start of the same key reports `ImageNotCached`. |
| `run_task_people_returns_subject_alpha_and_list_without_decoder` | People: `ALPHA`, the Subject grid, `mask_box` (0, 0, 1, 1), the list with fractions; decoder calls 0; S3OD runs 1. |
| `run_task_people_shares_subject_alpha_with_subject_and_background` | Subject, Background, then People: S3OD runs 1; the People alpha equals the Subject alpha. |
| `run_task_people_found_when_subject_alpha_is_empty` | Fake alpha 0.2 and two people: `found` true, `mask_area` 0, both `in_subject` false. |
| `run_task_person_runs_s3od_once_per_person` | Person 1 twice, then person 0: S3OD runs 2 more than after People; the second request for person 1 has `model_ms` 0; the recorded first input values differ from the photo run. |
| `run_task_person_returns_crop_grid_and_mask_box` | The alpha size equals the resized crop size; `mask_box` equals the crop rectangle divided by the image size. |
| `run_task_person_not_found_when_crop_alpha_below_half` | Fake alpha 0.49 for the crop run: `found` false with the E9 message; a second request runs no S3OD. |
| `run_task_person_failure_keeps_no_alpha` | Fake S3OD fails once on a crop: `Inference`; the next request runs S3OD again and succeeds. |
| `run_task_rejects_person_index_rules` | `PERSON` without an index, `SKY` with an index, `PEOPLE` with an index, index 5 of 2 people: `InvalidInput` before any model run. |
| `cached_key_only_start_runs_person_from_stored_crop` | Start with key and pixels, end, key-only start, Person 0: S3OD runs once; the alpha equals the alpha of a pixel start. |
| `run_task_sky_reads_stored_head` | Sky after a start: head runs 1 in total (at the start). |
| `server_maps_people_alpha_list_and_fractions` | People: `ALPHA`, `alpha_values`, `mask_box`, `subject_fraction`, `in_subject`. |
| `server_maps_person_alpha_and_mask_box` | Person: the crop alpha and its box; `found` false gives the message. |
| `server_rejects_person_without_index` | `PERSON` without `person_index`: header `INVALID_ARGUMENT`; no engine call. |

The SM3.C tests `run_task_computes_head_and_people_once_per_entry`,
`run_task_people_selection_unions_selected_masks`, and `server_maps_people_list_and_binary_mask`
are removed; `run_task_people_keeps_best_box_below_list_threshold` and
`run_task_rejects_invalid_task_and_index` are changed to the new People request.

### 8.2 C++

| Name | Observable assertion |
| --- | --- |
| `SegmentationClientTest.RunTaskCarriesTaskAndPersonIndex` | `kPerson` with index 1: the server sees `PERSON` and `has_person_index()` with 1; `kPeople` with no index: `has_person_index()` false. |
| `SegmentationClientTest.RunTaskMapsAlphaMaskBoxAndSubjectFraction` | The DTO receives `kAlpha`, the size, the alpha values, `mask_box`, and each person `subject_fraction` and `in_subject`. |
| `SegmentationClientTest.RunTaskNotFoundIsSuccessWithMessage` | Unchanged. |
| `AiMaskSegmentationServiceTest.RunTaskWithoutSessionFailsWithoutCall` | Unchanged. |
| `AiMaskSegmentationServiceTest.RunTaskUsesCurrentSession` | The fake client receives the session id, `kPerson`, and index 2; `kSubject` with no index. |

### 8.3 Port check (`segmentation_port_check_matches_reference_masks`, People part)

Per photo against the reference of step 9:

- People list length, order, and boxes within 1e-3 cell (unchanged).
- People alpha: equal to the Subject alpha of the same run.
- Each listed person: the crop rectangle equal in pixels; the crop alpha >= 0.5 with an IoU of at
  least 0.998 on the crop grid (the SM3.C tolerance of the Subject alpha on DirectML float16);
  `subject_fraction` within 0.01; `in_subject` equal when the reference fraction is more than 0.01
  from the constant.
- A key-only start of a cached photo gives the same person alphas.

---

## 9. Measurements

Windows, RTX 3080 Laptop, DirectML, release build; macOS, Apple Silicon, native Core ML. Record the
values; they are not acceptance limits.

| Item | Method | Expected range |
| --- | --- | --- |
| Start time with pixels (encoder, head, crops) | Port check `encoder_ms` against the SM3.C record | SM3.C value plus 2 to 30 ms |
| CPU time of the crops at the start (`DSC00527`, 20 people) | Port check log | under 100 ms |
| Bytes of the stored crops per entry (largest photo, `DSC00527`) | Port check log | at most 3 MB per person |
| First People request of a photo (one S3OD run) | Port check | about 210 ms DirectML, about 530 ms Core ML |
| Person request, first and from the entry | Port check | about 191 ms and 3 ms DirectML; about 531 ms and 3 ms Core ML |
| Dedicated GPU memory after People and 5 Person requests (Windows) | `Get-Counter "\GPU Process Memory(pid_<pid>*)\Dedicated Usage"` | SM3.C peak 2,955 MB; above 3 GB is a finding (issue) |

---

## 10. Build and run commands

### 10.1 Windows Rust

```powershell
Set-Location D:\Projects\pu-erh_lab\rust\puerh_mind
cmd /c "script\cargo_msvc.cmd test --release -- --skip live_"
D:\Projects\alcedo-ai-mask-lab\build\tmp\explore\venv\Scripts\python.exe -I `
  D:\Projects\pu-erh_lab\scripts\ai_models\sam2\make_segmentation_port_reference.py `
  --onnx-dir <model directory> --photos <lab input_4096 photos> --prompts <lab prompts.json> `
  --head-onnx <model directory>\people_sky_head.onnx --head-json <model directory>\people_sky_head.json `
  --subject-onnx <model directory>\subject_masks.onnx --out D:\Projects\pu-erh_lab\build\tmp\sm3d\port_reference
$env:ALCEDO_SEGMENTATION_MODEL_DIR = "<model directory>"
$env:ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3d\port_reference"
$env:ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE = "directml"
cmd /c "script\cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture"
$env:ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE = "cpu"
cmd /c "script\cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture"
```

### 10.2 Windows C++

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SegmentationClientTest AiMaskSegmentationServiceTest --parallel 4
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;" + $env:PATH
ctest --test-dir build/debug -R "SegmentationClientTest|AiMaskSegmentationServiceTest" --output-on-failure
```

### 10.3 macOS (SM3.C and SM3.D checks in one run)

Connect to `yurunmac.local` with the Windows OpenSSH client from the PowerShell tool. Copy
`build/tmp/sm3d/port_reference` to the Mac. The reference contains the SM3.C prompt, Sky, and
Subject masks and the SM3.D People masks, so one port check run covers the open SM3.C macOS item
and SM3.D.

```bash
cd rust/puerh_mind && cargo test --release -- --skip live_
ALCEDO_SEGMENTATION_MODEL_DIR=<directory with the .mlmodelc models and people_sky_head.json> \
ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR=<copy of the port reference> \
ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE="" cargo test --release --test segmentation_port_check -- --nocapture
ALCEDO_SEGMENTATION_MODEL_DIR=<same> ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR=<same> \
ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE=cpu cargo test --release --test segmentation_port_check -- --nocapture
cmake --build build/macos-debug --target SegmentationClientTest AiMaskSegmentationServiceTest
ctest --test-dir build/macos-debug -R "SegmentationClientTest|AiMaskSegmentationServiceTest" --output-on-failure
```

`build/macos-debug` must be configured with `-DALCEDO_BUILD_TESTS=ON`.

---

## 11. Repository rules that apply

Read `AGENTS.md` and the applicable skills again before implementation.

- No fallback: a failed head at the start is a start error; a failed S3OD run returns the real
  error; no SAM 2.1 mask replaces a person alpha; no other device after an error (A7).
- The head output, the People list, and the crops are owner state of the image entry, written once
  at its creation. No generation or token: the entry lifetime equals the image and the model set.
- Decide detail that this plan does not state, record it in section 12, and do not leave open
  decisions. Open one GitHub issue for each problem found during the work.
- Run MSVC builds through PowerShell with `scripts\msvc_env.cmd`; run `cargo` through
  `script\cargo_msvc.cmd`.
- Temporary files under `build/tmp/sm3d/`; lab check pages under the lab `build/tmp/explore/sm3d/`.
- Only targeted test runs; no full `ctest` run.
- LF line endings; check `git diff --stat` after bulk edits.
- Prohibited terms: check the new code and documents before completion.

## 12. Completion record

### 12.1 SM3.D completion record (2026-10-23)

**Status:** SM3.D.1 and SM3.D.2 are implemented. Windows: all tests pass; the port check passes on
DirectML and on the ONNX Runtime CPU device. macOS: all tests pass; the port check passes on Core
ML `All` (section 12.3).

**Source revision and branch:** `feature/ai-segmentation-sm3d` on `2722a740` (`main`). Lab:
`feature/people-from-s3od` of `alcedo-ai-mask-lab` on `2a33061` (commits `e34924f`, `9d069be`,
`21acc07`).

**User check (manual verification, 2026-10-23):** the user reviewed the check pages of section 7.1
step 3 (9 photos with people, 53 listed people, including `DSC00527` and `2088064441`) and accepted
the person edges. The findings that the user saw with the pages: small out-of-focus people next to a
sharp subject (`DSC_1535` persons 4, 5, 6) give the alpha of a part of the subject (issue #475), and
3 small people have no crop alpha >= 0.5 (E9). Lab report:
`docs/exploration/sm3d_people_from_s3od.md` of the lab.

**Final values:** `PERSON_CROP_MARGIN` 0.1 (no change after the check). `PERSON_IN_SUBJECT_FRACTION`
0.15: 48 of the 50 people with a crop alpha agree with the lab rule of E8 (0.05 to 0.30: 47; 0.50:
44; 0.70 and higher: 35). The two people that do not agree are `DSC_1535` persons 4 and 5 (the crop
alpha is the arm of the subject).

**Decisions made during the work:**

| Item | Decision | Reason |
| --- | --- | --- |
| Pixel edges of E3 and of `subject_fraction` | `floor(v * size + 1e-4)` and `ceil(v * size - 1e-4)`, computed in float64 from the float32 box corners, in the lab and in Rust; `PERSON_CROP_MARGIN` is float64. | The box corners are float32 values. Without the tolerance, the box (0.2, 0.3)-(0.4, 0.7) of section 8.1 gives x 180 to 421, not 420. The same float64 arithmetic in both programs gives the same rectangles: all 53 crop rectangles are equal in pixels on DirectML, CPU, and Core ML. |
| People list of a Person result | The entries carry `subject_fraction` and `in_subject` of the stored Subject alpha; 0 and false when the image has no Subject alpha yet. | The test `cached_key_only_start_runs_person_from_stored_crop` (section 8.1) allows one S3OD run, so a Person request cannot run S3OD on the photo. The host reads the included state from the People result (SM5.2). |
| `mask_box` | Part of the `Alpha` mask value (`TaskMaskValues::Alpha { values, mask_box }`), not a separate optional field. | Only an alpha grid has a box (E10); the type keeps that rule. |
| S3OD input resize (issue #474) | `subject_input_image` now gives the bytes of `cv2.resize(..., INTER_LINEAR)`: the vertical pass uses the OpenCV SIMD rule `((((a >> 4) * w0) >> 16) + (((b >> 4) * w1) >> 16) + 2) >> 2`, the vertical weights at the image edges are not clamped (only the rows), and each weight is rounded separately, ties to even. | The first port check failed on 22 of 53 person crops on DirectML (minimum IoU 0.616) and differed on the CPU too. The SM3.C resize was off by one in 6 % to 15 % of the values; whole photos tolerate that, small upscaled crops do not. The new resize is bit-exact on the 53 crops, the 14 photos, and 300 random sizes; the Subject grid IoU on the CPU also rose from 0.998423 to 0.999445. |
| Port check criterion of the person crop alpha (section 8.3) | 0.998 on the CPU devices, as planned. On the float16 devices (DirectML, Core ML `All`): IoU >= 0.99, or at most 0.05 % of the crop grid pixels different. | With the bit-exact input, DirectML gives IoUs down to 0.9926, and onnxruntime-directml in Python on the same inputs gives the same values (a temporary check script on the port reference crops). Small upscaled crops have wide edges near 0.5. `DSC_1535` person 5 (the subject arm, 1,719 inside pixels) has IoU 0.954 with 82 different pixels (0.022 %). The plan assumed the Subject tolerance; it does not hold for float16 crop runs. |
| Lab measurement scripts of issues #5 and #8 | The old People rule stays in the lab as `select_people_sam_boxes`, used only by `compare_providers.py` and `compare_with_measurement_scripts.py`. | These scripts reproduce recorded results of earlier issues. The People tool and the check use the new rule. |

**Actual changed modules and diff:** Alcedo Studio about 2,230 changed lines (1,753 added, 473
removed): Rust production about 560 (task rules, image entry, engine, server, proto), Rust unit tests
about 700, port check 345, Python reference 77, C++ 90 and its tests 86, documents. Lab: 2,700
added lines, of which 1,879 are the result JSON (scripts 700, report 122). The estimate (1,300 to
1,750) was low because of the engine tests and the port check.

**Primary success call chain:**

```text
AiMaskSegmentationService::StartImage(pixels, key)
  -> SegmentationClient::StartSession -> StartSegmentationSession -> SegmentationEngine::start_session
       -> sam2::preprocess_image, task_masks::subject_input_image, encoder
       -> head.run -> people_list_of (presences, decode_people, people_list,
          person_crop_rect + person_crop_image per person) -> ImageEntry::new -> cache insert
AiMaskSegmentationService::RunTask(kPeople)
  -> RunSegmentationTask(PEOPLE) -> run_task -> validate_task_request -> people_task
       -> photo_subject_alpha (subject slot, subject_alpha_of: one S3OD run per entry)
       -> person_list_entries (subject_fraction, in_subject)
       -> TaskResult{Alpha{Subject alpha, mask_box (0, 0, 1, 1)}, People list}
AiMaskSegmentationService::RunTask(kPerson, 2)
  -> RunSegmentationTask(PERSON, 2) -> person_task
       -> person_alphas[2] slot: subject_input_image(stored crop) -> subject_alpha_of (one S3OD run)
       -> TaskResult{Alpha{crop alpha, mask_box = crop rectangle}, People list} or E9 message
```

**Primary failure call chain:**

```text
start_session -> head.run fails -> SegmentationError::Inference -> header INTERNAL; no session, no
  cache entry (a key-only start reports IMAGE_NOT_CACHED)
person_task -> S3OD fails -> Inference -> person_alphas[i] stays empty; the next request runs S3OD
  again -> C++ SegmentationTaskResult.status.ok = false with the runtime message
run_task(PERSON without index | other task with index | index outside the list) -> InvalidInput
  before any model run -> header INVALID_ARGUMENT
```

### 12.2 Windows verification

| Test | Result |
| --- | --- |
| `cargo test --release -- --skip live_` | 365 + 4 + 5 + 1 passed, 0 failed, 0 warnings (`main`: 346 + 4 + 5 + 1 at SM3.C; the SM3.C People tests were replaced as section 8.1 says) |
| New and changed Rust tests of section 8.1 (`task_masks` 6 including `subject_input_image_matches_opencv_resize_bit_for_bit`, `engine` 16, `server` 3) | all passed |
| Port check, DirectML, 2 runs | passed: 201 compared masks; prompt masks minimum photo IoU 0.999973; Sky unions IoU 1.0; Subject grid IoU at least 0.998520; People alpha equal to the Subject alpha; 53 crop rectangles equal; person crop IoU at least 0.9926 (one crop at 0.954 with 0.022 % different pixels); fractions within 0.01; key-only starts give the stored person alphas |
| Port check, ONNX Runtime CPU | passed with the planned 0.998: person crop IoU at least 0.999330; Subject at least 0.999445; the 3 "not found" persons agree with the reference |
| `cargo clippy --release --all-targets` | no new finding: 70 warnings and the `semantic.rs` error on `main` and on the branch (issue #476) |
| C++ `SegmentationClientTest`, `AiMaskSegmentationServiceTest` (`win_debug`) | 22 of 22 passed (`RunTaskCarriesTaskAndPersonIndex`, `RunTaskMapsAlphaMaskBoxAndSubjectFraction`, `RunTaskUsesCurrentSession` changed or new) |

**Measurements (Windows, RTX 3080 Laptop, DirectML, release):**

| Item | Value | Expected |
| --- | ---: | --- |
| Start with pixels (preprocessing, encoder, head, crops), port check p50 | 149 to 154 ms (first 249 to 278) | SM3.C value plus 2 to 30 ms (SM3.C: 206 to 215 ms with task requests) |
| CPU time of the crops at the start, `DSC00527` (20 people); largest photo | 1.1 ms; 15 ms (`DSC00674`, one 2731 x 4096 crop resized) | under 100 ms |
| Bytes of the stored crops: `DSC00527` in total; largest crop of one person | 2.91 MB; 2.12 MB (`DSC00326`) | at most 3 MB per person |
| First People request of a photo (one S3OD run), p50 | 212 to 215 ms (first in the process 251 to 299) | about 210 ms |
| Person request, first, p50 (53 people); from the entry, p50 of all stored requests | 206 ms (202 to 228); 0.37 to 0.43 ms | about 191 ms and 3 ms |
| Dedicated GPU memory, peak of the port check (People and 53 Person requests) | 2,955 MB | below 3 GB (SM3.C peak 2,955 MB) |

### 12.3 macOS verification

Mac: Apple Silicon, macOS 27.0.1, rustc 1.90.0, `yurunmac.local`. Source: `origin/main`
(`2722a740`) with the patch of this branch. Models: `point_encoder.mlmodelc`,
`point_decoder.mlmodelc`, `people_sky_head.mlmodelc` with its JSON file (SHA-256 `43981e9e…`, the
same file as on Windows), and `s3od.mlmodelc` (`float16_embeddings_attention_float32`) from the
lab, against a copy of the Windows port reference `build/tmp/sm3d/port_reference`.

| Test | Result |
| --- | --- |
| `cargo test --release -- --skip live_` | 368 + 4 + 5 + 1 passed, 0 failed (3 more tests than on Windows: the macOS-only tests) |
| C++ `SegmentationClientTest`, `AiMaskSegmentationServiceTest` (`macos-debug`) | 22 of 22 passed |
| Port check, Core ML `All` (device `""`, `coreml-native:all`), 2 runs | passed with the float16 rule of section 12.1: 201 compared masks; prompt masks minimum photo IoU 0.999964; Sky unions IoU at least 0.999999; Subject grid IoU at least 0.998243; Background at least 0.999960; People alpha equal to the Subject alpha; 53 crop rectangles equal to the reference; person crop IoU at least 0.994048 except `DSC_1535` person 5 (0.909250, the subject arm; inside the 0.05 % share); fractions within 0.01. The first run with the CPU criterion 0.998 failed on 3 crops (`DSC00527` person 19, `DSC00562` person 5, `DSC_1535` person 5). |
| Port check, Core ML `CPUOnly` (device `cpu`) | failed (issue #477): 6 person crops below 0.998 and above the 0.05 % share (minimum `DSC_1456` person 1, IoU 0.908918, 1.34 % different) and `Nikon-D800e-raw-00007` Subject grid IoU 0.996392. The app uses Core ML `All`, not this device. The prompt masks, Sky, and Background pass (minimum photo IoU 0.999973). |

This port check also is the open SM3.C macOS port check (SM3.C plan, section 13.2): the SM3.C
tasks pass on Core ML `All`.

**Measurements (macOS, Core ML `All`, release, port check):**

| Item | Value | Expected |
| --- | ---: | --- |
| Start with pixels, encoder p50 | 424 to 430 ms | not specified for macOS |
| Decoder p50 (1 to 4 points) | 41 to 44 ms | — |
| CPU time of the crops at the start, `DSC00527`; largest photo | 0.6 ms; 11 ms (`P1024439`) | under 100 ms |
| Bytes of the stored crops | equal to Windows (same rectangles and resize) | at most 3 MB per person |
| First People request of a photo (one S3OD run), p50 | 964 and 1,012 ms | about 210 ms (Windows value); issue #478 |
| Person request, first, p50 (53 people) | 1,030 and 1,024 ms | issue #478 |
| Core ML `CPUOnly`: People p50; Person p50 | 3,348 to 3,361 ms; 3,405 to 3,444 ms | — |

### 12.4 Issues

- #474: S3OD input resize differs from OpenCV (fixed by this phase).
- #475: a click on a small blurred person next to the subject subtracts a part of the subject.
- #476: `cargo clippy --all-targets` stops on an always-true assertion in the semantic server test.
- #477: the port check fails on the Core ML `CPUOnly` device (Subject and person crops).
- #478: S3OD on Core ML takes about 1.0 s for each People or Person request (Windows: about 0.2 s).
