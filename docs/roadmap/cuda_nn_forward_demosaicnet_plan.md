# CUDA CNN Forward Framework + DemosaicNet Plan

Date: 2026-07-09

Status: Phase 0 partial (ReLU + `DeviceTensor` + `MlOpsTest` landed on
`feature/xtrans_improve`). Remaining phases are ready for implementation by the
next agent.

This document is the handoff plan for a **forward-only, CNN-focused CUDA
inference mini-framework** under `alcedo_studio/src/cuda/nn/`, sufficient to run
the two bundled demosaicnet weights:

- `alcedo_studio/src/config/models/bayer.safetensors`
- `alcedo_studio/src/config/models/xtrans.safetensors`

Metadata on both files: `format = demosaicnet-pytorch-state_dict`.

The framework must be efficient enough to sit inside the RAW / edit GPU
pipeline: intermediate results stay on device, prefer zero-copy with
`cv::cuda::GpuMat` at image boundaries, and avoid layout thrashing.

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

### 1.2 What already landed (Phase 0 partial)

| Item | Location |
|------|----------|
| `DeviceTensor` (non-owning, rank ≤ 8, f32, strides) | `include/cuda/nn/tensor.hpp` |
| Launch / error helpers | `include/cuda/nn/common.hpp` |
| Optimized ReLU (float4 + strided + pitched GpuMat) | `include/cuda/nn/relu.hpp`, `cuda/nn/relu.cu` |
| Test suite scaffold | `tests/ml_ops/relu_test.cu`, CMake target `MlOpsTest` |
| Linked into | `CudaUtils` library |

Measured ReLU contiguous bandwidth on the author’s machine: ~220–250 GB/s
(out-of-place and inplace). Treat that as the performance floor for future
elementwise ops.

### 1.3 Pipeline constraints (non-negotiable)

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

### 1.4 Namespace and file layout convention

```text
alcedo_studio/src/include/cuda/nn/     # public headers
alcedo_studio/src/cuda/nn/             # .cu implementations
alcedo_studio/tests/ml_ops/            # operator + model tests (MlOpsTest)
```

Namespace: `alcedo::cuda::nn`.

Library: keep growing `CudaUtils` until it becomes large enough to split
(`CudaNn`); do not create a second CUDA static lib without need.

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
| 9 | **Safetensors load** | Both | Host parse → device weight buffers. |
| 10 | **Workspace allocator** | Both | Scratch tensors, no per-layer `cudaMalloc` in steady state. |
| 11 | **Model runners** | Both | `BayerDemosaicNet`, `XTransDemosaicNet` orchestration. |

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

### 3.1 Layers

```text
┌─────────────────────────────────────────────────────────────┐
│  Pipeline / RAW stage (future)                              │
│    GpuMat CFA or mosaic RGB  →  DemosaicNetRunner  → GpuMat │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Model runners                                              │
│    BayerDemosaicNet / XTransDemosaicNet                     │
│    owns: device weights, fixed layer descriptors, workspace │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Operators (cuda/nn)                                        │
│    relu, conv2d, conv_transpose2d, mul, concat, crop,       │
│    layout_convert, (fused conv_bias_relu)                   │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│  Core                                                       │
│    DeviceTensor, DeviceBuffer/WorkspacePool, safetensors IO │
│    stream helpers, CUDA error checks                        │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Tensor policy

- **Internal:** contiguous NCHW `DeviceTensor`, `float*`, `cudaMalloc`/`pool`.
- **Boundary:** `DeviceTensor::FromGpuMat` already models pitched HWC; add
  explicit `PackHwcToNchw` / `UnpackNchwToHwc` for model I/O when channels > 1.
- **Weights:** NCHW OIHW as stored in PyTorch (`[out, in/groups, kH, kW]`),
  uploaded once at load; never reordered per frame unless a kernel requires a
  packed variant (then cache the packed copy).

### 3.3 Conv algorithm choices (efficiency)

Priority order for implementation:

1. **1×1 Conv** → pure GEMM (`cublasSgemm` or a tiny hand-written GEMM for small
   M). Shape: `(N*H*W) × Cin` × `Cin × Cout`.
2. **2×2 stride-2 pack** (`pack_mosaick`) → specialized kernel (tiny K, high
   bandwidth). Do not go through general im2col.
3. **3×3 valid, s=1, C≈64** → primary hot path. Prefer:
   - implicit GEMM / tiled shared-memory convolution, **or**
   - im2col into workspace + `cublasSgemm`,
   whichever is simpler to land correctly first; then optimize.
   Winograd F(2×2,3×3) is optional Phase 2+ if profiling shows conv dominates.
4. **ConvTranspose 2×2 s=2 groups=3** → specialized depthwise-ish upsample
   kernel (12→3 with groups=3 is cheap; do not use a general transpose path).
5. **Bias** → fused into epilogue of the same kernel (do not launch a second
   pass).

**Dependency policy:**

- `CUDA::cudart` required (already).
- `CUDA::cublas` allowed for GEMM if it simplifies 1×1 / im2col paths; link only
  when used.
- **Do not** require cuDNN for the first complete demosaicnet path.
- No Torch / ONNX Runtime.

### 3.4 Workspace / memory

`WorkspacePool` (or `NnWorkspace`) requirements:

- Thread- or stream-local reusable device buffers.
- Grow-only capacity for the largest scratch of the current model + tile size.
- API sketch: `auto buf = workspace.Acquire(bytes);` / RAII release in reverse
  order, or bump allocator reset per forward.
- Steady-state forward: **zero** `cudaMalloc` / `cudaFree`.

For full-resolution RAW, **tile** the image:

- Choose tile spatial size so peak activation memory fits a budget (e.g. 512² or
  1024² working tiles — profile).
- For valid conv stacks, each tile needs a **halo** equal to the total
  receptive-field border of that branch, **or** run the network on an
  overlapping padded tile and crop outputs. Document the chosen scheme in code
  comments and tests.
- Bayer pack requires even tile dims.

### 3.5 Numerics

- Match demosaicnet f32 PyTorch forward within a tight tolerance on random and
  real mosaics (suggested: max abs error ≤ 1e-4 on synthetic; ≤ 1e-3 on long
  chains if intermediate rounding differs — lock exact thresholds in tests).
- Deterministic given fixed weights and inputs (no atomic reductions in conv
  accumulation order that differ by launch config if we can avoid them; if GEMM
  nondeterminism appears, document it and compare with relaxed atol).

---

## 4. Execution plan (for the next LLM / implementer)

Work **phase by phase**. Each phase ends with green `MlOpsTest` (and any new
binaries). Do not start pipeline wiring before Phase 4 model runners pass
numerical tests.

### Phase 0 — Core tensor + ReLU  ✅ (partial, done)

**Done:**

- [x] `DeviceTensor`, ReLU, GpuMat overloads, `MlOpsTest` correctness + bandwidth.

**Still do if missing before Phase 1:**

- [ ] `DeviceBuffer` owning wrapper (RAII `cudaMalloc`/`Free`, upload/download)
      under `include/cuda/nn/device_buffer.hpp` (tests currently roll their own;
      promote to library).
- [ ] `WorkspacePool` skeleton (even if only used by later ops).

**Exit:** existing ReLU tests still pass; buffer/pool unit tests added.

### Phase 1 — Elementwise + structural ops

**Implement:**

| Op | Files | Acceptance |
|----|-------|------------|
| `Mul` | `mul.hpp` / `mul.cu` | float4 path; inplace optional |
| `ConcatChannels` | `concat.hpp` / `concat.cu` | only channel axis; output contiguous |
| `SliceChannels` / `SplitChannels` | `slice.hpp` / `slice.cu` | view if contiguous NCHW; else copy |
| `CenterCropSpatial` | `crop.hpp` / `crop.cu` | exact demosaicnet `_crop_like` |
| `PackHwcToNchw` / `UnpackNchwToHwc` | `layout.hpp` / `layout.cu` | GpuMat CV_32FC3 ↔ NCHW |

**Tests (`tests/ml_ops/`):**

- Correctness vs CPU reference for each op (odd sizes, multi-batch N=1 first).
- Concat + crop used together as in Bayer/XTrans skip path.
- Layout convert round-trip max error 0 on finite inputs.
- Bandwidth smoke for Mul (same style as ReLU).

**Exit:** all new tests in `MlOpsTest` green.

### Phase 2 — Conv2d + bias (+ fuse ReLU)

**Implement:**

- Generic API:

  ```cpp
  struct Conv2dParams {
    int in_channels, out_channels;
    int kH, kW, sH, sW, padH, padW;  // pad always 0 for demosaicnet
    int dilation = 1;
    int groups = 1;
    const float* weight;  // OIHW device
    const float* bias;    // optional, length out_channels
  };

  void Conv2d(const DeviceTensor& in, DeviceTensor& out,
              const Conv2dParams& p, cudaStream_t stream);
  void Conv2dBiasRelu(...);  // fused epilogue
  ```

- Specialized fast paths:
  1. `k=1,s=1` GEMM
  2. `k=2,s=2,pad=0` (pack)
  3. `k=3,s=1,pad=0` main path
- Output spatial size formula (validate in tests):

  ```text
  H_out = floor((H + 2*padH - d*(kH-1) - 1) / sH) + 1
  ```

**Tests:**

- Compare against a simple CPU conv reference (small shapes).
- Layer-level tests loading real weights:
  - `pack_mosaick`, `conv1`, `output` (1×1), `post_conv1` (6→64 and 67→64).
- Fused vs unfused `Conv+Bias+ReLU` bitwise or near-equality.
- Performance: 3×3, C=64, H=W=512, report ms and GFLOPs/s; fail only on
  catastrophic regressions (set a soft floor after first measurement on CI GPU).

**Exit:** conv unit tests green; no `cudaMalloc` inside hot path when workspace
is provided.

### Phase 3 — ConvTranspose2d (Bayer unpack)

**Implement:**

- API parallel to Conv2d.
- **Required specialization:** `in=12, out=3, k=2, s=2, groups=3` matching
  `unpack_mosaick`.
- Output size (PyTorch default `output_padding=0`):

  ```text
  H_out = (H - 1)*sH - 2*padH + d*(kH-1) + output_padding + 1
  ```

**Tests:**

- CPU reference for grouped transpose on tiny tensors.
- Load `unpack_mosaick` weights; compare one forward block.

**Exit:** green tests; used only by Bayer runner later.

### Phase 4 — Safetensors + model runners

**Implement:**

1. **Safetensors reader** (header JSON + raw tensor blobs), little-endian F32.
   - Prefer a minimal in-tree parser (header size `uint64` + JSON + data offset).
   - Validate dtype F32 and shapes against an expected manifest per variant.
2. **`DemosaicNetWeights`** holding device pointers + host metadata.
3. **`BayerDemosaicNet::Forward(in, out, stream, workspace)`**
4. **`XTransDemosaicNet::Forward(...)`**
5. Graph orchestration using Phase 1–3 ops exactly as §2.1 / §2.2.

**File suggestions:**

```text
include/cuda/nn/safetensors.hpp
include/cuda/nn/demosaicnet.hpp
cuda/nn/safetensors.cpp          # host I/O can be .cpp
cuda/nn/demosaicnet.cu           # or .cpp calling ops
```

**Tests:**

- Load both model files from
  `alcedo_studio/src/config/models/{bayer,xtrans}.safetensors`
  (path via compile definition or test resource copy).
- End-to-end forward on a **synthetic mosaic** vs a **reference**:
  - Preferred: dump reference tensors from a one-off Python demosaicnet script
    checked into `tests/ml_ops/reference/` (npy or raw f32 + json shape).
  - Minimum: CPU naive implementation of the same graph for a very small
    spatial size (e.g. 32×32) if Python reference is not yet available.
- Golden test: fixed seed mosaic, hash or file compare of output stats (mean,
  std, few corner pixels).

**Exit:** Bayer and XTrans forward match reference within agreed atol/rtol.

### Phase 5 — Pipeline integration (image path)

**Goals:**

- Entry point usable from RAW demosaic stage **or** a dedicated operator.
- Input: `cv::cuda::GpuMat` mosaic / CFA-derived RGB mosaic, f32.
- Output: `cv::cuda::GpuMat` RGB f32, same device/stream.
- Tiling for large images; workspace owned by the operator or a service.

**Suggested wiring (decide one; prefer the shallowest for first land):**

1. New GPU helper under `decoders/processor/operators/gpu/` e.g.
   `cuda_demosaicnet.hpp/.cu` that calls `cuda::nn::BayerDemosaicNet`, **or**
2. Optional branch inside existing demosaic selection once quality is validated.

**Must handle:**

- Bayer pattern phase (RGGB/GRBG/…) when packing the 3-channel mosaic the way
  demosaicnet expects (match training convention; document it).
- X-Trans pattern phase similarly.
- Even dimensions / crop policy for odd RAW sizes.
- Performance budget: aim for interactive preview tiles first; full-res can be
  async / lower priority.

**Tests:**

- Smoke on a real RAW sample from `tests/resources` (GPU only).
- Optional: visual / PSNR vs classical demosaic (not a hard gate).

**Exit:** one documented API that demosaics a GpuMat end-to-end with bundled
weights.

### Phase 6 — Optimization pass (after correctness)

Only after Phase 4–5 are correct:

1. Profile with Nsight Compute: top kernels by time.
2. Ensure `ConvBiasRelu` fused everywhere applicable.
3. Tune tile size and workspace reuse.
4. Consider Winograd or better 3×3 kernel if conv ≫ 70% of time.
5. Persist weights in a friendlier device layout if needed (still load from
   safetensors).
6. Multi-stream overlap (H2D weights already done; optional concurrent tiles).

---

## 5. Concrete first tasks (ordered for the next agent)

If starting immediately, execute in this order:

1. **Read** this doc + existing:
   - `include/cuda/nn/{tensor,relu,common}.hpp`
   - `cuda/nn/relu.cu`
   - `tests/ml_ops/relu_test.cu`
   - demosaicnet `modules.py` forward (linked in §2)
2. **Phase 0 cleanup:** promote `DeviceBuffer` + `WorkspacePool`.
3. **Phase 1:** Mul, Concat, Slice, CenterCrop, layout convert + tests.
4. **Phase 2:** Conv2dBias / Conv2dBiasRelu with 1×1 and 3×3 paths + tests
   against CPU and against real layer weights.
5. **Phase 3:** specialized `unpack_mosaick` transpose.
6. **Phase 4:** safetensors + full Bayer/XTrans runners + golden tests.
7. **Phase 5:** GpuMat pipeline entry + tiling.
8. **Phase 6:** profile-driven optimization.

Do **not** skip golden tests for “later”. Wrong pad/crop/group on transpose will
look “plausible” but is useless.

---

## 6. Testing and CMake conventions

- Single suite binary: **`MlOpsTest`** (already registered under
  `alcedo_tests_gpu`).
- Add sources as `tests/ml_ops/<op>_test.cu` or keep expanding `relu_test.cu`
  only while small; prefer one file per op once Phase 1 lands.
- Build:

  ```bat
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MlOpsTest --parallel 4
  build\debug\alcedo_studio\tests\MlOpsTest.exe
  ```

- Link: `GTest::gtest_main`, `CudaUtils`, `CUDA::cudart`, OpenCV as today.
  Add `CUDA::cublas` when GEMM is introduced.
- Guard with `ALCEDO_CUDA_ENABLED` (already).
- Model path: either
  `ALCEDO_DEMOASICNET_MODEL_DIR` compile definition pointing at
  `src/config/models`, or copy safetensors next to the test binary in CMake
  `POST_BUILD`.

---

## 7. Design rules for implementers

1. **Match demosaicnet math first**, optimize second.
2. **pad=0** for all pretrained 3×3 layers; do not “fix” borders with same-pad.
3. **groups=3** on Bayer upsample is mandatory (weight layout
   `[12,1,2,2]`).
4. Every new op: host CPU reference test + device test + stream parameter.
5. No intermediate H2D/D2H in model forward.
6. No per-layer alloc in steady state.
7. Prefer extending `CudaUtils` over new libraries.
8. Follow existing CUDA style in-repo (`CheckCuda` / exceptions, not
   `exit()`-style `CUDA_CHECK` in library code).
9. Private members use trailing `_` (project clang-tidy rule).
10. License header GPLv3 as in other sources.

---

## 8. Non-goals (explicit)

- Training / backward / autograd.
- General ONNX graph runtime.
- Arbitrary activations / Normalizations / Attention.
- FP16 / TF32 mixed precision (possible later; not required for v1).
- Shipping PyTorch or Python at runtime.
- Replacing classical demosaic until quality/perf gates pass (Phase 5+).

---

## 9. Acceptance criteria for “framework done”

The mini-framework is considered complete for demosaicnet when:

1. Both safetensors models load and run entirely on CUDA.
2. Bayer and XTrans forwards match a frozen reference within test tolerances.
3. `MlOpsTest` covers every primitive in §2.3.
4. A GpuMat in → GpuMat out path exists with no mandatory host intermediate.
5. Steady-state forward does not call `cudaMalloc`.
6. Documented tile + halo policy for large images.
7. Performance: full preview tile (e.g. ≤ 1K edge) completes at a latency
   acceptable for interactive use on a mid-range NVIDIA GPU; exact ms budget to
   be filled after first Phase 5 profile (record numbers in this doc or a
   follow-up note).

---

## 10. Reference index

| Resource | Path / URL |
|----------|------------|
| Bayer weights | `alcedo_studio/src/config/models/bayer.safetensors` |
| XTrans weights | `alcedo_studio/src/config/models/xtrans.safetensors` |
| NN headers | `alcedo_studio/src/include/cuda/nn/` |
| NN sources | `alcedo_studio/src/cuda/nn/` |
| Tests | `alcedo_studio/tests/ml_ops/` |
| PyTorch modules | https://github.com/mgharbi/demosaicnet/blob/master/demosaicnet/modules.py |
| Project CUDA style sample | `alcedo_studio/src/cuda/nn/relu.cu`, `edit/operators/geometry/cuda_geometry_ops.cu` |
| GpuMat usage in RAW | `decoders/processor/raw_processor_cuda.cpp` |

---

## 11. Suggested commit / PR slices

Keep PRs reviewable:

1. DeviceBuffer + WorkspacePool + Mul/Concat/Crop/Slice/Layout  
2. Conv2d (+ fused Bias/ReLU) + layer weight tests  
3. ConvTranspose2d unpack specialization  
4. Safetensors + Bayer/XTrans runners + golden tests  
5. Pipeline GpuMat entry + tiling  
6. Perf pass  

Each PR must leave `MlOpsTest` green.
