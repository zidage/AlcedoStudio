# Phase NM6 — Node-aware Adjustments and Serial Interactive Rendering

Date: 2026-09-05

Status: NM6.1–NM6.4 complete; NM6.P and NM6.5–NM6.9 planned.

Prerequisites: NM5 is complete. Preserve NM1 single live document/executor ownership,
NM2 multi-Grade execution, NM3 multi-Mask data, and NM4 history/recovery guarantees.

Parent: [Node-aware Pipeline Editing and Mask Authoring](../node_mask_editor_master_plan.md),
Sections 17, 21.7, 23, and 24.

## 1. Approved scope and decisions

本阶段让右侧调整栈实际编辑所选节点，并修正接入过程中发现的调度与缓存基础问题。
2026-09-05 用户审核已确定以下约束；它们取代此前讨论中的并发参数修改、逐帧全参数
内容哈希、为 Undo 保留历史结果的建议。

1. 滑块滑动、渲染侧应用参数、松手提交历史是三个不同操作。UI 持续响应输入；
   输入进入待处理队列，只有渲染侧消费时才修改 live document。
2. 同一 live pipeline 严格执行：应用参数 → 请求/执行本帧 → 本帧完成 → 下一次应用参数。
   不允许本帧渲染与下一批 live 参数修改重叠，不通过整图副本实现隔离。
3. Interactive 的一轮总目标为 16 ms，包括消费、参数应用、失效处理和渲染；
   不是渲染结束后再等待 16 ms。超时等待本帧完成，不重叠执行，不降低质量。
4. CUDA、OpenCL、Metal 共用执行和缓存决策。模板拥有流程，后端只特化具体 GPU 步骤。
5. 会话结果有效性使用变化版本与结果表示条件；不逐帧序列化、哈希整个节点参数体。
6. 每个计算结果在一个工作区只保留当前有效版本。Undo/Redo 请求正常 Quality base，
   不检索旧参数结果，不增加历史结果索引或 Undo 缓存策略。
7. 右侧 scope 下方：左边仅节点名称，右边从上到下为快门、ISO、光圈、焦距，
   中间竖线分隔。没有节点类型副标题、额外说明标题或 Mask 名称副标题。
8. Geometry 仍由文档拥有且作用于整张图像，但编辑入口只属于 Develop；Color Grade 和
   DRT/Post 不显示 Geometry，也不能从其上下文提交 Geometry 修改。
9. 参数读取/面板投射直接使用 Graph Node 中的类型化 Model，不经过 Node/Model 的
   ToJson/LoadJson 中转。保留 Patch/Commit 更新单位；禁止为合并一个 Patch 而把整个
   Model 序列化、复制、合并、再反序列化写回。具体边界见 6.2 节。

### 1.1 Scope exclusions

- 不实现 NM7 的 Brush/Radial/Linear viewer authoring；本阶段测试临时 Mask 更新的运行时入口。
- 不更换 QuickQanava，不新增图结构或节点 Enable/Disable 产品入口。
- 不改变算法、FULL decode、Interactive/Quality 的既定质量策略或 backend 选择。
- 不增加通用任务调度平台、全局显存预算调度器、第二份可写图或独立历史 executor。
- 不按廉价算子数量新增长期图像缓存，不为所有 LLF pyramid 层建立持久缓存。
- 不删除 Mask asset、文件身份等确实需要的内容寻址；只移除会话结果热路径上的全参数哈希。

## 2. Source audit at plan creation

以下为源码事实与风险分析，不是本轮测试通过记录。实施开始时重新确认当前提交和行号。

| Evidence | Current behavior | Required change |
| --- | --- | --- |
| [AdjustmentSlider.qml](../../../../../alcedo_studio/src/ui/alcedo_main/qml/AdjustmentSlider.qml), [adjustment models](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_adjustment_models.cpp) | Position changes call updateDrag; updateDrag applies the UI value and immediately calls submitInteractive. finishDrag submits settled input. | Separate local UI values, queued input, live mutation, and history completion. |
| [session controller](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp), [session service](../../../../../alcedo_studio/src/app/editor_session_service.cpp), [edit controller](../../../../../alcedo_studio/src/app/editor_session_edit_controller.cpp) | submitPatch routes to Patch/CommitAdjustment; HandlePatch calls CaptureAdjustmentBeforePreview before routing a render. | UI acceptance must not synchronously acquire live ownership or mutate document/history. |
| [command queue](../../../../../alcedo_studio/src/app/editor_session_command_queue.cpp) | Owner-thread submissions drain immediately. This queue is not a render-paced input consumer. | Extend the existing session flow with an explicit pending-input boundary; do not equate its current name with the required behavior. |
| [history mutation](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp), [live lock helper](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_shared_helpers.cpp) | Preview capture acquires GetRenderLock and applies the parameter patch. LockLivePipeline performs a blocking lock. | Move the mutation/history ownership step out of pointer handling. Remove GUI waits rather than pumping nested events. |
| [coordinator](../../../../../alcedo_studio/src/app/editor_render_coordinator.cpp) | ScheduleNext stops while inflight exists; pending requests are replaced per quality slot; completion pumps the next request. | Preserve one in-flight frame. Merge input deltas before slot replacement so different fields cannot disappear. |
| [scheduler port](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp), [pipeline scheduler](../../../../../alcedo_studio/src/renderer/pipeline_scheduler.cpp) | Scheduler rejects a second running job. Configuration, Apply and present handoff share render ownership; completion notification is deferred beyond the lock scope. | Reuse this serial boundary; eliminate duplicate parameter application and establish the precise GPU completion requirement. |
| [viewport](../../../../../alcedo_studio/src/ui/editor_rhi/editor_viewport_item.cpp) | Interactive present loop requests QQuick updates. Inspected producer paths do not establish a 16 ms total-budget consumer. | Keep presentation wakeups; add measured producer pacing instead of treating a vsync wakeup as parameter consumption. |
| [parameter target resolution](../../../../../alcedo_studio/src/app/editor_pipeline_command_service.cpp), [adjustment stack](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorAdjustmentStack.qml) | CurrentPanelColorGrade prefers PrimaryGrade; stack loads one session adjustment map. | Use the selected NodeId and exact AdjustmentInstanceId. |
| [result keys](../../../../../alcedo_studio/src/edit/runtime/result_content_key.cpp) | MixGrade/MixDrtPost serialize adjustment JSON; LLF re-traverses upstream grades. LLF shared identity omits some sensor/linearization inputs; its Mask traversal passes no active raster revisions. | Replace session validity with shared dependency invalidation; cover sensor and active Mask changes. |
| [PlanExecutor](../../../../../alcedo_studio/src/include/edit/runtime/plan_executor.hpp), [PassEncoder](../../../../../alcedo_studio/src/include/edit/runtime/pass_encoder.hpp) | Outer execution is templated; Grade and LLF internals still have three implementations. | Move decisions into common Grade/LLF templates and specialize individual GPU operations. |
| [GraphImageCache](../../../../../alcedo_studio/src/include/edit/runtime/graph_image_cache.hpp), [NodeResultCache](../../../../../alcedo_studio/src/include/edit/runtime/node_result_cache.hpp) | Image results retain content identities; LLF buffers/metadata use different paths across backends. | One current result per output; common validity/publication rules for image and LLF results. |
| [operator model base](../../../../../alcedo_studio/src/include/edit/operators/models/operator_model_base.hpp), [ParameterArena](../../../../../alcedo_studio/src/include/edit/runtime/parameter_arena.hpp) | Dirty patches are consumed/cleared; arena already supports field-range updates. | Keep upload dirty state separate from persistent result invalidation. Do not add full parameter copies for version tracking. |

Existing test sources: [coordinator tests](../../../../../alcedo_studio/tests/app/editor_render_coordinator_test.cpp),
[scheduler port tests](../../../../../alcedo_studio/tests/ui/editor_session_render_scheduler_port_test.cpp),
[adjustment model tests](../../../../../alcedo_studio/tests/ui/editor_adjustment_model_test.cpp),
[result key tests](../../../../../alcedo_studio/tests/edit/runtime/result_content_key_test.cpp),
[multi-Grade test support](../../../../../alcedo_studio/tests/edit/runtime/multi_grade_runtime_test_support.hpp),
[Metal LLF tests](../../../../../alcedo_studio/tests/edit/runtime/metal_llf_test.cpp).
Existing source/tests are starting points; their presence is not completion evidence.

### 2.1 External boundary

Official pages checked on 2026-09-05: [Graph data model](https://cneben.github.io/QuickQanava/graph.html),
[node selection](https://cneben.github.io/QuickQanava/nodes.html),
[topology observation](https://cneben.github.io/QuickQanava/advanced.html).
They describe topology, visual items and selection observation. Alcedo retains parameter ownership,
input pacing and history. Verify APIs against the pinned local checkout; documentation examples
do not change Alcedo's Qt Quick Basic style requirement.

## 3. Serial input and frame ownership

### 3.1 Three operations and one writer

```text
GUI pointer/keyboard input
  -> local control value
  -> enqueue minimal typed parameter change / ordered boundary
  -> return to Qt event loop

existing session/render owner, when idle and eligible
  -> take pending changes up to the next boundary
  -> acquire existing live pipeline ownership off the GUI thread
  -> validate complete batch and capture required history before-values
  -> apply live changes once and record affected dependencies
  -> render and safely hand off the resulting frame
  -> finish ownership, publish completion
  -> permit next input batch
```

Use the existing executor and render ownership mechanism. Identify the owner of history state and
its thread-affine collaborators before relocating any call. Queue minimal operations to that owner;
do not move QObject-dependent history code to a worker without adapting its boundaries. A GUI
notification contains only the required read-only presentation fields and follows the completed
owner operation. UI-only selection can remain responsive without reading mutable pipeline state.

The one-writer rule also covers node edits, Undo/Redo, checkout, Paste, RAW settings, image geometry,
Mask parameter updates and active raster updates. They share the owner admission boundary;
background export/save uses the existing NM1 ownership/recipe mechanism. Do not create another
document or executor to evade waiting. The UI queues work instead of waiting on that ownership.

### 3.2 Pending input representation

Reuse EditorAdjustmentPatch/EditorParameterTarget where possible. An input needs session/image
identity, NodeId, AdjustmentInstanceId or explicit document/endpoint owner, changed fields/values,
and an input-sequence identity. It must not contain the entire node/document/parameter collection.
Capture the target at input-sequence start; never resolve it from whatever node is selected later.

- Replace successive absolute assignments to the same target and field within the same sequence
  by the newest value. Merge distinct fields instead of dropping the older entire request.
- Preserve order across noncommuting operations, release, cancellation, selection handoff,
  topology edits, Undo/Redo, checkout and image switch. Relative edits require composition,
  not latest-value replacement. Brush point streams are not scalar replacement candidates.
- A bounded pending map for the current sequence plus ordered boundaries is sufficient;
  do not retain every pointer sample or add a generic event-stream subsystem.
- Queue acceptance means accepted for processing, not committed. Publish final success/failure
  asynchronously and keep the latest local control value from being overwritten by an older echo.
- Rejected/no-op normalized changes do not mark render dependencies or create history entries.

### 3.3 Release, cancellation and selection

First actually applied change captures the original parameter fields in existing NM4 history
representation. Release seals the sequence with its final values. The owner applies any remaining
values, commits once using NM4 failure semantics, and requests Quality base. A sequence released
before any preview still produces its final edit. A release identical to the last preview must
still commit that edit and request Quality; it does not pretend that a second parameter change occurred.

Cancel discards unapplied input and restores any applied provisional fields through the existing
history owner without committing. Request a restore frame only if visible/live content changed.
A failed commit follows NM4 restoration/WAL rules; never report success or leave a partially
applied multi-field mutation. Rendering failure reports the real error, keeps result caches invalid,
and does not silently undo an already durable history commit.

Selecting another node seals the current edit before retargeting future input. Old sequence IDs
cannot write to the new node. A pure selection has no render; a necessary release/restore render
belongs to the old edit, not to selection. Checkout/image switch uses existing lifecycle semantics
and rejects stale queued input. No implicit application of an old delta to a new document.

### 3.4 What completion means

The worker finishing CPU command encoding is not by itself proof that GPU parameter buffers can
be overwritten. Audit PipelineScheduler, renderer, sink, workspace and backend submission leases.
The next owner mutation is permitted only after the prior task no longer reads live parameters
and any reused GPU parameter/output storage is safe under its fence/lease rules. If a backend
still references shared parameter storage, its completion must wait for that use to finish.

Qt can continue presenting an immutable finished image after this boundary. Do not wait for the
monitor's next physical scanout as a parameter-ownership rule. Keep present wakeups and resource
leases; never make the GUI take a lock while the worker needs the GUI to release a present slot.
Completion/error/cancel paths must all release ownership and wake pending work exactly once.

### 3.5 Interactive 16 ms total target

Use a monotonic clock and an injectable clock/wakeup in tests. One Interactive cycle begins when
the owner starts consuming a batch; it ends at the safe completion/handoff boundary above.
Its total includes input application, required history preview bookkeeping, invalidation,
parameter upload, GPU work and necessary handoff waits. Track queue latency and Qt presentation
latency separately so budget accounting cannot hide either behind a different timestamp.

With continuously pending Interactive input, the next start is no earlier than both the prior
safe completion and prior start + 16 ms. A 5 ms cycle has at most 11 ms of pacing remainder;
a 22 ms cycle starts the next batch when complete, with no extra 16 ms delay. Idle input can start
immediately when eligible. Do not manufacture empty frames or replay missed timer ticks.

Release/Quality, Undo/Redo and other non-Interactive work bypass the Interactive pacing wait
after current ownership completes. They never bypass the ownership barrier. Coalesce obsolete
Interactive updates into their final sequence values before Quality, without crossing boundaries.
16 ms is a target and cadence, not permission to interrupt GPU work or lower decode/algorithm quality.

## 4. Runtime invalidation and current-result storage

### 4.1 Owner-maintained validity

Attach runtime invalidation metadata to the existing live document/runtime owner. It contains
only versions and dependency state, not parameter mirrors. Use compiled value/segment identities
and downstream adjacency from the actual execution plan; do not maintain a separate UI dependency graph.

At successful owner mutation, accumulate the precise changed model fields/structure. Before this
batch renders, propagate to affected cached results once. Parameters stay stable until this render
finishes. There is no design for required_revision changing halfway through that frame.

Each result has required_revision and completed_revision, plus its representation descriptor.
Revisions are monotonically assigned runtime values, not content hashes and not persisted history
numbers. Multiple mutations coalesced into one applied batch can share one new change version.
Untouched upstream results retain their version. A workspace compares its completed version with
the owner's required version; consuming GPU upload dirty bits does not erase invalidation.

Descriptors cover all pixel-affecting representation choices: source identity/generation,
document runtime generation, extent/format, reference/crop mapping, viewport where applicable,
sampling/decode/quality requirements and relevant implementation/backend capabilities. Compare
small stable fields or owner-maintained descriptor versions. Do not serialize parameters or hash
image bytes per frame. Immutable prepared-source and asset identities can be computed at load/change.

Version checkout/replacement must establish new runtime identity even if NodeIds repeat. Returning
to an earlier numeric parameter value is a new edit; no search for a previous result is performed.
Topology compilation happens on actual structure/source-layout changes, not ordinary slider values.
Remove hot-path whole-graph traversal used only to rediscover unchanged static identity as well.

### 4.2 Dependency boundaries

```text
sensor/develop content -> geometry/camera color -> upstream Grade output
  -> current pre-LLF segment -> LLF source -> LLF result
  -> current post-LLF segment -> current Mix/Mask -> current Grade output -> downstream
```

Use actual compiled operator order and resource dependencies, not hard-coded panel-name lists.
This is a validity graph; it does not require a persistent image allocation at every arrow.

| Mutation | LLF source in current Grade | LLF result in current Grade | Grade output/downstream |
| --- | --- | --- | --- |
| Pixel-affecting sensor/linearization/develop input | Invalidate | Invalidate | Invalidate |
| Upstream Grade output, including upstream active Brush revision | Invalidate | Invalidate | Invalidate |
| Current pre-LLF adjustment | Invalidate | Invalidate | Invalidate |
| Current LLF Shadows/Highlights | Retain | Invalidate | Invalidate |
| Current post-LLF adjustment | Retain | Retain | Invalidate |
| Current final Mix/Mask where applied after LLF | Retain | Retain | Invalidate |
| UI node selection or EXIF display refresh | Retain | Retain | Retain |

Camera WB/profile, demosaic method, highlight reconstruction, lens settings and linearization
must reach LLF through their real upstream dependencies. Mask union remains dependent on enabled
sources and their geometry; preserve sibling Mask results when only one source changes.
Viewport sampling and canonical reference validity remain separate. Panning must not rebuild a
valid canonical source merely because screen coordinates changed; crop/input changes must not
reuse an incompatible reference. A higher source-detail requirement invalidates insufficient data.

### 4.3 Allocation, failure and cache count

Refactor GraphImageCache to one current result per output in a workspace, with an unpublished
write while replacement is produced. Old buffers can survive only while GPU/presentation leases
require them; release/recycle afterward. No map of previous parameter-content identities.
Retain existing resource budget/eviction behavior for current reusable results. Eviction causes
recomputation with unchanged quality; it does not authorize an alternate execution path.

Publish completed_revision only after the result's producer succeeds and the GPU completion rule
is satisfied. A failed write must not leave newly stamped LLF metadata pointing at incomplete data.
Reuse existing transaction/fence machinery; do not add an independent multi-version publication layer.
Apply the same rule to CUDA buffers, Metal buffers and OpenCL images. A released node removes its
cache entries after leases permit. Background/one-shot renders preserve their existing isolation.

## 5. Shared Grade and LLF execution

Extend the current PlanExecutor/PassEncoder architecture with common Grade and LLF orchestration.
Proposed template names GradeExecutor and LocalToneExecutor are implementation targets, not
claims that those classes already exist. Prefer existing types if they can express these boundaries.

Shared code owns operator segmentation/fusion order, neighbor-operation barriers, ping-pong
destination selection, cache checks, source/result reuse, pyramid layout/traversal, remap/rebuild
order, final mixing, parameter-update decisions and success/error cleanup.

Backend operations expose allocation/binding, dispatch, copy, synchronization and native error
reporting. Do not fully specialize a complete Grade/LLF executor three times. Keep CUDA/OpenCL/Metal
kernels where platform syntax requires it, using the same host parameter definitions and algorithm
constants. Layout differences may change binding/dispatch, never cache decisions or quality.

Persist only current LLF source plane and adjusted result plane. Higher pyramid/remap/reconstruction
levels are scratch with common lifetime rules. A local-tone parameter edit retains source extraction
but rebuilds necessary pyramid/result work. Do not claim all pyramid levels are cached. CUDA moves
to the same source/result split and scratch policy as the common implementation. Preserve current
canonical-reference algorithm and sampling requirements without adding any substitute ROI policy.

Only changed adjustment parameter slots are repacked/uploaded. Audit MakeGradeRuntimeParams and
MakeFullDto callers so removing JSON hashing does not leave a full-node copy loop in the same path.
Use owner-scoped reads/minimal changed fields with existing ParameterArena; no extra parameter mirror.

## 6. Selected-node context and approved UI

EditorNodeController.selectedNodeId remains the sole selection. EditorAdjustmentContext is a
read-only application/QML projection of the selected node's supported parameters and capabilities;
it never owns another mutable NodeId or graph. Reuse the existing selected-parameter presentation
representation, with only the fields needed for UI load. Document its owner, GUI lifetime, atomic
publication and load-only use; a queued GUI projection cannot safely retain mutable worker references.
Never write that projection wholesale back into live state.

| Selection | Panels |
| --- | --- |
| Develop | RAW Decode, Develop-owned controls including WB/lens as appropriate, Geometry |
| Color Grade | Tone, Look, LUT, Masks context |
| DRT/Post | Display, Detail containing Clarity/Sharpen/Halation/Film Grain |

Use one capability/panel-key registry for navigation and body lookup. Keep a supported active page;
otherwise choose RAW, Tone or Display respectively. Geometry stays active only in Develop. Leaving
Develop while Geometry is active resolves the current input boundary and opens the new node's
default supported page. A hidden navigation item alone is insufficient: reject Geometry edits from
non-Develop panel contexts. Retain explicit document ownership in the actual parameter target.
NM6 Masks content identifies the current node's Mask context; no new viewer authoring controls.
An absent adjustment instance must be handled by an explicit owner operation if creation is legal;
never target another node or silently submit by type to the first matching Grade.

### 6.1 Context header

```text
scope area
-------------------------------------------------
                         | Shutter       1/250 s
Color Grade 2            | ISO               100
                         | Aperture        f/2.8
                         | Focal length    50 mm
-------------------------------------------------
supported adjustment navigation
selected panel
```

Values above are illustration only. Actual EXIF comes from the current image metadata owner via
an application read API. Use actual focal length, not the 35 mm equivalent. Format valid rational
shutter times, positive aperture/ISO/focal values with units; missing/invalid values display an em
dash. Keep four stable rows. Node switching must not reread/parse EXIF; image/metadata change updates it.

Left: node display name only, vertically centered, up to two lines, elided with full accessible
name/tooltip. No node kind, current-node prefix or Mask subtitle. Right: four rows in the exact
order shown. The vertical separator is structural. No independent panel fill, badges or status dots.

Map surface/dividers to cardSurfaceColor/cardBorderColor/dividerColor; node name to uiFontFamily,
fontSizeTitle/fontWeightStrong; labels to caption/muted tokens; numbers to dataFontFamily.
Spacing uses existing space tokens. Keep editorSidePanelWidthMin/Max and minimum hit areas.
Any required new header geometry tokens must be added to AppTheme and DESIGN.md together during
implementation. Register new QML in ALCEDO_MAIN_QML_FILES. Preserve Basic style and shared controls.

Geometry is available only in Develop and its body states whole-image scope. Context reloads on initial construction,
session rebind, node selection, panel re-entry, Undo/Redo and checkout. Loading never submits an edit,
restarts an input timer, rebuilds a stable LUT list or triggers photo rendering. UI draft values
belong to their active input sequence; an older completion cannot overwrite them.

### 6.2 Direct typed projection and focused Patch application

Implementation is now a separate prerequisite:
[Phase NM6.P — Native parameter access](phase_nm6p_native_parameter_access_plan.md).
It includes historical evidence, six ordered sub-phases, removal of superseded production paths
and human-readable maintenance criteria. This work must not be absorbed into NM6.6. Section 6.2
defines the desired behavior; NM6.P owns its foundational implementation and production cutover.

User clarification after NM6.4: the concern is repeated full-state JSON conversion, not the
existence of Patch/Commit units. Preserve completed serial queue and dependency-version work.
NM6.2's historical acceptance of field JSON records what landed; it does not authorize a final
Graph Node -> JSON -> Panel read path or whole-Model JSON merge/write-back.

```text
Read: selected NodeId -> application owner scoped Graph Node read
      -> actual adjustment instances / typed Model getters
      -> registered panel adapter -> existing QML presentation models

Write: changed panel value -> targeted Patch -> serial owner consume
       -> validate changed fields -> focused Model update -> existing Commit/history
```

Keep Graph Node/Model classes independent of Qt UI. Adapters resolve actual AdjustmentInstanceIds
and use existing TypeId/field enums and concrete Model APIs such as ScalarOperatorModel::Value().
Extend the single capability/panel registry with typed read/apply adapters, reusing existing
registration where possible. An adapter declares supported node kind, adjustment types/instances,
destination panel model and focused operations. A new panel adds its adapter and capability;
it must not require another global JSON shape/parser or another independent panel-order switch.
Do not build a generic RPC dispatcher, dynamic property bag or new reflection framework.

On node switch, populate all supported panels' required values in one coherent owner read.
Copying necessary UI values is allowed; cloning the node or an entire parallel parameter collection
is not. Use existing presentation models, not a new SelectedNodeState mirror. If asynchronous Qt
delivery requires an independent load-only message, document at its definition the displayed
fields/instance IDs, source owner, consistency boundary, immutable delivery lifetime and release.
Reject an obsolete selection/session message. Do not retain borrowed Model pointers across queued
callbacks, deletion, checkout or rendering. Do not replace JSON with MakeFullDto copies of every
operator followed by another copied panel DTO collection. Core state stays authoritative.

After edits, publish only affected UI fields when needed; preserve newer local input values and
suppress edit signals during loading. loadFromSnapshot may remain a transitional method name,
but production readers must use typed projection, not serialized Model JSON nested in QVariantMap.

New C++ entry points use IDs, field enums and typed values/change structs. Preserve Patch/Commit
as update units. JSON remains valid for persistence, import and external serialized Patch boundaries;
decode such a Patch once into the same typed change. Applying it must not call ToJson on current
Model state, merge a full state copy, and LoadJson the result. Validate related fields before the
complete owner mutation. Keep necessary NM4 undo before/after data, not a full-Model JSON backup
on every provisional update. Queue consumption transfers owned changes instead of copying whole
batches and strings just to pass them onward. A real replace-curve operation may carry the new
curve; moving one point must not read/replace unrelated operator state.

NM6.P owns runtime parameter packing/MakeFullDto cleanup, typed projection, focused owner Patch
application and production reader/writer cutover. NM6.5 then shares Grade/LLF execution decisions;
NM6.6 connects the finished APIs to selected-node context; NM6.7 builds the approved layout.
Preserve project/WAL/history formats. No completed phase is re-labelled as having done this work.

Additional acceptance requirements:

- Node selection loads all supported panels while node/operator ToJson, LoadJson and full DTO
  creation are forbidden in the projection call chain by test instrumentation.
- Scalar and related-field edits use focused owner updates without full-state JSON read/merge/reload.
  Instrumentation separates persistence serialization from projection and parameter application.
- Complex changes preserve unrelated fields and maintain atomic validation/update behavior.
- A new panel test uses the common typed adapter registration without a global JSON parser change.
- Geometry is visible and accepts panel input only in Develop; switching to Color Grade or DRT/Post
  selects the legal default. Its stored document-owned data remains one unchanged representation.

## 7. Ordered implementation phases

NM6.1–NM6.4 are complete. NM6.P is a separate prerequisite before NM6.5–NM6.9. Each phase must leave a buildable product path and
write its actual call chain and evidence into Section 10. New-file names are proposed; existing
links are verified entry points. Do not declare a phase complete based on implementation
inspection alone.

### NM6.1 — Prove input, ownership and completion boundaries

**Changes:** add focused deterministic tests around current slider-to-history calls, one in-flight
render, GUI lock avoidance and GPU-safe completion. Record the failing current behavior before
changing it. Trace current history thread affinity and image/source owner APIs, including EXIF.

**Primary call chain:** AdjustmentSlider → EditorAdjustmentValueModel → session Patch → history
preview mutation → coordinator → scheduler port → PipelineScheduler → renderer/sink → completion.

**Files/APIs:** Section 2 source links; extend existing adjustment model, session, coordinator and
scheduler tests. Use a controllable blocked renderer, completion latch and fake clock; do not use
timing sleeps as proof of ordering. Establish exact cache-hit/reference pixel fixtures for NM6.4/5.

**Acceptance:** tests distinguish UI values from live values and reproduce the unwanted synchronous
mutation path. A written ownership table names each state owner/thread and the safe completion event.
Tests destined to pass after NM6.2/3 stay in a reviewable change together with the fix, not a broken main.

##### NM6.1 completion record (2026-09-05)

**Status:** complete — characterization of the current slider → history → coordinator path.
No pending-input queue, serial consume/pacing, or GPU-safe completion was implemented. Product
behavior is unchanged; tests pass because they record the current synchronous live write.

**Revision:** branch `feature/queued-typed-adjustment-input`, base `ee6247c8`. This branch also
holds the following typed pending-input work so both land in one PR.

**Ownership table (current product, 2026-09-05):**

| State | Owner | Thread | Safe completion / mutation event today |
| --- | --- | --- | --- |
| UI control `value_` | `EditorAdjustmentValueModel` | GUI / QML | `applyValue` writes `value_` then `submitInteractive` before `updateDrag` returns |
| Session command queue | `EditorSessionService` + `QtEditorSessionCommandExecutor` | GUI (`QThread::currentThread() == target_->thread()`). Owner-thread `Submit` drains immediately | command handler returns after `Patch` / `CommitAdjustment` |
| History preview capture | `EditorHistoryMutation` via `EditorSessionHistoryPort` | same thread as `Patch` (GUI in production) | after blocking `LockLivePipeline` (`GetRenderLock`) and `ApplyEditorParameterPatch` |
| Live `PipelineDocument` | `PipelineGuard::document_` under `CPUPipelineExecutor::GetRenderLock()` | the thread that holds the unique lock; adjustment `Patch` takes this lock on the GUI/owner thread | mutex unlock after apply. **Not** gated on coordinator in-flight |
| In-flight render | `EditorRenderCoordinator` + `IEditorPipelineSchedulerPort` | scheduler / render worker | `EditorPipelineScheduleCompletion` is `(bool success, std::string message)`. Production completion is CPU Apply + render-lock release + that callback. **No GPU fence** is part of the callback |
| EXIF display | `Image::exif_display_` (`shutter_speed_`, `iso_`, `aperture_`, `focal_`; not `focal_35mm_`) | image object | not stored on `PipelineDocument` |

`LockLivePipeline` documents that GUI must not take this lock while still required for present;
adjustment `Patch` still does.

**Primary success call chain (current, unwanted for later phases):**

```text
AdjustmentSlider / updateDrag
  -> EditorAdjustmentValueModel.applyValue (UI value_)
  -> submitInteractive / submitNow
  -> EditorSessionController.submitPatch (Q_INVOKABLE, GUI)
  -> EditorSessionService::Patch (owner-thread command queue drains inline)
  -> EditorSessionEditController::HandlePatch
  -> CaptureAdjustmentBeforePreview
  -> LockLivePipeline(GetRenderLock) + ApplyEditorParameterPatch on live PipelineDocument
  -> RouteInitialRender -> EditorRenderCoordinator::Submit
  -> IEditorPipelineSchedulerPort::Schedule (one in-flight)
  -> completion: (bool, string) after CPU Apply / lock release; no GPU fence
```

**Primary failure / blocking call chain:**

```text
GUI Patch while renderer holds GetRenderLock
  -> CaptureAdjustmentBeforePreview blocks in LockLivePipeline
  -> UI callback does not return until the worker unlocks
  -> live document then mutates; coordinator may already have a different in-flight frame
```

```text
HandlePatch / CaptureAdjustmentBeforePreview rejects (unknown field, JSON, missing document)
  -> EditorSessionService::Reject
  -> submitPatch returns false
  -> UI value_ already updated; live document unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| UI value written and live patch submitted before `updateDrag` returns | `EditorAdjustmentModelTest.PointerDragWritesControlValueAndSubmitsLivePatchBeforeReturn` | PASS |
| UI value visible while render lock blocks live write; live mutates before latch completion | `EditorSerialInputBoundaryTest.PointerDragUpdatesControlValueBeforeLiveWriteAndMutatesLiveBeforeRenderCompletes` | PASS |
| Second interactive patch mutates live while coordinator still has in-flight | `EditorSerialInputBoundaryTest.InteractivePatchMutatesLiveDocumentWhileCoordinatorStillHasInflightFrame` | PASS |
| EXIF shutter/ISO/aperture/focal owned by `Image`, not `PipelineDocument` | `EditorSerialInputBoundaryTest.ImageExifDisplayFieldsAreOwnedByImageNotPipelineDocument` | PASS |
| One in-flight until completion latch; clock advance does not complete | `EditorRenderCoordinatorTest.BlockedRendererKeepsOneInflightUntilCompletionLatchReleases` | PASS |
| Owner-thread Patch captures history and routes render before return | `EditorSessionCommandQueueBaselineTest.OwnerThreadPatchCapturesHistoryAndRoutesRenderBeforeReturn` | PASS |
| Patch waits when history capture holds render lock (lock released by owning thread) | `EditorSessionCommandQueueBaselineTest.PatchWaitsWhenHistoryCaptureHoldsRenderLock` | PASS |
| Independent ACEScc expected pixels, packed-plane layout, non-finite fail | `GpuDagRawInputTest.CachedVersusFreshPixelFixture.*` (3 tests) | PASS |

Pixel comparison rule (fixed before later Grade/LLF execution): working space ACEScc RGB ramp
matching `gpu_dag_test::MakeF32RgbaPlane` at 16×12; metric max-abs on R/G/B; absolute tolerance
`1.0e-5f`; relative unused; any NaN/Inf fails. Independent expected values use
`multi_grade_test::ApplyExposureAcescc`.

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/misc/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSerialInputBoundaryTest --target EditorAdjustmentModelTest --target EditorRenderCoordinatorTest --target EditorSessionCommandQueueBaselineTest --target GpuDagRawInputTest
ctest --test-dir build/debug --output-on-failure -R "EditorSerialInputBoundaryTest|EditorAdjustmentModelTest\.PointerDragWritesControlValue|EditorRenderCoordinatorTest\.BlockedRendererKeepsOneInflight|EditorSessionCommandQueueBaselineTest\.(OwnerThreadPatchCapturesHistory|PatchWaitsWhenHistoryCapture)|CachedVersusFreshPixelFixture"
```

Focused suite: 10/10 PASS.

Related binaries (regression scan, not NM6.1 acceptance): 65/66 PASS.
`EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`
fails on `ee6247c8` without these edits (identity stays 30 instead of promoting 50). Out of scope.

**Checklist / exit condition:** all NM6.1 acceptance items met. Latch-blocked scheduler, completion
latch, and injectable monotonic clock are used; wall-clock sleeps are not ordering proof. Desired
later names such as `SliderMovesWhileBlockedRenderLeaveLiveParametersUnchanged` were **not** added
as failing tests.

**LOC note (grill-code-review):** no production source changes. New test support: latch scheduler
~83, manual clock ~23, pixel fixture header/test ~146, serial-boundary binary ~259. Extended
existing model/coordinator/command-queue tests. Largest touched test file remains
`editor_render_coordinator_test.cpp` (~746 lines). No new production type above the split threshold.

**Remaining gaps:** pending-input queue is NM6.2. Serial consume, 16 ms pacing, and GPU-safe
completion (fence/lease) are NM6.3. Node-aware targeting, header UI, and cache-version work remain
NM6.4+. Current `(bool, string)` completion is not GPU-buffer-safe ownership.

### NM6.2 — Queue typed input without GUI live mutation

**Changes:** add the bounded pending-input representation to the existing session flow; capture full
target/sequence identity; update models so UI changes enqueue. Separate accepted versus committed
signals. Merge absolute same-field updates and preserve independent fields/ordered boundaries.

**Primary call chain:** updateDrag/editValue/finishDrag → local model → enqueue typed input/boundary
→ owner wakeup; no CaptureAdjustmentBeforePreview or live lock inside the GUI event callback.

**Files/APIs:** editor_adjustment_models, editor_adjustment_submitter.hpp, editor_session_controller,
editor_session_service/command_queue and their existing headers. Proposed enqueue/sequence methods
replace the ambiguous synchronous result interpretation; keep compatibility inside this change only
where it still follows the approved queue, never as a second production mutation path.

**Acceptance:** with renderer paused, many UI updates return and update controls while live values
remain fixed. Independent parameters survive merging; release and node boundaries are not dropped.
Mouse, keyboard, wheel, reset, enum, toggle, curve and LUT entry paths use the same owner rule.

##### Phase NM6.2 completion record (2026-09-05)

**Status:** complete — GUI typed input enqueues change descriptions; live document and history stay unchanged until NM6.3 consume.

**Snapshot / copy decision (AGENTS.md owner-update rule):** Section 3.2 is a change description, not a live-parameter snapshot, when implemented as session/image ids, `EditorParameterTarget` ids captured at sequence start, the caller's field write payload, and ordered Release/Cancel/NodeSwitch seals. That path is what landed.

Forbidden copies that were not added:

- queueing `EditorRenderAdjustmentSnapshot` or full pipeline/panel `params_json`
- `CaptureAdjustmentBeforePreview` / `ReadEditorParameterJson` / `MakeAdjustmentSnapshotFromLivePipeline` on enqueue
- cloning `PipelineDocument` or `GetParams()` into the queue
- storing history `before_model_json` at enqueue
- using `EditorSessionIntent.adjustment` as the queue item

Allowed and used: `EditorAdjustmentPatch.params_json` as the UI field write payload (including nested ODT/lens/RAW JSON that QML already builds from local control models). That is not a copy of live document state. `PeekPendingInput` copies the queue's own contents for inspection.

Nested ODT/RAW/lens builders still send one field_key's full nested write object assembled from panel models. That pre-existed; NM6.2 stores the same write payload instead of capturing a live param body. History before-values remain NM6.3.

**Primary success call chain:**

```text
updateDrag / editValue / finishDrag / selectIndex / commitValue / curve drag / LUT selectPath
  -> typed model local value
  -> IEditorAdjustmentSubmitter::submitPatch
  -> EditorSessionController (present wakeup only; no Patch/CommitAdjustment)
  -> EditorSessionService::EnqueueAdjustmentInput
  -> EditorPendingInputQueue::AdmitFieldChange
  -> Accepted; live PipelineDocument and history unchanged
```

**Primary failure call chain:**

```text
canEdit false / not Interactive / empty field / missing image identity / in-sequence node retarget
  -> Reject; queue unchanged (or prior sequence preserved)
  -> UI control value may still update locally; live document unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Slider moves while render lock held leave live parameters and full document JSON unchanged | `EditorSerialInputBoundaryTest.SliderMovesWhileBlockedRenderLeaveLiveParametersUnchanged` | PASS |
| Distinct fields survive same-sequence coalescing | `EditorPendingInputTest` + `EditorSerialInputBoundaryTest.PendingDifferentFieldsSurviveInputCoalescing` | PASS |
| Release before consume keeps the final queued write once (commit is NM6.3) | `ReleaseBeforeFirstPreviewKeepsFinalQueuedValuesOnce` (maps 8.1 `ReleaseBeforeFirstPreviewCommitsFinalValuesOnce`) | PASS |
| Node switch seals the original target; later writes start a new sequence | `NodeSwitchKeepsQueuedEditOnOriginalTarget` + session/controller NodeSwitch cases | PASS |
| Value/enum/toggle/curve/LUT/color-temp enqueue without live write | `TypedModelsEnqueueThroughSameOwnerRuleWithoutLiveWrite` | PASS |
| Wheel/keyboard debounce still uses the same submitter seam | `EditorAdjustmentModelTest.WheelBurstSubmitsInteractivePerValueAndOneSettledAfterDebounce` | PASS |
| Enqueue does not capture history or copy `current_snapshot` | `EditorPendingInputSessionTest.EnqueueDoesNotCaptureHistoryOrApplyLivePatch` | PASS |
| Queued item is the changed-field payload only | `EditorPendingInputTest.QueuedItemCarriesOnlyChangedFieldPayload` | PASS |
| Interactive `submitPatch` does not emit `AdjustmentSnapshotChanged` or call Patch | `EditorSessionControllerPhase5ATest.InteractiveSubmitPatchDoesNotEmitAdjustmentSnapshotChanged` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPendingInputTest --target EditorPendingInputSessionTest --target EditorSerialInputBoundaryTest --target EditorAdjustmentModelTest --target EditorSessionControllerPhase5ATest --target EditorSessionCommandQueueBaselineTest
ctest --test-dir build/debug --output-on-failure -R "EditorPendingInputTest|EditorPendingInputSessionTest|EditorSerialInputBoundaryTest|EditorAdjustmentModelTest|EditorSessionControllerPhase5ATest|EditorSessionCommandQueueBaselineTest" -E "RapidImageSelectionKeepsRunningTarget"
```

Focused suite: 90/90 PASS.

Related: `EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection` still fails at identity 30 vs 50. Recorded as pre-existing on NM6.1 (`ee6247c8`); not caused by this enqueue path (`Patch` remains the live-apply API).

**Checklist / exit condition:** NM6.2 acceptance items met. GUI callbacks do not take the render lock or capture history. Same-field replacement is bounded; independent fields and Release/Cancel/NodeSwitch seals are retained. `submitPatch` is not a second live-mutation path.

**LOC note (grill-code-review):** new queue type is 174 header + 188 cpp. Session facade `editor_session_service.cpp` remains 1609 lines and `editor_session_controller.cpp` 1354; NM6.2 only added enqueue methods and rerouted `submitPatch`. Pending-input rules live in `EditorPendingInputQueue`, not a method split of the facade. Largest touched test file `editor_session_controller_phase5a_test.cpp` is 1493 lines (pre-existing concentration plus one NodeSwitch case).

**Remaining gaps:** owner consume, history before/apply/commit, 16 ms pacing, and GPU-safe completion are NM6.3. Production `submitPatch` still does not stamp NodeId/AdjustmentInstanceId; sequence target capture from selection is NM6.6. `Patch`/`CommitAdjustment` still live-apply when called directly so NM6.3 can consume through them. Present-loop wakeup is presentation-only and does not apply queued fields.

### NM6.3 — Consume one batch, render one frame, then consume again

**Changes:** make coordinator admission consume queued changes under the existing off-GUI owner;
move history before/apply/commit to that boundary and eliminate configure-time double application.
Implement Section 3.5 pacing and safe completion. Integrate cancellation, errors and structural edits.

**Primary call chain:** idle/deadline wakeup → consume next batch → validate/history before → apply
once → coordinator/scheduler → locked configure/Apply/handoff → safe completion → next admission.

**Files/APIs:** editor_render_coordinator, editor_session_render_controller/edit_controller,
editor_history_mutation/shared_helpers, scheduler port, renderer/pipeline_scheduler, viewport and
existing sink/workspace completion APIs. Adapt thread-affine history notifications through the session
owner. Document callback ordering and ensure completion cannot reenter admission before inflight setup.

**Acceptance:** fake-clock 5/16/22 ms cycles, no empty ticks, no overlap, no additional 16 ms after
overrun; release bypasses pacing after current completion; first-before/final-after history is correct.
Cancel/exception/hidden viewport cannot strand ownership or deadlock GUI presentation. Quality requests
remain distinct from Interactive; no quality reduction or input loss is accepted to meet timing.

##### Phase NM6.3 completion record (2026-09-05)

**Status:** complete — session owner consumes one pending-input batch, applies/captures/commits once, renders one inflight frame, then admits the next; Interactive uses remaining time inside 16 ms; failed/cancelled cycles and Undo/Checkout do not strand ownership or block the GUI.

**Primary success call chain:**

```text
GUI submitPatch / EnqueueAdjustmentInput
  -> EditorPendingInputQueue::AdmitFieldChange (no live write)
  -> EditorSessionService::RequestPendingInputConsume (PostCompletion)
  -> TryConsumePendingInput
  -> EditorSerialFrameAdmission::TryBeginCycle (Interactive wait or Release bypass)
  -> TakeReadyBatch
  -> EditorSessionEditController::HandlePendingSequence
  -> IEditorHistoryPort::CaptureAdjustmentBeforePreview (document + executor remirror)
  -> CommitAdjustment on Release/NodeSwitch
  -> RouteInitialRender (live_parameters_applied, skip configure re-apply)
  -> coordinator Schedule -> scheduler Complete (Present-wait in production adapters)
  -> NotifyRenderResult -> FinishSerialFrameIfNeeded -> CompleteIfMatches
  -> next TryConsumePendingInput or QTimer deadline wakeup
```

**Primary failure call chain:**

```text
scheduler Complete(false) / Cancelled
  -> NotifySchedulerCompleted emits Failed|Cancelled (ready_count unchanged)
  -> FinishSerialFrameIfNeeded CompleteIfMatches(published=false)
  -> ownership released, Interactive cadence cleared
  -> next queued batch may start immediately
Undo/Checkout while HoldsOwnership
  -> DeferIfLiveOwnershipHeld returns Accepted on the GUI stack
  -> after CompleteIfMatches, deferred owner work runs on the session thread
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ReleaseBeforeFirstPreviewCommitsFinalValuesOnce` | `EditorSerialFrameConsumptionTest`, `EditorSerialInputBoundaryTest` | PASS |
| `InteractiveCycleUsesRemainingTimeWithinSixteenMilliseconds` | `EditorSerialFrameConsumptionTest` | PASS |
| `OverBudgetInteractiveCycleStartsNextOnlyAfterCompletion` | `EditorSerialFrameConsumptionTest` | PASS |
| `QualityReleaseBypassesPacingAfterCurrentFrameCompletes` | `EditorSerialFrameConsumptionTest` | PASS |
| `FailedOrCancelledRenderReleasesOwnerWithoutPublishingResult` | `EditorSerialFrameConsumptionTest` | PASS |
| `GuiRemainsResponsiveWhilePresentNeedsAnUpdate` | `EditorSerialFrameConsumptionTest` | PASS |
| `UndoAndCheckoutWaitForOwnerWithoutBlockingGui` | `EditorSerialFrameConsumptionTest` | PASS |
| empty-queue deadline does not invent a frame | `DeadlineWithEmptyQueueDoesNotScheduleAnEmptyFrame` | PASS |
| hidden viewport aborts without stranding ownership | `HiddenViewportAbortsCycleWithoutStrandingOwnership` | PASS |
| NM6.1/6.2 queue cases (`SliderMovesWhileBlocked…`, coalescing, node-switch) | `EditorSerialInputBoundaryTest` / `EditorPendingInputTest` | PASS |
| first-before / final-after on live Mini-Git document | `EditorSerialInputBoundaryTest.ReleaseBeforeFirstPreviewCommitsFinalValuesOnce` (Undo restores default EV) | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPendingInputTest EditorPendingInputSessionTest EditorInteractivePacingTest EditorSerialFrameAdmissionTest EditorSerialFrameConsumptionTest EditorSessionEditControllerTest EditorAdjustmentPipelineTest EditorSerialInputBoundaryTest EditorSessionHistoryPortTest
ctest --test-dir build/debug --output-on-failure -C Debug -R "EditorSessionHistoryPortTest|EditorSerialInputBoundaryTest|EditorAdjustmentPipelineTest|EditorSessionEditControllerTest|EditorPendingInputTest|EditorPendingInputSessionTest|EditorInteractivePacingTest"
ctest --test-dir build/debug --output-on-failure -C Debug -R "EditorSerialFrameAdmissionTest|EditorSerialFrameConsumptionTest"
```

Suite totals: `EditorSessionHistoryPortTest` 75/75 PASS; remaining focused (pending input, pacing, edit controller, adjustment pipeline, serial input boundary) 48/48 PASS; serial admission+consumption 14/14 PASS after failed-cycle pacing fix. Combined 137/137 PASS.

**Checklist / exit condition:** all NM6.3 acceptance items above have executed tests. Section 8.1 names are implemented (NM6.2 queue names remain in the existing harnesses).

**LOC note (grill-code-review):** `editor_session_service.cpp` is 1785 lines. Consume routing stays on the facade; `EditorSerialFrameAdmission` (~128 lines) owns cycle ownership, 16 ms cadence, deadline arming, and deferred Undo/Checkout. History remirror/restore stays in `EditorHistoryMutation`. Edit-controller batch apply stays in `EditorSessionEditController`.

**Remaining gaps:** GPU-safe completion is the existing Present-wait (`cudaStreamSynchronize` / Metal `waitUntilCompleted` / OpenCL presentation events) plus coordinator `NotifySchedulerCompleted`; no new fence type. Multi-field Release still commits per field (a later WAL failure restores unsettled preview). Production `submitPatch` still does not stamp NodeId/AdjustmentInstanceId (NM6.6). Cache versions are NM6.4. Pixel/backend qualification is NM6.9. Debug-build Interactive cost is not a product quality change.

### NM6.4 — Replace session parameter hashes with dependency versions

**Changes:** add owner-maintained invalidation metadata and compiled downstream edges; update cache
lookup/publication to Section 4. Remove per-frame full parameter JSON hashing and repeated static-plan
identity construction. Keep immutable source/asset identities. Convert current-result storage and
cover history/structural/Mask mutations, not only panel edits.

**Primary call chain:** owner mutation → changed field/structure marks → coalesced dependency walk
→ PlanExecutor validity check → execute/skip → safe publication of completed version.

**Files/APIs:** pipeline_document/runtime owner, graph_compiler/execution_plan,
static_execution_plan_cache, result_content_key, plan_executor, graph_image_cache/node_result_cache,
operator model mutation APIs and ParameterArena. Reuse owner metadata; no parallel parameter state.

**Acceptance:** only affected results invalidate; no-op/selection keeps valid results; active raster
and all pixel-affecting Develop changes reach downstream LLF; quality/extent mismatch forces correct
recompute; dirty upload consumption cannot clear invalidation; checkout same NodeIds cannot reuse
old-document output. Repeated edits/Undo do not increase retained historical result count.

##### Phase NM6.4 completion record (2026-09-05)

**Status:** complete — session result validity is owner-maintained required/completed revisions plus
representation identity; GPU caches keep one current result per output; PlanExecutor skip/publish no
longer hashes full node parameter JSON on the hot path.

**Primary success call chain:**

```text
owner mutation (SetValue / ReplaceParams / Mask setter / topology / Renderer::SetDocument)
  -> RuntimeInvalidationState::CollectAndPropagate (once per BeginRender)
  -> BindCompiledPlan + CollectStructureChanges + CollectDevelop/Grade/DrtChanges
  -> InvalidateFrom (one change_version; compiled downstream including LLF ports)
  -> PlanExecutor BindValidResult(required_revision, ResultRepresentation)
  -> encode miss path / skip hit path
  -> RecordUnpublished(revision) -> PublishResults
  -> CompleteMatchingImages / MarkCompleted
```

**Primary failure call chain:**

```text
encode / upload / present failure
  -> CancelRender / DiscardUnpublished
  -> required stays ahead of completed; unpublished writes are dropped
  -> retry BindValidResult cannot treat partial LLF metadata as current
GPU upload TakeDirtyPatch failure
  -> dirty bits restored; CollectAndPropagate still does not consume operator dirty
checkout / new PipelineDocument pointer
  -> AdvanceDocumentEpoch bumps every required revision
  -> same NodeIds miss until the new generation is published
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| dirty consume leaves required ahead; no-op SetValue stays valid | `RuntimeInvalidation.*` in `GpuDagRawInputTest` | PASS |
| pre-LLF / LLF / post-LLF / Mix reuse matrix | `RuntimeInvalidation.PreLlfExposure…`, `LlfSlider…`, `PostLlfSaturationAndMix…` | PASS |
| WB / demosaic / highlights / lens reach LLF | `RuntimeInvalidation.WhiteBalance…`, `Demosaic…`, `HighlightReconstruction…`, `LensCorrection…` | PASS |
| middle Grade; sibling Mask; active raster | `MiddleGradeEditLeavesUpstreamValid`, `SiblingMaskSourceStaysValidWhenOneMaskChanges`, `ActiveRasterRevisionInvalidatesOnlyThatMaskSource`; `CudaMultiGradeFixture.MiddleGradeEditReusesUpstreamResults`; `CudaMaskFixture.OneMaskEditReusesSiblingAndUpstreamResults` | PASS |
| viewport vs crop; render_scale; Export quality | `ViewportChangeKeepsCanonicalLlf…`, `CropChangeMismatchesCanonicalLlf…`, `RenderScaleMismatch…`, `ExportQualityMismatch…` | PASS |
| checkout / document pointer cannot reuse old output | `DocumentEpochPreventsReuseOfSameNodeIds` | PASS |
| repeated edits do not grow retained result count | `RepeatedEditsDoNotGrowTrackedValueCount`; `CudaWorkspaceFixture.ResultCacheDoesNotTreatReusedTextureAllocationAsContentHit` (`PublishedCount()==1`) | PASS |
| new workspace after dirty already consumed still assigns required | `FreshStateAssignsRequiredWhenOperatorDirtyAlreadyConsumed`; `OpenClDevelopFixture.RgbDngWarpProducesFinalSensorImageAndReusesPublishedCache` | PASS |
| structure: add Grade / remove Mask | `AddedGradeLeavesUpstreamValidAndInvalidatesDisplay`; `RemovingMaskInvalidatesGradeOutputNotDevelop`; `CudaMultiGradeFixture.ReconnectChangesNoncommutingGradeResult` | PASS |
| failed write does not publish; retry cannot use unpublished | `CudaWorkspaceFixture.FailedSubmissionDoesNotPublishResultRevision`; `UnpublishedWriteIsNotValidUntilPublish`; `CudaResultCacheProductFixture.RendererFailureDoesNotPublishUnfinishedRevisions`; `OpenClGradeFixture.OpenClLlfFailedSubmissionDoesNotPublishReference` | PASS |
| LLF slider retains source, rebuilds result (GPU) | `OpenClGradeFixture.OpenClLlfSliderEditReusesCanonicalReference` | PASS |
| image switch: one current GPU result, prepared-source hit | `CudaResultCacheProductFixture.ImageSwitchBackReusesMatchingPreparedSourceAndGpuResults` | PASS |
| display-name / selection does not invalidate | `DisplayNameDoesNotInvalidateResults` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest --target GpuDagCudaWorkspaceTest --target GpuDagCudaDrtProductTest --target GpuDagCudaPrimaryGradeTest --target GpuDagCudaMaskTest --target GpuDagCudaDevelopTest --target GpuDagOpenClWorkspaceTest --target GpuDagOpenClGradeTest --target GpuDagOpenClDevelopTest --target EditorPipelineCommandServiceTest
build/debug/alcedo_studio/tests/edit/GpuDagRawInputTest_runtime/GpuDagRawInputTest.exe
build/debug/alcedo_studio/tests/app/EditorPipelineCommandServiceTest_runtime/EditorPipelineCommandServiceTest.exe
build/debug/alcedo_studio/tests/edit/GpuDagOpenClWorkspaceTest_runtime/GpuDagOpenClWorkspaceTest.exe
GpuDagCudaWorkspaceTest.exe --gtest_filter=*ResultCache*:*FailedSubmission*:*CancelledSubmission*:*UnpublishedWrite*:*SinkFailure*:*RepeatedNodeRemoval*:*DoNotInclude*:*InstantiatesCuda*
GpuDagCudaDevelopTest.exe --gtest_filter=*RgbDngWarp*
GpuDagOpenClDevelopTest.exe --gtest_filter=*RgbDngWarp*:*OpenClSecondDevelop*
GpuDagCudaDrtProductTest.exe --gtest_filter=*ImageSwitchBack*:*FailedSubmission*:*RendererFailure*
GpuDagCudaPrimaryGradeTest.exe --gtest_filter=*GradeWithoutPrimary*:*Reconnect*:*EmptyMask*:*MultiGrade*
GpuDagOpenClGradeTest.exe --gtest_filter=*Llf*:*GradeWithoutPrimary*:*Reconnect*
GpuDagCudaMaskTest.exe --gtest_filter=*Sibling*:*EmptyMask*
```

Suite totals: `GpuDagRawInputTest` 106/106 PASS; `EditorPipelineCommandServiceTest` 10/10 PASS; `GpuDagOpenClWorkspaceTest` 23/23 PASS; focused CUDA/OpenCL GPU slices 41/41 PASS. Combined 180/180 PASS. Metal execution was not run (Windows host).

**Checklist / exit condition:** NM6.4 acceptance items above have executed tests. Section 8.2 three-Grade real-RAW cached-versus-fresh pixel matrix remains NM6.9. Shared Grade/LLF host executors remain NM6.5.

**LOC note (grill-code-review):** new `runtime_invalidation.cpp` ~381 lines and `runtime_invalidation.hpp` ~163 lines own validity; `graph_image_cache.hpp` ~366 lines stays one cache type (rewritten in place for revision+representation lookup). `plan_executor.hpp` ~277 lines. Validity tests live in `runtime_invalidation_test.cpp` ~486 lines. No file crossed 1000 lines.

**Remaining gaps:** `HashLlf*` / `MixGrade` / `BuildFrameResultContentKeys` remain for identity tests and are not used by PlanExecutor or GPU LLF passes. OpenCL signed-distance metadata still stamps `completed_revision` 1 and matches via `ResultRepresentation.identity`. Camera-profile dirty is still lumped into `DevelopDirty::WhiteBalance`. Metal LLF/mask sources were updated with the same revision API and were not executed here. Shared three-backend Grade/LLF orchestration is NM6.5. Node targeting is NM6.6.

### NM6.P — Native parameter access prerequisite

**Status:** planned. Execute the [separate NM6.P plan](phase_nm6p_native_parameter_access_plan.md).
Its scope is direct Model read/update, minimal queued changes, typed panel projection, runtime
parameter packing and deletion of old full-state JSON/DTO intermediary paths. Preserve NM6.2–4
queue, pacing and invalidation behavior. This is an independent acceptance gate, not an extra list
of tasks inside NM6.6. It must finish before shared-executor/context implementation starts.

### NM6.5 — Share Grade and LLF decisions across all three backends

**Prerequisite:** NM6.P complete. Use its parameter read/packing APIs; do not refactor parameter
transport or UI projection again as part of backend orchestration.

**Changes:** introduce common template orchestration and per-step backend operations; unify LLF
source/result reuse, scratch policy and publication. Reuse NM6.P parameter packing. Remove replaced independent
decision loops in the same phase. Maintain present submission/resource ownership guarantees.

**Primary call chain:** PlanExecutor<Backend> → GradeExecutor<Backend> → LocalToneExecutor<Backend>
→ specialized GPU operation → common success/cleanup → current-result publication.

**Files/APIs:** pass_encoder/adjustment_runtime and CUDA, OpenCL, Metal primary_grade/local_tone
sources; shared host parameters and CMake source registration where needed. Follow OpenCL program
registry and Windows build skills when their respective code is touched. Shader mathematics stays
equivalent; shared host logic must not select a different algorithm per backend.

**Acceptance:** one common decision trace for equivalent inputs/edits; source-only versus result
rebuild tests on all three backends; failed LLF work does not publish reusable metadata; scratch
released at safe completion. Pixel output meets declared tolerances against independent expected
behavior and fresh execution. Backend-native submission APIs may differ, decisions may not.

### NM6.6 — Resolve context and exact node-owned edits

**Prerequisite:** NM6.P and NM6.5 complete. This phase connects selection and capabilities to the
already implemented typed readers/writers. It does not own the cross-layer parameter refactor.

**Changes:** implement read-only EditorAdjustmentContext, selected-target resolution and one panel
capability registry. Reuse NM5 selection restoration and NM4 exact history targets. Provide focused
application reads of selected parameters and current image's four EXIF fields.
Connect the Section 6.2 APIs delivered by NM6.P. Correct NodeId alone does not satisfy NM6.6
if this new integration reintroduces whole-state JSON projection or merge/write-back.

**Primary call chain:** EditorNodeController.SelectionChanged → owner scoped Graph Node/typed
Model read → registered panel adapter → coherent GUI values; targeted Patch → NM6.2 queue
→ NM6.3 owner → focused Model update → existing Commit/history.

**Files/APIs:** editor_node_controller, editor_session_controller/service,
editor_pipeline_command_service, editor_adjustment_pipeline, image metadata application API;
new context implementation/header if needed. Reuse Image/ExifDisplayMetaData owner access instead
of cloning image_controller JSON parsing into every panel load.
Use NM6.P getters/update operations and panel adapters. Missing foundational coverage blocks this
phase and returns to NM6.P acceptance; do not hide another broad parameter migration here.

**Acceptance:** two Grades with the same operator type edit independently; no implicit PrimaryGrade
target in panel submit/read; missing/wrong-owner instance fails explicitly. Selection does not
render or commit; queued old-target input cannot land on the new selection. EXIF is image-scoped.
Section 6.2 JSON/copy instrumentation passes. Geometry capability belongs only to Develop while
its actual target remains document-owned. Preserve Patch/Commit as the update unit.

### NM6.7 — Build node-name/EXIF header and capability-filtered panels

**Changes:** implement Section 6 header in the existing stack; bind navigation/body to the registry;
separate Develop, Grade, DRT/Post controls; expose Geometry only in Develop; retain Mask context
without NM7 authoring. Integrate sequence-aware UI values and typed load-only restore from Section 6.2.

**Primary call chain:** typed context → header/EXIF + supported navigation → panel presentation
setters; user edit alone enters the targeted Patch queue. No full-state JSON projection adapter.

**Files/APIs:** EditorAdjustmentStack.qml, EditorTonePanel.qml, Look/LUT/RAW/Display/Geometry
components, any new Detail/header component, AppTheme, DESIGN.md, alcedo_main/CMakeLists.txt.
Use alcedo-qml-ui and qt-qml skills for implementation; do not introduce new QML style conventions.

**Acceptance:** production QML tests at 260/320/460 px, long names, missing EXIF, enlarged text,
reduced motion and both themes. Names and EXIF remain legible without covering navigation.
Node switches keep compatible pages; LUT selection/scroll survives re-entry without submitting.

### NM6.8 — Verify history, lifecycle and end-to-end routing

**Changes:** complete boundary integration and remove old synchronous panel mutation routes;
cover release, cancel, rapid node switching, delete selection, Undo/Redo, checkout, Paste, close/reopen.
Update automation/UI harnesses that previously equated pointer movement with immediate live mutation.

**Primary call chain:** real panel input → owner batch → render → settled history → Undo/checkout
→ owner restore → correct context → Quality base → reopen/replay equivalence.

**Files/APIs:** session/history/selection tests, production QML harnesses, NM4 recovery/real-project
test support, coordinator and scheduler adapters. Do not move failures to NM8 merely because the
reproduction spans UI and runtime.

**Acceptance:** one commit per changed sequence, exact NodeId/AdjustmentInstanceId in history,
valid selection after deletion/re-entry/checkout, unchanged document on pure selection, consistent
save/reopen values, no deadlocks or stale input under pending frames and lifecycle boundaries.

### NM6.9 — Qualify correctness, resource bounds and Interactive cost

**Changes:** run the full relevant test matrix and collect real RAW results on CUDA/OpenCL/Metal.
Record release-build Interactive measurements, queue latency, stage costs, dispatch/skip counters,
source/result rebuilds and retained/scratch bytes. Debug build timing is diagnostic only.

**Primary call chain:** reproducible input replay → production queue/pacing → common executor
→ backend rendering/present → pixel/resource/timing evidence → completion record.

**Files/APIs:** existing multi-Grade/LLF qualification support and tests/perf, production editor
diagnostics, this plan's Section 10. Permanent performance tools go in existing tests/perf;
temporary logs/manifests/images go only in build/tmp/nm6/<phase>/.

**Acceptance:** all mandatory cases in Section 8 pass on applicable platforms. Missing Metal
execution remains incomplete platform qualification, never a Windows pass proxy. Report measured
over-budget cycles honestly; resolve regressions in scope without lowering quality. NM6 is complete
only when functional, cross-backend and lifecycle evidence exists, not when the document is written.

## 8. Required test and performance evidence

### 8.1 Deterministic scheduling and input cases

- SliderMovesWhileBlockedRenderLeaveLiveParametersUnchanged.
- PendingDifferentFieldsSurviveInputCoalescing.
- ReleaseBeforeFirstPreviewCommitsFinalValuesOnce.
- NodeSwitchKeepsQueuedEditOnOriginalTarget.
- InteractiveCycleUsesRemainingTimeWithinSixteenMilliseconds.
- OverBudgetInteractiveCycleStartsNextOnlyAfterCompletion.
- QualityReleaseBypassesPacingAfterCurrentFrameCompletes.
- FailedOrCancelledRenderReleasesOwnerWithoutPublishingResult.
- GuiRemainsResponsiveWhilePresentNeedsAnUpdate.
- UndoAndCheckoutWaitForOwnerWithoutBlockingGui.

Names state required behavior; place cases in existing harnesses where possible. Use worker
latches/fake clocks and assertions on write timestamps/ownership, not arbitrary sleeps.

### 8.2 Pixel and dependency cases

Use at least three Grades and a real RAW fixture plus small deterministic image/parameter cases.
Compare cached execution to fresh execution for each edited state; also use independent expected
operator/pixel cases so a shared bug cannot pass solely because cached and fresh paths agree.
Before execution define metric, color space, absolute/relative tolerance and non-finite handling
from existing backend/operator numerical evidence. Do not invent passing tolerances afterward.

- Edit middle Grade: upstream execution counters unchanged, middle/downstream recompute.
- Edit pre-LLF, LLF, post-LLF, final Mix/Mask: assert the Section 4.2 reuse matrix and pixels.
- Change demosaic/highlight reconstruction/linearization/WB/profile/lens input with stable dimensions:
  assert downstream LLF recompute and cached-versus-fresh agreement.
- Change upstream active Brush revision before asset commit: downstream LLF sees the new pixels;
  unchanged sibling Mask outputs stay reusable.
- Change viewport, crop, decode/quality and source detail: canonical sampling/validity are correct.
- Inject failure after source allocation and before result completion: retry cannot reuse partial data.
- Long edit/Undo sequence: current-result count is bounded by live plan outputs, not edit count;
  account separately for unavoidable outstanding presentation leases and temporary scratch.

### 8.3 UI and persistence cases

Test all three contexts, two same-type Grades, Develop-only Geometry access, EXIF missing/invalid formatting,
read-only loads, selection restore and rapid sequences. Run production Basic QML and real-project
history/reopen tests. Verify parameter echoes never reset an active newer UI value. Scope/EXIF/UI
updates must not generate parameter input or photo renders.

### 8.4 Measurements and commands

Record git revision, hardware/backend/driver, build preset, fixture identity, viewport, decode,
quality, LLF/Grade/Mask counts and warm/cold state. Measure p50/p95/max cycle, enqueue-to-consume,
GUI callback time, CPU preparation/invalidation, GPU work and presentation handoff separately.
Instrument full-parameter serialization/packing calls: session cache validation must perform zero
full-node serialization, and unchanged parameter slots must not be repacked each frame.

Windows commands run from repository root using scripts/msvc_env.cmd. Discover exact test/target
names from CMake/CTest after adding cases; do not assume a source filename is a registered target.

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug -N
ctest --test-dir build/debug --output-on-failure
cmd /c scripts\msvc_env.cmd --preset win_release -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4
```

Implementation records must include the actual focused commands/results before the wider suite.
Use corresponding macOS presets and real Metal execution for platform evidence. A test that skips
for unavailable GPU/fixture is recorded as skipped, not passed. No tests/builds were run when this
planning document was created.

## 9. Delivery order and risk controls

NM6.1 → NM6.2 → NM6.3 → NM6.4 → NM6.P → NM6.5 → NM6.6 → NM6.7 → NM6.8 → NM6.9.
Split reviewable PRs by these engineering boundaries. NM6.1 red-test evidence and its immediate
fix can share a PR. Do not expose node-aware controls until their exact-target serial write path works.

Highest-risk checks are GUI/history thread affinity, GPU parameter-buffer lifetime, lost independent
fields during quality-slot replacement, failure publication of LLF buffers, and incomplete mutation
coverage after removing parameter hashes. Each has a corresponding deterministic or pixel test above.
Cache validity cannot depend on every UI author remembering to call an invalidation helper; owner
operations and compiled dependencies are the enforcement points.

Review touched files for project terminology, data copies, owner boundaries and documentation.
For roadmap changes search the whole roadmap tree and filenames; fix violations in touched prose
and any renamed linked files together. No runtime metadata is written into document serialization.

## 10. Completion records

| Phase | Status | Implementation revision/PR | Actual call chain | Tests/evidence | Remaining work |
| --- | --- | --- | --- | --- | --- |
| NM6.1 | complete 2026-09-05 | `feature/queued-typed-adjustment-input` @ `ee6247c8` | slider → Patch → `LockLivePipeline` + live apply → coordinator; completion `(bool, string)`, no GPU fence | 10/10 focused PASS; see NM6.1 completion record | Queue/pacing/GPU-safe completion are NM6.2/3 |
| NM6.2 | complete 2026-09-05 | uncommitted on `feature/queued-typed-adjustment-input` @ `3a7a3825` | slider/model → `submitPatch` → `EnqueueAdjustmentInput` → `EditorPendingInputQueue::AdmitFieldChange`; live document/history unchanged | 90/90 focused PASS excluding pre-existing RapidImageSelection; see NM6.2 completion record | Consume, 16 ms pacing, GPU-safe completion are NM6.3 |
| NM6.3 | complete 2026-09-05 | uncommitted on `feature/nm6-serial-adjustment-consumption` @ `8ed06f88` | enqueue → PostCompletion consume → HandlePendingSequence → history capture/commit → RouteInitialRender → Present-wait completion → next admission | 137/137 focused PASS; see NM6.3 completion record | Cache versions NM6.4; node targeting NM6.6 |
| NM6.4 | complete 2026-09-05 | uncommitted on `feature/runtime-dependency-result-versions` | mutation → CollectAndPropagate → BindValidResult(required, representation) → skip/encode → RecordUnpublished → PublishResults / MarkCompleted | 180/180 focused PASS; see NM6.4 completion record | Shared Grade/LLF executors NM6.5; Metal GPU execution; Section 8.2 RAW pixel matrix NM6.9 |
| NM6.P | planned | — | — | [Separate execution plan](phase_nm6p_native_parameter_access_plan.md) | Native parameter read/write and production cutover |
| NM6.5 | planned | — | — | — | Shared three-backend execution after NM6.P |
| NM6.6 | planned | — | — | — | Node context/target routing |
| NM6.7 | planned | — | — | — | Approved header and panel UI |
| NM6.8 | planned | — | — | — | Lifecycle/history integration |
| NM6.9 | planned | — | — | — | Cross-platform qualification |

Plan creation record: 2026-09-05, source audit and user-approved design only. NM5 completion is
recorded in the [NM5 plan](phase_nm5_nodes_panel_plan.md). Update this table with implementation,
actual primary call chains, exact commands/results, measurements and unresolved failures as work lands.
