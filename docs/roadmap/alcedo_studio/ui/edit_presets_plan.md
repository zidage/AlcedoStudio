# Edit Presets Plan

- Date: 2026-11-24
- Status: PS1A, PS1B, PS2, PS3, PS4A, and PS4B implemented (2026-11-24); the user checks in the
  running application are open
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
  current header and draft row. A right click on a Version card opens its menu (issue #1124).
- The splitter height is stored in `QSettings`. The top part can collapse to its header.
- Header menu `[...]`: `Apply Preset File...`, `Refresh`, `Show Folder`.
- Header button `[+]`: `Add Preset...` opens a file dialog for `.alcpreset` files.
- A preset tile shows the preview image and the name. It has no tooltip with the item summary
  (issue #1124). The name and the tooltip are plain text.
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
| File kind | A regular file. A link is refused: a symbolic link, and on Windows each name-surrogate reparse point (a junction or a mount point). The reader opens the link itself, never its target. | `not a regular file` |
| File size | At most 4 MiB, read from the open handle | `file too large` |
| Encoding | UTF-8 without errors | `invalid text` |
| JSON depth | At most 16 levels, checked by a SAX scan that stops at the first deeper value, before the JSON value is built | `too deep` |
| JSON parse | `nlohmann::json::parse` without exceptions to the caller | `invalid JSON` |

### 3.2 Package rules

1. `ImportDocumentTransfer` checks the package. It refuses unknown fields at each level, another
   schema or version, an operator list, an AI Mask, an unknown adjustment type, and parameters
   that the model of the adjustment cannot load.
2. The fingerprint is required in a preset file. The file text must be the canonical dump. A
   package without a fingerprint is refused. (`ImportDocumentTransfer` accepts an absent
   fingerprint for other callers.)
3. Count limits: at most 32 Color Grades, 64 adjustments for each node, 64 Masks for each Grade.
4. Each string and each object key is at most 256 bytes. Each number is finite. JSON text has no
   form for a non-finite number: the parser refuses an overflowing number (`1e999`) as `invalid
   JSON`. A finite number above the float range is refused by the model loaders (#1097).

### 3.3 References to files

A package can refer to a file in two places (source audit, section 5). The rules apply to the
JSON before `ImportDocumentTransfer`, so that a path always fails with the reference rule.

| Field | Rule in a preset file |
| --- | --- |
| LMT `cube_path` (`FileLutReference`, an absolute path) | Refused. |
| LMT `reference` of kind `library` (`relative_path`) | Accepted only when the path is relative, has no root name, and has no `..` part. The LUT library resolves it below its root. |
| LMT `reference` of kind `official` | Accepted. |
| `lens_calib.lens_profile_db_path` | Accepted only when it is empty or the built-in database (`DevelopPayload` default, the relative path `src/config/lens_calib`). Each capture with Lens Correction carries the default. Each other path is refused. |

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
- The file name is the name plus `.alcpreset`. A name is refused, not changed, when it is empty,
  starts or ends with a space, ends with a dot, has a path separator, a control character, or one
  of `<>:"|?*`, is a reserved Windows device name, or is longer than 120 characters. In `Add` and
  `Save`, a name that exists gets ` 2`, ` 3`. A `Rename` to a name that exists is a conflict. No
  operation overwrites a file: a write goes to a temporary file and a rename that refuses an
  existing target.
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
| PS1A | Preset file with the safety rules, `SelectPresetItems`, `LoadJson` audit with its fixes | `app/`, `edit/` models | None | 1700 (actual) | complete |
| PS1B | Preset library service with the scan and the package cache, folder setting | `app/`, `album_backend`, settings QML | PS1A | 1400 (actual) | complete |
| PS2A | `PresetController`, `PresetListModel`, package summary, `PresetNameFromLabel`, `PresetControllerTest` | `app/`, `album_backend` | PS1B | 1300 (actual) | complete |
| PS2B | Versions panel with the preset part, drag in both directions, add, one-time apply, rename, delete, `Save as Preset` | QML, `AppTheme`, `.ts` | PS2A | 1200 (actual) | complete |
| PS3 | Preview images in the preset browser | `app/`, `album_backend`, QML | PS2 | 1550 net (actual) | complete |
| PS4A | `Apply Preset...` for many photos with previews: dialog, routing of the targets, library-photo previews (steps PS4.1 to PS4.3) | QML, `album_backend`, `app/thumbnail_service` | PS3 | 1700 (actual) | complete |
| PS4B | Automation commands `presets.list`, `presets.save`, `presets.apply` (step PS4.4) | automation, alcedo-skills | PS4A | 1350 with the record (actual) | complete |

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

- [x] Each test above passes in the debug build (`SymbolicLinkPresetIsRefused` skips on an
      account that cannot make a symbolic link; `JunctionPresetIsRefused` checks the same rule).
- [x] The mutation test passes under AddressSanitizer.
- [x] The completion record holds the `LoadJson` audit table and the issue numbers.
- [x] No reader of a preset file exists outside `ReadPresetFile`.

**Expected diff.** 1500 to 1900 lines. Actual: about 3100 lines (2200 production, 900 tests), so
the phase was split into PS1A and PS1B, one pull request each (section 8). Phase issue: #1093.

**Phase split (2026-11-24).** PS1A: `preset_file`, `SelectPresetItems`, the `LoadJson` audit
and its fixes (#1094, #1095, #1096, #1097), `PresetFileTest`. PS1B: `preset_library_scan`,
`PresetLibraryService` with the package cache, the module host entry, the settings tile,
`PresetLibraryServiceTest`.

**Implementation decisions** (detail that this plan did not specify; each agrees with D1 to D9).

| Item | Decision | Reason |
| --- | --- | --- |
| Link rule (3.1) | Windows refuses name-surrogate reparse points only (symbolic links, junctions, mount points); other reparse points, such as cloud file placeholders, are read. POSIX opens with `O_NOFOLLOW` and `O_NONBLOCK` and requires `S_ISREG`. | A placeholder of a synchronized folder is a regular file for the user. The risk of 3.1 is a link to another file. |
| Depth rule (3.1) | A SAX scan (`nlohmann::json::sax_parse`) checks depth, string and key length, and finite numbers, and stops at the first failure; the JSON value is built after the scan. | A hostile file costs one pass and no deep value is built. |
| Reference rules (3.3) | Applied to the raw JSON before `ImportDocumentTransfer`; `CheckPresetReferences(package)` applies the same rules for `Save`. | A path fails with `file reference` even when a model loader would also refuse it. |
| Lens database (3.3) | Empty or the built-in default (`src/config/lens_calib`) is accepted. | Each capture with Lens Correction carries the default; "refused when not empty" refused every such preset. |
| Count limits (3.2) | Checked on the JSON before the package is built; DRT/Post adjustments also have the limit of 64. | No work for an oversized package. |
| Names (3.5) | An invalid name is refused, not changed. `Rename` to an existing name is a conflict. | No silent change of a user-typed name. |
| `Add` of several files | The accepted files are added; each refused file is listed in the result (`kRefused`). The accepted text that the reader returned is written, not the source file again. | A bad file never enters the folder, and a good file is not held back by it. |
| Write | Temporary file `~alcpreset-<random>.tmp` in the target folder, then `QFile::rename`, which refuses an existing target. | No replace and no partial file. |
| Operation queue (6.3) | All operations run in request order on one worker. `UseRoot` stores the preference on the worker before the folder changes, so operations queued after it use the new folder. Name and reference checks of `Save`, `Rename`, and `Delete` run on the owner thread first and refuse at once. | `LutLibraryService::ReferenceForPath` is an owner-thread API. |
| Cache (6.2) | `PackageFor` refuses an id that the inventory does not list, without a file read. `PackageFileReadCount()` counts its reads. | No path outside the folder; a measurable cache. |
| Item summary (6.1) | Not in `PresetEntry`. PS2 builds it with the list model in `album_backend`. | The summary rows are UI text of the Paste confirmation. |
| Delete | `PresetLibraryServiceOptions::move_to_trash`, default `QFile::moveToTrash`. | Tests do not write into the trash of the user. |
| Host | `appModules.presetLibrary`; `PackageServiceFactories::preset_library_options` for tests. The settings tile `PresetSettingsSection` is on the LUT settings page, after the camera profiles tile: title, folder field with Browse, Rescan, error badge. | Same pattern as the DCP folder tile. |
| Float range (#1097) | `json_util::ToFloat` refuses a number outside the float range; the scalar models, HLS, LMT strength, Mask fields, the AI Mask recipe, and the Grade `mix` use it. | The conversion was undefined behavior. |

##### PS1 `LoadJson` audit (2026-11-24)

Scope: each adjustment type of the builtin catalog for Color Grades and DRT/Post, DRT parameters,
the Develop fields, geometry, Radial and Linear Masks. Brush Masks are not built by default
(`ALCEDO_ENABLE_BRUSH_MASK` is `OFF`). A value that sets a size, a count, an index, a selector, or
a loop bound was traced to its consumer.

| Model | Values | Range check | Result |
| --- | --- | --- | --- |
| 12 scalar adjustments | value | `SetValue` clamps | safe; float conversion fixed (#1097) |
| Sharpen | `radius` (tap count), `amount`, `threshold` | radius 0.1 to 64; taps at most 15 | safe |
| Clarity, Halation | sigma | from constants and geometry; taps capped | safe |
| CAT02 | `temperature`, `tint` | finite check, clamp | safe |
| Curve | point count, x order, finite coordinates | none in `LoadJson` | fixed (#1095) |
| HLS | bin and table lengths; `h_range_table` divisor | reads stop at 8 bins; kernel `fmaxf(w, 1)` | safe; float conversion fixed (#1097) |
| ColorWheel | `version` | refused unless absent or 2 | safe |
| LMT | `strength`, encodings, `reference` | 0 to 1; catalog encodings; reference validated | safe; paths refused by 3.3 |
| DRT | `method`, spaces, EOTF, presets | unknown value is the default | safe |
| DRT | `peak_luminance` | none | fixed (#1094): 1 to 10000 nits, else the load fails |
| DRT film print | stock and table ids; sliders | known ids; slider ranges | safe |
| Develop `raw_decode` | `ai_denoise.model_version` | 64 lowercase hex digits | safe |
| Develop `color_temp` | `custom_cct`, `custom_tint` | clamped in the color transform only | fixed (#1096): `RawWhiteBalanceWhite` clamps also |
| Develop `lens_calib` | `user_scale` (divisor), `target_projection` | finite positive; closed set | safe; database path by 3.3 |
| Geometry | `crop_rect`, rotation, aspect | clamped; normalized | safe |
| Radial, Linear Mask | radii, transition (divisors), normal | finite and not negative; kernel guards | safe |
| Mask ranges | color and luminance ranges, opacity | bounded | safe |
| Color Grade | `mix` | 0 to 1 | safe; float conversion fixed (#1097) |

Tone values without an upper bound (HLS adjustments, ColorWheel offsets, OpenDRT detail
parameters) set no size, count, index, or loop bound; very large values give inf or NaN pixels.
#1097 keeps this item open.

##### PS1A completion record (2026-11-24)

**Status:** complete.

**Primary success call chain:**

```text
ReadPresetFile(path)
  -> ReadRegularFileBytes: one handle, link refused, size from the handle (<= 4 MiB)
  -> ParsePresetText: UTF-8 -> SAX scan (depth 16, strings 256 bytes, finite numbers)
  -> json::parse -> CountLimitError -> fingerprint present -> PackageJsonReferenceError
  -> ImportDocumentTransfer (keys, schema, model LoadJson) -> fingerprint equal -> canonical text
  -> PresetFileReadResult{package, canonical_text}

WritePresetFile(package, path)
  -> ExportDocumentTransfer(package).dump() -> ParsePresetText (the reader accepts it)
  -> WritePresetText: temporary file -> QFile::rename (refuses an existing target)
```

**Primary failure call chain:**

```text
hostile file (link, 100 MiB, 1000 levels, UNC LUT path, unknown field, stale fingerprint)
  -> the first failed rule returns PresetFileReadResult{rule, message}; no exception leaves
  -> no package, no file written; the caller reports the message
```

**What was proven (executed tests, `build/debug`):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `WrittenPresetReadsBackWithEqualFingerprint`, `WriteRefusesToReplaceAnExistingFile` | `PresetFileTest` | PASS |
| `PresetAboveSizeLimitIsRefusedBeforeParse`, `PresetDeeperThanDepthLimitIsRefused`, `InvalidUtf8TextIsRefused` | `PresetFileTest` | PASS |
| `PresetWithoutFingerprintIsRefused`, `NonCanonicalPresetIsRefused` | `PresetFileTest` | PASS |
| `PresetWithUnknownFieldIsRefused`, `PresetWithUnknownAdjustmentTypeIsRefused` | `PresetFileTest` | PASS |
| `PresetWithAbsoluteLutPathIsRefused`, `PresetWithUncLutPathIsRefused`, `PresetWithParentPathLutReferenceIsRefused`, `PresetWithLensDatabasePathIsRefused`, `PresetWithForeignLensDatabasePathIsRefusedAndBuiltInPathIsAccepted`, `PresetWithLibraryOrOfficialLutReferenceIsAccepted` | `PresetFileTest` | PASS |
| `PresetWithNonFiniteNumberIsRefused` (refused as `invalid JSON` by the parser), `PresetWithNumberAboveFloatRangeIsRefused`, `PresetAboveGradeCountLimitIsRefused`, `PresetWithLongStringIsRefused` | `PresetFileTest` | PASS |
| `SymbolicLinkPresetIsRefused` | `PresetFileTest` | SKIPPED: this Windows account cannot make a symbolic link |
| `JunctionPresetIsRefused`, `FolderIsNotARegularPresetFile`, `PresetNameRulesRefuseFileSystemNames` | `PresetFileTest` | PASS |
| `PresetSelectionOmitsGeometryAndLensCorrection` | `PresetFileTest` | PASS |
| `MutatedPresetsNeverCrashReaderOrPastePlanner` (10000 cases, half with a valid fingerprint; 173 s in debug) | `PresetFileTest` | PASS |
| `CurveModelTest.LoadRefusesCurvesThatTheSplineSolverRefuses` (#1095), `DrtParamsModelLoad.PeakLuminanceOutsideTheDisplayRangeFailsTheLoadAndKeepsTheValue` (#1094), `RawWhiteBalanceWhiteClampsTheCustomWhiteAsTheColorTransformDoes` (#1096) | `GpuDagModelGraphTest` | PASS |

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target PresetFileTest GpuDagModelGraphTest DocumentTransferTest AdjustmentTransferCatalogTest EditorParameterCatalogTest EditorPipelineCommandServiceTest PipelineHistoryApplierTest alcedo_main --parallel 8"`, then each test binary from its `_runtime` folder.
Suite totals: `PresetFileTest` 24 passed, 1 skipped; `GpuDagModelGraphTest` 213/213;
`AdjustmentTransferCatalogTest` 13/13; `EditorParameterCatalogTest` 52/52;
`EditorPipelineCommandServiceTest` 24/24; `PipelineHistoryApplierTest` 15/15;
`DocumentTransferTest` 36/37: `TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches` fails
before this change too (the Color Wheel `version` key, #484 and #554).

**AddressSanitizer:** `win_debug_asan` (`build/debug-asan`), target `PresetFileTest`
only: 24 passed, 1 skipped (`SymbolicLinkPresetIsRefused`), no AddressSanitizer report;
the run with `MutatedPresetsNeverCrashReaderOrPastePlanner` took 15.6 minutes.

**LOC note:** `preset_file.cpp` 715, `preset_file.hpp` 159, `preset_file_test.cpp` about 530; the
loader fixes add about 110 lines in `edit/`.

**Remaining gaps:** tone values without an upper bound (#1097, see the audit).

##### PS1B completion record (2026-11-24)

**Status:** complete.

**Primary success call chain:**

```text
ApplicationModuleHost -> PresetLibraryService::Start -> Submit(kScan)
  -> worker: ScanPresetLibraryRoot(worker_root_) -> ReadPresetFile for each file
  -> owner: Publish -> inventory_ -> PruneCache -> InventoryChanged, OperationFinished

PresetLibraryService::Save(package, name)
  -> owner: PresetNameError -> ReplaceLutFilePaths (lut_reference_for_path ->
     LutLibraryService::ReferenceForPath) -> new fingerprint -> CheckPresetReferences
  -> worker: FreePresetName (" 2", " 3") -> WritePresetFile -> scan
  -> owner: Publish {affected_ids: [new id]}

PresetLibraryService::PackageFor(id)
  -> listed entry -> file size and write time equal to the cached read -> cached package
  -> else ReadPresetFile -> LRU front (64 packages)

Settings > LUTs and Color Management > Presets
  -> PresetSettingsSection: FolderDialog -> presetLibrary.useRoot(url)
  -> worker: is_directory -> scan -> SaveRoot (preference) -> worker_root_
  -> owner: root_, inventory_, cache cleared -> RootChanged, InventoryChanged
```

**Primary failure call chain:**

```text
Save of a LUT outside the LUT library / an invalid name
  -> Refuse on the owner thread: kRefused / kInvalidRequest, lastError, no worker task, no file
Add of a hostile file -> ReadPresetFile fails -> not written; result kRefused lists it
UseRoot of a missing folder -> kRootUnavailable; root, preference, inventory unchanged
Rename to an existing name -> QFile::rename refuses -> kConflict; both files unchanged
```

**What was proven (executed tests, `build/debug`):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `ScanListsGroupsAndMarksBadFilesNotAvailable`, `ScanStopsAtFileLimitWithError` | `PresetLibraryServiceTest` | PASS |
| `PackageForReadsFileOnceUntilFileChanges`, `PackageForRefusesIdThatTheInventoryDoesNotList`, `PackageCacheKeepsMostRecentlyUsedPackages` | `PresetLibraryServiceTest` | PASS |
| `AddCopiesValidFileAndRefusesBadFile`, `SaveNeverOverwrites`, `SaveRefusesInvalidName` | `PresetLibraryServiceTest` | PASS |
| `SaveWritesLibraryReferenceForLutLibraryFile`, `SaveRefusesLutOutsideLibrary` | `PresetLibraryServiceTest` | PASS |
| `RenameAndDeleteUpdateInventory`, `OperationsRunInRequestOrder` | `PresetLibraryServiceTest` | PASS |
| `UseRootScansNewFolderAndStoresPreference`, `UseRootOfMissingFolderKeepsCurrentFolder` | `PresetLibraryServiceTest` | PASS |
| `alcedo_main` links with the service; `PresetSettingsSection.qml` compiles (qmlcachegen) | `alcedo_main` | PASS |

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target PresetLibraryServiceTest alcedo_main --parallel 8"`, then `PresetLibraryServiceTest.exe`.
Suite totals: `PresetLibraryServiceTest` 14/14.

**Manual verification:** the Presets tile in Settings is pure UI; the user checks Browse, Rescan,
and the error badge in the application.

**LOC note:** `preset_library_service.cpp` 490, `preset_library_service.hpp` 258,
`preset_library_scan.cpp` 126, `preset_library_service_test.cpp` 369.

**Remaining gaps:** none for PS1B. PS2 adds the item summary of an entry (see the decisions).

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

**Expected diff.** 1500 to 1900 lines. Actual: about 2500 lines (1750 production, 470 tests,
the rest plan, `DESIGN.md`, and `.ts` entries), so the phase was split into PS2A (the C++ owners
and `PresetControllerTest`) and PS2B (the QML, the theme tokens, the translations), one pull
request each (section 8).

**Implementation decisions** (detail that this plan did not specify; each agrees with D1 to D9).

| Item | Decision | Reason |
| --- | --- | --- |
| Item summary | `SummarizeTransferPackage(package)` in `adjustment_transfer_catalog` gives the node and item names of the Copy dialog from the package alone; `PresetListModel` makes one line per node. The summary role reads the package through `PackageFor` when a tooltip opens. | A preset package has no source document, so the dialog rows cannot be used. No file read for tiles that are not hovered. |
| Name of a saved preset | `PresetNameFromLabel` makes the Version name a valid preset name: each refused character becomes `-`, spaces and trailing dots are removed, the length is cut to 120, empty is `Preset`. A name that the user types (Rename) is still refused, not changed (3.5). | A Version name such as `Warm/Cool` must not refuse a save by drag. |
| Name of a one-time Version | The file name without the extension. | D7 for a preset that has no entry. |
| Splitter height | `PresetController.browserHeight`, stored in `presets/browserHeight` (QSettings). 0 means `editorPresetPaneHeight`. | The QML `Settings` type needs a plugin of the static Qt build; the controller owns the other preset state already. |
| Preset part | `EditorPresetsSection.qml` holds the header, the error badge, the browser, the tile menu, the file dialogs, and the file drop. `EditorVersionsPanel.qml` holds the splitter and the drag between the parts. | `EditorVersionsPanel.qml` is above 900 lines; the drag needs both parts, the rest does not. |
| Drag | A panel-level drag in the pattern of `CollectionsPanel.qml`: the row or tile `MouseArea` starts it after `Qt.styleHints.startDragDistance` and reports scene positions; the panel tests the drop part on release. The rows keep `preventStealing`, so a drag does not flick the list. | No system drag object is needed inside one panel. Risk row of section 12. |
| Agent control | An apply is refused by `EditorSessionController::PasteAdjustmentPackage`; save, add, rename, and delete change preset files only and stay available. | The policy protects project state. |
| Rename after a save | Only a save by drag from the panel puts the new tile into rename. A save from the Copy dialog reports through the snackbar. | The dialog closes; a rename field in a closed rail page is not visible. |
| Plain text | `AlertBadge` shows its text as plain text for all callers. Preset tiles use their own `ToolTip` with a plain-text content item, not the shared attached tooltip. | 3.5. |

##### PS2 completion record (2026-11-24)

**Status:** complete for the code and the tests; the check of the five actions of 2.2 in the
running application is the user's (pure UI).

**Primary success call chain:**

```text
Drag a Version row onto the preset part
  -> EditorVersionsPanel.finishDrag -> PresetController::SaveVersionAsPreset(versionId)
  -> EditorSessionController::SnapshotHistorySource -> AdjustmentTransferCatalogService::ReadVersion
  -> SelectPresetItems -> AdjustmentTransferPackageBuilder::Build
  -> PresetNameFromLabel -> PresetLibraryService::Save -> worker: WritePresetFile -> scan
  -> owner: InventoryChanged (PresetListModel reset) -> OperationFinished
  -> PresetController::OperationFinished {save, presetIds} -> the new tile enters rename

Double click / Enter / drag a preset tile onto the Versions list
  -> PresetController::ApplyToEditor(id) -> PresetLibraryService::FindEntry, PackageFor
  -> EditorSessionController::PasteAdjustmentPackage(package, entry name)
  -> (current chain) ApplyPaste -> PasteLiveRootRelativeVersion -> new active Version

Drop one .alcpreset on the Versions list / Apply Preset File...
  -> PresetController::ApplyFileToEditor(url) -> ReadPresetFile -> PasteAdjustmentPackage

Copy dialog Save as Preset
  -> AppDialogs -> PresetController::SaveDialogSelectionAsPreset
  -> AdjustmentTransferDialogModel::BuildPackage -> PresetLibraryService::Save -> snackbar
```

**Primary failure call chain:**

```text
Preset not available / not listed / hostile one-time file
  -> FindEntry, entry.available, or ReadPresetFile refuses -> {success:false, message}
  -> no Paste starts; the commit graph, the Versions, and the active Version are unchanged
Agent holds control -> PasteAdjustmentPackage refuses with the policy message; no history change
Save refused (LUT outside the library, invalid name) -> PresetLibraryService::Refuse
  -> {success:false, message} -> the error badge of the preset part; no file
```

**What was proven (executed tests, `build/debug`):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `SaveVersionAsPresetWritesAllItemsExceptGeometryAndLens` | `PresetControllerTest` | PASS |
| `SaveVersionAsPresetReadsVersionThatIsNotActive` | `PresetControllerTest` | PASS |
| `ApplyToEditorCreatesActiveVersionNamedAfterPreset` | `PresetControllerTest` | PASS |
| `ApplyFileToEditorLeavesLibraryUnchanged` | `PresetControllerTest` | PASS |
| `ApplyOfUnavailablePresetChangesNoHistory` (an unavailable entry, an unlisted id, a one-time file with an absolute LUT path) | `PresetControllerTest` | PASS |
| `ApplyIsRefusedWhileAgentHoldsControl` | `PresetControllerTest` | PASS |
| `SaveDialogSelectionSavesOnlyTheSelectedItems` (step 6) | `PresetControllerTest` | PASS |
| `ListModelShowsFolderPresetsBeforeGroupsWithPlainSummary` | `PresetControllerTest` | PASS |
| `PresetNameFromLabelTest.LabelBecomesValidPresetName` | `PresetControllerTest` | PASS |
| `alcedo_main` links; the new and changed QML compiles (qmlcachegen) | `alcedo_main` | PASS |
| Paste of the Copy dialog (caller of the shared dialog model) | `AdjustmentTransferControllerTest` | PASS |
| Version card click, rename, draft, and scroll with the new card input and the preset part loaded; no QML error from the new files in the log | `EditorVersionsPanelQmlTest` | PASS |

The editor session of these tests is an `EditorSessionController` over a backend with a real
`CommitGraph`; its Paste is `AdjustmentTransferService::PasteAsRootRelativeVersion`, the operation
that the editor history port runs.

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target PresetControllerTest AdjustmentTransferControllerTest EditorVersionsPanelQmlTest alcedo_main --parallel 8"`,
then each test binary from its `_runtime` folder.
Suite totals: `PresetControllerTest` 9/9, `AdjustmentTransferControllerTest` 4/4,
`EditorVersionsPanelQmlTest` 20/20. The QML panel tests do not fail on a QML binding error
(#526), so the log was searched for errors of the new files: none.

**Checklist / exit condition:**

- [x] Each test above passes.
- [ ] The user confirms the five actions of 2.2 in the running application.
- [x] Each preset string in QML uses plain text format.

**LOC note:** `preset_controller.cpp` 314, `preset_controller.hpp` 126, `preset_list_model.cpp`
122, `PresetBrowser.qml` 238, `PresetTile.qml` 219, `EditorPresetsSection.qml` 274,
`EditorVersionsPanel.qml` 1218 (+274: drag, splitter, drop target; above 1000 lines before this
phase too, the preset part went to its own file), `preset_controller_test.cpp` 465.

**Remaining gaps:** the QML drag, splitter, and drop have no automated test (pure UI, plan
section 11).

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

- [ ] Each test above passes with the render through Auto to OpenCL. The tests pass; they render
      through the recording image port of the session tests, not through a GPU (completion record).
- [ ] The user confirms the previews in the running application.
- [x] The LUT preview tests still pass (`LutLibraryModelTest`).

**Expected diff.** 1100 to 1500 lines. Actual: about 2200 inserted and 640 deleted lines. About
600 of the inserted lines are the queue, LRU, and display code of `LutPreviewController`, moved
into the shared base class (step 3); the net is about 1550 lines (1000 production, 600 tests), so
the phase stays one pull request.

**Implementation decisions** (detail that this plan did not specify; each agrees with D1 to D9).

| Item | Decision | Reason |
| --- | --- | --- |
| Shared preview code | `BrowserPreviewController` (`album_backend`, a `QObject` base class) owns the watch counts, the request queue, the render in flight, the LRU of 200 images, the failed keys, the shown display images, and the display destination. `LutPreviewController` and `PresetPreviewController` derive from it and give only the image id, the entry key, the viewport state, and the render submission. The LUT controller keeps its public API, so `LutPreviewControllerTest` is unchanged except for type names. | Step 3: one copy of the queue and LRU code. The base class has owned state; the derived classes have no access to it. |
| Image store | `LutPreviewImageStore` became `BrowserPreviewImageStore` with a provider id (`browser_preview_image_provider.{hpp,cpp}`). The application registers one `BrowserPreviewImageProvider` for `alcedo-lut-preview` and one for `alcedo-preset-preview`. | Step 4: the display ICC conversion is the same code for both. |
| Render size and encoding | `MakeLutPreviewRenderRequest`, `LutPreviewOutputColor`, and `kLutPreviewLongEdge` moved to `app/browser_preview_render` as `MakeBrowserPreviewRenderRequest`, `BrowserPreviewOutputColor`, and `kBrowserPreviewLongEdge`. The library `LutPreviewImage` became `BrowserPreviewImage` (LUT document, preset document, render request). | Step 2: the shared name when both use it. |
| Session result | `EditorLutPreviewStatus`, `EditorLutPreviewResult`, and `EditorLutPreviewCompletion` became `EditorBrowserPreview*`. The result has `sensor_key`, set for a preset preview. `RenderLutPreview` and `RenderPresetPreview` share `EditorSessionService::RenderBrowserPreviewOnOwner`; the history port shares `BuildRootPreviewDocument`. | One owner path for both previews; the checks and messages stay the same. |
| Sensor key | `PresetPreviewSensorKey(document)`: the XXH3 hash of the Develop parameters JSON without the seven white-balance fields, that is, exactly the fields that `UseSensorSettingsFrom` takes from the current Version. The controller reads it from `EditorSessionController::pipeline_document()`, once per published document, and refreshes on `StateChanged`. | A content key, not the field revisions: a checkout of another Version with equal sensor settings keeps the previews. |
| Rendered sensor key | The session computes the sensor key of the document that it renders and returns it; the controller stores the pixels under that key (`RenderedEntryKey`) and shows them only when the tile key is equal. Interleaving: the GUI submits a sensor edit (session command 1), a tile is watched and the controller computes its key from the published document, which does not have the edit yet, and submits the preview (command 2); the owner reduces 1, then 2, so the preview has the new settings. Without the rendered key these pixels would stay under the key of the old settings and show after an Undo. `PreviewIsStoredUnderTheSensorKeyThatTheSessionRendered` drives this order. | The rule for consistency mechanisms in `AGENTS.md`: the interleaving is real and has a test. |
| Entry key | Preset id, fingerprint, and sensor key. The id is part of the key, so two files with equal content render twice. | A key without the id would need a refresh of every watched entry after each render. |
| Package read | `PackageFor` runs when the render starts, on the GUI thread, as an apply does. A file that fails a rule since the scan fails the preview of its key (no render); the tile shows the warning icon and stays in the list. | Plan 2.4 failure rule; the cache of 6.2 makes the second read free. |
| Failure icon | `PresetTile` shows the warning icon of an unavailable preset also when `previewFailed(id)` is true and no preview is painted. `previewFailed` is a new invokable of the base class; the LUT tiles do not use it. | Plan 2.4. No new theme token and no new string. |

##### PS3 completion record (2026-11-24)

**Status:** complete for the code and the tests; the preview images in the running application
are the user's check (pure UI), and no test renders a preset preview on a GPU.

**Primary success call chain:**

```text
PresetTile created or scrolled into view (QML)
  -> PresetPreviewController::watch(presetId)            [BrowserPreviewController]
  -> KeyFor = image id | preset id | fingerprint (PresetLibraryService::FindEntry)
             | PresetPreviewSensorKey(EditorSessionController::pipeline_document())
  -> LRU hit: Show -> BrowserPreviewImageStore::Put -> image://alcedo-preset-preview/<id>
  -> miss: Enqueue -> Pump (one in flight, not while renderBusy)
  -> PresetPreviewController::StartRender -> PresetLibraryService::PackageFor (cached package)
  -> IEditorSessionBackend::RenderPresetPreview {image id, package}
  -> owner: RenderBrowserPreviewOnOwner -> PresetPreviewSensorKey(CurrentPreview)
  -> EditorSessionHistoryPort::BuildPresetPreviewDocument -> BuildPresetPreviewDocument
       (root replay -> DocumentTransferPlanner::Plan -> ApplyPipelineEditBatch
        -> UseSensorSettingsFrom(current))
  -> IEditorImageRenderPort::ScheduleImages (MakeBrowserPreviewRenderRequest, 256 px)
  -> completion {Completed, image, sensor_key} -> queued to the GUI thread
  -> OnRendered: RenderedEntryKey -> CachePut -> Show -> revision -> tile Image
```

**Primary failure call chain:**

```text
Render failed / document refused / package fails a rule
  -> EditorBrowserPreviewResult{Failed} (or StartRender completes Failed without a render)
  -> OnRendered: failed key, no retry for this photo -> revision
  -> PresetTile: previewFailed -> warning icon; the preset stays listed and usable
Session not interactive / another image / command refused
  -> Rejected or StartRender false -> nothing stored; the next Refresh renders again
Photo switch -> Refresh: every preview hidden, queue and failed keys cleared
```

**What was proven (executed tests, `build/debug`):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `PreviewDocumentEqualsPasteResultExceptSensorSettings` (canonical JSON of the preview equals a real `PasteAsRootRelativeVersion` result with the same sensor settings; no commit) | `PresetPreviewImageTest` | PASS |
| `PreviewDocumentKeepsDemosaicAndDenoiseOfCurrentVersion` | `PresetPreviewImageTest` | PASS |
| `PreviewOfAnotherImageOrAnEmptyPresetIsRefused` | `PresetPreviewImageTest` | PASS |
| `SensorKeyFollowsSensorSettingsAndIgnoresWhiteBalanceAndLook` | `PresetPreviewImageTest` | PASS |
| `PreviewKeyChangesWithFingerprintImageAndSensorSettings` | `PresetPreviewControllerTest` | PASS |
| `OneRenderRunsAtATimeAndWaitsForViewport` | `PresetPreviewControllerTest` | PASS |
| `FailedPreviewIsNotRetriedAndTileStaysAvailable` (also: a package that fails a rule) | `PresetPreviewControllerTest` | PASS |
| `PreviewIsStoredUnderTheSensorKeyThatTheSessionRendered` | `PresetPreviewControllerTest` | PASS |
| `PresetWithoutFingerprintOrPhotoHasNoPreview` | `PresetPreviewControllerTest` | PASS |
| `PresetPreviewRendersThePresetOnTheRootWithTheWorkingSensorKey` (session and real history port) | `EditorComparisonServiceTest` | PASS |
| `PresetPreviewIsRefusedForAnotherImageOrWithoutAPackage` | `EditorComparisonServiceTest` | PASS |
| LUT previews on the shared base class (8 `LutPreviewControllerTest` cases) | `LutLibraryModelTest` | PASS |
| Preset commands (caller of the preset browser) | `PresetControllerTest` | PASS |
| Versions panel with the new tile bindings; no QML error of the preset files in the log | `EditorVersionsPanelQmlTest` | PASS |
| `alcedo_main` links; the changed QML compiles (qmlcachegen) | `alcedo_main` | PASS |

The session tests render through the recording image port of `EditorComparisonServiceTest`; they
check the document, the render request, and the result, not GPU pixels. The GPU path is the image
render port of the LUT previews, unchanged.

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target PresetPreviewImageTest PresetPreviewControllerTest EditorComparisonServiceTest LutLibraryModelTest PresetControllerTest EditorVersionsPanelQmlTest alcedo_main --parallel 8"`,
then each test binary from its `_runtime` folder (`EditorVersionsPanelQmlTest` with
`QT_QPA_PLATFORM=offscreen` and `QT_FORCE_STDERR_LOGGING=1`).
Suite totals: `PresetPreviewImageTest` 4/4, `PresetPreviewControllerTest` 5/5,
`EditorComparisonServiceTest` 29/29, `LutLibraryModelTest` 33/33, `PresetControllerTest` 9/9,
`EditorVersionsPanelQmlTest` 20/20. No full suite run.

**Checklist / exit condition:** the LUT tests pass; the GPU render of a preset preview and the
user check in the application are open (boxes above).

**LOC note:** `browser_preview_controller.cpp` 284, `browser_preview_controller.hpp` 195,
`preset_preview_controller.cpp` 178, `lut_preview_controller.cpp` 134 (was 349),
`preset_preview_image.cpp` 87, `PresetTile.qml` 299, `preset_preview_controller_test.cpp` 290,
`preset_preview_image_test.cpp` 202. `editor_session_service.cpp` is 4041 lines (above 1000 before
this phase); the two previews now share one owner function there instead of adding a copy.

**Remaining gaps:** the previews in the running application are not checked yet (pure UI, plan
section 11). PS4 gives the dialog a preview source for a library photo.

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

- [x] Each test above passes (PS4A and PS4B completion records).
- [x] Time record: `Apply Preset...` on 200 photos against `Paste Adjustments` of the same package
      on the same photos, release build. The preset time is not above the Paste time by more than
      5%.
- [ ] The user confirms the dialog in the running application.

**Expected diff.** 1400 to 1800 lines. With the tests, the shared test builders, the `.ts`
entries, and this record the estimate passed 2000 lines, so the phase has two pull requests
(section 8, issue #1109): PS4A (steps 1 to 3) and PS4B (step 4).

**Implementation decisions** (detail that this plan did not specify; each agrees with D1 to D9).

| Item | Decision | Reason |
| --- | --- | --- |
| Shared routing (step 2) | `AdjustmentTransferController::PastePackageToTargets(package, targetEntries, versionName)` holds the agent control refusal, the target collection, the editor-photo routing, and the start of the background task. `Paste` and `PresetController::ApplyToTargets`/`ApplyFileToTargets` call it. `PresetController::SetAdjustmentTransfer` gives the preset controller its owner. | One function for both controllers; the coordinator keeps its rule that a target is never open in the editor. |
| Result report | The result of the library targets is `AdjustmentTransferController::PasteFinished`, which the snackbar of `AppDialogs` already shows. The dialog closes when the apply starts and shows the start message. | Plan 2.3: the result report is the current one. |
| Refusal in the dialog | A preset that is not available, not listed, or a file that fails a rule keeps the dialog open with the reason in an `AlertBadge`. `Apply` is enabled only for an available tile; a double click on a tile is `Apply`. | Plan 2.6. |
| Policy | `requestApplyPreset()` refuses with the Paste reason (`canPasteAdjustments`), as `requestPasteAdjustments()` does; the menu item needs targets only. | The apply is a Paste. |
| Thumbnail operation (step 3) | `ThumbnailService::RenderPreview(ThumbnailPreviewRequest)`: the caller document builder runs on the lookup worker with the committed snapshot; the decode and render stages and the batch executors are the ones of a thumbnail at the 256 tier (EIGHTH decode). The pixels (RGBA8) go to the caller only: no guard enters the memory cache, the request has no disk key, and `CancelPending`/`InvalidateThumbnail` do not end it. | Risk row of section 12 resolved: the service renders a caller document without a cache write. |
| Output color | A `THUMBNAIL` render now takes `TaskOptions::export_output_color_` (empty for every thumbnail), so a preview of an HDR document renders with the SDR encoding of `BrowserPreviewOutputColor`. | A tile is an 8-bit SDR image, as for the editor previews. |
| Library preview source | `LibraryPresetPreviewSource` (`album_backend`) implements `PresetPreviewSource` with `ShowLibraryPhoto(element, image)`. A second `PresetPreviewController` owns it (`SetOwnedSource`) and is `appModules.presetDialogPreview`; its images come from the provider `image://alcedo-preset-dialog-preview/`, because the display ids of two controllers are independent. `PresetPreviewController::showLibraryPhoto` hands the photo to the source. | Step 3: the same controller code for the editor and the dialog. |
| Sensor key of a library photo | `ShowLibraryPhoto` reads the committed snapshot of the photo (`AcquireCommittedSnapshot`) on the GUI thread one time, when the dialog opens; the render reads it again on the worker and returns the sensor key of what it rendered (the PS3 rule). | The entry key needs the sensor key at once; one storage read per dialog open. |
| Viewport | The library source reports no viewport render: the thumbnail executors render beside the editor, as thumbnails do. | The rule of 2.4 protects the editor executor, which these renders do not use. |
| First target in the editor | The dialog uses the editor previews (`appModules.presetPreview`) when the editor holds the first target: `LoadHistorySnapshot` refuses a photo that the editor holds. | The editor session owns the history of that photo. |
| Tiles | The dialog creates its `PresetBrowser` in a `Loader` that is active only while the dialog is open, so the tiles watch previews only then. The dialog scans the folder when it opens (D9). | No render for a closed dialog. |

**Problems found:** #1110 (the preview LRU key has the image id only; image ids repeat across
projects), #1111 (the time of a multi-target Paste grows with each Version of the photos).

##### PS4A completion record (2026-11-24)

**Status:** complete for the code, the tests, and the time record; the dialog in the running
application is the user's check (pure UI).

**Primary success call chain:**

```text
Image context menu Apply Preset... (AppDialogs, action apply-preset)
  -> ImageActionsController.requestApplyPreset (canPasteAdjustments, targets of Paste)
  -> ApplyPresetDialog.open: PresetController::Refresh; first target not in the editor:
     PresetPreviewController::showLibraryPhoto (appModules.presetDialogPreview)
       -> LibraryPresetPreviewSource::ShowLibraryPhoto -> AcquireCommittedSnapshot -> sensor key
  -> PresetTile watch -> BrowserPreviewController::Pump -> PresetPreviewController::StartRender
       -> PresetLibraryService::PackageFor -> LibraryPresetPreviewSource::RenderPresetPreview
       -> ThumbnailService::RenderPreview: lookup worker: LoadHistorySnapshot
          -> BuildPresetPreviewDocument(root, package, committed) -> BrowserPreviewOutputColor
       -> decode (EIGHTH) -> batch executor render (THUMBNAIL, output color) -> RGBA8 pixels
       -> RGBA32F browser result (rendered sensor key) -> GUI thread: LRU -> Show
       -> image://alcedo-preset-dialog-preview/<id>
  -> Apply / double click: PresetController::ApplyToTargets(presetId, targets)
       -> ListedPackage: FindEntry, available, PackageFor (one read for all targets)
       -> AdjustmentTransferController::PastePackageToTargets(package, targets, preset name)
       -> photo open in the editor: EditorSessionController::PasteAdjustmentPackage
       -> other photos: AdjustmentTransferApplyCoordinator::StartApplyToTargets
          -> worker: LoadHistorySnapshot -> PasteAsRootRelativeVersion -> PersistHistory
          -> GUI thread: FinishApply (thumbnail refresh) -> PasteFinished -> snackbar
  -> Open File...: PresetController::ApplyFileToTargets -> ReadPresetFile -> the same Paste
```

**Primary failure call chain:**

```text
Preset not listed / not available / file fails a rule / agent holds control
  -> ListedPackage, ReadPresetFile, or UserStateChangeRefusal refuses -> {success:false, message}
  -> the dialog stays open with the reason (AlertBadge); no Paste starts, no history changes
One target cannot be pasted (no history, unknown photo)
  -> coordinator: failure row {elementId, message}; the other targets are persisted
  -> PasteFinished {appliedIds, failures} -> snackbar
Preview render fails (no RAW file, document refused)
  -> ThumbnailPreviewResult kError -> EditorBrowserPreviewResult Failed -> failed key
  -> PresetTile: warning icon; the preset stays usable
```

**What was proven (executed tests, `build/debug` unless stated):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `ApplyToTargetsCreatesOneVersionPerTargetNamedAfterPreset` | `PresetApplyTargetsTest` | PASS |
| `ApplyToTargetsReadsPresetFileOnce` (20 targets, one file read) | `PresetApplyTargetsTest` | PASS |
| `ApplyToTargetsRoutesOpenPhotoThroughEditorSession` (the stored history of the open photo does not change) | `PresetApplyTargetsTest` | PASS |
| `OneFailedTargetIsReportedAndOtherTargetsApply` | `PresetApplyTargetsTest` | PASS |
| `ApplyFileToTargetsLeavesLibraryUnchanged` | `PresetApplyTargetsTest` | PASS |
| `ApplyOfUnavailablePresetStartsNoPaste` | `PresetApplyTargetsTest` | PASS |
| `DialogPreviewRendersPresetOnLibraryPhoto` (GPU render of the sample linear DNG through the thumbnail executors, Auto to OpenCL; the editor previews render nothing) | `PresetApplyTargetsTest` | PASS |
| `LibraryPreviewRenderDoesNotWriteThumbnailCache` (same memory guard after the preview; a reopened service has the thumbnail disk entry only; the +2 EV preview differs from the thumbnail by a mean of 51.8 levels) | `ThumbnailCommittedRenderTest` | PASS |
| Thumbnail renders after the scheduler change (all 13 cases) | `ThumbnailCommittedRenderTest` | PASS |
| Preset commands with the shared test builders | `PresetControllerTest` | PASS (9/9) |
| Preset previews of the editor | `PresetPreviewControllerTest` | PASS (5/5) |
| Paste Adjustments over `PastePackageToTargets` | `AdjustmentTransferControllerTest` | PASS (4/4) |
| `alcedo_main` links; the new and changed QML compiles (qmlcachegen) | `alcedo_main` | PASS |
| Time record, release build: `DISABLED_RecordApplyPresetTimeAgainstPasteFor200Photos` | `PresetApplyTargetsTest` (`build/release`) | Paste 10285 ms, Apply Preset 9996 ms (means of the runs Paste, Preset, Preset, Paste); ratio 0.97 |

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target ThumbnailCommittedRenderTest PresetControllerTest PresetApplyTargetsTest PresetPreviewControllerTest AdjustmentTransferControllerTest --parallel 8"`,
`... --target alcedo_main`, then each test binary from its `_runtime` folder with
`QT_QPA_PLATFORM=offscreen`. Time record: `build/release` configured with
`-DALCEDO_BUILD_TESTS=ON`, `--target PresetApplyTargetsTest`, then
`PresetApplyTargetsTest.exe --gtest_also_run_disabled_tests --gtest_filter=*RecordApplyPresetTime*`.
Suite totals: `PresetApplyTargetsTest` 7/7, `ThumbnailCommittedRenderTest` 13/13,
`PresetControllerTest` 9/9, `PresetPreviewControllerTest` 5/5,
`AdjustmentTransferControllerTest` 4/4. No full suite run.

The first time record ran Paste before Preset in each round and gave a ratio of 1.26: each run
adds a Version to each photo, and a run takes about 3 s longer per Version (#1111). The record
now orders the runs so that both paths have the same mean Version count.

**Checklist / exit condition (PS4A part):**

- [x] Each test of PS4A above passes (`PresetsApplyCommandStoresAgentAttribution` is PS4B).
- [x] Time record: Apply Preset is not above Paste by more than 5% (ratio 0.97).
- [ ] The user confirms the dialog in the running application.

**LOC note:** `ApplyPresetDialog.qml` 327, `library_preset_preview_source.cpp` 151,
`library_preset_preview_source.hpp` 86, `preset_controller.cpp` 367, `thumbnail_service.cpp`
1079 (was 989; above 1000 lines after this phase: the preview operation reuses the stages of the
file instead of a copy; a split of the rendition stages into their own file is a refactor of
its own), `preset_apply_targets_test.cpp` 336, `preset_test_support.hpp` 153.

**Remaining gaps:** the dialog and the context menu entry have no automated test (pure UI, plan
section 11).

**PS4B implementation decisions** (detail that step 4 did not specify).

| Item | Decision | Reason |
| --- | --- | --- |
| Shared agent Paste | `automation_agent_paste.{hpp,cpp}` holds the target resolution (`element_ids` or the library selection), the Version name rule, the result schema, and `StartAgentPaste` (editor photo through the session, the other photos in the AdjustmentPaste task, agent attribution). `adjustments.paste` and `presets.apply` call it. | One agent Paste, as `PastePackageToTargets` is one GUI Paste. |
| Package lifetime | The agent Paste holds the package as `shared_ptr<const AdjustmentTransferPackage>`; the `adjustments.copy` store holds the same type. | The cache package of `PackageFor` and a copied package go through one path without a copy. |
| `presets.list` | Scans the folder (`Refresh`) and answers with the inventory of that scan, in the order of `PresetListModel`; `details` adds the node and item names of `SummarizeTransferPackage`. Needs no project. | D9: no file watcher; the browser scans when it opens. |
| `presets.save` | Reads the source as Copy Adjustments does (`ReadSourceHistory`: the editor session for the open photo), takes the items of a save by drag (`SelectPresetItems`, D4), and answers when the worker wrote the file. `PresetLibraryService` numbers each queued operation (`LastSubmittedRequest`, `OperationResult::request`), so the answer is the result of its own save when a GUI operation waits before it. | One worker queue for the GUI and the agent. |
| `presets.apply` | `preset_id` (the published inventory, `PackageFor`) or `file` (`ReadPresetFile` one time, R8). The default Version name is the preset name or the file stem. | Step 4; the routing of PS4A. |
| Errors | Unknown id: -32602 `/preset_id`. A preset that is not available or a file that fails a rule: -32002 with the reason. Both or neither source: -32602. | The error classes of `adjustments.paste`. |

##### PS4B completion record (2026-11-24)

**Status:** complete. The alcedo-skills reference is zidage/alcedo-skills#31.

**Primary success call chain:**

```text
alcedo-cli call presets.apply {preset_id | file, element_ids?, version_name?, note?}
  -> AutomationCommandRegistry (changes_state: agent control in a GUI session)
  -> preset_commands: AutomationEnteredProject -> ReadAppliedPreset
       -> preset_id: FindEntry, available, PresetLibraryService::PackageFor (one read)
       -> file: ReadPresetFile (the folder and the cache do not change)
  -> AgentPasteVersionName -> AgentPasteTargets (element_ids or the library selection)
  -> StartAgentPaste(package, ids, name, {author agent, note})
       -> photo open in the editor: EditorSessionController::RequestPasteAdjustmentPackage
          -> OperationWait (after the save checkpoint) -> answer.editor
       -> other photos: AdjustmentTransferApplyCoordinator::StartApplyToTargets
          -> {task_id, target_count} -> tasks.wait -> {applied_ids, failures}
alcedo-cli call presets.save {element_id, version_id?, name?}
  -> ReadSourceHistory -> RequestedVersion -> ReadVersion -> SelectPresetItems -> Build
  -> PresetLibraryService::Save (request n) -> worker WritePresetFile -> scan
  -> OperationFinished with LastResult().request == n -> {preset_id, name, nodes}
alcedo-cli call presets.list {details?}
  -> PresetLibraryService::Refresh (request n) -> worker scan -> OperationFinished (request n)
  -> {folder, presets[folder first, then groups], error?}
```

**Primary failure call chain:**

```text
Unknown preset id / both or neither source / bad name / unknown photo
  -> -32602 with the pointer; no Paste starts, no file is written
Preset not available / file fails a rule (a LUT outside the library) / nothing to save
  -> -32002 with the reason; the history of each photo does not change
Another Paste or an editor save runs, or the policy refuses Paste
  -> -32005 with the reason; nothing starts
No project / no element_ids in the headless host
  -> -32001
One photo fails in the background task
  -> failure row in the task result; the other photos get their Versions
```

**What was proven (executed tests, `build/debug`):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `PresetsApplyCommandStoresAgentAttribution` (3 photos; each head commit has the author `agent` and the note; the Version is named after the preset) | `AutomationPresetCommandsTest` | PASS |
| `PresetsListShowsFolderPresetsBeforeGroupsWithReasons` | `AutomationPresetCommandsTest` | PASS |
| `PresetsListScansFolderForFilesAddedAfterStart` | `AutomationPresetCommandsTest` | PASS |
| `PresetsSaveAnswersItsOwnPresetWithoutGeometryAndLens` (a GUI save queued first; the file has no geometry and no lens correction; `/name` refusal) | `AutomationPresetCommandsTest` | PASS |
| `PresetsApplyFileLeavesLibraryUnchanged` (folder listing, inventory, and read count do not change) | `AutomationPresetCommandsTest` | PASS |
| `PresetsApplyRefusesUnknownPresetBothSourcesAndHostileFile` (no commit, no Paste) | `AutomationPresetCommandsTest` | PASS |
| `PresetsSaveAndApplyWithoutProjectAreNotReady` | `AutomationPresetCommandsTest` | PASS |
| `adjustments.paste` over the shared agent Paste | `AutomationAdjustmentCommandsTest` | PASS (4/4) |
| Request numbers of the service | `PresetLibraryServiceTest` | PASS (14/14) |
| Callers of PS4A | `PresetControllerTest`, `PresetApplyTargetsTest` | PASS (9/9, 7/7) |
| The three schemas in the skills reference; the `adjustments.paste` schema does not change | `AutomationReferenceDocsTest` output + `generate_cli_reference.py --check` | the `presets.*` blocks pass; `labels.*` fails as on alcedo-skills `main` (alcedo-skills#24) |
| The reference examples | `alcedo-cli` against `alcedo_main --headless`, `build/release` | save, list, list with details, apply with `tasks.wait`, and the refusal of an id without `.alcpreset` |

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target AutomationPresetCommandsTest AutomationAdjustmentCommandsTest AutomationReferenceDocsTest PresetLibraryServiceTest PresetControllerTest PresetApplyTargetsTest"`,
then each test binary from its `_runtime` folder;
`python scripts/automation/generate_cli_reference.py --skills-dir <alcedo-skills branch> --check`.
Suite totals: `AutomationPresetCommandsTest` 7/7, `AutomationAdjustmentCommandsTest` 4/4,
`PresetLibraryServiceTest` 14/14, `PresetControllerTest` 9/9, `PresetApplyTargetsTest` 7/7. No
full suite run.

**Checklist / exit condition:** step 4 done; the PS4 tests pass. The user check of the dialog
(PS4A) is open.

**LOC note:** diff 1226 insertions and 210 deletions in 13 files with this record.
`preset_commands.cpp` 428, `automation_agent_paste.cpp` 244, `adjustment_commands.cpp` 591 (was
736; the agent Paste moved to `automation_agent_paste.cpp`), `automation_preset_commands_test.cpp`
306.

**Remaining gaps:** none for PS4B.

---

### Follow-up: Version context menu and preset tooltip (issue #1124)

Status: implemented 2026-11-24 on `feature/version-context-menu-export`.

| Item | Decision | Reason |
| --- | --- | --- |
| Preset tooltip | An available preset has no tooltip; the item summary is not shown. An unavailable preset keeps its tooltip with the reason. The `summary` role of `PresetListModel` and `PresetPackageSummaryText` are removed. | User request. The warning icon of an unavailable preset has no other text. |
| Version card | The Rename and Remove buttons are removed. A right click opens `editorVersionMenu`: `Rename Version`, `Export Version...`, `Remove Version`, with the old button conditions. Rename and Export start in the menu `onClosed`. | User request. The menu gives focus back on close (same cause as #1120). |
| Export Version | `PresetController::ExportVersion` builds the package of a drag-saved preset (D4), opens `QFileDialog::getSaveFileName`, and queues `PresetLibraryService::Export`. The file does not enter the preset list unless the chosen folder is the preset folder (then the folder is scanned). | "Export a Version" in the panel of presets: a shareable `.alcpreset` file. |
| Default file name | `<photo file name without extension>_<Version name>.alcpreset`, made valid with `PresetNameFromLabel` (`VersionExportFileName`). The dialog opens in the folder of the last export (`presets/exportFolder`), else Documents. | User request. |
| Replace | `PresetFileWriteMode::kReplace`: the temporary file is renamed over an existing regular file. The save dialog has asked before. Save and Add keep `kCreateNew`. | A confirmed overwrite must not fail with "exists". |
| Drop guide text | The band of the Versions list says `Drag here to apply this preset` (拖到这里，使用该预设), the expanded overlay `Release to apply this preset`. | User request. |

Tests: `PresetControllerTest.ExportVersionWritesPresetFileAtChosenPathAndReplacesIt`,
`ExportVersionCanceledInDialogWritesNothing`, `VersionExportFileNameTest`, and the Versions panel
QML tests that use the card menu instead of the buttons.

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
