# Brush Mask Architecture, History, and Raster Materialization Master Plan

Date: 2026-09-11

Status: planned for the release after the current Node Editor delivery. The current product build
keeps `ALCEDO_ENABLE_BRUSH_MASK=OFF`. Node Editor NM7.13 establishes the detachable build boundary;
all implementation and product qualification in this plan happens outside the current release.

Parent context:

- [Node-aware Pipeline Editing and Mask Creation Master Plan](node_mask_editor_master_plan.md)
- [NM7 Viewer Mask Creation and Editing](node_mask_editor/phase_nm7_viewer_mask_creation_plan.md)
- [Historical NM7 parameter replay and project-cache design](node_mask_editor/mask_command_replay_and_project_cache_plan.md)

The historical NM7 design and completion records remain evidence for code that exists. They are
not the target architecture for this plan. This plan replaces parameterized Brush samples embedded
in JSON, per-dab native dispatch, and render-time reconstruction from the entire stroke list.

---

## 1. Decision and release boundary

Brush is not part of the current Node Editor release. That release delivers:

- Radial Mask creation, selection, editing, history, Version, Paste, reopen, and export;
- Linear Gradient Mask creation, selection, editing, history, Version, Paste, reopen, and export;
- multi-Grade and multi-Mask Union execution;
- NM8 whole-DAG execution performance work with Brush excluded from the build.

The current release must not ship a hidden or unreachable Brush implementation. With
`ALCEDO_ENABLE_BRUSH_MASK=OFF`, Brush code is absent from the production build graph rather than
compiled and hidden at runtime. Unsupported Brush data fails at the project/document boundary; it
does not select Radial, Linear Gradient, CPU evaluation, a lower quality path, or an empty Mask.
NM7.13 cuts project metadata from `0.6.0` to the Brush-disabled `0.7.0` format. The abandoned
experimental Brush parameters in `0.6.0` are never an input to this plan and receive no conversion.

The next Brush release starts only after this plan has an execution plan and approved project-format
cutover. Turning the option on is not itself a release decision.

---

## 2. Problem statement

The current implementation couples four different forms of Brush data:

```text
pointer input
  -> canonical dab samples in BrushStroke
  -> JSON in edit commits and the full PipelineDocument checkpoint
  -> runtime replay into a canonical R8
  -> session-local GPU textures and signed-distance data
```

This produces several independent scaling failures:

1. A long stroke stores thousands or millions of sample objects as JSON.
2. The current complete PipelineDocument stores the accumulated sample bodies again.
3. Opening or switching an image parses and copies the full Brush payload on the session owner.
4. A cold render reconstructs R8 from samples even though a persistent R8 repository exists.
5. The current CUDA path launches one stamp kernel for each dab.
6. GUI source equality and overlay-bound calculations can walk every sample.
7. Whole-image R8 storage per Brush edit would replace CPU/GPU cost with unbounded disk growth.

Sampling-rate reduction alone cannot solve duplicated JSON, history growth, cold raster rebuilds,
or per-dab dispatch. The persistent model, history representation, raster materialization, native
execution, UI projection, and garbage collection must be designed together.

---

## 3. Locked product behavior

The following behavior is not negotiable within this plan:

- Brush supports Paint and Erase in exact evaluation order.
- Brush uses the required source feather behavior; no approximate blur replaces it.
- Interactive and Quality operate at their specified decode and render quality.
- Detail ROI changes can recompute effective Mask, Union, Grade, and downstream output.
- Leaving an image releases its Brush GPU coverage, distance, and effective-Mask allocations.
- The design does not add a global VRAM cache for inactive images.
- Opening an image with a valid materialized Brush reads and verifies R8 from project storage; it
  does not replay the full sample history.
- Missing, corrupt, stale, or mismatched required Brush materialization reports the real error.
  Render code does not silently rebuild through a different path.
- Undo, Redo, Version checkout, branch creation, recovery, Paste, reopen, and export reproduce the
  same Brush result.
- A user input sequence produces one settled history operation.
- No edit commit or complete document JSON embeds the full Brush sample list.
- Native Brush raster work uses a fixed small number of dispatches per update, not one dispatch per
  dab.

An explicit project repair operation may regenerate materialized data after validating source
objects written by this new architecture. A future format upgrade may start only from a supported
Brush-disabled `0.7.0` project or a released format produced by this plan. Neither operation accepts
the abandoned `0.6.0` Brush parameters. These operations are not part of ordinary rendering and must
report their own result.

---

## 4. Target ownership and representations

Brush uses three representations with separate owners and lifetimes.

```text
BrushModelOwner
  logical state used by PipelineDocument and Mini-Git
  ├─ stable MaskId
  ├─ ordered BrushStrokeRef values
  ├─ placement
  ├─ feather and Mask fields
  ├─ BrushStateHash
  └─ last Brush mutation commit

BrushStrokeStore
  project-owned immutable binary objects
  └─ path/control data or canonical sample data keyed by BrushStrokeBlobKey

BrushRasterStore
  project-owned verified R8 materialization
  ├─ BrushStateHash
  ├─ raster algorithm version
  ├─ canonical descriptor
  ├─ R8 tile-tree root
  └─ independently verified tile/content keys

RenderWorkspace
  one open image/session
  ├─ uploaded canonical R8
  ├─ signed-distance allocations
  ├─ effective ROI Mask
  └─ release on image/session exit
```

The document owner applies changes through focused operations. Callers do not copy the whole Brush,
edit it, and write it back. Immutable stroke bodies are resolved through the project-owned store with
a bounded view lifetime. GUI projection reads metadata and stored bounds; it does not load sample
bodies unless Brush editing requires them.

### 4.1 `BrushStrokeRef`

The logical document stores references instead of bodies:

```cpp
struct BrushStrokeRef {
  StrokeId stroke_id;
  BrushStrokeMode mode;
  BrushStrokeBlobKey blob_key;
  NormalizedRect reference_bounds;
  std::uint32_t point_count;
};
```

The exact fields are finalized in BM1 after measuring formats. Bounds and counts are metadata needed
by ordinary UI, invalidation, reachability, and diagnostics. They are validated against the blob when
it is first read; they are not an independently editable copy.

### 4.2 `BrushStateHash`

The Brush logical-state identity uses a domain-separated canonical hash over at least:

```text
source format version
raster algorithm version
canonical raster descriptor
ordered (StrokeId, mode, BrushStrokeBlobKey)
placement
coverage-affecting Brush fields
```

Raw coverage identity excludes downstream-only feather when feather does not modify stored raw R8.
The final choice is fixed by the exact native evaluation boundary and covered by mutation tests.

The last Brush mutation commit and `BrushStateHash` are stored separately:

- the commit identifies the Mini-Git history position that last changed the Brush;
- the state hash identifies the logical coverage input;
- the raster root identifies materialized R8 bytes.

Unrelated node or Grade commits do not invalidate raw Brush R8.

### 4.3 `BrushRasterMaterialization`

```cpp
struct BrushRasterMaterialization {
  MaskId mask_id;
  commit_hash_t last_brush_commit;
  Hash128 brush_state_hash;
  std::uint32_t raster_algorithm_version;
  MaskAssetDescriptor descriptor;
  BrushRasterRootKey raster_root;
};
```

This is the durable relation between history state and raster bytes. It is published only after the
referenced immutable objects exist. A database row never points to a partially written object.

---

## 5. Binary stroke format

JSON retains only small operation metadata and object keys. Brush path/sample bodies use a versioned,
canonical binary format suitable for hashing, bounds validation, streaming, and compression.

BM1 must compare at least these representations with fixed real input sequences:

- canonical samples using packed absolute values;
- first point plus delta-encoded positions;
- path control points plus deterministic dab expansion;
- UNORM16 strength/hardness versus float32;
- float32, fixed-point, or bounded integer coordinate/radius fields;
- block compression using the project-approved compression dependency, if any.

The selected format must define:

- byte order and exact field widths;
- finite/range validation;
- maximum points and decoded bytes per object;
- stable canonical encoding;
- version upgrade behavior;
- deterministic coordinate and pressure reconstruction;
- reference bounds and point-count verification;
- hash domain and collision handling;
- decompression limits before allocation.

Path fitting and lower input density are allowed only when an approved error bound proves that the
generated canonical dabs preserve the required result. Event frequency must not become persisted
image semantics. The expansion algorithm is versioned so CPU, CUDA, OpenCL, and Metal receive the
same ordered canonical input.

---

## 6. Mini-Git integration

Mini-Git commits reference immutable objects. They do not carry sample bodies.

### 6.1 Append

```text
Finish input
  -> canonicalize and encode one stroke object
  -> BrushStrokeStore::Put
  -> verify returned BrushStrokeBlobKey
  -> owner AppendBrushStrokeRef(NodeId, MaskId, ref)
  -> create AppendBrushStrokeRef change
  -> update BrushStateHash
  -> update native coverage and durable raster root
  -> publish history/materialization state
```

The history change stores `NodeId`, `MaskId`, stable `StrokeId`, evaluation index where required,
mode, and `BrushStrokeBlobKey`. Its inverse removes that exact reference.

### 6.2 Remove and insert

Removing a stroke records the stable identity, immutable object key, and original evaluation index.
Undo reinserts the same reference. It does not embed the removed body a second time.

### 6.3 Checkpoint and root state

The complete PipelineDocument stores ordered stroke references and Brush state identities. It does
not expand them into sample JSON. Pipeline root and image state remain small relative to painted
content.

### 6.4 Branch, Version, and Paste

Branches and Versions share immutable stroke objects and raster-tree nodes. Paste copies reachable
objects to the target project store, remaps `NodeId`, `MaskId`, and `StrokeId`, and preserves object
content keys when the canonical bytes are identical.

### 6.5 Save ordering and recovery

Filesystem objects are published before the DuckDB transaction refers to them. Content-addressed
orphan objects are safe after a failed database transaction and are removed by reachability cleanup.
The database transaction atomically advances history, the complete document, and the Brush
materialization relation. WAL recovery validates every referenced object before accepting the
recovered head.

---

## 7. Copy-on-write tiled R8 materialization

A complete 4096-long-edge R8 file for every Brush mutation is not acceptable. Canonical coverage is
stored as immutable tiles with structural sharing.

Initial design values, subject to BM3 measurement:

```text
pixel format: packed R8 coverage
tile extent: 256 x 256
tile identity: Hash(format, tile coordinate/domain, pixel bytes)
raster identity: root of an immutable tile index
```

One Brush update computes the exact dirty support and publishes only changed tiles. Unchanged tiles
keep their prior keys. A persistent radix tree, B-tree, or Merkle tree shares unchanged index nodes
between history states and branches.

```text
Raster root A
  ├─ unchanged tile groups ─────────────┐
  └─ changed group A                    │
                                       │ shared
Raster root B                          │
  ├─ unchanged tile groups ─────────────┘
  └─ changed group B
```

Undo, Redo, and Version checkout select the matching verified raster root. They do not reverse Paint
or Erase algebra and do not replay every earlier stroke.

The store validates on read:

1. materialization `BrushStateHash` equals the current logical Brush state;
2. the recorded last Brush commit matches the history relation;
3. raster algorithm and descriptor match the evaluator;
4. every loaded index node and tile recomputes to its requested content key;
5. the complete raster root recomputes to the recorded root.

The project owner provides byte count, object count, path, cleanup, reachability, and integrity
diagnostics. It does not use a global temporary directory as required product storage.

---

## 8. Native raster execution

The native algorithm preserves ordered Paint/Erase semantics without one launch per dab.

Target chain:

```text
ordered canonical dabs
  -> build or update tile-to-dab ranges
  -> upload packed dabs and tile ranges
  -> fixed small number of native dispatches
       one workgroup/block owns one dirty tile
       one thread owns one or more texels
       affecting dabs are evaluated in source order
  -> changed R8 tiles
  -> exact signed-distance feather
  -> effective ROI Mask
  -> Union and Grade Mix
```

The implementation may use one dispatch for rasterization and separate fixed dispatches for index
construction or required distance passes. Dispatch count must not grow linearly with dab count.

CUDA, OpenCL, and Metal share:

- canonical packed input;
- tile coverage and dirty-support rules;
- Paint/Erase evaluation order;
- R8 quantization;
- feather equations and boundary behavior;
- failure and publication rules.

Backend code owns resource binding and execution mechanics. No backend selects CPU or another GPU
backend after failure.

---

## 9. Render and GPU lifetime

Disk persistence does not introduce global VRAM retention.

```text
Open image
  -> load small document metadata
  -> resolve current BrushRasterMaterialization
  -> verify required R8 tiles
  -> upload canonical coverage into this RenderWorkspace
  -> build session-local signed-distance data

Leave image
  -> wait for existing readers/submissions
  -> release coverage, distance, and effective-Mask GPU resources
```

Interactive, QualityBase, and Detail ROI obey the existing DAG quality rules. A changed ROI can
recompute effective Mask, Union, Grade, and downstream output. It does not load stroke bodies or
reconstruct canonical coverage.

The whole-DAG performance work delivered by Node Editor NM8 is the baseline for this plan. Brush
must not reintroduce GUI-thread document serialization, whole-parameter hashing, unnecessary static
plan compilation, or unrelated downstream invalidation.

---

## 10. GUI and editing projection

Ordinary node selection, panel layout, viewport resize, ZoomPan, and filmstrip navigation do not
read Brush body objects.

The GUI projection exposes only stable and bounded metadata:

- Brush availability and selected `MaskId`;
- tool state and editable scalar values;
- stroke count and bounds when the approved UI requires them;
- current placement and reference mapping;
- operation/session identity.

Entering Brush editing may acquire the needed immutable stroke/path objects through the owner. The
adapter keeps handles/views with explicit lifetime; it does not receive a full `MaskSource` by value
on every backend notification. Overlay bounds come from validated stroke metadata or a maintained
owner index, not a full sample scan.

---

## 11. Detachable module boundary

Node Editor NM7.13 creates the build boundary before this plan starts. The intended target layout is:

```text
EditMaskCore
  MaskId, common Mask fields, Union, range fields, owner operations

EditMaskAnalytic
  Radial and Linear Gradient model, serialization, history, runtime, UI support

EditMaskBrush       [only when ALCEDO_ENABLE_BRUSH_MASK=ON]
  Brush model references, stores, history changes, materializer, native passes

EditorMaskAnalytic
  Radial and Linear Gradient controller/adapter/QML integration

EditorMaskBrush     [only when ALCEDO_ENABLE_BRUSH_MASK=ON]
  Brush input and editing integration
```

When disabled:

- no Brush `.cpp`, `.cu`, `.mm`, OpenCL source, QML, SVG, translation, test, or benchmark belongs to
  a production target;
- core and analytic public headers do not expose Brush implementation types;
- Mask creation menus and QML type lists contain only Radial and Linear Gradient;
- Brush history kinds and JSON readers are unavailable in the current project schema;
- opening unsupported Brush data fails at the document boundary;
- package manifests contain no Brush resource;
- generated dependency/link maps prove the absence of Brush objects and symbols.

After this plan is complete, enabling Brush adds the two optional modules through narrow registration
interfaces. It must not scatter feature checks through unrelated node, Grade, or rendering code.

---

## 12. Phase summary

| Phase | Status | Result |
| --- | --- | --- |
| BM0 — Extracted-boundary audit and format decision | planned | Re-audit the module boundary produced by NM7.13, measure real Brush payloads, select binary/path and tiled-R8 formats, and fix exact project cutover rules. |
| BM1 — Immutable binary stroke store | planned | Replace sample JSON with validated, bounded, content-addressed binary stroke/path objects and metadata references. |
| BM2 — Mini-Git, recovery, Version, and Paste integration | planned | Store object references in commits and complete documents; prove branch sharing, recovery ordering, reachability, and target-project transfer. |
| BM3 — Verified tiled R8 materialization | planned | Add copy-on-write R8 tiles, structural sharing, dual history/content validation, project-owned storage, cleanup, and integrity reporting. |
| BM4 — Batched three-backend raster execution | planned | Replace per-dab dispatch with ordered tiled raster work on CUDA, OpenCL, and Metal; integrate exact feather and ROI evaluation. |
| BM5 — Brush authoring and bounded GUI projection | planned | Reconnect input, editing, overlay, node drawer, history, and render pacing without whole-source copies or GUI sample scans. |
| BM6 — Product qualification and release cutover | planned | Qualify real RAW, long strokes, Undo/Redo, branches, reopen, Paste, package, corruption handling, latency, storage, and all native backends before enabling the product build. |

Each phase gets a dedicated execution plan only when it starts. An execution plan records exact files,
owner APIs, primary success and failure call chains, tests, fixtures, platform commands, executed
counts, performance distributions, storage measurements, and remaining gaps.

### 12.1 BM0 — Extracted-boundary audit and format decision

Audit current source after NM7.13 with Brush enabled only in an isolated developer build. Capture:

- real point counts and JSON/binary sizes for short, long, dense, Paint/Erase, and moved Brushes;
- current commit, WAL, complete-document, load, GUI, CPU, and native costs;
- the unconditional rejection of `0.6.0` experimental Brush data and the supported `0.7.0`
  analytic-only input to the next format cut;
- exact files and symbols that remain outside the optional module;
- candidate stroke encodings and R8 tile sizes;
- fixed hash domains and version fields.

Exit when the selected formats have byte-level specifications, bounds, failure behavior, and measured
evidence. Do not begin persistent schema implementation with unresolved hash or ownership semantics.

### 12.2 BM1 — Immutable binary stroke store

Implement the project-owned store, canonical encoder/decoder, integrity verification, memory limits,
metadata validation, and owner API. Replace complete sample bodies in the logical document with
immutable references. Prove identical input produces identical keys and malformed input cannot cause
unbounded allocation.

### 12.3 BM2 — Mini-Git, recovery, Version, and Paste integration

Replace Brush history payloads with object references. Update root state, complete document, WAL,
commit hashing, recovery, branch/Version traversal, Paste remapping, package copy, and reachability.
Prove one stroke object is shared across every referencing state and unavailable objects fail before
publishing a new head.

### 12.4 BM3 — Verified tiled R8 materialization

Implement immutable R8 tiles, the persistent tile index, structural sharing, materialization rows,
dual hash validation, atomic publication ordering, crash recovery, cleanup, and storage diagnostics.
Prove changed bytes and object count are proportional to dirty tiles rather than full canvas area or
history depth.

### 12.5 BM4 — Batched three-backend raster execution

Implement and qualify ordered tiled rasterization on CUDA, OpenCL, and Metal. Use independent expected
R8 bytes for Paint/Erase intersections, ordering, hard/soft dabs, quantization boundaries, translation,
large dirty areas, and exact feather. Record fixed dispatch counts and CPU/GPU timing separately.

### 12.6 BM5 — Brush authoring and bounded GUI projection

Reconnect Brush controls only through the optional module. Prove continuous input, cancellation,
selection, overlay mapping, node ownership, history settlement, render pacing, and stale-session
rejection. GUI work during panel resize, ZoomPan, and unrelated backend notification remains bounded
independently of total Brush points.

### 12.7 BM6 — Product qualification and release cutover

Run the enabled and disabled build matrices. The enabled build must pass real-RAW creation, Paint,
Erase, move, feather, multiple Masks, 1,000 operations, 1,000 Undo/Redo transitions, branch/Version,
save/reopen, Paste, export, package, corruption, interrupted publication, storage cleanup, and native
backend qualification. Only then may the product preset change Brush from disabled to enabled.

---

## 13. Acceptance matrix

| Area | Required evidence |
| --- | --- |
| Build boundary | Disabled product configure compiles, links, installs, and packages with no Brush source, symbol, QML, resource, history kind, or test in its build graph. |
| Stroke format | Byte-level canonical encoding, deterministic key, bounded decoder, metadata verification, version rejection, and measured compression. |
| History size | Commit and complete-document size remain bounded by references rather than stroke point count; shared objects are stored once. |
| History behavior | Append/remove/insert/move/feather, Undo/Redo, branch, Version, recovery, and Paste resolve the exact immutable objects. |
| Raster integrity | Last Brush commit, BrushStateHash, descriptor, algorithm version, tile keys, and raster root are independently verified. |
| Raster storage | Dirty updates publish only changed tiles/index paths; unchanged history and branches share objects; reachability cleanup preserves every live state. |
| Native pixels | CUDA/OpenCL/Metal output matches independent expected R8 values and final RGB tolerances without another backend. |
| Native work | Dispatch count is fixed and small per update; work and upload bytes follow dirty tiles and required feather propagation. |
| Open/switch | Valid materialized Brush loads from project storage without parsing stroke bodies or replaying samples. |
| GUI | Filmstrip, panel layout, node selection, ZoomPan, and ordinary Grade edits do not scan or copy Brush bodies. |
| GPU lifetime | Leaving an image releases Brush GPU allocations; reopen reloads verified disk data without a global VRAM cache. |
| Failure | Missing/corrupt/mismatched objects, database errors, native errors, stale work, and interrupted publication return precise errors and publish no invalid state. |

---

## 14. Performance and resource targets

BM0 records baselines and BM6 records final evidence on the same RAW, viewport, backend, build type,
and fixed input sequences. At minimum report p50, p95, maximum, bytes, object counts, and operation
counts for:

- input event to owner consumption;
- owner consumption to Interactive photo presentation;
- release to durable history/materialization publication;
- image open/switch to first frame and QualityBase;
- Brush object read/decode and R8 tile read/verification;
- native bin construction, raster dispatch, feather, Union, Grade, and presentation;
- GUI panel resize and ZoomPan while a large Brush is selected;
- current CPU objects, disk objects, native allocations, upload bytes, and released bytes.

No target can be met by lowering decode resolution, changing quality, reducing required feather,
dropping input, changing Paint/Erase semantics, retaining inactive-image VRAM, or selecting another
backend.

---

## 15. Main risks

### 15.1 Required R8 becomes an unverifiable cache

Prevent this by storing the explicit history relation and independently recomputing content keys on
read. A header containing the expected key is not proof that its following pixels match that key.

### 15.2 Every edit publishes a complete R8

Prevent this with dirty-tile publication and structural sharing. Measure physical bytes, not only
logical object counts.

### 15.3 Binary blobs only move the existing duplication

Prevent this by making commits and documents hold immutable keys, not embedded encoded byte arrays.

### 15.4 Feature checks spread through core code

Prevent this with optional targets and narrow registration seams. The disabled dependency map is an
acceptance artifact.

### 15.5 Concurrent dab writes change Paint/Erase order

Prevent this by assigning deterministic ordered evaluation to each output texel/tile and comparing
exact R8 bytes with an independent implementation.

### 15.6 Materialization and history disagree after failure

Publish immutable objects first, then atomically advance database references. Recovery verifies every
reference before accepting the head. Orphans are safe and later collected.

### 15.7 Old development data silently changes meaning

Use an explicit project-format boundary. Reject `0.6.0` unconditionally; no converter reads its
embedded experimental sample JSON. Any later verified upgrade starts from a supported released
format and records the result.

---

## 16. Global completion criteria

- [ ] The optional Brush modules are isolated behind one approved build option and registration seam.
- [ ] Disabled production builds contain no Brush implementation or product resource.
- [ ] Brush commits and complete documents contain references, not sample arrays.
- [ ] Binary stroke/path objects are canonical, versioned, bounded, verified, and content-addressed.
- [ ] Undo, Redo, branch, Version, recovery, and Paste share immutable objects without body copies.
- [ ] Project-owned R8 materialization validates last Brush mutation, BrushStateHash, descriptor,
      algorithm version, raster root, index nodes, and tile bytes.
- [ ] Changed raster storage grows with dirty tiles, not complete canvas size for every edit.
- [ ] Opening a valid Brush image reads verified R8 without sample replay.
- [ ] CUDA, OpenCL, and Metal use fixed-small-count ordered raster dispatch and exact feather behavior.
- [ ] Ordinary GUI and viewport changes do not load, compare, copy, or scan Brush bodies.
- [ ] Inactive-image Brush GPU allocations are released; there is no global VRAM cache.
- [ ] Required errors are explicit and do not select a lower-quality or alternate execution path.
- [ ] Real-RAW, history, package, integrity, performance, storage, and all native backend evidence is
      recorded before the product build enables Brush.
