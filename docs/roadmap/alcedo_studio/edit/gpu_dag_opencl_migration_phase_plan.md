# GPU DAG OpenCL 移植逐 Phase 计划

Date: 2026-08-28

Status: O0–O5 complete; O6 planned

Branch: `feature/gpu-dag-opencl`

Base: `feature/gpu-dag-workspace-vram`

Primary owner: Alcedo Studio 编辑管线 OpenCL 后端。

## 1. 必读设计文档

开始本计划前必须完整阅读：

- [GPU DAG 编辑管线重构 Phase 计划](gpu_dag_pipeline_rebuild_phase_plan.md)
- [GPU DAG Metal 移植逐 Phase 计划](gpu_dag_metal_migration_phase_plan.md)

主计划定义 PipelineDocument、GraphCompiler、ExecutionPlan、Model/DTO、dirty Patch、
RenderGeometryResolver、MaskStore、内容 key、缓存边界和 CUDA 参考行为。Metal 专项计划记录了
共享 `Renderer<Backend>`、`PlanExecutor<Backend>`、`BasicRenderWorkspace<Backend>` 和逐 Pass
后端特化的已落地写法。本计划只定义 OpenCL 移植的执行切片，不建立第二套 OpenCL graph、
Model、DTO、ROI、缓存语义或产品 session 调度。

发生冲突时遵循以下优先级：

1. 本计划对 OpenCL Phase、OpenCL 1.2 生命周期、program registry 和删除范围的明确要求；
2. GPU DAG 主计划的共享图、数据、缓存和几何设计；
3. 已落地 CUDA/Metal DAG 的共享 host 行为和像素参考；
4. 现有 OpenCL 旧管线只作为像素算法、设备互操作和性能基线，不能作为目标架构。

## 2. 当前基线、决策与边界

### 2.1 实施基线

本计划以包含以下内容的 `feature/gpu-dag-workspace-vram` 为 base：

- CUDA DAG 产品路径；
- `Renderer<Backend>`、`PlanExecutor<Backend>` 和 `PassEncoder<Backend, Kind>`；
- `BasicRenderWorkspace<Backend>` 及其 ParameterArena、TransientBufferArena、TexturePool、
  MaskTextureCache、GraphImageCache 和 NodeResultCache；
- Metal DAG 的 Develop、Geometry、CameraColor、Primary Grade、LLF、Mask、Mix、DRT 和 present；
- 当前 workspace VRAM 峰值与 Develop scratch 生命周期修正。

如果上述分支先合并或改名，OpenCL 分支可以改从等价集成提交创建，但必须记录准确 base SHA。
不得从只包含旧 G7 CUDA 实现、缺少共享 Renderer 或缺少现行 workspace 修正的提交开始。

本计划不重复 Metal M0 已完成的共享 host 重构。OpenCL 直接实例化现有模板，并只增加 OpenCL
Traits、CommandContext、PassEncoder、FramePresenter、program manifest 和 kernel。

### 2.2 Renderer 命名

产品/session 入口继续使用：

```cpp
template <class Backend>
class Renderer;

using CudaRenderer   = Renderer<CudaBackend>;
using MetalRenderer  = Renderer<MetalBackend>;
using OpenClRenderer = Renderer<OpenClBackend>;
```

不新增 `OpenClProductRenderer`、`OpenClDagRenderer` 或另一份 OpenCL 产品调度类。

`Renderer<OpenClBackend>` 复用现有：

- PreparedSourceCache；
- StaticExecutionPlanCache；
- MaskStore；
- session cache 与 one-shot cache 隔离；
- GraphCompiler 静态计划和逐帧 geometry 绑定；
- 失败传播、统计快照和 session 资源释放。

OpenCL 专属代码只负责原生资源、命令入队、program/kernel 获取、展示互操作和显式下载。

### 2.3 不允许 fallback

以下行为全部禁止：

- OpenCL context、program build、kernel、buffer、image、event 或 present 失败后进入 CPU；
- OpenCL Develop 失败后调用旧 RawProcessor OpenCL 整体产品路径；
- Neural demosaic 失败后改用 Legacy、RCD、CPU、CUDA 或 Metal；
- DAG OpenCL 失败后进入旧 `OpenCLGPUPipeline`、`GPUPipelineImpl`、PipelineStage 或 fused stage；
- OpenCL/D3D11 或 OpenCL/OpenGL 互操作失败后自动下载到 host 并作为成功帧提交；
- 为规避性能问题改变 DecodeRes、RenderQuality、viewport、输出精度或算法；
- 捕获真实错误并发布上一帧、空帧或低质量帧作为本次成功结果。

显式 host 输出只允许用于 export/test 请求。产品 Auto 策略如果在创建 renderer 之前选择了其他
后端，不属于本计划；一旦创建 `OpenClRenderer`，本次 render 不得切换后端。

失败必须取消未完成提交，不发布新的内容 key，并把原始 OpenCL 错误、program build log 或
互操作错误送到 app 层。

### 2.4 OpenCL 旧实现的用途

以下代码只能用于确认像素数学、设备互操作和旧性能基线：

- `edit/pipeline/pipeline_opencl_impl.cpp`；
- `edit/pipeline/pipeline_opencl_param.cpp`；
- `edit/pipeline/opencl_shader/edit_pipeline_fused.cl`；
- `edit/pipeline/opencl_shader/edit_pipeline_detail.cl`；
- `edit/operators/GPU_kernels/opencl_param.hpp`；
- `edit/operators/basic/highlight_shadow_local_tone_opencl.cpp`；
- RAW Processor 下现有 OpenCL operator wrappers；
- `opencl::OpenClImage` 的格式、上传和互操作代码；
- 当前 OpenCL scope analyzer 的设备资源包装。

不得把这些类型包装进 DAG 继续使用。存在算子私有 scratch、私有 program/kernel handle、
逐调用 `clCreateBuffer`、逐 stage 参数 buffer、内部 `clFinish`、host present 替代路径或总参数上传
的入口必须改成接收当前 OpenClCommandContext 与 workspace 资源的 encode-only 实现。

可复用 `.cl` 数学函数时，应移动到新的 DAG 编译单元或共享 `.cl` include；不得为了复用数学
继续保留旧 `OpenClFusedParams` ABI、旧 fused kernel 入口或 `OpenCLGPUPipeline`。

### 2.5 OpenCL 1.2 是最低且唯一的可移植基线

所有新 program 使用 `-cl-std=CL1.2`。不得要求 SVM、command buffer extension、subgroup、
OpenCL 2.x device enqueue 或厂商专属扩展才能执行默认三节点图。

设备创建 `OpenClRenderer` 前必须明确验证：

- GPU device 可用且 compiler 可用；
- `CL_DEVICE_IMAGE_SUPPORT` 为真；
- 支持 DAG 所需 RGBA32F、R32F 和 R8 image format；
- 最大 image 尺寸覆盖当前 RenderRequest；
- 最大单次分配、global memory 和 local memory 满足编译计划的峰值；
- work-group 和 work-item 限制满足所选 kernel 配置；
- 产品 present 所需 D3D11 或 OpenGL sharing 已在同一个 context 上建立。

缺少强制能力时直接返回带设备名和缺失能力的错误。不得静默改成 buffer-only、host present、
低分辨率或其他后端路径。

### 2.6 program 注册与构建生命周期

OpenCL program 注册必须集中且长寿命：

```text
RegisterBuiltinOpenClProgramManifests
  -> RegisterOpenClRawProcessorPrograms
  -> RegisterOpenClGpuDagPrograms
  -> RegisterOpenClScopePrograms
  -> RegisterOpenClDemosaicNetPrograms
  -> OpenClBackendProgramRegistry::RegisterAllPrograms
  -> OpenClContext::Initialize
  -> OpenClBackend::WarmUpPlan
  -> OpenClProgramLibrary::GetProgram
  -> OpenClKernelCache::GetKernel
```

规则：

- manifest 只描述 program 名、source path、build options 和启动策略；
- renderer、render device、PassEncoder、operator 和每帧执行路径不得注册 program；
- `OpenClProgramLibrary` 保持通用，只负责 descriptor、lazy build、warm-up 和 program cache；
- program 和 kernel 名在 OpenCL DAG 模块头中集中定义，不散落字符串；
- program 按编译单元分组，不按 C++ operator 对象一 kernel 一 program；
- `WarmUpPlan` 只构建当前 ExecutionPlan 需要的 program 和 kernel；
- program build、kernel create、cache hit/miss 必须有可读取的 API 计数；
- 稳定 render 的 program build 和 kernel create 必须为零。

## 3. 目标架构

```text
PipelineDocument
      │
      ▼
GraphCompiler ───────────────── shared, GPU-free
      │
      ▼
ExecutionPlan
      │
      ▼
Renderer<OpenClBackend>
├── PreparedSourceCache
├── StaticExecutionPlanCache
├── MaskStore
└── BasicRenderDevice<OpenClBackend>
    ├── OpenClCommandContext
    │   ├── process-wide in-order command queue reference
    │   ├── current render final event
    │   └── submission id
    ├── BasicRenderWorkspace<OpenClBackend>
    │   ├── ParameterArena
    │   ├── TransientBufferArena
    │   ├── TexturePool
    │   ├── MaskTextureCache
    │   ├── GraphImageCache
    │   └── NodeResultCache
    └── OpenClSharedGpuResources
        ├── OpenClProgramLibrary references
        ├── OpenClKernelCache
        ├── LUT image/buffer cache
        ├── immutable sampler state descriptions
        └── Neural model and activation ownership
```

### 3.1 编译期后端区分

共享 host 运行时不增加 OpenCL switch。使用现有模板和明确特化：

```cpp
using OpenClRenderDevice = BasicRenderDevice<OpenClBackend>;

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::GeometryResample> {
  static void Encode(OpenClRenderDevice& device,
                     const ExecutionPlan& plan,
                     const PreparedRawInput& input,
                     PipelineDocument& document,
                     MaskStore* mask_store);
};
```

规则：

- 外层可以保留一次 IRenderDevice 类型隐藏；
- 一帧内部不按后端做虚调用；
- GraphCompiler 不包含 `cl_*` 类型或 OpenCL header；
- GraphCompiler 不为 OpenCL 复制 pass 构建流程；
- OpenCL capability version 参与 StaticPlanKey；
- host 生命周期、缓存 key、GraphValueId 和 dirty 传播与 CUDA/Metal 相同；
- OpenCL C kernel 源码独立，参数语义和调整顺序使用共享 DTO/runtime 数据；
- 大段像素数学放在 `.cl` 和窄 C++ encoder 中，不放入泛型模板头。

### 3.2 OpenClBackend Traits

OpenClBackend 必须提供：

- move-only `Buffer` 和 `Texture2D` 包装；
- `OpenClCommandContext`；
- `cl_mem` 创建、retain、释放和字节统计；
- buffer range 上传与下载；
- RGBA32F/R32F image 上传与下载；
- R8 image dirty rectangle 上传；
- image-to-image、buffer-to-image 和 image-to-buffer copy；
- submission id、完成 id、最终 event 和 busy 查询；
- program build、kernel create、dispatch、upload、download、flush、wait 计数；
- device/context/queue、内存限制和 image format 能力访问；
- 按内容 key 保存 LUT 资源；
- 对当前计划进行 program/kernel warm-up。

`OpenClBackend` 必须使用 `OpenClContext::Instance()` 已选择的同一 device、context 和产品 queue。
不得为 workspace、RAW、scope 或 present 再创建第二个 context。开发 profiling queue 只允许测试和
性能 harness 显式安装，不得替换产品语义。

### 3.3 OpenClCommandContext 与 event 链

OpenCL 1.2 使用现有 in-order product queue。每次 render 的命令边界是：

```text
BeginRender
  -> 等待上一份 render final event
  -> 清理已完成 submission 的 event/lease
  -> 取得新的 submission id
  -> 上传 dirty 参数和输入 dirty range
  -> 按 ExecutionPlan 入队 kernel/copy
  -> 必要时执行共享 PlanExecutor 定义的 Develop scratch 生命周期同步点
  -> 入队 present/scope copy 或绑定最终 image
  -> enqueue final marker event
  -> clFlush
EndRender
```

要求：

- Pass 不得调用 `clFinish`、创建 queue 或等待自己刚入队的 kernel；
- Pass 只向当前 queue 入队，并把需要的 dependency event 交给 CommandContext；
- `Submit` 使用最终 marker event 表示本帧完成，不用 `clFinish` 模拟提交；
- `Wait` 只等待上一份 in-flight render 的最终 event；
- 共享 PlanExecutor 已定义的 Develop scratch 释放同步点可以等待当前队列到该边界完成，但不得
  由 RAW operator 私自增加更多 wait；
- explicit download 可以等待对应 copy event；产品 present 不做 host wait；
- encode 失败时释放当前帧创建的 event，丢弃 unpublished result，不推进 completed submission；
- event retain/release 必须计数并有泄漏测试。

### 3.4 统一资源分配

OpenCL 资源必须通过 RenderDevice 所拥有的 workspace 或 OpenClSharedGpuResources 创建。

| 资源 | 唯一所有者 | OpenCL 存储策略 |
| --- | --- | --- |
| 参数 | ParameterArena | 单一 grow-only `cl_mem` buffer，dirty range 写入 |
| 临时数据 | TransientBufferArena | grow-only `cl_mem` slab + 对齐 subrange |
| 图像结果 | TexturePool | RGBA32F/R32F `image2d`，按完整 descriptor 复用 |
| 节点结果 | GraphImageCache | TexturePool lease + 内容 key |
| Raster mask | MaskTextureCache | R8 `image2d` level 集合 |
| mask signed distance | workspace value/cache | R32F image/buffer + 内容 key |
| LUT | OpenClSharedGpuResources | image/buffer + 内容 key + 字节预算 |
| program | OpenClProgramLibrary | program name + source compilation unit |
| kernel | OpenClKernelCache | program name + kernel name |
| Neural 权重/激活 | OpenClSharedGpuResources/workspace | 权重安全复用，激活按 device/session 所有 |

要求：

- OpenCL 1.2 没有可移植的 mipmapped image 写入路径，mask 每一级使用独立 `image2d`；
- TexturePool 只复用完整匹配的 extent、format、flags 和 storage kind；
- active lease 和未完成 submission 使用的资源不可淘汰；
- plan 预热后，稳定 render 不创建或释放 buffer、image、program 或 kernel；
- operator、PassEncoder 和旧 `OpenClImage` wrapper 不拥有第二套 pool、LRU 或长期 scratch；
- 预算同时受 app 配置、`CL_DEVICE_GLOBAL_MEM_SIZE` 和
  `CL_DEVICE_MAX_MEM_ALLOC_SIZE` 限制，并为 viewer/scope/RAW 权重保留余量；
- 分配复用与内容命中使用不同计数，不能用同一个 `cl_mem` 句柄证明结果有效。

### 3.5 图像表示与产品互操作

DAG workspace 的持久图像结果使用 `Texture2D` 包装的 OpenCL image。RAW kernel 如果继续使用
线性 buffer，其 buffer 只属于当前 Develop pass 的 workspace transient；Pack pass 必须把结果
写入 `develop.sensor_linear` 对应 RGBA32F image。

产品 present 必须：

- 通过 `IFrameSink::MapResourceForWrite(FrameMemoryDomain::OpenClDevice)` 请求兼容目标；
- 对 `OpenClImage` 目标使用 OpenCL/OpenGL 或 OpenCL/D3D11 sharing；
- 在同一 product queue 上 acquire、copy、release；
- 用当前 render 的最终 event/signal 提交 `FinalDisplayFrameView`；
- 不下载到 host；
- sink 不提供兼容 OpenCL target 时明确失败。

scope 必须读取同一最终 DRT `cl_mem` image 和同一完成 event。现有只接受 linear buffer 的
OpenCL scope resource 必须扩展为明确的 image2d 输入，不得为 scope 建立隐式 host round-trip。

## 4. Phase 总表

| Phase | 主题 | 强制输出 |
| --- | --- | --- |
| O0 | OpenClBackend、workspace、program/kernel warm-up | 模板实例、统一资源、计划级 registry 路径 |
| O1 | Develop、Geometry、CameraColor | `develop.sensor_linear`、`geometry.scene_source`、`develop.image` |
| O2 | Primary Grade 融合路径 | ParameterArena、pointwise fusion、detail/LUT |
| O3 | LLF workspace 化 | canonical reference、ROI sampling、无私有 allocator/cache |
| O4 | Mask、Feather、Mix | R8 level 集合、距离场和统一 Normal Mix |
| O5 | DRT、scope、present 与产品切换 | OpenCL DAG 端到端产品路径 |
| O6 | 旧 OpenCL 管线删除与性能验收 | 产品路径无 PipelineStage/旧 fused 执行 |

每个 Phase 必须同时交付行为测试、资源测试和性能快照。不能先提交逐帧分配、逐 kernel
`clFinish`、逐算子 program build、总参数 H2D 或 host present，再把修复留给 O6。

## 5. Phase O0 — OpenClBackend、workspace 与 program 预热

目标：

- 实现完整 OpenClBackend Traits；
- 实例化 `Renderer<OpenClBackend>`、`BasicRenderDevice<OpenClBackend>` 和
  `BasicRenderWorkspace<OpenClBackend>`；
- 从第一份可执行代码起统一 buffer、image、event、program 和 kernel 生命周期；
- 保持 CUDA/Metal 当前图、缓存 key 和执行集不变。

工作：

1. 新增 `edit/runtime/opencl/opencl_backend.hpp/.cpp` 和 OpenClCommandContext。
2. 新增 move-only Buffer、Texture2D；禁止隐式 `cl_mem` copy ownership。
3. 实现 ParameterArena dirty range 写入和失败后的 dirty 恢复。
4. 实现 TransientBufferArena、TexturePool、MaskTextureCache、GraphImageCache、
   NodeResultCache 的 OpenCL 实例。
5. 实现 submission id、final marker event、completed submission 和 `IsResourceBusy`。
6. 扩展 OpenClApiCounters，覆盖 image create/release、event create/release、program build、kernel
   create、dispatch、H2D/D2H、flush 和 wait。
7. 新增 OpenClKernelCache；key 为稳定 program name + kernel name；cache 拥有 kernel 生命周期。
8. 新增 `RegisterOpenClGpuDagPrograms()` manifest。manifest 只描述编译单元，不创建 renderer、
   workspace、buffer、image 或 kernel。
9. 在 OpenClBackend::WarmUpPlan 中按 ExecutionPlan 的 pass 集合取得 program/kernel。
10. 保留 RAW、scope 和 DemosaicNet 的现有 module manifest；不得从 PassEncoder 直接调用
    `OpenClProgramLibrary::RegisterProgram()`。
11. 为安装后的 `.cl` 资源建立明确定位方式；测试不得依赖源码树绝对路径偶然存在。
12. 使用 `OpenClContext` 的同一 context/device/product queue；不创建私有 queue。
13. 增加 compile-only header hygiene 测试，普通共享 runtime 头不得泄漏 OpenCL header。

建议 DAG program 分组：

```text
opencl_dag_geometry_camera
opencl_dag_primary_grade
opencl_dag_local_tone
opencl_dag_mask
opencl_dag_drt
```

RAW linearize、RCD、X-Trans、highlight 和 DemosaicNet 继续由现有 RAW/DemosaicNet manifest
拥有。O1 可以增加 DAG 所需的 RAW pack entrypoint，但 program ownership 仍在 RAW 模块。

测试：

```text
RendererTemplateInstantiatesOpenClWithoutCudaOrMetalHeaders
OpenClBackendUsesThePreparedProcessContextAndProductQueue
OpenClParameterArenaUploadsOnlyDirtyRanges
OpenClTransientArenaRewindsWithoutAllocatingAnotherBuffer
OpenClTexturePoolReusesMatchingImages
OpenClTexturePoolDoesNotEvictBusySubmissionImages
OpenClMaskTextureCacheUsesOneWorkspaceByteBudget
OpenClPlanWarmUpBuildsOnlyRequiredProgramsAndKernels
OpenClSecondEmptyRenderCreatesNoBufferImageProgramOrKernel
OpenClFailedUploadRestoresDirtyFieldsAndPublishesNoResult
OpenClProgramManifestCanLoadFromInstalledResourceLayout
OpenClCommandContextReleasesEveryRetainedEvent
```

性能门槛：

- 第二次相同计划的 buffer/image/program/kernel create 和 release 都为零；
- 无 dirty 参数时 ParameterArena 上传字节为零；
- 除共享 Develop scratch 生命周期同步点外，不存在 pass 级 wait；
- program、kernel、资源和 event 统计通过 API 快照读取，不依赖日志解析。

完成条件：

- `OpenClRenderer` 是 `Renderer<OpenClBackend>` 别名；
- OpenCL 可进入同一 source/plan/result cache 流程；
- DAG manifest 经 backend registry 激活；
- program/kernel 预热和错误 build log 可测试；
- 没有新增运行时 fallback。

##### Phase O0 completion record (2026-08-27)

**Status:** complete — `OpenClBackend` Traits, `Renderer<OpenClBackend>` / workspace instantiation, process-wide kernel cache, `gpu_dag` program manifest, and plan-level warm-up.

**Date:** 2026-08-27
**Branch:** `feature/gpu-dag-opencl`
**Commit:** `8da2d330`

**Primary success call chain:**

```text
OpenClRenderer / OpenClRenderDevice construction
  -> OpenClBackend (OpenClContext device/context/product queue, capability check)
  -> BasicRenderWorkspace<OpenClBackend> (ParameterArena, TransientBufferArena,
     TexturePool, MaskTextureCache, GraphImageCache, NodeResultCache)
  -> RegisterOpenClBackendPrograms
       -> RegisterOpenClGpuDagPrograms (manifest only)
  -> OpenClBackend::WarmUpPlan(ExecutionPlan)
       -> OpenClProgramLibrary::GetProgram (required compilation units only)
       -> OpenClKernelCache::GetKernel (program name + kernel name)
  -> BeginRender / UploadDirty / EndRender (marker event + clFlush)
  -> Wait (final event, event release, completed submission)
```

**Primary failure call chain:**

```text
FailNextUpload / program build / missing kernel / missing image format
  -> throw std::runtime_error (original OpenCL status or build log)
  -> PendingParameterPatch restores Model dirty bits
  -> CancelRender discards unpublished writes, does not publish content keys
  -> no CPU, CUDA, Metal, or old OpenCLGPUPipeline substitute
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RendererTemplateInstantiatesOpenClWithoutCudaOrMetalHeaders` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClBackendUsesThePreparedProcessContextAndProductQueue` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClParameterArenaUploadsOnlyDirtyRanges` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClTransientArenaRewindsWithoutAllocatingAnotherBuffer` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClTexturePoolReusesMatchingImages` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClTexturePoolDoesNotEvictBusySubmissionImages` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClMaskTextureCacheUsesOneWorkspaceByteBudget` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClPlanWarmUpBuildsOnlyRequiredProgramsAndKernels` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClSecondEmptyRenderCreatesNoBufferImageProgramOrKernel` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClFailedUploadRestoresDirtyFieldsAndPublishesNoResult` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClProgramManifestCanLoadFromInstalledResourceLayout` | `GpuDagOpenClWorkspaceTest` | PASS |
| `OpenClCommandContextReleasesEveryRetainedEvent` | `GpuDagOpenClWorkspaceTest` | PASS |

Commands:

- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagOpenClWorkspaceTest`
- `ctest --test-dir build/debug -R GpuDagOpenClWorkspaceTest --output-on-failure` → 12/12

Suite totals: 12/12 GpuDagOpenClWorkspaceTest.

**Checklist / exit condition:** all O0 exit conditions met. `OpenClRenderer` is `Renderer<OpenClBackend>`. Session source/plan/result caches construct. `gpu_dag` manifest is activated through `RegisterOpenClBackendPrograms`. Warm-up builds only requested programs/kernels. Failed upload restores dirty fields and publishes no result. No runtime fallback was added.

**LOC note (grill-code-review):** `opencl_backend.hpp` 315, `opencl_backend.cpp` 776 (under 1000). `opencl_workspace_test.cpp` 489. `OpenClKernelCache` is a separate process-wide module (81+84 LOC). No changed production file crossed 1000 LOC.

**Implemented:**

- `edit/runtime/opencl/opencl_backend.hpp/.cpp` and `OpenClCommandContext`
- move-only `Buffer` / `Texture2D` owning `cl_mem`
- `OpenClKernelCache` (program name + kernel name)
- `RegisterOpenClGpuDagPrograms()` manifest (`opencl_dag_geometry_camera`, `opencl_dag_primary_grade`, `opencl_dag_local_tone`, `opencl_dag_mask`, `opencl_dag_drt`)
- OpenClApiCounters fields for image/event/flush
- installed-layout `.cl` resolution via existing `exe/opencl/<relative>` search plus CMake install of DAG shaders

**Deleted:** none

**Program evidence:**

- manifests/programs: `gpu_dag` with five compilation units, `required_at_startup = false`
- build/create/hit: Geometry-only `WarmUpPlan` builds `opencl_dag_geometry_camera` and the resample kernel; grade/tone/mask/drt stay unbuilt; second warm-up is a kernel cache hit
- missing source/build/kernel errors: installed-layout probe loads a program whose source-tree path does not exist; `OpenClProgramLibrary` still reports build failures with device/driver/options (existing library test)

**Resource evidence:**

- buffer create/release: second empty render buffer create/release = 0
- image create/release: second empty render image create = 0; matching TexturePool reuse keeps ResourceId
- event retain/release: create_event == release_event after WaitIdle
- upload/download ranges and bytes: dirty Amount upload is 4 bytes; second `UploadDirty` is 0 bytes
- waits/flushes: Submit uses marker + `clFlush`; Wait uses `clWaitForEvents` on the final marker; no `clFinish` on the product path

**Performance evidence:**

- device/driver/OpenCL C/Windows/build: Windows MSVC `win_debug`; OpenCL GPU selected by `OpenClContext::Initialize`
- fixture and render request: empty/workspace renders on `OpenClRenderDevice` after Geometry-only warm-up
- cold: first render allocates parameters, transient slab, textures, mask chain, and one value buffer
- warm median / p95: not a timed harness; second identical empty render create/release of buffer, image, program, and kernel is zero

**Residual gaps:** PassEncoder specializations, Develop/Geometry/CameraColor kernels, and product present/scope are O1–O5. O0 stub `.cl` files compile and warm up; they are not CUDA pixel references. Device name/driver string was not written to a separate performance JSON.

**Remaining work owned by the next named Phase:** O1 — Develop, Geometry, and CameraColor encode-only passes using this backend, workspace, and RAW-owned programs.

## 6. Phase O1 — Develop、Geometry 与 CameraColor

目标：

- 完整移植 CUDA DAG Develop 顺序；
- 保持三个独立内容缓存边界；
- 不调用 RawProcessor 的整体 OpenCL 产品路径；
- RAW kernel 继续由 RAW module manifest 所有。

固定图像流：

```text
PreparedRawInput
  -> UploadRaw/UploadRgb
  -> Linearize
  -> optional CfaClamp
  -> Demosaic
  -> HighlightRecover
  -> InverseCamMulPack
  -> Lens
  -> develop.sensor_linear
  -> GeometryResample
  -> geometry.scene_source
  -> CameraToAp1 + ACEScc encode
  -> develop.image
```

工作：

1. 将现有 RAW OpenCL kernel 暴露为 encode-only entrypoint，参数包括当前 CommandContext、
   workspace buffer/image 和不可变参数。
2. RCD、X-Trans 和 Neural 路径不再拥有私有 queue、长期 scratch、内部 `clFinish` 或第二份
   activation pool。
3. RAW 专用 `.cl` 继续放在 `decoders/processor/operators/gpu/opencl_shader/`，manifest 继续放在
   RAW module；不得把 RAW program 名移动到 edit runtime。
4. Pack pass 将 buffer 结果写入 workspace RGBA32F image；不得让旧 `OpenClImage` 成为
   GraphImageCache 所有者。
5. Geometry 使用 DAG OpenCL 编译单元；crop、rotation、viewport 和 dynamic resolution 只执行
   一次重采样。
6. Lens、Geometry 和 CameraColor 使用同一 product queue 和当前 event 链。
7. CameraColor 读取共享 DevelopColorTransform 结果，不在 OpenCL 端重新解释 CameraMatrices。
8. CCT/tint 改变只能让 CameraColor 及下游失效。
9. source、sensor、geometry 和 develop 内容 key 与 CUDA/Metal 使用同一构造函数。
10. Neural model failure 直接失败，不选择其他 demosaic。
11. 计划 warm-up 包含所选 demosaic 和 Geometry/CameraColor kernel；稳定帧不创建 kernel。

测试：

```text
OpenClDevelopLinearizeMatchesCudaReferenceWithinTolerance
OpenClDevelopRcdOrderMatchesCudaDemosaicThenHighlightRecovery
OpenClDevelopXTransMatchesCudaReferenceWithinTolerance
OpenClDevelopNeuralUsesSessionWorkspaceAndDoesNotSelectAnotherDemosaicOnFailure
OpenClGeometryUsesOneResampleForCropRotationViewportAndScale
OpenClCameraColorConsumesSharedDualIlluminantTransform
OpenClCctEditReusesSensorAndGeometryResults
OpenClSecondDevelopRenderRunsNoSourceUploadOrDevelopPass
OpenClDevelopPassesUseOneQueueAndOneRenderEventChain
OpenClMissingRawProgramReturnsItsBuildOrLookupError
```

完成条件：

- 三个 GraphValueId 分别拥有内容 key；
- Develop 稳定重绘不分配 OpenCL 资源；
- RAW OpenCL operator 不保存长期 scratch 或私有 kernel cache；
- 设备、program、kernel 和 event 错误直接失败。

##### Phase O1 completion record (2026-08-28)

**Status:** complete — encode-only OpenCL Develop, GeometryResample, and CameraColor on `Renderer<OpenClBackend>` with CUDA order and the three content-key boundaries.

**Date:** 2026-08-28
**Branch:** `feature/gpu-dag-opencl`
**Commit:** `05a29c4b`

**Primary success call chain:**

```text
OpenClRenderDevice::Execute / PassEncoder<OpenClBackend, UploadRaw|UploadRgb>
  -> ExecuteOpenClDevelop
       -> transient U16 upload + EncodeToLinearRef (+ optional EncodeCfaClamp01)
       -> EncodeBayerRcd | EncodeXTrans | EncodeNeural (session ActivationSlots)
       -> pack/copy to workspace RGBA32F image
       -> optional warp_rectilinear_rgba32f
  -> ExecuteOpenClGeometryResample (alias or one geometry_resample_rgba32f)
  -> ExecuteOpenClCameraColor (shared DevelopColorTransform -> ParameterArena -> camera_color_acescc)
  -> identity CopyTexture2D for PrimaryGrade and DRT until O2/O5
  -> EndRender marker + clFlush; Wait releases events
```

**Primary failure call chain:**

```text
Neural model load / missing RAW program or kernel / OpenCL enqueue error
  -> throw std::runtime_error (original status, program name, kernel name, or Neural Engine error)
  -> CancelRender discards unpublished writes, publishes no content key
  -> no Legacy/RCD/CPU/CUDA/Metal/old OpenCLGPUPipeline substitute
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OpenClDevelopLinearizeMatchesCudaReferenceWithinTolerance` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClDevelopRcdOrderMatchesCudaDemosaicThenHighlightRecovery` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClDevelopXTransMatchesCudaReferenceWithinTolerance` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClDevelopNeuralUsesSessionWorkspaceAndDoesNotSelectAnotherDemosaicOnFailure` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClGeometryUsesOneResampleForCropRotationViewportAndScale` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClCameraColorConsumesSharedDualIlluminantTransform` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClCctEditReusesSensorAndGeometryResults` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClSecondDevelopRenderRunsNoSourceUploadOrDevelopPass` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClDevelopPassesUseOneQueueAndOneRenderEventChain` | `GpuDagOpenClDevelopTest` | PASS |
| `OpenClMissingRawProgramReturnsItsBuildOrLookupError` | `GpuDagOpenClDevelopTest` | PASS |

Commands:

- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagOpenClDevelopTest --target GpuDagOpenClWorkspaceTest`
- `ctest --test-dir build/debug -R GpuDagOpenClDevelopTest --output-on-failure` → 10/10
- `ctest --test-dir build/debug -R GpuDagOpenClWorkspaceTest --output-on-failure` → 12/12 (O0 still green)

Suite totals: 10/10 GpuDagOpenClDevelopTest; 12/12 GpuDagOpenClWorkspaceTest.

**Checklist / exit condition:** all O1 exit conditions met. `develop.sensor_linear`, `geometry.scene_source`, and `develop.image` have independent content keys (CCT edit skips SensorDevelop and Geometry). Stable identical Develop render creates no buffer/image/program/kernel. RAW encode-only entrypoints take the product queue and workspace views; they do not own a queue or call `clFinish`. Missing program/kernel and Neural load failure throw and publish no result.

**LOC note (grill-code-review):** `opencl_encode.cpp` 485, `opencl_develop_pass.cpp` 528, `opencl_backend.cpp` 770, `opencl_develop_test.cpp` 525. No new production file crossed 1000 LOC.

**Implemented:**

- RAW encode-only API (`opencl_encode.hpp/.cpp`) using `OpenClKernelCache`, product queue, and buffer-offset views
- `ExecuteOpenClDevelop` / `ExecuteOpenClGeometryResample` / `ExecuteOpenClCameraColor` and PassEncoder specializations
- DAG `geometry_resample_rgba32f`, `camera_color_acescc`, and `warp_rectilinear_rgba32f` in `geometry_camera.cl`
- RAW pack/clamp/HLR-from-stats kernels; program/kernel names in `opencl_raw_programs.hpp`
- `WarmUpPlan` builds selected RAW kernels for the compiled CFA kind plus Geometry/CameraColor
- Identity PrimaryGrade and DRT copies so PlanExecutor cache-skip tests can finish

**Deleted:** none of the old OpenCL product wrappers. They remain until O6. The DAG path does not call them.

**Program evidence:**

- RAW programs stay in the `raw_processor` manifest; DAG warp/geometry/camera stay in `opencl_dag_geometry_camera`
- Bayer warm-up builds linearize/clamp/RCD/pack/HLR; X-Trans builds xtrans instead of RCD
- Missing kernel/program lookup returns program name plus kernel name or "not registered"

**Resource evidence:**

- buffer/image/program/kernel create: second identical Develop render 0 (`OpenClSecondDevelopRenderRunsNoSourceUploadOrDevelopPass`)
- CCT edit: `source_h2d_count == 0`, SensorDevelop/Geometry skip, CameraColor execute 1, kernel create 0
- event create == event release after Wait; product queue is `OpenClContext::Instance().Queue()`
- no DAG `clFinish`; HLR chrominance stays on device via `hlr_reconstruct_from_stats`

**Performance evidence:**

- device/driver/OpenCL C/Windows/build: Windows MSVC `win_debug`; OpenCL GPU selected by `OpenClContext::Initialize`
- fixture: synthetic 64×64 Bayer / 64×64 X-Trans / 16×12 Direct RGB; `OpenClRenderDevice::Execute` after plan warm-up
- cold: first reserve + first render allocates transients, textures, and kernels
- warm: second identical Develop render GPU resource/program/kernel create = 0; product A/B remains O6

**Residual gaps:** PrimaryGrade and DRT OpenCL encoders copy `develop.image` until O2/O5. Old RAW OpenCL wrappers still own `clFinish` and `OpenClImage` scratch for the pre-DAG product path. Neural success pixels are not asserted; load failure is. Product present/download remains unimplemented until O5.

**Remaining work owned by the next named Phase:** O2 — Primary Grade fusion (replace identity copy), ParameterArena slider dirty ranges, LUT image cache.

## 7. Phase O2 — Primary Grade 融合路径

目标：

- 移植默认 Grade 调整列表；
- 保留调整顺序；
- pointwise 调整使用融合 dispatch；
- 参数只通过 ParameterArena 更新。

工作：

1. 使用共享 `AdjustmentBehavior`、`GradeAdjustmentParams` 和 `MakeGradeRuntimeParams`；不建立
   OpenCL 版本的调整语义表。
2. GraphCompiler 继续生成共享调整顺序、ParameterArena binding 和 primary grade stages。
3. CAT02、Exposure、Contrast、White、Black、Curve、HLS、Saturation、Vibrance、Color Wheel
   和 LMT 在保持顺序的前提下融合。
4. Shadows/Highlights 在 LLF 边界前后拆成最多两个 pointwise dispatch。
5. Clarity、Sharpen、Halation 和 Film Grain 生成明确 neighbor pass，所有中间 image/buffer 来自
   workspace。
6. command offset 数据只在拓扑改变时上传；slider dirty Patch 不重编译计划。
7. LUT 由 OpenClSharedGpuResources 按内容 key 和字节预算保存；删除 DAG 路径中的静态 LUT
   map 和每次 dummy LUT buffer 创建。
8. 未使用 LUT 时使用预热创建的长期 dummy 资源或可空 kernel 变体；不得逐帧创建 fallback
   LUT buffer。
9. 普通 pointwise 调整不得一项一个 program、kernel object 或完整图像往返。

测试：

```text
OpenClPrimaryGradePreservesCompiledAdjustmentOrder
OpenClPointwiseAdjustmentsUseOneDispatchPerLlfSegment
OpenClSingleSliderEditUploadsOnlyItsParameterRange
OpenClExposureEditRunsOnlyPrimaryGradeAndDrt
OpenClLutResourceIsReusedByContentKey
OpenClDetailPassesAcquireAllResourcesFromWorkspace
OpenClPrimaryGradeMatchesCudaReferenceWithinTolerance
OpenClUnknownAdjustmentReturnsExplicitBackendError
OpenClStableGradeCreatesNoDummyLutOrStageParameterBuffer
```

性能门槛：

- 不允许一个 pointwise adjustment 对应一次 queue wait 或一次完整图像往返；
- 相同拓扑的 slider 编辑不创建 buffer、image、program 或 kernel；
- Grade command offset buffer 只在拓扑改变时更新；
- LUT 未变化时上传字节为零。

完成条件：

- OpenCL 与 CUDA/Metal 使用同一调整顺序和参数 slot；
- 默认值不改变图结构；
- fused、detail 和 LUT 失败直接失败；
- DAG Grade 不引用 `OpenClFusedParams` 或 `OperatorParams`。

##### Phase O2 completion record (2026-08-28)

**Status:** complete — OpenCL Primary Grade now encodes the shared adjustment order through one
workspace-backed DAG pass with fused pointwise, explicit detail, LUT, mix, and LLF-boundary stages.

**Date:** 2026-08-28
**Branch:** `feature/gpu-dag-opencl`
**Commit:** `350cd643`

**Primary success call chain:**

```text
Renderer<OpenClBackend>::Execute / PlanExecutor<OpenClBackend>::Execute
  -> OpenClBackend::WarmUpPlan
       -> opencl_dag_primary_grade / CL1.2 pointwise, detail, mix, and optional masked-mix kernels
       -> long-lived OpenClBackend dummy LUT resource
  -> BeginRender assigns the product-queue submission id
  -> PassEncoder<OpenClBackend, PrimaryColorGrade>
       -> ExecuteOpenClPrimaryGrade
            -> shared GraphCompiler order and AdjustmentBehavior resolution
            -> MakeGradeRuntimeParams -> ParameterArena slot/binding and dirty-range upload
            -> compact Fused / Detail / LlfBarrier operations at the compiled order boundaries
            -> NodeResultCache grade.primary:runtime.order command-offset upload on topology change
            -> TexturePool / GraphImageCache leases for ping-pong and final RGBA32F images
            -> primary_grade_pointwise_rgba32f or primary_grade_detail_rgba32f
            -> explicit local-tone identity barrier until O3
            -> primary_grade_mix_rgba32f or primary_grade_mix_masked_rgba32f
  -> PlanExecutor records the primary-grade content key
  -> existing DRT identity copy
  -> EndRender marker + clFlush; Wait releases the product-queue events
```

**Primary failure call chain:**

```text
Unknown adjustment / unsupported DTO / missing graph image / invalid LUT cube /
  command or parameter upload / program, kernel, argument, or OpenCL enqueue error
  -> throw std::runtime_error with the operation and original resource/error context
  -> BasicRenderDevice::CancelRender waits recorded work
       -> ReleaseUnsubmittedResourceUses clears cancelled LUT busy markers
       -> BasicRenderWorkspace discards unpublished image writes
  -> PlanExecutor reports the error and publishes no new content key
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OpenClPrimaryGradePreservesCompiledAdjustmentOrder` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClPointwiseAdjustmentsUseOneDispatchPerLlfSegment` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClSingleSliderEditUploadsOnlyItsParameterRange` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClExposureEditRunsOnlyPrimaryGradeAndDrt` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLutResourceIsReusedByContentKey` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClDetailPassesAcquireAllResourcesFromWorkspace` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClPrimaryGradeMatchesCudaReferenceWithinTolerance` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClUnknownAdjustmentReturnsExplicitBackendError` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClStableGradeCreatesNoDummyLutOrStageParameterBuffer` | `GpuDagOpenClGradeTest` | PASS |

Commands:

- `cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagOpenClGradeTest --target GpuDagOpenClWorkspaceTest --target GpuDagOpenClDevelopTest`
- `ctest --test-dir build/debug -R "GpuDagOpenCl(Workspace|Develop|Grade)Test" --output-on-failure` → 31/31
- `cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagCudaPrimaryGradeTest`
- `ctest --test-dir build/debug -R "GpuDagCudaPrimaryGradeTest" --output-on-failure` → 15/15

Suite totals: 9/9 required O2 tests; 31/31 OpenCL O0–O2 tests; 15/15 CUDA Primary Grade
regression tests.

**Checklist / exit condition:** all O2 exit conditions met. OpenCL and CUDA/Metal use the shared
adjustment behavior list, runtime parameter layout, compiled order, and ParameterArena slot model.
Default values retain the existing graph shape. CAT02, Exposure, Contrast, White, Black, Curve,
HLS, Saturation, Vibrance, Color Wheel, and LMT are traversed by the fused pointwise kernel;
Clarity, Sharpen, Halation, and Film Grain have explicit workspace-backed detail passes. Shadows
and Highlights form the explicit LLF boundary and are split into the surrounding fused segments.
Command offsets stay in the node-result workspace and are uploaded only for a new or changed
topology. LUT buffers are keyed by packed content and edge size, respect the shared byte budget,
and do not evict resources used by an incomplete submission. After warm-up, stable grade edits
create no buffer, image, program, or kernel; the slider test observes only the changed parameter
range. The DAG Grade path contains no `OpenClFusedParams` or `OperatorParams` references.

**LOC note (grill-code-review):** `opencl_primary_grade_pass.cpp` 510,
`opencl_primary_grade_pass.hpp` 42, `primary_grade.cl` 275, `opencl_grade_test.cpp` 426,
`opencl_backend.cpp` 897. No new production file crossed 1000 LOC.

**Implemented:**

- OpenCL Primary Grade encoder and `PassEncoder` specialization using the shared ExecutionPlan,
  `AdjustmentBehavior`, `GradeAdjustmentParams`, `MakeGradeRuntimeParams`, ParameterArena, and
  workspace image/value caches.
- CL1.2 primary-grade compilation unit with one program and pointwise/detail/mix kernel variants;
  fused pointwise operations preserve compiled order and avoid one program/kernel per adjustment.
- Explicit detail-pass resource boundaries for Clarity, Sharpen, Halation, and Film Grain, plus
  the LLF ordering barrier required by the compiled Shadows/Highlights stages.
- Long-lived dummy LUT warm-up, content-key LUT reuse, byte-budget LRU eviction that preserves
  busy resources, and cancellation cleanup for resources acquired by an unsubmitted encode.
- Command-offset topology storage in `NodeResultCache`; slider changes patch only their bound
  `GradeAdjustmentParams` range and do not compile a new plan.
- Nine exact O2 behavior/regression tests and CMake registration for `GpuDagOpenClGradeTest`.

**Deleted:** none of the old OpenCL product wrappers. They remain outside the DAG path until O6.

**Program evidence:**

- `opencl_dag_primary_grade` remains one registered manifest program with `-cl-std=CL1.2`.
- `WarmUpPlan` builds only the Primary Grade kernels required by the compiled plan, including the
  masked mix variant only when a compiled grade mask exists, and creates the dummy LUT once.
- The stable edit test observes zero program builds and kernel creates after warm-up; missing or
  invalid adjustment/LUT resources return explicit errors.

**Resource evidence:**

- ParameterArena owns one aligned grade parameter buffer; a single Exposure edit uploads its
  complete bound field range and leaves the model clean after commit.
- `grade.primary:runtime.order` is a workspace node buffer; the stable edit uploads zero command
  bytes because its topology is unchanged, while a recreated command buffer forces a fresh upload.
- Ping-pong images and detail intermediates come from `GraphImageCache` and `TexturePool`; the
  detail test observes four detail passes with zero second-render texture/buffer/program/kernel
  creation.
- The backend-owned long-lived LUT store reuses the same resource for unchanged packed content,
  uploads zero bytes on a hit, applies the byte budget, and skips busy entries during eviction.
- All Primary Grade kernels enqueue on the product OpenCL queue; `EndRender` adds the final marker
  and flush, and `Wait` releases retained events. The existing OpenCL workspace event regression
  remains green.

**Performance evidence:**

- device/driver/OpenCL C/Windows/build: OpenCL GPU selected by `OpenClContext::Initialize`,
  Windows MSVC `win_debug`, CL1.2 program build, OpenCL runtime execution on the configured GPU;
  CUDA cross-target check also passed in the same build tree.
- fixture and render request: direct 16×12 RGBA32F input for the O2 kernel/reference tests and
  the existing OpenCL workspace/Develop fixtures; normal `Renderer<OpenClBackend>` execution for
  the cache-boundary test.
- cold: Primary Grade program/kernel warm-up and first workspace resource acquisition are explicit
  and counted; the dummy LUT is created during warm-up instead of the encode path.
- warm median: not measured by the O2 behavior suite; the stable render assertions are zero
  resource/program/kernel creation after warm-up. Quantified warm median and p95 remain part of
  the O6 performance gate.
- warm p95: not measured by the O2 behavior suite; O6 owns the benchmark matrix and threshold.

**Remaining work owned by the next named Phase:** O3 — replace the explicit local-tone identity
barrier with the real LLF reference, pyramid, workspace-resource, and pixel-application path while
preserving the O2 fused-before/after ordering. O4 owns production mask/feather evaluation and
mix integration; O5 owns DRT, scope, device present, and product OpenCL switching; O6 owns old
OpenCL pipeline deletion and the final performance acceptance matrix.

## 8. Phase O3 — LLF workspace 化

目标：

- 对齐 CUDA canonical LLF reference 和 ROI sampling；
- DAG 路径不使用 `highlight_shadow_local_tone::OpenClStage`；
- pyramid、内容 key 和 event 生命周期全部进入 workspace。

工作：

1. 把 source/remap/sample/output pyramid 峰值写入 ExecutionPlan transient 需求。
2. 临时 pyramid buffer/image 全部从 TransientBufferArena 和 TexturePool 分配。
3. canonical reference 使用 workspace GraphValueId、内容 key 和 submission-safe lease。
4. ROI frame 使用 `MakeLlfSamplingPlan` 映射到 canonical reference。
5. 全图 reference 未建立时按 CUDA 当前规则计算，不伪造命中。
6. Shadows/Highlights 参数改变复用不依赖该参数的 canonical source reference。
7. 所有 LLF kernel 由 DAG local-tone program 编译单元和 OpenClKernelCache 提供。
8. Pass 内不得 `clFinish`；失败 submission 不发布 reference。
9. 旧 `OpenClStage` 可以在 O6 前继续服务尚未切换的旧产品路径，但 DAG 路径不得调用或持有它。

测试：

```text
OpenClLlfUsesWorkspaceTransientArenaForEveryPyramid
OpenClLlfFullFrameBuildsCanonicalReferenceOnce
OpenClLlfRoiSamplesCanonicalReferenceWithSharedGeometryPlan
OpenClLlfSliderEditReusesCanonicalReference
OpenClLlfFailedSubmissionDoesNotPublishReference
OpenClLlfSecondStableRenderCreatesNoBufferImageProgramOrKernel
OpenClLlfMatchesCudaReferenceWithinTolerance
OpenClLlfPassDoesNotFinishTheQueue
```

完成条件：

- canonical source/result 有稳定 GraphValueId 和内容 key；
- ROI 与 full-frame 使用同一数学和采样计划；
- LLF 没有私有 allocator、长期 scratch、private cache 或 queue wait；
- stable LLF render 的资源创建和 program/kernel 创建为零。

##### Phase O3 completion record (2026-08-28)

**Status:** complete — OpenCL DAG local tone now executes the real CUDA-aligned LLF reference,
pyramid, remap, collapse, sampling, and pixel-application path through the shared workspace.

**Date:** 2026-08-28
**Branch:** `feature/gpu-dag-opencl`
**Commit:** uncommitted working tree (no commit requested)

**Primary success call chain:**

```text
Renderer<OpenClBackend>::Execute / PlanExecutor<OpenClBackend>::Execute
  -> OpenClBackend::WarmUpPlan
       -> gpu_dag / CL1.2 local-tone compilation unit
       -> OpenClKernelCache entries for extract, reference-extract, pyramid-down, remap,
          select, collapse, and apply
  -> BeginRender assigns the product-queue submission id and resets the transient offset
  -> PassEncoder<OpenClBackend, PrimaryColorGrade>
       -> ExecuteOpenClPrimaryGrade
            -> HashLlfSourceKey and HashLlfReferenceKey
            -> ExecuteOpenClLocalTone
                 -> GraphImageCache local_tone.source.0/result.0 content-key lookup
                 -> TransientBufferArena source/remap-A/remap-B/result pyramid planes
                 -> local_tone_extract_reference for a full EditSpace frame, or local_tone_extract
                    for an isolated ROI
                 -> local_tone_pyr_down, local_tone_remap, local_tone_select, and
                    local_tone_collapse
                 -> MakeLlfSamplingPlan for canonical full-frame and ROI sampling
                 -> local_tone_apply on the same in-order product queue
                 -> GraphImageCache records canonical source/result writes as unpublished
  -> PlanExecutor records the primary-grade output key
  -> EndRender marker + clFlush; PublishResults publishes only the successful submission
```

**Primary failure call chain:**

```text
Invalid geometry/texture/key / transient allocation / program, kernel, argument, copy,
or OpenCL enqueue error
  -> throw std::runtime_error with the operation and original resource/error context
  -> BasicRenderDevice::CancelRender waits recorded work and discards unpublished image writes
  -> previously published canonical source/result identities remain available
  -> PlanExecutor reports the error and publishes no new canonical content key
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OpenClLlfUsesWorkspaceTransientArenaForEveryPyramid` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfFullFrameBuildsCanonicalReferenceOnce` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfRoiSamplesCanonicalReferenceWithSharedGeometryPlan` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfSliderEditReusesCanonicalReference` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfFailedSubmissionDoesNotPublishReference` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfSecondStableRenderCreatesNoBufferImageProgramOrKernel` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfMatchesCudaReferenceWithinTolerance` | `GpuDagOpenClGradeTest` | PASS |
| `OpenClLlfPassDoesNotFinishTheQueue` | `GpuDagOpenClGradeTest` | PASS |

Commands:

- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagOpenClGradeTest`
- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagOpenClWorkspaceTest GpuDagOpenClDevelopTest`
- `ctest --test-dir build/debug -R "GpuDagOpenClGradeTest" --output-on-failure` → 17/17
- `ctest --test-dir build/debug -R "GpuDagOpenCl(Workspace|Develop|Grade)Test" --output-on-failure` → 39/39

Suite totals: 8/8 required O3 tests; 17/17 GpuDagOpenClGradeTest tests; 39/39 combined OpenCL
O0–O3 tests.

**Checklist / exit condition:** all O3 exit conditions met. `GraphImageCache` owns stable
`local_tone.source.0` and `local_tone.result.0` identities, their source/reference content keys,
and the source long-edge descriptor needed to decide whether a full-frame resolution change
requires a rebuild. Full-frame and ROI frames use `MakeLlfSamplingPlan` from the shared resolved
geometry; an isolated ROI builds its own local reference and does not report a canonical hit.
The four LLF pyramid families use `TransientBufferArena` slab subranges, while canonical planes
use `GraphImageCache` leases backed by `TexturePool`. The local-tone pass has no private allocator,
long-lived scratch, or private result cache, and it does not wait for or finish the queue. The
identity barrier kernel was removed from the DAG path. A second stable LLF render creates zero
buffer, image, program, or kernel resources, and a failed submission leaves the previously
published canonical identities unchanged.

**LOC note (grill-code-review):** `opencl_local_tone_pass.cpp` 496,
`opencl_local_tone_pass.hpp` 34, `local_tone.cl` 332, `opencl_primary_grade_pass.cpp` 462,
`opencl_primary_grade_pass.hpp` 37, `opencl_backend.cpp` 829, `opencl_dag_programs.hpp` 39,
`graph_image_cache.hpp` 393, `opencl_grade_test.cpp` 939. No new production file crossed 1000
LOC.

**Implemented:**

- Replaced the OpenCL DAG local-tone identity copy with the CUDA-aligned ACEScc LLF math: source
  extraction, Gaussian pyramid construction, sampled remap selection, pyramid collapse, and
  RGB intensity application.
- Added seven centralized local-tone kernel names and a CL1.2 implementation in the DAG local-tone
  compilation unit. `WarmUpPlan` creates all required kernel objects before encode.
- Added `ExecuteOpenClLocalTone` with full-frame canonical reference creation, content-keyed source
  reuse across Shadows/Highlights edits, canonical ROI sampling, and isolated-ROI computation.
- Added GraphImageCache auxiliary metadata for the published canonical source resolution. The
  source and adjusted canonical images are recorded as unpublished until `PublishResults`.
- Routed all transient source/remap/sample/output planes through `TransientBufferArena`, including
  byte-offset views into one OpenCL slab, and kept all image ownership in `TexturePool` through
  `GraphImageCache`.
- Added the eight exact O3 behavior, failure, resource, queue, ROI, and pixel-tolerance tests;
  existing O0–O2 tests remain green.

**Deleted:** the `local_tone_llf_rgba32f` identity kernel and the DAG
`DispatchLocalToneBarrier` helper. The legacy `OpenClStage` remains outside the DAG path until
O6 as allowed by this phase plan.

**Program evidence:**

- The existing `gpu_dag` manifest local-tone descriptor now compiles the seven-kernel
  `local_tone.cl` unit with `-cl-std=CL1.2`; no per-frame program registration was added.
- `OpenClBackend::WarmUpPlan` obtains all local-tone kernel names from
  `opencl_dag_programs.hpp` and the process-wide `OpenClKernelCache`.
- The stable-render test observes zero program builds and kernel creates after warm-up; runtime
  execution created and used the real kernels on the configured OpenCL device.

**Resource evidence:**

- `GraphCompiler::EstimatePeakTransientBytes` includes `EstimateLlfTransientBytes`; the arena
  test verifies the plan reservation covers the four aligned pyramid families and the measured
  slab subranges.
- Canonical source/result images use stable graph IDs and content keys. The full-frame test
  confirms the second render samples the same source resource, and the slider test confirms the
  source key remains unchanged while the adjusted result key changes.
- The ROI test confirms the full-frame and viewport plans share the same geometry-derived LLF
  sampling function, while a separate device without a canonical result computes an isolated ROI
  reference instead of claiming a cache hit.
- The stable-render test observes zero buffer/image/program/kernel creation. The failed-submission
  test confirms cancellation preserves the previously published canonical source/result pair.
- Every copy and kernel event is retained by `OpenClCommandContext`; the queue test observes no
  additional wait while the local-tone pass is encoding. `EndRender` owns the marker/flush and
  `Wait` owns completion and event release.

**Performance evidence:**

- device/driver/OpenCL C/Windows/build: configured OpenCL device selected by `OpenClContext`,
  Windows MSVC `win_debug`, OpenCL C 1.2 source, and runtime execution of the real local-tone
  kernels. The exact driver string was not captured by this behavior suite.
- fixture and render request: direct RGBA32F input for the grade suite; the ROI test uses a 64×64
  full-frame render followed by a 32×64 visible viewport render and an isolated-device control.
  Pixel tolerance compares the OpenCL output with a CPU mirror of the CUDA LLF math.
- cold: first LLF render builds the local-tone program/kernel set during plan warm-up, allocates
  the workspace transient slab and canonical images, and records the canonical source/result.
- warm median: not measured by the O3 behavior suite; the stable-render test proves zero resource
  and program/kernel creation after warm-up. Quantified warm median and p95 remain part of O6.
- warm p95: not measured by the O3 behavior suite; O6 owns the benchmark matrix and threshold.

**Remaining work owned by the next named Phase:** O4 — production R8 mask, exact signed distance,
feather, and normal mix integration. O5 owns DRT, scope, present, and product OpenCL switching;
O6 owns old OpenCL pipeline deletion and the final performance acceptance matrix. The legacy
`OpenClStage` continues to serve only the pre-DAG product path and is not called or held by the
DAG local-tone implementation.

## 9. Phase O4 — Mask、Feather 与 Mix

目标：

- 完成 R8 Raster/Analytic Mask；
- 完成 exact signed Euclidean distance field；
- Grade 只存在一个 Normal Mix 出口；
- OpenCL 1.2 mask mip 使用独立 image2d level。

工作：

1. Raster mask 从 MaskStore 读取，上传到 workspace MaskTextureCache。
2. R8 dirty rectangle 只写入变化区域。
3. 每个 mip level 是 cache 拥有的独立 R8 image2d；mask pass 不持有 level。
4. signed distance 中间资源来自 transient arena；距离结果按 mask content key 保存。
5. feather radius 改变复用 signed distance 结果。
6. Analytic mask 和 Raster mask 都使用 ResolvedRenderGeometry/TextureSamplingPlan。
7. Mix kernel 读取原 Grade 输入、调整结果、grade mix 和可选 mask。
8. 未连接 mask 时使用常数 1，不创建虚假白色 image。
9. exact distance 的分带、合并、coverage 和 antialiased boundary 与 CUDA/Metal 相同；不得用近似
   blur 替代。
10. mask image format 或 device image 能力不足时明确失败。

测试：

```text
OpenClRasterMaskUploadsOnlyChangedR8Rectangle
OpenClRasterMaskLevelsUseWorkspaceCache
OpenClMaskFeatherMatchesExactSignedDistanceReference
OpenClFeatherRadiusEditReusesSignedDistanceResult
OpenClMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution
OpenClDisconnectedMaskUsesConstantOneWithoutImageAllocation
OpenClNormalMixMatchesCudaReferenceWithinTolerance
OpenClMaskCacheDoesNotEvictBusyImages
OpenClMaskLevelsUseSeparateOpenCl12Images
```

完成条件：

- raster dirty upload、level 复用和 busy 保护有计数证据；
- signed distance 内容 key 不包含 feather radius；
- disconnected mask 不增加 image 分配；
- mask、feather、mix 不调用旧 fused kernel。

##### Phase O4 completion record (2026-08-28)

**Status:** complete — OpenCL DAG now evaluates analytic and R8 raster masks, caches independent
mip images, computes exact signed distance and feathering, and sends one logical Normal Mix stage
through primary grade.

**Date:** 2026-08-28
**Branch:** `feature/gpu-dag-opencl`
**Commit:** uncommitted working tree (no commit requested)

**Primary success call chain:**

```text
OpenClRenderDevice::Execute / PlanExecutor<OpenClBackend>::Execute
  -> PassEncoder<OpenClBackend, GpuPassKind::MaskEvaluate>
       -> ExecuteOpenClMask
            -> GraphImageCache::AcquireImageForWrite(mask_output)
            -> analytic mask: mask_analytic_r8
               or raster mask: MaskStore::Load -> MaskTextureCache::Acquire
                  -> UploadTexture2D on cache miss, or UploadR8TextureRect for the dirty union
                  -> mask_generate_r8_mip for independent cached R8 image2d levels
                  -> mask_raster_sample_r8
               or raster feather: exact mask_band_horizontal/vertical
                  -> mask_compose_signed_distance -> mask_feather_sample
  -> ExecuteOpenClPrimaryGrade::DispatchMix
       -> one logical Normal Mix stage using the masked or unmasked CL entrypoint
            (original Grade input + adjusted result + grade mix * optional mask)
  -> PlanExecutor records unpublished results -> EndRender/clFlush -> PublishResults
```

**Primary failure call chain:**

```text
Unsupported R8/image capability, missing MaskStore, invalid mask asset, transient allocation,
program/kernel, argument, or OpenCL enqueue failure
  -> OpenClBackend capability validation or ExecuteOpenClMask throws with the operation context
  -> PlanExecutor catches -> BasicRenderDevice::CancelRender waits tracked work, releases
     unsubmitted resource uses, and discards unpublished image writes
  -> the error is reported and rethrown; previously published values remain and no CPU or
     alternate-backend substitute is selected
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OpenClRasterMaskUploadsOnlyChangedR8Rectangle` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClRasterMaskLevelsUseWorkspaceCache` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClMaskFeatherMatchesExactSignedDistanceReference` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClFeatherRadiusEditReusesSignedDistanceResult` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClDisconnectedMaskUsesConstantOneWithoutImageAllocation` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClNormalMixMatchesCudaReferenceWithinTolerance` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClMaskCacheDoesNotEvictBusyImages` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClMaskLevelsUseSeparateOpenCl12Images` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClGraduatedNdMaskFollowsReferenceSpaceNormal` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClRasterMaskRequiresMaskStoreAndReportsTheFailure` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |
| `OpenClPlanExecutorRunsRasterMaskBeforePrimaryGrade` | `GpuDagOpenClGradeTest_runtime/GpuDagOpenClGradeTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagOpenClGradeTest --parallel 4
.\build\debug\alcedo_studio\tests\edit\GpuDagOpenClGradeTest_runtime\GpuDagOpenClGradeTest.exe --gtest_filter='OpenClMaskFixture.*' --gtest_color=no
ctest --test-dir build\debug --output-on-failure -R "GpuDagOpenCl(Workspace|Grade)Test"
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagCudaMaskTest --parallel 4
.\build\debug\alcedo_studio\tests\edit\GpuDagCudaMaskTest_runtime\GpuDagCudaMaskTest.exe --gtest_color=no
```

Suite totals: 12/12 focused OpenCL mask tests; 41/41 registered OpenCL workspace and grade tests;
11/11 CUDA mask tests. The focused OpenCL suite includes all nine required O4 names, the
graduated analytic and explicit MaskStore-failure checks, and the real PlanExecutor ordering test.

**Checklist / exit condition:** all O4 exit conditions met.

- [x] Dirty raster upload records the union rectangle and only its packed R8 bytes; the test
  observes one `{1,1,4,4}` upload and 16 host-to-device bytes.
- [x] Workspace cache reuse, independent CL1.2 `image2d` mip resources, and busy-image protection
  have resource-id, object-type, entry-count, and in-flight cache evidence.
- [x] Exact signed distance uses transient slab subranges, while the saved content identity is
  derived from mask asset key and extent and excludes feather radius; the radius-edit test
  observes the same distance resource and zero new transient bytes.
- [x] The exact reference test covers binary distance, partial coverage, boundary quantization,
  and the feather result within two R8 levels; analytic radial and graduated masks use the shared
  resolved geometry path.
- [x] Disconnected masks keep the mask cache and mask image unallocated and use constant-one
  mixing; masked and unmasked normal mix outputs match the CUDA reference at the asserted points.
- [x] The OpenCL mask pass resolves only the named CL1.2 mask kernels, and the PlanExecutor test
  proves mask evaluation reaches primary grade before result publication; no retired combined
  mask/grade dispatch is used by this DAG path.

**LOC note (grill-code-review):** full-file LOC for the O4 implementation and wiring is
`opencl_mask_pass.cpp` 538, `opencl_mask_pass.hpp` 42, `mask.cl` 320,
`opencl_mask_test.cpp` 616, `node_result_cache.hpp` 113, `opencl_pass_encoder.hpp` 94,
`primary_grade.cl` 276, `opencl_dag_programs.hpp` 56, `opencl_backend.cpp` 907,
`src/edit/CMakeLists.txt` 179, and `tests/edit/CMakeLists.txt` 459. No O4 production or test file
crossed 1000 LOC; the focused test fixture remains within one mask/cache/integration
responsibility. Earlier O3 working-tree edits remain preserved.

**Implemented:**

- Replaced the OpenCL mask identity path with analytic radial/graduated evaluation, R8 raster
  upload and sampling, independent workspace-owned mip images, exact signed-distance banding, and
  radius-only feather sampling.
- Added the seven mask kernel names to the DAG manifest usage and plan warm-up. The runtime suite
  compiled and executed the CL1.2 mask program on the configured OpenCL device.
- Added device-buffer metadata for the signed-distance content identity without copying an
  OpenCL virtual pointer to host memory, and routed all distance intermediates through the shared
  transient arena.
- Routed the real mask pass through `PassEncoder<OpenClBackend, GpuPassKind::MaskEvaluate>` and
  made primary grade mix use the original input, adjusted result, grade mix, and optional R8 mask.
- Registered the twelve focused tests in `GpuDagOpenClGradeTest`; existing OpenCL workspace/grade
  and CUDA mask tests remain green.

**Residual gaps:** none inside the written O4 checklist. O5 still owns OpenCL DRT, scope, present,
and product switching. O6 still owns deletion of the legacy OpenCL stage and the final quantified
performance matrix; no warm median or p95 measurement is claimed here.

## 10. Phase O5 — DRT、scope、present 与产品切换

目标：

- 完成 OpenCL DAG 端到端产品路径；
- 直接提交 display image；
- OpenCL 编辑器不再进入旧 stage adapter；
- scope 和 present 不经过 host。

工作：

1. 移植 ACES 2.0 和 OpenDRT；输入固定为 AP1/ACEScc，输出为用户选择的 display encoding。
2. DRT output 使用 workspace RGBA32F image。
3. `FramePresenter<OpenClBackend>` 通过 sink 的 OpenCL mapping 提交设备图像。
4. OpenCL/GL 和 OpenCL/D3D11 acquire/copy/release 都在当前 product queue 和 event 链中完成。
5. scope tap retain 同一最终 DRT image，并携带同一 final event；更新 scope analyzer 读取 image2d。
6. 显式 host 输出只在 export/test 请求时下载；它不是 present 失败后的替代路径。
7. session cache 与 one-shot workspace 保持隔离。
8. PipelineMgmtService 返回管线时等待最终 event，并释放 session results、transients、参数和 Neural
   activation workspace。
9. Windows Auto/OpenCL 产品选择在选定 OpenCL 后直接创建 OpenClRenderer。
10. present、scope 或下载失败时不发布本次结果内容 key。
11. 产品路径删除对 `CreateOpenCLGPUPipeline` 的调用，但物理旧代码留到 O6 同一堆栈下一 PR 删除。

测试：

```text
OpenClDrtAcesMatchesCudaReferenceWithinTolerance
OpenClDrtOpenDrtMatchesCudaReferenceWithinTolerance
OpenClDrtEditRunsOnlyDrtPass
OpenClRendererPresentsWorkspaceImageWithoutHostDownload
OpenClScopeTapUsesTheFinalDisplayImageAndSubmissionEvent
OpenClOneShotRenderDoesNotPublishIntoSessionCache
OpenClPipelineReturnReleasesSessionResourcesAfterGpuCompletion
OpenClBackendFailureDoesNotEnterCpuOrLegacyOpenClExecution
OpenClRealRawEditorUsesTheThreeNodeDag
OpenClPresentRejectsAnIncompatibleSinkWithoutHostSubmission
```

完成条件：

- OpenCL 产品帧从 PreparedRawInput 到 display image 全部由 DAG 执行；
- present/scope 的 D2H bytes 为零；
- explicit download 只由请求触发；
- 产品入口不创建 `GPUPipelineWrapper` 或旧 `OpenCLGPUPipeline`；
- 失败帧不发布内容 key 或通知成功。

##### Phase O5 completion record (2026-08-28)

Status: complete
Date: 2026-08-28
Branch: `feature/gpu-dag-opencl`
Commit: `7523ae7a` base; O5 implementation remains uncommitted in the working tree.

Implemented:

- Added OpenCL DRT parameter resolution and the DAG DRT pass. ACES 2.0 and OpenDRT
  consume ACEScc/AP1 input and write display-encoded RGBA32F workspace images.
- Added `OpenClRenderer` presentation: product-queue image copy, final marker/event,
  retained final image/event for the scope tap, and explicit download only for host-output
  requests.
- Added OpenCL image2d histogram/waveform kernels with event-aware three-slot scope
  lifetime. OpenCL/GL lease acquire/release/finish now use `ProductQueue`.
- Routed the Windows OpenCL selection through `Renderer<OpenClBackend>`, with one-shot and
  session cleanup, Neural activation workspace release, and failure discard before publication.

Primary call chains:

- `EditPipeline::Apply` -> `ApplyGpuDagProduct(opencl_product_renderer_)` ->
  `Renderer<OpenClBackend>::Render` -> `PlanExecutor` -> `ExecuteOpenClDrt` ->
  `OpenClFramePresenter::Present` -> `OpenClBackend::FinalizePresentation`.
- `OpenClFramePresenter::Present` -> `FinalDisplayFrameView` carrying the final OpenCL
  image and event -> `OpenClScopeAnalyzer::SubmitFrame` -> image2d histogram/waveform.
- `Renderer::Render` completion/failure -> final-event wait or discard -> session/one-shot
  resource release -> Neural activation workspace release.

Deleted:

- No physical legacy files. The product entry no longer calls
  `CreateOpenCLGPUPipeline`; the old implementation remains intentionally for O6.

Tests:

- command: `cmd /c scripts\msvc_env.cmd --build build\debug --target EditRuntimeOpenCl --parallel 4`
- command: `cmd /c scripts\msvc_env.cmd --build build\debug --target EditPipeline --target GpuDagOpenClDrtProductTest --parallel 4`
- command: `cmd /c scripts\msvc_env.cmd --build build\debug --target EditorRhiViewport --parallel 4`
- command: `ctest --test-dir build\debug -R "GpuDagOpenClDrtProductTest" --output-on-failure`
- result: 10/10 passed; ACES/OpenDRT CUDA parity, DRT-only dirty edit, zero present D2H,
  final image/event scope identity, one-shot isolation, session cleanup, failure isolation,
  three-node RAW graph, and incompatible sink.
- command: `ctest --test-dir build\debug -R "GpuDagOpenCl(Workspace|Develop|Grade)Test" --output-on-failure`
- result: 51/51 passed on the NVIDIA GeForce RTX 3080 Laptop GPU (driver 610.622, OpenCL C 1.2).

Program evidence:

- manifests/programs: DRT manifest includes fused parameter/common/CST/DRT sources; scope
  manifest contains linear and image2d histogram/waveform kernels.
- build/create/hit: DRT program and kernel compiled/created on the real OpenCL device;
  repeated DRT render verified no new image/buffer allocations.
- missing source/build/kernel errors: existing program-library installed-resource and
  missing-kernel tests remain green; DRT runtime build errors use the shared diagnostic path.

Resource evidence:

- buffer create/release: DRT parameters use the shared `ParameterArena`; session/one-shot
  teardown releases workspace images, transients, and parameter slots after `WaitIdle`.
- image create/release: final workspace image and retained sink/scope image use shared
  RAII/lease ownership; scope slots release only after the completion event.
- event retain/release: presenter retains the final marker for the scope view; renderer
  command context and scope resources release their own references after completion.
- upload/download ranges and bytes: present and scope input consume device images; the
  final O5 present test recorded `d2h_bytes == 0`; host download is explicit.
- waits/flushes: product queue carries DAG, present, interop, and scope work; final marker
  is flushed before publication; session return waits the final event.

Performance evidence:

- device/driver/OpenCL C/Windows/build: NVIDIA GeForce RTX 3080 Laptop GPU; driver 610.622;
  OpenCL C 1.2; Windows/MSVC Debug build.
- fixture and render request: 32x32 RGBA/RAW fixtures, `DecodeRes::FULL`, OpenCL DRT/product
  test request.
- cold: not measured in O5.
- warm median: not measured in O5.
- warm p95: not measured in O5.

Remaining work owned by the next named Phase:

- O6: physically remove the legacy OpenCL pipeline/factory and obsolete fused ABI/program
  wiring, then record the cold/warm/p95 performance matrix and final resource counters.

##### Phase O5 follow-up (2026-08-28)

Status: product-editor `CL_INVALID_BUFFER_SIZE` (-61) on image open — fixed.

Cause:

- `OpenClBackend::CreateBuffer` issued one `clCreateBuffer` for the whole Develop transient
  slab. OpenCL rejects a single allocation above `CL_DEVICE_MAX_MEM_ALLOC_SIZE`.
- `ExecuteOpenClDevelop` reserved
  `peak_transient_bytes + host_pixels * 28 + 16KiB`. That extra was added on top of
  `max(develop, LLF)`, so a real RAW (HLR on by default) requested a slab larger than the
  device cap. 32x32 DAG tests never hit the limit.

Fix:

- `TransientBufferArena` splits capacity across slabs each `<= Backend::MaxSlabBytes()`.
- OpenCL `MaxSlabBytes` is `CL_DEVICE_MAX_MEM_ALLOC_SIZE` from context creation, aligned
  down to 256 bytes.
- Develop reserved `max(compiled peak, host_pixels * 44)` and rewound demosaic
  planes before HLR. That 44-byte formula is an OpenCL-only extra working set;
  CUDA keeps HLR inside the compiled exclusive 12 bytes/pixel peak. See the
  CUDA-aligned follow-up below.
- `Allocate` walks every reserved slab, then appends a new slab when the remainder
  cannot hold the next plane. `Reserve` still cannot replace slabs while pointers
  are live. Slab size is `CL_DEVICE_MAX_MEM_ALLOC_SIZE` from context creation,
  aligned down to 256 bytes.

Primary call chain:

```text
EditPipeline::Apply
  -> Renderer<OpenClBackend>::Render
  -> ExecuteOpenClDevelop
  -> TransientBufferArena::Reserve(max(peak, HLR exclusive))
  -> OpenClBackend::CreateSlab (<= MaxSlabBytes each)
  -> OpenClBackend::CreateBuffer
```

Tests: `OpenClMaxSlabBytesUsesDeviceReportedMaxMemAllocSize`,
`OpenClTransientReserveAboveMaxSlabUsesSeparateDeviceBuffers`,
`OpenClTransientAllocateAppendsASlabWhenTheReservedTailIsTooShort`,
`OpenClCreateBufferRejectsSizeAboveMaxSlabWithoutInvalidBufferSize`,
`OpenClDevelopHighlightRecoveryWritesTheOutputTextureWithoutASecondRgbaImage`,
`WorkspaceCannotReplaceReservedSlabWhileTransientPointersAreLive`.

##### Phase O5 follow-up (2026-08-28) CUDA-aligned HLR VRAM

Status: HLR VRAM overcommit and missing queue flush fixed. This did not remove the final 100MP
spinner; the later on-demand scratch follow-up records the allocator-loop root cause and complete
fix.

Cause:

- CUDA SensorDevelop reserves `plan.peak_transient_bytes` only. Bayer HLR keeps the
  five RCD planes, binds 12 bytes/pixel exclusive scratch, and
  `ApplyHighlightCorrectionAndPackRGBAOriented` writes the existing output texture.
  Merge and HLR are exclusive; the compiler does not add a second full-res RGBA32F.
- OpenCL acquired a second `Textures().Acquire` RGBA32F, rewound, then copied into
  two float4 transients because HLR kernels were float4-buffer only. That is
  `~4.4 GiB` transients plus `~1.55 GiB` output plus `~1.55 GiB` extra image on a
  100MP Bayer frame.
- `SynchronizeRecordedWork` waited on a marker without `clFlush`, so a large
  in-order NVIDIA queue could stall forever.

Fix:

- `ExecuteOpenClDevelop` reserves `plan.peak_transient_bytes` only, matching CUDA.
- Bayer HLR: planar mask / chroma / reconstruct-and-pack into `develop.sensor_linear`.
- X-Trans and Neural HLR: reconstruct into an arena RGBA buffer and copy into the
  existing output image. No second RGBA32F.
- `OpenClBackend::SynchronizeRecordedWork` and `Wait` flush before `clWaitForEvents`.
- `TransientBufferArena::EnsureCapacity` frees unused slabs before creating replacements.

Primary call chain:

```text
EditPipeline::Apply
  -> Renderer<OpenClBackend>::Render
  -> ExecuteOpenClDevelop
  -> TransientBufferArena::Reserve(plan.peak_transient_bytes)
  -> EncodeBayerRcd
  -> EncodeHighlightReconstructPlanarAndPack
     (mask 12 B/px from the same arena, write_imagef into sensor_linear)
  -> PlanExecutor SynchronizeRecordedWork
  -> TransientBuffers().ReleaseDeviceMemory
```

Tests: `OpenClDevelopHighlightRecoveryWritesTheOutputTextureWithoutASecondRgbaImage`,
`OpenClDevelopHighlightRecoveryFitsWhenEachSlabIsCappedBelowCompiledPeak`,
`ReserveFreesUnusedSlabsBeforeAllocatingReplacements`,
`OpenClSynchronizeRecordedWorkFlushesTheProductQueueBeforeWait`.

##### Phase O5 follow-up (2026-08-28) CUDA-aligned Neural tiles

Status: implemented. OpenCL Neural already tiled the network (`OpenClDemosaicNetTiledExecutor` +
shared `BuildTileJobs`). The sandwich packed a full-frame HWC3 mosaic and a
full-frame RGBA before packing the output.

CUDA `DemosaicNeuralEngine` packs each student tile from the aligned mono CFA
(`PackReflectPaddedCfaTile`), keeps activations tile-sized, and assembles into a
full-frame RGB canvas.

Fix:

- Product OpenCL tiles pack from the original mono CFA (`ForwardReflectMonoCfaToHwc`).
- DAG `EncodeNeural` no longer copies the linear CFA or materializes `mosaic_hwc` /
  process-static RGBA. RGB assembly stays one aligned HWC3 canvas (CUDA `output_rgb`).
- No-HLR packs that RGB into `develop.sensor_linear` with `copy_rgb_crop_inverse_orient`.

Primary call chain:

```text
ExecuteOpenClDevelop
  -> EncodeToLinearRef (arena CFA)
  -> EncodeNeural
  -> OpenClDemosaicNetTiledExecutor (PackReflect from mono CFA, tile activations)
  -> EncodeCopyRgbCropInverseOrient into develop.sensor_linear
```

Tests: `BayerReflectMonoCfaForwardMatchesSparseHwc3Within1eMinus4`,
`XTransReflectMonoCfaForwardMatchesSparseHwc3Within1eMinus4`,
`OpenClDevelopNeuralEngineWritesFiniteRgbFromMonoCfaTilesAndDiffersFromLegacy`,
`OpenClDevelopNeuralUsesSessionWorkspaceAndDoesNotSelectAnotherDemosaicOnFailure`.

##### Phase O5 follow-up (2026-08-28) on-demand Develop scratch and allocator progress

Status: complete. Four real 100MP Bayer fixtures finish through the production OpenCL Develop
path. The editor spinner and shutdown wait were caused by a CPU allocation loop, not by an
unfinished OpenCL event.

Cause:

- `TransientBufferArena::PlaceFrom` aligned the next candidate above an unaligned slab end. The
  subsequent `NextSlabOrigin` call returned that same slab end, and `AlignUp` reproduced the same
  candidate. The loop therefore made no progress and never reached another `clCreateBuffer` call.
- A 100MP U16 CFA allocation is not a 256-byte multiple. It deterministically exposed the loop on
  the following F32 plane allocation. The earlier whole-frame `Reserve(peak_transient_bytes)` also
  reproduced the same boundary condition when the estimated peak was split by
  `CL_DEVICE_MAX_MEM_ALLOC_SIZE`.
- The peak estimate described possible compiler liveness, not the exact runtime demosaic method,
  HLR branch, alignment padding, and device slab layout. The reserved memory was released after the
  same render, so eager reservation did not amortize allocation across frames.
- `QObject::~QObject: Timers cannot be stopped from another thread` was a secondary shutdown
  symptom: the render worker could not return from the allocator loop, so normal QObject teardown
  ordering could not complete.

Fix:

- `PlaceFrom` now treats a non-increasing aligned candidate as no fit. `Allocate` can then append a
  slab and retry instead of looping forever.
- OpenCL Develop no longer eagerly reserves `plan.peak_transient_bytes`. It requests CFA,
  linearization, selected demosaic, and HLR scratch from the arena only for the actual execution
  branch. The arena still owns the allocations and releases them together at frame completion.
- Legacy demosaic scratch is allocated before the final RGBA image. This keeps exact branch memory
  visible to the arena and avoids using a coarse estimate as allocation policy.
- Optional `ALCEDO_GPU_POOL_TRACE` stage and buffer-create diagnostics remain available without
  changing normal stdout behavior.

Primary success call chain:

```text
EditPipeline::Apply
  -> Renderer<OpenClBackend>::Render
  -> PlanExecutor
  -> ExecuteOpenClDevelop
  -> TransientBufferArena::Allocate(actual branch scratch)
  -> PlaceFrom returns no-fit when the candidate cannot advance
  -> AppendAndPlace(device-sized slab)
  -> EncodeBayerRcd
  -> EncodeHighlightReconstructPlanarAndPack
  -> SynchronizeRecordedWork (flush, then wait)
  -> TransientBuffers().ReleaseDeviceMemory
  -> publish completed frame
```

Primary allocation failure chain:

```text
TransientBufferArena::Allocate
  -> AppendAndPlace
  -> OpenClBackend::CreateBuffer
  -> real OpenCL allocation error
  -> PlanExecutor cancels the render
  -> no result publication and no backend or quality substitution
```

Verification:

- `OpenClHundredMegapixelBayerFixturesCompleteAndProduceFinitePixels`: passed in 34.786 s for
  `DSCF0224.RAF`, `DSCF0305.RAF`, `DSCF0337.RAF`, and `B0004841.dng`; each fixture contains more
  than 100 million sensor pixels and returns finite RGB from the completed image.
- `TransientBufferArena.UnalignedFirstAllocationAppendsAnotherSlabWithoutLooping`: passed; a
  257-byte first allocation followed by a 512-byte aligned allocation exercises the exact
  no-progress boundary without requiring a multi-gigabyte device.
- Focused OpenCL workspace and Develop run: 33 discovered, 32 passed, the environment-gated real
  fixture test skipped by default, zero failed, 40.02 s.
- `alcedo_main` debug target: built and linked successfully after the production changes.

目标：

- 删除 OpenCL 产品路径中的旧 pipeline、stage、总参数 ABI 和混合 operator 执行；
- 删除只服务旧 fused 路径的 program、kernel、测试和 CMake wiring；
- 证明新 OpenCL DAG 从 cold 到稳定帧满足性能和资源目标。

OpenCL 专属删除项：

- `edit/pipeline/pipeline_opencl_impl.cpp`；
- `edit/pipeline/pipeline_opencl_param.cpp`；
- OpenCL `CreateOpenCLGPUPipeline` 和旧 `GPUPipelineImpl` 分支；
- `edit/pipeline/opencl_kernel_dispatch.hpp` 中只服务旧 stage 的类型；
- `edit/operators/GPU_kernels/opencl_param.hpp`；
- OpenClFusedParams、OpenClFusedResources 和 OpenClFusedParamUploader；
- `highlight_shadow_local_tone::OpenClStage` 及其实现；
- 旧 fused stage 参数 buffer、neighbor stage buffer 和逐调用 dummy LUT buffer；
- `OpenCLGPUPipeline` 持有的 working/pre-HS/HS/blur/detail scratch；
- 旧执行路径中的 `ValidateParamsABI` 每帧 kernel/readback；
- `edit_pipeline_fused_rgba32f`、`edit_pipeline_fused_stage_rgba32f` 和旧 fused ABI validation
  kernel；
- 仅被旧入口使用的 `edit_pipeline_fused.cl`、`fused_params.cl`、
  `fused_params_validation.cl` 及旧 include；
- `RegisterOpenClEditPipelinePrograms()` 中旧 fused/detail descriptor；
- 旧 pipeline source、shader path define、测试 target 和 CMake source；
- `opencl_fused_edit_pipeline_test.cpp` 中依赖 GPUPipelineWrapper/OperatorParams 的测试；
- 已迁移算子的 OpenCL ApplyGPU/私有 GPU 资源入口。

删除时允许保留仍被 DAG `.cl` 编译单元实际 include 的数学 helper，但必须移动到明确共享路径并
改名为其数学用途。不得保留旧 fused entrypoint、旧参数布局或可重新启用旧 pipeline 的 build flag。

共享删除边界：

- OpenCL 完成后，OpenCL 产品源码不得引用 PipelineStage、PipelineStageName、OperatorParams、
  GPUPipelineWrapper 或 merged stage；
- CUDA/Metal 已完成的旧路径删除项如果仍在共享 factory 中，O6 必须按主计划最终状态一起清理；
- 全局共享类型只有在所有产品后端不再引用时才可物理删除；
- 不得因保留旧算法 reference test 而保留旧产品 factory；reference fixture 必须改为直接调用
  DAG renderer/pass 或保存 golden 数据。

静态检查：

```text
NoOpenClProductSourceReferencesPipelineStage
NoOpenClProductSourceReferencesOperatorParams
NoOpenClRuntimeOwnsOperatorPrivateScratchOrCache
NoLegacyOpenClPipelineFactoryRemains
NoLegacyOpenClFusedParameterAggregateRemains
NoLegacyOpenClFusedProgramIsRegisteredOrPackaged
NoOpenClDagPassRegistersProgramsAtRenderTime
NoOpenClDagPassFinishesTheQueue
```

O6 在记录第 12 节全部 A/B 和删除证据前不得标记 complete。

## 12. 性能验收

### 12.1 基线

删除旧代码前，在同一台 Windows 机器、同一 GPU、同一 driver、同一 OpenCL compiler、同一
Release 配置、同一 RAW、同一 DecodeRes、同一 viewport 和同一编辑序列下比较：

1. 本计划开始前的旧 OpenCL 产品路径；
2. OpenCL DAG 最终路径；
3. CUDA/Metal reference 的 pass 执行集、缓存失效范围和像素 reference。

不同 GPU 或不同厂商之间不比较绝对 wall time。跨后端只比较像素、pass 数、缓存边界、H2D/D2H
字节和稳定帧资源创建；绝对性能只在同一设备上做旧/新 OpenCL A/B。

每份记录必须包含：

- platform、device、vendor、driver、OpenCL C version；
- image support 和关键 image format；
- global memory、max allocation、local memory、work-group 限制；
- Windows、编译器、构建配置和 commit；
- RAW fixture、DecodeRes、RenderRequest、viewport 和调整序列。

### 12.2 场景

每个场景记录一次 cold render 和至少 30 次 warm render：

| 场景 | 允许执行的 GPU 图像工作 | 必须为零 |
| --- | --- | --- |
| 无变化重复渲染 | 已完成结果提交与 scope/present | LibRaw、source upload、plan compile、全部节点 pass、resource/program/kernel create/release、D2H |
| Exposure 连续调整 | PrimaryGrade、DRT | SensorDevelop、Geometry、CameraColor、资源创建/释放、D2H |
| CCT/tint 连续调整 | CameraColor、PrimaryGrade、DRT | SensorDevelop、Geometry、资源创建/释放、D2H |
| DRT 连续调整 | DRT | SensorDevelop、Geometry、CameraColor、PrimaryGrade、资源创建/释放、D2H |
| viewport/geometry 改变 | Geometry 及下游 | SensorDevelop、source upload、资源创建/释放、D2H |
| Develop 参数改变 | SensorDevelop 及下游 | LibRaw 重复 open/unpack、无关 source preparation、资源创建/释放 |
| mask dirty rectangle | Mask、PrimaryGrade、DRT | full mask upload、signed distance 重建（内容未变时）、D2H |
| feather radius 改变 | Mask coverage、PrimaryGrade、DRT | signed distance 重建、R8 upload、资源创建/释放 |
| 切图后切回 | 预算内内容命中与 present | 命中项的 LibRaw、source upload、节点 pass、资源创建/释放、D2H |

### 12.3 数值门槛

- warm interactive median 不高于旧 OpenCL 基线的 `1.05x`；
- warm interactive p95 不高于旧 OpenCL 基线的 `1.10x`；
- 无变化、Exposure、CCT、DRT、mask radius 和 viewport 编辑从第二帧起 GPU resource
  create/release 为零；
- 稳定帧 program build 和 kernel create 为零；
- 参数上传只覆盖 dirty range；
- source 未变化时 source upload bytes 为零；
- 产品 present 和 scope D2H bytes 为零；
- pointwise Grade dispatch 数不得随普通 pointwise adjustment 数线性增长；
- 除共享 Develop scratch 生命周期同步点和显式 download 外，render 内 `clFinish` 次数为零；
- event retain/release 在 session 释放后相等；
- 不得用降低质量、缩小输入、跳过要求算法或切换后端满足门槛；
- 未达到任一门槛时 O6 不得标记 complete。

## 13. 构建与 kernel 组织

建议目标：

```text
EditRuntimeOpenCl
OpenClProgramLibrary
GpuDagOpenClWorkspaceTest
GpuDagOpenClDevelopTest
GpuDagOpenClGradeTest
GpuDagOpenClMaskTest
GpuDagOpenClDrtTest
GpuDagOpenClRendererTest
GpuDagOpenClPerformanceTest
```

建议 kernel 组织：

```text
edit/runtime/opencl/shader/
  geometry_camera.cl
  primary_grade.cl
  local_tone.cl
  mask.cl
  drt.cl
  common/
    acescc.cl
    color_math.cl
    sampling.cl

decoders/processor/operators/gpu/opencl_shader/
  RAW 专用 linearize/demosaic/highlight/neural kernel
```

每个新增 OpenCL 编译单元必须：

- 由 CMake 明确提供 source/resource path；
- 加入 `RegisterOpenClGpuDagPrograms()` 或对应 RAW/scope manifest；
- 使用集中 program/kernel 常量；
- 在源码树运行和安装布局运行时都能找到；
- 由 OpenClProgramLibrary 构建，并保留完整 build log；
- 由 OpenClKernelCache 创建 kernel；
- 有缺失 source、program build failure 和缺失 kernel 的明确错误测试；
- 不通过静态对象构造或短生命周期对象注册；
- 不在每帧拼接不同 source 或 build options。

旧 fused shader 在 O6 删除前可以继续给旧产品基线使用，但新的 DAG manifest 不得把旧 fused
program 标记为自己的依赖。O5 产品切换后，新安装包不得再要求旧 fused program 成功构建。

## 14. 完成条件

### 14.1 架构

- [ ] OpenCL 使用主计划的 PipelineDocument、GraphCompiler、ExecutionPlan、DTO、dirty Patch、
      Geometry 和内容 key。
- [ ] 产品入口是 `OpenClRenderer = Renderer<OpenClBackend>`。
- [ ] 后端通过模板 Traits 和 PassEncoder 特化区分。
- [ ] OpenCL 产品路径不引用 PipelineStage、OperatorParams 或 GPUPipelineWrapper。
- [ ] OpenCL Pass 不拥有独立 pool、LRU、长期 scratch、context 或 command queue。

### 14.2 program 与 kernel

- [ ] DAG program 通过 OpenClBackendProgramRegistry 的 module manifest 注册。
- [ ] OpenClProgramLibrary 不依赖 edit、RAW、scope 或 UI 模块。
- [ ] program/kernel 名集中且稳定。
- [ ] WarmUpPlan 只构建当前计划所需 program/kernel。
- [ ] 稳定 render 不 build program 或 create kernel。
- [ ] 安装布局不依赖源码树绝对路径。

### 14.3 资源

- [ ] 每个 OpenCL RenderDevice 只有一个 workspace 和一个 CommandContext。
- [ ] 使用进程唯一 OpenClContext 和 product queue。
- [ ] 所有 transient、普通 image、mask image、结果 image 和 LUT 都经过统一所有者。
- [ ] 稳定 render 不创建或释放 buffer/image/program/kernel。
- [ ] busy submission 的资源不会被复用或淘汰。
- [ ] cache hit 由内容 key 证明。
- [ ] session 释放后 event retain/release 平衡。

### 14.4 行为

- [ ] Develop、Geometry、CameraColor、Grade、LLF、Mask、Mix 和 DRT 与 CUDA reference 在各自
      容差内一致。
- [ ] 同一调整顺序在 OpenCL 上不被重排。
- [ ] crop、rotation、viewport 和 dynamic resolution 只做一次图像重采样。
- [ ] GPU/Neural/program/present 失败不进入任何替代图像处理路径。
- [ ] scope 和 present 使用最终 OpenCL display image 和同一完成 event。
- [ ] 产品 present/scope 不下载 host 数据。

### 14.5 删除

- [ ] 旧 OpenCL pipeline factory 和实现已删除。
- [ ] OpenClFusedParams/OpenClFusedParamUploader 已删除。
- [ ] OpenClStage 私有 LLF cache/allocator 已删除。
- [ ] 旧 OpenCL fused program 不再注册、测试或打包。
- [ ] 已迁移 operator 不再提供旧图像执行入口。
- [ ] 不存在 build flag 可重新启用旧 OpenCL pipeline。

### 14.6 性能

- [ ] 第 12 节全部场景有 cold、median、p95 和资源计数记录。
- [ ] 同设备旧/新 OpenCL A/B 达到第 12.3 节门槛。
- [ ] 每个 Phase 的 completion record 已写入设备、driver、系统、构建配置、fixture、命令和结果。

## 15. Phase 完成记录格式

每个 Phase 完成后在对应章节末尾追加：

```text
Status: complete
Date:
Branch:
Commit:

Implemented:
-

Deleted:
-

Tests:
- command:
- result:

Program evidence:
- manifests/programs:
- build/create/hit:
- missing source/build/kernel errors:

Resource evidence:
- buffer create/release:
- image create/release:
- event retain/release:
- upload/download ranges and bytes:
- waits/flushes:

Performance evidence:
- device/driver/OpenCL C/Windows/build:
- fixture and render request:
- cold:
- warm median:
- warm p95:

Remaining work owned by the next named Phase:
-
```
