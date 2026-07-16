# Background Tasks and Declarative UI State Plan

Date: 2026-07-01

Primary roadmap owner: `alcedo_studio/src/ui/alcedo_main`

Status: planning. This plan is intentionally small: it captures the direction
for long-running UI tasks and the UI state model before committing to a large
rewrite.

## Problem

Several long-running workflows are currently presented as modal or near-modal
experiences:

- advanced image analysis runs inside `AdvancedContentAnalysisDialog.qml`, which
  is modal and cannot be closed while `ImageAnalysisController.running` is true;
- semantic label generation runs inside `SemanticGenerationDialog.qml`, also
  modal while generation is running;
- model download and activation expose progress inside the semantic settings
  panel, with local `modelDownloadRunning` and `modelActivationRunning` checks;
- import still uses a full-screen blocking overlay;
- `AlbumBackend` exposes only one coarse `taskStatus/taskProgress` pair, not a
  list of tasks or a policy for which actions should be disabled.

This was tolerable for short operations, but remote advanced image analysis can
run for one or two hours. In that case the task should keep running while the
user continues ordinary album browsing and image editing. Only the interactions
that can conflict with the task should be disabled.

## Current Code Shape

Relevant existing pieces:

- `ImageAnalysisController` owns remote analysis progress and an
  `ImageAnalysisJob` with `Cancel()` and `Wait()`.
- `SemanticGenerationController` owns local semantic generation progress and a
  `SemanticGenerationJob` with `Cancel()` and `Wait()`.
- `ModelDownloadController` wraps `ModelDownloadService`, which reports progress
  through Qt signals and supports `CancelDownload()`.
- `ThumbnailService` renders analysis inputs through `PipelineMgmtService`.
  For a cache miss it calls `LoadPipeline(elementId)`, schedules a thumbnail
  render, then calls `SavePipeline(...)` on the loaded guard. Because
  `PipelineMgmtService` keeps live pipeline executors by element id, this can
  touch the same pipeline object that an active editor session is modifying.
- `AlbumBackend::~AlbumBackend()` currently performs ad hoc shutdown work:
  semantic generation is canceled, search preview thumbnails are canceled, the
  editor session is finalized, import is marked canceled, and pipeline service
  is synced. Image analysis and model downloads do not have one unified shutdown
  path there.
- QML state is scattered across bindings such as `backendInteractive`,
  `analysisController.running`, `semanticController.running`, and
  `downloadController.modelDownloadRunning`.

The important observation is that the lower-level jobs already have cancellation
and progress mechanics. The missing module is the UI-facing task registry and
the policy that says what each running task blocks.

There is also a second, subtler problem: a background task can read/render a
thumbnail while the user is editing the same image. That must not make user
edits disappear, persist an accidental intermediate state, or let an analysis
result be based on an undefined mix of old and new pipeline parameters.

## Target Design

Add two small UI-facing modules:

1. `BackgroundTaskController`
2. `InteractionPolicyController`

They should be exposed from `AlbumBackend` as constant QObject properties:

- `backgroundTaskController`
- `interactionPolicyController`

The goal is not to make QML know every service detail. QML should ask simple
questions: "what tasks are running?", "can this button run?", and "why is this
disabled?"

## Background Task Model

Recommended first files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/background_task_controller.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/background_task_controller.cpp`
- `alcedo_studio/src/ui/alcedo_main/qml/BackgroundTaskBar.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/BackgroundTaskPopover.qml`

First-pass task data:

```cpp
enum class BackgroundTaskKind {
  ImageAnalysis,
  SemanticGeneration,
  ModelDownload,
  ModelActivation,
  Import,
  Export,
};

enum class BackgroundTaskState {
  Queued,
  Running,
  Canceling,
  Succeeded,
  Failed,
  Canceled,
};

enum class BackgroundTaskShutdownPolicy {
  CancelAndWait,
  WaitForFinish,
  DetachNotAllowed,
};

enum class InteractionCapability {
  EditImageDescription,
  EditImageRating,
  EditImageRatingReason,
  RunImageAnalysis,
  CommitImageAnalysisResults,
  ChangeImageAnalysisProvider,
  ChangeSemanticModel,
  RunSemanticGeneration,
  ChangeModelDownloadSettings,
  DeleteImages,
  CloseProject,
};

struct InteractionLock {
  InteractionCapability capability;
  uint64_t              element_id = 0;  // 0 means global for that capability.
  QString               reason;
};

struct BackgroundTaskSnapshot {
  QString                      id;
  BackgroundTaskKind           kind;
  BackgroundTaskState          state;
  QString                      title;
  QString                      detail;
  int                          progress_percent = 0;  // -1 for indeterminate.
  bool                         cancelable = false;
  BackgroundTaskShutdownPolicy shutdown_policy;
  QVariantList                 affected_targets;
  std::vector<InteractionLock> locks;
};
```

`BackgroundTaskController` should provide:

- `QVariantList tasks`
- `QVariantMap primaryTask`
- `int runningCount`
- `bool hasBlockingShutdownTasks`
- `Q_INVOKABLE bool CancelTask(QString taskId)`
- `Q_INVOKABLE void CancelAll()`
- C++ registration helpers for controllers to create/update/finish tasks.

In the first slice, controllers can still own their existing service jobs. They
register a task record and push progress updates into the task controller. A
later slice can move more orchestration into the background task module if that
actually simplifies callers.

## Interaction Policy Model

Recommended first files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/interaction_policy_controller.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/interaction_policy_controller.cpp`

`InteractionPolicyController` consumes:

- project readiness from `AlbumBackend`;
- accelerator preparation state;
- active background task locks;
- selected/focused image identity;
- persistent settings where relevant.

It exposes QML-friendly answers:

- `Q_INVOKABLE QVariantMap CanEditImageDescription(uint elementId)`
- `Q_INVOKABLE QVariantMap CanEditImageRating(uint elementId)`
- `Q_INVOKABLE QVariantMap CanEditImageRatingReason(uint elementId)`
- `Q_INVOKABLE QVariantMap CanRunImageAnalysis(const QVariantList& targets)`
- `Q_INVOKABLE QVariantMap CanChangeSemanticModel()`
- `Q_INVOKABLE QVariantMap CanRunSemanticGeneration()`
- `Q_INVOKABLE QVariantMap CanChangeModelDownloadSettings()`

Each result should look like:

```js
{
  allowed: true,
  reason: "",
  blockingTaskIds: []
}
```

For high-frequency bindings, add cached properties later. Do not start with a
giant property list unless profiling or QML ergonomics requires it.

## Task-Specific Blocking Rules

### Advanced Image Analysis

Task can run in the background.

Allowed while running:

- browse albums and search;
- open editor and adjust the edit pipeline;
- export unrelated or even same images, unless product direction says exports
  must include final AI fields;
- change most application settings.

Blocked while running:

- edit description, rating, or rating reason for affected images;
- start another analysis for the same affected images;
- delete or remove affected images from the project while they are in the
  running analysis set;
- change the active advanced-analysis provider/profile while the task is using
  it;
- close the app without first canceling/waiting through the shutdown protocol.

The first implementation should lock by `elementId` for image fields and
maintain an `analyzingElementIds` runtime set for delete/remove checks. A full
album analysis may represent the target as a global image-analysis lock.

Results should become visible per item. When one image finishes analysis, its
result enters a pending commit queue and is written immediately if no project
DB write barrier is active. If export or another DB-stabilizing operation holds
the barrier, the result waits in memory and flushes after the barrier is
released.

### Semantic Generation

Task can run in the background.

Allowed while running:

- browse albums and inspect generated labels as they land;
- edit images in the editor;
- change non-semantic settings.

Blocked while running:

- change or activate the semantic model;
- start another semantic generation run;
- delete the model files used by the task;
- possibly delete source images included in the task. This needs a product
  decision because the current job enumerates a fixed target list.

### Model Download

Task can run in the background.

Allowed while running:

- browse, edit, export, and run advanced remote image analysis if it does not
  need the local semantic model;
- edit unrelated settings.

Blocked while running:

- start another model download;
- change the model download directory, endpoint, or selected model for the
  in-flight download;
- activate or delete the same model until the download finishes or is canceled.

### Model Activation

Task can run in the background if progress is surfaced. It blocks semantic model
selection and semantic generation until it finishes. It should not block image
editing.

### Import and Export

Import/export can be migrated after the AI tasks. Import currently mutates the
project library and uses a full-screen overlay, so it needs more careful rules.
Export is already queue-like and is a good later candidate for the same task
bar.

Export should not wait for image analysis to finish. It should use the metadata
that is already committed when the export reads the project state. However,
while export is active, background tasks must not write to the project DB. The
first implementation should model this as a short-lived project DB write
barrier:

- export acquires the barrier before reading/exporting project metadata;
- image analysis may keep running, but finished results are queued in memory
  instead of being committed while the barrier is active;
- once export releases the barrier, queued analysis results flush through the
  normal user-confirmed overwrite path;
- if shutdown happens during this window, the shutdown path waits for export to
  release the barrier before flushing or canceling pending background writes.

## Main UI Surface

Add a compact background task area in the main window chrome:

- show a small status item when no task is active;
- show the primary running task title and progress when a task is active;
- show a count when multiple tasks are active;
- click opens a popover with all active/recent tasks;
- each task row shows title, detail, progress, status, and Cancel when
  available;
- task details can reopen the corresponding dialog/page in a non-modal mode.

For advanced image analysis, `AdvancedContentAnalysisDialog.qml` should gain a
`Run in Background` behavior:

- before start: still a normal setup dialog;
- after start: the dialog can be closed;
- the task continues and appears in the task bar;
- reopening from the task popover shows the same progress/details;
- cancel remains available from both the dialog and task popover.

For semantic generation, replace the running modal phase with task bar progress.
The import prompt can remain modal before the task starts.

## Persistent Settings, Runtime State, and UI Schema

Do not combine everything into one untyped global blob. Split it into:

- persistent settings: user preferences saved through typed wrappers over
  `QSettings` or JSON files;
- runtime state: active task snapshots, locks, temporary progress, current
  project readiness;
- effective UI state: computed answers from settings plus runtime state;
- UI schema: versioned JSON that describes selected panels, groups, control
  order, control type, and the state/action keys those controls bind to;
- theme tokens: versioned JSON that describes app-shipped colors, typography,
  radii, spacing, component variants, and progress indicators.

Possible module names:

- `UiSettingsStore`: typed reads/writes for settings that QML should not access
  directly;
- `RuntimeUiState`: volatile state such as focused image, selected targets, and
  background task locks;
- `InteractionPolicyController`: the QML-facing computed state;
- `UiSchemaStore`: loads and validates versioned JSON schemas;
- `UiSchemaController`: exposes schema sections and resolved field models to
  QML;
- `ThemeTokenStore`: loads and validates app-shipped theme token JSON;
- `AppTheme`: consumes validated theme tokens and exposes Qt/QML properties.

Recommended first files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/ui_schema_store.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/ui_schema_store.cpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/ui_schema_controller.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/ui_schema_controller.cpp`
- `alcedo_studio/src/ui/alcedo_main/qml/SchemaPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/SchemaFieldDelegate.qml`
- `alcedo_studio/src/ui/alcedo_main/schema/default_ui_schema.json`
- `alcedo_studio/src/ui/alcedo_main/schema/theme_manifest.json`
- `alcedo_studio/src/ui/alcedo_main/schema/themes/alcedo_dark.json`
- `alcedo_studio/src/ui/alcedo_main/schema/themes/classic_dark.json`
- `alcedo_studio/src/config/resource.qrc`

Bundled defaults should be compiled into Qt resources, for example:

- `:/ui_schema/default_ui_schema.json`
- `:/ui_schema/theme_manifest.json`
- `:/ui_schema/themes/alcedo_dark.json`
- `:/ui_schema/themes/classic_dark.json`

The source JSON files can live under `alcedo_studio/src/ui/alcedo_main/schema/`,
but production loading should not depend on those files being present on disk.
Development builds may support a disk override, such as a configured schema
directory or environment variable, so UI/theme iteration does not require
rebuilding resources. Invalid or missing overrides must fall back to the qrc
defaults and report a developer-visible warning. Future user-imported theme
files should use a separate import/overlay path, not the development override.

Themes should be managed through a manifest rather than one monolithic theme
file. `theme_manifest.json` lists bundled themes, their ids, display names,
resource paths, schema versions, and optional tags. Each theme JSON contains
only that theme's tokens. `AppTheme.availableThemes` should be generated from
the manifest so the built-in switcher uses the same shape as future imported
theme files.

First-version schema and theme loading should support exactly one
`schemaVersion`. Unsupported versions are rejected and fall back to the bundled
qrc defaults. Do not add migration logic until user-imported themes or external
schema overlays exist.

The first implementation should support a JSON-driven UI schema, but with a
strict whitelist. JSON may describe:

- panel sections and group ordering;
- known control types such as button, checkbox, combo box, segmented control,
  slider, text input, progress row, task row, provider picker, and model picker;
- `valueKey`, `settingKey`, `actionId`, and `capability` names that map to
  typed C++ state;
- `visibleWhen` and `enabledWhen` expressions limited to named boolean policy
  keys, not arbitrary JavaScript;
- localization keys and tooltip keys, not hard-coded long UI copy.

JSON must not contain:

- arbitrary JavaScript or QML component imports;
- storage queries, file paths, provider secrets, or network details;
- business rules for analysis, export, model activation, or project shutdown.

QML becomes the renderer for a small component library. C++ controllers still
own values, validation, side effects, task locks, and persistence. This keeps
the UI declarative without turning the schema into a second application
runtime.

First-version UI schema scope is QML-only. It should cover the main-window
surfaces that are already QML and closely related to background tasks,
settings, and analysis setup. The Qt Widgets editor should consume theme tokens
only; do not build a JSON layout renderer for editor modules yet, since those
modules may later migrate to QML.

Theme JSON should start as an app-bundled default, not a user-editable file.
Later, imported "theme files" can become validated overlays on top of the
bundled defaults. First-version theme JSON may describe:

- named color tokens currently hard-coded in `app_theme.cpp`, such as canvas,
  panel, base, text, muted text, accent, hover, divider, overlay, danger, and
  selection colors;
- typography roles, font stacks, font sizes, and weights, while keeping font
  resource registration in C++;
- shape tokens such as panel radius, button radius, input radius, card radius,
  and progress ring thickness;
- component variants for known controls such as primary button, secondary
  button, segmented control, progress ring, popover, input, checkbox, and task
  row.

Theme JSON must not contain arbitrary stylesheet strings. `AppTheme` should
translate validated tokens into Q_PROPERTY values for QML and into generated Qt
Widget stylesheets for the existing editor widgets. QML components should bind
to `appTheme` properties or resolved schema/theme field models instead of
hard-coding colors and radii. Widget stylesheets that cannot update purely by
binding must be reapplied when `ThemeChanged` fires.

First-version theme tokens must support runtime hot switching. If a proposed
token cannot be safely changed at runtime, leave that token out of the first
theme surface instead of shipping a restart-required option.

## Pipeline Snapshot Rule

Background image analysis needs a stable rendition of each target image. It
must not render through a live mutable editor pipeline without a clear snapshot
rule.

Recommended rule for the first version:

- extend `PipelineMgmtService` with a temporary read-only snapshot API;
- at task start, analysis captures the current pipeline parameters for each
  target into an independent snapshot. If the image is open in the editor, this
  snapshot should include the current live editor pipeline state;
- the snapshot is for background rendering only. It must not write to storage,
  clear dirty editor state, decrement live pipeline pins, or call
  `SavePipeline(...)` on the live guard;
- once a target is enqueued, analysis renders from that captured snapshot, not
  from whatever state the editor happens to hold later;
- user edits made after the snapshot are allowed and must continue to save
  normally;
- analysis results still write through the existing user-confirmed overwrite
  flow. Do not block or discard the result just because the user edited the
  pipeline after the snapshot was captured.

The existing executor already supports `ExportPipelineParams()` and
`ImportPipelineParams()`, so the first implementation can clone through JSON
without inventing a new pipeline serialization format:

```cpp
struct PipelineSnapshot {
  sl_element_id_t                      element_id = 0;
  image_id_t                           image_id   = 0;
  nlohmann::json                       pipeline_params;
  std::shared_ptr<CPUPipelineExecutor> executor;
};

class PipelineMgmtService final {
 public:
  auto LoadPipelineSnapshot(sl_element_id_t id) -> std::shared_ptr<PipelineSnapshot>;

  // Releases only temporary snapshot resources. This is not a storage write.
  void SavePipelineSnapshot(std::shared_ptr<PipelineSnapshot> snapshot);
};
```

The snapshot capture should hold the live executor render lock while exporting
pipeline params, then create a separate executor, import the params, set the
same bound file and accelerator preference, and build execution stages for the
background render. `SavePipelineSnapshot(...)` is intentionally a lifecycle
name, not persistence; `ReleasePipelineSnapshot(...)` may be clearer if the
call site does not need a save-shaped API.

This also points to a small image-analysis interface rather than ad hoc calls
to `ThumbnailService::GetThumbnailDetailed(...)`:

```cpp
class AnalysisRenditionProvider {
 public:
  auto CaptureSnapshot(const ImageAnalysisItem& item, std::string* error)
      -> std::shared_ptr<PipelineSnapshot>;
  void RequestThumbnail(std::shared_ptr<PipelineSnapshot> snapshot,
                        ThumbnailResolution resolution,
                        ImageAnalysisThumbnailCallback callback);
};
```

The current thumbnail path can remain for ordinary album thumbnails. Background
analysis should use the snapshot-aware path so it never pins or mutates the
live pipeline guard at all.

Interaction policy implication:

- editing pipeline parameters for an image that is already being analyzed can
  stay allowed, because the analysis has its own snapshot;
- starting analysis for an image with unsaved editor changes is allowed if
  snapshot capture can safely copy the live editor state under lock;
- closing the app must flush the editor save path before or after canceling
  background tasks according to the snapshot rule, but in all cases user edits
  win over background thumbnail cleanup.

## Shutdown Protocol

Add an explicit shutdown path before `AlbumBackend` destruction does final save
and cleanup:

1. `AlbumBackend::RequestShutdown()` or an equivalent QML-callable close gate
   asks `BackgroundTaskController` for active tasks.
2. If no tasks are active, proceed with the current close/save/package flow.
3. If tasks are active, show a short confirmation dialog. The default action is
   "Cancel background tasks and exit".
4. After confirmation, show a shutdown progress dialog:
   - "Canceling background tasks..."
   - per-task status lines;
   - no raw provider secrets or request bodies.
5. For `CancelAndWait` tasks, call their cancel callback and wait for finished.
6. For `WaitForFinish` tasks, wait until completion if they are known to be
   short and data-critical.
7. Only after tasks have finished/canceled should project state be saved,
   packaged, and the workspace cleaned.

Persistent pause/resume is out of scope for the first version. Advanced image
analysis and semantic generation are cancel-and-wait on exit. A model download
can become resumable later because aria2/staging files already make that
plausible, but it should not force every background task into a resumable model.

## Implementation Phases

### Phase 1 - Task Registry and Task Bar

- Add `BackgroundTaskController` and expose it from `AlbumBackend`.
- Add `BackgroundTaskBar.qml` and `BackgroundTaskPopover.qml`.
- Register image analysis, semantic generation, and model download tasks.
- Keep existing dialogs working, but mirror their progress into the task bar.

Acceptance:

- multiple active tasks can be listed;
- cancel from the task bar reaches the owning controller;
- task completion updates status without reopening the original dialog;
- no current modal behavior is removed yet.

### Phase 2 - Interaction Locks

- Add `InteractionPolicyController`.
- Teach image analysis, semantic generation, model download, and activation to
  publish locks.
- Teach export to publish a temporary project DB write barrier that queues
  background analysis result commits.
- Replace key QML `enabled:` bindings with policy checks in:
  - `AdvancedContentAnalysisDialog.qml`;
  - `ImageInspectorPanel.qml`;
  - `SemanticGenerationSettingsPanel.qml`;
  - model download controls in settings.

Acceptance:

- running image analysis disables description/rating/reason editing only for
  affected images;
- deleting or removing images in the active analysis set is blocked with a clear
  reason;
- editor pipeline controls remain usable during image analysis;
- semantic generation blocks semantic model changes without blocking normal
  browsing/editing;
- export does not wait for image analysis, but analysis result commits are
  queued until export releases the DB write barrier;
- disabled controls show a reason when the existing UI pattern allows it.

### Phase 3 - Snapshot-Based Analysis Rendering

- Add the temporary pipeline snapshot API to `PipelineMgmtService`.
- Add an analysis rendition path that renders from `PipelineSnapshot` instead
  of loading and saving the live thumbnail pipeline guard.
- Keep ordinary album thumbnails on the existing path until they need the same
  isolation.

Acceptance:

- analysis can capture the currently visible editor pipeline state without
  forcing it to disk first;
- later user edits save through the normal editor path and are not overwritten
  by the analysis thumbnail render;
- analysis results can still be written through the existing overwrite
  confirmation even if the user has edited the image since task start.

### Phase 4 - Make Long AI Tasks Truly Backgroundable

- Let `AdvancedContentAnalysisDialog.qml` close while analysis is running.
- Let semantic generation leave the modal progress popup after start.
- Reopen task details from the task popover.
- Remove the assumption that a running AI task owns the whole overlay layer.

Acceptance:

- a one-hour image analysis can run while the user edits another image;
- the focused image inspector refreshes when analysis results land;
- completed items become visible during the run instead of waiting for the
  whole batch to finish;
- fields affected by the running task stay disabled until the task finishes;
- cancel works from the task popover.

### Phase 5 - Unified Shutdown

- Add a close gate that asks the task controller to cancel/wait active tasks.
- Move ad hoc destructor cancellation into the explicit shutdown path.
- Keep destructor defensive, but not the primary place where user-visible task
  cleanup happens.

Acceptance:

- closing the app with image analysis running cancels it and waits for the job
  to settle before project save/package;
- closing with model download running cancels the download and stops the worker;
- closing with semantic generation running cancels and joins the job;
- the app does not destroy a controller while its task callback can still write
  into QML-facing state.

### Phase 6 - JSON UI Schema Foundation

- Introduce typed wrappers for settings currently read directly from scattered
  controllers.
- Add `UiSchemaStore` with versioned JSON schema loading and validation.
- Add `UiSchemaController` for QML-facing schema sections and resolved field
  models.
- Add `ThemeTokenStore` and teach `AppTheme` to read app-bundled default theme
  tokens instead of hard-coded theme structs.
- Add bundled schema/theme JSON files to `alcedo_studio/src/config/resource.qrc`
  and load qrc defaults as the production source of truth.
- Add `theme_manifest.json` and per-theme token files. Use the manifest to
  populate `AppTheme.availableThemes`.
- Validate a single supported schema/theme version. Reject unsupported versions
  and fall back to qrc defaults; do not implement migrations yet.
- Add an optional development-only disk override for faster schema/theme
  iteration, with qrc fallback on missing or invalid files.
- Add a runtime theme switch path: select a bundled theme, validate it, apply it
  to `AppTheme`, emit `ThemeChanged`, and reapply widget stylesheets.
- Build a small QML schema renderer for known controls instead of allowing
  arbitrary component names. Limit this renderer to QML main-window surfaces.
- Tokenize common QML/Widget styling values: colors, radii, spacing, button
  variants, progress ring styling, popover surfaces, and input surfaces.
- Start with settings and panels that affect task policy:
  - semantic model selection and directories;
  - semantic import-generation preference;
  - image analysis rating severity;
  - output language;
  - thumbnail cache settings if they need shared UI state;
  - advanced image analysis setup;
  - semantic generation settings;
  - background task popover rows.
- Keep Qt Widgets editor module layout out of JSON schema. It should use theme
  tokens for colors, radii, and generated stylesheets only.
- Keep raw provider credentials out of settings, as today.
- Do not support user-imported theme files in the first version. Reserve that
  for a later overlay/migration system.

Acceptance:

- QML reads effective state from controllers, not scattered `QSettings` keys;
- QML renders selected panels from validated schema sections;
- Qt Widgets editor modules do not need a schema renderer and remain structurally
  unchanged;
- invalid schema falls back to a bundled safe default and reports a developer
  visible error;
- invalid theme JSON falls back to the bundled safe default;
- invalid theme manifest entries are ignored or fall back to the default theme
  without preventing startup;
- unsupported schema/theme versions are rejected without migration and fall back
  to bundled defaults;
- production startup succeeds without any schema/theme JSON beside the
  executable because qrc defaults are sufficient;
- development disk overrides never replace qrc fallback safety;
- `AppTheme` exposes theme-token-backed properties to QML and generates widget
  styles from validated tokens;
- bundled themes can be switched at runtime without restarting the app;
- existing QML controls can use theme tokens for button style, progress ring
  style, radius, colors, and spacing without editing every call site;
- controllers do not duplicate normalization logic for the same setting;
- schema can change control order, grouping, labels, tooltips, and known control
  types for selected QML panels without editing panel QML;
- runtime task locks never overwrite persistent user choices.

## Incremental Implementation Plan

Use these as small implementation slices. Each slice should compile on its own
and leave the application usable.

### Step 1 - Background Task Registry Skeleton

Scope:

- add `BackgroundTaskController` with in-memory task records, task ids, state,
  progress, affected targets, locks, and cancel callbacks;
- expose it from `AlbumBackend`;
- mirror advanced image analysis, semantic generation, and model download
  progress into the registry without changing existing modal behavior.

Files:

- `background_task_controller.hpp/.cpp`
- `album_backend.hpp/.cpp`
- existing analysis, semantic generation, and model download controllers

Verification:

- C++ tests for register/update/finish/cancel lifecycle;
- manual check that current dialogs still behave as before.

### Step 2 - Task Bar and Popover

Scope:

- add `BackgroundTaskBar.qml` and `BackgroundTaskPopover.qml`;
- show running count, primary task, progress, recent failures, and cancel;
- wire cancel from QML back to the owning task callback.

Files:

- `BackgroundTaskBar.qml`
- `BackgroundTaskPopover.qml`
- `Main.qml`

Verification:

- manual check with simultaneous analysis/download/generation;
- cancel from the popover reaches the original controller.

### Step 3 - Interaction Policy and Locks

Scope:

- add `InteractionPolicyController`;
- publish locks from image analysis, semantic generation, model download, and
  model activation;
- maintain `analyzingElementIds`;
- route key QML `enabled` checks through policy results.

Files:

- `interaction_policy_controller.hpp/.cpp`
- `AdvancedContentAnalysisDialog.qml`
- `ImageInspectorPanel.qml`
- `SemanticGenerationSettingsPanel.qml`
- delete/remove entry points in the album UI

Verification:

- affected description/rating/reason controls are disabled;
- deleting or removing analyzed images is blocked;
- editor pipeline controls stay usable.

### Step 4 - DB Write Barrier and Per-Item Result Queue

Scope:

- add a project DB write barrier for export and other DB-stabilizing reads;
- make image analysis results enter a pending commit queue per completed item;
- commit immediately when no barrier is active, otherwise flush after release.

Files:

- image analysis controller/service result sink
- export service/controller integration
- a small DB write barrier helper owned by the UI/app service layer

Verification:

- export does not wait for image analysis;
- no analysis result writes to DB while export holds the barrier;
- completed results appear during long analysis runs when no barrier is active.

### Step 5 - Pipeline Snapshot Rendering for Analysis

Scope:

- add `PipelineMgmtService::LoadPipelineSnapshot(...)` and snapshot release;
- clone pipeline params under the live executor render lock;
- render analysis thumbnails from the independent snapshot executor;
- stop the analysis path from calling `SavePipeline(...)` on live thumbnail
  guards.

Files:

- `pipeline_service.hpp/.cpp`
- `thumbnail_service` or a new analysis rendition provider
- image analysis service/controller call sites

Verification:

- analysis can start while the selected image has unsaved editor changes;
- later editor saves persist normally;
- snapshot rendering does not clear dirty state or overwrite user edits.

### Step 6 - True Background AI Tasks and Shutdown Gate

Scope:

- let advanced image analysis and semantic generation leave their modal running
  states;
- reopen task details from the task popover;
- add a close gate that confirms, cancels, waits, then runs existing save and
  package cleanup;
- keep destructors defensive but move user-visible shutdown to the explicit
  path.

Files:

- `AdvancedContentAnalysisDialog.qml`
- `SemanticGenerationDialog.qml`
- `Main.qml`
- `AlbumBackend` shutdown path

Verification:

- a long analysis can run while browsing/editing;
- quitting with active tasks shows confirmation, cancels, waits, and exits
  cleanly.

### Step 7 - QML UI Schema Foundation

Scope:

- add qrc-backed `default_ui_schema.json`;
- add `UiSchemaStore` and `UiSchemaController` with strict validation;
- add development disk override with qrc fallback;
- build `SchemaPanel.qml` and `SchemaFieldDelegate.qml` for known controls;
- migrate only selected QML main-window settings/task/analysis panels.

Files:

- `ui_schema_store.hpp/.cpp`
- `ui_schema_controller.hpp/.cpp`
- `SchemaPanel.qml`
- `SchemaFieldDelegate.qml`
- `default_ui_schema.json`
- `resource.qrc`

Verification:

- invalid schema falls back to qrc default;
- unknown control/action/capability names are rejected;
- selected QML panels render from schema while Qt Widgets editor layout remains
  unchanged.

### Step 8 - Theme Manifest and Runtime Token Switching

Scope:

- add `theme_manifest.json` and per-theme token JSON files to qrc;
- add `ThemeTokenStore`;
- make `AppTheme.availableThemes` come from the manifest;
- make `AppTheme` expose token-backed Q_PROPERTY values and generated widget
  stylesheets;
- support runtime switching for bundled themes;
- keep user-imported theme files out of first version.

Files:

- `theme_manifest.json`
- `themes/alcedo_dark.json`
- `themes/classic_dark.json`
- `ThemeTokenStore`
- `app_theme.hpp/.cpp`
- `resource.qrc`
- QML components with hard-coded colors/radii touched by the first token pass

Verification:

- duplicate theme ids, missing default theme, invalid token shapes, bad colors,
  and unsupported versions are rejected with qrc fallback;
- switching bundled themes updates QML bindings and reapplies widget
  stylesheets without restart.

## Tests and Manual Checks

Focused C++ tests:

- task registration/update/finish lifecycle;
- cancel callback is called exactly once;
- `InteractionPolicyController` allows unrelated actions and blocks affected
  ones;
- `PipelineMgmtService::LoadPipelineSnapshot(...)` clones live pipeline params
  under the render lock and does not write storage;
- rendering an analysis thumbnail from a snapshot while the same image is dirty
  in the editor does not clear dirty state, call `SavePipeline(...)`, or
  overwrite the user's later save;
- export holds the project DB write barrier, analysis result writes queue while
  it is active, and queued writes flush after export releases it;
- per-item analysis results commit as soon as each item finishes when no DB
  write barrier is active;
- schema validation rejects unknown control types, unknown action ids, unknown
  capability names, and arbitrary expressions;
- theme validation rejects unknown token shapes, raw stylesheet strings, invalid
  colors, negative radii, and unsupported component variants;
- theme manifest validation rejects duplicate ids, missing default theme ids,
  unsupported manifest versions, and theme paths outside allowed qrc/override
  roots;
- unsupported UI schema or theme token versions are rejected without migration
  and fall back to qrc defaults;
- missing or invalid development override files fall back to qrc defaults;
- theme changes update QML-bound tokens through `ThemeChanged`, and widget
  stylesheets are reapplied during the same runtime theme switch;
- shutdown calls cancel and waits for all registered cancel-and-wait tasks.

Manual QML checks:

- start advanced analysis, close dialog, edit a different image;
- verify affected image description/rating fields are disabled;
- cancel image analysis from the task popover;
- start semantic generation and verify model controls are disabled but editor
  remains usable;
- start model download and verify only model download/model activation controls
  are blocked;
- quit while each task type is active.
- switch bundled themes while dialogs, task popovers, and editor widgets are
  visible, and verify QML plus widget styles update without restart.
