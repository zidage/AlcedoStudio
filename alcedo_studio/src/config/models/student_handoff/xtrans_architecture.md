# Architecture manifest: `xtrans_p2_s32_d4`

Phase 7 Alcedo hard-code specification. Implement this forward
explicitly in C++/CUDA. Do **not** build the graph by interpreting
safetensors metadata at runtime.

## Identity

| Field | Value |
|---|---|
| architecture | `xtrans_p2_s32_d4` |
| architecture_version | 1 |
| variant | `xtrans` |
| width / depth | 32 / 4 |
| pack_factor | 2 |
| parameters | 33679 |
| trainable parameters | 33487 |
| FP32 weight bytes | 134716 |
| checkpoint | `pytorch/runs/iterations/xtrans_aggressive_all_v4_handoff/checkpoints/best.pt` |
| checkpoint SHA-256 | `f985ba64404a4ef9e4662d4f556d184de1e47127ab046f7140fa4b614f4c7546` |
| source_zip | `pytorch/handoffs/xtrans_aggressive_all_v4_handoff.zip` |
| source_zip_sha256 | `dc795eff57033e488e91cd46ba69336923f86b60a7ad65ec15bc765d904d8bc0` |
| teacher_sha256 | `4bde95e316bcb1915a0f85ac9ca897d025b84e7eac443e31cc72877713a19a0b` |
| train_manifest_sha256 | `e7d48d5b52f880452dc142a75db11a6f3f9c6922319c7801b01d2b1c24e71ecb` |
| val_manifest_sha256 | `e6f794dcdc013bdbe8f0eae3c1e5dbf20b87a4b73976d4b5dbddc4de4b890e94` |
| training_commit | `unknown` |
| export format | `demosaicnet-pytorch-state_dict` |

## Tile / CFA contract (single tile)

| Field | Value |
|---|---:|
| tile_input | 1048 |
| pad_to (internal) | 1048 |
| natural output (pre final crop) | 1030 |
| tile_output (export) | 1024 |

## Full-frame CFA-phase-safe tiling (Phase 6 → Alcedo)

These constants are **mandatory** for full-RAW assembly:

| Constant | Value | Formula / note |
|---|---:|---|
| period | 6 | CFA lattice |
| border | 12 | `(input_tile - output_tile) / 2` |
| pad `c` | **12** | `border + (border % period)`; period-aligned reflect pad |
| step | **1020** | `output_tile - (output_tile % period)` |

Tile inputs start at `0, step, 2·step, …` on the period-aligned padded work mosaic. Each tile writes a centered `output_tile` block at `input + border`. When `step < output_tile` (X-Trans 1020), outputs overlap by 4 px — use first-writer ownership, do not average.

# xtrans_p2_s32_d4 hard-coded forward

All ops FP32. No TF32, no autocast. Valid convolutions (no padding) on
learned layers. Bottom/right pad only when pad_to > input (X-Trans p3).

```text
input mosaick [N,3,1048,1048] float32
  -> fixed space-to-depth pack Conv2d k=2 s=2 -> [N,12,H/f,W/f]
  -> trunk: 4 x Conv2d 32-ch 3x3 + ReLU (valid)
  -> residual: Conv2d 1x1 -> [N,12,.,.]
  -> fixed unpack ConvTranspose2d groups=3 k=2 s=2 -> RGB
  -> center-crop padded mosaick to residual RGB spatial size; concat on channel -> 6 ch
  -> post_conv: Conv2d 6->32 3x3 + ReLU (valid)
  -> output: Conv2d 32->3 1x1
  -> center crop to [N,3,1024,1024]
```

## Fixed pack / unpack

- **Pack:** Fixed one-hot space-to-depth Conv2d: in=3, out=12=3*2*2, k=2, stride=2, bias=False. Channel order: for color c in 0..2, for py,px row-major: out_i = c*4 + py*2 + px.
- **Unpack:** Fixed grouped ConvTranspose2d: in=12=3*2*2, out=3, k=2, stride=2, groups=3, bias=False. Per color g, residual channels g*4:(g+1)*4 are row-major sub-pixels. Weight layout [Cin, 1, k, k].

## Ordered layer / tensor shapes

| # | name | op | in_shape | out_shape | k | stride | groups |
|---:|---|---|---|---|---:|---:|---:|
| 0 | `input` | input | `[1, 3, 1048, 1048]` | `[1, 3, 1048, 1048]` |  |  |  |
| 1 | `pack` | Conv2d_fixed | `[1, 3, 1048, 1048]` | `[1, 12, 524, 524]` | 2 | 2 |  |
| 2 | `trunk_1` | Conv2d+ReLU | `[1, 12, 524, 524]` | `[1, 32, 522, 522]` | 3 | 1 |  |
| 3 | `trunk_2` | Conv2d+ReLU | `[1, 32, 522, 522]` | `[1, 32, 520, 520]` | 3 | 1 |  |
| 4 | `trunk_3` | Conv2d+ReLU | `[1, 32, 520, 520]` | `[1, 32, 518, 518]` | 3 | 1 |  |
| 5 | `trunk_4` | Conv2d+ReLU | `[1, 32, 518, 518]` | `[1, 32, 516, 516]` | 3 | 1 |  |
| 6 | `residual` | Conv2d | `[1, 32, 516, 516]` | `[1, 12, 516, 516]` | 1 | 1 |  |
| 7 | `unpack` | ConvTranspose2d_fixed | `[1, 12, 516, 516]` | `[1, 3, 1032, 1032]` | 2 | 2 | 3 |
| 8 | `concat_mosaick` | concat | `[[1, 3, 1032, 1032], [1, 3, 1032, 1032]]` | `[1, 6, 1032, 1032]` |  |  |  |
| 9 | `post_conv` | Conv2d+ReLU | `[1, 6, 1032, 1032]` | `[1, 32, 1030, 1030]` | 3 | 1 |  |
| 10 | `output` | Conv2d | `[1, 32, 1030, 1030]` | `[1, 3, 1030, 1030]` | 1 | 1 |  |
| 11 | `center_crop` | center_crop | `[1, 3, 1030, 1030]` | `[1, 3, 1024, 1024]` |  |  |  |

## Weight keys (safetensors / state_dict)

| key | shape | bytes | trainable |
|---|---|---:|:---:|
| `pack.weight` | `[12, 3, 2, 2]` | 576 | no (fixed) |
| `trunk.0.weight` | `[32, 12, 3, 3]` | 13824 | yes |
| `trunk.0.bias` | `[32]` | 128 | yes |
| `trunk.1.weight` | `[32, 32, 3, 3]` | 36864 | yes |
| `trunk.1.bias` | `[32]` | 128 | yes |
| `trunk.2.weight` | `[32, 32, 3, 3]` | 36864 | yes |
| `trunk.2.bias` | `[32]` | 128 | yes |
| `trunk.3.weight` | `[32, 32, 3, 3]` | 36864 | yes |
| `trunk.3.bias` | `[32]` | 128 | yes |
| `residual.weight` | `[12, 32, 1, 1]` | 1536 | yes |
| `residual.bias` | `[12]` | 48 | yes |
| `unpack.weight` | `[12, 1, 2, 2]` | 192 | no (fixed) |
| `post_conv.weight` | `[32, 6, 3, 3]` | 6912 | yes |
| `post_conv.bias` | `[32]` | 128 | yes |
| `output.weight` | `[3, 32, 1, 1]` | 384 | yes |
| `output.bias` | `[3]` | 12 | yes |

Conv2d weights are `[Cout, Cin, kH, kW]`; ConvTranspose2d (unpack) weights are `[Cin, Cout/groups, kH, kW]`. All little-endian float32, contiguous, row-major.

## FLOP accounting (dense FP32, MAC×2)

| Metric | Value |
|---|---:|
| tile_flops | 20900087040 |
| full_frame_tflop | 1.00320417792 |
| full_frame_flops | 1003204177920 |
| n_tiles (cost table) | 48 |
| fixture_id | xtrans_xt5 |
| budget_status | within_exploratory |

Full-frame topology cost uses non-overlapping `ceil(W/1024)×ceil(H/1024)` tiles. Product X-Trans assembly step 1020 may run extra overlapping work; document product FLOPs separately if needed.

| layer | oh×ow | flops |
|---|---:|---:|
| `pack` | 524×524 | 79077888 |
| `trunk_1` | 522×522 | 1883409408 |
| `trunk_2` | 520×520 | 4984012800 |
| `trunk_3` | 518×518 | 4945747968 |
| `trunk_4` | 516×516 | 4907630592 |
| `residual` | 516×516 | 204484608 |
| `unpack` | 1032×1032 | 25560576 |
| `post_conv` | 1030×1030 | 3666470400 |
| `output` | 1030×1030 | 203692800 |

## Safetensors metadata

```text
architecture = xtrans_p2_s32_d4
architecture_version = 1
cfa_period = 6
checkpoint_sha256 = f985ba64404a4ef9e4662d4f556d184de1e47127ab046f7140fa4b614f4c7546
format = demosaicnet-pytorch-state_dict
pack_factor = 2
phase = 7
source_zip_sha256 = dc795eff57033e488e91cd46ba69336923f86b60a7ad65ec15bc765d904d8bc0
teacher_sha256 = 4bde95e316bcb1915a0f85ac9ca897d025b84e7eac443e31cc72877713a19a0b
tile_border = 12
tile_input = 1048
tile_output = 1024
tile_pad = 12
tile_step = 1020
training_commit = unknown
variant = xtrans
```

## Training metrics (from selection / run)

```json
{
  "best_metric": 0.09295273385941982,
  "colab_provisional": null,
  "selection_cost": {
    "budget_status": "within_exploratory",
    "fixture_id": "xtrans_xt5",
    "fp32_weight_bytes": 134716,
    "full_frame_flops": 1003204177920,
    "full_frame_tflop": 1.00320417792,
    "n_tiles": 48,
    "parameter_count": 33679,
    "tile_flops": 20900087040,
    "trainable_parameter_count": 33487
  },
  "step": 13000,
  "training_provenance": {
    "batch_size": 32,
    "init_checkpoint_sha256": "88de544bb57ba9975671e1248c2c8291212f8c4ded14dfb41d48852db91c9a69",
    "init_from": "phase5 xtrans_p2_s32_d4 best.pt",
    "loss": {
      "chroma_edge": 0.4,
      "chroma_l1": 0.8,
      "sobel_l1": 0.35,
      "teacher_l1": 0.1,
      "zipper_tangent_edge_scale": 0.08,
      "zipper_tangent_margin": 0.002,
      "zipper_tangent_weight": 0.55
    },
    "lr": 2e-05,
    "optimizer": "adam",
    "steps": 15000,
    "teacher_source": "online"
  }
}
```

## Full-RAW comparison assets

```json
{
  "architecture": "xtrans_p2_s32_d4",
  "cases": {
    "DSCF0019": {
      "finite_ok": true,
      "legacy_domain": "display_srgb_rawpy",
      "ownership_ok": true,
      "packing_grid_student": {
        "adj_ratio_x": 1.6738468739587662,
        "adj_ratio_y": 1.663168178772959,
        "grid_score": 1.6685603020591342,
        "pack_factor": 2
      },
      "seam_student": {
        "n_seams": 12,
        "seam_max_mean_abs": 0.019940832629799843,
        "seam_mean_abs": 0.014344046474434435
      },
      "seam_teacher": {
        "n_seams": 12,
        "seam_max_mean_abs": 0.019799526780843735,
        "seam_mean_abs": 0.014000785925115148
      },
      "size_ok": true,
      "student_vs_legacy": {
        "chroma_mae": 0.02401326224207878,
        "gradient_l1": 0.09827443957328796,
        "psnr": 21.91587904372917,
        "rgb_mae": 0.061894528567790985,
        "ssim": 0.9134951829910278
      },
      "student_vs_teacher": {
        "chroma_mae": 0.003240960882976651,
        "gradient_l1": 0.02104523777961731,
        "psnr": 46.68630020480038,
        "rgb_mae": 0.0027939798310399055,
        "ssim": 0.995725691318512
      },
      "teacher_vs_legacy": {
        "chroma_mae": 0.025977663695812225,
        "gradient_l1": 0.11050665378570557,
        "psnr": 21.92834222528893,
        "rgb_mae": 0.06172449141740799,
        "ssim": 0.9066019058227539
      }
    },
    "DSCF2074": {
      "finite_ok": true,
      "legacy_domain": "display_srgb_rawpy",
      "ownership_ok": true,
      "packing_grid_student": {
        "adj_ratio_x": 1.3532216200554428,
        "adj_ratio_y": 1.3914305777463964,
        "grid_score": 1.3731362943321985,
        "pack_factor": 2
      },
      "seam_student": {
        "n_seams": 12,
        "seam_max_mean_abs": 0.010852671228349209,
        "seam_mean_abs": 0.008580787223763764
      },
      "seam_teacher": {
        "n_seams": 12,
        "seam_max_mean_abs": 0.009938743896782398,
        "seam_mean_abs": 0.00806030937625716
      },
      "size_ok": true,
      "student_vs_legacy": {
        "chroma_mae": 0.022559672594070435,
        "gradient_l1": 0.058829233050346375,
        "psnr": 25.883649944197387,
        "rgb_mae": 0.03150239586830139,
        "ssim": 0.9267734289169312
      },
      "student_vs_teacher": {
        "chroma_mae": 0.002537847263738513,
        "gradient_l1": 0.016322996467351913,
        "psnr": 47.81666820054659,
        "rgb_mae": 0.002220479305833578,
        "ssim": 0.993929386138916
      },
      "teacher_vs_legacy": {
        "chroma_mae": 0.022397717460989952,
        "gradient_l1": 0.06232437863945961,
        "psnr": 25.939658425132812,
        "rgb_mae": 0.031702592968940735,
        "ssim": 0.9262393712997437
      }
    }
  },
  "checkpoint": "D:\\Projects\\deepjoint_demosiacing\\demosaicnet_caffe\\pytorch\\runs\\iterations\\xtrans_aggressive_all_v4_handoff\\checkpoints\\best.pt",
  "checkpoint_sha256": "f985ba64404a4ef9e4662d4f556d184de1e47127ab046f7140fa4b614f4c7546",
  "output_root": "D:\\Projects\\deepjoint_demosiacing\\demosaicnet_caffe\\pytorch\\results\\iterations\\xtrans_aggressive_all_v4_handoff"
}
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
