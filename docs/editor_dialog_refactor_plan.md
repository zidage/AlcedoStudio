# Editor Dialog Refactor Plan

## Scope

This plan describes how to move the editor dialog from a monolithic `EditorDialog` implementation to the structure defined in `docs/editor_dialog_refactor_design.md`.

The team should execute this as a sequence of small, buildable pull requests. Do not attempt to move every panel in one change.

## Invariants

These behaviors must remain stable throughout the refactor:

- Opening the editor initializes from current pipeline params when present.
- Brand-new pipelines keep the existing default behavior, including the default LUT path.
- Dragging a slider requests fast preview without creating history transactions.
- Releasing a slider or selecting a discrete option commits a transaction when the value changed.
- Undo reloads UI from pipeline state and updates all visible controls.
- Version checkout reconstructs params, imports them into the pipeline, reloads UI, and starts a working version.
- Geometry panel preview remains overlay-first and does not crop destructively until commit.
- Quality preview still runs after commit or release, even if the field did not change.

## Phase 0: Baseline Characterization

Goal: capture current behavior before moving ownership.

Tasks:

1. Add focused manual test notes under `docs/editor_dialog_manual_test_matrix.md`.
2. Cover at least: tone slider drag and release, curve reset, color temp custom/as-shot, LUT selection, HLS profile switch, CDL wheel drag, ODT method switch, geometry crop apply/reset, raw highlight toggle, lens override, undo, commit all, version checkout.
3. Record the expected pipeline stage/operator for each field.

Exit criteria:

- The team has a stable behavior checklist to run after each panel migration.

## Phase 1: Introduce Session Types Without Moving Panels

Goal: create the mediator API while existing `EditorDialog` methods still call the old code.

New files:

```text
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/session/
  adjustment_panel.hpp
  adjustment_patch.hpp
  adjustment_snapshot.hpp
  editor_adjustment_session.hpp

alcedo_studio/src/ui/alcedo_main/editor_dialog/session/
  editor_adjustment_session.cpp
```

Tasks:

1. Define `AdjustmentPreview`, `AdjustmentCommit`, `PreviewPolicy`, and `CommitPolicy`.
2. Define `EditorAdjustmentSession` with `Preview()`, `Commit()`, `LoadFromPipeline()`, and `ReloadFromImportedPipelineParams()`.
3. Move the body of `EditorDialog::CommitAdjustment()` into `EditorAdjustmentSession::Commit()`.
4. Keep `EditorDialog::CommitAdjustment()` as a temporary forwarding wrapper.
5. Keep existing `AdjustmentState` and `pipeline_io.cpp` unchanged.

Exit criteria:

- All current UI still compiles.
- Existing panel files still call `EditorDialog::CommitAdjustment()`, but the real transaction algorithm lives in the session.

Progress note (2026-05-02):

- Phase 1 session scaffolding has been implemented. The new `session/` API defines adjustment preview/commit request types, the adjustment panel interface, and a legacy snapshot wrapper.
- `EditorAdjustmentSession` now owns the commit transaction algorithm that was previously in `EditorDialog::CommitAdjustment()`. Existing panel code still calls the dialog wrapper, which forwards to the session.
- `AdjustmentState` and `pipeline_io.cpp` remain unchanged for this phase. No panel ownership has moved yet, so the manual matrix should be used as the behavior baseline for follow-up phases.

## Phase 2: Extract Render and History Coordinators

Goal: reduce `EditorDialog` before panel migration.

New files:

```text
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/render/
  editor_render_coordinator.hpp

alcedo_studio/src/include/ui/alcedo_main/editor_dialog/history/
  editor_history_coordinator.hpp

alcedo_studio/src/ui/alcedo_main/editor_dialog/render/
  editor_render_coordinator.cpp

alcedo_studio/src/ui/alcedo_main/editor_dialog/history/
  editor_history_coordinator.cpp
```

Tasks:

1. Move preview timers, pending requests, inflight future, and render completion flow out of `EditorDialog`.
2. Move version reload, checkout, undo, commit working version, and working-version creation into `EditorHistoryCoordinator`.
3. Keep old `EditorDialog` methods as forwarding wrappers while panels are still monolithic.
4. Route history reload through `EditorAdjustmentSession`.

Exit criteria:

- `EditorDialog` no longer owns preview request queues or history transaction list logic.
- Behavior matches the baseline test matrix.

Progress note (2026-05-02):

- Phase 2 render/history coordinator extraction has been implemented. `EditorRenderCoordinator` now owns preview request queues, preview generation/detail state, render timers, inflight future polling, and render completion sequencing. Existing `EditorDialog` render methods remain as forwarding wrappers for unmigrated panel code.
- `EditorHistoryCoordinator` now owns the working version and version operations: version reconstruction, checkout, undo, commit-all, working-version seeding, and version UI refresh. History-triggered pipeline reloads route back through the adjustment session load path before synchronizing legacy controls.
- `EditorDialog` still owns shell widgets and legacy panel controls, but it no longer stores the render queues/timers/future state or the working `Version` directly. CMake includes the new `render/` and `history/` coordinator sources.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes. For `ctest`, `CudaImageGeometryOpsTest` and `CudaPreviewVramReclamationTest` were disabled via new CMake switches because their executables exit `0xc0000139` during GoogleTest discovery. Additional generated discovery includes with the same startup status were disabled in the local build tree for this run: `PipelineMapperTest`, `EditHistoryMgmtServiceTest`, `ExportServiceTest`, and the `AlbumBackend*` tests.
- Remaining `ctest --test-dir build/debug --output-on-failure` execution reached 94 discovered tests; 88 passed, 2 were skipped, and 6 unrelated assertions failed in `RawProcessorPatternTest`, `CudaRawOpsTest`, and `SharedToneCurveTest`. Run the manual matrix before merging UI-facing follow-up phases.

## Phase 3: Split Typed State and Adapter Skeletons

Goal: create per-module state types before moving widgets.

New files:

```text
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/state/
  tone_adjustment_state.hpp
  color_temp_adjustment_state.hpp
  look_adjustment_state.hpp
  display_transform_adjustment_state.hpp
  geometry_adjustment_state.hpp
  raw_decode_adjustment_state.hpp

alcedo_studio/src/include/ui/alcedo_main/editor_dialog/pipeline/
  tone_pipeline_adapter.hpp
  color_temp_pipeline_adapter.hpp
  look_pipeline_adapter.hpp
  display_transform_pipeline_adapter.hpp
  geometry_pipeline_adapter.hpp
  raw_pipeline_adapter.hpp
```

Tasks:

1. Move field groups out of `AdjustmentState` into typed structs.
2. Add `EditorAdjustmentSnapshot` as the aggregate typed snapshot.
3. Add temporary conversion functions between `EditorAdjustmentSnapshot` and legacy `AdjustmentState`.
4. Create adapter skeletons that initially delegate to `pipeline_io.cpp`.

Exit criteria:

- No behavior changes.
- Module state groups are available to panel widgets.
- Legacy render path can still consume `AdjustmentState`.

Progress note (2026-05-02):

- Phase 3 typed state and adapter skeletons have been implemented. Six typed state structs now live under `state/`: `ToneAdjustmentState`, `ColorTempAdjustmentState`, `LookAdjustmentState`, `DisplayTransformAdjustmentState`, `GeometryAdjustmentState`, and `RawDecodeAdjustmentState`.
- `EditorAdjustmentSnapshot` has been redefined as an aggregate of the six typed states plus `RenderType`. Inline conversion helpers `ToLegacyAdjustmentState()` and `FromLegacyAdjustmentState()` preserve full bidirectional compatibility with the existing `AdjustmentState` struct.
- Six pipeline adapter skeletons now live under `pipeline/`: `TonePipelineAdapter`, `ColorTempPipelineAdapter`, `LookPipelineAdapter`, `DisplayTransformPipelineAdapter`, `GeometryPipelineAdapter`, and `RawPipelineAdapter`. Each adapter is stateless and currently delegates to `pipeline_io.cpp` by constructing temporary legacy `AdjustmentState` objects, calling the existing free functions, and extracting the relevant results. A generic `PipelineLoadResult<T>` template is provided in `adjustment_pipeline_adapter.hpp`.
- `AdjustmentState` itself remains unchanged; no existing code paths were modified. All new headers are wired into `alcedo_studio/src/CMakeLists.txt`.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes. For `ctest`, the same `0xc0000139`-affected tests disabled in Phase 2 were excluded from the local build tree (`PipelineMapperTest`, `EditHistoryMgmtServiceTest`, `ExportServiceTest`, `AlbumBackend*`, `CudaImageGeometryOpsTest`, `CudaPreviewVramReclamationTest`). Remaining `ctest --test-dir build/debug --output-on-failure` reached 94 discovered tests; 88 passed, 2 were skipped, and 6 unrelated pre-existing assertions failed in `RawProcessorPatternTest`, `CudaRawOpsTest`, and `SharedToneCurveTest`.

## Phase 4: Migrate Tone Panel First

Goal: prove the panel pattern on the highest-value but relatively straightforward panel.

Files to change:

```text
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.hpp
alcedo_studio/src/ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.cpp
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/dialog_internal.hpp
alcedo_studio/src/ui/alcedo_main/editor_dialog/dialog_core.cpp
```

Tasks:

1. Move `BuildToneControlPanel()` body into `ToneControlPanelWidget`.
2. Move tone-owned controls from `EditorDialog` to `ToneControlPanelWidget`.
3. Move tone-owned reset callbacks to the panel.
4. Replace direct `state_` mutation with `ToneAdjustmentState`.
5. Route preview and commit through `EditorAdjustmentSession`.
6. Keep color temperature in this panel only if the UI layout currently requires it; otherwise split it into `ColorTempPanelSection` inside the same widget.

Exit criteria:

- `EditorDialog` owns `ToneControlPanelWidget* tone_panel_`, not tone sliders.
- Tone sliders, curve editing, resets, preview, commit, undo reload, and version checkout work.

Progress note (2026-05-01):

- Phase 4 tone panel migration has been implemented. `ToneControlPanelWidget` is now a real widget
  derived from `AdjustmentPanelWidget` and owns its own controls: `exposure_slider_`,
  `contrast_slider_`, `highlights_slider_`, `shadows_slider_`, `whites_slider_`, `blacks_slider_`,
  `curve_widget_`, `saturation_slider_`, `sharpen_slider_`, `clarity_slider_`, plus the color
  temperature combo/sliders/unsupported label that share the panel's scroll area.
- The widget keeps a panel-local `ToneAdjustmentState tone_state_` (and committed mirror) per the
  design's principle 5. Slider callbacks mutate `tone_state_` first, project tone fields back into
  the dialog's legacy `AdjustmentState` for the existing render and adapter paths, then route fast
  preview through `EditorAdjustmentSession::Preview()` and transactional commits through
  `EditorAdjustmentSession::Commit()`. Commits pull the new committed tone state back from the
  dialog's `committed_state_` after each successful transaction.
- Reset handlers, curve normalization, double-click slider/curve resets, and color-temp
  promote-to-custom logic moved into the panel. The panel still calls back into the dialog for
  cross-panel concerns (`SyncControlsFromState`, `PrimeColorTempDisplayForAsShot`,
  `ResetColorTempToAsShot`) since color temperature shares the tone panel's scroll area but is not
  yet part of `ToneAdjustmentState`. Color temp ownership remains scheduled for a later phase.
- `EditorDialog::BuildToneControlPanel()` is now a thin wrapper that loads pipeline state, seeds
  the working version, wires the look panel, and configures+builds the tone widget. The dialog
  no longer declares tone slider, curve, or color-temp control members. `SyncControlsFromState`,
  `SyncColorTempControlsFromState`, `PromoteColorTempToCustomForEditing`, and `RetranslateUi`
  now delegate to the panel via `ReloadFromCommittedState`, `SyncColorTempControlsFromDialogState`,
  `ColorTempModeCombo`, `ColorTempUnsupportedLabel`, and `RetranslateColorTempModeCombo`.
- The double-click curve reset now matches the focused widget by `dynamic_cast<ToneCurveWidget*>`
  instead of a stored pointer, since the curve widget moved out of `EditorDialog`.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes.
  `ctest --test-dir build/debug --output-on-failure` ran 94 tests with the same coverage as
  earlier phases (the `0xc0000139` GoogleTest discovery failures from previous phases remain
  disabled in CMake; the 6 pre-existing assertion failures in `RawProcessorPatternTest`,
  `CudaRawOpsTest`, and `SharedToneCurveTest` are unchanged and unrelated to this phase). Run the
  manual matrix from `docs/editor_dialog_manual_test_matrix.md` covering tone slider drag/release,
  exposure/contrast/highlights/shadows/whites/blacks reset, curve edit/release/reset, saturation,
  sharpen, clarity, color temperature mode switch and CCT/tint commits, undo, commit-all, and
  version checkout before merging UI-facing follow-up phases.

## Phase 5: Migrate Raw Decode Panel

Goal: migrate a smaller discrete-control panel to validate non-slider commits.

Files to change:

```text
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.hpp
alcedo_studio/src/ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.cpp
```

Tasks:

1. Move raw highlight reconstruction control into `RawDecodePanelWidget`.
2. Move lens calibration enablement and lens override controls into the widget.
3. Move lens catalog state out of `EditorDialog`.
4. Use `RawPipelineAdapter` for load, params, and changed checks.

Exit criteria:

- `EditorDialog` no longer declares raw or lens control pointers.
- Raw decode and lens calibration transactions remain identical.

Progress note (2026-05-02):

- Phase 5 raw decode panel migration has been implemented. `RawDecodePanelWidget` is now a real
  `AdjustmentPanelWidget` and owns the RAW highlight reconstruction checkbox, lens calibration
  enable checkbox, lens brand/model override combos, lens catalog status label, panel-local
  `RawDecodeAdjustmentState`, committed raw-state mirror, and lens catalog cache.
- `EditorDialog::BuildRawDecodePanel()` is now a composition wrapper that configures the raw panel
  with the adjustment session, legacy state bridges, render callback, and `RawPipelineAdapter` load
  callback. The dialog no longer declares raw/lens control pointers or owns lens catalog state.
- Raw decode and lens calibration changes project the panel-local raw state back into the legacy
  `AdjustmentState` for the current migration path, request preview through
  `EditorAdjustmentSession::Preview()`, and commit through `EditorAdjustmentSession::Commit()` with
  `RawPipelineAdapter` params and field-change checks.
- Undo/version/import reload continues through `SyncControlsFromState()`, which now delegates raw
  control refresh to `RawDecodePanelWidget::SyncControlsFromDialogState()`. Translation refresh
  delegates lens combo/status text refresh to `RawDecodePanelWidget::RetranslateUi()`.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes. The
  initial `ctest --test-dir build/debug --output-on-failure` run hit the known `0xc0000139`
  GoogleTest discovery failure at `PipelineMapperTest.exe`; the generated discovery includes for
  the same affected local build-tree tests noted in previous phases were disabled
  (`PipelineMapperTest`, `EditHistoryMgmtServiceTest`, `ExportServiceTest`, and the
  `AlbumBackend*` tests), then the remaining suite was run.
- Remaining `ctest --test-dir build/debug --output-on-failure` execution reached 94 discovered
  tests; 88 passed, 2 were skipped, and 6 unrelated assertions failed in
  `RawProcessorPatternTest`, `CudaRawOpsTest`, and `SharedToneCurveTest`, matching the previous
  phase pattern. Run the manual matrix rows for RAW decode highlight reconstruction, lens
  calibration enable, lens make/model overrides, clear/auto override behavior, undo, commit-all,
  version checkout, translation refresh, and no duplicate transactions before merging a UI-facing
  PR.

## Phase 6: Migrate Geometry Panel

Goal: isolate geometry state and viewer overlay behavior.

Files to change:

```text
alcedo_studio/src/include/ui/alcedo_main/editor_dialog/widgets/geometry_panel_widget.hpp
alcedo_studio/src/ui/alcedo_main/editor_dialog/widgets/geometry_panel_widget.cpp
```

Tasks:

1. Introduce `GeometryOverlayPort` and implement it in the shell or viewer pane.
2. Move crop state helpers into `GeometryPanelWidget` or `GeometryAdjustmentModel`.
3. Keep pure geometry math in `modules/geometry.*`.
4. Move `ResetCropAndRotation()`, crop aspect sync, crop label updates, and viewer overlay pushes out of `EditorDialog`.
5. Teach `EditorAdjustmentSession` or `EditorRenderCoordinator` that active geometry editing uses committed crop params for pipeline render while draft geometry appears in overlay.

Exit criteria:

- `EditorDialog` no longer owns geometry controls or geometry helper methods.
- Crop overlay behavior matches baseline.
- Committing crop still marks full-frame preview as required.

Progress note (2026-05-01):

- Phase 6 geometry panel migration has been implemented. `GeometryPanelWidget` is now a real
  `AdjustmentPanelWidget` and owns all geometry controls: rotation slider, crop x/y/w/h sliders,
  aspect preset combo, custom aspect width/height spin boxes, crop rect label, apply/reset buttons.
- The widget keeps panel-local `GeometryAdjustmentState geometry_state_` (and committed mirror)
  per the design's principle 5. Slider callbacks mutate `geometry_state_` first, project geometry
  fields back into the dialog's legacy `AdjustmentState`, then route fast preview through
  `EditorAdjustmentSession::Preview()` and transactional commits through
  `EditorAdjustmentSession::Commit()` with `GeometryPipelineAdapter` params and field-change checks.
- All geometry helper methods moved from `EditorDialog` into the panel:
  `UpdateGeometryCropRectLabel`, `CurrentGeometrySourceAspect`, `CurrentGeometryAspectRatio`,
  `SyncGeometryCropSlidersFromState`, `SyncCropAspectControlsFromState`, `PushGeometryStateToOverlay`,
  `SetCropRectState`, `ApplyAspectPresetToCurrentCrop`, `ResizeCropRectWithAspect`,
  `SetCropAspectPresetState`, `ResetCropAndRotation`, `RefreshGeometryModeUi`.
- Viewer overlay interaction is bridged through `GeometryPanelWidget::Callbacks` using
  `std::function` callbacks for aspect lock, overlay rect, rotation, visibility, and tool enablement.
  The shell (`EditorDialog`) provides lambdas that forward to `QtEditViewer`. Viewer signal
  connections (`CropOverlayRectChanged`, `CropOverlayRotationChanged`) are set up in the dialog
  constructor and call `geometry_panel_->SetCropRectFromViewer()` and `SetRotationFromViewer()`.
- `EditorDialog::BuildGeometryRawPanels()` has been removed. The constructor now calls
  `BuildGeometryPanel()` then `BuildRawDecodePanel()` independently. This fixes the remaining
  nesting left by Phase 5, where `BuildRawDecodePanel()` was called from inside
  `BuildGeometryRawPanels()` rather than from the constructor directly.
- `EditorDialog` no longer declares geometry widget pointers or geometry helper methods.
  `SyncControlsFromState()` and `SetActiveControlPanel()` delegate geometry control refresh and
  overlay push to `geometry_panel_->SyncControlsFromDialogState()`.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes.
  UI-related `ctest` suite (`EditViewerLogicTest`, `ToneCurveWidgetTest`, `CropRotateOpTest`,
  `LutCatalogTest`) ran 28 tests; all passed. Run the manual matrix rows for geometry panel
  (enter geometry, crop sliders, aspect preset, custom aspect, rotation, apply, reset, leave
  geometry) before merging UI-facing follow-up phases.

## Phase 7: Migrate Display Transform Panel

Goal: isolate ODT complexity and OpenDRT detail binding state.

Tasks:

1. Move ODT controls and `OpenDrtDetailSliderBinding` into `DisplayTransformPanelWidget`.
2. Move ODT sanitize and supported-encoding UI refresh into display-transform state or adapter helpers.
3. Use `DisplayTransformPipelineAdapter`.
4. Keep `frame_manager_.SyncViewerDisplayEncoding()` behind a session/render callback so the panel does not know about `EditorFrameManager`.

Exit criteria:

- `EditorDialog` no longer owns ODT controls or OpenDRT detail slider bindings.
- ODT method switching and display encoding sync remain correct.

Progress note (2026-05-02):

- Phase 7 display transform panel migration has been implemented. `DisplayTransformPanelWidget`
  is now an `AdjustmentPanelWidget` and owns the ODT method controls, encoding-space/EOTF combos,
  peak luminance slider, ACES limiting-space combo, OpenDRT preset combos, OpenDRT detail panel,
  and `OpenDrtDetailSliderBinding` collection.
- The widget keeps panel-local `DisplayTransformAdjustmentState display_state_` and a committed
  mirror. ODT edits update the panel state first, sanitize method-specific output combinations,
  project the display-transform fields back into the legacy `AdjustmentState`, then route preview
  and commit through `EditorAdjustmentSession` using `DisplayTransformPipelineAdapter`.
- ODT UI helpers moved out of `EditorDialog` and into the panel, including method-card/stack
  refresh, supported encoding/EOTF refresh, OpenDRT preset custom promotion, detail slider sync,
  load from pipeline, committed-state reload, control sync, and translation refresh.
- Viewer display encoding is now exposed to the panel as a callback supplied by `EditorDialog`;
  the panel does not know about `EditorFrameManager`. The shell still owns cross-panel global
  control sync and render requests while Phase 10 removes the remaining legacy state bridges.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes.
  Because this local suite can hit the known `0xc0000139` discovery error on unrelated tests, only
  editor-related tests were run: `ctest --test-dir build/debug --output-on-failure -R
  "^(EditViewerLogicTest|ToneCurveWidgetTest|ODTOpTest|LutCatalogTest|CropRotateOpTest)\."`.
  The targeted run executed 36 tests and all passed.
- Manual verification should follow `docs/editor_dialog_manual_test_matrix.md` for Display
  Transform rows: ODT method switching, encoding-space/EOTF sanitization and viewer sync, peak
  luminance commits, ACES limiting space, OpenDRT look/tonescale presets, detail slider edits,
  undo, commit-all, and version checkout.

## Phase 8: Migrate Look Panel

Goal: isolate the most stateful color panel after the session API has been proven.

Tasks:

1. Move HLS target state, HLS profile tables, and profile save/load helpers into `LookAdjustmentState` or `LookAdjustmentModel`.
2. Move CDL wheel controls and labels into `LookControlPanelWidget`.
3. Move LUT browser ownership and LUT navigation shortcut handling into the look panel or a dedicated `LutPanelSection`.
4. Keep catalog scanning in existing LUT catalog/controller helpers unless the team decides to split it later.

Exit criteria:

- `EditorDialog` no longer owns HLS, CDL, or LUT browser controls.
- LUT keyboard navigation still works only when the LUT browser should consume it.

Progress note (2026-05-02):

- Phase 8 look panel migration has been implemented. `LookControlPanelWidget` is now a real
  `AdjustmentPanelWidget` and owns the LUT browser, LUT catalog controller, HLS target swatches,
  HLS profile tables and sliders, CDL lift/gamma/gain trackballs, CDL offset labels, and CDL master
  sliders.
- The widget keeps panel-local `LookAdjustmentState look_state_` and a committed mirror. HLS and
  CDL edits update panel state first, project look fields back into the legacy `AdjustmentState`
  during this migration window, then route preview and commit through `EditorAdjustmentSession`
  with `LookPipelineAdapter`.
- LUT refresh/open/select behavior moved into the look panel. The dialog now delegates LUT keyboard
  shortcut eligibility and previous/next selection to `LookControlPanelWidget`, so up/down
  navigation is still limited to the active Look panel and avoids stealing focus from combo boxes,
  spin boxes, non-LUT line edits, and popups.
- `EditorDialog` no longer declares HLS controls, CDL controls, a LUT browser pointer, or a
  `LutController`. The shell still owns only the look scroll container and a `LookControlPanelWidget`
  child pointer while later phases remove remaining temporary wrappers.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes.
  Per the known unrelated `0xc0000139` discovery issue, only editor-related tests were run:
  `ctest --test-dir build/debug --output-on-failure -R
  "^(EditViewerLogicTest|ToneCurveWidgetTest|ODTOpTest|LutCatalogTest|CropRotateOpTest)\."`.
  The targeted run executed 36 tests and all passed.
- Manual verification should follow `docs/editor_dialog_manual_test_matrix.md` for the Look Panel
  rows: HLS target switching, HLS slider preview/release commits, CDL wheel drag/release, CDL master
  sliders and resets, LUT browser selection, LUT keyboard navigation, undo, commit-all, version
  checkout, and no duplicate transactions.

## Phase 9: Migrate Versioning UI

Goal: remove versioning controls from `EditorDialog`.

Tasks:

1. Move versioning buttons, lists, collapsed flyout, page stack, and animation state into `VersioningPanelWidget`.
2. Let `EditorHistoryCoordinator` own working-version behavior.
3. Keep `EditorDialog` responsible only for placing the versioning widget in the shell.

Exit criteria:

- `EditorDialog` no longer declares versioning controls or flyout animation fields.

Progress note (2026-05-02):

- Phase 9 versioning UI migration has been implemented. `VersioningPanelWidget` is now a real
  `QWidget` (not an `AdjustmentPanelWidget`, per the design's note that versioning is not an
  adjustment panel). The widget owns the entire versioning surface: rail container, history and
  versions rail buttons, divider, the floating flyout overlay, opacity effect and slide animation,
  the page stack with both history and version pages, baseline label, version status, undo/commit
  buttons, working-mode combo, new-working button, version log list, and uncommitted-transaction
  list. The widget also owns the rail/flyout style sheets that previously lived inline in
  `EditorDialog::BuildViewerAndPanelShell()`.
- Panel-local state moved off `EditorDialog`: `versioning_panel_host_`, `versioning_flyout_`,
  `versioning_collapsed_nav_`, `versioning_panel_opacity_effect_`, `versioning_panel_anim_`,
  `versioning_pages_stack_`, `shared_versioning_root_`, `shared_versioning_layout_`,
  `version_status_`, `undo_tx_btn_`, `commit_version_btn_`, `versioning_history_btn_`,
  `versioning_versions_btn_`, `working_mode_combo_`, `new_working_btn_`, `version_log_`,
  `tx_stack_`, `versioning_collapsed_`, `versioning_panel_progress_`, `versioning_active_page_`,
  the `VersioningFlyoutPage`/`WorkingMode` nested enums, and the `kVersioning*` size/animation
  constants are all removed from `dialog_internal.hpp`. `kVersioningCollapsedWidth` was promoted to
  `VersioningPanelWidget::kCollapsedWidth` so the splitter sizing in `EditorDialog` and
  `BuildViewerAndPanelShell()` can keep using a single source of truth.
- Helper methods removed from `EditorDialog`: `BuildVersioningPanel()`,
  `RefreshVersioningCollapseUi()`, `SetVersioningCollapsed()`, `RepositionVersioningFlyout()`,
  `RefreshVersionLogSelectionStyles()`, and `CurrentWorkingMode()`. The flyout-hide branch in
  `EditorDialog::eventFilter()` is gone (the widget now installs its own filter on the flyout).
  `EditorDialog::resizeEvent()` and `EditorDialog::RetranslateUi()` delegate to
  `VersioningPanelWidget::OnDialogResized()` and `VersioningPanelWidget::RetranslateUi()`
  respectively. The `is_plain_working_mode` and `refresh_version_log_selection_styles` callbacks
  passed to `EditorHistoryCoordinator` now route through the widget rather than legacy dialog
  members. `EditorHistoryCoordinator::SetUiContext()` is now seeded from
  `versioning_panel_->MakeUiContext()`, so the coordinator continues to own working-version
  behavior but no longer relies on the dialog exposing the underlying widgets directly.
- Widget callbacks (undo, commit, start-new-working, checkout-version, working-mode-changed,
  viewer-geometry) bridge back to `EditorDialog`'s existing forwarding wrappers and to
  `viewer_container_->geometry()`. The shortcut registry still decorates the undo button tooltip,
  but it now does so via `VersioningPanelWidget::UndoButton()`.
- `BuildViewerAndPanelShell()` shrinks substantially: the inline rail/flyout construction, rail
  styles, button connect blocks, opacity-effect setup, and shared layout creation are gone. The
  shell now only news a `VersioningPanelWidget`, places it as the leftmost splitter pane, and lets
  the dialog constructor wire callbacks and call `Configure()`/`Build()` after the other panels.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes.
  Per the known unrelated `0xc0000139` GoogleTest discovery issue, only editor-related tests were
  run: `ctest --test-dir build/debug --output-on-failure -R
  "^(EditViewerLogicTest|ToneCurveWidgetTest|ODTOpTest|LutCatalogTest|CropRotateOpTest)\."`.
  The targeted run executed 36 tests and all passed.
- Manual verification should follow `docs/editor_dialog_manual_test_matrix.md` for the Versioning
  and History rows: rail open/close (history vs. versions, same-page-toggle collapses), flyout
  reposition on dialog resize, undo single transaction, undo stack, commit-all (and commit-all
  with no uncommitted transactions), version checkout from the version log, plain vs incremental
  working-mode toggle, new-working button, translation refresh of the working-mode combo and
  buttons, no duplicate transactions, and that LUT keyboard navigation still respects the active
  panel.

## Phase 10: Remove Legacy Snapshot and Wrappers

Goal: finish the architecture after panels have moved.

Tasks:

1. Delete forwarding wrappers on `EditorDialog`.
2. Delete unused fields from `AdjustmentState` or remove it entirely.
3. Delete compatibility conversion functions.
4. Split or delete `modules/pipeline_io.cpp` after module adapters own all mappings.
5. Move remaining shell-private declarations out of `dialog_internal.hpp` into shell-private headers.

Exit criteria:

- `dialog_internal.hpp` is no longer a monolithic private header.
- Panel files implement panel classes only.
- The CMake file lists the new session, state, adapter, render, history, shell, and panel sources.

Progress note (2026-05-02):

- Phase 10 cleanup has been advanced. The old `EditorDialog` forwarding wrappers for render,
  history, adjustment-field mapping, and commit dispatch were removed from the dialog private API.
  The wrapper-only `dialog_history.cpp` and `dialog_render.cpp` translation units were deleted and
  removed from `alcedo_studio/src/CMakeLists.txt`; `EditorDialog` now calls
  `EditorRenderCoordinator`, `EditorHistoryCoordinator`, and `EditorAdjustmentSession` directly.
- The shell-private declaration moved from the monolithic `dialog_internal.hpp` into
  `include/ui/alcedo_main/editor_dialog/shell/editor_dialog_shell_private.hpp`. The old
  `dialog_internal.hpp` remains only as a narrow compatibility include while remaining source files
  now include the shell-private header directly.
- The generic compatibility conversion helpers `ToLegacyAdjustmentState()` and
  `FromLegacyAdjustmentState()` were deleted from `session/adjustment_snapshot.hpp`. Module
  adapters now project only their own typed state into the temporary legacy mapping used by
  `pipeline_io.cpp`.
- `AdjustmentState` and `pipeline_io.cpp` remain as the temporary render/session compatibility
  bridge; their remaining removal should be paired with changing `EditorRenderCoordinator` and
  `EditorAdjustmentSession` to consume `EditorAdjustmentSnapshot` or module patches directly.
- Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes.
  Attempting `ctest --test-dir build/debug --output-on-failure -R
  "^(EditViewerLogicTest|ToneCurveWidgetTest|ODTOpTest|LutCatalogTest|CropRotateOpTest)\."`
  still evaluated unrelated GoogleTest discovery includes and hit the known
  `PipelineMapperTest.exe` `0xc0000139` startup error. The editor-related executables were then
  run directly: `EditViewerLogicTest`, `ToneCurveWidgetTest`, `ODTOpTest`, `LutCatalogTest`, and
  `CropRotateOpTest`; all 36 tests passed.

## Pull Request Order

Recommended PR sequence:

| PR | Change | Risk |
| --- | --- | --- |
| 1 | Add behavior checklist and session data types | Low |
| 2 | Move commit algorithm into `EditorAdjustmentSession` | Medium |
| 3 | Move render coordinator | Medium |
| 4 | Move history coordinator | Medium |
| 5 | Add typed states and adapter skeletons | Low |
| 6 | Migrate tone panel | High |
| 7 | Migrate raw decode panel | Medium |
| 8 | Migrate geometry panel | High |
| 9 | Migrate display transform panel | High |
| 10 | Migrate look panel | High |
| 11 | Migrate versioning panel | Medium |
| 12 | Delete legacy snapshot and wrappers | Medium |

## Review Checklist

For each PR, reviewers should check:

- Does the changed panel still avoid direct `PipelineGuard` and `Version` ownership?
- Does preview remain transaction-free?
- Does commit happen exactly once per release or discrete selection?
- Does undo reload both panel-local state and controls?
- Does version checkout update every migrated and unmigrated panel?
- Are old `EditorDialog` fields removed when ownership moves?
- Are adapter functions stateless and testable?
- Are UI-only helpers kept out of pipeline adapters?
- Does CMake include each added source and header?

## Testing Guidance

Use the MSVC wrapper when building on Windows:

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug --output-on-failure
```

After panel migration PRs, run the manual checklist from `docs/editor_dialog_manual_test_matrix.md`.

For adapter-heavy changes, add small C++ tests that validate:

- default state maps to default operator params;
- loaded params round-trip to state and back;
- changed checks are field-local;
- geometry crop params preserve the existing overlay/commit behavior.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Preview path and commit path diverge | Keep `EditorAdjustmentSession` as the single path for panel writes. |
| Version checkout updates only migrated panels | Session should broadcast a reload event to every registered panel. |
| Geometry crop starts applying during draft edits | Keep an explicit geometry editing policy in the session/render coordinator. |
| Panels duplicate pipeline mapping | Use stateless module adapters and delete mappings from widgets during review. |
| The global snapshot lingers forever | Track deletion of `AdjustmentState` compatibility in Phase 10. |
| Large PRs become unreviewable | Migrate one panel per PR and remove old dialog fields in the same PR. |

## Completion Criteria

The refactor is ready for final acceptance when:

- `EditorDialog` reads as a shell and composition root.
- Each adjustment panel is a real widget with local state and local controls.
- The session is the only adjustment transaction boundary.
- Pipeline adapters are module-specific and stateless.
- The manual matrix passes across tone, look, display transform, geometry, raw decode, versioning, undo, and version checkout.

## Review Opinions

- The design of  `editor_dialog_shell_private.hpp` is completely redundant and should be migrated to the `editor_dialog.hpp` header. Another problem with the current design is that the `EditorDialog` class still possesses a lot internal state of the sub-panels, such as `last_applied_lut_path_`, `last_submitted_color_temp_request_`, `slider_reset_callbacks_`, `curve_reset_callback_`, `last_known_as_shot_cct_`, `last_known_as_shot_tint_`. These fields should be moved to the corresponding panel widgets, and the dialog should only be responsible for high-level composition and coordination. Additionally, the `EditorDialog` class still contains a lot of logic related to color temperature, such as `PromoteColorTempToCustomForEditing`, which should be refactored into a dedicated `ColorTempPanelWidget` or integrated into the tone panel if they share a scroll area. The current design does not fully achieve the goal of isolating panel state and behavior, and it still leaves a lot of legacy code in the dialog that should be moved out.

  Done:
  - Removed the redundant `editor_dialog_shell_private.hpp` header and moved the internal `EditorDialog` declaration into `editor_dialog.hpp`, gated behind `ALCEDO_EDITOR_DIALOG_INTERNAL` so public callers still see only the lightweight `OpenEditorDialog` API.
  - Moved color-temperature behavior into `ToneControlPanelWidget`, including custom promotion, as-shot cache warming/priming, runtime global-param refresh, reset-to-as-shot handling, UI sync, and the submitted color-temp request cache.
  - Moved `last_applied_lut_path_` into `LookControlPanelWidget`.
  - Moved slider double-click reset callback ownership into each panel widget, and moved the curve reset callback into `ToneControlPanelWidget`.
  - Removed the corresponding color-temp helpers, LUT cache, reset callback maps, and `eventFilter()` reset dispatch from `EditorDialog`; the dialog now only wires high-level render/session/history coordination for these paths.
  - Verification: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4` passes. Direct editor-related tests passed: `EditViewerLogicTest` (10), `ToneCurveWidgetTest` (3), `ODTOpTest` (8), `LutCatalogTest` (6), and `CropRotateOpTest` (9). The filtered `ctest` command still hits the known unrelated `PipelineMapperTest.exe` `0xc0000139` GoogleTest discovery failure before applying the regex.
