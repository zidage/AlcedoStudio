# Executor 所有权重构方案

Date: 2026-09-27
Branch: `fix/editor-pipeline-single-owner`
Base commit: `671802168`
现状审计：[2026-09-27-executor-ownership-audit.md](2026-09-27-executor-ownership-audit.md)（下文 S*/R*/E*/C* 编号均指审计中的条目）

路径均相对 `alcedo_studio/src/`。

---

## 1. 目标

1. **文档与执行彻底解耦。** `PipelineMgmtService` 只负责文档、历史和持久化，对外交出**不可变的管线图快照**
   （未编译）。编译（文档 → `ExecutionPlan`）在 executor 内完成，因为编译需要解码后的源布局
   （`DevelopCompileSource`），服务层拿不到。
2. **executor 按所有者持有，数量固定，与图片数无关。**
   - 编辑器：1 个交互式 executor。
   - 缩略图 / AI 分析 / CLIP：一个批处理 executor 池（N 个，可配置）。
   - 导出：1 个批处理 executor。
3. **executor 的状态只有**：GPU device、当前绑定的执行图（快照 + 编译出的 plan）、与该图关联的 GPU 资源和源缓存。
   不持有文档所有权、历史、pin、frame sink 以外的 UI 状态。
4. **GPU 资源由 executor 自己管理，不进任何 LRU。** executor 切换到另一张执行图（换绑定，见 §3.3）时，
   该图相关的 GPU 资源、plan cache、源缓存**全部立即释放**。
5. **渲染不写文档，也没有跨模块共享的锁。** 编辑器改参数时不拿任何 GPU 锁。

## 2. 不变的东西（不破坏项目结构）

- **目录与 CMake 结构不变。** 新代码放进现有目录和现有库：`app/`、`edit/graph`、`edit/runtime`、`edit/pipeline`、
  `renderer/`、`ui/alcedo_main/album_backend/`。只在测试需要独立链接时，才按现有惯例拆出独立 lib。
- **类名保留，职责收窄。** `PipelineMgmtService`、`PipelineExecutor`、`PipelineScheduler`、`Renderer<Backend>`、
  `ThumbnailService`、`ExportService`、`EditorSessionService` 以及编辑器的各个 port 接口都保留，改的是内部实现。
  不新建"总管理器"之类的上帝对象（NM1 §12 这条约束继续遵守）。
- **持久化格式不变。** DuckDB schema、`.alcd` 项目包、CommitGraph / WAL / checkpoint 格式、元素 JSON 格式都不改。
- **QML 可见 API 不变。** `EditorSessionController` 等 Q_PROPERTY / Q_INVOKABLE 保持源兼容。
- **呈现链路不变。** `IFrameSink` / `DirectFrameSink` / `EditorViewportRenderer` 的呈现链路保留。
- **保留的现有不变量：**
  - HEAD 只存在于 `CommitGraph` / `VersionRef`。
  - 打开中的图片只有编辑器会话能改它的历史（671802168）。
  - build-then-swap 的交换步骤 noexcept。
  - 缓存锁不跨越渲染、DB 或回调。
  - 后台任务不保存、不清脏状态。
- **每个阶段结束时应用可运行、测试全绿。** 不做一次性大爆炸替换。

## 3. 目标架构

### 3.1 三层

```
PipelineMgmtService（文档 / 历史 / 持久化）
  - 从存储加载、重放、保存；单写者租约（取代 editor_owned_）
  - 已提交快照缓存：(element_id) → shared_ptr<const PipelineGraphSnapshot>
    纯 CPU 值缓存，可以用 LRU，驱逐无 I/O、无 GPU
  - 编辑器打开的图：由编辑器会话在每次 commit 时发布已提交快照
            │
            │ shared_ptr<const PipelineGraphSnapshot>   （不可变、未编译）
            ▼
PipelineExecutor（按所有者持有）
  - Render(snapshot, input, request)
  - 内部：snapshot → plan（按拓扑 key 缓存，只缓存当前绑定）、参数打包（按 revision 比对）、执行、呈现 / 下载
  - 绑定改变 → 全量释放
            │
PipelineScheduler（每个所有者一个队列；编辑器已是 PipelineScheduler(1)）
```

### 3.2 `PipelineGraphSnapshot`

放在 `edit/graph/`：

```cpp
struct PipelineGraphSnapshot {
  std::shared_ptr<const PipelineDocument> document;   // 冻结，节点与工作文档结构共享
  sl_element_id_t                         element_id;
  PipelineLineageId                       lineage;    // 同一张图同一次加载的谱系；换图/重载即变
  head_commit_hash_t                      head;       // 已提交快照的 HEAD；编辑器预览快照为 nullopt
  transaction_chain_hash_t                chain;
  bool                                    committed;  // false = 编辑器预览（含未提交值）
};
```

- 非编辑器消费者**只能**拿到 `committed == true` 的快照。缩略图盘缓存 key、分析结果标签、导出配方，
  都直接用快照上的 `(head, chain)`。审计里的 C2、C3、S7 就此消失。
- 编辑器预览快照只在编辑器会话与编辑器 executor 之间流转，不进入 `PipelineMgmtService`。

### 3.3 executor 的绑定与释放规则

executor 的**绑定键** = `(lineage, source file identity)`。

| 事件 | 行为 |
|---|---|
| 渲染时绑定键与当前绑定不同（换图、Version checkout 重建谱系、重新加载） | 先 `WaitIdle`，再释放当前绑定的全部 GPU 结果纹理、transient、LLF 缓存、plan cache、源缓存和 neural demosaic 工作区。然后绑定新图。**device / stream / queue 保留**：它们属于 executor，不属于图。 |
| 同一绑定内拓扑变化（增删 Grade 节点） | plan 按 `StaticPlanKey` 重新编译；结果纹理由 revision 失效机制处理。不做全量释放。 |
| 同一绑定内参数变化 | 按 revision 只重新打包变化的槽，只重算下游结果。 |
| 批处理 executor 完成一个任务 | 任务即绑定。结束后全量释放（沿用现在的 ExactRelease 语义），device 保留。 |
| executor 析构 / 后端切换 | 全部释放。后端偏好在构造时确定；切换 = 重建所有者的 executor（后端切换本来就是重启生效，见 3008671ed）。 |

executor 集合里不再有任何"最后一个 pin 清理"和"闲置 LRU"逻辑。

### 3.4 编辑器数据流（目标）

```
滑块 → EditorSessionController::submitWrite
     → EditorSessionService（owner 线程）改写会话私有的工作文档（不拿任何 GPU 锁）
     → 冻结预览快照（COW，O(改动节点数)）
     → EditorRenderCoordinator 提交 {snapshot, intent}
     → 编辑器 PipelineScheduler(1) worker：EditorExecutor::Render(snapshot, ...) → IFrameSink
commit → MiniGitWorkingHistory 记录 → 冻结已提交快照 → PipelineMgmtService::PublishCommitted(element, snapshot)
                                                        → ThumbnailService 按新 head 失效 / 重绘
```

- 工作文档、`MiniGitWorkingHistory`、CommitGraph 都归 `EditorSessionService` 的会话状态所有，不再挂在缓存 guard 上。
- `render_lock_` 退化为编辑器 executor 的内部实现细节，或者直接删除（单 worker 天然串行）。
  历史操作不再等它，也不需要为它泵 GUI 事件。

---

## 4. 分阶段计划

阶段顺序依据两点：前一阶段必须让后一阶段成为可能；每一阶段结束时应用都可用。
P1、P2 是纯基础设施，不改变所有权。P3 起逐个消费者迁移。P7 统一删除共享机制。

### P0 基线与保护网

**目标：** 在动结构前固定行为基线，并补上现有测试覆盖不到的隔离场景。

- 记录基线测试结果：完整 ctest 结果，以及已知预存失败的清单（沿用 stash/rerun 的比对方式）。
- 新增失败测试，用来证明后续阶段修复了问题。当前代码下标为 `DISABLED_` 或预期失败：
  1. 同一文档交替做编辑器 session 渲染和 one-shot 渲染，两次之间改参数，编辑器输出必须反映新参数
     （对应审计 R7 的 dirty 位被抢先消费）。
  2. 编辑器拖动中导出打开的图，导出结果必须是已提交状态（C6）。
  3. 打开编辑器期间并发渲染同图缩略图，颜色不得变成 Rec.709（审计 §3 第 3 条）。
- **性能测量**（为 P2 设门槛），分别测典型文档（3 节点）和重笔刷蒙版文档：
  - `ClonePipelineDocument` 的耗时与分配量；
  - 每帧 `MakeStaticPlanKey` 与参数打包的耗时。
- 小修复（不依赖重构、用户可见）：`BindEditorStateFromStorage` 不再对已有 root 的 RAW 文档调用
  `BindWorkingSpaceDevelopData`（`pipeline_service.cpp:549-553`）。

**退出条件：** 基线已记录；新增测试已提交并标明预期；测量数据写回本节。

##### Phase P0 completion record (2026-09-28)

**Status:** complete — 基线已记录（定向套件，完整 ctest 未运行，见下）；三个保护测试已提交并在当前代码上证实失败；
成本测量已写回；Rec.709 小修复已落地并由新测试覆盖。

**小修复的主调用链（成功路径）：**

```text
EditorSessionPipelinePort / AdjustmentTransfer → PipelineMgmtService::AcquireEditorPipeline / LoadEditorPipeline
  -> BindEditorStateFromStorage(guard)
  -> InitializeImageRoot(guard, raw_color_context = nullptr)
       -> DB 锁内 GetImageEditState(id) → 已有 root
       -> 不拿 render lock，不改 live 文档（旧代码此处无条件 BindWorkingSpaceDevelopData → Rec.709）
       -> LoadGraph + GetRootSerializedPipelineState → SetPipelineHistoryState / CacheRootDocument
  -> checkpoint 或 BuildLiveDocumentFromRoot → BindLivePipelineDocument（render lock 内换文档）
  -> 同 guard 上的缩略图在任何时刻都只看到 RAW 相机 profile
```

**新 root 路径与失败路径：**

```text
InitializeImageRoot（无 edit state，导入时）
  -> DB 锁内查询为空 → 释放 DB 锁（避免 DB→render 锁序）
  -> render lock 内绑定 RAW context 或工作色彩空间 profile + ValidateProductDocument
  -> 重新拿 DB 锁并复查：仍为空 → CreateRootPipelinePersisted；已被并发创建 → 走"已有 root"加载分支
失败：ValidateProductDocument / DB 抛异常 → 异常原样上抛；已有 root 的 live 文档没有被改过，不需要回滚
```

**保护测试（`ExecutorIsolationTest`，`tests/app/executor_isolation_test.cpp`）：**

| 计划条目 | 测试名 | 当前代码 | 启用阶段 |
|---|---|---|---|
| 1. R7 dirty 位被 one-shot 抢先消费 | `DISABLED_EditorSessionRenderShowsParameterChangeAfterInterleavedOneShot` | **失败**（已证实）：曝光 +1.5 EV 后编辑器 session 渲染均值 0.635 < 改前 0.762×1.2，与新 executor 参考渲染最大差 0.273（容差 1e-4）。审计中的"疑似"R7 由此确认为真实缺陷 | P1 |
| 1 的对照 | `EditorSessionRenderShowsParameterChangeWithoutInterleavedOneShot` | 通过（去掉中间的 one-shot，其余相同） | — |
| 2. C6 拖动中导出 | `DISABLED_ExportDuringUnsettledEditorPreviewUsesCommittedState` | **失败**（已证实）：已提交导出与拖动中导出的 JPEG 最大差 98（容差 1） | P5 |
| 2 的对照 | `RepeatedExportOfEditorOwnedImageWithoutPreviewIsUnchanged` | 通过 | — |
| 3. 打开编辑器时 live 文档变 Rec.709 | `EditorOpenNeverExposesWorkingSpaceProfileOnLiveRawDocument` | 修复前**失败**：观察线程按缩略图的方式在 render lock 下读 live 文档，一次打开内看到 19338 次 Rec.709；修复后 0 次，通过。已启用 | P0（本阶段） |
| 小修复的确定性单测 | `PipelineMapperTest.InitializeImageRootOnExistingRawRootLeavesLiveCameraProfileUnchanged` | 通过 | P0 |

测试 3 按计划写成"并发读 live 文档"而不是"并发渲染缩略图"：缩略图读的就是同一个 guard 上的同一份文档，
直接断言文档 profile 比比较缩略图颜色更确定。`DISABLED_` 测试用 `--gtest_also_run_disabled_tests` 运行得到上面的失败数据。

**成本测量（`PipelineDocumentCopyCostTest`，`tests/edit/graph/pipeline_document_copy_cost_test.cpp`，win_debug，两次运行一致）：**

| 文档 | 规模 | `ClonePipelineDocument` | `MakeStaticPlanKey` | 打包全部 Grade 槽 | 打包 1 个槽 |
|---|---|---|---|---|---|
| 默认（3 节点） | 1 Grade，JSON 5.3 KB | 中位 2.67 ms，7310 次分配 | 中位 0.53 ms，2592 次分配 | 13 槽 22–25 µs | 0.6 µs |
| 多蒙版 | 4 Grade × 32 蒙版（16 径向 + 16 线性），JSON 60 KB | 中位 21.0 ms，55812 次分配 | 中位 2.33 ms，10318 次分配 | 52 槽 72–76 µs | 0.5 µs |

- 分配数来自 debug CRT 的 `_CrtSetAllocHook`，覆盖 EditGraph / EditRuntime DLL 内的分配。
- **笔刷蒙版未测：** `ALCEDO_ENABLE_BRUSH_MASK` 在所有 preset 中均为 OFF（"not part of the shipped product"），
  发布构建里没有笔刷文档。测试在该开关打开时会加入 8 个 × 200 笔 × 32 采样的笔刷蒙版；本次未打开。
  "重笔刷文档"在本阶段以"多蒙版文档"代替。
- **只有 debug 数据：** `win_release` 的 `ALCEDO_BUILD_TESTS=OFF`。P2 的门槛应在同一构建类型下与本表比较。
- 对 P1 / P2 的含义：`MakeStaticPlanKey` 每帧 0.5–2.3 ms、上千次分配（debug），与克隆同一数量级的十分之一，
  P1 改 revision 协议时应一并让静态 key 不必每帧重算；参数打包本身可以忽略。
- 建议的 P2 门槛（同为 debug）：多蒙版文档冻结 + 改一个滑块字段 ≤ 克隆耗时的 1%（约 0.2 ms），分配 ≤ 100 次。

**基线（完整 ctest 未运行）：** 按 AGENTS.md，智能体不自行运行完整 ctest。改为运行本重构涉及的定向套件，在 HEAD `fb8b3d65c` 上：

| 套件 | HEAD 基线 | P0 之后 |
|---|---|---|
| `PipelineMapperTest` `PipelineSharedUseTest` `ExportServiceTest` `GpuDagRawInputTest` `ImportServiceTest` | 207/207 通过，5 个预存 DISABLED | 213/213 通过（+`ExecutorIsolationTest` 3 个、`PipelineDocumentCopyCostTest` 2 个、`PipelineMapperTest` 1 个），7 个 DISABLED（新增 2 个为本阶段的预期失败测试） |
| 直接调用方：`AdjustmentTransferServiceMiniGitTest` `PipelineDngProfileBindingTest` `EditorSessionHistoryPortTest` `AdjustmentTransferControllerTest` `ThumbnailServiceTest` | — | 前四个 ctest 117/117 通过。`ThumbnailServiceTest` 过滤运行（排除 5 万次迭代的 `FuzzScroll*` 压力测试）：`OrdinaryThumbnailReusesLiveEditorExecutorAndDocument`（pin 计数，3 次中失败 1–2 次）与 `DiskCacheTracksRootAndActiveHeadAndServesAfterPipelineIsRemoved`（盘缓存 metadata 文件不存在，3/3 失败）在去掉 P0 生产改动的 HEAD 代码上同样失败，属预存问题；其余非压力测试通过 |

预存 DISABLED（非本阶段）：`PipelineMapperTests.FuzzTest`、`ThreadSafeTest`，`ExportServiceTests.ExportHdrJpeg_WritesUltraHdrFile`、
`BatchExport_LimitedCount_WritesReadableFiles`、`Manual_KeepExportFiles`。

Commands（PowerShell，PATH 前置 `build\debug\vcpkg_installed\x64-windows\debug\bin`）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineMapperTest PipelineSharedUseTest ExportServiceTest GpuDagRawInputTest ImportServiceTest ExecutorIsolationTest PipelineDocumentCopyCostTest
ctest --test-dir build/debug -j 1 -R "^(PipelineMapperTest|PipelineSharedUseTest|ExportServiceTest|GpuDagRawInputTest|ImportServiceTest|ExecutorIsolationTest|PipelineDocumentCopyCostTest)\." --output-on-failure
ExecutorIsolationTest.exe --gtest_also_run_disabled_tests      # 修复前代码上运行，得到上表的失败数据
PipelineDocumentCopyCostTest.exe                               # 两次
```

**Checklist / exit condition：**
- [x] 基线已记录（定向套件；完整 ctest 按项目规则未运行）
- [x] 新增测试 1、2 已提交为 `DISABLED_`，注释写明由 P1 / P5 启用；测试 3 随小修复启用
- [x] 测量数据已写回（笔刷文档与 release 数据缺失，原因见上）
- [x] 小修复：已有 root 时 `InitializeImageRoot` 不再改 live 文档的相机 profile

**LOC note：** 生产代码只改 `app/pipeline_service.cpp`（+24/−17，1183 行，原本就超过 1000 行，P7 计划将其降到一半以下）和
`pipeline_service.hpp` 注释。`pipeline_shared_use_test.cpp` 删除与新 `tests/support/raw_import_pipeline_fixture.hpp` 重复的导入 / 像素辅助函数（净 −58 行）。
新测试文件 371 + 277 行。

**Remaining gaps：**
- 测试 1 的失败说明 R7 现在就会让编辑器预览在同图缩略图渲染后停留在旧参数上（用户可见），修复属于 P1。
- 测试 3 在修复前是时序相关的检测（窗口大，本机 19338 次命中）；修复后的保证由确定性单测
  `InitializeImageRootOnExistingRawRootLeavesLiveCameraProfileUnchanged` 承担。该单测在修复前代码上的失败未单独运行，
  依据是旧代码会把 `color_matrix_1[0]` 从 0.625 改为 3.2404542。
- 笔刷文档和 release 构建的成本数据未测量。

### P1 运行时只读文档：dirty 位协议改为 revision 协议

**目标：** 渲染路径不再写文档。`Execute` 及所有 pass 改为接受 `const PipelineDocument&`。
这是之后一切的硬前置条件（审计 R7）。

- `IOperatorModel` / `OperatorModelBase`：
  - setter 在改动时从进程级单调计数器取号，写入模型的 `Revision()` 和逐字段的 `FieldRevisions()`（字段数小，定长数组）。
  - 删除 `TakeDirtyPatch` / `TakeDirtyFields` / `RestoreDirty` / `MarkAllDirty`。
  - `IsDirty` / `DirtyFields` 改为相对调用方提供的 revision 计算。
- `PipelineDocument`：
  - `topology_dirty_` 改为 `TopologyRevision()`；
  - `ColorGradeNodeModel` 的 mix dirty 改为 `MixRevision()`；
  - 蒙版已有 revision，沿用。
- 执行侧状态（归 executor 的 workspace，不归文档）：
  - `ParameterArena` 每个槽记录 `applied_revision`。`BindOrRefreshGradeRuntimeSlot` 等改为
    "槽缺失或 `model.Revision() != applied_revision` 时打包"。上传失败时不前进 `applied_revision`，替代 `RestoreDirty`。
  - `RuntimeInvalidationState::CollectAndPropagate` 改为比较逐字段 revision 与自己上次看到的值，
    不再调用 `ClearTopologyDirty` / `ClearMixDirty`。
- 波及范围（约 25 处）：
  - `edit/runtime/runtime_invalidation.cpp`、`grade_parameter_slot.hpp`；
  - CUDA / OpenCL / Metal 的 develop、camera_color、drt pass；
  - `pipeline_graph_commands.cpp`、`pipeline_document.cpp`、`app/pipeline_document_history.cpp`；
  - 各 pass encoder 的签名。
- 克隆：`ClonePipelineDocument` 保留 revision（JSON 往返后需重新盖号，否则克隆与源的 revision 可比性丢失）。
  新加载的文档取全新 revision，executor 必然全量打包。

**退出条件：**
- 全仓 grep 不到渲染路径对文档的非 const 引用。
- P0 测试 1 通过。
- 编辑器增量渲染的命中统计（`RenderSessionStats`、`PassStats`）与基线一致，即没有因改协议退化成全量重算。

##### Phase P1 completion record (2026-09-28)

**Status:** complete — 渲染路径只读文档；dirty 位协议已整体删除，改为进程级 revision 戳 + 各渲染 workspace 自己记录"上次应用的 revision"。
P0 测试 1（审计 R7）已启用并通过；编辑器增量渲染的逐帧命中统计与基线逐项一致。

**实现要点（与计划条目的对应）：**

| 计划条目 | 实现 |
|---|---|
| 进程级单调计数器 | `edit/operators/models/parameter_revision.{hpp,cpp}`：`ParameterRevision`、`kNoParameterRevision`、`NextParameterRevision()`（EditGraph 内唯一定义，跨 DLL 共用一个计数器）。比较一律用 `!=`：无关 Model 可能带更小的戳 |
| `IOperatorModel` revision | `Revision()`、`FieldsRevision(DirtyFieldMask)`（所选字段组内最新的戳）、`CopyRevisionsFrom()`（仅文档克隆使用）。`OperatorModelBase` 用按字段枚举位宽确定的定长数组保存逐字段戳；新 Model 构造时所有字段盖一次新戳；等值写入不盖戳 |
| 删除 dirty 接口 | 删除 `IsDirty` / `DirtyFields` / `TakeDirtyPatch` / `TakeDirtyFields` / `RestoreDirty` / `MarkAllDirty`、`pending_parameter_patch.hpp`（`PendingParameterPatch`、`TakePending*`）以及只为它服务的 `OperatorParamPatchDto`。`DirtyFieldMask` 保留，语义改为"字段选择掩码" |
| `topology_dirty_` → `TopologyRevision()` | `PipelineDocument::TopologyRevision()` / `MarkTopologyChanged()`。生产代码里原本只有 `CollectAndPropagate` 读它（且只是清掉），现在渲染侧没有读者 |
| mix dirty → `MixRevision()` | `ColorGradeNodeModel::MixRevision()`；`SetEnabled` / `SetMix` 值变化时盖戳 |
| `ParameterArena` 每槽 `applied_revision` | `WritePackedSlot(..., applied_revision)`、`AppliedRevision(key)`；`BindSlot` / `Clear` 清除记录。`BindOrRefreshGradeRuntimeSlot` 改为"槽缺失或 `AppliedRevision != model.Revision()` 时打包"，返回 `bool`。Metal DRT/Post 邻域槽同此 |
| DRT 显示参数 | 三个后端的 `BindDisplayParams`：槽缺失、DRT revision 变化、或本帧有导出色彩覆盖时打包；覆盖写入记为 `kNoParameterRevision`，所以下一个普通帧必然重新打包 |
| `CollectAndPropagate` | 签名改为 `const PipelineDocument&`。自存 last-seen：Develop 传感器字段组 / 白平衡字段组各一个戳、DRT 参数戳、按 `(owner node, adjustment instance)` 的调整戳、按 grade 的 mix 戳。调整与 mix 的表每帧按当前实例重建，删除后重新加入的 ID 视为变化。`AdvanceDocumentEpoch` / `Clear` 一并清空 |
| 各 pass 签名 | CUDA / OpenCL / Metal 的 develop、camera_color、primary grade、DRT、pass encoder、`PlanExecutor`、`BasicRenderDevice::Execute`、`GradeExecutor`、`DrtPostExecutor` 全部改为 `const PipelineDocument&` / const Model。develop 与 camera_color 中只"取走再提交" dirty 位的代码删除 |
| 克隆保留 revision | `ClonePipelineDocument` 在 JSON 往返并重绑 DNG profile 之后，按节点 ID / 调整实例 ID 复制 Develop、DRT、各 grade（含 mix）的戳和拓扑戳 |
| 渲染器持有只读文档 | `Renderer<Backend>::document_` 与 `PipelineExecutor::pipeline_document_` 改为 `shared_ptr<const PipelineDocument>`；`GpuDagDocument()` 返回 const（调用方只做指针比较） |

**上传失败语义（与计划原文不同）：** 计划写的是"上传失败时不前进 `applied_revision`"。实现在打包进 host 镜像时就记录 revision：
`ParameterArena::UploadDirty` 失败时把合并后的区间重新排队、host 字节保留，下一次上传必然送达同一份字节。
为失败再设一个"待提交 revision"不会改变任何可观察结果（AGENTS.md：没有可执行交错就不加一致性机制）。
`GradeRuntimeSlotFailedUploadKeepsPackedBytesQueued` 与三个后端的 `ParameterUploadFailureKeepsPackedBytesQueuedUntilRetry` 固定这一行为。

**主调用链（成功路径）：**

```text
编辑器滑块 → ApplyEditorParameterPatch
  -> ExposureModel::SetValue → OperatorModelBase::MutateWithDirtyFields → 变化字段盖 NextParameterRevision()
渲染（任意 workspace：编辑器 session device 或 one-shot device）
  -> Renderer<Backend>::Render（const 文档）→ BasicRenderDevice::Execute → PlanExecutor::Execute
  -> BasicRenderWorkspace::PrepareResultValidity → RuntimeInvalidationState::CollectAndPropagate(const doc)
       -> 与本 workspace 的 last-seen revision 比较 → 变化源 GraphValueId 失效并向下游传播 → 更新 last-seen
  -> GradeExecutor::Execute → BindAndScheduleGrade(const grade)
       -> BindOrRefreshGradeRuntimeSlot：AppliedRevision != Revision → 打包写入 arena 并记录 revision
  -> DrtPostExecutor::Execute → Ops::BindDisplayParams(const drt)（同上）
  -> ParameterArena::UploadDirty → GPU 执行 → 发布结果
  -> 文档未被写；另一个 workspace 之后渲染同一文档时，按自己的 last-seen 再次发现同一变化
```

**失败路径：**

```text
参数上传失败 → UploadDirty 重新排队区间并抛出 → CancelRender，结果不发布（completed 不前进）
  -> 下一帧：required 仍领先 completed，受影响结果重算；arena 记录的 revision 与 Model 相同，不重复打包，
     重新排队的字节随下一次 UploadDirty 送达
导出色彩覆盖帧 → DRT 显示槽记为 kNoParameterRevision → 下一个普通帧必然重新打包
历史恢复（*document = 编辑前的克隆）→ 克隆带编辑前的戳，与 last-seen 不等 → 受影响调整失效
  （RestoredOlderDocumentInvalidatesChangedAdjustment）
```

**退出条件：**

- [x] 渲染路径没有文档的非 const 引用：在 `edit/runtime`、`include/edit/runtime`、`renderer`、`edit/pipeline` 中 grep `PipelineDocument&`、`shared_ptr<PipelineDocument>` 和非 const Model 引用，结果为零（`Renderer` 与 `PipelineExecutor` 持有 `shared_ptr<const PipelineDocument>`）。
- [x] P0 测试 1 通过：`ExecutorIsolationTest.EditorSessionRenderShowsParameterChangeAfterInterleavedOneShot` 已去掉 `DISABLED_` 并通过。同一测试在基线源码上仍失败（均值 0.635 < 0.762×1.2，最大差 0.273，与 P0 记录一致）。
- [x] 编辑器增量命中统计与基线一致：新增 `ExecutorIsolationTest.EditorSessionEditSequenceReexecutesOnlyPassesDownstreamOfTheEdit`，经真实 `PipelineExecutor` 跑 7 帧（首帧、无改动、曝光、阴影 LLF、grade mix、白平衡、无改动），逐帧打印 `GpuNodePassStats` 与 plan cache 计数。把 `alcedo_studio/src` 换回基线（`5e11cbc84`）并重编同一测试后，**7 帧的全部计数逐项相同**：

| 帧 | sensor 执行/跳过 | camera 执行/跳过 | grade 执行 | drt 执行/跳过 | content hits | revision misses | plan hit/miss |
|---|---|---|---|---|---|---|---|
| 0 首帧 | 1/0 | 1/0 | 1 | 1/0 | 0 | 0 | 0/1 |
| 1 无改动 | 0/1 | 0/1 | 1 | 0/1 | 4 | 0 | 1/0 |
| 2 曝光 | 0/1 | 0/1 | 1 | 1/0 | 3 | 1 | 1/0 |
| 3 阴影（LLF） | 0/1 | 0/1 | 1 | 1/0 | 3 | 1 | 1/0 |
| 4 grade mix | 0/1 | 0/1 | 1 | 1/0 | 7 | 1 | 1/0 |
| 5 白平衡 | 0/1 | 1/0 | 1 | 1/0 | 2 | 3 | 1/0 |
| 6 无改动 | 0/1 | 0/1 | 1 | 0/1 | 8 | 0 | 1/0 |

grade 每帧都执行是既有设计（Grade `scene_output` 从不发布，见 `GradeExecutor::Execute` 注释），两种协议相同。
该对比只在 CUDA 上测量；OpenCL 由 `GpuDagOpenClGradeTest` / `GpuDagOpenClWorkspaceTest` 既有的增量断言覆盖；Metal 未编译（见下）。

**What was proven (executed tests)：**

| 名称 / 条目 | 目标 | 结果 |
|---|---|---|
| `EditorSessionRenderShowsParameterChangeAfterInterleavedOneShot`（P0 测试 1，已启用） | `ExecutorIsolationTest` | PASS（基线 FAIL） |
| `EditorSessionEditSequenceReexecutesOnlyPassesDownstreamOfTheEdit` | `ExecutorIsolationTest` | PASS，计数与基线相同 |
| `ParameterChangeInvalidatesEveryStateThatReadsTheDocument`（R7 单元级） | `GpuDagRawInputTest` | PASS |
| `CollectLeavesEveryDocumentRevisionUnchanged` | `GpuDagRawInputTest` | PASS |
| `ClonedDocumentWithEqualValuesKeepsPublishedResultsValid`、`RestoredOlderDocumentInvalidatesChangedAdjustment` | `GpuDagRawInputTest` | PASS |
| `GradeRuntimeSlotPackingInOneArenaLeavesOtherArenaStale`、`GradeRuntimeSlotWritesPackedBytesOnlyWhenModelRevisionChanges`、`GradeRuntimeSlotFailedUploadKeepsPackedBytesQueued` | `GpuDagRawInputTest` | PASS |
| `parameter_revision_test.cpp`（替代 `dirty_patch_test.cpp`，12 个：并发唯一、等值写不变、字段组、克隆保留、克隆后写只影响克隆、拒绝异类型拷贝等） | `GpuDagModelGraphTest` | PASS |
| 改写的 dirty 断言（命令服务、默认管线、拓扑命令、编辑器上下文 / 面板投影、复制目录、CUDA / OpenCL workspace 与 grade） | 各目标 | PASS |

Commands（PowerShell，PATH 前置 `build\debug\vcpkg_installed\x64-windows\debug\bin`）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8 --target <下列测试目标> alcedo_main
ctest --test-dir build/debug -j 1 -R "^(ExecutorIsolationTest|GpuDagModelGraphTest|GpuDagRawInputTest|GraphImageCacheRetentionTest|GpuDagCudaWorkspaceTest|GpuDagCudaDevelopTest|GpuDagCudaMaskTest|GpuDagCudaPrimaryGradeTest|GpuDagOpenClGradeTest|GpuDagOpenClWorkspaceTest|PipelineDocumentCopyCostTest|AdjustmentTransferCatalogTest|EditorAdjustmentContextTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|PipelineMapperTest|PipelineSharedUseTest|ExportServiceTest|ImportServiceTest|AdjustmentTransferServiceMiniGitTest|PipelineDngProfileBindingTest|EditorSessionHistoryPortTest|AdjustmentTransferControllerTest)\."
ctest --test-dir build/debug -j 1 -R "^(AdjustmentTransferServiceMiniGitTest|EditorSessionHistoryPortTest|ImportPipelineDocumentTest|PipelineDngProfileBindingTest|PipelineDocumentRenderTest|PipelineMapperTest|PipelineSchedulerRequestIdTest|PipelineSharedUseTest|ExecutorIsolationTest|ExportServiceTest|ImportServiceTest|AdjustmentTransferControllerTest|GpuDagOpenClWorkspaceTest)\."   # PipelineExecutor 改 const 之后
ThumbnailServiceTest.exe --gtest_filter=-*FuzzScroll*
基线对比：git stash push -- alcedo_studio/src → 重编 ExecutorIsolationTest / ThumbnailServiceTest → 运行 → 从 stash 恢复 src
```

Suite totals：

- 主定向集 709 个：707 通过；2 失败（下述测试环境残留，非 P1）；2 跳过（`InteractiveDagBaselinesDumpCurrentExecutionGpuTimes` 计时转储、`InstalledOpenClPackageBuildsEveryGpuDagProgram` 需要安装目录）；另有 7 个预存 `DISABLED_`（P0 列出的 5 个、P5 的 `ExportDuringUnsettledEditorPreviewUsesCommittedState` 等）。
- `PipelineExecutor` 改 const 之后的 13 个套件：222/222 通过（包括上面 2 个 OpenCL 测试）。
- `ThumbnailServiceTest`（排除 FuzzScroll）：21 通过、4 失败，全部预存：P0 已记录的 `OrdinaryThumbnailReusesLiveEditorExecutorAndDocument`、`DiskCacheTracksRootAndActiveHeadAndServesAfterPipelineIsRemoved`，以及 `MissingPipelineThrows`、`MissingImageThrows`（`GetThumbnail` 对缺失的 pipeline / 图片不抛异常；在基线源码上重编后同样失败，因此 P0 记录中"其余非压力测试通过"对这两个不成立）。
- 完整 ctest 按 AGENTS.md 未运行。

**两个 OpenCL 失败的原因：** `OpenClWorkspaceFixture.OpenClProgramManifestCanLoadFromInstalledResourceLayout` 在测试 exe 旁创建 `opencl/edit/runtime/opencl/shader/`，结束时只删文件不删目录。之后 `OpenClProgramLibrary` 看到该目录就按安装布局找 shader，于是 `OpenClPlanWarmUpBuildsOnlyRequiredProgramsAndKernels` 与 `OpenClSecondEmptyRenderCreatesNoBufferImageProgramOrKernel` 报 "failed to open source file ... geometry_camera.cl"。同一套件第一次运行时目录尚不存在，两者通过；删除空目录后重跑也通过。这是既有测试的清理缺陷，与 P1 无关，本阶段未修改。

**Checklist / exit condition：** 三项退出条件全部满足（见上）。

**LOC note：** 生产代码 54 个文件 +593/−530（大部分是三个后端 pass 的签名改 const），测试 36 个文件 +707/−401。改动文件均低于 1000 行（最大为 `color_grade_node_model.cpp` 526 行、`runtime_invalidation.cpp` 414 行）。`dirty_patch_test.cpp` 改名为 `parameter_revision_test.cpp` 并重写。只对改动行运行 `git clang-format`，并还原了它对未改动 include 块的重排。

**Remaining gaps：**

- **Metal 未编译：** 本机没有 macOS 环境。`metal_develop_pass.mm`、`metal_drt_pass.mm`、`metal_primary_grade_pass.mm`、`metal_pass_encoder.hpp` 和 Metal 测试的改动与 CUDA / OpenCL 同形，但只做了文本检查，需要在 macOS 上编译并运行 `GpuDagMetalWorkspaceTest` / `GpuDagMetalGradeTest`。
- **静态 plan key 仍每帧计算：** P0 建议 P1 顺带让 `MakeStaticPlanKey` 不必每帧重算，本阶段未做。`HashGraphTopology` 覆盖节点、调整列表、蒙版 ID、蒙版源类型以及 color / luminance range 是否存在，而蒙版编辑等路径并不都调用 `MarkTopologyChanged()`；只凭 `TopologyRevision()` 缓存 key，会在这些编辑之后复用错误的 plan。要做需先让这些入口都盖拓扑戳，属于独立改动，建议在 P2 冻结快照时一并确定拓扑身份。
- **蒙版 revision 仍是节点内计数器**（`next_mask_revision_`），按计划沿用。它不是进程级戳，不同文档的同一 `(grade, mask)` 可能得到相同数值，目前靠 `AdvanceDocumentEpoch` 清空 last-seen 兜住。P3 让 executor 按快照绑定时需改为进程级戳。
- 编辑器仍在 render lock 下写 live 文档（E1），渲染在另一线程读；P1 只保证渲染不写，读写互斥仍依赖 render lock，直到 P6。

### P2 不可变管线图快照与低成本冻结

**目标：** 引入 `PipelineGraphSnapshot`，并让冻结代价与改动量成正比，而不是与文档大小成正比。

- `PipelineGraph` 节点存储从 `unique_ptr<INodeModel>` 改为 `shared_ptr<INodeModel>`，并实现写时复制：
  - 非 const 访问（`FindNode`、`Develop()`、`PrimaryGrade()`、`Drt()`、各类 command）在节点被快照共享时先克隆该节点；
  - `PipelineDocument::Freeze() -> shared_ptr<const PipelineDocument>` 浅拷贝图结构，共享所有节点；
  - 大块数据（笔刷 stroke、DNG profile）在节点内部以 `shared_ptr<const>` 持有，节点克隆不复制它们。
- 编辑器 GUI 快照（`EditorSessionService::PublishDocumentSnapshot`，`editor_session_service.cpp:612-622`）
  改用 `Freeze()`，删除每次通知的整文档 JSON 克隆（审计 E5）。这是 P2 的第一个真实用户，
  用来在不动所有权的前提下验证 COW 的正确性。
- 定义 `PipelineGraphSnapshot`（§3.2）与 `PipelineLineageId`。

**退出条件：**
- 冻结耗时满足 P0 设定的门槛（建议：重笔刷文档单次冻结 + 改一个滑块字段 < 0.2 ms）。
- 快照在工作文档后续修改后内容不变（新增单测）。
- GUI 面板投影测试全绿。

##### Phase P2 completion record (2026-09-28)

**Status:** complete — 文档改为写时复制，`Freeze()` 只复制文档对象和节点 / 边两个 vector；编辑器 GUI 发布改用 `Freeze()`（审计 E5 删除）；
`PipelineGraphSnapshot` 与 `PipelineLineageId` 已定义并有单测。所有权结构未动（P3 起才有消费者使用快照类型）。

**实现要点（与计划条目的对应）：**

| 计划条目 | 实现 |
|---|---|
| 节点存储改为共享 + COW | `PipelineGraph::nodes_` 为 `vector<shared_ptr<const INodeModel>>`。节点对象创建时非 const，只经 `UnshareForWrite`（新 `edit/graph/copy_on_write.hpp`）取得可写引用：use count > 1 时先 `Clone()` 并替换同一下标；为 1 时 acquire fence 后原地写。`Nodes()` 只交出 `shared_ptr<const>`，因此从 `Nodes()` 拿可写指针在编译期被拒 |
| 非 const 访问先克隆 | 非 const `FindNode` 只复制它返回的那一个节点。`Develop()` / `PrimaryGrade()` / `Drt()` 先走 const 查找，再只对找到的节点取写权限，所以查找过程不会复制其他节点。图内部的存在性检查全部改为 const 查找 |
| 节点内部共享 | 计划只写"节点级 COW"。实测一个多蒙版 Grade（13 个调整、32 个蒙版）整节点深拷贝就超过 100 次分配，所以再下一层：`AdjustmentModelEntry::model` 改为 `shared_ptr<const IOperatorModel>`，写经 `MutableAdjustmentModel`；Color Grade 的蒙版列表与蒙版内容 revision 合为一个共享的 `MaskList`，写经 `MutableMaskList`。节点 `Clone()`（新 `INodeModel` 纯虚）是私有拷贝构造：共享 Model 与蒙版列表，复制 ID、标量字段和 Develop / DRT 端点参数 |
| Model 克隆 | 新 `IOperatorModel::Clone()`；`OperatorModelBase` 的受保护拷贝构造在源 Model 锁内复制 payload 与逐字段戳。克隆保留戳（值相等），渲染器看到的 revision 不变，不会重打包 |
| 大块数据 | DNG profile 本来就是 `shared_ptr`（`DngColorProfileRef`），节点克隆不复制。笔刷 stroke 在 `MaskList` 内，冻结与 Grade 克隆都不复制；只有蒙版写入才复制该 Grade 的蒙版列表（笔刷模块发布构建未启用） |
| `Freeze()` | `PipelineDocument::Freeze() const -> shared_ptr<const PipelineDocument>`，经私有拷贝构造共享全部节点。`PipelineDocument` / `PipelineGraph` 不可公开拷贝（只能 move），共享只能通过 `Freeze()` 产生；独立可编辑副本仍用 `ClonePipelineDocument` |
| GUI 快照 | `EditorSessionService::PublishDocumentSnapshot` 改为 `document->Freeze()`。Debug 构建记录每个已发布文档的 `DocumentRevisionFingerprint`，下次替换前断言未变（计划 §5 风险表的对策） |
| 快照类型 | `edit/graph/pipeline_graph_snapshot.{hpp,cpp}`：`PipelineLineageId`（进程内唯一，`Next()` 线程安全）与 `PipelineGraphSnapshot`（只能经 `Committed(...)` / `Preview(...)` 构造，拒绝空文档与空谱系；preview 无 head；committed 的 head 可空，对应只有 root 的历史）。`EditGraph` 因 `Hash128` 增加 `xxHash` 公开依赖 |
| 非编辑器的非 const 读取 | COW 之后非 const 访问器是写操作。审计出三处只读却用了非 const 访问的位置（导出入队读 DRT、Paste 读 DRT、`LoadPipeline` 读 Develop），改为 `std::as_const` |

**线程规则（写在 `UnshareForWrite` / `Freeze` 的前置条件里）：** 同一份共享部件只能有一个可写持有者，并且它只在一个线程（或同一把锁）上写；冻结文档从不写。
编辑器的 live 文档满足这一点：写入与 `PublishDocumentSnapshot` 都在会话 owner 线程上。渲染线程只在 render lock 下读 live 文档，
COW 替换 `nodes_` 元素也发生在持 render lock 的写路径里。未改动：编辑器写 live 文档仍需 render lock（E1），直到 P6。

**主调用链（成功路径）：**

```text
滑块 → EditorSessionService（owner 线程）→ history port → LockLivePipeline（render lock）
  -> live.PrimaryGrade()                       // 只复制被上次 Freeze 共享的这一个 Grade 节点
  -> ->FindAdjustmentByType(Exposure)          // 只复制这一个 Model，保留戳
  -> ExposureModel::SetValue                   // 新戳
  -> Emit → PublishDocumentSnapshot → live.Freeze()   // 新文档对象 + 节点 / 边 vector，节点全部共享
  -> published_document_ 替换；旧冻结文档随最后一个 GUI 持有者释放，其独占的旧节点一并释放
  -> GUI 读 pipeline_document()：不可变，无需锁
```

**失败路径：**

```text
代码持有可写节点 / Model 指针跨过一次 Freeze 再写 → 写穿到冻结文档
  -> Debug：下一次 PublishDocumentSnapshot 断言 DocumentRevisionFingerprint 不变
  -> Release：无检测；由单测覆盖全部写入口（见下表）
Clone / 蒙版列表复制抛异常（内存不足）→ UnshareForWrite 在替换前抛出，工作文档与冻结文档均不变
PipelineGraphSnapshot 以空文档或空谱系构造 → std::invalid_argument
```

**What was proven (executed tests)：**

| 名称 / 条目 | 目标 | 结果 |
|---|---|---|
| 冻结 + 一次滑块写 ≤ 0.2 ms、≤ 100 次分配：`DefaultDocumentFreezeAndOneSliderEditStayWithinLimit`、`ManyMaskDocumentFreezeAndOneSliderEditStayWithinLimit` | `PipelineDocumentCopyCostTest` | PASS。两次运行：默认文档中位 12.6–13.2 µs / 57 次分配；多蒙版文档中位 15.2–16.1 µs / 69 次分配（同构建下旧的整文档克隆为 20.6–20.8 ms / 55905 次） |
| 快照在后续修改后不变：`FrozenDocumentKeepsParameterValuesAfterEveryWorkingDocumentWrite`（Develop、Grade 调整、DRT 参数与 DRT/Post 调整、mix、enabled、名称、删除保护、几何、命名计数）、`FrozenDocumentKeepsMasksAfterEveryWorkingDocumentMaskWrite`（全部蒙版 setter 及经 `FindMask` / `MaskAt` 的直接写）、`FrozenDocumentKeepsTopologyAfterWorkingDocumentGraphCommands` | `GpuDagModelGraphTest` | PASS |
| 只复制被写部分：`ParameterEditCopiesOnlyTheEditedNodeAndModel`、`NonConstLookupCopiesOnlyTheReturnedNode`、`WorkingDocumentWritesInPlaceWhenNoFrozenDocumentSharesIt` | `GpuDagModelGraphTest` | PASS |
| 克隆保留戳：`CopiedNodesKeepTheChangeStampsOfTheirSource`、`RevisionFingerprintChangesWithEveryStampedWrite` | `GpuDagModelGraphTest` | PASS |
| 跨线程：`FrozenDocumentsReadOnAnotherThreadKeepTheirValuesWhileOwnerEdits`（owner 300 轮写 + 冻结，读线程逐个比对 JSON） | `GpuDagModelGraphTest` | PASS |
| 快照类型：`PipelineGraphSnapshot` 5 个、`PipelineLineageId.DefaultIdIsEmptyAndNextIdsAreUniqueAcrossThreads` | `GpuDagModelGraphTest` | PASS |
| E5 在服务层：`PublishedDocumentSharesNodesAndKeepsValuesAfterLiveEdit`（发布的文档与 live 共享节点；live 写后旧发布不变、未写节点仍为同一对象） | `EditorSessionNodeCommandTest` | PASS |
| GUI 面板投影与编辑器上下文 | `EditorPanelProjectionTest`、`EditorAdjustmentContextTest`、`EditorPipelineCommandServiceTest` | PASS |

Commands（PowerShell，PATH 前置 `build\debug\vcpkg_installed\x64-windows\debug\bin`）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8          # 全量构建，0 错误
ctest --test-dir build/debug -j 1 -R "^(GpuDagModelGraphTest|PipelineDocumentCopyCostTest|EditorSessionNodeCommandTest)\."
PipelineDocumentCopyCostTest.exe                                              # 两次，读取 [P2 cost] 行
ctest --test-dir build/debug -j 1 -R "^(ExecutorIsolationTest|GpuDagRawInputTest|GraphImageCacheRetentionTest|GpuDagCuda(Workspace|Develop|Mask|PrimaryGrade)Test|GpuDagOpenCl(Grade|Workspace)Test|AdjustmentTransfer.*|EditorAdjustmentContextTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|EditorNodeGraph.*|EditorMask.*|EditorSession.*|EditorHistory.*|EditorParameterWrite.*|EditorAdjustmentPipelineTest|PipelineMapperTest|PipelineSharedUseTest|PipelineGraph.*|PipelineDocument.*|PipelineHistory.*|PipelineEditBatchTest|PipelineDngProfileBindingTest|PipelineDocumentRenderTest|PipelineSchedulerRequestIdTest|ImportPipelineDocumentTest|ExportServiceTest|ImportServiceTest|MiniGit.*|DocumentTransfer.*)\."
ThumbnailServiceTest.exe --gtest_filter=-*FuzzScroll*
基线对比：git stash push -u -- alcedo_studio → 只重编失败的目标 → 运行 → git stash pop
```

Suite totals：

- 新测试所在的三个目标：111/111 通过（新增 18 个）。
- 定向回归集 1041 个（另 7 个预存 `DISABLED_`）：1032 通过，9 失败，全部预存：
  - `EditorSessionRenderSchedulerPortTest` 5 个、`EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`、`EditorSessionActionPolicyCq3Test.AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`：把 `alcedo_studio` 还原到 HEAD（`42de0f07e`）重编这三个目标后，同样 7 个失败。这些测试使用不带文档的 fake port，与 COW 无关。
  - `GpuDagOpenClWorkspaceTest` 2 个：P1 记录的测试清理缺陷（残留的 `opencl/edit/runtime/opencl/shader/` 目录）。
- `ThumbnailServiceTest`（排除 FuzzScroll）：20 通过，5 失败，全部预存。P1 已记录其中 4 个；第 5 个 `AnalysisRenditionRendersWithoutSavePipelineOnLiveGuard` 在 HEAD 源码上重编后 6/6 失败（无锁读 `pin_count_`，审计 S5），P2 源码上 3 次中 1 次通过。
- 读 `pipeline_document()` 的 UI 层测试（`EditorNodeDelegateQmlTest`、`EditorNodeGraphDraftTest`、`EditorNodeGraphProjectionTest`、`EditorNodeSelectionLayoutTest`、`EditorNodesPanelQmlTest`、`EditorPreviewPresentTrajectoryTest`）：4 个失败，全部预存（HEAD 源码上重编后同样 4 个失败）：`EditorNodeDelegateQml` 3 个 QML 输入测试与 `EditorNodeController.GeometryWriteRejectedWhenColorGradeIsSelected`。
- 最终构建上复跑 P2 相关 7 个目标：157/157 通过。
- 完整 ctest 按 AGENTS.md 未运行。

**Checklist / exit condition：**
- [x] 冻结耗时满足 P0 门槛：多蒙版文档冻结 + 改一个滑块中位 ~16 µs（< 200 µs）、69 次分配（≤ 100），由测试断言固定
- [x] 快照在工作文档后续修改后内容不变：参数、蒙版、拓扑三类写入口全覆盖，另有跨线程读测试
- [x] GUI 面板投影测试全绿（`EditorPanelProjectionTest` 及编辑器上下文 / 命令服务测试）

**LOC note：** 已有生产文件 21 个 +366 / −89（含 CMake），新增 `copy_on_write.hpp`（46 行）、`pipeline_graph_snapshot.{hpp,cpp}`（115 + 54 行）。改动文件均低于 1000 行（最大 `pipeline_graph.cpp` 621 行、`color_grade_node_model.cpp` 562 行）。已有测试文件 9 个 +159 / −7，新测试文件 323 + 113 行。

**Remaining gaps：**
- 蒙版写入会复制该 Grade 的整个蒙版列表（被冻结文档共享时）。拖动蒙版手柄的每个 tick 会复制一次该 Grade 的全部蒙版（多蒙版文档 32 个）。由 P2A 修复。
- 笔刷模块未启用，未测笔刷文档的冻结成本（与 P0 相同的限制）。
- Release 构建没有"写穿冻结文档"的检测，靠 Debug 断言与单测。编辑器仍经 render lock 写 live 文档（E1），P6 处理。
- `PipelineGraphSnapshot` 目前没有生产调用方；P3 / P4 开始使用。

### P2A 蒙版逐项写时复制

**目标：** 蒙版写入只复制被写的那一个蒙版。拖动蒙版手柄每个 tick 的冻结代价与该 Grade 的蒙版数量无关。

**问题（P2 遗留）：** P2 把一个 Color Grade 的全部蒙版与内容 revision 放在一个共享的 `MaskList` 里。
冻结文档共享该列表时，任何一次蒙版写入（`SetMaskOpacity`、`ReplaceMaskSource`、`FindMask` / `MaskAt` 的非 const 访问等）
都会复制整个列表：每个蒙版的 `MaskId`、显示名、source variant，以及 `std::map<MaskId, uint64_t>` 的全部节点。
编辑器拖动径向 / 线性蒙版时每个 tick 都会发布一次冻结文档，所以每个 tick 复制一次该 Grade 的全部蒙版
（多蒙版文档 32 个，约 100 次以上分配），超出 P0 为单次编辑设定的门槛。

**方案：**
- `ColorGradeNodeModel` 的蒙版存储改为逐项共享：
  - 列表元素为 `{shared_ptr<const MaskModel> mask; ParameterRevision content_revision;}`，列表本身仍以 `shared_ptr<const>` 共享；
  - 写一个蒙版：列表被共享时只复制元素 vector（N 个指针 + N 个戳，一次分配，不复制字符串），再只克隆被写的那个 `MaskModel`；
  - 增删、移动蒙版只改元素 vector，不克隆任何 `MaskModel`。
- 删除 `std::map<MaskId, uint64_t>`：内容 revision 存在元素里，查找按 `MaskId` 线性扫描（与 `FindMask` 相同）。
- 内容 revision 改为进程级戳（`NextParameterRevision()`），不再用节点内计数 `next_mask_revision_`。这同时解决 P1 记录的
  "不同文档的同一 `(grade, mask)` 可能得到相同 revision"问题，P3 按快照绑定时需要它。笔刷命令的 `expected_revision` 比较不受影响（仍是相等比较）。
- `Masks()` 目前返回 `std::span<const MaskModel>`，要求连续存储。改为返回按下标访问的只读视图（`MaskCount()` + `MaskAt(i) const`
  或一个可迭代的 const 视图类型）。调用方：生产代码 10 处、测试 33 处，全部同步修改。
- 冻结文档的蒙版值与 revision 保持不变的保证沿用 P2 的测试，并扩展到逐项共享。

**测试：**
- `MaskEditCopiesOnlyTheEditedMask`：冻结后改一个蒙版的不透明度，其余蒙版与冻结文档是同一对象，被改蒙版不同。
- `MaskDragTickCostDoesNotGrowWithMaskCount`（`PipelineDocumentCopyCostTest`）：一个 tick = 改一个蒙版 source + 冻结 + 释放上一个冻结文档。
  8 个与 32 个蒙版的 Grade 上分配次数相同；多蒙版文档中位 ≤ 0.2 ms、分配 ≤ 100（Debug）。
- `MaskContentRevisionIsUniqueAcrossDocuments`：两个独立文档里同 ID 蒙版的内容 revision 不相等。
- P2 的 `FrozenDocumentKeepsMasksAfterEveryWorkingDocumentMaskWrite` 与蒙版相关 GPU 测试（`GpuDagCudaMaskTest` 等）保持通过。

**退出条件：**
- 蒙版拖动 tick 的分配次数与蒙版数量无关，并满足 P0 门槛（测试断言）。
- 冻结文档在全部蒙版写入口之后内容不变。
- 蒙版渲染与失效测试（CUDA / OpenCL）全绿。

### P3 `PipelineExecutor` 改为"按请求接收快照"

**目标：** executor 不再绑定文档，也不再有一个两种世界共用的 Renderer。
消费者仍然暂时通过 guard 拿 executor，所有权结构这一阶段不动。

- `PipelineExecutor::Apply(snapshot, input, request)`：
  - 删除 `SetPipelineDocument` / `GpuDagDocument` / `HasGpuDagDocument`，删除 `Renderer::document_` 和 `SetDocument`。
    文档指针不再存两份（S8）。
  - 删除 `SetBoundFile` / `bound_file_id_`（死代码）。
- executor 按角色构造，二选一：
  - `ExecutorRole::Interactive`：一个 session device、plan cache、源缓存、可选的 frame sink；
  - `ExecutorRole::Batch`：一个专用队列 device，每个任务结束后全量释放。
  - `Renderer` 里的 `device_` 与 `one_shot_device_` 双 device 结构，以及 `RenderCachePolicy::BypassSessionCache` 分支，
    拆到两种角色里。每个 executor 只有一种（R1）。
- 实现 §3.3 的绑定键与全量释放。`ClearAllIntermediateBuffers` 改为内部的 `ReleaseBinding()`。
- 过渡期：guard 仍持有一个 executor，但它每次渲染把 `guard->document_` 冻结成快照再交给 `Apply`。
  为了不在这一阶段改变并发行为，缩略图 / 导出仍暂时借用这个 executor，只是走 Batch 路径。
  所以本阶段的 guard executor 同时实例化两种角色的 Renderer，P4 / P5 迁走消费者后删除。

**退出条件：**
- `pipeline_executor.hpp` 中不再有文档成员。
- 渲染输出与基线逐像素一致：抽样图片、编辑器三种帧角色、缩略图四档、导出 SDR / HDR。

##### Phase P3 completion record (2026-09-28)

**Status:** complete — `PipelineExecutor` 与 `Renderer<Backend>` 不再持有文档，每次渲染接收一个不可变的 `PipelineGraphSnapshot`；
Renderer 按角色构造（Interactive / Batch 二选一），`RenderCachePolicy` 与同一 Renderer 内的双 device 结构删除（R1）；
§3.3 的绑定键与全量释放已实现。所有权结构未动：消费者仍经 guard 借用同一个 executor，它同时持有两种角色的 Renderer。

**实现要点（与计划条目的对应）：**

| 计划条目 | 实现 |
|---|---|
| `Apply(snapshot, input, request)` | `PipelineExecutor::Apply(const PipelineGraphSnapshot&, input, request)`；`Renderer::Render(const PipelineGraphSnapshot&, ...)`。删除 `SetPipelineDocument` / `GpuDagDocument` / `HasGpuDagDocument`、`Renderer::document_` / `SetDocument`，以及 executor 的 `pipeline_document_` 成员。文档指针只剩 guard 上的一份（S8 的"存两份"消失，回滚 lambda 留到 P6） |
| 删除 `SetBoundFile` / `bound_file_id_` | 已删（连同只有它们使用的 `GetBoundFile`） |
| 按角色构造 | 新 `edit/runtime/executor_role.hpp`：`ExecutorRole { Interactive, Batch }` 与 `RenderBindingKey`。`Renderer(ExecutorRole, unpack)` 只有一个 device：Interactive 带 prepared source 缓存、plan cache、结果发布与可选 sink；Batch 用专用队列，每次解包 + 编译，结束后释放全部结果资源、保留 device（原 one-shot 语义）。`PipelineExecutor(ExecutorRole)` 只服务一种角色，另一种角色的请求抛 `std::invalid_argument`；默认构造的 executor 同时服务两种，只给 guard 用（过渡，P4 / P5 删除） |
| `RenderCachePolicy` 拆到角色里 | 枚举删除；`PipelineApplyRequest::cache_policy` 改为 `role`。scheduler 的 `MakeApplyRequest`：THUMBNAIL / FULL_RES_EXPORT → Batch，其余 → Interactive。Renderer 检查 `request.role == Role()` |
| 绑定键与全量释放 | Interactive 渲染时 `RenderBindingKey{lineage, element}` 与上次不同 → 先 `ReleaseBinding()`（WaitIdle、释放结果 / transient / LLF / 参数 arena、清 plan cache 与 prepared source、释放 neural demosaic 工作区、清 last-seen revision），device 与队列保留。同一键下不同的冻结文档对象不做任何释放，参数 / 拓扑变化照旧由 revision 与 `StaticPlanKey` 处理 |
| `ClearAllIntermediateBuffers` → `ReleaseBinding()` | `PipelineExecutor::ReleaseBinding()` 释放两个 Renderer 的绑定。计划写"内部的"，但 S2 / S3 / S9（驱逐、最后一 pin 清理、后端切换）在 P7 前仍需从外部释放，所以保持公开 |
| 过渡：guard 每次渲染冻结快照 | `PipelineGuard::lineage_`（`LoadPipeline` 构建文档时与每次 `BindLivePipelineDocument` 时取新值）与 `PipelineGuard::FreezeLiveSnapshot()`。`PipelineTask::snapshot_under_render_lock_` 为必填：scheduler 在 render lock 内、`configure_under_render_lock_` 之后调用它，再把结果交给 `Apply`；缺失或返回空则任务失败，不渲染其他文档。四个生产者（编辑器 port、缩略图、分析、导出）用 `MakeLiveSnapshotSource(guard)` 设置它 |

**与计划不同或计划未写明的决定：**

- **过渡快照的类型：** guard 冻结出的是 preview 快照（无 HEAD、空 chain）。live 文档可能含未提交的编辑器值，不能标成 committed；chain 留空是因为渲染线程不能读 `commit_graph_`（审计 §3 第 1 条，历史 owner 无锁替换它）。缩略图、分析、导出在 P4 / P5 之前仍渲染 live 文档，这是既有行为（C6），本阶段没有改变。
- **哪些操作换谱系：** `BindLivePipelineDocument` 一律取新谱系，包括打开编辑器时的重放、Version checkout、重建，以及编辑器内 Paste 的文档替换和失败回滚。原来 `SetDocument` 只推进 document epoch、保留源缓存；现在这些操作之后的第一帧会重新解包 RAW 并重编 plan。这是 §6 决策 2 的"先全量释放，P6 实测后再定"，本阶段没有测首帧耗时。历史恢复 `*document_ = clone`（同一对象）不换谱系，由 P1 的 revision 协议处理。
- **像素比较的容差：** 计划写"逐像素一致"。实测同一构建连续运行两次，Bayer / X-Trans 的部分用例最大相差 9.54e-6（CUDA 浮点，线性 DNG 完全一致），所以跨构建比较以"不超过同构建两次运行之间的差"为准。

**主调用链（成功路径）：**

```text
编辑器帧：EditorSessionRenderSchedulerPort::DispatchPipelineFrame
  -> PipelineTask{executor = guard->pipeline_, snapshot_under_render_lock_ = MakeLiveSnapshotSource(guard)}
  -> PipelineScheduler worker：lock render_lock → configure（AttachFrameSink）→ MakeApplyRequest（role = Interactive）
  -> guard->FreezeLiveSnapshot()：document_->Freeze() + lineage_ → PipelineGraphSnapshot::Preview
  -> PipelineExecutor::Apply(snapshot) → interactive Renderer::Render
       -> 键 (lineage, element) 与 Binding() 相同 → 复用 prepared source / plan / 已发布结果
          不同 → ReleaseBinding() 后作为新绑定的首帧
  -> Execute（只读快照文档）→ Present 到 sink
缩略图 / 分析 / 导出：同一 guard executor，MakeApplyRequest（role = Batch）→ batch Renderer::Render
  -> 解包 + CompileStatic → Execute → Download → 释放全部结果资源（device 保留）
  -> 不读、不写、不清 interactive Renderer 的任何缓存
Version checkout / 重建：BindLivePipelineDocument → 新 lineage_ → 下一编辑器帧全量释放后重建
```

**失败路径：**

```text
任务没有 snapshot source 或 source 返回空 → scheduler 抛 runtime_error → 阻塞结果为异常、on_complete(false)，不渲染
guard 没有文档或谱系为空 → FreezeLiveSnapshot / PipelineGraphSnapshot 抛 invalid_argument → 同上
请求角色与 executor 或 Renderer 不符 → invalid_argument，在创建 Renderer、读取输入之前
渲染 / 呈现失败 → 与之前相同：CancelRender 或 WaitIdle，丢弃未发布结果；Batch 另外释放全部结果资源；
  Interactive 的绑定保留（资源属于同一键，下一帧按 revision 重算）
```

**What was proven (executed tests)：**

| 名称 / 条目 | 目标 | 结果 |
|---|---|---|
| 渲染输出与基线一致：3 张图（线性 DNG、Bayer ARW、X-Trans RAF）× 9 个用例（编辑器 InteractivePrimary / QualityBase / DetailPatch、缩略图 256 / 512 / 1024 / 2048、导出 SDR Rec.709 / HDR Rec.2020 PQ），在共享 executor 上交错渲染 | `ExecutorSnapshotRenderTest.EveryRenderCaseMatchesARenderOnANewExecutorOfItsRole` | PASS：每个用例与新建的同角色 executor 的结果差 ≤ 1e-4 |
| 同上，跨构建：`ALCEDO_RENDER_OUTPUT_DIR` 转储 27 个用例，基线源码（`750d589ee`，同一测试改用旧 API）与 P3 源码比较 | 同上 + `compare_pixels.py` | 27/27 尺寸一致；线性 DNG 9 个用例逐字节一致；全部用例最大差 9.54e-6，与 P3 构建自身两次运行的最大差（9.54e-6）相同 |
| 同谱系连续快照复用源与 plan | `InteractiveRendersOfOneLineageReuseSourceAndPlan` | PASS |
| 换谱系全量释放后再渲染 | `InteractiveRenderOfAnotherLineageReleasesThePreviousBinding` | PASS |
| Batch 释放结果、保留 device、不动 interactive | `BatchRenderReleasesItsResultsAndKeepsItsDevice` | PASS |
| `ReleaseBinding` 清空两种 Renderer、保留 device | `ReleaseBindingReleasesEveryRendererAndKeepsDevices` | PASS |
| 单角色 executor 拒绝另一角色 | `ExecutorRoleTest.ExecutorOfOneRoleRejectsRequestsOfTheOtherRole` | PASS |
| 缺 snapshot source 的任务失败 | `ExecutorRoleTest.SchedulerFailsARenderTaskWithoutASnapshotSource` | PASS |
| P0 / P1 隔离测试（interleaved one-shot 现为同 executor 上的 batch 渲染） | `ExecutorIsolationTest` | PASS（P5 的 `DISABLED_` 保持） |
| 迁移后的 renderer 测试：batch 与 interactive 两个 Renderer 读同一快照、batch 不动 interactive 缓存、并行 batch 各用独立队列、换谱系释放 | `GpuDagCudaDrtProductTest`（含新 `RebindToNewLineageReleasesPreviousBindingBeforeRendering`）、`GpuDagOpenClDrtProductTest`、`PipelineDocumentRenderTest`、`PipelineSharedUseTest` | PASS |

Commands（PowerShell，PATH 前置 `build\debug\vcpkg_installed\x64-windows\debug\bin`）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8            # 全量构建，0 错误
ctest --test-dir build/debug -j 1 -R "^(ExecutorSnapshotRenderTest|ExecutorIsolationTest|GpuDagModelGraphTest|GpuDagRawInputTest|GraphImageCacheRetentionTest|GpuDagCuda(Workspace|Develop|Mask|PrimaryGrade|DrtProduct|DocumentGeometryRequest)Test|GpuDagOpenCl(Grade|Workspace|DrtProduct)Test|AdjustmentTransfer.*|EditorAdjustmentContextTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|EditorNodeGraph.*|EditorMask.*|EditorSession.*|EditorHistory.*|EditorVersion.*|EditorParameterWrite.*|EditorAdjustmentPipelineTest|PipelineMapperTest|PipelineSharedUseTest|PipelineServiceTest|PipelineGraph.*|PipelineDocument.*|PipelineHistory.*|PipelineEditBatchTest|PipelineDngProfileBindingTest|PipelineSchedulerRequestIdTest|PipelineFrameSinkTest|ImportPipelineDocumentTest|ExportServiceTest|ImportServiceTest|CiRawWorkflowTest|MiniGit.*|DocumentTransfer.*)\."
ThumbnailServiceTest.exe --gtest_filter=-*FuzzScroll*
$env:ALCEDO_RENDER_OUTPUT_DIR=...; ExecutorSnapshotRenderTest.exe      # P3 两次、基线一次
基线：git stash push -u -- alcedo_studio/src（及被迁移的测试文件）→ 只重编相关目标 → 运行 → git stash pop
```

Suite totals：

- 定向集 1296 个：1287 通过，9 失败，全部预存（另有 6 个预存 `DISABLED_`）：
  - `EditorSessionRenderSchedulerPortTest` 5 个、`EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`、`EditorSessionActionPolicyCq3Test.AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`：把 `alcedo_studio/src` 与 `editor_session_render_scheduler_port_test.cpp` 还原到基线后重编这三个目标，同样这 7 个失败（与 P2 记录一致）。
  - `GpuDagOpenClWorkspaceTest` 2 个：P1 记录的测试清理缺陷（残留的 `opencl/edit/runtime/opencl/shader/` 目录）。
- `ThumbnailServiceTest`（排除 FuzzScroll）：23 通过，2 失败，均为 P1 已记录的预存问题（`MissingPipelineThrows`、`MissingImageThrows`）。P2 记录中另外 3 个与 pin 计数 / 盘缓存相关的失败本次通过（它们是时序相关的，不作为 P3 的结论）。
- macOS（Apple Silicon，macOS 27.0，`build/macos-debug`，tests ON）：全量构建 0 错误。`GpuDagMetal*Test` 与 executor / pipeline 相关套件共 561 个，548 通过，13 失败，全部预存：`EditorSessionRenderSchedulerPortTest` 5 个（与 Windows 相同）；`GpuDagMetalDevelopTest` 2 个、`GpuDagMetalGradeTest` 5 个、`GpuDagMetalRendererTest.InteractiveQualityBaseInteractiveReuses2560PixelResults`——在同一台机器上切到 P2（`05277e9be`）重编这三个目标，同样这 8 个失败。`metal_full_pipeline_preview_test.cpp` 不在任何 CMake 目标中，只做了文本迁移。`ExecutorSnapshotRenderTest` 的 5 个 GPU 用例在 Mac 上跳过（该检出没有样例 RAW 的 Git LFS 内容），像素比较只在 CUDA 上完成；`ExecutorRoleTest` 2 个通过。
- 完整 ctest 按 AGENTS.md 未运行。

**Checklist / exit condition：**
- [x] `pipeline_executor.hpp` 中不再有文档成员（只有 `Apply` 的参数引用快照类型）
- [x] 渲染输出与基线逐像素一致：抽样 3 张图、编辑器三种帧角色、缩略图四档、导出 SDR / HDR（容差见上）

**LOC note：** 生产代码 19 个已有文件 +356 / −386，新增 `executor_role.hpp` 49 行。`renderer.hpp` 367 → 308 行，`pipeline_executor.cpp` 155 → 116 行。
`pipeline_service.cpp`（1194 行）与 `thumbnail_service.cpp`（1193 行）原本就超过 1000 行，本阶段各只增减十余行，计划在 P4 / P7 拆减。
测试 31 个已有文件 +981 / −661（四个子任务并行迁移，每处语义变化已逐条审阅），新增 `executor_snapshot_render_test.cpp` 572 行与 `support/render_snapshot_source.hpp` 84 行。
`pipeline_scheduler.cpp` 原为 CRLF，先在单独提交中转为 LF。

**Remaining gaps：**
- 编辑器在同一 guard 上 Paste、checkout、重建之后的首帧会重新解包 RAW（全量释放，§6 决策 2），首帧耗时未测，P6 实测。
- 蒙版内容 revision 仍是节点内计数器（P2A 未做）。同一谱系内它单调递增，不影响本阶段；跨谱系由换谱系时清空 last-seen 兜住。P2A 仍需在 P4 共享已提交快照前完成。
- 过渡形态：缩略图 / 导出仍借用编辑器所在的 guard executor 与 render lock，并渲染 live 文档（C1、C6 不变），P4 / P5 处理。
- `raw/opencl_cuda_full_pipeline_benchmark.cpp` 以前从不绑定文档（运行时必然失败），迁移后渲染无相机 profile 的默认文档，仍不代表真实 RAW 耗时；未运行。

### P4 缩略图 / 分析 executor 池

**目标：** 缩略图、AI 分析、CLIP 不再碰 guard 和编辑器的 executor。

- `PipelineMgmtService` 新增 `AcquireCommittedSnapshot(element_id) -> shared_ptr<const PipelineGraphSnapshot>`：
  - 编辑器持有租约的图，返回编辑器最近一次 `PublishCommitted` 的快照；
  - 其他图从存储构建：checkpoint 标签与 history tip 匹配就直接用，否则从 root 重放。
    不再读元素 JSON（审计 §3 第 7、9 条）；
  - 结果放入 CPU 快照缓存。
- 编辑器 commit 钩子：`MiniGitWorkingHistory` 发布 typed batch 成功后，冻结当前文档并调用 `PublishCommitted`。
  本阶段编辑器仍在 guard 上，冻结在现有的 `LockLivePipeline` 范围内进行。
- `ThumbnailService`：
  - 拥有 N 个 Batch executor（默认 2，见 §6 决策 1）和自己的 `PipelineScheduler`。
    不再使用 `render_service.hpp` 的共享静态池（R6）。
  - 每个任务 = 取快照 → 取空闲 executor → 渲染 → 归还。删除 `prepare_` 里的 `LoadPipeline`、`on_complete_` 里的
    `ReleasePipelineUse`、`HasGpuDagDocument` 检查（C1、B2）。
  - 盘缓存 key 直接取快照的 `(head, chain)`。删除 `ReadCurrentVersionHash`、`RenderedCommitLabel` 和
    `ThumbnailDiskCacheWriteAllowed` 的 dirty / unsettled 门控（C2、C3）。
  - 合并缩略图和分析两条渲染路径，`analysis_tokens_` 退化为按客户端的请求 id（C4）。
- `ImageAnalysisService` 与 `SemanticGenerationService` 中重复的 provider 适配器合并为一个（C5），
  放在 `app/` 现有文件里。
- `PipelineTask` / scheduler：Batch 任务不带 sink，删除 `MakeApplyRequest` 对 THUMBNAIL 的 sink 置空和失败特判（R2、R5）。

**退出条件：**
- `thumbnail_service.cpp` 不再引用 `PipelineGuard`、`LoadPipeline`、`ReleasePipelineUse`。
- 编辑器拖动期间同图缩略图不阻塞预览：新增计时测试，缩略图渲染期间编辑器帧延迟不受影响。
- 盘缓存 key 与渲染内容一致（沿用 `QueuedRenderDoesNotStorePixelsUnderStaleCommitLabel` 改写）。

##### Phase P4 completion record (2026-09-28)

**Status:** complete — 缩略图与分析只渲染已提交快照，运行在 `ThumbnailService` 自己的 Batch executor 池（2 个）和 scheduler 上，
不再加载 `PipelineGuard`、不再拿编辑器的 render lock、不再碰编辑器 executor。编辑器在每次已提交状态变化后发布快照。
滑块与 Mask 输入改用同一条"未提交输入"规则。

**实现要点（与计划条目的对应）：**

| 计划条目 | 实现 |
|---|---|
| `AcquireCommittedSnapshot` | `PipelineMgmtService::AcquireCommittedSnapshot` / `PublishCommitted`，委托给新的 `CommittedSnapshotCache`（`app/committed_snapshot_cache.{hpp,cpp}`）。编辑器持有的图（`editor_owned_`）返回编辑器最近一次发布的快照；其他图由 `LoadCommittedSnapshotFromStorage` 从已物化的历史构建：checkpoint 的 root / head / chain 标签与物化标签一致就用 checkpoint，否则 `LoadGraph` 后从 root 重放。不读元素 JSON |
| 无 root 的图片 | 按"每张图片都有 history root"这一不变量（用户确认），直接抛 `image N has no edit history root`，缩略图请求以 `kError` 带该原因结束。为了让这个不变量在代码里成立，`ImportServiceImpl` 的 pipeline service 参数改为必填（原本默认 `nullptr`，此时导入不建 root）；28 处测试夹具随之补上 |
| CPU 快照缓存 | 存储构建的快照放在 16 项 LRU 中（§6 决策 4）。复用前用新增的 `CommitGraphStore::GetMaterializedHistoryLabel` 读三列 `(root, head, chain)`（不读、不解析 checkpoint 文档），与快照标签相同才复用。所以直接写存储的写者（Paste 到库中的图）不需要任何失效调用。编辑器发布的快照放在 LRU 之外：淘汰它会退回到落后于 journal 的存储状态，而编辑器对同一状态只发布一次。编辑器释放图片（`ReleaseEditorPipeline`）后该条目移入 LRU、之后按存储校验；删除图片时清除 |
| 两条重放路径合一的前提 | 原 `pipeline_service.cpp` 匿名命名空间里的 root 解码、checkpoint 解码、相机 profile 绑定和按 root 重放移到 `app/pipeline_root_state.{hpp,cpp}`。编辑器加载与快照构建共用，不新增第三条重放路径（E8） |
| 编辑器 commit 钩子 | 计划写的是 typed batch 发布成功后发布。实际 HEAD 与文档还会被 undo / redo / 移动 HEAD、Version checkout、新建 / 分支 Version、Paste、discard、WAL 恢复改变，所以钩子放在 `EditorSessionHistoryPort`：打开图片后和每个改历史的操作之后调用 `EditorHistoryState::PublishCommittedSnapshot`。它在 owner 线程冻结 live 文档（写者就是这个线程，不需要 render lock），以 `(lineage, head, chain)` 去重，**live 文档含未提交值时不发布** |
| 未提交值的统一规则 | 调查发现 Mask 拖动与滑块拖动做的是同一件事（记录 before → 写 live → settle 成 typed batch / 取消恢复），但 Mask 不进 pending 序列：`unsettled_preview_` 在 Mask 拖动时不置位，保存路径会把临时蒙版写进声称等于 HEAD 的 checkpoint。现在 `LockedMaskDocumentOp` 在每条返回路径报告输入序列是否仍打开（`EditorMaskCreationController::HasOpenOperation`，其注释原来写"已有蒙版的编辑为 false"，与代码不符，已更正），`HistoryWorkingState::HasUncommittedLiveValues()` = 滑块 pending 非空 **或** Mask 输入打开，`unsettled_preview_` 与快照发布都用它 |
| 边界 settle 覆盖 Mask | `SettlePendingInputForBoundary` 原来只把拖动中的滑块当松手提交，现在拆成 `SettlePendingParameterInputForBoundary` + `SettleMaskInputForBoundary`：先应用排队的 Mask 命令，再用 `FinishMaskInput` 把仍打开的 Mask 输入当松手提交。排队消费与边界 settle 共用 `ApplyMaskCommandsToLiveDocument`，没有第二条 Mask 写路径 |
| `ThumbnailService` executor 池 | 拥有 `BatchExecutorPool`（默认 2，`kDefaultBatchExecutorCount`，构造参数可改）和同样数量 worker 的 `PipelineScheduler`；不再使用 `RenderService` 的静态池（R6，该静态池现在只有导出在用）。executor 首次使用时创建，后端偏好在服务构造时从 `PipelineMgmtService` 取（生产中 `project_handler.cpp` 先设偏好再构造服务）；后端不可用时该次渲染以真实错误失败 |
| 每个任务的流程 | lookup worker：检查取消 → `AcquireCommittedSnapshot` → 用快照标签查盘缓存 → 命中则交付；未命中则调度渲染：`prepare_` 读图并从池里取一个空闲 executor，`snapshot_under_render_lock_` 直接返回该快照，`on_complete_` 归还 executor 并在未交付时以 `kError` / `kCanceled` 结束。`LoadPipeline`、`ReleasePipelineUse`、`HasGpuDagDocument` 检查全部删除（C1） |
| 盘缓存 key | `edit_version_hash` = `head`（首个 commit 之前为 `root`）+ `:` + `chain`，`cache_schema_version` 升到 3（旧条目自然失效）。删除 `ReadCurrentVersionHash`（C2：原本每个请求在调用线程上开 DB 并 `LoadGraph`）、`RenderedCommitLabel`、`CommitLabelFromLiveGuard`、`EnqueueDiskWriteIfCommitLabelMatches` 与 `ThumbnailDiskCacheWriteAllowed`（C3）。key 与像素出自同一个快照，所以没有需要检查的不一致 |
| 合并两条渲染路径（C4） | 缩略图与分析共用 `RenditionRequest`（一次性交付：`DeliverPixels` / `Fail` 只生效一次）和 `State::StartRendition` / `LookUpRendition` / `ScheduleRenditionRender`。两者只在交付方式上不同：缩略图进内存 LRU 并应答所有等待者，分析一次性回调。`analysis_tokens_` 删除，改为 `RequestAnalysisRendition` 返回的 `AnalysisRenditionId`，取消只作用于这一个请求。原来以 `(element, resolution)` 为 key 的取消会连带取消另一客户端（图像分析与语义生成可同时运行）对同一图同一档的请求 |
| provider 适配器合并（C5） | `IImageAnalysisThumbnailProvider` / `ISemanticThumbnailProvider` 与两个逐字重复的适配器删除，统一为 `thumbnail_service.hpp` 中的 `IAnalysisRenditionProvider` 与 `ThumbnailServiceAnalysisRenditionProvider`（每个客户端一个实例，把自己的 key 映射到请求 id） |
| scheduler（R2、R5） | `MakeApplyRequest` 只有 Interactive 请求读取 executor 的 frame sink，Batch 请求一律不带 sink（THUMBNAIL 与 EXPORT 的置空特判删除）。`notify_thumbnail_failure_callbacks` 与 `require_gpu_valid` 删除：失败统一经 `on_complete_`；Batch 渲染没有 host 像素时以 `render produced no host pixels` 失败，原来会以"成功"结束且不调 callback |
| 生命周期 | 队列中的任务持有 `ThumbnailService::State`；`~ThumbnailService` 先 `StopWorkers()`（丢弃未开始的任务、等待运行中的任务），避免最后一个任务在 worker 线程上析构自己所在的线程池。新增 `PipelineScheduler::Shutdown()` |

**与计划不同或计划未写明的决定：**

- **P2A 不是阻塞项：** P3 记录写"P2A 仍需在 P4 共享已提交快照前完成"。核实后，Batch renderer 每次渲染都会 `ReleaseSessionResources()`，其中清空 `invalidation_` 与 `parameters_`，所以节点内的蒙版计数器对缩略图池没有影响；编辑器的 Interactive executor 按谱系释放，也不受影响。P2A 仍是蒙版拖动冻结成本的问题，与 P4 无关。
- **Mask 输入改走统一规则**（见上表）是用户在本阶段要求的：像 Mask 拖动这样看似特殊的路径，如果没有必须特殊的理由，就应走通用逻辑。Mask 与滑块在输入表示上仍不同（滑块 pending 只能表示"某个参数字段的 before JSON"，装不下"临时新增的蒙版"或"蒙版 source 替换"），完全统一需要把待提交输入统一表示为已应用在 live 上、可逆的 typed batch。这属于 P6 让未提交状态成为会话内部状态的范围，本阶段只统一了判定与边界 settle。
- **Open / Switch / Close 仍先取消 Mask 输入：** 这三个入口在进入 seal 之前调用 `AbortMaskCreation`（取消），而滑块拖动在同样的边界会被提交。按住指针拖动时通过 UI 切换图片的交错很难发生，本阶段没有改变这一行为，留作已知差异（见 Remaining gaps）。
- **执行器数量：** 缩略图并行度从共享静态池的 `max(2, 硬件线程数 / 2)` 个 worker 降为 2 个 executor（§6 决策 1 的默认值）。

**主调用链（成功路径）：**

```text
缩略图 / 分析请求 → ThumbnailService::GetThumbnailDetailed / RequestAnalysisRendition
  -> 内存 LRU 命中则直接交付（仅缩略图）
  -> State::StartRendition → lookup worker：LookUpRendition
       -> PipelineMgmtService::AcquireCommittedSnapshot(element)
            编辑器持有 → CommittedSnapshotCache 中编辑器发布的快照
            否则       → GetMaterializedHistoryLabel 与缓存快照比对 → 相同则复用
                         否则 LoadCommittedSnapshotFromStorage（checkpoint 或 root 重放 + 相机 profile）
       -> 盘缓存 key = (element, 档位, 用途, head:chain, schema 3) → 命中则交付
  -> ScheduleRenditionRender → ThumbnailService 自己的 PipelineScheduler worker
       -> prepare_：读图 + BatchExecutorPool::Take
       -> executor render lock（每个 executor 私有）→ Apply(snapshot, Batch) → host 像素
       -> callback_：RGBA8 → DeliverPixels（缩略图：LRU + 所有等待者；分析：一次性回调）→ 盘缓存写入
       -> on_complete_：归还 executor

编辑器已提交状态变化 → EditorSessionHistoryPort::<操作> → PublishCommittedAfter
  -> EditorHistoryState::PublishCommittedSnapshot（owner 线程）
       -> HasUncommittedLiveValues() 为真（滑块 pending 或 Mask 输入打开）→ 不发布
       -> (lineage, head, chain) 与上次相同 → 不发布
       -> live.Freeze() → PipelineGraphSnapshot::Committed → PipelineMgmtService::PublishCommitted
  -> 之后该图的缩略图 / 分析渲染这个快照；拖动中的值永远不会出现在缩略图、分析输入或盘缓存中

显式保存 / Paste / Version 操作 → SettlePendingInputForBoundary
  -> 滑块：拖动中的序列当松手提交
  -> Mask：排队命令 + FinishMaskInput（ApplyMaskCommandsToLiveDocument）→ settle 成一个 typed commit
  -> CaptureSaveCheckpoint 看到的 live 文档等于 HEAD
```

**失败路径：**

```text
图片没有 history root → LoadCommittedSnapshotFromStorage 抛 "has no edit history root"
  -> LookUpRendition 以 kError 交付该原因，不渲染任何替代文档
root / checkpoint / 历史解码或重放失败 → 同上，带真实错误
请求在任一阶段被取消 → Fail(kCanceled)；缩略图的旧请求代次已变时不动新请求的等待者
图片不在 image pool → prepare_ 抛出 → on_complete_(false, 原因) → kError
executor 后端不可用 → BatchExecutorPool::Take 抛出 → 同上
Batch 渲染没有 host 像素 → scheduler 以 "render produced no host pixels" 失败
编辑器发布失败（仅冻结 / 构造异常）→ qWarning 记录，历史操作本身不回滚；缩略图保持上一个已提交状态
边界 settle 时 Mask 命令被拒 → 取消打开的 Mask 输入，settle 返回错误，保存 / 切换以该错误拒绝
```

**What was proven (executed tests)：**

| 名称 / 条目 | 目标 | 结果 |
|---|---|---|
| 退出条件 2（计时）：`EditorFrameLatencyStaysUnchangedWhileThumbnailsRender` | `ThumbnailCommittedRenderTest` | PASS。同一张图持续渲染 k2048 缩略图时，编辑器帧（FAST_PREVIEW，guard executor）中位 171.8 ms，空闲时 160.6 ms；单个 k2048 缩略图中位 3411 ms（debug）。P4 之前编辑器帧会排在整个缩略图渲染之后 |
| 退出条件 2（确定性）：`ThumbnailRendersWhileTheEditorHoldsTheRenderLockOfTheImage` | 同上 | PASS：编辑器持有 render lock 期间，同图缩略图完成且为 kReady；guard executor 没有创建 batch renderer；`PipelineLoadCount` 为 0 |
| 退出条件 3：`DiskCacheEntriesAreLabelledWithTheRenderedCommittedState`（替代 `QueuedRenderDoesNotStorePixelsUnderStaleCommitLabel`） | 同上 | PASS：root 与 +1.5 EV 两个已提交状态各自写入自己标签的盘条目；新服务（内存缓存为空）分别发布两个状态后请求，两次都命中盘缓存（`hit_count` +2），与当初交付的像素差 1.31 / 1.52（JPEG），两个状态之间差 32.6 |
| 未提交值不进缩略图：`ThumbnailRendersTheCommittedStateNotUncommittedEditorValues` | 同上 | PASS：live 文档 +2 EV 未提交时缩略图与已提交状态逐像素相同；提交并发布后差 46.1 |
| 快照构建与复用：`StoredSnapshotEqualsTheDocumentTheEditorLoads`、`StoredSnapshotIsReusedUntilTheStoredHistoryChanges`、`ImageWithoutHistoryRootFailsWithTheRealError`、`PublishRejectsAPreviewSnapshot` | 同上 | PASS |
| 编辑器发布：`OpeningTheImagePublishesItsCommittedState`、`SliderPreviewIsNotPublishedUntilItSettles`、`CancelledPreviewLeavesThePublishedStateUnchanged`、`OpenMaskInputIsUncommittedUntilItsSettle`、`ReleasedImageIsServedFromStorageWhenItDiffers` | `EditorSessionHistoryPortTest`（新文件 `editor_committed_snapshot_publication_test.cpp`） | PASS |
| Mask 边界 settle：`OpenMaskDragIsReportedAsUncommittedInput`、`PersistCommitsAnOpenMaskDragBeforeTheSaveSealLikeASliderDrag` | `EditorPendingInputSessionTest`（驱动真实 `EditorSessionService`，fake history port 持有真实 live 文档） | PASS：拖动中报告为未提交；`PersistCurrentImage` 在 capture 之前把它 settle 成一个 `AddMask` commit，capture 时无打开的输入 |
| 缩略图不碰 guard：`ThumbnailRendersOnItsOwnExecutorAndLeavesTheLiveGuardUntouched`（原 `OrdinaryThumbnailReusesLiveEditorExecutorAndDocument`）、`ThumbnailAndAnalysisLeaveTheLiveGuardUntouched`（原 `BackgroundTasksReuseLivePipelineAndDocument`） | `ThumbnailServiceTest`、`PipelineSharedUseTest` | PASS：guard 的 executor、文档、谱系、pin、dirty 不变，没有创建 batch renderer，`PipelineLoadCount` 为 0 |
| 错误路径：`ThumbnailOfImageWithoutHistoryReportsError`、`ThumbnailOfImageMissingFromThePoolReportsError`（取代预存失败的 `MissingPipelineThrows` / `MissingImageThrows`：`GetThumbnail` 是异步的，不会抛出） | `ThumbnailServiceTest` | PASS：均以 `kError` 交付真实原因 |

删除的测试（断言的正是被移除的共享行为）：`ThumbnailDiskCacheWriteAllowedRejectsStalePreviewAndDirtyLabels`、`QueuedRenderDoesNotStorePixelsUnderStaleCommitLabel`、`BackgroundCompletionSignalsAfterPipelineUseRelease`（`PipelineSharedUseTest`），`ThumbnailRenderUsesLiveDocumentWithoutStageApplyOnto`、`AnalysisRenditionUsesLiveDocument`（`ThumbnailServiceTest`，由 `ThumbnailCommittedRenderTest` 的已提交状态用例取代）。

Commands（PowerShell，PATH 前置 `build\debug\vcpkg_installed\x64-windows\debug\bin`）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8          # 全量构建，0 错误
ctest --test-dir build/debug -j 1 -R "^(ThumbnailCommittedRenderTest|PipelineSharedUseTest|ImageAnalysisServiceTest|ImageAnalysisControllerTest|SemanticGenerationServiceTest|ImportServiceTest|ExportServiceTest|FilterServiceTest|ImportRawOnlyTest|CiRawWorkflowTest|ExecutorSnapshotRenderTest|ExecutorIsolationTest|GpuDagModelGraphTest|GpuDagRawInputTest|GraphImageCacheRetentionTest|GpuDagCuda(Workspace|Develop|Mask|PrimaryGrade|DrtProduct|DocumentGeometryRequest)Test|GpuDagOpenCl(Grade|Workspace|DrtProduct)Test|AdjustmentTransfer.*|EditorAdjustmentContextTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|EditorNodeGraph.*|EditorMask.*|EditorSession.*|EditorHistory.*|EditorVersion.*|EditorParameterWrite.*|EditorAdjustmentPipelineTest|PipelineMapperTest|PipelineServiceTest|PipelineGraph.*|PipelineDocument.*|PipelineHistory.*|PipelineEditBatchTest|PipelineDngProfileBindingTest|PipelineSchedulerRequestIdTest|PipelineFrameSinkTest|ImportPipelineDocumentTest|MiniGit.*|DocumentTransfer.*|CommitGraph.*)\."
ctest --test-dir build/debug -j 1 -R "^(EditorPendingInputSessionTest|AnalyticMaskCreationTest|EditorSession.*|EditorMask.*|EditorDocumentHistory.*|ThumbnailCommittedRenderTest|PipelineSharedUseTest|ImageAnalysisServiceTest|ImageAnalysisControllerTest|SemanticGenerationServiceTest|ImportServiceTest)\."   # Mask 边界 settle 与格式化之后
ThumbnailServiceTest.exe --gtest_filter=-*FuzzScroll*
```

Suite totals：

- 定向回归集 1472 个（Mask 边界 settle 与格式化之前）：1463 通过，9 失败，与 P2 / P3 记录的预存失败完全相同：`EditorSessionRenderSchedulerPortTest` 5 个、`GpuDagOpenClWorkspaceTest` 2 个（P1 记录的测试清理缺陷）、`EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`、`EditorSessionActionPolicyCq3Test.AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`。
- Mask 边界 settle 与格式化之后重新全量构建（0 错误），第二轮 445 个（Mask / 编辑器会话 / 历史 / 缩略图 / 分析 / 导入套件）：438 通过，7 失败，即上面的 7 个非 OpenCL 预存失败。
- `ThumbnailServiceTest`（排除 FuzzScroll）：23 通过，1 跳过（Metal 用例），0 失败。P3 记录的 2 个预存失败已改写为上表的错误路径测试。另有两个用例的测试数据被 P4 暴露出问题并已修正：`DiskCacheTracksRootAndActiveHeadAndServesAfterPipelineIsRemoved` 手工构造的 commit 把 before 写成 0 EV，而导入的 DNG 自带非零曝光。以前缩略图读元素 JSON、从不重放历史，这条无效 commit 从未被执行，现在重放会以真实错误拒绝它，改为以已提交值为 before。`ThumbnailRenderUsesInjectedRawMetadataForDng` 的直接渲染对照原来把 guard 文档做 JSON 往返（丢失已加载的 DNG profile），现在导入会建 root 并绑定 DNG profile，改为直接渲染同一个已提交快照。
- `ThumbnailCommittedRenderTest`：8/8 通过。
- 完整 ctest 按 AGENTS.md 未运行。

**Checklist / exit condition：**
- [x] `thumbnail_service.cpp` 不再引用 `PipelineGuard`、`LoadPipeline`、`ReleasePipelineUse`（grep 为零；头文件只在注释中写明"never loads a PipelineGuard"）
- [x] 编辑器拖动期间同图缩略图不阻塞预览：确定性测试 + 计时测试（数据见上）
- [x] 盘缓存 key 与渲染内容一致：`DiskCacheEntriesAreLabelledWithTheRenderedCommittedState`

**LOC note：** 生产代码 34 个文件（含 4 个新文件），测试 19 个文件。`thumbnail_service.cpp` 1193 → 1046 行，`pipeline_service.cpp` 1194 → 1080 行（root 状态 helper 移出，仍超过 1000 行，P7 计划减半）。新文件：`committed_snapshot_cache.{hpp,cpp}` 98 + 185 行，`pipeline_root_state.{hpp,cpp}` 92 + 142 行。`editor_session_service.cpp` 2532 → 2592 行，原本就远超 1000 行，本阶段只把 Mask 命令的锁定文档逻辑提成 `ApplyMaskCommandsToLiveDocument` 并加上边界 settle，没有拆分。新测试：`thumbnail_committed_render_test.cpp` 537 行，`editor_committed_snapshot_publication_test.cpp` 242 行。只对改动行运行 `git clang-format`；`image_analysis_service.hpp` 中 `RunJob` 声明被 clang-format 对齐到 100 列以外，保留了手工对齐。`pipeline_scheduler.hpp`、`render_service.hpp`、`import_service_test.cpp` 原为 CRLF，先在单独提交中转为 LF。

**Remaining gaps：**
- **Open / Switch / Close 取消 Mask 输入**，而滑块拖动在同样的边界被提交（见上）。要一致，需要让这三个入口也经 `SettleMaskInputForBoundary` 提交，本阶段未改。
- **Mask 输入的表示**仍与滑块不同，完全统一属于 P6（见上）。
- **导出仍借用 guard executor 并渲染 live 文档**（C6、C7），`RenderService` 静态池只剩导出使用，属于 P5。P0 测试 2（`DISABLED_ExportDuringUnsettledEditorPreviewUsesCommittedState`）保持禁用。
- **编辑器仍在 guard 上**：发布的快照取自 guard 的 live 文档，编辑器写 live 文档仍需 render lock（E1），直到 P6。
- **Metal 未编译**（本机无 macOS）：本阶段没有改 Metal 专有代码，但 `ThumbnailService` 的 executor 池在 Metal 上未运行。
- **UI 层未验证**：没有启动应用手动检查库视图缩略图与编辑器联动；`WorkspaceShellTest` 等 QML 套件按 AGENTS.md 未追查。

### P5 导出 executor，以及导入 / 复制 / 粘贴脱离 executor

**目标：** 剩下的非编辑器消费者全部不再构造或借用 executor。

- `ExportService` 拥有 1 个 Batch executor 和自己的队列：
  - 入队时取已提交快照并冻结进导出配方；
  - 输出色彩（DRT）从同一快照读，删除 `import_export.cpp:976-992` 的持锁读取（C6、C7）。
- `ImportService` 在私有文档上完成 root 初始化，调用 `PipelineMgmtService::InitializeImageRoot(element, document, raw_ctx)`
  的新重载，不构造 executor（S10、S11）。
- Copy 调整（`adjustment_transfer_controller.cpp:111-132`）：未打开的图用 `PipelineMgmtService::LoadHistorySnapshot(element)`
  （只读 CommitGraph + root），不构造 executor（C8）。
- Paste 到库中未打开的图（`adjustment_transfer_apply_coordinator.cpp:88-229`）：
  - 在私有 graph / 文档上完成重放；
  - 通过 `PipelineMgmtService::PersistHistory(element, graph, document)` 原子写入并发布新快照；
  - 失败时丢弃私有对象即可，删除对共享 guard 的回滚逻辑（C9）。
  - 打开中的图照旧经由编辑器会话（0580db1e4 的路由保留）。

**退出条件：**
- 除编辑器外，全仓不再有 `LoadPipeline` / `LoadEditorPipeline` 的调用方。
- P0 测试 2 通过。

##### Phase P5 completion record (2026-09-28)

**Status:** complete — 导出、导入、Copy、Paste 到库中图片都不再加载 `PipelineGuard`，也不再构造或借用编辑器的 executor。
导出在 `ExportService` 自己的 Batch executor 上渲染入队时取得的已提交快照，输出色彩从同一快照的 DRT 读取；
导入在私有文档上建立 root；Copy 读存储中的历史；Paste 在私有历史副本上完成，一次事务写入并发布新快照。
分支：`refactor/executor-ownership-p5`（基于 `refactor/executor-ownership-p4` 的 `8e2aba985`）。

**实现要点（与计划条目的对应）：**

| 计划条目 | 实现 |
|---|---|
| `ExportService` 拥有 1 个 Batch executor 和自己的队列 | `ExportService` 拥有 `BatchExecutorPool(1)` 和 `PipelineScheduler(1)`；编码仍在原来的 4 线程池上并行，渲染经这一个 executor 串行。P4 的 `BatchExecutorPool` 从 `thumbnail_service.cpp` 的匿名命名空间移到 `app/batch_executor_pool.hpp`，缩略图与导出共用一份实现。`render_service.hpp`（共享静态池）已无任何使用者，本阶段删除（计划原写在 P7） |
| 入队时取已提交快照 | `EnqueueExportTask` 调 `AcquireCommittedSnapshot`，快照与任务一起存进服务内部的 `QueuedExport`（调用方的 `ExportTask` 不变）。渲染任务的 `snapshot_under_render_lock_` 直接返回这个快照。编辑器持有的图返回编辑器最近发布的快照，拖动中的未提交值永远进不了导出（C6） |
| 输出色彩从同一快照读 | 配方没有显式 `output_color_` 时，`EnqueueExportTask` 从同一快照的 DRT 读取（`ExportColorProfileFromDrt`），像素与 ICC 用同一编码。`import_export.cpp` 的 `LoadPipeline` + render lock 读取整段删除（C7）。显式 `output_color_` 保留为"导出目标"（`ExportRecipe` 注释原本就写了"或显式目标"，`ExportPixelsAndIccUseTheSameRecipeColorConfiguration` 依赖它），它不是同一事实的第二来源：未设置时唯一来源是被渲染的快照 |
| `InitializeImageRoot(element, document, raw_ctx)` | 新签名，调用方交出私有文档：绑定源 DNG profile，再绑 RAW color context 或 Rec.709 工作空间 profile，校验后一次事务写 root、默认 Version、image edit state，最后写元素 JSON（兼容旧版本，§6 决策 3）。已有 root 时以 `image root already exists` 失败（root 不可变）。`ImportService` 传 `CreateDefaultPipelineDocument()`，不再 `LoadPipeline` / `SyncPipelineDocument` / `SavePipeline`（S10、S11）。原来每导入一张图就在 guard LRU 里留下一个带 executor 的条目，现在没有 |
| 旧的 guard 版 `InitializeImageRoot` | 删除公开接口。它的"已有 root"分支只是重复 `BindEditorStateFromStorage` 随后的读取；"没有 root"分支只剩编辑器打开时使用，改为私有的 `CreateMissingRootForEditor`（见 Remaining gaps） |
| Copy：`LoadHistorySnapshot(element)` | 返回 `ImageHistorySnapshot{graph_, root_}`（不可变的 CommitGraph 与解码后的 root，已绑定 DNG profile）。编辑器持有的图直接报错（其历史可能有尚未物化的 commit；controller 本来就把它路由到会话）。存储读取与校验抽成 `ReadStoredHistory`，编辑器加载（`BindEditorStateFromStorage`）、Copy、Paste 共用一条读取路径 |
| Paste：私有副本 + `PersistHistory` 原子写入并发布 | 计划签名是 `PersistHistory(element, graph, document)`。实现为 `PersistHistory(base, graph)`：文档由服务按 `graph` 的 active head 从 `base` 的 root 重放得出，而不是由调用方传入。这样 checkpoint 与历史只有一个来源，调用方无法写入与历史不一致的文档。一次 `Materialize` 事务写入新 commit、Version、image edit state 与 checkpoint（原来是先清 checkpoint 持久化、再在最后一个 pin 释放时回写 checkpoint 两步）；写前比对存储状态与 `base` 读取时的状态（与 `PersistEditorHistoryState` 共用 `SameMaterializedState`），不同则拒绝。成功后把快照放入已提交快照缓存并返回，Paste 的 HDR 标志从这个快照读 |
| 失败时丢弃私有对象 | coordinator 每个目标：`LoadHistorySnapshot` → 复制 graph → `PasteAsRootRelativeVersion` → `PersistHistory`。任何一步失败只记录失败，存储从未被改动。`restore_prior` 回滚 lambda、`RebuildActiveEditorPipeline` / `PersistEditorHistoryState` / `SavePipeline` 在这条路径上的使用全部删除（C9） |
| 打开中的图经由编辑器会话 | 不变：controller 把编辑器打开的图从目标中移出交给会话；`LoadHistorySnapshot` / `PersistHistory` 对编辑器持有的图报错，作为同一规则的第二道检查 |

**调查过的"特殊路径"（用户要求：没有特殊性就应走通用逻辑）：**

- **导出的输出色彩由 UI 单独读 live 文档：** 无特殊性。它与渲染读的是两个时刻的两个文档（C7），改为从被渲染的快照读。
- **Paste 两步写 checkpoint（先清空，最后一个 pin 释放时再回写）：** 无特殊性，是 guard 共享的副产品；回写失败还会被静默吞掉。改为同一事务写入。
- **Paste 把 `dirty_` 置 false、不写元素 JSON：** 追溯 `be021c955` / `587d89951`，没有记录理由。`PersistHistory` 不写元素 JSON，保持现有行为；元素 JSON 在 P5 之后已无渲染路径读取，是否对所有写者统一写入属于 §6 决策 3，未改。
- **`PersistHistory` 由调用方传文档：** 见上表，改为服务重放，去掉第二来源。
- **编辑器打开没有 root 的图时就地创建 root：** 这与 P4 确认的"每张图都有 root"不变量矛盾，而且对 RAW 图会绑定 Rec.709 工作空间 profile。调查结论：没有必须特殊的理由，应改为与快照加载一样以真实错误失败。但它属于编辑器加载路径（P6），且 68 处测试依赖在裸 id 上打开编辑器，本阶段只把它收窄为私有的 `CreateMissingRootForEditor` 并在此记录，未改行为。
- **研究工具 `HsResearchExportTool` 改 live 文档后导出：** 导出不再读 live 文档，所以改为像用户粘贴一样把调整提交为 root-relative Version 后再导出。

**主调用链（成功路径）：**

```text
导出：ImportExportHandler::BuildExportQueue（UI 线程）
  -> ExportService::EnqueueExportTask
       -> PipelineMgmtService::AcquireCommittedSnapshot(element)   编辑器持有 → 其发布的快照；否则存储
       -> 配方无显式色彩 → ExportColorProfileFromDrt(快照 DRT)
       -> QueuedExport{task, snapshot} 入队
  -> ExportAll → export_thread_pool_ → RunExportRenderTask
       -> ExportService::render_scheduler_ worker：prepare_ 取 BatchExecutorPool 的 executor
       -> Apply(snapshot, Batch) → host 像素 → on_complete_ 归还 executor
       -> ImageWriter（像素与 ICC 用同一 output_color_）→ 临时文件 → 提交文件

导入：ImportServiceImpl 元数据任务
  -> PipelineMgmtService::InitializeImageRoot(element, CreateDefaultPipelineDocument(), raw_ctx)
       -> BindSourceDngColorProfile → BindImportedCameraProfile / BindWorkingSpaceDevelopData → 校验
       -> CommitGraphStore::CreateRootPipelinePersisted（一次事务）→ 元素 JSON

Copy：AdjustmentTransferController::PrepareCopy（非编辑器图）
  -> PipelineMgmtService::LoadHistorySnapshot → ReadStoredHistory（LoadGraph + root + 校验 + DNG profile）
  -> dialog_model_->OpenSource(graph_, root_->document)

Paste：AdjustmentTransferApplyCoordinator::ApplyPackageToTargets（worker 线程，每个目标）
  -> LoadHistorySnapshot → CommitGraph 私有副本
  -> AdjustmentTransferService::PasteAsRootRelativeVersion(副本, root)
  -> PipelineMgmtService::PersistHistory(base, 副本)
       -> BuildDocumentFromRoot(副本 active head) → checkpoint
       -> DB 锁内：存储状态 == base 状态 → Materialize（commit + Version + state + checkpoint）
       -> PipelineGraphSnapshot::Committed → CommittedSnapshotCache::Publish
  -> FinishApply（owner 线程）：HDR 标志、缩略图失效与刷新（缩略图随后命中刚发布的快照）
```

**失败路径：**

```text
导出：图片没有 root / 历史无法构建 → EnqueueExportTask 抛出真实错误，任务不入队（UI 计入 skipped 与 first_error）
      快照无 DRT 或解析出的色彩无效 → 同上
      渲染 / 编码 / 提交失败 → ExportResult 带失败阶段，临时文件删除，executor 在 on_complete_ 归还
导入：root 已存在 → InitializeImageRoot 抛出，root 不变；导入记为元数据失败
Copy：编辑器持有该图 → LoadHistorySnapshot 抛出（controller 正常情况下先路由到会话）
      没有 root / 存储状态与 active Version 不符 → 抛出，PrepareCopy 返回错误
Paste：planner 拒绝 → 记录失败，存储未动
       重放失败 / 编辑器持有 / 存储状态已被其他写者改变 → PersistHistory 抛出，事务未开始或回滚，存储保持原状
```

**What was proven (executed tests)：**

| 名称 / 条目 | 目标 | 结果 |
|---|---|---|
| 退出条件 2：`ExportDuringUnsettledEditorPreviewUsesCommittedState`（去掉 `DISABLED_`） | `ExecutorIsolationTest` | PASS：编辑器 live 文档 +2 EV 且 `unsettled_preview_` 时导出，与已提交状态导出的像素差 ≤ 1 |
| 导出不加载 guard：`ExportRendersWithoutLoadingAPipelineGuard` | `LibraryHistoryAndExportTest`（新文件 `library_history_and_export_test.cpp`） | PASS：`PipelineConstructCount` / `PipelineLoadCount` 为 0 |
| 导出渲染入队时的已提交状态：`ExportRendersTheCommittedStateCapturedAtEnqueue` | 同上 | PASS：入队后再提交 +2 EV，该导出与参考导出平均差 ≤ 1；之后入队的导出平均差 > 5 |
| 输出色彩来自快照 DRT：`ExportIccProfileIsTheEncodingOfTheCommittedDrt` | 同上 | PASS：嵌入的 ICC 与快照 DRT 编码解析出的 ICC 逐字节相同 |
| 无历史的导出在入队时拒绝：`ExportOfAnImageWithoutHistoryIsRefusedAtEnqueue`、`EnqueueRefusesTasksThatCannotResolveTheirCommittedState`（取代 `ExportRecipeContainsResolvedOutputColorBeforeScheduling`） | 同上、`ExportServiceTest` | PASS：错误为 `has no edit history root`，`ExportAll` 结果为空 |
| 导入不加载 guard：`ImportCreatesTheRootWithoutLoadingAPipelineGuard` | `LibraryHistoryAndExportTest` | PASS：计数为 0；root 带 RAW color context 与有效相机矩阵；元素 JSON 与已提交快照文档相同 |
| root 不可变：`InitializeImageRootRefusesAnImageThatAlreadyHasARoot` | 同上 | PASS |
| Copy 读取：`HistorySnapshotIsReadWithoutAPipelineGuardAndStaysImmutable` | 同上 | PASS：之后的历史写入不改变已取得的快照 |
| 编辑器持有的图：`HistoryReadAndWriteRefuseTheImageTheEditorHolds` | 同上 | PASS：读、写都抛出，存储标签不变 |
| Paste 原子写入与发布：`PersistHistoryWritesTheNewStateAndPublishesItsSnapshot` | 同上 | PASS：存储标签、checkpoint 标签与文档都等于返回的快照；下一次 `AcquireCommittedSnapshot` 返回同一对象且不读存储；新服务从存储构建出相同文档 |
| 并发写者：`PersistHistoryRejectsAStaleBaseAndLeavesStorageUnchanged` | 同上 | PASS：第二个基于旧状态的写入被拒绝，存储保持第一个写入 |
| Copy / Paste 经 controller：`CopyDoesNotSaveOrRenderSourceImage`、`MultiTargetCoordinatorRefreshesOnlySuccessfulTargets`（新增断言） | `AdjustmentTransferControllerTest` | 见下方 Suite totals |
| 测试夹具：`CreateSeededPackedProject` 为每张图调用 `InitializeImageRoot`（与导入相同），使夹具满足"每张图都有 root" | `album_backend_seeded_project_fixture.hpp` | 见下方 Suite totals |
| 迁移：root 初始化 4 个用例（`ImageRootStoresCompleteDefaultDocumentAndDevelopData`、`NonRawImageRootBindsWorkingSpaceCameraProfile`、`EditorOpenOfExistingRawRootKeepsTheRawCameraProfile`（原 `InitializeImageRootOnExistingRawRootLeavesLiveCameraProfileUnchanged`）、`PersistedRawRootWithoutMatricesDoesNotReceiveWorkingSpaceProfile`）；library Paste 的 LUT 用例改用生产路径（`LibraryPasteWithoutStoredCheckpointStillRestoresLutFieldInLiveDocument` 先 Paste 再清 checkpoint） | `PipelineMapperTest`、`EditorSessionHistoryPortPersistTest` | 见下方 Suite totals |

Commands（PowerShell，PATH 前置 `build\debug\vcpkg_installed\x64-windows\debug\bin`）：

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8          # 全量构建，0 错误
ctest --test-dir build/debug -j 1 -R "^(LibraryHistoryAndExportTest|ExecutorIsolationTest|ExportServiceTest)\."
ctest --test-dir build/debug -j 1 -R "^(ThumbnailCommittedRenderTest|PipelineSharedUseTest|ImageAnalysisServiceTest|ImageAnalysisControllerTest|SemanticGenerationServiceTest|ImportServiceTest|FilterServiceTest|ImportRawOnlyTest|CiRawWorkflowTest|ExecutorSnapshotRenderTest|GpuDagModelGraphTest|GpuDagRawInputTest|GraphImageCacheRetentionTest|GpuDagCuda(Workspace|Develop|Mask|PrimaryGrade|DrtProduct|DocumentGeometryRequest)Test|GpuDagOpenCl(Grade|Workspace|DrtProduct)Test|AdjustmentTransfer.*|EditorAdjustmentContextTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|EditorNodeGraph.*|EditorMask.*|EditorSession.*|EditorHistory.*|EditorVersion.*|EditorParameterWrite.*|EditorAdjustmentPipelineTest|EditorPendingInputSessionTest|PipelineMapperTest|PipelineServiceTest|PipelineGraph.*|PipelineDocument.*|PipelineHistory.*|PipelineEditBatchTest|PipelineDngProfileBindingTest|PipelineSchedulerRequestIdTest|PipelineFrameSinkTest|ImportPipelineDocumentTest|MiniGit.*|DocumentTransfer.*|CommitGraph.*|SleeveServiceTest|AlbumBackend.*)\."
ThumbnailServiceTest.exe --gtest_filter=-*FuzzScroll*
```

Suite totals：

- P5 集（新文件 + 导出 + 隔离）：19 通过，0 失败，3 个预存 `DISABLED_`（`ExportServiceTest` 的 HDR / 批量 / 手工保留用例）。
- 定向回归集 1590 个（首轮）：1576 通过，14 失败。
  - 9 个是 P2–P4 记录的预存失败：`EditorSessionRenderSchedulerPortTest` 5 个、`GpuDagOpenClWorkspaceTest` 2 个、`EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`、`EditorSessionActionPolicyCq3Test.AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`。
  - `AdjustmentTransferControllerTest` 4 个：测试夹具 `CreateSeededPackedProject` 绕过导入直接写库文件，图片没有 history root；原来 Copy / Paste 经 `LoadEditorPipeline` 就地补建 root，掩盖了这一点。夹具改为像导入一样调用 `InitializeImageRoot` 建 root 后，4 个全部通过（没有放宽 Copy / Paste 的 root 要求）。
  - `SemanticGenerationServiceTest.GeneratesLabelsForRecursiveCameraSampleDatabaseAndSqlChecks`：测试体通过，`TearDown` 删除 DB 文件时文件仍被占用。stash 全部改动回到 `8e2aba985`、只重编该目标后同样失败，为预存问题。
- 修复夹具后重跑 `AdjustmentTransferControllerTest|AlbumBackend.*|LibraryHistoryAndExportTest|ExecutorIsolationTest|ExportServiceTest|SemanticGenerationServiceTest`：152 个，151 通过，1 失败（上面的预存 `TearDown` 失败）。
- `ThumbnailServiceTest`（排除 FuzzScroll，直接运行 exe）：23 通过，1 跳过（Metal 用例），0 失败。
- 完整 ctest 按 AGENTS.md 未运行。

**Checklist / exit condition：**
- [x] 除编辑器外，生产代码不再有 `LoadPipeline` / `LoadEditorPipeline` 的调用方：`LoadPipeline` 只在 `pipeline_service.cpp` 内部被 `LoadEditorPipeline` / `AcquireEditorPipeline` 调用；`LoadEditorPipeline` 只有编辑器的 `EditorSessionPipelinePort::EnsureLoaded`。测试中仍有调用，均为验证 guard 机制本身的用例（`pipeline_shared_use_test` 等），P7 随 guard 一起删除或改写
- [x] P0 测试 2 通过：`ExportDuringUnsettledEditorPreviewUsesCommittedState`

**LOC note：** 生产代码 12 个已有文件 + 新 `batch_executor_pool.hpp`（83 行），删除 `render_service.hpp`（32 行）。`pipeline_service.cpp` 1080 → 1144 行（新增 `ReadStoredHistory`、`LoadHistorySnapshot`、`PersistHistory`、新 `InitializeImageRoot`，删除旧 guard 版初始化与编辑器加载中的重复读取）；它原本就超过 1000 行，P7 删除 guard 状态机后计划减半，本阶段不拆。`export_service.cpp` 347 → 383 行，`thumbnail_service.cpp` 1046 → 993 行，`adjustment_transfer_apply_coordinator.cpp` 305 → 234 行。新测试 `library_history_and_export_test.cpp` 442 行。只对改动行运行 `git clang-format`（撤回了它对 `export_service.cpp` 中未改动 include 的重排）。

**Remaining gaps：**
- **编辑器打开没有 root 的图仍会就地创建 root**（`CreateMissingRootForEditor`，RAW 图会拿到工作空间 profile），与"每张图都有 root"不变量矛盾。P6 改编辑器加载时应改为以真实错误失败，并迁移依赖裸 id 打开编辑器的测试。
- **`SleeveService` 的复制图片只复制元素 JSON、不复制历史**（`sleeve_service.cpp` 的 `ClonePipelineForDuplicate`）：复制出的图片没有 root，P4 起它的缩略图、P5 起它的导出都会以 `has no edit history root` 失败。这是 P4 不变量暴露的既有问题，不属于 executor 所有权，本阶段未改。
- **导出入队在 UI 线程上取快照**：编辑器持有的图直接返回发布的快照；其他图在 checkpoint 标签匹配时读 checkpoint，否则从 root 重放。原来这里在 UI 线程上 `LoadPipeline`（读 JSON 并构造 executor）。未测大批量导出入队的耗时。
- **`PipelineSharedUseTest.ParallelBackgroundRendersPreservePixelsAndReleaseWorkspaces`** 观察的是 guard executor 的 batch 工作区，缩略图（P4）与导出（P5）都已不在 guard executor 上渲染，断言恒成立。按 P7 计划随 `pipeline_shared_use_test` 一起改写。
- **UI 层未验证**：没有启动应用手动检查导出、Copy、Paste；QML 套件按 AGENTS.md 未追查。Metal 未编译（本机无 macOS）。

### P6 编辑器独占 executor

**目标：** 编辑器会话拥有工作文档、历史和唯一的 Interactive executor。滑块路径不再拿任何跨模块的锁。

- `EditorSessionService` 的会话状态：
  - 持有工作文档（`shared_ptr<PipelineDocument>`，owner 线程可写）、`MiniGitWorkingHistory`、CommitGraph；
  - 通过 `PipelineMgmtService::AcquireEditorLease(element)` 取得单写者租约，并获得加载好的历史与文档；
  - 退出时 `ReleaseEditorLease`。
  - 租约只是一个"谁在写"的登记，不携带 executor（取代 `editor_owned_`、S6、A1）。
- 编辑器 executor 生命周期：
  - 由 `EditorSessionRenderSchedulerPort` 持有，与编辑器的 `PipelineScheduler(1)` 同寿命；
  - 构造时一次性挂上 `IFrameSink`，删除每任务的 `configure_under_render_lock_` 重挂（R3）；
  - 换图时首帧自动触发 §3.3 的全量释放。
- 历史写路径（`ui/alcedo_main/album_backend/editor_history_*.cpp`）：
  - `LockLivePipeline` 改为直接操作会话工作文档，owner 线程串行，不再需要锁（E1）；
  - 删除 `DeferIfLiveOwnershipHeld`、`EditorSerialFrameAdmission` 的 owner-work 延期队列、`WaitForSessionIdle` / 析构中的 `processEvents` 泵（E2、E3）。
    `EditorSerialFrameAdmission` 只保留节流（`EditorInteractivePacing`）。
  - `CaptureAdjustmentBeforePreview` / `RestoreUnsettledPreview` 保留语义（撤销未提交预览），
    但 `unsettled_preview_` 成为会话内部状态。持久化只写已提交文档，不再需要到处检查（S7、E4）。
  - Version checkout、rebuild：会话在私有文档上重放，成功后替换会话的工作文档指针，下一帧的快照谱系改变 →
    executor 全量释放。删除 `BindLivePipelineDocument` 与 4 处回滚 lambda（S8）。
  - 两条重放路径合一：`PipelineMgmtService` 提供纯函数式的 `ReplayDocument(root, graph, head)`，
    history 与 checkout 共用（E8）。
- 编辑器 port：
  - `EditorSessionPipelinePort` 不再持有 guard，只提供租约与快照发布；删除空 `Acquire`、`EnsureLoaded` 的 `load_mutex_`、死代码 `CheckoutVersion`（E6、E11）；
  - `EditorHistoryState` 的 graph 身份检查删除，只剩一个 graph 实例（E7）；
  - navigation seal 的 render-idle barrier 退化为"取消本 executor 的在飞帧"。保存不再依赖渲染空闲（E9）。
- `SettlePendingInputForBoundary` 与跨图 pending 丢弃保留。这是队列语义，不是所有权问题。

**退出条件：**
- 编辑器路径 grep 不到 `GetRenderLock`、`PipelineGuard`。
- 每个滑块 tick 不再有 owner 线程上的锁等待。
- 671802168 的回归测试全部保留并通过（改写为针对租约的断言）。
- `WorkspaceShellTests` / `EditorViewportReceivesRealPointerAndWheelEvents` 等 UI 测试全绿。
- 手工验证：拖动滑块、Undo/Redo、Version checkout、库 ↔ 编辑器切换、几何面板开关与裁切确认、ROI 放大 detail patch。

##### Phase P6 completion record (2026-09-28)

**Status:** complete (the manual UI check was not run) — the editor session owns its working document, its CommitGraph, and its immutable root through a single-writer lease. The editor render port owns the only Interactive executor. The editor path takes no `PipelineGuard` and no cross-module lock. Every frame renders an immutable preview snapshot that the history publishes after each write. History operations no longer wait for the in-flight frame.
Branch: `refactor/executor-ownership-p6` (based on `95404292b` of `refactor/executor-ownership-p5`). CRLF → LF conversion of three test files is in its own commit, `2d1daf858`.

**Implementation notes (mapped to plan items):**

| Plan item | Implementation |
|---|---|
| Lease `AcquireEditorLease` / `ReleaseEditorLease` | `PipelineMgmtService::AcquireEditorLease(id) -> EditorHistoryLease{graph_, root_, document_}`. The lease table `editor_leases_` (under `lock_`) replaces `editor_owned_`. It records who writes and holds no executor and no document. It reads the history in the same way as Copy and Paste (`ReadStoredHistory`). The document of the active head comes from the checkpoint when its labels match, otherwise from `BuildDocumentFromRoot`. `EditorHoldsImage` checks the lease table. `ReleaseEditorLease` ends the lease, returns the editor's last published committed snapshot to the cache (to be checked against storage), and writes its document to the element pipeline JSON (§6 decision 3: writes continue). `Sync()` also writes this JSON for images the editor still holds. Removed: `AcquireEditorPipeline`, `ReleaseEditorPipeline`, `LoadEditorPipeline`, `BindEditorStateFromStorage`, `CreateMissingRootForEditor`, the guard versions of `CheckoutVersion` / `RebuildActiveEditorPipeline` / `PersistEditorHistoryState`, and `PipelineGuard::editor_owned_` |
| Session state holds the working document, history, and CommitGraph | `HistoryWorkingState{graph, root, document, journal, history, …}`. `document` is the new `EditorWorkingDocument` (`app/editor_working_document.{hpp,cpp}`, separate library `EditorWorkingDocument`): it holds the working document and its lineage, writes happen on the owner thread without a lock, `Replace` takes a new lineage, `PublishPreview` freezes the document and publishes it, and `CurrentPreview` can be read from any thread. `EditorSessionPipelinePort` only acquires and releases leases and exposes previews (`AcquireLease` / `ReleaseLease` / `CurrentPreview`). `IEditorPipelinePort` has only `CurrentPreview` left. The empty `Acquire`, `EnsureLoaded` with its `load_mutex_`, and the dead `CheckoutVersion` are deleted (E6, E11) |
| Editor executor lifecycle | `EditorSessionRenderSchedulerPort` owns one `PipelineExecutor(ExecutorRole::Interactive)`. It is created on the first frame with the service's backend preference and lives as long as this port and its `PipelineScheduler(1)`. At dispatch, each frame takes `pipeline_port->CurrentPreview(element)`; if there is none, the frame fails as stale. An image switch changes the lineage, so the first frame triggers the §3.3 full release. `ClearSessionContext` (close / before a switch) queues `ReleaseBinding()` behind the in-flight frame on the single worker. `configure_under_render_lock_` is no longer set: the sink is attached in the task's `prepare_` on the single worker (see "Differences from the plan") (R3) |
| History write path without locks (E1) | `LockLivePipeline` is deleted. All 30+ mutation sites write `state->document->Document()` directly. The history port's `PublishAfterWrite` (after every operation, including preview capture and Mask operations) calls `PublishWorkingSnapshots`: it always publishes the preview, and when there is no uncommitted value it publishes the same frozen document as the committed snapshot (one freeze per write) |
| Delete owner-work deferral and event pumping (E2, E3) | `DeferIfLiveOwnershipHeld` and the deferral queue in `EditorSerialFrameAdmission` (`DeferOwnerWork` / `HasDeferredOwnerWork` / `TakeDeferredOwnerWork`) are deleted, so Undo / Redo / Checkout run immediately. `WaitForSessionIdle` / `CancelSessionAndWait` have no production callers; they are removed from the port, the coordinator, the render controller, and both port interfaces, together with the port's `processEvents` pump. `EditorSerialFrameAdmission` keeps only pacing |
| `unsettled_preview_` becomes session-internal (S7, E4) | The guard flag is no longer written by the editor. The only rule is `HistoryWorkingState::HasUncommittedLiveValues()`: while it holds, no committed snapshot is published, and the save capture refuses with a real error in one place (every sealing save settles open input first). `CaptureAdjustmentBeforePreview` / `RestoreUnsettledPreview` keep their meaning |
| Checkout, new Version, branch, Paste: build and then swap (S8) | Each operation builds the target document privately first (`EditorHistoryState::BuildDocumentForHead` → `BuildDocumentFromRoot`), with its panel projection. It then changes the graph, calls `SelectVersion`, and persists (`PersistEditorHistory`), restoring only the graph on failure. After everything succeeds it swaps with `document->Replace`. `BindLivePipelineDocument` is no longer used by the editor, and all 4 document-restore lambdas are deleted |
| Merge the two replay paths (E8) | The history's own `ReplayWorkingDocumentFromImmutableRoot` (which ran when `PipelineMapper()` was null and did not bind the RAW color context) is deleted. The editor, snapshot building, Copy, and Paste all use `BuildDocumentFromRoot` (the plan's `ReplayDocument`) |
| Remove the graph identity check (E7) | `EnsureWorkingState` no longer checks `history->graph() != guard->commit_graph_`. Only one `CommitGraph` instance exists (`HistoryWorkingState::graph`, shared with `MiniGitWorkingHistory`) and no API can rebind it |
| Navigation seal (E9) | `StartRenderIdleBarrier`: Version operations that stay on the same image (Checkout / new Version / branch) only cancel this session's frames and mark `render_idle` at once; completion no longer waits for render idle. Switch / Close still wait for the in-flight frame to finish before releasing (see "Differences from the plan") |
| Keep `SettlePendingInputForBoundary` | Unchanged |

**Unusual paths investigated (user request: without a real special case they should use the common logic):**

- **Editor opens an image without a root and creates the root on the spot** (P5 remaining gap): no special case found. It was a side effect of guard sharing (an arbitrary element id could be loaded), and it bound the Rec.709 working-space profile to RAW images. Deleted: `AcquireEditorLease` now fails with the real error `has no edit history root`, like thumbnails and export. Test fixtures that open the editor on images in storage now create the root first with `InitializeImageRoot`, the same way import does.
- **Owner work queued behind the in-flight frame (`DeferIfLiveOwnershipHeld`)**: no special case; it existed only because the history had to take the render lock that the frame held. The investigation also found that the deferred retry ran inside the owner reduction and so **skipped the action policy check** (the rewritten `UndoAndCheckoutRunWhileAFrameIsInFlight` first showed that Undo in this fake setup is rejected by the policy because `can_undo` is false). After deletion, Undo goes through the same policy as every other command.
- **History's fallback replay path** (E8): no special case, and it disagreed with the main path about binding the camera profile. Deleted.
- **Version operations cleared the checkpoint when persisting (`CaptureMaterializationClearingSerializedPipelineState`) and wrote it back later**: the only reason was that the guard document could not be guaranteed equal to the new head. The session now builds the document of the new head itself, so `PersistEditorHistory` writes the history and that document's checkpoint in one transaction. WAL recovery used to do two steps (`PersistEditorHistoryState` + `SavePipeline`); it is now the same single transaction. Audit §3 item 6 (WAL recovery calling `SavePipeline` on the editor's own guard and releasing the editor's pin) is gone with it.
- **The GUI document published separately** (`EditorSessionService::PublishDocumentSnapshot` froze once more before each notification): no special case; it was a second publication point for the same fact. `pipeline_document()` now reads the preview the history publishes, and the Debug freeze check (fingerprint) moves into `EditorWorkingDocument::PublishPreview`.
- **The Mask "locked document" access (`WithLockedLiveDocument`)**: once the lock is gone the name is wrong. Renamed to `WithWorkingDocument` / `MaskDocumentOp` / `MaskSettle` / `mask_input_open`. The behavior is unchanged: Mask input follows the same uncommitted-value rule.
- **The render port's destructor `processEvents` pump: kept.** The in-flight frame can be waiting in `DirectFrameSink` for the scene graph to hand over a present slot, which needs the GUI thread to process events. This is a present handshake, not ownership, so it is a real special case. The comment now states the reason.
- **Close / Switch still wait for render idle: kept.** After release the in-flight frame may still present to the viewport sink. That is sink lifetime, not document ownership. Version operations on the same image no longer wait.

**Differences from the plan or not stated in the plan:**

- **Frame sink "attached once at construction"**: not done. The sink is the `EditorViewportItem` sink resolved at submit time, and the QML viewport can be recreated after the port is built. The final design: the task's `prepare_` re-attaches only when the resolved sink changed. It runs on the port's single worker, which has exclusive access to the executor, and it does not take the render lock (the `PipelineExecutor` precondition documentation was changed to "holds the render lock or has exclusive access"). `configure_under_render_lock_` has no editor user left and is deleted together with the scheduler in P7.
- **Element pipeline JSON**: previously the editor guard's `dirty_` caused it to be written at project `Sync()` or eviction. It is now written from the editor's last published committed snapshot at lease release and at `Sync()`. The source of truth is still the history. A failed write does not stop the lease from being released; the port reports it with `qWarning` (`ReturnLeaseToService`), because some release paths run in destructors.
- **When previews are published**: the plan says "freeze preview snapshot → submit {snapshot, intent}". In the implementation the history publishes after each write, and the render dispatch takes the latest preview. Reason: the coordinator merges and delays intents (the quality / detail slots can start on the worker thread after an earlier frame ends). A snapshot pinned to the intent would make a quality frame that starts later render a document that is already out of date. Taking the latest preview matches the old behavior of freezing at render time.
- **`PipelineGuard` / `LoadPipeline` / `FreezeLiveSnapshot` / `MakeLiveSnapshotSource` / `BindLivePipelineDocument`** now have no production callers and are used only by the guard mechanism's own tests. They are deleted in P7 together with the guard.

**Primary call chain (success path):**

```text
Slider: EditorSessionController::submitWrite → EditorSessionService (owner thread) consume
  -> EditorSessionHistoryPort::CaptureAdjustmentBeforePreview
       -> EditorHistoryMutation: ApplyEditorParameterWrite(state->document->Document())   no lock
       -> PublishAfterWrite → EditorHistoryState::PublishWorkingSnapshots
            -> EditorWorkingDocument::PublishPreview (Freeze, O(changed nodes))
            -> uncommitted value present → do not publish committed
  -> EditorSessionRenderController::RouteInitialRender → EditorRenderCoordinator::Submit
  -> EditorSessionRenderSchedulerPort::DispatchPipelineFrame (owner or worker thread)
       -> pipeline_port->CurrentPreview(element) → task.snapshot = that preview
       -> EnsureExecutor (Interactive, first frame) ; prepare_ attaches the viewport sink
  -> PipelineScheduler(1) worker: Apply(snapshot) → binding key (lineage, element) → Present
Settle / Undo / Version: history writes / builds → Replace → PublishWorkingSnapshots
  -> PipelineMgmtService::PublishCommitted(same frozen document) → thumbnails / export read it
Open: EditorSessionHistoryPort::Acquire → EditorHistoryState::AcquireWorkingState
  -> EditorSessionPipelinePort::AcquireLease → PipelineMgmtService::AcquireEditorLease
  -> WAL alignment (missing suffix: replay into a graph copy → BuildDocumentFromRoot → swap
     → PersistEditorHistory in one transaction → truncate WAL) → PublishWorkingSnapshots
Close: history Release → ReleaseLease → ReleaseEditorLease (end lease, write JSON)
  ; ClearSessionContext → worker ReleaseBinding
```

**Failure paths:**

```text
Image without root / history cannot be decoded or replayed → AcquireEditorLease throws, lease not held → open fails with the real error
Lease already held → AcquireEditorLease throws; LoadHistorySnapshot / PersistHistory refuse while the editor holds it
Frame for an image whose lease is not held → no preview → job fails as "stale", nothing is loaded
Version build / selection / persistence fails → only the graph and selection are restored; the working document was never replaced
PersistEditorHistory: storage state ≠ expected → refused, storage and graph unchanged
WAL recovery persistence fails → recovered state stays in memory, WAL stays on disk for the next save (same as before)
Save capture with uncommitted values → refused ("unsettled"), single check point
Element JSON write at lease release fails → lease is released anyway, port logs qWarning, history unaffected
Preview / committed publication throws → qWarning, the preceding history operation stands (same rule as P4)
```

**What was proven (executed tests):**

| Name / item | Target | Result |
|---|---|---|
| Exit condition 2 (no lock wait on the slider path): `SliderTicksCompleteWhileAFrameHoldsTheEditorExecutorLock` | `EditorSessionHistoryPortTest` (new file `editor_working_document_test.cpp`) | PASS: while another thread holds the Interactive executor's render lock the whole time, 8 slider ticks plus the settle finish within 10 s. The preview after each tick has that tick's value |
| `EditorWorkingDocument`: `ConstructionPublishesAPreviewOfTheWorkingDocument`, `PublishedPreviewKeepsItsValuesAfterALaterWrite`, `ReplaceTakesANewLineageAndPublishesOnlyWhenAsked` | same as above | PASS |
| Exit condition 3 (671802168 regressions as lease assertions): `HeldEditorLeaseRefusesStorageHistoryUsersAndReleaseKeepsThePublishedState` (was `EditorOwnedPipelineIsNeverReboundFromStorage`) | `PipelineMapperTest` | PASS: a second lease throws; `LoadHistorySnapshot` / `PersistHistory` throw; `AcquireCommittedSnapshot` returns the editor-published object (storage is still at root); after release the element JSON equals the published document, and a new lease returns the persisted state |
| 671802168: `HistoryOfAnUnacquiredImageIsNeverLoaded` (asserts `CurrentPreview == nullptr`), `SwitchCommitsQueuedEditBeforeTheSaveSeal`, `SwitchIsRefusedWithTheRealErrorWhenTheQueuedEditCannotCommit` | `EditorSessionHistoryPortTest`, `EditorPendingInputSessionTest` | PASS. `SplitHistoryGraphFailsClosedInsteadOfSavingAnEmptyHead` becomes `SaveCaptureAndHistoryReadDescribeTheSameWorkingHead` (a split is now impossible by construction; this asserts that the capture and the history read see the same head) |
| Lease / persistence: `EditorLeaseOfAnImageWithoutHistoryRootFailsWithTheRealError`, `PersistEditorHistoryWritesHistoryAndCheckpointInOneTransaction`, `PersistEditorHistoryRefusesAnImageTheEditorDoesNotHold`, `EditorHistoryPersistenceRejectsAConcurrentMaterializedHistoryChange`, `VersionCheckoutPersistsTheReplayedDocumentAsTheCheckpointOfTheNewHead`, `EditorLeaseOfAnUnreplayableActiveHeadFailsWithTheReplayError` | `PipelineMapperTest` | PASS |
| No deferral: `UndoAndCheckoutRunWhileAFrameIsInFlight` (was `UndoAndCheckoutWaitForOwnerWithoutBlockingGui`) | `EditorSerialFrameConsumptionTest` | PASS: while a frame is in flight, Undo runs immediately and Checkout finishes after the checkpoint, and the frame is still running |
| Editor executor ownership: `FrameRendersOnThePortExecutorWithTheResolvedSinkAttached`, `RenderOfAnImageWithoutAHeldLeaseFailsAsStale`, `FrameRendersThePreviewPublishedLastAtDispatch` (CUDA, real DNG: rendered pixels follow the last published preview, not unpublished writes; binding = `{preview lineage, element}`), `ClearSessionContextReleasesTheExecutorBindingAfterTheFrame` (CUDA: after clear, binding is empty and result count is 0; executor and device unchanged) | `EditorSessionRenderSchedulerPortTest` | PASS |
| Version operations write the checkpoint of the new head: `CheckoutDefaultAfterPastePersistsWithoutLiveIdentityError`, `ProjectReopenPreservesDagVersionsHistoryAndMasks`, `VersionCheckoutReplacesTheDocumentWithoutRetakingTheLease` | `EditorSessionHistoryPortTest` | PASS |
| Export / thumbnails do not read uncommitted values or touch the editor executor: `ExportDuringUnsettledEditorPreviewUsesCommittedState`, `ThumbnailRendersWhileTheEditorHoldsTheRenderLockOfTheImage`, `EditorFrameLatencyStaysUnchangedWhileThumbnailsRender`, etc. | `ExecutorIsolationTest`, `ThumbnailCommittedRenderTest` | PASS (editor side now uses the lease + `EditorWorkingDocument` + test-owned Interactive executor) |
| Library Paste: `LibraryPastePersistsTheReplayedRootDocumentOfTheNewVersion` (rewritten from the guard-based `PasteAsNewVersionBindsTargetDocumentWithoutMirror`) | `AdjustmentTransferServiceMiniGitTest` | PASS |

Deleted tests: `EditorRenderCoordinatorTest.CancelSessionAndWaitJoinsTheMatchingSchedulerWork` and `WaitForSessionIdleDropsPendingButDoesNotCancelInflight` (the API is deleted and had no production callers); `EditorSessionHistoryPortTest.SelectedNodeProjectionCompletesWhileRenderLockHeld` (the history no longer touches any executor, so the case cannot happen; projection is still covered by `UnspecifiedWriteUsesSelectedProjectionNodeNotPrimaryGrade`); `EditorSessionLifecycleTest.PipelineAcquireFailureReturnsFalseAndNoHistoryAcquire` (there is no separate pipeline acquire any more; replaced by `HistoryAcquireFailureLeavesTheSessionFailedWithoutAGuard`).

Commands (PowerShell; builds and tests serialized with `build\tmp\p6\serial.ps1`, which adds the vcpkg debug bin to PATH and sets `QT_QPA_PLATFORM=offscreen`):

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 8 -- -k 0      # full build, 0 errors
ctest --test-dir build/debug -j 1 -R "^(EditorSessionHistoryPortTest|EditorSessionPipelinePortTest|EditorSerialInputBoundaryTest|EditorSessionRenderSchedulerPortTest|EditorSessionLifecycleTest|EditorSessionEditControllerTest|EditorSessionRenderControllerTest|EditorSessionNavigationControllerTest|EditorSessionNodeCommandTest|EditorSessionCommandQueue.*|EditorSessionActionPolicy.*|EditorSessionCq5.*|EditorSerialFrameConsumptionTest|EditorPendingInputSessionTest|EditorRenderCoordinatorTest|LibraryHistoryAndExportTest|ExecutorIsolationTest|ExportServiceTest|ThumbnailCommittedRenderTest|PipelineSharedUseTest|ImageAnalysisServiceTest|ImageAnalysisControllerTest|SemanticGenerationServiceTest|ImportServiceTest|FilterServiceTest|ImportRawOnlyTest|CiRawWorkflowTest|ExecutorSnapshotRenderTest|ExecutorRoleTest|GpuDagModelGraphTest|GpuDagRawInputTest|GraphImageCacheRetentionTest|GpuDagCuda(Workspace|Develop|Mask|PrimaryGrade|DrtProduct|DocumentGeometryRequest)Test|GpuDagOpenCl(Grade|Workspace|DrtProduct)Test|AdjustmentTransfer.*|EditorAdjustmentContextTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|EditorNodeGraph.*|EditorMask.*|EditorHistory.*|EditorVersion.*|EditorParameterWrite.*|EditorAdjustmentPipelineTest|PipelineMapperTest|PipelineServiceTest|PipelineGraph.*|PipelineDocument.*|PipelineHistory.*|PipelineEditBatchTest|PipelineDngProfileBindingTest|PipelineSchedulerRequestIdTest|PipelineFrameSinkTest|ImportPipelineDocumentTest|MiniGit.*|DocumentTransfer.*|CommitGraph.*|SleeveServiceTest|AlbumBackend.*)\."
ctest ... -R "^(EditorSessionHistoryPortTest|EditorSessionPipelinePortTest|EditorSerialInputBoundaryTest|EditorSerialFrameConsumptionTest|EditorPendingInputSessionTest|EditorSessionNavigationControllerTest|EditorSessionLifecycleTest|EditorSessionNodeCommandTest|EditorRenderCoordinatorTest|EditorSessionRenderControllerTest|EditorSessionEditControllerTest|PipelineMapperTest|AdjustmentTransfer.*|LibraryHistoryAndExportTest|ThumbnailCommittedRenderTest|ExecutorIsolationTest)\."   # after formatting and the fake fix
ThumbnailServiceTest.exe --gtest_filter=DiskCacheTracksRootAndActiveHeadAndServesAfterPipelineIsRemoved
```

Suite totals:

- Targeted regression set, 1598 tests: 1587 passed, 11 failed. 10 are baseline failures already recorded in P2–P5: `EditorSessionRenderSchedulerPortTest` 5 (the fixture input is an empty `ImageBuffer`, so the Renderer throws "product path requires encoded image bytes" before presenting; the migrated tests still fail this way), `GpuDagOpenClWorkspaceTest` 2, `EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`, `EditorSessionActionPolicyCq3Test.AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`, and `SemanticGenerationServiceTest.GeneratesLabelsForRecursiveCameraSampleDatabaseAndSqlChecks` (TearDown). The remaining one was `UndoAndCheckoutRunWhileAFrameIsInFlight` (the fake history port had no `can_undo`, see above). After fixing it and formatting, the 370-test editor / lease / export / thumbnail subset ran: 370/370 passed.
- `EditorMultiSliderQuickTest`: fails at `compile()` of `tst_multi_slider_handoff` with `module QtQuick.Controls plugin qtquickcontrols2plugin not found`. The QML import fails in the test runtime before any C++ changed in this phase runs. It was not compared against a clean HEAD. A teardown use-after-free in its `QuickTestSetup` (the backend was destroyed before its QObject children) was fixed during migration, and it now exits with 1 instead of crashing.
- `ThumbnailServiceTests.FuzzScrollBrowsingSharedPtrLifetimeStress` (50,000 iterations) was not assessed: it ran more than 20 minutes while holding the build lock and was stopped by hand.
- Full ctest was not run, per AGENTS.md.

**Checklist / exit condition:**
- [x] No `GetRenderLock` or `PipelineGuard` on the editor path: grep over `ui/alcedo_main/album_backend/editor_*`, `app/editor_*`, and `include/{app,ui/alcedo_main/album_backend}/editor_*` finds no calls. The only remaining `GetRenderLock` is inside `PipelineScheduler`'s shared task execution (the executor's internal lock, uncontended for the editor executor; P7 cleans up the scheduler)
- [x] No lock wait on the owner thread per slider tick: `SliderTicksCompleteWhileAFrameHoldsTheEditorExecutorLock`. The only mutex on the slider path is the history port's own `mutex_` (it serializes GUI history reads such as `ReadHistorySnapshot`, is held briefly, and never waits for rendering)
- [x] All 671802168 regression tests kept and passing (rewritten as lease assertions; see table)
- [ ] `WorkspaceShellTests` / `EditorViewportReceivesRealPointerAndWheelEvents`: `WorkspaceShellTest` is retired in `tests/ui/CMakeLists.txt` and not built (ctest: "No tests were found"). Not run
- [ ] Manual checks (dragging sliders, Undo/Redo, Version checkout, switching between library and editor, geometry panel and crop confirm, ROI zoom detail patch, first-frame time for decision 2): the application was not started, so not verified. This includes decision 2 in §6 ("does checkout do a full release"): the first-frame time after checkout and switch was not measured

**LOC note:** Production code: 46 files, +1106 / −1682; new files `editor_working_document.{hpp,cpp}` (88 + 51 lines). `pipeline_service.cpp` 1144 → 918, `editor_history_mutation.cpp` 1136 → 953, `editor_history_state_detail.cpp` 352 → 282, `editor_session_service.cpp` 2592 → 2527 (it was already far above 1000 lines; this phase only deletes, no split), `editor_session_render_scheduler_port.cpp` 597 → 629 (it now owns the executor). Tests: about 40 files, +2879 / −2019 (three sub-agents migrated them in parallel by file group; each semantic change is listed in their reports and in the table above). New test support: `tests/support/editor_lease_test_support.hpp` (in-memory lease; the root is bound in the same way as by `BuildDocumentFromRoot`, so opening and replaying give the same document) and `tests/support/editor_history_port_test_reads.hpp`. `git clang-format` was run only on changed lines; two realignments of unchanged lines were reverted by hand.

**Remaining gaps:**
- **UI layer not verified**: the application was not started, and the manual items above and the first-frame time after checkout were not measured. Metal was not compiled (no macOS on this machine); `metal_*` tests were not migrated with this phase and must be compiled on a Mac.
- **Guard code that is now dead**: `PipelineGuard`, `LoadPipeline`, `SavePipeline`, `ReleasePipelineUse`, `FreezeLiveSnapshot`, `MakeLiveSnapshotSource`, `BindLivePipelineDocument`, the scheduler's `configure_under_render_lock_`, and the executor's dual-role default constructor now serve only their own tests. They are deleted together in P7.
- **The history port's `mutex_`**: it serializes GUI reads of history (`history_snapshot()` / `active_version_id()` / panel projection) with the owner thread's writes. It is not a render lock, but a GUI read can make a slider tick wait a short time. Moving those reads to owner-published values (like `pipeline_document()`) belongs to a separate GUI read-path cleanup and is not changed in this phase.
- **Open / Switch / Close still cancel Mask input** (P4 remaining gap), unchanged.
- **`SleeveService` duplicates images without copying history** (P5 remaining gap): a copied image has no root, so after this phase it also cannot be opened in the editor (real error `has no edit history root`). Not fixed in this phase.

### Geometry 面板重构（#221，先于当前 P7）

**状态：** 代码完成（2026-09-28），相关测试通过；手工 UI 验证未做。缺陷记录：[#221](https://github.com/zidage/AlcedoStudio/issues/221)。

**原缺陷与根因：**

1. **裁切后旋转，不是旋转裁切后的画面。** 面板写死 `expand_to_fit=true`，输出是旋转后裁切框的外接框，四角采样到裁切框外的原图；叠加层又是"原图不动、框在转"。
2. **切图后 "Source aspect" 变化，裁切失效。** 面板写入的 `source_size` 被解析器丢弃，面板于是退回 `interaction.metricAspect`，而它来自 `EditorWorkspace.qml` 用首帧尺寸调用 `setImageSize` 的回退（可能是已裁切输出或视口大小的帧）。
3. **面板走独立的草稿通路**（Enter / 离开面板时确认、`panelDraftCommitRequested`、`setViewChangeRoutingEnabled`），与其他调整项不一致。

**语义（用户确认）：** Geometry 的输出就是裁切框这个长方形里的画面。裁切框在输出空间轴对齐，源图绕框中心旋转后由它取景；框的四个角不能超出源图，所以没有黑角。旋转时内容绕框中心转，框超出源图时按比例收缩。所有文档统一按此解释，`expand_to_fit` 删除。

**主要改动：**

- 管线：新增 `edit/geometry/crop_frame.{hpp,cpp}`（`ClampCropToRotatedSource`、`CropFrameCornersInReference`、`NormalizeRotationDegrees`），resolver 与 UI 共用同一个约束。`ImageGeometryParams.expand_to_fit` 换成 `GeometryOutputFrame`（`CropFrame` / 面板预览用的 `RotatedSourceBounds`）。`DocumentGeometryUse::UncroppedSource` 改为 `RotatedUncroppedSource`：整幅源图 + 文档旋转，外接框画布。GPU 重采样核不变。
- 文档模型：`ImageGeometryModel` 删除 `expand_to_fit`，新增 `aspect_preset` / `aspect_ratio`（旧 JSON 缺省取默认）。解析器拒绝 `source_size` / `enabled` / `enable_crop`；历史回放仍接受旧键 `expand_to_fit` 并忽略。投影回传比例字段；历史摘要读模型真实键。命令服务里未使用的重复几何解析器删除。
- 视口：裁切框以 presented frame 的 `ResolvedRenderGeometry` 为唯一坐标基准（与 Mask 共用 `MaskEditGeometry` 映射）；编辑在 frame space（reference 像素旋转 θ）里做轴对齐运算。源尺寸来自 `full_reference_extent`（`sourceImageWidth/Height`），删除 `setImageSize`、`metricAspect` 和 QML 首帧回退；真实缩放用 presented `edit_extent`。裁切工具打开时视图锁定在 fit（缩放、平移、双击、1:1 都不生效），不会出现 ROI detail patch。删除 `ViewChangeKind::CropRotate` 与 `setViewChangeRoutingEnabled`；指针编辑用一个信号 `cropFrameEdited(rect, degrees, isFinal)`。
- 面板：`EditorGeometryPanel.qml` 重写，模型带 `submitter` + `paramsBuilder`（与 Display Transform 面板同一通路）：拖动发 interactive，松手 / 键入 / reset 发 settled，每次 settled 一次提交。删除草稿、确认与 `RequestPanelDraftCommit` / `panelDraftCommitRequested`。Enter 仍回到 Tone，但不再提交。

**主调用链：**

- 滑块：`AdjustmentSlider` → `EditorAdjustmentValueModel::updateDrag/finishDrag` → `paramsBuilder` = `paramsAfter(driver)`（比例锁 + `interaction.clampCropRect`）→ `EditorSessionController::submitWrite` → pending input → `ImageGeometryModel::ApplyUpdate` → 渲染（面板打开时 `geometry_overlay_only` → `RotatedUncroppedSource`）。
- 叠加层：`EditorInteractionController::handlePress/Move/Release` → `CropInteractionController`（`maskEditViewMapping()`，frame space，`ClampCrop`）→ `cropFrameEdited` → 面板 `submitPatch("crop_rotate", …, isFinal)`。
- 渲染：`GraphCompiler::BindFrameGeometry` → `ImageParamsForRequest` → `ResolveRenderGeometry`（`ClampCropToRotatedSource`，输出 = 裁切框）。

**验证：** 相关测试目标 GpuDagGeometryTest、EditorCropInteractionTest（新）、OverlayCursorTest、MaskEditGeometryTest、EditorGeometryPanelQmlTest、EditorSessionControllerPhase5ATest、EditorPipelineCommandServiceTest、DocumentTransferTest、MaskThumbnailServiceTest、GpuDagCudaDocumentGeometryRequestTest、EditorPanelProjectionTest、EditorGeometryMathTest 全部通过。EditorSessionRenderSchedulerPortTest 有 5 个与几何无关的失败，在干净 HEAD 上同样失败。全量 ctest 未跑；OpenCL / Metal 未在本机运行；手工 UI 验证未做（见 `docs/editor_dialog_manual_test_matrix.md` 的 Geometry 一节）。

**遗留：** `editor_overlay_interaction_test.cpp` 与 `edit_viewer_logic_test.cpp` 未登记在任何 CMake 目标中（已按新 API 改写，但不编译）。新增 QML 文案未进 `.ts` 翻译。面板打开时，只改裁切框的 interactive 编辑仍会触发一次渲染（预览画面不随裁切变化）。

### P7 删除共享机制，收窄 `PipelineMgmtService`

**目标：** 删掉只为共享而存在的代码。

删除：
- `PipelineGuard` 整体。`PipelineMgmtService` 内部改为"快照缓存项 + 租约表"。
- `pin_count_`、`pinned_`、`live_ready_`、`initializing_`、`load_error_` 的 executor 部分，以及 `LoadPipeline` 状态机（S1）。
- `HandleEviction` 的 pin 跳过、`+5` 扩容和驱逐写回（S2）；`CleanupIdlePipelineResources`（S3）；
  `ReleasePipelineUse`、`SavePipeline` 的 pin 语义与 `will_release_last_pin`（S4）；`WaitUntilPinCount`（S5）。
- `SetAcceleratorBackendPreference` 的遍历（S9）。
- `Storage::live_pipelines_` 及 `RememberLivePipeline` / `GetLivePipeline` / `ForgetLivePipeline`，以及 `storage.hpp` 的 executor 前置声明（S12）。
- scheduler 的 `prepare_` 取 guard、`completion_guard` 延后完成的 hack、`configure_under_render_lock_`（R4）。
- `render_service.hpp` 的共享静态池（若 P4 / P5 后已无用户）。
- `pipeline_service.hpp` 对 `image_pool_service.hpp` / `pipeline_scheduler.hpp` 的无用 include。

测试：
- `tests/app/pipeline_shared_use_test.cpp` 中验证"共享不出错"的用例删除，改写为隔离性测试：
  各 executor 互不影响，快照不受后续编辑影响，换绑定后资源归零。
- `pipeline_service_test.cpp` 删除 `ForgetLivePipeline` 相关的冷加载绕行。

**退出条件：** `PipelineMgmtService` 的公开 API 只剩：加载 / 重放 / 持久化历史与文档、快照获取与发布、
租约、删除、root 初始化、垃圾回收。代码量显著下降（预期 `pipeline_service.cpp` 1176 行降到一半以下）。

### P8 文档与决策更新

- 在以下文档相关章节加 "Superseded by 2026-09-27 executor ownership refactor" 注记，并链接本方案：
  - single-live-pipeline 计划 A1；
  - NM1 §10.5 背景 1 与 §12；
  - G10 §6.1、§16.4；
  - phase_6c / 7a 的"PMS owns the live executor"。
- 关闭或更新：
  - 2026-05-24 文档的 frame sink 遗留项；
  - Issue #113（盘缓存标签）；
  - G10 中两个 pin 计数相关的 flaky 测试记录。
- 更新 `docs/technical/app/` 中涉及服务职责的说明。

---

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 重蹈 07–08 月 snapshot 方案被回退的覆辙 | 当时是"每个快照克隆一个 executor"、深拷贝参数、快照取自可变 live 状态。本方案 executor 数量是常数，快照 COW 共享节点，非编辑器只读已提交快照。在 P8 中正式废止旧决策并写明理由。 |
| P1 改 revision 协议导致编辑器增量渲染退化成全量重算 | P1 退出条件包含命中统计与基线对比；逐 backend（CUDA / OpenCL / Metal）验证。 |
| COW 漏掉某个非 const 访问路径，快照被原地修改 | 对 `Freeze()` 后的文档在 Debug 构建下记录节点 revision，渲染前后断言不变；P2 的 GUI 快照先行验证。 |
| 缩略图池 N 个 device 的显存占用 | device 本身只含 stream / queue，任务结束全量释放工作区。比现状（每张打开过的图保留一个 session device 到项目关闭）更少。N 可配置。 |
| 编辑器换图时全量释放导致首帧变慢 | 这是用户明确要求的语义。与现状相比，现状换图也要换 executor、缓存冷启动，差异只在源缓存。P6 手工验证首帧耗时。 |
| 已打开图片的缩略图在 P4 与 P6 之间的一致性 | P4 即引入 commit 钩子发布已提交快照，不依赖 P6。 |
| 多个 backend 的 pass 改签名工作量大 | P1 集中在 pass encoder 层做机械改动；Metal 若无本地构建环境，则以编译通过 + CI 为准，并在阶段记录中标明。 |

## 6. 需要确认的决策

1. **缩略图池大小 N。** 建议默认 2，可配置。现状的并行度来自"不同图片各自的 executor"，上限是 hw/2 个线程。
2. **Version checkout 是否全量释放。** 按 §3.3，checkout 会改变谱系，因此全量释放，包括已解码的源。
   若 checkout 首帧变慢不可接受，可以把源缓存的绑定键改为只看 source file identity，同图 checkout 保留解码结果。
   建议先按全量释放实施，P6 实测后再定。
3. **元素 JSON 的去留。** 建议 P4 起渲染不再读它；写入保持不变，以兼容旧版本读取。是否停止写入，另行决定，不在本次范围。
4. **已提交快照缓存的容量。** 纯 CPU 内存。建议沿用 16 项 LRU；它不再承载任何 GPU 资源。

## 7. 阶段状态

| 阶段 | 状态 |
|---|---|
| P0 基线与保护网 | 完成（2026-09-28） |
| P1 revision 协议 | 完成（2026-09-28） |
| P2 快照与 COW | 完成（2026-09-28） |
| P2A 蒙版逐项写时复制 | 未开始 |
| P3 executor 按请求接收快照 | 完成（2026-09-28） |
| P4 缩略图 / 分析池 | 完成（2026-09-28） |
| P5 导出、导入、复制、粘贴 | 完成（2026-09-28） |
| P6 编辑器独占 executor | 完成（2026-09-28，手工 UI 验证未做） |
| Geometry 面板重构（#221，先于 P7） | 完成（2026-09-28，手工 UI 验证未做） |
| P7 删除共享机制 | 未开始 |
| P8 文档与决策更新 | 未开始 |
