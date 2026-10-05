# LUT Input and Output Color Encoding Plan

Status: L1 partial (2026-10-05): the color encoding catalog landed; D-Log M is blocked (no DJI
definition) and Metal is not built or tested. L2 to L4 not started.

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
