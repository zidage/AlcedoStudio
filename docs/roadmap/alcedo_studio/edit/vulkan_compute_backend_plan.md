# Vulkan Compute Backend Master Plan

Date: 2026-11-22

Status: planned. Experiments VX1 to VX5 come first. VX1, VX2, VX3, and VX5 are complete
(2026-11-22, sections 9.1, 9.2, 9.3, and 9.5); VX5 has no restore run of a lost device (section
9.5). VX4 does not run: the neural networks use ONNX Runtime (user decision 2026-11-22, A5). VX2
changed B5 to two devices and stopped VK18 until the plan has a new resize design (section 9.2).
VK0 is complete (2026-11-23, #1063). VK1 is complete (2026-11-23, #1065). VK2 is complete
(2026-11-23, #1067). VK3 is complete (2026-11-23, #1080). VK4 is complete (2026-11-27, #1196).
VK5 is complete (2026-11-27, #1203). VK6 is complete (2026-11-27, #1209), on the Apple M4 with
MoltenVK and on Windows. VK7 is complete (2026-11-27, #1211). VK8 is complete (2026-11-27,
#1219). VK9 is complete (2026-11-27, #1225). VK10 is complete (2026-11-27, #1228). VK11 is
complete (2026-11-27, #1230).

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
| A5 | The neural networks that run in the pipeline use ONNX Runtime, as the UtNet2 model of AI Denoise does (DirectML on Windows, AI Denoise plan). A Vulkan neural network is not planned now: VX4 does not run, and VK15, VK16, and VK20 are deferred (user, 2026-11-22; this replaces the first form of A5, "DemosaicNet and UtNet2 move to Vulkan compute"). |
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

- macOS. The Metal backend keeps its kernels and its display path. MoltenVK is not a product
  path. The compute phases also run on an Apple M4 with MoltenVK as a development check (user
  request 2026-11-27, VK6); the reference backend of the comparison tests there is Metal.
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
| Allocation above the memory limit (section 4.2) | The render fails with the requested size, the limit, and the budget. No frame is published. The pool does not call `vkAllocateMemory` above the limit: Windows places such memory in shared system memory at about 1/300 of the speed and does not move it back (VX5). |
| A fence wait of the pipeline device returns `VK_TIMEOUT` | The render fails with a device stall error. A stall lasts until the Windows GPU timeout resets the GPU; a device loss follows (VX5). |
| `VK_ERROR_DEVICE_LOST` | The render fails with the error. The editor shows the error and does not publish a frame. `VulkanContext` restores the device (section 9.5, restore procedure). |

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
  `QQuickWindow` exists (initial image layouts). Qt Quick is the only user of the display queue
  (VX2, section 9.2). The render devices (editor, thumbnails, export) run on threads of their own
  and submit to the pipeline queue only through `VulkanContext::SubmitToPipelineQueue`, which
  holds one lock for `vkQueueSubmit2` (VK2).
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
- A record in a buffer has no member that is a struct. Two structs of one parameter range are two
  records, each found with address arithmetic. With a nested struct (`params->pattern.width`)
  `slangc` writes an access chain of an access chain, and the Intel driver stops in
  `vkCreateComputePipelines` for every entry point of the module (VK4 record). A push constant
  block can have a struct member.
- A kernel does not index a push constant array with a loop counter: it uses constant indices.
  With the loop form the Intel driver wrote the input values unchanged (VK4 record).
- A kernel does not take the signed remainder of a coordinate that can be negative. It adds a
  positive multiple of the period and takes an unsigned remainder. The NVIDIA driver gave the
  `OpSRem` of a negative coordinate that comes from the invocation id the unsigned result (VK4
  record).
- Buffer data has the scalar layout (`scalarBlockLayout`, `slangc -fvk-use-scalar-layout`), so a
  struct of a shared header has the layout of the host C++ struct.
- Descriptor sets use `VK_KHR_push_descriptor`. A kernel has one set. The set holds images only.
- Memory goes through VMA. The memory limit is the `VK_EXT_memory_budget` budget of the
  device-local heap minus a reserve of 512 MiB, or the application limit when that is lower. The
  pool reads the budget before each new allocation, because the budget changes with the memory use
  of other processes (VX5: from 7248 MiB to 3.9 GiB). The budget and the usage are values of the
  process on the adapter: the pipeline device, the display device, and the Direct3D 12 memory of
  ONNX Runtime DirectML count in them (VX5: equal to the DXGI values). Above the limit the pool
  frees cache entries; when no cache entry is left, the allocation fails (section 2.4).
  `vkAllocateMemory` does not fail at the budget, so the limit is a check of the pool (VX5).
- Allocations use `VK_EXT_memory_priority`: 0.2 for cache entries (`GraphImageCache`,
  `NodeResultCache`, `MaskTextureCache`), 0.8 for the images of the render in progress and for the
  frame image ring. Windows moves low-priority memory out first (VX5). The device does not enable
  `VK_EXT_pageable_device_local_memory`: with it an allocation does not fail even at 115 % of the
  heap (VX5).
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

The GLSL.std.450 `exp`, `exp2`, `log`, `log2`, and `pow` are less precise than the OpenCL
functions, and the shared curves amplify the difference. The `__SLANG__` branch of
`color_encoding_math.h` maps its `CE_*` macros to the functions of `vulkan/shader/math.slangh`
(about 1 ulp; VK3 record), so each kernel group that includes the header has `src/` and
`src/include/` in its include folders.

`slangc` writes no `NoContraction` decoration, also not for `precise` or `-fp-mode precise`, and
the NVIDIA driver fuses products into FMAs and combines the adds of a sum. A kernel whose OpenCL
program has `#pragma OPENCL FP_CONTRACT OFF` uses `AlcedoExactMul` for each multiply and
`AlcedoExactAdd` for each sum of three or more terms (`math.slangh`: an `OpFMul` or `OpFAdd` with
`NoContraction` from a `spirv_asm` block). Today that is the RCD group (VK4 record).

The GLSL.std.450 `sin` and `cos` have only an absolute error of 2^-11 in [-pi, pi], and `tan`,
`asin`, `atan`, and `atan2` are derived from them. A kernel that needs the precision of the OpenCL
trigonometric functions calls `AlcedoSin`, `AlcedoCos`, `AlcedoTan`, `AlcedoAsin`, `AlcedoAtan`,
and `AlcedoAtan2` of `math.slangh` (the single-precision Cephes forms, about 1 ulp for
|x| < 8192). Today that is the lens group: with the built-in functions the lens projections
differed from OpenCL by up to 4e-2 at up to 9937 values of a 640 x 480 image (VK5 record).

MoltenVK compiles the kernels with the Metal fast math option unless the instance sets
`MVK_CONFIG_FAST_MATH_ENABLED` to 0. `VulkanContext` sets it on macOS through
`VK_EXT_layer_settings`; with it the Develop pass on an Apple M4 is equal to Metal without fast
math and FP contraction bit for bit (VK6 record).

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
  -> BasicRenderDevice::CancelRender -> VulkanBackend::Wait submits the commands that the
     failed encode recorded and waits (VK2 record: as in OpenCL, where each enqueue runs)
  -> the unpublished writes are discarded; the completed submission id does not advance
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

**Status.** Not run (user decision 2026-11-22, A5): the neural networks use ONNX Runtime. VK15,
VK16, and VK20 are deferred.

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

**Status.** Complete (2026-11-22) for the budget: the rule is in section 4.2. Device loss: a read
of an address with no memory on the pipeline device stalls the GPU until the Windows GPU timeout;
the restore procedure of section 9.5 has no run, because a forced reset needs administrator rights
on this computer and a reset during the user's model experiments stops them. Section 9.5 has the
record.

## 7. Phase summary

Each phase is one pull request into `main`. Each phase from VK3 adds the cross-backend comparison
test of its kernels (B9).

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| VK0 | Build option, dependencies, `VulkanContext`, enum values | root CMake, `src/vulkan/`, `pipeline_accelerator` | VX1, VX2 | 1300 to 1700 | complete (#1063) |
| VK1 | Slang build rule, shader module registry, compute pipeline cache, kernel check tool | `src/vulkan/`, `tools/gpu_kernel_check.cpp` | VK0 | 1300 to 1700 | complete (#1065) |
| VK2 | `VulkanBackend`, `VulkanCommandContext`, workspace instance | `edit/runtime/vulkan/` | VK1, VX3, VX5 | 1600 to 1900 | complete (#1067) |
| VK3 | Kernel dispatch, shared color math, geometry utilities, comparison test support | `edit/runtime/vulkan/`, `src/vulkan/shader/`, `color_encoding_math.h` | VK2 | 1300 to 1700 | complete (#1080) |
| VK4 | RAW operators: linearize, CFA clamp, reference space, RCD, X-Trans, pack | `decoders/processor/operators/gpu/` | VK3 | 1400 to 1800 | complete (#1196) |
| VK5 | RAW operators: highlight reconstruction, lens calibration, DNG warp | `decoders/processor/operators/gpu/`, `edit/runtime/lens/vulkan/` | VK4 | 1500 to 1900 | complete (#1203) |
| VK6 | Develop pass (UploadRaw) | `edit/runtime/vulkan/` | VK5 | 1400 to 1800 | complete (#1209) |
| VK7 | GeometryResample, CameraToAp1, raster input (UploadRgb), DisplayToAp1 | `edit/runtime/vulkan/`, shared color headers | VK6 | 1100 to 1500 | complete (#1211) |
| VK8 | Primary grade point kernels and LUT resources | `edit/runtime/vulkan/` | VK7 | 1400 to 1800 | complete (#1219) |
| VK9 | Neighborhood kernels (Sharpen, Clarity, Halation) | `edit/runtime/vulkan/` | VK8 | 800 to 1200 | complete (#1225) |
| VK10 | Local tone | `edit/runtime/vulkan/` | VK8 | 1300 to 1700 | complete (#1228) |
| VK11 | MaskEvaluate, MaskUnion, AI mask coverage | `edit/runtime/vulkan/` | VK8 | 1300 to 1700 | complete (#1230) |
| VK12 | DiffusionFilter and Drt | `edit/runtime/vulkan/` | VK8 | 1500 to 1900 | planned |
| VK13 | Film print | `edit/runtime/vulkan/` | VK12 | 1600 to 1950 | planned |
| VK14 | Film print scan and film grain | `edit/runtime/vulkan/` | VK13 | 900 to 1300 | planned |
| VK15 | Neural network layers | `src/vulkan/nn/` | VK2, VX4 | 1500 to 1900 | deferred (A5) |
| VK16 | DemosaicNet modules and the neural Develop branch | `decoders/processor/nn/` | VK15, VK6 | 1600 to 1950 | deferred (A5) |
| VK17 | Scope analyzer, host download, `PipelineExecutor`, preference `vulkan` | `edit/scope/`, `edit/pipeline/`, `ui/alcedo_cli/` | VK14, VK16 | 1400 to 1800 | planned |
| VK18 | Editor display on Qt Quick Vulkan | `ui/editor_rhi/`, `ui/alcedo_main/main.cpp` | VK17, VX2, a new resize design (R1) | 1500 to 1900 | stopped (VX2) |
| VK19 | Release build, packaging, experimental setting | CMake presets, `scripts/`, Settings QML | VK18 | 700 to 1000 | planned |
| VK20 | UtNet2 forward code for AI Denoise | AI Denoise modules | VK15, #1030, VX4 | 1500 to 1900 | deferred (A5) |
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

- [x] A build with the option `OFF` has no Vulkan symbol and no new dependency.
- [x] The listed tests pass on the RTX 3080 Laptop GPU and on Iris Xe.
- [x] The validation layer reports no error during the context tests.

**Expected diff.** 1300 to 1700 lines.

**Completion record.**

##### Phase VK0 completion record (2026-11-23)

**Status:** complete. Issue #1063. Branch `feature/vulkan-vk0-context` on the plan branch
`docs/vulkan-compute-backend-plan` (pull request #1037).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Packages | `find_package(Vulkan)`, volk, VulkanMemoryAllocator | `find_package(volk CONFIG REQUIRED)` and `find_package(VulkanMemoryAllocator CONFIG REQUIRED)` from vcpkg (classic mode: `vcpkg install volk vulkan-memory-allocator`, volk 1.4.341.0, VMA 3.3.0). The Vulkan headers are the vcpkg port `vulkan-headers` 1.4.328 that volk uses. No `find_package(Vulkan)`. | volk loads `vulkan-1.dll` at run time, so the build links no Vulkan loader and needs no SDK. VK1 adds `find_package(Vulkan)` for `slangc` and `spirv-val`. A missing package with the option `ON` stops the configure (`REQUIRED`). |
| Definition | `HAVE_VULKAN` | `add_compile_definitions(HAVE_VULKAN)` in the root, as `HAVE_OPENCL` | Same form as OpenCL. |
| Target form | `VulkanContext` | Static library, or a DLL with `WINDOWS_EXPORT_ALL_SYMBOLS` when the CUDA targets are DLLs (`ALCEDO_BUILD_CUDA_DLLS`) | The rule of `OpenClContext`: a static singleton in each DLL gives each module its own context. |
| Device functions | none | `VulkanContext::PipelineDeviceFunctions()` gives a `VolkDeviceTable`. VMA gets its functions from `vmaImportVulkanFunctionsFromVolk`. | The volk globals are data of the DLL and are not exported. A table per device also serves the display device of VK18. |
| Capability record | `VulkanCapabilities` | `VulkanDeviceCapabilities` (`vulkan_device_capabilities.hpp`): name, ids, type, API version, LUID, extensions, features, queue families, format features, limits, largest device-local heap. `ReadVulkanDeviceCapabilities` fills it. | One record serves the requirement check, the selection, and `Capabilities()`. The LUID is for the DXGI match of VK18. |
| Device selection | VX2 rule | `SelectVulkanDevice` (`vulkan_device_selection.hpp`), a pure function: name filter (case-insensitive), then requirements, then discrete, integrated, other. A filter that matches no device, or only devices without the requirements, throws; no other device is selected (B8). | Testable without a GPU. |
| Queue requirement | "a compute queue with `timestampValidBits` above 0" | The first family with graphics, compute, and timestamps (`FindPipelineQueueFamily`) | B5: the pipeline device and the display device use queue 0 of one graphics family. |
| Optional extension | none | `VK_EXT_memory_priority` is enabled when the device has it (section 4.2 rule of VK2). `shaderFloat16` is read, not enabled. | VK2 sets the priority of each allocation. |
| Create twice | "one context for each process" | `Create` throws `std::logic_error` when a context exists. `InstanceOrCreate` checks and creates under one lock; `ResolveAcceleratorBackend(Vulkan)` uses it with `VulkanInitializationOptions::FromEnvironment()`. `Destroy` ends the context. | A resolve from two executors must not create two contexts. |
| Process exit | none | The context slot is never destroyed by a static destructor; only `Destroy` releases the context. | A static destructor in the DLL would call Vulkan and unload the loader under the Windows loader lock. |
| Validation | "enable the validation layer when it is installed" | `enable_validation_layer` (on in a debug build through `FromEnvironment`). The messenger is also chained into `vkCreateInstance`. `VulkanContext::ValidationErrorCount()` counts ERROR messages of type VALIDATION. Loader messages (type GENERAL) go to the `alcedo.gpu` log as warnings and are not counted. | This computer has two loader errors that are not API use: the registered layer file of Rockstar Social Club is missing, and the EOS overlay layer is registered twice. |
| `GpuImageWrapper` | VK2 decides about a `VulkanImage` member | `ResolveGpuBackend` throws for `GpuBackendKind::Vulkan` before the wrapper changes; each switch has a `Vulkan` case with the `None` cases. | The wrapper has no Vulkan image; the DAG path uses textures. |
| Preference text | none | `AcceleratorBackendPreferenceToString(Vulkan)` is `vulkan`; the Settings key and label of `project_module.cpp` are `vulkan` and `Vulkan`. The key is not parsed and the option list does not have Vulkan. | The setting value comes with VK17 (section 2.1). |
| Test name | `VulkanContextTest.SingleQueueDeviceGivesOneQueueForPipelineAndQt` | `VulkanContextTest.SingleQueueDeviceGivesThePipelineQueueZeroOfItsGraphicsFamily` | B5 changed to two devices: Qt gets its own device and queue in VK18, so no queue is shared. |

**Primary success call chain:**

```text
ResolveAcceleratorBackend(AcceleratorBackendPreference::Vulkan)
  -> PrepareExplicitVulkanRuntime
  -> VulkanContext::InstanceOrCreate(VulkanInitializationOptions::FromEnvironment())
       (lock) -> CreateUnlocked
  -> CreateInstance: volkInitialize -> loader version >= 1.3 -> vkCreateInstance
       (+ VK_LAYER_KHRONOS_validation, VK_EXT_debug_utils) -> volkLoadInstanceOnly
  -> SelectPhysicalDevice: vkEnumeratePhysicalDevices -> ReadVulkanDeviceCapabilities (each)
       -> SelectVulkanDevice -> CheckVulkanDeviceRequirements
  -> CreatePipelineDevice: FindPipelineQueueFamily -> vkCreateDevice (1 queue, features)
       -> volkLoadDeviceTable -> vkGetDeviceQueue(family, 0)
  -> CreatePipelineAllocator: vmaImportVulkanFunctionsFromVolk -> vmaCreateAllocator
  -> GpuBackendKind::Vulkan; VulkanContext::Capabilities() / PipelineQueue() / PipelineAllocator()
```

**Primary failure call chain:**

```text
SelectVulkanDevice: the filter accepts only devices without a requirement
  -> std::runtime_error "Vulkan: no device has the requirements of the Vulkan backend.
     <device> does not have: VK_KHR_push_descriptor."
  -> CreateUnlocked: the unique_ptr destroys the partial context (instance, messenger, loader)
  -> InstanceOrCreate / Create: the slot stays empty
  -> ResolveAcceleratorBackend(Vulkan) passes the exception to the caller
  -> a later Create runs again (VulkanContextTest.FilterThatMatchesNoDeviceThrowsAndLeavesNoContext)
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `VulkanRequirementsTest.DeviceWithoutPushDescriptorReportsTheExtensionName` | `VulkanRequirementsTest` | PASS |
| `VulkanRequirementsTest.Vulkan12DeviceReportsTheApiVersion` | `VulkanRequirementsTest` | PASS |
| 7 more requirement tests: all requirements pass; memory budget and scalar layout; linear filtering; absent format record; no timestamps; compute-only family; one-queue family | `VulkanRequirementsTest` | PASS |
| 7 `VulkanDeviceSelectionTest` tests: discrete first, integrated before CPU, filter with any letter case, no match names the devices, a matching device without a requirement is not replaced, a failing device is skipped by the default rule, no device | `VulkanRequirementsTest` | PASS |
| `VulkanContextTest.CreateOnLocalGpuReportsVulkan13AndAComputeQueue` | `VulkanContextTest` | PASS (RTX 3080 Laptop GPU and Iris Xe) |
| `VulkanContextTest.DeviceNameFilterSelectsTheMatchingDevice` | `VulkanContextTest` | PASS (selects Iris Xe in the default run, RTX 3080 in the Iris Xe run) |
| `VulkanContextTest.SingleQueueDeviceGivesThePipelineQueueZeroOfItsGraphicsFamily` | `VulkanContextTest` | PASS (Iris Xe, family 0) |
| `VulkanContextTest.FilterThatMatchesNoDeviceThrowsAndLeavesNoContext`, `SecondCreateThrowsAndKeepsTheFirstContext` | `VulkanContextTest` | PASS |
| `VulkanContextTest.InvalidApiUseIsCountedAsAValidationError` (a wrong `sType` gives one counted error, so the zero count of the other tests is a real result) | `VulkanContextTest` | PASS |
| `PipelineAcceleratorTest.AutoOnWindowsDoesNotResolveToVulkan` | `PipelineAcceleratorTest` | PASS (option `OFF` and `ON`) |
| `PipelineAcceleratorTest.VulkanPreferenceThrowsWhenTheBuildHasNoVulkan` | `PipelineAcceleratorTest` | PASS with `OFF` (skipped with `ON`) |
| `PipelineAcceleratorTest.VulkanPreferenceResolvesToVulkanAndCreatesTheContext` | `PipelineAcceleratorTest` | PASS with `ON` (skipped with `OFF`) |
| Validation errors during the context tests | `VulkanContextTest` fixture (`TearDown`) | 0 on both devices, except the one error that the test above makes |
| Option `OFF`: no Vulkan symbol, no new dependency | `AcceleratorBackend.lib`, `Image.lib`, `PipelineAcceleratorTest.exe`; `CMakeCache.txt`; `build.ninja` | No `vkCreateInstance`, `volkInitialize`, `vmaCreateAllocator`, `VulkanContext`, or `vulkan-1` string; no volk or VMA cache entry; no `VulkanContext` target |

Commands (worktree `build/debug`, one configure with the option `OFF`, then `ON`):

```text
cmd /c scripts\msvc_env.cmd --preset win_debug <vcpkg, Qt, CUDA arguments of the main build>
cmd /c scripts\msvc_env.cmd --build build\debug --target PipelineAcceleratorTest
ctest --test-dir build/debug -R "PipelineAcceleratorTest" -j 1 --output-on-failure      4/4 (1 skipped)
cmd /c scripts\msvc_env.cmd -S . -B build\debug -DALCEDO_ENABLE_VULKAN=ON
cmd /c scripts\msvc_env.cmd --build build\debug --target VulkanRequirementsTest VulkanContextTest PipelineAcceleratorTest
ctest --test-dir build/debug -R "Vulkan(Context|Requirements)Test|PipelineAcceleratorTest" -j 1 --output-on-failure
    default device: 26/26 (1 skipped); ALCEDO_VULKAN_DEVICE=Iris: 26/26 (1 skipped)
VulkanContextTest.exe (all tests in one process): 6/6 with each device
```

The full test suite was not run (repository rule). The application and the CUDA, OpenCL, and Metal
tests were not built in the worktree; the shared files have only added `Vulkan` cases.

**Checklist / exit condition:** all three exit criteria are met.

**LOC note:** 1989 added lines in 26 files (estimate 1300 to 1700): 1264 lines of new production
code, headers, and `src/vulkan/CMakeLists.txt`, 634 lines of tests and their CMake file, 91 lines
in shared files. The largest file is
`vulkan_context.cpp` (364 lines).

**Remaining gaps:** none for VK0. The display device (B5) is VK18. `FromEnvironment` reads
`ALCEDO_VULKAN_DEVICE`; no application path calls `ResolveAcceleratorBackend(Vulkan)` before VK17.

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

- [x] A Slang error stops the build with the file and the line.
- [x] The listed tests pass on both local devices.
- [x] `gpu_kernel_check --vulkan` exits with 0 for the test kernel.

**Expected diff.** 1300 to 1700 lines.

**Completion record.**

##### Phase VK1 completion record (2026-11-23)

**Status:** complete. Issue #1065. Branch `feature/vulkan-vk1-kernels` on the VK0 branch
`feature/vulkan-vk0-context` (pull request #1064).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Build function | `alcedo_add_slang_kernels(target, sources)` in `src/vulkan/CMakeLists.txt` | `alcedo_add_slang_kernels(<target> GROUP <group> SOURCE <file> KERNELS <kernel>... [INCLUDE_DIRS] [OUTPUT_DIR])` in `src/cmake/AlcedoSlangKernels.cmake`. `<target>` collects the groups (`AlcedoVulkanKernels`); each group is the custom target `<target>_<group>`. | Kernel groups of later phases are in `edit/` and use the same function. The build rule test includes the same file. The kernel list gives the `-entry` arguments, so a listed kernel that the source does not have stops the build. |
| Tools | VK0 record: "VK1 adds `find_package(Vulkan)` for `slangc` and `spirv-val`" | `alcedo_find_slang_tools()`: `find_program` for `slangc` and `spirv-val` with the hint `$ENV{VULKAN_SDK}/Bin`; the configure stops when one is absent and writes the `slangc` version. No `find_package(Vulkan)`. | `FindVulkan` has no `slangc` component, and the build links no Vulkan loader (VK0). |
| Build steps | `slangc`, then `spirv-val` | `slangc` writes the module and a depfile into `<binary dir>/slang_kernels/<group>/`; `spirv-val` checks that module; a script step (`write_slang_kernel_inventory.cmake`) then copies it to the kernel folder and writes the inventory. CMake converts the `slangc` depfile for Ninja. | A module that `spirv-val` rejects never reaches the kernel folder. A change of an included header builds the module again (`HeaderChangeBuildsTheModuleAgain`). |
| Inventory | "an asset inventory: kernel name, group file, checksum" | One file `<group>.kernels` for each group, one line `<kernel>\t<group>.spv\t<SHA-256>` for each kernel. `VulkanKernelInventory::Read` reads every `.kernels` file of the kernel folder and rejects a malformed line (file and line number) and a kernel name in two files (both files). | No CMake step must collect the groups of all folders. The uniqueness rule of the kernel name is checked where the names meet. |
| Kernel folders | Packaged path first, then the build tree path | Build folder `<build>/shaders/vulkan` (`ALCEDO_VULKAN_KERNEL_BUILD_ROOT`, a compile definition of `VulkanContext`). Packaged folder `<executable folder>/vulkan`; it is selected only when it has `runtime-assets.sha256`, and then it is the only source (`ResolveVulkanKernelFolder`, as `ResolveOpenClSourcePath`). | Test executables run from `<Target>_runtime/` and the build tool from `alcedo_studio/src`; both have no packaged folder and read the build folder. The install rule of the `.spv` files stays in VK19 (section 7). |
| Module load | Read `.spv` and `vkCreateShaderModule` | The registry reads the inventory at the first request, not at `VulkanContext::Create`. At the first request of a group it checks the SHA-256 of the file against the inventory and reads the `OpEntryPoint` names (`ReadSpirvEntryPointNames`); the requested kernel must be one of them. One `VkShaderModule` for each group. | A context needs no kernels (the VK0 tests do not build them). A stale module or an inventory that names a kernel the module does not have fails with the path, before `vkCreateComputePipelines` gets a wrong entry point name. |
| Pipeline key | Kernel name, binding list, specialization values | Key = kernel name and the specialization values sorted by constant id. The binding list and the push constant size are properties of the kernel: another value for a known kernel throws `std::logic_error`. A binding of a buffer type, a binding number or constant id used two times, and a push constant size that is not a multiple of 4 or above `maxPushConstantsSize` throw `std::invalid_argument` before a Vulkan call. | Section 4.2: one set of images, buffers as device addresses. A wrong layout is an error of the caller, not a second pipeline. |
| Pipeline form | none | `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT` on each pipeline. The descriptor set layout has `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`; a kernel without bindings has no set layout. | Section 4.2: an export dispatch is divided into row bands with `vkCmdDispatchBase`. |
| Disk store | `StoreToDisk()` and `LoadFromDisk()` | The constructor reads the file (a `VkPipelineCache` gets its data at creation) and `StoredDataStatus()` reports `NoFile`, `Loaded`, `DifferentDriver`, or `Damaged`. `StoreToDisk()` writes it; `VulkanContext::Destroy` calls it (a failure is a log warning). File: a header of the application (magic, format version, data size, SHA-256 of the data), then the data of `vkGetPipelineCacheData`. The data is given to the driver only when the file is complete and its `VkPipelineCacheHeaderVersionOne` has the vendor, the device, and the `pipelineCacheUUID` of the device. The write goes to a temporary file and a rename. | Some drivers do not check the data that they get. `VulkanDeviceCapabilities` gets `pipeline_cache_uuid` for the comparison. |
| Cache file path | "the application cache folder" | `VulkanInitializationOptions::pipeline_cache_file`. `FromEnvironment` leaves it empty, so no file is read or written. The application path that creates the context (VK17 `PipelineExecutor`, VK18 `main`) sets it to a file in `QStandardPaths::AppLocalDataLocation`. | `VulkanContext` has no Qt dependency. No application path creates the context before VK17. |
| Counters | Model `opencl_api_counters.cpp` | `VulkanApiCounters` with `ReadVulkanApiCounters` and `VulkanApiCounterDelta`. Always on (relaxed atomic increments), no enable flag. VK1 notes module loads, pipeline creations, and cache hits; the dispatch, transfer, submit, and wait counters are for the backend of VK2. | The counted calls cost much more than an increment, and the tests of section 4.2 need the counts in every build. |
| Test kernel | One kernel `test_fill.slang` | Group `test_fill` with two kernels: `test_fill` (a buffer through its device address, specialization constant 0 adds a value) and `test_fill_image` (an RGBA32F storage image at binding 0). Names, binding, and push constant structs are in `vulkan_test_fill_kernels.hpp`. | The image kernel proves the push descriptor path that each image pass of VK3 and later uses; the specialization constant proves the second cache key. |
| Kernel list of the check tool | "loads each packaged kernel and creates its pipeline" | `VulkanKernelCatalog()` (`vulkan_kernel_catalog.hpp`) has the descriptor of each kernel. `alcedo-gpu-check --vulkan` requires that the inventory and the catalog have the same kernels, creates each pipeline, and requires no validation error. It checks `runtime-assets.sha256` when the packaged folder has it. | A pipeline needs the layout of its kernel, and the layout comes from the group header. A new kernel group adds its descriptors to the catalog. |

**Primary success call chain:**

```text
build: alcedo_add_slang_kernels(AlcedoVulkanKernels GROUP test_fill ...)
  -> slangc (section 4.3 arguments, -entry test_fill -entry test_fill_image, -depfile)
  -> spirv-val --target-env vulkan1.3 --scalar-block-layout
  -> write_slang_kernel_inventory.cmake -> shaders/vulkan/test_fill.spv, test_fill.kernels
run: VulkanContext::Create -> CreateKernelObjects
       -> VulkanShaderModuleRegistry(device, VulkanKernelFolderCandidatesOfThisProcess())
       -> VulkanComputePipelineCache(device, registry, pipeline_cache_file)
            -> ReadStoredPipelineCacheData -> vkCreatePipelineCache
  -> VulkanComputePipelineCache::Pipeline(FillBufferDescriptor())
       -> check bindings, push constant size, specialization -> key not found
       -> VulkanShaderModuleRegistry::Module("test_fill")
            -> ResolveVulkanKernelFolder -> VulkanKernelInventory::Read -> Find
            -> read test_fill.spv -> SHA-256 = inventory -> ReadSpirvEntryPointNames
            -> vkCreateShaderModule (NoteVulkanModuleLoad)
       -> vkCreateDescriptorSetLayout (push descriptor, when bindings) -> vkCreatePipelineLayout
       -> vkCreateComputePipelines (DISPATCH_BASE, specialization) (NoteVulkanPipelineCreation)
  -> second Pipeline(...) -> same entry (NoteVulkanPipelineCacheHit)
  -> test: bind, push constants, dispatch, submit, read back the written value
VulkanContext::Destroy -> VulkanComputePipelineCache::StoreToDisk -> WriteStoredPipelineCacheData
```

**Primary failure call chain:**

```text
Pipeline({"absent_kernel", ...})
  -> VulkanShaderModuleRegistry::Module -> VulkanKernelInventory::Find
  -> std::runtime_error "Vulkan: the kernel absent_kernel is not in the kernel inventory.
     Kernel folder: <folder>. Searched folders: <packaged folder>; <build folder>."
  -> no module, no layout, no pipeline, no cache entry; a later request runs again
build: Slang error in <file>.slang
  -> slangc prints "<file>.slang(<line>): error ..." and exits with an error -> the build stops
  -> spirv-val and the inventory step do not run; the kernel folder keeps no new module
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `VulkanShaderModuleRegistryTest.PackagedPathIsUsedBeforeTheBuildTreePath` | `VulkanShaderModuleRegistryTest` | PASS (RTX 3080 Laptop GPU and Iris Xe) |
| `VulkanShaderModuleRegistryTest.AbsentKernelReportsItsNameAndTheSearchedPaths` | `VulkanShaderModuleRegistryTest` | PASS (both) |
| `VulkanComputePipelineCacheTest.SecondRequestForOneKernelCreatesNoPipeline` | `VulkanComputePipelineCacheTest` | PASS (both): 1 creation and 1 module load, then 0 and 0 with 1 cache hit |
| `VulkanComputePipelineCacheTest.FillKernelWritesTheRequestedValueToEachElement` | `VulkanComputePipelineCacheTest` | PASS (both): 1000 elements, not a multiple of the workgroup size |
| `VulkanComputePipelineCacheTest.StoredCacheDataFromADifferentDriverIsReplaced` | `VulkanComputePipelineCacheTest` | PASS (both): `DifferentDriver`, then the stored file has the cache UUID of the device, and the next context reports `Loaded` |
| 4 more registry tests: one module for a group, a module that differs from its inventory checksum, an inventory kernel without an entry point, and the module counter of each failure | `VulkanShaderModuleRegistryTest` | PASS (both) |
| 6 `VulkanKernelInventoryTest` tests: the build folder has the `test_fill` inventory, packaged folder with and without the asset list, malformed line (file and line number), a kernel name in two inventories, the entry points of the module | `VulkanShaderModuleRegistryTest` | PASS |
| 7 more cache tests: a specialization value gives a second pipeline that adds the value, the image kernel writes each texel of a 37 x 21 RGBA32F image through a push descriptor, a buffer binding is rejected with no Vulkan object, another push constant size for a known kernel is rejected, an absent kernel leaves no pipeline, each catalog kernel has a pipeline and the catalog equals the inventory, a damaged cache file is reported and not used; `VulkanPipelineCacheStoreTest.AbsentFileGivesNoFileAndNoData` | `VulkanComputePipelineCacheTest` | PASS (both) |
| A Slang error stops the build with the file and the line | `VulkanSlangBuildRuleTest.SlangErrorStopsTheBuildWithTheFileAndTheLine` (ctest, `cmake -P`, no GPU) | PASS: the output has `kernel_with_undefined_name.slang(7): error`, and no module or inventory is written |
| The build rule writes the module and the inventory; a header change builds again | `VulkanSlangBuildRuleTest.ValidGroupWritesTheModuleAndOneInventoryLineForEachKernel`, `HeaderChangeBuildsTheModuleAgain` | PASS |
| Validation errors | `TearDown` of each GPU test | 0 on both devices |
| `alcedo-gpu-check --vulkan` exits with 0 for the test kernel | `alcedo_gpu_check` (build folder) | PASS on both devices: `gpu.kernels.ok=vulkan kernels=2`, exit 0 |
| VK0 tests and the caller suite | `VulkanContextTest`, `VulkanRequirementsTest`, `PipelineAcceleratorTest` | PASS (4/4 with 1 skipped: the test for a build without Vulkan) |

Commands (worktree `build/debug`, `ALCEDO_ENABLE_VULKAN=ON`):

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target VulkanShaderModuleRegistryTest VulkanComputePipelineCacheTest VulkanContextTest VulkanRequirementsTest alcedo_gpu_check
ctest --test-dir build/debug -R "Vulkan(ShaderModuleRegistry|ComputePipelineCache|Context|Requirements|SlangBuildRule)Test" -j 1 --output-on-failure
    default device (RTX 3080 Laptop GPU): 47/47
ALCEDO_VULKAN_DEVICE=Iris ctest ... -R "Vulkan(ShaderModuleRegistry|ComputePipelineCache|Context|Requirements)Test": 44/44
build/debug/alcedo_studio/src/alcedo-gpu-check.exe --vulkan        (default device and Iris): exit 0
cmd /c scripts\msvc_env.cmd --build build\debug --target PipelineAcceleratorTest
ctest --test-dir build/debug -R "PipelineAcceleratorTest" -j 1: 4/4 (1 skipped)
```

The full test suite was not run (repository rule). A configure with the option `OFF` was not run
again: the new targets and the `--vulkan` code are inside `ALCEDO_VULKAN_ENABLED` and `HAVE_VULKAN`,
and the one shared change outside them is the condition of `alcedo_gpu_check` in
`src/tools/CMakeLists.txt`, which adds `ALCEDO_VULKAN_ENABLED`.

**Checklist / exit condition:** all three exit criteria are met.

**LOC note:** 2812 added lines in 32 files (estimate 1300 to 1700): 1759 lines of production code,
headers, CMake, and the kernel, and 1053 lines of tests. The estimate did not include the GPU test
support (`vulkan_test_device.hpp`, 271 lines: buffers, a storage image, one submission, barriers),
which the tests need until the backend of VK2 exists, and the build rule test project (172 lines).
The largest file is `vulkan_compute_pipeline_cache_test.cpp` (334 lines).

**Remaining gaps:** none for VK1. The install of the kernel folder and its asset list is VK19
(section 7). The application path that gives `pipeline_cache_file` comes with the first
application path that creates the context (VK17).

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
- The memory limit is the minimum of the application limit and the `VK_EXT_memory_budget` value
  minus 512 MiB (section 4.2, VX5). The pool reads the budget before each new allocation and sets
  the memory priority of each allocation (section 4.2).
- A stable render allocates nothing.
- `VulkanCommandContext` divides an export render into submissions by the rule of section 4.2
  (8 ms budget, one in flight, bands of rows). Moved to VK17 (completion record below). It keeps the time per row of each kernel, size, and
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
  -> no Vulkan object is created; TexturePool keeps its leases unchanged
  -> CancelRender -> Wait runs the recorded commands -> the completed submission id does not
     advance (completion record: the context does not drop the commands)
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

- [x] `static_assert(RenderBackend<VulkanBackend>)` compiles.
- [x] The listed tests pass on both local devices with no validation error.
- [x] The CUDA, OpenCL, and Metal workspace tests are unchanged.

**Expected diff.** 1600 to 1900 lines.

##### Phase VK2 completion record (2026-11-23)

**Status:** complete. Issue #1067. Branch `feature/vulkan-vk2-backend` on the VK1 branch
`feature/vulkan-vk1-kernels` (pull request #1066).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Files and target | `vulkan_backend.hpp/.cpp`, target `EditRuntimeVulkan` | `EditRuntimeVulkan` (static, `PUBLIC_DEPS EditRuntime VulkanContext`) with three owners: `VulkanBackend` (resources, transfers, memory limit, resource lifetime, submission ids), `VulkanCommandContext` (`vulkan_command_context.hpp/.cpp`: command pool, command buffers, fences, image layout record, barrier state), `VulkanStagingBuffers` (`vulkan_staging_buffers.hpp/.cpp`: host-visible staging chunks). `vulkan_render_device.hpp` has the `static_assert` and the aliases `VulkanRenderDevice` and `VulkanRenderWorkspace`. | One reason to change for each file. A pass encoder of VK3 and later includes the render device header only. |
| Image layout | "The command context owns the image layout of each texture during a render. A pass states the access that it needs." | Every image is in `VK_IMAGE_LAYOUT_GENERAL` after its first use. The first use in a recording records `UNDEFINED -> GENERAL` once (`VulkanBackend::PrepareImage`). The context keeps these resource ids; the backend adds them to its record of initialized images only when the batch is submitted, and `Texture2D::Reset` removes the id. A barrier between commands is then a global memory barrier: a new recording starts with one (it covers the earlier submissions of the queue), and a command after a write starts with one (`VulkanCommandContext::NoteWrite`). | Storage, sampled, and transfer use are all valid in `GENERAL`, so no pass needs a layout of its own. A layout that a dropped recording recorded never reaches the record of the backend. Finer barriers (by stage and access) can replace the global barrier in a pass encoder when a measurement shows a cost. |
| Failed encode | Failure chain: "`VulkanCommandContext` drops the command buffer" | `Wait` submits a recording that is still open and waits for it. `BasicRenderDevice::CancelRender` calls `Wait`, so the commands that a failed encode recorded run; the workspace discards the unpublished writes, and `CompletedSubmission` does not advance (no `Submit` set a render submission in flight). The context drops a recording only when `vkEndCommandBuffer` or the submission fails, and at its destruction. | The OpenCL backend runs each enqueue at once, and the shared templates expect that: `ParameterArena::UploadDirty` marks a slot as applied when the upload call returns, also outside `BeginRender`. A dropped command buffer would lose such an upload while the arena records it as applied. Section 4.4 and the failure chain above are changed. |
| Downloads | "download" in the success chain | `DownloadTexture2D` and `DownloadBufferRange` record the copy and a barrier to host reads, submit the recording, wait for the fences of the context, copy the bytes from the staging chunk, and only then let the staging memory return for reuse. | The host needs the bytes when the call returns (the OpenCL read also blocks). A staging chunk above 64 MiB is destroyed when it returns, so the copy comes first. |
| Staging | "staging buffers from `TransientBufferArena`" | `VulkanStagingBuffers` of the command context: one for uploads (`AUTO_PREFER_HOST`, sequential write) and one for downloads (random access, cached). Chunks of at least 4 MiB with a bump offset; a chunk belongs to the recording, then to its batch, then returns to a free list when the batch is complete. A chunk above 64 MiB is destroyed at that point. The bytes of an upload are copied into the chunk when the upload is recorded. | `TransientBufferArena` holds device-local memory without host access. A stable render reuses the chunks (`StagingBufferCreateCount` 0 in the second render). The caller can reuse its array at once (`HostUploadKeepsItsBytesAfterTheCallerReusesTheStorage`). Staging memory is system memory and is not in the device-local budget. |
| Resource lifetime | "A resource that an unfinished submission uses stays alive until the fence signals." | `Reset` gives the objects back to the backend with the last batch serial that can use them: the last submitted batch, or the next one when a recording is open. The backend destroys them at once when that batch is complete, else after the wait that completes it. One recording at a time can be open for a backend (a second command context gets `std::logic_error`). | Batch serials are given in submission order on one queue, so the fence of a batch covers every earlier batch. Memory that nothing uses is freed at once, so a Develop rewrite gets the memory of the textures that it releases. |
| Pipeline queue | Section 4.2: "Each queue has one user thread" | `VulkanContext::SubmitToPipelineQueue` holds one lock for `vkQueueSubmit2`. Fence waits are outside the lock. Section 4.2 is changed. | The editor, thumbnail, and export render devices run on threads of their own and share the one pipeline queue. A `VkQueue` needs external synchronization. The race of VX2 is with Qt on the display device, which has its own queue (B5). |
| Memory limit | Section 4.2 | Before each allocation `CheckMemoryLimit` reads the budget of the largest device-local heap (`vmaSetCurrentFrameIndex` makes VMA read `VK_EXT_memory_budget` again, then `vmaGetHeapBudgets`) and rejects an allocation above the budget minus 512 MiB, or above the application limit (`SetMemoryLimitBytes`), before the size limits of the device. The message has the size, the usage, the limit, and the budget; `gpu.alloc.failed` is written first. | VX5: the budget changes with the use of other processes, and `vkAllocateMemory` does not fail at the budget. |
| Memory priority | 0.2 for cache entries, 0.8 for the images of the render | Every allocation of VK2 has 0.8 (`kWorkingSetPriority`). | The priority is fixed at allocation. `GraphImageCache` and `NodeResultCache` get their images from the `TexturePool`, which allocates them for the render; an image becomes a cache entry later. The LUT cache (VK8) and the mask coverage cache (VK11) allocate their own entries and set 0.2 there. |
| Export split | "`VulkanCommandContext` divides an export render into submissions (8 ms, bands of rows) and keeps the time per row from timestamps" | Moved to VK17. VK2 submits one batch for each render. | VK2 has no kernel and no export render. The time per row comes from timestamps of real kernels, and the export path of Vulkan starts in VK17 (`PipelineExecutor`, `FramePresenter<VulkanBackend>::Download`). The VK17 entry of the scope table is changed. |
| Buffers | none | `Buffer::DevicePointer()` is the device address (`vkGetBufferDeviceAddress`). `MaxSlabBytes` is the Vulkan 1.3 `maxBufferSize` (new `VulkanDeviceCapabilities::max_buffer_bytes`); `MaxTransientBytes` is three quarters of the device-local heap. | Kernels read buffers through addresses (section 4.2), so the transient arena gives addresses directly. |
| Formats | Section 4.2: RGBA32F, R32F, R8 | The five `TextureFormat` values: RGBA8 and R16 unsigned integer are optional (`OptionalImageFormats()`, read into `VulkanDeviceCapabilities::formats`). `CreateTexture2D` requires storage and transfer support of the format and fails with the format name without it. Each texture has a `VkImageView`. | The workspace templates can ask for each format. The view is for the push descriptors of the pass encoders. |
| AI Mask edge | none | `RecordAiMaskEdge` throws `std::logic_error` that names VK11. | `MaskCoverageTextureCache` requires the function at compile time; the kernels come with VK11. |
| Counters | "the counters of VK1" | `VulkanApiCounters` gets `buffer_creations`, `image_creations`, and `destroyed_resources` (staging included). The backend has the counters of the OpenCL backend that the templates and tests read; the context has `CommandBufferCreateCount`, `FenceCreateCount`, and `StagingBufferCreateCount`. | The test of a stable render checks each Vulkan object that a render can create. |

**Primary success call chain:**

```text
BasicRenderDevice<VulkanBackend>::BeginRender -> BasicRenderWorkspace::BeginRender
  -> VulkanBackend::Wait (nothing open) -> SetSubmissionId(NextSubmissionId)
  -> ParameterArena::UploadDirty -> VulkanBackend::UploadBufferRange
       -> VulkanCommandContext::BeginCommand (new recording: memory barrier; NoteRecordingOpened)
       -> VulkanStagingBuffers::Allocate -> memcpy -> vkCmdCopyBuffer -> NoteWrite
  -> TexturePool::Acquire -> VulkanBackend::CreateTexture2D (first use only)
       -> CheckMemoryLimit (VK_EXT_memory_budget) -> vmaCreateImage -> vkCreateImageView
  -> VulkanBackend::UploadTexture2D -> BeginCommand (barrier after the write) -> PrepareImage
       (UNDEFINED -> GENERAL once) -> vkCmdCopyBufferToImage
  -> BasicRenderWorkspace::EndRender -> VulkanBackend::Submit
       -> VulkanCommandContext::SubmitRecording(batch serial) -> vkEndCommandBuffer
       -> VulkanContext::SubmitToPipelineQueue (lock, vkQueueSubmit2, fence)
       -> initialized image ids to the backend; staging chunks to the batch
  -> BasicRenderDevice::WaitIdle -> VulkanBackend::Wait -> vkWaitForFences
       -> command buffers and fences free; staging recycled; retired resources destroyed
       -> CompletedSubmission = render submission
  -> VulkanBackend::DownloadTexture2D -> copy to download staging -> host read barrier
       -> submit -> wait -> vmaInvalidateAllocation -> memcpy -> staging recycled
```

**Primary failure call chain:**

```text
UploadTexture2D fails (injected failure, or a range error) during the encode
  -> BasicRenderDevice::CancelRender -> VulkanBackend::Wait
       -> the commands recorded before the failure are submitted and waited (they write only
          unpublished images and parameter slots)
  -> BasicRenderWorkspace::CancelRender discards the unpublished writes
  -> CompletedSubmission keeps the last successful render; the earlier published result stays
CreateTexture2D above the memory limit
  -> CheckMemoryLimit -> gpu.alloc.failed -> std::runtime_error (size, usage, limit, budget)
  -> no Vulkan object, no allocation count; the pool keeps its leases
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `UploadedRgba32fImageDownloadsWithEqualValues` (37 x 23, packed rows) | `GpuDagVulkanWorkspaceTest` | PASS (RTX 3080 Laptop GPU and Iris Xe) |
| `R8DirtyRectangleUploadChangesOnlyTheRectangle` (each texel inside and outside, 28 uploaded bytes) | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `SecondRenderOfOnePlanAllocatesNoImageAndNoBuffer` (parameter upload, pooled image upload, transient range): 0 buffers, images, allocations, frees, command buffers, fences, staging buffers; 1 submission | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `TextureInAnUnfinishedSubmissionIsNotReleased`: the pool keeps the image until the wait, then frees it at once | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `AllocationAboveTheBudgetReportsSizeAndBudget`: a buffer of the whole budget and a texture above the application limit fail with the size and the limits; no allocation | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `FailedEncodeDoesNotAdvanceTheCompletedSubmission`: the earlier result stays valid, the failed one is not published | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| 12 more: the templates instantiate and one empty render completes; R32F and R8 round trip; buffer ranges at an offset; image to buffer to image to image in one render (barriers); the upload keeps its bytes after the caller overwrites them; the parameter slot uploads once; the transient arena rewinds to the same address; a buffer above the max slab size; a released texture stays alive until its batch is complete; a failed parameter upload stays queued and the next upload writes it; a second command context cannot record while the first records; the AI Mask edge names VK11 | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| Validation errors | `TearDown` of each test | 0 on both devices |
| VK0 and VK1 suites after the changes of `VulkanContext`, the capabilities, and the counters | `VulkanContextTest`, `VulkanRequirementsTest`, `VulkanShaderModuleRegistryTest`, `VulkanComputePipelineCacheTest`, `VulkanSlangBuildRuleTest`, `PipelineAcceleratorTest`, `alcedo-gpu-check --vulkan` | PASS |

Commands (worktree `build/debug`, `ALCEDO_ENABLE_VULKAN=ON`):

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagVulkanWorkspaceTest VulkanRequirementsTest VulkanContextTest VulkanShaderModuleRegistryTest VulkanComputePipelineCacheTest PipelineAcceleratorTest alcedo_gpu_check
ctest --test-dir build/debug -R "Vulkan" -j 1                        default device (RTX 3080 Laptop GPU): 69/69
ALCEDO_VULKAN_DEVICE=Iris ctest --test-dir build/debug -R "GpuDagVulkanWorkspaceTest|VulkanContextTest|VulkanComputePipelineCacheTest" -j 1: 35/35
ctest --test-dir build/debug -R "PipelineAcceleratorTest" -j 1: pass (1 skipped: the test for a build without Vulkan)
build/debug/alcedo_studio/src/alcedo-gpu-check.exe --vulkan: gpu.kernels.ok=vulkan kernels=2, exit 0
```

The CUDA, OpenCL, and Metal workspace tests did not run: VK2 changes no shared template, no
OpenCL, CUDA, or Metal file, and no CMake target of those backends (the changed shared files are
`src/edit/CMakeLists.txt` and `tests/edit/CMakeLists.txt`, each with one block inside
`ALCEDO_VULKAN_ENABLED`). The full test suite was not run (repository rule).

**Checklist / exit condition:** all three exit criteria are met.

**LOC note:** about 2570 added lines (estimate 1600 to 1900): 1880 lines in the 7 new production
files, 585 lines of tests, 104 lines in existing files. The estimate did not include the staging
owner (243 lines). The largest file is `vulkan_backend.cpp` (765 lines); its parts (resources,
lifetime, transfers, submission) share the state of the backend, so it stays one file.

**Remaining gaps:** none for VK2. The export split (8 ms submissions, bands of rows, time per row)
is in VK17. The 0.2 memory priority of cache entries comes with the caches that allocate their own
entries (VK8, VK11).

### Phase VK3: kernel dispatch, shared color math, geometry utilities, comparison test support

**Objective and deliverables.** A pass encoder records a kernel of the catalog with one call. The
color encodings and OkLab of the OpenCL kernels compile as Slang and give the OpenCL values. The
crop and resize of the Develop pass run on Vulkan images. A test runs one input on Vulkan and on
OpenCL on the same GPU and compares the results with a stated tolerance; each later phase uses
this support for its comparison test (B9).

**Inputs and prerequisites.** VK2 (`VulkanBackend`, `VulkanCommandContext`). The VX1 record:
the `__SLANG__` branch of `color_encoding_math.h`, the `cbrt` form, the kernel rules of sections
4.2 and 4.3.

**Source audit of the VK3 entry (2026-11-23).** The scope entry of VK3 named `common.cl`,
`cst.cl`, `oklab.cl`, `prng.cl`, and the geometry utilities. The source on the VK2 branch shows:

| Item of the entry | Source | Users in OpenCL programs | VK3 decision |
| --- | --- | --- | --- |
| Color encodings | `color/color_encoding_math.h` | every program of `opencl_gpu_dag_programs.cpp` | VK3: the `__SLANG__` branch |
| OkLab | `oklab.cl` | `primary_grade.cl`, `mask.cl` | VK3: `oklab.slangh` |
| `common.cl` helpers without parameters (matrix, RGC curve, sigmoid, contrast on luma, luma) | `common.cl` | none: no kernel calls them | not ported |
| `common.cl` tone curves, `cst.cl` (ACES 2.0, OpenDRT, LMT, LUT sample), `drt_params.cl` | DRT program | `drt.cl` through `OpenClDrtParams` | VK12, with the host packing of the DRT parameters (`opencl_drt_params.cpp`) that their test needs |
| Counter PRNG | `opencl/opencl_shader/prng.cl` | none: no program lists `ALCEDO_OPENCL_PRNG_CL`. The film grain uses `FilmGrainHash3` of `film_grain_field.h`. | not ported; [#1081](https://github.com/zidage/AlcedoStudio/issues/1081). The grain hash comes with VK14. |
| Crop and bilinear resize | `geometry_utils.cl` (`opencl_geometry_crop_resize_linear`), `OpenCL::Geometry::ResizeLinear` and `CropResize` | `ResizeLinear` in `opencl_develop_pass.cpp` (after the lens calibration) | VK3 |
| Area resize | `geometry_utils.cl` (`opencl_geometry_crop_resize_area`) | none: `CropResize` with `Area` has test callers only (the `Area` default argument of the OpenCL function) | not ported (completion record: its loop breaks the workgroup limit) |
| Affine warp, rotate, DNG warp of `geometry_utils.cl` | `geometry_utils.cl` | tests only. The DNG warp of the graph is `warp_rectilinear_rgba32f` of `geometry_camera.cl` (VK7). | not ported |

So the PRNG test of the entry is replaced by the encoding and OkLab tests below.

**Modules, files, and APIs.**

- `inc/edit/runtime/vulkan/vulkan_kernel_dispatch.hpp`, `edit/runtime/vulkan/vulkan_kernel_dispatch.cpp`
  (target `EditRuntimeVulkan`): `RecordKernelDispatch(backend, command_context, kernel descriptor,
  image bindings, push constant bytes, workgroup count)`. It gets the pipeline from
  `VulkanComputePipelineCache`, prepares each image (`PrepareImage`), pushes the descriptors and the
  push constants, records `vkCmdDispatch`, and notes the write. `PushConstantBytes(value)` gives the
  bytes of a push constant struct.
- `inc/color/color_encoding_math.h`: a `__SLANG__` branch of the `CE_*` macros (VX1).
- `vulkan/shader/math.slangh`: `AlcedoCbrt` (section 4.3), and `AlcedoExp`, `AlcedoExp2`,
  `AlcedoLog`, `AlcedoLog2`, `AlcedoPow`: exact argument reduction and polynomials of plain
  arithmetic, about 1 ulp. The `__SLANG__` branch of `color_encoding_math.h` maps the `CE_*`
  macros to them (completion record).
- `edit/runtime/vulkan/shader/oklab.slangh`: `AcesccToLinearAp1`, `LinearAp1ToAcescc`,
  `LinearAp1ToOkLab`, `OkLabToLinearAp1`, with the coefficients of `oklab.cl`.
- `edit/runtime/vulkan/shader/color_math_check.slang` and `inc/vulkan/vulkan_color_math_check_kernels.hpp`:
  the kernel group `color_math_check`. One kernel evaluates every transfer function in one
  direction for a list of values (the kernel of `color_encoding_opencl_test.cpp`); one kernel
  evaluates one OkLab or ACEScc function for a list of colors. They are check kernels as
  `test_fill`: the comparison tests and `alcedo-gpu-check --vulkan` use them.
- `vulkan/shader/geometry_utils.slang` and `inc/vulkan/vulkan_geometry_kernels.hpp`: the kernel
  group `geometry_utils` with `geometry_crop_resize_linear`, the form of
  `opencl_geometry_crop_resize_linear` on RGBA32F storage images (binding 0 source, binding 1
  destination).
- `inc/edit/runtime/vulkan/vulkan_geometry.hpp`, `edit/runtime/vulkan/vulkan_geometry.cpp`:
  `vulkan::geometry::EncodeCropResize(backend, command_context, source, destination, crop)`, the
  form of `OpenCL::Geometry::ResizeLinear` and of `CropResize` with `Bilinear`. The size of the
  destination is the output size. The full source at its own size is a `CopyTexture2D`.
- `VulkanKernelCatalog()`: the descriptors of the two new groups.
- `tests/vulkan/vulkan_opencl_comparison.hpp`: initializes the OpenCL runtime on the device of the
  Vulkan context (preferred device = Vulkan device name) and fails when the two device names
  differ; compares two value lists by a stated rule and tolerance and reports the largest
  difference, its index, both values, and the count above the tolerance; builds an OpenCL program
  from product source files and a test kernel text.
- Tests: `tests/edit/runtime/vulkan_kernel_dispatch_test.cpp` (in `GpuDagVulkanWorkspaceTest`),
  `tests/edit/runtime/vulkan_color_math_test.cpp` (`VulkanColorMathTest`),
  `tests/edit/runtime/vulkan_geometry_test.cpp` (`VulkanGeometryTest`). The last two exist when
  Vulkan and OpenCL are both enabled.

**Data rules and invariants.**

- A pass encoder does not submit and does not wait (section 4.2). `RecordKernelDispatch` records
  into the open recording of the command context.
- The images of a dispatch equal the bindings of its descriptor (same numbers), and the push
  constant bytes equal its `push_constant_bytes`. A mismatch fails before a command is recorded.
- A dispatch with a workgroup count of zero on an axis records nothing.
- `EncodeCropResize` takes RGBA32F textures only (the images of the graph). The 1 and 3 channel
  forms of the OpenCL buffer function serve the `ImageBuffer` path, which Vulkan does not have.
- The bilinear kernel reads 4 texels for each destination texel at each scale, so its workgroup
  time does not grow with the scale (section 4.2).
- The comparison input has the value range of the input of the matching OpenCL test (VX1).

**Implementation steps.**

1. Add `RecordKernelDispatch` and its tests with the `test_fill_image` kernel.
2. Add the `__SLANG__` branch, `math.slangh`, `oklab.slangh`, the `color_math_check` group, and
   their catalog entries.
3. Add the comparison support and `VulkanColorMathTest`.
4. Add the `geometry_utils` group, `EncodeCropResize`, the catalog entries, and
   `VulkanGeometryTest`.
5. Run the tests on both devices with the validation layer.

**Primary success call chain.**

```text
vulkan::geometry::EncodeCropResize(backend, command_context, source, destination, crop)
  -> check formats, two textures, crop inside the source
  -> RecordKernelDispatch(geometry_kernels::CropResizeLinearDescriptor(),
                          {0: source, 1: destination}, push constants, 16 x 16 groups)
       -> VulkanComputePipelineCache::Pipeline (cache hit after the first use)
       -> VulkanBackend::BeginCommand (barrier after an earlier write) -> PrepareImage (each image)
       -> vkCmdBindPipeline -> vkCmdPushDescriptorSetKHR -> vkCmdPushConstants -> vkCmdDispatch
       -> VulkanCommandContext::NoteWrite, NoteVulkanDispatch
  -> caller: VulkanBackend::Submit or DownloadTexture2D (the barrier of the next command covers the
     write)
```

**Primary failure call chain.**

```text
EncodeCropResize with a crop outside the source, a non-RGBA32F texture, or one texture
  -> std::invalid_argument with the values -> no command, no pipeline request
RecordKernelDispatch with an image for a binding that the kernel does not have
  -> std::invalid_argument with the kernel name and the binding -> no command recorded
```

**Tests and evidence.**

- `GpuDagVulkanWorkspaceTest.DispatchWritesEachTexelAndCountsOneDispatch`
- `GpuDagVulkanWorkspaceTest.CopyAfterADispatchReadsTheWrittenTexels`
- `GpuDagVulkanWorkspaceTest.DispatchWithAnImageForAnotherBindingIsRejected`
- `GpuDagVulkanWorkspaceTest.DispatchWithAnotherPushConstantSizeIsRejected`
- `GpuDagVulkanWorkspaceTest.SecondDispatchOfOneKernelCreatesNoPipeline`
- `GpuDagVulkanWorkspaceTest.DispatchOfZeroWorkgroupsRecordsNoCommand`
- `VulkanColorMathTest.EveryTransferFunctionDecodesToTheOpenClValues` (1e-5 relative, the rule of
  `CurvesMatchOcioBuiltinsOn4096CodeValues`; completion record)
- `VulkanColorMathTest.EveryTransferFunctionEncodesToTheOpenClValues`
- `VulkanColorMathTest.OkLabFunctionsGiveTheOpenClValues` (the grade input range of VX1;
  `ScaledDifference` 1e-6, the tolerance of `ColorEncodingOpenCl`, and 4e-6 for
  `OkLabToLinearAp1`)
- `VulkanGeometryTest.CropAtItsOwnSizeCopiesTheRegionExactly` (tolerance 0)
- `VulkanGeometryTest.ResizeOfACropEqualsOpenCl` (1e-5, the tolerance of
  `OpenClGeometryUtilsTest`)
- `VulkanGeometryTest.ResizeOfTheFullSourceEqualsOpenCl` (1e-5)
- `VulkanGeometryTest.FullSourceAtItsOwnSizeIsACopyWithNoDispatch`
- `VulkanGeometryTest.InvalidRequestIsRejectedBeforeACommand`

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagVulkanWorkspaceTest VulkanColorMathTest VulkanGeometryTest alcedo_gpu_check
ctest --test-dir build/debug -R "GpuDagVulkanWorkspaceTest|VulkanColorMathTest|VulkanGeometryTest" -j 1 --output-on-failure
ALCEDO_VULKAN_DEVICE=Iris: the same ctest command
```

**Exit criteria.**

- [x] The listed tests pass on both local devices with no validation error.
- [x] `alcedo-gpu-check --vulkan` creates the pipeline of each catalog kernel.
- [x] The OpenCL, CUDA, and Metal results are unchanged: `ColorEncodingOpenClTest` and the host
      color encoding tests pass with the changed header.

**Expected diff.** 1300 to 1700 lines.

##### Phase VK3 completion record (2026-11-23)

**Status:** complete. Issue #1080. Branch `feature/vulkan-vk3-shared-kernels` on the VK2 branch
`feature/vulkan-vk2-backend` (pull request #1068).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Math functions of the Slang branch | VX1: `CE_POW pow`, `CE_EXP exp`, `CE_EXP2 exp2`, `CE_LOG log`, `CE_LOG2 log2` (GLSL.std.450) | `math.slangh` has `AlcedoExp`, `AlcedoExp2` (Cody and Waite reduction, 2^n from the exponent bits, Taylor series of expm1 to r^9), `AlcedoLog`, `AlcedoLog2` (mantissa and exponent from the bits, 2 atanh series to s^11), and `AlcedoPow` = `AlcedoExp(y AlcedoLog(x))`; a base that is not positive keeps the GLSL.std.450 result. The `__SLANG__` branch of `color_encoding_math.h` includes `math.slangh` and maps the `CE_*` macros to them. A kernel group that includes the header has `src/` in its include folders. | With the GLSL.std.450 functions the ST 2084 decode differs from OpenCL by 3.7e-5 (RTX 3080 Laptop GPU) and 5.2e-5 (Iris Xe), above the 1e-5 that the host curves have against OCIO, and the LogC4 decode by 1.5e-6 (about 12 ulp, `exp2` of an argument near 19). The Vulkan precision of `exp2` is 3 + 2\|x\| ulp and of `log2` an absolute 2^-21 in [0.5, 2]; the ST 2084 decode subtracts a constant from a power near 1, so one ulp of that power is about 4e-5 of the light near the peak. With the functions of `math.slangh` the ST 2084 decode differs by 2.4e-6 at most, and both devices give the same values. HLSL `ldexp` and `frexp` take a float exponent, so the scale by 2^n and the mantissa come from the bits. |
| Transfer function tolerance | `ScaledDifference` at most 1e-6 (the tolerance of `ColorEncodingOpenCl`) | 1e-5 relative to `max(0.01, \|OpenCL\|)`, the rule of `CurvesMatchOcioBuiltinsOn4096CodeValues` (`DifferenceRule::RelativeAboveOneHundredth`). The ACEScc and OkLab functions keep `ScaledDifference` 1e-6. | The 1e-6 of `ColorEncodingOpenCl` holds because OpenCL and the host round each intermediate value of a curve in the same way. A separate implementation of the functions differs by one ulp of an intermediate value, and the shared formula amplifies it: the ST 2084 encode raises a base near 1 to m2 = 78.84, so one ulp of the base is 78.84 x 6e-8 = 4.7e-6 (measured 4.8e-6); the LogC4 decode is exp2 of an argument near 19, so one ulp of the argument is 1.3e-6 (measured 1.5e-6). The repository rule for a separate implementation of a curve is the OCIO comparison. With the `ScaledDifference` rule, 37 of the 39 decode curves and 38 of the 39 encode curves are inside 1e-6 (outside: ST 2084 and LogC4 decode, ST 2084 encode). |
| `OkLabToLinearAp1` tolerance | `ScaledDifference` 1e-6 | 4e-6 | The function is arithmetic only, and the two compilers fuse other multiply-adds: about 2 ulp in l_, m_, s_, 3 times that after the cube, and the output matrix sums terms of about 3.35 times the result, a float32 limit of about 3.2e-6 of the result. Measured 1.6e-6 on both devices. The other three color functions are inside 1e-6. |
| Area resize | `geometry_crop_resize_area` with a scale limit of 64 | Not ported. `EncodeCropResize` has no algorithm argument: it is the bilinear crop and resize. | No product path calls the area kernel: in OpenCL, `CropResize` with `Area` has test callers only (the default argument of the function), and the Develop pass calls `ResizeLinear`. The loop of the area kernel grows with the scale and breaks the workgroup limit of section 4.2: at the limit of 64 (4096 x 4096 to 64 x 64, 16 workgroups, each texel reads 65 x 65 texels) one dispatch took 123 to 129 ms on Iris Xe and 4.2 to 5.6 ms on the RTX 3080 Laptop GPU, so one workgroup ran for about that time. A later phase with an area caller divides the footprint over dispatches. |
| Dispatch recording | `RecordKernelDispatch(backend, command_context, descriptor, image bindings, push constant bytes, workgroup count)` | As planned, in `vulkan_kernel_dispatch.hpp/.cpp`. The pipeline comes from `VulkanContext::Instance().ComputePipelines()`. A request with images that do not match the bindings (count, binding number, binding used two times, empty texture) or another push constant size throws before the pipeline request. At most 8 images for each kernel. `WorkgroupsCovering` and `PushConstantBytes` are helpers of the caller. The device functions come from the context (`PipelineDeviceFunctions`), not from the backend. | One owner of the recording of a dispatch for each pass encoder. A pass encoder with a kernel of more than 8 images changes the limit. |
| Check kernels | `color_math_check` group | Two kernels on buffers (device addresses in the push constants, no image), float arrays with `uint` index arithmetic (no struct pointer arithmetic, section 4.2). The group is in the catalog, as `test_fill`. | `alcedo-gpu-check --vulkan` requires that the inventory and the catalog have the same kernels. |
| Comparison support | `tests/vulkan/vulkan_opencl_comparison.hpp` | `PrepareOpenClOnTheVulkanDevice` sets the Vulkan device name as the preferred OpenCL device (`TryInitializeOpenClRuntime`, no warm-up of the product programs) and fails when the two device names differ. `ExpectEqualWithinTolerance` prints one line for each case with both device names and the largest difference. `OpenClTestProgram` builds product sources with a test kernel; the test kernels have the arguments `(input, output, count, mode)`. Three difference rules: absolute, `ScaledDifference`, relative above 0.01. | `ALCEDO_VULKAN_DEVICE` alone selects the GPU of both backends, so a run on Iris Xe compares Intel with Intel. |
| Shared math of the scope entry | `common.cl`, `cst.cl`, `prng.cl`, DRT parameters | Not in VK3: source audit above. `common.cl` tone curves, `cst.cl`, and `drt_params.cl` go to VK12; the grain hash to VK14. Issue #1081 for the PRNG sources without a caller. | No kernel of VK3 to VK11 calls them; their tests need the host packing of VK12. |

**Primary success call chain:**

```text
build: alcedo_add_slang_kernels(AlcedoVulkanKernels GROUP color_math_check ...
         INCLUDE_DIRS src/include src) -> slangc includes color_encoding_math.h (__SLANG__ branch
         -> vulkan/shader/math.slangh) and oklab.slangh -> spirv-val -> color_math_check.spv
run (VulkanGeometryTest):
  VulkanRenderDevice -> VulkanBackend::CreateTexture2D (source, destination) -> UploadTexture2D
  -> vulkan::geometry::EncodeCropResize(backend, command_context, source, destination, crop)
       -> RequireRgba32f (both), two textures, crop inside the source; full source at its own size
          -> CopyTexture2D
       -> RecordKernelDispatch(CropResizeLinearDescriptor(), {0: source, 1: destination},
                               CropResizePushConstants, WorkgroupsCovering(width, height, 16, 16))
            -> check images and push constant size
            -> VulkanComputePipelineCache::Pipeline (creation at the first use, then a cache hit)
            -> VulkanBackend::BeginCommand (memory barrier after the upload) -> PrepareImage (each)
            -> vkCmdBindPipeline -> vkCmdPushDescriptorSetKHR -> vkCmdPushConstants
            -> vkCmdDispatch -> VulkanCommandContext::NoteWrite -> NoteVulkanDispatch
  -> VulkanBackend::DownloadTexture2D (barrier after the dispatch, submit, wait, copy)
  -> ExpectEqualWithinTolerance(OpenCL::Geometry::CropResize result, Vulkan result, 1e-5)
```

**Primary failure call chain:**

```text
EncodeCropResize with an R32F texture, one texture for both, an empty crop, or a crop outside
the source
  -> std::invalid_argument with the values -> no command, no pipeline request, no recording
RecordKernelDispatch with an image for binding 3 of a kernel with binding 0, an empty texture, or
16 push constant bytes for a kernel of 24
  -> std::invalid_argument with the kernel name and the values -> no command, no recording
PrepareOpenClOnTheVulkanDevice with an OpenCL device of another name
  -> the reason names both devices -> ASSERT in SetUp fails the comparison test
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `DispatchWritesEachTexelAndCountsOneDispatch` (37 x 21, not a multiple of the group; 1 dispatch, 0 submissions before the download) | `GpuDagVulkanWorkspaceTest` | PASS (RTX 3080 Laptop GPU and Iris Xe) |
| `CopyAfterADispatchReadsTheWrittenTexels` (dispatch, copy, dispatch in one recording: the copy has the first value, the image the second) | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `DispatchWithAnImageForAnotherBindingIsRejected`, `DispatchWithAnotherPushConstantSizeIsRejected` (0 dispatches, 0 pipeline requests, no recording) | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `SecondDispatchOfOneKernelCreatesNoPipeline` (0 creations, 0 module loads, 1 cache hit), `DispatchOfZeroWorkgroupsRecordsNoCommand` | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| `EveryTransferFunctionDecodesToTheOpenClValues` (4096 code values, 39 curves) | `VulkanColorMathTest` | PASS (both). Largest difference by the rule 2.1e-6 (RTX 3080) and 2.4e-6 (Iris Xe), ST 2084; LogC4 1.5e-6; 4 more curves from 1.0e-6 to 1.5e-6 (values below 1, which this rule compares relatively above 0.01) |
| `EveryTransferFunctionEncodesToTheOpenClValues` (0 and 2^-14 to 2^6) | `VulkanColorMathTest` | PASS (both). Largest 4.8e-6, ST 2084 (both); ACEScc 3.0e-6 near its zero crossing (linear 2^-9.72, absolute 3e-8); 2 more curves at 1.0e-6 and 1.2e-6 |
| `OkLabFunctionsGiveTheOpenClValues` (4096 colors of the VX1 photo range) | `VulkanColorMathTest` | PASS (both). `AcesccToLinearAp1` 1.2e-7 and 1.6e-7, `LinearAp1ToAcescc` 6.0e-8, `LinearAp1ToOkLab` 5.4e-7 and 7.2e-7, `OkLabToLinearAp1` 1.6e-6 (both) |
| `CropAtItsOwnSizeCopiesTheRegionExactly` | `VulkanGeometryTest` | PASS (both): 0 against OpenCL and equal to the source region |
| `ResizeOfACropEqualsOpenCl`, `ResizeOfTheFullSourceEqualsOpenCl` (6 cases: 2x and fractional downsample of a crop, one axis upsampled, upsample, downsample, 640 x 427 to 160 x 107) | `VulkanGeometryTest` | PASS (both). Largest 1.8e-7 (RTX 3080), 1.2e-7 (Iris Xe) |
| `FullSourceAtItsOwnSizeIsACopyWithNoDispatch`, `InvalidRequestIsRejectedBeforeACommand` (5 requests) | `VulkanGeometryTest` | PASS (both) |
| Validation errors | `TearDown` of each GPU test | 0 on both devices |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | PASS on both devices: `gpu.kernels.ok=vulkan kernels=5`, exit 0 |
| VK0 to VK2 suites and the build rule after the catalog change | `ctest -R Vulkan` (83 tests) | PASS on both devices (1 skipped: the accelerator test for a build without Vulkan) |
| The changed header in the other backends | `ColorEncodingOpenClTest`, `ColorEncodingCudaTest`, `ColorEncodingCatalogTest` (23 tests) | PASS |

Commands (worktree `build/debug`, `ALCEDO_ENABLE_VULKAN=ON`, logs in
`build/tmp/vulkan_backend/vk3/`):

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels GpuDagVulkanWorkspaceTest VulkanColorMathTest VulkanGeometryTest VulkanComputePipelineCacheTest VulkanShaderModuleRegistryTest alcedo_gpu_check
ctest --test-dir build/debug -R "Vulkan" -j 1 --output-on-failure        default device: 83/83 (1 skipped)
ALCEDO_VULKAN_DEVICE=Iris ctest --test-dir build/debug -R "Vulkan" -j 1  83/83 (1 skipped)
build/debug/alcedo_studio/src/alcedo-gpu-check.exe --vulkan              both devices: exit 0
VulkanColorMathTest.exe, VulkanGeometryTest.exe (direct runs for the measured differences)
cmd /c scripts\msvc_env.cmd --build build\debug --target ColorEncodingOpenClTest ColorEncodingCudaTest ColorEncodingCatalogTest PipelineAcceleratorTest
ctest --test-dir build/debug -R "ColorEncoding" -j 1: 23/23
```

The OpenCL, CUDA, and Metal pass tests did not run: the only shared file with a change of code
is `color_encoding_math.h`, and its change is a new `#elif defined(__SLANG__)` branch that only
`slangc` reads; the color encoding tests of OpenCL, CUDA, and the host pass with it. The Metal
build was not run (no Mac in this phase); the Metal shaders do not define `__SLANG__`. The full
test suite was not run (repository rule).

**Checklist / exit condition:** all three exit criteria are met.

**LOC note:** 1637 lines in 14 new files (771 lines of production code, headers, and kernels; 866
lines of tests and test support) and 62 lines in 5 existing files; inside the estimate of 1300
to 1700. The largest file is `vulkan_opencl_comparison.hpp` (247 lines).

**Remaining gaps:** none for VK3. The area resize has no product caller (decision above). The
DRT modules of the scope entry are in VK12, the grain hash in VK14. The PRNG sources without a
caller are issue #1081.

### Phase VK4: RAW operators: linearize, CFA clamp, RCD, X-Trans, pack

**Objective and deliverables.** The Develop pass of VK6 can record each RAW stage before the
highlight reconstruction on Vulkan: the linearization of an RGB input and of a CFA plane, the CFA
clamp, the RCD and X-Trans demosaic, and the pack of the demosaic result into the RGBA32F image of
the graph with the crop, the inverse white balance, and the orientation. Each stage gives the
OpenCL result on the same GPU for a real Bayer RAW and a real X-Trans RAW (B9).

**Inputs and prerequisites.** VK3 (`RecordKernelDispatch`, the comparison support). The kernel
rules of sections 4.2 and 4.3: buffers are device addresses in the push constants, parameter
records are in a buffer, a parameter record is found with `uint64_t` address arithmetic.

**Source audit of the VK4 entry (2026-11-27).** The scope entry named `opencl_encode.cpp`,
`to_linear_ref.cl`, `cvt_ref_space.cl`, `debayer_rcd.cl`, and `xtrans_interpolate.cl`. The callers
on the VK3 branch (`opencl_develop_pass.cpp`, the only product caller of the RAW encode functions)
show:

| OpenCL entry point or kernel | Product caller | VK4 decision |
| --- | --- | --- |
| `EncodeLinearizeRgb` (`linearize_rgb`) | Develop pass, `DebayeredRgb` and `DirectRgb` input | VK4 |
| `EncodeToLinearRef` (`to_linear_ref_u16_to_f32`) | Develop pass, CFA input | VK4 |
| `EncodeCfaClamp01` (`cfa_clamp01_f32`) | Develop pass, CFA input without highlight reconstruction | VK4 |
| `EncodeBayerRcd` (5 kernels) | Develop pass, legacy demosaic of a Bayer CFA | VK4 |
| `EncodeXTrans` (2 kernels) | Develop pass, legacy demosaic of an X-Trans CFA | VK4 |
| `EncodePackPlanesCropInverseOrient` (`pack_planes_crop_inverse_orient`) | Develop pass, RCD without highlight reconstruction | VK4 |
| `EncodeCopyRgbaCropInverseOrient` (`copy_rgba_crop_inverse_orient`) | Develop pass: X-Trans, RGB input, raster input, neural output | VK4 |
| `EncodeHighlightReconstruct`, `EncodeHighlightReconstructPlanarAndPack` | Develop pass with highlight reconstruction | VK5 (scope entry of VK5) |
| `EncodeCopyRgbCropInverseOrient` (`copy_rgb_crop_inverse_orient`) | none | not ported |
| `to_linear_ref_f32`, `rcd_merge_rgba`, `apply_inverse_cam_mul_rgba32f` | none (`apply_inverse_cam_mul_rgba32f` has a program check of `opencl_runtime_test.cpp` only) | not ported |

The four sources without a caller are reported in a follow-up issue (completion record).

**Modules, files, and APIs.**

- `inc/edit/runtime/vulkan/vulkan_buffer_view.hpp`, `edit/runtime/vulkan/vulkan_buffer_view.cpp`
  (target `EditRuntimeVulkan`): `VulkanBufferView` (a `VulkanBackend::Buffer` and a byte offset,
  the form of `OpenClBufferView`) and `RequireBufferRange(view, bytes, alignment, operation,
  label)`, which gives the device address of the range and throws `std::invalid_argument` with the
  values when the range is outside the buffer or the offset is not aligned.
- `inc/decoders/processor/operators/gpu/vulkan_raw_encode.hpp`,
  `decoders/processor/operators/gpu/vulkan_raw_encode.cpp` (new target `RawProcessorOpVulkan`):
  `vulkan::raw::EncodeLinearizeRgb`, `EncodeToLinearRef`, `EncodeCfaClamp01`, `EncodeBayerRcd`,
  `EncodeXTrans`, `EncodePackPlanesCropInverseOrient`, `EncodeCopyRgbaCropInverseOrient`. Each one
  takes `VulkanBackend&`, `VulkanCommandContext&`, the buffer views, and the parameters of the
  OpenCL function. The pack functions write a `VulkanBackend::Texture2D` (RGBA32F, the size of the
  oriented crop) and also take the source height, so they can check the crop. `EncodeToLinearRef`
  and `EncodeXTrans` take a parameter view of `kToLinearRefParameterBytes` or
  `kXTransParameterBytes` and record the upload of their parameter record into it.
- Kernel groups (Slang in `decoders/processor/operators/gpu/vulkan_shader/`, one header for each
  group in `inc/vulkan/`):

  | Group | Kernels | Header |
  | --- | --- | --- |
  | `raw_core` | `linearize_rgb`, `to_linear_ref_u16_to_f32`, `cfa_clamp01_f32` | `vulkan_raw_core_kernels.hpp` |
  | `debayer_rcd` | `rcd_init_and_vh`, `rcd_green_at_rb`, `rcd_pq_dir`, `rcd_rb_at_rb`, `rcd_rb_at_g` | `vulkan_debayer_rcd_kernels.hpp` |
  | `xtrans_interpolate` | `xtrans_green`, `xtrans_rgba` | `vulkan_xtrans_kernels.hpp` |
  | `raw_pack` | `pack_planes_crop_inverse_orient`, `copy_rgba_crop_inverse_orient` | `vulkan_raw_pack_kernels.hpp` |

  The kernel names are the OpenCL kernel names, so the audit of a later phase can find each
  kernel in both backends.
- `VulkanKernelCatalog()`: the descriptors of the four groups.
- Test: `tests/raw/vulkan_raw_operators_test.cpp` (`VulkanRawOperatorsTest`), when Vulkan and
  OpenCL are both enabled.

**Data rules and invariants.**

- The RAW intermediate data (the CFA plane, the linear plane, the RCD planes, the X-Trans green
  plane and RGBA buffer) are buffers, as in OpenCL. The pack output is the storage image of the
  graph. A kernel reads and writes a buffer through its device address (section 4.2).
- The parameter record of `to_linear_ref_u16_to_f32` (`WBParams` and `PatternParams`, 364 bytes)
  and of the X-Trans kernels (`XTransParams`, 172 bytes) are above the 128 bytes that every
  Vulkan device has for push constants, so they are in a buffer. The other kernels have their
  parameters in the push constants. Each record has the layout of the OpenCL struct (scalar layout).
- A U16 CFA sample is read from the 32-bit word that contains it, so the stage needs no 16-bit
  storage feature. The offset of the CFA view is a multiple of 2 bytes.
- An encode function records into the open recording of the command context. It does not submit
  and does not wait (section 4.2). A request with a range outside its buffer, an offset that is not
  aligned to its element, a destination texture of another format or size, or a crop outside the
  source fails with `std::invalid_argument` before a command is recorded.
- The orientation rule is the rule of `cvt_ref_space.cl`: flip 3 turns 180 degrees, flip 5 and 6
  turn 90 degrees and exchange the width and the height, every other value keeps the orientation.
- Each kernel is a per-texel kernel with a bounded footprint (RCD 9 x 9, X-Trans radius 4), so a
  workgroup is far below 1 ms (section 4.2).

**Implementation steps.**

1. Add `VulkanBufferView` and `RequireBufferRange`.
2. Add the four kernel groups, their headers, their catalog entries, and the build rules.
3. Add `vulkan_raw_encode.hpp/.cpp` and the target `RawProcessorOpVulkan`.
4. Add `VulkanRawOperatorsTest`: one stage at a time, the same host input on OpenCL (the product
   encode functions) and on Vulkan, and the input of the next stage is the OpenCL output of the
   stage before.
5. Run the tests on both devices with the validation layer, and `alcedo-gpu-check --vulkan`.

**Primary success call chain.**

```text
vulkan::raw::EncodeToLinearRef(backend, command_context, cfa_u16, linear, parameters, width,
                               height, linearization, pattern)
  -> RequireBufferRange (each view: size, alignment) -> device addresses
  -> pack ToLinearRefParameters (the WBParams and PatternParams of the OpenCL function)
  -> VulkanBackend::UploadBufferRange(parameters) (recorded copy, NoteWrite)
  -> RecordKernelDispatch(raw_core_kernels::ToLinearRefDescriptor(), {}, push constants with the
                          three addresses, 16 x 16 groups)
       -> BeginCommand (barrier after the upload) -> vkCmdBindPipeline -> vkCmdPushConstants
       -> vkCmdDispatch -> NoteWrite
vulkan::raw::EncodeBayerRcd -> 5 dispatches in the order of OpenCL, a barrier before each
vulkan::raw::EncodePackPlanesCropInverseOrient -> check crop and texture size
  -> RecordKernelDispatch(PackPlanesDescriptor(), {0: destination}, push constants)
caller (VK6, the test): VulkanBackend::Submit or a download
```

**Primary failure call chain.**

```text
encode with a range outside its buffer, an unaligned offset, a crop outside the source, or a
destination texture that is not RGBA32F of the oriented crop size
  -> std::invalid_argument with the operation, the view, and the values
  -> no upload, no pipeline request, no command, no recording
```

**Tests and evidence.**

- `VulkanRawOperatorsTest.LinearizeRgbOfALinearDngEqualsOpenCl` (with integer codes and with
  float codes)
- `VulkanRawOperatorsTest.ToLinearRefOfABayerRawEqualsOpenCl` (tolerance 1e-5, the tolerance of
  `OpenClDevelopLinearizeMatchesCudaReferenceWithinTolerance`)
- `VulkanRawOperatorsTest.ToLinearRefOfAnXTransRawEqualsOpenCl`
- `VulkanRawOperatorsTest.ToLinearRefAtAnOddCfaOffsetEqualsOpenCl` (a CFA view at offset 2 bytes)
- `VulkanRawOperatorsTest.CfaClampOfABayerRawEqualsOpenCl` (tolerance 0)
- `VulkanRawOperatorsTest.BayerRcdOfARealRawEqualsOpenCl`
- `VulkanRawOperatorsTest.XTransOfARealRawEqualsOpenClForThreePassesAndOnePass`
- `VulkanRawOperatorsTest.PackPlanesWithTheCropOfTheRawAndEachOrientationEqualsOpenCl`
  (tolerance 0)
- `VulkanRawOperatorsTest.CopyRgbaWithTheCropOfTheRawAndEachOrientationEqualsOpenCl`
  (tolerance 0)
- `VulkanRawOperatorsTest.RawStagesRecordIntoOneRecordingWithoutASubmission`
- `VulkanRawOperatorsTest.InvalidRequestIsRejectedBeforeACommand`

The RCD and X-Trans tolerances come from the measurement of this phase; the OpenCL tests compare
these stages with CUDA at 2e-3 (green of X-Trans) or check only that the result changes (RCD).
The completion record gives the measured differences and the reason for each tolerance (RCD
1e-4 relative above 1, X-Trans 1e-5).

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanRawOperatorsTest alcedo_gpu_check
ctest --test-dir build/debug -R "VulkanRawOperatorsTest" -j 1 --output-on-failure
ALCEDO_VULKAN_DEVICE=Iris: the same ctest command
```

**Exit criteria.**

- [x] The listed tests pass on both local devices with no validation error.
- [x] `alcedo-gpu-check --vulkan` creates the pipeline of each catalog kernel.
- [x] The OpenCL, CUDA, and Metal results are unchanged: VK4 changes no shared kernel source.

**Expected diff.** 1400 to 1800 lines.

##### Phase VK4 completion record (2026-11-27)

**Status:** complete. Issue #1196. Branch `feature/vulkan-vk4-raw-operators` on the VK3 branch
`feature/vulkan-vk3-shared-kernels` (pull request #1083).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Rounding of the RCD kernels | Port of `debayer_rcd.cl` | Each float multiply is `Mul` (`AlcedoExactMul`) and each sum of three or more terms is a chain of `Add` (`AlcedoExactAdd`): an `OpFMul` or `OpFAdd` with `NoContraction` from a `spirv_asm` block of `math.slangh`. A division stays `OpFDiv`. | The OpenCL program has `FP_CONTRACT OFF`. `slangc` 2025.17.2 writes no `NoContraction`, also not for `precise`. The RCD direction weights select between two estimates where they cross 0.5, so a change of one ulp in a weight can change a texel by a large amount. Measured on the Bayer RAW (RTX 3080 Laptop GPU, 24.5 megapixels): plain port, 64219 R values above 1e-5, largest 9.7e-2; with `Mul`, 17239 values, largest 2.6e-3; with `Mul` and `Add` in the 40-term statistic, 0 values above 1e-5. A CPU evaluation of `rcd_green_at_rb` (no FMA, IEEE division) gave the OpenCL values at the texels that differed, so the Vulkan port was the side that changed. The rest of the `Add` chains changed no value of this RAW; they keep the same rule for each sum of the group. |
| Division in RCD | | `OpFDiv` | At the 1-ulp level the OpenCL division of the NVIDIA driver is not correctly rounded either: the CPU result equals Vulkan at some texels and OpenCL at others. A corrected Vulkan division (`fma` residual step) did not reduce the largest difference (8.1e-6 against 8.5e-6), so it was removed. |
| RCD tolerance | From the measurement | 1e-4, `ScaledDifference` (relative above 1) | No OpenCL test has an RCD tolerance (the OpenCL Develop test checks only that highlight reconstruction changes the result). Measured largest difference 8.5e-6 on the RTX 3080 Laptop GPU (a clipped texel near 1.0) and 3.6e-7 on Iris Xe. The cause is one ulp of a quotient (row above), amplified by the selections; 1e-4 gives a margin of 12 over the measurement and is far below the 1e-3 to 1e-1 of a kernel defect (rows above). |
| X-Trans pattern lookup | `(x % period + period) % period` (OpenCL) | `WrapCoord`: `uint(coord + 64 period) % uint(period)` in `raw_utils.slangh`, for the X-Trans color and the pattern black | On the RTX 3080 Laptop GPU the X-Trans RGBA differed from OpenCL at x = 0 only (8522 values, largest 2.1e-2). A CPU model reproduced the Vulkan values exactly when the color of x = -1 came from an unsigned remainder. The SPIR-V has `OpSRem %int`, so the driver evaluates it as unsigned. With `WrapCoord` the X-Trans result equals OpenCL exactly on that GPU. Rule added to section 4.2. |
| Parameter record of `to_linear_ref_u16_to_f32` | One record | A `WBParams` at the parameter address and a `PatternParams` at `kPatternParamsOffset` = 204, each a flat record found with address arithmetic. The host struct `ToLinearRefParameters` keeps both (static assert of the offset). | With one Slang struct with two struct members, `vkCreateComputePipelines` of the Intel driver (Iris Xe) stopped with an access violation for every entry point of the `raw_core` module (bisect with the VX1 runner: reads of two fields of the nested `PatternParams` were enough; one field alone created). Flat records create on both devices. Rule added to section 4.2. |
| `linearize_rgb` channels | A loop over the three channels (OpenCL) | One call of `LinearizeChannel` for each channel with constant indices into a local copy of the parameters | With the loop that indexed the push constant arrays with the loop counter, the Iris Xe result was the input value for every RGB channel (alpha correct). Rule added to section 4.2. |
| Kernel groups | Four groups | As planned: `raw_core`, `debayer_rcd`, `xtrans_interpolate`, `raw_pack`, 12 kernels, OpenCL kernel names. One push constant layout for the five RCD kernels (80 bytes) and one for both X-Trans kernels (32 bytes). | One layout for each group of related kernels keeps the encode function short; each kernel reads the planes of its OpenCL arguments. `LinearizeRgbPushConstants` and `RcdPushConstants` have a `reserved` field: the C++ struct with a device address has the size of a multiple of 8. |
| Pack functions | OpenCL signatures | Also the source height, and a check of the destination texture (RGBA32F, the oriented crop size) | The crop check needs the height; the OpenCL functions do not check. |
| `RcdPlanes` | Five buffer views | A struct of the five views | The call has the views by name. |
| Not ported | Audit table | `EncodeCopyRgbCropInverseOrient`, `to_linear_ref_f32`, `rcd_merge_rgba`, `apply_inverse_cam_mul_rgba32f` | No product caller: issue #1201. |

**Primary success call chain:**

```text
VulkanRawOperatorsTest (VK6: the Develop pass)
  -> RawInputLoader::LoadEncoded (real NEF, RAF, linear DNG) -> host U16 CFA or F32 RGBA
  -> VulkanBackend::CreateBuffer, UploadBufferRange (recorded copy)
  -> vulkan::raw::EncodeToLinearRef(backend, command_context, cfa_u16, linear, parameters, ...)
       -> RequireBufferRange (each view: buffer, alignment, size) -> device addresses
       -> MakeToLinearRefParameters -> UploadBufferRange(parameters) (recorded, NoteWrite)
       -> RecordKernelDispatch(raw_core_kernels::ToLinearRefDescriptor(), {}, push constants,
                               16 x 16 groups) -> BeginCommand (barrier) -> vkCmdDispatch
  -> EncodeCfaClamp01 -> EncodeBayerRcd (5 dispatches, a barrier before each)
     or EncodeXTrans (parameter upload, 2 dispatches)
  -> EncodePackPlanesCropInverseOrient or EncodeCopyRgbaCropInverseOrient
       -> PackParams (crop inside the source, RGBA32F texture of the oriented crop size)
       -> RecordKernelDispatch(..., {0: destination}, ...) -> PrepareImage -> push descriptor
  -> VulkanBackend::DownloadTexture2D or DownloadBufferRange (submit, wait, copy)
  -> ExpectEqualWithinTolerance(OpenCL::Encode* result on the same GPU, Vulkan result)
```

**Primary failure call chain:**

```text
encode with a range outside its buffer, an offset that is not aligned to its element, a view
without a buffer, a parameter range smaller than its record, an empty crop or a crop outside the
source, or a destination that is not RGBA32F of the oriented crop size
  -> std::invalid_argument with the function, the view, and the values
  -> no upload, no pipeline request, no dispatch, no recording
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `LinearizeRgbOfALinearDngEqualsOpenCl` (24 megapixels, integer codes on and off) | `VulkanRawOperatorsTest` | PASS (RTX 3080 Laptop GPU and Iris Xe): 0 difference |
| `ToLinearRefOfABayerRawEqualsOpenCl` (Nikon NEF, 6064 x 4040) | `VulkanRawOperatorsTest` | PASS: 0 (RTX 3080), 0 (Iris Xe) |
| `ToLinearRefOfAnXTransRawEqualsOpenCl` (Fujifilm RAF, 7872 x 5196) | `VulkanRawOperatorsTest` | PASS: 0 (RTX 3080), 1.2e-7 (Iris Xe, 1 ulp) |
| `ToLinearRefAtAnOddCfaOffsetEqualsOpenCl` (512 rows at a CFA offset of 2 bytes; equal to the result at offset 0) | `VulkanRawOperatorsTest` | PASS (both): 0 |
| `CfaClampOfABayerRawEqualsOpenCl` (values moved to [-0.5, 1.5]) | `VulkanRawOperatorsTest` | PASS (both): 0 |
| `BayerRcdOfARealRawEqualsOpenCl` | `VulkanRawOperatorsTest` | PASS: R 2.4e-7, G 1.2e-7, B 8.5e-6 (RTX 3080); R 1.8e-7, G 1.8e-7, B 3.6e-7 (Iris Xe) |
| `XTransOfARealRawEqualsOpenClForThreePassesAndOnePass` (green and RGBA) | `VulkanRawOperatorsTest` | PASS: 0 (RTX 3080); green 0, RGBA 1.2e-7 (Iris Xe) |
| `PackPlanesWithTheCropOfTheRawAndEachOrientationEqualsOpenCl` (crop of the NEF, flip 0, 3, 5, 6) | `VulkanRawOperatorsTest` | PASS (both): 0 |
| `CopyRgbaWithTheCropOfTheRawAndEachOrientationEqualsOpenCl` (crop of the RAF, flip 0, 3, 5, 6) | `VulkanRawOperatorsTest` | PASS (both): 0 |
| `RawStagesRecordIntoOneRecordingWithoutASubmission` (8 dispatches, 0 submissions and waits, 1 upload of 364 bytes, then one submission; every texel written) | `VulkanRawOperatorsTest` | PASS (both) |
| `InvalidRequestIsRejectedBeforeACommand` (10 requests: 0 dispatches, 0 pipeline requests, 0 uploads, no recording) | `VulkanRawOperatorsTest` | PASS (both) |
| Validation errors | `TearDown` of each GPU test | 0 on both devices |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | PASS on both devices: `gpu.kernels.ok=vulkan kernels=17`, exit 0 |
| VK0 to VK3 suites after the catalog and `math.slangh` change | `ctest -R Vulkan` (94 tests with VK4) | PASS on both devices (1 skipped: the accelerator test for a build without Vulkan) |

Commands (worktree `build/debug`, `ALCEDO_ENABLE_VULKAN=ON`, logs in
`build/tmp/vulkan_backend/vk4/`):

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanRawOperatorsTest GpuDagVulkanWorkspaceTest VulkanColorMathTest VulkanGeometryTest VulkanComputePipelineCacheTest VulkanShaderModuleRegistryTest VulkanContextTest VulkanRequirementsTest PipelineAcceleratorTest alcedo_gpu_check
ctest --test-dir build/debug -R "Vulkan" -j 1                          default device: 94/94 (1 skipped)
ALCEDO_VULKAN_DEVICE=Iris ctest --test-dir build/debug -R "Vulkan" -j 1  94/94 (1 skipped)
build/debug/alcedo_studio/src/alcedo-gpu-check.exe --vulkan              both devices: exit 0
VulkanRawOperatorsTest.exe (direct runs on both devices for the measured differences)
build/vx1/vx1_runner.exe --case pipeline (pipeline creation bisect on Iris Xe, probe modules)
```

The OpenCL, CUDA, and Metal tests did not run: VK4 changes no OpenCL, CUDA, Metal, or shared
kernel source and no shared C++ code (the changed shared files are CMake lists, the Vulkan kernel
catalog, and the Vulkan-only `math.slangh`). The full test suite was not run (repository rule).

**Checklist / exit condition:** all three exit criteria are met.

**LOC note:** 2617 lines in 15 new files and 6 existing files: 1812 lines of production code,
headers, and kernels (943 of them Slang kernels, the ports of about 870 lines of OpenCL C), 754
lines of tests and test support, and 52 lines of CMake. Above the estimate of 1400 to 1800: the
estimate did not count the test support of a real RAW comparison (OpenCL buffers and images, 209
lines) and the exact-rounding form of RCD. The largest file is `vulkan_raw_operators_test.cpp`
(545 lines).

**Remaining gaps:** none for VK4. The highlight reconstruction, the lens calibration, and the DNG
warp are VK5. The four OpenCL kernels without a caller are issue #1201.

### Phase VK5: RAW operators: highlight reconstruction, lens calibration, DNG warp

**Objective and deliverables.** The Develop pass of VK6 can record the rest of its RAW stages on
Vulkan: the highlight reconstruction of an RGBA buffer and of the RCD planes, the DNG
`WarpRectilinear` opcode, and the lens calibration of `develop.sensor_linear`. Each stage gives
the OpenCL result on the same GPU (B9), and the lens stage also passes the CPU reference cases of
`opencl_lens_calib_test.cpp`.

**Inputs and prerequisites.** VK4 (`VulkanBufferView`, `RequireBufferRange`, the pack parameters,
the comparison support of real RAW inputs). VK3 (`RecordKernelDispatch`, `EncodeCropResize`,
`math.slangh`). The kernel rules of section 4.2.

**Source audit of the VK5 entry (2026-11-27).** The scope entry named `highlight_reconstruct.cl`,
`opencl_lens_calib_ops.cpp`, and `lens_calib.cl`. The callers in `opencl_develop_pass.cpp` show:

| OpenCL entry point or kernel | Product caller | VK5 decision |
| --- | --- | --- |
| `EncodeHighlightReconstruct` (`hlr_build_mask`, `hlr_dilate_mask`, `hlr_chrominance_contrib`, `hlr_reconstruct_from_stats`) | Develop pass with highlight reconstruction: X-Trans RGBA, RGB input, neural output | VK5 |
| `EncodeHighlightReconstructPlanarAndPack` (`hlr_build_mask_planar`, `hlr_dilate_mask`, `hlr_chrominance_contrib_planar`, `hlr_reconstruct_from_stats_planar_pack`) | Develop pass with highlight reconstruction: RCD planes | VK5 |
| `hlr_reconstruct` | none | not ported; added to issue #1201 |
| The `anyclipped` count of the two mask kernels | none: the OpenCL Develop pass fills it and never reads it (the CUDA form reads it on the host to skip the stage) | not ported |
| `EncodeWarp` (`warp_rectilinear_rgba32f` of `geometry_camera.cl`) | Develop pass, DNG with `WarpRectilinear` | VK5 |
| `OpenCL::Geometry::WarpRectilinear` (`opencl_geometry_utils.cpp`) | none in the Develop pass (geometry utility test only) | not ported |
| `ApplyLensCalibration` (`edit_geometry_lens_vignetting_rgba32f`, `edit_geometry_lens_warp_rgba32f`, the rectangle crop) and the resize back to the sensor size (`ExecuteOpenClLensCalibration`) | Develop pass, `GpuPassKind::Lens` | VK5 |

**Modules, files, and APIs.**

- `VulkanBackend::FillBufferRange(buffer, offset, bytes, value, command_context)`
  (`vkCmdFillBuffer`) and `VulkanBackend::CopyTexture2DRegion(source, rectangle, destination,
  command_context)` (`vkCmdCopyImage` of a rectangle into a texture of its size): the Vulkan forms
  of `OpenClBackend::FillDeviceMemory` and of `OpenClImage::CropTo`.
- `vulkan_raw_encode.hpp/.cpp` (target `RawProcessorOpVulkan`): `HighlightScratch` (three views:
  the clip mask, the dilated mask, the statistics), `HighlightMaskBytes(width, height)`,
  `kHighlightStatisticsBytes`, `EncodeHighlightReconstruct`, and
  `EncodeHighlightReconstructPlanarAndPack`, with the parameters of the OpenCL functions.
- `inc/edit/runtime/vulkan/vulkan_dng_warp.hpp`, `edit/runtime/vulkan/vulkan_dng_warp.cpp` (target
  `EditRuntimeVulkan`): `vulkan::geometry::EncodeWarpRectilinear(backend, command_context, source,
  destination, warp)`.
- `inc/edit/runtime/lens/vulkan/vulkan_lens_calib.hpp`,
  `edit/runtime/lens/vulkan/vulkan_lens_calib.cpp` (new target `EditRuntimeLensVulkan`):
  `vulkan::lens::ResolveLensCalibrationSteps(params, width, height)` (which stages run, and the
  rectangle crop), `kLensCalibParameterBytes`, and `EncodeLensCalibration(backend,
  command_context, image, scratch, parameters, params)`, where the scratch has the warp texture
  (the image size) and the crop texture (the crop size). The caller allocates the scratch of the
  steps (a pass encoder does not allocate).
- Kernel groups:

  | Group | Kernels | Header |
  | --- | --- | --- |
  | `highlight_reconstruct` | `hlr_build_mask`, `hlr_dilate_mask`, `hlr_chrominance_contrib`, `hlr_reconstruct_from_stats`, `hlr_build_mask_planar`, `hlr_chrominance_contrib_planar`, `hlr_reconstruct_from_stats_planar_pack` | `vulkan_highlight_kernels.hpp` |
  | `dng_warp` | `warp_rectilinear_rgba32f` | `vulkan_dng_warp_kernels.hpp` |
  | `lens_calib` | `edit_geometry_lens_vignetting_rgba32f`, `edit_geometry_lens_warp_rgba32f` | `vulkan_lens_calib_kernels.hpp` |

- Tests: `tests/raw/vulkan_highlight_reconstruct_test.cpp` (`VulkanHighlightReconstructTest`),
  `tests/edit/runtime/vulkan_lens_calib_test.cpp` (`VulkanLensCalibTest`, with
  `lens_warp_test_support.hpp`), and `tests/edit/runtime/vulkan_dng_warp_test.cpp`
  (`VulkanDngWarpTest`), when Vulkan and OpenCL are both enabled.

**Data rules and invariants.**

- The highlight input (an RGBA buffer or three planes) and the RGBA result are buffers; the planar
  form writes the RGBA32F image of the graph with the pack rule of VK4. The DNG warp and the lens
  calibration read and write RGBA32F storage images, the form of `develop.sensor_unwarped` and
  `develop.sensor_linear`.
- The clip mask and the dilated mask have one 32-bit word for each texel with one bit for each of
  R, G, and B. The OpenCL masks have one byte for each channel (six planes in each buffer); the
  devices have no 8-bit storage feature (VK0 enables none), and a byte written by a texel would
  share its word with the bytes of other texels. Scratch: 8 bytes for each texel against 12 in
  OpenCL.
- The statistics record (sums of R, G, B, and counts of R, G, B) is cleared by the encode function
  with `FillBufferRange` before the mask kernel, so the caller does not clear it. The chrominance
  sums are float sums by compare-and-swap, as in OpenCL; the order of the sums is not fixed on
  either backend, so the comparison tolerance covers a different rounding of the sums.
- The lens parameter record (`LensCalibGpuParams`, a flat record) is in a buffer: it is above the
  128 push constant bytes. `EncodeLensCalibration` records its upload, as `EncodeToLinearRef`.
- The lens stages are the OpenCL stages: vignetting in place; the warp to the warp texture; the
  rectangle crop (`ComputeRectCropRoi` of OpenCL) copied to the crop texture; then the bilinear
  resize of the crop (or of the warp result) back to the image size. The resize reads 0 outside
  the crop, as the OpenCL resize of the cropped image.
- An encode function records into the open recording; it does not submit or wait. An invalid
  request fails with `std::invalid_argument` before a command is recorded.

**Implementation steps.**

1. `FillBufferRange` and `CopyTexture2DRegion`.
2. The three kernel groups, their headers, catalog entries, and build rules.
3. The encode functions and the target `EditRuntimeLensVulkan`.
4. The three tests: the same host input on OpenCL (the product functions or kernels) and on
   Vulkan; the lens test also runs the three CPU reference cases.
5. Run the tests on both devices with the validation layer, and `alcedo-gpu-check --vulkan`.

**Tests and evidence.**

- `VulkanHighlightReconstructTest.RgbaHighlightsOfAnXTransRawEqualOpenCl`
- `VulkanHighlightReconstructTest.PlanarHighlightsOfABayerRawEqualOpenClForEachOrientation`
- `VulkanHighlightReconstructTest.UnclippedTexelsKeepTheirValues`
- `VulkanHighlightReconstructTest.HighlightStagesRecordIntoOneRecordingWithoutASubmission`
- `VulkanHighlightReconstructTest.InvalidRequestIsRejectedBeforeACommand`
- `VulkanDngWarpTest.OneCoefficientSetEqualsOpenCl`, `ThreeCoefficientSetsEqualOpenCl`,
  `WarpOfARealDngEqualsOpenCl`, `InvalidRequestIsRejectedBeforeACommand`
- `VulkanLensCalibTest.Issue713GratingKeepsAmplitudeAndMatchesCpuReference`,
  `HighlightEdgeHasNoRingingAndMatchesCpuReference`, `VignettingOnlyDoesNotResample` (the three
  OpenCL cases), `EachProjectionEqualsOpenCl`, `RectangleCropEqualsOpenCl`,
  `CircleCropEqualsOpenCl`, `StepsWithoutAnEffectRecordNothing`,
  `InvalidRequestIsRejectedBeforeACommand`

Tolerances come from the measurement of this phase; the completion record gives the measured
differences and the reason for each tolerance.

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanHighlightReconstructTest VulkanLensCalibTest VulkanDngWarpTest alcedo_gpu_check
ctest --test-dir build/debug -R "VulkanHighlightReconstructTest|VulkanLensCalibTest|VulkanDngWarpTest" -j 1 --output-on-failure
ALCEDO_VULKAN_DEVICE=Iris: the same ctest command
```

**Exit criteria.**

- [x] The listed tests pass on both local devices with no validation error.
- [x] `alcedo-gpu-check --vulkan` creates the pipeline of each catalog kernel.
- [x] The OpenCL, CUDA, and Metal results are unchanged: VK5 changes no shared kernel source.

**Expected diff.** 1500 to 1900 lines.

##### Phase VK5 completion record (2026-11-27)

**Status:** complete. Issue #1203. Branch `feature/vulkan-vk5-highlight-lens-warp` on the VK4
branch `feature/vulkan-vk4-raw-operators` (pull request #1202).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Highlight masks | One word for each texel | As planned: bit 0 R, bit 1 G, bit 2 B, in the clip mask and in the dilated mask. The 7 x 7 dilation ORs the words of the window and stops when all three bits are set. | No 8-bit storage feature (VK0). The result is the OpenCL result: the masks are internal. |
| Highlight statistics | Cleared by the encode function | One record of six float words (sums, then counts of R, G, B), cleared with `FillBufferRange`; the sums are the OpenCL compare-and-swap float adds of one value for each workgroup. The first guess of the loop is 0, so the word is read only by the atomic operation. | The OpenCL caller clears five buffers before the call; a Vulkan caller cannot forget it. |
| `HighlightParams` | The layout of the OpenCL struct | `HighlightLimits` (36 bytes): the clip levels and the dark limits as named fields, without the chrominance array (only `hlr_reconstruct` reads it, and it has no caller). Three push constant layouts: RGBA (80 bytes, also the dilation), planar (96), planar pack (120, with the `PackOrientParams` of VK4). | Named fields keep constant indices (section 4.2). The planar pack record fits in 128 bytes only without the chrominance. |
| Cube root of the reference mean | `pow(m, 1/3)` (OpenCL) | `AlcedoPow(m, 1/3)`, and 0 for a mean of 0 | GLSL.std.450 `pow` is `exp2(y log2 x)` (VK3 record). |
| `PackOrientParams` and the orientation rule | In `raw_pack.slang` | Moved to `raw_pack.slangh`, included by `raw_pack.slang` and `highlight_reconstruct.slang` | One definition for the two groups. |
| Lens trigonometric functions | Port of `lens_calib.cl` | `AlcedoSin`, `AlcedoCos`, `AlcedoTan`, `AlcedoAsin`, `AlcedoAtan`, `AlcedoAtan2` (`math.slangh`, Cephes single-precision forms); `sinpi` of the Lanczos weights is the reduced `AlcedoSin(pi r)`. Rule added to section 4.3. | With the GLSL.std.450 functions the projections differed from OpenCL by up to 4.4e-2 at 550 to 9937 values of the 640 x 480 image (RTX 3080 Laptop GPU). With the Cephes forms 11 to 99 values remained, and the CPU evaluation in double (next row) showed that they are not a precision problem. |
| Lens comparison input | `opencl_lens_calib_test.cpp` cases | The three CPU reference cases with their patterns (grating, highlight edge), and the comparisons with OpenCL on a position ramp, with two tolerances (inside the source; every texel). Rule added to section 11. | A double-precision evaluation of the differing texels on a pattern with detail: each one had a source position within 2e-5 pixel of an integer row, where the 2 x 2 clamp of the sampler (#713) switches to other rows. The CPU reference gave the OpenCL value at some of these texels and the Vulkan value at others, so the sampler is discontinuous there by design and neither backend is wrong. On the ramp the clamp does not act inside the source. |
| DNG warp comparison input | The OpenCL test tolerance 1e-5 | The position ramp, 1e-5 for texels whose bilinear footprint is inside the source (classified with the source positions in double), 8 ulp of the largest coordinate for the others | On a pattern with detail at 641 x 427 the differences were 3.5e-5: about 2 ulp of a source position near 600 times the gradient of the pattern. An exact-rounding form of the position (`AlcedoExactMul`, `AlcedoExactAdd`) did not reduce them, so it was removed. On Iris Xe the real DNG gave 9.8e-4 at the border: 2 ulp of a position near 8000. |
| Lens stages | The OpenCL stages and the resize | `EncodeLensCalibration`: vignetting in place; the warp into the warp texture; `CopyTexture2DRegion` of the rectangle crop (`OpenClImage::CropTo`); `EncodeCropResize` of the whole result back to the image size (a copy when the result has the image size). `ResolveLensCalibrationSteps` gives the stages and the crop, so the caller allocates the scratch. The sizes of the parameters must be the image size. | The resize of OpenCL reads 0 outside the cropped image; the copy to a texture of the crop size keeps that rule. A size of the parameters that is not the image size makes the OpenCL warp read outside its image. |
| DNG warp coefficients | `float coefficient_sets[3][6]` | Three arrays `set_r`, `set_g`, `set_b` in the push constants (92 bytes) and a selection with constant indices | The rule of section 4.2 for push constant arrays. |
| Backend transfers | Not in the plan | `VulkanBackend::FillBufferRange` (`vkCmdFillBuffer`) and `CopyTexture2DRegion` (`vkCmdCopyImage` with a source offset), with tests in `GpuDagVulkanWorkspaceTest` | The forms of `OpenClBackend::FillDeviceMemory` and `OpenClImage::CropTo`; VK6 needs both. |
| Bayer test RAW | A real RAW with clipped highlights | `raw/street/_DSC0155.NEF` (1.3 million reconstructed texels of 24.4 megapixels). The X-Trans RAW of VK4 has 24037. | `raw/blue_clipping/_DSC9729.ARW` changed only 430 texels, `raw/airplane/_DSC1704.NEF` 1076, `raw/landscape/_DSC4735.ARW` none. |
| Not ported | Source audit | `hlr_reconstruct` and the `anyclipped` count | No caller; added to issue #1201. |

**Primary success call chain:**

```text
VulkanHighlightReconstructTest (VK6: the Develop pass with highlight reconstruction)
  -> OpenCL ToLinearRef (no clamp) -> EncodeBayerRcd or EncodeXTrans -> the same host input
  -> vulkan::raw::EncodeHighlightReconstructPlanarAndPack(backend, context, r, g, b, destination,
                                                          scratch, cam_mul, crop, w, h, flip)
       -> RequireBufferRange (planes), PackParams (crop, RGBA32F of the oriented crop size)
       -> RequireHighlightScratch (masks of the crop size, statistics) -> MakeHighlightLimits
       -> FillBufferRange(statistics, 0) -> RecordKernelDispatch x 4: build mask, dilate,
          chrominance (workgroup sum, CAS float add), reconstruct and pack (push descriptor)
  -> DownloadTexture2D -> ExpectEqualWithinTolerance(OpenCL::EncodeHighlight..., Vulkan)
VulkanLensCalibTest (VK6: GpuPassKind::Lens)
  -> ResolveLensCalibrationSteps(params, w, h) -> the caller creates the warp and crop textures
  -> vulkan::lens::EncodeLensCalibration(backend, context, image, scratch, parameters, params)
       -> size checks -> UploadBufferRange(LensCalibGpuParams) -> vignetting dispatch (in place)
       -> warp dispatch (AlcedoSin/Cos/Tan/Atan, Lanczos-3 with the 2 x 2 clamp) -> warped
       -> CopyTexture2DRegion(crop) -> EncodeCropResize(result -> image)
VulkanDngWarpTest (VK6: develop.sensor_unwarped -> develop.sensor_linear)
  -> vulkan::geometry::EncodeWarpRectilinear(backend, context, source, destination, warp)
       -> RGBA32F, equal sizes, 1 to 3 sets -> RecordKernelDispatch(warp_rectilinear_rgba32f)
```

**Primary failure call chain:**

```text
a range outside its buffer, an unaligned view, masks smaller than the region, a crop outside
the planes, a destination of another size or format, a missing or wrong lens scratch texture,
lens parameters for another size, one texture as warp source and destination, an opcode without
coefficient sets
  -> std::invalid_argument with the function, the view or texture, and the values
  -> no fill, no upload, no pipeline request, no dispatch, no recording
```

**What was proven (executed tests):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `RgbaHighlightsOfAnXTransRawEqualOpenCl` (RAF, 7872 x 5196) | `VulkanHighlightReconstructTest` | PASS: 8.9e-7 (RTX 3080), 8.5e-7 (Iris Xe), relative above 1 |
| `PlanarHighlightsOfABayerRawEqualOpenClForEachOrientation` (NEF, flip 0, 3, 5, 6) | `VulkanHighlightReconstructTest` | PASS: 9.3e-7 (RTX 3080), 8.7e-7 (Iris Xe) |
| `UnclippedTexelsKeepTheirValues` (no change below 95 % of the clip level; no clipped channel darker) | `VulkanHighlightReconstructTest` | PASS (both) |
| `HighlightStagesRecordIntoOneRecordingWithoutASubmission` (8 dispatches, 0 submissions, 0 uploads, then one submission) | `VulkanHighlightReconstructTest` | PASS (both) |
| `InvalidRequestIsRejectedBeforeACommand` (8 requests) | `VulkanHighlightReconstructTest` | PASS (both) |
| `OneCoefficientSetEqualsOpenCl`, `ThreeCoefficientSetsEqualOpenCl` (641 x 427) | `VulkanDngWarpTest` | PASS: 3.0e-7 inside the source (both); no border texel |
| `WarpOfARealDngEqualsOpenCl` (the WarpRectilinear of a real DNG, 3 sets, 200 million values) | `VulkanDngWarpTest` | PASS: inside 1.8e-7 / 3.0e-7, border 1.2e-7 / 9.8e-4 (RTX 3080 / Iris Xe; border tolerance 7.8e-3) |
| `InvalidRequestIsRejectedBeforeACommand` (5 requests) | `VulkanDngWarpTest` | PASS (both) |
| `Issue713GratingKeepsAmplitudeAndMatchesCpuReference` | `VulkanLensCalibTest` | PASS: 8.3e-5 / 7.2e-5 from the CPU reference (tolerance 2e-4), amplitude 0.931 |
| `HighlightEdgeHasNoRingingAndMatchesCpuReference`, `VignettingOnlyDoesNotResample` | `VulkanLensCalibTest` | PASS (both) |
| `EachProjectionEqualsOpenCl` (13 pairs: each conversion to and from erect) | `VulkanLensCalibTest` | PASS: inside the source 1.5e-6 at most (both); every texel 3.0e-4 (RTX 3080), 3.7e-4 (Iris Xe) |
| `RectangleCropEqualsOpenCl` (after the warp; after the vignetting), `CircleCropEqualsOpenCl` | `VulkanLensCalibTest` | PASS: 4.2e-7, 1.8e-7, 4.8e-7 (RTX 3080); 4.2e-7, 1.2e-7, 6.0e-7 (Iris Xe) |
| `StepsWithoutAnEffectRecordNothing`, `InvalidRequestIsRejectedBeforeACommand` (8 requests) | `VulkanLensCalibTest` | PASS (both) |
| `BufferFillWritesTheValueOnlyInItsRange`, `TextureRegionCopyKeepsTheTexelsOfTheRectangle` | `GpuDagVulkanWorkspaceTest` | PASS (both) |
| Validation errors | `TearDown` of each GPU test | 0 on both devices |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | PASS on both devices: `gpu.kernels.ok=vulkan kernels=27`, exit 0 |
| VK0 to VK4 suites after the `math.slangh`, `raw_pack.slang`, backend, and catalog change | `ctest -R Vulkan` (111 tests) | PASS on both devices (1 skipped: the accelerator test for a build without Vulkan) |

Commands (worktree `build/debug`, `ALCEDO_ENABLE_VULKAN=ON`, logs in
`build/tmp/vulkan_backend/vk5/`):

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanRawOperatorsTest VulkanHighlightReconstructTest VulkanLensCalibTest VulkanDngWarpTest GpuDagVulkanWorkspaceTest VulkanColorMathTest VulkanGeometryTest VulkanComputePipelineCacheTest VulkanShaderModuleRegistryTest VulkanContextTest VulkanRequirementsTest PipelineAcceleratorTest alcedo_gpu_check
ctest --test-dir build/debug -R "Vulkan" -j 1                          default device: 111/111 (1 skipped)
ALCEDO_VULKAN_DEVICE=Iris ctest --test-dir build/debug -R "Vulkan" -j 1  111/111 (1 skipped)
build/debug/alcedo_studio/src/alcedo-gpu-check.exe --vulkan              both devices: exit 0
VulkanHighlightReconstructTest.exe, VulkanLensCalibTest.exe, VulkanDngWarpTest.exe (direct runs on both devices for the measured differences)
```

The OpenCL, CUDA, and Metal tests did not run: VK5 changes no OpenCL, CUDA, Metal, or shared
kernel source and no shared C++ code (the changed shared files are CMake lists, the Vulkan kernel
catalog, the Vulkan-only `math.slangh` and `raw_pack.slang`, and the test support of VK4). The
full test suite was not run (repository rule).

**Checklist / exit condition:** all three exit criteria are met.

**LOC note:** 3638 lines added and 113 removed in 14 new files and 15 existing files: 1336 lines
of Slang kernels and math (the ports of about 1090 lines of OpenCL C, and 174 lines of
trigonometric functions), 1010 lines of production C++, headers, and CMake, and 1292 lines of
tests and test support (74 lines moved from `vulkan_raw_operators_test.cpp` to its support
header). Above the estimate of 1500 to 1900 and above the phase limit of 2000 lines (section
13): the estimate did not count the trigonometric functions, the two backend transfers, or the
three comparison tests of real RAW and DNG inputs with the position classification that the
measurements needed. Each operator is a separate group of files (kernel group, header, encode
function, test), so the pull request can be read one operator at a time. The largest file is
`lens_calib.slang` (531 lines).

**Remaining gaps:** none for VK5. The Develop pass that calls these functions is VK6. The OpenCL
kernel and the count without a caller are in issue #1201.

### Phase VK6: Develop pass (UploadRaw, UploadRgb)

**Objective and deliverables.** `PassEncoder<VulkanBackend, UploadRaw>` and
`PassEncoder<VulkanBackend, UploadRgb>` write `develop.sensor_linear` from a prepared RAW input
with the RAW operators of VK4 and VK5, in the order of `ExecuteOpenClDevelop`. The result equals
the result of the reference backend on the same GPU (B9). The work runs on the Apple M4 with
MoltenVK too (user request 2026-11-27: VK6 on the Mac, to find out if the Vulkan backend runs
there); on that computer the reference backend is Metal, because the build has OpenCL on Windows
only.

**Inputs and prerequisites.** VK4 and VK5 (the encode functions, `FillBufferRange`,
`CopyTexture2DRegion`, `EncodeCropResize`). VK2 (`VulkanRenderDevice`, the workspace, `Wait`).
The kernel rules of section 4.2.

**Source audit of the VK6 entry (2026-11-27).** `ExecuteOpenClDevelop` (`opencl_develop_pass.cpp`)
has four input branches and two stages after them:

| OpenCL branch or stage | Kernels | VK6 decision |
| --- | --- | --- |
| CFA input, legacy demosaic: upload, `ToLinearRef`, `CfaClamp01` without highlight reconstruction, RCD or X-Trans, then the pack or the highlight reconstruction and the pack | VK4, VK5 | VK6 |
| CFA input, Neural Engine demosaic (`EncodeNeural`) | DemosaicNet (VK16, deferred by A5) | VK6 fails with an error that names the method and the Vulkan backend (B8). No other demosaic method runs. |
| `DebayeredRgb` or `DirectRgb` input: upload, `LinearizeRgb`, optional highlight reconstruction, crop and orientation | VK4, VK5 | VK6 |
| `RasterRgb` input: `LinearizeRaster` (`display_to_ap1.cl`) | VK7 | VK6 fails with an error that names VK7. |
| DNG `WarpRectilinear` (`develop.sensor_unwarped` to `develop.sensor_linear`) | VK5 | VK6 |
| `GpuPassKind::Lens` (`ExecuteOpenClLensCalibration`: copy to a buffer, wait, the lens operations of the OpenCL context queue, resize, copy back) | VK5 | VK6, recorded into the open recording without a wait |

**Modules, files, and APIs.**

- `inc/edit/runtime/vulkan/vulkan_develop_pass.hpp`, `edit/runtime/vulkan/vulkan_develop_pass.cpp`
  (new target `EditRuntimeVulkanPasses`, the target of the later pass encoders too):
  `ExecuteVulkanDevelop(VulkanRenderDevice&, const ExecutionPlan&, const PreparedRawInput&,
  const PipelineDocument&)`.
- `inc/edit/runtime/vulkan/vulkan_pass_encoder.hpp`: the two `PassEncoder` specializations.
- `VulkanBackend::SynchronizeRecordedWork(command_context)`: submits the open recording and waits
  for it, without a change of the render state; `PlanExecutor` calls it after the Develop pass so
  that the Develop scratch is destroyed before the geometry pass allocates.
- macOS: `VulkanContext` loads the Homebrew or bundle Vulkan loader when `volkInitialize` finds
  none, enables `VK_KHR_portability_enumeration` on the instance and `VK_KHR_portability_subset`
  on a device that lists it. No other code is specific to macOS.
- Test: `tests/edit/runtime/vulkan_develop_test.cpp` (`VulkanDevelopTest`), built when Vulkan and
  OpenCL or Metal are enabled. The reference backend is OpenCL when the build has OpenCL, else
  Metal.

**Data rules and invariants.**

- The Develop scratch (the CFA upload, the linear plane, the demosaic planes, the RGBA buffers,
  the highlight masks and statistics, the parameter records) is a set of `VulkanBackend::Buffer`
  objects of the pass, created for one Develop and released at the end of the function. A
  released buffer stays alive until the batch that uses it completes (VK2), so the release needs
  no wait. This is the Vulkan form of the OpenCL `ExactRelease` slabs. A stable render runs no
  Develop pass (`PlanExecutor` binds the published `develop.sensor_linear`), so it creates no
  scratch.
- The whole Develop pass records into the open recording; it submits nothing. The OpenCL lens
  stage waits for the queue twice, the Vulkan stage does not.
- The peak memory is the sum of the scratch of the branch, because nothing is destroyed before
  the submission completes. The OpenCL pass destroys each slab after its last kernel. For a
  45.7 megapixel Bayer RAW with highlight reconstruction the difference is the U16 upload, the
  linear plane, and the RCD weights, which OpenCL releases before the highlight stage (about
  460 MB of 1.55 GB). The export split of VK17 can release the scratch of a completed
  submission.
- The lens scratch textures (the warp texture and the crop texture) come from
  `VulkanBackend::CreateTexture2D` and follow the same release rule.
- An input without a Develop node, a call outside `BeginRender`, an empty CFA, or a CFA plane that
  is not tightly packed fails with `std::runtime_error` before a command is recorded, with the
  messages of the OpenCL pass.

**Implementation steps.**

1. macOS loader and portability extensions; the Mac build and the VK0 to VK2 tests on MoltenVK.
2. `SynchronizeRecordedWork`.
3. `ExecuteVulkanDevelop` and the pass encoders.
4. `VulkanDevelopTest` against OpenCL (Windows) and Metal (macOS).
5. Runs: Apple M4 (MoltenVK), RTX 3080 Laptop GPU and Iris Xe when the GPU of the Windows computer
   is free (section 11).

**Tests and evidence.**

- `VulkanDevelopTest.BayerRawEqualsTheReferenceBackend` (`raw/street/_DSC0155.NEF`, highlight
  reconstruction off and on)
- `VulkanDevelopTest.XTransRawEqualsTheReferenceBackend` (`raw/xtrans_edge/DSCF0019.RAF`, off and
  on)
- `VulkanDevelopTest.LinearDngEqualsTheReferenceBackend` (`raw/linear_dng/mfzoty.dng`, the
  `DebayeredRgb` branch, off and on)
- `VulkanDevelopTest.DngWarpOfARealDngEqualsTheReferenceBackend` (the DNG of VK5)
- `VulkanDevelopTest.LensCalibrationEqualsTheReferenceBackend` (a `DirectRgb` position ramp with
  the Lensfun profile of the OpenCL vignetting test, vignetting and distortion)
- `VulkanDevelopTest.DevelopRecordsIntoOneRecordingWithoutASubmission`
- `VulkanDevelopTest.ScratchIsReleasedAndTheOutputIsReusedByASecondDevelop`
- `VulkanDevelopTest.NeuralEngineAndRasterInputFailWithTheirPhase`
- `VulkanDevelopTest.InvalidRequestIsRejectedBeforeACommand`

Tolerances come from the measurement of this phase. The completion record gives the measured
differences and the reason for each tolerance.

**Build and run commands.**

```text
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanDevelopTest
         ctest --test-dir build/debug -R "VulkanDevelopTest" -j 1 --output-on-failure
macOS:   source ~/Projects/vulkan_tools/env.sh
         cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanDevelopTest
         ctest --test-dir build/macos-vulkan -R "Vulkan" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The listed tests pass on the Apple M4 with no validation error.
- [x] The listed tests pass on both Windows devices against OpenCL, or the record names the runs
      that did not take place. (Windows runs of 2026-11-27 below; Iris Xe with the bound of #1213.)
- [x] The OpenCL, CUDA, and Metal results are unchanged: VK6 changes no shared kernel source.

**Expected diff.** 1400 to 1800 lines.

##### Phase VK6 completion record (2026-11-27)

**Status:** complete on the Apple M4 (MoltenVK). Issue #1209. Branch
`feature/vulkan-vk6-develop-pass` on the VK5 branch `feature/vulkan-vk5-highlight-lens-warp`
(pull request #1205). The Windows runs against OpenCL did not take place: during the session the
RTX 3080 Laptop GPU was at 100 % load from an application of the user, and the GPU runs yield to
the user's work (section 11). MSVC build of the Vulkan targets: PASS (`AlcedoVulkanKernels VulkanContextTest GpuDagVulkanWorkspaceTest VulkanDevelopTest alcedo_gpu_check`, exit 0); the Windows tests did not run.

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| MoltenVK fast math | Not in the plan | `VulkanContext` sets the MoltenVK setting `MVK_CONFIG_FAST_MATH_ENABLED` to 0 (never) through `VK_EXT_layer_settings` at instance creation on macOS. Rule added to section 4.3. | MoltenVK compiles each kernel with the Metal fast math option by default. With it the X-Trans result of Vulkan was equal to fast-math Metal within 1.2e-7, and the RCD result differed from strict Metal by up to 0.13; without it Vulkan was equal to strict Metal bit for bit (next row). |
| Reference backend on the Mac | Metal | Metal, the product Develop pass. The test chooses OpenCL when the build has OpenCL, else Metal, at compile time. | The macOS build has no OpenCL. |
| Proof of the Vulkan result on the Mac | Not in the plan | A local experiment (not committed): Metal built without fast math and without FP contraction (`-fno-fast-math -ffp-contract=off` in the common Metal flags). Measured differences of `develop.sensor_linear` below. | The product Metal RAW kernels have fast math and contraction, the OpenCL kernels have neither (`FP_CONTRACT OFF` in the RCD program). Equality with strict Metal shows that MoltenVK runs the SPIR-V of VK4 and VK5 with the float rules of OpenCL. |
| Tolerance against product Metal | Not in the plan | The demosaic cases (Bayer, X-Trans): root mean square 2.5e-4, largest value 0.5. The other cases: the tolerances of the OpenCL reference. | Product Metal differs from strict Vulkan by up to 0.468 (Bayer, at x = 4) and 0.143 (X-Trans), with an rms of 1.3e-4 at most. A wrong kernel gives an rms of 1e-2 or more. |
| DNG warp comparison | The DNG of VK5 | The WarpRectilinear of that DNG on a position ramp of its sensor size (8680 x 5778, 3 sets), as a `DirectRgb` input with `dng_warp_rectilinear` | Plan section 11. On the DNG pixels the differences from Metal were up to 3.9e-3 inside the image, the position rounding of the source coordinate times the gradient of the image. |
| Warp border tolerance | 8 ulp of the largest coordinate (VK5) | 8 ulp of a coordinate below 16384 (7.8e-3) for texels with an edge distance below 16, 1e-5 for the others | The texels above 1e-5 were all in the 4-pixel border band, with a difference of 9.77e-4: 1 ulp of a source coordinate near 8671, because the footprint reads 0 outside the source. |
| Develop scratch | Buffers of the pass | As planned: `VulkanBackend::Buffer` objects of the call (CFA upload, linear plane, one parameter buffer with 256-byte aligned records, RCD planes or X-Trans buffers, highlight masks and statistics); the lens warp and crop textures from `CreateTexture2D`. A buffer is reset after its last recorded use; it is destroyed when its batch completes. | No transient arena slab can give a `VulkanBufferView` (a view needs the `Buffer` object), and the release needs no wait. |
| `SynchronizeRecordedWork` | Submit and wait | `SubmitRecording` and `WaitBatches` of the backend; the render submission in flight does not change | The Develop scratch rule of `PlanExecutor`. |
| Vulkan loader on macOS | Not in the plan | `volkInitialize` first; then `<bundle>/Contents/Frameworks/libvulkan.1.dylib`, `/opt/homebrew/lib/libvulkan.1.dylib`, `/usr/local/lib/libvulkan.1.dylib` with `volkInitializeCustom`. The error names each path. | dlopen of a bare name does not search the Homebrew folders. |
| Portability | Not in the plan | `VK_KHR_portability_enumeration` and `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` when the loader lists the extension; `VK_KHR_portability_subset` on a device that lists it. The device list of `VulkanContextTest` does the same. | Without them the loader reports no driver (`VK_ERROR_INCOMPATIBLE_DRIVER`). |
| Not ported | Source audit | The Neural Engine demosaic and the raster input fail with an error that names the decision A5 or the phase VK7 before a command is recorded | B8. |

**Measured differences of `develop.sensor_linear` on the Apple M4** (`ScaledAboveOne`, logs
`build/tmp/vulkan_backend/vk6/` of the Mac worktree):

| Case | Product Metal against strict Vulkan (the test) | Metal without fast math against MoltenVK with fast math | Strict Metal against strict Vulkan |
| --- | --- | --- | --- |
| Bayer NEF (`_DSC0155`, 24.4 MP) | 0.468, rms 5.9e-5 | 0.133, rms 2.5e-5 | 0 (equal) |
| Bayer NEF, highlight reconstruction | 0.428, rms 1.3e-4 | 0.428, rms 1.2e-4 | 9.5e-7, rms 2.0e-8 |
| X-Trans RAF (`DSCF0019`, 40.1 MP) | 0.143, rms 9.5e-5 | 0.143, rms 9.5e-5 | 0 (equal) |
| X-Trans RAF, highlight reconstruction | 0.143, rms 9.5e-5 | 0.143, rms 9.5e-5 | 9.5e-7, rms 1.8e-9 |
| Linear DNG (`mfzoty`, `DebayeredRgb`), off; on | 0; 5.8e-7 | not run | 0; 5.8e-7 |
| WarpRectilinear on the ramp (3 sets) | 2.1e-6 rms, border 9.8e-4, inside below 1e-5 | not run | 2.2e-6 rms, border 9.8e-4, inside below 1e-5 |
| Lens: vignetting, distortion, TCA on the 640 x 480 ramp | 8.3e-7 | 8.7e-7 | 1.2e-6 |

The product Metal against MoltenVK with fast math (the first run, before the setting): X-Trans
1.2e-7, the linear DNG 0, the Bayer RAW 0.468.

**Primary success call chain:**

```text
PlanExecutor<VulkanBackend> (VK17) or VulkanDevelopTest
  -> PassEncoder<VulkanBackend, UploadRaw | UploadRgb>::Encode -> ExecuteVulkanDevelop
       -> RequireSupportedInput (render open, Develop node, not raster, not Neural Engine, packed)
       -> AcquireImageForWrite(develop.sensor_unwarped or develop.sensor_linear, RGBA32F)
       -> CFA: UploadBufferRange(CFA) -> EncodeToLinearRef -> [EncodeCfaClamp01]
               -> EncodeBayerRcd -> EncodePackPlanesCropInverseOrient
                  | EncodeHighlightReconstructPlanarAndPack
               -> or EncodeXTrans -> [EncodeHighlightReconstruct] -> EncodeCopyRgbaCropInverseOrient
       -> RGB: UploadBufferRange -> EncodeLinearizeRgb -> [EncodeHighlightReconstruct]
               -> EncodeCopyRgbaCropInverseOrient
       -> DNG warp: AcquireImageForWrite(develop.sensor_linear) -> EncodeWarpRectilinear
       -> Lens: LensCalibrationResolver::Resolve -> ResolveLensCalibrationSteps
               -> CreateTexture2D (warp, crop) -> EncodeLensCalibration
       -> the scratch buffers retire; nothing is submitted
  -> PlanExecutor: SynchronizeRecordedWork -> the scratch is destroyed
```

**Primary failure call chain:**

```text
render not open, no Develop node, raster input (VK7), Neural Engine method (A5), empty CFA,
CFA or RGB plane not tightly packed
  -> std::runtime_error "ExecuteVulkanDevelop: ..." before an allocation or a command
an allocation above the memory limit, or an invalid view in an encode function
  -> std::runtime_error or std::invalid_argument; the buffers of the call retire with the open
     recording; PlanExecutor cancels the render (VK2 rule)
```

**What was proven (executed tests, Apple M4, MoltenVK 1.4.2, validation layer 1.4.363):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `BayerRawEqualsTheReferenceBackend` (off, on) | `VulkanDevelopTest` | PASS against Metal (rms 5.9e-5, 1.3e-4) |
| `XTransRawEqualsTheReferenceBackend` (off, on) | `VulkanDevelopTest` | PASS against Metal (rms 9.5e-5) |
| `LinearDngEqualsTheReferenceBackend` (off, on) | `VulkanDevelopTest` | PASS: 0, 5.8e-7 |
| `DngWarpOfARealDngEqualsTheReferenceBackend` | `VulkanDevelopTest` | PASS: inside below 1e-5, border 9.8e-4 |
| `LensCalibrationEqualsTheReferenceBackend` | `VulkanDevelopTest` | PASS: 8.3e-7 |
| `DevelopRecordsIntoOneRecordingWithoutASubmission` (0 submissions, 0 waits, then 1 submission) | `VulkanDevelopTest` | PASS |
| `ScratchIsReleasedAndTheOutputIsReusedByASecondDevelop` (1 texture, 11 buffers; after the wait 0 retired and only the output live; a second Develop creates no texture and gives equal values) | `VulkanDevelopTest` | PASS |
| `NeuralEngineAndRasterInputFailWithTheirPhase`, `InvalidRequestIsRejectedBeforeACommand` | `VulkanDevelopTest` | PASS |
| VK0 to VK2 suites on MoltenVK | `ctest -R "Vulkan\|PipelineAccelerator"` | 86 tests, 84 PASS, 2 skipped (Windows only) |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | PASS: `gpu.kernels.ok=vulkan kernels=27` |
| Validation errors | `TearDown` of each GPU test | 0 |
| The VK3 to VK5 comparison tests | | not built on macOS: they need OpenCL |
| Windows: `VulkanDevelopTest` against OpenCL on the RTX 3080 Laptop GPU and Iris Xe | | not run (see the status) |

Mac setup (development only, no product change): Homebrew `molten-vk` 1.4.2, `vulkan-loader`,
`vulkan-headers`, `vulkan-volk`, `vulkan-validationlayers`, `vulkan-tools`, `spirv-tools`
(1.4.363); Slang 2025.17.2 (the version of the Windows SDK) and VulkanMemoryAllocator 3.3.0 (header
in the vcpkg layout `include/vma/`) in `~/Projects/vulkan_tools`; a worktree
`~/Projects/PuerhLab-vulkan` with links to the dependencies of the main checkout. `VulkanContext`
logs Vulkan 1.3.357 (`vulkaninfo`: 1.4.357), one queue family, `maxPushConstantsSize` 4096, `shaderFloat64` false.

Commands:

```text
macOS:  source ~/Projects/vulkan_tools/env.sh
        cmake --preset macos_debug_tests -B build/macos-vulkan -DALCEDO_ENABLE_VULKAN=ON
              -DALCEDO_BUILD_TESTS_BY_DEFAULT=OFF -DALCEDO_BUILD_CI_TESTS=OFF
              -DALCEDO_SLANGC_EXECUTABLE=$HOME/Projects/vulkan_tools/slang/bin/slangc
              -DCMAKE_PREFIX_PATH=$HOME/Projects/vulkan_tools/vma
              -DALCEDO_FILM_PRINT_RESOURCE_DIR=$HOME/Projects/film_print_resources
        cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanRequirementsTest
              VulkanContextTest VulkanShaderModuleRegistryTest VulkanComputePipelineCacheTest
              GpuDagVulkanWorkspaceTest PipelineAcceleratorTest VulkanDevelopTest alcedo_gpu_check
        ctest --test-dir build/macos-vulkan -R "Vulkan|PipelineAccelerator" -j 1     86/86
        build/macos-vulkan/.../alcedo-gpu-check --vulkan                            exit 0
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels
              VulkanContextTest GpuDagVulkanWorkspaceTest VulkanDevelopTest alcedo_gpu_check
```

The OpenCL, CUDA, and Metal tests did not run: VK6 changes no OpenCL, CUDA, Metal, or shared
kernel source and no shared C++ code outside the Vulkan targets.

**Finding outside this phase.** The product Metal RAW kernels (RCD, X-Trans) give results that
differ from the strict form by up to 0.47 at a few texels near the image edge and by 1e-4 rms,
because of fast math and FMA contraction. The CUDA and OpenCL forms are strict. VK6 changes
nothing in Metal.

**Checklist / exit condition:** the first and third exit criteria are met. The second is open:
the Windows runs did not take place.

**LOC note:** about 1080 lines of code: 410 lines of production C++, headers, and CMake (the Develop pass 331, the macOS part of `VulkanContext` 85), 537 lines of tests, and the CMake rules. Below the estimate of 1400 to 1800: the kernels and encode functions came with VK4 and VK5.

**Remaining gaps:** the Windows runs of `VulkanDevelopTest` against OpenCL on the RTX 3080 Laptop
GPU and Iris Xe (B9), and of the VK0 to VK5 suites after the `VulkanContext` change.

**Windows runs (2026-11-27, during VK7, on the VK7 branch).** RTX 3080 Laptop GPU: the VK0 to VK7
suites pass (131 tests, 130 PASS, 1 skipped), `VulkanDevelopTest` 9 of 9 against OpenCL:

| Case | OpenCL against Vulkan, RTX 3080 | Iris Xe |
| --- | --- | --- |
| Bayer NEF, highlight reconstruction off / on | 3.1e-5 / 3.7e-6 | 0.0495, rms 1.4e-5 / 0.30 at x = 4, rms 1.2e-4 (#1213, accepted) |
| X-Trans RAF, off / on | equal / 8.7e-7 | 1.2e-7 / 8.5e-7 |
| Linear DNG, off / on | equal / 5.8e-7 | equal / 1.2e-7 |
| WarpRectilinear of a real DNG | 1.8e-7 | inside below 1e-5, border 9.8e-4 (1 ulp) |
| Lens vignetting, distortion, TCA | 5.8e-7 | 8.0e-7 |

Iris Xe: the RCD stage alone differs between the Iris Xe Vulkan and OpenCL drivers on this
clipped NEF in the red and blue planes (1441 and 1176 values above 1e-4, largest 0.058), the green
plane is equal, and neither Iris Xe backend gives the RTX 3080 result there; the VK4 RCD test uses
a NEF without large clipped areas (#1213). The user accepted the difference of 0.058 (2026-11-27).
`VulkanDevelopTest` therefore bounds the demosaic cases on an Intel GPU (vendor id 0x8086) by the
rule of the Metal comparison: rms 2.5e-4, largest 0.5. Other GPUs keep 1e-4 against OpenCL. With
it, `VulkanDevelopTest` passes 9 of 9 on both devices, and the second exit criterion is met.

### Phase VK7: GeometryResample, CameraToAp1, raster input, DisplayToAp1

**Objective and deliverables.** The Vulkan backend writes `develop.image` (ACEScc AP1, the input
of the Color Grades) for a RAW input and for a raster input, in the order of the OpenCL passes:
`PassEncoder<VulkanBackend, GeometryResample | CameraToAp1 | DisplayToAp1>` and the raster branch
of `ExecuteVulkanDevelop`. The work runs on the Apple M4 with MoltenVK (user request 2026-11-27:
VK7 on the Mac; the Windows computer is in use). The reference backend on that computer is Metal
(VK6 rule).

**Inputs and prerequisites.** VK6 (`ExecuteVulkanDevelop`, `develop.sensor_linear`). VK4
(`EncodeCopyRgbaCropInverseOrient`). VK2 (the parameter arena and the node value cache of the
workspace). VK3 (`color_encoding_math.h` in Slang, `math.slangh`).

**Source audit of the VK7 entry (2026-11-27).**

| OpenCL entry | Kernels and shared code | VK7 decision |
| --- | --- | --- |
| `ExecuteOpenClGeometryResample` (`opencl_develop_pass.cpp`): alias of `develop.sensor_linear` when `encode_geometry_resample` is false, else one resample into `geometry.scene_source` | `geometry_resample_rgba32f` (`geometry_camera.cl`): bilinear or bicubic (Catmull-Rom weights, normalized) sample at `render_to_decoded` of each texel center, border color outside | VK7. The bilinear and bicubic sample functions move to `geometry_sample.slangh`, shared with the DNG warp kernel of VK5 (same code). |
| `ExecuteOpenClCameraColor`: the `camera_to_ap1` matrix in the parameter arena (slot `camera_color`), the DNG profile tables in the node value cache (`UploadDngProfileGpuData`) | `camera_color_acescc`: matrix, `DngApplyColorProfile` (`dng_profile_gpu_math.h`), `AcesReferenceGamutCompress` (`aces_reference_gamut_compression.h`), `CeAcesccEncode` | VK7. The two headers get a `__SLANG__` branch. |
| Raster branch of `ExecuteOpenClDevelop`: upload at native depth, `LinearizeRaster` to RGBA32F, crop and orientation | `linearize_raster_rgba` (`display_to_ap1.cl`), `RlLinearize` (`raster_linearize_math.h`) | VK7. U8 and U16 samples are read from 32-bit words (the `LoadU16` rule of VK4: no 8-bit or 16-bit storage feature). |
| `ExecuteOpenClDisplayToAp1`: the block of `ResolveDisplayToAp1Block` in the parameter arena (slot `display_to_ap1`) | `display_to_ap1_acescc`, `display_to_ap0_linear` (only the OpenColorIO test calls the second), `D2aSourceToAcesccAp1` (`display_to_ap1_math.h`), the ACES 2.0 inverse of `aces2_reference_math.h` | VK7, both kernels. The two headers get a `__SLANG__` branch. |

**Modules, files, and APIs.**

- `edit/runtime/vulkan/shader/geometry_camera.slang` (group `geometry_camera`: kernels
  `geometry_resample_rgba32f`, `camera_color_acescc`) and `geometry_sample.slangh`;
  `edit/runtime/vulkan/shader/display_to_ap1.slang` (group `display_to_ap1`: kernels
  `linearize_raster_rgba`, `display_to_ap1_acescc`, `display_to_ap0_linear`).
- `include/vulkan/vulkan_geometry_camera_kernels.hpp`, `include/vulkan/vulkan_display_to_ap1_kernels.hpp`:
  names, bindings, push constant layouts; the descriptors go into `VulkanKernelCatalog()`.
- `inc/edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp`, `edit/runtime/vulkan/vulkan_geometry_camera_pass.cpp`
  (target `EditRuntimeVulkanPasses`): `ExecuteVulkanGeometryResample(VulkanRenderDevice&, const ExecutionPlan&)`,
  `ExecuteVulkanCameraColor(VulkanRenderDevice&, const ExecutionPlan&, const PipelineDocument&)`.
- `inc/edit/runtime/vulkan/vulkan_display_to_ap1_pass.hpp`, `edit/runtime/vulkan/vulkan_display_to_ap1_pass.cpp`:
  `ExecuteVulkanDisplayToAp1(...)`, and the encode functions `EncodeVulkanLinearizeRaster(...)`
  and `EncodeVulkanDisplayToAp1(..., DisplayToAp1Output)` that the Develop raster branch and the
  tests call.
- `vulkan_pass_encoder.hpp`: the three new `PassEncoder` specializations.
- `math.slangh`: `AlcedoLog10` for `A2R_LOG10`.
- Test: `tests/edit/runtime/vulkan_geometry_color_test.cpp` (`VulkanGeometryColorTest`), built
  when Vulkan and OpenCL or Metal are enabled (the rule of `VulkanDevelopTest`).

**Data rules and invariants.**

- A pass reads the shared parameters by device address: the parameter arena buffer of the
  workspace plus the binding offset (`camera_color`, `display_to_ap1`), the DNG profile tables of
  the node value cache. The arena and the cache keep the buffers alive past the submission
  (VK2 rule), as in OpenCL.
- `geometry.scene_source` is an alias of `develop.sensor_linear` when the plan has no resample; the
  pass then records nothing.
- The raster branch scratch (the upload at native depth, the linearize parameters, the RGBA32F
  plane) is a set of `VulkanBackend::Buffer` objects of the call (VK6 rule).
- Every pass records into the open recording and submits nothing.
- A missing input image, a call outside `BeginRender`, a document without a Develop node or
  without a raster input (DisplayToAp1), an unresolved color transform (CameraToAp1), or a raster
  plane that is not tightly packed fails with `std::runtime_error` and the message of the OpenCL
  pass before a command is recorded.
- Kernel math: `pow`, `log10`, `sin`, `cos`, `atan2` of the shared headers map to the
  `math.slangh` functions in the `__SLANG__` branch (the VK3 and VK5 rules: GLSL.std.450 is less
  precise than OpenCL).

**Implementation steps.**

1. `__SLANG__` branches of the five headers; `AlcedoLog10`.
2. The two kernel groups and their headers; the catalog.
3. The pass functions, the encode functions, the raster branch, the pass encoders.
4. `VulkanGeometryColorTest`.
5. Runs: Apple M4 (MoltenVK). The Windows runs against OpenCL wait until the Windows computer is
   free (section 11).

**Tests and evidence.**

- `VulkanGeometryColorTest.GeometryResampleEqualsTheHostSampleAndTheReferenceBackend` (crop,
  rotation, viewport, and scale of the OpenCL geometry test; bilinear and bicubic)
- `VulkanGeometryColorTest.GeometryWithoutResampleAliasesTheSensorImage`
- `VulkanGeometryColorTest.CameraColorWithADcpProfileMatchesTheScalarProfile` (the synthetic DCP
  of `VerifyDcpCameraProfile` on its CI ARW: `ExpectCameraPassMatchesScalarProfile`, 3e-4)
- `VulkanGeometryColorTest.RawDevelopImageEqualsTheReferenceBackend` (`raw/street/_DSC0155.NEF`,
  Develop, GeometryResample, CameraToAp1)
- `VulkanGeometryColorTest.RasterDevelopOutputMatchesTheHostEvaluationForEachRasterKind` (the
  eight fixtures of the Metal and OpenCL raster tests, `ExpectAcesccNear`, 3e-4) and against the
  reference backend
- `VulkanGeometryColorTest.Aces2InverseMatchesOcioCpuProcessor` and
  `AcesccOutputMatchesHostEvaluationForBothBranches` (the OpenCL DisplayToAp1 test cases and
  tolerances)
- `VulkanGeometryColorTest.PassesRecordIntoTheOpenRecordingWithoutASubmission`
- `VulkanGeometryColorTest.InvalidRequestsFailBeforeACommand`

**Build and run commands.**

```text
macOS:   source ~/Projects/vulkan_tools/env.sh
         cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanGeometryColorTest
               VulkanDevelopTest alcedo_gpu_check
         ctest --test-dir build/macos-vulkan -R "Vulkan" -j 1 --output-on-failure
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanGeometryColorTest
         ctest --test-dir build/debug -R "VulkanGeometryColorTest" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The listed tests pass on the Apple M4 with no validation error.
- [x] The listed tests pass on both Windows devices against OpenCL, or the record names the runs
      that did not take place. (RTX 3080 Laptop GPU and Iris Xe: 9 of 9.)
- [x] The OpenCL, CUDA, and Metal results are unchanged: the shared headers keep their OpenCL,
      CUDA, Metal, and host expansion (record).

**Expected diff.** 1100 to 1500 lines.

##### Phase VK7 completion record (2026-11-27)

**Status:** complete on the Apple M4 (MoltenVK), the RTX 3080 Laptop GPU, and Iris Xe. Issue
#1211. Branch `feature/vulkan-vk7-geometry-camera-raster` on the VK6 branch
`feature/vulkan-vk6-develop-pass` (pull request #1210). The work was done on the Mac (user
request); the Windows build and runs followed the same day when the Windows computer was free
(user request), on the committed branch.

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Pointers of the shared headers | A `__SLANG__` branch | Each header names its table pointer with one macro (`DNG_CONST_FLOAT_PTR`, `RL_CONST_FLOAT_PTR`, `A2R_CONST_FLOAT_PTR`, `D2A_CONST_FLOAT_PTR`) in place of `X_GLOBAL const float*`. The expansion for OpenCL, Metal, CUDA, and the host is the old text; Slang gets `float*`. `dng_profile_gpu_math.h` maps `unsigned` to `uint` in its Slang branch and removes the mapping at its end; its `uint` conditions are `!= 0`. | slangc rejects `const T*` (error 20017: use `Ptr<T, Access.Read>`) and has no `unsigned`. A macro of the whole type keeps one text for every backend. |
| ACES 2.0 inverse math | `math.slangh` functions | `AlcedoPow`, `AlcedoAtan2`, `AlcedoCos`, `AlcedoSin`, and a new `AlcedoLog10` (split `log10 2`, exact for every exponent) | The VK3 and VK5 rules. |
| Bilinear and bicubic sample | Shared with the DNG warp | `geometry_sample.slangh` takes the image as a parameter; `dng_warp.slang` uses it. | One code; the VK6 warp results did not change (below). |
| Arena address | Not in the plan | `ParameterSlotAddress` (`vulkan_parameter_address.hpp`): the device address of a slot after a range check. `EncodeVulkanDisplayToAp1` takes the parameter buffer and an offset, so the pass (arena) and the tests (own buffer) call one function. | The arena buffer is `const` for a reader; a `VulkanBufferView` needs a mutable buffer. |
| Reference comparison helpers | Not in the plan | The reference backend wrappers and `Compare` of `VulkanDevelopTest` moved to `tests/edit/runtime/vulkan_reference_backend_support.hpp` with the geometry, camera, and DisplayToAp1 wrappers. | Two test files use them. |
| Camera color comparison | A tolerance | Both backends run the camera pass on the `geometry.scene_source` of Vulkan. Bound: rms 1e-5; each value that differs by more than 1e-4 must equal the host evaluation within 3e-4 (the scalar tolerance of `ExpectCameraPassMatchesScalarProfile`). | 3 of 56 million values differ by up to 0.039 in ACEScc (one texel near black, linear 5e-4 to 8e-4 after the HueSatMap); Vulkan equals the host value bit for bit there and Metal (fast math) leaves it. |
| RAW fixture | `raw/street/_DSC0155.NEF` | The CI Sony ARW of `VerifyDcpCameraProfile` for the camera test and the end-to-end RAW test | It is in the repository (LFS) on every computer and has an imported camera profile; the NEF test RAW is a local copy. |
| VK6 test of the raster input | Fails with VK7 | `VulkanDevelopTest.NeuralEngineAndRasterPixelsWithoutRasterInputFail`: raster pixels of a document without a raster input object fail with the message of `RequireRasterInput` | The raster branch runs from this phase. |

**Measured differences on the Apple M4** (logs `build/tmp/vulkan_backend/vk7/` of the Mac
worktree; relative difference `|vulkan - reference| / max(1, |reference|)`):

| Case | Against | Largest | rms | Tolerance |
| --- | --- | --- | --- | --- |
| Geometry bilinear (crop, 15 degrees, viewport) | host sample | 1.2e-7 | | 1.5e-4 (OpenCL test) |
| Geometry bicubic | host sample | 0 | | 1.5e-4 |
| Geometry bilinear; bicubic | Metal | 2.9e-6; 4.3e-6 | 2.8e-7; 3.5e-7 | 1e-5 |
| Camera color, synthetic DCP, CI ARW (14.1 MP) | host, sparse grid | in 3e-4 | | 3e-4 |
| Camera color, the same input on both | Metal | 0.039 (3 values above 1e-4, equal to the host on Vulkan) | 5.3e-6 | rms 1e-5, the host rule |
| `develop.image` of the CI ARW, the whole chain | Metal | 0.0106 | 5.0e-6 | rms 2.5e-4 (VK6 demosaic rule) |
| Raster fixtures (8), `develop.image` | host evaluation | in tolerance | | `ExpectAcesccNear` 3e-4 |
| Raster fixtures (8) | Metal | 4.5e-4 (HLG TIFF), 8e-5 or less for the others | 1.6e-5 or less | `ExpectAcesccNear` 3e-4 |
| ACES 2.0 inverse, 5 displays, 33^3 grid | OpenColorIO 2.5.1 | `ExpectMatchesOcio` | | the OpenCL test rule |
| DisplayToAp1 ACEScc, display and scene branch | host evaluation | in 2e-4 | | 2e-4 (OpenCL test) |
| VK6 `VulkanDevelopTest` (after the warp refactor) | Metal | the VK6 values, unchanged | | VK6 |

**Primary success call chain:**

```text
PlanExecutor<VulkanBackend> (VK17) or VulkanGeometryColorTest
  -> PassEncoder<VulkanBackend, UploadRgb>::Encode -> ExecuteVulkanDevelop
       -> RasterRgb: RequireRasterInput, RequireRasterPixelsMatchDescription, packed check
       -> EncodeRasterInput: PackRasterLinearize -> UploadBufferRange (pixels, parameters)
          -> EncodeVulkanLinearizeRaster -> EncodeCopyRgbaCropInverseOrient (identity scale)
  -> PassEncoder<VulkanBackend, GeometryResample>::Encode -> ExecuteVulkanGeometryResample
       -> no resample: AliasImageFrom(geometry.scene_source, develop.sensor_linear)
       -> else AcquireImageForWrite -> RecordKernelDispatch(geometry_resample_rgba32f)
  -> PassEncoder<VulkanBackend, CameraToAp1>::Encode -> ExecuteVulkanCameraColor
       -> ResolveDevelopColorTransform -> arena slot camera_color -> UploadDirty
       -> PackDngProfileGpuData -> UploadDngProfileGpuData (node value cache)
       -> RecordKernelDispatch(camera_color_acescc) into develop.image
  -> or PassEncoder<VulkanBackend, DisplayToAp1>::Encode -> ExecuteVulkanDisplayToAp1
       -> ResolveDisplayToAp1Block -> arena slot display_to_ap1 -> UploadDirty
       -> EncodeVulkanDisplayToAp1(AcesccAp1) into develop.image
  -> nothing is submitted; EndRender submits the recording
```

**Primary failure call chain:**

```text
render not open, no develop.sensor_linear, no geometry.scene_source, no Develop node, an
unresolved color transform, a document without a raster input, raster pixels that do not match
the description or are not tightly packed
  -> std::runtime_error "ExecuteVulkan...: ..." before an allocation or a command
a parameter block outside its buffer, a texture that is not RGBA32F, a CFA format for the raster
linearization
  -> std::invalid_argument from the encode function; nothing is recorded
```

**What was proven (executed tests, Apple M4, MoltenVK 1.4.2, validation layer 1.4.363):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `GeometryResampleEqualsTheHostSampleAndTheReferenceBackend` | `VulkanGeometryColorTest` | PASS |
| `GeometryWithoutResampleAliasesTheSensorImage` | `VulkanGeometryColorTest` | PASS |
| `CameraColorWithADcpProfileMatchesTheScalarProfile` | `VulkanGeometryColorTest` | PASS |
| `RawDevelopImageEqualsTheReferenceBackend` | `VulkanGeometryColorTest` | PASS |
| `RasterDevelopOutputMatchesTheHostEvaluationForEachRasterKind` | `VulkanGeometryColorTest` | PASS |
| `Aces2InverseMatchesOcioCpuProcessor`, `AcesccOutputMatchesHostEvaluationForBothBranches` | `VulkanGeometryColorTest` | PASS |
| `PassesRecordIntoTheOpenRecordingWithoutASubmission` (0 submissions and waits, then 1 submission) | `VulkanGeometryColorTest` | PASS |
| `InvalidRequestsFailBeforeACommand` (no buffer, no texture, no recording) | `VulkanGeometryColorTest` | PASS |
| VK0 to VK6 suites and VK7 | `ctest -R "Vulkan\|PipelineAccelerator"` | 95 tests, 93 PASS, 2 skipped (Windows only) |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | PASS: `gpu.kernels.ok=vulkan kernels=32` |
| Validation errors | `TearDown` of each GPU test | 0 |
| Metal and host builds of the changed headers | Metal libraries `camera_color`, `display_to_ap1`, `drt` rebuilt; host evaluation in the tests | the Metal results of the VK6 and VK7 comparisons are the ones above |
| Windows: MSVC build of the Vulkan targets, the OpenCL tests, and the CUDA tests of the changed headers | `scripts\msvc_env.cmd --build build\debug` | PASS, no warning |

Commands:

```text
macOS:  source ~/Projects/vulkan_tools/env.sh
        cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanRequirementsTest
              VulkanContextTest VulkanShaderModuleRegistryTest VulkanComputePipelineCacheTest
              GpuDagVulkanWorkspaceTest PipelineAcceleratorTest VulkanDevelopTest
              VulkanGeometryColorTest alcedo_gpu_check
        ctest --test-dir build/macos-vulkan -R "Vulkan|PipelineAccelerator" -j 1     95: 93 PASS, 2 skipped
        build/macos-vulkan/.../alcedo-gpu-check --vulkan                            exit 0
```

**Finding outside this phase.** As in VK6, the product Metal kernels with fast math leave the host
value where the result is sensitive: one CI ARW texel near black differs by 0.039 in ACEScc after
the HueSatMap of a DCP profile. VK7 changes nothing in Metal.

**LOC note:** about 2080 added and 300 removed lines without the plan: the kernels 267 (and 40
in `dng_warp.slang`), the passes and kernel headers about 720, the shared header changes about
250 (mostly the macro rename and the alignment of clang-format), CMake and the catalog 52, the
tests 884 (the new test 663, the moved helpers 221; 150 lines removed from `VulkanDevelopTest`).
Above the estimate of 1100 to 1500: the shared header changes and the moved test helpers were not
in it.

**Windows runs (2026-11-27):** `VulkanGeometryColorTest` against OpenCL on the same GPU
(`ALCEDO_VULKAN_DEVICE=RTX` or `Iris`, validation layer on, 0 validation errors):

| Case | RTX 3080 Laptop GPU | Iris Xe |
| --- | --- | --- |
| Geometry bilinear: host; OpenCL | 2.9e-6; equal | 2.9e-6; equal |
| Geometry bicubic: host; OpenCL | 4.4e-6; 1.8e-7 | 4.4e-6; 1.8e-7 |
| Camera color with a DCP profile, the same input on both | 4.8e-7, rms 3.1e-8, 0 values above 1e-4 | 4.8e-7, rms 3.3e-8, 0 above 1e-4 |
| `develop.image` of the CI ARW, the whole chain | 4.2e-7, rms 2.9e-8 | 6.3e-7, rms 2.9e-8 |
| Raster fixtures (8) against OpenCL | 9.0e-5 or less | 7.5e-4 (HLG TIFF), 7.9e-5 or less for the others |
| Result | 9 of 9 PASS | 9 of 9 PASS |

With OpenCL as the reference the camera color differences are 4.8e-7: the 0.039 of the Apple M4 is
the fast math of the product Metal kernel. The suites: RTX 3080, 131 tests, 130 PASS, 1 skipped (a
test for a build without Vulkan); Iris Xe, 130 PASS and `VulkanDevelopTest.BayerRawEqualsTheReferenceBackend`
FAIL, a VK4 RCD finding on a clipped NEF (#1213, VK6 record; accepted, the Intel bound of the VK6
record makes it pass). `alcedo-gpu-check --vulkan`:
`gpu.kernels.ok=vulkan kernels=32` on both devices.

The OpenCL and CUDA tests of the changed headers (OpenCL on Iris Xe, the device of the OpenCL
test default): `GpuDagOpenClDisplayToAp1Test` 4 of 4, `GpuDagOpenClRasterDevelopTest` 4 of 4,
`GpuDagCudaDisplayToAp1Test` 5 of 5, `GpuDagCudaRasterDevelopTest` 9 of 9, `GpuDagOpenClDevelopTest`
(DCP, Canon DNG, Sony, geometry, converted DNG) 5 of 6. The failure
`OpenClDevelopFixture.DcpProfileMatchesScalarReference` (display change 0.004808, limit 0.005) is
#972: the binary of the main checkout without VK7 gives the same value.

A stale binary first failed `EachCatalogKernelHasAPipeline` (the old kernel catalog): a phase that
adds kernels rebuilds every Vulkan test target, not only its own.

**Remaining gaps:** none.

### Phase VK8: primary grade point kernels and LUT

**Objective and deliverables.** The Vulkan backend encodes the point stages of each Color Grade
in the order of the OpenCL pass: `PassEncoder<VulkanBackend, PrimaryColorGrade>` through the
shared `GradeExecutor`, with the White Balance, Exposure, Contrast, Whites, Blacks, Curve (RGB and
channel curves), HLS, Saturation, Vibrance, color wheel, and LMT LUT adjustments, the mix with the
Grade input, the Grade mask, and the curve input histogram (#888). LUT tables come from a LUT cache
of `VulkanBackend` by content key. The Grade neighborhood stage belongs to VK9 and the Local
Laplacian stage (Shadows, Highlights) to VK10; in this phase they stop the render with a message
that names their phase. Issue #1219.

**Inputs and prerequisites.** VK7 (`develop.image`). VK2 (the parameter arena, the node value
cache, the scene-work members as RGBA32F buffers). VK3 (`color_encoding_math.h` in Slang,
`oklab.slangh`, `math.slangh`).

**Source audit of the VK8 entry (2026-11-27).**

| OpenCL entry | Kernels and shared code | VK8 decision |
| --- | --- | --- |
| `OpenClGradeOps::DispatchPointwise`, `DispatchPointwiseWithMix` (`opencl_primary_grade_pass.cpp`) | `primary_grade_pointwise_scene_rgba32f` (`primary_grade.cl`): the records of the fused command list on a scene image (cached image or scene-work buffer), then the mix with the Grade input times the R8 mask | VK8 |
| `OpenClGradeOps::DispatchCurveInputHistogram` | `primary_grade_curve_input_histogram`: the records before the Curve on a sample grid, 768 bins in local memory, one atomic add for each bin of a workgroup | VK8 |
| `OpenClGradeOps::UploadFusedCommands`, `LoadLut` | the node value `runtime.order`, `TryPackGradeLut`, `OpenClBackend::AcquireLut` | VK8: the same upload rule; `VulkanBackend::AcquireLut` |
| `EnqueueGradePointwise`, `EnqueueGradeMix` | `primary_grade_pointwise_rgba32f`, `primary_grade_mix_rgba32f`, `primary_grade_mix_masked_rgba32f` | Out of the port: the three kernels have no product caller; `WarmUpPlan` warms them in place of the two dispatched kernels (#1220) |
| `OpenClGradeOps::DispatchHorizontal`, `DispatchVerticalApply` | `primary_grade_neighbor.cl` | VK9 |
| `OpenClGradeOps::ExecuteLocalTone` | `local_tone.cl` | VK10 |

**Modules, files, and APIs.**

- `edit/runtime/vulkan/shader/primary_grade.slang` (group `primary_grade`: kernels
  `primary_grade_pointwise_scene_rgba32f`, `primary_grade_curve_input_histogram`).
- `include/vulkan/vulkan_primary_grade_kernels.hpp`: names, bindings, push constant layouts; the
  descriptors go into `VulkanKernelCatalog()`.
- `inc/edit/runtime/vulkan/vulkan_scene_view.hpp`: `VulkanSceneView` and `VulkanBindScene`, the
  Vulkan form of `OpenClSceneView` for the Grade and the later scene passes.
- `inc/edit/runtime/vulkan/vulkan_primary_grade_pass.hpp`, `edit/runtime/vulkan/vulkan_primary_grade_pass.cpp`
  (target `EditRuntimeVulkanPasses`): `VulkanPrimaryGradeResult`, the two `ExecuteVulkanPrimaryGrade`
  overloads of the OpenCL pass, `VulkanGradeOps`.
- `VulkanBackend`: `AcquireLut`, `DummyLut`, `SetLutByteBudget`, `LutCacheBytes`,
  `LutCacheEntryCount`, `LutUploadBytes`, `LastLutResourceId`, `DummyStorageImage`,
  `GradeCommandTopologyHash`, and the struct `VulkanLutBinding`.
- `vulkan_pass_encoder.hpp`: the `PrimaryColorGrade` specialization.
- Test: `tests/edit/runtime/vulkan_primary_grade_test.cpp` (`VulkanPrimaryGradeTest`); the reference
  backend support gets `ExecuteReferencePrimaryGrade` and `DownloadScene`.

**Data rules and invariants.**

- A scene image is a cached RGBA32F storage image or a scene-work member (an RGBA32F row-major
  buffer). The kernel reads the buffer when its address in the push constants is set, else the
  storage image of its binding. A binding that the dispatch leaves unread holds the 1x1 dummy image
  of its format (RGBA32F or R8).
- The Grade writes a scene-work member (`DestinationWorkMember`), as in OpenCL.
- The adjustment records (`GradeAdjustmentParams`, 208 bytes) and the command offsets are read by
  address arithmetic: the parameter arena address plus the slot offset (VK4 rule for Intel).
- LUT cache: an entry is the buffer of one `PackedGradeLut` for its content key and edge, with the
  memory priority 0.2 (section 4.2). A miss removes the least recently used entries until the table
  fits in the byte budget (64 MiB, the OpenCL value). An entry that leaves the cache returns its
  buffer by the lifetime rule of `VulkanBackend`, so a recording that uses it stays valid.
- Every pass records into the open recording and submits nothing.
- Kernel math: `exp2`, `log2`, `pow`, `sin`, `cos` map to the `math.slangh` functions (VK3, VK5
  rules); `tanh` of the Contrast is `(1 - e^(-2|x|)) / (1 + e^(-2|x|))` with `AlcedoExp`; the LUT
  interpolation is `a + (b - a) t`, the OpenCL `mix`.

**Implementation steps.**

1. The kernel group and its header; the catalog.
2. The LUT cache, the dummy images, and the command topology hash of `VulkanBackend`.
3. `VulkanSceneView`, the pass, the pass encoder.
4. `VulkanPrimaryGradeTest`.
5. Runs: RTX 3080 Laptop GPU and Iris Xe against OpenCL; Apple M4 (MoltenVK) against Metal.

**Tests and evidence.**

- `VulkanPrimaryGradeTest.EachPointAdjustmentEqualsTheReferenceBackend` (16 cases: each point
  adjustment alone, the LUT at strength 1 and 0.5, and all together)
- `VulkanPrimaryGradeTest.GradeMixAndMaskEqualTheReferenceBackend`
- `VulkanPrimaryGradeTest.ThreeGradesComposeLikeTheReferenceBackend`
- `VulkanPrimaryGradeTest.DisabledAndZeroMixGradesAliasTheirInput`
- `VulkanPrimaryGradeTest.CurveInputHistogramCountsTheGradeInputOnTheCurveAxis`
- `VulkanPrimaryGradeTest.LutResourceIsReusedByContentKey`
- `VulkanPrimaryGradeTest.StableGradeUploadsNoCommandsAndCreatesNoResource`
- `VulkanPrimaryGradeTest.GradeRecordsIntoTheOpenRecordingWithoutASubmission`
- `VulkanPrimaryGradeTest.InvalidRequestsAndLaterStagesFail`

**Build and run commands.**

```text
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanPrimaryGradeTest
         set ALCEDO_VULKAN_DEVICE=RTX (or Iris)
         ctest --test-dir build/debug -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
macOS:   source ~/Projects/vulkan_tools/env.sh
         cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanPrimaryGradeTest
         ctest --test-dir build/macos-vulkan -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The listed tests pass on the RTX 3080 Laptop GPU and Iris Xe against OpenCL with no
      validation error.
- [x] The listed tests pass on the Apple M4 against Metal with no validation error.
- [x] The OpenCL, CUDA, and Metal results are unchanged: the phase changes no shared header and no
      source of those backends.

**Expected diff.** 1400 to 1800 lines.

##### Phase VK8 completion record (2026-11-27)

**Status:** complete on the RTX 3080 Laptop GPU, Iris Xe, and the Apple M4 (MoltenVK). Issue
#1219. Branch `feature/vulkan-vk8-primary-grade` on the VK7 branch
`feature/vulkan-vk7-geometry-camera-raster` (pull request #1212).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Scene images | Not in the plan | `VulkanSceneView` holds the storage image of a cached image or the device address of a scene-work buffer; the kernel selects the buffer by a nonzero address. Unread bindings hold `VulkanBackend::DummyStorageImage` of their format. | `RecordKernelDispatch` binds one texture for each binding; the address in the push constants replaces the OpenCL `is_buffer` argument. One dummy of each format serves every unread binding (Vulkan reads a storage image twice in one dispatch without a hazard). |
| Destination | The OpenCL kernel writes an image or a buffer | The destination is a scene-work buffer; another destination stops the pass with `std::runtime_error`. | `GradeExecutor` writes only scene-work members. |
| LUT cache eviction | The OpenCL rule skips entries of unfinished submissions | The least recently used entry leaves the cache at once. | The lifetime rule of `VulkanBackend` keeps a returned buffer until its last batch completes. |
| LUT cache priority | 0.2 (section 4.2) | `CreateBufferWithPriority(bytes, kCachePriority)`; the other allocations keep 0.8. | Section 4.2. |
| Contrast `tanh` | `math.slangh` functions | A local `AlcedoTanhLocal` from `AlcedoExp` in `primary_grade.slang` | Only the Contrast reads `tanh`; the function moves to `math.slangh` when a second kernel needs it. |
| Neighborhood and Local Laplacian stages | VK9, VK10 | `VulkanGradeOps` stops the render with `std::runtime_error` that names phase VK9 or VK10. | `GradeExecutor` needs the operations; the message names the missing phase. The default Grade has its neighborhood adjustments in the DRT finish (Sharpen, Clarity, Halation), so the test covers the VK10 message. |
| Mask input of the tests | MaskEvaluate | The test writes a synthetic R8 mask into the mask output of the Grade on both backends. | The Vulkan MaskEvaluate pass is VK11. |

**Measured differences** (logs `build/tmp/vulkan_backend/vk8/`; relative difference
`|vulkan - reference| / max(1, |reference|)` in ACEScc; the reference backend runs on the same GPU;
input 64 x 48, scene-linear 2^-7 to 2^2 with saturated and neutral texels, through Develop,
GeometryResample, and CameraToAp1 of each backend):

| Case | RTX 3080 (OpenCL): largest, rms | Iris Xe (OpenCL): largest, rms | Apple M4 (Metal): largest, rms | Tolerance |
| --- | --- | --- | --- | --- |
| White balance 4300 K | 3.3e-7, 5.8e-8 | 3.6e-7, 5.9e-8 | 2.8e-7, 3.2e-8 | 2e-4 |
| Exposure +1.3 | 2.5e-7, 3.6e-8 | 1.9e-7, 3.3e-8 | 2.4e-7, 2.4e-8 | 2e-4 |
| Contrast +100 | 6.1e-6, 1.8e-7 | 1.3e-5, 2.3e-7 | 7.9e-6, 2.2e-7 | 2e-4 |
| Contrast -40 | 5.4e-7, 7.3e-8 | 6.0e-7, 7.3e-8 | 5.7e-7, 5.7e-8 | 2e-4 |
| Whites +60; Whites -70; Blacks -45 | 3.6e-7; 2.4e-7; 3.0e-7 | 2.4e-7; 1.9e-7; 2.1e-7 | 3.3e-7; 2.4e-7; 2.5e-7 | 2e-4 |
| Curve, 16 points; RGB with red and blue | 3.9e-7; 3.9e-7 | 3.3e-7; 5.1e-7 | 3.7e-7; 5.8e-7 | 1e-5 |
| HLS | 3.0e-7, 3.9e-8 | 2.7e-7, 3.7e-8 | 3.3e-7, 3.0e-8 | 2e-4 |
| Saturation 1.6; Vibrance +50 | 2.5e-7; 2.7e-7 | 2.7e-7; 2.5e-7 | 3.6e-7; 3.0e-7 | 2e-4 |
| Color wheels | 3.3e-7, 3.9e-8 | 2.7e-7, 4.5e-8 | 2.8e-7, 4.3e-8 | 2e-4 |
| LUT 17^3, strength 1; 0.5 | 3.1e-7; 2.8e-7 | 3.0e-7; 2.5e-7 | 3.2e-7; 2.7e-7 | 2e-4 |
| Every point adjustment | 1.0e-5, 3.8e-7 | 9.0e-6, 3.7e-7 | 2.6e-5, 4.6e-7 | 2e-4 |
| Mix 0.6 | 2.0e-5, 2.2e-7 | 3.1e-5, 3.8e-7 | 1.7e-5, 2.1e-7 | 2e-4 |
| Mix 0.8 with an R8 mask | 3.0e-6, 1.0e-7 | 2.9e-5, 2.8e-7 | 1.6e-5, 1.8e-7 | 2e-4 |
| Three Grades (Exposure; Contrast at mix 0.5; Saturation) | 3.4e-6, 1.2e-7 | 4.8e-6, 1.3e-7 | 6.7e-6, 1.3e-7 | 2e-4 |
| Curve input histogram, 640 x 480, neutral; Exposure +0.7 | host counts, equal; in the bin edge margin | equal; in the margin | equal; in the margin | `curve_input_histogram_test_support.hpp` |

The largest differences are at near-black texels after the OkLab round trip of the Contrast, where
the float32 cube root and the ACEScc encode amplify one ulp.

**Primary success call chain:**

```text
PlanExecutor<VulkanBackend> (VK17) or VulkanPrimaryGradeTest
  -> PassEncoder<VulkanBackend, PrimaryColorGrade>::Encode -> ExecuteVulkanPrimaryGrade
       -> GradeExecutor<VulkanGradeOps>::Execute
            -> BindAndScheduleGrade -> ParameterArena::UploadDirty
            -> (histogram) RecordCurveInputHistogram -> VulkanGradeOps::DispatchCurveInputHistogram
                 -> RecordKernelDispatch(primary_grade_curve_input_histogram)
            -> UploadFusedCommands (node value runtime.order, when the topology hash changes)
            -> LoadLut -> TryPackGradeLut -> VulkanBackend::AcquireLut (cache hit, or upload)
            -> for each point stage: DispatchPointwise or DispatchPointwiseWithMix
                 -> EncodePointwise -> VulkanBindScene (source, destination, mix source), mask
                 -> RecordKernelDispatch(primary_grade_pointwise_scene_rgba32f) into a scene-work member
       -> the output binding of the last Grade
  -> nothing is submitted; EndRender submits the recording
```

**Primary failure call chain:**

```text
render not open, a plan without a Color Grade, a missing Grade node
  -> std::runtime_error "ExecuteVulkanPrimaryGrade: ..." before a command
a mask output that is missing or is not an R8 image of the Grade size, a scene image that is
missing or is not RGBA32F, a missing command buffer
  -> std::runtime_error before the dispatch of that stage; the caller cancels the render
a neighborhood stage, a Local Laplacian stage
  -> std::runtime_error "... (plan phase VK9, #1036)" or "(plan phase VK10, #1036)"
```

**What was proven (executed tests, validation layer on):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| The nine tests of the section | `VulkanPrimaryGradeTest` | PASS on RTX 3080, Iris Xe, Apple M4 |
| VK0 to VK8 suites, Windows | `ctest -R "Vulkan\|PipelineAccelerator"` | RTX 3080: 140 tests, 139 PASS, 1 skipped (a test for a build without Vulkan); Iris Xe: the same |
| VK0 to VK8 suites, Apple M4 | `ctest -R "Vulkan\|PipelineAccelerator"` | 104 tests, 102 PASS, 2 skipped (Windows only); `VulkanDevelopTest` needs the three test RAWs, copied for the run |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | `gpu.kernels.ok=vulkan kernels=34` on the three devices |
| Validation errors | `TearDown` of each GPU test | 0 |
| Windows MSVC build of every Vulkan target | `scripts\msvc_env.cmd --build build\debug` | PASS, no warning |

**Finding outside this phase.** Three OpenCL Grade kernels have no product caller, and
`OpenClBackend::WarmUpPlan` warms them in place of `primary_grade_pointwise_scene_rgba32f` and
`primary_grade_curve_input_histogram` (#1220).

**LOC note:** about 1900 added and 6 removed lines without the plan: the kernels 407, the pass,
the scene view, the kernel header, and the pass encoder 612, the backend LUT cache and dummies 174,
CMake and the catalog 14, the tests 695 (the new test 634, the reference support 41, CMake 20).
Above the estimate of 1400 to 1800: the test file holds sixteen comparison cases and the LUT cache
tests.

**Remaining gaps:** none.

### Phase VK9: neighborhood kernels (Sharpen, Clarity, Halation)

**Objective and deliverables.** The Vulkan backend has the two neighborhood kernels of
`primary_grade_neighbor.cl` that the product dispatches and the operations of the shared
`NeighborExecutor` (`VulkanNeighborOps`): the horizontal blur into a scratch image, then the
vertical blur in workgroup memory with Sharpen, Clarity, or Halation and the optional masked mix.
`VulkanGradeOps` takes these operations for a Grade neighborhood stage, and the DRT finish of VK12
takes them for Clarity, Sharpen, and Halation. Issue #1225.

**Inputs and prerequisites.** VK8 (`VulkanSceneView`, the dummy storage images, the scene-work
members as RGBA32F buffers). VK3 (`math.slangh`). VK2 (the texture pool and the node value cache of
the workspace).

**Source audit of the VK9 entry (2026-11-27).**

| OpenCL entry | Kernels and shared code | Callers | VK9 decision |
| --- | --- | --- | --- |
| `EnqueueOpenClNeighborHorizontalScene` (`opencl_neighbor_dispatch.hpp`) | `primary_grade_neighbor_blur_h_scene_rgba32f`: the Gaussian weights of `GradeNeighborParams`, or the exponential window of the Halation on `sigma_x`; the source clamped at the image edge | `OpenClGradeOps::DispatchHorizontal`, `OpenClDrtOps::DispatchHorizontal` | VK9 |
| `EnqueueOpenClNeighborVerticalScene` | `primary_grade_neighbor_apply_v_scene_rgba32f`: an 8 x (8 + 2 radius) RGBA tile of the horizontal result in local memory, Sharpen (threshold on the high-pass luma), Clarity (edge protection and midtone weight), Halation (spill times the redshift), the mix with the mix source times the R8 mask | the same two | VK9 |
| `EnqueueOpenClNeighborHorizontal`, `EnqueueOpenClNeighborVertical` | `primary_grade_neighbor_blur_h_rgba32f`, `primary_grade_neighbor_apply_v_rgba32f` | none; `OpenClBackend::WarmUpPlan` only | Out of the port: no product caller (comment on #1220) |
| `GradeExecutor`, the `Neighborhood` branch | `NeighborExecutor<Ops>` | none in a product render: a Color Grade holds no neighborhood adjustment (`ColorGradeAdjustmentTypes`, `RequireAdjustmentOwner`) | `VulkanGradeOps` takes `VulkanNeighborOps`, so the Grade follows the decision of #1226 without a change |
| `DrtPostExecutor` | `NeighborExecutor<Ops>` for Clarity, Sharpen, Halation; Film Grain has its own pass | the DRT pass | VK12 (the DRT pass), with `VulkanNeighborOps` of this phase |

Metal has no Grade neighborhood kernel: `MetalGradeOps::DispatchVerticalApply` runs one point
command, and the Metal DRT finish has its own display-referred kernels (`drt_neighbor.metal`). On
the Apple M4 the reference of this phase is therefore a host evaluation of
`primary_grade_neighbor.cl` in double precision; on Windows the test compares with the OpenCL
kernels and with the host evaluation.

**Modules, files, and APIs.**

- `edit/runtime/vulkan/shader/primary_grade_neighbor.slang` (group `primary_grade_neighbor`:
  kernels `primary_grade_neighbor_blur_h_scene_rgba32f`, `primary_grade_neighbor_apply_v_scene_rgba32f`).
- `include/vulkan/vulkan_primary_grade_neighbor_kernels.hpp`: names, bindings, push constant
  layouts, `kMaxRadius`; the descriptors go into `VulkanKernelCatalog()`.
- `inc/edit/runtime/vulkan/vulkan_neighbor_dispatch.hpp`, `edit/runtime/vulkan/vulkan_neighbor_dispatch.cpp`
  (target `EditRuntimeVulkanPasses`): `VulkanNeighborOps`, `RequireVulkanNeighborhoodKernelBehavior`,
  `VulkanNeighborParamsValueId`.
- `vulkan_scene_view.hpp`: `VulkanSceneImageBinding` and `VulkanRequireSceneMask`, moved from the
  Grade pass for both passes.
- `VulkanGradeOps` derives from `VulkanNeighborOps`.
- Test: `tests/edit/runtime/vulkan_neighborhood_test.cpp` (`VulkanNeighborhoodTest`).

**Data rules and invariants.**

- `GradeNeighborParams` (300 bytes) is above the push constant limit of 256 bytes of NVIDIA and
  Intel. The horizontal dispatch uploads it into the node value `runtime.neighbor.<command index>`
  of the work owner; the vertical dispatch reads the same buffer. The buffer is created once for each
  owner and command index, so a stable render creates no buffer. The kernels read it by address
  arithmetic (VK4 rule for Intel).
- The scratch image is an RGBA32F image of the texture pool, as in OpenCL.
- A source is a cached image or a scene-work member; a destination is a scene-work member or an
  RGBA32F image (the display output of the DRT finish); the destination may be the source (each
  invocation reads its source texel before it writes the same texel).
- The mix runs when the mix is below 1 or a mask is set (the OpenCL rule).
- The workgroup tile holds the red, green, and blue of 8 x (8 + 2 x 63) texels in three float
  planes (12 864 bytes), inside the Vulkan minimum of 16 KiB of workgroup memory; the adjustments
  read only the color of the vertical blur. The vertical radius is at most 63
  (`kGradeNeighborMaxTapCount` - 1); the host refuses a larger radius and more Gaussian taps than
  the radius gives before a command.
- The Halation window uses `AlcedoExp` for the OpenCL `exp` (VK3 rule).
- Every dispatch records into the open recording and submits nothing.

**Implementation steps.**

1. The kernel group and its header; the catalog.
2. `VulkanNeighborOps` and the two scene helpers; `VulkanGradeOps` from `VulkanNeighborOps`.
3. `VulkanNeighborhoodTest` with the host evaluation.
4. Runs: RTX 3080 Laptop GPU and Iris Xe against OpenCL and the host; Apple M4 (MoltenVK) against
   the host.

**Tests and evidence.**

- `VulkanNeighborhoodTest.SharpenEqualsTheReferences` (radius 1, radius 3 with the threshold 0.02,
  radius 8 with 15 taps)
- `VulkanNeighborhoodTest.ClarityEqualsTheReferences` (+80 and -60 at radius 45, +100 on a 37 x 29
  image)
- `VulkanNeighborhoodTest.HalationEqualsTheReferences` (sigma 7, sigma 9 x 2.5, sigma 25 at radius
  63)
- `VulkanNeighborhoodTest.MixAndMaskEqualTheReferences`
- `VulkanNeighborhoodTest.EachSourceAndDestinationKindGivesTheSameResult`
- `VulkanNeighborhoodTest.StableNeighborhoodCreatesNoResourceAndRecordsWithoutASubmission`
- `VulkanNeighborhoodTest.InvalidRequestsFailBeforeACommand`

**Build and run commands.**

```text
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanNeighborhoodTest
         set ALCEDO_VULKAN_DEVICE=RTX (or Iris)
         ctest --test-dir build/debug -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
macOS:   source ~/Projects/vulkan_tools/env.sh
         cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanNeighborhoodTest
         ctest --test-dir build/macos-vulkan -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The listed tests pass on the RTX 3080 Laptop GPU and Iris Xe against OpenCL and the host
      evaluation with no validation error.
- [x] The listed tests pass on the Apple M4 against the host evaluation with no validation error.
- [x] The OpenCL, CUDA, and Metal results are unchanged: the phase changes no shared header and no
      source of those backends.

**Expected diff.** 800 to 1200 lines.

##### Phase VK9 completion record (2026-11-27)

**Status:** complete on the RTX 3080 Laptop GPU, Iris Xe, and the Apple M4 (MoltenVK). Issue
#1225. Branch `feature/vulkan-vk9-neighborhood` on the VK8 branch `feature/vulkan-vk8-primary-grade`
(pull request #1221).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Phase scope | Neighbor kernels of the primary grade | The kernels and `VulkanNeighborOps`, tested through `NeighborExecutor<VulkanNeighborOps>` | A Color Grade holds no neighborhood adjustment (#1226); the product caller is the DRT finish, whose pass is VK12. The test drives the shared executor directly. |
| Parameters | Not in the plan | A node value buffer for each owner and command index, uploaded by the horizontal dispatch | 300 bytes are above the push constant limit; one buffer for each work keeps a stable render free of resource creation. |
| Destination | A scene-work member (VK8) | A scene-work member or an RGBA32F image | The DRT finish writes its last neighborhood step into the display output. |
| Reference on the Apple M4 | Metal (VK6 to VK8) | Host evaluation of `primary_grade_neighbor.cl` | Metal has no Grade neighborhood kernel. On Windows both the host evaluation and OpenCL equal Vulkan within 1.2e-6, so the host evaluation stands for OpenCL. |
| Sharpen threshold | Not in the plan | A texel whose high-pass luma is within 1e-5 of the threshold is left out of the comparison (fewer than 1 % of the texels) | The threshold test is a step: either branch is correct within float32. |
| Kernel scope | `primary_grade_neighbor.cl` | The two scene kernels; the two image kernels stay in OpenCL | The image kernels have no product caller (comment on #1220). |
| Workgroup tile | The OpenCL tile: RGBA, 16 bytes a texel | Three float planes of the color, 12 bytes a texel | The OpenCL tile at radius 63 is 17 152 bytes, above the Vulkan minimum of 16 KiB; the adjustments read only the color, so the result is the same. |

**Measured differences** (logs `build/tmp/vulkan_backend/vk9/`; relative difference
`|vulkan - reference| / max(1, |reference|)`; 203 x 157 input from 0.01 to 0.99 with hard edges and
texel noise; OpenCL on the same GPU; tolerance 1e-5):

| Case | RTX 3080: OpenCL; host | Iris Xe: OpenCL; host | Apple M4: host |
| --- | --- | --- | --- |
| Sharpen radius 1; radius 3, threshold 0.02; radius 8 | 0; 2.4e-7 | 0; 2.4e-7 | 2.4e-7 |
| Clarity +80; -60; +100 on 37 x 29 | 0; 3.0e-7 | 1.2e-7; 3.0e-7 | 3.0e-7 |
| Halation sigma 7; 9 x 2.5; 25 | 1.1e-6; 8.3e-7 | 1.2e-6; 8.9e-7 | 1.3e-6 |
| Mix 0.6; mix 0.8 with a mask; mix 1 with a mask | 8.9e-7; 6.0e-7 | 6.0e-7; 6.0e-7 | 5.4e-7 |

Each cell gives the largest difference of its cases. The Gaussian kernels equal OpenCL bit for bit
on the RTX 3080 and differ by one or two ulp on Iris Xe; the Halation differences come from the
`exp` of the window weights.

**Primary success call chain:**

```text
DrtPostExecutor<VulkanDrtOps> (VK12), GradeExecutor<VulkanGradeOps>, or VulkanNeighborhoodTest
  -> NeighborExecutor<Ops>::Execute
       -> VulkanNeighborOps::AcquireHorizontalScratch -> TexturePool::Acquire (RGBA32F)
       -> VulkanNeighborOps::DispatchHorizontal
            -> RequireKernelParams -> VulkanBindScene (source)
            -> node value runtime.neighbor.<index> -> VulkanBackend::UploadBufferRange
            -> RecordKernelDispatch(primary_grade_neighbor_blur_h_scene_rgba32f) into the scratch image
       -> VulkanNeighborOps::DispatchVerticalApply
            -> VulkanBindScene (source, destination, mix source), VulkanRequireSceneMask
            -> RecordKernelDispatch(primary_grade_neighbor_apply_v_scene_rgba32f) into the destination
       -> the scratch lease returns to the pool
  -> nothing is submitted; EndRender submits the recording
```

**Primary failure call chain:**

```text
Film Grain or a point behavior, a vertical radius above 63, more Gaussian taps than the radius gives
  -> std::invalid_argument "VulkanNeighborOps::Dispatch...: ..." before a command
a scratch image of another size, scene images of another size, a missing mask, a vertical dispatch
without its horizontal dispatch
  -> std::runtime_error "Vulkan neighborhood: ..." before the dispatch; the caller cancels the render
```

**What was proven (executed tests, validation layer on):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| The seven tests of the section | `VulkanNeighborhoodTest` | PASS on RTX 3080, Iris Xe, Apple M4 |
| VK0 to VK9 suites, Windows | `ctest -R "Vulkan\|PipelineAccelerator"` | Iris Xe: 147 tests, 146 PASS, 1 skipped (a test for a build without Vulkan). RTX 3080: the same before the three-plane tile; after it, 145 PASS, 1 skipped, and `VulkanDevelopTest.DngWarpOfARealDngEqualsTheReferenceBackend` refused by the memory limit (a game of the user held 3.9 GB, the budget was 3.1 GB; the test uses no kernel of this phase), with `VulkanNeighborhoodTest`, `VulkanPrimaryGradeTest`, and the kernel check passing |
| VK0 to VK9 suites, Apple M4 | `ctest -R "Vulkan\|PipelineAccelerator"` | 111 tests, 109 PASS, 2 skipped (Windows only); the three test RAWs copied for the run |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | `gpu.kernels.ok=vulkan kernels=36` on the three devices |
| Validation errors | `TearDown` of each GPU test | 0 |
| Windows MSVC build of every Vulkan target | `scripts\msvc_env.cmd --build build\debug` | PASS, no warning |

**Findings outside this phase.** A Color Grade holds no neighborhood adjustment, so the Grade
neighborhood stage of every backend has no product input, and the Metal point formulas for
behaviors 13 to 15 are unreachable (#1226). The two image kernels of `primary_grade_neighbor.cl`
have no caller and `WarmUpPlan` warms them in place of the two scene kernels (comment on #1220).

**LOC note:** about 1340 added and 58 removed lines without the plan: the kernels 256, the kernel
header 86, `VulkanNeighborOps` 270, the scene helpers and the Grade pass 46, CMake and the catalog 15,
the tests 691 (the new test 666 with the host evaluation, the reference support 5, CMake 20). Above
the estimate of 800 to 1200: the host evaluation (about 150 lines) is the reference on the Apple M4.

**Remaining gaps:** none.

### Phase VK10: local tone (Shadows, Highlights)

**Objective and deliverables.** The Vulkan backend runs the Local Laplacian stage of a Color Grade
(Shadows and Highlights) through the shared `LocalToneExecutor`: the extract of the log intensity
into the canonical mask plane, the Gaussian pyramids of the source and of each remapped sample,
the select of the Laplacian levels between two samples, the collapse, and the apply with the
optional masked mix. `VulkanGradeOps::ExecuteLocalTone` runs it in place of the VK10 error, with
the canonical source and result planes in the graph image cache, as in OpenCL. Issue #1228.

**Inputs and prerequisites.** VK8 (`GradeExecutor<VulkanGradeOps>`, `VulkanSceneView`, the dummy
storage images). VK3 (`math.slangh`, the Slang branch of `color_encoding_math.h`). VK2 (the
transient arena of the workspace, the graph image cache).

**Source audit of the VK10 entry (2026-11-27).**

| OpenCL entry | Kernels and shared code | Callers | VK10 decision |
| --- | --- | --- | --- |
| `OpenClLocalToneOps::Extract`, `ExtractReference` | `local_tone_extract_scene`, `local_tone_extract_reference_scene`: the bilinear read of a scene image (cached image or scene-work buffer), `LogIntensity` (AP1 intensity in ACEScc), and for the canonical plane the reference-to-render transform | `LocalToneExecutor::Prepare` | VK10 |
| `PyramidDown`, `Remap`, `Select`, `Collapse` | `local_tone_pyr_down` (5 x 5 binomial), `local_tone_remap` (`RemapDelta` with `pow`), `local_tone_select` (the Laplacian of two samples by the expand of the coarser level), `local_tone_collapse` | the same | VK10 |
| `ApplyAdjustedAndMix` | `local_tone_apply_scene`: the bilinear read of the two planes at the apply transform, the intensity ratio (at most 32), the gamut scale toward the target intensity, the mix with the Grade input times the R8 mask | `LocalToneExecutor::ApplyMix` | VK10 |
| `FillZero`, the canonical copies (`CopyImageToDeviceMemory`, `CopyDeviceMemoryToImage`) | buffer transfers of the OpenCL backend | `Prepare`, `PersistCanonical`, `ApplyCanonicalSampleAndMix` | VK10, as three kernels on device addresses (see the data rules) |
| `local_tone_extract`, `local_tone_extract_reference`, `local_tone_apply` (image kernels), `EnqueueLlfApply` | | none; `OpenClBackend::WarmUpPlan` only, which warms them in place of the two scene kernels | Out of the port: no product caller (comment on #1220) |

Metal has its own local tone kernels (`local_tone.metal`, image based) with the same formulas; on
the Apple M4 the reference is the Metal pass, on Windows the OpenCL pass on the same GPU.

**Modules, files, and APIs.**

- `edit/runtime/vulkan/shader/local_tone.slang` (group `local_tone`: kernels
  `local_tone_extract_scene`, `local_tone_extract_reference_scene`, `local_tone_pyr_down`,
  `local_tone_remap`, `local_tone_select`, `local_tone_collapse`, `local_tone_apply_scene`,
  `local_tone_fill_zero`, `local_tone_copy_image_to_plane`, `local_tone_copy_plane_to_image`).
- `include/vulkan/vulkan_local_tone_kernels.hpp`: names, bindings, push constant layouts; the
  descriptors go into `VulkanKernelCatalog()`.
- `inc/edit/runtime/vulkan/vulkan_local_tone_pass.hpp`, `edit/runtime/vulkan/vulkan_local_tone_pass.cpp`
  (target `EditRuntimeVulkanPasses`): `ExecuteVulkanLocalTone` with `VulkanLocalToneOps`, the form
  of `ExecuteOpenClLocalTone`.
- `VulkanGradeOps::ExecuteLocalTone` calls it; the VK10 error and `ThrowStageOfALaterPhase` go.
- Test: `tests/edit/runtime/vulkan_local_tone_test.cpp` (`VulkanLocalToneTest`).

**Data rules and invariants.**

- The pyramid planes are R32F row-major ranges of the transient arena of the workspace (the OpenCL
  rule); a kernel reads and writes them by device address (`TransientBufferArena::Allocate` gives
  the slab address plus the offset). A stable render reuses the slabs and creates no buffer.
- The canonical source and result planes are R32F images of the graph image cache
  (`local_tone.source.0`, `local_tone.result.0`), recorded unpublished and published by
  `PublishResults`, as in OpenCL. The copies between an image and a plane, and the zero fill of a
  plane, are kernels on the device address: the arena gives an address, not the `VkBuffer`, and a
  kernel copy is exact.
- Each push constant block is at most 128 bytes (the Vulkan minimum of `maxPushConstantsSize`).
  The transforms carry the six used values of the 3 x 3 matrix.
- `pow` of the remap is `AlcedoPow`, the ACEScc functions are the Slang branch of
  `color_encoding_math.h` (VK3 rule). The expand halves an even coordinate with an arithmetic shift,
  not a signed division (VK4 rule for negative coordinates on NVIDIA).
- The apply reads the working texel before it writes it; the working image is a scene-work member
  and equals the adjusted binding (the OpenCL precondition).
- Every dispatch records into the open recording and submits nothing.

**Implementation steps.**

1. The kernel group and its header; the catalog.
2. `VulkanLocalToneOps` and `ExecuteVulkanLocalTone`; `VulkanGradeOps::ExecuteLocalTone`.
3. `VulkanLocalToneTest`; the VK10 failure case leaves `VulkanPrimaryGradeTest`.
4. Runs: RTX 3080 Laptop GPU and Iris Xe against OpenCL; Apple M4 (MoltenVK) against Metal.

**Tests and evidence.**

- `VulkanLocalToneTest.ShadowsAndHighlightsEqualTheReferenceBackend` (full frame: Shadows +80 and
  -60, Highlights -100 and +50, both, the slider ends; an odd image size)
- `VulkanLocalToneTest.RegionOfInterestEqualsTheReferenceBackend` (the canonical sample after a full
  frame, and a region of interest on a device without a canonical plane)
- `VulkanLocalToneTest.SliderEditReusesTheCanonicalSourceAndEqualsTheReferenceBackend`
- `VulkanLocalToneTest.MixMaskAndHistogramEqualTheReferenceBackend`
- `VulkanLocalToneTest.CanonicalPlanesArePublishedOnlyAfterTheRender`
- `VulkanLocalToneTest.StableRenderSamplesTheCanonicalPlanesAndCreatesNoResource`
- `VulkanLocalToneTest.LocalToneRecordsIntoTheOpenRecordingWithoutASubmission`
- `VulkanLocalToneTest.InvalidRequestsFailAndPublishNothing`

**Build and run commands.**

```text
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanLocalToneTest VulkanPrimaryGradeTest
         set ALCEDO_VULKAN_DEVICE=RTX (or Iris)
         ctest --test-dir build/debug -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
macOS:   source ~/Projects/vulkan_tools/env.sh
         cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanLocalToneTest VulkanPrimaryGradeTest
         ctest --test-dir build/macos-vulkan -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The listed tests pass on the RTX 3080 Laptop GPU and Iris Xe against OpenCL with no
      validation error.
- [x] The listed tests pass on the Apple M4 against Metal with no validation error.
- [x] The OpenCL, CUDA, and Metal results are unchanged: the phase changes no shared header and no
      source of those backends.

**Expected diff.** 1300 to 1700 lines.

##### Phase VK10 completion record (2026-11-27)

**Status:** complete on the RTX 3080 Laptop GPU, Iris Xe, and the Apple M4 (MoltenVK). Issue
#1228. Branch `feature/vulkan-vk10-local-tone` on the VK9 branch `feature/vulkan-vk9-neighborhood`
(pull request #1227).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Plane transfers | `FillDeviceMemory`, `CopyImageToDeviceMemory`, `CopyDeviceMemoryToImage` of OpenCL | Three kernels on the device address: `local_tone_fill_zero`, `local_tone_copy_image_to_plane`, `local_tone_copy_plane_to_image` | The transient arena gives a device address, not the `VkBuffer` of its slab; a kernel copy is exact, and the backend needs no table of live buffers. |
| Zero fill extent | Not in the plan | Rows of 1024 texels | A plane of 2048 x 2048 texels in one row would need 262 144 workgroups on the x axis, above the guaranteed limit of 65 535. |
| Push constants | Not in the plan | At most 88 bytes for each kernel; the transforms carry the six used values of the 3 x 3 matrix | Inside the Vulkan minimum of 128 bytes, so no node value buffer is necessary (VK9 needed one for 300 bytes). |
| Mix flag | The OpenCL `mix_is_buffer >= 0` | `apply_mix` 0 or 1 | A scene image is a buffer by its address being set (VK8 rule), so the flag only says whether the mix runs. |
| Result struct | `OpenClLocalToneResult` | `VulkanLocalToneResult` with the decision trace of the executor | The test and a later caller read the decision without a second lookup. |
| Tolerance | The tolerance of the matching OpenCL test (2e-3 against the CPU reference) | 1e-5 against OpenCL and Metal, the neighborhood tolerance of VK9 | The measured largest difference is 5.4e-7 on the three devices; the OpenCL bound against a CPU model would hide a real fault. |

**Measured differences** (logs `build/tmp/vulkan_backend/vk10/`; relative difference
`|vulkan - reference| / max(1, |reference|)` of the ACEScc Grade output; 256 x 192 input from
2^-8 to 2^2 with a bright window, a dark block, and a 12 % texture; tolerance 1e-5):

| Case | RTX 3080: OpenCL | Iris Xe: OpenCL | Apple M4: Metal |
| --- | --- | --- | --- |
| Shadows +80, -60; Highlights -100, +50; both at the slider ends; 203 x 157 | 5.4e-7 | 5.1e-7 | 4.8e-7 |
| Region after a full frame (canonical sample); region without a canonical plane | 5.4e-7 | 4.8e-7 | 4.2e-7 |
| Slider edit 25 -> 70 (canonical source reused) | 4.8e-7 | 4.8e-7 | 4.2e-7 |
| Mix 0.6; mix 0.8 with a mask; mix 0.5 with a histogram | 3.0e-7 | 3.3e-7 | 3.0e-7 |

Each cell gives the largest difference of its cases (rms 4e-8 to 8e-8). The canonical sample
equals the render that built the planes bit for bit; the histogram render equals the render
without it bit for bit.

**Primary success call chain:**

```text
GradeExecutor<VulkanGradeOps>, the LocalLaplacian stage of the schedule
  -> VulkanGradeOps::ExecuteLocalTone -> ExecuteVulkanLocalTone
  -> LocalToneExecutor<VulkanLocalToneOps>::Prepare
       -> LookupCanonical (graph image cache, published revision and long edge) -> DecideLocalTone
       -> SampleCanonical: return (the apply copies the two canonical images into planes)
       -> else AllocateScratchPlane x 4 per level (transient arena, device addresses)
       -> ExtractReference (full frame) or Extract, or BindCanonicalSourcePlane (slider edit)
       -> PyramidDown per level; FillZero of the result levels
       -> per sample pair: Remap + PyramidDown (two samples), Select per level
       -> Collapse from the coarsest level
       -> PersistCanonicalSource / PersistCanonicalResult
            -> AcquireImageForWrite (R32F) -> local_tone_copy_plane_to_image -> RecordUnpublished
  -> LocalToneExecutor::ApplyMix -> local_tone_apply_scene into the working scene-work member
  -> nothing is submitted; EndRender submits; PublishResults publishes the canonical planes
```

**Primary failure call chain:**

```text
outside BeginRender, empty geometry, a working binding that is not the adjusted scene-work member,
a missing or wrong mask, scene images of different sizes, lost canonical planes
  -> std::runtime_error "ExecuteVulkanLocalTone: ..." (or the shared executor message) before the
     dispatch -> the caller cancels the render -> CancelRender drops the unpublished canonical planes;
     the published planes of an earlier render stay
```

**What was proven (executed tests, validation layer on):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| The eight tests of the section | `VulkanLocalToneTest` | PASS on RTX 3080, Iris Xe, Apple M4 |
| The Grade tests without the VK10 failure case (`InvalidRequestsFail`) | `VulkanPrimaryGradeTest` | PASS on RTX 3080, Iris Xe, Apple M4 |
| VK0 to VK10 suites, Windows | `ctest -R "Vulkan\|PipelineAccelerator"` | RTX 3080 and Iris Xe: 155 tests each, 154 PASS, 1 skipped (a test for a build without Vulkan) |
| VK0 to VK10 suites, Apple M4 | `ctest -R "Vulkan\|PipelineAccelerator"` | 119 tests, 117 PASS, 2 skipped (Windows only); the three test RAWs copied for the run |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | `gpu.kernels.ok=vulkan kernels=46` on the three devices |
| Validation errors | `TearDown` of each GPU test | 0 |
| Windows MSVC build of every Vulkan target | `scripts\msvc_env.cmd --build build\debug` | PASS, no warning |

**Findings outside this phase.** The three image kernels of `local_tone.cl` (`local_tone_extract`,
`local_tone_extract_reference`, `local_tone_apply` with `EnqueueLlfApply`) have no OpenCL caller,
and `WarmUpPlan` warms them in place of the three scene kernels (comment on #1220).

**LOC note:** about 1854 added and 32 removed lines without the plan: the kernels 442, the kernel
header 236, the pass 547 (`vulkan_local_tone_pass.cpp` 486, its header 61), the Grade pass 38,
CMake and the catalog 18, the tests 605 (the new test 572, the Grade test 13, CMake 20). Above the
estimate of 1300 to 1700 and below the phase limit of 2000: the three plane kernels and the
eight tests with their cases.

**Remaining gaps:** none.

### Phase VK11: MaskEvaluate, MaskUnion, AI Mask coverage

**Objective and deliverables.** The Vulkan backend evaluates the Masks of a Color Grade on the
render grid (MaskEvaluate: Radial, Linear Gradient, AI Mask, with the Luminance Range and the Hue
Range), folds the enabled Masks of a Grade into its mask output (MaskUnion), makes the Feather and
Expand of AI Mask coverage (`VulkanBackend::RecordAiMaskEdge` in place of the VK11 error), and
writes the three coverage displays of the probe path (the value display of a probed Mask, the
coverage display of a committed AI Mask, the range probe display of a candidate). The pass
encoders of MaskEvaluate and MaskUnion call the passes. Issue #1230.

**Inputs and prerequisites.** VK8 (`VulkanSceneView`, the dummy storage images, the R8 mask input of
the Grade). VK3 (`math.slangh`: `AlcedoAtan2`, `AlcedoSin`, `AlcedoCos`, `AlcedoCbrt`; the Slang
branch of `color_encoding_math.h`; `oklab.slangh` of VK8). VK2 (the graph image cache, the
workspace coverage cache `MaskCoverageTextureCache<VulkanBackend>`, the R8 upload).

**Source audit of the VK11 entry (2026-11-27).**

| OpenCL entry | Kernels and shared code | Callers | VK11 decision |
| --- | --- | --- | --- |
| `ExecuteOpenClMask` (per source) | `mask_analytic_r8`, `mask_analytic_ranged_r8`: the Radial and Linear Gradient coverage at the reference position of the render pixel; `mask_raster_r8`, `mask_raster_ranged_r8`: the mean of `samples x samples` bilinear reads of the coverage texture (`MakeRasterMaskSamplingPlan`); invert, `MaskRangeWeight` (OkLab of the ACEScc Grade input, the hue gate and the lightness window), opacity, R8 quantization | `PassEncoder<MaskEvaluate>` of `PlanExecutor`, the tests | VK11 |
| `ExecuteOpenClMaskUnion` | `mask_fill_zero_r8`, `mask_union_max_r8`, `CopyTexture2D`, `AliasImageFrom` | `PassEncoder<MaskUnion>` | VK11 |
| `ExecuteOpenClMask` (Grade and plan overloads) | the two passes above in backbone order | the tests | VK11 |
| `RecordMaskValueDisplay`, `RecordMaskCoverageDisplay`, `RecordMaskCandidateRangeDisplay` | `mask_value_display_rgba32f`; the raster kernels into an RGBA32F image | `PlanExecutor::RecordProbedMask`, `RecordProbedCandidate`, `MaskCoveragePresentation` (SM8.5) | VK11: the pipeline side of the probe path, so `PlanExecutor<VulkanBackend>` (VK17) compiles. The display of the target in the viewport stays VK18. |
| `OpenClBackend::RecordAiMaskEdge` | `ai_mask_morphology_r8` (3 x 3 dilate or erode, cross and square in turn), `ai_mask_feather_rows_r32f`, `ai_mask_feather_columns_r8` (the separable Gaussian of `AiMaskFeatherWeights`, round half to even) | `MaskCoverageTextureCache::AcquireStored`, `AcquireCandidate` | VK11 |
| Mask sources other than Radial, Linear Gradient, and AI Mask | none: `ExecuteOpenClMask` has no kernel for them | | The same error on Vulkan (`compiled mask does not match document`). |

Metal has the same kernels in `mask.metal` and `metal_ai_mask_coverage.mm`; on the Apple M4 the
reference is the Metal pass, on Windows the OpenCL pass on the same GPU.

**Modules, files, and APIs.**

- `edit/runtime/vulkan/shader/mask.slang` (group `mask`: `mask_analytic_r8`, `mask_raster_r8`,
  `mask_raster_rgba32f`, `mask_fill_zero_r8`, `mask_union_max_r8`, `mask_value_display_rgba32f`).
- `edit/runtime/vulkan/shader/ai_mask_coverage.slang` (group `ai_mask_coverage`:
  `ai_mask_morphology_r8`, `ai_mask_feather_rows_r32f`, `ai_mask_feather_columns_r8`).
- `include/vulkan/vulkan_mask_kernels.hpp`, `include/vulkan/vulkan_ai_mask_coverage_kernels.hpp`:
  names, bindings, push constant layouts; the descriptors go into `VulkanKernelCatalog()`.
- `inc/edit/runtime/vulkan/vulkan_mask_pass.hpp`, `edit/runtime/vulkan/vulkan_mask_pass.cpp` (target
  `EditRuntimeVulkanPasses`): `ExecuteVulkanMask` (per source, per Grade, per plan),
  `ExecuteVulkanMaskUnion`, the three display records.
- `edit/runtime/vulkan/vulkan_ai_mask_coverage.cpp` (target `EditRuntimeVulkan`):
  `VulkanBackend::RecordAiMaskEdge`; the VK11 error goes.
- `PassEncoder<VulkanBackend, GpuPassKind::MaskEvaluate>` and `<..., MaskUnion>` in
  `vulkan_pass_encoder.hpp`.
- Test: `tests/edit/runtime/vulkan_mask_test.cpp` (`VulkanMaskTest`, `VulkanMaskHarnessTest`).

**Data rules and invariants.**

- The Mask outputs are R8 UNORM storage images of the graph image cache; a kernel writes `k / 255`
  for the quantized code `k`, so the image holds `k` (the OpenCL `write_imagef` rule).
- A ranged kernel reads the Grade input only when a `*_restricted` flag of `MaskRangeParams` is
  set; with both flags zero the range weight is 1 and the value is the one of the kernel without
  ranges (`value * 1 * opacity` equals `value * opacity`). The OpenCL ranged and unranged kernel
  pairs are one Vulkan kernel each.
- The Grade input is a cached RGBA32F storage image or a scene-work member read by device address
  (VK8 rule); its size equals the render extent, checked before any command.
- Each push constant block is at most 128 bytes. The Radial and Linear Gradient values share seven
  floats; the ranges are the nine 4-byte words of `MaskRangeParams` (the kernel header does not
  include the Mask model, so the catalog library stays free of the edit libraries).
- The Feather half kernel (up to 201 floats) is a buffer of the `RecordAiMaskEdge` call, read by
  device address; the reset at the end of the call gives it back, and the retire rule keeps it
  until its batch completes (VK6 rule of call scratch).
- The Union copies the first enabled source into the Union output and folds each further source
  in place (`output = max(output, source)` reads and writes the same texel in one invocation), so
  it needs no scratch image.
- Trigonometric, cube root, and arc tangent calls are the `math.slangh` functions (VK3 and VK5
  rules); push constant arrays are read with constant indices (VK4 Intel rule).
- Every pass records into the open recording and submits nothing.

**Implementation steps.**

1. The two kernel groups and their headers; the catalog.
2. `VulkanBackend::RecordAiMaskEdge`.
3. `ExecuteVulkanMask`, `ExecuteVulkanMaskUnion`, the displays, the pass encoders.
4. `VulkanMaskTest`; the VK2 test of the VK11 error becomes a test of the edge validation.
5. Runs: RTX 3080 Laptop GPU and Iris Xe against OpenCL; Apple M4 (MoltenVK) against Metal.

**Tests and evidence.**

Against the reference backend on the same GPU (B9):

- `VulkanMaskTest.AnalyticMasksEqualTheReferenceBackend` (a soft Radial, a rotated inverted Radial
  at opacity 0.6, a diagonal Linear Gradient; full frame, crop and rotation, a region of interest,
  render scale 0.5)
- `VulkanMaskTest.RangedMasksEqualTheReferenceBackend` (Hue, wrapping Hue, Luminance, both with a
  raised saturation threshold, a Linear Gradient with Luminance; the Grade input as a cached image
  and as a scene-work member)
- `VulkanMaskTest.AiMasksEqualTheReferenceBackend` (no edge, Feather, Expand +2 and -3, both, a render
  scale, crop and rotation, ranges with invert and opacity)
- `VulkanMaskTest.AiMaskFeatherAndExpandCoverageEqualsTheReferenceBackend` (the coverage textures of
  eight edge adjustments, Feather 50 included)
- `VulkanMaskTest.MaskUnionEqualsTheReferenceBackendAndAliasesOneSource`
- `VulkanMaskTest.CoverageDisplaysEqualTheReferenceBackend`
- `VulkanMaskTest.MaskedGradesEqualTheReferenceBackend` (Develop to two masked Grades through the pass
  encoders; the second Grade's Hue Range reads the first Grade's output)

The shared checks of the OpenCL, CUDA, and Metal mask tests against their independent references
(`VulkanMaskHarnessTest`, the names of the OpenCL test): the twelve AI Mask checks and the seven
Mask range checks that run without `PlanExecutor`. `CheckCoverageOnlyWorkKeepsPhotoResults` and
`CheckSecondGradeHueRangeReadsFirstGradeOutput` need `PlanExecutor<VulkanBackend>` (VK17); the
second one is covered here by `MaskedGradesEqualTheReferenceBackend`.

Recording and failure rules:

- `VulkanMaskHarnessTest.MaskPassesRecordIntoTheOpenRecordingWithoutASubmission`
- `VulkanMaskHarnessTest.RepeatedEdgeWorkCreatesNoPipeline`
- `VulkanMaskHarnessTest.CoverageEntriesHaveTheCachePriorityAndRenderImagesTheWorkingOne`
- `VulkanMaskHarnessTest.InvalidRequestsFailAndRecordNothing`
- `GpuDagVulkanWorkspaceTest.AiMaskEdgeWithoutWorkFailsAndRecordsNothing` (replaces the VK2 test of
  the VK11 error)

**Build and run commands.**

```text
Windows: cmd /c scripts\msvc_env.cmd --build build\debug --target AlcedoVulkanKernels VulkanMaskTest <every Vulkan test target>
         set ALCEDO_VULKAN_DEVICE=RTX (or Iris)
         ctest --test-dir build/debug -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
macOS:   source ~/Projects/vulkan_tools/env.sh
         cmake --build build/macos-vulkan --target AlcedoVulkanKernels VulkanMaskTest <every Vulkan test target>
         ctest --test-dir build/macos-vulkan -R "Vulkan|PipelineAccelerator" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The listed tests pass on the RTX 3080 Laptop GPU and Iris Xe against OpenCL with no
      validation error.
- [x] The listed tests pass on the Apple M4 against Metal with no validation error.
- [x] The OpenCL, CUDA, and Metal results are unchanged: the phase changes no source of those
      backends. The shared `mask_coverage_texture_cache.hpp` asks a backend for a cache texture
      only when the backend has `CreateCacheTexture2D` (Vulkan); the other backends compile the
      same calls as before (`OpenClMaskFixture.*` passes after the change).

**Expected diff.** 1300 to 1700 lines.

##### Phase VK11 completion record (2026-11-27)

**Status:** complete on the RTX 3080 Laptop GPU, Iris Xe, and the Apple M4 (MoltenVK). Issue
#1230. Branch `feature/vulkan-vk11-masks` on the VK10 branch `feature/vulkan-vk10-local-tone`
(pull request #1229).

**Implementation decisions.** The plan did not specify these details, or the work found a better
form. Each one agrees with A1 to A7 and B1 to B11.

| Item | Plan text | Implementation | Reason |
| --- | --- | --- | --- |
| Ranged and unranged kernels | The OpenCL pairs `mask_analytic_r8` / `mask_analytic_ranged_r8` and `mask_raster_r8` / `mask_raster_ranged_r8` | One kernel each; both `*_restricted` flags zero skip the read of the Grade input | `value * 1 * opacity` equals `value * opacity`, so the result of the kernel without ranges is unchanged (the shared check `FullySelectingRangeMatchesTheMaskWithoutRangesByteForByte` passes), and the catalog has two kernels fewer. |
| Coverage display output | OpenCL writes `(value, 0, 0, 1)` with one kernel into an R8 or an RGBA32F image | `mask_raster_r8` and `mask_raster_rgba32f` | A Vulkan storage image binding has one format. |
| Union | Copy of the first source, then `max(cur, next)` into a scratch image and a copy back for each further source | Copy of the first source, then `output = max(output, source)` in place | One invocation reads and writes its own texel, so no scratch image and no copy back are necessary. |
| Push constants | Not in the plan | Analytic 128 bytes (Radial and Linear Gradient share seven floats), raster 96, edge 48, the others 8 | Inside the Vulkan minimum of 128 bytes. The ranges are the nine 4-byte words of `MaskRangeParams`, so the kernel header (in the catalog library `VulkanContext`) does not include the Mask model and its JSON dependency. |
| Feather weights | A by-value OpenCL argument of 808 bytes | A buffer of the `RecordAiMaskEdge` call, read by device address, given back at the end of the call | 201 floats do not fit in the push constants; the retire rule keeps the buffer until its batch completes (VK6 rule). |
| Memory priority of the coverage cache | Section 4.2 and the VK2 record: the mask coverage cache sets 0.2 for its entries | `VulkanBackend::CreateCacheTexture2D` (`kCachePriority`); the shared `MaskCoverageTextureCache` uses it for the coverage textures of its entries when the backend has it (`if constexpr (requires ...)`); the work textures keep 0.8. `Texture2D::MemoryPriority` gives the value. | The cache is shared by the four backends; OpenCL, CUDA, and Metal have no such call, so their code is the same as before. |
| Display records | VK18 has "mask coverage display" | The three pipeline-side records (`RecordMaskValueDisplay`, `RecordMaskCoverageDisplay`, `RecordMaskCandidateRangeDisplay`) are in VK11 | They are in `opencl_mask_pass.cpp` (the VK11 model) and `PlanExecutor` calls them, so `PlanExecutor<VulkanBackend>` (VK17) compiles. The viewport display of the target stays VK18. |
| Tolerance | The tolerance of the matching OpenCL test (one code against the CPU reference) | R8 codes: exact against OpenCL on NVIDIA; one code against OpenCL on Intel and against Metal; one code for a Hue Range or a Luminance Range | Measured below. The ranged weight uses atan2 and cube roots, whose last bits differ from the OpenCL and Metal built-ins; Metal builds its product mask kernels with fast math. |
| Wrong reference on Iris Xe | B9: compare with OpenCL on the same GPU | On Intel, a Union of three or more Masks is compared with the maximum of the Vulkan sources, and an Expand that writes a morphology work image a second time with the host morphology (exact) or the host AI Mask evaluation (one code); every Union is checked against the maximum of its sources on every device | OpenCL on Iris Xe gives wrong results for exactly these cases: its own check `OpenClMaskFixture.GpuExpandMatchesAlternatingMorphologyExactly` fails there (Expand +-50) and passes on the NVIDIA OpenCL device, and its Union of three Masks changes from run to run (up to 229 codes). The Vulkan results equal the independent references. User 2026-11-27: OpenCL goes to the archive after Vulkan (VK22), so no issue for it (#1231 closed). |
| Plan-executor checks | The shared checks of the OpenCL mask test | Without `CheckCoverageOnlyWorkKeepsPhotoResults` and `CheckSecondGradeHueRangeReadsFirstGradeOutput` | Both need `PlanExecutor<VulkanBackend>` (VK17). `MaskedGradesEqualTheReferenceBackend` covers the second: the second Grade's Hue Range reads the first Grade's scene-work output. |

**Measured differences** (logs `build/tmp/vulkan_backend/vk11/`; R8 codes of the Mask outputs, the
largest difference and the count of differing texels; the masked Grades are the relative
difference of the ACEScc output, tolerance 2e-4):

| Case | RTX 3080: OpenCL | Iris Xe: OpenCL | Apple M4: Metal |
| --- | --- | --- | --- |
| Analytic: 3 Masks, full frame, crop and rotation, region, render scale 0.5 | 0 | 1 code at 1 texel of 17 063 (rotated Radial); Union: equal to the maximum of the sources | 0 |
| Ranged: 5 Masks, cached image and scene-work input | 1 code at 1 texel of 6912 (Hue Range) | 1 code at 1 texel (Hue Range); Union: maximum of the sources | 1 code at 1 texel each (Hue Range, wrapping Hue Range) |
| AI Masks: 8 edge, scale, geometry, and range cases | 0 | 0; Feather 2.5 with Expand +4: host evaluation within one code | 1 code at 20 of 768 texels (Feather 2 at render scale 0.5), else 0 |
| Coverage textures of 8 edge adjustments (Feather 50 included) | 0 | 0; Expand +5, -4, and 3 with Feather: host morphology, exact | 0 |
| Union of 3 enabled Masks, one alias, zero fill | 0 | maximum of the sources; the independent reference within one code | 0 |
| Value, coverage, and candidate range displays | 0 | 0 | 1 code at 9 of 3072 texels (coverage display) |
| Two masked Grades (Develop to the second Grade) | 1.2e-5 | 1.6e-5 | 1.1e-5 |

**Primary success call chain:**

```text
PlanExecutor (VK17) or a test, per Color Grade with a Mask stack
  -> PassEncoder<VulkanBackend, MaskEvaluate>::Encode -> ExecuteVulkanMask (per source)
       -> RequireMaskModel; RangeSceneView (only a restricted range: cached image or scene-work
          member of the render extent) -> AcquireImageForWrite (effective output, R8)
       -> Radial / Linear Gradient: mask_analytic_r8
       -> AI Mask: BasicRenderWorkspace::AcquireAiMaskCoverage
            -> MaskCoverageTextureCache::AcquireStored -> Upload (CreateCacheTexture2D)
            -> with Feather or Expand: RecordEdge -> VulkanBackend::RecordAiMaskEdge
                 -> ai_mask_morphology_r8 per step (work textures), Feather weights buffer,
                    ai_mask_feather_rows_r32f, ai_mask_feather_columns_r8 (entry texture)
          -> mask_raster_r8 (samples x samples bilinear reads of the coverage texture)
  -> PassEncoder<VulkanBackend, MaskUnion>::Encode -> ExecuteVulkanMaskUnion
       -> none enabled: mask_fill_zero_r8; one: AliasImageFrom; more: CopyTexture2D of the first,
          mask_union_max_r8 in place for each further source
  -> the Grade reads the R8 mask output (VK8); nothing is submitted; EndRender submits
```

**Primary failure call chain:**

```text
outside BeginRender, empty geometry, a Mask source without a kernel, a ranged Mask without an
RGBA32F Grade input of the render extent, a missing asset, a missing enabled source of the Union,
an edge dispatch without work or with a missing or wrong texture, a display output of the wrong
format or size
  -> std::runtime_error ("ExecuteVulkanMask: ...", "ExecuteVulkanMaskUnion: ...",
     "VulkanBackend::RecordAiMaskEdge: ...", "AI Mask coverage: ...", "Record...Display: ...")
     before the dispatch of the failing pass; the output of a failed MaskEvaluate is not acquired
  -> the caller cancels the render; a failed edge work keeps the published coverage entries
```

**What was proven (executed tests, validation layer on):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| The seven comparisons of the section (`VulkanMaskTest.*`) | `VulkanMaskTest` | PASS on RTX 3080, Iris Xe, Apple M4 |
| The twelve AI Mask and seven Mask range checks against the independent references | `VulkanMaskTest` (`VulkanMaskHarnessTest.*`) | PASS on the three devices |
| Recording rule, pipeline reuse, memory priority, failures (four tests) | `VulkanMaskTest` (`VulkanMaskHarnessTest.*`) | PASS on the three devices |
| `AiMaskEdgeWithoutWorkFailsAndRecordsNothing` | `GpuDagVulkanWorkspaceTest` | PASS on the three devices |
| VK0 to VK11 suites, Windows | `ctest -R "Vulkan\|PipelineAccelerator"` | RTX 3080 and Iris Xe: 185 tests each, 184 PASS, 1 skipped (a test for a build without Vulkan) |
| VK0 to VK11 suites, Apple M4 | `ctest -R "Vulkan\|PipelineAccelerator"` | 149 tests, 147 PASS, 2 skipped (Windows only); the three test RAWs copied for the run |
| `alcedo-gpu-check --vulkan` | `alcedo_gpu_check` | `gpu.kernels.ok=vulkan kernels=55` on the three devices |
| `OpenClMaskFixture.*` after the change of the shared coverage cache | `GpuDagOpenClGradeTest`, NVIDIA OpenCL device | 31 PASS |
| Validation errors | `TearDown` of each GPU test | 0 |
| Synchronization validation (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC`) of the Union and edge cases | `VulkanMaskTest`, RTX 3080 | no hazard |
| Windows MSVC build of every Vulkan target | `scripts\msvc_env.cmd --build build\debug` | PASS, no warning |

**Findings outside this phase.** OpenCL on Iris Xe gives a wrong AI Mask Expand when the
morphology writes a work image a second time, and a nondeterministic wrong MaskUnion of three or
more Masks; separate submissions of each Vulkan step did not change the Vulkan result, which
equals the independent references. Not fixed: OpenCL goes to the archive after Vulkan (user
2026-11-27; #1231 closed as not planned).

**LOC note:** about 2640 added and 50 removed lines without the plan: the kernels 413 (`mask.slang`
287, `ai_mask_coverage.slang` 126), the kernel headers 253, the passes 627 (`vulkan_mask_pass.cpp`
371 and its header 124, `vulkan_ai_mask_coverage.cpp` 132), the backend priority and the shared cache
50, the pass encoders, CMake, and the catalog 60, the tests about 1240 (`vulkan_mask_test.cpp` 682
with the comparisons, `vulkan_mask_harness_test.cpp` 382 with the shared checks and the Vulkan rules,
`vulkan_mask_test_support.hpp` 132 with the inputs of both, and the workspace, Grade, and local tone
tests). Above the phase limit of 2000 lines, as VK4 and VK5: production code is about 1400 lines;
the rest is the two test files of one test target with two fixtures.

**Remaining gaps:** none.

### Phases VK3 to VK23: scope of each phase

Each entry gives the objective, the OpenCL model, and the main tests. The detailed section replaces
the entry before the implementation.

| Phase | Objective | OpenCL model | Main tests |
| --- | --- | --- | --- |
| VK3 | Detailed section above | | |
| VK4 | Detailed section above | | |
| VK5 | Detailed section above | | |
| VK6 | Detailed section above | | |
| VK7 | Detailed section above | | |
| VK8 | Detailed section above | | |
| VK9 | Detailed section above | | |
| VK10 | Detailed section above | | |
| VK11 | Detailed section above | | |
| VK12 | DiffusionFilter, `VulkanDrtOps` (its neighborhood operations are `VulkanNeighborOps` of VK9), DRT parameters, the tone curves of `common.cl`, the ACES 2.0 and OpenDRT math of `cst.cl` (moved from VK3) | `opencl_drt_pass.cpp`, `opencl_diffusion_filter_pass.cpp`, `drt.cl`, `drt_params.cl`, `common.cl`, `cst.cl` | Product output equals OpenCL for ACES and OpenDRT. |
| VK13 | Film print stage kernels | `opencl_film_print_pass.cpp`, `film_print.cl` | Negative to print output equals OpenCL. Reversal output equals OpenCL. |
| VK14 | Film print scan, film grain negative and post, the `__SLANG__` branch of the grain hash (`film_grain_field.h`) | `film_print_scan.cl`, `film_grain_*.cl`, `opencl_film_grain_pass.cpp` | Negative scan and grain equal OpenCL. |
| VK15 | Convolution layers, activation storage, workspace, stage profiler | `opencl/nn/*.cpp`, `demosaicnet_conv.cl` | Each layer equals the expected output of the weights file in the tolerance of the OpenCL layer tests. |
| VK16 | Bayer and X-Trans DemosaicNet modules, tiles, cache, the neural branch of Develop | `opencl_demosaicnet_*.cpp`, `demosaicnet_structural.cl` | Neural demosaic equals OpenCL. The time agrees with the VX4 record. |
| VK17 | Scope analyzer on the final DRT image, `FramePresenter<VulkanBackend>::Download`, `PipelineExecutor::vulkan_renderers_`, preference `vulkan` in the setting and in `alcedo-cli`, the export split of section 4.2 in `VulkanCommandContext` (8 ms submissions, one in flight, bands of rows, time per row from timestamps; moved from VK2) | `opencl_scope_analyzer.cpp`, `opencl_frame_presenter.cpp` | Export of one RAW on Vulkan equals the OpenCL export in tolerance. Scope data equals OpenCL. A 45 megapixel export has no submission above 8 ms of measured GPU time. |
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

Current state: not run (user decision 2026-11-22, A5). The neural networks use ONNX Runtime;
UtNet2 runs on ONNX Runtime DirectML in the Develop render (AI Denoise plan, DN3 record: 0.45 s for
45.4 megapixels in FP16 on the RTX 3080 Laptop GPU).

### 9.5 Memory budget and device loss (VX5)

Current state: on the RTX 3080 Laptop GPU, memory stays in video memory at about 410 GB/s until the
usage of the process is about 340 MiB below the `VK_EXT_memory_budget` budget. Memory that Windows
places above that point goes to shared system memory and runs at 1.3 GB/s; it stays there while it
is in use and after the process frees other memory. `vkAllocateMemory` fails only at 97 % of the
heap (and never with pageable memory), so the pool keeps its own limit: the budget minus 512 MiB
(section 4.2). The budget is a value of the process on the adapter, equal to the DXGI value, and it
falls when another process uses video memory. With `VK_EXT_memory_priority`, Windows moves the
low-priority memory out first. A read of an address with no memory on the pipeline device gives no
device loss: the GPU stops, and the GUI thread of the program stops with it, until the Windows GPU
timeout. The restore procedure below has no run.

#### 2026-11-22: allocation above the budget, a second process, and a fault on the pipeline device

**Setup.**

| Item | Value |
| --- | --- |
| Source | `main` `61d59bc98` with this plan. Experiment code: `experiments/vulkan/vx5/` on branch `feature/vulkan-backend-experiments` (`d06c70260`), with the VX2 context and viewport (the context recreates one device; the viewport item switches its producer and sets the image layouts of the new producer in a Qt frame) |
| Program | `vx5_memory`: the VulkanContext of VX2 with two devices on one physical device (B5). budget: no window; blocks of 256 MiB, each a dedicated device-local allocation with a device address, filled with `vkCmdFillBuffer`. After each new block, a touch pass (read and write of each `float4`, a timestamp after each block) over every block gives the speed of each block. Then 5 s of passes over every block (hold), 5 s over the newest 1.5 GiB (work), and 5 s after the oldest blocks are freed until the total is 512 MiB below the start budget (free). The run goes to the free budget at the start plus 2 GiB, or to the first failed allocation. lost: the viewport of VX3 (Qt Quick on the display device, 1280 x 800 on the LG ULTRAFINE; the producer on the pipeline device) and at 2 s one dispatch on the pipeline device that reads `0x7F0000000000` (an address with no memory). |
| Priority forms | none: no priority. split: 0.2 for the blocks inside the free start budget, 0.8 for the blocks above it (`VK_EXT_memory_priority`). pageable: split with `VK_EXT_pageable_device_local_memory`. other: none while a second `vx5_memory` process holds 3 GiB and passes over it each 100 ms for 60 s. |
| Devices | RTX 3080 Laptop GPU (8016 MiB device-local heap), driver 610.62. Iris Xe, driver 101.7092 (one heap of 16235 MiB in system memory). 32 GiB of system memory, 12 to 13 GiB free at the start. Vulkan SDK 1.4.328.1, `slangc` 2025.17.2, Qt 6.9.3 |
| Windows | `TdrDelay` 60 s (VX3) |
| Run check | `vx3/gpu_idle_guard.ps1` before and after each run (VX3). The guard held the first runs for the user's `UtNet2PhotoTimingTool` and `python` jobs. The run stops at 6 GiB of free system memory (not reached: lowest 7.8 GiB). |
| Validation | `VK_LAYER_KHRONOS_validation` on in `smoke_budget_nvidia` and `lost_nvidia_fixed_stall`: 0 errors, 0 warnings, 0 threading errors. The other runs have the layer off. |
| Evidence | worktree `build/tmp/vulkan_backend/vx5/`: `budget_nvidia_none`, `budget_nvidia_split`, `budget_nvidia_pageable`, `budget_nvidia_other3g` (with `.hold.txt` of the second process), `budget_intel_2g`, `lost_nvidia_fixed_stall`, `tdr_nvidia_dxcap_needs_admin`, `smoke_budget_nvidia`. `superseded/`: a first run of the none form with a build before the last change of the run report (same results). |

**Result: budget values.** The budget of the device-local heap of the RTX 3080 at the start is
7248 MiB (90 % of the heap); it falls to 6909 to 7011 MiB when the usage of the process passes
5 GiB. The Vulkan budget and usage are equal to the values of `IDXGIAdapter3::QueryVideoMemoryInfo`
for the process in each query. A block of 256 MiB on the display device adds 256 MiB to the usage
that the pipeline device reads: the values cover every device of the process. The start budget does
not show another process: with 3 GiB held by the second process, the budget at the start was still
7248 MiB; it fell to 4699 MiB at 2.3 GiB of own usage and to 3912 to 3924 MiB from 3 GiB. Iris Xe:
budget 15467 MiB of the 16235 MiB heap in system memory.

**Result: allocation above the budget, RTX 3080 Laptop GPU.** Speed of the touch pass (read and
write, GB/s); "slow" is below half of the speed of the first block.

| Form | Blocks in video memory | First slow block (own usage, budget) | Speed of the slow blocks | Allocation failure | Effect of priority |
| --- | --- | --- | --- | --- | --- |
| none | 0 to 24 (6400 MiB), 397 to 413 GB/s | 25 (6663 MiB, 7004 MiB) | 4.7, then 1.3 to 1.4 GB/s | `VK_ERROR_OUT_OF_DEVICE_MEMORY` at 7936 MiB (97 % of the heap) | - |
| split | 4 to 24, 28, 29 | 25 (6663 MiB, 7004 MiB) | 25 to 27 (priority 0.2): 1 to 4 GB/s | at 7936 MiB | The new blocks 28 and 29 (0.8) were placed in video memory at 278 to 282 GB/s; Windows moved the oldest blocks 0 to 3 (0.2) out to 3 GB/s. |
| pageable | 16 to 24, 28 to 35 | 25 (6663 MiB, 7011 MiB) | 25 to 27: 1 to 5 GB/s; 0 to 15 moved out: 3 GB/s | none up to the end of the run (9216 MiB, 115 % of the heap) | As split; Windows moved 16 blocks of priority 0.2 out for the 8 blocks of priority 0.8. |
| other (3 GiB in a second process) | 0 to 12, 28, 29 | 13 (3591 MiB, 3924 MiB) | 14 to 27: 1.3 to 1.4 GB/s | at 7936 MiB | - |

In every form a block in system memory stays slow during 5 s of passes over every block, during
5 s of passes over only the newest 1.5 GiB (the slow blocks among them stay slow), and after the
process frees its oldest blocks to 512 MiB below the start budget. In the other form the budget
came back to 7248 MiB at block 28 (at about the end of the 60 s hold time of the second process); then
blocks 16 to 23 came back to video memory within 15 s, and 14, 15, 24, 26, and 27 stayed slow to
the end of the run. A pass over the 7.5 GiB of the none form takes 1.6 to 1.7 s, of which the
1.25 GiB in system memory takes 1.6 s; the 6.25 GiB in video memory takes 33 ms. The fill of a
block takes 8 to 9 ms in video memory, 150 to 400 ms in system memory (none form), and 290 to
540 ms when Windows moves other blocks out for it (split and pageable forms). `vkAllocateMemory`
takes 0.4 to 13 ms in each form.

**Result: Iris Xe.** 8 blocks (2 GiB, below the budget): 28 to 35 GB/s for each block, no change
in the hold, work, and free phases. The run stays below the budget: the heap is system memory, and
a run to the budget would take 15 GiB of the free system memory.

**Result: a read of an address with no memory on the pipeline device, RTX 3080 Laptop GPU.** The
fence wait of the dispatch returned `VK_TIMEOUT` after 10 s; the pipeline device reported no
`VK_ERROR_DEVICE_LOST`. The NVIDIA driver wrote event 153 (`nvlddmkm`, "Error occurred on GPUID:
100") at the time of the fault and a second event 153 60 s later (the `TdrDelay` of this computer).
The GUI thread of the program did not run between the fault and the second event: the detection
callback that the load thread queued at 12 s did not run, the 40 s stop timer of the program ran at
about 60 s, and Qt Quick invalidated its scene graph 61.7 s after the start. The display device
therefore made no progress during the stall, as in the long-group form of VX3: Qt's render thread
waits for the GPU and the GUI thread waits for the render thread. With the Windows default
`TdrDelay` of 2 s the stall is 2 s. No program state after the reset was recorded: the run ended at
the reset.

**Result: forced reset.** `dxcap -forcetdr` exits with -1 and "DXCap: force TDR failed: Please run
as administrator"; the shell of the experiments has no administrator rights. A driver restart with
Win+Ctrl+Shift+B needs real keyboard input, which the experiments do not use on this computer. A
second fault run with the restore steps was stopped before its start (user, 2026-11-22: a model
experiment of the user ran; a GPU reset stops it). The fault form on Iris Xe did not run: it would
stall the GPU of the laptop panel for 60 s.

**Restore procedure (design, no run).** `vx5_memory` implements it; a run on a computer where a
reset is permitted records its times and the behavior of Qt Quick.

1. Detection: each `vkQueueSubmit` and `vkWaitForFences` of the pipeline device. Fence waits have a
   finite timeout; `VK_TIMEOUT` is a stall (section 2.4), `VK_ERROR_DEVICE_LOST` is a loss. A fence
   of each device that is never submitted, read with `vkGetFenceStatus`, and the
   `QQuickWindow::sceneGraphError` signal show a loss of the display device.
2. Pipeline device lost, display device alive: the viewport releases the frame images of the lost
   device (the item drops its `QRhiTexture` wrappers); after 3 more Qt frames the pipeline objects
   go (render workspace with its caches, shared resources, frame image ring), then
   `VulkanContext` destroys the pipeline device and creates a new one with the same create info. The
   new frame image ring is imported into the display device; its initial layouts are recorded in a
   Qt frame with `beginExternal`, because the display queue belongs to Qt's render thread after Qt
   starts (this changes the rule of section 4.2 for a restore). The first render then fills the
   caches again.
3. Display device lost: the editor window goes (or its scene graph is released), both devices are
   created again, and the window starts on the new display device. Which form Qt Quick 6.9 accepts
   for a device of the program (`fromDeviceObjects`) is the open question of this step.

**Items outside this record.** A restore run (both forms of step 3); the behavior of Qt Quick on a
lost display device; the Windows reset with the default `TdrDelay` of 2 s; a fault on Iris Xe;
allocation above the budget on Iris Xe; a new allocation after the free phase (whether it gets
video memory); priorities of the frame image ring against the cache entries during a drag.

## 10. Risks and stop conditions

| Id | Risk | Detection | Response |
| --- | --- | --- | --- |
| R1 | Qt Quick on Vulkan on Windows has a worse resize or a different display color than Direct3D 11. | VX2: the resize part occurred (Iris Xe about 4 times the Direct3D 11 time for each step, RTX 3080 a longer tail). The color part did not occur. | VK18 stops. The plan gets a new resize design ([#1040](https://github.com/zidage/AlcedoStudio/issues/1040)) before more display work. The compute phases VK0 to VK17 continue. |
| R2 | A long dispatch causes a device loss or stops the editor. | VX3: a whole export in one submission holds the preview for 100 ms (RTX 3080) to 2.4 s (Iris Xe); one workgroup of 3 s stops the display device for 3 s. The Qt Quick frames keep vsync with workgroups of normal length. | The split rule of section 4.2 (one export at a time, 8 ms submissions, one in flight, workgroups below 1 ms) and the VK2 data rule. |
| R3 | Hand-written neural network kernels are slower than the vendor convolution code. | VX4 (not run, A5) | The neural networks use ONNX Runtime (A5). The interop of the Vulkan pipeline with ONNX Runtime DirectML gets a design when VK15, VK16, or VK20 resumes. |
| R4 | The pipeline and Qt Quick race on the queues of one device. | VX2 thread safety validation: occurred on both devices, with a second queue and with the queue mutex (Qt's `vkDeviceWaitIdle` at resize) | Two devices on one physical device (B5, section 4.2). With them the validation reports 0 threading errors. |
| R5 | The results differ between GPU vendors more than the tolerance. | Comparison tests on NVIDIA and Intel | The kernel marks the affected expressions as precise. The tolerance does not increase without a record. |
| R6 | The archive of OpenCL removes support for GPUs with no Vulkan 1.3 driver. | Device reports from users, VK19 experimental period | User decision before VK22 (section 1.5). |
| R7 | Software that injects a Vulkan layer makes the application crash. | VX2 step 6 (OBS hook, Steam overlay, Steam Fossilize, NVIDIA layers loaded: no crash, no hang), reports in the experimental period | The application disables the named implicit layers at start. |
| R9 | The GUI thread waits for the Vulkan `beginFrame` of the render thread while the pipeline submits on its own device (about 10 ms of each frame on the NVIDIA driver). | VX2 | [#1041](https://github.com/zidage/AlcedoStudio/issues/1041): the cause and a design. VK18 measures input latency in the editor with the split device. |
| R8 | The backends on `main` change faster than the Vulkan passes follow. | Failing comparison tests | Section 5, rule 3. |
| R10 | A kernel reads an address with no memory. The GPU stops until the Windows GPU timeout, and the editor stops with it (VX5: no device loss before the timeout on NVIDIA 610.62). | VX5 | Debug test runs enable the GPU-assisted validation of the validation layer for each new kernel group. Section 2.4 reports the stall; the restore procedure of section 9.5 follows the reset. |
| R11 | The memory of the process passes the budget, for example when another process (a second Alcedo process, ONNX Runtime DirectML) takes video memory. Windows places the new memory in shared system memory at about 1/300 of the speed and does not move it back while it is in use. | VX5 | The memory limit and the priorities of section 4.2. Bug #1043 of AI Denoise is the same case for two DirectML processes. |

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
- macOS (development check from VK6): the Mac worktree `~/Projects/PuerhLab-vulkan`, the build
  folder `build/macos-vulkan` (preset `macos_debug_tests` with `ALCEDO_ENABLE_VULKAN=ON`), the tools
  and the test environment of `~/Projects/vulkan_tools` (`env.sh`). The comparison tests that need
  OpenCL are not built there; the reference backend of the Develop test is Metal.
- GPU tests run with `ctest -j 1`. Only the targeted Vulkan suites and the suites of their direct
  callers run. The full suite is the decision of the user.
- Each Vulkan test runs on the RTX 3080 Laptop GPU and on Iris Xe
  (`ALCEDO_VULKAN_DEVICE`). The record names the device of each result.
- Debug test runs enable the Vulkan validation layer. A validation error is a test failure.
- Logs and evidence go to `build/tmp/vulkan_backend/<phase>/`.
- A result that is unavailable is recorded as unavailable. A skipped test is recorded as skipped.
- A comparison of a resampling kernel (a warp, a lens correction) with OpenCL uses a position
  ramp as its input (R = (x + 0.5) / width, G = (y + 0.5) / height), so a value difference is a
  difference of the source position. Texels that sample inside the source have a tolerance of
  the position divided by the image size; texels whose footprint reaches the source border have
  a tolerance in ulp of the largest coordinate, because the border makes a value difference the
  position difference itself. A pattern with detail is not compared between backends: the 2 x 2
  clamp of the Lanczos sampler (issue #713) changes a result by a few hundredths when a position
  crosses an integer row or column, so a position difference of 1e-5 pixel at such a crossing
  changes the value by that amount (VK5 record). The CPU reference tests of a sampler keep their
  patterns with detail.

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
| Curve tolerance | Largest difference of the Tone Curve, ACEScc | 1.0e-5 | `kGpuCurveTolerance`, `tone_curve_test_curves.hpp` |
| LUT cache budget | Byte budget of the LUT cache of `VulkanBackend` | 64 MiB | the OpenCL value; phase VK8 |
| LUT cache priority | `VK_EXT_memory_priority` of a LUT cache entry | 0.2 | section 4.2 (VX5); phase VK8 |
| Histogram dispatch | Workgroup size and largest workgroup count of the curve input histogram | 256; 128 | the OpenCL dispatch; phase VK8 |
| Neighborhood tolerance | Largest relative difference of the Vulkan Sharpen, Clarity, and Halation from OpenCL and from the host evaluation | 1e-5 | phase VK9 record (measured 1.3e-6) |
| Neighborhood workgroup | Workgroup of both neighborhood kernels; largest vertical radius; workgroup tile | 8 x 8; 63; 12 864 bytes | the OpenCL dispatch, `kGradeNeighborMaxTapCount` - 1; phase VK9 |
| Local tone tolerance | Largest relative difference of the Vulkan Local Laplacian stage (Shadows, Highlights) from OpenCL and Metal, ACEScc | 1e-5 | phase VK10 record (measured 5.4e-7) |
| Local tone workgroup | Workgroup of every local tone kernel; texels of one row of the zero fill; largest push constant block | 16 x 16; 1024; 88 bytes | the OpenCL dispatch; phase VK10 |
| Sharpen threshold margin | High-pass luma distance from the threshold below which a texel is left out of the comparison | 1e-5 | phase VK9 record |
| Film tolerance | Largest difference of the film print output, 8-bit codes | 0.5 | `kRampLimitCodes`, `opencl_film_print_test.cpp` |
| Transfer function tolerance | Largest difference of a Vulkan transfer function from OpenCL, `\|a - b\| / max(0.01, \|b\|)` | 1e-5 | `CurvesMatchOcioBuiltinsOn4096CodeValues`; phase VK3 record |
| OkLab tolerance | Largest `ScaledDifference` of the Vulkan ACEScc and OkLab functions from OpenCL; `OkLabToLinearAp1` | 1e-6; 4e-6 | `ColorEncodingOpenCl`; phase VK3 record (float32 limit 3.2e-6) |
| Geometry tolerance | Largest difference of the Vulkan crop and bilinear resize from OpenCL; crop at its own size | 1e-5; 0 | `OpenClGeometryUtilsTest` |
| Vulkan math functions | `CE_POW`, `CE_EXP`, `CE_EXP2`, `CE_LOG`, `CE_LOG2` of the Slang branch | `AlcedoPow`, `AlcedoExp`, `AlcedoExp2`, `AlcedoLog`, `AlcedoLog2` of `math.slangh`, about 1 ulp | phase VK3 record |
| Vulkan trigonometric functions | The trigonometric functions of the lens kernels | `AlcedoSin`, `AlcedoCos`, `AlcedoTan`, `AlcedoAsin`, `AlcedoAtan`, `AlcedoAtan2` of `math.slangh` (Cephes, about 1 ulp for \|x\| < 8192) | phase VK5 record |
| Highlight tolerance | Largest `ScaledDifference` of the Vulkan highlight reconstruction from OpenCL | 1e-5 | phase VK5 record (measured 9.3e-7) |
| DNG warp tolerance | Largest difference on the position ramp: footprint inside the source; footprint at the border | 1e-5; 8 ulp of the largest coordinate | `OpenClGeometryUtilsTest`; phase VK5 record |
| Lens tolerance | Largest difference on the position ramp: Lanczos footprint inside the source; every texel | 1e-5; 5e-4 (8 ulp of 640) | phase VK5 record |
| Develop tolerance | Largest `ScaledDifference` of `develop.sensor_linear` from OpenCL; the linear DNG and the lens against Metal | 1e-4 (the RCD tolerance of VK4); 1e-4 and 5e-4 | phase VK6 record |
| Develop tolerance against product Metal | Demosaic cases on the Apple M4: root mean square; largest value | 2.5e-4; 0.5 | phase VK6 record (fast math and contraction of the Metal RAW kernels; measured 1.3e-4 and 0.468) |
| MoltenVK fast math | `MVK_CONFIG_FAST_MATH_ENABLED` of the instance on macOS | 0 (never) | phase VK6 record |
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
| Memory reserve | Part of the `VK_EXT_memory_budget` budget that the pool keeps free | 512 MiB | VX5 (spill starts about 340 MiB below the budget) |
| Memory limit | Largest memory use of the process for the pool | minimum of the application limit and the budget minus the reserve, read before each new allocation | VX5 (section 4.2) |
| Memory priority, cache entries | `VkMemoryPriorityAllocateInfoEXT::priority` of `GraphImageCache`, `NodeResultCache`, `MaskTextureCache` entries | 0.2 | VX5 |
| Memory priority, working images | Priority of the images of the render in progress and of the frame image ring | 0.8 | VX5 |
| Pageable device-local memory | `VK_EXT_pageable_device_local_memory` | not enabled | VX5 (allocations do not fail at 115 % of the heap) |
| Budget at the start, RTX 3080 Laptop GPU | Budget of the 8016 MiB device-local heap with no other load | 7248 MiB | VX5 |
| Speed in system memory | Touch pass of memory that Windows placed in shared system memory, RTX 3080 Laptop GPU | 1.3 to 1.4 GB/s (video memory: about 410 GB/s) | VX5 |
