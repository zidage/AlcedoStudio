# Phase NM7 — Viewer Mask Creation and Editing

Date: 2026-09-08

Status: NM7.1–NM7.12 completion records retained; NM7.12R implemented and partially
verified (release latency, real-adapter QML harness, and Metal still open);
NM7.12RR planned as one complete repair phase; NM7.13–NM7.15 planned.
This document records the NM7.1 source
audit, NM7.2 parameterized Brush owner operations, NM7.3 typed stroke history plus the
project/schema cutover, NM7.4 canonical rasterization with regional Mix replay, NM7.5
shared ReferenceSpace mapping with Brush placement, NM7.6 control-only retained QSG,
NM7.7 Radial/Linear creation plus existing-mask movement, NM7.8 parameter-mask
controls, drawer selection/deletion, and crop-style Gradient, NM7.9 accumulating
Brush paint/erase/move with typed stroke history, NM7.10 serial Interactive Mix
with one current Grade coverage result, NM7.11 project Mix-cache storage plus
Keep/DeleteOnProjectClose cleanup, and NM7.12 production Mask controls plus project
storage UI. The six reported Brush regressions require NM7.12R acceptance before Brush UI
can be considered qualified. Newly reported second-scale drawing, first-drag input loss,
and erased-content reappearance are covered by NM7.12RR. NM7.13–NM7.15 acceptance remains outstanding.

Parent: [Node-aware Pipeline Editing and Mask Creation](../node_mask_editor_master_plan.md),
Sections 8–12, 18, 20.3–20.4, 21.8, 23.5, and 24.

Prerequisites: NM1–NM5 ownership, multi-Grade/multi-Mask runtime and typed history,
and node selection; NM6 serial input consumption, native parameter access, result retention,
and selected-node integration. The current NM6 record lists NM6.8–NM6.9 as planned. Planning NM7
does not waive those exit conditions or authorize exposing incomplete dependent production actions.


2026-09-08 revised user direction: Brush paths and analytic parameters are the persistent source;
Undo/Redo applies reversible commands and recomputes affected regions. R8 is disposable current
coverage, never a raster history entry. Each Grade has one current Mix coverage cache slot, managed
under a user-configurable project root with per-project cleanup. Moving an existing Brush/Radial/
Gradient must update actual Interactive pixels before release; QSG shows controls without coverage
highlighting. This revision supersedes the earlier per-stroke immutable-asset design.

Required algorithm/storage design:
[Parameterized commands, regional replay and project cache](mask_command_replay_and_project_cache_plan.md).
Read it before implementing NM7.2–NM7.15. It gives equations, inverse operations, replay bounds,
cache lifecycle, project settings, failure behavior and an initial executable algorithm experiment.

2026-09-08 parameter-mask revision: retain NM7.1–NM7.7 completion records. Insert NM7.8
for Radial feather/range controls, Node drawer selection/deletion, and Geometry crop-style
Gradient controls. Former NM7.8–NM7.14 become NM7.9–NM7.15; forward references in historical
records use the new numbering. Selected analytic boundary lines are required during later
editing as well as creation; coverage fill remains prohibited. The reported Radial “半圆范围”
is specified here as the radial/elliptical range contour and feather boundaries of the existing
full-ellipse evaluator, without introducing a semicircle coverage algorithm.

## 1. Purpose and background for the executor

2026-09-10 Brush repair revision: insert **NM7.12R** immediately after NM7.12, without
renumbering NM7.13–NM7.15. The user requested a repair plan only; this revision does not
execute implementation, builds, tests, or performance qualification. NM7.12R supersedes
earlier Brush single-point movement controls and hard-edge creation defaults. Historical
completion records remain evidence for the tests they actually ran, not proof that the six
reported viewer problems are resolved.

NM7 makes local adjustment possible directly on the photograph. A user selects a Color Grade,
creates an area of influence with Brush, Radial, or Linear Gradient, and sees that Grade change
only the covered pixels. Later edits, Undo, Version checkout, reopening, and export must reproduce
that same area. The drawing is therefore an editing input to the existing image pipeline, not an
independent illustration layered over an otherwise unchanged photograph.

Three different things must remain distinct:

| Thing | Purpose | Owner / lifetime |
| --- | --- | --- |
| Mask model | Defines which pixels receive a Grade; saved with history and Version | `ColorGradeNodeModel` in the single live `PipelineDocument` |
| Editing input | Pointer position, active handle, tool settings, ordered stroke samples, unfinished operation | Application creation controller and existing serial input owner; one sequence |
| Viewer overlay | Cursor and handles; temporary creation guides only; no affected-area highlighting | `EditorOverlayItem` retained QSG nodes; derived display geometry |

The overlay should respond in the next available Qt Quick frame even when the image pipeline is
busy. It must never enter the exported photograph. Conversely, a responsive overlay does not prove
that the actual Mask was applied: the Interactive photograph must show the real native evaluator
output, and settled edits must request Quality through the existing renderer.

### 1.1 What the earlier phases already provide

- NM1 establishes one writable document and one shared executor for each live image. UI code
  cannot mutate document containers or build a second editable graph for preview.
- NM2 runs the ordered Color Grades. A Mask targets an exact owning `NodeId`; it does not target
  whichever Grade happens to be first or currently selected when queued work finishes.
- NM3 puts an ordered Mask list inside each Grade, establishes three native evaluators, immutable
  R8 assets, and request-owned active raster inputs with dirty rectangles. Those are source-audit
  facts: the new NM7 source/cache design deliberately replaces persistent R8 as edit data.
- NM4 gives structural/field/source/command mutations typed history, inverse application, Version
  recovery and Paste. NM7 extends that path to parameterized strokes and replaces new Brush asset-key history.
- NM5 owns selected-node identity and the QuickQanava projection. Mask rows identify children of
  a Grade; masks are not graph vertices and cannot acquire scene-image connectors.
- NM6 separates input acceptance from owner mutation and serial rendering. It keeps current
  results by dependency and reader lifetime. A slow GPU must not block the GUI input handler.

Read the earlier plans for historical decisions, but use their completion records and current
source for what actually exists. Several introductory source tables deliberately describe the
state before those phases were implemented.

### 1.2 Pixel semantics that the UI must explain correctly

For one Mask, source evaluation and feather precede invert and opacity. Range fields remain
null/disabled. Enabled masks combine by per-pixel maximum. Grade mixing remains:

```text
M = 1                                 when the list is empty
M = 0                                 when the nonempty list has no enabled masks
M = max(effective enabled coverage)    otherwise
output = input + clamp(M * grade_mix, 0, 1) * (adjusted - input)
```

Consequences worth testing and explaining in product copy:

- Adding the first empty Brush changes a Grade from full-image coverage to zero coverage until
  paint is applied. Delay the provisional insertion until valid creation input begins; merely
  choosing a tool must not momentarily remove the Grade from the photograph.
- Adding another Mask extends coverage; overlapping masks do not add their opacities.
- Erasing within the accumulating Brush only changes that Brush. Another enabled
  Mask can still cover the erased area. Erasing is not a new cross-Mask subtraction operation.
- Removing the last Mask restores full-image Grade coverage. Disabling the last enabled Mask
  yields zero coverage. These are intentionally different operations.
- A clean Grade can have no visible color change even with a valid Mask. Do not inject an
  exposure adjustment to make a Mask demonstration visible; tests explicitly prepare a Grade.
- Mask order affects list presentation, not Union pixels. Preserve the existing display-order
  policy; do not introduce pixel dependencies or photo history for a list-only move.

## 2. Decision register and scope

User decisions received on 2026-09-08:

| Decision | Required behavior | Implementation dependency |
| --- | --- | --- |
| Brush operations | Paint/erase with size and strength controls | NM7.2, NM7.4, NM7.9 |
| Persistent source | Parameterized ordered strokes; history stores reversible commands, not R8 revisions | NM7.2–NM7.3 |
| Accumulation/cache | Same Grade's strokes accumulate in one Brush; one current final Grade Mix R8 cache slot | NM7.4, NM7.10–NM7.11 |
| Radial initial drag | Center outward | NM7.7 |
| Radial feather/range | Explicit inner/outer feather controls and selected ellipse/boundary lines | NM7.8 |
| Node Mask drawer | Select existing Mask, reopen its controls and delete with Undo/Redo | NM7.8 |
| Gradient controls | Geometry crop-style dual strokes and edge grips on three parallel guides; no kite outline | NM7.8 |
| Existing Mask movement | Brush/Radial/Gradient update real Interactive pixels during drag, Quality after release | NM7.5, NM7.7–NM7.10 |
| QSG existing-mask display | Controls only, no affected-area fill or completed Brush path | NM7.6 |
| Initial creation guides | Working interpretation: temporary cursor/outline/path guides are allowed only during initial drawing, never area-fill highlighting; clarification requested | NM7.6 |
| Editing body | Seventh Mask parameter page in the Adjustment Stack; disabled until Mask creation or selection; Node drawer remains the sole Mask list | NM7.8, NM7.12 |
| Project storage | User-selected cache root, per-project Keep/DeleteOnProjectClose and Clear actions | NM7.11–NM7.12 |

Size and strength are explicit user controls. Automatic pen-pressure modulation was not requested;
this plan specifies manual size/strength, with stylus position accepted through the same input
route. Adding pressure mapping later requires an explicit product decision about radius/strength
curves and device behavior; do not infer pressure dynamics from stylus support.

The single current Brush raster is a per-Grade accumulation rule, not a restriction to one total
Mask: the Grade can also own multiple Radial and Linear Gradient masks. The Brush header action
resumes that Grade's existing Brush, or creates it on the first valid stroke if absent. It must
never create a new Mask row per stroke or per tool re-entry. Undo retains parameter commands; it never retains earlier raster revisions. Every enabled source
contributes to the one current Grade coverage result used by Mix.

Do not rewrite existing valid documents just to enforce a new UI creation rule. If a document
already contains multiple Brush sources, preserve their data and require selection of the existing
Brush target when entering from the header; do not silently flatten them, change opacity/invert,
or add another Brush. Record this exceptional-input behavior in tests. NM7-created workflows
produce one accumulating Brush per Grade. Raster-only old project formats are handled by the
explicit version gate described in the companion design, not silently converted into paths.

### 2.1 Included capability

1. Create and select Brush, Radial and Linear Gradient masks on the selected Color Grade.
2. Edit Radial center/axes/rotation/inner and outer feather, Linear origin/direction/transition,
   and Brush strokes plus supported Brush/Mask controls.
3. Expose existing Mask opacity, enabled, invert, rename and delete through focused owner calls.
4. Provide immediate retained QSG assistance, real Interactive preview, and settled Quality.
5. Support pointer release, Enter or panel-switch confirmation, Escape cancellation, keyboard editing, interruption, and stale
   asynchronous work rejection with precise one-operation/one-commit behavior.
6. Restore correct mask selection and values after Undo/Redo, checkout, re-entry, and image change.
7. Parameterize Brush paths and add reversible commands plus exact regional replay.
8. Add project cache settings, stable-slot writeback, per-project cleanup and cacheless recovery.
9. Qualify the actual viewer/native paths, storage bounds and failures.

### 2.2 Exclusions

No Color Range/Luminance Range selection UI, AI segmentation, arbitrary mask combinations,
new graph topology, a per-control-point stroke editor, second preview renderer, or automatic project
migration. Stroke parameters/history are in scope. Retain the existing quality/decode policy;
change Brush persistence and R8 caching as required by this revision. No CPU or
alternate-backend recovery path, lower-quality preview substitute, or per-frame whole-model JSON.

Pressure/tilt dynamics are not implied by accepting a stylus as a pointing device. Brush size,
hardness and strength are tool inputs for new samples; Mask opacity and source feather modify
existing coverage and have distinct persistence semantics.

## 3. Source audit at plan creation

Inspected working-tree revision: `0ccb7e1c`. Working tree was clean before this documentation
change. Paths below are existing unless expressly marked proposed. Recheck revision, APIs,
current callers and tests when execution starts.

| Source | Observed behavior | NM7 implication |
| --- | --- | --- |
| [Mask model](../../../../../alcedo_studio/src/include/edit/mask/mask_model.hpp) | Brush stores optional asset/descriptor/feather; analytic sources store typed fields; Mask owns enabled/opacity/invert/name | Change Brush source to canonical strokes/placement through this existing type; no parallel writable model |
| [Grade model](../../../../../alcedo_studio/src/include/edit/graph/color_grade_node_model.hpp) | Owns Mask list and focused add/remove/source/field APIs; tracks content revisions | Route changes through the owner and preserve revision propagation |
| [Active raster input](../../../../../alcedo_studio/src/include/edit/mask/active_raster_mask.hpp) | Full immutable R8 pixels plus target, generation, content revision and clipped raster rectangle | Cannot mutate a published buffer while a render reads it |
| [Mask store](../../../../../alcedo_studio/src/include/edit/mask/mask_store.hpp) | Content-addressed persistent asset API, not a disposable project cache | Replace new-Brush product consumers with parameter evaluation/project cache; never delete old assets as cache |
| [Typed history](../../../../../alcedo_studio/src/include/edit/history/pipeline_edit_batch.hpp) | `AddMaskChange`, `RemoveMaskChange`, `ReplaceMaskSourceChange`, `ReplaceMaskAssetChange`, `SetMaskFieldChange` exist | Extend NM4 with stroke insert/remove and placement operations; no new-Brush asset-key history |
| [Pending input](../../../../../alcedo_studio/src/include/app/editor_pending_input.hpp) | Coalesces absolute field writes and retains ordered release/cancel/node-switch boundaries | Brush streams need ordered append semantics, not newest-field replacement |
| [Parameter API](../../../../../alcedo_studio/src/include/app/editor_pipeline_command_service.hpp), [target validation](../../../../../alcedo_studio/src/include/app/editor_adjustment_types.hpp) | Ordinary adjustment path excludes Mask targets; rejection text still references NM3 | Add an explicit supported Mask command route; deleting a guard alone is insufficient |
| [Interaction controller](../../../../../alcedo_studio/src/include/ui/editor_rhi/editor_interaction_controller.hpp), [mapper](../../../../../alcedo_studio/src/include/ui/edit_viewer/viewport_mapper.hpp) | GUI owns view/crop mapping; item inputs are logical coordinates | Extend the existing mapping boundary, not QML formulas |
| [Resolved render geometry](../../../../../alcedo_studio/src/include/edit/geometry/resolved_render_geometry.hpp) | One rounded geometry supplies `render_to_reference` and related matrices | Use actual reference geometry rather than the dimensions of the last DetailPatch |
| [Overlay item](../../../../../alcedo_studio/src/ui/editor_rhi/editor_overlay_item.cpp) | Retained triangle geometry for crop/ROI, rebuild coalescing and node reuse | Extend this surface; factor Mask geometry into focused helpers |
| [Workspace](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspace.qml) | Point/Drag handlers can both forward press/move/release; wheel/pinch/double-click use the same viewer | Mode routing and sequence identity must prevent duplicate Brush input |
| [Adjustment header](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorAdjustmentHeader.qml) | Brush/Radial/Gradient actions are present without product commands | Wire these existing entry points, preserving header layout |
| [Masks panel](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorMasksContextPanel.qml) | Read-only type rows; NM6.7 says this module file is unused | Do not claim adding controls here alone makes the feature reachable |
| [Mask drawer](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeMaskDrawer.qml), [row](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeMaskTypeRow.qml) | Approved compact type/icon rows | Add selection/open behavior without moving editors into graph nodes |
| [Metal evaluator](../../../../../alcedo_studio/src/edit/runtime/metal/shader/mask.metal) | Analytic math uses normalized reference coordinates; Brush feather uses signed-distance work | Derive overlay boundaries from evaluator equations; audit native parity |
| [Project service](../../../../../alcedo_studio/src/app/project_service.cpp) | Owns project UUID, Save/Load metadata; Save reconstructs JSON | Persist per-project cache settings through owner and atomic write |
| [Cache settings UI](../../../../../alcedo_studio/src/ui/alcedo_main/qml/CacheSettingsPanel.qml) | Thumbnail-specific settings/stats via library module | Add separate project Mask cache body/adapter; do not reuse thumbnail configuration keys |
| [Project package](../../../../../alcedo_studio/src/app/project_package_backend.cpp) | Validates/materializes metadata and package contents | Carry source commands, not source-machine cache paths or raster history |
| [Existing viewer tests](../../../../../alcedo_studio/tests/ui/editor_overlay_interaction_test.cpp) | Crop/ROI geometry and interaction infrastructure | Extend coverage while preserving navigation/crop behavior |

Rechecked 2026-09-08 on Windows/MSVC debug (`build/debug`), Qt 6.9.3 (`D:/misc/Qt/6.9.3/msvc2022_64`),
CUDA Toolkit 12.8. NM6.8 and NM6.9 remain planned (lifecycle e2e and cross-backend pixel/perf
qualification). That does not authorize exposing Mask creation UI. Ordinary adjustment writes still
reject `ColorGradeMask` with the existing “until NM3” message. `BrushMaskSource` still persists
`asset_key`; `MaskStore` still publishes immutable `.r8mask` files; history still uses
`ReplaceMaskAssetChange`; transfer packages still list `mask_assets` keys; native Mask passes still
load `MaskStore` or request-owned active rasters. `EditorPendingInputQueue` still keeps the newest
absolute write per field. Parameterized strokes are not present in production JSON.

### 3.1 Reconcile NM6 product revisions before exposing UI

[NM6.7 completion](phase_nm6_node_aware_adjustments_plan.md#nm67--build-node-nameexif-header-and-capability-filtered-panels)
and [DESIGN.md](../../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md) supersede the older
capability-filtered navbar and vertical EXIF layout: EXIF occupies one row, and Mask tool actions
sit beside the node name. NM7 must preserve those landed choices.

The 2026-09-09 correction adds Mask as a seventh Adjustment Stack page. It is disabled until a
header creation action or Node drawer Mask selection enters Mask editing. The Node drawer is the
only Mask list; the page contains source-specific parameter controls and does not repeat management
rows. Store the previous ordinary panel key as session presentation state. Enter or selecting a
different adjustment page confirms and exits Mask editing, while Escape cancels. There are no
explicit Done/Cancel actions and the transient Mask page is not persisted as the ordinary panel.

## 4. Official Qt documentation and drawing decisions

Read on 2026-09-08. The project names Qt 6.9.3 in its supported packaging/build configuration.
Current unversioned Qt pages describe newer releases, so API availability must be checked against
the installed 6.9 headers. The Qt 6.9 `QQuickItem` archive was available; several other archived
pages did not load during research, so the current official pages below were read with explicit
version checks. No Qt upgrade is part of NM7.

| Official source | Rule applied to this module |
| --- | --- |
| [Qt 6.9 QQuickItem: custom scene graph items and resource handling](https://doc.qt.io/archives/qt-6.9/qquickitem.html#custom-scene-graph-items) | `ItemHasContents`, `update()` and `updatePaintNode()` form the display boundary. Keep QSG access on the render thread and resource cleanup on the correct thread. |
| [Scene Graph: Custom Geometry](https://doc.qt.io/qt-6/qtquick-scenegraph-customgeometry-example.html) | Reuse `oldNode`; setters schedule an update; geometry and materials have explicit node ownership. Borrow the lifecycle pattern, not the example's line-width choice. |
| [QSGGeometry](https://doc.qt.io/qt-6/qsggeometry.html) | Triangle strokes are portable. Reallocation invalidates geometry data; rewrite vertices/indices after `allocate`. Mark geometry dirty; nondefault upload patterns also require data-dirty calls. `setVertexCount`/`setIndexCount` require Qt 6.10. |
| [Qt Quick Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html) | Retained scene nodes are separate from the GUI object tree; custom graphics remain within the Qt Quick scene graph. |
| [Scene Graph Default Renderer](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html) | Clipping and material changes affect batching. Use a viewport clip and deliberate antialiasing; do not create one clip/material/item for each dab. |
| [Qt Quick Input Handlers](https://doc.qt.io/qt-6/qtquickhandlers-index.html) | Passive observation is distinct from exclusive input ownership. Mode gates must prevent multiple tools acting on the same input. |
| [PointHandler](https://doc.qt.io/qt-6/qml-qtquick-pointhandler.html) | Tracks a point from press with a passive grab. It is not an exclusive brush lock. Account for authentic versus synthesized device events. |
| [PointerHandler](https://doc.qt.io/qt-6/qml-qtquick-pointerhandler.html) | Handle `canceled` and `grabChanged`; inactive is not automatically a successful release. Disabling a handler cannot be relied on to finish domain work. |
| [DragHandler](https://doc.qt.io/qt-6/qml-qtquick-draghandler.html) | Use `target: null` for controller-owned movement; drag threshold must not discard a Brush press or the start of a line. |
| [handlerPoint](https://doc.qt.io/qt-6/qml-qtquick-handlerpoint.html), [eventPoint](https://doc.qt.io/qt-6/qml-qtquick-eventpoint.html) | Capture primitive position/device/point values during delivery. Do not retain Qt event objects; handler positions can reset after release. |
| [Accessible](https://doc.qt.io/qt-6/qml-qtquick-accessible.html) | QSG handles need ordinary accessible control proxies with names, roles, values and actions. Use APIs available in Qt 6.9, not newer attached properties. |

### 4.1 Retained QSG implementation specification

These are Alcedo design choices derived from the documented lifecycle and current overlay code.

- Keep the existing `EditorOverlayItem` above `EditorViewportItem`. The overlay receives no
  pointer grab. Input remains in the workspace adapter and application creation controller.
- Separate selected control outline/fill, necessary direction lines, Brush cursor and temporary
  initial-creation guides. Do not create source-area fill or existing-Brush path subtrees. Reuse nodes
  when only parameters or colors change. Do not append a QObject per sample.
- Build line widths and handle discs from triangle lists. Do not rely on wide native lines,
  `DrawLineLoop`, or runtime triangle fans. A CPU helper may expand a fan to triangles.
- Tessellate Radial curves by projected error: proposed maximum chord deviation 0.25 logical px.
  Clip to visible image/viewport and cap work by visible geometry, without altering the stored
  Mask or the image evaluator. Small degenerate shapes use finite, explicit empty geometry.
- Use explicit edge-alpha triangle fringes for antialiasing and premultiplied color consistently.
  Do not assume `QQuickItem::antialiasing` automatically fixes custom geometry. Test control outlines, intersections, joins and caps for
  dark seams or doubled alpha. Feather
  controls show selected analytic boundary lines and handles, without a filled feather band.
- QSG coordinates and handle hit areas are logical pixels. Image-space radius is transformed;
  handle and outline widths remain constant in logical pixels at any zoom or DPR.
- For a growing stroke under Qt 6.9, use bounded geometry chunks or exact allocations with full
  rewrite after allocation. Reuse completed chunks within the active sequence. Release sequence
  geometry after settle/cancel; do not retain every historic stroke for display.
- GUI-side geometry calculations consume only current input and published read-only display
  values. `updatePaintNode` synchronizes those derived values while Qt blocks the GUI thread;
  it must not acquire a live pipeline lock, read mutable worker state, perform cache I/O, or
  signal synchronous domain mutations.
- Theme/size/DPR/mode changes schedule the required geometry/material update. Empty visibility
  clears or disables stale content. If `oldNode` is null after scene invalidation, reconstruct
  from current display state; no dangling pointers back to a destroyed node tree.
- Prefer automatic node ownership for geometry/material cleanup. Any resource held outside that
  tree needs the documented render-job/invalidation cleanup, not GUI `deleteLater()`.
- During initial drawing only, temporary Brush path/contour guides may follow samples. Existing
  Brush editing shows the Move handle and necessary controls, not the completed path or raster fill. Reopening loads actual persisted stroke parameters; completed paths are not rendered as
  highlighted influence areas. Actual Grade pixels always use the real evaluator.

### 4.2 Pointer routing specification

Use one Mask input adapter in the existing viewport handler surface, enabled only for creation
mode. Keep the current navigation adapter for ordinary viewing. A PointHandler may provide the
single canonical press/move stream while competing left-button navigation is disabled. If a
separate exclusive handler is needed, it arbitrates ownership only; it must not forward a second
sample stream. Prove delivery on the pinned Qt version instead of trusting old source comments.

Capture `(device identity, point id, sequence id)` at press. Pass plain values to the controller.
Keep the last valid sample and consume a final release position when supplied by the event; never
read a reset `(0,0)` handler position as a new final point. Every sequence accepts exactly one
terminal release or cancellation. Ignore duplicates and delayed callbacks after termination.

## 5. Ownership, reversible commands and history

The [algorithm design](mask_command_replay_and_project_cache_plan.md#3-参数化数据与单一所有者)
defines the new persistent source and inverse operations. R8 is never a before-state for Undo.

`EditorMaskCreationController` is an application service; a thin album-backend adapter exposes it
to QML. It owns mode, active handle and exact captured identities, not another writable document,
Grade selection or Brush collection. Reuse `EditorNodeController` selected-node authority.

Suggested calls: BeginCreation, SelectMask, BeginMaskInput, AppendMaskInput, FinishMaskInput,
CancelMaskInput, BeginMaskMove, UpdateMaskPlacement, FinishCreationMode, CancelCreationMode,
SetMaskField and RemoveMask. All change requests capture project/image/session/Version/NodeId/
MaskId/sequence identity. Brush commands also capture stable StrokeId. Admission is not commit.

### 5.1 Data and allowed independent state

| Data | Owner and necessity | Lifetime / rule |
| --- | --- | --- |
| Canonical stroke samples | Brush source; primary creation data | Persist in document/NM4 history; no per-frame copy of the stroke collection |
| Pending new input | Existing serial input owner | Ordered new samples/changed fields; move/share them into owner at consumption |
| Inverse command payload | NM4 history | New/removed stroke body or changed scalar fields; no bitmap before/after |
| Spatial index | Mask runtime owner | Derived IDs/spans/bounds only; rebuildable, no pixel history |
| GUI display projection | GUI publication of selected controls | Minimal fields, identity/revision-tagged, load-only |
| Grade R8 / source scratch | Existing native executor | Current result or algorithm output; writes wait for reader leases, no historic results |
| QSG geometry | Qt render-thread nodes | Control geometry; no document access from updatePaintNode |

### 5.2 One operation and one commit

Choosing a source arms creation without document mutation. First valid input creates a provisional
Mask; first Brush release commits AddMask with its first canonical stroke, later releases commit
AppendBrushStroke on the same MaskId. An analytic drag commits its final source fields; a Brush
move commits before/after translation, without rewriting any sample coordinates.

Enter or selecting another adjustment page after release only exits mode. Escape rolls back
unfinished commands and replays affected regions; earlier completed strokes remain. No-op creation or unchanged placement creates no
commit. A release equal to the last provisional value still seals the actual earlier change.

Undo removes/restores the relevant command data using NM4; deterministic evaluation updates the
current R8. Deleting all caches must not alter this behavior. Do not create a second QUndoStack,
per-stroke image checkpoint or full-source copy for every appended stroke.

### 5.3 Serial preview and failure behavior

GUI input moves controls and queues focused changes. The existing NM6 consumer applies one batch
at a safe owner boundary, invalidates exact dependencies, replays required coverage and executes
Interactive. Brush point ordering survives coalescing; absolute placement updates keep the latest
value per exact sequence. Live parameters/buffers cannot change during native readers.

After release, NM4 commits parameter data then requests Quality. Disk cache publication is an
independent, coalesced stable-slot write, not a prerequisite for history. Command persistence
failure restores source through inverse operations; native failure reports its real error; cache
write failure reports its own error and cannot erase an already committed command.

Rename, selection and future-tool size/strength change do not affect current pixels. Existing
placement/opacity/invert/feather changes do, invalidate appropriate coverage and Grade/downstream
results, and keep unaffected upstream image results according to NM6. This revision replaces
persistent per-source Mask R8 retention with one current Grade Mix coverage and necessary scratch;
it preserves QualityBase bypass after RAW Develop.

## 6. Coordinate and evaluator specification

### 6.1 One mapping chain

```text
pointer in handler's local logical coordinates
  -> map to the actual viewport item (Qt item mapping if parents differ)
  -> existing viewport inverse (letterbox, zoom, pan, presentation mapping)
  -> full rendered/edit image position
  -> resolved geometry render_to_reference
  -> normalized ReferenceSpace (divide by full_reference_extent)
```

The inverse chain maps analytic boundaries and sample support to overlay coordinates. Trace the
current viewport's source UV semantics before composing matrices: do not apply crop/orientation
twice if a current owner mapping already accounts for it. Add explicit named mapping methods and
round-trip tests at this owner boundary rather than a second QML coordinate system.

DPR is applied only where the existing mapper crosses logical/physical space. DetailPatch extent,
Interactive output size, viewport resize and panel animation must not redefine ReferenceSpace.
Use pixel centers `(x + 0.5, y + 0.5)` for raster/evaluator tests; continuous pointer positions do
not receive an extra half-pixel offset.

Invalid/zero extents or noninvertible transforms reject input. A press outside image content does
not create a Mask. During a captured drag allow off-image coordinates as the source validator
permits; clip raster writes and display rather than clamping every input point onto an edge.
Brush leaving/re-entering the image keeps the actual path, without painting along the border.
Cancel the active operation before a document Geometry change. Navigation between operations
updates display mapping while leaving stored parameters unchanged.

### 6.2 Radial: use the actual normalized-space ellipse

The existing evaluator forms normalized reference `q`, center `c`, and rotated local coordinates:

```text
u = ( cos(rotation)*(q.x-c.x) + sin(rotation)*(q.y-c.y)) / major_radius
v = (-sin(rotation)*(q.x-c.x) + cos(rotation)*(q.y-c.y)) / minor_radius
rho = sqrt(u*u + v*v)
inner = max(0, 1-inner_feather)
outer = 1+outer_feather
coverage = 1-clamp((rho-inner)/max(outer-inner, evaluator_epsilon), 0, 1)
```

Generate each boundary at its `rho` from the inverse equation, then apply the shared viewport
mapping. During creation and while an existing Radial is selected, show the base ellipse
`rho=1` and inner/outer feather boundaries with independent radius and feather handles.
Coincident lines render once; `inner=0` reduces to the center without invalid geometry.
Unselected masks do not retain editing guides. No area highlighting or filled feather bands.
Rotation is stored in radians. It occurs in normalized coordinates; a rotated ellipse on a
non-square image is not generally reproduced by QML rotation of a screen-space ellipse.

Center drag translates. Axis handles update radii in the inverse local frame. Rotation uses
normalized-space angle with continuous wrap handling. Feather handles update the corresponding
boundary and do not silently alter radius. Keep axis identity stable when dragging across center;
never swap handle identities merely because numeric radii cross. Respect validator limits.
The approved center-out drag sets positive x/y radii; zero-area input stays transient until valid.

### 6.3 Linear Gradient: direction and transition have separate roles

For normalized reference `q`, origin `o`, and normalized direction `n`:

```text
d = dot(q-o, n)
t = clamp(d / max(transition_distance, evaluator_epsilon) + 0.5, 0, 1)
coverage = start_value + (end_value-start_value)*t
```

The three mathematical guide loci are `d = -distance/2`, `d = 0`, and `d = +distance/2`.
During creation and while an existing Gradient is selected, show all three parallel lines
clipped to the visible photograph, using Geometry crop-style dual strokes and edge grips.
Do not connect their ends into a diamond/kite or closed polygon. Center line translates,
boundary grips change width at fixed center, and a separate direction control rotates.
No coverage fill or crop outside-dimming. Map control points
through the shared item transform. On a
non-square image, a normalized normal is not directly a screen normal. Handle hit tests and edits
must use the same mapping as evaluation.

Proposed initial drag from `a` to `b`: `origin=(a+b)/2`, `normal=normalize(b-a)`,
`transition_distance=length(b-a)`, `start_value=1`, `end_value=0`. The press side is fully covered;
the release side is uncovered. Move center to translate, direction handle to rotate, boundary
handle to change width with fixed center. Invert uses the existing Mask field. Do not invent a
second independent Linear feather parameter: transition distance is its existing softness control.

### 6.4 Parameterized Brush, deterministic replay and movement

Use [algorithm design Sections 3–5](mask_command_replay_and_project_cache_plan.md#4-确定性笔刷生成).
Persist ordered canonical samples, paint/erase, radius/strength/hardness and algorithm version.
Sample in the reference pixel metric; maintain arc-length spacing remainder across event batches.
Use the same sample source for any permitted initial-creation guide, never resample Qt input twice.

Proposed initial diameter is 2% of the shorter reference edge, strength/hardness 100%, source
feather 0. Size and strength changes affect subsequent samples, including ordered changes inside
an open stroke. Show size in reference pixels and strength in percent; do not relabel Mask opacity
as Brush strength. Pressure dynamics remain outside the approved scope.

Whole-Brush movement changes `placement_translation`; input after movement is stored through
its inverse in local reference coordinates. Do not repeatedly resample or shift the prior R8.
Repeated A→B→A movement restores the same source and pixels without blur. Brush Move and Paint
are explicit modes, so relocating a Brush cannot accidentally append a stroke.

Coverage replay uses the current parameter source, a span/tile index and the union of old/new
affected domains. Erase/max/quantization are not pixel-invertible: restore commands, then replay.
Reevaluate other Mask contributors when the maximum changes. Signed-distance feather keeps its
complete required domain; small source damage is not proof of a local distance update.

Host canonical Brush raster generation runs on the serial owner worker and feeds existing native
source evaluation; selected CUDA/OpenCL/Metal feather/Union/Mix remain mandatory. No substitute
backend or reduced-quality path is introduced.

## 7. One current R8 cache and project storage

The [algorithm/storage design](mask_command_replay_and_project_cache_plan.md#6-一个当前-r8-槽的准确范围)
is authoritative for cache identity, writer ownership, representation and clearing behavior.

- Persistent edit data is Brush samples/placement plus analytic fields and NM4 commands.
- Each `(ProjectUUID, ImageId, NodeId)` has one stable current Grade coverage disk slot;
  no filename keyed by stroke, commit, Version or content hash.
- Current output replacement respects active readers. Source R8/float/feather work is transient
  executor scratch, not an additional long-lived per-Mask cache.
- Coalesce cache writes at safe idle/save/close boundaries; no full R8 asset per stroke.
- Parameter commit succeeds independently of cache durability. A deleted/stale cache is rebuilt
  by the same complete algorithm; corrupt parameter data must still fail explicitly.
- ProjectService owns path and Keep/DeleteOnProjectClose policy. Provide Clear per project;
  only remove owned cache files, never source samples/history/RAW or another project's namespace.
- Path switching and Clear fence pending jobs, wait for reader/writer safety and persist settings
  through the project owner. Old jobs cannot recreate cleared files or write to a new project's root.
- Project packaging, Version and Paste work without R8; format validation rejects raster-only
  unsupported documents before any cache deletion. No automatic raster-to-path conversion.

**Movement chain:** input → focused placement fields → safe owner consume → replay affected
coverage → one current Grade R8 → native Mix → Interactive photograph. Release → one parameter
commit → Quality. QSG displays control positions and does not display affected-area highlighting.

## 8. State machine and interruption rules

Use the master states `Inactive`, `Selected`, `Creating`, `Editing`, `Painting`, `Settling`, `Failed`.
A state has a single controller owner. An open operation captures image/session generation,
Version/working-head generation, NodeId, MaskId, sequence id and mapping identity. Existing NM4
before-values stay with history; they are not copied into another session model.

| Event | Required behavior |
| --- | --- |
| Choose a creation tool | Resolve exact Grade; arm Creating; no AddMask/history/render until valid input |
| Select an existing Mask | Load owner data, selected row and overlay; pure selection makes no photo render |
| Valid press / move | Editing or Painting; immediate overlay; queue focused input for Interactive |
| Release with a valid open operation | Seal once, enter Settling, publish one commit and request Quality |
| Escape | Cancel unfinished input, restore before-state/remove provisional addition, zero commits; exit mode |
| Enter or select another adjustment page after a released operation | Leave mode without a duplicate commit and restore/select the requested ordinary page |
| New press while Settling | Keep the tool visibly unavailable for a new stroke until acknowledgement; preserve GUI responsiveness and never silently accept then drop a press |
| Unexpected grab loss, touch cancellation, window deactivation or workspace hiding | Cancel open operation before disabling/destroying its input adapter; do not fabricate release |
| Switch to another Mask/tool | Settle a valid current operation, then retarget; abandon an unstarted/degenerate creation |
| Ordinary adjustment input | Resolve active Mask operation, hide overlay before adjustment; keep hidden until explicit Mask re-entry |
| Image switch / Version checkout / Undo / Redo / owner deletion | Cancel unfinished operation first, then execute requested lifecycle command in order |
| Explicit delete of selected Mask | Cancel its open operation, then issue one NM4 removal; deleting a never-committed new Mask only cancels it |
| Viewport resize or DPR change during an open operation | Cancel the unfinished operation before installing the new mapping; keep earlier committed strokes and recompute the selected overlay |
| Pan/zoom during an open drawing operation | Keep mapping fixed for that operation; do not route those same points to navigation |
| Pan/zoom between operations | Use existing viewer navigation, recompute overlay placement, do not change Mask data |
| Application close | Resolve the open operation before existing Save/Discard close policy runs |
| Owner/commit/native failure | Restore by NM4 policy, show precise error, clear input ownership; no silent alternate path |

While creation mode owns input, keep the owning Color Grade selected, disable graph structure and
layout input, and keep the Masks controls available. Reenable graph interaction when mode ends.
If an external command changes selection regardless, validate generations and terminate old input;
never reinterpret it as an edit of the newly selected Grade.

Suggested navigation binding: middle-button or Space+drag between operations, wheel/pinch zoom
between operations; suppress ordinary left-button pan and double-click zoom while the Mask tool
owns that button. A second touch cannot become another brush or steal half an edit; classify it
before tool mutation. Keep mouse, stylus and touch device acceptance explicit.

Fencing is required at input enqueue, owner consume, cache-job completion, commit acceptance and frame
presentation. A content revision orders results within a session; it does not replace image/
Version/sequence identity. A late result must release its resources even when presentation rejects it.

## 9. UI, accessibility and integration behavior

Implement Section 2's approved routing. `EditorMasksContextPanel.qml` is the seventh Adjustment
Stack parameter page; it is not a floating overlay, hidden unused module, or second Mask manager.

The Node Mask drawer exposes selectable compact type rows and a per-row delete action.
Selection targets exact NodeId/MaskId and opens the existing source in the Mask parameter page;
it never calls a header creation action. Delete must not propagate into row or graph actions.
The Node drawer alone exposes type, selection, deletion, and supported management commands. The
Mask parameter page edits source-specific values plus the controls implemented by its phase. Use
exact IDs, stable row updates, and a panel-level `selectedMaskId` binding. No list rebuild or forced
scroll containment on click.
After deletion select the next row at the old position, otherwise previous, otherwise no selection.
Undo restores IDs; select a restored mask only according to an explicit current-session selection
rule, never by a reused index. Selection-only restore stays load-only.

Do not add explicit Done/Cancel actions in the viewer or parameter page. Enter confirms and returns
to the previous ordinary page; selecting any other enabled adjustment page confirms and opens that
page. Escape remains the cancellation route.

Use existing approved Brush/Radial/Gradient SVGs, including their documented 2 px exception.
Define required Mask colors/handle geometry/hit-area/antialias widths as AppTheme semantic tokens
and add their actual values to DESIGN.md in the implementation change. Suggested roles are the
master's `maskOverlayControlColor`, `maskOverlayControlOutlineColor`, `maskOverlayInactiveColor`. Do not add a coverage-area color for the revised Mask UI. No ad-hoc palette, Material controls, badges, pills or status dots.

QSG nodes are not accessible controls. Expose lightweight accessible proxies for the selected
source's finite set of handles, or equivalent named numeric controls plus handle selection. Do
not create a proxy for every dab. Tab reaches the selected controls; arrows
move the focused handle, with a named numeric alternative for precision. Proposed step is one
ReferenceSpace pixel, Shift ten; rotations and feather widths use explicitly labelled units.
Text-input Delete/Escape/Enter must first obey text editing/rename semantics; graph Delete must
never delete the owning Grade when a Mask handle has focus.

Numeric controls and keyboard edits use the same begin/update/finish/cancel service as pointer
edits. Keyboard repeat settles once on release or explicit confirmation and updates Interactive pixels
while moving a control. Test shortcut conflicts
with crop, graph, global Undo and existing viewer navigation. Register commands through the
existing shortcut owner rather than scattering key codes through QML.

Loading on initial construction, session rebind, panel re-entry, Undo/Redo and checkout never submits
input. Reject older projection revisions when newer local input is still pending. Restore ordinary
panel focus on mode exit. Theme switch updates all QSG colors; reduced motion makes mode/panel
transitions immediate, and input-following geometry is never animated behind the pointer.

## 10. Revised ordered implementation phases

本次改变 NM3 source、NM4 history 与 runtime cache，因此先完成数据和重放能力，再开放 viewer。
保留已完成的 NM7.1–NM7.7，新增 NM7.8 参数蒙版改进；原 NM7.8–NM7.14
顺延为 NM7.9–NM7.15。原 NM7.11（现 NM7.12）的部分 UI 已为测试提前接线，
不代表该阶段全部完成。不能把旧的 immutable asset 测试当作新格式验收。
保持总体 NM1→NM6→NM7→NM8 顺序。

| Phase | Result | Dependency |
| --- | --- | --- |
| NM7.1 | Source audit, numerical rules and new format boundary | NM6 production gates |
| NM7.2 | Parameterized Brush source and focused owner operations | NM7.1 |
| NM7.3 | Typed reversible stroke/placement history, WAL and Version | NM7.2 |
| NM7.4 | Canonical rasterization, spatial index and regional replay | NM7.2–NM7.3 |
| NM7.5 | Shared ReferenceSpace mapping, Brush placement and hit testing | NM7.4 |
| NM7.6 | QSG control-only retained rendering | NM7.5 |
| NM7.7 | Radial and Linear creation plus existing-mask movement | NM7.3, NM7.5–NM7.6 |
| NM7.8 | Parameter-mask controls, drawer selection/deletion and crop-style Gradient | NM7.7 |
| NM7.9 | Accumulating Brush creation, erase, tool settings and movement | NM7.4–NM7.6 |
| NM7.10 | Serial Interactive replay and one current Grade R8 result | NM7.7–NM7.9 |
| NM7.11 | Project-owned cache settings, writeback and cleanup service | NM7.3, NM7.10 |
| NM7.12 | Production Mask controls and project storage UI | NM7.11 |
| NM7.12R | Repair Brush tool state, pointer alignment, move frame, erase, drawing cost and default feather | NM7.5–NM7.12 |
| NM7.12RR | Complete continuous Brush input, stable Erase, GPU execution and measured viewer responsiveness in one phase | NM7.12R implementation |
| NM7.13 | Interruptions, late jobs and complete lifecycle | NM7.12, NM7.12R, NM7.12RR |
| NM7.14 | Native pixel, persistence, cache bounds and recovery qualification | NM7.13 |
| NM7.15 | Real viewer/package/performance qualification and NM8 handoff | NM7.14 |

Each phase records actual files/APIs, success and failure call chains, commands, executed test
counts and remaining gaps. Branch names describe the result, e.g. `feature/brush-command-replay`
or `feature/project-mask-cache`. New names below are proposals, not existing APIs.

### NM7.1 — Audit the revised source and format boundary

**Purpose:** distinguish the old implemented R8 asset path from the new parameter-owned product.

**Work:** record current commit/Qt/runtime; recheck NM6.8/6.9; trace Mask commands, source/Union
caches, MaskStore, history assets, project settings and package/save consumers. Inventory all
`MaskAssetKey` dependencies and their replacements. Fix raster/dab encoding, algorithm version,
source feather units and output sampling rules in the companion design. Enumerate the exact
project/schema versions accepted after this change; no automatic conversion of raster-only input.

**Files/APIs:** Section 3 source table, project service/version constants, NM4 batch applier,
plan executor/native Mask pass and existing tests.

**Primary chain:** saved source/history → validated document → evaluator/cache → Grade Mix.
**Failure chain:** unsupported parameter/format → load rejection → original files left intact.

**Tests:** characterize current asset references and reader lifetime; register future test targets;
record evidence, not a claim that parameterized source already exists.

**Exit:** ownership and replacement inventory complete; no unaccounted product dependency on old R8 history.

##### Phase NM7.1 completion record (2026-09-08)

**Status:** complete — current R8-asset Brush path characterized; encoding/format identities locked; parameterized source not claimed.

**Primary success call chain:**

```text
saved Brush JSON (asset_key + descriptor + feather_radius)
  -> PipelineDocument::FromJson / MaskModelFromJson
  -> CollectPersistentMaskAssetKeys / VerifyPersistentMaskAssets
  -> MaskStore::Load (shared immutable R8; GPU MaskTextureCache keyed by MaskAssetKey)
  -> native Mask evaluate / Union / Grade Mix
```

**Primary failure call chain:**

```text
unsupported format_version, unknown source kind, or corrupt MaskStore payload
  -> loader/store throws before owner mutation
  -> source file bytes unchanged; no raster-to-stroke conversion
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Current published history/document identities | `BrushSourceFormatBoundaryTest` | PASS |
| Brush JSON stores `asset_key` and omits stroke fields | `BrushSourceFormatBoundaryTest` | PASS |
| Current loader ignores extra stroke fields (parameterized source absent) | `BrushSourceFormatBoundaryTest` | PASS |
| Unsupported document format leaves the source file unchanged | `BrushSourceFormatBoundaryTest` | PASS |
| Unknown source kind rejects without mutating the live document | `BrushSourceFormatBoundaryTest` | PASS |
| Held `MaskStore` readers keep immutable pixels after host-cache eviction | `BrushSourceFormatBoundaryTest` | PASS |
| Ordinary adjustment path still rejects Mask targets | `BrushSourceFormatBoundaryTest` | PASS |
| Dab coverage, round-half-up R8, paint/erase max/min | `BrushSourceFormatBoundaryTest` | PASS |
| Canonical raster long-edge cap 4096 | `BrushSourceFormatBoundaryTest` | PASS |
| Feather radius matches native texel scale | `BrushSourceFormatBoundaryTest` | PASS |
| Packed R8 bilinear sample matches native center filter | `BrushSourceFormatBoundaryTest` | PASS |
| 65 later parameterized-Brush test names catalogued | `BrushSourceFormatBoundaryTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --preset win_debug`
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target BrushSourceFormatBoundaryTest`
`ctest --test-dir build/debug -R BrushSourceFormatBoundaryTest --output-on-failure`

Suite totals: `12/12` PASS. Date / HEAD `6c0dae76` / Windows MSVC `win_debug` / Qt 6.9.3 / CUDA Toolkit 12.8 (no GPU tests in this phase).

**Checklist / exit condition:** inventory is in companion Sections 4.4 and 8.1–8.2; current product still depends on `MaskAssetKey` for Brush, which is recorded rather than removed.

**LOC note (grill-code-review):** `brush_raster_encoding.hpp` ~138, `brush_raster_encoding.cpp` ~152, `brush_source_format_boundary_test.cpp` ~290. No split required.

**Residual gaps:** NM7.2 must change `BrushMaskSource` to strokes; NM7.3 must switch the version gate to the Section 8.1 identities and reject raster-only input. Current loaders still accept document format 5 with `asset_key` and ignore unknown source keys. Adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned. Catalogued later test names are not yet executing behavior cases.

### NM7.2 — Add parameterized Brush data and owner operations

**Purpose:** persist the information needed to recreate a Brush without any cache file.

**Work:** change `BrushMaskSource` to ordered immutable canonical stroke bodies, StrokeId, algorithm
version and placement; keep Radial/Linear definitions. Add validated append/remove/insert stroke
and set-translation owner operations. Enforce exact NodeId/MaskId/StrokeId and before-revision;
update dirty metadata once after successful mutation. Parameter read projections remain minimal.

**Files/APIs:** `mask_model.hpp/.cpp`, `color_grade_node_model.hpp/.cpp`, document serialization;
proposed `brush_stroke.hpp` / `brush_mask_commands.hpp`; include defining headers.

**Primary chain:** exact source operation → validate complete input → Grade owner → revision.
**Tests:** `BrushSourceRoundTripsWithoutRasterFiles`, `BrushAppendKeepsExistingStrokeIds`,
`InvalidStrokeDoesNotPartiallyMutateSource`, `BrushTranslationDoesNotCopyOrRewriteSamples`.

**Exit:** one Brush per new Grade workflow; source can be read independently of `MaskStore`.

##### Phase NM7.2 completion record (2026-09-08)

**Status:** complete — parameterized Brush strokes, versions, and placement are owned by
`ColorGradeNodeModel`; JSON round-trips without `MaskStore`. Raster-only `asset_key` encoding
remains for existing documents until NM7.3.

**Primary success call chain:**

```text
AppendBrushStroke / InsertBrushStroke / RemoveBrushStroke / SetBrushTranslation
  -> ColorGradeNodeModel (exact NodeId, MaskId, StrokeId, before-revision)
  -> validate complete stroke or translation on a candidate source
  -> commit source on the Grade Mask + one MaskContentRevision bump
  -> MaskModelToJson writes strokes/placement (no asset_key when payload is parameterized)
```

**Primary failure call chain:**

```text
wrong NodeId / MaskId / kind / revision, duplicate StrokeId, NaN/empty samples,
unsupported algorithm version, or stale translation before-value
  -> throw before live source assignment
  -> Mask list, stroke bodies, and revision unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `BrushSourceRoundTripsWithoutRasterFiles` | `BrushParameterizedSourceTest` | PASS |
| `BrushAppendKeepsExistingStrokeIds` | `BrushParameterizedSourceTest` | PASS |
| `InvalidStrokeDoesNotPartiallyMutateSource` | `BrushParameterizedSourceTest` | PASS |
| `BrushTranslationDoesNotCopyOrRewriteSamples` | `BrushParameterizedSourceTest` | PASS |
| Insert/remove keep remaining sample bodies | `BrushParameterizedSourceTest` | PASS |
| Unsupported algorithm/version JSON rejected | `BrushParameterizedSourceTest` | PASS |
| Asset-key documents still omit stroke fields | `BrushSourceFormatBoundaryTest` | PASS |
| Malformed stroke fields on asset Brush rejected | `BrushSourceFormatBoundaryTest` | PASS |
| Typed batch canonical dump matches stored expected JSON | `PipelineEditBatchTest` | PASS |
| Grade Mask JSON round-trip and header hygiene | `GpuDagModelGraphTest` | PASS |
| Result content key still distinguishes Mask edits | `GpuDagRawInputTest` | PASS |
| Persistent asset-key collection still omits empty Brush | `PipelineHistoryApplierTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --preset win_debug`
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target BrushParameterizedSourceTest --target BrushSourceFormatBoundaryTest --target GpuDagModelGraphTest --target PipelineEditBatchTest`
`ctest --test-dir build/debug -R "BrushParameterizedSourceTest|BrushSourceFormatBoundaryTest|GpuDagModelGraphTest|PipelineEditBatchTest" --output-on-failure`
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest --target PipelineHistoryApplierTest`
`ctest --test-dir build/debug -R "GpuDagRawInputTest|PipelineHistoryApplierTest" --output-on-failure`

Suite totals: `6/6` `BrushParameterizedSourceTest` PASS; `12/12` `BrushSourceFormatBoundaryTest` PASS;
`101/101` combined first filter PASS; `151/151` `GpuDagRawInputTest`+`PipelineHistoryApplierTest` PASS.
Date / HEAD `f3d93ca2` (working tree) / Windows MSVC `win_debug` / Qt 6.9.3 / CUDA Toolkit 12.8
(no GPU tests in this phase).

**Checklist / exit condition:** parameterized Brush source is readable without `MaskStore`; append
targets an existing MaskId (one accumulating Brush for new workflows). Multiple existing Brush
Masks stay independent. Raster-only `asset_key` JSON is still accepted at document format 5.

**LOC note (grill-code-review):** `brush_stroke.hpp` 151, `brush_stroke.cpp` 193,
`brush_mask_commands.hpp` 71, `mask_model.hpp` 215, `mask_model.cpp` 533,
`color_grade_node_model.hpp` 249, `color_grade_node_model.cpp` 503,
`brush_parameterized_source_test.cpp` 305. No split required.

**Residual gaps:** NM7.3 must add typed history/WAL/Version operations and the Section 8.1
version gate, and reject raster-only Brush input. Native Mask passes still load `MaskStore` by
`asset_key` when present. Ordinary adjustment writes still reject Mask targets with the existing
“until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.3 — Extend NM4 reversible history and persistence

**Purpose:** Undo/Redo changes parameters, not references to per-stroke raster files.

**Work:** implement typed Append/Remove/InsertBrushStroke and SetBrushTranslation with exact inverse
fields; first stroke is one AddMask commit. Extend canonical payload, validators, WAL recovery,
materializer, checkpoint, Version checkout and Paste remapping. History stores only new/removed
sample bodies or changed fields, not all earlier strokes with every append. Reuse NM4 structural
inverse payloads for Mask/Grade deletion. Update project/package schema gate together.

**Files/APIs:** `pipeline_edit_batch`, `pipeline_history_applier`, `pipeline_document_history`,
history materializer, transfer/package and current project-version owner.

**Primary chain:** final input → typed command → WAL/history success → Version-replayable source.
**Failure chain:** persistence rejection → inverse through same owner → no new HEAD or half source.

**Tests:** `StrokeUndoRedoRestoresCommandOrderWithoutR8`, `FirstBrushStrokeCreatesOneCommit`,
`BrushMoveUndoRestoresExactTranslation`, `AppendHistoryDoesNotRepeatEarlierSamples`,
`ParameterizedBrushSurvivesWalRecoveryAndPaste`, `RasterOnlyFormatIsRejectedBeforeCacheCleanup`.

**Exit:** delete all new cache files and history/Version source restoration still succeeds.

##### Phase NM7.3 completion record (2026-09-08)

**Status:** complete — typed Append/Remove/InsertBrushStroke and SetBrushTranslation history, first stroke as one AddMask commit, WAL/Version/Paste remapping without R8 keys, and the companion Section 8.1 project/schema cutover (old identities rejected; raster-only Brush JSON rejected).

**Primary success call chain:**

```text
settled stroke or placement
  -> MakeAddMaskBatch (first stroke, parameterized Brush JSON)
     or MakeAppendBrushStrokeBatch / MakeRemoveBrushStrokeBatch
     / MakeInsertBrushStrokeBatch / MakeSetBrushTranslationBatch
  -> ApplyPipelineEditBatch (live MaskContentRevision; exact inverse fields)
  -> MiniGitWorkingHistory::AppendEdit -> WAL record
  -> Version first-parent replay / ReplayPipelineDocumentFromRoot
     / CaptureDocumentTransfer + PrepareDocumentPaste (StrokeId remap)
  -> parameterized Brush source restored after deleting disposable .r8mask files
```

**Primary failure call chain:**

```text
raster-only Brush JSON (asset_key / width / height / reference_bounds),
missing parameterized fields, or old project/document/batch/WAL identity
  -> BrushFromJson / PipelineDocument::FromJson / batch/WAL format gate
  -> source and cache bytes left unchanged; no Apply; no new HEAD
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `StrokeUndoRedoRestoresCommandOrderWithoutR8` | `PipelineHistoryApplierTest` | PASS |
| `FirstBrushStrokeCreatesOneCommit` | `PipelineHistoryApplierTest` | PASS |
| `BrushMoveUndoRestoresExactTranslation` | `PipelineHistoryApplierTest` | PASS |
| `AppendHistoryDoesNotRepeatEarlierSamples` | `PipelineEditBatchTest` | PASS |
| `ParameterizedBrushSurvivesWalRecoveryAndPaste` | `ParameterizedBrushHistoryPersistenceTest` | PASS |
| `RasterOnlyFormatIsRejectedBeforeCacheCleanup` | `BrushSourceFormatBoundaryTest` | PASS |
| Append undo restores earlier stroke without raster files | `EditorSessionHistoryPortTest` | PASS |
| Checkout restores strokes after cache deletion | `EditorSessionHistoryPortTest` | PASS |
| Project reopen restores DAG, versions, history, and stroke bodies | `EditorSessionHistoryPortTest` | PASS |
| Paste remaps NodeId/MaskId/StrokeId without MaskStore | `DocumentTransferTest` | PASS |
| Published identities 0.6.0 / 6 / 4 / 3 / 5 / v4; `kMaskImplementationVersion` stays 3 | `PipelineDocumentCheckpointTest`, `CommitGraphTest` | PASS |
| Expected document/batch dumps match parameterized Brush JSON | `PipelineDocumentCheckpointTest`, `PipelineEditBatchTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineEditBatchTest --target BrushSourceFormatBoundaryTest --target BrushParameterizedSourceTest --target PipelineHistoryApplierTest --target DocumentTransferTest --target ParameterizedBrushHistoryPersistenceTest --target PipelineDocumentCheckpointTest --target GpuDagModelGraphTest --target EditorSessionHistoryPortTest --target CommitGraphTest`
`ctest --test-dir build/debug --output-on-failure -R "PipelineEditBatchTest|BrushSourceFormatBoundaryTest|BrushParameterizedSourceTest|PipelineHistoryApplierTest|DocumentTransferTest|ParameterizedBrushHistoryPersistenceTest|PipelineDocumentCheckpointTest|GpuDagModelGraphTest|EditorSessionHistoryPortTest|^CommitGraphTest$"`

Suite totals: `219/219` PASS. Date / working tree on `feature/brush-stroke-history-persistence` (base `ef109e52`) / Windows MSVC `win_debug` / Qt 6.9.3 / CUDA Toolkit 12.8 (no GPU tests in this phase).

**Checklist / exit condition:** all required tests PASS; deleting dummy cache files still restores stroke order, IDs, sample bodies, and translation from history/WAL/Version/Paste.

**LOC note (grill-code-review):** After NM7.3 the batch codec was split NM6P-style (named module APIs, not a method-file split): `pipeline_edit_json.cpp` 453 (nested JSON collection), `pipeline_edit_change_validate.cpp` 353, `pipeline_edit_change_json.cpp` 581 (encode/decode), `pipeline_edit_batch.cpp` 423 (Make, Validate, CanonicalJSON, FromJSON, and projection). Headers: `pipeline_edit_batch.hpp` 464, `pipeline_edit_json.hpp` 88, `pipeline_edit_change.hpp` 56. Inverse/apply remains `pipeline_history_applier.cpp` 797. `pipeline_document_history.cpp` 593, `document_transfer.cpp` 625, `mask_model.cpp` 464, `parameterized_brush_history_persistence_test.cpp` 153, `pipeline_edit_batch_test.cpp` 735.

**Residual gaps:** native Mask evaluation still loads `MaskStore` when in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned. NM7.4 regional replay is not started. Project Mix cache service is NM7.11. `ReplaceMaskAsset` remains in the typed batch surface but cannot validate parameterized or raster-only Brush JSON through the current source gate.

### NM7.4 — Implement deterministic Brush replay and spatial indexing

**Purpose:** recover pixels from commands efficiently without raster history.

**Work:** implement companion Sections 4–5: canonical samples, arc-length dabs, integer max/min,
ordered span index, dirty old/new supports, surviving-source Union recomputation and full-required
feather domain. Initialize each reconstructed region from its defined base. Do not persist source
R8 per stroke or keep hidden raster checkpoints. Fixed finite-value and quantization rules are shared.

**Files/APIs:** proposed `edit/mask/brush_rasterizer`, `brush_spatial_index`, source geometry helpers;
existing native feather and shared execution interfaces where needed.

**Primary chain:** reversible source mutation → affected bounds → ordered candidate spans →
recomputed source → effective coverage → current Grade result.

**Tests:** `RegionalBrushReplayMatchesFullEvaluation`, `EraseUndoRestoresEarlierPaint`,
`RemovingUnionMaximumPreservesOtherMasks`, `BrushEventGroupingPreservesCanonicalPixels`,
`FeatherRebuildMatchesCompleteDistanceEvaluation`.

**Exit:** independent full-evaluation oracle passes paint/erase/overlap/translate/Undo/Redo; index
uses only IDs/spans/bounds. The simplified prototype is not a substitute for these production tests.

##### Phase NM7.4 completion record (2026-09-08)

**Status:** complete — host canonical Brush rasterization, local-space spatial index, regional
source replay, full-required signed-distance feather, and Grade Mix union without raster history.

**Primary success call chain:**

```text
reversible Brush/analytic mutation (Append/Remove stroke or SetBrushTranslation)
  -> BrushSourceOutputTexelSupport / EffectiveMaskTexelSupport (old union new)
  -> BrushSpatialIndex::QueryOutput (StrokeId + sample spans + local bounds)
  -> BrushRasterizer::ReplayRegion (b0 = 0 in dirty texels; paint max / erase min)
  -> BrushSignedDistanceFeather::Apply when feather_radius > 0 (complete field)
  -> GradeMaskCoverage Mix: empty list 255, all-disabled 0, else max(effective)
```

**Primary failure call chain:**

```text
unbound Brush index, unsupported algorithm version, or invalid sample encoding
  -> throw before Mix commit
  -> previous Mix region restored from the dirty-rectangle backup
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RegionalBrushReplayMatchesFullEvaluation` | `BrushRegionalReplayTest` | PASS |
| `EraseUndoRestoresEarlierPaint` | `BrushRegionalReplayTest` | PASS |
| `RemovingUnionMaximumPreservesOtherMasks` | `BrushRegionalReplayTest` | PASS |
| `BrushEventGroupingPreservesCanonicalPixels` | `BrushRegionalReplayTest` | PASS |
| `FeatherRebuildMatchesCompleteDistanceEvaluation` | `BrushRegionalReplayTest` | PASS |
| Dense/grouped pointer events keep remainder | `BrushCanonicalSamplerTest` | PASS |
| Winding stroke index does not mark interior tiles | `BrushSpatialIndexTest` | PASS |
| Translation query does not rebuild local spans | `BrushSpatialIndexTest` | PASS |
| Missing index leaves Mix unchanged | `BrushRegionalReplayTest` | PASS |
| Existing parameterized Brush JSON/owner tests | `BrushParameterizedSourceTest`, `BrushSourceFormatBoundaryTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target BrushCanonicalSamplerTest --target BrushSpatialIndexTest --target BrushRegionalReplayTest`
`ctest --test-dir build/debug --output-on-failure -R "BrushCanonicalSamplerTest|BrushSpatialIndexTest|BrushRegionalReplayTest"`
`ctest --test-dir build/debug --output-on-failure -R "BrushParameterizedSourceTest|BrushSourceFormatBoundaryTest"`

Suite totals: `14/14` NM7.4 binaries PASS; `19/19` existing Brush source tests PASS.
Date / working tree on `feature/brush-command-replay` (base `54975ddb`) / Windows MSVC `win_debug` / Qt 6.9.3 / CUDA Toolkit 12.8 (host Mix only; no GPU tests in this phase).

Independent oracle: `alcedo_studio/tests/edit/mask/brush_replay_oracle.hpp` (per-texel dab walk and exhaustive signed distance). Production uses the spatial index, regional stamp, and separable Euclidean distance.

**Checklist / exit condition:** all required tests PASS; index stores StrokeId, sample spans, and local bounds only; Mix/source R8 is current coverage, not a per-stroke checkpoint.

**LOC note (grill-code-review):** `brush_source_geometry.hpp` 112 / `.cpp` 206, `brush_canonical_sampler.hpp` 92 / `.cpp` 137, `brush_spatial_index.hpp` 105 / `.cpp` 140, `brush_rasterizer.hpp` 75 / `.cpp` 128, `brush_signed_distance.hpp` 59 / `.cpp` 167, `grade_mask_coverage.hpp` 99 / `.cpp` 253, `brush_replay_oracle.hpp` 190, `brush_regional_replay_test.cpp` 287. No split required.

**Residual gaps:** host Mix is not yet the Interactive/Quality native Mask pass (NM7.10) or the project Mix-cache slot (NM7.11). Shared ReferenceSpace pointer mapping is NM7.5. Native evaluation still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.5 — Implement shared mapping and parameterized movement

**Purpose:** input, controls and native pixels refer to the same image locations.

**Work:** trace item/logical → viewport → resolved reference mapping; avoid double crop/orientation.
Add Brush translation with inverse mapping for newly added samples after movement. Use old/new
supports for damage; preserve reference metric and stroke-local coordinates. Define logical handle
hit areas, off-image movement, invalid transforms and resize/DPR interruption.

**Files/APIs:** `editor_interaction_controller`, `viewport_mapper`, `resolved_render_geometry`,
proposed `ui/edit_viewer/mask_edit_geometry`; application movement commands.

**Primary chain:** item drag → reference delta → absolute placement/center/origin → owner replay
and forward mapping of controls.

**Tests:** `MaskReferenceMappingRoundTripsAcrossZoomPanAndDpr`,
`BrushMoveThenDrawUsesTranslatedLocalCoordinates`, `RepeatedBrushMoveDoesNotBlurCoverage`,
`DetailPatchDoesNotChangeMaskReferenceSpace`, `InvalidGeometryRejectsMaskPress`.

**Exit:** normalized mapping error ≤ 1e-5 and item round-trip error ≤ 0.25 logical px over documented
fixtures; move A→B→A recovers exact parameters and expected pixels.

##### Phase NM7.5 completion record (2026-09-08)

**Status:** complete — shared item/logical → photograph UV → `render_to_reference` mapping,
Brush local/world placement, logical handle hit testing, and resize/DPR mapping identity.

**Primary success call chain:**

```text
item/logical pointer (QQuickItem space)
  -> ViewportMapper letterbox/zoom/pan (RoiFrame expands displayed UV through source ROI)
  -> photograph UV * render_extent
  -> ResolvedRenderGeometry::render_to_reference
  -> ReferenceSpace pixels / normalized q
  -> BrushLocalFromReference / BrushPlacementForReferenceDrag
  -> ColorGradeNodeModel::SetBrushTranslation or BrushCanonicalSampler
  -> regional Mix replay; MapReferenceToItem for control positions
```

**Primary failure call chain:**

```text
zero extents, noninvertible render_to_reference, unset displayed geometry,
or press with photograph UV outside [0, 1]
  -> MaskEditGeometry::MapItemToReference returns empty
  -> no Mask press / no placement command
  -> MappingChanged(resize, DPR, geometry) cancels unfinished input before a new mapping is used
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MaskReferenceMappingRoundTripsAcrossZoomPanAndDpr` | `MaskEditGeometryTest` | PASS |
| `DetailPatchDoesNotChangeMaskReferenceSpace` | `MaskEditGeometryTest` | PASS |
| `InvalidGeometryRejectsMaskPress` | `MaskEditGeometryTest` | PASS |
| `BrushMoveThenDrawUsesTranslatedLocalCoordinates` | `BrushPlacementMappingTest` | PASS |
| `RepeatedBrushMoveDoesNotBlurCoverage` | `BrushPlacementMappingTest` | PASS |
| Logical handle radius constant across zoom | `MaskEditGeometryTest` | PASS |
| Existing Brush replay/index/sampler | `BrushRegionalReplayTest`, `BrushCanonicalSamplerTest`, `BrushSpatialIndexTest` | PASS |
| Crop overlay draft routing unchanged | `EditorGeometryOverlayDraftRoutingTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"`
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target MaskEditGeometryTest --target BrushPlacementMappingTest`
`ctest --test-dir build/debug --output-on-failure -R "MaskEditGeometryTest|BrushPlacementMappingTest"`
`ctest --test-dir build/debug --output-on-failure -R "BrushRegionalReplayTest|BrushCanonicalSamplerTest|EditorGeometryOverlayDraftRoutingTest|BrushSpatialIndexTest"`

Suite totals: `6/6` NM7.5 binaries PASS; `15/15` related Brush/overlay tests PASS.
Date / working tree on `feature/mask-reference-mapping` (base `dab6a8e5`) / Windows MSVC `win_debug` / Qt 6.9.3 / CUDA Toolkit 12.8 (host mapping only; no GPU tests in this phase).

**Checklist / exit condition:** required tests PASS. Normalized error ≤ 1e-5 and item round-trip ≤ 0.25 logical px on landscape/portrait, zoom/pan, DPR 1/1.25/1.5/2, and cropped/rotated fixtures. A→B→A restores translation, sample-body pointers, and Mix bytes. DetailPatch ROI overlay does not change FullFrame ReferenceSpace; Interactive `max_edge` does not redefine `full_reference_extent`.

**LOC note (grill-code-review):** `mask_edit_geometry.hpp` 183 / `.cpp` 258, `brush_placement.hpp` 79, `mask_edit_geometry_test.cpp` 270, `brush_placement_mapping_test.cpp` 151, `editor_interaction_controller.cpp` 1027. Mapping math lives in `MaskEditGeometry`; the controller only stores displayed photograph geometry and routes. Do not add Mask business rules to the controller. No split required this phase.

**Residual gaps:** QSG controls are NM7.6. Radial/Linear creation and existing-mask movement UI are NM7.7. Accumulating Brush paint/erase UI is NM7.9. Session does not yet publish live `ResolvedRenderGeometry` into `setDisplayedMaskGeometry` (NM7.10). Open-operation cancel on `MappingChanged` is NM7.13. Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.6 — Implement retained QSG controls without affected-area highlighting

**Purpose:** expose editable controls while the actual image supplies coverage feedback.

**Work:** keep `EditorOverlayItem`/retained nodes; draw selected handles, direction/connector lines,
Brush cursor and allowed creation guides. Existing-mask editing emits zero area-fill/heatmap/settled
stroke-path geometry. Add theme/control tokens to AppTheme and DESIGN together. Reuse old nodes,
triangle strokes and explicit antialiasing; preserve crop/ROI layer behavior and Qt 6.9 support.

**Files/APIs:** overlay item + focused geometry helpers, AppTheme, DESIGN, UI geometry/window tests.
**Primary chain:** local control change → `update()` → `updatePaintNode(oldNode)` → Qt frame.

**Tests:** `ExistingMaskEditHasControlsAndNoCoverageFill`, `MaskControlsUpdateWhileRenderIsHeld`,
`MaskOverlayReusesNodesForMovement`, `MaskControlsRecreateAfterSceneInvalidation`,
`MaskControlsKeepLogicalSizeAndThemeColors`.

**Exit:** real accelerated window captures show no mask-area tint while dragging an existing Mask;
no live pipeline lock or raster I/O inside Qt synchronization.

##### Phase NM7.6 completion record (2026-09-08)

**Status:** complete — retained QSG Mask controls (handles, connectors, Brush cursor, initial-creation
guides) with zero coverage-fill geometry; AppTheme/DESIGN tokens; crop/ROI overlay unchanged.

**Primary success call chain:**

```text
GUI publishes MaskOverlayDisplay (item-space handles from MaskOverlayLayout + MaskEditGeometry)
  -> EditorOverlayItem::setMaskOverlayDisplay
  -> BuildMaskOverlaySceneGeometry (triangle lists, premultiplied AA fringes)
  -> update()
  -> updatePaintNode(oldNode) reuses QSGGeometryNode children
  -> Qt Quick frame (controls only; photograph remains EditorViewportItem)
```

**Primary failure call chain:**

```text
Hidden display, degenerate Radial radii, or unmappable controls
  -> empty MaskOverlaySceneGeometry (coverage_fill_vertex_count stays 0)
  -> UpsertPremultipliedTriangleNode removes stale child nodes
  -> no pipeline lock, MaskStore I/O, or document mutation
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ExistingMaskEditHasControlsAndNoCoverageFill` | `MaskOverlayControlTest` | PASS |
| `MaskControlsUpdateWhileRenderIsHeld` | `MaskOverlayControlTest` | PASS |
| `MaskOverlayReusesNodesForMovement` | `MaskOverlayControlTest` | PASS |
| `MaskControlsRecreateAfterSceneInvalidation` | `MaskOverlayControlTest` | PASS |
| `MaskControlsKeepLogicalSizeAndThemeColors` | `MaskOverlayControlTest` | PASS |
| Degenerate Radial yields empty geometry | `MaskOverlayControlTest` | PASS |
| Radial creation outline chord error | `MaskOverlayControlTest` | PASS |
| Hidden display clears Mask nodes | `MaskOverlayControlTest` | PASS |
| Existing mapping / crop draft routing | `MaskEditGeometryTest`, `BrushPlacementMappingTest`, `EditorGeometryOverlayDraftRoutingTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"`
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target MaskOverlayControlTest`
`ctest --test-dir build/debug --output-on-failure -R "MaskOverlayControlTest"`
`ctest --test-dir build/debug --output-on-failure -R "MaskEditGeometryTest|EditorGeometryOverlayDraftRoutingTest|BrushPlacementMappingTest"`

Suite totals: `8/8` `MaskOverlayControlTest` PASS; `7/7` related mapping/overlay tests PASS.
Date / working tree on `feature/mask-qsg-controls` (base `e01bafdb`) / Windows MSVC `win_debug` / Qt 6.9.3 (`D:/misc/Qt/6.9.3/msvc2022_64`) / CUDA Toolkit 12.8. Window grabs used Qt `offscreen` QPA scene graph, not a native desktop GPU surface.

**Checklist / exit condition:** required tests PASS. Existing Brush/Radial/Linear displays emit handles and connectors with `coverage_fill_vertex_count == 0`. Offscreen `grabWindow` of an existing Radial interior matches the window color (no Mask-area tint). Overlay geometry rebuilds from the published display while a dummy render thread stays busy. `updatePaintNode` reuses Mask child nodes on movement and reconstructs them on a new window from current display state. Handle draw radius is constant across zoom/DPR; Brush cursor radius scales with the mapping. Theme tokens match DESIGN.md.

**LOC note (grill-code-review):** `mask_overlay_geometry.hpp` 155 / `.cpp` 287, `mask_overlay_layout.hpp` 101 / `.cpp` 372, `editor_overlay_item.hpp` 165 / `.cpp` 612, `mask_overlay_control_test.cpp` 402. Layout owns evaluator-inverse handle placement; geometry owns triangle tessellation. Overlay item only copies published triangles onto retained nodes. No split required.

**Residual gaps:** NM7.7 Radial/Linear creation and existing-mask movement UI (controller + Interactive pixels). NM7.9 accumulating Brush paint/erase UI. Session does not yet publish live Mask overlay display (still NM7.10). Accessible handle proxies are NM7.12. Offscreen QPA grabs are not a packaged D3D11/Metal desktop capture (NM7.15). Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.7 — Complete analytic creation and movement

**Purpose:** Radial and Linear support both first drawing and later real-time relocation.

**Work:** center-out Radial; center/axes/rotation/inner/outer feather controls; Linear center/direction/
transition controls and endpoint convention. Existing move keeps source shape fields fixed except
center/origin. Begin/update/release/cancel use common exact-target owner calls. Do not wait until
release to update the photographed result.

**Files/APIs:** creation controller, analytic geometry, Grade owner and NM4 field/source commands.
**Primary chain:** drag source → provisional fields → Interactive → one settled command → Quality.

**Tests:** `ExistingRadialMoveUpdatesInteractivePixelsBeforeRelease`,
`ExistingGradientMovePreservesDirectionAndUpdatesInteractivePixels`,
`RadialFeatherControlsMatchEvaluator`, `DegenerateAnalyticCreationCreatesNoCommit`,
`EscapeRestoresAnalyticSourceWithoutCommit`.

**Exit:** both shapes move with actual native preview and control-only QSG on non-square images.

##### Phase NM7.7 completion record (2026-09-08)

**Status:** complete — Radial/Linear center-out creation, existing-mask movement, and handle
edits apply provisional Grade source fields before release; one NM4 AddMask or
ReplaceMaskSource commit on settle; Escape restores with zero commits.

**Primary success call chain:**

```text
item/logical pointer (MaskEditGeometry::MapItemToReference)
  -> EditorMaskCreationController::BeginMaskInput / BeginMaskMove / AppendMaskInput
  -> RadialFromCenterOut / LinearFromEndpoints / ApplyAnalyticMaskHandle
  -> ColorGradeNodeModel::AddMask (first valid creation) or ReplaceMaskSource
  -> Interactive Mix callback (GradeMaskCoverage::EvaluateFull)
  -> FinishMaskInput
  -> MakeAddMaskBatch / MakeReplaceMaskSourceBatch
  -> MiniGitWorkingHistory::AppendEdit
  -> Quality requested (no second Apply; live already holds after values)
```

**Primary failure call chain:**

```text
degenerate zero-area creation, unchanged placement, or Escape/Cancel
  -> no AddMask, or RestoreLive (RemoveMask / ReplaceMaskSource to before JSON)
  -> history head unchanged; Grade source matches the captured before-state
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ExistingRadialMoveUpdatesInteractivePixelsBeforeRelease` | `AnalyticMaskCreationTest` | PASS |
| `ExistingGradientMovePreservesDirectionAndUpdatesInteractivePixels` | `AnalyticMaskCreationTest` | PASS |
| `RadialFeatherControlsMatchEvaluator` | `AnalyticMaskCreationTest` | PASS |
| `DegenerateAnalyticCreationCreatesNoCommit` | `AnalyticMaskCreationTest` | PASS |
| `EscapeRestoresAnalyticSourceWithoutCommit` | `AnalyticMaskCreationTest` | PASS |
| Valid Radial creation commits once; creating overlay has no coverage fill | `AnalyticMaskCreationTest` | PASS |
| Center-out radii stay positive and unswapped | `AnalyticMaskEditTest` | PASS |
| Linear endpoints set origin, unit normal, and width | `AnalyticMaskEditTest` | PASS |
| Rotation unwraps across ±π | `AnalyticMaskEditTest` | PASS |
| Existing mapping / overlay / Brush placement | `MaskEditGeometryTest`, `MaskOverlayControlTest`, `BrushPlacementMappingTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AnalyticMaskEditTest --target AnalyticMaskCreationTest`
`ctest --test-dir build/debug --output-on-failure -R "AnalyticMaskEditTest|AnalyticMaskCreationTest"`
`ctest --test-dir build/debug --output-on-failure -R "MaskOverlayControlTest|MaskEditGeometryTest|BrushPlacementMappingTest"`

Suite totals: `9/9` NM7.7 binaries PASS; `14/14` related mapping/overlay tests PASS.
Date / working tree on `feature/analytic-mask-creation-movement` (base `6eb68e4b`) / Windows MSVC `win_debug` / Qt 6.9.3 (`D:/misc/Qt/6.9.3/msvc2022_64`) / CUDA Toolkit 12.8. Interactive Mix is host `GradeMaskCoverage` using the native analytic equations; no GPU Mask pass in this phase.

**Checklist / exit condition:** required tests PASS. Existing Radial/Linear center/origin drags update Mix R8 before `AppendEdit`. Shape fields other than center/origin stay fixed. Independent plan-equation coverage at sampled texels matches Mix within 1 R8 code. Degenerate creation and Escape publish no history. Existing and creating overlays on a non-square 64×32 photograph keep `coverage_fill_vertex_count == 0`.

**LOC note (grill-code-review):** `analytic_mask_edit.hpp` 179 / `.cpp` 310, `editor_mask_creation_controller.hpp` 224 / `.cpp` 611, `analytic_mask_edit_test.cpp` 68, `analytic_mask_creation_test.cpp` 447. Geometry owns evaluator-inverse handle math; the controller owns mode, identities, provisional Grade writes, and settle/cancel. No split required.

**In-app test wiring (same branch, after owner-path completion):** Radial/Gradient header buttons, viewport left-button routing, control-only overlay, Escape, and session enqueue/consume are connected. Overlay uses local draft geometry plus identity photograph mapping when `ResolvedRenderGeometry` is unpublished. Interactive frames reuse `EditorRenderReason::InteractiveAdjustment` with `live_parameters_applied`; settle uses `SettledMaskEdit`.

```text
EditorAdjustmentHeader beginRadial/beginLinear
  -> EditorMaskCreationAdapter (item → MaskCreationSample, QSG overlay)
  -> EditorSessionService::EnqueueMaskCreation (coalesced Append)
  -> TryConsumePendingInput / WithLockedLiveDocument
  -> EditorMaskCreationController + PublishAppliedTypedBatch(document_already_at_after)
  -> InteractiveAdjustment or SettledMaskEdit
```

**Residual gaps:** NM7.9 accumulating Brush paint/erase UI. Live `ResolvedRenderGeometry` publication into `setDisplayedMaskGeometry` remains NM7.10. Project Mix-cache slot is NM7.11. Radial feather/range presentation, drawer selection/deletion and Gradient restyling are NM7.8; remaining Brush/cache UI and accessibility qualification are NM7.12. Open-operation cancel on `MappingChanged` is NM7.13. Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.8 — Improve parameter-mask controls and existing-mask editing

**Status:** complete. Builds on NM7.7 and the early UI wiring; does not repeat owner-path
implementation or wait for Brush accumulation and project cache settings.

**Purpose:** make Radial feather/range editable and visible, allow selection/deletion and
re-editing from the Node Mask drawer, and replace the Gradient kite with Geometry crop-style
controls in the actual workspace.

**Work / interaction specification:**

1. **Radial feather and range.** Show the selected base ellipse and both feather boundaries
   from Section 6.2 during creation and later editing, including after release. Provide separate
   labelled Inner feather and Outer feather sliders/numeric controls plus independently
   draggable contour handles. Display percentages of the base radius (`100 * stored value`)
   and enforce the existing source validator limits. Feather changes preserve center, radii
   and rotation. Distinguish radius and feather handles by position/shape and accessible names;
   coincident zero-feather controls remain independently reachable through panel/keyboard.
   Draw coincident contours once and handle a collapsed inner contour without invalid geometry.
   Range and feather use lines only, with no coverage tint or filled band.
2. **Node drawer selection.** Every existing Brush/Radial/Gradient row is selectable through
   stable NodeId/MaskId. Selecting Radial/Gradient loads current fields and viewer handles;
   later edits update that same MaskId. Selection creates no mask, history or photo render.
   Bind highlight to session selection and preserve scroll; do not rebuild the list on click.
   Restore controls on re-entry, session rebind and Undo/Redo using the load-only route. Keep
   compact type labels in the Node drawer and parameter editors in the Mask Adjustment Stack page.
   Brush rows support selection/deletion here; new paint/erase controls remain NM7.9.
3. **Deletion.** Add a compact per-row delete icon with pointer/keyboard access and a clear
   accessible name. Target the clicked row's exact mask, never its Grade or a reused index.
   Stop delete-event propagation. Cancel/restore unfinished edits for that target before removal,
   discard its queued updates and reject delayed results. Use the existing owner and typed
   `RemoveMaskChange` path for one settled deletion and one history operation. Undo restores
   source, ID and list position; Redo removes it again. Deleting a selected row chooses next,
   otherwise previous, otherwise none; deleting another row preserves selection. Undo deletion
   selects the restored mask only when selection is still empty, otherwise preserves the valid
   current selection. Deleting the last mask restores full-image Grade coverage, including a
   disabled last mask. Failed deletion preserves the committed mask and reports the real error.
   Text-input Delete retains text semantics; Delete with Mask controls focused never deletes
   the Grade. The cancel/delete boundary belongs here; broader lifecycle work remains NM7.13.
4. **Gradient design.** Use the three parallel loci from Section 6.3, clipped to the photograph,
   with Geometry crop overlay's two-layer high-contrast fine lines, short edge grips and
   hover/active feedback. No kite, diamond, enclosing polygon, or crop dimming. Center line drag
   translates; boundary grips change transition distance symmetrically around fixed origin;
   a separate direction/rotation handle rotates. Keep stroke/hit sizes constant in logical px
   across zoom/DPR. Do not change image crop settings or add another Gradient feather field.
5. **Owner/input integration.** Pointer, numeric and keyboard controls share the existing
   begin/update/finish/cancel service and exact target identity. Update actual Interactive pixels
   before release, then one typed history operation and Quality on settle. Escape restores the
   original source without a commit. Read/edit through the existing owner without a mirrored
   editable source. Preserve prior-panel restoration. Fix any mapping publication
   needed for these controls in this phase: cropped/rotated images must use actual resolved
   geometry, not a guessed identity transform. Remaining serial/cache integration is NM7.10.
6. **VI.** Follow `alcedo-qml-ui`, reuse shared controls and AppTheme tokens. Update AppTheme
   and DESIGN together if new values are needed. This revision supersedes the old creation-only
   guide and read-only drawer restrictions. Keep no-fill coverage policy.

**Files/APIs:** `EditorNodeMaskDrawer.qml`, `EditorNodeMaskTypeRow.qml`,
`EditorMasksContextPanel.qml`, workspace/adjustment stack, existing creation adapter/controller;
`EditorOverlayItem`, analytic geometry/hit testing, Grade Mask owner, pending input and typed
history; AppTheme/DESIGN. Inspect and reuse focused APIs before extending them.

**Primary chains:** drawer select → session selection → load existing source → selected QSG
controls; handle/numeric edit → queued owner update → Interactive pixels → settle → one typed
history operation → Quality. Delete → cancel target's unfinished input → owner removal and typed
history → selection/list/overlay update → rendered remaining coverage.

**Tests:** production QML input, owner/history tests and independent coverage calculations:
`RadialSelectionShowsEllipseAndBothFeatherBoundaries`,
`RadialFeatherControlsPreserveCenterRadiiAndRotation`,
`RadialFeatherDragUpdatesInteractivePixelsBeforeRelease`,
`CoincidentRadialBoundariesKeepFeatherControlsReachable`,
`NodeDrawerSelectionLoadsExistingMaskWithoutCreatingOrRendering`,
`SelectedMaskCanBeEditedAfterWorkspaceReentry`,
`NodeDrawerDeleteRemovesExactMaskAndUndoRestoresSource`,
`DeletingLastMaskRestoresFullGradeCoverage`,
`DeletingUnselectedMaskPreservesSelection`,
`DeletingMaskRejectsQueuedEditsAndDelayedFrames`,
`MaskDeleteWithViewerFocusDoesNotDeleteGrade`,
`GradientGuidesUseThreeParallelLinesWithoutClosedPolygon`,
`GradientBoundaryDragPreservesOriginAndChangesTransition`,
`AnalyticControlCancelRestoresSourceWithoutHistory`.

**Exit:** actual workspace create → release → select another row → reselect → edit → Undo/Redo
→ delete works with multiple Radial/Gradient masks on two Grades. Existing Brush rows can be
selected/deleted without adding a Brush. Verify 260/320/460 px in both themes, non-square and
cropped/rotated images, zoom/pan and DPR 1/1.25/1.5/2. Captures show selected radial range/feather
lines and crop-style Gradient grips without coverage fill. Independent Section 6 formulas match
sampled coverage within 1 R8 code. Prove exact history counts, stable IDs/scroll and actual photo
updates before release; control redraw alone does not pass. Record the executed native backend
and any remaining platform qualification explicitly.

##### Phase NM7.8 completion record (2026-09-09)

**Status:** complete — selected Radial range/feather lines, crop-style Gradient guides,
Node Mask drawer select/re-edit/delete, and document Geometry mapping publication.

**Primary success call chain:**

```text
drawer click / Masks body row
  -> EditorMaskCreationAdapter::selectMask
  -> Enqueue SelectMask
  -> EditorMaskCreationController::SelectMask (load-only; zero Mix / history)
  -> overlay from live Grade source; Masks body fields

handle or Inner/Outer feather slider
  -> BeginMove / Append
  -> ReplaceMaskSource live
  -> Interactive Mix (host GradeMaskCoverage)
  -> Finish
  -> one ReplaceMaskSource commit
  -> Quality

delete icon / Delete with Mask controls focused
  -> cancel target op + drop queued Append/BeginMove/Finish/BeginInput
  -> MakeRemoveMaskBatch
  -> selection next-then-previous; overlay/list update
  -> remaining Mix; Grade node retained
```

**Primary failure call chain:**

```text
degenerate creation or Escape
  -> RestoreLive; history head unchanged

failed RemoveMask publish
  -> re-insert stored Mask at the same index; committed Mask remains

Delete with Mask controls focused
  -> removeSelectedMask; Grade delete shortcut is not taken
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RadialSelectionShowsEllipseAndBothFeatherBoundaries` | `MaskOverlayControlTest` | PASS |
| `RadialFeatherControlsPreserveCenterRadiiAndRotation` | `AnalyticMaskCreationTest` | PASS |
| `RadialFeatherDragUpdatesInteractivePixelsBeforeRelease` | `AnalyticMaskCreationTest` | PASS |
| `CoincidentRadialBoundariesKeepFeatherControlsReachable` | `MaskOverlayControlTest`, `AnalyticMaskCreationTest` | PASS |
| `NodeDrawerSelectionLoadsExistingMaskWithoutCreatingOrRendering` | `AnalyticMaskCreationTest` | PASS |
| `SelectedMaskCanBeEditedAfterWorkspaceReentry` | `AnalyticMaskCreationTest` | PASS |
| `NodeDrawerDeleteRemovesExactMaskAndUndoRestoresSource` | `AnalyticMaskCreationTest` | PASS |
| `DeletingLastMaskRestoresFullGradeCoverage` | `AnalyticMaskCreationTest` | PASS |
| `DeletingUnselectedMaskPreservesSelection` | `AnalyticMaskCreationTest` | PASS |
| `DeletingMaskRejectsQueuedEditsAndDelayedFrames` | `AnalyticMaskCreationTest` | PASS |
| `MaskDeleteWithViewerFocusDoesNotDeleteGrade` | `AnalyticMaskCreationTest` | PASS |
| `GradientGuidesUseThreeParallelLinesWithoutClosedPolygon` | `MaskOverlayControlTest` | PASS |
| `GradientBoundaryDragPreservesOriginAndChangesTransition` | `AnalyticMaskCreationTest` | PASS |
| `AnalyticControlCancelRestoresSourceWithoutHistory` | `AnalyticMaskCreationTest` | PASS |
| Drawer select highlight without list rebuild; delete icon emits exact MaskId | `EditorNodeDelegateQmlTest` | PASS |
| Cropped/rotated photograph uses resolved geometry, not identity | `MaskEditGeometryTest` | PASS |
| Independent Section 6 formulas vs Mix within 1 R8 | `AnalyticMaskCreationTest` | PASS |
| Overlay `coverage_fill_vertex_count == 0`; no closed Gradient polygon | `MaskOverlayControlTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target MaskOverlayControlTest --target AnalyticMaskCreationTest --target MaskEditGeometryTest --target EditorNodeDelegateQmlTest --target AlbumBackendLib`
`ctest --test-dir build/debug --output-on-failure -R "MaskOverlayControlTest|AnalyticMaskCreationTest|MaskEditGeometryTest|EditorNodeDelegateQmlTest"`

Suite totals: `56/56` PASS (`EditorNodeDelegateQmlTest` 21, `MaskEditGeometryTest` 5, `MaskOverlayControlTest` 11, `AnalyticMaskCreationTest` 19).
Date / working tree on `feature/parameter-mask-controls` / Windows MSVC `win_debug` / Qt 6.9.3 (`D:/misc/Qt/6.9.3/msvc2022_64`) / CUDA Toolkit 12.8. Interactive Mix is host `GradeMaskCoverage` using the native analytic equations; no GPU Mask pass in this phase.

**Checklist / exit condition:** required named tests PASS. Radial selected overlay shows base ellipse plus inner/outer feather lines (rings vs discs); coincident zero-feather contours are drawn once and Inner/Outer remain reachable through `BeginMaskMove` after settle. Drawer selection is load-only. Delete uses typed `RemoveMaskChange`, restores source/ID/index on Undo, chooses next then previous, and does not delete the Grade. Gradient guides are three photograph-clipped parallel loci with edge grips, not a kite. Feather/center/origin edits update Mix before `AppendEdit`. Cropped/rotated mapping uses `MakeDocumentPhotographGeometry` plus displayed photograph size (`interactionImageInfo`), not an unpublished identity transform.

**LOC note (grill-code-review):** `editor_mask_creation_adapter.cpp` 682, `mask_overlay_layout.cpp` 524, `analytic_mask_creation_test.cpp` 785, `EditorMasksContextPanel.qml` 203, `mask_list_selection.hpp` 55. Overlay layout owns selected contours/guides; the adapter routes QML; the controller owns select/delete/settle; `MaskIdAfterDeletion` / `MaskIdAfterUndoRestore` stay pure. No split required.

**Residual gaps:** NM7.9 accumulating Brush paint/erase UI. Live pipeline `ResolvedRenderGeometry` from the Interactive/Quality frame (including Interactive `max_edge`) remains NM7.10. Project Mix-cache slot is NM7.11. Remaining Brush/cache UI, accessibility qualification, and Mask-page width/theme matrix at 260/320/460 px are NM7.12. Open-operation cancel on `MappingChanged` is NM7.13. No single workspace e2e with two Grades was executed. Overlay evidence is geometry/QSG assertions, not PNG captures. Failed `RemoveMask` history publish re-inserts in production but was not driven by a failing history port. Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

##### Phase NM7.8 correction record (2026-09-09)

**Status:** complete — removed the second Mask list, made Mask a disabled-until-editing seventh
Adjustment Stack page, added Enter/panel-switch confirmation, exposed Radial/Gradient parameters,
and corrected Gradient control-axis geometry under non-square view mapping.

**Primary success call chains:**

```text
header Mask action or Node drawer Mask row
  -> EditorMaskCreationAdapter begin/select
  -> maskCreationChanged
  -> EditorAdjustmentStack stores the ordinary page and opens masks
  -> EditorMasksContextPanel reads the selected live source

Mask parameter slider
  -> BeginAnalyticMove
  -> AppendMaskInput
  -> owner ReplaceMaskSource + Interactive pixels
  -> finishAnalyticControl
  -> FinishMaskInput + one history operation + Quality

Enter or adjustment-page selection
  -> EditorMaskCreationAdapter::finishBody
  -> FinishMode
  -> EditorMaskCreationController::FinishCreationMode
  -> ResetMode
  -> prior or requested ordinary page

Gradient direction pointer
  -> MapItemPointToLinearDirectionSample using the view transform transpose
  -> evaluator-space normal
  -> MapLinearLocusNormalPointToItem
  -> item-space control axis perpendicular to all three guides

Node drawer Mask-row pointer click
  -> EditorNodeMaskTypeRow MouseArea
  -> AlcedoQanGraph MaskRowSelected(nodeId, maskId)
  -> EditorNodesPanel selects the owning Color Grade
  -> EditorMaskCreationAdapter selects the exact Mask
```

**What was proven (macOS debug tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MaskPageActivatesOnlyForSelectedOrCreatingMask` | `EditorAdjustmentHeaderQmlTest` | PASS |
| `EnterEquivalentFinishesMaskEditAndRestoresPanel` | `EditorAdjustmentHeaderQmlTest` | PASS |
| `FinishModeSettlesOnceAndLeavesMaskEditingInactive` | `AnalyticMaskCreationTest` | PASS |
| `GradientControlAxisIsPerpendicularAfterNonSquareMapping` | `MaskOverlayControlTest` | PASS |
| Existing affected tests in the three binaries | 40 additional tests | PASS |
| `MaskRowSelectionSelectsItsOwningColorGrade` | `EditorNodesPanelQmlTest` | PASS |
| Node Mask row avoids style-controlled text and all Node QML avoids Material imports | `EditorNodeDelegateQmlTest` | PASS |

Commands:
`cmake --build --preset macos_debug_tests --target MaskOverlayControlTest EditorAdjustmentHeaderQmlTest AnalyticMaskCreationTest -j4`
`ctest --test-dir build/macos-debug-tests --output-on-failure -R "MaskOverlayControlTest|EditorAdjustmentHeaderQmlTest|AnalyticMaskCreationTest"`

Suite total: `44/44` PASS. A broader `WorkspaceShellTest` rebuild remains blocked by the unrelated
existing use of removed `EditorPendingFieldChange::params_json` in `workspace_shell_test.cpp:1102`;
the focused production-QML test loads the changed stack and Mask page successfully. The
`alcedo_main` build compiled the changed QML cache and production sources, then the existing macOS
test-preset link failed on missing generated `qml_register_types_Alcedo_Main()`.

The Node drawer click correction was additionally verified with `EditorNodesPanelQmlTest` `34/34`
PASS and `EditorNodeDelegateQmlTest` `21/21` PASS on macOS debug. The interactive Mask row uses
QtQuick `Item` / `MouseArea` / `Text` plus the shared `IconActionButton`; it has no Material import
and no style-controlled `Label` in its pointer path.

**LOC note (grill-code-review):** `editor_mask_creation_adapter.cpp` 885,
`mask_overlay_layout.cpp` 630, `EditorMasksContextPanel.qml` 243,
`EditorAdjustmentStack.qml` 383, `analytic_mask_creation_test.cpp` 871,
`editor_adjustment_header_qml_test.cpp` 751, `mask_overlay_control_test.cpp` 551. The adapter owns
the QML command bridge, overlay layout owns view/evaluator geometry conversion, and the parameter
page contains no Mask collection or duplicate owner state.

### NM7.9 — Complete accumulating Brush creation, erase and movement

**Purpose:** multiple strokes remain editable data in one Brush, with one current Grade raster.

**Work:** header resumes existing Brush; append canonical paint/erase strokes with size/strength
setting boundaries; move the whole accumulated Brush by translation, including erase operations.
Keep move input separate from paint input. Retain samples for history, not QSG display. No
`MaskStore::Put` or `ReplaceMaskAsset` on new Brush release.

**Files/APIs:** creation controller, Brush source/command owner, canonical sampler, mapping helpers.
**Primary chain:** paint input → append draft samples → replay; move control → set placement → replay;
release → corresponding single typed command → Quality.

**Tests:** `MultipleStrokesUseOneBrushMask`, `SizeAndStrengthChangesPersistInStrokeSamples`,
`MovingExistingBrushUpdatesInteractivePixels`, `BrushMoveDoesNotAppendStroke`,
`UndoLastStrokePreservesEarlierStrokes`, `BrushReleaseCreatesNoHistoricalRasterFile`.

**Exit:** paint/erase/move/Undo/Redo work after deleting caches, with stable MaskId and StrokeIds.

##### Phase NM7.9 completion record (2026-09-10)

**Status:** complete — one accumulating Brush per Grade for header-created work; paint/erase
append canonical strokes; Move sets placement without rewriting samples; first release is
AddMask, later strokes are AppendBrushStroke, moves are SetBrushTranslation.

**Primary success call chain:**

```text
header beginBrush / BeginCreation(Brush)
  -> BeginMaskInput (first dab; Paint or Erase)
  -> BrushMaskInput + ColorGradeNodeModel AddMask (provisional first stroke)
     or ReplaceMaskSource (draft on an existing Brush)
  -> Interactive GradeMaskCoverage::EvaluateFull
  -> FinishMaskInput
  -> MakeAddMaskBatch (first stroke) or MakeAppendBrushStrokeBatch
  -> history AppendEdit / PublishAppliedTypedBatch (document already at after)
  -> Quality requested

SetBrushTool(Move) + BeginMaskMove(BrushMove)
  -> SetBrushTranslation live (canonical samples unchanged)
  -> Interactive GradeMaskCoverage::EvaluateFull
  -> MakeSetBrushTranslationBatch(before, after)
  -> history AppendEdit / PublishAppliedTypedBatch
  -> Quality requested
```

**Primary failure call chain:**

```text
Escape / cancel of an unfinished first stroke
  -> RestoreLive RemoveMask of the provisional Brush
  -> history head unchanged; Grade Mask list empty

several existing Brushes and header BeginCreation without a named Brush
  -> reject "select an existing Brush before painting"
  -> document unchanged

Brush Move
  -> no AppendBrushStroke; sample bodies stay shared
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MultipleStrokesUseOneBrushMask` | `AccumulatingBrushCreationTest` | PASS |
| `SizeAndStrengthChangesPersistInStrokeSamples` | `AccumulatingBrushCreationTest` | PASS |
| `MovingExistingBrushUpdatesInteractivePixels` | `AccumulatingBrushCreationTest` | PASS |
| `BrushMoveDoesNotAppendStroke` | `AccumulatingBrushCreationTest` | PASS |
| `UndoLastStrokePreservesEarlierStrokes` (Undo + Redo) | `AccumulatingBrushCreationTest` | PASS |
| `BrushReleaseCreatesNoHistoricalRasterFile` | `AccumulatingBrushCreationTest` | PASS |
| `HeaderBrushResumesSingleExistingBrush` | `AccumulatingBrushCreationTest` | PASS |
| `HeaderBrushWithMultipleExistingBrushesRequiresSelection` | `AccumulatingBrushCreationTest` | PASS |
| `DefaultBrushRadiusIsTwoPercentDiameterOfShorterEdge` | `AccumulatingBrushCreationTest` | PASS |
| `CancelledFirstStrokeLeavesGradeUnchanged` | `AccumulatingBrushCreationTest` | PASS |
| `DraftSamplesExposeOpenStrokeWithoutSealing` | `BrushCanonicalSamplerTest` | PASS |
| Analytic creation/movement and header Mask buttons | `AnalyticMaskCreationTest`, `EditorAdjustmentHeaderQmlTest` | PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AccumulatingBrushCreationTest --target BrushCanonicalSamplerTest --target AnalyticMaskCreationTest --target EditorAdjustmentHeaderQmlTest`
`ctest --test-dir build/debug --output-on-failure -R "AccumulatingBrushCreationTest|BrushCanonicalSamplerTest|AnalyticMaskCreationTest|EditorAdjustmentHeaderQmlTest"`

Suite totals: `47/47` PASS. Date / working tree on `feature/accumulating-brush-paint-erase-move` (base `0e890073`) / Windows MSVC `win_debug`. Interactive Mix in these tests is host `GradeMaskCoverage`; no GPU Mask pass in this phase. Ordered paint/erase Append is not coalesced (`ordered_append`); analytic and Brush-move Append still coalesce.

**Checklist / exit condition:** required tests PASS. Paint and erase accumulate on one MaskId with stable StrokeIds. Size/strength boundaries persist in the sample body. Move updates Mix R8 before history and does not append a stroke. Undo of the last stroke keeps earlier strokes; Redo restores the erased stroke. First-stroke release JSON has no `asset_key` or `ReplaceMaskAsset`. Header resume of a single existing Brush and rejection when several Brushes exist without a named target are covered. No project Mix-cache files existed to delete (that storage is NM7.11); `BrushReleaseCreatesNoHistoricalRasterFile` shows the owner path does not write raster history.

**LOC note (grill-code-review):** `editor_mask_creation_controller.cpp` 1069 / `.hpp` 318,
`brush_mask_input.cpp` 49 / `.hpp` 82, `editor_mask_creation_adapter.cpp` 1159 / `.hpp` 168,
`accumulating_brush_creation_test.cpp` 367, `brush_canonical_sampler.hpp` 86 / `.cpp` 126.
`BrushMaskInput` owns the open-stroke sampler and dab settings. The controller still orchestrates
analytic creation plus Brush paint/erase/move above the 1000-line mark; a later split should be a
second owner (Brush vs analytic), not a method-file split. The adapter remains the QML command
bridge and overlay publisher; Brush overlay geometry stays in `mask_overlay_layout`.

**Remaining gaps:** native serial Interactive Mix and one Grade R8 slot are NM7.10. Project
Mix-cache slot, keep/delete-on-close, and Clear are NM7.11. Mask Adjustment Stack paint/erase/
size/strength/Move chrome and the 260/320/460 theme matrix are NM7.12. Open-operation cancel on
`MappingChanged` is NM7.13. Header `beginBrush` is wired; the Mask page does not yet expose Brush
tool settings. Native Mask still loads `MaskStore` when an in-memory `asset_key` is present.
Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain
planned.

### NM7.10 — Integrate serial Interactive evaluation and one current Grade R8

**Purpose:** connect all provisional movement to real pixels while respecting NM6 reader ownership.

**Work:** extend the existing queued input/serial consumer. Latest absolute placement coalesces;
ordered Brush samples do not. Replace persistent per-source R8 retention in this path with transient
source/feather scratch and one current Grade Mix coverage. Recompute old/new damaged areas and
surviving contributions; distinguish source-local from feather-global work. Tag identity/revision
at consume and completion; QualityBase still bypasses persistent results after Develop.

**Files/APIs:** pending input, session edit controller/coordinator, pipeline request/scheduler,
plan executor, mask texture/result cache, native CUDA/OpenCL/Metal Mask passes.

**Primary chain:** queued change → safe consume/apply → replay/Union → Grade Mix → Interactive
presentation → next consume. Release seals → durable command → Quality.

**Tests:** `MaskMoveNeverMutatesSourceDuringRender`, `LatestMoveValueSurvivesRelease`,
`CurrentGradeCoverageHasOneRetainedResult`, `DelayedOldFrameCannotReplaceNewMaskPosition`,
`QualityMaskEvaluationDoesNotOverwriteInteractiveCache`, `RebuiltMaskMatchesCacheHitPixels`.

**Exit:** actual end-to-end native request path works; a control redraw alone does not pass.

##### Phase NM7.10 completion record (2026-09-10)

**Status:** complete — serial consume plus native parameterized Mix; one current Grade coverage
result; failed encodes keep last-good Mix

**Primary success call chain:**

```text
EnqueueMaskCreation (coalesce latest placement; ordered_append keeps samples)
  -> TryConsumePendingInput (blocked while inflight / 16 ms Interactive pacing)
  -> WithLockedLiveDocument / ApplyMaskCreationCommand
  -> RouteMaskCreationRender (Interactive or SettledMaskEdit)
  -> PlanExecutor MaskEvaluate
  -> parameterized canonical replay -> ActiveRasterTextures (not MaskStore)
  -> feather scratch / Union -> mask.union published (one Grade Mix)
  -> Grade Mix photograph
```

**Primary failure call chain:**

```text
inflight or 16 ms pacing
  -> consume skipped; live Brush placement unchanged; queued Append remains
missing MaskStore asset_key / injected upload failure
  -> CancelRender discards unpublished writes
  -> DropUnusablePublishedImages keeps last-good Mix when frame identity matches
  -> PublishedRevision stays at the prior successful Mix
delayed older Mix write
  -> PublishSuccessfulSubmission drops the write when published revision is newer
QualityBase (SensorDevelopOnly)
  -> Mix is not published; DiscardUnpublished leaves Interactive Mix
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MaskMoveNeverMutatesSourceDuringRender` | `EditorSerialMaskInteractiveTest` | PASS |
| `LatestMoveValueSurvivesRelease` | `EditorSerialMaskInteractiveTest` | PASS |
| `CurrentGradeCoverageHasOneRetainedResult` | `GpuDagCudaMaskTest` | PASS |
| `DelayedOldFrameCannotReplaceNewMaskPosition` | `GpuDagCudaWorkspaceTest` | PASS |
| `QualityMaskEvaluationDoesNotOverwriteInteractiveCache` | `GpuDagCudaMaskTest` | PASS |
| `RebuiltMaskMatchesCacheHitPixels` | `GpuDagCudaMaskTest` | PASS |
| `MaskFailurePublishesNoSourceUnionOrGradeWrites` | `GpuDagCudaMaskTest` | PASS |
| `MaskUploadFailureKeepsPriorPublishedResults` | `GpuDagCudaMaskTest`, `GpuDagOpenClGradeTest` | PASS |
| `LastGoodGradeSurvivesExposureRevisionWhenFrameIdentityMatches` | `GraphImageCacheRetentionTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaMaskTest --target GpuDagCudaWorkspaceTest --target EditorSerialMaskInteractiveTest --target GraphImageCacheRetentionTest --target GpuDagOpenClGradeTest
ctest --test-dir build/debug --output-on-failure -R "EditorSerialMaskInteractiveTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest.CudaWorkspaceFixture.DelayedOldFrameCannotReplaceNewMaskPosition|GraphImageCacheRetentionTest|GpuDagOpenClGradeTest.OpenClMultiMaskResourceFixture.MaskUploadFailureKeepsPriorPublishedResults"
```

Suite totals: required names `6/6` PASS; `GpuDagCudaMaskTest` `31/31` PASS;
`GraphImageCacheRetentionTest` `16/16` PASS; OpenCL `MaskUploadFailureKeepsPriorPublishedResults`
PASS. Date / working tree on `feature/serial-interactive-grade-mix` (base `34ffdebf`) / Windows
MSVC `win_debug` / CUDA for native Mix pixels. OpenCL/Metal Mask passes synthesize the same
request-owned canonical replay; Metal was not executed on this host. Host Mix oracle is
`GradeMaskCoverage` with `ExpectR8WithinTolerance` (1 code) when canonical raster equals the
16×12 render extent. QualityBase tests call `DiscardUnpublished` after
`ResultPersistenceScope::SensorDevelopOnly`, matching product cleanup so unpublished QualityBase
writes cannot shadow Interactive Mix.

**Checklist / exit condition:** required tests PASS. Native CUDA request path evaluates
parameterized Brush Mix without `MaskStore` or per-source `MaskTextureCache`. Latest placement
Append coalesces; `ordered_append` samples are not coalesced. Inflight consume does not mutate
the live source. One published Grade Mix slot is retained across moves. A delayed older Mix
write cannot replace a newer published revision. QualityBase does not overwrite Interactive Mix.
Rebuilding Mix after dropping the published slot matches the prior pixels. Failed Mask encode
keeps last-good Mix/Union/Grade. A QSG control redraw is not the acceptance path.

**LOC note (grill-code-review):** `parameterized_brush_raster.hpp` 69 / `.cpp` 47;
`compiled_grade_mask.hpp` 87; `cuda_mask_pass.cu` 580; `opencl_mask_pass.cpp` 690;
`metal_mask_pass.mm` 670; `graph_image_cache.hpp` 450; `basic_render_workspace.hpp` 330;
`editor_session_service.hpp` 676 / `.cpp` 2028 (Peek is a mutex copy of the Mask queue; Mix
rules stay in PlanExecutor / GraphImageCache). `cuda_parameterized_grade_mix_test.cpp` 205;
`editor_serial_mask_interactive_test.cpp` 278. Session cpp was already above 1000 lines;
this slice did not add a second Mix owner there.

**Remaining gaps:** project Mix-cache files, keep/delete-on-close, and Clear are NM7.11.
Mask Adjustment Stack remaining chrome and the 260/320/460 theme matrix are NM7.12.
Open-operation cancel on `MappingChanged` is NM7.13. Native Mask still loads `MaskStore` when
an in-memory `asset_key` is present (legacy raster tests). Ordinary adjustment Mask writes still
fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned. Packaged viewer/performance
qualification is NM7.14–NM7.15. Metal parameterized Mix was not executed on this Windows host.

### NM7.11 — Add project-owned cache storage and maintenance

**Purpose:** bounded, disposable R8 storage controlled per project.

**Work:** companion Sections 6–8: ProjectService settings and metadata round-trip; stable file slot,
identity header/checksum, coalesced writer, atomic replace, generations/locks; retain/delete-on-close,
Clear, project-removal choice and root switching. Preserve original parameter data. Track previous
registered roots for explicit later cleanup. Missing cache rebuilds through the same algorithm;
unavailable configured storage produces a visible error with no silent path substitution.

**Files/APIs:** `project_service`, proposed `project_mask_cache_service.hpp/.cpp`, project module,
package service/backend, current MaskStore/reachability consumers for replacement inventory.

**Primary chain:** project setting → validate → atomic setting publish → per-project cache jobs;
Clear → maintenance boundary → stop old writer → delete only owned cache → invalidate.

**Tests:** `ProjectMaskCacheSettingsSurviveSaveAndReopen`, `ThousandStrokesKeepOneRasterSlot`,
`CacheClearCannotDeleteAnotherProjectsFiles`, `OldWriterCannotRecreateClearedCache`,
`RootChangeFailurePreservesOldSetting`, `CacheWriteFailureDoesNotLoseStrokeHistory`,
`CloseCleanupRunsAfterParameterSaveAndReadersFinish`.

**Exit:** new cache namespace can be completely removed without losing any supported edit/Version.

##### Phase NM7.11 completion record (2026-09-10)

**Status:** complete — project Mix-cache settings, one-slot writeback, Clear, root change, and close cleanup

**Primary success call chain:**

```text
SetMaskCacheRoot / SetMaskCacheRetention
  -> PrepareNamespace (no temp substitution)
  -> SaveProject writes mask_cache in metadata JSON
  -> ProjectMaskCacheService::PublishChosenRoot / EnqueueSettledWrite
  -> coalesced writer -> temp file + checksum -> atomic replace
     <chosen-root>/alcedo-mask-cache/<ProjectUUID>/<ImageId>/<hex(NodeId)>.r8cache
PersistCurrentProjectState / CloseAfterSuccessfulSave(Keep)
  -> FlushPendingWrites
DeleteOnProjectClose after parameter save
  -> close_cleanup_pending persisted
  -> wait readers/writers -> delete owned .r8cache only
```

**Primary failure call chain:**

```text
unusable new root or settings-revision mismatch
  -> no metadata write of the new root; previous files retained
publish hook / I/O failure
  -> temp removed; last-good slot kept; dirty retained; stroke JSON/history untouched
Clear / generation bump while a writer is in-flight
  -> old generation refuses atomic replace; cleared namespace stays empty
unavailable configured root
  -> error string; no process-temp fallback
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ProjectMaskCacheSettingsSurviveSaveAndReopen` | `ProjectMaskCacheServiceTest` | PASS |
| `ThousandStrokesKeepOneRasterSlot` | `ProjectMaskCacheServiceTest` | PASS |
| `CacheClearCannotDeleteAnotherProjectsFiles` | `ProjectMaskCacheServiceTest` | PASS |
| `OldWriterCannotRecreateClearedCache` | `ProjectMaskCacheServiceTest` | PASS |
| `RootChangeFailurePreservesOldSetting` | `ProjectMaskCacheServiceTest` | PASS |
| `CacheWriteFailureDoesNotLoseStrokeHistory` | `ProjectMaskCacheServiceTest` | PASS |
| `CloseCleanupRunsAfterParameterSaveAndReadersFinish` | `ProjectMaskCacheServiceTest` | PASS |
| Existing UUID save/load (metadata still round-trips) | `ProjectServiceTest` | PASS `6/6` |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target ProjectMaskCacheServiceTest
ctest --test-dir build/debug --output-on-failure -R "ProjectMaskCacheSettingsSurviveSaveAndReopen|ThousandStrokesKeepOneRasterSlot|CacheClearCannotDeleteAnotherProjectsFiles|OldWriterCannotRecreateClearedCache|RootChangeFailurePreservesOldSetting|CacheWriteFailureDoesNotLoseStrokeHistory|CloseCleanupRunsAfterParameterSaveAndReadersFinish"
ctest --test-dir build/debug --output-on-failure -R "ProjectServiceTest."
```

Suite totals: required names `7/7` PASS; `ProjectMaskCacheServiceTest` `7/7` PASS; `ProjectServiceTest` `6/6` PASS. Date / working tree on `feature/project-mask-cache` / Windows MSVC `win_debug`. Packages still contain only metadata JSON plus the DuckDB file (no `.r8cache`). `.r8mask` files are not deleted by Clear.

**Checklist / exit condition:** required tests PASS. Settings survive Save, Load, and packed reopen. One thousand coalesced writes leave one published slot. Clear of project A leaves project B and `.r8mask` files. A fenced in-flight writer cannot recreate a cleared slot. Failed root change keeps the previous setting and files. Failed cache publish leaves stroke JSON and project metadata intact. DeleteOnProjectClose waits for a held reader, then removes only the Mix-cache namespace after the parameter save.

**LOC note (grill-code-review):** `project_mask_cache_service.hpp` 226 / `.cpp` 745; `project_mask_cache_settings.hpp` 80 / `.cpp` 159; `project_service.hpp` 118 / `.cpp` 742; `project_mask_cache_service_test.cpp` 319. The cache writer owns generation, coalesced pending slots, and namespace deletion. ProjectService owns metadata keys, revision, and close-cleanup persistence. No file crossed 1000 lines.

**Residual gaps:** the serial Interactive Mix owner (GraphImageCache / PlanExecutor) does not yet call `EnqueueSettledWrite`; save/close flush the cache writer, but a settled native Mix is not copied onto disk until that enqueue exists. Mask Adjustment Stack cache UI, root chooser, and Clear copy are NM7.12. Open-operation cancel and project-switch fencing of in-flight cache jobs beyond the current close/switch hooks are NM7.13. Native Mix rebuild after a cacheless reopen is NM7.14. `RemoveRecentProject` still only edits the recent list (KeepFiles). Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.12 — Wire Mask editing and project cache UI

**Purpose:** make the fully working capability reachable through approved surfaces.

**Already brought forward:** Radial/Gradient header, viewport and session test wiring after
NM7.7; NM7.8 owns analytic controls and drawer selection/deletion. Retain and verify those paths.
The complete UI phase remains planned until its remaining work and acceptance pass.

**Work:** finish Brush routing into the Mask Adjustment Stack page and preserve prior panel restoration;
expose paint/erase/size/strength/Move and remaining source/Mask values. Add project
cache section through app APIs with root chooser, policy and per-project usage/Clear. Existing
cache settings are thumbnail-specific; keep the new project fields separate. Add keyboard/accessible
controls, focus rules, theme/width/reduced-motion tests and QML registration.

**Files/APIs:** header/stack/Mask parameter page/node drawer, `SettingDialog.qml`,
`CacheSettingsPanel.qml`, proposed `ProjectMaskCacheSettingsPanel.qml`, project adapter, AppTheme/DESIGN.

**Primary chain:** UI action → exact project/Mask owner → validated operation → completion projection.
**Tests:** `MaskPageActivatesOnlyForSelectedOrCreatingMask`, `MaskSelectionDoesNotSubmitOrJumpScroll`,
`KeyboardMaskMoveUsesSameInteractiveRoute`, `CacheSettingsTargetSelectedProjectOnly`,
`ProjectClearExplainsThatBrushHistoryIsRetained`.

**Exit:** production QML at 260/320/460 px in both themes; no unregistered/unused-only implementation.

##### Phase NM7.12 completion record (2026-09-10)

**Status:** complete — Mask value/Brush editing controls on the Mask page, keyboard move,
and project Mix-cache settings UI on production QML.

**Resolved decisions and actual files/APIs:**

- `EditorMaskCreationCommandKind::{BeginMaskField, SetMaskField}` queue typed Mask value edits;
  `BeginMaskFieldEdit`/`ApplyMaskFieldValue` live-apply then settle `SetMaskField` for
  `enabled`/`invert`/`opacity`/`display_name` and `ReplaceMaskSource` for `brush.feather`.
  `CancelMode`/`RemoveMask` prune queued field commands in `editor_session_service.cpp`.
- `EditorMaskCreationAdapter` exposes `maskEnabled`/`maskInvert`/`maskOpacityPercent`/`maskName`,
  `setMaskEnabled`/`setMaskInvert`/`setMaskName`, `beginMaskOpacity`/`updateMaskOpacity`,
  `beginBrushFeather`/`updateBrushFeatherPercent`, `brushRadiusPercent`/`setBrushRadiusPercent`,
  `setBrushStrengthPercent`, `setBrushTool`, and `maskNudgeAvailable`/`beginMaskNudge`/`nudgeMaskBy`.
  Keyboard move reuses the pointer `BeginMove`/`Append`/`Finish` route with a panel-pointer
  identity and `open_via_panel_`.
- `EditorMasksContextPanel.qml` adds the Brush section (Paint/Erase/Move segments, Size,
  Strength, Feather), Mask values (name, enabled, invert, opacity) and a keyboard Position
  nudge control; existing objectNames for the analytic controls are unchanged.
- `EditorMonoSlider.qml` gains `activeFocusOnTab`, arrow-key stepping (Shift = 10×), and
  `Accessible` Slider role/increase/decrease actions; every key step runs
  `onBegin → onUpdate → onFinish` so one press is one settled edit.
- `ProjectModule` publishes `maskCacheState` (`available`, `projectName`, `projectUuid`,
  `chosenRoot`, `effectiveRoot`, `retention`, `fileCount`, `byteCount`, `pendingWrites`,
  `dirty`, `lastError`, `applyError`) plus `ApplyMaskCacheRoot`/`ApplyMaskCacheRetention`/
  `ClearMaskCache`/`RefreshMaskCacheState` over `ProjectService`.
- `ProjectMaskCacheSettingsPanel.qml` is a separate Settings > Cache section with a root
  FolderDialog, Keep/DeleteOnProjectClose combo, usage rows, revision-aware reload and Clear;
  thumbnail settings in `CacheSettingsPanel.qml` are untouched. Registered in
  `alcedo_main/CMakeLists.txt` and hosted by `SettingDialog.qml` with apply-on-OK and reset-on-open.

**Primary success call chain:**

```text
Mask page control -> EditorMaskCreationAdapter invokable
  -> EditorMaskCreationCommand{BeginMaskField|SetMaskField|BeginMove|Append|Finish}
  -> EditorSessionService queue -> EditorMaskCreationController
  -> live apply (Interactive preview for pixel fields)
  -> PublishMaskFieldEdit / PublishSettledBatch
  -> MakeSetMaskFieldBatch | MakeReplaceMaskSourceBatch -> MiniGitWorkingHistory commit
  -> maskCreationChanged -> panel republish
Keyboard Arrow on nudge area -> beginMaskNudge -> SelectMask + BeginMove(handle)
  -> nudgeMaskBy -> Append samples -> Keys.onReleased -> finishAnalyticControl -> Finish
Settings OK / Apply -> ProjectMaskCacheSettingsPanel.applyPending
  -> ProjectModule::ApplyMaskCacheRoot / ApplyMaskCacheRetention
  -> ProjectService::SetMaskCacheRoot / SetMaskCacheRetention -> settings_revision++
  -> MaskCacheStateChanged -> reloadPending
```

**Primary cancel/failure call chain:**

```text
Mask edit rejection -> result.error; queued SetMaskField for a different field while a
  field edit is open -> rejected; CancelMaskInput restores before_field_value_/source
  with no commit; removeMask/CancelMode drop queued field commands
ApplyMaskCacheRoot/Retention failure -> ProjectService error string -> applyError -> visible
  status row; no temporary-root substitution
Clear -> ProjectMaskCacheService generation fence -> MaskCacheStateChanged refresh;
  hint text states Brush strokes/history survive and cache rebuilds
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MaskPageActivatesOnlyForSelectedOrCreatingMask` | `EditorAdjustmentHeaderQmlTest` | PASS |
| `MaskSelectionDoesNotSubmitOrJumpScroll` | `EditorAdjustmentHeaderQmlTest` | PASS |
| `KeyboardMaskMoveUsesSameInteractiveRoute` | `EditorAdjustmentHeaderQmlTest` | PASS |
| `MaskPageBrushSectionRoutesToolSizeStrengthAndFeather` | `EditorAdjustmentHeaderQmlTest` | PASS |
| `MaskPageLoadsAtPanelWidthsInBothThemesWithoutMotion` (260/320/460 × 2 themes, reduceMotion) | `EditorAdjustmentHeaderQmlTest` | PASS |
| `CacheSettingsTargetSelectedProjectOnly` | `ProjectMaskCacheSettingsQmlTest` | PASS |
| `ProjectClearExplainsThatBrushHistoryIsRetained` | `ProjectMaskCacheSettingsQmlTest` | PASS |
| `ErrorsSurfaceFromProjectModuleState` | `ProjectMaskCacheSettingsQmlTest` | PASS |
| Field one-shot/drag/cancel/unselected commits | `AccumulatingBrushCreationTest` (`OneShotMaskFieldEditsPublishTypedHistory`, `MaskFieldDragSettlesOneCommit`, `MaskFieldEditCancelRestoresLiveValueWithoutHistory`, `MaskFieldEditRequiresSelectedMask`) | PASS |

Commands:

```text
cmake --build build/macos-debug-tests --target EditorAdjustmentHeaderQmlTest \
    ProjectMaskCacheSettingsQmlTest AccumulatingBrushCreationTest AnalyticMaskCreationTest \
    EditorAdjustmentControlQmlTest EditorSerialInputBoundaryTest MaskOverlayControlTest -j 8
QT_QPA_PLATFORM=offscreen ./build/macos-debug-tests/alcedo_studio/tests/ui/<binary>
```

Suite totals (macOS `macos-debug-tests`, Qt 6.9.2, offscreen QPA): required names 9/9 PASS;
`AccumulatingBrushCreationTest` 14/14; `AnalyticMaskCreationTest` 20/20;
`EditorAdjustmentHeaderQmlTest` 16/16; `ProjectMaskCacheSettingsQmlTest` 3/3;
`EditorAdjustmentControlQmlTest` 8/8; `EditorSerialInputBoundaryTest` 7/7;
`MaskOverlayControlTest` 13/13. `alcedo_main` links in `macos-debug`.

**Checklist / exit condition:** required tests PASS. Production Mask page loads at
260/320/460 px under both themes with reduceMotion enabled; Brush tool segments, Size,
Strength, Feather and Mask name/enabled/invert/opacity all route through the adapter;
arrow-key nudge takes the same BeginMove/Append/Finish interactive route; project cache
section is separate from thumbnail cache, applies through `ProjectModule`, surfaces
`applyError`, and Clear explains that Brush strokes/history are retained.

**Residual gaps:** real-viewer/pointer Brush qualification and packaged D3D11/Metal capture
are NM7.15. Open-operation cancel on `MappingChanged`, grab cancellation and project-switch
fencing beyond current hooks are NM7.13. Cacheless reopen rebuild is NM7.14. The settled
native Mix still is not enqueued into the project cache writer from the serial owner.
`.r8mask` files and the recent-project list retain their existing KeepFiles behavior.
Ordinary adjustment Mask writes still fail with the existing “until NM3” text.
NM6.8–NM6.9 remain planned.

### NM7.12R — 修复 Brush 创建、坐标、移动框、擦除与绘制性能

**Date:** 2026-09-10。**Status:** planned — 本次仅完成代码路径调查、Qt 文档核对和修复方案；
未修改产品实现，未执行测试或测量，不声称已复现或修复用户报告的六个问题。

**目标：** 点击 Brush 后第一笔就是笔刷；笔尖与实际受影响像素对齐；Paint/Erase 与 Move
各有明确的输入和控件；擦除对当前 Brush 的真实 coverage 生效；持续绘制保持响应；新建 Brush
从第一笔起就带默认羽化。六项一起验收，不能只修控件后把实际像素或绘制性能留给 NM7.15。

**边界：** 沿用一组累计笔触对应一个 `MaskId` 的产品语义。Move 移动该 Brush 内所有 Paint/Erase
笔触，不新增单笔选择、缩放或旋转；也不改其他 Mask 的合成规则。缓存设置 UI 不在本次修复范围。
本阶段处理这些输入必需的释放、取消和过期回执；NM7.13 保留完整项目生命周期矩阵，NM7.14–NM7.15
保留全平台恢复和打包资格验证。当前平台上的六项真实 viewer 验收不能依赖后续阶段才能通过。

#### NM7.12R.1 — 代码证据与复现入口

以下是 2026-09-10 工作树的静态调查。用户现象是问题输入；表内实现事实并不等于运行时根因
已经确认。执行时先记录当前 commit、工作树、Qt 版本、RAW、viewport、DPR、backend 和 build type，
通过生产 `EditorAdjustmentHeader.qml` 的 Brush 按钮复现，再把原因与测试绑定。

| 问题 | 当前实现事实与调查位置 | 必须确认的运行时证据 |
| --- | --- | --- |
| 第一笔出现随鼠标转动的 Gradient | Header 已调用 `beginBrush()`。`EditorMaskCreationAdapter::handlePress` 与 `EnqueueAppendSample` 在通用创建分支中用“Radial，否则 Linear”构造 source；`BeginBrushTool`、`ApplyOwnerSource` 和 `SyncFromSession` 分别改变工具、source 与编辑状态。正常 Brush 分支本来应在此前返回 | 记录从按钮到首次按下、Append、owner consume、回执的 source kind、Brush tool、operation identity。分别断言文档 source、QSG display kind、实际 coverage；区分错误的是控件还是蒙版本身。覆盖先选 Gradient/Radial 再点 Brush，以及 owner 回执延迟 |
| 笔触出现在笔尖左上方 | `EditorWorkspace.qml` 直接传 `point.position`；`MakeSample` 通过共享 mapper 转 ReferenceSpace。`PublishDisplayedGeometry` 又用 `imageWidth/imageHeight` 与 document crop/rotation 重建 geometry；它不是直接接收当前照片帧的 resolved geometry。`PointHandler.onActiveChanged(false)` 仍读取可能已经清零的位置 | 同时记录事件所属 Item、映射后的 item/reference/local 点、当前帧 geometry identity、实际像素质心。不能只做同一函数的正反变换测试，也不能先认定是 DPR 或固定偏移 |
| 单点既像把手又像笔尖 | `MakeBrushExistingOverlayDisplay` 只把 `placement_translation` 映射成一个 `BrushMove` 点；该点不是笔触范围中心。`PublishOverlay` 在 Paint/Erase 未按下时也进入这一显示分支；`handleHover` 只更新把手命中，没有独立笔尖跟随。`setBrushTool` 只发状态通知，未立即重建 overlay | 空闲 Paint、绘制中、提交后、Erase、Move 的 production QML/QSG 截图与命中测试；验证状态切换后首帧即正确 |
| Erase 看起来没用 | owner 已有 `BrushStrokeMode::Erase`，rasterizer 已有 `min(previous, 255-dab)`；按钮存在不等于 mode、目标、dirty 和 native Mix 都正确。其他启用 Mask 的 max 合成也可能覆盖被擦除区域 | 先用单 Brush、enabled=true、invert=false、opacity=1、非恒等 Grade 验证，再加其他 Mask；检查中间 source R8、最终 Grade R8、照片与 history |
| 绘制卡顿，提交后调参数流畅 | 每条 Append 都先调用 `SetBrushStrokeParameters`，再调用 `AppendMaskInput`；前者即使参数未变化也可能 `ApplyLiveBrushDraft`。`DraftStroke` 复制当前完整 sample 前缀，`ComposeDraftBrush` 复制 strokes 容器；overlay 每次复制整条 item path。native 没有 active override 时经 `ParameterizedBrushActiveRasterForGrade` 完整重放并把 dirty 标成全图 | 分别测事件、GUI 发布、排队、owner 更新、规范重放、上传、羽化、Union/Mix 和显示。session 当前已把一个 consume batch 汇总成一次渲染，不能把每次 draft 更新错误描述为一次 GPU render |
| 默认无羽化 | `BrushMaskSource::feather_radius` 默认 0；adapter/input hardness 默认 1；`BeginBrushStroke` 新建分支使用空 Brush 默认值，面板 Feather reset 也写 0 | 第一笔按下到释放后的 canonical source、Feather 数值、边缘剖面一致；旧 Brush 的显式 0 不被加载流程改写 |

重点源文件入口：
[adapter](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_mask_creation_adapter.cpp)、
[controller](../../../../../alcedo_studio/src/app/editor_mask_creation_controller.cpp)、
[serial session](../../../../../alcedo_studio/src/app/editor_session_service.cpp)、
[mapping](../../../../../alcedo_studio/src/ui/edit_viewer/mask_edit_geometry.cpp)、
[overlay layout](../../../../../alcedo_studio/src/ui/edit_viewer/mask_overlay_layout.cpp)、
[native replay bridge](../../../../../alcedo_studio/src/include/edit/runtime/compiled_grade_mask.hpp)、
[full source replay](../../../../../alcedo_studio/src/edit/mask/parameterized_brush_raster.cpp)。

#### NM7.12R.2 — Qt 输入与几何规则

执行只使用 Qt 6.9 已有 API。下列官方在线页可能显示更新版本；不使用其中标注晚于 6.9 的功能。

| Qt 一手文档 | 对本次修复的约束 |
| --- | --- |
| [Qt Quick 坐标系统](https://doc.qt.io/qt-6/qtquick-visualcanvas-coordinates.html) 与 [Item 映射](https://doc.qt.io/qt-6/qml-qtquick-item.html#mapToItem-method) | 输入坐标属于具体 Item。边界上用 `mapToItem`/`mapFromItem` 转到 viewer logical coordinates；不能手减侧栏宽度、margin 或标题栏高度 |
| [handlerPoint](https://doc.qt.io/qt-6/qml-qtquick-handlerpoint.html#details) | `position` 相对 handler 的 parent；释放或由其他 handler 处理后可能归零。必须在有效事件交付中取值，不能在 inactive 回调把 `(0,0)` 当作真实释放点 |
| [PointHandler](https://doc.qt.io/qt-6/qml-qtquick-pointhandler.html) 与 [PointerHandler](https://doc.qt.io/qt-6/qml-qtquick-pointerhandler.html#signals) | 被动观察不代表独占输入；区分正常 release、`canceled` 和 grab 转移，携带真实 device/point 标识，一条输入只产生一个终止结果 |
| [DragHandler](https://doc.qt.io/qt-6/qml-qtquick-draghandler.html#target-prop) | `target: null` 让业务层维护位置。Paint 必须从 press 接收输入，不等 drag threshold；Move 不得让 handler 自动移动 QML Item 再把同一个位移应用到 source |
| [QQuickItem](https://doc.qt.io/qt-6/qquickitem.html#updatePaintNode) 与 [Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html) | GUI 修改控件数据后请求 `update()`；QSG 更新与节点复用在同步/渲染边界完成。`updatePaintNode` 执行期间 GUI 被阻塞，因此不能在其中做长路径扫描、重放或 GPU 等待 |

**统一坐标链：** 令 `p_h` 为 handler parent 的逻辑坐标，`p_v` 为 viewport 逻辑坐标，
`G` 为当前已显示照片对应的 `ResolvedRenderGeometry`，`t` 为 Brush 的参考像素平移。

```text
p_v = handlerParent.mapToItem(viewportItem, p_h)
u_d = ViewportMapper::WidgetPointToImageUv(p_v, widget, photograph, zoom, pan)
u_p = roi.origin + u_d * roi.extent           only for RoiFrame
u_p = u_d                                   otherwise
p_render = (u_p.x * G.render_extent.width, u_p.y * G.render_extent.height)
p_reference = G.render_to_reference * (p_render.x, p_render.y, 1)
p_local = p_reference - t

display(p_local) = MapReferenceToItem(G, p_local + t)
```

`ViewportMapper` 已处理内部 DPR 转换；QML 不再乘一次 DPR。RoiFrame 的 ROI 展开、full-frame
zoom/pan 和 DetailPatch 呈现规则只在共享 mapper 实现一次。ReferenceSpace 始终是完整参考图，
不能用缩小后的 Interactive 纹理或 DetailPatch 宽高重新定义。Orientation、裁切、旋转、
expand-to-fit 和取整后的 render extent 必须来自照片实际采用的 geometry。
上式先展开 ROI，因此 `G.render_extent`/矩阵必须对应整张 photograph；如果帧携带的是 patch
局部矩阵，则先在现有 geometry owner 转成同一整图表示，不能再把展开后的 UV 乘 patch extent。

修复步骤：

1. 在现有 presentation/session 边界发布已接纳照片帧的 resolved geometry 与 frame identity，
   供 `EditorInteractionController` 读取；移除 adapter 每次 `PublishOverlay` 重新推导照片几何
   的职责。只发布映射所需字段，复用现有 geometry 表示，不复制图像或另建可编辑 document。
2. press 时锁定本次 operation 的 mapping identity；Append/Release 若身份变化则有序取消，
   不能把一条笔触的前后半段写入不同坐标系。无有效 geometry 的 press 返回明确错误。
3. 用真实终止事件坐标采样最后一段；若现有 QML 回调无法保证事件仍有效，则在现有 Qt/C++
   输入边界提取 release，再路由到 adapter。不得用归零点，也不得用“最后 move 点”悄悄丢失
   release 独有的最后一段。保留独立 hover 点，仅用于控件，不作为已绘制 sample。
4. 规范 R8 第 `(i,j)` 个像素中心对应 `((i+0.5)*W/Rw, (j+0.5)*H/Rh)`；native 输出像素中心
   经现有 `MakeRasterMaskSamplingPlan` 采样。不要在输入或显示端补一个经验性半像素偏移。
5. 笔尖外形从参考像素圆映射：`p_reference + r*(cos(theta), sin(theta))`。仿射映射非等比时是
   椭圆，不能只映射 x 方向半径再画屏幕圆；映射为等比时可用中心与标量半径的快速路径。
   控件线宽/命中宽度保持逻辑像素，笔刷半径与 feather 保持参考像素。

独立数值用例：6000×4000 参考图在 900×700 viewer 中 fit，照片矩形为 `(0,50,900,600)`。
viewer 点 `(225,200)` 必须对应参考点 `(1500,1000)`；同一行列和中心点在 DPR 1、1.25、1.5、2
下不变。另用有已知裁切与 90° 旋转的手算点验证矩阵顺序；不能以生产 mapper 生成预期值。

#### NM7.12R.3 — 工具状态与两套控件

**状态归属：** owner 决定可接受的工具和 operation；adapter 只持有当前输入及显示所需的最小数据。
排队成功不等于 owner 已接受。请求/回执至少携带现有 session identity、`NodeId`、`MaskId`、
sequence 和工具选择修订标识；旧回执不能覆盖用户刚选择的 Brush，也不能把旧 Gradient source
重新发布到它的 overlay。未产生 `MaskId` 时仍可准备笔刷参数和显示 hover 轮廓。

| 状态 | 可见控件与输入 | 退出与历史 |
| --- | --- | --- |
| Paint ready，包含首次新建 | 跟随鼠标的空心笔刷轮廓；可显示羽化外边界；不显示 Move 点/框。已有 Brush 保持原位 | press 立即进入 Painting；单击也产生一个规范 dab |
| Painting | 只绘制，任何位置按下都不会移动原笔触；平移参数固定；Size/Strength 变化使用明确参数边界 | release 后等待 owner settle；首次有效笔触一个 `AddMask`，之后每笔一个 `AppendBrushStroke`；随后回 Paint ready |
| Erase ready / Erasing | 与 Paint 同坐标和半径的轮廓，以短划线区分；不复用实体圆点。只对当前已存在 Brush 擦除 | release 提交一个 Erase stroke；随后回 Erase ready。没有已提交 Brush 时禁用 Erase |
| Settling | 尚未取得提交结果时禁止 Move 和目标切换造成的半完成编辑；笔尖显示仍可跟随 hover | owner 回执决定可用性；失败取消未完成数据并显示具体错误，不虚报提交 |
| Move ready | 当前累计 Brush 的边界框与四角短线锚点；框内、边框和锚点统一表示整体平移 | press 命中框内或边框才进入 Moving；框外不绘制；不显示缩放/旋转鼠标图标 |
| Moving | 原框整体随指针移动，照片在 release 前更新；没有笔尖控件 | release 一个 `SetBrushTranslation`；取消恢复原平移；不新增 StrokeId |

Move 是用户明确切换的工具；release 后继续保留 Paint/Erase，方便连续画多笔。只有已提交 Brush
且没有打开的 stroke/field edit/待确认 settle 时允许 Move。来自 Node drawer 的 Brush 选择进入
可移动的编辑态；工具栏 Brush 按钮则明确进入 Paint。切换都必须立即重建 overlay 和鼠标形状。
Paint/Erase 期间隐藏并禁用平移的指针及键盘入口，owner 同时拒绝交错 BeginMove；仅禁用 UI 不够。
下一笔可作为新 sequence 排在前一笔 Finish 之后；接收新笔不能等待 Quality 帧或缓存落盘。
owner 的提交回执、照片呈现回执分开处理，旧笔 settle 回执不得清掉已开始的新笔 cursor 或工具。

**删除歧义分支：** 按 `MaskSourceKind` 明确分发 Brush/Radial/Linear；解析型创建函数只接收解析型
source，错误 kind 直接拒绝。不能把任何“不是 Radial”的情况构造为 Linear。`BeginBrushTool`
先确定 Paint 再填 command settings，避免 `EnqueueBrushSettings` 用先前的工具值覆盖请求。
`ApplyOwnerSource` 同步选中 source 时保留或明确恢复当前合法工具，不能无条件改变输入模式。

**边界框几何：** 在 owner 的只读笔触查询上计算参考像素中的保守范围，按 source revision 缓存
派生 bounds，禁止在每次 hover 扫描整套 strokes。所有 strength>0 的 Paint dab 的圆形支持域
取包围盒，包含各自 radius，再按 source feather 支持范围与采样边界向外扩展。Erase 不扩大
这个框；擦除后可以保留保守框，以便移动和 Undo，不能宣称它是实际非零像素的精确轮廓。
框不受 invert 后的全图 coverage 支持域影响，也不因裁切把源几何永久缩小。

```text
B_local = AABB(union of positive-strength Paint dab supports)
B_reference = expand(B_local, feather_support) + placement_translation
frame_vertices = MapReferenceToItem(each of the four B_reference corners)
t_after = t_before + (reference_at_current_pointer - reference_at_press)
```

保留映射后四边形的四个角，图片旋转时框也对应旋转，不把它再压成屏幕轴对齐框。空范围不创建
伪造中心点；全擦除但仍有 Paint 历史时可保留原保守框。框外不可见时保留面板键盘移动能力。
命中区域只与 viewport 可见区相交，crop 样式只复用视觉 token 与线段绘制，不复用 crop 的缩放动作。
拖动包括所有 Erase 记录的整个 Brush；各 sample body 不改写、不重采样。细线、四角锚点、cursor
内外轮廓全部走现有 retained QSG，不用逐 dab QML Item，不增加覆盖区域填色。

#### NM7.12R.4 — Erase 的真实像素与历史

沿用已持久化的规范算法，不在 UI 修复中更换擦除数学：

```text
a8 = clamp(floor(255 * DabCoverage(distance, radius, strength, hardness) + 0.5), 0, 255)
paint: b_next = max(b_previous, a8)
erase: b_next = min(b_previous, 255 - a8)
source strokes in order -> source feather -> invert -> opacity -> enabled Mask max -> Grade Mix
```

满强度中心的 `a8=255` 必须把已画源像素变成 0；半强度 `a8=128` 将 255 限到 127；重复相同
半强度擦除仍为 127，这不是持续流量式擦除。不得为了“看得见”改成逐帧乘法衰减。先证明
feather=0 时逐字节结果，再验证默认 feather 下符合已有羽化公式的软边，不把羽化后的数值当作
原始 Erase R8。平移后的 Brush 必须先减当前 `placement_translation` 再记录擦除点。

Erase 与 Paint 共用 Begin/ordered Append/Finish，但每笔 mode 在 Begin 固定，不能中途被异步
选择回执改回 Paint。dirty 区域清理并按 stroke 顺序重放；不能只用 max 把结果叠上去。擦除某个
Mask 后重算其他启用 Mask 的贡献，不能直接清零整个 Grade Mix。单 Brush 测试后增加重叠 Radial，
断言 Brush 已减少而重叠处最终 Mix 仍由 Radial 决定；面板说明擦除作用于当前 Brush。

取消丢弃未提交 Erase；Undo 移除该记录并重放，Redo 恢复同一记录。strength=0 或确认未改变任何
规范源像素的笔触不产生历史提交；不能仅凭最终照片没变化判为无效，因为其他 Mask、invert 或
恒等 Grade 可能隐藏当前 Brush 的变化。无 Brush 时不允许创建空的 Erase-only Mask。

#### NM7.12R.5 — 默认羽化与参数含义

本阶段将新建 Brush 的默认源羽化确定为短边的 **0.5%**：`f0 = 0.005 * min(W,H)`。
保留现有初始半径 `r0 = 0.01 * min(W,H)`，因此初始羽化半宽为 `r0/2`。这是新增的产品默认值，
不是从其他 Mask 的不同单位参数直接复制来的数值。6000×4000 图像的初始 radius=40 px、
diameter=80 px、source feather=20 px。Strength=100%，dab hardness=1 保留，软边由同一个
source feather 求值，避免同时修改两套软边语义。

- 在 owner 的新建 Brush 操作中设置 `feather_radius`，在首个 provisional dab 发布之前可见，
  与首个 `AddMask` 一起持久化。不要只改 QML 数值，也不要全局改反序列化默认值。
- `brush.feather` 是整组累计 Brush 的源参数，单位为参考像素；不是每条笔触的新字段。改变
  Size 不联动已存在 source 的 feather，追加到旧 Brush 时保留它原本的 feather，包括显式 0。
- 未创建时面板显示下一次新建 Brush 的 feather，允许设置；创建后显示 owner 值。新建前参数
  调整不写历史。已有 Brush 的 Feather 拖动使用一个字段编辑和一次 settle；reset 使用 `f0`，
  用户仍可手动设 0。加载/重进编辑器不触发 setter 提交，不替换旧项目数值。
- 明确 Size 显示的是直径还是半径。本阶段面向用户显示“笔刷直径”，像素值为 `2r`，百分比与
  slider 两端也使用直径；adapter 的既有 radius API 仍用半径，只在面板边界转换一次并测试。
- 显示空心的 dab 半径轮廓与外羽化提示线，不能把提示线承诺为复杂累计 coverage 的精确等值线。
  默认 feather 必须从绘制中的第一帧使用，释放时不允许突然由硬边换成软边。

#### NM7.12R.6 — 持续绘制性能与所有权修复

**已有快慢差异的解释范围：** settle 后只调 Grade 参数可以复用已有效的 Mask 结果；画笔不断改变
source，会触发 sample 发布、重放、上传与羽化。两条路径开销不同是合理的，当前实现中的重复工作
必须去除。`GradeMaskCoverage` 的 host 区域重放测试通过，不证明 `cuda_mask_pass.cu` 等 native
生产入口已经使用区域重放；必须在真实入口统计调用次数和处理像素数。

按以下顺序实现，各步都保持完整分辨率、现有算法和用户选择的 backend：

1. **事件接收与显示：** GUI 仅做坐标映射、提交最小输入和更新独立 cursor。hover 更新为 O(1)；
   同一 Qt 帧只发布一次显示几何。创建引导线使用有界的派生显示数据或追加已有几何，禁止每事件
   复制全部 `brush_item_path_`。显示简化不得删改 canonical samples 或代替真实照片更新。
2. **有序批处理：** 保留每个 Paint/Erase 输入及真实参数变化边界，按序消费；一个 serial cycle
   内完成整批采样后发布一次 source 变化/dirty 通知和最多一次 Interactive 请求。Move 可合并
   最新绝对值，Paint/Erase 不能这样丢掉弯折。Finish/Cancel 不被限速吞掉，也不跨 sequence 合并。
3. **去除重复 draft 发布：** 参数没变时 `SetBrushStrokeParameters` 完整返回无变化结果；当前
   sampler 已避免重复参数 sample，但 controller 仍需避免无变化的 source 重组。Append 没有
   新规范 sample 时只更新 cursor，不发布 source revision。取消 `DraftStroke` 每次复制整个前缀
   及 `ComposeDraftBrush` 每次复制已提交列表的做法。
4. **最小 owner 操作：** `BrushMaskInput` 保持唯一未完成 sample 存储，通过有效生命周期内的
   const view/新增 sample 范围交给串行求值；`ColorGradeNodeModel` 保持唯一已提交 source。
   在 `WithLockedLiveDocument` 所属安全周期使用 owner 查询和 focused append/finish/cancel
   操作，不再复制 source、改副本、写回。Finish 一次移动 sample body 到现有 immutable
   `BrushStroke`，首次原子 AddMask，后续原子 AppendBrushStroke；取消通过丢弃 draft 和参数
   重放恢复结果，不增加 R8 before-state。跨线程不能保留指向可增长 vector 的 span。
   首笔在 Begin 分配 provisional `MaskId` 并在 live Grade 插入仅含源元数据的 Brush，草稿
   samples 仍只由 input owner 持有；同一安全周期以 `DraftSamples()` 和已提交 source 求出
   对应 active raster，再用现有 `ActiveRasterMaskInput` 按准确 NodeId/MaskId 送入 native
   Mask pass。这样 compiled Mask 仍能找到目标，预览无需把草稿整个复制进 document。
   Finish 把该 provisional Mask 的最终参数以一次 AddMask 提交，不能重复插入；Cancel 或
   首笔确认无效时移除 provisional Mask。之后每笔的 draft 也经同一 active raster 路由。
   request 持有的 raster 是必要算法输出，必须说明分配、reader、释放和 dirty 的有效内容来源。
5. **把局部更新接进 native 入口：** 从新增 dab 支持域形成 outward-rounded dirty，使用空间
   索引查询相交 stroke 并按原顺序重放，重算该区域内所有必要 Mask 的 Union。复用现有
   `brush_rasterizer`、`brush_spatial_index`、`brush_source_geometry` 和 active raster 入口；
   改进 `compiled_grade_mask.hpp` / `parameterized_brush_raster.cpp` 的完整重放调用链，不能
   只优化没有接线的 host helper。索引追加新段，普通 Append 不重新索引全部历史。
6. **保持单 Grade R8 限制：** 最终 Mix 已经丢失单个 Brush 的源值，不能在 Mix 上直接做 Erase
   或用它反推出源 coverage。source/feather 中间结果只用 executor 管理的临时 scratch，按需
   顺序复用；不新增每 Mask/Stroke/Version 的长期全图 R8。若上传资源不是同一合法旧内容，
   必须先完整初始化；不能给新纹理只上传 dirty 矩形。读者释放前不原地改已发布资源。
7. **默认羽化必须一起优化：** 现有 signed-distance pass 可能需要全域计算，dirty dab 小不代表
   distance field 同样局部。优先消除同一 cycle 的重复重放/上传/羽化，复用分配与 pipeline。
   若进一步做有限半径区域羽化，先从现有距离公式推导 dirty 扩展、读取 halo 与采样边界；
   对 inside/outside、擦出洞、贴边、全擦除及相邻旧 stroke，与完整羽化逐像素对照。没有证明
   等价前，不把全域羽化替换成只处理 dab 矩形。不能把 feather 临时设 0 或把 source feather
   改成另一种 dab 算法来获得性能。
8. **依赖与调度：** source 变化只失效受影响的 Mask/Union/Mix 和依赖结果，不重复 RAW decode
   或清掉仍有效的上游 Grade 结果。继续使用现有 serial admission、revision 和 reader 释放
   边界；磁盘写回不进入 pointer/owner 热路径。积压输入分批排空并保留顺序；容量不足时报告
   真实错误并取消未完成笔触，不丢样本或切换 backend。

**度量和通过标准：** 在同一 RAW、viewport、backend、构建和笔刷参数上对比修复前后，分别记录
默认软笔、小硬笔、大软笔、1/100/1000 条已有笔触、冷/暖缓存、单 Mask/重叠多 Mask。固定轨迹
包括直线、快速折线和圈，另测 125/500/1000 Hz 输入；sample 数、渲染次数和工作量同时报告。
使用 Release 做时延资格验证，Debug 做诊断，不拿 Debug CUDA 的慢作为改质量理由。

- 本阶段沿用 Section 12 的 16 ms Interactive owner-cycle 目标，并将普通默认笔刷暖态
  p95 ≤ 16 ms 作为通过条件；给出 p50/p95/max 与完整硬件说明，不能只报平均帧率。
- GUI 输入处理 p95 ≤ 1 ms；cursor 应在下一个可用 Qt frame 反映最新位置。60 Hz 空闲显示下
  event-to-cursor p95 ≤ 16.7 ms；故意阻塞 native render 时仍不等待 GPU 或文档锁。
- 默认笔刷暖态 event-to-photo p95 ≤ 33.4 ms（60 Hz），持续输入结束后积压必须排空；单独
  记录 source replay、upload bytes、feather、Union/Mix、queue depth 和 release-to-Quality。
- 同一无参数变化的轨迹分批方式不同，最终 samples/R8 完全一致；源列表发布次数不随原始
  pointer event 逐次增长；不存在反复复制长度递增前缀造成的累计二次工作量。
- 大软笔或高重叠的确切最坏成本必须报告；若默认场景未达到目标，状态仍为 partial，并在当前
  原算法/backend 内继续优化，不能以“其他参数已经流畅”判为通过。

#### NM7.12R.7 — 文件职责与调用链

| 位置 | 修复职责 |
| --- | --- |
| `ui/alcedo_main/qml/EditorWorkspace.qml` | 明确事件 parent/viewport 转换；有效 release/cancel；输入设备和单 sequence 路由；工具对应鼠标形状 |
| `ui/alcedo_main/qml/EditorAdjustmentHeader.qml`、`EditorMasksContextPanel.qml` | 首次 Brush 入口；Paint/Erase/Move 可用性；直径/Strength/Feather 的单位与 reset；source 加载只读 |
| `ui/alcedo_main/album_backend/editor_mask_creation_adapter.cpp` 及头文件 | typed source 分发；工具选择回执校验；独立 cursor；立即发布工具切换；消费 bounds，不持有可编辑 Brush 镜像 |
| `ui/editor_rhi/editor_interaction_controller.cpp`、presentation/session 边界 | 当前已显示帧的 geometry 和 identity；共享正反映射；操作中映射变化的取消 |
| `ui/edit_viewer/mask_overlay_layout.cpp`、`mask_overlay_geometry.cpp`、`ui/editor_rhi/editor_overlay_item.cpp` | Brush 边界四边形、四角锚点、平移命中、空心笔尖与 QSG 节点复用 |
| `app/editor_mask_creation_controller.cpp`、`brush_mask_input.cpp`、`app/editor_session_service.cpp` | 新建默认羽化；批量输入与一次发布；唯一 draft 生命周期；提交/取消；禁止 Paint 与 Move 交错 |
| `edit/graph/color_grade_node_model.cpp` 及头文件、`edit/mask/brush_*` | owner 的最小更新/只读查询、不可变已提交 samples、派生范围/索引、确定性重放 |
| `include/edit/runtime/compiled_grade_mask.hpp`、`edit/mask/parameterized_brush_raster.cpp`、native Mask passes、`PlanExecutor`/`GraphImageCache` | 区域工作接入真实 backend；正确资源初始内容、羽化依赖、单 Grade R8 和 revision/reader 安全 |
| `alcedo_main/DESIGN.md`、AppTheme | 记录 Brush cursor、移动框和参数含义；复用 crop/Mask 线宽与配色，新增 token 时同步定义 |

上表源路径均相对 `alcedo_studio/src/`。执行前统计完整文件 LOC。adapter/controller 当前已经很大，
新增 cursor/框范围逻辑应放入对应 geometry 模块；draft 生命周期由已有 `BrushMaskInput` 承担。
只有状态归属确实分离才抽出新类型，不用拆几个方法文件或新增整包可变 context 假装解耦。
新 public API 说明线程、owner、view 有效期、失败与原子更新边界；头文件包含类型定义。

**成功调用链（目标）：**

```text
Header Brush / Mask panel Paint or Erase
  -> adapter requests exact tool + target -> serial owner accepts -> ready controls
Qt valid pointer event -> map parent to viewport -> displayed G -> reference/local sample
  -> ordered input queue -> safe serial consume -> BrushMaskInput canonical sample append
  -> owner scoped source read + open-stroke view -> dirty replay / native feather / Union
  -> one current Grade R8 -> native Grade Mix -> accepted Interactive photograph
Release with valid endpoint -> finish canonical samples -> AddMask or AppendBrushStroke
  -> one durable history commit -> owner completion -> ready controls -> Quality
Move frame press -> BeginMove -> current-reference minus press-reference
  -> SetBrushTranslation live operation -> old/new domain replay -> Interactive pixels
  -> release -> one SetBrushTranslation history commit -> Move ready
```

**取消/失败调用链（目标）：**

```text
grab cancel / mapping identity change / invalid target / rejected command
  -> fence remaining sequence input -> owner Cancel -> discard open samples or restore translation
  -> recompute affected current result -> no unfinished history entry -> ready/error UI
late owner/frame completion -> compare session + target + sequence/revision -> reject stale display
allocation / native / history failure -> original error -> discard unpublished output
  -> keep valid committed document/history and prior successful displayed result
  -> report failed operation; no alternate algorithm, quality, backend, or cache directory
```

#### NM7.12R.8 — 必须新增或加强的验收

测试名称如下是待实现验收，不是已存在或已通过声明。复用现有 target；生产入口整合测试建议
新建 `EditorBrushInteractionQmlTest`（`tests/ui/editor_brush_interaction_qml_test.cpp`），加载
实际 workspace/header/panel + 真 adapter/串行 owner，而不是仅用 fake model 回显按钮值。
新目标必须注册到 `tests/ui/CMakeLists.txt` 并执行非零测试。

| 行为 | 测试名称 | 层次/目标与核心断言 |
| --- | --- | --- |
| 第一笔类型 | `FirstHeaderBrushPressProducesOnlyBrushSource` | 新 QML target；真实按钮后从 press 到 release 均为 Brush，只有规范 dab coverage，没有 Linear source/guide |
| 异步切换 | `DelayedAnalyticSelectionCannotReplaceArmedBrush` | 新 QML target + `EditorSerialMaskInteractiveTest`；延迟旧选择回执，Brush source/tool/目标不倒退 |
| 单击与释放 | `BrushClickRecordsOneDabAtReleasePosition`、`BrushReleaseDoesNotAppendResetOrigin` | 新 QML target；无 move 的单击一次提交；release 独有末段不丢，inactive 归零不产生左上拖尾 |
| viewer 偏移 | `BrushPointerMatchesPhotoPixelsWithOffsetViewport` | 新 QML target；非零父 Item 偏移、面板宽度变化及已知照片点，校验 cursor 与真实像素，不只比 mapper 往返 |
| 几何矩阵 | `BrushReferenceMappingMatchesIndependentCropRotationPoints` | `MaskEditGeometryTest`；横/竖图、orientation、crop、旋转、fit/100%/zoom/pan、DPR 1/1.25/1.5/2，手算预期点 |
| 帧几何 | `BrushMappingUsesPresentedFrameAcrossDetailPatch` | `MaskEditGeometryTest` + 新 QML target；完整参考图不被 Interactive cap/ROI extent 替换，几何变更取消未完成笔触 |
| 笔尖形状 | `BrushCursorMatchesMappedReferenceCircle` | `MaskOverlayControlTest`；非等比仿射下轮廓为正确椭圆；中心误差 ≤ 0.25 logical px，轮廓离散误差 ≤ 0.25 logical px |
| Paint 锁定平移 | `PaintingOverMoveFrameNeverChangesBrushTranslation` | 新 QML target + `AccumulatingBrushCreationTest`；新增笔触期间 translation 与既有 samples 不变，Move/键盘平移禁用 |
| 移动框 | `MoveFrameEnclosesTranslatedPaintSupportWithFeather`、`MoveFrameDragTranslatesAllStrokesWithoutScaling` | `MaskOverlayControlTest` + `AccumulatingBrushCreationTest`；四角/边/内部命中；照片 release 前变化；sample body 身份不变；一次平移提交 |
| 模式显示 | `PaintEraseAndMovePublishDistinctControlsImmediately` | 新 QML target；hover 无按键即可跟随；Paint/Erase 没有 Move 点；Move 没有笔尖；不出现缩放鼠标形状 |
| Erase 数学 | `EraseStrokeReducesCoverageAtPointerBeforeRelease` | `AccumulatingBrushCreationTest` + `GpuDagCudaMaskTest`；硬边满/半强度的手写 R8 预期、默认软边的独立距离预期，真实照片在 release 前变化 |
| Union 与平移 | `ErasingMovedBrushPreservesOtherMaskContribution` | `GpuDagCudaMaskTest`；正确 local 坐标、选中 MaskId、重叠 Radial 保留；错误目标像素不变 |
| 擦除历史 | `EraseUndoRedoRestoresExpectedSourceCoverage`、`ZeroStrengthBrushInputCreatesNoCommit` | `AccumulatingBrushCreationTest` + 现有参数笔触持久化测试；精确 HEAD/次数/重放像素；取消不留空 stroke |
| 默认软边 | `FirstBrushDabUsesDefaultSourceFeather`、`ExistingZeroFeatherBrushKeepsItsStoredValue` | owner + 新 QML target + native；首帧到 settled 参数/边缘一致；新建、reset、保存重开；旧值 0 保留 |
| 参数显示 | `BrushDiameterAndFeatherControlsUseReferencePixelUnits` | `EditorAdjustmentHeaderQmlTest` / 新 QML target；40 px radius 显示 80 px diameter，Feather=20 px/0.5%，load-only 零提交 |
| 批处理 | `OrderedBrushBatchPublishesOneChangedSourceRevision`、`UnchangedBrushParametersDoNotRepublishDraft` | `EditorSerialMaskInteractiveTest`；Append sample 不丢，参数没变不重复发布；每个消费批次最多一个 source 通知/Interactive 请求 |
| 重放等价 | `PartitionedBrushInputProducesIdenticalCanonicalPixels`、`NativeRegionalBrushUpdateMatchesCompleteReplay` | `BrushCanonicalSamplerTest`、`BrushRegionalReplayTest` + `GpuDagCudaMaskTest`；不同事件分组同样本同 R8，其他 Mask 的幸存贡献一致 |
| 羽化区域 | `RegionalBrushFeatherMatchesCompleteFeatherAtDirtyBoundary` | host/native；halo、洞、图像边缘、全擦除、不同半径；未实现区域羽化时仍验证完整原算法，不能声称区域优化通过 |
| 响应与资源 | `BusyNativeRenderDoesNotBlockBrushCursorOrCancel`、`BrushDrawingKeepsOneRetainedGradeCoverage` | 新 QML target + `EditorSerialMaskInteractiveTest` + native；延迟 reader 时 GUI 可动、无同步等待/源竞争，无每笔 R8 文件 |

**比较方法：** 原始 canonical R8 使用手算/独立完整重放，要求逐字节一致；native 插值/羽化结果
沿用已有容差，通常最多 1 个 R8 code，不能为通过测试放宽。照片用非恒等 Grade 和确定的输入
像素验证 Mix 公式，并给出最大误差、失败坐标。像素质心比较要计入明确的 R8/native 采样误差，
不能把亚像素栅格离散误差与系统性坐标偏移混为一谈。几何正反变换是补充，不替代照片像素。

**生产 UI 资格验证：** 在 260/320/460 px 面板宽、两种主题及 reduceMotion 下完成六项场景；
用真实 Qt 鼠标事件按按钮和绘制，保留 Windows D3D11 overlay 与 CUDA 照片的截图、像素证据
及 NM7.12R.6 时延记录。没有对应设备的 Metal/OpenCL 项标为未执行，不用另一 backend 代测。
未来执行时遵守 MSVC wrapper，并从 `ctest -N` 确认实际注册目标；命令和完整结果写回本节。

**实施顺序：** R.1 先加失败复现与工作量计数；R.2/R.3 完成映射和工具隔离；R.4/R.5 完成擦除
和默认软边；R.6 在默认羽化开启的真实管线上优化；R.8 完成 UI/native/时延证据。编号按本节引用，
不得进入生产标识符、测试文件或目标名称。

**退出清单（本次全部保持未勾选）：**

- [ ] 第一笔及从解析型 Mask 切换后的第一笔，无 Gradient source/控件串入。
- [ ] 指针、QSG 笔尖、reference/local samples 与实际照片在完整几何矩阵内对齐。
- [ ] Paint/Erase 与 Move 输入完全分离；移动框有四角锚点，整体移动而不缩放。
- [ ] Erase 在 release 前改变当前 Brush 的真实 coverage，Union、取消、Undo/Redo 均正确。
- [ ] 新 Brush 第一帧即带默认羽化，参数单位/reset 一致，旧项目数值不被重置。
- [ ] 默认羽化开启时通过绘制时延目标；无完整前缀重复复制，无未授权的质量/backend 替换。
- [ ] 生产 QML、owner、native 像素和异步测试均有非零执行记录；结果与截图可追溯。
- [ ] 无新增 source 镜像或未说明必要性的拷贝；只读 view 生命周期、唯一 owner、reader 边界明确。
- [ ] 同步记录真实修复文件、主要调用链、测试数量、工作量/时延分布及未执行平台。

**本次规划记录：** 阅读相关生产调用链和既有测试入口，核对上列 Qt 官方文档；只修改本计划的
状态、依赖表与 NM7.12R 节。历史 NM7.12 完成记录不删除，六项修复不标记完成。

##### Phase NM7.12R implementation record (2026-09-10, win_debug, CUDA 12.8)

**Status:** partial — all six regressions have wired implementation and unit/host/GPU
coverage; release-build latency qualification, the real-adapter QML harness, and Metal
execution remain open.

**Primary success call chain:**

```text
Header Brush / panel Paint-Erase -> adapter stamps source_kind + tool + diameter/feather
  -> queued BeginInput (ordered, identity-fenced)
  -> EditorSessionService::ApplyMaskCreationCommand (serial, per-batch)
  -> controller BeginMaskInput(kind fence) -> BeginBrushStroke
  -> BrushMaskInput canonical samples -> provisional ApplyLiveBrushDraft / AppendBrushStroke
  -> workspace ParameterizedBrushReplayCache (regional dirty, retained index + shared pixels)
  -> ActiveRasterMaskInput(content_revision, dirty_rectangle)
  -> CUDA/OpenCL partial upload -> mask Mix -> presented frame
Release -> FinishBrushStroke -> AddMask (first stroke) / AppendBrushStroke -> Settling
Move drag -> BeginMaskMove(BrushMove fence) -> SetBrushTranslation live ops
  -> release -> one SetBrushTranslation commit
FramePresent -> submission.geometry = plan.geometry -> DirectFrameSink::NotifyFrameReady
  -> EditorViewportItem::NotePresentedMaskGeometry (stale-id fence)
  -> PresentedMaskGeometryChanged -> interaction->setDisplayedMaskGeometry
  -> maskEditViewMapping().geometry used by adapter MakeSample + overlays
```

**Primary failure / fencing chain:**

```text
mapping identity change mid-stroke -> adapter CancelIfMappingChanged
  -> owner Cancel -> discard open stroke samples -> controls back to armed
stale pointer identity / wrong source kind -> owner Reject before mutation
history publish failure -> RestoreLive + Failed state, no half-committed document
dirty replay throw -> entry invalidated; next call re-rasterizes full
erase on no mask / Move on no selection / wrong handle kind -> owner Reject
```

**What was proven (executed tests):**

| Criterion | Target / binary | Result |
| --- | --- | --- |
| Brush creation/erase/move/undo/no-commit | `AccumulatingBrushCreationTest` | 14/14 PASS |
| Analytic fencing, kind separation, handle drag | `AnalyticMaskCreationTest` | 34/34 PASS (mask_creation+edit labels) |
| Reference/item mapping, presented geometry | `MaskEditGeometryTest` | PASS (mask_edit label, 5/5) |
| Move frame quad, corner ticks, dashed cursor, hit-test | `MaskOverlayControlTest` (3 new cases) | 16/16 PASS |
| Regional replay == full evaluation, erase undo, union | `BrushRegionalReplayTest` | 6/6 PASS |
| Replay cache identity/dirty/eviction | `ParameterizedBrushReplayCacheTest` (new) | 10/10 PASS |
| Sampler determinism under event grouping | `BrushCanonicalSamplerTest`, `BrushSpatialIndexTest` | PASS |
| CUDA dirty-rect upload, feather, union | `GpuDagCudaMaskTest` (+ fixtures) | PASS |
| OpenCL dirty-rect upload, revision fencing | `GpuDagOpenClGradeTest.OpenClMaskFixture` | PASS (50/50 GPU mask tests) |
| Diameter semantics, Erase gating, tool switcher | `EditorAdjustmentHeaderQmlTest` | PASS (workspace_qml label) |

Commands: `scripts/msvc_env.cmd --build --preset win_debug --parallel 4`;
`ctest -R <suites> --output-on-failure` from `build/debug`.

**Checklist (honest state):**

- [x] First press creates `BrushMaskSource` only; analytic creation is kind-fenced.
- [x] Pointer mapping consumes the presented frame's `ResolvedRenderGeometry`; mapping
      changes cancel open strokes.
- [x] Paint/Erase/Move are distinct inputs and controls; Move frame uses translated
      Paint-support bounds with corner ticks and interior hit-test.
- [x] Erase replays `min(prev, 255-dab)` regionally; verified on host, CUDA, OpenCL.
- [x] New Brushes apply `0.5%`-of-short-edge default feather from the first dab;
      existing explicit 0 is preserved.
- [x] Continuous drawing replays only dirty regions through a retained workspace cache;
      immutable shared pixel snapshots; CUDA/OpenCL upload dirty rectangles.
- [ ] `EditorBrushInteractionQmlTest` (real workspace + real adapter + serial owner)
      — not created; QML coverage is header/panel level.
- [ ] Release-build p95 ≤ 16 ms drawing latency measurement — not executed.
- [ ] Metal pass — source updated for the new API; not compiled or executed (no Metal
      host in this environment).
- [ ] Production viewer evidence at 260/320/460 px, both themes, real pointer events —
      not executed.

**LOC note:** largest touched files — `editor_mask_creation_adapter.cpp` (~1500 LOC,
kept; brush cursor/geometry lives in `mask_overlay_layout`/`mask_overlay_geometry`),
`editor_mask_creation_controller.cpp` (dispatch + stroke lifecycle), new
`parameterized_brush_replay_cache.hpp/.cpp` (~200 LOC, workspace-retained).

**Remaining gaps:** latency qualification on a release build, real-adapter QML harness,
Metal compile/run, and full-viewer pointer evidence remain open and must be reported
against NM7.12R acceptance rather than assumed.

### NM7.12RR — 一次完成连续绘制、擦除稳定性和真实管线性能修复

**Date / source:** 2026-09-10，调查工作树 HEAD `fb95653f`。**Status:** planned。
本次只研究当前实现、核对 Qt/CUDA 一手资料并制定方案；没有重新运行用户的 RAW 绘制过程，
没有测得各环节耗时。以下区分源码可确认的执行行为、可以数学证明的判断问题和待复现的因果链。

**本阶段必须交付的整体能力：** Brush/Erase 从第一次 press 起即可连续拖动，不需要先点一下；
擦除在绘制、释放、Quality 回帧、下一次编辑和 Undo/Redo 后保持正确；默认羽化开启时，实际照片
随输入连续更新，消除秒级逐帧重绘。Windows CUDA/D3D11 是本次用户问题的直接资格验证路径。
共享接口改动必须保持 OpenCL/Metal 的既有语义，分别列出编译/执行证据，不能用其他 backend
替代 CUDA，也不能将未运行的平台说成通过。

**执行约束：** NM7.12RR 只有一个实施范围、一个完成记录和一套退出条件。下文各项是同一阶段
内必须完成的工作，不再生成 RR.1、RR.2、A/B 或新的补修阶段。输入、擦除、GPU、资源生命周期、
真实 QML 测试和性能测量都完成才可写 complete；不能再次以“实现已接线，实际拖动/时延以后测”
作为交付。NM7.12R 的历史记录保留，其未完成的真实 adapter harness 和本机时延验证并入本阶段。

**对“是否每帧生成 R8、是否没有走 GPU”的回答。** 当前路径是 CPU 与 GPU 混合，不是全 CPU，
也不是已有 GPU 计算就自然足够快：

```text
Qt input -> adapter -> ordered queue -> serial controller
  -> DraftStroke copies growing samples -> ComposeDraftBrush -> live source replacement
  -> ParameterizedBrushReplayCache -> CPU index rebuild / dirty-region rasterization
  -> shared host R8 -> CUDA active texture acquire -> initial full / later dirty upload
  -> full mip generation -> full inside/outside distance transform when bytes changed
  -> feather sampling -> enabled Mask Union -> Grade Mix -> D3D11 photograph presentation
```

`ParameterizedBrushReplayCache` 已保留 host 像素，`ActiveRasterTextures().Acquire` 也有 GPU
资源复用，所以**不能说每一帧必然新分配一张纹理**。但源码仍显示下列重复计算，必须分别计时：

| 已确认的执行行为 | 精确位置 | 性能或正确性含义，及尚缺的证据 |
| --- | --- | --- |
| 当前 canonical R8 长边上限为 4096，按参考图比例向上取整 | `brush_raster_encoding.cpp::CanonicalBrushRasterExtent` | 6000×4000 参考图产生 4096×2731，约 10.67 MiB R8；不是每次完整 6000×4000 R8，也不能进一步降低上限来掩盖慢 |
| 增长中的 draft 每次生成新的完整 sample body，source 列表也复制 | `BrushMaskInput::DraftStroke`、`ComposeDraftBrush`、`UpdateBrushPaint` | 无新增 dab 的事件已有早退，但有新 dab 时仍复制旧前缀；一个 batch 的每条有效 Append 仍会发布 source，不能把一批一次 render 当作一批一次 source 更新 |
| dirty 比较依据 sample body 指针；增长的同一 StrokeId 会计算整条旧/新 stroke 的支持域；每次非空 dirty 重建完整索引 | `parameterized_brush_replay_cache.cpp::BrushReplayDirtyTexels / Replay` | “区域重放”不是“仅新 dab”；长笔划越画越贵，弯曲路径的大包围盒包含大量空白。现有 `RegrownDraftStrokeReplaysItsGrownSupport` 测试正好接受这种扩大行为 |
| CPU raster 内逐 dab、逐像素计算距离、覆盖与 R8 写入 | `brush_rasterizer.cpp::StampDab / ReplayRegion` | overlap 多时大量重复 host 工作；仅做 GPU 上传局部化没有消除 CPU 栅格化成本 |
| 每次更高 content revision 上传 dirty 后仍生成完整 mip，并标记 `raster_bytes_changed=true` | `cuda_mask_pass.cu::ExecuteCudaMask` | 只改 feather/invert/opacity 或相同像素的 revision 也要核对是否被当成 source 像素变化；空 dirty 伪造 1×1 上传会触发多余工作 |
| 源像素变化后，inside/outside 两套 EDT 都扫描完整 canonical raster，再组合距离场 | 同文件 `must_compute`、`ParallelBandHorizontalKernel`、`ParallelBandVerticalKernel` | 羽化不是 CPU 算，但每行/列一个 block，且 launch 的 block size 是 **1**；单线程串行扫描整行/列，不能仅凭函数名把它视为高并行实现 |
| 纵向 EDT 每个 block 的动态 shared memory 为 `height*(sizeof(int)+sizeof(float))` | 同文件 vertical kernel launch | 4096×2731 源约 21.34 KiB/block，竖图高度 4096 时为 32 KiB/block，却只有一个执行线程；需用 profiler 验证实际占用率、访存与 GPU 时间 |
| 距离场及 horizontal/inside/outside 共四个源大小 float buffer | 同文件 scratch 分配 | 上述 4096×2731 示例约 170.69 MiB，仅是这四个 buffer；实际峰值还含 R8、mip、Mask outputs 与照片。分清保留分配和逐帧计算，不能靠一个 R8 字节数解释全部成本 |
| 本地 Debug 的该 `.cu` 实际编译 flags 含 `-G` | `build/debug/build.ninja` 的 `cuda_mask_pass.cu.obj`；根 `CMakeLists.txt` 的 CUDA Debug 选项 | device debug 会抑制优化，是放大器；尚未确认用户运行的 exe/DLL 就是这份 Debug。必须核对进程模块和同 commit Release，不能把换 Release 当成全部修复 |

CPU/GPU、分配/计算、submit/complete 必须分别计时。使用 [CUDA 12.8 性能指南](https://docs.nvidia.com/cuda/archive/12.8.0/cuda-c-best-practices-guide/index.html)
分析访存、warp 与 shared memory 约束，按 [NVCC 12.8 文档](https://docs.nvidia.com/cuda/archive/12.8.0/cuda-compiler-driver-nvcc/index.html)
识别 `-G` 与优化配置。GPU launch 的 CPU 返回耗时不是 kernel 耗时；使用同 stream CUDA events，
需要时间线时使用 [Nsight Systems](https://docs.nvidia.com/nsight-systems/UserGuide/index.html)，
不能为计时在每个子步骤增加同步而改变原始调度。

**先把三个用户现象放进同一条可追踪输入序列。** 从生产 Header 点击 Brush/Erase，直接 press →
移动至少 5 个非共线点 → release；不提前点照片、不提前给 adapter 注入选择。对比“先单击后拖动”
并分别覆盖冷首帧、Quality 已显示、Interactive 已显示、首次创建/已有 Brush、快速连续两笔。
记录 `device/point/sequence`、按钮、事件阶段、Qt grab、adapter open/tool/target、owner state、
sample 数、command kind、content revision、frame role、request id 与 terminal reason。
计数与时间戳写入可开关诊断，临时结果仅存 `build/tmp/brush_continuous_input/`。

重点排查以下路径，取得真实日志或失败测试后在同一阶段修复：

- `EditorWorkspace.qml` 当前仍由 PointHandler 转发，inactive 回调直接当 Release；没有在这里
  区分正常释放与取消。`handlerPoint` 在释放后可能归零，不能把它作为终点。设备 id/point id
  也不能由 adapter 固定为 1。Qt 对这些行为的说明见 [handlerPoint](https://doc.qt.io/qt-6/qml-qtquick-handlerpoint.html)。
- PointHandler 是被动 grab，Qt 文档说明它可继续观察其他独占 grab 期间的移动；不能照抄旧
  QML 注释就断定 TapHandler 一定吞掉它的 move。实际查 active/point/grab 的交付顺序，见
  [PointHandler](https://doc.qt.io/qt-6/qml-qtquick-pointhandler.html)。当前 DoubleTap 仍只按
  `editorControlsEnabled` 开启；Mask 拥有左键时需要隔离双击缩放，防止额外的 view change。
- `CancelIfMappingChanged` 使用 `MaskEditMappingIdentity` 的逐字段相等；其中包含 render
  extent、render_to_reference 和呈现模式。Quality/Interactive 换表示时可以字段不同而最终
  指针映射相同。`ConnectInteraction` 对任何 `viewChangeReported` 都取消也需一并审计。
  意外 Cancel 会调用 RestoreLive，能造成 draft Erase 的像素重新出现；这条链是高优先级
  假设，但本次未复现，不能写成用户问题已经定位。
- `DirectFrameSink::NotifyFrameReady` 在最终 slot gate 之前发送 geometry，
  `NotePresentedMaskGeometry` 主要按 request id 递增接纳。必须证明 geometry 属于实际已接纳
  显示的帧，而不是“准备好但未展示”的帧；图像/session identity 也必须参与校验。
- Erase 回现还要区分四种结果：draft 被 Cancel、Finish 未写入 Erase、host/GPU source 不一致、
  较旧照片覆盖新照片。逐层读取同一 revision 的 source 字节、effective Mask、Union 和最终帧，
  不用“有缓存”或“UI 问题”代替具体失败原因。

**输入修复必须保证的行为。** 工具 arm 和第一笔 Begin 保持顺序；在 owner 尚未返回新 `MaskId`
时也能接收同一 sequence 的后续点。每条有效 move 全部到达规范 sampler；保留弧长插值和参数
变化边界，不采用仅保存最后坐标的合并。press 立即记录首个 dab，release 使用仍有效的事件终点，
只产生一个 Finish；cancel/grab loss 只产生一个 Cancel。下一笔排在前一笔 Finish 后即可接收，
不等待 Quality 或缓存落盘。旧工具/旧笔回执不得清空当前新笔或把 Erase 改回 Paint。

保留单一真实事件写入者。先修现有 PointHandler 的路由和终止语义；若其 QML 回调确实不能在
当前 Qt 6.9 保证有效 release，则将这套完整 press/move/release/cancel 接收到一个 Qt Quick
输入 Item，由其持有该设备点的输入；不能再额外加第二个 DragHandler 同时向 owner 写 sample。
hover 只更新 cursor。pan/crop/双击缩放按工具所有权仲裁；越界释放仍结束原笔，第二触点和
合成鼠标不能触发重复提交。沿用 [PointerHandler 的取消与 grab 信号](https://doc.qt.io/qt-6/qml-qtquick-pointerhandler.html#signals)。

**用最终几何判断输入是否仍有效。** 把 item → ReferenceSpace 的组合变换作为同一性的基础：

```text
F = render_to_reference * diag(render_width, render_height, 1)
    * displayed_uv_to_photograph_uv * item_to_displayed_uv

6000×4000 Quality:     render_to_reference = I,             extent = 6000×4000
3000×2000 Interactive: render_to_reference = diag(2,2,1),   extent = 3000×2000
Both give identical F when viewport, crop, orientation, zoom and pan are unchanged.
```

上述两组 raw fields 不相等，数学上的组合映射却相等；直接按字段取消会把正常换分辨率当作
坐标变化。对两个 affine `F` 比较有效照片域的角点及测试点，使用明确浮点误差界限，并与原有
≤0.25 logical px 控件精度一致；image/session、参考图、crop/orientation 语义身份单独校验。
不把任意近似变化视为相等，更不能整体移除变化校验。等价 Quality/Interactive/ROI 表示切换
保持笔触；真实缩放/平移/裁切变化按既定策略有序取消，并给出原因。映射发布与呈现 frame 的
接纳保持同一顺序，修复前后都用独立已知照片点验证，不只做 mapper 自己的往返。

**Erase 的来源、提交和帧发布必须连续一致。** 每条 stroke 的 Paint/Erase mode 在 Begin 固定，
只作用于准确的 Grade/Mask。继续使用规范 R8 的 `paint=max(b,a8)`、`erase=min(b,255-a8)`；
GPU 并行不能打乱顺序。满强度擦除后的源像素在没有后续 Paint/Undo 的情况下不得增大。
该单调性先断言原始 Brush R8；羽化、invert、其他 Mask max 对最终照片的影响另按公式验证。

资源更新以成功完成为准，不能仅以“已排队上传”或较大 revision 判定内容有效：

- 当前 cache 的 `SharedPixels()` 返回 const handle，但其底层 buffer 仍被 rasterizer 原地
  修改；const shared_ptr 不提供不可变性。明确串行 reader 的持有区间，在所有 CPU/GPU
  reader 结束前禁止覆盖，测试故意保留 reader。不能为解决它默认每帧复制整张 R8。
- CPU replay 的 dirty 与 GPU texture 的已完成 base revision 必须匹配。跳过、中断、失败的
  upload 后，下一次 dirty 若只相对最近 host source，就可能漏掉较早的擦除；按 GPU 实际
  base 累积变更区域，或在内容未知时从参数执行同一精确重建，不能继续用未初始化旧像素。
- `SetUploadedPixels`、distance field、effective coverage、Union/Mix 的 pending/completed
  revision 分开维护，失败使相关未完成结果失效；不得让失败的源更新保留“已上传成功”标签。
  session/geometry/algorithm/storage identity 改变时清理对应资源，不靠默认 generation=1。
- 最终照片的接纳除 request 序号外还应验证当前 document/edit revision 与 session。已经显示
  Erase revision N 后，不得显示未包含该 Erase 的旧 Quality/Interactive 结果，即使其 request
  编号较大。取消恢复帧应是当前 owner 新的合法状态，不能伪装成陈旧帧绕过校验。
- Finish 必须提交完整已接受的 Erase sample；无效笔判断不能误丢真实变化。
  `BrushEraseOverlapsPaint` 当前是 erase×paint dab 的双重遍历，使用空间索引避免 release
  时另一个长停顿；同时验证无重叠/已擦空/半强度/移动后擦除/下一笔 Paint 的语义。
- cache 错误注入验证条目原子失效。NM7.12R 记录称 replay throw 会 invalidate，但当前 Replay
  函数未见对应事务/异常清理；需实测并落实，不能继承记录中的保证。

**性能实现目标：CPU 管输入和有序命令，GPU 管像素。** 本机 CUDA authoring 热路径应去掉
host 全图 R8 的生成/复制/上传往返。R8 仍是现有 coverage 格式，canonical 分辨率、量化、笔触
算法版本、source feather 与最终 Mix 语义保持不变；转移计算设备不授权修改图像质量。

同一个 phase 内完成以下整套实现，不能只优化其中一层：

- `BrushMaskInput` 唯一持有可增长 draft，已提交 samples 仍由 document owner 持有现有
  immutable body。通过安全作用域读取和明确新增 sample 范围消费；一个 serial batch 内
  接收全部有序点后，仅发布一次 changed source revision/Interactive 请求。取消每事件
  `DraftStroke` 全前缀复制、`ComposeDraftBrush` 列表复制与 source JSON 往返，Finish 一次
  移动最终 body 并发布历史。draft/source 不新增平行镜像。
- 为 GPU 上传新增规范 dab/参数边界和所需 tile 索引，按实际变更上传命令；同一 stroke 的
  未变前缀不重传。tile 中候选以 stroke/sample 顺序排列，每个输出像素按此序列计算 max/min。
  不允许 Paint 与 Erase 对同一像素无序原子竞争。与现有 CPU 独立 oracle 比较 canonical R8
  逐字节相等，包含半值量化、边缘、低强度、半径/硬度变化和重叠；不得用 fast-math 改变边界。
- dirty 从新增 sample 支持域/实际 owner change 描述生成，索引只追加新增范围。Move、Undo、
  删除、取消则涵盖 old/new 支持域，并重放所有相交贡献。用 tile 集合表达弯曲路径，避免把
  整条长笔的包围盒当作每帧 dirty；若 dense/全域变化需要完整求值，明确统计其工作量。
- 在 `PlanExecutor` 和 `cuda_mask_pass.cu` 的真实生产入口接入 GPU source 求值，不仅新增
  独立 benchmark kernel。移除本机生产 authoring 对 `ParameterizedBrushReplayCache` host
  像素缓存的依赖；CPU rasterizer 可作为独立测试 oracle，不作为 GPU 出错时的替代路径。
- 用真正并行且精确的 Euclidean distance transform 替换当前每 block 单线程行/列扫描。
  采用分带求解和并行合并的精确方法，可参考作者的 [Parallel Banding Algorithm](https://www.comp.nus.edu.sg/~tants/pba.html)。
  保留 inside/outside 分类、半像素边界修正、部分 coverage 的距离赋值、双线性距离采样和
  smoothstep 公式；不是换一个视觉相似的 blur。不能只把 block size 改为 256 后继续让
  `threadIdx.x != 0` 的线程退出，也不能用未证明等价的近似距离算法代替。
- 优先完成原分辨率精确 GPU EDT，使默认羽化下的全域必要计算也有效并行。区域距离优化如
  为达标所需，则同阶段证明读取 halo、dirty 扩展、全擦除/无 inside 或 outside 和纹理边界；
  特别注意现有路径先插值距离再羽化，不能未经证明先截断距离。没有等价证明就保留精确
  全域计算并继续优化它，不降低 feather 或先硬边预览、release 再软边。
- 区分 source-pixel revision、feather 参数和最终 Mask-field revision。source 未变且距离
  scratch 仍在合法使用期内时，调整 feather 半径复用该距离，invert/opacity 只重算下游。
  缺少有效中间结果时从规范命令做必要的精确 GPU 求值，并记录重建原因；不能为了声称零重算
  延长每 Mask 全图 scratch 的有效内容保留期。Grade 自身参数改变应复用仍有效的最终 Mix。
  未改变内容不再伪造 1×1 上传。soft 路径不生成根本不被读取的 R8 mip；需要 mip 的路径按
  依赖更新并检查正确采样，不能粗暴删除所有 mip。
- 当前 replay cache 每 `(Grade,Mask)` 保留一张 host R8（最多四项），已经与原计划“每 Grade
  一个当前 Mix、source 只作临时 scratch”的要求有差距。此次不扩成每 Mask 的长期 GPU R8
  副本。继续只保留每 Grade 的当前 Mix；源覆盖、EDT 工作区和 Union 中间结果由 executor
  scratch 顺序复用，允许池保留分配，但不保留未经授权的多套有效历史内容。冷 cache 从命令
  精确重建，失败显式返回；不能从最终 Mix 反推单个 Brush 源像素来做 Erase。
- 新纹理/新 tile 首次使用先初始化完整依赖区域；source buffers、published outputs 和 reader
  fence 生命周期写在定义处。必须等待的 GPU 边界只发生在 worker 的安全周期，GUI 不等待
  `WaitIdle`、源重放、history 锁或缓存 I/O。保留有效 RAW/上游 Grade 结果，避免 Mask 编辑
  重新 decode。照片依然在 press 到 release 之间实际更新，cursor 流畅不能替代照片流畅。

**实施位置与责任。** 路径相对 `alcedo_studio/src/`；新增类型按职责命名，不把 NM 编号写进代码。

| 文件/模块 | 本阶段结束时的责任 |
| --- | --- |
| `ui/alcedo_main/qml/EditorWorkspace.qml`、`ui/alcedo_main/album_backend/editor_mask_creation_adapter.*` | 一个输入写入者、真实终止事件、首拖不依赖预点击、模式仲裁与回执身份、独立 cursor |
| `ui/edit_viewer/mask_edit_geometry.cpp`、`ui/editor_rhi/editor_interaction_controller.cpp` | 比较实际 item→reference 映射；等价表示切换不断笔，真实几何变化明确取消 |
| `ui/editor_rhi/direct_frame_sink.cpp`、`editor_viewport_item.cpp` 及 presentation queue | geometry 与已接纳照片同序发布；session/edit revision 防止旧擦除前帧覆盖新结果 |
| `app/editor_mask_creation_controller.cpp`、`brush_mask_input.cpp`、`editor_session_service.cpp`、`edit/graph/color_grade_node_model.*` | 唯一 draft 与 source owner，批量规范采样/一次更新，完整 Finish/Cancel、最小 owner mutation、history |
| `edit/mask/brush_spatial_index.*`、`brush_source_geometry.*`、`brush_placement.hpp` | 追加索引、精确 dirty、Erase 有效性候选查询；现有 CPU raster 保留独立 oracle 职责 |
| `edit/mask/parameterized_brush_replay_cache.*`、`include/edit/runtime/compiled_grade_mask.hpp` | 清理 host raster 热路径与源镜像；若其他调用仍使用旧接口，明确 lifetime、invalid-base 和 error 行为并测试 |
| `edit/runtime/cuda/cuda_mask_pass.cu` 及所需 GPU mask 模块 | GPU 有序 raster、并行精确 EDT、必要采样/Union；避免把更多职责塞进单个 kernel 文件 |
| `include/edit/runtime/basic_render_workspace.hpp`、active texture/result cache、PlanExecutor | scratch/reader 生命周期，pending/completed revision，实际依赖失效与复用；接口同步检查 OpenCL/Metal |
| `tests/ui`、`tests/app`、`tests/edit` 的对应测试与 CMake | 真实 production QML 输入整合、异步复现、CPU/GPU oracle、原算法性能统计；必须注册并执行 |

**一套完整验证矩阵。** 以下名称是本阶段新增/加强的验收目标，不是已执行结果。
必须建立 `EditorBrushInteractionQmlTest`，加载生产 workspace/header/panel、真 adapter、真
串行 owner；只用 fake mask model 断言按钮调用，不能证明首拖/回现已修复。

| 用户行为或保证 | 必需测试 | 核心断言 |
| --- | --- | --- |
| 首次直接画 | `FirstBrushDragWithoutPriorClickPaintsWholePath` | fresh image/工具首次 arm 后直接 Qt press-move-release；非共线路径中段及末段 coverage，准确一次提交，无预点击/预选 MaskId |
| 首次直接擦 | `FirstEraseDragWithoutPriorClickErasesWholePath` | 切 Erase 后直接拖动；沿路径源像素减少，释放后继续保持；不只断言首个圆 |
| 连续操作 | `QueuedSecondStrokeSurvivesFirstStrokeCompletion` | owner/render 延迟时快速两笔，点/工具/目标不丢；不能等待 Quality 才接受第二笔 |
| 等价呈现几何 | `QualityToInteractiveEquivalentMappingKeepsBrushOpen` | 手算等价 F，换 extent/ROI 表示后仍保持同一笔；真实 zoom/crop change 的 Cancel 单独断言 |
| 事件终止 | `ReleasedBrushUsesValidEndpointAndCommitsExactlyOnce`、`CanceledBrushDoesNotCommit` | 真实 release 末段、归零后的 inactive、越界、重复终止、合成事件、grab cancel；不补原点、不重复提交 |
| 完成后不回现 | `ErasedCoverageSurvivesReleaseQualityAndNextEdit` | 绘制中、release、settled Quality、下一次 opacity/Grade edit、切页后连续比较 source/Union/photo；非恒等 Grade |
| 旧帧不能覆盖 | `LatePreEraseFrameCannotReplaceNewerErasePixels` | 延迟旧 Quality/Interactive 的不同到达顺序，含更大 request id 携带旧 edit revision；已显示的新擦除结果不倒退 |
| base revision 正确 | `FailedOrSkippedUploadPreservesAllPendingEraseRegions` | 两个相离的擦除区 A/B，中断 A 上传后应用 B，两处最终都正确；新纹理初始化、cache eviction、session 重建 |
| reader 不被改写 | `HeldMaskReaderPreventsRasterMutationUntilRelease` | 故意持有实际 consumer，跨下一次 replay/异常/geometry change；没有同时读写或伪不可变 handle |
| GPU 像素完全对应 | `GpuOrderedBrushRasterMatchesCanonicalR8Bytes` | 同样规范 samples 的 Paint/Erase 顺序、交叉、不同分批、hard/soft dab、量化边界逐字节相同 |
| 羽化不降质 | `ParallelGpuDistanceMatchesIndependentEuclideanDistance` | 独立精确 EDT 与 native source/feather；inside/outside 全空、洞、边缘、portrait；沿用既有 ≤1 R8 code 容差，不放宽 |
| 工作量有界 | `GrowingBrushUpdatesOnlyNewSampleCommandsAndAffectedTiles` | 100/1000 笔与长单笔，命令上传与新增样本相关；不重复复制前缀/重建完整索引/逐次扫完整笔划包围盒 |
| 参数改变复用正确 | `FeatherOpacityAndGradeEditsReuseValidBrushSourceWork` | Grade 参数改变复用有效 Mix；合法存活的距离不因 feather 参数变化失效；没有 host R8 上传；scratch 已释放时明确计数必要重建 |
| 历史/合成 | `EraseUndoRedoAndMovedBrushMatchFreshCommandReplay` | 移动后擦除、重叠其他 Mask、Undo/Redo、保存重开；相同参数产生同样 coverage，不依赖旧 cache |
| 真照片响应 | `BusyMaskRenderKeepsPointerInputAndCancelResponsive`、`ContinuousBrushPresentsIntermediatePhotoFrames` | 压住 native completion 时 GUI 仍接收，松开前至少多个真实照片帧，不允许 cursor-only 通过 |

已有 `AccumulatingBrushCreationTest`、`EditorSerialMaskInteractiveTest`、`MaskEditGeometryTest`、
`ParameterizedBrushReplayCacheTest`、`BrushCanonicalSamplerTest`、`BrushSpatialIndexTest`、
`BrushRegionalReplayTest`、`GpuDagCudaMaskTest` 和 frame queue 测试按实际代码复用/扩展。
保留独立 oracle；不能用同一 GPU/mapper 实现生成 expected 再比较自身。测试发现零案例、只编译
通过、只做 96×64 小图，均不构成实际绘制资格验证。

**性能验收也必须在这个 phase 完成。** 执行前确认用户运行 exe/DLL 路径、提交和编译 flags，
保存同 commit Debug 与 Release 对照；Release 实际 flags 不能含 `-G`，profiling 可用正确
构建中的行号信息。仍保留 Debug 调试用途，不把删掉 Debug 配置作为算法修复。
参考图至少覆盖 6000×4000 横图、4000×6000 竖图和更大实际 RAW；记录其 canonical extent。
默认 feather=短边 0.5%，另外测试硬边、大软笔、1/100/1000 strokes、1/5 个 Brush、重叠解析
Mask、首笔冷缓存和暖态；5 个 Brush 特别检查当前四项 LRU 阈值附近的重复初始化。

同一轨迹至少重复 30 次；鼠标频率 125/500/1000 Hz，60 Hz viewer，fit 与 zoom/pan 都覆盖。
分开统计 GUI event、queue wait、owner/sample/history、CPU index/replay、命令和像素上传字节、
GPU raster、mip、EDT 两个方向、feather、Union/Mix、GPU wait 与呈现。所有指标给 p50/p95/max、
工作量和硬件/软件身份，冷初始化与暖态分列。截图和 traces 保存到 `build/tmp/` 的本任务目录。

- 默认笔刷暖态 GUI 处理 p95 ≤1 ms，cursor 在下一个可用 Qt frame 更新；秒级 native 延迟
  不阻塞输入或 Cancel。
- 默认软笔连续 Paint、Erase 和 Move 的完整 Interactive owner-cycle p95 ≤16 ms，
  event-to-photo p95 ≤33.4 ms，暖态 max <100 ms；不能以跳过中间照片帧或丢弃笔触换平均值。
- 首笔在既有图像管线暖态但 Brush 资源冷态下，press-to-first-photo ≤100 ms；单独说明 GPU
  模块首加载与分配成本并在同阶段处理超标，不能要求用户先点一下完成初始化。
- 长笔划后半段不能出现随完整前缀反复复制导致的累积二次增长；source 未变的参数编辑不走
  host 像素重放/上传，合法有效结果不因无关 revision 被重复求值。按真实相交样本、像素数
  和 scratch 失效原因解释必要 GPU 重建、高重叠和大羽化成本，不虚构所有情况 O(1)。
- 除明确可重建 scratch 和单 Grade 当前 Mix 外，没有每 Mask/Stroke/Version 的长期完整
  R8 副本。披露 host/device 峰值、资源复用、bytes per update；不通过牺牲 reader 安全达标。

**构建与完成记录。** 通过 `scripts/msvc_env.cmd` 使用 `win_debug`/`win_release`，新 target
加入 CMake 后配置并构建，`ctest -N` 核对实际注册案例再运行。Windows 每次 configure/build/link
总等待预算按 AGENTS.md 从 **10–20 分钟起步**，宽目标/CUDA/应用链接优先 20 分钟，仍在编译
时继续放宽；短工具轮询不是杀进程超时。继续同一个构建 session，不能短超时反复重启。
本次规划不执行这些构建，不把既有 Debug 测试记录复制为新阶段通过证明。

**目标成功/失败调用链：**

```text
First real press -> one input owner -> stable item-to-reference mapping -> ordered sequence
  -> serial canonical sampling -> one batch publication -> GPU ordered tile raster
  -> exact parallel distance / feather -> Union -> Grade Mix
  -> accept frame by session + edit revision -> visible intermediate photograph
Valid release -> finish the same complete stroke -> one history commit -> matching Quality

Equivalent presentation switch -> keep the same stroke and mapping
Actual geometry change / grab cancel -> one Cancel -> owner restore -> current restore frame
Stale frame -> reject without replacing newer erased pixels
Failed GPU work -> invalidate pending content/base -> retain valid published result + real error
  -> next authorized operation derives exact pixels from current commands; no CPU/quality substitute
```

**一次性交付清单：**

- [ ] 在未预点击的新图像上，Paint/Erase 第一次直接拖动均覆盖完整轨迹。
- [ ] Erase 在 release、Quality、下一次编辑、Undo/Redo 与重开后不意外回现。
- [ ] 已区分并修复真实输入中断、误取消、提交丢失、缓存 base 和旧帧问题；每个实际原因有失败→通过证据。
- [ ] GPU source raster 与精确并行羽化已接入本机生产链；CPU 前缀复制和重复全域工作已清理。
- [ ] 实际照片默认软笔达到上述时延，记录冷/暖态、长笔/多 Mask、横/竖图及 Debug/Release 对照。
- [ ] 真实 workspace QML、owner、GPU、frame queue 和历史测试已注册并非零执行，原有移动框/坐标/默认羽化不回归。
- [ ] source/reader/资源 revision 生命周期安全，无新源镜像和未授权的长期 R8 多副本。
- [ ] 完成记录包含 commit、实际文件/API、两个调用链、测试精确命令/数量、像素误差、时延/资源表与平台执行范围。

本阶段任一条未满足时保持 partial，列出本阶段剩余工作并继续处理；不能再命名一个补修子阶段
把这些退出条件移走。其他原有 NM7.13–NM7.15 内容仍按原范围保留。

### NM7.13 — Complete cancellation and project/session lifecycle

**Purpose:** preserve state when operations terminate without a normal release.

**Work:** route grab cancellation, deactivation, workspace hide, adjustment input, owner deletion,
Undo/Redo, image/Version/project switch and close through ordered boundaries. Restore unfinished
commands and rebuild affected results, with no raster before-state. Fence old cache jobs by project
and storage generation in addition to image/Version/sequence identity. Keep graph actions disabled
only while creation mode owns input; release resources even when results are rejected.

**Files/APIs:** workspace input adapter, controller, session/project lifecycle, node/shortcut gates,
cache service and frame validation.

**Primary chain:** interruption → cancel/settle command → owner restore/commit → requested lifecycle
operation → stale result rejection / old writer retirement.

**Tests:** `GrabCancellationDoesNotCommitBrush`, `DuplicateReleaseCommitsOnce`,
`VersionCheckoutRejectsOldMaskFrame`, `ProjectSwitchRejectsOldCacheWrite`,
`MaskDeleteWithViewerFocusDoesNotDeleteGrade`, `WorkspaceHideRestoresUnfinishedMaskMove`.

**Exit:** delayed native completion, write jobs and genuine Qt input all obey the same outcome.

### NM7.14 — Qualify native pixels, bounded disk storage and recovery

**Purpose:** verify source, command replay and disposable cache as one product path.

**Work:** run the companion acceptance matrix plus main Section 11. Exercise 1,000 strokes,
1,000 Undo/Redo, many Versions and Clear on multiple projects. Reopen/cacheless package/Paste,
error injection, crash during atomic replacement and schema rejection. Record native backends
separately; no CPU/other-backend substitution for unavailable qualification hardware.

**Files/APIs:** shared native Mask fixtures, history/recovery/project/cache/UI tests and package tests.
**Primary chain:** actual tool → source commands → clear/save/reopen/Version/Paste → fresh native
coverage → expected pixels independent of any previous cache.

**Tests:** `CachelessProjectRestoresAllMaskVersions`, `NativeMaskReplayMatchesExpectedCoverage`,
`RepeatedHistoryNavigationKeepsRasterFileCountBounded`, `InterruptedCacheReplaceLeavesNoPartialFile`,
`PastedBrushUsesTargetProjectStoragePolicy`.

**Exit:** registered tests execute nonzero counts; actual file count/bytes and error cases recorded.

### NM7.15 — Qualify real viewer, packages and performance

**Purpose:** measure the requested Interactive editing and ensure it ships in installed builds.

**Work:** real RAW, nonidentity Grade, three sources, existing-source moves, paint/erase, Undo/Redo,
project cache root/clear/close, packaged reopen. Capture control-only QSG and actual image pixels;
measure event/queue/replay/feather/Union/Mix/presentation/cache-write costs and memory separately.
Update completion records and master status only after all required evidence exists.

**Files/APIs:** package/viewer tooling, plan and master completion records; temporary outputs under
`build/tmp/nm7/`. Preserve the source/owner naming and copy rules of AGENTS.md.

**Primary chain:** installed app → real input → Interactive photo → parameter commit → Quality →
cache clear → reopen/rebuild → same final photo.

**Exit:** NM8 receives exact platforms, commands, fixtures, latency distributions, storage bounds,
and explicitly unexecuted platform checks. No quality reduction or hidden raster-history cache.

## 11. Acceptance matrix and independent oracles

Test implementation and expected outputs must not share the same bug. Use independent analytic
calculations at chosen sample points and hand-specified R8 dab results, not production geometry
helpers to generate both sides of a comparison.

| Area | Required cases | Evidence / tolerance |
| --- | --- | --- |
| Mapping | landscape/portrait; crop/rotation/orientation; fit/actual/zoomed; pan; DPR 1/1.25/1.5/2; DetailPatch | Reference and item round-trip tolerances in NM7.5; exact identity preservation |
| Analytic coverage | Radial center/inside/feather/outside and rotated axes; Linear both ends/midpoint/direction | Independent scalar formulas; R8 error ≤ 1 code value unless existing tests are stricter |
| Brush pixels | multiple strokes on one MaskId, size/strength changes, click, sparse/dense events, overlaps, duplicates, hard/soft edge, erase, zero changes | Exact R8 bytes for deterministic raster output; different event grouping gives same bytes |
| Native runtime | CUDA/OpenCL/Metal, one/many Masks, two Grades, invert/opacity/feather, current-cache/fresh-replay | Existing NM3 numerical tolerances; record backend and runtime actually used |
| Overlay | finite vertices, winding, clipping, alpha seams, constant handle widths, next available Qt frame | Geometry assertions plus accelerated window captures; contour deviation ≤ 0.25 logical px |
| Parameter-mask UI | selected Radial range/feather lines; crop-style Gradient guides; drawer select/re-edit/delete | NM7.8 production QML input, IDs/history, before-release pixels and captures; no fill |
| Movement | existing Brush/Radial/Gradient; old/new domains; repeated translation; press/move/release | Interactive pixels update before release; controls only, no coverage fill; one final commit |
| Project cache | custom root, Clear, close cleanup, two projects, repeated Versions | Stable file count; delete-all-cache restores same history/coverage; no cross-project deletion |
| Input | press under threshold, outside release, canceled grab, synthesized mouse, second touch, repeated Enter | One source stream and exactly one terminal outcome; no duplicate commit |
| Ownership | held GPU/request reader, late callback, load/checkout during settle, new image with reused IDs | No simultaneous raster read/write or live mutation/render; no stale publication |
| History | creation, later edit, delete, cancel, no-op, Undo/Redo, cache corruption, WAL failure | Exact commit counts/HEAD, canonical document values and replayed expected coverage with no prior cache |
| UI | header/node entry, seven pages with Mask disabled outside editing, stable rows, focus, renamed/missing target, two themes/reduced motion | Production QML load; load-only emits zero commands/renders; actual reachable control path |
| Persistence | reopen, two Versions, Paste, export | Same settled source-derived coverage; target RAW metadata retained; no overlay in exported pixels |

For final RGB comparisons use existing pipeline fixture tolerances and record max/mean error plus
failure coordinates. Do not loosen tolerances merely because R8 edges make a visual mismatch
noticeable. Separate contour quantization tolerance from wrong transforms, wrong target or stale data.

No native execution is required for this documentation-only change. During execution, discover
registered targets with `ctest -N` and inspect CMake before naming exact commands in records.
Use repository macOS presets and the MSVC wrapper on Windows. New tests must have behavior-specific
names and be registered; a filter that executes zero tests is not qualification.

## 12. Performance and resource evidence

Preserve NM6's 16 ms total Interactive cycle target, including owner consumption, invalidation,
Mask work, Grade processing and safe completion. Measure separately:

- Qt event to overlay synchronization/presentation;
- enqueue to owner consume and queue depth/sample backlog;
- sample processing/raster update, dirty upload bytes and actual feather work;
- owner cycle total and Qt photo presentation latency;
- release to durable parameter commit and Quality presentation; coalesced cache publication separately;
- current CPU raster bytes, pending sample bytes, QSG vertices/nodes, native allocations/leases.

Run identical fixed sample paths on the same RAW/view/backend/build: one Mask and many Masks;
small Brush and large soft Brush; cold and warm runtime; first and subsequent parameterized edits. Report
p50/p95/max and actual operation counts. Do not compare Debug CUDA to Release Metal as a performance
conclusion. Deliberately hold a render to prove overlay input and cancellation stay responsive.

Expected bounds: no unnecessary full-raster copy/upload for local moves; full-domain changes
and required feather work are measured explicitly, no R8 history file per stroke, no
per-frame entire-stroke QML object rebuild, no repeated whole-prefix history copies, no overlapping
owner cycles, and no valid upstream Interactive-result eviction. Initial texture creation and
algorithm-required wide feather processing must be reported separately from local source uploads.
If resources cannot satisfy a complete operation, report the error and restore unfinished work;
do not drop data, lower quality or switch the backend.

## 13. Failure matrix and completion record template

| Failure | Required committed state | Required user/session behavior |
| --- | --- | --- |
| Invalid target / transform / fields | Unchanged | Precise rejection; no provisional half-Mask |
| Cancel before owner consume | Unchanged | Drop queued sequence; no restore render needed |
| Cancel after provisional application | Original state restored | Restore frame if pixels changed; no commit |
| Required R8/scratch allocation failure | Original source remains recoverable | Restore unfinished operation if needed; real error; no lower quality |
| Cache write failure | Durable parameter HEAD retained | Report I/O error, keep dirty status; no alternate directory or history rollback |
| Stale cache-job completion | New session untouched | Reject old generation; release temp files/readers through project cache owner |
| History persistence failure | Existing NM4 durable/restore rules | Never falsely report success; retain real error |
| GPU failure before/after settle | No invalid output published; valid durable commit retained | Real backend error; no alternate evaluator |
| Scene graph invalidation | Document/history unchanged | Recreate display geometry on next valid scene |

Each executor appends this record under its sub-phase; do not replace planned tests with a vague
“tested” line:

```text
NM7.x completion record
Date / commit / platform / Qt version / native backend / build type:
Status: planned | in progress | complete
Resolved decisions and actual files/APIs:
Primary success call chain:
Primary cancel/failure call chain:
Test name -> registered target -> command -> executed count -> result:
Reference fixture / expected output / tolerance:
Resource and latency measurements where applicable:
Source/header ownership and naming checks:
Remaining gaps and next dependency:
```

Global NM7 completion checklist:

- [ ] Section 2 approved decisions are reflected in the master/UI specification and implementation.
- [ ] All three source types work in the real viewer on the selected exact Color Grade.
- [ ] QSG input/geometry, actual evaluator and ReferenceSpace agree.
- [ ] Brush canonical input, parameterized strokes, one current Grade R8 slot, regional replay and reader ownership are proven.
- [ ] Creation, each edit/stroke, cancellation and deletion have the specified history outcome.
- [ ] Stale inputs, cache jobs and frames cannot cross image/Version/sequence boundaries.
- [ ] UI entry points, accessibility, disabled-until-editing Mask page and load-only selection restore are tested.
- [ ] Native runtime, cacheless persistence/recovery and exported pixels pass the acceptance matrix.
- [ ] One thousand strokes/Undo/Redo do not create per-step R8 files or pixel history.
- [ ] Per-project paths, retain/close-cleanup/Clear and stale-writer rejection are qualified.
- [ ] Resource/performance records include real platform results and no quality substitutions.
- [ ] New headers include defining headers unless a documented include-cycle/PIMPL exception applies.
- [ ] Added copies have the concrete owner/lifetime/consistency purpose specified in Section 5.1.
- [ ] Touched first-party names and roadmap text/links satisfy repository terminology rules.
- [ ] Master NM7 status and NM8 handoff accurately distinguish completion from remaining evidence.
