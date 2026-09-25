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

## Scope and verification

This support covers the embedded single/dual-illuminant color path described above.
It is not a complete Adobe Camera Raw renderer: external DCP selection, triple-illuminant
profiles, ProfileToneCurve, and Adobe process-version rendering are not implemented
here. Alcedo continues to use its existing ACES output transform. DNG decoding and
opcode support remain separate from this color-profile change.

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

Reference behavior was checked against Adobe's DNG SDK sources:
[camera calibration](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_color_spec.cpp),
[render ordering](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_render.cpp),
and [table import](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_hue_sat_map.cpp).
