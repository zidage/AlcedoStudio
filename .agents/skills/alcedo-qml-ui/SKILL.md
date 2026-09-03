---
name: alcedo-qml-ui
description: >
  Alcedo Studio QML UI implementation skill for the unified workspace and editor
  adjustment panels. Use when adding or editing QML under alcedo_main, AppTheme
  tokens, DESIGN.md VI, IconActionButton, adjustment panels (Tone/Look/LUT/…),
  snapshot load/restore, toolbar SVGs, or when the agent might reach for Material
  style, ad-hoc colors, or fragile selection bindings. Triggers: "QML panel",
  "LUT panel", "appTheme", "DESIGN.md", "VI", "adjustment stack", "Material",
  "selectedPath", "loadFromSnapshot", "/alcedo-qml-ui".
---

# Alcedo QML UI

Project-specific rules for Alcedo Studio’s Qt Quick UI. Distilled from LUT-panel
and adjustment-stack work. Prefer this skill over generic “Material” or
marketing UI skills when touching `alcedo_studio/src/ui/alcedo_main/`.

## Before any visual or panel change

1. Read **`alcedo_studio/src/ui/alcedo_main/DESIGN.md`** (VI specification).
2. Map every color, radius, space, type size, and icon size to an **`appTheme`**
   token or a listed shared component.
3. If a new value is required, add it to **`AppTheme`** (`app_theme.hpp` /
   `app_theme.cpp`) **and** `DESIGN.md` in the same change. Never invent a
   parallel palette in QML.
4. Prefer shared components: `IconActionButton.qml`, `CollapsibleSection.qml`,
   `DialogActionButton.qml`, `IconButton.qml`.
5. Register new `.qml` files in `alcedo_main/CMakeLists.txt`
   (`ALCEDO_MAIN_QML_FILES`) or the type will not resolve at runtime.

---

## Hard ban: Material Design style

Production app style is **Basic**, not Material:

```cpp
// main.cpp — required
QQuickStyle::setStyle("Basic");
```

**Do not:**

- `import QtQuick.Controls.Material` in editor/workspace panels
- `Material.foreground` / `Material.accent` / Material padding on dense chrome
- Rely on Material `Button` / `ToolButton` for structural icon actions (Material
  pads and stretches icon-only chrome into rectangles)

**Do:**

- Root structural SVG actions as **`IconActionButton`** (`Item` root, square hit
  / optical / source tokens, `ColorImage` tint)
- Custom surfaces as explicit `Rectangle` fills from `appTheme` (opaque named
  colors; Phase 4D)
- Leave Qt Quick Controls Basic to host only what you paint; do not fight
  Material ripples and minimum paddings

Tests may load Material for isolation harnesses; production QML must not.

---

## Hard ban: compound labels with centered-dot separators

Alcedo-owned UI copy must not join independent names, values, counts, modes, or
states into an `xx · xx` string. This rule applies to panel titles, node chrome,
metadata, badges, toolbars, list rows, and status text.

**Forbidden examples:** `On · 2 masks`, `RAW · Active`, `Tone Curve · Enabled`.

Give each item a separate visual role instead. Use a dedicated label, value,
icon with an accessible name, column, or row. Use spacing, alignment, or a
structural divider when the relationship needs visual grouping. Do not evade
this rule by replacing the centered dot with a bullet, slash, vertical bar, or
dash inside the same compound label.

## Hard ban: unrequested pills and badges

Do not add a new pill, badge, chip, tag, lozenge, or similar rounded text
container unless the user explicitly requests that element for the current UI.
Do not infer permission from a reference image, an existing component, or the
type of data. Use plain text, an icon, a standard action, or layout structure by
default. Do not copy an existing approved pill or badge into a new surface
without an explicit user request.

## Hard ban: unrequested status dots

Do not add a new status dot, presence dot, activity dot, notification dot, or
other colored circular indicator unless the user explicitly requests it for the
current UI. Do not infer permission from a reference image, an existing
component, or a semantic color token. Do not use a dot for decorative balance.
Existing approved indicators can remain in an unchanged surface. Do not copy
them into a new or changed surface without an explicit user request.

---

## Color and VI language

### Surfaces (editor family)

| Role | Token | Notes |
| --- | --- | --- |
| Card / panel shell | `cardSurfaceColor` | History, adjustment shell, filmstrip — same family |
| Sunken track / list well | `bgBaseColor` | Scope, adjustment nav track, LUT list track |
| Borders | `cardBorderColor` / `dividerColor` | 1 px hairlines |
| Text | `textColor` / `textMutedColor` | Primary / secondary |
| Icon default | `iconColor` | Toolbar SVG tint when not inverted |
| Accent | `accentColor` | Workspace thumb, favorite active on dark rows |
| Danger | `dangerColor` / `dangerTintColor` | Invalid / error only |

**Surface equality:** left rail, right adjustment stack, filmstrip, and viewport
placeholder share card surface. Disabled tools mute text/icons and disable
controls — **do not** recolor the shell to a second panel tone.

### Monochrome inverted list rows (dense catalogs: LUT)

Use list tokens, not ad-hoc `#D8D4CD` / `Qt.rgba`:

| Token | Role |
| --- | --- |
| `editorListSelectedFillColor` | Light bone selected well |
| `editorListSelectedInkColor` | Text/icon ink on that well (`bgBase`) |
| `editorListFavoriteIdleColor` / `…ActiveColor` | Star on dark rows |
| `editorListFavoriteIdleOnSelectedColor` / `…ActiveOnSelectedColor` | Star **inverted** on selected well |

**Rules:**

- Selected row = light bar + dark ink (B&W), not cool slider-blue accent.
- Favorite stars and type badges **invert with the row** so light-on-light never
  disappears.
- Non-selected type badge fill: white-family (`editorSliderHandleColor`), not
  card idle gray that disappears into the track.
- List selected well is **inset** from the sunken track (`spaceXs` ListView
  margins + row spacing) — never flush to the track border.
- LUT selection uses **one sliding chrome** on `ListView.contentItem` (not
  per-row opacity — recycling skips `Behavior`). Long jumps snap + fade; nearby
  moves animate `y`. Snapshot load must not `refresh` when path already exists
  (avoids contentY hitch).
- Segmented capsules (workspace, adjustment nav, method) share the same
  monochrome thumb/well tokens — no `accentColor` blue slab.
- No hover well under favorite stars unless DESIGN documents an exception.
- No folder-path chrome lines that add noise without product value.

### Spacing / type / icons

- Spacing: only `spaceXs`…`spaceXl`
- Panel titles: `fontSizeSection` + `fontWeightHeading` (not random 13/14 px)
- Structural SVGs: compact rails use
  `iconButtonHitSizeCompact` / `iconOpticalSizeCompact` /
  `iconSourceSizeCompact`
- **Normalize toolbar SVGs** in `panel_icons/`: `viewBox="0 0 24 24"`,
  `stroke="white"`, **`stroke-width="1.5"`** (DESIGN stroke rule). Do not mix
  Tabler 2 px defaults with 1.25 px assets in the same toolbar.
- Tint via `ColorImage` + `appTheme.iconColor` (or list invert tokens), not
  baked-in `#A7ABB3` strokes that fight the tint.

### Layout geometry

When building segmented nav thumbs, **define** `navHit` /
`navSpacing` (or equivalent) explicitly. Missing them → `NaN` geometry → icons
“fall out” of the track. That is not a Material bug; it is missing tokens.

---

## QML module and structure

1. New panel file → add to `ALCEDO_MAIN_QML_FILES` in
   `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt`.
2. Reconfigure/build after CMake list changes.
3. First-class navbar panels need a panel key in
   `EditorSessionController::NormalizeAdjustmentPanel` (e.g. `"lut"`). Unknown
   keys collapse to `"tone"`.
4. StackLayout index maps must stay consistent with nav order (tone / look /
   lut / display / geometry / raw).
5. Prefer panels as explicit StackLayout children (stable objectNames for tests),
   not anonymous Repeaters, when tests assert `editorAdjustmentPanel_*`.

---

## Panel snapshot load / selection restore (critical)

This failure mode has repeated across QML refactors: open image → select control
value → leave editor workspace → re-enter → UI does not show the restored
selection.

### Required pattern (every adjustment panel)

1. **Each panel owns** `loadFromSnapshot(snapshot)` (Tone / Look / LUT model).
2. Stack fans out on `AdjustmentSnapshotChanged` and session bind; **also**
   `Component.onCompleted` with `lastAppliedRevision = -1` so first frame loads
   when session is already set without a change signal.
3. Panel also loads on `onEditorSessionChanged` and when late-bound models
   arrive (`onLutModelChanged` / equivalent).
4. Load path uses **property write** (load-only), not user edit APIs:
   - Good: `model.selectedPath = path` / plain value setters
   - Bad: `selectPath()` / `editValue()` that submit history commits
5. Missing snapshot field → clear to default (e.g. empty LUT path → None), do not
   leave stale UI state.

### Selection highlight bindings

**Do not** hide the selected key only inside a JS helper:

```qml
// BAD — selectedPath access inside the function is often not tracked
readonly property bool entrySelected: root.isPathSelected(path, kind)
function isPathSelected(path, kind) {
    return path === root.lutModel.selectedPath  // dep may not invalidate
}
```

**Do** expose a panel-level alias the delegate binds:

```qml
// GOOD — binding re-evaluates when selection changes or snapshot loads
readonly property string selectedPath: lutModel ? String(lutModel.selectedPath || "") : ""
readonly property string selectedPathNormalized: normalizePath(selectedPath)

readonly property bool entrySelected: {
    var _dep = root.selectedPath  // establish dependency
    return root.isPathSelected(modelData.path, modelData.kind)
}
```

Normalize paths for compare (`\` → `/`, case) when OS path forms differ between
snapshot and catalog entries.

### Selection vs list rebuild / scroll

- User **click selection** must **not** rebuild the whole list or force
  `positionViewAtIndex(..., Contain)` (pins the row to the viewport bottom).
- Prefer: model emits `selectedPathChanged` **without** `entriesChanged` for
  highlight-only updates; QML derives selected from `selectedPath`.
- Structural rebuilds (catalog refresh, filter, sort, favorites-only) may
  preserve `contentY` and only scroll if the selected row is fully off-screen.

---

## Toolbar and dense chrome checklist

- [ ] All toolbar SVGs share optical/source tokens and `iconColor` (or documented
      selected gold for favorites filter only)
- [ ] SVG sources: white stroke, 1.5 width, 24×24 viewBox
- [ ] Icon actions use `IconActionButton` compact mode
- [ ] No Material ToolTip-only hacks required; Basic ToolTip on Item is fine
- [ ] Idle button fill on sunken tracks = `bgBaseColor`, not card surface nested
      cards
- [ ] Row heights uniform when product requires it (e.g. None vs file rows)

---

## Testing expectations

When changing adjustment / catalog panels:

1. **Model unit tests** — load-only vs submit, no spurious `entriesChanged` on
   select, favorites, filter.
2. **QML harness tests** (see `editor_lut_panel_qml_test.cpp` pattern):
   - Load production QML via `Loader` + `appTheme` context
   - Controllable fake model when filesystem catalog is unnecessary
   - Assert `selectedPath` after `loadFromSnapshot`
   - Assert `contentY` stable after select / click
   - Assert VI tokens equal `AppTheme` getters (no literal drift)
3. **Workspace shell** — navbar keys and stack indices when adding a panel.
4. Register new test targets in `alcedo_studio/tests/ui/CMakeLists.txt`.
5. Name tests by behavior (no “smoke” in names — project rule).

---

## Anti-patterns (from real incidents)

| Symptom | Cause | Fix |
| --- | --- | --- |
| UI fails to start after new QML | File not in `ALCEDO_MAIN_QML_FILES` | Register + reconfigure |
| LUT nav snaps to Tone | `NormalizeAdjustmentPanel` missing key | Add `"lut"` (etc.) |
| Icons explode / NaN layout | Missing `navHit` / spacing props | Define from `appTheme` |
| Material-looking padding / chrome | Material style or Material Button | Basic + IconActionButton |
| Selected row not restored | Binding / load ownership wrong | Panel `loadFromSnapshot` + `selectedPath` alias |
| Click jumps list to bottom | rebuild + `ListView.Contain` on select | No entries rebuild on select; preserve contentY |
| Star/badge invisible on selected | No invert tokens | `editorListFavorite*OnSelected` / badge invert |
| Toolbar icons look mismatched | Different stroke-width/color in SVG | Normalize assets to 1.5 / white |

---

## Workflow for a new adjustment panel

1. DESIGN.md + AppTheme tokens (if any new surface).
2. QML panel with `objectName`, theme props, `loadFromSnapshot`, selection alias.
3. Wire StackLayout + navbar + `NormalizeAdjustmentPanel`.
4. CMake QML list.
5. Model load-only vs submit APIs.
6. QML + model tests.
7. Manual: select → switch library/editor workspace → re-open → selection still
   shown; click mid-list → contentY stable.

---

## Canonical references

- VI: `alcedo_studio/src/ui/alcedo_main/DESIGN.md`
- Tokens: `alcedo_studio/src/include/ui/alcedo_main/app_theme.hpp`
- Style: `alcedo_studio/src/ui/alcedo_main/main.cpp` (`QQuickStyle::Basic`)
- Icon action: `qml/IconActionButton.qml`
- Stack fan-out: `qml/EditorAdjustmentStack.qml`
- Example panel: `qml/LUTPanel.qml`
- Example tests: `tests/ui/editor_lut_panel_qml_test.cpp`,
  `tests/ui/editor_look_model_test.cpp`
)
