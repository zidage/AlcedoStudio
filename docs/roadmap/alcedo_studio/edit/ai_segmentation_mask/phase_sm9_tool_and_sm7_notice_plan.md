# SM9 AI Mask Tool and SM7 Misalignment Notice - Phase Plan

Date: 2026-10-24. Status: planned. Product code is unchanged by this plan.

Parent: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), sections 9.6 and 9.8.
Prerequisite: [SM8 GPU Coverage and One Display Composite](phase_sm8_gpu_coverage_and_display_plan.md).
Source audit: `b3e1d479f60caa70b47205504348287b30114b47`, `main`.
Read the SM3, SM4, SM5, and SM6 phase plans linked by the parent before implementation.

## 1. Product specification

Implement the user-requested order: SM8, SM9, SM7, SM2, SM10.
SM7 does not block tool development. It remains required before release qualification.

Use one AI Mask dialog bound to the main photo viewport.
The dialog owns no second photo renderer and no second coverage composite.
Its app controller owns the existing `AiMaskEditSession` on a serial worker.
The QML model exposes values and focused actions through the application layer.
QML never calls a GPU backend, asset store, or sidecar directly.

Required control order:

```text
AI Mask                         Close
Prompt / Tasks
Prompt: Stroke / Click / Box    Add / Subtract
Tasks: Subject  Background  Sky  People
Level selection when the current click has three levels
Feather                        Expand
Show Mask
Undo                           Redo
Busy text or error text with the existing alert component
Cancel                         Commit
```

Use the existing tool/adjustment panel location and shared dialog components.
Keep the viewport visible and interactive for view navigation.
Do not create a second viewport inside the dialog.
At narrow widths, place controls in one scrollable column and keep Cancel/Commit reachable.
Use existing AppTheme spacing, fonts, control sizes, and Basic style.
Add any required visual role to AppTheme and `DESIGN.md` together.
Do not add pills, status dots, or independent palettes.

Preserve SM5 prompt-group rules, task ordering, person toggles, level rules, and the 64-point limit.
Reject an input that exceeds 64 points with the existing explanatory error.
Keep its previous valid group. Do not silently split it into another model operation.
Run a prompt only after click, stroke release, or box release.
Pointer movement draws input markers without inference.

While inference runs, keep the previous coverage and controls visible.
Disable new model input, Undo, Redo, and Commit until the request returns.
Keep Cancel and viewport pan/zoom available.
Show busy text that states the operation. Do not blank the photo.
Model errors and `NotFound` use the existing session result without adding a step.
GPU errors follow SM8 section 4.3, including explicit previous-result display and Retry.

Feather and Expand drag values preview through SM8 without appending a step for each move.
Pointer release or keyboard acceptance adds one session edge step through `SetEdgeAdjustment`.
Undo restores the previous accepted parameters and coverage without inference.
Escape during an edge edit restores its starting parameters and adds no step.
Mode changes, accepted prompts, task selections, level changes, and person toggles retain SM5 step semantics.

Route dialog Undo/Redo actions and their shortcuts to the temporary stack.
Do not send them to main history while the dialog owns the AI edit lease.
Give controls accessible names and visible keyboard focus.
Tab follows the control order. Space/Enter activates the focused control.
Escape first cancels an active pointer edit, then cancels the dialog when no edit is active.
Close has the same result as Cancel.

Commit stores original coverage and publishes one main history operation through SM5.
Keep the session open if asset write or history publication fails.
Cancel writes no asset or history and restores the previous committed preview selection.
An image switch ends the edit through the existing editor lifecycle.
Unload models when returning to ordinary adjustment, under D15 and D22.
Drain the serial session worker safely before destroying its model-output store.

Until SM2, a missing model shows the real error and expected local directory.
SM2 later replaces this with the approved download panel.
No different model, backend, resolution, or empty coverage is a substitute.

## 2. Source and owner audit

| Area | Existing owner and file | Work in this plan |
| --- | --- | --- |
| Temporary history | `src/include/app/ai_mask_edit_session.hpp`, `src/app/ai_mask_edit_session.cpp` | Call existing focused operations. Do not mirror its stack in QML. |
| Model results | `src/include/app/ai_mask_model_output_store.hpp` | Keep earlier native outputs for session Undo/Redo. |
| Open/Commit/Cancel | `src/app/editor_session_service.cpp` | Use the existing AI edit lease and asset-first history publication. |
| Prompt mapping | `src/ui/edit_viewer/ai_mask_prompt_geometry.cpp` | Reuse ReferenceSpace mapping from SM4. |
| Control drawing | `src/ui/editor_rhi/editor_overlay_item.cpp` | Add input marker presentation under its existing owner. No coverage image processing. |
| Dialog theme | `src/ui/alcedo_main/DESIGN.md`, `AppTheme` | Reuse shared controls and named roles. |
| Fingerprint | `MakeAiMaskGeometryFingerprint`, stored `AiSegmentationMaskSource::geometry_fingerprint` | SM7 compares through a scoped owner read. It does not recompute the Mask. |
| Thumbnail | `src/app/mask_thumbnail_evaluate.cpp`, existing `ThumbnailService` executor | Request AI effective coverage through SM8, sample to 128x128, and download only that thumbnail. No model call or full-coverage readback. |

Paths in this table start at `alcedo_studio/`.
SM8 APIs are prerequisites, not verified current source behavior.
There is no AI Mask tool dialog in this audited source revision.

## 3. Module and API map

Proposed app owner: `app/ai_mask_tool_controller.cpp` with its defining header in `include/app/`.
It owns the session worker and exposes Open, Prompt, Task, TogglePerson, SelectLevel, PreviewEdge,
AcceptEdge, Undo, Redo, RetryPreview, Commit, and Cancel operations.
Reuse the existing recipe and result types.
Do not add a second session-state object or copy full documents to supply QML properties.
Project current values through focused const reads on the owning thread.

Proposed UI adapter: `ui/alcedo_main/album_backend/editor_ai_mask_tool_model.cpp` with its header under `include/ui/`.
It exposes control values, enabled states, busy/error text, and app actions.
Proposed QML file: `ui/alcedo_main/qml/AiMaskDialog.qml`.
Register it in `ui/alcedo_main/CMakeLists.txt` and document its VI entry.

SM7 adds a read operation that compares the current Develop fingerprint with each committed AI Mask fingerprint.
The result names only mismatched Mask IDs and display names.
Do not persist another fingerprint, notice table, or copied document.
On failed owner reads, report the error instead of declaring the Mask aligned.

## 4. Implementation assignments

| Phase | Deliverable | Dependency | Expected diff | Status |
| --- | --- | --- | ---: | --- |
| SM9.1 | Tool controller, QML model, Open/Close, busy/error lifecycle | SM8 | 1100-1800 | planned |
| SM9.2 | Prompt/task/People controls and viewport input markers | SM9.1 | 1200-1900 | planned |
| SM9.3 | Edge preview, temporary history controls, Commit/Cancel, thumbnails | SM9.2 | 1100-1900 | planned |
| SM7 | Per-Mask geometry misalignment notice | SM9.3 | 400-800 | planned |

The three SM9 phases separate session lifecycle, model input, and edit completion.
Each estimate includes tests, QML registration, VI documentation, and plan evidence.
Split any assignment whose expected diff can exceed 2000 lines before implementing it.

### 4.1 SM9.1 - Tool lifecycle

**Objective and inputs.** Expose the SM8 request path in one dialog after SM8 completes.
**Modules and invariants.** Add the proposed app controller, UI model, and registered QML dialog.
The controller owns one serial session worker. No blocking inference runs on the GUI/render thread.

**Steps.**

1. Open through the existing editor lease and analysis-image operations.
2. Construct or restore the existing session and display stored coverage through SM8.
3. Expose loading, ready, previous-result/error, and closing states through focused model properties.
4. Implement Cancel/Close and image-navigation cleanup with the existing lifecycle identities.
5. Unload models on return to adjustment after pending model work drains.

**Success chain.** UI action -> app Open -> analysis/session -> SM8 preview -> ready dialog.
**Failure chain.** Missing model, analysis failure, or session error -> real error -> unchanged document -> Cancel remains available.

**Tests.** Proposed `AiMaskToolControllerTest` checks `OpenStoredMaskDisplaysWithoutInference`,
`OpenFailureLeavesDocumentUnchanged`, `CancelWhileInferenceRunsReleasesSessionAfterCompletion`,
and `ReturningToAdjustmentUnloadsModels`.
Test app ownership directly. Add manual keyboard and narrow-window verification.

**Commands.** Use SM8 section 9 groups A/B for the registered controller target and `alcedo_main`.
Run only named owner tests and their direct callers.
**Exit.** Open/Close and failure lifecycle pass; QML loads in the product; manual checks are recorded.
**Expected diff.** 1100-1800 lines. **Completion record.** Section 7; unfilled.

### 4.2 SM9.2 - Prompt and task input

**Objective and inputs.** Add complete model input after SM9.1.
**Modules and invariants.** Use the app controller, SM4 geometry, existing overlay control owner, and dialog controls.
Keep SM5 component order, add/subtract labels, level selection, and People semantics.

**Steps.**

1. Add Stroke, Click, Box, Add/Subtract, and Prompt/Tasks controls with existing shared components.
2. Map released input to normalized ReferenceSpace and show markers in the existing overlay.
3. Add Subject, Background, Sky, People, and person-box actions through the session owner.
4. Expose level selection only for the valid three-level click result.
5. Reject the 64-point overflow without changing the last valid group or rerunning on pointer movement.

**Success chain.** Released input -> shared geometry -> session operation -> retained model output -> SM8 preview.
**Failure chain.** Invalid input, `NotFound`, or inference error -> no session step -> previous display plus real message.

**Tests.** `PointerMovementDoesNotRunInference`, `ReleasedStrokeRunsOnePrompt`,
`PromptCoordinatesFollowCropAndRotation`, `PointLimitKeepsPreviousValidGroup`,
`PersonToggleUsesOrderedSessionOperation`, and `LevelChangeUsesRetainedOutputs`.
Use `AiMaskEditSessionTest`, `MaskEditGeometryTest`, and the proposed controller tests.

**Commands.** SM8 section 9 groups A/B; target the named owner and geometry behaviors.
**Exit.** All required input modes work in the production window; geometry and call counts pass.
**Expected diff.** 1200-1900 lines. **Completion record.** Section 7; unfilled.

### 4.3 SM9.3 - Edge editing and session completion

**Objective and inputs.** Complete the usable edit dialog after SM9.2.
**Modules and invariants.** Use the session edge API, SM8 display, existing history publication, and thumbnail owner.
Store one original asset only at Commit. Do not read adjusted GPU pixels back for storage.

**Steps.**

1. Add Feather and Expand controls with display-only intermediate updates and one accepted stack step.
2. Route local Undo/Redo and shortcuts to the session owner.
3. Bind Show Mask to the SM8 presentation state without a main document edit.
4. Connect Retry, Commit, Cancel, and previous-result error presentation to SM8 rules.
5. Use the existing thumbnail executor and asset reader for AI effective coverage through SM8.
   Sample the processed result to the existing 128x128 thumbnail grid. Download only this small result for the UI image.
6. Check focus, Escape precedence, translated labels, accessibility, and narrow-window layout.

**Success chain.** Edge preview -> same GPU operation/composite -> accepted local step -> original asset -> one main history operation.
**Failure chain.** Preview error blocks Commit; store/history error keeps the session open; Cancel leaves original document unchanged.

**Tests.** `FeatherDragCreatesOneSessionStep`, `EscapeRestoresStartingEdgeWithoutStep`,
`DialogUndoDoesNotReachMainHistory`, `CommitFailureKeepsDialogAndTemporaryStack`,
`CommitThenMainUndoRestoresOriginalMask`, `CancelWritesNoAssetOrHistory`, and
`AiMaskThumbnailLoadsWithoutModel`.
Use controller tests and the existing AI edit service tests. Check the real dialog manually.

**Commands.** SM8 section 9 groups A-C as relevant. Discover the existing service/thumbnail tests before execution.
**Exit.** A real photo completes Open, edit, Undo, Redo, Commit, main Undo, reopen, and Cancel.
**Expected diff.** 1100-1900 lines. **Completion record.** Section 7; unfilled.

### 4.4 SM7 - Misalignment notice after the tool

**Objective and inputs.** Show D9/P6 on the completed SM9 surface.
**Modules and invariants.** Read fingerprints through current app/document owners and show the notice in the tool/Mask list.
Do not change the stored Mask or run inference after a Develop change.

**Steps.**

1. Compute the current fingerprint through `MakeAiMaskGeometryFingerprint` when relevant Develop values change.
2. Compare it with each committed AI Mask through a scoped owner read.
3. Show the warning only for mismatched Masks using the existing warning component.
4. Explain that Lens/RAW changes can misalign this Mask and that the user can delete and recreate it.
5. Refresh the comparison after Undo/Redo and image/version selection without persisted notice state.

**Success chain.** Develop change -> current fingerprint -> per-Mask compare -> existing warning presentation.
**Failure chain.** Owner read error -> explicit error -> no false aligned result and no Mask mutation.

**Tests.** `GeometryChangeMarksOnlyMismatchedAiMasks`, `UndoRestoresFingerprintNoticeState`,
`ColorOnlyChangeDoesNotMarkGeometryMismatch`, and `NoticeNeverRunsInferenceOrChangesMask`.
Use proposed `AiMaskMisalignmentNoticeTest` and the tool-controller tests.

**Commands.** SM8 section 9 groups A/B for those registered targets and `alcedo_main`.
**Exit.** Per-Mask behavior and the warning display pass. SM7 completes before SM10 release qualification.
**Expected diff.** 400-800 lines. **Completion record.** Section 7; unfilled.

## 5. Evidence and build rules

Reread `AGENTS.md`, `alcedo-qml-ui`, `qt-qml`, and `alcedo-msvc-cmake` before implementation.
Read the complete current `DESIGN.md` and relevant VI entry before visual changes.
Use the existing presets and wrapper commands from SM8 section 9.
Register proposed tests in the relevant app/UI CMake file before building them.
Do not run a full test suite. Do not weaken tests to solve offscreen focus/input delivery.
Use C++ owners for deterministic behavior evidence and the real window for visual/input evidence.
Keep temporary files under `build/tmp/<phase>/`, then remove task-created temporary files.

## 6. Risks and stop conditions

If SM8 lacks a required focused operation, amend that assignment before implementing UI work.
Do not introduce a parallel renderer, stack, coverage cache, or sidecar client in QML.
If a phase can exceed 2000 lines, split it before implementation.
Report actual errors through the existing issue rules. Do not add a substitute model or display path.
Preserve the stored source and earlier outputs during every new model request.

## 7. Completion records

Create one record per assignment. No completion evidence is filled during planning.

```text
Phase / date / status:
Source revision and branch:
Actual changed modules and diff size:
Implemented behavior:
Primary success call chain:
Primary failure and restore call chain:
Commands and exit codes:
Discovered / passed / failed / skipped counts:
Manual product, focus, and accessibility checks:
Remaining issues or unavailable platforms:
```

### 7.1 SM9 MVP (issue #513), before SM9.1

```text
Phase / date / status: SM9 MVP / 2026-10-24 / implemented on Windows; not the SM9.1-SM9.3 product.
Source revision and branch: 8e1d8058 (SM8.7, PR #512); branch feature/ai-segmentation-sm9-mvp.
Actual changed modules and diff size: about 1300 lines.
  - New UI-layer owner EditorAiMaskTool (album_backend/editor_ai_mask_tool.*), exposed as
    appModules.aiMaskTool, destroyed and shut down by ApplicationModuleHost.
  - New EditorAiMaskPanel.qml over the adjustment panel pages; AI Mask button in
    EditorAdjustmentHeader; viewport routing and box rubber band in EditorWorkspace.
  - AiMaskSegmentationService::Status() (focused read of the sidecar model set id).
Implemented behavior:
  - Open (new Mask on the selected Color Grade, or the selected AI Mask): OpenAiMaskEdit ->
    RenderAiMaskAnalysisImage(KeyAndPixels) -> worker: sidecar lease and start, LoadModels from
    <model download directory>/alcedo-ai-mask-v1, StartImage, AiMaskEditSession.
  - Click prompt, box prompt (left drag), Add/Subtract, Prompt/Tasks mode, levels 0-2, Subject,
    Background, Sky, People and person toggles, Feather/Expand (drag preview without a step,
    release adds one step), session Undo/Redo, Retry preview, Commit, Cancel/Close.
  - Every applied step presents AcquireCoveragePixels through UpdateAiMaskCandidateCoverage.
  - Commit: PrepareCommit on the worker, CommitAiMaskEdit on the GUI thread; the result is
    matched by operation id from SessionResultObserved. A failed commit keeps the session.
  - The editor ending the edit (image change, close) closes the tool and unloads the models.
Decisions made during the work:
  - One UI-layer class instead of the app controller plus UI model of section 3: the MVP checks
    the production owners together; SM9.1 still owns the split.
  - The AiMaskEdit lease makes actions.canEdit false, which disabled the viewport pointer, wheel,
    and pan handlers. EditorWorkspace.viewportInputEnabled keeps them on while the tool is active
    (section 1: prompts and pan/zoom stay available).
  - previewStatus/previewError notify through previewChanged, not toolChanged: a Feather drag
    preview otherwise re-evaluated the slider value binding and reset the slider to the accepted
    value on every move.
  - The developer model set was copied to D:/Download/alcedo_models/alcedo-ai-mask-v1 (the
    configured model download directory) from build/tmp/sm3c/models (D13 developer setup).
Primary success call chain: header button -> EditorAiMaskTool::open -> OpenAiMaskEdit ->
  RenderAiMaskAnalysisImage -> StartOnWorker -> AiMaskEditSession -> UpdateAiMaskCandidateCoverage
  -> EditorMaskCoveragePreview -> EditorSessionRenderSchedulerPort -> viewport coverage channel.
Primary failure and restore call chain: any Open/start failure -> Fail(real error), Close stays
  available -> CancelAiMaskEdit + worker Close (unload, lease release).
Commands and exit codes:
  scripts\msvc_env.cmd --build --preset win_debug --target AiMaskSegmentationServiceTest alcedo_main: 0
  ctest -R '^AiMaskSegmentationServiceTest\.': 14/14 passed.
Manual product checks (real window, CUDA editor backend, DirectML sidecar, Sony ARW 2729x4096):
  - Open: sidecar start 2.0-3.0 s, model load 3.1-4.9 s, first encoder 2.5-4.0 s, warm
    re-open encoder 287 ms.
  - Click prompt on a spruce: 166 ms end to end, coverage displayed at the right place.
  - Sky task added to the tree: 212 ms; union displayed.
  - Feather 29 px drag preview and accept: one step; Undo/Redo restore 0 / 29 px without a model.
  - Closing the window during the edit ended the edit and unloaded the models.
Remaining issues or unavailable platforms (tracking issue #520):
  - #514: coverage disappears after the viewport releases its QRhi resources (window moved to
    another display) while the preview state stays Current.
  - #515: the accent-color tint at opacity 0.4 is often not visible; use a light red tint.
  - #516: the coverage overlay stays in the viewport after Commit.
  - #517: a click on an AI Mask in the Mask Group panel or node Mask drawer does not open the
    AI Mask edit.
  - #518: the sidecar start takes about 3 s; about 2.7 s is the CLIP profile preparation.
  - #519: four tasks and three prompt input modes (Point +/-, Path, Box) instead of the
    Prompt/Tasks switch; changes the section 1 control order.
  - Commit, reopen of a stored AI Mask, and People were not run in the window: the display
    layout changed during the run and the window input could no longer be mapped.
  - Observation: the Sky union stops about 20 px short of thin branches and ridges (SAM region
    decodes at 256 x 256 logits, SM3.C C5); visible as an untinted sky band.
  - No stroke prompt, no input markers, no keyboard/Escape rules, no translations, no
    thumbnails; SM9.1-SM9.3 own them. macOS not built.
```
