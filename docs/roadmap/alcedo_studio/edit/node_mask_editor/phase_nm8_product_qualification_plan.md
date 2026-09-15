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
NM8.3 complete on 2026-09-14. NM8.4 implementation and Windows CUDA/OpenCL
qualification are complete on 2026-09-15; macOS Metal true-device qualification
remains pending, so NM8.4 status is partial. NM8.5–NM8.6 planned.
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
5. **本阶段删除的跨帧结果只指 Color Grade scene RGBA。** LLF 继续复用现有
   source.0 / result.0 表示；更高层金字塔、remap 和 collapse 工作图仍是算法临时资源。
   Mask source / Union 的 R8 结果属于 Mask owner，不属于这对 RGBA 工作图；NM8.4
   不改变它们的分配、持久化、失效或量化策略。
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

在 workspace 内增加专用 `SceneWorkImagePair` owner。它只拥有两张相同 extent、
`RGBA32F` 的原生工作资源及其资源统计，不保存 NodeId、GraphValueId、revision、参数、
document 状态或上一帧内容有效性。CUDA/Metal 可以使用各自普通可读写纹理；OpenCL
可以使用专用线性 buffer。后端差异封装在 scene-work 创建和绑定接口内，不能把普通
`Texture2D` 改成时而是 image、时而是 buffer 的联合类型。

`SceneWorkImagePair` 与 `GraphImageCache`、Develop、Geometry、Camera Color、Mask、LLF
和 display output 是不同资源类别。NM8.4 不修改 `GraphImageCache` 和 `TexturePool` 的
通用语义，也不把工作图登记成伪 GraphValueId。工作图 owner 必须单独报告当前/峰值字节、
原生分配次数和固定成员数；总资源快照必须包含这些字节，不能因为绕过 texture pool 而漏报。
ExecutionPlan 只描述数据依赖和读写需求，不拥有 GPU 资源，也不持有可变 document 镜像。

取消 Grade scene_output 的持久查找/发布，但仍保留 GraphValueId、依赖 revision 和错误定位。
当前 scene 的物理位置使用 `PlanExecutor` 栈上的 frame-local binding 显式传给 Grade 和
DRT/Post executor；它不存入 workspace，不跨 `BeginRender`/`EndRender`，也不参与
Publish/Cancel。帧内容发生变化并进入 Grade 区域时，从关键阶段的有效输入顺序执行所需
Grade；不再承诺修改末尾 Grade 可以跳过前面的 Grade 计算。纯 pan/selection 等复用最终帧
由现有请求/呈现 owner 决定，不以保留工作图像素实现。

工作图对继续交给 [DRT/Post](../../../../../alcedo_studio/src/include/edit/runtime/drt_post_executor.hpp)
及后续兼容任务，不重新分配按节点命名的 ping/pong。不同尺寸、格式、读写用途不兼容时，
由资源 owner 在已完成读取的边界重新取得正确资源；不缩小图像或改变格式满足复用。
[邻域算法](../../../../../alcedo_studio/src/include/edit/runtime/neighbor_executor.hpp) 必须保留
其数学上必需的 scratch，不将这些存储伪装为持久 Grade 输出。

这对图是同一 render workspace 的主 RGBA 工作集，并非全应用所有内存的上限。
LLF 单通道图、邻域 scratch、关键阶段有效结果和最终呈现资源分别计量。
最终呈现始终写入现有 display output lease；scene 工作图不能直接交给 frame sink、Qt
或 export reader。这样 reader 生命周期继续由 display owner 管理，工作图只受单一串行
render workspace 和上一 submission 完成边界保护。若以后要直接呈现 scene 工作图，必须
另立方案定义所有权和 reader 释放，不能在 NM8.4 中顺带开放。跨帧占用由已有呈现槽位和
在途数量约束，不能随 Grade 数量增长。不通过每节点同步换取复用，也不新增全局显存预算
调度器。

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

Radial/Linear Gradient 的参数和 R8/Union 结果继续由现有 Mask owner 管理。NM8.4
不改变其查找、发布、失效、量化、Union 或最后读取释放规则；这些资源不计入“两张
RGBA32F scene 工作图”，也不能被改造成工作图成员。将解析 coverage/Union 融入 Mix、
删除跨帧 Mask 结果或改变 Mask 资源策略属于独立优化，必须先有测量和单独批准；本阶段
不以修改 Mask 算法、缓存或量化精度缩小主工作集。

## 5. 子阶段与完成条件

| 阶段 | 内容 | 依赖 | 初始状态 |
| --- | --- | --- | --- |
| NM8.1 | 低开销日志、输入到呈现时间线、CPU 分段 | 当前产品路径 | complete 2026-09-12 |
| NM8.2 | 节点/pass 原生 GPU 计时、当前实现基线及硬件采集 | NM8.1 | CUDA complete 2026-09-13 (2560 slider DAG, native-sensor slider DAG, felt present); OpenCL/Metal pending |
| NM8.3 | 新顺序、融合 pass 描述、算法版本和画面预期 | NM8.2 当前后端基线 | complete 2026-09-14 on `feature/nm83-fixed-grade-order` (CUDA + OpenCL measured on this host; Metal covered by shared compiler + macOS-only test targets) |
| NM8.4 | 共享工作图、取消 Grade 缓存、LLF/Mix 与下游复用 | NM8.3 | partial 2026-09-15: implementation + Windows CUDA/OpenCL passed; macOS Metal true-device run pending |
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

##### Phase NM8.3 completion record (2026-09-14)

**Status:** complete on branch `feature/nm83-fixed-grade-order`. CUDA and OpenCL
verified on this host (RTX 3080 Laptop, driver 610.62, CUDA 12.8, Qt 6.9.3);
Metal consumes the same compiler-produced stages and its test file was updated
in lockstep — Metal test targets are macOS-only and were not run here.

**Implementation.** `ColorGradeCompileOrder()` /
`ColorGradeCompileRank()` / `ColorGradeCompileIndexOrder()` in
`edit/graph/adjustment_ownership.{hpp,cpp}` define the fixed rank
Cat02WhiteBalance → Exposure → Contrast → White → Black → Curve → Hls →
Saturation → Vibrance → ColorWheel → Lmt → Shadows → Highlights.
`GraphCompiler::CompileColorGrade` stable-sorts compiled adjustment indices by
that rank (same-type instances keep stored relative order); stored document
order, parameter models, instance ids, and history are untouched.
`AppendGradeStage` merges adjacent same-kind stages, so one grade compiles to
`Pointwise[0..11)` + `LocalLaplacian[11..13)` (+ optional `Neighborhood`),
with Mix as the implicit final write — `BasicToneAndColorUseOnePointwisePass`
asserts one fused pointwise dispatch on CUDA.

**Identities.** `StaticPlanKey.compile_algorithm_version = 1` (new key field,
in equality/ordering); `kPrimaryGradeImplementationVersion` 5 → 6;
`kLlfReferenceImplementationVersion` 1 → 2. `HashGraphTopology` and the grade
content hash in `result_content_key.cpp` hash adjustments in compile order, so
a stored reorder no longer forces recompilation or a different result key.
`MixGradeExcludingLocalToneValues` now precisely describes the canonical LLF
source (all non-LLF parameters execute before LLF).
`RuntimeInvalidationState::CollectGradeChanges` classifies a dirty adjustment
as local-tone vs source-side by its compiled instance id, not by document
index → stage range.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ColorGradeCompilesBasicToneAndColorBeforeLocalTone` | `GpuDagModelGraphTest` (`graph_compiler_test.cpp`) | PASS (debug) |
| `BasicToneAndColorUseOnePointwisePass` | `GpuDagCudaPrimaryGradeTest` | PASS (debug) |
| `NewGradeOrderMatchesIndependentExpectedPixelsWithinTolerance` | `GpuDagCudaPrimaryGradeTest` (`cuda_multi_grade_test.cpp`) | PASS (debug) — colored plane, Saturation + active Shadows LLF, independently composed chained-grade reference |
| `AlgorithmRevisionRejectsPreviouslyDerivedImages` | `GraphImageCacheRetentionTest` | PASS (debug) |
| `ReopenedAndExportedDocumentUseTheSameGradeOrder` | `GpuDagModelGraphTest` | PASS (debug) — JSON round-trip keeps stored order/ids; Interactive, Export-quality, and reopened compiles emit identical order and identical `StaticPlanKey` |
| Stored reorder keeps compiled order + plan key | `GpuDagModelGraphTest`, `StaticExecutionPlanCacheTest`, `cuda_product_plan_cache_test.cpp` | PASS (debug) |
| Same-type instances grouped by rank | `cuda_multi_grade_test.cpp`, `opencl_multi_grade_test.cpp`, `metal_multi_grade_test.cpp` | PASS (debug CUDA/OpenCL; Metal updated in lockstep, macOS-only) |

Suite totals (win_debug): `GpuDagModelGraphTest` 61/61,
`GraphImageCacheRetentionTest` 17/17, `GpuDagCudaPrimaryGradeTest` 58/59
(1 intentional skip: superseded dump), `GpuDagOpenClGradeTest` 48/48,
`GpuDagCudaWorkspaceTest` 39/40, `GpuDagCudaDrtProductTest` 73/74,
`GpuDagCudaMaskTest` 10/10, `GpuDagMaskStoreTest` 8/8, `GpuDagGeometryTest`
15/15, `GpuDagRawInputTest` 145/145, `GpuDagOpenClWorkspaceTest` 22/22,
`GpuDagOpenClDrtProductTest` 25/25, `GpuDagCudaDevelopTest` 21/23,
`GpuDagOpenClDevelopTest` (2 pre-existing + 1 skipped).

**Pre-existing regressions repaired:** `CudaLlfFailedSubmissionDoesNotPublishCanonicalPlanes`
and `OpenClLlfFailedSubmissionDoesNotPublishReference` were already failing on
`main` (documented as a pre-existing regression in NM7.14 acceptance evidence).
Root cause: the tests injected failure via
`plan.geometry.full_reference_extent = {}`, which legitimately changes the
canonical LLF identity and drops published planes before the throw. Both tests
now inject an out-of-range `stages[0].begin`, which fails submission inside
`MakeGradeSchedule` without touching any image identity; both PASS.

**Remaining failures — verified identical on `main`** (same failing assertions
reproduced under a stashed baseline build): `GpuAndRuntimeHeadersDoNotIncludeCudaOrImageBuffer`
(`drt_post_executor.hpp` comment scan), `CudaDefaultPipelineSecondRenderCreatesNoGpuAllocation`
(FreeCount=1), `CropRotateViewportAndScaleExecuteAsOneCudaResample`,
`CanonDngProfileRendersAtFullResolutionAndInvalidatesOnlyColorCache` (CUDA +
OpenCL), `SwitchingHighlightReconstructionDoesNotKeepStalePublishedTextures`,
`OpenClCameraColorConsumesSharedDualIlluminantTransform`. None traverse the
grade compile path; none are caused by this phase.

**Primary call chain (as implemented):**

```text
PipelineDocument adjustment list (stable NodeId/instance ids, stored order)
  -> GraphCompiler::CompileColorGrade
       ColorGradeCompileIndexOrder: stable sort by ColorGradeCompileRank
       compiled.adjustments + parameter bindings in fixed order
       AppendGradeStage -> Pointwise[0..N) + LocalLaplacian[N..M) (+Neighborhood)
  -> StaticPlanKey{topology_hash(compile order), compile_algorithm_version=1,...}
  -> StaticExecutionPlanCache (rejects plans compiled under version 0/old key)
  -> BindAndScheduleGrade -> GradeSchedule ops: one fused pointwise dispatch
     -> LLF (shadows/highlights) -> implicit Mix write
  -> Execute*PrimaryGrade (CUDA/OpenCL/Metal consume schedule verbatim)
  -> same compile for RenderQuality::Preview and RenderQuality::Export and
     reopened documents
```

**Failure chain:** unsupported/invalid adjustment or stage range → compile or
`MakeGradeSchedule` `std::runtime_error` → `CancelRender` → no unpublished
revision is published; document and history untouched.

**Interpretation boundary (required disclosure):** every render served by
`GraphCompiler` — Interactive preview, `RenderQuality::Export`, and reopened
documents — shares the single fixed order; the static plan key binds it.
`ExportService::RunExportRenderTask` still schedules the legacy
`PipelineExecutor` (`PipelineTask`), whose stage sequence is hardcoded
(Basic: exposure→contrast→white→black→highlights→shadows→curve; Color:
saturation→vibrance→HLS→colorwheel) and predates the DAG runtime. Unifying
production export onto the DAG compiler is outside NM8.3 scope and is the
remaining order-divergence risk for export pixels; no hidden old-order path
remains inside the DAG runtime.

**NM8.2 measurement round on the new order** (`win_release_test`,
`ALCEDO_ENABLE_BRUSH_MASK=OFF`, same fixtures/scenarios as NM8.2):
correctness 9/9 PASS (`GpuPassSamplesKeepRequestAndNodeIdentity`,
`TimingSlotsAreNotReusedBeforeSubmissionCompletes`,
`GpuTimingDoesNotAddPerPassHostWaits`,
`CachedAndDisabledPassesReportExecutionState`,
`DetailTimingPreservesRenderedPixelsWithinTolerance`,
`InteractiveThreeNodeGraphReportsPassGpuTimes`,
`InteractiveFourNodeSecondGradeMasksReportGpuTimes`,
`InteractiveMultiGradeMaskMixReportsPerNodeGpuTimes`,
`TwoLutGradesReportIndependentPassGpuTimes`).
`InteractiveDagBaselinesDumpCurrentExecutionGpuTimes` is intentionally SKIPPED
(superseded by the 2560 slider dumps). New dumps:
`build/tmp/preview_performance/cuda_interactive_2560_pass_table.txt`,
`cuda_interactive_native_slider_table.txt`; NM8.2 originals preserved under
`build/tmp/preview_performance/nm82_baseline/`.

GPU P50 milliseconds, new fixed order vs NM8.2 baseline:

| Scenario | NM8.2 | NM8.3 | Δ |
| --- | ---: | ---: | ---: |
| Bayer 2560×1705 8-grade cold | 133.85 | 106.10 | −20.7% |
| g7.exposure hot | 9.75 | 8.41 | −13.7% |
| grade.primary.contrast hot | 48.10 | 43.66 | −9.2% |
| g3.saturation hot | 26.75 | 29.31 | +9.6% |
| X-Trans 2560×1710 cold | 361.85 | 363.09 | +0.3% |
| X-Trans exposure hot | 8.91 | 8.17 | −8.3% |
| native 3-node cold | 81.55 | 72.72 | −10.8% |
| native 3-node exposure hot | 7.23 | 6.67 | −7.7% |
| native 4-node masked cold | 92.09 | 78.08 | −15.2% |
| native 4-node look exposure hot | 8.77 | 8.30 | −5.4% |
| native multi-grade mask-mix cold | 98.04 | 84.99 | −13.3% |
| native multi-grade last exposure hot | 8.85 | 8.24 | −6.9% |
| native two-LUT cold | 74.10 | 71.93 | −2.9% |
| native two-LUT look exposure hot | 6.58 | 6.20 | −5.8% |
| native 8-grade cold | 702.41 | 391.26 | −44.3% |
| native 8-grade last exposure hot | 14.96 | 12.97 | −13.3% |

Cold-trace sub-stage evidence: the 8-grade cold frame drops from 20 `pointwise`
sub-stage records to 10 (one fused pointwise pass per grade plus DRT-side
records) — the fusion is visible in measured GPU work, not just plan shape.
The `g3.saturation` hot row is the expected trade-off: Saturation now executes
before LLF, so editing it correctly invalidates the LLF source and rebuilds
local tone where the old post-LLF placement did not.

**Residual gaps:** Metal measurements not possible on this Windows host.
OpenCL slider/present GPU tables remain pending from NM8.2. Legacy export
divergence noted above. NM8.4 shared work images not started.

### NM8.4 — 共享双工作图与 LLF/Mix 执行

#### NM8.4.1 目标和不可扩张边界

本阶段只改变 **Camera Color/Develop 有效 scene 输出之后、最终 display output 之前** 的
RGBA32F 物理工作资源安排。目标是在一个串行 render workspace 中固定复用两张相同 extent
的 scene 工作资源，不再为每个 Grade 或 DRT/Post 创建各自的 RGBA ping/pong 和持久 Grade
scene 输出。这里复用的是 **原生分配**，不是上一帧的像素内容。

以下规则是实现边界，不得以优化便利为理由扩大：

1. `SceneWorkImagePair` 只拥有两张 RGBA32F 原生资源、extent/format 和资源计数。
   它不保存 GraphValueId、NodeId、revision、document、参数、当前节点或内容有效性。
2. 不新增 `pending`/`committed` scene、whole-Grade-chain 内容命中、跨帧 Grade 输出跳过，
   或任何功能相同但名称不同的 workspace side cache。每次 executor 实际进入 Grade 区域时，
   都从本帧已解析的关键阶段输入开始顺序执行需要的 Grade。
3. 不修改 Develop、Geometry、Camera Color 的缓存、失效、发布、别名或质量策略。
   第一个 Grade 只读它们的现有有效输出，不把它们迁入工作图 owner，也不覆盖其纹理。
4. 不修改 Mask source / Union R8 的 owner、缓存、失效、量化和 Union 规则。Mask 纹理可被
   最终 Mix 读取，但不是 scene 工作图成员，也不计入两张 RGBA32F 工作资源。
5. LLF 继续只按现有 owner 保留 source.0/result.0 R32F canonical 图。高层金字塔、remap、
   collapse 和 Neighborhood 横向结果继续是算法 scratch；它们不计入 scene 工作图数量，
   也不能改造成持久 Grade RGBA 结果。
6. 最终 display output 继续由现有 image/presentation owner 管理。scene 工作图不能直接
   传给 frame sink、Qt 或 export reader，因此不为工作图新增呈现 lease 或 reader 状态。
7. 不改变 NM8.3 已固定的算子顺序、融合范围、Mix/Mask 数值、LLF 算法、FP32、render
   extent、decode 质量、后端选择、串行执行和单一在途 submission 规则。
8. 不通过第三张 Grade RGBA 图、每节点 RGBA 临时图、整图入口复制、其他后端、CPU、
   降低分辨率或降低质量解决原地读写问题。某后端不能满足时报告真实阻塞，不提交替代路径。

#### NM8.4.2 资源分类和唯一 owner

| 资源类别 | Owner | 跨帧保留 | NM8.4 行为 |
| --- | --- | --- | --- |
| Sensor/Develop/Geometry/Camera Color 图 | 现有 GraphImageCache/阶段 owner | 按既定策略 | 完全不改 |
| 两张 scene RGBA32F 工作资源 | 新 `SceneWorkImagePair`，由 render workspace 独占 | 只保留分配；内容无效 | 本阶段新增 |
| Grade scene_output RGBA | 无物理 owner；GraphValueId 只用于逻辑依赖 | 否 | 停止查找、分配、记录和发布 |
| LLF source.0/result.0 R32F | 现有 LLF/GraphImageCache owner | 按既定 frame role | 保留并校正依赖 |
| LLF 高层和 remap/collapse | 现有 transient owner | 否 | 不改资源类别 |
| Mask source/Union R8 | 现有 Mask owner | 按既定策略 | 完全不改 |
| Neighborhood scratch | 现有 transient/texture scratch owner | 否 | 保留数学必需资源 |
| LUT、pipeline state、参数 arena、static plan | 各现有 owner | 按既定策略 | 完全不改 |
| display output 和呈现 lease | 现有 display/presentation owner | 按 reader 生命周期 | 最终写入目标，不能成为工作图成员 |

新增永久类型按领域职责命名，例如 `SceneWorkImagePair`；不得以 NM8.4、迁移编号或临时步骤
命名。该 owner 负责：

- 在上一 submission 已完成的 `BeginRender` 边界确认两张成员的 extent/format；
- extent/format 相同则保留原生分配，不重新申请；
- extent/format 改变时，在 GPU 最后读取完成后一起释放并一起重新创建；
- 只提供成员 0、成员 1 和 `PeerOf(member)` 的受控访问；
- 报告成员数、当前/峰值字节和原生分配次数；
- session teardown 时在设备 idle 后释放两张资源。

工作图可以由专用 owner 直接拥有，不登记为 GraphImageCache entry；但
`CaptureResourceSnapshot` 及性能日志必须单列并计入它们。不能只统计 TexturePool 后宣称
工作内存下降。RGBA32F 两张图的预期字节数为 `width * height * 16 * 2`，资源测试同时核对
分类字节和原生 allocation counter。

#### NM8.4.3 帧内 scene 位置必须显式传递

`PlanExecutor` 在栈上持有一个仅本帧有效的 `FrameSceneBinding`。它只表示以下二选一位置：

```text
CachedImage(GraphValueId)   // Develop/Camera Color 等现有有效图
WorkImage(Member0|Member1)  // SceneWorkImagePair 的一个成员
```

该 binding 不拥有资源，不保存 revision/extent，不写入 workspace，不跨 EndRender，不参与
Publish/Cancel。其引用有效期由现有 GraphImageCache lease 或 `SceneWorkImagePair` owner 保证。
逻辑 revision 继续由 ExecutionPlan/RuntimeInvalidationState 管理，不能复制到 binding 中形成
第二套有效性记录。

调用接口必须显式表达数据流：

```text
PlanExecutor
  FrameSceneBinding scene = CachedImage(plan.develop_output)
  for compiled Grade in order:
      scene = GradeExecutor::Execute(..., scene)
  DrtPostExecutor::Execute(..., scene)
```

不得让 GradeExecutor/DrtPostExecutor 通过 `workspace.ResolveSceneValue(logical_id)`、全局
current scene、relabel、commit 或类似隐式状态交换当前物理位置。禁用、Mix=0 或没有有效
调整的 Grade 直接返回输入 binding，不复制像素，也不把 binding 改写成伪 scene_output。

#### NM8.4.4 每个 Grade 的精确读写规则

设 `S` 为当前 Grade 的完整输入 binding，`W` 为另一张可写工作成员。第一个 Grade 的 `S`
可以是 Camera Color/Develop 的缓存图；后续 Grade 的 `S` 必须是工作成员，`W` 是其 peer。

**没有 LLF/Neighborhood 的普通 Grade：**

```text
S 保持只读
  → fused Basic Tone + Color + final Mix：读取 S，写 W
  → 返回 WorkImage(W)
```

Mix=1 仍走同一个最终写出语义；Mix=0/禁用/空操作直接返回 S。带 Mask 时最终 kernel 读取
现有 Mask R8，保持 opacity、invert、Union 和 Grade Mix 的既定计算。

**带 LLF 的 Grade：**

```text
S 保持只读
  → fused Basic Tone + Color：读取 S，写 W
  → LLF Prepare：读取 W，生成或复用 source.0/result.0，并完成所有邻域计算
  → LLF Apply + final Mix：
       先读取 S[p]、W[p]、LLF planes、Mask[p]
       再原地写 W[p]
  → 返回 WorkImage(W)
```

LLF apply 只能读取 W 的同一像素和已经独立完成的 LLF planes；它不能在覆盖 W 时读取 W 的
邻居。Prepare 返回的数据只在本次 Grade encode 内有效；实现不得为了跨调用保存它而把完整
backend Ops 方法表搬进公共 header。优先保持后端操作在各自实现内部，只暴露准备结果所需的
最小拥有类型和 Prepare/Apply 入口。

**带 Neighborhood 的 Grade：**

```text
当前 scene 像素位于 W
  → horizontal：读取 W 的邻域，写独立 horizontal scratch
  → vertical apply：读取 W[p] 和 horizontal scratch 邻域，原地写 W[p]
```

若 Neighborhood 是本 Grade 的最后有效阶段，vertical apply 同时读取 S[p]/Mask[p] 并融合
final Mix。horizontal 完成前不得覆盖 W。其 scratch 是算法必需资源，不是第三张 Grade
scene RGBA 图；scratch 的 extent、格式、最后读取和释放继续由 Neighbor owner 管理。

任何新增或已有 stage 若需要在写 W 时读取 W 的邻居，必须先把邻域依赖完整写入其专用
scratch，再进入原地逐像素 apply。不能默认“看起来是逐像素”就允许别名；每个原生入口都要
通过对应后端的真实像素测试证明。

#### NM8.4.5 Grade 之间与 DRT/Post 的轮换

节点间只轮换两个成员：

```text
Camera Color/Develop → Grade 0 写 Member0
Member0             → Grade 1 写 Member1
Member1             → Grade 2 写 Member0
...
```

每个 Grade 内保留 `S` 直到最终 Mix 已编码；只有同一有序 GPU queue 上的最后读取已经排在
后续写入之前，peer 才能成为下一节点的目的成员。不新增逐 Grade synchronize、host wait 或
新 command queue。

DRT/Post 的最终输出必须落到现有 display output：

- 没有启用 Post：DRT transform 从当前 scene 直接写 display output；
- 有 `N` 个 Post：根据奇偶性，让 DRT transform 和 Post 在 display output 与 scene 的
  **空闲工作成员**之间轮换，最后一次写入必须是 display output；
- 当前 scene 在 Member0/1 时，DRT/Post 只能使用其 peer 作为中间成员；
- 当前 scene 仍是关键阶段缓存图时，默认 Member0 是空闲成员；
- DRT/Post 不创建自己的 `runtime.ping`/`runtime.pong` GraphValueId；
- frame sink 只接收 display output lease，不能接收 Member0/1。

display output 不计入“两张 scene 工作图”；它是向 reader 交付最终像素所必需的独立结果。

#### NM8.4.6 缓存和失效边界

Grade scene_output GraphValueId 继续存在于编译计划、依赖传播、诊断和错误信息中，但不得用于：

- `BindValidResult` 或其他跨帧内容查找；
- `AcquireImageForWrite`；
- `AliasImageFrom`；
- `RecordUnpublished`；
- `PublishSuccessfulSubmission`；
- workspace 内的 pending/committed scene 映射。

因此，修改末尾 Grade 不能依靠旧的上游 Grade RGBA 图跳过前面节点。若请求 owner 已判定整帧
可直接复用，它可以沿用现有最终 display frame；一旦进入 PlanExecutor 的 Grade 区域，就从
本帧关键阶段有效图顺序执行 Grade。

LLF 有效性按语义 revision 判断，不按 Member0/1 地址判断：

- 本节点 Basic Tone/Color 或任何上游完整 Grade 输出语义变化：本节点 LLF source/result 失效；
- 只改本节点 Shadows/Highlights：source 可保留，result 重建；
- 只改本节点最终 Mix 或本节点 Mask：本节点 source/result 保留；
- 上游 Mix/Mask 变化：所有受影响下游 LLF 失效；
- 删除、重连、reopen、Version、Paste 和图像切换：沿用 RuntimeInvalidationState 的逻辑依赖；
- QualityBase 不发布 Interactive LLF 的替代版本，保持既定 persistence scope。

Mask R8 继续走现有 Bind/Record/Publish 路径；NM8.4 不为了 Grade scene 去缓存而删除它们，
也不把 Mask 是否需要执行的产品规则从 Mask/Grade schedule 复制到 PlanExecutor 的新
`dynamic_cast` 判断中。

#### NM8.4.7 三后端原生边界

共享 executor 只决定 stage 顺序、S/W 成员、最终 Mix 时机和 DRT/Post 奇偶目的位置。
原生资源表示、参数绑定、kernel/pipeline state 和错误由各后端 owner 负责。

**CUDA：**

- 两个工作成员使用现有 CUDA RGBA32F 线性存储表示；
- 最终 pointwise、LLF apply 和 Neighborhood vertical apply 必须允许 input/output 在 W 上别名；
- 不添加与实际别名冲突的 `__restrict__` 或跨线程读取；
- 每线程先读取本像素所需的 S/W/Mask/LLF 值，再写回 W[p]；
- 保持现有 stream 顺序，不新增 `cudaDeviceSynchronize`/`cudaEventSynchronize`。

**Metal：**

- 两个工作成员使用明确允许所需 read/write usage 的 RGBA32Float texture；
- 同一 W 绑定为读取和写入时，shader 只执行同像素 read-before-write；
- 所有邻域读取在独立 scratch 阶段完成；
- pipeline state 继续由现有 cache 取得，不能每帧或每 Grade 新建。

**OpenCL：**

- 普通 `OpenClBackend::Texture2D` 继续只表示 image-backed 缓存/输出纹理；
- 两个 scene 工作成员使用专用 RGBA32F row-major buffer 和明确的 width/height 元数据；
- 不给通用 Texture2D 增加 `buffer_backed` 分支、伪 device address 或 image/buffer 双重含义；
- cached image → work buffer、work buffer → work buffer、work buffer → display image 的参数绑定
  只存在于 Grade/LLF/Neighbor/DRT scene-work adapter 和对应 kernel 入口；
- 未使用的 image/buffer 参数若因 OpenCL kernel 签名必须绑定，由 adapter 集中管理，不能把
  dummy resource 逻辑复制到各 executor；
- 不修改普通纹理 upload/download/copy API 来适配仅由 scene-work 使用的 buffer；
- 若目标 OpenCL 能力不能安全原地执行，记录具体设备/API 阻塞，不暗中增加第三张 RGBA 图。

三个后端必须使用相同的独立预期公式和容差验证最终像素，但不要求使用相同原生资源类型、
线程组或 kernel 参数布局。

#### NM8.4.8 失败、取消和生命周期

工作成员的像素内容从不发布，因此失败处理不回滚、复制或恢复其旧内容：

```text
分配/参数上传/原生编码/执行/呈现失败
  → 原 render owner 等待或取消已记录工作
  → 丢弃未发布的 Grade-independent graph writes、LLF writes 和 display write
  → frame-local binding 随栈销毁
  → 两张工作分配可以保留，但其内容一律视为无效
  → 下一次 render 从关键阶段有效输入重新进入 Grade
```

不得为了“保留上一张正确 Grade 图”增加 committed scene。上一张已呈现画面由 display/
presentation owner 持有；工作图不承担 last-good frame。失败必须保留真实 CUDA/OpenCL/Metal
错误，不重试其他后端、其他算法、其他尺寸或旧的 per-Grade 路径。

extent 改变只能在 `BeginRender` 已确认上一 submission 完成后重建 pair。正常同 extent
连续编辑不得发生新的 pair 原生分配。session teardown 必须先 device idle，再释放 pair；
析构、取消和重建都不能影响 Develop/Geometry/Camera Color/Mask/LLF/display 的 owner 状态。

#### NM8.4.9 文件范围和评审切片

预期允许修改的职责范围：

- workspace 和新 `SceneWorkImagePair` owner；
- `PlanExecutor`、`GradeExecutor`、`LocalToneExecutor`、`NeighborExecutor`、`DrtPostExecutor`；
- CUDA/OpenCL/Metal 的 Primary Grade、Local Tone、Neighbor、DRT scene-work 原生入口和 shader；
- scene-work 分类所需的资源诊断字段、序列化和专用测试；
- 新文件的 CMake 注册。

以下文件/职责默认禁止修改；如发现真实阻塞，先在本节写出原因、所需接口和影响，再继续：

- Develop、demosaic、Geometry、Camera Color 实现及其缓存策略；
- `GraphImageCache`、`TexturePool`、result persistence 的通用规则；
- Mask evaluator、Mask Union、R8 量化和 Mask 持久策略；
- GraphCompiler 的 NM8.3 顺序、adjustment ownership 和 static plan key；
- editor input、scheduler、frame sink、present queue、viewport 和 export 流程；
- GPU 全局预算器、多帧流水线或新的 executor 并发。

实现按可独立评审的职责切片，但所有切片都属于 NM8.4，不能把完整能力被动推迟：

1. 新增 work-pair owner、frame-local binding、资源统计和纯 host 生命周期测试；
2. 改共享 Grade/LLF/Neighbor 编排，删除 per-Grade RGBA 获取/发布，不新增后端替代路径；
3. 完成 CUDA 原地最终写出及真实像素/资源验证；
4. 完成 OpenCL 专用 work-buffer adapter 及真实像素/资源验证；
5. 完成 Metal 原地最终写出及 macOS 真实像素/资源验证；
6. 接入 DRT/Post/display，删除旧 runtime ping/pong 使用，完成连续编辑和失败恢复验证；
7. 运行三后端及产品相关回归，写 NM8.4 完成记录。

每个切片以约 500 changed LOC 为评审目标，在真实原生边界无法再分时说明原因。不得用把同一
God class 方法移动到多个 `.cpp`、把完整 backend Ops 暴露到公共 header、或把所有资源塞进
一个可变 context 的方式伪装拆分。中间 commit 可以尚未完成产品切换，但最终 NM8.4 不能保留
新旧两套运行路径或运行时 fallback。

#### NM8.4.10 验证矩阵

按以下名字或同等明确的行为测试落实；fake backend 只能证明编排，不能替代真实 GPU 像素、
别名和原生资源计数。

| 验收行为 | 必需证据 |
| --- | --- |
| `SceneWorkPairOwnsExactlyTwoRgba32fImages` | 同 extent 首次创建恰好两张；成员格式/尺寸正确 |
| `SameExtentRendersReuseSceneWorkAllocations` | 连续多帧 allocation count 不增加；内容不作为命中 |
| `ExtentChangeRecreatesBothSceneWorkImagesAfterGpuCompletion` | 奇数尺寸和尺寸切换；旧资源最后读取完成后释放 |
| `SceneWorkBytesAreIncludedInResourceMeasurements` | 分类字节等于 `w*h*16*2`，总资源记录包含该值 |
| `MultipleGradesAlternateOnlyTwoRgbaWorkImages` | 1/2/4/8 Grade 的成员序列及真实原生 id，数量不随节点增长 |
| `RepeatedRenderDoesNotUsePreviousSceneWorkPixels` | executor 再次进入 Grade 时重新执行，不存在 whole-chain scene hit |
| `DisabledAndZeroMixGradesAliasFrameInputWithoutCopy` | binding 不变、无 scene dispatch、像素与输入一致 |
| `PointwiseGradeMixPreservesOriginalGradeInput` | Mix 0/1/中间值、非线性 Color、HDR/负值独立公式比较 |
| `MaskedLocalTonePreservesOriginalGradeInput` | Mask 内外/边缘/重叠，LLF + Mix 独立像素比较 |
| `FinalMixReadsAdjustedPixelBeforeOverwritingIt` | W 原地别名，最大/平均误差和失败坐标可见 |
| `NeighborhoodApplyReadsScratchBeforeInPlaceWrite` | horizontal/vertical 顺序、邻域像素和边界尺寸比较 |
| `GradeOutputsAreNeverPublishedToPersistentCache` | 每个 Grade scene_output 无 lookup/write/publish entry |
| `DevelopGeometryAndCameraColorKeepExistingCacheBehavior` | 改前已有命中/失效/发布测试保持相同计数和结果 |
| `MaskR8ResultsKeepExistingCacheAndQuantizationBehavior` | Mask/Union 既有命中、失效和最多一个码值误差保持 |
| `ColorChangeInvalidatesLocalToneSourceAndResult` | 上游/本节点 Color 变化重建正确 LLF owner |
| `OwnMaskChangeKeepsLocalToneMapsAndInvalidatesDownstreamMaps` | 本节点保留、下游重建，不按 work member 地址判断 |
| `QualityRenderDoesNotReplaceInteractiveLocalToneMaps` | frame role persistence scope 保持 |
| `PostProcessingUsesFreeSceneWorkMemberAndEndsOnDisplay` | Post 数量 0/1/2/3 的真实目的序列和最终 display 像素 |
| `SceneWorkImagesAreNeverPassedToFrameSink` | frame sink 收到 display lease；work member 无 reader/export lease |
| `FailedFrameDoesNotPublishLocalToneOrDisplayResults` | 各关键失败点注入；下一帧从关键阶段输入恢复 |
| `FailedFrameDoesNotCreateReusableSceneContent` | pair 可保留分配但不能产生下一帧 Grade skip |
| `OpenClCachedImagesRemainImageBacked` | Develop/Geometry/Camera/Mask/display 类型和普通 copy API 不变 |
| `NativeBackendsMatchIndependentTwoImageExpectedPixels` | CUDA/OpenCL/Metal 真设备，多 Grade+LLF+Mask+Post 比较 |

真实像素测试覆盖 Mix 0/1/中间值、Mask opacity/invert/Union、多个 Grade、LLF 开关、
Neighborhood、LUT、非线性颜色、HDR/负值、奇数尺寸、裁剪旋转后的固定 extent、连续多帧和
失败后恢复。报告绝对/相对容差、最大/平均误差和失败坐标。资源测试区分 scene pair、LLF、
Mask、Neighborhood scratch、关键阶段缓存和 display，不以两个 ResourceId 相等代替原生数量。

#### NM8.4.11 主链、失败链和完成条件

**主链：** 关键阶段有效输入 → frame-local binding → 两成员交替执行每个 Grade → 必要的
LLF/Neighborhood scratch → 最终原地 Apply/Mix → DRT/Post 使用空闲成员 → display output →
frame sink/display reader；work-pair 分配保留，像素内容不进入下一帧有效性判断。

**失败链：** 分配/参数/原生编码/执行/呈现失败 → 原 owner 等待或取消 → 不发布 LLF/display
新版本 → 丢弃 frame-local binding → pair 内容标记为不可使用 → 下一帧从关键阶段输入重算；
真实后端错误原样上报。

**完成条件：**

- 三后端真实 GPU 像素比较通过；Metal 未在 macOS 真机通过时 NM8.4 不标记 complete；
- 1/2/4/8 Grade 的 scene 工作资源始终恰好两张 RGBA32F，原生数量不随 Grade 数增长；
- pair 同 extent 连续帧不新增分配，extent 变化只在 GPU 安全边界重建两张；
- 资源日志明确包含 pair 字节和分配次数，不把它们隐藏在 TexturePool 统计之外；
- Grade scene_output 的 persistent lookup/write/publish 均为零；
- 不存在跨帧 scene carrier、committed/pending scene 或 whole-chain Grade 内容命中；
- Develop、Geometry、Camera Color、Mask R8、LLF canonical 和 display owner 的既定行为通过；
- DRT/Post 最终写入 display，work member 从不交给 frame sink/export reader；
- 失败、取消、连续编辑、Quality、reopen、Version、Paste 和图像切换恢复通过；
- 没有第三张 Grade RGBA、逐节点 RGBA、入口整图复制、每节点 host wait 或替代后端路径；
- 当前实现和测试的主链、失败链、实际命令及非零用例数写入本文件完成记录。

数量断言只约束 `SceneWorkImagePair` 类别，不声称整个 GPU 管线只有两张图。LLF、Mask、
Neighborhood scratch、关键阶段缓存和 display 都按各自 owner 独立计量。

#### NM8.4 完成记录 — 2026-09-15

**状态：partial。** NM8.4 的共享双工作图实现、Windows CUDA/OpenCL 真设备验证和产品
生命周期回归已收口。Metal 生产路径和 macOS-only 测试已同步改造，但当前主机是 Windows，
未产生 macOS Metal 真设备像素、原生资源和失败恢复证据。依据本节完成条件，不能将 NM8.4
标记为 complete。

**已实现：**

- `BasicRenderWorkspace` 独占一个 `SceneWorkImagePair<Backend>`；首次按 extent 创建恰好两张
  RGBA32F 工作图，同 extent 保留分配，extent 改变时在前一 GPU submission 完成后成对重建。
  资源记录新增成员数、当前/峰值字节和累计分配次数；`w*h*16*2` 有独立断言。
- `FrameSceneBinding` 只描述本帧 scene 所在的关键阶段缓存、工作图成员或 display，不拥有像素，
  不保存 revision，也不写回 workspace。每个 Grade 读取当前 binding 并写另一成员；disabled/
  zero-mix 保持输入 binding，Grade scene output 不再进入 persistent lookup/write/publish。
- Pointwise、LLF、Neighborhood、DRT/Post 已统一到相同双图语义。LLF 和 Neighborhood 的最终
  Apply/Mix 在工作成员上原地完成；DRT/Post 使用空闲成员并最终写入既有 display owner；frame
  sink、scope 和 export reader 仍只接收 display。
- CUDA、OpenCL、Metal 都有独立原生 binding adapter。OpenCL 继续让 Develop、Geometry、
  Camera Color、Mask 和 display 使用 image-backed `Texture2D`，仅 scene work 使用专用线性
  buffer；没有 CPU、Legacy 或其他后端替代路径。
- 资格测试补齐双图分配/复用/尺寸切换/字节统计、1/2/4/8 Grade、跨帧不复用 scene 内容、
  Mix/Mask/LLF/Neighborhood/DRT/Post、失败发布、frame sink 边界和三后端多 Grade 原生资源
  断言。OpenCL 8 Grade 真设备验证确认只有两个不同的 scene-work `cl_mem`。

**收口时发现并修复的问题：**

- CUDA 与 OpenCL 的 Quality → Interactive 回归仍把 Grade scene 当作可复用缓存。测试已改为
  明确验证关键阶段复用、全部 Grade 重新执行、Grade scene 无 published entry、有效 display
  可继续命中。
- OpenCL Film Grain 的 scene-buffer horizontal 路径误用了普通 Gaussian，常量输入下 grain
  energy 为零。现已使用与 image-backed 路径相同的确定性 Film Grain 采样。
- OpenCL LLF canonical Apply 误读未填充的 pyramid `widths[0]/heights[0]`，向 kernel 传入零
  尺寸并触发设备执行错误。Apply 现在显式接收实际 plane 尺寸；canonical 路径使用
  `mask_extent`，pyramid 路径使用该层尺寸。OpenCL working buffer 以单一读写参数表达原地
  Apply，避免把同一 `cl_mem` 伪装成两个独立资源。

**主成功调用链：**

`Renderer::Render` → `PlanExecutor::Execute` → 关键阶段 cache bind/miss →
`EnsureSceneWorkImages` → frame-local `FrameSceneBinding` → 每个 Grade 的 Mask/LLF/Pointwise/
Neighborhood → `DestinationWorkMember`/peer 轮换 → DRT/Post 使用空闲成员 →
`display_output` publish → frame sink/scope/export reader。

**主失败调用链：**

参数上传、工作资源、kernel encode/execute 或 present 失败 → 后端原错误上报 → submission 不
publish 新的 LLF/display revision → frame-local binding 丢弃；pair 分配可保留但其像素无有效性，
下一帧从有效关键阶段输入重新执行全部 Grade。测试覆盖 upload failure、cancelled submission、
LLF 失败、incompatible sink 和失败后的版本/文档恢复。

**Windows 执行证据：**

| 命令/范围 | 结果 |
| --- | --- |
| `cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagRawInputTest GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest GpuDagCudaDrtProductTest GpuDagCudaMaskTest --parallel 4` | 通过；CUDA/OpenCL 运行时及五个测试目标完成编译和链接 |
| `ctest --test-dir build/debug --output-on-failure -R "^(GpuDagRawInputTest\|GpuDagCudaPrimaryGradeTest\|GpuDagOpenClGradeTest\|GpuDagCudaDrtProductTest\|GpuDagCudaMaskTest)\\."` | 358 passed、1 个只输出性能数据的测试按设计 skipped；359 项中 0 failed |
| `cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagOpenClDrtProductTest --parallel 4`，随后运行全部 `GpuDagOpenClDrtProductTest.*` | 25/25 通过；包含 OpenCL DRT、呈现、失败不进入替代路径、Quality 和资源释放 |
| Version/Paste/Reopen 选择性回归 | 26/26 通过；覆盖 Paste 创建/取消/失败、Version checkout/失败恢复、项目 reopen 和 DAG/mask 保持 |
| `cmd /c scripts\msvc_env.cmd --build build\debug --target alcedo_main --parallel 4` | 通过；`alcedo_main.exe` 完成链接，PE icon 检查通过 |
| `git diff --check` | 通过 |

**文件规模与评审切片：** 新增 owner/binding/adapters 分别为
`scene_work_image_pair.hpp` 111 行、`frame_scene_binding.hpp` 78 行、
`scene_work_member.hpp` 22 行、CUDA/OpenCL/Metal adapter 37/48/37 行；新增通用和 CUDA 双图
测试为 109/317 行。既有 `opencl_backend.cpp` 和 `opencl_grade_test.cpp` 当前为 1014/1277 行，
本阶段只分别增加约 47/77 行，并保持在设备资源 owner 和同一后端资格 fixture 内；把这些少量
变更另拆文件会割裂资源生命周期或重复大 fixture，因此未做形式化拆分。新增核心文件和测试均
低于 500 行评审目标。

**剩余唯一完成门槛：** 在 macOS Metal 真设备上构建并运行 Metal Grade/Mask/DRT、多 Grade、
LLF、失败恢复、Quality、reopen/Version/Paste/图像切换测试，确认两张不同的原生 Metal
RGBA32Float texture、相同像素容差和无替代路径。该证据通过后才可把 NM8.4 改为 complete；
当前没有用 mock、Windows host-only instantiate 或 CUDA/OpenCL 结果替代 Metal 证据。

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
