# Windows Display ICC Color Management Plan

- Date: 2026-11-12
- Status: W1, W2, W3, W4, and W5 implemented (2026-11-12). W6 implemented (2026-11-13).
- Tracking issue: [#873](https://github.com/zidage/AlcedoStudio/issues/873) (W1 to W3, PR [#874](https://github.com/zidage/AlcedoStudio/pull/874)); W4 [#875](https://github.com/zidage/AlcedoStudio/issues/875), W5 [#876](https://github.com/zidage/AlcedoStudio/issues/876), W6 [#877](https://github.com/zidage/AlcedoStudio/issues/877)
- Source revision of the audit: `e7251164b` (`main`)
- Related plans:
  - [LUT Color Encoding Plan](../edit/lut_color_encoding_plan.md): this plan uses its gamut
    catalog and its transfer functions.
  - [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md):
    this plan extends its viewport composite.
  - [Editor Adjustment and Version Comparison Plan](editor_adjustment_comparison_plan.md): this
    plan changes the pixels that its comparison view shows on Windows.

## 1. Decision record

The user goal: on Windows, a calibrated monitor shows the colors of a photo correctly. The
application reads the ICC profile that Windows associates with the monitor and converts the photo
pixels to that profile. The output encoding of a photo is a user selection (primaries and
transfer curve), so the conversion uses the selected encoding as its source. HDR display output on
Windows stays out of this plan.

The user approved these decisions on 2026-11-12.

| Id | Decision |
| --- | --- |
| D1 | All photo surfaces get the conversion, in phases: the editor viewport, the comparison view, the LUT previews, and the thumbnails of the library grid, the filmstrip, and the search results. |
| D2 | The conversion is a strict ICC conversion for every monitor profile. A monitor with the default sRGB profile of Windows also gets it: Rec.709 gamma 2.2 pixels are encoded again with the sRGB curve. The tone of the shadows on such a monitor changes from the current release. The result equals the macOS preview and equals an exported file in a color-managed viewer. |
| D3 | The Settings page `LUTs and Color Management` gets one read-only tile with the profile of the monitor and the conversion state. The conversion has no switch. |
| D4 | The colors of the application controls (panels, text, icons) stay as they are. |
| D5 | ST 2084 and HLG output keeps its current behavior on Windows. |

Decisions of the plan author that follow the industry practice of display color management:

| Id | Decision |
| --- | --- |
| D6 | The monitor profile is the per-user default that the Windows color system reports (`GetICMProfileW` on the device context of the monitor). |
| D7 | The rendering intent is relative colorimetric with black point compensation. |
| D8 | The video card gamma table of the profile (`vcgt`) stays with the operating system. The application does not apply it. |
| D9 | A monitor with Windows Advanced Color active (HDR, or Auto Color Management) has sRGB as its destination. The Desktop Window Manager interprets SDR application pixels as sRGB and converts them to the panel. |
| D10 | A monitor without a profile has sRGB as its destination. This is the rule of the Windows color system. |
| D11 | A profile that the application cannot use (not readable, not RGB, no conversion to the device) gives no conversion. The log and the settings tile show the reason. The application does not replace the profile with a different one. |

## 2. Terms

| Term | Meaning |
| --- | --- |
| Encoding | The primaries, the white point, and the transfer curve of display-encoded pixels: `ViewerDisplayConfig` (`encoding_space`, `encoding_eotf`). |
| SDR encoding | An encoding whose `encoding_eotf` is other than `ST2084` and `HLG`. |
| Destination | The ICC profile that the conversion targets for one window: the monitor profile, or sRGB (D9, D10). |
| Display table | A 3D lookup table from the code values of one SDR encoding to the code values of one destination: `DisplayTransformLut`. |
| Photo surface | A screen area that shows rendered photo pixels. |

## 3. Product design specification

### 3.1 Behavior

1. Each photo surface on Windows shows SDR photo pixels after the conversion from their encoding
   to the destination of the window.
2. The destination follows the window. When the window moves to a different monitor, the photo
   surfaces show the pixels for the new monitor. When the user changes the profile of the monitor
   in Windows and comes back to the application, the photo surfaces show the pixels for the new
   profile.
3. The conversion applies to each SDR encoding that the DRT node offers: Rec.709, P3-D65, and
   Rec.2020 primaries with the linear, gamma 1.8, gamma 2.2, gamma 2.6, BT.1886, and sRGB curves.
4. A color outside the gamut of the monitor clips to the gamut surface per channel after the
   colorimetric conversion.
5. Export, scopes, the histogram, the automation commands, and the disk caches keep the pixels of
   the encoding. The conversion is a presentation step only.
6. macOS keeps its current behavior: ColorSync converts the tagged `CAMetalLayer`.

### 3.2 Settings tile

The tile is in `LutSettingsPanel.qml`, below `CameraProfileFolderTile`. It follows
`.agents/skills/alcedo-qml-ui/SKILL.md` and `DESIGN.md`.

```text
+-- Display profile ---------------------------------------------------+
|  DELL U2723QE Calibrated 2026-10                          Applied    |
|  C:\Windows\System32\spool\drivers\color\U2723QE_D65.icm             |
+----------------------------------------------------------------------+
```

| Element | Content |
| --- | --- |
| Title | `Display profile` |
| First line, left | The `desc` text of the destination. `sRGB` when the destination is sRGB by D9 or D10. |
| First line, right | The state: `Applied`, `Managed by Windows` (D9), `No profile, sRGB` (D10), `Not applied` (D11), or `No monitor` (the window has no monitor yet). |
| Second line | The file path of the monitor profile, in the data font. Empty when the monitor has no profile. |
| Alert badge | The reason of D11 (`AlertBadge`, DESIGN.md "Alert badges"). Hidden for the other states. |

The tile shows the monitor of the settings window. It has no button and no input. The tile is
visible on Windows only. The zh_CN strings are added by hand (`lupdate` is not run).

### 3.3 Failure behavior

| Failure | Result |
| --- | --- |
| The profile file cannot be read, or is not an RGB profile with a conversion to the device | No conversion on each photo surface of the window. One `qCWarning(editorLog)` line with the file path and the reason. The settings tile shows the reason. |
| LittleCMS cannot build the conversion | The same result as above. |
| The graphics API has no 3D texture | The viewport composite reports `ViewportCompositeOutcome::Failed` with its error text, as for other composite errors. |
| The window handle is not a window (Qt offscreen platform) | No conversion, no warning. Tests on the offscreen platform keep their pixels. |

## 4. Scope

### 4.1 Included work by module

| Module | Responsibility |
| --- | --- |
| `ui/edit_viewer` (new library `DisplayColorTransform`) | Build a display table and convert an 8-bit image with LittleCMS 2. No Qt Quick, no Windows API. |
| `ui/edit_viewer` (new library `MonitorColorProfile`, Windows) | Read the profile path and the Advanced Color state of the monitor of a window. |
| `ui/edit_viewer` (new class `WindowDisplayDestination`) | Own the destination of one `QWindow` and publish its changes. |
| `ui/editor_rhi` | Sample the display table in the viewport composite. Connect `EditorViewportItem` to the destination. |
| `ui/alcedo_main/album_backend` | Convert comparison images, LUT previews, and thumbnails at their image providers. Supply the settings tile data. |
| `app/thumbnail_service` | Report the encoding of each delivered thumbnail. |
| `ui/alcedo_main/qml` | The settings tile. |

### 4.2 Exclusions

| Item | Owner that keeps it |
| --- | --- |
| HDR display output on Windows (ST 2084, HLG, scRGB swap chain) | A later plan. `ColorManager::ApplyWindowColorSpace` keeps its Windows result. |
| Soft proofing of a print or export profile | No current owner. |
| A user selection of the monitor profile inside the application | The Windows color settings. |
| Color management of the application controls | D4. |
| Mask thumbnails (`MaskThumbnailImageProvider`) | They are grayscale coverage, not photo pixels. |
| Linux | No current display color owner. |

## 5. Current source audit

All paths are below `alcedo_studio/`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Viewer encoding | `ViewerDisplayConfig`, `src/include/ui/edit_viewer/frame_sink.hpp`; `ViewerDisplayConfigFromDrt`, `src/include/edit/runtime/drt_display.hpp` | The DRT payload gives `encoding_space` (Rec.709, Rec.2020, P3-D65) and `encoding_eotf` to each presented frame. | None. |
| Window color space | `ColorManager::ApplyWindowColorSpace`, `src/ui/edit_viewer/color_manager.mm` and `color_manager.cpp` | macOS tags the `CAMetalLayer`. The Windows file returns `false` and does nothing. | None in this plan. |
| Viewport item | `EditorViewportItem`, `src/ui/editor_rhi/editor_viewport_item.cpp` | `applyDisplayConfig()` runs on the GUI thread on an encoding change, on `QWindow::screenChanged`, and on a visibility change. On Windows it has no effect. | Read the display table from the destination and give it to the renderer. |
| Viewport composite | `ViewportCompositePass`, `src/ui/editor_rhi/viewport_composite_pass.cpp`; `shaders/editor_viewport.frag` | One draw selects the base or detail photo and applies the Mask coverage tint. The output has the photo encoding. | Sample the display table as the last step. |
| Transfer curves | `CeDecode`, `src/include/color/color_encoding_math.h` | One function decodes each transfer function id. `ColorUtils::EOTF` values equal the ids. | None. The source curve is sampled from it. |
| Primaries | `color::GamutPrimariesXy`, `src/include/color/color_encoding_catalog.hpp` | Gives `{Rx, Ry, Gx, Gy, Bx, By, Wx, Wy}` of each gamut. | None. |
| LittleCMS | Root `CMakeLists.txt` (`find_package(lcms2)`); users: `Image`, `EditInput` | LittleCMS 2 is a current dependency. `src/image/icc_profile_reader.cpp` reads ICC profiles of input files. | Link the new library to `lcms2::lcms2`. |
| Comparison view | `ConvertRenderedImageForComparison`, `src/ui/alcedo_main/album_backend/comparison_presentation_image.cpp`; `ComparisonImageStore` | Converts RGBA32F to an RGBA8 `QImage` and rejects HDR. `RenderedPipelineImage::display` holds the encoding. The automation command `render_commands.cpp` calls the same function for file output. | Convert at the store, not in the shared function. |
| LUT previews | `LutPreviewController`, `src/ui/alcedo_main/album_backend/lut_preview_controller.cpp` | Calls `ConvertRenderedImageForComparison`, then `LutPreviewImageStore::Put`. | Convert before `Put`. |
| Thumbnails | `ThumbnailService`, `src/app/thumbnail_service.cpp`; `thumbnail_manager.cpp`; `search_controller.cpp`; `ThumbnailImageStore` | The service delivers an RGBA8 buffer. The buffer and the store entry (`image`, `revision`) hold no encoding. | Report the encoding with the buffer and keep it in the store entry. |
| Settings page | `src/ui/alcedo_main/qml/LutSettingsPanel.qml` | The page `LUTs and Color Management` has the LUT library tiles and `CameraProfileFolderTile`. | Add the tile of section 3.2. |
| Tests | `tests/ui/viewport_coverage_composite_test.cpp` with `viewport_coverage_composite_test_support.hpp`; `tests/ui/editor_window_color_management_test.mm` (macOS) | The composite runs on the real QRhi of each backend. No test reads a monitor profile on Windows. | New tests per phase. |

The pixels of a thumbnail have the encoding of the DRT node of the rendered pipeline. The audit
step of W6 confirmed this in source for the render path and the disk cache path (see the W6
completion record).

The audit was written when draft files of `display_color_transform` and `monitor_color_profile`
existed in the working tree only. W1 and W2 registered, completed, and tested them (see their
completion records).

## 6. Target architecture

### 6.1 Owners

| Owner | Input | Output | Lifetime | Error surface |
| --- | --- | --- | --- | --- |
| `DisplayColorTransform` functions | An SDR `ViewerDisplayConfig`, the bytes of a destination profile | A `DisplayTransformLut`, or a converted RGBA8 image | Stateless | `std::invalid_argument` for an HDR encoding; `std::runtime_error` with the profile name for an unusable profile |
| `QueryMonitorColorProfile` | A native window handle | `MonitorColorProfile` (device name, profile path, Advanced Color state), or `std::nullopt` | Stateless | `std::nullopt` when the handle is not a window |
| `WindowDisplayDestination` | One `QWindow` | The current `DisplayDestination` and the signal `DestinationChanged` | A child object of the window; one for each window | `DisplayDestination::error` holds the reason of D11 |
| `ViewportCompositePass` | A shared display table | The converted draw | The QRhi of the render thread | `ViewportCompositeOutcome::Failed` |
| `EditorViewportItem` | Its encoding and the destination of its window | The display table of the renderer | The item | The log line of section 3.3 |
| Image stores (`ComparisonImageStore`, `LutPreviewImageStore`, `ThumbnailImageStore`) | Source pixels with their encoding, the destination | Converted pixels for `requestImage` | The store | Unconverted pixels and the log line when D11 applies |

### 6.2 `DisplayDestination`

`DisplayDestination` is an immutable value that `WindowDisplayDestination` publishes as
`std::shared_ptr<const DisplayDestination>`. It is the one record of what the window converts to.
It exists because the profile bytes come from a file that can change, and each consumer (the
viewport item on the GUI thread, the image providers on their request threads) must convert with
the same bytes that the settings tile names. A consumer holds the pointer for one conversion.

| Field | Content |
| --- | --- |
| `kind` | `MonitorProfile`, `SrgbAdvancedColor` (D9), `SrgbNoProfile` (D10), `Unusable` (D11), or `NoWindow` |
| `profile_bytes` | The ICC bytes of the destination. Empty for `Unusable` and `NoWindow`. |
| `description` | The `desc` text of the profile |
| `profile_path` | The file of the monitor profile |
| `error` | The reason, for `Unusable` |

`WindowDisplayDestination` reads the monitor again on `QWindow::screenChanged`, on
`QGuiApplication::applicationStateChanged` to `Qt::ApplicationActive`, and on the first exposure of
the window. It compares the device name, the profile path, the last-write time and the size of the
file, and the Advanced Color state with the values of the last read. It publishes a new
`DisplayDestination` and emits `DestinationChanged` only when one of them differs. All of this runs
on the GUI thread, in sequence. The class needs no version counter.

### 6.3 Data flow of the viewport

```text
QWindow::screenChanged / application active / first exposure        (GUI thread)
  -> WindowDisplayDestination::Refresh
     -> QueryMonitorColorProfile(winId)
     -> read the profile file, validate with OpenDisplayProfile
     -> publish DisplayDestination, emit DestinationChanged
  -> EditorViewportItem::applyDisplayConfig
     -> BuildDisplayTransformLut(displayConfig(), destination->profile_bytes)
     -> store shared_ptr<const DisplayTransformLut> under mutex_, update()
EditorViewportRenderer::synchronize                                  (render thread, GUI blocked)
  -> composite_.SetDisplayTransform(item_->displayTransform())
EditorViewportRenderer::render
  -> ViewportCompositePass::Record
     -> upload the table to the 3D texture when the pointer differs from the uploaded one
     -> draw: photo selection -> coverage tint -> display table
```

The item builds a table when its encoding or the destination changes. `setDisplayConfig` returns
early for an equal encoding, so a frame with the same encoding builds nothing.

### 6.4 Data flow of an image provider

```text
producer (worker thread): store->Put(key, source QImage, ViewerDisplayConfig)
QML Image -> provider.requestImage(id)                               (image provider thread)
  -> store->Get(key): source pixels and encoding
  -> ConvertImageToDisplay(image, encoding, *destination)            (HDR encoding: no change)
  -> return the converted QImage
WindowDisplayDestination::DestinationChanged                         (GUI thread)
  -> store->SetDestination(destination)
  -> the owner of the URLs publishes new URLs through its current notification
```

Each store keeps the source pixels and their encoding. The converted pixels are a product of the
request and are not kept. A new destination therefore needs no second render.

### 6.5 Display table

| Property | Value |
| --- | --- |
| Nodes per axis | 65 (`kDisplayTransformLutEdge`) |
| Node layout | `rgba[((b * edge + g) * edge + r) * 4]`, alpha 1 |
| Input coordinate | `clamp(code, 0, 1) ^ input_exponent`; `input_exponent` is 1, and 1 / 2.2 for the linear curve |
| Source profile | `cmsCreateRGBProfile` with the primaries and the white of the gamut and a 4096-entry curve sampled from `CeDecode` |
| Conversion | `cmsCreateTransform`, `TYPE_RGB_FLT` to `TYPE_RGB_FLT`, `INTENT_RELATIVE_COLORIMETRIC`, `cmsFLAGS_BLACKPOINTCOMPENSATION` |
| Texture | `QRhiTexture::RGBA32F` with `QRhiTexture::ThreeDimensional`, linear filter, clamp to edge on three axes |
| Shader sampling | `texture(lut, (coordinate * (edge - 1) + 0.5) / edge)` |

## 7. File and API map

Current files that change (below `alcedo_studio/`):

- `src/ui/alcedo_main/CMakeLists.txt` or `src/ui/editor_rhi/CMakeLists.txt` (library registration)
- `src/ui/editor_rhi/shaders/editor_viewport.frag`
- `src/include/ui/editor_rhi/viewport_composite_pass.hpp`, `src/ui/editor_rhi/viewport_composite_pass.cpp`
- `src/include/ui/editor_rhi/editor_viewport_item.hpp`, `src/ui/editor_rhi/editor_viewport_item.cpp`
- `src/ui/editor_rhi/editor_viewport_renderer.cpp`
- `src/ui/alcedo_main/album_backend/comparison_image_provider.cpp`, `lut_preview_controller.cpp`,
  `lut_preview_image_provider.cpp`, `thumbnail_image_provider.cpp`, `thumbnail_manager.cpp`,
  `search_controller.cpp`, with their headers
- `src/app/thumbnail_service.cpp` and `src/include/app/thumbnail_service.hpp`
- `src/ui/alcedo_main/qml/LutSettingsPanel.qml`, the `.ts` files
- `tests/ui/CMakeLists.txt`, `tests/ui/viewport_coverage_composite_test.cpp`

Proposed files (below `alcedo_studio/`):

- `src/include/ui/edit_viewer/display_color_transform.hpp`, `src/ui/edit_viewer/display_color_transform.cpp` (draft exists)
- `src/include/ui/edit_viewer/monitor_color_profile.hpp`, `src/ui/edit_viewer/monitor_color_profile_win.cpp` (draft exists)
- `src/include/ui/edit_viewer/window_display_destination.hpp`, `src/ui/edit_viewer/window_display_destination.cpp`
- `src/ui/alcedo_main/qml/DisplayProfileTile.qml`
- `src/include/ui/alcedo_main/album_backend/display_profile_readout.hpp` and its `.cpp`
- `tests/ui/display_color_transform_test.cpp`, `tests/ui/monitor_color_profile_test.cpp`,
  `tests/ui/window_display_destination_test.cpp`, `tests/ui/display_profile_tile_qml_test.cpp`

Proposed APIs:

| API | Input | Output | Changes state | Error result |
| --- | --- | --- | --- | --- |
| `HasDisplayTransform(config)` | `ViewerDisplayConfig` | `bool`: SDR encoding | No | None |
| `SrgbDisplayProfile()` | None | ICC bytes of sRGB | No | `std::runtime_error` |
| `BuildDisplayTransformLut(source, display_profile, edge)` | Encoding, ICC bytes, nodes per axis | `DisplayTransformLut` by value | No | Section 6.1 |
| `ConvertImageToDisplay(image, source, display_profile)` | RGBA8 `QImage` by reference, encoding, ICC bytes | The image, converted in place | The image only | Section 6.1 |
| `DisplayImageTransform(source, display_profile)`, `Apply(image)` (W6) | Encoding, ICC bytes; then an RGBA8 `QImage` by reference on any thread | A prepared conversion; `Apply` converts the image in place with the pixels of `ConvertImageToDisplay` | The image only | Section 6.1, at construction; `std::invalid_argument` for another image format |
| `QueryMonitorColorProfile(native_window)` | `HWND` as `void*` | `std::optional<MonitorColorProfile>` | No | `std::nullopt` |
| `WindowDisplayDestination::ForWindow(window)` | `QWindow*` | The child object of the window; created on the first call | Creates the child | None |
| `WindowDisplayDestination::Destination()` | None | `std::shared_ptr<const DisplayDestination>` | No | `kind` and `error` |
| `ViewportCompositePass::SetDisplayTransform(lut)` | `std::shared_ptr<const DisplayTransformLut>` or null | None | The table of the next draws | `Record` reports `Failed` |
| `EditorViewportItem::displayTransform()` | None | `std::shared_ptr<const DisplayTransformLut>` | No | Null when there is no conversion |

## 8. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| W1 | Display table and image conversion library with tests | `ui/edit_viewer`, `tests/ui` | None | 500 to 700 | done |
| W2 | Monitor profile query and window destination with tests | `ui/edit_viewer`, `tests/ui` | W1 | 550 to 750 | done |
| W3 | Converted editor viewport | `ui/editor_rhi`, `tests/ui` | W1, W2 | 600 to 850 | done |
| W4 | Settings tile | `ui/alcedo_main` | W2 | 350 to 500 | done |
| W5 | Converted comparison view and LUT previews | `ui/alcedo_main/album_backend` | W1, W2 | 450 to 700 | done |
| W6 | Converted thumbnails | `app/thumbnail_service`, `ui/alcedo_main/album_backend` | W1, W2 | 700 to 1100 | done |

W5 and W6 are separate because W6 changes an app-layer result type and three thumbnail producers.

## 9. Detailed phases

Before each phase, read `AGENTS.md` and the applicable skills again
(`alcedo-msvc-cmake`, `alcedo-qml-ui`, `execute-phase-plan`).

### W1. Display table and image conversion library

**Objective and deliverables.** A static library `DisplayColorTransform` that builds a display
table and converts an RGBA8 image. A test executable proves the conversion against analytic
values.

**Inputs and prerequisites.** The two draft files of the library. `lcms2::lcms2`,
`ColorEncodingCatalog`.

**Modules, files, and APIs.**

- `display_color_transform.hpp` / `.cpp`: `DisplayTransformLut`, `HasDisplayTransform`,
  `SrgbDisplayProfile`, `BuildDisplayTransformLut`, and the new `ConvertImageToDisplay`.
- `src/ui/alcedo_main/CMakeLists.txt`: `def_library(DisplayColorTransform ...)` with
  `PUBLIC_DEPS Qt6::Gui opencv_core ColorEncodingCatalog` and `PRIVATE_DEPS lcms2::lcms2`.
- `tests/ui/display_color_transform_test.cpp` (proposed) and its registration.

**Data rules and invariants.**

- A table has `edge * edge * edge * 4` values in [0, 1]. Each alpha is 1.
- The functions hold no state and can run on any thread.
- The source profile comes from the gamut catalog and `CeDecode` only. No bundled export profile
  is read.
- `ConvertImageToDisplay` accepts `QImage::Format_RGBA8888` only and keeps the alpha values.

**Implementation steps.**

1. Register the library. Build it.
2. Add `ConvertImageToDisplay`: build the same source profile, create a
   `TYPE_RGBA_8` to `TYPE_RGBA_8` conversion with `cmsFLAGS_COPY_ALPHA` and the intent of D7, and
   convert each row in place. Throw `std::invalid_argument` for a different image format.
3. Share the source profile and the destination validation between the two functions.
4. Write the tests below.

**Primary success call chain.**
`BuildDisplayTransformLut -> OpenDisplayProfile -> BuildSourceProfile -> cmsCreateTransform -> cmsDoTransform per row -> DisplayTransformLut`

**Primary failure chain.**
`OpenDisplayProfile` cannot parse the bytes `-> std::runtime_error("... cannot be parsed")`. No
table exists. The caller keeps its previous state.

**Tests and evidence** (`DisplayColorTransformTest`).

| Test | Assertion |
| --- | --- |
| `SameEncodingAndDisplayProfileGiveIdentityTable` | Rec.709 gamma 2.2 to a LittleCMS profile with the same primaries and curve: each node equals its input within 5e-4. |
| `Rec709OnP3DisplayEqualsMatrixConversion` | Rec.709 gamma 2.2 to a P3-D65 gamma 2.2 profile: each node equals `encode(M * decode(code))` with `color::GamutConversionMatrix` within 1e-3. |
| `P3RedOutsideSrgbDisplayClipsPerChannel` | P3-D65 code (1, 0, 0) to sRGB gives (1, 0, 0). |
| `LinearEncodingUsesInputExponent` | A linear Rec.709 table has `input_exponent` 1 / 2.2, and on a Rec.709 gamma 2.2 display each node equals its coordinate within 5e-4. |
| `SrgbCurveOnSrgbDisplayIsIdentity` | Rec.709 with the sRGB curve to `SrgbDisplayProfile()` is the identity within 5e-4. |
| `Gamma22OnSrgbDisplayEqualsCurveConversion` | Rec.709 gamma 2.2 to sRGB: each gray node equals `CeSrgbEncode(code ^ 2.2)` within 1e-3 (D2). |
| `HdrEncodingIsRejected` | ST 2084 and HLG throw `std::invalid_argument`; `HasDisplayTransform` is false. |
| `UnusableProfileReportsItsReason` | Empty bytes, random bytes, and a CMYK profile each throw `std::runtime_error` with a different message. |
| `ConvertedImageEqualsTableAtNodeColors` | An RGBA8 image of table node colors equals the table values within 1 / 255 after `ConvertImageToDisplay`; alpha is unchanged. |

**Build and run commands** (PowerShell, repository root).

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DisplayColorTransformTest --parallel 4
$env:PATH = "build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "DisplayColorTransformTest" --output-on-failure
```

**Exit criteria.**

- [x] The nine tests pass on Windows.
- [x] The library builds on macOS (`macos_debug`).
- [x] The time of one 65-node table in the debug build is recorded.

**Expected diff.** 500 to 700 lines.

**Completion record.**

```text
Phase / date / status: W1 / 2026-11-12 / done
Source revision and branch: e7251164b (main) + feature/windows-display-icc-color-management
Actual changed modules: ui/edit_viewer (DisplayColorTransform, src/ui/alcedo_main/CMakeLists.txt),
  tests/ui (DisplayColorTransformTest)
Implemented behavior: DisplayTransformLut, HasDisplayTransform, SrgbDisplayProfile,
  BuildDisplayTransformLut, ConvertImageToDisplay (RGBA8888, in place, cmsFLAGS_COPY_ALPHA).
  Both conversions share BuildSourceProfile, OpenDisplayProfile, and one
  CreateDisplayConversion with the intent and flags of D7.
Explicitly unimplemented items: none
Primary success call chain: BuildDisplayTransformLut -> CreateDisplayConversion ->
  BuildSourceProfile (CeDecode curve, catalog primaries) + OpenDisplayProfile ->
  cmsCreateTransform(TYPE_RGB_FLT) -> cmsDoTransform per row -> clamp -> DisplayTransformLut
Primary failure and restore call chain: OpenDisplayProfile -> std::runtime_error ("is empty",
  "cannot be parsed", "is not an RGB profile", "has no conversion to the device"); no table
  exists, the caller keeps its state. HDR encoding -> std::invalid_argument.
Build and test commands with exit codes: build --target DisplayColorTransformTest (0);
  ctest -R DisplayColorTransformTest (0)
Discovered / passed / failed / skipped counts: 9 / 9 / 0 / 0
Manual verification: none required
Performance or resource evidence: one 65-node table: 115 to 132 ms in the debug build,
  43 to 45 ms in the release build (3 runs of DisplayColorTransformTest built in build/release).
  The release value is below the 50 ms limit of section 12.
Evidence path: build/tmp/display_icc/ (removed at the end of the task)
Remaining defects or unavailable platforms: none
```

Change against the plan: the identity and matrix tests compare each channel in code value
(5e-4 and 1e-3), or, on a display with a pure power curve, in linear light within 8.1e-5. A
stored ICC profile rounds each s15Fixed16 colorant by up to 7.6e-6, and the XYZ to RGB matrix of
the display multiplies that by its row sum (about 5.3) for each of the two other full channels. A
channel that is analytically 0 next to full channels is then up to 3.2e-5 in linear light, which
the pure power curve encodes as 0.013 because its slope at 0 is infinite. The tolerance in code
value alone failed only at such channels.

### W2. Monitor profile query and window destination

**Objective and deliverables.** `QueryMonitorColorProfile` on Windows and the
`WindowDisplayDestination` object of a window. A reviewer can read the destination of the primary
monitor in a test log.

**Inputs and prerequisites.** W1. The two draft files of the query.

**Modules, files, and APIs.**

- `monitor_color_profile.hpp` / `monitor_color_profile_win.cpp`: registered for `WIN32` only.
- `window_display_destination.hpp` / `.cpp` (proposed): `DisplayDestination`,
  `WindowDisplayDestination` (a `QObject`). The Windows part calls the query. On other platforms
  the destination is `NoWindow` and the class emits nothing.
- A function `ResolveDisplayDestination(const MonitorColorProfile&, read_file)` separate from the
  window, so that tests give it a profile state without a monitor.

**Data rules and invariants.**

- One `WindowDisplayDestination` for each `QWindow`: `ForWindow` returns the existing child.
- `Destination()` never returns null.
- The published value is immutable. A new read publishes a new value.
- All methods run on the GUI thread. `Destination()` returns a `shared_ptr` copy that other
  threads can hold.

**Implementation steps.**

1. Register `MonitorColorProfile` for Windows and build it.
2. Implement `ResolveDisplayDestination` with the rules D9, D10, D11. Validate the file bytes with
   `BuildDisplayTransformLut(ViewerDisplayConfig{}, bytes, 2)`; store its error text.
3. Implement `WindowDisplayDestination` with the three triggers of section 6.2 and the change
   comparison.
4. Log each published destination once with `qCInfo(editorLog)`: device name, kind, description.
5. Write the tests below.

**Primary success call chain.** Section 6.3, first block.

**Primary failure chain.**
`read_file` fails `-> DisplayDestination{kind = Unusable, error = "cannot read <path>"} -> DestinationChanged -> consumers remove their conversion`.

**Tests and evidence.**

| Test | Assertion |
| --- | --- |
| `MonitorColorProfileTest.PrimaryMonitorReportsADeviceName` | `QueryMonitorColorProfile(GetDesktopWindow())` has a value with a device name. When `profile_path` is set, the file exists. |
| `MonitorColorProfileTest.HandleThatIsNoWindowHasNoProfile` | Null and the value 1 give `std::nullopt`. |
| `WindowDisplayDestinationTest.AdvancedColorMonitorTargetsSrgb` | `advanced_color_active` gives `SrgbAdvancedColor` with the bytes of `SrgbDisplayProfile()`. |
| `WindowDisplayDestinationTest.MonitorWithoutProfileTargetsSrgb` | An empty path gives `SrgbNoProfile`. |
| `WindowDisplayDestinationTest.UnreadableProfileIsUnusableWithReason` | A path to a missing file and a path to a text file each give `Unusable`, empty bytes, and an error that names the path. |
| `WindowDisplayDestinationTest.ValidProfileIsTheDestination` | A profile file written by the test gives `MonitorProfile` with equal bytes and the `desc` text. |
| `WindowDisplayDestinationTest.EqualMonitorStatePublishesNothing` | Two refreshes with the same state emit `DestinationChanged` once. |
| `WindowDisplayDestinationTest.OffscreenWindowHasNoDestination` | A window of the offscreen platform gives `NoWindow` and no warning. |

**Build and run commands.** As W1 with the targets `MonitorColorProfileTest` and
`WindowDisplayDestinationTest`.

**Exit criteria.**

- [x] The eight tests pass on Windows.
- [ ] The log of a manual run names the profile of each monitor of the development machine. The
      test log names the primary monitor (below). The application writes one
      `[DisplayProfile]` line in `alcedo.editor` for each published destination; the run of the
      application on each monitor is the manual check of the user.

**Expected diff.** 550 to 750 lines.

**Completion record.**

```text
Phase / date / status: W2 / 2026-11-12 / done (manual multi-monitor log open, see exit criteria)
Source revision and branch: e7251164b (main) + feature/windows-display-icc-color-management
Actual changed modules: ui/edit_viewer (WindowDisplayDestination library with
  monitor_color_profile_win.cpp on WIN32), tests/ui (WindowDisplayDestinationTest,
  MonitorColorProfileTest)
Implemented behavior: QueryMonitorColorProfile (MonitorFromWindow, GetICMProfileW on the
  monitor DC, DISPLAYCONFIG advanced color state), ResolveDisplayDestination (D9, D10, D11),
  DisplayDestinationTracker (change comparison of section 6.2), WindowDisplayDestination
  (ForWindow child object, first exposure, screenChanged, application active, info and
  warning log lines).
Explicitly unimplemented items: none
Primary success call chain: QWindow expose / screenChanged / ApplicationActive ->
  WindowDisplayDestination::Refresh -> QueryMonitorColorProfile(winId) ->
  DisplayDestinationTracker::Update -> ResolveDisplayDestination -> read file ->
  BuildDisplayTransformLut(..., 2) -> publish shared_ptr<const DisplayDestination> ->
  DestinationChanged
Primary failure and restore call chain: file cannot be read or validated ->
  DisplayDestination{Unusable, error names the path} -> qCWarning(editorLog) ->
  DestinationChanged -> consumers have no bytes and remove their conversion
Build and test commands with exit codes: build --target WindowDisplayDestinationTest
  MonitorColorProfileTest (0); ctest -R "WindowDisplayDestinationTest|MonitorColorProfileTest" (0)
Discovered / passed / failed / skipped counts: 8 / 8 / 0 / 0
Manual verification: see exit criteria
Performance or resource evidence: not required
Evidence path: build/tmp/display_icc/ (removed at the end of the task)
Remaining defects or unavailable platforms: none
```

The primary monitor of the development machine in `MonitorColorProfileTest`:
`\\.\DISPLAY2`, `C:\WINDOWS\system32\spool\drivers\color\sRGB.icc`, Advanced Color inactive,
destination `MonitorProfile 'sRGB IEC61966-2.1'`.

Changes against the plan:

- `ResolveDisplayDestination(const MonitorColorProfile&)` reads the profile file itself and has no
  `read_file` parameter. The tests write real files in a temporary directory, so a replaceable
  reader has no caller.
- The change comparison is the class `DisplayDestinationTracker`, owned by
  `WindowDisplayDestination`. `EqualMonitorStatePublishesNothing` drives it with monitor states,
  because the offscreen test platform has no monitor. The test also replaces the file at the same
  path (a new calibration) and expects a new destination.
- `Refresh` queries the monitor only on the `windows` QPA and only when the window has its native
  window (`QWindow::handle()`). `winId()` of the offscreen platform is a counter, not an `HWND`,
  and `winId()` on a window without a native window creates it.

### W3. Converted editor viewport

**Objective and deliverables.** The editor viewport on Windows shows SDR frames converted to the
destination.

**Inputs and prerequisites.** W1, W2.

**Modules, files, and APIs.**

- `editor_viewport.frag`: uniform `vec4 displayLut` (x: applied, y: nodes per axis, z: input
  exponent) at the end of `ViewerUniforms`; `sampler3D displayLutTexture` at binding 5; the
  sampling of section 6.5 after the coverage tint.
- `ViewportCompositeUniforms`: the field `display_lut`.
- `ViewportCompositePass`: `SetDisplayTransform`, a 3D texture, its sampler, and the upload in
  `Record`. Without a table the pass binds a 2-node texture and the shader does not sample it.
- `EditorViewportItem`: `displayTransform()`, and a connection to
  `WindowDisplayDestination::DestinationChanged` in `attachWindow`.
- `EditorViewportRenderer::synchronize`: `composite_.SetDisplayTransform(item_->displayTransform())`.

**Data rules and invariants.**

- The table of the item is null when the encoding is HDR, when the destination kind is `Unusable`
  or `NoWindow`, and on platforms other than Windows.
- The coverage tint has the photo encoding and is converted with the photo.
- The pass uploads a table once: it compares the shared pointer with the uploaded one.
- A change of the node count creates the texture again and drops the shader resource bindings.
- `windowColorSpaceApplied` is true on Windows while the item has a table.
- The pixels that scopes, export, and the comparison render read stay unconverted: they do not
  come from the composite.

**Implementation steps.**

1. Change the shader, the uniform struct, and the pass. The static assertion or comment that ties
   `ViewportCompositeUniforms` to the std140 block gets the new field.
2. In `applyDisplayConfig`, on Windows: read the destination, build the table for the current
   encoding when the encoding or the destination differs from the ones of the current table,
   store it under `mutex_`, call `update()`.
3. Catch the `std::runtime_error` of the build, write the warning of section 3.3 once for that
   destination, and store a null table.
4. Connect `DestinationChanged` to `applyDisplayConfig`. Disconnect it in `detachWindow`.
5. Copy the table pointer in `synchronize`.
6. Extend the composite tests.

**Primary success call chain.** Section 6.3.

**Primary failure chain.**
`BuildDisplayTransformLut` throws `-> applyDisplayConfig logs the reason -> null table -> the composite draws the encoding pixels`.
`rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures)` is false
`-> Record returns Failed -> the item status text shows the error`.

**Tests and evidence.**

| Test | Assertion |
| --- | --- |
| `ViewportCoverageCompositeTest.DisplayTableConvertsThePhotoAtNodeColors` | A photo texture of node colors with a known non-identity table: each drawn pixel equals the table node within 1e-4, on CUDA and OpenCL. |
| `ViewportCoverageCompositeTest.DisplayTableInterpolatesBetweenNodes` | Colors between nodes equal a host trilinear sample of the table within 1e-3. |
| `ViewportCoverageCompositeTest.DisplayTableConvertsTheCoverageTint` | A covered pixel equals the table sample of the tinted value. |
| `ViewportCoverageCompositeTest.NoDisplayTableKeepsPhotoValues` | Without a table the drawn pixels equal the photo, including values above 1 (the current assertion stays). |
| `ViewportCoverageCompositeTest.ReplacedDisplayTableIsUploaded` | A second table with a different node count changes the drawn pixels to the second table. |
| `ViewportCoverageCompositeTest.LinearEncodingTableUsesInputExponent` | With `input_exponent` 1 / 2.2 the drawn pixel equals the node at `code ^ (1 / 2.2)`. |

The macOS run of `ViewportCoverageCompositeTest.Metal.*` proves that the shader with the new
binding compiles and that the no-table draw is unchanged.

**Manual verification (user).** On a wide-gamut calibrated monitor, a Rec.709 photo in the editor
and its exported file in a color-managed viewer look the same. A move of the window to a second
monitor changes the viewport within one frame after the move ends.

**Build and run commands.**

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ViewportCoverageCompositeTest alcedo_main --parallel 4
ctest --test-dir build/debug -R "ViewportCoverageCompositeTest" -j 1 --output-on-failure
```

**Exit criteria.**

- [x] The six tests pass on CUDA and OpenCL.
- [x] The current tests of `ViewportCoverageCompositeTest` pass on Windows and on Metal.
- [x] The log of the application names the destination and the table build time: the
      `[DisplayProfile]` lines of `WindowDisplayDestination::Refresh` and
      `EditorViewportItem::updateDisplayTransform` (code inspection; the application was built,
      not run).
- [ ] The user records the manual verification.

**Expected diff.** 600 to 850 lines.

**Completion record.**

```text
Phase / date / status: W3 / 2026-11-12 / done (manual verification of the user open)
Source revision and branch: e7251164b (main) + feature/windows-display-icc-color-management
Actual changed modules: ui/editor_rhi (editor_viewport.frag/.vert, ViewportCompositePass,
  EditorViewportItem, EditorViewportRenderer, CMake), tests/ui (ViewportCoverageCompositeTest)
Implemented behavior: uniform displayLut and sampler3D displayLutTexture (binding 5); the pass
  owns a 3D RGBA32F texture (2 nodes while there is no table), uploads a table once per
  pointer, creates the texture again for a new node count, and writes display_lut from the
  uploaded table. On Windows the item builds the table for its encoding and the destination of
  its window, follows DestinationChanged, and reports windowColorSpaceApplied while it has a
  table. synchronize() copies the table pointer to the pass.
Explicitly unimplemented items: none
Primary success call chain: DestinationChanged / setDisplayConfig / screenChanged ->
  EditorViewportItem::applyDisplayConfig -> updateDisplayTransform ->
  BuildDisplayTransformLut -> display_transform_ (mutex_) -> update() ->
  EditorViewportRenderer::synchronize -> ViewportCompositePass::SetDisplayTransform ->
  render -> Record -> EnsureDisplayTable (upload) -> draw: photo -> tint -> table
Primary failure and restore call chain: BuildDisplayTransformLut throws -> one qCWarning for
  that destination -> null table -> the composite draws the encoding pixels. No 3D texture or
  a table without a value for each node -> Record returns Failed -> status text names it.
Build and test commands with exit codes: Windows build --target ViewportCoverageCompositeTest
  MaskRangeCoverageProbeTest EditorViewportSuspensionTest (0), alcedo_main (0);
  ctest -R "ViewportCoverageCompositeTest|MaskRangeCoverageProbeTest|EditorViewportSuspensionTest"
  -j 1 (0). macOS macos_debug: build ViewportCoverageCompositeTest DisplayColorTransformTest
  WindowDisplayDestinationTest (0); ctest -R the same three (0).
Discovered / passed / failed / skipped counts: Windows: ViewportCoverageCompositeTest CUDA 16/16,
  OpenCL 16/16, MaskRangeCoverageProbeTest and EditorViewportSuspensionTest all passed, 0 skipped.
  macOS: 31 / 31 / 0 / 0 (16 Metal composite tests, 9 W1, 6 W2).
Manual verification: open (user): a Rec.709 photo in the editor and its export in a
  color-managed viewer on a wide-gamut calibrated monitor; a window move to a second monitor.
Performance or resource evidence: the table build time of W1; one 4.4 MB upload per table
Evidence path: build/tmp/display_icc/ (removed at the end of the task)
Remaining defects or unavailable platforms: none
```

Changes against the plan:

- The six tests are in the fixture `ViewportDisplayTableTest` (a subclass of
  `ViewportCoverageCompositeTest` in the same executable), so their full names are
  `ViewportCoverageCompositeTest.<Backend>.ViewportDisplayTableTest.<Name>`.
- `ViewportCompositePass::Record` writes `display_lut` from the table that it uploaded, and does
  not take it from the caller. The uniform and the bound texture then cannot disagree.
- The tables of the composite tests are synthetic and non-linear (the expected values are the
  table nodes and a host trilinear sample), so that the composite tests do not depend on
  LittleCMS. W1 covers the LittleCMS tables.
- The item creates `WindowDisplayDestination` on Windows only. On macOS the item keeps its
  ColorSync behavior and creates no destination object.
- The largest difference to the host trilinear sample is below the 1e-3 tolerance of the test; no
  trilinear stop condition of section 12 was reached.

### W4. Settings tile

**Objective and deliverables.** The tile of section 3.2.

**Inputs and prerequisites.** W2.

**Modules, files, and APIs.**

- `DisplayProfileReadout` (proposed, `album_backend`): a `QObject` with the properties
  `description`, `profilePath`, `stateText`, `hasError`, and the property `window`. It reads
  `WindowDisplayDestination::ForWindow(window)` and follows `DestinationChanged`. It holds no copy
  of the destination fields: each property getter reads the current `DisplayDestination`.
- `DisplayProfileTile.qml` (proposed), registered in `ALCEDO_MAIN_QML_FILES`.
- `LutSettingsPanel.qml`: the tile below `CameraProfileFolderTile`, `visible: Qt.platform.os === "windows"`.
- `alcedo_main_en.ts`, `alcedo_main_zh_CN.ts`: the new strings, added by hand.

**Data rules and invariants.** The tile changes no state. Its colors, spaces, and type come from
`appTheme`.

**Implementation steps.**

1. Add `DisplayProfileReadout` and register it for QML.
2. Add the tile with the `BentoTile` structure of `CameraProfileFolderTile.qml`.
3. Add the strings: `Display profile`, `Applied`, `Managed by Windows`, `No profile, sRGB`.
4. Write the test.

**Tests and evidence.**

| Test | Assertion |
| --- | --- |
| `DisplayProfileTileQmlTest.ShowsDescriptionPathAndStateOfEachKind` | For each `DisplayDestination::kind` given through a test destination, the three texts equal the table of section 3.2. |
| `DisplayProfileTileQmlTest.DestinationChangeUpdatesTheTexts` | After `DestinationChanged` the texts show the new destination. |

Run the QML test with `QT_FORCE_STDERR_LOGGING=1` and check the log for QML warnings.

**Exit criteria.**

- [x] The two tests pass.
- [ ] A screenshot of the tile on the development machine is in the pull request. The screenshot
      exists (see the completion record); the GitHub CLI cannot upload an image, so the user
      attaches it to the pull request.

**Expected diff.** 350 to 500 lines.

**Completion record.**

```text
Phase / date / status: W4 / 2026-11-12 / done (the user attaches the screenshot to the pull request)
Source revision and branch: 0a02d2c86 + feature/windows-display-icc-settings-tile (on PR #874)
Actual changed modules: ui/edit_viewer (WindowDisplayDestination::ApplyMonitorReading),
  ui/alcedo_main (DisplayProfileReadout library, DisplayProfileTile.qml, LutSettingsPanel.qml,
  alcedo_main_zh_CN.ts, ApplicationModuleHost registration), tests/ui (DisplayProfileTileQmlTest)
Implemented behavior: the tile below CameraProfileFolderTile on Windows shows the 'desc' text,
  the state, the profile path, and the reason of an unusable profile of the monitor of the
  settings window, and follows DestinationChanged.
Explicitly unimplemented items: none
Primary success call chain: DisplayProfileTile -> DisplayProfileReadout.window =
  Window.window -> WindowDisplayDestination::ForWindow -> connect DestinationChanged;
  QWindow expose / screenChanged / ApplicationActive -> Refresh -> QueryMonitorColorProfile ->
  ApplyMonitorReading -> DisplayDestinationTracker::Update -> DestinationChanged ->
  DisplayProfileReadout::destinationChanged -> the getters read Destination() -> the labels
Primary failure and restore call chain: the profile cannot be read or used ->
  DisplayDestination{Unusable, error} -> state Unusable -> `Not applied` and the reason in the
  alert badge; the next usable read hides the badge
Build and test commands with exit codes: build --target DisplayProfileTileQmlTest
  WindowDisplayDestinationTest LutSettingsQmlTest SettingsAboutQmlTest alcedo_main (0);
  ctest -R "DisplayProfileTileQmlTest|WindowDisplayDestinationTest" -j 1 (0) with
  QT_FORCE_STDERR_LOGGING=1 (no QML warning); ctest -R "LutSettingsQmlTest|SettingsAboutQmlTest"
  -j 1 (8: one failure, issue #878)
Discovered / passed / failed / skipped counts: DisplayProfileTileQmlTest 2 / 2 / 0 / 0;
  WindowDisplayDestinationTest 6 / 6 / 0 / 0; LutSettingsQmlTest 6 / 6 / 0 / 0 (the LUT page
  with the tile); SettingsAboutQmlTest 3 / 2 / 1 / 0: OpenLogFolderButtonOpensTheLogDirectory
  opens category 11, and About is category 10 since #858 (issue #878, present before W4);
  SettingsPagesUseTheThemeTypeScale passes with the tile
Manual verification: a screenshot of the tile with the monitor of the development machine
  (QueryMonitorColorProfile(GetDesktopWindow()) given to the tile window of the offscreen
  harness, in a local test case that is not committed): `LG ULTRAFINE`, `Applied`,
  `C:\WINDOWS\system32\spool\drivers\color\LG ULTRAFINE-2D8FE6D4-...-B2535AC83D83.icc`
Performance or resource evidence: not required
Evidence path: build/tmp/display_icc_w4/ (removed at the end of the task)
Remaining defects or unavailable platforms: macOS has no tile (Windows only by section 3.2)
```

Changes against the plan:

- The readout has the property `state` (enum `DisplayProfileReadout::State`) and `errorText` in
  place of `stateText` and `hasError`. The tile translates the state with `qsTr`, so that
  `QQmlEngine::retranslate` after a language change also changes the state text. A text that C++
  translates does not follow a retranslation.
- The reason of D11 is in an `AlertBadge` below the path, and the state text is `Not applied`.
  DESIGN.md requires the alert badge for each error sentence in place of plain red text, and
  section 3.2 requires the tile to follow DESIGN.md. Section 3.2 above shows the result.
- A window without a monitor (before its first exposure) shows the state `No monitor`.
- `WindowDisplayDestination::ApplyMonitorReading` is the second half of `Refresh` (publish, log
  line, signal). `Refresh` calls it with the Windows query; the test gives it monitor states,
  because the offscreen platform has no monitor.
- `alcedo_main_en.ts` gets no entry: the source texts are English and have no plural form, as for
  `CameraProfileFolderTile`.
- `DisplayProfileTileQmlTest` is built on Windows only, because the tile is Windows only
  (`Qt.platform.os`).

### W5. Converted comparison view and LUT previews

**Objective and deliverables.** The comparison view and the LUT preview tiles show converted
pixels on Windows.

**Inputs and prerequisites.** W1, W2.

**Modules, files, and APIs.**

- `ComparisonImageStore`: `PublishPair` takes the encoding of each side. `SetDestination`.
  `ComparisonImageProvider::requestImage` converts.
- `LutPreviewImageStore`: `Put` takes the encoding. `SetDestination`.
  `LutPreviewImageProvider::requestImage` converts.
- `ConvertRenderedImageForComparison` stays unchanged, because `render_commands.cpp` writes its
  result to a file.
- The owner that creates each store connects `DestinationChanged` of the main window to
  `SetDestination` and publishes new URLs: `EditorComparisonController` through its pair URLs,
  `LutPreviewController` through its `shown_` entries.

**Data rules and invariants.**

- A store entry holds source pixels and their encoding. `requestImage` returns converted pixels.
- An entry with an HDR encoding cannot exist: `ConvertRenderedImageForComparison` rejects it.
- `SetDestination` and `Get` use the mutex of the store.
- A destination of kind `Unusable` or `NoWindow` gives the source pixels.

**Implementation steps.**

1. Add the encoding to the two store entries and to their `Put` / `PublishPair` callers.
2. Add `SetDestination` and the conversion in the two `requestImage` functions.
3. Make each URL owner publish new URLs on a new destination, with the cache-defeating part that
   its URLs have now (`operation_id`, `display_id`).
4. Write the tests.

**Primary success call chain.** Section 6.4.

**Primary failure chain.**
`ConvertImageToDisplay` throws `-> requestImage logs the reason once for the destination -> returns the source pixels`.
The destination is then `Unusable` in W2 terms, so the settings tile shows the same reason.

**Tests and evidence.**

| Test | Assertion |
| --- | --- |
| `EditorComparisonImageProviderTest.RequestedImageIsConvertedToTheDestination` | With a P3 destination a Rec.709 pair gives pixels equal to `ConvertImageToDisplay` of the source. |
| `EditorComparisonImageProviderTest.NewDestinationGivesNewUrlsAndPixels` | After `SetDestination` the URLs differ and the pixels equal the new conversion. |
| `EditorComparisonImageProviderTest.NoDestinationKeepsSourcePixels` | Kind `NoWindow` gives the source pixels. |
| `LutPreviewControllerTest.PreviewIsConvertedToTheDestination` | The same assertions for one preview. |
| The current tests of the automation render command | The file output of the automation command is unchanged. |

**Exit criteria.**

- [x] The tests pass.
- [ ] The comparison view and the viewport show the same colors for the same photo on the
      development machine (manual, user).

**Expected diff.** 450 to 700 lines.

**Completion record.**

```text
Phase / date / status: W5 / 2026-11-12 / done (manual color check of the user open)
Source revision and branch: f9e304e0f + feature/windows-display-icc-comparison-lut-previews (on
  PR #879)
Actual changed modules: ui/edit_viewer (ConvertImageForDestination, DisplayImageConverter in
  WindowDisplayDestination), ui/alcedo_main/album_backend (ComparisonImageStore,
  ComparisonImageProvider, EditorComparisonController, LutPreviewImageStore,
  LutPreviewImageProvider, LutPreviewController, ApplicationModuleHost::AttachDisplayWindow),
  alcedo_main and alcedo_studio_test_host main.cpp, tests/ui (display_destination_test_support,
  EditorComparisonImageProviderTest, EditorComparisonControllerTest, LutPreviewControllerTest)
Implemented behavior: both stores keep the source pixels and their output encoding;
  requestImage converts them to the destination of the main window; a new destination gives the
  shown comparison pair and the shown LUT previews new URLs and renders nothing.
Explicitly unimplemented items: none
Primary success call chain: render result -> PublishComparisonPair(a.display, b.display) /
  LutPreviewController::OnRendered -> CachePut(image, result.image.display) -> Show ->
  LutPreviewImageStore::Put(id, image, encoding); QML Image (asynchronous) ->
  ComparisonImageProvider / LutPreviewImageProvider::requestImage -> store GetForDisplay ->
  DisplayImageConverter::Convert -> ConvertImageForDestination -> ConvertImageToDisplay;
  main.cpp -> ApplicationModuleHost::AttachDisplayWindow(main window) -> SetDisplayWindow of
  both controllers -> WindowDisplayDestination::DestinationChanged -> ApplyDisplayDestination
  -> store SetDestination -> new pair URLs (PairChanged) / new display ids (revisionChanged)
Primary failure and restore call chain: ConvertImageToDisplay throws ->
  DisplayImageConverter::Convert writes one `[DisplayProfile]` warning for that destination ->
  the source pixels; the next destination converts again
Build and test commands with exit codes: build build\debug --target
  EditorComparisonImageProviderTest EditorComparisonControllerTest LutLibraryModelTest
  EditorComparisonViewQmlTest WindowDisplayDestinationTest DisplayProfileTileQmlTest
  alcedo_main alcedo_studio_test_host (0); each test binary with QT_FORCE_STDERR_LOGGING=1 (0);
  EditorComparisonLargeRawPairTest (1, issue #880)
Discovered / passed / failed / skipped counts: EditorComparisonImageProviderTest 9 / 9 / 0 / 0;
  EditorComparisonControllerTest 6 / 6 / 0 / 0; LutLibraryModelTest 33 / 33 / 0 / 0 (with
  LutPreviewControllerTest 8); EditorComparisonViewQmlTest 5 / 5 / 0 / 0;
  WindowDisplayDestinationTest 6 / 6 / 0 / 0; DisplayProfileTileQmlTest 2 / 2 / 0 / 0;
  EditorComparisonLargeRawPairTest 1 / 0 / 1 / 0: the cold pair render on the editor executor
  does not complete within 600 s, before the comparison store (issue #880)
Manual verification: open (the user compares the comparison view and the viewport)
Performance or resource evidence: the conversion runs on the QML image loader thread for each
  request (both Image items are asynchronous); not measured
Evidence path: none
Remaining defects or unavailable platforms: on macOS the destination stays NoWindow and the
  stores give the source pixels, as before
```

Changes against the plan:

- The destination of a store and the one warning for each failed destination are in
  `DisplayImageConverter` (window destination library) with its own mutex, so that both stores
  and the thumbnail store of W6 share them. The store mutex guards the entries and the URL
  revision; the conversion runs outside both locks.
- A comparison URL has a third part, the destination revision
  (`<operation>/<a|b>/<revision>`), because the operation id of a ready pair does not change. The
  parser accepts the two-part form too.
- The automation render command is unchanged in source (`render_commands.cpp` calls
  `ConvertRenderedImageForComparison` and writes its result), and the headless host attaches no
  display window. Its tests were not run for this phase.
- `ApplicationModuleHost::AttachDisplayWindow` connects the two controllers to the main window
  after the QML root loads.

### W6. Converted thumbnails

**Objective and deliverables.** The thumbnails of the library grid, the filmstrip, and the search
results show converted pixels on Windows.

**Inputs and prerequisites.** W1, W2. The audit step below.

**Modules, files, and APIs.**

- `ThumbnailService`: the delivered result reports the `ViewerDisplayConfig` of its pixels. The
  service derives it from the DRT node of the `PipelineGraphSnapshot` of the request with
  `ViewerDisplayConfigFromDrt`. The disk cache format stays.
- `ThumbnailImageStore`: `Put` takes the encoding; the entry keeps it. `SetDestination` gives
  each entry a new revision. `ThumbnailImageProvider::requestImage` converts.
- `thumbnail_manager.cpp`, `search_controller.cpp`: pass the encoding to `Put`.
- The thumbnail owner publishes the new URLs of loaded rows through
  `AlbumThumbnailModel::updateThumbnailState`.

**Data rules and invariants.**

- A thumbnail with an HDR encoding is delivered without conversion.
- The disk cache holds encoding pixels. A destination change renders nothing again.
- The revision in a URL is for the QML cache only, as now.

**Implementation steps.**

1. Audit: confirm in source that the thumbnail pixels have the encoding of the DRT node of the
   rendered snapshot, for the render path and for the disk cache path. Record the result in this
   section. Stop and update the plan when the pixels have a different encoding.
2. Add the encoding to the delivered result of `ThumbnailService` for both paths.
3. Add the encoding to the store entry and the conversion to `requestImage`.
4. Implement `SetDestination` and the URL publication for loaded rows.
5. Measure the conversion time of a 512-pixel thumbnail and the frame times of a grid scroll with
   the recipe of the library grid benchmark. Record both.
6. Write the tests.

**Primary success call chain.** Section 6.4.

**Primary failure chain.** As W5.

**Tests and evidence.**

| Test | Assertion |
| --- | --- |
| `ThumbnailServiceTest.DeliveredThumbnailReportsTheDrtEncoding` | A document with P3-D65 gamma 2.2 output delivers that encoding from the render path and from the disk cache path. |
| `AlbumBackendThumbnailTest.RequestedThumbnailIsConvertedToTheDestination` | The provider pixels equal `ConvertImageToDisplay` of the stored source. |
| `AlbumBackendThumbnailTest.NewDestinationPublishesNewUrlsForLoadedRows` | After `SetDestination` each loaded row has a new `thumbUrl`, and the service ran no render. |
| `AlbumBackendThumbnailTest.HdrThumbnailIsNotConverted` | An ST 2084 entry returns its source pixels. |

**Exit criteria.**

- [x] The audit result is recorded.
- [x] The tests pass.
- [x] The grid scroll frame times with the conversion are recorded next to the values without it.
- [ ] The grid, the filmstrip, and the viewport show the same colors for the same photo on the
      development machine (manual, user).

**Expected diff.** 700 to 1100 lines.

**Audit result (step 1).** The thumbnail pixels have the encoding of the DRT node of the
rendered snapshot on both paths. The service therefore takes the encoding from the same snapshot
with `ThumbnailEncodingOf(snapshot)`, which is `ViewerDisplayConfigFromDrt` of the DRT node, or
the default `ViewerDisplayConfig` (Rec.709, gamma 2.2) for a document without one.

| Path | Source evidence | Encoding of the pixels |
| --- | --- | --- |
| Render | `ScheduleRenditionRender` renders the snapshot with `RenderType::THUMBNAIL`. `PipelineScheduler` gives that render type no `output_color`; only `FULL_RES_EXPORT` sets it. Without `output_color` the renderer (`renderer.inl.hpp`) takes the display encoding of the frame from `ViewerDisplayConfigFromDrt` of the DRT node, as for the editor viewer. `MakeDisplayCacheThumbnailBuffer` then scales the float code values by 255 to RGBA8 with no color step. | The DRT encoding of the rendered snapshot |
| Disk cache | The key of an entry is `CommittedHistoryLabel(snapshot)`: the head and the transaction chain, which fold the root and every commit, so one key names one DRT state. The entry holds the RGBA8 buffer of a render of that state (`EnqueueWrite` after `DeliverPixels`). A hit is a snapshot with the same label. | The DRT encoding of the snapshot of the request |

**Measurements (step 5).** Release build, LG ULTRAFINE monitor profile of the development
machine (`\\.\DISPLAY2`), Rec.709 gamma 2.2 source, 50 timed conversions after 10 warm-up
conversions per row. "One-shot" builds a LittleCMS conversion per image
(`ConvertImageForDestination`, the W5 path); "prepared" is `DisplayImageConverter::Convert` with
the kept conversion of this phase.

| Destination | Size | One-shot median / p90 | Prepared median / p90 |
| --- | --- | --- | --- |
| LG ULTRAFINE | 256 x 171 | 2.23 / 2.50 ms | 0.44 / 0.54 ms |
| LG ULTRAFINE | 512 x 341 | 3.51 / 3.83 ms | 1.77 / 2.03 ms |
| sRGB (LittleCMS built-in) | 256 x 171 | 3.45 / 3.88 ms | 0.47 / 0.52 ms |
| sRGB (LittleCMS built-in) | 512 x 341 | 4.93 / 5.17 ms | 1.95 / 2.32 ms |
| sRGB (LittleCMS built-in) | 1024 x 683 | 10.19 / 11.41 ms | 7.69 / 8.63 ms |

Grid frame times: the library grid benchmark recipe (release `alcedo_main`, the perf project of
the library grid benchmark, justified layout at zoom level 7, window client 3866 x 2090 at 192
dpi, `PostMessage` wheel notches and button clicks, `qt.scenegraph.time.renderloop` frame log).
"Without" is the W5 head `8c7972703`, where the thumbnail store converts nothing. Each cell is
frames / gaps above 50 ms / longest gap.

| Run | Fast scroll down (30 notches, 40 ms apart) | Slow scroll up (30 notches, 150 ms apart) | Zoom 7 to 4, longest gaps | Zoom 4 to 7, longest gaps |
| --- | --- | --- | --- | --- |
| Without, run 1 | 79 / 0 / 46 ms | 233 / 9 / 117 ms | 516, 500, 385 ms | 433, 428, 423 ms |
| Without, run 2 | 81 / 0 / 45 ms | 194 / 17 / 125 ms | 516, 500, 402 ms | 450, 441, 416 ms |
| With, one-shot conversion (`5f82369ea`) | 75 / 1 / 53 ms | 256 / 7 / 96 ms | 517, 515, 375 ms | 434, 433, 427 ms |
| With, prepared conversion (`6cdde3402`) | 83 / 0 / 50 ms | 202 / 15 / 134 ms | 500, 500, 403 ms | 445, 434, 417 ms |

The scroll values of the four runs overlap, and the zoom gaps of 300 to 520 ms and one gap of 430
to 465 ms after a scroll stops occur in the runs without the conversion too. A gap in this log
also counts idle time of the GUI thread, so it is an upper bound of blocked time. The
`[DisplayProfile]` line of each "with" run names the LG ULTRAFINE monitor profile, so the store
converted every thumbnail.

**Completion record.**

```text
Phase / date / status: W6 / 2026-11-13 / done (manual color check of the user open)
Source revision and branch: 6cdde3402 + feature/windows-display-icc-thumbnails (on PR #881)
Actual changed modules: app/thumbnail_service (ThumbnailGuard::encoding, ThumbnailEncodingOf),
  ui/alcedo_main/album_backend (ThumbnailImageStore, ThumbnailImageProvider, ThumbnailManager,
  SearchController, LibraryModule, ApplicationModuleHost::AttachDisplayWindow), ui/edit_viewer
  (DisplayImageTransform in DisplayColorTransform; prepared conversions in
  DisplayImageConverter), tests/app (ThumbnailServiceTest), tests/ui (AlbumBackendThumbnailTest,
  DisplayColorTransformTest, WindowDisplayDestinationTest)
Implemented behavior: each delivered thumbnail reports the encoding of its pixels from the
  render path and the disk cache path; the thumbnail store keeps the source pixels and the
  encoding; requestImage converts them to the destination of the main window; a new destination
  gives the loaded grid and filmstrip rows and the shown search previews new URLs and renders
  nothing; an ST 2084 or HLG thumbnail is shown unconverted
Explicitly unimplemented items: none
Primary success call chain: ThumbnailService::GetThumbnail -> lookup worker -> disk hit:
  DeliverPixels(pixels, ThumbnailEncodingOf(snapshot)) | render: ScheduleRenditionRender
  (encoding = ThumbnailEncodingOf(snapshot)) -> callback_ -> MakeDisplayCacheThumbnailBuffer ->
  DeliverPixels(pixels, encoding) -> on_pixels -> ThumbnailGuard::encoding ->
  ThumbnailManager / SearchController callback -> ThumbnailImageStore::Put(id, edge, image,
  guard->encoding) -> URL `image://alcedo-thumb/<id>/<edge>/<revision>`; QML Image
  (asynchronous) -> ThumbnailImageProvider::requestImage -> GetForDisplayByProviderId ->
  DisplayImageConverter::Convert -> TransformFor(destination, encoding) ->
  DisplayImageTransform::Apply; main.cpp -> ApplicationModuleHost::AttachDisplayWindow ->
  LibraryModule::SetDisplayWindow -> WindowDisplayDestination::DestinationChanged ->
  ApplyDisplayDestination -> SetDisplayDestination -> ThumbnailManager::SetDisplayDestination
  (store SetDestination, new revisions, UpdateThumbnailState of each pinned row) and
  SearchController::RepublishSearchPreviewThumbnails
Primary failure and restore call chain: building or applying the conversion throws ->
  DisplayImageConverter::Convert writes one `[DisplayProfile]` warning for that destination ->
  the source pixels; the next destination converts again
Build and test commands with exit codes: build build\debug --target ThumbnailServiceTest
  AlbumBackendThumbnailTest DisplayColorTransformTest WindowDisplayDestinationTest
  EditorComparisonImageProviderTest EditorComparisonControllerTest LutLibraryModelTest (0);
  ThumbnailServiceTest --gtest_filter=-*Fuzz* (0); each other binary (0); build build\release
  --target alcedo_main (0) for the frame times
Discovered / passed / failed / skipped counts: ThumbnailServiceTest without the fuzz cases
  25 / 24 / 0 / 1 (MetalGeometryPipelineThumbnailStillRenders, Metal only), with
  DeliveredThumbnailReportsTheDrtEncoding; AlbumBackendThumbnailTest 9 / 8 / 0 / 1
  (MetalThumbnailGridLifecycleWithGeometryOperatorsProducesProviderUrl, Metal only);
  DisplayColorTransformTest 10 / 10 / 0 / 0; WindowDisplayDestinationTest 7 / 7 / 0 / 0;
  EditorComparisonImageProviderTest 9 / 9 / 0 / 0; EditorComparisonControllerTest
  6 / 6 / 0 / 0; LutLibraryModelTest 33 / 33 / 0 / 0
Manual verification: open (the user compares the grid, the filmstrip, and the viewport)
Performance or resource evidence: the two tables above
Evidence path: build/tmp/display_icc/ (w6_conversion_bench.txt, w6_conversion_bench2.txt,
  w6_scroll_base1.txt, w6_scroll_base2.txt, w6_scroll_conv1.txt, w6_scroll_conv2.txt, and the
  test logs w6_*.log)
Remaining defects or unavailable platforms: on macOS the destination stays NoWindow and the
  store gives the source pixels, as before; the Metal thumbnail tests run on macOS only
```

Changes against the plan:

- The tests name the gtest suites `ThumbnailServiceTests.DeliveredThumbnailReportsTheDrtEncoding`
  and `ThumbnailTests.*` of the binary `AlbumBackendThumbnailTest`.
- `DisplayImageConverter` keeps one prepared `DisplayImageTransform` for each encoding of the
  current destination and drops them on `SetDestination`. Step 5 measured 2.2 to 3.5 ms per
  thumbnail for building the LittleCMS conversion alone, and the QML image provider serves the
  requests of one view one after another, so a page of about 150 cells waited for one build per
  cell. The kept conversion gives the same pixels (`ConvertImageToDisplay` builds and applies a
  `DisplayImageTransform` too); the W5 stores use it as well. This is the response of the risk
  "the thumbnail conversion makes the grid scroll slower" in section 12, applied to the
  conversion instead of to the converted pixels: the store keeps no converted images.
- `LibraryModule::SetDisplayDestination(destination)` is public. `SetDisplayWindow` connects
  `DestinationChanged` of the window to it, and the tests call it directly, because the UI test
  main runs a `QCoreApplication` without screens.
- A new destination publishes the new URLs of the search previews too
  (`SearchController::RepublishSearchPreviewThumbnails`): the pinned search thumbnails get the
  current URL of the store, and each shown key without a request in flight is published again.
- `ThumbnailImageStore::ParseUrl` reads the element id and edge of a row URL, so that
  `ThumbnailManager::SetDisplayDestination` publishes the current URL of the edge that each row
  shows. `Put` stores another image format as `Format_RGBA8888`; the producers give RGBA8888.

## 10. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| SDR photo on a monitor with a calibration profile | Each photo surface shows the colorimetric conversion to the profile | W3, W5, W6 |
| Same encoding and monitor profile | The pixels are unchanged within 5e-4 | W1 |
| Rec.709 gamma 2.2 on the default sRGB profile | The pixels are encoded with the sRGB curve (D2) | W1 |
| P3 or Rec.2020 color outside the monitor gamut | The color clips per channel | W1 |
| ST 2084 or HLG output | No conversion; the current behavior | W1, W3, W6 |
| Window moves to a second monitor | Each photo surface shows the pixels for the second monitor | W2, W3, W5, W6 |
| Profile changed in Windows, application active again | The same result as a monitor change | W2 |
| HDR or Auto Color Management active on the monitor | The destination is sRGB; the tile shows `Managed by Windows` | W2, W4 |
| Monitor without a profile | The destination is sRGB; the tile shows `No profile, sRGB` | W2, W4 |
| Profile file that is not a usable RGB profile | No conversion; the reason is in the log and in the tile | W2, W3, W4 |
| Export, scopes, automation render | The pixels keep the encoding | W3, W5 |
| Qt offscreen platform | No conversion and no warning | W2 |
| macOS | The current ColorSync behavior; the shader change compiles for Metal | W3 |

## 11. Build and evidence rules

- Presets: `win_debug` for tests, `win_release` for the manual checks of the application,
  `macos_debug` with `-DALCEDO_BUILD_TESTS=ON` for the Metal run of W3.
- Run each MSVC command through the PowerShell tool with `cmd /c scripts\msvc_env.cmd`.
- Run GPU tests with `ctest -j 1`. Run only the suites that a phase names. The full suite is a
  user action.
- Put logs and screenshots below `build/tmp/display_icc/`.
- No test stores expected pixel values. Each expected value comes from the catalog matrices, the
  `Ce*` curve functions, or the table itself.
- Record an unavailable backend as unavailable. Record a skipped test as skipped.
- The checks of color appearance on a calibrated monitor are manual and belong to the user.

## 12. Risks and stop conditions

| Risk | Detection signal | Response |
| --- | --- | --- |
| `GetICMProfileW` reports the hardware profile while Advanced Color converts the pixels | Colors are converted two times on an HDR or Auto Color Management monitor | D9 covers it. If `advancedColorEnabled` does not report Auto Color Management on a test machine, read `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2` and update W2. |
| The 65-node table build blocks the GUI thread too long | The recorded build time of W1 is above 50 ms in the release build | Stop. Update the plan with a worker build before W3 continues. |
| Trilinear sampling error for a profile with a strong lookup table | The interpolation test of W3 is above its tolerance with a real calibration profile | Stop. Update the plan with tetrahedral sampling in the shader. |
| The thumbnail encoding differs from the DRT encoding | The audit step of W6 | Stop. Update W6. |
| The thumbnail conversion makes the grid scroll slower | The frame times of W6 step 5 | Stop. Update the plan to convert once per destination in the store. |
| A phase can pass 2000 changed lines | The diff during the work | Split the phase in the plan first. |

Stop the implementation also when the source audit of section 5 no longer agrees with the branch.

## 13. Completion record template

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
Performance or resource evidence, if required:
Evidence path:
Remaining defects or unavailable platforms:
```

## 14. Parameters

| Parameter | Value | Defined in |
| --- | --- | --- |
| Nodes per axis of a display table | 65 | `kDisplayTransformLutEdge` |
| Entries of the source curve | 4096 | `display_color_transform.cpp` |
| Input exponent of a linear encoding | 1 / 2.2 | `display_color_transform.cpp` |
| Rendering intent | Relative colorimetric with black point compensation | D7 |
| Table texture format | RGBA32F, 3D, linear filter | Section 6.5 |
| Identity tolerance of a table | 5e-4 | W1 tests |
| Matrix conversion tolerance of a table | 1e-3 | W1 tests |
| Table build time limit on the GUI thread | 50 ms (release build) | Section 12 |
| Prepared image conversions of a store | One for each encoding of the current destination | `DisplayImageConverter`, W6 |
