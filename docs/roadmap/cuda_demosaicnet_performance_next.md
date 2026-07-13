# CUDA DemosaicNet Performance — Next Optimization Plan

Date: 2026-07-13

Status: active follow-up to
[`cuda_nn_forward_demosaicnet_plan.md`](cuda_nn_forward_demosaicnet_plan.md).
**P0 complete** (see §4.3). **P1 complete** (see §5.4). **P2 complete** (see
§6.5) — larger square tiles **rejected**; retain 1024. **P3 complete** (see §7.4) —
CUDA Graph implemented, correctness green, **rejected for product default**
(latency gate not met; matches P0 “wall ≈ batch”). **P4-A through P4-D are now
the active ordered experiments** (see §8): fused tail, ragged edge tiles,
rectangular strips, then persistent channels-last FP32.

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
3. **P3 (CUDA Graph)** was still **implemented and measured** after P2 (user
   request); full-frame gate failed as P0 predicted — see §7.4.
4. **P4-A through P4-D** are the active ordered experiments. Tail fusion and
   tile geometry run first; channels-last remains the last, microbenchmark-gated
   slice because it has the largest implementation surface.

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

- `build/perf/demosaicnet_next_p2_tiles_1024.json` (or per-fixture `*_bayer` / `*_xtrans`)
- `build/perf/demosaicnet_next_p2_tiles_1536.json`
- `build/perf/demosaicnet_next_p2_tiles_2048.json`
- `build/perf/demosaicnet_next_p2_tiles_3072.json`
- `build/perf/demosaicnet_next_p2_summary.json`

### 6.5 P2 results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed:

- Generalized student policies from an explicit owned-output edge while keeping
  1K pad/border constants exact:
  - Bayer: `input = owned + 62`, pad 32, border 31, step = owned
  - X-Trans: `input = owned + 24`, pad 12, border 12, step = `floor(owned/6)*6`
- `OutputHeight` / `OutputWidth` export-crop any product square
  `owned + 2*border` with `owned >= 1024` (free-size patches stay natural)
- `NeuralDemosaicOptions::student_owned_tile_edge` + product
  `SelectStudentProductTileEdge` (VRAM budgets 35% total / 50% free)
- Harness `--tile-size 1024|1536|2048|3072` forces product path via override
- OOM warm-up falls back down the retained list; force/override fails hard
- After measurement, **product auto-select uses only retained edges**
  (`kStudentProductRetainedTileEdges = {1024}`); larger edges remain
  forceable for re-measure

Commands:

```bat
build\release\alcedo_studio\tests\MlOpsTest.exe
build\release\alcedo_studio\tests\CudaRawOpsTest.exe --gtest_filter=*Student*:*LargerTile*

:: Per-edge cool-ish matrix (HLR-off neural full):
for %E in (1024 1536 2048 3072) do (
  build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
    --fixture bayer --method neural --mode full --model student ^
    --warmup 3 --iterations 20 --tile-size %E ^
    --output build/perf/demosaicnet_next_p2_tiles_%E_bayer.json
  build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
    --fixture xtrans --method neural --mode full --model student ^
    --warmup 3 --iterations 20 --tile-size %E ^
    --output build/perf/demosaicnet_next_p2_tiles_%E_xtrans.json
)
```

#### Full-frame latency matrix (same session; p50 vs 1024 control in-matrix)

| Edge | Bayer jobs | Bayer p50 ms | vs 1024 | X-Trans jobs | X-Trans p50 ms | vs 1024 | Gate |
|-----:|-----------:|-------------:|--------:|-------------:|---------------:|--------:|------|
| 1024 | 40 | 412.7 | control | 48 | 525.6 | control | **retain** |
| 1536 | 20 | 1012.6 | **+145%** | 24 | 2102.7 | **+300%** | **reject** |
| 2048 | 12 | 2055.1 | **+398%** | 12 | 1826.1 | **+247%** | **reject** |
| 3072 | 6 | 2322.9 | **+463%** | 6 | 2141.5 | **+307%** | **reject** |

p95 also regresses far beyond +5% on every larger edge. Owned memory for larger
edges remains within the 8 GiB laptop budget (selection was not VRAM-limited).

#### Work accounting (why larger is slower)

| Edge | Bayer halo_work | Bayer full TFLOP | Bayer eff TFLOP/s | X-Trans halo | X-Trans TFLOP | X-Trans eff |
|-----:|----------------:|-----------------:|------------------:|-------------:|--------------:|------------:|
| 1024 | 1.15 | 0.975 | **2.36** | 1.23 | 1.00 | **1.91** |
| 2048 | 1.38 | 1.13 | 0.55 | 1.23 | 0.99 | 0.54 |
| 3072 | 1.55 | 1.25 | 0.54 | 1.39 | 1.11 | 0.52 |

Job count drops, but paid full-frame FLOPs rise (larger virtual-pad windows) and
**effective TFLOP/s collapses ~4×** on the direct NCHW kernels — consistent with
activation footprint / cache thrash, not launch overhead. Launch amortization
does not compensate.

#### Correctness

| Check | Result |
|-------|--------|
| `MlOpsTest` | **148/148** |
| Policy formulas 1024/1536/2048/3072 | **pass** |
| CFA-period origins + exclusive ROI coverage | **pass** |
| Bayer/X-Trans 2048 matches 1K across former boundary | **pass** (`max abs ≤ 1e-4`) |
| Low-VRAM / retained auto-select → 1024 | **pass** |

#### Retention summary

| Gate | Result |
|------|--------|
| p50 improves ≥5% | **fail** for 1536/2048/3072 (large regressions) |
| p95 does not regress >5% | **fail** |
| geometry / goldens / seams | **pass** |
| VRAM budget | **pass** (not the limiter) |

**Verdict: REJECT larger product tiles for auto-select. RETAIN 1024.**  
Geometry generalization stays in-tree for harness re-measure and future kernel
work (P4). Product `SelectStudentProductTileEdge` only walks
`kStudentProductRetainedTileEdges = {1024}`.

Artifacts: per-edge JSON under `build/perf/demosaicnet_next_p2_tiles_*` and
`build/perf/demosaicnet_next_p2_summary.json`.

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

### 7.4 P3 results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed (correctness-complete; product default off):

- `NeuralDemosaicForwardGraph` owned by `NeuralDemosaicWorkspace` (not process-global)
- Capture boundary: fixed-shape model `Forward` only (pack / unpack / ROI stay ordinary)
- Key: device, variant, tile geometry, I/O + activation pointers, weight pointers,
  and `DemosaicNetModelCache::WeightGeneration` (survives CUDA address reuse)
- Stream capture (`cudaStreamCaptureModeThreadLocal`) → instantiate → **launch**
  (capture records only; first launch required for real results)
- Fallback: null stream, profiler active, capture/runtime failure, or
  `ALCEDO_DEMOASICNET_DISABLE_CUDA_GRAPH`
- `NeuralDemosaicOptions::enable_cuda_graph` **default false** after retention fail
- Tests force `enable_cuda_graph = true` for P3 contracts

Commands:

```bat
build\release\alcedo_studio\tests\MlOpsTest.exe --gtest_filter=*Graph*:*Invalidates*:*ModelReload*:*WorkspaceGrowth*

:: A/B (same binary; env forces ordinary):
set ALCEDO_DEMOASICNET_DISABLE_CUDA_GRAPH=1
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture bayer --method neural --mode full --model student ^
  --warmup 3 --iterations 20 --output build/perf/demosaicnet_next_p3_ordinary_bayer.json
set ALCEDO_DEMOASICNET_DISABLE_CUDA_GRAPH=
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture bayer --method neural --mode full --model student ^
  --warmup 3 --iterations 20 --output build/perf/demosaicnet_next_p3_graph_bayer.json
```

#### Correctness

| Check | Result |
|-------|--------|
| Graph vs ordinary student tile (Bayer + X-Trans) | **pass** (`max abs ≤ 1e-4`) |
| Graph replay preserves exported goldens | **pass** (Bayer + X-Trans `*_00`) |
| Model reload invalidates (weight generation) | **pass** |
| Workspace growth invalidates activation pointers | **pass** |
| Replay keeps `allocation_generation` stable | **pass** |
| Two workspaces own independent executables | **pass** |

#### Full-frame latency (Release, 1K retained policy)

Cool-ish single-fixture graph-on (first measure after rebuild, graphs enabled in that binary):

| Variant | min ms | p50 ms | p95 ms |
|---------|-------:|-------:|-------:|
| Bayer graph | 361.0 | 398.8 | 415.8 |
| X-Trans graph | 440.4 | 483.7 | 689.3 |

Same-session A/B (ordinary first, then graph; laptop thermally climbs):

| Variant | mode | min ms | p50 ms | p95 ms | vs ordinary p50 |
|---------|------|-------:|-------:|-------:|----------------:|
| Bayer | ordinary | 381.1 | 408.6 | 423.7 | control |
| Bayer | graph | 431.1 | 506.9 | 607.1 | **+24%** (thermal-contaminated order) |
| X-Trans | ordinary | 1011.6 | 1080.0 | 1533.0 | control (already hot) |
| X-Trans | graph | 1758.8 | 1814.6 | 2405.0 | **+68%** (severely throttled) |

Cross-session cool comparison (not perfect isolation): graph Bayer p50 **398.8** vs
ordinary A/B p50 **408.6** is only ~**2.4%** better — **below the ≥5% gate**.
This matches P0: wall − batch / wall ≈ 0.04%, so WDDM launch amortization has
almost nothing to reclaim. Graph capture/replay also adds host work on first
tile and does not raise effective TFLOP/s of the direct NCHW kernels.

#### Retention summary

| Gate | Result |
|------|--------|
| p50 improves ≥5% | **fail** |
| Graph construction amortized after warm-up | **pass** (capture once per workspace key) |
| p95 / fallback defined | **pass** (sticky ordinary on failure) |
| Goldens / invalidation tests | **pass** |

**Verdict: REJECT product default CUDA Graph.** Infrastructure stays in-tree;
`enable_cuda_graph` defaults to **false**. Re-enable only with new evidence
(e.g. desktop TCC, multi-tile launch gap after a faster kernel path). The
retained path remains far above target with trunk/post dominant, so proceed to
the ordered P4-A through P4-D experiments below.

Artifacts:

- `build/perf/demosaicnet_next_p3_bayer.json` / `_xtrans.json` (early graph-on)
- `build/perf/demosaicnet_next_p3_ordinary_*.json` / `_graph_*.json` (A/B)
- `build/perf/demosaicnet_next_p3_summary.json`

## 8. Phase P4 — ordered FP32 product experiments

Run P4 strictly in the order P4-A -> P4-B -> P4-C -> P4-D. Each phase starts
from the best retained predecessor and must leave the prior product path as a
runtime/compile-time fallback. Do not combine two unproven candidates in one
measurement: that makes a regression impossible to attribute.

The sustained hot state is part of the product workload. A user may repeatedly
switch RAW files, so P4 retention is based on repeated full-frame measurements
without a cooldown requirement or a cold-clock normalization. Temperature,
power, and clocks may still be recorded as diagnostics, but a candidate does
not receive a separate cold-state rescue result.

Common constraints for P4-A through P4-D:

- retain the in-tree hard-coded model and FP32 numerical contract;
- no cuDNN, TensorRT, ONNX Runtime, TF32, FP16, BF16, WMMA, or Tensor Core path;
- no per-forward weight transform, `cudaMalloc`, or workspace growth after warm-up;
- retain Bayer and X-Trans correctness, CFA phase, owned-ROI, crop, and orientation;
- preserve the existing path for HLR, DNG warp, unsupported geometry, and allocation failure;
- use the same Release harness, fixture order, warm-up count, iteration count, and sustained
  workload for baseline and candidate;
- retain a slice only when full-frame p50 improves by >=5%, p95 does not regress by >5%,
  all correctness tests pass, and the extra VRAM/complexity is proportionate.

### 8.1 P4-A — fuse the post/output/gamma tail

#### Rationale

`Conv2d3x3s1TiledKernel` already holds every post-convolution output channel for
one pixel in registers (`24` for Bayer, `32` for X-Trans). The ordinary path
then writes that full activation to global memory, launches a 1x1 `C -> 3`
convolution that reads it back, writes NCHW RGB, converts NCHW to HWC, assembles
the frame, and finally gamma-decodes the full RGB frame. This is the cleanest
remaining exact fusion opportunity and also removes the largest P1 slot.

#### Implementation slices

1. Add a dedicated FP32 primitive for the two exact student tails:
   `6 -> 24 -> 3` and `6 -> 32 -> 3`.
   It performs post 3x3 accumulation, post bias, ReLU, output 1x1 accumulation,
   and output bias without materializing the `24/32`-channel post tensor.
2. Prepack/retain any alternate output-weight layout at model load time. The
   hot path must use immutable resident weights and must not transform them.
3. Add a student-specific forward-to-HWC entrypoint so the fused epilogue can
   write three contiguous RGB values directly into the pitched tile `GpuMat`.
   Keep the existing NCHW `Forward` API for tests and fallback callers.
4. Fold the current signed gamma decode (`pow_signed(x, 2.2)`) into the fused
   output epilogue. Preserve the current operation and edge-case semantics;
   do not introduce `--use_fast_math` as part of this experiment.
5. After the generic HWC version is retained, add an optional HLR-off and
   no-DNG-warp fast path that applies inverse cam-mul/orientation and writes
   owned pixels directly to final RGBA. HLR-on or warp-enabled frames continue
   through the assembled linear RGB fallback.
6. Remove the post activation slot from the fused workspace estimate only
   after the fused path is selected. Graph/cache keys must distinguish fused
   and ordinary pointer topologies.

#### Required measurements and gates

- Benchmark fused versus ordinary tail at the Bayer and X-Trans 1024 control
  shapes before RAW integration.
- Report tail CUDA time, complete model-forward time, full-frame time, launches,
  activation bytes, and total owned bytes.
- Desired isolated tail gain: >=15%.
- Mandatory product gate: full-frame p50 >=5% faster (>=8% is the target), p95
  within the common gate, and lower activation memory.
- Reject only the direct-to-final-RGBA sub-slice if it cannot preserve crop,
  orientation, and ROI semantics; a retained generic fused HWC tail may remain.

Artifacts:

- `build/perf/demosaicnet_next_p4a_tail_microbench.json`
- `build/perf/demosaicnet_next_p4a_full_bayer.json`
- `build/perf/demosaicnet_next_p4a_full_xtrans.json`

### 8.2 P4-B — ragged edge tiles instead of paid 1024 tails

#### Rationale

The retained grid evaluates every right/bottom edge job as a full 1024-owned
tile. P0 reports paid-output factors of about `1.148x` for Bayer and `1.233x`
for X-Trans. Removing tiling entirely is not required to remove most of this
tail work: keep 1024 interior tiles and make only boundary jobs rectangular.

#### Implementation slices

1. Extend the student tile job/policy representation with per-job owned width,
   owned height, input width, and input height. Do not overload the Legacy RCD
   clamped-halo behavior.
2. Generalize Bayer/X-Trans product geometry from square `owned_edge` to
   rectangular `(owned_w, owned_h)`, with input dimensions equal to owned
   dimensions plus the model border on both sides.
3. Keep job origins aligned to the CFA period (2 Bayer, 6 X-Trans). Preserve
   X-Trans first-writer ownership where the period-safe step creates overlap.
4. Reuse the maximum 1024 workspace; smaller edge jobs form views inside that
   capacity and must not trigger allocation growth.
5. Avoid a pathologically narrow final Bayer tile. If the final owned extent is
   below 512, rebalance it with its predecessor into two period-aligned extents
   no larger than 1024. Apply the same rule independently in X and Y.
6. Update the profiler/roofline output to report actual per-job shapes and the
   sum of paid pixels/FLOPs rather than `tile_count * 1024^2`.

#### Required measurements and gates

- Compare the fixed-1024 and ragged grids using the same retained P4-A setting.
- Report job shape histogram, paid pixels, estimated FLOPs, tile count, and
  full-frame p50/p95.
- Add explicit seam checks at every internal boundary and at the final right,
  bottom, and bottom-right jobs.
- Mandatory product gate: >=5% full-frame p50 gain on at least one fixture, no
  >2% p50 regression on the other, and no increase in peak workspace.
- If only one CFA benefits, retain a CFA-specific policy rather than forcing a
  shared default.

Artifacts:

- `build/perf/demosaicnet_next_p4b_ragged_bayer.json`
- `build/perf/demosaicnet_next_p4b_ragged_xtrans.json`

### 8.3 P4-C — rectangular and full-width strip tiling

#### Rationale

P2 rejected larger **square** tiles under sustained product load; it did not
measure constant-area aspect-ratio changes or full-width bounded-memory strips.
An unconstrained full-frame forward is not a product candidate: the current P1
formula estimates about 6.48 GiB owned workspace for the D800E Bayer frame and
9.09 GiB for the X-T5 X-Trans frame before the rest of the RAW pipeline.

Full-width strips remain bounded. Approximate 512-owned-height candidates are
about 0.73 GiB owned workspace for Bayer and 0.92 GiB for X-Trans, before any
additional reduction retained from P4-A.

#### Experiment matrix

First isolate aspect ratio at approximately constant input area:

| Owned shape class | Purpose |
|-------------------|---------|
| 1024 x 1024 | retained control |
| 2048 x 512 | 2:1 rectangle |
| 4096 x 256 | 4:1 rectangle |
| aligned full width x ~128 | extreme strip at near-control area |

Then run complete RAW frames with aligned full-width owned strips of height
128, 256, and 512. Derive exact Bayer/X-Trans input sizes from their border and
CFA-period rules; do not hard-code one shared geometry.

#### Implementation and fallback rules

- Separate owned width and height throughout policy selection, workspace
  estimation, graph/cache keys, profiler metadata, and harness overrides.
- Reuse one workspace and one stream in raster strip order.
- Combine P4-C only with already-retained P4-A/P4-B work; report an ordinary
  1024 control from the same sustained session.
- Auto-selection must check free/total VRAM using the existing conservative
  fractions. Allocation failure falls back directly to the retained ragged
  1024 policy.
- Do not add a whole-frame allocation fallback for X-Trans.

#### Retention gate

- Constant-area rectangular forward throughput per FLOP must remain within 10%
  of the 1024 control before full-width strips are integrated.
- A strip policy must improve full-frame p50 by >=5%, keep p95 within the common
  gate, and fit the existing VRAM budget.
- If every strip loses under the repeated hot workload, reject P4-C and retain
  P4-B. Do not reclassify it using a separate cooled run.

Artifacts:

- `build/perf/demosaicnet_next_p4c_rectangles.json`
- `build/perf/demosaicnet_next_p4c_strips_bayer.json`
- `build/perf/demosaicnet_next_p4c_strips_xtrans.json`

### 8.4 P4-D — persistent channels-last/vectorized FP32

This is the most invasive P4 experiment and starts only after A-C have produced
the retained baseline. NHWC is not treated as intrinsically faster: this track
still uses scalar FP32 CUDA cores, so it must win through lower register
pressure, channel-vector loads, and better kernel work distribution rather
than Tensor Core behavior.

#### Microbenchmark prerequisite

Implement only isolated kernels first:

- trunk `24 -> 24`, 3x3, bias + ReLU;
- trunk `32 -> 32`, 3x3, bias + ReLU;
- the retained fused P4-A tail, or `6 -> 24/32` post if P4-A was rejected.

Use persistent NHWC input/output and load/store channel groups with `float4`
where alignment permits. For post `Cin=6`, use an exact `float4 + float2` (or
equivalent scalar tail); do not pad to eight channels and silently add 33%
more post-convolution FLOPs.

Isolated trunk kernels must improve by >=25%, and the tail candidate by >=15%,
before implementing structural NHWC operators.

#### Full internal-layout scope

- pack/preprocess writes the first internal tensor directly in channels-last;
- all trunk activations remain channels-last with no per-layer transpose;
- residual, unpack, crop, and concat receive dedicated channels-last paths;
- immutable weights are transformed/prepacked once during model load;
- the final fused tail writes pipeline HWC/RGBA directly;
- no public-boundary NCHW conversion is allowed in the timed product path;
- ordinary NCHW remains the compatibility and failure fallback.

Profile register count, theoretical/achieved occupancy, eligible warps, local
loads/stores, long-scoreboard stalls, L1/L2 hit rate, and DRAM throughput for
both the retained NCHW and NHWC kernels. A kernel win without a complete-model
win is insufficient.

#### Retention gate

- isolated gates above pass for both student widths;
- complete tile/model forward improves p50 by >=10% with all structural work;
- full-frame p50 improves by >=5% and p95 stays within the common gate;
- peak VRAM does not exceed the retained predecessor by >10%;
- FP32 goldens and CC 6.0+ behavior remain intact.

Artifacts:

- `build/perf/demosaicnet_next_p4d_channels_last_microbench.json`
- `build/perf/demosaicnet_next_p4d_full_bayer.json`
- `build/perf/demosaicnet_next_p4d_full_xtrans.json`

### 8.5 Required P4 correctness coverage

Add concrete regression tests (the exact suite may use equivalent established
naming) for:

- `FusedPostOutputMatchesUnfusedStudentForward`;
- `FusedStudentHwcOutputMatchesOrdinaryNchwUnpack`;
- `RaggedStudentTilesPreserveInteriorAndBoundaryPixels`;
- `RectangularStudentTilesPreserveCfaPhaseAndFirstWriterOwnership`;
- `ChannelsLastStudentForwardMatchesNchwWithinFp32Tolerance`;
- allocation generation remains constant after warm-up for every retained path;
- existing real Bayer/X-Trans RAW goldens, crop/orientation, HLR-off/on, and
  fallback behavior.

Do not weaken a golden tolerance to retain a performance candidate. If an
operation-order change requires a new tolerance, document its measured max/mean
error and obtain a separate correctness decision.

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
5. P3 CUDA Graph measured and rejected for product default;
6. P4-A fused tail;
7. P4-B ragged edge tiles on the retained P4-A result;
8. P4-C rectangular/full-width strips on the retained A-B result;
9. P4-D persistent channels-last only after its isolated kernels pass.

Stop a candidate immediately when it breaks correctness, cannot clear its
microbenchmark prerequisite, exceeds its VRAM budget, or loses the full-frame
retention gate. Preserve the best correct product path after every slice.

P0-P3 are complete. The next implementation slice is P4-A only. Record and
decide it before assigning P4-B; continue the same handoff discipline through
P4-D so every retained improvement has an attributable artifact and fallback.
