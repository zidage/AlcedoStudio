# Editor Dialog Manual Test Matrix

## Purpose

Use this checklist before and after each editor dialog refactor PR. The goal is to catch behavior drift while panel ownership moves from `EditorDialog` to dedicated widget classes.

Run these tests on a RAW image with existing pipeline params and on a newly imported RAW image with no saved adjustment params.

## General Setup

1. Open an image in the editor dialog.
2. Confirm the viewer renders an initial quality preview.
3. Confirm the versioning panel shows the current working version state.
4. Keep the application log visible if available.

Expected global behavior:

- Control changes produce preview updates.
- Slider drags do not append transactions until release.
- Discrete choices commit once per actual value change.
- Releasing or selecting the current value does not create a duplicate transaction.
- The pipeline dirty flag is set after committed changes.
- Undo restores UI controls and preview.

## Tone Panel

| Test | Steps | Expected result |
| --- | --- | --- |
| Exposure drag preview | Drag exposure without releasing. | Viewer updates with fast preview. No transaction is appended while dragging. |
| Exposure release commit | Release exposure after changing it. | One `EXPOSURE` transaction is appended in `Basic_Adjustment`; quality preview is scheduled. |
| Contrast reset | Change contrast, then activate reset. | Contrast returns to default and commits once if the value changed. |
| Highlights and shadows | Change highlights and shadows separately. | Each field commits its own operator params and undo reverses one transaction at a time. |
| Whites and blacks | Change whites and blacks separately. | Global-param scaled mapping remains equivalent to current behavior. |
| Curve edit preview | Move a curve control point. | Viewer previews the curve while editing. |
| Curve release commit | Release the curve edit. | One `CURVE` transaction is appended with normalized control points. |
| Saturation | Change saturation and release. | `SATURATION` operator updates in `Color_Adjustment`. |
| Detail controls | Change sharpen and clarity. | `SHARPEN` and `CLARITY` update in `Detail_Adjustment`. |

## Color Temperature

| Test | Steps | Expected result |
| --- | --- | --- |
| As-shot load | Open a RAW image with color metadata. | As-shot CCT and tint display resolved values when supported. |
| Custom mode | Switch to custom mode. | Existing displayed values are preserved as custom CCT and tint. |
| CCT change | Drag CCT in custom mode and release. | `COLOR_TEMP` transaction is committed once. |
| Tint change | Drag tint in custom mode and release. | `COLOR_TEMP` transaction is committed once. |
| Unsupported metadata | Open an image without supported color-temp metadata. | Unsupported label visibility matches current behavior. |

## Look Panel

| Test | Steps | Expected result |
| --- | --- | --- |
| HLS target switch | Select a candidate hue. | Current HLS profile is saved, selected profile loads, target label and swatches update. |
| HLS sliders | Change hue shift, lightness, saturation, and hue range. | HLS profile tables update and one `HLS` transaction commits per release. |
| CDL wheels | Drag lift, gamma, and gain wheels. | Offset labels update and `COLOR_WHEEL` commits on release. |
| CDL master sliders | Change lift, gamma, and gain master sliders. | Derived RGB labels update and commit behavior matches wheel changes. |
| LUT browser selection | Select a different LUT. | `LMT` params update once, preview refreshes, and selected item styling updates. |
| LUT keyboard navigation | Press up and down while LUT browser should consume keys. | Previous and next LUT selection works and unrelated widgets do not steal the shortcut. |

## Display Transform Panel

| Test | Steps | Expected result |
| --- | --- | --- |
| ODT method switch | Toggle between ACES and OpenDRT methods. | Method card styling and method-specific controls update. `ODT` commits once. |
| Encoding space | Change encoding color space. | EOTF choices are sanitized to supported options. Viewer display encoding syncs. |
| Encoding EOTF | Change EOTF. | `ODT` params update and viewer display encoding syncs. |
| Peak luminance | Change peak luminance and release. | `ODT` commits once. |
| ACES limiting space | Change ACES limiting space. | `ODT` commits once and reload keeps the selection. |
| OpenDRT presets | Change OpenDRT look and tonescale presets. | Preset controls update without corrupting detail values. |
| OpenDRT details | Edit a detail slider/spin box. | Preset is marked custom and `ODT` commits once on release/final change. |

## Geometry Panel

| Test | Steps | Expected result |
| --- | --- | --- |
| Enter geometry panel | Switch to geometry panel. | Crop overlay appears. Pipeline preview remains pre-geometry for editable crop bounds. |
| Crop sliders | Move x, y, width, and height sliders. | Overlay rect updates immediately; no crop transaction commits until apply or release behavior expected by current UI. |
| Aspect preset | Select a fixed aspect preset. | Crop rect is adjusted to the locked aspect and overlay lock is enabled. |
| Custom aspect | Edit width and height spin boxes. | Preset becomes custom and overlay lock uses the custom ratio. |
| Rotation | Drag rotation. | Overlay rotation updates immediately. |
| Apply geometry | Click apply. | One `CROP_ROTATE` transaction commits and a full-frame preview is marked required. |
| Reset geometry | Use reset button or shortcut. | Crop and rotation return to defaults and preview behavior matches current implementation. |
| Leave geometry panel | Switch away from geometry. | Overlay hides or returns to existing non-editing behavior; committed crop is used for pipeline render. |

## Raw Decode Panel

| Test | Steps | Expected result |
| --- | --- | --- |
| Highlight reconstruction | Toggle highlight reconstruction. | `RAW_DECODE` commits once and preview refreshes. |
| Lens calibration enable | Toggle lens calibration. | `LENS_CALIBRATION` enable state updates and commits once. |
| Lens make override | Select a lens make. | Model list refreshes; lens calibration params commit once. |
| Lens model override | Select a model. | Lens calibration params commit once. |
| Clear lens override | Clear make/model if supported by UI. | Params return to automatic/default behavior. |

## Versioning and History

| Test | Steps | Expected result |
| --- | --- | --- |
| Undo single transaction | Make one committed adjustment, then undo. | Pipeline, preview, committed state, and control state all revert. |
| Undo stack | Make several committed adjustments, then undo repeatedly. | Each undo reverts one transaction in reverse order. |
| Commit all | Make adjustments and click commit all. | Working version commits and UI starts a new working version from the commit. |
| Empty commit | Click commit all with no uncommitted transactions. | Existing informational behavior is preserved. |
| Version checkout | Select an older version. | Pipeline params import, all controls reload, preview updates, and working version is seeded correctly. |
| Plain mode | Switch to plain working mode and start a new working version. | Working-version behavior matches current implementation. |

## Regression Checks

| Check | Expected result |
| --- | --- |
| Reopen editor after changes | Saved pipeline params load into controls rather than defaults overwriting them. |
| Close editor after committed changes | Pipeline dirty state is preserved for project save flow. |
| Translation refresh | Text and tooltips update for all migrated and unmigrated panels. |
| Shortcut focus | Undo and LUT navigation shortcuts trigger only in the intended focus contexts. |
| No duplicate transactions | Releasing unchanged controls schedules quality preview if current behavior does, but does not append transactions. |
| No direct panel pipeline ownership | Migrated panel classes do not store `PipelineGuard`, `Version`, or `PipelineScheduler`. |
