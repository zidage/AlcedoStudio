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
| Data / numeric | `dataFontFamily` | body/caption | regular/strong | matching | Tabular metrics, zoom, crop degrees |
| Mono | `monoFontFamily` | body/caption | regular | matching | Diagnostic / mono readouts only |

Families resolve at runtime from registered Alcedo fonts (`AppTheme::RegisterFonts`).
Do not hardcode Inter, Roboto, Arial, or system UI fonts in feature QML.

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
| `editorScopeHeight` | 160 | Histogram / waveform slot preferred height |
| `editorScopeHeightMin` | 128 | Histogram / waveform slot minimum height |

The History/Versions rail width (60 px) and rail-button hit (46 px) stay under
Icon and action geometry; the rail width is not tokenized because it is locked to
the rail-button optical balance.

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
- **Adjustment navbar sliding window:** the five-item Tone/Look/… track uses a
  sliding accent thumb (`editorAdjustmentNavThumb`) as the only selected surface
  (OutBack slide + land scale; `reduceMotion` snaps). Segment buttons keep
  track-matched fills and brighten the icon on the active item.

---

## Component states

| State | Treatment |
| --- | --- |
| Empty | Muted body copy; card surface unchanged |
| Loading | Muted status label (e.g. viewport “Preparing…”) |
| Error | `dangerColor` / `dangerTintColor` — no ad-hoc reds |
| Disabled | Muted icon/text tint + `enabled: false`; editor shells keep card surface (no parent opacity, no second shell tone) |
| Selected | `buttonSelectedFillColor` well on icon actions (fill only — SVG tint stays default); library uses `selectedTintColor` |
| Hover | Quiet `buttonHoveredFillColor` well unless capsule exception applies |

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
| `motionFoldOpenMs` | 200 | Opening fold (emphasized) |
| `motionFoldCloseMs` | 160 | Closing fold (slightly faster) |
| `motionFadeMs` | 120 | Short fades |
| Easing | `Easing.OutCubic` | Open and close |
| `reduceMotion` | `QSettings("ui/reduceMotion")` | When true, all fold/fade durations resolve to **0**; final state unchanged |

**Fold rules (History/Versions, filmstrip, collapsible adjustment section):**

1. Logical expanded/collapsed (or session page) flips immediately.
2. Visual `*Progress` (0→1) drives geometry **and** opacity together.
3. Persistent rail / handle / section header stays stationary.
4. Intermediate content is clipped (`clip: true`).
5. Opening uses `motionFoldOpenMs`; closing uses `motionFoldCloseMs`.
6. `reduceMotion` snaps progress to the terminal value.
7. Tests may call `driveFoldProgress(t)` / `endFoldDrive()` to pin intermediate
   geometry without wall-clock sleeps. Hosts expose:
   - History/Versions: `panelOpenProgress`, `driveFoldProgress`, `endFoldDrive`
   - Filmstrip: `dockExpandProgress`, `driveFoldProgress`, `endFoldDrive`
   - `CollapsibleSection`: `foldProgress`, `driveFoldProgress`, `endFoldDrive`

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
