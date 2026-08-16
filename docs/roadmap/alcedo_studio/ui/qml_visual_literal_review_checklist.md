# QML visual-literal review checklist (Phase 4C)

Use this checklist on any PR that touches `alcedo_studio/src/ui/alcedo_main/qml`
or `AppTheme` design tokens. Goal: prevent unexplained visual literals and
DESIGN/token drift.

## Before merge

1. **Read** `alcedo_studio/src/ui/alcedo_main/DESIGN.md`.
2. **Token table:** every new size, color, radius, spacing, or duration either
   uses an existing `appTheme.*` property or adds one to `AppTheme` **and**
   DESIGN.md in the same change.
3. **No silent literals** for structural chrome in editor/workspace QML:
   - Icon hit / optical / source sizes → `iconButtonHitSize*`, `iconOpticalSize*`,
     `iconSourceSize*`
   - Card fills/borders → `cardSurfaceColor` / `cardBorderColor` (or
     `colCardSurface` / `colCardBorder` from the Main theme mirror)
   - Fold timing / easing → `motionFoldOpenMs` / `motionFoldCloseMs` /
     `motionEasing` / `reduceMotion`
   - Body spacing → `spaceXs`…`spaceXl`
4. **Documented exceptions only.** If a literal remains (e.g. History rail 46 px
   hit, window caption canvas), it is listed under “Compact exceptions” in
   DESIGN.md with a one-line why.
5. **Shared primitives first.** New structural SVG actions use
   `IconActionButton`; new collapsible groups use `CollapsibleSection`.
6. **Capsule rule.** Library/Editor segments must not reintroduce
   `highlightLevel`, segment hover/press fills, or focus rings. Only
   `workspaceSwitchThumb` indicates the active workspace.
7. **Copy.** No developer placeholders (“will appear here”, “TODO”, phase names)
   in visible `qsTr` strings.
8. **A11y.** Every SVG action has tooltip + `Accessible.name` + keyboard path.
9. **Tests.** `WorkspaceShellTest` (including Phase 4C motion/visual cases) and
   `MainQmlWorkflowTest` pass. Prefer `reduceMotion` for terminal-geometry
   workflow tests; use `driveFoldProgress` for intermediate motion proof.
10. **`git diff --check`** is clean.

## Quick grep aids (not exhaustive)

```text
# Suspicious fixed icon/hit sizes in editor shell QML
icon\.(width|height):\s*(1[6-9]|2[0-3])\b
width:\s*(16|18|20)\b
height:\s*(16|18|20)\b

# Missing DESIGN touch when AppTheme tokens change
# → update DESIGN.md tables in the same PR
```

## Out of scope

- Phase 5 backend/session/render scheduling
- Phase 6 production adjustment control bodies
- Whole-application restyle of Library non-shell chrome (unless it breaks the
  card-surface specification)
