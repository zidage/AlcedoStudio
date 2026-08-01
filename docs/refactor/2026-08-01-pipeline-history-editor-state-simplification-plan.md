# Pipeline / Edit History / Editor 状态简化修复方案

日期：2026-08-01

状态：可直接执行

## 1. 不可更改的设计结论

本修复严格遵守以下边界，不保留兼容性的第二套状态体系：

1. UI 和 Editor Session 不维护任何 `generation`。
2. 渲染侧生成单调递增的请求序号，并在提交帧前自行丢弃旧请求。
3. 不保存 immutable root，不保存 root pipeline JSON，不从 root 重建当前状态。
4. 默认状态由现有默认 operator 参数计算；目标版本状态由“默认参数 + 目标提交链”计算。
5. 图片固有数据直接保留在对应 operator 参数中，包括 CCM、as-shot CCT/Tint、相机与镜头型号、镜头校正输入或已解析系数。
6. `FramePresentationBroker` 保持废弃，并从源码、构建清单和测试中删除。
7. 不允许从 live executor 反向填充 history 基线；所有 `SeedImageLocalBaselineFromLivePipeline` 相关代码必须删除。
8. 不新增长期驻留的 runtime context、root snapshot、session snapshot 或其他状态副本。

## 2. 唯一状态来源

### 2.1 持久状态

唯一可持久化、可导入、可导出的图片编辑状态是 pipeline operator 参数。

operator 参数包含两类数据：

- 图片固有参数：相机和镜头型号、色彩矩阵、as-shot neutral、as-shot CCT/Tint、镜头校正输入或已解析系数。
- 用户编辑参数：曝光、饱和度、色温模式、自定义 CCT/Tint、镜头校正开关、裁切、曲线、LUT 等。

图片固有参数随图片导入后写入 operator，并在该图片整个生命周期内保留。用户编辑参数由 history commit 改变。

### 2.2 单次渲染参数

以下参数只属于一次渲染请求：

- 输出宽高；
- resize 比例和算法；
- viewport ROI；
- decode resolution；
- preview / export 类型；
- frame role；
- 取消标记；
- 渲染请求序号。

这些参数可以继续通过现有 `SetOperator` / `OperatorParams` / `SetRenderRegion` API 应用，但必须满足：

1. `ExportPipelineParams()` 不导出它们。
2. history 不记录它们。
3. paste / merge 不复制它们。
4. 单次 `Apply()` 结束或取消后，不能把它们遗留成下一次请求的输入。

### 2.3 后端参数

CPU、CUDA、Metal 和 OpenCL 使用的矩阵、系数与缓存键，由 operator 参数和本次渲染参数计算。

- CCM 在图片固有参数或色温编辑参数变化时重新计算。
- Lensfun 或镜头校正系数在镜头信息或镜头编辑参数变化时重新计算。
- 没有相关参数变化时直接复用缓存。
- 不在每帧开始时重复注入同一份 RAW 元数据。

## 3. 目标调用链

```text
ImportService
  -> 创建带默认参数的 CPUPipelineExecutor
  -> 解析 RAW / EXIF
  -> SetOperator(RAW_DECODE, 固有 RAW 参数)
  -> SetOperator(COLOR_TEMP, as-shot 参数)
  -> SetOperator(LENS_CALIB, 固有镜头参数和默认开关)
  -> ExportPipelineParams
  -> 保存当前 pipeline JSON

Editor open
  -> LoadPipeline
  -> ImportPipelineParams
  -> 直接读取 operator 参数更新面板
  -> 不创建 root snapshot
  -> 不从 live pipeline 填充 history

Edit
  -> UI 提交单字段 patch
  -> history 记录 before / after
  -> SetOperator
  -> 提交 render request

Version checkout
  -> 保存失败回滚所需的当前 pipeline JSON
  -> 对所有可编辑字段 SetOperator(默认参数)
  -> 保留同一 operator 内的图片固有参数
  -> 按顺序重放目标 Version 的 commit
  -> SetGlobalParams
  -> SetExecutionStages 一次
  -> 原子更新 active Version 与当前 pipeline JSON

Render
  -> PipelineScheduler 分配 request_id
  -> 应用本次 resize / ROI / decode 参数
  -> Apply
  -> 提交帧前比较 request_id
  -> 旧于该输出目标最新请求的帧直接丢弃
  -> UI 只收到已经通过检查的帧
```

## 4. 执行项

### 4.1 删除 Seed 补丁

1. 删除 `SeedImageLocalBaselineFromLivePipeline()` 声明和实现。
2. 删除 `SnapshotFieldParamsEmpty()`。
3. 删除 `EditorHistoryState::EnsureWorkingState()` 中所有 live executor 回读。
4. 删除为该行为增加的 `PasteThenCheckoutDefaultRestoresExposureAndKeepsSeededAsShotColorTemp` 测试。
5. 禁止增加任何同义的 populate、baseline repair 或 live fallback helper。

完成标准：

```text
rg "SeedImageLocalBaselineFromLivePipeline|KeepsSeededAsShot" alcedo_studio
```

没有结果。

### 4.2 删除 root 数据模型

修改以下核心类型：

- `edit/history/edit_commit.*`
- `edit/history/commit_graph.*`
- `edit/history/version_ref.*`
- `include/edit/history/commit_types.hpp`
- `include/edit/history/edit_commit.hpp`
- `include/edit/history/commit_graph.hpp`
- `include/edit/history/version_ref.hpp`

逐项执行：

1. 从 `EditCommit` 删除 `root_id_`、`GetRootId()` 以及所有构造参数中的 `root_id`。
2. commit hash 改为包含 `element_id`、父提交、payload 和时间戳，不再包含 root ID。
3. 从 `CommitGraph` 删除 `GetRootId()` 和所有 root 一致性检查。
4. commit 的归属使用现有 `element_id`，不再通过 root ID 间接定位图片。
5. 从 `ImageEditState`、`VersionRef`、checkpoint capture 和 materialization 删除 `root_id`。
6. 删除 `ComputeRootChainHash()`、`RootChainHashInput()`。
7. transaction chain 若只用于重复验证 head，则删除；active head 直接由 commit hash 标识。
8. 无父提交表示从默认 operator 参数开始计算，不表示存在一个持久 root 对象。
9. 将 UI 中的 “Create Root Version” 改为普通 “Create Version”；它只创建指向当前 head 的 Version ref。
10. 删除 `CreateRootVersionAndCheckout`、`CreateRootPipelinePersisted`、`GetRootSerializedPipelineState` 等 API。

完成标准：第一方代码中不再存在编辑历史语义的 `root_id`、`PipelineRoot`、`root_snapshot`、`immutable root` 或 `RootRelativeVersion`。

### 4.3 删除 PipelineRoot 持久化

修改：

- `storage/controller/db_controller.hpp`
- `storage/service/sleeve/edit_history/commit_graph_service.*`
- `storage/mapper/sleeve/edit_history/edit_commit_mapper.*`
- `storage/mapper/sleeve/edit_history/image_edit_state_mapper.*`
- `app/pipeline_service.*`
- `app/editor_mini_git_materializer.cpp`

逐项执行：

1. 删除 `PipelineRoot` 表。
2. 从 `EditCommit` 表删除 `root_id`，增加或使用 `element_id`。
3. 从 `ImageEditState` 表删除 `root_id`。
4. serialized pipeline state 只保存当前 materialized pipeline JSON 和对应 head commit hash。
5. `LoadEditorPipeline()` 只加载当前 pipeline JSON；checkpoint 不匹配时执行“默认参数 + commit 链”重建。
6. `PipelineMgmtService::InitializeImageRoot()` 整体删除。
7. `DecodedRootPipelineState`、`kRootRawColorContextKey` 和所有 root JSON 编解码整体删除。
8. 当前 pipeline JSON 必须已经包含渲染所需的图片固有 operator 参数，不再保存旁路 `raw_color_context`。

数据库迁移一次完成，不保留长期双格式读取：

1. 对每张已有图片，读取旧 root 参数并重放 active head，生成当前 pipeline JSON。
2. 把旧提交按父子顺序重算为不含 root ID 的新 commit hash。
3. 建立旧 hash 到新 hash 的临时映射，更新 first parent、second parent 和 Version head。
4. 把当前 pipeline JSON 与新 active head 写回 `ImageEditState`。
5. 删除旧 `PipelineRoot` 和带 `root_id` 的旧表结构。
6. 事务成功后才提升数据库 schema 版本；失败则整个迁移回滚。

### 4.4 导入时完成 pipeline 参数组装

修改：

- `app/import_service.*`
- `ui/alcedo_main/album_backend/project_handler.cpp`
- `edit/pipeline/default_pipeline_params.hpp`
- `edit/pipeline/pipeline_cpu.*`
- `edit/operators/raw/raw_decode_op.*`
- `edit/operators/basic/color_temp_op.*`
- `edit/operators/geometry/lens_calib_op.*`

逐项执行：

1. `ImportServiceImpl` 必须持有可用的 `PipelineMgmtService`，删除可选 `RootPipelineInitializer` callback。
2. 创建 pipeline 时直接安装默认 operator 参数，包括曝光 `+1.5`、饱和度 `+30`。
3. metadata extractor 的结果只在导入调用栈中存在。
4. 把相机型号、镜头型号、CCM、as-shot neutral、as-shot CCT/Tint、镜头参数写进对应 operator JSON。
5. `ColorTempOp` 明确区分 `as_shot_cct/as_shot_tint` 和 `custom_cct/custom_tint`；删除用 `resolved_*` 充当持久基线的逻辑。
6. `RawDecodeOp` 删除 `pre_populated_ctx_` 和 `latest_runtime_context_`；`GetParams()` 必须包含后续渲染所需的固有 RAW 参数。
7. `LensCalibOp` 保留目标图片的镜头固有参数，用户开关和强度作为可编辑键。
8. 导入结束前执行一次 `SetGlobalParams` 和 `SetExecutionStages`。
9. 保存完整 pipeline JSON，然后才把导入任务标为 metadata success。
10. 缩略图、首帧预览和导出必须直接使用这份 pipeline JSON，不再等待第一次 RAW 渲染补数据。

完成标准：刚完成导入、尚未发生任何渲染时，重新加载 pipeline 后即可得到正确的 as-shot 色温、CCM 和镜头参数。

### 4.5 明确持久参数与单次请求参数

逐项执行：

1. 每个 operator 的 `GetParams()` 只返回持久参数。
2. `RESIZE` 继续不进入 `ExportStageParams()`。
3. RAW decode 的单次 `decode_res` 不进入持久 JSON。
4. 用户裁切参数保留在 `CROP_ROTATE`；导出裁切和 resize 根据当前请求计算有效值。
5. `PipelineTask::SetExecutorRenderParams()` 在 `Apply()` 前安装本次有效参数。
6. 在同一个 render lock 内保存被覆盖的单次参数，并在成功、取消和异常出口恢复。
7. 删除 `PipelineScheduler` 中逐任务 `InjectRawMetadata()`。
8. 删除仅为逐帧 metadata 注入服务的 dirty 标记和缓存失效逻辑。

### 4.6 用默认参数计算版本状态

删除：

- `MakeEmptyCompleteAdjustmentSnapshot()`；
- `RootSnapshotFromMaterialized()`；
- 接受 `root_snapshot` 的 `SnapshotAtHead()`；
- `HistoryWorkingState::root_snapshot`；
- Editor 层 22 字段完整快照到 pipeline JSON 的重建路径。

新增或改写为一个直接算法，不新增状态对象：

```text
ApplyVersion(head):
  prior = ExportPipelineParams()
  for each editable operator:
    SetOperator(default editable params, preserve image-local params)
  for commit in FirstParentChain(head):
    SetOperator(commit.after)
  SetGlobalParams()
  SetExecutionStages() once
  on failure: ImportPipelineParams(prior)
```

具体要求：

1. 默认参数来自 `default_pipeline_params.hpp`，不得复制另一份默认值表。
2. `raw_decode`、`color_temp`、`lens_calib` 的 reset 只重置可编辑键，不删除图片固有键。
3. undo / redo 优先直接应用 commit 的 before / after，不重放完整 pipeline。
4. version checkout 才使用默认参数加提交链重算。
5. checkout 成功后一次性持久化 active Version 和当前 pipeline JSON。
6. checkout 任一步失败，live pipeline、active Version 和数据库必须全部保持原状态。

### 4.7 重写 paste 和 merge

逐项执行：

1. transfer package 只包含用户编辑字段，不包含图片固有字段和单次渲染字段。
2. 删除 `PasteLiveRootRelativeVersion` 命名和实现。
3. paste 以目标图片当前 pipeline 为操作对象，先保存 prior JSON 供失败回滚。
4. 需要生成“导入版本状态”时，先对目标图片的可编辑字段应用默认参数，再重放 package 中的编辑提交。
5. 目标图片自己的 CCM、as-shot CCT/Tint、相机和镜头参数始终保留。
6. merge 的共同基线由默认参数加共同祖先提交计算，不读取 root snapshot。
7. 冲突只比较用户编辑字段。
8. 所有输入和冲突选择先验证完整，再修改 live pipeline。
9. 任一 `SetOperator`、WAL 或数据库写入失败，恢复 prior pipeline JSON 和 prior active Version。

### 4.8 从 UI 和 Editor Session 删除所有 generation

修改：

- `app/editor_session_render_controller.*`
- `app/editor_render_coordinator.*`
- `app/editor_session_navigation_controller.cpp`
- `app/editor_session_service.cpp`
- `include/app/editor_render_intent.hpp`
- `include/app/editor_session_ports.hpp`
- `ui/alcedo_main/editor_dialog/render/editor_render_coordinator.*`
- `ui/alcedo_main/editor_dialog/session/editor_adjustment_session.*`
- `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp`

逐项执行：

1. 删除 `content_generation_`、`view_generation_`、UI `preview_generation_`。
2. 删除 `AdvanceContentGeneration()`、`AdvanceViewGeneration()`、`ResetForNewImage()` 中所有 generation 操作。
3. 删除 `SetActiveGenerations()`。
4. 从 `EditorRenderIntent`、scheduler port request、诊断结构和 QML 暴露值中删除 `render_generation`、`view_generation`、`preview_generation`。
5. Session 只表达“需要渲染什么”，不分配序号，也不判断结果是否过期。
6. 导航、undo、redo、checkout、resize 和面板修改统一提交普通 render request。
7. 删除 UI 回调中的 generation 比较和“是否展示该帧”的分支。

完成标准：

```text
rg "content_generation|view_generation|preview_generation|SetActiveGenerations|AdvanceContentGeneration|AdvanceViewGeneration" \
  alcedo_studio/src/app alcedo_studio/src/include/app \
  alcedo_studio/src/ui/alcedo_main alcedo_studio/src/include/ui/alcedo_main
```

没有 Editor / Session 渲染 generation 结果。与搜索等无关模块的独立请求计数不在本次范围内。

### 4.9 由 PipelineScheduler 独占渲染请求序号

修改：

- `renderer/pipeline_scheduler.*`
- `include/renderer/pipeline_task.hpp`
- `ui/editor_rhi/direct_frame_sink.*`
- `ui/editor_rhi/direct_present_queue.*`
- `include/ui/edit_viewer/frame_sink.hpp`

逐项执行：

1. `PipelineScheduler::ScheduleTask()` 分配单调递增的 `request_id`。
2. `PipelineTask` 在整个 decode、Apply 和提交阶段持有同一个不可变 `request_id`。
3. scheduler 为实际输出目标记录 `latest_submitted_request_id`。
4. 开始昂贵处理前，如果任务序号已经落后则取消。
5. `Apply()` 完成后、写入 sink 前再次比较；旧帧直接释放，不调用 UI callback。
6. sink 记录 `latest_accepted_request_id`，拒绝任何更小的提交。
7. `IFrameSink` 的完成提交 API 同时接收像素、request ID、frame role 和展示模式。
8. 删除共享的 `SetNextFramePreviewMetadata()` / `SetNextFramePresentationMode()` 单槽状态，避免旧像素读取新请求 metadata。
9. `DirectPresentQueue` 只管理已通过 request ID 检查的 GPU/CPU 资源，不再决定帧新旧。
10. UI 不接收被丢弃任务的成功结果，也不做二次过滤。

### 4.10 删除 FramePresentationBroker

逐项执行：

1. 删除 `ui/editor_rhi/frame_presentation_broker.cpp`。
2. 删除 `include/ui/editor_rhi/frame_presentation_broker.hpp`。
3. 从 `ui/editor_rhi/CMakeLists.txt` 删除两项。
4. 删除 `editor_rhi_contracts_test.cpp` 中全部 `FramePresentationBrokerTest`。
5. 不把它的 lease、target generation 或 accepted-generation 模型迁移到生产路径。
6. 需要的唯一旧帧规则直接实现于 `PipelineScheduler` 和实际 sink 的 request ID 比较。

### 4.11 删除 Session 的完整 pipeline 快照所有权

逐项执行：

1. `EditorSessionEditController` 不再维护累计 `adjustment_snapshot_`。
2. slider 输入只发送当前字段 patch。
3. settled edit 由 history 写 commit 并对 pipeline 执行同一个 patch。
4. undo / redo / checkout 完成后，面板从当前 pipeline operator 参数刷新。
5. 初始打开不把完整 snapshot 塞进 render intent。
6. render task 不再重放 22 字段完整 snapshot。
7. 删除 `LooksLikeCompleteEditorSnapshot()` 和硬编码字段数量 `22`。
8. 删除因为完整 snapshot 重放而产生的 replace / deep-merge 双路径。

## 5. 必须增加的测试

### 5.1 导入和持久参数

1. 导入 RAW 后、任何渲染前，pipeline JSON 已包含曝光 `1.5`、饱和度 `30`、CCM、as-shot CCT/Tint 和镜头型号。
2. 保存并重新加载后，上述参数完全一致。
3. 首帧渲染前后，持久 pipeline JSON 不发生变化。
4. 多次渲染不会重新执行 RAW metadata 注入或 Lensfun 匹配。

### 5.2 History

1. 无提交版本由默认 operator 参数计算。
2. checkout 单提交、多提交和 merge commit 后得到准确参数。
3. undo / redo 只应用 before / after。
4. checkout 失败恢复 prior pipeline 和 prior active Version。
5. 历史恢复不读取 live executor 来填补任何字段。

### 5.3 Paste / merge

1. paste 不复制源图片的相机、CCM、as-shot 白平衡和镜头信息。
2. paste 能在目标默认参数上重建源编辑结果。
3. merge 共同基线由默认参数和共同祖先提交计算。
4. 冲突验证失败时 live pipeline 零变化。

### 5.4 渲染请求序号

1. request 2 先完成后，request 1 即使晚到也不能进入 sink。
2. request 1 已进入 Apply、request 2 随后提交时，request 1 的像素不能被标记为 request 2。
3. 取消任务不会遗留 resize、ROI 或 decode 参数。
4. 切换图片后，旧图片任务不能向当前 sink 提交。
5. UI 和 Session 测试不再设置、推进或比较任何 generation。

### 5.5 删除项

1. 构建清单中不存在 `FramePresentationBroker`。
2. 第一方代码不存在 `SeedImageLocalBaselineFromLivePipeline`。
3. 编辑历史代码不存在 `PipelineRoot`、`root_snapshot` 和 root pipeline JSON。
4. Editor / Session 代码不存在内容、视图或预览 generation。

## 6. 执行顺序

按以下顺序提交，禁止继续给旧体系增加补丁：

1. 删除 Seed 补丁。
2. 让 import 直接写全 operator 固有参数。
3. 删除 scheduler 的逐帧 RAW metadata 注入。
4. 让版本状态可由默认参数加提交链稳定计算。
5. 重写 checkout、undo、redo、paste、merge，并保证失败回滚。
6. 完成数据库数据迁移，删除 root schema 和 root API。
7. 从 Session 和 UI 删除所有 generation。
8. 把 request ID 生成、取消和旧帧丢弃收口到 PipelineScheduler 与实际 sink。
9. 删除 `FramePresentationBroker`。
10. 删除完整 Editor snapshot 重放和 22 字段兼容代码。
11. 运行全部 pipeline、history、editor session、thumbnail、export 和 editor RHI 测试。

## 7. 最终验收标准

全部满足后才算完成：

- 一张图片只有一份可运行、可序列化的 pipeline operator 状态。
- 图片固有参数在导入时写入，此后渲染不再补写。
- 默认状态能够计算，不存在持久 root。
- checkout、merge 和 paste 不依赖 root snapshot。
- UI 和 Session 不维护任何渲染 generation。
- 渲染请求序号完全由渲染侧维护。
- 旧帧在进入 UI 前被丢弃。
- `FramePresentationBroker` 已删除。
- 没有 Seed、live baseline repair 或第一次渲染回填路径。
- resize、ROI、decode resolution 不进入持久 pipeline JSON。
- 任一 history 或持久化失败都不会留下半修改的 live pipeline。
