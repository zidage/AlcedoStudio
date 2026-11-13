# Alcedo Studio QML Visual Identity (VI)

**Read this file before changing or adding any QML visual.** Feature QML must
consume named `appTheme` properties or shared components listed below. Do not
introduce unexplained pixel, color, radius, or duration literals.

Owner: `alcedo_studio/src/ui/alcedo_main`  
Implementation source of truth: `AppTheme` (`app_theme.hpp` / `app_theme.cpp`)  
Shared components: `IconActionButton.qml`, `CollapsibleSection.qml`,
`DialogActionButton.qml`, `IconButton.qml`, `SegmentedCardSwitcher.qml`,
`SlidingIconNav.qml`, `DateFilterSection.qml`, `DateCommitGraph.qml`,
`AdjustmentSlider.qml`, `ThemedProgressBar.qml`, `ThemeCheckBox.qml`,
`ThemeSwitch.qml`, `SkeletonBlock.qml`, `StaggeredGridLoader.qml`,
`StaggeredGridMark.qml`, `AlertBadge.qml`

Per-file decisions: `docs/VI/README.md`. That catalog specializes this global
system for individual QML files and is updated with each approved visual change.

This document freezes the visual system for the unified QML workspace (Phase 4C).
Phase 5–6 must consume these tokens; they must not invent a parallel palette.

---

## Agent rule

1. Open this file.
2. Map the change to a token or shared component in the tables below.
3. If a new value is required, add it to `AppTheme` **and** this document in the
   same change. Never publish a value here that differs from code.
4. Prefer shared components over copy-pasted Button/Rectangle chrome.
5. Verify visual changes manually in every state listed by the applicable
   `docs/VI/README.md` entry. Automated tests cover behavior and accessibility,
   not exact colors, fills, borders, icon tint, or screenshots.
6. Apply the monochrome selection and restrained accent rules below to every
   new or changed surface. A shared component or existing color token does not
   exempt a surface from these rules.

Drift checklist: `docs/roadmap/alcedo_studio/ui/qml_visual_literal_review_checklist.md`

---

## Typography

| Role | Family token | Size token | Weight token | Line height | Notes |
| --- | --- | --- | --- | --- | --- |
| UI body | `uiFontFamily` | `fontSizeBody` (12) | `fontWeightRegular` (500) | `lineHeightBody` (16) | Default chrome copy |
| UI strong | `uiFontFamily` | `fontSizeBody` | `fontWeightStrong` (600) | `lineHeightBody` | Filmstrip counts, emphasis |
| UI title | `uiFontFamily` | `fontSizeTitle` (13) | `fontWeightStrong` | `lineHeightTitle` (18) | Panel subtitles |
| UI section | `uiFontFamily` | `fontSizeSection` (14) | `fontWeightHeading` (700) | `lineHeightSection` (20) | Panel titles |
| UI caption | `uiFontFamily` | `fontSizeCaption` (11) | `fontWeightRegular` | `lineHeightCaption` (14) | Secondary chrome |
| Headline | `headlineFontFamily` | `fontSizeHeadline` (22) | `fontWeightHeading` | `lineHeightHeadline` (28) | Empty-state titles |
| Settings card title | `headlineFontFamily` | `fontSizeSection` (14) | DemiBold | 1.1 | Title of a `PackageCard` (Settings > LUTs, Settings > AI Models) and of the folder tiles of those pages. Below it, after a `spaceSm` gap, one sentence in `uiFontFamily` + `fontSizeBody` muted text says what the package or model does; no list and no version text there (a Pill shows the version). A GitHub button beside the title opens the project of the package when it has one. Do not use a display size: a model name must stay on one line in the two-column page. |
| Display count | `headlineFontFamily` | `fontSizeDisplay` (72) | ExtraLight | 1 | The one large number of a Settings bento count tile (`CountTile`): LUT total, photo count, the version of About. |
| Data / numeric | `headlineFontFamily` (Manrope) with `font.features: { "tnum": 1 }` | body to display | DemiBold for inline counts, ExtraLight for display counts | matching | Counts, sizes, revisions, progress values, and remaining times. `dataFontFamily` (IBM Plex Sans) is retired page by page: a new or changed surface uses Manrope. Counts use the digit grouping of the locale (`DataFormat.grouped`); see "Settings bento pages" for large counts. |
| Mono | `monoFontFamily` | body/caption | regular | matching | Minigit history/Versions data (commit hashes, before/after delta lines) and the adjustment-header EXIF tokens. Family is **DM Mono** (`data_DMMono.ttf`). Do **not** use for general metrics, filmstrip counts, zoom, crop degrees, or the selected node display name. |

Families resolve at runtime from registered Alcedo fonts (`AppTheme::RegisterFonts`).
`uiFontFamily` is Inter in every language; Chinese glyphs come from the Inter ->
Noto Sans SC substitution. Do not make Noto Sans SC the primary UI family: its taller line
box (ascent 1.160 / descent 0.288 em) puts Latin text below the vertical center of a row.
Do not hardcode Inter, Roboto, Arial, or system UI fonts in feature QML.

### Monospace typography

Monospace (`monoFontFamily`, DM Mono) is limited to these surfaces:

| Surface | Token | What |
| --- | --- | --- |
| Version card commit line | `monoFontFamily` + `fontSizeCaption` | `Commit <8-hex>` or `Commit image root` |
| Transaction card hash | `monoFontFamily` + `fontSizeCaption` | `Commit <8-hex>` per row |
| Transaction before/after | `monoFontFamily` + `fontSizeBody` | Delta value line (`0 → +0.35`) |
| Adjustment header EXIF tokens | `monoFontFamily` + `fontSizeCaption` | Four equal-width cells: `100mm`, `f2.8`, `1/500s`, `ISO 100` |

Version and transaction **titles**, times, and section chrome stay on
`uiFontFamily`. The selected node display name in the adjustment header also stays
on `uiFontFamily`. Active Version is outline-only (1 px text-color border); no
`CURRENT HEAD` pill and no separate "Checked out" / dual Head+Commit labels.

---

## UI copy composition

### Hard ban: `xx · xx` labels

Do not join independent names, values, counts, modes, or states with a centered
dot in Alcedo-owned UI copy. This ban applies to panel titles, node chrome,
metadata, badges, toolbars, list rows, and status text.

Forbidden examples include `On · 2 masks`, `RAW · Active`, and
`Tone Curve · Enabled`. Give each item a separate visual role: a dedicated
label, value, icon with an accessible name, column, or row. Use spacing,
alignment, or a structural divider when the relationship needs visual grouping.
Do not reproduce the same compound-label pattern with a bullet, slash, vertical
bar, or dash.

### Hard ban: unrequested pills and badges

Do not add a new pill, badge, chip, tag, lozenge, or similar rounded text
container unless the user explicitly requests that element for the current UI.
A reference image, an existing component, or a data type does not give this
permission. Use plain text, an icon, a standard action, or layout structure by
default. Do not copy an existing approved pill or badge into a new surface
without an explicit user request.

The one standing exception is the alert badge below: the user requested it for
every warning and error message.

The History panel Agent chip is a second requested exception: the automation
plan (commit attribution) asks for an agent badge on each agent commit. It uses
the hash chip language of the same row (`badgeRadius`, 1 px `cardBorderColor`,
caption text) and sits after the hash chip. The note of the commit is muted
caption text on its own line, not a chip.

### Alert badges: warning and error messages

Every warning or error message (a failed operation, a missing file, a rejected
request, an invalid entry) is shown with the shared `AlertBadge.qml`: a
`alertBadgeFillColor` badge (`badgeRadius`, `spaceSm` / `spaceXs` padding) with
`alertBadgeTextColor` (white) caption text in `fontWeightStrong`. The text wraps
and the badge hides itself while the text is empty. Do not paint a warning or
error sentence in `dangerColor` text: red text on the dark surfaces is hard to
read. Neutral status (loading, empty states, hints, the reason an action is
unavailable) stays muted body text, not a badge.

This rule applies to every new or changed message. Existing `dangerColor` text
in files a change does not touch has not been converted yet; convert it when
the file is next changed. `dangerColor` itself remains the token for
destructive buttons, failure status lamps, and error borders.

### Hard ban: unrequested status dots

Do not add a new status dot, presence dot, activity dot, notification dot, or
other colored circular indicator unless the user explicitly requests it for the
current UI. A reference image, an existing component, or a semantic color token
does not give this permission. Do not use a dot for decorative balance.
Existing approved indicators can remain in an unchanged surface. Do not copy
them into a new or changed surface without an explicit user request.

### Hard ban: self-drawn SVG assets

Do not invent, trace, or generate SVG path data for Alcedo UI icons. When work
requires adding, replacing, or newly assigning an SVG, report the exact need
and intended source to the user before editing. Continue only after the user
provides the SVG or explicitly approves an existing repository or external
asset. Reuse an approved repository asset when it already expresses the same
meaning.

Record the source icon name, source URL, and license in every newly added SVG.
Normalization may change the source stroke color, viewBox formatting, and an
explicitly approved stroke width, but it must not redesign the approved path
geometry. The pipeline icon added on 2026-09-17 is user-provided and approved.
The Compare page icon `panel_icons/compare.svg` (Tabler `columns-2`, stroke
width normalized to 1.5) was approved by the user on 2026-10-01.

### Text truncation policy

Labels in content-driven layouts wrap (`wrapMode: Text.Wrap`); the parent
height follows `implicitHeight` (`Math.max(original floor, content height)`), so
narrow windows reflow instead of hiding copy. `elide` stays only where geometry
is genuinely fixed: thumbnail cards, uniform list rows positioned by
`index * rowHeight`, graph axes and slider tick labels, single-line field
placeholders, EXIF spec cells, and bounded banners that already wrap to a line
cap before eliding. Do not wrap text inside a fixed-height item without
updating its height binding.

---

## Color and surface hierarchy

Semantic colors follow the active theme (`currentThemeIndex` 0 Alcedo / 1 Classic)
and notify via `ThemeChanged`.

### Monochrome selection and restrained theme blue

**Product direction, 2026-09-16:** keep application chrome predominantly
monochrome. Use neutral surface contrast, text/icon inversion, and clear
structure to express selection. Theme blue must remain a restrained accent.
These rules apply to Library thumbnails, filmstrip items, Nodes, Mask Groups,
Mask rows, History/Versions, navigation, and other selectable controls.

**Hard ban: blue picture-frame selection.** Never surround a selected photo,
thumbnail, node, group, card, or panel with a blue border, outline, ring, glow,
or animated frame. This includes `accentColor`, `accentSecondaryColor`,
hardcoded blue, and the default selection decoration of a framework component.
Making the blue frame thinner or more transparent does not make it acceptable.
Do not replace it with a blue selection slab, tinted well, or blue side stripe.

- Dense rows and segments use `editorListSelectedFillColor` with
  `editorListSelectedInkColor` by default. A per-file VI entry can instead
  require a neutral outline with unchanged text and icon colors. Mask Groups
  use that outline treatment. The Adjustment Transfer Version and Node panes
  also use it: the selected row gets a `cardSurfaceColor` fill and a 1 px
  `textColor` outline, and its checkbox keeps the normal selected well. A
  keyboard focus ring without selection uses a 1 px `textMutedColor` outline.
- Nodes and established outline-based surfaces keep their neutral fill and
  use the existing text-color outline. Nodes use `graphSelectionOutlineColor`
  (`textColor`), never a theme-accent alias. A neutral outline is permitted;
  do not add nested frames around the group and each child thumbnail.
- A group containing the selected Mask remains identifiable through its
  header text/weight or a quiet neutral surface. Give the selected Mask row
  the strongest indication; do not compete with a second bright group frame.
- Keep hover, persistent selection, and keyboard focus distinct. Selection
  cannot depend only on hover or on a color hue. Keyboard focus must stay
  visible using the component's documented treatment; it must not become a
  persistent blue frame after a pointer selection.
- New or changed chrome starts with neutral AppTheme roles. Use theme blue
  only for a specific, documented action or state that needs the accent, keep
  it to that small element, and avoid repeating it across labels, icons,
  borders, and fills in the same control. Generic selection, group expansion,
  deletion locks, and panel activation do not justify blue.
- Photo pixels, color-editing data, RGB scope channels, and documented
  error/warning or connection semantics retain their meaningful colors.
  Their colors must not be reused as selection decoration.

The token table below also inventories existing implementation. The Library
workspace no longer uses `selectedTintColor` or any theme accent, except the
wordmark and the import progress ring (see "Library workspace chrome"). The
remaining `selectedTintColor` use is a legacy accent use, not the pattern for
new or changed selection. This policy update does not claim that all existing
QML usages have already been replaced. Keep AppTheme values and
this document synchronized when implementing a visual change; do not create a
parallel palette to satisfy the policy.

### Semantic tokens

| Role | Token | Use |
| --- | --- | --- |
| Canvas | `bgCanvasColor` | Outer gap behind blocks |
| Deep | `bgDeepColor` | Floating modals / popovers |
| Base | `bgBaseColor` | Sunken inputs, selected rail fill |
| Panel | `bgPanelColor` | Side panels, header/footer chrome |
| **Card surface** | **`cardSurfaceColor`** | **Library cards + editor cards** (alias of `bgPanelColor`) |
| **Card border** | **`cardBorderColor`** | **Shared card outline** (alias of `dividerColor`) |
| Text | `textColor` | Primary copy |
| Text muted | `textMutedColor` | Secondary / empty hints |
| Icon | `iconColor` | Default SVG tint |
| Accent | `accentColor` | Restricted theme accent for a documented action/state; never generic selection or workspace thumbs |
| Accent secondary | `accentSecondaryColor` | Secondary accent only where explicitly specified; never selected borders or a reason to use Material |
| Danger | `dangerColor` (`toneWine`) | Destructive emphasis |
| Danger tint | `dangerTintColor` | Soft danger wells |
| Alert badge fill | `alertBadgeFillColor` (`dangerColor`) | `AlertBadge.qml` fill for warning and error messages |
| Alert badge text | `alertBadgeTextColor` (white) | `AlertBadge.qml` message text |
| Background task finished | `backgroundTaskFinishedColor` | Green status lamp for completed/canceled work |
| Background task working | `backgroundTaskWorkingColor` | Yellow status lamp for queued/running/canceling work |
| Background task failed | `backgroundTaskFailedColor` | Red status lamp for failed work |
| Active model border | `activeModelBorderColor` (`backgroundTaskFinishedColor`) | 2 px border of the AI model card that is active in the open project (Settings > AI Models) |
| Selected tint | `selectedTintColor` | Legacy accent wash. The Library does not use it; the editor Mask type row (`EditorNodeMaskTypeRow.qml`) still does. New or changed selection follows the monochrome rules above |
| Hover | `hoverColor` | Quiet hover wash |
| Divider | `dividerColor` | Hairlines, card borders |
| Glass panel | `glassPanelColor` | Translucent shells when needed |
| Glass stroke | `glassStrokeColor` | Glass edges |
| Overlay | `overlayColor` | Modal scrims |
| **Button idle fill** | **`buttonIdleFillColor`** | **Icon button idle background** (= card surface, opaque) |
| **Button hovered fill** | **`buttonHoveredFillColor`** | **Opaque hover-state fill** (between panel and engaged) |
| **Button pressed fill** | **`buttonPressedFillColor`** | **Opaque pressed-state fill** (= hover well) |
| **Button selected fill** | **`buttonSelectedFillColor`** | **Opaque selected-state fill** (= pressed / hover well) |
| **Control fill** | **`controlFillColor`** | **Raised control surface**: `DialogActionButton` "normal" (Browse LUTs), `AdjustmentCombo` / `SearchableAdjustmentCombo` fields, AI Mask tool actions. About 0x13 above the panel so the control reads apart from card and `bgBaseColor` surfaces |
| **Control hovered fill** | **`controlHoveredFillColor`** | **Hover state of a raised control** |
| **Control pressed fill** | **`controlPressedFillColor`** | **Pressed state of a raised control** |
| **Control border** | **`controlBorderColor`** | **1 px edge of a raised control** (white at alpha 28) |
| **Disabled surface** | **`disabledSurfaceColor`** | **Reserved muted shell token** (not used for editor side-panel shells) |
| **List selected fill** | **`editorListSelectedFillColor`** | **Monochrome light well for dense catalog rows** (LUT browser, inverted selection) |
| **List selected ink** | **`editorListSelectedInkColor`** | **Text / icon ink on the light selected well** (= `bgBaseColor`) |
| **List favorite idle** | **`editorListFavoriteIdleColor`** | **Unstarred glyph on sunken (dark) rows** |
| **List favorite active** | **`editorListFavoriteActiveColor`** | **Starred glyph on sunken rows** (existing, localized `accentColor` use) |
| **List favorite idle on selected** | **`editorListFavoriteIdleOnSelectedColor`** | **Unstarred glyph inverted on the light well** |
| **List favorite active on selected** | **`editorListFavoriteActiveOnSelectedColor`** | **Starred glyph inverted on the light well** (full ink) |
| Rating star empty | `ratingStarEmptyColor` | Empty star of Library card ratings, the Inspector rating filter, and the photo Inspector rating; filled stars use `textColor`, the hover preview `textMutedColor` |
| Thumbnail edit mark fill | `thumbnailEditMarkFillColor` | `bgDeepColor` (alpha 200) fill of the edit mark chip on the top-right corner of a Library card or filmstrip photo |
| Thumbnail details scrim | `thumbnailDetailsScrimColor` | `bgDeepColor` (alpha 180) fill of the hover details panel on the bottom of a justified-rows Library tile |

**Monochrome inverted list selection:** dense catalogs (the LUT browser first) keep a
black-and-white row language — sunken `bgBaseColor` track, light
`editorListSelectedFillColor` bar, `editorListSelectedInkColor` for title and
secondary copy. Favorite stars **invert with the row**: muted light idle + the
documented favorite accent on dark rows; muted ink idle + full ink active on the selected light
well. Type badges use a white chip (`editorSliderHandleColor`) on dark rows and
invert to ink-on-bone when the row is selected. Do not reintroduce ad-hoc
`#D8D4CD` / `Qt.rgba` star or badge colors in feature QML.

**History/Versions outline selection:** the transaction timeline is a Git
graph: flat rows sit directly on the sunken `bgBaseColor` well while a
continuous 1 px `cardBorderColor` rail links state-driven node glyphs down the
left gutter — small solid disc for applied edits, small hollow ring for undone
(redo) rows, large hollow ring for the graph root, and a
large double ring (text-ink outline + inner dot) for the checked-out commit.
Only the checked-out row carries a `cardSurfaceColor` fill with the quiet 1 px
text-token outline; all other rows stay flat, showing at most a quiet
`hoverColor` wash on hover for movable rows. Named Version cards and the
persistent rail buttons keep the prior `cardSurfaceColor` + 1 px outline
language. None of these surfaces use the filled `editorListSelectedFillColor`
well; that filled selection remains reserved for dense catalog rows such as
LUT and is unchanged by the History/Versions refactor.

**List well inset (required):** the light selected bar must **not** flush the
sunken track border. Use `spaceXs` as list track padding (`ListView` margins)
and as inter-row gap (`ListView.spacing`) so track color frames every well.

**LUT browser tiles (LUT library plan L6A):** the LUT browser is an Editor
rail page with a filter card and a tile grid (see `docs/VI/README.md`). It is
an outline-selection surface: the applied tile, the chosen filter row, and the
current sort order draw a 1 px `graphSelectionOutlineColor` outline
(`graphSelectionOutlineWidth`) with no fill and no ink inversion. Hover and
keyboard focus use the quiet `buttonHoveredFillColor` well. Tiles sit inside a
sunken `bgBaseColor` well with `spaceXs` gaps. Selecting or starring a tile
never resets the tile rows or moves the scroll position.

**LUT tile preview (issue #598):** a grid tile shows the open photo with its LUT
above the title. The preview is the import root of the photo with the LUT at full
strength (not the current edit), 256 px on the long edge. The preview slot has
the 3:2 shape of the filmstrip slot and the full tile content width; the photo
fits the slot, is centered, and has `controlRadiusSmall` corners (`MultiEffect`
mask). Until the preview is ready, and for a LUT that cannot be applied, the slot
shows the `editorLutTileIconSize` cube centered on the well. The list layout has
no preview slot.

**Editor filmstrip tiles:** the filmstrip shares the Library selection, and a
tile has two independent states. (User request 2026-10-28: the former bone well
with an inset ink frame and a square caption bar read as a rubber band around
the photo, because the three shapes had different corner radii and left gaps.)
The photo keeps its own aspect ratio: it fits a 3:2 slot, is centered in it,
and has `controlRadiusSmall` corners (`MultiEffect` mask). There is no well,
frame, or fill around it; a `bgBaseColor` placeholder with the same radius
fills the slot while the thumbnail loads or when it is missing. The tile state
is **one ring** that follows the photo outline: its outer edge is `spaceXs`
outside the photo and its radius is `controlRadiusSmall + spaceXs`, so the ring
and the photo corners are concentric and the gap stays even all around.

| State | Ring | File name |
| --- | --- | --- |
| Editing (the image that the editor shows) | 2 px `textColor` | `textColor`, `fontWeightStrong` |
| Selected (in the Library selection, not open) | 2 px `textMutedColor` | `textColor` |
| Keyboard focus without a state | 1 px `textMutedColor` | `textMutedColor` |
| Idle | none | `textMutedColor` |

Editing wins when both apply. Hover is a `hoverColor` wash in the ring area,
behind the photo, and the file name changes to `textColor`; ring and text
colors change in `motionFadeMs`. A click in the Library changes only the
selected state; opening an image from the strip (click, Left/Right) also makes
it the only selected image, so keyboard navigation leaves one highlighted
tile. The file name is plain text below the ring (caption size, one line,
centered, middle elision), so it never covers the photo; it has no bar or
fill. The edit mark is the chip on the top-right corner of the photo (see
"Library thumbnail cards"). The caption does not show the rating, the date, or
the camera. The rating is an overlay on the photo: only the filled stars (`★`,
white, `dataFontFamily`) at the bottom-right corner, on a short shade over the
bottom part of the photo only (`bgDeepColor` gradient from transparent to alpha
0.78). The photo, the shade, the stars, and the edit mark are one layer with
one rounded mask, so none of them shows outside the photo corners. An
unrated image has no overlay and no shade. There is no container around the
stars, and they are display only: the user rates with the context menu and the
keyboard shortcuts. The Saving and Rendering badges of the editing tile sit on
the top-left corner of the photo. Every slot in the strip has the same size.
The tile width fits the 3:2 slot to the slot height. The dock minimum and
default height is 168 px (`EditorFilmstrip.minExpandedHeight`,
`kFilmstripExpandedHeightMin`), which gives a slot of about 100 px. There
is no separate rating badge.

**Monochrome segmented controls (family):** four chrome sites share one language:

| Site | Track | Sliding / selected well | Active glyph / label |
| --- | --- | --- | --- |
| Library/Editor capsule | `bgBaseColor` | `editorListSelectedFillColor` | `editorListSelectedInkColor` |
| Adjustment panel navbar | `bgBaseColor` | same fill (no accent blue) | same ink on selected icon |
| Histogram/Waveform navbar | `bgBaseColor` | same fill | same ink on selected icon |
| Display method segments | `bgBaseColor` | same fill | same ink on selected label |

Rules for all four: no `accentColor` slab; transparent segment chrome (thumb
or well is the only selected surface); idle icons/text `iconColor` /
`textMutedColor`; thumb/well leaves a small track inset (`spaceXs` optical
margin). Adjustment navbar keeps OutBack slide + land scale; method segments
and list wells use opacity fade where motion is short.

**Selection restore rule (adjustment panels):** each panel exposes
`loadFromSnapshot(snapshot)` and reads selection/values from the session
snapshot on `Component.onCompleted`, `onEditorSessionChanged`, and the stack’s
`AdjustmentSnapshotChanged` fan-out. List selection highlight must bind a
panel-level `selectedPath` (or equivalent) property — do not hide the
dependency inside a JS helper alone, or re-entry after a workspace switch will
leave the list without a selected row.

**Surface equality rule:** History/Versions rail and panel, adjustment shell,
filmstrip dock, and editor viewport placeholder all resolve base fill through
`cardSurfaceColor` / `colCardSurface`. Collapsed vs expanded must not change the
base fill — only width/height/opacity via fold progress. Disabled editor tools
keep that same card fill and mute text/icons / disable controls instead of
recoloring the shell to a second panel tone (which broke left/right column
unity). Scope and adjustment-nav tracks use `bgBaseColor` as sunken insets
inside the card — not a nested second card of the same fill.

---

## Spacing and margins

| Token | px | Typical use |
| --- | --- | --- |
| `spaceXs` | 4 | Tight insets |
| `spaceSm` | 8 | Compact gaps, section body margin |
| `spaceMd` | 12 | Default panel padding, desktop row gap |
| `spaceLg` | 16 | Panel title margins |
| `spaceXl` | 20 | Rare large separation |

### Library date activity graph

Capture-date heatmap in the album inspector (GitHub contribution-graph layout,
week columns that wrap to the inspector width). Geometry is independent of the
spacing ladder because the cells must stay dense enough for 365/366 days.

| Token | px | Use |
| --- | --- | --- |
| `dateGraphCellMinSize` | 10 | Floor for a day square; cells grow to fill the wrap row |
| `dateGraphCellGap` | 3 | Gap between day squares (GitHub pitch) |
| `dateGraphCellRadius` | 2 | Day-square corner |

Intensity is a neutral ramp from `bgBaseColor` toward `textColor`. It is not
GitHub green, not a theme accent, and not ad-hoc QML alpha:

| Token | Use |
| --- | --- |
| `dateGraphLevel0Color` | Empty / padding day (`bgBase` toward `textMuted`, 22%) |
| `dateGraphLevel1Color` | Lowest non-zero bucket (`bgBase` toward `text`, 25%) |
| `dateGraphLevel2Color` | Mid-low bucket (45%) |
| `dateGraphLevel3Color` | Mid-high bucket (65%) |
| `dateGraphLevel4Color` | Peak bucket (85%; stays below `textColor`) |

Selected day keeps its level fill and adds a 1 px `textColor` outline so
selection is not color-only. Month names sit above the week columns; there is
no weekday gutter. Calendar / activity uses `SlidingIconNav`; the year
picker reuses `AdjustmentCombo`. The day hover tip follows the pointer.

### Library workspace chrome

The Library workspace (grid, collections sidebar, Inspector, toolbar) is
monochrome in both themes. A theme accent appears only on the "Alcedo"
wordmark and the small import progress ring.

| Surface | Treatment |
| --- | --- |
| Neutral filled action: Load Project (empty Library), Import From Folder, Export queue | `editorListSelectedFillColor` fill, `Qt.darker(…, 1.06)` on hover and press; text and icon in `editorListSelectedInkColor`. Disabled: `bgBaseColor` fill with `textMutedColor` ink |
| Collections tree | File-browser rows of `collectionRowHeight`: `collectionIndentStep` indent per level, a `▸`/`▾` disclosure glyph in `textMutedColor` (the Library section-header glyphs), then the name in `fontSizeTitle` `textColor`. No folder icon. Names elide (uniform fixed-height rows). The root row (all images) has no disclosure glyph and aligns with the top-level collections. The photo count of the collection sits right-aligned after the name in `dataFontFamily` `fontSizeCaption` `textMutedColor`, with locale digit grouping; the root row counts the whole project. A row shows no count before the first count read and while its name field is open (issue #733) |
| Selected collection row | Quiet neutral well: `controlHoveredFillColor` with `controlRadiusSmall` corners; the name keeps `textColor` and turns `fontWeightStrong`; the photo count brightens from `textMutedColor` to `textColor` (user request 2026-11-07: the dark selected ink read dimmer than the idle count); the glyph stays `textMutedColor`. This is the per-file exception to the bright `editorListSelectedFillColor` well (user request 2026-11-07: the bright well was too heavy in the dense tree). No side stripe. Hover is the darker `hoverColor` wash |
| Collection drop target (drag to move) | `hoverColor` wash with a 1 px `textMutedColor` outline; the dragged name follows the pointer on a `bgBaseColor` label with a 1 px `dividerColor` outline. Rows that cannot take the folder (its own subtree, its current parent) show no outline |
| Collection drop target (file drop to import) | The same row style as the drag to move, on the row under a drag of local files and folders from the system file manager (issue #757). The draft row and the row being renamed show no outline. Every Library file drop answers Copy, so the file manager never deletes the dropped files |
| Library grid file drop overlay | Over the grid while local files and folders are dragged onto it: `colBgDeep` at alpha 0.78 with `controlRadiusSmall` corners and a 1 px `textMutedColor` outline; centered `import.svg` (28 px) in `textColor` above "Import to" and the current collection name in `fontSizeSection` `fontWeightHeading`. No explanatory text |
| New-collection and rename name field | Inline in the tree row: a plain `TextInput` (no Controls `TextField`, so no style-provided floating label or underline) in a `collectionRowHeight` - 4 high `bgBaseColor` box with `controlRadiusSmall` corners; 1 px `dividerColor` border, `textMutedColor` on focus; placeholder in `textMutedColor` inside the box; text selection `textMutedColor` (alpha 0.6) with `textColor` text. The "Add to" filter field uses the same construction |
| Grid zoom slider | `textColor` fill and handle on the `bgBaseColor` track; the handle has no border |
| Inspector resize handle | `textMutedColor` on hover and drag |
| Album Inspector sections (`InspectorSection.qml`) | Every field of the Library Overview (By Capture Date, By Import Time, By Edit Time, By Camera Model, By Labels, By Custom Label, By Rating, By Lens, Search Filter) is one rounded card: `bgBaseColor` fill, 1 px `cardBorderColor`, `panelRadius` corners, `spaceMd` from the panel edges, `spaceSm` between the cards. The header and, when the section is expanded, its content sit inside the same card; a collapsed section is the 36 px header alone. In the header: the fold glyph `▾`/`▸` in `textMutedColor` at the left (the Collections tree glyphs; empty for a section that does not fold), the title in title case (`uiFontFamily`, `fontSizeBody`, `fontWeightStrong`, `textColor`, no upper-case transform), and the sort ascending, sort descending, and Group actions (`InspectorFieldActions`) at the right with `spaceXs` between them. The title elides at the right; the actions stay on the title row at the minimum inspector width (300 px) in every UI language. Only when the header cannot hold the glyph, a 56 px title, and the actions do the actions move to a second header row, together at the right. A click on the header outside the actions folds the section with the fold rules of "Motion": the card height follows the clipped content, the content fades with the same progress, and the `▸` glyph turns 90 degrees to point down (`motionFoldOpenMs` / `motionFoldCloseMs`, `motionEasing`; `reduceMotion` makes it a cut); the date content stays loaded until the close ends. The content has `spaceMd` side and bottom padding. Controls inside the card keep their contrast against the `bgBaseColor` fill: chips and day tiles use `controlFillColor` (hover `controlHoveredFillColor`) with a 1 px `cardBorderColor`, the "+N more" and "Show less" chips are transparent with a 1 px `controlBorderColor`, the lens row hover is `controlFillColor`, and the calendar/activity switch track and the search field are `bgCanvasColor`. The Search Filter card shows the query text directly, without a nested box (user decisions 2026-11-09, issue #766: the sections had no visible boundaries, the actions spread across a second row, then rounded cards that extend over the expanded content, consistent with the other panel cards) |
| Album Inspector value search (By Camera Model, By Labels, By Lens) | First row of the section content: a plain `TextInput` in a `collectionRowHeight` hairline box (`bgCanvasColor`, sunken against the card; `controlRadiusSmall`, 1 px `dividerColor` border, `textMutedColor` on focus), a 12 px `search.svg` in `textMutedColor` at the left, the "Search" placeholder in `textMutedColor`, and a `×` that clears the text once there is text (Escape also clears). Typing filters the values of that section (case-insensitive, matches inside the value) and shows every match without the preview limit; the selected value stays in the list, first when it does not match. The field never changes the library query. The field is hidden when the section has no values |
| By Custom Label (`ColorLabelFilterChips.qml`) | The section between By Labels and By Rating. Its content is one chip per color label code in code order (Red to Gray), always present, then "Deleted". A chip is 38 px high with fully round ends, like the By Labels chips: `controlFillColor` fill (hover `controlHoveredFillColor`), 1 px `cardBorderColor`; inside, a 10 px dot of the code color (`appTheme.colorLabelColors`; a 12 px `trash.svg` in `textMutedColor` for Deleted), the user name of the code (`colorLabelNames`, 11 px `fontWeight` 600, `textColor`), and the match-set count in `dataFontFamily` with tabular figures in `textMutedColor`. Counts below 10,000 use the digit grouping of the locale; larger counts use the short form (12.8K, 102K, 1.25M; 1.28万 in Chinese). The chip of the active filter is the `editorListSelectedFillColor` well with `editorListSelectedInkColor` text; the dot keeps its color. A click toggles the filter (`colorLabel` with the code, or `deleted`). The header action is one pencil (`edit.svg`) in the `InspectorFieldActions` action style; it opens Settings > Language / Theme scrolled to Library > Color Labels. The section has no sort or Group actions, because a photo can have several codes (issue #772) |
| Inspector footer (`LibraryWorkspace.qml`) | The marks control and the button row (`LibraryInspectorActions.qml`) sit inside the Inspector panel, below the scrolling pages, with the inset of the Album section cards: 10 px page margin plus `spaceMd` at the sides and the bottom, `spaceSm` above the footer and between the marks card and the button row. So the marks card, the buttons, and the section cards have one width, and the panel is one surface from the tabs to the button (user decision 2026-11-11: the marks card had the card fill but stood outside the panel at the full panel width). The buttons are 44 px high. The Export tab hides the marks card; the Export button takes its place in the same footer |
| Library marks control (`LibraryMarksControl.qml`) | One card above the button row in the Inspector footer on the Album and Image tabs (hidden on the Export tab): `bgBaseColor` fill, 1 px `cardBorderColor`, `panelRadius` corners, `spaceMd` side padding. Three 40 px rows with a 1 px `dividerColor` hairline between them; each row starts with its name (Rating, Label, Delete) in a 64 px column, `fontSizeCaption`, `textMutedColor`. Rating: five 18 px stars in the rating filter inks; when the selected photos have different ratings, all stars are hollow and "Mixed" (`textMutedColor`) follows them; a click sets that rating on every selected photo, and a click on the star of the common rating clears it. Label: the seven codes as 18 px circles, 8 px apart: filled with the code color when all selected photos have the code, a 1.5 px ring of the code color with an 8 by 2 px dash in its center when some have it, the ring alone when none has it; hover draws a 1 px `textMutedColor` ring 3 px outside the circle; the tooltip is the user name of the code. A click on a code that all photos have removes it from all; otherwise it adds it to all; the other codes of each photo stay. Delete: a 28 px neutral button "Mark" (the secondary button fill and border of the inspector buttons); when all selected photos have the Delete mark it is the light filled "Marked" (`filledActionFill`, `filledActionInk`), and a click clears the mark. The control shows each change at once, with the library cards. It is enabled under the conditions of "Add to Export Queue" while `interactionPolicy.canChangeStateAsUser` is true; disabled, the stars and circles are at 45% opacity (issue #772) |
| Inspector stats cards (date calendar, By Camera Model, By Labels, By Lens) | Neutral bars (`textMutedColor`) and rank dots (`textColor`, then `textMutedColor`). The selected chip, row, or day tile is the `editorListSelectedFillColor` well with `editorListSelectedInkColor` text. Hover follows the section card rules above (a `controlFillColor` step) |
| Rating filter and photo Inspector rating | Filled stars `textColor`, hover preview `textMutedColor`, other stars `ratingStarEmptyColor` |
| Active search filter row | No marker dot; the row text names the filter |
| Photo Inspector AI description | `sparkles.svg` in `textMutedColor` and a 3 px `dividerColor` rule |

### Library thumbnail cards

The Library cards (`ThumbnailGridView.qml` and the photo cells of
`AlbumSectionView.qml`) and the editor filmstrip tiles use one language for the
edit state and the rating. The shared parts are `ThumbnailEditMark.qml` and
`ThumbnailStarRating.qml`.

- **Selection:** the normal card fill and a 2 px `textColor` outline in both
  views. Hover is the `hoverColor` wash. There is no accent wash or frame.
- **Edit mark:** an edited photo (its active Version has a head commit, the
  same rule as the edit groups and the edit-time sort) shows a small chip on
  the top-right corner of the painted photo: `panel_icons/adjustments.svg` in
  `textColor` on a `thumbnailEditMarkFillColor` fill. The icon is 12 px on an
  18 px chip with a 4 px radius, inset `spaceXs` from the corner; at 8 columns
  and more it is 10 px on a 14 px chip with a 3 px radius, inset 3 px. The
  chip shows at every zoom level and is not part of the caption. On a card
  with a missing source it sits left of the problem dot. (User decision
  2026-10-28: the caption mark was too large; a filled chip on the photo
  replaces the earlier "no container" rule.) An unedited photo shows no mark
  and no "Unedited" text. The
  tooltip has two lines: the relative edit time ("Edited 2 hours ago",
  "Edited yesterday", then the date) and the name of the active Version. The
  mark of the photo open in the editor follows its history at once (commit,
  undo, redo, Version switch, reset).
- **Stars:** `★` / `☆` in `dataFontFamily`. Filled stars use `textColor`,
  empty stars `ratingStarEmptyColor`, the hover preview `textMutedColor`.
- **2 to 6 columns:** the date line shows the capture date (and the labels in
  the flat grid) on the left and the five stars on the right. The stars are a
  control. An unrated photo shows no stars at rest; the five empty stars fade
  in (`motionFadeMs`) while the card is hovered. Hover previews the rating up to
  the star under the pointer. A click sets that rating for that photo only
  (`ImageController::SetImageRating` after `EvaluateEditImageRating`); a click
  on the star of the current rating clears it to 0. Two quick star clicks do
  not open the editor. After a rating change the filled stars appear one after
  another, 30 ms apart, with a short scale settle in `motionFadeMs`;
  `reduceMotion` makes it a cut. A multi-selection keeps the context menu and
  keyboard paths.
- **8 and 11 columns:** one caption line with the file name, the color label
  dots, and only the filled stars; the file name elides first. The stars are display
  only: no hover preview, no stars for an unrated photo, and a click on them is
  a normal card click (selection).
- **14 columns:** no caption, so no stars. The edit mark chip still shows.
- **Rating change:** the photo row shows the new rating at once
  (`AlbumThumbnailModel::updateRating`), and the Library reads only the
  statistics of the current query again (`LibraryModule::NoteRatingsChanged`).
  The photo rows are not reloaded: the cards, the scroll position, and the
  selection stay. Only when the shown order or the shown photos depend on the
  rating (rating sort, rating group, or a rating condition in the Inspector
  filter or the search) does the Library refresh.
- **HDR mark (flat grid):** a neutral chip (`bgBaseColor` fill, 1 px
  `textMutedColor` border, `textColor` icon), not gold.
- The accessible name of a card includes the rating ("3 stars", "Unrated") and
  "Edited" for an edited photo.
- **Zoom:** a zoom step moves each card from its drawn rectangle to its new
  rectangle (position and size) in 280 ms (`OutCubic`; `reduceMotion` makes it
  a cut). The card under the pointer (Ctrl+wheel) or at the view centre keeps
  that point at the same height on the screen. Cards that leave the view move
  to their new place and fade out; cards that enter fade in. A step during the
  animation starts from the drawn rectangles of that moment. The thumbnail of a
  larger tier replaces the shown one when it is ready. A card that already shows
  a thumbnail larger than a smaller new tier keeps it and its pin, and requests
  no render (`ThumbnailTier.js`); a card without a thumbnail requests the new
  tier. The grouped view uses the same rules (260 ms).

### Library justified rows

Settings "Language / Theme" > Library > Photo layout selects the layout of the
ungrouped Library: "Classic grid" (the cards above) or "Justified rows"
(`LibraryViewPreferences`, QSettings `library/gridLayout`). Grouped photos
always use the classic cells.

- **Rows:** the tiles of one row have one photo height; the tile width follows
  the aspect ratio of the oriented photo, and a complete row fills the view
  width. The photo fills the tile (`PreserveAspectCrop`); there is no 3:2 box
  and no side bands. The last row keeps the target height and starts at the
  left edge. The zoom slider sets the target row height: 480, 360, 280, 220,
  180, 140, 104, 80 px.
- **Gaps:** `cardInset` between tiles and rows (8, 6, 4 px as on the cards).
- **Caption:** the file name and the color label dots (see "Library color
  labels"), in `dataFontFamily` at the card title size; 22 px high, 16 px from
  8 columns, none at the smallest level.
- **Hover details:** while the pointer is on a tile, a panel on the bottom of
  the photo shows the camera, ISO, and aperture line, the date and labels line,
  and the stars, on a `thumbnailDetailsScrimColor` fill; the panel fades in
  `motionFadeMs`. At the levels where the cards have the star control, the stars
  are the same control here; at smaller levels they are display only.
- **Marks and selection:** the edit mark chip, the missing-source mark, and the
  Delete mark sit on the photo as on the cards. Selection is the 2 px
  `textColor` outline around the photo.
- **New result:** a result that replaces the shown one (a filter, the search, a
  collection) shows from its first photo, as in the classic grid (#841).

### Library delegate cost (#836)

A scroll or a zoom at the smallest level shows about 200 photos at once, so the
Library keeps the work per photo small:

- **Reuse:** a justified tile is a slot of `JustifiedLayoutModel`; a scroll gives
  the slot of a photo that leaves to a photo that enters. The classic grid reuses
  its cards (`reuseItems`). A reused delegate pins its new photo once, after the
  model has written every role of the change.
- **Parts made when shown:** the hover details of a tile (made on the first
  hover), the text of a thumbnail problem, and the classic caption of the shown
  zoom level (none at 14 columns, one line at 8 and 11, three lines at 2 to 6;
  made between frames).
- **Thumbnail state:** a delegate reads the thumbnail URL and state from the
  roles of its model row.

### Library sort, group, and sections

| Token | px | Use |
| --- | --- | --- |
| `librarySectionHeaderHeight` | 40 | Fixed height of a grouped Library header row; scroll offsets use it |
| `inspectorHeaderActionSize` | 24 | Square of the Album Inspector sort and Group icon actions, the grouped-view disclosure, and caption text actions |

The 24 px header actions are a documented compact exception to the 40-44 px hit
band: they are compact icon actions in dense Inspector headers, not structural SVG
actions. They stay keyboard reachable with accessible names and tooltips.
The sort and Group icons are the user-provided Tabler `sort-ascending`,
`sort-descending`, and `category-plus` assets (approved 2026-10-02), kept at the
upstream 2 px stroke and drawn at `iconOpticalSizeCompact`.
Selected actions use the monochrome list well (`editorListSelectedFillColor` /
`editorListSelectedInkColor`). Per-file details: `docs/VI/README.md`.

### Library loading

A Library read that replaces the shown result (a collection switch, an Inspector
filter, the search, a sort or group change) shows `StaggeredGridLoader.qml`. The
loader appears `libraryLoadingRevealDelayMs` after the read starts, so a fast read
draws no frame of it. A read that changes the shown result in place (a rating
write, an import into the shown collection) keeps the grid as it is and shows no
loader. The C++ side reports the first kind through `LibraryModule.queryReplacing`.

| State | Surface |
| --- | --- |
| No result shown (collection switch) | Placeholder grid: `bgBaseColor` cards at the column count, gap, card proportion, and radius of the classic photo grid at the current zoom level. A `textColor` wave at 10 % runs along the diagonals, top left to bottom right, 1600 ms per pass. The empty state ("No Photos Yet") stays hidden until the read publishes |
| Previous result shown (filter or search change) | The previous grid dims to 30 % over `motionFoldOpenMs`; a 72 px `bgPanelColor` card with a `glassStrokeColor` hairline stands at its centre and holds a 36 px `StaggeredGridMark` |
| One photo loading (a card, a justified tile, an Editor filmstrip card) | A `StaggeredGridMark` at the centre of the photo area until the thumbnail is ready: 27 px on Library cards and tiles, `iconOpticalSize` on filmstrip cards |
| Read failed | The previous result stays; the query row shows the error in `dangerColor` with Retry |

`StaggeredGridMark.qml` is the compact 3 x 3 staggered grid. Each tile rests at 75 %
size and 15 % wave level and grows to full size and full `textColor` as the wave
passes, `loadingWaveCycleMs` per pass. Its spacing is the mark width / 9 and its
tile radius the mark width / 18, both rounded, so one mark serves every size.

A dense grid shows a mark on every loading card, so a mark is one `ShaderEffect`
quad: `qml/shaders/staggered_grid_mark.frag` draws the nine tiles. Every mark reads
one shared clock, `appTheme.loadingWavePhase`, which advances every
`loadingWaveFrameMs` while at least one mark shows. All marks pulse together, and
the window repaints at the clock rate instead of the display rate.

The loaders draw no Material `BusyIndicator` and no spinner. Under `reduceMotion`
the wave stops and every tile draws at its rest level.

| Parameter | Value | Meaning |
| --- | --- | --- |
| `libraryLoadingRevealDelayMs` | 150 ms | Time from the start of a replacing read to the first loader frame |
| Placeholder wave strength | 10 % | Opacity of `textColor` over a placeholder card at the crest of the wave |
| Compact mark wave strength | 85 % | Opacity of `textColor` over a compact tile at the crest of the wave |
| `loadingWaveCycleMs` | 1200 ms | One pass of the compact mark wave |
| `loadingWaveFrameMs` | 33 ms | Interval of the shared mark clock (about 30 updates per second) |
| Stale result opacity | 30 % | Opacity of the previous grid while the replacing read runs |

### Library color labels

A photo carries any of seven color label codes (#768). The colors are fixed in
every theme; `appTheme.colorLabelColors` lists them in code order. The name of a
code is the user name from Settings, or the translated color name
(`appModules.libraryView.colorLabelNames`).

| Code | Color name | `colorLabelColors` value |
| --- | --- | --- |
| 0 | Red | `#E5534B` |
| 1 | Orange | `#E8863A` |
| 2 | Yellow | `#D8B43A` |
| 3 | Green | `#4FAE6B` |
| 4 | Blue | `#4C8DDB` |
| 5 | Purple | `#9B6BD3` |
| 6 | Gray | `#8C8C8C` |

Settings > Language / Theme > Library > Color Labels (`ColorLabelNamesEditor.qml`,
#770): one 36 px row per code, in code order. A 12 px color dot in a 22 px cell;
the name field, a plain `TextInput` in a 28 px `bgBaseColor` box with
`controlRadiusSmall` corners and a 1 px `dividerColor` border (`textMutedColor`
on focus); the translated color name as the placeholder in `textMutedColor`; a
`reset.svg` glyph in `textMutedColor` in a 28 px cell, shown only on a row with a
user name. The rows are at most 520 px wide. Names follow the Apply button of the
dialog, like the other settings of the page.

**Caption dots** (`ThumbnailColorLabelDots.qml`, #771): the classic cards, the
justified tiles, and the grouped cells show one dot per code after the file
name, in code order. A dot is 9 px (7 px at 8 and 11 columns), overlaps the
previous dot by 3 px, and has a 1.5 px ring of the surface behind it: the card
fill on a card (`cardSurfaceColor`, `hoverColor` while hovered) and
`glassPanelColor` under a justified tile. The space from the name to the first
dot color is 6 px (4 px at 8 and 11 columns). The file name elides first; the
dots stay whole. At 8 and 11 columns the line is the name, the dots, and the
filled stars. A photo without codes shows no dots. The accessible name of the
card lists the names of its codes.

**Delete mark** (`ThumbnailDeleteMark.qml`, #771): the photo box (the photo
area of a justified tile) is covered by a `rgba(6,6,6,0.72)` scrim with a round
trash badge in the centre: 34 px (24 px at 8 and 11 columns), `bgDeepColor`
fill, 1 px `controlBorderColor` hairline, `panel_icons/trash.svg` in
`textColor` at 16 px (12 px). The photo under it is desaturated (MultiEffect
saturation -0.65). The edit mark chip and the justified hover details stay above
the scrim. The file name turns `textMutedColor`. The mark takes no pointer
input: the photo stays selectable and editable. A change of the marks updates
the shown cards in place (`AlbumThumbnailModel::updateMarks`), without a
delegate rebuild or a scroll move.

---

## Radii

| Token | px | Use |
| --- | --- | --- |
| `panelRadius` | theme (10 Alcedo / 8 Classic) | Editor cards, filmstrip, rail shells |
| `controlRadius` | 10 | Icon actions, nav pills |
| `controlRadiusSmall` | 8 | Inner wells, collapsible section |
| `badgeRadius` | 6 | Crop angle badge, small chips |

---

## Application shell

Desktop chrome is a full-width toolbar above a two-column body:

`Vertical(Top toolbar, Horizontal(Collections sidebar, Vertical(Workspace, Background tasks)))`

The Library workspace inside that right stack is `Horizontal(Browser, Inspector)`.
The wordmark lives on the collections card. The collections toggle and
Library/Editor capsule remain on the full-width toolbar with File, Settings,
update, inspector toggle, and drawn caption buttons.

The top toolbar is one compact action band: `iconButtonHitSizeCompact +
spaceSm` (48 px), followed by `spaceSm` before the body so it preserves editor
height. The collections card remains a floating `panelRadius` surface with
`spaceMd` leading and bottom insets on every platform. On macOS, native traffic
lights sit over the toolbar; their vertical center is derived from `spaceMd +
(iconButtonHitSizeCompact + spaceSm) / 2`, exactly matching the toolbar action
axis. The buttons stay inside the system title-bar container. That container
is grown downward so the visible frames remain inside AppKit's title-bar hit
region; empty title-bar area passes events through to the QML toolbar. Resize
and maximize re-apply the same geometry. The close button keeps its 17 px
horizontal correction. The card must not be made flush or square to host the
wordmark.

The collections sidebar folds to zero width after a Library / Editor route
change. Width, following workspace gap, and opacity use `motionFoldOpenMs`,
`motionFoldCloseMs`, and `motionFadeMs`; `reduceMotion` resolves them to zero.
Its toggle stays at the leading side of the full-width toolbar, after the
macOS traffic-light reserve when applicable, and switches between the active
and inactive sidebar glyphs without moving. The Library/Editor capsule stays
beside it so workspace routing remains available while the sidebar is folded.

---

## Welcome surface

The welcome surface (`WelcomeDialog.qml`) is the modal start screen. It shows
the project that the app previews at startup and the other recent projects.
Product specification: `docs/roadmap/alcedo_studio/ui/welcome_project_overview_plan.md`
section 3. Per-file decisions: `docs/VI/README.md`, "Welcome surface".

`Vertical(blur backdrop, Card(Horizontal(Left column, Right column)))`

| Token | px | Use |
| --- | --- | --- |
| `welcomeCardWidth` | 1040 | Card width; the card shrinks to the window minus `spaceXl` on each side |
| `welcomeCardHeight` | 600 | Card height; same shrink rule |
| `welcomeSidebarWidth` | 260 | Left column (wordmark, actions, language, Quit) |
| `welcomeCoverWidth` | 300 | Cover block width |
| `welcomeCoverCompactWidth` | 240 | Cover block width when the right column cannot hold `welcomeCoverWidth + spaceXl + welcomeInfoMinWidth` |
| `welcomeCoverHeight` | 224 | Cover block height; also the information block height |
| `welcomeInfoMinWidth` | 260 | Information block minimum width |
| `welcomeEmptyContentWidth` | 400 | Text block width of the no-recent-project well |

Color and type map (the design canvas values map to existing tokens; the
welcome surface adds no color token):

| Role | Token |
| --- | --- |
| Backdrop | blurred shell + `overlayColor` (same modal shell as Settings) |
| Card | `cardSurfaceColor`, 1 px `cardBorderColor`, `panelRadius` |
| Recent list well, no-recent-project well | `bgBaseColor`, `controlRadiusSmall` (sunken track rule) |
| Recent row hover | `hoverColor` (the well is `bgBaseColor`, so `buttonHoveredFillColor` would not show in the Alcedo theme) |
| Primary action (Continue Editing, Create Project, empty-state New Project…) | `editorListSelectedFillColor` fill, `editorListSelectedInkColor` ink |
| Secondary action (Open Project…, New Project…) | `cardSurfaceColor` fill, 1 px `cardBorderColor`, hover `buttonHoveredFillColor` |
| Quiet action (Back, Quit) | no fill, `textMutedColor`; hover `buttonHoveredFillColor` + `textColor` |
| Keyboard focus | 1 px `textMutedColor` outline on the focused action or row |
| "Alcedo" in the wordmark | `accentColor` (the wordmark is the documented accent use) |
| Preview error | `dangerColor` body text |
| Cover tile without a photo, skeleton base | `bgBaseColor` |
| Wordmark | `headlineFontFamily`, `fontSizeHeadline`, `fontWeightHeading` |
| Numeric values (photos, edited, capture dates, relative time) | `headlineFontFamily` (Manrope). Do not use `dataFontFamily` here |

Rules:

- The cover block is one large tile (two parts of the width, both rows) and two
  small tiles, `spaceXs` apart. One `controlRadiusSmall` mask rounds the outer
  corners. Thumbnails use `PreserveAspectCrop`.
- The welcome actions use `WelcomeActionButton.qml` (`primary` / `secondary` /
  `quiet`). Height is `iconButtonHitSize` (compact: `iconButtonHitSizeCompact`).
  Icons reuse `folder-open.svg` and `folder-plus.svg`.
- No pill, badge, status dot, or compound `xx · xx` label. The statistics are
  a caption label above a value.
- While a project load runs, the recent rows, Open Project… and New Project…
  are disabled (one load at a time).

## Editor panel geometry

Side-panel and scope sizing for the editor desktop. Values are logical px; Qt
scales by DPR so they stay comfortable at 1.0 / 1.25 / 1.5 / 2.0. The preferred
width unifies the adjustment stack and the History/Versions/Nodes expanded panel so
the two side columns read as one family.

| Token | px | Use |
| --- | --- | --- |
| `editorSidePanelWidth` | 320 | Preferred width: adjustment stack + History/Versions/Nodes expanded panel |
| `editorSidePanelWidthMin` | 260 | Adjustment stack minimum (narrow-window floor) |
| `editorSidePanelWidthMax` | 460 | Adjustment stack maximum |
| `editorScopeHeight` | 192 | Histogram / waveform slot preferred height |
| `editorScopeHeightMin` | 160 | Histogram / waveform slot minimum height |
| `editorLutBrowserPanelWidth` | 560 | LUT browser rail page default width (drag-resizable; double click restores it); capped so the viewport keeps its minimum width |
| `editorLutBrowserPanelWidthMin` | 280 | Narrowest drag-resized LUT browser page |
| `editorLutBrowserFilterWidth` | 184 | LUT browser filter sidebar width |
| `editorLutTileMinWidth` | 128 | Minimum LUT tile width; the grid fits as many columns as this allows |
| `editorLutTileIconSize` | 36 | LUT tile cube placeholder icon, centered in the preview slot until the preview is ready |
| `editorAdjustmentHeaderMinHeight` | 58 | Node-name / EXIF header under the scope: EXIF caption line, `spaceXs` gap, compact Mask-tool row |
| `collectionsSidebarWidth` | 276 | Persistent left collections column |
| `collectionRowHeight` | 26 | Row of the collections tree, of its inline name field, and of the "Add to" collection list |
| `collectionIndentStep` | 14 | Indent per tree level in the collections tree and the "Add to" list; also the width of the disclosure-triangle column |
| `collectionMenuMaxRows` | 12 | Collection rows that the "Add to" sub-menu shows before its list scrolls |
| `collectionMenuFilterThreshold` | 15 | Collection count at which the "Add to" sub-menu shows its filter field |

### Search palette

`SearchPalette.qml` is the global search launcher (see `docs/VI/README.md`).

| Token | px | Use |
| --- | --- | --- |
| `searchPaletteMaxWidth` | 960 | Widest palette; the settings drawer opens inside this width |
| `searchPaletteWindowMarginX` | 28 | Smallest left and right gap between the palette and the window |
| `searchPaletteWindowMarginY` | 36 | Smallest top and bottom gap between the palette and the window |

**Editor close confirm:** `EditorCloseConfirmDialog` uses the same blur +
`overlayColor` modal shell and `DialogActionButton` actions (Cancel / Discard /
Save). Save explicitly calls `Finalize(true)`, routes to Library, then waits on
`sessionState` (`Saving` / `Switching`, same check as the filmstrip) until
`NoImage` before quitting. Discard uses `Finalize(false)`. Ordinary Library /
Editor workspace navigation only changes visibility; it preserves the editor
session, retained QML tree, viewport, and presentation sink.

The History/Versions rail width (60 px) and rail-button hit (46 px) stay under
Icon and action geometry; the rail width is not tokenized because it is locked to
the rail-button optical balance.

### Scope plot colors

Scope plot colors are semantic AppTheme tokens so the scene-graph item does not
carry palette literals into QML. They are used by both the histogram and the
QPainter-backed waveform density image.

| Token | Use |
| --- | --- |
| `scopeGridColor` | Guide lines: histogram zone lines, waveform level lines |
| `scopePlotBorderColor` | Plot baseline |
| `scopeHistogramRedColor` | Red channel hue |
| `scopeHistogramGreenColor` | Green channel hue |
| `scopeHistogramBlueColor` | Blue channel hue |
| `scopeMarkerColor` (`textColor`) | Histogram clip markers |

### Scope plot

The plot well is a `bgBaseColor` sunken inset with `controlRadiusSmall`
corners and no rules. The plot sits `spaceSm` inside the well on every side:
the `EditorScopeItem` has a `spaceXs` margin, so its square opaque surface stays
inside the rounded corners, and a `plotPadding` of `spaceSm - spaceXs`. One
`scopePlotBorderColor` baseline is under the plot in both modes.

- **Histogram.** Each channel is a filled area with a 1.25 px edge line. The
  channels add up (`CompositionMode_Plus`): the hue of each channel tint is
  scaled so that the three add up to white. Two channels show the mixed hue
  (red + green = yellow), all three show a neutral tone. The color is data
  color, so it does not break the monochrome chrome rule. The guide lines are
  the three vertical zone lines at 25, 50, and 75 percent (shadows, midtones,
  highlights). The height scale is a square-root scale against a robust
  reference (`BuildScopeHistogramRenderData`), so there are no horizontal level
  lines. A peak higher than the plot shows a 2 px break in the background
  color, 4 px under the top. A populated bin is at least 1 px high.
- **Clip markers.** When the shadow or highlight tail clips (2 percent of the
  pixels of a channel in the end bins), a `scopeMarkerColor` corner triangle
  (7 px) shows at the top of that end of the plot. There is no marker when the
  end does not clip.
- **Waveform.** The density image fills the same plot rectangle. The guide
  lines are the three horizontal level lines at 25, 50, and 75 percent.

### Plot pane

`PlotPane` is the reusable plot item of editor plots (the Tone Curve section is
its first user). It holds a view of the data (zoom and pan inside the data
limits), insets around the plot rectangle for labels, and an ordered list of
layers: the first layer draws at the bottom. Each layer has its own colors and
sizes as properties; bind them to the tokens below.

| Token | Value | Use |
| --- | --- | --- |
| `plotSurfaceColor` | `bgBaseColor` | Plot rectangle fill (`PlotPane.surfaceColor`) and the break across a cut histogram peak |
| `plotGridColor` | `bgBaseColor` to `textMutedColor` at 22 percent | Ordinary grid lines |
| `plotGridStrongColor` | `bgBaseColor` to `textMutedColor` at 55 percent | Strong grid line (for example 18% grey on the curve axis) |
| `plotLabelColor` | `textMutedColor` | Tick labels in the insets |
| `plotLineColor` | `textColor` | Data line (the curve) |
| `plotReferenceLineColor` | `bgBaseColor` to `textMutedColor` at 70 percent | Reference lines (the dashed identity line) |
| `plotHandleColor` | `textColor` | Handle ring; fill of the selected handle |
| `plotHandleOutlineColor` | `bgDeepColor` at alpha 200 | Dark disc behind a handle, so it reads over the histogram |
| `plotGridLineWidth` | 1 px | Grid line width |
| `plotLineWidth` | 1.25 px | Data line and histogram edge width |
| `plotHandleRadius` | 3.5 px | Handle radius: 7 px rings |
| `plotHandleRingWidth` | 1.25 px | Handle ring width |
| `plotHandleOutlineWidth` | 1 px | Width of the dark outline around a handle |
| `plotHitRadius` | 8 px | Pointer distance that hits a handle or a line |

- **Layers.** `PlotGridLayer` (lines at ticks, labels under and left of the
  plot), `PlotHistogramLayer` (the scope histogram look: additive channel
  fills with a 1.25 px edge, `scopeHistogram*Color` tints, one channel in its
  own tint when the layer shows one channel), `PlotPolylineLayer` (solid or
  dashed line), and `PlotHandleLayer` (rings; the selected handle is a filled
  disc; a hovered or pressed handle grows by 1 px). Widths are the visible
  width with the antialiased edge included.
- **Input.** A handle follows the pointer 1:1. Ctrl + wheel zooms at the
  pointer; the plain wheel scrolls the panel. A drag in an empty area moves a
  zoomed view. Cursors: pointing hand over a handle or a line, closed hand
  while dragging, open hand over an empty area of a zoomed view.

### Tone Curve section

The Tone Curve section of the Tone panel (`EditorToneCurveSection.qml`) is a
`CollapsibleSection` with the header actions Histogram (toggle,
`histogram.svg`), Fit (toggle, `fit-range.svg`), and Reset (`reset.svg`), all
`IconActionButton` compact; a selected toggle uses the selected fill. There is
no caption sentence.

- **Row 1.** Channel selector `RGB`, `R`, `G`, `B` (`SegmentedCardSwitcher`).
  A segment of a curve that is not the identity shows a `plotChannelMarkSize`
  dot before its title, in the ink color of the segment. The user asked for
  this dot in #890; it is the only status dot of the editor panels.
- **Plot.** A square `PlotPane` with `plotSurfaceColor`. Grid every 2 EV from
  18% grey with the strong line at 18% grey; signed EV labels under the plot
  only (`fontSizeCaption`, `plotLabelColor`). The histogram of the curve input
  is behind the curves. The dashed identity line uses
  `plotReferenceLineColor`, 4 px dashes with 4 px gaps. The curve of the
  selected channel is 1.25 px in `plotLineColor` (RGB) or its channel color;
  the curves of the other channels that are not the identity show in their
  colors at `plotInactiveLineOpacity`. Handles are the plot pane handles.
- **Pointer.** Over the plot, a 1 px vertical line in `plotPointerLineColor`
  follows the pointer, and `In` and `Out` with their EV values show at the top
  left of the plot in `fontSizeCaption` (`plotLabelColor` names,
  `textColor` values).
- **Row 2.** `In` and `Out` fields of the selected point: a plain `TextInput`
  in a hairline box (`bgBaseColor`, `controlRadiusSmall`, 1 px `dividerColor`,
  `textMutedColor` border on focus), 4 decimals, each followed by its EV value
  in `textMutedColor`.

| Token | Value | Use |
| --- | --- | --- |
| `plotChannelRedColor` | `#E86666` to `textColor` at 12 percent | Red curve |
| `plotChannelGreenColor` | `#74C266` to `textColor` at 12 percent | Green curve |
| `plotChannelBlueColor` | `#66A0E8` to `textColor` at 12 percent | Blue curve |
| `plotPointerLineColor` | `bgBaseColor` to `textMutedColor` at 45 percent | Vertical line at the pointer |
| `plotInactiveLineOpacity` | 0.4 | Opacity of the curves of the other channels |
| `plotCurveHitRadius` | 6 px | Pointer distance that adds a point on the curve line |
| `plotChannelMarkSize` | 4 px | Dot of a changed channel in the channel selector |

---

## Nodes panel and graph nodes

The Nodes page is the third expandable page in `EditorWorkspaceRail`. It shares
the rail shell, width limits, fold motion, and Loader lifetime with History and
Versions. Only one tool page can be open.

### Nodes and Mask Groups rail icons

Use `panel_icons/pipeline.svg` for the Nodes toggle. It uses the user-provided
Tabler `pipeline` paths:

```svg
<path d="M3 4h8" />
<path d="M4 4v5a6 6 0 0 0 6 6h3a1 1 0 0 1 1 1v4" />
<path d="M10 4v4a1 1 0 0 0 1 1h3a6 6 0 0 1 6 6v5" />
<path d="M13 20h8" />
<path d="M12 9v6" />
```

Use `panel_icons/box-multiple.svg` (user-provided Tabler `box-multiple`) for the
Mask Groups rail toggle, so it does not repeat a Nodes-like layer icon. The
Mask Group creation action keeps the Tabler `stack-2` layer icon
`panel_icons/nodes.svg`. Do not use `panel_icons/masks.svg` as the Mask Groups
rail icon; that asset represents an individual Mask or Mask preview.

Both rail assets use the shared 24×24 viewBox, white source stroke, and their
user-approved 2 px stroke width. Tint them with `ColorImage`. Do not add a
count, badge, or status dot to either icon.

### Graph canvas

The graph uses a vertical Develop-to-DRT/Post backbone. With no image, the canvas
shows the localized empty copy and no previous graph. While the session is
Loading, Acquiring, or Switching, it shows the loading copy and still hides the
previous image graph. A pending topology command shows muted plain text, not a
pill, badge, or status dot.

Keyboard input is scoped by focus. Nodes command ids and default sequences live
in `ShortcutRegistry` (`RegisterNodesPanelShortcuts`). The graph matches a key
through `commandIdForKey` in `EditorNodesPanel.handleGraphKey` with
`Keys.priority: Keys.BeforeItem` so the navigable canvas does not swallow Home,
arrows, or `C`. Masks disclosure keys stay on `EditorNodeMaskDrawer`. Add
confirm stays on `IconActionButton`.

| Input | Focus | Result |
| --- | --- | --- |
| `Tab` / `Shift+Tab` | Add, graph, Masks headers | Move between those targets |
| `Up` / `Down` | Graph | Previous / next backbone node |
| `Home` / `End` | Graph | Select Develop / DRT/Post |
| `Enter` / `Space` | Masks header | Open or close that drawer |
| `Enter` / `Space` | Add | Add a Color Grade |
| `F2` | Graph | Rename the selected Color Grade |
| `Delete` | Graph | Delete the selected Color Grade |
| `Ctrl++` | Graph | Add a Color Grade |
| `Ctrl+0` | Graph | Fit the graph |
| `C` | Graph | Pin Connect from the selected Develop or Color Grade |
| `Enter` | Graph, Connect active | Request the exclusive-port connection |
| `Escape` | Graph | Cancel rename or Connect; otherwise keep graph focus |

The canvas is a uniform
deep AppTheme surface with no grid: the panel assigns `grid: null` on the
`Qan.GraphView`, which swaps in QuickQanava's empty default grid. Do not add a
line or point grid, a nested card, minimap, gradient, shadow, glow, or glass
effect.

Use AppTheme roles for the canvas, edge, port, candidate edge, and node
selection. Add each missing role to `AppTheme` and this file in the same change.
Do not put raw graph colors in QML.

| Token | Value | Use |
| --- | --- | --- |
| `graphCanvasColor` | `bgDeepColor` | Graph view surface |
| `graphEdgeColor` | `textMutedColor` | Permanent backbone edge |
| `graphCandidateEdgeColor` | blend of `textMutedColor` toward `textColor` (0.35) | Connector request preview |
| `graphPortFillColor` | transparent | Hollow square port fill (canvas shows through) |
| `graphPortBorderColor` | `#3FB950` | Hollow square port outline |
| `graphNodeBorderColor` | blend of `cardSurfaceColor` toward `textMutedColor` (0.5) | Visible node card outline |
| `graphMaskDrawerSurfaceColor` | `bgBaseColor` | Sunken Mask drawer well inside the card |
| `graphSelectionOutlineColor` | `textColor` | Selected node outline |
| `graphNodeWidth` | 220 | Color Grade and endpoint width |
| `graphNodeVerticalGap` | 48 | Vertical space between backbone nodes |
| `graphNodeOriginX` | 48 | First-layout left origin |
| `graphNodeOriginY` | 48 | First-layout top origin |
| `graphEndpointHeight` | 40 | Develop / DRT/Post height |
| `graphNameRowHeight` | 32 | Color Grade name row |
| `graphNameRowDividerHeight` | 1 | Hairline between name row and Masks drawer |
| `graphMaskDrawerHeaderHeight` | 28 | `Masks` disclosure header |
| `graphMaskRowHeight` | 28 | One Mask type row |
| `graphPortSize` | 8 | Visible square port |
| `graphPortHitSize` | 16 | Port pointer target |
| `graphEdgeWidth` | 2 | Backbone stroke |
| `graphSelectionOutlineWidth` | 1 | Selected outline (inside the card; does not change node size) |

Ports are hollow green-outlined squares. The Alcedo horizontal dock delegate
(`EditorNodePortDock.qml`) uses zero outer margin, so port squares sit directly
against the node card edge; do not reintroduce the QuickQanava default dock
margin. The dock forwards its own position changes to
`hostNodeItem.updatePortsEdges()` (the upstream VerticalDock #145 pattern):
edge endpoints only recompute on port-local geometry changes, so without that
forwarding a dock move — anchor placement after creation or a drawer fold
changing the host height — leaves edges visually detached from their ports.
Node selection paints only the card outline: the adapter installs an
invisible QuickQanava selection delegate, so the default blue animated
selection item never renders. This enforces the global blue-frame ban; do not
restore the upstream decoration for focus, selection, or animation. That
delegate is still a full-size `z: 1` child
of the node; the card uses `z: 2` and the port dock uses `z: 3` so Mask rows
and ports stay the pointer target. Do not set `selectionDelegate` to null in
QML; null resets the QuickQanava default instead of disabling it.

### Editor viewport Mask overlay

Retained QSG Mask controls sit on `EditorOverlayItem` above the photograph. They
use a two-layer high-contrast stroke. The QSG overlay never paints a coverage
fill, heatmap, or completed Brush path; the Interactive photograph supplies
coverage feedback. Temporary Brush cursor/path guides are allowed during initial
drawing. Selected Radial shows the base ellipse and inner/outer feather boundary
lines during creation and later editing. Selected Gradient uses three parallel
lines with Geometry crop-style dual high-contrast strokes and short edge grips, without a kite,
closed polygon or crop dimming. Radius/feather and direction controls must be independently
reachable. Unselected analytic masks do not retain editing guides.

Mask editing has three mutually exclusive states: inactive, creating, and
editing an existing Mask. The header creation buttons and a Mask row selection
both enter this state machine and immediately open the transient Mask page.
`Enter` and `Escape` finish the edit, hide the overlay, and return to the prior
adjustment page. Selecting any node-parameter adjustment page also finishes the
Mask edit before opening that page. `Delete` removes the selected Mask and exits;
before a new Mask exists it exits the armed creation tool. These shortcuts are
disabled when Mask editing ends, so the Nodes graph resumes ownership of
`Delete` for Color Grade removal.

Handle and stroke widths are logical pixels and stay constant at any zoom or
DPR. Image-space Brush cursor radius is transformed through the shared
ReferenceSpace mapping. Do not add a coverage-area color to the QSG overlay. Do
not use Material controls, badges, pills, or status dots on this overlay.

Selected Radial shows the base ellipse plus inner and outer feather boundary
lines from the evaluator inverse. Coincident rhos are drawn once. A collapsed
inner contour is omitted rather than producing invalid geometry. Feather
handles are rings; radius handles are discs. Selected Gradient uses three
photograph-clipped parallel loci with Geometry crop-style dual strokes and
short edge grips. Ends are not joined into a kite, diamond, or closed
polygon. Crop overlay dimming is not reused.

| Token | Value | Use |
| --- | --- | --- |
| `maskOverlayControlColor` | `textColor` | Selected handle fill and connector stroke |
| `maskOverlayControlOutlineColor` | `bgCanvasColor` | Handle outline |
| `maskOverlayInactiveColor` | `textMutedColor` | Temporary initial-creation guides |
| `maskOverlayHandleRadius` | 5 | Handle disc radius (logical px) |
| `maskOverlayHandleOutlineWidth` | 1.2 | Handle outline width (logical px) |
| `maskOverlayStrokeWidth` | 1.5 | Connector and cursor stroke (logical px) |
| `maskOverlayAntialiasWidth` | 1.0 | Premultiplied edge-alpha fringe (logical px) |
| `maskOverlayHandleHitRadius` | 12 | Pointer hit radius (logical px) |
| `maskOverlayRotateHandleOffset` | 24 | Rotation/direction handle offset (logical px) |
| `maskOverlayGuideOuterWidth` | 3.0 | Geometry-crop outer guide stroke (logical px) |
| `maskOverlayGuideInnerWidth` | 1.2 | Geometry-crop inner guide stroke (logical px) |
| `maskOverlayGripOuterWidth` | 5.0 | Geometry-crop outer edge-grip width (logical px) |
| `maskOverlayGripInnerWidth` | 2.4 | Geometry-crop inner edge-grip width (logical px) |
| `maskOverlayGripSpanT0` | 0.38 | Start of the short edge grip along a visible guide |
| `maskOverlayGripSpanT1` | 0.62 | End of the short edge grip along a visible guide |

### Editor viewport Mask coverage display

The selected Mask coverage preview (an AI Mask candidate, one committed Mask, or
a Grade Mask Union) is shown by the one viewport composite in
`editor_viewport.frag`, after the base/detail photograph selection:
`a = clamp(coverage, 0, 1) * maskCoverageTintOpacity`,
`rgb = photo * (1 - a) + maskCoverageTintColor * a`. Uncovered pixels keep the
photograph value and every pixel keeps its alpha. The QSG Mask controls stay
above this draw and keep their input; the coverage takes no pointer or keyboard
input.

The tint is presentation state: changing it or hiding the coverage redraws the
viewport without a photo render. It never reaches export, thumbnails, or scopes.
The tint color is used as a display-encoded value of the photograph's display
encoding. Do not add a second coverage composite in QSG, QML, or the photo
pipeline. The visibility control belongs to the AI Mask tool controls; there is
no separate settings panel.

The tint is a fixed light red in every theme. Do not derive it from `accentColor`
or another theme color: a blue or green accent is not visible on sky, water,
distant mountains, or foliage. The light red is visible on these and on warm
cream tones.

| Token | Value | Use |
| --- | --- | --- |
| `maskCoverageTintColor` | `#FF5050` (light red, every theme) | Coverage tint of the viewport composite |
| `maskCoverageTintOpacity` | 0.5 | Overlay opacity at full coverage |

### AI Mask tool panel

`EditorAiMaskPanel.qml` covers the adjustment pages while an AI Mask edit is
open. Every control is an icon action with an accessible name and tooltip; the
panel has no section captions and no busy text. From top to bottom:

1. Header: `pencil-ai` icon, the title (`New AI Mask` / `Edit AI Mask`) in
   `fontSizeSection` + `fontWeightHeading`, and a `close.svg` action (Cancel).
2. Tasks, compact `IconActionButton`s: Subject `mask_icons/ai-subject.svg`
   (Tabler `sparkles-2`, the AI feature mark), Background
   `mask_icons/ai-background.svg` (Tabler `background`), Sky
   `mask_icons/ai-sky.svg` (Tabler `cloud`), People `mask_icons/ai-people.svg`
   (Tabler `users`). People shows the selected fill while People is the last
   component.
3. People list while People is the last component: one flat row per person
   (`ai-people.svg` icon and `Person N`) in a sunken `bgBaseColor` well with a
   `controlBorderColor` edge. A
   selected (included) person uses `editorListSelectedFillColor` /
   `editorListSelectedInkColor`. A click toggles the person; hover outlines that
   person in the viewport with a dashed frame.
4. Prompt tools in a `SlidingIconNav`: Point `mask_icons/ai-point.svg` (Tabler
   `pencil`), Box `mask_icons/ai-box.svg` (Tabler `marquee-2`), Path
   `mask_icons/ai-path.svg` (Tabler `scribble`). A second `SlidingIconNav`
   holds Add `panel_icons/plus.svg` and Subtract `panel_icons/minus.svg`.
5. Click level, only for a single click: `SlidingIconNav` with
   `mask_icons/ai-level-1.svg` to `ai-level-3.svg` (a larger circle selects a
   larger region).
6. Feather and Expand: the shared `AdjustmentSlider`.
7. Temporary Undo / Redo: `reset.svg` / `retry.svg`, like the History panel.
8. Errors use `AlertBadge`; a failed coverage display adds a `retry.svg` Retry
   action next to its badge. A notice (no object found, the 64-point limit)
   is muted caption text.
9. One full-width Confirm `DialogActionButton` ("normal", the Browse LUTs
   style) stays below the scroll area. There is no Cancel button: Close in the
   header cancels.
10. While an operation runs (after `motionFadeMs`, so Undo does not flash), a
    `RotatingTaskBorder` in `backgroundTaskWorkingColor` turns around the whole
    panel and a `ThemedProgressBar` at the bottom shows the stage. The stage
    label names the step (`Starting the AI runtime`, `Loading the models`,
    `Running the model`, ...) with a separate `n/5` data label for the Open
    stages. A model run without stages uses the indeterminate bar.

The panel uses `cardSurfaceColor`. Icon actions use the raised control fills
(`controlFillColor`, hovered, pressed); a selected action (People while People
is active) uses `editorListSelectedFillColor` with `editorListSelectedInkColor`
ink. The `SlidingIconNav` tracks use `controlFillColor` with a
`controlBorderColor` edge and `iconColor` idle icons.

The AI icons are normalized to the 24×24 viewBox and white source stroke with
the user-approved 2 px stroke.

### AI Mask viewport input

`AiMaskInputOverlayItem` (QSG, above the photograph and below the viewport
chrome) draws the input of the AI Mask tool. It never draws coverage; the
coverage is the viewport composite above.

- Point prompts of the open prompt group: discs with a dark rim, filled with
  `aiMaskInputAddColor` (green, plus glyph) or `aiMaskInputSubtractColor` (red,
  minus glyph). These signed markers were requested by the user for this tool;
  they are not status dots.
- Path prompts: the stroke in the mode color with a dark rim, 4 px core. The
  stroke being drawn shows the same way and stays until its model step returns.
- Box prompts: the frame in the mode color with a dark rim.
- Selected people: the outline of the union of their boxes in
  `maskOverlayControlColor` with a dark rim; one outline per connected group.
- Cursor: the viewport hides the platform cursor and the overlay draws the
  tool cursor. Point: a crosshair with a center dot. Box: a crosshair with
  dashed guides across the photograph. Path: a ring with a center dot. A
  green plus or red minus badge shows the mode. While a model request runs
  the cursor uses `maskOverlayInactiveColor` and has no badge.

| Token | Value | Use |
| --- | --- | --- |
| `aiMaskInputAddColor` | `#30A46C` (every theme) | Add markers, strokes, boxes, and cursor badge |
| `aiMaskInputSubtractColor` | `#E5484D` (every theme) | Subtract markers, strokes, boxes, and cursor badge |

Line, rim, and inactive colors reuse `maskOverlayControlColor`,
`maskOverlayControlOutlineColor`, and `maskOverlayInactiveColor`.

### Color Grade node content

A Color Grade node shows only:

1. Its display name.
2. A `Masks` drawer header.
3. Its Mask source types when the drawer is open.

Do not show a topology number. The default name can contain a creation number,
such as `Color Grade 3`. Topology changes do not change this name.

Do not show node-kind text, a status dot, On/Off content, an adjustment summary,
a Mask count, or a persistent action row. Do not add a pill or badge. Do not use
an `xx · xx` compound label or an equivalent separator pattern.

The node uses `cardSurfaceColor` with a 1 px `graphNodeBorderColor` outline that
stays visible against the canvas. The Mask drawer sits in a sunken
`graphMaskDrawerSurfaceColor` well inset inside that outline, with rounded
bottom corners that follow the card radius, so the drawer reads as a contained
section of the card rather than a floating overlay. Selection keeps the same
surface and swaps the outline to the high-contrast
`graphSelectionOutlineColor`. Selection does not change the node size. It does
not add a label, status dot, glow, blue border, or blue fill.

Develop and DRT/Post use compact endpoint delegates. They show their fixed names
and real ports. They do not show a Mask drawer. Do not add a `Locked` badge.

### Mask drawer

Each Color Grade Mask drawer starts open. The user can open or close it from the
full `Masks` header row. A disclosure chevron shows the fold direction. It is
not a status indicator.

The header stays visible when the drawer is closed. It does not show a Mask
count. The body clips during the fold and uses `motionFoldOpenMs`,
`motionFoldCloseMs`, and `motionEasing`. `reduceMotion` sets the duration to
zero.

Header hover and keyboard focus paint the `hoverColor` wash inside the well
only: the wash is inset by `graphSelectionOutlineWidth` on every side so the
card outline stays visible, and when the drawer is fully closed (the header
then spans the whole drawer) its bottom corners follow the well radius instead
of painting square corners over the card.

The open state is UI layout state. A drawer change does not modify the pipeline,
create history, or start photo rendering. An empty open drawer has no Mask rows.

Each Mask row is flat. It shows the approved source-type icon, localized type
name, and a compact per-row delete `IconActionButton`. Do not show the Mask
name, opacity, enabled value, invert value, ranges, or identity. Selection
uses stable NodeId/MaskId and loads the existing Mask into the temporary Masks
body and viewer without creating a mask or history. Bind highlight to session
selection with existing monochrome selection tokens and preserve scroll. Do
not rebuild the list on click. Delete must not propagate into row selection or
graph Grade deletion. Parameter editors stay in the Masks body.

| Model kind | UI label | Icon |
| --- | --- | --- |
| `MaskSourceKind::LinearGradient` | `Gradient` | `mask_icons/gradient.svg` |
| `MaskSourceKind::Radial` | `Radial` | `mask_icons/radial.svg` |
| `MaskSourceKind::AiSegmentation` | `AI` | none until the AI Mask tool (SM9) adds its icon |

Use these approved paths:

```svg
<!-- Gradient: Tabler wash-dry -->
<path d="M3 6a3 3 0 0 1 3 -3h12a3 3 0 0 1 3 3v12a3 3 0 0 1 -3 3h-12a3 3 0 0 1 -3 -3v-12" />

<!-- Radial: Tabler wash-dryclean -->
<path d="M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0" />
```

Normalize both files to the shared 24×24 viewBox and white source stroke.
Keep the user-approved 2 px stroke width. Use one optical size and one source
size for the group. The Radial circle is an approved type icon. Do not reuse it
as a status dot.

### Ports and variable height

Use one small square input port at the top and one small square output port at
the bottom of a Color Grade. The visible square has a larger input area. A port
is a connection control, not a status dot.

The output port follows the bottom of the node when the Mask drawer changes
height. Bound edges must update to the new port position. The user cannot resize
the node directly.

---

## Icon and action geometry

Three independent sizes — never inherit only the SVG viewBox:

| Token | px | Meaning |
| --- | --- | --- |
| `iconButtonHitSize` | 44 | Default square hit target |
| `iconButtonHitSizeCompact` | 40 | Dense segmented rows only |
| `iconOpticalSize` | 22 | Drawn icon size (normal) |
| `iconOpticalSizeCompact` | 18 | Drawn icon size (rails and dense navigation) |
| `iconSourceSize` | 24 | `Image.sourceSize` / raster request (normal) |
| `iconSourceSizeCompact` | 20 | Raster request (compact) |

**Hit band:** structural SVG actions use **40–44 px** hit targets. The painted
well is inset by 4 px on every side (36 px normal, 32 px compact), so pointer
comfort never dictates visual mass. History/Versions and the five-item
adjustment navbar use compact 40/32/18 hit/chrome/optical geometry. The
Library/Editor switch keeps a 40 px row hit area around a 32 px-high track and
uses compact 18 px SVGs.

**Reference:** `DialogActionButton.qml` (text actions, height 46) and
`IconActionButton.qml` (SVG structural actions).

**DPR:** optical size stays logical pixels; Qt scales the scene. Source size is
kept ≥ optical so 1.0 / 1.5 / 2.0 remain sharp. Verify with the Phase 4C icon
geometry tests (token equality + optional grab fixtures).

**Stroke weight:** editor and workspace structural SVGs use `stroke-width="1.5"`
on a 24×24 viewBox. At the compact 18 px optical size this resolves to roughly
1.125 logical pixels before antialiasing, keeping dense navigation crisp rather
than visually bold. Do not mix the upstream Tabler 2 px default with locally
normalized icons in the same navigation group. The user-approved Nodes pipeline
icon, Mask Groups layer icon, Gradient and Radial Mask icons, and the Album Inspector sort and
Group icons are documented 2 px exceptions.

**Every SVG action must:**

- Be keyboard reachable (`activeFocusOnTab` or equivalent)
- Expose a localized tooltip and `Accessible.name`
- Show a recognizable disabled opacity/tint
- Use the shared hit/optical/source tokens (or a documented exception)

**Tabler icons:** the official [Tabler](https://tabler.io/icons) icon set (MIT
license) is the preferred source for structural SVG actions. The Versions rail
button uses `panel_icons/versions.svg` (Tabler `versions`). A custom-drawn
icon requires a documented reason that no suitable Tabler symbol exists.
Non-Tabler assets are preserved for established Alcedo-specific actions
(adjustments, aperture, etc.) and for the history operation set.

---

## Borders and focus

- Default chrome borders: 1 px `cardBorderColor` / `dividerColor`.
- The existing structural icon action focus treatment is a 1 px accent at
  ~60% alpha via `IconActionButton.showFocusRing` (default true). This is a
  narrow keyboard-focus exception on the focused action, not a selection
  style. Do not copy it onto thumbnails, nodes, groups, cards, or panels, and
  do not retain it as a pointer-selected state. Prefer a documented neutral
  focus treatment for new surfaces while keeping keyboard focus visible.
- **Collections sidebar toggle exception:** this is an immediate toolbar action,
  not a selected mode. Pointer activation does not retain focus and it draws no
  accent focus border. Hover, press, and keyboard focus use the existing quiet
  `buttonHoveredFillColor` gray well; keyboard access remains available through
  `activeFocusOnTab`.
- **Library/Editor capsule exception:** segments draw **no** hover fill, press
  fill, or focus ring. The sliding `workspaceSwitchThumb` is the **only**
  selected-workspace indication. Hover still drives tooltips.
  **Monochrome VI:** track = `bgBaseColor`; thumb = `editorListSelectedFillColor`
  (light bone well); active segment icon ink = `editorListSelectedInkColor`;
  idle icons = `iconColor` / hover `textColor`. Do **not** paint the thumb with
  `accentColor` (no blue slab).
- **Adjustment navbar sliding window:** Tone / Look / LUT / Display /
  Geometry / RAW Decode stay on one stable track. The selected node does not
  hide pages. Write targeting still rejects fields the current node does not own.
  Uses the **same monochrome thumb** (`editorAdjustmentNavThumb` =
  `editorListSelectedFillColor`, no accent border). Active segment icon ink =
  `editorListSelectedInkColor`; idle = `textMutedColor`. Segment buttons:
  transparent wells, **no** hover fill, **no** focus ring (capsule family).
  Motion: OutBack slide on `x` + land scale pulse; `reduceMotion` snaps.
  Thumb size is inset from the hit cell (`spaceXs`) so the well is not flush
  with the sunken track edge.
- **Display method segments:** shared sunk track + monochrome inverted wells
  (see Display Transform panel); title-only, medium height, always expanded.

### Adjustment header (node name, EXIF, and Mask tools)

Place the header between the scope slot and the adjustment navbar. Surfaces use
`cardSurfaceColor` with `dividerColor` hairlines. Do not add a second panel fill,
badge, or status dot.

The header is one vertical stack. Both rows fill the same header width:

1. Top: four EXIF tokens in one `RowLayout`. Order is focal length, aperture,
   shutter, ISO. Each cell uses `Layout.fillWidth` with `preferredWidth` 0 so
   the four cells share the row equally and span the same width as the name and
   Mask-tool row. `monoFontFamily`, `fontSizeCaption`, `fontWeightRegular`,
   `lineHeightCaption`. Single line per cell, elided. Present tokens look like
   `100mm`, `f2.8`, `1/500s`, `ISO 100`. Missing or invalid values keep their cell
   and show an em dash. Focal length is actual mm, never 35 mm equivalent. Each
   token is its own label; do not join them with a centered dot or other
   compound separator.
2. Bottom row, horizontal:
   - Left: selected node display name only. `uiFontFamily`, `fontSizeTitle`,
     `fontWeightStrong`, `lineHeightTitle`. Vertically centered. Up to two lines,
     elided. Full name is the accessible name and tooltip. No node-kind subtitle.
     Do not use `monoFontFamily`.
   - Right: Radial, Gradient, then AI Mask `IconActionButton` compact actions
     using `qrc:/mask_icons/radial-add.svg`, `qrc:/mask_icons/gradient-add.svg`,
     and `qrc:/panel_icons/pencil-ai.svg` (user-provided Tabler `pencil-ai`).
     Idle fill is `buttonIdleFillColor`. Do not insert a vertical rule between
     the name and the tools.

Node switching does not reread EXIF. Image identity change updates the four EXIF
cells.

---

## Component states

| State | Treatment |
| --- | --- |
| Empty | Muted body copy; card surface unchanged |
| Loading | Muted status label (e.g. viewport “Preparing…”) |
| Error | Warning and error messages use `AlertBadge.qml` (white text on `alertBadgeFillColor`), never red text; other error emphasis uses `dangerColor` / `dangerTintColor` — no ad-hoc reds |
| Disabled | Muted icon/text tint + `enabled: false`; editor shells keep card surface (no parent opacity, no second shell tone) |
| Selected | Dense rows + segmented controls: `editorListSelectedFillColor` well + `editorListSelectedInkColor` ink. Icon actions outside capsules may use neutral `buttonSelectedFillColor` (fill only). Nodes and established outline-based surfaces use the text-color outline. New or changed Library selection follows the same monochrome policy. No blue frame, tint, slab, glow, or side stripe. |
| Hover | Quiet `buttonHoveredFillColor` well unless capsule exception applies (Library/Editor and adjustment nav segments: tooltip only) |

---

## Empty / product copy

Visible strings are product language only. Ban developer placeholders such as
“will appear here”, “TODO”, or phase names.

| Surface | Copy |
| --- | --- |
| Empty editor title | “Select an image to edit” |
| Empty editor body | “Open an image from the library, or keep this workspace ready for search results.” |
| Filmstrip empty | “No images” |
| History empty | “No edit history yet” |
| Versions empty | “No versions yet” |
| Nodes empty | “Select an image to edit nodes” |
| Nodes loading | “Loading node graph” |
| Nodes pending command | “Updating node graph” |
| Adjustment empty (no image) | “Select an image to enable adjustments” |
| Adjustment empty (has image) | “No adjustments yet” |
| LUT browser, empty library | “No LUTs in the library” / “Import .cube files, or copy them into the LUT folder and refresh.” |
| LUT browser, no results | “No LUTs match” / “Change the search or the filters to see more LUTs.” |
| LUT browser, loading | “Loading LUT library” |
| Comparison, rendering | “Rendering comparison images” |
| Comparison, image load failure | “The comparison images could not be loaded.” |
| Comparison, HDR output | “Comparison is unavailable for HDR output.” |
| Comparison, entry action (viewport and Versions header) | “Compare” |
| Comparison, Root source | “Before (imported)” |
| Comparison, current source | “Current working state” |
| Comparison, active Version source | “<Version name> (working state)” |

---

## Motion

Quiet desktop language. No bounce, overshoot, perpetual animation, or input
blocking. Session identity is never recreated by a fold. The one approved
perpetual animations are the `SkeletonBlock` content placeholder and the
`StaggeredGridLoader` Library loading grid (table below).

| Token | Value | Use |
| --- | --- | --- |
| `motionFoldOpenMs` | 200 | Opening fold (emphasized); also capsule thumb slide floor |
| `motionFoldCloseMs` | 160 | Closing fold (slightly faster) |
| `motionFadeMs` | 120 | Short fades; project-load overlay hold before fade-out |
| `motionEasing` | `QEasingCurve::OutCubic` (QML `Easing.OutCubic`) | Fold and fade easing. Bind `easing.type: appTheme.motionEasing` |
| `backgroundTaskAutoCollapseMs` | 3000 | Time the task summary remains expanded after a task state changes |
| `skeletonCycleMs` | 1200 | One sweep of the `SkeletonBlock` highlight band (`Easing.InOutSine`) |
| `reduceMotion` | `QSettings("ui/reduceMotion")` | When true, all fold/fade/slide durations resolve to **0**; final state unchanged |

**Monochrome selection motion:**

| Surface | Motion | Notes |
| --- | --- | --- |
| Workspace + adjustment thumbs | Slide on `x` (OutBack, land scale pulse) | Documented capsule exception to “no overshoot” for mechanical feel |
| Display method segments | Instant fill swap (optional future fade) | Title-only wells inside shared track |
| Content placeholder (`SkeletonBlock`) | A `hoverColor` band moves on `x` from left to right inside the clipped `bgBaseColor` block, `skeletonCycleMs` per sweep, `Easing.InOutSine`, repeated while the content loads | Approved exception to "no perpetual animation", for content placeholders only. The band stops when the block is invisible. Under `reduceMotion` the band does not run and the block is static |
| Library loading grid (`StaggeredGridLoader`) | A `textColor` wave runs along the tile diagonals, top left to bottom right. Placeholder grid: one GUI-thread `NumberAnimation` drives every tile, opacity wave, 1600 ms per pass. Compact mark (`StaggeredGridMark`): one shader quad per mark on the shared `loadingWavePhase` clock (`loadingWaveFrameMs` ticks), opacity and scale wave, `loadingWaveCycleMs` per pass | Approved exception to "no perpetual animation", for Library loading and photo loading marks only (Spacing "Library loading"). The wave stops when the loader is invisible. Under `reduceMotion` every tile is static at its rest level |
| Project loading overlay | Snap on immediately; after load, hold `motionFadeMs` then fade out `motionFoldCloseMs` | No fade-in — that flashed the empty library after Welcome closed |
| Library first reveal | Grid Loader fades in `motionFoldOpenMs` with `spaceMd` translateY | Prepared hidden while the overlay is up; plays as the overlay starts to fade; skipped under `reduceMotion` |
| Window maximize / restore / minimize | Native `QWindow` state transition (`showMaximized`, `showNormal`, `showMinimized`) | Windows keeps the standard resizable HWND styles and extends the client area through `WindowsFramelessWindow`. macOS keeps the system traffic lights over the leading side of the full-width toolbar and hides the title-bar surface with `Qt.ExpandedClientAreaHint` + `Qt.NoTitleBarBackgroundHint`; toolbar content reserves that leading region. Other platforms use Qt frameless behavior plus drawn caption buttons. The platform owns animation and geometry; QML never fades, snapshots, or interpolates the top-level window |

**Fold rules (History/Versions/Nodes, adjustment stack, filmstrip, collapsible section, Album Inspector sections):**

1. Logical expanded/collapsed (or session page) flips immediately.
2. Persistent rail / handle / section header stays stationary.
3. Intermediate content is clipped (`clip: true`). The folding body keeps its
   expanded size and is revealed by the host size; it does not slide via `x`/`y`.
4. Host layout size interpolates with progress so the center viewport grows and
   shrinks with the panel (same mode as the filmstrip dock).
5. Body opacity tracks the same progress. Duration is `motionFoldOpenMs` on open
   and `motionFoldCloseMs` on close. Easing is `motionEasing`.
6. `reduceMotion` snaps progress to the terminal value.
7. Tests may call `driveFoldProgress(t)` / `endFoldDrive()` to pin intermediate
   progress without wall-clock sleeps. Hosts expose:
   - History/Versions: `panelOpenProgress`, `totalWidth`, `layoutExpanded`,
     `driveFoldProgress`, `endFoldDrive`
   - Adjustment stack: `stackExpandProgress`, `driveFoldProgress`, `endFoldDrive`
   - Filmstrip: `dockExpandProgress`, `driveFoldProgress`, `endFoldDrive`
   - `CollapsibleSection`: `foldProgress`, `driveFoldProgress`, `endFoldDrive`

**History/Versions rail:**

1. `totalWidth` interpolates: `railWidth + (panelGap + expandedPanelWidth) *
   panelOpenProgress`. The editor row reflows every frame so the viewport moves
   with the panel.
2. The icon rail stays put. The panel host width and following gap track
   progress; the full-width panel body is clipped (no `x` slide).
3. Only the active page body is loaded (`Loader`) while progress is above zero.
   A closing fold keeps the last body until progress reaches 0. A fully closed
   rail owns no transaction or Version list delegates. Scroll offsets live on
   the rail and restore on reactivation.
4. Adjustment stack uses the same progress + clip + layout-width fold toward
   the window edge.

---

## Context menus

All popup menus (Library grid right-click, editor filmstrip right-click,
toolbar dropdowns) share one dark menu family — no per-site ad-hoc styling.

| Role | Token |
| --- | --- |
| Menu surface | `bgBaseColor` (same dark well as editor dropdown popups) |
| Menu outline | 1 px `dividerColor` hairline |
| Menu shell radius | `panelRadius` |
| Row hover / keyboard highlight | `hoverColor` wash, `controlRadiusSmall` radius |
| Row text | `textColor`; disabled rows mute to `textMutedColor` |
| Separator | 1 px `dividerColor`, `spaceXs` vertical air |
| Menu padding | `spaceXs` (highlight wash stays inset from the shell edge) |
| Row metrics | `spaceSm` horizontal / `spaceXs` vertical padding, `fontSizeBody` |
| State gutter | Reserved `spaceLg` left column for check marks so rows share one text edge |
| Open / close | `motionFadeMs` opacity fade, skipped under `reduceMotion` |

Use `AppContextMenu` + `AppMenuItem` + `AppMenuSeparator` for every new menu;
sub-menus declared as nested `AppContextMenu` with a `title` pick up the same
row delegate and arrow affordance automatically.

A sub-menu that lists project data of unbounded length (the image menu "Add to"
collection list) keeps its fixed actions as `AppMenuItem` rows at the top, then a
separator, then one bounded list: at most `collectionMenuMaxRows` rows of
`collectionRowHeight`, scrolling inside the menu, with a 1 px-outlined filter
field above it from `collectionMenuFilterThreshold` entries. List rows use the
menu row text edge, the `hoverColor` wash, and `textMutedColor` for rows that
are not a valid target.

---

## Settings bento pages

Settings > LUTs, Settings > AI Models, Settings > Local Content Recognition, and Settings > About
use `BentoTile` surfaces with the page margins 26 px (top) and 34 px (sides) and `spaceMd`
between tiles.

**Settings type scale** (#824). Every `Text` and `Label` of `SettingDialog.qml` and the
Settings pages uses an `appTheme` size token (11, 12, 13, 14, 22, and `fontSizeDisplay` 72 for
the count tile) and a weight of 600 or less; `SettingsAboutQmlTest` checks every page.

| Text | Size | Weight |
| --- | --- | --- |
| Dialog title ("Setting") and page title | `fontSizeHeadline` (Manrope) | `fontWeightStrong` |
| Section and tile titles | `fontSizeSection` | `fontWeightStrong` |
| Navigation item | `fontSizeTitle` | `fontWeightRegular`; selected `fontWeightStrong` |
| Row name | `fontSizeTitle` | `fontWeightStrong` |
| Values, descriptions, lists | `fontSizeBody` | `fontWeightRegular` |
| Buttons (`DialogActionButton`) | `fontSizeSection` | `fontWeightStrong` |

The dialog chrome is neutral: the selected navigation item is a `textColor` wash at 0.08 (rows
36 px, icons 16 px), the header is 72 px, and Done is the light `DialogActionButton` (34 px) in a
64 px footer. The Alcedo Studio wordmark on About is the one exception to the weight limit: it
is the wordmark of the welcome dialog (`headlineFontFamily`, `fontSizeHeadline`,
`fontWeightHeading`, "Alcedo" in `accentColor`).

**About** (`AboutPage.qml`): row 1 is the wordmark tile (copyright and license as muted name and
value rows; Sponsor, GitHub, and Docs with `heart.svg`, `github.svg`, and `book.svg`, Docs as the
light button) beside a `CountTile` with the version and the build. Row 2 holds the rows Logs
(Open Log Folder, the only log control of Settings), AI model download statement (View), and
Third-party licenses (View all opens a two-column list in the tile). Row 3 lists the
acknowledgements and components of README.md "Acknowledgments": the subject on one line in
`fontSizeBody` (236 px column) and its source links in muted Manrope `fontSizeBody` with
`↗`, then the trademark notice. `SettingsAboutQmlTest` fails when a README link is missing.

- **Count tile** (`CountTile.qml`, 190 px wide): the name in muted Manrope `fontSizeSection`
  Medium, the count in Manrope `fontSizeDisplay` (72) ExtraLight with tabular figures, centered.
  `valueText` shows a text in place of the count and `detailText` a line below it (About:
  the version and the build). A page that can
  hold 10,000 or more items sets `compactLargeCounts`: the count then shows at most three
  significant digits, truncated (12.8K, 102K, 1.25M; in Chinese 1.28万, 10.2万, 1.23亿), and the
  exact grouped count stands below it in muted Manrope `fontSizeBody`.
- **Split bar and legend** (`SplitBar.qml`, `LegendItem.qml`): a 10 px track in
  `bgBaseColor`, the first part from the left, a 3 px gap, the second part at the right. The
  legend swatch (8 px) carries the color of its bar part; it names a color, it is not a status
  dot. The LUT categories keep `accentColor` and `toneMist` at 0.7. The label coverage of
  Local Content Recognition is neutral: `textColor` for photos with labels and
  `textMutedColor` at 0.45 for photos that need labels.
- **Task progress in a tile**: `ThemedProgressBar` with `showDetails`; `fillColor` is
  `textColor` on neutral pages. The detail text is percent, `completed / total` (grouped), and
  the remaining time from `DataFormat.remaining` ("About 3 min remaining", "About 2 h 10 min
  remaining", "Less than a minute remaining", "Estimating the remaining time"). The running
  body takes the place of the idle body at the same height, so the tile does not resize.
- **Neutral actions**: Local Content Recognition and the AI label dialog use no theme blue. The
  main action is `DialogActionButton` `kind: "light"` (`textColor` fill, `bgCanvasColor`
  text); the other actions are `kind: "normal"`. "Move task to background" is an
  `IconButton` with `to_bg.svg` in the normal control colors (`controlFillColor`,
  `controlBorderColor`). Buttons are 34 px high in tiles and in this dialog.
- **Settings rows** in a tile: the name in `uiFontFamily` `fontSizeTitle` `fontWeightStrong` at
  the left; the value and the control at the right (combos 220 px wide); 1 px `dividerColor`
  lines between the rows. No explanatory sentence under a row or in a tile: the name, the value,
  and the state words carry the meaning. Background information goes into the page info badge.
- Local Content Recognition fits the dialog at its full size (980 x 620) without scrolling.

## Shared components

| Component | Responsibility |
| --- | --- |
| `IconActionButton.qml` | Structural SVG action (Item root, true square chrome): hit/optical/source, tooltip, a11y, hover, focus, selected fill; SVG tint via `ColorImage` (same as `Button.icon.color`) |
| `CollapsibleSection.qml` | Folding group shell with shared motion driver |
| `DialogActionButton.qml` | Text dialog actions (46 px floor; wrapped labels grow the button; `fontWeightStrong`); kinds `normal`, `light` (main action of a neutral surface), `accent`, `danger`, `warning`; optional `iconSource` (16 px monochrome SVG before the text, tinted with the text color) |
| `IconButton.qml` | Legacy square icon control; defaults now follow tokens |
| `SegmentedCardSwitcher.qml` | Monochrome segmented track; per-entry `enabled: false` keeps unsupported choices visible but non-interactive |
| `AdjustmentSlider.qml` | Shared numeric slider + field bound to `EditorAdjustmentValueModel` |
| `ThemedProgressBar.qml` | Shared determinate / indeterminate progress track (Background Tasks, Export, Settings tiles); detail text in Manrope with tabular figures |
| `BentoTile.qml` | Settings bento surface (radius 14, `cardSurfaceColor`, `cardBorderColor`, 22 px padding) |
| `CountTile.qml` | Bento tile with one large count; optional short form for large counts with the exact count below |
| `SplitBar.qml` / `LegendItem.qml` | Two-part count bar and its legend entries (swatch, name, grouped count) |
| `DataFormat.js` | Grouped counts, short large counts (K/M, 万/亿), and remaining-time text |
| `ThemeCheckBox.qml` | Monochrome check row (Adjustment Transfer / Export): selected well + check, muted label when off; disabled: hairline box, muted mark and label |
| `ThemeSwitch.qml` | Monochrome switch row (search settings): label + track; on = selected well + ink knob, off = sunken track + muted knob |
| `AppContextMenu.qml` | Shared dark popup menu shell (`Menu` + `AppMenuItem` delegate, fade transition, `openAt`) |
| `AppMenuItem.qml` | Shared dark menu row (state gutter, wrapping label, sub-menu arrow, hover wash) |
| `AppMenuSeparator.qml` | Shared 1 px menu group divider |
| `AlertBadge.qml` | Warning and error message: wrapped white caption text on an `alertBadgeFillColor` badge; hidden while empty |
| `SkeletonBlock.qml` | Content placeholder: `bgBaseColor` block with the moving `hoverColor` band (Motion); `animated: false` draws a static tile |
| `StaggeredGridLoader.qml` | Library loading: a grid of rounded tiles with a diagonal wave (Motion, "Library loading"); `rows: 0` fills the height as a placeholder grid, a fixed 3 x 3 grid with `scaleDepth` is the compact mark |
| `StaggeredGridMark.qml` | The compact 3 x 3 staggered grid as one shader quad on the shared `appTheme.loadingWavePhase` clock: the loading mark of a photo (Library card, justified tile, Editor filmstrip card) and of the Library over a kept result |

---

## Compact exceptions (documented)

| Location | Exception | Why |
| --- | --- | --- |
| History / Versions rail | compact 40 px hit, 32 px well, 18 px SVG | Quiet tools inside a 48 px persistent rail |
| Mask Group drag reorder | whole card lifts under the header name area (dims to 94%), 2 px accent hairline marks the insertion slot; no reorder buttons on the row | Reordering is a drag-and-drop interaction; Ctrl+Up/Ctrl+Down on the focused header covers keyboard and accessibility, so no compact hit areas are needed |
| Window caption buttons | custom canvas 16 px glyphs on Windows/Linux; hidden on macOS | OS-chrome parity, not content SVG set. macOS uses the system traffic lights in the expanded client area |

---

## Manual visual review matrix

Inspect these surfaces for empty, selected, collapsed, expanded, hover, and
disabled states where applicable:

- Library/Editor capsule + thumb
- Library thumbnail cards
- History/Versions rail + panel
- Adjustment shell + group fold
- Editor viewport empty state
- Filmstrip dock + handle
- Nodes and Mask Groups, including a selected Mask inside its owner group

For new or changed selection, verify both themes: no blue picture-frame
decoration or generic accent selection; selected text/icons remain legible;
hover, selected state, and keyboard focus remain distinguishable. Check
framework-provided selection items as well as Alcedo-painted chrome. Existing
accent tokens do not count as an exception to these acceptance checks.

DPR coverage: inspect icon optical/source sizing at logical 1.0, 1.5, and 2.0.
