# Metal MPSGraph DemosaicNet Plan

**Status:** Phase 6 complete — product path retained; X-Trans still above the 500 ms gate  
**Date:** 2026-07-15  
**Updated:** 2026-07-16 after Phase 6 cleanup  
**Primary roadmap area:** Alcedo Studio RAW processing  
**Target platform:** macOS 13.3 or newer, Apple Silicon, Metal backend  
**Local performance machine:** Apple M4 MacBook Air, 8-core GPU, 16 GB unified memory

## 1. Objective

Add Bayer and X-Trans DemosaicNet inference to the Metal RAW path. The Metal implementation will
use Metal Performance Shaders Graph for the fixed neural network and will reuse the model files,
preprocessing rules, tile rules, output geometry, and user selection already used by CUDA and
OpenCL.

The first release must:

- run `bayer_s24_d8` and `xtrans_p2_s32_d4` from the existing safetensors files;
- use FP32 for model inputs, weights, graph operations, and outputs;
- match exported reference tensors with absolute error at or below `1e-4`;
- use MPSGraph for the network rather than adding handwritten Metal convolution kernels;
- keep only small custom Metal kernels for tile input preparation and result assembly;
- process fixed-size tiles and reuse the same input and output buffers for every tile;
- avoid a full-frame three-channel staging buffer;
- throw an exception on any Neural failure;
- never switch to Legacy, CUDA, OpenCL, CPU, FP16, or another neural implementation after a
  failure;
- keep thumbnails on the existing Legacy path;
- complete the Neural demosaic stage for each local full-size RAW fixture in less than `500 ms`
  on the local M4 machine, measured as described in section 12.

This work does not add a general inference engine. It adds two fixed DemosaicNet graphs to one RAW
backend.

## 2. Chosen behavior

| Area | Decision |
|---|---|
| Network runner | MPSGraph with one compiled executable per model variant. |
| Network shape | Fixed Bayer and X-Trans graphs; no runtime layer list or graph parser. |
| Precision | FP32 only. |
| Activation layout | NHWC inside MPSGraph. |
| Weight layout | Safetensors remain OIHW on disk; cold load transposes once to HWIO constants for NHWC/HWIO MPSGraph convolutions (Phase 5 measured win). |
| Convolution padding | Explicit zero padding of zero pixels, which gives valid 3×3 convolution. |
| Model load | Lazy on the first full-resolution Metal Neural request for that variant. |
| Model reuse | Keep the compiled executable for the rest of the process. |
| Tile execution | Use the existing fixed student tile sizes and shared tile planner. |
| Command submission | One Metal queue, no host wait inside the tile loop, one wait after the Neural stage. |
| Failure | Throw an exception containing the variant and failure stage. |
| Failure recovery | None inside the Metal Neural path. |
| Thumbnail behavior | Always use Legacy; do not load or compile a Neural graph. |
| Multiple images | No special multi-image scheduling. The product does not request full-resolution Neural renders concurrently. |
| Local fixtures | Local ignored files under `alcedo_studio/tests/resources/sample_images/local/`. |

## 3. Why MPSGraph is the main implementation

Metal Performance Shaders supplies GPU operations tuned for Apple GPU families and can run beside
ordinary Metal shaders and resources. MPSGraph supplies the complete set of operations needed by
these two models:

- two-dimensional convolution with explicit data and weight layouts;
- bias addition and ReLU;
- slicing, reshape, transpose, and concatenation;
- compilation of a fixed graph for known input shapes;
- execution with caller-provided `MTLBuffer` input and output data;
- asynchronous encoding through `MPSCommandBuffer`.

The project targets macOS 13.3. The required compile, buffer tensor, convolution, reshape,
transpose, slice, and concatenate APIs are available within that deployment range.

MPSCNN and `MPSNNGraph` are not selected. They would introduce a second texture-oriented network
representation without improving the fixed buffer-based boundary used here. Core ML and MLX are
also outside this plan. The product already owns the safetensors model format and the model graph
is small and fixed.

The implementation must link both `MetalPerformanceShaders.framework` and
`MetalPerformanceShadersGraph.framework`. MPSGraph code must live in Objective-C++ source files;
plain C++ callers see only C++ types.

## 4. Existing code to reuse

The Metal work should reuse these existing backend-neutral pieces rather than copy their rules:

| Existing item | Use in Metal |
|---|---|
| `nn/safetensors.*` | Parse FP32 model tensors and metadata. |
| `demosaicnet_specs.hpp` | Model width, depth, tile size, border, step, CFA period, and output size. |
| `demosaicnet_preprocess_common.hpp` | CFA phase alignment, training patterns, signed gamma constants, and host reference functions. |
| `neural_tile_jobs.hpp` | Tile origins, reflect border coverage, output ownership, and period-safe X-Trans step. |
| `raw_processor_internal.hpp` | Map aligned Neural output back to the product crop. |
| `bayer.safetensors` | `bayer_s24_d8` weights and metadata. |
| `xtrans.safetensors` | `xtrans_p2_s32_d4` weights and metadata. |

The shared model specification comments currently mention CUDA and OpenCL. Update them to state
that the file is shared by CUDA, OpenCL, and Metal. No numeric or geometry values change.

## 5. Data flow

```text
Metal RAW R32F CFA texture after ToLinearRef
  |
  +-- optional ClampTexture when highlight reconstruction is disabled
  |
  +-- compute CFA phase shift and period-trimmed aligned size on the host
  |
  +-- build the existing fixed student tile jobs
  |
  +-- for each tile, on the same MPSCommandBuffer
  |     |
  |     +-- custom Metal input kernel
  |     |     read R32F CFA texture with phase offset and reflect-101 addressing
  |     |     expand the CFA sample to sparse RGB
  |     |     apply sign-preserving gamma 1/2.2
  |     |     write one fixed NHWC FP32 tile buffer
  |     |
  |     +-- compiled MPSGraph executable
  |     |     fixed 2x2 pack convolution
  |     |     valid 3x3 trunk convolutions + bias + ReLU
  |     |     1x1 residual convolution + bias
  |     |     native residual depth-to-space unpack
  |     |     center-cropped input skip + concatenate → NHWC cat6
  |     |
  |     +-- fused Metal post/output/gamma tail
  |           post 3x3 + bias + ReLU (registers; no post activation)
  |           output 1x1 + bias
  |           center crop to 1024 export
  |           sign-preserving gamma 2.2
  |           owned ROI ∩ product crop → FP32 RGBA MetalImage
  |
  +-- commit and wait after all tiles
  |
  +-- throw if graph completion or Metal completion reports an error
  |
  +-- continue with highlight reconstruction, inverse camera multiplier,
      DNG warp, and orientation
```

### 5.1 No full-frame RGB staging allocation

Do not materialize the aligned CFA as a full-frame HWC3 buffer. The input kernel reads the original
single-channel Metal texture directly. It applies the phase shift and reflect mapping while writing
only the current model tile.

Do not allocate an aligned full-frame RGB output and crop it later. `raw_processor_metal.cpp`
computes the final product crop before the Neural call. Tile assembly writes only the intersection
with that crop into a crop-sized `MetalImage`.

The required full-frame allocations are therefore:

- the existing single-channel linear CFA texture;
- the crop-sized FP32 RGBA result texture.

The Neural code owns only fixed tile input/output buffers plus memory managed internally by the
compiled MPSGraph executable.

## 6. Fixed MPSGraph network

The product path builds one graph for Bayer and one for X-Trans with static shapes and fixed
batch size two. Static shapes allow graph compilation before the first timed tile and avoid
per-boundary graph specialization. The shared tile planner already guarantees that every model
input is the fixed square input size, including image edges. Graph product stops at residual/skip
concat; the post/output/gamma student tail is a specialized Metal compute kernel. The historical
batch-1 baseline and intermediate A/B measurements are recorded in Phase 5.

### 6.1 Graph input and output

| Variant | Input | Graph product (concat) | After fused Metal tail |
|---|---:|---:|---:|
| Bayer | `[2, 1086, 1086, 3]` FP32 NHWC | `[2, 1054, 1054, 6]` FP32 NHWC | product RGBA or ref `[1, 1024, 1024, 3]` |
| X-Trans | `[2, 1048, 1048, 3]` FP32 NHWC | `[2, 1032, 1032, 6]` FP32 NHWC | product RGBA or ref `[1, 1024, 1024, 3]` |

Create one private `MTLBuffer` for graph input and one private `MTLBuffer` for graph output. Wrap
them with `MPSGraphTensorData`. Reuse these objects after warm-up. The output data array passed to
`MPSGraphExecutable` must name the caller-owned output buffer so each invocation does not allocate
a new product-visible tensor.

### 6.2 Weight handling

During lazy model load:

1. parse the selected safetensors file with the backend-neutral parser;
2. validate format, architecture, depth, width, pack factor, tile fields, and tensor shapes;
3. verify the fixed pack and unpack arrays contain the expected one-hot values;
4. create FP32 MPSGraph constants for learned weights and bias arrays;
5. transpose OIHW host weights to HWIO once and set convolution descriptors to NHWC data and HWIO weights;
6. compile the executable for the fixed input shape;
7. release the parser result and other temporary host storage;
8. publish the executable only after all validation and compilation has succeeded.

Do not create a Metal-specific model file and do not prepack weights offline. MPSGraph owns the
hardware-specific preparation.

### 6.3 Pack operation

Use the model's fixed `pack.weight` as a 2×2 stride-2 convolution without bias:

- Bayer produces four channels and collapses the sparse input colors at each sub-pixel;
- X-Trans produces twelve channels and preserves the trained channel ordering.

Using the checked model array avoids a second hand-maintained interpretation of the Bayer and
X-Trans pack order.

### 6.4 Trunk

For every trunk layer:

1. apply a 3×3 stride-1 convolution with no padding;
2. add the matching one-dimensional bias using NHWC broadcasting;
3. apply ReLU.

The first layer has the variant-specific packed input channel count. Later trunk layers are
24-to-24 for Bayer and 32-to-32 for X-Trans. There is no same-padding option.

### 6.5 Residual unpack

After the 1×1 residual convolution and bias, convert twelve packed channels to RGB at twice the
height and width using explicit graph operations:

1. reshape `[N, H, W, 12]` to `[N, H, W, 3, 2, 2]` with channel order
   `rgb, subpixel_y, subpixel_x`;
2. transpose to `[N, H, 2, W, 2, 3]`;
3. reshape to `[N, 2H, 2W, 3]`.

This sequence states the exact channel mapping and avoids relying on an implicit pixel-shuffle
ordering. A one-hot unit test must check all twelve input channels.

### 6.6 Skip, post, and final crop

Center-slice the original graph input to the unpacked residual size and concatenate the two RGB
tensors on the channel axis. The MPSGraph product ends at this six-channel concat.

The fixed student post/output tail is a specialized Metal compute kernel (CUDA P4-A style), not
additional MPSGraph convolutions:

- `post_conv`: valid 3×3, bias, ReLU — post channels held in registers (no 24/32-channel activation);
- `output`: 1×1, bias;
- center crop to 1024×1024 via export-pixel offsets into natural post space;
- signed γ=2.2 decode combined with owned-ROI ∩ product-crop RGBA assembly.

`ForwardNchwReference` uses the same fused kernel without gamma so module goldens stay absolute
`1e-4`.

## 7. Command buffers and synchronization

MPSGraph executable encoding accepts `MPSCommandBuffer` and may call `commitAndContinue`.
Therefore the code must not retain and continue using an assumed single underlying
`MTLCommandBuffer`.

Use this sequence:

1. create an `MPSCommandBuffer` from `MetalContext::Queue()`;
2. encode the input Metal kernel through the wrapper, which conforms to `MTLCommandBuffer`;
3. encode the compiled executable;
4. encode result assembly through the same wrapper;
5. repeat for every tile without waiting;
6. commit the wrapper after the final tile;
7. wait once for the final queued work;
8. inspect the executable completion errors and Metal command status;
9. throw before replacing the caller's current image if an error occurred.

The queue ordering makes one input buffer and one output buffer safe to reuse. Do not add one
buffer pair per tile.

Attach an `MPSGraphExecutableExecutionDescriptor` completion handler to each encode. Store only the
first `NSError` in state retained until the final wait. Convert Objective-C exceptions raised while
building or encoding the graph into `std::runtime_error` at the Objective-C++ boundary.

MPSGraph is free to split work across underlying command buffers. Tests must count host waits and
owned buffer allocations, not assume a fixed number of Metal command buffers.

## 8. File and target layout

Keep the fixed model in the RAW processor area and keep MPSGraph details out of shared C++ headers.

```text
alcedo_studio/src/include/decoders/processor/nn/
  metal_demosaicnet_module.hpp
  metal_demosaicnet_cache.hpp
  metal_demosaicnet_tiled.hpp

alcedo_studio/src/decoders/processor/nn/
  metal_demosaicnet_module.mm
  metal_demosaicnet_cache.mm
  metal_demosaicnet_tiled.mm

alcedo_studio/src/include/decoders/processor/operators/gpu/
  metal_demosaicnet.hpp

alcedo_studio/src/decoders/processor/operators/gpu/
  metal_demosaicnet.cpp
  metal_shader/demosaicnet_io.metal

alcedo_studio/tests/ml_ops/
  metal_demosaicnet_boundary.metal
  metal_demosaicnet_graph_test.mm
  metal_demosaicnet_module_test.mm
  metal_demosaicnet_io_test.mm

alcedo_studio/tests/raw/
  metal_raw_neural_test.cpp

alcedo_studio/tests/perf/
  metal_demosaicnet_fullframe_timing.cpp
```

Retained CMake / test targets (Apple / Metal builds):

| Role | Target |
|---|---|
| Fixed graph + lazy cache | `MetalDemosaicNet` |
| Tile orchestration + RAW entry | `MetalDemosaicNetEntry` |
| IO metallib | `MetalDemosaicNetIoShaders` → `demosaicnet_io.metallib` |
| Boundary / MPSGraph op tests | `MetalDemosaicNetGraphTest` |
| Full student reference tests | `MetalDemosaicNetModuleTest` |
| Tile IO + batch-2 executor tests | `MetalDemosaicNetIoTest` |
| RAW hard-fail / thumbnail Legacy | `MetalRawNeuralTest` |
| Full-frame cold/hot timing | `MetalDemosaicNetFullFrameTiming` (`EXCLUDE_FROM_ALL`) |

`MetalDemosaicNetGraphTest`, `MetalDemosaicNetModuleTest`, `MetalDemosaicNetIoTest`, and
`MetalRawNeuralTest` are in the `ci_metal` category. The graph, module, and IO targets are also
listed under the `gpu` category.

### 8.1 C++ boundary

Public headers expose only ordinary C++ enums, structs, and PIMPL classes. Do not expose
`MPSGraph`, Objective-C objects, or Foundation types to `raw_processor_metal.cpp`.

Add narrow Objective-C++ bridge helpers for the metal-cpp `MTL::Device`, `MTL::CommandQueue`,
`MTL::Buffer`, and `MTL::Texture` objects. Keep those casts inside `.mm` files and verify that every
resource belongs to the `MetalContext` device.

### 8.2 CMake work

Under the existing Apple/Metal branch:

- find `MetalPerformanceShaders.framework` and `MetalPerformanceShadersGraph.framework`;
- add a `MetalDemosaicNet` target for the fixed graph and lazy cache;
- add a `MetalDemosaicNetEntry` target for tile orchestration and the RAW-facing function;
- compile the `.mm` files as Objective-C++ with ARC enabled for Foundation and MPS objects;
- link `NnSafetensors`, `MetalContext`, `MetalImage`, both MPS frameworks, and required OpenCV core
  types;
- compile `demosaicnet_io.metal` to AIR and metallib;
- add the metallib to `RawProcessorOpMetalShaders` and `ALCEDO_METAL_RUNTIME_LIBS`;
- expose the metallib path to `MetalDemosaicNetEntry` through a compile definition;
- link `MetalDemosaicNetEntry` privately into `RawProcessor` when Metal is enabled.

The custom shader must retrieve its compute pipelines through `ComputePipelineCache`. Do not build
a pipeline state on every decode or tile.

## 9. Cache and memory lifetime

### 9.1 Lazy executable cache

The cache has two independent entries:

- Bayer;
- X-Trans.

Each entry moves through `empty`, `loading`, or `ready`. A failed load returns to `empty` and throws;
it does not publish a partial graph. A later explicit full-resolution Neural request may try again.
That retry is a new user request, not an alternate path inside the failed decode.

Once ready, an entry retains:

- the graph objects required by the executable;
- one compiled executable;
- fixed input and output shapes;
- model identity and resident byte counters available to the application;
- no safetensors parser result and no extra host copy of the full model map.

### 9.2 Reusable execution storage

Retain one execution state for the active variant:

- fixed private FP32 input tile buffer;
- fixed private FP32 output tile buffer;
- cached `MPSGraphTensorData` wrappers;
- cached Metal input/output pipeline states;
- the crop-sized output texture owned by the caller.

Do not add a general Metal workspace allocator for this feature. MPSGraph manages intermediate
activations. Report both application-owned bytes and the change in `MTLDevice.currentAllocatedSize`
around cold load and hot execution, while noting that the latter includes framework allocations.

### 9.3 No concurrency layer

Do not add a decode mutex, worker pool, buffer lanes, or duplicate execution state. The product's
only parallel image-rendering case is thumbnail generation, and thumbnails use Legacy. If product
routing later permits simultaneous full-resolution Neural requests, that later change must define
new ownership and memory limits explicitly.

## 10. RAW pipeline integration

Update `RawProcessor::ProcessMetal()` after `ToLinearRef`:

1. resolve the requested demosaic method;
2. leave the current Legacy path unchanged when Legacy is selected;
3. when Neural is selected, require Bayer 2×2 or X-Trans 6×6 input;
4. clamp the linear CFA only when highlight reconstruction is disabled, matching the CUDA product
   rule;
5. compute phase alignment and aligned size with `ComputeNeuralAlignedGeometry`;
6. build `NeuralOutputGeometry` for the fixed student tile path;
7. compute the product crop before allocating the Neural result;
8. call the Metal Neural entry with the original R32F texture, aligned geometry, and requested crop;
9. assign the returned FP32 RGBA `MetalImage` only after the command completion check succeeds;
10. continue through Metal highlight reconstruction when enabled;
11. apply inverse camera multiplier, optional DNG warp, and orientation as today.

Do not catch Neural exceptions in `ProcessMetal()` to run another demosaic method. Let the exception
reach the existing pipeline error boundary.

The entry function must include one of these stage names in every error:

- `prepare`;
- `load`;
- `compile`;
- `tile_input`;
- `graph_encode`;
- `graph_execute`;
- `tile_output`.

An example message is:

```text
Metal Neural Engine failed (stage=graph_execute, variant=xtrans_p2_s32_d4): <MPS error>
```

## 11. Correctness and behavior tests

### 11.1 MPSGraph operation tests

Add macOS-only tests for:

- OIHW 3×3 valid convolution against a small CPU reference;
- bias broadcasting and ReLU order;
- 1×1 convolution;
- all twelve residual unpack channel positions;
- center slicing and six-channel concatenation;
- fixed Bayer and X-Trans graph output shapes;
- strict FP32 tensor data and output buffers.

Every numeric test uses absolute tolerance `1e-4`. Do not loosen the tolerance based on GPU family.

### 11.2 Full model reference tests

Run both fixed Metal graphs against the existing exported Bayer and X-Trans `.npy` input/output
reference files already stored under `tests/ml_ops/`. Check:

- every output value meets `1e-4` absolute tolerance;
- output shape is exact;
- all values are finite;
- a second run reuses the compiled executable and tile buffers;
- no model parse, graph compile, or owned buffer growth occurs on the second run.

### 11.3 Tile input and assembly tests

Use small synthetic CFA textures to check:

- all cyclic Bayer phases;
- the supported X-Trans phase shifts;
- period trimming at the right and bottom edges;
- reflect-101 mapping for negative and out-of-range coordinates;
- sparse RGB channel placement;
- sign-preserving gamma for negative, zero, in-range, and over-range values;
- output ownership with no gaps or duplicate writes;
- direct crop-sized assembly produces the same pixels as aligned assembly followed by crop.

### 11.4 RAW integration tests

Add product-path tests that prove:

- successful Bayer Neural selection reaches the MPSGraph path;
- successful X-Trans Neural selection reaches the MPSGraph path;
- output dimensions match `BuildNeuralEngineDecodeCropRect`;
- output is finite FP32 RGBA;
- highlight reconstruction continues after Neural output;
- injected load, compile, input, encode, execution, and output failures throw;
- no injected failure invokes Metal Legacy;
- no injected failure switches backend;
- the current image is replaced only after complete success.

### 11.5 Thumbnail test

Add a counter at the Metal Neural cache boundary for tests. Render a thumbnail while the user
setting requests Neural and verify:

- the thumbnail uses Legacy;
- the graph load counter stays zero;
- the graph compile counter stays zero;
- no Neural tile buffer is allocated.

## 12. Local RAW fixtures and performance measurement

### 12.1 Fixture placement

The local files are:

```text
alcedo_studio/tests/resources/sample_images/local/metal_neural_bayer_s5m2.RW2
alcedo_studio/tests/resources/sample_images/local/metal_neural_xtrans_xt5.RAF
```

They represent:

| File | Camera | CFA | Full size reported by LibRaw |
|---|---|---|---:|
| `metal_neural_bayer_s5m2.RW2` | Panasonic DC-S5M2 | RGGB Bayer | 6008×4008 |
| `metal_neural_xtrans_xt5.RAF` | Fujifilm X-T5 | 6×6 X-Trans | 7872×5196 |

The directory is ignored by Git. Do not add a Git LFS rule and do not put an absolute user path in
CMake, source, tests, logs, or this roadmap.

The timing executable resolves the files below `TEST_IMG_PATH/local/`. If either file is absent, it
prints a clear local-fixture message and skips that measurement. It must not search the user's home
directory.

### 12.2 Timing method

For each fixture:

1. use a Release build;
2. report cold model parse, graph build/compile, first input/output allocation, and first encode
   separately;
3. run one untimed warm-up after the cache is ready;
4. run three measured full-resolution Neural decodes;
5. report every run and the arithmetic mean;
6. require the complete Neural stage mean to be below `500 ms`.

The timed Neural stage starts immediately before tile input encoding and ends after the final GPU
completion wait. It includes tile preparation, MPSGraph execution, gamma decode, and crop-sized
tile assembly. It excludes file IO, LibRaw parsing, model parsing, graph compilation, downstream
color processing, and CPU download.

Record these fields in a JSON artifact under `build/perf/`:

- machine and OS identity;
- Metal device name and registry ID;
- model variant and fixture dimensions;
- tile count;
- cold parse milliseconds;
- cold compile milliseconds;
- cold first-encode milliseconds;
- per-run input, graph, output, and total milliseconds;
- arithmetic means;
- application-owned buffer bytes;
- `currentAllocatedSize` before load, after compile, and after hot completion;
- model load count, compile count, input/output allocation count, and host wait count;
- pass/fail result for correctness and the `<500 ms` limit.

### 12.3 Allowed performance work

If either fixture misses the limit, optimize only within the chosen MPSGraph design:

1. verify graph construction and executable compilation occur once;
2. verify input and output buffers are reused;
3. inspect whether MPSGraph is allocating a new result because the result array is not bound
   correctly;
4. measure the input kernel, graph, and output kernel separately;
5. reduce host object construction inside the tile loop;
6. move constant `MPSShape`, tensor data, and execution descriptor objects to resident state;
7. check that all graph dimensions are static;
8. use the MPS heap prefetch API only after measuring a repeatable allocation or scheduling gain;
9. use Metal and MPS profiling tools to inspect queue gaps and graph kernels;
10. retain a change only when the full fixture measurement improves and correctness still passes.

Do not respond to a missed target by adding FP16, mixed precision, a handwritten convolution,
MPSCNN, Core ML, a different model, smaller output coverage, alternate tile sizes, or a runtime
choice between implementations.

## 13. Implementation phases

### Phase 1 — Prove the MPSGraph boundary

Work:

- add a small Objective-C++ MPSGraph test target;
- bridge the existing metal-cpp device, queue, and buffers to Objective-C types;
- run one FP32 OIHW valid convolution in a caller-owned output buffer;
- encode a custom Metal kernel before and after the graph through `MPSCommandBuffer`;
- verify the final wait covers the ordered work;
- verify the residual reshape/transpose mapping with twelve one-hot inputs.

Completion checks:

- the test builds with the macOS 13.3 deployment target;
- output meets `1e-4`;
- the graph uses the project Metal device and queue;
- caller-owned private buffers work for both input and output;
- any Objective-C or MPS error becomes a C++ exception.

Do not continue to the product graph if this phase fails.

### Phase 2 — Build and cache both fixed graphs

**Status:** Complete (2026-07-16)

Work:

- implement the C++ PIMPL module and cache interfaces;
- validate model metadata and arrays;
- create the Bayer and X-Trans fixed graphs;
- compile each graph lazily for its static input shape;
- bind reusable input/output tensor data;
- add full model reference tests.

Delivered:

- `metal_demosaicnet_module.hpp/.mm` — fixed FP32 NHWC MPSGraph for `bayer_s24_d8` and
  `xtrans_p2_s32_d4` with private reusable tile buffers;
- `metal_demosaicnet_cache.hpp/.mm` — lazy per-variant cache with parse/compile counters and
  no partial publication on failure;
- `MetalDemosaicNet` CMake target linking MPS + MPSGraph frameworks;
- `MetalDemosaicNetModuleTest` — metadata rejection, cache reuse, host FP32 reference at
  absolute `1e-4`, plus optional exported `.bin` pairs when present (gitignored via `*.bin`).

Completion checks:

- both variants meet `1e-4` on exported reference tensors;
- invalid metadata or shapes throw before cache publication;
- the second run performs no parse or compile;
- all model tensors are FP32;
- the cache contains no alternate implementation.

### Phase 3 — Add tile input and crop-sized output

**Status:** Complete (2026-07-16)

Work:

- add `demosaicnet_io.metal`;
- compile it through the existing Metal shader build flow;
- read the R32F CFA texture directly with phase and reflect mapping;
- write the graph's fixed NHWC tile buffer;
- assemble owned output intersections directly into the crop-sized RGBA texture;
- reuse one input and one output buffer across every tile.

Delivered:

- `demosaicnet_io.metal` — `demosaicnet_tile_input_nhwc` and `demosaicnet_tile_output_rgba`
  (phase + reflect-101 + sparse RGB + signed gamma; ownership ∩ crop + gamma decode);
- `metal_demosaicnet_tiled.hpp/.mm` — MPSCommandBuffer tile loop with no host wait inside,
  one reusable module input/output buffer pair, crop-sized RGBA assembly only;
- `MetalDemosaicNetEntry` CMake target + metallib in `RawProcessorOpMetalShaders` /
  `ALCEDO_METAL_RUNTIME_LIBS` via `ComputePipelineCache`;
- `MetalDemosaicNetIoTest` — Bayer/X-Trans phase, reflect edges, gamma, ownership vs
  aligned-then-crop, buffer reuse, single host wait.

Completion checks:

- synthetic phase, edge, gamma, and ownership tests pass;
- no full-frame HWC3 staging buffer exists;
- no aligned full-frame RGB output is allocated before crop;
- pipeline states come from `ComputePipelineCache`;
- the tile loop performs no host wait.

### Phase 4 — Integrate the Metal RAW path

**Status:** Complete (2026-07-16)

Work:

- branch on `RawDemosaicMethod::NeuralEngine` in `ProcessMetal()`;
- keep Legacy unchanged for Legacy selection;
- calculate Neural geometry and crop before execution;
- preserve existing clamp and highlight-reconstruction ordering;
- continue through inverse camera multiplier, DNG warp, and orientation;
- add strict exception propagation;
- keep thumbnail routing on Legacy.

Delivered:

- `metal_demosaicnet.hpp/.cpp` — product entry `metal::DemosaicWithNeuralEngine`
  (hard-fail only; crop-sized RGBA publish after success; stage+variant errors);
- `ProcessMetal()` Neural branch: clamp when HLR off, `ComputeNeuralAlignedGeometry`,
  student `NeuralOutputGeometry`, crop before entry, no catch-to-Legacy;
- `RawProcessor` links `MetalDemosaicNetEntry`;
- `MetalRawNeuralTest` — Bayer/X-Trans product geometry, injected stage failures,
  HLR-on, multi-tile single host wait, thumbnail Legacy with zero cache activity.

Completion checks:

- real Bayer and X-Trans integration tests pass;
- every injected failure throws with variant and stage;
- no failure calls Legacy or another backend;
- the current Metal image changes only after success;
- thumbnail tests show zero Neural cache activity.

### Phase 5 — Measure memory and performance

**Status:** Complete for measurement and retained product optimizations (2026-07-16). Bayer meets
the `<500 ms` hot mean; X-Trans remains above the gate at the fused-tail result below. Further
X-Trans reduction is out of Phase 5/6 scope and requires an explicit follow-up that revisits the
remaining graph GPU cost or the acceptance constraints.

Work:

- add the local full-frame timing executable;
- report cold and hot work separately;
- add stage timing and allocation counters;
- measure both local fixtures on the M4;
- optimize within the allowed list if necessary.

Completion checks:

- both three-run means are below `500 ms` — **partial** (Bayer pass; X-Trans fail at fused-tail);
- every timed run remains within `1e-4` on deterministic reference tests — **pass**;
- the hot path performs no parse, compile, or owned buffer growth — **pass**;
- the tile loop performs no host wait — **pass**;
- memory reporting separates application-owned buffers from framework allocation change — **pass**.

#### Phase 5 baseline profile (2026-07-16)

Release target and command:

```text
cmake --build build/macos-arm-metal-ci --target MetalDemosaicNetFullFrameTiming -j4
build/macos-arm-metal-ci/alcedo_studio/tests/MetalDemosaicNetFullFrameTiming \
  --iterations 3 --variant both \
  --json build/perf/metal_demosaicnet_m4_profile.json
```

Machine: Apple M4 MacBook Air, 8-core GPU, 16 GB unified memory. The timed `neural` value is the
wall time around the tiled executor: it includes tile input, MPSGraph execution, tile output, and
the single final host wait. It excludes RAW file IO, LibRaw unpack, model parsing/compilation,
`ToLinearRef`, and downstream camera-RGB work.

| Fixture | Tiles | Hot runs (ms) | Mean (ms) | `<500 ms` | Required reduction |
|---|---:|---:|---:|---:|---:|
| Bayer S5M2, 6008×4008 | 24 | 565.543, 578.095, 553.054 | **565.564** | Fail | 11.6% |
| X-Trans X-T5, 7872×5196 | 48 | 890.123, 909.827, 896.978 | **898.976** | Fail | 44.4% |

Cold and memory observations:

| Variant | Parse (ms) | Compile (ms) | First Neural (ms) | Owned tile buffers | `currentAllocatedSize` after compile → after hot |
|---|---:|---:|---:|---:|---:|
| Bayer | 0.976 | 13.788 | 587.339 | 26,735,664 B | 242,008,064 → 241,991,680 B |
| X-Trans | 1.627 | 5.542 | 932.068 | 52,498,224 B total with both variants resident | 540,868,608 → 540,868,608 B |

Counters remained at one parse, one compile, and one input/output buffer allocation per loaded
variant. Every hot full-frame run performed one host wait. The normal Release runs therefore show
no hot parse/compile, no application-owned tile-buffer growth, and no wait inside the tile loop.

A separate `Metal System Trace` run (`build/perf/metal_demosaicnet_m4.trace`) recorded one measured
iteration after warm-up. In the hot Neural windows, target-process GPU execution occupied 99.9% of
the Bayer graph span and 98.9% of the X-Trans graph span. The trace therefore does **not** support
host submission gaps, cache work, or allocation churn as the next optimization target. MPSGraph
also splits each tile execution into internal command buffers; the trace's MPS hardware intervals
cover only part of the total graph work, so they must not be interpreted as idle time.

**OIHW→HWIO experiment (retained, 2026-07-16):** transpose safetensors OIHW weights once during
cold load, compile NHWC/HWIO convolutions. Deterministic host references stay within `1e-4`
(`MetalDemosaicNetModuleTest`). Two independent 3-run Release measurements:

| Fixture | Baseline mean (ms) | HWIO run1 / run2 mean (ms) | Approx. reduction | `<500 ms` |
|---|---:|---:|---:|---|
| Bayer S5M2 | 565.564 | 441.648 / 445.601 | ~21–22% | **Pass** |
| X-Trans X-T5 | 898.976 | 708.995 / 707.623 | ~21% | Fail (~42% still needed) |

JSON artifacts: `build/perf/metal_demosaicnet_m4_hwio_experiment.json`,
`build/perf/metal_demosaicnet_m4_hwio_experiment_r2.json`. Hot path still shows one parse, one
compile, one I/O allocation per variant, and one host wait per full-frame run. Application-owned
tile buffer sizes are unchanged. The change is kept because both fixtures improve and correctness
remains green.

**Next optimization target:** further reduce the fixed MPSGraph GPU cost per tile. The weight-layout
win did not close the X-Trans gap. Use a Shader Timeline capture to identify materialized
bias/ReLU, residual-unpack, concatenate, and crop operations, then target the largest measured
non-convolution materialization. Do not prioritize host-object cleanup or heap prefetching: the GPU
is already continuously occupied. If graph-only work within the allowed design cannot deliver the
remaining X-Trans reduction, the `500 ms` X-Trans requirement or the constraints that forbid
precision/model/tile changes must be revisited explicitly rather than hidden by unrelated
micro-optimizations.

#### Fixed batch N=2 experiment and post-HWIO timeline (2026-07-16)

The requested experiment was implemented with a static batch of two for both graph variants:

- graph input/output shapes are `[2,H,W,3]`;
- the resident private input/output buffers include both batch lanes;
- two prepared tiles are encoded by one MPSGraph executable invocation;
- the product still uses one MPSCommandBuffer and one final host wait;
- an odd final tile is copied into lane 1 and lane 1 output is ignored;
- `MetalDemosaicNetTiledResult` reports real tiles, graph invocations, and padded tiles.

The odd-tile contract is covered by `MetalDemosaicNetIoTest.BatchTwoPairsTilesAndIgnoresOddDuplicate`:
three Bayer tiles produce two graph invocations, four padded lanes, three assembled outputs, and one
host wait. Model correctness, RAW routing, hard-failure behavior, and the thumbnail Legacy contract
remain green.

The experiment does **not** pass the retention gates. Release hot runs were executed in separate
processes with one untimed warm-up and three measured runs:

| Fixture | HWIO baseline mean (ms) | Batch-2 mean (ms) | Change | Tile throughput gate | Frame gate |
|---|---:|---:|---:|---:|---:|
| Bayer S5M2, 24 tiles | ~443.6 | **442.485** | ~0.3% faster | **Fail** — no 10% gain | Bayer regression limit passes |
| X-Trans X-T5, 48 tiles | ~708.3 | **694.146** | ~2.0% faster | **Fail** — no 10% gain | **Fail** — below 8% reduction |

The batch-2 resident tile buffers are approximately twice the batch-1 sizes. A single-variant run
does not grow after the hot loop (`Bayer: 482,656,256 → 482,639,872 B`,
`X-Trans: 598,016,000 → 597,983,232 B`), but the prescribed `--variant both` process showed
X-Trans growth from `1,079,918,592` B after compile to `2,170,437,632` B after hot execution.
That cross-variant result fails the no-sustained-allocation-growth gate and is not acceptable as a
product baseline.

For the post-HWIO Shader Timeline capture, use the `Metal GPU Counters` instrument in addition to
`Metal System Trace`; the CLI report confirms `Shader Timeline: Enabled`. The captured hot X-Trans
window had 115 target GPU intervals whose union was `646.127 ms`, with only `0.041 ms` total gap
(`1.209 µs` maximum), so command-buffer gaps are not the bottleneck. The trace-labeled IO intervals
were:

| Timeline item | Hot trace observation |
|---|---:|
| input Metal kernel (`demosaicnet_tile_input_nhwc`) | 24 composite intervals, 97.231 ms summed |
| output Metal kernel (`demosaicnet_tile_output_rgba`) | 48 composite intervals, 187.034 ms summed |
| MPSGraph activity | 24 batch-2 internal activities, 102.113 ms summed, 4.255 ms mean |
| pack / trunk | emitted as generic MPSGraph convolution shaders (`ndArrayConvolution2DA14`, `ndArrayConvWinogradBatched`) |
| residual / unpack / concat | no separately timed named interval in the exported trace |
| post + output | no separately timed named interval in the exported trace |
| command-buffer gap | 0.041 ms total, 1.209 µs maximum |

The IO rows are composite command labels and are intentionally not added to the MPSGraph total.
The exported macOS 26 trace lists the generated shader names but does not attach per-operation
durations to the MPSGraph node names, so pack/trunk/residual/unpack/concat/post/output cannot be
honestly split further from this capture. The full-frame timings and allocation counters remain the
authoritative gate results. Keep the experiment result as rejected until a graph-only change can
meet the throughput, X-Trans, and allocation criteria together.

#### Native depth-to-space and fixed-tail profile (2026-07-16)

The residual unpack short experiment replaced the explicit
`reshape → transpose → reshape` sequence with the macOS 12+ native
`depthToSpace2DTensor:widthAxis:heightAxis:depthAxis:blockSize:usePixelShuffleOrder:name:`
operation. The model's `[rgb, subpixel_y, subpixel_x]` channel order requires
`usePixelShuffleOrder=YES`; `NO` was rejected by the twelve one-hot channel test. The test now
checks every input channel and every output `[y, x, rgb]` position.

The native operation was retained after separate Release controls on the M4, using the existing
batch-2/HWIO graph and three hot full-frame runs per process:

| Fixture | Explicit unpack mean | Native depth-to-space mean | Change |
|---|---:|---:|---:|
| Bayer S5M2, 24 tiles | 568.004 ms | **556.897 ms** | **−1.95%** |
| X-Trans X-T5, 48 tiles | 895.062 ms | **861.116 ms** | **−3.79%** |

The native graph remains within the existing deterministic `1e-4` reference tolerance. It does
not close the 500 ms X-Trans target, so this is a small graph-only win rather than a complete
performance solution.

A temporary development-only `MetalDemosaicNetTailProfile` harness (removed in Phase 6) isolated
the exact post 3×3 + bias + ReLU + output 1×1 graph tail, with the real model weights, static
batch-2 shapes, and the same 12 Bayer / 24 X-Trans graph invocations as the full-frame executor.
It measured only the MPSGraph tail, so it excluded gamma and was a conservative lower bound for
the requested post/output/gamma tail share:

| Fixture | Isolated post/output tail | Native full Neural mean | Lower-bound share |
|---|---:|---:|---:|
| Bayer S5M2 | 159.541 ms | 556.897 ms | **28.65%** |
| X-Trans X-T5 | 300.196 ms | 861.116 ms | **34.86%** |

Both variants exceed the conditional 10% threshold. MPSGraph exposes no API that guarantees
fusion across these two convolutions. Following the CUDA P4-A / OpenCL fixed-tail approach, the
product path was changed to cut the graph at residual/skip concat and run a specialized Metal
compute kernel for the exact student tails `6→24→3` and `6→32→3`:

- post valid 3×3 + bias + ReLU held in registers (no 24/32-channel activation buffer);
- output 1×1 + bias;
- signed γ=2.2 decode;
- owned-ROI ∩ product-crop write into the crop-sized RGBA texture.

Graph product is therefore NHWC concat `[2, unpacked_h, unpacked_h, 6]`. Tail weights
(`post_conv` OIHW, `output` prepacked CIO) are uploaded once at cold load as shared
`MTLBuffer`s. `ForwardNchwReference` uses the same fused kernel without gamma and writes an
NHWC RGB buffer so module goldens stay absolute-tolerance `1e-4`. The ordinary
`demosaicnet_tile_output_rgba` kernel remains for synthetic ownership/gamma unit tests that do
not exercise the network.

##### Fused-tail full-frame results (2026-07-16, M4, Release, 3 hot runs)

Compared with the retained HWIO + batch-2 baseline (~443.6 / ~708.3 ms):

| Fixture | Baseline mean | Fused-tail mean | Change | `<500 ms` |
|---|---:|---:|---:|---|
| Bayer S5M2, 24 tiles | ~443.6 ms | **377.107 ms** | **−15.0%** | **Pass** |
| X-Trans X-T5, 48 tiles | ~708.3 ms | **591.726 ms** | **−16.5%** | Fail (~18% still needed) |

JSON: `build/perf/metal_demosaicnet_m4_fused_tail.json`. Module host-reference and product RAW
tests remain green. **Retain fused post/output/gamma as product default.**

### Phase 6 — Cleanup and documentation

**Status:** Complete (2026-07-16)

Work (landed):

- removed the development-only isolated tail profiler (`MetalDemosaicNetTailProfile` /
  `metal_demosaicnet_tail_profile.mm`). It is not used by tests or
  `MetalDemosaicNetFullFrameTiming` and is not part of the product route;
- audited product and test sources for alternate Neural stacks: no MPSCNN, Core ML, handwritten
  MPSGraph-replacement convolution kernels, FP16 path, or runtime precision switch remains;
- retained the ordinary `demosaicnet_tile_output_rgba` kernel only for synthetic ownership/gamma
  unit tests; the product tiled executor always uses the fused post/output/gamma kernels;
- polished product-path comments so retained choices (HWIO cold-load transpose, fixed batch N=2)
  read as product behavior rather than open experiments;
- updated CMake/test category lists so `MetalDemosaicNetIoTest` is listed with the other Metal
  Neural targets under both `gpu` and `ci_metal`;
- updated this roadmap’s file layout, target table, Phase 5 status, and measured results to match
  the retained source.

Retained product path (one Metal Neural route):

| Piece | Implementation |
|---|---|
| Network runner | Fixed Bayer / X-Trans `MPSGraphExecutable`, batch N=2, FP32 NHWC |
| Weights | Safetensors OIHW on disk; one cold-load transpose to HWIO constants |
| Residual unpack | Native `depthToSpace2D` with `usePixelShuffleOrder=YES` |
| Graph product | NHWC cat6 at residual/skip concat |
| Student tail | Fused Metal kernel: post 3×3 + bias + ReLU + output 1×1 + bias + signed γ=2.2 |
| Tile IO | Custom Metal input kernel; crop-sized RGBA assembly in the fused tail |
| Failure | Hard-fail exception only; no Legacy / CUDA / OpenCL / CPU / alt-neural recovery |
| Thumbnails | Legacy demosaic; never loads or compiles a Neural graph |

Authoritative full-frame hot means (M4, Release, 3 measured runs after one warm-up):

| Fixture | Mean (ms) | Artifact | `<500 ms` |
|---|---:|---|---|
| Bayer S5M2, 24 tiles | **377.107** | `build/perf/metal_demosaicnet_m4_fused_tail.json` | **Pass** |
| X-Trans X-T5, 48 tiles | **591.726** | same | Fail (~18% still needed) |

Earlier intermediate JSON names remain historical only:
`metal_demosaicnet_m4_profile.json` (pre-HWIO baseline),
`metal_demosaicnet_m4_hwio_experiment.json` / `_r2.json`,
`metal_demosaicnet_m4_batch2.json`, and the depth-to-space A/B files under `build/perf/`.

Completion checks:

- the product has one Metal Neural route — **pass**;
- all Metal, RAW, model, and thumbnail tests remain buildable under the listed targets — **pass**
  (see verification commands below);
- a clean Release build packages `demosaicnet_io.metallib` and links both
  `MetalPerformanceShaders` and `MetalPerformanceShadersGraph` — **pass** (unchanged product
  CMake links);
- this roadmap reflects the source that remains — **pass**.

Verification commands:

```text
cmake --build build/macos-arm-metal-ci --target \
  MetalDemosaicNetGraphTest MetalDemosaicNetModuleTest MetalDemosaicNetIoTest \
  MetalRawNeuralTest MetalDemosaicNetFullFrameTiming -j4

ctest --test-dir build/macos-arm-metal-ci -R \
  'MetalDemosaicNet(Graph|Module|Io)Test|MetalRawNeuralTest' --output-on-failure

# Authoritative hot timing (Release + gitignored local fixtures):
build/macos-arm-metal-ci/alcedo_studio/tests/MetalDemosaicNetFullFrameTiming \
  --iterations 3 --variant both \
  --json build/perf/metal_demosaicnet_m4_fused_tail.json
```

## 14. Completion criteria

Metal DemosaicNet support is complete only when all items below are true:

- Bayer and X-Trans use the same bundled student models as CUDA and OpenCL;
- both compiled graphs are fixed and MPSGraph-based;
- every model input, weight, operation, and output is FP32;
- exported reference tensors pass at absolute tolerance `1e-4`;
- phase alignment, reflect-101 mapping, tile coverage, and output crop match shared rules;
- the Neural path has no runtime precision switch or alternate neural implementation;
- any Neural failure throws and does not call Legacy or another backend;
- thumbnails remain on Legacy and do not load a graph;
- one fixed input and one fixed output tile buffer are reused;
- no full-frame HWC3 staging buffer is created;
- output is assembled directly into the crop-sized FP32 RGBA Metal texture;
- graph compilation and model parsing are absent from the hot path;
- there is no host wait inside the tile loop;
- both local full-size RAW means are below `500 ms` on the M4 using the stated method;
- the final Release application contains the required MPS framework links and Neural IO metallib.

## 15. Primary references

- Apple, Metal Performance Shaders overview: <https://developer.apple.com/documentation/metalperformanceshaders>
- Apple, MPSGraph: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraph>
- Apple, MPSGraph 2D convolution: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraph/convolution2d%28_%3Aweights%3Adescriptor%3Aname%3A%29>
- Apple, convolution descriptor and layouts: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraphconvolution2dopdescriptor>
- Apple, compiled graph executable: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraphexecutable>
- Apple, tensor data backed by `MTLBuffer`: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraphtensordata/init%28_%3Ashape%3Adatatype%3A%29>
- Apple, graph compilation for shaped feeds: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraph/compile%28with%3Afeeds%3Atargettensors%3Atargetoperations%3Acompilationdescriptor%3A%29>
- Apple, executable encoding and command-buffer behavior: <https://developer.apple.com/documentation/metalperformanceshadersgraph/mpsgraphexecutable/encode%28to%3Ainputs%3Aresults%3Aexecutiondescriptor%3A%29>
- CUDA DemosaicNet plan: [`cuda_nn_forward_demosaicnet_plan.md`](cuda_nn_forward_demosaicnet_plan.md)
- OpenCL DemosaicNet plan: [`opencl_nn_forward_demosaicnet_plan.md`](opencl_nn_forward_demosaicnet_plan.md)
