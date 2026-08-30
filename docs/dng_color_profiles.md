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

Profile data is immutable and shared by image metadata and the Develop node. Both
image metadata JSON and pipeline JSON preserve the tables. A content fingerprint is
recomputed when data is read and participates in camera-pass cache identity. Changing
only the profile reuses the sensor result and recomputes the color pass. Thumbnail
cache version 2 excludes images made before this support was added.

Older projects may have stored calibrated ColorMatrix values without their profile.
`PipelineMgmtService::InjectImageRawMetadata` resolves that case from the source DNG
before editor, thumbnail, or export rendering. It replaces the baked matrices with
tagged matrices and separate calibration, without mutating a shared Image during a
render. An unavailable or malformed source raises an error. Projects already storing
the profile do not reread the file for color metadata.

## Scope and verification

This support covers the embedded single/dual-illuminant color path described above.
It is not a complete Adobe Camera Raw renderer: external DCP selection, triple-illuminant
profiles, ProfileToneCurve, and Adobe process-version rendering are not implemented
here. Alcedo continues to use its existing ACES output transform. DNG decoding and
opcode support remain separate from this color-profile change.

`DngColorProfileTest` checks interpolation, encoding, calibration, invalid data,
serialization, and legacy metadata. The CUDA/OpenCL/Metal Develop test suites include
a full-resolution Canon R6 III DNG regression with scalar/GPU agreement, graph reload,
color-only cache invalidation, and highlight reconstruction. Private camera fixtures
are optional and produce an explicit skip when absent. Set `ALCEDO_DNG_RENDER_OUTPUT`
to an existing directory under `build/tmp/` to save diagnostic previews; the render
itself still runs at full resolution.

Reference behavior was checked against Adobe's DNG SDK sources:
[camera calibration](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_color_spec.cpp),
[render ordering](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_render.cpp),
and [table import](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_hue_sat_map.cpp).
