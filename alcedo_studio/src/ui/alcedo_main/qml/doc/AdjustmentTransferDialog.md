# AdjustmentTransferDialog

## Component Overview

`AdjustmentTransferDialog` is the Alcedo Studio modal for copying selected adjustment
parameters from a named image Version or confirming a previously captured package for one or more
target images. In copy mode it presents the source image's mini-Git Versions beside a grouped,
selectable parameter list. In paste mode it presents the captured parameters as a read-only
confirmation.

The component intentionally does not render Version thumbnails. Its source rows are metadata and
parameter summaries supplied by `AdjustmentTransferController`.

## Project Structure and Dependencies

`Main.qml` owns the production instance. It fills the dialog from
`AdjustmentTransferController.PrepareCopy()`, forwards copy acceptance to
`AdjustmentTransferController.CopyVersion()`, and routes paste acceptance through
`EditorAdjustmentTransferActions`.

The component imports Qt Quick, Qt Quick Controls Basic, Qt Quick Effects, and Qt Quick Layouts.
It consumes the application-provided `appTheme` object and composes the shared
`DialogActionButton` control. The file is registered in `ALCEDO_MAIN_QML_FILES` in the
`alcedo_main` CMake target.

Each `sourceVersions` entry is expected to provide `versionId`, `displayName`, `updatedAt`,
`active`, and `items`. Each adjustment entry is expected to provide `key`, `section`, `label`,
`value`, and `checked`.

## Component Hierarchy and Role

The root is a modal `Dialog`. Its content is divided into a title bar, a central workspace, and an
action bar. The central workspace contains a source Version list in copy mode and a grouped
parameter list in both modes. Inline section and parameter delegates provide keyboard and
accessibility behavior.

## Properties

| Property | Type | Default | Required | Description |
|---|---|---:|:---:|---|
| `mode` | `string` | `"copy"` | No | Selects `"copy"` or `"paste"` behavior. |
| `pasteStrategy` | `string` | `"paste"` | No | Stores the requested paste strategy for the owning workflow. |
| `sourceTitle` | `string` | Empty | No | Displays the source image name below the dialog title. |
| `selectedSourceVersionId` | `string` | Empty | No | Identifies the Version whose parameter rows are currently shown and copied. |
| `targetCount` | `int` | `0` | No | Reports the number of paste targets in the action-bar summary. |
| `sourceVersions` | `var` | Empty array | No | Holds Version metadata and per-Version adjustment rows for copy mode. |
| `adjustmentRows` | `var` | Empty array | No | Holds the currently displayed parameter rows. |
| `blurSource` | `Item` | `null` | No | Supplies the application content blurred behind the modal. |
| `cornerRadius` | `real` | `0` | No | Masks the modal overlay to the host window's corner radius. |
| `expandedSections` | `var` | Empty object | No | Stores explicit expanded/collapsed states keyed by section label. |
| `expandedSectionsRevision` | `int` | `0` | No | Invalidates the flattened parameter model after section-state changes. |
| `copyMode` | `bool` | Derived | No | Read-only flag that is true when `mode` is `"copy"`. |
| `selectedCount` | `int` | Derived | No | Read-only count of checked adjustment rows. |
| `displayRows` | `var` | Derived | No | Read-only flattened list of section headers and visible parameter rows. |

## Signals

#### copyAccepted(var selectedKeys, string versionId)

Emitted when the primary action is accepted in copy mode. The owner captures the listed parameter
keys from the specified source Version.

#### pasteAccepted(string strategy)

Emitted when paste confirmation is accepted. The owner applies the captured package using the
provided strategy.

#### pasteDiscarded()

Emitted when the user cancels a paste confirmation. The owner discards the pending package and
targets.

## Methods

#### selectedKeys() : var

Returns the stable keys for every checked adjustment row.

#### restoreListScroll(real contentY)

Restores the parameter list position on the next event-loop turn, clamped to its valid range.

#### setRowsPreservingScroll(var rows)

Replaces the adjustment rows while preserving the parameter list's current position.

#### setRowChecked(int index, bool checked)

Copies and updates one adjustment row so QML model change notification remains reliable.

#### setAllRowsChecked(bool checked)

Updates every adjustment row to the requested selection state.

#### sectionExpanded(string section, int ordinal) : bool

Returns the stored section state. When no state exists, the first two sections default to expanded.

#### toggleSection(string section, int ordinal)

Toggles a parameter section and rebuilds the flattened display model.

#### buildDisplayRows() : var

Groups adjustments by section and returns the flattened rows consumed by the parameter `ListView`.

#### selectSourceVersion(var versionRow)

Selects a Version, resets section expansion, replaces the parameter rows with that Version's
summary, and scrolls to the beginning.

#### versionTimeText(real seconds) : string

Formats a Version timestamp with the current locale, or returns the imported-state label when no
timestamp exists.

#### titleText() : string

Returns the localized title for the active mode.

#### acceptText() : string

Returns the localized primary-action label, including the selected setting count in copy mode.

## Inter-Component Interactions

`Main.qml` binds no live backend object into the dialog. Instead, it assigns immutable preparation
results before opening the modal. Version selection therefore remains local and does not check out
or alter the editor's active Version. Acceptance signals are the only mutation boundary.

`EditorAdjustmentTransferActions` supplies paste targets and enforces interaction policy before
the dialog opens or applies a package. `DialogActionButton` provides the shared Alcedo action
appearance, while `appTheme` supplies all surfaces, typography, spacing, radii, motion preferences,
and list-selection colors.

## Usage Example

```qml
AdjustmentTransferDialog {
    id: transferDialog
    sourceTitle: "IMG_0042.NEF"
    selectedSourceVersionId: "0123456789abcdef0123456789abcdef"
    sourceVersions: preparedVersions
    adjustmentRows: preparedRows

    onCopyAccepted: function(selectedKeys, versionId) {
        adjustmentTransfer.CopyVersion(sourceElementId, versionId, selectedKeys)
    }
}
```
