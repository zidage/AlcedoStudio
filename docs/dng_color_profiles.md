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
| Develop JSON `camera_profile_file` (same locations), present only for a selected DCP profile | `{"camera_model", "profile_name", "fingerprint"}`, and `"profile_tone": false` when the Tone Curve switch is off |
| `QSettings` key `cameraProfiles/dcpLibraryRoot` | DCP folder path |

A DCP reference holds identity strings and the fingerprint of the file content. The project
database, the project package, and exported files hold no matrix and no table of a DCP file. A
document without a DCP selection has no `camera_profile_file` key, so its JSON and root id stay
as before the DCP support.

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

## External DCP camera profiles

A DCP file (DNG Camera Profile file) from a folder that the user supplies can replace the import
profile of a RAW or DNG image
([DCP Camera Profile Plan](roadmap/alcedo_studio/raw-processing/dcp_camera_profile_plan.md)).
Alcedo Studio ships no DCP files. An example folder is `Resources/CameraProfiles/Camera` of a
Lightroom Classic installation.

### Library folder and camera match

The Camera profiles tile of **Settings > LUTs and Color Management** holds one DCP folder
(setting key `cameraProfiles/dcpLibraryRoot`), with Browse and Rescan.
`CameraProfileLibraryService` scans the folder on one worker task and publishes the complete index
of `DcpProfileLibrary` in one step. The scan reads only the identity of each file:
`UniqueCameraModel` and `ProfileName`. A file that fails to read counts as rejected, and the log
holds its path and reason. A full profile read (matrices and tables) happens when a document binds
the profile; the library keeps the last 32 loaded profiles.

A profile belongs to the camera of the image when the compact key of its `UniqueCameraModel`
(lower-case letters and digits) equals the compact key of the image model, or of the make word
and the model. The make word comes from an alias table (`NIKON CORPORATION` gives `Nikon`). The
match is exact: a profile of a similar model never matches.

### Selection and render

The editor section **Camera Profile**, above White Balance, lists the default entry and the
matched profiles in name order. The default entry is **Built-in** for a RAW file and **Embedded**
for a DNG file with an embedded profile; it renders the import profile. A selection is one settled
edit of the Develop field `camera_profile_file`, so Undo, Redo, and version checkout restore it.
When the combo holds only the default entry, a note under it gives the reason: no DCP folder is
set, or the folder has no profile of this camera.

With a selected profile, the camera pass takes `ColorMatrix1/2`, `ForwardMatrix1/2`, the two
illuminant CCT values, the HueSatMap, the LookTable, their encodings, and `BaselineExposureOffset`
from the DCP file. It takes `AsShotNeutral` (or `cam_mul`) from the image. For a DNG source it also
takes `AnalogBalance` and `BaselineExposure`, and `CameraCalibration1/2` when the
`CameraCalibrationSignature` of the source equals the `ProfileCalibrationSignature` of the DCP
file (identity calibration otherwise). A selection solves the as-shot white balance again from
the DCP matrices; custom temperature and tint keep their numbers. The selection changes only the
camera pass: the render reuses the sensor result.

The render path is the table evaluation of the embedded profile: one implementation for CUDA,
OpenCL, and Metal.

### Binding and errors

`BindDcpCameraProfile` resolves a stored reference through the library in every document build
(editor, thumbnail, export, comparison, and analysis renders). A reference that the library cannot
resolve stays unbound, and its render fails with `ColorTransformError::UnboundCameraProfileFile`
and the message `camera profile "<name>" for "<camera>" is not in the DCP folder`. The project
still opens, the combo shows the stored name in the warning color, and the user can select another
entry. When the library file of the identity has another fingerprint, the library file renders and
the reference takes its fingerprint.

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
writes the identity RGB curve and saturation 1.0 of Color Grade 1 in the same Undo step, each while it
still holds the RAW import default (the Film-like curve, saturation 1.3). A document imported
before the Film-like curve (#891) holds contrast 15 instead, and the entry writes contrast 0.
Exposure stays.

## Scope and verification

This support covers the single/dual-illuminant color path described above, from an embedded DNG
profile or a DCP file. Alcedo renders through its own output transform; a DCP tone curve enters
through the profile tone stage. Outside this scope: triple-illuminant profiles, the
`ProfileToneCurve` of an embedded DNG profile, the ACR3 default curve, and Adobe process-version
rendering. A DCP file with `ColorMatrix3`, `RGBTables`, or a `ReductionMatrix` fails to load with
an error that names the tag. The DCP reference is outside adjustment transfer, copy and paste, and
`alcedo-cli` parameters, because a reference belongs to one camera model. DNG decoding and opcode
support remain separate from this color-profile change.

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

The same three suites hold `DcpProfileMatchesScalarReference`. It renders a CI Sony ARW file
at full resolution with the import profile, selects a synthetic DCP file (two illuminants,
forward matrices, two HueSatMaps, a LookTable with sRGB value encoding, an exposure offset, and a
tone curve), and renders again on the same device. The second render reuses the sensor result
and runs the camera pass once; its samples equal scalar `DngApplyColorProfile` on the DCP tables
within 3e-4 in ACEScc. Clearing the selection renders the import pixels again.
`DcpFileReaderTest`, `DcpProfileLibraryTest`, `DcpCameraProfileRenderTest`,
`PipelineDcpProfileBindingTest`, and `EditorCameraProfileListModelTest` check the reader, the
library scan and match, the effective profile, the binding on each load path, and the editor
list. Test DCP files come from `tests/image/dcp_test_file_writer.hpp`; no vendor profile bytes
enter the repository.

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
| RAW import curve | RGB curve of the Default document | Tone Curve preset | `film_like` | `kDefaultPipelineToneCurvePresetId` (#891) |
| Earlier RAW import contrast | Contrast of a document imported before #891 | slider value | 15 | `kImportLookContrastBeforeFilmLike` |
| RAW import saturation | Saturation of the Default document | multiplier | 1.3 | `kDefaultPipelineSaturation` |
