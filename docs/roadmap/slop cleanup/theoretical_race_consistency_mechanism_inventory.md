# Theoretical Race Consistency Mechanism Inventory

Date: 2026-09-17

Tracked-source baseline: `c5809cdbe83b`

## Scope and reading rule

This inventory covers first-party editor code and `docs/roadmap/`, with emphasis on version,
generation, revision, request-ID, token, cancellation, and stale-result checks. OAuth values,
persisted file-format versions, Mini-Git identity, explicit user cancellation, and storage recovery
sequences are outside this audit unless they are also presented as UI or worker race protection.

The test applied to every candidate was:

1. Name the two production operations that can execute concurrently.
2. Name the two production events that can arrive out of order.
3. Trace an executable interleaving through the current call chain.

If the call chain did not supply those facts, the entry below records that absence. This document
contains observations and source locations only. It contains no replacement design, implementation
steps, or proposed edits.

The uncommitted NM9.4 implementation was active in a parallel task during this audit. Its new files
and its uncommitted hunks in `editor_node_controller.{hpp,cpp}` were not evaluated. Findings about
the Nodes controller below concern the pre-existing projection, draft, adapter, and command paths.

## Inventory summary

| ID | Area | Observed mechanism | Executable interleaving found |
| --- | --- | --- | --- |
| SC-01 | Nodes projection publication | Session generation plus projection and topology revisions reject older snapshots | No production producer or delivery path for an older snapshot was found |
| SC-02 | QuickQanava adapter | The same three values reject older applies; reverse pointer entries also carry a generation | No out-of-order adapter snapshot delivery was found; old reverse entries are erased or cleared |
| SC-03 | Nodes commands and draft | QML request generation plus a six-field draft identity guard GUI-only edits | No production command carries an older GUI generation to the controller; the explicit stale overload is test-only |
| SC-04 | Adjustment-panel projection | A session generation is stamped on a pulled value and immediately compared with the same backend's current request | No old projection payload is delivered; the production stamp is assigned at read time |
| SC-05 | Roadmap panel state publication | A monotonic snapshot revision and QML last-applied revision were specified for idempotent loading | No concurrency or event reordering was stated; a later roadmap phase rejects this public revision model |
| SC-06 | NM5, NM6.P4, and NM9.1 roadmap text | Multiple generations and revisions are required without naming a runnable ordering failure | No additional interleaving beyond SC-01 through SC-04 was found |

## SC-01 — Nodes projection publications cannot currently arrive out of order

### Observed mechanism

`EditorNodeGraphSnapshot` and `EditorMaskGroupSnapshot` each carry
`session_generation`, `projection_revision`, and `topology_revision`. The controller rejects a
generation mismatch and numerically older revisions before publishing a snapshot.

Sources:

- [`editor_node_graph_projection.hpp`](../../../alcedo_studio/src/include/app/editor_node_graph_projection.hpp#L63-L71)
  defines the three values on `EditorNodeGraphSnapshot`; the Mask Groups value repeats them at
  lines 124–128.
- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L405-L448)
  performs generation and older-revision rejection in `PublishSnapshot`.
- The controller is explicitly GUI-thread-only in
  [`editor_node_controller.hpp`](../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_node_controller.hpp#L28-L39).

### Production call-chain observation

The production refresh path synchronously reads the current identity, current document, and current
session generation, then builds and publishes the projection in the same call:

```text
EditorNodeController::refreshFromSession
  -> session_->pipeline_document()
  -> session_->session_generation()
  -> PublishDocument
  -> EditorNodeGraphProjection::Build / BuildMaskGroups
  -> PublishSnapshot / PublishMaskGroupSnapshot
```

Source:
[`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L691-L709).

A repository-wide call search found no first-party production caller that asynchronously constructs
and later delivers an `EditorNodeGraphSnapshot`. Outside the controller implementation,
`PublishSnapshot` is called by tests. `EditorNodeGraphProjection::AcceptsGeneration` is also called
only by tests.

The controller does defer the visual apply by one GUI event, but the queued closure does not capture
a snapshot. It calls `ApplyBoundGraphIfCurrent`, which reads the controller's current `snapshot_` or
current draft when it runs. Therefore two snapshot payloads are not queued and cannot overtake each
other on this route.

Sources:

- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L528-L552)
  selects the current committed projection or current draft at apply time.
- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L667-L687)
  coalesces deferred applies into one GUI-queue callback.

### Test observation

The stale-generation tests construct states that the production call chain does not produce:

- [`editor_node_graph_projection_test.cpp`](../../../alcedo_studio/tests/app/editor_node_graph_projection_test.cpp#L88-L92)
  compares a manually built generation 15 snapshot with literal generations 15 and 16.
- [`editor_node_controller_test.cpp`](../../../alcedo_studio/tests/ui/editor_node_controller_test.cpp#L360-L370)
  sets the fake backend to generation 12, then directly calls `PublishSnapshot` with a manually
  built generation 11 value.
- The fake backend's
  [`SetGeneration`](../../../alcedo_studio/tests/ui/editor_node_controller_test.cpp#L207)
  changes the request value without the state notification used by the normal session path.

No test cited by this family runs two production snapshot producers, queues two snapshot deliveries,
or demonstrates the later projection arriving before the earlier projection.

## SC-02 — QuickQanava repeats stale checks on a serialized GUI-only apply path

### Observed mechanism

`AlcedoQanGraph` rejects a snapshot when its session generation, topology revision, or projection
revision is numerically older than the applied snapshot. Its reverse node map stores both `NodeId`
and session generation, and `LiveNodeId` checks the stored generation again.

Sources:

- [`alcedo_qan_graph.hpp`](../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/alcedo_qan_graph.hpp#L54-L64)
  states that the adapter and its identity queries are GUI-thread-only.
- [`alcedo_qan_graph.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp#L397-L414)
  implements `RejectIfStale`.
- [`alcedo_qan_graph.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp#L244-L257)
  performs the reverse-map generation check.
- [`alcedo_qan_graph.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp#L592-L595)
  writes the generation into each reverse entry.

### Production lifetime observation

Topology replacement destroys mapped primitives and clears every identity map before inserting the
new topology. Individual node removal erases the reverse entry. An old Qan pointer therefore fails
the map lookup; a found entry with an old generation requires the adapter's own clear/erase invariant
to have already failed.

Sources:

- [`alcedo_qan_graph.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp#L325-L333)
  clears all maps, including `node_from_qan_`.
- [`alcedo_qan_graph.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp#L490-L526)
  destroys and clears before replacement and before restoration after a failed replacement.
- [`alcedo_qan_graph.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/alcedo_qan_graph.cpp#L897-L980)
  erases the reverse entry during individual removal.

The controller's deferred apply uses its current projection as described in SC-01. No separate
adapter queue was found that retains an earlier snapshot value and can deliver it after a newer one.

The adapter test
[`StalePrimitiveCannotSelectOrEditTheNewDocument`](../../../alcedo_studio/tests/ui/alcedo_qan_graph_test.cpp#L595-L609)
does not run a late callback from an old Qan object. It directly calls `ApplySnapshot` with a
manually constructed generation 11 value after generation 12 has already been applied.

### Nearby mechanism with a concrete ordering

`adapter_attach_generation_` is a separate mechanism. Its documented and tested event ordering is:

```text
QueueProjectionApply
  -> Nodes page unloads and set_graph_adapter(nullptr) runs
  -> the queued GUI callback runs afterward
```

Sources:

- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L574-L580)
  advances the attach generation when the adapter changes.
- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L667-L687)
  checks it in the queued callback.
- [`phase_nm5_nodes_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm5_nodes_panel_plan.md#L2975-L2982)
  states the same event ordering.
- [`editor_node_controller_test.cpp`](../../../alcedo_studio/tests/ui/editor_node_controller_test.cpp#L719-L725)
  queues an apply, detaches, then advances the Qt event queue.

This ordering exists for the attach generation. It was not found for the snapshot generation and
revision checks described above.

## SC-03 — Production Nodes commands do not carry the older request generation they reject

### Observed mechanism

The controller exposes a three-argument `requestConnect(source, destination, request_generation)`
and rejects a request generation that differs from `session_generation_`. The ordinary two-argument
entry point supplies `session_generation_` itself. The QML panel calls only the two-argument entry
point, and the Qan connector callback also substitutes the controller's current generation.

Sources:

- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L1215-L1234)
  shows the two overloads and the comparison.
- [`EditorNodesPanel.qml`](../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodesPanel.qml#L258)
  calls `requestConnect(source, destination)` without a captured generation.
- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L1278-L1306)
  routes connector moves and again calls the current-generation overload.
- The only explicit stale three-argument call found outside the implementation is in
  [`editor_node_controller_test.cpp`](../../../alcedo_studio/tests/ui/editor_node_controller_test.cpp#L601-L617).

The test does not queue a request under one session and deliver it under another. It directly passes
the literal value `1` to a controller whose current fake generation is `34`.

### Draft identity observation

`EditorNodeGraphDraftIdentity` copies element, image, Version, session generation, projection
revision, and topology revision. `MatchesIdentity` compares all six fields. Both the draft and its
controller are GUI-thread-only.

Sources:

- [`editor_node_graph_draft.hpp`](../../../alcedo_studio/src/include/app/editor_node_graph_draft.hpp#L20-L29)
  defines the six fields.
- [`editor_node_graph_draft.hpp`](../../../alcedo_studio/src/include/app/editor_node_graph_draft.hpp#L79-L88)
  states GUI-thread-only ownership.
- [`editor_node_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp#L1291-L1321)
  creates and compares the identity from the controller's current scalar values.

The automatic submission sends only `NodeGraphTopologyChange` through
`EditorSessionController::SubmitNodeGraphTopologyEdit`; the submitted value does not carry the
draft's session generation or topology revision.

Source:
[`editor_session_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp#L808-L817).

This differs from the roadmap statement that the live submission request separately carries both
values:
[`phase_nm5_nodes_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm5_nodes_panel_plan.md#L2529-L2535).

The stale-command tests mutate the fake backend's generation without emitting its normal change
notification, then synchronously invoke a controller action. Examples are
[`EndpointsAndStaleGenerationRejectCommandsBeforeBackendMutation`](../../../alcedo_studio/tests/ui/editor_node_controller_test.cpp#L444-L457)
and
[`MaskGroupCommandsRejectAStaleSessionGeneration`](../../../alcedo_studio/tests/ui/editor_node_controller_test.cpp#L1170-L1181).
No queued production Nodes command with a captured old generation was found.

## SC-04 — Panel projection generation is assigned after the projection is read

### Observed mechanism

`EditorPanelProjection` carries `session_generation`. `EditorSessionController::OnBackendChanged`
pulls the projection, pulls the current `SessionEpoch`, rejects a mismatch, and uses the generation
to choose between replacing or merging panel fields.

Sources:

- [`editor_panel_projection.hpp`](../../../alcedo_studio/src/include/app/editor_panel_projection.hpp#L113-L123)
  defines and describes the generation field.
- [`editor_session_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp#L278-L299)
  performs the compare and merge/replace decision.

The production backend does not return a projection value stamped when its fields were produced.
It synchronously reads `ReadPanelProjection`, then overwrites `projection.session_generation` with
the lifecycle's current active image-load request immediately before returning:

Source:
[`editor_session_service.cpp`](../../../alcedo_studio/src/app/editor_session_service.cpp#L627-L639).

The history port serializes the read with its mutex:
[`editor_session_history_port.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp#L295-L300).

Consequences visible in the present call chain:

- There is no independently queued projection payload whose embedded generation can be older than
  the request read by `SessionEpoch`.
- If panel fields did come from an older owner state, assigning the current generation after the
  read would label those fields as current; the equality check does not identify their production
  time.
- The only mismatched values found are injected by fake backends. The production implementation
  assigns the value from the current lifecycle on every read.

The test
[`StaleSessionGenerationLeavesPanelSnapshotUnchanged`](../../../alcedo_studio/tests/ui/editor_session_controller_phase5a_test.cpp#L1341-L1359)
sets the fake backend's active request to 7, then supplies a projection manually stamped 6. It does
not drive the production `EditorSessionService::panel_projection()` implementation.

The roadmap completion record describes this as a cross-thread stale delivery despite the pull and
restamp call chain:
[`phase_nm6p_native_parameter_access_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm6p_native_parameter_access_plan.md#L448-L486).

## SC-05 — The panel snapshot revision roadmap records a protocol with no ordering failure

Phase 6C-7 required an authoritative field snapshot plus a monotonically increasing revision. Its
recorded no-op path used a controller revision and a QML `lastAppliedRevision` check to avoid
reapplying an equal snapshot.

Sources:

- [`phase_6c_mini_git_history_and_pipeline_snapshot_plan.md`](../alcedo_studio/ui/phase_6c_mini_git_history_and_pipeline_snapshot_plan.md#L2408-L2417)
  states the revision deliverable.
- The same file's
  [completion record](../alcedo_studio/ui/phase_6c_mini_git_history_and_pipeline_snapshot_plan.md#L2426-L2455)
  describes equality checks and idempotent reapplication, but does not name two concurrent
  operations or two events that can be delivered out of order.

A later completed roadmap phase explicitly rejects the public snapshot revision and session-wide
generation model, and requires panels to reload from the focused content-change notification:
[`editor_session_command_queue_and_lock_simplification_plan.md`](../alcedo_studio/ui/editor_session_command_queue_and_lock_simplification_plan.md#L836-L895).

Current production QML still has a property named `lastAppliedRevision`, but `loadFromSnapshot`
increments it unconditionally and no production QML code reads it as an admission check:
[`EditorAdjustmentStack.qml`](../../../alcedo_studio/src/ui/alcedo_main/qml/EditorAdjustmentStack.qml#L112-L140).

The current CQ3 qualification tests list `snapshotRevision` as a banned public API name:

- [`editor_session_action_policy_cq3_test.cpp`](../../../alcedo_studio/tests/app/editor_session_action_policy_cq3_test.cpp#L369)
- [`editor_session_cq5_qualification_test.cpp`](../../../alcedo_studio/tests/app/editor_session_cq5_qualification_test.cpp#L194)

This is a roadmap-history mismatch rather than a current asynchronous delivery mechanism.

## SC-06 — Roadmap repetition of the same ungrounded checks

### NM5

NM5 specifies the three-value projection identity, generation rejection, reverse pointer generation,
and a generation-bearing reconnect request:

- [`phase_nm5_nodes_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm5_nodes_panel_plan.md#L510-L594)
- The draft later binds project, image, Version, session generation, projection revision, and
  topology revision in
  [`phase_nm5_nodes_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm5_nodes_panel_plan.md#L2297-L2344).

Those sections state the desired rejection behavior but do not identify two snapshot producers, two
command deliveries, or a queue that can reverse their order. NM5.8a later supplies a concrete event
ordering only for the adapter attach generation, which is recorded separately under SC-02.

### NM6.P4

NM6.P4 requires cross-thread old deliveries to be dropped, then records the panel projection path
as `ReadPanelProjection -> stamp session_generation -> controller discard-or-apply`:

- [`phase_nm6p_native_parameter_access_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm6p_native_parameter_access_plan.md#L438-L468)

The plan does not name a cross-thread delivery callback for this value. The current production path
is the synchronous pull and restamp described in SC-04.

### NM9.1 outside the NM9.4 thumbnail work

NM9.1 requires the owner to recheck session generation, image/Version, command state, and topology
revision before group operations, and rejects an intent after a preceding command changes its
insertion point:

- [`phase_nm9_mask_group_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm9_mask_group_panel_plan.md#L665-L676)

The implemented Mask Group controller methods use `ValidateCommandGeneration` on the GUI-only
controller. The stale tests change the fake backend generation without a normal session change
notification, as shown under SC-03. No group-operation value carrying an older generation through a
queue was found.

### NM9.4 reference only

NM9.4 is not changed or re-evaluated here. Its current plan combines a monotonic `request_id`, panel
binding generation, target identity, content key, weak receiver, subscription cancellation,
`PendingDelete`, and repeated GUI/provider checks:

- [`phase_nm9_mask_group_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm9_mask_group_panel_plan.md#L503-L558)
- Its planned scenario matrix is at
  [`phase_nm9_mask_group_panel_plan.md`](../alcedo_studio/edit/node_mask_editor/phase_nm9_mask_group_panel_plan.md#L1253-L1265).

The parallel simplified NM9.4 task is outside this inventory.

## Candidates checked and excluded from the findings

These mechanisms have a named production concurrency or ordering boundary and are not counted in
SC-01 through SC-06:

| Area | Concrete operations or event ordering | Sources |
| --- | --- | --- |
| Editor render and presentation | Pipeline worker completion can arrive after an image switch; A→B→A can otherwise present the first A frame into the second A session | [`editor_render_coordinator.cpp`](../../../alcedo_studio/src/app/editor_render_coordinator.cpp), [`qml_editor_rhi_unified_workspace_plan.md`](../alcedo_studio/ui/qml_editor_rhi_unified_workspace_plan.md#L1504-L1563) |
| Search preview thumbnails | Thumbnail completion, a conversion thread, and a queued GUI callback can all run after query or visibility changes | [`search_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/search_controller.cpp#L622-L758) |
| Nodes adapter attachment | A GUI apply is queued, the Loader detaches the adapter, then the queued callback runs | Sources listed in SC-02 |
| Scope analysis | GPU/backend analysis completion crosses into a controller that can already be bound to another presented frame | [`editor_scope_controller.cpp`](../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_scope_controller.cpp) |
| Journal, materialization, and saved-log generations | Durable records, compaction, and recovery compare persisted identities and sequences across process interruption | [`editor_history_materializer.cpp`](../../../alcedo_studio/src/app/editor_history_materializer.cpp), [`editor_transaction_journal.cpp`](../../../alcedo_studio/src/edit/history/editor_transaction_journal.cpp) |
| Download and AI task cancellation | A user cancellation request races with a real worker or external process and has user-visible task semantics | [`download_service.cpp`](../../../alcedo_studio/src/app/download_service.cpp), [`ai_sidecar_runtime_service.cpp`](../../../alcedo_studio/src/app/ai_sidecar_runtime_service.cpp) |
