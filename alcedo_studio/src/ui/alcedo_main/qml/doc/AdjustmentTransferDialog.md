# AdjustmentTransferDialog

## Component Overview

`AdjustmentTransferDialog` is the Alcedo Studio modal for copying selected adjustment
values from a named image Version or confirming a previously captured package for one or more
target images. In copy mode it presents three panes bound to a C++ dialog model: source
Versions, transferable nodes, and the focused node's transferable items. In paste mode it
presents the captured package summary as a read-only confirmation.

The component intentionally does not render Version thumbnails. Its rows are metadata and
value summaries supplied by `AdjustmentTransferController` and
`AdjustmentTransferDialogModel`.

## Project Structure and Dependencies

`AppDialogs.qml` owns the production instance. It binds
`appModules.adjustmentTransfer.dialogModel`, forwards copy acceptance to
`AdjustmentTransferController.CommitCopy()`, and routes paste acceptance through
`EditorAdjustmentTransferActions`.

The component imports Qt Quick, Qt Quick Controls Basic, Qt Quick Effects, and Qt Quick
Layouts. It consumes the application-provided `appTheme` object and composes the shared
`DialogActionButton` control. The file is registered in `ALCEDO_MAIN_QML_FILES` in the
`alcedo_main` CMake target.

Selection state is owned entirely by `AdjustmentTransferDialogModel` in C++. The dialog
sends stable-identity commands (`SelectVersion`, `FocusNode`, `SetNodeChecked`,
`SetItemChecked`) and never edits checked roles or holds a domain selection array. Focus
and inclusion are independent: focusing a node only swaps the item pane.

Each `versions` row provides `versionId`, `displayName`, `createdAt`, `updatedAt`,
`active`, and `selected`. Each `nodes` row provides `nodeId`, `displayName`, `nodeKind`,
`defaultGrade`, `checkState` (a `Qt::CheckState` value derived from child item states),
and `focused`. Each `items` row provides `itemKey`, `displayName`, `displayValue`,
`itemSection`, `itemKind`, `checked`, and `enabled`. `itemKey`, `nodeId`, and `versionId`
are opaque stable identities handed back to the model commands.

Each `adjustmentRows` summary entry provides `key`, `section`, `label`, `value`, and
`checked`.

## Component Hierarchy and Role

The root is a modal `Dialog`. Its content is divided into a title bar, a central workspace,
and an action bar. In copy mode the workspace contains a source Version `ListView`, a
transferable-node `ListView` with derived three-state checkboxes, and a focused-node item
`ListView` with per-row checkboxes. In paste mode the workspace shows a single read-only
grouped parameter list built from `adjustmentRows`.

## Properties

| Property | Type | Default | Required | Description |
|---|---|---:|:---:|---|
| `mode` | `string` | `"copy"` | No | Selects `"copy"` or `"paste"` behavior. |
| `pasteStrategy` | `string` | `"paste"` | No | Stores the requested paste strategy for the owning workflow. |
| `sourceTitle` | `string` | Empty | No | Displays the source image name below the dialog title. |
| `targetCount` | `int` | `0` | No | Reports the number of paste targets in the action-bar summary. |
| `dialogModel` | `var` | `null` | No | The `AdjustmentTransferDialogModel` owned by the controller; supplies the three list models and selection commands. |
| `adjustmentRows` | `var` | Empty array | No | Read-only package summary rows for paste mode. |
| `blurSource` | `Item` | `null` | No | Supplies the application content blurred behind the modal. |
| `cornerRadius` | `real` | `0` | No | Masks the modal overlay to the host window's corner radius. |
| `expandedSections` | `var` | Empty object | No | Stores explicit expanded/collapsed states keyed by section label. |
| `expandedSectionsRevision` | `int` | `0` | No | Invalidates the flattened paste model after section-state changes. |
| `copyMode` | `bool` | Derived | No | Read-only flag that is true when `mode` is `"copy"`. |
| `selectedSourceVersionId` | `string` | Derived | No | Read-only mirror of `dialogModel.selectedVersionId`. |
| `displayRows` | `var` | Derived | No | Read-only flattened list of section headers and visible summary rows for paste mode. |

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

#### sectionExpanded(string section, int ordinal) : bool

Returns the stored section state. When no state exists, the first two sections default to
expanded.

#### toggleSection(string section, int ordinal)

Toggles a paste summary section and rebuilds the flattened display model.

#### buildDisplayRows() : var

Groups `adjustmentRows` by section and returns the flattened rows consumed by the paste
`ListView`.

#### versionTimeText(real seconds) : string

Formats a Version timestamp with the current locale, or returns the imported-state label
when no timestamp exists.

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
before the dialog opens or applies a package. `DialogActionButton` provides the shared
Alcedo action appearance, while `appTheme` supplies all surfaces, typography, spacing,
radii, motion preferences, and list-selection colors.

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
