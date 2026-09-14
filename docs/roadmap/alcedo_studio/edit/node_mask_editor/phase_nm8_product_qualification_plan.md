# Phase NM8 — Preview Performance, Pass Scheduling and Product Qualification

Date: 2026-09-12

Status: NM8.1 complete on 2026-09-12 (low-overhead CPU/E2E logging).
NM8.2 CUDA measurement is complete on 2026-09-13: native pass GPU timestamps,
Interactive 2560 slider DAG traces, native-sensor slider DAG traces on the same
Bayer RAW, 8 Color Grade skip paths, and a ~10 s product `submitWrite`→
`frameSwapped` last-Exposure trajectory. OpenCL and Metal GPU timing remain
pending.
NM8.2R scheduling/presentation rework passed complete-UI qualification on
2026-09-14 under default VSync (session-owner thread + render-thread
Ready-frame consume).
NM8.3–NM8.6 planned.
NM7 已由用户确认完成；其历史测试记录保留在原方案中，本文件不补造执行证据。
2026-09-12 的首轮工作范围是 NM8.1–NM8.2：建立低开销测量和日志，采集当前实现的数据。
NM8.3–NM8.6 定义完整优化和最终资格验证，按依赖顺序执行。

Parent: [Node-aware Pipeline Editing and Mask Creation](../node_mask_editor_master_plan.md),
Sections 17.5, 21.9, 23, 24, and 26.

Prerequisites: 单一 live document、串行输入消费、多 Grade、Radial / Linear Gradient、
History / Version / Paste 和当前项目格式。所有执行使用 `ALCEDO_ENABLE_BRUSH_MASK=OFF`。
历史 NM6/NM7 记录中的未补证事项在最终产品验证中明确记录，不据此重新引入 Brush。

## 1. 本次锁定的架构决定

2026-09-12 用户确认以下目标，取代 NM6 中逐 Color Grade 保留 RGBA 结果的策略，
也取代 NM8 前期分析中的选择性保留 Grade 前缀结果建议：

1. 每个 Color Grade 使用 `Basic Tone → Color → Local Tone → Mix` 顺序。
2. Basic Tone 和 Color 融合成一个逐像素 pass。以现有融合 pass 为调度单元，
   不拆回逐算子 kernel，也不跨节点移动 Local Tone 或 Mix。
3. **取消 Color Grade 之间的跨帧图像结果缓存。** 不保留节点 RGBA 输出、
   Basic Tone / Color 中间图或 Mix 后 RGBA 图，不新增活跃节点前缀缓存。
4. **节点间共用一对 RGBA32F ping-pong 工作图。** 这对图由现有 render workspace
   管理，继续供后续兼容的 DRT/Post ping-pong 任务使用，不按 NodeId 配置各自一对图。
5. **Grade 内跨帧图像缓存只用于 LLF 图。** 复用现有 source.0 / result.0 表示；
   更高层金字塔、remap 和 collapse 工作图仍是算法临时资源。
6. Develop、Geometry 等关键阶段继续遵守既定复用规则。LUT、GPU pipeline state、
   参数 arena、静态执行计划属于各自 owner 的资源，不作为待删除的 Grade 图像缓存。
7. 保留每个节点完整调色后的 Mix：`output = input + weight * (adjusted - input)`。
   weight 使用既定 Grade Mix、Mask opacity、invert 和 Union 语义，不能改成缩放各算子参数。
8. 同一 live pipeline 的参数应用、渲染和安全完成继续串行；不通过新图副本或新 executor 并发。
9. 性能优化保持 FP32、既定 decode/渲染尺寸、LLF 算法、Mask 数值规则和选定后端。
   CUDA、OpenCL、Metal 共用编排和有效性决策，各后端负责原生执行。

本方案不承诺整个 DAG 只启动两个 kernel。Basic Tone + Color 是一个 pass；
LLF 准备包含多个原生 dispatch。LLF 最后的逐像素应用可以和 Mix 融合。
节点顺序、每节点 pass 顺序、GPU dispatch 数量分别记录。

## 2. 创建方案时的源码事实

调查基于工作树 HEAD `a30d8250`。以下是代码阅读结果，不是性能测量或测试通过记录。

| 位置 | 当前行为 | NM8 改动方向 |
| --- | --- | --- |
| [调整顺序](../../../../../alcedo_studio/src/edit/graph/adjustment_ownership.cpp) | Shadows/Highlights 位于 Curve/HLS 等颜色处理之前 | 固定 Basic Tone → Color → Local Tone → Mix；更新独立像素预期 |
| [GraphCompiler](../../../../../alcedo_studio/src/edit/runtime/graph_compiler.cpp) | 按相邻算法类别建立 stage，每个 Grade 生成逻辑 pass | 编译出明确的融合逐像素阶段、LLF、Mix 和读写要求 |
| [静态计划缓存](../../../../../alcedo_studio/src/edit/runtime/static_execution_plan_cache.cpp) | 查询时生成 topology key；命中按值返回 ExecutionPlan | 先测 key 构造、查询、复制及分配成本，再通过 owner 的安全读取接口优化 |
| [PlanExecutor](../../../../../alcedo_studio/src/include/edit/runtime/plan_executor.hpp) | 按 Grade 输出检查结果有效性并记录待发布输出 | 删除 Grade RGBA 的跨帧查找、发布和跳过路径；保留逻辑依赖版本 |
| [GradeExecutor](../../../../../alcedo_studio/src/include/edit/runtime/grade_executor.hpp) | 每节点安排 output/ping/pong，最后可能独立 Mix | 接入 workspace 工作图对，保住节点输入，取消按节点结果占位 |
| [Grade schedule](../../../../../alcedo_studio/src/edit/runtime/grade_schedule.cpp) | 合并相邻逐像素组；统计写图次数和目的位置 | 区分逻辑阶段、物理 pass、原生 dispatch；显式表示同像素读写要求 |
| [LocalToneExecutor](../../../../../alcedo_studio/src/include/edit/runtime/local_tone_executor.hpp) | 共用 LLF source/result 有效性、金字塔和应用流程 | 以 Color 之后的真实输入为依赖；将 LLF 准备与最终应用分开编排 |
| [结果保留策略](../../../../../alcedo_studio/src/include/edit/runtime/result_persistence.hpp) | Interactive/Detail 允许每个 graph value 保留；QualityBase 仅保留 sensor develop | 给 Grade 图像明确排除规则；保持不同 frame role 的既定边界 |
| [E2E 计时](../../../../../alcedo_studio/src/utils/diagnostics/render_e2e_timing.cpp) | 请求提交到纹理导入；默认 stdout；关闭输出仍采集并加锁 | 接入输入消费和 Qt 呈现边界，分层开关、结构化记录、异步输出 |
| [应用日志](../../../../../alcedo_studio/src/utils/diagnostics/app_logging.cpp) | Qt category，调用线程格式化、持锁写文件，批量 flush | 复用 diagnostics，性能事件先有界记录，后台汇总和写入 |
| [GPU pass stats](../../../../../alcedo_studio/src/include/edit/runtime/gpu_node_pass_stats.hpp) | 执行、跳过、上传及缓存计数，没有逐节点 GPU 时间 | 增加按帧关联的节点/pass 时间和资源计数，保留未执行原因 |

输入和呈现链路的改动位置：
[pending input](../../../../../alcedo_studio/src/app/editor_pending_input.cpp)、
[serial admission](../../../../../alcedo_studio/src/app/editor_serial_frame_admission.cpp)、
[render coordinator](../../../../../alcedo_studio/src/app/editor_render_coordinator.cpp)、
[pipeline scheduler](../../../../../alcedo_studio/src/renderer/pipeline_scheduler.cpp)、
[frame sink](../../../../../alcedo_studio/src/ui/editor_rhi/direct_frame_sink.cpp)、
[present queue](../../../../../alcedo_studio/src/ui/editor_rhi/direct_present_queue.cpp)、
[viewport renderer](../../../../../alcedo_studio/src/ui/editor_rhi/editor_viewport_renderer.cpp)、
[viewport item](../../../../../alcedo_studio/src/ui/editor_rhi/editor_viewport_item.cpp)。

## 3. 测量边界与数据

### 3.1 一帧的时间线

```text
输入被 app 接收，取得输入序号
  → owner 开始消费批次
  → 参数应用 / 依赖失效处理
  → render request 提交 / 排队 / 获准执行
  → 会话准备 / static plan 查找或编译 / 参数上传 / CPU 编码
  → GPU 执行 / 必要完成等待 / frame sink
  → GUI 唤醒 / scene graph / 纹理导入 / 合成提交
  → 包含该 request 的 Qt frame 排入呈现
```

必须保留现有 submit → import 指标，新增指标不能偷偷替换它的终点。
主 E2E 使用 app 接收输入 → 对应 Qt frame 排入呈现；它不等于显示器实际扫描完成。
`frameSwapped` 表示 frame 已排入呈现，`afterFrameEnd` 表示 frame 已提交，
不能称为物理显示完成。关联 window/frame 序号和实际被采用的 request，不能把下一个
无关 Qt frame 记给等待中的全部请求。[Qt QQuickWindow](https://doc.qt.io/qt-6.9/qquickwindow.html)

每批输入记录最早待消费和最新已消费输入的时间及序号；报告最早输入等待与最新值响应，
不以合并后重新打时间戳隐藏积压。没有用户输入的首图、Quality、ROI 等请求从 request
提交计时，并明确 `input_to_present` 不适用。采样时以整条请求链为单位选择。

| 指标 | 定义 / 用途 |
| --- | --- |
| Input E2E | 最新已消费输入被接收 → 对应 frame 排入呈现；另记最早输入等待 |
| Producer cycle | owner 开始消费 → 本轮资源及输出安全完成；已有 16 ms 目标只用于这一范围 |
| Request age | render request 提交 → import，以及提交 → Qt 呈现提交，分别报告 |
| CPU 阶段 | apply、invalidation、plan key/lookup/compile、allocation、encode、submit、等待；嵌套区间不重复相加 |
| GPU 节点 / pass | 完成后读取的设备时间；CPU encode 时间单列；父区间和子区间不重复相加 |
| GPU 工作 | dispatch、上传/拷贝字节、LLF source/result 重建或复用、Mask evaluate/union/mix |
| 呈现间隔 | 同 role 的不同新帧排入呈现的时间间隔；重复绘制旧图不冒充新预览帧 |
| 资源 | 当前及峰值字节、分配次数、工作图数量、LLF 图、关键阶段缓存、在途呈现 lease 分项 |
| 终态 | presented、coalesced、cancelled、failed、stale、dropped 及原因；计数完整 |

帧关联信息只保存必要值：session/image 身份、request/input 序号、document revision、
plan generation、backend、frame role、实际尺寸、NodeId、pass ID、阶段类型、时间及计数。
不复制 document、参数容器或图像，不在热路径序列化整图。重复文本通过受控的 ID 映射处理。
CPU 使用单调时钟；GPU 域内计算时长。跨域时间线只有完成校准后才能对齐。

### 3.2 逻辑节点和物理 pass 的对应

每个实际 pass 都有 ID、owner 节点、阶段类型和所属 request。LLF 的准备/应用分别可见；
Mask 工作单列并关联 owner。节点同时报告 Grade 本体和含 Mask 准备的总范围，避免口径混用。
缓存命中、禁用或空操作记录明确状态，不把未执行误报为一次耗时为零的 GPU 计算。

Basic Tone + Color 融合后只测该物理 pass，不把融合后的时间平均分配给内部算子。
若后续将多节点融合成一个 kernel，必须记录参与节点列表，GPU 时间归融合 pass；
逐节点诊断实验另行标识，不能宣称是融合运行中的独立实测时间。
本次主实现保留逐 Grade 执行边界，不以跨 Grade kernel 融合作为双 buffer 的前提。

### 3.3 日志模式和低开销要求

| 模式 | 行为 |
| --- | --- |
| Off | 入口快速退出，不创建计时资源、不构造字符串、不访问计时容器或锁 |
| Summary | 可配置整帧采样，后台周期汇总 P50/P95/P99、最大值、帧数、超预算和终态计数 |
| Detail | 限定时间/请求范围，记录每帧每节点每 pass 的 CPU/GPU 数值及资源数据 |
| Hardware capture | 在 Detail 的 ID 映射上增加厂商工具标记；独立采集硬件计数器 |

性能事件通过预分配、有界存储交给后台；热路径不格式化 JSON、不直接写文件、不等待
日志消费者。队列满时丢弃诊断事件并计数，不能阻塞 pipeline；有缺失的请求标记为不完整，
不进入完整 E2E 分位数。保留采样率、样本数和丢失计数。关闭和进程退出显式排空并收尾。
普通错误日志继续使用现有诊断模块；不为本阶段替换全应用日志库。

GPU events/sample buffers 预建并按完成点回收。采集不能在每个 pass 后新增
`cudaDeviceSynchronize`、`cudaEventSynchronize`、`waitUntilCompleted` 或 `clFinish`。
设备完成状态由现有 owner/队列提供；不复用尚未完成的计时槽位。
计时能力不足记录 unavailable 及原因，不用 CPU 提交时间替代 GPU 时间。

## 4. 双工作图、Mix 和 LLF 的执行方式

### 4.1 两张图如何保留原始输入

一般 Grade 执行如下；S 是该节点完整输入，W 是另一张可写工作图：

```text
S 保持不变
  → Basic Tone + Color：读取 S，写入 W
  → LLF 准备：读取 W，生成或复用独立的 LLF source/result 单通道图
  → 最终逐像素 pass：
       读取 S[p]、W[p]、LLF 图和有效 Mask
       W[p] = Mix(S[p], ApplyLocalTone(W[p], LLF), weight[p])
  → W 成为下一节点输入，另一张工作图成为目的图
```

最终 pass 在每线程写入前读完同一像素的 W；它不得从正在被覆盖的 W 读取邻居像素。
所有 LLF 邻域读取必须在此之前完成，最终应用需要的采样来自独立 LLF 图。
CUDA 指针别名约束、Metal texture access 和 OpenCL 原生读写方式必须分别验证；
不能仅因公式逐像素就假定当前 shader 允许输入输出重合。
若某后端现有绑定不支持，修改其原生资源访问和应用入口，并通过该后端验证；
不得暗中增加第三张每节点 RGBA 图或恢复 Grade 缓存来绕过。

LLF 关闭时，直接将 Mix 融入 Basic Tone + Color 的写出，S → W 一次逐像素 pass。
无有效调整、Grade 禁用或 Mix 为零时，经严格判定后传递输入绑定，不进行图像拷贝。
Mask Mix 仍使用既定数值和颜色空间，不能通过省略色彩转换减少工作。

第一个 Grade 直接读取只读的 Develop/Camera Color 有效输出，写入工作图之一；
Mix 保留该输入引用到最后读取完成，不为进入工作图对做整图复制。
后续节点使用两张工作图交替，不覆盖 Develop/Geometry 的已发布结果。

### 4.2 workspace 拥有工作图，逻辑图仍拥有依赖

改动现有 [workspace](../../../../../alcedo_studio/src/include/edit/runtime/basic_render_workspace.hpp)、
[GraphImageCache](../../../../../alcedo_studio/src/include/edit/runtime/graph_image_cache.hpp)、
[texture pool](../../../../../alcedo_studio/src/include/edit/runtime/texture_pool.hpp) 的受控操作，
由 workspace 取得工作图 lease 并安排读写。ExecutionPlan 只描述数据依赖和读写需求，
不拥有 GPU 资源，也不持有可变 document 镜像。

取消 Grade scene_output 的持久查找/发布，但仍保留 GraphValueId、依赖 revision 和错误定位。
必要的当前节点绑定是帧内借用，后续覆盖前撤销旧绑定，不留下貌似有效的历史节点输出。
帧内容发生变化并进入 Grade 区域时，从关键阶段的有效输入顺序执行所需 Grade；
不再承诺修改末尾 Grade 可以跳过前面的 Grade 计算。纯 pan/selection 等仍按既定规则复用最终帧。

工作图对继续交给 [DRT/Post](../../../../../alcedo_studio/src/include/edit/runtime/drt_post_executor.hpp)
及后续兼容任务，不重新分配按节点命名的 ping/pong。不同尺寸、格式、读写用途不兼容时，
由资源 owner 在已完成读取的边界重新取得正确资源；不缩小图像或改变格式满足复用。
[邻域算法](../../../../../alcedo_studio/src/include/edit/runtime/neighbor_executor.hpp) 必须保留
其数学上必需的 scratch，不将这些存储伪装为持久 Grade 输出。

这对图是同一 render workspace 的主 RGBA 工作集，并非全应用所有内存的上限。
LLF 单通道图、邻域 scratch、关键阶段有效结果和最终呈现资源分别计量。
最后输出仍被 GPU、Qt 或其他 reader 持有时，不能开始覆盖它；沿用现有提交、完成和
lease 释放机制。跨帧占用由已有呈现槽位和在途数量约束，不能随 Grade 数量增长。
不通过每节点同步换取复用，也不新增全局显存预算调度器。

### 4.3 LLF 缓存与 Mask 工作

LLF 保留每个 owner 的当前有效 source.0/result.0；source 对应 **Basic Tone + Color 之后**
的真实输入。输入身份、Develop/Geometry、上游 Grade 的完整输出语义、本节点 Basic Tone /
Color、算法版本、参考空间或表示条件变化都会使相应 source/result 失效。
Shadows/Highlights 变化可保留未变的 source，但 result 必须重建。
只改本节点最终 Mix 或本节点解析 Mask 不改变该节点 LLF source/result；
它会改变后续 Grade 的输入，后续 LLF 必须相应失效。

依赖版本与工作图地址分离。同一地址被下一节点覆盖，不意味着 LLF source 仍有效；
重算到另一张工作图也不等于内容必然变化。使用 owner 维护的语义版本，不逐帧像素哈希。
取消、失败或错误 submission 不发布新的 LLF 有效性。

保留既定 frame role 策略：Interactive/Detail 的 LLF 可按原有表示条件复用，
QualityBase 继续遵守 Develop 之后不写入会话持久结果的规则；不因“只缓存 LLF”
自动授权 Quality 覆盖 Interactive 的参考图。高层金字塔始终是算法临时存储。

Radial/Linear Gradient 的参数仍由 Mask owner 管理。Grade 不持有跨帧 R8/Union/Mix
图像结果；既有独立 Mask evaluator 输出只作为帧内临时资源，按最后读取释放。
将解析 coverage/Union 融入 Mix 属于 NM8.5 的候选优化，必须保持既定量化与 Union 数值；
本阶段不把修改 Mask 算法或量化精度作为性能手段。

## 5. 子阶段与完成条件

| 阶段 | 内容 | 依赖 | 初始状态 |
| --- | --- | --- | --- |
| NM8.1 | 低开销日志、输入到呈现时间线、CPU 分段 | 当前产品路径 | complete 2026-09-12 |
| NM8.2 | 节点/pass 原生 GPU 计时、当前实现基线及硬件采集 | NM8.1 | CUDA complete 2026-09-13 (2560 slider DAG, native-sensor slider DAG, felt present); OpenCL/Metal pending |
| NM8.3 | 新顺序、融合 pass 描述、算法版本和画面预期 | NM8.2 当前后端基线 | planned |
| NM8.4 | 共享工作图、取消 Grade 缓存、LLF/Mix 与下游复用 | NM8.3 | planned |
| NM8.5 | 根据 CUDA/Metal 数据优化热点和整帧开销 | NM8.4 | planned |
| NM8.6 | 三后端、真实 RAW、交互和安装包最终验证 | NM8.1–NM8.5 | planned |

### NM8.1 — 低开销性能日志与完整时间线

**工作：** 扩展第 2 节 diagnostics、输入/serial admission/coordinator/sink/viewport 路径；
用第 3 节口径关联 input、request 和 Qt frame，拆出 owner 消费、plan、CPU 编码和等待。
把 E2E stdout 迁到结构化日志；提供 Off/Summary/Detail、有界事件存储和后台写出。
产品启动即打开 Detail 采集，不读模式环境变量；测试通过 `SetMode(Off)` /
`ResetForTesting` 关闭。可选 `ALCEDO_PREVIEW_PERF_LOG` 指定文件，缺省写在应用
日志目录 `alcedo_preview_perf_<log-basename>.log`。文件行使用应用日志格式，只写
时长毫秒（e2e / input / apply / encode / wait），不写单调时钟绝对值；后台每秒
写一行窗口汇总，不按帧落盘。Windows 上 PreviewPerformance 为共享库，避免 CUDA
DLL 与主进程各持一份计时状态。
额外落地：删除产品预览路径上的 legacy `[RENDER_E2E]` / `[GPU_POOL]` / RAW `[LOG]` /
fused-pipeline FPS 打印；Develop 记录 decode 参数；每个 pass 记录子阶段 CPU 时间
（LLF / Mix / Linearize / Demosaic 等）；显存只在请求结束写一份汇总快照。
当前执行顺序、缓存、分辨率和 GPU 调度保持原样，以便取得改动前证据。
移除仅用于旧 profile 打印的 `stream.waitForCompletion()`，不增加新的逐 pass 设备等待。

**主链：** 输入接收 → serial owner → coordinator → scheduler → sink → Qt frame → 后台性能日志。
**失败链：** 请求合并/取消/失败/过期 → 记录终态 → 回收计时状态；日志满 → 记录丢失数量，渲染继续。

**验证：** `DisabledTimingDoesNotAllocateOrQueueEvents`、
`InitializeTurnsDetailLoggingOn`、
`StructuredLogWritesDurationMillisecondsInAppLogFormat`、
`WindowedLogWritesOneLineForMultiplePresentedFrames`、
`CoalescedInputsRetainFirstAndLatestAcceptedTimes`、
`PresentedFrameTimingMatchesConsumedRequest`、
`CancelledAndFailedRequestsReleaseTimingEntries`、
`FullDiagnosticQueueDoesNotBlockRenderOwner`、
`BackgroundWriterProducesCompleteStructuredRecords`。
使用真实队列/日志写入及可控制的时间输入验证顺序；不以 sleep 长短断言异步正确性。

**完成条件：** 文件日志可以还原一个 Interactive 请求及其呈现边界；关闭采集可快速退出；
日志事件与渲染失败分开处理；原有 app logging 测试通过；尚未取得的 GPU 字段明确不可用。

##### Phase NM8.1 completion record (2026-09-12)

**Status:** complete — Detail preview timing on at process start, input-to-present correlation,
CPU pass/sub-stage intervals, Develop decode parameters, aggregated GPU resource snapshot.
Native GPU durations remain unavailable.

**Primary success call chain:**

```text
AdmitFieldChange (first_accepted_ns / latest_accepted_ns)
  -> EditorSessionService::ConsumeTakenSequence (apply duration)
  -> EditorRenderCoordinator::Submit / Schedule
  -> PreviewPerformance::NoteSubmit / NoteInputTimes / NoteScheduled
  -> Renderer::Render (plan key/lookup/compile, encode, Develop decode params)
  -> PlanExecutor / GradeExecutor / LocalToneExecutor (pass + sub-stage CPU ns)
  -> workspace.CaptureResourceSnapshot
  -> DirectFrameSink (producer ready / present wake)
  -> EditorViewportRenderer::render (Qt frame of the imported request only)
  -> NoteDisplayed -> bounded queue -> background writer
```

**Primary failure call chain:**

```text
replaced / cancelled / failed / stale / dropped
  -> NoteTerminal
  -> pending sample released
  -> render owner continues

queue full
  -> events_lost++
  -> record not queued
  -> pending sample still released
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DisabledTimingDoesNotAllocateOrQueueEvents` | `PreviewPerformanceTest` | PASS |
| `InitializeTurnsDetailLoggingOn` | `PreviewPerformanceTest` | PASS |
| `StructuredLogWritesDurationMillisecondsInAppLogFormat` | `PreviewPerformanceTest` | PASS |
| `WindowedLogWritesOneLineForMultiplePresentedFrames` | `PreviewPerformanceTest` | PASS |
| `CoalescedInputsRetainFirstAndLatestAcceptedTimes` | `PreviewPerformanceTest` | PASS |
| `PresentedFrameTimingMatchesConsumedRequest` | `PreviewPerformanceTest` | PASS |
| `CancelledAndFailedRequestsReleaseTimingEntries` | `PreviewPerformanceTest` | PASS |
| `FullDiagnosticQueueDoesNotBlockRenderOwner` | `PreviewPerformanceTest` | PASS |
| `BackgroundWriterProducesCompleteStructuredRecords` | `PreviewPerformanceTest` | PASS |
| `DevelopPassRecordIncludesDecodeParameters` | `PreviewPerformanceTest` | PASS |
| `GradePassRecordsLlfAndMixSubStages` | `PreviewPerformanceTest` | PASS |
| `ResourceSnapshotReportsAggregatedPoolTotals` | `PreviewPerformanceTest` | PASS |
| `DefaultLoggingWritesNoPerFramePresentationInfo` | `EditorAppLoggingTest` | PASS |
| `EnablingPresentDebugCategoryRestoresPerFrameDetail` | `EditorAppLoggingTest` | PASS |
| `InfoAndDebugLoggingDoesNotFlushOncePerFrame` | `EditorAppLoggingTest` | PASS |
| `WarningAndCriticalFlushImmediately` | `EditorAppLoggingTest` | PASS |
| `ResourceSnapshotReportsAggregatedTextureAndTransientTotals` | `GpuDagCudaWorkspaceTest` | PASS |
| EditorPendingInputTest (13 cases) | `EditorPendingInputTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PreviewPerformanceTest --target EditorAppLoggingTest --target EditRuntime --target EditRuntimeCuda
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPendingInputTest --target EditorSessionService --target EditRuntimeOpenCl --target GpuDagCudaWorkspaceTest
ctest --test-dir build/debug -R "PreviewPerformanceTest|EditorAppLoggingTest" --output-on-failure
ctest --test-dir build/debug -R "EditorPendingInputTest\." --output-on-failure
ctest --test-dir build/debug -R "ResourceSnapshotReportsAggregated" --output-on-failure
```

Suite totals: PreviewPerformanceTest 12/12 PASS; EditorAppLoggingTest 4/4 PASS;
EditorPendingInputTest 13/13 PASS; GpuDagCudaWorkspaceTest snapshot case PASS.
`build/debug/CMakeCache.txt` has `ALCEDO_ENABLE_BRUSH_MASK:BOOL=OFF`.

**Checklist / exit condition:** all NM8.1 boxes covered by the tests above. GPU time
fields write `gpu=unavailable`. Qt frame is stamped only on the imported request.

**LOC note (grill-code-review):** `preview_performance.cpp` ~930 LOC (queue, intern,
writer, notes). Types live in `preview_performance_record.hpp`. RAII notes in
`preview_performance.hpp`. No file crossed 1000 LOC.

**Residual gaps:** CUDA native GPU events and 10 s Interactive traces are in
NM8.2 below. Metal product binary was not rebuilt on this Windows host; Metal
develop sub-stage notes are in source. Summary mode writes quantile lines from
completed presented samples; Detail writes per-request records.

### NM8.2 — 原生 GPU 计时与改动前基线

**工作：** 在共享 PlanExecutor/GradeExecutor/LocalToneExecutor 的实际执行边界发出
节点/pass 标记，三个 backend 接入自身时间戳。不能只给 GraphCompiler 的逻辑 pass 计时，
因为一个 Grade 或 LLF 仍包含多个真实 dispatch。预建计时槽，原 submission 完成后回收。

CUDA 在实际 stream 记录 events；Metal 使用 command buffer 总时间和设备支持的
counter sample 边界；OpenCL 使用启用 profiling 的 queue/event。记录能力、采样边界和
额外 barrier。硬件不支持某个细分时间时明确不可用，不拆成多个 command buffer 伪造精确度。
跨 stream/queue 只按实际依赖归因，不把 event 区间一律称为纯 kernel busy time。
[CUDA Event API](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__EVENT.html)、
[Metal GPU counters](https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers)、
[OpenCL queue/event profiling](https://registry.khronos.org/OpenCL/specs/unified/html/OpenCL_API.html)。

**主链：** 当前输入/plan → 节点/pass 标记 → 原生时间戳 → 原有完成点 → request 对应日志 → 基线报告。
**失败链：** 后端测量能力不足/计时失败 → 记录能力或错误，保留真实渲染结果状态；
GPU 执行失败 → 原生错误、无新结果发布，相关计时槽按完成状态安全回收。

**验证：** `GpuPassSamplesKeepRequestAndNodeIdentity`、
`TimingSlotsAreNotReusedBeforeSubmissionCompletes`、
`GpuTimingDoesNotAddPerPassHostWaits`、
`CachedAndDisabledPassesReportExecutionState`、
`DetailTimingPreservesRenderedPixelsWithinTolerance`。
GPU 时间以原生真实工作负载验证，不用 CPU sleep 代替；计时 API 错误路径单独验证。

**完成条件：** 第 6 节矩阵获得当前执行方式的节点/pass 时间、完整 CPU/E2E、资源数据和
采集开销；至少一份当前平台的系统时间线能与日志 request/NodeId 对应。
CUDA、Metal、OpenCL 分别记状态，缺少设备实测不能把整个三后端阶段标记 complete。
有已记录的本机基线即可继续本机开发；另一个后端优化前必须先采集它自身的原实现基线。

##### Phase NM8.2 completion record (2026-09-13)

**Status:** partial — CUDA native pass/sub-stage GPU timestamps and current-execution
Interactive DAG baseline are complete. OpenCL profiling-info and Metal command-buffer
GPU time are wired in source; they were not measured on this Windows host.

**Primary success call chain:**

```text
PlanExecutor / GradeExecutor / LocalToneExecutor execute branch
  -> GpuWorkSample<Device> (BeginGpuWorkSample)
  -> CudaGpuTimestampPool::Begin (cudaEventRecord start on CommandContext stream)
  -> native pass / sub-stage encode
  -> GpuWorkSample destructor (cudaEventRecord stop)
  -> EndRender Submit (existing disable-timing fence)
  -> WaitIdle or Present cudaStreamSynchronize
  -> CudaBackend::ResolveGpuTimestamps (cudaEventQuery + cudaEventElapsedTime)
  -> PreviewPerformance::NoteGpuDuration on the pending sample
  -> NoteDisplayed
  -> Detail window log gpu_ms= next to pass CPU times
```

**Primary failure call chain:**

```text
cudaEventElapsedTime / profiling-info failure
  -> gpu_status=Failed
  -> render result unchanged

Skipped / Aliased / Disabled pass
  -> gpu_status=Unavailable
  -> no published gpu_ns=0 as measured work

encode throw
  -> CancelRender
  -> DiscardGpuTimestamps
  -> no new published results
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `GpuDurationNoteFillsPassRecordBeforeDisplay` | `PreviewPerformanceTest` | PASS (debug) |
| `GpuPassSamplesKeepRequestAndNodeIdentity` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `TimingSlotsAreNotReusedBeforeSubmissionCompletes` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `GpuTimingDoesNotAddPerPassHostWaits` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `CachedAndDisabledPassesReportExecutionState` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `DetailTimingPreservesRenderedPixelsWithinTolerance` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `InteractiveThreeNodeGraphReportsPassGpuTimes` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `InteractiveFourNodeSecondGradeMasksReportGpuTimes` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `InteractiveMultiGradeMaskMixReportsPerNodeGpuTimes` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `TwoLutGradesReportIndependentPassGpuTimes` | `GpuDagCudaPrimaryGradeTest` | PASS (debug + win_release_test) |
| `InteractiveDagBaselinesDumpCurrentExecutionGpuTimes` | `GpuDagCudaPrimaryGradeTest` | PASS (debug 256×192; release 1920×1280 n=11 + Bayer FULL) |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PreviewPerformanceTest --target GpuDagCudaPrimaryGradeTest
ctest --test-dir build/debug -R "GpuDurationNoteFillsPassRecordBeforeDisplay|GpuPassSamplesKeepRequestAndNodeIdentity|TimingSlotsAreNotReused|GpuTimingDoesNotAddPerPassHostWaits|CachedAndDisabledPassesReportExecutionState|DetailTimingPreservesRenderedPixels|InteractiveThreeNodeGraph|InteractiveFourNodeSecondGradeMasks|InteractiveMultiGradeMaskMix|TwoLutGradesReportIndependentPassGpuTimes|InteractiveDagBaselinesDumpCurrentExecutionGpuTimes" --output-on-failure
cmd /c scripts\msvc_env.cmd --preset win_release_test -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_release_test --parallel 4 --target GpuDagCudaPrimaryGradeTest
ctest --test-dir build/release-test -R "GpuPassSamplesKeepRequestAndNodeIdentity|TimingSlotsAreNotReused|GpuTimingDoesNotAddPerPassHostWaits|CachedAndDisabledPassesReportExecutionState|DetailTimingPreservesRenderedPixels|InteractiveThreeNodeGraph|InteractiveFourNodeSecondGradeMasks|InteractiveMultiGradeMaskMix|TwoLutGradesReportIndependentPassGpuTimes" --output-on-failure
ctest --test-dir build/release-test -R "InteractiveDagBaselinesDumpCurrentExecutionGpuTimes" --output-on-failure --timeout 1200
```

Suite totals: debug 11/11 PASS; win_release_test 9/9 correctness PASS + dump PASS.
`build/debug/CMakeCache.txt` and `build/release-test/CMakeCache.txt` have
`ALCEDO_ENABLE_BRUSH_MASK:BOOL=OFF`.
Hardware: NVIDIA GeForce RTX 3080 Laptop GPU, 8192 MiB, driver 610.62, CUDA 12.8,
Windows 10.0.22635, Qt 6.9.3. Branch `feature/preview-gpu-pass-timing`,
base commit `671a0da1`.
Nsight Systems `nsys` is not on PATH (Nsight Compute `ncu.bat` is present and was
not used). The Detail pass records with `request_id` and NodeId are the correlated
timeline.

**Baseline table:** `build/tmp/preview_performance/cuda_interactive_dag_baseline_table.txt`
(P50 also in `cuda_dag_baseline_p50.txt`). DirectRgb 1920×1280 Interactive, n=11,
events_lost=0, texture 187.5 MB. Bayer FULL 3992×5992 3-node Develop UploadRaw
P50 GPU 103.1 ms (DecodeRes FULL). Top GPU costs on 1920×1280: UploadRgb ~9–11 ms,
DRT ~2.3–3.2 ms, CameraToAp1 ~2.2–2.4 ms.

**Checklist / exit condition:** CUDA required tests and the four user DAG baselines
are done. Section 6 items not in that list (8-grade, LLF on/off matrix, 10 s input
trajectory, Off vs Summary overhead) stay residual. OpenCL/Metal not complete.

**LOC note (grill-code-review):** `preview_performance.cpp` 1185 LOC (writer + notes;
already large after NM8.1). New CUDA pool ~163/.hpp ~83. `gpu_work_sample.hpp` ~53.
`cuda_preview_gpu_timing_test.cpp` ~680. No new type exceeds a second owner.

**Residual gaps:** OpenCL Interactive DAG GPU times not run (queue created with
`CL_QUEUE_PROFILING_ENABLE`; no measured table). Metal command-buffer GPUStart/End
cannot run on this Windows host; per-pass counters stay Unavailable. No Nsight
Systems timeline. The 2026-09-13 DirectRgb 1920×1280 / Bayer FULL 3992×5992 table
is not product Interactive. NM8.3 order fusion and NM8.4 shared work images were
not started.

##### NM8.2 remaining measurement (2560, felt E2E, RAW slider)

**Status:** complete on CUDA (2026-09-13). OpenCL / Metal present tables are not
on this host.

Exit conditions and evidence:

1. Interactive working size is `DecodeRes::FULL` + long-edge **2560**. Dump fails
   if `geometry.render_extent` long edge is not 2560 when the decoded long edge
   is larger. Do not change Interactive to HALF.
2. Felt E2E starts at `EditorSessionController::submitWrite` (`qml_write_ns`) and
   ends at `QQuickWindow::frameSwapped` for the Qt frame that imported that
   request. Keep submit → import as `e2e_ms`. Stamp worker start, sink submit,
   import, and `afterFrameEnd`. Do not stamp an unrelated Qt redraw.
3. Headline CFA is Bayer and X-Trans. DirectRgb is a control row. Report cold
   first Interactive and **hot slider** separately (session cache kept). Dominant
   GPU is per table, not mixed.
4. **Slider simulation is required.** Mutate operator parameters on the live
   document and produce frames. 8 Color Grades; first / mid / last; Exposure,
   Contrast, Curve/Color, Shadows/Highlights, Mix, Radial/Linear, DRT Clarity.
   This is how cache misses and scheduling stalls are caught. Static graphs with
   `ReleaseSessionResources()` between repeats are not Interactive slider data.

DAG dump: `build/tmp/preview_performance/cuda_interactive_2560_pass_table.txt`
and `cuda_interactive_native_slider_table.txt`.
Present dump: `build/tmp/preview_performance/cuda_interactive_2560_present_table.txt`
and `cuda_interactive_2560_present_frames.csv`. LLF-enabled present dump:
`cuda_interactive_2560_present_llf_table.txt` and
`cuda_interactive_2560_present_llf_frames.csv`.
Tests: `PreviewPerformanceTest` import/present correlation; `GpuDagCudaPrimaryGradeTest`
`EightGrade*` skip assertions,
`Interactive2560SliderBaselinesDumpCurrentExecutionGpuTimes`, and
`InteractiveNativeSliderBaselinesDumpCurrentExecutionGpuTimes`;
`EditorPreviewPresentTrajectoryTest.SubmitWriteHotExposureCompletesAtFrameSwapped`,
`Interactive2560PresentTrajectoryDumpSubmitWriteToFrameSwapped`, and
`Interactive2560PresentTrajectoryDumpLastGradeLlfEnabled`.

**Shared RAW for slider families (win_release_test, 2026-09-13).** Hardware:
NVIDIA GeForce RTX 3080 Laptop GPU, 8192 MiB, driver 610.62, CUDA 12.8. Brush
mask cache: `ALCEDO_ENABLE_BRUSH_MASK=OFF`. Bayer file:
`Tag @ryanbreitkreutz - Free files from @signatureeditscoDSC00830.ARW`. Develop
plane **4600×3064**, `DecodeRes::FULL`, `downsample_passes=0`. Session cache
kept on every hot slider frame. DirectRgb 1920×1280 with
`ReleaseSessionResources()` between repeats is a control row only. It is not
an Interactive slider.

Times in milliseconds.

**Family A — heavy 8-grade DAG slider** (`PopulateHeavyGrade` on each Color
Grade, includes LLF). Dump:
`build/tmp/preview_performance/cuda_interactive_2560_pass_table.txt` and
`cuda_interactive_native_slider_table.txt`. Hot repeats = 11.

| Slider | Size | Node.field | Value | GPU P50 | P50 executed trace |
| --- | --- | --- | --- | ---: | --- |
| Bayer 8-grade cold | 2560×1705 | all grades ev=0.2 | initial | 133.85 | UploadRaw 54.88, then all 8 grades + DRT |
| last Exposure hot | 2560×1705 | g7.exposure | 0.55 + 0.03×i | 9.75 | Develop skipped; g7 + DRT 1.61 |
| first Contrast hot | 2560×1705 | grade.primary.contrast | 12 + 1×i | 48.10 | all 8 grades (LLF on each) + DRT |
| mid Saturation hot | 2560×1705 | g3.saturation | 1.15 + 0.02×i | 26.75 | g3 through g7 + DRT |
| last Exposure hot | 4600×3064 native | g7.exposure | 0.55 + 0.03×i | 14.96 | Develop skipped; g7 LLF + DRT 5.86 |
| 8-grade cold | 4600×3064 native | all grades ev=0.2 | initial | 702.41 | UploadRaw + 8 LLF grades; peak 4418 MiB |

X-Trans 2560×1710, `grade.primary.exposure` 0.70 + 0.03×i: cold GPU 361.85
(UploadRaw 341.29), hot GPU P50 8.91 (Develop skipped).

**Family B — product present last-Exposure slider** (8 clean Color Grades,
no LLF). Dump:
`build/tmp/preview_performance/cuda_interactive_2560_present_table.txt` and
`cuda_interactive_2560_present_frames.csv`. Path: `submitWrite(exposure)` on
the last Color Grade. Start 0.15, step 0.02, wrap 0.10–1.80, period 8 ms,
10 s × 3 Detail plus Summary and Off. Interactive render **2560×1705**.
`events_lost=0`.

| Run | Mode | Writes | Presented | Dropped | qml p50/p95/p99 | gpu p50 | drt gpu p50 | sink p50 | encode p50 |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Detail | 690 | 421 | 35 | 19.91 / 25.07 / 26.04 | 4.31 | 3.11 | 5.23 | 0.05 |
| 2 | Detail | 650 | 408 | 35 | 21.47 / 24.59 / 25.33 | 5.64 | 3.71 | 6.66 | 0.05 |
| 3 | Detail | 650 | 420 | 26 | 21.28 / 24.95 / 25.76 | 5.57 | 3.57 | 6.51 | 0.05 |
| 4 | Summary | 647 | 412 | 34 | 21.48 / 25.00 / 25.91 | 5.54 | 3.59 | 6.48 | 0.05 |
| 5 | Off | 646 | — | — | no stamps | — | — | — | — |

Median presented frame (run 1): Develop skipped; only the last clean Grade
(pointwise 2.63) plus DRT 3.15. First presented frame after topology insert
still executes the seven extra Grades. Felt `qml_ms` P50 is 20–22 ms. DAG
encode stays 0.05 ms P50. Sink Map/copy is 5–7 ms P50. Off vs Detail does not
change the 10 s write-loop wall time.

**Family B2 — product present last-Exposure slider with LLF enabled** (8 clean
Color Grades, then last-grade Shadows=18 and Highlights=-12). Dump:
`build/tmp/preview_performance/cuda_interactive_2560_present_llf_table.txt` and
`cuda_interactive_2560_present_llf_frames.csv`. Same `submitWrite(exposure)`
slider as Family B after a node-switch seal between Shadows and Highlights.
10 s × 3 Detail. Interactive render **2560×1705**. `events_lost=0`.

| Run | Mode | Writes | Presented | Dropped | qml p50/p95/p99 | gpu p50 | llf gpu p50 | last grade gpu p50 | drt gpu p50 | sink p50 | encode p50 |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | Detail | 678 | 417 | 28 | 20.79 / 25.98 / 31.39 | 8.59 | 3.89 | 2.17 | 1.61 | 7.05 | 0.94 |
| 2 | Detail | 644 | 429 | 17 | 20.36 / 24.86 / 31.22 | 8.55 | 3.90 | 2.16 | 1.61 | 7.16 | 0.98 |
| 3 | Detail | 644 | 419 | 22 | 21.33 / 25.31 / 31.68 | 8.47 | 3.89 | 2.15 | 1.61 | 7.18 | 0.97 |

Median presented frame (run 1): Develop skipped; last Color Grade only
(pointwise, then LLF extract/pyramid/remap/select/collapse/apply) plus DRT 1.61.
LLF GPU P50 is 3.89 ms. Total GPU P50 is 8.5–8.6 ms versus 4.3–5.6 ms with LLF
off. Felt `qml_ms` P50 stays 20–21 ms. Sink Map/copy is 7.1–7.2 ms P50. DAG
encode is 0.94–0.98 ms P50.

Command:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_release_test --parallel 4 --target EditorPreviewPresentTrajectoryTest
EditorPreviewPresentTrajectoryTest.exe  (ALCEDO_TEST_EDITOR_BACKEND=cuda, windows QPA)
```

### NM8.2R — 完整 UI 的调度与呈现复核（2026-09-14 调度修复通过性能验收）

**复核背景。** 最小呈现 fixture 的约 16 ms 结果不能外推为产品拖动延迟。
用户使用 Release、Leica Q3 `L1010776.DNG`，删除 Mask，保留 Develop →
`grade.primary` → DRT 三节点后仍明显卡顿。RAW 文件位于
`alcedo_studio/tests/resources/sample_images/raw/camera/leica/q3/L1010776.DNG`；
Develop 为 FULL 9512×6328，Interactive 为 2560×1703，CUDA + D3D11，
NVIDIA GeForce RTX 3080 Laptop GPU。没有更换后端、算法或降低图像质量。

本轮检查的是 `bf2b2b7d` 上的未提交改动，包括独立 render-progress 通知、consume
wakeup 合并、呈现循环只在队列有 Ready 帧时继续，以及新增等待分段。用户生产日志
`alcedo_preview_perf_alcedo_20260913_221836_40284.log` 的稳定窗口如下；
完整路径为 `C:/Users/zidage/AppData/Local/Alcedo/Alcedo/logs/` 加该文件名。

| 日志窗口 | Presented / window ms | input P50 | qml P50 | extra_sched P50 | ready_to_gui P50 | gui_to_import P50 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 22:20:18 | 19 / 1041 | 83.31 | 85.41 | 28.65 | 0.06 | 41.89 |
| 22:20:19 | 17 / 1028 | 86.69 | 87.25 | 31.09 | 0.08 | 41.13 |
| 22:20:20 | 20 / 1013 | 84.37 | 85.17 | 30.90 | 0.07 | 39.87 |

单位 ms。`input_ms` 截止于 import，`qml_ms` 截止于 `frameSwapped`；二者终点不同。
`ready_to_gui` 实际从 present wake 计时，也不能解释为整个 producer-ready 后的等待。

**完整 UI 复现。** 修复 `alcedo_studio_test_host` 的 QuickQanava 链接、插件导入及
初始化，使它加载实际 `Alcedo.Main`，包括调整面板、节点组件、filmstrip 和 scopes。
使用隔离项目和上述 RAW；窗口逻辑尺寸 1200×760，DPR 2。外部 Node 进程每 8 ms
请求一次鼠标移动；本机实际约 65 次/秒。`pointer` 请求只投递一个 QMouseEvent，
经过 QQuickWindow 和实际 Exposure slider，不在处理函数中调用 `processEvents()`。
按下、持续移动 10 s、释放后验证画面和数值变化。该输入仍是合成窗口事件，不包含
物理鼠标到 Windows 分发之前的延迟。

首轮完整 UI 复现约 22 帧/秒，稳定窗口 qml P50 约 85–92 ms、gui_to_import
约 39–41 ms，与用户日志同量级。随后启用 Qt 窗口分段时间线和 request CSV，
对照同一完整 UI 的呈现等待；以下为已呈现且有输入、request id > 5 的样本，
包含最后释放后的结果，排除未呈现记录。P50 独立计算，不能逐列相加为一帧。

| 诊断配置 | 样本数 | input→frameSwapped P50 | input→Submit P50 | GUI update→consume P50 | consume→import P50 | 实际场景图 sync P50 | 新图呈现 / s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 默认 VSync、默认交换链等待 | 231 | 71.55 | 31.58 | 35.68 | 0.038 | 0.181 | 22.86 |
| 仅 `QT_D3D_MAX_FRAME_LATENCY=0` | 301 | 72.35 | 25.47 | 25.32 | 0.031 | 0.205 | 30.10 |
| 仅 `QSG_NO_VSYNC=1` | 566 | 18.29 | 7.34 | 5.37 | 0.035 | 0.142 | 56.60 |

前两行输入延迟 P95 分别 101.84、81.97 ms，第三行为 28.22 ms。
第二行将主要等待从 `beginFrame` 移到了 Present：前者 P50 从 15.96 ms 降至
0.031 ms，Present 从 0.25 ms 升至 15.89 ms，并未消除端到端等待。
第三行是因果对照，非产品优化：测量区间发生约 1.2 万次窗口渲染，不能以取消
VSync、窗口空转或 `frameSwapped` 回调提前代替真实显示性能验收。
时间线记录会扰动节奏；上述值是本次诊断样本，不替换原始生产窗口成绩。
整理测试宿主后的默认 VSync 复跑为 232 个样本，input→frameSwapped P50/P95
69.73/99.81 ms，实际 sync P50 0.173 ms，consume→import P50 0.039 ms，
新图呈现 22.86/s。该次还断言了非法 pointer 请求被拒绝、按下后 slider 持有拖动、
释放后结束拖动，以及 Exposure 从 1.50 变为 3.57；660 个 move，验证通过。

**同一帧证据。** 默认配置 request 250 以 Submit 为 0 ms：输入 -37.353，
producer ready 9.721，GUI update 10.234，consume 46.148，import 46.188，
frameSwapped 47.111。期间窗口在约 13 ms 和 30 ms 已完成两轮渲染，但这两轮
没有场景图同步；新照片到约 46 ms 才被消费。纹理导入本身约 0.040 ms。
因此“窗口在画”不等于“照片的新 Ready 帧正在被消费”。

**当前改动为什么仍不够。** Qt 6.9.3 的 `QQuickItem::update()` 会进入
`QSGThreadedRenderLoop::maybeUpdate` → `postUpdateRequest` →
`QWindow::requestUpdate()`。删除 Alcedo 的显式 requestUpdate 不会绕开这一链。
Windows 路径先等待 DXGI 的窗口更新通知；GUI 发起 polish/sync 后又可能等待
render thread 的 `QRhiD3D11::beginFrame()` 交换链等待。Qt 日志中的 `sync=16 ms`
包含 beginFrame，不能归为面板或节点图执行。依据是本轮信号分段和 Qt 6.9.3 的
[threaded render loop](https://github.com/qt/qtdeclarative/blob/v6.9.3/src/quick/scenegraph/qsgthreadedrenderloop.cpp)、
[D3D11 RHI](https://github.com/qt/qtbase/blob/v6.9.3/src/gui/rhi/qrhid3d11.cpp)。
现有证据不支持继续将剩余几十毫秒主要归因于 `loadFromSnapshot`。

**需要解决的两个调度依赖（尚未实施）。**

1. 参数合并、pacing deadline、live pipeline 的串行完成和下一帧消费，应由独立
   session owner 执行；GUI 发送最小参数修改并接收通知。保持一个 live pipeline、
   一个串行 Apply，不引入参数全量副本或并行 Apply。单纯提高 GUI 事件优先级
   无法抢占 GUI 正在等待 Qt sync 的时间。
2. Ready 帧的消费应能利用当前 render-thread 呈现机会，不必先经过 GUI 更新和
   下一次场景图同步。实现需保持 VSync、已有 slot/reader 生存期、图像/epoch
   校验和隐藏窗口停止行为，并在无输入、无待呈现帧时停止主动刷新。
   只搬走 producer owner 仍会留下约 40 ms 的呈现等待；只调整呈现仍会留下
   GUI owner 的 deadline/完成传导延迟。

**验收缺口。** 现有 `RepeatedInteractiveEnqueuePostsOneConsumeWakeup`、
`InteractiveFrameReadyDoesNotNotifySessionChange`、
`RenderProgressDoesNotBroadcastStateOrReloadAdjustmentSnapshot` 和
`PresentLoopContinueDoesNotTickWhenNoReadyFrameIsWaiting` 覆盖的是通知与局部队列行为，
没有验证上述完整窗口节奏。本轮未将这些单元测试视为性能通过证据，也未重跑全套。
必须补充 GUI 正处于窗口同步、Ready 在同步后到达、释放、切图、隐藏/恢复、关闭、
旧帧完成及单一 Apply 的测试；完整 UI 必须与最小 fixture 分别报告正常 VSync 下
的 request 数、P50/P95/P99 和新图呈现频率。
`extra_sched` 当前排除了零值且使用未关联 request 的最近 release 时间，
还需验证旧 completion 交错和零等待分布，不能单独据此精确归因 owner 排队。

**采集入口。** Release 构建命令为
`cmd /c scripts\msvc_env.cmd --build build/release --target alcedo_studio_test_host --parallel 4`。
设置 `ALCEDO_PREVIEW_PERF_LOG` 为汇总日志路径、`ALCEDO_TEST_FRAME_TRACE` 为 request
CSV 路径；后者同时输出带单调时间戳的 `[FrameTrace]` 窗口分段。Windows 下
`QT_FORCE_STDERR_LOGGING=1` 将 Qt 输出交给进程 stderr；`QSG_RENDER_TIMING=1`
启用 Qt 自带分段。独立测量保留默认 VSync；对照变量只在对应子进程设置。
JSON Lines 输入格式为 `{"id":1,"method":"pointer","phase":"press","x":1028,"y":527}`，
随后发送 `move` 和 `release`；坐标从实际 slider 的 sceneRect 取得。
原始文件和脚本保存在 `build/tmp/nm82r_latest_review/`，本节已写入必要数据，
不依赖被 Git 忽略的文件链接。测量宿主加载的实际 UI 已通过截图与渲染记录确认。

##### NM8.2R 调度修复与验收记录（2026-09-14）

**实施。** 按上文两条边界完成正式修复，未关闭 VSync、未改交换链配置、
未降低画质，保持一个 live pipeline 与一个串行 Apply。

1. **Session owner 线程化。** 新增 Qt-free
   `EditorSessionThreadedCommandExecutor`（`editor_session_command_queue.hpp/.cpp`）：
   专用 worker + 单调时钟 deadline 最小堆，`Post`/`PostDelayed`/`IsOwnerThread`/
   `Shutdown`；`Stop` 丢弃未到期的 delayed 任务，`Shutdown` join worker
   （自 join 防护）。`ApplicationModuleHost` 生产路径改用它，测试保留
   `EditorSessionManualCommandExecutor`（`DrainAll` 也排空 delayed bin）。
   `EditorSessionService` 析构先 join worker 再 `Stop` 队列、
   `CancelAndWait`——`DeliverCompletion` 纯异步投递，无死锁。
   `EditorSessionRuntime` 成员序调整使 service 先于 executor 引用析构。
   `EditorSessionLifecycle`/`EditorSessionNavigationController` 增加
   `SetOwnerCheck`（默认构造线程，service 注入 `command_queue_.IsOwnerThread`），
   lifecycle 全方法加互斥锁。admission deadline 经
   `PostCompletionDelayed` 回到 owner，不再依赖 GUI `QTimer`；
   `editor_session_controller.cpp` 的 `BindAdmissionDeadline` 与 timer 已删除。
   GUI 入口的直连判定由裸 `reducing_command_` 改为
   `InOwnerReduction()`（owner 线程且正在 reduce），GUI 调用方不再可能
   读到陈旧 true 而在线下执行 reducer。`pipeline_document()` 改为
   `std::shared_ptr<const PipelineDocument>` 快照（Emit/EndPublication
   时 `ClonePipelineDocument` 刷新），`EditorSessionHistoryPort` 全部
   façade 方法加互斥锁，mask-creation 读态由 service 侧镜像发布。
   Interactive `FrameReady` 不再 `NotifyChange`；render progress 经独立
   `SetRenderProgressObserver` 通道驱动 `RenderBusyChanged`/
   `RenderDiagnosticsChanged`，不再广播全量状态。

2. **Ready 帧的 render-thread 消费。** `EditorViewportItem` 在
   `attachWindow` 时以 `Qt::DirectConnection` 连接
   `QQuickWindow::beforeRendering`（先于 `QQuickRhiItemNode` 的
   `beforeRendering→render` 连接注册，故先执行）。lambda 只捕获
   `shared_ptr` 状态：`DirectPresentQueue::HasReadyFrame()` 或
   `DirectFrameSink::HasPendingImportedFrame()` 为真时，经
   `consume_arm_`（`shared_ptr<atomic<EditorViewportRenderer*>>`，
   `createRenderer` 发布、renderer 析构 CAS 清空）调
   `renderer->ArmForPresent()` → `QQuickRhiItemRenderer::update()`：
   置 node 的 `m_renderPending` 并请求下一窗口帧，同一次 render pass
   的 node `render()` 即消费 Ready 帧——不需要 GUI `update()`，也不需要
   再一轮场景图同步。`frame_sink_` 改 `shared_ptr` 保活 arm 回调；
   `continueInteractivePresentLoop` 只在确有 Ready 帧等待时才
   重新 dirty item，无输入无待呈现帧时停止主动刷新。

**完整 UI 验收（默认 VSync + 默认交换链，非对照组）。** 同一 Release 测试
宿主、同一张 `L1010776.DNG`、Develop → `grade.primary` → DRT 三节点、
Interactive 2560×1703、CUDA + D3D11；外部 Node 进程 press/654 次
move/release，Exposure 1.50 → 3.57，拖动持有与释放断言通过。稳定窗口
（拖动期间每秒汇总）：

| Presented / s | e2e_ms P50 | e2e_ms P95 | input_ms P50 | input_ms P95 | extra_sched P50 | ready_to_gui P50 | gui_to_import P50 | import_to_swap P50 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 59–61 | 4.45–5.98 | 6.54–7.90 | 7.60–11.29 | 16.82–21.25 | 0.40–0.78 | 0.10–0.42 | 0.76–3.10 | 0.40–0.49 |

单位 ms；`dropped=0 failed=0 cancelled=0`。对比修复前默认 VSync 基线
（input→frameSwapped P50 71.55、GUI update→consume P50 35.68、
新图呈现 22.86/s）：呈现频率达到显示刷新率 60/s，input P50 降至约
10 ms（P95 ≤ 21.3 ms），优于此前仅关闭 VSync 的对照组（18.29 ms、
56.60/s），且 VSync 与交换链等待全部保留。帧级 CSV 中 request 563、566
等样本 `gui=0`——Ready 帧完全经由 `beforeRendering` 呈现机会消费，
无 GUI update 记录；ready→consume 最坏约一个 vsync 周期。
原始采集位于 `build/tmp/nm82r_threaded/`（`threaded_default_perf.log`、
`threaded_default_frames.csv`、宿主 Qt 日志、驱动脚本与截图）。

**测试状态。** `win_debug` 全量构建通过；受影响测试全绿：
`EditorSessionCommandQueueBaselineTest` 15/16、`EditorSerialFrameAdmissionTest`
5/5、`EditorPendingInputSessionTest` 6/6、`EditorSessionLifecycleTest` 18/18、
`EditorSessionRenderControllerTest` 15/15、`EditorSerialFrameConsumptionTest`
12/12、`EditorSessionControllerPhase5ATest` 54/54（含新增
`RenderProgressDoesNotBroadcastStateOrReloadAdjustmentSnapshot` 与
`PresentLoopContinueDoesNotTickWhenNoReadyFrameIsWaiting`）、
`EditorSessionHistoryPortTest` 77/77、`EditorNodesPanelQmlTest` 41/41、
`EditorPreviewPresentTrajectoryTest` 1/1、`EditorSerialInputBoundaryTest` 7/7、
`PipelineFrameSinkTest` 36/36。既有失败与本次改动无关：
`RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`
（NM4.6 起既有）、`AdjustmentPanelsReloadOnlyWhenCommittedContentChanges`
（测试 JSON 键 `ev` 应为 `exposure_ev`）、`EditorSessionRenderSchedulerPortTest`
5 例（路径全为已提交代码，空 ImageBuffer 渲染失败）、
`SerialMaskInteractiveTest`（Sep 11 陈旧二进制，源码与 CMake 目标已不存在）。

### NM8.3 — 固定调色顺序与融合 pass 编译

**工作：** 更新调整类别和 GraphCompiler，使所有受支持的 Grade 都按
Basic Tone → Color → Local Tone → Mix 编译。Basic Tone 组保持白平衡/曝光/对比度/白黑场
内部顺序，Color 组保持 Curve/HLS/Saturation/Vibrance/ColorWheel/LMT 内部顺序，
Shadows/Highlights 进入一个 LLF 阶段。编译期间不修改 live document 或历史内容。
静态 plan 携带算法版本、阶段及读写需求，后端不另行猜测顺序。

新顺序是本次明确批准的画面语义。旧文档保存的调整排列由编译器按这套规则解析；
参数和历史身份不重写，不能保留按旧顺序执行的隐藏路径。更新 render algorithm revision、
静态 plan key 和相关派生图像缓存身份，防止重用旧算法缩略图、预览和 LLF 图。
若现有格式无法表达这一解释边界，在本阶段列出确切格式改动，不能让 reopen 和 export
选择不同顺序。算法顺序变化与之后的等价性能优化分别建立像素预期。

**主链：** document 的稳定节点/参数身份 → compiler 固定阶段 → 一个 Basic Tone + Color pass
→ LLF → Mix → 同一规则的 Interactive/Quality/export。
**失败链：** 非法结构/不支持调整 → 编译错误 → 不执行未验证计划，不修改 document。

**验证：** `ColorGradeCompilesBasicToneAndColorBeforeLocalTone`、
`BasicToneAndColorUseOnePointwisePass`、
`NewGradeOrderMatchesIndependentExpectedPixelsWithinTolerance`、
`AlgorithmRevisionRejectsPreviouslyDerivedImages`、
`ReopenedAndExportedDocumentUseTheSameGradeOrder`。

**完成条件：** 三后端共享唯一阶段顺序；有激活 LLF 和非线性 Color 的独立结果证明新顺序，
不能仅检查命令列表；旧顺序输出差异明确来自已批准的算法顺序变化。

### NM8.4 — 共享双工作图与 LLF/Mix 执行

**工作：** 按第 4 节整体修改 PlanExecutor、GradeExecutor、LocalToneExecutor、
result persistence/invalidation、workspace、GraphImageCache、DRT/Post 及三个原生后端。
Grade 工作图改为 workspace pair，移除持久节点 RGBA 和 R8 结果，LLF 只保留既有两类图。
拆开 LLF 准备和最终应用，最终应用融合 Mix，完成同像素读写的原生实现及资源声明。
参数变化只更新必要字段；无效节点删除/重连清理 LLF，不逐帧构造整图副本。

**主链：** 关键阶段有效输入 → shared pair → 每 Grade fused pointwise → LLF maps →
Local Tone + Mix → 下一个 Grade → DRT/Post 复用 → frame sink → reader 释放。
**失败链：** 分配/原生编码/执行/呈现失败 → 原 owner 取消和完成处理 → 不发布 LLF 新版本
→ 不把还在被读的工作图归还为可写；错误保留真实后端信息。

**验证：** `MultipleGradesReuseTwoRgbaWorkImages`、
`MaskedLocalTonePreservesOriginalGradeInput`、
`FinalMixReadsAdjustedPixelBeforeOverwritingIt`、
`GradeOutputsAreNeverPublishedToPersistentCache`、
`PostProcessingReusesGradeWorkImagesAfterLastRead`、
`PresentedImageLeasePreventsWorkImageOverwrite`、
`ColorChangeInvalidatesLocalToneSourceAndResult`、
`OwnMaskChangeKeepsLocalToneMapsAndInvalidatesDownstreamMaps`、
`QualityRenderDoesNotReplaceInteractiveLocalToneMaps`、
`FailedFrameDoesNotPublishLocalToneResults`。

**完成条件：** 真 GPU 像素比较通过；不含 LLF 的多节点主工作 RGBA 数量不随节点数增长；
有 LLF 时只允许各 owner 的必要 LLF 图增长；Grade 持久结果计数为零；后处理、reader、
跨帧复用和连续编辑通过验证。数量断言只用于明确的资源类别，不声称总显存只有两张图。

### NM8.5 — 硬件热点与整帧优化

**工作：** 用同一测试场景比较 NM8.2 原实现、新顺序、NM8.4 双图实现三个版本。
先用时间线确认 CPU、同步、分配或 GPU 哪部分占主要时间，再修改对应代码。
候选项包括 plan key/按值复制、重复参数布局和命令上传、工作图申请、LLF dispatch、
Mask/Union 临时图读写、pointwise 分支/寄存器压力、DRT/Post 和呈现交接。
逐项记录修改前后指标，不能以“kernel 少了”替代延迟证据。

CUDA：NVTX 关联 request/node/pass，Nsight Systems 检查空档、提交、同步、拷贝和并行；
Nsight Compute 只对已确认热点收集带宽、访存、寄存器、占用率和计算吞吐。
[Nsight Systems](https://docs.nvidia.com/nsight-systems/UserGuide/)、
[Nsight Compute](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html)。

Metal：对应 command buffer/encoder labels 和 CPU 标记，Instruments Metal System Trace
检查 CPU/GPU/内存和呈现；Xcode GPU trace/counters 检查具体 pass。
[Apple Metal 性能分析](https://developer.apple.com/documentation/xcode/analyzing-the-performance-of-your-metal-app)。

硬件采集可能重放、序列化或调整缓存状态。详细计数器采集期间的应用 E2E 单独标记，
不作为正常交互成绩。最终延迟从不挂详细 profiler 的相同优化构建取得。
CUDA、Metal 分别优化真实瓶颈；共享阶段和像素语义一致，不要求相同线程组配置。

**主链：** 对照数据 → 确认热点 → 单项优化 → 数值验证 → 重跑相同场景 → 记录收益。
**失败链：** 数值/资源/延迟回归 → 修正或撤销该优化，不降低质量、不改用其他后端。
**完成条件：** 主要热点有原始数据和处理结果，Off/Summary/Detail 开销量化，
目标硬件和场景的 P95/P99、工作内存及同步次数有可复核结果。

### NM8.6 — 全产品性能与最终资格验证

**工作：** 执行总方案第 23/26 节。覆盖真实 RAW、filmstrip 切图、右侧动画、ZoomPan、
Detail ROI、参数/Mask、拓扑编辑、Undo/Redo、Version、Paste、reopen、export、后台任务。
验证纯 UI 操作不引发新的 Grade 计算；释放操作 Quality 不被错误 pacing 延迟。
Windows/CUDA、OpenCL、macOS/Metal 和安装包分别记录，重新验证 Brush 排除边界。

**主链：** 安装包打开项目 → 完整编辑/恢复/导出 → 相同节点顺序和 Mask 像素 →
性能/内存数据 → 总方案完成记录。
**失败链：** 任何生命周期、数据恢复、原生执行或包装失败 → 精确记录失败项，
该平台/场景不标记通过，保持真实错误处理。

**完成条件：** 第 7 节数值/资源保证、既定 16 ms producer 目标及明确硬件上的 E2E 预算
均有证据；总方案全部完成条件通过后才能标记 NM8 complete。
未达到预算时记录具体耗时与瓶颈，继续优化；不能通过修改统计口径宣称达标。

## 6. 首轮性能测量清单

今天的执行入口是 NM8.1，然后 NM8.2；本次写方案本身不算完成这两个阶段。

1. 固定 commit、优化构建、GPU/驱动、Qt/OS、后端、窗口/viewport/DPR、RAW、decode 和
   实际 render 尺寸，记录文档与各节点参数。所有记录带 Brush OFF 的实际 cache 证据。
2. 做一遍首次加载，单列磁盘读取、RAW/模型/着色器初始化；再预热连续交互，直到明确的
   初始化工作完成。不得把首次加载混入热帧数据，也不得删除超预算的有效热帧样本。
3. 固定一次约 10 秒的输入轨迹，重复至少三次，保留有效帧数和所有请求终态。
   记录输入频率、合并数量、实测输出帧数；样本不足时不报告不可靠的高分位数。
4. 对下表至少采集单节点、四节点、八节点，以及一项带多个解析 Mask 的场景。
5. 比较 Off、Summary、Detail；再单独做一份 CUDA 系统时间线，Metal 主机就绪后按同样
   矩阵采集其自身基线。OpenCL 单列实测状态。没有运行的平台记 pending。
6. 输出一份表格和一条能追到具体 Grade/pass 的慢帧时间线，列出前三项实测成本。

| 场景 | 操作 | 用途 |
| --- | --- | --- |
| 单 Grade，无 LLF/Mask | 连续 Exposure/Color | 最短调色路径及日志成本 |
| 4/8 Grade，无 LLF | 分别改首/中/末节点 | 观察当前缓存收益、失效成本和多节点读写 |
| 4/8 Grade，LLF 启用 | 分别改上游 Color、本节点 Shadows/Highlights | 分开 LLF source 重建、result 重建和复用 |
| Grade 带 Radial/Gradient | 改位置、范围、feather、opacity、Grade Mix | coverage/union/mix 的成本及 LLF 依赖 |
| 连续输入后释放 | Interactive → Quality | 终值正确、串行完成、Quality 响应 |
| 同图 pan/zoom、Detail ROI | 固定视图轨迹 | 最终帧复用、ROI 和呈现等待 |

报告每组的 CPU/GPU/producer/E2E P50/P95/P99、最大值、帧间隔、dispatch/传输/分配、
Grade/LLF/Mask 执行次数、缓存状态、峰值显存、采样与事件丢失数。
明确本机硬件与目标场景，再根据基线分解预算；不编造通用的 GPU/E2E 毫秒阈值。
已有 16 ms producer 目标保持不变，E2E 另含输入等待和呈现排队。

## 7. 数值、资源和正确性证据

- 原执行方式加计时前后，必须在同后端保持最终像素结果；日志开关不能改变图像。
- 新顺序使用独立运算顺序和固定输入建立预期，不能以旧顺序的像素作为新算法正确性标准。
- 双图/融合 Mix 优化与相同新顺序的独立实现比较，复用现有测试中更严格的容差。
  新增测试需写明绝对/相对容差、最大/平均误差和失败坐标，不以“看起来相同”验收。
- 覆盖 Mix 0/1/中间值、Mask 内外/边缘/重叠、多个 Grade、LLF 开关、非线性 Color、
  HDR/负值、裁剪旋转、不同尺寸、奇数尺寸和连续多帧。R8 比较维持现有最多一个码值误差。
- LLF source/result 因不同依赖分别重建；本节点 Mask/Mix 变化保留本节点 LLF，
  上游 Mask/Mix 变化使下游 LLF 失效。重连、删除、图像/Version 切换和失败恢复均覆盖。
- 资源测试同时断言类别计数、实际 native 像素和 reader 生存期，不能只比较 ResourceId。
  GPU submission 未完成或呈现 reader 未释放时，池内资源不可写。
- 性能测试与正确性测试分开；没有真实 GPU 的测试不能证明原生数值、alias 安全或时间。
  不运行零匹配过滤器并把它记录为通过。

现有测试入口：
[graph compiler](../../../../../alcedo_studio/tests/edit/runtime/graph_compiler_test.cpp)、
[grade schedule](../../../../../alcedo_studio/tests/edit/runtime/grade_schedule_test.cpp)、
[CUDA multi Grade](../../../../../alcedo_studio/tests/edit/runtime/cuda_multi_grade_test.cpp)、
[OpenCL multi Grade](../../../../../alcedo_studio/tests/edit/runtime/opencl_multi_grade_test.cpp)、
[Metal multi Grade](../../../../../alcedo_studio/tests/edit/runtime/metal_multi_grade_test.cpp)、
[Metal LLF](../../../../../alcedo_studio/tests/edit/runtime/metal_llf_test.cpp)、
[image retention](../../../../../alcedo_studio/tests/edit/runtime/graph_image_cache_retention_test.cpp)、
[app logging](../../../../../alcedo_studio/tests/utils/editor_app_logging_test.cpp)。
扩展对应职责的测试并注册目标，不把所有行为塞入一个全应用 fixture。

## 8. 构建、采集和记录方式

遵循 [MSVC 构建 skill](../../../../../.agents/skills/alcedo-msvc-cmake/SKILL.md)。
Windows 用 wrapper，配置/编译总预算从 20 分钟开始，健康进程继续等待；工具短轮询不代表失败。
Release 用于性能；Debug 用于诊断正确性。所有临时日志、脚本、原始 trace 和表格放在
`build/tmp/preview_performance/`，不在仓库根目录创建临时文件，不提交原始机器日志。

NM8.2 CUDA used `win_debug` and `win_release_test` with `ALCEDO_ENABLE_BRUSH_MASK=OFF`
and wrote tables under `build/tmp/preview_performance/`. The templates below remain
the command pattern for later phases. Windows 优化测试构建使用现有 `win_release_test`；
实际 app 和测试 target、可执行文件路径在执行时从 CMake/CTest 发现并写入完成记录。

```powershell
# ALCEDO_ENABLE_BRUSH_MASK=OFF
cmd /c scripts\msvc_env.cmd --preset win_release_test -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
# ALCEDO_ENABLE_BRUSH_MASK=OFF: verify the configured cache before building
cmd /c scripts\msvc_env.cmd --build --preset win_release_test --parallel 4
# ALCEDO_ENABLE_BRUSH_MASK=OFF: enumerate registered cases before selecting targets
ctest --test-dir build/release-test -N
```

```bash
# ALCEDO_ENABLE_BRUSH_MASK=OFF; use the project's configured release key and toolchain
cmake --preset macos_release -DALCEDO_ENABLE_BRUSH_MASK=OFF
# ALCEDO_ENABLE_BRUSH_MASK=OFF: verify the configured cache before building
cmake --build --preset macos_release --target alcedo_main
```

build/test/install/package/profile 本身不接收 CMake `-D` 参数，记录必须引用已验证的
同一 build directory、实际可执行文件和 `ALCEDO_ENABLE_BRUSH_MASK=OFF` cache。
不得因性能测试修改正式发布安全配置。安装包测试使用既定 release/package 流程。
Windows 和 macOS 的实际硬件 profiler 命令、工具版本、采集范围和参数在 NM8.2/NM8.5
记录；以本机已安装工具能力为准，不虚构可用选项或已执行的命令。

## 9. 完成记录

每个阶段在本节追加记录，保留历史基线，不把“计划了测试”写成“测试通过”。

```text
Phase / date / status:
Commit / actual build / backend / device / Brush OFF evidence:
Changed behavior:
Primary success and failure call chains:
Actual test commands / nonzero case counts / outcomes:
Measurement mode / cases / sample counts / lost events:
CPU / GPU / producer / E2E / cadence / memory results:
Raw evidence location and reproducible input description:
Numerical tolerance and result:
Remaining platform or product verification:
```

当前执行记录：NM8.1 complete 2026-09-12. NM8.2 CUDA complete 2026-09-13
(GPU timestamps, 2560 slider DAG, native-sensor slider DAG, felt present).
OpenCL/Metal pending. NM8.2R scheduling/presentation fix complete and passed
complete-UI qualification on 2026-09-14 (default VSync, 60/s presented,
input P50 ~10 ms). See the dated records under those headings.
NM8.3–NM8.6 have no execution evidence yet.
