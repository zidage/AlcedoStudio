# DCP Camera Profile Plan

Date: 2026-11-12

Status: **C1 complete** (2026-11-12, reader and profile value). **C2 implemented** (2026-11-12,
library index, camera match, folder setting, Settings page); the user check of the Settings page
is open. **C3 complete** (2026-11-12, Develop reference and effective profile in the render). C4
to C7 planned.

Parent: [Roadmap](../../roadmap.md).

Direct prerequisite: the embedded DNG profile path that
[Embedded DNG color profiles](../../../dng_color_profiles.md) describes. This plan extends that
path. It does not replace it.

Tracking issue: [#851](https://github.com/zidage/AlcedoStudio/issues/851). Phase issues: C1 #844,
C2 #845, C3 #846, C4 #847, C5 #848, C6 #849, C7 #850.

Source audit revision: `c54ac05fa` on `main`.

Unless a path starts with `alcedo_studio/` or `docs/`, it is relative to `alcedo_studio/src/`.
Test paths are relative to `alcedo_studio/`.

Render backends: **CUDA, OpenCL, Metal only.** All host-side work in this plan is file parsing,
profile matching, and parameter packing. None of it renders pixels.

Section 14 defines the terms and parameters that this plan uses.

---

## 1. Decision record

### 1.1 User goal

The user selects a folder that holds DCP files (DNG Camera Profile files). An example is the
`Resources/CameraProfiles/Camera` folder of a Lightroom Classic installation. Alcedo Studio reads
the DCP files from that folder, finds the profiles that belong to the camera of the open image, and
lets the user select one. The render applies the selected profile through the current embedded
DNG profile path: camera RGB to AP1, to linear ProPhoto RGB with a D50 white point, table
evaluation, and back to AP1.

Alcedo Studio does not distribute profile files of other vendors. The user supplies the folder.

### 1.2 Approved decisions

| Id | Decision |
| --- | --- |
| D1 | A Settings page holds one DCP library folder. The folder path is the only persisted setting. |
| D2 | The editor matches profiles to the camera of the image automatically. The match uses the camera make and model, as the lens correction does for the lens. |
| D3 | The render reuses the embedded DNG profile path. No backend gets a second table evaluation. |
| D4 | Profile tables are session data. They stay in process memory. The project database, the project package, and the export files hold no table data and no matrix data of a DCP file. |
| D5 | The DCP reader is new C++ code, written from the DNG specification. The Python project `atonyxu/dcptool-python` (Apache-2.0) is a format reference only. No code is copied. |

### 1.3 Decisions made in this plan

| Id | Decision | Reason |
| --- | --- | --- |
| P-1 | The project stores a profile reference for each image version: the camera model of the profile, the profile name, and a content fingerprint. | The embedded DNG path stores a fingerprint and keeps the tables out of the project. A reference keeps an edit reproducible in a later session, and D4 stays true. |
| P-2 | The default for each image stays the current profile: the built-in camera matrices for a RAW file, and the embedded profile for a DNG file. A matched DCP profile applies only after the user selects it. | A camera has several profiles (Standard, Vivid, Portrait). No rule selects one of them for the user. |
| P-3 | A match is exact after name normalization. An approximate match is not permitted. | A profile of another camera gives wrong color with no visible error. |
| P-4 | A stored reference that the library cannot resolve fails the render with a named error. The editor shows the stored name and lets the user select another profile. | `AGENTS.md` permits no silent substitute. The embedded DNG path has the same rule for a missing source file. |
| P-5 | `ProfileToneCurve` is read past and not applied. | The embedded DNG path does not apply it. The ACES output transform owns tone reproduction. A display-referred curve before that transform maps tone two times. |
| P-6 | The profile reference is not part of adjustment transfer, copy and paste, or `alcedo-cli` in this plan. | A reference is specific to one camera model. The transfer catalogs exclude `camera_profile` today. |
| P-7 | A profile with `ColorMatrix3`, `RGBTables`, or a `ReductionMatrix` fails to load with an error that names the tag. | The current render path has two illuminants and three camera channels. |

### 1.4 Evidence from the user folder

A read-only survey of `D:\Adobe\Adobe Lightroom Classic\Resources\CameraProfiles\Camera`
(2026-11-12) gives these facts. They size the design. They are not test data.

| Fact | Value |
| --- | --- |
| DCP files | 2445, in 359 camera folders, 613 MB in total |
| File size | 36 KB to 279 KB, median 278 KB |
| Header | Little-endian TIFF, magic `0x4352`, one IFD |
| Tags in every file | `UniqueCameraModel`, `ProfileName`, `ColorMatrix1/2`, `ForwardMatrix1/2`, `CalibrationIlluminant1/2` (17 and 21), `ProfileLookTableDims/Data`, `ProfileToneCurve`, `ProfileCalibrationSignature`, `ProfileEmbedPolicy`, `ProfileCopyright` |
| Tags in some files | `ProfileLookTableEncoding` = 1 (2028), `DefaultBlackRender` (2028), `BaselineExposureOffset` (1841), `ProfileHueSatMapDims/Data1/Data2` (9), `ProfileGroupName` (3) |
| Look table dimensions | 90x16x16 (2056), 36x16x16 (354), 36x8x16 (25), 45x8x8 (10) |
| File name and folder | `<UniqueCameraModel>/<UniqueCameraModel> <ProfileName>.dcp` |

No file has a third illuminant or `RGBTables`. The largest look table has 69,120 floats, which is
below `kMaxDngTableFloats`.

---

## 2. Product design specification

### 2.1 Settings page "Camera Profiles"

`SettingDialog.qml` gets one new category, **Camera Profiles**.

```
Camera Profiles
+--------------------------------------------------------------+
| DCP Folder                                                   |
| [ D:\Adobe\...\CameraProfiles\Camera            ] [Browse]   |
|                                                              |
|  2,445            359                                        |
|  Profiles         Cameras                                    |
|                                                  [Rescan]    |
+--------------------------------------------------------------+
```

- The folder row uses `FolderPathField.qml`. **Browse** opens a `FolderDialog`.
- The two counts use the shared `CountTile` component and the compact number format.
- **Rescan** reads the folder again. It is disabled while a scan runs and when no folder is set.
- While a scan runs, the count tiles show the last completed counts and a busy indicator.
- An empty path is the default. Both counts then show `0`.
- A folder that does not exist, or that the process cannot read, shows the operating system error
  text below the folder row. The stored path stays, so that a removable drive works after it
  returns.
- A file that fails to parse does not stop the scan. The page shows the number of rejected files
  as a third tile, **Rejected**, when the number is above zero. The log holds one line for each
  rejected file with the path and the reason.
- Buttons are neutral (black and white). The page has no explanatory paragraph.

### 2.2 Editor section "Camera Profile"

`EditorRawDecodePanel.qml` gets one new `CollapsibleSection`, **Camera Profile**, above the white
balance section. The section is visible for a RAW document and hidden for a raster document.

```
Camera Profile
+----------------------------------------------+
| [ Camera Standard                         v ] |
+----------------------------------------------+
```

- The combo lists, in this order: the default entry, then each matched profile in ascending name
  order.
- The default entry is **Built-in** for a RAW file and **Embedded** for a DNG file that has an
  embedded profile.
- A selection commits one settled edit of the Develop field `camera_profile_file`. Undo, Redo, and
  version checkout restore the previous selection.
- When no folder is set, the section shows the combo with the default entry and a neutral button
  **Choose Folder**. The button opens the same `FolderDialog` as the Settings page.
- When the folder is set and no profile matches the camera, the combo holds the default entry
  only.
- When the stored reference does not resolve (P-4), the combo shows the stored profile name with
  the warning color of `appTheme`. The list still holds the default entry and the matched
  profiles. The viewport shows the render error through the current render error surface.
- The combo is disabled while the library scan runs.

### 2.3 Data rules

- **Profile identity.** `(unique_camera_model, profile_name)`, compared after normalization
  (section 2.4). When two files in the library have the same identity, the scan keeps the file
  with the path that sorts first and logs one warning that names both paths.
- **Fingerprint.** FNV-1a 64-bit hash of the parsed profile content, in the text form of
  `DngColorProfileFingerprintToText` (16 lowercase hexadecimal digits).
- **Stored reference.** Develop JSON key `camera_profile_file`, an object with the keys
  `camera_model`, `profile_name`, and `fingerprint`. The key is absent when the default profile
  applies. A document without the key serializes byte for byte as it does at revision
  `c54ac05fa`, so the root id of each current project stays valid.
- **Fingerprint mismatch.** When the library resolves the identity to a file with another
  fingerprint, the library file wins. The binding logs one warning. The reference in the live
  document takes the new fingerprint.
- **Effective profile.** With a selected DCP profile, the render takes these values from the DCP
  file: `ColorMatrix1/2`, `ForwardMatrix1/2`, the two illuminant CCT values, the hue and
  saturation maps, the look table, their encodings, and `BaselineExposureOffset`. The render
  takes these values from the image: `AsShotNeutral` (or `cam_mul`), and for a DNG source also
  `AnalogBalance`, `CameraCalibration1/2`, and `BaselineExposure` (phase C6).
- **White balance.** A profile change solves `as_shot_cct` and `as_shot_tint` again from the
  effective matrices. `custom_cct` and `custom_tint` keep their numbers.
- **Import-bound data.** `camera_profile` in the Develop payload keeps the import values. A DCP
  selection does not change it.

### 2.4 Camera match rule

Inputs: the camera make and model of the image (`RawRuntimeColorContext::camera_make_` and
`camera_model_`), and the `UniqueCameraModel` of each profile.

1. Compute `CompactCameraName` of the `UniqueCameraModel`.
2. Compute the candidate keys of the image:
   - `CompactCameraName(model)`;
   - `CompactCameraName(make_word + " " + model)`, where `make_word` is the first word of the
     make after the make alias table.
3. The profile matches when its key equals one candidate key.

The make alias table maps the Exif make to the name that `UniqueCameraModel` uses: `NIKON
CORPORATION` to `Nikon`, `OLYMPUS CORPORATION`, `OLYMPUS IMAGING CORP.`, and `OM Digital
Solutions` to `Olympus`, `RICOH IMAGING COMPANY, LTD.` and `PENTAX Corporation` to `Pentax`,
`LEICA CAMERA AG` to `Leica`, and each other make to its first word. Phase C2 completes the table
from the 359 camera names of section 1.4 and the Exif makes in `camera_color_matrix_db.cpp`.

For a DNG file, the camera model of the image already follows `UniqueCameraModel` when
`ShouldPreferUniqueCameraModel` selects it, so the same rule applies.

### 2.5 Asynchronous behavior

- The library scan runs on one worker task of the library service. It reads only the IFD and the
  two identity strings of each file.
- A scan publishes its complete index in one step. Readers see the previous index or the new
  index.
- A profile lookup for a render binding waits for a running scan and then reads the index. The
  wait has no timeout, because the render needs the result.
- A new folder path or **Rescan** starts a scan after the running scan ends. No cancel protocol is
  necessary: a scan of 2445 files reads about 2445 short byte ranges, and the target in phase C2
  is below 2 seconds on the development machine.
- A full profile load (tables) occurs on the thread that binds the document. A process-wide cache
  keeps loaded profiles.

---

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility |
| --- | --- |
| `image/` | DCP file reader, `DcpCameraProfile` value, camera match rule, library index, loaded-profile cache |
| `edit/graph/` | `camera_profile_file` reference in the Develop payload, effective profile resolution in the color transform |
| `edit/runtime/` | Table packing and content keys read the effective profile |
| `app/` | Library service with the folder setting and scan task, document binding on each load path, Develop field write and history ownership, editor read model for the panel |
| `ui/alcedo_main/` | Settings page, editor section, QML registration |
| `docs/` | `docs/dng_color_profiles.md` update |

### 3.2 Explicit exclusions

- `ProfileToneCurve`, `ProfileToneMethod`, and `DefaultBlackRender` rendering (P-5). The ACES
  output transform keeps tone reproduction.
- Triple-illuminant profiles, `RGBTables`, and four-channel cameras (P-7).
- DCP writing, editing, and conversion.
- Adjustment transfer, copy and paste, and `alcedo-cli` parameters for the reference (P-6).
- Raster documents. They have no camera profile.
- A copy of profile data into the project, the package, or an exported DNG.

### 3.3 Related plans

- [Embedded DNG color profiles](../../../dng_color_profiles.md): this plan extends its render
  path and its storage rule.
- [Raster Image Input Plan](../edit/raster_image_input_plan.md): the pattern for an optional
  Develop JSON key and a fourth Develop field (`input_profile`). This plan only depends on it.

---

## 4. Current source audit

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| DNG profile value | `include/image/dng_color_profile.hpp` | `DngColorProfile` holds calibration, two hue and saturation maps, the look table, and exposure. `MakeDngColorProfile` validates and computes the fingerprint. | Reuse unchanged for DCP tables. |
| DNG table import | `image/dng_color_profile_import.cpp` | `ReadMap` builds a `DngHueSatMap` from Exiv2 data, including the omitted zero-saturation row. | Move the row logic into a function that takes dimensions and a float span, so that the DCP reader uses it. |
| Camera matrices of a RAW file | `image/metadata/libraw_metadata_mapper.cpp:229-288`, `image/metadata/camera_color_matrix_db.cpp:246` | A non-DNG RAW gets `ColorMatrix1/2` from the built-in matrix database, fixed CCT values 2856 and 6504, no `ForwardMatrix`, and no table. | None. This stays the default. |
| Import-bound Develop data | `include/edit/graph/develop_node_model.hpp:36-50`, `edit/graph/develop_color_transform.cpp:606-650` | `DevelopCameraProfile` holds matrices, CCT values, `AsShotNeutral`, `cam_mul`, and a `DngColorProfileRef`. `BindImportedCameraProfile` copies it from the RAW context on each document build. | Stays the import value. The effective profile is a separate read. |
| Color transform | `edit/graph/develop_color_transform.cpp:698-819` | `ResolveDevelopColorTransform` uses `camera_profile` matrices and, with a bound DNG profile, `AnalogBalance` and `CameraCalibration`. It returns `UnboundDngProfile` for an unbound reference. | Read the effective matrices and tables. Add the unresolved DCP reference error. |
| Table packing | `edit/runtime/dng_profile_gpu_data.cpp:11-50` | `PackDngProfileGpuData` blends the two maps by CCT and packs a 32-float header and the tables. | Read the effective tables and the effective CCT values. |
| GPU evaluation | `include/edit/runtime/dng_profile_gpu_math.h`, `cuda_camera_color_pass.cu`, `opencl/shader/geometry_camera.cl`, `metal/shader/camera_color.metal` | The three backends compile `DngApplyColorProfile`. | None. |
| Content keys | `edit/runtime/result_content_key.cpp:94-128` | `MixCameraColorParams` mixes the DNG fingerprint and all `camera_profile` values. The sensor key excludes them. | Mix the DCP fingerprint. |
| Develop fields | `app/pipeline_history_applier.cpp:84-104` | Four fields own keys of the Develop JSON: `raw_decode`, `color_temp`, `lens_calib`, `input_profile`. `camera_profile` is outside history. | Add the field `camera_profile_file`, which owns the key `camera_profile_file`. |
| Field write | `app/editor_parameter_write.cpp:226`, `app/editor_parameter_write_parse.cpp`, `app/editor_adjustment_pipeline.cpp:44`, `app/editor_panel_projection.cpp:536`, `app/editor_adjustment_context.cpp:58,291`, `app/editor_pipeline_command_service.cpp:232` | Each Develop field has a parse function, an update struct, a Model operation, and a panel reader. | Add the same set for `camera_profile_file`. |
| Document build | `app/pipeline_root_state.cpp:107-145` | `BindRootCameraProfile` runs in `BuildDocumentFromRoot` and `BuildDocumentFromCheckpoint`. | Bind the DCP reference in the same function, after the import binding. |
| Source DNG binding | `app/source_dng_profile_binding.cpp` | Binds the embedded profile from the source file through `DngColorProfileCache`. | None for a RAW source. Phase C6 reads the camera calibration signature for a DNG source. |
| Preview documents | `app/editor_comparison_inputs.cpp:86-96`, `app/lut_preview_image.cpp:29-35`, `app/ai_mask_analysis_image.cpp:41-53` | Each builds a document from the root and then takes Develop settings from the current document (`UseSensorSettingsFrom`, `UseAllSettingsFrom`). | The two Model operations copy the reference and its bound profile with the other camera fields. |
| Settings folders | `app/lut_library_service.cpp:27,125-151`, `qml/LutSettingsPanel.qml`, `qml/FolderPathField.qml` | A `QObject` service owns a root path in `QSettings`, and a QML page opens a `FolderDialog`. | Add a service and a page with the same structure. |
| RAW panel | `ui/alcedo_main/qml/EditorRawDecodePanel.qml:397-545` | Sections: Input Color (raster), white balance, Lens Calibration, RAW Decode. | Add the Camera Profile section. |
| Camera identity in QML | `include/ui/alcedo_main/album_backend/editor_session_controller.hpp:124-125` | Only the lens identity is exposed. | The panel reads the matched list from a C++ model, so no camera string goes to QML. |
| Tone curve | none | No code reads `ProfileToneCurve`. `docs/dng_color_profiles.md:81` states this. | None (P-5). |
| Tests | `tests/edit/runtime/dng_color_profile_test.cpp`, `tests/image/dng_color_profile_cache_test.cpp`, `tests/app/pipeline_dng_profile_binding_test.cpp`, `tests/edit/runtime/dng_profile_test_support.hpp` | Cover the embedded path on the scalar reference and the three backends. | New suites in section 9. The current suites must pass unchanged. |

---

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Changes | Lifetime | Error surface |
| --- | --- | --- | --- | --- | --- |
| `ReadDcpCameraProfile` (function, `image/`) | File path | `DcpCameraProfilePtr` | Nothing | Call | `std::runtime_error` that names the file and the tag |
| `DcpProfileLibrary` (`image/`, process-wide `Shared()`) | Folder path | Index of identities, loaded profiles | Its index and its cache | Process | `Find` returns null for an unknown identity. A load error propagates. |
| `CameraProfileLibraryService` (`app/`, `QObject`) | `QSettings` path, user actions | Properties for QML: path, counts, scan state, error text | The folder setting, scan start | Application | Error text property |
| `DevelopParamsModel` (`edit/graph/`) | `DevelopCameraProfileFileUpdate` | Payload with reference and bound profile | The Develop payload | Document | `std::logic_error` for a raster document |
| `BindDcpCameraProfile` (function, `app/`) | Document | Bound document | The runtime pointer in the payload | Call | Leaves the reference unbound when the library has no such identity |
| `EditorCameraProfileListModel` (`ui/alcedo_main/album_backend/`) | Active image, library index | Rows for the combo | Its rows | Editor session | Empty list |

### 5.2 New value types

**`DcpCameraProfile`** (proposed, `include/image/dcp_camera_profile.hpp`). The parsed content of
one DCP file: `unique_camera_model`, `profile_name`, `color_matrix_1`, `color_matrix_2`,
`forward_matrices_valid`, `forward_matrix_1`, `forward_matrix_2`, `color_matrix_1_cct`,
`color_matrix_2_cct`, `profile_calibration_signature`, `tables` (a `DngColorProfilePtr` with
identity calibration, the maps, the look table, and `BaselineExposureOffset`), and `fingerprint`.
It is immutable after `MakeDcpCameraProfile` validates it. `DevelopCameraProfile` cannot hold this
content, because that struct is the import value of one image and its JSON is persisted.

**`DevelopCameraProfileFile`** (proposed, in `develop_node_model.hpp`). The reference and its
runtime binding: `camera_model`, `profile_name`, `fingerprint`, and a `DcpCameraProfilePtr` that
JSON does not write. It follows the three states of `DngColorProfileRef`: absent, bound, and
unbound. `DevelopPayload` gets `std::optional<DevelopCameraProfileFile> camera_profile_file`.

### 5.3 Data flow

```
Settings page
  -> CameraProfileLibraryService::SetLibraryRoot(path)      QSettings "cameraProfiles/dcpLibraryRoot"
  -> DcpProfileLibrary::Shared().Scan(path)                 worker task, index published once

Editor panel
  -> EditorCameraProfileListModel                           rows = default + DcpProfileLibrary::Match(make, model)
  -> editorSession.submitPatch("camera_profile_file", {...}, settled)
  -> ParseCameraProfileFileUpdate                           app/editor_parameter_write_parse.cpp
  -> ApplyEditorParameterValue                              resolves DcpProfileLibrary::Find(identity)
  -> DevelopParamsModel::ApplyCameraProfileFileUpdate       payload + as-shot solve, WhiteBalance dirty
  -> PipelineEditBatch (SetParameter, field camera_profile_file) -> history

Render
  -> ResolveDevelopColorTransform(payload)                  effective matrices
  -> PackDngProfileGpuData(payload, transform)              effective tables
  -> camera color pass (CUDA / OpenCL / Metal)              unchanged kernels

Load (thumbnail, export, editor lease, checkout, preview documents)
  -> BuildDocumentFromRoot / BuildDocumentFromCheckpoint
  -> BindRootCameraProfile                                  import binding, then BindDcpCameraProfile
```

### 5.4 Failure flow

```
Stored reference, identity absent from the library
  -> BindDcpCameraProfile leaves the reference unbound
  -> ResolveDevelopColorTransform returns ColorTransformError::UnboundCameraProfileFile
  -> message: camera profile "<profile_name>" for "<camera_model>" is not in the DCP folder "<path>"
  -> thumbnail, export, and editor render report the error through their current error paths
  -> no render occurs with another profile
```

---

## 6. File and API map

### 6.1 Proposed files

| File | Content |
| --- | --- |
| `include/image/dcp_camera_profile.hpp` | `DcpCameraProfile`, `DcpCameraProfilePtr`, `MakeDcpCameraProfile` |
| `include/image/dcp_file_reader.hpp`, `image/dcp_file_reader.cpp` | `ReadDcpProfileIdentity(path)`, `ReadDcpCameraProfile(path)` |
| `include/image/dcp_profile_library.hpp`, `image/dcp_profile_library.cpp` | `DcpProfileLibrary`, `DcpCameraModelMatches` |
| `include/app/camera_profile_library_service.hpp`, `app/camera_profile_library_service.cpp` | `CameraProfileLibraryService` |
| `include/app/dcp_camera_profile_binding.hpp`, `app/dcp_camera_profile_binding.cpp` | `BindDcpCameraProfile(PipelineDocument&)` |
| `include/ui/alcedo_main/album_backend/editor_camera_profile_list_model.hpp` and its `.cpp` | `EditorCameraProfileListModel` |
| `ui/alcedo_main/qml/CameraProfileSettingsPanel.qml` | Settings page |
| `ui/alcedo_main/qml/EditorCameraProfileSection.qml` | Editor section |
| `tests/image/dcp_file_reader_test.cpp`, `tests/image/dcp_profile_library_test.cpp`, `tests/image/dcp_test_file_writer.hpp` | Reader and library tests, and a writer of synthetic DCP files |
| `tests/edit/runtime/dcp_camera_profile_render_test.cpp` | Scalar transform tests |
| `tests/app/pipeline_dcp_profile_binding_test.cpp` | Load path and storage tests |

### 6.2 Proposed APIs

| API | Input | Output | Changes state | Error |
| --- | --- | --- | --- | --- |
| `ReadDcpProfileIdentity` | `std::filesystem::path` | `unique_camera_model`, `profile_name` | No | `std::runtime_error` |
| `ReadDcpCameraProfile` | `std::filesystem::path` | `DcpCameraProfilePtr` | No | `std::runtime_error` |
| `MakeDngHueSatMap` | Dimensions, encoding, `std::span<const float>` | `DngHueSatMap` | No | `std::runtime_error` |
| `DcpProfileLibrary::Scan` | Folder path | Scan result: profile count, camera count, rejected count | Replaces the index | Folder error in the result |
| `DcpProfileLibrary::Match` | Camera make, camera model | Sorted list of identities | No | None |
| `DcpProfileLibrary::Find` | `camera_model`, `profile_name` | `DcpCameraProfilePtr` or null | Fills the cache | A parse error propagates |
| `DevelopParamsModel::ApplyCameraProfileFileUpdate` | `DevelopCameraProfileFileUpdate` | None | Payload | `std::logic_error` for a raster document |
| `EffectiveCameraProfile` | `const DevelopPayload&` | Matrices, CCT values, and the table pointer for the render | No | Throws for an unbound reference |
| `BindDcpCameraProfile` | `PipelineDocument&` | None | Runtime pointer | None. An unknown identity stays unbound. |

---

## 7. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| C1 | DCP file reader and profile value | `image/` | None | 900 to 1200 | Complete |
| C2 | Library index, camera match, folder setting, Settings page | `image/`, `app/`, `ui/alcedo_main/` | C1 | 1300 to 1700 | Implemented; user page check open |
| C3 | Profile reference in the Develop payload and the effective profile in the render | `edit/graph/`, `edit/runtime/` | C1 | 1100 to 1500 | Complete |
| C4 | Develop field, history, and binding on each load path | `app/` | C2, C3 | 1200 to 1600 | Planned |
| C5 | Editor section and list model | `ui/alcedo_main/`, `app/` | C4 | 900 to 1300 | Planned |
| C6 | DCP profile on a DNG source | `image/`, `app/`, `edit/graph/` | C4 | 600 to 900 | Planned |
| C7 | Backend verification on CUDA, OpenCL, and Metal, user review, documents | tests, `docs/` | C5, C6 | 500 to 800 | Planned |

C2 and C3 have no dependency on each other. Each phase estimate is below 2000 lines.

Before each phase, the implementer reads `AGENTS.md` and the applicable skills again:
`alcedo-msvc-cmake` for each build, `alcedo-qml-ui` for C2 and C5, and `raw-processor-module` when
a phase touches RAW processor files.

---

## 8. Detailed phases

### C1. DCP file reader and profile value

**Objective and deliverables.** `ReadDcpCameraProfile` reads one DCP file into an immutable
`DcpCameraProfile`. A reviewer can run the reader tests with synthetic files.

**Inputs and prerequisites.** None.

**Modules, files, and APIs.** `dcp_camera_profile.hpp`, `dcp_file_reader.{hpp,cpp}` (proposed).
`image/dng_color_profile_import.cpp` and its header change: the row logic of `ReadMap` moves to
`MakeDngHueSatMap`. `image/CMakeLists.txt` and `tests/image/CMakeLists.txt` register the new
files.

**Data rules and invariants.**

- The reader accepts byte order `II` and `MM`, and magic `0x4352` and `42`.
- The reader reads the first IFD only. Each tag payload must be inside the file.
- Tag types: matrices are `SRATIONAL`, tables are `FLOAT`, dimensions are `LONG`, illuminants are
  `SHORT`, strings are `ASCII`. A tag with another type fails with the tag number.
- Required tags: `UniqueCameraModel`, `ProfileName`, `ColorMatrix1`. All other tags are optional.
- One `ColorMatrix` gives the same matrix and CCT in both slots, as the DNG import does. One
  `ForwardMatrix` does the same.
- `CalibrationIlluminant` maps to CCT through the current `CalibrationIlluminantToCct`. An
  illuminant with no CCT fails with the illuminant number.
- `ColorMatrix3`, `RGBTables`, `ReductionMatrix1/2/3`, and a matrix with a count other than 9 fail
  (P-7).
- A rational with a zero denominator fails.
- The file size limit is 64 MB. A larger file fails before the read.
- The fingerprint covers every field of `DcpCameraProfile`, including the fingerprint of `tables`.

**Implementation steps.**

1. Move the zero-saturation row logic of `ReadMap` into `MakeDngHueSatMap`. `ReadMap` calls it.
   `DngColorProfileTest` must pass with no change to its assertions.
2. Add `DcpCameraProfile` and `MakeDcpCameraProfile`. The function validates finite matrices, a
   positive CCT, and non-empty identity strings, and computes the fingerprint.
3. Add the TIFF IFD reader as a file-local class in `dcp_file_reader.cpp`. It reads the header,
   the entry table, and one payload on request.
4. Add `ReadDcpProfileIdentity`. It reads the header, the entry table, and the two string
   payloads. It does not read the tables.
5. Add `ReadDcpCameraProfile`. It builds `tables` through `MakeDngColorProfile` with identity
   calibration and `baseline_exposure_offset` from the file.
6. Add `tests/image/dcp_test_file_writer.hpp`. It writes a DCP file from a list of tags. Tests use
   it for each case. No vendor profile bytes enter the repository.

**Primary success call chain.**

```
ReadDcpCameraProfile(path) -> IFD reader -> matrices, CCT values, MakeDngHueSatMap x3
  -> MakeDngColorProfile(tables) -> MakeDcpCameraProfile -> DcpCameraProfilePtr
```

**Primary failure call chain.**

```
ReadDcpCameraProfile(path) -> payload offset outside the file
  -> std::runtime_error("DCP <path>: tag 50982 data is outside the file") -> no profile object
```

**Tests and evidence.** Target `DcpFileReaderTest`, label `ci_core`.

| Test | Assertion |
| --- | --- |
| `ReadsMatricesIlluminantsAndLookTableFromLittleEndianFile` | Each matrix element, both CCT values, and each table entry equal the written values. |
| `ReadsTheSameProfileFromBigEndianFile` | The fingerprint equals the little-endian result. |
| `SingleColorMatrixFillsBothIlluminantSlots` | Slot 2 equals slot 1, and both CCT values are equal. |
| `LookTableWithoutZeroSaturationRowsGetsNeutralRows` | The first saturation row has value scale 1 and the hue and saturation corrections of the second row. |
| `IdentityReadDoesNotReadTablePayloads` | A file whose table offset is outside the file gives the identity, and the full read fails. |
| `MissingProfileNameFails`, `ThirdIlluminantFails`, `RgbTablesFail`, `ZeroDenominatorFails`, `TruncatedIfdFails`, `WrongMagicFails` | Each throws, and the message holds the path and the tag number or the reason. |
| `EqualContentGivesEqualFingerprint` | Two files with equal tags in another IFD order give one fingerprint. |

**Build and run commands.**

```
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DcpFileReaderTest DngColorProfileTest --parallel 4
ctest --test-dir build/debug -R "DcpFileReaderTest|DngColorProfileTest" --output-on-failure
```

**Exit criteria.**

- [x] `DcpFileReaderTest` passes with all listed tests discovered.
- [x] `DngColorProfileTest` and `DngColorProfileCacheTest` pass unchanged.
- [x] A manual run of the reader over the user folder of section 1.4 loads 2445 files with zero
      rejected files. The record states the command and the counts.

**Expected diff.** 900 to 1200 lines.

**Completion record.**

```text
Phase / date / status: C1 / 2026-11-12 / complete
Source revision and branch: 10643e8d0 (plan branch docs/dcp-camera-profile-plan), work branch
  feature/dcp-file-reader
Actual changed modules: image/ (reader, profile value, MakeDngHueSatMap,
  CalibrationIlluminantToCct), tests/image/ (reader tests, synthetic DCP writer)
Implemented behavior:
  - include/image/dcp_camera_profile.hpp: DcpCameraProfile, DcpCameraProfilePtr,
    DcpCameraProfileToJson (fingerprint input), MakeDcpCameraProfile.
  - include/image/dcp_file_reader.hpp, image/dcp_file_reader.cpp: DcpProfileIdentity,
    ReadDcpProfileIdentity, ReadDcpCameraProfile, kMaxDcpFileBytes. The file-local class
    DcpIfdReader reads the header and the entry table, then one payload for each request.
  - MakeDngHueSatMap (dng_color_profile_import.hpp/.cpp) holds the row logic of ReadMap; the
    embedded DNG import and the DCP reader both call it.
  - CalibrationIlluminantToCct moved unchanged from an anonymous namespace in
    image/metadata/dng_color_metadata.cpp to include/image/dng_camera_matrix.hpp, so that the
    DCP reader uses the same table.
Detail decisions made in this phase:
  - String tags accept TIFF type ASCII and BYTE. The DNG specification permits BYTE (UTF-8) for
    ProfileName, ProfileCalibrationSignature, and ProfileCopyright.
  - ProfileHueSatMapDims and ProfileLookTableDims need count 3, as the DNG specification states.
    Encoding tags are LONG with count 1. The reader does not require ascending tag order; a tag
    that appears two times fails.
  - An absent CalibrationIlluminant tag reads as 0 (Unknown), which has no CCT, so a profile
    without an illuminant for each ColorMatrix fails with the tag number and illuminant 0.
  - Rejected tags (P-7): ReductionMatrix1/2/3, CalibrationIlluminant3, ColorMatrix3,
    ForwardMatrix3, ProfileHueSatMapData3, RGBTables. The check runs before any payload read.
  - The tables (DngColorProfile) have the profile name, identity calibration, unit analog
    balance, and baseline_exposure 0. The DCP fingerprint mixes the tables fingerprint, so it
    covers the complete table content.
  - Extra tests beyond the plan list: AcceptsTiffMagicFortyTwo,
    ProfileWithoutForwardMatricesMarksThemInvalid,
    ProfileToneCurveIsReadPastAndDoesNotChangeTheProfile (P-5), MissingColorMatrix1Fails,
    ReductionMatrixFails, FourChannelColorMatrixFails, IlluminantWithoutTemperatureFails,
    TagWithAnotherTypeFails, LookTableWithWrongEntryCountFails, FileAboveSizeLimitFailsBeforeRead,
    MissingFileFailsWithThePath.
  - The folder check is the test DcpFileReaderFolderTest.EveryFileOfTheUserFolderLoadsWithout
    Rejection. It reads the folder in the environment variable ALCEDO_DCP_FOLDER and skips when
    the variable is not set, as the private fixture tests do.
Explicitly unimplemented items: none in C1. Library index, camera match, and cache are C2.
Primary success call chain:
  ReadDcpCameraProfile(path)
    -> DcpIfdReader(path): file size limit, header (II/MM, 0x4352/42), entry table of IFD 0
    -> unsupported tag check (P-7)
    -> ReadRequiredString x2, ReadMatrix (CM1/CM2/FM1/FM2), ReadIlluminantCct
       -> CalibrationIlluminantToCct
    -> ReadMap x3 -> DcpIfdReader::ReadLongs/ReadFloats -> MakeDngHueSatMap
    -> MakeDngColorProfile(tables) -> MakeDcpCameraProfile -> DcpCameraProfilePtr
  ReadDcpProfileIdentity(path) -> DcpIfdReader(path) -> ReadRequiredString x2 -> identity
Primary failure and restore call chain:
  ReadDcpCameraProfile(path) -> DcpIfdReader::Payload: offset + size > file size
    -> DcpIfdReader::Fail -> std::runtime_error("DCP <path>: tag 50982 data is outside the
       file") -> no profile object. Errors of MakeDngHueSatMap, MakeDngColorProfile, and
       MakeDcpCameraProfile are thrown again with the path (and the data tag for a table).
       No state exists to restore.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DcpFileReaderTest
    DngColorProfileTest DngColorProfileCacheTest --parallel 4                       exit 0
  ctest --test-dir build/debug -R "DcpFileReaderTest|DngColorProfileTest|DngColorProfileCacheTest"
    --output-on-failure                                                             exit 0
  $env:ALCEDO_DCP_FOLDER = 'D:\Adobe\Adobe Lightroom Classic\Resources\CameraProfiles\Camera'
  build/debug/alcedo_studio/tests/image/DcpFileReaderTest_runtime/DcpFileReaderTest.exe exit 0
Discovered / passed / failed / skipped counts:
  ctest: 47 discovered, 47 passed, 0 failed (DcpFileReaderTest 24 with the folder test skipped,
  DngColorProfileTest 18, DngColorProfileCacheTest 5). Direct run with the folder: 24 / 24 passed.
Manual verification: folder run output "DCP files: 2445, cameras: 359, rejected: 0, identity
  read time: 3169 ms" (Debug build, identity reads only).
Performance or resource evidence, if required: not required in C1. The identity read time above
  is a Debug measurement; the scan time target of section 14 is a C2 Release measurement.
Evidence path: build/tmp/dcp_c1/ (build1.log, ctest1.log, folder_run.log), removed after the PR.
Remaining defects or unavailable platforms: macOS build of the new files not run in C1; the
  code uses only the C++20 standard library.
```

### C2. Library index, camera match, folder setting, and Settings page

**Objective and deliverables.** The user sets the folder in Settings. The page shows the profile
count and the camera count. `DcpProfileLibrary::Match` returns the profiles of a camera.

**Inputs and prerequisites.** C1.

**Modules, files, and APIs.** `dcp_profile_library.{hpp,cpp}`,
`camera_profile_library_service.{hpp,cpp}`, `CameraProfileSettingsPanel.qml` (proposed).
`ui/alcedo_main/qml/SettingDialog.qml` gets the category. `ALCEDO_MAIN_QML_FILES`, the module host
that registers `LutLibraryService`, and the CMake lists change. `DESIGN.md` changes only when the
page needs a new token.

**Data rules and invariants.**

- The setting key is `cameraProfiles/dcpLibraryRoot`. The default is an empty string.
- The scan walks the folder recursively and reads each file with the extension `.dcp`, compared
  without case.
- The index holds, for each profile: path, file size, last write time, `unique_camera_model`,
  `profile_name`, and the compact camera key.
- The index is in memory only. No file and no database table stores it.
- `Find` loads the full profile on the first request and keeps it in a least-recently-used cache
  of 32 profiles. The cache key is the path, the file size, and the last write time, as in
  `DngColorProfileCache`.
- `Match` and `Find` wait for a running scan (section 2.5).
- The match rule is section 2.4. `DcpCameraModelMatches` is a free function, so that tests call it
  without a library.

**Implementation steps.**

1. Add `DcpCameraModelMatches` and the make alias table. Build the table from the 359 camera
   names and the Exif makes in `camera_color_matrix_db.cpp`.
2. Add `DcpProfileLibrary` with `Scan`, `Match`, `Find`, `Root`, and the count readers. Publish
   the index under one mutex after the scan completes.
3. Add `CameraProfileLibraryService`. It reads the setting at start and starts one scan when the
   path is not empty. It exposes `rootPath`, `profileCount`, `cameraCount`, `rejectedCount`,
   `scanning`, and `errorText` as properties, and `setRootFolder(url)` and `rescan()` as
   invokables with lower-case names.
4. Register the service in the module host next to `LutLibraryService`.
5. Add `CameraProfileSettingsPanel.qml` per section 2.1 and the `SettingDialog.qml` category.
6. Measure the scan time of the user folder in a Release build and record it.

**Primary success call chain.**

```
CameraProfileSettingsPanel FolderDialog.onAccepted
  -> CameraProfileLibraryService::setRootFolder(url) -> QSettings write
  -> worker task: DcpProfileLibrary::Scan(path) -> ReadDcpProfileIdentity per file -> index published
  -> service properties change on the UI thread -> count tiles update
```

**Primary failure call chain.**

```
Scan(path) -> std::filesystem error for the folder
  -> scan result holds the error text, the previous index is cleared
  -> errorText property -> text below the folder row. The stored path stays.
```

**Tests and evidence.** Target `DcpProfileLibraryTest`, label `ci_core`.

| Test | Assertion |
| --- | --- |
| `ScanIndexesNestedFoldersAndCountsCameras` | Three cameras with seven files give profile count 7 and camera count 3. |
| `ScanSkipsAndCountsUnreadableFiles` | One truncated file gives rejected count 1, and the other files stay in the index. |
| `DuplicateIdentityKeepsFirstSortedPath` | `Find` returns the profile of the path that sorts first. |
| `MatchUsesMakeAliasAndModel` | Table-driven: (`SONY`, `ILCE-7M4`) matches `Sony ILCE-7M4`. (`NIKON CORPORATION`, `NIKON Z 6_2`) matches `Nikon Z 6_2`. (`Canon`, `Canon EOS R6`) matches `Canon EOS R6`. (`Panasonic`, `DC-S5`) matches `Panasonic DC-S5`. |
| `MatchRejectsNearNames` | (`Canon`, `Canon EOS R6`) does not match `Canon EOS R6 Mark II` or `Canon EOS R`. |
| `FindLoadsOnceAndReloadsAfterFileChange` | Two `Find` calls read the file once. A changed last write time causes one more read. |
| `FindReturnsNullForUnknownIdentity` | Null result and no exception. |
| `MissingFolderReportsErrorAndClearsIndex` | The result holds error text, and both counts are 0. |

The Settings page gets a C++ service test `CameraProfileLibraryServiceTest`:
`RootFolderPersistsInSettingsAndStartsOneScan` and `RescanRefreshesCountsAfterFileRemoval`. QML
layout is a manual check.

**Build and run commands.**

```
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DcpProfileLibraryTest CameraProfileLibraryServiceTest alcedo_main --parallel 4
ctest --test-dir build/debug -R "DcpProfileLibraryTest|CameraProfileLibraryServiceTest" --output-on-failure
```

**Exit criteria.**

- [x] Both suites pass.
- [ ] The Settings page shows 2445 profiles and 357 cameras for the user folder. The user
      confirms the page. (The service and the library report 2445 and 357 for the user folder;
      the page check by the user is open. 357, not 359: see the completion record.)
- [x] The recorded scan time of the user folder is below 2 seconds in a Release build. A higher
      value is a stop condition (section 12).
- [x] After the scan, the project database file has the same size and tables as before. (By
      inspection: the scan path links no storage library and writes only the QSettings key.)

**Expected diff.** 1300 to 1700 lines.

**Completion record.**

```text
Phase / date / status: C2 / 2026-11-12 / implemented; the user check of the Settings page is open
Source revision and branch: a80c4459a (C1, branch feature/dcp-file-reader), work branch
  feature/dcp-profile-library stacked on the C1 pull request
Actual changed modules: image/ (DcpProfileLibrary, camera match), app/
  (CameraProfileLibraryService), ui/alcedo_main/ (module host property cameraProfiles,
  CameraProfileSettingsPanel.qml, SettingDialog category), tests/image/, tests/app/, tests/ui/
  (category indices)
Implemented behavior:
  - include/image/dcp_profile_library.hpp, image/dcp_profile_library.cpp: DcpCameraMatchKeys,
    DcpUniqueCameraModelKey, DcpCameraModelMatches, DcpLibraryScanResult, DcpProfileLibrary
    (Shared, Scan, Match, Find, Root, ProfileCount, CameraCount, ProfileReadCount).
  - include/app/camera_profile_library_service.hpp, app/camera_profile_library_service.cpp:
    CameraProfileLibraryService (QObject) with the properties rootPath, profileCount,
    cameraCount, rejectedCount, errorText, scanning, and the invokables setRootFolder(url) and
    rescan(). One worker thread (ThreadPool of 1) runs the scans in request order.
  - ApplicationModuleHost constructs the service after the LUT services, calls Start(), shuts it
    down with the other library workers, and exposes it as appModules.cameraProfiles.
  - Settings category 9 "Camera Profiles" (camera.svg) between LUTs and AI Models. AI Models is
    now 10 and About is 11. The UI tests that name the indices changed with them.
Detail decisions made in this phase:
  - Camera count: the number of distinct compact camera keys. The user folder has 359
    UniqueCameraModel spellings and 357 cameras: "NIKON D810" and "Nikon D810", and "NIKON Z 7"
    and "Nikon Z 7", are one camera each under the normalization of section 2.3. Their profile
    names differ, so no profile is dropped (2445 profiles).
  - Identity key: the compact camera key and the exact profile name. Find compares the camera
    model by its compact key and the profile name exactly.
  - Make alias table: rows for olympus (Olympus, OM Digital Solutions), om (OM Digital
    Solutions, Olympus), ricoh (Ricoh, Pentax), pentax (Pentax, Ricoh), phase (Phase One),
    konica (Konica Minolta), eastman (Kodak). A make can give two profile makes, because one
    manufacturer sells bodies under two names (Ricoh GR and Pentax K; Olympus E-M and OM-1).
    Each other make uses its first word. This covers NIKON CORPORATION, LEICA CAMERA AG, SONY,
    FUJIFILM, SIGMA, and the 359 camera folders. The model alone is also a key, because an Exif
    model can hold the make ("NIKON Z 6_2", "Canon EOS R6", "PENTAX K-1").
  - Locking: the scan holds the index lock (std::shared_mutex) exclusively for its full
    duration. Match and Find take a shared lock, so they wait for a running scan (section 2.5)
    and see one complete index. Find releases the lock before the file read.
  - Find checks the file size and the last write time on each call (as DngColorProfileCache).
    It fails when the file now holds another identity than the index records, with a message
    that names the new identity. The failure leaves no cache entry.
  - A folder error (open or walk) clears the index and the rejected count. errorText takes
    std::error_code::message() through QString::fromLocal8Bit, because the Windows text is in
    the ANSI code page. An empty URL clears the setting and the counts with no error.
  - The busy indicator of section 2.1 is the spinning Rescan button, as on the LUT page. Browse
    is the button of FolderPathField. The Rejected tile shows when its count is above 0.
  - Log lines use the category alcedo.decode: one INFO line for each scan (counts and time), one
    WARN line for each rejected file (path and reason) and each duplicate identity (both paths).
  - Tests added to the plan list: MatchListsTheProfilesOfOneCameraByName,
    FindEvictsTheLeastRecentlyUsedProfile, FindFailsWhenTheFileNowHoldsAnotherIdentity,
    EmptyFolderPathClearsIndexWithoutError,
    DcpProfileLibraryFolderTest.UserFolderScanCountsProfilesAndCamerasWithoutRejection
    (ALCEDO_DCP_FOLDER), and the service tests MissingFolderShowsErrorAndKeepsStoredPath and
    EmptyFolderClearsSettingAndCounts. The match tests are in the suite DcpCameraMatchTest of
    the target DcpProfileLibraryTest.
  - tests/image/dcp_test_file_writer.hpp got MinimalDcpProfile (identity, ColorMatrix1,
    illuminant D65). The library tests and the service tests use it.
Explicitly unimplemented items: none in C2. The editor list model and the render binding are
  C4 and C5.
Primary success call chain:
  CameraProfileSettingsPanel FolderDialog.onAccepted -> page.useFolder(selectedFolder)
    -> CameraProfileLibraryService::setRootFolder(url) -> QSettings write and sync
       ("cameraProfiles/dcpLibraryRoot") -> RootChanged -> StartScan (scanning = true)
    -> worker thread: DcpProfileLibrary::Scan(path): exclusive index lock
       -> recursive_directory_iterator (skip_permission_denied), *.dcp without case
       -> sorted paths -> ReadDcpProfileIdentity for each file -> entries, by_identity_,
          by_camera_
    -> QMetaObject::invokeMethod (queued) -> PublishScanResult -> ScanResultChanged,
       ScanningChanged, ScanFinished -> CountTile Profiles and Cameras update
  DcpProfileLibrary::Match(make, model) -> DcpCameraMatchKeys -> by_camera_ -> sorted identities
  DcpProfileLibrary::Find(model, name) -> shared lock: by_identity_ -> file stamp
    -> LRU hit, or the loader (ReadDcpCameraProfile) with no lock -> identity check -> LRU insert
Primary failure and restore call chain:
  Scan(path) -> directory iterator error (missing folder, removed drive)
    -> index cleared, result.error = OS text -> PublishScanResult -> errorText -> AlertBadge
       below the folder row; counts 0; the QSettings path stays; Rescan reads it again later.
  Scan -> ReadDcpProfileIdentity throws -> rejected_count + 1, WARN log line, the scan continues.
  Find -> file changed to another identity -> std::runtime_error that names the new identity, no
    cache entry. Find -> file removed -> std::runtime_error with the path and the OS text.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH=...             exit 0
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DcpProfileLibraryTest
    CameraProfileLibraryServiceTest DcpFileReaderTest --parallel 4                    exit 0
  ctest --test-dir build/debug -R "DcpProfileLibraryTest|CameraProfileLibraryServiceTest|
    DcpFileReaderTest" --output-on-failure                                            exit 0
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main
    ApplicationModuleHostLifecycleTest SettingsAboutQmlTest AiModelsSettingsQmlTest
    ShortcutSettingsQmlTest LutSettingsQmlTest MainQmlWorkflowTest --parallel 4       exit 0
  ctest --test-dir build/debug -R "ApplicationModuleHostLifecycleTest|SettingsAboutQmlTest|
    AiModelsSettingsQmlTest|ShortcutSettingsQmlTest|LutSettingsQmlTest" -j 1           exit 8
    (1 failure, issue #585, see below)
  Release measurement: win_release configured with -DALCEDO_BUILD_TESTS=ON only to build
    DcpProfileLibraryTest, then configured again with -DALCEDO_BUILD_TESTS=OFF.
Discovered / passed / failed / skipped counts:
  New suites: 41 discovered, 39 passed, 0 failed, 2 skipped (the two folder tests without
  ALCEDO_DCP_FOLDER): DcpProfileLibraryTest 13 (1 skipped), CameraProfileLibraryServiceTest 4,
  DcpFileReaderTest 24 (1 skipped).
  Settings UI and host suites: 33 discovered, 32 passed, 1 failed:
  ShortcutSettingsQmlTest.RegistryRowsAppearInStableGroups expects no Comparison shortcut group
  (open issue #585, present before this change).
  MainQmlWorkflowTest.ProductionWindowLoadsAndRoutesCoreWorkspaceActions fails at line 136,
  before the changed About index (open issue #722).
Manual verification: Debug run of DcpProfileLibraryFolderTest with ALCEDO_DCP_FOLDER set to the
  Lightroom Classic CameraProfiles\Camera folder: "2445 profiles, 357 cameras, 0 rejected,
  1177 ms". The user has not checked the Settings page yet.
Performance or resource evidence, if required: Release build, three runs of the same scan:
  232 ms, 221 ms, 209 ms (2445 files, 357 cameras, 0 rejected). Target: below 2000 ms.
Evidence path: build/tmp/dcp_camera_profile/c2/ (configure, build, and ctest logs), removed
  after the pull request.
Remaining defects or unavailable platforms: macOS build of the new files not run; the code uses
  the C++20 standard library and Qt Core only. The Settings page layout needs the user check.
```

### C3. Profile reference in the Develop payload and the effective profile in the render

**Objective and deliverables.** A Develop payload with a bound DCP profile renders with the
matrices and tables of that profile on the scalar reference path. JSON stores the reference only.

**Inputs and prerequisites.** C1.

**Modules, files, and APIs.** `include/edit/graph/develop_node_model.hpp`,
`edit/graph/develop_node_model.cpp`, `edit/graph/develop_color_transform.{hpp,cpp}`,
`edit/runtime/dng_profile_gpu_data.cpp`, `edit/runtime/result_content_key.cpp`.

**Data rules and invariants.**

- `ToJson` writes `camera_profile_file` only when the optional has a value. Its value is
  `{"camera_model", "profile_name", "fingerprint"}`.
- `LoadJson` gives an unbound reference. It keeps a bound profile when the fingerprint equals the
  JSON fingerprint, as it does for `dng_profile_fingerprint`.
- Equality of two references compares `camera_model`, `profile_name`, and `fingerprint`.
- `EffectiveCameraProfile(payload)` is the only reader of matrices and tables for the render:
  - no reference: the values of `camera_profile`;
  - bound reference: matrices, forward matrices, and CCT values of the DCP profile, the tables
    of the DCP profile, and `AsShotNeutral` and `cam_mul` of `camera_profile`;
  - unbound reference: an error.
- `camera_profile` keeps its import values while a reference is set.
- `ApplyCameraProfileFileUpdate` sets or clears the reference, solves `as_shot_cct` and
  `as_shot_tint` from the effective matrices, and marks `WhiteBalance` dirty when a value changes.
  It throws `std::logic_error` for a raster document and leaves the payload unchanged.
- `UseSensorSettingsFrom` and `UseAllSettingsFrom` copy the reference with the other camera
  fields.
- `MixCameraColorParams` mixes the reference fingerprint. The sensor key stays independent of it.

**Implementation steps.**

1. Add `DevelopCameraProfileFile`, the payload member, the equality, and the JSON read and write.
2. Move the as-shot solve of `BindDevelopCameraProfile` into a function that takes matrices and
   the neutral. `BindDevelopCameraProfile` and `ApplyCameraProfileFileUpdate` call it.
   `BindDevelopCameraProfile` solves with the effective matrices when a bound reference exists.
3. Add `EffectiveCameraProfile` to `develop_color_transform.hpp`.
4. Change `ResolveDevelopColorTransform` to read the effective values. Add
   `ColorTransformError::UnboundCameraProfileFile` and its message (section 5.4).
5. Change `PackDngProfileGpuData` to read the effective tables and the effective CCT values.
6. Mix the fingerprint in `MixCameraColorParams`.
7. Add `DevelopCameraProfileFileUpdate` and `ApplyCameraProfileFileUpdate`.

**Primary success call chain.**

```
ApplyCameraProfileFileUpdate(reference + DcpCameraProfilePtr)
  -> payload.camera_profile_file set, as-shot values solved, WhiteBalance dirty
ResolveDevelopColorTransform(payload) -> EffectiveCameraProfile -> DCP ColorMatrix and ForwardMatrix
PackDngProfileGpuData(payload, transform) -> DCP look table in the buffer
```

**Primary failure call chain.**

```
LoadJson(JSON with camera_profile_file) -> unbound reference
ResolveDevelopColorTransform -> ColorTransformError::UnboundCameraProfileFile -> no transform
PackDngProfileGpuData -> throws the same message
```

**Tests and evidence.** Target `DcpCameraProfileRenderTest` (scalar, no GPU), label `ci_core`.

| Test | Assertion |
| --- | --- |
| `DocumentWithoutReferenceSerializesAsBefore` | `ToJson().dump()` of a default RAW payload equals the stored text `expected_develop_without_camera_profile_file.json`, captured at revision `c54ac05fa`. |
| `JsonStoresReferenceAndNoProfileValues` | The JSON holds the three reference keys. No DCP matrix element and no table value is in the JSON. |
| `BoundReferenceReplacesMatricesAndKeepsAsShotNeutral` | The resolved camera-to-XYZ matrix equals the matrix computed from the DCP forward matrices. `camera_profile` equals its value before the update. |
| `BoundReferencePacksDcpLookTable` | The packed buffer descriptor and entries equal the DCP look table. |
| `ClearingReferenceRestoresImportTransform` | The transform after set and clear equals the transform before set. |
| `UnboundReferenceFailsWithNamedError` | The error is `UnboundCameraProfileFile`, and the message holds the camera model and the profile name. |
| `ProfileChangeKeepsSensorKeyAndChangesCameraKey` | The sensor content key is equal before and after. The `develop_image` key differs. |
| `AsShotValuesFollowEffectiveMatrices` | `as_shot_cct` after the update equals the solve with the DCP matrices. `custom_cct` is unchanged. |
| `RasterDocumentRejectsUpdate` | `std::logic_error`, and the payload is unchanged. |
| `ComparisonDocumentTakesReferenceFromCurrent` | After `UseSensorSettingsFrom`, the reference and its bound profile equal those of the current Model. |

**Build and run commands.**

```
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DcpCameraProfileRenderTest DngColorProfileTest --parallel 4
ctest --test-dir build/debug -R "DcpCameraProfileRenderTest|DngColorProfileTest|DevelopCameraProfileImport" --output-on-failure
```

**Exit criteria.**

- [x] `DcpCameraProfileRenderTest` passes.
- [x] The current Develop model, color transform, and content key suites pass unchanged.
- [x] A project made before this phase opens: the root id check in `TryDecodeRootState` succeeds
      for a stored root in `PipelineDngProfileBindingTest`.

**Expected diff.** 1100 to 1500 lines.

**Completion record.**

```text
Phase / date / status: C3 / 2026-11-12 / complete
Source revision and branch: 7fb3403e1 (C2, branch feature/dcp-profile-library), work branch
  feature/dcp-profile-develop-reference stacked on the C2 pull request
Actual changed modules: edit/graph/ (Develop payload, Model, color transform), edit/runtime/
  (table packing, content keys, the camera color pass of CUDA, OpenCL, and Metal: message and
  packing call only), tests/edit/, tests/resources/expected_json/
Implemented behavior:
  - include/edit/graph/develop_node_model.hpp, edit/graph/develop_node_model.cpp:
    DevelopCameraProfileFile (camera_model, profile_name, fingerprint, DcpCameraProfilePtr
    profile; equality without the pointer), MakeDevelopCameraProfileFile,
    DevelopCameraProfileFileToJson, DevelopCameraProfileFileFromJson,
    DevelopPayload::camera_profile_file (std::optional), DevelopCameraProfileFileUpdate,
    DevelopParamsModel::ApplyCameraProfileFileUpdate and CameraProfileFile. ToJson writes the key
    only when the optional has a value. LoadJson keeps the bound profile for an equal reference
    and gives an unbound reference otherwise; an absent key clears the reference. ReplaceParams
    installs the bound profile of an equal reference (the C4 binding step uses this) and marks
    WhiteBalance dirty for a changed reference. IsDefault is false while a reference is set.
  - include/edit/graph/develop_color_transform.hpp, edit/graph/develop_color_transform.cpp:
    EffectiveCameraProfile, SolveDevelopAsShotWhiteBalance, ColorTransformFailureMessage,
    ColorTransformError::UnboundCameraProfileFile. ResolveDevelopColorTransform returns the new
    error for an unbound reference and otherwise reads every matrix, CCT value, and table from
    EffectiveCameraProfile. BindDevelopCameraProfile solves the as-shot values through
    SolveDevelopAsShotWhiteBalance, so a bound reference uses the DCP matrices.
  - edit/runtime/dng_profile_gpu_data.cpp: PackDngProfileGpuData(const DevelopPayload&,
    transform) packs the tables of the effective profile. The three backends call it and throw
    ColorTransformFailureMessage. The kernels do not change.
  - edit/runtime/result_content_key.cpp: MixCameraColorParams mixes the reference fingerprint
    when a reference is set. The sensor key does not read it.
Detail decisions made in this phase:
  - EffectiveCameraProfile returns a DevelopCameraProfile value: the existing representation
    that every matrix reader of develop_color_transform.cpp takes. It is a computed result; the
    payload member camera_profile keeps the import values. With a bound reference: ColorMatrix1/2,
    ForwardMatrix1/2, the two CCT values, calibration_illuminants_valid = true, and the DCP tables
    as dng_profile (identity AnalogBalance and CameraCalibration). AsShotNeutral and cam_mul stay.
    The render therefore takes the DNG branch of the transform (ForwardMatrix normalization to
    D50, BaselineExposureOffset of the DCP in the table header).
  - An embedded DNG profile of the source is not read while a DCP reference is bound. Phase C6
    adds the source calibration of a DNG source.
  - DevelopCameraProfileFileUpdate holds only the DcpCameraProfilePtr. The reference values come
    from the profile itself, so an update cannot name one profile and carry another. A null
    profile clears the reference.
  - Step 2: the as-shot solve became SolveDevelopAsShotWhiteBalance(DevelopPayload&). It reads
    the matrices and the neutral through ResolveDevelopColorTransform, which reads
    EffectiveCameraProfile, so one function serves the import binding and the update.
  - PackDngProfileGpuData keeps its DevelopCameraProfile overload (the current DNG tests call it)
    and gets the DevelopPayload overload that the backends call.
  - The render message is: camera profile "<profile_name>" for "<camera_model>" is not in the DCP
    folder. edit/graph/ has no access to the folder setting, so the message has no path. The C4
    binding step logs the folder path when the library has no such identity.
  - A malformed camera_profile_file value (not an object, a missing or empty string, a fingerprint
    that is not 16 lowercase hexadecimal digits) fails LoadJson with std::runtime_error, as the
    DNG profile fingerprint does.
  - The content key mixes nothing for a document without a reference, so the current keys stay.
  - The stored text expected_develop_without_camera_profile_file.json is the ToJson output of a
    RAW payload with the import matrices of FillTestCameraProfile and default settings, captured
    with a temporary test from the unchanged serializer (edit/ has no change between c54ac05fa and
    7fb3403e1) before the production change.
  - Tests added to the plan list: LoadJsonKeepsBoundProfileOnlyForEqualReference,
    MalformedReferenceJsonIsRejected, ReplaceParamsInstallsBoundProfileOfEqualReference. The
    suite name in the binary is DcpCameraProfileRender.
Explicitly unimplemented items: none in C3. The Develop field, history, and load-path binding are
  C4; the DNG source calibration is C6; the GPU comparison is C7.
Primary success call chain:
  DevelopParamsModel::ApplyCameraProfileFileUpdate({DcpCameraProfilePtr})
    -> MakeDevelopCameraProfileFile (identity and fingerprint from the profile)
    -> MutateWithDirtyFields: raster check -> payload.camera_profile_file = bound reference
    -> SolveDevelopAsShotWhiteBalance -> ResolveDevelopColorTransform (as_shot)
       -> EffectiveCameraProfile -> DCP matrices and CCT values -> as_shot_cct, as_shot_tint
    -> WhiteBalance stamp
  Render: ExecuteCudaCameraColor / ExecuteOpenClCameraColor / ExecuteMetalCameraColor
    -> ResolveDevelopColorTransform(payload) -> EffectiveCameraProfile -> DNG branch with the DCP
       ForwardMatrix -> camera_to_ap1
    -> PackDngProfileGpuData(payload, transform) -> EffectiveCameraProfile -> DCP maps blended by
       the DCP CCT values, DCP look table, exp2(BaselineExposureOffset)
    -> DngApplyColorProfile in the unchanged kernel
  Content keys: BuildFrameResultContentKeys -> MixCameraColorParams -> reference fingerprint in
    develop_image and downstream keys; sensor_linear and geometry keys unchanged
Primary failure and restore call chain:
  DevelopParamsModel::LoadJson(JSON with camera_profile_file) -> unbound reference
    -> ResolveDevelopColorTransform -> ColorTransformError::UnboundCameraProfileFile
    -> backend throws "Execute...CameraColor: camera profile "<name>" for "<model>" is not in the
       DCP folder" -> no render with another profile
    -> PackDngProfileGpuData(payload, ...) throws the same message; RawWhiteBalanceCct is empty
  ApplyCameraProfileFileUpdate on a raster document -> std::logic_error before any write; payload
    and stamps unchanged.
  ApplyCameraProfileFileUpdate({}) -> reference cleared -> as-shot values solved from the import
    matrices again -> transform and packed tables equal the values before the selection.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DcpCameraProfileRenderTest
    DngColorProfileTest --parallel 4                                                  exit 0
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditRuntimeCuda
    EditRuntimeOpenCl GpuDagModelGraphTest GpuDagRawInputTest PipelineDngProfileBindingTest
    ImportPipelineDocumentTest --parallel 4                                           exit 0
  ctest --test-dir build/debug -R "DcpCameraProfileRenderTest|DngColorProfileTest|
    GpuDagModelGraphTest|GpuDagResultContentKey|PipelineDngProfileBindingTest|
    ImportPipelineDocumentTest" -j 4 --output-on-failure                             exit 0
  After clang-format and the IsDefault change: the same build of DcpCameraProfileRenderTest,
    DngColorProfileTest, GpuDagModelGraphTest, EditRuntimeCuda, EditRuntimeOpenCl (exit 0) and
    ctest -R "DcpCameraProfileRenderTest|DngColorProfileTest|GpuDagModelGraphTest"   exit 0
Discovered / passed / failed / skipped counts:
  245 discovered, 245 passed, 0 failed, 0 skipped: DcpCameraProfileRenderTest 13,
  DngColorProfileTest 18, GpuDagModelGraphTest 182 (Develop model, JSON, color transform,
  imported camera profile JSON), GpuDagRawInputTest.GpuDagResultContentKey 25,
  PipelineDngProfileBindingTest 5 (stored root decoded through TryDecodeRootState),
  ImportPipelineDocumentTest 2. Second run: 213 of 213 passed.
Manual verification: none needed; no user-visible change in this phase.
Performance or resource evidence, if required: not required.
Evidence path: build/tmp/dcp_camera_profile/c3/ (build and ctest logs), removed after the pull
  request.
Remaining defects or unavailable platforms: the Metal pass (metal_develop_pass.mm) got the same
  two-line change as the CUDA and OpenCL passes; the macOS build was not run. C7 runs the three
  backends.
LOC note: develop_color_transform.cpp 938 lines, develop_node_model.cpp 573,
  develop_node_model.hpp 411, dcp_camera_profile_render_test.cpp 467. The transform file is near
  1000 lines; a further addition there needs a split by responsibility (white-balance solve
  apart from the matrix resolve).
```

### C4. Develop field, history, and binding on each load path

**Objective and deliverables.** A `camera_profile_file` edit goes through the editor command
path and history. Each load path binds the reference from the library. The project holds the
reference only.

**Inputs and prerequisites.** C2 and C3.

**Modules, files, and APIs.** `app/pipeline_history_applier.cpp`,
`app/editor_parameter_write.cpp`, `app/editor_parameter_write_parse.cpp`,
`app/editor_adjustment_pipeline.cpp`, `app/editor_adjustment_context.cpp`,
`app/editor_pipeline_command_service.cpp`, `app/editor_panel_projection.cpp`,
`app/pipeline_root_state.cpp`, `dcp_camera_profile_binding.{hpp,cpp}` (proposed). The transfer
catalogs (`adjustment_transfer_catalog.cpp`, `adjustment_transfer_package_builder.cpp`,
`document_transfer_planner.cpp`) get the new key in their exclusion lists.

**Data rules and invariants.**

- The field `camera_profile_file` owns the Develop key `camera_profile_file` only. A patch value
  is `{"camera_model", "profile_name"}` to select, or `null` to clear.
- The write path resolves the identity through `DcpProfileLibrary::Shared().Find`. An identity
  that the library does not hold rejects the edit with an error. The document is unchanged.
- The stored fingerprint is the fingerprint of the resolved profile.
- History stores the reference JSON. Replay, Undo, Redo, and checkout apply the reference and
  then bind it through `BindDcpCameraProfile`.
- `BindRootCameraProfile` calls `BindDcpCameraProfile` after `BindImportedCameraProfile`. This
  covers `BuildDocumentFromRoot` and `BuildDocumentFromCheckpoint`, and through them the editor
  lease, the committed document for thumbnails and exports, and the preview documents.
- In an editor session, an Undo that applies a batch directly on the working document binds in
  the same apply step, inside `ApplyEditorParameterValue`.
- A fingerprint mismatch follows section 2.3: the library file wins, with one warning.
- The root document never holds a reference, because import does not select a profile.

**Implementation steps.**

1. Add the field to `DevelopFieldOwnedKeys`, to the wrapper list of `FieldOwnedParameterJson`,
   and to `EditorAdjustmentField`.
2. Add `ParseCameraProfileFileUpdate` and the branch in `ApplyEditorParameterValue`. The branch
   resolves the profile and calls `ApplyCameraProfileFileUpdate`.
3. Add the field to the two lists that treat Develop fields as camera-pass edits
   (`editor_pipeline_command_service.cpp:232`, `editor_adjustment_context.cpp:291`) and to the
   field list at `editor_adjustment_context.cpp:58`.
4. Add `ReadCameraProfileFile` to the panel projection table.
5. Add `BindDcpCameraProfile` and the call in `BindRootCameraProfile`.
6. Add the key to the three transfer exclusion lists.
7. Confirm with a test that `WriteElementPipelineJson` and the checkpoint encoder write the
   reference and no profile value.

**Primary success call chain.**

```
submitPatch("camera_profile_file", {camera_model, profile_name}, settled = true)
  -> EditorSessionService::CommitAdjustment -> EditorSessionEditController::HandlePatch
  -> EditorHistoryMutation::CommitAdjustment -> MakeSetParameterBatch(before, after)
  -> ApplyEditorParameterValue -> DcpProfileLibrary::Find -> ApplyCameraProfileFileUpdate
  -> MiniGitWorkingHistory::AppendEdit -> render with the new camera pass key

Reopen
  -> AcquireEditorLease -> BuildDocumentFromCheckpoint -> BindRootCameraProfile
  -> BindImportedCameraProfile -> BindDcpCameraProfile -> bound reference -> render
```

**Primary failure and restore call chain.**

```
submitPatch with an identity that is not in the library
  -> ApplyEditorParameterValue returns false with the error text
  -> no batch is appended, the working document keeps the previous reference

Reopen with the folder not set
  -> BindDcpCameraProfile: Find returns null -> reference stays unbound
  -> render: UnboundCameraProfileFile -> the user sets the folder or selects the default entry
  -> selecting the default entry commits a clear edit, and the render uses the import profile
```

**Tests and evidence.** Target `PipelineDcpProfileBindingTest`, label `ci_core_flow`. Each test
uses a temporary library folder with synthetic DCP files and `Find` on a library instance that
the test injects.

| Test | Assertion |
| --- | --- |
| `CommittedSelectionStoresReferenceOnlyInProjectTables` | After the commit and the editor release, a scan of each project table text finds the reference keys and finds no element of the DCP look table. |
| `EditorLeaseCheckpointAndReplayBindTheReference` | Both load paths give a bound reference with the library fingerprint. |
| `CommittedDocumentForExportBindsTheReference` | The committed document resolves a transform equal to the editor document transform. |
| `UndoRestoresImportProfileAndRedoRestoresDcpProfile` | The effective matrices equal the import values after Undo and the DCP values after Redo. |
| `CheckoutOfEarlierVersionClearsTheReference` | The version before the selection has no reference. |
| `SelectionOfUnknownIdentityIsRejectedAndDocumentIsUnchanged` | The command fails, the history length is equal, and the document JSON is equal. |
| `MissingLibraryLeavesReferenceUnboundAndRenderFails` | The transform error is `UnboundCameraProfileFile`. |
| `ChangedLibraryFileWinsAndLogsOneWarning` | The bound fingerprint is the new one, and the log holds one warning. |
| `TransferAndPasteDoNotCarryTheReference` | The target document has no reference after a paste of all Develop fields. |
| `RootIdOfProjectMadeBeforeThisPlanStillValidates` | A stored root from a fixture project decodes with `TryDecodeRootState`. |

**Build and run commands.**

```
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target PipelineDcpProfileBindingTest PipelineDngProfileBindingTest --parallel 4
ctest --test-dir build/debug -R "PipelineDcpProfileBindingTest|PipelineDngProfileBindingTest|PipelineHistoryApplier" --output-on-failure
```

**Exit criteria.**

- [ ] `PipelineDcpProfileBindingTest` passes.
- [ ] `PipelineDngProfileBindingTest` and the history applier suites pass unchanged.
- [ ] The project file of a session with one selection has no DCP table data. The record states
      the search command.

**Expected diff.** 1200 to 1600 lines.

**Completion record.** Empty.

### C5. Editor section and list model

**Objective and deliverables.** The RAW panel shows the Camera Profile section of section 2.2.
The user selects a matched profile and sees the render change.

**Inputs and prerequisites.** C4.

**Modules, files, and APIs.** `EditorCameraProfileListModel` and
`EditorCameraProfileSection.qml` (proposed). `EditorRawDecodePanel.qml` hosts the section and
restores it in `loadFromSnapshot`. `editor_adjustment_models.cpp` registers the model.
`app/editor_adjustment_context.cpp` gives the model the camera make, the camera model, and
whether the source has an embedded profile.

**Data rules and invariants.**

- The model reads the camera identity from the RAW color context of the root state. It does not
  parse the source file again.
- Row roles: `label`, `cameraModel`, `profileName`, `isDefault`, `isMissing`.
- The model rebuilds its rows when the active image changes and when the library index changes.
  It does not rebuild on a selection.
- The current row comes from the panel projection (`ReadCameraProfileFile`), not from the last
  click.
- A stored reference that the index does not hold adds one row with `isMissing` true, so that
  the combo shows the stored name.
- The section does not keep a copy of the selection. `loadFromSnapshot` sets the current index.

**Implementation steps.**

1. Add the model with a `QAbstractListModel` base and the roles above.
2. Connect the model to the index-changed signal of `CameraProfileLibraryService`.
3. Add the section QML with `AdjustmentCombo`, the missing color state, and the **Choose Folder**
   button. Register it in `ALCEDO_MAIN_QML_FILES`.
4. Add the section to `EditorRawDecodePanel.qml` above `EditorWhiteBalanceSection`, with
   `visible: !root.rasterInput`.
5. Add the snapshot read of `camera_profile_file` to `loadFromSnapshot`.
6. Add the user-visible strings to the translation sources by hand. Do not run `lupdate` on a
   partial scan.

**Primary success call chain.**

```
Editor opens an ARW of a Sony ILCE-7M4
  -> EditorCameraProfileListModel rows = [Built-in] + Match("SONY", "ILCE-7M4")
  -> user selects "Camera ST" -> submitPatch("camera_profile_file", {...}, true)
  -> C4 success chain -> snapshot publish -> loadFromSnapshot sets the combo index
```

**Primary failure call chain.**

```
C4 rejects the patch (file removed after the scan)
  -> the session reports the error through the current command error path
  -> the next snapshot holds the previous reference -> the combo returns to the previous row
```

**Tests and evidence.** Target `EditorCameraProfileListModelTest` (C++ model test).

| Test | Assertion |
| --- | --- |
| `RowsAreDefaultThenMatchedProfilesInNameOrder` | Row 0 is the default row. The other rows are sorted by name. |
| `DngSourceWithEmbeddedProfileNamesDefaultRowEmbedded` | The label of row 0 is `Embedded`. |
| `UnresolvedStoredReferenceAddsMissingRow` | One row has `isMissing` true and the stored name. |
| `RowsRefreshAfterLibraryRescan` | A new file in the folder gives one more row after `rescan`. |
| `RasterDocumentHasNoRows` | Row count 0. |

QML behavior is a manual check with the user: selection, Undo, Redo, version checkout, image
change, missing reference state, and the **Choose Folder** button.

**Build and run commands.**

```
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorCameraProfileListModelTest alcedo_main --parallel 4
ctest --test-dir build/debug -R "EditorCameraProfileListModelTest" --output-on-failure
```

**Exit criteria.**

- [ ] The model test passes.
- [ ] The user confirms the section on one RAW file of each of two camera makes.
- [ ] No QML warning for the new files in a run with `QT_FORCE_STDERR_LOGGING=1`.

**Expected diff.** 900 to 1300 lines.

**Completion record.** Empty.

### C6. DCP profile on a DNG source

**Objective and deliverables.** A DNG file renders with a selected DCP profile and keeps its own
camera calibration.

**Inputs and prerequisites.** C4.

**Modules, files, and APIs.** `image/dng_color_profile_cache.{hpp,cpp}`,
`image/metadata/dng_color_metadata.cpp`, `app/dcp_camera_profile_binding.cpp`,
`edit/graph/develop_color_transform.cpp`.

**Data rules and invariants.**

- The effective tables of a DNG source with a DCP reference are a merged `DngColorProfile`:
  `AnalogBalance` and `BaselineExposure` of the source, the maps, the look table, the encodings,
  and `BaselineExposureOffset` of the DCP profile.
- `CameraCalibration1/2` of the source apply when the `CameraCalibrationSignature` of the source
  equals the `ProfileCalibrationSignature` of the DCP profile
  (`CameraCalibrationSignaturesMatch`). When they differ, the calibration is identity.
- `DngColorProfileCache` keeps the `CameraCalibrationSignature` and the unconditioned
  `CameraCalibration` matrices of the source beside the profile. They are runtime data. They are
  outside `DngColorProfileToJson`, so each stored DNG fingerprint stays valid.
- `DcpProfileLibrary` keeps merged profiles in a map with the key (source profile fingerprint,
  DCP fingerprint) and weak values, so that one merged object serves all documents of one image.
- The bound runtime pointer of the reference holds the merged tables for a DNG source.
- A DNG source without an embedded profile uses the C3 rule.

**Implementation steps.**

1. Extend the cache loader result with the source signature and matrices.
2. Add `MergeDcpTablesWithSourceCalibration` in `image/`.
3. Extend `BindDcpCameraProfile`: for a document whose `camera_profile.dng_profile` is
   referenced, build or read the merged profile and bind it.
4. Extend `EffectiveCameraProfile` to return the merged tables when they are bound.

**Primary success call chain.**

```
BindRootCameraProfile -> BindImportedCameraProfile (embedded profile bound)
  -> BindDcpCameraProfile -> DcpProfileLibrary::Find -> merged profile from the map or a new merge
  -> payload reference bound with merged tables -> render
```

**Primary failure call chain.**

```
Source DNG file missing -> LoadSourceDngColorProfile throws (current behavior) -> load fails
```

**Tests and evidence.** Additions to `PipelineDcpProfileBindingTest` and
`DcpCameraProfileRenderTest`.

| Test | Assertion |
| --- | --- |
| `DngSourceKeepsAnalogBalanceAndBaselineExposureWithDcpProfile` | The merged profile holds the source values and the DCP look table. |
| `CameraCalibrationAppliesOnlyWhenSignaturesMatch` | Equal signatures give the source matrices. Different signatures give identity. |
| `MergedProfileIsSharedByTwoDocumentsOfOneImage` | Both documents hold the same pointer. |
| `StoredDngFingerprintIsUnchangedByThisPhase` | The fingerprint of the fixture profile equals the value before the phase. |
| `ClearingReferenceOnDngSourceRestoresEmbeddedProfile` | The transform and the packed tables equal those before the selection. |

**Build and run commands.** As C4, with `DngColorProfileCacheTest` added.

**Exit criteria.**

- [ ] The new tests pass.
- [ ] `DngColorProfileCacheTest` and `PipelineDngProfileBindingTest` pass unchanged.

**Expected diff.** 600 to 900 lines.

**Completion record.** Empty.

### C7. Backend verification, user review, and documents

**Objective and deliverables.** Evidence that the three GPU backends render a DCP profile equal to
the scalar reference, a user review of real images, and current documents.

**Inputs and prerequisites.** C5 and C6.

**Modules, files, and APIs.** `tests/edit/runtime/dng_profile_test_support.hpp`,
`cuda_develop_test.cpp`, `opencl_develop_test.cpp`, `metal_develop_test.cpp`,
`docs/dng_color_profiles.md`, `docs/roadmap/README.md`.

**Data rules and invariants.**

- The GPU test uses a synthetic DCP file from the C1 writer and a RAW file from
  `sample_image/raw/`. It does not use a vendor profile.
- The comparison is scalar `DngApplyColorProfile` against GPU samples with the tolerance that
  `VerifyCanonDngProfile` uses. No stored expected pixels are added.
- A missing private fixture gives an explicit skip.

**Implementation steps.**

1. Add `template <class Device> VerifyDcpCameraProfile(const char* backend)` to the test support
   header: render with the default profile, select the synthetic profile, render again, compare
   samples with the scalar reference, and check that the sensor pass result is reused.
2. Call it from the three backend suites.
3. Run CUDA and OpenCL on Windows. Run Metal on the Mac. Check the free space of the Mac first.
4. User review: three RAW files from `sample_image/raw/` with a profile of the user folder,
   viewed in the application. The record states the user verdict.
5. Update `docs/dng_color_profiles.md`: add a section for external DCP profiles, add the
   reference to the storage table, and remove external DCP selection from the list of items that
   are not implemented.

**Tests and evidence.**

| Test | Assertion |
| --- | --- |
| `CudaDevelopTest.DcpProfileMatchesScalarReference` | GPU samples equal the scalar result within the tolerance. The second render reuses the sensor result. |
| `OpenClDevelopTest.DcpProfileMatchesScalarReference` | The same. |
| `MetalDevelopTest.DcpProfileMatchesScalarReference` | The same. |

**Build and run commands.**

```
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target CudaDevelopTest OpenClDevelopTest --parallel 4
ctest --test-dir build/debug -j 1 -R "DevelopTest.*Dcp" --output-on-failure
```

On the Mac: configure `macos_debug` with `-DALCEDO_BUILD_TESTS=ON`, build `MetalDevelopTest`, and
run the same filter.

**Exit criteria.**

- [ ] The three tests pass, or the record states the unavailable backend.
- [ ] The user review record exists.
- [ ] `docs/dng_color_profiles.md` describes the shipped behavior.

**Expected diff.** 500 to 800 lines.

**Completion record.** Empty.

---

## 9. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Read a valid DCP file of each byte order | Equal profile content and fingerprint | C1 |
| Read an invalid file | Error with path and tag, no profile object | C1 |
| Scan the user folder | 2445 profiles, 357 cameras (359 `UniqueCameraModel` spellings), 0 rejected, below 2 seconds | C2 |
| Match a camera | Exact normalized names only | C2 |
| Select a profile | Camera pass renders again. The sensor result is reused. | C3, C4 |
| Clear the selection | The render equals the render before the selection | C3 |
| Undo, Redo, checkout | The reference follows the version | C4 |
| Close and open the project | The same profile is bound from the folder | C4 |
| Open with the folder not set | Render error that names the profile. No substitute render. | C4 |
| Inspect the project database and package | Reference keys only | C4 |
| Open a project made before this plan | Same bytes, same root id, no error | C3, C4 |
| Export and thumbnail | Use the bound profile | C4 |
| DNG source | Source calibration with DCP tables | C6 |
| CUDA, OpenCL, Metal | Equal to the scalar reference within the tolerance | C7 |
| Raster document | No section, update rejected | C3, C5 |

---

## 10. Repository rules that apply

- No fallback (`AGENTS.md`): an unresolved reference fails the render (P-4).
- Update existing data through its owner: `camera_profile` stays the import value, and the
  reference changes only through `ApplyCameraProfileFileUpdate`.
- No consistency mechanism without an executable interleaving: the scan publishes one complete
  index, and this plan adds no version, token, or cancel protocol.
- Include the defining header. No forward declaration to skip an include.
- LF line endings. New files are formatted completely. Existing files get `git clang-format` on
  the changed lines only.
- Tests use the debug build. No agent runs the full test suite.
- No stored expected pixels. No vendor profile bytes in fixtures.
- New GPU work is for CUDA, OpenCL, and Metal only.
- SQL goes through duckorm. This plan adds no SQL and no table.
- QML follows the `alcedo-qml-ui` skill: Basic style, `appTheme` tokens, neutral buttons, no
  explanatory text, and registration in `ALCEDO_MAIN_QML_FILES`.

---

## 11. Build and evidence rules

- Presets: `win_debug` on Windows and `macos_debug` on macOS, with the wrappers of `AGENTS.md`.
- Run each MSVC command through PowerShell.
- Add `build/debug/vcpkg_installed/x64-windows/debug/bin` to `PATH` before a test run.
- Run GPU tests with `ctest -j 1`.
- Put logs and evidence in `build/tmp/dcp_camera_profile/<phase>/`.
- Record pass, fail, skip, and unavailable separately. A skipped test is not a pass.
- The full test suite is not part of any phase.

---

## 12. Risks and stop conditions

| Risk | Detection signal | Response |
| --- | --- | --- |
| The scan of a large folder is slow | C2 measurement above 2 seconds | Stop. Update section 2.5 with a measured design before C4. |
| A camera name does not match its profiles | User report or C2 table test | Add the make or model alias with a test row. Keep the exact rule. |
| The JSON of a document without a reference changes | `DocumentWithoutReferenceSerializesAsBefore` fails | Stop. Old projects must open. |
| An Undo path applies a batch without the bind step | `UndoRestoresImportProfileAndRedoRestoresDcpProfile` fails | Move the bind step to the owner that applies the batch. |
| The look of a profile differs from the vendor application | User review in C7 | Record the difference. P-5 is the known cause for contrast. Open an issue for each other cause. |
| A DCP file has a tag layout outside section 1.4 | Rejected count above zero for a user folder | Read the log line. Extend the reader with a test, or keep the explicit error for P-7 tags. |

Stop implementation and update this plan when a required owner API does not exist, when the
source audit no longer matches the branch, when a phase can pass 2000 lines, or when an operation
needs a substitute path that the user did not approve.

---

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

---

## 14. Terms and parameters

| Term or parameter | Meaning |
| --- | --- |
| DCP file | DNG Camera Profile file: a TIFF container with magic `0x4352` and one IFD of DNG profile tags |
| DCP library folder | The folder that the user sets in Settings. Setting key `cameraProfiles/dcpLibraryRoot`. |
| Profile identity | `(unique_camera_model, profile_name)` after normalization |
| `unique_camera_model` | Value of tag 50708, `UniqueCameraModel` |
| `profile_name` | Value of tag 50936, `ProfileName` |
| Fingerprint | FNV-1a 64-bit hash of the parsed profile content, written as 16 lowercase hexadecimal digits |
| Profile reference | Develop JSON key `camera_profile_file` with `camera_model`, `profile_name`, and `fingerprint` |
| Bound reference | A reference whose runtime pointer holds the loaded profile |
| Unbound reference | A reference read from JSON that no binding step has resolved |
| Import profile | The `camera_profile` values that import binds from the RAW color context |
| Effective profile | The matrices, CCT values, and tables that the render uses: the DCP values when a bound reference exists, and the import profile otherwise |
| Default entry | The combo row that clears the reference: Built-in for a RAW file, Embedded for a DNG file |
| Compact camera key | Result of `CompactCameraName`: lower-case letters and digits of a camera name |
| `kMaxDngTableFloats` | 4,194,304. The largest table that the current path accepts. |
| Loaded-profile cache size | 32 profiles |
| DCP file size limit | 64 MB |
| Scan time target | Below 2 seconds for 2445 files in a Release build on the development machine |
| CCT | Correlated color temperature of a calibration illuminant, in kelvin |
