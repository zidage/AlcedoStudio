# CUDA CNN Forward Framework + DemosaicNet Plan

Date: 2026-07-09
Updated: 2026-07-13
Primary roadmap owner: `alcedo_studio/src/decoders/processor/nn`

Status: Phases 0–6c landed for the teacher modules (generic CNN ops, hard-coded
Bayer/XTrans modules, lazy cache, goldens, Neural preprocess, and
full-active-area tiled CUDA decode).
Phase 8.1 landed (DemosaicNetPerfHarness + NeuralDemosaicWorkspace allocation
generation counters). **Phase 8A–8D landed:** hard-coded student forwards
(`bayer_s24_d8` / `xtrans_p2_s32_d4`) load bundled student safetensors with full
metadata + fixed one-hot pack/unpack checks and pass exported 1086→1024 /
1048→1024 goldens; shared `CudaTilePolicy` / `BuildTileJobs` plus
`PackReflectPaddedCfaTile` are in place with Legacy geometry regression and
student pad32/step1020/ownership planner tests. **Product `ProcessCudaTiled`
uses student virtual-pad policies** (Bayer pad32/border31/step1024, X-Trans
pad12/border12/step1020), fused `DemosaicStudentTileWithNeuralEngine`, first-writer
ROI assembly, and `NeuralOutputGeometry` so tile border is not re-subtracted from
the assembled frame. **Phase 8E landed:** `EnqueueDemosaicWithNeuralEngine` /
`EnqueueDemosaicStudentTileWithNeuralEngine` (no host wait; require caller-owned
workspace), sync wrappers wait once, `ProcessCudaTiled` pre-warms fixed student
workspace then enqueues pack→forward→ROI copy on one stream with a single final
wait. **Phase 8F landed:** student full-frame rebaseline + product FLOP/byte
roofline on the Release harness (`--model student`), with a retained 3×3 dispatch
fix for student widths (exact Cout-tile 24 / ungated Cout-tile 32 for thin-Cin
post_conv). Authoritative p50 ratios on RTX 3080 Laptop (CC 8.6, 20 iters):
Bayer ~4.22× Legacy, X-Trans ~5.70× Legacy (miss the ≤1.10 objective; FP32 track
still open — effective ~1.6–2.0 TFLOP/s vs sustained ~11.6). Historical teacher
baselines (~2.83 s Bayer / ~9.52 s X-Trans) remain comparison only. **Phase 8G landed (FP32 SIMT 3×3 candidates measured, not product-retained):**
apron-based register-micro-tiled implicit-GEMM kernels for square student trunks
(24→24 / 32→32) were implemented with per-layer harness + `QueryConv2d3x3KernelInfo`
resource reporting. On RTX 3080 Laptop they did not clear the full-frame ≥5% p50
retention rule versus the 8F multi-Cout direct path (best IG full-frame Bayer
~579 ms / X-Trans ~659 ms vs 8F ~499 / ~627 ms), so product dispatch keeps
`Conv2d3x3s1TiledKernel` and candidates remain queryable for re-measurement.
CUDA Graph and multi-lane stay gated; Tensor Core / TF32 / FP16 / BF16 remain
out of scope. See `build/perf/demosaicnet_student_8g_summary.json`. **The
post-8G FP32 small-Cout pass is product-retained:** exact `Cout=12` residual and
`Cout=3` output 1x1 kernels remove partial Cout tiles and duplicate input reads.
Release per-layer medians improved from ~0.65/0.87 to ~0.11/0.30 ms (Bayer) and
~0.78/0.86 to ~0.12/0.36 ms (X-Trans). A short thermally valid full-frame check
measured ~393 / ~427 ms; the long recheck that reached 85-87 C and P8/210 MHz
is rejected as throttled data, not an optimization result. **A benchmark-only
tile concurrency experiment is also complete and rejected for product use:**
2/3 independent stream+workspace lanes reduced throughput by ~5-8% versus one
lane while multiplying owned workspace VRAM (~384-429 MiB per lane). The
product remains single-stream/single-workspace. **Phase 8G+ FP32 Winograd
F(2×2,3×3) measured, not retained:** fused Winograd for square trunks
(24→24 / 32→32) is correct (CPU match + student goldens while dispatched) but
slower than product direct on Release microbench (~0.96 ms / ~3.1 TFLOP/s vs
direct ~0.61 ms / ~4.9 TFLOP/s on Bayer trunk) and regresses short full-frame
(~484 / ~561 ms vs small-Cout+direct ~393 / ~427 ms). Product stays on 8F
direct 3×3; Winograd remains queryable via `QueryConv2d3x3KernelInfo.candidate_*`.
See `build/perf/demosaicnet_student_8g_winograd_summary.json`.
**The follow-up Winograd implementation audit is complete:** the first fused
candidate repeated `B^T d B` once per output channel and transformed immutable
OIHW filters again in every spatial block. The repaired candidate pretransforms
filters once on the host/model-load side, computes each input transform once per
`(tile,Cin)` and shares it across Cout, and pads 16-coefficient shared-memory
records to 17 floats to avoid power-of-two bank aliasing. It is correct across
odd partial tiles and both student goldens, but still loses a same-build Release
recheck: Bayer square trunks ~1.13-1.20 ms vs direct ~0.57-0.60 ms; X-Trans
~1.65-1.68 ms vs direct ~0.91-0.93 ms. Product therefore remains direct. The
repaired path is opt-in through `Conv2dParams::winograd_f22_weight` and harness
`--conv-winograd`, so it cannot silently change normal dispatch. See
`build/perf/demosaicnet_student_8g_{direct_recheck,winograd_repaired}_conv.json`.

**Post-P4-D production decision (2026-07-13):** persistent FP32 NHWC is retained
for the complete product forward: Bayer uses the in-tree C=24 NHWC trunk,
X-Trans uses the CUTLASS C=32 trunk, and both use the NHWC residual/structural
path plus fused pitched-HWC tail. The next mandatory phase is **P5 production
consolidation**, specified in
[`cuda_demosaicnet_performance_next.md` §11](cuda_demosaicnet_performance_next.md).
P5 deletes rejected experiments rather than leaving them queryable. Therefore
all earlier statements in this document that Winograd, implicit-GEMM, CUDA
Graph, multi-lane, ragged/rectangular/strip/large-tile, or ordinary-NCHW neural
paths “stay in-tree”, “remain queryable”, or are kept for remeasurement are
historical results and are superseded by the P5 deletion inventory.

Phase 7 keeps the broader workspace policy / multi-image readiness work. Phase
8 first uses one ordered CUDA stream and one reusable workspace per decode;
multi-stream lanes are optional only if full-frame profiling proves that the
single stream leaves the GPU under-occupied. A separate architectural track
(§3.10) records how `WorkspacePool` generalizes beyond NN to the rest of the
image pipeline.

This document is the handoff plan for:

1. A **forward-only, CNN-focused CUDA inference mini-framework** under
   `alcedo_studio/src/cuda/nn/` (generic primitives, safetensors **DTO** IO,
   **workspace**).
2. A **domain-specific, hard-coded DemosaicNet demosaic module** under the RAW
   processor—not a runtime graph assembler, not a peer of `OpenClContext` /
   `MetalContext`. Topology is fixed in C++ (Bayer / XTrans as specialized
   modules); safetensors only supplies weight tensors via a unified DTO.
3. A **lazy model cache** (load on first user choice of NN demosaic; reuse for
   the rest of the process / later edit sessions) for immutable device weights.
4. A clear separation between **ephemeral scratch** (`WorkspacePool`) and
   **long-lived domain state** (weights)—and a note on migrating the rest of the
   pipeline’s scattered scratch RAII toward the same idea later.

**Non-design (explicit):** do **not** parse safetensors into a dynamic operator
graph, layer list, or “network builder” at runtime. That is not the product
architecture.

Bundled weights:

- `alcedo_studio/src/config/models/bayer.safetensors` (~155 KiB on disk;
  `bayer_s24_d8` student)
- `alcedo_studio/src/config/models/xtrans.safetensors` (~133 KiB on disk;
  `xtrans_p2_s32_d4` student)

Both use `format = demosaicnet-pytorch-state_dict`; Phase 8 validates the full
student architecture/tile metadata and export-report checksum, not only the
format tag.

The stack must be efficient enough to sit inside the RAW GPU path: intermediate
results stay on device, prefer zero-copy with `cv::cuda::GpuMat` at image
boundaries, and avoid layout thrashing. Weights stay resident **after first
use**; activations do not.

---

## 1. Context

### 1.1 Why this exists

Classical demosaic (RCD / AHD / X-Trans interpolation) already lives under
`decoders/processor/operators/gpu/`. The product goal is to add an optional
**learned demosaic** path using the public DemosaicNet architecture
([Gharbi et al. 2016](https://groups.csail.mit.edu/graphics/demosaicnet/),
PyTorch reference: [mgharbi/demosaicnet](https://github.com/mgharbi/demosaicnet)).

We intentionally **do not** pull ONNX Runtime / TensorRT / cuDNN as hard
dependencies for this path:

- The graphs are tiny, fixed CNNs (only Conv / ReLU / a few tensor ops).
- The rest of Alcedo’s GPU image path is custom CUDA + `GpuMat`.
- A small in-tree forward runtime keeps GPLv3 auditability and avoids another
  large runtime in the install package.

### 1.2 What already landed (Phases 0–3)

| Item | Location |
|------|----------|
| `DeviceTensor` (non-owning, rank ≤ 8, f32, strides) | `include/cuda/nn/tensor.hpp` |
| Launch / error helpers | `include/cuda/nn/common.hpp` |
| `DeviceBuffer` (RAII `cudaMalloc` / upload / download) | `include/cuda/nn/device_buffer.hpp` |
| `WorkspacePool` + `WorkspaceScope` (bump allocator) | `include/cuda/nn/workspace.hpp` |
| ReLU, Mul, Concat, Slice, CenterCrop, layout convert | `include/cuda/nn/*`, `cuda/nn/*` |
| `Conv2d` / `Conv2dBiasRelu` (1×1, 2×2 s=2, 3×3 s=1, generic) | `conv2d.hpp` / `conv2d.cu` |
| `ConvTranspose2d` (unpack_mosaick specialization + generic) | `conv_transpose2d.hpp` / `.cu` |
| Safetensors → host DTO (`SafetensorsTensorMap`) | `safetensors.hpp` / `safetensors.cpp` |
| Test suite | `tests/ml_ops/*`, CMake target `MlOpsTest` |
| Linked into | `CudaUtils` library |

Measured ReLU contiguous bandwidth on the author’s machine: ~220–250 GB/s
(out-of-place and inplace). Treat that as the performance floor for future
elementwise ops.

`WorkspacePool` is **explicitly not thread-safe** (“one pool per stream or
serialize access”). That fact drives ownership rules in §3.4.

### 1.3 Research snapshot: how scratch lives today

DemosaicNet is not landing into a clean green field. Existing GPU paths already
own ephemeral device memory in **many independent RAII shapes**:

| Site | What is owned | Pattern |
|------|---------------|---------|
| `GPU_KernelLauncher` (`edit/pipeline/gpu_scheduler.cuh`) | `work_buffer_` + `temp_buffer_` (`float4*`, `cudaMalloc`) | Grow-only; `ReleaseScratchBuffers()`; tracks `allocated_size_` only for *this* launcher |
| H/S local-tone / LLF (`tone_mapping.cuh`) | Per-level pyramid arrays (`source_`, `remap_a/b_`, `output_levels_`) | `EnsurePyramidBuffers` → many raw `cudaMalloc`s; released with stage resources |
| OpenCL / Metal H/S stages | Pyramid buffers via `EnsurePyramidBuffers` | Backend-local caches on stage objects |
| `CUDA::RcdWorkspace` | 5× `GpuMat` (r,g,b,vh_dir,pq_dir) | Stack/member RAII; `Reserve(size)` |
| `CUDA::HighlightWorkspace` | Device counters + mask capacity + `GpuMat result_` | Dedicated class with `Reserve` / `Release` |
| OpenCL pipeline impl | `detail_scratch_` (`OpenClImage`) | Member of executor; `ReleaseScratchBuffers` |
| Metal pipeline path | Ephemeral `MetalImage scratch` | Often call-local |
| RAW profiling | `cudaMemGetInfo` free/total | **Driver-level** residual VRAM, not sum of owned scratch |
| RHI present path | `cudaMemGetInfo` around present slots | Same: global free/total logging |

Pipeline already has *partial* discipline (`ReleasePreviewGpuScratch`,
`DebugGetAllocatedScratchBytes` on the merged-stage launcher) but:

1. **Accounting is incomplete.** Only the launcher’s two float4 buffers (and
   similar per-backend fields) are summed. LLF pyramids, RCD temps, highlight
   temps, geometry intermediates, etc. sit outside that counter.
2. **Ownership is fragmented.** Every operator invents a workspace struct /
   `EnsureXBuffers` path. Hard to reason about peak VRAM or reclaim policy.
3. **Observability falls back to driver APIs.** `cudaMemGetInfo` answers “how
   much is free on the device?” not “which subsystem owns what?”

**Implication for this plan (two folds):**

1. **Some shared scratch discipline is necessary** eventually—not as a device
   manager like OpenCL/Metal context, but as a **reusable workspace allocator**
   with byte accounting. `WorkspacePool` is the right seed for that (see
   §3.10). DemosaicNet is a first *good* consumer; it is not the last.
2. **DemosaicNet itself must stay narrow.** It is a domain model for demosaic,
   will later need OpenCL/Metal backends too, and must **not** be framed as a
   process-global GPU “context” peer of `OpenClContext` / `MetalContext`.

### 1.4 Pipeline constraints (non-negotiable)

1. **Device residency.** Host round-trips only for weights at load time and for
   tests. Forward never D2H intermediate activations.
2. **`GpuMat` at the image edge only.** CFA / RGB frames enter and leave as
   `cv::cuda::GpuMat` (`CV_32F`, typically HWC). Internal CNN activations are
   contiguous **NCHW** `DeviceTensor` buffers. Convert once in, once out.
3. **No unnecessary transpose.** Prefer one HWC→NCHW pack at the entry of the
   model, then stay NCHW until the final unpack to HWC `GpuMat`.
4. **Stream-aware.** Every op takes `cudaStream_t` / `cv::cuda::Stream*`. Default
   stream is allowed for tests; pipeline code must pass the active stream.
5. **Intermediates are ephemeral.** Allocate from a reusable workspace pool; do
   not retain full-res activation pyramids between frames.
6. **Valid convolutions match the released weights.** Pretrained demosaicnet is
   built with `pad=False` (no zero-padding on 3×3). Spatial size shrinks; the
   skip path uses center-crop (`_crop_like`). Do not silently switch to
   same-padding or the weights will be wrong.
7. **f32 only** for the first milestone (weights and activations are F32).
8. **Lazy weight load, then reuse.** Do not load at app/image-backend boot.
   Load on first request for NN demosaic (user-selected method / first
   qualifying decode). Keep resident and reuse across subsequent images and
   edit sessions in the same process. Never re-parse + H2D on every
   `RawProcessor` construction. See §3.5–§3.6.
9. **Domain model stays in the RAW processor module.** Generic CNN ops +
   `WorkspacePool` live in `cuda/nn/`; DemosaicNet graph, CFA mosaic pack, and
   **model cache** live under `decoders/processor/`. Future OpenCL/Metal
   demosaicnet ports extend the same domain module, not `OpenClContext`.

### 1.5 Namespace and file layout convention

**Generic CUDA CNN / scratch runtime** (ops, tensors, workspace, safetensors):

```text
alcedo_studio/src/include/cuda/nn/     # public headers
alcedo_studio/src/cuda/nn/             # .cu / .cpp implementations
alcedo_studio/tests/ml_ops/            # operator + IO + workspace tests
```

Namespace: `alcedo::cuda::nn`.

Library: keep growing `CudaUtils` until it becomes large enough to split
(`CudaNn`); do not create a second CUDA static lib without need.

**Domain DemosaicNet submodule** (RAW-only; hard-coded demosaic modules):

```text
alcedo_studio/src/include/decoders/processor/nn/
  demosaicnet_cache.hpp        # lazy cache of hard-coded module instances
  demosaicnet_module.hpp       # CRTP base: LoadWeights(SafetensorsTensorMap DTO)
  demosaicnet_bayer.hpp        # BayerDemosaicNet : fixed topology + device weights
  demosaicnet_xtrans.hpp       # XTransDemosaicNet : fixed topology + device weights
  demosaicnet_mosaic.hpp       # CFA → 3ch demosaicnet mosaic pack helpers

alcedo_studio/src/decoders/processor/nn/
  demosaicnet_cache.cpp
  demosaicnet_bayer.cu         # hard-coded Forward + LoadWeights impl
  demosaicnet_xtrans.cu
  demosaicnet_mosaic.cu

alcedo_studio/src/include/decoders/processor/operators/gpu/
  cuda_demosaicnet.hpp         # GpuMat entry used by raw_processor_cuda

alcedo_studio/src/decoders/processor/operators/gpu/
  cuda_demosaicnet.cu
```

Namespace sketch: keep under `alcedo` / decoder-local types consistent with
existing `CUDA::RcdWorkspace` style. Prefer names that read as **model cache /
hard-coded module / forward**, never as `*Context` that could be confused with
`OpenClContext` / `MetalContext`.

**Naming ban for implementers:** do **not** introduce
`DemosaicNetContext` as a process-global device-manager peer. Prefer:

- `DemosaicNetModelCache` — lazy cache of loaded module instances
- `BayerDemosaicNet` / `XTransDemosaicNet` — hard-coded modules (CRTP)
- `SafetensorsTensorMap` (or similar) — unified host DTO from the parser

---

## 2. DemosaicNet graphs (authoritative operator inventory)

Source of truth for structure: `demosaicnet/modules.py` (`BayerDemosaick`,
`XTransDemosaick`). Source of truth for tensors: the two `.safetensors` files.

**Important naming map** (PyTorch module → safetensors keys):

| PyTorch | Bayer safetensors keys |
|---------|------------------------|
| `pack_mosaic` | `pack_mosaick.{weight,bias}` |
| `main_processor.conv{i}` / `relu{i}` | `conv{i}.{weight,bias}` (ReLU has no params) |
| `residual_predictor` | `residual.{weight,bias}` |
| `upsampler` (`ConvTranspose2d`, groups=3) | `unpack_mosaick.{weight,bias}` |
| `fullres_processor.post_conv` / `post_relu` | `post_conv1.{weight,bias}` |
| `fullres_processor.output` | `output.{weight,bias}` |

### 2.1 Bayer (`bayer.safetensors`, depth=15, width=64)

**Input:** mosaic tensor `N×3×H×W` f32. Convention matches demosaicnet: a full
RGB tensor with only the CFA-sampled sites filled (others zero), **or** an
equivalent representation produced by our pipeline packer. Spatial `H,W` should
be even (pack uses 2×2 stride-2).

**Forward (exact order):**

```text
x0 = mosaic                                            # [N, 3, H, W]

# --- low-res branch ---
p  = Conv2d(x0; 3→4, k=2, s=2, pad=0) + bias           # pack_mosaick  [N,4,H/2,W/2]
h  = p
for i in 1..14:
    h = ReLU( Conv2d(h; Cin→64, k=3, s=1, pad=0) + b ) # conv1: 4→64; conv2..14: 64→64
h  = ReLU( Conv2d(h; 64→128, k=3, s=1, pad=0) + b )    # conv15
filters, masks = SplitChannels(h, 64, 64)              # each [N,64,h',w']
filtered = filters ⊙ masks                             # Mul
residual = Conv2d(filtered; 64→12, k=1, s=1) + b       # residual
up = ConvTranspose2d(residual; 12→3, k=2, s=2,
                     groups=3) + b                     # unpack_mosaick  [N,3,≈H,≈W]

# --- full-res branch ---
cropped = CenterCropTo(mosaic, up.spatial)             # crop_like
cat     = ConcatChannels(cropped, up)                  # [N,6,h_u,w_u]
y       = ReLU( Conv2d(cat; 6→64, k=3, s=1, pad=0)+b ) # post_conv1
out     = Conv2d(y; 64→3, k=1, s=1) + b                # output
return out
```

**Weight shapes (verify at load):**

| Key | Shape |
|-----|-------|
| `pack_mosaick.weight` | `[4, 3, 2, 2]` |
| `conv1.weight` | `[64, 4, 3, 3]` |
| `conv2..conv14.weight` | `[64, 64, 3, 3]` |
| `conv15.weight` | `[128, 64, 3, 3]` |
| `residual.weight` | `[12, 64, 1, 1]` |
| `unpack_mosaick.weight` | `[12, 1, 2, 2]` (ConvTranspose2d, groups=3) |
| `post_conv1.weight` | `[64, 6, 3, 3]` |
| `output.weight` | `[3, 64, 1, 1]` |
| `*.bias` | matching out-channels |

Spatial note: each valid 3×3 shrinks H and W by 2. After 15 such layers the
low-res map is much smaller than `H/2`; the transpose upsample does **not**
restore full `H×W` by itself relative to the original mosaic after all the
shrinks — `_crop_like` centers the mosaic to the upsampled residual’s spatial
size. Implement crop exactly as:

```text
crop_h = src_h - tgt_h;  crop_t = crop_h // 2; crop_b = crop_h - crop_t
crop_w = src_w - tgt_w;  crop_l = crop_w // 2; crop_r = crop_w - crop_l
return src[..., crop_t : src_h-crop_b, crop_l : src_w-crop_r]
```

### 2.2 X-Trans (`xtrans.safetensors`, depth=11, width=64)

**No pack / residual / transpose.** Full resolution throughout.

```text
h = mosaic                                             # [N, 3, H, W]
for i in 1..11:
    Cin = 3 if i==1 else 64
    h = ReLU( Conv2d(h; Cin→64, k=3, s=1, pad=0) + b )
cropped = CenterCropTo(mosaic, h.spatial)
cat     = ConcatChannels(cropped, h)                   # [N, 67, h', w']
y       = ReLU( Conv2d(cat; 67→64, k=3, s=1, pad=0)+b )# post_conv1
out     = Conv2d(y; 64→3, k=1, s=1) + b                # output
return out
```

| Key | Shape |
|-----|-------|
| `conv1.weight` | `[64, 3, 3, 3]` |
| `conv2..conv11.weight` | `[64, 64, 3, 3]` |
| `post_conv1.weight` | `[64, 67, 3, 3]` |
| `output.weight` | `[3, 64, 1, 1]` |

### 2.3 Complete primitive checklist (do not ship model without these)

| # | Primitive | Used by | Notes |
|---|-----------|---------|-------|
| 1 | **ReLU** | Both | Done. Keep fused with Conv later. |
| 2 | **Conv2d + bias** | Both | k∈{1,2,3}, s∈{1,2}, pad=0, groups=1 for all weight keys except transpose. |
| 3 | **ConvTranspose2d + bias** | Bayer only | k=2, s=2, groups=3, out=3. |
| 4 | **Mul** (elementwise) | Bayer | `filters * masks`. |
| 5 | **Split / Slice** (channel) | Bayer | Split 128 → 64+64. |
| 6 | **Concat** (channel) | Both | Bayer 3+3→6; XTrans 3+64→67. |
| 7 | **CenterCrop** (spatial) | Both | `_crop_like`. |
| 8 | **Layout convert** | Boundary | HWC `GpuMat` ↔ NCHW tensor (and optional planar pack). |
| 9 | **Safetensors → DTO** | Both | Host parse to a unified tensor map; **no** graph build. |
| 10 | **Workspace allocator** | Both | Scratch tensors; no per-layer `cudaMalloc` in steady state. |
| 11 | **Hard-coded modules** | Both | Compile-time Bayer / XTrans topology; `LoadWeights(DTO)` via CRTP. |
| 12 | **Lazy model cache** | Both | RAW-scoped cache of loaded modules; not a platform GPU context. |

Optional later (not required for bit-accuracy vs demosaicnet, but useful):

- **Add** (generic residual)
- **LeakyReLU / GELU** (not in these weights)
- **Pad** (only if we later train same-pad models)

### 2.4 Fused op opportunities (required for pipeline speed)

Implement fused variants **as the default path** once the unfused ops are
correct. Unfused remains for unit tests.

| Fused kernel | Replaces | Why |
|--------------|----------|-----|
| `ConvBiasRelu` | Conv2d + bias + ReLU | Every main/post layer except final `output` and `residual`. |
| `ConvBias` | Conv2d + bias | Final `output`, `residual`, `pack`, `unpack`. |
| `ChannelMul` | Split is free if Conv15 writes 128 contiguous channels; Mul is one bandwidth pass | Avoid materializing two temporaries if possible. |
| `Concat` as view | Prefer writing next conv to read two sources via dual-input kernel **or** one tightly packed concat buffer | Concat of large maps is pure bandwidth; keep it contiguous. |

Do **not** fuse across the Bayer split/mul boundary until tests lock numerics.

---

## 3. Target architecture

### 3.1 What is a “context” here (and what is not)

| Kind | Examples in tree | Owns | Lifetime | Role for DemosaicNet |
|------|------------------|------|----------|----------------------|
| **Platform GPU context** | `OpenClContext`, `MetalContext`, `OpenClProgramLibrary` | Device, queue, program compile | Process | **Out of scope.** DemosaicNet does not become one of these. |
| **Generic scratch / workspace** | new `WorkspacePool`; eventually could cover launcher work/temp, LLF levels | Ephemeral device bytes + byte accounting | Frame / session / lease | **Shared idea**, first used by NN; broader migration is §3.10 |
| **Hard-coded NN module** | planned `BayerDemosaicNet` / `XTransDemosaicNet` | Fixed topology + device weight slots | Module instance (cached lazy) | **Yes — specialized demosaic, not a graph IR** |
| **Domain model cache** | planned `DemosaicNetModelCache` | Loaded module instances | Lazy → process (until unload) | **Yes — RAW-scoped only** |
| **Operator-local workspace structs** | `RcdWorkspace`, `HighlightWorkspace`, LLF pyramids | Typed temps for one algorithm | Call / stage | Legacy pattern; do not invent another for NN |

**DemosaicNet is fold (B): domain-scoped hard-coded demosaic modules.**  
**WorkspacePool is fold (A): the necessary “some context” for scratch**—but it
is an **allocator**, not a device manager, and it must not be named or
documented as “the DemosaicNet context.”

### 3.2 Layers

```text
┌──────────────────────────────────────────────────────────────────────┐
│  Product trigger (editor / RawParams)                                │
│    User selects DemosaicNet  →  first decode that needs it           │
│    lazy EnsureLoaded(Bayer|XTrans)  →  module stays for process      │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ shared hard-coded module (after first load)
┌───────────────────────────────▼──────────────────────────────────────┐
│  RAW stage (per image / per worker)                                  │
│    RawProcessor::ProcessCuda*  →  CUDA::DemosaicNet*(GpuMat, …)      │
│    call-local: stream + WorkspacePool (activations only)             │
└───────────────────────────────┬──────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────┐
│  Domain submodule  decoders/processor/nn/   (RAW only)               │
│    DemosaicNetModelCache     lazy cache of module instances          │
│    CRTP NnWeightModule       LoadWeights(SafetensorsTensorMap)       │
│    BayerDemosaicNet          HARD-CODED topology + Forward           │
│    XTransDemosaicNet         HARD-CODED topology + Forward           │
│    mosaic pack               CFA → 3ch demosaicnet input             │
│    [future] OpenCL/Metal     same modules, other backends            │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ LoadWeights consumes DTO only
┌───────────────────────────────▼──────────────────────────────────────┐
│  Generic  cuda/nn                                                    │
│    ops: relu, conv2d, conv_transpose2d, mul, concat, crop, layout    │
│    DeviceTensor, DeviceBuffer, WorkspacePool                         │
│    safetensors parser → SafetensorsTensorMap (host DTO, no graph)    │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.2.1 Hard-coded modules + safetensors DTO + CRTP (authoritative)

**Intent:** DemosaicNet is a **special demosaic algorithm** with a fixed graph,
written out in C++ the same way RCD is a fixed algorithm—not an interpreter for
arbitrary CNN graphs.

| Piece | Responsibility | Does **not** do |
|-------|----------------|-----------------|
| Safetensors parser (`cuda/nn`) | File → unified host **DTO** (`SafetensorsTensorMap`: name → dtype, shape, bytes) | Build layers, infer topology, allocate device graphs |
| CRTP base (`NnWeightModule<Derived>` or similar) | Unified `LoadWeights(const SafetensorsTensorMap&)` entry; optional helpers (`RequireF32(key, shape)`, upload slot) | Own demosaic policy or CFA knowledge |
| `BayerDemosaicNet` / `XTransDemosaicNet` | **Compile-time** topology (member weight buffers + hard-coded `Forward`); derived `LoadWeights` maps **known keys** into **known slots** | Runtime assembly from key set; dynamic depth/width |
| Model cache | Lazy construct + `LoadWeights` once per variant; hold the module instance | Parse files every decode |

**DTO sketch** (host-only, generic; lives next to safetensors parser):

```text
struct SafetensorsTensor {
  std::string name;
  // dtype: F32 for v1; reject others or store enum
  std::vector<std::int64_t> shape;
  std::vector<float> data;          // contiguous host payload (F32)
  // optional: data_offsets metadata if zero-copy mmap later
};

// Unified DTO handed to every hard-coded module.
using SafetensorsTensorMap = std::unordered_map<std::string, SafetensorsTensor>;
// or a small class with At(key), Require(key, shape), etc.
```

**CRTP sketch** (domain or thin generic helper—prefer domain header if the base
is only used by demosaic modules; keep helpers that are pure DTO→buffer in
`cuda/nn` if reusable):

```text
// Derived hard-codes topology. Base only standardizes weight ingestion.
template <typename Derived>
class NnWeightModule {
 public:
  // Unified entry: parser DTO in → device weight slots filled by Derived.
  void LoadWeights(const SafetensorsTensorMap& tensors, cudaStream_t stream = nullptr) {
    static_cast<Derived*>(this)->LoadWeightsImpl(tensors, stream);
    loaded_ = true;
  }

  [[nodiscard]] auto weights_loaded() const -> bool { return loaded_; }

 protected:
  // Shared helpers available to Derived::LoadWeightsImpl:
  //   RequireTensor(map, key, expected_shape) -> const SafetensorsTensor&
  //   UploadTo(DeviceBuffer&, const SafetensorsTensor&, stream)
  bool loaded_ = false;
};

class BayerDemosaicNet : public NnWeightModule<BayerDemosaicNet> {
 public:
  void LoadWeightsImpl(const SafetensorsTensorMap& t, cudaStream_t stream);
  void Forward(DeviceTensor in, DeviceTensor out, WorkspacePool& ws,
               cudaStream_t stream) const;

 private:
  // Fixed slots — not a vector<Layer>.
  DeviceBuffer pack_w_, pack_b_;
  DeviceBuffer conv_w_[15], conv_b_[15];   // or explicit conv1..conv15 members
  DeviceBuffer residual_w_, residual_b_;
  DeviceBuffer unpack_w_, unpack_b_;
  DeviceBuffer post_w_, post_b_;
  DeviceBuffer output_w_, output_b_;
};

class XTransDemosaicNet : public NnWeightModule<XTransDemosaicNet> { ... };
```

**`LoadWeightsImpl` contract (per derived module):**

1. Require every key listed in §2.1 or §2.2 with exact F32 shapes.
2. Reject missing / extra-critical / wrong-shape tensors with a clear error
   (extra unknown keys may be ignored).
3. Upload into the fixed `DeviceBuffer` slots (stream-aware H2D).
4. **Never** resize the graph, change depth/width, or allocate a “layer list”
   from the file. If the file does not match the hard-coded module, load fails.

**`Forward` contract:** straight-line code calling `cuda::nn` ops in the order
of §2.1 / §2.2—same spirit as writing out RCD stages. No interpreter loop over
a runtime graph IR.

**Why CRTP (not virtual base for v1):**

- Zero overhead on the load path; derived can keep non-virtual `Forward` and
  concrete weight members without vtable on the hot path.
- Shared `LoadWeights` entry + `RequireTensor` / upload helpers without forcing
  a polymorphic container of “any network.”
- If a third hard-coded RAW NN appears later, it reuses the same DTO + CRTP
  load pattern without inventing a graph runtime.

**Anti-patterns (forbidden):**

- `vector<unique_ptr<Layer>>` built by scanning safetensors keys.
- ONNX-style “execute graph from file” for demosaicnet.
- Storing only a blob of weights without named fixed slots mapped at load.
- Generic “model zoo runner” in `cuda/nn` that demosaic plugs into as data.

### 3.3 Tensor policy

- **Internal:** contiguous NCHW `DeviceTensor`, `float*`, pool-backed.
- **Boundary:** explicit `PackHwcToNchw` / `UnpackNchwToHwc` for model I/O when
  channels > 1.
- **Weights:** PyTorch OIHW layout, uploaded **once per variant** on lazy load;
  never reordered per frame unless a kernel needs a packed copy (then cache that
  packing on the weight pack, still once).

### 3.4 Conv algorithm choices (efficiency)

Priority order for implementation:

1. **1×1 Conv** → hand-written tiled GEMM (cudart only; **no cuBLAS** — packaging
   ships only `cudart64_*.dll`). Shape: `(N*H*W) × Cin` × `Cin × Cout`.
2. **2×2 stride-2 pack** (`pack_mosaick`) → specialized kernel (tiny K, high
   bandwidth). Do not go through general im2col.
3. **3×3 valid, s=1, C≈64** → primary hot path via direct kernel with filter in
   shared memory. Optional later: im2col + custom GEMM or Winograd F(2×2,3×3)
   if profiling shows conv dominates (still without cuBLAS/cuDNN).
4. **ConvTranspose 2×2 s=2 groups=3** → specialized depthwise-ish upsample
   kernel (12→3 with groups=3 is cheap; do not use a general transpose path).
5. **Bias** → fused into epilogue of the same kernel (do not launch a second
   pass).

**Dependency policy:**

- `CUDA::cudart` required (already) — **only** CUDA runtime DLL in the install.
- **Do not** link cuBLAS or cuDNN (would pull extra DLLs beyond `cudart64`).
- No Torch / ONNX Runtime.

### 3.5 Ownership split: model cache vs execution workspace

| Resource | Owner | Lifetime | Concurrent use |
|----------|-------|----------|----------------|
| Safetensors on disk | install / `config/models` | N/A | read-only |
| Host DTO (`SafetensorsTensorMap`) | transient during lazy load | first use of that variant | single-threaded load under mutex |
| **Hard-coded module + device weight slots** | **`BayerDemosaicNet` / `XTransDemosaicNet` inside model cache** | **lazy → process** | **immutable after load; many readers OK** |
| Graph topology | C++ source of the module (`Forward` body) | compile-time | reentrant |
| **`WorkspacePool` activations** | **caller (RawProcessor path / test)** | frame / tile | **not shared across concurrent forwards** |
| `cudaStream` | caller | per `Process()` | one stream per concurrent job |
| CFA pack temps | workspace or call-local GpuMats | frame | same as workspace |

**Why weights are cached (but not “backend-init context”)**

- Combined device footprint is small (~few MiB f32). Keeping a loaded variant
  resident is cheap vs a full-res RAW frame.
- `RawProcessor` is constructed per decode. Reloading safetensors + H2D every
  time is pure waste.
- Users who never choose NN demosaic should **not** pay load cost at app start.
- After the first editor choice, weights should survive across images and edit
  sessions in the same process (lazy once, then reuse).

**Why workspace is separate**

- `WorkspacePool` is a bump allocator and is **not thread-safe**.
- Concurrent forwards that share one pool corrupt activations.
- Activations dominate peak VRAM; they must be reclaimable without unloading
  weights (same spirit as `ReleasePreviewGpuScratch` vs keeping LUTs).

**Rule:** after `LoadWeights`, the module’s weight slots are immutable for
Forward. Forward is a pure function of `(module weights, input, stream,
workspace)`. Topology never comes from the file.

### 3.6 Lazy model cache API (RAW-scoped design target)

Not a platform device context. A small domain service owned by the RAW module.
It caches **hard-coded module instances**, not a graph IR.

```text
// Sketch only — names may adjust to local style.

enum class DemosaicNetVariant { Bayer, XTrans };

struct DemosaicNetLoadOptions {
  std::filesystem::path model_dir;  // contains bayer.safetensors, xtrans.safetensors
  // optional: prefer async H2D on a given stream
};

class DemosaicNetModelCache {
 public:
  static auto Instance() -> DemosaicNetModelCache&;

  // Lazy: no-op if already loaded. Thread-safe. Per variant:
  //   1) Parse safetensors → SafetensorsTensorMap (DTO)
  //   2) Construct BayerDemosaicNet / XTransDemosaicNet (empty slots)
  //   3) module.LoadWeights(dto)  // CRTP; fills fixed slots
  auto EnsureLoaded(DemosaicNetVariant variant,
                    const DemosaicNetLoadOptions& options = {}) -> bool;

  [[nodiscard]] auto IsLoaded(DemosaicNetVariant) const -> bool;
  [[nodiscard]] auto LastError() const -> std::string;
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  [[nodiscard]] auto Bayer() const -> const BayerDemosaicNet&;
  [[nodiscard]] auto XTrans() const -> const XTransDemosaicNet&;
};
```

Load path (canonical):

```text
path = model_dir / "bayer.safetensors"
dto  = cuda::nn::LoadSafetensors(path)          // Phase 4: host DTO only
auto module = std::make_unique<BayerDemosaicNet>();
module->LoadWeights(dto, stream);               // Phase 5: CRTP → fixed slots
cache.Store(std::move(module));
// later: cache.Bayer().Forward(in, out, workspace, stream);  // hard-coded graph
```

GpuMat operator sketch (RAW pipeline edge):

```text
// Ensures the needed variant is loaded (lazy), then module.Forward(...).
// Never fopen's safetensors from RawProcessor ctor.
bool CUDA::DemosaicNetBayer(const cv::cuda::GpuMat& cfa_or_mosaic,
                            cv::cuda::GpuMat& rgb_out,
                            BayerPattern pattern,
                            WorkspacePool& workspace,
                            cv::cuda::Stream* stream);
```

**Anti-patterns (forbidden):**

- Eager load at app main / “CUDA image backend initialized” for users who never
  enable NN demosaic.
- Loading safetensors inside every `RawProcessor` constructor.
- Naming this type `*Context` next to `OpenClContext` / `MetalContext`.
- Putting CFA / LibRaw / demosaic policy into `cuda/nn`.
- Stuffing a process-global mutable `WorkspacePool` into the model cache without
  a lease protocol.
- Assembling a runtime layer graph from the DTO key list.

### 3.7 When to load (product lifecycle)

| Event | Action |
|-------|--------|
| App start / CUDA backend ready | **Nothing** for DemosaicNet |
| Thumbnail / import decode (default Classical) | **Nothing** |
| User selects NN demosaic in editor (or RawParams) | Soft preference only; may still defer disk load |
| First `ProcessCuda` that actually needs NN | `EnsureLoaded(variant)` once; reuse thereafter |
| Later images / edit sessions in same process | Reuse resident weights |
| Process exit | Destruct cache; free device weights |
| Optional future: low-VRAM pressure | `Unload` if product wants; not required for v1 |

**Fail soft:** if load fails → Classical demosaic; log once per variant.

**Tests:** call `EnsureLoaded` in suite setup (or let the first golden forward
trigger it). Assert second forward does not change weight device pointers.

Model path resolution order:

1. Explicit `DemosaicNetLoadOptions::model_dir`
2. Compile-time `ALCEDO_DEMOASICNET_MODEL_DIR` (tests / dev)
3. Install-relative app resource path (packaging)
4. Source-tree `alcedo_studio/src/config/models` (dev convenience only)

### 3.8 Parallelism and multi-worker sharing

**Product fact:** parallel RAW work today is primarily thumbnail generation via
`DecoderScheduler` / `ThumbnailService` thread pools. Thumbnails should continue
to use **classical** demosaic. NN demosaic is for quality / editor full-res.

**Engineering requirement:** design as if multiple concurrent NN forwards can
share the **weight cache**.

| Scenario | Weights | Workspace | Stream |
|----------|---------|-----------|--------|
| Editor full-res NN demosaic | lazy-loaded cache | one pool | one stream |
| N concurrent NN demosaics (future) | same cache | **N pools** | **N streams** |
| Thumbnail workers | cache may be cold or warm but unused | Classical path | N/A |

Concurrency rules:

1. Read-only device weights: safe across streams after load.
2. Each concurrent forward: distinct `WorkspacePool` (or serialize).
3. `EnsureLoaded`: only writer path; mutex / call_once per variant.
4. No global CUDA lock around Forward for correctness.
5. Calling thread must have the correct CUDA device current (existing RAW rule).

### 3.9 Workspace / memory (activations for DemosaicNet)

`WorkspacePool` requirements for NN forward:

- Per-execution reusable device slab (caller-owned in v1).
- Grow-only; `Reserve(peak)` before hot path.
- Steady-state: **zero** `cudaMalloc` / `cudaFree` after reserve.
- `used_bytes()` / `capacity_bytes()` enable **owned** VRAM accounting without
  calling `cudaMemGetInfo`.

For full-resolution RAW Neural path (**Phase 6**):

1. **Preprocess (6b):** after `ToLinearRef` (which already did WB), CFA
   phase-align to training origin + gamma 1/2.2 in; after the network, gamma
   2.2 out back to linear camera RGB. When HLR is on, **no** `Clamp01` (needs
   values `> 1`); when HLR is off, `Clamp01` is expected (same as today).
2. **Tile (6c):** reuse the existing CUDA full-frame vs tiled split with a
   demosaic-specific RF halo — not a greenfield scheduler. See Phase 6.

Peak weight VRAM is tiny; peak **activation** VRAM dominates.

### 3.10 Broader workspace architecture (follow-on track, not NN-blocking)

This subsection records why a workspace abstraction is necessary beyond
DemosaicNet, based on the inventory in §1.3. **It is not required to finish
Phases 4–6**, but implementers should not invent a third private scratch style
for NN that makes future unification harder.

**Goals (later PRs / phases outside or after NN v1):**

1. **One allocator vocabulary** for ephemeral GPU bytes: reserve / allocate /
   reset / release / `capacity_bytes` / `used_bytes`.
2. **Composable ownership:** a pipeline stage or RAW process owns one (or a few)
   pools; algorithms borrow slices instead of each owning five `GpuMat`s and
   four `cudaMalloc` pyramids.
3. **Accurate VRAM reporting:** sum of known owners (pools + image buffers +
   LUTs + model caches) as first-class metrics; use `cudaMemGetInfo` only as a
   residual/sanity check, not the sole truth.
4. **Reclaim policies already exist** (`ReleasePreviewGpuScratch`,
   `ReleaseResources`)—extend them to drop pool capacity high-water marks the
   same way launcher work/temp is dropped today.

**Suggested migration order (non-blocking for demosaicnet goldens):**

| Step | Target | Note |
|------|--------|------|
| A | DemosaicNet activations use `WorkspacePool` | **This plan Phases 5–7** |
| B | Optional: RAW `RcdWorkspace` / `HighlightWorkspace` temps that are pure scratch | Keep typed views if helpful; back storage with pool where practical |
| C | `GPU_KernelLauncher` work/temp → pool or shared slab with byte accounting | Aligns with existing `GetAllocatedScratchBytes` |
| D | LLF / H/S pyramid levels → pool-backed or size-class slabs | Largest win for mid-tone / local tone VRAM |
| E | Cross-backend (OpenCL/Metal) analogous bump or ring scratch | Same *policy*, backend-specific storage |

**What stays out of WorkspacePool:**

- Long-lived **image** buffers (`ImageBuffer` / present slots).
- Long-lived **LUTs** and CST tables.
- Long-lived **model weights** (model cache).
- Anything that must survive across frames with identity (cached masks keyed by
  `hs_mask_base_cache_key_` may be “session scratch” with a different policy—
  do not force them into a per-forward bump without a design pass).

### 3.11 Numerics

- Match demosaicnet f32 PyTorch forward within a tight tolerance on random and
  real mosaics (suggested: max abs error ≤ 1e-4 on synthetic; ≤ 1e-3 on long
  chains if intermediate rounding differs — lock exact thresholds in tests).
- Deterministic given fixed weights and inputs (document any unavoidable GEMM
  nondeterminism and use relaxed atol if needed).

### 3.12 Future OpenCL / Metal demosaicnet

Domain API (`EnsureLoaded`, hard-coded module `Forward`, GpuMat-ish edge) should
be **backend-agnostic at the RAW policy layer**. Topology remains hard-coded per
backend (or shared host orchestration with backend kernels). v1 is CUDA only.
Later:

- Same safetensors → same host DTO; same CRTP `LoadWeights` key tables.
- Backend-specific device weight storage and kernels.
- Same lazy cache policy (load on first use of that backend+variant).
- Do **not** hang this off `OpenClContext` as “another program”; treat it as RAW
  domain state that *uses* the platform context for device/queue access.
- Still **no** runtime graph assembly on any backend.

---

## 4. Execution plan (for the next LLM / implementer)

Work **phase by phase**. Each phase ends with green `MlOpsTest` (and any new
binaries). Do not start RAW pipeline wiring before Phase 5 model runners pass
numerical tests. Do not claim “ready for product full-res” before Phase 6b preprocess
(phase-align + gamma) and Phase 6c tiling (reuse existing CUDA tile path) are
both in. Do **not** block Phase 4–6 on full pipeline workspace migration
(§3.10).

### Phase 0 — Core tensor + ReLU  ✅ (done)

**Done:**

- [x] `DeviceTensor`, ReLU, GpuMat overloads, `MlOpsTest` correctness + bandwidth.
- [x] `DeviceBuffer` owning wrapper under `include/cuda/nn/device_buffer.hpp`.
- [x] `WorkspacePool` + `WorkspaceScope` under `include/cuda/nn/workspace.hpp`.

**Exit:** existing ReLU tests still pass; buffer/pool unit tests added.

### Phase 1 — Elementwise + structural ops  ✅ (done)

**Done:**

| Op | Files | Acceptance |
|----|-------|------------|
| `Mul` | `mul.hpp` / `mul.cu` | float4 path; inplace optional |
| `ConcatChannels` | `concat.hpp` / `concat.cu` | only channel axis; output contiguous |
| `SliceChannels` / `SplitChannels` | `slice.hpp` / `slice.cu` | view if contiguous NCHW; else copy |
| `CenterCropSpatial` | `crop.hpp` / `crop.cu` | exact demosaicnet `_crop_like` |
| `PackHwcToNchw` / `UnpackNchwToHwc` | `layout.hpp` / `layout.cu` | GpuMat CV_32FC3 ↔ NCHW |

**Tests:** correctness, structural compose, layout round-trip, elementwise
bandwidth floor. ✅

**Exit:** all new tests in `MlOpsTest` green.

### Phase 2 — Conv2d + bias (+ fuse ReLU)  ✅ (done)

**Done:** specialized 1×1 / 2×2 s=2 / 3×3 paths; real weight layer tests; fused
`Conv2dBiasRelu`; cudart-only. See `conv2d_test.cu`.

**Exit:** conv unit tests green; no `cudaMalloc` on hot path when workspace
provided.

### Phase 3 — ConvTranspose2d (Bayer unpack)  ✅ (done)

**Done:** unpack_mosaick specialization + generic grouped fallback;
`conv_transpose2d_test.cu` green.

**Exit:** green tests; used only by Bayer runner later.

### Phase 4 — Safetensors parser → unified host DTO (`cuda/nn` only)  ✅ (done)

**Done:** `LoadSafetensors` → `SafetensorsTensorMap` (nlohmann::json header);
`RequireF32Tensor` / `UploadToDevice` / `UploadTo`; real-model shape tables +
synthetic negative tests; conv2d/conv_transpose real-weight tests use the DTO.

**Scope boundary:** file format → **host DTO** only. No demosaic topology, no
runtime graph, no RAW module types, no pipeline migration.

**Design rule for this phase:** the output of Phase 4 is a bag of named host
tensors (`SafetensorsTensorMap`). It is **not** a network. Hard-coded modules
in Phase 5 consume that DTO via CRTP `LoadWeights`.

**Implement:**

1. **Safetensors reader** (header JSON + raw tensor blobs), little-endian F32.
   - Minimal in-tree parser (header size `uint64` + JSON + data offset).
   - API: `LoadSafetensors(path) -> SafetensorsTensorMap` (or equivalent DTO
     type with `find` / `at` / iteration).
2. **Per-tensor record:** name, dtype, shape, contiguous host `float` payload
   (v1: require F32; clear error on other dtypes).
3. **Lookup helpers** (generic, for CRTP loaders later):
   - `Find(map, key)`, `Require(map, key, expected_shape)` → const ref or throw.
   - Shape compare utility.
4. **Optional thin upload helper** (may live here or next to `DeviceBuffer`):
   host tensor → `DeviceBuffer` H2D (stream-aware). Used by Phase 5
   `LoadWeightsImpl`; not a “build model” API.
5. **Deduplicate** ad-hoc parsers in `conv2d_test.cu` /
   `conv_transpose2d_test.cu` if cheap in the same PR.

**Files:**

```text
include/cuda/nn/safetensors.hpp      # DTO + LoadSafetensors + Require helpers
cuda/nn/safetensors.cpp
```

**DTO API sketch:**

```text
struct SafetensorsTensor {
  std::string name;
  enum class Dtype { F32 /* extend later */ } dtype;
  std::vector<std::int64_t> shape;
  std::vector<float> data;
  [[nodiscard]] auto numel() const -> std::size_t;
};

class SafetensorsTensorMap {
 public:
  [[nodiscard]] auto contains(std::string_view key) const -> bool;
  [[nodiscard]] auto at(std::string_view key) const -> const SafetensorsTensor&;
  // iteration over entries...
};

[[nodiscard]] auto LoadSafetensors(const std::filesystem::path& path)
    -> SafetensorsTensorMap;

[[nodiscard]] auto RequireF32Tensor(const SafetensorsTensorMap& map,
                                    std::string_view key,
                                    std::initializer_list<std::int64_t> shape)
    -> const SafetensorsTensor&;
```

**Tests:**

- Load both real files; assert keys needed by §2.1 / §2.2 **exist** with expected
  shapes (table-driven checks—still not “building” a network).
- Reject truncated file / bad header / non-F32.
- `RequireF32Tensor` fails on wrong shape.
- Optional: H2D upload helper bit-identical round-trip for one tensor.

**Exit:** any caller can obtain a unified DTO from disk without domain
knowledge. `MlOpsTest` green.

**Non-goals for Phase 4:**

- Model forward / hard-coded demosaic modules.
- CRTP base (Phase 5).
- Lazy model cache.
- Runtime graph assembly (forever non-goal).
- GpuMat demosaic / pipeline scratch migration.

### Phase 5 — Hard-coded modules (CRTP LoadWeights) + lazy cache  ✅ (done)

**Scope boundary:** DemosaicNet as a **RAW-owned, hard-coded demosaic NN**.
Uses Phase 1–4 ops + DTO. Product GpuMat wiring landed in Phase 6a; this phase
owns the modules, cache, and golden / shape contracts.

**Done:**

1. **CRTP base** `NnWeightModule<Derived>` — `include/decoders/processor/nn/demosaicnet_module.hpp`
   - Public `LoadWeights(const SafetensorsTensorMap&, cudaStream_t)`.
   - Dispatches to `Derived::LoadWeightsImpl`.
   - Shared `RequireUpload` helper (`RequireF32Tensor` + `UploadToDevice`).
2. **`BayerDemosaicNet` / `XTransDemosaicNet`**
   - Fixed `DeviceBufferF32` weight slots; `LoadWeightsImpl` maps §2.1 / §2.2 keys.
   - Straight-line `Forward` (Conv2dBiasRelu / Mul / Split / Concat / Crop /
     ConvTranspose); caller-owned `WorkspacePool`.
   - `EstimateWorkspaceBytes(H,W,N)` + spatial helpers (`H_out = H - 62` Bayer,
     `H - 24` XTrans).
3. **`DemosaicNetModelCache`** — lazy, mutex, cold by default; independent
   Bayer/XTrans load; `Unload` for tests; model dir via options / env /
   `ALCEDO_DEMOASICNET_MODEL_DIR` / source defaults.
4. **CMake target `DemosaicNet`** (links `CudaUtils` only). Consumer is the
   RawProcessor CUDA backend (`raw_processor_cuda.cpp` / `cuda_demosaicnet.*`),
   **not** `RawProcessorOp`. Headers under `include/decoders/processor/nn/`.
5. **Tests** in `tests/ml_ops/demosaicnet_module_test.cu` + PyTorch goldens under
   `tests/ml_ops/goldens/` (all Phase 5 filters green).

**Exit (met):** hard-coded Bayer/XTrans match PyTorch reference (≤1e-4); weights
enter only via DTO + CRTP `LoadWeights`; cache is lazy and RAW-scoped; **no**
runtime graph builder.

### Phase 6 — RAW product entry + Neural preprocess + tiling (redesigned)

Phase 6 has three layers. **6a + 6b are done.** Active work is **6c**:

| Layer | Status | Focus |
|-------|--------|--------|
| **6a** | ✅ | GpuMat entry, Method selection, lazy load, Legacy fallback |
| **6b** | ✅ | Neural Engine **preprocess**: CFA phase-align + gamma 1/2.2 in / 2.2 out |
| **6c** | ⏳ **next** | Tiling via existing CUDA tile path; Neural Engine defaults to tiled |

Do **not** invent a second tile scheduler inside `cuda/nn`. Preprocess lives on
the RAW Neural path (around `DemosaicWithNeuralEngine` / `raw_processor_cuda`),
not in generic `cuda/nn` ops.

#### 6a — GpuMat entry + product selection + lazy trigger  ✅ (landed)

**Goals (met):**

- GpuMat CFA → RGB entry for the RAW demosaic stage
  (`cuda_demosaicnet.hpp` / `.cu` → `DemosaicWithNeuralEngine`).
- Product names: **Legacy** and **Neural Engine**
  (`RawDemosaicMethod { Default, Legacy, NeuralEngine }`).
- `Default` → Bayer Legacy, X-Trans Neural Engine.
- Any decode resolution other than `FULL` → Legacy (thumbnails never NN).
- Load only when the NN path is actually taken; load failure → Legacy.
- Wired into `raw_processor_cuda.cpp` only (not `RawProcessorOp`).
- Odd Bayer dims: drop trailing row/column to keep 2×2 pack phase.

**Existing real-RAW tests (patch-level, purpose-named — not “smoke”):**

| Test | Purpose | Fixture |
|------|---------|---------|
| `NeuralEngineDemosaicsRealBayerRawPatchToValidRgb` | Real Bayer CFA 64×64 patch → CV_32FC3 valid-conv size | `raw/camera/nikon/d800e/Nikon-D800e-raw-00002.nef` |
| `RawProcessorDefaultXTransLoadsNeuralEngineOnRealRawPatch` | Default X-Trans on real RAF patch loads cache + CUDA RGBA | `raw/camera/fuji/xt5/DSCF2074.RAF` |
| `RawProcessorNeuralLoadFailureFallsBackToLegacyAndKeepsCacheCold` | Missing model dir → Legacy path, cache stays cold | same Nikon NEF patch |
| Synthetic shape / cache-reuse tests in `cuda_raw_ops_test.cpp` | border math, weight pointer reuse, load failure | synthetic GpuMat |

**Gap after 6a:**

1. No **CFA phase-align** to the model’s training origin — pack uses camera
   phase as-is; pretrained weights expect fixed origin (GRBG / X-Trans 6×6).
2. No **gamma encode/decode** around the network — model was trained on
   sRGB-ish (γ≈2.2) mosaics; pipeline CFA after `ToLinearRef` is linear.
3. Tests are **small patches**, not full camera frames.
4. Neural Engine **forces FullFrame**; `ProcessCudaTiled` never calls NN.

#### 6b — Neural Engine preprocess (CFA phase-align + gamma)  ✅ (done)

##### Python reference tree (read this first)

External repo (not a submodule of pu-erh_lab). On the author’s machine:

```text
D:\Projects\deepjoint_demosiacing\demosaicnet_caffe\
```

| Path | What to copy / match |
|------|----------------------|
| `...\pytorch\raw_pipeline.py` | **Primary product pipeline reference**: CFA phase-align (`_find_align_shift`, `BAYER_TARGET` GRBG, `XTRANS_TARGET`), black/normalize, as-shot WB (→ Alcedo `ToLinearRef`), **gamma `1/2.2` in / `2.2` out**, pad + tiled demosaic, then camera→sRGB/filmic (downstream of demosaic in Alcedo) |
| `...\pytorch\infer.py` | Mosaic pack (`_make_mosaic` / `_bayer_mosaic` / `_xtrans_mosaic`), **tile loop** (`_run_tiles`, `_model_crop`), noise-model hooks |
| `...\pytorch\model.py` | PyTorch DemosaicNet topology (already mirrored in hard-coded C++ modules) |
| `...\pytorch\raw_color.py` | Camera→sRGB / filmic helpers (Alcedo already has color path after demosaic; do not re-port unless comparing outputs) |
| `...\demosaicnet\layers.py` | Original layer definitions (X-Trans 6×6 masks, Bayer pack conventions) |
| `...\demosaicnet\models.py` | Original network defs |
| `...\pytorch\bayer.pt` / `xtrans.pt` | PyTorch weights (Alcedo ships `config/models/*.safetensors` converted from these) |

Upstream public project (architecture only; Alcedo weights + local caffe port above are the practical refs):

- https://github.com/mgharbi/demosaicnet
- https://groups.csail.mit.edu/graphics/demosaicnet/

Alcedo maps `raw_pipeline.py` onto the **existing** CUDA RAW order; it does **not**
re-implement white balance (`ToLinearRef` already did it).

##### Neural path data order (product)

```text
upload / downsample
    → ToLinearRef          # black subtract, normalize, as-shot WB  ← already done
    → CFA phase-align      # crop (sy,sx) so origin matches training CFA   [6b]
    → gamma encode         # x^(1/2.2) on linear CFA                       [6b]
    → (tile loop)          # 6c: copy halo tile / reflect-pad as needed
    → PackCfaMosaic + DemosaicNet Forward
    → gamma decode         # x^2.2 → linear camera RGB                     [6b]
    → active/border crop, inverse cam mul, pack RGBA, geo  (unchanged after)
```

**Explicitly out of 6b:**

- White balance / `cam_mul` gains on the mosaic — already applied inside
  `ToLinearRef`. Do **not** multiply CFA by WB again (reference Python does WB
  before gamma only because it lacks Alcedo’s `ToLinearRef`).
- Camera→sRGB / ACES filmic / EXIF orientation — stay downstream of demosaic
  (existing pack + edit pipeline).
- Unconditional range clamp of CFA/RGB when **highlight reconstruction is
  enabled** — see HLR / clamp policy below.

##### CFA phase-align

Training origins (from demosaicnet / `raw_pipeline.py`):

| Variant | Period | Origin pattern at (0,0) |
|---------|--------|-------------------------|
| Bayer | 2 | **GRBG** — `[[G,R],[B,G]]` (rawpy indices 1,0 / 2,1) |
| X-Trans | 6 | Fixed 6×6 from `XTransMosaickLayer` / `XTRANS_TARGET` |

Algorithm (host, once per decode; same as `_find_align_shift`):

1. Read the camera CFA phase from `RawCfaPattern` (already on `RawProcessor`).
2. Search cyclic shifts `(sy, sx)` in `[0, period)` such that cropping the CFA by
   that shift makes the origin match the training target (compare R/G/B only;
   collapse dual-green if needed).
3. If no pure cyclic shift matches (rotated/reflected CFA) → **fail soft to
   Legacy** (same policy as load failure); log the reason.
4. Crop the linear CFA GpuMat to `[sy:, sx:]`, then trim to a multiple of
   `period` on both axes. Update the effective `RawCfaPattern` so that after
   crop, origin is the training pattern (or pass a fixed “model pattern” into
   pack and stop using the unaligned camera phase).
5. Geometry bookkeeping: phase crop shifts the active-area / default-crop
   origin by `(sx, sy)`. Fold into existing crop helpers
   (`BuildBorderLossDecodeCropRect` / decode crop) so output framing stays
   consistent with Legacy for the same sensor crop (minus NN border loss).

Tile path (6c) runs **after** global phase-align: tile origins are relative to
the aligned CFA, so Bayer even / X-Trans mod-6 alignment is w.r.t. the model
origin. `ShiftBayerPattern` / X-Trans equivalent still applies per tile for
pack phase when the tile’s top-left is not (0,0) of the aligned image—or pack
with the fixed model pattern and absolute coordinates mod period.

##### Gamma encode / decode

Model expects approximately display-encoded mosaics; Alcedo’s post-`ToLinearRef`
CFA is scene-linear (WB’d).

| Step | Op | Domain |
|------|-----|--------|
| Encode (before pack/forward) | `y = sign-preserving x^(1/2.2)` | per CFA sample, single-channel |
| Decode (after forward, on RGB) | `x = sign-preserving y^2.2` | per RGB channel |

**When HLR is enabled, values above 1.0 must pass through the power curve
without being saturated to 1** (only the gamma is applied). Highlights after WB
often exceed 1.0; they must still be present after demosaic for highlight
reconstruction.

Suggested signed power (does not saturate to `[0,1]` by itself):

```text
pow_signed(x, g) = copysign(pow(abs(x), g), x)   # abs(x)==0 → 0
```

Implementation notes:

- Small CUDA elementwise kernel on GpuMat; stream-aware; no host round-trip.
- Apply encode **after** phase-align crop, **before** mosaic pack / tiles.
- Apply decode **after** NN RGB output, **before** inverse cam mul / pack RGBA
  **and before** highlight reconstruction (when HLR is on).
- Do **not** put gamma into the hard-coded network `Forward` itself—keep it as
  demosaic-stage sandwich so goldens that feed already-encoded mosaics stay
  valid (module tests keep raw mosaic→RGB; product path owns the sandwich).
- Gamma kernels themselves must **not** hard-clip to `[0,1]`; product-level
  `Clamp01` is a separate, intentional policy gated by HLR (below).

##### `Clamp01` vs highlight reconstruction (intentional product policy)

This is **not** debt. Match the existing full-frame Neural / Legacy pattern:

| `highlights_reconstruct_` | Pre-demosaic `Clamp01` on CFA | Rationale |
|---------------------------|-------------------------------|-----------|
| **true** (HLR on) | **Skip** | HLR needs over-range samples (`> 1` after WB) |
| **false** (HLR off) | **Apply** `Clamp01` | Expected when reconstruct is disabled |

Same rule for full-frame and tiled Neural branches. Keep today’s Neural full-frame
behavior:

```cpp
if (!params_.highlights_reconstruct_) {
  CUDA::Clamp01(gpu_img, &stream);
}
// then phase-align / gamma / DemosaicWithNeuralEngine ...
```

**Still forbidden when HLR is on:**

- `Clamp01` (or equivalent) on CFA before demosaic
- `Clamp01` / saturate on RGB after demosaic before highlight stats / correction
- Gamma encode/decode that internally saturates to `[0,1]` (would defeat HLR
  even if the product clamp is skipped)
- Reference-style `np.clip(R, 0.0, 1.0)` after the network on the HLR-on path

**When HLR is off:** product `Clamp01` is required/expected; do not “fix” it.

##### Where the code lives

Suggested placement (names flexible):

```text
include/decoders/processor/nn/demosaicnet_preprocess.hpp
decoders/processor/nn/demosaicnet_preprocess.cu   # or .cpp + small .cu
  - FindCfaAlignShift(pattern) → optional (sy,sx)
  - ApplyCfaPhaseAlign(GpuMat& cfa, pattern, sy, sx) → updated pattern / sizes
  - GammaEncodeCfa / GammaDecodeRgb (GpuMat, stream)
```

Call sites: `ProcessCudaFullFrame` Neural branch and (later) `ProcessCudaTiled`
Neural branch — **one helper** used by both, e.g.
`PrepareNeuralEngineCfa(...)` / `FinishNeuralEngineRgb(...)`.

##### 6b tests (purpose-named)

| Test (suggested name) | Asserts |
|----------------------|---------|
| `FindCfaAlignShift_MatchesGrbgOriginForCommonBayerPhases` | All four Bayer phases map to a unique (sy,sx) yielding GRBG |
| `FindCfaAlignShift_MatchesXTransTrainingOrigin` | Real Fuji pattern (or synthetic X-Trans) aligns to target 6×6 |
| `FindCfaAlignShift_UnsupportedRotationFailsSoft` | Impossible CFA → nullopt / Legacy path |
| `NeuralEngineGammaEncodeDecode_RoundTripsLinearRgb` | encode→decode ≈ identity on positive linear patches, **including values > 1** (gamma alone does not saturate) |
| `NeuralEngineGammaEncodeDecode_PreservesOverRangeHighlights` | input 1.5 / 2.0 / 4.0 stay > 1 through encode→decode (no hard clip inside gamma) |
| `NeuralEnginePath_SkipsClamp01WhenHighlightsReconstruct` | Neural + HLR on: over-range CFA/RGB can exceed 1 after preprocess/demosaic |
| `NeuralEnginePath_AppliesClamp01WhenHighlightsReconstructOff` | Neural + HLR off: CFA is clamped to `[0,1]` before demosaic (expected product behavior) |
| `NeuralEnginePhaseAlignAndGamma_OnRealBayerRawPatch` | Real NEF patch: after `ToLinearRef` + align + γ-in → NN → γ-out produces finite CV_32FC3; origin pattern is GRBG |
| `NeuralEnginePhaseAlignAndGamma_OnRealXTransRawPatch` | Same for Fuji RAF / X-Trans |

Fixtures:
`tests/resources/sample_images/raw/camera/nikon/...`, `.../fuji/xt5/...`.

##### 6b exit

- [x] Phase-align implemented; training origin documented in code
      (`demosaicnet_preprocess.hpp`: GRBG + X-Trans 6×6 targets, `FindCfaAlignShift`).
- [x] Gamma 1/2.2 in / 2.2 out on the Neural path only (Legacy unchanged).
- [x] WB **not** reapplied; depends on `ToLinearRef` only.
- [x] `Clamp01` only when `highlights_reconstruct_ == false`; HLR-on path
      preserves over-range (gamma must not hard-clip to 1).
- [x] Full-frame Neural path uses preprocess (tiling not required to close 6b).
- [x] Purpose-named unit + real-RAW patch tests green.

#### 6c — Neural Engine tiling via existing CUDA tile path  ⏳ (after 6b)

> Historical teacher contract: this section describes the Phase 6c path that
> landed before the student exports replaced the bundled weights. Phase 8 keeps
> the same `BuildTileJobs` / `ProcessCudaTiled` ownership but supersedes the
> `inner_size + halo` policy with the student input/output/pad/step contract.
> In particular, do not carry `halo=31` into Bayer student full-frame padding or
> `step=1024` into X-Trans student tiling.

**Design rule:** extend what already works for high-res RCD; do not redesign
tiling from scratch. Preprocess (6b) is **global** before the tile loop (or
applied consistently per tile if that proves simpler—prefer once globally so
phase crop is shared).

##### Existing infrastructure (reuse)

| Piece | Location | Role today |
|-------|----------|------------|
| `CudaExecutionMode::{FullFrame,Tiled}` | `raw_processor_internal.hpp` | Mode enum |
| `SelectCudaExecutionMode` | `raw_processor.cpp` | Routes Legacy Bayer by long-edge > `kCudaTileThresholdLongEdge` (9000); **Neural Engine forced FullFrame** |
| `kCudaTileInnerSize` / `kCudaTileHaloSize` | same header | 1024 inner, 16 halo (RCD-sized) |
| `CudaTileJob` + `BuildTileJobs` | `raw_processor_cuda.cpp` | Grid of `source_rect` / `inner_rect_in_tile` / `output_rect` |
| `ShiftBayerPattern` | same | CFA phase at tile origin |
| `ProcessCudaTiled` | same | Upload → downsample → `ToLinearRef` → per-tile RCD → assemble → geo → pack |
| `SetCudaExecutionModeOverrideForTesting` | `raw_processor.cpp` | Force mode in tests |
| `DemosaicWithNeuralEngine` | `cuda_demosaicnet.cu` | Single CFA GpuMat → RGB (valid-conv shrink) |

##### Routing policy (product)

| Condition | Mode |
|-----------|------|
| Resolved method **Neural Engine** + CUDA + `FULL` | **Always `Tiled`** — not long-edge routing |
| Resolved method **Legacy** + Bayer + long edge > 9000 | `Tiled` (unchanged) |
| Legacy + smaller / non-Bayer / non-CUDA | `FullFrame` (unchanged) |
| Neural Engine + non-`FULL` | Never selected (already forced Legacy in resolve) |

Rationale: NN activation cost is high even below 9K long edge; tile-by-default
is the product default when the user (or X-Trans Default) picks Neural Engine.

##### Halo / geometry (document in code + tests)

Valid-convolution spatial loss (already on the modules):

- Bayer: `kSpatialLoss = 62` → **border = 31**
- X-Trans: `kSpatialLoss = 24` → **border = 12**
- RCD Legacy: halo 16 + crop radius 4 (keep as-is)

**Do not** reuse `kCudaTileHaloSize = 16` for Neural Engine. Parameterize the
job builder:

```text
BuildTileJobs(active_rect, full_size, inner_size, halo)
```

Suggested defaults:

- `inner_size`: keep **1024** (match existing CUDA tiles; profile later in Phase 8).
- `halo` for Neural Engine: **`source_border`** (31 Bayer / 12 X-Trans) so a full
  halo tile’s NN output size equals the inner rect (no second crop math).
- Edge tiles: prefer **reflect-pad** the (already phase-aligned, gamma-encoded)
  CFA tile to a full halo before `DemosaicWithNeuralEngine` (matches
  demosaicnet `_run_tiles` / `raw_pipeline.py`).

CFA constraints after 6b global align:

- Bayer: even source dims relative to aligned origin.
- X-Trans: period-6 relative to aligned origin; unlock X-Trans for Neural
  Engine tiled (Default X-Trans is Neural Engine).

##### Implementation shape (recommended)

1. Land **6b preprocess** on full-frame Neural first (correctness without tiles).
2. **Generalize** `BuildTileJobs` → `inner_size` + `halo`.
3. **`SelectCudaExecutionMode`:** Neural Engine → `Tiled` (drop FullFrame force);
   allow X-Trans + Neural into tiled mode.
4. **`ProcessCudaTiled` Neural branch:**
   `ToLinearRef` → **phase-align + γ-encode (once)** → tile loop
   (copy/pad → `DemosaicWithNeuralEngine` with model-phase pattern;
   pre-demosaic `Clamp01` only if HLR off, same as full-frame) → assemble
   → **γ-decode once on full RGB** (or per-tile before assemble; once is
   cheaper; gamma itself never saturates to 1) → HLR / inverse cam mul / pack /
   geo.
5. **Workspace:** one `WorkspacePool` per tiled pass; `Reserve` once; reset
   between tiles; zero `cudaMalloc` after reserve.
6. Keep full-frame Neural (with 6b preprocess) for tests / override / tiny frames.
7. Still no app-boot weight load.

##### 6c tests (purpose-named; real fixtures under `raw/camera/`)

| Test (suggested name) | Asserts |
|----------------------|---------|
| `SelectCudaExecutionMode_NeuralEngineDefaultsToTiled` | Neural Engine → `Tiled` regardless of long edge |
| `SelectCudaExecutionMode_LegacyStillUsesLongEdgeThreshold` | Legacy Bayer still thresholds at 9000 |
| `ProcessCudaTiled_NeuralEngineBayerAssemblesActiveAreaFromRealRaw` | Real Bayer RAW active area → CUDA RGBA; cache loaded once; preprocess applied |
| `ProcessCudaTiled_NeuralEngineMatchesFullFrameOnOverlappingTiles` | Tiled vs full-frame interior agreement (both with 6b preprocess) |
| `ProcessCudaTiled_NeuralEngineXTransRealRawLoadsAndAssembles` | Real Fuji RAF tiled Neural path |
| `ProcessCudaTiled_NeuralEngineReservesWorkspaceOncePerPass` | Pool capacity stable across tiles |

Fixtures: `tests/resources/sample_images/raw/camera/...`. Heap-allocate LibRaw.

##### Exit criteria for Phase 6 (all of 6a–6c)

- [x] GpuMat API + Method selection + lazy load + Legacy fallback (6a).
- [x] CFA phase-align to training origin; unsupported CFA → Legacy (6b).
- [x] Gamma 1/2.2 in / 2.2 out on Neural path only; **no** second WB (6b).
- [x] Neural `Clamp01` gated by HLR: skip when reconstruct on, apply when off (6b).
- [x] Neural Engine **defaults to tiled** CUDA execution (6c).
- [x] `ProcessCudaTiled` runs Neural Engine with RF-correct halo; Bayer + X-Trans (6c).
- [x] At least one **full active-area** real Bayer RAW and one real X-Trans RAW
      test (purpose-named) green on CUDA (6c; preprocess required).
- [x] Tiled vs full-frame agreement on an interior region (both with preprocess).
- [x] Steady-state: workspace reserved once per decode; no per-tile
      `cudaMalloc` after reserve.
- [x] Thumbnails / non-`FULL` never enter NN or NN workspace.

### Phase 7 — Workspace policy + parallel readiness

Only after Phase 6 preprocess + tiling correctness:

1. Steady-state polish: owned VRAM counters (`ResidentWeightBytes`, pool
   `capacity_bytes` / `used_bytes`) in debug logs; `cudaMemGetInfo` residual only.
2. Optional: small free-list of workspaces for concurrent NN (mutex on
   borrow/return only)—**not** on the model cache hot path.
3. Confirm multi-image concurrent decode does not share one `WorkspacePool`
   across threads.
4. Confirm thumbnails never touch NN workspace (regression guard).

**Exit:** concurrent NN safe by construction; observability is owned-bytes first.

### Phase 8 — Student models + CFA-safe reuse of the CUDA tile path

Phase 8 replaces the old teacher modules with the two bundled student models,
then measures and optimizes the resulting full active-area path. This is not a
second NN tile scheduler: extend the existing `CudaTileJob` / `BuildTileJobs` /
`ProcessCudaTiled` seam with a model-specific policy and keep one tile loop.

Correct CFA phase and output geometry are gates. In particular, the model's
valid/output crop, the period-aligned virtual reflect pad, the global CFA-align
crop, and the final sensor/default crop are four different coordinate changes;
they must not be collapsed into one `source_border` integer. Throughput work
starts only after both student forwards match their exported goldens and the
tiled assembly matches the handoff contract without seams or uncovered pixels.

#### 8.0 Performance contract

##### Primary fixtures

| Variant | Required real RAW fixture | Historical teacher Release tile assembly (2026-07-11) |
|---------|---------------------------|----------------------------------------------------|
| Bayer | `raw/camera/nikon/d800e/Nikon-D800e-raw-00002.nef` | ~2.83 s |
| X-Trans | `raw/camera/fuji/xt5/DSCF2074.RAF` | ~9.52 s |

The benchmark must also support additional Bayer and X-Trans fixtures, but the
two rows above are the stable comparison pair and may not be silently replaced
when numbers regress.

##### Timed scope

The primary `full_process_hot_ms` interval is:

```text
already-unpacked LibRaw CFA
  -> RawProcessor CUDA FULL path
  -> ToLinearRef / Neural preprocess or Legacy equivalent
  -> full active-area demosaic
  -> RGB tile assembly
  -> gamma decode (Neural only)
  -> identical HLR / inverse cam-mul / RGBA pack configuration
  -> completed CUDA output
```

File IO and `LibRaw::unpack()` are outside the interval. Model parsing and first
weight upload are reported separately as `cold_load_ms`; they are outside the
hot-path SLO because weights are process-lifetime cached. The timed hot pass
must include all method-specific preprocessing and synchronization needed to
hand a completed full image to the next pipeline stage.

##### Performance objectives and non-negotiable correctness gates

For both primary fixtures, on the declared reference GPU/driver:

1. The primary optimization score is `neural_p50_ms / legacy_p50_ms`, followed
   by the p95 ratio. The target is <=1.10; a miss does not by itself block
   release, but the measured gap and roofline explanation must be recorded.
2. Report absolute p50/p95 and progress toward the 100 ms stretch target; do not
   turn 100 ms into a flaky CI assertion.
3. Use at least 20 measured runs and report variance/outliers rather than
   selecting the fastest run.
4. Output correctness remains within the existing frozen FP32/golden tolerances;
   a precision change needs its own explicitly approved tolerance and quality
   evidence.
5. No device allocation grows during measured iterations after warm-up.
6. The output dimensions, active-area mapping, CFA phase, gamma sandwich, HLR
   behavior, and lazy model-cache behavior remain unchanged.

Legacy and Neural must be run in the same process, alternating method order per
iteration to reduce clock/temperature bias. The benchmark records GPU name,
compute capability, driver/runtime versions, power state where available,
fixture dimensions, active pixels, tile count, tile size, lane count, and git
commit. `Legacy` means the traditional demosaic selected for that exact Bayer
or X-Trans RAW through the same `RawProcessor` entrypoint and with the same
post-demosaic output configuration; it is not a synthetic Conv-only baseline.

#### 8.1 Dedicated benchmark harness (land first) ✅

Add a standalone CUDA performance executable rather than putting hard timing
assertions in the ordinary correctness suites:

```text
alcedo_studio/tests/perf/demosaicnet_perf_harness.cpp
alcedo_studio/tests/perf/demosaicnet_perf_metrics.hpp
alcedo_studio/tests/perf/demosaicnet_perf_metrics.cpp
CMake target: DemosaicNetPerfHarness
```

The harness links `RawProcessor`, `CudaDemosaicNetEntry`, `DemosaicNet`,
`CudaUtils`, LibRaw, OpenCV CUDA, and `CUDA::cudart`. Do not add Google Benchmark
or another runtime dependency: the harness needs deterministic control over
warm-up, streams, fixtures, and JSON output. It is built in Release only for
authoritative numbers.

Required CLI:

```bat
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture bayer|xtrans|all ^
  --method legacy|neural|both ^
  --mode full|tile|conv ^
  --warmup 3 --iterations 20 ^
  --tile-size 1024 --lanes 1 ^
  --output build/perf/demosaicnet.json
```

Required modes:

| Mode | Purpose | Primary measurement |
|------|---------|---------------------|
| `full` | Primary product measurement; full active-area real RAW | wall-clock + CUDA completion, Neural vs Legacy |
| `tile` | Fixed-shape runner diagnosis | Bayer input 1086² -> output 1024²; X-Trans input 1048² -> output 1024² |
| `conv` | Kernel tuning without RAW setup | exact layer shapes from both hard-coded modules |

Harness rules:

1. Open/unpack the RAW once, outside timed iterations.
2. Run one correctness pass before timing; abort the benchmark on shape,
   finite-value, CFA-phase, or reference mismatch.
3. Warm the model cache, CUDA context, workspaces, GpuMat buffers, and graph
   objects before measured iterations.
4. Use CUDA events for device-stage intervals and `steady_clock` for the full
   product interval. Do not insert per-layer synchronizations in normal timing
   mode.
5. Report min, median, p90, p95, max, standard deviation, active megapixels per
   second, tiles per second, and Neural/Legacy ratios.
6. Emit machine-readable JSON plus a compact console table. Performance
   artifacts live under `build/perf/` and are not committed.
7. Add owned allocation-generation counters to `NeuralDemosaicWorkspace`; the
   generation must remain constant throughout measured iterations. Continue to
   treat `cudaMemGetInfo` as residual observability, not an ownership counter.
8. Provide `--profile-ranges` to add named ranges for preprocess, pack,
   each hard-coded layer, structural ops, unpack, assemble, and postprocess.
   CUDA-event timing must work even when Nsight performance-counter permission
   is unavailable.

The harness is not part of default `ctest`. Add an opt-in CMake option such as
`ALCEDO_ENABLE_GPU_PERF_TESTS`; dedicated benchmark machines may register the
full-mode command with the `performance` label and alert on regressions against
the recorded Neural/Legacy ratios. Timing is not part of default correctness CI.

Build command:

```bat
cmd /c scripts\msvc_env.cmd --build --preset win_release --target DemosaicNetPerfHarness --parallel 4
```

#### 8.2 Authoritative student contracts (land first)

The source of truth is the checked-in handoff, not the old teacher constants:

- `alcedo_studio/src/config/models/student_handoff/bayer_architecture.md`
- `alcedo_studio/src/config/models/student_handoff/xtrans_architecture.md`
- the matching checked-in export reports
- external exported goldens under
  `D:/Projects/deepjoint_demosiacing/demosaicnet_caffe/pytorch/handoff/golden/`

The current bundled `bayer.safetensors` and `xtrans.safetensors` SHA-256 values
match those export reports. Validate the full metadata identity at load
(`architecture`, version, CFA period, pack factor, tile input/output/border/pad/
step, checkpoint hash); do not construct a runtime graph from it.

The existing checked-in `bayer_64` / `xtrans_48` goldens belong to the teacher
modules and are not student proof. In 8A, import or deterministically convert the
student 1086→1024 and 1048→1024 `.pt` fixtures into the test-data format (or an
explicit external-data test target if repository size policy rejects them), and
verify every file/manifest SHA-256 from the export reports.

| Contract | Bayer student | X-Trans student |
|----------|---------------|-----------------|
| Architecture | `bayer_s24_d8` | `xtrans_p2_s32_d4` |
| Width / trunk depth | 24 / 8 | 32 / 4 |
| Fixed pack | collapse-color 2×2/s2, 3→4 | space-to-depth 2×2/s2, 3→12 |
| Trunk | 8 × valid 3×3 + ReLU | 4 × valid 3×3 + ReLU |
| Residual / unpack | 1×1 → 12; grouped 2×2/s2 transpose → RGB | same |
| Tail | concat cropped mosaic; 6→width valid 3×3 + ReLU; 1×1 → RGB | same |
| Tile input → natural → export | 1086 → 1052 → center-crop 1024 | 1048 → 1030 → center-crop 1024 |
| CFA period | 2 | 6 |
| Effective exported border | 31 | 12 |
| Period-safe virtual pad | **32** | **12** |
| Grid step | **1024** | **1020** |

Non-negotiable consequences:

1. Bayer `pad=31` is forbidden. The border is 31, but the leading virtual pad
   is 32 so the GRBG model origin remains phase aligned.
2. X-Trans cannot use a 1024 grid step: `1024 % 6 == 4`. Use step 1020; its
   1024-wide outputs overlap by four pixels.
3. `ShiftRawCfaPattern` is not a repair for an off-period student tile. It only
   changes channel placement; the fixed pack and learned convolutions still see
   the wrong spatial phase. Every tile input origin must satisfy
   `origin.x % period == 0 && origin.y % period == 0` in the aligned/padded CFA
   lattice, and the model receives the fixed training pattern.
4. Fixed pack/unpack weights may be uploaded from the file or compiled as
   constants, but must be checked against the exported one-hot tensors.

**Exit:** both hard-coded student modules load the bundled weights and match all
exported FP32 golden tensors before product tiling is changed.

#### 8.3 Reuse and generalize the existing tile planner

Keep `BuildTileJobs` as the only grid builder. Add a policy/config overload (the
Legacy overload delegates to it) rather than adding a Neural-only scheduler:

```cpp
struct CudaTilePolicy {
  cv::Size input_tile;     // student: 1086² / 1048²
  cv::Size output_tile;    // 1024²
  cv::Size step;           // 1024² / 1020²
  cv::Point virtual_pad;   // 32,32 / 12,12
  cv::Point output_border; // 31,31 / 12,12
  int cfa_period;          // 2 / 6
};

struct CudaTileJob {
  cv::Point input_origin;       // signed, in aligned CFA coordinates
  cv::Rect model_output_roi;    // disjoint owned part of the 1024² result
  cv::Rect destination_roi;     // same extent, in assembled aligned RGB
};
```

The existing Legacy `source_rect / inner_rect_in_tile / output_rect` semantics
can remain as fields or be expressed by the same generalized job. Do not fork
the loop or duplicate edge handling.

##### Virtual padding: no full padded CFA allocation

After `PrepareNeuralEngineCfa`, treat the aligned CFA as if it had symmetric
reflect padding, but do not materialize that full image. For grid coordinate
`g=(gx,gy)`:

```text
input_origin_in_aligned = g * step - virtual_pad
model_output_origin     = input_origin_in_aligned + output_border
```

Do **not** implement this by reflecting the scalar CFA and then assigning RGB
channels from the reflected tile's destination coordinates. The handoff
reference reflects an already packed three-channel sparse mosaic; at a reflected
edge, the non-zero channel belongs to the reflected **source** coordinate. This
distinction is observable for the X-Trans student's color-preserving
space-to-depth pack.

Add a RAW-domain fused primitive such as
`PackReflectPaddedCfaTile(aligned_cfa, signed_origin, training_pattern,
input_tensor, stream)`. For each tile pixel it:

1. maps the signed aligned coordinate with OpenCV/NumPy-compatible reflect
   semantics;
2. reads the scalar CFA sample at that reflected source coordinate;
3. obtains the RGB plane from the reflected source coordinate's CFA phase;
4. writes that value to the matching NCHW plane and zeros the other two.

This is exactly “pack full sparse mosaic, reflect-pad, then slice,” without
allocating either the full 3-channel mosaic or a scalar padded frame. Interior
tiles take the same kernel fast path without reflection. Because `virtual_pad`
and `step` are multiples of the CFA period, every model input origin remains at
the training phase. Add hard assertions for this.

This mapping gives Bayer's first model output origin `-32 + 31 = -1` (clip one
top/left pixel) and X-Trans's `-12 + 12 = 0`. It restores boundary context
without turning the student border into a permanent full-frame size loss.

##### Deterministic overlap ownership

Do not allocate a per-pixel ownership mask in the product path. Precompute
disjoint ROIs in `BuildTileJobs`:

- clip each 1024² model output against the aligned destination;
- for any non-first X/Y grid position, discard the leading
  `output_tile - step` pixels on that axis;
- therefore X-Trans later tiles discard four leading rows/columns and implement
  handoff-compatible first-writer ownership; Bayer has no inter-tile overlap;
- reject a plan with uncovered pixels, overlapping destination ROIs, or a tile
  origin that is not period aligned.

Copy only `tile_rgb(model_output_roi)` to `output_rgb(destination_roi)`. Edge
tiles still run the same fixed input/output shapes, so workspace capacity and a
future CUDA Graph remain stable.

Tests (pure planner tests do not require CUDA):

- `BuildTileJobs_BayerStudentUsesPad32AndPeriodAlignedOrigins`
- `BuildTileJobs_XTransStudentUsesStep1020AndPeriodAlignedOrigins`
- `BuildTileJobs_XTransStudentAssignsOverlapToFirstWriter`
- `BuildTileJobs_StudentDestinationRoisCoverEveryPixelExactlyOnce`
- `PackReflectPaddedCfaTile_BayerMatchesPackThenReflectReference`
- `PackReflectPaddedCfaTile_XTransMatchesPackThenReflectReferenceAtAllEdges`

**Exit:** one planner produces unchanged Legacy jobs plus phase-safe Bayer and
X-Trans student jobs, with exact ownership coverage proved in tests.

#### 8.4 Student forward + product assembly

Implement the two student modules as fixed C++ topologies. Prefer new explicit
student classes (or a deliberate rename of the existing classes) over silently
changing constants while comments/tests still describe the teachers. The
runtime graph remains hard-coded; safetensors is only the weight DTO.

`ProcessCudaTiled` remains the product owner of preprocessing, tile iteration,
assembly, gamma decode, sensor crop, HLR/color, orientation, and RGBA packing:

```text
ToLinearRef
  -> optional Clamp01
  -> PrepareNeuralEngineCfa once          # global phase crop + period trim + gamma encode
  -> BuildTileJobs(student policy)        # existing planner, virtual pad
  -> for each job:
       PackReflectPaddedCfaTile(signed origin -> fixed NCHW input tensor)
       student Forward(fixed workspace)
       copy owned model_output_roi -> destination_roi
  -> FinishNeuralEngineRgb once
  -> map/crop to requested sensor rectangle once
  -> HLR / inverse cam-mul / orientation / RGBA once
```

##### Geometry contract

Replace `BuildNeuralEngineDecodeCropRect(..., source_border, shift_x, shift_y)`
with an explicit coordinate description, for example:

```cpp
struct NeuralOutputGeometry {
  cv::Point aligned_origin_in_original;  // (phase sx, phase sy), after downsample units
  cv::Point output_origin_in_aligned;    // student tiled path: (0,0)
  cv::Size output_size;                  // assembled aligned RGB size
};
```

The crop helper maps the requested active/default crop from original CFA space
through that transform and intersects it with `output_size`. For the student
tiled path, virtual padding restores a same-size aligned RGB, so
`output_origin_in_aligned=(0,0)`; `output_border` is tile-local context and must
not be subtracted again. Keep the old full-frame teacher mapping only as a test
adapter while it exists (`output_origin_in_aligned=(31,31)` or `(12,12)`).

Coordinate invariants:

1. The global phase crop `(sx,sy)` is applied exactly once and is the only shift
   from original CFA to aligned CFA.
2. Period trimming removes only trailing rows/columns; it never moves the
   aligned origin.
3. Virtual reflect padding changes context, not output coordinates or final
   dimensions.
4. The student's final center crop is already represented by
   `output_border`; assembly must not crop another 31/12 pixels.
5. The sensor/default crop, DNG warp, and orientation happen only after full
   RGB assembly, exactly as in the existing CUDA path.

Correctness tests:

- exported golden tests for `bayer_s24_d8` and `xtrans_p2_s32_d4`;
- `ProcessCudaTiled_BayerStudentMatchesReferenceAcross1024Boundary`;
- `ProcessCudaTiled_XTransStudentMatchesReferenceAcross1020Boundary`;
- `ProcessCudaTiled_BayerStudentPad32PreservesGrbgOrigin`;
- `ProcessCudaTiled_XTransStudentOverlapHasNoUnwrittenOrDoubleWrittenPixels`;
- full real Nikon/Fuji output size, phase, finite-value, HLR-on/off, default-crop
  and orientation regressions.

Use the checked-in handoff seam/packing-grid metrics as regression diagnostics,
not just visual inspection. Heap-allocate `LibRaw` in every WebGPU-related RAW
test as required by repository policy.

**Exit:** both real RAW fixtures complete through the existing CUDA tiled
product path with the student topology, exact geometry, and deterministic tile
ownership.

#### 8.5 One asynchronous stream before parallel lanes  ✅

`DemosaicWithNeuralEngine` currently synchronizes every call. Remove that
barrier, but keep the first implementation deliberately single-stream and
single-workspace:

1. `EnqueueDemosaicWithNeuralEngine(...)` validates an already-warm model,
   enqueues pack/forward/final center-crop/unpack, and does not synchronize.
2. The existing synchronous entry becomes a wrapper that calls enqueue then
   waits; correctness tests and simple callers keep their current behavior.
3. `ProcessCudaTiled` uses one stream. On that stream it enqueues fused
   reflect/pack → forward → owned output ROI copy for a job before enqueueing
   the next job. CUDA stream ordering makes reuse of the fixed input/RGB buffers
   and one `NeuralDemosaicWorkspace` safe without events or locks.
4. Warm and reserve the exact fixed student shape before the loop. No
   `cudaMalloc`, `GpuMat::create`, model load, or workspace growth is allowed
   after the first tile enqueue.
5. Synchronize once at the existing product completion boundary, after global
   gamma/HLR/color/pack work has also been enqueued.

This removes host/device bubbles with the smallest VRAM footprint. Do not add
`NeuralTileLane`, a ready queue, or host worker threads in this sub-phase.

Tests:

- `NeuralEngineAsyncBayerStudentMatchesSynchronousForward`
- `NeuralEngineAsyncXTransStudentMatchesSynchronousForward`
- `ProcessCudaTiled_StudentSingleStreamReusesBuffersAfterQueuedRoiCopy`
- `NeuralEngineStudentWorkspaceGenerationStableAfterWarmup`
- `NeuralEngineStudentTimedPassHasOneFinalSynchronization`

**Exit:** the product loop enqueues all tiles on one stream with one final wait
and constant owned allocation generation.

#### 8.6 Rebaseline students, then optimize measured bottlenecks

The teacher layer timings above this revision are not applicable to the student
shapes. Extend the landed harness with `--model student` and report the exact
product jobs, including Bayer's clipped first output and X-Trans's four-pixel
overlap work.

For every Conv2d, compute:

```text
FLOPs = 2 * N * Cout * Hout * Wout * Cin * kH * kW
```

Report full-frame FLOPs, bytes, effective TFLOP/s/bandwidth, p50/p95, active MP/s,
tile count, and Neural/Legacy ratio. The handoff estimates (~0.975 TFLOP Bayer,
~1.003 TFLOP X-Trans) are cross-checks only; product accounting wins because it
includes exact grid overlap and edge jobs.

Optimization order:

1. Rebaseline the correct student single-stream path. Topology reduction is
   already delivered; do not tune obsolete 64-channel teacher kernels first.
2. Remove remaining per-tile allocation/synchronization and unnecessary layout
   copies. Measure pack, learned 3×3 trunk/post layers, center-crop, unpack, and
   assembly separately.
3. Tune 3×3 direct kernels for the actual `24→24`, `32→32`, `4→24`, `12→32`,
   `6→24`, and `6→32` shape families. Keep a small compile-time dispatch table;
   no product runtime autotuner.
4. Prepack learned weights once at lazy load only if profiling shows inefficient
   loads. Keep safetensors as the source layout.
5. Capture one CUDA Graph per variant only after buffers/pointers are stable
   **and after Phase 8G has exhausted the FP32 kernel pass**.
   Keep signed-origin fused reflect/pack and variable owned-ROI output copy
   outside the graph. Retain graph replay only for a >=5% full-frame p50 win
   with no p95 or allocation regression.
6. Try 2+ streams only if Nsight/harness evidence shows launch gaps or low SM
   occupancy after steps 1–5. Each lane must own its stream, fixed buffers, and
   workspace; immutable weights may be shared. Select lane count by measured
   full-frame win and owned-VRAM budget. The default remains one lane.
7. Tensor Core, TF32, FP16, and BF16 optimization is not accepted for this
   roadmap revision. Keep the product and benchmark path FP32 on ordinary CUDA
   cores and compatible with the existing CC 6.0+ minimum.

Do not introduce cuDNN/TensorRT as a hard product dependency without a separate
architecture decision.

#### 8.7 Phase 8G: FP32 SIMT implicit-GEMM 3x3 kernels

8F shows that launch overhead is not the primary gap: the product performs
~0.975 / ~1.003 TFLOP but sustains only ~1.95 / ~1.60 TFLOP/s. The current
3x3 kernel assigns one output pixel to each thread and keeps every Cout value in
that thread (`float acc[kCoutTile]`). The exact student dispatch therefore keeps
24 or 32 live accumulators per thread before address and input temporaries. That
design reuses the input apron, but it makes register pressure and instruction
level parallelism inseparable from Cout and is the first bottleneck to remove.

The selected 8G direction is a **scalar-FMA FP32 SIMT implicit-GEMM kernel** for
the learned 3x3 layers. It must not use WMMA, MMA/PTX, Tensor Core libraries,
TF32, FP16, BF16, `cp.async`, or an architecture-specific path above the current
CC 6.0 minimum.

Implementation order:

1. Add a benchmark-only per-layer CUDA-event breakdown for pack, each 3x3
   shape family, 1x1/unpack, and structural work. Record kernel attributes
   (`numRegs`, static/dynamic shared bytes) and calculated active blocks/warps
   per SM for each compiled candidate. Do not add synchronization to the product
   path or timing assertions to correctness CI.
2. Add a compile-time-dispatched implicit-GEMM 3x3 kernel for the dominant
   `24->24` and `32->32` trunks. Treat contiguous output pixels as GEMM M,
   output channels as N, and `Cin*9` as K; cooperatively stage FP32 input and
   weight K-slices in shared memory. Each thread owns a small fixed MxN register
   micro-tile instead of all 24/32 Cout accumulators for one pixel.
3. Keep OIHW safetensors as the source layout. If coalesced K-major weight reads
   require a transpose, prepack once during lazy model upload, account the
   immutable device bytes, and share the packed weights read-only. Never prepack
   per tile or per forward.
4. Extend the same kernel only to `4->24`, `12->32`, `6->24`, and `6->32` when
   the layer breakdown shows a full-frame contribution worth optimizing. Thin
   Cin layers may retain the 8F direct kernel when it is faster.
5. Keep `Conv2d3x3s1TiledKernel` as the generic/fallback implementation. The
   product dispatch table may select only frozen, measured shape families; do
   not add a runtime autotuner, device-specific tuning database, or second tile
   scheduler.

8G acceptance and stopping rules:

- Both exported student goldens, `MlOpsTest`, Bayer/X-Trans full-RAW geometry,
  CFA phase, seam/ownership, HLR, and allocation-generation tests remain green.
- Numerical behavior remains the existing FP32 contract; any tolerance change
  requires a separate quality decision and is not part of 8G.
- Retain a kernel candidate only with a >=5% full-frame p50 win on at least one
  variant and no regression on the other variant that dispatches it. Report
  p50/p95, per-layer time, effective TFLOP/s, registers, occupancy, and owned
  weight/workspace bytes against the 8F JSON baseline.
- Target the trunk first. Do not spend 8G on CUDA Graph or multiple streams
  unless the retained FP32 kernel raises compute throughput enough that a new
  profile shows launch gaps totaling >=5% of full-frame p50.
- Stop the 8G kernel search after the frozen compile-time candidates fail the
  >=5% retention rule; preserve the best correct FP32 dispatch and report the
  remaining gap rather than introducing Tensor Core or reduced precision.

#### 8.8 Post-8G FP32 small-Cout 1x1 pass

The direct-trunk recheck showed that the retained 8F 3x3 kernels already sustain
~4.9-5.5 TFLOP/s in isolated Release measurements. The earlier ~1.5-2.0
TFLOP/s figure divided total product latency by convolution FLOPs and therefore
did not describe the 3x3 kernels alone. Per-layer timing instead exposed two
low-arithmetic-intensity 1x1 layers with disproportionate latency:

- `residual`: `24/32 -> 12`; the generic Cout-tile-8 kernel launches a second
  partial tile and reads the input again;
- `output`: `24/32 -> 3`; five of every eight Cout lanes are inactive.

The retained `Conv2d1x1SmallCoutKernel<3|12>` assigns one spatial position to
each thread, reads each Cin value once, and accumulates the exact Cout values in
registers. It uses scalar FP32 FMA only, allocates no workspace, preserves NCHW
and OIHW layouts, and keeps the CC 6.0+ contract.

Measured Release medians at the product tile shapes:

| Layer | 8F generic 1x1 | Exact small-Cout | Delta |
|------|----------------:|-----------------:|------:|
| Bayer residual `24->12` | ~0.652 ms | ~0.106 ms | -84% |
| Bayer output `24->3` | ~0.868 ms | ~0.295 ms | -66% |
| X-Trans residual `32->12` | ~0.783 ms | ~0.122 ms | -84% |
| X-Trans output `32->3` | ~0.863 ms | ~0.363 ms | -58% |

At 40/48 product jobs this explains approximately 45/56 ms of full-frame
savings. A short alternating full-frame run (warm-up 1, 5 iterations) measured
Bayer ~393 ms and X-Trans ~427 ms p50. Treat those absolute values as a retained
sanity check, not a replacement 20-iteration baseline: the laptop reached 87 C
after the run, and a preceding long run fell to P8 / 210 MHz and produced
invalid multi-second samples.

Correctness coverage includes purpose-named CPU comparisons for product
`24->12` residual and `32->3` output shapes plus both exported student goldens.
The non-performance Conv2d/DemosaicNet selection passes 27/27 tests. Existing
Debug C64 soft performance floors are excluded from this correctness result.

#### 8.9 Post-8G tile concurrency experiment

`DemosaicNetPerfHarness --mode tile --lanes N` now creates real independent
lanes for experiment only. Each lane owns one CUDA stream,
`NeuralDemosaicWorkspace`, boundary buffers, and RGB output; immutable model
weights and the synthetic read-only CFA input are shared. Timed batches enqueue
all lanes before waiting, validate identical RGB output, and report batch wall
time, maximum per-lane CUDA event time, aggregate allocation generation, and
owned device bytes. This does not change `ProcessCudaTiled`.

Short Release batches (warm-up 2, 10 iterations) on RTX 3080 Laptop, P0 with SM
clock ~1.67-1.71 GHz:

| Variant | 1 lane | 2 lanes | 3 lanes | Owned VRAM per lane |
|---------|-------:|--------:|--------:|--------------------:|
| Bayer | ~163 tiles/s | ~155 tiles/s | ~157 tiles/s | ~429 MiB |
| X-Trans | ~188 tiles/s | ~178 tiles/s | ~173 tiles/s | ~384 MiB |

Batch latency scaled almost linearly with lane count, so the concurrent streams
compete for already-saturated SM/register/shared-memory resources rather than
hiding launch gaps. No candidate clears the >=5% full-frame retention gate;
multi-lane product execution is rejected and the default remains one lane.
Artifacts: `build/perf/demosaicnet_tile_lanes_{1,2,3}.json`.

#### 8.10 Winograd implementation audit and repaired candidate

The initial F(2x2,3x3) result did not isolate the algorithm from avoidable
implementation overhead. The follow-up audit found three concrete issues:

1. `B^T d B` was recomputed independently by all 24/32 output-channel threads
   for the same spatial tile and Cin value.
2. Immutable 3x3 filters were transformed from OIHW inside every spatial block
   and every forward rather than once before hot-path dispatch.
3. Tight 16-float U/V records created regular shared-memory bank aliasing. A
   17-float shared stride reduced the repaired candidate latency by roughly
   2.2-2.5x versus its unpadded version in the same implementation series.

The retained experimental API accepts an optional pretransformed
`[Cout,Cin,16]` pointer. Generic callers still supply OIHW only and select direct;
the performance harness opts in explicitly with `--conv-winograd`. CPU reference
tests cover 24- and 32-channel square trunks with partial odd edge tiles. All
136 non-performance `MlOpsTest` cases, including student exported goldens, pass.

Same-build Release medians on RTX 3080 Laptop (20 measured iterations):

| Trunk | Direct | Repaired Winograd | Result |
|-------|-------:|------------------:|--------|
| Bayer 24->24 | ~0.57-0.60 ms | ~1.13-1.20 ms | reject (~1.9-2.0x slower) |
| X-Trans 32->32 | ~0.91-0.93 ms | ~1.65-1.68 ms | reject (~1.8x slower) |

The F(2x2,3x3) multiply-count reduction is real, but for these narrow FP32 NCHW
layers the transform, synchronization, shared staging, and four-output epilogue
remain more expensive than the retained direct kernel. Do not product-dispatch
this candidate or extend it to thin-Cin layers. Future work toward the 100 ms
goal should target activation lifetime/tile-size and launch amortization, or a
deliberate channels-last/vectorized convolution track, rather than another fused
F(2x2,3x3) tile permutation.

The executable follow-up plan is tracked separately in
[`cuda_demosaicnet_performance_next.md`](cuda_demosaicnet_performance_next.md).

Artifacts:

- `build/perf/demosaicnet_student_8g_direct_recheck_conv.json`
- `build/perf/demosaicnet_student_8g_winograd_repaired_conv.json`
- rejected diagnostic layouts:
  `demosaicnet_student_8g_winograd_{prepacked,prepacked_padded,
  prepacked_transposed,warp_per_tile}_conv.json`

#### 8.11 Measurement matrix and stopping rules

Every optimization candidate is evaluated with this matrix:

| Axis | Values |
|------|--------|
| Variant | Bayer, X-Trans |
| Method | Legacy, Neural |
| Scope | conv, 1K tile, full active-area |
| Tile policy | Bayer 1086/1024/step1024/pad32; X-Trans 1048/1024/step1020/pad12 |
| Lanes | 1 primary; 2+ only after the single-stream profiling gate |
| Precision | FP32 only |
| HLR | off (primary), on (regression/secondary timing) |
| Iteration | 3 warm-up, >=20 measured |
| Device state | temperature, P-state, SM/memory clocks at start/end |

Stopping rules:

1. Reject any candidate that breaks a handoff metadata/golden, CFA-origin,
   ownership, seam, geometry, HLR, or allocation invariant, even if faster.
2. Keep a change only when full-frame p50 improves by >=5% or it is a necessary
   dependency for a measured later win.
3. A variant-specific kernel dispatch is allowed; a hidden variant-specific
   tile scheduler is not.
4. If student FP32 + async single stream + retained kernel wins remains more
   than 2× slower than Legacy and the measured roofline says FP32 cannot close
   the gap, stop low-value tuning and report the constraint. Do not cross into
   Tensor Core or reduced-precision work under Phase 8G.
5. Never infer full-frame success by multiplying a single-tile number; always
   run the full fixtures because lane saturation, WDDM scheduling, clocks, and
   edge-tile shapes affect p95.
6. Reject a performance run when thermal/power throttling changes the GPU to a
   low-clock state (for example the observed P8 / 210 MHz) or when clocks vary
   enough to dominate the candidate delta. Cool the device and rerun candidates
   in short alternating batches; never interpret throttling as a code regression.

#### 8.12 Implementation slices

The change is intentionally split so topology, tile geometry, and performance
work do not land as one unreviewable patch:

1. **8A — Student module contract:** ✅ hard-code both student forwards, validate
   bundled metadata/checksums/fixed weights, update workspace estimates, and
   pass exported goldens. No product routing change.
2. **8B — Shared tile policy:** ✅ generalize `CudaTileJob/BuildTileJobs`, add
   signed-origin fused reflect/pack and disjoint ownership ROIs, and prove Legacy
   jobs are unchanged with CPU planner tests.
3. **8C — Bayer product integration:** ✅ pad32/border31/step1024, explicit output
   geometry, real Nikon full-RAW and seam/phase regressions.
4. **8D — X-Trans product integration:** ✅ pad12/border12/step1020, four-pixel
   first-writer overlap ownership, real Fuji full-RAW and seam/phase regressions.
5. **8E — Async single-stream execution:** ✅ enqueue API, one reusable fixed
   workspace/buffer set, one final synchronization, stable allocation generation.
6. **8F — Student performance pass:** ✅ rebaseline Legacy/student full-frame,
   exact product FLOP/byte roofline (handoff ~0.975 / ~1.003 TFLOP matches product
   accounting), retained student-width 3×3 dispatch; CUDA Graph / lanes / mixed
   precision deferred with measured rationale (see exit criteria +
   `build/perf/demosaicnet_student_8f_summary.json`).
7. **8G — FP32 SIMT 3x3 kernel pass:** ✅ per-layer + kernel-attribute harness;
   apron-based implicit-GEMM candidates for `24->24` / `32->32` implemented and
   correctness-tested; full-frame retention failed (≥5% p50) so 8F direct remains
   product path; thin-Cin left on direct; CUDA Graph / lanes / Tensor Core /
   reduced precision still excluded (see `build/perf/demosaicnet_student_8g_summary.json`).
8. **Post-8G — FP32 exact small-Cout 1x1 pass:** ✅ product-retain exact
   `Cout=12` residual and `Cout=3` output kernels; per-layer latency falls
   58-84%, short full-frame sanity check is ~393 / ~427 ms, and 27/27 selected
   correctness/golden tests pass. Long thermally throttled samples are rejected;
   see `build/perf/demosaicnet_student_small_cout_{conv,full_short}.json`.
9. **Post-8G — tile concurrency experiment:** ✅ benchmark-only real 1/2/3
   stream+workspace lanes with output/allocation/VRAM checks. Two and three
   lanes regress throughput by ~5-8% and consume ~2x/3x workspace VRAM, so the
   product path remains single-lane; see
   `build/perf/demosaicnet_tile_lanes_{1,2,3}.json`.
10. **8G+ — FP32 Winograd F(2×2,3×3) trunk pass:** ✅ fused candidate for
    `24->24` / `32->32` implemented and correctness-tested; per-layer and short
    full-frame retention failed vs product direct (+57% trunk latency; short
    full-frame ~484/561 ms vs ~393/427 ms). Product remains 8F direct 3×3;
    candidate queryable as `winograd_f22_24/32`. See
    `build/perf/demosaicnet_student_8g_winograd_{conv,full_short,summary}.json`.
11. **8G+ audit — repaired prepacked/shared-input Winograd:** ✅ remove repeated
    input/filter transforms, add bank-conflict-safe shared layout, explicit
    `--conv-winograd` measurement, partial-edge CPU comparisons, and same-build
    direct comparison. Best repaired candidate remains ~1.8-2.0x slower, so the
    product stays direct; see
    `build/perf/demosaicnet_student_8g_{direct_recheck,winograd_repaired}_conv.json`.

Each slice must keep `MlOpsTest` and RAW CUDA tests green. Build/test with the
repository's MSVC wrapper; do not add timing assertions to default correctness
CI.

##### Phase 8 exit criteria

- [x] Bundled weights are accepted only as `bayer_s24_d8` and
      `xtrans_p2_s32_d4`; both hard-coded forwards pass exported goldens. (8A)
- [x] Existing `BuildTileJobs` is the single scheduler for Legacy and Neural;
      Legacy job geometry is unchanged. (8B planner; product wiring in 8C/8D)
- [x] Bayer uses pad32/border31/step1024 and every tile input is mod-2 aligned.
      (8B policy+tests; product wiring in 8C)
- [x] X-Trans uses pad12/border12/step1020, every tile input is mod-6 aligned,
      and the four-pixel overlap has deterministic first-writer ownership.
      (8B policy+tests; product wiring in 8D)
- [x] Assembled student RGB maps explicitly to aligned/original CFA coordinates;
      phase crop, trailing trim, tile border, and final sensor crop are each
      applied exactly once. (`NeuralOutputGeometry` + student tiled product path)
- [x] Bayer and X-Trans full-active-area Legacy baselines and student/Legacy p50
      and p95 ratios are recorded on the same runs/hardware. (8F Release harness,
      RTX 3080 Laptop / CC 8.6 / driver 13.3 / runtime 12.8; see
      `build/perf/demosaicnet_student_{bayer,xtrans}_post_dispatch.json`)
- [x] The best measured Bayer and X-Trans ratios are reported against the <=1.10
      objective; any remaining gap has a roofline/kernel explanation.
      (Bayer p50 ratio ≈4.22, X-Trans ≈5.70 — miss ≤1.10; full-frame work is only
      ~0.975 / ~1.003 TFLOP so Legacy parity needs ~8–9 TFLOP/s, within sustained
      FP32 ~11.6, but measured effective is only ~1.6–2.0 TFLOP/s → continue FP32
      direct-kernel track; interim kernel headroom is large.)
- [x] Absolute p50/p95 and progress toward the 100 ms stretch target are
      reported without making 100 ms a completion gate.
      (Bayer neural p50/p95 ≈499/543 ms; X-Trans ≈627/1230 ms; stretch best-case
      FP32 ≈84–87 ms if sustained rates were achieved.)
- [x] Correctness/golden/full-RAW tests pass for the selected precision path.
      (Harness aborts on shape/finite failure before timing; FP32 path unchanged.)
- [x] The default product path uses one stream/workspace and no per-tile
      synchronization; any retained multi-lane path has a measured >=5% win and
      a bounded owned-VRAM calculation. (8E single-stream; multi-lane still gated)
- [x] No allocation-generation change occurs during timed hot iterations.
      (workspace generation tests + 8E student warm path; harness recheck in 8F)
- [x] CUDA Graph replay, FP32 student shape variants, optional lanes, and gated
      precision have each been retained or rejected with full-frame data; no
      known >=5% full-frame win remains untested.
      (**Retained:** Cout-tile 24 for Bayer student + Cout-tile 32 for thin-Cin
      post_conv — measured full-frame p50 wins vs pre-dispatch baseline.
      **Deferred / rejected for default path:** CUDA Graph (no ≥5% proof yet;
      primary gap is kernel rate not launch overhead); multi-lane (gated until
      single-stream kernel util rises); mixed precision (FP32 envelope still
      sufficient in principle for Legacy parity).)
- [x] Full benchmark command, device metadata, commit, and JSON summary are
      recorded and reproducible.
      (`DemosaicNetPerfHarness --model student --mode full --warmup 3
      --iterations 20`; summary `build/perf/demosaicnet_student_8f_summary.json`.)

### Phase 9 — Optional / separate: pipeline workspace migration (track §3.10)

**Not required for demosaicnet “path done.”** Schedule as its own roadmap item
or PR series when ready:

1. Inventory and document remaining scratch owners (keep §1.3 table updated).
2. Migrate `GPU_KernelLauncher` work/temp accounting to a shared vocabulary
   (`capacity_bytes` / release high-water).
3. Prototype one heavy consumer (e.g. LLF pyramid levels) on pool-backed storage.
4. Build a simple “owned VRAM by subsystem” debug dump; keep `cudaMemGetInfo` as
   residual only.

If Phase 9 starts early, **do not** block Phase 4–6 or force a big-bang rewrite
of RCD/highlight in the same PR as the first golden forward.

---

## 5. Concrete first tasks (ordered for the next agent)

1. **Read** this doc + existing:
   - `include/cuda/nn/{tensor,relu,common,workspace,device_buffer}.hpp`
   - `cuda/nn/{relu,conv2d,conv_transpose2d}.cu`
   - `tests/ml_ops/*` (safetensors scrapers in conv tests)
   - demosaicnet `modules.py` forward (upstream) + hard-coded C++ modules
   - `raw_processor.hpp` + `raw_processor_cuda.cpp`
   - Scratch inventory samples: `gpu_scheduler.cuh`, `tone_mapping.cuh`
     (`EnsurePyramidBuffers`), `cuda_debayer_rcd.hpp` (`RcdWorkspace`),
     `cuda_highlight_reconstruct.hpp` (`HighlightWorkspace`)
   - **Python product reference (Phase 6b/6c):**
     `D:\Projects\deepjoint_demosiacing\demosaicnet_caffe\pytorch\raw_pipeline.py`
     (phase-align, gamma, full RAW flow),
     `...\pytorch\infer.py` (`_make_mosaic`, `_run_tiles`),
     `...\pytorch\model.py`, `...\demosaicnet\layers.py`
2. **Phase 0–3:** already done.
3. **Phase 4:** safetensors → `SafetensorsTensorMap` DTO in `cuda/nn` (no graph). ✅
4. **Phase 5:** CRTP `LoadWeights` + hard-coded Bayer/XTrans modules + lazy
   cache + goldens + no-reload / dual-workspace concurrency. ✅
5. **Phase 6a:** GpuMat entry + RawParams + lazy trigger (no backend boot load);
   thumbnails Classical. ✅
6. **Phase 6b:** ✅ Neural preprocess — CFA phase-align + gamma 1/2.2 in / 2.2 out
   after `ToLinearRef` (`demosaicnet_preprocess.*`, wired in `ProcessCudaFullFrame`).
7. **Phase 6c:** Neural Engine tiling via existing `ProcessCudaTiled` /
   `BuildTileJobs`; default tiled for Neural Engine; full active-area RAW tests. ✅
8. **Phase 7:** workspace policy + parallel readiness.
9. **Phase 8:** hard-code the two bundled student models, reuse/generalize the
   existing CUDA tile planner for pad32/step1024 Bayer and pad12/step1020
   X-Trans, make the single-stream loop asynchronous, then rebaseline/optimize.
10. **Phase 9 (optional):** broader workspace migration when prioritized.
11. **P5 production consolidation (next):** before any new optimization, execute
    the deletion inventory in `cuda_demosaicnet_performance_next.md` §11. Leave
    fixed 1024 single-stream persistent NHWC as the only neural execution path;
    remove experiment flags/assets/tests and neural-to-neural fallbacks; retain
    only failure recovery to Classical/Legacy.

Do **not** skip golden tests. Wrong pad/crop/group looks “plausible” but is
useless.

Do **not** implement model runners inside `cuda/nn`. Domain placement is
intentional.

Do **not** introduce a DemosaicNet type that pretends to be a platform GPU
context.

---

## 6. Testing and CMake conventions

- Primary suite: **`MlOpsTest`** for ops, safetensors, model cache, goldens.
- RAW product / tiling tests: **`CudaRawOpsTest`** and related targets under
  `alcedo_studio/tests/raw/` (fixtures in
  `tests/resources/sample_images/raw/camera/...`).
- **No “smoke” tests.** Every test name and comment must state the contract
  (see `Agents.md` test-naming ban). Prefer real camera RAW over synthetic CFA
  when the claim is “works on real RAW.”
- Build:

  ```bat
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MlOpsTest --parallel 4
  build\debug\alcedo_studio\tests\MlOpsTest.exe

  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target CudaRawOpsTest --parallel 4
  build\debug\alcedo_studio\tests\CudaRawOpsTest.exe --gtest_filter=*NeuralEngine*
  ```

- Link: `GTest::gtest_main`, `CudaUtils`, `CUDA::cudart`, OpenCV; domain tests
  link the library that owns `decoders/processor/nn`.
  **Do not** add `CUDA::cublas` / cuDNN.
- Guard with `ALCEDO_CUDA_ENABLED` / `HAVE_CUDA`.
- Model path: `ALCEDO_DEMOASICNET_MODEL_DIR` and/or POST_BUILD copy; same helper
  for tests and production lazy load.
- LibRaw in tests: heap-allocate (`std::make_unique<LibRaw>()`).

---

## 7. Design rules for implementers

1. **Match demosaicnet math first**, optimize second.
2. **pad=0** for all pretrained 3×3 layers.
3. **groups=3** on Bayer upsample (`[12,1,2,2]` weight layout).
4. Every new op: host CPU reference + device test + stream parameter.
5. No intermediate H2D/D2H in model forward.
6. No per-layer alloc in steady state.
7. Generic ops / `WorkspacePool` / safetensors in `cuda/nn`; domain demosaicnet
   only under `decoders/processor/`.
8. Existing CUDA style (`CheckCuda` / exceptions).
9. Private members trailing `_`.
10. GPLv3 license headers.
11. **Lazy load once per variant** via model cache; never in `RawProcessor` ctor;
    never at unconditional app boot.
12. **Immutable shared weights after LoadWeights; private workspaces** for
    concurrency.
13. Thumbnails / parallel decode default to **Classical**.
14. Fail soft to Classical if load fails.
14b. Neural Engine on CUDA FULL **defaults to tiled** via existing
    `ProcessCudaTiled` / `BuildTileJobs`; Phase 8 supplies the student-specific
    input/output/pad/step policy. Do not force FullFrame and do not invent a
    parallel NN tile scheduler.
14c. Neural path **must** CFA phase-align to training origin and apply gamma
    1/2.2 encode / 2.2 decode around the network after `ToLinearRef`. Do **not**
    re-apply white balance; do not put gamma inside hard-coded `Forward`.
14d. Neural path `Clamp01` is **gated by** `highlights_reconstruct_`: skip when
    HLR is on (over-range required); **apply when HLR is off** (expected).
    Gamma kernels must not hard-clip to `[0,1]` themselves.
15. Do **not** name domain types `*Context` in a way that confuses them with
    OpenCL/Metal device contexts.
16. Prefer **owned byte counters** (`ResidentWeightBytes`,
    `WorkspacePool::capacity_bytes`) over inventing another `cudaMemGetInfo`-only
    diagnostic for this subsystem.
17. **Hard-code topology** in C++ modules; safetensors is **weights only**
    (DTO → CRTP `LoadWeights`). Never assemble a runtime op graph from the file.
18. Prefer **CRTP** for the unified weight-load entry so each specialized module
    keeps concrete slots and a non-virtual hot-path `Forward`.

---

## 8. Non-goals (explicit)

- Training / backward / autograd.
- General ONNX graph runtime.
- Arbitrary activations / Normalizations / Attention.
- FP16 / TF32 mixed precision (possible later; not required for v1).
- Shipping PyTorch or Python at runtime.
- Replacing Legacy demosaic as the Bayer default until quality/perf gates pass.
- Reloading or cloning full weight packs per `RawProcessor`.
- Putting CFA / LibRaw concerns into `cuda/nn`.
- Modeling DemosaicNet as a peer of `OpenClContext` / `MetalContext`.
- Eager weight load when the user never selects NN demosaic.
- Big-bang rewrite of all pipeline scratch into `WorkspacePool` inside the first
  demosaicnet PR (tracked as Phase 9 / §3.10).
- Process-global mutable activation workspace without a lease protocol.
- **Runtime network / graph assembly** from safetensors (layer lists, dynamic
  depth/width, “execute whatever is in the file”).

---

## 9. Acceptance criteria for “DemosaicNet path done”

1. Bayer and XTrans are **hard-coded modules** that **lazy-load once per
   variant** (DTO → CRTP `LoadWeights`) into a RAW-scoped cache and run on CUDA.
2. Forwards match a frozen reference within test tolerances.
3. `MlOpsTest` covers primitives in §2.3, DTO parse, lazy load, shape-reject on
   load, and no-reload behavior.
4. Domain code lives under `decoders/processor/nn/` (+ GpuMat operator); generic
   ops + safetensors DTO stay in `cuda/nn/`.
5. GpuMat in → GpuMat out with no mandatory host intermediate.
6. Steady-state forward does not `cudaMalloc` after workspace reserve.
7. Concurrent forwards with distinct workspaces are defined and tested.
8. Neural path applies **CFA phase-align** + **gamma 1/2.2 in / 2.2 out** after
   `ToLinearRef` (Phase 6b); WB is not reapplied; `Clamp01` only when HLR is
   off (when HLR is on, over-range samples are preserved).
9. Student tile policy is implemented by **reusing** `ProcessCudaTiled` /
   `BuildTileJobs`: Bayer pad32/border31/step1024, X-Trans
   pad12/border12/step1020 with deterministic overlap ownership (Phase 8);
   Neural Engine defaults to tiled mode.
10. `RawProcessor` construction does not touch weight files; cold cache until
    first NN use.
11. Users who never select NN demosaic do not pay model load cost at app start.
12. Interactive latency for a ≤1K-edge tile recorded after Phase 6/8 profile.
13. At least one full active-area real Bayer RAW and one real X-Trans RAW
    demosaic test (purpose-named) pass on CUDA.
14. Production cleanup leaves no runtime/harness selector for a rejected neural
    implementation. Original safetensors are the only model assets; shipping
    weights are prepacked once at lazy load; the sole fallback is high-level
    Neural failure to Classical/Legacy.

Phase 9 (pipeline-wide workspace migration) is **not** part of this acceptance
bar.

---

## 10. Reference index

| Resource | Path / URL |
|----------|------------|
| Bayer weights | `alcedo_studio/src/config/models/bayer.safetensors` |
| XTrans weights | `alcedo_studio/src/config/models/xtrans.safetensors` |
| Generic NN headers | `alcedo_studio/src/include/cuda/nn/` |
| Generic NN sources | `alcedo_studio/src/cuda/nn/` |
| Domain NN (planned) | `include/decoders/processor/nn/` (CRTP modules + cache) |
| Safetensors DTO (planned) | `include/cuda/nn/safetensors.hpp` |
| GpuMat demosaic op (planned) | `decoders/processor/operators/gpu/cuda_demosaicnet.*` |
| Tests | `alcedo_studio/tests/ml_ops/` |
| PyTorch modules | https://github.com/mgharbi/demosaicnet/blob/master/demosaicnet/modules.py |
| Platform GPU contexts (contrast only) | `include/opencl/opencl_context.hpp`, `include/metal/metal_context.hpp` |
| Pipeline scratch launcher | `include/edit/pipeline/gpu_scheduler.cuh` |
| LLF pyramid allocs | `include/edit/operators/GPU_kernels/tone_mapping.cuh` |
| RCD / highlight workspaces | `operators/gpu/cuda_debayer_rcd.hpp`, `cuda_highlight_reconstruct.hpp` |
| Preview scratch reclaim | `pipeline_cpu.hpp` `ReleasePreviewGpuScratch`, `cuda_preview_vram_reclamation_test.cu` |
| RAW CUDA demosaic call sites | `decoders/processor/raw_processor_cuda.cpp` |
| CUDA tile mode selection | `raw_processor.cpp` `SelectCudaExecutionMode` |
| CUDA tile jobs / RCD tiled path | `raw_processor_cuda.cpp` `BuildTileJobs` / `ProcessCudaTiled` |
| Real camera RAW fixtures | `tests/resources/sample_images/raw/camera/` |
| Python ref root (external) | `D:\Projects\deepjoint_demosiacing\demosaicnet_caffe\` |
| Python product pipeline | `...\pytorch\raw_pipeline.py` — phase-align, gamma in/out, tile; WB → Alcedo `ToLinearRef` |
| Python mosaic + tiles | `...\pytorch\infer.py` — `_make_mosaic`, `_run_tiles`, `_model_crop` |
| Python model | `...\pytorch\model.py` |
| Python color (post-demosaic) | `...\pytorch\raw_color.py` |
| Original layers / nets | `...\demosaicnet\layers.py`, `...\demosaicnet\models.py` |
| Neural preprocess (planned 6b) | `decoders/processor/nn/demosaicnet_preprocess.*` |
| Workspace non-thread-safety | `include/cuda/nn/workspace.hpp` |

---

## 11. Suggested commit / PR slices

1. DeviceBuffer + WorkspacePool + Mul/Concat/Crop/Slice/Layout  ✅  
2. Conv2d (+ fused Bias/ReLU) + layer weight tests  ✅  
3. ConvTranspose2d unpack specialization  ✅  
4. **Safetensors → DTO in `cuda/nn` + tests** (Phase 4)  
5. **CRTP LoadWeights + hard-coded Bayer/XTrans + lazy cache + goldens**
   (Phase 5) ✅  
6. **GpuMat entry + RawParams + lazy trigger (no boot load)** (Phase 6a) ✅  
7. **Neural preprocess: CFA phase-align + gamma 1/2.2 in / 2.2 out** (Phase 6b;
   WB already in `ToLinearRef`)  
8. **Neural Engine tiling via existing CUDA tile path + full-res RAW tests**
   (Phase 6c)  
9. **Workspace policy + parallel readiness** (Phase 7)  
10. **Student modules + CFA-safe shared tiling + perf pass** (Phase 8)
11. **Optional:** pipeline workspace migration PR series (Phase 9 / §3.10)

Each demosaicnet PR must leave `MlOpsTest` green and must not reintroduce
per-processor weight reload or a fake platform “context.”

---

## 12. Design decision record

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Where does DemosaicNet live? | `decoders/processor/nn/` only | Domain demosaic; will grow OpenCL/Metal; not a platform GPU service |
| Analogy to OpenCL/Metal context? | **No** | Those manage device/queue/programs; this only caches hard-coded modules |
| Topology source | **Hard-coded C++ modules** (`Forward` body + fixed weight slots) | Specialized demosaic algorithm, not a graph runtime |
| Safetensors role | **Weights DTO only** (`SafetensorsTensorMap`) | Parser stays generic; never builds a network |
| Weight ingestion API | **CRTP** `NnWeightModule` + `LoadWeights(DTO)` | Unified load entry; concrete slots; no vtable on hot path |
| Weight load policy | **Lazy** on first NN use | Users who never enable NN pay nothing at boot; still reuse after first load |
| Weight lifetime after load | Process (until optional Unload) | Avoid reload across images / edit sessions |
| Who owns activations? | Caller `WorkspacePool` | Pool is not thread-safe; activations reclaimable without unloading weights |
| Who owns weights? | Hard-coded module instance in RAW model cache | Shared, immutable after load, concurrent readers OK |
| Thumbnail NN? | No by default | Parallel thumb path stays Classical |
| Neural Engine tile routing? | **Always tiled** on CUDA FULL (not long-edge threshold) | Activation VRAM dominates; reuse `ProcessCudaTiled` with fixed student tile shapes |
| Tile implementation? | Extend existing `BuildTileJobs` / `ProcessCudaTiled` with the Phase 8 policy | One scheduler; Bayer pad32/step1024, X-Trans pad12/step1020 |
| Neural preprocess (Phase 6b)? | **CFA phase-align + γ 1/2.2 in / 2.2 out** after `ToLinearRef` | Matches demosaicnet training domain; required before full-res quality claims |
| White balance on Neural path? | **Only via `ToLinearRef`** | Do not re-apply cam_mul/WB before the network (Python reference does WB only because it has no `ToLinearRef`) |
| Range clamp on Neural path? | **`Clamp01` iff HLR off** (same as current full-frame Neural) | HLR needs `> 1`; with reconstruct disabled, clamp is the intended product path |
| Preprocess vs tiling order? | **6b preprocess first, then 6c tiling** | Correct domain on full-frame, then scale with existing tiles |
| Is “some context” still needed? | Yes — as **workspace/scratch discipline**, not as DemosaicNet-as-context | Inventory of scattered RAII scratch (§1.3); `WorkspacePool` is the seed |
| Pipeline scratch unification | Separate Phase 9 track | Do not block demosaicnet goldens on big-bang migration |
| VRAM observability | Prefer owned byte counters; `cudaMemGetInfo` residual only | Matches how free VRAM is logged today but adds subsystem truth |
| Safetensors code location | Generic DTO parse in `cuda/nn`; key/shape tables inside each module’s `LoadWeightsImpl` | Parser reusable; manifests stay with the hard-coded module |
