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

## LUT browser (LUT library plan L6A)

### `EditorWorkspaceRail.qml`

- The `LUTs` rail entry uses the existing `qrc:/panel_icons/box.svg`, the same
  LUT icon as the adjustment navbar. It uses the shared compact rail treatment.
- The `luts` page opens at `editorLutBrowserPanelWidth` in the standard card
  shell and is capped so the viewport keeps its minimum width. A grip on the
  panel's trailing edge (`SizeHorCursor`; a 1 px `textMutedColor` line fades in
  on hover or drag) resizes it live down to `editorLutBrowserPanelWidthMin`; a
  double click restores the default. The rail keeps the width, the filter
  sidebar state, and the grid/list choice while the page unloads.

### `EditorLutBrowserPanel.qml`

- The toolbar spans the top of the page: the filter sidebar toggle (the
  existing `layout-sidebar` / `layout-sidebar-inactive` glyphs), the sunken
  `bgBaseColor` search track (search glyph, native text input, a compact clear
  action while text is present), sort, and the grid/list segments.
- Grid/list segments follow the monochrome segmented family at toolbar height:
  `bgBaseColor` track, `editorListSelectedFillColor` well under the current
  segment, `editorListSelectedInkColor` glyph. Icons are the user-provided
  Tabler `layout-grid` and `layout-list` (`panel_icons/view-grid.svg`,
  `panel_icons/view-list.svg`).
- Below the toolbar the filter sidebar sits beside the results. It folds to
  zero width with `motionFoldOpenMs` / `motionFoldCloseMs` and `motionEasing`
  (`reduceMotion` resolves them to zero); a 1 px `cardBorderColor` divider
  separates the docked sidebar. When the page is too narrow for the sidebar and
  two tile columns, the open sidebar floats over the results with the card
  border instead of squeezing them.

### `EditorLutFilterCard.qml`

- Docked, the sidebar has no chrome of its own; floating, it adds the standard
  card border and `controlRadiusSmall`.
- Title `Filters` uses `fontSizeTitle`, `fontWeightHeading`. A `Clear` caption
  action appears only when a predicate is active.
- `Favorites` is the first choice, above the sections: one row with the star
  glyph (`editorListFavoriteActiveColor` while the filter is on), its count,
  and a count bar scaled to every candidate. It behaves like every other
  choice; choosing it again shows every LUT.
- Sections follow the Album inspector layout: uppercase caption title
  (`fontSizeCaption`, `fontWeightStrong`, muted), then one row per choice with
  a quiet count bar (`editorListSelectedFillColor` at low alpha) behind the
  label and count. The selected row keeps its count bar and adds a 1 px outline
  (`graphSelectionOutlineColor`, `graphSelectionOutlineWidth`); it has no fill
  and no ink change. Hover and keyboard focus use `buttonHoveredFillColor`.
  There is no accent color, side stripe, pill, or dot. Choosing the selected
  row again returns that dimension to All.
### `EditorLutResultCard.qml`

- No card chrome of its own. The footer holds the muted count and the compact
  `IconActionButton` library actions (import, refresh, open folder); library
  and favorite errors appear above it in `dangerColor`.
- The target indicator is plain text: muted `Applies to` label and the node
  name, the current LUT on its own line, its print on a separate muted line,
  and the Missing explanation in `dangerColor`. It has no remove button: choosing
  the applied tile again removes the LUT. Without a target it shows only the
  controller's reason in muted text. The sort menu marks the current order with
  the same 1 px outline.
- Tiles sit in a sunken `bgBaseColor` well with `spaceXs` gaps. A tile shows the
  approved Tabler `cube` icon (`panel_icons/lut-cube.svg`,
  `editorLutTileIconSize`), the full title (wrapped, never elided), the print on
  its own line, and the status line for invalid or 1D entries. Every tile in a
  row takes the height of the row's tallest tile.
- The applied tile has a 1 px outline (`graphSelectionOutlineColor`,
  `graphSelectionOutlineWidth`) and no fill; its text, icon, and star keep their
  normal colors. Hover and keyboard focus use `buttonHoveredFillColor`.
  Unselectable tiles are muted.
- The favorite star is a glyph only (no hover well), shown when starred or while
  the tile is hovered, with the `editorListFavorite*` tokens.
- The list layout is the same view with one column: compact rows without the
  cube placeholder, title (`fontSizeBody`) and print left-aligned, and the star
  always shown in its trailing column. Selection, hover, and muted states match
  the tiles.
- Empty, loading, and zero-result states are centered text with
  `DialogActionButton` actions; library errors use `dangerColor`.

### `EditorLutControlPanel.qml`

- Adjustment page title `LUT` (`fontSizeTitle`, `fontWeightHeading`), the LUT
  name, its print line, the Missing line, the shared `AdjustmentSlider` for
  strength, then `Browse LUTs` and `Remove` as compact `DialogActionButton`s.

### Empty state link to Settings (L6B)

- The empty-library state adds `Download official LUTs` (`DialogActionButton`),
  which opens Settings on the LUTs page.

### Manual review

Check both themes at 1.0 and 1.5 DPR: empty library, loading, many results,
zero results, long names, applied tile, focused tile, favorite on applied and
idle tiles, no target (RAW or DRT selected), Missing LUT, grid and list
layouts, the filter fold, the floating sidebar on a narrow page, and a drag
resize down to the minimum width.

## LUT Settings (LUT library plan L6B)

### `LutSettingsPanel.qml` (Settings > LUTs)

- The page follows the other Settings pages: `SettingsSection` titles
  (`fontSizeSection`, `fontWeightHeading`) with a divider, a 160 px label
  column, and a 176 px indent for detail lines under it. The nav entry uses
  `panel_icons/lut-cube.svg`.
- `LUT library`: the root path in `dataFontFamily` (wraps anywhere), the LUT
  count and verification state in muted caption text, then four compact
  `DialogActionButton`s (Open folder, Refresh, Use another folder, Move
  library) in a wrapping row.
- A chosen folder opens an inline confirmation card (`dividerColor` 1 px
  border, `panelRadius`, no fill): the question, the folder path, and what the
  operation does, or the reason it cannot start in `dangerColor`. Only the
  confirm button (`accent`) starts the operation.
- A running library operation shows its name and an indeterminate
  `ThemedProgressBar`; a move adds Cancel. Library errors use `dangerColor`;
  files kept after a move are listed in muted caption text.
- `Official LUT packages`: the check state line (muted, or `dangerColor` on a
  failed check) with `Check again`, then one bordered row per package: name,
  status, `%n LUTs · size download · revision` in `dataFontFamily`, a
  determinate `ThemedProgressBar` while downloading (indeterminate while
  verifying and installing), the row error in `dangerColor`, and at most one
  action button (`accent`; `Retry` uses the normal kind) plus Cancel for the
  running row. No pills, badges, or status dots.

### Manual review

Check both themes: package rows in every status, a failed check, a long
Unicode root path, the move confirmation with and without an error, and a
running move with Cancel.

## Welcome surface (welcome overview plan Phase 2)

### `WelcomeDialog.qml`

- Owns the modal shell (blur backdrop + `overlayColor`), the card, the left
  column, and the right column switch: overview, no-recent-project well, or
  the new-project form. It reads project data only from
  `WelcomeProjectPreviewAdapter` and `recentProjects` and reports every action
  through a signal.
- The items are declared in Tab order: Open Project…, New Project…, the right
  column, the language selector, Quit. Do not reorder them for layout reasons;
  the footer is positioned with anchors.
- `serviceMessage` shows only after the user starts an open or create action
  on the surface. The startup preview messages never show.
- The language selector is a dark sunken `ComboBox` (`bgBaseColor`, 1 px
  `cardBorderColor`) with the monochrome selected well in its popup.

### `WelcomeProjectOverview.qml`

- Heading, cover block, information block (name, path elided in the middle,
  statistics, Continue Editing). Statistics are separate caption labels and
  Manrope values; the capture-date value spans both columns.
- Loading: skeleton bars in place of the values. Failed: the statistics hide and
  the real error text shows in `dangerColor`; Continue Editing is disabled.

### `WelcomeCoverMosaic.qml`

- Three tiles under one rounded mask. A tile with a cover row shows an
  animated skeleton until its thumbnail is ready. A tile without a photo, and
  every tile in the failed state, is a static `bgBaseColor` tile.

### `WelcomeRecentProjectList.qml`

- Sunken `bgBaseColor` well, `spaceXs` inset and row gap. A row shows the
  name, the folder path, and the relative time (Manrope, right aligned). No
  photo count. Hover is `hoverColor`; keyboard focus is a 1 px
  `textMutedColor` outline.

### `WelcomeNewProjectForm.qml`

- Back (quiet), title, name field (`bgBaseColor` field, `cardBorderColor`
  outline, `textMutedColor` outline on focus), storage folder
  (`FolderPathField`), and the primary Create Project action.

### Manual review

Review at 1280 x 800 and at the minimum window size, in both Alcedo and
Classic themes, in Simplified Chinese and English:

1. Ready state with three cover thumbnails, with fewer than three photos, and
   with no capture date.
2. Loading state after a row click: skeleton tiles and bars move; with
   `reduceMotion` on they are static.
3. Failed state with a damaged package.
4. No recent project: the well with New Project… and Open Existing Project….
5. New-project form: Back restores the previous column; Create shows the ring.
6. Tab order and Enter / Space on rows and on Continue Editing.

## Editor comparison (comparison plan Phase 3)

### `EditorComparisonView.qml`

- One opaque `cardSurfaceColor` surface covers the editor viewport, its
  photograph, and its overlays. It takes every pointer and wheel event. The
  viewport item stays alive and bound underneath.
- Areas of the reference canvas without image pixels show the same
  `cardSurfaceColor`. Do not add a frame, shadow, or checkerboard around a
  cropped image: the empty area shows the crop footprint.
- Side captions are plain text: a muted `fontWeightStrong` side name (`A` or
  `B`) and the source label in `textColor`, both `fontSizeCaption`. The first
  side is at the top left. The second side is at the top right (left/right) or
  bottom left (top/bottom). Source labels wrap. No pill, badge, or dot.
- Complete images: the two regions are separated by a `spaceXs` gap. Divider:
  a 1 px `textColor` line with a centered grip. The grip is a `bgBaseColor`
  well with a 1 px `textColor` outline and `controlRadiusSmall`; hover, drag,
  and keyboard focus change only its fill to `buttonHoveredFillColor`. The grip
  hit area is `iconButtonHitSizeCompact`.
- Loading text is muted `fontSizeBody`. Error text uses `dangerColor`. No
  partial pair is shown while one image is loading.

### `EditorComparisonCanvas.qml`

- No chrome. The image is drawn with the placement transform and clipped to
  the canvas. The canvas never stretches an image to the reference rectangle.

### `EditorComparisonPanel.qml`

- Adjustment page title `Compare` (`fontSizeTitle`, `fontWeightHeading`).
- Comparison kind, display mode, and orientation use `SegmentedCardSwitcher`
  (monochrome selected well). The A and B selectors use `AdjustmentCombo`
  without a reset action.
- `Swap A and B`, `Close`, and `Retry` are `DialogActionButton`s of the normal
  kind. Retry appears only after a failure.
- Status text is muted for loading and HDR-unavailable reasons and uses
  `dangerColor` for errors. The fixed-sensor-settings explanation is muted
  `fontSizeCaption` and wraps.

### Comparison entry and Compare page (comparison plan Phase 4)

- `EditorWorkspace.qml`: one text action `Compare` at the top left of the
  viewport (`spaceSm` margin), a `DialogActionButton` of the normal kind with
  `iconButtonHitSizeCompact` height and `fontSizeBody` text. It is hidden while a
  comparison is open. When entry is not admitted (for example HDR output), the
  action is disabled and its tooltip states the reason. The comparison view
  covers the viewport above every viewport overlay; the viewport stays visible
  underneath.
- `EditorVersionsPanel.qml`: the header has the same text action `Compare`
  before the icon actions. It opens Version comparison.
- `EditorAdjustmentStack.qml`: the Compare page is the only entry on nav page 2,
  like Mask on page 1. Its icon is `panel_icons/compare.svg` (Tabler
  `columns-2`, MIT, stroke-width 1.5, approved by the user on 2026-10-01). The
  entry is enabled and scrolled into the track only while a comparison is open.
  While comparing, the nav stays enabled so the other pages can be read; their
  controls are disabled.

### Manual review

Check both themes at 1.0 and 1.5 DPR, at the minimum window width and a wide
window: the viewport and Versions Compare actions, the HDR-disabled action
tooltip, Escape closing the comparison (and not a focused text field), each layout (complete left/right, complete top/bottom, divider
left/right, divider top/bottom), a half crop and a rotated crop on one side,
swap, divider drag to both ends, keyboard focus on the divider (arrows, Shift
arrows, Home, End), loading, error with Retry, HDR unavailable, and long source
labels.

## Library sort, group, and sections (album sort and group plan, Phase 3)

### `InspectorFieldActions.qml`

- One row per Album Inspector field header: an ascending and a descending sort
  action (`sort-ascending.svg`, `sort-descending.svg`) and a Group action
  (`category-plus.svg`). The sort icons mean photo sort; the Group icon means
  group. Do not name the Group action Order by.
- Each action is `inspectorHeaderActionSize` square with `badgeRadius` corners.
  A selected action uses `editorListSelectedFillColor` with
  `editorListSelectedInkColor` ink; idle actions use `textMutedColor` ink, hover
  uses `buttonHoveredFillColor` and `textColor` ink. Keyboard focus is a 1 px
  `textMutedColor` outline. No accent fill, outline, or side stripe.
- Icons are `iconOpticalSizeCompact` tinted through `ColorImage`. Every action
  has a full accessible name and a tooltip; date tooltips explain the full-time order inside a day.
- Bind to the accepted LibraryModule options only. At most one sort action and one
  Group action are active across the Inspector.

### `StatsCard.qml`, `DateFilterSection.qml`, `StarRatingFilter.qml`

- Optional `headerActions` slot beside the title, loaded apart from the content,
  so a collapsed section keeps its actions. The slot is empty for other callers.
- Titles wrap. When the title and the actions do not fit on one line, the whole
  action row moves below the title. Do not clip the checkbox hit area.

### `AlbumInspectorPanel.qml`

- Field order: capture date, import time, camera model, labels, rating, lens.
- The import-time section reuses `DateFilterSection` with `activityAvailable:
  false`: calendar tiles only, no activity graph and no style switch. It has its
  own day selection; it shares the renderer, not the selected value.

### `AlbumSectionView.qml`

- One vertical `ListView`: full-width group header rows of
  `librarySectionHeaderHeight` and photo rows whose height follows the zoom
  column metrics of `ThumbnailGridView.qml`.
- Headers have separate disclosure (`▸`/`▾` text action), title, and count
  roles. The title is one elided line with the full text as accessible name;
  the count is muted `dataFontFamily` caption text. No pill, badge, or dot.
- Photo cells use `cardSurfaceColor`, `hoverColor` on hover, and a neutral
  selection outline (2 px `textColor`); keyboard focus without selection is a
  1 px `textMutedColor` outline. No blue frame or tint.
- Expand all and Collapse all are muted caption text actions above the list.

### `LibraryWorkspace.qml`

- The grouped mode loads `AlbumSectionView`; the flat mode keeps
  `ThumbnailGridView`.
- A running query shows a muted Updating caption above the kept content. A
  failed query shows the error in `dangerColor` with a compact Retry
  `DialogActionButton`. A successful empty filtered result shows No Matching
  Photos.

### Manual review

Review in Alcedo and Classic themes, at a narrow Inspector width, with the
Inspector closed, with collapsed groups, with a failed query, and with an empty
filtered result.


## Folder import (large folder import refactor)

### `FolderImportConfirmDialog.qml`

- The dialog opens as soon as a folder is chosen and starts
  `ImportExportHandler.folderScan` on a worker thread. It never waits for the
  scan before it appears.
- While the scan runs: a strong `dataFontFamily` summary with the live file
  count, the folder being read on one muted caption line (`ElideMiddle`, the
  documented exception: the text changes several times a second and must not
  change the dialog height), and an indeterminate `ThemedProgressBar`.
- Until the first batch of files arrives, the list well shows `SkeletonBlock`
  rows. Rows show the file name and, in a separate muted caption, the folder
  relative to the chosen folder. Rows are uniform-height, so both labels elide.
- After the scan: `%1 file(s) found` plus the muted note that only RAW files
  are imported. An empty or unreadable folder shows its message in the summary
  line and keeps the Import action disabled.
- The accent action reads `Scanning...` and stays disabled while the scan runs.

### `ImportProgressOverlay.qml`

- Ring and large count measure photos imported out of photos expected
  (`importTotal - importFailed`); rejected files leave the expected total, so
  the total shrinks as the import runs.
- The phase caption and the `ThemedProgressBar` below it measure files checked
  out of all files. Phases: `Preparing files... a of b`, `Reading photos... a of
  b files checked`, `Saving to the library...`.
- The ring is indeterminate (render-thread `RotationAnimator`) at the start of
  preparing and while saving; the progress track is indeterminate while saving.
- Files that are not RAW are reported in a muted caption as skipped; only real
  failures use `dangerColor`. Cancel is hidden while saving.

### Manual review

Import a folder with tens of thousands of files of which most are not RAW, in
Alcedo and Classic themes: the dialog appears at once, the count and folder
line move while scanning, Import is available only after the scan, the overlay
appears immediately after Import, the expected total shrinks, the track keeps
moving, and the saving phase shows a spinning ring.
