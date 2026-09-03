# Phase NM5 — QuickQanava Nodes Panel

Date: 2026-09-02

Status: NM5.1 complete; NM5.2–NM5.8 planned

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

Record the date, pinned connector properties, commands, test count, success chain, and failure
chain here.

---

## 15. NM5.8 — Lifecycle, accessibility, build, install, and package checks

### 15.1 Input context

NM5.1–NM5.7 provide the Nodes page. This sub-phase adds no product feature. It closes lifecycle,
accessibility, localization, build, and package gaps.

NM8 still owns final real-RAW and three-backend qualification.

### 15.2 Official documentation

- [Installation](https://cneben.github.io/QuickQanava/installation.html): all sections.
- [Graph](https://cneben.github.io/QuickQanava/graph.html): `QuickQanava Initialization` and `Graph View`.
- [API Reference](https://cneben.github.io/QuickQanava/reference.html): the local-Doxygen notice.
- [Licence](https://cneben.github.io/QuickQanava/licence.html): source and binary notice requirements.

### 15.3 Work

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

### 15.4 Main call chain

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

### 15.5 Tests and exit criteria

- No-image state uses localized plain text.
- Loading never shows the previous image graph.
- Command failure keeps the prior graph and exact counter state.
- Keyboard input can select, open drawers, Add, Rename, Delete, Fit, and Reconnect.
- A screen reader reads node names, Mask types, disclosure state, and actions.
- It does not read topology numbers, On/Off state, adjustment summaries, or hidden Mask fields.
- Both themes and all four target DPR values pass the visual matrix.
- The production QML tree contains no unapproved pill, badge, or status dot.
- The Windows package loads QuickQanava.
- The macOS package loads QuickQanava when that environment is available.
- The package contains the required third-party notices.
- Lifetime checks find no stale Qan pointer use.

### 15.6 Completion record

Record the date, commands, test count, visual matrix, package results, and unavailable environment
checks here.

---

## 16. Test target plan

Prefer an existing target when its responsibility matches. Add a target only when ownership stays
clear.

| Target | Responsibility |
| --- | --- |
| `PipelineDocumentDefaultNameTest` | Serialized counter, default names, format validation, and replay. |
| `EditorNodeGraphProjectionTest` | Snapshot values, Mask kinds, revisions, NodeId, and generation. |
| `EditorNodeControllerTest` | Selection, Add, Rename, Delete, Reconnect, failure, and render reasons. |
| `AlcedoQanGraphTest` | Primitive mapping, ports, edges, selection, role updates, and stale-object rejection. |
| `EditorNodesPanelQmlTest` | VI, node content, drawer behavior, icons, keyboard, accessibility, and layout restore. |
| `EditorWorkspaceToolRailLifecycleQmlTest` | History, Versions, and Nodes mutual exclusion and Loader lifetime. |
| `WorkspaceShellTest` | Column geometry, minimum window size, focus order, and production type registration. |
| `EditorSessionHistoryPortTest` | Typed graph history, Undo, Redo, WAL failure restoration, and counter replay. |

Every test name must state the behavior that it checks. Do not use `smoke` in a test name.

### 16.1 Required visual matrix

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

## 17. Failure matrix

| Failure point | Required result |
| --- | --- |
| QuickQanava import | App or test startup fails with the real QML error. Do not continue with a substitute canvas. |
| Projection read | Keep the prior graph. Show the exact error. Do not create a default replacement graph. |
| Stale session generation | Reject the snapshot or request. Do not touch the current session. |
| Qan node creation | Roll back the adapter update. Keep the prior complete Qan projection. |
| Qan edge creation or binding | Roll back the adapter update. Leave no orphan edge or port. |
| Add validation | Do not create a node, consume a name number, commit history, or render. |
| Rename validation | Keep the prior name. Do not create history. |
| Delete validation | Keep the node, edges, selection, counter, and layout. |
| Reconnect validation | Close the preview. Keep the permanent edges. |
| WAL append | NM4 restores document and history. Restore counter, projection, and selection. |
| Version checkout | NM4 restores the prior Version after failure. Keep its selection and layout. |
| Loader teardown | Keep only plain controller and layout values. Keep no Qan pointer. |
| Package QML module load | Fail package acceptance. Do not release a build with a hidden Nodes page. |

---

## 18. Performance targets

- A parameter slider update does not rebuild the Qan graph.
- A Mask drawer role update changes only its owner node.
- A topology update scales with affected nodes and edges.
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
- live Qan object count after 100 panel open/close cycles.

Do not reduce decode resolution, output quality, or backend behavior to meet these targets.

---

## 19. NM5 completion criteria

- [x] The production target links the pinned QuickQanava module.
- [x] Production initializes QuickQanava before QML load.
- [x] Production remains on Qt Quick Controls Basic.
- [ ] Nodes is a page in the shared editor tool rail.
- [ ] History, Versions, and Nodes share one page state and Loader.
- [ ] `PipelineDocument` remains the only product graph.
- [ ] The document stores the next default Color Grade name number.
- [ ] A new document names its primary Grade `Color Grade 1`.
- [ ] Add uses increasing default names and exact typed-history replay.
- [ ] Nodes show no topology numbers.
- [ ] Nodes show no status dots.
- [ ] Nodes show no On/Off state or action.
- [ ] Nodes show no adjustment summary.
- [ ] Each Color Grade shows a default-open Mask drawer.
- [ ] The user can close and reopen each drawer.
- [ ] Drawer state is local UI state and creates no history or render.
- [ ] Mask rows show only the approved type icon and type label.
- [ ] The Nodes rail uses the approved `stack-2` path.
- [ ] Gradient, Radial, and Brush use the approved paths.
- [ ] The page contains no unapproved pill, badge, or status dot.
- [ ] The page contains no `xx · xx` compound label or equivalent substitute.
- [ ] Projection uses stable NodeId, MaskId, revisions, and session generation.
- [ ] No Qan pointer survives a generation replacement.
- [ ] Parameter edits do not rebuild the graph.
- [ ] Version checkout publishes the correct projection.
- [ ] The initial layout is vertical and deterministic.
- [ ] Node positions, view, zoom, selection, and drawer state are local UI state.
- [ ] The product allows one selected node.
- [ ] Add creates a clean Color Grade.
- [ ] Rename and Delete use NM4 typed history.
- [ ] Reconnect uses the official visual connector.
- [ ] `connectorCreateDefaultEdge` is false.
- [ ] The backend accepts a request before permanent Qan edges change.
- [ ] All command failures preserve the prior product and projected graph.
- [ ] Add, Rename, Delete, and Reconnect support Undo and Redo.
- [ ] All visible actions support keyboard input and accessibility.
- [ ] All product text uses localization.
- [ ] All visual values use AppTheme and `DESIGN.md`.
- [ ] Node delegates use no shadow, glow, gradient, or Material style.
- [ ] Reduced-motion behavior passes.
- [ ] The viewer keeps its 360 logical-pixel floor at 960×640.
- [ ] Windows build, install, and package checks pass.
- [ ] macOS build, install, and package checks pass when the environment is available.
- [ ] Required third-party notices are in the package.
- [ ] Every sub-phase completion record lists pinned API differences.

NM6 must use NM5 `selectedNodeId`. NM6 must not create another node selection source.
