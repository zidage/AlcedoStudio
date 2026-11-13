# Black Pro-Mist Diffusion Filter Simulation Plan

Date: 2026-09-30

Status: **CUDA MVP implemented** on `feature/black-pro-mist`; **OpenCL parity** on
`feature/black-pro-mist-opencl`; **Metal parity** on `feature/black-pro-mist-metal`. The
implemented design differs
from this plan by user decision on 2026-09-30. Section 0 records the implemented design. It
replaces sections 1.3, 1.4 (the "DRT-owned scene-referred step" item), 3.3, 3.4, 6, and the phase
split in sections 10 to 17. The algorithm of section 3.1 and the pyramid of section 3.2 apply,
with the changes in section 0.

Parent: [Roadmap](../../roadmap.md), section "Still Planned".

Direct prerequisites:

- [GPU DAG Pipeline Rebuild](gpu_dag_pipeline_rebuild_phase_plan.md) (complete runtime that this
  plan extends).
- [GPU DAG OpenCL Migration](gpu_dag_opencl_migration_phase_plan.md) (OpenCL 1.2 baseline and
  program registration rules).
- [GPU DAG Metal Migration](gpu_dag_metal_migration_phase_plan.md) (Metal pass structure).
- [NM10 Node-aware Adjustment Transfer](node_mask_editor/phase_nm10_adjustment_transfer_plan.md)
  (Develop transfer items and package schema rules).
- [Alcedo Studio QML Visual Identity](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md).

Source audit revision: `03d83dee2` on `feature/lut-settings-panel`.

All source paths in this plan are relative to `alcedo_studio/src/` unless the path starts with
`alcedo_studio/`, `docs/`, or `tests/`. Test paths are relative to `alcedo_studio/`.

---

## 0. Implemented design (user decision, 2026-09-30)

### 0.1 Decisions

- The phase split does not apply. The feature ships as one change.
- The document stores only the strength. The Model layer resolves every other parameter.
- The filter is one pass, `DiffusionFilter`, between the last Color Grade and the DRT display
  transform. The pass is in every compiled plan. With strength 0 it only decodes ACEScc to linear
  AP1. The DRT display kernels read linear AP1 and do not decode ACEScc. The transfer function is
  therefore decoded once per frame, by the DiffusionFilter pass.
- CUDA, OpenCL, and Metal implement the scatter with the same kernels and results.

### 0.2 Storage and Model

- `DrtParamsModel` owns `diffusion_strength` in [0, 1]. The JSON key is
  `params.diffusion.strength` of the DRT node. `ToJson` omits the object when the strength is 0,
  so current documents and expected serialized files do not change. `LoadJson` reads a missing
  key as 0 and rejects a non-finite or out-of-range value.
- `DrtDirty::Diffusion` marks a strength change. The display content key already hashes the DRT
  params JSON, and a DRT revision change already invalidates the display output.
- `ResolveDiffusionFilterShape(strength)` (`include/edit/graph/diffusion_filter_model.hpp`)
  gives `scatter_fraction = 0.16 * strength` and fixed values for glow radius (0.12), base sigma
  (0.002), power-law exponent (2.6), black mist (0.5), absorption (0.1), highlight gain (6.0),
  and highlight knee (0.8). The first calibration used 0.4; slider 100 now gives the look of its
  slider 40.
- The editor field key is `diffusion` (`EditorScalarWrite`, JSON `{"strength": s}`), owned by the
  document DRT node. The `odt` field reads `DrtParamsModel::OutputTransformJson`, which excludes
  the strength, so ODT history rows keep their format.
- The UI is the Post Processing page (`EditorPostProcessPanel.qml`, panel id `post`, sparkle
  icon) of the right-side adjustment stack. It holds Diffusion, Clarity, Sharpen, Film Grain,
  and Halation. The page is available for Color Grade and DRT selections. The Look panel no
  longer shows the DRT/Post sliders. The old unused Detail page (`detail`) became this page.

### 0.3 Runtime

- The scatter image lives on a full-frame canvas, not on the render. `DiffusionCanvasExtent`
  (`include/edit/runtime/diffusion_filter_plan.hpp`) is the full reference frame scaled down to a
  long edge of at most 2048 (`kDiffusionCanvasMaxLongEdge`, the same limit as the LLF canonical
  reference, `local_tone_mapping::kReferenceMaskMaxLongEdge`). The limit applies to preview,
  detail, and export renders alike.
- `MakeDiffusionFilterLayout(canvas, shape)` computes the pyramid of section 3.2 in canvas
  texels. Every sigma is a fraction of the canvas short side, so one frame always has one
  pyramid, whatever the render resolution, zoom, or viewport.
- `MakeDiffusionScatterMapping(geometry, layout)` maps the render onto the base level:
  `render_to_base` for the mix, `base_to_render` for the reduction. Both go through
  `render_to_reference`, like the LLF sampling plan. The reduction averages `reduce_samples`
  squared bilinear render samples per base texel; the count follows the texel footprint in render
  pixels (at most 16 per axis), so a full-resolution export averages more samples into the same
  texel than a preview does.
- The scatter image is a published result, `DiffusionScatterId(drt)` =
  `drt:diffusion.scatter.0`, with the canonical (viewport-free) representation identity of the
  LLF ports. Result invalidation adds the edges DRT scene input -> scatter -> display and marks the
  scatter stale when `DrtDirty::Diffusion` changes or the DRT scene input changes.
- `DecideDiffusionScatter` chooses per encode. A render that covers the full edit space samples
  the published image when it was built from a render with at least its own long edge; otherwise
  it rebuilds and publishes. A viewport ROI render samples any current published image, so the
  glow at a pixel does not change with zoom and light outside the viewport still scatters in.
  Without a current published image an ROI render builds a submission-local image from its own
  pixels and does not publish it. QualityBase renders do not read or publish it.
- `PlanExecutor` runs DiffusionFilter and then the DRT only when the display result misses. The
  pass writes the scene-work member that `DestinationWorkMember` selects.
- The CUDA pass (`edit/runtime/cuda/cuda_diffusion_filter_pass.cu`): the boosted reduction onto
  the base level, 13-tap downsample, 9-tap tent upsample and accumulate into the scatter image,
  then a mix that samples the scatter image with a cubic B-spline at `render_to_base`.
  Intermediate pyramid levels are pooled scratch textures no larger than the canvas. A canvas
  with a short side of 707 texels or more starts at level 1 (for a 3:2 frame, 1024 x 683).
- The OpenCL pass (`edit/runtime/opencl/opencl_diffusion_filter_pass.cpp`, kernels appended to
  the `opencl_dag_drt` program in `shader/drt.cl`) runs the same kernels, the same
  sample/rebuild/publish decision, and the same `DiffusionScatterId` result. Pyramid levels and
  the scatter image are pooled `Rgba32f` images; the scene input and output keep the
  dual-storage (image or scene-work buffer) binding. `GpuDagOpenClGradeTest` runs the CUDA test
  cases against the shared CPU reference (`tests/edit/runtime/diffusion_filter_reference.hpp`).
- The Metal pass (`edit/runtime/metal/metal_diffusion_filter_pass.mm`, kernels appended to
  `shader/drt.metal` in the DRT metallib) runs the same kernels, the same sample/rebuild/publish
  decision, and the same `DiffusionScatterId` result. Pyramid levels and the scatter image are
  pooled `Rgba32f` textures; every dispatch goes into the frame's serial compute encoder, and
  the tracked textures order each level after the level it reads. Pipeline states come from the
  Metal pipeline cache and are warmed with the DRT pipelines. `GpuDagMetalDrtTest` runs the CUDA
  test cases against the shared CPU reference.
- `kDrtImplementationVersion` is 5.

### 0.4 Known limits of the implemented design

- The scatter reads the Color Grade output of the render that builds it, not the Develop output.
  The highlight boost is not linear, so a scatter image built from a reduced-resolution preview
  differs a little from one built at export resolution (section 1.4 measured this error). The
  user accepted this placement. A later full-edit render with a longer edge rebuilds the image.
- An ROI render with no current published scatter image (for example when it runs before the
  first full-frame render of a new revision) sees only its own pixels. The next full-frame render
  publishes the image.
- Adjustment Transfer does not copy the strength yet.

---

## 1. Decision record

### 1.1 Approved goal

The user asked to record the algorithm design for a simulated Tiffen Black Pro-Mist filter.
The user also asked for an implementation plan. The effect is a new editor effect.
This document is a design and a plan. It does not implement the effect.

A Black Pro-Mist filter is an optical diffusion filter. It sits in front of the lens.
It scatters a small fraction of the incoming light into a wide, soft glow.
Bright light sources get a visible glow. Midtone texture stays sharp.
Black specks in the filter absorb a little light and reduce the flat veil.
The reduced veil keeps shadows and contrast. This is the difference from a plain Pro-Mist.

### 1.2 Research inputs

- Reference article: [Simulating a Tiffen Black Pro-Mist filter in post](https://www.eoshd.com/comments/topic/27604-simulating-a-tiffen-black-pro-mist-filter-in-post/).
  The article is a forum post. It uses a display-referred dual blur. A large blur acts on
  mids and shadows. A smaller blur acts on highlights. The user mixes the two to taste.
  The article gives no numbers. It says to apply the effect before the log-to-display
  conversion, because a real filter acts before the sensor. It names two limits: clipped
  highlights and light sources outside the frame.
- Pyramid filters: Jorge Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
  Warfare", SIGGRAPH 2014 Advances in Real-Time Rendering course. This plan uses its 13-tap
  downsample filter and its 9-tap tent upsample filter.
- Local simulation: Python scripts `bpm.py`, `compare.py`, and `validate.py` in
  `artifacts/bpm_sim/`. That directory is gitignored. The scripts are local evidence only.
  They are not repository files. Section 1.5 records their results.

### 1.3 Selected design

The effect uses energy-conserving scatter in scene-linear light. It does not use a threshold.
Light sources are orders of magnitude brighter than midtones in linear light.
Therefore a linear scatter makes a visible glow around light sources and a small change in
midtones. No threshold or luminance key is necessary for highlight selectivity.

The effect runs in the Develop section of the pipeline:

1. A new `DiffusionScatter` pass reads the cached full-resolution Develop output.
2. The pass writes one small scatter image at a fixed fraction of the full resolution.
3. The existing `CameraToAp1` pass mixes the scatter image into the render-resolution image.

The Develop node owns the effect parameters. Section 1.4 gives the reasons.

### 1.4 Rejected options

These options must not return without a new user decision:

- **Display-referred threshold and dual blur (the article method).** It needs a threshold.
  It blurs midtones. Faces and fine texture become soft in the simulation.
- **A fifth DRT/Post adjustment.** `DrtPostExecutor::Execute` runs the display transform first.
  Clarity, Sharpen, Halation, and Film Grain then act on display-referred values. A diffusion
  filter must act on scene-linear light. Also, `RequireCompleteDrtPostTypes` requires exactly
  four DRT/Post adjustments in `DrtNodeModel::ToJson` and `DrtNodeModel::FromJson`. A fifth type
  makes every current project fail to load.
- **A DRT-owned scene-referred step before the display transform.** The DRT input has the
  render resolution. The Interactive render is smaller than the export render. The highlight
  boost (section 3.1) is not linear. The simulation measured a 21.8% relative error of the
  effect when the boost ran on a 1/4-scale image. Preview and export would not agree.
- **A Color Grade adjustment.** `ColorGradeNodeModel` rejects neighborhood operations
  (`include/edit/graph/color_grade_node_model.hpp`). The Color Grade input also has the render
  resolution. The same boost error applies.
- **A preview-only or reduced-resolution substitute.** `AGENTS.md` forbids a fallback without
  explicit user approval. The selected design gives the same scatter image to Interactive,
  QualityBase, Detail, and export renders.

### 1.5 Evidence status at plan creation

The Python simulation used two Panasonic RW2 night images, 6008 x 4008 pixels. The results are
local evidence. They are not automated repository tests.

| Check | Result |
| --- | --- |
| PSF impulse energy | 1.0000000 |
| PSF radial profile | Follows r^-2.6 from about 4 px to about 12% of the short side, then cuts off. |
| Shift variance with `pyrDown`/`pyrUp` | Off-grid impulse peak ratio 0.895. This result selects the 13-tap and tent filters. |
| Energy with boost 0 and black 0 | Mean input 0.090395, mean output 0.090394. |
| Preview (1/4 scale) against export then downscale, linear scatter only | 2.9% relative error of the effect. |
| Same, boost on the 1/4-scale image | 21.8% relative error of the effect. |
| Same, boost at full resolution before the downscale | 3.8% relative error of the effect. |
| CPU cost, NumPy and OpenCV, pyramid only, 24 MP | 302 ms. |
| GPU cost | Not measured. Estimate: two full-resolution reads, one write, and small levels. About 1-2 ms. |
| Visual result | Headlights, street lamps, and Ferris wheel lights glow. Railings, road texture, and faces stay sharp. The article dual blur makes midtones soft. |

The strength values are not calibrated against a physical filter. Section 9 records this item.

---

## 2. Terms

This plan uses one term for each concept.

| Term | Meaning |
| --- | --- |
| Diffusion filter | The simulated optical filter. The product feature. Code prefix `Diffusion`. |
| Develop output | The cached `develop:sensor_linear` image. Camera scene-linear RGB at the decoded extent. |
| Scatter image | The small image `B` that holds the scattered light. One per image and parameter set. |
| Scatter pyramid | The chain of half-resolution levels that builds the scatter image. |
| Base level | The first pyramid level that has a non-zero weight. Level index `L0`. |
| Highlight boost | The factor that raises near-clip pixels in the scatter branch only. |
| Black mist | The amount of black specks. It reduces the widest levels and absorbs light. |
| Mix | The final operation `T * ((1 - s) * I + s * B)`. |
| Short side | `min(width, height)` of the develop output extent, in pixels. |

---

## 3. Product design specification

### 3.1 Algorithm

All operations use camera scene-linear RGB. Each channel is independent.

```text
B   = PSF (*) Boost(I)
out = T * ((1 - s) * I + s * Warm(B))
T   = 1 - 0.1 * black_mist * s

PSF = sum over k = 0 .. K-1 of  w_k * Gauss(sigma_0 * 2^k)
w_k is proportional to sigma_k^(2 - p) * (1 - black_mist * (k / (K - 1))^2)
sum of w_k = 1
```

- `I` is the Develop output.
- `s` is the stored `strength`.
- The PSF approximates a heavy-tailed profile `r^-p` with `p = 2.6`. It has a small core.
  It stops at the maximum radius.
- A sum of Gaussians with `w_k` proportional to `sigma_k^(2 - p)` approximates the power law.
- `black_mist` reduces the widest levels. The veil and the shadow lift become smaller.
- When `K` is 1, the veil term is 0 and `w_0` is 1.

Highlight boost:

```text
r     = max over c of (I_c / clip_c)
t     = smoothstep(knee = 0.8, 1.0, r)
Boost = I * (1 + highlight_glow * t)
```

- The boost applies to the scatter branch only. The direct term `(1 - s) * I` has no boost.
- Clipped sensor data holds less energy than the real light source. The boost restores a
  plausible glow energy. Without it, lamps glow much less than with a real filter.
- `clip_c` is the clip level of channel `c` in the Develop output. Phase DF2 must find the owner
  of this value and record it. Inference, not a verified fact: after `InverseCamMulPack`, the clip
  level is 1.0 in all channels, because linearization normalizes the white level. The value
  comes from `PreparedRawInput::linearization` (`RawLinearizationParams`). For direct RGB input,
  the clip level is 1.0.
- The simulation used Rec.709 luma for `r`. This plan uses the maximum clip-relative channel,
  because a sensor clips each channel separately. Section 9 records this item as a decision
  for the user to confirm.

Warmth:

```text
Warm(B) = W * B
W       = inverse(M) * diag(1 + warmth, 1, 1 - warmth) * M
```

- `M` is the camera-to-AP1 matrix that `CameraToAp1` already uses.
- The tint is defined in AP1. The pass applies it in camera space through `W`.
- When `warmth` is 0, `W` is the identity matrix. The pass skips the multiply.

### 3.2 Pyramid implementation

The implementation uses a dyadic pyramid. The cost is O(N). It uses no direct convolution and
no tap limit. `kGradeNeighborMaxTapCount` does not apply.

```text
sigma_0     = max(0.002 * short_side, 1.0)             develop pixels
sigma_max   = glow_radius * short_side                 develop pixels
L0          = clamp(round(log2(sigma_0)), 0, 20)       base level index
K           = ceil(log2(sigma_max / sigma_0)) + 1      level count, at least 1
level j     = develop extent / 2^j, each axis rounded up, at least 1 pixel
```

If level `L0 + K - 1` has fewer than 1 pixel on an axis, `K` becomes the largest value that
keeps 1 pixel on both axes. The weights are then normalized again.

For 6008 x 4008 and the default glow radius: `sigma_0 = 8.016`, `L0 = 3`, `K = 7`.
The base level is 751 x 501. The coarsest level is 1/512 of the develop extent.

Kernels, in order:

1. **Reduce and boost.** One kernel reads the develop output. It applies the highlight boost
   to each tap. It writes the base level directly. Each output texel uses a separable tent
   footprint of `2 * 2^L0` develop pixels on each axis. No full-resolution or half-resolution
   intermediate texture exists.
2. **Downsample.** For each level `L0 + 1 .. L0 + K - 1`, one kernel applies the 13-tap
   downsample filter to the previous level.
3. **Upsample and accumulate.** From the coarsest level to the base level, one kernel applies
   the 9-tap tent upsample filter to the accumulated coarser result. It adds `w_k * level_k`.
   The final result at the base level is the scatter image `B`.
4. **Mix.** The `CameraToAp1` kernel samples `B` at the decoded coordinate of each render pixel.
   It uses a cubic B-spline sample made of four bilinear fetches. It computes the mix before
   the camera-to-AP1 matrix, the DNG profile, the reference gamut compression, and the ACEScc
   encode.

All kernels use clamp-to-edge addressing.

The scatter image and the pyramid levels use `TextureFormat::Rgba32f`. The current source has
no half-float texture format.

### 3.3 Parameters

The Develop node stores one `diffusion_filter` JSON object in its `params`.

| JSON key | Type | Range | Default | Proposed UI label | Effect |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | - | `false` | Diffusion | Turns the effect on or off. |
| `strength` | float | 0.0 to 0.5 | 0.20 | Strength | The scatter fraction `s`. |
| `glow_radius` | float | 0.02 to 0.25 | 0.12 | Glow Size | `sigma_max` as a fraction of the short side. |
| `black_mist` | float | 0.0 to 1.0 | 0.5 | Black Mist | Veil reduction and light absorption. |
| `highlight_glow` | float | 0.0 to 12.0 | 6.0 | Highlight Glow | The boost gain. |
| `warmth` | float | -0.2 to 0.2 | 0.0 | Warmth | The tint of the scattered light. |

Fixed constants. They are not user parameters. A change to a constant increments
`kDiffusionScatterImplementationVersion`.

| Constant | Value |
| --- | --- |
| `kDiffusionBaseSigmaFraction` | 0.002 |
| `kDiffusionPowerLawExponent` | 2.6 |
| `kDiffusionBoostKnee` | 0.8 |
| `kDiffusionBlackAbsorption` | 0.1 |

The effect is active when `enabled` is true and `strength` is greater than 0.

The stored `strength` is `s`. It is not a slider percentage. The UI can show another scale.
The filter grade calibration (section 9, U3) can change UI labels or presets. It does not change
the meaning of a stored value.

### 3.4 Resolution and consistency rules

- The scatter image depends only on the develop output and on `glow_radius`, `black_mist`, and
  `highlight_glow`. It does not depend on the render extent, the viewport, or the frame role.
- The highlight boost runs at the develop output resolution. The editor decodes RAW files with
  `DecodeRes::FULL` (`edit/input/raw_input_loader.cpp`). Therefore Interactive, QualityBase,
  Detail, and export renders read the same scatter image.
- The mix is linear. A resample of the mixed image equals the mix of the resampled parts.
  Therefore a preview equals a downscaled export within the resample tolerance.
- A Detail ROI render samples the same full-frame scatter image. Light from outside the ROI
  glows into the ROI.
- Light from outside the crop also glows into the crop. A real filter scatters light before
  a crop exists. This behavior is intended.
- Where the render pixel maps outside the decoded extent (the geometry border), the mix does
  not apply. The border color stays unchanged.
- The PSF is a fraction of the short side. Its shape in develop pixels depends on the develop
  output extent. Every render of one image uses one develop output extent. Therefore every
  render of one image uses the same PSF.

### 3.5 Success behavior

- With the effect active, lamps and other light sources get a soft glow with a bright core and
  a long tail.
- Midtone edges and texture stay sharp. The midtone change is small.
- A higher `black_mist` gives less haze and less shadow lift.
- A higher `highlight_glow` gives more glow around clipped light sources only.
- An exposure or tone change after Develop changes the glow the same way it changes the light
  source. This matches a capture with a real filter.
- A change to `strength`, `warmth`, or `enabled` does not recompute the scatter image.
- A change to `glow_radius`, `black_mist`, or `highlight_glow` recomputes the scatter image.
  It does not decode the RAW file again.
- Undo, Redo, Version checkout, Paste, reopen, and export give the same result.

### 3.6 Failure behavior

- An editor write with a non-finite or out-of-range value fails. The document does not change.
  The caller receives an error that names the key and the range.
- A document load with a non-finite or out-of-range stored value fails with an error that names
  the key. The loader does not clamp the value.
- A scatter pass failure throws from the backend pass. The render publishes no frame.
  The cached develop output stays valid. The current displayed frame stays on screen.
- No backend selects another backend, the CPU, a smaller decode, or a pass without the effect
  after a failure.

### 3.7 Serialization and compatibility

- `DevelopParamsModel::LoadJson` reads the `diffusion_filter` object. A missing object or a
  missing key uses the default value. Current `0.10.0` documents load with the effect off.
- Section 9, U1 decides the write rule and the format identity. Phase DF1 must not start until
  the user decides U1.
- The Adjustment Transfer package gets a new Develop item. Section 9, U4 decides the schema
  identity.

### 3.8 User interface

Section 9, U2 decides the panel placement and the labels. The recommended design follows.

```text
Detail
  Diffusion                                   [toggle]
    Strength          ----------o----------   20
    Glow Size         -----------o---------   12
    Black Mist        ----------o----------   50
    Highlight Glow    ----------o----------    6
    Warmth            ----------o----------    0
  Clarity ...
  Sharpen ...
  Halation ...
  Film Grain ...
```

- The section uses the current `EditorAdjustmentValueModel` slider rows and `appTheme` tokens.
- The toggle writes `diffusion_filter.enabled`. The sliders stay enabled when the toggle is off,
  like the current Halation section. Confirm this rule against `EditorDetailPanel.qml` in DF6.
- One slider drag produces one settled history edit when the pointer is released.
- Reset behavior follows the current Detail page sliders. DF6 does not add a new reset control.
- The UI does not show the names "Tiffen" or "Pro-Mist". These are trademarks.
- The UI adds no pill, badge, or status dot.

### 3.9 Performance targets

These are planning values. They are not evidence.

| Item | Target |
| --- | --- |
| Scatter pass GPU time, 24 MP develop output, CUDA release build | 3 ms or less |
| Mix cost added to `CameraToAp1` | 0.5 ms or less at a 2560 px long edge |
| Scatter memory, 24 MP | Less than 16 MB for the scatter image and all levels |
| Strength or warmth edit | No scatter pass execution (pass statistics show a skip) |

Stop and report if the measured scatter pass time is more than 10 ms. Do not lower the
resolution to meet a target.

---

## 4. Scope

### 4.1 Included modules

| Module | Responsibility |
| --- | --- |
| `edit/graph` Develop model | Store, validate, update, and serialize the diffusion parameters. |
| `app` editor write path | Parse and apply `diffusion_filter` writes as one settled history edit. |
| `edit/runtime` plan and keys | Compute pyramid layout and weights. Schedule the pass. Build content keys. Invalidate results. |
| CUDA runtime | Scatter kernels and the fused mix in `CameraToAp1`. |
| Metal runtime | The same kernels and mix in Metal shaders. |
| OpenCL runtime | The same kernels and mix in OpenCL 1.2 programs. |
| QML editor | The Diffusion section and its translations. |
| `app` Adjustment Transfer | Copy and paste the diffusion parameters as one Develop item. |
| Tests | CPU reference, model, compiler, key, invalidation, backend pixel, QML model, and transfer tests. |

### 4.2 Explicit exclusions

- This plan does not change the four DRT/Post adjustments. `DrtNodeModel` keeps Clarity,
  Sharpen, Halation, and Film Grain.
- This plan does not change Halation. Halation stays a display-referred DRT/Post neighborhood
  adjustment. A scene-referred Halation that uses the scatter pyramid needs its own user
  decision and its own plan.
- This plan does not add a half-float texture format.
- This plan does not change the decode resolution policy of library thumbnails. Section 9, U5
  records the effect on thumbnails.
- This plan does not add light sources outside the image frame. The source image cannot show
  them.
- This plan does not fix the stale DRT comments in section 5.2. They are not in the changed code.

### 4.3 Related plans

| Plan | Relation |
| --- | --- |
| [GPU DAG Pipeline Rebuild](gpu_dag_pipeline_rebuild_phase_plan.md) | Extends. Adds one Develop pass and one `CameraToAp1` input. |
| [GPU DAG OpenCL Migration](gpu_dag_opencl_migration_phase_plan.md) | Depends on. Uses its program registration and OpenCL 1.2 rules. |
| [GPU DAG Metal Migration](gpu_dag_metal_migration_phase_plan.md) | Depends on. Uses its Metal pass structure. |
| [NM10 Adjustment Transfer](node_mask_editor/phase_nm10_adjustment_transfer_plan.md) | Extends. Adds one Develop transfer item. |

---

## 5. Current source audit

### 5.1 Verified source facts

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Pass kinds | `include/edit/runtime/pass_kind.hpp` | `GpuPassKind` values 0-10 and 12-14. Value 11 is unused. | Append `DiffusionScatter = 15`. Do not reuse 11. |
| Develop compile | `edit/runtime/graph_compiler.cpp`, `CompileDevelopPasses` | Pushes Upload, Linearize, CfaClamp, Demosaic, HighlightRecover, InverseCamMulPack, Lens, GeometryResample, CameraToAp1. | Push `DiffusionScatter` after `Lens`. Add its output as a `CameraToAp1` input. |
| Frame binding | `GraphCompiler::BindFrameGeometry`, `graph_compiler.cpp:509` | Sets per-frame geometry and `encode_geometry_resample`. | No change. |
| Develop output | `include/edit/runtime/plan_executor.hpp` | `develop:sensor_linear` has the decoded extent. `BindOrMiss` reuses it. QualityBase persists only this value. | Add the scatter block after the Develop block and before the Geometry block. |
| Develop output color | `include/edit/input/prepared_raw_input.hpp:148` | "Output of Develop is camera scene-linear RGB." | The scatter pass reads it without decode. |
| Decode resolution | `edit/input/raw_input_loader.cpp:536`, `app/thumbnail_service.cpp:41-49` | The editor uses `DecodeRes::FULL`. Thumbnails use reduced decode. | No change. See U5. |
| Camera color | `edit/runtime/cuda/cuda_camera_color_pass.cu`, `metal/shader/camera_color.metal`, `opencl/shader/geometry_camera.cl` | Matrix `M`, then DNG profile, then reference gamut compression, then ACEScc encode. The CUDA kernel uses a 1D pixel index. | Add the mix before `M`. Derive x and y from the index. |
| Geometry resample | `edit/runtime/cuda/geometry_resample.cu` and backend peers | Resamples the develop output to the render extent. Aliases it when the resample is identity. | No change. |
| DRT pass | `edit/runtime/cuda/cuda_drt_pass.cu`, `DrtKernel` | Decodes ACEScc, compresses gamut, applies ACES 2.0 or OpenDRT, encodes display. | No change. |
| DRT/Post order | `include/edit/runtime/drt_post_executor.hpp`, `drt_post_schedule.hpp` | Display transform runs first. Neighborhood Post adjustments run after it. | No change. |
| DRT/Post count | `edit/graph/adjustment_ownership.cpp`, `RequireCompleteDrtPostTypes` | Requires exactly four types. `DrtNodeModel::ToJson` and `FromJson` call it. | No change. |
| Color Grade ownership | `include/edit/graph/color_grade_node_model.hpp` | Rejects Clarity, Sharpen, Halation, and Film Grain. | No change. |
| Develop parameters | `include/edit/graph/develop_node_model.hpp`, `DevelopPayload`, `DevelopDirty` | Four dirty groups: Demosaic, Highlights, WhiteBalance, Lens. Focused update structs per group. | Add fields, one update struct, and two dirty groups. |
| Develop JSON | `edit/graph/develop_node_model.cpp`, `LoadJson` | Reads each key with the current value as the default. | Read and validate the new object. |
| Content keys | `edit/runtime/result_content_key.cpp`, `BuildFrameResultContentKeys` | Sensor key, geometry key from the sensor key, camera key from the geometry key. | Add a scatter key. Mix it and the mix fields into the camera key. |
| Implementation versions | `include/edit/runtime/content_key.hpp` | `kCameraColorImplementationVersion = 5`. | Increment to 6. Add `kDiffusionScatterImplementationVersion = 1`. |
| Invalidation | `edit/runtime/runtime_invalidation.cpp`, `CollectDevelopChanges` | Sensor field revision invalidates `sensor_linear`. White balance revision invalidates `develop_output`. | Add a scatter revision and a mix revision. |
| Result persistence | `include/edit/runtime/result_persistence.hpp` | QualityBase persists only `develop:sensor_linear`. | No change. QualityBase computes the scatter image in its own submission. |
| Pyramid code | `edit/runtime/cuda/cuda_local_tone_pass.cu`, `include/edit/runtime/local_tone_plan.hpp`, `metal/shader/local_tone.metal`, `opencl/shader/local_tone.cl` | Local Laplacian pyramid with `LocalTonePyramidLayout`. | Use as the structure pattern. Do not share its layout type. |
| Texture formats | `include/edit/runtime/texture_format.hpp` | `R8`, `Rgba8`, `R32f`, `Rgba32f`, `R16u`. | No change. Use `Rgba32f`. |
| Render scale helpers | `edit/runtime/adjustment_runtime.cpp` | `RenderAxisScale`, `NeighborhoodRenderScale`, `CopyRenderMapping` are in an unnamed namespace. | Not used. The scatter pass uses develop pixels. |
| Behavior enum | `include/edit/runtime/adjustment_runtime.hpp`, `AdjustmentBehavior` | Shader constants copy its values, for example `kDrtNeighborBehaviorHalation = 15u` in `metal/shader/drt_neighbor.metal`. | No change. The diffusion filter is not an adjustment behavior. |
| OpenCL programs | `include/edit/runtime/opencl/opencl_dag_programs.hpp`, `edit/runtime/opencl/opencl_gpu_dag_programs.cpp`, `opencl/CMakeLists.txt` | Manifest `gpu_dag` with five programs. | Add one program and its kernel names. |
| Metal shaders | `metal/CMakeLists.txt` | Builds one metallib for each DAG shader. | Add one metallib. |
| Lens write path | `app/editor_parameter_write_parse.cpp`, `app/editor_parameter_write.cpp`, `include/app/editor_parameter_write.hpp` | Field key `lens_calib` parses into `DevelopLensCalibrationUpdate`. | Add field key `diffusion_filter`. |
| Duplicate parser | `app/editor_pipeline_command_service.cpp:422` | Holds a second `ParseLensCalibrationUpdate`. | Do not add a second diffusion parser. See DF1 step 5. |
| Transfer items | `include/app/adjustment_transfer_types.hpp` | `AdjustmentTransferItemKind` has `RawDecode`, `WhiteBalance`, `LensCalibration`, `Geometry`. | Append `DiffusionFilter`. |
| Transfer schema | `include/edit/history/pipeline_history_format.hpp` | `kAdjustmentTransferSchema = "alcedo.adjustment_transfer.v7"`. | See U4. |
| Project format | `include/edit/history/pipeline_history_format.hpp` | Project `0.10.0`. Loaders reject other versions without conversion. | See U1. |
| Detail panel | `ui/alcedo_main/qml/EditorDetailPanel.qml` | Clarity, Sharpen, Halation, Film Grain. | See U2. |
| Translation count | `tests/ui/simplified_chinese_catalog_test.cpp` | `EditorDetailPanel` has 6 messages. Total 214. | Update both counts in DF6. |

### 5.2 Observed limitations

- No pass in the current pipeline has scene-linear data at the full decoded resolution after
  Color Grade. The only full-resolution scene-linear image is the Develop output.
- Two comments describe an old order. `include/edit/graph/drt_node_model.hpp:186` says the
  DRT/Post adjustments "run in ACEScc before the display transform". The `CompiledDrtNode`
  comment in `include/edit/runtime/execution_plan.hpp` says "neighborhood steps then display
  transform". `GraphCompiler` and `DrtPostExecutor` run the display transform first.
- The current neighborhood executor uses separable direct convolution with at most 64 taps.
  It cannot make a PSF with a radius of 12% of the short side.

### 5.3 Current tests and targets

| Target | Relevant sources |
| --- | --- |
| `GpuDagModelGraphTest` | `tests/edit/graph/default_pipeline_test.cpp`, `json_roundtrip_test.cpp` |
| `GpuDagRawInputTest` | `tests/edit/runtime/graph_compiler_test.cpp`, `result_content_key_test.cpp`, `runtime_invalidation_test.cpp`, `local_tone_plan_test.cpp` |
| `PipelineEditBatchTest` | `tests/edit/history/pipeline_edit_batch_test.cpp`, `expected_serialized/` |
| `GpuDagCudaDevelopTest` | `tests/edit/runtime/cuda_develop_test.cpp` |
| `GpuDagMetalDevelopTest` | `tests/edit/runtime/metal_develop_test.cpp` |
| `GpuDagOpenClDevelopTest` | `tests/edit/runtime/opencl_develop_test.cpp` |
| `EditorPipelineCommandServiceTest`, `EditorPanelProjectionTest`, `EditorAdjustmentContextTest`, `PipelineHistoryApplierTest` | `tests/app/` |
| `DocumentTransferTest`, `AdjustmentTransferServiceTest`, `AdjustmentTransferCatalogTest` | `tests/app/` |
| `EditorAdjustmentModelTest`, `SimplifiedChineseCatalogTest` | `tests/ui/` |

Confirm each target name in `tests/edit/CMakeLists.txt`, `tests/app/CMakeLists.txt`, and
`tests/ui/CMakeLists.txt` before a phase starts.

---

## 6. Target architecture

### 6.1 Owners

#### `DevelopParamsModel` (current, extended)

- Input: `DevelopDiffusionFilterUpdate` from the editor write path. JSON from document load.
- Output: `DevelopPayload` diffusion fields through `Params()` and field readers.
- Changes: the diffusion fields only, through `ApplyDiffusionFilterUpdate`.
- Validation: rejects non-finite and out-of-range values before mutation.
- Dirty groups: `DiffusionScatter` for `glow_radius`, `black_mist`, `highlight_glow`.
  `DiffusionMix` for `enabled`, `strength`, `black_mist`, `warmth`.
- Lifetime: the Develop node of one `PipelineDocument`.
- Error surface: `std::invalid_argument` for a write. `std::runtime_error` for a load.

#### Diffusion scatter plan (proposed, `include/edit/runtime/diffusion_scatter_plan.hpp`)

- Input: develop output extent, diffusion fields, camera-to-AP1 matrix.
- Output: `DiffusionScatterLayout`, normalized weights, and GPU parameter structs.
- Changes: nothing. Pure functions.
- Error surface: `std::invalid_argument` for an empty extent.

#### `GraphCompiler` (current, extended)

- Adds the `DiffusionScatter` pass and the `diffusion_scatter_output` value to every plan that
  has a Develop node. The pass is always in the static plan. The executor skips it when the
  effect is not active. A parameter change therefore never forces a recompile.

#### `PlanExecutor<Backend>` (current, extended)

- Runs the scatter block after the Develop block and before the Geometry block.
- Calls `BindOrMiss` for `diffusion_scatter_output` at the base level extent.
- On a miss, calls `PassEncoder<Backend, GpuPassKind::DiffusionScatter>::Encode`.
- Records pass statistics `diffusion_scatter_execute` and `diffusion_scatter_skip`.

#### Backend scatter passes (proposed, one per backend)

- Input: `develop:sensor_linear`, `DiffusionScatterGpuParams`.
- Output: the scatter image in `diffusion_scatter_output`.
- Scratch: pyramid levels from the workspace texture pool. The pass releases them before it
  returns.
- Error surface: `std::runtime_error` with the backend prefix, for example
  `ExecuteCudaDiffusionScatter: missing develop.sensor_linear`.

#### Backend `CameraToAp1` passes (current, extended)

- Input: `geometry.scene_source`, optional scatter image, `DiffusionMixGpuParams`.
- Output: `develop.image` (ACEScc AP1), as today.
- When the effect is not active, the pass runs the current kernel without change.

### 6.2 Data flow

```text
PreparedRawInput
  -> Develop passes (Upload .. Lens)            develop:sensor_linear   decoded extent, cached
  -> DiffusionScatter (when active)             develop:runtime.diffusion_scatter
                                                base extent, cached for Interactive/Detail
  -> GeometryResample                           geometry:scene_source   render extent
  -> CameraToAp1 + mix (reads both values)      develop:image           ACEScc AP1
  -> Color Grade nodes -> DRT -> DRT/Post       display
```

### 6.3 Content keys

```text
sensor_linear      = unchanged
diffusion_scatter  = Hash(sensor_linear, glow_radius, black_mist, highlight_glow,
                          kDiffusionScatterImplementationVersion)
geometry           = unchanged
develop_image      = Hash(geometry, camera color params, kCameraColorImplementationVersion,
                          active flag, and when active:
                          diffusion_scatter, strength, black_mist, warmth)
```

### 6.4 Invalidation

```text
DevelopDirty::DiffusionScatter revision changed
  -> origin develop:runtime.diffusion_scatter
  -> dependents through the CameraToAp1 input: develop:image, Grade, LLF, DRT, display

DevelopDirty::DiffusionMix revision changed
  -> origin develop:image (same origin as a white balance change)

develop:sensor_linear is never an origin of a diffusion change.
```

### 6.5 Primary success call chain

```text
QML slider release (Glow Size)
  -> editor write `diffusion_filter` {glow_radius}
  -> ParseDiffusionFilterUpdate -> validate range
  -> DevelopParamsModel::ApplyDiffusionFilterUpdate (DiffusionScatter dirty)
  -> one settled history edit
  -> render request
  -> RuntimeInvalidationState::CollectDevelopChanges -> origin diffusion_scatter
  -> PlanExecutor: Develop block BindOrMiss hit (no decode)
  -> scatter block miss -> reduce+boost -> downsample x (K-1) -> upsample+accumulate x (K-1)
  -> GeometryResample hit or execute
  -> CameraToAp1 + mix -> Grade -> DRT -> DRT/Post
  -> EndRender -> publish frame
```

### 6.6 Primary failure and restore call chain

```text
scatter kernel launch or texture acquire fails
  -> backend pass throws std::runtime_error with backend prefix
  -> PlanExecutor catch -> CancelRender
  -> no frame publish; unpublished writes discarded
  -> develop:sensor_linear stays published and valid
  -> the viewport keeps the last published frame
  -> the render coordinator reports the error message
```

---

## 7. File and API map

### 7.1 Current files that change

| File | Phase | Change |
| --- | --- | --- |
| `include/edit/graph/develop_node_model.hpp` | DF1 | Payload fields, update struct, dirty groups, readers, apply operation. |
| `edit/graph/develop_node_model.cpp` | DF1 | Apply, validate, `ToJson`, `LoadJson`, `IsDefault`, equality. |
| `include/app/editor_parameter_write.hpp` | DF1 | Add `DevelopDiffusionFilterUpdate` to the write variant. |
| `app/editor_parameter_write_parse.cpp` | DF1 | `ParseDiffusionFilterUpdate` and field key dispatch. |
| `app/editor_parameter_write.cpp` | DF1 | Apply the update to the Develop owner. |
| `app/editor_panel_projection.cpp` | DF1 | Project the `diffusion_filter` values. |
| `app/editor_adjustment_context.cpp` | DF1 | Map field key `diffusion_filter` to the Develop owner. |
| `app/pipeline_history_applier.cpp` | DF1 | Apply history replay of the new field. Confirm the need in DF1 step 1. |
| `include/edit/runtime/pass_kind.hpp` | DF2 | `DiffusionScatter = 15` and its name. |
| `include/edit/runtime/execution_plan.hpp` | DF2 | `diffusion_scatter_output`. |
| `edit/runtime/graph_compiler.cpp` | DF2 | Push the pass and the new `CameraToAp1` input. |
| `include/edit/runtime/content_key.hpp` | DF2 | Version constants. |
| `edit/runtime/result_content_key.cpp`, `include/edit/runtime/result_content_key.hpp` | DF2 | Scatter key and camera key inputs. |
| `edit/runtime/runtime_invalidation.cpp`, `include/edit/runtime/runtime_invalidation.hpp` | DF2 | Two revisions and origins. |
| `include/edit/runtime/plan_executor.hpp` | DF2 | Scatter block and statistics. |
| `include/edit/runtime/gpu_node_pass_stats.hpp` | DF2 | Two counters next to `sensor_develop_skip`. |
| `include/edit/runtime/cuda/cuda_pass_encoder.hpp`, `edit/runtime/cuda/cuda_camera_color_pass.cu`, `edit/CMakeLists.txt` | DF3 | CUDA encoder, mix, build. |
| `include/edit/runtime/metal/metal_pass_encoder.hpp`, `edit/runtime/metal/metal_develop_pass.mm`, `edit/runtime/metal/shader/camera_color.metal`, `metal/CMakeLists.txt` | DF4 | Metal encoder, mix, build. |
| `include/edit/runtime/opencl/opencl_pass_encoder.hpp`, `edit/runtime/opencl/opencl_develop_pass.cpp`, `edit/runtime/opencl/shader/geometry_camera.cl`, `include/edit/runtime/opencl/opencl_dag_programs.hpp`, `edit/runtime/opencl/opencl_gpu_dag_programs.cpp`, `opencl/CMakeLists.txt` | DF5 | OpenCL encoder, mix, program, build. |
| `ui/alcedo_main/qml/EditorDetailPanel.qml` (or the U2 panel) | DF6 | Diffusion section. |
| `ui/alcedo_main/i18n/alcedo_main_en.ts`, `alcedo_main_zh_CN.ts` | DF6, DF7 | Manual message edits. |
| `include/app/adjustment_transfer_types.hpp`, `app/adjustment_transfer_package_builder.cpp`, `app/document_transfer_planner.cpp`, `app/document_transfer.cpp` | DF7 | Transfer item. |
| `include/edit/history/pipeline_history_format.hpp` | DF1 (U1 option B), DF7 (U4) | Format identities. |

### 7.2 Proposed files

| Proposed file | Phase | Responsibility |
| --- | --- | --- |
| `include/edit/runtime/diffusion_scatter_plan.hpp` | DF2 | Constants, layout, weights, activity check, GPU parameter structs. |
| `edit/runtime/diffusion_scatter_plan.cpp` | DF2 | Implementations of the plan functions. |
| `tests/edit/runtime/diffusion_scatter_reference.hpp` | DF2 | CPU double-precision reference of the exact GPU algorithm. |
| `tests/edit/runtime/diffusion_scatter_plan_test.cpp` | DF2 | Plan and reference tests. |
| `include/edit/runtime/cuda/cuda_diffusion_scatter_pass.hpp`, `edit/runtime/cuda/cuda_diffusion_scatter_pass.cu` | DF3 | CUDA kernels and pass. |
| `include/edit/runtime/metal/metal_diffusion_scatter_pass.hpp`, `edit/runtime/metal/metal_diffusion_scatter_pass.mm`, `edit/runtime/metal/shader/diffusion_scatter.metal` | DF4 | Metal kernels and pass. |
| `include/edit/runtime/opencl/opencl_diffusion_scatter_pass.hpp`, `edit/runtime/opencl/opencl_diffusion_scatter_pass.cpp`, `edit/runtime/opencl/shader/diffusion_scatter.cl` | DF5 | OpenCL kernels and pass. |

### 7.3 Proposed APIs

```cpp
// include/edit/graph/develop_node_model.hpp
struct DevelopDiffusionFilterUpdate {
  std::optional<bool>  enabled;
  std::optional<float> strength;
  std::optional<float> glow_radius;
  std::optional<float> black_mist;
  std::optional<float> highlight_glow;
  std::optional<float> warmth;
};
void DevelopParamsModel::ApplyDiffusionFilterUpdate(DevelopDiffusionFilterUpdate update);
```

- Validates every present value before mutation. Throws `std::invalid_argument` for a
  non-finite or out-of-range value. Changes no field when it throws.
- Marks only the dirty groups of changed fields. An equal value marks nothing.

```cpp
// include/edit/runtime/diffusion_scatter_plan.hpp (proposed)
struct DiffusionScatterLayout {
  std::uint32_t base_level  = 0;   // L0
  std::uint32_t level_count = 0;   // K
  std::array<ImageExtent, kDiffusionMaxLevels> extents{};  // index 0 = base level
  std::array<float, kDiffusionMaxLevels>       weights{};  // sum 1
};
auto MakeDiffusionScatterLayout(ImageExtent develop_extent, float glow_radius,
                                float black_mist) -> DiffusionScatterLayout;
auto IsDiffusionFilterActive(const DevelopPayload& payload) -> bool;
auto MakeDiffusionWarmthMatrix(std::span<const float, 9> camera_to_ap1, float warmth)
    -> std::array<float, 9>;
struct alignas(16) DiffusionScatterGpuParams { /* clip levels, boost gain, knee, layout */ };
struct alignas(16) DiffusionMixGpuParams { /* strength, absorption T, warmth matrix,
                                               render_to_decoded, decoded and base extents */ };
```

- `MakeDiffusionScatterLayout` throws `std::invalid_argument` for an empty extent.
- `kDiffusionMaxLevels` is 16. The layout rule in section 3.2 keeps `K` at or below it for
  every extent up to 2^20 pixels on an axis. DF2 tests this limit.
- The GPU structs are POD. Each backend copies them as bytes. DF2 adds `static_assert` checks for
  size and alignment.

---

## 8. Implementation entry requirements

Before each phase, the implementer must:

1. Read the current repository `AGENTS.md` again. The rules can change after plan creation.
2. Read `.agents/skills/alcedo-msvc-cmake/SKILL.md` before a Windows configure or build.
3. Read `.agents/skills/opencl-program-registry/SKILL.md` before DF5.
4. Read `.agents/skills/alcedo-qml-ui/SKILL.md` and `ui/alcedo_main/DESIGN.md` before DF6.
5. Check the section 5 audit against the current branch. Record each difference in the phase
   completion record.
6. Stop and update this plan when an owner or API in section 5 no longer exists.
7. Check that the section 9 decisions that the phase needs have a user answer.

These current repository rules apply directly:

- Use C++20, the repository clang-format style, and trailing `_` for private members.
- Include the defining header. Do not add a forward declaration to skip an include.
- Include only the needed OpenCV module headers.
- Update the Develop parameters through `DevelopParamsModel` operations only. Do not copy the
  payload, edit the copy, and write it back.
- Do not add a generation, token, or stale-result guard. The scatter image uses the existing
  content key and invalidation mechanisms.
- Do not add a fallback. A backend failure is a real error.
- Use LF line endings. Some edited files can have CRLF. Check line endings with a byte read
  before the edit.
- Run MSVC builds through the PowerShell tool. Use only the `win_debug` and `win_release`
  presets.
- Do not run the full `ctest` suite. Run the named targets.
- Do not run `lupdate`. Edit `.ts` files by hand.
- Use behavior names for tests. Do not use the prohibited terms in `AGENTS.md`.
- Put temporary evidence under `build/tmp/diffusion_filter/`. Remove it when the phase ends.

---

## 9. Unresolved decisions

| Id | Decision | Options | Recommendation | Phase that needs it |
| --- | --- | --- | --- | --- |
| U1 | Develop JSON write rule and project format | A: `ToJson` omits `diffusion_filter` when all six values equal the defaults. Current `0.10.0` documents stay byte-identical. No format change. B: `ToJson` always writes the object. Cut the project format to `0.11.0` and reject `0.10.0` without conversion. | A. It keeps current projects and the expected serialized fixtures valid. | DF1 |
| U2 | Panel placement and labels | A: Diffusion section at the top of the Detail page (`EditorDetailPanel.qml`). B: a section in the RAW page next to lens correction (`EditorRawDecodePanel.qml`). C: a new page. | A, with the labels in section 3.3. | DF6 |
| U3 | Strength calibration | Map filter grades 1/8, 1/4, 1/2, and 1 to `s`. The simulation suggests about 0.08 to 0.4 (tested 0.10, 0.20, 0.35). Method: photograph one scene with and without a physical filter. Fit `s`, `p`, and `glow_radius` by least squares on linear RAW data. | Keep the stored `s`. Add grade presets only after a calibration. | DF6 (presets only) |
| U4 | Transfer schema identity | A: increment to `alcedo.adjustment_transfer.v8` and reject v7 without conversion. B: keep v7 and accept a missing `diffusion_filter` key. | A. The Develop transfer items used this rule when they changed the schema to v7. | DF7 |
| U5 | Library thumbnails | Thumbnails decode at a reduced resolution. The highlight boost then differs. The simulation measured a 21.8% relative error of the effect at 1/4 scale. A: accept and document the difference. B: decode images with an active filter at full resolution for thumbnails. | Ask the user. B changes thumbnail cost. | DF7 |
| U6 | Boost measure | A: maximum clip-relative channel (section 3.1). B: Rec.709 luma, as in the simulation. | A. A sensor clips each channel separately. Confirm with the DF7 visual check. | DF2 |

---

## 10. Phase summary

The estimate counts production code, tests, build files, QML, translations, and plan updates.
It excludes generated files and temporary evidence.

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| DF1 | Develop diffusion parameters, JSON, and editor write path | `edit/graph`, `app` write path | U1 answered | 900-1,400 lines | Not started |
| DF2 | Scatter plan, CPU reference, compile, keys, invalidation, executor block | `edit/runtime` host code, tests | DF1, U6 answered | 1,200-1,700 lines | Not started |
| DF3 | CUDA scatter kernels, fused mix, and CUDA evidence | CUDA runtime, tests | DF2 | 1,100-1,700 lines | Not started |
| DF4 | Metal scatter kernels, fused mix, and Metal evidence | Metal runtime, shaders, tests | DF2 | 900-1,400 lines | Not started |
| DF5 | OpenCL scatter kernels, fused mix, and OpenCL evidence | OpenCL runtime, programs, tests | DF2 | 900-1,400 lines | Not started |
| DF6 | Diffusion section in QML and translations | QML, panel projection, `.ts`, UI tests | DF3, DF4, DF5, U2 answered | 700-1,200 lines | Not started |
| DF7 | Adjustment Transfer item and product qualification | `app` transfer, tests, evidence | DF6, U4 and U5 answered | 1,000-1,600 lines | Not started |

No phase has an expected diff above 2000 lines.

Split reasons:

- DF2 separates the backend-neutral host code from the kernels. A combined DF2 and DF3 has a
  material risk of more than 2000 lines.
- DF3, DF4, and DF5 are separate because each backend has its own kernels, build registration,
  and test target. Each backend needs its own review.
- DF6 comes after all three backends. No product surface can turn on the effect before every
  backend implements it.
- DF7 separates the transfer schema change from the QML change.

Between DF2 and DF5, a document with an active filter can reach a backend that has no scatter
pass yet. That backend throws `std::runtime_error("<Backend> DiffusionScatter is not
implemented")`. This is a real error, not a fallback. No UI, transfer, or paste path can create
such a document before DF6 and DF7.

If an upper estimate passes 2000 lines during implementation, split that phase first.
Do not continue with an oversized phase and explain the split after review.

---

## 11. DF1 — Develop diffusion parameters, JSON, and editor write path

### 11.1 Objective and deliverables

Add the six diffusion fields to the Develop parameter owner. Add validation, dirty groups,
JSON read and write, and the editor write path. A reviewer can write `diffusion_filter` values
through the editor service, see one history edit, and reload the document with the same values.

This phase has no render change and no QML change.

### 11.2 Inputs and prerequisites

- The user answer to U1.
- Current `DevelopParamsModel` update pattern (`ApplyLensCalibrationUpdate`).
- Current editor write path for field key `lens_calib`.
- Current expected serialized fixtures in `alcedo_studio/tests/edit/history/expected_serialized/`.

### 11.3 Modules, files, and APIs

Modify:

- `include/edit/graph/develop_node_model.hpp` and `edit/graph/develop_node_model.cpp`:
  `DevelopPayload` fields, `DevelopDiffusionFilterUpdate`, `DevelopDirty::DiffusionScatter =
  1U << 4`, `DevelopDirty::DiffusionMix = 1U << 5`, `DevelopDirty::All`, field readers,
  `ApplyDiffusionFilterUpdate`, equality, `IsDefault`, `ToJson`, `LoadJson`.
- `include/app/editor_parameter_write.hpp`, `app/editor_parameter_write_parse.cpp`,
  `app/editor_parameter_write.cpp`: field key `diffusion_filter`.
- `app/editor_panel_projection.cpp`, `app/editor_adjustment_context.cpp`: projection and owner
  mapping.
- `app/pipeline_history_applier.cpp`: only when step 1 shows that replay needs a field case.
- `include/edit/history/pipeline_history_format.hpp`: only for U1 option B.

### 11.4 Data rules and invariants

- The six fields have the ranges and defaults of section 3.3.
- `ApplyDiffusionFilterUpdate` validates every present value before it changes any field.
- `glow_radius` and `highlight_glow` changes mark `DiffusionScatter`.
- `enabled`, `strength`, and `warmth` changes mark `DiffusionMix`.
- A `black_mist` change marks both groups.
- A diffusion change never marks `Demosaic`, `Highlights`, `WhiteBalance`, or `Lens`.
- `LoadJson` reads a missing key as its default. It rejects a present non-finite or
  out-of-range value with `std::runtime_error` that names the key.
- The JSON key names are exactly those in section 3.3.
- U1 option A: `ToJson` omits the object when all six values equal the defaults.
- The editor write path parses the field once, in `editor_parameter_write_parse.cpp`.

### 11.5 Implementation steps

1. Trace the `lens_calib` write from QML to the owner. Record each file that dispatches on the
   field key. Record whether `pipeline_history_applier.cpp` replays by field key.
2. Add the payload fields with default member initializers. Extend `operator==`.
3. Add `DevelopDiffusionFilterUpdate` and the two dirty groups. Add both groups to `All`.
4. Implement `ApplyDiffusionFilterUpdate` with the `Mutate` pattern of
   `ApplyLensCalibrationUpdate`. Validate first. Mark only the changed groups.
5. Add `ParseDiffusionFilterUpdate` to `editor_parameter_write_parse.cpp`. Reject unknown keys
   in the object with the existing error form. Do not add a copy to
   `editor_pipeline_command_service.cpp`. If that file dispatches the field, call the single
   parser.
6. Add the `DevelopDiffusionFilterUpdate` variant member and the apply case in
   `editor_parameter_write.cpp`.
7. Add the panel projection of the six values and the owner mapping of the field key.
8. Implement `ToJson` and `LoadJson` by the U1 answer.
9. For U1 option B, change the format identities and add the rejection test. Update the
   expected serialized fixtures by hand from an independent expected value.
10. Run the named tests. Record the results.

The phase must not change `SensorFieldMask`, content keys, or any render code.

### 11.6 Primary success call chain

```text
editor write {field: diffusion_filter, params: {strength: 0.3}}
  -> ParseDiffusionFilterUpdate -> DevelopDiffusionFilterUpdate{strength = 0.3}
  -> DevelopParamsModel::ApplyDiffusionFilterUpdate
       validate -> Mutate(DiffusionMix) -> revision increments
  -> one settled PipelineEditBatch
  -> document ToJson contains diffusion_filter.strength = 0.3
  -> reload -> LoadJson -> Params().strength == 0.3
```

### 11.7 Primary failure and restore call chain

```text
editor write {diffusion_filter: {strength: 0.9}}
  -> ParseDiffusionFilterUpdate accepts the number
  -> ApplyDiffusionFilterUpdate validation fails (range 0.0 to 0.5)
  -> std::invalid_argument "diffusion_filter.strength must be in [0, 0.5]"
  -> no field changes, no dirty mark, no history edit
  -> the editor reports the error
```

### 11.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `DiffusionFilterDefaultsMatchSpecification` | A new Develop model has the six defaults of section 3.3. |
| `DiffusionFilterRoundTripsThroughDevelopJson` | Non-default values survive `ToJson` and `LoadJson` exactly. |
| `DevelopJsonWithoutDiffusionFilterLoadsDisabledDefaults` | A current `0.10.0` Develop JSON loads with `enabled == false` and the defaults. |
| `DefaultDiffusionFilterIsOmittedFromDevelopJson` | U1 option A only: default values write no `diffusion_filter` key. |
| `DevelopJsonRejectsOutOfRangeDiffusionValue` | `black_mist = 1.5` in JSON throws and names `black_mist`. |
| `DevelopJsonRejectsNonFiniteDiffusionValue` | A non-finite `strength` throws and names `strength`. |
| `DiffusionScatterFieldMarksOnlyScatterDirtyGroup` | A `glow_radius` update changes the `DiffusionScatter` revision only. |
| `DiffusionMixFieldMarksOnlyMixDirtyGroup` | A `strength` update changes the `DiffusionMix` revision only. |
| `BlackMistMarksScatterAndMixDirtyGroups` | A `black_mist` update changes both revisions. |
| `DiffusionUpdateKeepsSensorFieldRevision` | No diffusion update changes the Demosaic, Highlights, or Lens revisions. |
| `InvalidDiffusionUpdateChangesNoField` | An update with one valid and one invalid value throws and changes neither value. |
| `DiffusionFilterWriteCreatesOneHistoryEdit` | One editor write adds one history entry. Undo restores the prior values. |
| `DiffusionFilterWriteRejectsUnknownKey` | An unknown key in the object fails with the parser error. |
| `PanelProjectionReportsDiffusionFilterValues` | The Develop panel projection has the six current values. |
| Existing expected serialized tests | `PipelineEditBatchTest` expected files still match under U1 option A. |

Use independent expected values. Do not produce expected JSON from the encoder under test.

### 11.9 Build and run commands

Run from the repository root in the PowerShell tool:

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest EditorPipelineCommandServiceTest EditorPanelProjectionTest EditorAdjustmentContextTest PipelineHistoryApplierTest PipelineEditBatchTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "^(GpuDagModelGraphTest|EditorPipelineCommandServiceTest|EditorPanelProjectionTest|EditorAdjustmentContextTest|PipelineHistoryApplierTest|PipelineEditBatchTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(GpuDagModelGraphTest|EditorPipelineCommandServiceTest|EditorPanelProjectionTest|EditorAdjustmentContextTest|PipelineHistoryApplierTest|PipelineEditBatchTest)\." > build/tmp/diffusion_filter/df1_ctest.log
```

Confirm the target names before execution. Confirm test discovery with `-N` before you claim
that a test ran.

### 11.10 Exit criteria

- [ ] The six fields exist with the specified ranges and defaults.
- [ ] Invalid writes and invalid loads fail and change nothing.
- [ ] Dirty groups match section 11.4.
- [ ] Current documents load with the effect off.
- [ ] The U1 answer is implemented and tested.
- [ ] One editor write creates one history edit.
- [ ] All named tests pass. The record gives discovered, passed, failed, and skipped counts.

### 11.11 Expected diff

900-1,400 lines. Split the app write path into its own phase if the estimate passes 2000 lines.

### 11.12 Completion record

```text
Phase / date / status:
Source revision and branch:
Actual changed modules:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual verification:
Evidence path:
Remaining defects:
```

---

## 12. DF2 — Scatter plan, CPU reference, compile, keys, invalidation, and executor block

### 12.1 Objective and deliverables

Add the backend-neutral design of section 3.2 as tested host code. Add a CPU reference that
later phases use as the expected result. Schedule the pass in every plan. Add the content keys,
the invalidation origins, and the executor block. A reviewer can inspect the compiled plan,
the keys, and the invalidation result without a GPU.

### 12.2 Inputs and prerequisites

- DF1 complete.
- The user answer to U6.
- The clip level owner. Step 1 finds it.

### 12.3 Modules, files, and APIs

Add (proposed): `include/edit/runtime/diffusion_scatter_plan.hpp`,
`edit/runtime/diffusion_scatter_plan.cpp`, `tests/edit/runtime/diffusion_scatter_reference.hpp`,
`tests/edit/runtime/diffusion_scatter_plan_test.cpp`.

Modify: `include/edit/runtime/pass_kind.hpp`, `include/edit/runtime/execution_plan.hpp`,
`edit/runtime/graph_compiler.cpp`, `include/edit/runtime/content_key.hpp`,
`edit/runtime/result_content_key.cpp`, `edit/runtime/runtime_invalidation.cpp` and its header,
`include/edit/runtime/plan_executor.hpp`, `include/edit/runtime/gpu_node_pass_stats.hpp`, the three backend pass
encoder headers, `edit/CMakeLists.txt`, and `alcedo_studio/tests/edit/CMakeLists.txt`.

### 12.4 Data rules and invariants

- `GpuPassKind::DiffusionScatter` has value 15. No current value changes.
- The compiled pass order is `Lens`, `DiffusionScatter`, `GeometryResample`, `CameraToAp1`.
- `diffusion_scatter_output` is `GraphValueId{develop_id, PortId{"runtime.diffusion_scatter"}}`.
- `CameraToAp1` lists `diffusion_scatter_output` as an input in every plan.
- The static plan does not depend on diffusion parameter values.
- The scatter key does not include `enabled`, `strength`, or `warmth`.
- The sensor key does not include any diffusion field.
- The camera key includes the active flag. It includes the scatter key and the mix fields only
  when the effect is active.
- `kCameraColorImplementationVersion` is 6. `kDiffusionScatterImplementationVersion` is 1.
- The CPU reference uses double precision. It implements the same taps, weights, rounding, and
  edge rules as section 3.2. It is test code only.

### 12.5 Implementation steps

1. Find the clip level of each channel in the Develop output. Read the linearize,
   highlight recovery, and `InverseCamMulPack` code on all backends. Record the result and its
   source lines in the completion record. Stop if the backends disagree.
2. Implement `MakeDiffusionScatterLayout`, the weights, `IsDiffusionFilterActive`, and
   `MakeDiffusionWarmthMatrix`. Implement the GPU parameter structs with `static_assert` checks.
3. Implement the CPU reference: reduce and boost, 13-tap downsample, 9-tap tent upsample and
   accumulate, B-spline mix, and the full effect for an image and a render mapping.
4. Append the pass kind and its name. Add the plan value.
5. Push the pass in `CompileDevelopPasses`. Add the new `CameraToAp1` input.
6. Add the version constants and the keys of section 6.3.
7. Add the two revisions and origins of section 6.4 to `CollectDevelopChanges`.
8. Add the scatter block to `PlanExecutor::Execute` between the Develop and Geometry blocks.
   Skip the block and count a skip when the effect is not active.
9. Declare `PassEncoder<Backend, GpuPassKind::DiffusionScatter>` for CUDA, Metal, and OpenCL.
   Each throws `std::runtime_error("<Backend> DiffusionScatter is not implemented")` until its
   backend phase.
10. Register the new test source in `GpuDagRawInputTest`.

The phase must not change a kernel, a shader, or a QML file.

### 12.6 Primary success call chain

```text
GraphCompiler::CompileStatic(document with Develop)
  -> CompileDevelopPasses: ... Lens, DiffusionScatter, GeometryResample, CameraToAp1
  -> BuildFrameResultContentKeys: sensor, scatter, geometry, camera keys
  -> RuntimeInvalidationState::CollectDevelopChanges(glow_radius edit)
       -> origin runtime.diffusion_scatter -> dependents develop:image .. display
  -> PlanExecutor (inactive filter): scatter block skipped, statistics count one skip
```

### 12.7 Primary failure and restore call chain

```text
active filter on a backend before its phase
  -> PassEncoder<Backend, DiffusionScatter>::Encode throws "not implemented"
  -> PlanExecutor catch -> CancelRender -> rethrow
  -> no frame publish; develop:sensor_linear stays published
```

### 12.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `DiffusionLayoutForSimulationImageUsesBaseLevelThreeAndSevenLevels` | 6008 x 4008 with `glow_radius = 0.12` gives `L0 = 3`, `K = 7`, base 751 x 501. |
| `DiffusionLayoutClampsLevelCountForSmallImages` | A 64 x 48 extent gives a coarsest level of at least 1 x 1 and weights that sum to 1. |
| `DiffusionLayoutStaysWithinMaximumLevels` | Extents up to 2^20 on an axis give `K <= kDiffusionMaxLevels`. |
| `DiffusionWeightsSumToOneAndFollowPowerLaw` | With `black_mist = 0`, `w_(k+1) / w_k = 2^(2 - 2.6)` within 1e-6. |
| `BlackMistAttenuatesWidestLevel` | With `black_mist = 1`, the last weight before normalization is 0. |
| `WarmthMatrixIsIdentityAtZeroWarmth` | The matrix equals identity within 1e-6. |
| `ReferencePsfConservesImpulseEnergy` | With boost 0 and `black_mist = 0`, the sum of `B` equals the impulse energy within 1e-6. |
| `ReferencePsfFollowsPowerLawBetweenCoreAndCutoff` | The fitted log-log slope of the radial profile is -2.6 within 0.2, between 4 px and `0.1 * short_side`. |
| `ReferenceOffGridImpulsePeakRatioMeetsTarget` | The off-grid peak ratio is 0.95 or more. This is a planning target. Record the measured value. |
| `ReferenceBoostAppliesOnlyNearClip` | A pixel at `r = 0.5` has no boost. A pixel at `r = 1.0` gets `1 + highlight_glow`. |
| `ReferencePreviewMatchesDownscaledExport` | A synthetic scene with clipped lamps: preview mix against a downscaled full mix. Relative error of the effect is 5% or less. |
| `GraphCompilerSchedulesDiffusionScatterBetweenLensAndGeometry` | Pass order matches section 12.4. |
| `CameraToAp1ReadsDiffusionScatterOutput` | The `CameraToAp1` pass inputs include `runtime.diffusion_scatter`. |
| `DiffusionParameterEditDoesNotRecompileStaticPlan` | `NeedsRecompile` is false after a diffusion edit. |
| `StrengthEditKeepsScatterKeyAndChangesCameraKey` | Section 6.3. |
| `GlowRadiusEditChangesScatterKeyAndKeepsSensorKey` | Section 6.3. |
| `InactiveFilterCameraKeyIgnoresDiffusionValues` | With `enabled = false`, a `glow_radius` change keeps the camera key. |
| `DiffusionScatterEditInvalidatesScatterAndDownstreamOnly` | The origin set contains `runtime.diffusion_scatter` and not `sensor_linear`. |
| `DiffusionMixEditInvalidatesDevelopImage` | The origin set contains `develop:image` only. |
| `DiffusionMixEditInvalidatesLlfCanonicalSourceLikeWhiteBalanceEdit` | The LLF canonical source validity after a strength edit equals its validity after a white balance edit. |
| `InactiveFilterSkipsScatterBlock` | A host executor test with a test backend counts one skip and no encode. |

### 12.9 Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest GpuDagModelGraphTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "^GpuDagRawInputTest\."
ctest --test-dir build/debug --output-on-failure -R "^(GpuDagRawInputTest|GpuDagModelGraphTest)\." > build/tmp/diffusion_filter/df2_ctest.log
```

Build `alcedo_main` once to prove that the three backends compile with the new encoders.

### 12.10 Exit criteria

- [ ] The layout, weights, and reference tests pass.
- [ ] The compiler, key, and invalidation tests pass.
- [ ] The clip level owner and value are recorded with source lines.
- [ ] `alcedo_main` builds.
- [ ] No kernel or QML file changed.

### 12.11 Expected diff

1,200-1,700 lines. Move the CPU reference and its tests into a separate phase if the estimate
passes 2000 lines.

### 12.12 Completion record

Use the template in section 11.12.

---

## 13. DF3 — CUDA scatter kernels, fused mix, and CUDA evidence

### 13.1 Objective and deliverables

Implement the four kernels of section 3.2 in CUDA. Add the mix to the CUDA `CameraToAp1` kernel.
Prove the result against the CPU reference and on a real RAW file. Record CUDA timing.

### 13.2 Inputs and prerequisites

- DF2 complete.
- A Windows machine with CUDA 12.8 and compute capability 6.0 or higher.

### 13.3 Modules, files, and APIs

Add (proposed): `include/edit/runtime/cuda/cuda_diffusion_scatter_pass.hpp`,
`edit/runtime/cuda/cuda_diffusion_scatter_pass.cu` with `ExecuteCudaDiffusionScatter`.

Modify: `include/edit/runtime/cuda/cuda_pass_encoder.hpp`,
`edit/runtime/cuda/cuda_camera_color_pass.cu`, `edit/CMakeLists.txt`,
`alcedo_studio/tests/edit/runtime/cuda_develop_test.cpp` or a new test source registered in
`GpuDagCudaDevelopTest`.

### 13.4 Data rules and invariants

- The scatter pass reads `develop:sensor_linear` only. It writes `runtime.diffusion_scatter` at
  the base extent.
- The pass acquires level textures from `Workspace().Textures()`. It releases every lease before
  it returns.
- No texture larger than the base extent exists during the pass.
- With the effect inactive, `CameraToAp1` output is bit-identical to the DF2 output.
- The mix uses the render-to-decoded mapping of `plan.geometry`. It does not apply outside the
  decoded extent.
- All launches use the command context stream of the render device.

### 13.5 Implementation steps

1. Implement the reduce-and-boost kernel with the tent footprint and per-tap boost.
2. Implement the 13-tap downsample kernel.
3. Implement the 9-tap tent upsample-and-accumulate kernel.
4. Implement `ExecuteCudaDiffusionScatter`: validate inputs, compute the layout, acquire levels,
   launch the chain, write the output image, release levels.
5. Replace the DF2 encoder stub with a call to `ExecuteCudaDiffusionScatter`.
6. Add a mix variant of the camera color kernel: 2D index from the 1D index, B-spline sample,
   warmth matrix, mix, then the current matrix, profile, compression, and encode.
7. Select the mix variant in `ExecuteCudaCameraColor` only when the effect is active.
8. Add the tests. Record CUDA timing in release mode.

### 13.6 Primary success call chain

```text
PlanExecutor scatter block miss
  -> PassEncoder<CudaBackend, DiffusionScatter>::Encode
  -> ExecuteCudaDiffusionScatter
       -> ReduceBoostKernel (sensor_linear -> level L0)
       -> Down13Kernel x (K-1)
       -> UpAccumulateKernel x (K-1) -> runtime.diffusion_scatter
  -> Record(runtime.diffusion_scatter)
  -> ExecuteCudaCameraColor -> CameraColorMixKernel -> develop:image
```

### 13.7 Primary failure and restore call chain

```text
sensor_linear missing, or a launch error from cudaGetLastError
  -> std::runtime_error "ExecuteCudaDiffusionScatter: ..."
  -> PlanExecutor CancelRender -> rethrow
  -> no publish; sensor_linear stays published; no CPU or other-backend retry
```

### 13.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `CudaDiffusionScatterMatchesCpuReferenceWithinTolerance` | Synthetic 1024 x 683 image: maximum relative error against the reference is 1e-3 or less where the reference is above 1e-4. |
| `CudaDiffusionImpulseEnergyIsConserved` | Boost 0, `black_mist` 0: output energy over input energy is 1 within 1e-3. |
| `CudaInactiveDiffusionKeepsCameraColorOutputBitIdentical` | `enabled = false` output equals the output without the feature code path. |
| `CudaDiffusionPreviewMatchesDownscaledExportWithinTolerance` | A CI RAW fixture: a 2560 px render against the downscaled full render. Relative error of the effect is 5% or less. |
| `CudaDiffusionDetailRoiMatchesFullFrameCrop` | A 1:1 ROI render equals the matching crop of the full render within 1e-3 relative error. |
| `CudaHighlightGlowIncreasesGlowAroundClippedLight` | Glow energy in a ring around a clipped lamp rises when `highlight_glow` rises. Midtone patch change stays below 1%. |
| `CudaStrengthEditReusesScatterImage` | A second render after a strength edit counts one scatter skip and no scatter execute. |
| `CudaGlowRadiusEditKeepsSensorDevelopCached` | A render after a `glow_radius` edit counts one sensor develop skip. |
| `CudaDiffusionScatterWithoutSensorLinearThrows` | The pass throws the named error and publishes nothing. |

Record the scatter pass GPU time on a 24 MP RAW in a release build: p50, p95, and maximum over
50 renders. Store the log under `build/tmp/diffusion_filter/`. Record the numbers in the
completion record.

### 13.9 Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaDevelopTest GpuDagRawInputTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "^GpuDagCudaDevelopTest\."
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(GpuDagCudaDevelopTest|GpuDagRawInputTest)\." > build/tmp/diffusion_filter/df3_ctest.log
```

Use `win_release` only for the timing record.

### 13.10 Exit criteria

- [ ] The CUDA output matches the CPU reference.
- [ ] Preview and export agree within the stated tolerance.
- [ ] ROI and full frame agree.
- [ ] Strength and warmth edits do not run the scatter pass.
- [ ] The timing record exists. The time is 10 ms or less.

### 13.11 Expected diff

1,100-1,700 lines.

### 13.12 Completion record

Use the template in section 11.12. Add the timing distribution.

---

## 14. DF4 — Metal scatter kernels, fused mix, and Metal evidence

### 14.1 Objective and deliverables

Implement section 3.2 in Metal with the same results as CUDA. Add the mix to
`camera_color.metal`.

### 14.2 Inputs and prerequisites

- DF2 complete. DF3 complete is recommended, because it fixes the kernel details.
- A macOS machine with Metal. Configure `macos_debug` with `-DALCEDO_BUILD_TESTS=ON`.

### 14.3 Modules, files, and APIs

Add (proposed): `include/edit/runtime/metal/metal_diffusion_scatter_pass.hpp`,
`edit/runtime/metal/metal_diffusion_scatter_pass.mm`,
`edit/runtime/metal/shader/diffusion_scatter.metal`.

Modify: `include/edit/runtime/metal/metal_pass_encoder.hpp`,
`edit/runtime/metal/metal_develop_pass.mm`, `edit/runtime/metal/shader/camera_color.metal`,
`metal/CMakeLists.txt` (new metallib and compile definition), `edit/CMakeLists.txt`,
`alcedo_studio/tests/edit/runtime/metal_develop_test.cpp`.

### 14.4 Data rules and invariants

- The rules of section 13.4 apply.
- Compute pipeline states come from the existing Metal pipeline cache, as in
  `metal_local_tone_pass.mm`. Do not create a pipeline state for each render.
- The shader uses the byte layout of `DiffusionScatterGpuParams` and `DiffusionMixGpuParams`.

### 14.5 Implementation steps

1. Port the three scatter kernels to `diffusion_scatter.metal`.
2. Register the metallib in `metal/CMakeLists.txt` like `local_tone.metal`.
3. Implement `ExecuteMetalDiffusionScatter` and replace the encoder stub.
4. Add the mix variant to `camera_color.metal` and select it in `metal_develop_pass.mm`.
5. Add the Metal tests.

### 14.6 Primary success call chain

Same as section 13.6 with the Metal pass and shader names.

### 14.7 Primary failure and restore call chain

Same as section 13.7 with `ExecuteMetalDiffusionScatter` and the Metal command buffer error.

### 14.8 Tests and evidence

Add the Metal forms of the section 13.8 tests with the prefix `Metal` in place of `Cuda`.
Add `MetalDiffusionMatchesCudaWithinTolerance` only if a shared expected file exists. Otherwise
compare both backends to the CPU reference.

### 14.9 Build and run commands

```bash
cmake --preset macos_debug -DALCEDO_BUILD_TESTS=ON
cmake --build --preset macos_debug --target GpuDagMetalDevelopTest
ctest --test-dir build/macos-debug -N -R "^GpuDagMetalDevelopTest\."
ctest --test-dir build/macos-debug -j 1 --output-on-failure -R "^GpuDagMetalDevelopTest\."
```

On Windows, record the Metal result as unavailable. Do not record it as a pass.

### 14.10 Exit criteria

- [ ] The Metal output matches the CPU reference.
- [ ] Preview, export, and ROI agree within the section 13.8 tolerances.
- [ ] The Metal tests ran on macOS, or the record states that they are unavailable.

### 14.11 Expected diff

900-1,400 lines.

### 14.12 Completion record

Use the template in section 11.12.

---

## 15. DF5 — OpenCL scatter kernels, fused mix, and OpenCL evidence

### 15.1 Objective and deliverables

Implement section 3.2 in OpenCL 1.2 with the same results as CUDA. Add the mix to the OpenCL
camera color kernel. Register the new program through the `gpu_dag` manifest.

### 15.2 Inputs and prerequisites

- DF2 complete. DF3 complete is recommended.
- An OpenCL device. The OpenCL 1.2 baseline of the OpenCL migration plan applies.

### 15.3 Modules, files, and APIs

Add (proposed): `include/edit/runtime/opencl/opencl_diffusion_scatter_pass.hpp`,
`edit/runtime/opencl/opencl_diffusion_scatter_pass.cpp`,
`edit/runtime/opencl/shader/diffusion_scatter.cl`.

Modify: `include/edit/runtime/opencl/opencl_pass_encoder.hpp`,
`edit/runtime/opencl/opencl_develop_pass.cpp`, `edit/runtime/opencl/shader/geometry_camera.cl`,
`include/edit/runtime/opencl/opencl_dag_programs.hpp` (program name
`opencl_dag_diffusion_scatter` and kernel names), `edit/runtime/opencl/opencl_gpu_dag_programs.cpp`,
`opencl/CMakeLists.txt` (compile definition `ALCEDO_OPENCL_DAG_DIFFUSION_SCATTER_CL`),
`edit/CMakeLists.txt`, `alcedo_studio/tests/edit/runtime/opencl_develop_test.cpp`.

### 15.4 Data rules and invariants

- The rules of section 13.4 apply.
- The program is registered once in the `gpu_dag` manifest. No pass registers a program.
- Program and kernel names are constants in `opencl_dag_programs.hpp`. Execution code does not
  hold string literals for them.
- The kernels use only OpenCL 1.2 features.

### 15.5 Implementation steps

1. Port the three scatter kernels to `diffusion_scatter.cl`.
2. Add the program descriptor to the manifest and the compile definition to CMake.
3. Implement `ExecuteOpenClDiffusionScatter` and replace the encoder stub.
4. Add the mix kernel variant to `geometry_camera.cl` and select it in
   `opencl_develop_pass.cpp`.
5. Add the OpenCL tests.

### 15.6 Primary success call chain

Same as section 13.6 with the OpenCL pass, program, and kernel names.

### 15.7 Primary failure and restore call chain

Same as section 13.7 with `ExecuteOpenClDiffusionScatter` and the OpenCL error code in the
message. A program build failure reports the build log. It does not select another backend.

### 15.8 Tests and evidence

Add the OpenCL forms of the section 13.8 tests with the prefix `OpenCl`. Add
`OpenClDiffusionProgramBuildsFromManifest`: the program builds through the registry and each
kernel name resolves.

### 15.9 Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagOpenClDevelopTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "^GpuDagOpenClDevelopTest\."
ctest --test-dir build/debug -j 1 --output-on-failure -R "^GpuDagOpenClDevelopTest\." > build/tmp/diffusion_filter/df5_ctest.log
```

### 15.10 Exit criteria

- [ ] The OpenCL output matches the CPU reference.
- [ ] Preview, export, and ROI agree within the section 13.8 tolerances.
- [ ] The program builds through the manifest.

### 15.11 Expected diff

900-1,400 lines.

### 15.12 Completion record

Use the template in section 11.12.

---

## 16. DF6 — Diffusion section in QML and translations

### 16.1 Objective and deliverables

Add the Diffusion section of section 3.8 in the panel that U2 selects. Add English and
Simplified Chinese strings. A user can turn on the effect and change all five values.

### 16.2 Inputs and prerequisites

- DF3, DF4, and DF5 complete. Record an unavailable backend as a gap, not as complete.
- The user answer to U2. The user answer to U3 if the UI shows grade presets.

### 16.3 Modules, files, and APIs

Modify: `ui/alcedo_main/qml/EditorDetailPanel.qml` (U2 option A), the panel presentation owner
that loads panel values from a snapshot, `ui/alcedo_main/i18n/alcedo_main_en.ts`,
`ui/alcedo_main/i18n/alcedo_main_zh_CN.ts`, `alcedo_studio/tests/ui/simplified_chinese_catalog_test.cpp`,
and the UI test registration when a new test source is necessary.

Add a new QML file only if the section cannot stay in the current file. Register a new file in
the QML file list of `ui/alcedo_main` CMake.

### 16.4 Data rules and invariants

- Each control writes field key `diffusion_filter` with one object key.
- Each slider range equals the section 3.3 range. The QML does not clamp to another range.
- The panel loads all six values from the panel snapshot on workspace entry.
- All colors, spaces, radii, and type sizes come from `appTheme`.
- The production style stays Basic. No Material import.
- No `lupdate` run. The `.ts` edits are manual.

### 16.5 Implementation steps

1. Read the current Halation section and its value model setup.
2. Add six value models with the field key and object keys.
3. Add the section layout above Clarity with the current section component.
4. Add snapshot load for the six values.
5. Add the strings to both `.ts` files. Mark each Chinese string as finished.
6. Update the `EditorDetailPanel` count and the total count in `SimplifiedChineseCatalogTest`.
7. Add the tests.

### 16.6 Primary success call chain

```text
Strength slider release at 0.3
  -> EditorAdjustmentValueModel commit {diffusion_filter: {strength: 0.3}}
  -> editor write path (DF1) -> one history edit
  -> render (DF2-DF5) -> viewport shows the glow
  -> workspace re-entry -> snapshot load -> slider shows 0.3
```

### 16.7 Primary failure and restore call chain

```text
editor write fails (for example a stale session)
  -> the write path returns the error
  -> the value model restores the committed value from the snapshot
  -> the slider shows the committed value; the document is unchanged
```

### 16.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `DiffusionSectionWritesEachFieldWithDevelopFieldKey` | Each value model commit produces one write with field key `diffusion_filter` and one object key. |
| `DiffusionSectionLoadsValuesFromPanelSnapshot` | A snapshot with non-default values sets all six controls. |
| `DiffusionSliderRangesMatchSpecification` | Each slider minimum and maximum equals section 3.3. |
| `SimplifiedChineseCatalogCoversDiffusionSection` | The catalog test passes with the new counts and no unfinished message. |

Offscreen QML input tests are unreliable in this repository. Verify behavior through the C++
owner and model tests first. Report skipped QML input coverage as skipped.

### 16.9 Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main EditorAdjustmentModelTest EditorPanelProjectionTest SimplifiedChineseCatalogTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "^(EditorAdjustmentModelTest|EditorPanelProjectionTest|SimplifiedChineseCatalogTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(EditorAdjustmentModelTest|EditorPanelProjectionTest|SimplifiedChineseCatalogTest)\." > build/tmp/diffusion_filter/df6_ctest.log
```

### 16.10 Exit criteria

- [ ] The section exists in the U2 panel with the U2 labels.
- [ ] All six values write, load, and restore.
- [ ] English and Simplified Chinese strings exist and the catalog test passes.
- [ ] The user checked the section in the running application. Record this as manual evidence.

### 16.11 Expected diff

700-1,200 lines.

### 16.12 Completion record

Use the template in section 11.12.

---

## 17. DF7 — Adjustment Transfer item and product qualification

### 17.1 Objective and deliverables

Add the diffusion parameters to Adjustment Transfer as one Develop item. Qualify the complete
feature on real RAW files with history, reopen, paste, export, and all available backends.

### 17.2 Inputs and prerequisites

- DF6 complete.
- The user answers to U4 and U5.
- Real RAW night images with clipped light sources. The two Panasonic RW2 images of the
  simulation are suitable for manual checks. They are local files, not repository fixtures.

### 17.3 Modules, files, and APIs

Modify: `include/app/adjustment_transfer_types.hpp` (append
`AdjustmentTransferItemKind::DiffusionFilter`, add `TransferDevelopValue::diffusion_filter`,
extend `Empty()`), `app/adjustment_transfer_package_builder.cpp`,
`app/document_transfer_planner.cpp`, `app/document_transfer.cpp`, the transfer catalog and item
pane label with its translations, `include/edit/history/pipeline_history_format.hpp` (U4),
transfer tests, and the transfer expected serialized file.

### 17.4 Data rules and invariants

- The item holds exactly the six keys of the `diffusion_filter` field.
- The item is valid only on the Develop endpoint.
- A paste applies the six values as one focused Develop update and one history edit.
- The U4 answer decides the schema identity and the rejection of older packages.

### 17.5 Implementation steps

1. Append the item kind. Do not change existing enumerator values.
2. Add the value to the package builder, the catalog, and the planner.
3. Implement the U4 schema rule and its rejection test.
4. Add the item label and translations. Update the catalog counts.
5. Run the qualification matrix of section 18 on each available backend.
6. Record the U5 result for thumbnails.
7. Ask the user for the visual check of section 17.8.

### 17.6 Primary success call chain

```text
Copy Adjustments with the Diffusion item selected
  -> package builder copies diffusion_filter (six keys)
  -> Paste Adjustments on another image
  -> planner -> ApplyDiffusionFilterUpdate on the target Develop model
  -> one history edit -> render shows the same effect
```

### 17.7 Primary failure and restore call chain

```text
package with an older schema (U4 option A) or an invalid diffusion value
  -> import validation fails with the schema or range error
  -> no target document changes
```

### 17.8 Tests and evidence

| Test behavior | Required assertion |
| --- | --- |
| `DiffusionFilterTransferCopiesAllSixValues` | The package holds the six source values and no other Develop key for this item. |
| `DiffusionFilterPasteCreatesOneHistoryEdit` | The target has one new history edit. Undo restores the prior values. |
| `TransferPackageRejectsPreviousSchema` | U4 option A only: v7 import fails with the schema error. |
| `DiffusionFilterSurvivesSaveAndReopen` | A saved project reopens with the same values and the same rendered pixels within 1e-3. |
| `DiffusionFilterFollowsVersionCheckout` | Two Versions with different strengths render their own values after checkout. |
| `DiffusionExportMatchesEditorQualityRender` | Export pixels match the editor full-resolution render within 1e-3 on each available backend. |

Manual checks by the user, recorded as user evidence:

- Night street image: lamps and headlights glow. Railings, road texture, and faces stay sharp.
- `black_mist` 0 against 1: haze and shadow lift decrease.
- The U6 boost measure looks correct on colored lights.
- Library thumbnail against editor view: the U5 result.

### 17.9 Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DocumentTransferTest AdjustmentTransferServiceTest AdjustmentTransferCatalogTest SimplifiedChineseCatalogTest GpuDagCudaDevelopTest GpuDagOpenClDevelopTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -N -R "^(DocumentTransferTest|AdjustmentTransferServiceTest|AdjustmentTransferCatalogTest|SimplifiedChineseCatalogTest)\."
ctest --test-dir build/debug -j 1 --output-on-failure -R "^(DocumentTransferTest|AdjustmentTransferServiceTest|AdjustmentTransferCatalogTest|SimplifiedChineseCatalogTest|GpuDagCudaDevelopTest|GpuDagOpenClDevelopTest)\." > build/tmp/diffusion_filter/df7_ctest.log
```

Run the Metal forms on macOS with the commands of section 14.9.

### 17.10 Exit criteria

- [ ] Transfer copies and pastes the six values as one item.
- [ ] The U4 schema rule is implemented and tested.
- [ ] Reopen, Version, Undo, Redo, and export results match.
- [ ] Each backend result is pass, fail, or unavailable. None is omitted.
- [ ] The user manual checks are recorded as user evidence.

### 17.11 Expected diff

1,000-1,600 lines.

### 17.12 Completion record

Use the template in section 11.12. Add the per-backend result table and the U5 result.

---

## 18. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Default document | The effect is off. Pixels equal the pre-feature pixels. | DF1, DF3-DF5 |
| Current `0.10.0` project | Opens with the effect off (U1 option A). | DF1 |
| Boundary values | `strength` 0 and 0.5, `glow_radius` 0.02 and 0.25, `black_mist` 0 and 1, `highlight_glow` 0 and 12, `warmth` -0.2 and 0.2 render without error. | DF3-DF5 |
| Invalid input | Out-of-range or non-finite values fail on write and on load. Nothing changes. | DF1 |
| Energy | With boost 0 and `black_mist` 0, total energy stays within 1e-3. | DF2-DF5 |
| Highlight selectivity | Clipped lamps glow. A midtone patch changes by less than 1% at the default strength. | DF3-DF5 |
| Preview and export | Relative error of the effect is 5% or less. | DF2-DF5 |
| Detail ROI | ROI equals the full-frame crop within 1e-3. | DF3-DF5 |
| Cache reuse | Strength and warmth edits skip the scatter pass. Scatter edits skip the RAW decode. | DF3-DF5 |
| Owner failure | A pass failure publishes no frame and keeps the develop output cached. | DF3-DF5 |
| Persistence | Save and reopen keep values and pixels. | DF7 |
| Undo and Redo | One edit per settled input. Undo restores prior pixels. | DF1, DF7 |
| Versions and Paste | Each Version keeps its values. Paste applies one item. | DF7 |
| QML | Sliders load, write, and restore. Strings exist in both languages. | DF6 |
| Real product | Night RAW images show the specified look. The user confirms. | DF7 |

---

## 19. Build and evidence rules

- Windows: `win_debug` for tests. `win_release` for timing only. Build directory `build/debug`
  or `build/release`. Run MSVC commands through the PowerShell tool.
- macOS: `macos_debug` with `-DALCEDO_BUILD_TESTS=ON` in `build/macos-debug`.
- Add the vcpkg debug `bin` directory to `PATH` before `ctest`. Qt is linked statically.
- Run GPU tests with `ctest -j 1`.
- A test executable can load an old first-party DLL from its `_runtime/` folder. Relink the test
  when a DLL-only change can affect it.
- Required backends: CUDA and OpenCL on Windows. Metal on macOS. Record each backend as pass,
  fail, skip, or unavailable. Do not report an unavailable result as zero. Do not report a skip
  as a pass.
- Put logs under `build/tmp/diffusion_filter/`. Remove them when the phase is complete.
- Do not run the full `ctest` suite. Only the user can start it.
- Manual checks by the user supplement the automated tests. Record them as user evidence.

---

## 20. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| The clip level differs between backends or input kinds. | DF2 step 1 finds different values. | Stop. Record the facts. Ask the user before a choice. |
| The boost makes colored lights too strong (U6). | DF7 visual check. | Report with images. Do not change the formula without a user decision. |
| The tent reduction shows shift variance. | `ReferenceOffGridImpulsePeakRatioMeetsTarget` below 0.95. | Record the value. Propose a wider footprint with its measured cost. |
| B-spline sampling of the base level shows structure around small lights. | DF3 visual check at 1:1 zoom. | Record crops. Propose a finer base level with its memory cost. |
| The scatter pass is slow. | DF3 timing above 10 ms. | Stop. Report the timing. Do not reduce resolution. |
| Content key or invalidation misses a dependency. | A stale frame after an edit in DF3 tests. | Fix the key or the origin. Do not add a stale-result guard. |
| Serialized fixtures change under U1 option A. | `PipelineEditBatchTest` expected files fail. | Stop. The omit rule has a defect. |
| A backend phase cannot run on the local machine. | No device. | Record unavailable. Do not mark the phase complete for that backend. |

Stop implementation when:

- a required owner API in section 5 does not exist;
- the source audit no longer matches the branch;
- a phase can pass the 2000-line limit;
- a persistence change lacks the U1 or U4 answer;
- an operation needs a fallback that the user did not approve;
- a proposed consistency mechanism lacks a real production interleaving.

Update this plan before implementation continues.
