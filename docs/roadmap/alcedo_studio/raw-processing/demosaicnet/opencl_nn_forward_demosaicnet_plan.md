# OpenCL NN Forward DemosaicNet Migration and Alignment Plan

**Status:** Correctness, product routing, Phase 7.0 telemetry, Phase 7.1 residency, Phase 7.2, Phase 7.3, Phase 7.4, and Phase 8 retention/cleanup are complete; both local OpenCL full-frame means pass the `<500 ms` acceptance gate
**Date:** 2026-07-14  
**Updated:** 2026-07-15 after the Phase 8 retention and cleanup audit
**Primary roadmap owner:** `alcedo_studio/src/decoders/processor/nn`  
**Scope:** Port the CUDA DemosaicNet forward path to OpenCL without introducing a general-purpose inference runtime.

## 1. Objective

Implement Bayer and X-Trans DemosaicNet inference on the user-selected OpenCL backend while preserving the CUDA behavior, tile coverage, preprocessing, postprocessing, and output behavior.

The implementation must:

- run the same trained Bayer and X-Trans student models used by CUDA;
- use pure FP32 weights, activations, and accumulation;
- match exported reference tensors with absolute tolerance `1e-4`;
- process the two CUDA performance-harness RAW fixtures in less than `500 ms` each on the local GPU, measured as the arithmetic mean of three hot runs;
- keep OpenCL Neural failures inside the OpenCL backend by falling back to OpenCL Legacy;
- do not link the complete CLBlast library; retain minimal derived kernel material only if a faithful
  CLBlast control wins the Phase 7.2 A/B;
- remain a hard-coded DemosaicNet implementation rather than becoming a graph executor.

The target local device is an NVIDIA GeForce RTX 3080 Laptop GPU with 8 GB VRAM. CUDA remains the behavioral and performance comparison on this machine, but the OpenCL implementation must stay portable across conforming OpenCL devices supported by the project.

### 1.1 Performance recovery decision

The first full-frame results change the emphasis of Phase 7. This is not evidence that OpenCL is
intrinsically an order of magnitude slower than CUDA on this GPU. The current path contains two
independent implementation failures:

1. **The execution state is not resident.** Two OpenCL sub-buffers are created and released for
   every tile, and the full-frame staging buffers, workspace, tiled executor, tile buffers, and
   several kernel objects are recreated for every decode. Per-tile sub-buffer release alone accounts
   for 1.70 seconds of Bayer time and 2.76 seconds of X-Trans time in the diagnostic profile.
2. **The old 3x3 kernel was a scalar baseline.** It retained CLBlast-style parameter names, but
   those parameters did not drive work-group tiling, local-memory staging, register blocking, or
   vector width. Phase 7.2 replaces it with a project-owned fixed-shape kernel.

The major required improvement is consequently explicit: **make the complete Neural execution
resident, then replace the nominal CLBlast-derived scalar convolution with a real tiled,
register-blocked direct convolution specialized for the fixed C24 and C32 trunks.** Allocation
cleanup is mandatory but cannot by itself reach the `<500 ms` gate. Subtracting the measured
sub-buffer-release cost from wall time still leaves approximately 2.78 seconds for Bayer and 4.60
seconds for X-Trans.

The full CLBlast library is not linked into this path, and the product source no longer retains a
CLBlast-derived extract. The selected product candidate is a project-owned persistent-NHWC,
fixed-shape direct-convolution implementation shaped after the CUDA path.

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
| Convolution | Real tiled/register-blocked fixed-shape direct convolution for 3x3 layers; compare a project-owned CUDA-shaped OpenCL kernel with a faithful minimal CLBlast control. Specialized project kernel for C4 1x1 layers. |
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

### 3.2 Existing OpenCL infrastructure and landed migration

The project already has the correct foundation:

- `OpenClContext` selects the device and owns a single in-order queue;
- `OpenClImage` is a buffer-backed HWC allocation, not an OpenCL image object;
- `OpenClProgramLibrary` owns compiled-program caching;
- `OpenClBackendProgramRegistry` centrally activates module manifests;
- the OpenCL RAW path already implements Bayer and X-Trans Legacy processing.

The reusable OpenCL NN buffer layer, fixed Bayer/X-Trans modules, shared model/tile utilities,
product routing, correctness tests, and full-frame timing harness now exist. The remaining blocking
gap is performance: the current correct implementation violates its intended resident-lifetime and
tiled-convolution architecture.

### 3.3 Program lifecycle correction landed

Neural programs are registered centrally with `required_at_startup = false`, so they remain lazy
because they are large, optional, and used only when the user selects OpenCL Neural.

Runtime warm-up compiles only programs marked `required_at_startup`. Keep this behavior unchanged;
the Phase 7 problem is buffer/kernel execution-object lifetime after the program and model are ready,
not repeated OpenCL program compilation.

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

### 5.4 Project-owned convolution source

The product convolution source is
`alcedo_studio/src/decoders/processor/operators/gpu/opencl_shader/demosaicnet_conv.cl`.
It is a fixed 3x3 stride-1 NHWC4 implementation and does not import the CLBlast C++ API, runtime
database, host tuner, indirect im2col path, or unrelated BLAS levels.

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

The shipping path must be a real tiled FP32 direct convolution specialized for the project's fixed
NHWC4 layout. The first implementation attempted to derive this from CLBlast `Xconvgemm`, but the
result retained names rather than the upstream work mapping. Phase 7.2 therefore compares a
project-owned CUDA-shaped OpenCL kernel with a faithful minimal CLBlast control and retains the
faster correct implementation.

Required behavior:

- direct spatial convolution with no im2col allocation;
- vectorized `float4` channel loads and blocked weight loads;
- compile-time kernel geometry and channel blocking for the production shapes;
- an explicit local work-group, cooperative local-memory reuse, and multi-output register blocking;
- bias addition in the accumulator epilogue;
- optional ReLU in the same epilogue;
- no implicit precision conversion;
- bounds-safe writes for edge work-groups.

If the CLBlast control is retained, the offline parameter search must cover the relevant parameters
and every one must be live in the compiled work mapping:

```text
KWID, MDIMAD, MDIMCD, NDIMBD, NDIMCD,
PADA, PADB, VWMD, VWND, WGD
```

For the project-owned kernel, tune actual spatial tile, output block, vector width, and local-memory
choices instead of preserving CLBlast parameter names. Only configurations that pass the
CPU/reference correctness filter are eligible for timing. Tune Bayer and X-Trans shapes separately
when their best settings differ.

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

Reserve resident grow-only storage sized for the largest supported tile and selected variant:

- packed boundary input;
- two ping-pong trunk activation slots;
- structural/post temporary storage that cannot be fused;
- tile output staging when direct assembly is not legal.

The fixed product network should own the two ping-pong activation slots as dedicated buffers, not
as per-forward sub-buffer views into a generic slab. A generic `WorkspacePool` may remain for tests
or unrelated NN primitives, but its sub-buffer API is not part of the DemosaicNet hot path.

Expose allocation-generation and memory-object lifecycle counters for tests and performance
diagnostics. After warm-up, three measured runs must leave every scratch generation unchanged and
must create zero sub-buffers.

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

### 12.3 Pre-Phase 7 stage profile — 2026-07-14

Profiled before beginning tuning on the target local GPU:

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 3080 Laptop GPU, 8 GB |
| Driver | NVIDIA 610.62 |
| Build | `win_release`, `CMAKE_BUILD_TYPE=Release`, MSVC `/O2` |
| Fixtures | Nikon D800E Bayer and Fujifilm X-T5 X-Trans |
| Hot timing | one warm-up followed by three complete OpenCL Neural decodes; wall-clock `RawProcessor::Process` |
| Stage timing | three additional hot decodes; diagnostic queue drains at named boundaries only, so their stage sums include host enqueue and required device completion but are not product-path timings |

The timing harness currently prints `Debug (NDEBUG not set)` even in the Release configuration because
the C++ compile flags do not define `NDEBUG`; the configured build type and `/O2` flags above are the
authoritative build facts.

| Fixture | Hot samples (ms) | Hot mean (ms) | Gate | Result |
|---|---:|---:|---:|---|
| Nikon D800E Bayer | 4594.02, 4454.09, 4385.27 | 4477.79 | <500 | fail (8.96× gate) |
| Fujifilm X-T5 X-Trans | 7361.26, 7543.82, 7181.94 | 7362.34 | <500 | fail (14.72× gate) |

Stage means below are full-frame sums across the same three diagnostic runs. Call counts expose the
fixed 1024-owned-tile work: 40 Bayer tiles and 48 X-Trans tiles.

| Stage | Bayer ms (calls/run) | X-Trans ms (calls/run) | Finding |
|---|---:|---:|---|
| Phase crop + HWC pack | 30.35 (1) | 29.31 (1) | Small full-frame boundary cost. |
| Reflect-101 tile pack | 8.88 (40) | 13.13 (48) | Not a primary limiter. |
| Workspace sub-buffer setup | 0.34 (40) | 2.29 (48) | Negligible. |
| NCHW-to-NHWC4 pack | 772.31 (40) | 1262.46 (48) | 18.0% / 17.8% of traced time; unexpectedly large for a pack and likely includes sub-buffer first-use/driver object effects. |
| Trunk 3×3 convolutions, including the first unequal-channel layer | 1609.95 (40) | 2492.42 (48) | 37.5% / 35.1%; largest pure convolution region. |
| Residual 1×1 + unpack/concat | 22.91 (40) | 42.79 (48) | Minor. |
| Post 3×3, output 1×1, RGB export | 104.46 (40) | 423.45 (48) | Secondary on X-Trans; retain as a later tuning target. |
| Workspace sub-buffer release | 1697.07 (40) | 2758.91 (48) | 39.5% / 38.9%; primary blocker. Releasing per-tile OpenCL sub-buffers forces costly driver-side work. |
| Tile assembly | 24.24 (40) | 35.33 (48) | Minor. |
| RGB-to-RGBA boundary | 27.19 (1) | 32.60 (1) | Small full-frame boundary cost. |
| **Traced total** | **4297.70** | **7092.69** | Matches the wall-clock Neural stage within normal run variance. |

Phase 7 must first keep the two workspace slots resident for the whole Neural decode (use tensor views
or fixed offsets rather than creating/releasing `cl_mem` sub-buffers per tile). Next remeasure the
pack and replace/tune the trunk 3×3 paths. Structural fusion is not the first leverage point: all
structural stages other than the X-Trans post/output path are small relative to those three costs.

### 12.4 Root-cause audit and hypothesis verdicts

| Hypothesis | Verdict | Repository evidence | Required action |
|---|---|---|---|
| Repeated device allocation is dominant | **Confirmed** | `ForwardImpl` creates and releases two sub-buffers for every tile; `DemosaicWithNeuralEngine` constructs the workspace and tiled executor per decode; the executor then allocates its tile input/output buffers. Sub-buffer release is 39.5% / 38.9% of traced time. | Remove sub-buffers from the fixed network and move all reusable scratch, tables, and kernels into a context-keyed resident execution object. |
| Hidden activation host/device exchange | **Not the primary problem** | The hot forward contains no activation `clEnqueueReadBuffer` or `clEnqueueWriteBuffer`. Model weight upload is a cold cache operation. The only recurring host-originated transfer found in the product path is the 2x2/6x6 CFA lookup table created with `CL_MEM_COPY_HOST_PTR`; it is tiny but should still become resident. | Add byte counters to prove hot H2D/D2H is zero, then keep the CFA table resident. Do not spend the first optimization cycle on PCIe transfer work. |
| Explicit waits occur per tile | **Not in the unprofiled product loop** | The tile loop intentionally enqueues on one in-order queue and performs one final `clFinish`. The diagnostic stage profiler adds `clFinish` at boundaries and must not be used as the final device-time authority. | Replace boundary-drain timing with event profiling on a profiling-enabled queue; keep the existing wall-clock run as the product truth. |
| Object destruction causes implicit driver work | **Confirmed** | Two `clReleaseMemObject` calls occur at every forward/tile boundary. Khronos specifies that deletion happens only after the reference count reaches zero and queued users finish; repeated last-reference release puts object retirement directly in the hot loop. The measured release region is 1.70 s / 2.76 s. | Hold stable scratch allocations across every tile and across hot decodes. Timed runs must execute zero `clCreateSubBuffer` calls and zero scratch-buffer last releases. |
| CLBlast itself is ten times slower than CUDA | **Rejected as an attribution** | No CLBlast runtime is linked. The local source does not contain the upstream local-memory tiles, `MWID x NWID` register accumulators, barriers, or CLBlast global/local launch formula. Most advertised tuning macros are dead. | Treat the current kernel as a project-owned scalar baseline. A/B a faithful upstream algorithm against a CUDA-shaped custom OpenCL kernel before deciding what, if anything, to retain from CLBlast. |
| OpenCL launch/runtime overhead explains the whole gap | **Rejected** | Later OpenCL RAW pipeline stages are already comparable with CUDA, and the traced time is concentrated in sub-buffer retirement and fixed convolution/pack regions rather than evenly across every enqueue. | Optimize the identified regions before considering command-buffer replay or broader runtime changes. |

### 12.5 Why the former "CLBlast tuning" could not work

The upstream CLBlast single-kernel convolution maps output patches and output channels into `WGD`
tiles, fixes the local work-group to `{MDIMCD, NDIMCD, 1}`, stages A/B tiles through `__local`
memory, and accumulates an `MWID x NWID` register tile per work item. See the upstream
[`xconvgemm.cpp`](https://github.com/CNugteren/CLBlast/blob/1.7.0/src/routines/levelx/xconvgemm.cpp)
launch formula and
[`xconvgemm_part2.opencl`](https://github.com/CNugteren/CLBlast/blob/1.7.0/src/kernels/levelx/xconvgemm_part2.opencl)
kernel body.

The former local `demosaicnet_conv3x3_nhwc4` instead:

- dispatches `global = {out_w, out_h, batch * out_channel_blocks}` with no explicit local size;
- assigns one work item to one output pixel and one four-channel output block;
- reloads the 3x3 input neighborhood for every output block and neighboring pixel;
- performs no cooperative spatial-halo load and no cooperative weight-tile load;
- accumulates only four scalar outputs;
- keeps dynamic boundary and logical-lane branches inside the innermost production loops;
- defines CLBlast-style macros without using them to change the work mapping or memory hierarchy.

This was a legal correctness kernel, but it was not a tiled convolution and its provenance was
misleading. Phase 7.2 removed the dead CLBlast-shaped source and replaced it with the project-owned
kernel described below.

CLBlast's own tuning documentation remains historical A/B reference material, not product
provenance.

### 12.6 Performance budget after the first profile

Use the retained CUDA fixed-1024 persistent-NHWC path as the same-machine reference: approximately
375.5 ms p50 for both fixtures in the most directly comparable retained measurements. The OpenCL
means are currently about 11.9x Bayer and 19.6x X-Trans slower; the aggregation differs (three-run
mean versus CUDA p50), so these ratios are directional rather than an acceptance metric.

| Recovery step | Bayer budget | X-Trans budget | Interpretation |
|---|---:|---:|---|
| Current wall mean | 4477.79 ms | 7362.34 ms | Observed failure. |
| Wall minus measured sub-buffer release | ~2780.72 ms | ~4603.43 ms | Allocation/lifetime repair alone is still far from the gate. |
| First target after resident execution | <=3000 ms and >=30% faster | <=5000 ms and >=30% faster | Confirms object-lifecycle removal had the expected order of effect. |
| Target after convolution replacement | <=750 ms | <=750 ms | Do not move to minor fusion/launch work while above this threshold. |
| Shipping gate | <500 ms mean | <500 ms mean | Existing product requirement; correctness remains `1e-4` FP32. |

The intermediate budgets are diagnostic gates, not permission to stop. Missing a budget sends work
back to the preceding root cause; it does not weaken the final `<500 ms` requirement.

### 12.7 Phase 7.0 trustworthy telemetry — 2026-07-14

Landed instrumentation and harness:

| Piece | Location |
|---|---|
| API lifecycle counters | `opencl/opencl_api_counters.hpp` (create/sub/kernel/release, H2D/D2H bytes, builds, waits) |
| Profiling queue override | `OpenClContext::InstallProfilingQueueOverride()` (`CL_QUEUE_PROFILING_ENABLE`, same context) |
| Event stage profiler | `opencl/nn/demosaicnet_stage_profiler.hpp` mode `EventTimestamps` (no mid-network `clFinish`) |
| Full-frame harness | `OpenClDemosaicNetFullFrameTiming` with `--compare --event-profile --cuda-control --json` |
| Artifact | `build/perf/opencl_nn_phase7_0.json` |

Same-session Release-configured run on RTX 3080 Laptop (driver 610.62); C++ still reports
`NDEBUG` unset while `/O2` is active (pre-existing build-flag note). One warm-up, three hot means.
CUDA Neural control ran first per fixture, then OpenCL wall, then OpenCL event profile.

| Fixture | Unprofiled wall mean | Event-profile wall mean | Residual (event−wall) | CUDA control mean |
|---|---:|---:|---:|---:|
| Nikon D800E Bayer | 4051.16 ms | 4273.71 ms | +222.56 ms (5.5%) | 286.54 ms |
| Fujifilm X-T5 X-Trans | 6769.98 ms | 7206.52 ms | +436.54 ms (6.5%) | 603.12 ms |

Event-mode device exclusive sums (sum of `END−START`; no boundary `clFinish`):

| Fixture | device_exec_sum | host stage wall sum | residual wall−device | events/run |
|---|---:|---:|---:|---:|
| Bayer | 972.10 ms | 4104.93 ms | 3301.62 ms | 643 |
| X-Trans | 1884.74 ms | 7015.78 ms | 5321.78 ms | 579 |

Hot-run mean API counters (full `RawProcessor::Process`, not Neural-only):

| Counter | Bayer hot mean | X-Trans hot mean | Interpretation |
|---|---:|---:|---|
| `create_sub_buffer` | 80 | 96 | 2 per tile; confirms sub-buffer churn. |
| `create_buffer` | 11 | 11 | Per-decode scratch/images, not steady-state zero. |
| `create_kernel` | 5 | 5 | Structural helpers recreated on the product path. |
| `release_mem_object` | 91 | 107 | Last-reference release in the hot loop. |
| `h2d_bytes` | ~73.1e6 | ~81.8e6 | Full-frame CFA upload into OpenCL (~RAW ushort plane); **not** Neural activation exchange. |
| `d2h_bytes` | 0 | 0 | **Proven zero** on hot runs. |
| `program_builds` | 0 | 0 | Hot path does not recompile. |
| `final_waits` | 1 | 1 | One Neural-stage `WaitQueue` (plus `OpenClImage` crop finish counted under `queue_finish`). |

Largest host-only stage under event mode (Bayer): `workspace_subbuffer_release` ≈ 2556 ms host /
40 calls, zero device events — pure last-reference retirement. Largest device stage: `trunk_3x3`
≈ 821 ms Bayer / 1446 ms X-Trans. `pack_nchw_to_nhwc4` remains host-heavy (≈750 / 1219 ms) while
device pack is only a few milliseconds, consistent with queue back-pressure and sub-buffer first-use
rather than pure arithmetic.

Exit gate status:

- event sums collected after the product final wait without diagnostic boundary `clFinish` — **pass**;
- unprofiled vs event-profiled wall residual ≈ 5–6% with residual explicitly reported — **pass**;
- hot D2H proven zero; hot H2D is full-frame CFA upload (counter-proven), not inferred Neural traffic — **pass**;
- `OpenClNnPrimitivesTest` (9) and `OpenClDemosaicNetTest` (13) still pass at `1e-4` — **pass**.

Phase 7.1 must drive hot `create_sub_buffer` to zero and remove the measured release/host stalls before
convolution replacement.

### 12.8 Phase 7.1 resident execution — 2026-07-14

Landed residency changes:

| Piece | Location |
|---|---|
| Two grow-only activation slots (no SubBuffer) | `opencl/nn/activation_slots.hpp` + module `ForwardNchwToHwc` |
| Context-keyed product execution state | `opencl_demosaicnet.cpp` `ResidentExecutionState` (kernels, CFA table, HWC staging, tiled executor, slots) |
| Fused phase crop + HWC pack | `demosaicnet_pack_cfa_mono_to_hwc3` ROI args; no `aligned_mono` temporary |
| Reusable Neural RGBA staging | static `neural_rgba_staging` in `raw_processor_opencl.cpp` |
| Final-output counter | `create_buffer_final_output` in `OpenClApiCounters` |
| Artifact (cool wall) | `build/perf/opencl_nn_phase7_1_wall.json` |
| Artifact (first compare session) | `build/perf/opencl_nn_phase7_1.json` (overwritten by later thermal runs; use wall JSON for gate) |

Cool wall-only Release-configured run on RTX 3080 Laptop (driver 610.62), temp start ≈82°C, one warm-up, three hot means. Baseline is Phase 7.0 unprofiled wall (§12.7).

| Fixture | Phase 7.0 wall mean | Phase 7.1 wall mean | Δ | Gate ≥30% |
|---|---:|---:|---:|---|
| Nikon D800E Bayer | 4051.16 ms | **1204.05 ms** | **−70.3%** | **pass** |
| Fujifilm X-T5 X-Trans | 6769.98 ms | **2001.15 ms** | **−70.4%** | **pass** |

CUDA control (same machine, earlier cool compare session): Bayer ≈347 ms, X-Trans ≈496 ms. OpenCL remains dominated by device `trunk_3x3` scalar convolution — the Phase 7.2 target.

Hot-run mean API counters (full `RawProcessor::Process`):

| Counter | Bayer hot mean | X-Trans hot mean | Interpretation |
|---|---:|---:|---|
| `create_sub_buffer` | **0** | **0** | Sub-buffer churn eliminated. |
| `create_kernel` | **0** | **0** | Structural/module kernels resident. |
| `program_builds` | **0** | **0** | No hot recompile. |
| `d2h_bytes` | **0** | **0** | Proven zero. |
| `final_waits` | **1** | **1** | One Neural-stage wait. |
| `create_buffer` | 3 | 3 | Residual process sandwich (CFA mono↔RGBA process buffer + decode crop), not Neural ping-pong. |
| `create_buffer_final_output` | 0 | 0 | Aligned Neural RGBA staging reuses after warm-up. |
| `h2d_bytes` | ~73.1e6 | ~81.8e6 | Full-frame CFA upload into OpenCL; not Neural activations. |
| `release_mem_object` | 3 | 3 | Paired with residual process-buffer recreates. |

Event-mode note (first compare session, cooler GPU): `workspace_subbuffer_setup` / `workspace_subbuffer_release` stages are **gone**. Largest device stage is still `trunk_3x3` (Bayer ≈1.1 s device sum under event queue). Long stacked event/CUDA sessions overheat this laptop GPU and inflate later means; treat cool wall JSON as the residency gate.

Exit gate status:

- timed execution performs exactly zero `clCreateSubBuffer` — **pass**;
- no hot kernel creation, weight upload, program build, or D2H after capacity warm-up — **pass**;
- hot Neural activation scratch is grow-only resident (slots + staging); residual `create_buffer=3` is process mono/RGBA/crop ownership, not tile SubBuffers — **pass with residual noted for boundary polish**;
- one final Neural wait — **pass**;
- both fixtures improve ≥30% from Phase 7.0 baseline — **pass** (≈70% each on cool wall);
- correctness: `OpenClNnPrimitivesTest` (9) and `OpenClDemosaicNetTest` (13) pass at `1e-4` — **pass**.

Phase 7.2 must replace the scalar 3×3 kernel; residency is no longer the primary wall-time blocker.

### 12.9 Phase 7.2 project-owned tiled convolution — 2026-07-14

Implemented in `demosaicnet_conv.cl` and `opencl/nn/convolution.cpp`:

| Piece | Implementation |
|---|---|
| Spatial mapping | Explicit `16×8×1` local work-group; rounded spatial globals with bounds-safe writes. |
| Local reuse | Cooperative 3×3 input-halo staging and packed-weight staging in `__local` memory. |
| Register blocking | Each work-item computes every output C4 block from one spatial position, matching the CUDA NHWC kernel. |
| Shape coverage | Bayer and X-Trans first/trunk/post 3×3 channel families; runtime logical-channel masking remains active. |
| Epilogue | FP32 accumulation, bias, ReLU, and zeroing of padded output lanes. |
| Provenance | Removed the unused CLBlast-shaped extract and its misleading tuning surface. |

Validation on the local RTX 3080 Laptop GPU:

- `OpenClNnPrimitivesTest`: 9/9 passed, including production shapes, padding, edge bounds, and
  asynchronous multi-layer dispatch;
- `OpenClDemosaicNetTest`: 13/13 passed, including all four exported Bayer/X-Trans references at
  absolute tolerance `1e-4` and tiled coverage;
- CUDA-shaped wall run, three hot iterations: Bayer **381.57 ms**, X-Trans **413.24 ms**;
  both pass the roadmap's `<500 ms` two-fixture acceptance gate.
- Short event profile: Bayer trunk device sum **174.18 ms**, X-Trans trunk device sum
  **163.64 ms**; full-frame event-profiled wall samples were **374.13 ms** and **403.95 ms**.
- The timing harness was configured through the `win_release` preset, but reports
  `NDEBUG` as unset; treat the wall artifact as a measured local gate result, not as a clean
  compiler-mode comparison against CUDA.
- Artifacts: `build/perf/opencl_nn_phase7_2_cuda_shaped_wall.json` and
  `build/perf/opencl_nn_phase7_2_cuda_shaped_event.json`.

No relaxed precision, changed tile coverage, or extra full-frame concurrency was used.

### 12.10 Phase 7.3 direct packing and boundary re-profile — 2026-07-14

Implemented from the Phase 7.2 event profile:

| Piece | Implementation |
|---|---|
| Direct first-input packing | Bayer and X-Trans kernels combine reflect-101, signed-gamma encoding, and their fixed 2x2 pack directly into the first persistent NHWC4 activation. |
| Residual skip input | A direct HWC3 reflect/gamma residual-concat kernel removes the second consumer of the former NCHW tile. |
| C6 post layer | Fixed two-block C6 input kernel preserves FP32 accumulation, bias, ReLU, and output-lane masking. |
| Output tail | Fixed C3/one-block 1x1 kernel avoids accumulating unused output blocks for the final RGB tail. |
| Stage telemetry | Post 3x3, output tail, and output RGB are reported separately; tile assembly, phase crop, and RGB/RGBA conversion remain separate passes. |

The legacy NCHW entrypoint remains available for module/reference tests. Product tiling uses only the
direct HWC3 path and no longer owns a tile NCHW buffer or its per-tile pack dispatch.

Validation on the local NVIDIA GeForce RTX 3080 Laptop GPU (driver 610.62; the `win_release`
preset still reports `NDEBUG` unset):

- `OpenClNnPrimitivesTest`: 9/9 passed;
- `OpenClDemosaicNetTest`: 15/15 passed, including both direct HWC3-vs-NCHW reflected-input
  comparisons and all four exported references at absolute tolerance `1e-4`;
- hot product counters remain at zero `create_sub_buffer`, zero `create_kernel`, zero program
  builds, zero D2H bytes, and one final wait;
- direct packing removes 40 Bayer / 48 X-Trans pack-and-layout enqueues per full-frame decode;
- event-profiled hot means: Bayer **407.44 ms**, X-Trans **428.24 ms**;
- final wall artifact means: Bayer **390.01 ms**, X-Trans **419.55 ms**;
- artifacts: `build/perf/opencl_nn_phase7_3_wall.json` and
  `build/perf/opencl_nn_phase7_3_event.json`.

The event profile records the material retained layers independently:

| Fixture | Reflect pack | Residual concat | Post 3x3 | Output tail | Output RGB | Assembly |
|---|---:|---:|---:|---:|---:|---:|
| Bayer | 2.59 ms | 14.43 ms | 34.90 ms | 13.92 ms | 3.33 ms | 2.47 ms |
| X-Trans | 4.62 ms | 20.33 ms | 59.16 ms | 24.94 ms | 3.69 ms | 2.98 ms |

No additional fusion was retained for phase crop, tile assembly, output RGB extraction, or
RGB/RGBA conversion: each remains below the material boundary threshold, and the direct pack and
residual comparisons meet the existing `1e-4` FP32 contract.

### 12.11 Phase 7.4 submission-overhead reduction — 2026-07-15

Implemented only the measured submission optimization:

| Piece | Implementation |
|---|---|
| Per-layer kernel ownership | Each trunk layer, residual 1x1, post 3x3, and output 1x1 owns a dedicated resident `cl_kernel`; mutable kernel argument state is never shared between weighted layers. |
| Immutable argument binding | Weights, bias, logical/physical channel metadata, and the 1x1 ReLU flag are bound once when the model cache entry becomes ready. Each tile only binds its input/output buffers and spatial dimensions. |
| Portable fallback | The ordinary in-order enqueue path remains the product path. The selected NVIDIA device reports no `cl_khr_command_buffer`, so command-buffer replay is not retained or required for correctness. Device extensions are now recorded by the performance harness. |

The Phase 7.3 event profile showed host enqueue wall of **123.35 ms Bayer / 112.02 ms X-Trans**
against device execution of **279.00 ms / 302.33 ms**. That made submission work material, while
the actual queued-to-submit delay was only **1.24 ms / 1.13 ms**; the dominant removable part was
the repeated host-side argument binding and mutable-kernel setup.

Validation on the local NVIDIA GeForce RTX 3080 Laptop GPU (driver 610.62):

- `OpenClNnPrimitivesTest` and `OpenClDemosaicNetTest`: **24/24 passed**;
- all four exported student references and direct HWC3-vs-NCHW comparisons remain within absolute
  tolerance `1e-4`;
- hot product counters remain at zero `create_sub_buffer`, zero `create_kernel`, zero program
  builds, zero D2H bytes, and one final wait;
- cold kernel creation is expected to rise to 20 Bayer / 12 X-Trans because each weighted layer
  now owns its own immutable-argument kernel object; this cost is outside the hot mean;
- event-profiled host enqueue mean becomes **103.97 ms Bayer / 104.19 ms X-Trans**, a reduction of
  approximately **15.7% / 7.0%** from Phase 7.3;
- unprofiled wall means are **345.94 ms Bayer / 367.29 ms X-Trans**, compared with Phase 7.3's
  **390.01 ms / 419.55 ms**. This is an improvement of approximately **11.3% / 12.5%**; the
  comparison is reported with GPU telemetry because clock and temperature vary between sessions;
  the event host-enqueue reduction is the direct optimization signal;
- artifacts: `build/perf/opencl_nn_phase7_4_wall.json` and
  `build/perf/opencl_nn_phase7_4_event.json`.

Exit gate status:

- the retained launch optimization improves the measured host-submit component by more than 3%
  and the full-frame wall means remain below 500 ms — **pass**;
- command-buffer replay is not retained because `cl_khr_command_buffer` is absent on the selected
  device — **pass with portable fallback**;
- ordinary and fallback behavior remain unchanged because no optional extension is required —
  **pass**.

### 12.12 Phase 8 retention and cleanup audit — 2026-07-15

The retained product path is now documented and its development-only surface has been audited:

- `demosaicnet_conv.cl` is the project-owned fixed 16x8 direct-convolution implementation. No
  CLBlast extract, CLBlast tuning constants, im2col path, alternative precision path, runtime
  tuner, or tuning database remains in the OpenCL DemosaicNet source or build targets;
- `WorkspacePool` and `SubBuffer` remain only as reusable primitive APIs with direct tests. The
  product DemosaicNet path uses dedicated resident `ActivationSlots` and creates no sub-buffer per
  tile;
- all DemosaicNet programs are described by the `raw_demosaicnet` manifest and activated through
  `OpenClBackendProgramRegistry`; the OpenCL program library remains RAW-agnostic;
- build diagnostics now include the stable program name, selected device, driver, and build
  options; RAW Neural fallback diagnostics identify the failure stage, variant, and OpenCL Legacy
  fallback backend explicitly;
- the retained architecture, ownership boundaries, cleanup rules, and verification commands are
  recorded in this roadmap and in the source-level ownership comments beside the OpenCL targets.

Verification on the local NVIDIA GeForce RTX 3080 Laptop GPU (driver 610.62) passed:

- the Windows debug build and targeted OpenCL suite: **40/40 tests passed**, including the new
  program-build diagnostic contract;
- the Windows `win_release` build and three-hot-run full-frame harness: Bayer wall mean **383.61 ms**
  and X-Trans wall mean **413.94 ms**, both below the `<500 ms` gate;
- the same hot runs recorded zero sub-buffer creation, zero Neural kernel creation, zero program
  builds, zero D2H bytes, and one final wait for both fixtures;
- the Release harness reports the retained ordinary in-order path because
  `cl_khr_command_buffer` is unavailable. The JSON report is a development artifact under
  `build/perf/` and is not part of the shipped product.

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

### Phase 7 — Recover performance and enforce the full-image gate

Phase 7 is a measured recovery sequence. Complete each subphase and record its JSON artifact before
starting the next; do not hide a failed root cause under several simultaneous changes.

#### Phase 7.0 — Establish trustworthy telemetry — **complete (2026-07-14)**

Work (landed):

- development-only profiling-enabled in-order queue via `OpenClContext::InstallProfilingQueueOverride()`;
- `DemosaicNetProfileMode::EventTimestamps` attaches events without mid-network waits; timestamps
  collected after the product final wait;
- harness reports device execution, queue delay, host enqueue wall, and final wall separately;
- `OpenClApiCounters` count buffer/sub-buffer/kernel create, last-reference releases, H2D/D2H bytes,
  program builds, and final waits for cold/hot runs;
- device/driver/build options and GPU clock/temperature (nvidia-smi) recorded;
- CUDA and OpenCL controls alternate in the same session (`--cuda-control`).

Exit gate (met — see §12.7 and `build/perf/opencl_nn_phase7_0.json`):

- event sums do not require diagnostic boundary `clFinish` calls;
- unprofiled wall and event-profiled wall residual ≈ 5–6% with residual reported;
- hot D2H proven zero; hot H2D quantified as full-frame CFA upload, not Neural activations;
- correctness tests unchanged within `1e-4`.

#### Phase 7.1 — Make execution resident and delete sub-buffer churn — **complete (2026-07-14)**

Work (landed):

- introduce one context-keyed, serialized DemosaicNet execution state alongside the resident model;
- give that state two dedicated grow-only activation buffers rather than creating two `cl_mem`
  sub-buffer objects per tile; fixed Bayer/X-Trans networks do not need a generic suballocator here;
- retain the tiled executor, tile input/output buffers, structural kernels, CFA lookup buffers, and
  reusable full-frame staging capacity across hot decodes;
- fuse phase crop directly into the HWC pack so the full-frame `aligned_mono` temporary is not
  materialized;
- distinguish unavoidable caller-owned final output allocation from removable Neural scratch
  allocation in the counters;
- retain the existing one-active-decode mutex so mutable kernel arguments and scratch reuse remain
  race-free.

Exit gate (see §12.8 for numbers):

- timed execution performs exactly zero `clCreateSubBuffer` calls — **pass**;
- timed execution performs no Neural activation SubBuffer create/release, kernel creation, weight
  upload, program build, or D2H after capacity warm-up — **pass**;
- residual hot `create_buffer=3` is process-buffer mono↔RGBA/crop ownership, not tile scratch —
  **pass with residual noted**;
- one and only one final Neural wait remains — **pass**;
- both fixtures improve by at least 30% from the recorded baseline — **pass** (≈70% cool wall).

#### Phase 7.2 — Replace the scalar 3x3 kernel

Build one isolated harness that times the exact production shapes and compare these candidates:

1. **Baseline:** current one-pixel/one-`float4` scalar kernel.
2. **Preferred candidate:** project-owned fixed-shape OpenCL direct convolution modeled on the
   retained CUDA persistent-NHWC kernel: explicit local size, cooperative input-halo loading,
   register-blocked output channels/pixels, compile-time channel counts, fused bias/ReLU, and
   branch-free valid-convolution trunk loops.
3. **Control candidate:** a faithful minimal adaptation of upstream CLBlast single-kernel
   `Xconvgemm`, including its actual local-memory tiles, register tile, local-size formula, and live
   tuning parameters. This is a benchmark/control and does not authorize linking the full CLBlast
   runtime into the product.

Tune and validate these shape families independently:

- Bayer first layer and C24->C24 trunk;
- X-Trans first layer and C32->C32 trunk;
- C6->C24 / C6->C32 post 3x3;
- border-free valid convolution separately from any padded reference-test shape.

Every parameter candidate must pass the CPU/exported reference check before timing. Record local
memory, registers when the compiler exposes them, work-group geometry, achieved throughput, and
event time. Do not use dead build parameters: a tuning dimension is valid only if source review and
the compiled launch geometry show that it changes the algorithm.

Exit gate:

- the selected kernel beats the scalar baseline by at least 5x on both retained trunk shapes;
- its same-tile trunk event sum is no worse than 1.5x the retained CUDA trunk on the same GPU;
- both full-frame means reach `<=750 ms` before work moves to minor structural/launch polishing;
- all four exported model references remain within absolute error `1e-4`;
- provenance matches the selected implementation, including removal of misleading CLBlast claims
  if the project-owned kernel wins.

#### Phase 7.3 — Re-profile pack, post, and boundary passes

Work only from the new event profile:

- remeasure the NCHW/HWC packing stage after stable activation buffers remove first-use and
  sub-buffer effects;
- if it remains material, pack reflect-101 + signed gamma directly into the network's first NHWC4
  input and remove the intermediate NCHW tile;
- specialize the C6-padded post layer and X-Trans output tail if they exceed 10% of the new frame;
- retain separate passes whenever a fusion changes FP32 operation ordering beyond `1e-4`;
- avoid optimizing tile assembly, phase crop, or RGB/RGBA conversion while each remains below 5%.

Exit gate:

- no material full-frame intermediate exists solely to convert between two adjacent OpenCL kernels;
- no newly fused pass violates negative/over-range/highlight behavior;
- each retained optimization improves full-frame mean by at least 3% or removes a measured memory
  allocation without regression.

#### Phase 7.4 — Reduce submission overhead only if it is measured

Work:

- compare host enqueue wall time with event execution time after convolution is fixed;
- cache immutable kernel objects and avoid rebinding invariant weight/bias arguments where a safe
  per-layer dispatch object can own them;
- consider `cl_khr_command_buffer` replay only when the selected device reports the extension and
  queue-gap/host-submit time is at least 5% of the remaining frame;
- keep the ordinary in-order enqueue path as the portable fallback;
- do not revisit larger/ragged tiles unless new same-session evidence overturns the already rejected
  CUDA tile experiments.

Exit gate:

- no launch optimization is retained without at least 3% full-frame improvement;
- replay and ordinary paths produce the same reference output and fallback behavior;
- product correctness does not depend on an optional extension.

#### Phase 7.5 — Final acceptance

Exit gate:

- Nikon Bayer mean of three hot runs is `<500 ms`;
- Fujifilm X-Trans mean of three hot runs is `<500 ms`;
- measured runs perform no removable scratch allocation, compilation, kernel creation, weight
  upload, or host-visible activation transfer;
- cold and hot costs, individual samples, event-stage sums, allocation/transfer counters, and CUDA
  same-session comparison are recorded separately;
- correctness tests remain within `1e-4` using pure FP32.

### Phase 8 — Retention and cleanup audit

Work:

- remove temporary tuning-only variants not selected for production;
- make convolution provenance match the retained implementation: either keep only genuinely used
  CLBlast-derived code or remove the CLBlast extract and its misleading tuning surface;
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

When the full-image mean misses the gate, optimize in this order using event and lifecycle data:

1. replace boundary `clFinish` profiling with non-perturbing event telemetry;
2. eliminate all per-tile sub-buffer creation/release and make scratch/executors/kernels resident;
3. prove hot H2D/D2H bytes are zero and eliminate the recurring CFA-table upload;
4. replace the scalar 3x3 implementation with a real tiled/register-blocked fixed-shape kernel;
5. re-profile before deciding whether pack, post, or boundary fusion is still material;
6. specialize the first, last, and C6-padded layers only when each exceeds 10% of the new frame;
7. inspect queue gaps and host submission only after device convolution is within 1.5x CUDA.

The first three items repair execution lifecycle; item 4 is the major compute improvement. Adjusting
the current dead CLBlast-style constants is not an optimization step.

Do not use FP16, mixed precision, looser tolerance, changed tile coverage, or parallel full-image decoding to reach the gate.

## 16. Risks and controls

| Risk | Control |
|---|---|
| CLBlast branding hides a project-owned scalar kernel | Require a faithful algorithmic A/B, keep only source that materially contributes to the winner, and make `UPSTREAM.md` match the actual code. |
| Resident scratch increases VRAM pressure | Report per-variant and full-frame peak bytes, keep one serialized execution state, grow only to observed capacity, and release it with the model/context cache rather than per tile. |
| Event profiling perturbs the product path | Use a separate profiling-enabled in-order queue, retain events without intermediate waits, and keep unprofiled wall time authoritative. |
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
- convolution provenance is truthful: any retained CLBlast-derived source is licensed, traceable,
  and materially used, otherwise the obsolete extract has been removed;
- the Nikon D800E and Fujifilm X-T5 Release means are each below `500 ms` across three hot runs on the local GPU;
- correctness remains within tolerance using pure FP32;
- no general inference runtime, product runtime tuner, full-image Neural parallelism, or unused CLBlast subsystem is introduced.

## 18. Upstream implementation references

- CLBlast direct convolution host selection and kernel structure: <https://github.com/CNugteren/CLBlast/blob/1.7.0/src/routines/levelx/xconvgemm.cpp>
- CLBlast convolution kernel fragments: <https://github.com/CNugteren/CLBlast/tree/1.7.0/src/kernels/levelx>
- CLBlast device/shape tuning guidance: <https://github.com/CNugteren/CLBlast/blob/1.7.0/doc/tuning.md>
- Khronos sub-buffer semantics: <https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clCreateSubBuffer.html>
- Khronos memory-object lifetime semantics: <https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clReleaseMemObject.html>
- Khronos NDRange/local-work-size semantics: <https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clEnqueueNDRangeKernel.html>
- MNN OpenCL buffer convolution variants and channel blocking: <https://github.com/alibaba/MNN/blob/master/source/backend/opencl/execution/buffer/ConvBufExecution.cpp>
- TNN OpenCL convolution channel/output blocking and fused activation: <https://github.com/Tencent/TNN/blob/master/source/tnn/device/opencl/acc/convolution/opencl_conv_layer_common_acc.cc>

These references guide kernel structure and tuning boundaries. They do not justify importing the corresponding inference runtimes.
