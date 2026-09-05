# Phase NM5 — QuickQanava Nodes Panel

Date: 2026-09-02

Status: NM5.1–NM5.7R implementation recorded; NM5.8a–NM5.8d complete 2026-09-04; NM5.8e–NM5.8g pending

Prerequisites: NM4 is complete. NM1.4R and NM1.5 behavior remains required.

Parent plan: [Node-aware Pipeline Editing and Mask Authoring](../node_mask_editor_master_plan.md),
Sections 15–18 and 21.6.

---

## 1. Objective

NM5 adds the production Nodes page to the editor tool rail. The page uses the pinned
QuickQanava module. It shows the image backbone of the current `PipelineDocument`.

NM5 lets the user:

- open and close the Nodes page;
- select one pipeline node;
- add a clean Color Grade;
- rename a Color Grade;
- delete a Color Grade;
- move a Color Grade to another valid backbone position;
- move and zoom the graph view;
- move nodes for local layout;
- open and close each Color Grade Mask drawer.

The node card shows the node name and the Mask stack. It does not show adjustment names.
Each Mask row shows only the Mask source type.

All pipeline changes use the NM4 typed-history path. All graph validation stays in Alcedo.
QuickQanava displays the projection and reports user input. It never owns product data.

### 1.1 Scope exclusions

NM5 does not:

- add node Enable or Disable actions;
- show On, Off, Active, or Inactive state in a node;
- show topology numbers;
- show adjustment summaries;
- show Mask counts;
- edit, add, remove, rename, or reorder Masks;
- add Mask authoring to the viewer;
- change right-side adjustment ownership;
- add arbitrary branches, groups, or compositors;
- use QuickQanava groups;
- implement a second graph canvas;
- replace QuickQanava navigation, selection, ports, edges, or visual connectors;
- add a minimap.

The domain can retain existing enabled fields for stored data and execution. NM5 does not expose
those fields in the Nodes UI.

### 1.2 Product boundary after NM5

NM5 provides the complete Nodes page and its real edit paths. NM6 consumes the selected NodeId
for right-side adjustment routing. NM7 adds Mask selection and Mask authoring. NM8 performs the
final cross-platform qualification.

---

## 2. Required repository context

Read the current source before each sub-phase. A file name is not evidence that an interface is
still unchanged.

| Source | Required information |
| --- | --- |
| [Master plan](../node_mask_editor_master_plan.md) | Read Sections 3, 5, 6, 11–18, 20, 21, and 23–26. They define identity, writes, history, UI, tests, and failure behavior. |
| [NM0 plan](phase_nm0_quickqanava_integration_plan.md) | Read the pinned revision, static module setup, licence work, and incomplete production checks. |
| [NM1 plan](phase_nm1_pipeline_document_editing_plan.md) | Read the single live document rule, render lock, failure restoration, and render reasons. |
| [NM2 plan](phase_nm2_multi_grade_runtime_plan.md) | Read backbone order, NodeId identity, and multi-Grade execution. |
| [NM3 plan](phase_nm3_multi_mask_runtime_plan.md) | Read the ordered display list of Masks and `MaskSourceKind`. |
| [NM4 plan](phase_nm4_history_version_paste_plan.md) | Read typed graph changes, Undo, Redo, checkout, recovery, and WAL failure behavior. |
| [QML VI](../../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md) | Read Basic style, AppTheme, copy bans, icon rules, panel geometry, focus, fold motion, and Loader lifetime. |
| [EditorWorkspace.qml](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspace.qml) | Read the current editor columns and the 360 logical-pixel viewer floor. |
| [EditorWorkspaceRail.qml](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspaceRail.qml) | Read the current History and Versions pages, fold progress, Loader, and rail actions. |
| [EditorSessionController](../../../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp) | Read session identity, page state, action availability, and generation signals. |
| [EditorSessionHistoryPort](../../../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp) | Read the NM4 Add, Remove, Reconnect, and Rename entry points. Do not expose `SetColorGradeEnabled` in NM5. |
| [PipelineDocument](../../../../../alcedo_studio/src/include/edit/graph/pipeline_document.hpp) | Read serialization, graph ownership, and the default document factory. |
| [ColorGradeNodeModel](../../../../../alcedo_studio/src/include/edit/graph/color_grade_node_model.hpp) | Read display name, Masks, and input/output ports. |
| [MaskModel](../../../../../alcedo_studio/src/include/edit/mask/mask_model.hpp) | Read `Brush`, `Radial`, and `LinearGradient` source kinds. |

### 2.1 Fixed product invariants

- `PipelineDocument` is the only writable product graph.
- QuickQanava objects are projection objects.
- A `qan::Node*` is never a NodeId.
- A Qan object does not survive a projection generation change.
- QML calls application-layer controllers only.
- QML never modifies the product graph directly.
- A graph command uses the NM4 typed batch, WAL, history head, and render path.
- A UI-only change does not create a history commit.
- A UI-only change does not start a photo render.
- A failed graph command leaves the document, history, projection, and permanent Qan graph unchanged.
- A stale session or generation cannot change the current image.
- The product graph has one Develop-to-DRT scene-image backbone.
- Develop and DRT/Post cannot be removed or renamed.
- The Nodes UI has one selected node.

---

## 3. QuickQanava official-source rule

QuickQanava is a specialized third-party library. Do not infer its interface from a generic node
editor or another graph library.

The pinned checkout is tag `2.50`, commit
`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`.

The website can describe a newer revision. The pinned checkout defines the buildable interface.
If the website and checkout differ, record the difference in the sub-phase completion record.
Do not move to another upstream revision during NM5.

### 3.1 Official documentation index

| Official page | Required sections | NM5 use |
| --- | --- | --- |
| [QuickQanava](https://cneben.github.io/QuickQanava/index.html) | `Introduction` | Confirms the directed-graph, QML-delegate, drag, resize, and visual-topology scope. |
| [Installation](https://cneben.github.io/QuickQanava/installation.html) | `QuickQanava Quick Start`, `Using from external projects with CMake`, `Dependencies` | Defines the static-library and CMake integration path. |
| [Graph](https://cneben.github.io/QuickQanava/graph.html) | `Data Model`, `QuickQanava Initialization`, `Graph View`, `Grid` | Defines topology/view separation, initialization, navigation, and official grid support. |
| [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html) | `Adding content`, `Docks and Ports`, `Node Resizing`, `Observing Topology`, `Selection`, `Defining Custom Nodes` | Defines nodes, visual items, port binding, size changes, selection, and custom delegates. |
| [Edges](https://cneben.github.io/QuickQanava/edges.html) | `Creating Edges`, `Visual creation of edges`, `Visual Connectors`, `Custom Connectors` | Defines edge insertion and the request-only connector mode. |
| [Styles](https://cneben.github.io/QuickQanava/styles.html) | `Defining Styles`, `Node Style`, `Material Styling` | Defines style properties. Alcedo does not copy the Material appearance. |
| [Utilities](https://cneben.github.io/QuickQanava/utilities.html) | `Navigable` | Points to the navigation interface. Verify exact methods in the pinned header. |
| [Advanced use (C++)](https://cneben.github.io/QuickQanava/advanced.html) | `Using from C++`, `Defining Custom Topology`, `Insertion of non Visual Content`, `Observation of Topological Modifications` | Defines C++ access, factories, and topology observation. |
| [Samples](https://cneben.github.io/QuickQanava/samples.html) | `Custom Nodes: 'custom'`, `Navigable Area: 'navigable'`, `Topology Sample: 'topology'` | Confirms official usage patterns. Sample appearance is not Alcedo VI. |
| [API Reference](https://cneben.github.io/QuickQanava/reference.html) | `API Reference` | States that the detailed online reference is unavailable. Generate local Doxygen or read pinned headers. |
| [Licence](https://cneben.github.io/QuickQanava/licence.html) | `Licence` | Defines source and binary redistribution notices. |

### 3.2 Required pinned headers

Check these files before using an exact C++ or QML property name:

- `alcedo_studio/src/third_party/QuickQanava/src/qanGraph.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanGraph.hpp`
- `alcedo_studio/src/third_party/QuickQanava/src/qanGraphView.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanNavigable.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanNode.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanNodeItem.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanPortItem.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanEdge.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanEdgeItem.h`
- `alcedo_studio/src/third_party/QuickQanava/src/qanStyle.h`

Generate local Doxygen from the pinned checkout when the headers do not give enough context.
Save generated output under `build/tmp/nm5_quickqanava_docs/`. Do not add it to the repository.

### 3.3 Confirmed framework boundaries

The official documentation confirms these boundaries:

- `Qan.GraphView.graph` binds a graph to a view.
- `qan::Graph` or `Qan.Graph` modifies QuickQanava topology.
- A topology primitive has a visual item through `item` or `getItem()`.
- `Qan.GraphView` supplies mouse navigation when navigation is enabled.
- `Qan.LineGrid` is the supported documented grid. The website marks PointGrid as deprecated. The
  pinned 2.50 source has removed PointGrid. NM5 uses only `Qan.LineGrid`.
- `insertPort()` creates ports.
- `bindEdgeSource()` and `bindEdgeDestination()` bind an edge to ports.
- custom QML delegates define node content;
- rectangular node bounds update after width or height changes;
- selection uses graph policy and selected-node state;
- `connectorCreateDefaultEdge = false` prevents the default connector from inserting an edge;
- `connectorRequestEdgeCreation(src, dst)` reports a request when default insertion is disabled.

The official documentation does not make `PipelineDocument` a QuickQanava object. It does not
define Alcedo history, Version behavior, naming, Mask semantics, or render scheduling.

---

## 4. Current source baseline

### 4.1 Existing capability

- NM0 adds the QuickQanava submodule and builds its static module.
- NM0 records the BSD-3-Clause licence and the bezier dependency notice.
- NM0 does not link QuickQanava into the production `alcedo_main` path.
- NM4 provides typed Add, Remove, Reconnect, Rename, and replay behavior.
- `PipelineDocument` serializes the graph and all Color Grade Masks.
- `ColorGradeNodeModel::Masks()` keeps Mask display order.
- `GetMaskSourceKind()` returns `Brush`, `Radial`, or `LinearGradient`.
- `EditorWorkspaceRail.qml` supports History and Versions.
- The rail destroys a loaded page after the close fold reaches zero.
- The rail stores list offsets outside page Loaders.
- `DESIGN.md` defines Basic style, AppTheme use, icon geometry, panel size, and fold motion.

### 4.2 Missing capability

- The production executable does not initialize QuickQanava.
- The production QML module does not contain a Nodes page.
- No application projection maps `PipelineDocument` into Qan primitives.
- No controller owns selected NodeId for the Nodes page.
- No UI store owns graph position, view position, zoom, or drawer state.
- No default Color Grade naming counter exists in `PipelineDocument`.
- The default node name is currently `Color Grade`, without a number.
- No approved Alcedo node delegate exists.
- No Mask type icon resources exist for the node drawer.
- The current rail page property is named for History only.

### 4.3 Existing APIs that NM5 must not expose

The current domain and history layers include `SetColorGradeEnabled`. NM5 does not remove this
stored behavior. NM5 also does not expose it in QML, the node menu, keyboard input, node content,
screen-reader text, or tests of visible actions.

---

## 5. Approved Nodes UI

### 5.1 Workspace position

Nodes is the third expandable page in `EditorWorkspaceRail`. History, Versions, and Nodes use one
page key and one Loader. Only one page can be open.

```text
Top toolbar

┌────────┬─────────────────────────┬───────────────────┐
│ Rail   │ Viewer                  │ Adjustment stack  │
│        │                         │                   │
│ Hist   │                         │ Selected node     │
│ Vers   │                         │ context in NM6    │
│ Nodes  │                         │                   │
│ Tasks  │                         │                   │
├────────┴─────────────────────────┴───────────────────┤
│ Filmstrip under the viewer column                   │
└─────────────────────────────────────────────────────┘
```

Nodes replaces only the left expandable body. It does not cover the viewer. It does not create a
floating window.

### 5.2 Rail icon

Use the user-approved Tabler `stack-2` path for the Nodes toggle:

```svg
<path d="M12 4l-8 4l8 4l8 -4l-8 -4" />
<path d="M4 12l8 4l8 -4" />
<path d="M4 16l8 4l8 -4" />
```

Store the normalized asset as `alcedo_studio/src/config/panel_icons/nodes.svg`.

The asset must use:

- `viewBox="0 0 24 24"`;
- `fill="none"`;
- `stroke="white"`;
- `stroke-width="2"`;
- round line caps and joins.

Add the asset to `alcedo_studio/src/config/resource.qrc`. Use `IconActionButton` and AppTheme tint.
Use `Nodes` for the tooltip and accessible name. Do not add a count or status marker to the icon.

### 5.3 Page structure

The page has two visible layers:

1. A compact header.
2. The graph canvas.

The header shows `Nodes` and one Add action. It does not show a node count, Version summary, Fit
label, status text, pill, badge, or status dot. The Add action uses `IconActionButton`.

Fit remains available through `Ctrl+0` and the canvas context menu. This keeps the header simple.

The canvas uses a deep AppTheme surface and a quiet official `Qan.LineGrid`. It has no nested card,
gradient, shadow, glow, glass effect, or minimap.

### 5.4 Graph direction

The default graph is vertical:

```text
Develop
   ↓
Color Grade 1
   ↓
Color Grade 2
   ↓
DRT/Post
```

Develop is at the top. DRT/Post is at the bottom. Color Grades follow execution order.

The first view of a Version uses deterministic positions. Do not depend on an undocumented
upstream layout algorithm. QuickQanava supplies display and navigation, not product ordering.

Users can move nodes. Users can move and zoom the view. These changes are local UI state.

### 5.5 Default Color Grade names

Do not show a topology number. A node name can contain a creation number.

The default names are:

```text
Color Grade 1
Color Grade 2
Color Grade 3
```

`Color Grade 1` is the primary Grade in a new default document. The document starts its next-name
counter at `2`.

The counter has these rules:

- a successful Add operation consumes one value;
- a failed Add operation does not consume a value;
- Rename does not change the counter;
- Remove does not decrease the counter;
- Reconnect does not change the counter;
- node movement does not change the counter;
- existing node names do not change when topology order changes;
- each serialized document stores its next counter value;
- Undo, Redo, recovery, reopen, and Version checkout restore deterministic document state.

The counter is product metadata. It is not Qan state and not local layout state.

Update `PipelineDocument` serialization with an explicit field such as
`next_color_grade_name_number`. Bump the document format when required by the current NM4 format
rules. Do not migrate an unsupported older project format.

The Add typed change must preserve enough before and after data for exact replay. Reinserting a
stored node during Undo or Redo must not claim another default name.

### 5.6 Ordinary Color Grade node

An ordinary node contains:

1. The node display name.
2. A Mask drawer header.
3. The Mask rows when the drawer is open.

It does not contain:

- a topology number;
- node-kind text;
- a status dot;
- On or Off text;
- an Enable or Disable action;
- an adjustment name or adjustment summary;
- a Mask count;
- a rename, delete, or other persistent action row;
- a pill, badge, chip, tag, or lozenge;
- an `xx · xx` compound label.

Rename and Delete stay available through the shared context menu and keyboard input. The card does
not reserve space for these actions.

### 5.7 Mask drawer

Each Color Grade has one Mask drawer below the name row. The drawer is open by default. The user can
close or open it from its `Masks` header row.

Open state:

```text
             ■
   ┌────────────────────┐
   │ Color Grade 4      │
   ├────────────────────┤
   │ Masks            ▾ │
   ├────────────────────┤
   │ [icon] Gradient    │
   │ [icon] Radial      │
   │ [icon] Brush       │
   └────────────────────┘
             ■
```

Closed state:

```text
             ■
   ┌────────────────────┐
   │ Color Grade 4      │
   ├────────────────────┤
   │ Masks            ▸ │
   └────────────────────┘
             ■
```

The drawer follows these rules:

- the `Masks` header stays visible in both states;
- the header does not show a count;
- the header uses a disclosure chevron, not a state dot;
- the full header row is operable;
- the open or closed state is local UI layout state;
- a drawer change does not modify `PipelineDocument`;
- a drawer change does not create history;
- a drawer change does not start photo rendering;
- the body clips during fold motion;
- `motionFoldOpenMs`, `motionFoldCloseMs`, and `motionEasing` control the fold;
- `reduceMotion` sets the duration to zero;
- the output port follows the bottom of the current delegate height;
- bound edges update after the delegate height changes.

Store drawer state outside the Loader. Key it by project, image, Version, and NodeId. A new key
starts open. Undo can restore a removed NodeId and recover its prior drawer state.

An open empty drawer has no Mask rows.

### 5.8 Mask rows

Mask rows are read-only in NM5. Their order equals `ColorGradeNodeModel::Masks()` display order.

Each row shows only:

- the approved source-type icon;
- the localized type name.

The mapping is:

| Model kind | UI label | Asset |
| --- | --- | --- |
| `MaskSourceKind::LinearGradient` | `Gradient` | `qrc:/mask_icons/gradient.svg` |
| `MaskSourceKind::Radial` | `Radial` | `qrc:/mask_icons/radial.svg` |
| `MaskSourceKind::Brush` | `Brush` | `qrc:/mask_icons/brush.svg` |

Do not show the Mask display name, opacity, enabled value, invert value, ranges, MaskId, timestamp,
selection, or actions. Do not add a hover action or row menu in NM5.

Keep MaskId in the application projection. NM7 needs stable identity. Do not expose the ID as UI
text.

### 5.9 Approved Mask SVG paths

Gradient uses the user-approved Tabler `wash-dry` path:

```svg
<path d="M3 6a3 3 0 0 1 3 -3h12a3 3 0 0 1 3 3v12a3 3 0 0 1 -3 3h-12a3 3 0 0 1 -3 -3v-12" />
```

Radial uses the user-approved Tabler `wash-dryclean` path:

```svg
<path d="M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0" />
```

Brush uses the user-approved Tabler `brush` paths:

```svg
<path d="M3 21v-4a4 4 0 1 1 4 4h-4" />
<path d="M21 3a16 16 0 0 0 -12.8 10.2" />
<path d="M21 3a16 16 0 0 1 -10.2 12.8" />
<path d="M10.6 9a9 9 0 0 1 4.4 4.4" />
```

Store these assets under `alcedo_studio/src/config/mask_icons/`. Add them to `resource.qrc`.

All three assets use the same 24×24 viewBox, white source stroke, and user-approved 2 px stroke as
the Nodes icon. Use AppTheme tint at runtime. Do not change one icon's optical size to compensate
for its path complexity.

The Radial circle is an approved type icon. It is not a status dot. Do not reuse it as one.

### 5.10 Endpoints

Develop and DRT/Post use compact endpoint delegates. They show their fixed names and real ports.
They do not show the Mask drawer.

Do not add a `Locked` badge or a status dot. Use action availability, tooltip text, and accessible
text to explain why Rename or Delete is unavailable.

### 5.11 Selection

The product allows one selected node. Selection does not change node size.

Use the shared surface and a high-contrast outline. Do not add a selected-state label, pill, badge,
status dot, glow, or large blue fill. The selection outline contains the full open drawer.

QuickQanava selection is a view of `EditorNodeController.selectedNodeId`. The controller owns the
product selection. A Qan selected-node list cannot become a second selection source.

### 5.12 Ports and edges

Each Color Grade has one top input port and one bottom output port. Develop has no scene-image input.
DRT/Post has no scene-image output.

Use a small square as the visible port. Give it a larger hit area. A port is a connection control,
not a status dot.

Use a thin edge with an AppTheme role. Do not use flow animation, glow, or decorative arrows.

The permanent edge set always comes from the accepted Alcedo projection. A visual connector is only
a request preview until the backend accepts it.

### 5.13 Copy and decoration bans

The Nodes page must follow the global VI rules:

- no `xx · xx` compound labels;
- no equivalent compound label with a bullet, slash, vertical bar, or dash;
- no new pill, badge, chip, tag, or lozenge without an explicit user request;
- no new status dot without an explicit user request;
- no shadow, glow, gradient, or glass effect;
- no Material import or Material control appearance;
- no raw visual literals when an AppTheme role is required.

---

## 6. Ownership and interfaces

### 6.1 Projection

Add an immutable application projection. Use value types across the UI boundary.

The projection needs at least:

```text
EditorNodeGraphSnapshot
  session_generation
  projection_revision
  topology_revision
  nodes[]
  edges[]

EditorNodeProjection
  node_id
  node_kind
  display_name
  masks[]

EditorNodeMaskProjection
  mask_id
  source_kind

EditorNodeEdgeProjection
  source_node_id
  source_port_id
  destination_node_id
  destination_port_id
```

Do not project adjustments, enabled values, mix, Mask opacity, or Mask display names into node-card
roles. Those values do not appear in the NM5 node delegate.

Projection update rules:

- a parameter edit does not rebuild the graph;
- a Mask source-kind change updates the owning Grade drawer;
- a Mask Add, Remove, or display-order change updates the owning Grade drawer;
- a Rename updates one node label;
- a topology revision adds, removes, or reconnects Qan primitives;
- a Version checkout replaces the snapshot;
- a generation change rejects older snapshots and Qan callbacks.

### 6.2 Controller

`EditorNodeController` owns:

- the current session handle and generation;
- `selectedNodeId`;
- graph command availability;
- Add, Rename, Delete, and Reconnect requests;
- projection revision publication;
- exact localized failure text;
- selection restoration after graph changes.

It does not own Qan visuals or layout coordinates.

Suggested QML-facing actions:

```text
addColorGrade()
renameColorGrade(nodeId, displayName)
removeColorGrade(nodeId)
requestReconnect(nodeId, predecessorId, successorId, requestGeneration)
selectNode(nodeId)
```

Do not add `setNodeEnabled()` or an equivalent QML action.

### 6.3 Qan adapter

`AlcedoQanGraph` maps projection values to Qan primitives. Keep maps in both directions:

```text
NodeId -> QPointer<qan::Node>
qan::Node* -> NodeId plus projection generation
edge identity -> QPointer<qan::Edge>
```

Clear all reverse maps before a full projection replacement. Reject a callback when its primitive
or generation is stale.

The adapter can insert and remove Qan primitives after an accepted projection update. It cannot
call Alcedo domain mutation functions directly.

### 6.4 Layout store

`EditorNodeLayoutStore` owns only local UI values:

```text
LayoutKey
  project identity
  image identity
  Version identity

LayoutValue
  preferred panel width
  view position
  zoom
  selected NodeId
  node positions by NodeId
  Mask drawer open state by NodeId
```

The store does not keep `QObject*`, `qan::Node*`, or QML item pointers. It does not write
`PipelineDocument` or history.

### 6.5 Default-name counter

Add the next-name counter to `PipelineDocument`. The graph command consumes the counter only after
all Add validation succeeds.

The typed Add payload must make forward and inverse replay exact. Use explicit before and after
counter values if the current whole-node payload does not make counter replay exact.

The default document factory sets:

```text
primary Grade display name = Color Grade 1
next Color Grade name number = 2
```

Remove and Rename do not modify the counter. Reinsertion from stored JSON does not allocate a name.

---

## 7. Sub-phase map

| Sub-phase | Result | Required official sections |
| --- | --- | --- |
| NM5.1 | Production link, initialization, and read-only proof | Installation: `QuickQanava Quick Start`, `Using from external projects with CMake`; Graph: `QuickQanava Initialization`, `Graph View` |
| NM5.2 | Default-name counter and immutable projection | Graph: `Data Model`; Nodes: `Adding content`, `Observing Topology`; Advanced: `Using from C++`, `Defining Custom Topology` |
| NM5.3 | Qan adapter, ports, and read-only topology | Nodes: `Adding content`, `Docks and Ports`, `Observing Topology`; Edges: `Creating Edges`; Advanced: `Defining Custom Topology` |
| NM5.4 | Alcedo node delegate and Mask drawer | Nodes: `Docks and Ports`, `Node Resizing`, `Selection`, `Defining Custom Nodes`; Styles: `Defining Styles`, `Node Style`; Samples: `Custom Nodes: 'custom'` |
| NM5.5 | Rail, navigation, selection, and layout state | Graph: `Graph View`, `Grid`; Nodes: `Node Resizing`, `Selection`; Utilities: `Navigable`; Samples: `Navigable Area: 'navigable'`, `Topology Sample: 'topology'` |
| NM5.6 | Add, Rename, and Delete | Graph: `Data Model`; Nodes: `Adding content`, `Observing Topology`, `Selection`; Advanced: `Observation of Topological Modifications` |
| NM5.7 | Request-only visual connector and Reconnect | Nodes: `Docks and Ports`; Edges: `Visual creation of edges`, `Visual Connectors`, `Custom Connectors` |
| NM5.7R | Incremental draft topology and atomic automatic submission | Nodes: `Docks and Ports`, `Observing Topology`; Edges: `Visual creation of edges`, `Visual Connectors`, `Custom Connectors`; Advanced: `Defining Custom Topology`, `Observation of Topological Modifications` |
| NM5.8 | Lifecycle, accessibility, build, install, and package checks | Installation; Graph: `QuickQanava Initialization`, `Graph View`; API Reference notice; Licence |

Each sub-phase can use one or more PRs. Every PR must build and test its completed scope.

---

## 8. NM5.1 — Production link, initialization, and read-only proof

### 8.1 Input context

NM0 builds the pinned static QuickQanava module. It does not prove that the production engine can
load a Qan graph. This sub-phase proves that path before product projection work starts.

### 8.2 Official documentation

- [Installation](https://cneben.github.io/QuickQanava/installation.html): `QuickQanava Quick Start` and `Using from external projects with CMake`.
- [Graph](https://cneben.github.io/QuickQanava/graph.html): `QuickQanava Initialization` and `Graph View`.
- [Advanced use (C++)](https://cneben.github.io/QuickQanava/advanced.html): `Using from C++`.

### 8.3 Work

1. Link the production owner target to the pinned QuickQanava target.
2. Call the pinned initialization function before the production engine loads QML.
3. Keep `QQuickStyle::setStyle("Basic")`.
4. Add a Loader-owned, read-only Qan graph proof to a test harness.
5. Create the graph and one node only through documented Qan entry points.
6. Destroy and recreate the Loader repeatedly.
7. Record all differences between website examples and the pinned interface.

### 8.4 Main call chain

```text
alcedo_main startup
  -> set Qt Quick Controls Basic style
  -> initialize pinned QuickQanava module
  -> load Alcedo QML engine
  -> harness Loader creates Qan.GraphView
  -> Qan.Graph binds to GraphView.graph
```

### 8.5 Files

- `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt`
- `alcedo_studio/src/ui/alcedo_main/main.cpp`
- an NM5 integration harness under `alcedo_studio/tests/ui/`
- `alcedo_studio/tests/ui/CMakeLists.txt`

### 8.6 Tests and exit criteria

- The production target links the pinned module.
- Initialization occurs before QML load.
- The production style remains Basic.
- The harness loads and destroys the Qan objects without warnings or stale access.
- A missing Qan import fails with the real QML error.
- The test does not replace a failed graph with a placeholder implementation.

### 8.7 Completion record

Record the date, revision, exact initialization signature, commands, test count, and upstream
differences here after implementation.

##### NM5.1 completion record (2026-09-03)

**Status:** complete — production `alcedo_main` links the pinned QuickQanava static module and
initializes it before the Alcedo QML module loads. The production-style harness proves a
Loader-owned `Qan.GraphView`/`Qan.Graph` with one visual node, repeated Loader teardown and
recreation, and the real missing-module error path.

**Revision:** base repository revision `9ba15d40`, branch
`feature/nodes-panel-foundation`; QuickQanava remains submodule tag `2.50` at
`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`. The pinned checkout has no source changes.

**Primary success call chains:**

```text
alcedo_main startup
  -> QQuickStyle::setStyle("Basic")
  -> QQmlApplicationEngine with qrc:/ import path
  -> QuickQanava::initialize(QQmlEngine* engine) with &engine
  -> engine.loadFromModule("Alcedo.Main", "Main")
```

```text
QuickQanavaProductionIntegrationTest
  -> Basic style and qrc:/ import path
  -> QuickQanava::initialize(&engine)
  -> Loader sourceComponent creates Qan.GraphView
  -> Qan.Graph binds to GraphView.graph
  -> Component.onCompleted calls graph.insertNode()
  -> node label and visual item are observed
  -> Loader active=false/true destroys and recreates the graph five times
```

**Tests and evidence:**

| Evidence | Result |
| --- | --- |
| `alcedo_main` links `QuickQanava` and `QuickQanavaplugin` | PASS |
| Basic style remains selected before engine creation | PASS |
| One `Qan.Graph` and one `Qan.Node` load through the Loader | PASS |
| `Qan.GraphView.graph` binds to the Loader-owned graph and the node has an item | PASS |
| Five Loader teardown/recreation cycles leave no stale graph objects | PASS |
| Missing QuickQanava import reports the actual QML error and creates no root object | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target QuickQanavaProductionIntegrationTest alcedo_main --parallel 4
build/debug/alcedo_studio/tests/ui/QuickQanavaProductionIntegrationTest_runtime/QuickQanavaProductionIntegrationTest.exe --gtest_color=no
```

The direct test run passed all 3 tests. A filtered CTest run could not reach this target because
the UI directory's existing PRE_TEST discovery first launches
`SharedToneCurveTest_runtime/SharedToneCurveTest.exe`, which exits with `0xc0000139` for the
pre-existing lensfun runtime entry-point mismatch. The new binary was therefore run directly;
its own runtime directory contains the expected offscreen Qt plugins and all three tests pass.

**Pinned API differences:**

- The website Graph snippet shows a no-argument `QuickQanava::initialize()` form, but the pinned
  2.50 header exposes only `static void initialize(QQmlEngine* engine)`. Production and the
  harness use the pinned `QuickQanava::initialize(&engine)` signature.
- Website external-project examples add a source-directory import path. Alcedo embeds the static
  module and adds `qrc:/` before initialization, which is the path used by the pinned
  `QuickQanava` component factories.
- The pinned QML sources use unversioned `import QuickQanava as Qan`, while the public harness
  uses the documented `import QuickQanava 2.0 as Qan`. Qt 6.9.3 therefore needs the Alcedo-owned
  QML module wrapper to publish the pinned QML files at both 2.0 and 1.0 while keeping the
  module's public version at 2.0. This keeps the submodule checkout unchanged and allows both
  the documented entry point and the pinned nested imports to resolve.

**Checklist / exit condition:** all NM5.1 work and exit conditions are checked. No production
PipelineDocument or Qan projection was added in this setup phase.

**LOC note (grill-code-review):** 237-line integration harness; 30-line test registration;
7-line QML module-version compatibility setup; 8 production link/initialization lines. The
QuickQanava submodule source remains unchanged.

**Residual gaps:** NM5.2–NM5.8 still own the immutable Alcedo projection, node delegate, Nodes
rail page, selection/navigation state, graph commands, visual connector, and final lifecycle/build
qualification. macOS checks were not run on this Windows host. The unrelated CTest discovery
runtime mismatch remains outside NM5.1.

---

## 9. NM5.2 — Default-name counter and immutable projection

### 9.1 Input context

NM4 can rebuild the full document at any history head. `PipelineDocument` does not yet store a
default-name counter. The UI also has no immutable graph snapshot.

This sub-phase changes product data and application projection. It does not create production Qan
primitives.

### 9.2 Official documentation

- [Graph](https://cneben.github.io/QuickQanava/graph.html): `Data Model`.
- [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html): `Adding content` and `Observing Topology`.
- [Advanced use (C++)](https://cneben.github.io/QuickQanava/advanced.html): `Using from C++` and `Defining Custom Topology`.

These sections explain Qan topology and visual primitives. They do not define Alcedo naming or
history. Alcedo owns both.

### 9.3 Work

1. Add the serialized next-name counter to `PipelineDocument`.
2. Set the primary default name to `Color Grade 1`.
3. Set the new-document counter to `2`.
4. Make a successful Add consume one number.
5. Keep the counter unchanged after Add failure, Rename, Remove, or Reconnect.
6. Extend typed Add replay with exact counter state.
7. Update format validation and fixtures.
8. Add immutable node, Mask, edge, revision, and generation projection values.
9. Project MaskId and source kind in display order.
10. Omit adjustment and enabled roles from the node projection.

### 9.4 Main call chains

Read path:

```text
history head or Version checkout
  -> current PipelineDocument under the read boundary
  -> EditorNodeGraphProjection
  -> immutable EditorNodeGraphSnapshot
  -> UI-thread publication with session generation
```

Default-name path:

```text
Add command validation
  -> read next_color_grade_name_number
  -> create a local Color Grade change with "Color Grade N" and next value N + 1
  -> validate the complete local graph change
  -> capture node JSON and exact before/after counter values
  -> accept both node and counter changes, or restore both on failure
```

### 9.5 Files and interfaces

Plan to modify:

- `pipeline_document.hpp/.cpp`
- `color_grade_node_model.cpp`
- `pipeline_graph_commands.hpp/.cpp`
- `pipeline_edit_batch.hpp/.cpp`
- `pipeline_document_history.hpp/.cpp`
- NM4 graph/history tests and stored fixtures

Plan to add:

- `editor_node_graph_projection.hpp/.cpp`
- projection tests

### 9.6 Tests and exit criteria

- A default document contains `Color Grade 1` and counter value `2`.
- Two successful Adds create `Color Grade 2` and `Color Grade 3`.
- Rename does not change the next value.
- Remove does not reuse a consumed value in the same document state.
- Reconnect does not change names or counter state.
- Add failure does not consume a value.
- Undo and Redo restore exact node names and counter values.
- Recovery and reopen restore exact node names and counter values.
- Version checkout publishes the matching projection.
- Mask projection order equals model display order.
- A parameter edit does not publish a topology rebuild.
- A stale generation snapshot is rejected.

### 9.7 Completion record

Record the date, format revision, schema field, commands, test count, success chain, and failure
chain here after implementation.

##### NM5.2 completion record (2026-09-03)

**Status:** complete — `PipelineDocument` now stores and validates the next default Color Grade
name number. A new document starts with `Color Grade 1` and counter value `2`; a successful typed
Add consumes exactly one value, while failure, Rename, Remove, Reconnect, and stored-node
insertion leave the counter unchanged. Forward, inverse, recovery, reopen, and Version checkout
restore the recorded counter and display name values exactly. The new Alcedo-owned projection
contains only stable node, Mask, edge, revision, and session-generation values; it does not add
production Qan primitives.

**Revision and format:** base repository revision `777c9244` (`NM5.1`), branch
`feature/nodes-panel-name-counter-projection`. The serialized field is
`next_color_grade_name_number`. Because the field changes the persisted document and typed history
payloads, all incompatible format identities move together: project `0.5.0`, packed project `5`,
PipelineDocument `5`, image-edit schema `3`, commit `3`, chain `3`, typed batch `2`, root `3`,
checkpoint `3`, Mini-Git WAL `4`, and transfer schema `alcedo.adjustment_transfer.v3`. No old
format migration or substitute read path was added.

**Primary success call chains:**

```text
AddColorGrade
  -> validate predecessor, node id, and complete candidate graph
  -> read next_color_grade_name_number = N
  -> assign "Color Grade N" to the local Color Grade model
  -> validate and apply the candidate graph
  -> consume N only after graph acceptance
  -> capture typed Add before = N and after = N + 1
  -> forward replay applies stored node JSON and exact after value
  -> inverse replay removes the node and restores exact before value
```

```text
history head / recovery / reopen / Version checkout
  -> materialize the exact PipelineDocument JSON
  -> EditorNodeGraphProjection::Build
  -> value-only EditorNodeGraphSnapshot
  -> EditorNodeGraphProjection::AcceptsGeneration
```

Stored-node and transfer path:

```text
Paste or stored Color Grade insertion
  -> remap stored node JSON
  -> typed Add before = after = current counter
  -> InsertColorGradeFromJson
  -> keep the stored display name and counter unchanged
```

**Failure chain:**

```text
invalid, duplicate, disconnected, or exhausted Add
  -> return the actual graph or counter error
  -> do not apply the candidate document
  -> leave graph, display names, and counter unchanged
```

**Projection values and update rules:** `EditorNodeGraphSnapshot` owns copied vectors for nodes
and edges plus `session_generation`, `projection_revision`, and `topology_revision`. Each node
copies `NodeId`, node kind, display name, and ordered Mask `{MaskId, source_kind}` values. Each edge
copies its endpoint node and port IDs. Adjustment values, enabled state, mix values, topology
numbers, and Qan pointers are absent. A parameter-only document edit can rebuild value data with
the same topology revision; a topology update receives a new topology revision; a session consumer
accepts only an exact session generation.

**Tests and evidence:**

| Target | Result |
| --- | --- |
| `EditorNodeGraphProjectionTest` | 6/6 passed: values, Mask order/source kinds, parameter stability, revisions, generation, invalid backbone. |
| `PipelineDocumentDefaultNameTest` | 6/6 passed: defaults, increasing names, failure, replay, stored insertion, format/overflow validation. |
| `PipelineHistoryApplierTest`, `PipelineEditBatchTest`, `GpuDagModelGraphTest`, `PipelineDocumentCheckpointTest` | 109/109 passed. |
| `DocumentTransferTest`, `AdjustmentTransferServiceMiniGitTest`, `AdjustmentTransferServiceTest` | 24/24 passed. |
| `EditorSessionHistoryPortTest`, `EditorSessionCheckpointStoreTest` | 76/76 passed, including typed Add, recovery, reopen, checkout counter/name assertions, and projection rebuilds for both checked-out states. |
| **Total direct runtime execution** | **221/221 passed across 11 targets.** |

Commands used:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorNodeGraphProjectionTest PipelineDocumentDefaultNameTest PipelineHistoryApplierTest PipelineEditBatchTest GpuDagModelGraphTest PipelineDocumentCheckpointTest DocumentTransferTest AdjustmentTransferServiceMiniGitTest AdjustmentTransferServiceTest EditorSessionHistoryPortTest EditorSessionCheckpointStoreTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorSessionHistoryPortTest PipelineDocumentDefaultNameTest --parallel 4
```

The selected CTest listing could not complete because its existing UI pre-test discovery launches
`SharedToneCurveTest_runtime/SharedToneCurveTest.exe`, which exits with `0xc0000139` from the
pre-existing lensfun runtime entry-point mismatch. The 11 target binaries were therefore run from
their generated `_runtime` directories; all 221 tests passed, including the final rebuilt session
history target.

**Pinned API differences:** none in this sub-phase. NM5.2 adds no Qan dependency or primitive; the
projection is an Alcedo-owned C++ value boundary. The pinned QuickQanava integration and its
documented-versus-pinned initialization differences remain recorded in NM5.1.

**LOC note (grill-code-review):** 413 production additions including the new projection library,
257 lines in two new focused test files, and 314 additions/77 deletions in already tracked files
including format fixtures and regression assertions. The pre-existing 1,179-line
`pipeline_edit_batch.cpp` is 1,222 lines after the focused validation additions; no new source or
test file exceeds the repository review threshold, and the existing large source file was not
split outside this phase's scope.

**Residual gaps:** NM5.3–NM5.8 still own Qan adapter primitives, ports, delegates, Nodes rail/page
UI, local layout state, visual connector behavior, accessibility, packaging, and final lifecycle
qualification. The CTest discovery runtime mismatch remains outside this sub-phase; direct test
binaries pass. macOS checks were not run on this Windows host.

---

## 10. NM5.3 — Qan adapter, ports, and read-only topology

### 10.1 Input context

NM5.2 provides immutable values with stable IDs and explicit revisions. QuickQanava still cannot
become a product data source.

This sub-phase maps a snapshot into Qan primitives. It does not expose product graph commands.

### 10.2 Official documentation

- [Graph](https://cneben.github.io/QuickQanava/graph.html): `Data Model` and `Graph View`.
- [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html): `Adding content`, `Docks and Ports`, and `Observing Topology`.
- [Edges](https://cneben.github.io/QuickQanava/edges.html): `Creating Edges`.
- [Advanced use (C++)](https://cneben.github.io/QuickQanava/advanced.html): `Defining Custom Topology`.

### 10.3 Work

1. Add the thin `AlcedoQanGraph` adapter.
2. Create one Qan primitive for each projected pipeline node.
3. Create one Qan edge for each projected backbone edge.
4. Create and bind the real top and bottom ports.
5. Keep NodeId and edge identity maps outside QML text labels.
6. Apply role-only updates without rebuilding topology.
7. Replace all primitives after Version or generation replacement.
8. Reject callbacks from an older generation.
9. Clear reverse maps before object destruction.

### 10.4 Main call chain

```text
EditorNodeGraphSnapshot
  -> AlcedoQanGraph compares revisions
  -> documented Qan insert/remove calls
  -> documented port insertion and binding
  -> Qan graph bound to GraphView
```

### 10.5 Files and interfaces

Plan to add:

- `alcedo_qan_graph.hpp/.cpp`
- adapter tests

Plan to modify:

- `AlbumBackendLib` sources and links
- QML type registration if QML constructs the adapter
- `alcedo_main/CMakeLists.txt`

Verify exact pinned names for:

- node and edge insertion/removal;
- port insertion;
- edge source and destination binding;
- primitive visual-item access;
- node selected state;
- object-destruction signals.

### 10.6 Tests and exit criteria

- NodeId maps to one live Qan node in the current generation.
- Each backbone edge binds to the correct ports.
- A Rename updates one label without replacing the graph.
- A Mask kind change updates one node without replacing edges.
- A Version replacement removes every old primitive and reverse-map entry.
- A stale primitive cannot select or edit the new document.
- Adapter failure restores the prior complete Qan projection.

### 10.7 Completion record

Record the date, pinned API signatures, commands, test count, success chain, and failure chain here.

##### NM5.3 completion record (2026-09-03)

**Status:** complete — `AlcedoQanGraph` maps an immutable `EditorNodeGraphSnapshot` onto
documented QuickQanava primitives. Each projected node, backbone edge, and real top/bottom
port is created through pinned `insertNode` / `insertEdge` / `insertPort` / `bindEdge` calls.
NodeId and edge identity live only in adapter maps, not in Qan labels. Rename and Mask
source-kind updates keep the live Qan objects. Version or generation replacement clears
reverse maps before destroying primitives. A stale snapshot or Qan pointer cannot select or
edit the live document. A failed Qan insert restores the prior complete projection. This
sub-phase does not expose Add, Rename, Delete, or Reconnect product commands, and QML does
not construct the adapter yet.

**Revision:** base repository revision `2829e926` (`NM5.2`), branch
`feature/nodes-panel-qan-adapter`. QuickQanava remains submodule tag `2.50` at
`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`.

**Pinned API signatures used:**

```text
qan::Graph::insertNode(QQmlComponent* = nullptr, qan::NodeStyle* = nullptr)
qan::Graph::removeNode(qan::Node* node, bool force = false)
qan::Graph::insertEdge(qan::Node* source, qan::Node* destination, QQmlComponent* = nullptr)
qan::Graph::removeEdge(qan::Edge* edge, bool force = false)
qan::Graph::insertPort(qan::Node* node, qan::NodeItem::Dock dock,
                       qan::PortItem::Type portType = InOut, QString label = "", QString id = "")
qan::Graph::bindEdge(qan::Edge* edge, qan::PortItem* outPort, qan::PortItem* inPort)
qan::Node::setLabel(const QString&)
qan::Node::getItem()
qan::NodeItem::findPort(const QString& portId)
qan::EdgeItem::getSourceItem() / getDestinationItem()
QObject::destroyed
```

**Primary success call chain:**

```text
EditorNodeGraphProjection::Build
  -> EditorNodeGraphSnapshot
  -> AlcedoQanGraph::ApplySnapshot
  -> same generation + topology revision: setLabel and Mask roles in place
  -> generation or topology change: clear identity maps
  -> qan::Graph::removeEdge / removeNode(force)
  -> qan::Graph::insertNode / insertPort / insertEdge / bindEdge
  -> NodeId <-> QPointer<qan::Node> and edge-identity maps
  -> GraphView-owned qan::Graph
```

**Primary failure call chain:**

```text
stale session/topology/projection revision
  -> reject ApplySnapshot
  -> live Qan primitives, maps, and revisions unchanged
```

```text
Qan insertNode / insertPort / insertEdge / bindEdge failure
  -> rebuild_in_progress rejects LiveNodeId
  -> destroy partial primitives
  -> rebuild the prior snapshot
  -> return the real Qan error; no substitute graph
```

**Pinned API differences:**

- The `insertPort` comment refers to `qan::NodeItem::getPort()`. Pinned 2.50 exposes
  `findPort(const QString& portId)` instead. The adapter stores QPointer ports keyed by
  product PortId and uses empty Qan port labels with `in:` / `out:` identity strings.
- `qan::Node::delegate()` and `qan::Edge::delegate()` cache a process-lifetime
  `QQmlComponent` on the first `QQmlEngine`. A second engine cannot create visual items
  through C++ `insertNode()`. Adapter tests share one engine and recreate the Loader-owned
  graph. Production already uses one engine.
- The class comment says visual connectors default to enabled. Pinned 2.50 sets
  `_connectorEnabled = false`. This sub-phase leaves that default; later work enables
  request-only mode.
- Website `QuickQanava::initialize()` remains `initialize(QQmlEngine*)` as recorded in NM5.1.

**Tests and evidence:**

| Required criterion | Test | Result |
| --- | --- | --- |
| NodeId maps to one live Qan node | `MapsEachProjectedNodeIdToOneLiveQanNodeInTheCurrentGeneration` | PASS |
| Backbone edges bind to top/bottom ports | `BindsEachBackboneEdgeToTheMatchingTopAndBottomPorts` | PASS |
| Rename updates one label without replacing the graph | `RenameUpdatesOneNodeLabelWithoutReplacingQanPrimitives` | PASS |
| Mask kind change updates one node without replacing edges | `MaskKindChangeUpdatesOneNodeWithoutReplacingEdges` | PASS |
| Version replacement removes old primitives and reverse maps | `VersionReplacementRemovesOldPrimitivesAndReverseMapEntries` | PASS |
| Stale primitive cannot select or edit | `StalePrimitiveCannotSelectOrEditTheNewDocument` | PASS |
| Adapter failure restores the prior projection | `AdapterInsertFailureRestoresThePriorCompleteQanProjection` | PASS |
| Loader destruction clears identity maps | `GraphDestructionClearsIdentityMaps` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlcedoQanGraphTest --parallel 4
build/debug/alcedo_studio/tests/ui/AlcedoQanGraphTest_runtime/AlcedoQanGraphTest.exe --gtest_color=no
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumBackendLib --parallel 4
```

Suite totals: **8/8 passed** on the direct runtime binary. `AlbumBackendLib` links
`AlcedoQanGraph`. QML type registration was not added because QML does not construct the
adapter in this sub-phase.

**Checklist / exit condition:** all NM5.3 work and exit conditions are checked. No node
delegate, Mask drawer, Nodes rail page, or graph command was added.

**LOC note (grill-code-review):** 212-line adapter header, 450-line adapter source, 421-line
focused test file, plus CMake wiring in `alcedo_main/CMakeLists.txt` and `tests/ui/CMakeLists.txt`.
No changed file exceeds the 1000-line review threshold.

**Residual gaps:** NM5.4–NM5.8 still own the Alcedo node delegate, Mask drawer, Nodes rail
page, layout/selection store, Add/Rename/Delete, visual connector, accessibility, and
package checks. macOS checks were not run on this Windows host. The unrelated CTest
discovery runtime mismatch remains outside this sub-phase; the adapter binary was run
directly.

---

## 11. NM5.4 — Alcedo node delegate and Mask drawer

### 11.1 Input context

NM5.3 provides a read-only Qan graph. Default QuickQanava delegates do not follow Alcedo VI. The
approved node uses a variable-height Mask drawer.

This sub-phase changes display only. It does not expose graph commands.

### 11.2 Official documentation

- [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html): `Docks and Ports`, `Node Resizing`, `Selection`, and `Defining Custom Nodes`.
- [Styles](https://cneben.github.io/QuickQanava/styles.html): `Defining Styles` and `Node Style`.
- [Samples](https://cneben.github.io/QuickQanava/samples.html): `Custom Nodes: 'custom'`.
- [Advanced use (C++)](https://cneben.github.io/QuickQanava/advanced.html): `Defining Custom Topology`.

The upstream Material section identifies style binding points only. Do not import Material.

### 11.3 Work

1. Add the Color Grade delegate with name row and Mask drawer.
2. Add compact Develop and DRT/Post delegates.
3. Keep Qan node resize disabled for user input.
4. Let the delegate height follow its drawer content.
5. Verify automatic rectangular bound updates after height changes.
6. Keep the output port at the current node bottom.
7. Add Gradient, Radial, and Brush type rows.
8. Add the four approved SVG resources.
9. Add required graph AppTheme roles and document them in `DESIGN.md`.
10. Use a shared disclosure control and existing fold motion.
11. Apply all copy and decoration bans from Section 5.13.

### 11.4 Main call chain

```text
projected node and Mask kinds
  -> Qan primitive uses Alcedo custom delegate
  -> node name row
  -> default-open Mask drawer
  -> icon and localized type row for each Mask
  -> delegate height changes
  -> Qan rectangular bounds and bound edges update
```

### 11.5 Files

Plan to add:

- `qml/EditorNodeDelegate.qml`
- `qml/EditorEndpointNodeDelegate.qml`
- `qml/EditorNodeMaskDrawer.qml`
- `qml/EditorNodeMaskTypeRow.qml`
- `qml/EditorNodePortDelegate.qml`
- `qml/EditorNodeEdgeDelegate.qml` only if the pinned extension point requires it
- `config/panel_icons/nodes.svg`
- `config/mask_icons/gradient.svg`
- `config/mask_icons/radial.svg`
- `config/mask_icons/brush.svg`

Plan to modify:

- `config/resource.qrc`
- `app_theme.hpp/.cpp`
- `DESIGN.md`
- `alcedo_main/CMakeLists.txt`

### 11.6 Tests and exit criteria

- The node has no topology number.
- The node has no status dot.
- The node has no On or Off text.
- The node has no adjustment summary.
- The node has no Mask count.
- The node has no pill or badge.
- Each Mask row shows only approved type icon and type label.
- Mask row order equals model display order.
- The drawer starts open for a new layout key.
- The user can close and reopen the drawer.
- The output port and edges follow both heights.
- Drawer input does not create history or rendering.
- Production QML has no Material import.
- Node effects, shadow, glow, and gradient stay disabled.
- Both themes and DPR 1.0, 1.25, 1.5, and 2.0 keep icons clear.

### 11.7 Completion record

Record the date, screenshots, token additions, pinned delegate API, commands, and test count here.

##### NM5.4 completion record (2026-09-03)

**Status:** complete — Alcedo Color Grade, endpoint, Mask-drawer, port, and edge
delegates replace the default QuickQanava chrome. Color Grade cards show only
the display name and a default-open Mask drawer. Mask rows show the approved
type icon and localized type label in model display order. Drawer open/close is
local UI state: it does not write `PipelineDocument`, create history, or start
a photo render. Output ports and bound edges follow both open and closed
heights. This sub-phase does not add the Nodes rail page, layout store, or
graph commands.

**Revision:** base repository revision `b3de1074` (`NM5.3`), branch
`feature/nodes-panel-delegates`. QuickQanava remains submodule tag `2.50` at
`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`.

**Pinned delegate API:**

```text
qan::Graph::insertNode(QQmlComponent* nodeComponent, qan::NodeStyle* = nullptr)
qan::Graph::insertEdge(qan::Node* source, qan::Node* destination, QQmlComponent*)
qan::Graph::portDelegate / qmlSetPortDelegate  (graph takes ownership)
qan::NodeItem root for custom node delegates
qan::NodeItem::resizable = false
qan::NodeItem::setDefaultBoundingShape()
qan::HorizontalDock anchors.top: hostNodeItem.bottom
qan::EdgeItem::srcShape / dstShape = EdgeStyle::None
```

**Token additions (AppTheme + DESIGN.md):** `graphCanvasColor`, `graphGridColor`,
`graphEdgeColor`, `graphCandidateEdgeColor`, `graphPortFillColor`,
`graphPortBorderColor`, `graphSelectionOutlineColor`, `graphNodeWidth` (220),
`graphEndpointHeight` (40), `graphNameRowHeight` (32),
`graphMaskDrawerHeaderHeight` (28), `graphMaskRowHeight` (28), `graphPortSize`
(8), `graphPortHitSize` (16), `graphEdgeWidth` (1),
`graphSelectionOutlineWidth` (1).

**Assets:** `panel_icons/nodes.svg` (Tabler `stack-2`) and
`mask_icons/{gradient,radial,brush}.svg` (Tabler `wash-dry`, `wash-dryclean`,
`brush`), 24×24, white stroke, user-approved 2 px width, listed in
`resource.qrc`.

**Primary success call chain:**

```text
EditorNodeGraphSnapshot
  -> AlcedoQanGraph::EnsureDelegates
  -> qan::Graph::insertNode(Color Grade or endpoint QQmlComponent)
  -> ApplyNodePresentation (label, nodeKind, masks, resizable=false)
  -> qan::Graph::insertPort with Alcedo square portDelegate
  -> qan::Graph::insertEdge with Alcedo edge delegate
  -> Color Grade name row + default-open Mask drawer
  -> icon and localized type row for each Mask
  -> setDefaultBoundingShape after height change
  -> bottom dock / output port follow the current node bottom
```

**Primary failure call chain:**

```text
missing or unloadable delegate URL
  -> ApplySnapshot returns the real QML or empty-URL error
  -> no Qan primitives inserted
  -> no fallback to default Node.qml / Port.qml / Edge.qml
```

```text
Mask drawer header toggle
  -> local expanded / drawerOpen only
  -> projection revision, topology revision, and node presentation unchanged
```

**Pinned API differences:**

- Website custom-node samples import Material. Alcedo delegates use Basic and
  AppTheme only.
- Website text says rectangular bounds regenerate automatically on width/height
  change. Pinned `qan::NodeItem` only fills an empty shape lazily; Alcedo
  delegates call `setDefaultBoundingShape()` on size changes.
- Default `NodeStyle.effectType` is shadow. Alcedo delegates do not use
  `Qan.RectNodeTemplate`, so those effects never paint.
- Default edge `dstShape` is Arrow. Each Alcedo edge owns a dedicated
  `Qan.EdgeStyle` with `None` endings and `graphEdgeWidth` / `graphEdgeColor`.
- `qan::Graph::portDelegate` takes ownership of the component. The adapter
  installs a new instance on each bound graph instead of sharing one object
  across Loader recreation.
- Website `insertNode()` snippets omit a component. Alcedo always passes the
  kind-specific delegate; an empty URL is a hard error.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Node has no topology number, On/Off, adjustment summary, Mask count, pill, or badge | `ColorGradeShowsNameWithoutTopologyStatusAdjustmentOrMaskCount` | PASS |
| Mask rows show only approved type icon and type label in display order | `MaskRowsShowOnlyApprovedTypeIconAndLabelInDisplayOrder` | PASS |
| Drawer starts open; user can close and reopen; no history/render | `MaskDrawerStartsOpenAndUserCanCloseAndReopenWithoutHistory` | PASS |
| Output port and edges follow both heights | `OutputPortAndEdgesFollowOpenAndClosedDrawerHeight` | PASS |
| Endpoints omit the Mask drawer | `EndpointDelegatesOmitMaskDrawerAndKeepCompactHeight` | PASS |
| Empty open drawer has no Mask rows | `EmptyOpenDrawerHasNoMaskRows` | PASS |
| Both themes keep AppTheme card tokens | `NodeVisualTokensMatchAppThemeInBothThemes` | PASS |
| Compact source size ≥ optical size; icons ready in both themes | `MaskTypeIconsStayReadyForCompactSourceSizeAcrossThemes` | PASS |
| Production node QML has no Material, shadow, glow, or gradient chrome | `ProductionNodeQmlHasNoMaterialImportOrEffectChrome` | PASS |
| Missing delegate URL fails with the real error | `ApplySnapshotFailsWhenColorGradeDelegateUrlIsEmpty` | PASS |
| Adapter mapping, ports, role updates, stale rejection, restore | `AlcedoQanGraphTest` (8 prior + empty-URL) | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlcedoQanGraphTest EditorNodeDelegateQmlTest AlcedoQanGraph AlcedoMainQml --parallel 4
build/debug/alcedo_studio/tests/ui/AlcedoQanGraphTest_runtime/AlcedoQanGraphTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorNodeDelegateQmlTest_runtime/EditorNodeDelegateQmlTest.exe --gtest_color=no
```

Suite totals: **9/9** `AlcedoQanGraphTest`, **9/9** `EditorNodeDelegateQmlTest`,
**18/18** direct runtime execution. CTest discovery still hits the unrelated
`SharedToneCurveTest` lensfun entry-point mismatch; binaries were run from
their `_runtime` directories.

No screenshot grab matrix was captured. VI bans, token equality, icon Ready
status, and both theme indices are asserted in-process. DPR 1.0 / 1.25 / 1.5 /
2.0 sharpness is covered by the DESIGN source≥optical token rule, not by
grabbed frames.

**Checklist / exit condition:** all NM5.4 work and exit conditions are checked.
No Nodes rail page, layout store, or graph command was added.

**LOC note (grill-code-review):** new delegates 97 + 59 + 198 + 85 + 31 + 32
QML lines; adapter source 665 and header 283 after delegate URL/install
additions; new `EditorNodeDelegateQmlTest` 574 lines; `app_theme.cpp` is 1,221
lines after 36 token getters (already over the 1,000-line review threshold
before this sub-phase; no responsibility split in this display-only change).
No new source file exceeds 1,000 lines.

**Residual gaps:** NM5.5–NM5.8 still own the Nodes rail page, navigation,
selection/`EditorNodeController`, layout store, Add/Rename/Delete, visual
connector, accessibility of the full page, packaging, and screenshot visual
matrix. The `nodes.svg` asset is registered but not yet a rail action. macOS
checks were not run on this Windows host. The unrelated CTest discovery runtime
mismatch remains outside this sub-phase.

---

## 12. NM5.5 — Rail, navigation, selection, and layout state

### 12.1 Input context

NM5.4 provides the read-only production visual. The rail still supports only History and Versions.
The rail destroys its page Loader after a complete close.

This sub-phase enables selection and UI layout. It does not expose graph mutations.

### 12.2 Official documentation

- [Graph](https://cneben.github.io/QuickQanava/graph.html): `Graph View` and `Grid`.
- [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html): `Node Resizing` and `Selection`.
- [Utilities](https://cneben.github.io/QuickQanava/utilities.html): `Navigable`.
- [Samples](https://cneben.github.io/QuickQanava/samples.html): `Navigable Area: 'navigable'` and `Topology Sample: 'topology'`.

### 12.3 Work

1. Rename `historyPanelPage` to a neutral tool-page property.
2. Accept only `""`, `history`, `versions`, and `nodes`.
3. Migrate object names and tests to neutral rail names.
4. Add the approved Nodes rail action.
5. Add `EditorNodesPanel.qml`.
6. Reuse the current rail fold and Loader.
7. Destroy graph delegates after a complete close.
8. Keep plain layout values outside the Loader.
9. Use the existing side-panel width range.
10. Preserve the 360 logical-pixel viewer floor.
11. Configure one selected node.
12. Make `EditorNodeController.selectedNodeId` authoritative.
13. Add backbone keyboard navigation.
14. Add `Ctrl+0` Fit behavior after pinned-header verification.
15. Save node positions, view position, zoom, selection, and drawer state.
16. Restore per-Version layout and selection.

### 12.4 Main call chains

Open:

```text
Nodes rail action
  -> tool page becomes nodes
  -> existing fold opens the body
  -> Loader creates EditorNodesPanel
  -> GraphView binds AlcedoQanGraph
  -> layout store applies positions, view, zoom, drawer state, and selection
```

Close:

```text
Nodes rail action or another tool page
  -> capture plain layout values
  -> fold closes
  -> Loader destroys GraphView at progress zero
  -> controller and layout store keep no Qan pointers
```

Select:

```text
documented Qan node click
  -> adapter resolves NodeId and generation
  -> EditorNodeController.selectNode
  -> selectedNodeId changes
  -> adapter applies the one-node visual selection
```

### 12.5 Keyboard and accessibility

| Input | Result |
| --- | --- |
| `Tab`, `Shift+Tab` | Move through the header, canvas, and operable drawer headers. |
| `Up`, `Down` | Select the previous or next backbone node. |
| `Home`, `End` | Select Develop or DRT/Post. |
| `Enter`, `Space` on drawer header | Open or close the current drawer. |
| `F2` | Rename a selected Color Grade after NM5.6. |
| `Delete` | Delete a selected Color Grade after NM5.6. |
| `Ctrl++` | Add a Color Grade after NM5.6. |
| `Ctrl+0` | Fit the graph. |
| `Escape` | Cancel rename or a connector request. Otherwise return focus to the canvas. |

Every action needs localized tooltip text and `Accessible.name`. Every focus target needs a visible
focus treatment. A screen reader must read the node name and Mask type rows. It must not announce
hidden enabled or adjustment data.

### 12.6 Files

Plan to add:

- `qml/EditorNodesPanel.qml`
- `editor_node_controller.hpp/.cpp`
- `editor_node_layout_store.hpp/.cpp`

Plan to modify:

- `EditorWorkspaceRail.qml`
- `EditorWorkspace.qml`
- `EditorSessionController`
- QML type registration
- `alcedo_main/CMakeLists.txt`
- rail and workspace tests

### 12.7 Tests and exit criteria

- Only the four allowed page keys are accepted.
- History, Versions, and Nodes are mutually exclusive.
- A full close leaves no graph delegates.
- Reopen restores positions, view, zoom, selection, and drawer state.
- Two Versions keep separate layout values.
- Undo restoration of a NodeId can recover its prior position and drawer state.
- Ctrl-click cannot create a second product selection.
- Graph movement and drawer folds do not start photo rendering.
- `reduceMotion` makes all related folds immediate.
- The viewer stays at least 360 logical pixels wide at 960×640.

### 12.8 Completion record

Record the date, pinned navigation API, commands, test count, success chain, and failure chain here.

##### NM5.5 completion record (2026-09-03)

**Status:** complete — Nodes is the third expandable tool-rail page. History,
Versions, and Nodes share one Loader and one `editorToolPanelPage` key
(`""` / `history` / `versions` / `nodes`). `EditorNodeController` owns
`selectedNodeId` and publishes the immutable projection.
`EditorNodeLayoutStore` owns view position, zoom, node positions, Mask-drawer
open state, selection, and preferred panel width, keyed by project, image, and
Version. A full close destroys GraphView delegates. Reopen and Version checkout
restore local layout. Graph movement, Fit, and drawer folds do not write
`PipelineDocument` or start a photo render. Add/Rename/Delete remain disabled
until later work.

**Revision:** base repository revision `a74dfe9c` (`NM5.4`), branch
`feature/nodes-panel-rail-selection-layout`. QuickQanava remains submodule tag
`2.50` at `56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`.

**Pinned navigation API:**

```text
Qan.GraphView.navigable
Qan.GraphView.fitContentInView()
Qan.GraphView.zoom / containerItem / navigated / nodeClicked / rightClicked
Qan.GraphView.selectionRectEnabled = false
Qan.LineGrid via GraphView.grid / gridThickColor
qan::Graph::setMultipleSelectionEnabled(false)
qan::Graph::clearSelection / setNodeSelected
qan::Graph::nodeMoved
qan::Navigable::moveTo / centerOnPosition
```

**Primary success call chain:**

```text
Nodes rail action
  -> EditorSessionController.editorToolPanelPage = "nodes"
  -> existing fold opens the body
  -> Loader creates EditorNodesPanel
  -> EditorNodeController.refreshFromSession
  -> EditorNodeGraphProjection::Build
  -> AlcedoQanGraph::ApplySnapshot
  -> EditorNodeLayoutStore.activate + EnsureDefaultPositions
  -> GraphView zoom / pan / node positions / drawerOpen / one-node selection
```

```text
documented Qan node click
  -> AlcedoQanGraph.liveNodeId
  -> EditorNodeController.selectNode
  -> selectedNodeId changes
  -> AlcedoQanGraph.applyProductSelection (one Qan selected node)
```

**Primary failure call chain:**

```text
stale session generation snapshot
  -> EditorNodeController.PublishSnapshot rejects
  -> live snapshot, selection, layout store, and Qan graph unchanged
  -> lastError reports the real generation mismatch
```

```text
unknown NodeId or Ctrl-click extra node
  -> selectNode fails closed or replaces the one product selection
  -> Qan multipleSelectionEnabled is false
  -> product selectedNodeId remains a single NodeId
```

**Pinned API differences:**

- Website Graph View samples leave `multipleSelectionEnabled` true and show a
  Material-blue selection rectangle. Alcedo sets `multipleSelectionEnabled`
  false and `selectionRectEnabled` false. Product selection is the controller
  NodeId; the Alcedo card outline is the visible selected state.
- `qan::Graph::setSelectionDelegate` is protected in pinned 2.50, and
  `selectionDelegate: null` resets the default blue animated selection item
  instead of disabling it. The adapter installs an inline invisible `Item`
  through `QObject::setProperty`; see the visual polish follow-up below.
- Official `GraphView.qml` ships AlwaysOn scrollbars and an origin cross.
  The Nodes canvas turns both off. Fit uses pinned `fitContentInView()`.
- Website `QuickQanava::initialize()` remains `initialize(QQmlEngine*)` as
  recorded in NM5.1. Rail QML tests must add `qrc:/` to the engine import path
  before initialize, or `Qan.LineGrid` does not resolve.
- `EditorWorkspaceRail` owns a typed `Component { EditorNodesPanel { } }`.
  QML parses that type when `Main.qml` loads, even before the Nodes Loader
  creates the page. Every executable that loads production `Main.qml` must
  link `QuickQanavaplugin` and call `QuickQanava::initialize` after
  `addImportPath("qrc:/")`. Production `main.cpp` already does this. The
  shared Main.qml test fixture now matches that startup order.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Only `""`, `history`, `versions`, `nodes` accepted | `EditorSessionToolPanelPage.AcceptsOnlyEmptyHistoryVersionsAndNodes` | PASS |
| History, Versions, and Nodes are mutually exclusive | `EditorNodesPanelQmlTest.HistoryVersionsAndNodesAreMutuallyExclusive` | PASS |
| Full close leaves no graph delegates | `EditorNodesPanelQmlTest.FullCloseDestroysGraphDelegates` | PASS |
| Reopen restores positions, view, zoom, selection, drawer | `EditorNodesPanelQmlTest.ReopenRestoresPositionsViewZoomSelectionAndDrawerState` | PASS |
| Two Versions keep separate layout values | `EditorNodesPanelQmlTest.TwoVersionsKeepSeparateLayoutValues` | PASS |
| Undo restoration of a NodeId recovers position and drawer | `EditorNodeLayoutStore.RemovedNodeIdKeepsPriorPositionAndDrawerState` | PASS |
| Ctrl-click cannot create a second product selection | `EditorNodesPanelQmlTest.CtrlClickCannotCreateASecondProductSelection` | PASS |
| Graph movement and drawer folds do not start photo rendering | `EditorNodesPanelQmlTest.GraphMovementAndDrawerFoldsDoNotStartPhotoRendering` | PASS |
| `reduceMotion` makes related folds immediate | `EditorNodesPanelQmlTest.ReduceMotionMakesRelatedFoldsImmediate` | PASS |
| Stale generation snapshot rejected | `EditorNodeController.StaleGenerationSnapshotIsRejected` | PASS |
| Rail Loader lifecycle (History/Versions) still holds | `EditorHistoryVersionsRailLifecycleQmlTest` | 5/5 PASS |
| Versions panel after Nodes Component in the rail | `EditorVersionsPanelQmlTest` | 11/11 PASS |
| History transactions panel after Nodes Component | `EditorHistoryTransactionsPanelQmlTest` | 6/6 PASS |
| Adapter + delegate regression | `AlcedoQanGraphTest` + `EditorNodeDelegateQmlTest` | 9/9 + 9/9 PASS |
| Viewer stays ≥ 360 logical pixels at 960×640 | `WorkspaceShellTests.NodesPageKeepsViewerAtLeast360LogicalPixelsAt960x640` | PASS |
| Narrow window keeps left/center/right order | `WorkspaceShellTests.NarrowWindowKeepsSidePanelOrderAndMinViewport` | PASS |
| Nodes rail button hit/optical/accessible name | `WorkspaceShellTests.StructuralIconActionsExposeHitOpticalAndAccessibleNames` | PASS |
| History fold driver after Nodes page | `WorkspaceShellTests.HistoryFoldDriverPinsIntermediateAndTerminalGeometry` | PASS |
| Production Main.qml still loads after Nodes import | `MainQmlWorkflowTests.ProductionWindowLoadsAndRoutesCoreWorkspaceActions` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest EditorHistoryVersionsRailLifecycleQmlTest EditorVersionsPanelQmlTest EditorHistoryTransactionsPanelQmlTest AlcedoQanGraphTest EditorNodeDelegateQmlTest WorkspaceShellTest MainQmlWorkflowTest --parallel 4
build/debug/alcedo_studio/tests/ui/EditorNodeSelectionLayoutTest_runtime/EditorNodeSelectionLayoutTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorNodesPanelQmlTest_runtime/EditorNodesPanelQmlTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorHistoryVersionsRailLifecycleQmlTest_runtime/EditorHistoryVersionsRailLifecycleQmlTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorVersionsPanelQmlTest_runtime/EditorVersionsPanelQmlTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorHistoryTransactionsPanelQmlTest_runtime/EditorHistoryTransactionsPanelQmlTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/MainQmlWorkflowTest_runtime/MainQmlWorkflowTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/WorkspaceShellTest_runtime/WorkspaceShellTest.exe --gtest_filter=*NodesPageKeepsViewerAtLeast360LogicalPixelsAt960x640*:*NarrowWindowKeepsSidePanelOrderAndMinViewport*:*HistoryFoldDriverPinsIntermediateAndTerminalGeometry*:*StructuralIconActionsExposeHitOpticalAndAccessibleNames* --gtest_color=no
```

Suite totals: **11/11** `EditorNodeSelectionLayoutTest`, **8/8** `EditorNodesPanelQmlTest`,
**5/5** `EditorHistoryVersionsRailLifecycleQmlTest`, **11/11** `EditorVersionsPanelQmlTest`,
**6/6** `EditorHistoryTransactionsPanelQmlTest`, **9/9** `AlcedoQanGraphTest`,
**9/9** `EditorNodeDelegateQmlTest`, **1/1** `MainQmlWorkflowTest`,
**4/4** filtered `WorkspaceShellTest`. Direct runtime execution: **64/64** on those
cases. CTest discovery still hits the unrelated `SharedToneCurveTest` lensfun
entry-point mismatch; binaries were run from their `_runtime` directories.

**Checklist / exit condition:** all NM5.5 work and exit conditions are checked.
Add, Rename, Delete, and Reconnect are not exposed. The Add header button exists
and stays disabled.

**LOC note (grill-code-review):** new `EditorNodeLayoutStore` 172+242 lines;
`EditorNodeController` 145+310; `EditorNodesPanel.qml` 258; focused tests
88+132+200. Adapter source 732 and header 294 after selection/layout helpers.
`EditorWorkspaceRail.qml` 345 after the Nodes page and neutral object names.
`app_theme.cpp` remains above the 1,000-line review threshold (three geometry
token getters added; no responsibility split in this UI-state change). No new
source file exceeds 1,000 lines.

**Residual gaps:** NM5.6–NM5.8 still own Add/Rename/Delete, request-only
Reconnect, accessibility of the full page, and package checks. Backbone
keyboard methods are unit-tested; QML `Keys.onPressed` for Up/Down/Home/End
and Ctrl+0 is implemented and Fit is invoked from the panel, but there is no
QML keyClick assertion. Layout store keys include project id, but the panel
passes an empty project string because the session has no project-identity
field; element, image, and Version still separate layouts. The RAW-backed
`HistoryAndVersionsOpenSwitchAndCollapseFromLeftNavbar` case was not finished
here because `WaitForInteractiveImage` did not return on this GPU path;
History/Versions/Nodes exclusivity is proven by `EditorNodesPanelQmlTest`.
macOS checks were not run on this Windows host. The unrelated CTest discovery
runtime mismatch remains outside this sub-phase.

**Visual polish follow-up (2026-09-03):** node graph visuals were corrected
after NM5.5 review. Ports are hollow green-outlined squares
(`graphPortFillColor` transparent, `graphPortBorderColor` `#3FB950`, 1.5 px
stroke) on a new zero-margin Alcedo horizontal dock delegate
(`EditorNodePortDock.qml`, installed through `horizontalDockDelegate`), so
port squares sit flush against the node card edge. Backbone edges use
`graphEdgeWidth` 2. The earlier `selectionDelegate: null` note was wrong:
pinned QuickQanava 2.50 treats null as "reset to the default blue animated
`SelectionItem`". `AlcedoQanGraph::ConfigureGraphPolicy` now installs an
inline invisible `Item` as the selection delegate, so selection is only the
card outline swap. Node cards use the new visible `graphNodeBorderColor`
outline, and the Mask drawer sits in a sunken `graphMaskDrawerSurfaceColor`
well inset inside the card border with rounded bottom corners, so the drawer
no longer reads as a floating overlay. New coverage:
`AlcedoQanGraph.InstallsFlushPortDockAndInvisibleSelectionDelegate`,
`EditorNodeDelegateQml.PortSquareIsHollowGreenOutlineAcrossThemes`,
`EditorNodeDelegateQml.BackboneEdgeStrokeMatchesGraphEdgeTokens`,
`EditorNodeDelegateQml.MaskDrawerWellIsInsetInsideVisibleCardBorder`. Full
`ctest -L quickqanava`: 33/33 PASS.

**Visual polish follow-up 2 (2026-09-03):** three review findings fixed.
The Mask drawer header hover/focus wash now keeps a
`graphSelectionOutlineWidth` bottom inset and rounds its bottom corners to the
well radius whenever the header spans the whole drawer (closed, or open with no
Mask rows), so hovering no longer paints over the card's bottom border
(`EditorNodeMaskDrawer.qml` wash, objectName
`editorNodeMaskDrawerHeaderWash`). First-open edge/node misalignment is fixed:
QuickQanava edge items only recompute endpoints on port-local geometry changes,
and the first polish pass moves the zero-margin docks to their anchored
positions without any port-local change, leaving edges stale from the first
rendered frame. `EditorNodePortDock.qml` now forwards dock position changes to
`hostNodeItem.updatePortsEdges()` (the upstream VerticalDock #145 pattern);
this also keeps edges glued during Mask drawer folds. The canvas grid is
removed: the panel assigns `grid: null` on the `Qan.GraphView`, which swaps in
QuickQanava's empty default grid, and the unused `graphGridColor` token is
dropped from AppTheme and DESIGN.md. New coverage:
`EditorNodeDelegateQml.EdgeEndpointsStayGluedToPortsThroughFirstOpenBurstAndDrawerFold`
(verified to fail without the dock forwarding),
`EditorNodeDelegateQml.MaskDrawerHeaderWashKeepsCardBorderVisibleOnHover`,
`EditorNodesPanelQmlTest.GraphCanvasPaintsUniformBackgroundWithoutGrid`. Full
`ctest -L quickqanava`: 36/36 PASS.

---

## 13. NM5.6 — Add, Rename, and Delete

### 13.1 Input context

NM5.5 provides stable selection and lifecycle behavior. NM4 provides real typed graph history. NM5.2
provides the naming counter. This sub-phase connects only Add, Rename, and Delete.

Reconnect remains in NM5.7. Enable and Disable are not NM5 actions.

### 13.2 Official documentation

- [Graph](https://cneben.github.io/QuickQanava/graph.html): `Data Model`.
- [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html): `Adding content`, `Observing Topology`, and `Selection`.
- [Advanced use (C++)](https://cneben.github.io/QuickQanava/advanced.html): `Observation of Topological Modifications`.

These sections apply only to Qan projection updates. Alcedo commands own the product change.

### 13.3 Work

1. Connect the header Add action to `EditorNodeController`.
2. Generate a stable NodeId in the application layer.
3. Insert before DRT/Post, or after the selected Grade through the existing `before_node_id` rule.
4. Claim the next default name inside the accepted domain change.
5. Connect F2 and the node context menu to typed Rename.
6. Connect Delete and the context menu to typed Remove.
7. Do not put persistent action buttons on the node card.
8. Disable conflicting commands while a command is active.
9. Apply Qan changes only after a successful projection update.
10. Restore selection after deletion by successor, predecessor, then DRT/Post.
11. Publish exact errors without creating a substitute graph.

### 13.4 Main call chain

```text
Add, Rename, or Delete UI request
  -> EditorNodeController validates session, generation, and NodeId
  -> EditorSessionHistoryPort command
  -> domain change under the render lock
  -> typed batch and WAL
  -> history head publication
  -> node or topology projection revision
  -> AlcedoQanGraph update
  -> Quality render only when pixels or topology changed
```

Rename is metadata-only. It creates history but does not start a photo render.

### 13.5 Files

- `editor_node_controller.hpp/.cpp`
- `EditorNodesPanel.qml`
- `EditorNodeDelegate.qml`
- `EditorSessionHistoryPort`
- default-name counter and typed Add files from NM5.2
- session action availability
- history presentation

### 13.6 Tests and exit criteria

- Add creates one clean Grade with the next default name.
- Add creates one commit and one required Quality render.
- Rename creates one metadata commit and no render.
- Delete bridges the correct predecessor and successor.
- Delete creates one commit and one required Quality render.
- Develop and DRT/Post reject Rename and Delete.
- No visible Enable or Disable action exists.
- Invalid NodeId and stale generation requests fail.
- WAL failure restores document, history, counter, projection, and selection.
- A failed command creates no temporary permanent Qan node.
- Undo and Redo restore names, topology, Mask drawer content, and selection.

### 13.7 Completion record

Record the date, commands, test count, success chain, failure chain, and render reasons here.

##### NM5.6 completion record (2026-09-04)

**Status:** complete — the Nodes header and `Ctrl++` add a clean Color Grade after the selected
Grade or before DRT. F2 and the node context menu rename the selected Grade in place. Delete and
the node context menu remove the selected Grade and select its successor, predecessor, or DRT.
The UI exposes no Enable or Disable action and adds no persistent card actions. All three commands
use the session queue and NM4 typed history; Qan receives only the accepted document projection.

**Revision and branch:** base repository revision `2dd00168`, branch
`feature/nodes-panel-add-rename-delete`.

**Primary success call chains:**

```text
header Add or Ctrl++
  -> EditorNodeController::addCleanColorGrade
  -> verify the bound session, internal generation fence, and edit availability
  -> create grade.<QUuid> and resolve the next before_node_id
  -> EditorSessionController::SubmitAddColorGrade
  -> EditorSessionService queue admission as CommitAdjustment
  -> EditorSessionHistoryPort::AddColorGrade under the render lock
  -> typed Add batch and Mini-Git WAL append
  -> history publication and EditorNodeGraphProjection refresh
  -> AlcedoQanGraph applies the accepted topology
  -> select the new stable NodeId
  -> GraphTopologyChanged Quality render
```

```text
F2 or node-menu Rename
  -> EditorNodeController::renameColorGrade
  -> reject endpoints, missing ids, blank names, stale sessions, and blocked edits
  -> EditorSessionController::SubmitRenameColorGrade
  -> EditorSessionService queue admission as CommitAdjustment
  -> EditorSessionHistoryPort::RenameColorGrade
  -> typed Rename batch and Mini-Git WAL append
  -> metadata projection refresh with the same NodeId and selection
  -> no render reason and no photo render
```

```text
Delete key or node-menu Delete
  -> EditorNodeController::deleteColorGrade
  -> verify a live Color Grade and remember successor plus predecessor
  -> EditorSessionController::SubmitRemoveColorGrade
  -> EditorSessionService queue admission as CommitAdjustment
  -> EditorSessionHistoryPort::RemoveColorGrade
  -> typed Remove batch bridges the exact backbone neighbors and appends the WAL record
  -> accepted topology projection replaces the permanent Qan topology
  -> select successor, predecessor, or DRT; retain the removed NodeId for Undo selection restore
  -> GraphTopologyChanged Quality render
```

**Failure chain:**

```text
invalid endpoint, missing NodeId, stale internal generation, blocked action, or active command
  -> reject before history mutation

domain validation or Mini-Git WAL append failure
  -> NM4 restores document, head, default-name counter, and published render reason
  -> EditorSessionService returns the exact error without routing a render
  -> EditorNodeController keeps selection and the accepted projection
  -> AlcedoQanGraph creates no temporary permanent node or edge
  -> EditorNodesPanel shows the exact error
```

**Render reasons:** successful Add and Delete publish `GraphTopologyChanged`, which routes one
Quality render. Rename publishes no render reason and routes no photo render. Failed commands do
not change the prior published render reason and route no render.

**Tests and evidence:**

| Target or suite | Result |
| --- | --- |
| `EditorSessionNodeCommandTest` | 4/4 passed: queue/service routing, one history change, Add/Delete topology render, Rename no-render, and exact failure publication. |
| `EditorSessionActionPolicyCq3Test` | 12/12 passed: the new command kinds use settled-edit admission, and public QML/API generation tokens remain absent. |
| `EditorNodeSelectionLayoutTest` | 16/16 passed: stable Add placement/name, Rename identity, Delete fallback, Undo selection restore, endpoint/missing-id/stale-session rejection, and failure preservation. |
| `EditorNodesPanelQmlTest` | 14/14 passed: header Add, `Ctrl++`, F2, Delete, the shared right-click menu, no Enable action, exact error text, and unchanged permanent Qan projection on failure. |
| `AlcedoQanGraphTest` | 10/10 passed: live identity mapping, role-only Rename, topology replacement, stale-pointer rejection, and adapter rollback. |
| Filtered `EditorSessionHistoryPortTest` and `EditorDocumentHistoryTest` | 51/51 passed: real typed Add/Rename/Delete Undo/Redo, exact edges and Mask content, render intent, and real WAL-open failure restoration. |
| **Focused direct runtime execution** | **107/107 passed across six binaries.** |
| `ctest --test-dir build/debug -L quickqanava --output-on-failure` | **41/41 passed.** |

Build and test commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionActionPolicyCq3Test EditorSessionNodeCommandTest EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest EditorSessionHistoryPortTest AlcedoQanGraphTest alcedo_main
ctest --test-dir build/debug -L quickqanava --output-on-failure
```

The six focused binaries were also run directly from their generated `_runtime` directories. The
production `alcedo_main` target and the QML cache compilation completed successfully. The wrapper
printed its existing `vswhere.exe` lookup warning but found the configured MSVC environment and
completed each build.

**Pinned API differences:** the online Nodes documentation describes graph-level selection and
node-event observation but does not give the QML callback parameter list used here. The pinned
2.50 `qanGraphView.h` exposes `nodeRightClicked(qan::Node*, QPointF)`. The Nodes page therefore
handles pinned `onNodeRightClicked(node, position)`, resolves the `qan::Node*` through
`AlcedoQanGraph::liveNodeId`, and opens the shared menu only for a current mapped node. No pinned
source was changed.

**LOC note (grill-code-review):** the implementation adds 631 production lines and 554 test lines,
including the new 131-line focused service test. The largest touched production files were already
large orchestration files; the focused additions remain in their existing queue, controller, and
history responsibilities. `EditorNodeController` is 519 lines and `EditorNodesPanel.qml` is 467
lines after this phase. No new file exceeds the review threshold.

**Review result:** grill-code-review found no remaining high-confidence correctness, ownership,
performance, naming, fixture-fidelity, or maintainability blocker. Its verification pass caught
and closed queue-admission mapping, public generation-token exposure, duplicate projection refresh,
right-click coordinate routing, and formatter-churn issues before completion.

**Residual gaps:** NM5.7 still owns the request-only visual connector and Reconnect UI. NM5.8 still
owns final accessibility/localization/theme audits plus Windows install/package and available-macOS
qualification. Those later checks are not claimed here.

##### NM5.6 completion record (2026-09-04, history presentation and session availability)

**Status:** complete — typed Add/Rename/Delete history rows show saved Color Grade names instead of
the generic adjustment “Edit” title, and Nodes Add/Rename/Delete disable when session
`can_edit` flips through `ActionAvailabilityChanged` without a snapshot change. Nested Add while a
command is already active is rejected before a second backend mutation. Undo of Delete restores the
removed Grade’s Mask drawer content on the node projection.

This record covers the residual of 13.5 (`history presentation`, `session action availability`) and
the 13.6 Mask-restore and command-active criteria. The earlier 2026-09-04 record still covers the
header/`Ctrl++`/F2/Delete command path.

**Revision and branch:** base repository revision `2dd00168`, branch
`feature/nodes-panel-add-rename-delete`.

**Primary success call chain:**

```text
typed Add / Rename / Delete commit
  -> ProjectPipelineEditHistory
  -> EditorHistoryCommit.presentation_key + node_display_name
  -> PresentEditorHistoryCommit(commit)
  -> EditorHistoryModel displayName / deltaText
```

```text
header Add / Ctrl++ / F2 rename / Delete
  -> EditorNodeController
  -> EditorSessionController::Submit*
  -> EditorSessionService queue (CommitAdjustment admission)
  -> EditorSessionHistoryPort typed batch + WAL
  -> projection / AlcedoQanGraph after accept
  -> Add/Delete: GraphTopologyChanged Quality render; Rename: no render
```

**Availability call chain:**

```text
session ActionAvailabilityChanged (observer, no StateChanged)
  -> EditorNodeController::ActionAvailabilityChanged
  -> QML re-reads canAddColorGrade / canRenameColorGrade / canDeleteColorGrade
```

**Primary failure call chain:**

```text
blocked can_edit / command_active_ / WAL append failure
  -> reject before mutation, or restore document / counter / projection / selection
  -> exact error
  -> no temporary permanent Qan node
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `TypedGraphOperationsUseSavedNamesAndKeepAdjustmentRows` | `EditorSessionHistoryPortTest` | PASS |
| `AddRenameAndDeleteSnapshotsPresentTypedHistoryTitles` | `EditorSessionHistoryPortTest` (`EditorDocumentHistoryTest`) | PASS |
| `HistoryModelPresentsTypedAddColorGradeTitleAndName` | `EditorSessionControllerPhase5ATest` | PASS |
| `BlockedEditAvailabilityDisablesAddWithoutSnapshotChange` | `EditorNodeSelectionLayoutTest` | PASS |
| `ActiveCommandRejectsNestedAddBeforeBackendMutation` | `EditorNodeSelectionLayoutTest` | PASS |
| `DeleteSelectsSuccessorThenUndoProjectionRestoresDeletedSelection` (Mask restore) | `EditorNodeSelectionLayoutTest` | PASS |
| Add/Rename/Delete queue, render, exact WAL error | `EditorSessionNodeCommandTest` | PASS 4/4 |
| Settled-edit admission for graph command kinds | `EditorSessionActionPolicyCq3Test` | PASS 12/12 |
| Header Add, `Ctrl++`, F2, Delete, context menu, failure Qan | `EditorNodesPanelQmlTest` | PASS 14/14 |
| Role-only Rename, topology replacement, adapter rollback | `AlcedoQanGraphTest` | PASS 10/10 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest EditorNodeSelectionLayoutTest EditorSessionControllerPhase5ATest EditorNodesPanelQmlTest EditorSessionNodeCommandTest EditorSessionActionPolicyCq3Test AlcedoQanGraphTest
```

Ninja reported no outstanding work for those targets (they were already built in this tree). The
seven binaries were then run directly from their `_runtime` directories. Suite totals:
**174/174 passed** (72 + 18 + 44 + 4 + 12 + 14 + 10). Logs:
`build/tmp/nm5.6-residual/<binary>.log`. `ctest -L quickqanava` was not re-run in this residual
pass; the labeled Qan binaries above were executed directly instead.

**Checklist / exit condition:** 13.3 items 1–11 and 13.6 remain satisfied, including History titles
for typed graph operations, session action availability without a snapshot change, nested-command
rejection, and Undo restoring Mask drawer content on the node projection.

**LOC note (grill-code-review):** residual production is the commit-object
`PresentEditorHistoryCommit` overload (~80 lines in
`editor_history_commit_presentation.cpp`) plus `EditorHistoryModel::PresentationFor` using that
overload and `EditorNodeController` forwarding `ActionAvailabilityChanged`. Residual tests add the
six named cases above. `editor_history_commit_presentation.cpp` is 788 lines;
`editor_node_controller.cpp` is 473 lines. No new file exceeds the review split threshold.

**Remaining gaps:** NM5.7 still owns the request-only visual connector and Reconnect UI. NM5.8 still
owns final accessibility/localization/theme audits plus Windows install/package and available-macOS
qualification.

##### NM5.6 completion record (2026-09-04, live Qan apply on the open Nodes page)

**Status:** complete — Add, Rename, and Delete apply the published snapshot onto the bound
`AlcedoQanGraph` from `EditorNodeController` while the Nodes page stays open. The page binds
`graphAdapter` for the life of the Loader and clears it on destroy. Topology rebuilds re-parent
and show live node and edge items on the GraphView container, so the user does not close and
reopen the panel to see the accepted backbone.

**Revision and branch:** base repository revision `2dd00168`, branch
`feature/nodes-panel-add-rename-delete`.

**Primary success call chain:**

```text
Add / Rename / Delete
  -> EditorSessionService typed history accept
  -> EditorNodeController::refreshFromSession / PublishSnapshot
  -> QueueProjectionApply
  -> ApplyBoundGraph
  -> AlcedoQanGraph::ApplySnapshot
  -> AttachLiveVisuals on the GraphView container
```

**What was proven (executed tests):**

| Target or suite | Result |
| --- | --- |
| `EditorNodeSelectionLayoutTest` | 19/19 passed, including live adapter/layout binding and Undo selection restore. |
| `EditorNodesPanelQmlTest` | 15/15 passed, including open-page adapter bind, live Add Qan identity, and Delete successor cards remaining visible on the GraphView. |
| `AlcedoQanGraphTest` | 11/11 passed, including topology replacement and hidden removed cards. |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest AlcedoQanGraphTest alcedo_main
```

The three binaries were run directly from their `_runtime` directories after the live-apply
change. `alcedo_main` linked with the updated QML cache. macOS checks were not run on this
Windows host. NM5.7–NM5.8 remain outside this sub-phase.

---

## 14. NM5.7 — Request-only visual connector and Reconnect

### 14.1 Input context

NM5.6 connects all planned graph commands except Reconnect. The product graph still permits one
backbone. A visual connector can report a request, but it cannot create product topology.

### 14.2 Official documentation

- [Nodes/Groups](https://cneben.github.io/QuickQanava/nodes.html): `Docks and Ports`.
- [Edges](https://cneben.github.io/QuickQanava/edges.html): `Visual creation of edges`, `Visual Connectors`, and `Custom Connectors`.
- [Samples](https://cneben.github.io/QuickQanava/samples.html): the feature matrix entry for visual connectors.

The official Edges page requires `connectorCreateDefaultEdge = false` for request-only behavior.
It then reports `connectorRequestEdgeCreation(src, dst)`.

### 14.3 Work

1. Enable the documented visual connector.
2. Set `connectorCreateDefaultEdge` to false.
3. Let only a selected Color Grade start a move request.
4. Keep Develop and DRT/Post fixed in the backbone.
5. Resolve Qan source and target through generation-checked identity maps.
6. Remove the moving Grade from an in-memory order calculation only.
7. Resolve the target position in the remaining backbone.
8. Compute the new predecessor and successor.
9. Call NM4 `ReconnectColorGrade` through the controller.
10. Update permanent Qan edges only after projection publication.
11. Remove the preview and show exact error text after failure.

### 14.4 Main call chain

```text
user drags the official visual connector
  -> Qan shows a temporary connector
  -> connectorRequestEdgeCreation(src, dst)
  -> adapter resolves IDs and generation
  -> controller computes predecessor and successor
  -> EditorSessionHistoryPort::ReconnectColorGrade
  -> domain validation and typed history
  -> topology projection revision
  -> permanent Qan edge update
```

Failure path:

```text
invalid, stale, cyclic, fan-in, or fan-out request
  -> controller rejects the request
  -> no PipelineDocument change
  -> no history commit
  -> no permanent Qan edge
  -> temporary connector closes
  -> localized error text
```

### 14.5 Files and pinned interfaces

- `AlcedoQanGraph`
- `EditorNodeController`
- `EditorNodePortDelegate.qml`
- `EditorNodeEdgeDelegate.qml`, if required
- `EditorNodesPanel.qml`
- `EditorSessionHistoryPort::ReconnectColorGrade`

Verify the pinned forms of:

- port insertion and binding;
- `connectorEnabled`;
- `connectorCreateDefaultEdge`;
- `connectorRequestEdgeCreation`;
- connector source configuration;
- node connectable state.

### 14.6 Tests and exit criteria

- Develop has no incoming move target.
- DRT/Post has no outgoing move source.
- Each Grade has one input and one output.
- A valid move creates one commit.
- A no-op move creates no commit.
- Cycle, fan-in, and fan-out requests fail.
- A stale Qan primitive cannot change the current Version.
- Backend failure leaves the permanent edge set unchanged.
- Undo and Redo restore the exact order.
- Drawer height does not change port identity or reconnect results.

### 14.7 Completion record

##### NM5.7 completion record (2026-09-04)

**Status:** complete — the Nodes page enables the pinned QuickQanava visual connector in
request-only mode. Only the selected Color Grade can start a move. A drop is resolved through
generation-checked identity maps, the moving Grade is removed from an in-memory backbone order,
and NM4 `ReconnectColorGrade` runs with the computed predecessor and successor. Permanent Qan
edges update only after the accepted projection is published.

**Revision and branch:** base repository revision `dcd9dce8` (`origin/main` with NM5.6 merged),
branch `feature/nodes-panel-visual-connector-reconnect`.

**Pinned connector properties (QuickQanava 2.50 `56bdf78d`):**

| Property / signal | Pinned value used |
| --- | --- |
| `connectorEnabled` | `true` (pinned header default is `false`; website text says default `true`) |
| `connectorCreateDefaultEdge` | `false` |
| `connectorRequestEdgeCreation(src, dst, srcPort, dstPort)` | request-only path; official website omits the port arguments present in 2.50 |
| `connectorEdgeColor` | `AppTheme.graphCandidateEdgeColor` |
| `connectorColor` | `AppTheme.graphPortBorderColor` |
| selected Color Grade `connectable` | `OutConnectable`; connector `sourcePort` is the bottom `image` output |
| other Color Grades | `InConnectable` |
| Develop | `UnConnectable` (no incoming move target) |
| DRT/Post | `InConnectable` (no outgoing move source) |

**Primary success call chain:**

```text
user drags the official visual connector from the selected Color Grade
  -> Qan shows a temporary connector (no default insertEdge)
  -> connectorRequestEdgeCreation(src, dst, srcPort, dstPort)
  -> AlcedoQanGraph resolves live NodeIds and generation
  -> EditorNodeController removes the moving Grade from remaining backbone order
  -> compute predecessor and successor (insert before dest, or after an output port)
  -> EditorSessionController::SubmitReconnectColorGrade
  -> EditorSessionService queue admission as CommitAdjustment
  -> EditorSessionHistoryPort::ReconnectColorGrade under the render lock
  -> typed Reconnect batch and Mini-Git WAL append
  -> topology projection revision
  -> AlcedoQanGraph replaces permanent edges from the accepted snapshot
  -> GraphTopologyChanged Quality render
```

**Primary failure call chain:**

```text
Develop incoming target, DRT outgoing source, unselected source, self-cycle,
non-adjacent fan-in/fan-out, stale Qan primitive, stale generation, or backend failure
  -> reject before or during history mutation
  -> no PipelineDocument change
  -> no history commit
  -> hideConnectorPreview (temporary Qan edge closed)
  -> permanent Qan edge set unchanged
  -> EditorNodesPanel shows the exact error
```

A no-op drop onto the current successor returns success without a history commit.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Develop has no incoming move target; DRT has no outgoing source | `EditorNodeSelectionLayoutTest` | PASS |
| Each Grade has one input and one output; request-only connector; theme colors | `AlcedoQanGraphTest` | PASS |
| Valid move creates one commit and one topology Quality render | `EditorSessionNodeCommandTest`, `EditorNodeSelectionLayoutTest`, `EditorNodesPanelQmlTest` | PASS |
| No-op move creates no commit | `EditorNodeSelectionLayoutTest` | PASS |
| Cycle, fan-in, and fan-out requests fail | `EditorNodeSelectionLayoutTest` | PASS |
| Stale Qan primitive cannot change the current Version | `AlcedoQanGraphTest` | PASS |
| Backend failure leaves permanent edges and exact error | `EditorNodesPanelQmlTest` | PASS |
| Undo restores exact order and selection | `EditorNodeSelectionLayoutTest` | PASS |
| Drawer fold does not change port identity or reconnect neighbors | `AlcedoQanGraphTest`, `EditorNodesPanelQmlTest` | PASS |

| Target or suite | Result |
| --- | --- |
| `EditorSessionNodeCommandTest` | 5/5 passed |
| `EditorSessionActionPolicyCq3Test` | 12/12 passed |
| `EditorNodeSelectionLayoutTest` | 25/25 passed |
| `AlcedoQanGraphTest` | 15/15 passed |
| `EditorNodesPanelQmlTest` | 19/19 passed |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionNodeCommandTest EditorSessionActionPolicyCq3Test EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest AlcedoQanGraphTest alcedo_main
build\debug\alcedo_studio\tests\app\EditorSessionNodeCommandTest_runtime\EditorSessionNodeCommandTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionActionPolicyCq3Test_runtime\EditorSessionActionPolicyCq3Test.exe
build\debug\alcedo_studio\tests\ui\EditorNodeSelectionLayoutTest_runtime\EditorNodeSelectionLayoutTest.exe
build\debug\alcedo_studio\tests\ui\AlcedoQanGraphTest_runtime\AlcedoQanGraphTest.exe
build\debug\alcedo_studio\tests\ui\EditorNodesPanelQmlTest_runtime\EditorNodesPanelQmlTest.exe
```

`alcedo_main` linked. macOS checks were not run on this Windows host.

**Checklist / exit condition:** all boxes in 14.6 checked.

**LOC note (grill-code-review):** `editor_node_controller.cpp` 761; `alcedo_qan_graph.cpp` 942;
`EditorNodesPanel.qml` 455. `editor_session_service.cpp` 1547 and
`editor_session_controller.cpp` 1263 were already above 1000; this sub-phase only added the
Reconnect routing methods beside the existing Add/Remove/Rename paths and did not absorb more
business rules into those facades.

**Residual gaps:** NM5.7R supersedes this immediate-reconnect product behavior with incremental
draft topology editing and atomic automatic submission. NM5.8 still owns lifecycle,
accessibility, localization, install, and package checks. Keyboard topology editing is listed
under NM5.8. Real-RAW and three-backend qualification remain NM8.

---

## 15. NM5.7R — Incremental draft topology and atomic automatic submission

### 15.1 Supersession and objective

NM5.7 proved the pinned request-only QuickQanava connector and the existing atomic
`ReconnectColorGrade` path. Its completion record remains historical evidence. NM5.7R supersedes
the NM5.7 product behavior that interprets every connector drop as an immediately valid backbone
move with automatically computed predecessor and successor.

NM5.7R makes the Nodes page a real topology editor:

- Add creates one disconnected draft Color Grade below the main vertical DAG.
- Connect changes only the requested source-output and destination-input relationship.
- A supported edit may temporarily break the Develop-to-DRT path or leave Color Grades detached.
- An unsupported edit is rejected before any draft state changes.
- No draft operation changes `PipelineDocument`, history, Version state, or rendering while the
  draft is incomplete.
- The first accepted operation that produces a complete supported graph automatically submits the
  accumulated change.
- The UI has no Apply or Cancel action.
- The product graph receives one atomic topology-delta instruction. It is never replaced with a
  draft copy.
- The canvas is created from a complete projection on initial load, and the draft is initialized
  from that projection once at the first topology edit. Later ordinary operations update only
  affected nodes, ports, edges, indexes, and presentation values.

### 15.2 Fixed NM5.7R invariants

- `PipelineDocument` remains the only writable product graph.
- `EditorNodeGraphDraft` is UI editing state. It is not serialized product data and is not a render
  input.
- Creating a draft reads the current complete projection once and binds its project, image,
  Version, session generation, projection revision, and topology revision.
- Add, Delete, and Connect mutate the same draft object incrementally. They do not reconstruct it
  from a snapshot after each operation.
- Ordinary draft changes do not call full-graph `AlcedoQanGraph::ApplySnapshot` and do not replace
  the Qan graph.
- A rejected operation changes no draft node, edge, index, accumulated delta, layout value, or Qan
  primitive.
- An incomplete but structurally supported draft remains visible and produces no history commit,
  WAL append, topology publication, or photo-render request.
- A complete changed draft submits automatically. A complete draft whose accumulated delta is
  empty ends node editing without a commit or render.
- One automatic submission contains one `NodeGraphTopologyChange`, produces at most one history
  commit, and requests exactly one `GraphTopologyChanged` Quality render after success.
- The live `PipelineGraph` applies the change in place under the render lock. Unchanged
  `INodeModel` object identities remain stable.
- Forward failure and inverse replay failure restore exact node ownership, node order, edge order,
  next-name counter, history head, and topology revision.
- QuickQanava remains a view and input reporter. It never decides product validity or inserts a
  permanent edge from a connector drop.
- No degraded graph, automatic bridge, alternate backend, or other substitute is created after an
  error.

### 15.3 Draft ownership and incremental indexes

Add a focused `EditorNodeGraphDraft` application/UI-backend type rather than adding draft mutation
rules to QML or expanding `EditorNodeController` with graph-container responsibilities.

The draft owns copied value data and incremental lookup indexes such as:

```text
base identity
  project, element, image, Version
  session generation
  projection revision
  topology revision

current draft values
  nodes by NodeId
  edges by stable edge key
  one edge by source output port
  one edge by destination input port
  adjacency by NodeId

net product delta
  inserted nodes
  removed nodes
  disconnected base edges
  connected draft edges
  before and after next-name counter
```

The controller creates this object lazily on the first Add, Delete, or Connect request. Initial
construction may copy the complete immutable product projection. After construction, the object
survives every ordinary edit for the same base identity.

The accumulated delta is updated during each operation. Do not recalculate it by comparing two
complete graphs after every operation:

- removing an edge added by the draft cancels that connected-edge entry;
- adding an edge removed from the base cancels that disconnected-edge entry;
- adding and then deleting an uncommitted node cancels its inserted-node entry and all incident
  draft-only edges;
- restoring a removed base node and its exact base edges cancels its removal entries;
- when every entry is canceled, the draft equals the base and node editing ends automatically.

Full graph traversal is permitted for read-only validity checks. It must not allocate a replacement
draft, replace the current containers, or republish a complete Qan topology.

### 15.4 Incremental Add and default placement

The Nodes header keeps its existing Add action and gains no Apply or Cancel action.

Add performs only these draft changes:

1. Create the draft once if none exists.
2. Allocate one stable provisional NodeId.
3. Create one clean draft Color Grade value.
4. Add the node to the inserted-node accumulator.
5. Assign a deterministic position below the main vertical DAG at the backbone origin x. Stack
   multiple unconnected new nodes downward without moving existing nodes.
6. Incrementally insert one Qan node and its input/output ports.
7. Select the new draft node in the Nodes page.
8. Run the read-only submission-validity check.

Add does not choose an insertion point and does not create an edge. It does not call
`SubmitAddColorGrade`, consume the product counter, write history, or request rendering.

Draft names preview the names that the atomic product change will store. A draft that returns to
the base without submission consumes no name number. The final atomic change stores explicit
before and after counter values so success, failure, Undo, Redo, recovery, and Version checkout are
deterministic.

### 15.5 Incremental exclusive-port Connect

Every supported Color Grade image port can participate in editing; starting a connection is not
limited to the selected Color Grade. Develop exposes only its output role. DRT/Post exposes only
its input role. QuickQanava remains in request-only mode with
`connectorCreateDefaultEdge = false`.

One accepted request from `source.output` to `destination.input` has this exact behavior:

```text
oldOutgoing = the current edge from source.output, if present
oldIncoming = the current edge into destination.input, if present

remove oldOutgoing
remove oldIncoming when it is a different edge
add source.output -> destination.input
```

This is exclusive-port replacement, not a request to move a whole Color Grade. It does not infer a
new successor, infer a new predecessor, bridge the nodes left behind, or restore a complete
backbone automatically.

For an initial `A -> B -> C` graph with a detached new `D`:

```text
Connect A -> D
  disconnect A -> B
  connect A -> D
  visible draft: A -> D and B -> C
  result: incomplete; no submission and no render

Connect D -> C
  disconnect B -> C
  connect D -> C
  visible draft: A -> D -> C and detached B
  result: incomplete; no submission and no render
```

The user must reconnect B into the unique path or delete B from the draft. The operation that first
makes the full graph valid triggers automatic submission.

Each accepted Connect updates only:

- at most two removed draft edges;
- one inserted draft edge;
- the affected source-output and destination-input indexes;
- the affected adjacency entries;
- the accumulated delta;
- the corresponding Qan edge primitives and their presentation.

Unchanged draft and Qan node, port, and edge identities remain stable.

### 15.6 Operation admission and submission validity

Use two different checks. Do not reject an incomplete editing state as though it were an
unsupported connection.

#### 15.6.1 Operation admission

Before mutating the draft, validate the small candidate edit against the current indexes. Check:

- current base identity and session generation;
- live source and destination identities;
- output-to-input direction;
- matching port data types;
- supported source and destination node types;
- supported runtime/compiler connection semantics;
- Develop cannot be an input target;
- DRT/Post cannot be an output source;
- source and destination are distinct;
- the candidate edge, after ignoring the edges it would replace, does not create a cycle.

Cycle detection may traverse the current adjacency index while excluding the at-most-two edges
scheduled for removal. It must not copy or temporarily overwrite the draft.

When admission fails:

```text
reject before mutation
  -> draft values and indexes unchanged
  -> accumulated delta unchanged
  -> Qan primitives unchanged
  -> connector preview closes
  -> EditorNodesPanel shows the exact error
```

An unknown or unsupported node type must name the unsupported type or connection in the displayed
error. Do not collapse it into a generic Reconnect failure.

#### 15.6.2 Submission validity

After an admitted incremental operation, perform a read-only full check. A draft can submit only
when it has:

- exactly one Develop and one DRT/Post;
- one unique Develop-to-DRT scene-image path;
- no cycle, scene-image fan-in, or scene-image fan-out;
- exactly one source for every required input;
- every Color Grade on the unique image path;
- only runtime/compiler-supported node types on the path;
- valid port direction and data-type pairs.

Missing required input, a broken Develop-to-DRT path, detached Color Grades, and multiple detached
components are permitted draft states. They disable submission but do not undo an admitted edit.

### 15.7 One atomic product topology change

Add one stored change kind and one matching operation kind with a precise name such as
`NodeGraphTopologyChange` and `EditNodeGraph`. One automatic submission contains exactly one of
these stored changes; it is not a list of Add, Remove, and Reconnect commands.

The stored change carries only the net delta from its bound base graph:

```text
inserted nodes
  complete node JSON
  final node index

removed nodes
  complete node JSON
  original node index

disconnected edges
  exact endpoint and port IDs
  original edge index

connected edges
  exact endpoint and port IDs
  final edge index

before and after next Color Grade name number
```

The live submission request separately carries the expected session generation and topology
revision. These transient guards are not persisted as replay data.

Add a single domain entry point such as `PipelineGraph::ApplyTopologyDelta`. It must:

1. Require that every expected removed node and disconnected edge exactly matches the live graph.
2. Require that inserted NodeIds and connected edge identities do not conflict.
3. Reserve all forward and restoration storage before mutation.
4. Retain removed `unique_ptr<INodeModel>` objects and exact indexes.
5. Retain removed edges and exact indexes.
6. Apply all removals and insertions in place to the same live `PipelineGraph` object.
7. Set the after-name counter.
8. Validate the complete final graph once, after the whole delta is present.
9. Commit on success.
10. On any validation error or exception, restore original node ownership, node order, edge order,
    counter, and observable revision before returning the real error.

Do not assign a draft `PipelineDocument` or draft `PipelineGraph` over the live object. Do not call
the existing single-node Add/Remove/Reconnect commands sequentially. The live graph must never
expose an intermediate partial delta outside the render lock.

The current general history applier clones `PipelineDocument` as a broad restoration guard. That
is not the application mechanism for `NodeGraphTopologyChange`. Route this one stored change
through its self-restoring atomic entry point without `ClonePipelineDocument` or assignment from a
copied document. The atomic entry point must either complete or restore the same live objects
before it returns. A history-publication failure applies the stored inverse through the same atomic
entry point.

Forward history application uses the stored direction. Undo applies the exact inverse through the
same atomic entry point. Redo applies the forward direction again. WAL failure, history-publication
failure, live-pipeline publication failure, and recovery retain the existing NM4 restoration
requirements.

### 15.8 Automatic submission and canvas promotion

After every admitted Add, Delete, or Connect:

```text
incrementally mutate the existing draft
  -> incrementally update affected Qan primitives
  -> run read-only submission validation
  -> invalid: stay in node editing; no history and no render
  -> valid with empty delta: discard draft metadata and return to committed state
  -> valid with non-empty delta:
       EditorNodeController::SubmitNodeGraphTopologyEdit
       -> generation and topology-revision guard
       -> EditorSessionService queue admission
       -> render lock
       -> one NodeGraphTopologyChange applied in place
       -> one typed history batch and WAL append
       -> one topology publication
       -> promote the current canvas state to committed state
       -> one GraphTopologyChanged Quality render
```

Successful submission must not rebuild the already-correct Qan topology. Update its applied
revision metadata, clear the draft delta, and change affected edge presentation from candidate to
permanent. Keep unaffected Qan objects and all stored node positions.

The full product projection can rebuild the canvas only for initial load, image change, Version
change, Undo, Redo, recovery, session-generation replacement, adapter recreation, or an explicit
stale-state resynchronization. Ordinary draft edits and successful automatic submission do not use
that path.

### 15.9 Error and lifecycle behavior

- An unsupported candidate operation keeps the draft byte-for-byte and identity-for-identity
  unchanged and displays the exact error.
- If an incremental Qan insertion or removal fails, reverse that operation's affected draft values,
  indexes, accumulated delta, and Qan primitives. Keep the draft at its exact pre-operation state,
  publish no product command, and show the real adapter error.
- An incomplete draft may show one localized plain-text instruction to complete the Develop-to-DRT
  path. Do not add a pill, badge, status dot, or separate status chrome.
- If atomic product submission fails, restore the live graph and history, retain the current valid
  draft and its delta, request no render, and show the exact error.
- Do not continuously retry a failed submission. The next admitted draft edit performs the next
  validity check and may submit the resulting delta.
- Closing and reopening the Nodes Loader for the same bound base identity restores the same draft
  values. No Qan pointer survives Loader destruction.
- An image, Version, session-generation, or external topology change invalidates the old draft. It
  cannot submit against the new graph. Discard its non-product state during the identity change and
  create a new draft only after the next topology edit.
- Escape cancels only an active connector preview or rename input. It does not provide a hidden
  whole-draft Cancel action.
- Restoring the base topology manually reduces the accumulated delta to empty and exits node
  editing without a history commit or render.

### 15.10 Files and responsibilities

Expected implementation areas:

- `EditorNodeGraphDraft`: incremental nodes, edges, port indexes, adjacency, validation, and net
  delta.
- `EditorNodeController`: draft lifetime, automatic-submission state machine, selection, errors,
  and session guards.
- `AlcedoQanGraph`: incremental insert/remove node and edge entry points, live identity checks, and
  promotion from candidate to permanent presentation.
- `EditorNodesPanel.qml`: no Apply/Cancel controls; plain editing guidance and exact errors.
- `EditorNodeLayoutStore`: deterministic positions below the vertical DAG without moving stored
  backbone positions.
- `PipelineEditBatch`: the one new operation and stored change discriminator, canonical JSON,
  validation, and history projection.
- `PipelineGraph` and the history applier: one in-place atomic topology-delta application in both
  directions.
- editor session service/controller/history port: one automatic-submission route and one render
  reason after successful publication.

Keep QML free of topology mutation and validity rules. Keep the draft type free of QObject/Qan
ownership. Keep the atomic domain change free of layout, selection, and canvas presentation data.

### 15.11 Tests and exit criteria

#### Draft and controller

- The first topology edit creates one draft from the complete current projection.
- Later ordinary operations keep the same draft object and incrementally mutate only affected
  values and indexes.
- Add inserts one disconnected clean Color Grade below the main vertical DAG and produces no
  product command or render while the draft is incomplete.
- Add does not move existing node positions.
- `A -> D` replaces `A -> B` and preserves `B -> C` without submitting.
- `D -> C` replaces `B -> C`, leaves B detached, and does not submit.
- Reconnecting or deleting B so that the full path becomes valid automatically submits once.
- An unsupported type, wrong port direction, type mismatch, endpoint violation, self-edge, or cycle
  leaves the draft and delta unchanged and shows the exact error.
- Failure of an affected Qan node or edge insertion restores the exact pre-operation draft, delta,
  indexes, and Qan primitives without a product command.
- Returning the draft to the base empties the delta and exits editing without commit or render.
- An incomplete draft survives Nodes Loader destruction and recreation for the same base identity.
- A stale generation or topology revision cannot submit the draft to a newer document.

#### Qan adapter

- An accepted connection removes at most two affected edge primitives and inserts one.
- Unaffected Qan node, port, and edge QObject identities remain unchanged across Add, Connect,
  Delete, and successful automatic submission.
- Rejected operations change no Qan primitive.
- Ordinary draft operations and successful automatic submission do not invoke full topology
  replacement.
- Successful submission promotes candidate presentation without recreating the topology.

#### Atomic domain and history

- One valid draft invokes one `SubmitNodeGraphTopologyEdit` request.
- One request persists one `NodeGraphTopologyChange`, not a sequence of stored Add/Remove/Reconnect
  changes.
- The live `PipelineGraph` object and all unaffected `INodeModel` object addresses remain stable.
- The final graph is validated only after the whole delta is applied.
- Failure injection after each removal, insertion, counter update, WAL step, history-publication
  step, and live-pipeline publication step restores exact state.
- Forward, Undo, Redo, recovery, reopen, and Version checkout reproduce exact nodes, object data,
  node order, edges, edge order, and next-name counter.
- An incomplete draft produces zero commits and zero render requests across any number of edits.
- Successful automatic submission produces one history commit and one
  `GraphTopologyChanged` Quality render.

#### UI and performance

- The header contains no Apply or Cancel action.
- The incomplete-state instruction and exact error use existing AppTheme text and danger roles.
- No new badge, pill, status dot, gradient, shadow, glow, or Material style is introduced.
- A 32-Grade graph operation cost scales with affected indexes and the required read-only validity
  traversal; it does not include full draft or Qan reconstruction.
- Drawer folding and node movement preserve port identity and do not affect the accumulated delta.

### 15.12 Completion record

Record the implementation revision, branch, exact atomic-change format, primary success and
failure call chains, test commands and counts, failure-injection evidence, object-identity evidence,
Qan incremental-update evidence, Windows build result, and unavailable macOS checks here.

##### NM5.7R completion record (2026-09-04)

**Status:** complete — the Nodes page is an incremental topology editor. Add inserts one
disconnected draft Color Grade. Connect replaces only the source output edge and the destination
input edge. Incomplete supported graphs stay editable with no product write, history commit, or
render. The first admitted operation that restores a complete Develop-to-DRT path submits one
`NodeGraphTopologyChange`. The live `PipelineGraph` applies that delta in place. The UI has no
Apply or Cancel action.

**Revision and branch:** working tree on `feature/nodes-panel-visual-connector-reconnect`
(base `9a7a6625`). QuickQanava remains submodule tag `2.50` at
`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`.

**Atomic-change format:** typed batch `operation_kind = edit_node_graph` with exactly one
`node_graph_topology_change`. Stored fields: inserted nodes (canonical Color Grade JSON +
`final_node_index`), removed nodes (JSON + `original_node_index`), disconnected edges (exact
endpoints + `original_edge_index`), connected edges (exact endpoints + `final_edge_index`),
and before/after `next_color_grade_name_number`. Session generation and topology revision are
transient submit guards and are not persisted. Batch format version remains `2`; unknown kinds
still fail closed. No old-format migration was added.

**Primary success call chain:**

```text
Add or exclusive-port Connect or Delete
  -> EditorNodeController creates EditorNodeGraphDraft from the live document once
  -> incremental draft mutation (indexes and net delta updated in place)
  -> AlcedoQanGraph inserts/removes only affected primitives
  -> read-only submission validity
  -> incomplete: stay in node editing; no history; no render
  -> valid empty delta: discard draft; no history; no render
  -> valid non-empty delta:
       EditorNodeController::MaybeSubmitDraft
       -> EditorSessionController::SubmitNodeGraphTopologyEdit
       -> EditorSessionService queue admission as CommitAdjustment
       -> EditorSessionHistoryPort::EditNodeGraph under the render lock
       -> PipelineGraph::ApplyTopologyDelta in place on the live graph
       -> one typed EditNodeGraph batch and Mini-Git WAL append
       -> PromoteCommittedSnapshot (no Qan topology replacement)
       -> one GraphTopologyChanged Quality render
```

**Primary failure call chain:**

```text
unsupported port role, self-edge, cycle, unknown endpoint, or stale generation
  -> reject before draft mutation
  -> draft values, indexes, delta, layout, and Qan primitives unchanged
  -> connector preview closes
  -> EditorNodesPanel shows the exact error
```

```text
Qan insert/remove failure
  -> restore the pre-operation draft
  -> reverse only the affected Qan primitives
  -> no product command

atomic submit / WAL failure
  -> ApplyNodeGraphTopologyChange inverse restores live node objects, order, edges, and counter
  -> retain the valid draft and its delta
  -> no render
  -> exact error text
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| First construction copies the document once; later ops keep the same draft object | `EditorNodeGraphDraftTest` | PASS |
| Add inserts one disconnected Color Grade and does not consume the product counter | `EditorNodeGraphDraftTest`, `EditorNodeController`, `EditorNodesPanelQmlTest` | PASS |
| `A -> D` then `D -> C` leaves the skipped Grade detached without submitting | `EditorNodeGraphDraftTest`, `EditorNodeController` | PASS |
| Reconnecting or deleting the detached Grade makes the path valid | `EditorNodeGraphDraftTest` | PASS |
| Completing the path submits one `NodeGraphTopologyChange` and one topology Quality render | `EditorNodeController`, `EditorSessionNodeCommandTest`, `EditorNodesPanelQmlTest` | PASS |
| Unsupported connect leaves draft and delta unchanged with exact error | `EditorNodeGraphDraftTest`, `EditorNodeController` | PASS |
| Returning to the base empties the delta and exits without a commit | `EditorNodeGraphDraftTest`, `EditorNodeController` | PASS |
| In-place forward/inverse keeps unaffected `INodeModel` addresses | `PipelineGraphTopologyDeltaTest` | PASS |
| Failure after counter, disconnect, insert, connect, and validate restores exact state | `PipelineGraphTopologyDeltaTest` | PASS |
| Typed batch apply does not clone the live graph | `PipelineGraphTopologyDeltaTest` | PASS |
| Incremental Qan insert/remove preserves unaffected QObject identities | `AlcedoQanGraphTest` | PASS |
| Ordinary draft connect does not increment topology replacement | `EditorNodesPanelQmlTest` | PASS |
| Header has no Apply or Cancel action | `EditorNodesPanelQmlTest` | PASS |
| Submit failure keeps the product graph, shows the exact error, and retains the draft | `EditorNodesPanelQmlTest` | PASS |

| Target or suite | Result |
| --- | --- |
| `EditorNodeGraphDraftTest` | 8/8 passed |
| `PipelineGraphTopologyDeltaTest` | 5/5 passed |
| `PipelineEditBatchTest` | 17/17 passed |
| `PipelineHistoryApplierTest` | 12/12 passed |
| `EditorSessionNodeCommandTest` | 6/6 passed |
| `EditorSessionActionPolicyCq3Test` | 12/12 passed |
| `EditorNodeSelectionLayoutTest` | 24/24 passed |
| `AlcedoQanGraphTest` | 16/16 passed |
| `EditorNodesPanelQmlTest` | 19/19 passed |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorNodeGraphDraftTest PipelineGraphTopologyDeltaTest PipelineEditBatchTest PipelineHistoryApplierTest EditorSessionNodeCommandTest EditorSessionActionPolicyCq3Test EditorNodeSelectionLayoutTest AlcedoQanGraphTest EditorNodesPanelQmlTest alcedo_main
build\debug\alcedo_studio\tests\app\EditorNodeGraphDraftTest_runtime\EditorNodeGraphDraftTest.exe
build\debug\alcedo_studio\tests\app\PipelineGraphTopologyDeltaTest_runtime\PipelineGraphTopologyDeltaTest.exe
build\debug\alcedo_studio\tests\edit\PipelineEditBatchTest_runtime\PipelineEditBatchTest.exe
build\debug\alcedo_studio\tests\app\PipelineHistoryApplierTest_runtime\PipelineHistoryApplierTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionNodeCommandTest_runtime\EditorSessionNodeCommandTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionActionPolicyCq3Test_runtime\EditorSessionActionPolicyCq3Test.exe
build\debug\alcedo_studio\tests\ui\EditorNodeSelectionLayoutTest_runtime\EditorNodeSelectionLayoutTest.exe
build\debug\alcedo_studio\tests\ui\AlcedoQanGraphTest_runtime\AlcedoQanGraphTest.exe
build\debug\alcedo_studio\tests\ui\EditorNodesPanelQmlTest_runtime\EditorNodesPanelQmlTest.exe
```

`alcedo_main` linked. macOS checks were not run on this Windows host.

**Pinned API differences:**

- Request-only connector is unchanged: `connectorEnabled = true`,
  `connectorCreateDefaultEdge = false`. The pinned 2.50
  `connectorRequestEdgeCreation(src, dst, srcPort, dstPort)` still includes port
  arguments omitted by the website.
- Connectable policy now matches exclusive-port editing: Develop is
  `OutConnectable`, every Color Grade is `Connectable`, DRT/Post is
  `InConnectable`. Starting a connection is not limited to the selected Color Grade.
- Incremental `insertEdge` + `bindEdge` on a live port with
  `Multiplicity::Single` rejects the bind while the removed edge item remains in
  the port list. Incremental edge removal therefore clears that port's in/out
  edge items before `removeEdge`.

**Checklist / exit condition:** all 15.11 draft, Qan, atomic-domain, and UI
criteria in this sub-phase are covered by the executed tests above. Keyboard
topology editing, accessibility matrix, install/package, and licence packaging
remain NM5.8.

**LOC note (grill-code-review):** `editor_node_graph_draft.cpp` 555;
`editor_node_controller.cpp` 903; `EditorNodesPanel.qml` 509.
`alcedo_qan_graph.cpp` is 1244 after incremental insert/remove/promote; the extra
size is the incremental primitive API, not a second topology owner.
`pipeline_edit_batch.cpp` remains 1486 as the typed-change serializer; this
sub-phase added one change kind beside the existing variants and did not absorb
new unrelated responsibilities.

**Residual gaps:** NM5.8 still owns lifecycle, accessibility, localization
expansion, install, and package checks. Dedicated Mini-Git recovery/reopen/
Version-checkout cases for `EditNodeGraph` use the same
`ApplyPipelineEditBatch` inverse path proven above and were not duplicated as a
separate history-port suite. Keyboard topology editing remains NM5.8. Real-RAW
and three-backend qualification remain NM8.

##### Follow-up (2026-09-04): default Add placement

Unconnected Add nodes use the backbone origin x and sit below the lowest existing
card plus the vertical gap. Later unconnected Adds stack downward. They no longer
use a right-side lane. Proven by `EditorNodeLayoutStore` and `EditorNodeController`
placement tests in `EditorNodeSelectionLayoutTest`.

---

## 16. NM5.8 — Code simplification and final Nodes-page checks

### 16.1 Input context

NM5.1–NM5.7 provide the original Nodes page. NM5.7R replaces the immediate-reconnect product
behavior with incremental draft editing and atomic automatic submission. The
[2026-09-04 implementation review](node_panel_simplification_review.md) found duplicated
QML/controller update paths, whole-draft rollback copies, repeated index construction, and obsolete
command entry candidates. NM5.8 now removes that maintenance cost before closing lifecycle,
accessibility, localization, build, and package gaps. Existing product behavior remains required.

Earlier completion records are historical execution records. A passing object-address assertion
does not prove bounded copying, and direct inverse application does not prove product reopen or
Version checkout. The evidence gaps below remain open even where NM5.7R previously marked them
complete.

NM8 still owns final real-RAW and three-backend qualification.

### 16.2 Official documentation

- [Installation](https://cneben.github.io/QuickQanava/installation.html): all sections.
- [Graph](https://cneben.github.io/QuickQanava/graph.html): `QuickQanava Initialization` and `Graph View`.
- [API Reference](https://cneben.github.io/QuickQanava/reference.html): the local-Doxygen notice.
- [Licence](https://cneben.github.io/QuickQanava/licence.html): source and binary notice requirements.

### 16.3 Work

Execute NM5.8a through NM5.8g in order. Each stage must leave a usable, tested Nodes page.
Use responsibility boundaries for commits; approximately 500 changed lines is a review target,
not a reason to leave half a state transition in another commit. Do not introduce generic command
frameworks, a second product graph, a second selection owner, or a new rendering backend path.

#### 16.3.1 NM5.8a — One projection, selection, and layout update path

**Files:** `EditorNodesPanel.qml`, `editor_node_controller.{hpp,cpp}`,
`editor_node_layout_store.{hpp,cpp}`, `editor_node_controller_test.cpp`,
`editor_node_layout_store_test.cpp`, and `editor_nodes_panel_qml_test.cpp`.
Existing files live under the source/test directories listed in the review.

- Keep committed projection identity, revisions, selection, command availability, and the queued
  projection update in `EditorNodeController`. Keep per-image/Version positions, zoom, view,
  saved selection, and drawer values in `EditorNodeLayoutStore`; saved selection is restoration
  data, not a second live selection source.
- Keep GraphView navigation and rename focus/text in QML. The controller owns applying node
  positions, drawers, and live selection to the adapter. QML restores only GraphView zoom/view.
- Replace the combination of `Binding`, `bindControllerAdapter`, `bindGraph`, `GraphChanged`,
  `SnapshotChanged`, and completion-time rebinding with one attach/detach route. Remove the QML
  node traversal duplicated by `ApplyBoundGraph`; preserve layout-key activation before restore.
- Consolidate committed revision publication used by `PublishSnapshot` and `MaybeSubmitDraft`.
  Keep draft changes on the incremental route. Do not turn each mutation into `ApplySnapshot`.
- Audit `ContainsNode`, `IndexOf`, and `NodeFor` for one lookup implementation. Do not introduce
  another index unless a measured use requires it.
- Give deferred updates an explicit lifetime/identity check. Replace the unqualified
  `skip_next_session_refresh_` suppression with completion matching if event-order tests show
  it is needed; do not suppress unrelated image/Version notifications.

**Acceptance:** extend `EditorNodeSelectionLayoutTest` and `EditorNodesPanelQmlTest` to assert one
logical projection apply per committed revision, no replacement for ordinary draft edits, one
live selection, layout-key ordering, and close/reopen with an update already queued. Existing
`ReopenRestoresPositionsViewZoomSelectionAndDrawerState` and
`TwoVersionsKeepSeparateLayoutValues` remain required. Count apply requests as well as topology
replacements; identity retention alone cannot detect duplicate role/layout work.

##### NM5.8a completion record (2026-09-04)

**Status:** complete — one attach/apply route, live selection on the controller, layout-key restore,
queued apply lifetime, image/Version identity not swallowed by submit echo; workspace-switch
projection regression corrected 2026-09-04

**Workspace-switch regression correction:** NM5.8a initially connected both `StateChanged` and
`HistoryChanged` to the same refresh route. Each identity check called `history_snapshot()`, which
walks Version refs and the active first-parent commit path and formats every commit row on the GUI
thread. Ordinary workspace/session notifications could therefore perform multiple full history
projections, rebuild an unchanged Nodes value snapshot, and queue another Qan apply.

The corrected route keeps the immutable Nodes snapshot only as the published boundary:

```text
StateChanged
  -> compare element/image/session generation only
  -> unchanged: no history read, no Nodes projection, no Qan work

HistoryChanged
  -> compare monotonic history revision
  -> read active Version identity through ReadActiveVersionId (no ref/commit walk)
  -> build a candidate Nodes projection only when the history revision changed
  -> nodes/edges unchanged: keep the current projection revision and do not queue Qan
  -> nodes/edges changed: publish once and queue one apply
```

`history_snapshot()` remains the History/Versions model API; it is no longer used as a session
identity lookup by `EditorNodeController`. Repeated render, presentation, and workspace state
notifications do not enter history projection or node publication.

**Primary success call chain:**

```text
Nodes Loader creates EditorNodesPanel
  -> attachAdapter / set_graph_adapter
  -> ApplyBoundGraph (immediate if the Qan graph exists, else queued on GraphChanged)
  -> SyncLayoutKey then applyToGraph
  -> EnsureDefaultPositions, node positions, drawers, ApplyProductSelection
  -> QML restoreGraphView (zoom and pan only)
```

Ordinary draft edits stay on the incremental route:

```text
addCleanColorGrade / deleteColorGrade / requestConnect
  -> EditorNodeGraphDraft mutation
  -> ApplyMutationToGraph (insert/remove projected primitives)
  -> ActiveNodes/ActiveEdges read the draft
  -> snapshot_ stays the last committed projection
```

**Primary failure call chain:**

```text
PublishSnapshot queues ApplyBoundGraph, then the page unloads
  -> set_graph_adapter(null) increments attach generation
  -> ApplyBoundGraphIfCurrent sees a stale generation or null adapter
  -> skipped_stale_projection_apply_count, no ApplySnapshot
```

```text
session image/Version/generation differs from the cached Nodes identity
  -> SessionIdentityChanged
  -> discard draft, refreshFromSession
  -> submitted-echo skip does not apply
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OpenPageAppliesCommittedProjectionOnce` | `EditorNodesPanelQmlTest` | PASS |
| `OrdinaryDraftEditsDoNotReplaceQanTopology` | `EditorNodesPanelQmlTest` | PASS |
| `CloseWithQueuedApplyThenReopenRestoresOnce` | `EditorNodesPanelQmlTest` | PASS |
| `ReopenRestoresPositionsViewZoomSelectionAndDrawerState` | `EditorNodesPanelQmlTest` | PASS |
| `TwoVersionsKeepSeparateLayoutValues` | `EditorNodesPanelQmlTest` | PASS |
| `IdenticalCommittedProjectionDoesNotPublishOrQueueAnotherApply` | `EditorNodeSelectionLayoutTest` | PASS |
| `RepeatedSessionStateChangesDoNotReadHistoryOrRepublishNodes` | `EditorNodeSelectionLayoutTest` | PASS |
| `UnchangedHistoryProjectionDoesNotPublishOrQueueAnotherApply` | `EditorNodeSelectionLayoutTest` | PASS |
| `ChangedHistoryProjectionPublishesExactlyOneApply` | `EditorNodeSelectionLayoutTest` | PASS |
| `QueuedProjectionApplyIgnoresStaleAdapterAfterDetach` | `EditorNodeSelectionLayoutTest` | PASS |
| `LayoutKeyActivatesBeforeSavedSelectionRestore` | `EditorNodeSelectionLayoutTest` | PASS |
| `SavedLayoutSelectionDoesNotOverrideLiveSelectionOnTheSameKey` | `EditorNodeSelectionLayoutTest` | PASS |
| `OrdinaryDraftEditsDoNotCopyIntoTheCommittedSnapshot` | `EditorNodeSelectionLayoutTest` | PASS |
| `ImageSwitchAfterSubmitRefreshesTheCommittedProjection` | `EditorNodeSelectionLayoutTest` | PASS |
| `TwoVersionsKeepSeparatePositionsAndDrawerState` | `EditorNodeSelectionLayoutTest` | PASS |
| `ActiveVersionIdentityReadReturnsOnlyTheCheckedOutRef` | `EditorSessionHistoryPortTest` | PASS |

Regression commands: `cmd /c scripts\msvc_env.cmd --build build\debug --target EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest EditorSessionHistoryPortTest --parallel 4`, then the three generated test executables.
Suite totals after the correction: `EditorNodesPanelQmlTest` 22/22;
`EditorNodeSelectionLayoutTest` 37/37; `EditorSessionHistoryPortTest` 73/73.

**Checklist / exit condition:** 16.3.1 acceptance items covered by the tests above.

**LOC note (grill-code-review):** `editor_node_controller.cpp` 957 lines (under the ~1000-line split threshold; NM5.8c moves Qan mutation reversal out of this file). `EditorNodesPanel.qml` 414 lines. Layout store sources were not split; the controller now activates the layout key and applies positions, drawers, and live selection.

**Residual gaps:** `EditorNodeLayoutStore::ensureDefaultsFrom` still reads committed `snapshot()` and has no QML caller. NM5.8c still owns adapter `ApplyMutation` and delegate-library extraction. Keyboard, persistent history, and package checks remain NM5.8e–g. NM8 retains real-RAW and three-backend qualification.

#### 16.3.2 NM5.8b — Bounded draft rollback and one node projection implementation

**Files:** `app/editor_node_graph_draft.cpp`, `include/app/editor_node_graph_draft.hpp`,
`app/editor_node_graph_projection.cpp`, `include/app/editor_node_graph_projection.hpp`,
`editor_node_controller.{hpp,cpp}`, `editor_node_graph_draft_test.cpp`, and
`editor_node_graph_projection_test.cpp`.

- Keep `EditorNodeGraphDraft` as the sole owner of draft topology, base identity, default-name
  counter, indexes, net delta, and last-operation reversal data. Preserve the immutable base data
  needed to serialize removed nodes and restore original ordering.
- Replace `Checkpoint` copies of all nodes, edges, JSON, indexes, and accumulated deltas with an
  operation reversal record containing only affected entries and prior counter/order values.
  First land reversal with existing storage; then simplify indexing in a separate coherent commit.
- Replace per-edge `RebuildIndexes` calls with affected-entry updates. Prefer stable edge keys or
  stable storage so removing an edge does not renumber every adjacency entry. Materialize ordered
  serialized indexes at `MakeChange`, where full traversal is acceptable.
- Remove unused `EdgeRecord` and unused conversion/accessor surfaces after a repository caller
  scan. Consolidate paired key/value maps only where both have identical ownership and lifetime.
- Share node-kind/name/Mask-row projection through `EditorNodeGraphProjection::ProjectNode`.
  Keep committed backbone traversal and draft traversal distinct: detached nodes are legitimate
  draft content, and Mask display order must remain the stored order.
- Stop copying `CurrentSnapshot` into controller `snapshot_` after each draft operation. The
  controller exposes the active read view from the draft or committed snapshot; materialize a
  complete value snapshot only at an explicit projection boundary such as page recreation.
- Compute submission validity once per admitted mutation and expose the result. QML availability
  reads must not repeat graph traversals; invalid requests must retain the prior cached result.

**Acceptance:** `EditorNodeGraphDraftTest` must cover Add/Delete/Connect reversal, net cancellation,
multiple detached nodes, exact counter/order/JSON restoration, and a 32-Grade graph with large
Mask payloads over 100 connections. Add deterministic work/copy instrumentation that asserts no
whole-draft checkpoint or per-edge complete index rebuild. Record validity traversal separately;
it may visit the whole graph. Compare serialized forward/inverse results with
`PipelineGraphTopologyDeltaTest`. Do not claim this stage complete based on `&draft` equality.

##### NM5.8b completion record (2026-09-04)

**Status:** complete — bounded last-mutation reversal, stable edge keys, shared `ProjectNode`/`KindOf`,
cached submission validity, no per-edit copy into controller `snapshot_`

**Primary success call chain:**

```text
EditorNodeGraphDraft::FromDocument
  -> ProjectNode for every live node (including detached Color Grades)
  -> RebuildIndexes once
  -> ComputeSubmissionValid once
AddColorGrade / RemoveColorGrade / Connect
  -> BeginReversal of affected entries and prior counter/validity
  -> InsertNodeAt / EraseNodeAt / InsertEdgeAt / RemoveEdgeByKey / DisconnectDraftKeys
  -> FinishMutation ComputeSubmissionValid once
RestoreLastMutation
  -> reverse added/removed nodes and edges, restore JSON/delta maps and counters
MakeChange
  -> materialize ordered serialized indexes
  -> ApplyNodeGraphTopologyChange Forward then Inverse restores document JSON and name counter
```

**Primary failure call chain:**

```text
Connect rejected (self-connect, cycle, unknown node, unsupported port)
  -> AdmitConnect fails before BeginReversal
  -> indexes, delta, and cached SubmissionValid unchanged
  -> validity_traversals does not increase
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `AddDeleteConnectReversalRestoresExactCounterOrderAndJson` | `EditorNodeGraphDraftTest` | PASS |
| `NetCancellationRestoresBaseWithoutWholeDraftCopy` | `EditorNodeGraphDraftTest` | PASS |
| `MultipleDetachedGradesKeepIndependentNodesAndEdges` | `EditorNodeGraphDraftTest` | PASS |
| `RejectedConnectRetainsCachedSubmissionValidity` | `EditorNodeGraphDraftTest` | PASS |
| `SerializedDraftChangeMatchesInPlaceForwardAndInverse` | `EditorNodeGraphDraftTest` | PASS |
| `ThirtyTwoGradeGraphRepeatedConnectStaysBounded` | `EditorNodeGraphDraftTest` | PASS |
| `ProjectNodeCopiesStoredMaskOrderForDetachedGrades` | `EditorNodeGraphProjectionTest` | PASS |
| `OrdinaryDraftEditsDoNotCopyIntoTheCommittedSnapshot` | `EditorNodeSelectionLayoutTest` | PASS |

Commands: same MSVC debug build as NM5.8a, then `EditorNodeGraphDraftTest.exe` and `EditorNodeGraphProjectionTest.exe`.
Suite totals: `EditorNodeGraphDraftTest` 14/14; `EditorNodeGraphProjectionTest` 7/7. Work stats assert `complete_state_copies == 0` and `complete_index_rebuilds == 0` after construction; 32-Grade × 8 Masks × 100 connects recorded `validity_traversals == 101`.

**Checklist / exit condition:** 16.3.2 acceptance items covered. Forward/inverse comparison uses `ApplyNodeGraphTopologyChange` (the same in-place applier as `PipelineGraphTopologyDeltaTest`), not `&draft` equality.

**LOC note (grill-code-review):** `editor_node_graph_draft.cpp` 642 lines; header 213. `editor_node_graph_projection.cpp` 82 lines; header 110.

**Residual gaps:** NM5.8c still owns Qan primitive reversal. Persistent Undo/Redo/WAL/reopen/checkout evidence remains NM5.8e. NM8 retains real-RAW and three-backend qualification.

#### 16.3.3 NM5.8c — Qan primitive ownership and atomic visual mutation

**Files:** `alcedo_qan_graph.{hpp,cpp}`, new
`qan_delegate_library.{hpp,cpp}` in the matching `album_backend` source/include directories,
`editor_node_controller.cpp`, `alcedo_qan_graph_test.cpp`, focused
`qan_delegate_library_test.cpp`, and the production/UI-test CMake source lists.

- Extract `QanDelegateLibrary` with `Configure`, `EnsureLoaded`, `ComponentFor`, and `Reset` APIs.
  It owns delegate URLs, engine identity, cached components, and per-graph delegate-install state.
  Move `EnsureDelegates`, `LoadComponent`, `DropCachedDelegates`, and delegate installation with
  that state. It receives explicit engine/graph arguments and never a controller/adapter parent
  pointer, shared mutable context, or friend access.
- `AlcedoQanGraph` retains graph lifetime, NodeId/Qan maps, ports, candidate edges, applied
  projection, drawer connections, and applied selection. Full replacement and incremental insert
  must share primitive creation, presentation, registration, and cleanup helpers. Preserve pinned
  QuickQanava port-list cleanup needed before rebinding an exclusive port.
- Move Qan-operation reversal out of `EditorNodeController::ApplyMutationToGraph` into an adapter
  `ApplyMutation` API. Track only successfully completed visual changes. On failure return the
  original error plus any reversal error; the controller reverses the draft once.
- Remove redundant stored node-projection copies where one authoritative value plus lookup is
  sufficient. Keep reverse pointer-to-NodeId maps: they enforce generation/lifetime safety.
- Preserve separate full replacement and incremental mutation operations. They have different
  lifetime guarantees and must not be collapsed into a rebuild on every edit.

**Acceptance:** `AlcedoQanGraphTest` injects failure at each node/port/edge insertion, removal, and
binding step, and during visual reversal. Assert unrelated QObject identities, exact edges,
selection/layout, error visibility, and no product submission. Test `QanDelegateLibrary` without
constructing `EditorNodeController`; require unload/recreate with a different engine. Re-run the
production QML panel tests after registering all new files. Physical `.cpp` splitting alone does
not complete this stage.

##### NM5.8c completion record (2026-09-04)

**Status:** complete — delegate ownership is isolated, full replacement and incremental visual
operations share the same primitive helpers, and draft mutation reversal is adapter-owned.

**Primary success call chains:**

```text
AlcedoQanGraph::InsertTopology / InsertProjectedNode
  -> qmlEngine(bound graph)
  -> QanDelegateLibrary::EnsureLoaded(engine, graph)
  -> LoadComponent for node and edge delegates
  -> install graph-owned port, dock, and invisible-selection components
  -> InsertNodeVisual / InsertEdgeVisual
  -> register NodeId/Qan and edge/port maps, presentation, and drawer signals

EditorNodeController::ApplyDraftMutationToAdapter
  -> AlcedoQanGraph::ApplyMutation
  -> prevalidate mapped removals and insertions
  -> RemoveEdgeVisual / RemoveNodeVisual for completed removals
  -> InsertNodeVisual / InsertEdgeVisual for completed insertions
  -> update the applied projection only after every visual step succeeds
  -> preserve selection and layout, then return success
```

**Primary failure call chain:**

```text
Injected node/port/edge insertion, binding, or removal failure
  -> return the actual Qan operation error
  -> reverse only the completed visual operations in reverse order
  -> restore QuickQanava port lists before an exclusive edge can be rebound
  -> restore edge candidate state, drawer connections, selection, and layout
  -> append every visual-reversal error to the original error
  -> controller calls EditorNodeGraphDraft::RestoreLastMutation once
  -> controller exposes the adapter error without submitting a product change
```

**What was proven (executed tests):**

| Required behavior | Target / binary | Result |
| --- | --- | --- |
| Node, port, edge insertion and edge-binding failure preserve unrelated identities, edges, selection, and layout | `AlcedoQanGraphTest` | PASS |
| Edge, port, and node removal failure restores the mapped projection | `AlcedoQanGraphTest` | PASS |
| Visual reversal failure returns both the original and reversal errors | `AlcedoQanGraphTest` | PASS |
| Failed visual mutation leaves `PipelineDocument` unchanged | `AlcedoQanGraphTest` | PASS |
| Delegate loading and graph-owned installation without `EditorNodeController` | `QanDelegateLibraryTest` | PASS |
| Reset followed by loading through a different `QQmlEngine` | `QanDelegateLibraryTest` | PASS |
| Draft add/delete/connect behavior and no product command for an incomplete draft | `EditorNodeSelectionLayoutTest` | PASS |
| Registered Nodes-page QML behavior after the adapter change | `EditorNodesPanelQmlTest` | PASS |

**Commands and totals:**

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlcedoQanGraphTest QanDelegateLibraryTest EditorNodesPanelQmlTest EditorNodeSelectionLayoutTest
build/debug/alcedo_studio/tests/ui/AlcedoQanGraphTest_runtime/AlcedoQanGraphTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/QanDelegateLibraryTest_runtime/QanDelegateLibraryTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorNodesPanelQmlTest_runtime/EditorNodesPanelQmlTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorNodeSelectionLayoutTest_runtime/EditorNodeSelectionLayoutTest.exe --gtest_color=no
```

The MSVC build completed successfully. `AlcedoQanGraphTest` ran 25/25, `QanDelegateLibraryTest`
ran 2/2, `EditorNodesPanelQmlTest` ran 22/22, and `EditorNodeSelectionLayoutTest` ran 34/34.
The build and runtime logs were kept under `build/tmp/`. The runtime suites still print existing
Qt/QuickQanava QML warnings about native-style customization, unmatched `Connections` handlers,
and undefined dock properties; they did not affect pass/fail results and remain outside this
stage's scope.

**Checklist / exit condition:**

- [x] `QanDelegateLibrary` owns delegate URLs, engine identity, cached components, and graph
      installation markers; it has no controller or adapter parent and no shared context.
- [x] Full replacement and incremental operations use shared node/edge creation, presentation,
      registration, and cleanup helpers.
- [x] Qan reversal tracks only completed visual operations and reports reversal errors.
- [x] The adapter retains reverse Qan-pointer-to-`NodeId` maps and no redundant node projection
      map; `applied_` is the authoritative node-card value store.
- [x] The controller has one adapter mutation call and one draft reversal on adapter failure.
- [x] Production and focused test CMake source lists register every new file.
- [x] Failure injection covers every requested primitive boundary and a failing visual reversal.
- [x] Delegate tests use separate engines and do not construct `EditorNodeController`.

**LOC / diff note:** The working-tree diff is 1,275 insertions and 456 deletions across tracked
files, plus 430 lines in the three new files (`qan_delegate_library.hpp` 109,
`qan_delegate_library.cpp` 178, and `qan_delegate_library_test.cpp` 143). The adapter source is
1,598 lines (1,159 before this stage) and its header is 473 lines (368 before); the delegate
library is split into the new 109-line header and 178-line source. The controller source is 988
lines (957 before), below the plan's approximate split threshold after Qan rollback moved out.

**Residual gaps:** NM5.8d still owns obsolete command-entry cleanup. NM5.8e still owns persistent
topology Undo/Redo, WAL recovery, reopen, checkout, and long-lived Loader evidence. NM5.8f still
owns keyboard, accessibility, localization, large-font, and visual-state qualification. NM5.8g
still owns fresh install/package, notices, available macOS, and final regression evidence. NM8
retains real-RAW and three-backend qualification.

#### 16.3.4 NM5.8d — Remove unused command entry points; preserve stored history

**Files:** `editor_session_controller.{hpp,cpp}`, `editor_node_controller.{hpp,cpp}`,
`app/editor_session_service.cpp`, `include/app/editor_session_service.hpp`,
`include/app/editor_session_ports.hpp`, `include/app/editor_session_command_queue.hpp`,
`app/editor_action_policy.cpp`, `editor_session_history_port.{hpp,cpp}`,
`editor_history_mutation.{hpp,cpp}`, and their existing command/history tests.

- Inventory callers of `SubmitAddColorGrade`, `SubmitRemoveColorGrade`, and
  `SubmitReconnectColorGrade`; the review found only declarations/definitions in production.
  Delete unused controller wrappers and the unused `canReconnectSelectedColorGrade` surface.
  Audit service, queue, port, and mutation methods independently before removing the corresponding
  obsolete interactive entry chain. Existing tests must move to the supported path or the actual
  retained domain API; do not keep a production entry point solely for an old test.
- Keep `RenameColorGrade` and `EditNodeGraph` as distinct commands. Consolidate their common
  success publication/render-reason/revision tail in a small service helper. The service retains
  queue admission and lifecycle policy; the helper owns no new state.
- Preserve Add/Remove/Reconnect typed payload parsing, serialization, inverse replay, presentation,
  and Mask reachability where stored data requires them. `document_transfer.cpp` still constructs
  Add/Remove changes. Preserve its producers and the supporting domain operations.
- Do not change stored kind strings, hashes, format versions, name-counter semantics, or paste
  behavior as a cleanup side effect. Do not remove current-panel projection of document values
  from NM1/NM4; NM6 must still receive a working adjustment panel.
- Limit this stage to node command ownership. The oversized session/pipeline/history files do not
  justify a general service rewrite. Any wider split needs its own state inventory and acceptance.

**Acceptance:** run `EditorSessionNodeCommandTest`, action-policy tests, `PipelineEditBatchTest`,
`PipelineHistoryApplierTest`, document-transfer tests, and Mask reachability tests. Prove that one
topology command publishes one batch/revision/Quality render, Rename publishes no photo render,
and old payloads still replay with exact names/counters and pasted Mask assets. Record a final
production caller scan for every removed public symbol.

##### NM5.8d completion record (2026-09-04)

**Status:** complete — unused interactive Add/Remove/Reconnect session entries deleted; stored
typed payloads, Capture/Make helpers, document-transfer producers, inverse replay, paste, and
Mask reachability retained; Rename and EditNodeGraph share one success-publication helper

**Primary success call chain:**

```text
EditorNodeController draft AddColorGrade / RemoveColorGrade / Connect
  -> MaybeSubmitDraft
  -> EditorSessionController::SubmitNodeGraphTopologyEdit
  -> EditorSessionService::EditNodeGraph
  -> IEditorHistoryPort::EditNodeGraph
  -> EditorHistoryMutation::EditNodeGraph
  -> EditorSessionService::PublishTypedNodeHistorySuccess
  -> LastPublishedRenderReason GraphTopologyChanged
  -> one Quality RouteInitialRender + BumpHistoryRevision + Emit
```

**Rename success call chain:**

```text
EditorNodeController::renameColorGrade
  -> EditorSessionController::SubmitRenameColorGrade
  -> EditorSessionService::RenameColorGrade
  -> IEditorHistoryPort::RenameColorGrade
  -> EditorSessionService::PublishTypedNodeHistorySuccess
  -> LastPublishedRenderReason empty
  -> Accepted, no photo render, BumpHistoryRevision + Emit
```

**Stored typed-payload replay (not an interactive session command):**

```text
CaptureAddColorGradeChange / CaptureRemoveColorGradeChange / CaptureReconnectColorGradeChange
  -> MakeAddColorGradeBatch / MakeRemoveColorGradeBatch / MakeReconnectColorGradeBatch
  -> EditorSessionHistoryPort::CommitPipelineEditBatch
  -> EditorHistoryMutation::CommitPipelineEditBatch
  -> PublishAppliedTypedBatch
  -> Undo / Redo / WAL reopen / paste restore names, counters, and Mask assets
```

**Primary failure call chain:**

```text
RenameColorGrade or EditNodeGraph history mutation returns false (for example journal append)
  -> EditorSessionService::Reject with the exact mutation error
  -> no BumpHistoryRevision, no RouteInitialRender
```

**Production caller scan (removed public symbols; `alcedo_studio/src` matches: none):**

- `SubmitAddColorGrade`, `SubmitRemoveColorGrade`, `SubmitReconnectColorGrade`
- `canReconnectSelectedColorGrade` / `can_reconnect_selected_color_grade`
- `IEditorSessionBackend` / `EditorSessionService` `AddColorGrade`, `RemoveColorGrade`,
  `ReconnectColorGrade`
- `EditorSessionCommandKind::{Add,Remove,Reconnect}ColorGrade` and the extra
  `before_node_id` / `predecessor_node_id` / `successor_node_id` command fields
- `IEditorHistoryPort` / `EditorSessionHistoryPort` / `EditorHistoryMutation`
  interactive Add/Remove/Reconnect methods
- `EditorActionPolicy` cases for those three kinds

Retained names that still have production callers: draft
`EditorNodeGraphDraft::AddColorGrade` / `RemoveColorGrade`; domain
`ReconnectColorGrade`; typed `AddColorGradeChange` / `RemoveColorGradeChange` /
`ReconnectColorGradeChange`; Capture/Make helpers; `document_transfer.cpp` Add/Remove
producers; codec, applier, presentation keys, and Mask reachability.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RenameCreatesOneHistoryChangeWithoutRender` | `EditorSessionNodeCommandTest` | PASS |
| `EditNodeGraphCreatesOneHistoryChangeAndRoutesTopologyQualityRender` | `EditorSessionNodeCommandTest` | PASS |
| `JournalFailurePublishesExactErrorWithoutHistoryOrRenderChange` | `EditorSessionNodeCommandTest` | PASS |
| `RenameAndEditNodeGraphUseTheSameAdmissionDecisionAsSettledAdjustments` | `EditorSessionActionPolicyCq3Test` | PASS |
| Typed Add/Remove/Reconnect parse, serialize, inverse, and presentation | `PipelineEditBatchTest` | PASS |
| Applier replay of stored typed batches | `PipelineHistoryApplierTest` | PASS |
| `PasteRemapsEveryNodeAdjustmentAndMaskId` | `DocumentTransferTest` | PASS |
| Inactive Version and WAL keep referenced Mask assets | `MaskAssetReachabilityTest` | PASS |
| `AddGradeUndoRedoPreservesStableIdsAndCleanValues` | `EditorSessionHistoryPortTest` | PASS |
| `DeleteGradeUndoRestoresNodeMasksAndExactEdges` | `EditorSessionHistoryPortTest` | PASS |
| `ReconnectUndoRedoRestoresBackboneOrder` | `EditorSessionHistoryPortTest` | PASS |
| `AddRenameAndDeleteSnapshotsPresentTypedHistoryTitles` | `EditorSessionHistoryPortTest` | PASS |
| `AddJournalFailureRestoresDocumentHeadCounterAndPublishedRenderReason` | `EditorSessionHistoryPortTest` | PASS |
| `RecoveryAppliesCommittedTypedSuffixExactlyOnce` (exact name and counter) | `EditorSessionHistoryPortTest` | PASS |
| Incomplete draft still publishes no product topology command | `EditorNodeSelectionLayoutTest` | PASS |
| Incomplete draft still publishes no product topology command | `EditorNodesPanelQmlTest` | PASS |

**Commands:**

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionNodeCommandTest EditorSessionActionPolicyCq3Test PipelineEditBatchTest PipelineHistoryApplierTest DocumentTransferTest MaskAssetReachabilityTest EditorSessionHistoryPortTest EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest
build/debug/alcedo_studio/tests/app/EditorSessionNodeCommandTest_runtime/EditorSessionNodeCommandTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/app/EditorSessionActionPolicyCq3Test_runtime/EditorSessionActionPolicyCq3Test.exe --gtest_color=no
build/debug/alcedo_studio/tests/edit/PipelineEditBatchTest_runtime/PipelineEditBatchTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/app/PipelineHistoryApplierTest_runtime/PipelineHistoryApplierTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/app/DocumentTransferTest_runtime/DocumentTransferTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/app/MaskAssetReachabilityTest_runtime/MaskAssetReachabilityTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorSessionHistoryPortTest_runtime/EditorSessionHistoryPortTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorNodeSelectionLayoutTest_runtime/EditorNodeSelectionLayoutTest.exe --gtest_color=no
build/debug/alcedo_studio/tests/ui/EditorNodesPanelQmlTest_runtime/EditorNodesPanelQmlTest.exe --gtest_color=no
```

Suite totals: `EditorSessionNodeCommandTest` 3/3; `EditorSessionActionPolicyCq3Test` 12/12;
`PipelineEditBatchTest` 17/17; `PipelineHistoryApplierTest` 12/12; `DocumentTransferTest` 6/6;
`MaskAssetReachabilityTest` 2/2; `EditorSessionHistoryPortTest` 73/73 (includes
`EditorDocumentHistoryTest` and `EditorVersionCheckoutTest`); `EditorNodeSelectionLayoutTest`
37/37; `EditorNodesPanelQmlTest` 22/22. Logs under `build/tmp/nm5-8d/`. Existing Qt/QuickQanava
QML warnings about native-style customization, unmatched `Connections` handlers, and undefined
dock properties did not affect pass/fail.

**Checklist / exit condition:**

- [x] Unused `SubmitAddColorGrade` / `SubmitRemoveColorGrade` / `SubmitReconnectColorGrade`
      wrappers and `canReconnectSelectedColorGrade` deleted after a production caller scan.
- [x] Matching unused service, queue, action-policy, history-port, and mutation interactive
      entries removed; tests moved to `EditNodeGraph` or `CommitPipelineEditBatch`.
- [x] `RenameColorGrade` and `EditNodeGraph` remain distinct commands; shared success tail is
      `PublishTypedNodeHistorySuccess` with no extra state.
- [x] One topology command publishes one history change, one revision bump, and one Quality
      `GraphTopologyChanged` render; Rename publishes no photo render.
- [x] Stored Add/Remove/Reconnect parsing, serialization, inverse replay, presentation,
      document-transfer producers, paste Mask remapping, and Mask reachability retained.
- [x] Stored kind strings, hashes, format versions, name-counter semantics, and paste behavior
      unchanged as a cleanup side effect.
- [x] No general session/pipeline/history rewrite.

**LOC note (grill-code-review):** Working-tree diff is 153 insertions and 629 deletions across
21 tracked files. After this stage: `editor_session_service.cpp` 1,560 lines (header 517);
`editor_history_mutation.cpp` 978 (header 85); `editor_session_controller.cpp` 1,351;
`editor_node_controller.cpp` 1,031. The session service remains above the approximate split
threshold; this stage only deleted unused command entries and added a small success helper, as
the plan required. No general service rewrite.

**Residual gaps:** NM5.8e still owns persistent topology Undo/Redo, WAL recovery, reopen,
checkout, and long-lived Loader evidence for `NodeGraphTopologyChange` submitted through the
production history port. NM5.8f still owns keyboard, accessibility, localization, large-font,
and visual-state qualification. NM5.8g still owns fresh install/package, notices, available
macOS, and final regression evidence. NM8 retains real-RAW and three-backend qualification.

#### 16.3.5 NM5.8e — Real history and lifecycle evidence

**Files:** `editor_nodes_panel_qml_test.cpp`, `editor_node_controller_test.cpp`,
`editor_session_node_command_test.cpp`, new `tests/edit/history/editor_node_topology_history_test.cpp`,
`pipeline_history_applier_test.cpp`, `tests/edit/history/editor_version_checkout_test.cpp`, and
their focused fixtures. Register the new persistent-history source in the matching history test
target. Reuse the storage setup patterns in `editor_session_history_port_test.cpp` without growing
that oversized file or the shared 754-line rail harness with unrelated storage and GPU setup.

- Submit a real `NodeGraphTopologyChange` through the production history port and temporary
  storage, then Undo, Redo, recover the WAL, reopen, and checkout a second Version.
- Assert document serialization, node/edge order, counters, head/Version, Mask asset values,
  projection identity, and render reason/count. Direct domain inverse tests and fake-history call
  counters remain useful unit evidence but do not satisfy this acceptance.
- Inject WAL failure, post-commit projection failure, and projection-promotion failure. Require
  exact errors; no success result may silently erase a projection error. Assert product commit
  state separately from visual state when the failure occurs after publication.
- Exercise queued refresh after image switch, same-image Version switch, incomplete draft during
  parameter/Mask-role changes, panel replacement, and 100 Loader open/close cycles. Record live
  Qan object counts and prove no stale callback mutates the next page/session.
- Correct test names that still describe immediate reconnect/rebuild or bridge-on-delete when
  their assertions now exercise draft editing. Keep assertions on the actual user-visible result.

**Acceptance:** all new persistent-history tests run from registered targets. Fill the matrix in
Sections 17–19 with executable evidence, including recovery failure and subsequent usable state.
Only then mark exact topology Undo/Redo/recovery/reopen/checkout complete in Section 20.

#### 16.3.6 NM5.8f — Keyboard, accessibility, localization, and visual states

Complete the original empty/loading/pending/error states, keyboard connection flow, drawer focus,
accessible names, localization, 30–40 percent text expansion, large system fonts, and the full
Section 17.1 visual matrix. Use the Alcedo QML skills before implementation. Keep domain validation
in the domain; translate structured known errors once at the application presentation boundary
while preserving exact technical details for unknown failures.

**Acceptance:** `EditorNodesPanelQmlTest`, delegate tests, rail lifecycle tests, and
`WorkspaceShellTest` cover every visible action and all existing VI restrictions. Record real
screen-reader checks separately from QML property assertions. No new Apply/Cancel UI is allowed.

#### 16.3.7 NM5.8g — Build, install, package, and final regression

Complete the original Windows/MSVC and available macOS builds, install/package QML import checks,
and QuickQanava/bezier notices. Run the affected graph, history, adapter, QML, workspace, and
NM1–NM4 regression targets after the cleanup. Native runtime tests remain required if runtime
source changes; this split does not authorize backend algorithm or quality changes.

**Acceptance:** record exact configure/build/test/install/package commands, target registration,
test counts, installed-app startup, licence contents, visual matrix, and unavailable environments.
Run the Section 19 performance checks on the resulting production path. Fresh build evidence is
required; the review's existing-binary reruns are only a baseline. NM8 retains real-RAW and final
three-backend qualification.

**Original acceptance checklist, retained across NM5.8a–NM5.8g:**

1. Complete empty, loading, pending-command, and error behavior.
2. Complete localized text and accessible names.
3. Check 30–40 percent text expansion.
4. Check large system font sizes.
5. Repeat Loader destruction and recreation.
6. Check image switch, Version checkout, Undo, Redo, and recovery.
7. Check every UI ban with property assertions or screenshots.
8. Build on Windows with the repository MSVC wrapper.
9. Build on macOS when the environment is available.
10. Check install and package QML imports.
11. Check QuickQanava and bezier licence notices.
12. Run all affected graph, history, adapter, QML, and workspace tests.

### 16.4 Main call chain

```text
packaged app start
  -> Basic style
  -> pinned QuickQanava initialization
  -> static QML module is available
  -> editor opens
  -> Nodes Loader creates GraphView
  -> current Version projection appears
  -> all visible product changes use typed history
```

### 16.5 Tests and exit criteria

- No-image state uses localized plain text.
- Loading never shows the previous image graph.
- Command failure keeps the prior graph and exact counter state.
- Keyboard input can select, open drawers, Add, Rename, Delete, Fit, and start supported topology
  connections without exposing an Apply or Cancel action.
- A screen reader reads node names, Mask types, disclosure state, and actions.
- It does not read topology numbers, On/Off state, adjustment summaries, or hidden Mask fields.
- Both themes and all four target DPR values pass the visual matrix.
- The production QML tree contains no unapproved pill, badge, or status dot.
- The Windows package loads QuickQanava.
- The macOS package loads QuickQanava when that environment is available.
- The package contains the required third-party notices.
- Lifetime checks find no stale Qan pointer use.

### 16.6 Completion record

Record the date, commands, test count, visual matrix, package results, and unavailable environment
checks here.

| Stage | Status | Required evidence |
| --- | --- | --- |
| NM5.8a | Complete 2026-09-04 | One update route; queued teardown and layout restore |
| NM5.8b | Complete 2026-09-04 | Bounded reversal/copy work; exact draft values and counters |
| NM5.8c | Complete 2026-09-04 | Delegate ownership; primitive failure and reversal matrix |
| NM5.8d | Complete 2026-09-04 | Caller deletion audit; retained typed replay/paste |
| NM5.8e | Pending | Persistent topology history; lifecycle and failure recovery |
| NM5.8f | Pending | Keyboard, screen reader, localization, visual matrix |
| NM5.8g | Pending | Fresh builds, regression, installed packages, notices |

---

## 17. Test target plan

Prefer an existing target when its responsibility matches. Add a target only when ownership stays
clear.

| Target | Responsibility |
| --- | --- |
| `PipelineDocumentDefaultNameTest` | Serialized counter, default names, format validation, and replay. |
| `EditorNodeGraphProjectionTest` | Snapshot values, Mask kinds, revisions, NodeId, and generation. |
| `EditorNodeGraphDraftTest` | One-time construction, incremental Add/Delete/Connect, net-delta cancellation, admission checks, and submission validity. |
| `PipelineGraphTopologyDeltaTest` | In-place atomic forward/inverse application, exact restoration, object identity, and final validation. |
| `EditorNodeControllerTest` | Selection, draft lifetime, automatic submission, stale guards, failure, and render reasons. |
| `AlcedoQanGraphTest` | Primitive mapping, incremental node/edge updates, identity preservation, selection, role updates, and stale-object rejection. |
| `EditorNodesPanelQmlTest` | VI, node content, drawer behavior, no Apply/Cancel action, exact errors, keyboard, accessibility, and layout restore. |
| `EditorWorkspaceToolRailLifecycleQmlTest` | History, Versions, and Nodes mutual exclusion and Loader lifetime. |
| `WorkspaceShellTest` | Column geometry, minimum window size, focus order, and production type registration. |
| `EditorSessionHistoryPortTest` | Typed graph history, Undo, Redo, WAL failure restoration, and counter replay. |

Every test name must state the behavior that it checks. Do not use `smoke` in a test name.

### 17.1 Required visual matrix

Capture or assert:

- empty and populated Mask drawers;
- open and closed Mask drawers;
- selected and unselected nodes;
- Develop, Color Grade, and DRT/Post delegates;
- long translated node names;
- 0, 1, and many Masks;
- both Alcedo themes;
- DPR 1.0, 1.25, 1.5, and 2.0;
- reduced motion;
- minimum 960×640 window geometry.

The matrix must show that the page has no topology number, status dot, On/Off content, adjustment
summary, Mask count, pill, badge, shadow, glow, gradient, or Material chrome.

---

## 18. Failure matrix

| Failure point | Required result |
| --- | --- |
| QuickQanava import | App or test startup fails with the real QML error. Do not continue with a substitute canvas. |
| Projection read | Keep the prior graph. Show the exact error. Do not create a default replacement graph. |
| Stale session generation | Reject the snapshot or request. Do not touch the current session. |
| Qan node creation | Roll back the adapter update. Keep the prior complete Qan projection. |
| Qan edge creation or binding | Roll back the adapter update. Leave no orphan edge or port. |
| Incremental draft Qan update | Reverse only the affected draft and Qan changes. Preserve all unrelated object identities and issue no product command. |
| Draft creation | Keep the prior committed canvas. Show the exact error. Do not create a partial draft. |
| Draft Add | Keep existing draft objects and edges. Do not consume a product name number, commit history, or render while incomplete. |
| Unsupported draft connection | Reject before mutation. Keep draft values, indexes, delta, layout, and Qan primitives unchanged. |
| Incomplete draft | Keep the incremental draft visible. Do not change product state, history, Version, or rendering. |
| Atomic topology validation | Restore exact node ownership, node order, edge order, counter, and live object identity. Keep the draft. |
| Rename validation | Keep the prior name. Do not create history. |
| Draft Delete validation | Keep the node, edges, indexes, accumulated delta, selection, counter, and layout. |
| Connector admission | Close the preview. Keep the current draft edges and exact error. |
| WAL append | NM4 restores document and history. Restore counter, projection, and selection. |
| Version checkout | NM4 restores the prior Version after failure. Keep its selection and layout. |
| Loader teardown | Keep only plain controller and layout values. Keep no Qan pointer. |
| Package QML module load | Fail package acceptance. Do not release a build with a hidden Nodes page. |

---

## 19. Performance targets

- A parameter slider update does not rebuild the Qan graph.
- A Mask drawer role update changes only its owner node.
- Draft construction reads one complete projection once per bound base identity.
- An ordinary draft Add or Delete updates only the affected node, incident edges, and indexes.
- An ordinary draft Connect removes at most two Qan edges and inserts one Qan edge.
- An ordinary draft edit never reconstructs or overwrites the draft or Qan graph.
- Successful automatic submission promotes the current canvas without a Qan topology rebuild.
- Submission-validity traversal may scale with total draft nodes and edges; mutation work scales
  with the affected nodes, edges, and indexes.
- A fully closed panel retains no graph delegates.
- The default graph shows input feedback within 100 ms after first open.
- Node selection shows feedback within 100 ms.
- Add, Delete, and Reconnect show pending feedback within 100 ms.
- Graph navigation, node movement, and drawer folds do not start photo rendering.
- Layout storage does not block the GUI thread.

Record:

- first Nodes Loader time;
- delegate count for the default graph and a 32-Grade graph;
- projection apply time for a 32-Grade graph;
- frame time for 100 selections;
- frame time for opening and closing a node with many Mask rows;
- frame time and changed Qan object count for 100 accepted draft connections;
- draft and Qan object identity retention across 100 accepted draft connections;
- live Qan object count after 100 panel open/close cycles.

Do not reduce decode resolution, output quality, or backend behavior to meet these targets.

---

## 20. NM5 completion criteria

- [x] The production target links the pinned QuickQanava module.
- [x] Production initializes QuickQanava before QML load.
- [x] Production remains on Qt Quick Controls Basic.
- [x] Nodes is a page in the shared editor tool rail.
- [x] History, Versions, and Nodes share one page state and Loader.
- [x] `PipelineDocument` remains the only product graph.
- [x] The document stores the next default Color Grade name number.
- [x] A new document names its primary Grade `Color Grade 1`.
- [x] Add uses increasing default names and exact typed-history replay.
- [x] Nodes show no topology numbers.
- [x] Nodes show no status dots.
- [x] Nodes show no On/Off state or action.
- [x] Nodes show no adjustment summary.
- [x] Each Color Grade shows a default-open Mask drawer.
- [x] The user can close and reopen each drawer.
- [x] Drawer state is local UI state and creates no history or render.
- [x] Mask rows show only the approved type icon and type label.
- [x] The Nodes rail uses the approved `stack-2` path.
- [x] Gradient, Radial, and Brush use the approved paths.
- [x] The page contains no unapproved pill, badge, or status dot.
- [x] The page contains no `xx · xx` compound label or equivalent substitute.
- [x] Projection uses stable NodeId, MaskId, revisions, and session generation.
- [x] No Qan pointer survives a generation replacement.
- [x] Parameter edits do not rebuild the graph.
- [x] Version checkout publishes the correct projection.
- [x] The initial layout is vertical and deterministic.
- [x] Node positions, view, zoom, selection, and drawer state are local UI state.
- [x] The product allows one selected node.
- [x] Add creates a clean Color Grade.
- [x] Rename and Delete use NM4 typed history.
- [x] NM5.7 Reconnect uses the official visual connector in request-only mode.
- [x] `connectorCreateDefaultEdge` is false.
- [x] NM5.7R Add creates one disconnected draft Color Grade without a product change or render.
- [x] One draft is created from the complete projection and then mutated incrementally.
- [x] Ordinary draft operations do not reconstruct or overwrite the draft or Qan graph.
- [x] Exclusive-port Connect replaces only the source's prior output edge and the destination's
      prior input edge.
- [x] Supported incomplete graphs remain editable without a product change, history commit, or
      render request.
- [x] Unsupported node types and connections are rejected before draft mutation with exact text.
- [x] The first operation that restores a valid graph automatically submits; the UI has no Apply
      or Cancel action.
- [x] Automatic submission contains one `NodeGraphTopologyChange`, not stored
      Add/Remove/Reconnect steps.
- [x] The live `PipelineGraph` applies the topology delta atomically in place and is never replaced
      by the draft.
- [x] Successful automatic submission preserves unaffected Qan and `INodeModel` identities and
      requests one Quality render.
- [x] All command failures preserve the prior product graph, history, Version, revision, and
      rendered state while retaining an applicable draft.
- [ ] Atomic topology edits have dedicated production-history evidence for exact Undo, Redo,
      recovery, reopen, and Version checkout (NM5.8e; direct inverse tests already pass).
- [ ] All visible actions support keyboard input and accessibility.
- [ ] All product text uses localization.
- [ ] All visual values use AppTheme and `DESIGN.md`.
- [x] Node delegates use no shadow, glow, gradient, or Material style.
- [x] Reduced-motion behavior passes.
- [x] The viewer keeps its 360 logical-pixel floor at 960×640.
- [ ] Windows build, install, and package checks pass.
- [ ] macOS build, install, and package checks pass when the environment is available.
- [ ] Required third-party notices are in the package.
- [ ] Every sub-phase completion record lists pinned API differences.

NM6 must use NM5 `selectedNodeId`. NM6 must not create another node selection source.
