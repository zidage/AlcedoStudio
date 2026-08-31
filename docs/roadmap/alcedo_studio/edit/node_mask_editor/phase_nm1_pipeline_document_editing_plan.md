# Phase NM1 — PipelineDocument Editing Foundation

Date: 2026-08-29

Revised: 2026-08-30 — 按单 live document、原地函数应用、共享 executor 重定执行方案。

Status: in progress — NM1.4 A/B 已完成并通过本版验收；NM1.2 保留；NM1.4 C、NM1.5 待实施。

Branch: `feature/pipeline-document-editing`

Base: `origin/main` at NM0 merge (`d5a96267`)。

对应[总体方案](../node_mask_editor_master_plan.md)第 2.1、6、11、21.2、21.5 节。
[NM0](phase_nm0_quickqanava_integration_plan.md)只记录 QuickQanava 接入；history 的阶段边界定义在总体方案 NM4。

本版替换原 NM1 的整图 candidate、独立 snapshot executor 和多份编辑状态要求。既有功能不推倒重来；
从当前 NM1.4 工作区继续，先消除 NM1.1/NM1.3 的相关实现债务，再完成渲染和保存读取。
2026-08-30 的方案修订未撤回未提交源码；其后 NM1.4 A 已定向实施，见第 10.1 节完成记录；NM1.4 B 见第 10.2 节完成记录。过去的测试结果保留在第 16 节，不代表 C 或整个 NM1 已完成。

---

## 1. 目标与优先级

**NM1 的交付物是一套可以直接使用的内存编辑基础：每张图片一个 live document，参数和结构通过领域函数原地修改；
同一 executor 根据任务请求渲染；保存读取不再经过 stage 镜像。**

NM1 结束时，应能从 app/service 测试完成以下流程：

1. 创建或加载 document，取得同一图片的 pipeline 使用权。
2. 通过完整 target 修改参数；通过领域命令新增、删除、重接 Color Grade。
3. 用同一 document/executor 交替执行编辑器、缩略图、分析和导出渲染。
4. 保存并重新读取相同的 document 参数和结构，释放后台任务不触发保存。
5. 非法输入、任务取消、WAL 写入失败和存储失败均返回真实结果，不留下半次修改或泄漏使用权。

这不是完整节点历史的交付阶段。typed history schema、跨进程节点重放、Version/Paste 和项目格式切换仍属于 NM4。
Nodes UI 属于 NM5，完整 adjustment UI 接线属于 NM6。

冲突时按以下顺序执行：

1. 2026-08-30 用户确认的本版简化原则和 NM4 边界。
2. 总体方案的单主链、Default/Clean、完整 target、质量策略与禁止替代实现的产品规则。
3. 原完成记录和现有代码；它们只说明起点，不能反过来要求保留复制模型。

参考：[单 live pipeline、WAL 与 checkpoint 方案](../../ui/editor_single_live_pipeline_wal_checkpoint_plan.md)。
其中关于单操作对象、history 决定 HEAD 的原则继续适用；历史文档中的不同 root/chain 设计不作为 NM1 新增状态的理由。

## 2. 当前起点与未提交改动处置

截至本次审阅：

| 已有内容 | 保留的能力 | 必须修正的部分 |
| --- | --- | --- |
| NM1.1 图命令与主链校验 | 完整删除、重接、稳定 NodeId、零 Grade 编译 | 命令内整图 JSON 回滚及调用方 candidate clone |
| NM1.2 Default/Clean 工厂 | Default 的 1.5 EV/1.3 saturation；独立 Clean 工厂 | 不重写该功能 |
| NM1.3 完整 target 与 live history 接线 | 输入序列锁定、参数写入、同会话参数 Undo/Redo | 每次 preview/settled 的整图 clone、未统一的锁、stage 参数读写 |
| 未提交 NM1.4 | DAG 像素接线、错误传播、已有真实 RAW 测试准备 | 独立 document/executor、stage export/import、逐层旧格式分流、强制副本独立的测试 |
| 现有 scheduler/runtime | render lock、任务参数、frame sink 切换、session/单次资源 | 检查所有退出路径和调用方是否真正使用这些能力 |

执行决定：**在当前工作区定向改写，不执行整批 reset/revert。** 只撤掉下面列出的实现要求；
删除前确认用途，不触碰无关改动。原 NM1.4 的“complete”已撤销。

| 文件/实现 | 继续实施时的处置 |
| --- | --- |
| `pipeline_service.hpp/.cpp` 的 `PipelineSnapshot`、`LoadPipelineSnapshot`、`ReleasePipelineSnapshot` | 用普通 pipeline 获取/释放承载后台任务；迁移调用方后删除 snapshot 专用对象和生命周期 |
| `pipeline_document.hpp/.cpp` 的 `HasUsablePipelineGraph` | 将合法性检查集中到 document 读取边界；不再把它作为逐层选择 live、executor、存储或 stage 的分流器 |
| `pipeline_cpu.cpp` 的 `!require_host_output` remirror 条件 | 删除产品 remirror；不能只保护 host output 而让编辑器继续覆盖 document |
| `thumbnail_service.cpp`、`export_service.cpp` | 保留 DAG 输出和真实错误；改为持有普通 pipeline 使用权，任务配置移入 render lock |
| snapshot/thumbnail 测试 | 保留参数、输出、错误、资源释放断言；删除“必须新建 executor”“必须不共享 Model”的验收 |
| `default_pipeline_test.cpp` 新增的格式探测断言 | 按统一读取边界的行为测试取舍，不为保留辅助函数而保留断言 |

## 3. 锁定的模型

| 对象 | 职责 |
| --- | --- |
| History | 唯一拥有 Version/HEAD；最终已提交状态的恢复依据 |
| Live `PipelineDocument` / OperatorModel | 唯一可写内存状态；预览期间允许包含尚未提交的值 |
| `PipelineGuard` 或等价使用句柄 | 保证同一图片的 document/executor 存活；不是冻结状态 |
| Executor / Renderer | 读取 document 和本次 RenderRequest；维护运行资源、编译和结果缓存 |
| Pipeline checkpoint | 已保存 document 及其对应提交位置；只用于快速恢复，不是另一份可编辑状态 |
| WAL | 保护尚未完成持久化的操作；不是第二份 history |

“document 是编辑和渲染输入”不等于“document 覆盖 history”。完整的加载顺序在 NM4 接入：
先恢复合法 WAL、确定 history HEAD，再比较 checkpoint 记录的 commit hash；不匹配则从确定的初始状态重放。
NM1 不新增并行 HEAD、逐参数 chain hash、document/stage 身份比较或多写者发布协议。

这里的 commit hash 是状态所对应的提交标签，不是 document 内容哈希。不能把实时读取 history HEAD
当成“参数已应用到该提交”的证明。NM4 负责把实际应用位置与 checkpoint 一起正确保存。
JSON round-trip 测试、节点定位和渲染缓存键各自保留，不用这些比较来协调多份编辑模型。

### 3.1 完整 target 与项目读取边界

每个 patch 必须带完整 `EditorParameterTarget`：

- `owner_kind` 为 Document / Develop / ColorGrade / DrtPost；NM1 拒绝 Mask 写入。
- `field_key` 非空，且与 patch 的 `field_key` 相同。
- Document 的 `node_id` 为空；其余三类的 `node_id` 非空。
- ColorGrade 的 `adjustment_instance_id` 非空；NM1 的 `mask_id` 为空。
- app 不根据字段目录、选中节点或前一次 patch 补缺失 target。

输入序列的第一个合法 patch 锁定 target。同一序列后续 patch 即使携带另一个完整 NodeId，
仍写锁定目标；后续 patch 自身不完整则拒绝。取消恢复该目标的 before。
当前 QML 缺完整 target 的请求继续拒绝，不为 NM1 临时补 UI。

发布新项目格式后只读取支持的项目元数据版本；旧项目在项目打开入口拒绝，不迁移。
NM4 负责版本提升和最终 schema 切换，NM1 不提前宣称已经完成版本门禁升级。
NM1 的 document 读取只接受当前有效图数据；缺失、损坏或非法图明确失败，不从 stage/default/live 另找替代来源。
新图片导入时创建 Default 是正常初始化；已有图片读取失败后创建 Default 则禁止。

### 3.2 NM1 与 NM4 的 history 边界

NM1 保留已完成的同会话参数 Commit/Undo/Redo 和 WAL 失败恢复，不将 typed `PipelineEditBatch` 提前实施。
`OrdinaryEditPayload` 的既有格式可以保留到 NM4；其中的 stage/operator 字段只是旧日志标识，
不能要求产品继续维护另一张可写 stage 参数表。

`document_edit_by_commit` 是 NM1.3 已有的同会话参数 before/after 记录，当前不扩展、不复制成另一份 document、
不新增持久化、不得作为跨进程重放来源。NM4 用真实 typed payload 替换后删除。
NM1 可调整其写入，使 before/after 来自 Model 的实际规范化值，并通过相同的领域函数恢复。

完整节点新增/删除/重接的历史记录、进程重启后的节点 Undo/Redo、Version checkout、Paste 和 WAL 节点重放
均不在 NM1 验收范围。缺少所需记录时不能静默跳过并报告成功，也不能转回 stage 编辑；
依赖这些能力的用户入口保持未开放。NM1 不为填这个阶段空缺设计新的历史桥接框架。

## 4. 范围

| NM1 必须完成 | 本阶段不做 |
| --- | --- |
| 原地参数/图命令；局部失败恢复；统一访问互斥 | 完整 typed history 与节点 recovery（NM4） |
| Default/Clean；完整 target；同会话已有参数 Undo/Redo | 多 Grade 的完整 GPU 执行、按节点缓存优化、后处理迁移（NM2） |
| 同一图片的共享 document/executor 渲染 | Mask 列表与资源编辑（NM3） |
| 停止产品 Load/Save/Apply 的 stage 镜像 | 旧项目和旧提交迁移（不做） |
| document 序列化保存与读取；导入时模型参数完整 | 新项目 metadata/history schema 切换（NM4） |
| 获取/释放与持久化分离；任务参数无泄漏 | Nodes/Mask/adjustment UI 接线（NM5–NM7） |

CPU stage 类型和测试工具不必在 NM1 全部删除，但不得继续成为产品参数源或镜像。
不增加持久化 root document、完整 committed document 副本、shadow CommitGraph 或通用 Prepare/Publish 服务。
初始默认状态的跨版本稳定性由 NM4 的存储设计保证；NM1 只提供正确的工厂和 Model 操作。

## 5. 实现规则

### 5.1 状态转移通过函数完成

概念上：`DocumentAt(head) = ApplyEdits(initial_document, edits_to_head)`。
实现上在同一 document 上依次调用领域函数，不需要为每一步分配新 document。

- 输入解析、值规范化和前置条件检查不产生副作用；通过 Model 行为得到合法的新参数。
- 局部新参数、待插入节点、受影响边和 before 值可以临时存在；禁止复制无关节点/OperatorModel。
- JSON 留在请求解析、序列化、既有日志边界和测试比较处，不用整图 JSON 重演 Model 行为。
- 优先保留已有领域函数，不为“函数式”新增 reducer 框架、命令总线、工厂层或接口层。
- 原地 setter 必须按现有规则更新 dirty/cache 信息；禁止把所有参数修改都标记成拓扑修改。

### 5.2 原子性与失败恢复

参数修改：完整解析并校验所改参数，再应用到目标 Model。setter 如果可能部分失败，先保留该参数对象的旧值；
恢复只涉及该对象，不重建整个 document。曝光、改名等不改变连线的操作不做整图拓扑验证。

图命令：检查 endpoint、NodeId、插入位置和受影响连接；预先完成必要分配，在同一互斥范围内修改节点和边，
验证主链；失败将保留的节点/边移回。未受影响的 Model 继续存在。结构验证保留，失败不暴露中间图。

预览开始只捕获一次所改目标的 before；preview 直接改 live，不写 WAL、不创建 commit。
settled 使用规范化 before/after；没有变化就结束。WAL append/flush 成功后完成既有 history 记录和 live 应用。
已有 preview 在失败时恢复 before；成功后才确认 dirty 并请求最终渲染。
失败撤销只涉及本次未生效操作和对应日志尾，不清空此前仍负责恢复的 WAL。

正常 GPU 执行失败返回真实错误，保留最后一个成功显示结果，不切换算法、后端或质量。
GPU 输出失败不应被当成隐式 Undo：已经成功记录的编辑仍在；输入/结构验证失败与渲染失败分别处理。
局部恢复本身失败时明确报告并停止继续使用损坏状态，不能吞异常后继续渲染。

### 5.3 一处访问互斥

复用 executor 的 `GetRenderLock()` 及现有 owner-thread 等待方式：

- Model 写入、图结构修改、序列化读取与渲染读取遵循同一把锁。
- scheduler 在锁内安装请求参数、执行并完成必要的输出交接；锁外不改共享 executor。
- 缓存锁只用于查找和 pin，不在持有它时等待渲染、访问 DuckDB 或执行回调。
- 领域函数不重复获取调用方已持有的锁；明确 app/scheduler 是共享访问边界。
- 完成、取消和异常都恢复请求参数、frame sink 并释放使用权；释放不能在回调中递归等待同一 render lock。

同图任务串行，不承诺后台任务零等待；复用现有优先级/取消机制，不另起任务系统。

### 5.4 单主链

合法图为 `Develop -> ColorGrade[0..n] -> DRT/Post`；零 Grade 时 Develop 直连 DRT。
拒绝删除/替换 endpoint、环、scene-image fan-in/fan-out、端口类型错误和游离 Color Grade。
NM3 之前不重写 Mask 所有权。

NM2 之前 compiler 保留现有阶段能力：零 Grade 跳过 Grade pass；有 Grade 时编译主链第一个，
不能依赖它叫 `grade.primary`。多 Grade GPU 执行留在 NM2，不对用户开放新增节点入口。
NM1 的新像素验收使用当前已支持的默认图，图命令的多节点测试验证模型与编译结果，不冒充多节点渲染证明。

## 6. 从当前工作区继续的顺序

```text
NM1.4 A：修正 NM1.1/NM1.3 的原地修改与锁
  -> NM1.4 B：统一 document 输入，删除产品 Apply 的 stage 覆盖
  -> NM1.4 C：后台任务共用 pipeline/executor，迁移并删除 snapshot API
  -> NM1.5：统一保存读取、导入初始化与释放边界，执行全阶段验收
```

A/B/C 是 NM1.4 内部实施顺序，不新增一级 Phase。B 必须先于 C，避免共享 executor 后编辑器再次覆盖后台所读文档。
每一段应可单独构建和运行受影响测试；完整 NM1 以第 14 节为准。

## 7. NM1.1 保留的图能力

保留 `AddCleanColorGrade`、`RemoveColorGradeAndBridge`、`ReconnectColorGrade`、
`RenameColorGrade`、`SetColorGradeEnabled`；参数中的 document 表示当前操作对象，不再要求 candidate。

- Add 在指定 `before_node_id` 之前插入；指定 DRT 则插入主链末尾。
- Remove 可删除任意 Grade，包括 `grade.primary`；删除最后一个后桥接 Develop/DRT。
- Rename 不改 NodeId；Develop/DRT 不允许以 Grade 命令修改。
- 图容器不向 UI 暴露可变访问；测试用领域命令或专用 fixture 建图。
- forward/inverse 后参数与连接恢复；不要求副本有不同地址。

已有主链校验、删除与 compiler 测试继续保留。整图复制和 JSON 回滚的删除由 NM1.4 A 完成，
不能因这部分代码已经提交就排除在简化范围外。

## 8. NM1.2 保留的 Default/Clean

`CreateDefaultPipelineDocument()` 创建 Develop、Default Grade、DRT；
Default 自带 exposure 1.5 EV、saturation 1.3，不经过 importer。

`CreateCleanColorGradeNode()` / `MakeClean()` 创建无视觉变化 Grade：
exposure 0 EV、saturation 1.0、enabled、mix 1、无 mask，
不包含 Clarity/Sharpen/Halation/Film Grain。Default 内现有后处理项由 NM2 搬到 DRT/Post。

历史重放所说的 clean ground 是清除旧用户编辑后得到的确定初始文档，必须保留图片固有参数；
不等于把 Default 改成 Clean Grade，也不等于添加一份持久化 root snapshot。

## 9. NM1.3 保留的参数能力

完整 target、输入序列锁定和拒绝规则按第 3.1 节执行。同会话已有参数 Undo/Redo 按第 3.2 节执行。

```text
HandlePatch(complete target)
  -> validate target / parse and normalize parameters
  -> lock shared live pipeline
  -> capture local before once; apply through Model
  -> preview: request render
  -> settled: record existing WAL/history edit; finish live change
  -> Undo/Redo: apply recorded local before/after through the same Model operation
```

`PublishEditorParameterPatch` 的 clone/validate-whole-graph/move 实现应被直接应用函数取代。
before/after 不能继续从 stage 表读取；既有 payload 需要的字段表示在原有窄边界处理，
不能因为旧日志仍有 `stage_name` 就调用 stage `SetOperator` 维护镜像。
不新建完整 committed document；已有 UI 参数投影只读，不能成为编辑源。

## 10. NM1.4 — 原地编辑与共享 DAG 渲染

Status: partial — A complete（2026-08-30）；B complete（2026-08-30）；C rework required。A/B 使用下列新证据，C 不沿用旧 snapshot 测试的完成结论。

### 10.1 A：原地修改和局部恢复

工作：

1. 清理 `editor_pipeline_command_service.cpp` 的整图 clone；规范化参数后直接调用 Model。
2. 清理 `pipeline_graph_commands.cpp` 的整图 JSON 回滚；保留受影响节点/边以便恢复。
3. 统一 `editor_history_mutation.cpp` 中 preview/settled/Undo/Redo 的 document 访问锁。
4. 同会话 before/after 来自 document，删除为维持 stage 镜像发生的参数读写。
5. 保留未知字段、缺 target、Mask target、无效拓扑、WAL 失败的拒绝与恢复行为。

验收：

- `ParameterPatchPreservesUnchangedModels`：修改一个值，其余节点/Model 不重建，所改值正确。
- `InvalidParameterLeavesLiveValueAndHistoryHeadUnchanged`：非法输入不部分改值。
- `GraphCommandFailureRestoresAffectedNodesAndEdges`：失败恢复精确连接、NodeId 和参数。
- 重新运行现有完整 target、输入锁定、Undo 和 `JournalAppendFailureRestoresDocumentExposureEv` 测试。
- 参数重复修改不触发整图序列化，也不改变无关拓扑；用调用链检查及必要的测试计数证明，不新增生产观测框架。

#### NM1.4 A completion record (2026-08-30)

**Status:** complete — 参数直接写既有 Model；结构修改只保留受影响节点/边；preview、settled、同会话 Undo/Redo、取消和显式 HEAD 移动在 executor render lock 内访问 document。

**实现与验收清单：**

- [x] `PublishEditorParameterPatch` 直接调用 `ApplyEditorParameterPatch`，不 clone、整图验证或替换 live document。
- [x] 在 Model setter 前检查参数键、类型、有限数值和数组长度；Model 执行既有规范化。setter 异常仅恢复该 Model；恢复异常返回明确错误。
- [x] Add/Remove/Reconnect 经 `PipelineGraph::ApplyBackboneEdit` 预留容器容量，只暂存受影响边及删除节点的原对象；失败恢复节点所有权和精确边顺序。Rename/Enabled 不验证整图。
- [x] 首个成功 patch 锁定 target 并捕获 before；无效首 patch 不锁定目标，后续不完整 target 仍拒绝。
- [x] WAL/history 的 before/after 来自规范化后的 document；相同规范化值不产生 commit。仅在既有日志边界将 `exposure_ev` 表示为 `exposure`，不调用 stage 参数读写。
- [x] 同会话 Undo/Redo 仅应用所经过提交的局部值。无同会话 document target 的记录在 WAL publication 前拒绝，不经 stage 重放；完整 merge/跨会话重放仍属于 NM4。

**Primary success call chain:**

```text
EditorSessionEditController::HandlePatch(complete target)
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
  -> LockLivePipeline(GetRenderLock) -> capture target Model before once
  -> ApplyEditorParameterPatch -> validate supplied values -> existing Model::LoadJson/setters
  -> CommitAdjustment: read normalized after -> PrepareAppendEdit -> PublishPreparedEdit(WAL/history)
  -> record local before/after + update read-only panel projection -> release lock -> RenderRouted

Undo / Redo / MoveHeadToCommit
  -> same render lock -> prepare head move -> resolve existing local target records
  -> PublishPreparedHeadMove -> ApplyEditorParameterPatch(local before/after)
  -> update panel projection -> release lock

Add / Remove / Reconnect Color Grade
  -> check endpoints and affected edges -> ApplyBackboneEdit
  -> retain removed node/edges -> mutate -> Validate + ValidateImageBackbone
  -> success: mark topology dirty, retain all unrelated Models
```

**Primary failure call chain:**

```text
invalid target / unknown field / Mask / invalid parameter
  -> reject before setters -> live value and history HEAD unchanged
throwing Model setter -> restore this Model's before value -> return real error
WAL append failure after preview -> restore locked target's before -> no HEAD advance
invalid graph edit -> restore original node ownership and edge order -> return validation errors
history record missing same-session target -> reject before WAL/document mutation
```

**What was proven (executed tests):**

| Required name / criterion | Target / suite | Result |
| --- | --- | --- |
| `ParameterPatchPreservesUnchangedModels` | `EditorPipelineCommandServiceTest` | PASS；40 次参数修改保留全部节点/Model 地址、无关 dirty 状态与拓扑；测试内计数 Model 的序列化调用为 0 |
| `InvalidParameterLeavesLiveValueAndHistoryHeadUnchanged` | `EditorSessionHistoryPortTest.EditorDocumentHistoryTest` | PASS；首个非法值和 settled 非法值均不部分应用或移动 HEAD |
| `GraphCommandFailureRestoresAffectedNodesAndEdges` | `GpuDagModelGraphTest` | PASS；重接、删除、插入校验失败恢复对象、NodeId、参数和精确连接；非法图上的原位置重接也被拒绝 |
| `InvalidCompoundParameterDoesNotPartiallyApplyOrDirtyModel`、`GeometryAndDevelopRejectInvalidValuesBeforeAnyWrite` | `EditorPipelineCommandServiceTest` | PASS；错误类型、未知键、非有限值与复合参数边界 |
| `ThrowingSetterRestoresOnlyAffectedModelParameters` | `EditorPipelineCommandServiceTest` | PASS；注入 setter 部分写入后抛异常，局部值恢复且其余对象不重建 |
| 既有完整 target、输入序列锁定、未知字段/Mask 拒绝、`UndoSettledExposureRestoresDocumentValue`、`JournalAppendFailureRestoresDocumentExposureEv` | `EditorSessionHistoryPortTest.EditorDocumentHistoryTest` | PASS；原行为测试移至独立 document fixture，保留断言目标 |
| `RejectedFirstPatchDoesNotLockTargetAndHistoryUsesNormalizedModelValues` | `EditorSessionHistoryPortTest.EditorDocumentHistoryTest` | PASS；额外 Grade 目标独立、clamp 后 before/after 入日志、规范化 no-op、Undo/Redo 与 stage 不变 |
| `PreviewCommitUndoRedoAndCancelWaitForRenderLock` | `EditorSessionHistoryPortTest.EditorDocumentHistoryTest` | PASS；持锁屏障与 promise 控制顺序，无 sleep；所有操作等待同一锁，地址保持且 stage 不变 |

Commands（仓库根目录）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest EditorPipelineCommandServiceTest EditorSessionHistoryPortTest EditorAdjustmentPipelineTest EditorSessionEditControllerTest
ctest --test-dir build/debug -R "GpuDagModelGraphTest|EditorPipelineCommandServiceTest|EditorSessionHistoryPortTest|EditorAdjustmentPipelineTest|EditorSessionEditControllerTest" --output-on-failure
```

Suite totals: **121/121 PASS，0 skipped** — Graph 47、CommandService 8、HistoryPort 49（含 document 编辑 13）、AdjustmentPipeline 6、SessionEditController 11。
最终日志：`build/tmp/nm1/nm14a-final-build.log`、`build/tmp/nm1/nm14a-final-ctest.log`。

**旧测试调整：** 移除“preview 不取渲染锁”的过时前提，由新的五类操作互斥测试覆盖；曝光请求 fixture 使用 `exposure_ev`，参数结果以 document 和 Model 的 clamp 后值为准；四个无 document target 的旧历史移动测试改为断言真实拒绝、HEAD/document/投影/WAL 不变。Version/Paste 的其余既有回归仍运行；未通过恢复 stage 写入使旧断言通过。

**Checklist / exit condition:** 第 10.1 节五项工作和全部验收已完成。仅勾选第 14 节由 A 证明的两个条件；NM1.4 整体保持 partial，B/C 与 NM1.5 未完成。

**LOC note（grill-code-review）：**

| 本次文件 | 总 LOC | Diff + / - |
| --- | ---: | ---: |
| `src/app/editor_pipeline_command_service.cpp` | 253 | +65 / -27 |
| `src/include/app/editor_pipeline_command_service.hpp` | 57 | +14 / -17 |
| `src/edit/graph/pipeline_graph.cpp` | 425 | +61 / -0 |
| `src/include/edit/graph/pipeline_graph.hpp` | 98 | +11 / -0 |
| `src/edit/graph/pipeline_graph_commands.cpp` | 252 | +51 / -117 |
| `src/include/edit/graph/pipeline_graph_commands.hpp` | 74 | +17 / -12 |
| `src/ui/alcedo_main/album_backend/editor_history_mutation.cpp` | 503 | +126 / -210 |
| `src/include/ui/alcedo_main/album_backend/editor_history_mutation.hpp` | 66 | +9 / -3 |
| `src/include/ui/alcedo_main/album_backend/editor_history_state_detail.hpp` | 101 | +1 / -1 |
| `tests/app/editor_pipeline_command_service_test.cpp` | 206 | +132 / -0 |
| `tests/edit/graph/pipeline_graph_command_test.cpp` | 296 | +41 / -0 |
| `tests/edit/history/editor_document_history_test.cpp` | 351 | +351 / -0 |
| `tests/edit/history/editor_session_history_port_test.cpp` | 1843 | +104 / -339 |
| `tests/ui/CMakeLists.txt` | 1399 | +1 / -0 |

原 `editor_session_history_port_test.cpp` 仍超过 1,000 LOC：本次已将 document 参数行为及其独立 guard/document/WAL fixture 移到 `editor_document_history_test.cpp`，没有继续向旧 fixture 添加该职责。旧文件剩余 Version/Paste/持久化集成与纯投影测试不在 A 中重构；将来可按 transfer、persistence、projection 分成独立 fixture，每组自行拥有所需 graph/guard/journal/services，不能以共享巨大 fixture 的方式拆文件。`tests/ui/CMakeLists.txt` 是测试注册清单，本次仅加一个源文件，无运行时状态需要拆分。

**Residual gaps / scope boundaries:** B 的产品 Apply remirror、C 的共享后台 executor/lifetime、NM1.5 的统一 I/O 仍保留当前工作区实现，未修改也未以 A 的测试宣称通过；无真实 RAW/GPU 像素或完整应用交互验收。NM4 的 typed history、跨会话节点/merge 重放及 Version/Paste 新模型接线没有提前实施。全树术语扫描未发现 roadmap 违规；本次代码/测试无禁用术语，其他 QML/旧源码的既存命名未纳入此范围。

### 10.2 B：让所有产品渲染只读 document

工作：

1. 从 `ApplyGpuDagProduct` 删除产品 `LegacyPipelineImporter::ApplyOnto`，不再按 host/editor 区分参数来源。
2. 产品渲染要求绑定同一 live document；缺失时返回真实错误，不能去 executor/stage/store 找替代文档。
3. executor 的请求配置仅处理输出、decode、ROI、sink、cache 等运行参数，不能修改持久 Model。
4. RAW camera/profile/lens 参数在正常导入或 document 加载时准备到位；
   移除任务调用方为复制 executor 而重新灌入 stage 参数的动作，不改变既有解码质量。
5. 保留现有 Renderer 资源策略；没有证据时不重写缓存或强制共享全部 GPU 中间资源。

验收：

- `EditorAndHostRenderUseDocumentParameters`：实际改变 document 参数后，编辑器/host 输出符合该值；
  用可区分的像素结果或可靠的数值参考证明，不只断言输出非空和 JSON 未变。
- `RenderLeavesPersistentDocumentParametersUnchanged`：不同任务前后持久参数、节点和边相同。
- 对默认图运行真实 RAW；GPU 失败不进入 CPU、旧 stage 或较低质量出图。

#### NM1.4 B completion record (2026-08-30)

**Status:** complete — 产品 `Apply` 只读已绑定 document；host/thumbnail/export 走 BypassSessionCache，复用同一 one-shot device 并在交付后释放 workspace；并行独立 renderer 的 one-shot 分配可完成且不写入 session cache。

**实现与验收清单：**

- [x] `ApplyGpuDagProduct` 不再调用 `LegacyPipelineImporter::ApplyOnto`；editor/host 都从绑定 document 取参数，stage 曝光与 document 故意不一致时像素仍跟 document。
- [x] 未绑定 document 时 `Apply` 抛出真实错误，不创建 renderer、不执行 stage、不写输出。
- [x] `BuildGpuDagRenderRequest` 只翻译 viewport/resize/quality；`RenderLeavesPersistentDocumentParametersUnchanged` 证明节点、边、Model 地址和 stage JSON 在多种请求配置后不变。
- [x] `InjectRawMetadata` 仅在导入/显式加载路径写入 Develop；任务 `Apply` 不从 stage 补相机矩阵。缺 document 相机配置失败，不读 stage metadata。
- [x] Thumbnail/export 的 `SetExecutorRenderParams` 关闭 session cache；连续 Bypass 复用 one-shot device，交付后 texture pool used bytes 为 0；两条并行 Bypass 使用不同 device，均释放 workspace，且不增加 session prepared-source 计数。

**Primary success call chain:**

```text
CPUPipelineExecutor::Apply (bound document)
  -> BuildGpuDagRenderRequest (viewport/decode/resize/quality only)
  -> cache_policy = enable_cache_ ? UseSessionCache : BypassSessionCache
  -> ApplyGpuDagProduct -> Renderer::Render(document, request, cache_policy)
  -> session: prepared-source / plan / published GPU results
  -> bypass: EnsureOneShotDevice (reuse) -> CompileStatic -> Execute
     -> download/present -> ReleaseSessionResources on one-shot workspace
  -> pixels match document parameters; persistent Models unchanged
```

**Primary failure call chain:**

```text
missing PipelineDocument
  -> throw before renderer construction
  -> no stage execution, no frame-sink notify, no GPU substitute
GPU presentation/download failure
  -> discard unpublished; bypass also WaitIdle + ReleaseSessionResources
  -> propagate runtime_error; last successful display retained
  -> no CPU / legacy stage / lower-quality retry
CPU / unsupported backend preference
  -> throw "supported GPU backend"; no legacy stage Apply
```

**What was proven (executed tests):**

| Required name / criterion | Target / suite | Result |
| --- | --- | --- |
| `EditorAndHostRenderUseDocumentParameters` | `PipelineDocumentRenderTest` | PASS；stage 曝光 ±9 EV 时 editor/host 像素跟 document −1.5 / +1.5 EV 参考，INF < 2e-5，明暗可区分 |
| `RenderLeavesPersistentDocumentParametersUnchanged` | `PipelineDocumentRenderTest` | PASS；host/editor 交替改 decode/ROI/cache 后 document JSON、stage JSON、Exposure 地址不变 |
| 默认图真实 RAW；GPU 失败不降级 | `DefaultDocumentRendersRealRawAtFullDecodeAndOutputResolution`、`FailedGpuPresentationPropagatesErrorWithoutSubstituteOutput`、`CpuPreferenceFailsInsteadOfExecutingLegacyStages` | PASS；FULL decode 输出尺寸等于 DNG；拒绝 mapping 后无 host 帧；CPU 偏好抛错且 renderer 仍为空 |
| `MissingDocumentFailsWithoutExecutingStages` | `PipelineDocumentRenderTest` | PASS |
| `HostBypassRendersReuseOneShotDeviceAndLeaveSessionCacheUntouched` | `PipelineDocumentRenderTest` | PASS；两次 host Bypass 同一 one-shot device，pool used=0，随后 editor 仍命中 session prepared source |
| `RepeatedOneShotRendersReuseDeviceAndReleaseWorkspace` | `GpuDagCudaDrtProductTest` / `CudaResultCacheProductFixture` | PASS |
| `ParallelOneShotRendersCompleteAndReleaseWorkspaces` | `GpuDagCudaDrtProductTest` / `CudaResultCacheProductFixture` | PASS；两线程独立 renderer 同时 Bypass，各自 device 非空且不同，published/pool=0，session hits/misses=0 |
| `ThumbnailAndExportTasksDisableSessionCache` | `PipelineFrameSinkTest` | PASS；THUMBNAIL/FULL_RES_EXPORT 关闭 cache 并 force CPU output；FAST_PREVIEW 恢复 session cache |
| `OneShotRenderDoesNotReadWriteOrClearEditorSessionCaches`、`RendererOneShotWorkspaceCannotPublishIntoSessionCache` | `GpuDagCudaDrtProductTest` | PASS（既有） |

Commands（仓库根目录）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineDocumentRenderTest GpuDagCudaDrtProductTest PipelineFrameSinkTest
ctest --test-dir build/debug -R "PipelineDocumentRenderTest|GpuDagCudaDrtProductTest|PipelineFrameSinkTest" --output-on-failure
```

Suite totals:

- `PipelineDocumentRenderTest` **10/10 PASS**
- `PipelineFrameSinkTest` **34/34 PASS**
- `GpuDagCudaDrtProductTest` **41/43 PASS** — 本次新增的 one-shot 复用/并行测试均 PASS；失败的 `ProductRendererViewportAndMaxEdgeResampleDecodedSourceWithoutSizeMismatch` 与 `ProductRendererRendersLegacyImportWithTintWithoutUnregisteredType` 未改其源码，属既有 viewport/tint 问题，不是 B 的 document 只读或 Bypass 证据

最终日志：`build/tmp/nm1/nm14b-ctest.log`、`build/tmp/nm1/nm14b-frame-sink-ctest.log`。

**Checklist / exit condition:** 第 10.2 节五项工作与三条验收已完成。第 14 节仅能勾选由 B 证明的 Apply 只读 document；共享 executor、无保存 release、统一 I/O 仍属 C / NM1.5。

**LOC note（grill-code-review）：**

| 本次文件 | 总 LOC | Diff + / - |
| --- | ---: | ---: |
| `src/edit/pipeline/pipeline_cpu.cpp` | 810 | + / − 以删除 ApplyOnto remirror 为主（约 −200 净） |
| `src/include/edit/pipeline/pipeline_cpu.hpp` | 273 | +46 / − 少量 |
| `src/include/edit/runtime/renderer.hpp` | 306 | +36 / −0（`OneShotResources`、`DebugOneShotDeviceIdentity`） |
| `tests/edit/pipeline/pipeline_document_render_test.cpp` | 358 | 新文件（含 HostBypass 复用断言） |
| `tests/edit/runtime/cuda_result_cache_test.cpp` | 735 | +120 / −0 |
| `tests/edit/pipeline/pipeline_frame_sink_test.cpp` | 1086 | +30 本测试；其余为既有 SetExecutorRenderParams 改为读 request snapshot |
| `tests/edit/CMakeLists.txt` | 496 | +9 / −0（注册 `PipelineDocumentRenderTest`） |

`pipeline_frame_sink_test.cpp` 已超过 1,000 LOC：本次只在 `SetExecutorRenderParams` 组旁增加 cache 开关断言，未再向该文件加入 GPU 像素或并行分配职责。按 sink 生命周期 / 请求参数 / 并发锁拆分属于后续整理，不是 B 的退出条件。`cuda_result_cache_test.cpp` 仍低于 1,000 LOC。

**Residual gaps / scope boundaries:** C 的共享 live executor、snapshot API 删除、后台 release 不保存仍未做；thumbnail/export 当前仍可持有独立 snapshot executor，B 只证明 Bypass 不建 session cache、one-shot device 复用且交付后释放、并行独立 renderer 不互相写入 session cache。`RemirrorGpuDagDocument` 仍在 `pipeline_service` 的 load/save 边界，属 NM1.5。上述两条既有 `GpuDagCudaDrtProductTest` 失败未在 B 中修复。无完整应用交互或 Metal/OpenCL 对等 Bypass 并行证据。

### 10.3 C：共享 pipeline 使用权

工作：

1. cache hit 返回同一图片已有 document/executor；cache miss 走同一创建/读取入口，不建立后台专用加载链。
2. 任务持有使用句柄至真正完成；缓存淘汰不能使使用中的对象失效，也不能同时创建同图片的另一套 live 对象。
3. 获取、释放与保存分开。释放最后一个后台使用权也不写存储、不清 dirty；
   编辑器仍持有使用权时不能清掉其 GPU session cache。
4. thumbnail、analysis 和 export 都使用普通 pipeline 句柄；迁移调用方后删除 snapshot 专用 API。
5. `SetForceCPUOutput`、decode/resize、frame sink 等共享 executor 配置只在 scheduler 锁内发生。
   使用既有请求参数保存/恢复机制，不另加一份编辑状态快照。
6. 任务使用取得 render lock 时的 document，渲染期间不允许写入。
   缩略图不保证与编辑器每帧同步；analysis/export 的单次输出必须来自同一次连贯读取，
   不宣称冻结了排队时刻的参数。NM1 不新增按任意历史 HEAD 导出的能力。
7. 沿用现有缩略图失效/取消机制。若磁盘结果按提交标签缓存，不能把排队时旧标签贴到执行时新像素上；
   预览值也不能冒充已提交结果写入该缓存。必要时只不写这次磁盘缓存，仍交付实际渲染结果。
   这是现有缓存写入的正确性要求，不新增 hash/token/修订号，也不恢复冻结 document。
8. 所有完成回调只终结一次；避免 callback 在 render lock 内释放资源时重复取锁。

主调用链：

```text
Thumbnail / Analysis / Export
  -> acquire normal pipeline handle (same element => same live document/executor)
  -> PipelineTask(input, RenderRequest, handle)
  -> scheduler: lock -> configure -> Renderer::Render(document) -> restore -> unlock
  -> deliver output / real error / cancellation
  -> release handle (no save)
```

关键验收：

| 测试 | 必须观察到的行为 |
| --- | --- |
| `BackgroundTasksReuseLivePipelineAndDocument` | cache hit 复用同一对象；任务返回实际 DAG 结果 |
| `BackgroundCacheMissUsesNormalDocumentLoad` | 无后台专用参数导入；并发取得同图片使用权不产生第二份 live |
| `ThumbnailThenEditorRestoresTaskParameters` | 同 executor 上编辑器→缩略图→编辑器；ROI/decode/sink/输出模式正确恢复 |
| `AnalysisAndExportUseSharedExecutorWithoutChangingEdits` | 单次读取连贯；分析和完整质量导出不改用户参数 |
| `CanceledAndFailedTaskReleasesPipelineUse` | 取消/配置异常/Apply 异常各自终结一次，释放 pin、恢复请求状态 |
| `BackgroundReleaseDoesNotSaveOrClearEditorState` | 无 DB 写入，不清 dirty；编辑器仍持有时缓存不被整体释放 |
| `DocumentMutationWaitsForSharedRender` | 通过屏障控制读写顺序，观察到完整旧值或新值，无半更新；不用 sleep 猜时序 |
| `QueuedRenderDoesNotStorePixelsUnderStaleCommitLabel` | 排队后编辑不会造成错误缓存标签；预览不污染已提交磁盘结果 |

### 10.4 修改范围

- `alcedo_studio/src/app/editor_pipeline_command_service.cpp`
- `alcedo_studio/src/edit/graph/pipeline_graph_commands.cpp`
- `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp`
- `alcedo_studio/src/include/app/pipeline_service.hpp`、`src/app/pipeline_service.cpp`
- `alcedo_studio/src/app/thumbnail_service.cpp`、`src/app/export_service.cpp`
- `alcedo_studio/src/include/renderer/pipeline_task.hpp`、`src/renderer/pipeline_scheduler.cpp`
- `alcedo_studio/src/include/edit/pipeline/pipeline_cpu.hpp`、`src/edit/pipeline/pipeline_cpu.cpp`
- 对应 graph、command、history port、pipeline、thumbnail、export 和 scheduler 测试。

以上路径中缩写的 `src/...` 均相对于 `alcedo_studio/`。
优先删除旧分支并复用现有函数；文件变大时按已有职责拆分，不用新服务层包住旧复杂度。

## 11. NM1.5 — 保存读取与阶段验收

Status: not started。

### 11.1 工作

1. `SavePipeline` / `SyncPipelineDocument` 从锁保护的 document 序列化。
   不 `Import(stages)` 替换图，不写 `legacy_stage_adapter`，不覆盖 history HEAD。
   编辑器保存前先完成或取消现有预览输入，再保存 settled 状态；不把 preview 标成已提交 checkpoint，
   也不为绕过这个顺序保留第二份 committed document。
2. 序列化内容与 dirty 处理对应同一次受保护的状态；写入失败不清 dirty、不清 WAL。
   保存期间若释放锁，必须保证后续编辑不被这次保存误标为已保存；优先使用现有串行保存边界。
3. 普通 document 读取集中在一处：解析当前格式并验证节点、边和主链。
   不“试 live，失败试 executor，再试 store/stage”；单个来源无效就报告错误。
4. mapper 的产品 document 读写不解包 stage adapter；正常新图片初始化直接从 Default 工厂和图片固有参数建图。
5. 从产品 load/apply/save 路径移除 remirror；现有旧 history 加载若不能处理新节点记录，不得通过重放 stage 覆盖图。
   不新增 history root snapshot，不在 NM1 改 commit/WAL schema 或实现节点重放。
6. 释放句柄只管理生命周期；现有调用方确实需要保存时显式调用保存，不能靠后台释放顺带保存。
7. 项目打开继续使用既有 metadata 校验入口；新版本常量和旧项目全面拒绝在 NM4 一次切换。
   NM1 的新图测试使用当前开发格式，不添加旧项目升级 fixture 或迁移代码。
8. 审核 NM1.4 的所有调用方、字段缓存失效、资源清理和失败路径，执行第 14 节退出检查。

成功链：

```text
local edit functions -> live document
  -> explicit save under coherent access -> document JSON in storage
  -> release / normal load -> same nodes, edges, parameters
```

失败链：

```text
invalid stored document -> report load error -> no importer/default substitute
storage write failure -> keep live edits and dirty/WAL -> report save error
background release -> no persistence attempt
```

### 11.2 验收

| 测试 | 必须观察到的行为 |
| --- | --- |
| `DocumentSaveReloadPreservesNodesEdgesAndParameters` | 额外 Grade、删除 primary、重接、参数、enabled/name 均 round-trip；这是 document I/O，不是 history reopen |
| `SavedDocumentContainsNoStageAdapter` | 持久图不包含旧 stage 镜像，保存不重建节点 |
| `InvalidStoredDocumentFailsWithoutReplacement` | 缺失字段、非法拓扑、损坏值准确失败；不生成默认图 |
| `FailedDocumentSaveKeepsDirtyStateAndJournal` | 失败保留未保存状态及已有日志，history head 不被改写 |
| `SaveDoesNotPersistUnsettledPreviewAsCommittedState` | 保存遵循完成/取消输入的顺序，重读结果不把预览冒充 settled 值 |
| `ImportCreatesRenderableDocumentWithoutStageMirror` | 新图 Default 参数、相机/镜头固有数据完整；未打开编辑器也能 DAG 出图 |

主要范围为 pipeline service、pipeline mapper、必要的 import 初始化、对应 app/storage 测试。
只有读取边界测试需要覆盖非法格式；不再让每个渲染调用方重复证明“stage-only 项目被拒绝”。

## 12. API 与所有权约束

保留简单的领域函数形态，例如：

```cpp
ApplyEditorParameterPatch(PipelineDocument& document, target, params, error);
AddCleanColorGrade(PipelineDocument& document, before_node_id, new_id);
RemoveColorGradeAndBridge(PipelineDocument& document, node_id);
ReconnectColorGrade(PipelineDocument& document, node_id, predecessor, successor);
```

以上为形态说明，不要求新增同名接口或通用命令类型。函数操作当前文档；锁和 WAL/history 由 app 边界协调。
共享生命周期使用现有 guard/pin 机制，缺少无保存 release 时只补最小对称释放能力。
不新增 snapshot registry、租约服务、发布 token 或每消费者一个 executor。

## 13. 验证方式与证据

以下为 NM1 全阶段验证要求；NM1.4 A 的已执行命令、结果和范围见第 10.1 节，B 见第 10.2 节；C 与 NM1.5 仍须独立验收。

Windows 构建遵循 `alcedo-msvc-cmake` 技能，从仓库根目录运行：

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest PipelineMapperTest ThumbnailServiceTest EditorPipelineCommandServiceTest EditorSessionHistoryPortTest
ctest --test-dir build/debug -R "GpuDagModelGraphTest|PipelineMapperTest|ThumbnailServiceTest|EditorPipelineCommandServiceTest|EditorSessionHistoryPortTest" --output-on-failure
```

同时从实际 CMake/CTest 清单确认并运行 export、pipeline scheduler、editor controller 和 import 受影响目标；
不猜测尚未存在的 target 名。测试矩阵中的新名称是行为要求，可以合并为参数化测试，不机械地每行新增一个文件。

先运行各修改段的聚焦测试，最后运行完整受影响 suite。GPU 数值断言使用受支持真实 RAW 或固定可验证输入，
保留当前质量设置；缺 fixture/后端时记录未验证，不把跳过写成通过。并发测试用确定的屏障。
代码检查确认产品路径无整图 clone、stage remirror 和 snapshot executor 创建；输出比较不能只检查 buffer 非空。

临时工具、日志、测试证据仅放 `build/tmp/nm1/`。完成记录须包含实际命令、测试结果和未覆盖项；
不能以旧 snapshot 测试通过替代共享 executor 与失败路径证明。

## 14. NM1 退出条件

- [ ] 同一图片只有一个 live document/executor；后台任务复用普通使用句柄。
- [x] preview/settled/参数 Undo/Redo 不深拷贝整图；图命令只保留局部恢复数据（NM1.4 A）。
- [ ] document 读写、渲染和序列化遵守统一互斥，完成/取消/异常没有锁重入或使用权泄漏。
- [ ] Add/Remove/Reconnect/Rename/Enabled 保留；非法操作恢复节点、边、值和已支持的 history 状态。
- [x] Default 工厂自带 1.5 EV/1.3 saturation；Clean 工厂无视觉变化且无四项后处理（既有证据，集成时复测）。
- [x] 完整 target、输入序列锁定、未知字段/Mask 拒绝及同会话参数 Undo/Redo 在简化后重新通过（NM1.4 A）。
- [ ] 编辑器、缩略图、analysis、export 从同一 document 得到正确 DAG 像素；单次参数不污染后续任务。
- [ ] 后台完成不保存、不清 dirty；最后一次使用结束前不淘汰对象，编辑器在用的缓存不被整体释放。
- [ ] 产品 Apply/Load/Save 不再使用 stage 镜像或 stage 作为参数源；旧类型可留作非产品代码。
- [ ] 新图导入和 document 保存读取完整；存储失败保留未保存状态，坏图读取真实失败。
- [ ] NM1 未引入完整 typed history、跨进程节点重放、旧项目迁移或额外 root/document 副本。
- [ ] 不开放依赖 NM2/NM4/NM5/NM6 的生产入口；不宣称节点 history/recovery 或多 Grade GPU 已完成。
- [ ] 第 10/11 节行为有执行证据；第 16 节追加本版实施结果，NM1.4/NM1.5 才能改为 complete。

## 15. 交给后续 Phase 的边界

- **NM2：** 在现有单 live document 和共享 executor 上执行完整 Grade 主链，迁移 DRT/Post，完善按节点缓存；
  不恢复独立参数副本来绕过任务状态问题。
- **NM3：** Mask 字段和资源操作遵循局部修改原则；不可变像素资产不要求 document 不可变。
- **NM4：** 使用 NM1 领域函数实现 typed `PipelineEditBatch`、forward/inverse、节点 WAL/recovery、Version/Paste；
  替换 `document_edit_by_commit` 等临时记录，切换 checkpoint/history/project metadata 格式。
  以 history 为准重放，checkpoint hash 只用于跳过不必要重放；不保留旧项目兼容读取和 stage 提交迁移。
- **NM5/NM6：** 在模型、真实多节点渲染和 history 完成后接 UI；不把 QML 对象当作模型或复制存储。
- **Stage 类型删除：** 后续删除仍未被产品使用的旧类型及测试 importer，不为保留它们增加新镜像。
- **NML：** 已取消，不创建旧项目升级阶段。

## 16. 既有证据与本版状态

以下为 2026-08-29/30 原执行记录的摘要，保留其实际测试范围。旧复制调用链不再是实施要求。

| 原阶段 | 已记录证据 | 本版解释 |
| --- | --- | --- |
| NM1.1 | `GpuDagModelGraphTest` 42/42；`GpuDagRawInputTest.GpuDagGraphCompiler` 18/18 PASS | 图功能与编译行为保留；原地实现要复测 |
| NM1.2 | `GpuDagModelGraphTest` 46/46 PASS | Default/Clean 功能保留 |
| NM1.3 | HistoryPort 46、AdjustmentPipeline 6、CommandService 4、SessionEditController 11、ActionPolicyCq3 10；合计 77/77 PASS | 完整 target 与同会话参数行为保留；不证明跨进程 history，锁与复制要修正 |
| 未提交 NM1.4 | PipelineMapper filter 5/5、Thumbnail filter 4/4、ModelGraph filter 1/1 PASS | 只证明原 snapshot 实现下的检查；不足以通过本版共享渲染验收 |

旧记录主要行为名：

- `RemovePrimaryGradeKeepsRemainingGradesAndValidBackbone`
- `RemoveLastColorGradeLeavesDevelopConnectedToDrt`
- `GraphCompilerCompilesFirstBackboneGradeWhenPrimaryIdIsAbsent`
- `DefaultPipelineDocumentBakesOnePointFiveEvAndSaturationOnePointThree`
- `MakeCleanColorGradeUsesIdentityParamsAndOmitsPostAdjustments`
- `IncompleteTargetRejectedLeavesDocumentHashAndHistoryHeadUnchanged`
- `ProvisionalSequenceReusesTargetResolvedAtFirstPatch`
- `UndoSettledExposureRestoresDocumentValue`
- `JournalAppendFailureRestoresDocumentExposureEv`

历史命令使用第 13 节 MSVC wrapper 和对应 CTest suite；原 NM1.4 使用可执行文件 filter：
`*LoadPipelineSnapshot*`、`*StageOnlyStore*`、`*ThumbnailRenderUsesGpuDagDocument*`、
`*AnalysisRenditionUsesSameDocumentSnapshot*`、`*DefaultPipelineHasDevelop*`。历史日志位于 `build/tmp/nm1/`。

已知证据限制：原 NM1.4 的 `mfzoty.dng` 用 image pool + LibRaw 准备输入，因本机 Exiv2 路径打开失败
未证明完整 ImportToFolder 链；它也不能替代 import 初始化验收。QML field-only 请求仍被拒绝；
非 primary Grade 的实际 GPU 执行和跨进程节点恢复分别属于 NM2/NM4。

本版方案更新（2026-08-30）：**documentation only**。源码/测试保持原样，没有新 build/test 结果。
实施后追加真实成功链、失败链、命令、结果及缺口，再更新状态和退出清单。

NM1.4 A 实施结果（2026-08-30）：**complete**，本版验收 121/121 PASS，成功链、失败链、测试和缺口见第 10.1 节完成记录。

NM1.4 B 实施结果（2026-08-30）：**complete** — 产品 Apply 只读绑定 document；BypassSessionCache 复用 one-shot device 并释放 workspace；并行独立 renderer 的 one-shot 分配通过。证据见第 10.2 节完成记录。NM1.4 C、NM1.5 的状态未提前提升。
