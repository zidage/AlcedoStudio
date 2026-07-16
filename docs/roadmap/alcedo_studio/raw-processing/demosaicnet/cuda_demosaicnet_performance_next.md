# CUDA DemosaicNet Performance — Next Optimization Plan

Date: 2026-07-13

Primary roadmap owner: `alcedo_studio/src/decoders/processor/nn`

Status: active follow-up to
[`cuda_nn_forward_demosaicnet_plan.md`](cuda_nn_forward_demosaicnet_plan.md).
**P0 complete** (see §4.3). **P1 complete** (see §5.4). **P2 complete** (see
§6.5) — larger square tiles **rejected**; retain 1024. **P3 complete** (see §7.4) —
CUDA Graph implemented, correctness green, **rejected for product default**
(latency gate not met; matches P0 “wall ≈ batch”). **P4-A complete** (see §8.1) —
fused post/output/gamma tail **retained** as product default. **P4-B complete**
(see §8.2) — ragged edge tiles **rejected** for product default (paid pixels drop
but full-frame p50 regresses). **P4-C complete** (see §8.3) — rectangular /
full-width strip tiling **rejected** for product default. **P4-D complete and
retained** (see §8.4) — the complete persistent channels-last FP32 path is now
the product default for Bayer and X-Trans. **P5 production cleanup complete**
(see §11.10): rejected experiments, dispatch switches, and neural-to-neural
fallbacks deleted; shipping path is fixed **1024 + persistent NHWC + fused HWC
tail** with product soft-fail only to Classical/Legacy.

## 1. Objective and current baseline

Keep the existing in-tree, hard-coded FP32 CUDA inference runtime. Do not add
ONNX Runtime, TensorRT, cuDNN, a runtime graph interpreter, reduced precision,
or a new model architecture in this track.

Current retained RTX 3080 Laptop short full-frame medians (Release,
`--warmup 3 --iterations 20`, HLR-off, post-P4-A fused tail):

| Variant | Retained latency (p50) | Stretch target |
|---------|-----------------------:|---------------:|
| Bayer student (`bayer_s24_d8`) | ~376 ms | <=100 ms |
| X-Trans student (`xtrans_p2_s32_d4`) | ~412 ms | <=100 ms |

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

#### 8.1.1 P4-A results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed:

- `cuda/nn/fused_post_output.cu` — exact `6→24→3` / `6→32→3` FP32 kernel:
  post 3×3 + bias + ReLU + output 1×1 + optional signed γ=2.2, no post tensor
- CIO output-weight prepack once at model load (`output_w_cio_`)
- `BayerDemosaicNet` / `XTransDemosaicNet`: fused NCHW `Forward` default;
  product `ForwardHwc` writes pitched tile RGB with gamma in the epilogue
- Peak-live workspace drops the post slot (~60% less activation on 1K tiles)
- Ordinary path retained: `force_ordinary_tail` / env
  `ALCEDO_DEMOASICNET_DISABLE_FUSED_TAIL=1`
- Optional direct-to-final-RGBA (plan slice 5) **not** implemented in this slice;
  product still assembles linear HWC then inverse cam-mul / HLR / pack

Correctness (Release `MlOpsTest` filter):

- goldens 00/01 Bayer + X-Trans
- `FusedPostOutputMatchesUnfusedStudentForward` (+ X-Trans)
- `FusedStudentHwcOutputMatchesOrdinaryNchwUnpack`
- peak-live fused vs ordinary workspace estimates

##### Profiled-frame tail isolation (CUDA-event ranges)

| Metric | Bayer ordinary | Bayer fused | X-Trans ordinary | X-Trans fused |
|--------|---------------:|------------:|-----------------:|--------------:|
| post_output ms | 47.4 | **24.5** | 110.3† | **71.2**† |
| tail gain | | **48.4%** | | **35.4%** |
| nchw_hwc_unpack ms | 2.8 | 0 | 5.7 | 0 |
| launches / tile | 19 | **16** | 15 | **12** |
| activation workspace | 168 MiB | **67 MiB** | 208 MiB | **79 MiB** |

† X-Trans profiled absolute times were thermally elevated (SM clocks down to
~480–1050 MHz); relative tail gain still clears the ≥15% isolated gate.

##### Fair full-frame A/B (cool between runs; `--warmup 3 --iterations 20`)

| Variant | Ordinary p50 | Fused p50 | p50 gain | Ordinary p95 | Fused p95 |
|---------|-------------:|----------:|---------:|-------------:|----------:|
| Bayer D800e | 412.6 ms | **376.3 ms** | **+8.8%** | 487.9 | **385.0** |
| X-Trans XT5 | 455.0 ms | **411.7 ms** | **+9.5%** | 668.6 | **488.0** |

##### Decision gates

| Gate | Threshold | Result | Verdict |
|------|-----------|--------|---------|
| Isolated tail | ≥15% | Bayer 48% / X-Trans 35% | **pass** |
| Full-frame p50 | ≥5% (target ≥8%) | +8.8% / +9.5% | **pass** |
| p95 | no >5% regression | improved on both | **pass** |
| Activation memory | lower | −60% class | **pass** |
| Correctness | goldens + fused/unfused | green | **pass** |

**Verdict: RETAIN fused post/output/gamma as product default.** Ordinary path
remains the runtime fallback. P4-B (ragged edge tiles) was measured next and
**rejected** (see §8.2.1). Direct-to-final-RGBA remains optional follow-up and is
not required for retention.

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
- `build/perf/demosaicnet_next_p4b_fixed_bayer.json` / `_xtrans.json` (same-session controls)
- `build/perf/demosaicnet_next_p4b_summary.json`

#### 8.2.1 P4-B results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed (opt-in experiment; **not** product default):

- Per-job `owned_w/h` + `input_w/h` on `CudaTileJob`; student planner via
  `PlanStudentAxisSpans` (shrink edge extents to destination ROI, rebalance when
  final dest extent &lt; 512 into two period-aligned extents ≤ 1024)
- `SetStudentRaggedEdgeTilesEnabled` / harness `--ragged-edges`; product default
  remains **false** (fixed 1024 jobs)
- Workspace RGB is grow-only so ragged edge jobs reuse the max 1K reservation
  without post-warm realloc; `ProcessCudaTiled` sets per-job owned sizes
- Harness paid-pixel accounting sums actual per-job owned areas
- Correctness: planner cover/rebalance/CFA-phase tests; GPU assembly
  `RaggedEdgeBayerAssemblyMatchesFixed1024WithinFp32Tolerance` (max abs ≤ 1e-3)

##### Paid work (geometry)

| Variant | Fixed paid px | Ragged paid px | Reduction |
|---------|--------------:|---------------:|----------:|
| Bayer D800e (40 jobs) | 41,943,040 | 36,570,624 | **−12.8%** |
| X-Trans XT5 (48 jobs) | 50,331,648 | 41,143,528 | **−18.3%** |

Matches the P0 paid-output / halo story: most of the 1.15–1.23× paid factor is
on right/bottom tails and is removed by ragged extents.

##### Full-frame A/B (same session after cooldown; `--warmup 3 --iterations 20`)

| Variant | Fixed p50 | Ragged p50 | p50 Δ | Fixed p95 | Ragged p95 |
|---------|----------:|-----------:|------:|----------:|-----------:|
| Bayer D800e | 404.8 ms | **525.0 ms** | **−29.7%** | 439.6 | **692.0** |
| X-Trans XT5 | 1512.3 ms† | **2023.5 ms**† | **−33.8%** | 2441.9 | 2058.0 |

† X-Trans absolute times were thermally elevated in this session (sustained load
after Bayer). Relative regression vs the **same-session fixed control** is still
decisive. Bayer **min** also worsened (396.7 → 416.9 ms), so the loss is not
explained by p50 noise alone.

##### Decision gates

| Gate | Threshold | Result | Verdict |
|------|-----------|--------|---------|
| Full-frame p50 | ≥5% on ≥1 fixture | both regress | **fail** |
| Other fixture | no &gt;2% p50 regression | both regress | **fail** |
| Peak workspace | no increase | still max-1K reserve | **pass** |
| Correctness | seams + assembly | green | **pass** |

**Verdict: REJECT ragged edge tiles for product default.** Infrastructure stays
opt-in (`--ragged-edges` / `SetStudentRaggedEdgeTilesEnabled(true)`). Retained
path remains square 1024 + fused tail (P4-A). Smaller variable-shape edge
kernels do not convert paid-pixel savings into wall-time on this WDDM laptop
path (likely launch/occupancy inefficiency dominating the ~13–18% FLOP cut).

Next ordered experiment: **P4-D** (channels-last FP32) after its isolated kernel
gates pass. P4-C was already measured and rejected against fixed 1024.

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
- `build/perf/demosaicnet_next_p4c_summary.json`

#### 8.3.1 P4-C results (2026-07-13, RTX 3080 Laptop, Release)

Implementation landed (infrastructure retained for harness/experiments; **not**
product-default):

- Product export geometry accepts rectangular tiles: per-axis
  `owned = input − 2·border` when both axes ≥ `kMinProductOwned` (128)
- `MakeBayer/XTransStudentTilePolicy(owned_w, owned_h)` + full-width strip helpers
- `StudentOwnedTileShape` override / selection; `NeuralDemosaicOptions`
  `student_owned_tile_w/h`; product path falls back to square 1024 on OOM
- Harness: `--tile-width` / `--tile-height` / `--strip-height`
- Correctness: rectangular export sizes, CFA phase + first-writer cover,
  Bayer strip assembly vs 1K interior (`CudaTileJobsTest`, `CudaRawOpsTest`)

##### Constant-area tile microbench (`--mode tile`, ~1M owned pixels)

| Owned shape | Bayer p50 ms | vs 1K | X-Trans p50 ms | vs 1K |
|-------------|-------------:|------:|---------------:|------:|
| 1024×1024 (control) | 5.38 | — | 4.60 | — |
| 2048×512 | 5.52 | **+2.7%** | 4.67 | **+1.5%** |
| 4096×256 | 5.94 | **+10.4%** | 4.72 | **+2.6%** |
| 8192×128 | 6.79 | **+26%** | 4.99 | **+8.6%** |

Moderate 2:1 rectangles stay within the ±10% FLOP-normalized gate for both CFA
families; extreme aspect ratios (especially Bayer 8:1) lose. Proceeded to
full-frame strips per plan.

##### Full-frame strip A/B (sustained hot; `--warmup 3 --iterations 20`)

| Policy | Bayer jobs | Bayer p50 | vs 1K | X-Trans jobs | X-Trans p50 | vs 1K |
|--------|----------:|----------:|------:|-------------:|------------:|------:|
| 1024² control | 40 | **328.8 ms** | — | 48 | **906.9 ms**† | — |
| full-W × 128 | 78 (2×39)‡ | 757 ms | **−57%** | 42 | 1567 ms | **−42%** |
| full-W × 256 | 40 (2×20)‡ | 1968 ms | **−83%** | 21 | 1489 ms | **−39%** |
| full-W × 512 | 20 (2×10)‡ | 2902 ms | **−89%** | 11 | 1562 ms | **−42%** |

† X-Trans control was thermally elevated after the Bayer series (min still
622 ms vs strip mins ≥1388 ms). Relative strip loss holds.

‡ Bayer full-width owned = cover width still schedules **two** horizontal jobs
because pad32/border31 places the first model origin at −1; residual one-pixel
column forces a second nearly full-width paid tile. That amplifies strip cost
but is not the sole cause (X-Trans is 1×N and still loses ≥39%).

Root cause under sustained load: wide NCHW product tiles destroy the cache
behavior that the retained 1K square direct kernels rely on. Reducing job count
does not recover wall time when each job’s spatial width is ~7–8K.

##### Decision gates

| Gate | Threshold | Result | Verdict |
|------|-----------|--------|---------|
| Constant-area within 10% of 1K | 2048×512 | pass both CFA | **pass** (moderate only) |
| Full-frame strip p50 ≥5% faster | strips | all regress | **fail** |
| p95 within common gate | no >5% regress | failed with p50 | **fail** |
| VRAM fit | conservative fractions | strips fit; not limiting | n/a |
| Correctness | rect + strip tests | green | **pass** |

**Verdict: REJECT P4-C for product default.** Retain square **1024** + P4-A
fused tail. Rectangular/strip policy APIs and harness overrides stay in-tree for
future experiments (e.g. after a channels-last kernel path), but auto-select
never picks strips. P4-B was measured afterward and also **rejected** (see
§8.2.1). Next ordered experiment: **P4-D** (channels-last FP32) after isolated
kernel gates.

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

- `build/perf/demosaicnet_next_p4d_nchw_microbench.json`
- `build/perf/demosaicnet_next_p4d_channels_last_microbench.json`

#### P4-D result (2026-07-13, RTX 3080 Laptop, Release)

Implementation stopped at the required isolated-kernel gate. The benchmark-only
candidate keeps input/output activations in persistent NHWC, prepacks immutable
OIHW weights once to `[Cin,3,3,Cout]`, and uses `float4` cooperative loads and
stores. It is not reachable through the product `Conv2d` dispatch.

Correctness: `ChannelsLastSquareStudentTrunksMatchNchwReference` passed for
both 24 and 32 channels (FP32 tolerance `2e-4`).

| Trunk | Retained NCHW p50 | NHWC p50 | Gain | Gate |
|-------|------------------:|---------:|-----:|------|
| Bayer 24→24 (layers 2–8 mean) | 0.589 ms | **0.438 ms** | **25.7%** | pass |
| X-Trans 32→32 (layers 2–4 mean) | 0.902 ms | **0.709 ms** | **21.4%** | fail |

A wider 8×32 / 256-thread X-Trans variant measured 0.752 ms mean, worse than
the retained 8×16 / 128-thread NHWC candidate (0.709 ms). The best 32-channel
candidate therefore remains below the required 25% isolated trunk improvement.
Per the stopping rule, no NHWC residual/structural/tail path or full-frame
experiment was implemented.

**Verdict: PROMOTE P4-D as the next product implementation track.** The
measured 21–26% isolated-trunk gain is sufficient evidence to complete and
measure the end-to-end channels-last product path. Preserve the current NCHW
path as a correctness/failure fallback until that result is accepted.

### P4-D production objective

Complete the persistent FP32 channels-last implementation, validate it against
the existing Bayer/X-Trans reference outputs and sustained full-frame measurements, then
make the winning path the product default. Once that decision is complete,
remove superseded convolution experiments, benchmark-only dispatch switches,
and their dead supporting code so the shipping runtime contains one maintained
implementation plus only the required compatibility fallback. This is a goal,
not a detailed implementation plan.

#### P4-D first full-frame result (2026-07-13)

The first integration replaced only the equal-width trunk layers and converted
the activation layout at both ends of that section. It did not meet the product
gate, so the production HWC entry was restored to NCHW.

| Fixture | Full-frame p50 | Result |
|---------|---------------:|--------|
| Bayer D800e | 384.4 ms | no retained improvement |
| X-Trans XT5 | 493.1 ms | regression with high tail variation |

The offline NHWC model assets and isolated-kernel evidence are retained for the
production objective above. No default-path change is made from this result.

#### CUTLASS FP32 NHWC candidate (2026-07-13)

NVIDIA CUTLASS 3.9.2 is integrated as a pinned submodule at commit
`ad7b2f5e84fcfa124cb02b91d5bd26d238c0459e` under its BSD-3-Clause license.
The candidate uses the SIMT FP32 implicit-GEMM fprop kernel with optimized NHWC
iterators, fuses per-channel bias and ReLU in the epilogue, runs on the caller's
CUDA stream, and requires no workspace for `split_k_slices=1`. Immutable OIHW
weights are prepacked to CUTLASS KRSC order outside the measured forward loop.

An exact 100-iteration A/B on the current checkout selected a hybrid dispatch:

| Equal-width trunk | In-tree NHWC p50 mean | CUTLASS p50 mean | Decision |
|-------------------|-----------------------:|-----------------:|----------|
| Bayer 24→24 (layers 2–8) | **0.438 ms** | 0.506 ms | retain in-tree `float4` kernel |
| X-Trans 32→32 (layers 2–4) | 0.893 ms | **0.705 ms** | select CUTLASS 128×32 CTA |

The final X-Trans CUTLASS run also reduced the three-layer mean p95 from
1.498 ms to 1.010 ms. Its 0.705 ms p50 matches the earlier best
in-tree result (0.709 ms) while avoiding the current kernel's tail collapse;
this is primarily a stability/dispatch win, not evidence of another 20% beyond
the best historical NHWC measurement. `--conv-cutlass` now benchmarks this
hybrid policy. The product default remains unchanged until activations stay
channels-last across the complete model section, so no boundary transpose is
charged per trunk segment.

Artifacts:

- `build/perf/demosaicnet_cutlass_ab_current_nhwc.json`
- `build/perf/demosaicnet_cutlass_ab_candidate.json`
- `build/perf/demosaicnet_cutlass_hybrid.json`

#### Current product-path range profile after CUTLASS integration

This profile still measures the retained NCHW product path; CUTLASS is only
reachable through the isolated `--conv-cutlass` switch until the complete
persistent-NHWC section exists.

| Range (one full frame) | Bayer D800e | X-Trans XT5 |
|------------------------|------------:|------------:|
| hot full-process p50 (10 runs) | 393.698 ms | 427.550 ms |
| CUDA batch | 366.124 ms | 382.640 ms |
| phase/crop/linear | 13.512 ms | 17.749 ms |
| reflect-pad + pack | 2.003 ms | 2.281 ms |
| pack convolution | 6.103 ms | 21.259 ms |
| equal-width trunk | 218.968 ms | 198.595 ms |
| residual/unpack/crop/concat | 17.090 ms | 22.689 ms |
| post/output | 22.744 ms | 36.632 ms |
| owned ROI copy | 2.959 ms | 2.583 ms |
| final stream wait | 22.586 ms | 24.995 ms |

The explicitly named ranges account for 77.4% (Bayer) and 78.9% (X-Trans) of
the CUDA batch, so the remaining gap is still GPU work or range coverage, not
host launch overhead: wall time exceeds the CUDA batch by only 0.04%. The final
stream wait is host time draining already-enqueued GPU work and is not additive
to the CUDA batch duration.

Artifact: `build/perf/demosaicnet_cutlass_post_profile_ranges.json`.

#### Complete persistent-NHWC product result (2026-07-13)

The complete path removes the two section-boundary transposes that invalidated
the first full-frame experiment. Pack and the unequal first trunk remain NCHW;
the result is converted once into a dead ping-pong slot, every equal-width trunk
then remains NHWC, and the residual, factor-2 unpack, centered mosaic crop,
concat, post convolution, output projection, gamma, and pitched-HWC store all
have layout-aware paths. No public-boundary NCHW conversion remains in the
timed path.

Immutable weights are transformed once during model load: Bayer equal-width
trunks use `[Cin,3,3,Cout]`, X-Trans equal-width trunks use CUTLASS KRSC, and
the residual 1x1 uses `[Cin,12]`. The residual kernel maps 16 pixels x 12 output
channels to a 192-thread block so each pixel's threads read adjacent weights.
The final NHWC cooperative load is channel-fast and transposes only inside
shared memory before the fused post/output epilogue.

Correctness and lifetime checks passed:

- `PersistentNhwcBayerForwardMatchesRetainedHwcForward` (`3e-4`);
- `PersistentNhwcXTransForwardMatchesRetainedHwcForward` (`4e-4`);
- `FusedStudentHwcOutputMatchesOrdinaryNchwUnpack`;
- `StudentForwardRepeatedRunsKeepAllocationGenerationStable`.

The 1024 tile gate uses separate 200-iteration runs for each fixture. Both
paths report allocation generation 1 after warm-up and identical owned VRAM.

| Fixture | NCHW p50 | persistent NHWC p50 | Gain | NCHW p95 | NHWC p95 | Owned VRAM |
|---------|---------:|--------------------:|-----:|---------:|---------:|-----------:|
| Bayer D800e | 6.529 ms | **5.073 ms** | **22.3%** | 7.771 ms | **6.050 ms** | 104.053 MiB |
| X-Trans XT5 | 5.219 ms | **4.250 ms** | **18.6%** | 6.069 ms | **5.019 ms** | 115.532 MiB |

Sustained full-frame runs were separated by fixture and started at matched idle
temperatures (82--84 C). This matters on the RTX 3080 Laptop: combining both
fixtures into one long process reaches the thermal limit and makes run order,
not layout, dominate the tail.

| Fixture | NCHW p50 | persistent NHWC p50 | Gain | NCHW p95 | NHWC p95 |
|---------|---------:|--------------------:|-----:|---------:|---------:|
| Bayer D800e | 467.941 ms | **375.511 ms** | **19.8%** | 746.654 ms | **502.858 ms** |
| X-Trans XT5 | 430.847 ms | **375.624 ms** | **12.8%** | 548.282 ms | **396.426 ms** |

The matched-start data includes thermal throttling and therefore is the
retention result, not a claim about the fastest absolute latency. A short
1785-MHz range capture shows the attainable cool-state p50 at 294.477 ms
(Bayer) and 331.851 ms (X-Trans), and locates the improvement:

| CUDA range | Bayer NCHW | Bayer NHWC | X-Trans NCHW | X-Trans NHWC |
|------------|-----------:|-----------:|--------------:|--------------:|
| CUDA batch | 353.524 ms | **270.462 ms** | 377.287 ms | **291.854 ms** |
| equal-width trunk | 216.543 ms | **151.058 ms** | 194.113 ms | **130.842 ms** |
| residual/unpack/crop/concat | 19.071 ms | **11.695 ms** | 22.848 ms | **17.480 ms** |
| post/output | 22.739 ms | **19.201 ms** | 36.341 ms | **31.927 ms** |
| final host stream wait | 22.192 ms | 19.105 ms | 24.569 ms | 23.004 ms |

There is no synchronization inside either model forward. All convolution and
structural kernels enqueue on the caller's stream. Synchronization is confined
to: one cold-load stream wait after immutable weight upload; the explicitly
synchronous public wrappers; and one product-frame `waitForCompletion()` after
all tiles, HLR, orientation, and float4 packing have been enqueued. The profiler
uses event synchronization only while finalizing already-completed samples.
The per-tile product loop calls the asynchronous enqueue API and never waits.

**Verdict: retain persistent NHWC as the product default.** The temporary
`ALCEDO_DEMOASICNET_DISABLE_PERSISTENT_NHWC=1` A/B escape hatch and its ordinary
NCHW implementation are scheduled for deletion in P5; historical JSON is the
record of that comparison. The 100 ms stretch goal is not met; optimization may
resume only after production cleanup establishes one unambiguous baseline.

Artifacts:

- `build/perf/demosaicnet_retained_hwc_bayer_tile_control_v3_repeat.json`
- `build/perf/demosaicnet_persistent_nhwc_bayer_tile_candidate_v3_repeat2.json`
- `build/perf/demosaicnet_retained_hwc_xtrans_tile_control_v3_repeat.json`
- `build/perf/demosaicnet_persistent_nhwc_xtrans_tile_candidate_v3_repeat2.json`
- `build/perf/demosaicnet_retained_hwc_bayer_full_v3_matched_start.json`
- `build/perf/demosaicnet_persistent_nhwc_bayer_full_v3_matched_start.json`
- `build/perf/demosaicnet_retained_hwc_xtrans_full_v3_matched_start.json`
- `build/perf/demosaicnet_persistent_nhwc_xtrans_full_v3_matched_start.json`
- `build/perf/demosaicnet_retained_hwc_profile_ranges_control_v3.json`
- `build/perf/demosaicnet_persistent_nhwc_profile_ranges_v3.json`

### 8.5 Required P4 correctness coverage

After P5, correctness must cover only shipping contracts, not deleted
c

- Bayer and X-Trans persistent-NHWC forwards match their exported FP32 goldens;
- fixed Bayer 1086->1024 and X-Trans 1048->1024 tile geometry preserves CFA
  phase, overlap ownership, seams, and final HWC/gamma semantics;
- allocation generation remains constant after warm-up;
- existing real Bayer/X-Trans RAW goldens, crop/orientation, and HLR-off/on pass;
- model/preprocess/forward failure falls back to Classical/Legacy without using
  another neural layout or convolution implementation.

Delete tests whose only assertion is that a rejected path still works: CUDA
Graph capture/replay, Winograd, implicit-GEMM candidate attributes, ragged
edges, rectangular/strip tiles, multi-lane execution, fused-vs-unfused tail,
and persistent-NHWC-vs-ordinary-NCHW comparisons.

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
9. P4-D persistent channels-last only after its isolated kernels pass;
10. P5 delete all rejected/superseded implementations and freeze one product
    path before starting another optimization track.

Stop a candidate immediately when it breaks correctness, cannot clear its
microbenchmark prerequisite, exceeds its VRAM budget, or loses the full-frame
retention gate. Preserve the best correct product path after every slice.

P0–P5 are complete (P4-A and P4-D retained; P2/P3/P4-B/P4-C rejected; P5 deleted
the rejected implementations). The retained product path is square **1024 +
persistent NHWC + fused HWC tail**. Only a hard failure of that path may fall
back to Classical/Legacy.

## 11. Phase P5 — production consolidation and experiment deletion

P5 is deletion work, not another benchmark candidate. Historical source code is
not required for reproducibility: the roadmap, pinned commits, and JSON artifacts
already record rejected results. Do not preserve dead code behind a macro,
environment variable, harness flag, or `if (false)` branch.

### 11.1 Shipping path that must remain

The final CUDA Neural path is exactly:

1. fixed 1024-owned single-stream tile scheduling;
2. reflect/phase-aware CFA pack and the unequal first NCHW convolution;
3. one NCHW-to-NHWC transition into the two-slot activation ping-pong;
4. Bayer 24-channel in-tree NHWC trunks and X-Trans 32-channel CUTLASS trunks;
5. NHWC residual 1x1 plus fused unpack/crop/concat;
6. NHWC fused post/output/gamma writing pitched HWC;
7. ROI assembly, HLR/orientation, float4 packing, and one final stream wait.

Keep these implementation APIs (renaming the two model entrypoints to the
unqualified product name is encouraged after old overloads are removed):

- `BayerDemosaicNet::ForwardHwcChannelsLast`;
- `XTransDemosaicNet::ForwardHwcChannelsLast`;
- `TransformConv2d3x3WeightsNhwc` and the **C=24 only**
  `Conv2d3x3NhwcBiasRelu` instantiation;
- `TransformConv2d3x3WeightsCutlassKrsc` and
  `Conv2d3x3NhwcCutlassBiasRelu` for **C=32**;
- `TransformDemosaicNetResidualWeightsNhwc`,
  `DemosaicNetResidual1x1Nhwc`, and
  `DemosaicNetUnpackCropConcatNhwc`;
- `FusedPostOutputNhwcToHwc`;
- `PackReflectPaddedCfaTile`, the fixed student `BuildTileJobs` contract,
  `WorkspacePool`, lazy model cache, profiler ranges, async enqueue APIs, and
  synchronous convenience wrappers.

CUTLASS and its BSD-3-Clause notice remain required because the selected
X-Trans product trunk calls it. Generic `cuda/nn` primitives used outside the
DemosaicNet product call graph are not cleanup targets merely because the final
model does not call them.

### 11.2 File and logical-module removal map

| Area | Files/modules | P5 action |
|------|---------------|-----------|
| Convolution candidates | `cuda/nn/conv2d.cu`, `include/cuda/nn/conv2d.hpp` | delete Winograd, implicit-GEMM, small-Cout NCHW, in-tree C=32 NHWC, and candidate-query code; retain generic direct ops needed by pack/first trunk and C=24 NHWC |
| Selected X-Trans trunk | `cuda/nn/cutlass_conv2d.cu/.hpp`, CUTLASS submodule | retain and remove “candidate” naming/comments; this is production |
| Model forwards | `demosaicnet_bayer.cu/.hpp`, `demosaicnet_xtrans.cu/.hpp` | delete ordinary NCHW and old NCHW-to-HWC forwards; retain one persistent-NHWC HWC entrypoint per variant |
| Tail/structure | `fused_post_output.cu/.hpp`, `demosaicnet_nhwc.cu/.hpp` | delete NCHW/unfused switches; retain NHWC residual, structural compose, and fused HWC tail |
| Runtime orchestration | `cuda_demosaicnet.cu/.hpp` | delete Graph, alternate-forward dispatch, tile-size options, and output tensor fallback |
| Workspace/cache | `NeuralDemosaicWorkspace`, `demosaicnet_cache.cpp/.hpp`, activation-slot helpers | delete graph state, output buffer, weight generation, and ordinary-tail slot accounting |
| Tile planner | `cuda_tile_jobs.hpp` | delete large/rectangular/strip/ragged selection; retain fixed Bayer/X-Trans policies and shared Legacy job coverage |
| Product entry | `raw_processor_cuda.cpp` | replace selection/retry branches with one fixed policy; retain high-level Neural-to-Classical/Legacy soft failure |
| Benchmark | `demosaicnet_perf_harness.cpp` | delete every rejected algorithm/shape/lane selector; retain fixed-path full/tile/profile measurement |
| Tests | `conv2d_test.cu`, `demosaicnet_module_test.cu`, `cuda_tile_jobs_test.cpp`, `cuda_raw_ops_test.cpp` | delete rejected-path tests and rewrite goldens to exercise the selected product forward directly |
| Offline assets | `scripts/prepack_demosaicnet_nhwc.py`, `bayer_nhwc.safetensors`, `xtrans_nhwc.safetensors` | delete entire files; original safetensors plus load-time prepack are authoritative |

### 11.3 Delete rejected convolution implementations

Remove the following from `cuda/nn/conv2d.cu`, `include/cuda/nn/conv2d.hpp`,
their tests, and the perf harness:

- `Conv2dParams::winograd_f22_weight`;
- `WinogradFilterTransform3x3`, `WinogradInputTransform4x4`,
  `WinogradOutputTransform4x4`, `Conv2d3x3s1WinogradF22Kernel`,
  `LaunchConv2d3x3WinogradF22`, `WinogradF22SmemBytes`, and
  `TransformConv2d3x3WeightsWinogradF22`;
- `Conv2d3x3s1ImplicitGemmKernel`, `LaunchConv2d3x3ImplicitGemm`, and all
  `kIg*` candidate constants/attribute queries;
- `Conv2d3x3KernelInfo::candidate_*`, the `has_prepacked_winograd` query input,
  and benchmark-only `QueryConv2d3x3NhwcKernelInfo`; remove
  `QueryConv2d3x3KernelInfo` entirely if no retained profiler consumes its
  selected-kernel fields after harness simplification;
- the in-tree **C=32** `Conv2d3x3s1NhwcTiledKernel` dispatch/measurement branch;
  X-Trans ships CUTLASS, while the in-tree NHWC kernel remains only for C=24;
- `Conv2d1x1SmallCoutKernel<3|12>` and its dispatch/tests once the ordinary
  NCHW model forward is gone; the shipping residual/output use NHWC residual and
  the fused HWC tail instead.

Do not replace these with a generic candidate registry or compile-time option.

### 11.4 Delete alternate neural forwards and CUDA Graph

Remove the complete ordinary-NCHW neural execution path:

- `EnvDisablesPersistentNhwc` and
  `ALCEDO_DEMOASICNET_DISABLE_PERSISTENT_NHWC`;
- `FusedPostOutputEnabled` and
  `ALCEDO_DEMOASICNET_DISABLE_FUSED_TAIL`;
- `BayerDemosaicNet::Forward`, `BayerDemosaicNet::ForwardHwc`,
  `XTransDemosaicNet::Forward`, and `XTransDemosaicNet::ForwardHwc`;
- every `force_ordinary_tail` parameter/branch;
- NCHW `FusedPostOutputToHwc` and the NCHW specialization/branch inside the
  fused post/output kernel;
- `RunModelOrdinary`, the graph-aware `RunModel`, output-tensor unpack branches,
  and all `FinishNeuralEngineRgb` calls that exist only because the selected
  tile epilogue might be disabled;
- the `fuse_post_output` workspace-estimate switch, ordinary post slot, and
  tests comparing fused output to an unfused path.

Delete CUDA Graph as one unit:

- `NeuralForwardGraphKey` and `NeuralDemosaicForwardGraph`, including capture,
  replay, invalidation, counters, and abort handling;
- `NeuralDemosaicWorkspace::forward_graph_` and accessors;
- `NeuralDemosaicOptions::enable_cuda_graph`, `EnvDisablesCudaGraph`, and
  `ALCEDO_DEMOASICNET_DISABLE_CUDA_GRAPH`;
- `DemosaicNetModelCache::WeightGeneration` and its generation state, which
  exists only to invalidate graph pointers;
- all graph-only tests from `demosaicnet_module_test.cu`.

After these deletions, `RunModelHwc` must call the selected channels-last model
entrypoint unconditionally.

### 11.5 Delete rejected tile shapes, ragged scheduling, and multi-lane code

Reduce `cuda_tile_jobs.hpp` to the fixed product policies plus shared Legacy
job scheduling. Delete:

- `StudentRaggedEdgeTilesEnabled`, `SetStudentRaggedEdgeTilesEnabled`,
  `kStudentRaggedMinOwnedTail`, `SplitRaggedPair`, the policy
  `ragged_edge_tiles` field, and ragged partition branches;
- `kStudentProductTileEdges`, `kStudentProductRetainedTileEdges`,
  `kStudentStripOwnedHeights`, `kStudentMinOwnedTileEdge`, and
  `kStudentMinOwnedRectAxis` when no fixed-policy validation uses them;
- `StudentOwnedTileShape`, `StudentProductTileShapeOverride`,
  `Set/GetStudentProductTileShapeOverride`, `SelectStudentProductTileShape`,
  VRAM-budget estimation/selection helpers, and smaller-tile retry lists;
- rectangular `MakeBayerStudentTilePolicy(w,h)` /
  `MakeXTransStudentTilePolicy(w,h)` overloads and
  `MakeBayerStudentStripPolicy` / `MakeXTransStudentStripPolicy`;
- `NeuralDemosaicOptions::student_owned_tile_edge`,
  `student_owned_tile_w`, and `student_owned_tile_h`.

In `RawProcessor::ProcessCudaTiled`, replace `cudaMemGetInfo`, `selected`,
`try_warm_tile`, retained-edge retries, and per-job option rewrites with one
fixed policy and one `EnsureCapacity` call. Workspace reservation failure must
fall through to Classical/Legacy; it must not try another neural tile shape.

Delete the corresponding ragged/rectangular/strip/large-tile tests. Preserve
the fixed Bayer/X-Trans CFA phase, destination coverage, X-Trans first-writer
overlap, seam, and real-RAW tests.

The product remains one stream/workspace. Remove the harness-only lane vector,
per-lane streams/workspaces, `wait_all_lanes`, cross-lane output comparison, and
all lane-count JSON fields/tests.

### 11.6 Simplify the performance harness

Keep the harness only as a regression tool for the selected product path:
fixture/raw selection, warm-up/iteration counts, full/tile scopes, JSON output,
and `--profile-ranges` may remain. Profiling changes measurement only and does
not select another algorithm.

Delete these CLI options, config fields, parser/help text, JSON fields, and
their controlled branches:

- `--model` (student is the only model);
- `--tile-size`, `--tile-width`, `--tile-height`, and `--strip-height`;
- `--ragged-edges`;
- `--lanes`;
- `--conv-winograd`;
- `--conv-channels-last`;
- `--conv-cutlass`.

`--mode conv` may remain only if it benchmarks the fixed shipping dispatch:
Bayer C=24 in-tree NHWC, X-Trans C=32 CUTLASS, and the actual pack/post kernels.
It must not contain an algorithm selector or compile/link rejected kernels.

### 11.7 Delete redundant assets, weights, and workspace storage

Return to the original bundled `bayer.safetensors` and `xtrans.safetensors` as
the only model assets. Prepack shipping layouts once during lazy load:

- change `DemosaicNetModelCache::VariantFileName` and model-directory probes
  back to the original filenames;
- in Bayer load, derive CKCO from `trunk.i.weight` with
  `TransformConv2d3x3WeightsNhwc`; do not require `trunk.i.nhwc_weight`;
- X-Trans continues deriving CUTLASS KRSC from original OIHW;
- delete `scripts/prepack_demosaicnet_nhwc.py`,
  `bayer_nhwc.safetensors`, and `xtrans_nhwc.safetensors`;
- update tests and packaging to reference only the original assets.

Once ordinary NCHW forward is removed, stop uploading/storing device weights
used only by it: Bayer/X-Trans equal-width `trunk_w_` entries, `unpack_w_`,
`residual_w_`, and `output_w_`. Keep the pack weight, unequal first-trunk
weight, NHWC/KRSC trunk weights, biases, NHWC residual weight, post weight, and
CIO output weight. Update `ResidentWeightBytes` accordingly.

Remove `NeuralDemosaicWorkspace::output_buffer_`, its accessor, `output_numel`,
and related allocation accounting after every output-tensor fallback branch is
gone. Keep the packed input buffer, activation pool, and pitched HWC RGB buffer.

### 11.8 The only fallback that remains

Retain the existing high-level soft failure from Neural to Classical/Legacy
when model resolution/load, metadata validation, device allocation, preprocess,
or the selected forward fails. This is required product error handling.

Do not retain any of the following as a “fallback”:

- ordinary NCHW DemosaicNet;
- unfused post/output;
- CUDA Graph ordinary replay/capture fallback;
- smaller, larger, ragged, rectangular, or strip neural tiles;
- alternate convolution algorithms;
- a second neural weight asset.

### 11.9 Execution order and completion gate

Apply the cleanup in reviewable deletion slices, keeping the selected path
buildable after each slice:

1. simplify harness/tests so rejected code no longer has callers;
2. delete Graph and alternate NCHW/fused-tail runtime branches;
3. freeze fixed tile policies and delete ragged/rectangular/multi-lane code;
4. delete Winograd/implicit-GEMM/in-tree-C32/small-Cout candidates;
5. return to original safetensors, remove redundant device storage/assets, and
   update CMake/install wiring;
6. run correctness and performance gates, then remove any newly exposed dead
   includes, helpers, comments, and documentation claims.

P5 is complete only when:

- repository search finds no deleted CLI flags, environment variables,
  candidate symbols, `_nhwc.safetensors`, or prepack script references;
- Release builds through `scripts/msvc_env.cmd` for `RawProcessor`, `MlOpsTest`,
  `CudaRawOpsTest`, and `DemosaicNetPerfHarness`;
- selected model goldens, fixed tile planner, async/sync, allocation stability,
  real Bayer/X-Trans RAW, HLR, crop/orientation, and Classical/Legacy soft-fail
  tests pass;
- the simplified harness executes one Bayer and one X-Trans full/tile run with
  no algorithm-selection flags;
- matched-state p50 does not regress by more than 5%, p95 remains within the
  common gate, allocation generation stays constant, and owned VRAM does not
  increase relative to the P4-D artifacts;
- the shipping call graph contains one neural forward, one tile policy per CFA
  family, one convolution choice per retained shape, and one product-level
  fallback to Classical/Legacy.

### 11.10 P5 results (2026-07-13)

Production consolidation landed. Historical rejected results remain in this
roadmap and in `build/perf/*` JSON; source no longer carries alternate neural
paths.

#### Deleted

| Area | Removed |
|------|---------|
| CUDA Graph | `NeuralForwardGraphKey`, `NeuralDemosaicForwardGraph`, workspace graph state, `enable_cuda_graph`, `ALCEDO_DEMOASICNET_DISABLE_CUDA_GRAPH`, graph tests |
| Ordinary NCHW | `Forward` / `ForwardHwc`, `force_ordinary_tail`, `ALCEDO_DEMOASICNET_DISABLE_PERSISTENT_NHWC`, NCHW fused-tail kernels, `FusedPostOutputEnabled` |
| Tile experiments | ragged edges, rectangular/strip policies, VRAM auto-select/retry, multi-lane harness |
| Convolution candidates | Winograd F(2×2,3×3), implicit-GEMM, in-tree C=32 NHWC, candidate query fields |
| Assets | `bayer_nhwc.safetensors`, `xtrans_nhwc.safetensors`, `scripts/prepack_demosaicnet_nhwc.py` |
| Device storage | ordinary equal-width `trunk_w_`, `residual_w_`, `unpack_w_`, `output_w_` OIHW, workspace `output_buffer_` |

#### Shipping path (only)

1. Fixed 1024-owned single-stream tiles (`MakeBayerStudentTilePolicy()` /
   `MakeXTransStudentTilePolicy()`).
2. Reflect/phase CFA pack + unequal first NCHW trunk.
3. One NCHW→NHWC transition; Bayer C=24 in-tree NHWC trunks; X-Trans C=32 CUTLASS.
4. NHWC residual + fused unpack/crop/concat + `FusedPostOutputNhwcToHwc`.
5. ROI assembly + HLR/orientation + float4 pack + one final stream wait.
6. Soft-fail only to Classical/Legacy on neural failure.

Weights load from original `bayer.safetensors` / `xtrans.safetensors`; NHWC/KRSC
and CIO layouts are derived once at load time.

#### Verification

- Release build: `RawProcessor`, `MlOpsTest`, `CudaRawOpsTest`,
  `DemosaicNetPerfHarness` via `scripts/msvc_env.cmd --preset win_release`.
- `MlOpsTest` demosaic/conv/tile filter: **46/46 pass** (goldens via
  `ForwardHwcChannelsLast`, fixed tile planner).
- `CudaRawOpsTest` neural/student filter: **26/26 pass** (real RAW, async,
  allocation stability, load→Legacy soft-fail).
- Simplified harness (no algorithm flags):

```bat
build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
  --fixture all --method neural --mode full --warmup 1 --iterations 3 ^
  --output build/perf/demosaicnet_p5_cleanup_gate.json
```

| Variant | p50 ms (3-iter gate) | jobs | vs P4-D retained ~376 / ~412 |
|---------|---------------------:|-----:|------------------------------|
| Bayer D800e | ~301 | 40 | no regression (well under +5%) |
| X-Trans XT5 | ~321 | 48 | no regression (well under +5%) |

Artifact: `build/perf/demosaicnet_p5_cleanup_gate.json`.

**Verdict: P5 complete.** The product neural path is unambiguous; further
optimization may resume against this single baseline.
