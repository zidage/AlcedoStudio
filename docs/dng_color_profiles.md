# Embedded DNG color profiles

Alcedo applies embedded DNG camera calibration and color correction tables before its
ACES output transform. This fixes the desaturated appearance caused by using a
ForwardMatrix without its accompanying HueSatMap and LookTable.

## Supported color data

| Data | Handling |
| --- | --- |
| ColorMatrix1/2, CalibrationIlluminant1/2 | Preserve the tagged matrices; interpolate by reciprocal color temperature. |
| AnalogBalance, CameraCalibration1/2 | Use separate calibration data when solving white balance and converting individual camera channels to reference camera channels. Camera/profile signatures must match for CameraCalibration to apply. |
| ForwardMatrix1/2 | Normalize each endpoint to D50 and combine it with reference-camera white balance. A table does not disable ForwardMatrix. |
| ProfileHueSatMapData1/2 | Interpolate illuminant tables, wrap the hue coordinate, and evaluate the original table entries. |
| ProfileLookTableData | Evaluate after the hue/saturation map and baseline exposure. |
| ProfileHueSatMapEncoding, ProfileLookTableEncoding | Linear or sRGB value encoding. |
| BaselineExposure, BaselineExposureOffset | Apply the sum as an exposure adjustment in stops. |

Tables use value/hue/saturation order, with saturation varying fastest. Both two-axis
dimensions and three-axis dimensions are read. If a table omits its zero-saturation
rows, those rows use the first saturation row's hue/saturation corrections and a value
scale of one, as specified by the DNG SDK. Invalid dimensions, missing data, unknown
encodings, non-finite corrections, and invalid neutral value scales raise errors.
Each table is limited to 4,194,304 floats; larger tables fail explicitly.

## Rendering and storage

The camera pass transforms camera RGB to AP1, converts to linear ProPhoto RGB with a
D50 white point for the embedded tables, applies HueSatMap, baseline exposure, and
LookTable in that order, then converts back to AP1 and encodes ACEScc. CUDA, OpenCL,
and Metal compile the same table evaluation code. Table buffers belong to the render
workspace and remain alive through GPU submission. No CPU rendering substitute is used.

Alcedo retains positive scene values above one for its floating-point pipeline. Table
lookup coordinates stop at the table boundary; table corrections do not clip the
resulting positive scene values. This is an Alcedo scene-linear extension, not a claim
of pixel equivalence to the SDK's bounded reference renderer. Negative ProPhoto
channels are clamped before HSV table evaluation.

Profile data is immutable. One shared profile object serves image metadata and the
Develop node of every document that references it.

### Storage

Project data does not store the profile tables. Project format 0.10.0 stores only a
profile reference:

| Location | Stored value |
| --- | --- |
| `Image.metadata` → `RawRuntimeColorContext.DngProfileFingerprint` | 16 hexadecimal digits, or null |
| Develop JSON `camera_profile.dng_profile_fingerprint` (`PipelineParam`, `ImageEditState` checkpoint, `PipelineRoot` document) | 16 hexadecimal digits, or null |

The fingerprint is the FNV-1a hash of the complete profile content. It is the profile
input to the root id hash and to the camera-pass cache identity. Changing only the
profile reuses the sensor result and recomputes the color pass. The small RAW context
fields (color and forward matrices, AsShotNeutral, illuminant CCTs, cam_mul, lens data)
stay stored, because Develop binding needs them before decode.

The profile tables are runtime data:

- Import reads the profile from the DNG IFD0 tags and binds it on the imported image and
  the new document.
- `DngColorProfileCache` (process-wide) keeps profiles read from source files. Its key is
  the normalized path plus file size and last write time, and it holds at most 100 files
  (least recently used eviction). Loads with the same fingerprint share one profile object.
- `PipelineMgmtService` binds the referenced profile from the element's source file before
  a loaded document goes live: in `LoadPipeline` (thumbnail, export, and analysis
  renders), `InitializeImageRoot` for an existing root, `LoadEditorPipeline` (root state,
  checkpoint, and replay), `CheckoutVersion`, and `RebuildActiveEditorPipeline`.
  `ClonePipelineDocument` keeps the source document's bound profile.
- A document read from JSON holds an unbound reference. Rendering an unbound reference
  fails (`ColorTransformError::UnboundDngProfile`); it never renders without the profile.
- A missing source file fails the load; the render needs the RAW data from that file too.
- When the source file has another profile than the stored fingerprint, the source file
  wins, and one warning is logged. The cache identity changes with the fingerprint.

## DCP camera profiles and the profile tone stage

A DCP file from the user's DCP folder can replace the import profile of a RAW or DNG image
([DCP Camera Profile Plan](roadmap/alcedo_studio/raw-processing/dcp_camera_profile_plan.md)).
Its matrices, HueSatMap, and LookTable take the camera pass above. The project stores the
reference `camera_profile_file` {camera model, profile name, fingerprint}; the tables stay in
memory.

The `ProfileToneCurve` of a DCP profile (tag 50940) applies through the profile tone stage
([DCP Profile Tone Curve Study](roadmap/alcedo_studio/raw-processing/dcp_profile_tone_curve_study.md),
section 6). Its symbols:

- `x` is the AP1 scene-linear RGB at the entry of the display transform, after every grade.
- `M` is the camera-pass matrix pair from AP1 to linear ProPhoto RGB (D50).
- `E_a` is the exposure anchor, 1.5 EV: Alcedo +1.5 EV renders mid grey as ACR 0 EV.
- `C` is the profile curve as a 4097-entry spline table.
- `H` is the highlight shoulder at the curve input: identity up to the knee `k` = 0.5 (one stop
  below raw white), then `1 - (1 - k)^2 / (v + 1 - 2k)` for an input `v`. It has slope 1 at the
  knee and stays below 1, so raw white maps to 0.75 and each further stop halves the distance to
  the curve's white.
- `N_m^-1` is the inverse neutral response of DRT method `m` (ACES 2.0 or OpenDRT) at its
  default parameters on a 100-nit SDR reference display.

Per pixel, the stage computes `M^-1 * N_m^-1(R_C(H(M * x * 2^-E_a)))`, with `H` per channel and
negative channels at 0, where `R_C` is the DNG SDK baseline RGB tone with `C`. The DRT then
renders the result, so the DRT maps tone once. A neutral below the knee renders the profile curve
on the reference display. Scene light above raw white, from a higher exposure or from highlight
reconstruction, rolls toward display white and keeps its gradation in place of a clip at raw
white (study section 6.6). CUDA, OpenCL, and Metal compile
one implementation (`profile_tone_gpu_math.h`). The stage runs when the reference is bound, its
profile has the curve, the Develop key `camera_profile_file.profile_tone` is true (the default;
the key is written only as `false`), and the DRT method is ACES 2.0 or OpenDRT. Film Print and
Negative Scan render without it.

In the editor, the switch **Tone Curve** under the Camera Profile combo sets `profile_tone`. A
profile tone entry, a settled edit after which the stage runs and before which it stayed out,
writes contrast 0 and saturation 1.0 of Color Grade 1 in the same Undo step, each while it still
holds the RAW import default (contrast 15, saturation 1.3). Exposure stays.

## Scope and verification

This support covers the single/dual-illuminant color path described above, from an embedded DNG
profile or a DCP file. Alcedo renders through its own output transform; a DCP tone curve enters
through the profile tone stage. Outside this scope: triple-illuminant profiles, the
`ProfileToneCurve` of an embedded DNG profile, the ACR3 default curve, and Adobe process-version
rendering. DNG decoding and opcode support remain separate from this color-profile change.

`DngColorProfileTest` checks interpolation, encoding, calibration, invalid data,
fingerprint persistence, unbound references, and profile binding on clone and JSON load.
`DngColorProfileCacheTest` checks eviction, sharing by fingerprint, and reload after a
file change. `PipelineDngProfileBindingTest` checks that project tables hold no profile
tables and that each load path binds the source profile. The CUDA/OpenCL/Metal Develop
test suites include a full-resolution Canon R6 III DNG regression with scalar/GPU
agreement, graph reload, a render of the reloaded and source-bound document that must
match the import-bound render, color-only cache invalidation, and highlight
reconstruction. Private camera fixtures
are optional and produce an explicit skip when absent. Set `ALCEDO_DNG_RENDER_OUTPUT`
to an existing directory under `build/tmp/` to save diagnostic previews; the render
itself still runs at full resolution.

`DngToneCurveTest`, `ProfileToneReferenceTest`, and the `ProfileTone` fixtures of the CUDA,
OpenCL, and Metal DRT suites check the curve, the stage, and its GPU renders.
`EditorProfileToneEntryHistoryTest` checks the entry reset and its Undo step.

Reference behavior was checked against Adobe's DNG SDK sources:
[camera calibration](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_color_spec.cpp),
[render ordering](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_render.cpp),
and [table import](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_hue_sat_map.cpp).

## Terms and parameters

| Symbol or parameter | Meaning | Unit | Value | Source |
| --- | --- | --- | --- | --- |
| `x` | AP1 scene-linear RGB at the display transform entry | relative scene light | per pixel | Alcedo |
| `M` | AP1 to linear ProPhoto RGB (D50) | matrix | camera-pass matrix | Alcedo |
| `E_a` | Exposure anchor between Alcedo and ACR | EV | 1.5 | Data, study section 3.3 |
| `C` | Profile tone curve as a spline table | display light | 4097 entries on [0, 1] | DNG SDK |
| `H` | Highlight shoulder at the curve input | ACR scale (raw white 1) | identity to `k`, limit 1 | Study section 6.6 |
| `k` | Knee of `H` (`kProfileToneHighlightKnee`) | ACR scale | 0.5, one stop below raw white | Study section 6.6 |
| `R_C` | Baseline RGB tone with `C` | display light | per pixel | DNG SDK `RefBaselineRGBTone` |
| `N_m^-1` | Inverse neutral response of DRT method `m` | relative scene light | 4097 entries | Alcedo DRT, default parameters, 100 nits |
| `profile_tone` | Develop key of `camera_profile_file`; written only as `false` | boolean | default true | Study section 6.3 |
| Entry contrast | Contrast of Color Grade 1 after a profile tone entry | slider value | 0 | User decision, study section 6.5 |
| Entry saturation | Saturation of Color Grade 1 after a profile tone entry | multiplier | 1.0 | User decision, study section 6.5 |
| RAW import contrast | Contrast of the Default document | slider value | 15 | `kDefaultPipelineContrast` |
| RAW import saturation | Saturation of the Default document | multiplier | 1.3 | `kDefaultPipelineSaturation` |
