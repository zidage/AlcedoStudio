# Vulkan Compute Backend Master Plan

Date: 2026-11-22

Status: planned. Experiments VX1 to VX5 come first. VX1 and VX2 are complete (2026-11-22,
sections 9.1 and 9.2). VX2 changed B5 to two devices and stopped VK18 until the plan has a new
resize design (section 9.2). No implementation phase has started.

Tracking issue: [#1036](https://github.com/zidage/AlcedoStudio/issues/1036)

Source revision of the audit: `61d59bc98` (`main`)

Experiment branch: `feature/vulkan-backend-experiments` (worktree, does not merge)

Related plans:

- [GPU DAG Pipeline Rebuild Phase Plan](gpu_dag_pipeline_rebuild_phase_plan.md): the shared graph,
  the execution plan, and the caches. This plan depends on it and does not change it.
- [GPU DAG Metal Migration Phase Plan](gpu_dag_metal_migration_phase_plan.md) and
  [GPU DAG OpenCL Migration Phase Plan](gpu_dag_opencl_migration_phase_plan.md): the two earlier
  backend ports. This plan uses the same port pattern.
- [QML Editor and Qt RHI Unified Workspace Refactor Plan](../ui/qml_editor_rhi_unified_workspace_plan.md):
  the frame lease and present path. Phase VK18 extends it.
- [Windows Display ICC Color Management Plan](../ui/windows_display_icc_color_management_plan.md):
  the display transform that the Vulkan swapchain must keep.
- AI Denoise Integration Master Plan (pull request #1031, tracking issue #1030): the UtNet2 model.
  Phase VK20 depends on it.

## 1. Decision record

### 1.1 Goal

The image pipeline on Windows has two compute backends, CUDA and OpenCL. Each backend has its own
display pairing: CUDA with Qt Quick on Direct3D 11, and OpenCL with Qt Quick on OpenGL. macOS has a
third backend, Metal. Each operator, RAW stage, and neural network layer is written three times.
A defect can be in one backend only. The number of backends increases the defect rate and decreases
the development speed.

The target is one Vulkan compute backend on Windows. Qt Quick renders with Vulkan on the same
`VkDevice`. The pipeline gives the finished frame to the editor viewport as a `VkImage`. The CUDA
backend, the OpenCL backend, the Direct3D 11 pairing, and the OpenGL pairing go to the archive when
Vulkan is the Windows default.

### 1.2 Approved decisions

These decisions come from the user (2026-11-22).

| Id | Decision |
| --- | --- |
| A1 | The cost of the rewrite is not a factor in the design. A one-time Vulkan state design is acceptable. |
| A2 | Feature work on `main` continues on the current backends during this work. A release must not wait for this work. |
| A3 | Experiments run in a dedicated worktree from `main`. |
| A4 | The Vulkan backend follows the structure of the current backends: the same templates, the same pass list, the same test layout. |
| A5 | The neural networks that run in the pipeline move to Vulkan compute: DemosaicNet and the UtNet2 model of AI Denoise. |
| A6 | The mask models stay in the Rust sidecar. This plan does not change them. |
| A7 | The CUDA libraries have little use in the product. Their absence in Vulkan is acceptable. |

### 1.3 Decisions of this plan

The plan author made these decisions. Each one agrees with A1 to A7.

| Id | Decision | Reason |
| --- | --- | --- |
| B1 | Vulkan is a fourth backend on `main` behind the build option `ALCEDO_ENABLE_VULKAN`. The option is `OFF` in release builds until VK19. | A long-lived fork must merge each new kernel and each interface change of `main`. An added backend does not touch the three current backends, so A2 is true without a fork. |
| B2 | The worktree branch holds experiment code only. Experiment code does not merge. Its findings go into section 9. | Experiment code answers a question. It does not follow the structure rules of the product code. |
| B3 | The kernel language is Slang. The build compiles each kernel group to SPIR-V with `slangc`. VX1 confirmed this decision (section 9.1). | Slang includes the shared math headers, `film_print_math.h` included, with a macro layer of 28 lines. GLSL has no pointer syntax and cannot include that header. One Slang source can also give Metal Shading Language. |
| B4 | The minimum is Vulkan 1.3. | Vulkan 1.3 includes dynamic rendering, synchronization2, and maintenance4 in the core. The two devices of the development computer report 1.4. |
| B5 | Alcedo creates the `VkInstance` and two `VkDevice` objects on one `VkPhysicalDevice`: the display device and the pipeline device. Qt Quick receives the display device through `QQuickGraphicsDevice::fromDeviceObjects`. A frame image is allocated on the pipeline device with Win32 external memory and imported into the display device (zero copy). VX2 changed this decision from one device (section 9.2). | Qt calls `vkDeviceWaitIdle` on its device at each swapchain resize, before `beforeFrameBegin`. That call needs exclusive access to every queue of the device, so a pipeline queue on the same device races with it: the validation layer reports it with a second queue and with the queue mutex, and a run without the mutex crashed. On a second device Qt's call touches no pipeline queue. A device with one queue (Iris Xe) also needs no mutex, because each device gets its own `VkQueue`. One physical device keeps one memory heap and one budget. |
| B6 | There is one editor pairing for Vulkan: Vulkan pipeline with Qt Quick Vulkan. A pairing of Vulkan compute with Direct3D 11 or OpenGL does not exist. | A mixed pairing brings back an interop API. |
| B7 | Before VK18, the Vulkan backend is available to tests, export, thumbnails, and `alcedo-cli` only. These paths use the explicit host download that the current backends also have. | The display change is the only step that is not additive. It comes after all passes are equal. |
| B8 | A Vulkan failure reports the real error. It does not select CUDA, OpenCL, or a host path. | `AGENTS.md`, "No fallback without explicit authorization". |
| B9 | A Vulkan pass is complete when a test renders one input on Vulkan and on OpenCL and the difference is in the tolerance of the matching OpenCL test. The input has the value range of the input of that OpenCL test: a developed RAW image, or a synthetic image in the same range. | OpenCL is the backend that tests select through Auto. The comparison uses rendered values and a tolerance. It stores no expected pixels. VX1: an input with channel spreads of 4.4 stops gives differences above the grade tolerance between OpenCL on NVIDIA and OpenCL on Intel too (section 9.1). |
| B10 | New features go to the current backends first. The Vulkan implementation of a new kernel follows in the phase that owns its pass. | A requirement to write each new feature four times decreases the feature speed, which A2 prohibits. |
| B11 | The archive of CUDA and OpenCL moves the code to `alcedo_studio/deprecated/`. It does not delete it. | Repository rule for legacy code. |

### 1.4 Exclusions

- macOS. The Metal backend keeps its kernels and its display path. MoltenVK is not used.
- Linux packaging and a Linux editor backend. The Vulkan code must compile without a Windows API
  outside the window surface, but this plan adds no Linux target.
- The Rust sidecar and its ONNX Runtime (A6).
- The shared graph, the execution plan, the content keys, and the caches. They stay as the GPU DAG
  master plan defines them.
- The legacy WebGPU and CPU render paths. They are out of the build.

### 1.5 User decisions that this plan needs

| Decision | Needed before |
| --- | --- |
| The date on which Vulkan becomes the Windows default | VK21 |
| The minimum GPU that the product supports after the archive of OpenCL (section 10, risk R6) | VK22 |

## 2. Product design specification

### 2.1 Visible result during the work (before VK19)

A release build has no Vulkan code. The user sees no change.

A development build with `ALCEDO_ENABLE_VULKAN=ON` accepts these values:

| Entry | Value | Available from |
| --- | --- | --- |
| QSettings key `gpu/acceleratorBackend` | `vulkan` | VK17 (export, thumbnails) |
| `alcedo-cli` backend argument | `vulkan` | VK17 |
| `--editor-backend=` | `vulkan` | VK18 |
| Environment variable `ALCEDO_TEST_EDITOR_BACKEND` | `vulkan` | VK18 |
| Environment variable `ALCEDO_VULKAN_DEVICE` | part of a device name | VK0 |

`Auto` does not select Vulkan before VK21.

### 2.2 Visible result at VK19

The Settings page shows `Vulkan` in the list of GPU backends. The entry has the label
`Experimental`. The selection applies at the next start, as for the current backends.

### 2.3 Visible result at VK21 and later

`Auto` selects Vulkan on Windows. CUDA and OpenCL stay selectable until VK22 and VK23. After VK23
the Windows backend list has one entry.

### 2.4 Failure behavior

| Failure | Result |
| --- | --- |
| No Vulkan 1.3 device, or a required feature is absent | Start fails with the device name and the name of the absent feature. |
| Shader module absent or pipeline creation fails | The render fails with the kernel name and the `VkResult`. |
| Allocation fails | The render fails with the requested size and the budget. No frame is published. |
| `VK_ERROR_DEVICE_LOST` | The render fails with the error. The editor shows the error and does not publish a frame. VX5 defines the device restore procedure. |

### 2.5 Data rules

- The Vulkan backend adds no persisted format. Project files, edit history, and content keys do
  not change.
- The capability version of the Vulkan backend is a part of `StaticPlanKey`, as for OpenCL.
- The numeric result is equal to the OpenCL result in the tolerance of B9.

## 3. Current source audit

All paths are relative to `alcedo_studio/src/`. `inc/` is `alcedo_studio/src/include/`.

### 3.1 Structure

The three backends are parallel implementations. Compile-time templates connect them. There is no
virtual GPU interface.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Backend names | `GpuBackendKind` (`inc/image/gpu_backend.hpp`), `AcceleratorBackendPreference` and `ResolveAcceleratorBackend` (`inc/edit/pipeline/pipeline_accelerator.hpp`, `edit/pipeline/pipeline_accelerator.cpp`), `GpuBackend` (`inc/edit/scope/scope_analyzer.hpp`) | Values for CUDA, OpenCL, Metal. Auto on Windows selects OpenCL, then CUDA. | Add a Vulkan value to each enum. |
| Editor pairing | `editor_rhi::EditorBackend` (`inc/ui/editor_rhi/editor_backend.hpp`) | `Cuda` (Direct3D 11), `OpenCl` (OpenGL), `Metal`. Selected one time at start. | Add `Vulkan` (Qt Quick Vulkan). |
| Backend class | `CudaBackend`, `OpenClBackend`, `MetalBackend` in `inc/edit/runtime/<backend>/` | Each satisfies the concept `RenderBackend` (`inc/edit/runtime/render_backend.hpp`): `Buffer`, `Texture2D`, `CommandContext`, `CreateBuffer`, `CreateTexture2D`, `Submit`, `Wait`. | Add `VulkanBackend`. |
| Shared host templates | `BasicRenderDevice`, `BasicRenderWorkspace`, `TexturePool`, `PlanExecutor`, `Renderer`, `TransientBufferArena` in `inc/edit/runtime/` and `inc/gpu/` | One instance for each backend type. | Instantiate for `VulkanBackend`. No template change is expected. |
| Pass dispatch | `PassEncoder<Backend, Kind>` (`inc/edit/runtime/pass_encoder.hpp`), `GpuPassKind` (`inc/edit/runtime/pass_kind.hpp`) | Each backend specializes 10 kinds: UploadRaw, UploadRgb, GeometryResample, CameraToAp1, DisplayToAp1, MaskEvaluate, MaskUnion, PrimaryColorGrade, DiffusionFilter, Drt. The Develop steps run in UploadRaw. | Add 10 specializations. |
| Shared executors | `GradeExecutor<Ops>`, `LocalToneExecutor<Ops>`, `NeighborExecutor<Ops>`, `DrtPostExecutor<Ops>` | Each backend supplies an `Ops` struct. | Add `VulkanGradeOps`, `VulkanLocalToneOps`, `VulkanDrtOps`. |
| Top switch | `PipelineExecutor` (`inc/edit/pipeline/pipeline_executor.hpp`, `edit/pipeline/pipeline_executor.cpp`) | One renderer list for each backend under `#ifdef HAVE_*`. | Add `vulkan_renderers_` under `HAVE_VULKAN`. |
| Legacy image type | `ImageBuffer` and `GpuImageWrapper` (`inc/image/image_buffer.hpp`) | Hold `cv::cuda::GpuMat`, `OpenClImage`, `MetalImage` under `#ifdef`. | VK2 decides from the callers if a `VulkanImage` member is necessary. The DAG path uses `Texture2D`. |
| Build options | Root `CMakeLists.txt` lines 116 to 128 | `ALCEDO_ENABLE_CUDA`, `ALCEDO_ENABLE_METAL`, `ALCEDO_ENABLE_OPENCL` and the definitions `HAVE_CUDA`, `HAVE_METAL`, `HAVE_OPENCL`. | Add `ALCEDO_ENABLE_VULKAN` and `HAVE_VULKAN`. |
| Vulkan code | none | The tree has no Vulkan, SPIR-V, Slang, volk, or VMA code. `tests/ui/editor_rhi_contracts_test.cpp` line 52 asserts that `--editor-backend=vulkan` is rejected. | VK18 changes this assertion and renames the test file (prohibited term in the file name). |

### 3.2 Size of one backend

The OpenCL backend is the nearest model. Its size gives the estimate for Vulkan.

| Part | OpenCL files | Lines |
| --- | --- | ---: |
| Context, program library, kernel cache, geometry utilities | `opencl/*.cpp`, `opencl/opencl_shader/*.cl` | 2300 |
| Neural network layers | `opencl/nn/*.cpp` | 1100 |
| Backend class and pass encoders (host) | `edit/runtime/opencl/*.cpp` | 5900 |
| Edit kernels | `edit/runtime/opencl/shader/*.cl` (16 files) | 4500 |
| RAW operators (host and kernels) | `decoders/processor/operators/gpu/opencl_*`, `opencl_shader/*.cl` (8 files) | 3300 |
| DemosaicNet modules | `decoders/processor/nn/opencl_demosaicnet_*.cpp` | 1600 |
| Lens calibration and scope | `edit/runtime/lens/opencl/`, `edit/scope/opencl_*` | 1600 |
| Headers | `inc/edit/runtime/opencl/`, `inc/opencl/` | 2700 |
| Tests | `tests/edit/runtime/opencl_*` (16 files) | 9400 |
| Total | | 32400 |

### 3.3 Kernels that Vulkan must have

| Group | OpenCL source | Kernels |
| --- | --- | --- |
| Shared includes | `common.cl`, `cst.cl`, `oklab.cl`, `drt_params.cl`, `prng.cl`, `raw_utils_opencl.cl` | include only |
| RAW | `to_linear_ref.cl` (4), `cvt_ref_space.cl` (4), `debayer_rcd.cl` (6), `xtrans_interpolate.cl` (2), `highlight_reconstruct.cl` (8) | 24 |
| Lens and geometry | `lens_calib.cl` (2), `geometry_utils.cl` (5), `geometry_camera.cl` (3) | 10 |
| Raster input | `display_to_ap1.cl` (3) | 3 |
| Primary grade | `primary_grade.cl` (5), `primary_grade_neighbor.cl` (4) | 9 |
| Local tone | `local_tone.cl` (10) | 10 |
| Mask | `mask.cl` (7), `ai_mask_coverage.cl` (3) | 10 |
| DRT and diffusion | `drt.cl` (7) | 7 |
| Film | `film_print.cl` (18), `film_print_scan.cl` (2), `film_grain_negative.cl` (1), `film_grain_post.cl` (1) | 22 |
| DemosaicNet | `demosaicnet_conv.cl` (4), `demosaicnet_structural.cl` (19) | 23 |
| Scope | `scope_analyzer.cl` (4) | 4 |

The shared math headers (`.h` files such as `dng_profile_gpu_math.h`, listed in
`ALCEDO_OPENCL_PACKAGED_HEADERS` of `opencl/CMakeLists.txt`) compile as C++ and as OpenCL C. VX1
finds out if they also compile as Slang.

### 3.4 Display path

| Pairing | Files | Behavior |
| --- | --- | --- |
| CUDA with Direct3D 11 | `CudaD3D11LeaseAdapter` (`ui/editor_rhi/lease_target_adapters.cpp`), `cuda_adapter_discovery.cpp`, `inc/ui/edit_viewer/d3d_cuda_interop_utils.hpp` | A shared Direct3D 11 texture is imported into CUDA as external memory. The pipeline copies the frame into it. |
| OpenCL with OpenGL | `OpenClOpenGlLeaseAdapter`, `OpenClGlSharingBootstrap` in `ui/editor_rhi/editor_startup.cpp` | `clCreateFromGLTexture`, acquire, copy, release. |
| Metal | `IFrameSink::SubmitMetalFrame`, `ui/editor_rhi/editor_viewport_renderer.cpp` | The `MTLTexture` goes to the viewport with zero copy. |

The frame sink enums `FrameMemoryDomain` and `FrameWriteTargetType`
(`inc/ui/edit_viewer/frame_sink.hpp`) and the lease enums
(`inc/ui/editor_rhi/frame_presentation_lease.hpp`) name CUDA and OpenCL targets. The graphics API
is set in `ui/alcedo_main/main.cpp` and `ui/editor_rhi/editor_startup.cpp`
(`ApplyEditorBackendBeforeWindow`).

### 3.5 Facts of the development computer (2026-11-22)

| Item | Value | Source |
| --- | --- | --- |
| Qt 6.9.3 `QT_FEATURE_vulkan` | 1 | `QtGui/qtgui-config.h` |
| `QQuickGraphicsDevice::fromDeviceObjects` | present | `QtQuick/qquickgraphicsdevice.h` |
| Vulkan SDK | `D:\misc\VulkanSDK`, `slangc` 2025.17.2, `glslc` | `VULKAN_SDK` |
| NVIDIA GeForce RTX 3080 Laptop GPU | Vulkan 1.4.341. Queue family 0: 16 queues (graphics, compute). Queue family 2: 8 queues (compute). | `vulkaninfo` |
| Intel Iris Xe Graphics | Vulkan 1.4.323. One queue (graphics, compute). | `vulkaninfo` |
| Both devices | `shaderFloat16` true, `VK_EXT_memory_budget`, `VK_KHR_push_descriptor`, `maxPushConstantsSize` 256, `maxImageDimension2D` 32768 (NVIDIA) and 16384 (Intel) | `vulkaninfo` |

## 4. Target architecture

```text
PipelineDocument
      |
GraphCompiler                      shared, no GPU type
      |
ExecutionPlan
      |
Renderer<VulkanBackend>
`-- BasicRenderDevice<VulkanBackend>
    |-- VulkanCommandContext        one command buffer for each render, one submit
    |-- BasicRenderWorkspace<VulkanBackend>
    |   `-- ParameterArena, TransientBufferArena, TexturePool,
    |       MaskTextureCache, GraphImageCache, NodeResultCache
    `-- VulkanSharedGpuResources
        |-- VulkanShaderModuleRegistry   SPIR-V by kernel name
        |-- VulkanComputePipelineCache   pipeline by kernel name and specialization
        |-- LUT images by content key
        `-- neural network weights
VulkanContext                      process owner of instance, pipeline device, display device,
                                   queues, allocators
```

### 4.1 Owners

| Owner | Input | Output | Lifetime | Error surface |
| --- | --- | --- | --- | --- |
| `VulkanContext` | Device preference (`ALCEDO_VULKAN_DEVICE`), required features | `VkInstance`, `VkPhysicalDevice`, the pipeline `VkDevice` and its queue, the display `VkDevice` (from VK18) and its queue, one VMA allocator for each device, capability record | Process. Created before the first `QQuickWindow` when the editor uses Vulkan. | Exception with the device name and the absent feature |
| `VulkanShaderModuleRegistry` | Kernel name | `VkShaderModule` from a packaged `.spv` file | `VulkanContext` | Exception with the kernel name and the file path |
| `VulkanComputePipelineCache` | Kernel name, specialization values | `VkPipeline`, `VkPipelineLayout`, descriptor set layout | `VulkanContext`. The `VkPipelineCache` data is stored in the application cache folder. | Exception with the kernel name and the `VkResult` |
| `VulkanBackend` | Size, format | `Buffer`, `Texture2D` (move-only), counters | Render device | Exception with the size and the budget |
| `VulkanCommandContext` | Pass encoders | One recorded command buffer, one submission, one completion fence | One render | Exception with the `VkResult`. The context drops the frame result. |
| `FramePresenter<VulkanBackend>` | Final DRT image | Host pixels (`Download`), or a `VkImage` for the frame sink (`Present`) | One render | Exception. No frame is published. |

### 4.2 Rules

- The pipeline and Qt Quick use two `VkDevice` objects on one `VkPhysicalDevice` (B5). Each
  device has one queue of the graphics family. The pipeline code uses only the pipeline device;
  Qt Quick uses only the display device. The program uses the display queue only before the first
  `QQuickWindow` exists (initial image layouts). Each queue has one user thread (VX2, section 9.2).
- A frame image for the viewport is an RGBA32F image of the pipeline device, in a dedicated
  allocation exported as `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`, and an image with
  the same create info on the display device, bound to the imported memory. The Win32 handle is
  closed after the import. The pipeline releases the image to `VK_QUEUE_FAMILY_EXTERNAL` in
  `SHADER_READ_ONLY_OPTIMAL`; the viewport renderer acquires it in the same layout in the Qt
  command buffer (`QRhiCommandBuffer::beginExternal`) of the frame that samples it first. The
  display image enters `QRhiTexture::createFrom` with `SHADER_READ_ONLY_OPTIMAL`.
- A frame image stays unchanged until Qt has begun the second frame after the last frame that
  sampled it (Qt has two frames in flight). The ring has at least five images.
- The display device enables every supported core feature except `robustBufferAccess`, and the
  extensions of `QRhiVulkanInitParams::preferredExtensionsForImportedDevice()` that the device
  has. Qt reads the features of the physical device and assumes that they are enabled for an
  imported device.
- One render records all its passes into one command buffer and submits one time. A pass encoder
  does not submit and does not wait.
- The application exports one photo at a time (user decision 2026-11-22: one export render holds
  the export resources). An export render (full resolution) goes to the pipeline queue in
  submissions of at most 8 ms of estimated GPU time, one submission at a time: the export thread
  waits for the fence of a submission before it submits the next (VX3, section 9.3). A dispatch that
  is longer than the budget is divided into bands of rows with `vkCmdDispatchBase`
  (`VK_PIPELINE_CREATE_DISPATCH_BASE_BIT`); the bands of one dispatch need no barrier between
  them. Each submission begins with a compute-to-compute memory barrier, which covers the earlier
  submissions of the queue. The estimate of a dispatch is its time per row from the timestamps of
  the earlier renders of the same kernel, size, and device. A preview render (2560 pixels on the
  long edge during a drag, 4K when the interaction stops) and a thumbnail render are one
  submission each.
- A preview request waits for at most the export submission on the queue (8 ms with the rule
  above); the preview submission goes to the queue before the next export submission.
- A workgroup finishes in less than 1 ms. A kernel with a loop of data-dependent length divides
  the loop over more workgroups or more dispatches. The GPU switches to another `VkDevice` (Qt
  Quick, the desktop) between workgroups, so one workgroup that runs for seconds stops the display
  and reaches the Windows GPU timeout (VX3).
- Images use `VK_FORMAT_R32G32B32A32_SFLOAT`, `VK_FORMAT_R32_SFLOAT`, and `VK_FORMAT_R8_UNORM`.
  These are the Vulkan forms of the three formats that the OpenCL backend requires.
- Parameters go through the `ParameterArena` buffer. Buffers are device addresses
  (`bufferDeviceAddress`). The push constants of a dispatch hold the device addresses of its
  buffers and the offsets in them. The shared math headers read their tables through pointers, and
  a Slang pointer is a device address (VX1).
- A kernel finds a parameter record with integer address arithmetic on a `uint64_t` address, for
  example `(Record*)(base + offset + k * sizeof(Record))`. Pointer arithmetic on a pointer to a
  struct (`p + k`) is prohibited: the Intel driver 101.7092 stops in `vkCreateComputePipelines`
  on that form after inlining (VX1). Pointer arithmetic on a pointer to `float` or `uint` is
  permitted.
- Buffer data has the scalar layout (`scalarBlockLayout`, `slangc -fvk-use-scalar-layout`), so a
  struct of a shared header has the layout of the host C++ struct.
- Descriptor sets use `VK_KHR_push_descriptor`. A kernel has one set. The set holds images only.
- Memory goes through VMA. The budget uses `VK_EXT_memory_budget` and the application limit.
- A stable render creates no pipeline, no shader module, no image, and no buffer. Counters prove
  this, as for OpenCL.
- Kernel names and binding numbers are defined in one header for each kernel group.

### 4.3 Kernel build

```text
<group>.slang  --slangc (build time)-->  <group>.spv   one module, one entry point for each kernel
        --spirv-val (build time)
        -> build tree  shaders/vulkan/<group>.spv
        -> install     <bin>/vulkan/<group>.spv  with an asset inventory and checksums
        -> VulkanShaderModuleRegistry::Module(group name), entry point = kernel name
```

`slangc` arguments (VX1): `-target spirv -profile spirv_1_6 -fvk-use-scalar-layout
-fvk-use-entrypoint-name -O2`, and `-I alcedo_studio/src/include` for the shared headers.
`-fvk-use-entrypoint-name` keeps the kernel name as the entry point name. With one `-entry`,
`slangc` names the entry point `main` without this argument.

The build runs `spirv-val --target-env vulkan1.3 --scalar-block-layout` on each module. `slangc`
does not validate its output: in VX1 it wrote one module that `spirv-val` rejects (an `OpPhi`
with a wrong count of incoming blocks) and reported no error.

The build fails when `slangc` or `spirv-val` reports an error. The application compiles no shader
source at run time. The install rule follows `alcedo_install_opencl_sources` (root
`CMakeLists.txt`).

The shared math headers have a `__SLANG__` branch beside the OpenCL C and Metal branches. Two
rules keep one header text for all targets:

- An array member of a struct is read through `&params->field[0]`, not through the array name.
  Slang has no conversion of an array to a pointer. The form is equal in C++, CUDA, OpenCL C, and
  Metal.
- A function that takes a pointer to a value of the caller is in a `#if !defined(__SLANG__)`
  block. Slang has pointers to device memory only. In VX1 the one such function
  (`FilmKernelNegativeScanTerms`) is a host function.

SPIR-V has no `cbrt`. The Slang form is `exp2(log2(|x|) / 3)` and one Newton step. Its difference
from a double-precision `cbrt` has the size of the difference of the OpenCL `cbrt` (VX1). A second
Newton step changes the result by little.

### 4.4 Primary call chains

Success, host output (export, thumbnail, test):

```text
PipelineExecutor (preference Vulkan)
  -> Renderer<VulkanBackend>::Render
  -> PlanExecutor<VulkanBackend>            for each pass
       -> PassEncoder<VulkanBackend, Kind>::Encode   records dispatches
  -> VulkanBackend::Submit                  one vkQueueSubmit2 on the pipeline queue
  -> FramePresenter<VulkanBackend>::Download -> host pixels
```

Success, editor (from VK18):

```text
main: VulkanContext::Create (pipeline device, display device)
  -> QQuickGraphicsDevice::fromDeviceObjects(physical device, display device, family, queue 0)
  -> Renderer<VulkanBackend>::Render -> FramePresenter<VulkanBackend>::Present
       writes the exported frame image, releases it to VK_QUEUE_FAMILY_EXTERNAL, waits its fence
  -> IFrameSink (Vulkan image target: the imported display image) -> DirectPresentQueue
  -> EditorViewportRenderer: acquire barrier (beginExternal), QRhiTexture::createFrom(VkImage)
  -> viewport composite pass (scaleZoom.y = -1 when !QRhi::isYUpInNDC())
```

Failure:

```text
PassEncoder or Submit throws (VkResult, kernel name)
  -> VulkanCommandContext drops the command buffer and the frame result
  -> no content key is published; the caches keep their earlier entries
  -> Renderer returns the error to PipelineExecutor -> application service -> user message
```

## 5. Parallel work rules

1. `main` stays releasable. Each implementation pull request adds Vulkan files and the minimum
   `#ifdef HAVE_VULKAN` lines in shared files. It changes no CUDA, OpenCL, or Metal behavior.
2. Release presets keep `ALCEDO_ENABLE_VULKAN=OFF` until VK19. The development computer sets it to
   `ON` in `build/debug`.
3. A new kernel or a kernel change on `main` goes to CUDA, OpenCL, and Metal as before. If the
   Vulkan pass of that kernel is complete, the cross-backend comparison test of that pass fails.
   The author of the change then updates the Slang kernel in the same pull request, or opens a
   sub-issue of #1036 and marks the pass as behind in section 7.
4. The experiment worktree is `D:\Projects\pu-erh_lab\.claude\worktrees\vulkan-backend`. It
   rebases on `main` when an experiment needs a newer source. It has no pull request.
5. Each phase gets a sub-issue of #1036 when its work starts. The pull request names it with
   `Closes`.

## 6. Experiments

The experiments answer the questions that decide the design. Each one writes its findings into
section 9 by date. An experiment does not need product code structure.

### VX1: kernel language

**Question.** Can Slang express the current kernels with the shared math headers, and does the
SPIR-V give the OpenCL values on NVIDIA and Intel?

**Work.**

1. Write `primary_grade` (point kernel, many parameters) and `film_print` stage kernels (tables,
   3D LUT sampling) in Slang from the `.cl` sources.
2. Include one shared `.h` math header in Slang. Record each macro that the header needs.
3. Compile with `slangc -target spirv`. Run through a minimum Vulkan compute program.
4. Compare with the OpenCL output of the same input on the RTX 3080 Laptop GPU and on Iris Xe.
5. Compile the same Slang source with `-target metal`. Record if the output compiles with
   `xcrun metal` (on the Mac, when available).
6. Write the same point kernel in GLSL and compare the source size and the build time.

**Acceptance criteria for Slang.** The difference from OpenCL is in the tolerance of
`opencl_grade_test.cpp` and `opencl_film_print_test.cpp`. The shared header compiles with a macro
layer of less than 100 lines. The build time for one kernel group is less than 10 seconds.
If one criterion fails, B3 changes to GLSL with `glslc`, and section 4.3 changes with it.

**Status.** Complete (2026-11-22). All three criteria pass. Section 9.1 has the record.

### VX2: Qt Quick on Vulkan with a shared device

**Question.** Does Qt Quick on Vulkan on Windows have the quality of the Direct3D 11 path, with a
device that Alcedo creates?

**Work.**

1. Start the `EditorRhiHarness` with `QSGRendererInterface::Vulkan` and a device from
   `fromDeviceObjects`.
2. Show a `VkImage` that a compute shader writes, through `QRhiTexture::createFrom`, with zero copy.
3. Test the queue rule of section 4.2 on the RTX 3080 Laptop GPU (second queue) and on Iris Xe
   (one queue, mutex). Run with the validation layer and its thread safety check.
4. Measure the frame time during a window resize and during a continuous slider drag. Compare with
   the Direct3D 11 pairing.
5. Find the swapchain format and color space that Qt selects. Find out if the display transform of
   the Windows display ICC plan gives the same pixels as on Direct3D 11.
6. Start with common injected layers present (screen recorder, overlay). Record a crash or a hang.
7. Find the adapter rule for a computer with two GPUs: the device that owns the display, and the
   device that Alcedo selects.

**Acceptance criteria.** Zero copy display works on both devices with no validation error. The
resize and drag frame times are not more than 10 % above the Direct3D 11 values. The display
transform gives the same pixels. If the resize or color result fails, the plan records the defect
and VK18 stops until the plan has a new display design.

**Status.** Complete (2026-11-22). Zero copy display, the drag frame times, and the display pixels
pass on both devices. The resize criterion fails on both devices: Iris Xe needs about 4 times the
Direct3D 11 time for each resize step, and the RTX 3080 Laptop GPU has a longer tail. VK18 stops
until the plan has a new resize design; the compute phases VK0 to VK17 continue. The queue rule of
section 4.2 failed in its first form and changed to two devices (B5). Section 9.2 has the record.

### VX3: long dispatch on the shared device

**Question.** Which dispatch size keeps the device alive and the editor responsive?

**Work.**

1. Run the local tone kernels and the film print kernels on a 45 megapixel image at full size in
   one submission. Record the submission time and the editor frame time.
2. Divide the same work into tiles and into submissions of a fixed time budget. Repeat.
3. Run a compute shader that is longer than the Windows GPU timeout (2 seconds) on each device.
   Record the result.

**Result.** A split rule: the maximum time of one submission and the tile size for each kernel
class. Section 4.2 gets the rule.

**Status.** Complete (2026-11-22) on the RTX 3080 Laptop GPU. The rule of section 4.2 is one
export at a time, a submission budget of 8 ms with one submission in flight, bands of rows for a
dispatch above the budget, and a workgroup time below 1 ms. Iris Xe results are indicative (user decision
2026-11-22: OpenCL stays on such devices for a long time; the experiments iterate fast). The
Windows GPU timeout itself is outside the test: this computer has `TdrDelay` 60 s. Section 9.3 has
the record.

### VX4: neural network throughput

**Question.** Is a hand-written Vulkan compute network fast enough for DemosaicNet and UtNet2?

**Work.**

1. Port the OpenCL NHWC4 conv3x3 and conv1x1 layers (`opencl/nn/convolution.cpp`,
   `demosaicnet_conv.cl`) to Slang with FP16 weights and activations (`shaderFloat16`).
2. Measure the DemosaicNet Bayer forward time on a 45 megapixel photo. Compare with the OpenCL and
   CUDA times of the DemosaicNet plans.
3. Run UtNet2 through ncnn with its Vulkan backend as an upper limit for a tuned Vulkan network.
   ncnn is a measurement tool here. It does not become a dependency.
4. Compare with the ONNX Runtime DirectML time of the AI Denoise plan (DN1 record: 0.82 s for
   45.4 megapixels at tile 1536 on the RTX 3080 Laptop GPU).

**Acceptance criteria.** DemosaicNet on Vulkan is not slower than on OpenCL. UtNet2 on Vulkan is
not more than 2 times the DirectML time. If UtNet2 fails this criterion, VK20 changes to ONNX
Runtime DirectML with a Direct3D 12 resource import through `VK_KHR_external_memory_win32`, and the
plan records that one interop pairing stays.

### VX5: video memory budget and device loss

**Question.** How does the application keep to its memory budget, and how does it continue after a
device loss?

**Work.**

1. Allocate above the `VK_EXT_memory_budget` value. Record when Windows moves memory to shared
   system memory and the speed that results.
2. Cause a device loss (driver restart with `Win+Ctrl+Shift+B`, and a timeout from VX3). Record
   what Qt Quick does with a device that it does not own.
3. Design the restore procedure: which owner recreates `VulkanContext`, and how the caches of
   `BasicRenderWorkspace` become empty.

**Result.** The budget rule for `TexturePool` and the restore procedure for section 2.4.

## 7. Phase summary

Each phase is one pull request into `main`. Each phase from VK3 adds the cross-backend comparison
test of its kernels (B9).

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| VK0 | Build option, dependencies, `VulkanContext`, enum values | root CMake, `src/vulkan/`, `pipeline_accelerator` | VX1, VX2 | 1300 to 1700 | planned |
| VK1 | Slang build rule, shader module registry, compute pipeline cache, kernel check tool | `src/vulkan/`, `tools/gpu_kernel_check.cpp` | VK0 | 1300 to 1700 | planned |
| VK2 | `VulkanBackend`, `VulkanCommandContext`, workspace instance | `edit/runtime/vulkan/` | VK1, VX3, VX5 | 1600 to 1900 | planned |
| VK3 | Shared kernel modules, geometry utilities, comparison test support | `edit/runtime/vulkan/shader/`, `src/vulkan/` | VK2 | 1500 to 1900 | planned |
| VK4 | RAW operators: linearize, CFA clamp, reference space, RCD, X-Trans, pack | `decoders/processor/operators/gpu/` | VK3 | 1400 to 1800 | planned |
| VK5 | RAW operators: highlight reconstruction, lens calibration, DNG warp | `decoders/processor/operators/gpu/`, `edit/runtime/lens/vulkan/` | VK4 | 1500 to 1900 | planned |
| VK6 | Develop pass (UploadRaw) | `edit/runtime/vulkan/` | VK5 | 1400 to 1800 | planned |
| VK7 | GeometryResample, CameraToAp1, UploadRgb, DisplayToAp1 | `edit/runtime/vulkan/` | VK6 | 1100 to 1500 | planned |
| VK8 | Primary grade point kernels and LUT resources | `edit/runtime/vulkan/` | VK7 | 1400 to 1800 | planned |
| VK9 | Primary grade neighbor kernels | `edit/runtime/vulkan/` | VK8 | 800 to 1200 | planned |
| VK10 | Local tone | `edit/runtime/vulkan/` | VK8 | 1300 to 1700 | planned |
| VK11 | MaskEvaluate, MaskUnion, AI mask coverage | `edit/runtime/vulkan/` | VK8 | 1300 to 1700 | planned |
| VK12 | DiffusionFilter and Drt | `edit/runtime/vulkan/` | VK8 | 1500 to 1900 | planned |
| VK13 | Film print | `edit/runtime/vulkan/` | VK12 | 1600 to 1950 | planned |
| VK14 | Film print scan and film grain | `edit/runtime/vulkan/` | VK13 | 900 to 1300 | planned |
| VK15 | Neural network layers | `src/vulkan/nn/` | VK2, VX4 | 1500 to 1900 | planned |
| VK16 | DemosaicNet modules and the neural Develop branch | `decoders/processor/nn/` | VK15, VK6 | 1600 to 1950 | planned |
| VK17 | Scope analyzer, host download, `PipelineExecutor`, preference `vulkan` | `edit/scope/`, `edit/pipeline/`, `ui/alcedo_cli/` | VK14, VK16 | 1400 to 1800 | planned |
| VK18 | Editor display on Qt Quick Vulkan | `ui/editor_rhi/`, `ui/alcedo_main/main.cpp` | VK17, VX2, a new resize design (R1) | 1500 to 1900 | stopped (VX2) |
| VK19 | Release build, packaging, experimental setting | CMake presets, `scripts/`, Settings QML | VK18 | 700 to 1000 | planned |
| VK20 | UtNet2 forward code for AI Denoise | AI Denoise modules | VK15, #1030, VX4 | 1500 to 1900 | planned |
| VK21 | Vulkan as the Windows default | `pipeline_accelerator`, `editor_backend` | VK19, user decision | 200 to 400 | planned |
| VK22 | Archive of the OpenCL backend and the OpenGL pairing | `alcedo_studio/deprecated/` | VK21, user decision | file moves, 600 to 1000 changed lines | planned |
| VK23 | Archive of the CUDA backend and the Direct3D 11 pairing | `alcedo_studio/deprecated/` | VK22 | file moves, 800 to 1200 changed lines | planned |

Split reason: one OpenCL pass with its kernels and tests is 1000 to 2200 lines (section 3.2), so
each pass group is one phase, and the three largest groups (RAW operators, primary grade, film) are
two phases each. A phase whose estimate passes 2000 lines during its detailed plan is divided
again. For VK22 and VK23 the limit counts the changed lines. A file move with no content change is
not a changed line.

VK0 to VK2 have a detailed section below. Each later phase gets its detailed section in this file
before its implementation starts, after the experiments that it depends on have a record.

## 8. Detailed phases

Before each phase, the implementer reads `AGENTS.md` and the applicable skills again.

### Phase VK0: build option, dependencies, `VulkanContext`, enum values

**Objective and deliverables.** A development build with `ALCEDO_ENABLE_VULKAN=ON` creates a
Vulkan device that satisfies the requirements of this plan, and reports its capabilities. A
reviewer sees a test that creates the context on the local GPU and a clear error for an absent
feature.

**Inputs and prerequisites.** The VX1 record (kernel language) and the VX2 record (queue rule,
device selection rule).

**Modules, files, and APIs.**

- Root `CMakeLists.txt`: option `ALCEDO_ENABLE_VULKAN` (default `OFF`), definition `HAVE_VULKAN`,
  `find_package(Vulkan)`, and the packages for volk and VulkanMemoryAllocator.
- `alcedo_studio/src/vulkan/CMakeLists.txt` (proposed): target `VulkanContext`.
- `inc/vulkan/vulkan_context.hpp`, `src/vulkan/vulkan_context.cpp` (proposed):
  - `VulkanContext::Create(const VulkanInitializationOptions&) -> VulkanContext&`. Creates the
    instance, selects the physical device, creates the pipeline device, its queue, and its
    allocator. The display device follows in VK18 (B5). Changes process state. Throws with the
    device name and the absent feature.
  - `VulkanContext::Instance() -> VulkanContext&`. Read access after `Create`.
  - `VulkanContext::Capabilities() const -> const VulkanCapabilities&`: API version, queue
    families, limits, and optional features.
  - `VulkanContext::PipelineQueue()`: the `VkDevice` of the pipeline, its `VkQueue`, the queue
    family, and the queue index. The pipeline thread is the only user of this queue.
- `inc/vulkan/vulkan_requirements.hpp`, `src/vulkan/vulkan_requirements.cpp` (proposed):
  `CheckVulkanDeviceRequirements(physical device) -> VulkanRequirementResult` with the name of
  each absent requirement. Pure function of the device properties. The requirements of the VX1
  kernels: API 1.3, `bufferDeviceAddress`, `scalarBlockLayout`, `shaderInt64`,
  `synchronization2`, `VK_KHR_push_descriptor`, and a compute queue with `timestampValidBits`
  above 0. Both devices of the development computer have them.
- `inc/image/gpu_backend.hpp`: `GpuBackendKind::Vulkan`.
- `inc/edit/pipeline/pipeline_accelerator.hpp`, `edit/pipeline/pipeline_accelerator.cpp`:
  `AcceleratorBackendPreference::Vulkan`. `ResolveAcceleratorBackend(Vulkan)` gives Vulkan when
  `HAVE_VULKAN` is defined and the requirements pass, and throws in all other cases.
  `IsImplementedMergedPipelineBackend(Vulkan)` returns false until VK17.

**Data rules and invariants.**

- One `VulkanContext` for each process.
- Required: Vulkan 1.3, a compute queue, the three image formats of section 4.2 as storage images
  and as sampled images with linear filtering, `VK_KHR_push_descriptor`, `VK_EXT_memory_budget`.
- `ALCEDO_VULKAN_DEVICE` selects a device by a part of its name. With no value, the selection uses
  the rule of the VX2 record.
- Auto does not give Vulkan.

**Implementation steps.**

1. Add the build option and the packages. Configure `build/debug` with the option `ON`.
2. Add `CheckVulkanDeviceRequirements` and its tests with recorded device property sets.
3. Add `VulkanContext` with instance creation, device selection, device creation, queues, and the
   allocator. In a debug build, enable the validation layer when it is installed.
4. Add the enum values and the resolve rule. Update each `switch` over the enums so that the
   compiler reports no unhandled value.
5. Register the tests under `ALCEDO_BUILD_GPU_TESTS` and `HAVE_VULKAN`.

**Primary success call chain.**

```text
test -> VulkanContext::Create(options)
  -> vkCreateInstance -> enumerate devices -> CheckVulkanDeviceRequirements
  -> vkCreateDevice -> queues -> vmaCreateAllocator
  -> VulkanContext::Capabilities()
```

**Primary failure call chain.**

```text
CheckVulkanDeviceRequirements reports "VK_KHR_push_descriptor"
  -> VulkanContext::Create throws (device name, requirement name)
  -> ResolveAcceleratorBackend(Vulkan) passes the exception to the caller
  -> no device exists; a later Create can run again
```

**Tests and evidence.**

- `VulkanRequirementsTest.DeviceWithoutPushDescriptorReportsTheExtensionName`
- `VulkanRequirementsTest.Vulkan12DeviceReportsTheApiVersion`
- `VulkanContextTest.CreateOnLocalGpuReportsVulkan13AndAComputeQueue`
- `VulkanContextTest.DeviceNameFilterSelectsTheMatchingDevice`
- `VulkanContextTest.SingleQueueDeviceGivesOneQueueForPipelineAndQt` (runs on Iris Xe through
  `ALCEDO_VULKAN_DEVICE`)
- `PipelineAcceleratorTest.AutoOnWindowsDoesNotResolveToVulkan`
- `PipelineAcceleratorTest.VulkanPreferenceThrowsWhenTheBuildHasNoVulkan`

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DALCEDO_ENABLE_VULKAN=ON
cmd /c scripts\msvc_env.cmd --build build\debug --target VulkanContextTest VulkanRequirementsTest
ctest --test-dir build/debug -R "Vulkan(Context|Requirements)Test" -j 1 --output-on-failure
```

**Exit criteria.**

- [ ] A build with the option `OFF` has no Vulkan symbol and no new dependency.
- [ ] The listed tests pass on the RTX 3080 Laptop GPU and on Iris Xe.
- [ ] The validation layer reports no error during the context tests.

**Expected diff.** 1300 to 1700 lines.

**Completion record.** (empty)

### Phase VK1: Slang build rule, shader module registry, compute pipeline cache, kernel check tool

**Objective and deliverables.** The build compiles Slang kernels to SPIR-V. The application loads
a kernel by name and gets a compute pipeline. A reviewer sees one test kernel that runs and
writes a known value, and a counter that shows zero pipeline creations on the second use.

**Inputs and prerequisites.** VK0. The VX1 record (section 9.1): the `slangc` arguments, the
`spirv-val` step, and the kernel rules of sections 4.2 and 4.3.

**Modules, files, and APIs.**

- `alcedo_studio/src/vulkan/CMakeLists.txt`: function `alcedo_add_slang_kernels(target, sources)`
  (proposed). It runs `slangc` one time for each kernel group with one `-entry` for each kernel and
  the arguments of section 4.3, writes `<group>.spv`, runs `spirv-val` on it, and writes an asset
  inventory: kernel name, group file, checksum.
- `inc/vulkan/vulkan_shader_module_registry.hpp`, `.cpp` (proposed):
  `VulkanShaderModuleRegistry::Module(std::string_view kernel) -> VkShaderModule`. It finds the
  group file of the kernel in the inventory and resolves the packaged path first, then the build
  tree path, as `ResolveOpenClSourcePath` does. The entry point name is the kernel name.
- `inc/vulkan/vulkan_compute_pipeline_cache.hpp`, `.cpp` (proposed):
  `VulkanComputePipelineCache::Pipeline(const VulkanKernelDescriptor&) -> const VulkanComputePipeline&`.
  The descriptor has the kernel name, the binding list, and the specialization values.
  `StoreToDisk()` and `LoadFromDisk()` handle the `VkPipelineCache` data in the application cache
  folder. A data file from a different driver is ignored and replaced.
- `inc/vulkan/vulkan_api_counters.hpp`, `.cpp` (proposed): counters for module load, pipeline
  creation, cache hit, dispatch, upload, download, submit, and wait. Model:
  `opencl/opencl_api_counters.cpp`.
- `alcedo_studio/src/tools/gpu_kernel_check.cpp`: argument `--vulkan`. It loads each packaged
  kernel and creates its pipeline.

**Data rules and invariants.**

- A kernel name is unique in the process.
- The registry and the cache are owned by `VulkanContext`. A pass encoder gets a pipeline and does
  not create one.
- The application compiles no shader source at run time.

**Implementation steps.**

1. Add the CMake function and one test kernel `test_fill.slang`.
2. Add the registry with the path rule and the inventory check.
3. Add the pipeline cache with the descriptor layout from the binding list and push descriptors.
4. Add the counters.
5. Add the disk store of the pipeline cache data.
6. Add `--vulkan` to `gpu_kernel_check`.

**Primary success call chain.**

```text
test -> VulkanComputePipelineCache::Pipeline({"test_fill", bindings})
  -> VulkanShaderModuleRegistry::Module("test_fill") -> read .spv -> vkCreateShaderModule
  -> vkCreateComputePipelines -> cached entry
test -> record dispatch -> submit -> read back the buffer
```

**Primary failure call chain.**

```text
Module("absent_kernel") -> the inventory has no entry
  -> exception (kernel name, searched paths) -> the cache gets no entry
```

**Tests and evidence.**

- `VulkanShaderModuleRegistryTest.PackagedPathIsUsedBeforeTheBuildTreePath`
- `VulkanShaderModuleRegistryTest.AbsentKernelReportsItsNameAndTheSearchedPaths`
- `VulkanComputePipelineCacheTest.SecondRequestForOneKernelCreatesNoPipeline`
- `VulkanComputePipelineCacheTest.FillKernelWritesTheRequestedValueToEachElement`
- `VulkanComputePipelineCacheTest.StoredCacheDataFromADifferentDriverIsReplaced`

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target VulkanComputePipelineCacheTest VulkanShaderModuleRegistryTest gpu_kernel_check
ctest --test-dir build/debug -R "Vulkan(ShaderModuleRegistry|ComputePipelineCache)Test" -j 1 --output-on-failure
```

**Exit criteria.**

- [ ] A Slang error stops the build with the file and the line.
- [ ] The listed tests pass on both local devices.
- [ ] `gpu_kernel_check --vulkan` exits with 0 for the test kernel.

**Expected diff.** 1300 to 1700 lines.

**Completion record.** (empty)

### Phase VK2: `VulkanBackend`, `VulkanCommandContext`, render workspace

**Objective and deliverables.** `VulkanBackend` satisfies `RenderBackend`.
`BasicRenderDevice<VulkanBackend>` and `BasicRenderWorkspace<VulkanBackend>` compile and pass the
workspace tests that the OpenCL backend passes.

**Inputs and prerequisites.** VK1. The VX3 record (split rule) and the VX5 record (budget rule).

**Modules, files, and APIs.**

- `inc/edit/runtime/vulkan/vulkan_backend.hpp`, `edit/runtime/vulkan/vulkan_backend.cpp`
  (proposed), target `EditRuntimeVulkan` in `edit/CMakeLists.txt`:
  - `VulkanBackend::Buffer` and `VulkanBackend::Texture2D`: move-only owners of a VMA allocation.
  - `CreateBuffer(size)`, `CreateTexture2D(width, height, TextureFormat)`.
  - Upload and download of a buffer range and of an image, with a dirty rectangle for R8.
  - Image to image, buffer to image, and image to buffer copy.
  - `Submit(VulkanCommandContext&)`, `Wait(VulkanCommandContext&)`.
  - `kName = "Vulkan"`, byte statistics, and the counters of VK1.
- `VulkanCommandContext`: `BeginRender`, the command buffer of the render, the image layout record
  for each texture, the submission id, and the completion fence.
- `inc/edit/runtime/vulkan/vulkan_render_device.hpp` (proposed):
  `using VulkanRenderDevice = BasicRenderDevice<VulkanBackend>;`
- `tests/edit/runtime/vulkan_workspace_test.cpp` and `vulkan_workspace_test_support.hpp`
  (proposed), target `GpuDagVulkanWorkspaceTest`.

**Data rules and invariants.**

- The command context owns the image layout of each texture during a render. A pass states the
  access that it needs. The context records the transition.
- A resource that an unfinished submission uses stays alive until the fence signals.
- The budget is the minimum of the application limit and the `VK_EXT_memory_budget` value, with
  the reserve of the VX5 record.
- A stable render allocates nothing.
- `VulkanCommandContext` divides an export render into submissions by the rule of section 4.2
  (8 ms budget, one in flight, bands of rows). It keeps the time per row of each kernel, size, and
  device from the timestamps of the earlier renders. A preview render is one submission.

**Implementation steps.**

1. Add `Buffer` and `Texture2D` with VMA.
2. Add the upload, download, and copy operations with staging buffers from `TransientBufferArena`.
3. Add `VulkanCommandContext` with the layout record and one submission for each render.
4. Add `Submit` and `Wait` on the pipeline queue of the pipeline device (section 4.2).
5. Instantiate the device and workspace templates. Change a shared template only when the
   compiler shows a real difference, and record the change in this section.
6. Port the tests of `opencl_workspace_test.cpp` by behavior.

**Primary success call chain.**

```text
BasicRenderDevice<VulkanBackend>::BeginRender -> VulkanCommandContext::BeginRender
  -> TexturePool::Acquire -> VulkanBackend::CreateTexture2D (first use only)
  -> upload -> copy -> VulkanBackend::Submit -> fence -> download
```

**Primary failure call chain.**

```text
CreateTexture2D passes the budget -> exception (size, budget)
  -> VulkanCommandContext drops the command buffer
  -> TexturePool keeps its leases unchanged -> the submission id does not advance
```

**Tests and evidence.**

- `GpuDagVulkanWorkspaceTest.UploadedRgba32fImageDownloadsWithEqualValues`
- `GpuDagVulkanWorkspaceTest.R8DirtyRectangleUploadChangesOnlyTheRectangle`
- `GpuDagVulkanWorkspaceTest.SecondRenderOfOnePlanAllocatesNoImageAndNoBuffer`
- `GpuDagVulkanWorkspaceTest.TextureInAnUnfinishedSubmissionIsNotReleased`
- `GpuDagVulkanWorkspaceTest.AllocationAboveTheBudgetReportsSizeAndBudget`
- `GpuDagVulkanWorkspaceTest.FailedEncodeDoesNotAdvanceTheCompletedSubmission`

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagVulkanWorkspaceTest
ctest --test-dir build/debug -R "GpuDagVulkanWorkspaceTest" -j 1 --output-on-failure
```

**Exit criteria.**

- [ ] `static_assert(RenderBackend<VulkanBackend>)` compiles.
- [ ] The listed tests pass on both local devices with no validation error.
- [ ] The CUDA, OpenCL, and Metal workspace tests are unchanged.

**Expected diff.** 1600 to 1900 lines.

**Completion record.** (empty)

### Phases VK3 to VK23: scope of each phase

Each entry gives the objective, the OpenCL model, and the main tests. The detailed section replaces
the entry before the implementation.

| Phase | Objective | OpenCL model | Main tests |
| --- | --- | --- | --- |
| VK3 | Slang modules for the shared math (color space transforms, OKLab, PRNG, DRT parameters), geometry utilities (crop, resize, warp), and a test helper that renders one input on Vulkan and on OpenCL and compares with a tolerance | `common.cl`, `cst.cl`, `oklab.cl`, `prng.cl`, `opencl_geometry_utils.cpp`, `geometry_utils.cl` | Geometry results equal OpenCL. The PRNG sequence equals the OpenCL sequence. |
| VK4 | RAW encode entry points: `EncodeLinearizeRgb`, `EncodeToLinearRef`, `EncodeCfaClamp01`, `EncodeBayerRcd`, `EncodeXTrans`, pack and copy with crop and orientation | `opencl_encode.cpp`, `to_linear_ref.cl`, `cvt_ref_space.cl`, `debayer_rcd.cl`, `xtrans_interpolate.cl` | Each stage equals OpenCL on a real Bayer RAW and a real X-Trans RAW. |
| VK5 | Highlight reconstruction, lens calibration, DNG rectilinear warp | `highlight_reconstruct.cl`, `opencl_lens_calib_ops.cpp`, `lens_calib.cl` | Clipped highlights equal OpenCL. Lens results equal `opencl_lens_calib_test.cpp` cases. |
| VK6 | `PassEncoder<VulkanBackend, UploadRaw>` with the Develop steps and the scratch lifetime rule of `PlanExecutor` | `opencl_develop_pass.cpp` | `develop.sensor_linear` equals OpenCL. A stable render allocates nothing. |
| VK7 | GeometryResample, CameraToAp1, UploadRgb, DisplayToAp1 | `geometry_camera.cl`, `display_to_ap1.cl`, `opencl_display_to_ap1_pass.cpp` | `develop.image` equals OpenCL for a RAW and for a raster input. |
| VK8 | `VulkanGradeOps` point kernels, LUT images by content key | `opencl_primary_grade_pass.cpp`, `primary_grade.cl` | Each adjustment equals OpenCL. LUT resources are reused by content key. |
| VK9 | Neighbor kernels of the primary grade | `primary_grade_neighbor.cl` | Detail adjustments equal OpenCL. |
| VK10 | `VulkanLocalToneOps` | `opencl_local_tone_pass.cpp`, `local_tone.cl` | Local tone equals OpenCL at full size and for a region of interest. |
| VK11 | MaskEvaluate, MaskUnion, AI mask coverage | `opencl_mask_pass.cpp`, `mask.cl`, `opencl_ai_mask_coverage.cpp` | Mask values and coverage equal OpenCL. |
| VK12 | DiffusionFilter, `VulkanDrtOps`, DRT parameters | `opencl_drt_pass.cpp`, `opencl_diffusion_filter_pass.cpp`, `drt.cl` | Product output equals OpenCL for ACES and OpenDRT. |
| VK13 | Film print stage kernels | `opencl_film_print_pass.cpp`, `film_print.cl` | Negative to print output equals OpenCL. Reversal output equals OpenCL. |
| VK14 | Film print scan, film grain negative and post | `film_print_scan.cl`, `film_grain_*.cl`, `opencl_film_grain_pass.cpp` | Negative scan and grain equal OpenCL. |
| VK15 | Convolution layers, activation storage, workspace, stage profiler | `opencl/nn/*.cpp`, `demosaicnet_conv.cl` | Each layer equals the expected output of the weights file in the tolerance of the OpenCL layer tests. |
| VK16 | Bayer and X-Trans DemosaicNet modules, tiles, cache, the neural branch of Develop | `opencl_demosaicnet_*.cpp`, `demosaicnet_structural.cl` | Neural demosaic equals OpenCL. The time agrees with the VX4 record. |
| VK17 | Scope analyzer on the final DRT image, `FramePresenter<VulkanBackend>::Download`, `PipelineExecutor::vulkan_renderers_`, preference `vulkan` in the setting and in `alcedo-cli` | `opencl_scope_analyzer.cpp`, `opencl_frame_presenter.cpp` | Export of one RAW on Vulkan equals the OpenCL export in tolerance. Scope data equals OpenCL. |
| VK18 | `EditorBackend::Vulkan`, the display device of `VulkanContext` and start with `fromDeviceObjects`, Vulkan frame target in `IFrameSink` and in the lease enums (exported frame images, section 4.2), `EditorViewportRenderer` acquire barrier and import of the `VkImage`, orientation correction of `ViewportCompositePass`, mask coverage display. Rename of `editor_rhi_contracts_test.cpp`. | `OpenClOpenGlLeaseAdapter`, Metal zero copy path | The harness shows a rendered frame with `--editor-backend=vulkan`. The presented pixels equal the downloaded pixels. |
| VK19 | `ALCEDO_ENABLE_VULKAN=ON` in `win_release`, install of the `.spv` files, checks in `scripts/verify_windows_install_tree.ps1`, the Settings entry with the label `Experimental` | `alcedo_install_opencl_sources`, `package_windows.ps1` | The installed tree passes `gpu_kernel_check --vulkan`. |
| VK20 | UtNet2 forward code on the layers of VK15, or the DirectML path with resource import (VX4 decides) | AI Denoise plan, phase for the OpenCL forward code | Output equals the expected outputs of the lab release in its tolerance. |
| VK21 | Auto selects Vulkan on Windows. The record has the time and memory of Vulkan against CUDA and OpenCL for the scenarios of section 12 of the OpenCL plan. | `pipeline_accelerator.cpp`, `editor_backend.cpp` | Auto resolves to Vulkan. Each scenario time is in the record. |
| VK22 | Move the OpenCL backend, its tests, and the OpenGL pairing to `alcedo_studio/deprecated/`. Remove `ALCEDO_ENABLE_OPENCL` from the build. The comparison tests of B9 change their second backend to CUDA before this phase, or to stored statistics that the phase plan defines. | G10 plan | The build has no OpenCL symbol. |
| VK23 | Move the CUDA backend, its tests, and the Direct3D 11 pairing to `alcedo_studio/deprecated/`. Remove the CUDA toolkit from the Windows build. | G10 plan | The build has no CUDA symbol. The Windows installer has no CUDA runtime file. |

## 9. Experiment findings

Each category has a current state that is changed in place, and dated entries below it.

### 9.1 Kernel language (VX1)

Current state: Slang is the kernel language (B3). The Slang kernels give the OpenCL values on the
RTX 3080 Laptop GPU and on Iris Xe in the tolerance of the OpenCL tests, with the shared headers
and a macro layer of 28 lines. The `slangc` arguments, the `spirv-val` step, and the kernel rules
are in sections 4.2 and 4.3.

#### 2026-11-22: Slang and GLSL against OpenCL, NVIDIA and Intel

**Setup.**

| Item | Value |
| --- | --- |
| Source | `main` `61d59bc98` with this plan. Experiment code: `experiments/vulkan/vx1/` on branch `feature/vulkan-backend-experiments` |
| Kernels in Slang | `primary_grade_pointwise_rgba32f` (with `oklab.cl`); `film_print_exposure` (FP1), `film_print_first_amounts` (FP4 and FP5), `film_print_display` (FP6) |
| Shared headers in Slang | `color_encoding_math.h`, `aces_reference_gamut_compression.h`, `film_print_math.h` |
| Reference | The product `.cl` sources, concatenated in the order of `opencl_gpu_dag_programs.cpp`, `-cl-std=CL1.2`, on the same device |
| Tools | Vulkan SDK 1.4.328.1, `slangc` 2025.17.2, `glslc`, MSVC 14.44 |
| Devices | RTX 3080 Laptop GPU, driver 610.62 (Vulkan and OpenCL). Iris Xe, driver 101.7092 (Vulkan), OpenCL driver 32.0.101.7092 |
| Image | 2048 x 1536, RGBA32F storage images. Buffers are device addresses in the push constants |
| Validation | `VK_LAYER_KHRONOS_validation` on in each comparison run: 0 errors, 0 warnings |
| Evidence | worktree `build/tmp/vulkan_backend/vx1/`: one log for each case and device |

Grade input "photo": ACEScc 0.25 to 0.65 (2^-5.3 to 2^1.7 linear) and channel spreads of
+-0.06 ACEScc (+-1 stop). Grade input "stress": ACEScc -0.05 to 1.15 and channel spreads of
+-0.25 ACEScc (+-4.4 stops). Both inputs have a grey ramp, neutral pixels, and ties of the
largest channel. The grade has all 11 behaviors of `primary_grade.cl`: CAT02, exposure,
contrast, whites, blacks, curves (4 records), HLS, saturation, vibrance, tone wheels, and a
33-node LUT at strength 0.8.

Film input: linear AP1 from 2^-12 to 2^4, colours outside AP1, and a row of zeros. The tables
are synthetic, with the layout of `film_print_kernel_tables.hpp` and the value ranges of a stock:
64 u'v' nodes, 24 locus facets, curves and inhibition curves of 260 to 300 values, a print table
of 33 nodes. Variants: print with the negative grade and the cinema look on sRGB; light table
with the surround on PQ; and the form with the blurred inhibitor (`inhibitor_blur` 1).

**Result: difference of Vulkan (Slang) from OpenCL.**

| Case | Tolerance | RTX 3080 Laptop GPU | Iris Xe |
| --- | --- | --- | --- |
| Grade, photo input | 2.0e-4 ACEScc | 3.8e-5 | 6.7e-5 |
| Grade, stress input | 2.0e-4 ACEScc | 1.8e-2 (30335 of 9.4 million values above) | 1.9e-2 (31427 above) |
| Film print, sRGB | 0.5 codes (8 bit) | 2.4e-4 codes | 3.0e-4 codes |
| Film light table, PQ | 0.5 codes | 1.9e-4 codes | 1.2e-4 codes |
| Film, blurred inhibitor form | 0.5 codes | 2.4e-4 codes | 2.7e-4 codes |

The tolerances are those of `OpenClPrimaryGradeMatchesCudaReferenceWithinTolerance` and of
`kRampLimitCodes` in `opencl_film_print_test.cpp`. In the film cases, the log exposure between
the kernels differs by 1.9e-6 log10 at most and the FP4 and FP5 amounts by 1.3e-6. Alpha is equal in each
case.

**Result: the stress input.** Each adjustment alone on the stress input differs by 3.3e-6 or
less, except contrast (2.4e-3, 73 values on NVIDIA). The contrast outliers have one channel near
-0.35 ACEScc, in the segment of the curve near the floor where linear light is below 2^-15. There
the channel is a difference of cubes of the OkLab inverse, and a difference of a few float32 ulp
in linear light becomes 4e-4 ACEScc. A double-precision form of the contrast on the host gives:

| Contrast alone, stress input, NVIDIA | Largest difference | Values above 2.0e-4 |
| --- | --- | --- |
| OpenCL against double precision | 1.6e-3 | 118 |
| Vulkan against double precision | 1.3e-3 | 110 |

The same backend on the two devices, full grade:

| Pair | Stress input | Photo input |
| --- | --- | --- |
| OpenCL NVIDIA against OpenCL Intel | 1.6e-2 (29130 above) | 3.1e-5 |
| Vulkan NVIDIA against Vulkan Intel | 2.9e-2 (34325 above) | 4.3e-5 |

So the stress difference is a property of float32 in this grade. OpenCL has it between two
vendors. The comparison input of B9 has the value range of the input of the OpenCL test.

**Result: the shared headers.** Each header gets a `__SLANG__` branch of macros: 13 lines in
`color_encoding_math.h`, 3 lines in `aces_reference_gamut_compression.h`, 12 lines in
`film_print_math.h`. `film_print_math.h` also gets 7 changed lines (`&params->field[0]` for an
array member) and one `#if !defined(__SLANG__)` block around the host function
`FilmKernelNegativeScanTerms` (`const T*` pointers to values of the caller). The OpenCL program
and the MSVC host build compile the changed headers with no change of result. CUDA and Metal did
not compile them in this experiment; the changed form is valid C++ and Metal Shading Language.

**Result: `cbrt`.** SPIR-V has no `cbrt`. `exp2(log2(|x|) / 3)` alone gives 144 contrast values
above 2.0e-4 against double precision; with one Newton step 110; with two steps 106. One step is
the product form.

**Result: build time and size (median of 5 runs).**

| Source | Compile | SPIR-V | Source lines (code) |
| --- | --- | --- | --- |
| `primary_grade.slang`, one entry point, `-O2` | 343 ms | 42764 bytes | 316 |
| `film_print.slang`, three entry points, `-O2` | 442 ms | 108156 bytes | 104 |
| `primary_grade.comp` (GLSL), `glslc -O` | 159 ms | 40308 bytes | 265 |

The Slang grade source has about 50 lines of experiment switches (`VX1_*` macros). The GLSL
kernel gives the same differences from OpenCL as the Slang kernel on both devices. GLSL can
include `color_encoding_math.h` with a branch of 9 lines. It cannot include `film_print_math.h`:
that header has 71 lines with `->`, 39 pointer declarations, and 14 sums of a pointer and an
offset, and GLSL has no pointer syntax. Pipeline creation takes 0.4 ms (Intel) and 1.3 ms
(NVIDIA); the shader caches of the drivers were warm, so this is not a first-start time.

**Result: defects of the tools and drivers.**

| Item | Behavior | Rule for the product |
| --- | --- | --- |
| Intel driver 101.7092 | `vkCreateComputePipelines` stops the process (access violation) for the grade kernel at `-O1` and `-O2`. The cause is `p + k` on a pointer to the record struct (the Curve slot). `-O0` creates the pipeline. `spirv-opt` reproduces the failure from the `-O0` module after `inline-entry-points-exhaustive`. `spirv-val` accepts the module, and NVIDIA runs it. | Section 4.2: a record address is `uint64_t` arithmetic. With that form, the kernel at `-O2` runs on Intel with the results above. |
| `slangc` 2025.17.2 | With one `-entry`, the SPIR-V entry point is `main`. With more than one, each keeps its name. | `-fvk-use-entrypoint-name` (section 4.3). |
| `slangc` 2025.17.2 | A copy of a record from a device pointer into a local value, at `-O0`, gives an `OpPhi` with a wrong count of incoming blocks. `slangc` reports no error. | `spirv-val` in the build (section 4.3). |
| `slangc` 2025.17.2 | `-fp-mode precise` gives the same results and adds no `NoContraction` decoration. | The default floating-point mode. |

**Result: Metal target (step 5).** `slangc -target metal` accepts device pointers and pointer
arithmetic, also on struct pointers. It rejects the address-of operator `&`, so the film kernels
with `film_print_math.h` do not compile for Metal in this form. The grade kernel without `&` and a
pointer probe compile with `xcrun metal` (Apple metal 32023.864, `-std=metal3.0`) with 0 errors
and 0 warnings. They did not run on the Mac. Metal is outside this plan (section 1.4), so this
only limits a later reuse of the Slang film kernels on macOS.

**Indicative times (best of 5, OpenCL event profiling against Vulkan timestamps).**

| Kernel | RTX 3080 OpenCL | RTX 3080 Vulkan | Iris Xe OpenCL | Iris Xe Vulkan |
| --- | --- | --- | --- | --- |
| Grade, 11 adjustments, photo input | 0.78 ms | 0.76 ms | 12.5 ms | 9.1 ms |
| FP1 exposure | 0.60 ms | 0.41 ms | 5.9 ms | 4.3 ms |
| FP4 and FP5 amounts | 0.19 ms | 0.19 ms | 2.9 ms | 2.9 ms |
| FP6 display | 0.51 ms | 0.34 ms | 7.8 ms | 5.4 ms |

The OpenCL kernels read `image2d_t` through a sampler; the Vulkan kernels read storage images.
VX1 makes no performance decision.

**Items outside this record.** The film tables are synthetic; VK13 compares the real tables of
each stock. The CUDA backend was not compared. A first-start pipeline creation time was not
measured.

### 9.2 Qt Quick on Vulkan (VX2)

Current state: Qt Quick 6.9.3 on Vulkan shows a compute-written `VkImage` with zero copy on the
RTX 3080 Laptop GPU and on Iris Xe, with no validation error. The product composite shaders and
the display table give the Direct3D 11 pixels exactly, after one correction of the orientation.
Drag frame times of the split device are within 1 % of Direct3D 11 (p99). Resize fails the
criterion: Iris Xe needs about 70 ms for each resize step against 16.5 ms on Direct3D 11. The
pipeline and Qt Quick use two `VkDevice` objects on one physical device (B5); one device with a
queue mutex races with Qt's `vkDeviceWaitIdle`. With the pipeline on a second queue or device, the
GUI thread waits about 10 ms of each frame on the NVIDIA driver (Direct3D 11: 0.6 ms). VK18 stops
until the plan has a new resize design.

#### 2026-11-22: Qt Quick Vulkan with devices of the program, against Qt Quick Direct3D 11

**Setup.**

| Item | Value |
| --- | --- |
| Source | `main` `61d59bc98` with this plan. Experiment code: `experiments/vulkan/vx2/` on branch `feature/vulkan-backend-experiments` |
| Program | `vx2_viewport`, a stand-alone Qt Quick program (no QML engine) with one `QQuickRhiItem` with the settings of `EditorViewportItem` (RGBA32F colour buffer, no mirror, no blending). It draws with the product shaders `editor_viewport.vert` and `editor_viewport.frag` unchanged, the vertex data of `ViewportCompositePass`, and a 65-node display table. The product harness `EditorRhiHarness` needs the full product build in the worktree, so the program stands in for it. |
| Vulkan mode | The program creates the `VkInstance` (surface extensions, `VK_EXT_swapchain_colorspace`) and the device, and gives them to Qt with `QVulkanInstance::setVkInstance` and `QQuickGraphicsDevice::fromDeviceObjects`. A producer thread writes the photo into a ring of five RGBA32F images with a compute shader on the pipeline queue, waits its fence, and Qt wraps the image with `QRhiTexture::createFrom`. |
| Direct3D 11 mode | `QSGRendererInterface::Direct3D11` on the same GPU (`fromAdapter` with the LUID). A QRhi compute pass in the Qt frame runs the same compute shader (one GLSL source, `qsb` to HLSL; the SPIR-V of the same `.qsb` file runs in Vulkan mode). |
| Queue modes | separate: queue 1 of the graphics family of the Qt device. shared: queue 0 with the queue mutex, held from `beforeFrameBegin` to `afterFrameEnd` (frame) or from `afterRendering` to `afterFrameEnd` (submit). split: queue 0 of a second `VkDevice` |
| Display table | sRGB to the ICC profile of the monitor of the window, LittleCMS, relative colorimetric with black point compensation, the table layout of `DisplayTransformLut` |
| Devices | RTX 3080 Laptop GPU, driver 610.62. Iris Xe, driver 101.7092. Qt 6.9.3, Vulkan SDK 1.4.328.1 |
| Screens | LG ULTRAFINE, 3840 x 2160, 60 Hz, scale 2, output of the NVIDIA adapter, ICC profile `LG ULTRAFINE`. Laptop panel, 2560 x 1440, 240 Hz, scale 1.5, output of the Intel adapter, profile `sRGB IEC61966-2.1` |
| Load | Drag: a new photo for each frame (requested at `frameSwapped`), 2560 x 1600, 8 s, the first second dropped. Resize: `QWindow::resize` every 16 ms, a triangle wave from 900 x 560 to 1500 x 940 logical pixels. Stress: drag and resize together, the producer back to back. Interactive resizing with the mouse (the Windows sizing loop) needs real input and was not run. |
| Evidence | worktree `build/tmp/vulkan_backend/vx2/`: one log for each run, grabs as PNG |

Measures: the frame period is the time between two `afterFrameEnd` signals. Missed vsyncs are the
vsync intervals of the measured time minus the frames. The GUI block is the lateness of a 2 ms
timer on the GUI thread (the time that an input event waits). The resize latency is the time from
`QWindow::resize` to the end of the first frame with the new swapchain size.

**Result: zero copy display and validation.** Both devices show the producer image with zero copy
in each queue mode, with `VK_LAYER_KHRONOS_validation` on: 0 errors and 0 warnings outside the
stress runs. `R32G32B32A32_SFLOAT` has linear filtering on both devices, so the linear sampler of
the composite pass is valid. A device with the minimum features (no core feature) gives 0
validation errors with Qt Quick too.

**Result: display pixels.** The window is grabbed with `QQuickWindow::grabWindow` at one device
pixel for each photo pixel (960 x 720). The photo has grey and channel ramps, 4096 colours, values
from -0.25 to 1.5, and an orientation mark.

| Device, screen | Vulkan against Direct3D 11 | Both against a CPU composite (double precision) |
| --- | --- | --- |
| RTX 3080 Laptop GPU, LG ULTRAFINE | 0 of 2073600 values differ | 0.56 codes at most |
| Iris Xe, laptop panel | 0 of 2073600 | 0.51 codes |
| Iris Xe, LG ULTRAFINE (other adapter) | 0 of 2073600 | 0.51 codes |
| RTX 3080 Laptop GPU, split device | 0 of 2073600 | 0.56 codes |

The product shaders with the product vertex data draw the photo upside down on Vulkan: the grab
matches the CPU composite only after a vertical flip (0.56 codes). `QRhi::isYUpInNDC()` is false
on Vulkan and true on Direct3D 11. The correction is `scaleZoom.y = -1` when `isYUpInNDC()` is
false; all values in the table use it. VK18 adds it to `ViewportCompositePass`.

Qt creates the swapchain with `VK_FORMAT_B8G8R8A8_UNORM`, `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`,
`VK_PRESENT_MODE_FIFO_KHR`, and 3 images on both devices (`VK_LAYER_LUNARG_api_dump`). The format
is UNORM, so the display table output reaches the monitor without a second encoding. The NVIDIA
surface also offers `R16G16B16A16_SFLOAT` with `EXTENDED_SRGB_LINEAR` and `A2B10G10R10_UNORM`
with `HDR10_ST2084`; Qt reports `HDRExtendedSrgbLinear` and `HDR10` as supported on Vulkan and
neither on Direct3D 11 for the same screen. The Intel surface offers 8-bit formats only.

**Result: drag frame times, LG ULTRAFINE (60 Hz), RTX 3080 Laptop GPU.**

| Mode | Period p50 | p99 | max | Missed vsyncs | GUI block p95 | GUI block max |
| --- | --- | --- | --- | --- | --- | --- |
| Direct3D 11 | 16.66 ms | 18.27 ms | 19.39 ms | 0 | 0.58 ms | 2.8 ms |
| Vulkan, shared queue, submit lock | 16.75 / 16.61 ms | 19.09 / 19.03 ms | 22.2 / 20.4 ms | 0 / 0 | 0.49 / 0.37 ms | 19.2 / 35.3 ms |
| Vulkan, shared queue, frame lock | 16.67 ms | 19.29 ms | 21.6 ms | 0 | 0.54 ms | 20.4 ms |
| Vulkan, separate queue | 23.91 / 16.67 ms | 28.65 / 18.94 ms | 29.8 / 20.3 ms | 1 / 0 | 19.8 / 10.2 ms | 35.8 / 36.8 ms |
| Vulkan, split device | 16.65 ms | 18.45 ms | 19.18 ms | 0 | 10.2 ms | 24.5 ms |

Two values are two runs. The p99 period of the shared and split modes is at most 6 % above Direct3D 11 (one
separate-queue run had alternating periods of about 9 and 24 ms with one missed vsync). The
content latency (request to frame end) is one frame in each mode.

The render thread of the threaded render loop calls `QRhi::beginFrame` before `sync()`, and the
GUI thread waits until `sync()` (Qt 6.9 `QSGRenderThread::syncAndRender`). When the pipeline
submits on a queue other than Qt's queue (separate queue or split device), `beginFrame` waits
about 10 ms on the NVIDIA driver, and the GUI thread waits with it: the GUI block p95 is 10 ms
against 0.5 ms. With the pipeline on Qt's queue the wait is 0.06 ms. The wait is in the fence or
the image acquire of `beginFrame`; the experiment cannot divide it further. Frame periods are equal
in both cases.

**Result: pipeline load during the drag, RTX 3080 Laptop GPU, LG ULTRAFINE.** The producer runs
back to back with about 14 ms of GPU work for each image; the window renders each vsync.

| Mode | Period p50 | p99 | Missed vsyncs | Producer images/s |
| --- | --- | --- | --- | --- |
| split device | 16.61 ms | 19.48 ms | 0 | 64.8 |
| separate queue | 24.51 ms | 30.95 ms | 1 | 65.0 |
| shared queue, submit lock | 6.62 ms | 29.90 ms | -1 | 30.1 |

The split device keeps the window at vsync and the pipeline at full speed. On one shared queue the
pipeline gets half the images, and the frame periods alternate. On Iris Xe all modes give about
50 frames/s with 15 ms of pipeline work for each image (one GPU engine for both).

**Result: resize.**

| Device, screen, mode | Resize latency p50 | p95 | p99 | max |
| --- | --- | --- | --- | --- |
| RTX 3080, LG, Direct3D 11 (two runs) | 16.65 / 16.45 ms | 17.51 / 17.26 ms | 17.99 / 17.48 ms | 20.2 / 19.8 ms |
| RTX 3080, LG, Vulkan separate (two runs) | 15.07 / 15.73 ms | 25.07 / 30.99 ms | 34.16 / 33.90 ms | 47.3 / 61.8 ms |
| RTX 3080, LG, Vulkan shared | 15.38 ms | 29.95 ms | 31.90 ms | 42.7 ms |
| RTX 3080, LG, Vulkan split | 15.16 ms | 20.18 ms | 26.28 ms | 34.9 ms |
| RTX 3080, laptop panel, Direct3D 11 | 16.36 ms | 20.06 ms | 23.83 ms | 26.6 ms |
| RTX 3080, laptop panel, Vulkan separate | 24.76 ms | 38.99 ms | 45.17 ms | 54.9 ms |
| Iris Xe, laptop panel, Direct3D 11 (two runs) | 16.45 / 16.53 ms | 29.77 / 19.70 ms | 41.68 / 29.71 ms | 63.4 / 39.5 ms |
| Iris Xe, laptop panel, Vulkan shared, submit lock (two runs) | 74.74 / 85.74 ms | 94.62 / 123.0 ms | 103.7 / 162.4 ms | 106.8 / 176.9 ms |
| Iris Xe, laptop panel, Vulkan shared, no mutex | 65.98 ms | 93.70 ms | 116.4 ms | 134.7 ms |
| Iris Xe, laptop panel, Vulkan split | 67.00 ms | 80.16 ms | 86.40 ms | 97.5 ms |

On Iris Xe the time goes into the render thread outside the frame (p95 77 to 106 ms between two
frames), where Qt recreates the swapchain: `QRhiVulkan::recreateSwapChain` calls
`vkDeviceWaitIdle`, creates the new swapchain, and releases the old one with a second
`vkDeviceWaitIdle`. The queue mode does not change it. On the RTX 3080 the median is equal to
Direct3D 11 and the tail is longer; the split device has the shortest tail. During a resize the
GUI thread waits for the resized frame on both APIs (GUI block p50 32 ms on Direct3D 11, 14 to 16 ms on
Vulkan RTX 3080, 70 to 94 ms on Vulkan Iris Xe).

**Result: the queue rule (stress, validation with thread safety).**

| Device, mode | Threading errors | Result |
| --- | --- | --- |
| RTX 3080, separate queue | 10 (limit of the layer) | `vkQueueSubmit` of the pipeline during Qt's `vkDeviceWaitIdle` |
| RTX 3080, shared queue, no mutex | 10 | process stopped with `0xC0000409` |
| RTX 3080, shared queue, frame lock | 10 | `vkDeviceWaitIdle` and `vkQueueSubmit` at the same time |
| RTX 3080, shared queue, submit lock | 10 | the same |
| Iris Xe, shared queue, no mutex | 10 | the process stopped responding; ended after several minutes |
| Iris Xe, shared queue, frame lock | 10 | `vkDeviceWaitIdle` and `vkQueueSubmit` at the same time |
| Iris Xe, shared queue, submit lock | 10 | the same |
| RTX 3080, split device | 0 | 156 producer images/s, 10 s |
| Iris Xe, split device | 0 | 42 producer images/s, 10 s |

`vkDeviceWaitIdle` needs exclusive access to all queues of its device. Qt calls it in
`recreateSwapChain` (swapchain resize), in `releaseSwapChainResources`, and in `destroy`; the
resize runs before `beforeFrameBegin`, outside both lock spans, and Qt has no signal before it.
`QRhi::finish()` calls `vkQueueWaitIdle` on Qt's queue. On a second device none of these calls
touches the pipeline queue.

**Result: two GPUs.** DXGI lists the LG ULTRAFINE as an output of the NVIDIA adapter and the
laptop panel as an output of the Intel adapter. Vulkan lists the RTX 3080 first and two Iris Xe
devices with different LUIDs (`19f13`, the adapter with the panel output, and `261de`, with no
output); `vkGetPhysicalDeviceSurfaceSupportKHR` is true for each device on each screen. Qt
Direct3D 11 without `fromAdapter` selects the RTX 3080 on both screens. A device can present to
the screen of the other adapter: the drag p99 is 18.19 ms (RTX 3080 on the panel; Direct3D 11
17.62 ms) and 19.05 ms (Iris Xe on the LG ULTRAFINE; Direct3D 11 32.78 ms with 5 missed vsyncs),
and the RTX 3080 resize p50 on the panel is 24.76 ms (Direct3D 11 16.36 ms). The rule for VK0:
`ALCEDO_VULKAN_DEVICE` selects the device; by default the discrete GPU, as Qt Direct3D 11 does,
and the device is matched to DXGI by LUID.

On the 240 Hz laptop panel Direct3D 11 (both GPUs) and Vulkan on the RTX 3080 run at 60 frames/s; Vulkan
on Iris Xe runs at 82 to 128 frames/s. Frame times on that panel are therefore compared only
inside one API.

**Result: injected layers.** The loader inserts `VK_LAYER_NV_optimus`, `VK_LAYER_NV_present`, and
`VK_LAYER_OBS_HOOK` (OBS Studio) by default, and `VK_LAYER_VALVE_steam_overlay` and
`VK_LAYER_VALVE_steam_fossilize` with their enable variables. Stress runs of 8 s with all five, on
both devices with the split device, ended normally with frame counts like those of the runs
without the layers. The EOS overlay layer (`EOSOverlayVkLayer-Win64.json`, `api_version`
1.2.136) was found with its enable variable and not inserted; the loader log gives no reason.
Screen recording and overlay drawing were inactive in these runs, so R7 keeps its response.
`VK_LAYER_VALVE_steam_fossilize` wrote two pipeline cache files (`fossilize.<hash>.<n>.foz`) into
the working directory of the process; the application sets its working directory to a folder of
its own before it creates the instance.

**Items outside this record.** Interactive resizing with the mouse; a monitor with HDR on (both
outputs report SDR); device loss (VX5); `preferredExtensionsForImportedDevice` was not enabled in
the experiment (Qt pretends that the extensions are present; no validation error resulted).

### 9.3 Long dispatch (VX3)

Current state: on the RTX 3080 Laptop GPU the Qt Quick window of the display device keeps vsync
during a 45 megapixel export on the pipeline device in every submission form, because the two
`VkDevice` objects are separate GPU contexts and the GPU switches between them at workgroup
boundaries. The submission form decides the wait of the preview render, which shares the pipeline
queue with the export: the whole export in one submission (about 100 ms) delays a 2560-pixel
preview from 7 ms to 101 ms; submissions of 8 ms with one in flight keep it at 12 to 16 ms (a 4K
preview at 19 to 23 ms) and cost no measurable export time (88.3 ms against 90.3 ms whole). One
workgroup that runs for 3 s stops the display device for the 3 s. The rule is in section 4.2.

The product exports one photo at a time with one export submission in flight (user decision
2026-11-22, after this record). Of the forms with one in flight, 8 ms has the lowest export cost
(Iris Xe, indicative: 1515 ms against 1577 ms at 4 ms) and keeps the preview wait at 8 ms. Two in
flight lowers the export time further (86.5 ms at 8 ms) and stays outside the product rule.

#### 2026-11-22: 45 megapixel export, 2560 and 4K preview, and 3 s kernels on the pipeline device

**Setup.**

| Item | Value |
| --- | --- |
| Source | `main` `61d59bc98` with this plan. Experiment code: `experiments/vulkan/vx3/` on branch `feature/vulkan-backend-experiments`, with the VX2 viewport code |
| Program | `vx3_load`: the VulkanContext of VX2 with two devices on one physical device (B5). Qt Quick on the display device draws a 1280 x 800 window each vsync; the VX2 producer writes its viewport image. The export and the preview run on the pipeline device, each with its own command pool, query pool, and fences; one lock serializes `vkQueueSubmit` on the pipeline queue. |
| Export | 8256 x 5504 (45.4 megapixels), 226 dispatches: Local Tone with the canonical planes at 2048 x 1366 (7 levels, 15 samples; 216 dispatches on the planes and the full-size apply), then film print FP1, FP4 and FP5 (inhibitor form), FP6 (the VX1 Slang kernels), with three pairs of separable blurs of radius 32 (spread, inhibitor, halation). RGBA32F storage images, planes of 3 floats per pixel as device addresses. The film tables are the synthetic tables of VX1. 3.0 GiB of device memory. |
| Preview | The same 226 dispatches at 2560 x 1707 (blur radius 10) each 16.7 ms (a drag: a new request each frame, one render at a time) or at 3840 x 2560 (blur radius 15) each 250 ms (the settled view). One submission each. |
| Submission forms | whole: one submission. op: one submission for each dispatch (226). budget B: submissions of at most B ms of estimated GPU time; a dispatch above the room of a submission is divided into bands of 16-row groups with `vkCmdDispatchBase`. The estimate is the time per row from a calibration render of the same size (timestamps after each dispatch). In flight: the count of export submissions on the queue before the export thread waits for a fence. |
| Spin kernels | `vx3_spin`: a chain of dependent multiply-adds of a chosen length. wide: one dispatch of 8192 x 4096 threads (131072 groups), about 3 s in all. long: one group of 16 x 16 threads for 3 s. many: 300 dispatches of 10 ms in one submission. |
| Devices | RTX 3080 Laptop GPU, driver 610.62, LG ULTRAFINE at 60 Hz (screen 0). Iris Xe, driver 101.7092, laptop panel at 240 Hz (screen 1). Vulkan SDK 1.4.328.1, `slangc` 2025.17.2, Qt 6.9.3 |
| Windows | `TdrDelay` and `TdrDdiDelay` 60 s on this computer (the Windows default is 2 s); `HwSchMode` unset |
| Run check | `gpu_idle_guard.ps1` before and after each run: no process of the model experiments or tests (python, ONNX Runtime, alcedo, ctest), no process other than the desktop compositor above 10 % of a GPU engine, NVIDIA use at most 40 %. A run counts when both checks pass and the reference second before the export (same run) has a frame period p99 of at most 19 ms. The user's `UtNet2PhotoTimingTool`, `ctest` and `python` runs held the series several times. |
| Validation | `VK_LAYER_KHRONOS_validation` on in the check runs (2048 x 1536 export; 4096 x 2730 export with the 2560 preview): 0 errors, 0 warnings, 0 threading errors. Timing runs have the layer off. |
| Evidence | worktree `build/tmp/vulkan_backend/vx3/`: one log for each run (`kernels_*`, `win_*`, `spin_*`), `summarize.ps1` for the table form. `superseded/`: runs of a first harness that drew the window only with a new image. |

**Result: dispatch times of the export, no window (best of 3 calibration renders).**

| Class | Dispatches | RTX 3080 Laptop GPU | Iris Xe |
| --- | --- | --- | --- |
| Point, full size (FP1, FP4/FP5, FP6, Local Tone apply) | 4 | 3.0 to 4.8 ms each (0.067 to 0.106 ms per megapixel) | 45 to 65 ms each (0.99 to 1.44 ms per megapixel) |
| Separable blur of radius 32, full size | 6 | 11.0 to 11.9 ms each (0.24 to 0.26 ms per megapixel) | 187 to 200 ms each (4.1 to 4.4 ms per megapixel) |
| Local Tone planes at 2048 x 1366 | 216 | 3.3 ms in all, largest 0.71 ms | 41 ms in all, largest 10.1 ms |
| Render | 226 | 86.3 ms | 1424.5 ms |

**Result: export time for each submission form, no window (one render, wall time).**

| Form | Submissions (RTX 3080) | RTX 3080 Laptop GPU | Iris Xe |
| --- | --- | --- | --- |
| whole | 1 | 90.3 ms | 1553.4 ms |
| op | 226 | 106.2 ms | 1591.4 ms |
| budget 4 ms, 1 in flight | 22 | 93.1 ms | 1576.9 ms |
| budget 4 ms, 2 in flight | 22 | 87.1 ms | 1458.9 ms |
| budget 8 ms, 1 in flight | 11 | 88.3 ms | 1514.8 ms |
| budget 8 ms, 2 in flight | 11 | 86.5 ms | 1456.2 ms |
| budget 16 ms, 1 in flight | 6 | 87.1 ms | 1465.3 ms |
| budget 33 ms, 2 in flight | 3 | 85.4 ms | 1452.6 ms |

A submission costs about 0.07 ms of CPU and GPU idle time on the RTX 3080 and 0.13 ms on Iris Xe
when the thread waits for each fence (op and one in flight); with two in flight the GPU has the
next submission at the fence and the idle time is below 1 ms for the whole render. The Iris Xe
whole render took about 100 ms more than the split forms; the record has one run of each form.

**Result: preview during the export, RTX 3080 Laptop GPU, LG ULTRAFINE.** Each run has 12 exports
back to back. Preview latency: from the start of the request to the fence of the preview
submission. Frame period: Qt Quick window, during the exports.

| Preview | Export form | Preview latency p50 / p95 / max | Frame period p99 / max | Export time (mean of 12) |
| --- | --- | --- | --- | --- |
| 2560, drag | none | 6.9 / 8.4 / 8.8 ms | 17.7 / 18.8 ms | - |
| 2560, drag | whole | 100.7 / 103.4 / 103.8 ms | 17.4 / 17.5 ms | 101.0 ms |
| 2560, drag | op | 14.3 / 20.1 / 22.2 ms | 17.8 / 18.2 ms | 181.8 ms |
| 2560, drag | budget 16 ms, 1 in flight | 24.0 / 25.8 / 26.4 ms | 17.9 / 18.0 ms | 137.8 ms |
| 2560, drag | budget 8 ms, 1 in flight | 12.2 / 15.5 / 16.4 ms | 17.6 / 18.1 ms | 161.6 ms |
| 2560, drag | budget 4 ms, 2 in flight | 13.8 / 16.2 / 16.7 ms | 17.6 / 18.0 ms | 162.3 ms |
| 4K, settle | none (reference second) | 16.2 to 16.5 ms (p50) | 18.3 / 19.0 ms | - |
| 4K, settle | whole | 78.5 / 102.1 / 108.0 ms | 17.6 / 17.7 ms | 103.8 ms |
| 4K, settle | budget 16 ms, 1 in flight | 23.9 / 27.8 / 31.9 ms | 18.8 / 23.0 ms | 102.5 ms |
| 4K, settle | budget 8 ms, 1 in flight | 18.6 / 20.8 / 22.6 ms | 17.8 / 17.8 ms | 103.1 ms |
| 4K, settle | budget 4 ms, 2 in flight | 20.9 / 21.1 / 21.1 ms | 17.7 / 18.6 ms | 102.5 ms |

The preview latency is the preview render (6.5 ms at 2560, 13.1 ms at 4K) and the export work
ahead of it on the queue: the whole export, or at most the submissions in flight. During a drag
the export gets the GPU between previews, so its time grows from 101 ms to 162 ms; at the settled
view (one preview each 250 ms) the export time stays at 103 ms. The Qt Quick frame period p99 is
within 1 ms of the reference in each form, the whole export included.

Some runs of the RTX 3080 had frame periods of about 6 and 28 ms alternating for the whole run,
the reference second before the export included; the run check marks those runs and the table
uses the runs of the same case with a clean reference.

**Result: preview during the export, Iris Xe (indicative).** The Iris Xe runs on the 240 Hz panel
have a reference frame period p99 of 31 to 56 ms (Vulkan on that panel runs at about 100 frames/s,
VX2), so the run check does not apply to them. A 2560 preview takes 110 to 190 ms of GPU time and a
4K preview 200 to 340 ms. The whole export beside a drag preview took 2.4 to 2.9 s in one
submission, and the preview waited 2.4 s. With split submissions a drag preview runs back to back,
the queue alternates one export submission and one preview render, and the export takes 16 to
37 s instead of 1.5 s.

**Result: 3 s kernels, RTX 3080 Laptop GPU, with a 2560 drag preview.**

| Form | Device loss | Frame period during the kernel p99 / max | Frames in 3 s | Preview latency |
| --- | --- | --- | --- | --- |
| many: 300 dispatches of 10 ms, one submission | none | 18.2 / 18.8 ms | 183 | 3053 ms |
| wide: one dispatch of 131072 groups, 3.2 s | none | 44.4 to 44.6 / 46.6 to 49.8 ms (two runs) | 171 to 184 | 3215 to 3250 ms |
| long: one group, 3.0 s | none | - | 1 | 3003 ms |

The many and wide forms keep the display device drawing: the GPU switches contexts between
dispatches and between groups. The wide runs have a reference second with the alternating periods
of the paragraph above, so their frame periods are indicative. With one group running, Qt Quick
drew one frame in 3 s: the GPU switches to another context only at a group boundary. The display
of the LG ULTRAFINE (an output of this GPU) is a context of the same GPU. With the Windows default
`TdrDelay` of 2 s, Windows resets a GPU that does not switch within 2 s, so the long form is the
form that reaches the timeout; this computer has 60 s and the test cannot confirm the reset. In
every form the preview waits for the whole kernel, as for a whole export.

**Items outside this record.** The Windows GPU timeout at 2 s (a change of `TdrDelay` needs a
restart of the computer); Iris Xe beyond the indicative values; a blur radius of 128 (the largest
`FilmBlurKernel`); queue priorities (`VK_EXT_global_priority`) as a second way to order the preview
before the export; a minimum share of the GPU for the export during a long drag.

### 9.4 Neural network throughput (VX4)

Current state: no measurement on Vulkan. Reference time: ONNX Runtime DirectML 0.82 s for
45.4 megapixels (AI Denoise plan, DN1 record).

### 9.5 Memory budget and device loss (VX5)

Current state: no measurement.

## 10. Risks and stop conditions

| Id | Risk | Detection | Response |
| --- | --- | --- | --- |
| R1 | Qt Quick on Vulkan on Windows has a worse resize or a different display color than Direct3D 11. | VX2: the resize part occurred (Iris Xe about 4 times the Direct3D 11 time for each step, RTX 3080 a longer tail). The color part did not occur. | VK18 stops. The plan gets a new resize design ([#1040](https://github.com/zidage/AlcedoStudio/issues/1040)) before more display work. The compute phases VK0 to VK17 continue. |
| R2 | A long dispatch causes a device loss or stops the editor. | VX3: a whole export in one submission holds the preview for 100 ms (RTX 3080) to 2.4 s (Iris Xe); one workgroup of 3 s stops the display device for 3 s. The Qt Quick frames keep vsync with workgroups of normal length. | The split rule of section 4.2 (one export at a time, 8 ms submissions, one in flight, workgroups below 1 ms) and the VK2 data rule. |
| R3 | Hand-written neural network kernels are slower than the vendor convolution code. | VX4 | DemosaicNet: the criterion of VX4 decides. UtNet2: VK20 uses DirectML with resource import, and the plan records the remaining interop pairing. |
| R4 | The pipeline and Qt Quick race on the queues of one device. | VX2 thread safety validation: occurred on both devices, with a second queue and with the queue mutex (Qt's `vkDeviceWaitIdle` at resize) | Two devices on one physical device (B5, section 4.2). With them the validation reports 0 threading errors. |
| R5 | The results differ between GPU vendors more than the tolerance. | Comparison tests on NVIDIA and Intel | The kernel marks the affected expressions as precise. The tolerance does not increase without a record. |
| R6 | The archive of OpenCL removes support for GPUs with no Vulkan 1.3 driver. | Device reports from users, VK19 experimental period | User decision before VK22 (section 1.5). |
| R7 | Software that injects a Vulkan layer makes the application crash. | VX2 step 6 (OBS hook, Steam overlay, Steam Fossilize, NVIDIA layers loaded: no crash, no hang), reports in the experimental period | The application disables the named implicit layers at start. |
| R9 | The GUI thread waits for the Vulkan `beginFrame` of the render thread while the pipeline submits on its own device (about 10 ms of each frame on the NVIDIA driver). | VX2 | [#1041](https://github.com/zidage/AlcedoStudio/issues/1041): the cause and a design. VK18 measures input latency in the editor with the split device. |
| R8 | The backends on `main` change faster than the Vulkan passes follow. | Failing comparison tests | Section 5, rule 3. |

Stop the implementation of a phase when:

- an experiment that the phase depends on has no record;
- the source audit of section 3 does not agree with the branch;
- the estimate of the phase passes 2000 lines;
- the phase needs a change to CUDA, OpenCL, or Metal behavior;
- the phase needs a substitute path that B8 prohibits.

Update this plan before the implementation continues.

## 11. Build and evidence rules

- Presets: `win_debug` with `-DALCEDO_ENABLE_VULKAN=ON` in `build/debug`. No new preset and no new
  build directory. The experiment worktree uses its own `build/debug` with the dependency setup of
  a worktree.
- MSVC commands run through the PowerShell tool with `scripts\msvc_env.cmd`.
- GPU tests run with `ctest -j 1`. Only the targeted Vulkan suites and the suites of their direct
  callers run. The full suite is the decision of the user.
- Each Vulkan test runs on the RTX 3080 Laptop GPU and on Iris Xe
  (`ALCEDO_VULKAN_DEVICE`). The record names the device of each result.
- Debug test runs enable the Vulkan validation layer. A validation error is a test failure.
- Logs and evidence go to `build/tmp/vulkan_backend/<phase>/`.
- A result that is unavailable is recorded as unavailable. A skipped test is recorded as skipped.

## 12. Completion record template

```text
Phase / date / status:
Source revision and branch:
Actual changed modules:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts, for each device:
Validation layer result:
Cross-backend comparison result (maximum difference, tolerance):
Performance or resource evidence, if required:
Evidence path:
Remaining defects or unavailable platforms:
```

## 13. Parameter table

| Parameter | Meaning | Value | Source |
| --- | --- | --- | --- |
| `ALCEDO_ENABLE_VULKAN` | CMake option that adds the Vulkan backend to the build | `OFF` until VK19 | this plan (B1) |
| `HAVE_VULKAN` | Compile definition of the option | set by the option | this plan |
| `ALCEDO_VULKAN_DEVICE` | Environment variable. Part of a device name that selects the device. | text | this plan, model `ALCEDO_OPENCL_DEVICE` |
| `gpu/acceleratorBackend` | QSettings key of the backend preference | adds `vulkan` | current source |
| Minimum API | Vulkan version that the device must report | 1.3 | this plan (B4) |
| Image formats | Formats of pipeline images | RGBA32F, R32F, R8 | current OpenCL backend |
| Comparison tolerance | Maximum difference between Vulkan and OpenCL in a test | the tolerance of the matching OpenCL test | this plan (B9) |
| Grade tolerance | Largest difference of the primary grade point kernel, ACEScc | 2.0e-4 | `opencl_grade_test.cpp` |
| Film tolerance | Largest difference of the film print output, 8-bit codes | 0.5 | `kRampLimitCodes`, `opencl_film_print_test.cpp` |
| `slangc` arguments | Arguments of each kernel group build | `-target spirv -profile spirv_1_6 -fvk-use-scalar-layout -fvk-use-entrypoint-name -O2` | VX1 (section 9.1) |
| `spirv-val` arguments | Validation of each module in the build | `--target-env vulkan1.3 --scalar-block-layout` | VX1 |
| `cbrt` Newton steps | Steps after `exp2(log2(\|x\|) / 3)` in the Slang `cbrt` | 1 | VX1 |
| Slang macro layer | Lines of the `__SLANG__` branches of the three shared headers | 28 | VX1 |
| Phase size limit | Maximum changed lines of one phase | 2000 | `alcedo-create-plan` skill |
| Windows GPU timeout | Time after which Windows resets a GPU that does not respond | 2 s (default); 60 s on the development computer (`TdrDelay`) | Windows display driver model; registry of the development computer |
| Export submission budget | Maximum estimated GPU time of one export submission | 8 ms | VX3 (section 9.3) |
| Export submissions in flight | Export submissions on the pipeline queue before the export waits for a fence | 1 | user, 2026-11-22; VX3 |
| Exports at a time | Export renders that run at one time | 1 | user, 2026-11-22 |
| Band | Unit of a dispatch that is divided over submissions | 16 rows (one row of 16 x 16 groups) | VX3 |
| Workgroup time limit | Maximum running time of one workgroup | 1 ms | VX3 (one group of 3 s stops the display device) |
| Preview sizes | Long edge of the preview render: during a drag, at the settled view | 2560, 3840 pixels | user, 2026-11-22 |
| Export render, RTX 3080 Laptop GPU | Local Tone and film print, 45.4 megapixels, 226 dispatches | 86 ms | VX3 |
| Export render, Iris Xe | The same render | 1.42 to 1.55 s | VX3 |
| UtNet2 time limit | Maximum UtNet2 time on Vulkan relative to DirectML | 2 times | this plan (VX4) |
| DirectML reference time | UtNet2, 45.4 megapixels, tile 1536, RTX 3080 Laptop GPU | 0.82 s | AI Denoise plan, DN1 record |
| Devices for each physical device | `VkDevice` objects that Alcedo creates: pipeline device and display device | 2 | VX2 (section 9.2) |
| Frame image handle type | External memory type of a viewport frame image | `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`, dedicated allocation | VX2 |
| Frame image ring | Viewport frame images for each viewport | at least 5 | VX2 (2 Qt frames in flight) |
| Swapchain of Qt (SDR) | Format, color space, present mode, images that Qt 6.9.3 creates | `B8G8R8A8_UNORM`, `SRGB_NONLINEAR`, FIFO, 3 | VX2, `VK_LAYER_LUNARG_api_dump` |
| Orientation correction | `scaleZoom.y` of the composite pass when `QRhi::isYUpInNDC()` is false | -1 | VX2 |
| Resize criterion | Largest resize latency of Vulkan relative to Direct3D 11 | 1.1 times | this plan (VX2) |
