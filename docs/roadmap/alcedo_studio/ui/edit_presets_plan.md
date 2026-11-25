# Edit Presets Plan

- Date: 2026-11-24
- Status: planned
- Tracking issue: #1090
- Direct prerequisite plans:
  [Phase NM10, Node-aware Adjustment Transfer](../edit/node_mask_editor/phase_nm10_adjustment_transfer_plan.md)
  (complete), [LUT Library and Package Management](lut_library_and_package_management_plan.md)
  (complete)
- Source revision of the audit: `68a2c2356` (`main`)

Before the implementation of a phase starts, read `AGENTS.md`, `CONTRIBUTING.md`, and the skills
`alcedo-qml-ui`, `alcedo-create-pr`, and `alcedo-msvc-cmake` again. The repository rules can change
after this plan.

---

## 1. Decision record

### 1.1 User goal (2026-11-24)

Alcedo Studio gets presets. The user gave these eight requirements.

| Id | Requirement |
| --- | --- |
| R1 | A preset is the Adjustment Transfer package that Copy Adjustments makes. |
| R2 | To apply a preset is to apply that package. It makes a new Version. |
| R3 | The user selects one folder for presets. The user can add a preset from another person. A preset from another person must not be able to run code or attack the application. |
| R4 | The Versions panel has two parts: presets at the top, the Versions of the photo at the bottom. A drag of a Version into the top part makes a preset. A drag of a preset into the bottom part runs the Paste code, which makes a new Version and makes it active. |
| R5 | Presets go into a small memory cache, so that an apply to many photos is fast. |
| R6 | The image context menu gets `Apply Preset...`. It opens a dialog in which the user selects a preset for all selected photos. |
| R7 | The preset list shows preview images, as the LUT browser does. The Versions panel and the dialog of R6 use the same code. A preset can change the RAW Decode options, but the demosaic method and the noise reduction do not go into a preview image. |
| R8 | The user can apply a preset file one time. This makes a Version only. The application does not keep the file. |

### 1.2 Detail decisions of this plan

The user did not specify these items. This plan decides them.

| Id | Decision | Reason |
| --- | --- | --- |
| D1 | A preset file is the canonical JSON of `ExportDocumentTransfer`, with the fingerprint. It has no added header. The extension is `.alcpreset`. The preset name is the file name without the extension. | R1. No second format and no second parser. A package that automation writes is a preset. |
| D2 | A preset file of another schema or document format version is not available. The list shows it as not available. The compatibility rule of preset files is the rule of project history in `pipeline_history_format.hpp`: a change that converts stored project history must convert preset files in the same change. | The transfer schema has no conversion today, and the loaders accept one value only. |
| D3 | The default preset folder is `~/.alcedo/presets`. Each direct subfolder is a group. Deeper folders are not read. | Same location rule as the LUT library root (`~/.alcedo/luts`). |
| D4 | A preset that a drag makes contains each transferable item of the Version except `Crop and Rotate` and `Lens Correction`. The Copy dialog gets the action `Save as Preset`, which saves the items that the user selected there. | A crop and a lens profile belong to one photo and one lens. The dialog gives the full selection when the user wants it. |
| D5 | A preset file contains no absolute path. Section 3.3 gives the rule for LUT references and the lens profile database. | R3. |
| D6 | A preview image shows the preset on the import root of the photo, with the sensor settings of the current Version. This is the exact result of an apply, except for the sensor stage. | R7, and the same design as the LUT preview (#598). |
| D7 | The new Version gets the name of the preset. | The Versions list then shows which preset made a Version. |
| D8 | `Delete` moves the preset file to the system trash. | The file can be the only copy. |
| D9 | The preset library has no file watcher. It scans at start, at a refresh, after its own writes, and when the preset browser opens. | Same rule as the LUT library. |

### 1.3 Rules that stay

- Paste never merges into a Version. A preset apply always makes a new root-relative Version.
- AI Masks are not transferable. A preset contains no AI Mask.
- A preset apply keeps the camera profile, the as-shot white balance, and the root identity of the
  target photo.

---

## 2. Product design specification

### 2.1 Versions panel

```text
+--------------------------------------+
| Presets                 [+] [...]    |   header: Add, menu
| +---------+ +---------+              |
| | preview | | preview |              |   PresetBrowser (shared)
| | Name    | | Name    |              |   groups as section titles
| +---------+ +---------+              |
|======================================|   splitter, height is stored
| Versions       [cmp] [br] [root] [p] |   current header, unchanged
| | Version 3 (active)               | |
| | Version 2                        | |   current list, unchanged
+--------------------------------------+
```

- The top part is the preset browser. The bottom part is the current Versions list with its
  current header, draft row, and row actions.
- The splitter height is stored in `QSettings`. The top part can collapse to its header.
- Header menu `[...]`: `Apply Preset File...`, `Refresh`, `Show Folder`.
- Header button `[+]`: `Add Preset...` opens a file dialog for `.alcpreset` files.
- A preset tile shows the preview image and the name. The tooltip shows the item summary of the
  package (the same rows as the Paste confirmation). The name and the tooltip are plain text.
- A tile that is not available shows the warning icon only. Its tooltip shows the reason.
- Tile context menu: `Apply`, `Rename`, `Show in Folder`, `Delete`.
- An empty preset folder shows the drop icon only, with no text.
- All colors, spaces, and sizes come from `appTheme` and `DESIGN.md`. New tokens go into both in
  the same change. The style is Basic.

### 2.2 Drag, drop, and activation

| Action | Result |
| --- | --- |
| Drag a Version row into the preset part | Save a preset of that Version (D4). The name is the Version name, made unique. The new tile enters inline rename. |
| Drag a preset tile into the Versions part | Apply the preset to the open photo. |
| Double-click a preset tile, or `Enter` on the focused tile | Apply the preset to the open photo. |
| Drop `.alcpreset` files from the system on the preset part | Add each file (same as `Add Preset...`). |
| Drop one `.alcpreset` file from the system on the Versions part | Apply the file one time (R8). |

During a drag, the part that accepts the drop shows the drop highlight. A drop outside an
accepting part does nothing. `Escape` cancels a drag.

The apply of a preset to the open photo is refused while an automation agent holds control, as
`PasteAdjustmentPackage` does today.

### 2.3 `Apply Preset...` dialog

- The image context menu shows `Apply Preset...` below `Paste Adjustments`. The targets are the
  targets of `Paste Adjustments` (`libraryMutations.ResolveTargets`).
- The dialog contains the shared preset browser and the buttons `Open File...`, `Cancel`, and
  `Apply`. `Apply` is disabled until a preset is selected.
- The preview images use the first target photo.
- `Open File...` applies one file one time to the targets (R8) and closes the dialog.
- `Apply` starts the current multi-target Paste. The background task, its locks, the thumbnail
  refresh, and the result report are the current ones. A target that is open in the editor goes
  through the editor session, as Paste does today.
- Buttons use `DialogActionButton`. The dialog shows no count of presets or targets.

### 2.4 Preview images

- Long edge 256 px, memory LRU only, one render at a time, no render while the viewport renders.
- The preview document is the import root plus the preset, built by the Paste planner. The Develop
  sensor settings (demosaic method, AI Denoise, and the other inputs of the sensor result) come
  from the current Version through `UseSensorSettingsFrom`. The render then uses the sensor result
  that the editor holds.
- A preset that changes only sensor settings gives a preview equal to the root. This is accepted.
- A preview is not valid after a change of the preset file (fingerprint), the photo, or the sensor
  settings of the current Version.
- A failed preview shows the warning icon on the tile. The tile stays usable.

### 2.5 Settings

The settings dialog gets a `Presets` row group on the page that holds the LUT library folder: the
preset folder (`FolderPathField` plus `FolderDialog`) and `Refresh`. A change of the folder reads
the new folder. It moves no file.

### 2.6 Failure behavior

- Save, add, rename, delete, and apply report the real error in the panel error label or the
  dialog. No operation continues with a changed or reduced preset.
- An apply that fails changes no history. This is the current Paste behavior.
- A file that fails a rule of section 3 is never written into the preset folder by `Add`.

---

## 3. Safety of preset files

A preset file is data. The application never runs content of a preset file. The risk is a file
that a person made by hand to attack the reader, the operators, or the display. Each rule below is
a check in one function, `ReadPresetFile`, which is the only reader of a preset file from disk.
`Add`, the folder scan, the one-time apply, and automation all call it.

### 3.1 File and parse limits

| Rule | Limit | Failure |
| --- | --- | --- |
| File kind | A regular file. A symbolic link or a reparse point is refused. | `not a regular file` |
| File size | At most 4 MiB, read from the open handle | `file too large` |
| Encoding | UTF-8 without errors | `invalid text` |
| JSON depth | At most 16 levels, checked by the parse callback during the parse | `too deep` |
| JSON parse | `nlohmann::json::parse` without exceptions to the caller | `invalid JSON` |

### 3.2 Package rules

1. `ImportDocumentTransfer` checks the package. It refuses unknown fields at each level, another
   schema or version, an operator list, an AI Mask, an unknown adjustment type, and parameters
   that the model of the adjustment cannot load.
2. The fingerprint is required in a preset file. The file text must be the canonical dump. A
   package without a fingerprint is refused. (`ImportDocumentTransfer` accepts an absent
   fingerprint for other callers.)
3. Count limits: at most 32 Color Grades, 64 adjustments for each node, 64 Masks for each Grade.
4. Each string is at most 256 bytes. Each number is finite.

### 3.3 References to files

A package can refer to a file in two places (source audit, section 5).

| Field | Rule in a preset file |
| --- | --- |
| LMT `cube_path` (`FileLutReference`, an absolute path) | Refused. |
| LMT `reference` of kind `library` (`relative_path`) | Accepted only when the path is relative, has no root name, and has no `..` part. The LUT library resolves it below its root. |
| LMT `reference` of kind `official` | Accepted. |
| `lens_calib.lens_profile_db_path` | Refused when it is not empty. |

Reason: an absolute path lets a preset make the application open any file, and on Windows a UNC
path makes the system send the credentials of the user to another computer.

When the application saves a preset:

- A `FileLutReference` whose file is an entry of the LUT library is written as the `library`
  reference of that entry.
- Each other absolute path refuses the save. The error names the item. The user adds the LUT to
  the LUT library, or saves from the Copy dialog without that item.

### 3.4 Operator parameters

The model of each adjustment loads its parameters in `ImportDocumentTransfer`. Phase PS1 audits
the `LoadJson` of each transferable model (Color Grade adjustments, DRT/Post adjustments, DRT
parameters, the Develop fields, geometry, Radial and Linear Masks) for range checks on each value
that sets a size, a count, an index, or a loop bound in a kernel. The audit result is a table in
the PS1 completion record. Each missing range check gets its own issue and its fix in PS1.

### 3.5 Display and file names

- QML shows the preset name, the Grade names, and the tooltip with `textFormat: Text.PlainText`.
  Rich text from a file can load remote content.
- `Add` and `Save` make the file name from the name: no path separator, no control character, no
  reserved Windows device name, at most 120 characters. A name that exists gets ` 2`, ` 3`. No
  operation overwrites a file.
- The scan reads the preset folder and its direct subfolders only, follows no link, and stops at
  2000 files with an error.

---

## 4. Scope

### 4.1 Included work

| Module | Responsibility |
| --- | --- |
| `app/` preset file | `ReadPresetFile`, `WritePresetFile`, the rules of section 3 |
| `app/` preset library service | Folder, scan, inventory, package cache, add, save, rename, delete |
| `app/` preset preview | Preview document, session render request |
| `ui/alcedo_main/album_backend` | Preset list model, preset controller, preview controller, image provider |
| `ui/alcedo_main/qml` | `PresetBrowser`, Versions panel split, `ApplyPresetDialog`, settings rows, context menu entry |
| `ui/alcedo_main/automation` | `presets.list`, `presets.save`, `presets.apply` |
| `docs/` | `DESIGN.md` tokens, QML docs, this plan |

### 4.2 Exclusions

- Preset packages for download, a feed, or a signature. The LUT package service keeps that work.
- Conversion of older transfer schemas (D2).
- A merge of a preset into the current Version.
- The WebGPU and CPU render paths.

---

## 5. Current source audit

Paths are relative to `alcedo_studio/src/`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Package type | `include/app/adjustment_transfer_types.hpp` | `AdjustmentTransferPackage`: sparse Develop, geometry, Color Grades, Masks, DRT/Post, fingerprint | None |
| Package JSON | `app/document_transfer.cpp` (`ImportDocumentTransfer` :414, `ValidateDocumentTransfer` :316) | Strict keys, schema `alcedo.adjustment_transfer.v7`, document format 7, model `LoadJson` for each adjustment. No size limit. The fingerprint is optional. The caller parses the JSON. No file reader or writer exists in the application | PS1 adds the file reader and writer around it |
| Capture | `app/adjustment_transfer_package_builder.cpp` (`Build`, `SelectAllTransferableItems`), `app/adjustment_transfer_catalog.cpp` (`ReadVersion`) | Builds a package from a document and a selection. `ReadVersion` replays a Version that is not active | PS1 adds the selection of D4 |
| Paste, library targets | `ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp` (`StartApplyToTargets` :59) | One `AdjustmentPaste` background task, one package for all targets, `PasteAsRootRelativeVersion` for each target, thumbnail refresh | PS4 calls it with a preset package |
| Paste, open photo | `EditorSessionController::PasteAdjustmentPackage` (:2404), `EditorSessionService::PasteAdjustments` (:814), `EditorHistoryTransfer::PasteLiveRootRelativeVersion` (:63) | Session command `ApplyPaste`. This path calls `DocumentTransferPlanner::Plan` itself | PS2 calls it with a preset package |
| Controller | `adjustment_transfer_controller.cpp` | Holds the copied package in memory. Version name is always `Pasted Adjustments`. `strategy` accepts `paste` only | PS2 adds `Save as Preset` to the Copy dialog path |
| Version names | `editor_history_shared_helpers.cpp:74` (` 2`), `app/adjustment_transfer_service.cpp:17` (` (N)`) | The two Paste paths make a name unique in different forms | None. Issue #1091 |
| Versions panel | `qml/EditorVersionsPanel.qml` (944 lines), `EditorHistoryModel` | One column: header, draft row, list `editorVersionsList`. No context menu and no drag | PS2 |
| Drag examples | `qml/CollectionsPanel.qml:1661`, `qml/LibraryWorkspace.qml:656`, `FileDrop.js` | Row drag in a list with a drop target; file `DropArea` | PS2 uses both patterns |
| LUT library | `app/lut_library_service.cpp` (`LutLibraryService`) | One root (`lut/libraryRoot`, default `~/.alcedo/luts`), scan on one worker, results on the owner thread, one operation at a time, no file watcher | PS1 follows this structure |
| LUT preview | `app/lut_preview_image.cpp` (`BuildLutPreviewDocument`), `EditorSessionService::RenderLutPreview` (:3024), `lut_preview_controller.cpp`, `lut_preview_image_provider.cpp` | Root replay, `UseSensorSettingsFrom(current)`, 256 px, LRU of 200, one render at a time, `image://alcedo-lut-preview/<id>` | PS3 adds the preset form of each part |
| LUT browser QML | `qml/EditorLutResultCard.qml` (tile :439, preview image :568) | Row list of tiles, watch and unwatch of previews | PS2 and PS3 use the tile structure |
| File references | `edit/operators/models/lmt_model.cpp:51` (`cube_path`, `reference`), `app/pipeline_history_applier.cpp:92` (`lens_profile_db_path`) | Paste copies the parameters as they are. The render resolves them | PS1, section 3.3 |
| Image menu | `qml/ImageContextMenu.qml` (:238), `ImageActionsController.qml`, `AppDialogs.qml` | `copy-adjustments`, `paste-adjustments` | PS4 |
| Automation | `automation/commands/adjustment_commands.cpp` | `adjustments.items`, `adjustments.copy`, `adjustments.paste`. Packages stay in memory | PS4 |
| Thumbnail render | `include/app/thumbnail_service.hpp` | Renders the active Version of a photo into its cache. No operation renders a caller document | PS4 |
| Settings | `qml/SettingDialog.qml`, `qml/LutSettingsPanel.qml`, `qml/FolderPathField.qml` | Folder field plus `FolderDialog` | PS1 |

Tests that cover these areas: `tests/app/document_transfer_test.cpp`,
`adjustment_transfer_service_test.cpp`, `lut_library_service_test.cpp`,
`tests/ui/adjustment_transfer_controller_test.cpp`, `lut_preview_controller_test.cpp`,
`editor_versions_panel_qml_test.cpp`, `tests/automation/automation_adjustment_commands_test.cpp`.

---

## 6. Target architecture

### 6.1 Owners

| Owner | Input | Output | Changes | Lifetime | Error surface |
| --- | --- | --- | --- | --- | --- |
| `ReadPresetFile` / `WritePresetFile` (free functions, `app/preset_file`) | A path, or a package and a path | A package, or a new file | `Write` makes one new file through a temporary file and a rename | None | A result value with the rule that failed |
| `PresetLibraryService` (`app/`, `QObject`) | Folder setting, user operations | Inventory, packages, change signals | The preset folder, the inventory, the package cache | Application | Result of each operation; `available=false` plus reason for each bad entry |
| `PresetListModel` (`album_backend`) | Inventory of the service | Rows for QML | Nothing in the service | QML host | None |
| `PresetController` (`album_backend`, `appModules.presets`) | QML commands | Calls to the service, the editor session, the apply coordinator | Nothing of its own except the result of the last operation | Application | `{success, message}` maps and signals, as `AdjustmentTransferController` |
| `PresetPreviewController` (`album_backend`, `appModules.presetPreview`) | Watched preset ids, a preview source | Preview images | Its LRU | Application | Failed key set |

The inventory is the scan result of the folder. It is not a copy of other state. One entry has: id
(the path relative to the folder), name, group, file size, write time, fingerprint, the item
summary, and the availability with its reason.

### 6.2 Package cache (R5)

`PresetLibraryService` keeps parsed packages as `std::shared_ptr<const AdjustmentTransferPackage>`
in an LRU of 64 entries. The key is the entry id. An entry is valid while the file size and the
write time are equal to the values at the read. `PackageFor(id)` checks the file stamp, then
returns the cached package or reads the file again through `ReadPresetFile`. The multi-target
Paste takes one package for all targets, so an apply to many photos reads and validates the file
at most one time. The cache holds immutable values, and one thread (the owner thread) reads and
writes the LRU.

### 6.3 Threads

The scan runs on one worker and posts the inventory to the owner thread, as `LutLibraryService`
does. The service runs one operation at a time. A request during an operation waits in the
operation queue of the service. This plan adds no version, token, or cancel protocol.

### 6.4 Call chains

Save by drag:

```text
EditorVersionsPanel drop(versionId)
 -> PresetController::SaveVersionAsPreset(versionId)
 -> ReadSourceHistory -> AdjustmentTransferCatalogService::ReadVersion
 -> AdjustmentTransferPackageBuilder::Build(document, SelectPresetItems(document))
 -> PresetLibraryService::Save(package, name)
      -> reference rules of 3.3 -> WritePresetFile -> scan -> InventoryChanged
 -> QML: the new tile enters rename
```

Apply to the open photo:

```text
PresetBrowser activate(id) or drop on the Versions part
 -> PresetController::ApplyToEditor(id)
 -> PresetLibraryService::PackageFor(id)
 -> EditorSessionController::PasteAdjustmentPackage(package, presetName)
 -> (current chain) ApplyPaste command -> PasteLiveRootRelativeVersion -> new active Version
```

Apply to library targets:

```text
ApplyPresetDialog Apply
 -> PresetController::ApplyToTargets(id, targetEntries)
 -> PackageFor(id)
 -> the target that is open in the editor: PasteAdjustmentPackage
 -> the other targets: AdjustmentTransferApplyCoordinator::StartApplyToTargets(package, ids, presetName)
 -> ApplyFinished -> result map to QML
```

One-time apply: the same two chains, with `ReadPresetFile(path)` in place of `PackageFor(id)`.
The package does not enter the service.

Failure: `ReadPresetFile` or `PackageFor` returns the failed rule. The controller returns
`{success:false, message}`. No Paste starts and no history changes. A failed Paste keeps the
current restore behavior of each Paste path.

---

## 7. File and API map

Proposed files (all new):

| File | Content |
| --- | --- |
| `include/app/preset_file.hpp`, `app/preset_file.cpp` | `ReadPresetFile(path) -> PresetFileReadResult`, `ParsePresetText(bytes)`, `WritePresetFile(package, path) -> PresetFileWriteResult`, `CheckPresetReferences(package)`, the limits as constants |
| `include/app/preset_library_service.hpp`, `app/preset_library_service.cpp`, `app/preset_library_scan.cpp` | `PresetLibraryService`, `PresetLibraryPreferences` (`presets/libraryRoot`), `PresetEntry` |
| `include/app/preset_preview_image.hpp`, `app/preset_preview_image.cpp` | `BuildPresetPreviewDocument(graph, root, current, element_id, package)` |
| `album_backend/preset_list_model.{hpp,cpp}`, `preset_controller.{hpp,cpp}`, `preset_preview_controller.{hpp,cpp}`, `preset_preview_image_provider.{hpp,cpp}` | Qt owners of 6.1 |
| `qml/PresetBrowser.qml`, `qml/PresetTile.qml`, `qml/ApplyPresetDialog.qml`, `qml/PresetSettingsSection.qml` | Shared browser, tile, dialog, settings rows |
| Tests | Section 9 names them |

Current files that change: `app/adjustment_transfer_package_builder.{hpp,cpp}`
(`SelectPresetItems`), `app/editor_session_service.{hpp,cpp}` and the session ports
(`RenderPresetPreview`), `app/thumbnail_service` (PS4), `qml/EditorVersionsPanel.qml`,
`qml/AdjustmentTransferDialog.qml`, `qml/ImageContextMenu.qml`, `qml/ImageActionsController.qml`,
`qml/AppDialogs.qml`, `qml/SettingDialog.qml`, `application_module_host.cpp`,
`automation/commands/adjustment_commands.cpp`, the two `CMakeLists.txt` files,
`i18n/alcedo_main_en.ts`, `i18n/alcedo_main_zh_CN.ts` (edited by hand), `DESIGN.md`.

---

## 8. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| PS1 | Preset file with the safety rules, preset library service with the package cache, folder setting | `app/`, settings QML | None | 1500 to 1900 | planned |
| PS2 | Versions panel with the preset part, drag in both directions, add, one-time apply, rename, delete, `Save as Preset` | `album_backend`, QML | PS1 | 1500 to 1900 | planned |
| PS3 | Preview images in the preset browser | `app/`, `album_backend`, QML | PS2 | 1100 to 1500 | planned |
| PS4 | `Apply Preset...` for many photos with previews, automation commands | QML, `album_backend`, `app/thumbnail_service`, automation | PS3 | 1400 to 1800 | planned |

Each phase is one pull request. If an estimate grows above 2000 lines during the work, split the
phase and update this table first.

---

## 9. Detailed phases

### PS1: Preset file, safety rules, and library service

**Objective and deliverables.** The application can write a preset file, read it with each rule of
section 3, keep the preset folder inventory, and return cached packages. The settings dialog can
change the folder. No preset UI exists after this phase except the settings rows.

**Implementation steps.**

1. Add `preset_file`: the limits of 3.1, the parse with the depth callback, the call of
   `ImportDocumentTransfer`, the required fingerprint, the count and string limits of 3.2, and
   `CheckPresetReferences` (3.3). Each failure returns the rule id and a message. No exception
   leaves the function.
2. Add `WritePresetFile`: canonical dump, write to a temporary file in the target folder, rename
   to the final name. It refuses to replace a file.
3. Add `SelectPresetItems(document)` to the package builder: all transferable items except
   `Geometry` and `LensCalibration` (D4).
4. Add `PresetLibraryService` with `Start`, `UseRoot`, `Refresh`, `Save(package, name)`,
   `Add(paths)`, `Rename(id, name)`, `Delete(id)` (system trash), `PackageFor(id)`, `Entries()`,
   and the signals `InventoryChanged` and `RootChanged`. `Save` changes a `FileLutReference` of a
   LUT library entry into its `library` reference through `LutLibraryService`, then checks 3.3.
5. Add the package cache of 6.2.
6. Audit the `LoadJson` of each transferable model (3.4). Open one issue for each missing range
   check and correct it in this phase.
7. Add `PresetSettingsSection.qml` and its entry in `SettingDialog.qml`.

**Tests and evidence** (`tests/app/preset_file_test.cpp`, `preset_library_service_test.cpp`,
`tests/ui/preset_settings_qml_test.cpp` is not added: pure UI).

| Test | Assertion |
| --- | --- |
| `WrittenPresetReadsBackWithEqualFingerprint` | Write then read returns a package with the same fingerprint |
| `PresetAboveSizeLimitIsRefusedBeforeParse` | A 4 MiB + 1 byte file returns `file too large` and the parser does not run |
| `PresetDeeperThanDepthLimitIsRefused` | 17 nested levels return `too deep` |
| `PresetWithoutFingerprintIsRefused`, `NonCanonicalPresetIsRefused` | Each returns its rule |
| `PresetWithUnknownFieldIsRefused`, `PresetWithUnknownAdjustmentTypeIsRefused` | The error names the field or the type |
| `PresetWithAbsoluteLutPathIsRefused`, `PresetWithUncLutPathIsRefused`, `PresetWithParentPathLutReferenceIsRefused`, `PresetWithLensDatabasePathIsRefused` | Each returns the reference rule |
| `PresetWithNonFiniteNumberIsRefused`, `PresetAboveGradeCountLimitIsRefused`, `PresetWithLongStringIsRefused` | Each returns its rule |
| `SymbolicLinkPresetIsRefused` | A link to a valid preset returns `not a regular file` |
| `MutatedPresetsNeverCrashReaderOrPastePlanner` | A fixed-seed mutation of valid presets (10000 cases) through `ParsePresetText` and, when accepted, `DocumentTransferPlanner::Plan` on a default document. The run under `win_debug_asan` reports no error |
| `SaveWritesLibraryReferenceForLutLibraryFile`, `SaveRefusesLutOutsideLibrary` | 3.3 save rules |
| `ScanListsGroupsAndMarksBadFilesNotAvailable` | Root and one subfolder level are listed; a bad file has `available=false` and its reason |
| `PackageForReadsFileOnceUntilFileChanges` | Two calls read the file one time; a changed write time causes a second read |
| `AddCopiesValidFileAndRefusesBadFile`, `SaveNeverOverwrites`, `RenameAndDeleteUpdateInventory` | Folder content and inventory after each operation |

**Build and run.**

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target PresetFileTest PresetLibraryServiceTest DocumentTransferTest"
ctest --test-dir build/debug -R "PresetFile|PresetLibraryService|DocumentTransfer" --output-on-failure
```

Run `MutatedPresetsNeverCrashReaderOrPastePlanner` in `build/debug-asan` also.

**Exit criteria.**

- [ ] Each test above passes in the debug build.
- [ ] The mutation test passes under AddressSanitizer.
- [ ] The completion record holds the `LoadJson` audit table and the issue numbers.
- [ ] No reader of a preset file exists outside `ReadPresetFile`.

**Expected diff.** 1500 to 1900 lines.

**Completion record.** Empty.

### PS2: Versions panel, drag, and preset commands

**Objective and deliverables.** The Versions panel shows the preset browser above the Versions
list. Each row of the table in 2.2 works. Tiles show the name and a neutral image area (previews
come in PS3). The Copy dialog has `Save as Preset`.

**Implementation steps.**

1. Add `PresetListModel` (roles: `presetId`, `name`, `group`, `available`, `reason`, `summary`,
   `fingerprint`) and `PresetController` with `SaveVersionAsPreset(versionId)`,
   `SaveDialogSelectionAsPreset()`, `AddFiles(urls)`, `ApplyToEditor(presetId)`,
   `ApplyFileToEditor(url)`, `Rename`, `Delete`, `ShowInFolder`, `Refresh`. Register both in
   `application_module_host.cpp`.
2. `ApplyToEditor` and `ApplyFileToEditor` call `EditorSessionController::PasteAdjustmentPackage`
   with the preset name as the Version name.
3. Add `PresetBrowser.qml` and `PresetTile.qml`. The browser is a row list of tiles with group
   titles, as `EditorLutResultCard.qml`. It has the properties `selectedPresetId` and
   `previewSource`, and the signals `activated(presetId)` and `contextRequested`. Selection by
   click does not rebuild the list and does not move `contentY`.
4. Split `EditorVersionsPanel.qml` with a splitter. Keep each current `objectName` of the Versions
   part.
5. Add the row drag of a Version (pattern of `CollectionsPanel.qml:1661`) and the tile drag of a
   preset. Add the two file `DropArea` items with `FileDrop.js`.
6. Add `Save as Preset` to `AdjustmentTransferDialog.qml` in copy mode. It builds the package of
   the dialog selection and calls `PresetLibraryService::Save`.
7. Add the strings to both `.ts` files by hand. Add the tokens to `AppTheme` and `DESIGN.md`.
   Register the QML files in `ALCEDO_MAIN_QML_FILES`.

**Tests and evidence** (`tests/ui/preset_controller_test.cpp`).

| Test | Assertion |
| --- | --- |
| `SaveVersionAsPresetWritesAllItemsExceptGeometryAndLens` | The saved package has no `geometry` and no `lens_calib`; the other selected fields are present |
| `SaveVersionAsPresetReadsVersionThatIsNotActive` | The package equals the capture of that Version and the active Version does not change |
| `ApplyToEditorCreatesActiveVersionNamedAfterPreset` | The session history has one more Version, it is active, and its name is the preset name |
| `ApplyFileToEditorLeavesLibraryUnchanged` | A new Version exists; the preset folder and the inventory are equal to their state before |
| `ApplyOfUnavailablePresetChangesNoHistory` | The result is a failure and the commit graph is equal to its state before |
| `ApplyIsRefusedWhileAgentHoldsControl` | The result is a failure with the policy message |

The QML drag and layout get no automated test (pure UI). The user checks them in the application.

**Build and run.**

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target PresetControllerTest AdjustmentTransferControllerTest alcedo_main"
ctest --test-dir build/debug -R "PresetController|AdjustmentTransferController" --output-on-failure
```

**Exit criteria.**

- [ ] Each test above passes.
- [ ] The user confirms the five actions of 2.2 in the running application.
- [ ] Each preset string in QML uses plain text format.

**Expected diff.** 1500 to 1900 lines.

**Completion record.** Empty.

### PS3: Preview images

**Objective and deliverables.** Each available tile in the Versions panel shows the preview image
of 2.4 for the open photo.

**Implementation steps.**

1. Add `BuildPresetPreviewDocument`: replay the import root, apply the package with
   `DocumentTransferPlanner::Plan` (the Paste planner), then call
   `UseSensorSettingsFrom(current->Document())`. Return `PipelineGraphSnapshot::Preview`.
2. Add `EditorPresetPreviewRequest {image_id, package}` and
   `EditorSessionService::RenderPresetPreview`, with the port functions that `RenderLutPreview`
   has. Use the render request and the queue of the LUT preview (`MakeLutPreviewRenderRequest`
   moves to a shared name when both use it).
3. Add `PresetPreviewController`: `watch`, `unwatch`, `previewUrl(id, revision)`, LRU of 200, one
   render at a time, no start while `render_busy`. The key is image id, preset fingerprint, and
   the hash of the sensor settings of the current Version. Put the shared queue and LRU code of
   the LUT and preset preview controllers in one helper; do not copy the file.
4. Add the image provider `image://alcedo-preset-preview/<id>` with the display ICC conversion of
   the LUT provider.
5. Bind `PresetTile.qml` to the preview source.

**Tests and evidence** (`tests/app/preset_preview_image_test.cpp`,
`tests/ui/preset_preview_controller_test.cpp`).

| Test | Assertion |
| --- | --- |
| `PreviewDocumentEqualsPasteResultExceptSensorSettings` | The preview document equals the document of a real Paste of the package after both get the sensor settings of the current Version |
| `PreviewDocumentKeepsDemosaicAndDenoiseOfCurrentVersion` | A preset with another demosaic method and AI Denoise leaves both values of the current Version in the preview document |
| `PreviewKeyChangesWithFingerprintImageAndSensorSettings` | Each of the three changes makes a new render; an edit of a Color Grade of the current Version makes none |
| `OneRenderRunsAtATimeAndWaitsForViewport` | The second request starts after the first completes and not while `render_busy` |
| `FailedPreviewIsNotRetriedAndTileStaysAvailable` | One failed render, no second request, the entry stays available |

**Exit criteria.**

- [ ] Each test above passes with the render through Auto to OpenCL.
- [ ] The user confirms the previews in the running application.
- [ ] The LUT preview tests still pass (`LutLibraryModelTest`).

**Expected diff.** 1100 to 1500 lines.

**Completion record.** Empty.

### PS4: `Apply Preset...` for many photos, and automation

**Objective and deliverables.** The image context menu applies a preset or a preset file to all
targets. The dialog shows previews of the first target. Automation can list, save, and apply
presets.

**Implementation steps.**

1. Add `ApplyPresetDialog.qml` to `AppDialogs.qml` with `PresetBrowser`. Add the action
   `apply-preset` to `ImageContextMenu.qml` and `requestApplyPreset()` to
   `ImageActionsController.qml`.
2. Add `PresetController::ApplyToTargets(presetId, targetEntries)` and
   `ApplyFileToTargets(url, targetEntries)`. Route the target that is open in the editor through
   the session and the other targets through `StartApplyToTargets`, as
   `AdjustmentTransferController::Paste` does. Move that routing into one function that both
   controllers call.
3. Add the preview source for a library photo: an operation of `ThumbnailService` that renders a
   caller preview document of one photo at 256 px and returns the image without a write to the
   thumbnail cache. The sensor settings come from the active Version of that photo.
   `PresetPreviewController` gets the source from the browser (`previewSource`), so the editor
   and the dialog use the same controller code.
4. Add `presets.list`, `presets.save {element, version, name}`, and
   `presets.apply {preset | file, targets, version_name}` to the automation commands, with agent
   attribution on apply. Update `alcedo-cli schema` and the CLI reference in the alcedo-skills
   repository.

**Tests and evidence** (`tests/ui/preset_controller_test.cpp`,
`tests/automation/automation_preset_commands_test.cpp`).

| Test | Assertion |
| --- | --- |
| `ApplyToTargetsCreatesOneVersionPerTargetNamedAfterPreset` | Each target has one more Version with the preset name |
| `ApplyToTargetsReadsPresetFileOnce` | The file read count is one for 20 targets |
| `ApplyToTargetsRoutesOpenPhotoThroughEditorSession` | The open photo gets its Version from the session; the coordinator does not load it |
| `OneFailedTargetIsReportedAndOtherTargetsApply` | The result map has one failure row and the applied ids of the other targets |
| `ApplyFileToTargetsLeavesLibraryUnchanged` | The inventory is equal to its state before |
| `LibraryPreviewRenderDoesNotWriteThumbnailCache` | The thumbnail cache entry of the photo is equal to its state before |
| `PresetsApplyCommandStoresAgentAttribution` | The Paste commit of each target has the agent author |

**Exit criteria.**

- [ ] Each test above passes.
- [ ] Time record: `Apply Preset...` on 200 photos against `Paste Adjustments` of the same package
      on the same photos, release build. The preset time is not above the Paste time by more than
      5%.
- [ ] The user confirms the dialog in the running application.

**Expected diff.** 1400 to 1800 lines.

**Completion record.** Empty.

---

## 10. Cross-phase acceptance matrix

| Behavior | Expected result |
| --- | --- |
| Save a Version as a preset, apply it to another photo | The new Version renders the same adjustments, without the crop and the lens settings of the source |
| Apply a preset with a RAW Decode change | The new Version has the RAW Decode values of the preset; the preview before the apply showed the sensor result of the current Version |
| Add a preset file with an absolute or UNC LUT path | Refused with the reference rule; the application opens no such path |
| Add a file of 100 MiB, or nested 1000 levels | Refused by the limit before the complete parse |
| A preset of another schema in the folder | Listed as not available; no apply possible |
| A LUT of a preset is not in the LUT library of this computer | The apply makes the Version; the render reports the missing LUT as it does for a Paste today |
| Apply a preset to 200 photos, one RAW file missing | The other photos get the Version; the result reports the failed target |
| Reopen the application | The preset folder, the splitter height, and all preset files are as before |
| Undo after an apply in the editor | The behavior of Paste: the user switches back to the earlier Version |

---

## 11. Build and evidence rules

- Presets `win_debug` (tests) and `win_release` (time record). macOS: `macos_debug` with tests on.
- MSVC commands run through PowerShell with `scripts\msvc_env.cmd`.
- Render tests use Auto to OpenCL. GPU tests run with `ctest -j 1`.
- Temporary evidence goes to `build/tmp/presets/<phase>/`.
- No full test suite run. Each phase runs its named tests and the suites of its direct callers.
- Pure UI behavior is checked by the user in the application and recorded as manual verification.

---

## 12. Risks and stop conditions

| Risk | Detection | Response |
| --- | --- | --- |
| A model `LoadJson` accepts a value that a kernel cannot handle | PS1 audit and mutation test | Issue and fix in PS1 |
| `ThumbnailService` cannot render a caller document without a cache write | PS4 step 3 | Stop. Update this plan with the owner operation that the service needs |
| The two Paste paths give different unique names (` 2` against ` (N)`) | Seen in the audit | Issue #1091. The preset work does not change it |
| A future schema change makes all saved presets not available | D2 | The schema change must convert preset files or state the loss in its plan |
| A drag in the rail conflicts with the row click and inline rename | PS2 manual check | The drag starts after the system drag distance only |

Stop the implementation and update the plan when the source audit does not agree with the branch,
when a phase can pass 2000 lines, or when an operation needs a substitute path that the user did
not approve.

---

## 13. Completion record template

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
Performance or resource evidence, if required:
Evidence path:
Remaining defects or unavailable platforms:
```
