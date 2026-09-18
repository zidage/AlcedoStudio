# Alcedo Studio QML per-file VI

This catalog records approved visual decisions for individual QML files. It
complements the global token, spacing, typography, icon, and motion rules in
[`DESIGN.md`](../../alcedo_studio/src/ui/alcedo_main/DESIGN.md). `AppTheme`
remains the runtime source of truth for values. This file records how each QML
surface must use those values.

Update the relevant file entry whenever an approved visual decision changes.
Do not add automated tests whose purpose is to freeze colors, fills, borders,
icon tint, shadows, or screenshots. Functional tests can verify interaction,
state ownership, accessibility, clipping, and persistence. Review appearance
manually in all documented states and in both Alcedo and Classic themes.

## Mask Groups panel

### `EditorWorkspaceRail.qml`

- Use `qrc:/panel_icons/pipeline.svg` for the Nodes page. This is the
  user-provided Tabler pipeline icon.
- Use `qrc:/panel_icons/nodes.svg` for the Mask Groups page. Despite the legacy
  filename, this asset is the approved Tabler `stack-2` layer icon.
- Do not use `masks.svg` for the Mask Groups navigation entry. Reserve it for
  individual Mask meaning and Mask previews.
- Both entries use the existing compact rail hit, chrome, optical, tint, hover,
  focus, and selected-outline treatment. Do not add per-icon geometry.

### Shared state language

| State | Required treatment |
| --- | --- |
| Default | Neutral card or transparent row surface with existing text and muted SVG colors. |
| Hover | `hoverColor` wash. Hover never replaces persistent selection. |
| Selected group | Keep the card surface unchanged. Draw only the outer card outline with `graphSelectionOutlineColor` and `graphSelectionOutlineWidth`. Do not invert text or SVG colors. |
| Group owns selected Mask | Use a quiet header wash and strong group name. Do not add a second selection outline around the group. |
| Selected Mask | Keep the row surface unchanged. Draw only the row outline with `graphSelectionOutlineColor` and `graphSelectionOutlineWidth`. Do not outline the preview well. Do not invert text or SVG colors. |
| Keyboard focus | Use the existing neutral focus outline or hover wash. Focus remains visible independently from selection. |
| Disabled content | Keep the normal disabled/muted colors. Selection does not promote disabled text or SVGs to selected ink. |

Theme blue is not a selection color for this panel. Selection must not add a
blue fill, blue outline, side bar, glow, or thumbnail frame.

### `EditorMaskGroupsPanel.qml`

- Own the page shell, toolbar, list track, empty/loading/error states, and the
  ordered list of group delegates.
- Use the existing panel, card, spacing, typography, and compact hit-area
  tokens. Do not introduce local color or size literals.
- Present groups from downstream to upstream: the downstream Color Grade is at
  the top and the upstream Color Grade is at the bottom.
- Pass `graphSelectionOutlineColor` and `graphSelectionOutlineWidth` to group
  delegates. The panel does not paint a second selection layer.
- Preserve list scroll while model data refreshes. Opening this page must not
  reposition a visible selected row without need.

### `EditorMaskGroupDelegate.qml`

- Paint one `cardSurfaceColor` group card with the standard
  `cardBorderColor` outline.
- For a selected group, replace only that outer outline with the neutral
  selection outline. Keep the header, preview well, label, lock icon, delete
  icon, and Mask SVG at their default colors.
- A selected child Mask makes `ownerActive` a secondary state: show the quiet
  header wash and strong group label, while leaving the group card on its
  standard outline.
- Keep one disclosure chevron at the leading edge. Activating the main header
  selects the group and toggles its drawer. Clicking the chevron toggles the
  drawer without invoking an action button. Lock and delete hit areas remain
  exclusive and never toggle the drawer.
- The header stays visible in both drawer states. The body clips and changes
  height through the existing fold tokens; it does not animate its base color.
- Do not add selected-state SVG recoloring. `masks.svg`, lock, and trash icons
  keep their normal semantic tint when the group becomes selected.

### `EditorMaskGroupMaskRow.qml`

- Keep each Mask row flat and transparent over the group card body.
- For a selected Mask, draw only the neutral outline around the complete row.
  Keep the interior transparent except for transient hover or focus wash.
- Keep the preview well independent from selection. It must not gain a nested
  selected border.
- Keep the Mask type SVG, lock icon, and trash icon at their normal tints when
  the row becomes selected. The type SVG stays muted; lock state alone can use
  the established locked/unlocked semantic treatment.
- Use strong label weight to support selection recognition without changing
  label color. Disabled Mask text remains muted.
- Keep lock and delete hit areas exclusive. Clicking those buttons must not
  also select the Mask row.

### Manual review

Review the panel at 260 px, 320 px, and 460 px widths in both themes:

1. Select a group with an open drawer and with a closed drawer. Confirm that
   only the outer group outline changes and every SVG keeps the same color.
2. Select a Mask. Confirm that only its row outline becomes persistent, its
   preview has no nested selection frame, and its owner group uses only quiet
   header emphasis.
3. Hover and keyboard-focus selected and unselected rows. Confirm that hover,
   focus, and persistent selection remain distinguishable.
4. Click the main group header once in each drawer state. Confirm that it
   closes when open and opens when closed. Confirm that lock and delete clicks
   do not toggle it.
