# SM5 AI Mask Source, R8 Asset Store, and Edit Session — Phase Plan

Date: 2026-10-23.

Status: SM5.1 complete (2026-10-23, section 13.1). SM5.2 complete (2026-10-23, section 13.2).
SM5.3 complete (2026-10-23, section 13.3).

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), section
9.4. Issues: SM5.1 #470, SM5.2 #471, SM5.3 #472. Tracking issue: #468.

Prerequisites:

- SM4 ([phase plan](phase_sm4_analysis_render_plan.md), merged in `1015ff4e`): the analysis image,
  its image key, and the prompt coordinates in normalized ReferenceSpace.
- SM3.D ([phase plan](phase_sm3d_people_masks_from_s3od_plan.md)): the People and Person task
  results. SM5.2 needs SM3.D for the People component. SM5.1 does not need it.

Source revision used for the audit: `1015ff4e` (`main`).

---

## 1. Decision record

### 1.1 User request (2026-10-23)

- An AI Mask edit has many small steps: strokes, add and subtract points, boxes, the task masks,
  the People boxes, Feather, Expand, and the change between the prompt input and the task input.
  These steps must not go into the main history. One AI Mask edit session keeps its own temporary
  history. The session publishes one main history operation when it ends. Main history Undo and
  Redo then work on whole AI Mask edits.
- The R8 coverage of each session must stay in the project, so that main history Undo can go back
  to an earlier Mask. The design must not leave much unused data and must not need an extra cache
  folder.
- People masks come from S3OD, not from SAM 2.1 box prompts (SM3.D). The People task starts with
  the full-image S3OD alpha. A click on a person box adds or removes that person. Overlapping
  neighbors get the normal Add and Subtract order, with no special rule.

### 1.2 Approved decisions

These decisions are master plan D16 to D22. P2, P3, and P6 of the master plan are approved.

| ID | Decision |
| --- | --- |
| D16 | Two-level history. The AI Mask edit session keeps an in-memory stack of session states. The live document does not change during the session. Commit publishes one `AddMask` or `ReplaceMaskSource` batch. Cancel publishes nothing. Main history Undo and Redo do not change. |
| D17 | Only a commit writes R8 bytes: one content-addressed `MaskAsset` in a table of the project DuckDB file. The asset holds the composed model coverage before Feather and Expand. Reachability cleanup removes assets that no stored history refers to. |
| D18 | Paste does not copy AI Masks. The copy result names the Masks that it skipped. |
| D19 | The format change is additive: one new table and one new Mask source kind. The project metadata version and the document JSON version do not change. Older projects open without change. An older build reports its real "unknown Mask source kind" error for an image with an AI Mask. |
| D20 | Feather and Expand apply when SM6 makes the coverage texture. The stored R8 does not contain them. |
| D21 | People from S3OD (SM3.D): the People result is the full-image S3OD alpha; a box click adds or removes one person with S3OD on the person crop; ordered Add and Subtract, no neighbor rule. |
| D22 | S3OD stays resident from model load to unload. |
| P2 | Prompts and coverage use normalized ReferenceSpace. |
| P3 | The R8 asset covers only the bounding rectangle of the coverage, plus a border. The maximum axis is `kMaximumRasterMaskAxis` (4096). |
| P6 | Each AI Mask stores a geometry fingerprint of the Lens and RAW decode settings. SM7 marks a Mask whose fingerprint differs from the current settings. |

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| M1 | The source type is `AiSegmentationMaskSource`, kind text `ai_segmentation`, `MaskSourceKind::AiSegmentation = 3`. Fields: `asset_key`, `descriptor` (asset extent and `reference_bounds`), `analysis_extent`, `recipe`, `edge` (Feather and Expand), `model_set_id`, `geometry_fingerprint`. | One source holds every value that rendering (key, descriptor, edge), editing (recipe), and the D9 notice (fingerprint, model set) need. |
| M2 | The recipe types live in `edit/mask/ai_mask_recipe.hpp`: `AiMaskRecipe` is an ordered list of `AiMaskComponent`. A component has an `operation` (Add or Subtract) and a content: a task (`Subject`, `Background`, `Sky`, `People` with ordered person toggles) or a prompt group (ordered prompt steps). The session state and the source use the same types. | One data model for the session and the document. No mirror type. |
| M3 | Prompt group rule. A prompt step goes into the last component when that component is an open prompt group. Otherwise it starts a new group. The tool mode (Add or Subtract) of the step sets the SAM 2.1 point label inside an open group. For a new group, the tool mode sets the group operation and the points get the Add label. A group is open after a prompt step. It closes after a task step, a person step, and an input mode change. | Inside a group, Add and Subtract points are the SAM 2.1 labels that the lab checked (E3). A Subtract stroke on a task mask then removes the region that the stroke selects; SAM 2.1 cannot select from subtract points only. |
| M4 | Each prompt step decodes all points and the box of its group in one decoder call, with the selected logits of the previous step of the group as `mask_input` (master plan 8.2.3). A group holds at most 64 points (SM3.A limit). A step stores its selected level: the decoder `selected_index`, or the level that the user selects. Only a step with one point and no box has three levels. | The replay of a group gives the same masks as the original steps on the same image embedding. |
| M5 | People component. Its base is the Subject alpha of the People result (SM3.D E1). Its person toggles apply in order: Add = maximum, Subtract = minimum with (255 - person). A click on a box makes a Subtract toggle when the person is shown as included, else an Add toggle. A person is shown as included from `in_subject` (SM3.D E8) until its first toggle; after a toggle, the state is the operation of its last toggle. A person result with `found = false` (SM3.D E9) makes no step. | SM3.D owns the rules that read model outputs. The host only composes. |
| M6 | Session coverage is R8 at the session image size. Conversion: SAM 2.1 logits (256 x 256) bilinear to the session size with half-pixel centers, inside where > 0, 255 or 0; an `ALPHA` grid bilinear (half-pixel centers) into its `mask_box` rectangle, `round(clamp(alpha) * 255)`, 0 outside the box (changed by master plan D23: a joint bilateral upsampling guided by the analysis image, see section 13.2.4); Sky `BINARY` as given. Fold of the components in order: Add = maximum, Subtract = minimum with (255 - component). | The master plan rule for SAM 2.1 masks (8.2.3), the SM3.D paste rule (2.5), and the Union rule of D6. |
| M7 | The session keeps each model output in its native grid (logits, alpha grid, Sky R8) in a map keyed by the canonical JSON of the component prefix that produced it. It keeps the session-size coverage only of the components of the current state. A session step that a stack entry already has needs no model call. The stack has no length limit. | A prompt step output is 256 KB. 100 steps need about 25 MB. Session-size coverages of all steps would need up to 16 MB each. |
| M8 | `AiMaskEdgeAdjustment{feather_px, expand_px}`: `feather_px` in [0, 50] (Gaussian sigma), `expand_px` integer in [-50, 50] (dilate when > 0, erode when < 0), both in pixels of the analysis image. Default 0 and 0. The semantics are lab function F8 (`adjust_mask` in the lab `mask_pipeline.py`). | The asset raster has the analysis image resolution, so lab pixels apply without a scale. SM6 applies the function (D20). |
| M9 | Commit crops the session coverage to the bounding rectangle of its values > 0, widened by 1 pixel on each side and clipped to the image. A coverage without a value > 0 is a commit error: "The AI Mask has no coverage." | P3. The zero border gives the bilinear sampling of SM6 a zero edge. An empty Mask has no use. |
| M10 | Table `MaskAsset(asset_key VARCHAR PRIMARY KEY, format_version INTEGER, width INTEGER, height INTEGER, bounds_x FLOAT, bounds_y FLOAT, bounds_w FLOAT, bounds_h FLOAT, pixel_byte_count BIGINT, codec VARCHAR, data BLOB)`, created with `CREATE TABLE IF NOT EXISTS` at every database open. The pixels are compressed with zlib (`compress2`, `Z_DEFAULT_COMPRESSION`), codec text `zlib`. A put of an existing key writes nothing. A read inflates, checks the byte count, and recomputes `MakeMaskAssetKey`. | zlib is already a direct dependency (`find_package(ZLIB REQUIRED)` in the root `CMakeLists.txt`, linked by the image library) on Windows and macOS. R8 masks are long runs of 0 and 255; deflate compresses them well. The `.alcd` package carries only the metadata JSON and the DuckDB file, so the table travels with Save and needs no package change. |
| M11 | Reachability cleanup runs in `ProjectHandler::PersistProjectForClose`, right after `CollectUnreachableEditCommits`. The marked keys come from every remaining `EditCommit.edit_payload`, every `ImageEditState.serialized_pipeline_state`, every `PipelineRoot.serialized_pipeline_state`, and every record of each journal file in the editor journal directory. A recursive JSON walk marks the `asset_key` of each object with `"kind": "ai_segmentation"`. One transaction deletes the rows with other keys. Mark and sweep hold the same database connection lock. | Commit cleanup leaves only reachable commits. The JSON walk finds the key in every batch type (`AddMask`, `RemoveMask`, `ReplaceMaskSource`, `RemoveColorGrade`, node graph changes, `Paste`) without a rule per change type. Assets that a reachable commit refers to stay, because Undo needs them. |
| M12 | Write order: the asset put commits its own DuckDB transaction before the history publish. A failed publish leaves an unreferenced row; M11 removes it at the next clean close. | The journal record and the history row never refer to a missing asset. |
| M13 | `geometry_fingerprint` is 32 lowercase hexadecimal digits of XXH3 128-bit over the canonical JSON of these Develop fields: `demosaic_method`, `highlights_reconstruct`, `lens_enabled`, `apply_distortion`, `apply_tca`, `apply_crop`, `auto_scale`, `use_user_scale`, `user_scale`, `projection_enabled`, `target_projection`, `lens_maker`, `lens_model`. | D9 names Lens and RAW decode. Vignetting and white balance do not move image content. |
| M14 | `StartSegmentationSessionResponse` gains `uint32 image_width = 5` and `uint32 image_height = 6`, the size of the image entry; `SegmentationSessionResult` gains the same fields. The session image size comes from this response for a pixel start and for a key-only start. | A key-only start (B1) has no pixels, so the host does not know the session size without the response. |
| M15 | Opening an existing AI Mask starts a session with the stored recipe and edge. The session shows the stored asset as the current coverage without a model call. The first step that changes the components computes the outputs of all components of the new state (task requests, person requests, prompt group replay). An Undo back to the opened state shows the stored asset again. An `analysis_extent` that differs from the session image size is an open error. A Feather or Expand step needs no model call. | Most reopen edits change only Feather or Expand. A replay result can differ from the stored coverage after a Develop change; D9 covers that case. |
| M16 | New operation lease `EditorOperationLeaseKind::AiMaskEdit`. Open acquires it; Commit and Cancel release it. It blocks `PreviewAdjustment`, `CommitAdjustment`, `Undo`, `Redo`, `MoveHead`, `DiscardChanges`, `CheckoutVersion`, `CreateRootVersion`, `BranchVersion`, `RenameVersion`, `RemoveVersion`, `ApplyPaste`, and `OpenComparison`. View changes and image selection stay available; an image change ends the session first (master plan 3.3). | The live document cannot change under an open session, so Commit needs no conflict check. The lease is the existing admission rule of the editor, as for the comparison. |
| M17 | Paste: `AdjustmentTransferPackageBuilder::Build` does not put AI Masks into the package. When the selected Masks of a Grade are all AI Masks, the Grade entry has no Mask set. `ListAiMasksExcludedFromTransfer(document, selection)` returns the skipped Masks (node id, Mask id, display name). `MaskModelFromJson` inside `ImportPackage` rejects the kind `ai_segmentation`. | D18. No AI Mask JSON enters a package, so the transfer schema `alcedo.adjustment_transfer.v7` does not change. |
| M18 | The owner of the asset store is `PipelineMgmtService` (`PutMaskAsset`, `ReadMaskAsset`, `CollectUnreferencedMaskAssets`). Each call takes the connection guard of the project database at the operation boundary. The editor writes through a new port `IEditorMaskAssetPort`. | Export, thumbnails, and the editor already hold `PipelineMgmtService`. The DuckDB ownership rule of `AGENTS.md` keeps connections per operation. |
| M19 | The JSON of Radial, Linear Gradient, and all other existing values does not change. | A JSON byte change of existing values changes the root id hash and the history byte comparison of old projects. |
| M20 | `edit/mask/mask_asset.cpp` moves from `EditMaskBrush` (built only with `ALCEDO_ENABLE_BRUSH_MASK`) to `EditMaskCore`. | `MakeMaskAssetKey` is not in the production build today. |
| M21 | Until SM6 and SM9, each render, thumbnail, overlay, and analytic edit path that receives an AI Mask reports a real error that names the missing evaluation. No path draws an empty or substitute Mask. | Master plan 3.3. SM5 merges before SM6; the tool UI (SM9) is the first producer, so no user reaches these errors before SM6. |

### 1.4 Rejected options

- One main history commit per session step.
- Writing an R8 for each session step, or for a Feather or Expand change.
- A project folder or a per-user cache folder for R8 files (lost at Save without a package change).
- The 256 x 256 tiled copy-on-write store of the Brush plan for AI Masks. One AI Mask is one
  immutable raster per commit; tiles add no sharing.
- Stored R8 after Feather and Expand (the SM7 outline before D20).
- A conflict check at Commit (M16 makes the conflict impossible).
- A reference count per asset. M11 recomputes reachability from the stored history.

---

## 2. Product and data specification

SM5 has no visible change in the product UI except the Paste notice (M17). SM9 is the first user
of the edit session.

### 2.1 Source JSON (example)

```json
{
  "kind": "ai_segmentation",
  "format_version": 1,
  "asset_key": "4f0c...e1",
  "asset_width": 812,
  "asset_height": 1530,
  "reference_bounds": {"x": 0.21, "y": 0.05, "w": 0.198, "h": 0.498},
  "analysis_width": 4096,
  "analysis_height": 3072,
  "feather_px": 2.0,
  "expand_px": -1,
  "model_set_id": "sam2.1-tiny+people-sky-head-p1r2e15+s3od-synth",
  "geometry_fingerprint": "9a1b...77",
  "components": [
    {"operation": "add", "task": "people",
     "person_toggles": [{"index": 2, "operation": "add", "box": {"x0": 0.6, "y0": 0.2, "x1": 0.8, "y1": 0.9}}]},
    {"operation": "subtract",
     "prompt_steps": [{"points": [{"x": 0.31, "y": 0.42, "label": "add"}], "box": null, "level": 1}]}
  ]
}
```

Validation rules (`ValidateMaskModel`):

| Field | Rule |
| --- | --- |
| `asset_key` | 32 lowercase hexadecimal digits. |
| `asset_width`, `asset_height` | 1 to 4096; at most `analysis_width` and `analysis_height`. |
| `reference_bounds` | Finite; `x, y >= 0`; `w, h > 0`; `x + w <= 1`, `y + h <= 1`. |
| `analysis_width`, `analysis_height` | 1 to 4096. |
| `feather_px` | Finite, 0 to 50. |
| `expand_px` | Integer, -50 to 50. |
| `model_set_id` | Not empty. |
| `geometry_fingerprint` | 32 lowercase hexadecimal digits. |
| `components` | At least one. Each has `operation` `add` or `subtract` and exactly one of `task` and `prompt_steps`. |
| `task` | `subject`, `background`, `sky`, or `people`. `person_toggles` only with `people`. |
| `person_toggles` | Each has `index`, `operation`, and the normalized clipped `box` of the People list (for display only). |
| `prompt_steps` | At least one step. Each step has points or a box or both. Points and box corners in [0, 1]. Label `add` or `subtract`. At most 64 points in a group. `level` 0 to 2, and > 0 only for a step with one point and no box. |

The person `box` is not used for the replay; the replay asks for the person by `index`. A replay
`index` outside the People list is a step error (SM3.D 2.6).

### 2.2 Session behavior

| Step | Model call | Result |
| --- | --- | --- |
| Prompt step (stroke, click, box) with tool mode Add or Subtract | One decode (M3, M4) | New state; the coverage changes. |
| Level change of the last single-click step | None | New state. |
| Task step (Subject, Background, Sky, People) with operation Add or Subtract | One task request (a stored result in the sidecar after the first) | New state. A `found = false` result makes no state and returns its message. |
| Person box click (last component is People) | One Person request the first time for that person | New state with an Add or Subtract toggle (M5). `found = false` makes no state. |
| Feather or Expand change | None | New state; the coverage before Feather and Expand does not change. |
| Input mode change (prompt input, task input) | None | New state; closes the open prompt group (M3). |
| Undo, Redo | None | The previous or next state and its coverage. |
| Commit | None | Asset and one main history operation (section 5.3). |
| Cancel, image change | None | Nothing is published. |

A failed model call makes no state and keeps the current state (master plan 3.3). A new state
after an Undo removes the Redo states.

### 2.3 Main history

| Action | Batch | Undo |
| --- | --- | --- |
| Commit of a new AI Mask | `AddMask` with the Mask JSON (default name "AI Mask N") | Removes the Mask. |
| Commit of an opened AI Mask with a changed source | `ReplaceMaskSource` (before and after source JSON) | Restores the earlier source and its asset key. |
| Commit of an opened AI Mask with an equal source | Nothing (current `PublishReplaceSource` behavior) | — |

Version, Branch, and reopen share the commits and the asset rows. Export reads the committed
snapshot; SM6 reads the asset through `PipelineMgmtService::ReadMaskAsset`.

---

## 3. Scope

### 3.1 Included

| Module | Responsibility |
| --- | --- |
| `edit/mask` | Recipe types, `AiSegmentationMaskSource`, JSON, validation, the JSON key walk, `mask_asset.cpp` in `EditMaskCore`. |
| `edit/runtime` | Content key of the AI source; the compile error of M21. |
| `storage/store` | The `MaskAsset` table, `MaskAssetStore` (put, read, list, delete), zlib. |
| `app/pipeline_service` | `PutMaskAsset`, `ReadMaskAsset`, `CollectUnreferencedMaskAssets`. |
| `app/ai_mask_analysis_image` | `MakeAiMaskGeometryFingerprint`. |
| `app/ai_mask_edit_session` (new) | Session stack, output map, composition, commit payload. |
| `app/ai_mask_segmentation_service` | `IAiMaskSegmentationRunner`; session size (M14). |
| `rust/puerh_mind` proto and server, `sidecar_client` | `image_width`, `image_height` in the start response. |
| `app/editor_session_service`, `app/editor_action_policy`, `app/editor_mask_creation_controller`, `app/editor_session_ports` | Lease, Open, Commit, Cancel, `IEditorMaskAssetPort`, `CommitAiMaskSource`. |
| `album_backend` | Port implementation; asset cleanup call; Paste notice in the transfer dialog model. |
| `app/adjustment_transfer_package_builder` | AI Mask exclusion (M17). |
| Other switch sites of `MaskSource` | Explicit AI branch (M21). |

### 3.2 Excluded

- Raster `MaskEvaluate`, texture upload, and Feather and Expand (SM6).
- The D9 notice (SM7).
- The coverage preview request (SM8).
- The AI Mask tool controller, QML, prompt presentation, and the Mask thumbnail from R8 (SM9).

---

## 4. Current source audit

All paths are under `alcedo_studio/src/` unless they start with `rust/` or `docs/`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Mask source | `include/edit/mask/mask_model.hpp:32-102` | `MaskSourceKind` Brush (ifdef) 0, Radial 1, LinearGradient 2. `MaskSource` variant. | M1. |
| Source dispatch | `edit/mask/mask_model.cpp:306-380` | `GetMaskSourceKind` returns LinearGradient for every non-Radial value; `ValidateMaskModel` and `MaskModelToJson` use `if constexpr` with a final `else` for Linear Gradient. | Explicit AI branches. |
| Raster asset | `include/edit/mask/mask_asset.hpp`, `edit/mask/mask_asset.cpp` | `MaskAsset`, `MaskAssetDescriptor`, `CanonicalMaskAssetBytes`, `MakeMaskAssetKey` (XXH3 128). The source file is built only in `EditMaskBrush` (`edit/CMakeLists.txt:40-48`). | M20. |
| Batches | `include/edit/history/pipeline_edit_batch.hpp:22-292` | `AddMaskChange`, `RemoveMaskChange`, `ReplaceMaskSourceChange` hold Mask and source JSON. | None; the AI source travels as JSON. |
| Mask creation owner | `include/app/editor_mask_creation_controller.hpp:139-347`, `app/editor_mask_creation_controller.cpp:840-936` | `PublishSettledBatch`, `PublishAddMask`, `PublishReplaceSource` through the settle publisher of `editor_session_service.cpp:1478` and `:1630`. | `CommitAiMaskSource`. |
| Action policy | `include/app/editor_action_policy.hpp:61-71`, `app/editor_action_policy.cpp:110-166` | Lease kinds and their blocked actions; `Comparison` blocks image writes and history movement. | M16. |
| Editor ports | `include/app/editor_session_ports.hpp`, `include/app/editor_session_service.hpp:679-692` | Pipeline, history, tasks, thumbnails, render, images. | `IEditorMaskAssetPort`. |
| Schema | `include/storage/store/database.hpp:180-300`, `storage/store/database.cpp:78-92` | `CREATE TABLE IF NOT EXISTS` DDL strings run at every open. No BLOB column exists. | M10. |
| Commit cleanup | `storage/store/edit_history/commit_graph_store.cpp:480-511`, `app/pipeline_service.cpp:331-336`, `ui/alcedo_main/album_backend/project_handler.cpp:419-423` | Deletes unreachable `EditCommit` rows at clean close. | M11 after it. |
| Journal | `include/edit/history/mini_git_working_history.hpp:75-131`, `ui/alcedo_main/album_backend/application_module_host.cpp:309-324` | `MiniGitJournal::Load`, `Snapshot`; files `editor-journal/image-<id>.mini-git.wal` beside the runtime DB. | Read by M11. |
| Package | `app/project_package_backend.cpp:685-745` | Metadata JSON and DuckDB bytes only. | None (M10). |
| Content key | `edit/runtime/result_content_key.cpp:200-245` | Mixes Radial and Linear fields; Brush strokes. | Mix asset key, descriptor, edge. |
| Compile | `edit/runtime/graph_compiler.cpp:296-311` | Brush throws "Brush mask rasterization is not available in this build". | M21 for AI. |
| Other dispatch sites | `edit/mask/grade_mask_coverage.cpp:143`, `edit/mask/analytic_mask_edit.cpp:249`, `edit/runtime/opencl/opencl_mask_pass.cpp:173`, `cuda_mask_pass.cu`, `metal_mask_pass.mm`, `app/mask_thumbnail_spec.cpp`, `app/mask_thumbnail_evaluate.cpp`, `app/editor_mask_creation_controller.cpp:73`, `:750`, `ui/edit_viewer/mask_overlay_layout.cpp`, `ui/alcedo_main/album_backend/editor_mask_creation_adapter.cpp`, `editor_node_controller.cpp`, `alcedo_qan_graph.cpp`, `include/app/editor_node_graph_projection.hpp` | Radial or else Linear Gradient. | M21 (error or "AI" label). |
| Paste | `app/adjustment_transfer_package_builder.cpp:124`, `app/document_transfer.cpp:176-182`, `app/document_transfer_planner.cpp:213-232`, callers `ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.cpp:300`, `ui/alcedo_main/automation/commands/adjustment_commands.cpp:563` | Copies all Masks of a selected Masks item. | M17. |
| Segmentation | `include/app/ai_mask_segmentation_service.hpp:46-112`, `include/sidecar_client/dto/segmentation.hpp:55-146`, `rust/puerh_mind/proto/segmentation.proto:99-104` | Blocking calls on one thread; the start response has no image size. | M14; `IAiMaskSegmentationRunner`. |
| Develop fields | `include/edit/graph/develop_node_model.hpp:68-115`, accessors `:186-245` | Lens and RAW decode fields read under the Model lock. | M13 input. |
| Lab F8 | `alcedo-ai-mask-lab` `scripts/poc/ai_mask_mvp/mask_pipeline.py:812-832`, `ai_mask_app.py:61-62` | Edge and Feather in photo pixels, range 50. | M8 values. |

---

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Changes | Reads | Lifetime | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| `MaskAssetStore` (storage) | connection, `MaskAsset` or key | rows, verified `MaskAsset` | `MaskAsset` table | — | one operation | `std::runtime_error`: missing, corrupt, codec, SQL |
| `PipelineMgmtService` | asset, key, journal directory | asset, delete count | through the store | commit tables, journals | project | as the store |
| `AiMaskEditSession` (app) | runner, session size, model set id, fingerprint, optional stored source and asset | coverage, People list, step results, commit payload | its own stack and output map | — | one tool session | step result with the real message; `PrepareCommit` throws |
| `EditorSessionService` | Open, Commit, Cancel commands | results | lease; through the mask controller: the document and history | document | editor session | `Rejected`, `Failed` with the message |
| `EditorMaskCreationController` | grade, Mask id, source | one batch | live document (after values), through the settle publisher | — | editor | `EditorMaskCreationResult` |

### 5.2 Session step (prompt stroke)

```text
SM9 tool worker thread
  -> AiMaskEditSession::AddPromptStep(step, mode)
     -> next recipe = current recipe + step (M3)
     -> output key of the group prefix -> output map hit? -> no model call
        miss: runner.Decode(all group points, box, mask_input = previous logits)  (M4)
     -> on failure: return {Failed, message}; stack unchanged
     -> push state; drop Redo states
     -> ComposeAiMaskCoverage(state, outputs, session size)  (M6)
     -> return {Applied}
```

### 5.3 Commit

```text
SM9 tool worker: AiMaskEditSession::PrepareCommit()
  -> bounding rectangle + 1 px (M9) -> MaskAsset{MakeMaskAssetKey(...)}
  -> AiSegmentationMaskSource{key, descriptor, analysis_extent, recipe, edge, model set, fingerprint}
SM9 -> EditorSessionService::CommitAiMaskEdit(EditorAiMaskCommit)
  -> SubmitCommand(CommitAiMaskEdit)                              session owner thread
  -> ValidateMaskModel(source); asset.key == source.asset_key
  -> IEditorMaskAssetPort::PutMaskAsset(asset)                    own DuckDB transaction (M12)
     -> PipelineMgmtService::PutMaskAsset -> MaskAssetStore::Put
  -> EditorMaskCreationController::CommitAiMaskSource(grade, mask id?, source)
     -> PublishAddMask | PublishReplaceSource -> settle publisher
        -> PublishAppliedTypedBatch -> journal append -> live apply
  -> ReleaseLeasesByKind(AiMaskEdit) -> notify
```

### 5.4 Failure chains

| Failure | State that stays valid | Result |
| --- | --- | --- |
| Model call fails in a step | Session state and coverage | `{Failed, message}`; SM9 shows it. |
| Task, People, or Person not found | Session state | `{NotFound, message}`. |
| `PrepareCommit` on empty coverage | Session | Throws "The AI Mask has no coverage." |
| Asset put fails | Document, history, session, lease | `Failed` with the store message; the user can commit again or cancel. |
| History publish fails after the put | Document and history (rolled back by `PublishAppliedTypedBatch`), session, lease | `Failed` with the publish message; the row stays unreferenced until M11. |
| Read of a missing or corrupt asset | Store unchanged | Throws "Mask asset <key> is missing from the project" or "Mask asset <key> is corrupt: <reason>". |
| Cleanup fails | Assets unchanged (transaction rolled back) | `PersistProjectForClose` returns the error, as for commit cleanup. |

### 5.5 Copies

`EditorSessionService::OpenAiMaskEdit` returns a copy of the stored `AiSegmentationMaskSource`.
Purpose: the session edits its own recipe while the live document stays unchanged (D16). Owner:
the session. Lifetime: until Commit or Cancel. Consistency: the lease (M16) blocks every document
write while the session is open, and Commit writes through the owner operation
`ReplaceMaskSource`. The commit payload (`MaskAsset`, source) is the output value of the session,
not a copy of existing state.

---

## 6. File and API map

### 6.1 Current files that change

| File | Change |
| --- | --- |
| `include/edit/mask/mask_model.hpp`, `edit/mask/mask_model.cpp` | M1, JSON, validation, explicit branches. |
| `edit/CMakeLists.txt` | M20; `ai_mask_recipe.cpp`, `ai_mask_asset_references.cpp` in `EditMaskCore`. |
| `edit/runtime/result_content_key.cpp`, `edit/runtime/graph_compiler.cpp` | Content key; M21. |
| Dispatch sites of section 4 | M21. |
| `include/storage/store/database.hpp`, `storage/store/database.cpp`, `storage/CMakeLists.txt` | `mask_asset_table_query`; zlib link. |
| `include/app/pipeline_service.hpp`, `app/pipeline_service.cpp` | M18 operations. |
| `ui/alcedo_main/album_backend/project_handler.cpp` | M11 call. |
| `include/app/ai_mask_analysis_image.hpp`, `app/ai_mask_analysis_image.cpp` | `MakeAiMaskGeometryFingerprint`. |
| `rust/puerh_mind/proto/segmentation.proto`, `rust/puerh_mind/src/server/segmentation.rs`, `rust/puerh_mind/src/service/segmentation/engine.rs` | M14. |
| `include/sidecar_client/dto/segmentation.hpp`, `sidecar_client/segmentation_client.cpp`, `include/app/ai_mask_segmentation_service.hpp`, `app/ai_mask_segmentation_service.cpp` | M14; `IAiMaskSegmentationRunner`. |
| `include/app/editor_action_policy.hpp`, `app/editor_action_policy.cpp` | M16. |
| `include/app/editor_session_ports.hpp`, `include/app/editor_session_service.hpp`, `app/editor_session_service.cpp`, `app/editor_session_command_queue.hpp` | Open, Commit, Cancel; port. |
| `include/app/editor_mask_creation_controller.hpp`, `app/editor_mask_creation_controller.cpp` | `CommitAiMaskSource`. |
| `app/adjustment_transfer_package_builder.cpp`, `include/app/adjustment_transfer_package_builder.hpp` | M17. |
| `ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.{hpp,cpp}`, the transfer dialog QML, `ui/alcedo_main/automation/commands/adjustment_commands.cpp` | Paste notice and the automation field `skipped_ai_masks`. |

### 6.2 Proposed files

| File | API |
| --- | --- |
| `include/edit/mask/ai_mask_recipe.hpp`, `edit/mask/ai_mask_recipe.cpp` | `AiMaskComponentOperation`, `AiMaskTaskKind`, `AiMaskPromptPoint`, `AiMaskPromptStep`, `AiMaskPromptGroup`, `AiMaskPersonToggle`, `AiMaskTaskComponent`, `AiMaskComponent`, `AiMaskRecipe`, `AiMaskEdgeAdjustment`; `AiMaskRecipeToJson`, `AiMaskRecipeFromJson`, `ValidateAiMaskRecipe` (throws `std::runtime_error`). |
| `include/edit/mask/ai_mask_asset_references.hpp`, `edit/mask/ai_mask_asset_references.cpp` | `CollectAiMaskAssetKeys(const nlohmann::json& value, std::set<MaskAssetKey>& keys)`: recursive walk; no state change except `keys`. |
| `include/storage/store/mask_asset/mask_asset_store.hpp`, `storage/store/mask_asset/mask_asset_store.cpp` | `class MaskAssetStore { explicit MaskAssetStore(duckdb_connection); auto Put(const MaskAsset&) -> bool /*true when written*/; auto Read(const MaskAssetKey&) -> MaskAsset; auto ListKeys() -> std::vector<MaskAssetKey>; auto DeleteKeysNotIn(const std::set<MaskAssetKey>&) -> std::size_t; }`. Put validates the asset and recomputes its key. |
| `include/app/ai_mask_edit_session.hpp`, `app/ai_mask_edit_session.cpp` (library `AiMaskEditSession`) | Section 7.2. |
| `include/app/ai_mask_coverage_composition.hpp`, `app/ai_mask_coverage_composition.cpp` | `AiMaskModelOutput` (variant: `AiMaskSamLogits{size, logits}`, `AiMaskAlphaGrid{width, height, values, box}`, `AiMaskBinaryR8{width, height, values}`); `ConvertModelOutputToCoverage(const AiMaskModelOutput&, Extent2D) -> std::vector<uint8_t>`; `FoldAiMaskCoverage(std::vector<uint8_t>& coverage, std::span<const uint8_t> component, AiMaskComponentOperation)`; `CropAiMaskCoverage(std::span<const uint8_t>, Extent2D) -> std::optional<MaskAsset>` (M9). Pure functions. |
| `ui/alcedo_main/album_backend/editor_session_mask_asset_port.{hpp,cpp}` | `EditorSessionMaskAssetPort : IEditorMaskAssetPort` over `PipelineMgmtService`. |

### 6.3 Proposed API signatures

```cpp
// include/app/pipeline_service.hpp
auto PutMaskAsset(const MaskAsset& asset) -> void;                       // throws on error
auto ReadMaskAsset(const MaskAssetKey& key) -> MaskAsset;                // throws: missing, corrupt
auto CollectUnreferencedMaskAssets(const std::filesystem::path& journal_directory) -> std::size_t;

// include/app/ai_mask_analysis_image.hpp
auto MakeAiMaskGeometryFingerprint(const DevelopParamsModel& develop) -> std::string;

// include/app/ai_mask_segmentation_service.hpp
class IAiMaskSegmentationRunner {
 public:
  virtual ~IAiMaskSegmentationRunner() = default;
  virtual auto Decode(const SegmentationPrompt& prompt) -> SegmentationDecodeResult = 0;
  virtual auto RunTask(SegmentationTask task, std::optional<uint32_t> person_index)
      -> SegmentationTaskResult = 0;
};

// include/app/editor_session_ports.hpp
class IEditorMaskAssetPort {
 public:
  virtual ~IEditorMaskAssetPort() = default;
  virtual auto PutMaskAsset(const MaskAsset& asset, std::string* error) -> bool = 0;
};

// include/app/editor_session_service.hpp
struct EditorAiMaskEditOpenResult {
  EditorSessionResult                     result;
  std::string                             geometry_fingerprint;
  std::optional<AiSegmentationMaskSource> stored_source;   // set when a Mask id was given
};
struct EditorAiMaskCommit {
  NodeId                   grade_id;
  std::optional<MaskId>    mask_id;        // empty: new Mask
  MaskAsset                asset;
  AiSegmentationMaskSource source;
};
auto OpenAiMaskEdit(const NodeId& grade_id, std::optional<MaskId> mask_id)
    -> EditorAiMaskEditOpenResult;
auto CommitAiMaskEdit(EditorAiMaskCommit commit) -> EditorSessionResult;
auto CancelAiMaskEdit() -> EditorSessionResult;

// include/app/editor_mask_creation_controller.hpp
auto CommitAiMaskSource(const NodeId& grade_id, const std::optional<MaskId>& mask_id,
                        const AiSegmentationMaskSource& source) -> EditorMaskCreationResult;
```

---

## 7. Implementation phases

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| SM5.1 | AI Mask source kind, recipe JSON, asset table and store, reachability cleanup, explicit dispatch | `edit/mask`, `edit/runtime`, `storage`, `app/pipeline_service`, album backend | SM4 | 1,500-1,900 | complete (13.1) |
| SM5.2 | AI Mask edit session: stack, output map, composition, People toggles, reopen, commit payload; session size in the start response | `app/ai_mask_edit_session`, segmentation service, proto | SM5.1, SM3.D | 1,300-1,700 | complete (13.2) |
| SM5.3 | Editor integration: lease, Open, Commit, Cancel, asset port; Paste exclusion and notice | `app/editor_session_service`, policy, mask controller, transfer, album backend | SM5.1, SM5.2 | 1,000-1,400 | complete (13.3) |

SM5 is split into three parts because one part with the session and the editor integration can
pass 2,000 lines. Order: SM5.1, then SM3.D and SM5.2, then SM5.3.

### 7.1 SM5.1 — Source, asset store, and cleanup

**Objective and deliverables.** The document can hold an AI Mask. The project stores its R8
asset. Clean close removes unreferenced assets. Every existing dispatch site handles the new kind.

**Inputs.** `main` with SM4. No sidecar is needed.

**Data rules and invariants.** M1, M2, M8, M10, M11, M12, M13, M18 to M21. A put never changes
an existing row. A read returns only verified pixels.

**Implementation steps.**

1. Move `mask_asset.cpp` into `EditMaskCore` (M20). Keep `EditMaskBrush` linking `EditMaskCore`.
2. Add `ai_mask_recipe` with the types of section 6.2, the JSON of section 2.1, and
   `ValidateAiMaskRecipe`.
3. Add `AiSegmentationMaskSource` and `MaskSourceKind::AiSegmentation`. Make every `if constexpr`
   chain in `mask_model.cpp` explicit for the four kinds. `MaskModelFromJson` reads `ai_segmentation`
   and rejects unknown fields.
4. Add `CollectAiMaskAssetKeys`.
5. Add `MakeAiMaskGeometryFingerprint` (M13).
6. Mix `asset_key`, `descriptor`, `feather_px`, and `expand_px` into the result content key. The
   recipe does not change pixels and is not mixed.
7. Make each dispatch site of section 4 explicit. Render, thumbnail, analytic edit, and overlay
   paths throw `"<owner>: AI Segmentation Mask <operation> is not available in this build"`.
   Node graph and Mask list labels show the kind text "AI".
8. Add `mask_asset_table_query` and run it in `Database` initialization after the commit graph
   tables.
9. Add `MaskAssetStore` with DuckDB prepared statements (`duckdb_bind_blob`) and zlib. Link the
   storage library to `$<IF:$<TARGET_EXISTS:zlibstatic>,zlibstatic,ZLIB::ZLIB>`.
10. Add the three `PipelineMgmtService` operations. `CollectUnreferencedMaskAssets` takes one
    connection guard and its lock for mark and sweep; it reads each `*.mini-git.wal` file of the
    journal directory with `MiniGitJournal::Load` and walks each record payload.
11. Call `CollectUnreferencedMaskAssets` in `ProjectHandler::PersistProjectForClose` after
    `CollectUnreachableEditCommits`, with the journal directory of `application_module_host.cpp`
    (`db_path().parent_path() / "editor-journal"`, else the workspace directory).

**Primary success call chain.**

```text
PipelineMgmtService::PutMaskAsset(asset)
  -> GetConnectionGuard -> Lock -> MaskAssetStore::Put
     -> ValidateMaskAsset -> MakeMaskAssetKey == asset.key -> compress2
     -> INSERT ... ON CONFLICT (asset_key) DO NOTHING
PipelineMgmtService::ReadMaskAsset(key)
  -> MaskAssetStore::Read -> SELECT -> uncompress -> byte count -> MakeMaskAssetKey == key
ProjectHandler::PersistProjectForClose
  -> CollectUnreachableEditCommits -> CollectUnreferencedMaskAssets(journal directory)
     -> marks from EditCommit, ImageEditState, PipelineRoot, journals -> DeleteKeysNotIn
```

**Primary failure chain.** A changed `data` byte gives "Mask asset <key> is corrupt: key
mismatch". A missing row gives the missing error. A failed delete rolls back; no row is lost.

**Tests and evidence.** Section 8.1.

**Exit criteria.**

- [x] The tests of section 8.1 pass.
- [x] The existing tests of the touched targets pass, or each failure also fails on `main` (#480).
- [x] `AlbumBackendLib` and `alcedo_main` build.
- [x] The Radial and Linear Gradient JSON bytes are unchanged (test 8.1).

**Expected diff.** 1,500-1,900 lines (production 800-950, tests 650-850, build files 30, plan
50).

**Completion record.** Section 13.

### 7.2 SM5.2 — AI Mask edit session

**Objective and deliverables.** An app-layer session that runs every step of section 2.2 against
a runner, keeps the session stack, composes the coverage, opens a stored AI Mask, and makes the
commit payload.

**Inputs.** SM5.1 types. SM3.D task results (`PERSON`, `mask_box`, `in_subject`).

**API.**

```cpp
enum class AiMaskInputMode : std::uint8_t { Prompt, Task };
enum class AiMaskStepStatus : std::uint8_t { Applied, NotFound, Failed, Rejected };
struct AiMaskStepResult { AiMaskStepStatus status; std::string message; };
struct AiMaskSessionImage {
  Extent2D    extent;               // M14
  std::string model_set_id;
  std::string geometry_fingerprint;
};
class AiMaskEditSession {
 public:
  AiMaskEditSession(IAiMaskSegmentationRunner& runner, AiMaskSessionImage image);
  AiMaskEditSession(IAiMaskSegmentationRunner& runner, AiMaskSessionImage image,
                    const AiSegmentationMaskSource& stored, MaskAsset stored_asset);  // M15
  auto AddPromptStep(AiMaskPromptStep step, AiMaskComponentOperation mode) -> AiMaskStepResult;
  auto SelectLevel(std::uint32_t level) -> AiMaskStepResult;
  auto AddTask(AiMaskTaskKind task, AiMaskComponentOperation operation) -> AiMaskStepResult;
  auto TogglePerson(std::uint32_t person_index) -> AiMaskStepResult;
  auto SetEdgeAdjustment(AiMaskEdgeAdjustment edge) -> AiMaskStepResult;
  auto SetInputMode(AiMaskInputMode mode) -> AiMaskStepResult;
  auto Undo() -> bool;
  auto Redo() -> bool;
  [[nodiscard]] auto CanUndo() const -> bool;
  [[nodiscard]] auto CanRedo() const -> bool;
  [[nodiscard]] auto Coverage() const -> std::span<const std::uint8_t>;   // session size
  [[nodiscard]] auto People() const -> std::span<const SegmentationPerson>;
  [[nodiscard]] auto PersonIncluded(std::uint32_t person_index) const -> bool;  // M5
  [[nodiscard]] auto Recipe() const -> const AiMaskRecipe&;
  [[nodiscard]] auto Edge() const -> const AiMaskEdgeAdjustment&;
  auto PrepareCommit() const -> std::pair<MaskAsset, AiSegmentationMaskSource>;     // M9
};
```

Threading: not thread-safe; one thread (the SM9 tool worker) calls every method, as for
`AiMaskSegmentationService`. The runner calls block.

**Data rules and invariants.**

- A state is `{recipe, edge, input_mode, open_prompt_group}`. The stack is a vector and a current
  index. A new state removes the states after the current index.
- A state enters the stack only after all its outputs exist and its coverage is composed.
- `SelectLevel` applies only when the last component is a prompt group whose last step has one
  point and no box; else `Rejected`.
- `TogglePerson` applies only when the last component is People; else `Rejected`. An index outside
  the People list is `Rejected` without a runner call.
- `AddTask(People)` stores the People list of the result; `People()` returns it.
- Output keys: the canonical JSON of the task (`{"task": ...}`), of the person (`{"person": i}`),
  and of the prompt group prefix (all steps up to the step).
- Composition reads the component outputs in recipe order (M6). The People component folds its
  base and toggles first, then folds into the total with its operation.

**Implementation steps.**

1. M14: add `image_width` and `image_height` to the proto response, the Rust server mapping (the
   entry size), the C++ DTO, and the client mapping.
2. Add `IAiMaskSegmentationRunner`; `AiMaskSegmentationService` implements it.
3. Add `ai_mask_coverage_composition` (section 6.2) with OpenCV `cv::resize(INTER_LINEAR)`
   (`<opencv2/imgproc.hpp>`, `<opencv2/core/mat.hpp>`).
4. Add `AiMaskEditSession` with the steps of section 2.2, the output map (M7), the People rule
   (M5), Undo and Redo, the reopen constructor (M15), and `PrepareCommit` (M9). Prompt group
   replay decodes the steps of a group in order, each with the previous selected logits.
5. Register the library `AiMaskEditSession` (links `AiMaskSegmentationService`, `EditMaskCore`,
   `opencv_imgproc`) and the test binary `AiMaskEditSessionTest`.

**Primary success call chain (People, then remove person 1).**

```text
AddTask(People, Add)
  -> runner.RunTask(PEOPLE) -> alpha grid (mask_box 0,0,1,1), People list with in_subject
  -> push state -> Coverage = alpha converted to session size
TogglePerson(1)   PersonIncluded(1) == in_subject == true -> Subtract
  -> runner.RunTask(PERSON, 1) -> crop alpha grid with mask_box
  -> push state -> People component = min(base, 255 - person 1) -> fold
```

**Primary failure chain.** `RunTask(PERSON, 3)` returns `found = false`: result `NotFound` with
the sidecar message; the stack, coverage, and `PersonIncluded(3)` do not change. A runner error:
`Failed` with the runner message; nothing changes.

**Tests and evidence.** Section 8.2.

**Exit criteria.**

- [x] The tests of section 8.2 pass, and the Rust tests of the start response pass.
- [x] `AiMaskSegmentationServiceTest` and `SegmentationClientTest` pass.

**Expected diff.** 1,300-1,700 lines (production 650-800, tests 600-850, Rust and proto 50).

**Completion record.** Section 13.

### 7.3 SM5.3 — Editor integration and Paste

**Objective and deliverables.** An open AI Mask edit holds the editor lease; Commit stores the
asset and publishes one history operation; Cancel publishes nothing; Paste skips AI Masks and
names them.

**Inputs.** SM5.1 and SM5.2.

**Data rules and invariants.** M12, M16, M17, section 5.5.

**Implementation steps.**

1. Add `EditorOperationLeaseKind::AiMaskEdit` and its blocked action list (M16).
2. Add `IEditorMaskAssetPort` to the editor ports and `EditorSessionMaskAssetPort` in the album
   backend.
3. Add `EditorMaskCreationController::CommitAiMaskSource`: without a Mask id, a new `MaskModel`
   (new id from the existing Mask id source, name "AI Mask N", the source) through `PublishAddMask`;
   with a Mask id, the existing Mask must have the AI kind, then `PublishReplaceSource`.
4. Add `OpenAiMaskEdit`, `CommitAiMaskEdit`, and `CancelAiMaskEdit` as owner commands. Open
   rejects a missing Grade, a missing Mask, a Mask of another kind, and a second open session.
   Open reads the Develop of the working document for `MakeAiMaskGeometryFingerprint`. An image
   change and the session close release the lease.
5. M17 in `AdjustmentTransferPackageBuilder::Build`; add `ListAiMasksExcludedFromTransfer`.
6. The transfer dialog model exposes `excludedAiMaskNotice` (empty, or "AI Masks are not copied:
   <names>"); the dialog QML shows it below the selection. The automation command adds
   `skipped_ai_masks` (array of Mask ids and names) to its response.

**Primary success call chain.** Section 5.3.

**Primary failure chain.** Section 5.4, rows "Asset put fails" and "History publish fails after
the put".

**Tests and evidence.** Section 8.3.

**Exit criteria.**

- [x] The tests of section 8.3 pass.
- [x] `AlbumBackendLib` and `alcedo_main` build.
- [x] The existing tests of the touched targets pass, or each failure also fails on `main` (#484).

**Expected diff.** 1,000-1,400 lines (production 450-600, tests 500-750, QML 20).

**Completion record.** Section 13.

---

## 8. Tests

### 8.1 SM5.1

| Test | Target | Assertion |
| --- | --- | --- |
| `AiSegmentationSourceJsonRoundTripKeepsAllFields` | `GpuDagModelGraphTest` (`color_grade_mask_model_test.cpp`) | `MaskModelFromJson(MaskModelToJson(m)) == m` for a recipe with People toggles and two prompt groups; the JSON has kind `ai_segmentation`. |
| `AiSegmentationSourceRejectsInvalidFields` | same | Each rule of section 2.1 throws: key with 31 digits, extent 4097, bounds outside [0, 1], 65 points in a group, level 1 with a box, toggles on `sky`, `feather_px` 51, `expand_px` 1.5, empty components, unknown field. |
| `ExistingMaskJsonBytesAreUnchanged` | same | Radial and Linear Gradient `MaskModelToJson(...).dump()` equal the expected strings of the current code. |
| `CollectAiMaskAssetKeysFindsKeysInEveryBatchType` | `PipelineEditBatchTest` | Keys from `AddMask`, `RemoveMask`, `ReplaceMaskSource` (before and after), a removed Grade, and a Paste batch; a Radial Mask gives no key. |
| `GeometryFingerprintTracksLensAndRawDecodeOnly` | `EditorComparisonInputsTest` | A distortion, lens model, or demosaic change changes the fingerprint; a white balance or vignetting change does not. |
| `AiSegmentationMaskContentKeyTracksAssetAndEdge` | `GpuDagRawInputTest` (`result_content_key_test.cpp`) | Another asset key, Feather, or Expand changes the key; another recipe with the same asset does not. |
| `AiSegmentationMaskCompileReportsMissingRasterEvaluation` | `GpuDagRawInputTest` (`graph_compiler_test.cpp`) | Compile throws the M21 message. |
| `MaskAssetPutThenReadReturnsVerifiedPixels` | `MaskAssetStoreTest` (new) | Read gives the same descriptor and pixels; the row `data` is smaller than the pixels for a two-region 1024 x 768 mask. |
| `MaskAssetPutOfExistingKeyWritesNothing` | same | The second put returns false; one row. |
| `MaskAssetPutRejectsWrongKey` | same | A key that differs from `MakeMaskAssetKey` throws; no row. |
| `MaskAssetReadReportsMissingAndCorruptRows` | same | Unknown key: missing error. A changed `data` byte (SQL update): corrupt error. |
| `ExistingProjectDatabaseGetsTheMaskAssetTable` | `ProjectCompatibilityTest` | A database made without the table opens, gets the table, and keeps its rows. |
| `PackedProjectKeepsMaskAssetRows` | `ProjectCompatibilityTest` | Pack, unpack, and open: the asset reads back with the same pixels. |
| `CleanupKeepsAssetsOfReachableHistory` | `PipelineMapperTest` (`pipeline_service_test.cpp`) | Assets referenced by a reachable commit, a checkpoint, a root, and only by a journal record stay. |
| `CleanupRemovesUnreferencedAssets` | same | An orphan row and an asset of an unreachable commit (after commit cleanup) are deleted; the count is 2. |

### 8.2 SM5.2

The tests use a fake runner that returns fixed logits and alpha grids and counts calls.

| Test | Target | Assertion |
| --- | --- | --- |
| `LogitsConvertToSessionSizeWithThresholdZero` | `AiMaskEditSessionTest` | A 256 x 256 grid with one positive quadrant gives 255 in that quadrant of a 1024 x 768 coverage and 0 elsewhere; values equal the `cv::resize` reference. |
| `AlphaGridPastesThroughMaskBox` | same | A person grid with `mask_box` (0.5, 0.5, 0.25, 0.25) changes only that rectangle; values are `round(alpha * 255)` at gain 1, steepened by half the upscale factor otherwise (D23). |
| `FoldAddsWithMaximumAndSubtractsWithInverse` | same | Known components give the expected R8 values. |
| `PromptStepsChainMaskInputInsideAGroup` | same | The second stroke decodes the points of both strokes with the first selected logits as `mask_input`. |
| `SubtractStrokeAfterTaskStartsSubtractGroup` | same | After Subject, a Subtract stroke makes a new group with operation Subtract and Add labels; the coverage loses the selected region. |
| `UndoAndRedoNeedNoModelCall` | same | Three steps, two Undo, two Redo: runner call count stays 3; the coverage equals the coverage of each state. |
| `NewStepAfterUndoDropsRedo` | same | `CanRedo` is false after the step. |
| `PeopleClickUsesInSubjectThenToggles` | same | Person 0 `in_subject` true: click gives Subtract, `PersonIncluded(0)` false; second click gives Add. |
| `PersonNotFoundMakesNoStep` | same | `NotFound` with the message; stack size unchanged. |
| `FailedModelCallKeepsState` | same | `Failed`; recipe and coverage unchanged. |
| `EdgeAndModeStepsNeedNoModelCall` | same | Call count unchanged; Undo restores the edge; a prompt step after a mode change starts a new group. |
| `LevelChangeAppliesOnlyToSingleClick` | same | Level change after a click: new state, no call; after a stroke: `Rejected`. |
| `PointLimitRejectsThe65thPoint` | same | A step that makes 65 points in a group: `Rejected`, no call. |
| `ReopenShowsStoredCoverageWithoutModelCall` | same | Coverage equals the stored asset placed at its bounds; no call; Feather step: no call; first prompt step: replay calls for all components, then the new decode. |
| `ReopenRejectsAnotherAnalysisSize` | same | Constructor throws for `analysis_extent` 4096 x 3072 and a session of 4096 x 2731. |
| `PrepareCommitCropsToCoverageWithBorder` | same | Bounds, extent, key, and pixels of a coverage with one rectangle; the source has the recipe, edge, model set, and fingerprint. |
| `PrepareCommitRejectsEmptyCoverage` | same | Throws "The AI Mask has no coverage." |
| `start_response_carries_image_size` | Rust `server::segmentation` tests | A pixel start and a key-only start of the same entry return its width and height. |
| `StartSessionResultCarriesImageSize` | `SegmentationClientTest` | The DTO has the fake server values. |

### 8.3 SM5.3

| Test | Target | Assertion |
| --- | --- | --- |
| `AiMaskEditLeaseBlocksDocumentWritesAndHistory` | `EditorSessionActionPolicyCq3Test` | With the lease, the actions of M16 are blocked; `RequestViewChange` and `SelectImage` are allowed. |
| `CommitNewAiMaskStoresAssetThenPublishesAddMask` | `EditorComparisonServiceTest` (`editor_comparison_service_test.cpp`, the binary that compiles the real editor session ports) | The port receives the asset before the publisher receives one `AddMask` batch; the lease is released. |
| `CommitOpenedAiMaskPublishesReplaceMaskSource` | same | One `ReplaceMaskSource` with the stored source as before; an equal source publishes nothing. |
| `MainUndoRestoresThePreviousAiSource` | same | After Commit, main Undo gives the earlier source JSON and asset key; Redo gives the new one. |
| `AssetPutFailureKeepsDocumentAndLease` | same | The port fails: no batch, the lease stays, the error is the port message. |
| `PublishFailureAfterPutKeepsDocument` | same | The publisher fails: the document equals the state before; the lease stays. |
| `CancelPublishesNothing` | same | No port call, no batch, lease released. |
| `OpenRejectsMissingOrNonAiMask` | same | A Radial Mask id and an unknown id give `Rejected`. |
| `PasteBuildSkipsAiMasks` | `AdjustmentTransferServiceTest` | A Grade with one Radial and one AI Mask: the package has only the Radial Mask; `ListAiMasksExcludedFromTransfer` names the AI Mask. |
| `ImportPackageRejectsAiMaskKind` | same | A package JSON with an `ai_segmentation` Mask throws. |
| `TransferDialogShowsSkippedAiMasks` | `AdjustmentTransferDialogModelTest` | `excludedAiMaskNotice` names the AI Mask. |

---

## 9. Measurements

Record on Windows (RTX 3080 Laptop, release build) for one 4096 x 3072 session: the composition
time of one prompt step and of a People toggle, the `PrepareCommit` time, and the asset row size
and put and read times for the Subject coverage of three quality photos. Report a measurement
that was not taken as not taken.

---

## 10. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest PipelineEditBatchTest EditorComparisonInputsTest GpuDagRawInputTest MaskAssetStoreTest ProjectCompatibilityTest PipelineMapperTest AiMaskEditSessionTest AiMaskSegmentationServiceTest SegmentationClientTest EditorSessionActionPolicyCq3Test EditorComparisonServiceTest AdjustmentTransferServiceTest AdjustmentTransferDialogModelTest AlbumBackendLib alcedo_main
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -j 1 --output-on-failure -R "<the test targets of the part>"
```

```powershell
Set-Location D:\Projects\pu-erh_lab\rust\puerh_mind
cargo test --lib segmentation
```

Confirm the real target names in the test `CMakeLists.txt` files before the build; add new targets
with `gtest_discover_tests` and the `alcedo_register_test_target` call of their directory. Put logs
under `build/tmp/sm5/`.

---

## 11. Repository rules that apply

- Read `AGENTS.md` and the applicable skills again before implementation.
- No fallback: a missing or corrupt asset, a failed model call, and an empty coverage are errors.
- Update data through its owner: the session edits only its own recipe; Commit writes through
  `AddMask` or `ReplaceMaskSource`; the copy of section 5.5 has its documented reason.
- No new consistency mechanism without an executable interleaving: the lease (M16) is the existing
  admission rule; no generation or token is added.
- DuckDB connections come from the database guard at each operation (M18).
- Include the defining header; include only the needed OpenCV headers.
- LF line endings. Format new files completely; format only the changed lines of existing files.
- Run only the targets of section 10. Only the user starts a full test suite.
- Make implementation decisions and record them in this plan; open a GitHub issue for each problem
  found during the work.

---

## 12. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| A JSON change of an existing value changes old root ids. | `ExistingMaskJsonBytesAreUnchanged` fails. | Restore the bytes before other work continues. |
| The composition at 4096 x 3072 is slow on the tool thread. | Section 9 measurement above 100 ms per step. | Report it in an issue with the measurement; compose only the bounding rectangle of the changed component. |
| An unlisted dispatch site treats the AI kind as Linear Gradient. | A search for `std::get<LinearGradientMaskSource>` and for `else` after `RadialMaskSource` in touched files. | Add the explicit branch of M21. |
| The journal directory differs from the path of `application_module_host.cpp`. | `CleanupKeepsAssetsOfReachableHistory` with a journal record fails. | Pass the path from the one function that builds it. |

Stop implementation when a condition of master plan section 12 applies.

---

## 13. Completion record

### 13.1 SM5.1

```text
Phase / date / status: SM5.1 / 2026-10-23 / complete.
Source revision and branch: base dc3ad438 (main); branch
  feature/ai-segmentation-sm5-1-source-asset-store.
Actual changed modules: edit/mask (recipe, AI source, key walk, mask_asset.cpp in EditMaskCore),
  edit/runtime (content key, compile error, CUDA/OpenCL/Metal mask pass branches),
  storage/store (MaskAsset DDL, MaskAssetStore, CommitGraphStore history JSON read),
  app/pipeline_service, app/ai_mask_analysis_image, app mask thumbnail and creation sites,
  album_backend (cleanup call, journal directory, kind keys), Mask row QML labels, DESIGN.md.
Implemented behavior: M1, M2, M8, M10, M11, M12 (store side), M13, M18 (PipelineMgmtService
  part), M19, M20, M21; the steps 1 to 11 of section 7.1.
Explicitly unimplemented items: none of SM5.1. The editor port, session, lease, and Paste
  exclusion belong to SM5.2 and SM5.3.
Primary success call chain: section 13.1.1.
Primary failure and restore call chain: section 13.1.1.
Build and test commands with exit codes: section 13.1.2.
Discovered / passed / failed / skipped counts: section 13.1.2.
Manual verification: none; SM5.1 has no producer of AI Masks in the product (SM9).
Performance or resource evidence, if required: not required for SM5.1 (section 9 is for the
  session); a 1024 x 768 two-region asset stores less than 1/20 of its pixel bytes (test).
Evidence path: build/tmp/sm5/ (removed at completion).
Remaining defects or unavailable platforms: the Metal mask pass change was not compiled (no
  macOS build in this run).
```

#### 13.1.1 Call chains

```text
PipelineMgmtService::PutMaskAsset(asset)
  -> Database::GetConnectionGuard -> Lock -> MaskAssetStore::Put
     -> ValidateMaskAsset -> MakeMaskAssetKey == asset.key -> compress2 (zlib)
     -> INSERT ... ON CONFLICT (asset_key) DO NOTHING      (true when a row was written)
PipelineMgmtService::ReadMaskAsset(key)
  -> MaskAssetStore::Read -> SELECT -> format, codec, dimension checks -> uncompress
     -> byte count -> MakeMaskAssetKey == key -> MaskAsset
ProjectHandler::PersistProjectForClose
  -> PipelineMgmtService::Sync -> CollectUnreachableEditCommits
  -> CollectUnreferencedMaskAssets(ProjectHandler::editor_journal_directory())
     -> one guard + lock; MaskAssetStore::ListKeys (empty: return 0)
     -> CommitGraphStore::VisitStoredHistoryJsonContaining("ai_segmentation")
        (EditCommit.edit_payload, ImageEditState and PipelineRoot serialized_pipeline_state)
        -> CollectAiMaskAssetKeys
     -> each *.mini-git.wal: MiniGitJournal::Load -> record commit payload -> CollectAiMaskAssetKeys
     -> MaskAssetStore::DeleteKeysNotIn (one transaction)
GraphCompiler::Compile (document with an AI Mask)
  -> CompileMaskStack -> throws "GraphCompiler: AI Segmentation Mask raster evaluation is not
     available in this build"
```

```text
Put with a key that does not match the pixels -> runtime_error; no row.
Read of an unknown key -> "Mask asset <key> is missing from the project".
Read of a changed row -> "Mask asset <key> is corrupt: <reason>" (key mismatch, byte count,
  dimensions, codec, or zlib inflate failure); the row stays unchanged.
Cleanup: a query, a journal read, or a delete fails -> rollback, no row deleted ->
  PersistProjectForClose returns the error, as for commit cleanup.
```

#### 13.1.2 Evidence

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"   # exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest PipelineEditBatchTest GpuDagRawInputTest MaskAssetStoreTest   # exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorComparisonInputsTest ProjectCompatibilityTest PipelineMapperTest AlbumBackendLib alcedo_main   # exit 0
ctest --test-dir build/debug -j 1 -R "^(GpuDagModelGraphTest|PipelineEditBatchTest|GpuDagRawInputTest|MaskAssetStoreTest)\."   # exit 0, 367/367 passed
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(GpuDagModelGraphTest|PipelineEditBatchTest|GpuDagRawInputTest|MaskAssetStoreTest|EditorComparisonInputsTest|ProjectCompatibilityTest|PipelineMapperTest)\."   # after formatting and a rebuild of all the targets above: exit 8, 420/421 passed (the failure is #480, see below)
```

| Test (section 8.1) | Target | Result |
| --- | --- | --- |
| `AiSegmentationSourceJsonRoundTripKeepsAllFields` | `GpuDagModelGraphTest` | PASS |
| `AiSegmentationSourceRejectsInvalidFields` | `GpuDagModelGraphTest` | PASS |
| `ExistingMaskJsonBytesAreUnchanged` | `GpuDagModelGraphTest` | PASS |
| `CollectAiMaskAssetKeysFindsKeysInEveryBatchType` | `PipelineEditBatchTest` | PASS |
| `GeometryFingerprintTracksLensAndRawDecodeOnly` | `EditorComparisonInputsTest` | PASS |
| `AiSegmentationMaskContentKeyTracksAssetAndEdge` | `GpuDagRawInputTest` | PASS |
| `AiSegmentationMaskCompileReportsMissingRasterEvaluation` | `GpuDagRawInputTest` | PASS |
| `MaskAssetPutThenReadReturnsVerifiedPixels` | `MaskAssetStoreTest` | PASS |
| `MaskAssetPutOfExistingKeyWritesNothing` | `MaskAssetStoreTest` | PASS |
| `MaskAssetPutRejectsWrongKey` | `MaskAssetStoreTest` | PASS |
| `MaskAssetReadReportsMissingAndCorruptRows` | `MaskAssetStoreTest` | PASS |
| `DeleteKeysNotInRemovesOnlyUnlistedRows` (added) | `MaskAssetStoreTest` | PASS |
| `ExistingProjectDatabaseGetsTheMaskAssetTable` | `ProjectCompatibilityTest` | PASS |
| `PackedProjectKeepsMaskAssetRows` | `ProjectCompatibilityTest` | PASS |
| `CleanupKeepsAssetsOfReachableHistory` | `PipelineMapperTest` | PASS |
| `CleanupRemovesUnreferencedAssets` | `PipelineMapperTest` | PASS |

Suite totals of the final run: `GpuDagModelGraphTest` 145/145, `PipelineEditBatchTest` 21/21, `GpuDagRawInputTest` 196/196, `MaskAssetStoreTest` 5/5, `EditorComparisonInputsTest` 16/16, `PipelineMapperTest` 34/34, `ProjectCompatibilityTest` 3/4. The one failure is
`ProjectCompatibilityTest.ProjectWrittenByCurrentMainOpensAndRendersUnchanged`: the CUDA render
hash is `4331927259852930198`, the fixture records `4717977625645122346`. Clean `main`
(`dc3ad438`, changes stashed, only that target rebuilt) fails with the same hashes; issue #480.
The full suite was not run (only the user starts it).

#### 13.1.3 Implementation decisions

- The point label of a prompt step uses `AiMaskComponentOperation` (the type list of section 6.2
  has no label type). A new `AiMaskBox {x0, y0, x1, y1}` holds the prompt box and the person box;
  the reader requires ordered corners in [0, 1].
- `AiSegmentationMaskSource` has no `format_version` member. The writer writes 1 and the reader
  requires 1 (`kAiSegmentationSourceFormatVersion`).
- `x + w <= 1` and `y + h <= 1` of `reference_bounds` allow 1e-6 for float rounding of the M9
  crop.
- A changed `data` byte reports a zlib inflate failure, not "key mismatch": zlib checks the
  Adler-32 of the stream before the key check. The test accepts any "corrupt" reason for that
  case and checks the exact "key mismatch" text for a changed `bounds_x` column.
- The table columns are `NOT NULL`. `PipelineMgmtService::PutMaskAsset` returns `void` (section
  6.3); `MaskAssetStore::Put` returns whether it wrote a row.
- The cleanup mark reads the history tables through a new owner operation,
  `CommitGraphStore::VisitStoredHistoryJsonContaining`. SQL keeps only the values whose text
  contains `ai_segmentation`, so the values without an AI Mask are not parsed. The cleanup
  returns at once when the table has no row. An unreadable journal file is an error, because a
  mark without it can delete an asset that only that journal refers to.
- `ProjectHandler::editor_journal_directory()` is the one function that builds the journal
  directory; the module host builds each journal path from it (risk table row 4).
- Explicit M21 branches: the compiler, the CUDA, OpenCL, and Metal mask passes, the host
  `GradeMaskCoverage`, `ApplyAnalyticMaskHandle`, and `MakeSingleMaskThumbnailSpec` throw the
  "<owner>: AI Segmentation Mask <operation> is not available in this build" text.
  `AnalyticKindFromMask` throws for a source that is not analytic. The node graph and Mask list
  key is `aiSegmentation` with the label "AI"; the AI Mask icon comes with SM9.
- `MaskAssetStoreTest` is registered beside the other storage tests in
  `tests/sleeve/CMakeLists.txt`. `GeometryFingerprintTracksLensAndRawDecodeOnly` lives in
  `ai_mask_analysis_image_test.cpp`, which `EditorComparisonInputsTest` compiles.

### 13.2 SM5.2

```text
Phase / date / status: SM5.2 / 2026-10-23 / complete.
Source revision and branch: base 11cb8747 (main, SM5.1 merged); branch
  feature/ai-segmentation-sm5-2-edit-session.
Actual changed modules: rust/puerh_mind (proto, engine StartedSession, server mapping),
  sidecar_client (DTO, client mapping), app/ai_mask_segmentation_service
  (IAiMaskSegmentationRunner), new library AiMaskEditSession (app/ai_mask_edit_session,
  app/ai_mask_model_output_store, app/ai_mask_coverage_composition), edit/mask/mask_model
  (public ValidateAiSegmentationMaskSource), test CMake (AiMaskEditSessionTest).
Implemented behavior: M3 to M7, M9, M14, M15, D16 (session side), D21; the steps 1 to 5 of
  section 7.2.
Explicitly unimplemented items: none of SM5.2. The lease, Open, Commit, Cancel, the asset port,
  and the Paste exclusion belong to SM5.3.
Primary success call chain: section 13.2.1.
Primary failure and restore call chain: section 13.2.1.
Build and test commands with exit codes: section 13.2.2.
Discovered / passed / failed / skipped counts: section 13.2.2.
Manual verification: none; SM9 is the first user of the session.
Performance or resource evidence, if required: section 13.2.3 (section 9).
Evidence path: build/tmp/sm5/ (removed at completion).
Remaining defects or unavailable platforms: no macOS run (the Rust change is platform-neutral;
  the C++ session has no platform code). Issue #482 (clang-tidy member suffix rule) was found
  during the work.
```

#### 13.2.1 Call chains

```text
AiMaskEditSession::AddTask(People, Add)
  -> next state = current + {Add, People}; prompt group closed
  -> EnsureOutputs -> AiMaskModelOutputStore::Ensure
     -> output {"task":"people"} missing -> runner.RunTask(kPeople)
     -> ValidateAiMaskModelOutput -> alpha grid and People list kept
  -> Push -> Compose: ComponentCoverage (ConvertModelOutputToCoverage, mask_box 0,0,1,1)
     -> FoldAiMaskCoverage(Add) -> Coverage(); Redo states dropped
AiMaskEditSession::TogglePerson(1)
  -> PersonIncluded(1) == in_subject == true -> toggle {1, Subtract, People list box}
  -> Ensure: output {"person":1} missing -> runner.RunTask(kPerson, 1) -> crop alpha + mask_box
  -> Compose: People base, then min(base, 255 - person 1) inside the crop -> fold into total
AiMaskEditSession::AddPromptStep(stroke, Subtract)
  -> open group: labels = Subtract; else new group {Subtract} with Add labels (M3)
  -> point count <= 64 and ValidateAiMaskRecipe, else Rejected (no call)
  -> Ensure: each missing group prefix -> runner.Decode(all points, latest box,
     mask_input = selected logits of the step before) (M4)
  -> step level = decoder selected_index -> Push -> Compose
Undo / Redo -> Compose from the kept outputs (no runner call)
AiMaskEditSession(runner, image, stored source, stored asset)        (M15)
  -> ValidateAiSegmentationMaskSource -> analysis size == session size -> asset key and pixels
  -> PlaceAiMaskAssetInCoverage -> first state = stored recipe and edge, no runner call
  -> Feather, Expand, and mode steps keep the stored coverage; the first component change
     replays every component through Ensure, then decodes the new step
AiMaskEditSession::PrepareCommit
  -> CropAiMaskCoverage (values > 0, 1-pixel border, clipped) -> MakeMaskAssetKey
  -> AiSegmentationMaskSource{key, descriptor, session size, recipe, edge,
     model set and fingerprint (the stored ones for a stored-coverage state)}
  -> ValidateAiSegmentationMaskSource -> {asset, source}
SegmentationEngine::start_session -> StartedSession{image_width, image_height} (entry size,
  also for a cache hit) -> server -> StartSegmentationSessionResponse fields 5 and 6
  -> SegmentationClient -> SegmentationSessionResult{image_width, image_height}
```

```text
runner status not ok (decode, task, person) -> {Failed, sidecar message}; the outputs computed
  before the failure stay in the store; stack, recipe, and coverage do not change.
task, People, or person found = false -> {NotFound, sidecar message}; nothing changes.
Invalid model output (logit or alpha count, box, Sky size, level count) -> {Failed, message}.
Invalid step (65th point, level change of a non-click, toggle without People last, index outside
  the People list, invalid or unchanged edge, unchanged mode) -> {Rejected}; no runner call.
PrepareCommit with no value > 0 -> std::runtime_error "The AI Mask has no coverage."
Reopen with another analysis size, another asset, or changed pixels -> std::runtime_error.
```

#### 13.2.2 Evidence

Commands (PowerShell, repository root unless noted):

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"   # exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AiMaskEditSessionTest AiMaskSegmentationServiceTest SegmentationClientTest GpuDagModelGraphTest   # exit 0
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(AiMaskEditSessionTest|AiMaskSegmentationServiceTest|SegmentationClientTest|GpuDagModelGraphTest)\."   # exit 0, 186/186 passed
# rust/puerh_mind:
cargo test --lib segmentation   # exit 0, 82 passed, 0 failed
cargo fmt -- --check            # exit 0
```

| Test (section 8.2) | Target | Result |
| --- | --- | --- |
| `LogitsConvertToSessionSizeWithThresholdZero` | `AiMaskEditSessionTest` | PASS |
| `AlphaGridPastesThroughMaskBox` | `AiMaskEditSessionTest` | PASS |
| `FoldAddsWithMaximumAndSubtractsWithInverse` | `AiMaskEditSessionTest` | PASS |
| `PromptStepsChainMaskInputInsideAGroup` | `AiMaskEditSessionTest` | PASS |
| `SubtractStrokeAfterTaskStartsSubtractGroup` | `AiMaskEditSessionTest` | PASS |
| `UndoAndRedoNeedNoModelCall` | `AiMaskEditSessionTest` | PASS |
| `NewStepAfterUndoDropsRedo` | `AiMaskEditSessionTest` | PASS |
| `PeopleClickUsesInSubjectThenToggles` | `AiMaskEditSessionTest` | PASS |
| `PersonNotFoundMakesNoStep` | `AiMaskEditSessionTest` | PASS |
| `FailedModelCallKeepsState` | `AiMaskEditSessionTest` | PASS |
| `EdgeAndModeStepsNeedNoModelCall` | `AiMaskEditSessionTest` | PASS |
| `LevelChangeAppliesOnlyToSingleClick` | `AiMaskEditSessionTest` | PASS |
| `PointLimitRejectsThe65thPoint` | `AiMaskEditSessionTest` | PASS |
| `ReopenShowsStoredCoverageWithoutModelCall` | `AiMaskEditSessionTest` | PASS |
| `ReopenRejectsAnotherAnalysisSize` | `AiMaskEditSessionTest` | PASS |
| `PrepareCommitCropsToCoverageWithBorder` | `AiMaskEditSessionTest` | PASS |
| `PrepareCommitRejectsEmptyCoverage` | `AiMaskEditSessionTest` | PASS |
| `start_response_carries_image_size` | Rust `server::segmentation` | PASS |
| `key_only_start_hits_after_session_end` (image size assertions added) | Rust `service::segmentation::engine` | PASS |
| `StartSessionResultCarriesImageSize` | `SegmentationClientTest` | PASS |
| `StartResultsCarryTheSessionImageSize` (added) | `AiMaskSegmentationServiceTest` | PASS |

Suite totals: `AiMaskEditSessionTest` 17/17, `AiMaskSegmentationServiceTest` 13/13,
`SegmentationClientTest` 11/11, `GpuDagModelGraphTest` 145/145 (the target of the changed
`mask_model.cpp`), Rust `segmentation` 82/82. The full suite was not run (only the user starts
it).

#### 13.2.3 Measurements (section 9)

Windows, the release flags of `win_release` (`/O2 /arch:AVX2 /GL`): the `AiMaskEditSession`
library built in `build/release` and a benchmark executable under `build/tmp/sm5/bench/`
(removed). One 4096 x 3072 session with a fake runner (1024 x 768 People alpha, one 600 x 1024
person crop, 256 x 256 logits). Three runs; the times include the fake runner (< 1 ms).

| Step | Time |
| --- | ---: |
| `AddTask(People)` (convert and fold) | 22-25 ms |
| `TogglePerson` (People base, person, fold) | 37-40 ms |
| `AddPromptStep` (logits to 4096 x 3072, fold of 3 components) | 25-30 ms |
| `Undo` (compose again) | 25-26 ms |
| `PrepareCommit` (crop of a 4096 x 3072 coverage, key) | 16-17 ms |

The first version used a per-pixel `std::lround` and scalar folds: People took 152 ms and a person
toggle 210 ms (above the 100 ms of the risk table). The conversions and folds now use OpenCV
`cv::compare`, `convertTo`, `cv::max`, `cv::min`, and `cv::bitwise_not`; the tests above check the
values. A debug build takes 150-430 ms per step (not representative).

Not taken: the asset row size and the put and read times for the Subject coverage of three
quality photos. They need the real models on the quality photos and the SM5.3 commit path; take
them with SM5.3.

#### 13.2.4 Implementation decisions

- The output map and the runner calls have a separate owner, `AiMaskModelOutputStore`
  (`app/ai_mask_model_output_store`). The session owns the stack and the step rules; the
  composition functions are pure. `AiMaskStepStatus` and `AiMaskStepResult` are declared in the
  store header, which the session header includes.
- Output keys: `{"task": ...}`, `{"person": i}`, and `{"prompt": <components JSON of the group
  prefix>}` with the group operation Add and the level of the last step 0, because the decode
  depends on neither. A level change therefore needs no decode.
- The decoder box of a group prefix is the box of the latest step that has one.
- Three levels exist only when the group prefix is one point without a box (the sidecar click
  rule `is_click`). A second click in an open group decodes two points and has one level.
  `SelectLevel` rejects other groups, a level above 2, and the level that is already selected.
- An unchanged edge, an unchanged input mode, and an invalid edge are `Rejected`; they make no
  stack entry.
- `PersonIncluded` reads the toggles of the last People component of the recipe.
- A session opened with People as the last component has no People list until the People task
  runs: `TogglePerson` runs it first (the replay needs it), then checks the index.
- A reopened session starts in Prompt mode with the stored prompt group open when the last stored
  component is a prompt group (the next prompt step continues that group after the replay), else
  in Task mode.
- A state whose recipe equals the stored recipe shows the stored coverage and commits the stored
  `model_set_id` and `geometry_fingerprint`, because its coverage comes from them. Another state
  commits the session values.
- Alpha grids: the session pixels whose centers lie in `mask_box` get `cv::resize` of the grid to
  that rectangle (the lab `alpha_on_image` mapping for the integer-pixel boxes of the sidecar),
  then `convertTo(CV_8U, 255)`: rounding with ties to even and saturation to [0, 255] (the
  clamp).
- Changed 2026-10-24 by master plan D23 (#522): the alpha grid is upsampled by a joint bilateral
  filter. `AiMaskSessionImage::guide` (from `MakeAiMaskCoverageGuide` of the analysis image, made
  by the AI Mask tool at session start) goes to `AiMaskModelOutputStore` and to
  `ConvertModelOutputToCoverage`. Pixels whose 5 x 5 cell window has one alpha value take it;
  other pixels take the weighted mean of their 4 x 4 nearest cells; the value is then steepened
  around 0.5 by gain = max(1, half the upscale factor). Without a guide, only the spatial weight
  applies. The test `GuidedAlphaEdgeFollowsTheImageEdge` checks low-contrast straight edges at 4x,
  near-vertical and near-horizontal included.
- Model outputs are validated when they arrive: the logit count, the alpha count and box, the Sky
  size equal to the session size, and the decoder mask count equal to the level count. A
  violation is `Failed`.
- `ValidateAiSegmentationMaskSource` is now public in `mask_model.hpp`: the session validates the
  stored source and the commit source without a `MaskModel`.
- `PlaceAiMaskAssetInCoverage` (the inverse of `CropAiMaskCoverage`) is added for M15.
- `AiMaskSegmentationService::RunTask` has no default argument now; the interface method has
  none. The three test calls pass `std::nullopt`.
- Diff size: production C++ about 1,360 lines (above the 650-800 of section 7.2 because of the
  separate output store and the API comments), tests about 820, Rust and proto 39.

### 13.3 SM5.3

```text
Phase / date / status: SM5.3 / 2026-10-23 / complete.
Source revision and branch: base ad964bd7 (main, SM5.2 merged; tested on 96ff8cd5, the same
  tree); branch feature/ai-segmentation-sm5-3-editor-integration.
Actual changed modules: app/editor_action_policy (AiMaskEdit lease), app/editor_session_command_queue
  (three command kinds), app/editor_session_ports (IEditorMaskAssetPort), app/editor_session_service
  and editor_session_bootstrap (Open, Commit, Cancel, mask asset dependency),
  app/editor_mask_creation_controller (CommitAiMaskSource), app/adjustment_transfer_package_builder
  and app/document_transfer (M17), album_backend (EditorSessionMaskAssetPort, module host wiring,
  transfer dialog notice), automation adjustments.copy (skipped_ai_masks), AdjustmentTransferDialog
  QML, zh_CN translation of the notice, tests.
Implemented behavior: M12 (editor side), M16, M17, M18 (editor port), section 5.5; the steps 1 to 6
  of section 7.3.
Explicitly unimplemented items: none of SM5.3. The tool controller and QML that call Open, Commit,
  and Cancel are SM9.
Primary success call chain: section 13.3.1.
Primary failure and restore call chain: section 13.3.1.
Build and test commands with exit codes: section 13.3.2.
Discovered / passed / failed / skipped counts: section 13.3.2.
Manual verification: none; SM9 is the first caller of the editor commands.
Performance or resource evidence, if required: section 13.3.3.
Evidence path: build/tmp/sm5_3/ (removed at completion).
Remaining defects or unavailable platforms: no macOS build in this run (the change has no platform
  code). Issue #484 (pre-existing DocumentTransferTest failure) was found during the work.
```

#### 13.3.1 Call chains

```text
EditorSessionService::OpenAiMaskEdit(grade, mask id?, completion)
  -> SubmitCommand(OpenAiMaskEdit)                            session owner thread, no policy action
  -> no open AI Mask edit -> EvaluateDocumentWriteAdmission(all leases) == CommitAdjustment allowed
  -> SettlePendingInputForBoundary -> AbortMaskCreation (the analytic Mask tool closes)
  -> IEditorPipelinePort::CurrentPreview -> Color Grade -> MakeAiMaskGeometryFingerprint(Develop)
  -> mask id: AI Mask of the Grade -> IEditorMaskAssetPort::ReadMaskAsset(stored key)
  -> AcquireLease(AiMaskEdit, "Commit or cancel the AI Mask edit first")
  -> completion {Accepted, fingerprint, stored source copy, stored asset}
EditorSessionService::CommitAiMaskEdit(EditorAiMaskCommit)
  -> SubmitCommand(CommitAiMaskEdit)                          session owner thread
  -> open edit, same Grade and Mask id -> EvaluateDocumentWriteAdmission(without AiMaskEdit)
  -> ValidateAiSegmentationMaskSource; asset key and descriptor == source key and descriptor
  -> IEditorMaskAssetPort::PutMaskAsset -> EditorSessionMaskAssetPort
     -> PipelineMgmtService::PutMaskAsset -> MaskAssetStore::Put   own DuckDB transaction (M12)
  -> IEditorHistoryPort::WithWorkingDocument
     -> EditorMaskCreationController::CommitAiMaskSource
        new: mask.ai.N, "AI Mask N" -> PublishAddMask
        opened: AI kind check -> ApplyLiveSource -> PublishReplaceSource (equal source: nothing)
        -> settle publisher -> PublishAppliedTypedBatch -> journal append -> live document
        -> ResetMode (Inactive)
  -> EndAiMaskEditOnOwner (lease released)
  -> PublishTypedNodeHistorySuccess: render by the history render reason, history revision
EditorSessionService::CancelAiMaskEdit -> EndAiMaskEditOnOwner -> Accepted; no port call, no batch
Open, Switch, Close, Shutdown -> CloseComparisonOnOwner -> EndAiMaskEditOnOwner
AdjustmentTransferPackageBuilder::Build -> BuildColorGradeEntry
  -> Masks item: every non-AI Mask; only AI Masks: no Mask set -> ValidateDocumentTransfer
ListAiMasksExcludedFromTransfer(document, selection)
  -> AdjustmentTransferDialogModel::excluded_ai_mask_notice -> AdjustmentTransferDialog.qml label
  -> adjustments.copy "skipped_ai_masks"
```

```text
Open refused (another AI edit, a lease, no history, unsettled input, missing Grade or Mask,
  non-AI Mask, unreadable asset) -> completion {Rejected or Failed, reason}; no lease.
Commit refused (no edit, other Grade or Mask, a write lease, invalid source, asset/source
  mismatch, no asset port) -> Rejected; no port call; lease stays.
Asset put fails -> Failed with the store message; document, history, and lease stay.
History publish fails after the put -> PublishAddMask / PublishReplaceSource RestoreLive and
  PublishAppliedTypedBatch rollback -> Failed with the publish message; document, history, and
  lease stay; the stored row is unreferenced until the M11 cleanup at clean close.
Queued slider input, boundary, and Mask commands (except Cancel) during the edit -> Rejected at
  admission.
ImportPackage of a package with an AI Mask -> ValidateDocumentTransfer throws "transfer Color
  Grade contains the AI Mask '<id>'; AI Masks are not transferable".
```

#### 13.3.2 Evidence

Commands (PowerShell, repository root):

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"   # exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionActionPolicyCq3Test EditorComparisonServiceTest AdjustmentTransferServiceTest AdjustmentTransferDialogModelTest AnalyticMaskCreationTest DocumentTransferTest AdjustmentTransferServiceMiniGitTest AutomationAdjustmentCommandsTest AlbumBackendLib alcedo_main   # exit 0 (after formatting)
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(EditorSessionActionPolicyCq3Test|EditorComparisonServiceTest|AdjustmentTransferServiceTest|AdjustmentTransferDialogModelTest|AnalyticMaskCreationTest|DocumentTransferTest|AdjustmentTransferServiceMiniGitTest|AutomationAdjustmentCommandsTest)\."   # exit 8, 130/131 passed (the failure is #484, see below)
```

| Test (section 8.3) | Target | Result |
| --- | --- | --- |
| `AiMaskEditLeaseBlocksDocumentWritesAndHistory` | `EditorSessionActionPolicyCq3Test` | PASS |
| `CommitNewAiMaskStoresAssetThenPublishesAddMask` | `EditorComparisonServiceTest` | PASS |
| `CommitOpenedAiMaskPublishesReplaceMaskSource` | `EditorComparisonServiceTest` | PASS |
| `MainUndoRestoresThePreviousAiSource` | `EditorComparisonServiceTest` | PASS |
| `AssetPutFailureKeepsDocumentAndLease` | `EditorComparisonServiceTest` | PASS |
| `PublishFailureAfterPutKeepsDocument` | `EditorComparisonServiceTest` | PASS |
| `CancelPublishesNothing` | `EditorComparisonServiceTest` | PASS |
| `OpenRejectsMissingOrNonAiMask` | `EditorComparisonServiceTest` | PASS |
| `OpenAiMaskEditRefusesQueuedEditsAndImageChangeEndsIt` (added) | `EditorComparisonServiceTest` | PASS |
| `PasteBuildSkipsAiMasks` | `AdjustmentTransferServiceTest` | PASS |
| `ImportPackageRejectsAiMaskKind` | `AdjustmentTransferServiceTest` | PASS |
| `TransferDialogShowsSkippedAiMasks` | `AdjustmentTransferDialogModelTest` | PASS |

Suite totals: `EditorSessionActionPolicyCq3Test` 15/15, `EditorComparisonServiceTest` 22/22,
`AdjustmentTransferServiceTest` 8/8, `AdjustmentTransferDialogModelTest` 16/16,
`AnalyticMaskCreationTest` 23/23 (the target of the changed Mask creation controller),
`DocumentTransferTest` 33/34, `AdjustmentTransferServiceMiniGitTest` 9/9,
`AutomationAdjustmentCommandsTest` 4/4. The one failure is
`DocumentTransferTest.TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches`: the package has the
Color Wheel parameter key `version`. Clean `96ff8cd5` (changes stashed, only that target rebuilt)
fails the same way; issue #484. `AlbumBackendLib` and `alcedo_main` build. The full suite was not
run (only the user starts it).

#### 13.3.3 Measurements (section 9)

Not taken: the asset row size and the put and read times for the Subject coverage of three
quality photos (deferred from 13.2.3). They need the sidecar models on the quality photos and a
producer of the commit payload; SM5 has no producer (SM9 is the first). The store path itself is
covered by the SM5.1 tests (a 1024 x 768 two-region asset stores less than 1/20 of its pixel
bytes).

#### 13.3.4 Implementation decisions

- `OpenAiMaskEdit` gives its result through a completion (`EditorAiMaskEditOpenCompletion`),
  as `RenderAiMaskAnalysisImage` does: a command that the tool worker thread submits gets only a
  queue acknowledgement, and SM9 needs the fingerprint and the stored source. The result has the
  plan fields and also `stored_asset`: the reopen constructor of `AiMaskEditSession` needs the
  verified asset, so `IEditorMaskAssetPort` also has `ReadMaskAsset`. A missing or corrupt asset
  is an Open error (no fallback).
- The three command kinds have no policy action. The owner evaluates CommitAdjustment admission
  itself: Open with every lease (a second session, the comparison, a save, a Paste, and image
  load all refuse it, and a refused Open still reaches its completion); Commit without the
  AiMaskEdit lease, so a save checkpoint that started during the session still refuses the
  write.
- Open settles pending input (slider and Mask) and closes the analytic Mask tool
  (`AbortMaskCreation`), so the session starts from a committed document. Queued slider input,
  input boundaries, and Mask commands other than Cancel are refused at admission while the edit
  is open, as for the comparison (`ai_mask_edit_active_`).
- Commit checks the source (`ValidateAiSegmentationMaskSource`) and that the asset key and
  descriptor equal the source key and descriptor before the put; the store recomputes the key
  from the pixels.
- Commit success uses `PublishTypedNodeHistorySuccess`: the render follows the render reason that
  the history published. Until SM6 that render reports the M21 error of the compiler.
- A new AI Mask gets the first free id `mask.ai.N` and the name "AI Mask N" with the same N.
  `CommitAiMaskSource` leaves the Mask creation controller Inactive on every return.
- M17: `ValidateDocumentTransfer` (run by Build and by ImportPackage) rejects an AI Mask, instead of
  `MaskModelFromJson`, which also reads documents. A Grade whose selected Masks are all AI Masks
  keeps its entry without a Mask set; destination Masks then do not change.
- The session tests live in `tests/app/editor_ai_mask_edit_service_test.cpp`, compiled into the
  `EditorComparisonServiceTest` binary (the comparison test file is 814 lines). The publish
  failure is made by a read-only journal file (all write bits removed; Windows sets the read-only
  attribute only then).
- The zh_CN translation of the notice is added to `alcedo_main_zh_CN.ts` by hand (no `lupdate`).
- LOC note: `editor_session_service.cpp` is 3,319 lines (3,085 before) and its header 1,201; both
  were above 1,000 lines before SM5.3. The AI Mask edit adds one target struct and two members;
  its asset I/O is in the port and its document change in the Mask creation controller.
- Diff size: production C++ about 640 lines, tests about 670, QML 17, build files 4,
  translation 4.
