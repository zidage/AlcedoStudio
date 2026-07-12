# Architecture manifest: `bayer_s24_d8`

Phase 7 Alcedo hard-code specification. Implement this forward
explicitly in C++/CUDA. Do **not** build the graph by interpreting
safetensors metadata at runtime.

## Identity

| Field | Value |
|---|---|
| architecture | `bayer_s24_d8` |
| architecture_version | 1 |
| variant | `bayer` |
| width / depth | 24 / 8 |
| pack_factor | 2 |
| parameters | 39135 |
| trainable parameters | 39039 |
| FP32 weight bytes | 156540 |
| checkpoint | `pytorch/runs/phase45_handoff/phase4/bayer_s24_d8/checkpoints/best.pt` |
| checkpoint SHA-256 | `f00fb0e4f4a49e32344ffb0add583bee98c7d5dbfda6c593b5b066d08f9de69f` |
| teacher_sha256 | `02be9ed3238c6761114e75d2b059fe66019292277627d04b38d1a1b91375c2d5` |
| train_manifest_sha256 | `cb992bf2ff6e6a3333829bcd14b3349c6faa67b29b77c3d3b883b06fed098665` |
| val_manifest_sha256 | `e6f794dcdc013bdbe8f0eae3c1e5dbf20b87a4b73976d4b5dbddc4de4b890e94` |
| training_commit | `unknown` |
| export format | `demosaicnet-pytorch-state_dict` |

## Tile / CFA contract (single tile)

| Field | Value |
|---|---:|
| tile_input | 1086 |
| pad_to (internal) | 1086 |
| natural output (pre final crop) | 1052 |
| tile_output (export) | 1024 |

## Full-frame CFA-phase-safe tiling (Phase 6 → Alcedo)

These constants are **mandatory** for full-RAW assembly:

| Constant | Value | Formula / note |
|---|---:|---|
| period | 2 | CFA lattice |
| border | 31 | `(input_tile - output_tile) / 2` |
| pad `c` | **32** | `border + (border % period)`; period-aligned reflect pad |
| step | **1024** | `output_tile - (output_tile % period)` |

> **FORBIDDEN:** `pad = border = 31` for Bayer. That odd pad flips GRBG phase at the network origin and green-casts full-frame teacher/student outputs. Always use **pad=32**.

Tile inputs start at `0, step, 2·step, …` on the period-aligned padded work mosaic. Each tile writes a centered `output_tile` block at `input + border`. When `step < output_tile` (X-Trans 1020), outputs overlap by 4 px — use first-writer ownership, do not average.

# bayer_s24_d8 hard-coded forward

All ops FP32. No TF32, no autocast. Valid convolutions (no padding) on
learned layers. Bottom/right pad only when pad_to > input (X-Trans p3).

```text
input mosaick [N,3,1086,1086] float32
  -> fixed collapse-colors pack Conv2d k=2 s=2 -> [N,4,H/f,W/f]
  -> trunk: 8 x Conv2d 24-ch 3x3 + ReLU (valid)
  -> residual: Conv2d 1x1 -> [N,12,.,.]
  -> fixed unpack ConvTranspose2d groups=3 k=2 s=2 -> RGB
  -> center-crop padded mosaick to residual RGB spatial size; concat on channel -> 6 ch
  -> post_conv: Conv2d 6->24 3x3 + ReLU (valid)
  -> output: Conv2d 24->3 1x1
  -> center crop to [N,3,1024,1024]
```

## Fixed pack / unpack

- **Pack:** Fixed one-hot Conv2d: in=3, out=4=2*2, k=2, stride=2, bias=False. Each output channel sums all input colors at one sub-pixel (Bayer sparse mosaic: only one color live).
- **Unpack:** Fixed grouped ConvTranspose2d: in=12=3*2*2, out=3, k=2, stride=2, groups=3, bias=False. Per color g, residual channels g*4:(g+1)*4 are row-major sub-pixels. Weight layout [Cin, 1, k, k].

## Ordered layer / tensor shapes

| # | name | op | in_shape | out_shape | k | stride | groups |
|---:|---|---|---|---|---:|---:|---:|
| 0 | `input` | input | `[1, 3, 1086, 1086]` | `[1, 3, 1086, 1086]` |  |  |  |
| 1 | `pack` | Conv2d_fixed | `[1, 3, 1086, 1086]` | `[1, 4, 543, 543]` | 2 | 2 |  |
| 2 | `trunk_1` | Conv2d+ReLU | `[1, 4, 543, 543]` | `[1, 24, 541, 541]` | 3 | 1 |  |
| 3 | `trunk_2` | Conv2d+ReLU | `[1, 24, 541, 541]` | `[1, 24, 539, 539]` | 3 | 1 |  |
| 4 | `trunk_3` | Conv2d+ReLU | `[1, 24, 539, 539]` | `[1, 24, 537, 537]` | 3 | 1 |  |
| 5 | `trunk_4` | Conv2d+ReLU | `[1, 24, 537, 537]` | `[1, 24, 535, 535]` | 3 | 1 |  |
| 6 | `trunk_5` | Conv2d+ReLU | `[1, 24, 535, 535]` | `[1, 24, 533, 533]` | 3 | 1 |  |
| 7 | `trunk_6` | Conv2d+ReLU | `[1, 24, 533, 533]` | `[1, 24, 531, 531]` | 3 | 1 |  |
| 8 | `trunk_7` | Conv2d+ReLU | `[1, 24, 531, 531]` | `[1, 24, 529, 529]` | 3 | 1 |  |
| 9 | `trunk_8` | Conv2d+ReLU | `[1, 24, 529, 529]` | `[1, 24, 527, 527]` | 3 | 1 |  |
| 10 | `residual` | Conv2d | `[1, 24, 527, 527]` | `[1, 12, 527, 527]` | 1 | 1 |  |
| 11 | `unpack` | ConvTranspose2d_fixed | `[1, 12, 527, 527]` | `[1, 3, 1054, 1054]` | 2 | 2 | 3 |
| 12 | `concat_mosaick` | concat | `[[1, 3, 1054, 1054], [1, 3, 1054, 1054]]` | `[1, 6, 1054, 1054]` |  |  |  |
| 13 | `post_conv` | Conv2d+ReLU | `[1, 6, 1054, 1054]` | `[1, 24, 1052, 1052]` | 3 | 1 |  |
| 14 | `output` | Conv2d | `[1, 24, 1052, 1052]` | `[1, 3, 1052, 1052]` | 1 | 1 |  |
| 15 | `center_crop` | center_crop | `[1, 3, 1052, 1052]` | `[1, 3, 1024, 1024]` |  |  |  |

## Weight keys (safetensors / state_dict)

| key | shape | bytes | trainable |
|---|---|---:|:---:|
| `pack.weight` | `[4, 3, 2, 2]` | 192 | no (fixed) |
| `trunk.0.weight` | `[24, 4, 3, 3]` | 3456 | yes |
| `trunk.0.bias` | `[24]` | 96 | yes |
| `trunk.1.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.1.bias` | `[24]` | 96 | yes |
| `trunk.2.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.2.bias` | `[24]` | 96 | yes |
| `trunk.3.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.3.bias` | `[24]` | 96 | yes |
| `trunk.4.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.4.bias` | `[24]` | 96 | yes |
| `trunk.5.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.5.bias` | `[24]` | 96 | yes |
| `trunk.6.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.6.bias` | `[24]` | 96 | yes |
| `trunk.7.weight` | `[24, 24, 3, 3]` | 20736 | yes |
| `trunk.7.bias` | `[24]` | 96 | yes |
| `residual.weight` | `[12, 24, 1, 1]` | 1152 | yes |
| `residual.bias` | `[12]` | 48 | yes |
| `unpack.weight` | `[12, 1, 2, 2]` | 192 | no (fixed) |
| `post_conv.weight` | `[24, 6, 3, 3]` | 5184 | yes |
| `post_conv.bias` | `[24]` | 96 | yes |
| `output.weight` | `[3, 24, 1, 1]` | 288 | yes |
| `output.bias` | `[3]` | 12 | yes |

Conv2d weights are `[Cout, Cin, kH, kW]`; ConvTranspose2d (unpack) weights are `[Cin, Cout/groups, kH, kW]`. All little-endian float32, contiguous, row-major.

## FLOP accounting (dense FP32, MAC×2)

| Metric | Value |
|---|---:|
| tile_flops | 24367838784 |
| full_frame_tflop | 0.97471355136 |
| full_frame_flops | 974713551360 |
| n_tiles (cost table) | 40 |
| fixture_id | bayer_d800e |
| budget_status | within_preferred |

Full-frame topology cost uses non-overlapping `ceil(W/1024)×ceil(H/1024)` tiles. Product X-Trans assembly step 1020 may run extra overlapping work; document product FLOPs separately if needed.

| layer | oh×ow | flops |
|---|---:|---:|
| `pack` | 543×543 | 28305504 |
| `trunk_1` | 541×541 | 505752768 |
| `trunk_2` | 539×539 | 3012121728 |
| `trunk_3` | 537×537 | 2989809792 |
| `trunk_4` | 535×535 | 2967580800 |
| `trunk_5` | 533×533 | 2945434752 |
| `trunk_6` | 531×531 | 2923371648 |
| `trunk_7` | 529×529 | 2901391488 |
| `trunk_8` | 527×527 | 2879494272 |
| `residual` | 527×527 | 159971904 |
| `unpack` | 1054×1054 | 26661984 |
| `post_conv` | 1052×1052 | 2868576768 |
| `output` | 1052×1052 | 159365376 |

## Safetensors metadata

```text
architecture = bayer_s24_d8
architecture_version = 1
cfa_period = 2
checkpoint_sha256 = f00fb0e4f4a49e32344ffb0add583bee98c7d5dbfda6c593b5b066d08f9de69f
format = demosaicnet-pytorch-state_dict
pack_factor = 2
phase = 6
teacher_sha256 = 02be9ed3238c6761114e75d2b059fe66019292277627d04b38d1a1b91375c2d5
tile_border = 31
tile_input = 1086
tile_output = 1024
tile_pad = 32
tile_step = 1024
training_commit = unknown
variant = bayer
```

## Training metrics (from selection / run)

```json
{
  "best_metric": 0.057003541849553585,
  "colab_provisional": {
    "aggregate": {
      "chroma_mae": 0.018930843926646047,
      "gradient_l1": 0.12076827897236217,
      "psnr": 32.11614860244519,
      "rgb_mae": 0.015933009538042824,
      "ssim": 0.9493877917975188,
      "teacher_rgb_mae": 0.05433703776384936
    },
    "architecture": "bayer_s24_d8",
    "cost": {
      "budget_status": "within_preferred",
      "fp32_weight_bytes": 156540,
      "full_frame_tflop": 0.97471355136,
      "parameter_count": 39135,
      "tile_flops": 24367838784
    },
    "phase_audit": "/content/drive/MyDrive/demosaicnet_fp32_runs/phase4/bayer_s24_d8/audit_stage_b.json",
    "phase_audit_passed": true,
    "rank": 1,
    "rank_key": [
      1,
      32.11614860244519,
      -0.018930843926646047,
      -0.12076827897236217
    ],
    "report": "/content/drive/MyDrive/demosaicnet_fp32_runs/phase4/bayer_s24_d8/eval_stage_b.json"
  },
  "selection_cost": {
    "budget_status": "within_preferred",
    "fixture_id": "bayer_d800e",
    "fp32_weight_bytes": 156540,
    "full_frame_flops": 974713551360,
    "full_frame_tflop": 0.97471355136,
    "n_tiles": 40,
    "parameter_count": 39135,
    "tile_flops": 24367838784
  },
  "step": 99000,
  "training_provenance": null
}
```

## Full-RAW comparison assets

```json
[
  {
    "active_hw": [
      3753,
      5634
    ],
    "case_id": "bayer_regression_5d_mark_ii_iso800",
    "metrics": {
      "finite_ok": true,
      "legacy_domain": null,
      "ownership_ok": true,
      "packing_grid_student": {
        "adj_ratio_x": 1.399757332919629,
        "adj_ratio_y": 1.396520710962476,
        "grid_score": 1.398078126772345,
        "pack_factor": 2
      },
      "seam_student": {
        "n_seams": 8,
        "seam_max_mean_abs": 0.013091185130178928,
        "seam_mean_abs": 0.00975631276378408
      },
      "seam_teacher": {
        "n_seams": 8,
        "seam_max_mean_abs": 0.013485858216881752,
        "seam_mean_abs": 0.009727138094604015
      },
      "size_ok": true,
      "student_vs_legacy": null,
      "student_vs_teacher": {
        "chroma_mae": 0.002005237154662609,
        "gradient_l1": 0.012609144672751427,
        "psnr": 49.73008174258598,
        "rgb_mae": 0.002051505958661437,
        "ssim": 0.9943485260009766
      },
      "teacher_vs_legacy": null
    },
    "output_dir": "pytorch/results/phase6_product_full_raw/bayer_regression_5d_mark_ii_iso800",
    "source_id": "5d_mark_ii_iso800.tiff",
    "source_kind": "tiff"
  },
  {
    "active_hw": [
      4924,
      7378
    ],
    "case_id": "bayer_raw_Nikon-D800e-raw-00002",
    "metrics": {
      "finite_ok": true,
      "legacy_domain": "display_srgb_rawpy",
      "ownership_ok": true,
      "packing_grid_student": {
        "adj_ratio_x": 1.2408594619981588,
        "adj_ratio_y": 1.3579659711618712,
        "grid_score": 1.3012905066469196,
        "pack_factor": 2
      },
      "seam_student": {
        "n_seams": 11,
        "seam_max_mean_abs": 0.004483461380004883,
        "seam_mean_abs": 0.0030666230331090364
      },
      "seam_teacher": {
        "n_seams": 11,
        "seam_max_mean_abs": 0.004049539566040039,
        "seam_mean_abs": 0.0026422707914290104
      },
      "size_ok": true,
      "student_vs_legacy": {
        "chroma_mae": 0.00651275971904397,
        "gradient_l1": 0.019320670515298843,
        "psnr": 24.65183982331208,
        "rgb_mae": 0.04787789657711983,
        "ssim": 0.9628258347511292
      },
      "student_vs_teacher": {
        "chroma_mae": 0.0010849045356735587,
        "gradient_l1": 0.004692611284554005,
        "psnr": 58.27337924647104,
        "rgb_mae": 0.0009174894657917321,
        "ssim": 0.9991058707237244
      },
      "teacher_vs_legacy": {
        "chroma_mae": 0.005225091706961393,
        "gradient_l1": 0.02536286972463131,
        "psnr": 24.783968266020114,
        "rgb_mae": 0.047103315591812134,
        "ssim": 0.960601806640625
      }
    },
    "output_dir": "pytorch/results/phase6_product_full_raw/bayer_raw_Nikon-D800e-raw-00002",
    "source_id": "Nikon-D800e-raw-00002.nef",
    "source_kind": "raw"
  }
]
```

## Alcedo migration checklist (do not modify Alcedo from this repo)

Copy into the Alcedo tree (exact destinations are product-owned; names below
match the existing Neural Engine layout conventions):

| Handoff artifact | Suggested Alcedo destination / use |
|---|---|
| `bayer_student.safetensors` | Neural weight store / loader for Bayer student |
| `xtrans_student.safetensors` | Neural weight store / loader for X-Trans student |
| `bayer_architecture.md` | Source for hard-coded Bayer forward + constants |
| `xtrans_architecture.md` | Source for hard-coded X-Trans forward + constants |
| `golden/` | Unit-test vectors for C++/CUDA bit-exact / tight-tol checks |

Expected module / constant changes (hard-code from the architecture manifests;
do **not** interpret safetensors metadata as a dynamic graph):

1. **Topology / op graph**
   - Replace full teacher DemosaicNet stack with the student ordered forward
     (fixed pack Conv2d → trunk Conv3×3/ReLU × depth → residual 1×1 →
     fixed unpack ConvTranspose2d groups=3 → concat mosaic skip →
     post Conv3×3/ReLU → 1×1 RGB → center crop 1024).
   - Bayer pack collapses colors (`out_ch = pack_factor² = 4`).
   - X-Trans pack is space-to-depth (`out_ch = 3 * pack_factor² = 12` for p2).
   - Keep pack/unpack weights fixed (one-hot); they are still exported so the
     loader can memcpy them or assert equality against compile-time constants.

2. **Tile contracts**
   - Bayer: input 1086 → export 1024; X-Trans: input 1048 → export 1024.
   - Full-frame tiling must use Phase 6 CFA-phase-safe constants:
     - Bayer: period=2, border=31, **pad=32**, step=1024.
     - X-Trans: period=6, border=12, pad=12, step=**1020**.
   - **Forbidden:** Bayer reflect pad = 31 (phase flip → green cast on teacher
     and invalid student evaluation). See
     `pytorch/reports/phase6_full_raw_validation.md`.

3. **Operators required (existing CUDA set)**
   - FP32 Conv2d + bias + ReLU (valid, no padding on learned convs).
   - Fixed-weight stride pack Conv2d and grouped ConvTranspose2d unpack.
   - Center crop, channel concat, reflect pad for full-frame context only.
   - No BatchNorm, LayerNorm, attention, or dynamic control flow.

4. **Weight layout**
   - Contiguous little-endian float32, PyTorch NCHW / ConvTranspose layout
     (`[out,in,kh,kw]` and `[in, out/groups, kh, kw]`).
   - Load by state_dict key names listed in the architecture manifest.

5. **Golden tests**
   - Load `golden/<variant>_input_*.pt` / `*_output_*.pt` and require tight
     agreement with the hard-coded forward (bit-exact preferred on CPU ref;
     device may use a documented ULP/abs tol).
