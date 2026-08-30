# Phase NM1 — PipelineDocument Editing Foundation

Date: 2026-08-29

Status: in progress — NM1.1–NM1.3 complete; NM1.4–NM1.5 not started.

Branch: `feature/pipeline-document-editing`

Base: `origin/main` at NM0 merge (`d5a96267`, QuickQanava pin already on main).

对应总体方案：[Node-aware Pipeline Editing and Mask Authoring 总体方案](../node_mask_editor_master_plan.md) 第 2.1、6、7.1、11、21.2 节。

本文件是 NM1 的执行方案。总体方案只跟踪一级 Phase；本文件拆 `NM1.1`–`NM1.5`，并记录调用链、测试和未完成项。

---

## 1. 必读

- [总体方案](../node_mask_editor_master_plan.md)
- [GPU DAG 编辑管线重构 Phase 计划](../gpu_dag_pipeline_rebuild_phase_plan.md) 中 format v2、`CreateDefaultPipelineDocument`、`LegacyPipelineImporter`
- 本仓库 `AGENTS.md`：roadmap 用词禁令、测试禁止 `smoke`、临时文件只放 `build/tmp/`

发生冲突时的优先级：

1. 总体方案已锁定的产品语义（单主链、Clean 与 Default 的区别、typed target、失败回滚）。2026-08-30 起，总体方案不再要求旧 stage 项目可打开，也不再要求双向 stage 镜像。
2. 本执行方案对 NM1 子 Phase、完整 typed target、输入序列锁定、以及 thumbnail DAG 的明确要求。
3. 当前代码中已经落地的 GPU DAG 三节点 runtime。

---

## 2. 开工时源码审计（2026-08-29）

### 2.1 双写仍然存在

产品编辑状态不是 `PipelineDocument`。当前写入链：

```text
QML slider
  -> EditorSessionEditController::HandlePatch(field_key)
  -> EditorHistoryMutation::CommitAdjustment
  -> ApplyEditorAdjustmentOperatorState(CPUPipelineExecutor stages)
  -> GPU ApplyGpuDagProduct
       if mirror: LegacyPipelineImporter::ApplyOnto(document, stage JSON)
  -> SavePipeline dirty: Import(stages) 整份替换 document_
```

`LoadPipelineDocument` 注释写明：live CPU stages 是 editor source of truth，format v2 图被 remirror。`SyncPipelineDocument` 只写 `document.ToJson()`，但 dirty `SavePipeline` 会用 stages 重建整张图。

`AllowsLegacyStageAdapterRemirror` 只检查 Develop / `grade.primary` / DRT 三个节点是否存在。

### 2.2 图命令不完整

`PipelineGraph` 只有 `AddNode()` 和 `Connect()`，并公开可变 `Nodes()`。没有断开、删除、桥接或原子 Reconnect。`Validate()` 检查 endpoint 数量、端口类型、环和单端口 fan-in，**不**检查：

- 恰好一条 Develop → … → DRT 的 scene-image 路径；
- scene-image 禁止 fan-out；
- 全部 Color Grade 都在这条路径上；
- Develop / DRT 不可删除、不可替换。

### 2.3 Default 基线不在文档工厂里

`ColorGradeNodeModel::MakeDefault()` 用 catalog 默认值：`ExposureModel` 的 `exposure_ev = 0`，`SaturationModel` 的 `saturation = 1.0`。产品 `+1.5 EV` / 旧 UI `+30` 只存在于 `pipeline_defaults::kCleanBaselineExposure` / `kCleanBaselineSaturation`，靠 remirror 灌进图。

`LegacyPipelineImporter` 的对应关系已经固定：legacy `exposure = 1.5` → `exposure_ev = 1.5`；legacy `saturation = 30` → 新模型 `1.3`（`1 + offset/100`）。

注意：现有 `kCleanBaseline*` 名字是产品 Default 观感，不是总体方案里的 Clean（无视觉变化）。NM1 新 API 不得复用 `CleanBaseline` 表示 identity。

### 2.4 参数没有节点身份

`EditorAdjustmentPatch` 只有 `field_key` + JSON。`HandlePatch` 不碰 `PipelineDocument`。History payload 仍是 `OrdinaryEditPayload`（`OperatorType + PipelineStageName + field`）。这是 NM4 要换掉的格式；NM1 不改 commit schema。

### 2.5 Thumbnail 仍先重建 stage 再渲染

`LoadPipelineSnapshot` 导出 stage JSON，`ImportPipelineParams` 建成独立 `CPUPipelineExecutor`，再 `SetPipelineDocument(..., mirror)`。`ThumbnailService` 把该 executor 交给 `PipelineTask`，`SetForceCPUOutput(true)` 后走 executor Apply。GPU 路径会再次从 stages `ApplyOnto` 文档。这就是「转到 stage 上再渲染」。

分析 rendition 和 export 共用同一 snapshot API。

### 2.6 Compiler 仍按固定 id 找一个 Grade

`GraphCompiler::CompileStatic` 要求 `document.PrimaryGrade()`（`grade.primary`），只编一个 `PrimaryColorGrade` pass。多 Color Grade 存在于图里也不会执行。这是 NM2 的工作。NM1 只把「当前仍只执行一个 Grade」的查找从硬编码 id 改成主链上的 Color Grade 列表，以免删除 `grade.primary` 后文档无法编译。

---

## 3. 本 Phase 锁定的产品决定

这些决定来自开工评审，覆盖总体方案 21.2 的原表述。

1. **新编辑的写入权威是 `PipelineDocument`。** 调整条、history 的 live mutation、thumbnail/analysis 像素都不把 CPU stage 当作渲染或提交的源。
2. **删除能力完整保留。** `RemoveColorGradeAndBridge` 可删除任意 Color Grade，包括 Default / `grade.primary`。不能删除 Develop 或 DRT。删光 Color Grade 后主链为 Develop → DRT，这是合法文档。
3. **产品不保留双向 stage 镜像。** 新编辑只写 `PipelineDocument`。产品 Load / Save / Apply / thumbnail 不以 CPU stage 表为兼容层。进程内 stage 表可以暂时留在代码里，直到后续 Phase 删除整个 stage；NM1 不得为了旧读取器再做 document → stage 或 stage → document 的产品镜像。
4. **Thumbnail 和 snapshot 像素必须 DAG 渲染。** 克隆 `PipelineDocument`，交给现有 GPU `Renderer`。禁止 `ExportPipelineParams` → `ImportPipelineParams` → stage Apply 作为出图像素路径。
5. **History 的 typed `PipelineEditBatch`、Version/Paste、format 升级属于 NM4。** NM1 只要求 live 提交/Undo/Redo 作用在文档上；payload 仍可暂时是 `OrdinaryEditPayload`。NM1 与后续 Phase 都不把旧 mini-git commit 从 stage 身份迁移成 DAG mutation。
6. **不支持 DAG document 之前的项目。** 存储里若没有可用的 format v2 图（`nodes` 与 `edges`），产品拒绝打开，并返回真实错误。不要把旧 stage JSON import 成默认三节点图。不要为了打开旧项目而改写 mini-git commit。一级 Phase NML（旧存储升级）取消。后续 Phase 删除 stage 表，不做旧项目升级。

### 3.1 2026-08-30 产品规则

NM 方案执行期间不为当前 UI 补完整 target。写入路径按最终生产接口设计。缺少 `owner_kind`、`node_id` 或 `adjustment_instance_id` 的 patch 一律拒绝。app 不根据 field catalog 或当前选中节点填入缺省 target。

每个 patch 必须带完整 `EditorParameterTarget`：

- `owner_kind` 为 Document / Develop / ColorGrade / DrtPost 之一（Mask 写入拒绝）
- `field_key` 非空，且与 patch 上的 `field_key` 相同
- Document：`node_id` 为空
- Develop / ColorGrade / DrtPost：`node_id` 非空
- ColorGrade：`adjustment_instance_id` 非空
- `mask_id` 在 NM1 必须为空

输入序列：第一个合法 patch 锁定 target。同一序列的后续 patch 即使带不同 `node_id`，仍写入锁定的 NodeId。每个后续 patch 仍必须自身完整；不完整则拒绝，不靠锁定去补字段。

产品不保留 stage 镜像，也不打开没有可用图的旧存储。NM1.3 把 live 编辑写入文档。NM1.5 阻止 Apply/Save 用 stage 覆盖文档。后续 Phase 删除 stage 表。NML 取消。

---

## 4. 范围与明确排除

### 4.1 NM1 包含

- 候选文档 mutation、主链 validation、原子 Add / Remove / Reconnect / Rename / SetEnabled；
- 稳定 `NodeId`；失败不改 live document / history head；
- `CreateDefaultPipelineDocument` 在工厂内写入 Default 基线；`MakeClean` / `CreateCleanColorGradeNode`；
- `EditorParameterTarget`；每个 patch 必须带完整 target；输入序列锁定 target；缺字段拒绝；
- History live 路径改为写/读文档（payload 格式暂不升级）；
- Thumbnail、analysis rendition、以及同一 snapshot API 上的像素改为 DAG；
- 产品 GPU Apply 以文档为渲染输入；
- `SavePipeline` 不得再用 stages `Import` 整份替换文档；
- 无可用图的旧存储：Load 失败，不 import。

### 4.2 NM1 不包含

| 排除项 | 归属 |
| --- | --- |
| 主链上多个 Color Grade 的真实 GPU 执行、按 NodeId 的 dirty/cache | NM2 |
| Clarity / Sharpen / Halation / Film Grain 搬到 DRT/Post | NM2 |
| 多 Mask、Range 字段落地、不可变 MaskStore `Put()` | NM3 |
| typed `PipelineEditBatch`、每 Version 一 DAG 的 history schema、Paste、format 提升 | NM4 |
| 打开或升级 DAG document 之前的 stage-only 项目；迁移 mini-git commit | 不做（NML 取消） |
| 从代码中删除 CPU stage 表 / `LegacyPipelineImporter` | 后续 Phase，不是 NM1 |
| Nodes 面板、QuickQanava 生产链接、开放用户 Add 入口 | NM5 |
| 按节点切换右侧 adjustment stack | NM6 |
| Viewer 蒙版绘制 | NM7 |

生产 UI 在本 Phase **不**开放新增/删除/重连节点入口。领域 API 和 tests 必须先具备完整删除能力。

---

## 5. 跨子 Phase 的实现规则

### 5.1 候选文档 + 失败回滚

所有图 mutation 和 settled 参数 mutation：

```text
clone PipelineDocument
  -> apply typed mutation on candidate
  -> ValidateGraph + ValidateImageBackbone
  -> 失败：丢弃 candidate；live document、history head、最后一帧不变
  -> 成功：替换 live document
```

不得先改 live 再捕获异常继续。不得用 CPU 或其他 backend 代替失败的 GPU 路径。

### 5.2 主链不变量（在现有 `Validate()` 之上）

合法 scene-image 拓扑：

```text
Develop -> ColorGrade[0] -> ... -> ColorGrade[n] -> DRT/Post
```

或零个 Color Grade：

```text
Develop -> DRT/Post
```

必须拒绝：删 Develop/DRT、替换 endpoint 类型、scene-image fan-in/fan-out、Color Grade 不在主链上、环、端口类型错误。Mask 边仍按现有顶层 mask node 规则验证（NM3 之前不改 Mask 所有权）。

### 5.3 删除与当前单 Grade compiler

NM2 之前 runtime 仍然只执行 **一个** Color Grade。NM1 必须让删除后的文档仍能编译当前形状：

```text
Color grades on image backbone
  0 -> 跳过 PrimaryColorGrade pass，DRT 输入接 Develop 输出
  1 或更多 -> 只编译主链上第一个 Color Grade（按路径顺序，不要求 id 为 grade.primary）
```

其余 Color Grade 保存在文档里，本 Phase 不执行。这不是多 Grade runtime。

`PipelineDocument::PrimaryGrade()` 对默认三节点文档仍可按 `grade.primary` 查找，供现有 tests 使用。Compiler 不得再把「缺少 `grade.primary`」当成硬失败，只要主链合法。

### 5.4 Stage 表与打开规则

产品写入与渲染方向：

| 方向 | NM1 是否允许 | 用途 |
| --- | ---: | --- |
| 新编辑 → `PipelineDocument` | 必须 | 唯一新写入权威 |
| document → stage 表（产品兼容镜像） | 禁止 | 不为旧读取器保留镜像 |
| stage → document 覆盖一次**新的**文档编辑 | 禁止 | 这是当前 Save/Apply remirror 的缺陷 |
| thumbnail/analysis：stage JSON → Import → Apply | 禁止 | 必须 DAG |
| 无图的旧 stage 存储 → Import 成默认三节点后打开 | 禁止 | 拒绝打开，返回真实错误 |

`LegacyPipelineImporter` 可以暂时留在仓库，供现有 GPU DAG 测试使用。产品 Load 不得用它打开用户项目。后续删除 stage 的 Phase 再删除 importer。

NM1.5 仍必须从产品 Apply/Save 中移除 live stages `ApplyOnto` / `Import` 覆盖文档。这是停止双写，不是旧项目兼容。

### 5.5 文件体量

`pipeline_service.cpp` 已经很大。本 Phase 若继续往里堆 Load/Save/snapshot 逻辑，按职责拆出 document persist / snapshot 辅助单元，而不是再加一整段。`EditorPipelineCommandService` 必须是独立类型，不把图规则塞回 `PipelineMgmtService` 或 QML。

---

## 6. 子 Phase 顺序

```text
NM1.1 主链 validation 与图命令（含完整删除）
  -> NM1.2 Default / Clean 工厂
  -> NM1.3 typed target 与参数写入文档（含 history live）
  -> NM1.4 thumbnail / snapshot DAG 渲染
  -> NM1.5 产品写入权威与 Save/Apply 停止用 stage 覆盖新文档
```

同一分支 `feature/pipeline-document-editing` 上按序落地。可以按 `NM1.1+NM1.2`、`NM1.3`、`NM1.4`、`NM1.5` 拆 PR；不要把 NM2–NM8 塞进本分支。

---

## 7. NM1.1 — 主链 validation 与图命令

### 7.1 结果

领域层可以原子地 Add / Remove / Reconnect / Rename / SetEnabled。QML 仍不能改 `Nodes()` / `Edges()`。删除任意 Color Grade（含 `grade.primary`）后文档满足第 5.2 节不变量。Compiler 按 5.3 编译。

### 7.2 API

```cpp
auto AddCleanColorGrade(PipelineDocument& candidate, const NodeId& before_node_id,
                        NodeId new_id) -> std::vector<GraphValidationError>;
auto RemoveColorGradeAndBridge(PipelineDocument& candidate, const NodeId& node_id)
    -> std::vector<GraphValidationError>;
auto ReconnectColorGrade(PipelineDocument& candidate, const NodeId& node_id,
                          const NodeId& new_predecessor_id, const NodeId& new_successor_id)
    -> std::vector<GraphValidationError>;
auto RenameColorGrade(PipelineDocument& candidate, const NodeId& node_id,
                       std::string display_name) -> std::vector<GraphValidationError>;
auto SetColorGradeEnabled(PipelineDocument& candidate, const NodeId& node_id, bool enabled)
    -> std::vector<GraphValidationError>;
```

`RemoveColorGradeAndBridge`：删除该节点及其相邻两条 scene-image 边，再把前驱接到后继。一次调用，不暴露中间图。

`AddCleanColorGrade(before_node_id)`：在 `before_node_id` 之前插入（即新节点成为 `before` 的前驱）。若 `before` 是 DRT，则插在最后一个 Color Grade（或 Develop）之后。具体插入点必须在测试里写死，并与日后 NM5 UI 一致。

节点需要 `display_name`。Develop / DRT 显示名固定，Rename 拒绝。Color Grade 改名不改 `NodeId`。

关掉 `PipelineGraph::Nodes()` 的可变引用。测试用命令或测试 builder 建图。

### 7.3 主成功调用链

```text
test / future EditorPipelineCommandService
  -> ClonePipelineDocument
  -> RemoveColorGradeAndBridge(candidate, node_id)
  -> Validate + ValidateImageBackbone
  -> GraphCompiler::CompileStatic (5.3 规则)
  -> replace live document
```

### 7.4 主失败调用链

```text
Remove Develop or DRT
  or Reconnect that creates scene-image fan-out
  -> candidate discarded
  -> live ToJson hash, node count, history head unchanged
```

### 7.5 文件

- `alcedo_studio/src/include/edit/graph/pipeline_graph.hpp`
- `alcedo_studio/src/edit/graph/pipeline_graph.cpp`
- `alcedo_studio/src/include/edit/graph/graph_validation.hpp`
- 新 `pipeline_graph_commands.hpp` / `.cpp`（或同等名字；命令拥有规则，`PipelineGraph` 不向 UI 暴露容器）
- `alcedo_studio/src/include/edit/graph/pipeline_document.hpp` / `.cpp`（主链遍历、`display_name`）
- `alcedo_studio/src/include/edit/graph/color_grade_node_model.hpp` / `.cpp`
- `alcedo_studio/src/include/edit/graph/i_node_model.hpp`（display name）
- `alcedo_studio/src/edit/runtime/graph_compiler.cpp`（5.3 查找规则）
- `alcedo_studio/tests/edit/graph/graph_validation_test.cpp`
- 新 `alcedo_studio/tests/edit/graph/pipeline_graph_command_test.cpp`
- `alcedo_studio/tests/edit/CMakeLists.txt`（`GpuDagModelGraphTest`）

### 7.6 测试

| 名称 | 断言 |
| --- | --- |
| `AddCleanColorGradeKeepsSingleImageBackbone` | 插入后仍一条 Develop→…→DRT 路径，新 NodeId 稳定 |
| `RemoveColorGradeAndBridgeConnectsPredecessorToSuccessor` | 一次调用后无悬空边；前驱接到后继 |
| `RemovePrimaryGradeKeepsRemainingGradesAndValidBackbone` | 删除 `grade.primary` 后其余 Color Grade 仍在主链，NodeId 不变 |
| `RemoveLastColorGradeLeavesDevelopConnectedToDrt` | 零 Color Grade 合法 |
| `RemoveDevelopOrDrtIsRejectedWithoutMutation` | hash 与节点数不变 |
| `InvalidReconnectLeavesDocumentHashUnchanged` | 非法 fan-out/环不改 canonical JSON |
| `GraphMutationInverseRestoresCanonicalDocumentJson` | forward 再 inverse，canonical JSON 一致 |
| `GraphCompilerSkipsGradePassWhenBackboneHasNoColorGrade` | 零 Grade 时 CompileStatic 成功且无 PrimaryColorGrade |
| `GraphCompilerCompilesFirstBackboneGradeWhenPrimaryIdIsAbsent` | 仅剩非 `grade.primary` 的 Grade 时仍能编译那一个 |

Canonical JSON：对 `ToJson()` 做稳定序列化比较（节点/边顺序确定）。不要用指针或 Qan 对象。

##### Phase NM1.1 completion record (2026-08-29)

**Status:** complete — atomic graph commands, image-backbone validation, const-only `Nodes()`, GraphCompiler first-backbone-grade lookup.

**Primary success call chain:**

```text
test / future EditorPipelineCommandService
  -> ClonePipelineDocument (or mutate a candidate)
  -> RemoveColorGradeAndBridge / AddCleanColorGrade / ReconnectColorGrade
  -> PipelineGraph::Validate + ValidateImageBackbone
  -> GraphCompiler::CompileStatic (0 grades: skip PrimaryColorGrade; else first backbone grade)
  -> replace live document (candidate kept on success)
```

**Primary failure call chain:**

```text
Remove Develop or DRT
  or Reconnect that creates scene-image fan-out
  -> command returns GraphValidationError
  -> candidate restored from ToJson snapshot
  -> live ToJson dump, node count, and history head unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `AddCleanColorGradeKeepsSingleImageBackbone` | `GpuDagModelGraphTest` | PASS |
| `RemoveColorGradeAndBridgeConnectsPredecessorToSuccessor` | `GpuDagModelGraphTest` | PASS |
| `RemovePrimaryGradeKeepsRemainingGradesAndValidBackbone` | `GpuDagModelGraphTest` | PASS |
| `RemoveLastColorGradeLeavesDevelopConnectedToDrt` | `GpuDagModelGraphTest` | PASS |
| `RemoveDevelopOrDrtIsRejectedWithoutMutation` | `GpuDagModelGraphTest` | PASS |
| `InvalidReconnectLeavesDocumentHashUnchanged` | `GpuDagModelGraphTest` | PASS |
| `GraphMutationInverseRestoresCanonicalDocumentJson` | `GpuDagModelGraphTest` | PASS |
| `GraphCompilerSkipsGradePassWhenBackboneHasNoColorGrade` | `GpuDagModelGraphTest` | PASS |
| `GraphCompilerCompilesFirstBackboneGradeWhenPrimaryIdIsAbsent` | `GpuDagModelGraphTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest
ctest --test-dir build/debug -R GpuDagModelGraphTest --output-on-failure
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest
ctest --test-dir build/debug -R "GpuDagRawInputTest.GpuDagGraphCompiler" --output-on-failure
```

Suite totals: `GpuDagModelGraphTest` 42/42 PASS; `GpuDagRawInputTest.GpuDagGraphCompiler` 18/18 PASS.

**Checklist / exit condition:** NM1.1 API, backbone validation, deletion including `grade.primary`, and compiler 5.3 lookup are done. NM1 overall exit items for Default/Clean factories, typed targets, thumbnail DAG, and product Save/Apply remain for NM1.2–NM1.5.

**LOC note (grill-code-review):** largest changed files — `pipeline_graph.cpp` 364, `pipeline_graph_commands.cpp` 318, `graph_compiler.cpp` 302, `pipeline_graph_command_test.cpp` 222. All under 1000 LOC; commands own mutation rules as a separate type from `PipelineGraph`.

**Remaining gaps:** `AddCleanColorGrade` currently inserts `ColorGradeNodeModel::MakeDefault` (Clean vs Default factories are NM1.2). CUDA/OpenCL/Metal grade encoders and `result_content_key` still look up `document.PrimaryGrade()` (`grade.primary`); CompileStatic no longer requires that id, but GPU execute of a non-primary remaining grade is not proven here (NM2 execution). Production UI still has no Add/Remove entry (NM5).

---

## 8. NM1.2 — Default 与 Clean 工厂

### 8.1 结果

两个入口从名字上即可区分：

```cpp
auto CreateDefaultPipelineDocument() -> PipelineDocument;
auto CreateCleanColorGradeNode(NodeId id) -> std::unique_ptr<ColorGradeNodeModel>;
// 或 ColorGradeNodeModel::MakeClean(NodeId)
```

`CreateDefaultPipelineDocument()`：

- Develop、Default Color Grade（默认仍可用 id `grade.primary`）、DRT；
- Default Color Grade 带产品基线：`exposure_ev = 1.5`，saturation `1.3`（对应旧 `+30`）；
- 其余 Default 调整与当前 `MakeDefault` 目录一致（含 Clarity/Sharpen/Halation/Film Grain，直到 NM2 搬家）；
- 不依赖 remirror 才能得到基线。

`CreateCleanColorGradeNode()` / `MakeClean()`：

- Exposure `0 EV`，saturation `1.0`，其余 Color Grade 参数为无视觉变化；
- `enabled = true`，`mix = 1`，无 mask；
- **不含** Clarity、Sharpen、Halation、Film Grain；
- 不是 Default 节点改几个 Patch 装出来的。

现有 `pipeline_defaults::kCleanBaseline*` 可以继续给 CPU 兼容层用，但新文档工厂代码用 Default / identity 这类准确名字。

### 8.2 主成功调用链

```text
CreateDefaultPipelineDocument
  -> MakeDefault Color Grade
  -> set exposure_ev 1.5 and saturation 1.3
  -> ValidateImageBackbone empty
```

```text
AddCleanColorGrade
  -> CreateCleanColorGradeNode
  -> insert + reconnect candidate backbone
```

### 8.3 主失败调用链

```text
UI or test tries to build a Clean node by patching Default
  -> not an API; tests assert MakeClean identity without going through Default
```

### 8.4 文件

- `color_grade_node_model.hpp` / `.cpp`
- `pipeline_document.hpp` / `.cpp`
- `alcedo_studio/tests/edit/graph/default_pipeline_test.cpp`
- 新或并入 `pipeline_graph_command_test.cpp` 的 Clean 断言

### 8.5 测试

| 名称 | 断言 |
| --- | --- |
| `DefaultPipelineDocumentBakesOnePointFiveEvAndSaturationOnePointThree` | 不经 importer，Default Grade 即为 1.5 / 1.3 |
| `MakeCleanColorGradeUsesIdentityParamsAndOmitsPostAdjustments` | 0 EV、sat 1.0、无四项后处理 |
| `AddCleanColorGradeDoesNotCopyDefaultExposureOrSaturation` | 新节点不是 +1.5 / 1.3 |

##### Phase NM1.2 completion record (2026-08-30)

**Status:** complete — Default document factory bakes +1.5 EV / saturation 1.3; Clean node factory is identity without the four post adjustments.

**Primary success call chain:**

```text
CreateDefaultPipelineDocument
  -> ColorGradeNodeModel::MakeDefault (catalog identity, 17 adjustments)
  -> ApplyDefaultPipelineLook (exposure_ev 1.5, saturation 1.3)
  -> Validate + ValidateImageBackbone empty
```

```text
AddCleanColorGrade
  -> CreateCleanColorGradeNode / ColorGradeNodeModel::MakeClean
  -> insert + reconnect candidate backbone
```

**Primary failure call chain:**

```text
UI or test tries to build a Clean node by patching Default
  -> not an API
  -> MakeClean is a separate type list (13 adjustments); patched Default JSON still differs
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DefaultPipelineDocumentBakesOnePointFiveEvAndSaturationOnePointThree` | `GpuDagModelGraphTest` | PASS |
| `MakeCleanColorGradeUsesIdentityParamsAndOmitsPostAdjustments` | `GpuDagModelGraphTest` | PASS |
| `AddCleanColorGradeDoesNotCopyDefaultExposureOrSaturation` | `GpuDagModelGraphTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest
ctest --test-dir build/debug -R GpuDagModelGraphTest --output-on-failure
```

Suite totals: `GpuDagModelGraphTest` 46/46 PASS.

**Checklist / exit condition:** NM1.2 Default vs Clean factories, baked product look without remirror, and AddClean using MakeClean are done. NM1 overall items for typed targets, thumbnail DAG, and product Save/Apply remain for NM1.3–NM1.5.

**LOC note (grill-code-review):** largest changed files — `pipeline_graph_commands.cpp` 318, `pipeline_graph_command_test.cpp` 255, `color_grade_node_model.cpp` 216, `legacy_import_test.cpp` 209, `pipeline_document.cpp` 176, `default_pipeline_test.cpp` 155. All under 1000 LOC. Clean vs Default lists are separate factories; product look is applied only in `CreateDefaultPipelineDocument`.

**Remaining gaps:** typed `EditorParameterTarget` and history live document writes (NM1.3); thumbnail/snapshot DAG (NM1.4); product Apply/Save still remirror from stages (NM1.5). `pipeline_defaults::kCleanBaseline*` remains for the CPU compat layer. Default Color Grade still includes Clarity/Sharpen/Halation/Film Grain until NM2.

---

## 9. NM1.3 — Typed target 与 history live 写文档

### 9.1 结果

```text
EditorParameterTarget
  owner_kind: Document | Develop | ColorGrade | ColorGradeMask | DrtPost
  node_id                  // Document 为空
  adjustment_instance_id
  mask_id                  // NM1 只预留；写入拒绝
  field_key
```

现有 `EditorAdjustmentPatch` 增加必填 `target`。规则见第 3.1 节：不完整、Mask、未知 field 均拒绝。app 不填缺省 target。

输入序列第一个合法 patch 锁定 target。后续完整 patch 复用该锁定，即使它们携带另一个 `node_id`。

`EditorHistoryMutation::CommitAdjustment` / Undo / Redo 的 **live 效果**写 `PipelineGuard::document_`。不要同步 stage 镜像。失败回滚以文档为准。`OrdinaryEditPayload` 本 Phase 不改 schema。

Mask target 和多节点 UI 选择恢复属于后续 Phase；本 Phase 拒绝 Mask target 写入。

### 9.2 主成功调用链

```text
HandlePatch(settled)
  -> lock EditorParameterTarget (first patch of the input sequence)
  -> Capture before from document
  -> PrepareAppendEdit(OrdinaryEditPayload)   // schema unchanged
  -> candidate document SetAdjustmentField
  -> validate
  -> PublishPreparedEdit
  -> publish document
  -> SettledAdjustment render reads document
```

### 9.3 主失败调用链

```text
unknown field, missing instance, or Mask target
  -> Rejected
  -> document hash and history head unchanged
```

```text
WAL or history publish fails
  -> restore document to before
  -> no new head
```

### 9.4 文件

- `alcedo_studio/src/include/app/editor_adjustment_types.hpp`
- `alcedo_studio/src/app/editor_adjustment_pipeline.cpp`（未知 field 仍拒绝）
- `alcedo_studio/src/include/app/editor_pipeline_command_service.hpp` / `.cpp`（完整 target 校验、文档字段写入）
- `alcedo_studio/src/app/editor_session_edit_controller.cpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp`
- `alcedo_studio/tests/app/editor_adjustment_pipeline_test.cpp`
- `alcedo_studio/tests/app/editor_pipeline_command_service_test.cpp`
- `alcedo_studio/tests/app/editor_session_edit_controller_test.cpp`
- `alcedo_studio/tests/edit/history/editor_session_history_port_test.cpp`
- `alcedo_studio/tests/support/editor_parameter_target_test.hpp`（测试夹具；不是产品缺省 target）

### 9.5 测试

| 名称 | 断言 |
| --- | --- |
| `SettledExposurePatchWritesPrimaryGradeDocumentNotOnlyStages` | `document` 上 exposure_ev 变化 |
| `IncompleteTargetRejectedLeavesDocumentHashAndHistoryHeadUnchanged` | 缺少 owner_kind 或 node_id 时拒绝，不填缺省 |
| `ProvisionalSequenceReusesTargetResolvedAtFirstPatch` | 同一序列后半段仍写第一个完整 patch 锁定的 NodeId |
| `UnknownFieldRejectedLeavesDocumentHashAndHistoryHeadUnchanged` | 失败封闭 |
| `UndoSettledExposureRestoresDocumentValue` | Undo 后文档回到 before |
| `MaskTargetWriteIsRejected` | NM1 不写 Mask |

History 投影文案、commit hash 算法、journal 格式留给 NM4。本 Phase 只要 Undo 后文档值正确。

##### Phase NM1.3 completion record (2026-08-30)

**Status:** complete — production patches require a complete `EditorParameterTarget`; live Capture/Commit/Undo/Redo write `PipelineGuard::document_`; incomplete, Mask, and unknown-field patches are rejected without filling a default target.

**Primary success call chain:**

```text
HandlePatch(settled, complete EditorParameterTarget)
  -> DescribeEditorParameterTargetError empty
  -> ResolveEditorAdjustmentField
  -> CaptureAdjustmentBeforePreview
       lock target on first complete patch of this field_key
       ReadEditorParameterJson before
       PublishEditorParameterPatch(document, locked target, params)
  -> CommitAdjustment
       PublishEditorParameterPatch after
       PrepareAppendEdit(OrdinaryEditPayload)   // schema unchanged
       PublishPreparedEdit
       record document_edit_by_commit[hash]
  -> Undo / Redo
       PublishPreparedHeadMove
       ApplyDocumentFieldEdits from document_edit_by_commit
```

**Primary failure call chain:**

```text
missing owner_kind / node_id / adjustment_instance_id, Mask target, or unknown field
  -> Rejected before history publish
  -> document canonical JSON unchanged
  -> working head unchanged
```

```text
WAL / history publish fails after provisional document write
  -> restore document from before_model_json
  -> no new head
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `SettledExposurePatchWritesPrimaryGradeDocumentNotOnlyStages` | `EditorSessionHistoryPortTest`, `EditorPipelineCommandServiceTest` | PASS |
| `IncompleteTargetRejectedLeavesDocumentHashAndHistoryHeadUnchanged` | `EditorSessionHistoryPortTest` | PASS |
| `ProvisionalSequenceReusesTargetResolvedAtFirstPatch` | `EditorSessionHistoryPortTest` | PASS |
| `UnknownFieldRejectedLeavesDocumentHashAndHistoryHeadUnchanged` | `EditorSessionHistoryPortTest` | PASS |
| `UndoSettledExposureRestoresDocumentValue` | `EditorSessionHistoryPortTest` | PASS |
| `MaskTargetWriteIsRejected` | `EditorSessionHistoryPortTest`, `EditorPipelineCommandServiceTest` | PASS |
| `IncompleteLaterPatchRejectedLeavesLockedDocumentUnchanged` (3.1 later-patch rule) | `EditorSessionHistoryPortTest` | PASS |
| `JournalAppendFailureRestoresDocumentExposureEv` (9.3 WAL restore) | `EditorSessionHistoryPortTest` | PASS |
| `IncompleteTargetIsRejected` / `MaskTargetIsRejected` | `EditorSessionEditControllerTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPipelineCommandServiceTest --target EditorSessionEditControllerTest --target EditorSessionHistoryPortTest --target EditorSessionActionPolicyCq3Test --target EditorAdjustmentPipelineTest
ctest --test-dir build/debug -R "EditorPipelineCommandServiceTest|EditorSessionEditControllerTest|EditorSessionHistoryPortTest|EditorSessionActionPolicyCq3Test|EditorAdjustmentPipelineTest" --output-on-failure
```

Suite totals: `EditorSessionHistoryPortTest` 46/46 PASS; `EditorAdjustmentPipelineTest` 6/6 PASS; `EditorPipelineCommandServiceTest` 4/4 PASS; `EditorSessionEditControllerTest` 11/11 PASS; `EditorSessionActionPolicyCq3Test` 10/10 PASS. Combined filtered run: 77/77 PASS. Logs: `build/tmp/nm1/`.

**Checklist / exit condition:** NM1.3 typed target, input-sequence lock, live document write, and Undo document restore are done. NM1 overall Apply/Save authority and thumbnail DAG remain for NM1.4–NM1.5.

**LOC note (grill-code-review):** production files under 1000 LOC — `editor_history_mutation.cpp` 539, `editor_pipeline_command_service.cpp` 198, `editor_session_edit_controller.cpp` 149, `editor_adjustment_types.hpp` 112. `editor_session_history_port_test.cpp` is 1842 lines (pre-existing history/WAL/paste suite; NM1.3 tests appended). Not split in this phase.

**Remaining gaps:** QML `submitPatch` still sends `field_key` only; those writes are rejected until NM6 fills a complete target. `document_edit_by_commit` is in-process; crash reopen still replays stage WAL, not a persisted document mutation (NM4). Product Apply/Save may still remirror from stages (NM1.5). Thumbnail/snapshot still clone via stages (NM1.4). `CheckoutVersion` does not restore document JSON from the side table. Process-internal stage `SetOperator` on commit remains until the stage table is deleted.

---

## 10. NM1.4 — Thumbnail 与 snapshot DAG

### 10.1 结果

`PipelineSnapshot` 携带独立的 `PipelineDocument` 克隆（不共享 live Model 指针）。Thumbnail、analysis rendition 用该文档走 GPU DAG 出图。

禁止：

```text
ExportPipelineParams
  -> ImportPipelineParams
  -> stage Apply / remirror
  -> thumbnail pixels
```

允许 snapshot 仍持有 `CPUPipelineExecutor` 作为 GPU session / 调度宿主，但 Apply 必须绑定已克隆的文档且 **不得**用 snapshot 的 stage JSON 覆盖该文档后再渲染。

`LoadPipelineSnapshot` 在 cache miss 时仍可 `LoadPipeline`，但捕获的是文档，不是「只导出 stages 再重建一份编辑状态」。

Export 目前走同一 snapshot。本子 Phase 把 snapshot API 改成文档权威后，export 像素也必须 DAG，不得留下一条 stage-only 克隆。

### 10.2 主成功调用链

```text
ThumbnailService::GetThumbnail
  -> LoadPipelineSnapshot
  -> ClonePipelineDocument(live or stored)
  -> snapshot.document
  -> GPU Renderer::Render(document)
  -> host thumbnail buffer
```

### 10.3 主失败调用链

```text
missing document on a v2 image
  -> snapshot load fails with real error
  -> no stage-only thumbnail substitute
```

```text
stage-only storage, no usable graph
  -> Load / snapshot fails with real error
  -> no LegacyPipelineImporter product open path
```

### 10.4 文件

- `alcedo_studio/src/include/app/pipeline_service.hpp`（`PipelineSnapshot` 增加 document）
- `alcedo_studio/src/app/pipeline_service.cpp`（`LoadPipelineSnapshot`）
- `alcedo_studio/src/app/thumbnail_service.cpp`
- `alcedo_studio/src/edit/pipeline/pipeline_cpu.cpp`（thumbnail Apply 不再 `ApplyOnto` 覆盖 snapshot 文档）
- `alcedo_studio/tests/app/thumbnail_service_test.cpp`

### 10.5 测试

| 名称 | 断言 |
| --- | --- |
| `LoadPipelineSnapshotClonesDocumentWithoutReimportingStagesAsRenderSource` | snapshot 文档与 live 文档 canonical JSON 一致，且不是从 stage JSON Import 出来覆盖 live 图 |
| `ThumbnailRenderUsesGpuDagDocumentWithoutStageApplyOnto` | 文档上的 exposure 与 thumbnail 路径使用的文档一致；渲染前文档 hash 不被 stage JSON 改写 |
| `AnalysisRenditionUsesSameDocumentSnapshot` | 分析路径同样绑定 snapshot document |
| `StageOnlyStoreFailsSnapshotLoadWithoutImporterSubstitute` | 无图存储失败；不 Import 成默认图再出缩略图 |

若现有 `ThumbnailServiceTest` 依赖 remirror 或 `MirrorsLegacyStageAdapter()`，改为断言 DAG 文档身份，或断言无图存储失败。

---

## 11. NM1.5 — 产品写入权威（停止 stage 覆盖文档）

### 11.1 结果

Live editor：

- 新参数写入文档（NM1.3）；
- GPU 产品帧读取文档；
- **禁止**每次 Apply 用当前 stages `ApplyOnto` 覆盖刚刚写过的文档；
- **禁止** dirty `SavePipeline` 用 `LegacyPipelineImporter::Import(stages)` 整份替换 `document_`。

`SavePipeline` / `SyncPipelineDocument` 持久化 `document.ToJson()`。不要为旧读取器附带 `legacy_stage_adapter` 镜像 blob。

Load 规则：

- format v2 且含完整 nodes/edges：文档权威。不要用 adapter 或 stage 表覆盖图。
- 仅有旧 stage JSON / adapter、没有可用图：Load 失败，返回真实错误。不要 Import 成默认三节点。不要升级 mini-git commit。

`InitializeImageRoot` 必须把当时的完整默认 **文档**记入 root，而不是只存 stage 表。后续 catalog 默认值变化不得重写已存在 root。

本子 Phase 不从代码中删除 CPU stage 表。删除 stage 属于后续 Phase。

### 11.2 主成功调用链

```text
settled slider
  -> document mutation
  -> GPU Render(document)
  -> SyncPipelineDocument / Save writes document ToJson
```

### 11.3 主失败调用链

```text
ApplyOnto of live stages would overwrite a newer document edit
  -> this path is removed from product Apply/Save
```

```text
stage-only store, no usable graph
  -> real error
  -> project does not open
  -> no CPU stage editor path restored
```

### 11.4 文件

- `alcedo_studio/src/app/pipeline_service.cpp`（Load / Save / Remirror / InitializeImageRoot）
- `alcedo_studio/src/edit/pipeline/pipeline_cpu.cpp`（`ApplyGpuDagProduct`）
- `alcedo_studio/src/include/edit/graph/pipeline_document.hpp`（产品默认路径不再调用 `AllowsLegacyStageAdapterRemirror`）
- `alcedo_studio/src/storage/mapper/pipeline/pipeline_mapper.cpp`
- `alcedo_studio/tests/app/pipeline_service_test.cpp`
- `alcedo_studio/tests/edit/graph/legacy_import_test.cpp`（importer 可留作非产品测试；产品 Load 不调用）

### 11.5 测试

| 名称 | 断言 |
| --- | --- |
| `DirtySaveWritesDocumentJsonAndDoesNotReplaceGraphFromStages` | 文档里若有额外 Color Grade，save 后仍在 |
| `GpuApplyDoesNotApplyOntoDocumentFromLiveStagesAfterDocumentEdit` | 文档 exposure 不被 stage 旧值盖回 |
| `Format2LoadTreatsDocumentAsAuthorityWhenNodesPresent` | load 不因 adapter 抹掉图结构 |
| `StageOnlyStoreLoadFailsWithoutDefaultGraphImport` | 无图存储失败；不打开；不 Import |

现有 `ReloadedFormat2GraphWithoutAdapterStillRemirrorsCpuNeuralEngine` 一类测试必须改写：不再要求 reload 后仍把 CPU 当权威去 remirror 覆盖文档。旧名 `LegacyStageOnlyStoreImportsOnceThenRendersDocument` 不再作为产品要求。

---

## 12. 建议的 app 类型

```text
EditorPipelineCommandService
  Prepare(mutation) -> candidate clone, validate, optional CompileStatic
  Publish -> document, history head if settled
```

QML 本 Phase 不调用图命令。`EditorSessionEditController` 和 `EditorHistoryMutation` 走该 service 或同等窄接口，避免再直接 `SetOperator` 后指望 ApplyOnto。

---

## 13. 构建与证据

Windows：

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineMapperTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target ThumbnailServiceTest
```

日志放 `build/tmp/nm1/`。每个子 Phase 完成后在本文件追加 completion record（调用链、命令、通过的测试名）。不要把这些日志写进总体方案。

---

## 14. NM1 退出条件

- [ ] 新编辑写入 `PipelineDocument`；产品 Apply/Save 不再用 live stages 覆盖新文档
- [ ] 产品不提供 stage 镜像；无可用图的旧存储 Load 失败
- [ ] Thumbnail / analysis / 同 snapshot 像素路径 DAG 渲染，不经 stage ApplyOnto
- [x] Add / Remove（含 primary）/ Reconnect 原子、失败回滚、canonical JSON 可逆
- [x] Default 三节点工厂自带 `+1.5 EV` / saturation `1.3`；Clean 为 identity 且无四项后处理
- [x] `EditorParameterTarget`：每个 patch 完整；缺字段拒绝；输入序列锁定；Mask target 拒绝
- [x] History live Undo 恢复文档值（payload schema 仍可是旧的）
- [ ] 生产 UI 仍无新增节点入口
- [ ] 不声称打开或升级 DAG document 之前的项目；NML 取消

---

## 15. 与后续 Phase 的接口

- **Stage table delete (later, not NM1):** 从代码中删除 CPU stage 表和产品路径上的 `LegacyPipelineImporter`。不是旧项目升级。
- **NML：** 取消。本产品不打开、不升级 DAG document 之前的 stage-only 项目，也不迁移旧 mini-git commit。
- **NM2：** 主链上每一个 Color Grade 都执行；本 Phase 只执行第一个。删除能力已在 NM1 证明，NM2 不得再拿「compiler 需要 `grade.primary`」限制删除。NM2 假定打开的项目已经是可用 DAG 文档。无图项目不会进入 NM2。
- **NM4：** 把 `OrdinaryEditPayload` 换成 typed `PipelineEditBatch`；checkpoint 存文档而不是只存 stage params。NM4 不回填旧 stage commit。
- **NM6：** 右侧面板按选中节点发送完整 target。NM1 已要求完整 target 与输入序列锁定。

---

## 16. Completion records

NM1.1 (2026-08-29): complete — recorded under §7.
NM1.2 (2026-08-30): complete — recorded under §8.
NM1.3 (2026-08-30): complete — recorded under §9.

后续子 Phase 完成后按同一模板追加。模板：

##### Phase NM1.x completion record (YYYY-MM-DD)

**Status:** complete | partial — …

**Primary success call chain:** （`text` 箭头链）

**Primary failure call chain:**

**What was proven (executed tests):** 表

Commands: …

**Remaining gaps:**
