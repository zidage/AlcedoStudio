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

## Adjustment Transfer dialog

### `AdjustmentTransferDialog.qml`

- Own only the modal shell: header, backdrop, footer actions, and the pane
  `RowLayout`. Pane visuals live in the three pane files below.
- The footer holds only the fixed `Cancel` and mode action
  (`Copy Adjustments` / `Paste Adjustments`) `DialogActionButton`s. No numeric
  summaries, counts, or dynamic action labels.
- All enabled action-button text stays white; the primary action keeps the
  documented accent CTA fill. Never pair a blue or accent fill with dark text.

### `AdjustmentTransferVersionPane.qml`

- Read-only Version catalog rows: initial-letter tile, display name, timestamp,
  and a separate trailing `Active` caption. Do not use compound-dot labels such
  as `Active · time`.
- Selected Version row uses `editorListSelectedFillColor` with
  `editorListSelectedInkColor` for every glyph and label. Hover stays the quiet
  `buttonHoveredFillColor` wash; keyboard focus is a 1 px `textColor` outline
  owned by the list, independent from selection.

### `AdjustmentTransferNodePane.qml`

- Copy mode shows a `Select All` three-state `ThemeCheckBox` and a `Clear`
  `DialogActionButton` in the pane header; paste mode hides that row and every
  row checkbox (`readOnly`).
- The focused node uses the monochrome selected well + ink. Row checkboxes are
  box-only `ThemeCheckBox` instances; the row body still owns node focus, so
  the checkbox must not fill the row.
- Focus and inclusion stay independent: clicking a row body focuses the node;
  clicking its checkbox toggles the derived node check state.

### `AdjustmentTransferItemPane.qml`

- Same `Select All` / `Clear` header language as the node pane, scoped to the
  focused node only. Paste mode hides the header and renders read-only rows
  with no checkbox.
- Each Color Grade exposes exactly one all-or-none `Masks` row; it is disabled
  when the grade owns no Masks. Never render one checkbox per Mask.
- Item rows group under caption section headers (`Tone`, `Look`, `LUT`,
  `Display Transform`, `Masks`) separated by `dividerColor` hairlines.
- Replay errors surface as a `dangerColor` caption above the item well; an
  empty item list shows `No transferable adjustments.` in muted text.

### Shared state language

| State | Required treatment |
| --- | --- |
| Selected / focused row | `editorListSelectedFillColor` well + `editorListSelectedInkColor` ink; no accent fill, border, or stripe. |
| Checked / partial checkbox | Bone well + ink mark via `ThemeCheckBox`; partial uses the ink dash mark. |
| Hover | `buttonHoveredFillColor` wash; never replaces selection. |
| Keyboard focus | 1 px `textColor` outline on the focused list row; visible independently from selection. |
| Disabled row | Muted text and disabled checkbox state; the Masks row dims rather than disappearing. |

### Manual review

Review the dialog at 260 px / 320 px / 460 px pane widths, in both Alcedo and
Classic themes, and at DPR 1.0, 1.5, and 2.0:

1. Copy mode: confirm the Version, node, and item panes render with stable
   widths and that `Select All` / `Clear` act on their own scope only.
2. Confirm focus and inclusion are independent: a focused node keeps its
   checkbox state; a checked node stays checked when focus moves.
3. Confirm the Masks row is a single all-or-none checkbox.
4. Confirm scroll positions in all three lists survive checkbox and focus
   changes.
5. Confirm Tab reaches the Version, node, and item lists and then the footer
   actions, and that Space / Enter behave as documented.
6. Paste mode: confirm read-only node and item panes with no selection
   controls and no numeric footer.
