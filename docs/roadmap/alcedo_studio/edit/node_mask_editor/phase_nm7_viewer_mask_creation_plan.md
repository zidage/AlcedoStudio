# Phase NM7 — Viewer Mask Creation and Editing

Date: 2026-09-08

Status: NM7.1–NM7.7 complete; NM7.8–NM7.14 planned. This document records the NM7.1 source
audit, NM7.2 parameterized Brush owner operations, NM7.3 typed stroke history plus the
project/schema cutover, NM7.4 canonical rasterization with regional Mix replay, NM7.5
shared ReferenceSpace mapping with Brush placement, NM7.6 control-only retained QSG,
and NM7.7 Radial/Linear creation plus existing-mask movement. Remaining sub-phases are
unimplemented.

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
Read it before implementing NM7.2–NM7.14. It gives equations, inverse operations, replay bounds,
cache lifecycle, project settings, failure behavior and an initial executable algorithm experiment.

## 1. Purpose and background for the executor

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
| Brush operations | Paint/erase with size and strength controls | NM7.2, NM7.4, NM7.8 |
| Persistent source | Parameterized ordered strokes; history stores reversible commands, not R8 revisions | NM7.2–NM7.3 |
| Accumulation/cache | Same Grade's strokes accumulate in one Brush; one current final Grade Mix R8 cache slot | NM7.4, NM7.9–NM7.10 |
| Radial initial drag | Center outward | NM7.7 |
| Existing Mask movement | Brush/Radial/Gradient update real Interactive pixels during drag, Quality after release | NM7.5, NM7.7–NM7.9 |
| QSG existing-mask display | Controls only, no affected-area fill or completed Brush path | NM7.6 |
| Initial creation guides | Working interpretation: temporary cursor/outline/path guides are allowed only during initial drawing, never area-fill highlighting; clarification requested | NM7.6 |
| Editing body | Temporary Masks body; preserve six adjustment tabs | NM7.11 |
| Project storage | User-selected cache root, per-project Keep/DeleteOnProjectClose and Clear actions | NM7.10–NM7.11 |

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
5. Support pointer release, Done/Enter, Escape/Cancel, keyboard editing, interruption, and stale
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
capability-filtered navbar and vertical EXIF layout: six tabs remain, EXIF occupies one row, and
Mask tool actions sit beside the node name. NM7 must preserve those landed choices.

The temporary Masks body approved in Section 2 makes the older master Masks panel reachable
without adding a seventh tab. Store the previous ordinary panel key as session presentation state,
restore it on Done/Cancel, and never submit edits just because a body opens or closes. This routing
choice was approved on 2026-09-08 and is reflected in master Section 18.1.

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
  controls may show a handle/connector, not a highlighted feather band.
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

Done after release only exits mode. Cancel rolls back unfinished commands and replays affected
regions; earlier completed strokes remain. No-op creation or unchanged placement creates no
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
mapping. Use these boundaries to locate radius/feather controls. Initial creation may display outline
guides; editing an existing Mask shows the necessary handles/connectors without area highlighting
or filled feather bands.
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
During initial creation, these lines may form guides; for an existing Mask use them to position
finite direction/width controls without filling or highlighting the affected area. Map control points
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
| Release / Enter / Done with a valid open operation | Seal once, enter Settling, publish one commit and request Quality |
| Escape / Cancel | Cancel unfinished input, restore before-state/remove provisional addition, zero commits; exit mode |
| Enter / Done after a released operation | Leave mode without a duplicate commit |
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

Implement Section 2's approved routing, preserving the landed header and six-tab product decision.
The approved temporary body reuses `EditorMasksContextPanel.qml` as the actual Masks editor and
loads through the adjustment shell; it is not a hidden unused QML module or a separate window.

The list exposes type icon, Mask name, selection, and supported commands. Selected-row controls
edit name, enabled, invert, opacity and source-specific values plus an explicit Brush Move action. Use exact IDs, stable row updates,
and a panel-level `selectedMaskId` binding. No list rebuild or forced scroll containment on click.
After deletion select the next row at the old position, otherwise previous, otherwise no selection.
Undo restores IDs; select a restored mask only according to an explicit current-session selection
rule, never by a reused index. Selection-only restore stays load-only.

The compact viewer creation bar contains separate source-kind and name text plus Done/Cancel.
Use existing shared actions and `cardSurfaceColor`, `cardBorderColor`, `panelRadius`, spacing,
font and icon tokens. Place it in a reserved viewer edge area; if it would cover an active primary
handle, relocate the bar within that area without moving the photo or changing coordinates.

Use existing approved Brush/Radial/Gradient SVGs, including their documented 2 px exception.
Define required Mask colors/handle geometry/hit-area/antialias widths as AppTheme semantic tokens
and add their actual values to DESIGN.md in the implementation change. Suggested roles are the
master's `maskOverlayControlColor`, `maskOverlayControlOutlineColor`, `maskOverlayInactiveColor`. Do not add a coverage-area color for the revised Mask UI. No ad-hoc palette, Material controls, badges, pills or status dots.

QSG nodes are not accessible controls. Expose lightweight accessible proxies for the selected
source's finite set of handles, or equivalent named numeric controls plus handle selection. Do
not create a proxy for every dab. Tab reaches the list, selected controls and Done/Cancel; arrows
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
下面的 NM7.1–NM7.14 **替代上一版十二阶段拆分**；旧阶段没有本轮 production completion，
不能把旧的 immutable asset 测试当作新格式验收。保持总体 NM1→NM6→NM7→NM8 顺序。

| Phase | Result | Dependency |
| --- | --- | --- |
| NM7.1 | Source audit, numerical rules and new format boundary | NM6 production gates |
| NM7.2 | Parameterized Brush source and focused owner operations | NM7.1 |
| NM7.3 | Typed reversible stroke/placement history, WAL and Version | NM7.2 |
| NM7.4 | Canonical rasterization, spatial index and regional replay | NM7.2–NM7.3 |
| NM7.5 | Shared ReferenceSpace mapping, Brush placement and hit testing | NM7.4 |
| NM7.6 | QSG control-only retained rendering | NM7.5 |
| NM7.7 | Radial and Linear creation plus existing-mask movement | NM7.3, NM7.5–NM7.6 |
| NM7.8 | Accumulating Brush creation, erase, tool settings and movement | NM7.4–NM7.6 |
| NM7.9 | Serial Interactive replay and one current Grade R8 result | NM7.7–NM7.8 |
| NM7.10 | Project-owned cache settings, writeback and cleanup service | NM7.3, NM7.9 |
| NM7.11 | Production Mask controls and project storage UI | NM7.10 |
| NM7.12 | Interruptions, late jobs and complete lifecycle | NM7.11 |
| NM7.13 | Native pixel, persistence, cache bounds and recovery qualification | NM7.12 |
| NM7.14 | Real viewer/package/performance qualification and NM8 handoff | NM7.13 |

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

**Residual gaps:** native Mask evaluation still loads `MaskStore` when in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned. NM7.4 regional replay is not started. Project Mix cache service is NM7.10. `ReplaceMaskAsset` remains in the typed batch surface but cannot validate parameterized or raster-only Brush JSON through the current source gate.

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

**Residual gaps:** host Mix is not yet the Interactive/Quality native Mask pass (NM7.9) or the project Mix-cache slot (NM7.10). Shared ReferenceSpace pointer mapping is NM7.5. Native evaluation still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

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

**Residual gaps:** QSG controls are NM7.6. Radial/Linear creation and existing-mask movement UI are NM7.7. Accumulating Brush paint/erase UI is NM7.8. Session does not yet publish live `ResolvedRenderGeometry` into `setDisplayedMaskGeometry` (NM7.9). Open-operation cancel on `MappingChanged` is NM7.12. Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

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

**Residual gaps:** NM7.7 Radial/Linear creation and existing-mask movement UI (controller + Interactive pixels). NM7.8 accumulating Brush paint/erase UI. Session does not yet publish live Mask overlay display (still NM7.9). Accessible handle proxies are NM7.11. Offscreen QPA grabs are not a packaged D3D11/Metal desktop capture (NM7.14). Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

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

**Residual gaps:** NM7.8 accumulating Brush paint/erase UI. Session still does not publish live Mask overlay display or enqueue Interactive GPU frames (NM7.9). Project Mix-cache slot is NM7.10. Header/Masks-body wiring and accessible handles are NM7.11. Open-operation cancel on `MappingChanged` is NM7.12. Native Mask still loads `MaskStore` when an in-memory `asset_key` is present. Ordinary adjustment Mask writes still fail with the existing “until NM3” text. NM6.8–NM6.9 remain planned.

### NM7.8 — Complete accumulating Brush creation, erase and movement

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

### NM7.9 — Integrate serial Interactive evaluation and one current Grade R8

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

### NM7.10 — Add project-owned cache storage and maintenance

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

### NM7.11 — Wire Mask editing and project cache UI

**Purpose:** make the fully working capability reachable through approved surfaces.

**Work:** connect header tools/node Mask rows to temporary Masks body; preserve six tabs and prior
panel restoration; expose paint/erase/size/strength/Move and existing source/Mask values. Add project
cache section through app APIs with root chooser, policy and per-project usage/Clear. Existing
cache settings are thumbnail-specific; keep the new project fields separate. Add keyboard/accessible
controls, focus rules, theme/width/reduced-motion tests and QML registration.

**Files/APIs:** header/stack/Masks body/node drawer/creation bar, `SettingDialog.qml`,
`CacheSettingsPanel.qml`, proposed `ProjectMaskCacheSettingsPanel.qml`, project adapter, AppTheme/DESIGN.

**Primary chain:** UI action → exact project/Mask owner → validated operation → completion projection.
**Tests:** `MaskModePreservesSixTabs`, `MaskSelectionDoesNotSubmitOrJumpScroll`,
`KeyboardMaskMoveUsesSameInteractiveRoute`, `CacheSettingsTargetSelectedProjectOnly`,
`ProjectClearExplainsThatBrushHistoryIsRetained`.

**Exit:** production QML at 260/320/460 px in both themes; no unregistered/unused-only implementation.

### NM7.12 — Complete cancellation and project/session lifecycle

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

### NM7.13 — Qualify native pixels, bounded disk storage and recovery

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

### NM7.14 — Qualify real viewer, packages and performance

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
| Movement | existing Brush/Radial/Gradient; old/new domains; repeated translation; press/move/release | Interactive pixels update before release; controls only, no coverage fill; one final commit |
| Project cache | custom root, Clear, close cleanup, two projects, repeated Versions | Stable file count; delete-all-cache restores same history/coverage; no cross-project deletion |
| Input | press under threshold, outside release, canceled grab, synthesized mouse, second touch, repeated Done | One source stream and exactly one terminal outcome; no duplicate commit |
| Ownership | held GPU/request reader, late callback, load/checkout during settle, new image with reused IDs | No simultaneous raster read/write or live mutation/render; no stale publication |
| History | creation, later edit, delete, cancel, no-op, Undo/Redo, cache corruption, WAL failure | Exact commit counts/HEAD, canonical document values and replayed expected coverage with no prior cache |
| UI | header/node entry, six tabs, stable rows, focus, renamed/missing target, two themes/reduced motion | Production QML load; load-only emits zero commands/renders; actual reachable control path |
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
- [ ] UI entry points, accessibility, six-tab behavior and load-only selection restore are tested.
- [ ] Native runtime, cacheless persistence/recovery and exported pixels pass the acceptance matrix.
- [ ] One thousand strokes/Undo/Redo do not create per-step R8 files or pixel history.
- [ ] Per-project paths, retain/close-cleanup/Clear and stale-writer rejection are qualified.
- [ ] Resource/performance records include real platform results and no quality substitutions.
- [ ] New headers include defining headers unless a documented include-cycle/PIMPL exception applies.
- [ ] Added copies have the concrete owner/lifetime/consistency purpose specified in Section 5.1.
- [ ] Touched first-party names and roadmap text/links satisfy repository terminology rules.
- [ ] Master NM7 status and NM8 handoff accurately distinguish completion from remaining evidence.
