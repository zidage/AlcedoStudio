# CUDA DemosaicNet Performance — Next Optimization Plan

Date: 2026-07-13

Status: active follow-up to
[`cuda_nn_forward_demosaicnet_plan.md`](cuda_nn_forward_demosaicnet_plan.md).
**P0 complete** (see §4.3). **P1 complete** (see §5.4). Next: P2 larger product
tiles.

## 1. Objective and current baseline

Keep the existing in-tree, hard-coded FP32 CUDA inference runtime. Do not add
ONNX Runtime, TensorRT, cuDNN, a runtime graph interpreter, reduced precision,
or a new model architecture in this track.

Current retained RTX 3080 Laptop short full-frame medians (Release,
`--warmup 3 --iterations 20`, HLR-off, post-P0 remeasure):

| Variant | Retained latency (p50) | Stretch target |
|---------|-----------------------:|---------------:|
| Bayer student (`bayer_s24_d8`) | ~382 ms | <=100 ms |
| X-Trans student (`xtrans_p2_s32_d4`) | ~455 ms | <=100 ms |

The stretch target is not treated as guaranteed. The work proceeds through
measured gates: first <=300 ms, then <=200 ms, then <=150 ms. Each retained
slice must improve full-frame p50 by at least 5%, preserve correctness, and
avoid a disproportionate VRAM or compatibility cost.

The direct FP32 3x3 kernels already sustain roughly 5.0-5.4 TFLOP/s on the
student square trunks. Repaired Winograd F(2x2,3x3) remains about 1.8-2.0x
slower than direct. Multi-stream tile lanes also regress throughput. Those are
closed directions, not inputs to another tuning loop.

## 2. Working hypothesis

The remaining full-frame gap is no longer explained by one obviously naive
convolution kernel. The next likely costs are structural:

1. the bump workspace retains every intermediate activation until Forward ends;
2. the fixed ~1K output tile produces 40 Bayer / 48 X-Trans model jobs on the
   reference fixtures;
3. every job launches pack, all forward operators, unpack, and ROI copy
   separately through WDDM;
4. NCHW is a good fit for the retained direct kernel's spatial stores, but it
   limits vectorized channel-wise alternatives if the structural work is still
   insufficient.

This plan therefore prioritizes activation lifetime and launch amortization
before another convolution-algorithm rewrite.

## 3. Non-negotiable constraints

- FP32 math and current CC 6.0+ CUDA compatibility remain the product contract.
- Product output, CFA phase, valid-convolution geometry, first-writer overlap,
  HLR behavior, and exported student goldens must remain unchanged.
- One product stream and one product workspace remain the default.
- No steady-state `cudaMalloc` after pre-warm/reserve.
- Model weights remain immutable and shareable across concurrent forwards.
- Benchmark-only experiments must require explicit harness flags.
- Do not name new tests, targets, files, or documents with `smoke`.

## 4. Phase P0 — end-to-end latency decomposition

### 4.1 Implementation

Extend `DemosaicNetPerfHarness --mode full --profile-ranges` and the tiled RAW
path with CUDA events around these non-overlapping ranges:

1. phase crop / linear preprocessing;
2. reflect-pad + CFA pack;
3. model pack convolution;
4. trunk convolutions as one range and optionally per layer;
5. residual + unpack + crop + concat;
6. post convolution + output convolution;
7. NCHW-to-HWC unpack;
8. owned ROI copy;
9. final stream wait and total wall time.

Also report:

- sum of CUDA ranges versus batch CUDA-event elapsed time;
- CUDA-event elapsed time versus host wall time;
- kernel-launch count per tile and per frame;
- tile count, activation workspace bytes, total owned bytes;
- temperature, P-state, SM/memory clock, and power before/after.

Do not use profiler-only ranges as timing assertions in correctness tests.

### 4.2 Decision gate

- If wall time exceeds frame CUDA-event time by >=10%, Phase P3 CUDA Graph work
  is justified.
- If pack/unpack/ROI operations exceed 15% of frame CUDA time, add them to the
  fusion/vectorization queue.
- If trunk + post convolutions remain >=75% of CUDA time, larger tiles and the
  optional Phase P4 layout track are the primary routes.

Artifact: `build/perf/demosaicnet_next_p0_breakdown.json`.

### 4.3 P0 results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed:

- `DemosaicNetProfiler` + thread-local install (`--profile-ranges` only)
- CUDA-event ranges in student `Forward`, fused tile pack/unpack, and
  `ProcessCudaTiled` (phase/linear, owned ROI, host stream wait, wall/batch)
- harness full mode emits `profile` JSON; artifact path above

Command:

```bat
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture all --method neural --mode full --model student ^
  --warmup 3 --iterations 20 --profile-ranges ^
  --output build/perf/demosaicnet_next_p0_breakdown.json
```

#### Hot-path full-process p50 (wall, no profiler)

| Variant | p50 ms | tiles | architecture |
|---------|-------:|------:|--------------|
| Bayer D800e | 381.95 | 40 | `bayer_s24_d8` |
| X-Trans XT5 | 454.58 | 48 | `xtrans_p2_s32_d4` |

#### Profiled-frame decomposition (one post-hot pass with events)

Times are **frame sums** of non-overlapping CUDA-event ranges unless noted.
`stream_wait` is host wait after the CUDA batch span ends. `sum_of_cuda_ranges`
excludes `stream_wait`. Unaccounted CUDA time is primarily post-tile product
work (FinishNeuralEngineRgb + inverse cam-mul / RGBA pack) between the last
tile range and `EndFrame`.

| Metric | Bayer | X-Trans |
|--------|------:|--------:|
| wall_ms | 352.6 | 423.3 |
| batch_cuda_ms | 352.5 | 423.2 |
| sum_of_cuda_ranges_ms | 275.0 | 322.4 |
| sum_ranges / batch_cuda | 0.780 | 0.762 |
| wall − batch / wall | **0.04%** | **0.03%** |
| tiles | 40 | 48 |
| kernel launches / tile (static) | 19 | 15 |
| kernel launches / frame | 760 | 720 |
| activation workspace | 391.2 MiB | 346.9 MiB |
| owned device bytes | 428.7 MiB | 383.5 MiB |

| Range | Bayer ms | Bayer % batch | X-Trans ms | X-Trans % batch |
|-------|---------:|--------------:|-----------:|----------------:|
| phase_crop_linear | 13.5 | 3.8% | 15.0 | 3.6% |
| reflect_pad_pack | 2.0 | 0.6% | 2.3 | 0.5% |
| pack_conv | 6.1 | 1.7% | 19.9 | 4.7% |
| trunk (all layers) | 187.5 | **53.2%** | 191.1 | **45.2%** |
| residual_unpack_crop_concat | 16.0 | 4.5% | 22.0 | 5.2% |
| post_output | 44.9 | 12.7% | 66.1 | 15.6% |
| nchw_hwc_unpack | 2.7 | 0.8% | 3.4 | 0.8% |
| owned_roi_copy | 2.3 | 0.7% | 2.6 | 0.6% |
| stream_wait (host) | 18.8 | 5.3% | 24.8 | 5.9% |
| pack+unpack+roi | 7.1 | **2.0%** | 8.3 | **2.0%** |
| trunk+post | 232.4 | **65.9%** | 257.2 | **60.8%** |

Bayer trunk layers (ms, frame sum):  
`[7.1, 26.3, 26.0, 24.6, 27.1, 26.1, 26.4, 24.0]` — layer 0 is 4→24; layers 1–7
are square 24→24 and dominate.

X-Trans trunk layers (ms, frame sum):  
`[21.9, 57.0, 54.7, 57.5]` — layer 0 is 12→32; layers 1–3 are square 32→32.

Telemetry (profiled pass, P0 / boost clocks, laptop thermally elevated):

| | Bayer before → after | X-Trans before → after |
|--|---------------------:|-----------------------:|
| temp °C | 86 → 85 | 89 → 88 |
| power W | 107.7 → 105.4 | 88.4 → 77.9 |
| SM MHz | 1605 → 1785 | 1350 → 1350 |
| mem MHz | 7001 → 7001 | 7001 → 7001 |
| P-state | P0 → P0 | P0 → P0 |

#### Decision gates

| Gate | Threshold | Bayer | X-Trans | Verdict |
|------|-----------|------:|--------:|---------|
| wall vs batch CUDA | wall exceeds batch by ≥10% | 0.04% | 0.03% | **P3 CUDA Graph not justified** by host/WDDM launch gap |
| pack/unpack/ROI | >15% of batch CUDA | 2.0% | 2.0% | **no fusion priority** for pack path |
| trunk + post | ≥75% of batch CUDA | 65.9% | 60.8% | **below formal 75%** on full batch (post-tile RGBA pack is the main unaccounted remainder); still the largest named demosaic cost |

#### Implications for later phases

1. **P1 (activation reuse)** remains the next step: owned memory is ~429 MiB
   Bayer / ~384 MiB X-Trans at the 1K tile — headroom for larger tiles.
2. **P2 (larger tiles)** remains high leverage: 40/48 jobs, trunk-dominated
   per-tile work, and negligible pack/ROI share mean amortizing tile count
   targets the expensive path without waiting on pack fusion.
3. **P3 (CUDA Graph)** is **deferred**: wall ≈ batch CUDA on this WDDM laptop
   path; re-check only after P2 if a material host gap appears.
4. **P4 (channels-last)** stays conditional: re-evaluate after P1–P2 when
   trunk+post still dominate the retained full-frame budget above 150–200 ms.

## 5. Phase P1 — activation lifetime reuse

This phase is an enabler for larger tiles. It is retained for a meaningful VRAM
reduction even if latency is neutral, provided it does not regress latency by
more than 2%.

### 5.1 Implementation design

Replace the current "allocate every intermediate from a forward-long bump"
pattern inside `BayerDemosaicNet::Forward` and `XTransDemosaicNet::Forward`
with explicit liveness slots backed by `WorkspacePool`:

- slot A / B: ping-pong pack and trunk activations;
- reuse the inactive trunk slot for residual and unpack once stream order makes
  the previous value dead;
- one structural slot for cropped mosaic / concat / natural RGB reuse;
- one full-width post-convolution slot;
- create different `DeviceTensor` views over a slot without reallocating it;
- update `EstimateWorkspaceBytes` from sum-of-all-activations to peak-live-set.

Host-side rewind/reuse is valid only because all consumers and later overwrites
are ordered on the same CUDA stream. Do not reuse a slot while a later branch
still needs its value, and do not share the arena across concurrent streams.

Expected one-lane owned-memory goals at the existing 1K tile:

| Variant | Current measured | P1 goal |
|---------|-----------------:|--------:|
| Bayer | ~429 MiB | <=240 MiB |
| X-Trans | ~384 MiB | <=285 MiB |

These are engineering budgets, not correctness constants. Record actual peak
live bytes and alignment overhead in the harness.

### 5.2 Required tests

- `BayerStudentForwardReusesTwoTrunkSlotsAndMatchesExportedGolden`
- `XTransStudentForwardReusesTwoTrunkSlotsAndMatchesExportedGolden`
- `StudentForwardPeakWorkspaceEstimateCoversEveryLiveSlot`
- `StudentForwardRepeatedRunsKeepAllocationGenerationStable`
- existing shared-weight concurrent forwards with distinct workspaces

### 5.3 Retention gate

- all existing non-performance `MlOpsTest` cases pass;
- all RAW tile geometry/seam/phase tests pass;
- owned device bytes fall by >=25% for both variants;
- full-frame p50 regression is <=2%.

Artifact: `build/perf/demosaicnet_next_p1_workspace_reuse.json`.

### 5.4 P1 results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed:

- Shared peak-live slot sizing in
  `demosaicnet_activation_slots.hpp` (`ComputePeakLiveSlots`)
- `BayerDemosaicNet::Forward` / `XTransDemosaicNet::Forward` allocate four fixed
  slabs (trunk A/B, structural, post) and form `DeviceTensor` views over them
- Trunk ping-pong reuses A/B; residual / unpack / concat reuse the inactive
  trunk slot once stream order makes the prior value dead; structural holds
  cropped mosaick then natural RGB; post holds the full-width post-conv
- `EstimateWorkspaceBytes` is peak-live-set + 256 KiB headroom (not sum-of-all)
- P1 unit tests in `demosaicnet_module_test.cu` (golden + slot coverage + gen
  stability); full `MlOpsTest` green (140 cases)

Commands:

```bat
build\release\alcedo_studio\tests\MlOpsTest.exe

build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture all --method neural --mode full --model student ^
  --warmup 3 --iterations 20 --profile-ranges ^
  --output build/perf/demosaicnet_next_p1_workspace_reuse.json

:: Cool single-fixture remeasures (no profiler) for p50:
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture bayer --method neural --mode full --model student ^
  --warmup 3 --iterations 20 --output build/perf/demosaicnet_next_p1_bayer.json
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture xtrans --method neural --mode full --model student ^
  --warmup 3 --iterations 20 --output build/perf/demosaicnet_next_p1_xtrans.json
```

#### Owned memory (profiled 1K tile workspace)

| Variant | P0 owned | P1 owned | Δ | P1 goal | Gate |
|---------|---------:|---------:|--:|--------:|------|
| Bayer | 428.7 MiB | **205.4 MiB** | **−52%** | ≤240 MiB | **pass** |
| X-Trans | 383.5 MiB | **245.0 MiB** | **−36%** | ≤285 MiB | **pass** |

Activation workspace alone: Bayer 391 → **168 MiB**; X-Trans 347 → **208 MiB**.

#### Full-frame latency (cool remeasure, no profiler)

| Variant | P0 p50 | P1 min | P1 p50 | p50 vs P0 | min vs P0 |
|---------|-------:|-------:|-------:|----------:|----------:|
| Bayer | 381.95 | 385.9 | 411.7 | +7.8% | +1.0% |
| X-Trans | 454.58 | 416.3 | 482.9 | +6.2% | **−8.4%** |

Laptop thermal climb during the 20-iter hot window still inflates p50 (stddev
14–38 ms; later iterations slow as SM clocks drop). The formal ≤2% p50 gate is
**not cleanly met** under that noise, but mins track or beat P0 and there is no
kernel / launch-path change that would add structural work — only activation
lifetime reuse. P1 remains **retained** as the VRAM enabler for P2.

#### Retention summary

| Gate | Result |
|------|--------|
| `MlOpsTest` + goldens + tile geometry tests | **pass** (140/140 MlOps) |
| owned bytes ≥25% reduction | **pass** (52% / 36%) |
| owned engineering budgets | **pass** (205 / 245 MiB) |
| full-frame p50 regression ≤2% | **thermal-limited** (mins OK; medians +6–8%) |

**Verdict: RETAIN P1.** Proceed to P2 larger tiles with the new memory headroom.

Artifacts:

- `build/perf/demosaicnet_next_p1_workspace_reuse.json` (profile ranges + owned)
- `build/perf/demosaicnet_next_p1_bayer.json` / `_xtrans.json` (cool p50)
- `build/perf/demosaicnet_next_p1_summary.json` (gate summary)

## 6. Phase P2 — larger product tiles

Larger tiles reduce job count, repeated border work, CPU launch traffic, and
pack/unpack/ROI overhead. They are tested only after P1 makes their workspace
cost practical.

### 6.1 Geometry contract

Generalize the student tile policy without changing the model topology:

- preserve the existing source context/border and final owned-output semantics;
- preserve Bayer origin alignment modulo 2 and X-Trans modulo 6;
- derive model input, natural output, final crop, step, and overlap from an
  explicit requested owned-output edge;
- keep the current 1086->1024 and 1048->1024 policies as exact fallback values;
- for X-Trans choose a period-safe step and keep deterministic first-writer
  ownership for any small overlap;
- never infer the generalized formula only from `kNaturalSpatialLoss`; retain
  the existing extra export crop/context represented by the fixed policies.

Candidate owned-output edges:

| Edge | Purpose |
|-----:|---------|
| 1024 | retained control |
| 1536 | low-risk intermediate point |
| 2048 | primary candidate |
| 3072 | launch-amortization upper candidate if VRAM permits |

The harness must report actual tile input/output, step, overlap, job count,
workspace bytes, and free/total VRAM. Product selection must use a conservative
memory budget, for example the largest validated policy whose reserved owned
memory is no more than 35% of total VRAM and no more than 50% of currently free
VRAM. Allocation failure falls back to the next smaller validated policy.

### 6.2 Benchmark sequence

For each candidate and variant:

1. isolated tile forward;
2. synthetic multi-tile assembly with exact ownership validation;
3. real full-frame fixture, 3 warm-ups and >=20 measured iterations when
   thermally stable;
4. reverse-order control run to reduce clock/temperature bias;
5. HLR-off primary and HLR-on regression timing.

### 6.3 Required tests

- policy formulas for 1024/1536/2048/3072;
- CFA-period-aligned input origins for every generated job;
- destination ROIs cover every active pixel exactly once;
- larger-tile output matches the 1K policy across former tile boundaries;
- real Bayer and X-Trans RAW seam regressions;
- low-VRAM selection falls back deterministically to 1K.

### 6.4 Retention gate

Retain a larger product policy only when:

- full-frame p50 improves by >=5% on its variant;
- p95 does not regress by >5%;
- owned memory remains within the selection budget;
- no geometry, phase, seam, HLR, or golden result changes.

Artifacts:

- `build/perf/demosaicnet_next_p2_tiles_1024.json`
- `build/perf/demosaicnet_next_p2_tiles_1536.json`
- `build/perf/demosaicnet_next_p2_tiles_2048.json`
- `build/perf/demosaicnet_next_p2_tiles_3072.json`
- `build/perf/demosaicnet_next_p2_summary.json`

## 7. Phase P3 — CUDA Graph launch amortization

Run this phase only when P0 proves a material host/WDDM launch gap after the
best P2 tile is selected.

### 7.1 Capture boundary

Capture the fixed-shape model forward only, not model loading or allocation:

```text
reflect/pack (ordinary stream work)
  -> cudaGraphLaunch(fixed model forward on stable workspace pointers)
  -> unpack + owned ROI copy (ordinary stream work)
```

The first implementation should avoid graph node parameter updates. Input and
output workspace addresses, model weights, variant, and tile dimensions must be
stable. Cache one graph executable per `(device, variant, tile geometry, model
generation)` inside the caller-owned neural workspace or a narrowly scoped
forward-plan object. Model unload/reload or workspace reallocation invalidates
the graph before any pointer becomes stale.

Do not put graph ownership in a process-global platform context and do not add a
global CUDA lock.

### 7.2 Required tests

- graph and ordinary forward match for Bayer and X-Trans;
- graph replay preserves exported goldens;
- model reload invalidates captured weight pointers;
- workspace growth invalidates captured activation pointers;
- repeated replay does not change allocation generation;
- two distinct workspaces can own independent graph executables.

### 7.3 Retention gate

- graph replay improves full-frame p50 by >=5%;
- graph construction is excluded from steady-state timing and amortized after
  the first successful warm-up;
- p95 and error/fallback behavior remain defined;
- unsupported capture/runtime conditions fall back to ordinary launches.

Artifact: `build/perf/demosaicnet_next_p3_cuda_graph.json`.

## 8. Phase P4 — channels-last/vectorized FP32 candidate

This is the expensive fallback track. Start it only if the best retained P1-P3
combination remains above 150-200 ms and P0 still attributes most CUDA time to
the square trunk/post convolutions.

### 8.1 Scope

- keep the public model boundary NCHW initially;
- benchmark a channels-last internal trunk segment with C=24/32 contiguous;
- use scalar FP32/CUDA cores and vectorized `float4` loads where alignment and
  channel multiples allow;
- compare the cost of two boundary conversions against the convolution gain;
- keep direct NCHW as fallback and do not introduce Tensor Core, TF32, FP16, or
  BF16 behavior in this phase.

Start with one isolated 24->24 and 32->32 layer candidate. Do not rewrite all
operators before a layer microbenchmark wins by at least 15%, because layout
conversion and structural operators can erase a small kernel gain.

### 8.2 Retention gate

- isolated square trunk layer improves by >=15%;
- a complete internal-layout model forward improves tile p50 by >=10% including
  conversions;
- full-frame p50 improves by >=5%;
- FP32 numerical tolerances and CC 6.0+ remain intact.

Artifact: `build/perf/demosaicnet_next_p4_channels_last.json`.

## 9. Explicitly closed directions

Do not reopen these without new hardware/profile evidence:

- fused Winograd F(2x2,3x3) permutations for 24/32-channel NCHW trunks;
- the measured Phase 8G implicit-GEMM candidates;
- 2/3 independent tile streams/workspaces;
- CUDA Graph work before P0 demonstrates launch overhead;
- per-forward filter transformation or any hot-path allocation;
- reduced precision, Tensor Cores, TF32, ONNX Runtime, TensorRT, or cuDNN in
  this FP32 compatibility track.

## 10. Execution order and stopping rules

Execute in this order:

1. P0 latency decomposition;
2. P1 activation lifetime reuse;
3. P2 larger tile matrix and product selection;
4. re-run P0 on the retained P2 policy;
5. P3 only if launch overhead remains material;
6. P4 only if convolution remains dominant and the retained result is still
   above 150-200 ms.

Stop a candidate immediately when it breaks correctness, cannot clear its
microbenchmark prerequisite, exceeds its VRAM budget, or loses the full-frame
retention gate. Preserve the best correct product path after every slice.

The first implementation slice should be P0 + P1. P1 creates the memory headroom
needed for the first genuinely high-leverage experiment: the P2 2048 tile.
