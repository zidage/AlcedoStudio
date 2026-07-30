# Alcedo Studio QML Visual Identity (VI)

**Read this file before changing or adding any QML visual.** Feature QML must
consume named `appTheme` properties or shared components listed below. Do not
introduce unexplained pixel, color, radius, or duration literals.

Owner: `alcedo_studio/src/ui/alcedo_main`  
Implementation source of truth: `AppTheme` (`app_theme.hpp` / `app_theme.cpp`)  
Shared components: `IconActionButton.qml`, `CollapsibleSection.qml`,
`DialogActionButton.qml`, `IconButton.qml`

This document freezes the visual system for the unified QML workspace (Phase 4C).
Phase 5–6 must consume these tokens; they must not invent a parallel palette.

---

## Agent rule

1. Open this file.
2. Map the change to a token or shared component in the tables below.
3. If a new value is required, add it to `AppTheme` **and** this document in the
   same change. Never publish a value here that differs from code.
4. Prefer shared components over copy-pasted Button/Rectangle chrome.
5. Run the Phase 4C visual/motion tests after structural QML changes.

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
| Data / numeric | `dataFontFamily` | body/caption | regular/strong | matching | Tabular metrics, zoom, crop degrees (IBM Plex Sans) |
| Mono (minigit) | `monoFontFamily` | body/caption | regular | matching | **Only** minigit history/Versions data: commit hashes, before/after delta lines. Family is **DM Mono** (`data_DMMono.ttf`). Do **not** use for general metrics, filmstrip counts, zoom, or crop degrees — those stay on `dataFontFamily`. |

Families resolve at runtime from registered Alcedo fonts (`AppTheme::RegisterFonts`).
Do not hardcode Inter, Roboto, Arial, or system UI fonts in feature QML.

### Minigit typography

The editor history / Versions rail (mini-Git) is the only product surface that
uses monospace:

| Surface | Token | What |
| --- | --- | --- |
| Version card commit line | `monoFontFamily` + `fontSizeCaption` | `Commit <8-hex>` or `Commit image root` |
| Transaction card hash | `monoFontFamily` + `fontSizeCaption` | `Commit <8-hex>` per row |
| Transaction before/after | `monoFontFamily` + `fontSizeBody` | Delta value line (`0 → +0.35`) |

Version and transaction **titles**, times, and section chrome stay on
`uiFontFamily`. Active Version is outline-only (1 px text-color border); no
`CURRENT HEAD` pill and no separate "Checked out" / dual Head+Commit labels.

---

## Color and surface hierarchy

Semantic colors follow the active theme (`currentThemeIndex` 0 Alcedo / 1 Classic)
and notify via `ThemeChanged`.

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
| Accent | `accentColor` (`toneGold`) | Workspace thumb, primary accent |
| Accent secondary | `accentSecondaryColor` | Thumb border, Material primary |
| Danger | `dangerColor` (`toneWine`) | Destructive emphasis |
| Danger tint | `dangerTintColor` | Soft danger wells |
| Selected tint | `selectedTintColor` | Library selected card wash |
| Hover | `hoverColor` | Quiet hover wash |
| Divider | `dividerColor` | Hairlines, card borders |
| Glass panel | `glassPanelColor` | Translucent shells when needed |
| Glass stroke | `glassStrokeColor` | Glass edges |
| Overlay | `overlayColor` | Modal scrims |
| **Button idle fill** | **`buttonIdleFillColor`** | **Icon button idle background** (= card surface, opaque) |
| **Button hovered fill** | **`buttonHoveredFillColor`** | **Opaque hover-state fill** (between panel and engaged) |
| **Button pressed fill** | **`buttonPressedFillColor`** | **Opaque pressed-state fill** (= hover well) |
| **Button selected fill** | **`buttonSelectedFillColor`** | **Opaque selected-state fill** (= pressed / hover well) |
| **Disabled surface** | **`disabledSurfaceColor`** | **Reserved muted shell token** (not used for editor side-panel shells) |
| **Merge current** | **`mergeCurrentColor` / `mergeCurrentFillColor`** | **Git red for Current labels, value ink, borders, and selected wells** |
| **Merge incoming** | **`mergeIncomingColor` / `mergeIncomingFillColor`** | **Git green for Incoming labels, value ink, borders, and selected wells** |
| **List selected fill** | **`editorListSelectedFillColor`** | **Monochrome light well for dense catalog rows** (LUT browser, inverted selection) |
| **List selected ink** | **`editorListSelectedInkColor`** | **Text / icon ink on the light selected well** (= `bgBaseColor`) |
| **List favorite idle** | **`editorListFavoriteIdleColor`** | **Unstarred glyph on sunken (dark) rows** |
| **List favorite active** | **`editorListFavoriteActiveColor`** | **Starred glyph on sunken rows** (`accentColor` / toneGold) |
| **List favorite idle on selected** | **`editorListFavoriteIdleOnSelectedColor`** | **Unstarred glyph inverted on the light well** |
| **List favorite active on selected** | **`editorListFavoriteActiveOnSelectedColor`** | **Starred glyph inverted on the light well** (full ink) |

**Monochrome inverted list selection:** dense catalogs (LUT panel first) keep a
black-and-white row language — sunken `bgBaseColor` track, light
`editorListSelectedFillColor` bar, `editorListSelectedInkColor` for title and
secondary copy. Favorite stars **invert with the row**: muted light idle + gold
active on dark rows; muted ink idle + full ink active on the selected light
well. Type badges use a white chip (`editorSliderHandleColor`) on dark rows and
invert to ink-on-bone when the row is selected. Do not reintroduce ad-hoc
`#D8D4CD` / `Qt.rgba` star or badge colors in feature QML.

**History/Versions outline selection:** the transaction timeline, named Version
cards, and their persistent rail buttons keep `cardSurfaceColor` and use the
primary text token as a quiet 1 px outline for the active item. They do not use
the filled `editorListSelectedFillColor` well; that filled selection remains
reserved for dense catalog rows such as LUT and is unchanged by the
History/Versions refactor.

**List well inset (required):** the light selected bar must **not** flush the
sunken track border. Use `spaceXs` as list track padding (`ListView` margins)
and as inter-row gap (`ListView.spacing`) so track color frames every well.

**LUT selection chrome (required):** one sliding `editorLutSelectionChrome`
rectangle parented to `ListView.contentItem` paints the light well. Do **not**
per-row `opacity` fills — recycled delegates start already selected so
`Behavior on opacity` never runs. Nearby selection moves animate `y`
(`motionFoldOpenMs`, `OutCubic`); long jumps (distance > viewport height) snap
`y` and fade opacity in. Snapshot echo must **not** call catalog `refresh`
when the path is already in the model (that reassigns the list and hitch
`contentY`). Hover wells use the same inset geometry.

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

---

## Radii

| Token | px | Use |
| --- | --- | --- |
| `panelRadius` | theme (10 Alcedo / 8 Classic) | Editor cards, filmstrip, rail shells |
| `controlRadius` | 10 | Icon actions, nav pills |
| `controlRadiusSmall` | 8 | Inner wells, collapsible section |
| `badgeRadius` | 6 | Crop angle badge, small chips |

---

## Editor panel geometry

Side-panel and scope sizing for the editor desktop. Values are logical px; Qt
scales by DPR so they stay comfortable at 1.0 / 1.25 / 1.5 / 2.0. The preferred
width unifies the adjustment stack and the History/Versions expanded panel so
the two side columns read as one family.

| Token | px | Use |
| --- | --- | --- |
| `editorSidePanelWidth` | 320 | Preferred width: adjustment stack + History/Versions expanded panel |
| `editorSidePanelWidthMin` | 260 | Adjustment stack minimum (narrow-window floor) |
| `editorSidePanelWidthMax` | 460 | Adjustment stack maximum |
| `editorMergeDialogWidth` | 960 | Merge conflict resolution dialog — top action bar + three-column Current / Incoming / Merged cards |
| `editorScopeHeight` | 192 | Histogram / waveform slot preferred height |
| `editorScopeHeightMin` | 160 | Histogram / waveform slot minimum height |

**Merge dialog layout:** `EditorMergeDialog` is centered on `Overlay.overlay`
with the shared MultiEffect blur + `overlayColor` dim used by other modal
dialogs. Header is one row: `Merge Conflicts` title with Cancel / Complete on
the same vertical center. Conflict rows scroll in the middle; sticky
Use All Current / Incoming sit below the list, horizontally centered. Conflict
rows have no outer card chrome — only the three comparison panes are cards
(`bgBaseColor`, quiet border; selected sides use merge-tint borders). Complete
uses the monochrome selected fill.

The History/Versions rail width (60 px) and rail-button hit (46 px) stay under
Icon and action geometry; the rail width is not tokenized because it is locked to
the rail-button optical balance.

### Scope plot colors

Scope plot colors are semantic AppTheme tokens so the scene-graph item does not
carry palette literals into QML. They are used by both histogram lines and the
QPainter-backed waveform density image.

| Token | Use |
| --- | --- |
| `scopeGridColor` | Plot grid lines |
| `scopePlotBorderColor` | Horizontal plot rules |
| `scopeHistogramRedColor` | Red channel |
| `scopeHistogramGreenColor` | Green channel |
| `scopeHistogramBlueColor` | Blue channel |

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
normalized icons in the same navigation group.

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
- Focus rings on structural icon actions: 1 px accent at ~60% alpha via
  `IconActionButton.showFocusRing` (default true).
- **Library/Editor capsule exception:** segments draw **no** hover fill, press
  fill, or focus ring. The sliding `workspaceSwitchThumb` is the **only**
  selected-workspace indication. Hover still drives tooltips.
  **Monochrome VI:** track = `bgBaseColor`; thumb = `editorListSelectedFillColor`
  (light bone well); active segment icon ink = `editorListSelectedInkColor`;
  idle icons = `iconColor` / hover `textColor`. Do **not** paint the thumb with
  `accentColor` (no blue slab).
- **Adjustment navbar sliding window:** Tone/Look/LUT/Display/Geometry/RAW track
  uses the **same monochrome thumb** (`editorAdjustmentNavThumb` =
  `editorListSelectedFillColor`, no accent border). Active segment icon ink =
  `editorListSelectedInkColor`; idle = `textMutedColor`. Segment buttons:
  transparent wells, **no** hover fill, **no** focus ring (capsule family).
  Motion: OutBack slide on `x` + land scale pulse; `reduceMotion` snaps.
  Thumb size is inset from the hit cell (`spaceXs`) so the well is not flush
  with the sunken track edge.
- **Display method segments:** shared sunk track + monochrome inverted wells
  (see Display Transform panel); title-only, medium height, always expanded.

---

## Component states

| State | Treatment |
| --- | --- |
| Empty | Muted body copy; card surface unchanged |
| Loading | Muted status label (e.g. viewport “Preparing…”) |
| Error | `dangerColor` / `dangerTintColor` — no ad-hoc reds |
| Disabled | Muted icon/text tint + `enabled: false`; editor shells keep card surface (no parent opacity, no second shell tone) |
| Selected | Dense catalogs + segmented capsules: `editorListSelectedFillColor` well + `editorListSelectedInkColor` ink (B&W). Icon actions outside capsules may still use `buttonSelectedFillColor` (fill only). Library cards use `selectedTintColor`. Never use cool blue accent as a selected slab in editor chrome. |
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
| Adjustment empty (no image) | “Select an image to enable adjustments” |
| Adjustment empty (has image) | “No adjustments yet” |

---

## Motion

Quiet desktop language. No bounce, overshoot, perpetual animation, or input
blocking. Session identity is never recreated by a fold.

| Token | Value | Use |
| --- | --- | --- |
| `motionFoldOpenMs` | 200 | Opening fold (emphasized); also capsule thumb slide floor |
| `motionFoldCloseMs` | 160 | Closing fold (slightly faster) |
| `motionFadeMs` | 120 | Short fades — **LUT list selected well opacity** |
| Easing | `Easing.OutCubic` | Open/close and list selection fade |
| `reduceMotion` | `QSettings("ui/reduceMotion")` | When true, all fold/fade/slide durations resolve to **0**; final state unchanged |

**Monochrome selection motion:**

| Surface | Motion | Notes |
| --- | --- | --- |
| LUT list selected well | Single sliding chrome: nearby `y` slide (`motionFoldOpenMs`); long jump snaps + opacity fade-in | Never per-delegate opacity; never flush to track; no catalog refresh on same-path snapshot echo |
| Workspace + adjustment thumbs | Slide on `x` (OutBack, land scale pulse) | Documented capsule exception to “no overshoot” for mechanical feel |
| Display method segments | Instant fill swap (optional future fade) | Title-only wells inside shared track |

**Fold rules (History/Versions, filmstrip, collapsible adjustment section):**

1. Logical expanded/collapsed (or session page) flips immediately.
2. Persistent rail / handle / section header stays stationary.
3. Intermediate content is clipped (`clip: true`).
4. Opening uses `motionFoldOpenMs`; closing uses `motionFoldCloseMs`.
5. `reduceMotion` snaps progress to the terminal value.
6. Tests may call `driveFoldProgress(t)` / `endFoldDrive()` to pin intermediate
   progress without wall-clock sleeps. Hosts expose:
   - History/Versions: `panelOpenProgress`, `panelSlideX`, `layoutExpanded`,
     `driveFoldProgress`, `endFoldDrive`
   - Filmstrip: `dockExpandProgress`, `driveFoldProgress`, `endFoldDrive`
   - `CollapsibleSection`: `foldProgress`, `driveFoldProgress`, `endFoldDrive`

**History/Versions rail (Phase 7A R6 — transform-only reveal):**

1. Outer layout width is **binary** (rail-only vs rail + full panel). It snaps once
   when the panel becomes layout-expanded or fully closed — it does **not** track
   `panelOpenProgress` every animation frame (avoids workspace re-layout thrash).
2. `panelOpenProgress` drives **transform-only** inner motion (`x` slide via
   `panelSlideX`). No opacity animation on the history/Versions panel subtree.
3. Only the active page body is loaded (`Loader`). Closed rail owns no transaction
   or Version list delegates. Scroll offsets live on the rail and restore on
   reactivation.
4. Filmstrip and `CollapsibleSection` may still animate height + opacity together;
   that exception is limited to those hosts and is not used on the history rail.

---

## Shared components

| Component | Responsibility |
| --- | --- |
| `IconActionButton.qml` | Structural SVG action (Item root, true square chrome): hit/optical/source, tooltip, a11y, hover, focus, selected fill; SVG tint via `ColorImage` (same as `Button.icon.color`) |
| `CollapsibleSection.qml` | Folding group shell with shared motion driver |
| `DialogActionButton.qml` | Text dialog actions (height 46 reference) |
| `IconButton.qml` | Legacy square icon control; defaults now follow tokens |

---

## Compact exceptions (documented)

| Location | Exception | Why |
| --- | --- | --- |
| History / Versions rail | compact 40 px hit, 32 px well, 18 px SVG | Quiet tools inside a 48 px persistent rail |
| Window caption buttons | custom canvas 16 px glyphs | OS-chrome parity, not content SVG set |

---

## Screenshot / theme matrix (acceptance)

Capture or property-assert these surfaces for empty, selected, collapsed,
expanded, hover, and disabled where applicable:

- Library/Editor capsule + thumb
- Library thumbnail cards
- History/Versions rail + panel
- Adjustment shell + group fold
- Editor viewport empty state
- Filmstrip dock + handle

DPR coverage: icon optical/source token equality at logical 1.0; optional
window grabs under `tests/ui/fixtures/phase4c/` when
`ALCEDO_PHASE4C_WRITE_FIXTURES=1`.
