# Raster Image Input (JPEG, PNG, TIFF, OpenEXR) Plan

Date: 2026-10-04

Status: **Planned; decisions recorded.** No code has changed. Section 12 records the product
owner's decisions of 2026-10-04, and the plan follows them.

Parent: [Roadmap](../../roadmap.md).

Other areas this plan touches:

- **Import (UI and service):** content classification, folder import file-type selection, import
  options.
- **Metadata and storage:** the image type value, the source color description, EXIF read from
  non-RAW files, and compatibility with existing projects (section 7).
- **Thumbnails and export:** raster decode for renditions, and metadata copy on export.

Source audit revision: `abdb000c8` on `main`.

Unless a path starts with `alcedo_studio/` or `docs/`, it is relative to `alcedo_studio/src/`.
Test paths are relative to `alcedo_studio/`.

Render backends: **CUDA, OpenCL, Metal only.** WebGPU and the CPU render path are deprecated and
get no work in this plan. All host-side (CPU) work in this plan is file decoding, ICC parsing,
table precomputation and parameter packing. None of it renders pixels.

---

## 1. Goal

Import, edit, and export JPEG, PNG, TIFF and OpenEXR files with a defined color path:

1. Read the color description the file carries. That is an embedded ICC profile, or the color
   fields the format defines: PNG `cICP`/`sRGB`/`gAMA`/`cHRM`, the OpenEXR `chromaticities`
   attribute, and the EXIF `ColorSpace` tag with the interoperability index. Only these four
   formats are in scope.
2. Treat these files as **not RAW**. Do not read or apply camera color matrices, DNG profiles,
   as-shot white balance, demosaic or highlight reconstruction. The Color Grade **CAT02 white
   balance** (Look panel) still works unchanged.
3. From the color description, find the source primaries, white point and transfer function
   (EOTF), best effort. If nothing usable is found, use **sRGB**; the product owner allowed this
   one default. What happens next depends on the input:
   - **Display-referred** pixels go through the **inverse ACES 2.0 output transform**, with
     **OpenColorIO 2.5.1 as the reference**, into ACES2065-1 scene-linear.
   - **Scene-linear** pixels go through a 3×3 matrix only.

   Both then enter the existing working space (AP1, ACEScc encoded). The Develop white-balance
   stage is skipped.
4. Rework folder import so the user picks which file types to import. The default is RAW, JPEG,
   TIFF, PNG and OpenEXR, and the last selection is remembered. Every other file type stays
   disabled.
5. **Existing projects open unchanged.** There are no table changes and no format-version bumps.
   A RAW document serializes byte for byte as it does today. Section 7 has the details.

The inverse transform must be efficient, and the design must allow a future, non-invertible DRT.
For that reason the inverse is a fixed ACES 2.0 inverse that does not depend on the DRT the
document's DRT node uses (section 5.6).

### Non-goals

- Formats other than JPEG, PNG, TIFF and OpenEXR (WebP, HEIF/AVIF, JPEG XL, PSD).
- JPEG gain maps (Ultra HDR / ISO 21496-1). The SDR base image is decoded and the gain map is
  ignored.
- CMYK images. They are rejected at import as unsupported (decision D4).
- Alpha. It is discarded (decision D6).
- Writing anything into source files. Ratings and tags stay in the project database, as today.
- An inverse for OpenDRT or any future DRT.
- Opening a project that contains raster images in an **older** Alcedo build. Section 7.5 states
  what that build does.

---

## 2. Current state (audited)

### 2.1 Import rejects everything that is not RAW

- `image/metadata_extractor.cpp:1797-1806`: `ExtractEXIF_ToImage` accepts a file only if LibRaw
  opens it. The test is the file's content, not its extension. Otherwise it throws
  `UNSUPPORTED_FORMAT`.
- `app/import_service.cpp:186-384`: every path gets a placeholder element and a metadata task.
  Rejected files are deleted in `SyncImports` (`:386-413`) and counted as `unsupported_`.
- `include/type/supported_file_type.hpp:59-68` holds a RAW-only, case-sensitive extension set.
  No production import code uses it.
- `include/image/image.hpp:21` declares `ImageType { DEFAULT, JPEG, PNG, TIFF, ARW, CR2, CR3,
  NEF, DNG }`. Import never sets it (`import_service.cpp:284` TODO), so every existing row stores
  `0` (`DEFAULT`) in `Image.type`.
- Tests that assert the RAW-only rule, and that this plan changes:
  - `tests/app/import_raw_only_test.cpp:156, :197`
  - `tests/image/metadata_extractor_test.cpp:335, :380`
  - `tests/ui/album_backend_import_test.cpp:120, :161, :191, :530`

### 2.2 Folder import

- `FolderImportScanModel` (`ui/alcedo_main/album_backend/folder_import_scan_model.cpp`) scans on
  a worker thread with a recursive `recursive_directory_iterator`. It keeps every regular file
  (`:128`) and sends batches of 512 paths or one batch every 100 ms. It does not classify files.
- `qml/FolderImportConfirmDialog.qml` lists all scanned files. Its note at `:149` says only RAW
  files are imported. It has no options.
- `ImportOptions` (`include/app/import_service.hpp:36-43`) is built with defaults
  (`import_export.cpp:228`) and ignored (`import_service.cpp:190`).
- The file picker uses the filter `All Files (*)` (`qml/AppDialogs.qml:38-50`).

### 2.3 Decode and develop graph

- The decoder is LibRaw only. Both `include/edit/runtime/renderer.hpp:211-222` and
  `edit/input/prepared_source_cache.cpp:14` call `RawInputLoader::LoadEncoded`.
- Host plane formats are only `U16Cfa` and `F32Rgba` (`include/edit/input/prepared_raw_input.hpp:37-40`).
- Pass order (`edit/runtime/graph_compiler.cpp:416-437`): `UploadRgb` (or the RAW passes), then
  `Lens`, `GeometryResample`, `CameraToAp1`, the Color Grades, `DiffusionFilter` and `Drt`.
- The `PlanExecutor` result cache (`include/edit/runtime/plan_executor.hpp:90-186`) holds
  `sensor_linear_output`, `geometry_output` and `develop_output` (the output of CameraToAp1).
  These passes run again only when their validity keys change. A Color Grade slider change does
  not run them.
- Working space: CameraToAp1 writes **ACEScc-encoded AP1**. The DiffusionFilter pass decodes it
  to linear AP1 for the DRT.
- `BindRgbWorkingSpaceCameraProfile` (`edit/graph/develop_color_transform.cpp:653-677`) is the
  only existing hook for non-RAW input. It treats pixels as **linear** Rec.709 and runs them
  through the camera white-balance solve. Nothing on the input side decodes a transfer function.
  Section 6.4 replaces this hook.

### 2.4 Output transform

- The forward ACES 2.0 runs on all three backends (`odt_funcs.cuh`, `drt_aces.metal`, `cst.cl`).
  Its tables come from `ResolveACESODTRuntime(ColorSpace, float peak)`
  (`edit/runtime/drt/aces_odt_runtime.cpp:146-197`), which caches them in a process-wide map keyed
  by the limiting-space **enum** and the peak luminance.
- The forward deliberately differs from the Academy CTL and from OCIO. The differences are a Hunt
  colorfulness fade, `chroma_j_floor`, black for `J <= 1e-3`, `limit_rgb_preserve_chroma`, the
  ACES 1.3 RGC before the transform, a different `s_2` normalization, and 362-entry tables
  instead of OCIO's 363.
- No inverse exists.
- `DrtEotf` (`include/edit/graph/drt_node_model.hpp:36-44`) has no piecewise sRGB value, so
  "sRGB" output today is Gamma 2.2. The EOTF is serialized as a string (`drt_node_model.cpp:411`).

### 2.5 Persistence (relevant to section 7)

- **`Image` table** (`include/storage/mapper/image/image_mapper.hpp:60-80`): `id`, `image_path`,
  `file_name`, `type UINT32`, `metadata VARCHAR` (JSON), plus derived search columns such as
  `file_ext`, `camera_make` and `rating`. `RawRuntimeColorContext` lives inside the `metadata`
  JSON under the key `"RawRuntimeColorContext"` (`image/image.cpp:22, 190-239`).
- **Root and checkpoint state** (`edit/history/pipeline_document_checkpoint.cpp:105-170`):
  - `DecodePipelineRootState` requires an **exact** key set and **exact** version equality:
    root 5, document 7, image edit schema 5.
  - It also requires that the nested document re-serializes to the same JSON.
  - `ComputeRootId` hashes the versions, the document dump and the raw-color dump.
  - The file says it "does not convert older root JSON".
- **Every version constant is checked for equality, with no migration**
  (`include/edit/history/pipeline_history_format.hpp:40-70`): project file `0.10.0`, packed
  format 7, document 7, image edit schema 5, commit 5, chain 5, edit batch 4, root 5,
  checkpoint 5.
- **Image-specific Develop data already lives in the document.** `DevelopPayload.camera_profile`
  is serialized by `DevelopParamsModel::ToJson` (`edit/graph/develop_node_model.cpp:263-300`),
  and `LoadJson` reads every key with a default.

### 2.6 Libraries available (vcpkg classic mode)

| Library | Version | Use in this plan |
|---|---|---|
| OpenImageIO | 3.0.9.1 | Pixel decode for PNG, TIFF and EXR; header attributes |
| libjpeg-turbo | 3.1.3 | JPEG decode through the TurboJPEG API, including DCT-scaled decode |
| lcms2 | 2.17 | ICC parsing, and host conversion for LUT-based RGB profiles. Found by CMake (`CMakeLists.txt:470-484`); nothing links it yet |
| OpenColorIO | 2.5.1 | **Reference for the inverse.** The ACES2 code is ported; the library is linked by tests only (section 5) |
| Exiv2 | 0.28.5 | EXIF and XMP display metadata for JPEG, PNG and TIFF |

The bundled export ICC profiles in `alcedo_studio/src/config/icc/` (Rec.709, P3 variants,
Rec.2020 PQ/HLG, upstream v4 profiles) are the first ICC test fixtures. An Alcedo export must
re-import with the same primaries and transfer function.

---

## 3. Terms

- **Raster input:** a JPEG, PNG, TIFF or OpenEXR file decoded as RGB pixels, not sensor data.
- **Display-referred raster:** code values encode light **after** a display rendering. This is
  every integer JPEG, PNG and TIFF, plus float TIFF whose ICC curve is not linear. These pixels go
  through the inverse ACES 2.0 output transform.
- **Scene-linear raster:** values encode scene light. This is every OpenEXR, plus float TIFF with
  a linear ICC curve or no ICC (decision D3). These pixels go through a 3×3 matrix only.
- **Source color description:** primaries, white point, transfer function and peak luminance,
  together with where each value came from (ICC, CICP, PNG chunks, EXR attribute, EXIF, or the
  sRGB default).

---

## 4. Source color description

### 4.1 Data type

New header `include/image/raster_color_description.hpp`:

```cpp
enum class RasterReferral : uint8_t { DisplayReferred, SceneLinear };

enum class RasterTransferKind : uint8_t {
  Linear, SrgbPiecewise, Gamma, Bt1886, IccParametric, IccSampled, St2084, Hlg,
};

enum class RasterColorOrigin : uint8_t {
  IccMatrixShaper, IccLutConverted, IccCicp, PngCicp, PngSrgbChunk, PngGamaChrm,
  ExrChromaticities, ExrAcesContainer, ExifInteropAdobeRgb, DefaultSrgb,
};

struct RasterTransfer {
  RasterTransferKind   kind_ = RasterTransferKind::SrgbPiecewise;
  float                gamma_ = 2.2f;           // Gamma
  uint8_t              icc_parametric_type_ = 0;
  std::array<float, 7> icc_params_{};          // ICC parametric g,a,b,c,d,e,f
  std::vector<float>   sampled_;               // IccSampled: 4096 entries over input 0..1
};

struct RasterColorDescription {
  RasterReferral                referral_;
  std::array<float, 8>          primaries_xy_;           // Rx Ry Gx Gy Bx By Wx Wy, native white
  std::array<RasterTransfer, 3> transfer_;               // equal in the common case
  float                         peak_luminance_nits_ = 100.0f;
  RasterColorOrigin             origin_;
  std::string                   profile_description_;    // ICC 'desc', for the UI only
  std::string                   default_reason_;         // why DefaultSrgb was used, if it was
};
```

Section 7 defines where this is persisted and its JSON form.

### 4.2 Resolution rules per format

The rules are applied in order, and the first one that gives a **usable** description wins.
"Usable" means:

- the primaries form a non-degenerate triangle;
- the white point lies inside that triangle;
- the transfer curve is monotonic.

A description that fails these checks counts as not present, and `default_reason_` records why.

| Format | Order of precedence |
|---|---|
| JPEG | 1. ICC (APP2 `ICC_PROFILE`, multi-segment). 2. EXIF `ColorSpace = 0xFFFF` with interoperability index `R03` → Adobe RGB (1998). 3. sRGB default. |
| PNG | 1. `cICP`. 2. `iCCP`. 3. `sRGB` chunk. 4. `gAMA` + `cHRM`; `gAMA` alone uses sRGB primaries with that gamma. 5. sRGB default. |
| TIFF | 1. ICC (tag 34675). 2. sRGB default. Float TIFF without ICC uses the scene-linear Rec.709 default (D3). |
| OpenEXR | 1. `chromaticities` attribute. 2. `acesImageContainerFlag = 1` → AP0. 3. Rec.709 primaries with D65 white, the OpenEXR specification default. |

- **PNG:**
  - `cICP` takes precedence over `iCCP`, `sRGB`, `gAMA` and `cHRM`, as the PNG Third Edition
    specifies.
  - A small first-party chunk reader walks the chunks up to `IDAT` and reads `cICP`, `iCCP`
    (zlib), `sRGB`, `gAMA`, `cHRM`, `mDCv` and `cLLI`. This way it does not depend on which chunks
    the installed OpenImageIO exposes.
- **ICC** is parsed with lcms2 (`cmsOpenProfileFromMem`). What happens depends on the profile:
  - **RGB matrix-shaper** (`cmsIsMatrixShaper`):
    - Read `rXYZ`, `gXYZ` and `bXYZ`. These are D50-adapted, so undo the adaptation with the
      inverse of `chad`. For v2 profiles without `chad`, use `wtpt` and Bradford.
    - Parametric TRCs keep their type and parameters. Curves within 1e-4 of sRGB or a pure gamma
      are canonicalized to the analytic form. Other curves are sampled to 4096 entries with
      `cmsEvalToneCurveFloat`.
  - **ICC v4.4 `cicp` tag:** it overrides the profile's TRC and primaries. This is how
    `config/icc/rec2020_pq.icc`-style profiles state PQ or HLG.
  - **LUT-based RGB profile**, meaning `A2B0` with no matrix-shaper tags (decision D4):
    - At decode, lcms2 converts the pixels on the host (relative colorimetric, float) to linear
      Rec.2020.
    - The description becomes Rec.2020 primaries, `Linear` transfer, display-referred, origin
      `IccLutConverted`.
    - The transform is cached per profile hash for the decode session.
  - **Gray** (`grayTRC`) on single-channel images: the gray value is copied to R, G and B, with
    sRGB primaries and the profile curve.
  - **CMYK** (data color space `cmsSigCmykData`) and CMYK JPEG/TIFF without ICC are rejected at
    import as `UNSUPPORTED_FORMAT`.
- **Peak luminance** applies to display-referred input:
  - SDR transfer functions: 100 nits.
  - PQ: the `mDCv` maximum luminance, else `cLLI` MaxCLL, else 1000 nits, clamped to
    [100, 10000].
  - HLG: 1000 nits, matching the fixed 1000-nit HLG form already used on output
    (`disp_enc_funcs.cuh:209-243`).

### 4.3 Where it is parsed

`image/raster_color_description.cpp` exposes
`ResolveRasterColorDescription(std::span<const std::byte> file_bytes, RasterFileKind kind)`.
Import calls it once (section 7.2). Render does **not** parse the profile again; it reads the
stored description (section 7.3). The only exception is a LUT-based ICC: the decoder needs the
profile bytes to run the lcms2 conversion, and it checks that the profile hash equals the stored
one.

---

## 5. Inverse ACES 2.0 output transform (OCIO reference)

### 5.1 Reference

The inverse is **OpenColorIO 2.5.1's ACES 2.0 output transform inverse** (decision D1), which
implements the Academy reference. The port has two parts:

- **Host side:** `ACES2/Transform.cpp` init functions (`init_JMhParams`, `init_ToneScaleParams`,
  `init_ChromaCompressParams`, `init_GamutCompressParams`, `make_uniform_hue_gamut_table`,
  `make_upper_hull_gamma`, `determine_hue_linearity_search_range`), with OCIO's table layout of
  360 nominal entries plus 1 lower and 2 upper wrap entries (363 total).
- **Device side:** the per-pixel inverse from `FixedFunctionOpCPU.cpp`
  (`Renderer_ACES_OutputTransform20::inv`, `:1135-1166`) and the inverse shader generator in
  `FixedFunctionOpGPU.cpp` (`Add_ACES_OutputTransform_Inv_Shader`, `:1368-1440`), ported to
  CUDA, OpenCL and Metal.

The port is OCIO's code, not Alcedo's forward run backwards:

- It does not include the Hunt fade, `chroma_j_floor`, the RGC, or Alcedo's `s_2`
  normalization. It lives in new files and does not share parameter structs with the forward
  DRT, so a later change to the forward cannot change the inverse.
- Consequence, accepted by the product owner: a raster image viewed back through **Alcedo's**
  ACES 2.0 forward is close to the source but not identical. The deviations are in the shadows
  and saturated darks, where Alcedo's forward departs from the reference.

**License:** OpenColorIO is BSD-3-Clause. The ported files carry OCIO's copyright notice, and
`THIRD_PARTY_NOTICE.txt` gets an entry.

### 5.2 Parameters

- **Input JMh params:** AP0. **Reach:** AP1. Both match OCIO.
- **Limiting primaries:** the **source** primaries and white.
  - Equal source RGB values (the source white) map to M = 0 and come out as ACES neutral. This
    is the "no white simulation" case of the OCIO builtins, so no white scale is applied.
  - If any source primary lies outside AP1 (ProPhoto RGB), the limiting primaries are AP1.
    ACES 2.0 requires the limiting gamut to lie inside the reach gamut. Colors outside the AP1
    reach boundary land on it.
- **Peak luminance:** from section 4.2.
- **Cache:** new `ResolveAces2InverseRuntime(primaries_xy[8], peak)`. Its process-wide cache is
  keyed by the bit patterns and guarded by a mutex, the same pattern as `aces_odt_runtime.cpp`.
  A library holds a few distinct profiles (sRGB, Display P3, Adobe RGB), so the table build
  usually happens once per profile per process.

### 5.3 Per-pixel stages

The input is display-linear RGB in the limiting primaries, normalized as OCIO does, with
1.0 = 100 nits. The `LinearizeRaster` step writes it (section 6.2). The stages follow OCIO's
`inv()`:

1. Clamp to [0, peak/100], as the OCIO builtin does on the forward side.
2. **RGB → JMh** with the limiting params.
3. **Inverse gamut compression:**
   - Below the analytic threshold `lerp(cusp_J, limit_J_max, 0.3)`, use the closed form.
   - Above it, evaluate `compressGamut<inverse>` once to estimate `Jx`, then once more with that
     `Jx`. That is at most two evaluations, with no loop.
   - `remap_M` uses the closed-form Reinhard inverse.
   - The gamut boundary is OCIO's non-iterative line and power-curve intersection, in the **GPU
     generator's** form (`FixedFunctionOpGPU.cpp:1050-1051`), because all three backends are GPUs.
4. **Inverse tonescale (closed form):** `Z = clamp(J→Y, 0, inverse_limit)`,
   `f = (Z + sqrt(Z·(4·t_1 + Z))) / 2`, `Y = s_2 / ((m_2 / f)^(1/g) − 1)`, then Y → J.
5. **Inverse chroma compression (closed form):** OCIO `chroma_compress_inv`, which runs the two
   rational `toe_inv` steps in reverse order and then multiplies M by
   `(J_ts / J)^(−model_gamma_inv)`.
6. **JMh → AP0 RGB** with the input params. The result is **ACES2065-1 scene-linear**.
7. **AP0 → AP1** (a constant matrix), then clamp to [0, 8·r_hit]. That is the AP1 range the OCIO
   builtin accepts on the forward side, so the inverse result is valid input for the DRT again.
8. **ACEScc encode** into `develop_output`, the same encoding `CameraToAp1` writes.

### 5.4 Cost and placement (the efficiency design)

- **Runs at render extent, and its output is cached.**
  - The inverse sits in the slot of `CameraToAp1`, after `GeometryResample`, so it runs on the
    render extent (preview, detail patch or export), not the full source.
  - Its output is `develop_output`, which `PlanExecutor` caches by validity key. Color Grade, DRT
    and post-processing edits do **not** run it again. It runs only on a geometry or extent
    change, a new source, or a change to the input color description or override.
- **Per-pixel work is closed form, the same order as the forward DRT,** which already runs every
  frame on all three backends:
  - Two Hellwig CAM conversions, about 3 `pow` each.
  - One `atan2` and one `sincos`.
  - The 3-harmonic `Mnorm` polynomial.
  - One bounded hue-table search inside the precomputed linearity range.
  - At most two gamut-compression evaluations.
  - One `sqrt` and one `pow` for the tonescale.
  - Two rational toes.
  - No loops or iterative solves.
- **Tables:**
  - Reach M, the hue table, and cusp J, M and upper-hull gamma (packed as OCIO packs them) are
    uploaded **once per key**.
  - CUDA uses texture objects, Metal constant arrays and OpenCL buffers. The upload follows the
    `cuda_drt_runtime_state.cpp:81-99` pattern: upload only when the `shared_ptr` identity changes.
  - Each backend's `DisplayToAp1` state holds its own table set, separate from the DRT node's.
- **A 3D LUT was rejected.** A baked 65³ table would trade the closed form for one tetrahedral
  lookup. However:
  - The inverse is steepest at the gamut boundary, which is where saturated display colors sit
    and where a LUT's interpolation error is largest.
  - 16-bit and PQ input need a finer grid.
  - Because the output is cached (above), per-pixel cost is not on the slider path anyway.
- **Performance targets (release build):**
  - The `DisplayToAp1` pass takes at most **1.25×** the forward `Drt` pass time at the same
    extent on the same backend, measured with the existing pass statistics.
  - A table build for a new key takes at most **20 ms**. Implementation note: start each hue's
    upper-hull gamma search from the previous hue's result. OCIO's `make_upper_hull_gamma` lists
    this as a TODO, and the result must stay within section 5.7's tolerance of OCIO's tables.

### 5.5 Scene-linear path (OpenEXR, linear float TIFF)

There is no inverse DRT (decision D3). `DisplayToAp1` runs its scene-linear branch, which applies
one 3×3 matrix: source RGB → XYZ (native white) → CAT02 to the AP1 white → AP1.

It then applies the ACES 1.3 RGC, for the same reason `CameraToAp1` does: EXR can hold values
outside AP1. Last, it encodes ACEScc. This is one pass kind with two branches, so there is one
result-cache slot.

### 5.6 Independence from the document DRT

The inverse is always the OCIO ACES 2.0 inverse, whatever the DRT node selects. The DRT node only
controls output.

- Viewing through OpenDRT does not reproduce the source file. The product owner accepted this.
- A future non-invertible DRT needs no change here.

### 5.7 Verification of the inverse

OpenColorIO 2.5.1 is linked **by the test target only**. Every test runs on each backend: CUDA
and OpenCL on Windows, Metal on macOS.

- **`Aces2InverseMatchesOcioCpuProcessor`**
  - The input is a 33³ grid of display RGB in [0, 1].
  - The limiting spaces are Rec.709, P3-D65, Rec.2020 and Adobe RGB at 100 nits, plus Rec.2020
    at 1000 nits. They are built as OCIO `BuiltinTransform` or `FixedFunctionTransform`
    `ACES_OUTPUT_TRANSFORM_20` with direction inverse.
  - The required accuracy is a relative error of at most 1e-3 per AP0 channel, or an absolute
    error of 1e-5 for values below 1e-2. It holds for every grid point.
- **`Aces2InverseHostTablesMatchOcioWithinTolerance`**
  - This runs on the host only.
  - It compares the cusp, reach and gamma tables and the hue-linearity range against the values
    from OCIO's `init_GamutCompressParams`. It covers the warm-start upper-hull search.
- **`Aces2InverseReturnsBlackForBlackAndNeutralForSourceWhite`**
  - Display (0,0,0) maps to 0.
  - Source white maps to AP0 with `max/min − 1 < 1e-4`.
- **`Aces2InverseRuntimeIsBuiltOncePerPrimariesAndPeak`:** a host test of the cache.

---

## 6. Decode and develop graph for raster input

### 6.1 Decoder: `RasterInputLoader`

New `edit/input/raster_input_loader.cpp`, beside `RawInputLoader`.

- **Content classification** (`ClassifyImageContent(bytes)`):
  1. JPEG `FF D8 FF`.
  2. PNG `89 50 4E 47 0D 0A 1A 0A`.
  3. OpenEXR `76 2F 31 01`.
  4. TIFF `II*\0` or `MM\0*`. DNG, NEF, CR2, ARW and PEF are TIFF containers too. A file with
     TIFF magic is **RAW** if it has a `DNGVersion` tag, or if LibRaw opens it and reports a
     non-empty camera make together with a color matrix. Otherwise it is a **raster TIFF**.
  5. Anything else goes to LibRaw, as today (CR3, RAF, RW2 and others).
- **Pixel decode:**
  - **JPEG:** TurboJPEG `tj3Decompress8`. For `DecodeRes` EIGHTH, QUARTER and HALF it uses
    TurboJPEG scaling 1/8, 1/4 and 1/2. These are the existing reduced-resolution decode levels
    used for RAW thumbnails (`thumbnail_service.cpp:43-55`).
  - **PNG, TIFF, EXR:** OpenImageIO `ImageInput`, first subimage, at full size. Thumbnails are
    downscaled in linear light on the GPU in `GeometryResample`.
- **Host plane formats:**
  - Add `U8Rgba` and `U16Rgba` to `HostPixelFormat`, and `Rgba16u` to `TextureFormat`.
  - Integer files stay at their native depth until the GPU linearize step. A 24 MP JPEG holds
    96 MB rather than 384 MB as F32.
  - Float TIFF and EXR (half or float) use `F32Rgba`. LUT-ICC files converted by lcms2 also use
    `F32Rgba`.
- **Channels:**
  - Gray is copied to RGB.
  - Palette PNG is expanded by OpenImageIO.
  - **Alpha is discarded**, and color is rendered as stored (decision D6). EXR is not
    un-premultiplied.
- **Orientation:** EXIF `Orientation` (JPEG, TIFF, PNG `eXIf`) maps to the existing
  `RawSensorGeometry` flip. EXR uses identity.
- **Output:** a `PreparedRawInput` with `input_kind = RasterRgb`.
  - The color description comes from the document (section 7.3), not from the file.
  - `PreparedSourceKey` hashes its fingerprint.
  - Bump `kRawInputPreparationVersion` to 7. This is an in-memory cache key and is not persisted.
- **Dispatch:**
  - The default unpack function (`renderer.hpp:211-222`, `prepared_source_cache.cpp:14`) becomes
    `LoadEncodedImage(bytes, res)`. It classifies once and calls exactly one loader.
  - There is no retry with the other decoder. A failure reports that decoder's error.

### 6.2 Passes

- **New `DevelopInputKind::Raster`.**
  - `CompileDevelopPasses` builds `UploadRgb → Lens → GeometryResample → DisplayToAp1`.
    `DisplayToAp1` is a new `GpuPassKind` value, 16.
  - It never adds `Linearize`, `CfaClamp`, `Demosaic`, `HighlightRecover`, `InverseCamMulPack`
    or `CameraToAp1` for raster input.
- **`UploadRgb` for raster input:**
  - It uploads U8, U16 or F32 RGBA and runs `LinearizeRaster`, which applies the per-channel
    transfer function and writes F32 display-linear RGB in the source primaries. Scene-linear
    input passes through unchanged.
  - The transfer function is analytic for `Linear`, `SrgbPiecewise`, `Gamma`, `Bt1886`, ICC
    parametric, PQ and HLG. `IccSampled` uses a 4096-entry 1D buffer with linear interpolation.
  - The result is `sensor_linear_output`, which is cached, so lens correction and geometry
    resample **linear** values, as they do for RAW.
- **`DisplayToAp1`:** section 5.3 for display-referred input, section 5.5 for scene-linear
  input. It writes ACEScc AP1 to `develop_output`.
- **Validity key:**
  - Inputs: the description fingerprint, the inverse runtime key, the D5 override, and the
    existing geometry inputs.
  - RAW Develop white-balance fields are not part of the key.
- **Kernels:**
  - CUDA `edit/runtime/cuda/cuda_display_to_ap1_pass.cu`, with device code in
    `include/edit/runtime/cuda/drt/aces2_ocio_inverse.cuh`.
  - OpenCL `edit/runtime/opencl/shader/display_to_ap1.cl`, registered under the existing
    OpenCL 1.2 program rules.
  - Metal `edit/runtime/metal/shader/display_to_ap1.metal`.

### 6.3 Develop payload and the disabled RAW stages

- **`DevelopPayload` gains an optional `input` object** (section 7.3 defines its JSON).
  - When it is absent the document is RAW, so every existing document stays RAW.
  - When it is present the document is raster, and the following hold:
    - `ResolveDevelopColorTransform`, `BindDevelopCameraProfile`, `BindImportedCameraProfile`,
      `BindSourceDngColorProfile` and `LookupCameraColorMatrices` are never called.
      `CameraColorGpuParams` is not packed.
    - `ApplyColorTemperatureUpdate` and the `raw_decode`/`color_temp` field writes
      (`app/editor_parameter_write_parse.cpp:354-430`) reject the write with an explicit error.
      They are not no-ops.
    - `lens_enabled` stays editable and defaults to `false`, because camera JPEGs are normally
      corrected in camera.
- **CAT02 (Look panel) is unchanged.** `Cat02WhiteBalanceModel` works on linear AP1 inside the
  Color Grade and reads no RAW metadata. Its help text (`EditorLookPanel.qml:624`) already
  mentions JPEG use.

### 6.4 Removing the working-space camera profile hook

- `BindRgbWorkingSpaceCameraProfile`, `BindWorkingSpaceDevelopData` and the no-RAW-context branch
  of `EnsureRenderableCameraProfile` (`app/pipeline_root_state.cpp:67-93`,
  `app/pipeline_service.cpp:193-194`) existed for non-RAW RGB input.
- Import never produced such documents, because it was RAW-only. Before removing the hook, Phase
  R3 checks that no existing root can reach it: a scan of a sample of existing `.alcd` projects
  for roots that have `raw_color_context = null` and `camera_profile.color_matrices_valid =
  false`.
  - If none exist, the hook is removed. A RAW document whose root has no RAW context then becomes
    a load error that names the element.
  - If such roots exist, the hook stays for **RAW documents only**, and the R3 report records why.
- The `FromDirectRgb` tests that depend on the hook (`tests/edit/runtime/cuda_develop_test.cpp:151`,
  `cuda_drt_product_test.cpp`) move to an explicit raster `input` object with `Linear` transfer
  and Rec.709 primaries.

### 6.5 Default document for raster images (decision D2)

`CreateDefaultRasterPipelineDocument`:

- **Develop:** the `input` object (section 7.3), with `lens_enabled = false`.
- **Color Grade:** neutral (identity). The RAW default look is **not** applied: +1.5 EV, contrast
  15 and saturation 1.3 (`pipeline_document.hpp:165-169`).
- **DRT:**
  - Method: ACES 2.0.
  - Limiting space: the source primaries mapped to the nearest `DrtColorSpace`. sRGB/Rec.709 →
    Rec709; Display P3 → P3D65; Rec.2020, Adobe RGB and ProPhoto → Rec2020.
  - Peak: the source peak.
  - EOTF: the source transfer. This adds `DrtEotf::SrgbPiecewise`, serialized as the string
    `"srgb_piecewise"`. The kernels already have that curve as the moncurve branch of
    `eotf_inv`. The work is the enum value, the string, the UI entry, and the export ICC mapping.

### 6.6 Editor UI

- **`EditorRawDecodePanel.qml`:** for raster images it hides white balance, demosaic and
  highlight reconstruction.
- **Read-only Input color row:** shows the profile description and its origin. Examples:
  "Display P3 — embedded ICC", "sRGB — no profile, assumed", "Rec.709 linear — EXR
  chromaticities".
- **Input profile menu (decision D5):** the entries are Auto (from the file), sRGB, Display P3,
  Adobe RGB, Rec.2020, ProPhoto and Linear Rec.709.
  - The editor field key is `input_profile`.
  - It writes `input.profile_override` through a normal `PipelineEditBatch` field write, so it
    is undoable and goes into history.
  - It changes only the `DisplayToAp1` validity key.

---

## 7. Persistence and compatibility with existing projects

### 7.1 Rules

1. **No DDL.** There are no new tables or columns and no type changes. The plan uses only the
   existing `Image.type UINT32` value, the `Image.metadata` JSON column, and the root and
   checkpoint document JSON.
2. **No version constant changes.** Project file `0.10.0`, packed format 7, document format 7,
   image edit schema 5, commit 5, chain 5, edit batch 4, root 5 and checkpoint 5 all stay as they
   are. Every one of them is checked for equality without migration (section 2.5), so a bump
   would make every existing project fail to open.
3. **Existing data serializes byte for byte as today.**
   - Every new JSON key is **omitted** when it does not apply. A RAW document never writes
     `input`, and a RAW image's `metadata` never writes the raster key.
   - Existing RAW documents therefore keep their canonical dump, their `root_id` hash and their
     checkpoint labels.
4. **New keys go into objects that are already read leniently.**
   - `DevelopParamsModel::LoadJson` reads keys with defaults.
   - `ExifDisplayMetaData::FromJson` ignores keys it does not know.
   - The strict root decoder (`RequireExactObjectKeys`) is **not** given a new key.

### 7.2 `Image` row

- **`Image.type`**
  - Existing rows store `0` (`DEFAULT`) and keep it. `DEFAULT` means "RAW, or imported before
    this change".
  - New raster imports store `JPEG` (1), `PNG` (2) or `TIFF` (3). `EXR` is appended to the end
    of the enum as 9, so no existing integer changes meaning.
  - New RAW imports keep storing `DEFAULT`, so old and new RAW rows look the same.
  - **No code decides RAW versus raster from `Image.type`.** The document's Develop `input`
    object decides it (section 7.3). `Image.type` is only for display and filters.
- **`Image.metadata` JSON**
  - It gains an optional key `"RasterColorDescription"`, beside the existing
    `"RawRuntimeColorContext"`. The two are never both present.
  - The value is the JSON of section 7.3, written once at import, and read by the library
    inspector and the HDR flag.
  - `Image::ExifDisplayToJson` (`image/image.cpp:204-215`) erases this key, as it already erases
    the RAW key.
- **Search columns**
  - `file_ext`, `camera_make`, `capture_at`, `rating` and the others are filled from the
    display metadata as today, so raster images are searchable with no schema change.

### 7.3 Where the color description lives: the Develop `input` object

**The owner of the description that rendering uses is the image's root document.** It is stored
in the Develop node, which is where `camera_profile` already keeps per-image color data
(section 2.5). Because the root document is immutable and covered by `root_id`, a changed
description cannot appear silently in an existing history.

```json
"input": {
  "kind": "raster",
  "source_color": {
    "referral": "display_referred",
    "primaries_xy": [0.68, 0.32, 0.265, 0.69, 0.15, 0.06, 0.3127, 0.329],
    "transfer": [{"kind": "srgb_piecewise"}],
    "peak_luminance_nits": 100.0,
    "origin": "icc_matrix_shaper",
    "profile_description": "Display P3",
    "icc_sha256": "…"
  },
  "profile_override": "auto"
}
```

- **`transfer`** has one entry when all channels match and three when they differ. A sampled
  curve is stored as `{"kind":"icc_sampled","samples":[…4096 floats…]}`. That costs about 40 KB
  per affected image, in the root only; checkpoints carry the same document. Sampled curves are
  rare, because almost every real profile is parametric or gamma.
- **`icc_sha256`** is present only when an ICC was used. It is the profile hash that the
  LUT-based ICC decode checks (section 4.3).
- **`profile_override`** is the only editable field. Its values are `auto`, `srgb`,
  `display_p3`, `adobe_rgb`, `rec2020`, `prophoto` and `linear_rec709`. Edits to it go through
  history like any other Develop field.
- **Reading at render time:** the pipeline reads `input` from the document it already holds. The
  decoder does not parse the profile again, so the stored description cannot disagree with the
  file's.
  - If the source file is replaced on disk by a different image, the stored description still
    applies. This matches what RAW does today with its stored `camera_profile`.
  - A changed file is a separate, existing concern: file identity tracking.

### 7.4 Checks for existing projects

Phase R4 and Phase R3 include these tests:

- **`ExistingRawRootDocumentsSerializeByteIdenticalAfterRasterChange`**
  - Load the roots and checkpoints from the existing test projects (`tests/resources` `.alcd`
    fixtures and a project written by the current `main` build in the test setup).
  - Re-serialize them, and require byte-identical JSON and an unchanged `root_id`.
- **`ProjectWrittenByCurrentMainOpensAndRendersUnchanged`**
  - The fixture is a project packed by build `abdb000c8`, committed under
    `tests/resources/compat/`.
  - The test opens it, renders one element, and requires equal pixel hashes on CUDA.
- **`ImageRowsWithoutRasterKeyLoadAsRaw`**
  - `metadata` JSON without `"RasterColorDescription"` loads with `has_raster_description_ =
    false`, and `type` 0 keeps its value.
- **`RasterKeyIsOmittedFromRawImageMetadataJson`**
  - Covers the RAW `metadata` written after this change.

### 7.5 Older builds opening a project with raster images (accepted limitation)

- An older build reads the new Develop `input` object leniently and ignores it. It would then
  treat the image as RAW, find no RAW context, and fall into the working-space profile hook (if
  that build still has it) or fail to render.
- Older builds already reject raster files at import, so this only happens when a newer project
  is opened in an older build. That is a downgrade, which is not supported.
- The release notes state it.

---

## 8. Import and metadata

- **`ExtractEXIF_ToImage`:**
  - It classifies by content (section 6.1) and checks the result against
    `ImportOptions.allowed_categories_` (section 9). Then it branches:
    - **RAW:** unchanged.
    - **Raster:**
      - Display metadata uses the existing Exiv2 code (`GetDisplayMetadataFromExif`,
        `metadata_extractor.cpp:1623-1722`): make, model, lens, exposure, date and rating, with
        XMP before EXIF.
      - EXR reads OpenImageIO header attributes instead.
      - Dimensions come from the decoder header.
      - The color description is resolved (section 4.3), and `Image.type` and the
        `"RasterColorDescription"` metadata key are set.
- **`EncodeImageRoot`:** a raster overload builds the section 6.5 document with the `input`
  object and writes `raw_color_context = null`, using the existing root key.
- **HDR detection** (`metadata_extractor.cpp:1250-1272`) adds PQ or HLG raster input, and EXR, as
  HDR.
- **Thumbnails:** `ThumbnailService::DecodeRenditionSource`
  (`app/thumbnail_service.cpp:422-468`) calls `LoadEncodedImage`.
- **Export:**
  - Metadata copy (`image_writer.cpp:391`) already works for any source.
  - The **output** ICC always comes from `ExportIccProfileResolver`, never from the source file.
    An assertion test covers this.
- **Error reporting:**
  - A missing, unparseable or unusable color description is not an error. The file imports with
    the sRGB default, and the inspector shows `default_reason_`. This is the authorized default.
  - A truncated or corrupt pixel stream is an import failure (`failed_`).
  - CMYK is `UNSUPPORTED_FORMAT`.

---

## 9. Folder import with file-type selection

### 9.1 Types and classification

- **One source of truth.** `include/type/supported_file_type.hpp` is replaced by:

  ```cpp
  enum class ImportFileCategory : uint8_t { Raw, Jpeg, Tiff, Png, OpenExr, Other };
  auto CategoryForExtension(std::string_view ext) -> ImportFileCategory;  // case-insensitive
  using ImportCategoryMask = uint8_t;  // bit per category except Other
  ```

  `ImportFileCategory` is an `enum class` with explicit mask operators. Unscoped enumerators in
  `alcedo` collide with existing type names.
- **Extensions:**
  - **RAW:** `.3fr .arw .cr2 .cr3 .crw .dcr .dng .erf .fff .iiq .kdc .mef .mos .mrw .nef .nrw
    .orf .pef .raf .raw .rw2 .rwl .sr2 .srf .srw .x3f`. `docs/supported_raw_formats.md` is
    updated from the same table.
  - **JPEG:** `.jpg .jpeg .jpe .jfif`.
  - **TIFF:** `.tif .tiff`.
  - **PNG:** `.png`.
  - **OpenEXR:** `.exr`.
  - Matching is case-insensitive, which fixes the `.Nef` gap.
- **Content decides the type, and both checks apply.**
  - The extension decides which files appear in the scan list.
  - At metadata time, a file is imported only if its **content** category is in the allowed mask.
  - Example: a JPEG renamed `.nef`, with only RAW allowed, is reported as an excluded type.
  - This keeps the existing content-first rule and the test
    `ImportDecidesRawByContentNotByFileExtension`.
- **"Other" files are never imported.** They are counted and shown, and their checkbox is
  disabled.

### 9.2 Scan model

`FolderImportScanModel`:

- Stores one `uint8_t` category beside each path, plus a count per category.
- Exposes `categoryCounts` and `allowedCategories` (read and write) as `Q_PROPERTY`s, plus a
  filtered list view.
- A checkbox change rebuilds an index vector in O(n) on the UI thread, with no rescan. For a
  200k-file folder this takes at most 30 ms.
- `TakeFilePaths()` returns only paths in allowed categories.

### 9.3 Dialog

`FolderImportConfirmDialog.qml` adds a **File types** row:

- One checkbox per category, with its count. RAW, JPEG, TIFF, PNG and OpenEXR are on by default.
- **Other** shows its count and cannot be checked.
- The note at `:149` becomes "Files of other types are skipped."
- Import is enabled when the scan has finished and the allowed count is greater than 0.

### 9.4 Options and persistence (decision D7)

- **`ImportOptions` gains `allowed_categories_`.** `import_service.cpp:190` stops discarding
  `options`.
- **Folder import** passes the dialog selection.
- **The selection is remembered** in `QSettings` key `import/folderAllowedCategories`, with all
  five categories as the default. QSettings is the application settings store, not the project
  database, so rule 7.1 does not apply to it.
- **File-picker import** allows all five categories, because the user picked those files
  explicitly. Its `nameFilters` becomes `["Supported images (…all extensions…)", "All files (*)"]`.

### 9.5 Reporting

- `ImportProgress::unsupported_` is split into two counters. `ImportProgress` is an in-memory
  type, not stored data.
  - `excluded_type_`: the content category is not allowed.
  - `unsupported_`: the content is not decodable as any supported type, CMYK included.
- The overlay (`qml/ImportProgressOverlay.qml:140-160`) and the final status text
  (`import_export.cpp:694-696`) report both counts.
- **Translations:** edit the `.ts` files by hand. Do not run `lupdate`.

---

## 10. Test fixtures

A generator script writes the fixtures to `alcedo_studio/tests/resources/raster/`. Each name
states the file's contents:

- **JPEG:** `srgb_icc_8bit.jpg`, `display_p3_icc_8bit.jpg`, `adobe_rgb_exif_r03_no_icc.jpg`,
  `untagged_8bit.jpg`, `cmyk_icc.jpg` (must be rejected).
- **PNG:** `cicp_rec2020_pq_16bit.png`, `iccp_display_p3.png`, `srgb_chunk.png`,
  `gama_chrm_only.png`, `palette_untagged.png`, `gray_gamma22.png`, `rgba_with_alpha.png`.
- **TIFF:**
  - `rec2020_icc_16bit.tif`, `prophoto_icc_16bit.tif`, `float32_linear_icc.tif`,
    `float32_no_icc.tif`.
  - `uncompressed_rgb16_no_make.tif` must classify as raster, not RAW.
  - `lut_based_rgb_icc.tif` goes through the lcms2 conversion.
- **EXR:** `chromaticities_p3_half.exr`, `aces_container_flag.exr`, `no_chromaticities_float.exr`.
- **Alcedo exports:** a 16-bit TIFF exported with each `config/icc/*.icc` profile, which must
  re-import with the same description.
- **Compatibility:** `tests/resources/compat/main_abdb000c8_project.alcd` (section 7.4).

---

## 11. Phases

Each phase lists its work, the tests it adds, and its completion criteria. Tests are run per
target (`ctest -R`), never the full suite.

### Phase R1 — Source color description

- Work: `RasterColorDescription`, `ResolveRasterColorDescription`, the PNG chunk reader, the
  lcms2 ICC reader (link `lcms2::lcms2` to the image library), and its JSON (section 7.3).
- Tests (`RasterColorDescriptionTest`):
  - `IccMatrixShaperYieldsNativePrimariesAfterChadInverse`
  - `IccV44CicpTagOverridesTrc`
  - `PngCicpOverridesIccpAndSrgbChunk`
  - `PngGamaWithoutChrmUsesSrgbPrimaries`
  - `JpegExifR03WithoutIccResolvesAdobeRgb`
  - `ExrWithoutChromaticitiesResolvesRec709Linear`
  - `Float32TiffWithoutIccIsSceneLinear`
  - `LutBasedRgbIccResolvesToLinearRec2020Converted`
  - `CmykIccIsRejected`
  - `UnusableIccFallsBackToSrgbAndRecordsReason`
  - `ExportedIccProfilesReimportWithSamePrimariesAndTransfer`
  - `DescriptionJsonRoundTripsAndOmitsAbsentFields`
- Done when: every fixture in section 10 resolves to its documented description.

### Phase R2 — OCIO-reference inverse runtime

- Work:
  - The host port of OCIO's ACES2 init and tables, with the warm-start upper-hull search.
  - `ResolveAces2InverseRuntime`.
  - The device port to CUDA, OpenCL and Metal, and the `DisplayToAp1` kernels.
  - The `THIRD_PARTY_NOTICE.txt` entry.
- Tests: section 5.7, with OpenColorIO linked by the test target only.
- Done when:
  - The tests pass on CUDA and OpenCL on Windows, and on Metal on macOS.
  - The section 5.4 performance figures are measured and written into this document.

### Phase R3 — Decoder and graph

- Work:
  - `RasterInputLoader`, `ClassifyImageContent`, and the new host and texture formats.
  - `LinearizeRaster`, `DevelopInputKind::Raster`, the compiler branch, and the validity key.
  - The `DevelopPayload.input` model, the `input_profile` field write, and `DrtEotf::SrgbPiecewise`.
  - The audit and removal of the working-space profile hook (section 6.4).
- Tests:
  - `RasterJpegRendersThroughDisplayToAp1WithoutCameraToAp1Pass`: pass list.
  - `RasterDocumentRejectsColorTemperatureWrite`.
  - `RasterDevelopOutputIsCachedAcrossColorGradeEdits`: pass statistics show `DisplayToAp1`
    running once over two renders that differ only in a grade slider.
  - `InputProfileOverrideReRunsOnlyDisplayToAp1`.
  - `Cat02WhiteBalanceShiftsRasterImageLikeRawImage`.
  - `TiffMagicWithoutCameraMakeClassifiesAsRaster` and `DngClassifiesAsRaw`.
  - `RgbaPngRendersColorAndIgnoresAlpha`.
  - `ExistingRawRootDocumentsSerializeByteIdenticalAfterRasterChange` (section 7.4).
- Done when:
  - The tests pass on CUDA, OpenCL and Metal.
  - The RAW develop suites show no regression against a clean `HEAD`.

### Phase R4 — Import, metadata, thumbnails, export

- Work:
  - The `ExtractEXIF_ToImage` branch, `ImageType::EXR`, and the `"RasterColorDescription"`
    metadata key.
  - The raster root encoding, the default raster document, HDR detection, the thumbnail
    dispatch, and the export ICC assertion.
- Update the RAW-only tests from section 2.1 and rename them after the new behavior. Example:
  `MixedFolderImportsRawAndSelectedRasterTypesAndLeavesNoOrphanImageRows`.
- Tests:
  - `ImportedJpegStoresDescriptionInDevelopInputAndRatingFromXmp`
  - `ImportedExrIsSceneLinearAndHdr`
  - `ReexportedRasterUsesExportProfileNotSourceIcc`
  - `RasterThumbnailUsesScaledJpegDecode`
  - The rest of section 7.4.
- Done when these pass: `ImportServiceTest`, `ImportRawOnlyTest` (renamed
  `ImportContentClassificationTest`), `MetadataExtractorTest`, `ThumbnailServiceTest` (run
  directly) and `ExportServiceTest`, plus the compatibility tests.

### Phase R5 — Folder import file-type selection

- Work:
  - `ImportFileCategory`, plus categories and a filtered view in the scan model.
  - The dialog row, `ImportOptions.allowed_categories_`, and the QSettings key.
  - The picker filters, the split counters, the texts and the translations.
- Tests (`AlbumBackendImportTest`):
  - `FolderScanCountsFilesPerCategory`
  - `FolderImportImportsOnlyCheckedCategories`
  - `OtherCategoryCannotBeEnabled`
  - `RenamedJpegWithOnlyRawAllowedIsReportedAsExcludedType`
  - `AllowedCategoriesAreRestoredFromSettings`
  - A filter-rebuild benchmark over 200k synthetic paths.
- Done when the tests pass and the dialog is checked by hand. The offscreen QML harness is not a
  reliable check (`AGENTS.md`), so the report says the check was manual.

**Order:** R1 → R2 → R3 → R4. R5 needs only the R1 classification, so it can run in parallel with
R2 and R3.

---

## 12. Decisions (recorded 2026-10-04)

| ID | Decision |
|---|---|
| D1 | The inverse follows **OpenColorIO 2.5.1** (the Academy reference), not Alcedo's modified forward. A round trip through Alcedo's own forward does not have to be exact. |
| D2 | The raster default document has a **neutral** Color Grade, so the RAW default look is not applied. The DRT is ACES 2.0, with limiting space, peak and EOTF matched to the source, and `DrtEotf::SrgbPiecewise` is added. |
| D3 | OpenEXR, and float TIFF with a linear ICC curve or no ICC, are **scene-linear**: they get a matrix only and no inverse DRT. |
| D4 | LUT-based RGB ICC profiles are converted on the host by lcms2. CMYK is rejected at import. |
| D5 | The Develop panel has a manual **Input profile** override (`input.profile_override`, in history). |
| D6 | Alpha is **discarded**. |
| D7 | The folder-import file-type selection is **remembered** in `QSettings`. |
| — | **Compatibility:** no DDL, no version bumps, and existing data serializes byte for byte (section 7). |

---

## 13. Risks

| Risk | Consequence | Handling |
|---|---|---|
| LibRaw opens a plain TIFF as RAW | A scanned TIFF takes the RAW path and fails | Classification rule 4 (section 6.1), plus the `uncompressed_rgb16_no_make.tif` fixture |
| A source gamut is wider than AP1 (ProPhoto) | Colors outside AP1 are clamped to the reach boundary | Section 5.2. The inspector shows the profile |
| A clipped display white (255) maps to the top of the tonescale | Lowering exposure shows flat highlights | Inherent to inverting a display rendering |
| A future change makes a RAW document write the new `input` key or changes key order | `root_id` mismatch, and existing histories fail to load | The section 7.4 byte-identity test runs in R3 and R4 |
| The OCIO port drifts from OCIO, for example after an OCIO upgrade | The inverse no longer matches the reference | Section 5.7 tests against the linked OCIO version |
| `BT1886` decode uses 2.6 while encode uses 2.4 (`disp_enc_funcs.cuh:275-276`) | Existing output inconsistency | `LinearizeRaster` uses its own curve set. Fix that bug separately |
| Large float TIFF or EXR (≥ 100 MP) | High host memory peak as F32 | Integer TIFF stays U16. Report the peak in R3 |
| Older build opens a project with raster images | Raster images do not render in that build | Downgrade is not supported; the release notes say so (section 7.5) |
