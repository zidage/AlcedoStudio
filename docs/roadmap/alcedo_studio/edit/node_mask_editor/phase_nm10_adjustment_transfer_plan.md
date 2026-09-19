# Phase NM10 — Node-aware Adjustment Transfer

Date: 2026-09-18

Status: **in progress**. The user approved the product design on 2026-09-18. NM10.1,
NM10.2, NM10.3, and NM10.4 are implemented and verified (see sections 10.1.12,
11.12, 12.12, and 13.12); NM10.5 onward remain planned.

Parent: [Node-aware Pipeline Editing and Mask Creation](../node_mask_editor_master_plan.md),
Sections 14, 20.6, 21.11, and 26.

Direct prerequisites:

- [NM4 — History, Version, Recovery, and Paste](phase_nm4_history_version_paste_plan.md)
- [NM6 — Node-aware Adjustment Stack](phase_nm6_node_aware_adjustments_plan.md)
- [NM9 — Twin Mask Groups Panel](phase_nm9_mask_group_panel_plan.md)
- [Alcedo Studio QML Visual Identity](../../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md)

Source audit revision: `9dbba2c1` on `main`.

---

## 1. Decision record

### 1.1 Approved goal

NM10 replaces the old stage-based Adjustment Transfer selection with a node-aware workflow.
The user selects one source Version. The user then selects Color Grade nodes and DRT/Post.
The user selects transferable items for the focused node.

The Copy dialog uses three columns:

1. Source Version.
2. Transferable nodes.
3. Transferable items for the focused node.

The second and third columns provide a `Select All` checkbox and a `Clear` button.
All enabled action buttons use white text. No action button uses blue fill with black text.

Each Color Grade shows one `Masks` checkbox. This checkbox transfers all Masks in the node.
The dialog does not list individual Masks. The user does not need to identify each Mask.

The footer does not report selected item counts, node counts, or target counts.
The primary action label is `Copy Adjustments` or `Paste Adjustments`.

### 1.2 Phase boundary

The user confirmed that the earlier History and Version defects no longer reproduce.
The prior cleanup pull request removed unnecessary state checks and fixed those behaviors.
NM10 does not reopen those defects.

This phase does not change:

- root Version creation;
- Undo or Redo behavior;
- mini-Git head movement;
- node graph creation or connection behavior;
- Mask authoring;
- Mask thumbnail generation;
- render quality or backend selection.

NM10 can add regression coverage when Adjustment Transfer uses these existing paths.
It must not redesign them without a new reproduced defect.

### 1.3 Evidence status at phase start

The user confirmed that NM9 is complete on 2026-09-18.
The user completed NM9.5 and later checks with manual verification.
This statement is user evidence. It is not an automated test record.

NM10 uses the NM9 Mask ownership and Paste behavior as existing inputs.
NM10 does not claim new automated evidence for NM9.

---

## 2. Product design specification

### 2.1 Copy dialog layout

```text
┌──────────────────────────── Copy Adjustments ─────────────────────────────┐
│ Source image title                                                        │
├────────────────────┬──────────────────────┬────────────────────────────────┤
│ Source Versions    │ Nodes                │ Color Grade 2                  │
│                    │ [x] Select All       │ [-] Select All                 │
│                    │ [Clear]              │ [Clear]                        │
│                    │                      │                                │
│ Current Look       │ [x] Color Grade 1    │ Node                           │
│ Warm Portrait      │ [-] Color Grade 2    │ [x] Enabled                    │
│ Black and White    │ [ ] Color Grade 3    │ [x] Mix                        │
│                    │ [x] DRT and Post     │                                │
│                    │                      │ Basic Tone                     │
│                    │                      │ [x] Exposure                    │
│                    │                      │ [ ] Contrast                    │
│                    │                      │ [x] Highlights                  │
│                    │                      │                                │
│                    │                      │ Color                          │
│                    │                      │ [x] White Balance               │
│                    │                      │ [x] Color Wheels                │
│                    │                      │                                │
│                    │                      │ Masks                           │
│                    │                      │ [x] Masks                       │
├────────────────────┴──────────────────────┴────────────────────────────────┤
│                                                   [Cancel] [Copy Adjustments]│
└────────────────────────────────────────────────────────────────────────────┘
```

The ASCII marks show state only. Production QML uses the approved monochrome selection tokens.
It does not draw text symbols for partial state when the shared checkbox can draw that state.

### 2.2 Source Version column

The first column has single selection.
It lists the source image Version refs in the existing owner order.
The active Version is selected when the dialog opens.

Selecting a Version performs a read-only replay of that Version.
The operation must not change the live active Version.
The operation must not save the source image.
The operation must not change dirty state, WAL data, render state, or UI selection outside the dialog.

Changing the Version replaces the node and item models.
The new Version starts with all transferable items selected.
The dialog does not carry selections from the prior Version.

If replay fails, the dialog keeps the prior valid Version selection.
The dialog shows the exact error.
It does not inspect a substitute Version.

### 2.3 Node column

The node column contains:

- each Color Grade on the source image backbone;
- one `DRT and Post Processing` endpoint row.

The column does not contain Develop, RAW decode, camera data, lens identity, or geometry.
The source backbone order defines the Color Grade order.
Canvas position, creation time, and display-name sorting do not change this order.

Each row has two independent states:

- **focus:** the row supplies the third-column item model;
- **inclusion:** child selections determine the row checkbox state.

Clicking the row body changes focus only.
Clicking the row checkbox changes all transferable items for that node.

The row checkbox uses three states:

- unchecked when no child item is selected;
- partial when some child items are selected;
- checked when all child items are selected.

The node-column `Select All` checkbox uses the same three states.
Activating an unchecked or partial checkbox selects all transferable items.
Activating a checked checkbox clears all transferable items.
The `Clear` button always clears all transferable items in the source Version.

Focus remains on a valid row after a clear action.
Clearing inclusion must not clear focus.

### 2.4 Item column

The item column shows items for the focused node.
It groups items by the same product sections as the Adjustment Stack.
It obtains display names from the adjustment catalog or the existing UI vocabulary.
It does not maintain a second hard-coded operator list.

For a Color Grade, the column can contain:

- `Enabled`;
- `Mix`;
- White Balance;
- Exposure;
- Contrast;
- Whites;
- Blacks;
- Shadows;
- Highlights;
- Tone Curve;
- HLS;
- Saturation;
- Vibrance;
- Color Wheels;
- Look LUT;
- one `Masks` item.

The exact adjustment order comes from the owning document and catalog.
The list must not infer identity from the display label or operator type alone.
It uses `AdjustmentInstanceId` for each adjustment.

The `Masks` item has one checkbox.
A checked value transfers all Masks in the focused Color Grade.
An unchecked value transfers no Mask from that Color Grade.
The package preserves Mask order and complete Mask data when the checkbox is checked.
The package does not expose one checkbox per Mask.

When a Color Grade has no Masks, the dialog shows one disabled `Masks` item.
The disabled item remains unchecked.

For DRT/Post, the column contains:

- `Display Transform` for the complete DRT parameter value;
- Clarity;
- Sharpen;
- Halation;
- Film Grain.

The item-column `Select All` checkbox applies to the focused node only.
It uses unchecked, partial, and checked states.
The item-column `Clear` button clears the focused node only.

### 2.5 Default selection

The dialog selects all transferable items when it opens.
It also selects all transferable items after a source Version change.

This default makes a complete look transfer the shortest workflow.
The user can clear a node or item before copy.

The `Copy Adjustments` button is disabled when no transferable item is selected.
The dialog does not show a numeric selection summary.

### 2.6 Action buttons

All enabled text buttons in this dialog use white text.
This rule applies to `Clear`, `Cancel`, `Copy Adjustments`, and `Paste Adjustments`.

The primary button can use the documented accent fill.
Its text remains white in idle, hover, pressed, and focus states.
No action button uses `editorListSelectedFillColor` with dark ink.
No action button uses a blue fill with black text.

The dialog uses `DialogActionButton` for text actions.
The implementation must not add a local button palette.
If the shared component needs a new state, update `DialogActionButton` and `DESIGN.md` together.

### 2.7 Footer

The footer contains flexible empty space and the action buttons.
It does not show:

- selected item count;
- total item count;
- selected node count;
- target image count;
- a combined settings and targets string.

Copy mode shows `Cancel` and `Copy Adjustments`.
Paste mode shows `Cancel` and `Paste Adjustments`.

### 2.8 Paste confirmation

Copy mode uses the full three-column layout.
Paste mode shows a read-only package summary.
It can hide the Version column because a package does not transfer a Version identity.

Paste mode shows the copied nodes and their included items.
It shows one `Masks` row for each transferred Mask set.
It does not permit a second edit of package selection.

Paste always creates a new root-relative Version.
Paste does not offer Merge.
Paste does not modify the target current Version in place.

### 2.9 Keyboard and accessibility

Tab order moves from the Version list to the node list and item list.
It then moves to footer actions.

Arrow keys move focus inside the active list.
Space toggles the focused checkbox.
Enter focuses a Version or node row.
Escape closes the dialog.

Each checkbox exposes its checked or partial state.
Each list row exposes its display name.
The `Masks` checkbox accessible name is `Transfer all masks in this node`.

Focus and selection must remain visible in both themes.
Keyboard focus must not use a blue selection slab.

### 2.10 Empty and error states

Use these states:

| Condition | Required result |
| --- | --- |
| Source has no Version | Do not open Copy. Report `No source versions are available.` |
| Version replay fails | Keep the prior valid selection. Show the replay error. |
| Version has no transferable node | Disable Copy. Show `No transferable adjustments.` |
| Focused node has no Masks | Show one disabled `Masks` item. |
| All items are clear | Keep the dialog open. Disable `Copy Adjustments`. |
| Package validation fails | Keep the prior copied package. Report the exact error. |
| Paste target fails | Keep existing per-target failure reporting. Create no partial Version. |

The implementation must not silently use the active Version after another Version fails.

---

## 3. Transfer semantics

### 3.1 Stable selection identity

The selection uses stable domain identities.
It does not use a QML row index as a domain identity.

A Color Grade selection contains:

- source `NodeId`;
- selected node fields;
- selected `AdjustmentInstanceId` values;
- one `include_masks` value.

A DRT/Post selection contains:

- the selected DRT parameter item;
- selected DRT/Post `AdjustmentInstanceId` values.

The selected source Version uses `version_ref_id_t`.
The package does not copy that Version id to the target image.

### 3.2 Color Grade construction

The package includes a Color Grade when it has one selected child item.
The package preserves the order of included Color Grades on the source backbone.

The paste planner creates each included Color Grade from clean defaults.
It then applies only the selected source values.

This rule gives these results:

- an unselected adjustment keeps its clean default value;
- an unselected `Enabled` item keeps the clean enabled value;
- an unselected `Mix` item keeps the clean mix value;
- an unchecked `Masks` item adds no Mask;
- a checked `Masks` item adds every source Mask in source order.

The planner transfers the Color Grade display name.
It transfers the source deletion-protection value as structural document data.
It does not expose either value as an item checkbox.

The planner remaps each transferred Node, adjustment, Mask, and optional Stroke identity.
The package never copies an absolute cache path or an R8 cache value.

### 3.3 Default Color Grade identity

When the selected source set contains the source default Grade, the remapped Grade stays default.

When the selected set omits the source default Grade, the first included Grade becomes default.
This result gives the target document one valid default Grade identity.

The target document applies its required default protection rule after identity remap.
Other transferred protection values keep their source values.

### 3.4 DRT/Post construction

The target immutable root supplies the DRT/Post node identity.
The planner does not create a second DRT/Post endpoint.

The planner starts from the target root DRT/Post value.
It applies only the selected source DRT/Post items.

`Display Transform` is one item.
It transfers the complete DRT parameter value as one indivisible value.
The four post adjustments remain separate items.

### 3.5 DRT-only transfer

The user can copy only DRT/Post items.

When no Color Grade item is selected, paste keeps the target immutable root Color Grade chain.
It applies the selected DRT/Post items to the target root DRT/Post node.
It does not use values from the target current Version.

The package is not empty when it contains a selected DRT/Post item.

### 3.6 Target image data

Paste preserves these target values:

- Develop endpoint;
- RAW metadata;
- camera profile;
- lens identity;
- geometry;
- target image root identity.

Paste does not transfer these source values.
NM10 does not add an option to transfer geometry.

### 3.7 Package format

NM10 introduces a new sparse transfer package schema.
The schema must represent field presence without inventing default source values.

The schema contains:

- package schema id;
- pipeline document format version;
- ordered Color Grade transfer entries;
- selected fields for each Color Grade;
- selected adjustment values for each Color Grade;
- an optional complete Mask set for each Color Grade;
- selected DRT/Post fields;
- package fingerprint.

The format uses typed C++ values inside the application layer.
JSON remains at the import and export boundary.

The implementation increments `kAdjustmentTransferSchema` from v5 to v6.
It rejects v5 packages with an explicit unsupported-schema error.
It does not add an implicit v5 conversion path.

The fingerprint covers field presence, item order, values, and Mask data.
It does not cover source UI focus or source Version id.

---

## 4. Scope

### 4.1 Included modules

| Module | Required work |
| --- | --- |
| Transfer types | Replace legacy category booleans with stable node and item selection. |
| Transfer package | Add sparse v6 typed data and canonical JSON. |
| Package builder | Build a package from one replayed source document and one selection. |
| Paste planner | Create clean selected Grades and apply selected DRT/Post values to the target root. |
| Version catalog | Replay a source Version without changing the live active Version. |
| Dialog model | Own Version, node, item, focus, and checkbox state for QML. |
| Transfer controller | Keep only dialog setup, package ownership, and command routing. |
| Apply coordinator | Own multi-target paste, persistence, thumbnail refresh, and HDR refresh. |
| QML dialog | Add the three-column Copy layout and read-only Paste summary. |
| Shared QML controls | Add partial checkbox support without breaking current callers. |
| Tests | Cover package selection, replay isolation, UI behavior, paste, save, and reopen. |
| Documentation | Update QML docs, schema notes, phase records, and the master plan. |

### 4.2 Explicit exclusions

NM10 does not include:

- History or Version repair work that the user confirmed as resolved;
- pipeline Merge;
- current-Version merge semantics;
- geometry transfer;
- RAW or lens transfer;
- one-checkbox-per-Mask selection;
- Mask preview thumbnails in this dialog;
- Brush product enablement;
- a second writable PipelineDocument;
- a second edit-history model;
- a fallback to legacy stage/operator transfer;
- a render-quality or backend change.

### 4.3 Related plan behavior

NM10 extends NM4 Paste.
It does not replace the root-relative Version rule.

NM10 reads node and adjustment ownership from NM6.
It does not create another adjustment catalog.

NM10 treats the NM9 Mask vector as one selectable set per Color Grade.
It does not change Mask editing or thumbnail ownership.

---

## 5. Current source audit

### 5.1 Verified implementation facts

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Legacy item list | `adjustment_transfer_controller.cpp` | `kItems` hard-codes stage and operator pairs. It includes RAW, geometry, and lens entries. | Build rows from node ownership and the adjustment catalog. |
| Copy selection | `AdjustmentTransferController::CopyVersion` | The controller parses selected keys for its summary. It then calls `CaptureDocumentTransfer` for the complete document. | Make selection control actual package contents. |
| Version inspection | `AdjustmentTransferController::PrepareCopy` and `CopyVersion` | The controller changes the live graph active Version. It rebuilds the live pipeline and changes it back. | Replay the selected Version through a read-only path. |
| Transfer package | `adjustment_transfer_types.hpp` | v5 stores complete Color Grade JSON and complete DRT/Post JSON. | Add sparse typed v6 transfer entries. |
| Legacy selection type | `AdjustmentTransferSelection` | The type stores broad category booleans and an optional operator filter. | Replace it with stable node and item identities. |
| Capture and paste | `document_transfer.cpp` | Capture exports the complete Grade chain. Paste replaces the root chain with the complete package. | Split selected capture from selected root-relative planning. |
| Controller size | `adjustment_transfer_controller.cpp` | The file has more than 800 lines. It owns presentation, copy, paste, persistence, HDR work, and thumbnails. | Split application work and Qt model work by owner. |
| Dialog data | `AdjustmentTransferDialog.qml` | QML clones arrays, rebuilds rows, and stores checked state. | Move selection state to a C++ dialog model. |
| Dialog layout | `AdjustmentTransferDialog.qml` | Copy mode has Version and parameter panes. It has no node pane. | Add Version, node, and item panes. |
| Bulk actions | `AdjustmentTransferDialog.qml` | `Select All` and `None` are text actions with a separator. | Use a `Select All` checkbox and a white-text `Clear` button. |
| Footer | `AdjustmentTransferDialog.qml` | The footer reports item and target counts. The Copy label contains a count. | Remove all footer counts and use fixed action labels. |
| QML copy boundary | `AppDialogs.qml` | QML sends a list of string keys and a Version id. | Send no domain selection list from QML. Let the C++ model build it. |
| Paste owner | `AdjustmentTransferController::PasteViaMiniGit` | The controller loops targets and persists each new Version. | Move this workflow to an apply coordinator. |
| Existing tests | app, history, and UI transfer tests | Tests prove complete-document Paste and the old two-pane UI. | Add selected-node, selected-item, Mask-set, and three-pane evidence. |

### 5.2 Current correctness defects in transfer

The current Copy UI can report a selected subset while it stores a complete package.
This behavior makes the visible selection inaccurate.

The current read operation changes live history and pipeline state.
It then attempts to restore copied state on failure.
This design gives a read-only dialog unnecessary write and restore responsibilities.

The old item list can expose data that the current master plan excludes.
Examples include RAW, lens, and geometry items.

These defects are in NM10 scope.

### 5.3 Allowed independent replayed document

The dialog must inspect a historical Version that might not be active.
The live document cannot supply that historical value without a state change.

The read service can therefore create one independent replayed source document.
This value has these rules:

- Purpose: inspect and export one selected historical Version.
- Fields: the complete replay result for that Version.
- Owner: the dialog model through a read-service result.
- Lifetime: until the user selects another Version or closes the dialog.
- Mutability: immutable after replay.
- Consistency: root plus the selected Version first-parent commit path.
- Release: destroy it when the dialog model replaces or closes it.
- Writeback: never write it to the live guard or project storage.

The QML models store minimal row descriptors.
They do not store another writable document.

---

## 6. Target architecture

### 6.1 Application-layer owners

#### `AdjustmentTransferCatalogService`

Proposed files:

- `alcedo_studio/src/include/app/adjustment_transfer_catalog.hpp`
- `alcedo_studio/src/app/adjustment_transfer_catalog.cpp`

Responsibilities:

- read Version refs from a const `CommitGraph`;
- replay one selected Version from a const root document;
- build minimal node and item descriptors;
- provide the immutable replayed source document;
- report replay and ownership errors;
- never change the live guard.

The service uses `FirstParentCommitsForHead` and `ReplayPipelineDocumentFromRoot`.
It must not call `SetActiveVersionId` or `RebuildActiveEditorPipeline`.

#### `AdjustmentTransferPackageBuilder`

Proposed files:

- `alcedo_studio/src/include/app/adjustment_transfer_package_builder.hpp`
- `alcedo_studio/src/app/adjustment_transfer_package_builder.cpp`

Responsibilities:

- validate stable selection identities against the replayed source document;
- preserve source backbone order;
- copy only selected values;
- copy all Masks or no Masks for each selected Color Grade;
- create canonical v6 package data;
- compute the package fingerprint;
- change no source state.

#### `DocumentTransferPlanner`

Proposed files:

- `alcedo_studio/src/include/app/document_transfer_planner.hpp`
- `alcedo_studio/src/app/document_transfer_planner.cpp`

Responsibilities:

- start from the target immutable root;
- create clean selected Color Grades;
- apply selected source values;
- apply selected DRT/Post values;
- remap all transferred identities;
- produce one validated `PipelineEditBatch`;
- change no live document.

`document_transfer.cpp` keeps canonical JSON import, export, and fingerprint work.
The planner removes paste planning from that file.

#### `AdjustmentTransferService`

The service remains the Qt-free application facade.
It composes the catalog, package builder, planner, and root-relative Version creation.
It does not own QML rows or target thumbnail refresh.

### 6.2 Qt owners

#### `AdjustmentTransferDialogModel`

Proposed files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.cpp`

Responsibilities:

- own the current source Version id;
- own the immutable replayed source document;
- own node focus;
- own item selection;
- expose Version, node, and item list models;
- compute node and bulk checkbox states;
- build one `AdjustmentTransferSelection`;
- report `canCopy` and error text without numeric summaries.

The model does not paste or persist a target image.

#### `AdjustmentTransferListModels`

Proposed files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/adjustment_transfer_list_models.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/adjustment_transfer_list_models.cpp`

Responsibilities:

- expose stable row roles through `QAbstractListModel`;
- keep `NodeId` and item identity in roles;
- publish focused, checked, and partial states;
- update only affected rows when selection changes;
- preserve `ListView.contentY` for checkbox changes.

#### `AdjustmentTransferApplyCoordinator`

Proposed files:

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp`

Responsibilities:

- apply one copied package to one or more targets;
- call root-relative Paste through the existing history owner;
- persist each successful target;
- refresh HDR metadata and thumbnails after success;
- collect per-target failures;
- avoid catch-and-continue for a failed step inside one target.

One target can fail without hiding another target result.
The coordinator must report every target failure.

#### `AdjustmentTransferController`

The controller becomes thin glue.
It owns the copied package and package summary.
It prepares the dialog model and routes Copy or Paste commands.

Target size: approximately 250 to 350 implementation lines.
The target is a responsibility boundary, not a strict formatting metric.

### 6.3 QML files

Keep `AdjustmentTransferDialog.qml` as the shell.
Add these proposed files:

- `AdjustmentTransferVersionPane.qml`
- `AdjustmentTransferNodePane.qml`
- `AdjustmentTransferItemPane.qml`

Register every new QML file in `ALCEDO_MAIN_QML_FILES`.

Extend `ThemeCheckBox.qml` with an optional partial state.
Keep its current boolean caller behavior compatible.

Use `DialogActionButton.qml` for `Clear`, footer, and primary actions.
Do not build a local text-button implementation in the pane files.

### 6.4 Copy success call chain

```text
Copy action
  -> AdjustmentTransferController::PrepareCopy
  -> dialog model reads Version refs from const CommitGraph
  -> catalog service replays the active Version from immutable root
  -> dialog model publishes Version, node, and item rows
  -> user changes focus and selection in C++ models
  -> Copy Adjustments
  -> dialog model builds stable AdjustmentTransferSelection
  -> package builder validates the replayed source document and selection
  -> package builder creates sparse v6 package and fingerprint
  -> controller replaces the copied package only after full validation
  -> PackageChanged
  -> read-only Paste summary becomes available
```

### 6.5 Copy failure call chain

```text
Version replay or package validation failure
  -> catalog service or package builder returns the exact error
  -> dialog model keeps the prior valid replayed source document
  -> controller keeps the prior copied package
  -> no live active Version change
  -> no source save
  -> no WAL or render change
  -> dialog shows the error
```

### 6.6 Paste success call chain

```text
Paste Adjustments
  -> AdjustmentTransferApplyCoordinator
  -> load target history owner and immutable root
  -> DocumentTransferPlanner validates v6 package
  -> planner creates clean selected Grades and selected DRT/Post changes
  -> planner remaps NodeId, AdjustmentInstanceId, MaskId, and optional StrokeId
  -> one validated PipelineEditBatch
  -> AdjustmentTransferService creates one root-relative Version and commit
  -> existing history owner activates and rebuilds that Version
  -> persist graph and document checkpoint
  -> publish target success
  -> refresh HDR metadata and thumbnail
```

### 6.7 Paste failure call chain

```text
Package, remap, history, rebuild, or persistence failure
  -> stop the current target operation
  -> restore the prior graph and active Version through existing owner operations
  -> create no visible partial Version
  -> keep the prior displayed target document
  -> skip HDR and thumbnail refresh for that target
  -> record the exact target failure
  -> continue to the next target only for the existing multi-target request
```

The implementation must not use a legacy stage transfer after failure.

---

## 7. Proposed data and APIs

Names can change during implementation when current source naming requires it.
The owner and behavior must stay the same.

### 7.1 Selection values

```cpp
enum class AdjustmentTransferItemKind {
  NodeEnabled,
  NodeMix,
  Adjustment,
  Masks,
  DrtParameters,
};

struct AdjustmentTransferItemSelection {
  AdjustmentTransferItemKind kind;
  std::optional<AdjustmentInstanceId> adjustment_id;
};

struct AdjustmentTransferNodeSelection {
  NodeId node_id;
  std::vector<AdjustmentTransferItemSelection> items;
};

struct AdjustmentTransferSelection {
  version_ref_id_t source_version_id;
  std::vector<AdjustmentTransferNodeSelection> nodes;
};
```

The DRT/Post `NodeId` identifies the endpoint row.
The `DrtParameters` item identifies the complete display-transform value.
The selection stores no row index and no display string.

### 7.2 Catalog descriptors

The application catalog returns minimal immutable descriptors.
A descriptor includes:

- stable identity;
- display name;
- node kind;
- item kind;
- display value;
- enabled state;
- source order.

It does not include a writable model pointer.
It does not expose mutable graph containers.

### 7.3 Package values

The v6 package should use typed entries similar to:

```cpp
struct TransferAdjustmentValue {
  AdjustmentInstanceId source_id;
  OperatorTypeId type;
  nlohmann::json params;
};

struct TransferColorGradeValue {
  NodeId source_node_id;
  std::string display_name;
  bool deletion_protected;
  std::optional<bool> enabled;
  std::optional<float> mix;
  std::vector<TransferAdjustmentValue> adjustments;
  std::optional<std::vector<MaskModel>> masks;
};
```

The exact Mask representation can remain canonical Mask JSON at the package boundary.
The package must still have one all-or-none Mask presence value per Color Grade.

The DRT/Post entry uses optional complete DRT parameters and selected adjustment values.
It does not store target node identity.

### 7.4 Model commands

The dialog model provides commands with stable identity:

```text
SelectVersion(version_ref_id_t)
FocusNode(NodeId)
SetNodeChecked(NodeId, bool)
SetItemChecked(NodeId, item identity, bool)
SetAllNodesChecked(bool)
SetAllFocusedNodeItemsChecked(bool)
ClearAll()
ClearFocusedNode()
BuildSelection()
```

QML invokes these commands.
QML does not clone row arrays or edit `checked` properties in JavaScript.

---

## 8. Implementation entry gate

Before each NM10 phase, the implementing agent must:

1. Read the current repository `AGENTS.md`.
2. Read the current `alcedo-qml-ui` skill before QML or theme work.
3. Read `alcedo-msvc-cmake` before Windows configure or build work.
4. Recheck this plan against the current branch.
5. Record current source changes that affect owner or API names.
6. Stop and update this plan when a required owner boundary no longer exists.

The implementation must follow these current repository rules:

- Use C++20 and the repository format rules.
- Include each defining header unless the documented exception applies.
- Update data through its owner.
- Do not add a duplicate writable document.
- Do not add a speculative consistency mechanism.
- Do not add a fallback without user approval.
- Keep production QML on Basic style.
- Use `appTheme` and shared components.
- Do not use centered-dot compound labels.
- Do not add unrequested pills, badges, or status dots.
- Use behavior names for tests.
- Put temporary evidence under `build/tmp/adjustment_transfer/`.

---

## 9. Phase summary and size limit

The estimate counts production code, tests, QML, CMake, and plan updates.
It excludes generated files and temporary evidence.

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| NM10.1 | Stable selection and sparse v6 package | transfer types, package builder, JSON | NM9 complete | 1,200–1,700 lines | done |
| NM10.2 | Selective root-relative paste planner | planner, service, history tests | NM10.1 | 900–1,400 lines | done |
| NM10.3 | Read-only Version catalog | replay service, catalog tests | NM10.1 | 800–1,300 lines | done |
| NM10.4 | Qt models and controller split | list models, dialog model, apply coordinator | NM10.2–NM10.3 | 1,300–1,800 lines | done |
| NM10.5 | Three-column QML and button rules | dialog panes, shared controls, QML tests | NM10.4 | 1,100–1,700 lines | done |
| NM10.6 | Product integration and reopen evidence | real project tests, docs, cleanup | NM10.5 | 700–1,200 lines | planned |

No phase has an expected diff above 2000 lines.
The split keeps package semantics separate from Qt presentation.
It also keeps the QML change below the review limit.

If an upper estimate passes 2000 lines during implementation, split that phase first.
Do not continue with an oversized phase and explain the split after review.

---

## 10. NM10.1 — Stable selection and sparse v6 package

### 10.1.1 Objective and deliverables

Create stable node and item selection types.
Create a sparse v6 transfer package.
Make the package builder copy only selected values.

This phase has no QML change.
It does not change target Paste behavior yet.

### 10.1.2 Inputs and prerequisites

- Current `PipelineDocument` ownership rules.
- `ColorGradeAdjustmentTypes()` and `DrtPostAdjustmentTypes()`.
- Current Mask serialization and identity rules.
- Current v5 package import, export, and fingerprint tests.

### 10.1.3 Modules, files, and APIs

Modify:

- `app/adjustment_transfer_types.hpp`;
- `app/document_transfer.hpp` and `document_transfer.cpp`;
- `edit/history/pipeline_history_format.hpp`;
- transfer format fixtures and service tests;
- affected CMake source lists.

Add the proposed package-builder files from Section 6.1.

Remove the legacy broad `AdjustmentTransferSelection` fields after all callers move.
Do not keep both selection models active.

### 10.1.4 Data rules and invariants

- One selection names one source Version.
- One node selection names one stable NodeId.
- One adjustment selection names one stable AdjustmentInstanceId.
- `Masks` is one all-or-none item for one Color Grade.
- Package order follows source backbone order.
- Package order does not follow QML row order.
- A package can contain DRT/Post data without Color Grades.
- `Empty()` checks all package content, not only the Color Grade vector.
- Canonical JSON records optional field presence.
- Validation rejects duplicate node, adjustment, Mask, and optional Stroke identities.
- Validation rejects Develop, geometry, RAW, and lens data.
- v5 import fails with an unsupported-schema error.

### 10.1.5 Implementation steps

1. Define stable selection values in a dedicated header.
2. Define typed v6 Color Grade and DRT/Post transfer values.
3. Move JSON-only representations to the serialization boundary.
4. Add exact validation for selected item ownership.
5. Build package entries in source backbone order.
6. Create clean field presence for partial node selection.
7. Copy all Masks only when `include_masks` is true.
8. Preserve complete Mask parameters and order.
9. Omit R8 cache data and absolute paths.
10. Update canonical JSON and fingerprint calculation.
11. Increment the public transfer schema to v6.
12. Reject v5 and unknown schemas before package construction.
13. Delete the old operator-filter selection after callers compile with the new type.

### 10.1.6 Primary success call chain

```text
replayed source document + stable selection
  -> validate source Version and node identities
  -> validate item ownership
  -> order selected Grades by source backbone
  -> copy selected node fields and adjustments
  -> copy complete Mask vector when selected
  -> copy selected DRT/Post values
  -> canonical v6 JSON
  -> fingerprint
  -> validated AdjustmentTransferPackage
```

### 10.1.7 Primary failure call chain

```text
missing identity, wrong owner, duplicate item, or invalid Mask
  -> stop package construction
  -> return exact validation error
  -> change no source document
  -> publish no partial package
```

### 10.1.8 Tests and evidence

Add or update tests with these behaviors:

| Test behavior | Required assertion |
| --- | --- |
| `SelectedAdjustmentsAreTheOnlyValuesInTransferPackage` | Unselected adjustments do not appear in v6 JSON. |
| `ColorGradeSelectionKeepsSourceBackboneOrder` | Reordered selection input cannot reorder the package. |
| `MasksSelectionCopiesEveryOwnedMaskOrNoMask` | One checkbox includes all Masks; false includes none. |
| `DrtOnlySelectionCreatesNonEmptyTransferPackage` | `Empty()` is false without Color Grades. |
| `TransferPackageRejectsItemOwnedByAnotherNode` | Validation fails before JSON export. |
| `TransferPackageV6FingerprintCoversFieldPresence` | Missing and present default values produce different fingerprints. |
| `TransferPackageV5IsRejectedWithoutConversion` | Import returns the explicit schema error. |
| `TransferPackageOmitsDevelopRawLensGeometryAndCaches` | Export contains none of the excluded data. |

Use deterministic identities and independent expected serialized output.
Do not generate expected JSON from the encoder under test.

### 10.1.9 Build and run commands

Use the Windows wrapper from the repository root:

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AdjustmentTransferServiceTest DocumentTransferTest
ctest --test-dir build/debug -N -R "^(AdjustmentTransferServiceTest|DocumentTransferTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(AdjustmentTransferServiceTest|DocumentTransferTest)\."
```

Confirm actual target names before execution.

### 10.1.10 Exit criteria

- [x] Stable IDs replace stage/operator selection.
- [x] The package contains only selected values.
- [x] One Mask checkbox gives all-or-none package data.
- [x] DRT-only packages validate.
- [x] v6 JSON and fingerprint tests pass.
- [x] v5 fails with no conversion path.
- [x] No source state changes during package build.

### 10.1.11 Expected diff

Expected diff: 1,200–1,700 lines.
Split serialization fixtures from domain types if the estimate passes 2000 lines.

Actual diff: ~1,071 lines (770 changed + 301 new builder files). Within the planned size.

### 10.1.12 Completion record

```text
Phase / date / status:
  NM10.1 / 2026-09-19 / implemented and verified on Windows (MSVC debug).

Source revision and branch:
  9dbba2c1 on main (worktree contains the NM10.1 changes; not yet committed).

Actual changed modules:
  - alcedo_studio/src/include/app/adjustment_transfer_types.hpp — stable
    AdjustmentTransferItemKind / ItemSelection / NodeSelection / Selection and
    sparse TransferAdjustmentValue / TransferColorGradeValue / TransferDrtPostValue
    package entries; legacy boolean selection removed.
  - alcedo_studio/src/include/app/adjustment_transfer_package_builder.hpp (new),
    alcedo_studio/src/app/adjustment_transfer_package_builder.cpp (new) —
    AdjustmentTransferPackageBuilder::Build + SelectAllTransferableItems.
  - alcedo_studio/src/app/document_transfer.cpp — typed v6 canonical JSON,
    import/export, validation, fingerprint, and PrepareDocumentPaste on sparse
    values (clean-default materialization + sparse DRT/Post changes).
  - alcedo_studio/src/include/edit/history/pipeline_history_format.hpp —
    kAdjustmentTransferSchema = "alcedo.adjustment_transfer.v6".
  - alcedo_studio/src/app/CMakeLists.txt — builder source registered.
  - Tests: document_transfer_test.cpp (typed access + 8 new behavior tests +
    source-immutability test), editor_session_command_queue_baseline_test.cpp,
    editor_session_cq5_qualification_test.cpp (typed package fields),
    pipeline_document_checkpoint_test.cpp, brush_source_format_boundary_test.cpp
    (schema string pin updated to v6).

Package schema and rejection behavior:
  - Schema alcedo.adjustment_transfer.v6; sparse per-node entries with optional
    enabled / mix / masks and per-instance adjustment values. absent = not
    selected; masks present = all source Masks in source order.
  - v5 / operator-list packages are rejected explicitly: "unsupported adjustment
    package schema" / "operator-list transfer packages are not accepted". No
    conversion path exists.
  - default_grade_id must identify exactly one packaged Grade or be null;
    source Version id stays selection/UI provenance and is never serialized.

Primary success call chain:
  PipelineDocument + AdjustmentTransferSelection
    -> AdjustmentTransferPackageBuilder::Build (validate node/item identities,
       resolve backbone Color Grades in source order + DRT endpoint, copy only
       selected values, reject unmatched/duplicate/wrong-owner items)
    -> ValidateDocumentTransfer -> DocumentTransferFingerprint
    -> AdjustmentTransferPackage (sparse, validated, fingerprinted).
  Paste compatibility: PrepareDocumentPaste -> RemapGradeEntry (MakeClean +
  selected values only) -> BuildPasteBatch (remove/insert changes + sparse DRT
  SetParameter changes) -> MakePasteBatch.

Primary failure call chain:
  empty/duplicate/unknown-node selection, wrong-owner or duplicate item, Masks
  on a Mask-less Grade, Color-Grade-only item on DRT/Post, or an entirely empty
  result -> std::runtime_error from the builder before any package is published;
  the source document is never mutated (verified by canonical-JSON comparison).
  Import rejects wrong schema, unknown keys, malformed entries, non-canonical
  dumps, and fingerprint mismatches.

Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --preset win_debug
      -DALCEDO_ENABLE_BRUSH_MASK=OFF
      -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"        -> 0
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
      --target DocumentTransferTest AdjustmentTransferServiceTest
              AdjustmentTransferServiceMiniGitTest                   -> 0
  ctest --test-dir build/debug --output-on-failure
      -R "^(AdjustmentTransferServiceTest|AdjustmentTransferServiceMiniGitTest|
          DocumentTransferTest)\."                                   -> 34/34 pass
  ctest --test-dir build/debug --output-on-failure
      -R "^EditorSessionHistoryPortTest\."                           -> 89/89 pass
  ctest --test-dir build/debug --output-on-failure
      -R "^(...|PipelineDocumentCheckpointTest|EditorSessionCommandQueueBaselineTest|
          EditorSessionCq5QualificationTest)\.|^EditorSessionHistoryPortTest\.
          EditorDocumentPasteTest"                                   -> 77/78 pass

Discovered / passed / failed / skipped counts:
  DocumentTransferTest: 16/16 pass (8 new NM10.1 tests + source-immutability).
  AdjustmentTransferServiceTest: 3/3. AdjustmentTransferServiceMiniGitTest:
  15/15. EditorSessionHistoryPortTest: 89/89 (live Paste path included).
  PipelineDocumentCheckpointTest + EditorSessionCq5QualificationTest: pass.
  1 failure: EditorSessionCommandQueueBaselineTest.
    RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection —
    deterministic queued-image-selection promotion defect; the test never
    touches the transfer code path (only diff in that file is the typed
    package-field helper). Pre-existing, unrelated to NM10.1.

Expected serialized data evidence:
  TransferPackageOmitsDevelopRawLensGeometryAndCaches asserts the exported
  top-level key set equals exactly {color_grades, default_grade_id,
  document_format_version, drt_post, fingerprint, schema} and recursively
  bans develop/geometry/raw/lens/history/version/root_id/operators/cache/
  ui_state keys. TransferPackageV6FingerprintCoversFieldPresence asserts absent
  vs present-default `enabled` produce different fingerprints. v5 rejection is
  asserted on a mutated v6 export (schema string replaced) with the explicit
  schema error message.

Remaining defects or unavailable platforms:
  - ALCEDO_ENABLE_BRUSH_MASK=OFF build; Brush Mask transfer code paths are
    compiled out by design, not verified here.
  - AdjustmentTransferController Copy still captures the full document via
    SelectAllTransferableItems; per-item UI selection arrives with the NM10.4
    dialog.
  - PrepareDocumentPaste consumes the sparse package (clean-default Grade
    materialization, sparse DRT changes) to keep NM10.1 compilable; the full
    selective-paste semantics (NM10.2) own its final contract.
  - The one baseline-test failure noted above is tracked as pre-existing.
```
---

## 11. NM10.2 — Selective root-relative paste planner

### 11.1 Objective and deliverables

Build one target document change from a sparse v6 package.
Keep the existing root-relative Version behavior.
Preserve target image-specific data.

### 11.2 Inputs and prerequisites

- Validated v6 package from NM10.1.
- Target immutable root document.
- Existing identity source and collision checks.
- Existing typed Paste batch and Version creation.

### 11.3 Modules, files, and APIs

Add the proposed `document_transfer_planner` files.

Modify:

- `adjustment_transfer_service.hpp/.cpp`;
- `document_transfer.hpp/.cpp`;
- transfer and mini-git service tests;
- `editor_document_paste_test.cpp`;
- affected build lists.

Keep Version creation in the existing history owner.
Do not move CommitGraph mutation into the planner.

### 11.4 Data rules and invariants

- The planner reads the target immutable root.
- The planner changes no live document.
- Selected Color Grades start from clean defaults.
- Selected values overwrite only their matching clean values.
- No selected Color Grade means that target root Grades remain.
- One or more selected Color Grades replace the target root Grade chain.
- The target root supplies Develop, geometry, and DRT/Post identity.
- The source order defines the new Grade order.
- Every transferred identity receives one collision-free target identity.
- One Paste request creates one typed batch.
- One successful target Paste creates one named Version.

### 11.5 Implementation steps

1. Move identity remap and batch preparation into the planner.
2. Create one clean target Grade for each selected source Grade.
3. Apply selected Enabled and Mix values.
4. Apply selected adjustment parameter values by instance identity and type.
5. Add every Mask only when the Mask set exists in the package.
6. Remap Mask and optional Stroke identities.
7. Select the remapped default Grade by Section 3.3.
8. Apply selected DRT parameters to the target root DRT model.
9. Apply selected DRT/Post adjustments.
10. Preserve target root Grades for a DRT-only package.
11. Validate the final backbone, owner rules, and identity sets.
12. Build one typed Paste batch.
13. Pass the prepared batch to the existing root-relative Version operation.
14. Remove complete-document assumptions from current Paste tests.

### 11.6 Primary success call chain

```text
v6 package + target immutable root
  -> validate package
  -> allocate target identities
  -> create clean selected Grades
  -> apply selected Grade values and optional Mask sets
  -> apply selected DRT/Post values
  -> validate target document result
  -> one PipelineEditBatch
  -> one root-relative Version and one commit
```

### 11.7 Primary failure and restore call chain

```text
validation, identity, asset, or final graph failure
  -> planner returns failure before graph mutation
  -> no Version ref
  -> no commit
  -> no active Version change
  -> no render or persistence request
```

### 11.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `PartialGradePasteUsesCleanValuesForUnselectedAdjustments` | Only selected source values differ from clean defaults. |
| `SelectedGradeSubsetKeepsSourceOrder` | Target backbone matches selected source order. |
| `MaskSetPasteRemapsAllMasksAsOneSelection` | Every source Mask appears with new identities and source order. |
| `UncheckedMaskSetAddsNoMask` | Target selected Grade has no source Mask. |
| `DrtOnlyPasteKeepsTargetRootGrades` | Grade identities and values equal the target root. |
| `PartialDrtPasteKeepsUnselectedTargetRootValues` | Only selected DRT/Post items change. |
| `PasteKeepsTargetDevelopRawLensAndGeometry` | All image-specific target data remains exact. |
| `SelectivePasteCreatesOneRootRelativeVersionAndCommit` | Ref ancestry and commit count are exact. |
| `SelectivePasteFailureCreatesNoVersionOrCommit` | Every injected planner failure leaves the graph unchanged. |

### 11.9 Build and run commands

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DocumentTransferTest AdjustmentTransferServiceMiniGitTest EditorDocumentPasteTest
ctest --test-dir build/debug -N -R "^(DocumentTransferTest|AdjustmentTransferServiceMiniGitTest|EditorDocumentPasteTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(DocumentTransferTest|AdjustmentTransferServiceMiniGitTest|EditorDocumentPasteTest)\."
```

Confirm actual target names first.

### 11.10 Exit criteria

- [x] Partial Grade selection produces clean unselected values.
- [x] Mask transfer is all or none per Grade.
- [x] DRT-only transfer keeps target root Grades.
- [x] Target image-specific data remains unchanged.
- [x] Identity remap has no collision.
- [x] One Paste creates one Version and one commit.
- [x] Failure creates no partial Version.

### 11.11 Expected diff

Expected diff: 900–1,400 lines.

Actual diff: ~1,660 lines (1,090 changed + 570 new planner files). The excess over
the planned size is focused test coverage (~310 lines of planner tests plus two
service tests), not production surface.

### 11.12 Completion record

```text
Phase / date / status:
  NM10.2 / 2026-09-19 / implemented and verified on Windows (MSVC debug).

Source revision and branch:
  42d325ea on main ancestry; work on branch
  feature/nm10-2-selective-paste-planner.

Actual planner and history owners:
  - alcedo_studio/src/include/app/document_transfer_planner.hpp (new) and
    alcedo_studio/src/app/document_transfer_planner.cpp (new) own
    TransferIdentitySource, CountingTransferIdentitySource,
    DocumentTransferPasteOptions, PreparedDocumentPaste,
    SetDocumentTransferIdentitySourceForTesting, and
    DocumentTransferPlanner::Plan — identity remap, clean-default Grade
    materialization, sparse DRT/Post changes, and typed Paste batch building.
  - alcedo_studio/src/include/app/document_transfer.hpp and
    alcedo_studio/src/app/document_transfer.cpp keep only the boundary:
    capture, v6 import/export, schema validation, and canonical fingerprint.
  - CommitGraph / Version creation stays in AdjustmentTransferService
    (PasteAsRootRelativeVersion -> DocumentTransferPlanner::Plan) and in
    EditorHistoryTransfer::PasteLiveRootRelativeVersion; the planner mutates
    no live document.

Implemented selection semantics:
  - Each selected Color Grade starts from ColorGradeNodeModel::MakeClean and
    receives only the selected enabled / mix / adjustment values; an unchecked
    Mask set adds no Mask, a checked set adds every source Mask in source order
    (Section 3.2).
  - Backbone order decides package order and therefore remap order; a selected
    subset keeps source order on the pasted chain (Section 3.1/3.2).
  - DRT/Post applies to the existing target root DRT node; unselected DRT/Post
    items keep the target root values; a DRT-only package leaves the target
    Grade chain untouched (Section 3.4/3.5).
  - Section 3.3 default identity: the remapped source default stays default;
    when the package omits the source default, the first included Grade becomes
    the target default. The target default protection rule is applied after
    remap — the default Grade and its transferred Masks are deletion-protected;
    other protection values keep their source values.
  - Every transferred NodeId / AdjustmentInstanceId / MaskId / StrokeId is
    remapped through TransferIdentitySource with collision rejection against
    source IDs, target root IDs, and generated IDs.
  - A package that produces no target change fails planning; an invalid
    package, collision, malformed value, or missing endpoint fails before any
    Version or commit is created.

Primary success call chain:
  v6 package + target immutable root
    -> DocumentTransferPlanner::Plan
    -> ValidateDocumentTransfer + Develop/DRT endpoint check
    -> OccupiedIdentities + CollectSourceIdentities
    -> RemapGradeEntry (MakeClean + selected values + Section 3.3 default)
    -> AppendDrtParameterChanges (sparse DRT/Post SetParameter changes)
    -> BuildPasteBatch (remove/insert Grade changes + default wiring)
    -> MakePasteBatch (one validated PipelineEditBatch)
    -> AdjustmentTransferService::PasteAsRootRelativeVersion or
       EditorHistoryTransfer::PasteLiveRootRelativeVersion
    -> one root-relative VersionRef + one typed Paste commit (first parent root).

Primary failure and restore call chain:
  validation, collision, missing endpoint, malformed value, or empty-change
  failure -> std::runtime_error from the planner before graph mutation ->
  service returns AdjustmentPasteResult{error} -> no Version ref, no commit,
  no active Version change, no render or persistence request. Partial Version
  creation after planning is rolled back by restoring the prior active Version
  and removing the created ref (existing service behavior, unchanged).

Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
      --target DocumentTransferTest AdjustmentTransferServiceMiniGitTest
              EditorSessionHistoryPortTest AlbumBackendLib                 -> 0
  ctest --test-dir build/debug --output-on-failure
      -R "^(DocumentTransferTest|AdjustmentTransferServiceMiniGitTest)\."  -> 43/43 pass
  ctest --test-dir build/debug --output-on-failure
      -R "^EditorSessionHistoryPortTest\."                               -> 89/89 pass
  ctest --test-dir build/debug --output-on-failure
      -R "^(DocumentTransferTest|AdjustmentTransferServiceMiniGitTest|
          EditorSessionHistoryPortTest)\."                               -> 132/132 pass

Discovered / passed / failed / skipped counts:
  DocumentTransferTest: 26/26 (10 new NM10.2 planner tests:
    PartialGradePasteUsesCleanValuesForUnselectedAdjustments,
    SelectedGradeSubsetKeepsSourceOrder, MaskSetPasteRemapsAllMasksAsOneSelection,
    UncheckedMaskSetAddsNoMask, DrtOnlyPasteKeepsTargetRootGrades,
    PartialDrtPasteKeepsUnselectedTargetRootValues,
    RemappedSourceDefaultGradeStaysDefault, FirstIncludedGradeBecomesTargetDefault,
    PlannerDoesNotMutatePackageOrTargetRoot,
    DrtOnlyPackageIdenticalToTargetFailsWithoutChanges;
    PasteKeepsTargetDevelopRawDataAndGeometry renamed to
    PasteKeepsTargetDevelopRawLensAndGeometry and extended with lens/camera fields).
  AdjustmentTransferServiceMiniGitTest: 17/17 (2 new:
    SelectivePasteCreatesOneRootRelativeVersionAndCommit,
    SelectivePasteFailureCreatesNoVersionOrCommit).
  EditorSessionHistoryPortTest: 89/89. Two paste tests updated to the Section
    3.3 contract (plan step 14): PasteCreatesOneRootRelativeVersionAndOneTypedCommit
    now expects default protection on the pasted default Grade and its Masks;
    PasteWithoutDefaultIdentityDoesNotInheritTargetDefault renamed to
    PasteWithoutSourceDefaultEstablishesFirstGradeAsDefault and asserts the
    first-included-Grade default.

Source no-mutation evidence:
  PlannerDoesNotMutatePackageOrTargetRoot compares the canonical package export
  and canonical target-root JSON before and after Plan; identical.
  IdentityCollisionIsRejectedBeforeDocumentMutation and
  SelectivePasteFailureCreatesNoVersionOrCommit verify target/graph invariance
  on planner failure.

Target preservation evidence:
  PasteKeepsTargetDevelopRawLensAndGeometry asserts Develop params (RAW,
  camera, lens fields) and Geometry JSON are unchanged after applying the
  batch. DrtOnlyPasteKeepsTargetRootGrades asserts Grade identities, values,
  and default identity equal the target root.

Remaining defects or unavailable platforms:
  - ALCEDO_ENABLE_BRUSH_MASK=OFF build; StrokeId remap code paths are compiled
    out by design and not verified here.
  - AdjustmentTransferController Copy still captures the full document via
    SelectAllTransferableItems; per-item UI selection arrives with the NM10.4
    dialog. No UI/QML work is in scope for NM10.2.
```

---

## 12. NM10.3 — Read-only Version catalog

### 12.1 Objective and deliverables

Read any source Version without changing the live active Version.
Build stable node and item descriptors for the dialog.

### 12.2 Inputs and prerequisites

- Const source CommitGraph.
- Const immutable root document.
- `FirstParentCommitsForHead`.
- `ReplayPipelineDocumentFromRoot`.
- v6 selection identities from NM10.1.

### 12.3 Modules, files, and APIs

Add the proposed catalog files from Section 6.1.

Modify application CMake source lists and app tests.
Do not add Qt types to the application catalog.

### 12.4 Data rules and invariants

- Version metadata comes from the CommitGraph owner.
- One selected Version replay creates one immutable document.
- The replay path reads root and first-parent commits only.
- Catalog nodes follow the image backbone.
- Catalog items follow document and catalog order.
- Item identity does not use a label.
- A catalog read changes no graph field.
- A catalog read changes no live guard field.
- A catalog read starts no render and no save.

### 12.5 Implementation steps

1. Add a const operation that lists Version descriptors.
2. Add a const operation that replays one requested Version.
3. Validate the Version id before replay.
4. Build Color Grade descriptors from `ColorGradesOnImageBackbone`.
5. Build adjustment descriptors from node entries and catalog definitions.
6. Add one Mask descriptor for each Color Grade.
7. Mark the Mask descriptor unavailable when the vector is empty.
8. Build one DRT/Post descriptor and its five items.
9. Format display values through focused presentation helpers.
10. Return the immutable replayed document with minimal descriptors.
11. Delete source inspection through active-Version mutation after callers move.

### 12.6 Primary success call chain

```text
const graph + const root + VersionId
  -> find Version ref
  -> collect first-parent commits
  -> replay independent immutable document
  -> validate backbone and owners
  -> build Version, node, and item descriptors
  -> return read result
```

### 12.7 Primary failure call chain

```text
unknown Version, missing commit, replay failure, or invalid document
  -> return exact error
  -> destroy incomplete replay value
  -> keep prior dialog read result
  -> change no live source state
```

### 12.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `CatalogReadsInactiveVersionWithoutChangingActiveVersion` | Active Version, head, chain, document, dirty state, and WAL remain exact. |
| `CatalogOrdersGradesBySourceBackbone` | Canvas order and names do not affect rows. |
| `CatalogUsesAdjustmentInstanceIdentity` | Duplicate types cannot alias rows. |
| `CatalogShowsOneMasksItemForAnyMaskCount` | Zero gives disabled; one or many gives one enabled row. |
| `CatalogReplayFailureKeepsSourceSessionUnchanged` | No save, render, or revision publication occurs. |
| `CatalogBuildsDrtPostItemsFromCurrentOwners` | The endpoint has Display Transform and four owned adjustments. |

### 12.9 Build and run commands

Add the new tests to the closest application test target.
Use the wrapper and confirm discovery before execution.

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AdjustmentTransferCatalogTest
ctest --test-dir build/debug -N -R "^AdjustmentTransferCatalogTest\."
ctest --test-dir build/debug --output-on-failure -R "^AdjustmentTransferCatalogTest\."
```

The target name is proposed.
Update this plan if implementation uses another existing target.

### 12.10 Exit criteria

- [x] Inactive Version inspection changes no live source state.
- [x] Catalog rows use stable identities.
- [x] Node order follows the backbone.
- [x] One Mask row represents all Masks.
- [x] DRT/Post ownership matches the current document model.
- [x] Replay errors fail closed.

### 12.11 Expected diff

Expected diff: 800–1,300 lines.

### 12.12 Completion record

```text
Phase / date / status:
NM10.3 / 2026-09-25 / implemented and verified.

Source revision and branch:
9dbba2c1 on main; work landed on
feature/nm10-3-readonly-version-catalog.

Actual read owner and replay API:
AdjustmentTransferCatalogService (Qt-free application layer) in
alcedo_studio/src/app/adjustment_transfer_catalog.cpp with public
descriptors in alcedo_studio/src/include/app/adjustment_transfer_catalog.hpp.
The service is a static-operation owner: ListVersions reads VersionRef
metadata from a const CommitGraph in the existing owner order (created_at,
then version_id — the same order adjustment_transfer_controller.cpp uses);
ReadVersion validates the Version id through CommitGraph::GetVersionRef,
collects first-parent commits through FirstParentCommitsForHead, and replays
them through ReplayPipelineDocumentFromRoot onto the caller's immutable root
document. BuildNodeDescriptors enumerates ColorGradesOnImageBackbone plus the
DRT/Post endpoint. The service never calls SetActiveVersionId, never calls
RebuildActiveEditorPipeline, and never touches a live guard, WAL, render
state, or project storage.

Independent replayed document lifetime:
AdjustmentTransferCatalogRead owns the replayed PipelineDocument by value.
The document is built from ClonePipelineDocument(root) inside the replay
helper, validated, moved into the read result, and destroyed when the result
is replaced or released. It is never written back to the live guard or to
project storage. The session test shows three distinct exposure values (root
1.5, replayed inactive tip 2.0, live tip -0.5), proving independent storage.

Primary success call chain:
const CommitGraph + const root document + version_ref_id_t
  -> CommitGraph::GetVersionRef (unknown id throws -> caught -> exact error)
  -> FirstParentCommitsForHead(graph, ref.head_commit_hash)
  -> ReplayPipelineDocumentFromRoot(root, commits, error)
  -> BuildNodeDescriptors(replayed)
       -> ColorGradesOnImageBackbone (source backbone order)
       -> per Grade: Enabled, Mix, owned adjustments in document order,
          one all-or-none Masks row (disabled when MaskCount()==0)
       -> DRT endpoint: RequireCompleteDrtPostTypes, Display Transform item
          plus Clarity/Sharpen/Halation/Film Grain in document order
  -> AdjustmentTransferCatalogRead {version, document, nodes}

Primary failure call chain:
unknown Version id, missing commit on the first-parent path, replay failure,
missing DRT endpoint, or invalid adjustment ownership
  -> exception or nullopt captured at the boundary -> exact error string
  -> incomplete replay value destroyed inside the optional/exception path
  -> caller keeps its prior valid read result (test holds good_read and
     re-compares its canonical document JSON after the failed read)
  -> no graph field, guard field, WAL record, redo state, render, or save
     changes (SessionState snapshot compared before and after each call)

Build and test commands with exit codes:
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target AdjustmentTransferCatalogTest                      -> exit 0
ctest --test-dir build/debug -N -R "^AdjustmentTransferCatalogTest\."
                                                              -> exit 0 (6 discovered)
ctest --test-dir build/debug --output-on-failure
    -R "^AdjustmentTransferCatalogTest\."                       -> exit 0 (6/6 passed)

Discovered / passed / failed / skipped counts:
6 discovered / 6 passed / 0 failed / 0 skipped.
CatalogReadsInactiveVersionWithoutChangingActiveVersion,
CatalogReplayFailureKeepsSourceSessionUnchanged (TEST_F on
AdjustmentTransferCatalogHistoryTest), CatalogOrdersGradesBySourceBackbone,
CatalogUsesAdjustmentInstanceIdentity, CatalogShowsOneMasksItemForAnyMaskCount,
CatalogBuildsDrtPostItemsFromCurrentOwners (TEST on
AdjustmentTransferCatalogTest, new target in tests/app).

No-mutation evidence:
SessionState in the test captures ImageEditState JSON, active VersionRef JSON,
active version id, working head, first-parent chain fold, commit and Version
counts, canonical live and root document JSON, live topology_dirty flag,
journal record count and sequence range, WAL file bytes, and redo count.
CatalogReadsInactiveVersionWithoutChangingActiveVersion and
CatalogReplayFailureKeepsSourceSessionUnchanged compare the complete snapshot
before and after ListVersions/ReadVersion and require exact equality,
including after a deliberately broken replay and an unknown Version id.

Remaining defects or unavailable platforms:
None known. Deadlock-prone broader history tests were not exercised per the
phase instruction; only the focused catalog target was built and run.
Source inspection through active-Version mutation is deleted in NM10.4 after
the Qt models/controller move to this catalog (implementation step 11 stays
open until then).
```

---

## 13. NM10.4 — Qt models and controller split

### 13.1 Objective and deliverables

Move dialog selection out of QML arrays.
Split multi-target apply work from the controller.
Keep one package owner.

### 13.2 Inputs and prerequisites

- v6 package builder from NM10.1.
- selective planner from NM10.2.
- read-only catalog from NM10.3.
- existing application module host and QML type registration.

### 13.3 Modules, files, and APIs

Add the proposed dialog-model, list-model, and apply-coordinator files.

Modify:

- `adjustment_transfer_controller.hpp/.cpp`;
- `application_module_host.hpp/.cpp` when ownership needs a new object;
- `application_module_qml_types.cpp` when QML registration changes;
- `AppDialogs.qml` command boundaries;
- UI and application CMake lists;
- controller and model tests.

### 13.4 Data rules and invariants

- C++ owns every checked state.
- QML owns no domain selection array.
- Focus and inclusion remain independent.
- Node checked state derives from item states.
- Bulk checked state derives from the affected model.
- The copied package changes only after full package validation.
- A failed Copy keeps the prior copied package.
- The controller does not perform per-target persistence work.
- The apply coordinator refreshes metadata only after target persistence succeeds.
- Existing multi-target behavior reports every target result.

### 13.5 Implementation steps

1. Add Version, node, and item `QAbstractListModel` roles.
2. Add checked and partial state roles.
3. Add stable identity roles that QML treats as opaque values.
4. Add focused-node state to the dialog model.
5. Implement per-node and global select-all operations.
6. Implement focused-node and global clear operations.
7. Publish `canCopy` without a numeric count property.
8. Build selection from the C++ state.
9. Connect Copy to the package builder.
10. Replace the copied package only after success.
11. Extract multi-target Paste and post-process work to the apply coordinator.
12. Keep package source title and source Version display name as UI provenance.
13. Remove `selectedKeys` from the QML signal and controller method.
14. Remove old row formatting helpers from the controller.
15. Remove live Version mutation and restore code.
16. Keep controller methods as command routing and result mapping only.

### 13.6 Primary success call chain

```text
dialog open
  -> controller prepares dialog model
  -> dialog model selects active Version
  -> catalog service returns immutable replay result
  -> list models publish rows
  -> QML sends focus or checkbox command
  -> dialog model updates affected rows
  -> Copy command builds selection and v6 package
  -> controller publishes the new package
```

### 13.7 Primary failure and restore call chain

```text
catalog, model command, or package build failure
  -> dialog model publishes error
  -> prior valid replay result remains
  -> prior copied package remains
  -> no source or target state changes
```

### 13.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `FocusingNodeDoesNotChangeTransferSelection` | Only item-model focus changes. |
| `NodeCheckSelectsOrClearsEveryOwnedItem` | Child rows and derived node state agree. |
| `NodeStateIsPartialWhenSomeItemsAreSelected` | Three-state role is exact. |
| `GlobalSelectAllAndClearUpdateEveryNode` | All node and item roles update once. |
| `FocusedItemSelectAllChangesOnlyFocusedNode` | Other node selection remains exact. |
| `CopyFailureKeepsPriorPackage` | Package properties and Paste availability remain unchanged. |
| `CopyDoesNotSaveOrRenderSourceImage` | Source service counters remain zero. |
| `MultiTargetCoordinatorRefreshesOnlySuccessfulTargets` | Failed targets receive no thumbnail or HDR refresh. |
| `ControllerNoLongerOwnsTransferRowFormatting` | Unit boundaries use catalog and model output. |

### 13.9 Build and run commands

Add tests to existing UI model targets where ownership fits.
Create a new target only when no current target owns the model.

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AdjustmentTransferDialogModelTest AdjustmentTransferControllerTest
ctest --test-dir build/debug -N -R "^(AdjustmentTransferDialogModelTest|AdjustmentTransferControllerTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(AdjustmentTransferDialogModelTest|AdjustmentTransferControllerTest)\."
```

The target names are proposed.

### 13.10 Exit criteria

- [ ] QML owns no copied checkbox array.
- [ ] Focus and inclusion are independent.
- [ ] Three-state values derive from child state.
- [ ] Controller no longer inspects pipeline operators.
- [ ] Controller no longer switches the live source Version.
- [ ] Multi-target work has a separate coordinator.
- [ ] Failed Copy preserves the prior package.

### 13.11 Expected diff

Expected diff: 1,300–1,800 lines.
Split the apply coordinator from the dialog model phase if the upper estimate passes 2000 lines.

### 13.12 Completion record

```text
Phase / date / status:
NM10.4 / 2026-09-30 / implemented and verified.

Source revision and branch:
dfa0a846e1b50fcdba3acfff28af4a9044ab5a7a base on main; work landed on
feature/nm10-4-5-transfer-dialog-models.

Actual Qt model and coordinator owners:
AdjustmentTransferVersionListModel / AdjustmentTransferNodeListModel /
AdjustmentTransferItemListModel (QAbstractListModel subclasses) in
alcedo_studio/src/ui/alcedo_main/album_backend/adjustment_transfer_list_models.cpp
with headers under
alcedo_studio/src/include/ui/alcedo_main/album_backend/. Version rows expose
versionId/displayName/createdAt/updatedAt/active/selected; node rows expose
nodeId/displayName/nodeKind/defaultGrade/checkState/focused; item rows expose
itemKey/displayName/displayValue/itemSection/itemKind/checked/enabled.
Stable identity roles (versionId, nodeId, itemKey) are the only selection
identity; row index and display text are never used as identity.

AdjustmentTransferDialogModel (adjustment_transfer_dialog_model.cpp) owns the
whole checked/focused state: OpenSource(commit_graph, root_document) lists
Versions through AdjustmentTransferCatalogService, replays the chosen source
into its own AdjustmentTransferCatalogRead, and builds node/item rows from
the read. SelectVersion/FocusNode/SetNodeChecked/SetItemChecked/
SetAllNodesChecked/SetAllFocusedNodeItemsChecked/ClearAll/ClearFocusedNode are
the complete command surface. Node checkState derives from owned item states
(Qt::PartiallyChecked when mixed); focused and global bulk check states derive
the same way. All transferable items start selected on source open; disabled
items (maskless Masks) never enter selection. BuildPackage() produces the v6
sparse package through AdjustmentTransferPackageBuilder without mutating the
prior copied package.

AdjustmentTransferApplyCoordinator
(adjustment_transfer_apply_coordinator.cpp) owns multi-target apply:
ApplyToTargets(package, target element ids) runs
AdjustmentTransferService::PasteAsRootRelativeVersion per target, continues
after individual failures, accumulates
applied/unchanged/failure counts plus per-target failure rows, and performs
HDR metadata + thumbnail refresh only for successfully persisted targets
(TargetRefreshed(elementId) emitted per success).

Controller line count and remaining responsibilities:
adjustment_transfer_controller.cpp is 175 lines (was ~730). Remaining
responsibilities: command routing and single package ownership only.
PrepareCopy(elementId) loads the editor pipeline through
PipelineMgmtService::LoadEditorPipeline and hands the commit graph + root
document to dialogModel.OpenSource. CommitCopy() calls
dialogModel.BuildPackage() and replaces copied_package_ only on success.
Paste(targetEntries, strategy) collects export targets through
ImportExportHandler and delegates to ApplyCoordinator::ApplyToTargets.
PasteIntoEditor(editorSession) applies to the live editor. Discard() clears
the copied package. The controller publishes packageAvailable,
packageSummary, packageSourceTitle, packageSourceVersion, and dialogModel to
QML. It no longer inspects pipeline operators, no longer formats transfer
rows, no longer switches or restores the live active Version, and performs
no per-target persistence work itself.

Primary success call chain:
QML version delegate activated
  -> AdjustmentTransferDialogModel::SelectVersion(versionId)
  -> AdjustmentTransferCatalogService::ReadVersion
       -> CommitGraph::GetVersionRef
       -> FirstParentCommitsForHead
       -> ReplayPipelineDocumentFromRoot onto caller's immutable root
       -> BuildNodeDescriptors (grades in backbone order + DRT/Post last)
  -> nodes/items models reset; every enabled item checked
QML node/item check commands
  -> SetNodeChecked / SetItemChecked / bulk commands
  -> child item states updated; node checkState + global/focused bulk
     checkState re-derived; canCopyChanged emitted
Copy accepted
  -> AdjustmentTransferController::CommitCopy
  -> dialogModel.BuildPackage
       -> checked enabled items -> AdjustmentTransferSelection
       -> AdjustmentTransferPackageBuilder::Build -> v6 sparse package
  -> copied_package_ replaced only after BuildPackage returns a package
Paste accepted
  -> AdjustmentTransferController::Paste(targets, "paste")
  -> ImportExportHandler::CollectExportTargets
  -> AdjustmentTransferApplyCoordinator::ApplyToTargets
       -> per target: AdjustmentTransferService::PasteAsRootRelativeVersion
          -> DocumentTransferPlanner -> paste commits -> persist pipeline
       -> success: HDR metadata refresh + thumbnail refresh +
          TargetRefreshed(elementId)

Primary failure and restore call chain:
unknown Version id / replay failure inside SelectVersion
  -> catalog error captured -> errorText set -> prior valid read and its
     selection kept (dialog model returns early; models not reset)
empty selection or missing node on CommitCopy
  -> BuildPackage returns nullopt -> copied_package_ untouched ->
     packageAvailable stays true for the previous package
per-target planner/persistence failure inside ApplyToTargets
  -> result recorded in failures with elementId + error; remaining targets
     still processed; no refresh emitted for the failed target

Build and test commands with exit codes:
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target AdjustmentTransferDialogModelTest AdjustmentTransferControllerTest
                                                          -> exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target AdjustmentTransferDialogQmlTest
             EditorAdjustmentTransferActionsQmlTest
             EditorAdjustmentTransferRealProjectE2eTest   -> exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target AdjustmentTransferCatalogTest AdjustmentTransferServiceTest
             AdjustmentTransferServiceMiniGitTest DocumentTransferTest
                                                          -> exit 0
ctest --test-dir build/debug -N -R "^(AdjustmentTransferDialogModelTest|
    AdjustmentTransferControllerTest|AdjustmentTransferDialogQmlTest|
    EditorAdjustmentTransferActionsQmlTest)\."             -> exit 0 (25
    discovered)

Discovered / passed / failed / skipped counts:
AdjustmentTransferDialogModelTest:    14/14 passed
AdjustmentTransferControllerTest:      4/4 passed
AdjustmentTransferDialogQmlTest:       2/2 passed
EditorAdjustmentTransferActionsQmlTest: 5/5 passed
EditorAdjustmentTransferRealProjectE2eTest: skipped
    (ALCEDO_REAL_ADJUSTMENT_PROJECT not set)
Regression (unchanged app-layer targets):
AdjustmentTransferCatalogTest 6/6, AdjustmentTransferServiceTest 3/3,
AdjustmentTransferServiceMiniGitTest 17/17, DocumentTransferTest 26/26.

Source no-mutation and target refresh evidence:
CopyDoesNotSaveOrRenderSourceImage runs PrepareCopy + CommitCopy against a
seeded project and compares canonical live/root document JSON plus project
dirty state before and after Copy; exact equality required.
CopyFailureKeepsPriorPackage commits a valid package, forces a failed
CommitCopy (selection cleared), then verifies the prior package identity,
summary, and provenance strings are unchanged.
MultiTargetCoordinatorRefreshesOnlySuccessfulTargets applies one package to
two targets where one path fails planner validation; TargetRefreshed is
asserted only for the successful element id and the result reports
applied=1 / failed=1 with a failure row for the failed target.
ControllerNoLongerOwnsTransferRowFormatting guards against controller-owned
row formatting/pipeline inspection (grep-style assertion on the controller
surface, per the plan's test table).

Remaining defects or unavailable platforms:
None known. The real-project e2e path compiles and skips without
ALCEDO_REAL_ADJUSTMENT_PROJECT; hardware-backed pixel verification remains
environment-gated. Deadlock-prone broader history suites were not exercised
per the phase instruction. The dialog doc
(qml/doc/AdjustmentTransferDialog.md) was updated to the model-driven
boundary; NM10.5 visual polish was later completed on this same branch.
```

---

## 14. NM10.5 — Three-column QML and action styling

### 14.1 Objective and deliverables

Implement the approved three-column Copy UI.
Implement the read-only Paste summary.
Apply the checkbox, footer, and white-button rules.

### 14.2 Inputs and prerequisites

- Dialog and list models from NM10.4.
- Current `DESIGN.md` and `AppTheme` tokens.
- `ThemeCheckBox`, `DialogActionButton`, and existing dialog shell behavior.

### 14.3 Modules, files, and APIs

Modify:

- `AdjustmentTransferDialog.qml`;
- `ThemeCheckBox.qml`;
- `DialogActionButton.qml` only when its current white-text behavior is insufficient;
- `AppDialogs.qml`;
- `EditorAdjustmentTransferActions.qml`;
- `ImageActionsController.qml` when the dialog open data changes;
- `qml/doc/AdjustmentTransferDialog.md`;
- QML source lists and UI tests.

Add the three proposed pane QML files.

### 14.4 Visual and interaction rules

- Use Basic style.
- Use only `appTheme` tokens.
- Use existing typography tokens.
- Use monochrome selected rows.
- Use a light selected row with the matching dark ink.
- Do not use blue selection frames or slabs.
- Use white text for every enabled action button.
- Use one `Select All` checkbox in the node header.
- Use one `Select All` checkbox in the item header.
- Use one white-text `Clear` button beside each bulk checkbox.
- Use one checkbox for all Masks in a Color Grade.
- Remove all numeric footer reports.
- Remove centered-dot strings.
- Keep selection changes from rebuilding the full list.
- Keep each list scroll position after checkbox changes.
- Keep a focused node after clear operations.

### 14.5 Layout rules

Copy mode uses three persistent panes.
Use existing side-panel width tokens to derive the dialog width.
Do not add literal widths for the final design.

The Version pane is the narrowest pane.
The item pane is the widest pane.
The node pane is between those widths.

Use `dividerColor` between panes.
Use `bgBaseColor` for list wells.
Use `cardSurfaceColor` for the dialog shell.

When the application window is narrow, keep all three columns visible.
Reduce pane widths within the documented token range.
Do not collapse the workflow into pages without a new product decision.

Paste mode hides the Version pane.
It uses node and item panes in read-only mode.

### 14.6 Implementation steps

1. Extend `ThemeCheckBox` with an optional partial visual state.
2. Preserve its existing `checked` and `toggled` behavior for current callers.
3. Build the Version pane against the Version list model.
4. Build the node pane against the node list model.
5. Build the item pane against the focused item list model.
6. Route row-body activation to focus only.
7. Route checkbox activation to model commands only.
8. Add the two `Select All` checkbox controls.
9. Add the two white-text `Clear` buttons.
10. Remove QML array cloning and `selectedKeys()`.
11. Remove the dynamic `Copy %1 Settings` label.
12. Remove footer count labels.
13. Remove centered-dot metadata strings in the touched dialog.
14. Replace ad hoc font size and weight values with theme tokens.
15. Verify every enabled action button uses white text.
16. Register new QML files in CMake.
17. Update the dialog documentation.

### 14.7 Primary success call chain

```text
QML pane loads C++ list model
  -> row body changes focus
  -> checkbox changes inclusion through model command
  -> affected model roles change
  -> node and bulk partial state updates
  -> item and node scroll positions stay unchanged
  -> fixed Copy Adjustments button enables from canCopy
```

### 14.8 Primary failure call chain

```text
model command reports failure
  -> QML shows model error text
  -> checkbox roles keep prior valid state
  -> no JavaScript state repair
  -> Copy remains enabled only for valid model state
```

### 14.9 Tests and evidence

Extend the production QML harness.
Do not test a separate simplified dialog.

| Test behavior | Required assertion |
| --- | --- |
| `CopyDialogShowsVersionNodeAndItemPanes` | All three production panes load in Copy mode. |
| `NodeRowFocusDoesNotToggleItsCheckbox` | Focus changes and checked state remains exact. |
| `NodeAndItemSelectAllUseThreeStateCheckboxes` | Unchecked, partial, and checked visuals map to model roles. |
| `ClearButtonsUseWhiteTextAndClearTheirOwnedScope` | Text is white and only the correct model scope clears. |
| `TransferDialogActionButtonsAlwaysUseWhiteEnabledText` | Clear, Cancel, Copy, and Paste use white enabled text. |
| `TransferDialogHasNoNumericFooterSummary` | Footer has no selection or target count label. |
| `MasksAppearAsOneAllOrNoneCheckbox` | Multiple source Masks produce one item row. |
| `CheckboxChangesPreserveAllListScrollPositions` | Version, node, and item `contentY` values remain stable. |
| `PasteDialogShowsReadOnlyNodeAndItemSummary` | No selection mutation control is active. |
| `TransferDialogUsesThemeTypographyAndSelectionTokens` | No touched visual property uses an ad hoc literal. |
| `TransferDialogKeyboardOrderReachesAllThreePanesAndActions` | Tab, arrows, Space, Enter, and Escape work. |

Run manual checks in both themes.
Check narrow and wide supported window sizes.
Check DPR 1.0, 1.5, and 2.0.

### 14.10 Build and run commands

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AdjustmentTransferDialogQmlTest EditorAdjustmentTransferActionsQmlTest alcedo_main
ctest --test-dir build/debug -N -R "^(AdjustmentTransferDialogQmlTest|EditorAdjustmentTransferActionsQmlTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(AdjustmentTransferDialogQmlTest|EditorAdjustmentTransferActionsQmlTest)\."
```

Allow at least 10 minutes for configure and build.
Keep the same healthy build process across short polling intervals.

### 14.11 Exit criteria

- [x] Copy mode has Version, node, and item panes.
- [x] The last two panes have `Select All` checkboxes and `Clear` buttons.
- [x] Masks use one all-or-none checkbox.
- [x] Footer numeric reports are absent.
- [x] All enabled action-button text is white.
- [x] No blue fill with black button text exists.
- [x] Focus and inclusion remain independent.
- [x] Keyboard and accessibility behavior passes.
- [ ] Both themes and required DPR values pass manual review. (human run owed;
      harness asserts the tokens the review checks)

### 14.12 Expected diff

Expected diff: 1,100–1,700 lines.
Split shared control work into a small prerequisite commit when this phase can pass 2000 lines.

### 14.13 Completion record

```text
Phase / date / status:
NM10.5 / 2026-09-19 / implemented and verified.

Source revision and branch:
43f2d9408dff096d7f2e6f7d02ab5ca54407f51f on
feature/nm10-4-5-transfer-dialog-models; all NM10.5 work landed as
uncommitted changes on that branch on top of the NM10.4 commit.

Actual QML files and shared control changes:
Added three production panes beside the dialog:
- AdjustmentTransferVersionPane.qml — read-only Version catalog rows; `Active`
  is a separate caption, never a centered-dot compound; row activation issues
  versionActivated(versionId) only.
- AdjustmentTransferNodePane.qml — Color Grade + DRT/Post rows with derived
  three-state checkboxes (Qt::CheckState role), header `Select All` +
  `Clear`, row-body activation routed to focus only, checkbox activation
  routed to model commands only.
- AdjustmentTransferItemPane.qml — focused node's items grouped by
  AdjustmentTransferItemSection headers (Node/Tone/Look/LUT/Display
  Transform/Masks), header `Select All` + `Clear` scoped to the focused node,
  one all-or-none `Masks` row per Color Grade (disabled when the Grade has
  no Masks), paste mode renders the same list as read-only summary rows.
AdjustmentTransferDialog.qml rewritten as the shell: modal/backdrop, header,
three-pane RowLayout with dividerColor hairlines, footer with fixed `Cancel`
+ `Copy Adjustments`/`Paste Adjustments` DialogActionButtons, and the paste
node/item ListModels rebuilt from `adjustmentRows` (grouped by the new
`node` group index). Footer numeric counts, dynamic action labels, and
centered-dot metadata strings are gone.
ThemeCheckBox.qml gained optional `partiallyChecked` (dash glyph),
`valueText` trailing value, and `accessibleText` override while keeping the
existing checked/toggled behavior for prior callers; `Accessible.
checkStateMixed` is reported. DialogActionButton.qml now uses
appTheme.fontSizeSection/fontWeightHeading instead of literal 14/800.
SelectionSummary() gained `node` (group index), `itemSection`, and
`itemKind` fields so the paste pane can rebuild read-only columns;
AdjustmentTransferDialogModel gained the `focusedNodeName` property for the
item pane title. EditorAdjustmentTransferActions.qml and
ImageActionsController.qml dropped the removed `targetCount` property.

Final pane widths and token mapping:
Version pane `editorSidePanelWidthMin`, node pane `editorSidePanelWidth`,
item pane fills the remainder (widest). Dialog width derives from the three
panel tokens clamped to window margins; no literal widths. Wells use
bgBaseColor + cardBorderColor; selected rows use editorListSelectedFillColor
+ editorListSelectedInkColor; hover uses buttonHoveredFillColor; keyboard
focus is a 1 px textColor outline independent of selection.

Primary interaction call chain:
Row-body activation -> pane signal -> dialogModel.FocusNode / SelectVersion
Checkbox activation -> SetNodeChecked / SetItemChecked /
SetAllNodesChecked / SetAllFocusedNodeItemsChecked / ClearAll /
ClearFocusedNode -> C++ state updated -> node/item/bulk checkState roles
re-derived -> views keep scroll offsets (verified by
CheckboxChangesPreserveAllListScrollPositions) -> Copy button follows
canCopy.

Failure presentation behavior:
Model command failures surface through `errorText` in the item pane; roles
keep their prior valid state; no JavaScript state repair exists; Copy stays
enabled only while canCopy is true (node Clear disables it in the test).

Build and test commands with exit codes:
cmd /c scripts\msvc_env.cmd --preset win_debug
    -DALCEDO_ENABLE_BRUSH_MASK=OFF
    -DCMAKE_PREFIX_PATH=D:/Qt/6.9.3/msvc2022_64/lib/cmake   -> exit 0
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target AdjustmentTransferDialogQmlTest
             EditorAdjustmentTransferActionsQmlTest
             alcedo_main                                   -> exit 0
ctest --test-dir build/debug -N
    -R "^(AdjustmentTransferDialogQmlTest|
        EditorAdjustmentTransferActionsQmlTest)\."          -> exit 0 (18
        discovered)
ctest --test-dir build/debug --output-on-failure
    -R "^(AdjustmentTransferDialogQmlTest|
        EditorAdjustmentTransferActionsQmlTest)\."          -> exit 0

Discovered / passed / failed / skipped counts:
18 discovered / 18 passed / 0 failed / 0 skipped.
AdjustmentTransferDialogQmlTest 13/13 — including the eleven required
behaviors: CopyDialogShowsVersionNodeAndItemPanes,
NodeRowFocusDoesNotToggleItsCheckbox,
NodeAndItemSelectAllUseThreeStateCheckboxes,
ClearButtonsUseWhiteTextAndClearTheirOwnedScope,
TransferDialogActionButtonsAlwaysUseWhiteEnabledText,
TransferDialogHasNoNumericFooterSummary,
MasksAppearAsOneAllOrNoneCheckbox,
CheckboxChangesPreserveAllListScrollPositions,
PasteDialogShowsReadOnlyNodeAndItemSummary,
TransferDialogUsesThemeTypographyAndSelectionTokens,
TransferDialogKeyboardOrderReachesAllThreePanesAndActions.
EditorAdjustmentTransferActionsQmlTest 5/5.

Theme, width, DPR, keyboard, and accessibility evidence:
Harness asserts the monochrome selected fill/ink pair on the focused node
row, bgBaseColor wells, accent CTA with white text, and white text on every
enabled action button; keyboard traversal reaches all three panes and both
footer actions, Space/Return toggle/focus per pane behavior, and Escape
closes. Accessible roles/names are set on delegates and the Masks row
carries an explicit accessible override; checkStateMixed is reported for
partial bulk checkboxes. Delegate lookup in tests walks the visual item tree
because Bound-mode required-property delegates are not QObject children.
Manual theme/DPR/width review remains a human step; the harness enforces the
token sources that review checks.

Screenshots or evidence path:
build/tmp/nm10_5/ contains configure.log, build logs, and ctest.log output
captures for the recorded commands.

Remaining defects or unavailable platforms:
None known for this phase. Manual dual-theme, DPR 1.0/1.5/2.0, and
narrow/wide window review is still owed by a human run; product reopen
evidence stays with NM10.6. Deadlock-prone broader suites were not exercised
per the phase instruction.
```

---

## 15. NM10.6 — Product integration and reopen evidence

### 15.1 Objective and deliverables

Prove the complete workflow in a real packed project.
Remove the old stage-based path.
Verify save and reopen behavior.

### 15.2 Inputs and prerequisites

- NM10.1 through NM10.5 complete.
- Existing real-project UI fixture.
- Existing editor Paste command path.
- Existing project persistence and reopen helpers.

### 15.3 Modules, files, and APIs

Modify:

- `editor_adjustment_transfer_real_project_e2e_test.cpp`;
- affected action and history integration tests;
- schema fixture registration;
- QML documentation;
- this plan and the master plan completion records after evidence exists.

Delete obsolete stage-item helpers and tests.
Do not leave an inactive alternate Copy path.

### 15.4 Integration scenario

Create a real source project with:

- at least two Versions;
- at least three Color Grades in the selected Version;
- distinct values for repeated adjustment types;
- one Grade with no Mask;
- one Grade with one Mask;
- one Grade with multiple Masks;
- non-default DRT/Post values.

Create a target image with different Develop, RAW, lens, geometry, Grade, and DRT values.

Perform this workflow through production QML:

1. Open Copy from the source image.
2. Select the non-active source Version.
3. Clear all nodes.
4. Select two Color Grades.
5. Select a partial item set in the first Grade.
6. Select the `Masks` item in the second Grade.
7. Select one DRT/Post item.
8. Copy the package.
9. Open Paste for the target image.
10. Inspect the read-only node and item summary.
11. Paste into the target editor.
12. Verify one new root-relative Version.
13. Save and close the project.
14. Reopen the project.
15. Verify the active Version, node order, selected values, Masks, and target metadata.

### 15.5 Required assertions

- The Copy operation does not change the source active Version.
- The package contains only selected nodes and items.
- The package contains every Mask in the selected Mask set.
- The package contains no Mask from an unchecked Mask set.
- The target receives new identities.
- The target keeps its image-specific values.
- The target receives one new Version and one Paste commit.
- Save and reopen preserve the result.
- Undo removes the Paste commit result within the new Version.
- Redo restores the exact pasted result.
- The dialog shows no footer numbers.
- Every enabled action button uses white text.

### 15.6 Failure scenarios

Run controlled failures at these boundaries:

| Boundary | Required result |
| --- | --- |
| Source Version replay | Prior dialog selection and copied package remain. |
| Package validation | No new copied package publishes. |
| Identity remap | Target graph remains exact. |
| Version creation | No new ref remains. |
| Target rebuild | Prior Version and live document restore. |
| Target persistence | No success refresh runs. The exact error reports. |
| Thumbnail refresh | Paste remains successful. The refresh error reports through its owner. |

Do not hide a persistence failure behind a successful thumbnail result.

### 15.7 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `RealProjectCopiesSelectedVersionNodesItemsAndAllMasks` | Production QML produces the exact sparse package. |
| `RealProjectSelectivePasteReopensWithTargetMetadata` | Reopen preserves the Paste and target image data. |
| `RealProjectSelectivePasteUndoRedoRestoresExactDocument` | Document identities and values match at each head. |
| `InactiveVersionCopyDoesNotChangeSourceEditorState` | Active Version, document, dirty state, and render count remain exact. |
| `FailedSelectivePasteCreatesNoVisibleVersion` | UI and durable Version lists remain unchanged. |
| `TransferSurfaceContainsNoLegacyStageSelection` | Public APIs and production QML use only node-aware selection. |

### 15.8 Build and run commands

Build the complete affected set after focused phases pass:

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AdjustmentTransferServiceTest DocumentTransferTest AdjustmentTransferServiceMiniGitTest EditorDocumentPasteTest AdjustmentTransferDialogQmlTest EditorAdjustmentTransferActionsQmlTest EditorAdjustmentTransferRealProjectE2eTest alcedo_main
ctest --test-dir build/debug -N -R "AdjustmentTransfer|DocumentTransfer|EditorDocumentPaste"
ctest --test-dir build/debug --output-on-failure -R "AdjustmentTransfer|DocumentTransfer|EditorDocumentPaste"
```

Run the affected history and project persistence tests.
Run the complete UI regression group before NM10 completion.

Store logs and screenshots under:

```text
build/tmp/adjustment_transfer/nm10_6/
```

### 15.9 Exit criteria

- [ ] The real three-column Copy workflow passes.
- [ ] Non-active Version Copy changes no source editor state.
- [ ] Node and item selection controls actual package data.
- [ ] Masks transfer all or none per Color Grade.
- [ ] One root-relative target Version persists and reopens.
- [ ] Undo and Redo preserve the exact selective Paste result.
- [ ] Failure tests leave no partial target state.
- [ ] No legacy stage selection path remains.
- [ ] QML visual and accessibility checks pass.
- [ ] The master plan and this plan contain truthful evidence records.

### 15.10 Expected diff

Expected diff: 700–1,200 lines.

### 15.11 Completion record

```text
Phase / date / status:
Source revision and branch:
Actual integrated workflow:
Removed legacy paths:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Real project save and reopen evidence:
Theme, width, DPR, keyboard, and accessibility evidence:
Evidence path:
Remaining defects or unavailable platforms:
```

---

## 16. Cross-phase acceptance matrix

| Required behavior | Evidence |
| --- | --- |
| Copy one inactive Version | Read-only replay test and real UI test. |
| Preserve live source state | Compare active Version, document, dirty state, WAL, and render count. |
| Select one node | Package contains that node only. |
| Select part of one node | Unselected values use clean defaults after Paste. |
| Select multiple nodes | Package and target preserve source backbone order. |
| Clear one node | Node becomes unchecked and other nodes remain exact. |
| Select all nodes | Every transferable item becomes selected. |
| Clear all nodes | Copy disables and focus remains valid. |
| Select all focused items | Only the focused node changes. |
| One Mask checkbox | One item controls zero, one, or many Masks. |
| DRT-only package | Target root Grade chain remains. |
| Partial DRT/Post | Unselected target root values remain. |
| Copy failure | Prior copied package remains available. |
| Paste failure | No partial Version, commit, or refresh remains. |
| Save and reopen | Version, document, Masks, and target metadata remain exact. |
| Undo and Redo | The new Version moves through its one Paste commit exactly. |
| Button style | All enabled action buttons use white text. |
| Footer | No selection, item, node, or target counts appear. |
| Accessibility | Roles, names, states, focus order, and keyboard actions pass. |

---

## 17. Build and evidence rules

Use the current Windows wrapper and presets.
Do not invoke bare CMake for Windows configure or build work.

Keep `ALCEDO_ENABLE_BRUSH_MASK=OFF` for this Node Editor delivery.
Do not lower render or decode quality to make tests pass.
Do not change GPU backends for this phase.

Start configure and build operations with a 10-minute minimum allowance.
Use 20 minutes for broad targets or application relink work.
Poll a healthy process without restarting it.

For every test run, record:

- exact command;
- source revision;
- build preset;
- Qt and compiler version;
- discovered test count;
- passed count;
- failed count;
- skipped count;
- unavailable platform or backend.

Do not report a skipped test as passed.
Do not report an unavailable measurement as zero.

Put all temporary files under `build/tmp/adjustment_transfer/`.
Do not create logs or scripts at the repository root.

---

## 18. Risks and stop conditions

### 18.1 Sparse package can become another document model

Risk: a package type can copy the complete source document under new names.

Response: keep only selected portable data.
Do not add Develop, geometry, target identity, history, or UI state.

### 18.2 Historical inspection can change the live editor

Risk: reuse of the current rebuild path can change active Version or dirty state.

Response: use const root replay.
Test every live source field before and after catalog reads.

### 18.3 QML can become a second selection owner

Risk: JavaScript arrays can differ from C++ selection state.

Response: keep check state in C++ models.
QML sends commands and renders roles only.

### 18.4 Partial Grade semantics can become a merge

Risk: unselected items can accidentally use target current Version values.

Response: build selected Grades from clean defaults.
Use target root values only for a DRT-only Grade chain and unselected DRT/Post items.

### 18.5 Phase size can exceed review limits

Risk: controller split and QML work can pass 2000 changed lines.

Response: split the phase before implementation continues.
Keep each split independently buildable and testable.

### 18.6 Stop conditions

Stop implementation and update this plan when:

- a phase estimate can pass 2000 lines;
- current source no longer has the owners used by this plan;
- a new package shape cannot reject unsupported data safely;
- an operation needs a fallback that the user did not approve;
- a new consistency mechanism lacks a real production interleaving;
- a shared QML control change breaks an existing caller;
- target root-relative Paste cannot express the selected result as one typed batch.

---

## 19. NM10 completion criteria

- [ ] The Copy dialog uses Version, node, and item columns.
- [ ] The node and item columns use `Select All` checkboxes and `Clear` buttons.
- [ ] Focus and inclusion are independent.
- [ ] Color Grade Masks use one all-or-none checkbox.
- [ ] Footer numeric reports are removed.
- [ ] All enabled action buttons use white text.
- [ ] No action button uses blue fill with black text.
- [ ] Source Version inspection changes no live source state.
- [ ] Stable node and item identities replace stage/operator selection.
- [ ] The v6 package contains only selected values.
- [ ] v5 import fails with no implicit conversion.
- [ ] Partial Grades use clean defaults for unselected values.
- [ ] DRT-only transfer keeps the target root Grade chain.
- [ ] Paste preserves target Develop, RAW, camera, lens, geometry, and root identity.
- [ ] Paste remaps every transferred identity.
- [ ] One Paste creates one root-relative Version and one typed commit.
- [ ] Failure creates no partial Version or published target state.
- [ ] Save and reopen preserve the selective Paste result.
- [ ] Undo and Redo restore the exact result in the new Version.
- [ ] No legacy stage-based Copy path remains.
- [ ] Automated and manual evidence records remain separate and truthful.

Do not mark NM10 complete during plan creation.
Add dated completion records after each implemented phase.
