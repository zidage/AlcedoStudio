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
| P1 revision 协议 | 未开始 |
| P2 快照与 COW | 未开始 |
| P3 executor 按请求接收快照 | 未开始 |
| P4 缩略图 / 分析池 | 未开始 |
| P5 导出、导入、复制、粘贴 | 未开始 |
| P6 编辑器独占 executor | 未开始 |
| P7 删除共享机制 | 未开始 |
| P8 文档与决策更新 | 未开始 |
