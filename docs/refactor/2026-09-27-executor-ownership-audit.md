# Executor / Document 所有权审计（重构前现状）

Date: 2026-09-27
Branch: `fix/editor-pipeline-single-owner`（作为本次重构的方案分支）
Base commit: `671802168 fix(editor-history-ownership)`

本文只记录现状与为现有所有权模型付出的补丁，是后续重构方案的依据，不含实施计划。
路径均相对 `alcedo_studio/src/`，行号以 base commit 为准。

---

## 0. 一句话结论

`PipelineGuard` 是一个**按图片缓存、所有模块共享**的对象，里面同时装着：GPU executor、可变的
live document、CommitGraph 历史、缓存 pin 计数、持久化脏标记、编辑器会话标记。executor 的
`render_lock_` 同时充当 GPU 渲染锁、文档写锁和历史所有权锁。缩略图、导出、AI 分析、导入、
粘贴、复制对话框、编辑器全部通过 `LoadPipeline` 拿到**同一个** guard。

过去九个月里 `PipelineGuard` 的每一个字段几乎都是为修一个"共享导致的 bug"而加的，
`pipeline_service.cpp` 61 次提交、`pipeline_executor.cpp` 107 次、`pipeline_scheduler.cpp` 75 次。

---

## 1. 融合在一起的东西

`include/app/pipeline_service.hpp:47-99` `PipelineGuard`：

| 类别 | 字段 | 引入 |
|---|---|---|
| 执行器 | `pipeline_` | — |
| 可变文档 | `document_` | — |
| 缓存生命周期 | `pinned_`（只写不读）、`pin_count_`、`live_ready_`、`initializing_`、`load_error_` | d41daf587 (02-07)、ff5ba421a (08-31) |
| 持久化 | `dirty_`、`serialized_state_needs_writeback_` | 07–09 月 |
| 编辑器会话 | `unsettled_preview_`、`editor_owned_` | ff5ba421a、671802168 (09-27) |
| 历史 | `commit_graph_`、`root_id_`、`root_document_` | 07–09 月 |

`PipelineExecutor::render_lock_`（`include/edit/pipeline/pipeline_executor.hpp:54-58`）三锁合一：

1. GPU 渲染互斥；
2. 文档写锁（`pipeline_service.cpp:506-508` 注释直接称其为 "the document lock"）；
3. 历史所有权锁（`LockLivePipeline`，`editor_history_shared_helpers.cpp:15-23`，
   `editor_history_mutation.cpp` 中约 30 处）。

并且**渲染期间一直持有到 present 交接完成**（`renderer/pipeline_scheduler.cpp:371-381`，
`ui/editor_rhi/direct_frame_sink.cpp:132-136`）。

隐藏的第二个 executor 注册表：`Storage::live_pipelines_`（`sleeve/storage.hpp:24,51`，
`sleeve/storage.cpp:99-121`）。LRU 驱逐不会忘记它，只有 Delete 会。结果是项目会话期间
**每张打开过的图片都保留一个 executor 和它的 session device**，16 项 LRU 根本不约束 GPU 资源。

---

## 2. 为共享模型付出的补丁清单

### 2.1 PipelineMgmtService 层

| # | 位置 | 补丁 | 被什么逼出来的 |
|---|---|---|---|
| S1 | `pipeline_service.cpp:335-499` | `LoadPipeline` 三段状态机：命中→等 `live_ready_`；已缓存但闲置→重新 init executor；未命中→插占位 guard 再构建（占位重检复制了命中逻辑） | 缓存条目既是文档又是可共享的 executor；闲置清理后 executor 变"未就绪" |
| S2 | `:231-301` `HandleEviction` | 跳过被 pin/`editor_owned_` 的项；全被 pin 时 LRU 无上限 `+5` 扩容；在触发驱逐的线程上同步写 DB；失败还要把 guard 塞回并强置 `live_ready_=true` | GPU 状态和未保存编辑放在同一缓存项里：加载第 17 张缩略图可能触发另一张图的 DB 写 |
| S3 | `:303-322` `CleanupIdlePipelineResources` | 最后一个 pin 释放时清 GPU 缓存并 `DetachFrameSink`，锁序 render→cache，还要防重获取竞态 | 谁最后放手谁负责收拾编辑器的 sink 和 session 缓存 |
| S4 | `:735-847` `SavePipeline` vs `ReleasePipelineUse` | "保存"和"用完"是一个调用；checkpoint 只在 `pin_count_<=1` 时写（`:752-757`） | 持久化挂在缓存生命周期上；缩略图恰好持有 pin 时 checkpoint 被跳过，行为依赖竞态 |
| S5 | `:324-333` `WaitUntilPinCount` | 等 pin 计数 | 仅测试使用；G10 文档记录了两个因无锁读 `pin_count_` 导致的 flaky 测试 |
| S6 | `:592-642` `editor_owned_` + Acquire/Release/LoadEditorPipeline | 用标志模拟"编辑器独占" | 历史、文档、executor 都在任何人可 load 的缓存对象上 |
| S7 | `hpp:63-65` `unsettled_preview_`，检查点 5 处 | 编辑器拖动中的值不是 history head 时禁止保存/写缩略图盘缓存 | 预览写进的就是被持久化、被缩略图/导出渲染的同一个文档 |
| S8 | `:220-229` `BindLivePipelineDocument` + 4 处失败回滚 lambda | 换文档要同时换 guard 和 executor 里各 renderer 的指针，失败要在锁内绑回旧文档 | 文档指针存两份（guard + Renderer::document_） |
| S9 | `:1104-1127` `SetAcceleratorBackendPreference` | 遍历所有缓存 guard 切后端；每次 load 还要重新应用一次 | executor 一图一个散落在缓存里；生产中唯一调用在缓存为空时，fan-out 循环实为死代码 |
| S10 | `:540-590` `InitializeImageRoot` | 为给文档绑相机 profile 而拿 executor 的 render lock | render lock 是事实上的文档锁 |
| S11 | `import_service.cpp:30-49` | 导入为了写 root 文档构造了完整 executor、解析后端、注册到 Storage | 文档只能通过 guard（带 executor）存在 |
| S12 | `:1058-1102` + `sleeve_service.cpp:123` | 删除要同时 `ForgetLivePipeline` | Storage 注册表 |

### 2.2 渲染层（executor / scheduler / renderer）

| # | 位置 | 补丁 | 被什么逼出来的 |
|---|---|---|---|
| R1 | `renderer.hpp:69-78,231-245`，`renderer.inl.hpp:93-124` | `RenderCachePolicy::BypassSessionCache`：同一个 Renderer 里再建一个 one-shot device，每次重新 `CompileStatic`、重新解包 RAW | 缩略图/导出在编辑器的 executor 上跑，必须不污染编辑器 session 缓存 |
| R2 | `pipeline_scheduler.cpp:128` vs `181-203` | `MakeApplyRequest` 默认取 executor 上的 sink，THUMBNAIL/EXPORT 再强制置空 | sink 是 executor 上的持久状态 |
| R3 | `editor_session_render_scheduler_port.cpp:460-469`，`pipeline_task.hpp:71-73` | 每个编辑器任务在锁内 `configure_under_render_lock_` 重新 AttachFrameSink | 最后一个 pin 的清理会 detach 编辑器的 sink |
| R4 | `pipeline_scheduler.cpp:249-261` | 自定义 deleter 的 `completion_guard`，保证 `on_complete_` 在所有锁释放后才跑 | `on_complete_` 里要 `ReleasePipelineUse`，后者要拿 render lock |
| R5 | `pipeline_scheduler.cpp:291-309,442-461` | 通用 scheduler 对 THUMBNAIL 特判失败回调、`require_gpu_valid` | 一个 scheduler/任务类型服务两种完全不同的工作 |
| R6 | `render_service.hpp:19-24` | 缩略图、分析、导出共用一个静态池（hw/2 线程），导出阻塞式 FULL_RES 可饿死缩略图 | 没有按所有者分队列 |
| R7 | `runtime_invalidation.cpp:268-287`；`TakePendingDirtyFields` 于各 pass | **渲染器会清文档上的 dirty 位**，`Execute` 因此拿非 const `PipelineDocument&` | dirty 位是存在共享文档上的"单消费者"变更协议 |

R7 是本次重构的**硬前置条件**：只要 dirty 位还是文档上的可变状态且由渲染消费，
两个 executor 就不能读同一份文档（哪怕是不可变的）。疑似现存隐患：编辑器改参数后，
同图的缩略图 one-shot 渲染先跑、消费了 dirty 位，编辑器 session arena 看到 `!IsDirty()`
（`grade_parameter_slot.hpp:36`）而漏更新。`BackgroundRendersKeepEditorResultCacheReusable`
测试在两次渲染之间没有编辑，覆盖不到。

### 2.3 编辑器层

| # | 位置 | 补丁 | 被什么逼出来的 |
|---|---|---|---|
| E1 | `editor_history_shared_helpers.cpp:15-23` + mutation 约 30 处 | 每次参数写都拿 render lock，直接改 `guard->document_` | executor 渲染的是一份它不拥有的可变文档 |
| E2 | `editor_session_service.cpp:1831-1844`，`editor_serial_frame_admission.cpp:32-55,114-132` | `DeferIfLiveOwnershipHeld`：帧在飞时把 Checkout/Undo/Redo 排队延后 | 历史操作要等 render lock，而 render 在等 GUI 线程 present |
| E3 | `editor_session_render_scheduler_port.cpp:129-146,517-539` | GUI 线程等渲染空闲时 `processEvents` 泵事件 | 同上：持锁的 worker 等 GUI present，GUI 又要等这把锁 |
| E4 | `editor_history_mutation.cpp:321-509` | `CaptureAdjustmentBeforePreview` / `RestoreUnsettledPreview` 为每个字段保存前后 JSON | 预览值直接写进被持久化的文档 |
| E5 | `editor_session_service.cpp:612-622` | 每次通知前完整 clone 一份 `PipelineDocument` 给 GUI 读 | GUI 不能碰锁下被改的 live 文档 |
| E6 | `editor_session_pipeline_port.cpp:19-23,69-108` | Pipeline port 的 `Acquire` 什么都不做，真正加载由 history port 触发并补了 `load_mutex_` | "pipeline" 和 "history" 是同一个 guard，谁先碰谁加载 |
| E7 | `editor_history_state_detail.cpp:34-59` | Acquire/Ensure 拆分 + `history->graph() != guard->commit_graph_` 身份检查 | `MiniGitWorkingHistory` 与 guard 各持一份 CommitGraph 指针，被重绑会静默分叉 |
| E8 | `editor_history_state_detail.cpp:281-316` vs `pipeline_service.cpp:900-1049` | 两条文档重放路径（`PipelineMapper()` 为空时走 history 自带的） | service 拥有 guard 所以拥有"重放+绑定"，history 又需要一份备用 |
| E9 | `editor_session_navigation_controller.cpp:246-283` | 每次 seal 都要 render-idle barrier，再和 checkpoint 结果汇合 | 释放 guard / 换文档要等 worker 不再持有 executor 和文档 |
| E10 | `editor_session_render_controller.cpp:83-94` → `graph_compiler.cpp:72-79` | 几何面板打开时"不裁切渲染"作为每帧 request 覆盖 | 编辑器不能持有自己的图变体，只能对共享文档每帧覆盖 |
| E11 | `EditorSessionPipelinePort::CheckoutVersion` (`:110-130`) | 死代码，无调用方 | — |

`671802168` 本身就是这一类：渲染 worker、Version checkout、Copy 三条路径会对**已在编辑器中打开的图**
调 `LoadEditorPipeline`，从存储重绑 CommitGraph 和文档，导致未保存历史丢失或分叉。
修法只能是再加一个 `editor_owned_` 标志、在每个入口检查、补驱逐豁免、补 `SnapshotHistorySource`、
补边界处的 pending input 结算。

**滑块 → 画面** 当前链路：约 12 跳、3 次线程切换、7 个抽象 port、约 16 个具体类，
**每个滑块 tick 拿两次 render lock**（owner 线程写文档一次、worker 渲染一次），这是 E2/E3/E9 的根源。

### 2.4 其他消费者

| # | 位置 | 问题 |
|---|---|---|
| C1 | `thumbnail_service.cpp:457-514,601-610` | 每张缩略图 pin 该图自己的 executor；缓存未命中时为一张缩略图构造整套 executor+文档+renderer。编辑器打开的图，其缩略图与编辑器预览串行，并在整个 Apply 期间阻塞预览 |
| C2 | `thumbnail_service.cpp:231-307` | 每个请求都在调用线程（通常是 UI）开 DB 连接、`LoadGraph` 整个 CommitGraph，只为读 head hash——因为 `LoadPipeline` 读的元素 JSON 没有 head 标签 |
| C3 | `thumbnail_types.hpp:42-56`，`thumbnail_service.cpp:270-286` | 盘缓存写入门控读 `unsettled_preview_`/`dirty_`，但读的时机是渲染结束、锁释放之后：TOCTOU + 数据竞争；内存 LRU 和分析结果根本不受门控 |
| C4 | `thumbnail_service.cpp:166,211-229,689-916` | 单独的 `analysis_tokens_` 命名空间；缩略图与分析两条渲染路径近乎复制粘贴 |
| C5 | `image_analysis_service.cpp:329-358` vs `semantic_generation_service.cpp:509-539` | 两个逐字重复的 provider 适配器 |
| C6 | `export_service.cpp:117-231` | 导出借用 live guard，**不检查 `unsettled_preview_`**，打开中的图会导出编辑器拖动中的值；全分辨率 Apply 期间阻塞编辑器预览 |
| C7 | `import_export.cpp:976-992` | 入队时拿 render lock 只为读 DRT 输出色彩；真正渲染发生在之后，颜色和编辑可能不一致 |
| C8 | `adjustment_transfer_controller.cpp:111-132` | Copy 一张未打开的图：`LoadEditorPipeline` 构造 executor、重放文档，只为复制 CommitGraph |
| C9 | `adjustment_transfer_apply_coordinator.cpp:88-229` | Paste 在共享 guard 上原地改，失败要回滚 graph、文档指针、`dirty_`、写回标志；正确性实际靠 UI 交互锁保证 |

正面先例：`MaskThumbnailService`（`app/mask_thumbnail_service.cpp`）从文档构造值类型 spec，
由无状态 CPU worker 光栅化，不碰 executor——正是目标形态。

---

## 3. 已确认 / 疑似的正确性问题

已在源码中确认：

1. **`commit_graph_` 数据竞争**：`SetPipelineHistoryState`（`pipeline_service.cpp:212-216`）无锁替换
   `shared_ptr`，缩略图回调（`thumbnail_service.cpp:248-252`）只在自己的锁下读。经 Copy/Paste 可达。
2. **渲染器改写文档状态**：`RuntimeInvalidationState::CollectAndPropagate` 清 `TopologyDirty` 和
   `MixDirty`（`runtime_invalidation.cpp:275-287`）。
3. **打开编辑器时临时把 RAW 文档改成 Rec.709**：`BindEditorStateFromStorage`（`:648`）调用
   `InitializeImageRoot(pipeline)`，`raw_color_context` 为空 → `BindWorkingSpaceDevelopData`
   无条件把共享 live 文档的 Develop 相机 profile 改成 Rec.709（`:549-553`，`:106-117`）。之后才被重放文档替换；
   窗口期内同 guard 的缩略图可能渲染错色，重绑抛异常时错色文档留在 guard 上。
4. **`Storage::live_pipelines_` 让 executor/session device 活到项目关闭。**

疑似，需验证：

5. `LoadEditorPipeline` 在锁内检查 `editor_owned_` 后解锁再 `BindEditorStateFromStorage`，检查-执行竞态（`:595-604`）。
6. WAL 恢复路径对编辑器自己的 guard 调 `SavePipeline`（`editor_history_state_detail.cpp:183`），会释放编辑器的 pin，
   最后一 pin 清理可能清掉编辑器缓存并 detach sink。
7. discard/checkout 把 `dirty_` 置 false 但不写元素 JSON（`editor_history_mutation.cpp:953,1065`），
   驱逐/重启后 `LoadPipeline` 读到旧文档，而缩略图盘缓存 key 用的是新 head。
8. R7 描述的 dirty 位被后台渲染抢先消费。**已证实（2026-09-28，P0）：**
   `ExecutorIsolationTest.DISABLED_EditorSessionRenderShowsParameterChangeAfterInterleavedOneShot`。
9. 缩略图渲染的文档来源取决于缓存历史：元素 JSON（`:456`）或编辑器重放出的文档（`:700-721`）。

---

## 4. 历史：这条路走过一次回头路

| 时间 | 事件 |
|---|---|
| 07-02 `311cd5c25` | 分析改用 `PipelineSnapshot`（克隆出的独立 executor） |
| 08-23 `895d00a28` | G7R.H：缩略图/导出也改到 snapshot executor，因为共享 render lock 造成约 12.5 s 编辑器卡顿 |
| 08-28 `024a4e4f8` | 修 snapshot 拷到陈旧文档 |
| 08-31 `ff5ba421a` | NM1.4C/R **删除 snapshot**，回到"每图一个 live document/executor、guard/pin 共享"；同时加入 `ReleasePipelineUse`、`live_ready_`、`CleanupIdlePipelineResources`、`unsettled_preview_` 和 1064 行 `pipeline_shared_use_test.cpp` |

NM1 文档（`docs/roadmap/alcedo_studio/edit/node_mask_editor/phase_nm1_pipeline_document_editing_plan.md`
§10.5 背景 1、§12）明确写了"不恢复 PipelineSnapshot……不新增每消费者一个 executor"，
理由是：当时的 snapshot 是**按快照克隆出的 executor**（显存翻倍）、要复制参数/Model、快照文档会陈旧，
以及 SetEnableCache/Capture-Restore 这类模式开关状态。

新方案必须正面回答这段历史。关键区别在于：

- 当时失败的是"**每个快照/每张图一个 executor**"，executor 数量随图片和任务增长；
  现在要做的是"**每个所有者固定数量的长寿 executor**"（编辑器 1、缩略图池 N、导出 1），
  executor 数量是常数，与图片数无关——显存反而比现在（每张打开过的图一个 executor 留到项目关闭）更少。
- 当时共享的是可变文档 + 可变 dirty 位，快照只能深拷贝；现在要共享的是**不可变的编译产物**
  （plan + 带 revision 的参数快照），每个 executor 自己和上一次应用的 revision 做 diff。
- 当时的"陈旧快照"问题来自快照取自编辑器的 live 可变状态；新模型下非编辑器消费者只看已提交 head，
  快照按 `(element, head)` 天然有身份，不存在"陈旧"——只存在"不是最新 head"，这是正确语义。

以下已有决策与本方向冲突，需要在方案里显式废止：single-live-pipeline 计划 A1、
NM1 L549-550 / §12、G10 §6.1 "executor owned by PipelineGuard"。
以下未决项会被本方向顺带解决：2026-05-24 文档遗留的"frame sink 是 executor 持久状态"、
single-live 计划点名推迟的"param graph 与 GPU executor 拆分"、CQ 计划"命令处理不等 render mutex"
与后续写文档拿 render lock 的冲突、Issue #113 盘缓存标签、pin 计数 flaky 测试。

---

## 5. 两类工作的本质差异（来自代码）

| | 缩略图 / 分析 / 导出 | 编辑器实时预览 |
|---|---|---|
| 规模 | 多图批量，去重，吞吐优先 | 单图，延迟优先 |
| 分辨率 | 固定档位 256/512/1024/2048，RAW 降采样解码；导出 FULL 但只下载到 host | 永远 FULL 解码；长边 2560 fast / 4096 quality，视口大小的 detail ROI |
| 缓存 | 不需要增量；按 `(文档 hash, 解码档, 尺寸)` 缓存编译产物即可 | session 缓存：PreparedSource、plan cache、增量重渲染、LLF 等中间结果 |
| 输出 | CPU RGBA8 / 编码文件，按 commit 标签写盘缓存 | GPU present 到 `IFrameSink`，帧角色（QualityBase / DetailPatch） |
| 状态 | 应当无状态：任意 executor × 任意图 × 任意已提交 head | 有状态：sink、视口、session 缓存、未提交的工作值 |
| 取消 | 按 `(element, tier)` 的代次 token | 按 sink 的 request id |

---

## 6. 目标形态（仅作方向，方案另写）

```
History / DocumentStore      —— 每图 CommitGraph + root + checkpoint，单写者租约（取代 editor_owned_）
        │ (element, head) → 不可变 PipelineDocument
PipelineService（编译服务）   —— 文档 → 不可变 CompiledGraph（plan + 带 revision 的参数快照），纯值缓存，驱逐无 I/O 无 GPU
        │ shared_ptr<const CompiledGraph>
Executors（按所有者）
  ├─ EditorExecutor ×1       —— 持有 session device、永久 sink、session 缓存；只接收编译产物，帧边界换图
  ├─ ThumbnailExecutor ×N    —— 缩略图/分析共用池，one-shot device，无 sink
  └─ ExportExecutor ×1       —— 独立队列
```

编辑器的工作文档是会话私有的：滑块写文档不需要任何 GPU 锁，写完在帧边界编译/发布给自己的 executor；
只有 commit 之后才产生新的已提交 head，其他消费者才能看到。

按此形态，第 2 节中 S1–S12、R1–R6、E1–E3、E6–E9、C1–C4、C6–C9 均可删除或退化为平凡代码；
R7（dirty 位协议）必须先改。
