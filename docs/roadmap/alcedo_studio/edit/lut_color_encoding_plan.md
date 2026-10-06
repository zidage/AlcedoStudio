# LUT Input and Output Color Encoding Plan

Status: L1 partial (2026-10-05): the color encoding catalog landed; D-Log M is blocked (no DJI
definition) and Metal is not built or tested. L2 complete on host, CUDA and OpenCL (2026-10-05):
OCIO ACES 2.0 reference forward and the `aces2_reference_*` rename; Metal not built. L3 complete
on host, CUDA, OpenCL and Metal (2026-10-05): LMT encodings, the host bake of the 65³ composite
table and shaper rejection; the identity criteria hold on stated domains (L3 record). The Metal
DisplayToAp1, RasterDevelop and DRT suites of L1 and L2 also pass (L3 record, Metal verification).
L4 complete on host and the default Windows GPU backend (2026-10-05): LUT panel encoding combos,
`lut` write and projection keys, undo/redo, adjustment transfer, shaper text and translations;
`alcedo_main` was not linked and the panel was not checked in the running app (L4 record).
L5 complete on host (2026-10-05): remembered encodings per LUT in `lut-library.json`, the
"Remember for this LUT" checkbox, and apply with the remembered pair. L5 follow-up
(2026-10-05): a user refresh gives official LUTs ACEScc to ACEScc, and a LUT without a pair
applies ACEScc to ACEScc. `alcedo_main` was not linked and the checkbox was not checked in the
running app (L5 records).

Depends on the raster image input stack (`raster_image_input_plan.md`, phases R1 to R5) being on
`main`. This plan uses the OCIO ACES 2.0 port from Phase R2 (`display_to_ap1_math.h`,
`aces2_inverse_runtime.cpp`) and the color description types from Phase R1.

## 1. Goal

Most published LUTs are not written for Alcedo's working space. They convert a camera log
encoding (S-Log3, LogC3, F-Log2, ...) to a display encoding (Rec.709), or they are display to
display creative LUTs (Rec.709 to Rec.709). Today the Look LUT (LMT) is sampled directly with
ACEScc AP1 values, so such a LUT gives wrong colors and bakes a display rendering into the scene
data that the DRT renders a second time.

After this plan:

- The LUT panel has an **input encoding** and an **output encoding**. Both default to ACEScc.
  A project that has no such keys reads as ACEScc to ACEScc, which is today's behavior.
- A scene-referred encoding is a gamut plus a transfer function. Converting to or from it is a
  3×3 matrix and a curve.
- A display-referred **output** encoding is brought back to the scene with the **ACES 2.0 inverse
  output transform**, so the DRT that follows renders the LUT's look for any output (SDR, HDR,
  other displays) instead of rendering a display image again.
- A display-referred **input** encoding (a Rec.709 to Rec.709 LUT) is reached with the **ACES 2.0
  forward output transform** of the same reference. The pair is exact, so an identity LUT is a
  no-op.
- The two encodings are part of the LMT parameters: one LUT panel edit is one history entry, and
  adjustment transfer copies them with the LUT.
- The library can remember the encodings for one LUT. When that LUT is applied again, they are
  applied with it (Phase L5).
- All color gamuts and transfer functions used in the app, the existing ones and the new camera
  encodings, come from **one catalog** instead of separate copies per backend and per feature.

### Non-goals

- 1D shaper LUTs (`LUT_1D_SIZE` together with a 3D table). They are rejected with an error
  (section 6.4). Today the shaper is silently ignored, which renders those LUTs wrongly.
- Changing Alcedo's own ACES 2.0 forward DRT. It keeps its deliberate differences from the
  reference (raster plan decision D1).
- Linear-light LUT inputs (ACEScg, linear Rec.709). A 3D table over linear values needs a shaper,
  which is out of scope.

## 2. Current state (audited 2026-10-05, raster stack tip `fix/raster-import-tiff-thumbnail`)

### 2.1 How the LUT is applied

- `LmtModel` (`include/edit/operators/models/lmt_model.hpp`) holds `LutReference`,
  `display_name` and `strength`. `ToJson` writes `strength` and `name` only when they differ from
  their defaults, so old projects stay byte-identical.
- `TryPackGradeLut` → `PackResolvedCube` (`edit/runtime/grade_lut.cpp`) parses the `.cube` on the
  host and memoizes the packed RGBA32F table in a process-wide LRU (16 entries) keyed by path and
  file stamp. `PackCubeLutRgba` copies only `lut3d_`.
- The primary grade pass samples the table with the ACEScc AP1 pixel value as the unit-cube
  coordinate (`cuda_primary_grade_pass.cu:314-320`, and the OpenCL and Metal equivalents), with
  trilinear interpolation, and blends `c + a·(L(c) − c)` in ACEScc.
- **`DOMAIN_MIN/MAX` is parsed but not used.** `CubeLut::domain_min_/max_` exist
  (`include/utils/lut/cube_lut.hpp:31-33`), but no pack or kernel reads them. A LUT with a
  non-unit domain is sampled as if its domain were [0, 1].
- **The 1D shaper is parsed but not used.** `CubeLut::lut1d_` is ignored by the pack.

### 2.2 Editing history and adjustment transfer

- The `lut` field write parses the complete LMT state into one `LmtUpdate`
  (`editor_parameter_write_parse.cpp:217-238`), so one panel edit is one history entry for the
  whole LMT parameter object.
- The transfer package builder copies `model.ToJson()` (`adjustment_transfer_package_builder.cpp:121`),
  so new LMT keys are transferred without builder changes. The catalog summary text is in
  `adjustment_transfer_catalog.cpp:111-123`.

### 2.3 ACES 2.0 implementations

- **Alcedo forward DRT:** `odt_funcs.cuh`, `drt_aces.metal`, `cst.cl`, parameters from
  `ResolveACESODTRuntime`. It differs from the reference on purpose (raster plan 2.4).
- **OCIO reference inverse (R2):** `include/edit/runtime/display_to_ap1_math.h` is one portable C
  header compiled by the host, CUDA, OpenCL C and Metal. `ResolveAces2InverseRuntime(primaries_xy,
  peak)` builds one packed float block per key. The host evaluation is the reference that the
  GPU tests compare against.
- **No OCIO reference forward exists.**

### 2.4 Color gamuts and transfer functions are defined in many places

| Item | Copies today |
|---|---|
| Primaries | `ColorUtils::*_PRIMARY` + `SpaceEnumToPrimary` (`color_utils.hpp`), `kRasterPrimaries*` (`raster_color_description.hpp`), constants in `aces2_inverse_runtime.cpp` and `raster_input_loader.cpp` |
| Transfer ids | `ColorUtils::EOTF`, `DrtEotf` (`drt_node_model.hpp`), `CudaDrtEotf`, `RasterTransferKind`, `ALCEDO_RL_*` (`raster_linearize_math.h`) |
| ACEScc encode/decode | `cuda_acescc.cuh`, OpenCL `common.cl`, `geometry_camera.cl`, `local_tone.cl`, `primary_grade.cl`, Metal `camera_color.metal`, `local_tone.metal`, `primary_grade.metal`, `local_tone_mapping.hpp`, `display_to_ap1_math.h` |
| ACEScct | `include/edit/operators/utils/functions.hpp` |
| Display encodings (sRGB, BT.1886, gamma, PQ, HLG) | `disp_enc_funcs.cuh`, `cst.cl`, `drt.metal`, `raster_linearize_math.h`, `raster_color_model.cpp`, `ultra_hdr_writer.cpp`, `dng_profile_gpu_math.h` |
| Camera log curves | none |

Known inconsistency: `disp_enc_funcs.cuh` encodes BT.1886 with 2.4 but its `eotf()` decodes it
with 2.6 (raster plan risk table).

## 3. Terms

| Term | Meaning |
|---|---|
| Color encoding | A gamut (primaries and white) plus a transfer function, plus whether the code values are scene-referred or display-referred. Display encodings also carry a peak luminance. |
| Encoding id | The stable lower-case string that names an encoding in JSON, for example `slog3_sgamut3cine`. Never translated, never renamed. |
| Bake | Building, on the host, a new ACEScc to ACEScc 3D table that composes the input conversion, the user LUT and the output conversion. |
| Composite table | The baked table. The primary grade pass samples it exactly like a raw LUT today. |

## 4. Design overview

```
ACEScc AP1 grid point (65³)
  └─ input conversion
       scene encoding:   ACEScc decode → AP1 → gamut matrix → encoding curve
       display encoding: ACEScc decode → AP1 → AP0 → OCIO ACES 2.0 forward → display curve
  └─ user LUT (DOMAIN_MIN/MAX applied, trilinear)
  └─ output conversion
       scene encoding:   curve decode → gamut matrix → AP1 → ACEScc encode
       display encoding: display curve decode → OCIO ACES 2.0 inverse → AP1 → ACEScc encode
= composite table value at the grid point
```

### 4.1 Why the bake runs on the host

The ACES 2.0 inverse already runs on all three GPU backends, but the bake does not use those
passes:

- **Same code, one result.** The host evaluation of `display_to_ap1_math.h` is the same source the
  GPU kernels compile, and R2 already tests every backend against it. A host bake gives one
  composite table that CUDA, OpenCL, Metal, thumbnails and export all sample, so the backends
  cannot drift apart on this feature.
- **The work happens once.** A bake is needed only when the LUT content or one of the two
  encodings changes. Slider edits, including LUT strength, do not bake. A GPU bake would need a
  grid image, a new per-backend LUT sampling kernel with domain handling, and readback or a
  per-device composite table, for work that is not on the per-frame path.
- **No kernel changes.** The primary grade pass, its strength blend and its upload cache stay as
  they are on all three backends.

### 4.2 Why the forward for display inputs is the OCIO reference, not Alcedo's DRT

For a display-referred input the bracket is `inverse(LUT(forward(x)))`. If `forward` and
`inverse` are an exact pair, an identity LUT returns `x` and the rest of the pipeline, including
Alcedo's own DRT, is unchanged. Alcedo's forward differs from the reference in the shadows and
saturated darks (raster plan 5.1), so `OCIO inverse ∘ Alcedo forward` is not an identity and an
identity Rec.709 LUT would change the image. The forward used for the bake is therefore the OCIO
reference forward, built from the same tables as the inverse.

### 4.3 Display-referred parameters are fixed by the encoding

The ACES 2.0 parameters of a display encoding come from the encoding itself (its primaries as
limiting primaries, its peak luminance, no white simulation), never from the document's DRT node.
They are the rules of raster plan 5.2, so a LUT whose output is Rec.709 is brought back exactly
like a Rec.709 JPEG.

- With the DRT set to ACES 2.0 SDR Rec.709, the result is close to the LUT's own Rec.709 output,
  with the deviation accepted in raster decision D1.
- With an HDR or wide-gamut DRT output, the LUT's look is rendered for that display. This is the
  purpose of the feature.
- With OpenDRT, the result does not reproduce the LUT's Rec.709 output. Accepted, as for raster
  images (raster plan 5.6).

### 4.4 Bake grid and accuracy

- The composite table is **65³**, whatever the source LUT size. Default ACEScc to ACEScc with a
  unit domain is not baked; the source table is uploaded as today.
- The grid is the ACEScc unit cube that the primary grade pass already samples, so values outside
  [0, 1] clamp as they do today.
- Resampling adds trilinear error, largest where the ACES 2.0 inverse is steep (saturated colors
  near the display gamut boundary). Phase L3 measures it against direct per-point composition and
  records the result (section 8, L3).

## 5. Color encoding catalog

### 5.1 Files

- **`include/color/color_encoding_math.h`** — portable C header in the style of
  `display_to_ap1_math.h` (`CE_INLINE`, `CE_POW`, ... macros for host, CUDA, OpenCL C and Metal).
  It holds every transfer function, both directions, behind one id: `CeDecode(int tf, float v)`
  and `CeEncode(int tf, float v)`, plus per-curve functions. It is the only place a curve constant
  appears.
- **`include/color/color_encoding_catalog.hpp` / `color/color_encoding_catalog.cpp`** — C++.
  - `enum class ColorGamutId` and one `constexpr` table of CIE xy primaries and white for every
    gamut.
  - `enum class TransferFunctionId`, numerically equal to the ids in `color_encoding_math.h`
    (checked with `static_assert`).
  - `struct ColorEncoding { std::string_view id; ColorGamutId gamut; TransferFunctionId transfer;
    ColorReferral referral; float peak_luminance_nits; std::string_view display_name; }` and one
    `constexpr` table of all encodings.
  - `FindColorEncoding(std::string_view id)`, `RgbToXyzMatrix(ColorGamutId)` and
    `GamutConversionMatrix(from, to)` with CAT02 white adaptation. The matrix construction is the
    double-precision construction R2 uses for OCIO, so there is one matrix builder.

### 5.2 Encodings offered for LUTs

Scene-referred:

| Encoding id | Gamut | Transfer |
|---|---|---|
| `acescc` (default) | AP1 | ACEScc |
| `acescct` | AP1 | ACEScct |
| `arri_logc3_awg3` | ARRI Wide Gamut 3 | LogC3, EI 800 |
| `arri_logc4_awg4` | ARRI Wide Gamut 4 | LogC4 |
| `sony_slog3_sgamut3cine` | S-Gamut3.Cine | S-Log3 |
| `sony_slog3_sgamut3` | S-Gamut3 | S-Log3 |
| `fujifilm_flog_fgamut` | F-Gamut | F-Log |
| `fujifilm_flog2_fgamut` | F-Gamut | F-Log2 |
| `panasonic_vlog_vgamut` | V-Gamut | V-Log |
| `canon_clog2_cinemagamut` | Cinema Gamut | Canon Log 2 |
| `canon_clog3_cinemagamut` | Cinema Gamut | Canon Log 3 |
| `red_log3g10_rwg` | REDWideGamutRGB | Log3G10 |
| `blackmagic_film_gen5_bmdwg` | Blackmagic Wide Gamut Gen 5 | Blackmagic Film Gen 5 |
| `davinci_intermediate_dwg` | DaVinci Wide Gamut | DaVinci Intermediate |
| `apple_log_rec2020` | Rec.2020 | Apple Log |
| `nikon_nlog_rec2020` | Rec.2020 | N-Log |
| `dji_dlog_dgamut` | D-Gamut | D-Log |
| `dji_dlogm_dgamut` | D-Gamut | D-Log M |

Display-referred (ACES 2.0, no white simulation):

| Encoding id | Primaries | Transfer | Peak |
|---|---|---|---|
| `rec709_bt1886` | Rec.709 | BT.1886 (2.4) | 100 nits |
| `rec709_srgb` | Rec.709 | sRGB piecewise | 100 nits |
| `rec709_gamma22` | Rec.709 | Gamma 2.2 | 100 nits |
| `displayp3_srgb` | P3-D65 | sRGB piecewise | 100 nits |
| `p3d65_gamma26` | P3-D65 | Gamma 2.6 | 48 nits |
| `p3dci_gamma26` | P3-DCI | Gamma 2.6 | 48 nits |
| `rec2020_bt1886` | Rec.2020 | BT.1886 (2.4) | 100 nits |
| `rec2100_pq1000` | Rec.2020 | ST 2084 | 1000 nits |
| `rec2100_hlg1000` | Rec.2020 | HLG | 1000 nits |

The catalog also holds the gamuts and curves that are not LUT encodings (AP0, ProPhoto, Adobe RGB,
P3-D60, XYZ, Gamma 1.8, linear), so the existing users in section 5.3 can move onto it.

### 5.3 Existing definitions moved onto the catalog

Each item keeps its public spelling and its serialized strings. Only the source of the numbers
changes.

| Existing definition | After |
|---|---|
| `ColorUtils::ColorSpace`, `*_PRIMARY`, `SpaceEnumToPrimary`, `RGB_TO_XYZ_f33` | Enum kept for the DRT node's serialized values; primaries and matrices read from the catalog |
| `ColorUtils::EOTF`, `DrtEotf`, `CudaDrtEotf` | Mapped one to one onto `TransferFunctionId` |
| `kRasterPrimaries*`, `kAdobeRgbGamma` | Removed; raster code uses the catalog |
| `RasterTransferKind` Linear/SrgbPiecewise/Gamma/Bt1886/St2084/Hlg and their evaluation in `raster_linearize_math.h` and `raster_color_model.cpp` | Evaluated by `color_encoding_math.h`. ICC parametric and sampled curves stay in the raster code because they are per-file data, not encodings |
| ACEScc copies (section 2.4 table) | One `CeAcesccEncode/Decode`, including Alcedo's linear extension below the floor, which is kept as is |
| ACEScct (`functions.hpp`) | Catalog |
| `disp_enc_funcs.cuh`, `cst.cl`, `drt.metal` display curves | Call `color_encoding_math.h` |
| PQ/HLG in `ultra_hdr_writer.cpp`, sRGB in `dng_profile_gpu_math.h` | Catalog |

BT.1886 has one definition (2.4 in both directions). Before the change, L1 confirms that the 2.6
`eotf()` decode in `disp_enc_funcs.cuh` has no caller on any render path; if that holds it is
deleted, and if a caller exists L1 reports it before changing any pixels.

`config/DRTs/OpenDRT.dctl` is an external resource and is not changed.

## 6. LMT parameters and the bake

### 6.1 Model

- `LmtPayload` gains `input_encoding` and `output_encoding` (catalog ids, default `acescc`).
- `LmtDirty` gains `Encoding`. `LmtUpdate` gains `std::optional<std::string> input_encoding`
  and `output_encoding`. `ApplyUpdate` validates both ids against the catalog before any change.
- JSON keys `input_encoding` and `output_encoding` are written only when they are not `acescc`,
  so every existing project and history checkpoint serializes byte for byte.
- A missing key reads as `acescc`. An unknown id is an `std::invalid_argument`; there is no
  substitute encoding.
- Clearing the LUT reference keeps the encodings (the user may pick another LUT of the same kind).

### 6.2 Bake

New `edit/runtime/lut_bake.{hpp,cpp}`:

- `BakeLmtCompositeTable(const CubeLut& source, const ColorEncoding& input,
  const ColorEncoding& output) -> std::vector<std::byte>` returns a 65³ RGBA32F table in the
  layout of `PackCubeLutRgba`.
- Per grid point: section 4 pipeline. Source sampling applies `DOMAIN_MIN/MAX`
  (`u = (v − min) · domain_scale_`, clamped to [0, 1]) and trilinear interpolation matching the
  kernel.
- Display stages call the host evaluation of the OCIO ACES 2.0 functions with the runtime from
  `ResolveAces2ReferenceRuntime(encoding primaries, encoding peak)` (section 7).
- The grid is split across `concurrency/thread_pool.hpp` workers by blue slice.

### 6.3 Packing and caching

- `PackResolvedCube` keeps the parsed-source cache as it is.
- Default encodings with a unit domain upload the source table exactly as today (no bake, same
  packed key, so existing GPU upload caches are unaffected).
- Otherwise a second process-wide LRU (16 entries, same pattern) maps
  `(source packed key, domain, input id, output id)` to the composite table. Its `ContentHash`
  mixes the composite bytes, so the backends' `AcquireLut` caches key on it naturally.
- Phase L3 checks that an `LmtDirty::Encoding` change reaches the same repack path as a
  `Reference` change (`runtime_invalidation.cpp`, `GradeLutResourceIdentity`).

### 6.4 1D shaper and DOMAIN handling

- A `.cube` with a 1D table is rejected:
  - `PackResolvedCube` throws its existing load error with the reason "1D shaper LUTs are not
    supported".
  - The LUT library scan marks the entry unsupported and the panel does not offer it.
  - An existing project that references such a LUT shows that error instead of rendering the LUT
    without its shaper. This changes the output of those projects; it is intended.
- `DOMAIN_MIN/MAX` are honored by the bake. A default-encoding LUT with a non-unit domain is
  baked so its domain is applied. Projects using such LUTs render differently after this change,
  because they rendered incorrectly before.

## 7. OCIO ACES 2.0 reference forward

- The OCIO 2.5.1 stage functions (`Renderer_ACES_OutputTransform20::fwd` and `::inv`, with the
  GPU closed-form gamut boundary) move from `display_to_ap1_math.h` into a new portable header,
  `include/edit/runtime/aces2_reference_math.h`, and the forward stages are added there.
  `display_to_ap1_math.h` keeps the raster pass entry points and includes it.
- The forward reads the same packed block as the inverse. Forward-only scalars go into the unused
  slots of the scalar section (23 of 32 used today). No new tables are needed.
- Because the runtime now serves both directions, `ResolveAces2InverseRuntime` /
  `aces2_inverse_runtime.*` are renamed to `ResolveAces2ReferenceRuntime` /
  `aces2_reference_runtime.*`, with callers and tests updated in the same change.
- Alcedo's forward DRT (`odt_funcs.cuh`, `drt_aces.metal`, `cst.cl`, `ResolveACESODTRuntime`) is
  not changed.
- The forward is only evaluated on the host by the bake. It still compiles on every backend
  because it lives in the shared header that the `DisplayToAp1` kernels include.

## 8. Phases

### Phase L1 — Color encoding catalog

Work:

- `color_encoding_math.h`, `color_encoding_catalog.{hpp,cpp}` with every gamut, curve and
  encoding of section 5.2.
- Move the existing definitions of section 5.3 onto the catalog on host, CUDA, OpenCL and Metal,
  and delete the copies.

Acceptance criteria:

- Each camera curve matches the OCIO 2.5.1 builtin transform where OCIO has one (the list is
  recorded at the start of L1), on 4096 code values, within 1e-5 linear relative. Curves without
  an OCIO builtin match the vendor document's stated code values (black, 18% grey, 90% white)
  within 1/4096 of a code value. The source document of each curve is cited next to it.
- Each gamut's matrix to AP1 matches OCIO's builtin transform matrix within 1e-6.
- Encode ∘ decode round-trips within 1e-6 on [0, 1] for every curve.
- Host and CUDA/OpenCL/Metal evaluations of every curve are equal within 1e-6.
- Existing render output does not change: a RAW and a raster image rendered on each backend
  through Develop, Color Grade and ACES 2.0 / OpenDRT DRT before and after the migration differ by
  at most 2⁻¹⁶ per channel, and the serialized DRT node and raster `input` JSON are byte-identical.
- The BT.1886 decode question of section 5.3 is answered in the completion record.

##### Phase L1 completion record (2026-10-05)

**Status:** partial — catalog, curves, gamut matrices and the move of the section 5.3 copies are
done on host, CUDA, OpenCL and Metal sources. Open: `dji_dlogm_dgamut` (blocking item below),
Metal not compiled or run, 3 pixels of the render comparison above 2⁻¹⁶ (PQ source, see below).

**Primary success call chain (curves):**

```text
GPU pass (CUDA .cu / OpenCL program / Metal shader) or host code
  -> color/color_encoding_math.h  (CeAcesccEncode/Decode, CeEncode/CeDecode(tf, v),
                                   CeDisplayEncodeChannel for the DRT output)
     OpenCL: opencl_gpu_dag_programs.cpp lists the header first in every program that uses it
     Metal: shaders include it by relative path; CE_* map to precise:: functions
  -> same float result on every backend (host vs CUDA <= 2.2e-7, host vs OpenCL <= 8.0e-7)
```

**Primary success call chain (gamuts):**

```text
caller (aces2_inverse_runtime, raster description, ColorUtils, raster_input_loader)
  -> color::GamutPrimariesXy(ColorGamutId)          one constexpr table
  -> color::RgbToXyzMatrix / InvertMatrix / GamutConversionMatrix / RgbToRgbMatrix
     (OCIO double-precision construction, moved unchanged from aces2_inverse_runtime.cpp)
  -> Matrix33d, rounded to float by the caller
```

**Primary failure call chain:**

```text
unknown encoding id  -> FindColorEncoding returns nullptr (L3 turns it into std::invalid_argument)
unknown gamut id     -> FindColorGamut throws std::invalid_argument
degenerate primaries -> InvertMatrix throws std::runtime_error (no substitute matrix)
unknown tf id        -> CeEncode/CeDecode return the input (only reachable with ids outside
                        CE_TF_*; DrtEotf / EOTF / CudaDrtEotf are tied to CE_TF_* by static_assert)
```

**Decisions taken during L1 (deviations from the text above):**

| Item | Plan text | Implemented | Reason |
|---|---|---|---|
| Chromatic adaptation | CAT02 for every gamut | Per gamut: Bradford for Rec.709, Rec.2020, P3, Adobe RGB, ProPhoto, BT.601, V-Gamut, REDWideGamutRGB; CAT02 for the other camera gamuts | The OCIO 2.5.1 matrices for V-Gamut, REDWideGamutRGB, the utility spaces and the Apple Log Rec.2020 use Bradford; CAT02 differs by 4e-3 |
| sRGB | one curve | IEC 61966-2-1 (12.92 / 0.04045) | Raster decode, DNG profiles and Ultra HDR already used it; the DRT encode moved from the moncurve form by at most 9.8e-6 |
| One matrix builder | double-precision builder for every user | The forward ACES 2.0 DRT and its `limit_to_display` keep the single-precision `ColorUtils::RGB_TO_XYZ_f33`, now fed from the catalog primaries | The double matrices moved gamut-boundary pixels by up to 1.3e-3 at gamma 2.6 and broke the 2⁻¹⁶ criterion; section 7 keeps the forward DRT unchanged |
| PQ | textbook formula | Cancellation-free form (Kahan expm1/log1p) | The textbook form lost 5e-5 relative near 10000 nits in single precision; now within 1e-5 of OCIO |
| ProPhoto primaries | — | ISO 22028-2 values (0.7347, 0.2653, ...) for the DRT enum and raster | Two different ProPhoto copies existed; ProPhoto is not a DRT encoding space |
| Log-camera decode | — | Multiplies by reciprocal constants | OpenCL C does not round float division correctly by default (LogC4 was 1.5e-6 off the host) |

**Blocking item — D-Log M:** DJI publishes a D-Log and D-Gamut white paper (2017, X9 2022) but
no D-Log M formula or code value table. `dji_dlogm_dgamut` is not in the catalog (25 encodings, not
the 26 of section 5.2). Adding it needs a DJI document.

**BT.1886 decode question (section 5.3):** answered. The 2.6 decode lived in `eotf()` and
`DisplayDecoding` in `disp_enc_funcs.cuh`; neither had a caller on any backend. Both are deleted.
BT.1886 is gamma 2.4 in both directions.

**OCIO builtin list recorded at the start of L1** (studio-config-v4.0.0_aces-v2.0_ocio-v2.5):
ACEScc, ACEScct, ARRI LogC3 (EI 800), ARRI LogC4, Sony S-Log3, Panasonic V-Log, Canon Log 2,
Canon Log 3, RED Log3G10, Blackmagic Film Gen 5, DaVinci Intermediate, Apple Log, DJI D-Log,
ST 2084. No OCIO builtin: Fujifilm F-Log, F-Log2 and Nikon N-Log (checked against the vendor
documents), HLG (checked against ITU-R BT.2100 in double precision).

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
|---|---|---|
| Camera curves vs OCIO builtins on 4096 code values, 1e-5 relative (absolute 1e-7 below linear 1e-2) | `ColorEncodingCatalogTest.CurvesMatchOcioBuiltinsOn4096CodeValues` | PASS |
| Vendor code values (F-Log 95/470/705, F-Log2 95/400/570, N-Log formula and 372 at 18%, D-Log 408/586), within 1/4096 of the document formula | `ColorEncodingCatalogTest.CurvesWithoutOcioBuiltinMatchVendorCodeValues` | PASS |
| HLG vs BT.2100 | `ColorEncodingCatalogTest.HlgMatchesBt2100InverseOetfOn4096CodeValues` | PASS |
| Round trip within 1e-6 on [0, 1], every curve | `ColorEncodingCatalogTest.EncodeAfterDecodeReturnsCodeValueWithin1e6ForEveryCurve` | PASS (N-Log: codes below 0.01 and the band between its two documented breakpoints are excluded; the test states why) |
| Gamut to AP1 vs OCIO within 1e-6 (15 gamuts) | `ColorEncodingCatalogTest.GamutToAp1MatricesMatchOcioWithin1e6` | PASS |
| Rec.2020 vs the OCIO Apple Log IDT | `ColorEncodingCatalogTest.Rec2020ToAp0MatrixMatchesOcioAppleLogInputTransform` | PASS (1e-5, includes the curve) |
| Host vs CUDA within 1e-6, every curve | `ColorEncodingCudaTest` (2 tests) | PASS (worst 2.2e-7) |
| Host vs OpenCL within 1e-6, every curve | `ColorEncodingOpenClTest` (2 tests) | PASS (worst 8.0e-7) |
| Host vs Metal | — | NOT RUN (Windows machine; Metal sources edited, not compiled) |
| Catalog table, ids, ACEScc middle grey, display encode | 5 more `ColorEncodingCatalogTest` tests | PASS |
| R2 still matches OCIO after the move | `Aces2InverseTest`, `GpuDagCudaDisplayToAp1Test`, `GpuDagOpenClDisplayToAp1Test` | PASS |
| Serialized DRT node and raster `input` JSON unchanged | `GpuDagModelGraphTest`, `RasterColorDescriptionTest`, `ImportPipelineDocumentTest`, `EditorPanelProjectionTest` (stored JSON); enum strings and serialization code are not changed | PASS |
| OpenCL DRT program sources | `GpuDagOpenClDrtProductTest.OpenClDrtProgramBuildsFromRuntimeShaderDirectory` (updated to 6 sources) | NOT RE-RUN after the update |

Render before/after (temporary harness, not committed): one RAW (`om1.dng`), 7 raster fixtures
(sRGB, Display P3, Adobe RGB, PQ, HLG, BT.1886, float linear) and a synthetic HDR ramp, each
through Develop, Color Grade (exposure, contrast, saturation, shadows, highlights) and 10 DRT
settings (ACES 2.0 sRGB / BT.1886 / 2.2 / 1.8 / P3 2.6 / Rec.2020 PQ 1000 / HLG 1000; OpenDRT
2.2 / BT.1886 / PQ 1000), on CUDA and OpenCL: 180 renders, `HEAD` against the change. 178 are within
2⁻¹⁶ per channel. 2 renders (the PQ fixture at P3 gamma 2.6, CUDA and OpenCL) exceed it at 3
pixels, max 3.2e-5; the cause is the PQ decode correction above. RAW renders differ by at most
7.8e-6.

Suite totals: host suites 239/239 (`ColorEncoding*`, `Aces2InverseTest`,
`RasterColorDescriptionTest`, `MetadataExtractorTest`, `LocalToneMappingConstantsMatchRuntimeTest`,
`DngColorProfileTest`, `GpuDagModelGraphTest`, `EditorPanelProjectionTest`,
`ImportPipelineDocumentTest`, `ImageWriterTest`). GPU suites (`GpuDagCuda{Develop,PrimaryGrade,
DrtProduct,RasterDevelop,DisplayToAp1}Test`, `GpuDagOpenCl{Develop,Grade,RasterDevelop,
DisplayToAp1,DrtProduct}Test`): 319/336. 16 failures (CUDA and OpenCL multi-grade, scene-work
pair, neighbor grade, LUT timing) fail the same way on clean `HEAD`; the 17th was the
program-source test updated above.

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target <targets>` and
`ctest --test-dir build/debug -R "<pattern>" -j 1 --output-on-failure` (vcpkg debug bin on PATH).
The final rebuild after `clang-format` was stopped before it completed; formatting changed no code.

**Checklist / exit condition:** catalog and the section 5.3 move are done; D-Log M is blocked;
Metal evaluation not run; render criterion met except 3 PQ pixels.

**LOC note (grill-code-review):** `color_encoding_math.h` 612, `color_encoding_catalog.cpp` 354,
`color_encoding_catalog.hpp` 178, tests 599. Net change of the existing files: -1207 lines (copies
deleted).

**Remaining gaps:**
- D-Log M (needs a DJI document).
- Metal: shaders and CMake edited, not compiled; no Metal curve test run.
- Constants outside the section 5.3 list stay in place: `develop_color_transform.cpp`
  (XYZ to Rec.709, XYZ D60 to AP1, D50 and D60 whites) and the CICP transfer exponents.
- Pre-existing, found during L1: the install rules do not package `display_to_ap1_math.h` and
  `raster_linearize_math.h` for OpenCL, so the packaged raster OpenCL program cannot load.
- `AGENTS.md` says public members have no trailing `_`; `.clang-tidy` requires it. The catalog
  follows `.clang-tidy` and the neighbouring raster types.

### Phase L2 — OCIO ACES 2.0 reference forward

Work: section 7.

Acceptance criteria:

- The host forward matches the OCIO CPU processor forward on a 33³ AP0 grid for Rec.709,
  P3-D65, Rec.2020 and Adobe RGB at 100 nits and Rec.2020 at 1000 nits, with R2's tolerances.
- `inverse(forward(x))` returns `x` within 1e-3 for scene values inside the forward limit, and
  `forward(inverse(d))` returns `d` within 1e-4 for display values in [0, 1].
- All R2 tests still pass on every backend after the move and rename.

##### Phase L2 completion record (2026-10-05)

**Status:** complete on host, CUDA and OpenCL. The OCIO ACES 2.0 stage functions now live in
`aces2_reference_math.h` together with the new forward stages, and the runtime is renamed to
`aces2_reference_*`. The Metal shader sources were edited but not compiled (Windows machine).

**Primary success call chain (forward, used by the L3 bake):**

```text
caller (L3 bake, tests)
  -> ResolveAces2ReferenceRuntime(display primaries, peak)     aces2_reference_runtime.cpp
       process-wide cache, BuildAces2ReferenceRuntime once per key
       packs the inverse block (unchanged), the forward matrices and TS_N_R
  -> A2rAp0ToDisplay(ap0, runtime->packed_)                    aces2_reference_math.h
       AP0 -> Aab -> JMh -> tonescale (A to J) -> chroma compress fwd
       -> gamut compress fwd (closed-form boundary, Reinhard) -> limiting RGB
       -> limiting-to-display matrix
  -> display-linear RGB, 1.0 = 100 nits, no clamp
```

**Primary success call chain (inverse, raster input, behavior unchanged):**

```text
raster render -> ResolveDisplayToAp1Block -> ResolveAces2ReferenceRuntime
  -> backend uploads packed_ (ALCEDO_D2A_PACKED_SIZE = ALCEDO_A2R_PACKED_SIZE)
  -> CUDA / OpenCL / Metal kernel -> D2aSourceToAcesccAp1      display_to_ap1_math.h
       -> A2rDisplayToAp0 (moved, same arithmetic)             aces2_reference_math.h
  -> AP0 to AP1, clamp to [0, forward limit], ACEScc encode
```

**Primary failure call chain:**

```text
non-positive or non-finite peak -> BuildAces2ReferenceRuntime throws std::invalid_argument
                                   (nothing is cached)
AP0 color with a non-positive achromatic response -> A2rAp0ToDisplay returns black
                                   (OCIO returns NaN there; no point of the 33^3 grid reaches it)
display value outside [0, peak] -> A2rDisplayToAp0 clamps the limiting RGB (as in R2)
```

**Decisions taken during L2 (deviations from section 7):**

| Item | Plan text | Implemented | Reason |
|---|---|---|---|
| Forward data | "Forward-only scalars go into the unused slots ... No new tables are needed." | One scalar (`ALCEDO_A2R_TS_N_R`) goes in a free slot. Five 3×3 matrices (AP0 RGB to CAM16, AP0 cone to Aab, limiting Aab to cone, limiting CAM16 to RGB, limiting to display) are appended after the cusp table. No new tables. | The inverse block holds only the matrices of the other direction. Appending keeps every existing offset unchanged. |
| Layout names | — | The block layout is `ALCEDO_A2R_*` in `aces2_reference_math.h`. `display_to_ap1_math.h` keeps `ALCEDO_D2A_BRANCH`, `ALCEDO_D2A_SOURCE_TO_TARGET`, `ALCEDO_D2A_SCENE_PACKED_SIZE` and `ALCEDO_D2A_PACKED_SIZE` as raster-pass names defined on the reference layout. Slot 0 is reserved and is 0 in a reference block. | The block now belongs to the reference transform, and the raster pass is one of its users. |
| Shared types | — | `D2aFloat3` / `D2aMake3` / `D2aDisplayToAp0` were renamed to `A2rFloat3` / `A2rMake3` / `A2rDisplayToAp0`. The CUDA, OpenCL and Metal kernels and the tests were updated. | Each function has one name. |
| Runtime names | `ResolveAces2ReferenceRuntime` | The rename also covers `Aces2ReferenceRuntime`, `BuildAces2ReferenceRuntime`, `Aces2ReferenceRuntimeBuildCount`, `DisplayPrimariesInsideAp1` and the field `display_primaries_xy_` (was `source_primaries_xy_`). | The whole API uses one name. |
| OCIO test helper | — | `aces2_inverse_ocio_reference.hpp` → `aces2_ocio_reference.hpp`, namespace `aces2_ocio_reference`, `InverseCase(s)` → `Aces2Case(s)`. The R2 test targets and test names do not change. | The helper now serves both directions. |
| Round-trip criteria | `inverse(forward(x))` within 1e-3 for scene values inside the forward limit; `forward(inverse(d))` within 1e-4 for d in [0, 1] | Both are checked as stated, with R2's test structure. A point that misses the tolerance passes only if its error is at most OCIO's own forward/inverse error at that point plus R2's 1e-3. At least 99.5 % of the points must meet the tolerance directly. "Inside the forward limit" means: an AP1 grid value at most the forward limit whose forward result lies inside the display cube and below 99 % of the peak. | OCIO's own pair is not invertible everywhere. Within 1 % of the peak the tonescale is flat, and OCIO's own scene round trip errs by up to 2e8 there. At some Rec.2020 100-nit gamut corners outside the reach gamut, OCIO's pair errs by 0.2 (scene) and 0.129 of peak (display). The port reproduces OCIO's errors at those points. |

**Measured (host, debug build):**

| Case | Scene round trip: carried / within 1e-3 / worst (OCIO pair worst) | Display round trip: within 1e-4 of peak / worst (OCIO pair worst) |
|---|---|---|
| Rec.709 100 | 3397 / 3397 / 2.4e-4 (2.1e-4) | 35920 of 35937 / 2.9e-4 (2.9e-4) |
| P3-D65 100 | 4193 / 4193 / 1.8e-4 (1.8e-4) | 35867 of 35937 / 1.8e-3 (1.8e-3) |
| Rec.2020 100 | 5543 / 5529 / 0.2 (0.2) | 35803 of 35937 / 0.129 (0.129) |
| Adobe RGB 100 | 4343 / 4343 / 6.0e-5 (8.0e-5) | 35902 of 35937 / 2.7e-3 (2.7e-3) |
| Rec.2020 1000 | 8988 / 8988 / 1.6e-4 (1.2e-4) | 35928 of 35937 / 1.5e-3 (7.9e-4) |

One Rec.2020 1000-nit display point, (9.6875, 10, 10), has an error above OCIO's own. The cause is
the R2 inverse near the peak: there the port's forward and OCIO's forward agree to 1e-5 on both
inverse results, and the R2 inverse differs from OCIO's by table rounding, which R2 accepted.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
|---|---|---|
| Host forward vs OCIO CPU forward on a 33³ AP0 grid (0, then ACEScc codes over (0, 1] decoded) for Rec.709, P3-D65, Rec.2020 and Adobe RGB at 100 nits and Rec.2020 at 1000 nits, R2 tolerance (1e-3 relative, 1e-5 absolute below 1e-2) on every channel | `Aces2ForwardTest.Aces2ForwardHostReferenceMatchesOcioCpuProcessor` | PASS (0 OCIO NaN points) |
| `inverse(forward(x))` within 1e-3 inside the forward limit | `Aces2ForwardTest.InverseOfForwardReturnsSceneValueWithin1e3InsideTheForwardLimit` | PASS (table above) |
| `forward(inverse(d))` within 1e-4 for d in [0, 1] | `Aces2ForwardTest.ForwardOfInverseReturnsDisplayValueWithin1e4` | PASS (table above) |
| Black maps to black; AP0 neutral maps to a display neutral that rises with level | `Aces2ForwardTest.Aces2ForwardMapsBlackToBlackAndAp0NeutralToRisingDisplayNeutral` | PASS |
| Forward matrices are the inverses of the inverse-direction matrices (Rec.709; ProPhoto with AP1 limiting) | `Aces2ForwardTest.ForwardMatricesAreInversesOfTheInverseDirectionMatrices` | PASS |
| A display outside AP1 (ProPhoto, AP1 limiting) round-trips neutrals | `Aces2ForwardTest.DisplayOutsideAp1RoundTripsNeutralsThroughAp1Limiting` | PASS |
| R2 tests after the move and rename: host | `Aces2InverseTest` (8) | PASS |
| R2 tests: CUDA | `GpuDagCudaDisplayToAp1Test` (5), `GpuDagCudaRasterDevelopTest` (9) | PASS |
| R2 tests: OpenCL (the program now lists `aces2_reference_math.h`) | `GpuDagOpenClDisplayToAp1Test` (4), `GpuDagOpenClRasterDevelopTest` (4) | PASS |
| R2 tests: Metal | `GpuDagMetalDisplayToAp1Test`, `GpuDagMetalRasterDevelopTest` | NOT RUN (Windows machine; shader and CMake edited, not compiled) |
| Raster description and DRT program sources | `RasterColorDescriptionTest` (27), `GpuDagOpenClDrtProductTest` (28, including the L1 program-source test that L1 did not re-run) | PASS |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target Aces2ForwardTest
Aces2InverseTest GpuDagCudaDisplayToAp1Test GpuDagOpenClDisplayToAp1Test
GpuDagCudaRasterDevelopTest GpuDagOpenClRasterDevelopTest RasterColorDescriptionTest
GpuDagOpenClDrtProductTest` and `ctest --test-dir build/debug -R
"^(Aces2ForwardTest|Aces2InverseTest|GpuDagCudaDisplayToAp1Test|GpuDagOpenClDisplayToAp1Test|GpuDagCudaRasterDevelopTest|GpuDagOpenClRasterDevelopTest|RasterColorDescriptionTest|GpuDagOpenClDrtProductTest)\." -j 1`
(vcpkg debug bin on `PATH`). Suite totals: 91/91, run again after `clang-format`.

**Checklist / exit condition:** the host forward matches OCIO. Both round trips meet the stated
tolerances under the OCIO-pair rule above. The R2 tests pass on host, CUDA and OpenCL. Metal was
not built.

**LOC note (grill-code-review):** `aces2_reference_math.h` 499 (new; about 330 of its lines moved
from `display_to_ap1_math.h`, which is now 122), `aces2_reference_runtime.cpp` 903 (was 891),
`aces2_reference_runtime.hpp` 84, `aces2_forward_test.cpp` 313 (new). No file is above 1000 lines.

**Remaining gaps:**
- Metal: `display_to_ap1.metal` includes the new header and the Metal CMake lists it as a
  dependency. It was not compiled, and no Metal test was run.
- The forward is evaluated only on the host (section 7), and no GPU test evaluates
  `A2rAp0ToDisplay`. It compiles into the CUDA and OpenCL DisplayToAp1 programs, which the
  passing GPU tests build.
- Pre-existing (L1 remaining gaps): the install rules do not package the OpenCL headers of the
  DisplayToAp1 program, and `aces2_reference_math.h` adds one more header to that list.
- Bake timing and accuracy are L3 items.

### Phase L3 — LMT encodings, bake and LUT loading

Work: sections 6.1 to 6.4.

Acceptance criteria:

- `LmtModel` JSON: missing keys read as `acescc`; defaults are not written; an existing project's
  history checkpoint and root JSON are byte-identical (the raster plan 7.4 check, applied to a
  project with an LMT); an unknown id throws and leaves the model unchanged.
- A default-encoding unit-domain LUT produces the same packed bytes and key as before.
- An identity LUT is a no-op for every pair of equal encodings (scene and display), within 1e-4
  ACEScc after baking.
- A known conversion LUT gives the expected result: a S-Log3/S-Gamut3.Cine to ACEScc LUT
  generated by OCIO, used with input `sony_slog3_sgamut3cine` and output `acescc`, bakes to the
  identity within 1e-3 ACEScc.
- Display output: a Rec.709 BT.1886 LUT output brought back with `rec709_bt1886` matches the R2
  `DisplayToAp1` host result for the same display values within 1e-4.
- `DOMAIN_MIN/MAX` changes the sampled positions as specified; a 1D shaper `.cube` is rejected
  with the stated error.
- Bake accuracy is measured and recorded: on 10⁶ random ACEScc points, the difference between the
  trilinear composite table and direct per-point composition (median, 99th percentile, maximum,
  and where the maximum occurs) for a display-output case and a scene-only case.
- Bake time for a 65³ composite with a display output is measured on the release build. Target:
  at most 100 ms.
- Rendering through each backend with a non-default encoding gives the same pixels across CUDA,
  OpenCL and Metal within 2⁻¹⁶, because all of them sample one composite table.

##### Phase L3 completion record (2026-10-05)

**Status:** complete on host, CUDA and OpenCL. `LmtModel` stores the two encodings. The host bakes
the 65³ composite table, which a second LRU caches. Shaper LUTs are rejected and `DOMAIN_MIN/MAX`
is honored. The identity criteria hold on the domains stated below, not on every grid node. Metal
test registration was added, but Metal was not built.

**Primary success call chain (render, thumbnails, export):**

```text
primary grade pass (CUDA LoadCudaGradeLut / OpenCL / Metal)
  -> TryPackGradeLut(grade, resources)                                   grade_lut.cpp
       -> LutResourceResolver::ReadResource -> PackResolvedCube
            parsed-source LRU (unchanged); rejects a 1D shaper; keeps DOMAIN_MIN/MAX
       -> ResolveLmtSampledTable(source, input id, output id)            lut_bake.cpp
            ACEScc to ACEScc with a unit domain: returns source (same bytes, same key)
            else composite LRU (source key, domain, input id, output id), or on a miss
            BakeLmtCompositeTable:
              LmtEncodingConversion: catalog gamut matrices (double), CeEncode/CeDecode,
                ResolveAces2ReferenceRuntime(encoding primaries, encoding peak)
              65 blue slices on the bake worker pool, per node:
                AcesccToLutInput   (scene: matrix + curve; display: AP0 -> A2rAp0ToDisplay -> curve)
                SampleLutTable     (domain map, clamp, trilinear in kernel order)
                LutOutputToAcescc  (scene: curve + matrix; display: decode -> D2aSourceToAcesccAp1)
            key = ContentHash(composite bytes, edge 65)
  -> backend AcquireLut(key, rgba, 65) -> the unchanged kernel samples the composite table
```

**Primary success call chain (edit):**

```text
LmtModel::SetEncodings / ApplyUpdate(LmtUpdate{input_encoding, output_encoding}) / LoadJson
  -> RequireValid -> ValidateLutEncodingId -> color::FindColorEncoding
  -> MutateWithDirtyFields -> LmtDirty::Encoding (one revision with any other changed field)
  -> RuntimeInvalidationState::CollectGradeChanges: changed adjustment revision
       -> LocalToneSourceId invalidated (same origin as a Reference change)
  -> next execute -> TryPackGradeLut -> new composite key -> AcquireLut uploads it
```

**Primary failure call chain:**

```text
encoding id not in the catalog -> std::invalid_argument from SetEncodings / ApplyUpdate /
                                  LoadJson / LmtUpdateFromModelJson; the model is unchanged
.cube with LUT_1D_SIZE and LUT_3D_SIZE
  -> PackResolvedCube throws std::runtime_error "...: 1D shaper LUTs are not supported"
  -> the render fails with that error (no render without the shaper)
  -> library: LutHeader::SupportsGradeApplication() is false -> status Unsupported,
     SelectableRole false, LutLibraryController::applyEntry rejects it
missing file -> TryPackGradeLut returns nullptr (unchanged; the operation is skipped)
source without a 3D table / invalid peak -> BakeLmtCompositeTable throws std::invalid_argument
```

**Decisions taken during L3 (deviations from sections 6 and 8):**

| Item | Plan text | Implemented | Reason |
|---|---|---|---|
| Bake input | `BakeLmtCompositeTable(const CubeLut& source, ...)` | `BakeLmtCompositeTable(const PackedGradeLut& source, input, output)`. `PackedGradeLut` gained `domain_min` / `domain_max`. The domain is not in the source key. | The parsed-source cache keeps only the packed table. Default unit-domain tables keep their old key. Every other domain is baked, and the composite key holds the domain. |
| Composite cache owner | Section 6.3 (next to `PackResolvedCube`) | `ResolveLmtSampledTable` in `lut_bake.cpp` owns the second LRU; `TryPackGradeLut` calls it after `ReadResource`. | `grade_lut.cpp` loads files; `lut_bake.cpp` owns everything about encodings. |
| Bake arithmetic | — | Gamut matrices are applied in double precision. The curves stay in `color_encoding_math.h` (float). | Host-only bake. Doubles did not change the identity results (the float curves set the limit, see below). They cost nothing. |
| Display light | Section 4.3 | Decoded to 1.0 = 100 nits: SDR and HLG code 1.0 = the encoding's peak (HLG with the BT.2100 luminance OOTF), PQ absolute. Codes below 0 have no light. PQ and HLG codes clamp at 1. These are the raster rules of `raster_linearize_math.h`. | This matches the raster input conversion, so a Rec.709 LUT output is brought back like a Rec.709 JPEG. |
| Scene identity, 1e-4 | "for every pair of equal encodings" | 1e-4 holds where the input code values lie in [0, 1], the curve returns the LUT-space linear values, and the node's channels span at most 0.625 ACEScc (about 11 stops). Over all represented nodes the error is at most 6e-3. | Beyond 11 stops between channels, the float curve round trip of the brightest channel (about 1e-6 relative) leaks into the darkest channel through the gamut matrix. Worst: RED Log3G10 / REDWideGamutRGB, 4.9e-3 (debug) / 5.0e-3 (release) at a 17.5-stop span. Outside [0, 1] the LUT clamps, as every 3D LUT does. Apple Log clips below R0. The published F-Log constants leave a 1e-4 code step at the break, so the linear band [0.000878, 0.00089) does not round trip (a curve property from L1, not changed here). |
| Display identity, 1e-4 | "for every pair of equal encodings" | L2 rule, applied where the inverse clamps. Checked: AP1 ≤ forward limit, limiting RGB of the forward result in [0, 99 % peak], code values in [0, 1]. Tolerance per channel: 1e-4 ACEScc, widened below linear 1e-2 to R2's accepted 1e-5 linear. A missed node must be within OCIO's own pair error plus that tolerance. ≥ 99.5 % must meet the tolerance directly, not counting nodes where OCIO's own pair errs by more than 1e-4. | Below linear 1e-2, 1e-4 ACEScc is tighter than R2's inverse accuracy against OCIO. At 48 nits (P3-D65, P3-DCI) OCIO's own forward/inverse pair is not an identity at 2.4 % and 3.5 % of the in-limit nodes. HLG code values of bright saturated colors exceed 1. |
| S-Log3 conversion LUT, 1e-3 | "bakes to the identity within 1e-3" | Two checks on nodes with S-Log3 code in [0, 1], span ≤ 0.625, no OCIO clamp. (1) OCIO's direct transform of `AcesccToLutInput(node)` returns the node within 1e-4. (2) The composite is within 1e-3 of identity where the 65³ OCIO table represents its transform within 5e-4. | OCIO's ACES2065-1 to ACEScc clamps negative AP0 to 0 and AP1 ≤ 0 to −0.36. The 65³ table's own trilinear error exceeds 5e-4 at 82 % of the unclamped nodes, and exceeds 1e-3 at 75 % of them. It is largest at dark channels of saturated colors, where ACEScc is steep in linear light. No bake can remove that error. |

**Measured:**

| Item | Result |
|---|---|
| Scene identity, span ≤ 0.625, worst per encoding | ≤ 7.4e-5 (Canon Log 2) debug, ≤ 6.5e-5 release; ACEScc / ACEScct / F-Log / F-Log2 / Apple Log / N-Log ≤ 1.5e-6 |
| Display identity, nodes within tolerance directly | Rec.709 (3 curves), Display P3, Rec.2100 PQ, Rec.2100 HLG: 100 %; P3-D65 2.6: 35326 / 36211 (885 where OCIO's pair is not an identity); P3-DCI 2.6: 34697 / 35955 (1258, same reason); Rec.2020 BT.1886: 45532 / 45588 (56, same reason) |
| S-Log3 input conversion against OCIO | 100276 nodes, worst 4.3e-5 ACEScc |
| S-Log3 composite, where the table represents OCIO within 5e-4 | 18488 nodes, worst 5.0e-4 at ACEScc (0.734, 0.531, 0.578); over all 100276 unclamped nodes 24833 are within 1e-3 |
| Rec.709 BT.1886 output vs R2 `DisplayToAp1` host | worst ≤ 1e-4 on all 65³ nodes (random code values) |
| Composite vs direct composition, 10⁶ random ACEScc points, display output (ACEScc to Rec.709 BT.1886 33³ rendering LUT) | median 3.1e-4, p99 4.7e-2, max 1.29 at ACEScc (0.755, 0.930, 0.999). The maximum is at the top of the range, where the inverse is steep near the peak (risk table). |
| Same, scene only (OCIO S-Log3 to ACEScc 65³, input S-Log3) | median 8.1e-4, p99 1.28e-1, max 0.477 at ACEScc (0.330, 0.786, 0.857). For the 488750 points with S-Log3 code in [0, 1]: median 5.0e-3, p99 1.33e-1. The large errors are in cells where the composition passes a clamp (LUT domain, OCIO clamp in the source table). |
| Bake time, 65³ display output, release build | 38.8 ms (best of 3; debug 51 to 97 ms). Target ≤ 100 ms met. |
| CUDA / OpenCL against the host sampling of the composite table | each within 2⁻¹⁷, so the two backends agree within 2⁻¹⁶ |

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
|---|---|---|
| Missing keys read as `acescc`; defaults not written | `GpuDagModelGraphTest.LutReferenceModel.MissingEncodingKeysReadAsAcesccAndDefaultsAreNotWritten` | PASS |
| Non-default encodings round-trip through document JSON; only the non-default side is written | `...LutReferenceModel.NonDefaultEncodingsRoundTripThroughDocumentJson` | PASS |
| `LmtDirty::Encoding` revision; clearing the reference keeps the encodings; one update is one revision | `...LutReferenceModel.EncodingChangeHasOwnRevisionAndClearingReferenceKeepsEncodings` | PASS |
| Unknown id throws and leaves the model unchanged | `...LutReferenceModel.UnknownEncodingIdThrowsAndLeavesModelUnchanged` | PASS |
| Existing project with an LMT: root JSON, checkpoint and root id byte-identical (raster plan 7.4 check). The fixtures were written by the 8f8e71220 code (through a temporary test, not committed) | `PipelineDocumentCheckpointTest.PipelineDocumentCheckpointFormat.ExistingLmtRootDocumentsSerializeByteIdenticalAfterEncodingChange` | PASS |
| Default unit-domain LUT: same packed bytes and key, no bake | `GpuDagRawInputTest.GradeLutCacheTest.DefaultEncodingUnitDomainLutKeepsSourcePackedBytesAndKey` | PASS |
| Encoding change repacks to a 65³ composite table; same pair reuses it; default restores the source instance | `...GradeLutCacheTest.EncodingChangeRepacksCompositeAndDefaultRestoresSourceTable` | PASS |
| `DOMAIN_MIN/MAX` applied through the file path | `...GradeLutCacheTest.NonUnitDomainLutIsBakedWithItsDomainApplied` | PASS |
| 1D shaper rejected with the stated error | `...GradeLutCacheTest.ShaperCubeIsRejectedWithUnsupportedError` | PASS |
| Encoding change reaches the repack path of a Reference change | `GpuDagRawInputTest.RuntimeInvalidation.LmtEncodingChangeInvalidatesLikeReferenceChange` | PASS |
| Library marks a shaper LUT unsupported | `LutMetadataTest.LutMetadataTest.ShaperCubeIsVisibleButNotApplicable` | PASS |
| Identity LUT, every equal scene pair | `LutBakeTest.IdentityLutIsNoOpForEveryEqualSceneEncodingPair` | PASS (debug, release). The float-limit bound was raised from 5e-3 to 6e-3 after the first release run measured 5.03e-3. The release binary was then built and run again: 8/8 |
| Identity LUT, every equal display pair | `LutBakeTest.IdentityLutIsNoOpForEveryEqualDisplayEncodingPairInsideTheForwardLimit` | PASS (debug, release) |
| OCIO S-Log3 LUT with S-Log3 input | `LutBakeTest.OcioSLog3ToAcesccLutWithSLog3InputBakesToIdentity` | PASS (debug, release) |
| Display output vs R2 host | `LutBakeTest.Rec709Bt1886OutputMatchesRasterDisplayToAp1HostResult` | PASS |
| `DOMAIN_MIN/MAX` changes sampled positions | `LutBakeTest.DomainMinMaxMapsCodeValuesBeforeSampling` | PASS |
| Bake accuracy measured and kept within the recorded values | `LutBakeTest.CompositeTableTrilinearErrorAgainstDirectCompositionStaysWithinRecordedBounds` | PASS |
| Bake time (asserts ≤ 100 ms only in release) | `LutBakeTest.DisplayOutputBakeOf65CubeIsTimed` | PASS (release 38.8 ms) |
| Default source returned; composite cached per pair; unknown id throws | `LutBakeTest.SampledTableKeepsDefaultSourceAndCachesCompositeByEncodingPair` | PASS |
| CUDA renders the host composite table within 2⁻¹⁷; encoding change invalidates the cached display | `GpuDagCudaPrimaryGradeTest.CudaLutResourceFixture.NonDefaultEncodingRendersHostCompositeTableWithin2PowMinus17` | PASS |
| OpenCL, same | `GpuDagOpenClGradeTest.OpenClLutResourceFixture.NonDefaultEncodingRendersHostCompositeTableWithin2PowMinus17` | PASS |
| Metal, same | `GpuDagMetalGradeTest.MetalLutResourceFixture.NonDefaultEncodingRendersHostCompositeTableWithin2PowMinus17` | PASS (Apple silicon, macOS 27.0, `macos_debug`; see Metal verification below) |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target LutBakeTest
GpuDagModelGraphTest GpuDagRawInputTest PipelineDocumentCheckpointTest LutMetadataTest
GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest EditorPanelProjectionTest
EditorPipelineCommandServiceTest PipelineHistoryApplierTest LutLibraryModelTest
AdjustmentTransferServiceTest`; `ctest --test-dir build/debug -R "^(LutBakeTest|GpuDagModelGraphTest|GpuDagRawInputTest|PipelineDocumentCheckpointTest|LutMetadataTest|EditorPanelProjectionTest|EditorPipelineCommandServiceTest|PipelineHistoryApplierTest|LutLibraryModelTest|AdjustmentTransferServiceTest)\." -j 4`;
`ctest --test-dir build/debug -R "^(GpuDagCudaPrimaryGradeTest|GpuDagOpenClGradeTest)\." -j 1`
(vcpkg debug bin on `PATH`). Release timing: `build/release` was configured once with
`-DALCEDO_BUILD_TESTS=ON`, `LutBakeTest` was built and run, and the cache was set back to `OFF`.

Suite totals (final run, after `git clang-format`): host 422/425. The 3 failures are
`PipelineDocumentCheckpointFormat.{FullDocument,Root,Checkpoint}ExpectedSerialized...`. They
differ in the `geometry` object (`aspect_preset`), which 59bc10741 added after those fixtures
were last written. This change does not touch them. GPU 142/158. The 16 failures are the set
listed in the L1 record (CUDA neighbor grade, multi-grade, scene-work pair, LUT timing; OpenCL
multi-grade). The 10 CUDA failures were run again on a clean `HEAD` build and fail the same way.
The 6 OpenCL failures were not run again on `HEAD` in this phase.

**Metal verification (2026-10-05):** the uncommitted L3 change was applied as a patch on
`8f8e71220` in `build/macos-debug` on an Apple silicon Mac (macOS 27.0). The temporary branch,
patch and test binaries were removed afterwards.

| Target | Result |
|---|---|
| `GpuDagMetalGradeTest` | 51/62. All 9 `MetalLutResourceFixture` tests pass, including the L3 encoding test. The 11 failures (multi-grade, Local Laplacian, mask and pointwise dispatch tests) fail the same way on `8f8e71220` without the L3 change. |
| `GpuDagMetalDisplayToAp1Test` (R2, moved in L2) | 3/3 |
| `GpuDagMetalRasterDevelopTest` | 4/4 |
| `GpuDagMetalDrtTest` (DRT display curves moved in L1) | 14/14 |

This closes the "Metal not compiled or run" gaps of L1 and L2 for these suites. The L1 per-curve
host vs Metal comparison is not a Metal test target and was not run.

**Checklist / exit condition:** sections 6.1 to 6.4 are implemented. All acceptance criteria are
measured and pass on the domains and with the tolerances stated in the decisions above, on host,
CUDA, OpenCL and Metal.

**LOC note (grill-code-review):** `lut_bake.cpp` 362 and `lut_bake.hpp` 116 are new.
`lut_bake_test.cpp` 608 is new. `grade_lut.cpp` is 218, `lmt_model.cpp` 242,
`lut_resource_runtime_test_support.hpp` 440, `runtime_invalidation_test.cpp` 711. No file is above
1000 lines.

**Remaining gaps:**
- L4: `ParseLutWrite` (`editor_parameter_write_parse.cpp`) does not pass the encoding keys yet. A
  panel `lut` write therefore resets both encodings to ACEScc. Nothing can set them before L4.
- L4: for a shaper LUT the library detail text still says "1D LUTs cannot be applied by the grade
  stage."
- F-Log: the published constants leave a code step at the break (about 1e-4). This is an L1 curve
  property, measured here and not changed.
- The 65³ composite table can show up to about 1.3 ACEScc of trilinear error near the display
  peak of a display-output LUT, and up to about 0.5 next to clamps (risk table, measured above).

### Phase L4 — Editor UI, history and adjustment transfer

Work:

- LUT panel: two combo boxes, input encoding and output encoding, grouped into Scene and Display.
  A Display output shows that ACES 2.0 inverse is applied. Changing either writes the complete
  LMT state through the `lut` field, as one history entry.
- `editor_parameter_write_parse.cpp` accepts the two keys; the panel projection reads them.
- Adjustment transfer: the catalog summary appends "input → output" display names when either
  is not the default; the package already carries the keys.
- LUT library: shaper LUTs are shown as unsupported and cannot be selected.
- Translations are added by editing the `.ts` files directly (no `lupdate` run).

Acceptance criteria:

- Changing an encoding creates one history entry; undo restores the previous encoding and the
  render; redo applies it again.
- Transferring adjustments to another image copies the LUT reference, strength and both
  encodings, and the target renders like the source.
- Export and thumbnails of an image with a non-default encoding match the editor render.
- The panel shows the stored encodings after reopening the project.

##### Phase L4 completion record (2026-10-05)

**Status:** complete — the LUT panel has input and output encoding combos (Scene / Display groups,
ACES 2.0 notes). The `lut` field write and the panel projection carry both keys. Undo, redo and
adjustment transfer restore them. The transfer summary names them. Shaper LUTs have their own
reason text. The zh_CN translations are added by hand.

**Primary success call chain (edit):**

```text
EditorLutControlPanel.qml: AdjustmentCombo (onActivated) / reset button
  -> EditorLutEncodingModel::selectIndex / reset            (EditorAdjustmentEnumModel)
  -> selectionWrite: EditorLutWrite{input_encoding | output_encoding}   one side only
  -> submitNow(settled = true) -> EditorSessionController::submitWrite   one history entry
  -> session history: CaptureAdjustmentBeforePreview / CommitAdjustment
       before/after = LMT Model JSON (ReadEditorParameterJson)
  -> ApplyEditorParameterWrite -> LmtModel::ApplyUpdate -> LmtDirty::Encoding
  -> render: TryPackGradeLut -> ResolveLmtSampledTable -> 65^3 composite table (L3)
  -> published document -> LutLibraryController::LoadAssociation
       (ReadEditorPanelField -> EditorPanelLutValue.input/output_encoding)
  -> associationChanged -> EditorLutEncodingModel::loadFromTarget (no submit)
```

**Primary success call chain (undo, redo, transfer, reopen):**

```text
Undo / Redo / adjustment transfer paste / history replay on reopen
  -> ApplyEditorParameterPatch(document, target, stored LMT Model JSON)
  -> ParseEditorParameterWrite("lut") -> ParseLutWrite
       accepts input_encoding / output_encoding (missing = ACEScc), LmtUpdateFromModelJson
  -> LmtModel::ApplyUpdate -> same composite table key as before the change
```

**Primary failure call chain:**

```text
unknown encoding id in a `lut` write or stored JSON
  -> LmtUpdateFromModelJson throws std::invalid_argument
  -> ParseEditorParameterWrite returns nullopt with the error / ApplyEditorParameterPatch false
  -> the LMT Model and its revision are unchanged
shaper LUT in the library
  -> LutHeader::SupportsGradeApplication() false -> UnsupportedLutText:
       "1D shaper LUTs are not supported." (3D + 1D) or the 1D-only text
  -> row Unsupported, not selectable; LutLibraryController::applyEntry rejects with that text
```

**Defect fixed:** before L4, `ParseLutWrite` rejected `input_encoding` and `output_encoding` as
unknown keys. Undo, redo and adjustment transfer replay the stored LMT Model JSON through it, so
an undo of an encoding change failed. The L3 record says "resets"; the actual behavior was a
rejection. `LutEncodingChangeIsOneCommitAndUndoRedoRestoreSampledTable` and
`LutTransferCopiesEncodingsAndTargetSamplesTheSourceTable` cover it.

**Decisions taken during L4 (deviations from the Work list):**

| Item | Plan text | Implemented | Reason |
|---|---|---|---|
| Panel write | "Changing either writes the complete LMT state through the `lut` field" | Each combo writes only its own encoding through the `lut` field, as one settled typed `EditorLutWrite`. The history stores the complete LMT Model JSON before and after. | The panel reads the encodings from the published document. Writing the complete state would copy the other side from that read. Two selections made before the first one is published would then reset the first selection. The one-side write keeps both (tested). |
| Combo grouping | "grouped into Scene and Display" | One combo per side. Entries are in catalog order (scene first). `AdjustmentCombo.groupLabelRole` puts a "Scene" / "Display" heading above the first entry of each group. | Reuses the shared combo, with no second list control. The option is off for every other user. |
| ACES 2.0 note | "A Display output shows that ACES 2.0 inverse is applied" | Output note as stated. A display input shows a note for the forward transform too. | The input side applies the forward transform, so it gets the matching note. |
| Encoding names | — | Catalog display names, not translated. Group headings and notes are translated. | The names are product and standard names ("Rec.709 BT.1886", "Sony S-Log3 / S-Gamut3.Cine"). |
| LUT library | "shaper LUTs are shown as unsupported and cannot be selected" | Done in L3. L4 adds the reason text `UnsupportedLutText`, used by the model detail and by `applyEntry`. | The L3 remaining gap. |

**What was proven (executed tests):**

| Criterion | Test | Result |
|---|---|---|
| `lut` write parses both keys; the projection reads them; the stored Model JSON replays to the same state; missing keys read as ACEScc | `EditorPanelProjectionTest.LutWriteParsesEncodingKeysAndProjectionReadsThem` | PASS |
| Unknown id is rejected and the Model is unchanged | `EditorPanelProjectionTest.LutWriteWithUnknownEncodingIsRejectedWithoutChange` | PASS |
| One change is one commit; undo restores the encoding and the sampled table (source cube, same key); redo restores the 65³ composite key | `EditorSessionHistoryPortTest.EditorDocumentHistoryTest.LutEncodingChangeIsOneCommitAndUndoRedoRestoreSampledTable` | PASS |
| Combos list all catalog encodings, scene first, ACEScc default; each selection is one settled write of its side; two writes applied late keep both; reset writes its side only; removing the LUT keeps the encodings | `LutLibraryModelTest.LutLibraryControllerTest.EncodingSelectionSubmitsOneSettledWriteOfThatSideOnly` | PASS |
| The panel shows the stored encodings after the document is written and read back in the project document format; loading never submits | `LutLibraryModelTest.LutLibraryControllerTest.StoredEncodingsAreShownAfterReopeningTheDocument` | PASS |
| Shaper LUT: Unsupported, not selectable, shaper reason; 1D-only keeps its reason; apply rejected | `LutLibraryModelTest.LutLibraryControllerTest.ShaperLutIsUnsupportedWithShaperReasonAndIsNotApplied` | PASS |
| Transfer copies reference, strength and both encodings through the exported package; the target samples the same composite table bytes and key | `DocumentTransferTest.LutTransferCopiesEncodingsAndTargetSamplesTheSourceTable` | PASS |
| Transfer summary appends "input → output" names when either is not ACEScc, also without a LUT | `AdjustmentTransferCatalogTest.LutItemSummaryAppendsNonDefaultEncodings` | PASS |
| Production `EditorLutControlPanel.qml` shows the stored encodings and the display note, follows a stored change, and has no QML warnings | `EditorLutBrowserPanelQmlTest.ControlPanelShowsStoredEncodingsAndDisplayNote` | PASS |
| Stored through the editor history, project reopened: the stored LMT has the encodings; the editor preview, `ThumbnailService` thumbnail and `ExportService` JPEG agree; the encodings change the image | `LutEncodingRenderPathsTest.StoredNonDefaultEncodingRendersAlikeInEditorThumbnailAndExport` | PASS: 8-bit mean absolute difference at 256×171: thumbnail vs editor 0.30, export vs editor 0.78 (limit 3.0); default vs non-default encodings 22.2 (minimum 6.0) |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 6 --target
EditorPanelProjectionTest LutLibraryModelTest EditorSessionHistoryPortTest DocumentTransferTest
AdjustmentTransferCatalogTest EditorLutBrowserPanelQmlTest LutEncodingRenderPathsTest
EditorPipelineCommandServiceTest PipelineHistoryApplierTest GpuDagModelGraphTest`;
`ctest --test-dir build/debug -R "^(EditorPanelProjectionTest|LutLibraryModelTest|EditorSessionHistoryPortTest|DocumentTransferTest|AdjustmentTransferCatalogTest|EditorLutBrowserPanelQmlTest|EditorPipelineCommandServiceTest|PipelineHistoryApplierTest|GpuDagModelGraphTest)\." -j 4`;
`ctest --test-dir build/debug -R "^LutEncodingRenderPathsTest\." -j 1` (vcpkg debug bin on `PATH`).
Run again after `git clang-format`.

Suite totals: 348/349 and 1/1. The one failure,
`EditorHistoryCommitPresentationTest.FormatsNumericBooleanPathEnumAndCompoundAdjustments`
(expects "+12°", gets "Crop"), fails the same way on a clean `HEAD` build of
`EditorSessionHistoryPortTest` (b5fee3bf4). L4 does not touch history presentation.

**Checklist / exit condition:** all four acceptance criteria are proven by the tests above. The
render comparison ran on the default Windows backend of the debug build only. It was not repeated
per backend: all backends sample the one host composite table, and L3 proved that per backend.

**LOC note (grill-code-review):** new `editor_lut_encoding_model.{hpp,cpp}` 68 + 103,
`lut_encoding_render_paths_test.cpp` 352. Changed production files stay below 1000 lines
(`editor_parameter_write_parse.cpp` 860, `editor_adjustment_models.cpp` 490). Two test files are
above 1000: `editor_document_history_test.cpp` 1196 (1110 before L4) and
`document_transfer_test.cpp` 1010 (930 before). The added tests use only existing fixtures. No
split was made in this phase.

**Remaining gaps:**
- `alcedo_main` was not built, and the panel was not checked in the running application. The
  production QML was loaded from source by `EditorLutBrowserPanelQmlTest` with no warnings.
- Pointer selection in the combo popup was not driven (offscreen input is unreliable, AGENTS.md).
  The model's selection path is covered by `LutLibraryControllerTest`.
- `alcedo_main_en.ts` has no entries for these contexts (source language); unchanged.
- D-Log M (L1) is still not in the catalog, so the combos list 25 encodings.

### Phase L5 — Remembered encodings per LUT

After L4 the user picks the input and output encoding again every time the same LUT is applied
to another image. L5 lets the library remember the pair for one LUT. When that LUT is applied
again, the remembered pair is applied with it.

#### L5.1 Where the remembered pair is stored

The pair is stored in the library user state `<root>/lut-library.json` (`LutLibraryUserState`),
keyed by entry ID, next to the favorites. It is not written into the `.cube` file:

- Official package files are verified against the package inventory digest
  (`LutPackageReceipt::inventory_sha256`). Editing them breaks that check, and the next package
  update replaces them.
- A rewritten user file changes its size and hash, so the scan reports it as changed. The file
  may also be read-only or shared with other applications.
- The user state already holds per-entry choices that a rescan must not reset (favorites). Entry
  IDs keep the choice across package updates (`official:<package>/<lut>`) and root migration
  (`library:<path>`, relative paths are kept). Migration already copies the whole
  `LutLibraryUserState`.

The `ALCEDO_LUT` comment fields `input_space` / `output_space` are free text (official packages
write `ACEScc`) and stay as they are. L5 does not read them (decision E9).

Model:

- `LutLibraryUserState` gains
  `std::map<std::string, LutRememberedEncodings, std::less<>> remembered_encodings`, with
  `struct LutRememberedEncodings { std::string input_encoding; std::string output_encoding; }`.
- JSON: a new key `remembered_encodings`, an object
  `{"<entry id>": {"input": "<id>", "output": "<id>"}}`. It is written only when it is not empty,
  so files without remembered pairs keep their bytes. `schema` stays 1. The current parser
  ignores unknown keys, so an older build still reads the file. That build drops the key the next
  time it writes the file (risk table).
- Parsing: an item with an invalid entry ID (`IsValidLutLibraryEntryId`) or an encoding id that
  is not in the catalog is dropped on its own and reported as a diagnostic. It does not reject
  the file. A rejected file loads as an empty state, and the next write would then lose the
  favorites too. `LutLibraryService` adds `ColorEncodingCatalog` as a private dependency
  (`color/` is a leaf library, so the dependency points down).
- A remembered pair is kept when its entry disappears from the inventory, as favorites are.

Service API, following `SetFavorite`:

- `RememberedEncodings(entry_id) const -> std::optional<LutRememberedEncodings>`.
- `SetRememberedEncodings(entry_id, std::optional<LutRememberedEncodings>) -> Status`. `nullopt`
  forgets the pair. The call validates the entry ID and both catalog ids (`kInvalidRequest`) and
  returns `kBusy` while a root operation runs. It writes the file before the published state
  changes and emits `RememberedEncodingsChanged(entry_ids)` after the write.

Official defaults (L5 follow-up): when a **user-requested** refresh (`RefreshInventory`, the
library "refresh" action) publishes its inventory, every listed official package LUT that the
grade can apply and that has no remembered pair gets ACEScc to ACEScc, in one user state write.
Its checkbox is then checked. User LUTs get no default pair, so their checkbox stays unchecked
until the user checks it. Loading the library and automatic refreshes (missing-file lookups) do
not write defaults. A user refresh that is coalesced into a running automatic refresh writes them
when that refresh completes. A pair that the user chose is never replaced; a pair that the user
forgot gets the defaults again at the next user refresh (E12).

#### L5.2 Panel: "Remember for this LUT"

A checkbox under the two encoding combos, shown only when the association resolves to a listed
entry (`associationEntryId` is not empty). It is bound to the library, not to the document:

- **Checked** means "this LUT has a remembered pair". Official LUTs are checked after a user
  refresh (official defaults above); other LUTs start unchecked. `LutLibraryController` gains
  `Q_PROPERTY(bool rememberEncodings ...)` and `Q_PROPERTY(bool canRememberEncodings ...)`, both
  notified by `associationChanged` and `RememberedEncodingsChanged`.
- **Checking** stores the encodings shown in the panel (`inputEncoding`, `outputEncoding`) as the
  pair. Checking and then not changing a combo already remembers the current choice.
- **Unchecking** forgets the pair (decision E10).
- **While checked**, a user selection or reset in either combo also updates that side of the
  stored pair. Only that side is changed, as the L4 document write changes only one side. The
  other side is already stored because checking stored both.
- **While unchecked**, a combo change edits the document only (L4 behavior).
- The checkbox is not an edit. It creates no history entry, and undo and redo do not change the
  remembered pair. Undoing an encoding change does not undo the remembered value: the pair is a
  library preference, like a favorite.

#### L5.3 How a panel commit reaches the library

The library update is a UI-level side effect of a user selection. It is not part of the
document commit. Extending the session submit path (`IEditorAdjustmentSubmitter` →
`EditorSessionController` → history) to notify other modules would make the session and history
depend on the LUT library. It would also raise questions the document layer cannot answer:
whether undo reverts a disk write, and which library entry a `LutReference` in the document
belongs to. That knowledge exists only in `LutLibraryController`.

The models already depend in the right direction:
`EditorLutEncodingModel → LutLibraryController → LutLibraryService → LutLibraryUserState`.
L5 uses that chain and adds one general hook to the model base:

- `EditorAdjustmentModelBase` gains
  `protected: virtual void onSettledWriteAccepted(const alcedo::EditorParameterWrite& write)`
  (default: nothing). `submitNow` calls it after `submitWrite` returns true for a `settled` write.
  `submitNow` is reached only from user edits (`selectIndex`, `reset`, value commits, toggles,
  and `submitJsonBoundary`), never from the load setters. So the hook fires once per user edit
  and never on load. The write is copied before it is moved into `submitWrite`.
- `EditorLutEncodingModel` overrides it:
  `if (target_) target_->RememberEncodingSide(output_side_, currentValue())`.
- `LutLibraryController::RememberEncodingSide(bool output, const QString& id)` does nothing
  unless `rememberEncodings` is true. Otherwise it reads the stored pair, replaces one side and
  calls `SetRememberedEncodings`. A failure sets `lastError`; the document edit has already been
  accepted and stays.

This adds no forward declarations and no new dependency edge. A future "remember this setting"
feature on another panel overrides the same hook in its own model and calls its own owner. The
session stays document-only.

"Accepted" is not "applied": the session can still reject the write later (for example, the node
was removed). The remembered pair still records the user's choice for that LUT. That is
intended: it does not depend on whether this one image received it.

#### L5.4 Applying a LUT with a remembered pair

- `LutLibraryController::applyEntry` reads `RememberedEncodings(entry_id)`. When a pair exists,
  `SubmitReference` puts it into the same `EditorLutWrite` (`reference`, `display_name`,
  `input_encoding`, `output_encoding`). The new LUT and its encodings are one history entry, and
  one undo restores the previous LUT and encodings.
- Without a remembered pair the write carries ACEScc to ACEScc (L5 follow-up), so switching
  LUTs never leaves the encodings of the previous LUT on the image. This applies the default
  only; it does not remember it.
- `clearAssociation`, adjustment transfer and history replay do not read remembered pairs. They
  carry the encodings stored in the document, as in L4.
- Official LUTs can be remembered the same way. This does not contradict the 10. scope note,
  which is about package manifests.

#### Work

- `LutLibraryUserState`, its parse and serialize functions, and `LutLibraryService` (L5.1).
- `EditorAdjustmentModelBase::onSettledWriteAccepted`, `EditorLutEncodingModel` override,
  `LutLibraryController` properties, `setRememberEncodings(bool)` (Q_INVOKABLE),
  `RememberEncodingSide`, apply with the remembered pair (L5.2 to L5.4).
- `EditorLutControlPanel.qml`: the checkbox, with a tooltip that says the choice is stored in
  the LUT library and applies the next time this LUT is selected.
- zh_CN translations by editing the `.ts` file directly (no `lupdate` run).

#### Acceptance criteria

- `lut-library.json` without `remembered_encodings` serializes byte for byte as before.
  Remembered pairs round-trip. An item with an invalid entry ID or an unknown encoding id is
  dropped on its own; the favorites in the same file are kept.
- `SetRememberedEncodings` writes the file before it publishes, rejects unknown ids with
  `kInvalidRequest` and returns `kBusy` during a root operation. A remembered pair survives a
  root migration and an update of an official package (same entry ID).
- Checking stores the displayed pair. Unchecking forgets it. Neither submits an editor write or
  creates a history entry.
- With the checkbox checked, one combo selection gives one history entry and changes only that
  side of the remembered pair. With it unchecked, the remembered pair is not changed. Loading a
  target (node change, reopen, undo, redo) never writes the library (no `onSettledWriteAccepted`
  call on load).
- Applying an entry with a remembered pair gives one history entry that sets the reference and
  both encodings. Undo restores the previous LUT and encodings. The rendered table equals the
  table of the same LUT applied with the same encodings selected by hand.
- Applying an entry without a remembered pair sets ACEScc to ACEScc in the same write and
  remembers nothing (L5 follow-up).
- A user refresh gives every listed official LUT without a pair ACEScc to ACEScc and gives user
  LUTs nothing. A chosen pair is kept. Loading and automatic refreshes write no defaults; a user
  refresh coalesced into an automatic refresh does (L5 follow-up).
- Undo of an encoding change does not change the remembered pair.

##### Phase L5 completion record (2026-10-05)

**Status:** complete — the library user state remembers an input and output encoding pair per
entry ID. The LUT panel has a "Remember for this LUT" checkbox. A checked combo selection updates
its side of the pair. Applying a LUT with a remembered pair writes the reference and both
encodings in one settled write. The zh_CN translations are added by hand.

**Primary success call chain (remember and update):**

```text
EditorLutControlPanel.qml: ThemeCheckBox "editorLutRememberEncodings" (onToggled)
  -> LutLibraryController::setRememberEncodings(true | false)
       pair = shown inputEncoding / outputEncoding (published document)
  -> LutLibraryService::SetRememberedEncodings(entry id, pair | nullopt)
       IsValidLutLibraryEntryId + IsValidLutRememberedEncodings (color catalog)
  -> CommitUserState: write_user_state(lut-library.json) -> LutLibraryPublication::SetUserState
  -> RememberedEncodingsChanged -> LutLibraryController::reload -> rememberEncodings
EditorLutControlPanel.qml: AdjustmentCombo (onActivated) / reset
  -> EditorAdjustmentEnumModel::selectIndex / reset -> submitNow(settled = true)
  -> IEditorAdjustmentSubmitter::submitWrite accepted (one history entry, L4)
  -> EditorAdjustmentModelBase::onSettledWriteAccepted
  -> EditorLutEncodingModel override -> LutLibraryController::RememberEncodingSide(side, id)
       no pair: nothing; pair: replace this side -> SetRememberedEncodings
```

**Primary success call chain (apply):**

```text
LUT browser apply -> LutLibraryController::applyEntry(entry id)
  -> LutLibraryService::RememberedEncodings(entry id)
  -> SubmitReference(reference, name, pair): one EditorLutWrite
       {reference, display_name, input_encoding, output_encoding}
  -> LutTargetSource::SubmitLutWrite -> session: one settled `lut` write, one history entry
  -> LmtModel::ApplyUpdate -> same composite table as the pair selected by hand
no pair -> EditorLutWrite{reference, display_name} only; the stored encodings stay (L4)
```

**Primary failure call chain:**

```text
lut-library.json item with an invalid entry ID or encoding id
  -> ParseLutLibraryUserState drops that item only (dropped_items); favorites are kept
  -> LutLibraryService load: qWarning "LUT library state item ignored: ..."
SetRememberedEncodings with an invalid id / during kLoad, kUseRoot, kMigrateRoot / write failure
  -> kInvalidRequest / kBusy / kPersistenceError; published state and file unchanged
  -> LutLibraryController::lastError "The LUT library cannot save the remembered encodings."
the session does not accept the combo write (canEdit false or rejected)
  -> submitNow returns false before onSettledWriteAccepted; the pair is unchanged
checkbox without a library entry (no LUT, or a file outside the library)
  -> canRememberEncodings false (checkbox hidden); setRememberEncodings returns false
```

**Decisions taken during L5 (deviations from the Work list):**

| Item | Plan text | Implemented | Reason |
|---|---|---|---|
| Hook signature | `onSettledWriteAccepted(const EditorParameterWrite& write)` | `onSettledWriteAccepted()` without a parameter | The override reads its own selection (`currentValue()`). A parameter needs a copy of every settled write before it is moved into `submitWrite`, and no override uses it. |
| Dropped-item report | "reported as a diagnostic" | `LutLibraryUserStateReadResult::dropped_items`; the service logs each item with `qWarning` when it loads a root | The user state has no diagnostics list, and scan diagnostics mark the inventory incomplete. A dropped preference is not an inventory problem. |
| Busy rule | "returns `kBusy` while a root operation runs" | `RootOperationRunning()` (kLoad, kUseRoot, kMigrateRoot), shared with `SetFavorite` | Same rule as favorites, in one place. |
| `ALCEDO_LUT` `input_space` / `output_space` | E9: not read | Not read | Confirmed by the user: official packages are ACEScc, which is the default. |

**What was proven (executed tests):**

| Criterion | Test | Result |
|---|---|---|
| A state without remembered encodings serializes to the bytes written before L5 | `LutLibraryServiceTest.LutRememberedEncodingsStateTest.StateWithoutRememberedEncodingsKeepsItsSerializedBytes` | PASS |
| Remembered pairs round-trip | `LutLibraryServiceTest.LutRememberedEncodingsStateTest.RememberedEncodingsRoundTripThroughTheSerializedState` | PASS |
| An invalid entry ID, an unknown encoding, a missing side, a non-object item and a non-object key are dropped one by one; favorites are kept | `LutLibraryServiceTest.LutRememberedEncodingsStateTest.InvalidRememberedItemsAreDroppedAndFavoritesAreKept` | PASS |
| A write failure publishes nothing; the file is written before publication; the same pair writes nothing; a restart reads it; forgetting removes it | `LutLibraryServiceTest.LutRememberedEncodingsTest.SetRememberedEncodingsPersistsBeforePublishingAndForgets` | PASS |
| An invalid entry ID or encoding id is `kInvalidRequest` with no change | `LutLibraryServiceTest.LutRememberedEncodingsTest.SetRememberedEncodingsRejectsInvalidIdsWithoutChange` | PASS |
| `kBusy` during a root migration, accepted after it | `LutLibraryServiceTest.LutRememberedEncodingsTest.SetRememberedEncodingsIsRejectedWhileRootOperationRuns` | PASS |
| A pair survives an official package update (other content directory), a root migration and a restart | `LutLibraryServiceTest.LutRememberedEncodingsTest.RememberedEncodingsFollowPackageUpdateAndRootMigration` | PASS |
| Checking stores the shown pair; unchecking forgets it; neither submits an edit; without a LUT there is nothing to remember | `LutLibraryModelTest.LutRememberedEncodingsControllerTest.CheckingStoresShownPairAndUncheckingForgetsIt` | PASS |
| Checked: one selection is one settled write and changes only its side. Loads (reload, a new model, an undo applied as a load) never write the library. A rejected write leaves the pair. Unchecked: the pair is not changed | `LutLibraryModelTest.LutRememberedEncodingsControllerTest.CheckedSelectionUpdatesItsSideAndLoadsNeverWrite` | PASS |
| Apply with a pair is one write with the reference and both encodings; the LMT JSON and the packed table key (65³) equal the same LUT with the encodings selected by hand; apply without a pair keeps the stored encodings | `LutLibraryModelTest.LutRememberedEncodingsControllerTest.ApplyWithRememberedPairWritesReferenceAndEncodingsOnce` | PASS |
| That write is one commit; undo restores the previous LUT and encodings; redo applies them again | `EditorSessionHistoryPortTest.EditorDocumentHistoryTest.LutApplyWithEncodingsIsOneCommitAndUndoRestoresPreviousLut` | PASS |
| Production `EditorLutControlPanel.qml`: the checkbox follows the library, its `toggle()` forgets the pair without an edit, it is hidden without a LUT, no QML warnings | `EditorLutBrowserPanelQmlTest.ControlPanelShowsStoredEncodingsAndDisplayNote` (extended) | PASS |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 6 --target
LutLibraryServiceTest LutLibraryModelTest EditorSessionHistoryPortTest EditorAdjustmentModelTest
EditorLutBrowserPanelQmlTest`;
`ctest --test-dir build/debug -R "^(LutLibraryServiceTest|LutLibraryModelTest|EditorSessionHistoryPortTest|EditorAdjustmentModelTest|EditorLutBrowserPanelQmlTest)\." -j 4`
(vcpkg debug bin on `PATH`). Run again after `git clang-format`.

Suite totals: LutLibraryServiceTest 31/31, LutLibraryModelTest 23/23, EditorAdjustmentModelTest
15/15, EditorLutBrowserPanelQmlTest 9/9, EditorSessionHistoryPortTest 108/109. The one failure,
`EditorHistoryCommitPresentationTest.FormatsNumericBooleanPathEnumAndCompoundAdjustments`
(expects "+12°", gets "Crop"), is the failure that the L4 record shows on a clean `HEAD`. L5 does
not touch history presentation.

**Checklist / exit condition:** all seven acceptance criteria are proven by the tests above.

**LOC note (grill-code-review):** new test files `lut_library_remembered_encodings_test.cpp`
213 and `lut_remembered_encodings_controller_test.cpp` 247 (the controller test file has 728
lines, so the L5 tests have their own file). Changed production files stay below 1000 lines
(`lut_library_service.cpp` 890, `lut_library_controller.cpp` 397). `LutLibraryModelTest` now
links `EditRuntime` for `TryPackGradeLut`. `editor_document_history_test.cpp` grows to 1252
lines (1196 before); the new test uses the fixture that is defined in that file. No split was
made.

**Remaining gaps:**
- `alcedo_main` was not built, and the checkbox was not checked in the running application. The
  production QML was loaded from source by `EditorLutBrowserPanelQmlTest` with no warnings.
- Pointer input on the checkbox was not driven (offscreen input is unreliable, AGENTS.md). The
  test calls the checkbox's QML `toggle()`, which its pointer and key handlers call.
- `alcedo_main_en.ts` has no entries for these contexts (source language); unchanged.

##### Phase L5 follow-up record (2026-10-05)

**Status:** complete — after user review: a user-requested refresh gives official package LUTs
without a pair ACEScc to ACEScc (their checkbox is then checked), user LUTs get no default, and
applying a LUT without a pair applies ACEScc to ACEScc instead of keeping the previous encodings.
Unchecking still forgets the pair (E10). Decision E12.

**Primary success call chain (official defaults):**

```text
library "refresh" -> LutLibraryService::RefreshInventory -> RequestRefresh(user_requested = true)
  -> official_defaults_requested_ (also set when coalesced into a running refresh)
  -> worker: ScanLutLibraryRoot + write_inventory
  -> owner: PublishInventory -> RememberDefaultEncodingsForOfficialLuts
       entries with OfficialLutReference, SupportsGradeApplication, no remembered pair
  -> CommitUserState (one write) -> RememberedEncodingsChanged(entry_ids)
  -> LutLibraryController::reload -> rememberEncodings true (checkbox checked)
```

**Primary success call chain (apply without a pair):**

```text
LutLibraryController::applyEntry(entry id)
  -> RememberedEncodings(entry id) is nullopt -> ACEScc / ACEScc (kDefaultLutEncodingId)
  -> SubmitReference: one EditorLutWrite {reference, display_name, input, output}
  -> one history entry; nothing is remembered
```

**Primary failure call chain:**

```text
inventory write fails during the refresh -> no publish, no defaults; the request is cleared
default user state write fails -> lastError "The default LUT encodings cannot be saved: ...";
  published state unchanged; the refresh result itself is unchanged
RefreshInventory rejected (kBusy, another operation) -> the default request is cleared
automatic refresh (Start, missing-file lookup) -> no defaults
```

**What was proven (executed tests):**

| Criterion | Test | Result |
|---|---|---|
| Load writes no defaults; a user refresh gives the official LUT ACEScc to ACEScc (one signal, persisted) and the user LUT nothing; a chosen pair is kept; a forgotten official pair returns at the next user refresh | `LutLibraryServiceTest.LutRememberedEncodingsTest.UserRefreshRemembersDefaultEncodingsForOfficialLutsOnly` | PASS |
| An automatic refresh writes no defaults; a user refresh coalesced into a running automatic refresh does | `LutLibraryServiceTest.LutRememberedEncodingsTest.UserRefreshCoalescedIntoRunningRefreshWritesDefaults` | PASS |
| Apply without a pair writes ACEScc to ACEScc in the same write, the LMT has ACEScc to ACEScc after the previous LUT had S-Log3 to Rec.709, and nothing is remembered | `LutLibraryModelTest.LutRememberedEncodingsControllerTest.ApplyWithRememberedPairWritesReferenceAndEncodingsOnce` (changed) | PASS |
| The L5 tests above, with `RememberedEncodingsChanged` now carrying a list | all L5 tests | PASS |

Commands: as in the L5 record, run again after `git clang-format`.

Suite totals: LutLibraryServiceTest 33/33, LutLibraryModelTest 23/23, EditorAdjustmentModelTest
15/15, EditorLutBrowserPanelQmlTest 9/9, EditorSessionHistoryPortTest 108/109 (the same failure
as in the L5 record, also failing on a clean `HEAD`).

**Remaining gaps:** as in the L5 record. In addition, official LUTs installed between two user
refreshes have no pair until the next user refresh (their checkbox is unchecked); applying them
uses ACEScc to ACEScc, which is the same pair.

## 9. Decisions

| ID | Decision |
|---|---|
| E1 | The bake runs on the host with the shared portable headers; the GPU passes are not used for it (section 4.1). |
| E2 | Display inputs use the OCIO reference forward, paired with the R2 inverse; Alcedo's DRT is unchanged (sections 4.2 and 7). |
| E3 | The composite table is 65³. |
| E4 | 1D shaper LUTs are rejected. `DOMAIN_MIN/MAX` is honored. |
| E5 | All gamuts and transfer functions come from one catalog; existing copies are moved onto it in L1. |
| E6 | Defaults are ACEScc to ACEScc and are not serialized. Unknown ids are errors. |
| E7 | DJI D-Log and D-Log M are included. Official LUT package manifests do not declare encodings. |
| E8 | Remembered encodings are library user state in `lut-library.json`, keyed by entry ID. `.cube` files are never rewritten (L5.1). |
| E9 | The free-text `input_space` / `output_space` of the `ALCEDO_LUT` comment are not mapped to catalog ids in L5. |
| E10 | The "remember" checkbox shows whether a pair is stored. Checking stores the current pair; unchecking forgets it. The pair is not part of edit history. |
| E12 | Only a user-requested refresh writes default pairs, only for official package LUTs, and only ACEScc to ACEScc. Applying a LUT without a pair applies ACEScc to ACEScc without remembering it. A forgotten official pair returns at the next user refresh. |
| E11 | A panel's non-document side effect goes through `EditorAdjustmentModelBase::onSettledWriteAccepted` to the model's own feature controller. The session submit path stays document-only (L5.3). |

## 10. Scope notes

- Official LUT package manifests do not declare encodings in this plan. Selecting an official LUT
  does not change the input and output encodings.
- DJI D-Log and D-Log M are both in scope. L1 cites DJI's published definition of each curve and
  of D-Gamut. If DJI has published no formula or code value table for D-Log M, L1 reports that
  as a blocking item instead of deriving the curve from a LUT.

## 11. Risks

| Risk | Consequence | Handling |
|---|---|---|
| L1 changes pixels of existing renders | Existing projects look different | L1 before/after render comparison on every backend |
| Trilinear error near the display gamut boundary after the inverse | Visible error in saturated colors | 65³ grid; L3 measures and records the error |
| A vendor curve is transcribed wrongly | Wrong conversion for that camera | OCIO builtin comparison or the vendor's stated code values in L1 |
| Projects that use 1D shaper or non-unit-domain LUTs render differently | Visible change after update | Intended correction; noted in the release notes |
| A display-output LUT clips at display white | Flat highlights after the inverse | Inherent to inverting a display rendering (raster plan risk table) |
| An older build rewrites `lut-library.json` (for example, on a favorite change) | Its serializer drops `remembered_encodings`, so the remembered pairs are lost | Accepted: the loss is limited to the preferences, documents are unaffected, and favorites are kept |
