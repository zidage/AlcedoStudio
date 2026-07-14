# OpenCL NN Forward DemosaicNet Migration and Alignment Plan

**Status:** Design decisions locked; ready for implementation  
**Date:** 2026-07-14  
**Scope:** Port the CUDA DemosaicNet forward path to OpenCL without introducing a general-purpose inference runtime.

## 1. Objective

Implement Bayer and X-Trans DemosaicNet inference on the user-selected OpenCL backend while preserving the CUDA behavior, tile coverage, preprocessing, postprocessing, and output behavior.

The implementation must:

- run the same trained Bayer and X-Trans student models used by CUDA;
- use pure FP32 weights, activations, and accumulation;
- match exported reference tensors with absolute tolerance `1e-4`;
- process the two CUDA performance-harness RAW fixtures in less than `500 ms` each on the local GPU, measured as the arithmetic mean of three hot runs;
- keep OpenCL Neural failures inside the OpenCL backend by falling back to OpenCL Legacy;
- import only the required CLBlast direct-convolution kernel material, not the complete CLBlast library;
- remain a hard-coded DemosaicNet implementation rather than becoming a graph executor.

The target local device is an NVIDIA GeForce RTX 3080 Laptop GPU with 8 GB VRAM. CUDA remains the behavioral and performance comparison on this machine, but the OpenCL implementation must stay portable across conforming OpenCL devices supported by the project.

## 2. Locked design decisions

| Area | Decision |
|---|---|
| Performance fixtures | Use the same Nikon D800E Bayer RAW and Fujifilm X-T5 X-Trans RAW as the CUDA harness. |
| Timing method | Release build; one warm-up; three measured runs; arithmetic mean. |
| Performance gate | Both full-image means must be `< 500 ms`. |
| Cold cost | Program compilation, model parsing, weight packing, and initial upload are reported separately and excluded from the hot mean. |
| Numeric mode | FP32 only, including accumulation. |
| Correctness | Exported reference tensors use absolute tolerance `1e-4`. |
| Tensor layout | Persistent buffer-backed `NHWC4` activations through the network. |
| Convolution | Minimal CLBlast-derived single-kernel direct convolution for 3x3 layers; specialized project kernel for C4 1x1 layers. |
| Fusion | Fuse bias and ReLU into convolution; fuse structural unpack, skip, post, output, and gamma operations where data dependencies allow. |
| Network representation | Hard-coded Bayer and X-Trans modules; no runtime graph or generic layer list. |
| Tiling | Extract and share the exact CUDA student tile planner and coverage rules. |
| Queueing | One in-order queue, one active Neural decode workspace, no per-tile `clFinish`, and one wait at the end of the Neural stage. |
| Full-image concurrency | No parallel full-image Neural decoding. |
| Tuning | Development-only offline tuning; selected parameters become build-time kernel options. No product runtime tuner or tuning cache. |
| OpenCL failure | OpenCL Neural falls back only to OpenCL Legacy. |
| CUDA failure | CUDA Neural falls back only to CUDA Legacy. |
| Terminology | All new or modified OpenCL work uses `reference` for expected-result data and checks, and uses plain terms such as `behavior`, `requirements`, or `rules`. Historical files outside this work are not renamed. |

## 3. Current state and gaps

### 3.1 CUDA implementation to preserve

The CUDA implementation in `alcedo_studio/src/include/decoders/processor/nn/demosaicnet_module.hpp` owns CUDA buffers and streams directly. Its two fixed networks are:

- Bayer: C24 trunk, depth 8, input tile 1086, output tile 1024, padding 32, border 31, step 1024, CFA period 2.
- X-Trans: C32 trunk, depth 4, input tile 1048, output tile 1024, padding 12, border 12, step 1020, CFA period 6.

The CUDA path already defines the required observable behavior:

- phase alignment to the model's training pattern;
- signed gamma encoding with exponent `1 / 2.2` after linear-reference normalization;
- phase-aware reflect-101 tile packing;
- hard-coded residual and post-network structure;
- gamma decoding with exponent `2.2`;
- conditional clamping based on highlight-reconstruction state;
- exact student tile coverage and overlap behavior;
- lazy model parsing, validation, upload, and reuse.

### 3.2 Existing OpenCL infrastructure

The project already has the correct foundation:

- `OpenClContext` selects the device and owns a single in-order queue;
- `OpenClImage` is a buffer-backed HWC allocation, not an OpenCL image object;
- `OpenClProgramLibrary` owns compiled-program caching;
- `OpenClBackendProgramRegistry` centrally activates module manifests;
- the OpenCL RAW path already implements Bayer and X-Trans Legacy processing.

The missing pieces are a reusable OpenCL NN buffer layer, the fixed DemosaicNet modules, shared backend-neutral model/tile utilities, product routing, and representative correctness/performance coverage.

### 3.3 Lifecycle correction required

`WarmUpOpenClRuntime()` currently warms every registered program. Neural programs must remain lazy because they are large, optional, and used only when the user selects OpenCL Neural.

Change runtime warm-up to compile only programs marked `required_at_startup`. Register all DemosaicNet programs with `required_at_startup = false`. This keeps application startup independent of Neural model availability while retaining centralized registration.

## 4. Target architecture

```text
RawProcessor OpenCL branch
  |
  +-- ToLinearRef and existing white-balance preparation
  |
  +-- user selected OpenCL Neural
        |
        +-- preserve original linear CFA input
        +-- phase-align and period-trim private Neural input
        +-- signed gamma encode + HWC-to-NHWC4 pack
        +-- shared student tile planner
        +-- OpenCL Bayer or X-Trans fixed module
        |     |
        |     +-- resident prepacked FP32 weights
        |     +-- two-slot grow-only workspace
        |     +-- direct 3x3 convolution + bias + ReLU
        |     +-- specialized 1x1 convolution
        |     +-- fused structural/post/output kernels
        |
        +-- enqueue all tiles on one in-order queue
        +-- final Neural-stage wait
        +-- common crop / highlight reconstruction / color flow
        |
        +-- on any Neural setup or execution failure
              |
              +-- discard private Neural output
              +-- run OpenCL Legacy from preserved linear CFA input
```

CUDA follows the equivalent routing rule independently: CUDA Neural failure enters CUDA Legacy and never switches to OpenCL.

## 5. File and target layout

Names may be adjusted to existing local conventions during implementation, but ownership boundaries must remain as follows.

### 5.1 Backend-neutral model utilities

```text
alcedo_studio/src/include/nn/safetensors.hpp
alcedo_studio/src/nn/safetensors.cpp
alcedo_studio/src/include/decoders/processor/nn/demosaicnet_specs.hpp
alcedo_studio/src/include/decoders/processor/nn/demosaicnet_preprocess_common.hpp
alcedo_studio/src/include/decoders/processor/neural_tile_jobs.hpp
```

Responsibilities:

- parse safetensors and expose validated host-side tensor descriptors without CUDA dependencies;
- define Bayer/X-Trans model topology and tile constants once;
- expose CFA phase alignment and signed-gamma constants independently of a GPU API;
- own tile planning, overlap, bounds, and coverage proof independently of CUDA/OpenCL.

Keep compatibility includes or narrow aliases where necessary so the CUDA migration is behavior-preserving and reviewable.

### 5.2 Reusable OpenCL NN primitives

```text
alcedo_studio/src/include/opencl/nn/device_buffer.hpp
alcedo_studio/src/include/opencl/nn/tensor_view.hpp
alcedo_studio/src/include/opencl/nn/workspace.hpp
alcedo_studio/src/include/opencl/nn/convolution.hpp
alcedo_studio/src/opencl/nn/device_buffer.cpp
alcedo_studio/src/opencl/nn/workspace.cpp
alcedo_studio/src/opencl/nn/convolution.cpp
```

This layer owns only reusable mechanics:

- move-only `cl_mem` buffers with explicit byte/element capacity;
- non-owning tensor views containing dimensions, blocked strides, and logical channels;
- grow-only workspace reservation;
- kernel argument binding and enqueue helpers;
- OpenCL event timing hooks used by the development harness;
- direct 3x3 and specialized 1x1 convolution dispatch.

It must not know Bayer/X-Trans topology, model filenames, RAW metadata, tile planning, or fallback policy.

### 5.3 RAW-domain DemosaicNet implementation

```text
alcedo_studio/src/include/decoders/processor/nn/opencl_demosaicnet_module.hpp
alcedo_studio/src/include/decoders/processor/nn/opencl_demosaicnet_cache.hpp
alcedo_studio/src/decoders/processor/nn/opencl_demosaicnet_module.cpp
alcedo_studio/src/decoders/processor/nn/opencl_demosaicnet_cache.cpp
alcedo_studio/src/include/decoders/processor/operators/gpu/opencl_demosaicnet.hpp
alcedo_studio/src/decoders/processor/operators/gpu/opencl_demosaicnet.cpp
alcedo_studio/src/decoders/processor/operators/gpu/opencl_demosaicnet_programs.cpp
alcedo_studio/src/decoders/processor/operators/gpu/opencl_shader/demosaicnet_structural.cl
```

This layer owns:

- fixed Bayer and X-Trans topology;
- model metadata validation;
- one-time weight conversion and upload;
- lazy model cache and failed-load state;
- tile packing and assembly;
- structural residual/post layers;
- product-facing Neural execution and error reporting.

### 5.4 Minimal CLBlast-derived source

```text
alcedo_studio/src/third_party/clblast_min/LICENSE
alcedo_studio/src/third_party/clblast_min/UPSTREAM.md
alcedo_studio/src/third_party/clblast_min/xconvgemm_direct_f32_nhwc4.cl
```

`UPSTREAM.md` must record:

- upstream repository: <https://github.com/CNugteren/CLBlast>;
- upstream tag `1.7.0` and commit `ca2fc3cb09d4917cc72d4ca661d30296865a4afc`;
- source fragments used from `xconvgemm_part1.opencl`, `xconvgemm_part2.opencl`, and the direct GEMM helper fragments required by the single-kernel path;
- every local layout, indexing, fusion, and build-option modification;
- the Apache-2.0 notice and preservation requirements.

Do not import the CLBlast C++ API, runtime database, host tuner, indirect im2col path, other precisions, unrelated BLAS levels, tests, samples, or build system.

## 6. Tensor and weight layout

### 6.1 Activations

Use buffer-backed `NHWC4` for every persistent activation:

```text
index = (((n * height + y) * width + x) * channel_blocks + cb)
value = float4(channel cb*4 ... cb*4+3)
```

Logical channel counts and physical blocks:

| Tensor | Logical channels | Physical blocks |
|---|---:|---:|
| Bayer input | 4 | 1 |
| X-Trans input | 12 | 3 |
| Bayer trunk | 24 | 6 |
| X-Trans trunk | 32 | 8 |
| Residual output | 12 | 3 |
| Post input | 6 | 2, with two padded lanes |
| RGB output | 3 | one `float4` while internal; write three HWC values at boundary |

Convert the existing HWC OpenCL RAW buffer once at the Neural boundary. Do not transpose to NCHW, create per-layer OpenCL images, or materialize im2col buffers.

### 6.2 Weights

Parse the original FP32 tensors once and prepack convolution weights into an `OHWI4o4i`-style blocked representation:

```text
[output_block][kernel_y][kernel_x][input_block][output_lane][input_lane]
```

Rules:

- preserve the exact CUDA tensor semantics before blocking;
- zero-fill only physical padding lanes;
- keep logical channel counts in tensor metadata so padded lanes cannot affect output;
- upload the packed allocation once per model/context;
- validate tensor name, rank, dimensions, data type, byte range, and complete model topology before publishing the cache entry.

The cache must never expose a partially initialized model.

## 7. Kernel design

### 7.1 Direct 3x3 convolution

Derive the FP32 single-kernel path from CLBlast `Xconvgemm`, specialized for the project's fixed NHWC4 buffer layout.

Required behavior:

- direct spatial convolution with no im2col allocation;
- vectorized `float4` channel loads and blocked weight loads;
- compile-time kernel geometry and channel blocking;
- bias addition in the accumulator epilogue;
- optional ReLU in the same epilogue;
- no implicit precision conversion;
- bounds-safe writes for edge work-groups.

The offline parameter search must cover the relevant CLBlast parameters:

```text
KWID, MDIMAD, MDIMCD, NDIMBD, NDIMCD,
PADA, PADB, VWMD, VWND, WGD
```

Only configurations that pass the CPU/reference correctness filter are eligible for timing. Tune Bayer and X-Trans shapes separately when their best settings differ.

### 7.2 Specialized 1x1 convolution

Implement a small project-owned NHWC4 1x1 kernel for channel transforms where direct 3x3 machinery adds unnecessary address arithmetic. It must support:

- C4-aligned input/output blocks;
- bias;
- optional ReLU;
- logical-channel masking for the padded C6-to-C8 post input;
- the same FP32 accumulation and `1e-4` reference requirement.

### 7.3 Structural fusion

Use dedicated fixed-network kernels to reduce intermediate memory traffic:

- pack/phase/gamma input where a single pass is legal;
- combine residual unpack and skip connection;
- form the post-network C6 logical input directly in an eight-lane allocation;
- combine final RGB extraction, gamma decode, conditional clamp, and tile assembly when the highlight-reconstruction rules permit it.

Fusion must not change operation ordering where that would violate the `1e-4` reference tolerance. Each fused kernel needs a CPU/reference test covering negative values, values above one, padded channels, and image edges.

### 7.4 Synchronization

All tile commands are enqueued in dependency order on the existing in-order queue. Do not call `clFinish` or block on an event inside the tile loop.

Use one final wait when the Neural output becomes a CPU-visible or externally synchronized dependency. Profiling builds may retain events until timing data is collected, but product execution must not add event waits between layers.

## 8. Workspace and cache ownership

### 8.1 Workspace

Reserve a grow-only workspace sized for the largest supported tile and selected variant:

- packed boundary input;
- two ping-pong trunk activation slots;
- structural/post temporary storage that cannot be fused;
- tile output staging when direct assembly is not legal.

Expose an allocation-generation counter for tests and performance diagnostics. After warm-up, three measured runs must leave this counter unchanged.

Only one Neural decode may use the workspace at a time. Enforce this with the existing RAW processing serialization or a narrow mutex at the model/workspace boundary; do not duplicate full-image workspaces to manufacture concurrency the product does not expose.

### 8.2 Lazy model cache

Cache entries are keyed by:

- OpenCL context/device identity;
- model variant: Bayer or X-Trans;
- resolved model path and file identity sufficient to avoid stale reuse.

An entry transitions atomically through unloaded, ready, or failed state. A failed Neural load remains cold for that attempt and routes to same-backend Legacy. Diagnostic state must retain the concrete parse, validation, build, allocation, or enqueue failure without repeatedly rebuilding during one decode.

## 9. Program registration and compilation lifecycle

Add a DemosaicNet-specific manifest and activate it only from `RegisterOpenClBackendPrograms(...)`.

Suggested stable program names:

```text
raw_demosaicnet_conv_bayer
raw_demosaicnet_conv_xtrans
raw_demosaicnet_structural
```

Requirements:

- all are `required_at_startup = false`;
- the program library remains generic and contains no RAW-specific source paths;
- model/cache constructors do not register programs;
- kernels are created once when the lazy model becomes ready and reused for its lifetime;
- no program build or kernel creation occurs per tile or per layer invocation;
- offline-selected tuning values enter the program build options as constants;
- build logs include stable program name, device, driver, and selected build options.

Although `cl_kernel` arguments are mutable, reuse is safe because this design permits only one active Neural decode on the in-order queue. If those execution rules change, kernel pooling or per-dispatch kernel objects must be designed explicitly rather than silently sharing mutable arguments.

## 10. RAW pipeline integration and fallback

### 10.1 Branch point

Branch to OpenCL Neural after the existing linear-reference and white-balance preparation, matching CUDA preprocessing order.

Keep the original linear CFA buffer intact until Neural succeeds. Phase alignment, period trimming, gamma packing, and tile staging operate on private Neural storage. This guarantees that Legacy fallback receives the same input it would have received if Neural had not been attempted.

### 10.2 Success path

On success:

1. determine Bayer or X-Trans variant from existing RAW metadata;
2. validate the model and input pattern;
3. align the CFA phase and trim to the variant period;
4. generate shared tile jobs;
5. enqueue all Neural tiles and assemble the aligned RGB output;
6. restore the product-visible crop/dimensions;
7. continue through the existing highlight-reconstruction and color pipeline.

### 10.3 Failure path

Any failure before the Neural result is committed routes to OpenCL Legacy:

- missing or invalid model;
- unsupported metadata/pattern;
- program build failure;
- weight allocation/upload failure;
- workspace allocation failure;
- kernel argument or enqueue failure;
- final execution failure detected at synchronization.

The fallback must:

- remain on OpenCL;
- use the preserved original linear CFA buffer;
- clear/discard partial Neural output;
- emit one actionable diagnostic containing variant and failure stage;
- produce the normal OpenCL Legacy result and downstream dimensions.

Do not fall back from OpenCL to CUDA or CPU as part of this feature. CUDA follows its own CUDA Neural-to-CUDA Legacy rule.

## 11. Correctness and regression test plan

All test names must state the behavior or requirement being checked. Do not add vague execution-only tests.

### 11.1 Backend-neutral tests

- safetensors accepts the known student model metadata and tensor inventory;
- malformed rank, dimension, data type, byte range, duplicate tensor, and missing tensor are rejected;
- shared Bayer/X-Trans tile planners reproduce the existing CUDA job list exactly;
- coverage tests prove every output pixel is written once according to the established overlap rule;
- CFA phase alignment and reflect-101 mapping cover all Bayer and X-Trans phases.

### 11.2 OpenCL NN primitive tests

- direct 3x3 convolution matches a CPU implementation for every production channel shape;
- bias and ReLU epilogues match the unfused CPU operation order;
- 1x1 convolution ignores padded logical channels;
- boundary work-groups never write outside the requested tensor view;
- grow-only workspace reuses allocations once capacity is sufficient;
- enqueue helpers do not synchronize inside a multi-layer sequence.

Use small deterministic tensors plus adversarial negative, over-range, and non-multiple work-group dimensions.

### 11.3 DemosaicNet module tests

- run all four existing exported student reference input/output pairs: two Bayer and two X-Trans;
- require absolute error `<= 1e-4` for every value;
- report max absolute error, first failing coordinate, logical channel, expected value, and actual value;
- prove the second invocation reuses compiled programs, kernels, weights, and workspace;
- reject a model whose topology does not match the fixed variant;
- verify signed gamma behavior for negative and over-range input.

When both CUDA and OpenCL are built, add a diagnostic comparison over the same reference tensors. The authoritative acceptance remains each backend's reference requirements, avoiding accidental dependence on identical floating-point reduction details.

### 11.4 RAW integration tests

- OpenCL Neural demosaics a real Bayer RAW to finite RGB with expected dimensions;
- OpenCL Neural demosaics a real X-Trans RAW to finite RGB with expected dimensions;
- phase-aligned crop and final crop preserve the expected sensor-visible geometry;
- highlight-reconstruction enabled output preserves required over-range values;
- injected model-load failure falls back to OpenCL Legacy and does not enter CUDA;
- injected program-build/enqueue failure falls back to OpenCL Legacy from the untouched linear CFA input;
- successful Neural execution does not invoke Legacy;
- a multi-tile decode performs no per-tile host wait.

Heap-allocate `LibRaw` processors in new integration fixtures to keep large decoder state off the test stack.

## 12. Performance harness and offline tuning

### 12.1 Development kernel harness

Add a development-only kernel harness that:

- enumerates the bounded parameter grid for production layer shapes;
- rejects configurations that fail the reference tolerance before timing;
- warms each valid configuration;
- records kernel-event time by layer and full fixed module;
- writes a machine-readable result with device, driver, OpenCL version, build options, tensor shape, workspace bytes, and parameter set;
- selects independent Bayer/X-Trans constants if necessary.

The harness is not linked into the product and no tuning database is shipped.

The selected configuration should also be checked against nearby parameters to avoid committing a result caused by timing noise. The product still uses one fixed configuration per program variant.

### 12.2 Full RAW performance harness

Use exactly these fixture classes from the CUDA harness:

- Nikon D800E Bayer RAW;
- Fujifilm X-T5 X-Trans RAW.

For each fixture:

1. record cold program compile, model validation, weight packing, upload, and first workspace allocation separately;
2. run one complete warm-up decode;
3. run three complete measured decodes;
4. compute the arithmetic mean of the three measurements;
5. assert the mean is below `500 ms`;
6. assert no allocation generation changes during measured runs;
7. record individual samples, mean, output dimensions, max reference error where applicable, and stage timings.

Report CUDA timing from the same machine/run configuration when CUDA is available. The CUDA result is a comparison and optimization guide; the hard OpenCL acceptance gate is the two `<500 ms` means.

Keep the performance gate out of default CI because it is hardware-specific. Make the harness an explicit Release target and preserve its JSON results as local/benchmark artifacts rather than source fixtures.

## 13. Implementation phases and exit gates

### Phase 1 — Extract shared requirements

Work:

- move safetensors parsing to a backend-neutral target;
- extract model specs, CFA alignment, and tile planning;
- adapt CUDA callers without changing CUDA outputs or tile lists;
- add backend-neutral parser/planner tests.

Exit gate:

- CUDA builds unchanged in behavior;
- exact CUDA tile-list equivalence tests pass for Bayer and X-Trans;
- parser tests pass without linking CUDA.

### Phase 2 — Register programs and import the minimal CLBlast source

Work:

- add Apache-2.0 license and upstream provenance;
- extract only the FP32 single-kernel direct-convolution material;
- add DemosaicNet program manifest;
- change runtime warm-up to required programs only;
- add program-library lifecycle tests.

Exit gate:

- no full CLBlast dependency or build system exists in the tree;
- DemosaicNet programs remain uncompiled at application startup;
- each program compiles once on first explicit use and is reused.

### Phase 3 — Implement OpenCL NN primitives

Work:

- add device buffer, tensor view, workspace, 3x3 dispatch, and 1x1 dispatch;
- implement `OHWI4o4i` packing;
- add deterministic CPU/reference operator tests;
- add allocation-generation and synchronization instrumentation.

Exit gate:

- every production convolution shape passes `1e-4` tolerance;
- fused bias/ReLU ordering is verified;
- no invocation performs an implicit program build;
- workspace reuse test shows no reallocation after reserve.

### Phase 4 — Implement fixed Bayer and X-Trans modules

Work:

- implement hard-coded topology and weight binding;
- implement structural/post kernels and safe fusions;
- add lazy model cache;
- run four exported student reference pairs.

Exit gate:

- all reference values meet `1e-4` absolute tolerance;
- invalid metadata cannot publish a cache entry;
- second invocation performs no model parse, weight upload, kernel creation, or workspace allocation.

### Phase 5 — Add tiled product execution

Work:

- integrate shared tile jobs;
- implement phase-aware reflect-101 packing;
- enqueue the complete tile loop asynchronously on the in-order queue;
- assemble final RGB while preserving established overlap behavior.

Exit gate:

- multi-tile Bayer and X-Trans coverage tests pass;
- no wait or `clFinish` occurs inside the tile loop;
- aligned output matches reference dimensions and phase.

### Phase 6 — Integrate RAW routing and fallback

Work:

- add OpenCL Neural selection in `raw_processor_opencl.cpp` at the matched CUDA stage;
- preserve the pre-Neural linear CFA input;
- continue through common crop/highlight/color processing on success;
- add injected-failure paths and same-backend fallback tests.

Exit gate:

- OpenCL Neural failure always reaches OpenCL Legacy and never CUDA;
- CUDA Neural failure remains CUDA Legacy only;
- successful paths do not execute Legacy;
- failure at every defined stage produces a valid Legacy result.

### Phase 7 — Tune and enforce full-image performance

Work:

- run offline tuning on the local RTX 3080 Laptop GPU;
- solidify selected parameters in program build options;
- profile bandwidth-heavy structural kernels and reduce unnecessary passes;
- run the Release full RAW harness.

Exit gate:

- Nikon Bayer mean of three hot runs is `<500 ms`;
- Fujifilm X-Trans mean of three hot runs is `<500 ms`;
- measured runs perform no allocation, compilation, kernel creation, or weight upload;
- cold and hot costs are reported separately;
- correctness tests remain within `1e-4` after tuning.

### Phase 8 — Retention and cleanup audit

Work:

- remove temporary tuning-only variants not selected for production;
- verify all imported CLBlast-derived code is used;
- verify source manifests and CMake targets own the correct files;
- update backend/user-facing diagnostics and developer documentation;
- run debug correctness suites and Release performance harness.

Exit gate:

- no unused generalized runtime, im2col buffer, alternative precision path, or tuning database remains;
- program registration occurs only through the central backend registry;
- the implementation and tests use the locked terminology and explicit behavior-oriented names.

## 14. Build and verification targets

Add narrow targets so development does not require rebuilding unrelated UI components. Suggested target roles:

```text
OpenClNnOpsTest
OpenClDemosaicNetTest
OpenClRawNeuralTest
OpenClDemosaicNetKernelHarness
OpenClDemosaicNetPerfHarness
```

On Windows, configure and build only through `scripts/msvc_env.cmd` and repository presets. Representative commands after targets exist:

```bat
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target OpenClNnOpsTest OpenClDemosaicNetTest OpenClRawNeuralTest --parallel 4

cmd /c scripts\msvc_env.cmd --preset win_release -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_release --target OpenClDemosaicNetKernelHarness OpenClDemosaicNetPerfHarness --parallel 4
```

## 15. Performance optimization order

When the full-image mean misses the gate, optimize in this order using event data:

1. eliminate accidental synchronization and allocation in the hot path;
2. verify weights, kernels, and programs are reused;
3. tune direct 3x3 work-group and register blocking for the two production trunks;
4. reduce structural/post memory passes through safe fusion;
5. specialize first, last, and C6-padded layers where generic dispatch wastes lanes;
6. reduce tile pack/assembly traffic without changing shared tile coverage;
7. inspect queue gaps and host-side tile-loop overhead.

Do not use FP16, mixed precision, looser tolerance, changed tile coverage, or parallel full-image decoding to reach the gate.

## 16. Risks and controls

| Risk | Control |
|---|---|
| CLBlast extraction silently changes convolution semantics | Preserve provenance, keep the extracted source small, and compare every production shape to CPU/reference results. |
| NVIDIA-only tuning harms portability | Keep correctness generic, bound work-group sizes using device limits, and retain conservative build constants for unsupported capability classes if required. |
| Fused operations exceed numeric tolerance | Test fused and unfused operation order; retain a separate pass when fusion cannot meet `1e-4`. |
| Mutable reused kernels race | Enforce one active Neural decode and one in-order queue; document the execution rules beside kernel ownership. |
| Neural failure corrupts Legacy input | Preserve the linear CFA buffer and commit Neural output only after successful final synchronization. |
| Startup regresses due to optional program compilation | Warm only `required_at_startup` programs and keep all Neural manifests lazy. |
| Weight layout conversion dominates repeated calls | Pack/upload once during lazy cache creation and assert reuse in tests/harness metadata. |
| Tile seams differ from CUDA | Share the planner and phase-aware reflect mapping rather than reimplementing OpenCL-specific coverage. |

## 17. Completion criteria

The migration is complete only when all of the following are true:

- Bayer and X-Trans OpenCL modules load the same student models as CUDA;
- all four exported reference pairs pass at absolute tolerance `1e-4`;
- real Bayer and X-Trans RAW integration tests pass with correct dimensions and finite RGB output;
- tile planning and coverage are shared with CUDA and proven equivalent;
- Neural programs, kernels, packed weights, and workspace are reused after warm-up;
- the product performs no per-tile host synchronization;
- OpenCL Neural failures fall back only to OpenCL Legacy;
- CUDA Neural failures fall back only to CUDA Legacy;
- the minimal CLBlast-derived source is licensed, traceable, and limited to code actually compiled;
- the Nikon D800E and Fujifilm X-T5 Release means are each below `500 ms` across three hot runs on the local GPU;
- correctness remains within tolerance using pure FP32;
- no general inference runtime, product runtime tuner, full-image Neural parallelism, or unused CLBlast subsystem is introduced.

## 18. Upstream implementation references

- CLBlast direct convolution host selection and kernel structure: <https://github.com/CNugteren/CLBlast/blob/1.7.0/src/routines/levelx/xconvgemm.cpp>
- CLBlast convolution kernel fragments: <https://github.com/CNugteren/CLBlast/tree/1.7.0/src/kernels/levelx>
- MNN OpenCL buffer convolution variants and channel blocking: <https://github.com/alibaba/MNN/blob/master/source/backend/opencl/execution/buffer/ConvBufExecution.cpp>
- TNN OpenCL convolution channel/output blocking and fused activation: <https://github.com/Tencent/TNN/blob/master/source/tnn/device/opencl/acc/convolution/opencl_conv_layer_common_acc.cc>

These references guide kernel structure and tuning boundaries. They do not justify importing the corresponding inference runtimes.
