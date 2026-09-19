# Configurable Keyboard Shortcut Registry Plan

Date: 2026-09-19  
Status: Planned  
Source revision: `796f1f90`  
Primary area: Alcedo Studio UI  
Affected areas: UI input routing, settings persistence, editor history, Library selection,
filmstrip navigation, Versions, LUT, mask editing, and Nodes selection

Related plans:

- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Background Tasks and Declarative UI State Plan](background_tasks_ui_state_plan.md)
- [Editor History Durability and Version Transfer Design](editor_history_durability_and_version_transfer_design.md)
- [Phase 7A History and Versions Repair and UI Refactor Plan](phase_7a_history_versions_repair_and_ui_refactor_plan.md)

## 1. Outcome

Alcedo Studio will have one named keyboard shortcut registry for basic UI commands. The registry
will own default bindings, current user bindings, input type, scope metadata, conflict validation,
display text, and persistence. UI components will keep ownership of the actions that the bindings
invoke.

The first release will cover these product operations:

1. Left and Right switch the active image from the editor filmstrip.
2. Ctrl+Z runs Undo. Ctrl+R runs Redo.
3. Ctrl+A selects all images in the active Library album.
4. Shift extends Library selection. The existing Ctrl toggle behavior remains available.
5. Ctrl+A in the focused Versions panel creates a default version from the root.
6. Ctrl+S in Library saves the project.
7. Ctrl+S in Editor saves the current image into the project.
8. Up and Down in the focused LUT panel select the previous or next usable LUT.
9. The current mask-edit Escape, Delete, Return, and Enter behavior uses the same registry.
10. Delete in the focused Nodes panel deletes the selected Color Grade nodes.
11. Shift in the Nodes panel extends or reduces the node selection.

This work does not add workflow shortcuts. It does not add macros, command sequences, global
operating-system shortcuts, or user-authored automation.

## 2. Decision Record

### 2.1 Accepted product requirements

- The settings dialog gets one Keyboard category.
- Each row shows the current binding.
- A pointer click on the binding field starts capture.
- Enter or Escape saves the current candidate and leaves capture. The capture field never records
  either key.
- Plain Delete clears the candidate while capture is active. The user then presses Enter or Escape
  to save the unassigned state.
- The registry accepts one key chord or one modifier-only input per captured binding.
- Shift must be a first-class modifier-only binding because Library and Nodes use it during pointer
  selection.
- A custom binding replaces the complete default binding set for that command. Restore Default
  restores all default aliases.
- The user can clear a command. An empty saved value is different from a missing setting.
- Existing commands that use Return, Enter, Escape, or plain Delete may keep those built-in
  defaults. A user can restore those defaults. Capture does not let the user create a new binding
  whose base key is Return, Enter, or Escape. Plain Delete without another modifier always clears
  the capture field. A modified Delete chord remains recordable.
- Binding changes persist when the user commits the row. The dialog-level Done button does not own
  a second copy of shortcut settings.
- A conflict never silently replaces another command.
- Two commands may share a binding only when their input scopes cannot be active together, or when
  an exclusive mode has an explicit higher priority.
- Text editing keeps native text behavior. Product shortcuts do not run while an editable text
  field owns focus. The active shortcut capture field is the only exception.
- The exact requested Ctrl defaults use `Qt::ControlModifier` on every platform. Native display
  text may use the platform glyph for that modifier.

### 2.2 Nodes behavior added by the user correction

- Plain node selection replaces the current node selection.
- Shift+pointer selection toggles one node in the current selection.
- The last selected node is the primary node. Existing single-node adjustment, rename, connect, and
  mask operations continue to use that primary node.
- Shift+Up and Shift+Down extend selection during normal node navigation.
- Up and Down without Shift replace selection.
- Connect mode keeps its current destination navigation. It does not extend node selection.
- Delete removes every selected Color Grade through one admitted draft mutation.
- Develop and DRT/Post remain selectable, but they are not deletable.
- If the selected set contains a non-deletable node, a protected node, or a node that owns Masks,
  the full Delete request fails before the draft or Qan view changes.
- Nodes Delete does not auto-connect the surviving neighbors. It keeps the current Nodes draft
  model. The user must complete a valid graph before Alcedo submits one topology edit.
- The Nodes selection set belongs to `EditorNodeController`. QuickQanava renders that state but does
  not become the product selection owner.
- Multi-selection does not add batch adjustment, batch rename, or batch mask editing.

### 2.3 Explicit exclusions

- Workflow and macro authoring.
- Multi-step key sequences such as Ctrl+K followed by Ctrl+C.
- Shortcut import, export, cloud sync, or named profiles.
- Operating-system-wide shortcuts.
- A command palette.
- User changes to fixed pointer actions.
- Batch adjustment values for multiple Nodes.
- Lasso or rectangle node selection.
- Changes to project save, image checkpoint, undo, redo, version, LUT, or mask domain behavior.
- A new image-save fallback when the editor checkpoint fails.
- A new project-save fallback when project packaging fails.

### 2.4 Unresolved product decisions

None. This plan fixes the first-release interaction and error behavior.

## 3. Current State

### 3.1 Existing registry

`ShortcutRegistry` currently stores named Nodes commands in a `std::map`. It provides default
`QKeySequence` values, tooltip text, optional `QAction` creation, and a linear
`commandIdForKey()` lookup.

Current limits:

- It registers only Nodes commands.
- It has no input scope in command lookup.
- It has no user override storage.
- It has no model for settings UI.
- It cannot represent modifier-only input.
- It returns the first map match when two commands use the same key.
- Tooltip text always uses the default binding.
- QML owns most required bindings directly, so changes cannot update every surface from one place.

### 3.2 Existing UI paths

- `Main.qml` owns a window-level Select All shortcut.
- `ThumbnailGridView.qml` hard-codes Shift and Ctrl checks for range and toggle selection.
- `EditorFilmstrip.qml` moves keyboard focus with Left and Right. Return or Space switches the
  image. The new behavior must switch the image directly with Left and Right.
- `EditorWorkspace.qml` owns mask Escape, Delete, Return, and Enter shortcuts.
- `EditorVersionsPanel.qml` creates a root version through an inline name draft.
- `LUTPanel.qml` has no Up or Down selection path.
- `EditorNodesPanel.qml` resolves the existing Nodes registry commands, but Delete acts on only
  `selectedNodeId`.
- `SettingDialog.qml` has no Keyboard page.

### 3.3 Existing action owners

- `SelectionState.qml` and `ImageActionsController` own Library selection.
- `ProjectLaunchController.qml` and `ProjectModule::SaveProject()` own project save.
- `EditorSessionController::PersistCurrentImage()` owns the current image checkpoint.
- The history model calls `EditorSessionController::Undo()` and `Redo()`.
- `WorkspaceRouter.qml` owns editor image switching.
- `EditorHistoryModel` owns root-version creation.
- `LUTPanel.qml` and its LUT model own LUT selection.
- Mask controls in `EditorWorkspace.qml` own mask finish, confirm, and delete behavior.
- `EditorNodeController` owns node edits and the primary node selection.
- `EditorNodeGraphDraft` owns admitted draft topology changes.

The shortcut work must call these owners. It must not copy their state into the registry.

## 4. Product Design

### 4.1 Settings navigation

Add Keyboard after Updates and before About. Keep Updates at index 6 so
`Main.openUpdateSettings()` remains valid. Keyboard becomes index 7. About moves to index 8.

The page uses the current settings shell and `appTheme` values. It does not add Material-only
chrome, raw colors, raw spacing, large rounded containers, status dots, or new icon files.

```text
+----------------------+-----------------------------------------------+
| Settings             | Keyboard                                      |
|                      |                                               |
| Language             | Library                                       |
| Theme                | Select all                 [ Ctrl+A         ] |
| Cache                | Extend selection           [ Shift          ] |
| ...                  | Toggle selection           [ Ctrl           ] |
| Updates              | Save project               [ Ctrl+S         ] |
| Keyboard             |                                               |
| About                | Editor                                        |
|                      | Undo                       [ Ctrl+Z         ] |
|                      | Redo                       [ Ctrl+R         ] |
|                      | Save current image         [ Ctrl+S         ] |
|                      |                                               |
|                      | Filmstrip / Versions / LUT / Masks / Nodes   |
|                      | ...                                           |
|                      |                                      [ Done ] |
+----------------------+-----------------------------------------------+
```

Each row contains:

- a translated action name;
- optional concise scope help when the same binding appears in another scope;
- one focusable binding field;
- a text Restore Default action when the current value differs from the default;
- one inline error line when capture or persistence fails.

The row must fit the current dialog width. Long translated labels wrap. The binding field keeps a
stable minimum width. The page scrolls when the window is short.

### 4.2 Capture states

The field has these states:

| State | Display | Input behavior |
| --- | --- | --- |
| Idle | Current native key text, or `Unassigned` | Click or keyboard activation starts capture |
| Capturing | `Press a shortcut` or the current candidate | Key input changes only the row candidate |
| Conflict | Candidate plus a translated conflict message | Enter and Escape do not save; capture stays active |
| Persistence error | Last effective value plus the write error | Registry state does not change |
| Disabled action | Current binding at reduced emphasis | Binding remains editable unless the row is marked fixed |

Capture rules:

1. A non-modifier key press records that key plus the current modifiers.
2. A modifier press starts a possible modifier-only candidate.
3. If the user releases that modifier before another key arrives, capture records the modifier-only
   candidate for commands that accept modifier input.
4. If another key arrives first, capture records a key chord.
5. Enter or Escape validates and saves the candidate. It never becomes the candidate.
6. Plain Delete clears the candidate. It stays in capture until Enter or Escape saves it.
7. Tab keeps normal focus traversal. It cancels the unsaved candidate and moves focus.
8. A pointer click on another row cancels the first unsaved candidate, then starts capture for the
   second row.
9. Closing the dialog while capture is active first saves through Escape. A second Escape may close
   the dialog under the existing close policy.
10. The field accepts Space as a normal binding candidate. Keyboard activation of an idle field uses
    Return before capture starts, so that Return is not recorded.

The capture component owns only the candidate for one active row. It does not copy the full
registry model.

### 4.3 Default binding matrix

`Scope` names below are stable registry data. They are not translated identifiers.

| Command id | User label | Default | Scope | Repeat | Input kind |
| --- | --- | --- | --- | --- | --- |
| `library.selectAll` | Select all | Ctrl+A | `workspace.library` | No | Key chord |
| `library.extendSelection` | Extend selection | Shift | `workspace.library` | N/A | Modifier |
| `library.toggleSelection` | Toggle selection | Ctrl | `workspace.library` | N/A | Modifier |
| `library.saveProject` | Save project | Ctrl+S | `workspace.library` | No | Key chord |
| `editor.undo` | Undo | Ctrl+Z | `workspace.editor` | No | Key chord |
| `editor.redo` | Redo | Ctrl+R | `workspace.editor` | No | Key chord |
| `editor.saveCurrentImage` | Save current image | Ctrl+S | `workspace.editor` | No | Key chord |
| `filmstrip.previousImage` | Previous image | Left | `editor.filmstrip` | Yes | Key chord |
| `filmstrip.nextImage` | Next image | Right | `editor.filmstrip` | Yes | Key chord |
| `filmstrip.selectAll` | Select all filmstrip images | Ctrl+A | `editor.filmstrip` | No | Key chord |
| `versions.createDefaultFromRoot` | Create default version from root | Ctrl+A | `editor.versions` | No | Key chord |
| `lut.selectPrevious` | Select previous LUT | Up | `editor.lut` | Yes | Key chord |
| `lut.selectNext` | Select next LUT | Down | `editor.lut` | Yes | Key chord |
| `mask.finishEdit` | Finish mask edit | Escape | `editor.maskEdit` | No | Key chord |
| `mask.deleteSelection` | Delete selected mask item | Delete | `editor.maskEdit` | No | Key chord |
| `mask.confirmEdit` | Confirm mask edit | Return, Enter | `editor.maskEdit` | No | Key chord |
| `nodes.deleteSelection` | Delete selected nodes | Delete | `editor.nodes` | No | Key chord |
| `nodes.extendSelection` | Extend node selection | Shift | `editor.nodes` | N/A | Modifier |

The registry keeps the existing Nodes defaults for Add Color Grade, Fit, Rename, Connect,
Complete Connect, previous node, next node, Develop, DRT/Post, and Cancel. Rename
`nodes.deleteColorGrade` to `nodes.deleteSelection`. Provide a one-time settings-key alias so any
local override under the old id moves to the new id. Do not keep both active ids.

### 4.4 Scope activation and priority

The registry records scope relationships. QML tells the input wrapper which scope is active. The
registry never infers workspace or focus from copied UI state.

Priority from highest to lowest:

1. `shortcut.capture`
2. `text.input`
3. `editor.maskEdit`
4. one focused local surface: `editor.filmstrip`, `editor.versions`, `editor.lut`, or `editor.nodes`
5. `workspace.editor` or `workspace.library`
6. application scope, reserved for future basic commands

Activation rules:

- Only one workspace scope is active.
- Only one focused local surface is active.
- Mask edit is exclusive and shadows local surface commands.
- A text input shadows product commands. Native text selection, deletion, arrow movement, undo, and
  redo remain available.
- Shortcut capture shadows every product command and dialog Escape handling.
- Disabled commands do not consume an event.
- A handled command accepts the event exactly once.

Conflict validation uses the same activation rules. It allows Ctrl+A in Library, Filmstrip, and
Versions because those scopes are exclusive. It allows Delete in mask edit and Nodes because mask
edit has exclusive higher priority. It rejects two commands with the same binding in the same
scope. It also rejects a workspace binding that would collide with a local binding while both can
be active.

### 4.5 Command behavior

#### Library

- Ctrl+A calls the current `selectAllCurrentAlbum()` path.
- The command is disabled when no project is open, a blocking dialog is open, or the current album
  cannot accept selection.
- Shift range selection preserves the current anchor behavior.
- Ctrl toggle selection preserves the current behavior.
- Ctrl+Shift range selection remains additive because both registered modifiers are active.
- Rubber-band and pointer selection use registry modifier matching. They do not hard-code Shift or
  Ctrl.
- Ctrl+S calls `ProjectLaunchController.requestSaveProject()`.

#### Editor history and save

- Ctrl+Z calls the current Undo path only when `actions.canUndo` is true.
- Ctrl+R calls the current Redo path only when `actions.canRedo` is true.
- Ctrl+S calls `EditorSessionController.PersistCurrentImage()` only when an image exists and no
  editor checkpoint is active.
- Editor Ctrl+S does not call `ProjectModule::SaveProject()`.
- A save failure uses the existing service error presentation. It does not run a different save
  path.

#### Filmstrip

- Left selects and activates the adjacent previous image in one input operation.
- Right selects and activates the adjacent next image in one input operation.
- The operation uses `WorkspaceRouter.openEditor()` and the current session switch path.
- The command stops at list bounds and reports no error.
- It updates filmstrip focus to the activated item.
- It preserves the existing selection set unless the existing activation path changes it.
- Return and Space activation remain registered basic commands or fixed aliases. This plan does not
  remove keyboard accessibility that already works.

#### Versions

- Ctrl+A runs only while the Versions surface owns focus.
- It does not run while the inline version-name field owns focus. Native text Select All wins.
- The panel computes the same translated default name that the current create draft uses.
- The panel calls `historyModel.createRootVersion(defaultName)` immediately. It does not open the
  inline name draft.
- `canMutateVersions` and `actions.canCreateRootVersion` gate the command.
- Failure uses the current history error presentation. It does not create a local placeholder.

#### LUT

- Up and Down traverse the current visible LUT rows.
- Traversal skips headers, missing files, disabled rows, and any row that the model marks unusable.
- At a boundary, the command keeps the current selection.
- A successful move calls the existing `lutModel.selectPath()` path.
- The list scrolls the selected row into view without recreating the list or resetting `contentY`.
- The search field and other editable controls keep native Up and Down behavior while they own
  focus.

#### Masks

- Move the current Escape, Delete, Return, and Enter bindings into registry entries.
- Keep the current `maskControlsActive` gates and action functions.
- Mask edit input shadows Nodes Delete, Nodes Complete Connect, and other local commands.
- When mask edit ends, Nodes shortcuts become eligible again through the current focus rules.

#### Nodes

- Shift selection updates the controller-owned ordered selection.
- `selectedNodeId` remains the primary node compatibility property.
- Add `selectedNodeIds` and `canDeleteSelectedNodes` properties.
- The adjustment panel, rename, connect, and Masks use only the primary node.
- Delete calls `deleteSelectedNodes()`.
- The controller validates the full selected set before it changes a draft.
- `EditorNodeGraphDraft::RemoveColorGrades()` begins one reversal record and returns one
  `EditorNodeGraphDraftMutation` with every removed node and edge.
- `AlcedoQanGraph::ApplyMutation()` receives that one mutation.
- If the draft becomes valid, `MaybeSubmitDraft()` submits one `NodeGraphTopologyChange`.
- If the draft remains incomplete, the controller keeps it visible and waits for the user to
  reconnect it.
- Undo restores the submitted multi-delete and reconnect edit as one history step.
- A failed precheck leaves selection, draft indexes, Qan items, and history unchanged.

## 5. Architecture

### 5.1 Ownership

| State or operation | Owner | Lifetime and thread |
| --- | --- | --- |
| Command ids, labels, groups, defaults, current bindings, scope metadata, and conflict state | `ShortcutRegistry` | Application QML engine; GUI thread |
| Saved shortcut overrides | `QSettings`, accessed only by `ShortcutRegistry` | Application lifetime; registry writes synchronously on GUI thread |
| One active unsaved capture candidate | `ShortcutCaptureField.qml` | One focused row; discarded on focus cancellation |
| Active workspace and focused surface | Existing QML workspace and panel focus owners | Existing QML lifetime; passed as enable state, not copied into C++ |
| Library selected image ids and anchor | `SelectionState.qml` | Existing workspace state |
| Node selected ids and primary node | `EditorNodeController` | Current editor session; GUI thread |
| Draft node batch removal | `EditorNodeGraphDraft` | Current node edit draft; GUI thread |
| Qan selected visuals | `AlcedoQanGraph` projection of controller state | Current graph item lifetime; GUI thread |
| Domain actions | Existing project, session, history, LUT, mask, and node owners | Existing lifetimes |

Do not add a second shortcut settings list or a second node selection list. The settings page reads
registry model roles. Qan reads the controller selection.

### 5.2 Registry data model

Change `ShortcutRegistry` from `QObject` to `QAbstractListModel`. Include the defining Qt header.
Keep it as the QML singleton.

Use these project-owned value types:

```cpp
enum class ShortcutInputKind {
  KeyChord,
  Modifier,
};

struct ShortcutInput {
  ShortcutInputKind kind;
  Qt::Key key;
  Qt::KeyboardModifiers modifiers;
};

struct ShortcutBindingSpec {
  ShortcutCommandId id;
  QString group;
  QString description;
  QList<ShortcutInput> default_bindings;
  ShortcutScope scope;
  ShortcutInputKind input_kind;
  bool auto_repeat;
  bool settings_visible;
};
```

`ShortcutRegistry` keeps ordered entries. It also keeps an id-to-row index for direct lookup. The
ordered entries are the sole current-binding state.

Model roles:

- `commandId`
- `groupText`
- `descriptionText`
- `bindingText`
- `defaultBindingText`
- `assigned`
- `usesDefault`
- `inputKind`
- `scopeText`
- `autoRepeat`
- `settingsVisible`
- `validationError`

Required callable API:

```cpp
Q_INVOKABLE bool matches(const QString& command_id, int key, int modifiers) const;
Q_INVOKABLE bool modifierMatches(const QString& command_id, int modifiers) const;
Q_INVOKABLE QString commandIdForKey(const QString& scope, int key, int modifiers) const;
Q_INVOKABLE QStringList keySequenceTexts(const QString& command_id) const;
Q_INVOKABLE QVariantMap validateCandidate(const QString& command_id,
                                          int key,
                                          int modifiers,
                                          int input_kind) const;
Q_INVOKABLE QVariantMap saveCandidate(const QString& command_id,
                                      int key,
                                      int modifiers,
                                      int input_kind);
Q_INVOKABLE QVariantMap clearBinding(const QString& command_id);
Q_INVOKABLE QVariantMap restoreDefault(const QString& command_id);
Q_INVOKABLE QString shortcutText(const QString& command_id) const;
Q_INVOKABLE QString decorateTooltip(const QString& base_text,
                                    const QString& command_id) const;
```

Use a named result map with `succeeded`, `errorCode`, `message`, and `conflictingCommandId`. Do not
return an unstructured Boolean for a settings mutation.

Keep `QAction` support only for current C++ callers that need it. Configure each action from the
effective binding set. QML input remains the live application path.

### 5.3 Registration and load order

1. Construct the registry.
2. Register all built-in command specifications.
3. Verify unique ids and valid built-in defaults.
4. Load saved overrides.
5. Validate all loaded overrides as one set.
6. Publish model rows and effective bindings.

Registration errors are programming errors. Test them and report them during startup. Do not accept
duplicate command ids.

### 5.4 Persistence schema

Use one versioned `QSettings` group:

```text
keyboardShortcuts/v1/<command-id>/state     = custom | unassigned
keyboardShortcuts/v1/<command-id>/bindings  = QStringList
```

Rules:

- A missing `state` uses the built-in default.
- `unassigned` uses no binding.
- `custom` requires one or more valid serialized inputs.
- Serialize key chords with `QKeySequence::PortableText` plus an explicit input-kind prefix.
- Serialize modifier-only input with stable names: `Shift`, `Control`, `Alt`, or `Meta`.
- Display with `QKeySequence::NativeText`.
- Unknown command ids remain ignored. They do not create settings rows.
- Migrate `nodes.deleteColorGrade` to `nodes.deleteSelection` once, then remove the old key.
- If a saved override is malformed or conflicts with another active binding, do not silently use
  the default. Disable that saved binding, publish a row error, and let the user restore or replace
  it.
- Save the setting before changing the effective in-memory binding. Call `sync()` and inspect
  `QSettings::status()`.
- If the write fails, keep the prior effective binding and return a persistence error.

The registry can accept an owned `QSettings` instance in tests. Production construction uses the
application organization and application names. This supports isolated temporary INI tests without
a parallel persistence abstraction.

### 5.5 Input wrappers

Add `RegisteredShortcut.qml` for commands that fit QML `Shortcut` behavior. It receives:

- `commandId`;
- `activeScope`;
- `enabled`;
- an activation callback.

It listens for registry binding changes and updates its sequence list. It applies the registry
repeat policy. It must not contain a command-id switch.

Focus-owned surfaces keep `Keys.onPressed`. They call `matches()` or scoped
`commandIdForKey()`. This applies to Filmstrip, LUT, Nodes, and capture input. Pointer selection asks
`modifierMatches()` for Shift and Ctrl behavior.

### 5.6 Success call chains

#### Library Ctrl+S

```text
Ctrl+S
  -> RegisteredShortcut(library.saveProject)
  -> ProjectLaunchController.requestSaveProject()
  -> ProjectModule.SaveProject()
  -> existing message presentation
```

#### Editor Ctrl+S

```text
Ctrl+S
  -> RegisteredShortcut(editor.saveCurrentImage)
  -> EditorSessionController.PersistCurrentImage()
  -> EditorSessionService current-image checkpoint
  -> thumbnail refresh and existing completion state
```

#### Filmstrip Right

```text
Right
  -> EditorFilmstrip scoped key handler
  -> ShortcutRegistry.matches(filmstrip.nextImage)
  -> activate adjacent element id
  -> WorkspaceRouter.openEditor(...)
  -> editor session switch
```

#### Versions Ctrl+A

```text
Ctrl+A
  -> focused EditorVersionsPanel
  -> createDefaultRootVersion()
  -> historyModel.createRootVersion(defaultName)
  -> existing version refresh and selection
```

#### Nodes Shift selection and Delete

```text
Shift+node press
  -> EditorNodesPanel reads nodes.extendSelection
  -> EditorNodeController.toggleNodeSelection(nodeId)
  -> selectedNodeIds + primary selectedNodeId
  -> AlcedoQanGraph.applyProductSelection(ids, primary)

Delete
  -> nodes.deleteSelection
  -> EditorNodeController.deleteSelectedNodes()
  -> EditorNodeGraphDraft.RemoveColorGrades(document, ids)
  -> one EditorNodeGraphDraftMutation
  -> AlcedoQanGraph.ApplyMutation(...)
  -> MaybeSubmitDraft() when the graph is valid
  -> one NodeGraphTopologyChange and one history step
```

### 5.7 Failure call chains

#### Binding conflict

```text
capture candidate
  -> ShortcutRegistry.validateCandidate(...)
  -> overlap check with scope activation rules
  -> conflict result names the other command
  -> capture remains active
  -> no QSettings write and no binding change
```

#### Settings write failure

```text
valid candidate
  -> write versioned QSettings key
  -> sync reports an error
  -> keep prior effective binding
  -> show inline persistence error
```

#### Invalid saved override

```text
startup load
  -> parse or conflict validation fails
  -> mark the saved row invalid and unassigned
  -> publish a settings error
  -> do not activate the default silently
```

#### Editor save failure

```text
editor.saveCurrentImage
  -> PersistCurrentImage()
  -> existing service failure
  -> existing error presentation
  -> no project-save substitute
```

#### Nodes multi-delete rejection

```text
deleteSelectedNodes()
  -> validate every selected id
  -> one id is protected, owns Masks, or is not a Color Grade
  -> return the owner error
  -> selection, draft, Qan items, and history stay unchanged
```

## 6. Exact Module and File Scope

### 6.1 Existing files to modify

Registry and registration:

- `alcedo_studio/src/include/ui/alcedo_main/shortcut_registry.hpp`
- `alcedo_studio/src/ui/alcedo_main/shortcut_registry.cpp`
- `alcedo_studio/src/ui/alcedo_main/application_module_qml_types.cpp`
- `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt`
- `alcedo_studio/tests/ui/shortcut_registry_test.cpp`

Workspace and settings QML:

- `alcedo_studio/src/ui/alcedo_main/qml/Main.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/SettingDialog.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/ThumbnailGridView.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorFilmstrip.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspace.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorVersionsPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/LUTPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/EditorNodesPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_en.ts`
- `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts`
- `alcedo_studio/src/ui/alcedo_main/DESIGN.md` only if implementation needs a new reusable token

Nodes C++:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_node_controller.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp`
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/alcedo_qan_graph.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp`
- `alcedo_studio/src/include/app/editor_node_graph_draft.hpp`
- `alcedo_studio/src/app/editor_node_graph_draft.cpp`

Tests and build registration:

- `alcedo_studio/tests/ui/CMakeLists.txt`
- `alcedo_studio/tests/app/CMakeLists.txt`
- `alcedo_studio/tests/ui/main_qml_workflow_test.cpp`
- `alcedo_studio/tests/ui/workspace_shell_test.cpp`
- `alcedo_studio/tests/ui/editor_filmstrip_qml_test.cpp`
- `alcedo_studio/tests/ui/editor_versions_panel_qml_test.cpp`
- `alcedo_studio/tests/ui/editor_lut_panel_qml_test.cpp`
- `alcedo_studio/tests/ui/editor_nodes_panel_qml_test.cpp`
- `alcedo_studio/tests/ui/editor_node_controller_test.cpp`
- `alcedo_studio/tests/ui/alcedo_qan_graph_test.cpp`
- `alcedo_studio/tests/app/editor_node_graph_draft_test.cpp`

### 6.2 New files

- `alcedo_studio/src/ui/alcedo_main/qml/RegisteredShortcut.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/KeyboardSettingsPanel.qml`
- `alcedo_studio/src/ui/alcedo_main/qml/ShortcutCaptureField.qml`
- `alcedo_studio/tests/ui/shortcut_settings_qml_test.cpp`

Register every new QML file in `ALCEDO_MAIN_QML_FILES`. Add a focused
`ShortcutSettingsQmlTest` target instead of loading the full application for every capture-field
case.

### 6.3 Files that are not expected to change

- Project serialization formats.
- `ProjectModule::SaveProject()` behavior.
- `EditorSessionController::PersistCurrentImage()` behavior.
- History journal formats.
- LUT catalog storage.
- Mask domain models.
- QuickQanava submodule source.

If implementation evidence requires a change in one of these areas, update this plan before the
phase expands.

## 7. Phase Summary

| Phase | Part | Result | Dependency | Estimated diff |
| --- | --- | --- | --- | --- |
| A1 | Back-end binding | Registry model, scopes, persistence, conflict checks, modifier input | None | 800-1,250 lines |
| A2 | Back-end binding | Library, Editor, Filmstrip, Versions, LUT, and Mask runtime routes | A1 | 900-1,450 lines |
| A3 | Back-end binding | Controller-owned Nodes multi-selection and atomic draft delete | A1 | 1,100-1,700 lines |
| B1 | Settings UI | Keyboard page, capture interaction, translations, end-to-end settings tests | A1, A2, A3 | 900-1,500 lines |

The split keeps each expected upper bound below 2,000 changed lines. A2 and A3 are separate because
Nodes multi-selection changes an application owner and a topology draft API. It must not be hidden
inside general QML routing work.

## 8. Phase A1 - Registry Model, Scope Rules, and Persistence

### Objective

Make `ShortcutRegistry` the only owner of named shortcut metadata and current bindings. Support key
chords, modifier-only input, disjoint scopes, conflicts, settings storage, current tooltip text, and
live QML updates.

### Prerequisites

- The current `ShortcutRegistryTest` passes.
- The source revision is still compatible with the file map in Section 6.
- No parallel change has added another shortcut settings owner.

### Implementation steps

1. Define `ShortcutInputKind`, `ShortcutInput`, and `ShortcutScope` in the registry header.
2. Replace `QObject` inheritance with `QAbstractListModel`.
3. Replace the sorted map as model storage with ordered entries plus an id-to-row lookup.
4. Add roles from Section 5.2.
5. Register all command ids and defaults from Section 4.3.
6. Keep existing Nodes command registration in a focused registration function. Add Library,
   Editor, Filmstrip, Versions, LUT, and Mask registration functions.
7. Add registration finalization. Reject duplicate ids, invalid defaults, and same-scope default
   conflicts.
8. Add normalized matching for Return and keypad Enter where the built-in command lists both.
9. Add modifier-only matching. Match only the configured modifier bits that the command needs.
10. Add the scope overlap table and deterministic conflict validation.
11. Add current binding text and tooltip decoration.
12. Emit row `dataChanged` and a command-specific binding signal after a successful mutation.
13. Add versioned QSettings load, save, clear, restore, and old Nodes id migration.
14. Keep invalid saved values visible as row errors and inactive bindings.
15. Keep the prior in-memory binding after a settings write failure.
16. Change the `ShortcutRegistryTest` label from the Nodes-only label to
    `keyboard_shortcuts;workspace_qml`.

### Invariants

- One command id has one registry entry.
- One row is the only effective binding state for that command.
- Scope conflict checks do not depend on registration order.
- Missing settings use defaults. Invalid settings do not.
- A failed settings write does not change the effective binding.
- Tooltips show the current effective binding.
- Modifier-only input never invokes an action on modifier press. UI owners use it only as pointer or
  selection mode.
- All registry mutation happens on the GUI thread.

### Tests

Extend `ShortcutRegistryTest` with:

- `RegisteredDefaultsResolveOnlyInsideTheirDeclaredScopes`
- `DisjointScopesMayShareOneBinding`
- `OverlappingScopesRejectDuplicateBindingWithoutChangingPriorValue`
- `MaskExclusiveScopeMayUseNodesDeleteBinding`
- `ModifierOnlyBindingMatchesRequiredPointerModifier`
- `CustomBindingReplacesAllDefaultAliases`
- `ClearBindingPersistsUnassignedStateAcrossRegistryRestart`
- `RestoreDefaultRestoresAllBuiltInAliases`
- `MalformedSavedBindingStaysInactiveAndReportsItsRowError`
- `ConflictingSavedBindingsStayInactiveUntilUserResolution`
- `OldNodesDeleteIdMovesToDeleteSelectionOnce`
- `ReservedCaptureBaseKeysFailUserValidation`
- `SettingsWriteFailureKeepsPriorEffectiveBinding`
- `TooltipUsesCurrentEffectiveBinding`
- `DuplicateCommandIdFailsRegistrationFinalization`

Use a temporary INI-backed `QSettings` file. Use a path that produces a deterministic access error
for the write-failure case. Do not use the developer's real application settings.

### Verification

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build build\debug --target ShortcutRegistryTest --parallel 4
ctest --test-dir build/debug -R "^ShortcutRegistryTest\." --output-on-failure
```

Allow 10-20 minutes for configure or build. Keep any captured output under
`build/tmp/configurable_keyboard_shortcuts/a1/`.

### Exit criteria

- Every required command has a stable id, group, scope, default, repeat policy, and input kind.
- The registry loads and saves overrides without a QML settings page.
- Scope sharing and scope conflicts have direct test evidence.
- Modifier-only Shift has direct test evidence.
- No runtime lookup depends on map iteration order.
- The existing Nodes shortcut tests pass with the new registry implementation.

### Completion record

When complete, add:

- commit id;
- changed public APIs;
- successful and failed call-chain evidence;
- exact test commands and counts;
- remaining risks or `None`.

#### Completion record - 2026-09-19

**Status:** complete on branch `feature/configurable-shortcut-registry-a1`.

**Commits:**

- `0eb4f88b` docs(roadmap): add configurable keyboard shortcut registry plan
- `9e51a19f` feat(shortcuts): registry model with scopes, modifiers, and persistence (A1)
- `e4b79036` refactor(shortcuts): split built-in command table out of registry

**Changed public APIs** (`alcedo_studio/src/include/ui/alcedo_main/shortcut_registry.hpp`):

- `ShortcutRegistry` now inherits `QAbstractListModel` (was `QObject`) with roles `commandId`,
  `groupText`, `descriptionText`, `bindingText`, `defaultBindingText`, `assigned`, `usesDefault`,
  `inputKind`, `scopeText`, `autoRepeat`, `settingsVisible`, `validationError`.
- New value types: `ShortcutInputKind` (`KeyChord`, `Modifier`), `ShortcutInput`,
  `ShortcutBindingSpec`; new `shortcut_scope` and `shortcut_id` constant namespaces.
- New C++ API: `Register(ShortcutBindingSpec)`, `FinalizeRegistration()`, `RegistrationErrors()`,
  `RowForCommand`, `Action`, `DecorateTooltip`, `RefreshEnabledStates()`.
- New QML API: `matches`, `modifierMatches`, `commandIdForKey(scope, key, modifiers)` (scope
  argument added), `keySequenceTexts`, `validateCandidate`, `saveCandidate`, `clearBinding`,
  `restoreDefault`, `shortcutText`, `decorateTooltip`.
- New free functions: `RegisterLibraryShortcuts`, `RegisterEditorShortcuts`,
  `RegisterFilmstripShortcuts`, `RegisterVersionsShortcuts`, `RegisterLutShortcuts`,
  `RegisterMaskShortcuts`, `RegisterNodesPanelShortcuts`, `RegisterBuiltinShortcuts`,
  `RegisterShortcutRegistryQmlType`.
- `commandIdForKey` signature change propagated to `EditorNodesPanel.qml`; old Nodes delete id
  `nodes.deleteColorGrade` migrates to `nodes.deleteSelection` on load.

**Success call-chain evidence** (from `ShortcutRegistryTest` run below):

```text
QML Keys.onPressed
  -> ShortcutRegistry.commandIdForKey(scope, key, modifiers)      [shortcut_registry.cpp]
  -> FindEntry + binding match inside declared scope
  -> command id returned; QML owner invokes its action
ShortcutRegistry.saveCandidate
  -> ValidateCandidate (reserved keys, kind, cross-scope conflicts)
  -> ApplyBindings + UpdateActionShortcuts + NotifyRowChanged
  -> QSettings sync write; bindingChanged emitted
```

**Failure call-chain evidence:**

```text
Malformed saved value
  -> LoadOverrides -> parse failure -> row_error set, entry stays inactive
  -> validationError role exposes message; binding never matches input
Settings write failure
  -> saveCandidate -> QSettings::sync status != NoError
  -> Result(false, kErrPersistence); prior in-memory binding retained
  -> SettingsWriteFailureKeepsPriorEffectiveBinding verifies
Duplicate/conflicting registration
  -> FinalizeRegistration -> registration_errors_ + inactive entry
  -> DuplicateCommandIdFailsRegistrationFinalization verifies
```

**Test commands and counts:**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target ShortcutRegistryTest
ctest --test-dir build/debug -R "^ShortcutRegistryTest\." --output-on-failure
```

Result: **17/17 passed** (label `keyboard_shortcuts`), including all 15 required A1 tests plus the
two retained Nodes regression tests. Build logs kept under
`build/tmp/configurable_keyboard_shortcuts/a1/`.

**Remaining risks:** None for A1 scope. QML runtime routes for non-Nodes surfaces remain unbound by
design until A2; `WorkspaceShellTest` remains disabled per `AGENTS.md` guidance.

## 9. Phase A2 - Basic UI Command Routing

### Objective

Route the required Library, Editor, Filmstrip, Versions, LUT, and Mask operations through A1
bindings. Keep each action with its current owner.

### Prerequisites

- Phase A1 is complete.
- Registry bindings update QML listeners without restarting the application.
- Existing workspace, filmstrip, versions, LUT, and mask tests pass before modification.

### Implementation steps

1. Add `RegisteredShortcut.qml` and register it in `ALCEDO_MAIN_QML_FILES`.
2. Replace the window-level hard-coded Select All shortcut with `library.selectAll`.
3. Gate Library commands by active workspace, project state, dialog state, and text-input focus.
4. Replace hard-coded Shift and Ctrl checks in `ThumbnailGridView.qml` with
   `library.extendSelection` and `library.toggleSelection` modifier matching.
5. Route Library Ctrl+S to `ProjectLaunchController.requestSaveProject()`.
6. Add editor Undo, Redo, and Save Current Image routes with the current action availability.
7. Change Filmstrip Left and Right from focus-only movement to adjacent image activation. Update
   focus after activation and keep bounds as a no-op.
8. Register the existing Filmstrip Select All behavior so it shares Ctrl+A safely with Versions.
9. Add `createDefaultRootVersion()` in `EditorVersionsPanel.qml`. Reuse the existing default-name
   calculation and root-version API.
10. Add LUT previous and next selection helpers. Skip unusable rows and keep the chosen row visible.
11. Move mask Escape, Delete, Return, and Enter sequence ownership into the registry. Keep the
    current mask functions and enable gates.
12. Add a common editable-focus check. Use active focus item properties and the existing controls.
    Do not add a stale cached focus flag.
13. Update tooltips that display any migrated command.
14. Add translated command labels to the current translation flow.

### Invariants

- The registry selects the binding. The surface selects the action.
- Ctrl+S has one Library meaning and one Editor meaning.
- Text input owns Ctrl+A, Delete, arrows, Undo, and Redo while it has focus.
- Filmstrip navigation uses the real session switch path.
- Version creation uses the real history model.
- LUT movement uses the real selection API.
- Mask mode receives its commands before Nodes or editor-local surfaces.
- A disabled command does not consume the key event.
- No failure starts an alternate operation.

### Tests

Add or extend these tests:

`WorkspaceShellTest` and `MainQmlWorkflowTest`:

- `LibrarySelectAllRunsOnlyInLibraryWorkspace`
- `LibraryShiftBindingExtendsRangeAndControlBindingTogglesSelection`
- `LibraryCtrlSUsesProjectSaveAndDoesNotPersistOnlyCurrentImage`
- `EditorCtrlSUsesCurrentImageCheckpointAndDoesNotSaveTheProject`
- `TextFieldFocusKeepsNativeSelectAllUndoAndArrowInput`
- `ChangedBindingRunsWithoutApplicationRestart`

`EditorFilmstripQmlTest`:

- `LeftAndRightSwitchToAdjacentImagesAndMoveFilmstripFocus`
- `ArrowNavigationStopsAtFilmstripBounds`
- `FilmstripSelectAllAndVersionsCreateMayShareCtrlAByFocus`

`EditorVersionsPanelQmlTest`:

- `CtrlACreatesDefaultRootVersionWhenVersionsOwnsFocus`
- `VersionNameFieldKeepsNativeCtrlA`
- `DisabledRootVersionActionDoesNotConsumeCtrlA`

`EditorLutPanelQmlTest`:

- `UpAndDownSelectUsableLutsWithoutRebuildingTheCatalog`
- `LutArrowSelectionSkipsMissingAndDisabledRows`
- `LutSearchFieldKeepsNativeArrowInput`

`EditorNodesPanelQmlTest` or the existing mask input test target:

- `MaskShortcutsShadowNodesCommandsOnlyWhileMaskEditIsActive`
- `MaskEnterEscapeAndDeleteKeepExistingActionsAfterRegistryRouting`

History and checkpoint integration:

- `UndoAndRedoBindingsRespectActionAvailability`
- `EditorSaveFailureShowsOwnerErrorWithoutProjectSaveSubstitute`

### Verification

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target MainQmlWorkflowTest WorkspaceShellTest EditorFilmstripQmlTest EditorVersionsPanelQmlTest EditorLutPanelQmlTest EditorNodesPanelQmlTest EditorCheckpointQmlIntegrationTest --parallel 4
ctest --test-dir build/debug -R "^(MainQmlWorkflowTest|WorkspaceShellTest|EditorFilmstripQmlTest|EditorVersionsPanelQmlTest|EditorLutPanelQmlTest|EditorNodesPanelQmlTest|EditorCheckpointQmlIntegrationTest)\." --output-on-failure
```

Run manual keyboard checks in both Library and Editor. Check both app themes, a short window, a
high-DPI display, and an active text field. Store notes under
`build/tmp/configurable_keyboard_shortcuts/a2/`.

### Exit criteria

- Every non-Nodes command in Section 4.3 uses the registry.
- Library and Editor Ctrl+S call different existing owners.
- Filmstrip Left and Right switch images, not only focus.
- Versions Ctrl+A creates a default root version only in its focused surface.
- LUT Up and Down select usable entries.
- Mask keys keep current behavior and priority.
- Text editing has regression coverage.
- All listed owner tests pass.

### Completion record

When complete, add:

- commit id;
- action-owner call chains for every surface;
- exact test commands and counts;
- manual focus and theme evidence;
- remaining risks or `None`.

## 10. Phase A3 - Nodes Multi-selection and Delete

### Objective

Add controller-owned Nodes multi-selection. Use the registry Shift modifier to change selection and
the registry Delete command to remove the selected Color Grades through one draft mutation.

### Prerequisites

- Phase A1 is complete.
- Existing Nodes controller, draft, Qan adapter, and QML tests pass.
- The implementation team has confirmed that raw QuickQanava selection remains a rendering detail.

### Implementation steps

1. Add an ordered `std::vector<NodeId>` selection to `EditorNodeController`.
2. Expose `selectedNodeIds` as `QStringList`. Keep `selectedNodeId` as the primary compatibility
   property.
3. Replace selection methods with focused owner operations:
   - `selectNode(nodeId)` replaces selection;
   - `toggleNodeSelection(nodeId)` adds or removes one node;
   - `extendNodeSelectionByStep(direction)` adds the keyboard neighbor;
   - `clearNodeSelection()` clears both the set and primary node.
4. Keep the most recently added selected node as primary.
5. If the primary node is removed from selection, choose the most recently selected remaining node.
6. On session refresh, prune ids that no longer exist. Preserve the remaining order. Apply the
   existing saved single selection only when no live selection survives.
7. Replace Qan's single-selection projection API with
   `applyProductSelection(QStringList ids, QString primaryId)`.
8. Enable visual multi-selection in the graph. Keep controller state authoritative.
9. Use the existing neutral selected outline for every selected node. Give keyboard focus a
   separate existing focus treatment if the current delegate already has one. Do not add accent
   fills or new selected colors.
10. Read pointer modifiers at the `EditorNodesPanel` input boundary. Use
    `nodes.extendSelection` to choose replace or toggle mode.
11. Detect Mask-row presses before node multi-selection. A Mask-row action replaces selection with
    its owning node and starts the current Mask action. Shift does not extend selection from a
    Mask-row action.
12. Add `canDeleteSelectedNodes` and `deleteSelectedNodes()`.
13. Prevalidate every selected id. Reject the full request if any id is not a deletable Color Grade.
14. Add `EditorNodeGraphDraft::RemoveColorGrades(document, ids)`. Validate all ids before
    `BeginReversal()`.
15. Remove all admitted nodes and incident edges under one reversal record. Return one mutation.
16. Apply that mutation once to Qan.
17. Remove deleted ids from selection. If no selected node survives, choose the nearest downstream
    surviving graph node, then the nearest upstream node.
18. Call `MaybeSubmitDraft()` once.
19. Keep an incomplete draft visible. Do not auto-connect neighbors.
20. Rename the QML command use from `nodes.deleteColorGrade` to `nodes.deleteSelection`.
21. Make Shift+Up and Shift+Down extend selection in normal navigation. Keep connect-mode
    navigation unchanged.

### Invariants

- `EditorNodeController` owns the selected id order and primary id.
- `selectedNodeId` is empty exactly when `selectedNodeIds` is empty.
- Every selected id exists in the current committed view or active draft.
- QuickQanava selection cannot change product selection without a controller call.
- A multi-delete precheck completes before any draft or view mutation.
- One admitted Delete input creates one draft reversal record.
- Qan receives one mutation for the Delete input.
- A valid completed draft creates one history step.
- A failed delete leaves all state unchanged.
- Multi-selection does not change which one node owns adjustment controls.

### Tests

`EditorNodeSelectionLayoutTest`:

- `PlainNodeSelectionReplacesTheCurrentSelection`
- `ShiftSelectionTogglesOneNodeAndMakesAddedNodePrimary`
- `RemovingPrimarySelectionChoosesMostRecentRemainingNode`
- `SessionRefreshPrunesMissingSelectedIdsAndPreservesRemainingOrder`
- `ShiftArrowExtendsSelectionOutsideConnectMode`
- `ConnectModeArrowNavigationDoesNotExtendSelection`
- `PrimaryNodeAloneDrivesAdjustmentAndRenameActions`

`EditorNodeGraphDraftTest`:

- `RemoveColorGradesUsesOneReversalAndReturnsOneMutation`
- `ProtectedNodeInMultiDeleteLeavesDraftUnchanged`
- `NodeWithMasksInMultiDeleteLeavesDraftUnchanged`
- `DevelopOrDrtInMultiDeleteLeavesDraftUnchanged`
- `RestoreLastMutationRestoresEveryNodeAndEdgeFromMultiDelete`
- `MultiDeleteMayRemainIncompleteUntilOneReconnectMakesItValid`

`AlcedoQanGraphTest`:

- `ProductMultiSelectionAppliesAllSelectedVisualsAndOnePrimaryNode`
- `RawQanSelectionDoesNotReplaceControllerOwnedSelection`
- `OneBatchMutationRemovesAllRequestedQanNodes`

`EditorNodesPanelQmlTest`:

- `ShiftClickTogglesNodeSelectionThroughTheRegistryModifier`
- `PlainClickReplacesNodeMultiSelection`
- `DeleteRunsOneSelectedNodesRequest`
- `MaskRowPressDoesNotExtendNodeSelection`
- `MaskDeleteShadowsNodesDeleteWhileMaskEditIsActive`
- `SubmittedMultiDeleteAndReconnectUndoesAsOneHistoryStep`
- `RejectedMultiDeleteKeepsSelectionDraftViewAndHistoryUnchanged`

### Verification

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target EditorNodeGraphDraftTest EditorNodeSelectionLayoutTest AlcedoQanGraphTest EditorNodesPanelQmlTest --parallel 4
ctest --test-dir build/debug -R "^(EditorNodeGraphDraftTest|EditorNodeSelectionLayoutTest|AlcedoQanGraphTest|EditorNodesPanelQmlTest)\." --output-on-failure
```

Run a manual Nodes check with Develop, three Color Grades, DRT/Post, a protected grade, and a grade
that owns a Mask. Store notes under `build/tmp/configurable_keyboard_shortcuts/a3/`.

### Exit criteria

- Shift pointer and keyboard selection produce controller-owned multi-selection.
- Delete acts on the full selected Color Grade set.
- Rejected sets cause no partial mutation.
- Qan renders all selected nodes without owning the product state.
- The primary node keeps existing single-node controls stable.
- Multi-delete and reconnect submit one history step.
- All listed Nodes tests pass.

### Completion record

When complete, add:

- commit id;
- controller and draft API changes;
- successful and rejected multi-delete call chains;
- exact test commands and counts;
- manual protected-node and Mask-owner evidence;
- remaining risks or `None`.

## 11. Phase B1 - Keyboard Settings Page and Capture Interaction

### Objective

Expose the registry in Settings. Implement direct capture, validation, clear, restore, translated
errors, keyboard access, and live runtime updates.

### Prerequisites

- Phase A1 is complete.
- A2 and A3 commands use the registry.
- The registry publishes current model roles and row mutation results.
- No command still keeps a second editable key value in QML.

### Implementation steps

1. Add Keyboard at category index 7. Move About to index 8.
2. Add `KeyboardSettingsPanel.qml` with a scrollable grouped list over the registry model.
3. Filter only `settingsVisible` rows. Preserve registry order within groups.
4. Add `ShortcutCaptureField.qml`.
5. Implement idle, capturing, candidate, conflict, unassigned, and persistence-error states.
6. Implement modifier press and release handling for modifier-only commands.
7. Make Enter and Escape save. Reject them as candidate base keys.
8. Make plain Delete clear the candidate. Accept modified Delete chords.
9. Make Tab cancel the unsaved candidate and move focus.
10. Add Restore Default as a text action on changed rows.
11. Send every save, clear, and restore operation through registry APIs.
12. Keep the capture active after a validation conflict.
13. Show the conflicting translated action name and scope in the error.
14. Show persistence failure without changing the displayed effective value.
15. Make live tooltip and runtime input update after a successful row commit.
16. Give fields and actions stable object names for QML tests.
17. Add accessible names, descriptions, focus order, and selected/focused visual states.
18. Use only `appTheme` values and existing settings components. If a new reusable value is needed,
    add it to `AppTheme` and `DESIGN.md` in the same change.
19. Add English and Simplified Chinese translations.
20. Update the QML source list and add `ShortcutSettingsQmlTest`.

### Invariants

- The settings page reads the registry model directly.
- Only one capture field is active.
- Enter and Escape save during capture and never become custom bindings.
- Plain Delete clears during capture.
- A conflict does not write settings.
- A write failure does not change effective input.
- Dialog Done does not keep or apply a second shortcut data set.
- A successful change affects open surfaces immediately.
- The UI remains usable with keyboard only.
- The UI uses Basic-style project components and theme values.

### Tests

Add `ShortcutSettingsQmlTest` with:

- `KeyboardCategoryIsReachableAndAboutStillOpens`
- `RegistryRowsAppearInStableGroups`
- `ClickingBindingFieldStartsCapture`
- `ReturnSavesCandidateWithoutBecomingTheBinding`
- `EscapeSavesCandidateWithoutClosingTheDialog`
- `SecondEscapeClosesTheDialogAfterCaptureEnds`
- `PlainDeleteClearsCandidateAndSavePersistsUnassignedState`
- `ModifiedDeleteCanBeCaptured`
- `ModifierReleaseCapturesShiftForModifierCommands`
- `KeyAfterModifierCapturesAChordForKeyCommands`
- `ConflictKeepsCaptureActiveAndNamesTheOtherAction`
- `PersistenceFailureKeepsThePriorDisplayedBinding`
- `RestoreDefaultRestoresReservedBuiltInKeys`
- `TabCancelsUnsavedCandidateAndMovesFocus`
- `SuccessfulChangeUpdatesAnOpenRegisteredShortcut`
- `CaptureFieldExposesAccessibleNameDescriptionAndFocus`
- `KeyboardPageUsesThemeValuesWithoutRawVisualColors`

Extend `MainQmlWorkflowTest` with:

- `SettingsDoneDoesNotOverwriteCommittedShortcutChanges`
- `TextInputAndCapturePriorityPreventProductCommandActivation`

Run `alcedo_main_lupdate` and review both translation files. Do not accept an untranslated visible
string in either supported language.

### Verification

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target ShortcutSettingsQmlTest MainQmlWorkflowTest alcedo_main_lupdate alcedo_main_lrelease --parallel 4
ctest --test-dir build/debug -R "^(ShortcutSettingsQmlTest|MainQmlWorkflowTest)\." --output-on-failure
```

Then run the complete focused suite:

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target ShortcutRegistryTest ShortcutSettingsQmlTest MainQmlWorkflowTest WorkspaceShellTest EditorFilmstripQmlTest EditorVersionsPanelQmlTest EditorLutPanelQmlTest EditorNodeGraphDraftTest EditorNodeSelectionLayoutTest AlcedoQanGraphTest EditorNodesPanelQmlTest EditorCheckpointQmlIntegrationTest --parallel 4
ctest --test-dir build/debug -R "^(ShortcutRegistryTest|ShortcutSettingsQmlTest|MainQmlWorkflowTest|WorkspaceShellTest|EditorFilmstripQmlTest|EditorVersionsPanelQmlTest|EditorLutPanelQmlTest|EditorNodeGraphDraftTest|EditorNodeSelectionLayoutTest|AlcedoQanGraphTest|EditorNodesPanelQmlTest|EditorCheckpointQmlIntegrationTest)\." --output-on-failure
```

Run manual checks in English and Simplified Chinese. Check Light and Dark themes, high DPI, 125%
text scaling, a short settings window, keyboard-only navigation, a screen reader name query, and
settings persistence after application restart. Store notes under
`build/tmp/configurable_keyboard_shortcuts/b1/`.

### Exit criteria

- Settings shows every visible registered basic command.
- Click starts capture.
- Enter and Escape save without becoming bindings.
- Delete clears input.
- Shift can be captured as modifier-only input.
- Conflicts and persistence failures are explicit and non-destructive.
- Restore Default can recover built-in Escape, Enter, and Delete bindings.
- New values work in already-open surfaces.
- Both translations and both themes pass review.
- The complete focused suite passes.

### Completion record

When complete, add:

- commit id;
- screenshots or rendered captures for both themes and both languages;
- capture success, conflict, clear, restore, and write-failure evidence;
- exact test commands and counts;
- accessibility and keyboard-only notes;
- remaining risks or `None`.

## 12. Cross-phase Acceptance Matrix

| Requirement | Primary phase | Automated evidence | Manual evidence |
| --- | --- | --- | --- |
| Filmstrip Left and Right switch images | A2 | `EditorFilmstripQmlTest` | Bounds and repeated input |
| Ctrl+Z and Ctrl+R Undo and Redo | A2 | history and workflow tests | Toolbar state matches command state |
| Library Ctrl+A | A2 | `WorkspaceShellTest` | Album with many images |
| Library Shift multi-selection | A1, A2 | registry and workspace tests | Pointer range and rubber-band |
| Versions Ctrl+A creates a default root version | A2 | `EditorVersionsPanelQmlTest` | Focus and inline-name field |
| Library Ctrl+S saves project | A2 | workflow test | Project save feedback |
| Editor Ctrl+S saves current image | A2 | checkpoint integration test | Checkpoint progress and failure |
| LUT Up and Down select LUTs | A2 | `EditorLutPanelQmlTest` | Missing file and list bounds |
| Mask Escape, Delete, Enter remain valid | A2 | mask and Nodes QML tests | Mode transition priority |
| Nodes Delete removes selected nodes | A3 | controller, draft, Qan, and QML tests | Mixed protected selection |
| Nodes Shift multi-selection | A3 | controller and QML tests | Primary-node clarity |
| Settings capture and persistence | B1 | registry and settings QML tests | Restart application |
| Enter and Escape save capture | B1 | settings QML tests | Dialog Escape sequence |
| Delete clears capture | B1 | settings QML tests | Clear and restore default |

## 13. Build, Quality, and Documentation Checks

Each implementation phase must run formatting on touched C++ files and QML lint checks that the
repository already supports. Run CMake through `scripts/msvc_env.cmd` on Windows. Do not use a bare
configure or build command.

Before phase completion:

1. Build the focused targets.
2. Run each built test executable through CTest discovery.
3. Run the complete focused suite from Phase B1 after all phases land.
4. Run `alcedo_main_lupdate` and review translation changes.
5. Search touched source, tests, and this plan for prohibited terminology from `AGENTS.md`.
6. Review new includes. Do not add first-party forward declarations unless a documented include
   cycle or PIMPL boundary requires one.
7. Review new copied state. Keep shortcut bindings in the registry and node selection in the
   controller.
8. Do not add a generation number, stale-result token, or cancellation protocol. All work here is
   on the GUI thread and no current call chain can reorder these mutations.
9. Keep logs and temporary output under `build/tmp/configurable_keyboard_shortcuts/`.

## 14. Risks and Mitigations

| Risk | Required mitigation |
| --- | --- |
| Ctrl+A activates the wrong action | Scope-aware matching, exclusive focused surfaces, and text-input tests |
| Runtime changes do not update QML `Shortcut` objects | `RegisteredShortcut` listens to command binding changes and updates sequences |
| Shift alone fires an action | Treat modifier bindings as selection-mode queries only |
| Capture Escape closes the dialog | Capture handles Escape before dialog close and commits the row |
| Plain Delete cannot be restored as a default | Restore Default bypasses user-candidate restrictions and restores registered defaults |
| Invalid saved settings silently change behavior | Keep invalid custom binding inactive and show a row error |
| Node multi-selection diverges from Qan visuals | Controller publishes one selection list and adapter applies the complete list |
| Multi-delete partially changes a draft | Prevalidate all ids, then use one draft reversal and one Qan mutation |
| Multi-delete creates several history steps | Submit one completed `NodeGraphTopologyChange` |
| Editor Ctrl+S saves the wrong scope | Separate command ids and action-owner tests for Library and Editor |
| Arrow commands interfere with search or name fields | Editable-focus priority and direct regression tests |
| New settings visuals diverge from Alcedo VI | Reuse settings components and `appTheme`; update `DESIGN.md` with any new token |

## 15. Final Completion Evidence

The feature is complete only when all four phases have a completion record and all items below are
true:

- One registry owns every binding in Section 4.3.
- Settings can capture, clear, restore, validate, and persist those bindings.
- Focus and scope decide which duplicate binding is active.
- Library, Editor, Filmstrip, Versions, LUT, Masks, and Nodes use their existing action owners.
- Nodes Shift selection and Delete use controller-owned multi-selection and one admitted draft
  mutation.
- No workflow or macro system was added.
- The complete focused test suite passes.
- Manual evidence covers both languages, both themes, keyboard-only use, text input priority,
  settings restart, and the error paths in this plan.

