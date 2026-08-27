# GPU DAG Metal 移植逐 Phase 计划

Date: 2026-08-24

Status: M0–M5 complete; M6–M7 planned

Branch: `feature/gpu-dag-metal`

Primary owner: Alcedo Studio 编辑管线 Metal 后端。

## 1. 必读设计文档

开始本计划前必须完整阅读：

- [GPU DAG 编辑管线重构 Phase 计划](gpu_dag_pipeline_rebuild_phase_plan.md)

该文档定义 PipelineDocument、GraphCompiler、ExecutionPlan、Model/DTO、dirty Patch、
RenderGeometryResolver、MaskStore、内容 key、缓存边界和 CUDA 参考行为。本计划只定义 Metal
移植的执行切片，不重新建立一套 Metal graph、Model、DTO、ROI 或缓存语义。

发生冲突时遵循以下优先级：

1. 本计划对 Metal Phase、Metal 资源生命周期和 Metal 删除范围的明确要求；
2. GPU DAG 主计划的共享图、数据、缓存和几何设计；
3. 现有 Metal 旧管线只作为像素算法和性能基线，不能作为目标架构。

## 2. 决策与边界

### 2.1 G7R.4 和 G7R.5 不阻塞 Metal

Metal 移植不以 G7R.4 creative CAT02 或 G7R.5 CUDA 完整统计收尾为前置条件。

- 当前 UI 没有暴露新的 node creative CAT02 编辑入口；
- Metal 对已经进入 PipelineDocument 的参数保持与当前 CUDA DAG 一致；
- 本计划不扩大 CUDA creative CAT02 行为；
- 本计划不承担 G7R.5 的 CUDA 基线收尾；
- Metal 自身的资源计数、逐 Pass 计时和稳定帧性能仍然是本计划的强制验收项。

这项决定只解除 Metal 的依赖，不允许 Metal 引入 CPU、旧管线或其他 GPU 后端替代路径。

### 2.2 Renderer 命名

共享产品/session 渲染入口命名为：

```cpp
template <class Backend>
class Renderer;

using CudaRenderer = Renderer<CudaBackend>;
using MetalRenderer = Renderer<MetalBackend>;
```

不新增 `ProductRenderer`、`BasicProductRenderer` 或 `MetalProductRenderer`。现有
`CudaProductRenderer` 在共享 Renderer 落地时迁移到上述命名。

`Renderer<Backend>` 负责：

- PreparedSourceCache；
- StaticExecutionPlanCache；
- RenderDevice 和 workspace 生命周期；
- MaskStore 读取入口；
- session cache 与 one-shot cache 隔离；
- GraphCompiler 静态计划和逐帧 geometry 绑定；
- 后端展示与显式 host 输出；
- 失败传播、统计快照和 session 资源释放。

### 2.3 不允许 fallback

以下行为全部禁止：

- Metal kernel、metallib、buffer、texture、command buffer 或 present 失败后进入 CPU；
- Metal Develop 失败后调用旧 RawProcessor Metal 整体管线；
- Neural Engine 失败后改用 Legacy demosaic；
- DAG Metal 失败后进入旧 `GPUPipelineImpl`、PipelineStage 或 fused stage；
- 为规避性能问题改变 DecodeRes、RenderQuality、viewport 或输出精度；
- 捕获真实错误并提交上一帧、空帧或低质量帧作为成功结果。

失败必须取消未完成提交，不发布新的内容 key，并把原始错误送到 app 层。

### 2.4 Metal 旧实现的用途

以下代码只能用于确认算法、shader 数学和旧性能基线：

- `edit/pipeline/pipeline_metal_impl.cpp`；
- `edit/pipeline/metal_shader/fused_pipeline.metal`；
- `edit/operators/GPU_kernels/metal_param.hpp`；
- `edit/operators/basic/highlight_shadow_local_tone_metal.cpp`；
- RAW Processor 下现有 Metal operator wrappers；
- `metal::MetalImage` 的格式和展示互操作实现。

不得把它们包装进 DAG 继续使用。存在静态 scratch、算子私有 buffer、内部 command buffer、
逐调用 commit/wait 或总参数上传的入口必须拆成接收当前 command context 与 workspace 资源的
encode-only 实现。

## 3. 目标架构

```text
PipelineDocument
      │
      ▼
GraphCompiler ──────────────── shared, GPU-free
      │
      ▼
ExecutionPlan
      │
      ▼
Renderer<MetalBackend>
├── PreparedSourceCache
├── StaticExecutionPlanCache
├── MaskStore
└── BasicRenderDevice<MetalBackend>
    ├── MetalCommandContext
    ├── BasicRenderWorkspace<MetalBackend>
    │   ├── ParameterArena
    │   ├── TransientBufferArena
    │   ├── TexturePool
    │   ├── MaskTextureCache
    │   ├── GraphImageCache
    │   └── NodeResultCache
    └── MetalSharedGpuResources
        ├── ComputePipelineCache references
        ├── LUT texture cache
        ├── immutable sampler states
        └── Neural model and activation ownership
```

### 3.1 编译期后端区分

共享 host 运行时使用模板，后端差异使用明确特化：

```cpp
template <class Backend>
concept RenderBackend = requires(Backend backend,
                                 typename Backend::CommandContext& commands) {
  typename Backend::Buffer;
  typename Backend::Texture2D;
  typename Backend::CommandContext;
  backend.CreateBuffer(std::size_t{});
  backend.CreateTexture2D(std::uint32_t{}, std::uint32_t{}, TextureFormat{});
  backend.Submit(commands);
  backend.Wait(commands);
};

template <RenderBackend Backend>
class BasicRenderDevice;

template <RenderBackend Backend, GpuPassKind Kind>
struct PassEncoder;

template <RenderBackend Backend>
class PlanExecutor;

template <RenderBackend Backend>
class Renderer;
```

示例：

```cpp
template <>
struct PassEncoder<MetalBackend, GpuPassKind::GeometryResample> {
  static void Encode(MetalRenderContext& context,
                     const ExecutionPlan& plan);
};
```

规则：

- 外层可以保留一次 IRenderDevice 类型隐藏；
- 一帧内部不按后端做虚调用；
- GraphCompiler 不包含 CUDA/Metal 原生类型；
- GraphCompiler 不为每个后端复制 pass 构建流程；
- 后端能力通过 capability version 和 Traits 表达；
- shader 源码按 CUDA/MSL 分开，host 生命周期和缓存流程保持共享；
- 模板只负责稳定的生命周期与调度，不把大段像素数学塞进泛型头文件。

### 3.2 MetalBackend Traits

MetalBackend 必须提供：

- move-only `Buffer` 和 `Texture2D` 包装；
- `MetalCommandContext`；
- buffer/texture 创建、释放和字节统计；
- buffer range 上传与下载；
- R8 texture rect 上传；
- texture-to-texture blit；
- submission id、完成 id 和 busy 查询；
- command buffer 提交、等待和取消后的资源处理；
- device、queue、MTLHeap 和资源预算访问；
- 分配、释放、上传和 pipeline-state 计数。

Metal 原生类型只出现在 Metal runtime、`.mm` 和受 `HAVE_METAL` 保护的边界中。

### 3.3 CommandContext

每次 render 只使用一个 Metal command buffer：

```text
BeginRender
  -> 等待上一份提交完成
  -> 创建本帧唯一 command buffer
  -> 上传 dirty 参数和输入 dirty range
  -> 按 ExecutionPlan 编码 compute/blit encoder
  -> 编码 present/scope 所需操作
  -> commit
EndRender
```

禁止 Pass 自行创建 command buffer、queue、commit 或 wait。必要的 encoder 边界由资源 hazard、
blit/compute 类型和可读性决定，不按旧 stage 划分。

### 3.4 统一资源分配

Metal 资源必须通过 RenderDevice 所拥有的 workspace 或 SharedGpuResources 创建。

| 资源 | 唯一所有者 | Metal 存储策略 |
| --- | --- | --- |
| 参数 | ParameterArena | 单一 shared/managed buffer，dirty range 写入 |
| 临时 buffer | TransientBufferArena | device-private grow-only slab |
| 中间图像 | TexturePool | MTLHeap 上的 private texture |
| 节点结果 | GraphImageCache | TexturePool lease + 内容 key |
| Raster mask | MaskTextureCache | R8 private texture/mip chain |
| mask signed distance | workspace KV/cache | private buffer/texture + 内容 key |
| LUT | MetalSharedGpuResources | 内容 key + 字节预算 |
| pipeline state | ComputePipelineCache | metallib path + function name |
| Neural 权重/激活 | MetalSharedGpuResources/workspace | 权重跨 session 安全复用，激活按 device/session 所有 |

要求：

- MTLHeap 页只在没有 in-flight submission 时增长；
- texture pool 只复用完整匹配的 extent、format、usage 和 storage mode；
- active lease 和 busy submission 使用的资源不可淘汰；
- plan 预热后，稳定 render 不创建或销毁 buffer、texture、heap 或 pipeline state；
- 算子、Pass encoder 和 shader wrapper 不拥有第二套 LRU、pool 或长期 scratch；
- `recommendedMaxWorkingSetSize` 参与预算计算，并保留 app/UI 纹理所需余量；
- 分配复用与内容命中使用不同计数，不能用 texture identity 证明结果有效。

## 4. Phase 总表

| Phase | 主题 | 强制输出 |
| --- | --- | --- |
| M0 | 共享 Renderer 与模板执行骨架 | CUDA 继续运行；Metal 后端可编译接入 |
| M1 | MetalBackend、workspace 与 pipeline warm-up | 统一资源机制和零稳定帧创建证据 |
| M2 | Develop、Geometry、CameraColor | `develop.sensor_linear`、`geometry.scene_source`、`develop.image` |
| M3 | Primary Grade 融合路径 | 参数 arena、pointwise fusion、detail/LUT |
| M4 | LLF workspace 化 | canonical reference、ROI sampling、无私有 allocator/cache |
| M5 | Mask、Feather、Mix | R8/mip/距离场和统一 Normal Mix |
| M6 | DRT、scope、present 与产品切换 | Metal DAG 端到端产品路径 |
| M7 | 旧 Metal 管线删除与性能验收 | macOS 产品路径无 PipelineStage/旧 op 执行 |

每个 Phase 都必须同时交付行为测试、资源测试和性能快照。不能先提交逐帧分配、逐算子 dispatch、
逐 Pass wait 或重复 H2D，再把修复留给最终 Phase。

## 5. Phase M0 — 共享 Renderer 与模板执行骨架

目标：

- 把 CUDA 产品/session 调度提炼成 `Renderer<CudaBackend>`；
- 建立可实例化 `Renderer<MetalBackend>` 的 host 模板；
- 保持 CUDA 当前像素输出、缓存 key 和执行集不变；
- G7R.4/R.5 不作为 M0 完成条件。

工作：

1. 把 `CudaProductRenderer` 中后端无关的 source cache、plan cache、MaskStore、session/one-shot
   流程移动到 `Renderer<Backend>`。
2. 保留 `CudaRenderer` 别名，更新 CUDA 产品调用点。
3. 把 `CudaRenderDevice::Execute` 中内容 key 查询、pass execute/skip 统计、失败取消和发布流程
   提炼到 `PlanExecutor<Backend>`。
4. 通过 `PassEncoder<Backend, Kind>` 特化调用 CUDA 和 Metal 实现。
5. 把 ViewerDisplayConfig 解析留在共享 DRT DTO 层；native present 留在 Backend Presenter。
6. 为 CUDA 和 Metal 使用不同 capability version，但共享 StaticPlanKey 规则。
7. 对模板头增加 compile-only header hygiene 测试，防止 Metal/CUDA 原生头泄漏。

禁止：

- 复制一份 `MetalRenderer` 产品调度代码；
- 把 `GpuBackendKind` switch 放入每个 pass；
- 为 Metal 建立不同的 GraphValueId 或缓存失效规则；
- 在 M0 修改 CUDA 像素数学或补做 G7R.4/R.5。

测试：

```text
RendererTemplateInstantiatesCudaWithoutMetalHeaders
RendererTemplateInstantiatesMetalWithoutCudaHeaders
CudaRendererPreservesCurrentPlanAndResultCacheKeys
RendererOneShotWorkspaceCannotPublishIntoSessionCache
RendererFailureDoesNotPublishUnfinishedContentKeys
GraphCompilerPassListIsBackendNativeTypeFree
```

完成条件：

- 产品/session 类名是 `Renderer<Backend>`；
- CUDA 现有集中测试通过；
- Metal 可以接入同一 source/plan/result cache 流程；
- 没有新增运行时 fallback。

##### Phase M0 completion record (2026-08-24)

**Status:** complete — shared `Renderer<Backend>` / `PlanExecutor<Backend>` / `PassEncoder<Backend, Kind>` skeleton; CUDA product path unchanged; Metal host types instantiate the same session caches.

**Primary success call chain:**

```text
CPUPipelineExecutor::Apply (CUDA)
  -> Renderer<CudaBackend>::Render(UseSessionCache)
  -> PreparedSourceCache::AcquireEncoded
  -> StaticExecutionPlanCache::GetOrCompile(kCudaDagBackendCapabilityVersion)
  -> GraphCompiler::BindFrameGeometry
  -> PlanExecutor<CudaBackend>::Execute
       miss -> PassEncoder<CudaBackend, Kind>::Encode
       hit  -> skip + GpuNodePassStats
  -> FramePresenter<CudaBackend>::Present / Download
  -> PublishResults
```

**Primary failure call chain:**

```text
PassEncoder / upload / present throws
  -> PlanExecutor::CancelRender (discard unpublished)
  -> Renderer does not PublishResults
  -> ReportError + rethrow
  -> no CPU, no old Metal pipeline, no unpublished content key
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RendererTemplateInstantiatesCudaWithoutMetalHeaders` | `GpuDagCudaWorkspaceTest` | PASS |
| `RendererTemplateInstantiatesMetalWithoutCudaHeaders` | `GpuDagRawInputTest` | PASS |
| `CudaRendererPreservesCurrentPlanAndResultCacheKeys` | `GpuDagCudaDrtProductTest` | PASS |
| `RendererOneShotWorkspaceCannotPublishIntoSessionCache` | `GpuDagCudaDrtProductTest` | PASS |
| `RendererFailureDoesNotPublishUnfinishedContentKeys` | `GpuDagCudaDrtProductTest` | PASS |
| `GraphCompilerPassListIsBackendNativeTypeFree` | `GpuDagRawInputTest` | PASS |

Commands:

- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest GpuDagCudaWorkspaceTest GpuDagCudaDrtProductTest EditPipeline`
- `ctest --test-dir build/debug -R GpuDagRawInputTest --output-on-failure` → 39/39
- `ctest --test-dir build/debug -R GpuDagCudaWorkspaceTest --output-on-failure` → 17/17
- `ctest --test-dir build/debug -R GpuDagCudaDrtProductTest --output-on-failure` → 41/41

Suite totals: 39/39 RawInput, 17/17 CUDA workspace, 41/41 CUDA product.

**Checklist / exit condition:** all M0 exit conditions met.

**LOC note (grill-code-review):** new shared headers stay under 250 LOC (`renderer.hpp` 233, `plan_executor.hpp` 147, `metal_backend.hpp` 154). `cuda_result_cache_test.cpp` 565 LOC. `pipeline_cpu.cpp` 790 LOC, only call-site rename. No file crossed 1000 LOC in this change.

**Residual gaps:** Metal `CreateBuffer` / `PassEncoder` / present still throw until M1–M6 implement GPU resources and encode-only passes. CUDA pixel math and G7R.4/R.5 were not changed.

## 6. Phase M1 — MetalBackend、workspace 与 pipeline warm-up

目标：

- 实现完整 MetalBackend Traits；
- 实例化 `BasicRenderWorkspace<MetalBackend>`；
- 从第一份可执行代码起统一 buffer、texture、cache 和 pipeline state 生命周期。

工作：

1. 新增 `edit/runtime/metal/metal_backend.hpp/.mm`。
2. 新增 move-only Buffer、Texture2D 和 MetalCommandContext。
3. 为 private buffer/texture 建立 Metal heap page allocator，并由 MetalBackend 独占。
4. 实现 ParameterArena dirty range 写入；managed storage 设备执行必要同步标记。
5. 实现 R8 dirty rectangle blit 和 RGBA32F/R32F texture 上传。
6. 实现 submission id、完成回调和 `IsResourceBusy`。
7. 实例化 TransientBufferArena、TexturePool、MaskTextureCache、GraphImageCache 和
   NodeResultCache。
8. 扩展 ComputePipelineCache 的只读 hit/miss/create 统计；稳定帧不得创建 pipeline state。
9. 使用 app 选择的同一 Metal device；禁止 workspace、MetalImage 和 present 分别创建 device。
10. 在首次执行计划前解析 metallib 和预热该计划需要的 pipeline state。

测试：

```text
MetalParameterArenaUploadsOnlyDirtyRanges
MetalTransientArenaRewindsWithoutReallocatingItsSlab
MetalTexturePoolReusesMatchingPrivateTextures
MetalTexturePoolDoesNotEvictBusySubmissionResources
MetalMaskTextureCacheUsesOneWorkspaceByteBudget
MetalSecondEmptyRenderCreatesNoBufferTextureHeapOrPipelineState
MetalFailedUploadRestoresDirtyFieldsAndPublishesNoResult
MetalRenderDeviceUsesThePresentationDevice
```

性能门槛：

- 第二次相同计划的 buffer/texture/heap/pipeline create 和 free 都为零；
- 无 dirty 参数时 ParameterArena 上传字节为零；
- BeginRender/EndRender 之外不存在 pass 级 wait；
- 统计通过 API 快照读取，不依赖日志解析。

##### Phase M1 completion record (2026-08-25)

**Status:** complete
**Date:** 2026-08-25
**Branch:** `feature/gpu-dag-metal`
**Commit:** `e3230a14`

**Implemented:**

- `EditRuntimeMetal` with `metal_backend.mm`: move-only Buffer/Texture2D, one command buffer per render, private MTLHeap page allocator, shared/managed ParameterArena buffer with `didModifyRange` on managed storage, R8 rect blit, RGBA32F/R32F texture upload/download, submission id + completed-handler + `IsResourceBusy`.
- Instantiated `BasicRenderWorkspace<MetalBackend>`: ParameterArena, TransientBufferArena, TexturePool, MaskTextureCache, GraphImageCache, NodeResultCache.
- `ComputePipelineCache` hit/miss/create snapshot API; `MetalBackend::WarmUpPipelines` / `WarmUpPlan` before `PlanExecutor::Execute`.
- One process Metal device: `MetalContext::BindPresentationDevice`; Qt Quick `setGraphicsDevice` uses `MetalContext` device/queue. Workspace `NativeDevice()` is that same pointer.

**Deleted:**

- Header-only MetalBackend stubs that threw on every GPU allocation (replaced by host TU on non-Metal and Metal runtime on macOS).

**Tests:**

- command: `cmake --preset macos_debug_tests`
- command: `cmake --build --preset macos_debug_tests --target GpuDagMetalWorkspaceTest GpuDagRawInputTest --parallel 8`
- command: `ctest --test-dir build/macos-debug-tests -R GpuDagMetalWorkspaceTest --output-on-failure`
- result: 8/8 PASS

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MetalParameterArenaUploadsOnlyDirtyRanges` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalTransientArenaRewindsWithoutReallocatingItsSlab` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalTexturePoolReusesMatchingPrivateTextures` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalTexturePoolDoesNotEvictBusySubmissionResources` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalMaskTextureCacheUsesOneWorkspaceByteBudget` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalSecondEmptyRenderCreatesNoBufferTextureHeapOrPipelineState` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalFailedUploadRestoresDirtyFieldsAndPublishesNoResult` | `GpuDagMetalWorkspaceTest` | PASS |
| `MetalRenderDeviceUsesThePresentationDevice` | `GpuDagMetalWorkspaceTest` | PASS |

Also: `GpuDagRawInputTest.RendererTemplateInstantiatesMetalWithoutCudaHeaders` PASS (Metal host header remains Metal.hpp-free).

**Resource evidence:**

- buffer create/free: second empty render 0/0 (`MetalSecondEmptyRenderCreatesNoBufferTextureHeapOrPipelineState`; transient rewind 0 malloc/free)
- texture create/free: second matching acquire 0/0 (`MetalTexturePoolReusesMatchingPrivateTextures`)
- heap growth: second empty render 0 (`HeapCreateCount() == 0`)
- pipeline create/hit: first `convert_r32f_to_r32f` warm-up creates >= 1; second warm-up hits >= 1; second empty render create 0 (API snapshot, not logs)
- upload ranges/bytes: dirty sharpen amount is one 4-byte range; unchanged parameters upload 0 bytes

**Performance evidence:**

- device/macOS/Xcode/build: MacBook Air (Mac16,12) Apple M4, macOS 26.5.2 (25F84), Xcode 26.3 (17C529), `macos_debug_tests` Debug
- fixture and render request: `MetalRenderDevice` BeginRender/EndRender after peak reserve; no product RAW frame in M1
- cold: first reserve + first render allocates heap/buffer/texture/pipeline
- warm median: not a product-frame A/B; second empty render GPU resource create/free = 0
- warm p95: same; product A/B remains M7

**Remaining work owned by the next named Phase:**

- M2: encode-only Develop/Geometry/CameraColor on this command context and workspace; no RAW Processor product path; pixel comparison with CUDA.

**LOC note:** `metal_backend.hpp` 213, `plan_executor.hpp` 169, `metal_backend.mm` 785. No new file crossed 1000 LOC.

**Residual gaps:** PassEncoder Metal specializations still throw until M2–M6. LUT/Neural ownership slots exist as sampler/cache hooks only. Product present/download remains unimplemented until M6.

## 7. Phase M2 — Develop、Geometry 与 CameraColor

目标：

- 完整移植 CUDA DAG Develop 顺序；
- 保持三个独立内容缓存边界；
- 不调用 RawProcessor 的整体 Metal 产品路径。

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

1. 将现有 RAW Metal shader 暴露为 encode-only entrypoint，参数包括当前 command buffer、
   workspace texture/buffer 和不可变参数。
2. RCD、X-Trans 和 Neural 路径不再使用静态 scratch 或内部 command buffer。
3. RAW Processor 模块内的 RAW 专用 shader 继续放在
   `decoders/processor/operators/gpu/metal_shader/`。
4. 新增或重命名 RAW Metal shader 时，同步更新 `metal/CMakeLists.txt` 的 air/metallib、
   `RawProcessorOpMetalShaders` 和编译定义。
5. 通用 crop/resize/warp 保持在 `metal/metal_utils/`；DAG Geometry Pass 只负责编码与资源绑定。
6. Lens、Geometry 和 CameraColor 使用同一帧 command buffer。
7. CameraColor 读取共享 DevelopColorTransform 结果，不在 Metal 端重新解释 CameraMatrices。
8. CCT/tint 改变只能让 CameraColor 及下游失效。
9. source、sensor、geometry 和 develop 内容 key 与 CUDA 使用同一构造函数。

测试：

```text
MetalDevelopLinearizeMatchesCudaReferenceWithinTolerance
MetalDevelopRcdOrderMatchesCudaDemosaicThenHighlightRecovery
MetalDevelopXTransMatchesCudaReferenceWithinTolerance
MetalDevelopNeuralUsesSessionWorkspaceAndDoesNotSelectLegacyOnFailure
MetalGeometryUsesOneResampleForCropRotationViewportAndScale
MetalCameraColorConsumesSharedDualIlluminantTransform
MetalCctEditReusesSensorAndGeometryResults
MetalSecondDevelopRenderRunsNoSourceUploadOrDevelopPass
MetalDevelopPassesUseOneCommandBuffer
```

完成条件：

- 三个 GraphValueId 分别拥有内容 key；
- Develop 稳定重绘不分配 GPU 资源；
- RAW Metal operator 不保存长期 scratch；
- kernel/metallib 错误直接失败。

##### Phase M2 completion record (2026-08-26)

**Status:** complete
**Date:** 2026-08-26
**Branch:** `feature/gpu-dag-metal`
**Commit:** `f3522df5`

**Implemented:**

- Encode-only RAW Metal entrypoints (`metal_encode.hpp/.cpp`) that take the current command buffer plus workspace textures/buffers: linearize, CFA Clamp01, RCD, X-Trans, highlight reconstruct, pack/orient, DNG warp.
- `ExecuteMetalDevelop` / `ExecuteMetalGeometryResample` / `ExecuteMetalCameraColor` on `Renderer<MetalBackend>` / `PlanExecutor<MetalBackend>` with the CUDA Develop order and the three content-key boundaries (`develop.sensor_linear`, `geometry.scene_source`, `develop.image`).
- DAG geometry resample and camera-color ACEScc shaders under `edit/runtime/metal/shader/`; DNG warp texture encode stays in `metal/metal_utils/`.
- Neural Engine tiles encode onto the session command buffer (`MetalDemosaicNetTiledDispatch::command_buffer`); load failure throws and does not select Legacy.
- `MetalBackend::WarmUpPlan` warms Develop/Geometry/CameraColor pipeline states before execute. One command buffer per render (`CommandBufferCreateCount`).
- Identity texture copy for PrimaryGrade and DRT so PlanExecutor can finish M2 cache-skip tests. Pixel math for those passes remains M3/M6.

**Deleted:**

- None of the old Metal product wrappers. They remain until M7. The DAG path does not call them.

**Tests:**

- command: `cmake --preset macos_debug_tests`
- command: `cmake --build --preset macos_debug_tests --target GpuDagMetalDevelopTest GpuDagMetalWorkspaceTest --parallel 8`
- command: `ctest --test-dir build/macos-debug-tests -R GpuDagMetalDevelopTest --output-on-failure`
- result: 10/10 PASS
- command: `ctest --test-dir build/macos-debug-tests -R GpuDagMetalWorkspaceTest --output-on-failure`
- result: 8/8 PASS

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MetalDevelopLinearizeMatchesCudaReferenceWithinTolerance` | `GpuDagMetalDevelopTest` | PASS |
| `MetalDevelopRcdOrderMatchesCudaDemosaicThenHighlightRecovery` | `GpuDagMetalDevelopTest` | PASS |
| `MetalDevelopXTransMatchesCudaReferenceWithinTolerance` | `GpuDagMetalDevelopTest` | PASS |
| `MetalDevelopNeuralUsesSessionWorkspaceAndDoesNotSelectLegacyOnFailure` | `GpuDagMetalDevelopTest` | PASS |
| `MetalGeometryUsesOneResampleForCropRotationViewportAndScale` | `GpuDagMetalDevelopTest` | PASS |
| `MetalCameraColorConsumesSharedDualIlluminantTransform` | `GpuDagMetalDevelopTest` | PASS |
| `MetalCctEditReusesSensorAndGeometryResults` | `GpuDagMetalDevelopTest` | PASS |
| `MetalSecondDevelopRenderRunsNoSourceUploadOrDevelopPass` | `GpuDagMetalDevelopTest` | PASS |
| `MetalDevelopPassesUseOneCommandBuffer` | `GpuDagMetalDevelopTest` | PASS |
| Missing metallib throws (`MetalGeometryResampleMissingMetallibThrowsExplicitError`) | `GpuDagMetalDevelopTest` | PASS |

**Resource evidence:**

- buffer create/free: second stable Develop render 0/0 (`MetalSecondDevelopRenderRunsNoSourceUploadOrDevelopPass`)
- texture create/free: second stable Develop render 0/0
- heap growth: second stable Develop render 0
- pipeline create/hit: first WarmUpPlan creates Develop/Geometry/CameraColor states; second identical render create 0
- upload ranges/bytes: second identical render `source_h2d_count == 0`; CCT edit uploads only CameraColor parameter range and skips SensorDevelop/Geometry

**Performance evidence:**

- device/macOS/Xcode/build: MacBook Air (Mac16,12) Apple M4, macOS 26.5.2 (25F84), Xcode 26.3 (17C529), `macos_debug_tests` Debug
- fixture and render request: synthetic 64×64 Bayer / 64×64 X-Trans / 16×12 Direct RGB; `MetalRenderDevice::Execute` after pipeline warm-up
- cold: first reserve + first render allocates heap/buffer/texture/pipeline
- warm median: not a product-frame A/B; second identical Develop render GPU resource create/free = 0
- warm p95: same; product A/B remains M7

**Remaining work owned by the next named Phase:**

- M3: Primary Grade fusion (replace identity copy), ParameterArena slider dirty ranges, LUT texture cache.
- M4–M6: LLF, Mask/Mix, DRT/present. Identity DRT copy is not a display path.

**LOC note:** `metal_backend.hpp` 225, `metal_develop_pass.hpp` 41, `metal_pass_encoder.hpp` 68, `metal_encode.hpp` 53, `metal_backend.mm` 860, `metal_develop_pass.mm` 531, `metal_encode.cpp` 455. No new file crossed 1000 LOC.

**Residual gaps:** PrimaryGrade and DRT Metal encoders copy `develop.image` until M3/M6. Old RAW Metal wrappers still own static scratch for the pre-DAG product path. Product present/download remains unimplemented until M6.

## 8. Phase M3 — Primary Grade 融合路径

目标：

- 移植默认 Grade 调整列表；
- 保留调整顺序；
- pointwise 调整使用融合 dispatch；
- 参数只通过 ParameterArena 更新。

工作：

1. 将 backend-neutral adjustment semantic 与 CUDA/Metal PassEncoder 特化分开。
2. GraphCompiler 生成调整顺序和 ParameterArena binding，不生成 Metal 原生对象。
3. 使用一份 command buffer 保存参数 offset 顺序。
4. CAT02、Exposure、Contrast、White、Black、Curve、HLS、Saturation、Vibrance、Color Wheel
   和 LMT 在保持顺序的前提下融合。
5. Shadows/Highlights 在 LLF 边界前后拆成最多两个 pointwise dispatch。
6. Clarity、Sharpen、Halation 和 Film Grain 根据邻域需求生成明确 pass，临时纹理来自 TexturePool。
7. LUT texture 由 MetalSharedGpuResources 按内容 key 和字节预算保存；删除 MetalLutBuffer
   的独立生命周期。
8. 默认值不改变图结构；slider patch 不重编译 ExecutionPlan。
9. 本 Phase 只要求当前 CUDA DAG 的 CAT02 参数行为一致，不增加 G7R.4 UI 或数学范围。

测试：

```text
MetalPrimaryGradePreservesCompiledAdjustmentOrder
MetalPointwiseAdjustmentsUseOneDispatchPerLlfSegment
MetalSingleSliderEditUploadsOnlyItsParameterRange
MetalExposureEditRunsOnlyPrimaryGradeAndDrt
MetalLutTextureIsReusedByContentKey
MetalDetailPassesAcquireAllTexturesFromWorkspace
MetalPrimaryGradeMatchesCudaReferenceWithinTolerance
MetalUnknownAdjustmentReturnsExplicitBackendError
```

性能门槛：

- 不允许一个 pointwise adjustment 对应一次 command buffer 或一次完整纹理往返；
- 相同拓扑的 slider 编辑不创建 buffer、texture 或 pipeline state；
- Grade command offset buffer 只在拓扑改变时更新；
- LUT 未变化时上传字节为零。

##### Phase M3 completion record (2026-08-26)

**Status:** complete
**Date:** 2026-08-26
**Branch:** `feature/gpu-dag-metal`
**Commit:** `b5622cfe`

**Implemented:**

- Backend-neutral `AdjustmentBehavior` / `GradeAdjustmentParams` / `MakeGradeRuntimeParams` in `EditRuntime`. CUDA aliases remain in `cuda_adjustment_runtime.hpp`.
- GraphCompiler records Neighborhood for Clarity/Sharpen/Halation/Film Grain and fused `primary_grade_stages` (pointwise segments around LocalLaplacian). Slider values still do not recompile.
- `ExecuteMetalPrimaryGrade` on `Renderer<MetalBackend>` / `PlanExecutor<MetalBackend>`: ParameterArena dirty ranges, one command-offset buffer uploaded only when topology changes, fused pointwise dispatch per LLF segment, explicit TexturePool detail passes, Normal Mix without a fake mask texture.
- LUT voxels owned by MetalBackend content-key cache with a byte budget. The DAG path does not use `MetalLutBuffer`.
- `primary_grade.metal` (`primary_grade_pointwise`, `primary_grade_mix`) compiled into `GpuDagMetalShaders`. `WarmUpPlan` warms those states. Missing metallib throws.

**Deleted:**

- CUDA-only adjustment runtime TU (`cuda_adjustment_runtime.cpp` is no longer a compile source; symbols live in shared `adjustment_runtime.cpp`).
- Metal PrimaryGrade identity copy. DRT still copies `grade.primary:image` until M6. Old `MetalLutBuffer` remains for the pre-DAG fused path until M7.

**Tests:**

- command: `cmake --preset macos_debug_tests`
- command: `cmake --build --preset macos_debug_tests --target GpuDagMetalGradeTest GpuDagMetalDevelopTest GpuDagMetalWorkspaceTest GpuDagRawInputTest --parallel 8`
- command: `ctest --test-dir build/macos-debug-tests -R GpuDagMetalGradeTest --output-on-failure`
- result: 9/9 PASS
- command: `ctest --test-dir build/macos-debug-tests -R 'GpuDagMetalWorkspaceTest|GpuDagMetalDevelopTest|GpuDagRawInputTest.GpuDagGraphCompiler' --output-on-failure`
- result: 8/8 workspace, 10/10 develop, 15/15 GraphCompiler PASS

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MetalPrimaryGradePreservesCompiledAdjustmentOrder` | `GpuDagMetalGradeTest` | PASS |
| `MetalPointwiseAdjustmentsUseOneDispatchPerLlfSegment` | `GpuDagMetalGradeTest` | PASS |
| `MetalSingleSliderEditUploadsOnlyItsParameterRange` | `GpuDagMetalGradeTest` | PASS |
| `MetalExposureEditRunsOnlyPrimaryGradeAndDrt` | `GpuDagMetalGradeTest` | PASS |
| `MetalLutTextureIsReusedByContentKey` | `GpuDagMetalGradeTest` | PASS |
| `MetalDetailPassesAcquireAllTexturesFromWorkspace` | `GpuDagMetalGradeTest` | PASS |
| `MetalPrimaryGradeMatchesCudaReferenceWithinTolerance` | `GpuDagMetalGradeTest` | PASS |
| `MetalUnknownAdjustmentReturnsExplicitBackendError` | `GpuDagMetalGradeTest` | PASS |
| Missing metallib throws (`MetalPrimaryGradeMissingMetallibThrowsExplicitError`) | `GpuDagMetalGradeTest` | PASS |

**Resource evidence:**

- buffer create/free: second slider / detail / LUT-stable render 0/0 (`MetalSingleSliderEditUploadsOnlyItsParameterRange`, `MetalDetailPassesAcquireAllTexturesFromWorkspace`, `MetalLutTextureIsReusedByContentKey`)
- texture create/free: second matching Grade render 0/0
- heap growth: second matching Grade render 0
- pipeline create/hit: first WarmUpPlan creates `primary_grade_pointwise` / `primary_grade_mix`; second identical render create 0
- upload ranges/bytes: Exposure slider uploads one ParameterArena slot range; unchanged LUT `LutUploadBytes() == 0`; command-offset buffer is not rewritten when topology is unchanged

**Performance evidence:**

- device/macOS/Xcode/build: MacBook Air (Mac16,12) Apple M4, macOS 26.5.2 (25F84), Xcode 26.3 (17C529), `macos_debug_tests` Debug
- fixture and render request: synthetic Direct RGB 16×12; `ExecuteMetalPrimaryGrade` / `MetalRenderDevice::Execute` after pipeline warm-up
- cold: first reserve + first render allocates heap/buffer/texture/pipeline
- warm median: not a product-frame A/B; second identical Grade render GPU resource create/free = 0; one fused pointwise dispatch when LLF is inactive, two when Shadows is non-zero
- warm p95: same; product A/B remains M7

**Remaining work owned by the next named Phase:**

- M4: LLF workspace pyramids. M3 copies through the LLF barrier so pointwise order is preserved; it does not run Local Laplacian.
- M5: Mask/Feather/Mix with R8 and signed distance. Disconnected mask still uses constant 1.
- M6: DRT/present. Identity DRT copy is not a display path.

**LOC note:** `adjustment_runtime.hpp` 74, `metal_primary_grade_pass.hpp` 43, `metal_backend.hpp` 262, `metal_primary_grade_pass.mm` 450, `primary_grade.metal` 242, `metal_grade_test.cpp` 427. No new file crossed 1000 LOC.

**Residual gaps:** LLF pixel math waits for M4. DRT Metal encoder still copies Grade output until M6. Old RAW Metal wrappers still own static scratch for the pre-DAG product path.

## 9. Phase M4 — LLF workspace 化

目标：

- 对齐 CUDA canonical LLF reference 和 ROI sampling；
- 删除 `highlight_shadow_local_tone::MetalStage` 的内存与缓存所有权。

工作：

1. 把 source/remap/sample/output pyramid 的峰值写入 ExecutionPlan transient 需求。
2. 临时 pyramid buffer 全部从 TransientBufferArena 分配。
3. canonical reference 使用 workspace GraphValueId、内容 key 和 submission-safe lease。
4. ROI frame 使用 `MakeLlfSamplingPlan` 映射到 canonical reference。
5. 全图 reference 未建立时按 CUDA 当前规则计算，不伪造命中。
6. Shadows/Highlights 参数改变复用不依赖该参数的 canonical reference。
7. 删除静态或 MetalStage-owned pyramid arrays、cached key 和 allocator。
8. LLF pipeline state 继续通过 ComputePipelineCache 获取。

测试：

```text
MetalLlfUsesWorkspaceTransientArenaForEveryPyramid
MetalLlfFullFrameBuildsCanonicalReferenceOnce
MetalLlfRoiSamplesCanonicalReferenceWithSharedGeometryPlan
MetalLlfSliderEditReusesCanonicalReference
MetalLlfFailedSubmissionDoesNotPublishReference
MetalLlfSecondStableRenderCreatesNoBufferTextureOrPipelineState
MetalLlfMatchesCudaReferenceWithinTolerance
```

##### Phase M4 completion record (2026-08-26)

**Status:** complete
**Date:** 2026-08-26
**Branch:** `feature/gpu-dag-metal`
**Commit:** `94415693`

**Implemented:**

- `ExecuteMetalLocalTone` encode-only LLF on the current command buffer. Source/remap/result pyramids allocate from `TransientBufferArena`. Canonical `local_tone.source.0` / `local_tone.result.0` live in workspace `Values()` with GraphValueId and `HashLlfSourceKey` / `HashLlfReferenceKey`.
- Full EditSpace frames seed the canonical reference through `reference_to_render`. Later ROI frames sample it with `MakeLlfSamplingPlan`. Isolated ROI without a canonical plane rebuilds locally and does not mark a hit.
- Shadows/Highlights slider edits reuse the canonical source plane (`HashLlfSourceKey` omits those slider values) and rebuild only the adjusted result.
- `GraphCompiler` writes LLF pyramid peak bytes into `ExecutionPlan.peak_transient_bytes`. Pipeline states come from `ComputePipelineCache` (`local_tone.metal` → `GpuDagMetalShaders`). Missing metallib throws.
- DAG path does not call `highlight_shadow_local_tone::MetalStage`. That type still exists for the pre-DAG fused pipeline until M7.

**Deleted:**

- None of the old Metal product wrappers. `MetalStage` remains until M7. The DAG path does not own its pyramid arrays, cached keys, or allocator.

**Tests:**

- command: `cmake --preset macos_debug_tests`
- command: `cmake --build --preset macos_debug_tests --target GpuDagMetalGradeTest GpuDagMetalDevelopTest GpuDagMetalWorkspaceTest GpuDagRawInputTest --parallel 8`
- command: `ctest --test-dir build/macos-debug-tests -R 'GpuDagMetalGradeTest|GpuDagMetalWorkspaceTest|GpuDagMetalDevelopTest|GpuDagRawInputTest.GpuDagGraphCompiler|GpuDagRawInputTest.GpuDagResultContentKey' --output-on-failure`
- result: 17/17 grade (9 M3 + 8 M4), 8/8 workspace, 10/10 develop, 15/15 GraphCompiler, 11/11 ResultContentKey PASS

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MetalLlfUsesWorkspaceTransientArenaForEveryPyramid` | `GpuDagMetalGradeTest` | PASS |
| `MetalLlfFullFrameBuildsCanonicalReferenceOnce` | `GpuDagMetalGradeTest` | PASS |
| `MetalLlfRoiSamplesCanonicalReferenceWithSharedGeometryPlan` | `GpuDagMetalGradeTest` | PASS |
| `MetalLlfSliderEditReusesCanonicalReference` | `GpuDagMetalGradeTest` | PASS |
| `MetalLlfFailedSubmissionDoesNotPublishReference` | `GpuDagMetalGradeTest` | PASS |
| `MetalLlfSecondStableRenderCreatesNoBufferTextureOrPipelineState` | `GpuDagMetalGradeTest` | PASS |
| `MetalLlfMatchesCudaReferenceWithinTolerance` | `GpuDagMetalGradeTest` | PASS |
| Missing metallib throws (`MetalLocalToneMissingMetallibThrowsExplicitError`) | `GpuDagMetalGradeTest` | PASS |

**Resource evidence:**

- buffer create/free: second stable LLF render 0/0 (`MetalLlfSecondStableRenderCreatesNoBufferTextureOrPipelineState`); slider edit reuses `local_tone.source.0` ResourceId (`MetalLlfSliderEditReusesCanonicalReference`)
- texture create/free: second matching LLF render 0/0
- heap growth: second matching LLF render 0
- pipeline create/hit: first WarmUp/GetPipelineState creates extract/pyr_down/remap/select/collapse/apply; second identical render create 0 (API snapshot, not logs)
- upload ranges/bytes: canonical source is not re-extracted on a Shadows slider edit; failed encode leaves the previous canonical ResourceId unpublished for the failed frame

**Performance evidence:**

- device/macOS/Xcode/build: MacBook Air (Mac16,12) Apple M4, macOS 26.5.2 (25F84), Xcode 26.3 (17C529), `macos_debug_tests` Debug
- fixture and render request: synthetic Direct RGB 16×12 / 32×32 / 64×64 split and neighborhood planes; `ExecuteMetalPrimaryGrade` after Develop/Geometry/CameraColor
- cold: first reserve + first LLF render allocates heap/buffer/texture/pipeline and transient pyramids
- warm median: not a product-frame A/B; second identical full-frame LLF render GPU resource create/free = 0 and samples the canonical plane
- warm p95: same; product A/B remains M7

**Remaining work owned by the next named Phase:**

- M5: Mask/Feather/Mix with R8 and signed distance. Disconnected mask still uses constant 1.
- M6: DRT/present. Identity DRT copy is not a display path.
- M7: delete `highlight_shadow_local_tone::MetalStage` and the old fused Metal product path.

**LOC note:** `metal_local_tone_pass.hpp` 46, `metal_local_tone_pass.mm` 543, `local_tone.metal` 339, `metal_llf_test.cpp` 703, `metal_backend.mm` 929. No new file crossed 1000 LOC.

**Residual gaps:** DRT Metal encoder still copies Grade output until M6. Old RAW Metal wrappers and `MetalStage` still own static scratch for the pre-DAG product path.

## 10. Phase M5 — Mask、Feather 与 Mix

目标：

- 完成 R8 Raster/Analytic Mask；
- 完成 exact signed Euclidean distance field；
- Grade 只存在一个 Normal Mix 出口。

工作：

1. Raster mask 从 MaskStore 读取，上传到 workspace MaskTextureCache。
2. R8 dirty rect 使用 Metal blit，不上传未变化区域。
3. mip level 由统一 cache 持有，不由 mask pass 持有。
4. signed distance 中间资源来自 transient arena；距离结果按 mask content key 保存。
5. feather radius 改变复用 signed distance 结果。
6. Analytic mask 和 Raster mask 都使用 ResolvedRenderGeometry/TextureSamplingPlan。
7. Mix kernel 读取原 Grade 输入、调整结果、grade mix 和可选 mask。
8. 未连接 mask 时使用常数 1，不创建虚假白色纹理。

测试：

```text
MetalRasterMaskUploadsOnlyChangedR8Rectangle
MetalRasterMaskMipChainUsesWorkspaceCache
MetalMaskFeatherMatchesExactSignedDistanceReference
MetalFeatherRadiusEditReusesSignedDistanceResult
MetalMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution
MetalDisconnectedMaskUsesConstantOneWithoutTextureAllocation
MetalNormalMixMatchesCudaReferenceWithinTolerance
MetalMaskCacheDoesNotEvictBusyTextures
```

##### Phase M5 completion record (2026-08-26)

**Status:** complete
**Date:** 2026-08-26
**Branch:** `feature/gpu-dag-metal`
**Commit:** `fd1850e9`

**Implemented:**

- `ExecuteMetalMask` encode-only MaskEvaluate/MaskFeather on the current command buffer. Raster R8 assets load from MaskStore into workspace `MaskTextureCache` mip chains. Dirty rectangles union to one blit; unchanged texels are not uploaded.
- Analytic Radial/GraduatedNd and raster sampling both use `ResolvedRenderGeometry` / `MakeRasterMaskSamplingPlan`. Feather uses exact signed Euclidean distance (parallel-band horizontal/vertical + compose) matching the CUDA coverage, plateau, and antialiased-boundary rules.
- Signed-distance intermediates allocate from `TransientBufferArena`. The signed-distance result lives in workspace `Values()` under GraphValueId and a mask content key (asset key + extent, no feather radius). A radius-only edit reuses the same ResourceId.
- Primary Grade has one Normal Mix exit. Connected masks multiply `grade.Mix` by the RenderSpace R8 coverage. Disconnected masks use constant 1 and do not allocate a white R8 texture.
- `GraphCompiler` adds raster signed-distance peak bytes to `ExecutionPlan.peak_transient_bytes`. Mask pipeline states come from `ComputePipelineCache` (`mask.metal` → `GpuDagMetalShaders`). Missing metallib throws.

**Deleted:**

- None of the old Metal product wrappers. The DAG path does not call fused-pipeline mask or CPU image processing.

**Tests:**

- command: `cmake --preset macos_debug_tests`
- command: `cmake --build --preset macos_debug_tests --target GpuDagMetalGradeTest GpuDagMetalDevelopTest GpuDagMetalWorkspaceTest GpuDagRawInputTest --parallel 8`
- command: `ctest --test-dir build/macos-debug-tests -R 'GpuDagMetalGradeTest|GpuDagMetalWorkspaceTest|GpuDagMetalDevelopTest|GpuDagRawInputTest.GpuDagGraphCompiler|GpuDagRawInputTest.GpuDagResultContentKey' --output-on-failure`
- result: 25/25 grade (9 M3 + 8 M4 + 8 M5), 8/8 workspace, 10/10 develop, 15/15 GraphCompiler, 11/11 ResultContentKey PASS

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MetalRasterMaskUploadsOnlyChangedR8Rectangle` | `GpuDagMetalGradeTest` | PASS |
| `MetalRasterMaskMipChainUsesWorkspaceCache` | `GpuDagMetalGradeTest` | PASS |
| `MetalMaskFeatherMatchesExactSignedDistanceReference` | `GpuDagMetalGradeTest` | PASS |
| `MetalFeatherRadiusEditReusesSignedDistanceResult` | `GpuDagMetalGradeTest` | PASS |
| `MetalMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution` | `GpuDagMetalGradeTest` | PASS |
| `MetalDisconnectedMaskUsesConstantOneWithoutTextureAllocation` | `GpuDagMetalGradeTest` | PASS |
| `MetalNormalMixMatchesCudaReferenceWithinTolerance` | `GpuDagMetalGradeTest` | PASS |
| `MetalMaskCacheDoesNotEvictBusyTextures` | `GpuDagMetalGradeTest` | PASS |

**Resource evidence:**

- buffer create/free: feather-radius edit reuses the signed-distance ResourceId and reports 0 additional SDF transient bytes (`MetalFeatherRadiusEditReusesSignedDistanceResult`)
- texture create/free: same MaskAssetKey at full and half render scale keeps the persistent R8 ResourceId and mip chain (`MetalRasterMaskMipChainUsesWorkspaceCache`)
- dirty upload: unioned R8 rectangle `{1,1,4,4}` and 16 host-to-device bytes (`MetalRasterMaskUploadsOnlyChangedR8Rectangle`)
- cache: busy submission textures are not evicted under a 1-byte budget (`MetalMaskCacheDoesNotEvictBusyTextures`)
- disconnected mix: `MaskTextures` entry count stays 0 and no mask GraphValueId is allocated (`MetalDisconnectedMaskUsesConstantOneWithoutTextureAllocation`)

**Performance evidence:**

- device/macOS/Xcode/build: MacBook Air (Mac16,12) Apple M4, macOS 26.5.2 (25F84), Xcode 26.3 (17C529), `macos_debug_tests` Debug
- fixture and render request: synthetic Direct RGB 16×12 / 9×9; `ExecuteMetalMask` and `ExecuteMetalPrimaryGrade` after Develop/Geometry/CameraColor
- cold: first raster upload + mip chain + optional signed-distance transients allocate heap/buffer/texture/pipeline
- warm median: not a product-frame A/B; identical mask asset at a new render scale reuses the persistent texture; radius-only feather reuses signed distance
- warm p95: same; product A/B remains M7

**Remaining work owned by the next named Phase:**

- M6: DRT/present. Identity DRT copy is not a display path.
- M7: delete `highlight_shadow_local_tone::MetalStage` and the old fused Metal product path.

**LOC note:** `metal_mask_pass.hpp` 45, `metal_mask_pass.mm` 511, `mask.metal` 322, `metal_mask_test.cpp` 439, `metal_primary_grade_pass.mm` 476, `primary_grade.metal` 257. No new file crossed 1000 LOC.

**Residual gaps:** DRT Metal encoder still copies Grade output until M6. Old RAW Metal wrappers and `MetalStage` still own static scratch for the pre-DAG product path.

## 11. Phase M6 — DRT、scope、present 与产品切换

目标：

- 完成 Metal DAG 端到端产品路径；
- 直接提交 display texture；
- macOS 编辑器不再进入旧 Metal stage adapter。

工作：

1. 移植 ACES 2.0 和 OpenDRT；输入固定为 AP1/ACEScc，输出为用户选择的 display encoding。
2. DRT output 使用 workspace RGBA32F texture。
3. `Renderer<MetalBackend>` 通过 Metal Presenter 向 IFrameSink 提交 retained MTLTexture。
4. scope tap 读取同一最终纹理与同一 submission signal，不做 host round-trip。
5. 显式 host 输出只在 export/test 请求时下载；它不是失败替代路径。
6. session cache 与 one-shot workspace 保持隔离。
7. PipelineMgmtService 返回管线时等待设备空闲并释放 session results、transients、参数和
   Neural activation workspace。
8. macOS Auto/Metal 产品选择直接创建 MetalRenderer。
9. present、scope 或下载失败时不发布本次结果内容 key。

测试：

```text
MetalDrtAcesMatchesCudaReferenceWithinTolerance
MetalDrtOpenDrtMatchesCudaReferenceWithinTolerance
MetalDrtEditRunsOnlyDrtPass
MetalRendererPresentsWorkspaceTextureWithoutHostDownload
MetalScopeTapUsesTheFinalDisplayTextureAndSubmissionSignal
MetalOneShotRenderDoesNotPublishIntoSessionCache
MetalPipelineReturnReleasesSessionResourcesAfterGpuCompletion
MetalBackendFailureDoesNotEnterCpuOrLegacyMetalExecution
MetalRealRawEditorUsesTheThreeNodeDag
```

## 12. Phase M7 — 旧 Metal 管线删除与性能验收

目标：

- 删除 macOS 产品路径中的旧 pipeline、stage 和混合 operator 执行；
- 证明新 Metal DAG 从第一帧到稳定帧满足性能和资源目标。

Metal 专属删除项：

- `edit/pipeline/pipeline_metal_impl.cpp`；
- Metal `CreateMetalGPUPipeline` 和旧 `GPUPipelineImpl` 分支；
- `edit/pipeline/metal_kernel_dispatch.hpp`；
- `edit/pipeline/metal_pipeline_stats.hpp` 中只服务旧路径的结构；
- `edit/operators/GPU_kernels/metal_param.hpp`；
- MetalFusedParams、MetalFusedResources 和 MetalFusedParamUploader；
- `highlight_shadow_local_tone::MetalStage`；
- 旧 fused stage 参数 buffer 和 neighbor stage buffer；
- 每帧创建 MetalImage scratch 的旧入口；
- 已被 DAG shader 覆盖的 `fused_pipeline.metal` 入口；
- 旧 Metal stage adapter 的 CMake source、metallib、编译定义和 bundle 安装项；
- 已迁移算子的 Metal ApplyGPU/私有 GPU 资源入口。

共享删除边界：

- Metal 完成后，macOS 产品源码不得引用 PipelineStage、PipelineStageName、OperatorParams、
  GPUPipelineWrapper 或 merged stage；
- 全局共享类型的物理删除继续由 GPU DAG 主计划 G10 执行，因为 OpenCL 在完成 G8 前仍可能
  编译引用这些类型；
- G10 不是保留旧 Metal 产品路径的理由；Metal M7 必须先删除所有 Metal 引用和实现；
- 不允许保留 build flag 重新启用旧 Metal pipeline。

静态检查：

```text
NoMetalProductSourceReferencesPipelineStage
NoMetalProductSourceReferencesOperatorParams
NoMetalRuntimeOwnsOperatorPrivateScratchOrCache
NoLegacyMetalPipelineFactoryRemains
NoLegacyMetalFusedParameterAggregateRemains
NoLegacyMetalMetallibIsPackaged
```

## 13. 性能验收

### 13.1 基线

在同一台 Mac、同一 macOS、同一 Xcode/Metal compiler、同一 Release 配置、同一 RAW、同一
DecodeRes、同一 viewport 和同一编辑序列下比较：

1. 本计划开始前的旧 Metal 产品路径；
2. Metal DAG 最终路径；
3. CUDA reference 的 pass 执行集、缓存失效范围和像素 reference。

CUDA 和 Metal 不在不同硬件之间比较绝对 wall time。跨后端比较像素、pass 数、缓存边界、
H2D 字节和稳定帧资源创建；绝对性能只在同一台 Mac 上做旧/新 Metal A/B。

### 13.2 场景

每个场景记录一次 cold render 和至少 30 次 warm render：

| 场景 | 允许执行的 GPU 图像工作 | 必须为零 |
| --- | --- | --- |
| 无变化重复渲染 | 已完成结果提交与 scope/present | LibRaw、source upload、plan compile、全部节点 pass、GPU resource create/free |
| Exposure 连续调整 | PrimaryGrade、DRT | SensorDevelop、Geometry、CameraColor、资源创建/释放 |
| CCT/tint 连续调整 | CameraColor、PrimaryGrade、DRT | SensorDevelop、Geometry、资源创建/释放 |
| DRT 连续调整 | DRT | SensorDevelop、Geometry、CameraColor、PrimaryGrade、资源创建/释放 |
| viewport/geometry 改变 | Geometry 及下游 | SensorDevelop、source upload、资源创建/释放 |
| Develop 参数改变 | SensorDevelop 及下游 | LibRaw 重复 open/unpack、无关 source preparation、资源创建/释放 |
| 切图后切回 | 预算内内容命中与 present | 命中项的 LibRaw、source upload、节点 pass、资源创建/释放 |

### 13.3 数值门槛

- warm interactive median 不高于旧 Metal 基线的 `1.05x`；
- warm interactive p95 不高于旧 Metal 基线的 `1.10x`；
- 无变化、Exposure、CCT、DRT 和 viewport 编辑从第二帧起 GPU resource create/free 为零；
- 稳定帧 compute pipeline create 为零；
- 参数上传只覆盖 dirty range；
- source 未变化时 source upload bytes 为零；
- pointwise Grade dispatch 数不得随普通 pointwise adjustment 数线性增长；
- 不得用降低质量、缩小输入或跳过要求的算法满足门槛；
- 未达到任一门槛时 M7 不得标记 complete。

## 14. 构建与 shader 组织

Metal runtime host 文件使用 Objective-C++，模板实例放在 `.mm` 中，普通共享头不包含
Metal.hpp。

建议目标：

```text
EditRuntimeMetal
GpuDagMetalShaders
GpuDagMetalDevelopTest
GpuDagMetalWorkspaceTest
GpuDagMetalGradeTest
GpuDagMetalMaskTest
GpuDagMetalDrtTest
GpuDagMetalRendererTest
GpuDagMetalPerformanceTest
```

shader 组织：

```text
edit/runtime/metal/shader/
  geometry_resample.metal
  camera_color.metal
  primary_grade.metal
  local_tone.metal
  mask.metal
  drt.metal

decoders/processor/operators/gpu/metal_shader/
  RAW 专用 linearize/demosaic/highlight/neural shader

metal/metal_utils/
  可跨 edit 与 RAW Processor 使用的 geometry/blit/format 工具
```

每个新增 metallib 必须：

- 由 CMake 明确编译 `.metal -> .air -> .metallib`；
- 加入正确 shader target；
- 暴露路径给匹配 runtime target；
- 加入 app bundle/install 列表；
- 由 ComputePipelineCache 加载；
- 有缺失 metallib 和缺失 function 的明确错误测试。

## 15. 完成条件

### 15.1 架构

- [ ] Metal 使用 GPU DAG 主计划的 PipelineDocument、GraphCompiler、ExecutionPlan、DTO、
      dirty Patch、Geometry 和内容 key。
- [ ] 产品入口命名为 `Renderer<Backend>`，存在 `CudaRenderer` 和 `MetalRenderer` 别名。
- [ ] 后端通过模板 Traits 和 PassEncoder 特化区分。
- [ ] macOS 产品路径不引用 PipelineStage、OperatorParams 或 GPUPipelineWrapper。
- [ ] Metal Pass 不拥有独立 pool、LRU、长期 scratch 或 command queue。

### 15.2 资源

- [ ] 每个 Metal RenderDevice 只有一个 workspace 和一个 command context。
- [ ] 所有 transient buffer、普通 texture、mask texture、结果 texture 和 LUT 都经过统一所有者。
- [ ] 稳定 render 不创建或销毁 buffer、texture、heap 或 pipeline state。
- [ ] busy submission 的资源不会被复用或淘汰。
- [ ] cache hit 由内容 key 证明。

### 15.3 行为

- [ ] Develop、Geometry、CameraColor、Grade、LLF、Mask、Mix 和 DRT 与 CUDA reference 在各自
      容差内一致。
- [ ] 同一调整顺序在 Metal 上不被重排。
- [ ] crop、rotation、viewport 和 dynamic resolution 只做一次图像重采样。
- [ ] GPU/Neural/present 失败不进入任何替代图像处理路径。
- [ ] scope 和 present 使用最终 Metal display texture。

### 15.4 删除

- [ ] 旧 Metal pipeline factory 和实现已删除。
- [ ] MetalFusedParams/MetalFusedParamUploader 已删除。
- [ ] MetalStage 私有 LLF cache/allocator 已删除。
- [ ] 旧 Metal fused/stage metallib 不再构建或打包。
- [ ] Metal 已迁移 operator 不再提供旧图像执行入口。

### 15.5 性能

- [ ] 第 13 节全部场景有 cold、median、p95 和资源计数记录。
- [ ] 同机旧/新 Metal A/B 达到第 13.3 节门槛。
- [ ] 每个 Phase 的 completion record 已写入实际设备、系统、构建配置、fixture、命令和结果。

## 16. Phase 完成记录格式

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

Resource evidence:
- buffer create/free:
- texture create/free:
- heap growth:
- pipeline create/hit:
- upload ranges/bytes:

Performance evidence:
- device/macOS/Xcode/build:
- fixture and render request:
- cold:
- warm median:
- warm p95:

Remaining work owned by the next named Phase:
-
```
