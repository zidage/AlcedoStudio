# Welcome Screen Project Overview Plan

Date: 2026-10-01

Status: Phase 1 implemented and tested on Windows (2026-10-01). The two Phase 1
manual app checks and the macOS verification are not run yet. Phase 2 and
Phase 3 planned.

Branch: `feature/welcome-project-overview`, created from
`refact/project-open-speed` at `94d77d092`.

Primary owner: `alcedo_studio/src/ui/alcedo_main`

Direct prerequisites (on the base branch, not yet in `main`):

- `2d89d567b` refact(project-open): one-query folder children load and
  conditional BM25 index work. This makes a background project load short
  enough to run at startup.
- `85d2a3bb2` refact(thumbnail-loading): RAW decode off the render executors
  and a per-project thumbnail disk cache. This makes the three cover
  thumbnails of a loaded project come from disk in most cases.

Related plans:

| Plan | Relation |
| --- | --- |
| [background_tasks_ui_state_plan.md](background_tasks_ui_state_plan.md) | Depends on. `ProjectSwitchBlockReason` and the import/export locks stay the only project-switch admission rule. |
| [qml_editor_rhi_unified_workspace_plan.md](qml_editor_rhi_unified_workspace_plan.md) | Depends on. The Library workspace stays loaded under the welcome surface. This plan does not change the workspace router. |

Design source: the "Alcedo Studio 欢迎界面" Design canvas
(<https://claude.ai/artifact/4f53TGJMUbqA5LHoFVf54g>), artboards
"欢迎界面（有最近项目）" (`Main.dc.html`) and "欢迎界面（无最近项目）"
(`Empty.dc.html`). Section 3 records the approved changes to that design.

## 1. Decision record

### 1.1 Product goal

The welcome screen shows an overview of the most recent project before the
user enters it. Alcedo Studio loads that project at startup in the background,
the same way Lightroom Classic and Capture One open the last catalog. The
welcome screen then shows real numbers and real cover thumbnails from the
loaded project. The user enters the project with one click and does not wait
for a second load.

### 1.2 Approved decisions

1. **Preload the most recent project at startup.** At startup, the app loads
   the first entry of the recent-project list through the normal project load
   path. All project services start, as they do after a normal open. The
   Library workspace under the welcome surface receives the project data.
   The welcome surface stays visible. This plan calls this state
   **preview**: a project is loaded and the user has not entered it.
2. **Reuse the Library path.** The overview uses the loaded project's owners.
   The cover thumbnails are the first three rows of the Library thumbnail
   model in the Library default order. They come through the existing
   `ThumbnailManager` pin and request path. No second, lighter project reader
   is added.
3. **Overview statistics.** The overview shows three values only:
   - **Photos (照片):** the number of photo files in the project.
   - **Edited (已编辑):** the number of photo files that have at least one
     `EditCommit` row in their edit history root, in any Version.
   - **Capture dates (拍摄日期):** the earliest and the latest
     `Image.capture_at` value of the project's photo files.
4. **Removed from the design.** The "版本" (Versions) value and the
   "上次编辑到 … / 12 分钟前" (last edited image) block are not part of the
   product. The photo count column ("286 张") in the "其他最近项目" list is not
   part of the product. The ".alcd 文件拖进这个窗口" drag-and-drop hint is
   removed. The missing-file row with "重新定位…" is not part of the product.
5. **Select another recent project.** A click on a row in "其他最近项目"
   loads that project in preview mode. The upper area ("继续上次的项目")
   shows a skeleton placeholder animation while the load runs. The selected
   project becomes the upper preview. The previously previewed project
   returns to the list.
6. **One load at a time.** While a load runs, the recent-project rows, "打开项目…"
   and "新建项目…" are disabled.
7. **Continue (继续编辑).** "继续编辑" enters the previewed project and shows the
   Library. When the preview load is still running, the click shows the
   existing blocking project-loading ring overlay. The overlay stays until the
   load completes and the Library is visible.
8. **Open project (打开项目…).** The file dialog result loads the project in
   enter mode. The loading ring overlay shows until the Library is visible.
   The project does not go through preview.
9. **New project (新建项目…).** The right column of the welcome surface
   changes to the new-project form (project name and storage folder). The form
   has a back action that restores the overview. Confirm creates the project
   in enter mode with the loading ring overlay, as today.
10. **No save for an unentered project.** A preview project has no user
    changes. Two operations skip the project persist sequence (pipeline
    `Sync`, `SleeveService::Sync`, `ImagePoolService::SyncWithStorage`,
    `SaveProject`, live DB snapshot, `.alcd` repack) for a project that the
    user did not enter:
    - a switch to another project from the welcome surface;
    - app exit from the welcome surface ("退出" or window close).
11. **Preview load failure.** When a preview load fails (missing file,
    unsupported or damaged package, or a load exception), the upper area
    shows the project name and the real error message. "继续编辑" is disabled.
    The app removes the failed entry from the recent-project list. The app
    does not try another project automatically.
12. **No return-to-welcome action.** The welcome surface appears only at
    startup. After the user enters a project, a project switch uses the
    existing open path and the normal persist sequence.
13. **Missing recent files.** The current silent removal at startup stays
    (`LoadRecentProjectsFromSettings`).
14. **Colors.** The design hex values map to existing `AppTheme` tokens
    (section 3.6). The plan adds no color token.
15. **Numeric font.** All numeric values on the welcome surface use Manrope
    (`appTheme.headlineFontFamily`).

### 1.3 Rejected options

- A separate lightweight DuckDB reader for the welcome surface. Rejected:
  decision 2 reuses the full load path, and the full load is required anyway
  when the user continues.
- Statistics stored in the QSettings recent-project entry or read from the
  `.alcd` meta JSON. Rejected with the removal of the list photo count.
- A persisted "last edited image" record. Rejected with the removal of that
  block.

## 2. Terms

- **Preview:** a loaded project with `projectEntered == false`. The welcome
  surface is visible. The user cannot change project data.
- **Enter:** the transition that makes `projectEntered == true`, hides the
  welcome surface, and shows the Library.
- **Entry mode:** the mode of one load request, `kPreview` or `kEnter`.
- **Persist sequence:** the save steps in `ProjectHandler::StartProjectLoad`
  (`project_handler.cpp:113-152`) and in
  `ApplicationModuleHost::ShutdownModules` that write the project back to its
  `.alcd` file.
- **Overview:** the three statistics of decision 3.

## 3. Product design specification

### 3.1 Layout, project available

```text
+--------------------------------------------------------------------------+
| Welcome card (1040 x 600 at 1280 x 800, cardSurfaceColor, radius 10)     |
| +-----------------+  +-----------------------------------------------+   |
| | Alcedo Studio   |  | 继续上次的项目                                  |   |
| | (wordmark)      |  | +-------------------+  +----------------------+ |   |
| | 每个项目是一个…  |  | | cover 1  | cover 2 |  | <project name>       | |   |
| |                 |  | | (2fr,    |---------|  | <project .alcd path> | |   |
| | [打开项目…]      |  | |  2 rows) | cover 3 |  | 照片        已编辑     | |   |
| | [新建项目…]      |  | +-------------------+  | 412         37        | |   |
| |                 |  |                        | 拍摄日期               | |   |
| |                 |  |                        | 2026-08-12 至 08-20    | |   |
| |                 |  |                        | [      继续编辑      ] | |   |
| |                 |  |                        +----------------------+ |   |
| |                 |  | 其他最近项目                                    |   |
| |                 |  | +-------------------------------------------+ |   |
| |                 |  | | <name>                         <relative> | |   |
| |                 |  | | <path>                                    | |   |
| | ─────────────── |  | | ...                                       | |   |
| | [简体中文 v] 退出 |  | +-------------------------------------------+ |   |
| +-----------------+  +-----------------------------------------------+   |
+--------------------------------------------------------------------------+
```

- Left column (260 px): wordmark, one-line description, "打开项目…",
  "新建项目…", divider, language selector, "退出". The drag-and-drop hint is
  removed.
- Right column, upper area "继续上次的项目": a cover block (300 x 224) and an
  information block.
  - Cover block: one large tile (2fr, two rows) and two small tiles (1fr). Gap
    4 px. Outer corners use radius 8. Each tile shows the cover thumbnail
    with `PreserveAspectCrop`.
  - Information block: project name (headline size, one line, elided), the
    `.alcd` path (caption size, muted, one line, elided in the middle),
    statistics grid, and the primary "继续编辑" button at the bottom.
  - Statistics grid: two columns. Row 1: "照片" and "已编辑". Row 2: "拍摄日期"
    spans both columns. Labels use caption size and `textMutedColor`. Values
    use Manrope, weight 600.
- Right column, lower area "其他最近项目": a sunken list well. Each row shows
  the project name, the folder path, and the relative last-opened time. The
  row has no photo count. The list holds all recent entries except the
  previewed project, in `lastOpenedMs` order. The list scrolls when it is
  longer than the well.

### 3.2 Value formats

- Photos and edited: integer with locale group separators
  (`Qt.locale().toString(n)`).
- Capture dates, by case:
  - both dates are on the same day: `YYYY-MM-DD`;
  - same year: `YYYY-MM-DD 至 MM-DD` (English: `YYYY-MM-DD to MM-DD`);
  - different years: `YYYY-MM-DD 至 YYYY-MM-DD`;
  - no photo has a capture date: the text "无拍摄日期" (English:
    "No capture dates") in `textMutedColor`.
- Relative time in the list: reuse `relativeTimeLabel()` from the current
  `WelcomeDialog.qml:165-195`.
- All new user-facing strings use `qsTr` and get manual entries in the
  Simplified Chinese `.ts` file. Do not run `lupdate`.

### 3.3 States of the upper area

| State | Condition | Cover block | Information block | 继续编辑 |
| --- | --- | --- | --- | --- |
| Loading | a preview load runs for `welcomeProjectPath` | skeleton tiles | name and path from the recent entry; skeleton bars for values | enabled; click shows the ring overlay and enters when the load completes |
| Ready | preview loaded, `welcomeProjectPath` equals the loaded path | thumbnails; a tile whose thumbnail is not ready shows a skeleton tile | values | enabled |
| Ready, fewer than 3 photos | `photoCount < 3` | tiles without a photo show a plain `bgBaseColor` tile without animation | values | enabled |
| Failed | the last preview load failed | plain `bgBaseColor` tiles | name from the failed path, error text in `dangerColor` | disabled |

The skeleton placeholder is a rounded rectangle in `bgBaseColor` with a
highlight band (`hoverColor`) that moves from left to right. One cycle takes
1200 ms with `Easing.InOutSine`. Under `reduceMotion` the band does not move
and the tile is a static `bgBaseColor` rectangle. The user requested this
placeholder animation. Phase 2 adds it to the DESIGN.md Motion section as the
one approved perpetual animation for content placeholders.

### 3.4 Layout, no recent project

This follows `Empty.dc.html` without the drag-and-drop hint. The left column
keeps only the wordmark, the description, the language selector and "退出".
The right column is a sunken well with the centered text block "新建第一个项目",
one explanation line, the primary "新建项目…" button, and the secondary
"打开已有项目…" button. No preview load runs.

### 3.5 New-project form

"新建项目…" replaces the right column content with the form. The form keeps
the fields and validation of the current `WelcomeDialog.qml` page 1
(l.690-880): project name field, `FolderPathField` with `FolderDialog`, and the
create action. A back action ("返回") restores the previous right column
content. The preview project stays loaded while the form is visible. Confirm
calls `CreateProjectInFolderNamed` (enter mode).

### 3.6 Color and type mapping

| Design value | Role | `appTheme` token |
| --- | --- | --- |
| `#0B0B0C` | page floor | `bgCanvasColor` |
| `rgba(0,0,0,0.55)` | backdrop dim | existing blur backdrop of `WelcomeDialog` with `overlayColor` |
| `#141415` | welcome card | `cardSurfaceColor` |
| `#0E0E0F` | sunken list well, secondary hover | `bgPanelColor` for the well; `buttonHoveredFillColor` for hover |
| `#2A2A2D` | borders, skeleton base | `cardBorderColor` / `dividerColor` for borders; `bgBaseColor` for tiles |
| `#ECEAE6` | primary text | `textColor` |
| `#8E8C88` | secondary text | `textMutedColor` |
| `#E7E3DB` | primary button fill | `editorListSelectedFillColor` |
| `#0E0E0F` on primary | primary button ink | `editorListSelectedInkColor` |
| `#4C80A8` | "Alcedo" in the wordmark | `accentColor` |
| `#C0707A` | error text | `dangerColor` |

- Wordmark: `headlineFontFamily` (Manrope), as in `CollectionsPanel.qml:185-201`.
- Headings and labels: `uiFontFamily` with the existing `fontSize*` tokens.
- Numeric values (photos, edited, capture dates, relative time): Manrope.
  Do not use `dataFontFamily` (IBM Plex Sans). `app_theme.cpp:355-357` retires it.
- Spacing and radii: only `space*` and radius tokens. No new literal values.

### 3.7 Focus and keyboard

- Initial focus: "继续编辑" when a preview is ready or loading; "新建项目…"
  in the empty state.
- Tab order: left column buttons, "继续编辑", list rows, language selector,
  "退出".
- Enter or Space on a focused list row selects it. Enter on "继续编辑" enters.
- Disabled controls (decision 6 and the failed state) do not take focus.
- Each cover tile has an accessible name: "项目封面". The statistics have
  accessible names that include the label and the value.

### 3.8 Narrow window

The welcome card has the current minimum size behavior of `WelcomeDialog`.
Below a card width of 880 px, the cover block shrinks to 240 px width and the
information block keeps a minimum width of 260 px. The card does not scroll
horizontally.

## 4. Scope

### 4.1 Included work by module

| Module | Responsibility |
| --- | --- |
| `storage/store/sleeve/element_store` | One read query for the overview of the whole project. |
| `app/album_browse_service` | Exposes that query to the UI layer. |
| `ui/alcedo_main/album_backend/project_handler` | Entry mode per load, the entered state, persist skip for an unentered project, overview read on the loader thread. |
| `ui/alcedo_main/album_backend/project_module` | QML surface: preview and enter operations, state properties, failure publication, recent-list update rule. |
| `ui/alcedo_main/album_backend/application_module_host` | Exit persist skip for an unentered project (Phase 1). Runtime workspace removal after the project services close the DuckDB file (Phase 3). |
| `ui/alcedo_main/qml/ProjectLaunchController.qml`, `ShellSignals.qml`, `Main.qml` | Welcome visibility by `projectEntered`, startup preload, continue-while-loading flow. |
| `ui/alcedo_main/qml/WelcomeProjectPreviewAdapter.qml` (proposed) | Non-visual adapter: cover rows from the Library thumbnail model, thumbnail pins, overview and state for the view. |
| `ui/alcedo_main/qml/WelcomeDialog.qml` and new view components | The new visual design. |
| `ui/alcedo_main/DESIGN.md` | Welcome surface section and the skeleton motion rule. |

### 4.2 Exclusions

- No return-to-welcome or close-project action (decision 12).
- No relocate action for missing recent files (decision 13).
- No drag-and-drop open.
- No change to the `.alcd` format, the project meta JSON, or the DuckDB schema.
- No change to the persist sequence for an entered project.
- No change to `automationMode`: in automation mode the welcome surface stays
  hidden and no startup preload runs, as today.

## 5. Current source audit

All paths are under `alcedo_studio/src/` unless stated otherwise. These are
verified source facts at `94d77d092`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Welcome surface | `ui/alcedo_main/qml/WelcomeDialog.qml` (885 lines) | Modal `Dialog` on `Overlay.overlay`, `closePolicy: Popup.NoAutoClose`, blur backdrop (l.197-239). `SwipeView` page 0: title, load, create, recent `ListView` (4 rows, "View All"). Page 1: new-project form (l.690-880). Hardcoded palette (l.32-40). | Replace the page 0 visual with section 3. Move the form into the right column. Use `appTheme` tokens. |
| Welcome instance | `ui/alcedo_main/qml/AppDialogs.qml:331-373` | Binds `recentProjects`, `serviceMessage`, `acceleratorWarning`. Load, create, and row click go through `host.beginProjectLaunch(...)`. | Row click calls `PreviewProject`. Continue calls the new enter flow. |
| Welcome visibility | `ui/alcedo_main/qml/ProjectLaunchController.qml:140-155` | Visible when `!welcomeDismissedForLaunch && !automationMode && !serviceReady && !projectLoading`. | Visible when `!automationMode && !projectEntered`. |
| Startup | `ProjectLaunchController.qml:57-92`, `Main.qml:506-507` | `start()` opens the welcome surface after the shell has a size. No project loads. | After the welcome surface opens, preview the first recent entry when one exists and `acceleratorPreparing` is false. |
| Launch flow | `ProjectLaunchController.qml:37-54, 119-130` | `beginProjectLaunch` sets `projectLaunchPending`, dismisses the welcome surface, and runs the action after 16 ms. | Keep for enter-mode actions. Add the continue flow (section 6.4). |
| Loading ring | `ui/alcedo_main/qml/ProjectLoadingOverlay.qml`, `Main.qml:857-863` | Visible while `projectLaunchPending || projectLoading`. Plays the Library reveal on hide. | Visible only for enter-mode work: `projectLaunchPending || (projectLoading && projectLoadEntryMode == enter)`. A preview load does not show it. |
| Recent list | `ui/alcedo_main/album_backend/project_module.cpp:39-42, 192-208, 736-847` | QSettings `projects/recent`, 12 entries `{path, name, folderPath, lastOpenedMs}`. `RegisterRecentProject` runs after every successful open (`project_handler.cpp:260-263`). | `RegisterRecentProject` runs only on enter. A failed preview removes the entry. |
| Load entry | `project_module.cpp:514-563` (`LoadProject`), `:569-612` (`CreateProjectInFolderNamed`), `:477-486` (`PromptAndLoadProject`) | Rejects a second load while one runs. Pre-checks existence and the magic header, then calls `handler_.OpenPackedProject`. | Add `PreviewProject` and `EnterLoadedProject`. Existing entry points use enter mode. |
| Load worker | `ui/alcedo_main/album_backend/project_handler.cpp:46-277` | UI thread: block check, `FinalizeEditorSession`, loading state, detached `std::thread`. Worker: persist the old project (l.113-152), unpack, `ProjectService`, services. UI thread (queued): swap, `project_opened` hook, `serviceReady`, recent update, `projectChanged`, workspace cleanup. | Pass the entry mode. Skip the old-project persist sequence when the old project is not entered. Read the overview on the worker. Publish entry state and overview with the swap. |
| Exit | `ui/alcedo_main/album_backend/application_module_host.cpp:422-504` | Cancels tasks, finalizes the editor, then `Sync`, `CollectUnreachableEditCommits`, semantic model purge, persist, package, `CleanupWorkspaceDirectory`. | When the project is not entered, skip `Sync`, `CollectUnreachableEditCommits`, the purge, persist and package. Keep task cancel, editor finalize, thumbnail disk-cache metadata flush and workspace cleanup. |
| Photo count | `storage/store/sleeve/element_store.cpp:32-60, 674` | `BuildScopedFileQuery` with folder 0 joins `Element`, `FileImage`, `Image` with `e.type = 0`. `CountFilesInFolder` counts it. | Add one overview query on the same base FROM clause. |
| Edited | `include/storage/store/database.hpp:65-95` | No edited flag. Import writes `PipelineRoot`, `ImageEditState` and a default `VersionRef` with an empty head. It writes no `EditCommit` row (`edit/history/version_ref.cpp:43-66`). `EditCommit.root_id` is indexed. | Count files with `EXISTS (SELECT 1 FROM EditCommit c WHERE c.root_id = s.root_id)` through `ImageEditState s ON s.element_id = e.id`. |
| Capture dates | `database.hpp:38-39`, `storage/mapper/image/image_search_columns.cpp:137-153` | `Image.capture_at TIMESTAMP` from `DateTimeOriginal` (fallback `DateTime`). Unparseable values are NULL. | `MIN(i.capture_at)`, `MAX(i.capture_at)` on the same scoped FROM clause. |
| Library order | `ui/alcedo_main/album_backend/library_module.cpp:240-300`, `folder_controller.cpp:318-331` | `project_opened` runs `ReloadFolderTree` (root folder) and `ReloadCurrentFolder`, which pages `ListFilesInFolderById` into the thumbnail model. | No change. The cover reads rows 0-2 of this model. |
| Cover thumbnails | `ui/alcedo_main/album_backend/thumbnail_manager.cpp:102-160`, `include/ui/alcedo_main/album_backend/album_thumbnail_model.hpp:84-90` | `SetThumbnailVisible` keeps a ref-counted pin per `(element, maxEdge)`. A pin with another `maxEdge` releases the other pins of that element. The model has `getItemsInRange`. The image provider is `image://alcedo-thumb/...`. | The adapter pins rows 0-2 with the grid's `desiredMaxEdge`, so it shares the grid's pins. |
| Fonts | `ui/alcedo_main/app_theme.cpp:149-158, 338-375` | Manrope is bundled (`:/fonts/main_Manrope.ttf`) and is `headlineFontFamily`. | No font registration change. |
| Skeleton | none | No skeleton component exists. DESIGN.md Motion (l.835-857) forbids perpetual animation. | Add `SkeletonBlock.qml` and a documented exception. |
| Tests | `alcedo_studio/tests/ui/album_backend_project_test.cpp` (`AlbumBackendProjectTest`), `tests/ui/application_module_host_shutdown_test.cpp` (`ApplicationModuleHostShutdownTest`), `tests/app/filter_service_test.cpp` (`FilterServiceTest`) | Cover load, create, pack, and data summary. No preview or overview coverage. | Add the tests in sections 9 and 10. |

Inference, not verified: no code path writes the project package while the
welcome surface is visible. The welcome surface is modal. All implicit save
call sites (`image_controller.cpp:517-530`, `folder_controller.cpp:395,455`,
`import_export.cpp:633`, `semantic_generation_controller.cpp:1131`,
`adjustment_transfer_apply_coordinator.cpp:179`) start from a user action in
the Library or the editor. Phase 1 adds a test that proves the package bytes
do not change across a preview switch.

## 6. Target architecture

### 6.1 Owners

**`ElementStore` (storage)**

- Input: none (whole project scope).
- Output: `ProjectOverviewCounts` (section 7.2).
- Reads: `Element`, `FileImage`, `Image`, `ImageEditState`, `EditCommit`.
- Changes: nothing.
- Error surface: throws the DuckDB error as the other `ElementStore` queries do.

**`ProjectHandler` (album backend)**

- Owns: the loaded project and services (unchanged), `project_entered_`,
  `load_entry_mode_` for the running load, `project_overview_`.
- Changes `project_entered_` only on the UI thread: on load completion and in
  `EnterLoadedProject`.
- Reads the overview on the loader worker thread after `ProjectService`
  construction and before the queued UI-thread swap.
- Lifetime of `project_overview_`: set on each successful load completion,
  cleared when a new load starts. It is a point-in-time value for the welcome
  surface. It stays valid while the project is not entered because no user
  edit can run in that state. After enter, nothing reads it.

**`ProjectModule` (QML surface)**

- Input: QML calls `PreviewProject`, `EnterLoadedProject`, and the existing
  `LoadProject`, `PromptAndLoadProject`, `CreateProjectInFolderNamed`.
- Output: properties `projectEntered`, `projectLoadEntryMode`,
  `welcomeProjectPath`, `projectOverview`, `previewErrorMessage`, and the
  existing `recentProjects`, `projectLoading`, `serviceReady`.
- Changes: the QSettings recent list (register on enter, remove on preview
  failure).

**`ApplicationModuleHost`**

- Reads `ProjectHandler::project_entered()` in `ShutdownModules` and skips the
  persist sequence when it is false.

**`WelcomeProjectPreviewAdapter.qml` (proposed)**

- Input: `appModules.project`, `appModules.library.thumbnailModel`, the grid
  `desiredMaxEdge`.
- Output: `state` (`"loading"`, `"ready"`, `"failed"`, `"empty"`), `projectName`,
  `projectPath`, `photoCount`, `editedPhotoCount`, `captureRangeText`,
  `coverItems` (array of up to 3 `{elementId, imageId, thumbUrl, thumbLoading}`),
  `errorText`, `continueEnabled`, `selectionEnabled`.
- Changes: thumbnail pins through `appModules.library.SetThumbnailVisible`.
  It releases each pin it took when the cover row changes, when the adapter is
  disabled, and on `Component.onDestruction`.

### 6.2 Entry state rules

1. A load request has one entry mode. `PreviewProject` uses `kPreview`.
   `LoadProject`, `PromptAndLoadProject` and `CreateProjectInFolderNamed` use
   `kEnter`.
2. On success, `project_entered_ = (load_entry_mode_ == kEnter)`.
3. `EnterLoadedProject()` with a loaded, unentered project and no running load
   sets `project_entered_ = true` and registers the recent entry.
4. `EnterLoadedProject()` during a running `kPreview` load sets
   `load_entry_mode_ = kEnter`. The completion then applies rule 2. Both the
   call and the completion run on the UI thread, so no interleaving exists
   between them. This is a user request, not a consistency mechanism.
5. `EnterLoadedProject()` in any other state returns `false` and changes
   nothing.
6. `PreviewProject(path)` with `path` equal to the loaded, unentered project
   and no running load sets `welcomeProjectPath` and returns `true` without a
   reload.
7. The persist sequence for the old project runs only when the old project is
   entered. The worker captures the old entered value on the UI thread before
   the thread starts.

### 6.3 Primary success call chain: startup preview and continue

```text
Main.qml Component.onCompleted
 -> ProjectLaunchController.start() -> welcome surface opens
 -> recentProjects[0] exists && !acceleratorPreparing
 -> appModules.project.PreviewProject(path)                       [UI thread]
    -> pre-check (exists, magic header) -> welcomeProjectPath = path
    -> ProjectHandler::StartProjectLoad(kPreview)
       -> SetProjectLoadingState(true)                            [UI thread]
       -> worker: old project not entered or none -> skip persist [worker]
          -> unpack -> ProjectService -> services
          -> browse->ReadProjectOverview() -> overview
       -> queued: swap members, project_opened hook               [UI thread]
          (folder tree root, ReloadCurrentFolder -> thumbnail model rows)
          -> project_overview_ = overview, project_entered_ = false
          -> serviceReady = true, projectChanged, loading false
 -> adapter: state "ready", reads rows 0-2, pins thumbnails
 -> user clicks 继续编辑 -> EnterLoadedProject()
    -> project_entered_ = true, RegisterRecentProject, projectEnteredChanged
 -> ProjectLaunchController: welcome closes -> Library reveal
```

### 6.4 Continue while the preview load runs

```text
user clicks 继续编辑 while projectLoading && mode == kPreview
 -> EnterLoadedProject() -> load_entry_mode_ = kEnter -> returns true
 -> projectLoadEntryMode changes -> ProjectLoadingOverlay visible (ring)
 -> load completes -> project_entered_ = true -> RegisterRecentProject
 -> welcome closes -> overlay hides -> Library reveal
```

### 6.5 Primary failure chain: preview load fails

```text
PreviewProject(pathB) while project A is in preview
 -> worker: A not entered -> skip persist -> unpack B throws
 -> queued failure handler                                         [UI thread]
    -> project A stays loaded, project_entered_ stays false
    -> RemoveRecentProject(pathB)
    -> previewErrorMessage = real error text, welcomeProjectPath = pathB
    -> SetProjectLoadingState(false)
 -> adapter: state "failed", 继续编辑 disabled, rows enabled again
 -> user selects A -> PreviewProject(pathA) -> rule 6, no reload
```

When the failed load was switched to enter mode (section 6.4), the same chain
runs. The ring overlay hides, the welcome surface stays, and the upper area
shows the failed state.

### 6.6 Exit from the welcome surface

```text
退出 or window close, project not entered
 -> ApplicationModuleHost::ShutdownModules
    -> cancel tasks, finalize editor (no session), flush thumbnail cache metadata
    -> skip Sync, CollectUnreachableEditCommits, purge, persist, package
    -> CleanupWorkspaceDirectory
```

## 7. File and API map

### 7.1 Current files that change

| File | Phase | Change |
| --- | --- | --- |
| `src/include/storage/store/sleeve/element_store.hpp`, `src/storage/store/sleeve/element_store.cpp` | 1 | `ReadProjectOverview()` |
| `src/include/app/album_browse_service.hpp`, `src/app/album_browse_service.cpp` | 1 | `ReadProjectOverview()` pass-through |
| `src/include/ui/alcedo_main/album_backend/project_handler.hpp`, `src/ui/alcedo_main/album_backend/project_handler.cpp` | 1 | entry mode, entered state, overview, persist skip |
| `src/include/ui/alcedo_main/album_backend/project_module.hpp`, `src/ui/alcedo_main/album_backend/project_module.cpp` | 1 | `PreviewProject`, `EnterLoadedProject`, properties, recent rules |
| `src/ui/alcedo_main/album_backend/application_module_host.cpp` | 1, 3 | Phase 1: exit persist skip. Phase 3: workspace removal after `project_` destruction |
| `src/include/ui/alcedo_main/album_backend/application_module_host.hpp` | 3 | `pending_workspace_removal_` member |
| `src/ui/alcedo_main/qml/ProjectLaunchController.qml`, `ShellSignals.qml`, `Main.qml`, `AppDialogs.qml` | 1 | visibility, startup preview, continue flow, overlay condition |
| `src/ui/alcedo_main/qml/WelcomeDialog.qml` | 1 (minimal), 2 (rewrite) | Phase 1: row click calls `PreviewProject` then `EnterLoadedProject`. Phase 2: new design. |
| `src/ui/alcedo_main/CMakeLists.txt` | 1, 2 | register new QML files in `ALCEDO_MAIN_QML_FILES` |
| `src/ui/alcedo_main/DESIGN.md` | 2 | Welcome section and skeleton motion rule |
| Simplified Chinese `.ts` file used by `alcedo_main` | 2 | manual entries for new strings |
| `alcedo_studio/tests/ui/album_backend_project_test.cpp`, `tests/ui/application_module_host_shutdown_test.cpp` | 1, 3 | new tests |

### 7.2 Proposed APIs

**`struct ProjectOverviewCounts`** (proposed, in `element_store.hpp` next to
the other `ElementStore` result types)

```cpp
struct ProjectOverviewCounts {
  uint64_t                                         photo_count        = 0;
  uint64_t                                         edited_photo_count = 0;
  std::optional<std::chrono::sys_time<std::chrono::microseconds>> earliest_capture_at;
  std::optional<std::chrono::sys_time<std::chrono::microseconds>> latest_capture_at;
};
```

It is a query result, not a copy of an existing object. Use the time type that
`ElementStore` already uses for `capture_at` if one exists; the executor
confirms this in the current header.

**`auto ElementStore::ReadProjectOverview() -> ProjectOverviewCounts`**

- One SQL statement on the `BuildScopedFileQuery` base FROM clause with
  folder 0, plus `LEFT JOIN ImageEditState s ON s.element_id = e.id`:
  `COUNT(*)`,
  `COUNT(*) FILTER (WHERE EXISTS (SELECT 1 FROM EditCommit c WHERE c.root_id = s.root_id))`,
  `MIN(i.capture_at)`, `MAX(i.capture_at)`.
- Does not change state. Throws on a DuckDB error.

**`auto AlbumBrowseService::ReadProjectOverview() -> ProjectOverviewCounts`**

- Pass-through. Does not change state.

**`enum class ProjectEntryMode : uint8_t { kPreview, kEnter };`** (proposed,
`project_handler.hpp`)

**`ProjectHandler`**

- `StartProjectLoad(..., ProjectEntryMode mode)` and
  `OpenPackedProject(..., ProjectEntryMode mode)`: the mode becomes
  `load_entry_mode_`.
- `auto project_entered() const -> bool`.
- `auto load_entry_mode() const -> ProjectEntryMode`.
- `auto project_overview() const -> const std::optional<ProjectOverviewCounts>&`.
- `auto RequestEnterLoadedProject() -> bool`: rules 3-5 of section 6.2.

**`ProjectModule`** (Q_PROPERTY and Q_INVOKABLE)

| Member | Type | Notify | Rule |
| --- | --- | --- | --- |
| `projectEntered` | `bool` | `projectEnteredChanged` | `handler_.project_entered()` |
| `projectLoadEntryMode` | `QString` (`"preview"`, `"enter"`, `""` when idle) | `projectLoadStateChanged` | mode of the running load |
| `welcomeProjectPath` | `QString` | `welcomeProjectChanged` | path that the upper area describes |
| `projectOverview` | `QVariantMap` `{photoCount: int, editedPhotoCount: int, earliestCaptureDate: QDate, latestCaptureDate: QDate}`; empty map when absent; invalid `QDate` when no capture date | `welcomeProjectChanged` | from `project_overview()` |
| `previewErrorMessage` | `QString` | `welcomeProjectChanged` | last preview failure, cleared on the next `PreviewProject` |
| `PreviewProject(QString path)` | `bool` | | returns `false` and changes nothing when a load runs or `ProjectSwitchBlockReason()` is not empty |
| `EnterLoadedProject()` | `bool` | | section 6.2 rules 3-5 |

## 8. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| 1 | Backend and QML glue: overview query, entry mode, preview load, persist skip, welcome visibility by entry state, startup preview, continue flow, preview adapter | storage, app, album_backend, launch QML | base branch | 900-1400 | implemented; manual checks open |
| 2 | Welcome surface UI: new layout, cover, statistics, skeleton, list, empty state, form in the right column, DESIGN.md, translations | `WelcomeDialog.qml` and new QML components, DESIGN.md, `.ts` | Phase 1 | 1200-1800 | planned |
| 3 | Runtime workspace removal at exit: remove the unpacked project workspace after the project services close the DuckDB file | `application_module_host`, shutdown tests | Phase 1 | 80-160 | planned |

Every phase is verified on Windows (`win_debug`) and on macOS
(`macos_debug_tests` for tests, `macos_debug` for the app). A phase is complete
only when both platforms have a recorded result.

Phase 3 does not depend on Phase 2. It can run before or after Phase 2.

Phase 2 replaces most of the 885-line `WelcomeDialog.qml`. Its upper estimate
counts the removed lines. If the estimate passes 2000 lines during
implementation, stop and split the empty state and the form into a separate
phase.

## 9. Detailed phases

### Phase 1 — Preview load and welcome glue

**Objective and deliverables**

- The app loads the most recent project at startup in preview mode.
- The welcome surface stays visible until the user enters a project.
- `ProjectModule` exposes the entry state, the overview, the welcome project
  path and the preview error.
- A switch from an unentered project and an exit from the welcome surface do
  not repack the `.alcd` file.
- `WelcomeProjectPreviewAdapter.qml` gives the Phase 2 view all its data.
- Interim UI: the current `WelcomeDialog` rows call `PreviewProject` and then
  `EnterLoadedProject`. A click on the preloaded project enters at once.

**Inputs and prerequisites**

- Branch `feature/welcome-project-overview` at or after `94d77d092`.
- Read `AGENTS.md` and the skills `alcedo-qml-ui`, `alcedo-msvc-cmake`, and
  `qt-qml` again before you start.

**Modules, files, and APIs**

See section 7. New file (proposed):
`src/ui/alcedo_main/qml/WelcomeProjectPreviewAdapter.qml`.

**Data rules and invariants**

- `project_entered_`, `load_entry_mode_` and `project_overview_` change only on
  the UI thread.
- The worker receives the old project's entered value by value at thread start.
- `RegisterRecentProject` runs only when `project_entered_` becomes `true`.
- A preview failure removes exactly one recent entry: the failed path.
- An enter-mode failure from `LoadProject` keeps the current behavior
  (pre-check failures remove the entry, worker failures set
  `serviceMessage`).
- `ProjectLoadingOverlay` never shows for a `kPreview` load.
- The adapter takes at most three pins and releases every pin it took.
- No new fallback: when the overview query throws, the load fails with that
  error. Do not publish a partial overview.

**Implementation steps**

1. Add `ProjectOverviewCounts` and `ElementStore::ReadProjectOverview()`.
   Build the SQL from `BuildScopedFileQuery` with folder 0 so the scope equals
   `CountFilesInFolder(0)`. Map NULL `MIN`/`MAX` to `std::nullopt`.
2. Add `AlbumBrowseService::ReadProjectOverview()`.
3. Add `ProjectEntryMode` and the three `ProjectHandler` members. Add the mode
   parameter to `StartProjectLoad` and `OpenPackedProject`. Update all callers.
   The create path passes `kEnter`.
4. In `StartProjectLoad`, capture `old_project_entered` on the UI thread. In
   the worker, run the persist block (`project_handler.cpp:113-152`) only when
   `old_project_entered` is `true`. Keep
   `old_thumbnail->FlushDiskCacheMetadata()` unconditional. It writes the
   thumbnail cache index, not the project package.
5. In the worker, after service construction, call
   `ReadProjectOverview()` and move the result into the queued lambda.
6. In the queued success lambda, set `project_overview_` and
   `project_entered_` before `HandleProjectOpened()` returns to QML. Move
   `RegisterRecentProject` behind `project_entered_`. Emit
   `projectEnteredChanged` and `welcomeProjectChanged` after
   `projectChanged`.
7. In the queued failure lambda for `kPreview`, call `RemoveRecentProject` and
   set `previewErrorMessage`. Do not change the old project.
8. Add `RequestEnterLoadedProject()` and `ProjectModule::EnterLoadedProject()`.
9. Add `ProjectModule::PreviewProject()`. Reuse the `LoadProject` pre-checks.
   A pre-check failure in preview mode follows step 7.
10. In `ApplicationModuleHost::ShutdownModules`, skip `Sync`,
    `CollectUnreachableEditCommits`, the semantic model purge,
    `PersistCurrentProjectState` and `PackageCurrentProjectFiles` when
    `project_entered()` is `false`. Keep the other steps.
11. QML: `ProjectLaunchController.updateWelcomeDialogVisibility()` uses
    `!automationMode && !appModules.project.projectEntered`. Remove
    `welcomeDismissedForLaunch` usage where the new condition covers it.
12. QML: `ProjectLaunchController.start()` calls `PreviewProject(recent[0].path)`
    after the welcome surface opens. Wait for `acceleratorPreparing` to become
    `false` through `ShellSignals` before the call.
13. QML: add `continueWelcomeProject()` to `ProjectLaunchController`. It calls
    `EnterLoadedProject()`. When the load is still running, the overlay shows
    because `projectLoadEntryMode` is `"enter"`.
14. QML: change the `ProjectLoadingOverlay` visibility to the section 5 rule.
15. QML: add `WelcomeProjectPreviewAdapter.qml`. Read rows with
    `thumbnailModel.getItemsInRange(0, 2)`. Refresh on the model's
    `modelReset`, `rowsInserted`, `rowsRemoved` and `dataChanged` for rows 0-2.
    Pin with the Library grid `desiredMaxEdge`; expose that value from the
    Library workspace to the adapter through the existing `Main.qml` host
    aliases. Format `captureRangeText` by section 3.2.
16. QML: interim `WelcomeDialog` row click calls `PreviewProject` and, on
    `true`, `continueWelcomeProject()`.
17. Register the new QML file in `src/ui/alcedo_main/CMakeLists.txt`.

**Primary success call chain**

Section 6.3 and section 6.4.

**Primary failure and restore call chain**

Section 6.5. The old project stays loaded and unentered. Its `.alcd` file is
unchanged. The error text is the real exception message.

**Tests and evidence**

`AlbumBackendProjectTest` (existing file, new cases):

- `PreviewProjectLoadsWithoutEnteringAndKeepsRecentOrder`: create two packed
  projects, register both, preview the older one. Assert `projectEntered` is
  `false`, `serviceReady` is `true`, and the `recentProjects` order and
  `lastOpenedMs` values are unchanged.
- `EnterLoadedProjectRegistersRecentEntry`: after preview, call
  `EnterLoadedProject`. Assert `projectEntered` is `true` and the project is
  first in `recentProjects` with a new `lastOpenedMs`.
- `EnterDuringPreviewLoadEntersOnCompletion`: call `PreviewProject`, then
  `EnterLoadedProject` before completion. Assert the call returns `true` and
  `projectEntered` is `true` after completion.
- `PreviewSwitchLeavesUnenteredPackageBytesUnchanged`: preview A, then
  preview B. Assert A's `.alcd` XXH3 and size are unchanged.
- `EnteredProjectSwitchRepacksPreviousPackage`: enter A, then load B. Assert
  the A package was rewritten (modification time changed). This proves the
  skip is scoped.
- `PreviewFailureRemovesRecentEntryAndKeepsLoadedProject`: preview A, then
  preview a corrupt package C. Assert A stays loaded, C is absent from
  `recentProjects`, and `previewErrorMessage` is not empty.
- `PreviewOfLoadedProjectDoesNotReload`: preview A twice. Assert no second
  `projectLoadStateChanged(true)` signal.
- `SecondPreviewWhileLoadingIsRejected`: assert `PreviewProject` returns
  `false` during a running load.
- `PreviewOverviewCountsPhotosEditedAndCaptureRange`: build a project with
  three imported photos, add one `EditCommit` to one photo's root, set
  `capture_at` on two photos and NULL on one. Assert `photoCount == 3`,
  `editedPhotoCount == 1`, and the earliest and latest dates.
- `PreviewOverviewOfEmptyProjectHasNoCaptureRange`: assert zero counts and
  invalid dates.
- `PreviewPopulatesFirstThumbnailRowsInLibraryOrder`: assert the Library
  thumbnail model rows 0-2 equal the first three files of
  `ListFilesInFolderById(root, 0, 3)`.

`ApplicationModuleHostShutdownTest` (existing file, new cases):

- `ShutdownWithUnenteredProjectSkipsRepack`: preview a project, shut down.
  Assert the `.alcd` XXH3 is unchanged and the workspace directory is removed.
- `ShutdownWithEnteredProjectRepacks`: enter a project, shut down. Assert the
  package was rewritten.

Overview query edge (in `AlbumBackendProjectTest` or `FilterServiceTest`,
where the fixture can insert rows directly):

- `ProjectOverviewExcludesImagesWithoutFileBinding`: an `Image` row without a
  `FileImage` row does not count.

QML: no new offscreen QML test. `AGENTS.md` marks those suites unreliable.
Record the manual checks below.

Manual checks:

- Start with two recent projects. The Library under the welcome surface
  holds the first project. A click on the first row enters without the ring.
- Start, then click a row of a large project. The ring does not show during
  the preview load.

**Build and run commands**

Run through the PowerShell tool:

```text
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlbumBackendProjectTest ApplicationModuleHostShutdownTest alcedo_main
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "AlbumBackendProjectTest|ApplicationModuleHostShutdownTest" --output-on-failure
```

Put logs in `build/tmp/welcome_overview_phase1/`. Confirm with
`ctest -N -R ...` that the new tests are discovered before you report a pass.
Do not run the full suite.

**Exit criteria**

- [x] All Phase 1 tests are discovered and pass.
- [x] `PreviewSwitchLeavesUnenteredPackageBytesUnchanged` and
      `ShutdownWithUnenteredProjectSkipsRepack` pass.
- [x] `alcedo_main` builds.
- [ ] The two manual checks are recorded.
- [x] The terminology check finds no prohibited term in changed files.
- [ ] macOS: `AlbumBackendProjectTest` and `ApplicationModuleHostShutdownTest`
      pass in `macos_debug_tests`, `alcedo_main` builds in `macos_debug`, and
      the two manual checks are recorded on macOS.

**Expected diff**

900-1400 lines: storage and service 80-120, handler and module 250-400,
host 30-60, QML glue and adapter 250-400, tests 300-450.

**Completion record**

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

##### Phase 1 completion record (2026-10-01)

**Status:** partial. The implementation and all automated Phase 1 tests are
complete on Windows. The two manual app checks are not run: this session cannot
drive the desktop window of `alcedo_main`. macOS is not built or tested.

**Source revision and branch:** `feature/welcome-project-overview` at
`94d77d092`, uncommitted working tree.

**Actual changed modules:**

| Module | Files |
| --- | --- |
| storage | `include/storage/store/sleeve/element_store.hpp`, `storage/store/sleeve/element_store.cpp`: `ProjectOverviewCounts`, `ElementStore::ReadProjectOverview()` |
| app | `include/app/album_browse_service.hpp`, `app/album_browse_service.cpp`: `ReadProjectOverview()` pass-through that throws |
| album_backend | `project_handler.{hpp,cpp}`: `ProjectEntryMode`, `project_entered_`, `load_entry_mode_`, `project_overview_`, `recent_project_path_`, `RequestEnterLoadedProject()`, persist skip, overview read on the loader thread. `project_module.{hpp,cpp}`: five Q_PROPERTYs, `PreviewProject`, `EnterLoadedProject`, `HandlePreviewLoadFailed`, `StartPackedProjectLoad` (pre-checks shared with `LoadProject`). `application_module_host.cpp`: exit persist skip. `album_thumbnail_model.{hpp,cpp}`: `getThumbnailStatesInRange` |
| QML | `ProjectLaunchController.qml`, `ShellSignals.qml`, `Main.qml`, `AppDialogs.qml`, `LibraryWorkspace.qml` (`thumbnailMaxEdge`), new `WelcomeProjectPreviewAdapter.qml`, `CMakeLists.txt` registration |
| tests | `tests/ui/album_backend_project_test.cpp`, `tests/ui/application_module_host_shutdown_test.cpp`, new `tests/ui/welcome_project_test_support.hpp` |

**Implemented behavior:** sections 6.2 to 6.6. The startup preview of
`recentProjects[0]` waits for an open welcome surface and for
`acceleratorPreparing == false`. The welcome surface is visible while
`!automationMode && !projectEntered && !welcomeDismissedForLaunch`. The ring is
visible for `projectLaunchPending || (projectLoading && projectLoadEntryMode == "enter")`.
The interim `WelcomeDialog` row calls `PreviewProject` and then
`continueWelcomeProject()`.

**Deviations from section 7 (mapping):**

- Capture dates: `ElementStore` already reads capture dates as `YYYY-MM-DD`
  text (`SearchResultRow::capture_date_`). `ProjectOverviewCounts` therefore
  has `std::optional<std::string> earliest_capture_date_` /
  `latest_capture_date_` from `MIN/MAX(i.capture_date)` in place of
  `sys_time` values. `ProjectModule::projectOverview` converts them to `QDate`
  as specified.
- Edited count: `COUNT(*) FILTER (WHERE EXISTS (SELECT 1 FROM ImageEditState s
  JOIN EditCommit c ON c.root_id = s.root_id WHERE s.element_id = e.id))`.
  This has the same result as the `LEFT JOIN` form and keeps the
  `BuildScopedFileQuery(0)` FROM clause unchanged.
- `project_overview_` is not cleared at load start. It is replaced only by a
  successful load. A failed preview keeps the loaded project, so its overview
  stays correct for the rule 6 return to that project.
- Signal names follow the `ProjectModule` style: `ProjectEnteredChanged`,
  `WelcomeProjectChanged`. `projectLoadEntryMode` uses the existing
  `ProjectLoadStateChanged`.
- Cover rows: `getItemsInRange` does not carry `thumbUrl` / `thumbLoading`, so
  the model has a new `getThumbnailStatesInRange(first, last)` that returns
  `{elementId, imageId, thumbUrl, thumbLoading}`. The selection payload of
  `getItemsInRange` is unchanged.
- `welcomeDismissedForLaunch` stays for enter-mode launches (open, create,
  continue during load): the welcome popup is above the ring overlay, so it
  must close while the ring shows. `onProjectLoadStateChanged` resets it when
  the load ends, so a failed enter-mode load shows the welcome surface again
  when no project is entered.
- `PreviewProject` treats a workspace creation failure as a preview error but
  does not remove the recent entry, because the file is not the cause. This is
  the same rule that `LoadProject` uses.
- `ProjectHandler::InitializeServices` lost its default arguments; its only
  caller passes all of them.
- The tests compare the full package bytes and the modification time (the
  test moves the time one hour back first) in place of an XXH3 value.

**Primary success call chain (startup preview, continue):**

```text
Main.qml Component.onCompleted -> ProjectLaunchController.start()
 -> openWelcomeAfterShellReady() -> welcome open -> requestStartupPreview()
    (again from ShellSignals.onAcceleratorPreparationStateChanged)
 -> ProjectModule::PreviewProject(path)                          [UI thread]
    -> StartPackedProjectLoad (exists, magic header, workspace)
    -> ProjectHandler::OpenPackedProject(kPreview)
       -> StartProjectLoad: load_entry_mode_ = kPreview, old_project_entered captured
       -> worker: skip old pipeline Sync + persist + pack when not entered
          -> unpack -> ProjectService -> services
          -> AlbumBrowseService::ReadProjectOverview -> ElementStore::ReadProjectOverview
       -> queued success: swap, recent_project_path_, project_overview_,
          project_entered_ = (load_entry_mode_ == kEnter) -> project_opened hook
          (folder tree, thumbnail model rows) -> ProjectChanged
          -> NotifyProjectEntryStateChanged -> loading false
 -> WelcomeProjectPreviewAdapter: state "ready", rows 0-2, pins at grid tier
 -> continueWelcomeProject() -> EnterLoadedProject -> RequestEnterLoadedProject
    -> project_entered_ = true -> RegisterRecentProject -> ProjectEnteredChanged
 -> ShellSignals.onProjectEnteredChanged -> welcome closes -> revealLibraryAfterProjectLoad
```

Continue during the preview load: `RequestEnterLoadedProject` sets
`load_entry_mode_ = kEnter` and emits `ProjectLoadStateChanged` ->
`projectLoadEntryMode == "enter"` -> ring visible, welcome dismissed -> the
completion sets `project_entered_ = true` and registers the recent entry.

**Primary failure and restore call chain:**

```text
PreviewProject(pathC) while project A is previewed
 -> worker: A not entered -> skip persist -> UnpackProjectToWorkspace throws (checksum)
 -> CleanupWorkspaceDirectory(workspace of C)
 -> queued failure [UI thread]: A stays loaded, project_entered_ stays false
    -> serviceMessage "Requested project failed to open: ..."
    -> started_as_preview -> HandlePreviewLoadFailed(pathC, real error)
       -> RemoveRecentProject(pathC), welcomeProjectPath = C, previewErrorMessage set
 -> adapter state "failed", continueEnabled false, selectionEnabled true
 -> PreviewProject(pathA) -> rule 6: no reload, welcomeProjectPath = A, error cleared
Pre-check failure (missing file or bad header) -> HandlePreviewLoadFailed without a load.
Exit while not entered -> ShutdownModules: cancel tasks, finalize editor,
  FlushDiskCacheMetadata, skip Sync / CollectUnreachableEditCommits / purge /
  persist / pack -> CleanupWorkspaceDirectory.
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `PreviewProjectLoadsWithoutEnteringAndKeepsRecentOrder` | `AlbumBackendProjectTest` | PASS |
| `EnterLoadedProjectRegistersRecentEntry` | `AlbumBackendProjectTest` | PASS |
| `EnterDuringPreviewLoadEntersOnCompletion` | `AlbumBackendProjectTest` | PASS |
| `PreviewSwitchLeavesUnenteredPackageBytesUnchanged` | `AlbumBackendProjectTest` | PASS |
| `EnteredProjectSwitchRepacksPreviousPackage` | `AlbumBackendProjectTest` | PASS |
| `PreviewFailureRemovesRecentEntryAndKeepsLoadedProject` | `AlbumBackendProjectTest` | PASS |
| `PreviewOfMissingFileRemovesRecentEntryWithoutLoad` (added: pre-check failure path of step 9) | `AlbumBackendProjectTest` | PASS |
| `PreviewOfLoadedProjectDoesNotReload` | `AlbumBackendProjectTest` | PASS |
| `SecondPreviewWhileLoadingIsRejected` | `AlbumBackendProjectTest` | PASS |
| `PreviewOverviewCountsPhotosEditedAndCaptureRange` | `AlbumBackendProjectTest` | PASS |
| `PreviewOverviewOfEmptyProjectHasNoCaptureRange` | `AlbumBackendProjectTest` | PASS |
| `ProjectOverviewExcludesImagesWithoutFileBinding` | `AlbumBackendProjectTest` | PASS |
| `PreviewPopulatesFirstThumbnailRowsInLibraryOrder` | `AlbumBackendProjectTest` | PASS |
| `ShutdownWithUnenteredProjectSkipsRepack` | `ApplicationModuleHostShutdownTest` | PASS (package checks only; see remaining defects) |
| `ShutdownWithEnteredProjectRepacks` | `ApplicationModuleHostShutdownTest` | PASS |
| Existing cases of both suites | both | PASS (3 `LoadProject_ExternalPackedProjectFromEnv_*` SKIP without their environment variable, as before) |
| Changed QML loads without new warnings | `MainQmlWorkflowTest` (not a Phase 1 target) | 2/3 PASS; `ProductionWindowLoadsAndRoutesCoreWorkspaceActions` fails on clean `94d77d092` too, with the same only warning (`AppDialogs.qml` `onPasteFinished` Connections). A dump of all warnings with the Phase 1 change showed only that warning. |

**Build and test commands with exit codes** (PowerShell tool, repository root):

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlbumBackendProjectTest ApplicationModuleHostShutdownTest alcedo_main   -> 0
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "WelcomePreviewTests|ShutdownWith"   -> 15 Phase 1 tests listed
ctest --test-dir build/debug -R "AlbumBackendProjectTest|ApplicationModuleHostShutdownTest" --output-on-failure   -> 0
qmllint (Qt 6.9.3) on the changed QML files -> no syntax or type errors; only the
  existing unqualified `appModules` context-property warnings and Loader.item member warnings
```

**Discovered / passed / failed / skipped counts:** 43 discovered, 40 passed,
0 failed, 3 skipped (existing cases that need an environment variable). The 15 Phase 1 cases all pass.

**Manual verification:** not run (no desktop control in this session). Open:

- Start with two recent projects. The Library under the welcome surface holds
  the first project. A click on the first row enters without the ring.
- Start, then click a row of a large project. The ring does not show during
  the preview load.

**Evidence path:** `build/tmp/welcome_overview_phase1/` (build and ctest
logs). Remove it when the manual checks are recorded.

**LOC note:** diff 21 files, +1231 / -64 (inside the 900-1400 estimate).
`tests/ui/album_backend_project_test.cpp` is now 1078 lines; the welcome
preview cases (about 350 lines) are the split candidate for a
`WelcomeProjectPreviewTest` target when the file grows again.
`project_module.cpp` is 953 lines.

**Remaining defects or unavailable platforms:**

- Existing defect, not caused by Phase 1: on Windows
  `ShutdownModules` cannot remove the runtime workspace. `CleanupWorkspaceDirectory`
  runs while `ProjectService` still holds the DuckDB file, and `remove_all`
  stops at the first locked file. A probe listed `<name>.db`, `<name>.db.wal`
  and `<name>.json` after `Shutdown()` for entered and unentered projects.
  The two shutdown tests therefore do not assert the workspace removal that
  section 9 lists. Phase 3 fixes this defect and adds the removal assertions.
- macOS was not built or tested. The macOS exit criterion of Phase 1 is open.

### Phase 2 — Welcome surface UI

**Objective and deliverables**

- `WelcomeDialog.qml` renders section 3: project-available layout, empty
  layout, upper-area states, skeleton placeholders, recent list, and the
  new-project form in the right column.
- All colors, spaces, radii and type come from `appTheme` (section 3.6).
- Numeric values use Manrope.
- DESIGN.md records the welcome surface and the skeleton motion rule.
- Simplified Chinese translations exist for all new strings.

**Inputs and prerequisites**

- Phase 1 complete.
- Read `AGENTS.md`, `src/ui/alcedo_main/DESIGN.md`, and the skills
  `alcedo-qml-ui` and `qt-qml` again.

**Modules, files, and APIs**

- `src/ui/alcedo_main/qml/WelcomeDialog.qml`: shell, backdrop, left column,
  right column state switch (overview, form, empty).
- Proposed components in `src/ui/alcedo_main/qml/`:
  - `WelcomeProjectOverview.qml`: upper area, binds only to the adapter.
  - `WelcomeCoverMosaic.qml`: three tiles, skeleton and empty tiles.
  - `WelcomeRecentProjectList.qml`: list well and rows.
  - `WelcomeNewProjectForm.qml`: the form moved from page 1.
  - `SkeletonBlock.qml`: placeholder rectangle with the moving band.
- `AppDialogs.qml`: bind the new signals (`previewRequested(path)`,
  `continueRequested()`, `openRequested()`, `createRequested(folder, name)`).
- `src/ui/alcedo_main/CMakeLists.txt`: register the new files.
- `src/ui/alcedo_main/DESIGN.md`: a "Welcome surface" section with the token
  map of section 3.6, and a Motion table row for `SkeletonBlock`.
- The Simplified Chinese `.ts` file: manual entries.

**Data rules and invariants**

- The view reads data only from `WelcomeProjectPreviewAdapter` and
  `appModules.project.recentProjects`. It does not call `ProjectHandler`
  operations directly.
- The recent list excludes `welcomeProjectPath`.
- Rows and left-column actions bind `enabled` to `adapter.selectionEnabled`.
- No pill, badge, status dot, or `xx · xx` compound label (`alcedo-qml-ui`
  hard bans).
- Production style stays Basic. Do not import Material.
- `SkeletonBlock` animates only `x` of the band inside a clipped rectangle. It
  stops when invisible and under `reduceMotion`.

**Implementation steps**

1. Add `SkeletonBlock.qml`.
2. Add `WelcomeCoverMosaic.qml` with the section 3.3 tile rules.
3. Add `WelcomeProjectOverview.qml` with the information block and the four
   states.
4. Add `WelcomeRecentProjectList.qml`. Reuse `relativeTimeLabel()`.
5. Move the page 1 form into `WelcomeNewProjectForm.qml`. Keep its validation
   and its `FolderDialog`. Add the back action.
6. Rewrite `WelcomeDialog.qml`: remove the `SwipeView`, the hardcoded palette
   and the "View All" toggle. Build the two-column card. Keep the CUDA
   warning dialog and the update-check timer unchanged.
7. Connect the signals in `AppDialogs.qml` to `PreviewProject`,
   `continueWelcomeProject()`, `beginProjectLaunch(PromptAndLoadProject)` and
   `beginProjectLaunch(CreateProjectInFolderNamed)`.
8. Apply the section 3.7 focus order and accessible names.
9. Update DESIGN.md.
10. Add the translations by hand. Do not run `lupdate`.

**Primary success call chain**

```text
adapter.state "loading" -> skeleton tiles and bars
 -> projectChanged -> adapter.state "ready" -> values in Manrope
 -> thumbUrl for rows 0-2 -> tiles show images
 -> 继续编辑 -> continueWelcomeProject() -> welcome closes -> Library reveal
```

**Primary failure and restore call chain**

```text
previewErrorMessage set -> adapter.state "failed"
 -> name + dangerColor error text, 继续编辑 disabled
 -> list rows enabled -> user selects another project -> state "loading"
```

**Tests and evidence**

- No offscreen QML test (see `AGENTS.md`). Behavior that Phase 2 depends on
  is covered by the Phase 1 C++ tests.
- Run `qmllint` on the new and changed QML files when the build provides it.
  Record the result.
- Manual checks with screenshots in `build/tmp/welcome_overview_phase2/`:
  1. Project available, ready state, at 1280 x 800, in Simplified Chinese and
     English.
  2. Loading state after a row click (skeleton visible).
  3. `reduceMotion` on: skeleton is static.
  4. Failed state with a corrupt package.
  5. Empty state with no recent project.
  6. New-project form, back action, and create.
  7. Continue during loading shows the ring, then the Library.
  8. Keyboard: Tab order and Enter on a row and on 继续编辑.
  9. Narrow window: the card does not scroll horizontally.

**Build and run commands**

```text
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
```

**Exit criteria**

- [ ] `alcedo_main` builds and starts.
- [ ] All nine manual checks are recorded with screenshots.
- [ ] No literal color, space or font size in the new QML. A search for `#`
      color literals in the changed files finds none.
- [ ] DESIGN.md and the `.ts` file are updated in the same change.
- [ ] The terminology check finds no prohibited term in changed files.
- [ ] macOS: `alcedo_main` builds in `macos_debug`, and the nine manual checks
      are recorded on macOS with screenshots.

**Expected diff**

1200-1800 lines: new components 600-900, `WelcomeDialog.qml` rewrite
(net, counting removed lines) 400-650, DESIGN.md 60-100, translations
80-150.

**Completion record**

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

### Phase 3 — Runtime workspace removal at exit

**Objective and deliverables**

- At app exit, the app removes the runtime workspace of the loaded project
  (`<temp>/alcedo_main/runtime_<name>_<ids>/` with `<name>.db`,
  `<name>.db.wal` and `<name>.json`). This applies to an entered project and
  to an unentered (preview) project.
- A removal failure is reported with the path and the operating system error.
  It is not ignored.
- The persist sequence of an entered project does not change.
- The behavior is verified on Windows and on macOS.

**Defect record (verified source facts at `94d77d092` plus Phase 1)**

- `main.cpp` keeps `ApplicationModuleHost app_modules` on the stack. It calls
  `app_modules.Shutdown()` (l.386) and the destructor runs at the end of
  `main`.
- `ApplicationModuleHost::ShutdownModules` calls
  `album_util::CleanupWorkspaceDirectory(project_->handler().workspace_dir())`
  as its last step. At that time `ProjectHandler` still owns the
  `ProjectService`, so the DuckDB database file is open.
- `CleanupWorkspaceDirectory` calls `std::filesystem::remove_all(dir, ec)` and
  discards `ec`. On Windows an open file cannot be deleted. `remove_all` stops
  at the first failure, so no file of the workspace is removed.
- `~ApplicationModuleHost` destroys `project_` (`ProjectModule`, which owns
  `ProjectHandler` and the `ProjectService`) after `library_`, `folders_`,
  `images_`, `stats_`, `search_`, `import_export_` and the editor session
  owners. The modules destroyed after `project_` (`lut_packages_`,
  `lut_library_`, `updates_`, `model_download_service_`, `download_service_`,
  `interaction_policy_`, `background_tasks_`) do not hold a project service.
- Phase 1 evidence: a probe in `ShutdownWithUnenteredProjectSkipsRepack`
  listed all three workspace files after `Shutdown()`. The entered path showed
  the same result.

Inference, not verified: on macOS, `unlink` removes a directory entry of an
open file, so the exit leak probably does not occur there. The Phase 3 tests
must still run on macOS. They prove that removal after the database closes is
correct on both platforms, and that DuckDB closes without an error after the
removal.

**Inputs and prerequisites**

- Phase 1 complete on the branch.
- Read `AGENTS.md` and the skills `alcedo-msvc-cmake` and `qt-cmake-project`
  again before you start.

**Modules, files, and APIs**

- `src/include/ui/alcedo_main/album_backend/application_module_host.hpp`: add
  `std::filesystem::path pending_workspace_removal_{}`. It holds the workspace
  path that the destructor removes. It is a path value, not a copy of project
  state.
- `src/ui/alcedo_main/album_backend/application_module_host.cpp`:
  - `ShutdownModules`: replace the two `CleanupWorkspaceDirectory` calls (the
    entered branch and the unentered branch) with
    `pending_workspace_removal_ = project_->handler().workspace_dir()`.
  - `~ApplicationModuleHost`: after `destroy(project_, "ProjectModule")`, call
    `RemovePendingWorkspace()`.
  - New private `void RemovePendingWorkspace()`: calls
    `std::filesystem::remove_all(pending_workspace_removal_, ec)`. When `ec` is
    set, it writes `qWarning` with the path and `ec.message()`. It clears
    `pending_workspace_removal_`.
- `alcedo_studio/tests/ui/application_module_host_shutdown_test.cpp`: new cases.

**Data rules and invariants**

- The workspace removal runs after the last owner of the `ProjectService` is
  destroyed. No project module reads the workspace after the removal.
- `ShutdownModules` keeps all other steps and their order. The persist and
  pack steps of an entered project run before the removal, as today.
- `Shutdown()` can run more than one time. The removal runs one time, in the
  destructor.
- The project switch cleanup in `ProjectHandler::StartProjectLoad` (the old
  workspace after the swap) and the loader failure cleanup do not change in
  this phase. Phase 3 records their behavior through
  `ProjectSwitchRemovesPreviousWorkspace` (see the tests). If that test fails,
  stop and report the result as a separate defect.
- No fallback: do not retry the removal and do not schedule a removal at the
  next start.

**Implementation steps**

1. Add `pending_workspace_removal_` and `RemovePendingWorkspace()` to
   `ApplicationModuleHost`.
2. In `ShutdownModules`, set `pending_workspace_removal_` in place of the two
   `CleanupWorkspaceDirectory` calls.
3. In `~ApplicationModuleHost`, call `RemovePendingWorkspace()` directly after
   `destroy(project_, "ProjectModule")`.
4. Change the comment in `ShutdownModules` that describes the exit steps so it
   names the destructor as the place of the workspace removal.
5. Add the tests below. Put each host in its own scope so the destructor runs
   before the assertions.
6. Build and run on Windows. Build and run on macOS.

**Primary success call chain**

```text
退出 or window close -> main.cpp app_modules.Shutdown()
 -> ApplicationModuleHost::ShutdownModules
    -> cancel tasks, finalize editor, (entered: Sync, persist, pack)
    -> pending_workspace_removal_ = workspace_dir()
 -> end of main -> ~ApplicationModuleHost
    -> destroy modules ... destroy(project_) -> ProjectService closes DuckDB
    -> RemovePendingWorkspace() -> remove_all(workspace) -> directory gone
```

**Primary failure and restore call chain**

```text
remove_all fails (for example, another process holds a workspace file)
 -> RemovePendingWorkspace writes qWarning(path, ec.message())
 -> pending_workspace_removal_ cleared -> process exit continues
 -> the project package is already written (entered) or unchanged (unentered)
```

**Tests and evidence**

`ApplicationModuleHostShutdownTest` (existing file, new cases):

- `ShutdownRemovesUnenteredProjectWorkspace`: preview a packed project. Keep
  the workspace path. Destroy the host. Assert the workspace directory does not
  exist and the package bytes and modification time are unchanged.
- `ShutdownRemovesEnteredProjectWorkspace`: preview and enter a packed
  project. Keep the workspace path. Destroy the host. Assert the workspace
  directory does not exist and the package modification time changed.
- `ShutdownWithoutProjectRemovesNothing`: create a host without a project and
  destroy it. Assert no exception and no `qWarning` about a workspace removal.
- `RepeatedShutdownRemovesWorkspaceOnce`: call `Shutdown()` two times, then
  destroy the host. Assert the workspace directory does not exist and the
  package bytes are unchanged for an unentered project.

`AlbumBackendProjectTest` (existing file, new case):

- `ProjectSwitchRemovesPreviousWorkspace`: preview project A, keep its
  workspace path, preview project B. Assert the workspace of A does not exist
  after the load of B completes. This records the current switch behavior on
  both platforms.

Use the builders in `tests/ui/welcome_project_test_support.hpp`. Keep the
Phase 1 cases `ShutdownWithUnenteredProjectSkipsRepack` and
`ShutdownWithEnteredProjectRepacks` unchanged.

Manual checks (Windows and macOS):

- Start the app, let the startup preview load, and quit from the welcome
  surface. The `alcedo_main/runtime_*` directory of that project is gone.
- Start the app, enter a project, quit. The runtime directory is gone and the
  `.alcd` package has a new modification time.

**Build and run commands**

Windows, through the PowerShell tool:

```text
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlbumBackendProjectTest ApplicationModuleHostShutdownTest alcedo_main
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "AlbumBackendProjectTest|ApplicationModuleHostShutdownTest" --output-on-failure
```

macOS:

```text
cmake --preset macos_debug_tests
cmake --build --preset macos_debug_tests --target AlbumBackendProjectTest ApplicationModuleHostShutdownTest
ctest --test-dir build/macos-debug-tests -R "AlbumBackendProjectTest|ApplicationModuleHostShutdownTest" --output-on-failure
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_main
```

Put logs in `build/tmp/welcome_overview_phase3/`. Confirm with `ctest -N -R ...`
on both platforms that the new tests are discovered before you report a pass.

**Exit criteria**

- [ ] The five Phase 3 tests are discovered and pass on Windows.
- [ ] The five Phase 3 tests are discovered and pass on macOS.
- [ ] The Phase 1 cases of both suites still pass on both platforms.
- [ ] `alcedo_main` builds on both platforms.
- [ ] The two manual checks are recorded on Windows and on macOS.
- [ ] The terminology check finds no prohibited term in changed files.

**Expected diff**

80-160 lines: host 25-40, tests 55-120.

**Completion record**

```text
Phase / date / status:
Source revision and branch:
Actual changed modules:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes (Windows, macOS):
Discovered / passed / failed / skipped counts (Windows, macOS):
Manual verification (Windows, macOS):
Evidence path:
Remaining defects or unavailable platforms:
```

## 10. Cross-phase acceptance matrix

| Behavior | Expected result | Evidence |
| --- | --- | --- |
| Startup with recent projects | Most recent project loads in preview; welcome surface visible; no ring | Phase 1 test + manual |
| Startup without recent projects | Empty layout; no load | Phase 2 manual 5 |
| Select another project | Skeleton, then overview; previous project in the list; old package unchanged | `PreviewSwitchLeavesUnenteredPackageBytesUnchanged`, manual 2 |
| Click during a running load | Rows and actions disabled; `PreviewProject` returns `false` | `SecondPreviewWhileLoadingIsRejected` |
| Continue when ready | Enters at once; recent entry updated | `EnterLoadedProjectRegistersRecentEntry` |
| Continue while loading | Ring until the Library is visible | `EnterDuringPreviewLoadEntersOnCompletion`, manual 7 |
| Open project | Direct load with ring; no preview | manual |
| New project | Form in the right column; create enters with ring | manual 6 |
| Preview failure | Error text; entry removed; previous project stays loaded | `PreviewFailureRemovesRecentEntryAndKeepsLoadedProject`, manual 4 |
| Exit from welcome | No repack; workspace removed | `ShutdownWithUnenteredProjectSkipsRepack` |
| Exit after enter | Repack as today | `ShutdownWithEnteredProjectRepacks` |
| Workspace at exit | Runtime workspace removed after the database closes, entered or not | `ShutdownRemovesUnenteredProjectWorkspace`, `ShutdownRemovesEnteredProjectWorkspace` (Phase 3) |
| macOS | Every phase result above is also recorded on macOS | Phase exit criteria |
| Overview values | Photos, edited by commit presence, capture range without NULLs | overview tests |
| Cover | Rows 0-2 of the Library order; grid pins shared | `PreviewPopulatesFirstThumbnailRowsInLibraryOrder`, manual 1 |
| `reduceMotion` | Static skeleton | manual 3 |
| Accessibility | Focus order and accessible names | manual 8 |

## 11. Build and evidence rules

- Windows: `win_debug` preset in `build/debug` through
  `scripts\msvc_env.cmd` in the PowerShell tool.
- macOS: `macos_debug_tests` preset in `build/macos-debug-tests` for the
  tests, and `macos_debug` in `build/macos-debug` for the app.
- Each phase records Windows and macOS results. A platform that was not run is
  recorded as not run, and the phase stays partial.
- Run only the named test targets. The user starts any full suite run.
- Record each result as pass, fail, skip, or not run. A skip is not a pass.
- Evidence goes to `build/tmp/welcome_overview_phase<N>/`. Remove it when the
  phase is complete.
- Manual checks supplement the C++ tests for all QML behavior.

## 12. Risks and stop conditions

| Risk | Detection signal | Response |
| --- | --- | --- |
| A code path writes project data during preview | `PreviewSwitchLeavesUnenteredPackageBytesUnchanged` fails | Find the writer. Stop and report. Do not add a dirty flag without a user decision. |
| Startup preview competes with accelerator preparation | Preview starts while `acceleratorPreparing` is `true` | Wait for the flag through `ShellSignals` (step 12). |
| Cover pins change the grid tier | Grid thumbnails reload when the welcome surface closes | Use the grid `desiredMaxEdge` for the cover pins. |
| Overview query is slow on a large project | Load time increase in `project_open_benchmark` | Record the time. Do not move the query to a fallback path; report the number. |
| Overlay logic relies on removed `welcomeDismissedForLaunch` | Welcome surface reappears after a failed enter-mode load while a project is loaded | Keep the welcome surface visible only by `!projectEntered`; add a manual check. |

Stop implementation when:

- a required owner API does not exist;
- the source audit no longer matches the branch;
- a phase can pass the 2000-line limit;
- an operation needs a fallback that the user did not authorize;
- a proposed consistency mechanism lacks a real production interleaving.
