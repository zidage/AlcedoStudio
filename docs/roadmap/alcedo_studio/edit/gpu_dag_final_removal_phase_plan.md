# Phase G10 — Legacy Pipeline Removal and Release Qualification

Date: 2026-09-22

Status: **planned**. No G10 sub-phase has implementation evidence.

Parent: [GPU DAG Pipeline Rebuild Phase Plan](gpu_dag_pipeline_rebuild_phase_plan.md),
Section 44 (G10) and Section 47 (global completion criteria).

Direct prerequisites and related plans:

| Plan | Relation to this plan |
| --- | --- |
| [GPU DAG Pipeline Rebuild](gpu_dag_pipeline_rebuild_phase_plan.md) | Parent. This plan replaces the short G10 section with an executable phase split. Sections 6–30 still define Model, DTO, graph, workspace, geometry, Mask, and cache semantics. |
| [GPU DAG OpenCL Migration](gpu_dag_opencl_migration_phase_plan.md) | O0–O5 are complete. This plan executes the O6 removal list (Section 11 of that plan) and O6 performance acceptance. O6 does not run separately. |
| [GPU DAG Metal Migration](gpu_dag_metal_migration_phase_plan.md) | M0–M6 are complete. This plan executes the M7 removal list (Section 12 of that plan) and M7 performance acceptance. M7 does not run separately. |
| [Node-aware Pipeline Editing master plan](node_mask_editor_master_plan.md) | Defines `PipelineDocument` as the only writable edit state (Section 2.1). This plan removes the last stage mirror that the master plan prohibits. |
| [NM4 History, Version, Recovery, and Paste](node_mask_editor/phase_nm4_history_version_paste_plan.md) | Defines typed `PipelineEditBatch` history, root, checkpoint, and project cutover rules. This plan keeps every NM4 hash and payload format. |
| [Native parameter access](node_mask_editor/native_parameter_access.md) | Describes the current edit call chain, including the CPU executor remirror. This plan removes that remirror step and updates the note. |
| [NM10 Node-aware Adjustment Transfer](node_mask_editor/phase_nm10_adjustment_transfer_plan.md) | NM10.5 and later are planned. This plan changes only the Paste mirror and rollback calls that NM10 does not own. |
| [Editor Single Live Pipeline + WAL + Checkpoint](../ui/editor_single_live_pipeline_wal_checkpoint_plan.md) | Its "Final locked identity model" stays binding: history owns HEAD, and the live document owns parameters. |

Source audit revision: `92085ffe` on `main`. The local commit `ed50e111` (a contrast change that
edited legacy operator code) was discarded on 2026-09-22 and is not part of this audit. Cited line
numbers were checked again at `92085ffe`.

---

## 1. Decision record

### 1.1 Approved goal

Alcedo Studio v0.3.0 has complete product features. Legacy pipeline structures and dead code
remain in the source tree. G10 of the GPU DAG plan defined their removal but never ran.
This plan completes G10 so that the project can enter the release-preparation stage.

After G10:

1. The GPU DAG is the only image-processing path for CUDA, OpenCL, and Metal.
2. `PipelineDocument` is the only parameter store. No code mirrors document values into a
   second parameter table.
3. No product source, test, or build file references `PipelineStage`, `PipelineStageName`,
   `OperatorParams`, `IOperatorBase::Apply`, `IOperatorBase::ApplyGPU`, `SetGlobalParams`,
   `EnableGlobalParams`, `GPUPipelineWrapper`, the legacy `OperatorType` enum, or a merged stage.
4. Edit history, Undo, Redo, Version checkout, Paste, reopen, thumbnail, and export keep their
   current user-visible behavior and their current commit and chain hashes.
5. All three backends pass the performance acceptance criteria against their named pre-DAG
   baselines.

### 1.2 User decisions made on 2026-09-22

**Project format.** G10 changes the saved project format to `0.9.0`.

- G10 removes the legacy `EditHistory` table, the legacy `image-N.wal` transaction journal, and
  stage-JSON rollback data.
- A project with metadata version `0.8.0` or older fails at the project-open boundary with an
  explicit unsupported-version error. No converter runs. No history reader runs.
- Commit, chain, batch, root, checkpoint, and Mini-Git WAL formats do not change. Their byte
  layout and hash inputs stay identical. Section 4.3 gives the exact values.

**Legacy code preservation.** Legacy source files are preserved and marked as deprecated.
They must not be part of the C++ compile graph of the project.

- A legacy source file that G10 removes from the build moves unchanged into the deprecated
  archive (Section 4.4).
- No CMake target, source list, include directory, compile definition, install rule, package
  rule, format target, or tidy target references the archive.
- The archive does not need to compile. No product or test file includes an archive file.
- Code that G10 deletes from inside a file that stays in the build is not archived. Git history
  keeps it.

The user stated this rule for the old OpenCL and Metal backend code and confirmed on 2026-09-22
that it applies to every legacy source file that G10 removes as a whole file: CUDA, CPU kernels,
stage, legacy operators, the legacy history store, and `RawProcessor`.

**Geometry panel frame.** The user confirmed on 2026-09-22 that the viewer must show the
uncropped, unrotated source image while the Geometry panel is open. The overlay draws the crop
frame and rotation guides. This restores the behavior that the viewer already expects
(defect D2). G10.1 implements it.

**Existing `gate` identifiers.** The user decided on 2026-09-22 not to rename existing uses in
one sweep. The `AGENTS.md` rule renames a use when its file is touched.

**Performance acceptance.** G10 uses the pinned-commit A/B method.

- The executor removes legacy code from the compile graph without a prior A/B run.
- The executor measures each old path by building its pinned baseline commit on the same device.
- G10.11 cannot be complete until the Windows OpenCL record and the macOS Metal record exist and
  meet the thresholds. The CUDA record is also required (Section 14).

**Terminology.** The user prohibited the word `gate` and its derived forms in this project.
The same change adds the rule to `AGENTS.md`. This plan uses criteria, requirement, check,
checklist, and precondition.

### 1.3 Decisions that must not return

- Do not add a converter from `0.8.0` projects or from stage JSON.
- Do not keep the stage table "for tests" or "for comparison". Tests compare against stored
  expected values or against the DAG.
- Do not keep a build option that can compile the legacy pipeline again.
- Do not replace the stage rollback copy with a new document snapshot. Use build-then-swap
  (Section 6.3).
- Do not add a CPU image-processing path or a cross-backend substitute when a GPU path fails.
- Do not add a generation, epoch, or stale-result guard during this cleanup. No phase has an
  executable interleaving that needs one.

### 1.4 Evidence status at plan creation

- Product rendering already uses only the GPU DAG. `PipelineStage::ApplyStage` has no caller in
  `alcedo_studio/src` (verified by source search at `92085ffe`).
- OpenCL O0–O5 and Metal M0–M6 completion records exist in their plans. This plan does not add
  new evidence for them.
- G7R.4 (creative CAT02) and G7R.5 (observability) are still open in the parent plan. G10 does
  not complete them. Section 4.2 explains the boundary.

---

## 2. Terms

| Term | Meaning in this plan |
| --- | --- |
| Stage table | The seven `PipelineStage` objects, the merged stage, and the `OperatorParams` value that `CPUPipelineExecutor` owns. |
| Stage mirror | Any write of document values into the stage table, and any read of stage-table values that follows. |
| Stage-JSON rollback | A copy of `ExportPipelineParams()` that code restores with `ImportPipelineParams()` after a failure. |
| Legacy history store | `EditHistory`, `Version`, `WorkingVersion`, `EditTransaction`, the `EditHistory` DuckDB table, and the `image-N.wal` transaction journal. It is separate from the Mini-Git commit graph. |
| Deprecated archive | The directory `alcedo_studio/deprecated/legacy_pipeline/` (Section 4.4). |
| Compile graph | All CMake targets, source lists, include paths, compile definitions, custom commands, and generated compile databases of the project. |
| Pinned baseline | A named commit that the executor builds on the same device to measure the old path. |
| DAG-shared helper | A file under a legacy directory that the DAG runtime still includes or compiles. It moves before its directory is archived. |

---

## 3. Product behavior specification

G10 is a removal and relocation plan. It does not change any intended user-visible behavior.

### 3.1 Behavior that must stay identical

| Behavior | Required result after G10 | Evidence |
| --- | --- | --- |
| Slider edit preview and commit | Same pixels, same history row title, same commit hash for the same edit sequence | G10.2 tests |
| Undo, Redo, direct head move | Same document state and chain hash | G10.2 tests |
| Version checkout | Same document after checkout; failure leaves the prior Version active | G10.3 tests |
| Project reopen with a matching checkpoint | Checkpoint is used; no replay | G10.3 tests |
| Project reopen without a matching checkpoint | Replay from the immutable root to the Version head | G10.3 tests |
| Paste as a new Version | Same target document and Version creation | G10.3 tests |
| Thumbnail and export pixels | Match stored expected pixels within the stated tolerance | G10.5, G10.6, G10.11 |
| HDR export flag | Follows the DRT node encoding EOTF | G10.1 test |
| DRT, lens correction, CUDA detail and film grain pixels | Bitwise identical parameter buffers; pixels within the stated tolerance | G10.5, G10.6 |
| Import of RAW and non-RAW files | Same root document and camera profile | G10.1, G10.4 tests |

### 3.2 Behavior that changes

| Behavior | Change | Reason |
| --- | --- | --- |
| Opening a `0.8.0` project | Fails at open with an unsupported-version message | User decision 1.2 |
| OpenCL startup | No longer compiles the legacy `edit_pipeline` programs at startup | Those programs have no product caller |
| Installed OpenCL package | Ships `aces_reference_gamut_compression.h` next to the other DAG sources | Fix for defect D4 (Section 5.4) |
| Viewer frame while the Geometry panel is open | Shows the uncropped, unrotated source; the overlay draws the crop and rotation. Closing the panel shows the document crop again. | User decision 1.2; fix for defect D2 |

### 3.3 Failure behavior

- A failed edit, Undo, Redo, checkout, Paste, or open leaves the prior document and Version
  active. The caller receives the original error text.
- A GPU backend failure throws the real error. No CPU or other-backend path runs.
- A `0.8.0` project reports this message and opens no database table:
  `Project version 0.8.0 is not supported. Alcedo Studio 0.3 opens project version 0.9.0.`
  Keep the existing message format in `project_service.cpp` if it already states both values.

---

## 4. Scope

### 4.1 Included work by module

| Module | Responsibility in G10 |
| --- | --- |
| `app/` editor adjustment, history, pipeline, import, and transfer services | Remove the stage mirror and stage-JSON rollback. Read parameters from `PipelineDocument` only. |
| `ui/alcedo_main/album_backend/` history, session, and transfer ports | Remove mirror calls, legacy snapshots, and the legacy journal writer port. |
| `renderer/` scheduler | Remove stage reads and `OperatorParams` writes. |
| `edit/pipeline/` | Reduce the executor to document binding and renderer dispatch. Move DAG-shared headers out. |
| `edit/runtime/` | Receive the DRT resolver, lens resolver, local-tone header, apply request header, and shared shader sources. |
| `edit/operators/` | Keep only Model, DTO, catalog, and shared data tables. Archive every legacy operator class. |
| `edit/history/` and `storage/` | Remove the legacy history store and the `EditHistory` table. |
| `sleeve/` | Remove the per-file `EditHistory` member. |
| `decoders/` | Archive `RawProcessor`, its backend wrappers, and CPU RAW operators. Keep the RAW color and pattern headers that the DAG uses. |
| `opencl/`, `metal/` build and registry | Remove legacy programs, metallibs, and install entries. |
| Root and module `CMakeLists.txt`, CI workflow | Remove legacy sources, targets, and the legacy journal fuzz step. |
| `alcedo_studio/deprecated/legacy_pipeline/` | Hold the preserved legacy files outside the compile graph. |
| `docs/roadmap/` | Update the parent, OpenCL, and Metal plans and the roadmap index. |

### 4.2 Explicit exclusions

| Excluded work | Owner that keeps it |
| --- | --- |
| G7R.4 creative CAT02 math | Parent plan Section 41.6 |
| G7R.5 observability counters beyond what G10.11 needs for A/B | Parent plan Section 41.7 |
| QML visual changes | No owner. G10 does not change QML files except a renamed C++ symbol, if a QML file names it. |
| Brush Mask work | [Brush Mask Architecture master plan](brush_mask_architecture_master_plan.md) |
| Renaming existing `gate` identifiers in files that G10 does not touch | No sweep (user decision 1.2). The `AGENTS.md` rule renames them when their file is touched. |
| CMake project version value (`0.2.9` in the root `CMakeLists.txt`) | Release preparation, not G10 |

### 4.3 Format values

| Value | Before G10 | After G10 |
| --- | --- | --- |
| `kProjectFileVersion` | `0.8.0` | `0.9.0` |
| `kMinSupportedProjectFileVersion` | `0.8.0` | `0.9.0` |
| `kMaxSupportedProjectFileVersion` | `0.8.0` | `0.9.0` |
| `kPackedProjectFormatVersion` | 7 | 7 (the packed header layout does not change; both open paths check the metadata version) |
| `kPipelineDocumentFormatVersion` | 7 | 7 |
| `kImageEditSchemaVersion` | 5 | 5 |
| `kCommitFormatVersion`, `kChainFormatVersion` | 5, 5 | 5, 5 |
| `kPipelineEditBatchFormatVersion` | 4 | 4 |
| `kRootStateFormatVersion`, `kCheckpointStateFormatVersion` | 5, 5 | 5, 5 |
| `kMiniGitJournalRecordFormatVersion` | 6 | 6 |
| `kAdjustmentTransferSchema` | `alcedo.adjustment_transfer.v6` | unchanged |
| DuckDB `EditHistory` table | created for new projects | not created |

All values are in
[pipeline_history_format.hpp](../../../../alcedo_studio/src/include/edit/history/pipeline_history_format.hpp).
The table DDL is in
[database.hpp](../../../../alcedo_studio/src/include/storage/store/database.hpp).

### 4.4 Deprecated archive rules

Location: `alcedo_studio/deprecated/legacy_pipeline/` (proposed).

The location is outside `alcedo_studio/src` and `alcedo_studio/tests`. The root
`CMakeLists.txt` `format` and `tidy` targets use `GLOB_RECURSE` on those two trees
(lines 1654–1668 at `92085ffe`). A location inside them would enter both targets.

Rules:

1. Keep the original relative path under the archive root. Example:
   `alcedo_studio/src/edit/pipeline/pipeline_stage.cpp` moves to
   `alcedo_studio/deprecated/legacy_pipeline/src/edit/pipeline/pipeline_stage.cpp`.
2. Move files with `git mv` and do not change their content in the same commit. This keeps Git
   rename detection and file history.
3. Add `alcedo_studio/deprecated/legacy_pipeline/README.md` in G10.9. It states:
   - the files are deprecated and are not built, formatted, tidied, installed, or packaged;
   - the files may not compile against the current tree;
   - the commit that removed each group from the compile graph;
   - that restoring any file into the build requires a new approved plan.
4. Do not add a `CMakeLists.txt` inside the archive.
5. Do not add the archive to a CMake `GLOB`, `add_subdirectory`, `include_directories`, or
   `install` rule.
6. The static test `DeprecatedLegacyArchiveIsOutsideCompileGraph` (G10.9) enforces rules 4–5.

---

## 5. Current source audit

All paths are relative to `alcedo_studio/src/` unless stated. Line numbers are at `92085ffe`.

### 5.1 Product render path

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Render entry | `PipelineScheduler::ScheduleTask` → `CPUPipelineExecutor::Apply` (`renderer/pipeline_scheduler.cpp:665`, `edit/pipeline/pipeline_cpu.cpp:231-273`) | Dispatches only to `Renderer<Backend>`. Throws when no document or no supported backend exists. | Keep. Rename the executor in G10.8. |
| Legacy execution | `PipelineStage::ApplyStage` (`edit/pipeline/pipeline_stage.cpp:328`) | No caller in `src/`. Tests call it. | Archive in G10.9. |
| Legacy GPU wrapper | `GPUPipelineWrapper` and `CreateGPUPipeline` (`edit/pipeline/pipeline_gpu_wrapper.cpp`) | Constructed by `PipelineStage::SetAcceleratorBackend`. `Execute` is reachable only through `ApplyStage`. | Archive in G10.9 and G10.10. |
| Executor construction | `CPUPipelineExecutor` constructors (`pipeline_cpu.cpp:153-208`) | Always build seven stages, call `InitDefaultPipeline`, create a merged stage and a `GPUPipelineWrapper` (`:344-381`). | Remove in G10.7. |
| Per-render stage work | `editor_session_render_scheduler_port.cpp:454-477` | Calls `ApplyEditorAdjustmentSnapshot` when `live_parameters_applied` is false, `EnsureLoadingOperatorDefaults`, `DisableEditorGeometryOperatorForOverlay`, and always `AttachExecutionStages` → `SetExecutionStages(sink)`. | Keep only the frame sink attach in G10.1 and G10.2. |
| Scheduler stage reads | `HasActiveGeometryRotation` (`pipeline_scheduler.cpp:33-67`) | Reads the stage `CROP_ROTATE` operator on each FAST_PREVIEW. | Remove in G10.1 (defect D1). |
| Scheduler global writes | `pipeline_scheduler.cpp:169-183, 394` | Writes `render_frame_role_` and `render_hs_preserve_source_detail_` into `OperatorParams`. | Remove in G10.1. |
| Cancel propagation | `SetCancelRequested` → `SyncRawDecodeRuntimeControls` (`pipeline_cpu.cpp:251, 415-431`) | Writes the cancel callback into the stage `RawDecodeOp`. The DAG reads `request.cancel_requested`. | Remove the stage write in G10.1. |
| Post-render scratch | `ReleasePreviewGpuScratch` (`pipeline_scheduler.cpp:517`, `pipeline_cpu.cpp:778-782`) | Touches only the merged stage. | Remove in G10.1. |
| Dead scheduler helpers | `BuildRenderSourceCacheKey` (`:98-129`), `SetExecutorRenderParams`, `ResetPreviewRenderParams`, `ResetThumbnailRenderParams`, `Capture/RestoreOneShotRenderParams` | No product caller. Tests call some. | Remove in G10.1. |

### 5.2 Edit and history mirror

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Live preview write | `EditorHistoryMutation` preview path (`ui/alcedo_main/album_backend/editor_history_mutation.cpp:379-459`) | Writes the document, then `MirrorTargetToExecutor` → `RemirrorEditorParameterToExecutor` (`app/editor_adjustment_pipeline.cpp:508-527`). A mirror failure rolls back the document write (`:437-442`). | G10.2 removes the mirror. |
| Commit | `editor_history_mutation.cpp:503-576` | After `PublishPreparedEdit`, calls `ApplyHistoryCommitToLivePipeline` (`editor_adjustment_pipeline.cpp:470-506`) and `RefreshCommittedSnapshotFromLive` → `MakeAdjustmentSnapshotFromLivePipeline` (`editor_history_shared_helpers.cpp:186-215`). | G10.2 removes both. |
| Undo, Redo, head move | `ApplyCommitToLiveDocument` (`editor_history_mutation.cpp:165-223`) | Applies the batch to the document, then to the stage table (`:216`). | G10.2 removes the stage step. |
| Version head replay | `ApplyVersionHeadToLivePipeline` (`editor_adjustment_pipeline.cpp:556-599`) | Resets stage operators, replays every first-parent commit into stages, then `RemirrorCurrentPanelFromDocument`. Restores `ImportPipelineParams(prior)` on failure. Called from `pipeline_service.cpp:344, 1082` and `editor_history_state_detail.cpp:349`. | G10.3 removes it. |
| Checkout | `PipelineMgmtService::CheckoutVersion` (`app/pipeline_service.cpp:996-1110`) | Replays a new document first, then clones the prior document and exports stage JSON for rollback, swaps, mirrors, and restores both on failure. | G10.3 changes it to build-then-swap without a copy (Section 6.3). |
| Rollback copies | `editor_history_state_detail.cpp:131, 142, 327, 342`; `editor_history_version_refs.cpp:53, 73`; `editor_history_mutation.cpp:1065, 1086` | Capture and restore stage JSON. | G10.3 removes them. |
| Legacy snapshot | `HistoryWorkingState::committed_snapshot`, `root_snapshot` (`include/ui/alcedo_main/album_backend/editor_history_state_detail.hpp:55-56`) | Filled from stage operators. `adjustment_snapshot()` (`app/editor_session_service.cpp:613-625`) has no production consumer. QML reads `panel_projection`. | G10.2 removes both fields and the accessor after a search confirms no production consumer. |
| Paste | `editor_history_transfer.cpp:198` (`RemirrorCurrentPanelFromDocument`), `:288-318` (`CancelLivePaste`), `adjustment_transfer_apply_coordinator.cpp:90, 105` (`RebuildActiveEditorPipeline`) | Mirror after Paste. `CancelLivePaste` restores only the stage snapshot and has no production caller. | G10.3 removes the mirror and `CancelLivePaste`. |
| Field mapping | `FieldSpec` and `EditorAdjustmentFieldKey` (`editor_adjustment_pipeline.cpp:26-97, 172-203`), key renames (`:407-435`) | Map `field_key` to `(PipelineStageName, OperatorType)`. | G10.2 and G10.3 remove the mirror use. G10.7 removes the table. |
| History row presentation | `editor_history_commit_presentation.cpp:171+, 796-799` | Maps `field_key` to `OperatorType` for display names and icons. | G10.7 replaces it with a `field_key` table. Stored data does not change. |

### 5.3 Import, open, and persistence

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Import assembly | `AssembleImportPipelineParams` (`app/import_service.cpp:33-109`) | Writes `source_size`, RAW context, and lens EXIF into stage operators. Only `InjectRawMetadata` reaches the document. | G10.1 keeps only the document camera-profile binding. |
| Camera profile binding | `CPUPipelineExecutor::InjectRawMetadata` (`pipeline_cpu.cpp:706-750`) | Calls `ApplyImportedCameraProfile` on the document (`:43-55`), then writes stage `COLOR_TEMP` and `RAW_DECODE`. | G10.1 moves the document part to an owner operation and removes the stage part. |
| Lens identity | `DevelopNodeModel` (`include/edit/graph/develop_node_model.hpp:75-103, 141-159`) | Empty lens maker and model mean Auto. Develop uses the prepared RAW lens identity. | No change. This proves that stage lens EXIF is not needed. |
| Lens catalog default | `editor_lens_catalog_model.cpp:26` | Uses `pipeline_defaults::MakeDefaultLensCalibParams()` for the default database path. | G10.1 reads the `DevelopNodeModel` default. |
| Open | `PipelineMgmtService::LoadPipeline` (`pipeline_service.cpp:468-646`) | Creates an executor with stages, runs `EnsureDefault*` and `ResyncGlobalParamsFromOperators` (`:595-599`, bodies `:65-172`), loads the format-7 document. | G10.7 removes the stage steps. |
| Editor open | `LoadEditorPipeline` (`:739-829`) | Uses the checkpoint when labels match. Otherwise `ReplayLiveDocumentFromRoot` (`:332-350`), which calls the mirror. | G10.3 removes the mirror call. |
| Stage JSON mapper | `PipelineMapper::FromParams` (`storage/mapper/pipeline/pipeline_mapper.cpp:36-51`) | Imports stage JSON when `format_version < 2`. Callers have no product use. | G10.7 removes the stage import. |
| Legacy history store | `SleeveFile::SetImage`, `Copy` (`sleeve/sleeve_element/sleeve_file.cpp:34-55`); `ElementStore` (`storage/store/sleeve/element_store.cpp:173-175, 238-239, 352-353`); `EditHistoryMapper` (`storage/mapper/sleeve/edit_history/history_mapper.cpp`); `EditHistory` constructor builds a `CPUPipelineExecutor` (`edit/history/edit_history.cpp:25-36`) | Import and file copy create and store an `EditHistory` row. | G10.4 removes the store. |
| Legacy journal | `EditorSessionJournalWriterPort` (`ui/alcedo_main/album_backend/application_module_host.cpp:140-154, 196-197`); save calls `CommitJournalAsync` (`app/editor_save_checkpoint_service.cpp:133-135`); discard calls `DiscardUnflushed` | Opens `editor-journal/image-N.wal`. No code appends records. | G10.4 removes it. |
| Dead history services | `EditHistoryMgmtService`, `EditorHistoryMaterializer`, `ElementStore::MaterializeEditorState` (`app/CMakeLists.txt:300-308`) | Linked only by tests. The materializer writes stage JSON into `PipelineParam`. | G10.4 archives them. |
| Dead importer | `LegacyPipelineImporter` (`edit/graph/legacy_pipeline_importer.cpp`), `AllowsLegacyStageAdapterRemirror` (`edit/graph/pipeline_document.cpp:367`), `mirror_legacy_stage_adapter_` (`include/edit/pipeline/pipeline_cpu.hpp:83`) | No product caller. Production passes `false`. | G10.7 removes the flag. G10.9 archives the importer. |

### 5.4 Observed defects that G10 must handle

Each item is a source fact unless marked as an inference.

| ID | Defect | Evidence | G10 action |
| --- | --- | --- | --- |
| D1 | `HasActiveGeometryRotation` always returns false after any geometry edit. | `ReadEditorParameterJson` returns `ImageGeometryModel` JSON without a `crop_rotate` key (`app/editor_pipeline_command_service.cpp:926-928`). `CpuParamsFromModelJson` does not add the key (`editor_adjustment_pipeline.cpp:407-416`). `CropRotateOp::SetParams` disables itself when the key is missing (`edit/operators/geometry/crop_rotate_op.cpp:557-571`). | G10.1 deletes the check. This keeps the current effective behavior. A test proves that rotated-crop FAST_PREVIEW ROI frames match full-frame pixels. |
| D2 | The Geometry panel overlay does not disable the crop in the DAG render. | `geometry_overlay_only` reaches only `DisableEditorGeometryOperatorForOverlay`, which writes the stage (`editor_adjustment_pipeline.cpp:277-281`). `GraphCompiler` always reads `document.Geometry()` (`edit/runtime/graph_compiler.cpp:73-75`). No runtime file reads the overlay value. `EditorInteractionController::fitFraction` expects the full source while the crop overlay is visible (`ui/editor_rhi/editor_interaction_controller.cpp:781-795`). Inference: the product has shown the cropped frame in the Geometry panel since the CUDA product switch, so the zoom readout and the frame disagree. | G10.1 replaces the stage-only call with a render-request option that renders the uncropped source (Section 6.7). |
| D3 | The HDR export flag reads a stage-table value. | `adjustment_transfer_apply_coordinator.cpp:198` reads `GetGlobalParams().to_output_params_.eotf_`. The DRT node owns `encoding_eotf` (`include/edit/graph/drt_node_model.hpp:104`). Inference: the value can be stale because the ODT mirror does not update `to_output_params_` for every DRT edit. | G10.1 reads the DRT node. A test changes only the document EOTF and checks the flag. |
| D4 | The installed OpenCL package lacks `aces_reference_gamut_compression.h`. | The DAG geometry-camera and DRT programs list `ALCEDO_OPENCL_ACES_RGC_H` (`edit/runtime/opencl/opencl_gpu_dag_programs.cpp:25, 51`). The install rules copy only `dng_profile_gpu_math.h` (root `CMakeLists.txt:937, 1056`). `ResolveOpenClSourcePath` finds sources next to the executable when the build path does not exist (`opencl/opencl_program_library.cpp:76-96`). Inference: an installed package on a machine without the source tree fails to build these two programs. | G10.10 adds the install rule and an installed-package test. |
| D5 | OpenCL startup compiles two programs that nothing uses. | `edit_pipeline_fused` and `edit_pipeline_detail` are `required_at_startup = true` (`edit/pipeline/opencl_pipeline_programs.cpp:35, 52`). | G10.10 removes the manifest. |

### 5.5 DAG dependencies on legacy code

These files block the removal of their legacy directories. Each one moves before its directory
is archived.

| Dependency | Current use | Destination |
| --- | --- | --- |
| `ODT_Op` + `OperatorParams` for DRT tables | `edit/runtime/cuda/cuda_drt_pass.cu:23, 42-46`; `cuda_drt_runtime_state.cuh:7-8, 17-18`; `edit/runtime/metal/metal_drt_params.cpp:10, 41-44`; `edit/runtime/opencl/opencl_drt_params.cpp:14, 166-170` | G10.5: `DrtOutputResolver` in `edit/runtime/drt/` |
| `odt_cpu::Resolve*` math | `edit/operators/cst/open_drt_cpu.{hpp,cpp}`, `aces_odt_cpu.{hpp,cpp}` | G10.5: `edit/runtime/drt/` |
| CUDA `GPU_TO_OUTPUT_Params`, `GPUParamsConverter` | `edit/operators/GPU_kernels/param.cuh:285, 1061-1070`, `fused_param.hpp:327` | G10.5: a DRT-only CUDA parameter header without `op_base.hpp` |
| CUDA DRT device math | `GPU_kernels/color_mgmt/*.cuh` (7 files) | G10.5: `edit/runtime/cuda/drt/` |
| OpenCL DRT parameter structs | `GPU_kernels/opencl_param.hpp:27-170` | G10.5: `include/edit/runtime/opencl/opencl_drt_gpu_params.hpp` |
| OpenCL DRT shader sources | `edit/pipeline/opencl_shader/fused_params.cl`, `common.cl`, `cst.cl` (`opencl_gpu_dag_programs.cpp:47-54`) | G10.5: `edit/runtime/opencl/shader/` |
| `LensCalibOp` resolution | `cuda_develop_pass.cpp:64`, `metal_develop_pass.mm:53`, `opencl_develop_pass.cpp:90`; code at `edit/operators/geometry/lens_calib_op.cpp:42-532, 536-610, 1057-1277` | G10.6: `LensCalibrationResolver` in `edit/runtime/lens/` |
| Lens kernels | `cuda_lens_calib_ops.cu`, `metal_lens_calib.cpp`, `lens_calib.metal`, `opencl_lens_calib_ops.cpp`, `lens_calib.cl`, `cuda_geometry_ops.cu` in target `Operators` | G10.6: target `EditRuntimeLens` (proposed) |
| CUDA detail and film grain helpers | `GPU_kernels/detail.cuh`, `film_grain.cuh` used by `edit/runtime/cuda/cuda_neighbor_grade.cuh:15-16` | G10.6: `edit/runtime/cuda/` |
| Metal PRNG shader | `GPU_kernels/metal_shader/prng.metal` included by `edit/runtime/metal/shader/drt_neighbor.metal:10` | G10.6: `edit/runtime/metal/shader/` |
| Local-tone math header | `include/edit/pipeline/local_tone_mapping.hpp` (10+ runtime files, `graph_compiler.cpp:25`) | G10.6: `include/edit/runtime/local_tone_mapping.hpp` |
| Apply request header | `include/edit/pipeline/pipeline_apply_request.hpp` | G10.6: `include/edit/runtime/pipeline_apply_request.hpp` |
| Scope analyzer | `edit/scope/*` compiled in target `EditPipeline` (`decoders/CMakeLists.txt:200-220`), included by all frame presenters | G10.6: own target `EditScope` (proposed) |
| Data tables | `edit/operators/basic/planckian_locus_table.hpp` (`edit/graph/develop_color_transform.cpp:14`); `basic/camera_matrices.hpp` (`image/metadata_extractor.cpp:31`) | Stay in place. They are data, not operators. G10.9 keeps them in the build. |
| `ColorTempOp` | Calls `ResolveDevelopColorTransform`; the DAG calls that function directly | No move. G10.9 archives the class. |

### 5.6 Legacy files by backend

This list is the archive input for G10.9 and G10.10. Line counts are at `92085ffe`.

- Shared: `edit/pipeline/pipeline_stage.{cpp,hpp}` (915/319), `pipeline_gpu_wrapper.{cpp,hpp}`
  (121/68), `include/edit/pipeline/pipeline.hpp` (41), `tile_scheduler.hpp` (96),
  `highlight_shadow_local_tone.hpp` (49), `default_pipeline_params.hpp` (150).
- CUDA: `pipeline_gpu_impl.cu` (74), `gpu_scheduler.cuh` (303), `kernel_stream_gpu.cuh` (203),
  `cuda_output_texture_tail.cuh` (41), `GPU_kernels/{basic,color,cst,halation,tone_mapping}.cuh`,
  `param.cuh` and `fused_param.hpp` after G10.5, `cuda_downsample.cu`, `cuda_rotate.cu`,
  `cuda_debayer_ahd.cu`.
- OpenCL: `pipeline_opencl_impl.cpp` (753), `pipeline_opencl_param.cpp` (16),
  `opencl_kernel_dispatch.hpp` (96), `opencl_pipeline_programs.{cpp,hpp}` (61/47),
  `GPU_kernels/opencl_param.hpp` after G10.5, `highlight_shadow_local_tone_opencl.{cpp,hpp}`
  (462/84), nine legacy `.cl` files under `edit/pipeline/opencl_shader/`, RAW-processor-only
  OpenCL wrappers.
- Metal: `pipeline_metal_impl.cpp` (640), `metal_kernel_dispatch.hpp` (131),
  `metal_pipeline_stats.hpp` (33), `GPU_kernels/metal_param.hpp` (689),
  `highlight_shadow_local_tone_metal.{cpp,hpp}` (696/98), `fused_pipeline.metal` (777),
  eight legacy shaders under `GPU_kernels/metal_shader/`, RAW-processor-only Metal wrappers.
- CPU: all of `edit/operators/CPU_kernels/`, `decoders/processor/operators/cpu/*`,
  `raw_processor.{cpp,hpp}`, `raw_processor_internal.hpp`, `raw_processor_{cuda,metal,opencl}.cpp`.
- Operators: `op_base.hpp`, `operator_factory.{hpp,cpp}`, and the 25 legacy operator classes
  listed in Section 5.7.

### 5.7 Legacy operator classes

`RawDecodeOp`, `ResizeOp`, `CropRotateOp`, `LensCalibOp` (after G10.6), `ExposureOp`,
`ContrastOp`, `WhiteOp`, `BlackOp`, `ShadowsOp`, `HighlightsOp`, `ColorTempOp`, `HLSOp`,
`SaturationOp`, `TintOp`, `VibranceOp`, `CurveOp`, `ClarityOp`, `SharpenOp`, `ColorWheelOp`,
`OCIO_ACES_Transform_Op`, `OCIO_LMT_Transform_Op`, `ODT_Op` (after G10.5), `FilmGrainOp`,
`HalationOp`, and `CVCvtColorOp` (not compiled today).

The new Model layer (`include/edit/operators/models/`, `include/edit/graph/`) includes no legacy
operator header. It stays.

### 5.8 Tests that use legacy types

| Group | Targets or files | G10 action |
| --- | --- | --- |
| Stage execution | `PipelineFrameSinkTest`, `EditorGeometryOverlayPipelineTest`, `PipelineSchedulerRequestIdTest` (stage reads) | Rewrite against the DAG in the phase that removes the API; archive stage-only cases. |
| Mirror APIs | `EditorSessionHistoryPortTest`, `EditorAdjustmentPipelineTest`, `EditorRenderCoordinatorTest` | Rewrite in G10.2 and G10.3 to assert document state. |
| Stage JSON | `PipelineMapperTest` (pipeline service cases), `ThumbnailServiceTest`, `PipelineDocumentRenderTest` | Rewrite in G10.3 and G10.7. |
| Legacy history | `EditHistoryMgmtServiceTest`, `SleeveServiceTest` history cases, `version_hash_test`, `EditorHistoryMaterializerTest`, `EditorTransactionJournalTest`, `EditorJournalWriterTest`, `EditorJournalFuzzFrameworkTest`, `EditorSessionJournalWriterPortTest` | Archive in G10.4. |
| Legacy operator and kernel | `ODTOpTest`, `CropRotateOpTest`, `FilmGrainOpTest`, `HalationOpTest`, `ToneMappingFacadeTest`, `ToneMappingOwnershipTest`, `SharedToneCurveTest`, the local-tone constant test (`tests/edit/CMakeLists.txt:100`), the CUDA ODT stage test (`tests/cuda/CMakeLists.txt:48`), `FilmGrainCudaStageTest`, `HalationCudaStageTest`, `CudaPreviewVramReclamationTest` | Archive in G10.9. Replace coverage with G10.5 and G10.6 resolver tests. |
| Legacy RAW and OpenCL | `OpenClRuntimeTest` legacy program cases, `OpenClCudaPipelineCompareTest`, `OpenClCudaFullPipelineBenchmark`, `ColorTempCudaSanityTest`, `RawProcessorCropTest`, `CudaRawOpsTest`, `MetalRawOpsTest`, `OpenClRawNeuralTest`, `OpenClCudaToLinearRefCompareTest`, `OpenClCudaRcdCompareTest`, `DagRawDump --sequential` | G10.10 archives RawProcessor-only cases and replaces parity references (Section 6.6). |
| RawProcessor as a parity reference | `GpuDagMetalDevelopTest` and `decoded_rgb_test_support.hpp` users | G10.10 replaces the reference with stored expected pixel files. |
| Unregistered test sources | `tests/opencl/opencl_fused_edit_pipeline_test.cpp`, `tests/edit/pipeline/{cpu_pipeline,pipeline_serial,pipeline_scheduler,tile}_test.cpp`, `tests/edit/operators/**/*_op_test.cpp`, `tests/edit/history/{transaction,version,history}_test.cpp`, `tests/raw/metal_*` preview tests, `tests/raw/opencl_raw_ops_test.cpp` | Archive in the phase that archives their subject. |

Several archived targets contain the prohibited word `smoke`. Archiving removes them from the
build. Do not create a new test with that word.

---

## 6. Target architecture

### 6.1 Owners after G10

| Owner | Input | Output | Changes | Reads only | Lifetime | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| `PipelineDocument` | Typed setters, `PipelineEditBatch` apply | Model values, JSON | All edit parameters | — | One live instance per `PipelineGuard` | Validation errors from setters and batch apply |
| `PipelineMgmtService` | Element id, storage | `PipelineGuard` with document, executor, commit graph | Guard fields, live document pointer | Storage rows | Cached per element id | Load, replay, and checkout errors returned as text |
| `PipelineExecutor` (renamed in G10.8) | Bound document, render request | Rendered frame or host image | Renderer instances, frame sink binding | Document | Owned by `PipelineGuard` | Throws the GPU or input error |
| `Renderer<Backend>` | Document, request | Presented or downloaded frame | Session caches, workspace | Document | Owned by the executor | Throws |
| `DrtOutputResolver` (proposed, G10.5) | `DrtNodeModel` parameters | `DrtOutputRuntime` value | Nothing | DRT parameters | Value result per call | Returns an error for invalid parameters; no default substitute |
| `LensCalibrationResolver` (proposed, G10.6) | `DevelopPayload`, prepared RAW lens identity | `LensCalibGpuParams` value or a "no profile" result | Nothing | Lensfun database | Database cache owned by the resolver | Returns an error when the database cannot load |
| Mini-Git history (`CommitGraph`, WAL) | Batches | Head, chain hash | History rows | — | Per editor session | Existing NM4 errors |

### 6.2 Edit call chain after G10.2

```text
QML submitWrite
  -> EditorSessionController::submitWrite
  -> EditorSessionService::EnqueueAdjustmentInput
  -> EditorSessionEditController::HandlePendingSequence
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
       ReadEditorParameterJson(document)            (before value)
       ApplyEditorParameterWrite(document)          (only write)
  -> render request with live_parameters_applied
  -> EditorHistoryMutation::CommitAdjustment
       MakeSetParameterBatch
       PrepareAppendEdit -> PublishPreparedEdit     (WAL append, head move, chain fold)
  -> ProjectDocumentEdit -> panel_projection
```

No step writes a stage operator. No step reads a stage operator.

### 6.3 Checkout and replay without a rollback copy

The current checkout already replays into a separate document before the swap. After the stage
mirror is removed, the only work after the swap is the camera-profile binding. G10.3 moves that
binding before the swap.

```text
CheckoutVersion(version_id)
  -> read target head from CommitGraph
  -> decode immutable root state
  -> ReplayPipelineDocumentFromRoot(root, first-parent commits)   (new document; may fail)
  -> BindRootCameraProfile(new document, root raw color context)  (may fail)
  -> under render lock:
       graph.SetActiveVersionId(version_id)
       BindLivePipelineDocument(guard, new document)             (no throw)
  -> mark checkpoint write-back and dirty
```

Failure before the swap returns the error. The prior document and Version stay active. No clone of
the prior document and no stage JSON is taken. `BindLivePipelineDocument` must not throw: it
moves a `shared_ptr` and forwards it to existing renderers. G10.3 adds `noexcept` where the call
chain allows it and a test that proves the prior state after an injected replay failure.

The same pattern applies to `ReplayLiveDocumentFromRoot`, `RebuildActiveEditorPipeline`, Version
ref restore, and Paste.

### 6.4 Camera profile binding

`ApplyImportedCameraProfile` (`pipeline_cpu.cpp:43-55`) becomes a free function on the document
owner: `BindImportedCameraProfile(PipelineDocument&, const RawRuntimeColorContext&)` in
`include/edit/graph/develop_color_transform.hpp` (proposed; this header already owns
`BindDevelopCameraProfile`). It replaces its parameters in one `ReplaceParams` call and does not
mark the document dirty when the value is equal. Import, open, replay, and checkout call it.
`InjectRawMetadata` is removed from the executor in G10.7.

### 6.5 DRT output resolution

```text
DrtNodeModel::Params()
  -> DrtOutputResolver::Resolve(params)            (CPU, no OperatorParams)
       ACES 2.0: ResolveAcesOdtRuntime
       OpenDRT:  ResolveOpenDrtRuntime, ResolveOpenDrtDisplayLinearScale
  -> DrtOutputRuntime                              (display gamut, EOTF, tables)
  -> backend packer
       CUDA:   PackCudaDrtParams   -> CudaDrtGpuParams
       Metal:  PackMetalDrtParams  -> MetalDrtGpuParams (existing struct)
       OpenCL: PackOpenClDrtParams -> OpenClDrtGpuParams
  -> ParameterArena slot -> dirty-range upload
```

`DrtOutputRuntime` replaces the use of `ColorUtils::TO_OUTPUT_Params` only if that struct
depends on `OperatorParams`. It does not: `utils/color_utils.hpp` defines it and the UI already
uses it. Reuse `TO_OUTPUT_Params` as the resolver output. Do not add a parallel struct.

### 6.6 RawProcessor parity references

`GpuDagMetalDevelopTest` and tests that use `decoded_rgb_test_support.hpp` compare DAG output
with `RawProcessor` output. G10.10 archives `RawProcessor`. Before that, G10.10 generates stored
expected pixel files from the current `RawProcessor` path at the pinned revision and commits
them under `alcedo_studio/tests/resources/expected_pixels/develop/` (proposed). Each file name
states the fixture, DecodeRes, and demosaic method, for example
`ci_dng_half_rcd_expected_linear_rgb.exr`. Each test states the comparison rule and tolerance.

### 6.7 Uncropped source frame for the Geometry panel

`RenderRequest` (`include/edit/geometry/render_request.hpp:64-68`) gains one field:

```cpp
enum class DocumentGeometryUse : std::uint8_t {
  ApplyCropAndRotation,  // default: every product render
  UncroppedSource,       // Geometry panel open: identity crop, zero rotation
};

struct RenderRequest {
  ViewRequest         view{};
  ResolutionRequest   resolution{};
  SamplingFootprint   footprint{};
  DocumentGeometryUse document_geometry = DocumentGeometryUse::ApplyCropAndRotation;
};
```

`GraphCompiler::BindFrameGeometry` (`edit/runtime/graph_compiler.cpp:500-508`) passes identity
`ImageGeometryParams` (crop rect `[0, 0, 1, 1]`, rotation 0, `expand_to_fit` from the document)
to `ResolveRenderGeometry` when the request asks for `UncroppedSource`. The document does not
change. No edit and no history commit happens.

The `geometry.scene_source` content key already includes the resolved geometry. The uncropped
frame and the cropped frame therefore have different keys and never share a cached result.
`develop.sensor_linear` does not depend on geometry and stays reusable.

Call chain:

```text
EditorSessionController (active panel == Geometry)
  -> EditorSessionService::SetGeometryOverlayActive(true)
  -> EditorSessionRenderController: intent.geometry_overlay_only = true
  -> EditorSessionRenderSchedulerPort: render description document_geometry = UncroppedSource
  -> PipelineTask::MakeApplyRequest: request.geometry.document_geometry = UncroppedSource
  -> Renderer<Backend>::Render -> GraphCompiler::BindFrameGeometry (identity user geometry)
  -> Geometry, CameraColor, Grade, DRT run; SensorDevelop is reused
```

Only editor viewport requests set `UncroppedSource`. Thumbnail, export, analysis, and Mask
thumbnail requests keep the default. The scheduler does not infer the value from other state.

---

## 7. File and API map

### 7.1 Current files that change

| File | Phases | Change |
| --- | --- | --- |
| `app/editor_adjustment_pipeline.{hpp,cpp}` | G10.1–G10.3, G10.7 | Remove mirror, rollback, stage helpers, `FieldSpec`. |
| `app/pipeline_service.{hpp,cpp}` | G10.3, G10.7 | Build-then-swap; remove stage defaults. |
| `app/import_service.cpp` | G10.1, G10.4 | Document-only import assembly; no `EditHistory`. |
| `app/editor_save_checkpoint_service.cpp` | G10.4 | Remove legacy journal commit. |
| `app/editor_session_*` controllers | G10.2, G10.4 | Remove legacy journal discard and snapshot accessors. |
| `ui/alcedo_main/album_backend/editor_history_*.cpp` | G10.2, G10.3 | Remove mirror and rollback. |
| `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp` | G10.1, G10.2 | Keep only frame sink attach under the render lock. |
| `ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp` | G10.1, G10.3 | Read EOTF from the DRT node; remove mirror. |
| `ui/alcedo_main/album_backend/application_module_host.cpp` | G10.4 | Remove the legacy journal writer port. |
| `ui/alcedo_main/editor_support/controllers/pipeline_controller.cpp` | G10.1, G10.7 | Remove stage defaults and stage attach. |
| `ui/alcedo_main/main.cpp`, `ui/alcedo_studio_test_host/main.cpp` | G10.7 | Remove `RegisterAllOperators`. |
| `renderer/pipeline_scheduler.cpp`, `include/renderer/pipeline_task.hpp` | G10.1, G10.6 | Remove stage reads and writes; include moved headers. |
| `edit/pipeline/pipeline_cpu.{hpp,cpp}` | G10.1, G10.7, G10.8 | Reduce, then rename to `pipeline_executor.{hpp,cpp}`. |
| `edit/runtime/{cuda,metal,opencl}/*drt*` | G10.5 | Use `DrtOutputResolver`. |
| `edit/runtime/{cuda,metal,opencl}/*develop_pass*` | G10.6 | Use `LensCalibrationResolver`. |
| `include/edit/history/pipeline_history_format.hpp` | G10.4 | Project version `0.9.0`. |
| `include/storage/store/database.hpp`, `storage/store/sleeve/element_store.cpp` | G10.4 | Remove `EditHistory` table and store calls. |
| `sleeve/sleeve_element/sleeve_file.{hpp,cpp}`, `sleeve/sleeve_filesystem.cpp` | G10.4 | Remove `edit_history_`. |
| `decoders/CMakeLists.txt`, `edit/CMakeLists.txt`, `opencl/CMakeLists.txt`, `metal/CMakeLists.txt`, `app/CMakeLists.txt`, root `CMakeLists.txt`, test `CMakeLists.txt` files | G10.4–G10.10 | Remove legacy sources and targets; add moved files. |
| `.github/workflows/cpp-ci.yml` | G10.4 | Remove the legacy journal fuzz step (lines 86–90 at `92085ffe`). |

### 7.2 Proposed files and APIs

| API | Input | Output | Ownership | Validation | Error | Changes state |
| --- | --- | --- | --- | --- | --- | --- |
| `BindImportedCameraProfile(PipelineDocument&, const RawRuntimeColorContext&) -> void` | Document, RAW color context | — | Caller owns the document under its render lock | Reuses `BindDevelopCameraProfile` rules | Throws on invalid matrices, same as today | Yes, Develop parameters |
| `IsHdrExportEncoding(const DrtNodeModel&) -> bool` | DRT node | bool | — | — | — | No |
| `DrtOutputResolver::Resolve(const DrtNodeParams&) -> std::expected-like result<ColorUtils::TO_OUTPUT_Params>` | DRT parameters | Resolved output transform | Value | Validates method, gamut, EOTF, peak luminance | Returns error text; no default | No |
| `PackCudaDrtParams(const ColorUtils::TO_OUTPUT_Params&) -> CudaDrtGpuParams` | Resolved transform | CUDA struct | Value | — | — | No |
| `PackOpenClDrtParams(const ColorUtils::TO_OUTPUT_Params&) -> OpenClDrtGpuParams` | Resolved transform | OpenCL struct | Value | — | — | No |
| `LensCalibrationResolver::Resolve(const DevelopPayload&, const PreparedLensIdentity&) -> result<std::optional<LensCalibGpuParams>>` | Develop parameters, RAW lens identity | GPU lens parameters or no profile | Resolver owns the database cache | Validates database path and projection | Error when the database cannot load | Database cache only |

Use the project's existing result or error-out-parameter style at each site. Do not add a new
error framework. Proposed files:

- `alcedo_studio/src/include/edit/runtime/drt/drt_output_resolver.hpp`
- `alcedo_studio/src/edit/runtime/drt/drt_output_resolver.cpp`
- `alcedo_studio/src/edit/runtime/drt/{open_drt_runtime,aces_odt_runtime}.{hpp,cpp}` (moved math)
- `alcedo_studio/src/include/edit/runtime/cuda/cuda_drt_gpu_params.cuh`
- `alcedo_studio/src/include/edit/runtime/opencl/opencl_drt_gpu_params.hpp`
- `alcedo_studio/src/include/edit/runtime/lens/lens_calibration_resolver.hpp`
- `alcedo_studio/src/edit/runtime/lens/lens_calibration_resolver.cpp`
- `alcedo_studio/src/include/edit/runtime/local_tone_mapping.hpp` (moved)
- `alcedo_studio/src/include/edit/runtime/pipeline_apply_request.hpp` (moved)
- `alcedo_studio/deprecated/legacy_pipeline/README.md`
- `alcedo_studio/tests/ci/legacy_removal_source_checks.cmake` (static checks)

Follow the include rule in `AGENTS.md`: include the defining header. Do not add forward
declarations to remove includes.

---

## 8. Implementation entry requirements

Before each phase starts, the executor must:

1. Read `AGENTS.md` again. Read `alcedo-msvc-cmake`, `opencl-program-registry`,
   `raw-processor-module`, and `execute-phase-plan` skills when the phase touches their scope.
   Read `grill-code-review` before review.
2. Confirm that the branch contains every earlier G10 phase.
3. Search the source for each API that the phase removes. Stop and update this plan when a new
   production caller exists.
4. Build the phase test targets once on the unchanged branch and record the counts. A phase
   cannot report a regression-free result without this record.
5. Put all logs under `build/tmp/g10_<phase>/`.
6. Use the PowerShell tool for MSVC builds on Windows. The Bash tool runs the root `Makefile`
   instead of CMake for `cmd /c scripts\msvc_env.cmd` commands.
7. Add `vcpkg_installed/x64-windows/debug/bin` (or the configured vcpkg debug `bin`) to `PATH`
   before `ctest`. Qt is statically linked. `gtest_discover_tests` reports `_NOT_BUILT`
   without the DLL path.

---

## 9. Phase summary and size limit

Counting rule: count added, removed, and changed lines in production code, tests, build files,
resources, and documentation. A `git mv` into the archive without content change counts as zero
lines. Generated expected-pixel files and temporary evidence do not count.

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| G10.1 | No product code reads stage-table values except the mirror itself; Geometry panel shows the uncropped source | scheduler, import, transfer, lens catalog, executor, render request | — | 1200–1700 | planned |
| G10.2 | Live edit, commit, Undo, Redo use the document only | history mutation, edit controller, render port | G10.1 | 1200–1800 | planned |
| G10.3 | Open, checkout, rebuild, Version refs, Paste use build-then-swap without stage JSON | pipeline service, history state, transfer | G10.2 | 1200–1800 | planned |
| G10.4 | Legacy history store removed; project format `0.9.0` | sleeve, storage, history, journal, CI | G10.3 | 900–1500 | planned |
| G10.5 | DRT resolution moved out of `ODT_Op` and `OperatorParams` on three backends | runtime DRT | G10.1 | 1300–1900 | planned |
| G10.6 | Lens resolver, CUDA detail and grain helpers, shared headers, shaders, and scope target moved | runtime, CMake | G10.5 | 1000–1700 | planned |
| G10.7 | Executor and services have no stage table; history presentation uses `field_key` | executor, services, presentation | G10.3, G10.6 | 1400–1900 | planned |
| G10.8 | `CPUPipelineExecutor` renamed to `PipelineExecutor` | all users | G10.7 | 500–900 | planned |
| G10.9 | Shared, CUDA, CPU, and operator legacy files archived out of the compile graph | CMake, archive | G10.8 | 700–1300 | planned |
| G10.10 | OpenCL, Metal, and RawProcessor legacy files archived; packaging fixed | CMake, registry, install, tests | G10.9 | 900–1600 | planned |
| G10.11 | Static checks, full suites, installed packages, three-backend A/B, plan records | tests, docs | G10.10 | 600–1100 | planned |

Split reasons:

- G10.2 and G10.3 are separate because the stage table has no reader after G10.1. A partial
  mirror is write-only state and cannot change output. Each half is reviewable alone.
- G10.5 and G10.6 are separate because DRT touches three backends and parameter packing. Lens
  and header moves are independent build changes.
- G10.7 and G10.8 are separate because the rename touches about 55 files mechanically. It must
  not hide logic changes.
- G10.9 and G10.10 are separate because OpenCL and Metal need their own platform builds.

G10.5 and G10.6 can run in parallel with G10.2–G10.4 on a separate branch. G10.7 needs both lines.

---

## 10. Phase G10.1 — Remove product reads of stage-table values

### 10.1 Objective and deliverables

After this phase, no product code reads a stage-table value. The mirror writes remain, but
nothing consumes them. A reviewer can observe:

- the scheduler has no stage read and no `OperatorParams` write;
- the HDR export flag comes from the DRT node;
- import and open bind the camera profile on the document only;
- the viewer shows the uncropped source while the Geometry panel is open;
- a source check fails if a product file outside the mirror calls a stage read API.

### 10.2 Inputs and prerequisites

- Source at `92085ffe` or later with the audit facts of Sections 5.1–5.4.
- No earlier G10 phase.

### 10.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `renderer/pipeline_scheduler.cpp` | Delete `HasActiveGeometryRotation`, its two uses, `BuildRenderSourceCacheKey`, the `OperatorParams` writes at `:169-183, 394`, the `ReleasePreviewGpuScratch` call, and the dead helpers in Section 5.1. |
| `edit/pipeline/pipeline_cpu.{hpp,cpp}` | Delete `SyncRawDecodeRuntimeControls` and its call in `SetCancelRequested`. Delete `ReleasePreviewGpuScratch`. |
| `ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp` | Use `IsHdrExportEncoding(*document.Drt())`. |
| `include/edit/graph/drt_node_model.hpp` or an existing DRT helper header | Add `IsHdrExportEncoding` (proposed) next to the EOTF enum. |
| `include/edit/graph/develop_color_transform.hpp`, `edit/graph/develop_color_transform.cpp` | Add `BindImportedCameraProfile` (Section 6.4). |
| `edit/pipeline/pipeline_cpu.cpp` | `InjectRawMetadata` calls `BindImportedCameraProfile`. Its stage writes stay until G10.7, because the mirror still writes those operators. |
| `app/import_service.cpp` | Remove the `source_size`, `RAW_DECODE` context, and lens EXIF stage writes. Keep the call that binds the camera profile. |
| `ui/alcedo_main/album_backend/editor_lens_catalog_model.cpp` | Read the default lens database path from `DevelopNodeModel` defaults. |
| `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp` | Remove the `DisableEditorGeometryOperatorForOverlay` call. Set the render description `document_geometry` from `intent.geometry_overlay_only`. |
| `include/edit/geometry/render_request.hpp` | Add `DocumentGeometryUse` and the `document_geometry` field (Section 6.7). |
| `edit/runtime/graph_compiler.cpp` | Use identity user geometry in `BindFrameGeometry` for `UncroppedSource`. |
| `renderer/pipeline_scheduler.cpp` and the render description type | Carry `document_geometry` from the render description into `request.geometry`. |
| `app/editor_adjustment_pipeline.{hpp,cpp}` | Delete `DisableEditorGeometryOperatorForOverlay`. |
| `tests/ci/legacy_removal_source_checks.cmake` (proposed) and its `add_test` registration | Add the first source check. |

### 10.4 Data rules and invariants

- The document is the only source for geometry, DRT EOTF, camera profile, and lens identity.
- Removing the rotation check must keep today's effective request: the rotation flag is false
  for every request today (defect D1). The resulting FAST_PREVIEW request must be identical to
  the current request for rotated and unrotated documents.
- While the Geometry panel is open, the editor frame is the uncropped, unrotated source. Its
  extent has the source aspect ratio. When the panel closes, the next frame applies the document
  crop and rotation again.
- `UncroppedSource` never changes the document, the history, the checkpoint, or any non-editor
  render.
- `BindImportedCameraProfile` must produce the same Develop parameters as
  `ApplyImportedCameraProfile` for the same input.

### 10.5 Implementation steps

1. Add `BindImportedCameraProfile`. Move the body of `ApplyImportedCameraProfile` into it. Make
   `InjectRawMetadata` call it. Delete the anonymous-namespace function.
2. Add `IsHdrExportEncoding`. Replace the global read in the transfer coordinator. The
   coordinator reads the document under the same guard it uses today.
3. Replace the lens catalog default read.
4. Remove the import stage writes for `source_size`, RAW context, and lens EXIF. Keep the
   `InjectRawMetadata` call. Keep the executor defaults that the mirror still needs.
5. Delete the scheduler rotation check and pass `false` semantics directly: the viewport region
   loads when `viewport_region_render` is true. Delete the `OperatorParams` writes.
6. Delete `SyncRawDecodeRuntimeControls`. `Apply` stores the cancel callback only in the request.
7. Delete `ReleasePreviewGpuScratch` and its call.
8. Delete the overlay stage call and function. Add `DocumentGeometryUse` to `RenderRequest`,
   carry it from the editor intent to the request, and apply it in `BindFrameGeometry`.
9. Delete the dead scheduler helpers and update their tests.
10. Add the source check `NoProductCodeReadsStageTableOutsideMirror`. It scans
    `alcedo_studio/src` for `GetStage(`, `GetGlobalParams(`, and `GetOperator(` and allows only
    the files that still host the mirror (`app/editor_adjustment_pipeline.cpp`,
    `app/pipeline_service.cpp`, `app/import_service.cpp`, `edit/pipeline/pipeline_cpu.cpp`,
    `ui/alcedo_main/editor_support/controllers/pipeline_controller.cpp`,
    `ui/alcedo_main/album_backend/editor_history_shared_helpers.cpp`,
    `edit/pipeline/pipeline_stage.*`, `edit/history/edit_transaction.cpp`, and
    `edit/operators/**`). Later phases shrink this list to zero.

### 10.6 Primary success call chain

```text
Import RAW file
  -> ImportService::AssembleImportPipelineParams
  -> PipelineMgmtService::LoadPipeline (document bound)
  -> CPUPipelineExecutor::InjectRawMetadata
       -> BindImportedCameraProfile(document, ctx)
  -> PersistAssembledImportPipeline -> SyncPipelineDocument

HDR flag after DRT EOTF edit
  -> ApplyEditorParameterWrite(document, drt.encoding_eotf)
  -> AdjustmentTransferApplyCoordinator
       -> IsHdrExportEncoding(document.Drt()) -> PersistImageHdrFlag

Geometry panel open
  -> see Section 6.7
```

### 10.7 Primary failure and restore call chain

```text
BindImportedCameraProfile throws (invalid matrices)
  -> import item fails with the thrown message
  -> no document write happens (ReplaceParams was not reached)
  -> existing import error reporting publishes the failure
```

### 10.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `FastPreviewRequestIsUnchangedForRotatedCrop` | `PipelineSchedulerRequestIdTest` | For a document with a 7° rotation and a crop, the FAST_PREVIEW request has the same presentation mode, viewport, max edge, and decode resolution as the current code produces (values recorded in the test). |
| `RotatedCropFastPreviewRoiMatchesFullFramePixels` | `GpuDagCudaDrtProductTest` | A ROI FAST_PREVIEW frame of a rotated crop matches the same region of the full-frame render within 1/1024 per channel. |
| `HdrExportFlagFollowsDocumentDrtEncoding` | `AdjustmentTransferServiceTest` or the coordinator test target | Setting only the document EOTF to PQ sets the flag; setting it to Gamma 2.2 clears it. |
| `ImportBindsCameraProfileOnDocumentOnly` | `ImportPipelineDocumentTest` | After import, Develop camera matrices equal the expected values; the test does not read a stage. |
| `BindImportedCameraProfileMatchesPreviousDevelopParameters` | `GpuDagModelGraphTest` | For three fixtures (DNG, non-DNG RAW, non-RAW), the new function produces Develop JSON equal to stored expected JSON captured from `ApplyImportedCameraProfile` at `92085ffe`. |
| `LensCatalogDefaultPathComesFromDevelopDefaults` | `EditorAdjustmentContextTest` | The catalog database path equals `DevelopNodeModel` default. |
| `CancelRequestReachesRendererWithoutStageWrite` | `GpuDagCudaDrtProductTest` | A cancel callback set on the request stops the render with the existing cancel result; no stage API is called (the test uses no stage accessor). |
| `NoProductCodeReadsStageTableOutsideMirror` | `ctest` script test | The scan finds no match outside the allowed list. |
| `UncroppedSourceRequestIgnoresDocumentCropAndRotation` | `GpuDagGeometryTest` | With crop `[0.2, 0.1, 0.5, 0.6]` and rotation 7 degrees, `BindFrameGeometry` with `UncroppedSource` resolves the same geometry as a document with identity geometry; the document JSON is unchanged. |
| `GeometryPanelFrameShowsUncroppedSourceAndReusesSensorDevelop` | `GpuDagCudaDrtProductTest` | After a cropped render, an `UncroppedSource` render has the source aspect ratio, matches the identity-geometry render within 1/1024 per channel, and skips SensorDevelop. |
| `ClosingGeometryPanelRendersDocumentCropAgain` | `GpuDagCudaDrtProductTest` | The next default request returns the cropped extent and matches the first cropped render bitwise. |
| `GeometryOverlayIntentSetsUncroppedSourceOnlyForEditorRequests` | `EditorSessionRenderSchedulerPortTest` | The editor request carries `UncroppedSource` while the overlay is active; thumbnail and export requests carry the default. |

### 10.9 Build and run commands

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineSchedulerRequestIdTest GpuDagCudaDrtProductTest GpuDagGeometryTest EditorSessionRenderSchedulerPortTest ImportPipelineDocumentTest GpuDagModelGraphTest EditorAdjustmentContextTest AdjustmentTransferServiceTest alcedo_main
ctest --test-dir build/debug --output-on-failure -R "PipelineSchedulerRequestIdTest|GpuDagCudaDrtProductTest|GpuDagGeometryTest|EditorSessionRenderSchedulerPortTest|ImportPipelineDocumentTest|GpuDagModelGraphTest|EditorAdjustmentContextTest|AdjustmentTransferServiceTest|NoProductCodeReadsStageTableOutsideMirror"
```

### 10.10 Exit criteria

- [ ] All twelve tests pass and are discovered by `ctest`.
- [ ] Manual check on Windows CUDA: open the Geometry panel on a cropped and rotated image; the
      viewer shows the full source with the crop overlay, and the zoom readout matches the frame.
      Close the panel; the viewer shows the crop. Record it as manual evidence.
- [ ] `alcedo_main` builds on `win_debug`.
- [ ] The source check passes.
- [ ] `git diff --name-only -- alcedo_studio/src/ui/alcedo_main/qml` is empty.

### 10.11 Expected diff

1200–1700 lines. The Geometry panel frame adds about 300 lines to the stage-reader removal.

### 10.12 Completion record

```text
Phase / date / status:
Source revision and branch:
Actual changed modules:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual verification:
Evidence path:
Remaining defects or unavailable platforms:
```

---

## 11. Phase G10.2 — Document-only live edit, commit, Undo, and Redo

### 11.1 Objective and deliverables

Live edits, commits, Undo, Redo, and direct head moves write only the document. The stage table
receives no write from these paths. The legacy adjustment snapshot is removed.

### 11.2 Inputs and prerequisites

- G10.1 complete. The stage table has no product reader.

### 11.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `ui/alcedo_main/album_backend/editor_history_mutation.cpp` | Remove `MirrorTargetToExecutor`, the mirror-failure rollback at `:437-442`, `ApplyHistoryCommitToLivePipeline` calls, and `RefreshCommittedSnapshotFromLive`. |
| `ui/alcedo_main/album_backend/editor_history_shared_helpers.cpp` | Delete `MakeAdjustmentSnapshotFromLivePipeline`, `MakePipelineParamsFromSnapshot`, `MakeAdjustmentSnapshotFromPipelineParams`. |
| `include/ui/alcedo_main/album_backend/editor_history_state_detail.hpp` | Delete `committed_snapshot` and `root_snapshot`. |
| `app/editor_session_service.{hpp,cpp}` | Delete `adjustment_snapshot()` after a source search confirms that no production code, QML file, or Q_PROPERTY reads it. Stop and update this plan if a consumer exists. |
| `app/editor_adjustment_pipeline.{hpp,cpp}` | Delete `RemirrorEditorParameterToExecutor`, `ApplyHistoryCommitToLivePipeline`, `ApplyEditorAdjustmentSnapshot`, `ReadEditorAdjustmentOperatorState`, `SnapshotTouchesImageLoading`, and `EditorAdjustmentExecutorParamsFromWrite`. Keep `EditorAdjustmentDocumentParamsFromWrite`. |
| `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp` | Remove the `ApplyEditorAdjustmentSnapshot` and `EnsureLoadingOperatorDefaults` branch. |
| `app/editor_session_render_controller.cpp` and `include/app/editor_render_intent.hpp` | Remove the adjustment snapshot from the render intent if no other consumer remains. Keep `live_parameters_applied` only if another reader exists; otherwise delete it. |

### 11.4 Data rules and invariants

- One settled edit creates one commit. Commit and chain hashes for a fixed edit sequence must
  equal the values at `92085ffe`.
- A write failure in `ApplyEditorParameterWrite` leaves the document unchanged. This is the
  existing owner behavior.
- The deferred initial render path (`editor_session_render_controller.cpp:32-35, 55-58,
  106-110`) today applies the snapshot when `live_parameters_applied` is false. After this phase,
  every render reads the bound document. The initial render must still present the current
  document.

### 11.5 Implementation steps

1. Record the commit and chain hashes of the fixed sequence in Section 11.8 on the unchanged
   branch. Store them in the test as expected values.
2. Remove the mirror from the preview write. Keep the before-value capture from the document.
3. Remove the stage write and legacy snapshot refresh from commit.
4. Remove the stage write from `ApplyCommitToLiveDocument`.
5. Remove the snapshot fields and helpers.
6. Remove the render-lock snapshot branch.
7. Update `native_parameter_access.md`: remove the remirror step and the "CPU executor remirror"
   serialization boundary.
8. Rewrite tests that call removed APIs to assert document values and history rows.

### 11.6 Primary success call chain

See Section 6.2.

### 11.7 Primary failure and restore call chain

```text
ApplyEditorParameterWrite rejects the value
  -> CaptureAdjustmentBeforePreview returns false with the Model error
  -> no render request is issued for that value
  -> document keeps the previous value
  -> no commit is prepared

PublishPreparedEdit fails (WAL append error)
  -> existing NM4 path restores the before value through the document
  -> head and chain stay at the previous commit
  -> caller receives the WAL error
```

### 11.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `ExposureEditSequenceProducesUnchangedCommitAndChainHashes` | `EditorSessionHistoryPortTest` | Exposure 0.5, Contrast 10, Undo, Redo produce the recorded commit and chain hashes. |
| `LivePreviewWriteChangesOnlyDocument` | `EditorSessionHistoryPortTest` | After a preview write, the document value changes and the executor exposes no stage API use (the test compiles without stage headers). |
| `UndoRedoRestoresDocumentValuesAndHead` | `EditorSessionHistoryPortTest` | Undo returns the before value; Redo returns the after value; head matches. |
| `RejectedWriteKeepsDocumentAndCreatesNoCommit` | `EditorSessionEditControllerTest` | An out-of-range curve write fails; commit count is unchanged. |
| `WalAppendFailureRestoresBeforeValue` | `EditorMiniGitJournalRecoveryTest` | Injected WAL failure keeps the before value and the previous head. |
| `DeferredInitialRenderPresentsBoundDocument` | `EditorSessionRenderControllerTest` | The first render after open uses the bound document values. |
| `RenderPortConfiguresOnlyFrameSinkUnderRenderLock` | `EditorSessionRenderSchedulerPortTest` | The configure step attaches the sink and calls no adjustment function. |

### 11.9 Build and run commands

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest EditorSessionEditControllerTest EditorMiniGitJournalRecoveryTest EditorSessionRenderControllerTest EditorSessionRenderSchedulerPortTest EditorAdjustmentPipelineTest EditorRenderCoordinatorTest PipelineEditBatchTest CommitGraphTest alcedo_main
ctest --test-dir build/debug --output-on-failure -R "EditorSession|EditorMiniGit|EditorAdjustmentPipelineTest|EditorRenderCoordinatorTest|PipelineEditBatchTest|CommitGraphTest|NoProductCodeReadsStageTableOutsideMirror"
```

### 11.10 Exit criteria

- [ ] All listed tests pass and are discovered.
- [ ] Recorded hashes are unchanged.
- [ ] The allowed list of `NoProductCodeReadsStageTableOutsideMirror` no longer contains
      `editor_history_shared_helpers.cpp`.
- [ ] Manual check on Windows CUDA: drag Exposure, release, Undo, Redo; the viewer and the
      history panel match. Record it as manual evidence.

### 11.11 Expected diff

1200–1800 lines.

### 11.12 Completion record

Use the template in Section 10.12.

---

## 12. Phase G10.3 — Build-then-swap for open, checkout, rebuild, Version refs, and Paste

### 12.1 Objective and deliverables

Every document replacement uses build-then-swap. No path exports or imports stage JSON. No path
clones the prior document for rollback.

### 12.2 Inputs and prerequisites

- G10.2 complete.

### 12.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `app/pipeline_service.cpp` | `ReplayLiveDocumentFromRoot`, `CheckoutVersion`, `RebuildActiveEditorPipeline`: Section 6.3 pattern. Delete `prior_params`, `prior_document`, `restore_prior`, `ImportSerializedPipelineState`. |
| `app/editor_adjustment_pipeline.cpp` | Delete `ApplyVersionHeadToLivePipeline`, `RemirrorCurrentPanelFromDocument`, `ResetEditableOperatorsToDefaultsPreservingImageLocal`, `IsImageLocalParamKey`, `MergePreservingImageLocal`. |
| `ui/alcedo_main/album_backend/editor_history_state_detail.cpp` | Replace stage JSON capture and restore at `:131, 142, 327, 342, 343, 349, 355` with build-then-swap. |
| `ui/alcedo_main/album_backend/editor_history_version_refs.cpp` | Same at `:53, 73, 74`. |
| `ui/alcedo_main/album_backend/editor_history_mutation.cpp` | Same at `:1065, 1086, 1087`. |
| `ui/alcedo_main/album_backend/editor_history_transfer.cpp` | Remove the mirror at `:198`; delete `CancelLivePaste` (`:288-318`) and its declaration. |
| `ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp` | Keep `RebuildActiveEditorPipeline`; it no longer mirrors. |

### 12.4 Data rules and invariants

- History head is the only authority (locked identity model).
- The checkpoint is used only when root, head, and chain labels match.
- Swap happens only after replay and camera-profile binding succeed.
- `BindLivePipelineDocument` does not throw.
- A failed operation leaves the prior Version active and the prior document bound.

### 12.5 Implementation steps

1. Add a failure-injection seam that already exists in the replay tests, or use a commit with an
   invalid target to make `ReplayPipelineDocumentFromRoot` fail. Do not add a production-only
   test hook.
2. Change `ReplayLiveDocumentFromRoot` to bind the camera profile on the new document before the
   swap.
3. Change `CheckoutVersion` to the Section 6.3 sequence. Set the active Version and swap under
   one render lock scope.
4. Change `RebuildActiveEditorPipeline`, Version ref restore, and history state replay the same
   way.
5. Remove the Paste mirror and `CancelLivePaste`.
6. Delete the stage replay helpers.
7. Remove `SetExecutionStages()` calls from the changed call sites. The executor keeps the method
   until G10.7.

### 12.6 Primary success call chain

See Section 6.3.

### 12.7 Primary failure and restore call chain

```text
ReplayPipelineDocumentFromRoot fails at commit N
  -> CheckoutVersion returns false with the replay error
  -> graph.GetActiveVersionId() == prior Version
  -> guard.document_ == prior document pointer
  -> renderer keeps the prior document
```

### 12.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `CheckoutReplayFailureKeepsPriorVersionAndDocumentPointer` | `PipelineMapperTest` (pipeline service cases) | Active Version id and document pointer equal the prior values; the error text contains the replay error. |
| `CheckoutSuccessBindsReplayedDocumentAndMarksWriteBack` | same | Document values equal the target Version; write-back flag is set. |
| `ReopenWithMatchingCheckpointSkipsReplay` | `EditorCheckpointNavigationTest` | Replay count is zero. |
| `ReopenWithStaleCheckpointReplaysFromRoot` | same | Document equals the replayed Version head. |
| `PasteAsNewVersionBindsTargetDocumentWithoutMirror` | `AdjustmentTransferServiceMiniGitTest` | Target Version document equals the expected document; no stage API exists in the test link line. |
| `VersionRefRestoreFailureKeepsPriorDocument` | `EditorSessionHistoryPortTest` | Same assertions as checkout for Version ref restore. |
| `CheckoutAndPasteHashesAreUnchanged` | `AdjustmentTransferServiceMiniGitTest` | Commit and chain hashes of a fixed Paste sequence equal the values recorded at `92085ffe`. |

### 12.9 Build and run commands

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineMapperTest EditorCheckpointNavigationTest EditorCheckpointQmlIntegrationTest AdjustmentTransferServiceMiniGitTest AdjustmentTransferServiceTest EditorSessionHistoryPortTest PipelineHistoryApplierTest PipelineDocumentCheckpointTest ThumbnailServiceTest alcedo_main
ctest --test-dir build/debug --output-on-failure -R "PipelineMapperTest|EditorCheckpoint|AdjustmentTransfer|EditorSessionHistoryPortTest|PipelineHistoryApplierTest|PipelineDocumentCheckpointTest|NoProductCodeReadsStageTableOutsideMirror"
```

`ThumbnailServiceTest` has no `gtest_discover_tests` registration today. Run the executable
directly and record its output. Do not add discovery in this phase without running all of its
cases in the `ctest` environment first.

### 12.10 Exit criteria

- [ ] All listed tests pass.
- [ ] `ExportPipelineParams` and `ImportPipelineParams` have no caller in `alcedo_studio/src`
      outside `edit/pipeline/`.
- [ ] Manual check on Windows CUDA: create Version B, switch A → B → A, Paste to another image;
      viewer, history rows, and Version rail match. Record it as manual evidence.

### 12.11 Expected diff

1200–1800 lines.

### 12.12 Completion record

Use the template in Section 10.12.

---

## 13. Phase G10.4 — Remove the legacy history store and cut the project format to 0.9.0

### 13.1 Objective and deliverables

- No code creates, stores, or reads an `EditHistory` object or table row.
- No code opens `editor-journal/image-N.wal`.
- New projects save metadata version `0.9.0`. `0.8.0` projects fail at open.
- Legacy history files move to the deprecated archive.

### 13.2 Inputs and prerequisites

- G10.3 complete.

### 13.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `include/edit/history/pipeline_history_format.hpp` | Section 4.3 values. Update the header table comment. |
| `include/storage/store/database.hpp` | Remove `CREATE TABLE EditHistory`. Update the stale `0.6.0` comment near line 188 to name `0.9.0`. |
| `include/app/project_package_backend.hpp` | Update the stale version comment at lines 32–33. |
| `storage/store/sleeve/element_store.{hpp,cpp}` | Remove history mapper use at `:173-175, 238-239, 352-353, 592+` and `MaterializeEditorState`. |
| `sleeve/sleeve_element/sleeve_file.{hpp,cpp}`, `sleeve/sleeve_filesystem.cpp` | Remove `edit_history_`, `current_version_`, `GetEditHistory`, `SetEditHistory`. |
| `ui/alcedo_main/album_backend/application_module_host.cpp` | Remove `journal_path` and `EditorSessionJournalWriterPort` construction. Keep `mini_git_journal_path`. |
| `app/editor_save_checkpoint_service.cpp`, `editor_session_edit_controller.cpp`, `editor_session_navigation_controller.cpp` | Remove `CommitJournalAsync` and `DiscardUnflushed` calls on the legacy journal. |
| `app/CMakeLists.txt`, `edit/CMakeLists.txt`, `storage` CMake, test CMake | Remove archived sources and targets. |
| `.github/workflows/cpp-ci.yml` | Remove the `EditorJournalFuzzFrameworkTest` step. |
| Archive (`git mv`) | `edit/history/edit_history.*`, `version.*`, `working_version.*` (if present), `edit_transaction.*`, `editor_transaction_journal.*`, `editor_journal_writer.*`, journal recovery sources, `app/history_mgmt_service.*`, `app/editor_history_materializer.*`, `storage/mapper/sleeve/edit_history/*`, `editor_session_journal_writer_port.*`, and their tests listed in Section 5.8. |

Confirm the exact file names with a search before the move. Stop and update this plan if a
file hosts both legacy and Mini-Git code.

### 13.4 Data rules and invariants

- Mini-Git tables (`EditCommit`, `VersionRef`, `ImageEditState`, `PipelineRoot`) and
  `PipelineParam` keep their DDL and content.
- Every format value in Section 4.3 other than the project metadata stays unchanged.
- The open path checks the metadata version before it opens any history table. This is the
  existing order (`project_service.cpp:529-543`, `project_package_backend.cpp:254, 968-975`).
- Stale `editor-journal/image-*.wal` files inside a `0.9.0` project directory are ignored. No code
  deletes user files.

### 13.5 Implementation steps

1. Change the three version constants.
2. Remove the table DDL and all mapper calls.
3. Remove the sleeve history members and update copy and import.
4. Remove the legacy journal port and its calls.
5. Archive the legacy history files with `git mv` in one commit without content change. Remove
   them from CMake in the next commit.
6. Update tests that construct `0.8.0` fixtures to use `0.9.0`. Keep one `0.8.0` fixture for the
   rejection test.
7. Update the CI workflow.

### 13.6 Primary success call chain

```text
Create project
  -> ProjectService::CreateProject
  -> database schema without EditHistory
  -> metadata project_file_version = "0.9.0"

Import image
  -> SleeveFile::SetImage (no history object)
  -> ImportService -> PipelineMgmtService -> InitializeImageRoot (Mini-Git root)
```

### 13.7 Primary failure and restore call chain

```text
Open project with metadata 0.8.0
  -> IsSupportedProjectVersion("0.8.0") == false
  -> ProjectService returns the unsupported-version error
  -> no DuckDB history table is read; no file is changed
```

### 13.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `ProjectVersion080FailsBeforeHistoryLoad` | `CommitGraphTest` (existing old-metadata cases updated) | Open fails with the version message; history reader call count is zero. |
| `PackedProjectVersion080FailsBeforeDatabaseOpen` | `ProjectServiceTest` | Packed `.alcd` with `0.8.0` metadata fails; no temporary database is created. |
| `NewProjectWritesVersion090AndHasNoEditHistoryTable` | `ProjectServiceTest` | Metadata is `0.9.0`; `duckdb_tables()` has no `EditHistory`. |
| `ImportAndCopyDoNotCreateLegacyHistory` | `SleeveServiceTest` | Import and copy succeed; no legacy table access occurs. |
| `SaveDoesNotOpenLegacyImageJournal` | `EditorSessionTaskPortTest` or save service test | Save completes; no `image-N.wal` exists under the project journal directory. |
| `TypedCommitAndChainIdentityUnchangedAfterFormatCut` | `PipelineEditBatchTest` | Existing stored hash values still match. |
| `CurrentProjectReopensWithSameHeadAndChain` | `EditorCheckpointNavigationTest` | Save, close, reopen a `0.9.0` project; head and chain are equal. |

### 13.9 Build and run commands

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug --output-on-failure -R "CommitGraphTest|ProjectServiceTest|SleeveServiceTest|SleeveFSTest|EditorSessionTaskPortTest|PipelineEditBatchTest|EditorCheckpoint|EditorMiniGit|ImportServiceTest"
```

Reconfigure because target lists change. Build all targets because removed targets can break
unrelated link lines.

### 13.10 Exit criteria

- [ ] All listed tests pass.
- [ ] A full `win_debug` build succeeds.
- [ ] `git grep -nw EditHistory -- alcedo_studio/src alcedo_studio/tests` returns no match.
      Mini-Git types use other names.
- [ ] The macOS CI workflow still parses (`act` is not required; review the YAML diff).

### 13.11 Expected diff

900–1500 lines.

### 13.12 Completion record

Use the template in Section 10.12.

---

## 14. Phase G10.5 — DRT output resolution without `ODT_Op` or `OperatorParams`

### 14.1 Objective and deliverables

The three DRT passes resolve output transforms from `DrtNodeModel` parameters through
`DrtOutputResolver`. No runtime file includes `op_base.hpp`, `odt_op.hpp`, `param.cuh`,
`fused_param.hpp`, or `opencl_param.hpp`.

### 14.2 Inputs and prerequisites

- G10.1 complete. This phase can run in parallel with G10.2–G10.4.

### 14.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `edit/runtime/drt/*` (proposed) | `DrtOutputResolver`; move `odt_op.cpp` lines 16–138 (OpenDRT JSON helpers) and 150–279 (validation and runtime rebuild) without `OperatorParams`. Move `open_drt_cpu.*` and `aces_odt_cpu.*`. |
| `include/edit/runtime/cuda/cuda_drt_gpu_params.cuh` (proposed) | Copy `GPU_TO_OUTPUT_Params` and its nested ODT/OpenDRT structs byte-for-byte. Add `static_assert` on size and field offsets equal to the old struct. |
| `edit/runtime/cuda/drt/*` (proposed) | Move `GPU_kernels/color_mgmt/*.cuh`. Replace `param.cuh` includes with the new header. |
| `edit/runtime/cuda/cuda_drt_pass.cu`, `cuda_drt_runtime_state.cuh` | Hold `ColorUtils::TO_OUTPUT_Params` and `CudaDrtGpuParams` instead of `OperatorParams` and `GPUOperatorParams`. |
| `include/edit/runtime/opencl/opencl_drt_gpu_params.hpp` (proposed) | Move lines 27–170 of `opencl_param.hpp`. Keep `static_assert` layout checks. |
| `edit/runtime/opencl/opencl_drt_params.cpp`, `opencl_drt_pass.cpp`, `include/edit/runtime/opencl/opencl_drt_params.hpp` | Use the new header and resolver. |
| `edit/runtime/opencl/shader/` | Move `fused_params.cl` (renamed `drt_params.cl`), `common.cl`, and `cst.cl` with `git mv`. Update `opencl/CMakeLists.txt` defines and `opencl_gpu_dag_programs.cpp`. Rename the defines to drop `EDIT_PIPELINE`. Update both install blocks in the root `CMakeLists.txt`. |
| `edit/runtime/metal/metal_drt_params.cpp` | Use the resolver. `MetalDrtGpuParams` stays. |
| CMake | Add the resolver sources to the runtime library that all three backends link. Remove `Operators` linkage from runtime targets if no other use remains; otherwise record the remaining use. |

### 14.4 Data rules and invariants

- For each supported DRT configuration, the packed GPU parameter bytes must equal the bytes
  that the old path produces. This is the primary proof.
- The resolver must reject an invalid method or EOTF with an error. It must not return a default
  transform.
- Resolution runs at parameter-dirty time, as today. It does not run per frame.

### 14.5 Implementation steps

1. On the unchanged branch, write a test helper that packs DRT parameters through the old path
   for the configuration matrix below. Save the bytes as expected data files under
   `alcedo_studio/tests/resources/expected_parameters/drt/` (proposed), one file per backend and
   configuration, for example `cuda_aces20_p3d65_pq_1000nit_expected_params.bin`.
2. Add the resolver and move the math.
3. Switch CUDA, then OpenCL, then Metal. Run the byte test after each switch.
4. Move the OpenCL shader sources and update registry and install rules.
5. Remove the old includes from runtime files.

Configuration matrix: {ACES 2.0, OpenDRT} × {Rec.709 Gamma 2.2, P3-D65 Gamma 2.2,
Rec.2020 PQ 1000 nit, Rec.2020 HLG} × {default look, one non-default OpenDRT look}.
OpenDRT look applies only to OpenDRT rows.

### 14.6 Primary success call chain

See Section 6.5.

### 14.7 Primary failure and restore call chain

```text
DrtOutputResolver::Resolve rejects an invalid EOTF
  -> DRT pass throws with the resolver message
  -> PlanExecutor::CancelRender
  -> workspace publishes no DRT content key
  -> Renderer propagates the error to PipelineScheduler
```

### 14.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `CudaDrtParameterBytesMatchStoredExpectedBytes` | `GpuDagCudaDrtProductTest` | For each matrix row, packed bytes equal the stored file. |
| `OpenClDrtParameterBytesMatchStoredExpectedBytes` | `GpuDagOpenClDrtProductTest` | Same. |
| `MetalDrtParameterBytesMatchStoredExpectedBytes` | `GpuDagMetalDrtTest` (macOS) | Same. |
| `DrtOutputResolverRejectsUnknownEncodingEotf` | `GpuDagModelGraphTest` or a new `DrtOutputResolverTest` | Error returned; no transform. |
| `CudaDrtOutputMatchesStoredExpectedPixels` | `GpuDagCudaDrtProductTest` | A fixed AP1 input renders within 1/4096 per channel of stored pixels. |
| `RuntimeSourcesDoNotIncludeLegacyOperatorHeaders` | source check | No file under `edit/runtime` or `include/edit/runtime` includes `op_base.hpp`, `odt_op.hpp`, `param.cuh`, `fused_param.hpp`, or `opencl_param.hpp`. |
| `OpenClDrtProgramBuildsFromRuntimeShaderDirectory` | `GpuDagOpenClDrtProductTest` | The DRT program builds; its source paths are under `edit/runtime/opencl/shader`. |

### 14.9 Build and run commands

Windows:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaDrtProductTest GpuDagOpenClDrtProductTest GpuDagModelGraphTest alcedo_main
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaDrtProductTest|GpuDagOpenClDrtProductTest|GpuDagModelGraphTest|RuntimeSourcesDoNotIncludeLegacyOperatorHeaders"
```

macOS:

```text
cmake --preset macos_debug_tests
cmake --build --preset macos_debug_tests --target GpuDagMetalDrtTest GpuDagMetalRendererTest --parallel 8
ctest --test-dir build/macos-debug-tests -R "GpuDagMetalDrtTest|GpuDagMetalRendererTest" --output-on-failure
```

### 14.10 Exit criteria

- [ ] Byte tests pass on CUDA and OpenCL on Windows and on Metal on macOS.
- [ ] If no macOS runner is available, the phase stays `in progress`. The record says
      `Metal: unavailable`, not `pass`.
- [ ] The source check passes.

### 14.11 Expected diff

1300–1900 lines. Expected byte files do not count.

### 14.12 Completion record

Use the template in Section 10.12.

---

## 15. Phase G10.6 — Lens resolver, CUDA helpers, shared headers, shaders, and scope target

### 15.1 Objective and deliverables

No DAG file includes or compiles a file under `edit/pipeline/`, `edit/operators/GPU_kernels/`,
`edit/operators/CPU_kernels/`, or a legacy operator header. Scope has its own target.

### 15.2 Inputs and prerequisites

- G10.5 complete.

### 15.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `edit/runtime/lens/lens_calibration_resolver.*` (proposed) | Move Lensfun database, scaling helpers, and `ResolveRuntimeForMeta` from `lens_calib_op.cpp` without `OperatorParams* owner`. Move `lens_calib_runtime.hpp` to `include/edit/runtime/lens/`. |
| `edit/runtime/{cuda,metal,opencl}/*develop_pass*` | Call the resolver. |
| Lens kernel files | Move `cuda_lens_calib_ops.cu`, `cuda_geometry_ops.cu`, `metal_lens_calib.cpp`, `lens_calib.metal`, `opencl_lens_calib_ops.cpp`, `lens_calib.cl` to `edit/runtime/lens/{cuda,metal,opencl}/` in target `EditRuntimeLens` (proposed). Update Metal library and OpenCL geometry manifest paths and install rules. |
| `edit/runtime/cuda/` | Move the `detail_*` and `FilmGrain*` device helpers from `GPU_kernels/detail.cuh` and `film_grain.cuh` into `cuda_detail_math.cuh` and `cuda_film_grain_math.cuh`. They must not include `param.cuh`. |
| `edit/runtime/metal/shader/` | Move `prng.metal`; update `metal/CMakeLists.txt:351`. |
| `include/edit/runtime/local_tone_mapping.hpp` | `git mv` from `include/edit/pipeline/`; update includes. |
| `include/edit/runtime/pipeline_apply_request.hpp` | `git mv` from `include/edit/pipeline/`; update includes in `renderer.hpp`, `renderer.inl.hpp`, `pipeline_task.hpp`. |
| `edit/scope/*` | New target `EditScope`. Remove the sources from `EditPipeline` (`decoders/CMakeLists.txt:200-220`). Frame presenters link `EditScope`. |
| Local-tone constant test (`tests/edit/CMakeLists.txt:100`) | Move the constants it checks from `highlight_shadow_local_tone.hpp` into `local_tone_mapping.hpp` if the DAG uses them, and rename the test `LocalToneMappingConstantsMatchRuntime`. Otherwise archive the test in G10.9. |

### 15.4 Data rules and invariants

- Lens parameters from the resolver must equal the old `LensCalibOp` result field by field for
  the lens fixture set.
- Moved device helpers must keep the same arithmetic. CUDA detail and film grain pixels must be
  bitwise equal for a fixed seed.

### 15.5 Implementation steps

1. On the unchanged branch, save the `LensCalibGpuParams` output for the fixture set (one DNG
   with embedded warp, one Lensfun-matched RAW, one unmatched RAW, one user catalog selection)
   as expected JSON under `alcedo_studio/tests/resources/expected_parameters/lens/`.
2. Add the resolver and switch the three develop passes.
3. Move the lens kernels and the target.
4. Move the CUDA helpers, the Metal PRNG, and the two headers.
5. Create `EditScope`.
6. Add the source check `DagSourcesDoNotReferenceLegacyDirectories`.

### 15.6 Primary success call chain

```text
Develop pass (any backend)
  -> LensCalibrationResolver::Resolve(develop payload, prepared lens identity)
  -> LensCalibGpuParams (or no profile)
  -> EditRuntimeLens backend kernel
```

### 15.7 Primary failure and restore call chain

```text
Lensfun database path cannot load
  -> Resolver returns the load error
  -> Develop pass throws with that error
  -> PlanExecutor::CancelRender; no develop content key is published
```

This is the current behavior of `LensCalibOp::ResolveRuntimeForImage`. Confirm it before the
move. If the current code silently disables lens correction on a load failure, keep that exact
behavior and record it; do not add a new failure mode in this phase.

### 15.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `LensResolverMatchesStoredLensParametersForFixtureSet` | `LensCalibDevelopResolveTest` (rewritten) | Each field equals the stored JSON. |
| `CudaNeighborGradeDetailAndGrainPixelsAreUnchanged` | `GpuDagCudaPrimaryGradeTest` | Fixed seed output equals stored pixels bitwise. |
| `DagSourcesDoNotReferenceLegacyDirectories` | source check | No file under `edit/runtime`, `include/edit/runtime`, `edit/graph`, or `renderer` includes a path under `edit/pipeline/` other than the executor header, or any path under `edit/operators/GPU_kernels`, `edit/operators/CPU_kernels`, or a legacy operator header. |
| `FramePresenterLinksScopeWithoutEditPipeline` | build check | `GpuDagCudaDrtProductTest` links with `EditScope` and without the legacy sources. |
| Existing develop suites | `GpuDagCudaDevelopTest`, `GpuDagOpenClDevelopTest`, `GpuDagMetalDevelopTest` | Pass unchanged. |

### 15.9 Build and run commands

Same pattern as Section 14.9 with the targets above, on Windows and macOS.

### 15.10 Exit criteria

- [ ] All tests pass on Windows (CUDA and OpenCL) and macOS (Metal).
- [ ] The source check passes.

### 15.11 Expected diff

1000–1700 lines.

### 15.12 Completion record

Use the template in Section 10.12.

---

## 16. Phase G10.7 — Executor and services without a stage table

### 16.1 Objective and deliverables

`CPUPipelineExecutor` owns only the document binding, render lock, accelerator selection, frame
sink, bound file, cancel request, and renderer instances. It has no stage, merged stage,
`OperatorParams`, or `GPUPipelineWrapper`. Services and controllers do not create operator
defaults. History row presentation uses a `field_key` table.

### 16.2 Inputs and prerequisites

- G10.3 and G10.6 complete.

### 16.3 Modules, files, and APIs

| File | Change |
| --- | --- |
| `edit/pipeline/pipeline_cpu.{hpp,cpp}` | Delete members `stages_`, `exec_stages_`, `merged_stages_`, `global_params_`, `is_thumbnail_`, `backend_`, `mirror_legacy_stage_adapter_`, and request-shaping members used only by the no-argument `Apply`. Delete the no-argument `Apply` if no product caller remains; otherwise keep it and record the caller. Delete every method in the "Legacy-only" row of the audit (Section 5.1 source), and `InjectRawMetadata`. |
| `include/edit/pipeline/pipeline.hpp` | Delete the `PipelineExecutor` stage interface. Remove the base class from the executor. |
| `app/pipeline_service.cpp` | Delete `EnsureDefault*`, `ResyncGlobalParamsFromOperators`, `ImportSerializedPipelineState`, `ResetTransientPreviewState` stage parts; call `BindImportedCameraProfile` where `InjectRawMetadata` ran. |
| `app/import_service.cpp` | Delete the remaining stage code in `AssembleImportPipelineParams`. |
| `ui/alcedo_main/editor_support/controllers/pipeline_controller.cpp` | Delete `EnsureLoadingOperatorDefaults`, `RebuildBaselinePipelineForImage` stage parts, and stage attach. Keep only frame sink attach. |
| `storage/mapper/pipeline/pipeline_mapper.cpp` | Delete the stage import branch and dead `GetPipelineByElementId`/`Update(pipeline)` if they have no caller. |
| `app/editor_adjustment_pipeline.{hpp,cpp}` | Delete `FieldSpec`, `EditorAdjustmentFieldKey`, and the remaining stage helpers. |
| `ui/alcedo_main/album_backend/editor_history_commit_presentation.cpp` | Replace `OperatorType` lookups with a `field_key` → display-name and icon table. Keep every displayed string and icon. |
| `ui/alcedo_main/main.cpp`, `ui/alcedo_studio_test_host/main.cpp`, tests | Delete `RegisterAllOperators`. |
| `edit/graph/pipeline_document.cpp` | Delete `AllowsLegacyStageAdapterRemirror`. |
| `include/ui/alcedo_main/album_backend/editor_color_temp_model.hpp` | Update comments that name the `ColorTempOp` JSON shape. Keep the Q_INVOKABLE names; renaming them would change QML. |

### 16.4 Data rules and invariants

- History row titles and icons for every field key stay identical.
- The executor's public API used by services, scheduler, thumbnail, export, and editor stays
  source-compatible except for deleted legacy methods.

### 16.5 Implementation steps

1. Capture the history row title and icon for every field key on the unchanged branch into a
   test table.
2. Replace presentation lookups.
3. Delete service and controller stage code.
4. Delete executor members and methods. Build after each group.
5. Delete `pipeline.hpp`.
6. Shrink the `NoProductCodeReadsStageTableOutsideMirror` allowed list to only
   `edit/pipeline/pipeline_stage.*`, `edit/history` archive candidates (none after G10.4), and
   `edit/operators/**`. Rename the check to `NoProductCodeUsesStageTable`.

### 16.6 Primary success call chain

```text
PipelineMgmtService::LoadPipeline
  -> new CPUPipelineExecutor (no stages)
  -> LoadPipelineDocument -> BindLivePipelineDocument
  -> executor.SetPipelineDocument
  -> PipelineScheduler -> executor.Apply(input, request) -> Renderer<Backend>::Render
```

### 16.7 Primary failure and restore call chain

```text
Apply without a bound document
  -> throws "GPU DAG document is not bound" (existing text)
  -> PipelineScheduler reports the failure; no substitute render
```

### 16.8 Tests and evidence

| Test | Target | Assertion |
| --- | --- | --- |
| `HistoryRowTitlesAndIconsAreUnchangedForEveryFieldKey` | `EditorHistoryOperationPublisherTest` or the presentation test target | Every field key maps to the recorded title and icon. |
| `ExecutorConstructsWithoutOperatorRegistry` | `PipelineSharedUseTest` | Constructing and rendering with an executor succeeds in a process that never registers operators. |
| `ThumbnailAndExportRenderFromDocumentOnly` | `ThumbnailServiceTest` (direct run), `ExportServiceTest` | Output pixels match stored expected pixels within 1/1024. |
| `NoProductCodeUsesStageTable` | source check | Passes with the reduced allowed list. |
| Full `win_debug` suite | `ctest` | Pass counts equal the pre-phase record except removed and added tests. |

### 16.9 Build and run commands

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug --output-on-failure
```

Compare against the pre-phase record. Report known pre-existing failures by test name.

### 16.10 Exit criteria

- [ ] Full build and full `ctest` run recorded.
- [ ] No regression against the pre-phase record.
- [ ] Manual check on Windows CUDA and Windows OpenCL: open, edit, Version switch, export.

### 16.11 Expected diff

1400–1900 lines.

### 16.12 Completion record

Use the template in Section 10.12.

---

## 17. Phase G10.8 — Rename `CPUPipelineExecutor` to `PipelineExecutor`

### 17.1 Objective and deliverables

The executor name states its role. `pipeline_cpu.{hpp,cpp}` becomes
`pipeline_executor.{hpp,cpp}`. No logic changes.

### 17.2 Inputs and prerequisites

- G10.7 complete. `pipeline.hpp` no longer defines `PipelineExecutor`.

### 17.3 Modules, files, and APIs

All files that name `CPUPipelineExecutor` (about 22 in `src/` and 33 in `tests/` at
`92085ffe`). Member names such as `pipeline_executor_` already match.

### 17.4 Data rules and invariants

- No behavior change. The diff contains only the rename, include path changes, and
  clang-format results on changed lines.

### 17.5 Implementation steps

1. `git mv` the two files.
2. Replace the identifier and include path.
3. Run clang-format on changed files only.
4. Search QML and `.ts` translation files for the old name. Expect no match.

### 17.6 Primary success call chain

Unchanged from Section 16.6 with the new name.

### 17.7 Primary failure and restore call chain

Unchanged.

### 17.8 Tests and evidence

- Full `win_debug` build and `ctest`: same counts as G10.7.
- macOS `macos_debug_tests` build of Metal targets: same counts as the last macOS record.
- `git grep -n CPUPipelineExecutor` returns nothing outside the archive and roadmap history.

### 17.9 Build and run commands

Same as Section 16.9, plus the macOS build in Section 14.9.

### 17.10 Exit criteria

- [ ] Counts equal G10.7.
- [ ] The diff has no logic change (reviewer confirms).

### 17.11 Expected diff

500–900 lines.

### 17.12 Completion record

Use the template in Section 10.12.

---

## 18. Phase G10.9 — Archive shared, CUDA, CPU, and operator legacy files

### 18.1 Objective and deliverables

The deprecated archive exists with its README. Shared stage files, CUDA legacy pipeline files,
CPU kernels, legacy operator classes, `op_base.hpp`, `operator_factory.*`,
`LegacyPipelineImporter`, and their tests are in the archive and outside the compile graph.

### 18.2 Inputs and prerequisites

- G10.8 complete.
- `NoProductCodeUsesStageTable` passes with only archive candidates in its allowed list.

### 18.3 Modules, files, and APIs

- Archive: Section 5.6 "Shared", "CUDA", "CPU kernels", and "Operators" groups; Section 5.7
  classes; `edit/graph/legacy_pipeline_importer.*`; legacy tests in Section 5.8 rows "Stage
  execution" (stage-only cases), "Legacy operator and kernel".
- Keep in build: `edit/operators/models/**`, `adjustment_catalog.*`, `builtin_type_ids.hpp`,
  `planckian_locus_table.hpp`, `camera_matrices.hpp`, `utils/color_utils.hpp`,
  `geometry/resize_algorithm.hpp`, and any header that the source check proves is still used.
- `include/edit/operators/op_kernel.hpp` is used by `renderer/pipeline_task.hpp:14`. Remove that
  include if unused; otherwise move the used type to `include/renderer/`.
- CMake: remove `OPERATORS_SRCS` legacy entries, `EditPipeline` legacy sources, CUDA legacy
  sources, and test targets.

### 18.4 Data rules and invariants

- Every archived file keeps identical bytes (checked by `git diff -M100% --stat`).
- No archived path appears in any CMake file, compile database, or install manifest.

### 18.5 Implementation steps

1. Create the archive README.
2. `git mv` each group. Commit without content change.
3. Remove CMake references in the next commit. Build.
4. Add the static checks in 18.8.
5. Update the parent plan Section 47 checkboxes that this phase proves.

### 18.6 Primary success call chain

```text
cmake configure (win_debug)
  -> no target lists an archive path
  -> build succeeds
  -> static checks pass
```

### 18.7 Primary failure and restore call chain

```text
A kept file still includes an archived header
  -> build fails with the missing include
  -> move the required declaration to its DAG owner, or stop and update this plan
  -> do not restore the archived file into the build
```

### 18.8 Tests and evidence

| Test | Kind | Assertion |
| --- | --- | --- |
| `DeprecatedLegacyArchiveIsOutsideCompileGraph` | CMake script test | No `CMakeLists.txt` or `*.cmake` file under the repository (excluding `third_party`) contains `deprecated/legacy_pipeline`; `build/debug/compile_commands.json` contains no archive path. |
| `NoPipelineStageTypeRemainsInFirstPartySource` | source check | No match for `PipelineStage` or `PipelineStageName` under `alcedo_studio/src` or `alcedo_studio/tests`. |
| `NoOperatorParamsAggregateRemainsInFirstPartySource` | source check | No match for `OperatorParams` or `GPUOperatorParams`. |
| `AllBuiltInOperatorModelsHaveNoImageApplyEntryPoint` | source check | No `Apply(` or `ApplyGPU(` member in `edit/operators/models` or `include/edit/operators/models`. |
| `NoLegacyParameterImporterOrStageAdapterRemainsInProductPath` | source check | No `LegacyPipelineImporter`, `legacy_stage_adapter`, `MirrorsLegacyStageAdapter`. |
| `NoLegacyOperatorTypeEnumRemains` | source check | No `OperatorType::` and no `enum class OperatorType`. |
| Full suite | `ctest` | Counts recorded; removed tests listed by name. |

### 18.9 Build and run commands

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug --output-on-failure
```

Also run the macOS configure and build, because `edit/operators` changes affect the Metal build.

### 18.10 Exit criteria

- [ ] Full build and tests pass on Windows.
- [ ] macOS configure and build pass.
- [ ] All six checks pass.

### 18.11 Expected diff

700–1300 lines (CMake, tests, README, check script). `git mv` counts as zero.

### 18.12 Completion record

Use the template in Section 10.12.

---

## 19. Phase G10.10 — Archive OpenCL, Metal, and RawProcessor legacy files; fix packaging

### 19.1 Objective and deliverables

- OpenCL and Metal legacy pipeline files are archived (O6 and M7 removal lists).
- `RawProcessor`, its backend wrappers, CPU RAW operators, and `RawDecoder` are archived.
- The legacy `edit_pipeline` OpenCL manifest is gone; startup warm-up no longer builds it.
- The legacy Metal metallib is not built, bundled, or installed.
- The installed OpenCL package contains every DAG program source (defect D4).
- Parity tests use stored expected pixels instead of `RawProcessor`.

### 19.2 Inputs and prerequisites

- G10.9 complete.
- A Windows machine with an OpenCL device and a Mac with Metal for verification.

### 19.3 Modules, files, and APIs

| Area | Change |
| --- | --- |
| OpenCL | Archive Section 5.6 OpenCL group. Remove `RegisterOpenClEditPipelinePrograms` from `opencl/opencl_backend_program_registry.cpp:33`. Remove the nine legacy `.cl` paths from `opencl/CMakeLists.txt:10-21, 113-124` and line 57. Remove the `edit/pipeline/opencl_shader/` install blocks from the root `CMakeLists.txt:927-928, 1046-1047`. |
| Metal | Archive Section 5.6 Metal group. Remove `EditPipelineMetalShaders` (`metal/CMakeLists.txt:23-41, 219-227`), line 376 in `ALCEDO_METAL_RUNTIME_LIBS`, `decoders/CMakeLists.txt:144, 148, 238, 241`, and `ui/alcedo_main/CMakeLists.txt:927`. |
| RAW | Archive `RawProcessor` files, `decoders/processor/operators/cpu/*`, RAW-only backend wrappers (Section 5.6), and `RawDecoder` if `DecodeType::RAW` has no requester (verify `decoder_scheduler.cpp:117`). Keep `raw_color_context.hpp`, `raw_processor_pattern.hpp`, `raw_demosaic_method.hpp`, `nn/*`, `neural_tile_jobs.hpp`, `opencl_encode.cpp`, `metal_encode.cpp`, and RAW kernels that DAG develop passes call. |
| Packaging | Add `aces_reference_gamut_compression.h` to both OpenCL install blocks next to `dng_profile_gpu_math.h`. |
| Tests | Section 5.8 rows "Legacy RAW and OpenCL" and "RawProcessor as a parity reference". Generate stored expected pixels first (Section 6.6). Remove legacy program cases from `OpenClRuntimeTest`. |
| Unknown item | `MetalUtilsShaders` (`metal_convert.metal`): search for DAG use. Keep it if any DAG or scope file uses it. |

### 19.4 Data rules and invariants

- Stored expected pixels are generated at the pre-phase revision from the `RawProcessor` path
  that the parity tests use today. The test states the tolerance that the current test uses.
- No DAG program is `required_at_startup` unless it was before this phase.

### 19.5 Implementation steps

1. Generate stored expected pixels on the unchanged branch (CUDA and OpenCL on Windows, Metal on
   macOS). Record generation commands in the completion record.
2. Switch parity tests to the stored files.
3. Archive OpenCL files, update registry and CMake, build.
4. Archive Metal files, update CMake and bundle lists, build on macOS.
5. Archive RAW files, update CMake, build on both platforms.
6. Fix the install rule. Build the Windows package and the macOS bundle.
7. Add the checks in 19.8.

### 19.6 Primary success call chain

```text
Application start (OpenCL)
  -> OpenClBackendProgramRegistry manifests: raw_processor, geometry, scope, demosaicnet, gpu_dag
  -> WarmUpRequiredPrograms builds only required DAG-era programs
  -> first render builds gpu_dag programs from installed sources
```

### 19.7 Primary failure and restore call chain

```text
Installed package misses a DAG program source
  -> OpenClProgramLibrary reports the missing path with the program name
  -> InstalledOpenClPackageBuildsEveryGpuDagProgram fails in CI evidence
  -> fix the install rule; no fallback program is added
```

### 19.8 Tests and evidence

| Test | Kind | Assertion |
| --- | --- | --- |
| `NoLegacyOpenClPipelineFactoryRemains` | source check | No `CreateOpenCLGPUPipeline`, `OpenCLGPUPipeline`, `OpenClFusedParams`, `OpenClFusedParamUploader`, `OpenClStage`. |
| `NoLegacyOpenClFusedProgramIsRegisteredOrPackaged` | `OpenClRuntimeTest` + install manifest check | Registry has no `edit_pipeline` manifest; install manifest has no `edit/pipeline/opencl_shader`. |
| `NoLegacyMetalPipelineFactoryRemains` | source check | No `CreateMetalGPUPipeline`, `MetalGPUPipeline`, `MetalFusedParams`, `MetalStage`. |
| `NoLegacyMetalMetallibIsPackaged` | macOS bundle check | The bundle has no fused pipeline metallib. |
| `NoRawProcessorEntryRemainsInProductBuild` | source check | No `RawProcessor` class use in `alcedo_studio/src`. |
| `InstalledOpenClPackageBuildsEveryGpuDagProgram` | installed-package test | Run from `build/install` with the source tree path hidden (rename the build-tree shader directories or run on a copy); every `gpu_dag` program builds. |
| `MetalDevelopMatchesStoredExpectedPixels` | `GpuDagMetalDevelopTest` | Output within the stated tolerance of the stored file. |
| `OpenClDevelopMatchesStoredExpectedPixels` | `GpuDagOpenClDevelopTest` | Same. |

### 19.9 Build and run commands

Windows:

```text
cmd /c scripts\msvc_env.cmd --preset win_release -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4
cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1 -BuildDir build/release -Preset win_release
```

macOS:

```text
cmake --preset macos_release
cmake --build --preset macos_release
cmake --install build/macos-release
```

Run debug test suites on both platforms as in Sections 16.9 and 14.9.

### 19.10 Exit criteria

- [ ] All checks pass on both platforms.
- [ ] The Windows installer and the macOS bundle build.
- [ ] The installed OpenCL test passes.
- [ ] O6 Section 14.5 and M7 Section 15.4 removal checkboxes are checked in their plans with a
      link to this record.

### 19.11 Expected diff

900–1600 lines. Stored expected pixel files do not count.

### 19.12 Completion record

Use the template in Section 10.12.

---

## 20. Phase G10.11 — Release qualification

### 20.1 Objective and deliverables

- All G10 static checks and the parent plan G10 test list pass.
- Full test suites pass on Windows (CUDA and OpenCL) and macOS (Metal).
- Installed packages open, edit, save, reopen, and export a project.
- CUDA, OpenCL, and Metal A/B records meet the thresholds in Section 22.
- The parent plan, OpenCL plan, and Metal plan contain truthful completion records.

### 20.2 Inputs and prerequisites

- G10.10 complete.
- Access to the same Windows machine for CUDA and OpenCL baselines and the same Mac for Metal.

### 20.3 Modules, files, and APIs

- Tests: parent plan G10 names not yet implemented:
  `DefaultPipelineContainsExactlyDevelopGradeAndDrt`, `AllGpuBackendsUseBasicRenderWorkspace`,
  `NoCpuImageOperatorEntryPointRemainsInProductPipeline`,
  `DefaultPipelineRoundTripPreservesThreeNodeGraph`,
  `RasterMaskRoundTripPreservesR8DataAndSamplingBounds`,
  `CropRotateViewportAndDynamicResolutionMatchAcrossBackends`,
  `SteadyStateRenderAllocatesNoGpuBufferOrTextureAcrossBackends`,
  `OnlyDirtyParameterRangesTransferAcrossBackends`. Reuse existing tests that already prove a
  name; record the mapping instead of adding a duplicate.
- A/B harness: extend the existing performance tooling that NM8 and O5/M6 records used. Put
  results under `build/tmp/g10_11/ab/`.
- Docs: parent plan Sections 32, 44, 47; OpenCL plan status and Section 14; Metal plan status and
  Section 15; roadmap index.

### 20.4 Data rules and invariants

- A skipped test is not a pass. An unavailable platform is reported as unavailable.
- A/B uses the same device, driver, OS, compiler, Release configuration, RAW fixture, DecodeRes,
  viewport, and edit sequence for old and new.

### 20.5 Implementation steps

1. Run full debug suites on both platforms. Record counts by target.
2. Build release packages. Run the installed-package checklist (Section 20.8).
3. Build each pinned baseline (Section 22.1) on the same device. Run the scenario list.
4. Run the same scenarios on the G10 head.
5. Write the records into this plan and into the parent, OpenCL, and Metal plans.

### 20.6 Primary success call chain

```text
Installed app start -> open 0.9.0 project -> edit -> Version switch -> save -> reopen
  -> export JPEG and HDR -> outputs match stored expected pixels within tolerance
```

### 20.7 Primary failure and restore call chain

```text
Any A/B threshold is not met
  -> G10.11 stays in progress
  -> record the scenario, values, and suspected pass
  -> open a named follow-up phase in this plan; do not lower quality or change DecodeRes
```

### 20.8 Tests and evidence

Installed-package checklist (manual, both platforms):

1. Start the installed app on a machine or user account without the source tree.
2. Create a project. Import one DNG, one non-DNG RAW, one JPEG.
3. Edit Exposure, a curve, a Radial Mask, and DRT EOTF.
4. Create Version B. Switch A → B → A. Undo and Redo.
5. Save, close, reopen. Confirm head, Version, and pixels.
6. Export SDR JPEG and an HDR file.
7. Open a `0.8.0` project. Confirm the version message.

Automated evidence: full suites, static checks, A/B tables.

### 20.9 Build and run commands

Use Sections 16.9, 14.9, and 19.9.

### 20.10 Exit criteria

- [ ] Full suites recorded on both platforms with discovered, passed, failed, skipped counts.
- [ ] Installed-package checklist recorded as user-confirmed manual evidence on both platforms.
- [ ] CUDA, OpenCL, and Metal A/B tables meet Section 22.3.
- [ ] Parent Section 47 checkboxes updated only where evidence exists.

### 20.11 Expected diff

600–1100 lines.

### 20.12 Completion record

Use the template in Section 10.12, plus the performance fields in Section 22.4.

---

## 21. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Slider drag and release | Same pixels; one commit | G10.2 |
| Invalid parameter write | Rejected; no commit; document unchanged | G10.2 |
| Undo and Redo | Document and head restored | G10.2 |
| Version checkout success | Target document bound; write-back set | G10.3 |
| Version checkout failure | Prior Version and document stay | G10.3 |
| Reopen with matching checkpoint | No replay | G10.3 |
| Reopen with stale checkpoint | Replay from root | G10.3 |
| Paste to another image | New Version with expected document | G10.3 |
| `0.8.0` project | Rejected at open | G10.4 |
| New project | `0.9.0`, no `EditHistory` table | G10.4 |
| DRT configurations | Byte-identical GPU parameters on three backends | G10.5 |
| Lens fixture set | Identical lens parameters | G10.6 |
| CUDA detail and grain | Bitwise identical pixels with a fixed seed | G10.6 |
| History row titles | Identical text and icon per field key | G10.7 |
| Thumbnail and export | Stored expected pixels within tolerance | G10.7, G10.11 |
| Compile graph | No archive path | G10.9, G10.10 |
| Installed OpenCL | All DAG programs build without the source tree | G10.10 |
| Geometry panel open and close | Uncropped source while open; document crop after close | G10.1 |
| Performance | Section 22 thresholds on three backends | G10.11 |

QML behavior: G10 changes no QML. Verify through C++ owner and service tests. `WorkspaceShellTest`
results are not evidence for this plan (see `AGENTS.md`).

---

## 22. Performance acceptance

### 22.1 Pinned baselines

| Backend | Baseline commit | Reason |
| --- | --- | --- |
| CUDA | `bf6686fb` | Parent plan Section 41.9: last commit before the GPU DAG rebuild |
| OpenCL | `ffb291ea` | Parent of `7af8b741` (OpenCL DAG O5 product path): last commit where the OpenCL product path used the legacy pipeline |
| Metal | `9c1df791` | Parent of `fba20f6c` (Metal DAG M6 product switch on `main`): last commit where the Metal product path used the legacy pipeline |

Build each baseline in a separate worktree under `build/tmp/g10_11/baselines/<backend>/`. Do not
merge baseline code into the G10 branch.

If a baseline does not build with the current toolchain, record the exact error. Stop and ask the
user for a replacement baseline. Do not pick a different commit without approval.

### 22.2 Scenarios

Use the scenario tables of the parent plan Section 41.9, OpenCL plan Section 12.2, and Metal plan
Section 13.2. Each scenario records one cold render and at least 30 warm renders.

### 22.3 Thresholds

- Warm interactive median: at most `1.05x` the baseline.
- Warm interactive p95: at most `1.10x` the baseline.
- QualityBase, DetailPatch, and image-switch-return median and p95: at most `1.10x` the baseline.
- From the second frame of the no-change, Exposure, CCT, and DRT scenarios: zero GPU resource
  create and free.
- OpenCL: zero stable-frame program builds and kernel creates; zero render-internal `clFinish`
  except the documented Develop scratch synchronization and explicit download.
- Metal: zero stable-frame compute pipeline creates.
- Do not meet a threshold by lowering quality, reducing input size, skipping a required
  algorithm, or switching backend.

### 22.4 Record fields

Each record contains: device, vendor, driver, OS, compiler, build configuration, commit, RAW
fixture, DecodeRes, request, viewport, edit sequence, cold time, warm median, warm p95, pass
counts, H2D bytes, D2H bytes, resource create and free counts, program or pipeline creates, and
the command lines.

---

## 23. Build and evidence rules

- Windows presets: `win_debug`, `win_release` through `scripts\msvc_env.cmd`.
- macOS presets: `macos_debug`, `macos_debug_tests`, `macos_release`, and the CI preset
  `macos_arm_metal_ci`.
- Required backends: CUDA and OpenCL on Windows; Metal on macOS.
- Temporary evidence: `build/tmp/g10_<phase>/`.
- Record pass, fail, skip, and unavailable separately. Do not report an unavailable platform as
  zero. Do not report a skipped test as a pass.
- Confirm discovery with `ctest -N -R <name>` before claiming that a test ran.
- Manual checks supplement automated tests. Record them as user-confirmed manual evidence.

---

## 24. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| A hidden product reader of the stage table exists | Source check or a pixel test fails after a mirror removal | Stop. Convert the reader to the document in the current phase. Update the audit table. |
| Checkout or Undo changes a hash | Hash test fails | Stop. The batch or document path changed. Do not update the expected hash. |
| DRT bytes differ after the move | Byte test fails | Stop. Find the arithmetic or layout difference. Do not widen the test to a pixel tolerance. |
| Lens resolver changes behavior on load failure | Resolver test differs from the stored record | Keep the old behavior in this plan. Report it as a separate defect. |
| A kept file includes an archived header | Build fails | Move the declaration to its DAG owner, or stop and update this plan. |
| A baseline commit does not build | Build error in the baseline worktree | Record the error; ask the user for a replacement. |
| An A/B threshold fails | Section 22.3 comparison | G10.11 stays in progress; create a named follow-up phase. |
| The Metal runner is unavailable | No macOS build | Phases that need Metal evidence stay in progress; record "unavailable". |
| A phase diff grows past 2000 lines | Diff stat during implementation | Stop and split the phase in this plan before continuing. |

Stop implementation when:

- a required owner API does not exist;
- the source audit no longer matches the branch;
- a phase can pass the 2000-line limit;
- a new persistence change lacks the rejection rule of Section 4.3;
- an operation needs a fallback that the user did not authorize;
- a proposed consistency mechanism lacks a real production interleaving.

---

## 25. Decision record for questions raised at plan creation

The user answered all three questions on 2026-09-22. No decision is open.

| Question | Decision | Where applied |
| --- | --- | --- |
| Archive scope | Apply the archive rule to every whole legacy file, not only OpenCL and Metal | Sections 1.2, 4.4, G10.4, G10.9, G10.10 |
| Geometry panel frame (defect D2) | Show the uncropped, unrotated source while the panel is open | Sections 1.2, 3.2, 6.7, G10.1 |
| Existing `gate` identifiers | Do not rename them in one sweep; rename when a file is touched | Sections 1.2, 4.2 |
