# Nodes panel implementation and simplification review

Date: 2026-09-04. Source baseline: `2bedebdd`; working tree was clean before this review.

Scope: NM5 production panel, controller, draft, projection, adapter, and their NM1–NM4 document,
session-command, history, paste, compiler, and Mask boundaries. This is a source-structure review
plus targeted execution evidence, not renewed numerical qualification of every GPU backend.
Implementation work is specified in [NM5.8a–NM5.8g](phase_nm5_nodes_panel_plan.md#16-nm58--code-simplification-and-final-nodes-page-checks).

## Findings

### R1 — P1: draft editing still performs whole-graph copying and index work

**Style/maintainability; performance mechanism established by source, latency not measured.**
`app/editor_node_graph_draft.cpp:173` copies nodes, edges, node JSON, every working index, and net
delta maps into `Checkpoint` for each admitted mutation. `RemoveEdgeAt` and `InsertEdge` at lines
275–285 each call `RebuildIndexes`; a connection replacing two edges can rebuild three times.
`editor_node_controller.cpp:722` then obtains a complete value snapshot from the draft.
`FinishMutation`, `MaybeSubmitDraft`, and `incomplete_draft` also repeat submission-validity work.

This is the highest-value simplification because the machinery duplicates state and work while
Section 19 promises mutation work proportional to affected entries. Keep a bounded reversal record,
stable edge indexing, one active read view, and one computed validity result (NM5.8b). Full validation
traversal is still allowed. Avoid replacing these structures with another whole-graph context.

**Coverage gap:** `FirstConstructionCopiesTheCompleteProjectionOnce` asserts sizes, identity, and
validity; it counts no copies. `AddInsertsOneDisconnectedGradeWithoutConsumingTheProductCounter`
compares `&draft`, which remains equal even if every member is copied. Add operation/copy counters
and payload-heavy 32-Grade repeated-edit tests; object identity alone does not prove this target.

### R2 — P2: QML and controller duplicate projection, layout, and selection application

**Style/maintainability.** `EditorNodesPanel.qml:110` restores every node position/drawer and applies
selection, duplicating `EditorNodeController::ApplyBoundGraph` at line 387. The panel combines a
`Binding` at line 152 with imperative adapter assignment, `bindGraph`, completion handling, and
queued callbacks from both snapshot and graph signals. The controller independently queues apply
from `PublishSnapshot` and `GraphChanged`. Successful application applies selection more than once.

Consolidate attachment and projection scheduling in the controller, retain layout values in the
layout store, and leave GraphView navigation/focus in QML (NM5.8a). Existing reopen/layout tests
passed, but do not count duplicate apply calls. Add those assertions before deleting update paths.
Do not add a wrapper component merely to move the duplicate logic into another QML file.

### R3 — P2: Qan creation, reversal, delegate caching, and publication have mixed ownership

**Style/maintainability.** `alcedo_qan_graph.cpp` is 1253 lines. `InsertTopology:488` and
`InsertProjectedNode:600` repeat node creation/presentation/map/port work, while the controller's
`ApplyMutationToGraph:732` performs Qan rollback. The adapter owns delegate loading/installation,
primitive lifetime, identity maps, presentation, selection, and connector translation.

Put visual mutation/reversal behind one adapter API and share primitive creation; extract only the
cohesive delegate-library responsibility with its engine, URL, component, and installation state
(NM5.8c). A full replacement still needs a different lifecycle from an incremental edit.
The reverse-pointer map is necessary generation validation, not redundant product data.

**Coverage gap:** test each partial visual mutation and reversal failure. Existing domain
`FailureAfterEachStepRestoresExactState` covers product graph steps, not every Qan operation.
Source inspection alone does not establish a user-visible rollback failure.

### R4 — P2: unused interactive entry candidates coexist with required historical types

**Style/maintainability.** Repository search found production declarations/definitions only for
`EditorSessionController::SubmitAddColorGrade:654`, `SubmitRemoveColorGrade:667`, and
`SubmitReconnectColorGrade:692`; the current node controller uses draft topology submission.
`canReconnectSelectedColorGrade` has no QML consumer and describes the earlier selection policy.

Audit and delete unused upper-layer entry points. Follow each service/queue/port/mutation caller
before deleting the rest. `EditorSessionService:1058–1266` repeats publication/render/revision code
across five node commands; keep a small shared completion helper for retained commands (NM5.8d).

Do **not** delete the old typed payloads: `document_transfer.cpp:357–374` still creates Remove/Add
changes, `pipeline_history_applier.cpp` replays them, and Mask reachability inspects their stored
nodes. Serialized history and active paste producers are concrete retained uses. Rename also has
different render semantics from topology edits and remains a separate command.

### R5 — P2: projection policy is implemented twice

**Style/maintainability.** `EditorNodeGraphDraft::KindOf/ProjectNode:37–70` and
`EditorNodeGraphProjection::Build:37–92` both map model types, names, and ordered Mask rows.
Share the node-to-value conversion in the projection module (NM5.8b). Do not merge their graph
traversals: committed projection uses a valid backbone; draft projection includes detached nodes.
Keep NM3 stored Mask display order distinct from the compiler's deterministic MaskId ordering.

### R6 — P2: topology history completion exceeds the dedicated test evidence

**Coverage gap.** The NM5.7R record acknowledges no separate recovery/reopen/checkout cases for
`EditNodeGraph`, but Section 20 checked off all of them. `EditorSessionNodeCommandTest` uses
`ControllableEditorHistoryPort`; its topology test counts calls/revision/render requests rather
than persisting and replaying a real change. Controller and panel fixtures apply real domain deltas
through replacement session backends. `PipelineHistoryApplierTest` contains general replay tests,
not the full new topology publication/reopen sequence.

The existing persistent suites in `tests/edit/history/editor_session_history_port_test.cpp`,
`editor_version_checkout_test.cpp`, `editor_document_history_test.cpp`, and
`editor_history_materializer_test.cpp` also have no direct `EditNodeGraph`/`NodeGraphTopology`
case. Add the new scenario in a focused history source instead of enlarging the history-port file.

Keep the passing unit tests and add the production-history matrix in NM5.8e. The plan checkbox is
now open with its exact evidence requirement. Also inject post-commit projection failure and
promotion failure around `MaybeSubmitDraft:797`; its separate publication/error path deserves
execution evidence before consolidation. No runtime failure is claimed from reading that path.

## Retained NM1–NM4 responsibilities

- **NM1:** `PipelineDocument` remains writable product state. Current-panel read projection and
  render-lock restoration remain needed; they cannot be deleted merely because Nodes now has a
  separate visual projection. `PipelineMgmtService::CheckoutVersion:1002` owns storage replay and
  restoration, which needs a broader state inventory before any independent service split.
- **NM2:** `graph_compiler.cpp:306` compiles a Grade with explicit NodeId/input/output and
  adjustment ownership. Separate compiled runtime values and UI snapshots serve different readers.
  No evidence from this review justifies merging native backend implementations or removing the
  graph compiler's ownership checks. GPU kernels were not numerically requalified this round.
- **NM3:** `CompileGradeOwnedMaskStack:257` generates Mask execution inputs/Union; the Nodes
  projection emits stored display rows. Sharing these two ordering policies would change behavior.
  MaskStore lifetime, raster requests, and persistent-asset reachability remain distinct duties.
- **NM4:** typed changes, inverse replay, immutable roots, WAL publication, and paste are retained.
  This review inspected current callers instead of treating older roadmap residual notes as proof
  that already-removed migration code still exists. Broader pipeline-service decomposition is not
  a prerequisite for the bounded Nodes cleanup.

## State ownership after simplification

| Owner | Mutable state and responsibility |
| --- | --- |
| `EditorNodeController` | Session/adapter/layout handles and connections; committed snapshot and revisions; image/Version identity; live selection and restoration candidate; command/error state; one queued update; ownership of the draft and submission completion matching |
| `EditorNodeGraphDraft` | Base identity/order/values; editable nodes/edges/counter; lookup and adjacency indexes; net delta; bounded operation reversal; cached validity |
| `EditorNodeLayoutStore` | Per-key positions, view, zoom, drawer values, and saved selection used only for restoration |
| `EditorNodesPanel` | Rename text/id/focus, GraphView lifetime/navigation, and local menu interaction |
| `AlcedoQanGraph` | Graph handle; applied values/revisions; node/edge/port/reverse maps; candidate flags; drawer connections; applied selection; replacement state/counters; local visual reversal |
| `QanDelegateLibrary` | Delegate URLs, engine handle, cached components, and graph-specific installation handles |
| `EditorSessionService` | Existing queue/lifecycle/operation state; typed command routing and success publication; no new generic mutable command context |

The extracted delegate library must be independently constructible and tested. Public module APIs
need Doxygen describing thread affinity, pointer lifetime, mutation/reversal, and failure behavior.
The current QML update helpers and draft index/checkpoint helpers lack comparable explanations;
document the retained operations rather than adding comments to obsolete duplicates.

## Test evidence and acceptance matrix

Executed on Windows against **existing** `build/debug` runtime binaries. No configure/build was
performed, so these results do not establish that every binary was built from `2bedebdd`.
All nine binaries exited 0: **116 tests passed, no reported failures or skips**.

| Target | Passed | What its inspected assertions support |
| --- | ---: | --- |
| `EditorNodeGraphDraftTest` | 8 | Draft topology, cancellation, invalid connect, reversal; not bounded copy work |
| `EditorNodeGraphProjectionTest` | 6 | Projected values, identity, ordered Mask rows |
| `PipelineGraphTopologyDeltaTest` | 5 | In-place forward/inverse and injected domain-step restoration |
| `PipelineEditBatchTest` | 17 | Typed payload validation/serialization and history values |
| `PipelineHistoryApplierTest` | 12 | Apply/inverse, general replay, locking, and asset checks |
| `EditorSessionNodeCommandTest` | 6 | Command routing/revision/render assertions with fake history |
| `EditorNodeSelectionLayoutTest` | 27 | Controller selection/draft behavior and layout values |
| `AlcedoQanGraphTest` | 16 | Adapter projection, incremental primitives, identity/lifetime checks |
| `EditorNodesPanelQmlTest` | 19 | Real QML panel with test backend, draft actions, layout, and errors |

Reproduction commands (run from repository root):

```powershell
& build/debug/alcedo_studio/tests/app/EditorNodeGraphDraftTest_runtime/EditorNodeGraphDraftTest.exe
& build/debug/alcedo_studio/tests/app/EditorNodeGraphProjectionTest_runtime/EditorNodeGraphProjectionTest.exe
& build/debug/alcedo_studio/tests/app/PipelineGraphTopologyDeltaTest_runtime/PipelineGraphTopologyDeltaTest.exe
& build/debug/alcedo_studio/tests/edit/PipelineEditBatchTest_runtime/PipelineEditBatchTest.exe
& build/debug/alcedo_studio/tests/app/PipelineHistoryApplierTest_runtime/PipelineHistoryApplierTest.exe
& build/debug/alcedo_studio/tests/app/EditorSessionNodeCommandTest_runtime/EditorSessionNodeCommandTest.exe
& build/debug/alcedo_studio/tests/ui/EditorNodeSelectionLayoutTest_runtime/EditorNodeSelectionLayoutTest.exe
& build/debug/alcedo_studio/tests/ui/AlcedoQanGraphTest_runtime/AlcedoQanGraphTest.exe
& build/debug/alcedo_studio/tests/ui/EditorNodesPanelQmlTest_runtime/EditorNodesPanelQmlTest.exe
```

Per-target execution output is in untracked `build/tmp/nm5-review/<target>.log`.
No observed test failure was found. Persistent new-topology history, copy/work bounds, full visual
matrix, native screen reader, fresh build, installed package, macOS, and real-RAW remain outside
the demonstrated result. Do not infer them from passing unit or QML-fixture tests.

## Source size and split decisions

All source/test files below have **0 added / 0 removed lines in this review**. Only this report and
the phase plan were edited. Counts include blank lines; large files require responsibility review,
not automatic file chopping.

The phase plan was already 3088 lines, mostly prior sub-phase requirements and completion records.
This review retains those records and splits the new work by responsibility inside Section 16;
the separate review report keeps the evidence discussion out of the execution steps. A later
documentation-only extraction can move completed records with stable links, but is not a code
simplification deliverable.

| File | LOC | Decision |
| --- | ---: | --- |
| `EditorNodesPanel.qml` | 509 | Delete duplicate update work before considering component extraction |
| `editor_node_controller.cpp` | 903 | Move visual reversal to adapter; keep command/projection facade focused |
| `alcedo_qan_graph.cpp` | 1253 | Extract delegate library with its state; consolidate primitive helpers |
| `editor_node_graph_draft.cpp` | 555 | Simplify reversal/index state in place |
| `editor_node_graph_projection.cpp` | 93 | Own reusable node projection |
| `editor_session_service.cpp` | 1679 | Remove obsolete entry candidates and duplicate completion tail; no general rewrite |
| `editor_history_mutation.cpp` | 1055 | Remove only verified-unused node entry methods; retain typed publication owner |
| `pipeline_service.cpp` | 1355 | Separate future load/replay/persistence work needs complete guard/cache/lock inventory |
| `pipeline_edit_batch.cpp` | 1486 | Codec has many variants; a future graph-change codec may own stateless read/write APIs; preserve format now |
| `pipeline_graph_commands.cpp` | 311 | Retain domain operations still used by history/paste |
| `pipeline_history_applier.cpp` | 629 | Retain inverse/replay boundary and add focused topology-history evidence |
| `graph_compiler.cpp` | 497 | Retain runtime compilation boundary |
| `editor_nodes_panel_qml_test.cpp` | 496 | Correct stale names and add lifecycle/apply-count assertions |
| `editor_node_controller_test.cpp` | 504 | Keep controller fixture focused; do not embed persistent storage here |
| `alcedo_qan_graph_test.cpp` | 672 | Extract delegate tests with delegate owner |
| `editor_history_versions_rail_qml_harness.hpp` | 754 | Do not grow into storage/runtime fixture; add focused history fixture |

## Main call chains to preserve

```text
Add/Delete/Connect in QML or Qan connector
  -> EditorNodeController admission
  -> EditorNodeGraphDraft mutation + validity
  -> AlcedoQanGraph incremental visual mutation
  -> incomplete: retain draft, no history/render
  -> complete: SubmitNodeGraphTopologyEdit -> EditorSessionService::EditNodeGraph
     -> EditorSessionHistoryPort -> EditorHistoryMutation
     -> typed batch + in-place product delta + WAL/head publication
     -> one topology Quality render + committed projection promotion

Rename -> controller -> session Rename -> typed name change
  -> publication -> node role update, no photo render

Version checkout/recovery/reopen -> persistent root + typed replay
  -> live document under existing lock/restoration boundary
  -> session notification -> committed projection + layout-key restore

Qan mutation failure -> reverse completed visual steps + reverse draft operation
  -> exact error; no product submission
WAL/publication failure -> product inverse restoration
  -> retain applicable draft + exact error; no topology render
```

NM5.8 stages must record source/test deletions, retained compatibility callers, total/diff LOC,
registration, and executed evidence. Do not count relocation of one class's methods as reduced
ownership or claim all previous NM phases have been requalified by this review.
