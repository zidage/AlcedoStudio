# CUDA CNN Forward Framework + DemosaicNet Plan

Date: 2026-07-09

Status: Phase 3 complete (ConvTranspose2d with specialized unpack_mosaick
12→3 k=2 s=2 groups=3 path + generic grouped gather fallback; pure cudart).
Phases 0–3 remain done. Next: Phase 4 generic safetensors IO, then a
**RAW-scoped, lazy-loaded** DemosaicNet submodule (Phases 5–8). A separate
architectural track (§3.10) records how `WorkspacePool` generalizes beyond NN
to the rest of the image pipeline.

This document is the handoff plan for:

1. A **forward-only, CNN-focused CUDA inference mini-framework** under
   `alcedo_studio/src/cuda/nn/` (generic primitives, IO, **workspace**).
2. A **domain-specific DemosaicNet submodule** that lives only under the RAW
   processor module—not as a peer of `OpenClContext` / `MetalContext`.
3. A **lazy model cache** (load on first user choice of NN demosaic; reuse for
   the rest of the process / later edit sessions) for immutable device weights.
4. A clear separation between **ephemeral scratch** (`WorkspacePool`) and
   **long-lived domain state** (weights)—and a note on migrating the rest of the
   pipeline’s scattered scratch RAII toward the same idea later.

Bundled weights:

- `alcedo_studio/src/config/models/bayer.safetensors` (~2.2 MiB on disk)
- `alcedo_studio/src/config/models/xtrans.safetensors` (~1.6 MiB on disk)

Metadata on both files: `format = demosaicnet-pytorch-state_dict`.

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

**Domain DemosaicNet submodule** (RAW-only; not a platform GPU service):

```text
alcedo_studio/src/include/decoders/processor/nn/
  demosaicnet_cache.hpp        # lazy model cache (NOT a device context)
  demosaicnet_weights.hpp      # immutable device weight pack + manifests
  demosaicnet_bayer.hpp        # Bayer graph runner (views weights)
  demosaicnet_xtrans.hpp       # XTrans graph runner (views weights)
  demosaicnet_mosaic.hpp       # CFA → 3ch demosaicnet mosaic pack helpers

alcedo_studio/src/decoders/processor/nn/
  demosaicnet_cache.cpp
  demosaicnet_weights.cpp
  demosaicnet_bayer.cu
  demosaicnet_xtrans.cu
  demosaicnet_mosaic.cu

alcedo_studio/src/include/decoders/processor/operators/gpu/
  cuda_demosaicnet.hpp         # GpuMat entry used by raw_processor_cuda

alcedo_studio/src/decoders/processor/operators/gpu/
  cuda_demosaicnet.cu
```

Namespace sketch: keep under `alcedo` / decoder-local types consistent with
existing `CUDA::RcdWorkspace` style. Prefer names that read as **model cache /
weights / forward**, never as `*Context` that could be confused with
`OpenClContext` / `MetalContext`.

**Naming ban for implementers:** do **not** introduce
`DemosaicNetContext` as a process-global device-manager peer. Prefer:

- `DemosaicNetModelCache` / `DemosaicNetWeightCache` — long-lived weight store
- `DemosaicNetBayerWeights` / `…XTransWeights` — immutable packs
- `BayerDemosaicNetForward` — pure graph function

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
| 9 | **Safetensors load** | Both | Host parse → device weight buffers (lazy, once per variant). |
| 10 | **Workspace allocator** | Both | Scratch tensors; no per-layer `cudaMalloc` in steady state. |
| 11 | **Model runners** | Both | Domain Bayer / XTrans forward under RAW `nn/`. |
| 12 | **Lazy model cache** | Both | RAW-scoped; not a platform GPU context. |

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
| **Domain model cache** | planned `DemosaicNetModelCache` | Immutable weights only | Lazy → process (until unload) | **Yes — RAW-scoped only** |
| **Operator-local workspace structs** | `RcdWorkspace`, `HighlightWorkspace`, LLF pyramids | Typed temps for one algorithm | Call / stage | Legacy pattern; do not invent another for NN |

**DemosaicNet is fold (B): domain-scoped.**  
**WorkspacePool is fold (A): the necessary “some context” for scratch**—but it
is an **allocator**, not a device manager, and it must not be named or
documented as “the DemosaicNet context.”

### 3.2 Layers

```text
┌──────────────────────────────────────────────────────────────────────┐
│  Product trigger (editor / RawParams)                                │
│    User selects DemosaicNet  →  first decode that needs it           │
│    lazy EnsureLoaded(Bayer|XTrans)  →  weights stay for process      │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ shared immutable weights (after first load)
┌───────────────────────────────▼──────────────────────────────────────┐
│  RAW stage (per image / per worker)                                  │
│    RawProcessor::ProcessCuda*  →  CUDA::DemosaicNet*(GpuMat, …)      │
│    call-local: stream + WorkspacePool (activations only)             │
└───────────────────────────────┬──────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────┐
│  Domain submodule  decoders/processor/nn/   (RAW only)               │
│    DemosaicNetModelCache   lazy weight cache (not a GPU backend)     │
│    DemosaicNet*Weights     device buffers + manifests                │
│    Bayer / XTrans Forward  graph only; view weights                  │
│    mosaic pack             CFA → 3ch demosaicnet input               │
│    [future] OpenCL/Metal   same domain API, other backends           │
└───────────────────────────────┬──────────────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────────────┐
│  Generic  cuda/nn                                                    │
│    ops: relu, conv2d, conv_transpose2d, mul, concat, crop, layout    │
│    DeviceTensor, DeviceBuffer, WorkspacePool, safetensors IO         │
└──────────────────────────────────────────────────────────────────────┘
```

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
| Host parse of tensors | transient during lazy load | first use of that variant | single-threaded load under mutex |
| **Device weight buffers** | **`DemosaicNetModelCache` (RAW module)** | **lazy → process** | **immutable after load; many readers OK** |
| Layer descriptors | weight pack | same as weights | immutable |
| Graph forward | free functions / stateless runners | N/A | reentrant |
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

**Rule:** the model cache **never** mutates weights after load. Forward is a pure
function of `(weights, input, stream, workspace)`.

### 3.6 Lazy model cache API (RAW-scoped design target)

Not a platform device context. A small domain service owned by the RAW module.

```text
// Sketch only — names may adjust to local style.

enum class DemosaicNetVariant { Bayer, XTrans };

struct DemosaicNetLoadOptions {
  std::filesystem::path model_dir;  // contains bayer.safetensors, xtrans.safetensors
  // optional: prefer async H2D on a given stream
};

class DemosaicNetModelCache {
 public:
  // Module-local singleton or process-static is fine; do not market it as
  // “GPU backend context.” Prefer free functions + static cache if that reads
  // clearer next to CUDA::RcdWorkspace style.
  static auto Instance() -> DemosaicNetModelCache&;

  // Lazy: no-op if already loaded. Thread-safe. Loads only the requested variant.
  auto EnsureLoaded(DemosaicNetVariant variant,
                    const DemosaicNetLoadOptions& options = {}) -> bool;

  [[nodiscard]] auto IsLoaded(DemosaicNetVariant) const -> bool;
  [[nodiscard]] auto LastError() const -> std::string;
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;  // owned accounting

  [[nodiscard]] auto BayerWeights() const -> const DemosaicNetBayerWeights&;
  [[nodiscard]] auto XTransWeights() const -> const DemosaicNetXTransWeights&;

  // Optional: free weights under memory pressure (product can call later).
  // void Unload(DemosaicNetVariant);
  // void UnloadAll();
};
```

Runner sketch (domain module; **does not own weights**, **does not load files**):

```text
void BayerDemosaicNetForward(const DemosaicNetBayerWeights& w,
                             DeviceTensor in,   // NCHW mosaic
                             DeviceTensor out,  // NCHW RGB
                             WorkspacePool& workspace,
                             cudaStream_t stream);

void XTransDemosaicNetForward(...);
```

GpuMat operator sketch (RAW pipeline edge):

```text
// Ensures the needed variant is loaded (lazy), then runs forward.
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

For full-resolution RAW, **tile** the image (Phase 7):

- Tile spatial size so peak activations fit a budget (512² / 1024² — profile).
- Valid conv stacks need halo = receptive-field border, **or** overlapping
  tiles + output crop. Document the scheme in code + tests.
- Bayer pack requires even tile dims.

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

Domain API (`EnsureLoaded`, weights view, Forward, GpuMat-ish edge) should be
**backend-agnostic at the RAW policy layer**. v1 implements CUDA only. Later:

- Same safetensors + host manifests.
- Backend-specific device weight upload and kernels.
- Same lazy cache policy (load on first use of that backend+variant).
- Do **not** hang this off `OpenClContext` as “another program”; treat it as RAW
  domain state that *uses* the platform context for device/queue access.

---

## 4. Execution plan (for the next LLM / implementer)

Work **phase by phase**. Each phase ends with green `MlOpsTest` (and any new
binaries). Do not start RAW pipeline wiring before Phase 5 model runners pass
numerical tests. Do not claim “ready for product” before Phase 6 GpuMat entry
exists. Do **not** block Phase 4–6 on full pipeline workspace migration (§3.10).

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

**Tests:** correctness, structural compose, layout round-trip, bandwidth smoke. ✅

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

### Phase 4 — Generic safetensors IO (`cuda/nn` only)

**Scope boundary:** host/device **file format** support only. No DemosaicNet
graph, no RAW model cache, no pipeline migration.

**Implement:**

1. **Safetensors reader** (header JSON + raw tensor blobs), little-endian F32.
   - Minimal in-tree parser (header size `uint64` + JSON + data offset).
   - Host tensors by key: dtype, shape, contiguous `float` bytes.
2. **Shape / dtype validation helpers** (generic).
3. **Device upload helper:** host tensor → `DeviceBuffer` / `DeviceTensor`
   (stream-aware).
4. **Deduplicate** ad-hoc parsers in `conv2d_test.cu` /
   `conv_transpose2d_test.cu` if cheap in the same PR.

**Files:**

```text
include/cuda/nn/safetensors.hpp
cuda/nn/safetensors.cpp
```

**Tests:** load both real models’ keys/shapes; reject corrupt files; upload
round-trip bit-identical.

**Exit:** library loads packaged tensors without domain semantics. `MlOpsTest`
green.

**Non-goals:** model forward, model cache, GpuMat demosaic, pipeline scratch
migration.

### Phase 5 — Domain runners + lazy model cache (`decoders/processor/nn`)

**Scope boundary:** DemosaicNet as a **RAW-owned neural submodule**. Uses
Phase 1–4 primitives. Still no full `RawProcessor` product wiring (Phase 6).

**Implement:**

1. **`DemosaicNetBayerWeights` / `DemosaicNetXTransWeights`**
   - Own device buffers; immutable after construction.
   - Built from Phase 4 reader + one H2D pass.
2. **`DemosaicNetModelCache`** (lazy, RAW-scoped)
   - `EnsureLoaded(variant)` — **only** load path; mutex / call_once per variant.
   - `IsLoaded` / `ResidentWeightBytes` / weight accessors.
   - **Does not** own a process-global inference `WorkspacePool` in v1.
   - **Does not** load at construction of the cache object (cold by default).
3. **Runners** Bayer / XTrans Forward
   - Graph as §2.1 / §2.2; signature: const weights + workspace + stream.
4. **Peak workspace estimator** (H,W / tile) → `Reserve` before forward.
5. **CMake:** link into RAW / existing CUDA targets; headers under
   `include/decoders/processor/nn/`.

**Tests:**

- Cold cache: `IsLoaded == false` until `EnsureLoaded` or first forward path.
- Lazy load both variants independently (Bayer load must not force XTrans).
- End-to-end forward vs reference (Python dump preferred; tiny CPU graph
  fallback for 32×32).
- Golden: fixed seed mosaic stats / corner pixels.
- **No reload:** two sequential forwards → identical weight device pointers.
- **Concurrency smoke:** two threads, two `WorkspacePool`s, shared cache, small
  tensors; both succeed.

**Exit:** forwards match reference; cache is lazy and RAW-scoped; no platform
“context” type; domain code under `decoders/processor/nn/`.

### Phase 6 — RAW GpuMat entry + product selection + lazy trigger

**Goals:**

- GpuMat CFA/mosaic → RGB entry for RAW demosaic stage.
- Classical remains default; NN is opt-in.
- Load triggers only when NN path is actually taken.

**Implement:**

1. `cuda_demosaicnet.hpp/.cu`:
   - CFA → demosaicnet 3ch mosaic (pattern phase; document training convention).
   - `EnsureLoaded` for needed variant, then Forward + layout convert.
2. `RawParams` demosaic mode, e.g.
   `enum class RawDemosaicMethod { Classical, DemosaicNet };` default Classical.
3. Branch in `raw_processor_cuda.cpp` only when:
   - method is DemosaicNet, **and**
   - decode policy allows it (recommend FULL / high quality only;
     **thumbnails stay Classical**), **and**
   - `EnsureLoaded` succeeds (else Classical fallback).
4. **No** app-main / image-backend eager init hook for weights.
5. Even dimensions / crop policy for odd RAW sizes.

**Tests:**

- Smoke on real RAW from `tests/resources` (GPU only).
- Flag off → Classical behavior unchanged; cache stays cold.
- Flag on first time → load once; second image reuses pointers.
- Load failure → Classical fallback.
- Optional PSNR vs classical (not a hard gate).

**Exit:** documented GpuMat API; `RawProcessor` never opens safetensors in its
constructor; first NN use pays load cost once.

### Phase 7 — Tiling, workspace policy, parallel readiness

Only after Phase 5–6 correctness:

1. Tile + halo policy for large full-res frames; document RF math.
2. Steady-state: `Reserve(peak_for_tile)`; zero `cudaMalloc` in hot loop.
3. Optional: small free-list of workspaces for concurrent NN (mutex on
   borrow/return only)—**not** on the model cache hot path.
4. Confirm thumbnails never touch NN workspace.
5. Expose `ResidentWeightBytes` + workspace capacity in debug logs (prefer owned
   counters over sole reliance on `cudaMemGetInfo`).

**Exit:** large-image path documented; concurrent NN safe by construction.

### Phase 8 — Optimization pass (after correctness)

1. Nsight Compute: top kernels.
2. Fuse `ConvBiasRelu` everywhere applicable.
3. Tune tile size / workspace reuse.
4. Winograd / better 3×3 only if conv dominates.
5. Optional friendlier device weight layout (still lazy-load from safetensors).
6. Record ≤1K-edge interactive latency numbers in this doc or a follow-up note.

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
   - demosaicnet `modules.py` forward
   - `raw_processor.hpp` + `raw_processor_cuda.cpp`
   - Scratch inventory samples: `gpu_scheduler.cuh`, `tone_mapping.cuh`
     (`EnsurePyramidBuffers`), `cuda_debayer_rcd.hpp` (`RcdWorkspace`),
     `cuda_highlight_reconstruct.hpp` (`HighlightWorkspace`)
2. **Phase 0–3:** already done.
3. **Phase 4:** generic safetensors in `cuda/nn`.
4. **Phase 5:** domain weight packs + **lazy** `DemosaicNetModelCache` + runners
   + goldens + no-reload / dual-workspace concurrency smoke.
5. **Phase 6:** GpuMat entry + RawParams + **lazy trigger** (no backend boot
   load); thumbnails Classical.
6. **Phase 7:** tiling + workspace policy.
7. **Phase 8:** profile-driven optimization.
8. **Phase 9 (optional):** broader workspace migration when prioritized.

Do **not** skip golden tests. Wrong pad/crop/group looks “plausible” but is
useless.

Do **not** implement model runners inside `cuda/nn`. Domain placement is
intentional.

Do **not** introduce a DemosaicNet type that pretends to be a platform GPU
context.

---

## 6. Testing and CMake conventions

- Primary suite: **`MlOpsTest`** for ops, safetensors, model cache, goldens.
- RAW smoke after Phase 6: `tests/raw/` if that matches existing CUDA RAW tests.
- Build:

  ```bat
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MlOpsTest --parallel 4
  build\debug\alcedo_studio\tests\MlOpsTest.exe
  ```

- Link: `GTest::gtest_main`, `CudaUtils`, `CUDA::cudart`, OpenCV; domain tests
  link the library that owns `decoders/processor/nn`.
  **Do not** add `CUDA::cublas` / cuDNN.
- Guard with `ALCEDO_CUDA_ENABLED` / `HAVE_CUDA`.
- Model path: `ALCEDO_DEMOASICNET_MODEL_DIR` and/or POST_BUILD copy; same helper
  for tests and production lazy load.

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
12. **Immutable shared weights; private workspaces** for concurrency.
13. Thumbnails / parallel decode default to **Classical**.
14. Fail soft to Classical if load fails.
15. Do **not** name domain types `*Context` in a way that confuses them with
    OpenCL/Metal device contexts.
16. Prefer **owned byte counters** (`ResidentWeightBytes`,
    `WorkspacePool::capacity_bytes`) over inventing another `cudaMemGetInfo`-only
    diagnostic for this subsystem.

---

## 8. Non-goals (explicit)

- Training / backward / autograd.
- General ONNX graph runtime.
- Arbitrary activations / Normalizations / Attention.
- FP16 / TF32 mixed precision (possible later; not required for v1).
- Shipping PyTorch or Python at runtime.
- Replacing classical demosaic as the default until quality/perf gates pass.
- Reloading or cloning full weight packs per `RawProcessor`.
- Putting CFA / LibRaw concerns into `cuda/nn`.
- Modeling DemosaicNet as a peer of `OpenClContext` / `MetalContext`.
- Eager weight load when the user never selects NN demosaic.
- Big-bang rewrite of all pipeline scratch into `WorkspacePool` inside the first
  demosaicnet PR (tracked as Phase 9 / §3.10).
- Process-global mutable activation workspace without a lease protocol.

---

## 9. Acceptance criteria for “DemosaicNet path done”

1. Bayer and XTrans models **lazy-load once per variant** into a RAW-scoped
   model cache and run entirely on CUDA.
2. Forwards match a frozen reference within test tolerances.
3. `MlOpsTest` covers primitives in §2.3, lazy load, and no-reload behavior.
4. Domain code lives under `decoders/processor/nn/` (+ GpuMat operator); generic
   ops stay in `cuda/nn/`.
5. GpuMat in → GpuMat out with no mandatory host intermediate.
6. Steady-state forward does not `cudaMalloc` after workspace reserve.
7. Concurrent forwards with distinct workspaces are defined and smoke-tested.
8. Tile + halo policy documented (Phase 7).
9. `RawProcessor` construction does not touch weight files; cold cache until
   first NN use.
10. Users who never select NN demosaic do not pay model load cost at app start.
11. Interactive latency for a ≤1K-edge tile recorded after Phase 6/8 profile.

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
| Domain NN (planned) | `include/decoders/processor/nn/`, `decoders/processor/nn/` |
| GpuMat demosaic op (planned) | `decoders/processor/operators/gpu/cuda_demosaicnet.*` |
| Tests | `alcedo_studio/tests/ml_ops/` |
| PyTorch modules | https://github.com/mgharbi/demosaicnet/blob/master/demosaicnet/modules.py |
| Platform GPU contexts (contrast only) | `include/opencl/opencl_context.hpp`, `include/metal/metal_context.hpp` |
| Pipeline scratch launcher | `include/edit/pipeline/gpu_scheduler.cuh` |
| LLF pyramid allocs | `include/edit/operators/GPU_kernels/tone_mapping.cuh` |
| RCD / highlight workspaces | `operators/gpu/cuda_debayer_rcd.hpp`, `cuda_highlight_reconstruct.hpp` |
| Preview scratch reclaim | `pipeline_cpu.hpp` `ReleasePreviewGpuScratch`, `cuda_preview_vram_reclamation_test.cu` |
| RAW CUDA demosaic call sites | `decoders/processor/raw_processor_cuda.cpp` |
| Workspace non-thread-safety | `include/cuda/nn/workspace.hpp` |

---

## 11. Suggested commit / PR slices

1. DeviceBuffer + WorkspacePool + Mul/Concat/Crop/Slice/Layout  ✅  
2. Conv2d (+ fused Bias/ReLU) + layer weight tests  ✅  
3. ConvTranspose2d unpack specialization  ✅  
4. **Generic safetensors IO in `cuda/nn` + tests** (Phase 4)  
5. **Domain weight packs + lazy model cache + Bayer/XTrans runners + goldens**
   (Phase 5)  
6. **GpuMat entry + RawParams + lazy trigger (no boot load)** (Phase 6)  
7. **Tiling + workspace policy** (Phase 7)  
8. **Perf pass** (Phase 8)  
9. **Optional:** pipeline workspace migration PR series (Phase 9 / §3.10)

Each demosaicnet PR must leave `MlOpsTest` green and must not reintroduce
per-processor weight reload or a fake platform “context.”

---

## 12. Design decision record

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Where does DemosaicNet live? | `decoders/processor/nn/` only | Domain demosaic; will grow OpenCL/Metal; not a platform GPU service |
| Analogy to OpenCL/Metal context? | **No** | Those manage device/queue/programs; this only caches domain weights |
| Weight load policy | **Lazy** on first NN use | Users who never enable NN pay nothing at boot; still reuse after first load |
| Weight lifetime after load | Process (until optional Unload) | Avoid reload across images / edit sessions |
| Who owns activations? | Caller `WorkspacePool` | Pool is not thread-safe; activations reclaimable without unloading weights |
| Who owns weights? | RAW model cache | Shared, immutable, concurrent readers OK |
| Thumbnail NN? | No by default | Parallel thumb path stays Classical |
| Is “some context” still needed? | Yes — as **workspace/scratch discipline**, not as DemosaicNet-as-context | Inventory of scattered RAII scratch (§1.3); `WorkspacePool` is the seed |
| Pipeline scratch unification | Separate Phase 9 track | Do not block demosaicnet goldens on big-bang migration |
| VRAM observability | Prefer owned byte counters; `cudaMemGetInfo` residual only | Matches how free VRAM is logged today but adds subsystem truth |
| Safetensors code location | Generic parse in `cuda/nn`; domain manifests at weight-pack load | Parser reusable; shape tables domain-specific |
