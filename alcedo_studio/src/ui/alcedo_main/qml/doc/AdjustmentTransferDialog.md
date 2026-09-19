# AdjustmentTransferDialog

## Component Overview

`AdjustmentTransferDialog` is the Alcedo Studio modal for copying selected adjustment
values from a named image Version or confirming a previously captured package for one or more
target images. In copy mode it presents three panes bound to a C++ dialog model: source
Versions, transferable nodes, and the focused node's transferable items. In paste mode it
presents the captured package as read-only node and item summaries over the same pane
geometry.

The component intentionally does not render Version thumbnails. Its rows are metadata and
value summaries supplied by `AdjustmentTransferController` and
`AdjustmentTransferDialogModel`.

## Project Structure and Dependencies

`AppDialogs.qml` owns the production instance. It binds
`appModules.adjustmentTransfer.dialogModel`, forwards copy acceptance to
`AdjustmentTransferController.CommitCopy()`, and routes paste acceptance through
`EditorAdjustmentTransferActions`.

The dialog shell composes three production pane components declared next to it:

- `AdjustmentTransferVersionPane.qml` — read-only Version catalog rows.
- `AdjustmentTransferNodePane.qml` — transferable nodes with derived three-state
  checkboxes, a `Select All` checkbox, and a `Clear` action (copy mode).
- `AdjustmentTransferItemPane.qml` — items of the focused node grouped by
  Adjustment Stack sections, with its own `Select All` and `Clear` scope
  (copy mode). The single all-or-none `Masks` row lives here.

Both list panes are reused in paste mode with `readOnly: true`: the header
controls and checkboxes disappear and node focus only swaps the read-only item
column. The paste pane models are local `ListModel`s rebuilt from
`adjustmentRows` by the shell; no selection mutation exists.

The component imports Qt Quick, Qt Quick Controls Basic, Qt Quick Effects, and Qt Quick
Layouts. It consumes the application-provided `appTheme` object and composes the shared
`ThemeCheckBox` and `DialogActionButton` controls. All four files are registered in
`ALCEDO_MAIN_QML_FILES` in the `alcedo_main` CMake target.

Selection state is owned entirely by `AdjustmentTransferDialogModel` in C++. The dialog
sends stable-identity commands (`SelectVersion`, `FocusNode`, `SetNodeChecked`,
`SetItemChecked`, `SetAllNodesChecked`, `SetAllFocusedNodeItemsChecked`, `ClearAll`,
`ClearFocusedNode`) and never edits checked roles or holds a domain selection array. Focus
and inclusion are independent: focusing a node only swaps the item pane.

Each `versions` row provides `versionId`, `displayName`, `createdAt`, `updatedAt`,
`active`, and `selected`. Each `nodes` row provides `nodeId`, `displayName`, `nodeKind`,
`defaultGrade`, `checkState` (a `Qt::CheckState` value derived from child item states),
and `focused`. Each `items` row provides `itemKey`, `displayName`, `displayValue`,
`itemSection`, `itemKind`, `checked`, and `enabled`. `itemKey`, `nodeId`, and `versionId`
are opaque stable identities handed back to the model commands.

Each `adjustmentRows` summary entry provides `key`, `section` (the owning node's display
name), `label`, `value`, `checked`, plus `node` (the node group index), `itemSection`
(the `AdjustmentTransferItemSection` value), and `itemKind`. The shell groups rows by
`node` to rebuild the paste node list and swaps the item list on node focus.

## Component Hierarchy and Role

The root is a modal `Dialog`. Its content is divided into a title bar, a central
workspace, and an action bar. The workspace is a `RowLayout` of the three pane
components separated by `dividerColor` hairlines. In copy mode all three panes show
selection controls; in paste mode the Version pane is hidden and the node and item panes
render read-only rows over the package summary.

Each pane owns a caption header, optional `Select All` / `Clear` controls, and a
`ListView` inside a `bgBaseColor` well with a `cardBorderColor` outline. Bulk checkbox
states (`allNodesCheckState`, `focusedItemsCheckState`) come from the C++ model as
`Qt::CheckState` values; `ThemeCheckBox.partiallyChecked` renders the mixed mark.
Selection visuals are monochrome: `editorListSelectedFillColor` wells with
`editorListSelectedInkColor` ink; keyboard focus is a 1 px `textColor` outline owned by
the list, independent from pointer selection.

The footer carries only the fixed `Cancel` and mode action (`Copy Adjustments` /
`Paste Adjustments`) `DialogActionButton`s. There is no numeric summary; enabled action
text stays white.

## Properties

| Property | Type | Default | Required | Description |
|---|---|---:|:---:|---|
| `mode` | `string` | `"copy"` | No | Selects `"copy"` or `"paste"` behavior. |
| `pasteStrategy` | `string` | `"paste"` | No | Stores the requested paste strategy for the owning workflow. |
| `sourceTitle` | `string` | Empty | No | Displays the source image name below the dialog title. |
| `dialogModel` | `var` | `null` | No | The `AdjustmentTransferDialogModel` owned by the controller; supplies the three list models, bulk check states, and selection commands. |
| `adjustmentRows` | `var` | Empty array | No | Read-only package summary rows for paste mode. |
| `blurSource` | `Item` | `null` | No | Supplies the application content blurred behind the modal. |
| `cornerRadius` | `real` | `0` | No | Masks the modal overlay to the host window's corner radius. |
| `copyMode` | `bool` | Derived | No | Read-only flag that is true when `mode` is `"copy"`. |
| `selectedSourceVersionId` | `string` | Derived | No | Read-only mirror of `dialogModel.selectedVersionId`. |
| `focusedPasteNodeIndex` | `int` | `0` | No | Index of the paste node whose rows fill the read-only item pane. |
| `pasteNodeModel` | `ListModel` | Empty | No | Read-only node rows rebuilt from `adjustmentRows` for paste mode. |
| `pasteItemModel` | `ListModel` | Empty | No | Read-only item rows of the focused paste node. |

## Signals

#### copyAccepted()

Emitted when the primary action is accepted in copy mode. The owner commits the dialog
model's current selection through `AdjustmentTransferController.CommitCopy()`; the
selection itself never crosses the QML boundary.

#### pasteAccepted(string strategy)

Emitted when paste confirmation is accepted. The owner applies the captured package using
the provided strategy.

#### pasteDiscarded()

Emitted when the user cancels a paste confirmation. The owner discards the pending package
and targets.

## Methods

#### rebuildPasteModels()

Clears and rebuilds `pasteNodeModel` and `pasteItemModel` from `adjustmentRows`, keyed by
each row's `node` group (falling back to `section` for legacy rows). Runs on
`adjustmentRows` changes and every `opened`.

#### rebuildPasteItems()

Repopulates `pasteItemModel` with the rows of the focused paste node.

#### focusPasteNode(string nodeKey)

Moves the focused paste node, updates its `focused` flag in place, and rebuilds the item
list. Read-only; never mutates the C++ model.

#### titleText() : string

Returns the localized title for the active mode.

#### acceptText() : string

Returns the localized primary-action label for the active mode.

## Inter-Component Interactions

`AppDialogs.qml` binds the controller-owned `dialogModel` into the dialog. Version
selection replays the chosen Version through the read-only catalog service inside the
model; it never checks out or alters the editor's active Version, and the copied package
changes only after a fully validated build. Acceptance signals are the only mutation
boundary.

`EditorAdjustmentTransferActions` supplies paste targets and enforces interaction policy
before the dialog opens or applies a package. `ThemeCheckBox` provides the shared
monochrome checkbox including the partial-state mark; `DialogActionButton` provides the
shared Alcedo action appearance with white enabled text; `appTheme` supplies all
surfaces, typography, spacing, radii, motion preferences, and list-selection colors.

## Usage Example

```qml
AdjustmentTransferDialog {
    id: transferDialog
    dialogModel: appModules.adjustmentTransfer.dialogModel
    sourceTitle: "IMG_0042.NEF"

    onCopyAccepted: {
        const result = appModules.adjustmentTransfer.CommitCopy()
        if (result && result.message) {
            host.showSnackbar(result.message)
        }
    }
}
```
