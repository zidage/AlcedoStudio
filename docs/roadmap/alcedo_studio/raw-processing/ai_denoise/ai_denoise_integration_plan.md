# AI Denoise Integration Master Plan

- Date: 2026-11-21
- Status: DN0 complete (2026-11-21, lab PR zidage/alcedo-denoise-lab#2, issue #1032); DN1
  complete (2026-11-21, lab PR zidage/alcedo-denoise-lab#3, issue #1033; U3 closed, U2 with
  the user); DN2 to DN13 planned. 2026-11-22: D2 and D13 changed to ONNX Runtime DirectML in
  C++ on Windows; DN2, DN3, DN10, and DN11 rewritten for it. 2026-11-22: macOS measured (DN1
  addition, ONNX Runtime on macOS); D2 extended to Core ML on macOS; DN10 rewritten for it.
  2026-11-22: FP16 on the Neural Engine measured (DN1 addition); U5 closed: macOS runs FP16 on
  the Neural Engine (D2, DN10). 2026-11-22: FP16 on DirectML measured (DN1 addition); U6
  closed: Windows runs FP16 too. 2026-11-22: the user removed the stored result file: the model
  runs in the Develop render (D5 to D12, sections 2, 5, DN4 to DN9 rewritten; DN4 and DN9
  removed; U2 closed). 2026-11-22: U1 closed: `target_time` is the measured FP16 time (D4: no
  distillation); a device above `slow_device_limit` gets the slow-device error with the action
  that turns the noise reduction off (2.4, DN3, DN6 to DN8). 2026-11-22: DN2 complete (issue
  #1038, lab PR zidage/alcedo-denoise-lab#4); the user checked the FP16 sheets by eye and
  accepted FP16. 2026-11-22: DN3 complete (issue #1042): 0.45 s at 45.4 MP on the RTX 3080
  Laptop GPU (`target_time` 0.72 s); the Intel Iris Xe gets the slow-device error; a false
  slow-device error under video memory load is issue #1043 (a stop condition of DN6).
  2026-11-22: DN5 complete (issue #1045): the Develop parameter `ai_denoise`, the denoised input
  kind, and its Develop passes on CUDA and OpenCL (Metal code written, not built).
  2026-11-22: DN6 complete (issue #1048): the model runs in the loader of a full-size render;
  issue #1043 resolved by the tile size from the video memory of all processes and the
  slow-device check from the fastest measured tile; 0.56 to 0.72 s for a ready run at 42.2 MP.
  2026-11-22: DN7 complete (issue #1050): the editor switch with the locked Method track, the
  labels `Auto` and `Neural`, and the viewport card of each model error with
  `Turn Off AI Denoise`; the user removed the model version notice (D10). 2026-11-23: DN14
  complete (issue #1059): the steps of a render in a pill and a segmented bar, and the
  placeholder image before the first frame. 2026-11-23: DN8 complete (issue #1061): several photos
  get AI Denoise through Copy and Paste of the RAW Decode item (no library action, user); an
  export item with an AI Denoise error stays in the queue with `Turn Off AI Denoise`.
  2026-11-23: DN10 complete (issue #1069): the FP16 graph on the Neural Engine of macOS (Core ML,
  one session for each tile shape, a model cache for each model version), 45.4 MP in 0.55 s on
  the M4; the DN5 Metal code built and run. 2026-11-23: DN12 complete (issue #1072): the AI
  model feed lists the AI Denoise package next to the AI Mask set; the render reads the graph
  from the installed package. 2026-11-23: DN13 complete (issue #1074): `alcedo-cli` has
  `denoise.status` and `denoise.set`, a failed run reaches an agent as
  `data.ai_denoise_error`, `docs/ai_denoise.md` is the user page, and the alcedo skill uses AI
  Denoise on its own only for a high ISO photo with clearly visible noise (user)
- Source revision of the audit: `main` at `61d59bc98`
- Model source: [alcedo-denoise-lab](https://github.com/zidage/alcedo-denoise-lab), `docs/plan.md`
  steps 7 and 8
- Related plans:
  - [CUDA CNN Forward Framework + DemosaicNet Plan](../demosaicnet/cuda_nn_forward_demosaicnet_plan.md)
    and [its performance follow-up](../demosaicnet/cuda_demosaicnet_performance_next.md): the
    DemosaicNet time is the reference of DN1 and DN3. Since 2026-11-22 (D2) this plan does not
    extend their CUDA layer library.
  - [External AI Model Download Plan](../../ai/external_ai_model_download_plan.md): phase DN12
    extends its feed to more than one package.
  - [GPU DAG Pipeline Rebuild Phase Plan](../../edit/gpu_dag_pipeline_rebuild_phase_plan.md): phase
    DN5 adds one input kind to its Develop passes.

Before the implementation of a phase, read `AGENTS.md` and the applicable skills again. The
repository rules can change after the date of this plan.

## 1. Decision record

### 1.1 Goal

Alcedo Studio gets AI noise reduction in the Develop stage. The user turns it on for a photo.
The Develop render runs a neural network on the RAW mosaic in place of its own linearization and
demosaic, as it runs the DemosaicNet student for the Neural demosaic. The result stays in the
memory cache of the render; it is not stored on disk (2026-11-22: in FP16 the model takes about
the time of the DemosaicNet student, DN1 additions).

### 1.2 Approved decisions

| Id | Decision | Date |
| --- | --- | --- |
| D1 | The model is the full UtNet2 Bayer model of the lab, `models/dark_area_loss/checkpoint_10000.pt`. It has no strength control. | 2026-11-21 |
| D2 | The model runs in FP16 in ONNX Runtime, in the process of Alcedo Studio through the C++ API of ONNX Runtime: on Windows with the DirectML execution provider (U6), on macOS on the Neural Engine with the Core ML execution provider (U5). Both use one FP16 graph of the rescaled layers that the lab makes from the safetensors weights. The Rust sidecar (`alcedo_mind`) has no part in the noise reduction. TensorRT and cuDNN are not used. Changed on 2026-11-22 from own CUDA forward code (after the DirectML measurement of DN1), then to FP16 on both platforms (U5, U6). | 2026-11-22 |
| D3 | The work starts with the weights export and the measurements. The publication of the model package on R2 is the last product phase. | 2026-11-21 |
| D4 | No distillation to a smaller model. `target_time` is the measured FP16 time of the full model (U1), so the full model meets it. Changed on 2026-11-22 (was: distillation if the DN3 time is above `target_time`). | 2026-11-22 |
| D5 | The model runs in the render of the editor and of the export, when the Develop sensor stage is computed (a new photo, a change of a sensor Develop parameter, a cache miss). The editor shows its normal render state; there is no blocking progress display and no batch run. A paste only sets the parameter. Replaced on 2026-11-22 (was: a blocking run with a progress display, Cancel, and a batch run). | 2026-11-22 |
| D6 | The application stores no denoised image on disk: no stored result file and no separate DNG file. The RAW file stays the photo. The denoised image is an entry of the prepared source cache in memory. Replaced on 2026-11-22 by the user (was: a stored result file of the RAW file). | 2026-11-22 |
| D7 | Removed on 2026-11-22 with the stored result file (was: the folder of the stored results in the settings). | 2026-11-22 |
| D8 | Removed on 2026-11-22 with the stored result file (was: the deletion of the stored result file with the RAW file). | 2026-11-22 |
| D9 | A project package (`.alcd`) holds the Develop parameter of the noise reduction and no denoised image. A project opened on another computer runs the model there. Changed on 2026-11-22. | 2026-11-22 |
| D10 | The Develop parameter records the model version (the SHA-256 of the FP16 graph) when the user turns the noise reduction on. The render uses the installed model, and its content key holds the installed version. The editor shows no notice when the installed version differs from the recorded version (the user removed the notice on 2026-11-22, DN7). Replaced on 2026-11-22 (was: a stored result keeps the result of its model version). | 2026-11-22 |
| D11 | The Develop node keeps its structure: one root node, no input port. The noise reduction is a parameter of the Develop node. The loader runs the model on the mosaic and gives the result to the Develop passes as a second prepared input of the same RAW file. Changed on 2026-11-22 (the loader runs the model in place of a read of the stored result). | 2026-11-22 |
| D12 | A thumbnail and a semantic input image render from the reduced mosaic without the model (the Legacy demosaic, as now). The editor and the export run the model at the full size. When the model cannot run (not installed, no supported device, a run error), the editor and the export report the error and do not render the photo without the noise reduction. Changed on 2026-11-22. | 2026-11-22 |
| D13 | Platform order: Windows (DirectML, for each render backend and each GPU with Direct3D 12), then macOS (Core ML on the Neural Engine of Apple silicon), then Linux. On a platform without its forward phase, the application does not show the noise reduction control. Changed on 2026-11-22 from the backend order CUDA, Metal, OpenCL. | 2026-11-22 |
| D14 | The first version supports Bayer sensors only. For an X-Trans or a monochrome photo, the application reports that the photo is not supported. A paste for several photos sets the parameter of each photo; the render of a photo that is not Bayer reports it (changed on 2026-11-23, DN8: no library action). | 2026-11-23 |

### 1.3 Decisions that wait for a measurement

| Id | Decision | Measurement that it needs | Owner |
| --- | --- | --- | --- |
| U1 | `target_time`: the largest time for one 45 megapixel photo. Closed 2026-11-22: the user set it to the measured FP16 times of the DN1 additions, 0.72 s on the RTX 3080 Laptop GPU (DirectML) and 0.60 s on the MacBook Air M4 (Neural Engine). The exit criteria of DN3 and DN10 compare the time of a full photo in Alcedo Studio with them; a time more than 15% above stops the phase until the record states the cause. A slower device gets the slow-device error (`slow_device_limit`, 2.4); the Intel Iris Xe (5.16 s) is such a device | DN1 additions, DirectML and FP16 | User |
| U2 | The pixel format and the compression of the stored result file. Closed 2026-11-22: the stored result file is removed (D6) | DN1, stored format table | User |
| U3 | The tile size and the tile overlap. Closed 2026-11-21: overlap 96 packed px, tile size the largest of 512 to 1536 that fits (DN1 completion record) | DN1, border difference table | Implementer, recorded in DN3 |
| U4 | The highlight handling of the model output. The user decided on 2026-11-22 that no clip mask is made: high-noise photos have a low dynamic range, and many scenes that need the highlight recovery are not at a high ISO | DN1, clipped area table | Implementer, recorded in DN5 |
| U5 | macOS: FP16 on the Neural Engine (0.60 s at 45.4 MP, 75.7 to 80.6 dB against FP32 at +2 EV) in place of FP32 on the GPU (3.55 s). A change to D2 (FP32) and to the lab constraint "FP32 in Alcedo Studio". If yes: the rescale calibration on many photos, a check of the output for values that are not finite (the run then reports the error), and a check by eye of the FP16 output. Closed 2026-11-22: the user agreed; D2 and DN10 changed | DN1 addition, FP16 on the Neural Engine | User |
| U6 | Windows: FP16 on DirectML (the macOS FP16 graph, 0.72 s against 0.94 s at 45.4 MP now; 0.044 s against 0.090 s for the GPU part of a tile; 70.0 to 72.8 dB against FP32 at +2 EV) in place of FP32. If yes: one FP16 graph for both platforms in the model package, the output check of DN10 also on Windows, and DN3 keeps the tiles in DirectML memory to get the GPU gain. Closed 2026-11-22: the user agreed; D2, DN2, DN3 changed | DN1 addition, FP16 on DirectML | User |

### 1.4 Options that must not return

- A separate DNG file for the denoised image.
- A background denoise job that runs while the user edits.
- A stored result file beside the RAW file.
- A render without noise reduction when the noise reduction is on and the model cannot run, in
  the editor or in the export. Only the user turns the noise reduction off, with the
  `Turn Off AI Denoise` control of the error (2.4).
- A run that continues on a device above `slow_device_limit`, or a CPU run in its place.
- A different model, a lower resolution, FP32, or a different backend after a failure of the
  model run. The render reports the error.
- The model in the Rust sidecar (`alcedo_mind`).

## 2. Product design specification

### 2.1 Editor

- The RAW Decode panel gets one switch, `AI Denoise`, above the demosaic method. The method
  labels are `Auto`, `Legacy`, and `Neural` (DN7).
- The switch is visible when the platform has a forward phase (D13) and the photo is a Bayer
  RAW file. On Windows, the switch does not depend on the render backend (CUDA or OpenCL): the
  model runs on DirectML. For an X-Trans or a monochrome photo, the switch is disabled and its
  tooltip states that the sensor type is not supported.
- When the switch is on, the demosaic method control shows one locked segment `AI Denoise`,
  because the model also does the demosaic. When the switch goes off, the control shows the
  stored method again (DN7, the mock approved on 2026-11-22).
- When the user turns the switch on and the model is not installed, the editor opens the AI Models
  page of the settings, as the AI Mask tool does. The switch stays off.
- When the user turns the switch on, the edit history gets one `SetParameter` change with the
  installed model version (D10), and the next frame renders with the model. The viewport shows
  its normal render state during the run of the model.
- When the user opens a photo whose noise reduction is on and the model is not installed, the
  editor shows the render error with a control that opens the AI Models page (D12).
- Each render error of the model (2.4) has the control `Turn Off AI Denoise`. It writes one
  `SetParameter` change that turns the switch off and renders the photo again without the
  model. Undo turns the switch on again; the next render runs the model again.
- The editor shows no notice when the installed model version differs from the version in the
  parameter (user, 2026-11-22).

### 2.2 Library, paste, and export

- Several photos get the noise reduction through Copy Adjustments and Paste Adjustments: a
  paste of the RAW Decode item with the noise reduction on sets the parameter as each other
  paste; it runs no model. The library has no separate action (user, 2026-11-23, DN8). A pasted
  photo that is not Bayer reports `SensorNotSupported` at its render (D14).
- The export renders each photo with the model when its noise reduction is on. A photo whose
  model run fails gets the error of that photo in the export queue; the other photos continue.
- The queue item of that photo has the control `Turn Off AI Denoise`. It writes the change
  that turns the switch off into the active Version of the photo and puts the item back to
  `Queued` for the next export.

### 2.3 Model session

- One process-wide owner holds the ONNX Runtime session of UtNet2 (DN6). The editor render and
  the export use it one run at a time.
- The owner creates the session at the first run and keeps it while the editor has a photo open
  or an export runs. It releases the session when the editor closes and when the editor GPU
  release holder releases the editor resources (export, semantic generation).
- The first run of a session includes its creation (Windows: 0.07 to 0.53 s; macOS: 0.28 to
  0.46 s for each tile shape, DN1 additions). On Windows, DirectML also compiles at the first
  tile of each new tile width of a session: 0.22 to 0.35 s on the RTX 3080 Laptop GPU, 9.4 s for
  the first tile of a session on the Intel Iris Xe (DN3 completion record).
- The owner keeps the slow-device result of the device until the process ends. A later run on
  that device gives the slow-device error at once and runs no tile.

### 2.4 Failure behavior

| Failure | Result |
| --- | --- |
| Model not installed | Switch: the AI Models page opens, the switch stays off. Render of a photo with the parameter on: the render error with a control to the AI Models page |
| Sensor type not supported | The switch is disabled; the render of a pasted photo reports it with `Turn Off AI Denoise` |
| RAW file missing or its mount offline | The existing "missing" behavior of the editor and the export |
| GPU or Neural Engine error, or not enough memory for the smallest tile | The render error with the error text of ONNX Runtime |
| No Direct3D 12 GPU, or ONNX Runtime or DirectML.dll fails to load | The switch is hidden; a render of a photo with the parameter on reports the ONNX Runtime error. The application does not run the model on the CPU |
| A value that is not finite in an output tile (FP16 overflow) | The render error that names the tile; the lab calibrates again (section 8) |
| The device is too slow: the projected time of the photo is above `slow_device_limit` (for example the Intel Iris Xe, 0.114 s for each megapixel) | The run stops after the tile that shows it; the render error states that the device is too slow for AI Denoise. The owner keeps the result (2.3) |

Each render error of the model has the control `Turn Off AI Denoise` (2.1, 2.2). A model that
is not installed also has the control that opens the AI Models page.

The slow-device check: the tile loop measures the time of the tile runs. The creation of a
session and the first run of each tile shape on a session (it compiles the graph; DN3: also on
DirectML) are not measured. From the third measured tile on, the loop projects the time of the
whole photo from the fastest time for each output pixel of the measured tiles (DN6, issue #1043:
another process that shares the GPU slows single tiles of a fast device). When the projected
time is above `slow_device_limit` multiplied by the megapixels of the photo, the run stops. The
check also runs after the last tile (DN3), with the tiles that were measured. A run with no
measured tile (a photo of one tile on a new session) has no check; the next run has it.

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility in this plan |
| --- | --- |
| alcedo-denoise-lab | Weights export, FP16 rescale calibration and ONNX graph export, expected outputs, measurements, `docs/alcedo_integration.md` |
| `cmake`, `ui/alcedo_main/CMakeLists.txt` | The ONNX Runtime packages as a C++ dependency (DirectML on Windows, the osx-arm64 library on macOS), and their install rules |
| `decoders/processor/nn` | The ONNX Runtime sessions of UtNet2 (DirectML, Core ML), the overlap tile planner, the tile loop |
| `edit/input` | The model run in the loader, the process-wide session owner, the denoised entry of the prepared source cache |
| `edit/graph`, `edit/runtime` | The Develop parameter, the denoised input kind, the pass list, the content keys |
| `ui/alcedo_main` | The switch, the render error with the AI Models control, the export queue controls, the GPU release of the session |
| `opencl` | The forward code of Linux (DN11) |
| `app/ai_model_package_*`, `scripts/ai_models` | The feed with more than one package, the denoise model package |
| `ui/alcedo_main/automation` | The `denoise` commands of `alcedo-cli` |

### 3.2 Exclusions

- The linear UtNet2 model for X-Trans. The lab has no fine-tuned linear model.
- A strength control and a blend with the original image.
- A smaller distilled model (D4).
- The WebGPU and CPU render paths. They are deprecated.

## 4. Current source audit

Paths are relative to `alcedo_studio/src` unless they start with `docs/`, `scripts/`, or
`alcedo_studio/`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Develop passes | `CompileDevelopPasses`, `edit/runtime/graph_compiler.cpp:427` | CFA input: UploadRaw, Linearize, CfaClamp, Demosaic, HighlightRecover, InverseCamMulPack, Lens, GeometryResample, CameraToAp1. RGB input (`RawInputKind::DebayeredRgb`): UploadRgb, Lens, GeometryResample, CameraToAp1 | A pass list for the denoised input kind that keeps HighlightRecover |
| Linearization | `edit/runtime/cuda/cuda_develop_pass.cpp:102`, `decoders/processor/operators/gpu/cuda_white_balance.cu:114` | Black level, white level, and the relative as-shot white balance are applied before the demosaic. `InverseCamMulPack` removes the white balance after the highlight recovery | The model output enters after the demosaic in the same value domain |
| Highlight recovery | `decoders/processor/operators/gpu/cuda_highlight_reconstruct.cu` | Works on demosaiced RGB with white balance, normalized to the white level. The clip levels come from `cam_mul`. It reads no mosaic and no clip mask | None, if U4 shows that clipped areas of the model output stay at the clip level |
| RGB input without linearization | `RawInputLoader::FromDirectRgb`, `edit/input/raw_input_loader.cpp:628`; `cuda_sensor_demosaic.cpp:279` | `rgb_linearization` is empty, so the highlight recovery is off | The denoised input sets `rgb_linearization` from the RAW metadata |
| Decode resolution | `renderer/pipeline_scheduler.cpp:154`, `include/edit/runtime/develop_demosaic.hpp:26` | Editor and export decode at `DecodeRes::FULL`. Thumbnails and semantic input images reduce the mosaic on the host and force the Legacy demosaic | None for D12 |
| Develop node | `edit/graph/develop_node_model.cpp:591-598`, `include/edit/graph/develop_node_model.hpp:171` | Root node, no input port. `DevelopPayload::demosaic_method` holds `default`, `legacy`, or `neural_engine` | One new field; no port |
| Root id | `ComputeRootId`, `edit/history/pipeline_document_checkpoint.cpp:105` | The id is a hash of the document JSON. A new JSON key in every document changes the id of old roots | Write the new key only when the noise reduction is on |
| Content keys | `BuildFrameResultContentKeys`, `edit/runtime/result_content_key.cpp:548` | `develop.sensor_linear` is keyed on the prepared source, the linearization, and the sensor Develop parameters | Add the noise reduction parameter and the installed model version |
| Prepared source cache | `include/edit/input/prepared_source_cache.hpp` | Key: FNV hash of the encoded bytes, byte count, downsample passes, `kRawInputPreparationVersion` | Add the denoised input as its own entry kind (key: the source key, the model version, the input preparation version). A 45.4 MP entry holds 545 MB in FP32 |
| GPU release | `include/ui/alcedo_main/album_backend/editor_gpu_resource_release.hpp`, `PipelineExecutor::ReleaseBinding` | Export and semantic generation release the GPU resources of the editor before they run | The release also releases the model session (2.3); the export creates it again for its own renders |
| ONNX Runtime | `rust/puerh_mind/Cargo.toml:25-33` (`ort` 2.0.0-rc.13, ONNX Runtime 1.28); `ui/alcedo_main/CMakeLists.txt:1237` | Only the Rust sidecar uses ONNX Runtime. The pyke binaries link ONNX Runtime statically into `alcedo_mind.exe` (`onnxruntime.lib`, no DLL, no headers). The sidecar build copies DirectML.dll 1.15.4 beside `alcedo_main`. The vcpkg port of the tree (`vcpkg/ports/onnxruntime`, 1.23.2) has no DirectML feature, and the manifest does not use it | A C++ dependency on the NuGet packages `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 (onnxruntime.dll and the C and C++ headers) and `Microsoft.AI.DirectML` 1.15.4 (DN2). The sidecar keeps its static ONNX Runtime; one DirectML.dll of one version is in the install folder. macOS (DN10): `libonnxruntime.dylib` of the osx-arm64 folder of `Microsoft.ML.OnnxRuntime` 1.24.4 (CPU, Core ML, and WebGPU execution providers), the same headers as on Windows. The macOS sidecar `alcedo_mind` also links ONNX Runtime 1.28 statically (no dylib in the bundle), in its own process |
| CUDA conv layers | `cuda/nn/conv2d.cu:671-753`, `cuda/nn/cutlass_conv2d.cu:31-105` | Fast kernels for convolutions without padding only. Max pooling, LeakyReLU, and PixelShuffle are absent | None on Windows (D2). Input for an own forward code of DN11 |
| Tile planner | `include/decoders/processor/neural_tile_jobs.hpp:190-331` | Planned for a network that shrinks its input: 1086 in, 1024 out, no overlap between outputs | A planner for an output of the input size with an overlap |
| Weights | `decoders/processor/nn/demosaicnet_cache.cpp:83-147`, `nn/safetensors.cpp:96` | The DemosaicNet files are part of the installer. The loader checks metadata strings | The denoise ONNX graph comes from the model folder of the AI Models page |
| Tests of model code | `alcedo_studio/tests/cuda/CMakeLists.txt:14`, target `MlOpsTest` | CUDA layer and module tests with stored expected outputs | A new test target for the ONNX Runtime session (Windows only) |
| Timing tools | `alcedo_studio/deprecated/legacy_pipeline/tests/perf/` | No CMake target builds them | A new timing tool target for UtNet2 |
| Model packages | `app/ai_model_package_service.cpp:40`, `scripts/ai_models/prepare_ai_model_packages.py:90` | One known package. The script makes a feed with one entry, so a second package replaces the first | A feed with all packages |
| Paste | `ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp:73` | A paste writes a Version for each target and starts no computation | None: the paste sets the parameter (D5) |
| Files beside RAW files | `decoders/metadata_decoder.cpp:41` | The application reads XMP files and writes no file into a photo folder | None (D6) |
| Noise reduction | none | The editor has no noise reduction operator and no Detail panel | New |

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Changes | Error surface |
| --- | --- | --- | --- | --- |
| `UtNet2DirectMlSession` (proposed, Windows, DN2) | One packed tile, the FP16 ONNX graph | One RGB tile (float32, times `output_scale`) | Its ONNX Runtime session and the DirectML memory arena of the session | Exception with the ONNX Runtime status text, or with the tile of a value that is not finite |
| `UtNet2CoreMlSession` (proposed, macOS, DN10) | One packed tile, the FP16 ONNX graph | One RGB tile (float32, times `output_scale`) | Its ONNX Runtime sessions (one for each tile shape) and the Core ML models that they compile for the Neural Engine | The same |
| `DenoiseModelSession` (proposed, `edit/input`, DN6) | A packed mosaic, the installed FP16 graph | The gain-matched RGB image of the mosaic | The platform session; one run at a time for the editor and the export | Exception that the render reports |
| `RawInputLoader` (current) | RAW bytes, the Develop parameter | `PreparedRawInput` of the denoised kind, through the prepared source cache | The denoised entry of the prepared source cache | Exception when the parameter is on and the model cannot run |
| Develop passes (current) | `PreparedRawInput` | `develop.sensor_linear` | GPU textures of the frame | Existing render error path |

The model runs only in the loader, for a render at the full size. A render with the noise
reduction on and a model that cannot run is an error (D12); no other code path starts a model
run.

### 5.2 Data flow

```text
Render with the noise reduction on (editor or export, DecodeRes::FULL):
  RawInputLoader::LoadEncoded
    -> PreparedSourceCache lookup (source key, model version, preparation version)
    -> on a miss:
       -> LibRaw unpack -> ClassifyRawInput -> reject a sensor that is not Bayer
       -> input preparation of the lab specification -> packed mosaic in host memory
       -> DenoiseModelSession::Run(packed mosaic)
          -> the platform session (created at the first run; DirectML on Windows, Core ML on
             the Neural Engine on macOS), the DN3 tile loop with the overlap planner
          -> for each tile: input tile in float16 -> Run -> output check -> crop into the output
             -> accumulate the sums of the gain match
          -> apply the gain match factor
       -> insert the denoised entry into the prepared source cache
    -> LibRaw metadata: linearization and cam_mul
    -> PreparedRawInput { kind = denoised RGB, rgb_linearization = set }
  CompileDevelopPasses
    -> UploadRgb (to the value domain of the highlight recovery) -> HighlightRecover
    -> InverseCamMulPack -> Lens -> GeometryResample -> CameraToAp1

Thumbnail or semantic input image (reduced mosaic): the current Legacy path, no model (D12).

Editor close, or EditorGpuReleaseHolder (export, semantic generation):
  -> DenoiseModelSession::Release -> the platform session ends and frees its memory

### 5.3 Model facts

The topology is from `raw_denoiser.py` of the authors (commit `e80fb7b1`), read on 2026-11-21.

- Input: 4 channels at half the mosaic size. Output: 3 channels at the mosaic size.
- 18 convolutions 3x3 with zero padding 1, each followed by LeakyReLU. Channels: 4, 32, 64, 128,
  256, 512 down, and back to 32.
- 4 max pooling layers 2x2. 4 transposed convolutions 2x2 with stride 2. 4 concatenations of the
  upsampled tensor and the encoder tensor of the same level.
- Output layer: convolution 1x1 from 32 to 12 channels, then PixelShuffle(2).
- The height and the width of the input are multiples of 16.
- Cost by a count of the layers: 184,832 multiply-accumulate operations for each packed pixel,
  46.2 GMAC for each megapixel of the mosaic. The decoder convolutions are 60% of it. Each
  resolution level of the encoder after the first has the same cost.
- The theoretical receptive field radius is about 100 packed pixels. With that overlap on each
  side, a tile of 512 packed pixels keeps 35% of its area and a tile of 1024 keeps 63%. The
  measured overlap of DN1 replaces this estimate: 96 packed px gives the one-pass output, so a
  tile of 1024 keeps 66% of its area and a tile of 1536 keeps 76% (DN1 completion record).

## 6. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| DN0 | Weights file, expected outputs, input and output specification | lab | none | 600 to 900 | complete (#1032) |
| DN1 | Measurements: tile overlap, PyTorch time, clipped areas, stored formats, ONNX Runtime on DirectML and macOS, FP16 | lab | DN0 | 700 to 1100 | complete (#1033) |
| DN2 | FP16 rescale calibration and FP16 ONNX graph in the lab release; ONNX Runtime DirectML as a C++ dependency; the DirectML session of UtNet2 with its tests | lab, `cmake`, `decoders/processor/nn` | DN0 | 900 to 1400 | complete (#1038) |
| DN3 | Overlap tile planner, tile loop with float16 tiles, slow-device check, full photo run on DirectML, time against `target_time` | `decoders/processor/nn` | DN1, DN2 | 800 to 1200 | complete (#1042) |
| DN4 | Removed on 2026-11-22 (was: the stored result reader and writer) | | | | removed |
| DN5 | Develop parameter, denoised input kind, pass list, content keys (upload on CUDA, Metal, OpenCL) | `edit` | U4 | 1000 to 1500 | complete (#1045) |
| DN6 | Model run in the loader: `DenoiseModelSession`, input preparation, gain match, the denoised entry of the prepared source cache, GPU release, errors | `edit/input`, `decoders/processor/nn` | DN3, DN5 | 800 to 1300 | complete (#1048) |
| DN7 | Editor switch, render error with the AI Models control | `ui/alcedo_main`, `app`, `edit/input` | DN6 | 900 to 1400 | complete (#1050) |
| DN8 | Batch through Paste, and export errors | `ui/alcedo_main`, `app` | DN7 | 300 to 600 | complete (#1061) |
| DN9 | Removed on 2026-11-22 (was: the settings section for the stored results) | | | | removed |
| DN10 | macOS forward code: the Core ML session of UtNet2 on the Neural Engine in ONNX Runtime (C++), the osx-arm64 library | `cmake`, `decoders/processor/nn` | DN3 | 600 to 1000 | complete (#1069) |
| DN11 | Linux forward code | `opencl` or ONNX Runtime WebGPU | DN3 | 1400 to 1900 | last (2026-11-22: the only Linux environment is WSL) |
| DN12 | Feed with more than one package, denoise model package, AI Models card | `app`, `scripts/ai_models` | DN7 | 900 to 1400 | complete (#1072) |
| DN13 | `alcedo-cli` denoise commands, user documentation | automation, `docs` | DN8 | 400 to 800 | complete (#1074) |
| DN14 | Render feedback in the editor: placeholder image before the first frame, the step of the render (Reading RAW, AI Denoise tile k of n, Developing group k of n) in a pill and a segmented bar | `ui/alcedo_main`, `app`, `edit/input`, `edit/runtime` | DN7 | 900 to 1400 | complete (#1059) |

Order of the remaining phases: DN14, DN8, DN10, DN12, DN13, and DN11 last (DN11 last and DN14
added by the user on 2026-11-22; DN14 before DN8 by the user on 2026-11-23).
DN10 is complete (2026-11-23): the DN2 graph runs in Alcedo Studio on the Neural Engine of the
M4 in 0.55 s at 45.4 MP.

DN0 to DN3, DN5 to DN8, DN10, DN12 to DN14 have detailed sections below. DN5 to DN13 depend on U4 (closed). Each gets its
detailed section in this file before its implementation starts, with the format of DN0 to DN3.
DN5 can start before DN3 completes: its tests build the denoised prepared input from the DN0
expected outputs, so the Develop integration can be checked before the tile loop exists.

Since DN12, the AI Models page installs the FP16 ONNX graph into
`<model folder>/alcedo-ai-denoise-v1/` (the package `alcedo-ai-denoise`). The model folder is
the QSettings value `semantic/modelDirectory`. The folder `ai-denoise-v1` that developers
filled by hand before DN12 is not read.

## 7. Detailed phases

### DN0: weights export and specification (lab repository)

**Objective and deliverables**

- `models/export/utnet2_bayer_dark_loss_10k.safetensors` with a record of its SHA-256.
- Expected outputs for the layer tests and the module test of DN2 and DN3.
- The first part of `docs/alcedo_integration.md`: tensor names, input preparation, output meaning.

**Inputs and prerequisites**

- `models/dark_area_loss/checkpoint_10000.pt` and its `run.json`.
- The authors' code at commit `e80fb7b1` (the commit of `run.json`).

**Files (all proposed, lab repository)**

- `scripts/export/export_weights.py`
- `scripts/export/make_expected_outputs.py`
- `scripts/export/README.md`
- `docs/alcedo_integration.md`

**Data rules**

- Tensor names are the PyTorch `state_dict` names (`convs1.0.weight`, `up1.weight`,
  `output_module.0.bias`, and so on). The layout is the PyTorch layout. Alcedo Studio changes the
  layout at load time.
- Metadata of the file: `architecture = utnet2_bayer_funit32`, `checkpoint_sha256`,
  `leaky_relu_slope` (the value that `get_activation_class_params` gives), `input_preparation_version`,
  `license`.
- The expected outputs come from PyTorch FP32 with TF32 off. The script also runs the model on the
  CPU and records the largest absolute and relative difference between the CPU and the GPU output.
  The tolerance of the Alcedo Studio tests is set from this difference and stated in the README.

**Implementation steps**

1. Write `export_weights.py`. It loads the checkpoint, checks that the tensor names and shapes
   agree with the topology of section 5.3, and writes the safetensors file and a JSON table of
   names, shapes, and the file SHA-256. It fails when a tensor is absent or has a different shape.
2. Write `make_expected_outputs.py`. Inputs: two tensors from a recorded random generator state
   (4 x 64 x 64 and 4 x 256 x 256, values 0 to 1), and one packed crop of 4 x 512 x 512 from a
   test split photo of RawNIND. Outputs: the model output of each input, and for the 64 x 64 input
   the output of each of the 23 layers. A manifest gives the shape and the SHA-256 of each file.
3. Write the specification part. State each item from the code, with the file and line of the
   authors' code that proves it:
   - black level and white level normalization, and the handling of values above the white level;
   - the crop that moves each CFA pattern to the RGGB phase, and how Alcedo Studio puts the removed
     border rows and columns back;
   - whether the white balance is applied to the mosaic (the audit of this plan did not confirm it);
   - the padding to a multiple of 16;
   - the meaning of the output (camera RGB, color matrix absent) and the definition of the gain
     match factor, with the pixels that it uses.
4. Record the result in `docs/findings/inference_cost.md`.

**Tests and evidence**

- `export_weights.py --verify` loads the safetensors file into a new model and compares its output
  with the checkpoint output on the three inputs. The difference is 0.
- The manifest and the tolerance record are committed. The weights file and the expected outputs
  are committed only if their total size is below 40 MB; above that, they go to a release asset
  with their SHA-256 in the README.

**Exit criteria**

- [x] The weights file loads in a new model with an output difference of 0.
- [x] The manifest lists each expected output with its shape and SHA-256.
- [x] The specification states each item of step 3 with its proof.

**Expected diff**: 600 to 900 lines.

##### Phase DN0 completion record (2026-11-21)

**Status:** complete. Issue #1032, lab pull request
[zidage/alcedo-denoise-lab#2](https://github.com/zidage/alcedo-denoise-lab/pull/2) (branch
`feature/weights-export`), release asset
[`export-utnet2-bayer-dark-loss-10k`](https://github.com/zidage/alcedo-denoise-lab/releases/tag/export-utnet2-bayer-dark-loss-10k).

**Deliverables:**

| Item | Value |
| --- | --- |
| Weights file | `utnet2_bayer_dark_loss_10k.safetensors`, 31,047,448 bytes, SHA-256 `0bfb9ddb07e6e6c20e6764dfc30336d9314d5760f875992f2144f13dd0df13a5`, 46 tensors, 7,760,748 parameters |
| Metadata | `architecture = utnet2_bayer_funit32`, `leaky_relu_slope = 0.2`, `input_preparation_version = 1`, `license = GPL-3.0 AND CC-BY-4.0`, `checkpoint_sha256`, `checkpoint_step`, `authors_commit`, `layout`, `license_note` |
| Expected outputs | `random_64.safetensors` (input, output, 23 layer outputs `layer.<prefix>`), `random_256.safetensors`, `rawnind_crop_512.safetensors` (input, output, `rgb_xyz_matrix`); NCHW float32 without the batch axis, before the gain match |
| Manifest | lab `results/export/expected_outputs_manifest.json`: SHA-256 and byte size of each file, shape and CPU against GPU difference of each tensor, `tolerance_abs` of each tensor |
| Tolerance of DN2, DN3, DN10, DN11 | `max abs(alcedo - expected) <= 1e-5 * max abs(expected)` for each tensor. 1e-5 is 4 times the largest CPU against GPU difference of PyTorch (2.4e-6 of the largest value, layer `tconvs1.0`), rounded up |
| Specification | lab `docs/alcedo_integration.md`, sections "Weights file" to "Expected outputs and tolerance" |

**Primary success call chain:**

```text
export_weights.py
  -> torch.load(checkpoint)["model"] -> check_tensors (names, shapes, float32 against LAYERS)
  -> write_safetensors (sorted metadata, forward order) -> tensors JSON table with SHA-256
make_expected_outputs.py
  -> safetensors.load_file -> authors' UtNet2.load_state_dict(strict=True)
  -> GPU run (TF32 off, deterministic cuDNN) and CPU run (4 threads), hooks on the 23 layers
  -> write_safetensors for each input -> manifest (differences, tolerance, gain match factors)
export_weights.py --verify
  -> safetensors.load_file -> new UtNet2(strict=True) -> torch.equal to the checkpoint tensors
  -> outputs of the export and the checkpoint on the 3 inputs: max abs difference 0
  -> stored expected outputs: max abs difference 0 -> verify JSON, exit status 0
```

**Primary failure call chain:**

```text
checkpoint with a missing tensor, a different shape, an extra tensor, or a float64 tensor
  -> check_tensors raises RuntimeError that names each tensor and the expected shape
  -> no file is written
verify finds a difference
  -> "passed": false in the verify JSON -> exit status 1
```

**What was proven (executed checks):**

| Criterion | Evidence | Result |
| --- | --- | --- |
| The weights load in a new model with an output difference of 0 | lab `results/export/utnet2_bayer_dark_loss_10k_verify.json` | PASS: 3 of 3 outputs at 0, all 46 tensors bit for bit, metadata equal |
| The expected outputs come from this weights file | the same file, `export_vs_stored_expected_max_abs` | PASS: 0 on 3 of 3 |
| Each topology difference stops the export | one-time check `build/tmp/export/check_topology_errors.py` (lab, not committed) | PASS: 4 of 4 cases named |
| The same bytes on each run | two runs of both scripts, SHA-256 of 4 files | PASS: equal |
| The release assets have the recorded SHA-256 | `gh release download` of the 4 assets, `sha256sum` | PASS: 4 of 4 |

Commands (lab repository, `build/tmp/export/venv`, PyTorch 2.11.0+cu128, RTX 3080 Laptop GPU):

```text
P="build/tmp/export/venv/Scripts/python -I"
A="--authors-repo build/tmp/reference_measurement/rawnind_jddc"
$P scripts/export/export_weights.py $A
$P scripts/export/make_expected_outputs.py $A
$P scripts/export/export_weights.py $A --verify
```

**Facts for the later phases** (lab `docs/alcedo_integration.md` gives the proof lines):

- Input: `(raw - black) / (maximum - black)` with LibRaw `color.maximum`, no clip, no white
  balance, packed RGGB. The authors use `black_level_per_channel[0]` for all four positions;
  Alcedo Studio uses the level of each position (at most 2 levels apart on the local RawNIND
  files, Canon only). On Sony ILCE-7C files `maximum` is 16,383 and `linear_max` 15,360, so
  saturation is at 0.9355 of the input domain. DN5 and U4 must use this value domain.
- CFA phase: the authors remove the first and the last row or column to reach RGGB. Alcedo
  Studio extends the mosaic by one row at the top (GBRG, BGGR) and one column at the left (GRBG,
  BGGR) by reflection without a repeat of the edge pixel, so every pixel of the area gets an
  output. DN3 and DN6 implement this.
- Alignment: the authors crop to a multiple of 16 packed pixels. Alcedo Studio fills each tile by
  reflection of the packed image (DN3 data rule).
- Output: camera RGB without white balance; the scale is not defined (about 1e5 times the input;
  factor 9.69e-6 on the crop). The gain match factor of DN6 is
  `sum_c w_c mean_in(c) / sum_c w_c mean_out(c)`, `w_c` the column sums of the camera to
  Rec.2020 matrix, over the result pixels. It differs from the authors' factor by 3.4e-4 of the
  factor on the crop.

**Changes against the plan text:**

- The weights file is at `models/export/dark_area_loss/` of the lab, not `models/export/`: the
  lab `models/README.md` puts each export in `models/export/<run>/`.
- The scripts write the safetensors files with an own writer (`write_safetensors`, sorted
  metadata keys). The `safetensors` package 0.8.0 writes the metadata in a random order, so its
  file had a different SHA-256 on each run and a recorded SHA-256 could not be made again. The
  `safetensors` package reads the files in `--verify`.
- The total size is 57.5 MB, above 40 MB, so the weights and the expected outputs are a release
  asset (the rule of "Tests and evidence").

**LOC note:** lab diff 2,074 lines, of which 929 are result JSON tables; 1,145 lines of scripts
and documents (largest file `make_expected_outputs.py`, 291 lines), above the estimate of 600 to
900 because of the topology table, the deterministic writer, and the specification.

**Remaining gaps:** none for DN0. The tile size, the overlap, the highlight handling, and the
stored result format are DN1. The time of the model is DN1 (PyTorch) and DN3 (Alcedo Studio).

### DN1: measurements (lab repository)

**Objective and deliverables**

Four tables in `docs/findings/`, which close U2, U3, and U4 and give the PyTorch reference time.

**Inputs and prerequisites**

DN0. The evaluation photos of the lab. One photo each of about 24, 36, and 45 megapixels.

**Resource rules**

Run one measurement job at a time. Run the model in tiles so that the GPU memory stays bounded.
Use 4 CPU threads. Record the peak working set and the peak GPU memory of each job.

**Implementation steps**

1. **Tile overlap.** For tile sizes 256, 512, 768, and 1024 packed pixels and overlaps from 16 to
   128 packed pixels in steps of 16, compare the tiled output with the output of one pass on a
   crop that fits in one pass. Record the largest and the RMS difference in the 16 pixels at each
   tile border, in linear values and after the +2 EV development. Record the same difference
   between the TF32 and the IEEE FP32 output as the reference level. Select the smallest overlap
   whose border difference is below that level. Record the peak GPU memory of one tile.
2. **PyTorch time.** FP32 with TF32 off, in tiles of the selected sizes, for the three photos.
   Record the time, the GPU temperature, and the GPU clock of each run.
3. **Clipped areas.** On 5 or more photos with clipped highlights (one or more with a clipped
   single channel), compare the gain-matched model output with the normalized mosaic in the
   clipped areas. Record the distribution of the output value relative to the white level for each
   channel. State whether the clip threshold of Alcedo Studio (0.987 of the white level after the
   white balance) still finds these areas.
4. **Stored formats.** For FP16 with lossless compression (byte planes and zstd) and for lossy
   JPEG XL at 4 or more distances, on linear and on log-encoded values: the size for each
   megapixel, the encode and decode time, and the developed PSNR at +2 EV and +5 EV on the whole
   image, on dark crops, and on detail crops. Make comparison sheets for a check by eye.
5. Write `scripts/export/write_stored_result.py` after U2 is decided. It writes a stored result
   file in the selected format, for the early check of DN4 and DN5.

**Exit criteria**

- [x] Each of the four tables is in the findings files with its command and its source file.
- [x] `tile_size` and `tile_overlap` have values in the parameter table of the lab plan.
- [x] The user has the stored format table and the sheets for U2.

**Expected diff**: 700 to 1100 lines.

##### Phase DN1 completion record (2026-11-21)

**Status:** complete for steps 1 to 4. Issue #1033, lab pull request
[zidage/alcedo-denoise-lab#3](https://github.com/zidage/alcedo-denoise-lab/pull/3) (branch
`feature/denoise-measurements`, based on the DN0 branch). Step 5 (`write_stored_result.py`) starts
when the user selects the format (U2), as the step states.

**Results** (PyTorch 2.11.0+cu128, cuDNN 9.19.0, IEEE FP32 with TF32 off, RTX 3080 Laptop GPU
8 GB with 3.1 GiB used by other programs, weights `utnet2_bayer_dark_loss_10k.safetensors`):

| Table | Result | Lab findings entry |
| --- | --- | --- |
| Tile overlap | 96 packed px. The tiled output is then the one-pass output (difference 0 for tiles of 512 to 1024, below 1e-6 developed for 256). At 80 the border difference is 3 to 30 times the TF32 level. The receptive field is 92 packed px; tile inputs at multiples of 16 keep the pooling grid | `inference_cost.md`, tile overlap |
| GPU memory of one tile | 163 MiB (256), 200 MiB (512), 451 MiB (768), 800 MiB (1024), 1,800 MiB (1536), 3,200 MiB (2048), PyTorch allocated memory above the weights and the packed image | `inference_cost.md`, tile overlap and PyTorch time |
| PyTorch time, 45.4 MP | 0.92 s with tiles of 1536 (1.96 GiB allocated, 2.85 GiB reserved), 1.37 s with 1024 (0.98 GiB), 1.39 s with 768, 2.17 s with 512, 20.7 s with 256, 1.67 s with 2048 | `inference_cost.md`, PyTorch time |
| PyTorch time, 24.3 / 32.5 / 36.2 / 42.4 MP | 0.58 to 0.97 s / 0.83 to 1.14 s / 1.03 to 1.13 s / 0.92 to 1.56 s for tiles of 768 to 2048 | the same |
| GPU state during the time runs | 71 to 89 C, median SM clock 1,350 to 1,785 MHz (largest 2,100); largest used GPU memory of the process (NVML, CUDA context included) 4.36 GiB | the same |
| Clipped areas | On 5 of 6 photos, the gain-matched output in clipped cells is 0.888 to 0.992 of `linear_max` (median); 0% to 72% of the pixels reach 0.987. The clip threshold of Alcedo Studio does not find most clipped areas in the stored result | `quality.md`, model output in clipped highlights |
| Stored formats | float16: at least 81.8 dB developed. JPEG XL lossless of float16 1.9 to 2.3 MB/MP (86 to 106 MB at 45 MP, encode 0.09 s/MP, decode 0.05 s/MP); float16 byte planes + zstd 3 3.3 to 3.6 MB/MP (0.02 s/MP each way); lossy JPEG XL 0.007 to 0.31 MB/MP at 26 to 54 dB, color blocks in dark areas at +5 EV for linear values from distance 0.5. Clip mask: at most 2,871 bytes | `quality.md`, stored result formats |

**Effect on the decisions that wait for a measurement:**

- U3 (implementer): `tile_overlap` = 96 packed px. `tile_size` = the largest of 512, 768, 1024,
  and 1536 whose workspace fits in the free GPU memory. Closed here; DN3 uses these values.
- U4 (implementer, DN5): the stored result alone does not let the highlight recovery find the
  clipped areas. DN5 gives the highlight recovery the clipped photosites of the mosaic (a clip
  mask in the stored result file, at most 3 KB, or the mosaic read by the loader). The detailed
  section of DN5 states which. Changed on 2026-11-22: the user decided that no clip mask is
  made (U4 row of section 1.3).
- U2 (user): open. The table and the sheets are with the user (lab plan, "Items that need the
  user").

**Primary success call chain:**

```text
measure_photo_time.py (and the three other scripts)
  -> measurement_common.prepare_packed_rggb (rawpy: visible area, black of each position,
     LibRaw maximum, reflection to RGGB, 4-channel pack)
  -> measurement_common.load_model (safetensors, architecture check, strict load)
  -> measurement_common.run_tiled
     -> plan_tiles (plan_axis: first tile keeps [0, T - O), next tiles T - 2 O, last at N - T)
     -> packed image to the GPU -> for each tile: tile_input -> UtNet2 forward (CUDA events)
        -> kept rectangle -> pinned host output
     -> PyTorch peak allocated and reserved memory, GpuSampler (NVML temperature, clock, power,
        used memory every 50 ms)
  -> gain_match_factor -> to_rec2020 -> developed -> results/export/*.json
```

**Primary failure call chain:**

```text
weights with another architecture, a missing tensor, or another shape
  -> load_model raises RuntimeError (metadata) or load_state_dict(strict=True) raises
  -> no measurement runs, no result file
raw file that is not a 2 x 2 Bayer pattern
  -> prepare_packed_rggb raises ValueError with the file name and the pattern
tile size not a multiple of 16 or not larger than 2 x overlap
  -> plan_axis raises ValueError; no tile runs
```

**What was proven (executed checks):**

| Criterion | Evidence | Result |
| --- | --- | --- |
| The planner covers each axis exactly one time with at least the overlap of context | one-time check `build/tmp/dn1/check_planner_and_clip_survey.py` (lab, not committed): 8 lengths x 4 tile sizes x 8 overlaps | PASS |
| The tiled output at the selected overlap equals one pass | `results/export/tile_overlap.json`, 3 crops of 1536 packed px | PASS: 0 for 512 to 1024, at most 7.8e-7 developed for 256 |
| The stored format encoders give back their input | one-time check `build/tmp/dn1/check_codecs.py` and `check_codecs_smooth.py`: lossless formats equal float16; lossy JPEG XL keeps the channel means | PASS |
| The four tables have a command and a source file | lab `scripts/export/README.md`, "Measurements (phase DN1)", and the four findings entries | PASS |

Commands (lab repository, `build/tmp/export/venv`):

```text
P="build/tmp/export/venv/Scripts/python -I -u"
A="--authors-repo build/tmp/reference_measurement/rawnind_jddc"
$P scripts/export/measure_tile_overlap.py $A
$P scripts/export/measure_photo_time.py $A --overlap-by-tile 256:96 512:96 768:96 1024:96 1536:96 2048:96
$P scripts/export/measure_clipped_areas.py $A --tile 1536 --overlap 96
$P scripts/export/measure_stored_formats.py $A
```

**Changes against the plan text:**

- No local photo has 36 or 45 megapixels. The 36.2 MP size is a crop of the 42.4 MP Sony
  ILCE-7RM3 photo and the 45.4 MP size (Nikon Z 8, 8256 x 5504) is that photo extended by
  reflection. The time of an FP32 convolution does not depend on the pixel values. The 24.3,
  32.5, and 42.4 MP photos are real.
- The time table also has tiles of 1536 and 2048 packed px. With an overlap of 96, a tile of 1024
  keeps 66% of its area and a tile of 1536 keeps 76%, and 1536 is the fastest on every photo. So
  `tile_size` extends to 1536 (section 9). 2048 reserves 4.85 GiB and is slower on this 8 GB GPU.
- The time depends on the tile count, not only on the photo size, because the last tile of an
  axis has the full tile size. DN3 records this in the time of its planner.
- Step 3 used 6 photos; one of them (Canon EOS R3) has a LibRaw `linear_max` below its largest
  raw values, so its "clipped" cells are not all saturated. The other 5 give the result.
- The overlap selection uses the developed values (+2 EV) for the largest and the RMS difference;
  the linear values are recorded beside them.
- The stored format table also has JPEG XL lossless and float16 byte planes with a difference of
  the left neighbour. The log encoding is `log2(v + 2^-16)` over [0, 2] as 16-bit codes with the
  sRGB transfer tag.

**LOC note:** lab diff 6 changed files (+298 lines) and 5 new scripts (1,344 lines; largest
`measurement_common.py`, 446 lines), plus 4 result JSON files (310 KB). Above the estimate of
700 to 1100 lines because of the shared module and the extra format rows.

**Remaining gaps:** step 5 (`write_stored_result.py`) after U2; dropped on 2026-11-22 with the
stored result file (D6). Not measured: the forward code of Alcedo Studio (DN3), the highlight
recovery on the model output by eye (DN5).

##### Phase DN1 addition (2026-11-22): last tile, cuDNN algorithms, TF32

The user asked how much faster the model can run, with 2 times the DemosaicNet time as the
level at which a stored result may not be necessary. Lab commit `1ffaf8a` on the same pull
request; lab entry `inference_cost.md`, 2026-11-22, last tile and cuDNN algorithms.

| Change | 45.4 MP, tiles of 1536 | 36.2 MP, best tile |
| --- | ---: | ---: |
| DN1 record: deterministic cuDNN algorithms, last tile of the full size | 0.92 s | 1.03 s |
| Fastest cuDNN algorithms (`cudnn.benchmark`), output within 1.1e-6 | 0.73 s | 0.76 s |
| Fastest algorithms and the last tile shrunk to its need | **0.69 s** | **0.64 s** |
| TF32 instead of FP32 (full last tile) | 0.72 s | 0.68 s |

- **TF32 is not a useful change.** On this GeForce GPU it saves 0 to 0.17 s against the fastest
  FP32 algorithms and changes the output by 5.5e-4. D2 (FP32) stays.
- **The tile planner must start each tile input at a multiple of 16.** The DN1 planner put the
  last tile of an axis at `[N - T, N)`. When `N` is not a multiple of 16 (the 24.3, 32.5, and
  42.4 MP photos), that tile moves the grid of the max pooling layers, and its output differs
  from one pass by up to 6% of the largest value. The lab planner now shrinks the last tile: its
  input starts `O` px before its kept range and is filled by reflection to a multiple of 16. The
  output is then the one-pass output (0 on a crop of 1500 x 1400 packed px), and the 36.2 MP
  photo computes 10.8 M packed px instead of 14.2 M. DN3 implements this planner (data rules and
  test below). The clipped-area table is made again with it (changes in the third decimal); the
  stored format table compares each format with one result, so the planner does not change it.
- **Against DemosaicNet:** the DemosaicNet student of Alcedo Studio takes 376 ms (p50 from 82 to
  84 C) and 294 ms (cool) on the 36.3 MP D800e Bayer photo
  (`cuda_demosaicnet_performance_next.md`). The model takes 0.64 s on 36.2 MP in PyTorch, 1.7
  times the 376 ms and 2.2 times the 294 ms. The DemosaicNet time is its complete stage; the
  model time does not include the input preparation and the gain match. DN3 measures the same
  span in Alcedo Studio. The own kernels of DN2 must reach the cuDNN level (8.6 TFLOP/s in this
  run) for this ratio; the current FP32 kernels of Alcedo Studio reach 1.6 to 2.0 TFLOP/s.
- The cuDNN search of the algorithms takes 12 to 37 s on the first call of a new tile shape. The
  forward code of DN2 selects its kernels at build time and has no search. (Own kernels are
  replaced by ONNX Runtime DirectML since 2026-11-22, D2; DirectML has no such search: the first
  run of a session takes 0.16 to 0.53 s.)

##### Phase DN1 addition (2026-11-22): ONNX Runtime with DirectML

The user asked for the time of the model in ONNX Runtime with the DirectML execution provider.
Lab commit `51c4007` on the same pull request; lab entry `inference_cost.md`, 2026-11-22, ONNX
Runtime DirectML. ONNX Runtime 1.24.4 (the last `onnxruntime-directml` package on PyPI) with
DirectML 1.15.4, the DirectML.dll that Alcedo Studio ships beside ONNX Runtime 1.28 of the AI
Mask sidecar. The ONNX graph is made from the safetensors weights of DN0 (opset 17, 50 nodes,
height and width free, each node on DirectML).

| Measurement | Result |
| --- | --- |
| Precision against the DN0 expected outputs | 9.6e-7 to 1.7e-6 of the largest value on the RTX 3080 Laptop GPU and on the Intel Iris Xe; the DN0 tolerance is 1e-5 |
| Time, one session with free height and width, tiles of 1536, overlap 96, shrunk last tile | 0.49 s (24.3 MP), 0.69 s (32.5 MP), 0.71 s (36.2 MP), 0.97 s (42.4 MP), 0.82 s (45.4 MP) |
| PyTorch, fastest FP32 cuDNN algorithms, the same session | 0.41, 0.51, 0.56, 0.68, 0.69 s: DirectML is 1.2 to 1.3 times |
| Time with tiles of 1024 | 1.06 s at 45.4 MP |
| Used GPU memory of the process | 3.3 GiB (tiles of 1536), 1.0 GiB (tiles of 1024); PyTorch 4.8 GiB |
| Host copies | 35 ms of the 114 ms of one tile of 1536 (input and output through host memory); 0.2 s of the 0.82 s at 45.4 MP |
| One session for each tile shape | The same output; 9% to 17% faster when it fits, but each session keeps its own memory arena: 4 sessions of tiles of 1536 use 5.7 GiB and spill to shared memory (6.6 s at 45.4 MP) |
| `ep.dml.disable_memory_arena` | 1.3 to 2.5 times longer with one free session |
| One shape for all tiles (last tile filled by reflection to the full tile) | Not the one-pass output: up to 7.7e-2 of the largest value at the bottom and right border |
| Session creation and first run | 0.07 to 0.13 s and 0.16 to 0.53 s |
| Intel Iris Xe, tiles of 1024 | 3.5 s (24.3 MP), 6.9 s (45.4 MP) |

- **Against DemosaicNet:** 0.71 s on 36.2 MP is 1.9 times the 376 ms of the DemosaicNet student.
  Own CUDA kernels reach this time only at the cuDNN level (section DN1 addition of the last
  tile); the current FP32 kernels of Alcedo Studio reach 1.6 to 2.0 TFLOP/s.
- **Effect on the decisions:** the user changed D2 and D13 on 2026-11-22 after this record: on
  Windows the model runs in ONNX Runtime DirectML in the process of Alcedo Studio (C++), not in
  the Rust sidecar. DN2 and DN3 are rewritten for it. The session strategy of DN3 is one session
  with free height and width, memory arena on, and the shrunk last tile.
- GPU state: DirectML at 65 to 79 C, PyTorch at 72 to 88 C; median SM clock 1,485 to 1,800 MHz
  in both. Not measured: ONNX Runtime 1.28 of Alcedo Studio, AMD GPUs, the Core ML execution
  provider (measured in the next addition), copies that overlap the run.

##### Phase DN1 addition (2026-11-22): ONNX Runtime on macOS

The user asked for the macOS measurement with the model in ONNX Runtime in the C++ code of
Alcedo Studio (not the sidecar), on the compute backends that macOS has. Lab commit `7717219`
on the same pull request; lab entry `inference_cost.md`, 2026-11-22, ONNX Runtime on macOS. A C++
tool (lab `scripts/export/macos/ort_macos_measure.mm`) runs the ONNX graph of the DirectML
addition through the C++ API of ONNX Runtime, with the DN1 planner (overlap 96, last tile
shrunk) and the DN0 expected outputs. Machine: MacBook Air (Mac16,12), Apple M4 with 8 GPU
cores, 16 GB unified memory, macOS 27.0.1, no fan; the thermal state stayed nominal. ONNX
Runtime 1.24.4 and 1.32.0 from the osx-arm64 folder of the NuGet package
`Microsoft.ML.OnnxRuntime` (CPU, Core ML, and WebGPU execution providers in one
`libonnxruntime.dylib`), and the WebGPU plugin provider 0.6.0. Memory: the peak physical
footprint of the process, which counts the GPU memory on this Mac and about 0.75 GiB of host
arrays at 45.4 MP.

| Backend in ONNX Runtime (1.24.4) | Placement and precision | 45.4 MP, tiles of 1536 |
| --- | --- | ---: |
| Core ML, MLProgram, CPUAndGPU, one session with free height and width, graph without DepthToSpace (PixelShuffle on the host) | 49 of 49 nodes on Core ML, each operation on the GPU; 5.6e-7 to 8.2e-7 | **3.55 s**, 5.5 GiB |
| Core ML, MLProgram, CPUAndGPU, one session for each tile shape | 50 of 50 nodes, GPU; the same output | 3.15 to 5.04 s in one process, 3.25 to 3.32 s in two others; 9.9 to 10.1 GiB |
| Core ML, MLProgram, one session with free height and width, full graph | 49 of 50 nodes: Core ML does not take DepthToSpace with free dimensions; with that node on the CPU provider 3.72 s, 4.7 GiB | |
| Core ML, MLProgram, ALL | The compute plan selects the GPU for each operation, as CPUAndGPU | |
| Core ML, MLProgram, CPUAndNeuralEngine or CPUOnly | Each operation on the CPU; the Neural Engine takes no FP32 operation; 9.6e-7 to 1.5e-6 | 5.19 s at 24.3 MP (CPUOnly, tiles of 1024) |
| Core ML, NeuralNetwork format | 45 of 50 nodes: no ConvTranspose, no DepthToSpace | |
| WebGPU (package or plugin), free or per shape | All nodes; 9.6e-7 to 1.5e-6 | 9.06 to 9.64 s, 4.7 to 6.5 GiB |
| CPU provider, 4 threads | 1.0e-6 to 1.5e-6 | 12.7 s at 24.3 MP (tiles of 1024) |

Core ML on the GPU with one free-shape session and the graph without DepthToSpace, median
seconds (peak footprint at 45.4 MP):

| Tile | 24.3 MP | 32.5 MP | 36.2 MP | 42.4 MP | 45.4 MP |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1024 | 2.16 | 3.00 | 3.25 | 3.80 | 4.40 (3.2 GiB) |
| 1536 | 1.83 | 2.50 | 2.82 | 3.31 | 3.55 (5.5 GiB) |
| 2048 | 1.60 | 2.46 | 2.78 | 3.23 | 3.49 (7.7 GiB) |

- **Selected for DN10** (replaced on 2026-11-22 by the next addition, FP16 on the Neural
  Engine): Core ML, `ModelFormat` MLProgram, `MLComputeUnits` CPUAndGPU,
  `AllowLowPrecisionAccumulationOnGPU` 0 (1 gives the same output), one session with free height
  and width, the graph without the last DepthToSpace node (output `output_packed` (1, 12, h, w)),
  the PixelShuffle in the crop copy of the tile loop, tiles of 1536 (1024 when the memory does
  not permit 1536). This is the session strategy of Windows. One session for each shape is 7% to
  12% faster, but it uses 10 GiB at tiles of 1536 and its times vary by up to 1.6 times.
- **ONNX Runtime 1.24.4 on both platforms:** 1.32.0 has the same times (3.51 s, 3.32 s, 3.76 s
  for the three Core ML rows) and the same precision on the GPU. One version gives one set of
  headers for Windows and macOS.
- **Windows keeps the full graph:** the graph without DepthToSpace on DirectML (PixelShuffle in
  NumPy) takes 1.26 s against 0.85 s at 45.4 MP; the output is the same. The model package holds
  both graph files.
- **Against DemosaicNet on the same M4:** the Metal DemosaicNet student takes 377 ms on the 24 MP
  Bayer photo of the Lumix S5M2 (`metal_nn_forward_demosaicnet_plan.md`). 1.83 s on 24.3 MP is 4.9
  times (Windows: 1.9 times). At this FP32 time a stored result was necessary on macOS; the
  FP16 addition below brings the time to the DemosaicNet time, and the user removed the stored
  result on 2026-11-22 (D6).
- **Session start:** Core ML session creation 0.09 to 0.50 s, first run of a tile of 1536 0.49 to
  0.79 s. WebGPU compiles its shaders in the first run (1.35 to 1.73 s).
- **Memory after the release:** in one process, the footprint after the release of the `free`
  session grows from photo to photo (0.97 to 4.9 GiB, of which 0.75 GiB are host arrays). The lab
  did not find the owner of this memory. DN10 measures the footprint of Alcedo Studio after a run
  and after a batch.
- Not measured: M4 Pro, M4 Max, and other Apple GPUs; Intel Macs; Macs with more or less than
  16 GB; Core ML input and output in `IOSurface` memory.

##### Phase DN1 addition (2026-11-22): FP16 on the Neural Engine, and the RAW processing of macOS

The user found the 3.55 s of the GPU too slow and asked for other ways and for a comparison with
the RAW processing of the macOS 27 system API. Lab commit `54867e1`; lab entry
`inference_cost.md`, 2026-11-22, FP16 on the Neural Engine. The same Mac and planner.

- **The GPU in FP32 is near its limit.** At 45.4 MP with tiles of 1536 the model computes 4.9
  TFLOP. Core ML reaches 1.4 to 1.8 TFLOP/s; the FP32 peak of the 8-core M4 GPU is about 3.4
  TFLOP/s (8 cores x 128 ALUs x 2 x about 1.5 GHz, an estimate). Own Metal code can save at most
  about half of the 3.55 s. FP16 on the GPU saves 7% to 15% (3.30 s).
- **FP16 on the Neural Engine:** the lab rescales the layers exactly (FP32 output within
  8.7e-7, scale of the last layer put back on the host) so that the activations fit in FP16
  (largest 119 to 3,413 on 11 photos, margin 19 times to 65,504). Core ML then places each of
  the 52 operations on the Neural Engine (one session for each tile shape; a free-shape session
  puts half of them on the CPU). The graph has float16 input and output; the Core ML execution
  provider does not take a Cast from a float32 input.

| macOS, M4, tiles of 1536 | 24.3 MP | 36.2 MP | 45.4 MP | Peak footprint |
| --- | ---: | ---: | ---: | ---: |
| Neural Engine, FP16, one session for each shape | **0.37 s** | **0.51 s** | **0.60 s** | 2.0 GiB |
| GPU, FP16, free shape | 1.56 s | 2.58 s | 3.30 s | 3.2 GiB |
| GPU, FP32, free shape (the DN10 configuration) | 1.83 s | 2.82 s | 3.55 s | 5.5 GiB |
| DirectML FP32, RTX 3080 Laptop GPU (Windows) | 0.49 s | 0.71 s | 0.82 s | 3.3 GiB |

| Output of the Neural Engine against FP32 (developed PSNR) | +2 EV whole | +2 EV dark | +5 EV dark |
| --- | ---: | ---: | ---: |
| 24.3 MP, Canon EOS R3 | 80.6 dB | 82.7 dB | 75.2 dB |
| 32.5 MP, Canon EOS 90D, high ISO | 80.3 dB | 82.4 dB | 74.9 dB |
| 42.4 MP, Sony ILCE-7RM3 | 75.7 dB | 67.8 dB | 60.2 dB |

- The float16 stored result (U2) has at least 81.8 dB, the lossy JPEG XL formats 26 to 54 dB.
- **RAW processing of macOS:** Core Image `CIRAWFilter` (decoder version 8) takes 0.36 to 0.60 s
  for the whole photo (decode, demosaic, its luminance and color noise reduction, color, render
  to host memory) on 24.0, 32.3, and 42.2 MP files. The macOS 27 SDK has no other RAW noise
  reduction API. The Neural Engine time of the model (0.37 s at 24.3 MP) is in that range and
  equals the Metal DemosaicNet student (377 ms).
- **Windows:** FP16 on DirectML saves 17% to 20% (0.74 s against 0.89 s at 45.4 MP), with a
  photo output within 3.8e-3 of the largest FP32 value. The RTX 3080 Laptop GPU has no Neural
  Engine; the Windows gain of FP16 is small.
- **Effect:** decision U5 (section 1.3). The user agreed on 2026-11-22: macOS runs the FP16
  graph on the Neural Engine (D2, DN10). This replaces the GPU configuration of the previous
  addition ("Selected for DN10"); the macOS graph is the FP16 graph with DepthToSpace, so the
  graph without DepthToSpace is not part of the model package.

##### Phase DN1 addition (2026-11-22): FP16 on DirectML

The user asked about FP16 on Windows too. Lab commit `d98a14f`; lab entry `inference_cost.md`,
2026-11-22, FP16 on DirectML. The FP16 graph of the previous addition on DirectML (ONNX Runtime
1.24.4), one session with free height and width.

| Windows, DirectML | 24.3 MP | 36.2 MP | 45.4 MP | One tile of 1536 in device memory |
| --- | ---: | ---: | ---: | ---: |
| RTX 3080 Laptop GPU, FP32, tiles of 1536 | 0.53 s | 0.78 s | 0.94 s | 0.090 s |
| RTX 3080 Laptop GPU, FP16, tiles of 1536 | 0.44 s | 0.55 s | 0.72 s | 0.044 s |
| Intel Iris Xe, FP32, tiles of 1024 | 3.57 s | | 6.68 s | |
| Intel Iris Xe, FP16, tiles of 1024 | 2.76 s | | 5.16 s | |

- FP16 halves the GPU time of a tile, but the full run is only 1.2 to 1.4 times faster: with
  the tile input and output through host memory, the copies and the host tile work are about
  half of the FP16 time (a `run` call of a tile of 1536: 0.095 s, of which 0.044 s on the GPU).
  A tile loop with the packed image and the output in DirectML memory (IOBinding) can bring the
  45.4 MP time near 6 x 0.044 s plus one upload and one readback.
- Developed PSNR of DirectML FP16 against FP32: 70.0 to 72.8 dB at +2 EV on the whole image,
  69.7 to 71.0 dB at +5 EV on the dark crops (Neural Engine: 75.7 to 80.6 and 60.2 to 75.2 dB).
- On the Iris Xe, two of four processes ended with the DirectML error 887A0005 (device removed),
  one of them in FP32 before any run; the cause is open.
- **Effect:** decision U6 (section 1.3).

### DN2: FP16 graph, ONNX Runtime DirectML dependency, UtNet2 session

Rewritten on 2026-11-22 for D2 (ONNX Runtime DirectML in C++), then for FP16 (U5, U6). The plan
of own CUDA layers of UtNet2 is replaced; the DN1 addition of the last tile states the cuDNN
level that those layers needed.

**Objective and deliverables**

- In the lab: the FP16 rescale calibration on many photos, the FP16 ONNX graph as a release
  asset beside the safetensors weights (with its SHA-256, `output_scale`, the calibration record,
  and the metadata of the weights file), and sheets of the FP16 and the FP32 output for a check
  by eye by the user. This graph is the model of Windows and macOS (D2).
- In Alcedo Studio: ONNX Runtime with DirectML as a C++ dependency on Windows, one session of
  UtNet2 that runs one tile of the FP16 graph, and its tests against the DN0 expected outputs.

**Inputs and prerequisites**

DN0. The DN1 additions of 2026-11-22 (ONNX Runtime DirectML, FP16 on the Neural Engine, FP16 on
DirectML). `build/debug` configured.

**Modules, files, and APIs**

Lab:

| File | Responsibility |
| --- | --- |
| `scripts/export/export_onnx.py` (proposed) | The FP32 graph (the export function of `measure_onnx_directml.py`), the reference of the FP16 graph in the lab |
| `scripts/export/export_fp16_onnx.py` (proposed) | From `macos/write_fp16_graph.py`: the rescale calibrated on all evaluation photos and on crops of the RawNIND test split, with a target that keeps a margin of 8 times or more to 65,504 on the photos outside the calibration (`check_fp16_range.py`); the FP16 graph with float16 input `input` (1, 4, h, w) and output `output` (1, 3, 2h, 2w), height and width free; `metadata_props`: `architecture`, `leaky_relu_slope`, `input_preparation_version`, the SHA-256 of the safetensors weights, `precision` = `float16`, `output_scale`, the calibration photo list and the smallest margin |
| `scripts/export/make_fp16_sheets.py` (proposed) | +2 EV and +5 EV sheets of the FP16 and the FP32 output of the evaluation photos (whole image, dark crops, detail crops), for the check by eye |

Alcedo Studio (headers under `include/decoders/processor/nn/`, sources under
`decoders/processor/nn/`):

| File (proposed) | Responsibility |
| --- | --- |
| `cmake/OnnxRuntimeDirectMl.cmake` | Downloads the NuGet packages `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and `Microsoft.AI.DirectML` 1.15.4 at configure time with `file(DOWNLOAD ... EXPECTED_HASH SHA256=...)` into the build tree, extracts them, and defines the imported target `alcedo::onnxruntime_directml` (headers, `onnxruntime.lib`, `onnxruntime.dll`). Windows x64 only |
| `ui/alcedo_main/CMakeLists.txt` | Copies `onnxruntime.dll` and `onnxruntime_providers_shared.dll` beside `alcedo_main` in the build and the install. DirectML.dll comes from the DirectML package; the build fails when the DirectML.dll that the AI Mask sidecar build copies has a different file version |
| `utnet2_specs.hpp` | Input and output names, the alignment of 16, the tile sizes, the metadata keys and values that the sessions check |
| `utnet2_directml_session.hpp`, `utnet2_directml_session.cpp` | `UtNet2DirectMlSession`: one `Ort::Env` for the process, one session with free height and width on the DirectML adapter of high performance, memory arena on, CPU fallback refused (`session.disable_cpu_ep_fallback`), graph optimization level all, `ORT_SEQUENTIAL`, memory pattern off. `Run(tile)`: float32 host input (4, h, w) converted to float16, the float16 output (3, 2h, 2w) checked for values that are not finite and converted to float32 times `output_scale` (F16C). `Release()` ends the session and frees its memory arena |

**Data rules**

- FP16 graph, float16 input and output; the conversion and the scale on the host.
- The session fails at load when the graph file is absent, when a metadata value differs from
  `utnet2_specs.hpp` (`precision` included), or when a node cannot run on DirectML. The error text
  names the file and the first difference.
- `Run` throws `std::runtime_error` with the ONNX Runtime status text and the tile shape, for a
  height or a width that is not a multiple of 16, and for an output value that is not finite
  (with the tile rectangle).
- The session is not used on the CPU execution provider in the application (2.4).
- Only `decoders/processor/nn` includes ONNX Runtime headers. The ONNX Runtime types do not appear
  in a public header of another module.
- The application has one `onnxruntime.dll` (1.24.4) and one DirectML.dll (1.15.4). The AI Mask
  sidecar keeps its own static ONNX Runtime 1.28 in its process; it has no part in the noise
  reduction.

**Implementation steps**

1. Lab: write `export_onnx.py`, `export_fp16_onnx.py` (calibration, margin check, graph), and
   `make_fp16_sheets.py`. Add the FP16 graph and its SHA-256 to the release asset of DN0 and to
   the README of `scripts/export/`. Give the sheets to the user; a visible difference stops DN2.
2. Write `cmake/OnnxRuntimeDirectMl.cmake` and the install rules. Check the install folder: one
   `onnxruntime.dll`, one DirectML.dll, and the AI Mask sidecar still starts with its DirectML
   session.
3. Write `utnet2_specs.hpp` and `UtNet2DirectMlSession`.
4. Add the test target `UtNet2DirectMlTest` (proposed, `alcedo_studio/tests/ai_denoise/`, built
   on Windows only). The tests read the graph file and the DN0 expected outputs from the folder
   in the environment variable `ALCEDO_AI_DENOISE_TEST_ASSETS`; without it, they are skipped
   with a message that names the variable.

**Primary success call chain**

```text
test -> UtNet2DirectMlSession(graph path)
     -> Ort::Env -> SessionOptions (DirectML adapter, arena on, no CPU fallback)
     -> Ort::Session (graph metadata checked: architecture, precision, output_scale)
     -> Run(host tile) -> float16 input -> Ort::Session::Run on DirectML
     -> float16 output check -> float32 times output_scale -> host output
     -> compare with the DN0 expected output
```

**Primary failure call chain**

```text
graph with another architecture, an FP32 graph, or a node that DirectML cannot run
  -> the session constructor throws std::runtime_error with the file path and the reason
  -> no tile runs; the caller reports the error
output tile with an infinite value
  -> Run throws std::runtime_error with the tile rectangle; the caller reports the error
```

**Tests** (`UtNet2DirectMlTest`)

| Test | Assertion |
| --- | --- |
| `OutputMatchesPyTorchForEachExpectedOutput` | `random_64`, `random_256`, and `rawnind_crop_512` within 7e-3 of the largest expected value (set from the calibrated graph: 1.5 times its largest DirectML difference, 4.07e-3, rounded up) |
| `LoadFailsOnDifferentArchitectureMetadata` | A copy of the graph with a different `architecture` value gives an error that names the key |
| `LoadFailsOnMissingFile` | The error names the path |
| `RunRejectsSizeThatIsNotAMultipleOfSixteen` | A 4 x 72 x 64 input throws; no output |
| `RunThrowsOnAnOutputThatIsNotFinite` | An input of 6e4 in each value overflows FP16; `Run` throws with the tile rectangle |
| `ReleaseFreesTheSessionAndALaterRunLoadsAgain` | After `Release`, `Run` loads the session again and gives the same output |

**Build and run commands**

```text
Set-Location <repository root>
cmd /c "scripts\msvc_env.cmd --build build\debug --target UtNet2DirectMlTest"
$env:ALCEDO_AI_DENOISE_TEST_ASSETS = "<folder with the graph and the expected outputs>"
ctest --test-dir build/debug -R "UtNet2DirectMlTest" -j 1 --output-on-failure
```

**Exit criteria**

- [x] The FP16 graph is in the lab release with its SHA-256; the smallest margin of the photos
      outside the calibration is 8 times or more.
- [x] The user checked the sheets by eye (2026-11-22: no visible difference; FP16 accepted).
- [x] The six tests pass in `build/debug` (seven with `LoadFailsOnFloat32Graph`).
- [x] The install folder has one `onnxruntime.dll` and one DirectML.dll, and the AI Mask sidecar
      runs its DirectML session.

**Expected diff**: 900 to 1400 lines (lab included).

##### Phase DN2 completion record (2026-11-22)

**Status:** complete. The lab FP16 graph, the ONNX Runtime DirectML dependency, the session,
and its tests. The user checked the sheets by eye on 2026-11-22: no visible difference, FP16
accepted. Issue #1038; lab PR zidage/alcedo-denoise-lab#4 (stacked on #3).

**Primary success call chain:**

```text
UtNet2DirectMlSession(graph path)                       decoders/processor/nn
  -> Impl::Load -> ProcessEnv -> OnnxRuntimeApi
     -> LoadLibraryExW("onnxruntime.dll", folder of the executable, then System32)
     -> OrtGetApiBase()->GetApi(ORT_API_VERSION) -> Ort::InitApi    (delay-loaded DLL)
  -> SessionOptions: ORT_ENABLE_ALL, ORT_SEQUENTIAL, memory pattern off,
     session.disable_cpu_ep_fallback = 1, OrtDmlApi::SessionOptionsAppendExecutionProvider_DML2
     (HighPerformance, Gpu)
  -> Ort::Session(env, graph) -> CheckGraph: names, float16, channels, metadata
     (architecture, precision, input_preparation_version, weights_sha256, output_scale)
Run(float32 input span, PackedTileRect, float32 output span)
  -> size checks -> (session created again after Release)
  -> cv::Mat::convertTo CV_16F (host) -> Ort::Value over the reused float16 buffers
  -> Ort::Session::Run on DirectML -> convertTo CV_32F times output_scale
  -> cv::checkRange (finite) -> output span
```

**Primary failure call chain:**

```text
graph missing / FP32 graph / other metadata / node DirectML cannot run / DLL load error
  -> Impl::Load throws std::runtime_error("UtNet2 graph <path>: <first difference or ORT text>")
  -> no session; the caller reports the error (DN6)
tile height or width not a multiple of 16, or a span of the wrong size
  -> Run throws before any conversion; the output span is not written
output value not finite (FP16 overflow)
  -> Run throws "UtNet2 tile at (x, y) of w x h packed px: ... channel c, output row r, column k"
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OutputMatchesPyTorchForEachExpectedOutput` (4.07e-3, 3.76e-3, 2.76e-3 of the largest value) | `UtNet2DirectMlTest` | PASS |
| `LoadFailsOnDifferentArchitectureMetadata` | `UtNet2DirectMlTest` | PASS |
| `LoadFailsOnFloat32Graph` (added: the FP32 graph of the failure chain) | `UtNet2DirectMlTest` | PASS |
| `LoadFailsOnMissingFile` | `UtNet2DirectMlTest` | PASS |
| `RunRejectsSizeThatIsNotAMultipleOfSixteen` | `UtNet2DirectMlTest` | PASS |
| `RunThrowsOnAnOutputThatIsNotFinite` | `UtNet2DirectMlTest` | PASS |
| `ReleaseFreesTheSessionAndALaterRunLoadsAgain` (bit-equal output after the new load) | `UtNet2DirectMlTest` | PASS |
| Without `ALCEDO_AI_DENOISE_TEST_ASSETS` each test skips with a message that names it | `UtNet2DirectMlTest` | SKIPPED as intended |
| FP16 calibration: smallest held-out margin 71.7 (8 required) on 78 photos | lab `export_fp16_onnx.py` | PASS |
| `alcedo_main` folder: one `onnxruntime.dll` (package file), one `onnxruntime_providers_shared.dll`, one DirectML.dll with the SHA-256 of the sidecar copy (`9c9e6d82...`) | `build/debug` | PASS |
| Install folder (`cmake --install build/debug`): one file each of the three DLLs | `build/tmp/dn2/install/bin` | PASS |
| `verify_windows_install_tree.ps1` on that install | | the new entries pass; the script stops at `Qt6Core.dll` because a debug install has `Qt6Cored.dll` |
| AI Mask sidecar beside the new DLLs (gRPC with grpcurl): `LoadSegmentationModels` with device `directml` loads the 5 models on `directml:0` (backend `onnxruntime-directml`), and `StartSegmentationSession` runs the encoder on a 64 x 64 image (125 ms; 166 ms from the install folder); the process loads the DirectML.dll of the folder and no `onnxruntime*.dll` (its import table has none) | `build/debug` and the install folder | PASS |

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target UtNet2DirectMlTest"
$env:ALCEDO_AI_DENOISE_TEST_ASSETS = "<folder: the FP16 graph, the FP32 graph, the 3 expected outputs>"
ctest --test-dir build/debug -R "UtNet2DirectMlTest" -j 1 --output-on-failure   # 7/7
cmd /c "scripts\msvc_env.cmd --build build\debug --target alcedo_main"
cmd /c "scripts\msvc_env.cmd --install build\debug --prefix <absolute folder> --component Unspecified"
lab: export_onnx.py, export_fp16_onnx.py, make_fp16_sheets.py (README of scripts/export/)
```

Lab results: the FP16 graph (15,552,242 bytes, SHA-256 `0bc98084c9bff16d...`, `output_scale`
170.34) and `fp16_calibration.json` are assets of the release
`export-utnet2-bayer-dark-loss-10k`. FP16 against FP32 on the 11 evaluation photos: 68.2 to
76.5 dB at +2 EV on the whole image, 73.3 dB or more on the dark crops, 60.8 to 75.3 dB at +5 EV
(lab `quality.md`, 2026-11-22). The sheets are in the lab folder
`results/export/images/fp16_sheets/` (not committed).

Detail choices made during the work (the plan did not state them):

- The CMake module is `scripts/cmake/fetch_onnxruntime_directml.cmake` with the copy script
  `scripts/cmake/copy_onnxruntime_directml_runtime.cmake`, beside `fetch_aria2.cmake`; the
  repository has no root `cmake/` folder. The library target is `UtNet2DirectMl`
  (`decoders/CMakeLists.txt`). `alcedo_main` gets the DLLs now and links the target in DN6.
- `onnxruntime.dll` is delay-loaded (`/DELAYLOAD`, `ORT_API_MANUAL_INIT`) and loaded by the
  session from the folder of the executable, so `alcedo_main` starts without it (2.4: the
  switch is hidden and a render reports the load error). Windows 11 has an older
  `System32\onnxruntime.dll`; when it is loaded, the API version check names its path.
- The DirectML.dll check compares the SHA-256 of the package file and of the sidecar copy at
  each build (the same file of the same package), in place of the file version.
- `Run` takes float32 spans and a `PackedTileRect` (the origin of the tile in the photo), so that
  the error names the tile rectangle; DN3 gives the rectangle of each tile.
- The adapter is selected with `OrtDmlApi::SessionOptionsAppendExecutionProvider_DML2`
  (HighPerformance, Gpu), the documented DirectML API; the generic provider registration does
  not list DML.
- The host conversion uses OpenCV `convertTo` (`CV_16F`), which uses F16C, in place of own F16C
  code.
- Calibration set: the whole evaluation photos and centre crops of 1536 packed px of all raw
  files of the RawNIND test split. Held-out set: the whole raw files of the 8 RawNIND scenes
  outside the test split. Rescale target 1024 as in DN1.
- FP16 test tolerance 7e-3 (rule above), and the test `LoadFailsOnFloat32Graph`.
- The installer gets the license texts of ONNX Runtime and DirectML (`THIRD_PARTY_NOTICE.txt`
  entries 33 and 34); DirectML.dll of the sidecar had no entry before.

**Checklist / exit condition:** all four items checked.

**LOC note (grill-code-review):** Alcedo Studio: 848 new lines (session 319 + 80 + 48, test 221
+ 20, CMake 160) and about 50 changed lines in existing CMake, the install check, and the notice.
Lab: 3 new scripts (633 lines) and the JSON records (`fp16_calibration.json` 824 lines,
`fp16_sheets.json` 902 lines). No file is near 1000 lines of code.

**Remaining gaps:** the session on the Intel Iris Xe and on an AMD GPU (DN3 timing tool); the
release install check (`verify_windows_install_tree.ps1` needs a release install).

### DN3: overlap tile planner and full photo run on DirectML

Rewritten on 2026-11-22 for D2, then for FP16 and the run in the render (D5, D6).

**Objective and deliverables**

The tile loop that denoises one full Bayer mosaic with `UtNet2DirectMlSession`, a test against
one pass, and the measured time for 24, 36, and 45 megapixel photos. The model runs in each full
render whose sensor stage is computed, so the reference is the DemosaicNet student (376 ms on
36.3 MP), and the target is `target_time` (U1). The phase also adds the slow-device check (2.4).

**Inputs and prerequisites**

DN2. The `tile_size` and `tile_overlap` values of DN1 and the session strategy of the DN1
addition (one session with free height and width).

**Modules, files, and APIs** (proposed)

| File | Responsibility |
| --- | --- |
| `include/decoders/processor/nn/overlap_tile_jobs.hpp` | `PlanOverlapTiles(width, height, tile, overlap, alignment)`: the input rectangle and the kept output rectangle of each tile (lab `measurement_common.plan_axis` with the shrunk last tile) |
| `decoders/processor/nn/utnet2_tile_loop.cpp` | `DenoiseBayerMosaic(session, packed mosaic, tile callback, cancel check, clock)`: the tile loop for `UtNet2DirectMlSession` and `UtNet2CoreMlSession` (the same interface), the tile size selection, the input tile with reflection, `Run`, the crop to the kept rectangle, the slow-device check (2.4) |
| `include/decoders/processor/nn/denoise_errors.hpp` | `DenoiseDeviceTooSlowError` (a `std::runtime_error` with the projected and the allowed time), so that DN6 keeps the result and DN7 shows the slow-device text |
| `alcedo_studio/tests/perf/utnet2_photo_timing_tool.cpp` | Full photo time, the time of `Run`, and the used GPU memory |

**Data rules**

- The kept output rectangles of all tiles cover the image exactly one time.
- Each tile input starts at a multiple of 16 packed px. The last tile of an axis is shrunk: its
  input starts `tile_overlap` px before its kept range, ends at the image border, and is filled
  by reflection to a multiple of 16 (DN1 addition of the last tile). A fill to the full tile
  size is not used: it changes the border output (DN1 addition, DirectML).
- One session with free height and width for all tiles and photos of a run. Sessions for each
  tile shape are not used: each keeps its own memory arena, and at tiles of 1536 they spill to
  shared memory on an 8 GB GPU (DN1 addition, DirectML).
- Tile size: the largest of 1536, 1024, and 768 whose measured process memory fits in the free
  local video memory of the adapter (`IDXGIAdapter3::QueryVideoMemoryInfo`, budget minus current
  usage). DN3 measures the memory of the FP16 graph for the three sizes (FP32: 3.3, 1.0, and
  0.9 GiB). When 768 does not fit, the run throws the memory error. It does not use a different
  backend.
- Tile input and output in float16: the loop binds float16 DirectML buffers (`IOBinding`) and
  reuses them for each tile of a size. The timing tool reports the GPU time of the tiles beside
  the full time (DN1: 0.044 s of GPU time in a `run` of 0.095 s for a tile of 1536). When the
  full time at 45.4 MP is more than 1.5 times the GPU time, DN3 also keeps the packed image in
  DirectML memory and extracts the tiles there.
- The cancel check runs between tiles (a render that the editor no longer needs).
- The slow-device check of 2.4 runs between tiles, after the cancel check. Its clock is a
  parameter (a steady clock in the product, a fake clock in the tests). The time of the
  session creation and of the first run of a session is excluded. Above the limit, the loop
  throws `DenoiseDeviceTooSlowError` and runs no more tiles.
- The tile loop does not own the session: `DenoiseModelSession` (DN6) keeps it between renders
  and releases it (2.3).

**Implementation steps**

1. Write the planner and its tests.
2. Write the tile loop: tile size selection, input tile with reflection, `Run`, crop, callback
   with the RGB tile.
3. Write the tile loop tests.
4. Write the photo timing tool. For each photo it reports the tile size, the tile count, the time
   of the tile loop, the sum of the `Run` times, the used GPU memory of the process, and the GPU
   temperature and clock (NVML when the adapter is NVIDIA).
5. Run the tool on the three photos in the release build. Record the table beside the DN1
   DirectML and PyTorch times and against `target_time` (0.72 s at 45.4 MP on the RTX 3080
   Laptop GPU).
6. Run the tool on the Intel Iris Xe. Record the time for each megapixel and the tile at which
   the slow-device check stops the run.

**Primary success call chain**

```text
DenoiseBayerMosaic
  -> UtNet2DirectMlSession (load one time for the run)
  -> select the tile size from the free video memory
  -> PlanOverlapTiles
  -> for each tile: input with reflection -> Run -> crop -> callback(tile RGB) -> cancel check
```

**Primary failure call chain**

```text
DirectML device removed or out of memory during Run
  -> Run throws std::runtime_error with the ONNX Runtime status text
  -> no more tile runs; the render reports the error (2.4)

Projected time above slow_device_limit after a measured tile
  -> DenoiseBayerMosaic throws DenoiseDeviceTooSlowError
  -> no more tile runs; DenoiseModelSession keeps the result (DN6); the render reports the
     slow-device error with Turn Off AI Denoise (2.4)
```

**Tests**

| Test | Assertion |
| --- | --- |
| `OverlapTilesCoverTheImageExactlyOnce` | For sizes that are and are not multiples of the tile, each output pixel is in one kept rectangle |
| `OverlapTileInputsAreMultiplesOfSixteen` | Each input rectangle starts at a multiple of 16 and has a width and a height that are multiples of 16, also for image sizes that are not multiples of 16 |
| `TiledOutputEqualsOnePassOnTheExpectedCrop` | On `rawnind_crop_512` with tiles of 256 and overlap 96, the tiled output equals the one-pass output within 1e-5 of the largest value |
| `TileSizeSelectionUsesTheLargestThatFits` | For free memory values below and above the three thresholds, the selected size is the largest that fits; below the smallest, the error |
| `TileLoopStopsAtCancelRequest` | After the cancel request, no more tile callback runs |
| `TileLoopStopsWhenTheProjectedTimeIsAboveTheLimit` | With a fake session and a fake clock at 0.114 s for each megapixel, the loop throws `DenoiseDeviceTooSlowError` after the first measured tile; at 0.016 s it runs all tiles |
| `SlowDeviceCheckExcludesTheFirstRunOfASession` | A first run of 2 s on a new session followed by fast tiles completes; a photo of one tile on a new session completes without a check |

**Exit criteria**

- [x] The seven tests pass in `build/debug` (nine with the two added tests).
- [x] The time table of the three photos is in the completion record. The 45.4 MP time on the
      RTX 3080 Laptop GPU is at most 15% above `target_time` (0.83 s), or the record states the
      cause and the phase stops (U1). (0.45 s.)
- [x] The Iris Xe run stops with the slow-device error, and the record states its time.

**Expected diff**: 800 to 1200 lines.

##### Phase DN3 completion record (2026-11-22)

**Status:** complete. The overlap tile planner, the tile loop with the slow-device check, the
float16 tile input and output in DirectML buffers of the session, the tests, the photo timing
tool, and the times on the RTX 3080 Laptop GPU and the Intel Iris Xe. Issue #1042. A false
slow-device error on the RTX 3080 Laptop GPU when other processes hold the video memory is issue
#1043; section 8 makes it a stop condition of DN6.

**Primary success call chain:**

```text
DenoiseBayerMosaic(session, packed mosaic, output, on_tile, cancel check, clock)
                                                    decoders/processor/nn/utnet2_tile_loop.cpp
  -> UtNet2TileSession::DeviceMemory (DXGI QueryVideoMemoryInfo: budget, usage, held arena)
  -> SelectUtNet2TileSize(UtNet2Spec::kDirectMlTileMemory, budget - usage + held)
  -> PlanOverlapTiles (overlap 96, inputs at multiples of 16, last tile shrunk)
  -> for each tile: cancel check -> slow-device check of the tiles before
     -> CopyTileInput (reflection past the bottom and the right border)
     -> UtNet2TileSession::IsReadyFor(shape) (measured or not)
     -> UtNet2DirectMlSession::Run(input, tile rect, crop of the kept rectangle)
        -> float32 to float16 (cv::parallel_for_) into the mapped upload buffer
        -> queue: copy to the DirectML input buffer, timestamp 0
        -> Ort::IoBinding over the DirectML allocations (CreateGPUAllocationFromD3DResource)
           -> Ort::Session::Run on the queue of the session (DML1: own ID3D12Device, IDMLDevice,
              ID3D12CommandQueue) -> SynchronizeOutputs
        -> queue: timestamp 1, copy of the rows of the kept rectangle to the readback buffer,
           fence wait
        -> float16 to float32 times output_scale into the output image, finite check (parallel)
     -> on_tile(index, input rect, kept rect)
  -> slow-device check after the last tile -> UtNet2TileLoopRecord
```

**Primary failure call chain:**

```text
projected time above slow_device_limit x megapixels after a measured tile
  -> DenoiseDeviceTooSlowError(projected, limit, tiles run, tile count); no more tiles
  -> the caller (DN6) keeps the result for the device and reports the slow-device error
too little local video memory for tiles of 768
  -> SelectUtNet2TileSize throws "not enough video memory: tiles of 768 packed px need 736 MiB"
DirectML, ONNX Runtime, or Direct3D 12 failure in a tile (device removed included)
  -> Run throws std::runtime_error with the tile rectangle, the status text, and the device
     removed reason; no more tiles
cancel check true between tiles
  -> the loop returns the record with `cancelled`; the output image is incomplete
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OverlapTilesCoverTheImageExactlyOnce` (12 sizes; also: each kept rectangle has the overlap of context inside the image) | `UtNet2TileLoopTest` | PASS |
| `OverlapTileInputsAreMultiplesOfSixteen` (12 lengths x 4 tile sizes; the 45.4 MP width keeps a last tile of 1440) | `UtNet2TileLoopTest` | PASS |
| `TileSizeSelectionUsesTheLargestThatFits` (also: the loop counts the memory that the session holds) | `UtNet2TileLoopTest` | PASS |
| `TileInputsAreTheMosaicWithReflectionAndTheCropsFormTheOutput` (added: each input value of each tile, and each output value of a session with a known one-pass output) | `UtNet2TileLoopTest` | PASS |
| `TileLoopStopsAtCancelRequest` | `UtNet2TileLoopTest` | PASS |
| `TileLoopStopsWhenTheProjectedTimeIsAboveTheLimit` (0.114 s/MP: stop after tile 1 of 8; 0.016 s/MP: 8 tiles) | `UtNet2TileLoopTest` | PASS |
| `SlowDeviceCheckExcludesTheFirstRunOfASession` | `UtNet2TileLoopTest` | PASS |
| `TiledOutputEqualsOnePassOnTheExpectedCrop` (one tile through the loop: bit-equal to one pass; tiles of 256: 2.45e-3, the level of a change of the input shape 2.45e-3) | `UtNet2DirectMlTest` | PASS |
| `SessionIsReadyOnlyForATileShapeThatRan` (added) | `UtNet2DirectMlTest` | PASS |
| The 7 DN2 tests on the new device path (3.76e-3 to 4.07e-3, as in DN2) | `UtNet2DirectMlTest` | PASS |

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target UtNet2TileLoopTest UtNet2DirectMlTest UtNet2PhotoTimingTool"
$env:ALCEDO_AI_DENOISE_TEST_ASSETS = "<folder: the FP16 graph, the FP32 graph, the 3 expected outputs>"
ctest --test-dir build/debug -R "UtNet2TileLoopTest|UtNet2DirectMlTest" -j 1 --output-on-failure   # 16/16
cmd /c "scripts\msvc_env.cmd --build build\release-test --target UtNet2PhotoTimingTool"
lab: python -I scripts/export/write_raw_inputs.py <folder>     # the packed photos, input preparation 1
UtNet2PhotoTimingTool --graph <fp16 graph> --inputs <folder> --photos 24mp 36mp 45mp --repeats 3 --tile auto
UtNet2PhotoTimingTool ... --photos 45mp --tile 1024 (768)          # one process for each tile size
UtNet2PhotoTimingTool ... --adapter minimum-power [--no-slow-device-check] [--per-tile]
```

**Time table** (release build, RTX 3080 Laptop GPU 8 GB with about 2.4 GiB used by other
programs, tiles of 1536 selected from the device memory, median of 3 runs after one run that is
not measured; GPU 58 to 66 C, SM clock 1725 to 1800 MHz):

| Photo | Tile loop | Sum of `Run` | Device time | DN1 DirectML FP16 (Python, host input and output) | DN1 PyTorch FP32, fastest algorithms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 24.3 MP (6 tiles) | **0.27 s** (0.26 to 0.27) | 0.25 s | 0.19 s | 0.44 s | 0.41 s |
| 36.2 MP (6 tiles) | **0.36 s** (0.34 to 0.39) | 0.32 s | 0.24 s | 0.55 s | 0.56 s |
| 45.4 MP (6 tiles) | **0.45 s** (0.44 to 0.48) | 0.42 s | 0.33 s | 0.72 s | 0.69 s |

- **Against `target_time`:** 0.45 s against 0.72 s at 45.4 MP, 37% below. A second process run
  of the same photo gave 0.41 to 0.43 s.
- **Against DemosaicNet:** 0.36 s on 36.2 MP; the DemosaicNet student takes 376 ms on 36.3 MP.
- Tiles of 1024 at 45.4 MP: 0.58 s (0.56 to 0.62), 20 tiles; tiles of 768: 0.70 s (0.68 to
  0.74), 35 tiles.
- Local video memory of the process (DXGI `CurrentUsage`, the session creates nothing before):
  2672 MiB after the 45.4 MP photo and 2699 MiB after the three photos with tiles of 1536; 753
  MiB (1024) and 730 MiB (768). FP32 in DN1: 3.3, 1.0, 0.9 GiB. `kDirectMlTileMemory` holds
  2704, 768, and 736 MiB.
- First photo on a new session (session creation 0.32 to 0.39 s before it): 0.79 s (24.3 MP) and
  0.98 s (45.4 MP) for the tile loop. The first tile of each new tile width compiles: 0.34 s
  (1536 x 1536) and 0.22 to 0.35 s (1440 x 1536) of device time; a new height of a known width
  took no extra time.
- **Host time:** before the parallel conversions, the tile loop took 0.62 to 0.69 s at 45.4 MP,
  2.2 times the device time. A tile of 1536 had 8.5 ms for the input conversion, 20 ms for the
  output conversion, 15 ms for the finite check, and 4.5 ms for the readback, beside 47 ms on the
  device. With the conversions and the check in `cv::parallel_for_`, the tile loop is 1.39 to
  1.46 times the device time. The data rule keeps the packed image in DirectML memory only above
  1.5 times, so the packed image stays in host memory.

**Intel Iris Xe** (the same laptop, `--adapter minimum-power`, tiles of 1536 selected: the
local budget of this UMA adapter is 15.5 GiB):

| Run | 24.3 MP | 45.4 MP |
| --- | --- | --- |
| Slow-device check, ready session | stops after tile 1 of 6 at 0.67 s; projected 1.91 to 1.92 s, limit 1.22 s | stops after tile 1 of 6 at 0.67 to 0.79 s; projected 3.59 to 3.95 s, limit 2.27 s |
| Slow-device check, new session | the first tile compiles (9.4 s, not measured); stops after tile 2 at 10.1 s; projected 2.11 s | |
| Without the check | 2.19 to 2.30 s (0.090 to 0.094 s/MP) | 3.95 s (0.087 s/MP), 1714 MiB |

No device removal (887A0005) in the two Iris Xe processes of DN3 (the lab saw it in 2 of 4). The
question of section 8, whether the render backend of the editor continues after a removal,
stays with DN6, where the editor render runs beside the session.

**Under load** (issue #1043): two processes that start together both select tiles of 1536. The
second process then runs its tiles in about 0.45 s and gets the slow-device error in 6 of 7
runs (projected 2.5 to 3.5 s); with 6.5 GiB used by other programs, one process with tiles of
1536 got it too. Two processes with tiles of 1536 and 1024 ran at 0.41 to 0.53 s and 0.55 to
0.69 s, so the cause is the oversubscribed video memory, not the compute of the other process.

**Changes against the plan text and detail choices:**

- `TiledOutputEqualsOnePassOnTheExpectedCrop`: the 1e-5 of the plan is the FP32 level of DN1.
  DirectML in FP16 rounds differently for each input shape: a one-pass run on a top left crop of
  256 or 384 packed px differs from the 512 one-pass output by 1.8e-3 to 2.45e-3 of the largest
  value in the px that do not depend on the border. The test now requires one tile through the
  loop to be bit-equal to the one pass, and the tiled output to be within 1.5 times that measured
  level (the rule of the DN2 tolerance; 1e-5 when the level is 0). The exact tile geometry is
  proven by `TileInputsAreTheMosaicWithReflectionAndTheCropsFormTheOutput` with a test session.
- The session runs on its own Direct3D 12 device, queue, and DirectML device
  (`SessionOptionsAppendExecutionProvider_DML1`), in place of `..._DML2`, so that the tile buffers
  are its own D3D12 buffers (`CreateGPUAllocationFromD3DResource`) bound with `Ort::IoBinding`
  and copied on the same queue, and so that the queue gives timestamps. The adapter is the first
  hardware adapter of the DXGI GPU preference (`DirectMlAdapterPreference`; the product uses high
  performance, the timing tool also minimum power). DirectML.dll is loaded from the folder of the
  executable for `DMLCreateDevice1` (feature level 5.0). The queue has
  `D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT`, as the queue that ONNX Runtime creates.
- The buffers grow to the largest tile and are reused for each tile of any size. The readback
  copies only the rows of the kept rectangle; the finite check covers the kept rectangle (the
  values that enter the photo).
- `IsReadyFor` is true for a tile shape that ran on the loaded session (the first tile of a new
  width compiles, see above). The DN2 text "the first run of a session" is extended to each
  shape, as on macOS (2.4 changed).
- The slow-device check also runs after the last tile: the device is too slow for the next
  renders, and DN6 keeps the result.
- Tile size selection: available = budget - usage + the memory that the session holds (the
  `kDirectMlTileMemory` entry of the largest tile side that the loaded session ran), so a loaded
  session with its arena keeps its tile size.
- Files: the planner also has `overlap_tile_jobs.cpp`, the loop has the public header
  `utnet2_tile_loop.hpp` (the `UtNet2TileSession` interface, the views, the options, the record).
  `PackedTileRect` moved from the session header to `overlap_tile_jobs.hpp`. The library
  `UtNet2TileLoop` has no platform dependency; `UtNet2DirectMl` links it and d3d12 and dxgi. The
  timing tool is `alcedo_studio/tests/ai_denoise/utnet2_photo_timing_tool.cpp` (target
  `UtNet2PhotoTimingTool`, not a test), beside the tests of AI Denoise; `tests/perf` has no CMake
  folder. The test target `UtNet2TileLoopTest` is in the `core` and `ci_core` categories.
- The photos are the packed photos of the lab tool `write_raw_inputs.py` (the DN1 photo list).

**Checklist / exit condition:** all three items checked.

**LOC note (grill-code-review):** 13 files, 1979 added and 152 removed lines (estimate 800 to
1200). Production: `utnet2_directml_session.cpp` 749 lines (319 in DN2; the Direct3D 12 device,
the buffers, the timestamps), `utnet2_tile_loop.cpp` 192, `overlap_tile_jobs.cpp` 66, headers
441. Tests 708 lines, timing tool 311. No file is near 1000 lines.

**Remaining gaps:** issue #1043 (a false slow-device error under video memory load; DN6 stop
condition). Not measured: AMD GPUs; the DN2 output tests on the Iris Xe (the timing tool ran
there; the tests select the high-performance adapter); the editor render backend after a
DirectML device removal (DN6).

### DN5: Develop parameter, denoised input kind, and the Develop passes

Written on 2026-11-22, before the implementation.

**Objective and deliverables**

The Develop node gets the AI Denoise parameter (the switch and the model version, D10), and the
Develop passes of CUDA, OpenCL, and Metal render a denoised prepared input: a camera RGB image
of the whole decode area at the full size. The loader does not run the model in this phase
(DN6); the tests build the denoised input from the DN0 expected outputs and from synthetic
planes. A render that needs the model and has no denoised input reports an error (1.4).

**Inputs and prerequisites**

U4 (closed: no clip mask). The value domain of the gain-matched model output (lab
`docs/alcedo_integration.md`, "Output and gain match"). DN5 does not need DN3.

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `include/edit/graph/develop_node_model.hpp`, `edit/graph/develop_node_model.cpp` | `DevelopAiDenoise` (`model_version`, the SHA-256 of the FP16 graph in 64 lowercase hexadecimal digits); `DevelopPayload::ai_denoise` (`std::optional`; absent is off); `DevelopDirty::Denoise`; `DevelopParamsModel::AiDenoise` and `ApplyAiDenoiseUpdate`; the JSON key `ai_denoise` |
| `include/decoders/processor/raw_processor_pattern.hpp`, `include/edit/runtime/develop_compile_source.hpp` | `RawInputKind::DenoisedRgb`, `DevelopInputKind::DenoisedRgb` |
| `include/edit/input/raw_input_loader.hpp`, `edit/input/raw_input_loader.cpp` | `RawInputLoader::FromDenoisedBayer(cfa input, denoised plane, model version)` |
| `include/edit/input/prepared_raw_input.hpp` | `PreparedSourceKey::denoise_model_hash` |
| `include/decoders/processor/raw_rgb_linearization_params.hpp` and the `linearize_rgb` kernels of CUDA, OpenCL, and Metal | The range clamp after the scale (`clamp_to_range`, `range_max`) |
| `include/edit/runtime/develop_ai_denoise.hpp` (new) | `DenoisedRgbLinearization`, `DevelopRgbLinearization(input, highlights_reconstruct)`, `RequireDevelopInputForAiDenoise(params, source)` |
| `edit/runtime/graph_compiler.cpp`, `edit/runtime/result_content_key.cpp`, `edit/runtime/runtime_invalidation.cpp` | The pass list of the denoised kind, the content keys, the sensor dirty group |
| `edit/runtime/{cuda,opencl,metal}/*_develop_pass.*`, `include/edit/runtime/plan_executor.hpp` | The denoised kind on the RGB upload path of each backend; the input check before the Develop encode |

**Data rules**

- **JSON.** The key `ai_denoise` (`{"model_version": "<64 lowercase hexadecimal digits>"}`) is
  written only when the switch is on, so the JSON and the root id of every other document stay
  as before (section 8). A key that is not such an object, or an `ai_denoise` key in a raster
  document, stops the load with an error; the Model is then unchanged.
- **Dirty group.** A change of `ai_denoise` (on, off, or another model version) marks
  `DevelopDirty::Denoise`. The sensor field mask of the runtime invalidation holds it, so
  `develop.sensor_linear` and all later results are computed again.
- **Denoised plane.** F32 RGBA, the size of the decode area of the full-size CFA input
  (`BuildDecodeCropRect`, the DNG default crop included), camera RGB without white balance. The
  value domain of channel `c` is the domain of the CFA Linearize step before its clamp and its
  white balance: 0 is `black_level[c]`, 1 is `white_level[c]` of `RawLinearizationParams` (green
  uses entry 1). Values below 0 and above 1 are kept. The model domain of the lab uses LibRaw
  `maximum` as the white level; DN6 multiplies channel `c` by the gain match factor and by
  `(maximum - black[c]) / (white_level[c] - black[c])` in one pass.
- **Denoised input.** `FromDenoisedBayer` takes a full-size Bayer CFA input (`LoadEncoded` or
  `FromUnpackedCfa` at `DecodeRes::FULL`) and copies its linearization, color context, DNG warp,
  orientation, and encoded identity. The sensor geometry collapses to the plane, as for the
  direct RGB input, so the develop output is the oriented decode area (the CFA path loses up to
  4 px at a sensor border to the RCD border; the model does not). `source_key.input_kind` is
  `DenoisedRgb` and `source_key.denoise_model_hash` is the FNV-1a hash of the model version.
  A reduced CFA input, an X-Trans input, a plane of another size or format, and a model version
  that is not 64 lowercase hexadecimal digits are rejected.
- **Linearization and highlights.** The denoised input runs the RGB upload path: `linearize_rgb`
  with black 0 and the relative as-shot white balance as the scale (as the CFA Linearize step,
  only when LibRaw has not applied it), then the highlight recovery of the RGB path with the
  `cam_mul` of the CFA input, then the inverse white balance and the pack. The clamp of the CFA
  path is kept: with the highlight recovery on, `linearize_rgb` clamps channel `c` to
  `[0, wb[c]]` (the clamp to the white level before the white balance); with it off, to
  `[0, min(wb[c], 1)]` (also the CfaClamp after the white balance). `RawRgbLinearizationParams`
  gets `clamp_to_range` and `range_max[3]` for it (28 to 44 bytes); the direct RGB input keeps
  `clamp_to_range` 0, so its output does not change.
- **Highlight handling (U4).** No clip mask. The highlight recovery runs unchanged on the
  denoised RGB: clip level 0.987 of the white level after the white balance, soft ramp from
  0.95. DN1 measured the gain-matched output in clipped cells at 0.888 to 0.992 of the white
  level (median), so the recovery finds part of the clipped areas only; the user accepted this
  (U4, 2026-11-22).
- **Pass list.** UploadRgb, HighlightRecover, InverseCamMulPack, Lens, GeometryResample,
  CameraToAp1. No Linearize, CfaClamp, or Demosaic pass. The demosaic method resolves to Legacy
  and runs no model.
- **Content keys.** The sensor key mixes `denoise_model_hash` of the prepared source and the
  switch only when they are set, so the keys of all other inputs and documents stay as before.
  The installed version is in the prepared input; the recorded version of the parameter is not
  mixed (D10: the render uses the installed model).
- **Input check (D12, 1.4).** Before the Develop encode, the plan executor calls
  `RequireDevelopInputForAiDenoise`. Switch on and a full-size input (`downsample_passes` 0) that
  is not denoised: the render error ("AI Denoise is on and the prepared input is not denoised"
  for a Bayer CFA; "AI Denoise supports Bayer sensors only" for X-Trans and direct RGB). A
  denoised input with the switch off: the render error. A reduced input (thumbnail, semantic
  input image) with the switch on renders the CFA path as now.
- **Not in DN5.** The model run in the loader and the denoised cache entry (DN6). The editor
  parameter field (`SetParameter`, the history replay of the owned key `ai_denoise`, the paste of
  the RAW Decode item) is in DN7 with the switch, which writes it.

**Implementation steps**

1. The parameter: payload field, JSON, dirty bit, focused update, equality, tests.
2. The input kinds, `FromDenoisedBayer`, the source key, tests.
3. The range clamp of `linearize_rgb` on the three backends, `develop_ai_denoise.hpp`, the RGB
   upload path for the denoised kind, the transient estimates, the OpenCL kernel warm-up.
4. The pass list, the content keys, the sensor dirty group, the input check, tests.
5. The GPU tests on CUDA and OpenCL in `build/debug`; the Metal test source beside them.

**Primary success call chain**

```text
render with ai_denoise on (DN6 supplies the input; DN5 tests build it)
  -> RawInputLoader::FromDenoisedBayer(full-size CFA input, denoised plane, model version)
  -> GraphCompiler::Compile -> CompileDevelopPasses(DenoisedRgb): UploadRgb, HighlightRecover,
     InverseCamMulPack, Lens, GeometryResample, CameraToAp1
  -> plan executor: BuildFrameResultContentKeys (switch, denoise_model_hash) -> miss
     -> RequireDevelopInputForAiDenoise -> Execute<Backend>Develop (RGB upload path)
        -> upload -> linearize_rgb(DevelopRgbLinearization: white balance, range clamp)
        -> highlight recovery (cam_mul) -> inverse white balance, crop, orientation -> sensor_linear
```

**Primary failure call chain**

```text
ai_denoise on, full-size CFA input (no loader run)
  -> RequireDevelopInputForAiDenoise throws std::runtime_error
  -> the render error; no Develop pass runs
JSON ai_denoise malformed, or present in a raster document
  -> DevelopParamsModel::LoadJson throws before a field changes -> the document load fails
```

**Tests**

| Test | Assertion |
| --- | --- |
| `AiDenoiseKeyIsAbsentWhileTheSwitchIsOff` | The default Develop JSON has no `ai_denoise` key; a document without it keeps its JSON bytes and its root id through decode and encode |
| `AiDenoiseJsonRoundTripsTheModelVersion` | On: the key holds the version; load and write give the same JSON; off again removes the key |
| `AiDenoiseJsonRejectsAMalformedValueAndKeepsTheModel` | A version of another length, upper case, a string in place of the object, and the key in a raster document stop the load; the Model JSON is unchanged |
| `AiDenoiseChangeMarksTheSensorGroupDirty` | `ApplyAiDenoiseUpdate` on, another version, off: each marks `Denoise` once; an equal update marks nothing; a raster document throws |
| `AiDenoiseChangeInvalidatesTheSensorResult` | The runtime invalidation gives `develop.sensor_linear` as the origin of the change |
| `DenoisedInputTakesTheCfaMetadataAndTheDecodeArea` | Kind, extents, crop, orientation, linearization, white balance scale and range of `rgb_linearization`, source key with the model hash |
| `DenoisedInputRejectsAReducedOrNonBayerInputAndAWrongPlane` | The rejected inputs of the data rules throw |
| `DenoisedInputCompilesTheRgbUploadWithHighlightRecovery` | The pass list of the data rules, in order; no Linearize, CfaClamp, Demosaic |
| `SensorKeyHoldsTheSwitchAndTheModelVersion` | The sensor key changes with the switch and with the model hash of the input, and is the earlier key again when the switch is off again; another recorded version alone does not change it |
| `InputCheckRejectsAFullSizeInputThatIsNotDenoised` | The function of the input check for each kind, size, and switch state |
| `DenoisedPlaneBecomesTheSensorImageWithTheCfaClamp` (CUDA, OpenCL, Metal) | Synthetic plane with values below 0, between, and above 1: `develop.sensor_linear` equals `clamp(v x wb, 0, r) / wb` for HLR on (r = wb) and off (r = min(wb, 1)) |
| `FullSizeRenderWithTheSwitchOnAndACfaInputReportsTheError` (CUDA, OpenCL, Metal) | `Execute` throws; a reduced CFA input with the switch on renders |
| `DenoisedExpectedCropKeepsTheLevelsAndLowersTheNoiseOfTheCfaRender` (CUDA, OpenCL, Metal; DN0 assets) | `rawnind_crop_512`: the CFA render of its input mosaic and the render of its gain-matched output; channel means of `develop.sensor_linear` within 3%; the mean absolute Laplacian of the denoised render at most half of the CFA render |

**Exit criteria**

- [x] The tests pass in `build/debug` on CUDA and OpenCL (the Metal tests run in DN10 on the
      Mac, which builds the Metal code).
- [x] The tests of the touched suites (`GpuDagModelGraphTest`, `GpuDagRawInputTest`,
      `PipelineDocumentCheckpointTest`, the CUDA and OpenCL Develop tests) have no new failure.

**Expected diff**: 1000 to 1500 lines.

##### Phase DN5 completion record (2026-11-22)

**Status:** complete. The Develop parameter `ai_denoise`, `RawInputLoader::FromDenoisedBayer`,
the denoised input kind on the RGB upload path of CUDA, OpenCL, and Metal, the range clamp of
`linearize_rgb`, the pass list, the content keys, the sensor dirty group, and the input check of
the plan executor. Issue #1045. The Metal code is written and not built (no Mac in this phase);
DN10 builds and runs its tests.

**Primary success call chain:**

```text
render with ai_denoise on (DN6 supplies the input; the DN5 tests build it)
  -> RawInputLoader::FromDenoisedBayer(full-size Bayer CFA input, F32 RGBA plane of the decode
     area, model version)                                     edit/input/raw_input_loader.cpp
     -> rgb_linearization = raw_norm::BuildDenoisedRgbLinearization(cfa linearization)
     -> FinishPrepared (RGB geometry), source_key.denoise_model_hash
  -> GraphCompiler::Compile -> CompileDevelopPasses(DenoisedRgb)
     -> UploadRgb, HighlightRecover, InverseCamMulPack, Lens, GeometryResample, CameraToAp1
  -> PlanExecutor::Execute (include/edit/runtime/plan_executor.hpp)
     -> RequireDevelopInputForAiDenoise(Develop params, plan.source)
     -> BindOrMiss(sensor key: switch + denoise_model_hash) -> miss
     -> PassEncoder<UploadRgb> -> Execute{Cuda,OpenCl,Metal}Develop (RGB upload branch)
        -> upload -> linearize_rgb(DevelopRgbLinearization(input, highlights_reconstruct))
        -> highlight recovery with cam_mul (on) -> inverse white balance, crop, orientation
        -> develop.sensor_linear
```

**Primary failure call chain:**

```text
ai_denoise on, full-size input that is not denoised (DN5: every full-size render with the switch)
  -> RequireDevelopInputForAiDenoise throws std::runtime_error before the sensor lookup
     ("AI Denoise is on and the prepared input is not denoised"; X-Trans and direct RGB:
     "AI Denoise supports Bayer sensors only")
  -> Execute cancels the render and rethrows -> the render error; no Develop pass runs
denoised input with the switch off -> the same check, "AI Denoise is off and the prepared input
  is denoised"
JSON ai_denoise malformed, or in a raster document
  -> DevelopParamsModel::LoadJson throws before Mutate -> the Model is unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `AiDenoiseKeyIsAbsentWhileTheSwitchIsOff` (a root written before DN5; on changes the root id and round-trips; off gives the bytes and the root id again) | `PipelineDocumentCheckpointTest` | PASS |
| `AiDenoiseJsonRoundTripsTheModelVersion` | `GpuDagModelGraphTest` | PASS |
| `AiDenoiseJsonRejectsAMalformedValueAndKeepsTheModel` (7 malformed values and the raster key) | `GpuDagModelGraphTest` | PASS |
| `AiDenoiseChangeMarksTheSensorGroupDirty` (also `ReplaceParams`, and the invalid version) | `GpuDagModelGraphTest` | PASS |
| `AiDenoiseIsRejectedForARasterDocument` (added) | `GpuDagModelGraphTest` | PASS |
| `AiDenoiseChangeInvalidatesTheSensorResult` | `GpuDagRawInputTest` | PASS |
| `DenoisedInputTakesTheCfaMetadataAndTheDecodeArea` (margins, DNG default crop, orientation 6, a file with the white balance applied) | `GpuDagRawInputTest` | PASS |
| `DenoisedInputRejectsAReducedOrNonBayerInputAndAWrongPlane` | `GpuDagRawInputTest` | PASS |
| `DenoisedInputCompilesTheRgbUploadWithHighlightRecovery` | `GpuDagRawInputTest` | PASS |
| `SensorKeyHoldsTheSwitchAndTheModelVersion` | `GpuDagRawInputTest` | PASS |
| `InputCheckRejectsAFullSizeInputThatIsNotDenoised` | `GpuDagRawInputTest` | PASS |
| `DenoisedPlaneBecomesTheSensorImageWithTheCfaClamp` | `GpuDagCudaDevelopTest`, `GpuDagOpenClDevelopTest` | PASS (CUDA after the fix of #1046) |
| `FullSizeRenderWithTheSwitchOnAndACfaInputReportsTheError` | `GpuDagCudaDevelopTest`, `GpuDagOpenClDevelopTest` | PASS |
| `DenoisedExpectedCropKeepsTheLevelsAndLowersTheNoiseOfTheCfaRender` | `GpuDagCudaDevelopTest`, `GpuDagOpenClDevelopTest` | PASS |
| The three tests of the backend (Metal) | `GpuDagMetalDevelopTest` | not built (DN10) |
| The decoded RGB tests on the changed `linearize_rgb` (`UnpackedRgbLevels...`, `RgbDngWarp...`) | CUDA and OpenCL Develop tests | PASS |

Measured on `rawnind_crop_512` (ISO 6400, RTX 3080 Laptop GPU), ratio of the channel means of
the denoised render to the CFA render, and the mean absolute Laplacian of G:

| Backend | R | G | B | Laplacian CFA | Laplacian denoised |
| --- | ---: | ---: | ---: | ---: | ---: |
| CUDA | 0.9971 | 1.0003 | 1.0141 | 0.01212 | 0.00149 |
| OpenCL | 1.0059 | 1.0068 | 1.0215 | 0.01361 | 0.00149 |

The CUDA ratios are the ratios of the lab gain match against the mosaic means (0.997, 1.000,
1.015); the OpenCL CFA render has lower channel means than the CUDA CFA render (its RCD and
highlight passes), so its ratios are up to 0.7% higher. The noise is 8 to 9 times lower.

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target GpuDagModelGraphTest GpuDagRawInputTest PipelineDocumentCheckpointTest GpuDagCudaDevelopTest GpuDagOpenClDevelopTest"
$env:ALCEDO_AI_DENOISE_TEST_ASSETS = "<folder with the DN0 expected outputs>"
ctest --test-dir build/debug -R "AiDenoise|Denoised|DenoiseChange|FullSizeRenderWithTheSwitchOn|UnpackedRgbLevels|RgbDngWarp" -j 1   # 21/21
ctest --test-dir build/debug -R "^(GpuDagModelGraphTest|GpuDagRawInputTest|PipelineDocumentCheckpointTest|GpuDagCudaDevelopTest|GpuDagOpenClDevelopTest)\." -j 1   # 564/569
ctest --test-dir build/debug -R "^(GpuDagCudaDrtProductTest|GpuDagCudaPrimaryGradeTest)\." -j 1   # 219/230
```

Suite totals: 564 of 569 in the five touched suites. The 5 failures exist without DN5:
`ExistingRawRoot...`, `ExistingLmtRoot...`, `ExistingFilmGrainRoot...` (issue #553), and
`DcpProfileMatchesScalarReference` on CUDA and OpenCL (issue #972; the same value 0.004808 as
on the Intel Iris Xe, on a CFA path with the highlight recovery off). The CUDA grade suites (for
the fix of #1046): 219 of 230; the 11 failures are the multi-grade, scene work pair, neighbor
detail, and LUT timing tests of issues #905 and #690, which render direct RGB inputs without RGB
linearization and do not run the highlight kernels. Full suite: not run (AGENTS.md).

**Changes against the plan text and detail choices:**

- **CUDA highlight defect (issue #1046).** With the highlight recovery on and no clipped pixel,
  the CUDA pack kernels clamped each white-balanced channel to 1 (the CFA path too); OpenCL and
  Metal did not. The kernels now clamp below at 0 only (`SanitizeAndPackRGBA*Kernel`), as the
  reconstruct kernels do. Separate commit.
- `IsAiDenoiseModelVersion` is an inline function in
  `include/edit/graph/ai_denoise_model_version.hpp`, and the denoised linearization is
  `raw_norm::BuildDenoisedRgbLinearization` beside `BuildRgbLinearization`, so `EditInput` gets
  no link dependency on `EditGraph`. `develop_ai_denoise.hpp` holds `DevelopRgbLinearization` and
  `RequireDevelopInputForAiDenoise`.
- `HashAiDenoiseModelVersion` (FNV-1a of the version text, never 0) is in
  `prepared_raw_input.hpp`. `PreparedSourceKey::Lookup` does not hold the model hash: the lookup
  of the denoised cache entry is DN6.
- The input check runs before the sensor result lookup, so a published sensor result never
  hides a wrong input.
- The pixel test renders orientation 0; orientation 6 is in the extents of
  `DenoisedInputTakesTheCfaMetadataAndTheDecodeArea` (the pack kernels are the RGB pack of the
  direct RGB input).
- The level tolerance of the DN0 crop test is 3% (plan text: 2%): the lab gain match leaves the
  blue mean 1.5% above the mosaic mean, and the OpenCL CFA render adds 0.7%.
- `cuda_white_balance.cu` was converted from CRLF to LF in a separate commit.
- `AiDenoiseIsRejectedForARasterDocument` was added.

**Checklist / exit condition:** both items checked.

**LOC note (grill-code-review):** 37 files. Production 385 added and 36 removed lines (the
#1046 fix included); tests 778 added (`denoised_rgb_test_support.hpp` 304,
`develop_ai_denoise_input_test.cpp` 242, `develop_ai_denoise_test.cpp` 136); this plan section
and record. Largest touched production files: `raw_input_loader.cpp` 778 lines,
`develop_node_model.cpp` 667. No file is near 1000 lines.

**Remaining gaps:** the Metal build and its three tests (DN10). The editor parameter field of
`ai_denoise` (`SetParameter`, the history replay of the owned key, the paste of the RAW Decode
item) is DN7. Until DN6, every full-size render of a document with the switch on reports the
input error (the switch has no control before DN7).

### DN6: model run in the loader

Written on 2026-11-22, before the implementation.

**Objective and deliverables**

A full-size render (editor, export) of a Bayer photo with `ai_denoise` on runs the model on the
mosaic and renders the denoised input of DN5. The deliverables: the input preparation of the lab
specification (version 1) and the gain match, the process-wide owner of the platform session
`DenoiseModelSession`, the denoised entry of the prepared source cache, the release of the model
session with the editor GPU release and at the editor close, the error kinds of 2.4, and the
resolution of issue #1043 (a stop condition of DN6, section 8).

**Inputs and prerequisites**

DN3 (tile loop, slow-device check), DN5 (`FromDenoisedBayer`, the input check). Lab
`docs/alcedo_integration.md`: "Input preparation (version 1)", "Output and gain match".

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `include/edit/input/ai_denoise_input.hpp`, `edit/input/ai_denoise_input.cpp` (new) | `PackBayerForAiDenoise(cfa input)`: the packed RGGB mosaic of the decode area (phase and odd sizes by reflection), the input means of the gain match. `AiDenoiseGainMatch`: the column sums of the camera RGB to linear Rec.2020 matrix, the output sums of a tile, the factor. `AssembleDenoisedPlane`: the F32 RGBA plane of the decode area in the DN5 value domain |
| `include/edit/input/ai_denoise_model_session.hpp`, `edit/input/ai_denoise_model_session.cpp` (new) | `DenoiseModelSession`: the installed model version, the platform session (created at the first run, released on request), one run at a time, the slow-device result of each device, `AiDenoiseError` with its kind |
| `include/edit/input/ai_denoise_platform.hpp`, `edit/input/ai_denoise_directml_platform.cpp` (new, Windows) | `AiDenoisePlatform`: the device key (the adapter LUID) and the creation of a platform session; `UtNet2DirectMlSession` on Windows |
| `include/edit/input/prepared_source_cache.hpp`, `edit/input/prepared_source_cache.cpp` | `AcquireDenoised`: the lookup of the denoised entry (source key, model version, preparation version), on a miss the CFA entry and the run |
| `include/edit/runtime/renderer.hpp`, `include/edit/runtime/detail/renderer.inl.hpp`, `edit/pipeline/pipeline_executor.cpp` | The denoised input for a full-size render with the switch on; `PipelineExecutor::ReleaseAiDenoiseSession` |
| `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp`, the application start | The release with the editor GPU release and at the editor close; the model folder of the session |
| `decoders/processor/nn/utnet2_tile_loop.cpp`, `utnet2_directml_session.cpp` | Issue #1043: the tile size from the local memory that all processes use; the slow-device check from the fastest of the first measured tiles |

**Data rules**

- **Where the model runs.** Only in the renderer, for `DecodeRes::FULL` with `ai_denoise` on
  and a Bayer CFA input. An X-Trans, direct RGB, or raster input keeps its CFA or RGB input, and
  the DN5 input check reports "AI Denoise supports Bayer sensors only". A reduced decode
  (thumbnail, semantic input image) renders the CFA path (D12).
- **Input preparation (lab version 1).** The area is the decode area of DN5
  (`BuildDecodeCropRect`). The value of a sample is `(raw - black) / (maximum - black_c)`:
  `black` is `black_level[c]` plus the pattern black of the position (the CFA Linearize step), and
  `maximum` is LibRaw `color.maximum` (a new field of `RawLinearizationParams`, filled at the
  unpack). No clip. A GBRG or BGGR area gets one row at the top, a GRBG or BGGR area one column
  at the left, and an odd size one row or column at the bottom or the right, each by reflection
  without the edge pixel; then the mosaic starts with R and is packed to 4 planes (R, G1, G2, B).
- **Gain match.** `g = sum_c w_c mean_in(c) / sum_c w_c mean_out(c)`: `w_c` the column sums of
  `XYZ_to_Rec2020 * inverse(xyz_to_camera)`, with `xyz_to_camera` the camera matrix of the render
  at D65 (`ResolveDevelopXyzToCameraD65`: the effective camera profile of the Develop node, its
  ColorMatrix at 6504 K with the AnalogBalance and CameraCalibration of a DNG profile; #1055),
  `mean_in(c)` the mean of the prepared samples of color `c` in the area (G: both
  positions), `mean_out(c)` the mean of output channel `c` in the area. The tile callback adds
  the output sums of the kept rectangle of each tile inside the area (double precision).
- **Denoised plane.** Channel `c` of the area: `g * out_c * (maximum - black_level[c]) /
  (white_level[c] - black_level[c])` (green: entry 1), alpha 1: the value domain of DN5.
- **Cache.** The denoised entry has its own lookup: the CFA lookup plus the hash of the
  installed model version. A miss takes the CFA entry (a hit or an unpack), runs the model,
  releases the CFA lease, and inserts the denoised entry; the CFA entry is then the first
  candidate of the eviction. The host budget (512 MiB) applies; the denoised entry of 45.4 MP
  is 726 MiB (F32 RGBA) and a leased entry is never evicted. A batch render (export) runs the
  model at each render, as it unpacks at each render.
- **Model version.** The installed version is the SHA-256 of the graph file at
  `<model folder>/ai-denoise-v1/utnet2_bayer_dark_loss_10k_fp16.onnx`, read again when the size
  or the time of the file changes. The model folder comes from the application
  (`semantic/modelDirectory`). No file: `AiDenoiseError` of the kind `ModelNotInstalled`.
- **Session.** One `DenoiseModelSession` in the process (`ProcessDenoiseModelSession()`); the
  executors use it unless a test gives its own. A mutex serializes the runs of the editor and
  the export. The platform session is created at the first run and kept until `Release`. A run
  error releases the platform session (a removed DirectML device is not used again), and the
  next run creates it again.
- **Slow device.** A `DenoiseDeviceTooSlowError` is kept for the device key until the process
  ends, also after `Release`; a later run on that device throws at once and creates no session.
- **Release.** The editor GPU release (export, semantic generation) and the editor close call
  `PipelineExecutor::ReleaseAiDenoiseSession` after `ReleaseBinding`, on the editor worker. The
  end of an editor GPU release also releases it (the export ran its renders). A binding change
  (another photo) keeps the session.
- **Error kinds.** `AiDenoiseError` (a `std::runtime_error`) has `ModelNotInstalled`,
  `DeviceTooSlow`, and `RunFailed` (ONNX Runtime, DirectML, Direct3D 12, the video memory, a value
  that is not finite, no platform session on this platform). The renderer passes it through
  unchanged, so that DN7 and DN8 select the text and the controls.
- **Issue #1043.** The tile size uses the smaller of the budget of the process and the local
  memory of the adapter that no process uses (the performance counter
  `GPU Adapter Memory\Dedicated Usage`; not on a UMA adapter). The slow-device check projects from
  the fastest of the measured tiles, from the third measured tile on and after the last tile.

**Implementation steps**

1. Issue #1043 in the tile loop and the DirectML session; the measurement under load.
2. The input preparation, the gain match, the plane assembly, and their tests.
3. `DenoiseModelSession`, the DirectML platform, and their tests with a test platform.
4. `AcquireDenoised`, the renderer, the executor release, and their tests.
5. The release in the editor port and the model folder at the application start.
6. The real run: a RAW file through the CUDA and OpenCL renderer with the FP16 graph.

**Primary success call chain**

```text
Renderer::RenderImage (DecodeRes::FULL, ai_denoise on)
  -> PreparedSourceCache::AcquireDenoised(encoded, installed version) -> miss
     -> AcquireEncoded (CFA entry) -> DenoiseModelSession::Denoise(cfa input)
        -> PackBayerForAiDenoise -> DenoiseBayerMosaic(platform session) -> output sums per tile
        -> gain match factor -> AssembleDenoisedPlane -> RawInputLoader::FromDenoisedBayer
     -> insert the denoised entry
  -> CompileDevelopPasses(DenoisedRgb) -> the Develop passes of DN5
```

**Primary failure call chain**

```text
no graph file -> AiDenoiseError(ModelNotInstalled) before a lookup -> the render error
slow device -> DenoiseDeviceTooSlowError -> kept for the device -> AiDenoiseError(DeviceTooSlow)
run error -> the platform session is released -> AiDenoiseError(RunFailed) -> the render error;
  no cache entry
```

**Tests**

| Test | Assertion |
| --- | --- |
| `PackedMosaicStartsWithRedForEachBayerPhase` | For RGGB, GBRG, GRBG, BGGR and odd sizes: each packed value is the prepared value of the reflected mosaic position, the plane 0 sample is R |
| `PreparedValuesUseTheBlackOfEachColorAndTheLibRawMaximum` | Per-color black, pattern black, maximum; values below black and above maximum stay unclipped; the input means |
| `GainMatchFactorEqualsTheLabValueOnTheExpectedCrop` (DN0 assets) | `rawnind_crop_512` with its `rgb_xyz_matrix`: the factor equals the lab `channel_sums` value within 1e-6 relative |
| `DenoisedPlaneIsTheGainMatchedOutputInTheWhiteLevelDomain` | The area crop, the factor, the scale to the white level of each channel, alpha 1 |
| `ModelSessionRunsOneDenoiseAtATime` | Two threads: no overlap of runs in the test platform |
| `ModelSessionCreatesThePlatformSessionOnceAndAgainAfterRelease` | Session count after runs, `Release`, and the next run |
| `SlowDeviceResultIsKeptForTheDeviceAfterRelease` | After the slow-device error and `Release`, a run on the device throws `DeviceTooSlow` at once and creates no session; another device runs |
| `RunErrorReleasesThePlatformSessionAndReportsRunFailed` | Kind, session released, next run creates one |
| `MissingGraphReportsModelNotInstalled` | Kind, no session |
| `InstalledModelVersionIsTheSha256OfTheGraphFile` | Version text; a changed file gives a new version |
| `DenoisedEntryIsReusedAndRunsTheModelOnce` | Two acquires: one run; another model version: a second run; the CFA entry is evicted first |
| `NonBayerInputKeepsItsCfaEntryWithoutARun` | X-Trans or direct RGB: no run, the input of the CFA entry |
| `FullSizeRenderWithTheSwitchOnRendersTheModelOutput` (CUDA, OpenCL) | A test platform with a known output: `develop.sensor_linear` is that output; a second render runs no model; a reduced render runs no model |
| `ModelErrorIsTheRenderErrorWithItsKind` (CUDA, OpenCL) | `AiDenoiseError` of each kind passes out of `Render` |
| `RawPhotoRendersWithTheFp16GraphOnDirectMl` (CUDA, OpenCL; assets) | A RAW file of the test assets: finite sensor result, channel means within 3% of the CFA render, lower noise |

**Exit criteria**

- [x] The tests pass in `build/debug` on CUDA and OpenCL.
- [x] Issue #1043: no slow-device error on the RTX 3080 Laptop GPU with another process that
      holds video memory; the Intel Iris Xe keeps the error. The times are in the record.
- [x] The touched suites (`UtNet2TileLoopTest`, `UtNet2DirectMlTest`, `GpuDagRawInputTest`, the
      CUDA and OpenCL Develop tests) have no new failure.

**Expected diff**: 800 to 1300 lines.

##### Phase DN6 completion record (2026-11-22)

**Status:** complete. A full-size render of a Bayer photo with `ai_denoise` on runs the FP16
graph on DirectML in the loader and renders the denoised input of DN5, on CUDA and OpenCL. The
input preparation, the gain match, `DenoiseModelSession`, the denoised cache entry, the release
with the editor GPU release and at the editor close, the error kinds, and issue #1043. Issue
#1048.

**Primary success call chain:**

```text
Renderer::RenderImage (DecodeRes::FULL, ai_denoise on)      include/edit/runtime/detail/renderer.inl.hpp
  -> DenoiseModelSession::InstalledModelVersion (SHA-256 of the graph file, kept by size and time)
  -> detail::AiDenoiseXyzToCamera -> ResolveDevelopXyzToCameraD65 (the camera matrix of the
     render at D65; no matrix: the render error of the camera colour pass)
  -> PreparedSourceCache::AcquireDenoised(encoded, version, camera) -> miss (key: HashAiDenoiseRun)
     -> AcquireEncoded(FULL): the CFA entry (hit or LibRaw unpack); not Bayer CFA -> that lease
     -> DenoiseModelSession::Denoise(cfa, version, camera) (run mutex) edit/input/ai_denoise_model_session.cpp
        -> AiDenoisePlatform::DeviceKey (DirectMlAdapterKey: description and LUID); slow-device map
        -> PackBayerForAiDenoise (decode area, phase rows and columns by reflection, input means)
        -> AiDenoiseGainWeights(camera) -> CreateSession on the first run (UtNet2DirectMlSession)
        -> nn::DenoiseBayerMosaic (AvailableTileMemory: process budget and adapter memory of all
           processes) -> each tile: Run -> AiDenoiseOutputSums::AddKept
        -> AiDenoiseGainFactor -> AssembleDenoisedPlane -> RawInputLoader::FromDenoisedBayer
     -> the CFA lease ends -> Insert (eviction takes the unleased CFA entry first)
  -> plan compile (DenoisedRgb) -> RequireDevelopInputForAiDenoise -> the Develop passes of DN5
Batch render (export): unpack -> Denoise at each render (no cache, as the unpack)
Editor close, editor GPU release start and end (EditorSessionRenderSchedulerPort, editor worker)
  -> PipelineExecutor::ReleaseBinding -> PipelineExecutor::ReleaseAiDenoiseSession
  -> DenoiseModelSession::Release (waits for a run; the slow-device map stays)
```

**Primary failure call chain:**

```text
no model folder or no graph file -> InstalledModelVersion throws AiDenoiseError(ModelNotInstalled)
  before the cache lookup -> the render error; no unpack, no cache entry
slow device -> DenoiseDeviceTooSlowError -> kept for the device key, the session released
  -> AiDenoiseError(DeviceTooSlow); a later run on that device throws at once, no session
ONNX Runtime, DirectML, Direct3D 12, video memory, a value that is not finite, no platform
  -> the platform session released (a removed device is not used again)
  -> AiDenoiseError(RunFailed) -> the render error; no denoised entry; the next frame runs again
X-Trans, direct RGB, raster with the switch on -> the CFA or RGB entry, no run
  -> RequireDevelopInputForAiDenoise: "AI Denoise supports Bayer sensors only"
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `PackedMosaicStartsWithRedForEachBayerPhase` (4 patterns x 3 sizes, each packed value and its color) | `AiDenoiseLoaderTest` | PASS |
| `PreparedValuesUseTheBlackOfEachColorAndTheLibRawMaximum` (margins with an odd top margin, pattern black, unclipped values, the means) | `AiDenoiseLoaderTest` | PASS |
| `PreparationRejectsAnInputThatIsNotAFullSizeBayerCfa` (added) | `AiDenoiseLoaderTest` | PASS |
| `OutputSumsAddOnlyTheOutputOfTheArea` (added) | `AiDenoiseLoaderTest` | PASS |
| `DenoisedPlaneIsTheGainMatchedOutputInTheWhiteLevelDomain` | `AiDenoiseLoaderTest` | PASS |
| `GainMatchFactorEqualsTheLabValueOnTheExpectedCrop` (weights within 1e-5, factor within 1e-6 of the lab `channel_sums` 9.6937e-6) | `AiDenoiseLoaderTest` | PASS |
| `PackedMosaicEqualsTheLabPreparationOfARawFile` (added: `_DSC0726.ARW` against the lab `prepare_packed_rggb`; largest difference at most 1e-7, means within 1e-6) | `AiDenoiseLoaderTest` | PASS |
| `InstalledModelVersionIsTheSha256OfTheGraphFile` | `AiDenoiseLoaderTest` | PASS |
| `MissingGraphReportsModelNotInstalled` | `AiDenoiseLoaderTest` | PASS |
| `DenoiseGivesTheGainMatchedInputOfTheDecodeArea` (added: two tiles; the plane equals the one-pass plane within 1e-6) | `AiDenoiseLoaderTest` | PASS |
| `ModelSessionCreatesThePlatformSessionOnceAndAgainAfterRelease` (also a new installed version) | `AiDenoiseLoaderTest` | PASS |
| `ModelSessionRunsOneDenoiseAtATime` (3 threads, at most 1 tile run at a time) | `AiDenoiseLoaderTest` | PASS |
| `SlowDeviceResultIsKeptForTheDeviceAfterRelease` | `AiDenoiseLoaderTest` | PASS |
| `RunErrorReleasesThePlatformSessionAndReportsRunFailed` | `AiDenoiseLoaderTest` | PASS |
| `PlatformWithoutForwardCodeReportsRunFailed` (added) | `AiDenoiseLoaderTest` | PASS |
| `DenoisedEntryIsReusedAndRunsTheModelOnce` | `AiDenoiseLoaderTest` | PASS |
| `NonBayerInputKeepsItsEntryWithoutARun` | `AiDenoiseLoaderTest` | PASS |
| `FullSizeRenderWithTheSwitchOnRendersTheModelOutput` (one run for two frames, the cached entry, a reduced frame without a run, the switch off, the batch run at each render) | `AiDenoiseRenderTest` (CUDA, OpenCL) | PASS |
| `ModelErrorIsTheRenderErrorWithItsKind` (each kind out of `Render`; no entry after a run error) | `AiDenoiseRenderTest` (CUDA, OpenCL) | PASS |
| `RawPhotoRendersWithTheFp16GraphOnDirectMl` (`_DSC0726.ARW`, the product platform; display means of a 512 px crop at 100% within 5% of the CFA render, Laplacian lower; release and a new session) | `AiDenoiseRenderTest` (CUDA, OpenCL) | PASS |
| `TileSizeSelectionCountsTheVideoMemoryOfOtherProcesses` (added, #1043) | `UtNet2TileLoopTest` | PASS |
| `SlowDeviceCheckUsesTheFastestMeasuredTile` (added, #1043) | `UtNet2TileLoopTest` | PASS |
| The DN3 tests with the changed check (the stops after 3 measured tiles) | `UtNet2TileLoopTest`, `UtNet2DirectMlTest` | PASS |

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target alcedo_main AiDenoiseLoaderTest AiDenoiseRenderTest UtNet2TileLoopTest UtNet2DirectMlTest GpuDagRawInputTest GpuDagCudaDevelopTest GpuDagOpenClDevelopTest GpuDagCudaDrtProductTest GpuDagOpenClDrtProductTest ExecutorIsolationTest ExecutorSnapshotRenderTest"
lab: python build/tmp/dn6/lab_pack_raw.py alcedo_studio/tests/resources/sample_images/raw/_DSC0726.ARW <assets>
$env:ALCEDO_AI_DENOISE_TEST_ASSETS = "<folder: the FP16 graph, the DN0 expected outputs, _DSC0726.packed.f32/.txt>"
ctest --test-dir build/debug -R "^(AiDenoiseLoaderTest|AiDenoiseRenderTest|UtNet2TileLoopTest|UtNet2DirectMlTest|GpuDagRawInputTest)\." -j 1   # 320/320
ctest --test-dir build/debug -R "^(...|GpuDagCudaDevelopTest|GpuDagOpenClDevelopTest|ExecutorIsolationTest|ExecutorSnapshotRenderTest)\." -j 1   # 391/393
ctest --test-dir build/debug -R "^(GpuDagCudaDrtProductTest|GpuDagOpenClDrtProductTest)\." -j 1   # 164/164
cmd /c "scripts\msvc_env.cmd --build build\release-test --target AiDenoiseRenderTest UtNet2PhotoTimingTool"
AiDenoiseRenderTest --gtest_filter=*RawPhoto*   # the times below; UtNet2PhotoTimingTool for #1043
```

Suite totals: 17/17 `AiDenoiseLoaderTest`, 6/6 `AiDenoiseRenderTest`, 9/9 `UtNet2TileLoopTest`, 9/9
`UtNet2DirectMlTest`, 279/279 `GpuDagRawInputTest`; CUDA and OpenCL Develop and executor suites
391/393: the 2 failures are `DcpProfileMatchesScalarReference` on CUDA and OpenCL (issue #972,
without DN6 too); DRT product suites 164/164. Full suite: not run (AGENTS.md).

**Times** (release build `build/release-test`, RTX 3080 Laptop GPU, `_DSC0726.ARW` 7952 x 5304 px
= 42.2 MP of the decode area, 6 tiles of 1536; other programs used 1.6 to 2.2 GiB and up to 61%
of the GPU at times):

| Run | Tile loop | `Denoise` (preparation, tiles, gain match, plane) |
| --- | ---: | ---: |
| Ready session (a cache miss of a photo of a known size), 12 runs | 0.41 to 0.47 s (one 0.67 s) | 0.56 to 0.72 s (one 0.90 s) |
| First run of a session (session creation 0.35 to 0.96 s; the first tile of each new width compiles) | 0.85 to 1.39 s | 1.36 to 2.51 s |
| First render of the photo in the editor (unpack, the run above, the Develop passes, the display) | | 2.8 to 4.2 s |

- At 45.4 MP the ready run scales to about 0.62 to 0.77 s; `target_time` is 0.72 s, its 15% limit
  0.83 s. The host part (preparation, sums, plane) is 0.15 to 0.25 s beside the tile loop.
- Debug build: the ready run is not measured; the first run takes 1.9 to 2.7 s.
- Host memory of a 45.4 MP run: the packed mosaic 182 MB, the model output 545 MB, the denoised
  plane 726 MiB (F32 RGBA) at the same time, beside the CFA entry (91 MB). The plane is the cache
  entry: it is above the host budget of 512 MiB; a leased entry stays, and the next insertion of
  another entry evicts it.

**Issue #1043** (release build, `UtNet2PhotoTimingTool`, 45.4 MP):

| Case | Before DN6 | DN6 |
| --- | --- | --- |
| Another process holds 4.6 GiB and writes it every 100 ms (6.2 to 7.0 GiB in use before the run) | tiles of 1536 forced: slow-device error in 4 of 4 runs (projected 7.9 to 35 s); the DN6 tile size alone (1024): error in 3 of 4 runs (projected 2.34 to 2.94 s, single tiles of 0.16 s after the thrash) | tiles of 1024 selected, 14 of 14 runs complete, 0.64 to 1.03 s |
| Two processes started together | (DN3: error in 6 of 7 runs) | both select 1536, 16 of 16 runs complete, 0.75 to 1.27 s |
| Intel Iris Xe (`--adapter minimum-power`) | stop after the first measured tile | stop after 3 measured tiles: 24.3 MP projected 1.92 to 2.07 s (limit 1.22 s), 45.4 MP 3.54 to 3.73 s (limit 2.27 s), 1.5 to 2.0 s to the stop on a ready session |

The cause was two parts: the DXGI budget of a process does not count the video memory of the
other processes (a selected tile of 1536 oversubscribed the local memory), and a single slow tile
of a fast device during the paging of another process stopped the run.

**Changes against the plan text and detail choices:**

- **#1043.** `DeviceMemoryState` has the local memory of the adapter (DXGI
  `DedicatedVideoMemory`) and its use by all processes (the performance counter `GPU Adapter
  Memory(luid_...)\Dedicated Usage`, PDH); `AvailableTileMemory` takes the smaller of the free
  budget and the free local memory, plus the held arena. A UMA adapter (D3D12 architecture
  `UMA`) uses the budget only. The slow-device check projects from the fastest measured tile
  from the third measured tile on (`UtNet2Spec::kSlowDeviceMeasuredTiles`), and after the last
  tile with the tiles that were measured. Sections 2.4 and 9 changed.
- **The decode area of a Sony file is its default crop** (7952 x 5304 inside the visible
  7968 x 5320 of LibRaw): the model input is the area that Alcedo develops, so it differs from the
  lab packing by the 8 px border. The comparison test uses the common area.
- **`RawLinearizationParams::libraw_maximum`** holds LibRaw `color.maximum` (not hashed: the
  source key holds the file).
- **The gain match uses the camera matrix of the render (#1055, 2026-11-22).** DN6 read LibRaw
  `color.cam_xyz`. LibRaw fills that field only from its own Adobe table: for a DNG it reads the
  ColorMatrix into a local array for `rgb_cam` only, so the field is zero for DNG files of models
  outside the table (phones, DJI, Leica, Sigma) and for cameras newer than the table (GFX100RF,
  Lumix L10, Z5II); 27 Bayer files of the sample set had a zero matrix and the run stopped with
  "the camera matrix is singular". The run now takes `ResolveDevelopXyzToCameraD65` of the Develop
  node, the matrix that the render uses, and fails only where the render fails. The matrix is in
  the key of the denoised entry (`PreparedSourceKey::denoise_hash` = `HashAiDenoiseRun`): another
  camera profile runs the model again, a white balance change does not. For the ILCE-7RM3 of the
  lab comparison the ColorMatrix2 of `camera_matrices.hpp` is not the matrix of the LibRaw table,
  so the weights differ from the lab weights (R 3.78 / 2.69, G 1.19 / 1.21, B 0.48 / 0.83); the
  DirectML run on that file keeps the channel means of the CFA render within 2.5%.
- **Pattern black:** the prepared value subtracts the pattern black of the position, as the CFA
  Linearize step; the denominator uses `maximum - black_level[c]`. For a file without a pattern
  black this is the lab preparation.
- **The installed version is read before the lookup,** so a non-Bayer photo with the switch on
  and no model reports `ModelNotInstalled`, not the sensor error (the switch is disabled for such a
  photo in DN7; only a paste can set it).
- **Release:** also at the end of an editor GPU release (the export ran its own runs), and at the
  shutdown of the application module host. A binding change keeps the session.
- **No cancel check** in the product run: one ready run takes about 0.6 s; the frame that needs
  the result waits for it.
- **The cache counts** a denoised miss in `misses` and the CFA lookup again in its own counter;
  `Stats::denoise_runs` counts the model runs.
- **Link:** the DirectML session names d3d12, dxgi, pdh, and delayimp with `#pragma comment(lib)`,
  and the delay load of onnxruntime.dll is `$<HOST_LINK:...>`: `EditPipeline` (a CUDA library)
  passes the link line of EditInput to the device link of nvcc, which cannot open a library by its
  bare name.
- **Process instance:** `ProcessDenoiseModelSession()` is called only by `PipelineExecutor` (and the
  application start and shutdown), all in the executable; renderers get the session from their
  executor, so a copy of EditInput in a DLL does not make a second instance.
- The timing tool prints the adapter memory of all processes and the non-local memory of the
  process.

**Checklist / exit condition:** all three items checked.

**LOC note (grill-code-review):** production: 7 new files 849 lines (`ai_denoise_input.cpp` 290,
`ai_denoise_model_session.cpp` 196, headers 317), about 300 changed lines in 17 files; tests 1370
lines (4 new files, the tile loop test); the plan section and this record. Larger than the
estimate of 800 to 1300 with the tests. The two editor files that this phase touches with 10 lines
were above 1000 lines before it (`editor_session_render_scheduler_port.cpp` 1149,
`application_module_host.cpp` 1026); no new file is above 300 lines.

**Remaining gaps:** a real device removal of DirectML (887A0005) is not reproduced: a run error
releases the session and the next run creates it, but whether the editor backend continues when
the removal resets the adapter that it shares (a laptop with only the Intel Iris Xe) is not
measured. The Metal and macOS paths (DN10). The control and the texts of the error kinds (DN7,
DN8).

##### Phase DN6 addition (2026-11-22): input exposure curve

**Reason (user test of DN7, issue #1053):** an underexposed ISO 800 photo got less noise reduction than a
normally exposed ISO 3200 photo. The RawNIND training pairs keep the scene brightness at each
ISO (the noisy input against its clean image: median +1.02 EV, 5% to 95% 0 to +1.08 EV, the
same at ISO 100 and ISO 65535; 3404 noisy pairs of the lab training data), and the training
gives the input no gain (`match_gain: output`). So the model sees the noise of each ISO only at
the brightness of a normal exposure, and the input preparation of DN6 gave a dark photo to the
model at its own brightness. The user decided on 2026-11-22 to raise the exposure in Alcedo
Studio directly, before a lab measurement.

**Design.** `DenoiseModelSession::Denoise` applies a curve to the packed mosaic after
`PackBayerForAiDenoise` and before the tile loop; the gain match runs in the domain of the curve
(the input means after the curve); `AssembleDenoisedPlane` applies the inverse curve before the
white level scale. So the result keeps the exposure of the photo, and the model sees a dark
photo at the training brightness.

| Item | Value | Source |
| --- | --- | --- |
| Level of a photo | Mean of the R, G, and B means of the prepared samples of the decode area | The statistic of the lab record (`camera_rgb_mean_mosaic`) |
| `kAiDenoiseTargetInputLevel` | 0.028 of the white level: the median level of the 3404 noisy training inputs (clean level / `raw_gain`; 5% to 95%: 0.0056 to 0.072) | Lab `data/patches/rawnind_bayer_demosaicnet` |
| Gain | target / level, from 1 to `kAiDenoiseMaxInputGain`; a photo at or above the target keeps the identity | This addition |
| `kAiDenoiseMaxInputGain` | 16 (4 EV) | FP16 check below |
| Curve below the knee | `y = gain x` up to `y = kAiDenoiseCurveKnee` = 0.5, also for values below 0 | This addition |
| Curve above the knee | `y = 0.5 + s ln(1 + gain (x - 0.5 / gain) / s)`, the shoulder width `s` by bisection so that `x = 1` gives `y = 1`; above `x = 1` a line with the slope of the shoulder there | Keeps the highlights at or below the white level, the range of the training inputs |
| Time | One pass over the packed mosaic (curve and means) and the inverse in the existing assembly pass; the model time does not change | |

**FP16 check** (lab venv, ONNX Runtime 1.24.4 DirectML, the FP16 graph of DN2, non-overlapping
crops of 1024 packed px, `tests/resources/sample_images/raw` folders `night`, `lowlight`,
`high_iso`): 20 photos read; 14 at or above the target keep the identity. With the curve:
`lowlight/DSC_0604.NEF` level 0.0030, gain 9.37 (3.23 EV), largest output 628 (493 without the
curve); `high_iso/_DSC2546.NEF` gain 2.07, 144 (89); four night photos gain 1.05 to 1.51, 742
to 836 (760 to 841). No value is non-finite; the largest output is 1% of the float16 limit.

**Check by eye** (crop of 512 packed px, gray world balance, the green mean to 18%):
`DSC_0604.NEF` with the curve has visibly less noise in the flat gray areas than without it,
with the same edges; `_DSC2546.NEF` (2.07x) shows a small difference; the clipped light of
`night/2136729222.rw2` has the same edge and colors with and without the curve.

**Tests** (`AiDenoiseLoaderTest`): the curve (the gain up to the knee, `x = 1` stays at 1, a
continuous slope at the knee and at the white level, rising, inverse); the gain rule (identity
at and above the target, the gain below it, the largest gain for a black frame and a level
below 0); the curve on the planes and the means of the area only; the inverse in
`AssembleDenoisedPlane` across the line, the shoulder, and the line above 1; a dark photo
through `DenoiseModelSession` with the test session (its output is its input) returns the
prepared input of each cell (largest relative difference at most 1e-5, gain 5.04) and
`Stats().last_input_gain`.

**Call chain:**

```text
DenoiseModelSession::Denoise
  -> PackBayerForAiDenoise (prepared values, input means)
  -> AiDenoiseInputCurveFor(input means) -> ApplyAiDenoiseInputCurve (planes, means after the curve)
  -> DenoiseBayerMosaic -> AiDenoiseGainFactor (means after the curve)
  -> AssembleDenoisedPlane (inverse curve, then the white level scale) -> FromDenoisedBayer
```

**Remaining gaps:** the user checks the effect on the underexposed photos of the user in the
editor. The lab measurement of the quality against the brightness of the input (RawNIND test
split) is not run.

### DN7: editor switch and render errors

Written on 2026-11-22, before the implementation. The user approved the panel design of the mock
on 2026-11-22 and removed the model version notice (D10, 2.1).

**Objective and deliverables**

The RAW Decode card of the editor gets the `AI Denoise` switch, and the viewport shows each
model error of a render with its controls. The deliverables: the `ai_denoise` key in the
`raw_decode` field (write, history, undo, paste), the switch and the locked Method track, the
labels `Auto` and `Neural`, the Bayer check of the open photo, the platform check, the typed
render error from the render to the viewport, the error card with `Manage` and
`Turn Off AI Denoise`, and the entry of a photo whose first frame fails with a model error.

**Inputs and prerequisites**

DN6 (`DenoiseModelSession`, `AiDenoiseError`). The approved mock of 2026-11-22.

**Product rules** (2.1 is the specification; these are the details of the implementation)

- The switch row is the first row of the RAW Decode card, above `Method`. It is hidden when the
  platform has no forward code (D13): no platform (macOS, Linux until DN10 and DN11), no hardware
  adapter with Direct3D 12, or `onnxruntime.dll` or `DirectML.dll` does not load. The check runs
  once in the process.
- The Method labels are `Auto` (value `default`), `Legacy`, and `Neural` (value
  `neural_engine`). The stored values do not change. Reason: on macOS the model runs on the Apple
  Neural Engine, while the Neural demosaic runs on the GPU.
- While the switch is on, the Method track shows one selected segment `AI Denoise` that takes no
  input. The stored `demosaic_method` stays; when the switch goes off, the track shows it again.
- A photo that is not a Bayer RAW file (X-Trans, monochrome, linear DNG, four-color CFA): the
  switch is disabled; its tooltip is `Not supported for X-Trans sensors` or
  `Not supported for this sensor`. The editor reads the sensor layout of the open photo from the
  LibRaw metadata of the file on a worker thread (no unpack). A file that cannot be read keeps the
  switch enabled; the render then reports its own error.
- The switch on: the control reads the installed model version. No model: the AI Models page of
  the settings opens, and the switch stays off (no history change). A model: one `raw_decode`
  change with `ai_denoise: {"model_version": <installed>}`.
- The switch off and `Turn Off AI Denoise`: one `raw_decode` change with `ai_denoise: null`.
- The error card is in the center of the viewport while the last frame of the open photo failed
  with a model error, until a frame of that photo is ready. It shows the title of the kind and
  the text of the error:

  | Kind | Title | Controls |
  | --- | --- | --- |
  | `ModelNotInstalled` | AI Denoise model not installed | `Manage` (opens the AI Models page), `Turn Off AI Denoise` |
  | `DeviceTooSlow` | This GPU is too slow for AI Denoise | `Turn Off AI Denoise` |
  | `SensorNotSupported` | AI Denoise does not support this sensor | `Turn Off AI Denoise` |
  | `RunFailed` | AI Denoise failed | `Turn Off AI Denoise` |

- A first frame that fails with a model error opens the photo (the session enters Interactive,
  as after a first frame) so that the card and `Turn Off AI Denoise` can act on it. A first frame
  that fails with another error keeps the present behavior.
- No model version notice (the user removed it on 2026-11-22). The parameter still records the
  version (D10).

**Data rules**

- **`raw_decode` owns `ai_denoise`.** `DevelopFieldOwnedKeys("raw_decode")` gets `ai_denoise`, so
  the history, the undo, and the paste of the RAW Decode item carry it.
  `DevelopRawDecodeUpdate::ai_denoise` is `std::optional<std::optional<DevelopAiDenoise>>`: absent
  keeps the switch, an empty inner value turns it off.
- **Parse.** A write: `ai_denoise` an object (`model_version` of 64 lowercase hexadecimal digits)
  turns it on, `null` turns it off, absent keeps it. A complete value (history replay, paste):
  absent turns it off, because the Model JSON has the key only while the switch is on. A
  `raw_decode` value of an old history entry has no key and the document has none either, so
  the replay of an old project is unchanged.
- **Typed render error.** `AiDenoiseError` moves to `edit/input/ai_denoise_error.hpp` (no
  dependencies) and gets the kind `SensorNotSupported` (the Bayer check of DN5). Its `what()` is
  `AI Denoise error (<kind token>): <text>`; `Text()` is the text. `ParseAiDenoiseRenderError`
  finds the marker in a render message and returns the kind and the text. The render message
  keeps this form through the scheduler, the coordinator, and the session without a change of
  their callback signatures (the form of `kPresentationSuspendedMessage`). DN8 reads the export
  error with the same function.
- **Error state.** `EditorSessionRenderController` keeps the model error of the open photo: set by
  a failed frame of that photo whose message has the marker, cleared by a ready frame of that
  photo and by the load of another photo. A change emits `AiDenoiseErrorChanged`; the session
  service then notifies a change; `EditorSessionController` publishes `aiDenoiseError`
  (`kind`, `text`, empty without an error).
- **Switch owner.** `EditorAiDenoiseControl` (album backend, `appModules.editorAiDenoise`): the
  platform check, the sensor layout of the open photo, `turnOn()` and `turnOff()` through the
  `IEditorAdjustmentSubmitter` of the editor session, and the signal that opens the AI Models
  page. It keeps no copy of the switch state: the panel reads the switch from the panel
  projection (`raw.ai_denoise`).

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `include/edit/input/ai_denoise_error.hpp` (new, header only: the editor session reads it without `EditInput`) | `AiDenoiseErrorKind`, `AiDenoiseError`, `ParseAiDenoiseRenderError` |
| `include/edit/input/ai_denoise_model_session.hpp`, `.cpp` | Include the error header; `IsPlatformAvailable()` |
| `include/edit/input/ai_denoise_platform.hpp`, `ai_denoise_directml_platform.cpp`, `decoders/processor/nn/utnet2_directml_session.*` | `AiDenoisePlatform::CheckRuntime`; `nn::LoadDirectMlRuntime` |
| `include/edit/input/raw_sensor_layout.hpp`, `edit/input/raw_sensor_layout.cpp` (new) | `ReadRawSensorLayout(path)`: Bayer, X-Trans, or another layout from the LibRaw metadata |
| `include/edit/runtime/develop_ai_denoise.hpp` | The Bayer check throws `AiDenoiseError(SensorNotSupported)` |
| `include/edit/graph/develop_node_model.hpp`, `.cpp` | `DevelopRawDecodeUpdate::ai_denoise` |
| `app/editor_parameter_write_parse.cpp`, `app/pipeline_history_applier.cpp`, `app/editor_panel_projection.cpp`, `include/app/editor_panel_projection.hpp`, `app/editor_parameter_catalog.cpp` | Parse, owned key, projection, labels |
| `app/editor_session_render_controller.cpp`, `app/editor_session_service.cpp`, their headers, `include/app/editor_session_types.hpp` | The error state, the first frame with a model error, the change notification |
| `ui/alcedo_main/album_backend/editor_session_controller.*`, `editor_panel_presentation.cpp` | `aiDenoiseError`; `raw.ai_denoise` in the snapshot |
| `ui/alcedo_main/album_backend/editor_ai_denoise_control.*` (new), `application_module_host.cpp` | The switch owner and its services |
| `qml/EditorRawDecodePanel.qml`, `qml/EditorAiDenoiseErrorCard.qml` (new), `qml/EditorWorkspace.qml`, `qml/EditorAdjustmentStack.qml`, the `.ts` files | The switch, the locked track, the card |

**Primary success call chain**

```text
switch on (EditorRawDecodePanel) -> EditorAiDenoiseControl::turnOn
  -> DenoiseModelSession::InstalledModelVersion -> IEditorAdjustmentSubmitter::submitWrite
     ("raw_decode", DevelopRawDecodeUpdate{ai_denoise = {version}}, settled)
  -> ApplyEditorParameterWrite -> DevelopParamsModel::ApplyRawDecodeUpdate (DevelopDirty::Denoise)
  -> one history commit -> render with the model (DN6) -> panel projection raw.ai_denoise
Turn Off AI Denoise (card) -> EditorAiDenoiseControl::turnOff -> the same write with ai_denoise
  off -> render without the model -> FrameReady -> the error state clears -> the card hides
```

**Primary failure call chain**

```text
render -> AiDenoiseError (what(): marker, kind, text) -> PipelineScheduler TaskOutcome message
  -> EditorRenderCoordinator Failed -> EditorSessionRenderController::NotifyRenderResult
  -> ParseAiDenoiseRenderError -> error state set -> AiDenoiseErrorChanged -> NotifyChange
  -> EditorSessionController::aiDenoiseError -> EditorAiDenoiseErrorCard
first frame (Loading) with the marker -> the photo opens (Interactive) with the error state
switch on without a model -> aiModelsPageRequested -> Settings, AI Models; no write
```

**Implementation steps**

1. The error header, the kind `SensorNotSupported`, the parse function, the platform check.
2. The `ai_denoise` key of `raw_decode`: update, parse, owned key, projection, labels.
3. The error state in the render controller, the first frame, the session notification, the Qt
   property.
4. `ReadRawSensorLayout`, `EditorAiDenoiseControl`, the module host.
5. QML: the switch, the locked track, the card, the translations.

**Tests**

| Test | Assertion |
| --- | --- |
| `RenderMessageKeepsTheKindAndTheTextOfTheError` | Each kind: `what()` has the marker; the parse of a message with text before it gives the kind and the text; a message without the marker gives none |
| `BayerCheckReportsSensorNotSupported` | The DN5 check throws `AiDenoiseError` of the kind `SensorNotSupported` |
| `RawDecodeWriteTurnsAiDenoiseOnAndOff` | Write with the object, with null, and without the key: the switch and the version of the Model, the dirty mask |
| `RawDecodeValueWithoutTheKeyTurnsAiDenoiseOff` | A complete value without the key turns it off; with the key turns it on; an invalid version is rejected and the Model is unchanged |
| `RawDecodeOwnedJsonHasTheKeyOnlyWhileOn` | `FieldOwnedParameterJson("raw_decode", ...)` |
| `PanelProjectionReportsTheSwitch` | `EditorPanelRawDecodeValue::ai_denoise` |
| `PasteOfRawDecodeCarriesTheSwitch` | The package of the RAW Decode item has the key; the paste turns it on in the target, and a paste without the key turns it off |
| `TurnOffAiDenoiseIsOneChangeAndUndoTurnsItOn` (session) | The write is one commit; the next render request is of a document without the key; undo turns it on and the next request has it |
| `FirstFrameWithAModelErrorOpensThePhotoWithTheError` (session) | State Interactive, the error kind and text; a ready frame clears it; another failed frame without the marker leaves no error |
| `FirstFrameWithAnotherErrorFailsAsBefore` (session) | State Failed, no AI Denoise error |
| `TurnOnWithoutAModelRequestsTheAiModelsPageAndWritesNothing` | `EditorAiDenoiseControl` with a fake submitter |
| `TurnOnWritesTheInstalledVersionAndTurnOffWritesNull` | The written update |
| `SensorLayoutOfTheTestRawFiles` | A Bayer ARW is Bayer; a missing file reports an error |

No QML test: the panel and the card are pure UI (repository rule).

**Exit criteria**

- [x] The tests pass in `build/debug`.
- [x] The touched suites (the app editor tests, `EditorRawDecodePanelQmlTest`, `AiDenoiseLoaderTest`,
      `DevelopAiDenoiseInputTest`) have no new failure.
- [ ] The application starts; the RAW Decode card shows the switch on Windows. (The production
      window of `MainQmlWorkflowTest` loads the changed QML without a warning; the running
      application is not checked by eye, see the record.)

**Expected diff**: 900 to 1400 lines.

##### Phase DN7 completion record (2026-11-22)

**Status:** complete. The RAW Decode card has the `AI Denoise` switch with the locked Method
track and the labels `Auto` and `Neural`; the viewport shows each model error of a render with
`Turn Off AI Denoise` (and `Manage` for a missing model); a first frame with a model error opens
the photo. The switch is the `ai_denoise` key of the `raw_decode` field, so the history, the undo,
and the paste carry it. Issue #1050.

**Primary success call chain:**

```text
switch on (EditorRawDecodePanel.setAiDenoise) -> EditorAiDenoiseControl::turnOn
  -> DenoiseModelSession::InstalledModelVersion
  -> IEditorAdjustmentSubmitter::submitWrite("raw_decode",
       DevelopRawDecodeUpdate{ai_denoise = DevelopAiDenoise{version}}, settled)
  -> EditorSessionController -> EditorSessionService (one settled commit)
  -> ApplyEditorParameterWrite -> DevelopParamsModel::ApplyRawDecodeUpdate (DevelopDirty::Denoise)
  -> render with the model (DN6) -> panel projection raw.ai_denoise -> the locked Method track
Turn Off AI Denoise (EditorAiDenoiseErrorCard) -> EditorAiDenoiseControl::turnOff
  -> the same write with an empty ai_denoise -> render without the model -> FrameReady
  -> EditorSessionRenderController::SetAiDenoiseError(nullopt) -> AiDenoiseErrorChanged
  -> EditorSessionService::NotifyChange -> EditorSessionController::SyncAiDenoiseError -> card hidden
undo -> ParseEditorParameterValue("raw_decode", stored Model JSON with the key) -> switch on
open photo -> StateChanged -> EditorAiDenoiseControl::SetImage -> QThreadPool: ReadRawSensorLayout
  (LibRaw open_file, no unpack) -> sensorLayout (bayer, xtrans, other) -> switch enabled or not
```

**Primary failure call chain:**

```text
render -> AiDenoiseError (what(): "AI Denoise error (<kind>): <text>") -> PipelineScheduler
  TaskOutcome message -> EditorRenderCoordinator Failed -> EditorSessionRenderController
  ::NotifyRenderResult -> ParseAiDenoiseRenderError -> SetAiDenoiseError -> AiDenoiseErrorChanged
  -> EditorSessionController.aiDenoiseError {kind, text} -> EditorAiDenoiseErrorCard
first frame (Loading) with the marker -> CompleteFirstFrame -> Interactive with the error
first frame with another error -> RenderFailed -> session Failed (unchanged)
switch on without a model -> turnOn returns model_not_installed -> host.openAiModelSettings();
  no write; the switch returns to off
X-Trans or monochrome photo with the switch on (a paste) -> RequireDevelopInputForAiDenoise
  -> AiDenoiseError(SensorNotSupported) -> the card; the switch stays enabled to turn it off
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RenderMessageKeepsTheKindAndTheTextOfTheError` (each kind; text before the marker) | `AiDenoiseLoaderTest` | PASS |
| `MessageWithoutAnAiDenoiseErrorHasNone` (added) | `AiDenoiseLoaderTest` | PASS |
| `PlatformIsAvailableWhenItsDeviceAndRuntimeLoad` (added: one check; a runtime load error; no platform) | `AiDenoiseLoaderTest` | PASS |
| `SensorLayoutOfTheTestRawFiles`, as `SensorLayoutOfABayerRawFileIsBayer`, `SensorLayoutOfALinearDngIsNotBayer`, `SensorLayoutOfAFileThatLibRawCannotOpenIsAnError` | `AiDenoiseLoaderTest` | PASS |
| `BayerCheckReportsSensorNotSupported`, in `InputCheckRejectsAFullSizeInputThatIsNotDenoised` | `GpuDagRawInputTest` (`DevelopAiDenoiseInputTest`) | PASS |
| `RawDecodeWriteTurnsAiDenoiseOnAndOff` (the key absent keeps the switch; the method stays) | `EditorAiDenoiseSwitchTest` | PASS |
| `TypedWriteOfTheSwitchUsesTheRawDecodeUpdate` (added) | `EditorAiDenoiseSwitchTest` | PASS |
| `RawDecodeValueWithoutTheKeyTurnsAiDenoiseOff` (an invalid version leaves the Model unchanged) | `EditorAiDenoiseSwitchTest` | PASS |
| `RawDecodeOwnedJsonHasTheKeyOnlyWhileOn` | `EditorAiDenoiseSwitchTest` | PASS |
| `PanelProjectionReportsTheSwitch`, as `PanelProjectionReportsTheSwitchAndKeepsTheMethod` | `EditorAiDenoiseSwitchTest` | PASS |
| `PasteOfRawDecodeCarriesTheSwitch` (the inverse turns it off; an item without the key turns it off) | `EditorAiDenoiseSwitchTest` | PASS |
| `TurnOffAiDenoiseIsOneChangeAndUndoTurnsItOn` (the next frame request renders the switch off; one undo returns to the head of the switch on and the next request has it) | `EditorAiDenoiseSessionTest` | PASS |
| `FirstFrameWithAModelErrorOpensThePhotoWithTheError` (a failed frame without the marker keeps the error) | `EditorAiDenoiseSessionTest` | PASS |
| `ReadyFrameClearsTheErrorOfThePhoto` (added) | `EditorAiDenoiseSessionTest` | PASS |
| `FirstFrameWithAnotherErrorFailsAsBefore` | `EditorAiDenoiseSessionTest` | PASS |
| `TurnOnWithoutAModelRequestsTheAiModelsPageAndWritesNothing` | `EditorAiDenoiseControlTest` | PASS |
| `TurnOnWritesTheInstalledVersionAndTurnOffWritesNull` (only the switch is written, settled) | `EditorAiDenoiseControlTest` | PASS |
| `TurnOnIsRefusedWithoutAPlatformOrAReadableModel`, `PlatformIsCheckedOnce` (added) | `EditorAiDenoiseControlTest` | PASS |
| `SensorLayoutOfTheOpenPhotoIsReadOnAWorkerThread`, `LayoutOfAPhotoThatIsNoLongerOpenIsIgnored` (added) | `EditorAiDenoiseControlTest` | PASS |
| The labels `Auto` and `Neural` with the stored values | `EditorParameterCatalogTest` | PASS |

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target AiDenoiseLoaderTest EditorAiDenoiseSwitchTest EditorAiDenoiseSessionTest EditorAiDenoiseControlTest EditorHistoryMoveDuringInputTest EditorRawDecodePanelQmlTest EditorPipelineCommandServiceTest EditorParameterCatalogTest EditorPanelProjectionTest DocumentTransferTest GpuDagRawInputTest EditorSessionRenderControllerTest EditorRenderCoordinatorTest MainQmlWorkflowTest alcedo_main"
ctest --test-dir build/debug -R "^(AiDenoiseLoaderTest|EditorAiDenoiseSwitchTest|EditorAiDenoiseSessionTest|EditorAiDenoiseControlTest|EditorHistoryMoveDuringInputTest|EditorRawDecodePanelQmlTest|EditorPipelineCommandServiceTest|EditorParameterCatalogTest|EditorPanelProjectionTest|DocumentTransferTest)\." -j 4   # 179/180, 2 skipped
ctest --test-dir build/debug -R "^(GpuDagRawInputTest\.DevelopAiDenoiseInputTest|EditorSessionRenderControllerTest|EditorRenderCoordinatorTest)\." -j 4   # 59/59
ctest --test-dir build/debug -R "^MainQmlWorkflowTest\." -j 1   # 4/5
```

Suite totals: 179/180 of the editor and loader suites: the failure is
`DocumentTransferTest.TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches` (the Color Wheel
`version` key, issues #484 and #554, without DN7 too); the 2 skipped tests need the DN6 lab assets.
59/59 of the render controller, the coordinator, and the DN5 input tests. `MainQmlWorkflowTest`
4/5: the failure is the removed `toggleByUser` (issue #722); the production window loads the
changed `EditorWorkspace.qml` and `EditorRawDecodePanel.qml` without a QML warning. Full suite:
not run (AGENTS.md).

**Changes against the plan text and detail choices:**

- **Issue #1052 (a DN6 defect, found in the user test of DN7).** The editor render reported
  "no AI model folder is set" with the model installed: `ProcessDenoiseModelSession()` is a
  static of the static library `EditInput`, so the EditPipeline DLL (`PipelineExecutor`) and the
  application each have an instance, and the application set the folder on its own. The DN6
  record ("one instance") is wrong for the Windows DLL build. `PipelineExecutor::
  ProcessAiDenoiseSession()` returns the instance of the executors; the application sets the
  folder, reads the installed model, checks the platform, and releases the session through it.
  Test: `AiDenoiseRenderTest.ExecutorsUseTheSessionWhoseModelFolderTheApplicationSets` (PASS).
- **The error header is header only.** `EditorSessionRenderController` reads the render message
  without linking `EditInput` (LibRaw, ONNX Runtime); a first build failed at the link of the
  session library.
- **`turnOn()` returns a word** (`on`, `model_not_installed`, `refused`) and the panel opens the AI
  Models page through the host: a QML `Connections` handler does not reach a PascalCase C++ signal
  in the compiled module.
- **`sensorLayout`** is a word (`bayer`, `xtrans`, `other`, empty while unknown); the panel selects
  the translated tooltip. The read is LibRaw `open_file` on `QThreadPool`; a result for a photo
  that is no longer open is dropped by its image id.
- **The switch stays enabled for a photo that is not Bayer while it is on** (a paste can set it),
  so the user can turn it off from the panel too.
- **The quality-base frame after a first frame with a model error** is routed as after any first
  frame: it fails at once for a missing model or a slow device; after a run error it runs the
  model again, which can clear a temporary video memory error.
- **The status label `Preparing image viewport`** is hidden while the card shows.
- The card is not tested by a QML test (repository rule: no test for pure UI). The application
  was not started by hand on this computer (it would take the screen of the user).

**Checklist / exit condition:** the tests pass; the touched suites have no new failure; the
production window loads the changed QML. The switch in the running application is not checked
by eye.

**LOC note (grill-code-review):** production 1011 lines (new: `EditorAiDenoiseControl` 226,
`ai_denoise_error.hpp` 125, `EditorAiDenoiseErrorCard.qml` 113, `raw_sensor_layout` 85), tests 837
lines, plan 172 lines: above the estimate of 900 to 1400 with the tests. The changed files above
1000 lines were above it before DN7 (`editor_session_service.cpp` 3973 with 18 lines of DN7,
`editor_session_controller.cpp` 2362 with 20, `application_module_host.cpp` 1061 with 34,
`EditorWorkspace.qml` 1118 with 22); the new logic is in its own owners.

**Remaining gaps:** the library action and the export errors (DN8; the export executor uses the
same instance through the default of `PipelineExecutor`, which reads the export error
with `ParseAiDenoiseRenderError`); the macOS and Linux platforms (DN10, DN11) show no switch until
then.

### DN14: render feedback in the editor

Written on 2026-11-23, before the implementation. The user moved DN14 before DN8 and asked for
feedback at each step of a render, because the Develop part of the first render takes long. The
user chose direction A of the mock of 2026-11-23 (a caption pill and a segmented bar).

**Objective and deliverables**

While a render of the editor reads the RAW file or runs the model, the viewport shows the step
of the render and its count; before the first frame of a photo, it shows a placeholder image.
The deliverables: the step progress of a render from the renderer to the viewport, the tile
count of the model run, the Develop group count of the plan executor, the placeholder choice
and its image provider, the pill and the bar in QML, the translations.

**Inputs and prerequisites**

DN6 (`DenoiseModelSession`, the tile callback of DN3), DN7 (the error state of the render
controller), the viewport render alert of issue #1056.

**Product rules** (the mock of 2026-11-23, direction A)

- **Steps of a render.** `Reading RAW` (the unpack of the encoded RAW file), `AI Denoise` (the
  tile loop), `Developing` (the Develop passes of the plan). A render has `Reading RAW` when its
  source is not in the prepared source cache, `AI Denoise` when the switch is on and the denoised
  source is not in the cache, and `Developing` always.
- **When it shows.** Only a render with `Reading RAW` or `AI Denoise` reports steps: the first
  frame of a photo or of another Version, the frame after the switch goes on, a frame after the
  prepared source cache was cleared (editor GPU release). A render that reads only the caches
  (a slider frame, the Quality Base frame after the first frame) reports nothing and keeps the
  present spinners.
- **Counts.** `AI Denoise`: tile k of n from the tile callback; before the first tile (the
  session creation and the first tile, 0.1 to 0.5 s) it shows 0 of n, and n is 0 until the
  loop has planned its tiles (the pill then shows no count). `Developing`: the group that runs,
  of the groups of the plan: `Sensor`, `Lens and geometry`, `Color`, `Color Grade i of n` (one
  for each Color Grade of the plan), `Display`. The sensor group ends with a wait for the GPU;
  in an editor render the later groups are recorded without a wait (the session-packed
  allocation), so their names change when the host records them and the frame completes after
  the last. The mock said "the group that the GPU finished"; the group that runs is what the
  plan executor knows without a new wait.
- **Pill and bar.** A frosted pill at the bottom left of the viewport (12 px from the edges):
  one dot for each step of the render (done, current, later), the step name, its count. Under
  it, at the bottom edge of the viewport, a bar of 3 px with one segment for each step, 2 px
  apart: a done step is full, the current step fills by its count, `Reading RAW` (no count)
  sweeps. The pill and the bar fade out in 200 ms when the render ends (frame, error,
  cancellation).
- **Spinners.** The center spinner of the viewport does not show while the pill shows. The
  corner spinner of slider frames does not change.
- **Error.** A render that ends with an error hides the pill and the bar; the render alert of
  issue #1056 shows the AI Denoise error (DN7).
- **Placeholder before the first frame**, in this order:
  1. the library thumbnail of the open Version from the thumbnail image store, the largest tier
     that a view of the application holds (2048, 1024, 512, 256). It is rendered from the
     committed edit with its crop and without the model. The editor copies the image at the
     open, because the store drops an image when its last view releases it.
  2. the camera preview of the RAW file (`EmbeddedPreviewReader`), only for a Version without
     a crop and without a rotation, read on a worker thread.
  3. none: the present label `Preparing image viewport`, with the pill and the bar.
  The placeholder is fitted in the viewport like the frame at the fit zoom and hides at the
  first presented frame. The image is released when the photo enters Interactive or closes.
- **Switch on in an open photo.** The current frame stays; the pill and the bar show over it
  (the source is in the cache, so the render has the steps `AI Denoise` and `Developing`).

**Data rules**

- **Progress value.** `RenderProgress` (`edit/runtime/render_progress.hpp`, header only, so the
  editor session reads it without the render libraries): the steps of the render
  (`read_raw`, `ai_denoise`), the current step, `completed` and `total` (tiles; Develop groups),
  and for `Developing` the group kind and the Color Grade index and count. A value, copied
  through the queues.
- **Route.** The render controller puts a progress callback into each render intent
  (presentation state, in no key, as the Mask coverage probe). The scheduler port gives it to
  the pipeline task, the task to the apply request, and the renderer reports through it on the
  render worker. The session service posts each value to its owner queue, where the render
  controller keeps it as the progress of the open photo; the queue orders it against the
  render results, because the scheduler completes a task on its worker before it starts the
  next. A terminal result of the photo (ready, failed, cancelled, refused while suspended) and
  the load of another photo clear it. Each change notifies the render progress observer;
  `EditorSessionController` publishes `renderProgress`.
- **Cache queries.** `PreparedSourceCache::HoldsEncoded` and `HoldsDenoised` answer whether an
  acquire would read the RAW file or run the model, without a change of the cache.
- **Placeholder owner.** `EditorViewportPlaceholder` (album backend, `appModules.editorPlaceholder`)
  keeps the placeholder image of the open photo and serves it through the image provider
  `alcedo-editor-placeholder`. It keeps no copy of the session state: the module host gives it
  the open photo and the session state.

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `include/edit/runtime/render_progress.hpp` (new) | `RenderProgress`, `RenderProgressCallback`, `RenderProgressReporter` |
| `include/edit/input/prepared_source_cache.hpp`, `.cpp` | `HoldsEncoded`, `HoldsDenoised` |
| `include/edit/input/ai_denoise_model_session.hpp`, `.cpp` | `Denoise` reports the tiles |
| `include/edit/runtime/pipeline_apply_request.hpp`, `include/renderer/pipeline_task.hpp`, `renderer/pipeline_scheduler.cpp` | The callback of the request and of the task |
| `include/edit/runtime/detail/renderer.inl.hpp`, `include/edit/runtime/plan_executor.hpp`, the `Execute` of the render devices | The steps, the Develop groups |
| `include/app/editor_render_intent.hpp`, `ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp` | The callback of the intent |
| `app/editor_session_render_controller.cpp`, `app/editor_session_service.cpp`, their headers | The progress state, the owner queue, the observer |
| `ui/alcedo_main/album_backend/editor_session_controller.*` | `renderProgress` |
| `ui/alcedo_main/album_backend/editor_viewport_placeholder.*` (new), `application_module_host.cpp` | The placeholder choice, its image provider |
| `qml/EditorRenderProgress.qml` (new), `qml/EditorWorkspace.qml`, the `.ts` files | The pill, the bar, the placeholder image |

**Tests**

| Test | Assertion |
| --- | --- |
| `CacheReportsWhetherAnAcquireReadsTheRawOrRunsTheModel` | `HoldsEncoded` and `HoldsDenoised` before and after the acquires; the stats do not change |
| `DenoiseReportsEachTileOfTheRun` (`AiDenoiseLoaderTest`) | k = 1 to n of n in order, n = the tiles of the record |
| `ReporterSendsTheStepsAndTheDevelopGroups` | The values of `RenderProgressReporter` for a render with each set of steps |
| `RenderThatReadsTheRawReportsItsStepsInOrder` (GPU render test) | Reading RAW, then the Develop groups 1 to n of n with their kinds; the next render of the same source reports nothing |
| `ProgressOfTheOpenPhotoIsPublishedAndClearedByItsFrame` (session) | The callback of the intent gives the progress of the session; a ready frame clears it; a failed frame clears it; a value of another load request is dropped |
| `ProgressIsClearedWhenAnotherPhotoLoads` (session) | |
| `PlaceholderUsesTheLargestHeldThumbnail` | The largest tier of the store, copied |
| `PlaceholderReadsTheCameraPreviewOnlyWithoutCropAndRotation` | No thumbnail: the preview is read on a worker for a Version without crop and rotation; with a crop or a rotation there is none |
| `PlaceholderOfAPhotoThatIsNoLongerOpenIsDropped`, `PlaceholderIsReleasedWhenThePhotoIsInteractive` | |

No QML test: the pill, the bar, and the placeholder image are pure UI (repository rule).

**Exit criteria**

- [x] The tests pass in `build/debug`.
- [x] The touched suites have no new failure.
- [x] The production window of `MainQmlWorkflowTest` loads the changed QML without a warning.

**Expected diff**: 900 to 1400 lines (the estimate of section 6 was for the bar alone).

##### Phase DN14 completion record (2026-11-23)

**Status:** complete. A render of the open photo that reads the RAW file or runs the model shows
its steps in a pill and a segmented bar at the bottom of the viewport (`Reading RAW`,
`AI Denoise` with tile k of n, `Developing` with the group that runs); before the first frame
the viewport shows the held library thumbnail of the photo or its camera preview. Issue #1059.

**Primary success call chain:**

```text
open photo -> EditorSessionRenderController::MakeRenderIntent (intent.render_progress = the
  receiver with the image-load request) -> EditorSessionRenderSchedulerPort (task.render_progress_)
  -> PipelineTask::MakeApplyRequest (request.progress) -> Renderer::Render (render worker):
     PreparedSourceCache::HoldsDenoised / HoldsEncoded -> RenderProgressReporter(read_raw, ai_denoise)
     -> ReadRawStarted -> AcquireDenoised -> DenoiseModelSession::Denoise(on_tile)
        -> AiDenoiseTiles(0, 0), then (k, n) after each tile of DenoiseBayerMosaic
     -> PlanExecutor::Execute(progress) -> DevelopGroupStarted(Sensor, Lens and geometry, Color,
        Color Grade i, Display)
  -> receiver -> EditorSessionService command queue (owner) -> EditorSessionRenderController
     ::NotifyRenderProgress (the active image-load request only) -> PublishRenderProgressIfChanged
  -> RenderProgressObserver -> EditorSessionController::SyncRenderProgress -> renderProgress
  -> EditorRenderProgress.qml (pill, bar)
FrameReady -> EditorSessionRenderController::NotifyRenderResult clears the progress -> the pill
  and the bar fill and fade out (viewportProgressFadeMs)
state Acquiring/Loading/Switching -> ApplicationModuleHost -> EditorViewportPlaceholder
  ::SetWaitingPhoto -> held thumbnail (largest tier of ThumbnailImageStore, copied)
  | QThreadPool: committed snapshot without crop and rotation -> EmbeddedPreviewReader::Read(768)
  -> EditorPlaceholderImage -> image://alcedo-editor-placeholder/<revision> -> Image under the
  viewport item while no frame is presented; Interactive -> SetWaitingPhoto(0, 0) releases it
```

**Primary failure call chain:**

```text
model error in the tile loop -> AiDenoiseError -> Failed result -> NotifyRenderResult clears the
  progress -> the pill fades; ParseAiDenoiseRenderError -> the render alert of #1056 (DN7)
cancelled or refused (suspended) frame -> the progress clears with the result
progress of a closed or replaced photo -> NotifyRenderProgress: another image-load request -> dropped
render that reads only the caches -> inactive RenderProgressReporter -> nothing reported; the
  present spinners
camera preview of a photo that no longer waits -> ApplyCameraPreview: another photo -> dropped
geometry check or preview read throws -> logged; no placeholder, the label shows
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `CacheReportsWhetherAnAcquireReadsTheRawOrRunsTheModel` (also: the queries change no stats; another version or camera matrix is another entry; Clear) | `AiDenoiseLoaderTest` | PASS |
| `DenoiseReportsEachTileOfTheRun` | `AiDenoiseLoaderTest` | PASS |
| `ReporterSendsTheStepsAndTheDevelopGroups` | `AiDenoiseLoaderTest` (`RenderProgressTest`) | PASS |
| `RenderThatReadsOnlyTheCachesReportsNothing` (added) | `AiDenoiseLoaderTest` (`RenderProgressTest`) | PASS |
| `RenderThatReadsTheRawReportsItsStepsInOrder` (CUDA and OpenCL: Reading RAW, tiles 1 to n, groups in order; the next frame reports nothing; the switch off reports Reading RAW and the groups; the switch on in the same document reports no Reading RAW) | `AiDenoiseRenderTest` | PASS (both backends) |
| `ProgressOfTheOpenPhotoIsPublishedAndClearedByItsFrame` (the owner queue, the observer, the same value notifies nothing, a ready and a failed frame clear it) | `EditorAiDenoiseSessionTest` | PASS |
| `ProgressIsClearedWhenAnotherPhotoLoads`, as `ProgressOfAPhotoThatIsNoLongerOpenIsDropped` (the close clears it; a late value is dropped) | `EditorAiDenoiseSessionTest` | PASS |
| `PlaceholderUsesTheLargestHeldThumbnail` (a copy; the provider serves it; no preview read) | `EditorViewportPlaceholderTest` | PASS |
| `PlaceholderReadsTheCameraPreviewOnlyWithoutCropAndRotation` (worker thread; a crop and a failed check give none) | `EditorViewportPlaceholderTest` | PASS |
| `PlaceholderOfAPhotoThatIsNoLongerOpenIsDropped`, `PlaceholderIsReleasedWhenThePhotoStopsWaiting` (as `...StopsWaiting`; a new URL for each open) | `EditorViewportPlaceholderTest` | PASS |
| The FP16 graph on DirectML through the renderers, with the lab assets (`ALCEDO_AI_DENOISE_TEST_ASSETS=build/tmp/dn6/assets`) | `AiDenoiseRenderTest` | PASS (CUDA, OpenCL) |
| The production window loads the changed QML without a warning (`QT_FORCE_STDERR_LOGGING=1`) | `MainQmlWorkflowTest` | 4/5: the failure is `toggleByUser` (issue #722); no QML warning |

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target alcedo_main"
cmd /c "scripts\msvc_env.cmd --build build\debug --target AiDenoiseLoaderTest AiDenoiseRenderTest EditorAiDenoiseSessionTest EditorViewportPlaceholderTest EditorSessionRenderControllerTest EditorAiDenoiseControlTest MainQmlWorkflowTest"
ctest --test-dir build/debug -R "^(AiDenoiseLoaderTest|EditorAiDenoiseSessionTest|EditorViewportPlaceholderTest|EditorSessionRenderControllerTest|EditorAiDenoiseControlTest)\." -j 4   # 71/71, 2 skipped (lab assets)
ctest --test-dir build/debug -R "^AiDenoiseRenderTest\." -j 1   # 9/9 with ALCEDO_AI_DENOISE_TEST_ASSETS
ctest --test-dir build/debug -R "^MainQmlWorkflowTest\." -j 1   # 4/5 (#722)
ctest --test-dir build/debug -R "^(EditorRenderCoordinatorTest|EditorSessionRenderSchedulerPortTest|EditorSessionServiceFacadeTest|EditorSessionControllerPhase5ATest|GpuDagRawInputTest|EditorAiDenoiseSwitchTest)\." -j 4   # 423/423
```

Suite totals: see the commands. Full suite: not run (AGENTS.md).

**Changes against the plan text and detail choices:**

- **The Develop count is the group that runs, not the group that the GPU finished** (the mock
  text). The plan executor waits for the GPU after the sensor group; in an editor render the
  later groups are recorded without a wait. A new wait would slow every render.
- **Reading RAW is reported before the acquire** when the cache query says the acquire unpacks;
  `AcquireDenoised` keeps its signature.
- **A batch render (export) also reports** when its request has a receiver; no export sets one
  yet (DN8 can).
- **The pill has no background blur**: a fill of (18, 18, 18) at 184/255 with the glass stroke.
  A blur of the region under the pill would need a second capture of the RHI viewport for each
  frame of the run.
- **The placeholder states** are Acquiring, Loading, and Switching of the session; its camera
  preview is read at a shorter edge of at least 768 px.
- **DESIGN.md** gets "Viewport render progress": the user requested the pill (2026-11-23), an
  exception to the ban of unrequested pills for this surface only.
- **A test trap:** two snapshot sources are two bindings of a renderer, and a new binding clears
  the source cache, so the switch-on case of the renderer test turns the switch on in the same
  document, as the editor does.
- The pill, the bar, and the placeholder image are not checked by a QML test (repository rule)
  and were not checked by eye in the running application.

**Checklist / exit condition:** the tests pass in `build/debug`; the touched suites have no new
failure; the production window loads the changed QML without a warning.

**LOC note:** about 1800 added lines in `alcedo_studio` (about 620 of them tests, 120 the
translations and DESIGN.md); new files below 220 lines. `editor_session_service.cpp` (3986
lines) and `editor_session_controller.cpp` (2433 lines) were above 1000 lines before; DN14 adds
13 and 71 lines to them.

**Remaining gaps:** the feedback in the running application is not checked by eye.

**2026-11-23, user test: the placeholder did not show.** Cause: the QML image showed only while
`EditorViewportItem.presentationAvailable` was false, but that property says that the viewport
has a render target (`consumer_available` of the present queue), which it has before any frame;
the image was also under the viewport item (z 1 against 2). The upload of the image is not the
cause: it is a QML `Image` from its own provider and does not pass through the render queue.
Fix: the image stands above the viewport item (z 2.5, under the overlays and the status chrome)
while it has a source, and it is released when `presentedFrameCount` of the viewport advances
after it showed (`EditorViewportPlaceholder::release`), so no empty frame shows between the two.
The Interactive state no longer releases it (`SetFirstFrameReady`); a camera preview that arrives
after the first frame is dropped. Tests `PlaceholderStaysUntilTheViewportComposesTheFirstFrame`
and `CameraPreviewAfterTheFirstFrameIsDropped` (`EditorViewportPlaceholderTest`, 6/6 PASS). The
log line `Editor placeholder: <kind> <w> x <h>` names the chosen image.

### DN8: batch through Paste, and export errors

Written on 2026-11-23, before the implementation. Revised on 2026-11-23 by the user: AI Denoise
for several photos goes through Copy Adjustments and Paste Adjustments (the RAW Decode item); the
library gets no action of its own.

**Objective and deliverables**

Several photos get AI Denoise through the existing Copy and Paste of the RAW Decode item, and the
export reports a model error for each photo and continues. The deliverables: the export item
status of a model error, the `Turn Off AI Denoise` and `Manage` controls of the export queue
item, the stored-history write of the switch for `Turn Off AI Denoise`, the queue state after it,
the translations.

**Inputs and prerequisites**

DN6 (the model run in the export render through the session of `PipelineExecutor`), DN7
(`AiDenoiseError`, `ParseAiDenoiseRenderError`, `EditorAiDenoiseControl`, the `ai_denoise` key of
`raw_decode` and its paste), the export item status `missing` of issue #1018.

**Product rules** (2.2 is the specification; these are the details of the implementation)

- **Several photos.** The user turns AI Denoise on in one photo, copies its adjustments with the
  RAW Decode item, and pastes them on the other photos. The paste carries `ai_denoise` (DN7,
  `PasteOfRawDecodeCarriesTheSwitch`) and runs no model (D5). The library has no separate
  `AI Denoise` action (user, 2026-11-23).
- **A pasted photo that is not Bayer** gets the parameter as each other paste; its render reports
  `SensorNotSupported` with `Turn Off AI Denoise` (the card of DN7, the export item below). The
  paste does not read the sensor of its targets.
- **Export error.** A photo whose export render fails with an AI Denoise error gets the item
  status `ai_denoise`. It stays in the export queue, as a `missing` photo does; the other photos
  of the run continue. The queue row shows the title of the kind (the titles of the DN7 card)
  and the controls: `Turn Off AI Denoise`, and `Manage` for `model_not_installed`.
- **Turn Off AI Denoise of a queue item** writes one `raw_decode` change with `ai_denoise: null`
  on the active Version of the photo (through the editor session when the editor has it open),
  then puts the item back to `Queued`: it leaves the run whose status it showed and joins the
  next export. It starts no export. A background task `AI Denoise` shows the write and holds the
  locks of a Paste, because it writes a stored history.

**Data rules**

- **Stored-history write.** `WriteAiDenoiseSwitchToStoredHistory` (`app/ai_denoise_history_write`)
  reads the history with `PipelineMgmtService::LoadHistorySnapshot`, replays the active Version
  head (`BuildDocumentFromRoot`), applies the `raw_decode` write to the replayed document
  (`ApplyEditorParameterWrite`), builds the batch with `MakeSetParameterBatch`, inserts one commit
  on the active Version, and writes the history with `PersistHistory` (one transaction; a
  failure writes nothing). It returns `changed`, the head commit time, and the Version name.
- **Item status.** `ExportProgress` gets the message of a finished task. `ExportItemStatusOf`
  gives `succeeded`, `missing`, `ai_denoise` (the message has an AI Denoise error), or `failed`.
  `ImportExportHandler::exportItemAiDenoiseErrors` maps the status key to `{kind, text}`.
- **Queue state.** `ExportQueue::ApplyExportState` keeps the `ai_denoise` entries.
  `ExportQueue::ReturnToQueued(element_id)` sets the run of the entry to 0, so the entry shows
  `Queued` and the next run takes it.
- **Owner.** `ExportAiDenoiseControl` (album backend, `appModules.exportAiDenoise`):
  `Turn Off AI Denoise` of a queue item. Its services are functions (the agent refusal, the
  stored-history writer bound to the pipeline service on the GUI thread, the editor photo and
  its switch, the refresh of a written photo), so the tests run it without the application. One
  write at a time.

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `include/app/ai_denoise_history_write.hpp`, `app/ai_denoise_history_write.cpp` (new) | `WriteAiDenoiseSwitchToStoredHistory` |
| `include/app/export_service.hpp`, `app/export_service.cpp` | `ExportProgress::task_message_` |
| `include/ui/alcedo_main/album_backend/import_export.hpp`, `import_export.cpp` | `ExportItemStatusOf`, `exportItemAiDenoiseErrors` |
| `include/ui/alcedo_main/album_backend/export_queue.hpp`, `export_queue.cpp` | Keep `ai_denoise` entries; `ReturnToQueued` |
| `include/ui/alcedo_main/album_backend/export_ai_denoise_control.hpp`, `.cpp` (new), `application_module_host.*` | `Turn Off AI Denoise` of a queue item, the background task |
| `include/ui/alcedo_main/album_backend/background_task_controller.hpp`, `.cpp` | `BackgroundTaskKind::AiDenoiseSwitch` |
| `qml/ExportInspectorPanel.qml`, `qml/InspectorPanel.qml`, `qml/LibraryWorkspace.qml`, `qml/BackgroundTaskBar.qml`, `qml/BackgroundTasksDialog.qml`, the `.ts` files | The queue row controls, the task name |

**Primary success call chain**

```text
export run -> render -> AiDenoiseError -> ExportResult::message_ -> ExportProgress::task_message_
  -> ExportItemStatusOf = ai_denoise, exportItemAiDenoiseErrors -> ExportQueue keeps the entry
  -> queue row: title of the kind, Turn Off AI Denoise
Turn Off AI Denoise (queue row) -> ExportAiDenoiseControl::turnOffForExportItem -> worker:
  WriteAiDenoiseSwitchToStoredHistory(off) -> GUI: refresh -> ExportQueue::ReturnToQueued -> Queued
```

**Primary failure call chain**

```text
Turn Off write fails -> the item keeps the ai_denoise status; the message in the snackbar
an agent holds control, or a write runs -> refused with the reason; nothing starts
```

**Implementation steps**

1. `WriteAiDenoiseSwitchToStoredHistory` and its tests.
2. The export message, the item status, the kept queue entries, `ReturnToQueued`.
3. `ExportAiDenoiseControl`, the background task kind, the module host.
4. QML: the queue row controls, the task name, the translations.

**Tests**

| Test | Assertion |
| --- | --- |
| `TurnOnWritesOneRawDecodeCommitOnTheActiveVersion` | One new commit on the active Version; the replayed document has the switch with the version; the Version count is unchanged; the other parameters of the head stay |
| `WriteOfTheSameValueIsUnchanged` | The switch already on with the version, or already off: `changed` false, no commit |
| `TurnOffRemovesTheKeyFromTheStoredDocument` | The stored document has no `ai_denoise` key after the write (root id rule of DN5) |
| `WriteRefusesAnImageThatTheEditorHolds` | Throws; the stored history is unchanged |
| `ItemStatusOfAnAiDenoiseErrorIsAiDenoise` | `ExportItemStatusOf` for each status; a message with the marker after other text |
| `AiDenoiseEntriesStayInTheQueueAndReturnToQueued` (`ExportQueueTest`) | The entry stays after its run; `ReturnToQueued` clears its status key; the next run takes it |
| `TurnOffOfAQueueItemWritesTheSwitchOffAndReturnsTheItemToQueued` | The write is off; the queue entry has no status key after it |
| `FailedTurnOffKeepsTheItemInItsRun` | |
| `TurnOffOfTheEditorPhotoGoesThroughTheEditor` | |
| `TurnOffIsRefusedWhileAnAgentHoldsControlOrAWriteRuns` | |

No QML test: the row controls are pure UI (repository rule).

**Exit criteria**

- [x] The tests pass in `build/debug`.
- [x] The touched suites (`ExportQueueTest`, `EditorAiDenoiseControlTest`) have no new failure.
- [x] The production window of `MainQmlWorkflowTest` loads the changed QML without a new warning.

**Expected diff**: 700 to 1000 lines.

##### Phase DN8 completion record (2026-11-23)

**Status:** complete. Several photos get AI Denoise through Copy and Paste of the RAW Decode
item. The export keeps a photo whose render fails with an AI Denoise error in the queue with the
title of the error and the controls `Turn Off AI Denoise` and `Manage`. Issue #1061.

**Primary success call chain:**

```text
Copy Adjustments (RAW Decode item of a photo with the switch on) -> Paste Adjustments
  -> AdjustmentTransferApplyCoordinator (or the editor session for the open photo): the
     raw_decode value with ai_denoise in a new root-relative Version of each target (DN7)
export run -> ExportService::RunExportRenderTask -> render throws AiDenoiseError -> ExportResult
  ::message_ -> ExportProgress::task_message_ -> ImportExportHandler: ExportItemStatusOf =
  ai_denoise, exportItemAiDenoiseErrors[key] = {kind, text} -> ExportQueue::ApplyExportState keeps
  the entry -> ExportInspectorPanel row: title of the kind, Turn Off AI Denoise (Manage)
Turn Off AI Denoise (row) -> ExportAiDenoiseControl::turnOffForExportItem
  -> BackgroundTaskController::RegisterTask(AiDenoiseSwitch, the locks of a Paste)
  -> QThreadPool: WriteAiDenoiseSwitchToStoredHistory(off): LoadHistorySnapshot ->
     BuildDocumentFromRoot(active head) -> CompleteCurrentPanelParameterTarget(raw_decode) ->
     ApplyEditorParameterWrite -> MakeSetParameterBatch -> one commit on the active Version ->
     PersistHistory (one transaction)
     (the photo open in the editor: EditorAiDenoiseControl::turnOff on the GUI thread)
  -> GUI: refresh (SetAlbumItemEditState, InvalidateThumbnail, RefreshCurrentThumbnail,
     NoteEditHistoryChanged) -> ExportQueue::ReturnToQueued (run 0, no statusKey) -> Queued; the
     next export takes it -> FinishTask -> TurnOffFinished
```

**Primary failure call chain:**

```text
Turn Off write throws (the editor took the image, another writer, a replay failure)
  -> the history is unchanged; the item keeps its run and its ai_denoise status
  -> TurnOffFinished with the message -> ExportInspectorPanel.messageRequested -> snackbar
editor refuses the change -> the same, "The editor did not accept the change."
an agent holds control, or a write runs -> turnOffForExportItem refused with the reason -> snackbar
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `TurnOnWritesOneRawDecodeCommitOnTheActiveVersion` (the first parent is the old head; the Version list and the active Version stay; an earlier exposure edit stays) | `AiDenoiseHistoryWriteTest` | PASS |
| `WriteOfTheSameValueIsUnchanged` | `AiDenoiseHistoryWriteTest` | PASS |
| `TurnOffRemovesTheKeyFromTheStoredDocument` | `AiDenoiseHistoryWriteTest` | PASS |
| `WriteRefusesAnImageThatTheEditorHolds` | `AiDenoiseHistoryWriteTest` | PASS |
| `ItemStatusOfAnAiDenoiseErrorIsAiDenoise` | `ExportQueueTest` | PASS |
| `AiDenoiseEntriesStayInTheQueueAndReturnToQueued` | `ExportQueueTest` | PASS |
| `TurnOffOfAQueueItemWritesTheSwitchOffAndReturnsTheItemToQueued`, as `...ReturnsItToQueued` | `ExportAiDenoiseControlTest` | PASS |
| `FailedTurnOffKeepsTheItemInItsRun` | `ExportAiDenoiseControlTest` | PASS |
| `TurnOffOfTheEditorPhotoGoesThroughTheEditor` | `ExportAiDenoiseControlTest` | PASS |
| `TurnOffIsRefusedWhileAnAgentHoldsControlOrAWriteRuns` | `ExportAiDenoiseControlTest` | PASS |
| The paste of the RAW Decode item carries the switch (DN7, `PasteOfRawDecodeCarriesTheSwitch`) | `EditorAiDenoiseSwitchTest` | PASS in DN7; not changed by DN8 |
| The editor switch is unchanged | `EditorAiDenoiseControlTest` | PASS |
| The production window loads the changed QML (`QT_FORCE_STDERR_LOGGING=1`) | `MainQmlWorkflowTest` | 4/5: the failure is `toggleByUser` (#722); the only QML warning is the Material `Dialog.qml` radius warning of #722 and #749 |

Commands:

```text
cmd /c "scripts\msvc_env.cmd --build build\debug --target AiDenoiseHistoryWriteTest ExportQueueTest ExportAiDenoiseControlTest EditorAiDenoiseControlTest MainQmlWorkflowTest alcedo_main"
ctest --test-dir build/debug -R "^(AiDenoiseHistoryWriteTest|ExportQueueTest|ExportAiDenoiseControlTest|EditorAiDenoiseControlTest)\." -j 4   # 23/23
ctest --test-dir build/debug -R "^MainQmlWorkflowTest\." -j 1   # 4/5 (#722)
```

Suite totals: see the commands. Full suite: not run (AGENTS.md).

**Changes against the plan text and detail choices:**

- **No library action (user, 2026-11-23).** The first version of DN8 added `AI Denoise` to the
  image context menu with its own Bayer check and background run. The user rejected the extra
  design: several photos go through Copy and Paste Adjustments, which already carries the switch.
  Section 2.2 and D14 follow: the paste does not read the sensor; a render of a photo that is not
  Bayer reports `SensorNotSupported` with `Turn Off AI Denoise`.
- **`Turn Off AI Denoise` writes on the active Version**: the change of the editor switch, so the
  editor undoes it as any other change and the export renders it.
- **The writer is bound on the GUI thread** (`stored_history_writer`): the pipeline service of
  the project is read on the GUI thread, as the Paste coordinator reads it, and the worker only
  calls the bound writer.
- **`Turn Off AI Denoise` starts no export**: the item shows `Queued` and the next export takes it.
- **The automation export items** report the new state `ai_denoise` without a change of the
  command (the state is the item status text).
- The row controls are not tested by a QML test (repository rule) and were not checked by eye in
  the running application.

**Checklist / exit condition:** the tests pass in `build/debug`; the touched suites have no new
failure; the production window loads the changed QML without a new warning.

**LOC note:** about 1000 added lines in `alcedo_studio` and the plan; new files below 200 lines.
`application_module_host.cpp` (above 1000 lines before) gets the wiring of the control; the logic
is in the new owners.

**Remaining gaps:** the queue row is not checked by eye in the running application; an export
with a real model error was not run (the tests drive the status from the render message).

### DN10: macOS forward code (Core ML on the Neural Engine)

Written on 2026-11-23, before the implementation. The fixed points are the DN10 bullet of
"DN11 to DN13" (the configuration of the DN1 addition "FP16 on the Neural Engine", U5) and D13.

**Objective and deliverables**

AI Denoise runs on macOS (Apple silicon) in the Develop render of the editor and of the export,
with the FP16 graph of DN2 on the Neural Engine. The deliverables:

- `UtNet2CoreMlSession`: the ONNX Runtime session of the FP16 graph with the Core ML execution
  provider, one session for each tile input shape, with the run, check, and release interface
  of `UtNet2DirectMlSession`, so that the DN3 tile loop and `DenoiseModelSession` run unchanged.
- `libonnxruntime.dylib` of `Microsoft.ML.OnnxRuntime` 1.24.4 (osx-arm64) as a C++ dependency,
  downloaded at configure time with its SHA-256, linked by the session, and installed into
  `Contents/Frameworks` of the bundle.
- The AI Denoise platform of macOS (`CreateProductAiDenoisePlatform`), so that the switch of the
  RAW Decode panel appears on macOS (D13).
- The first build and run of the DN5 Metal code, and the DN6 render tests on the Metal renderer.
- The time of a full photo against `target_time` (0.60 s at 45.4 MP on the M4), the session
  creation time of a photo size with and without `ModelCacheDirectory`, and the footprint of the
  process after a run, after a batch of photos, and after the release of the sessions.

**Inputs and prerequisites**

DN2 (the FP16 graph, SHA-256 `0bc98084...`, `output_scale` 170.34; the DN0 expected outputs),
DN3 (the tile planner and the tile loop), DN5 (the Metal code of the denoised input), DN6
(`DenoiseModelSession`, `AiDenoisePlatform`), DN7 (the switch, shown when
`IsPlatformAvailable`). The MacBook Air M4 (16 GB, macOS 27.0.1) of the DN1 additions, reached
over SSH. The graph has the free dimensions `height`, `width`, `output_height`, and
`output_width`; its 50 nodes use Conv, LeakyRelu, MaxPool, ConvTranspose, Concat, and
DepthToSpace.

**Product rules**

- **Device.** The model runs on the Neural Engine of Apple silicon: Core ML `ModelFormat`
  MLProgram, `MLComputeUnits` CPUAndNeuralEngine, `session.disable_cpu_ep_fallback` 1 (a node
  that Core ML does not take is a load error, not a CPU run: section 1.4). An Intel Mac has no
  build of the session (the macOS build is arm64), so it has no platform and no switch (D13).
- **One session for each tile input shape.** The shape is fixed with the free dimension
  overrides of the four dimensions. A photo size uses up to 4 shapes (the full tile, the last
  column, the last row, the corner). The sessions of the shapes that the editor and the export
  used stay until `Release` (2.3); the first run of a new shape creates its session and is not
  measured by the slow-device check (2.4).
- **Tile size.** From the memory of the Mac, with the table `UtNet2Spec::kCoreMlTileMemory`
  (1536, 1024, 768 packed px; values from the DN10 measurement of the footprint growth). The
  memory that the selection can use is the physical memory that is free or inactive (Mach
  `vm_statistics64`), plus the memory that the sessions hold. The session gives its table to the
  tile loop (`UtNet2TileSession::TileMemory`), so the loop selects with the table of the platform
  that runs.
- **Errors** are the errors of 2.4: a load error names the graph and the ONNX Runtime or Core ML
  text; a run error names the tile; a value that is not finite names the tile and the value.

**Data rules**

- No persisted format changes. The model version is the SHA-256 of the same FP16 graph file on
  both platforms (D10), so a project from Windows opens on macOS with the same content key.
- `ModelCacheDirectory` (the compiled Core ML models of the sessions): DN10 measures the session
  creation of a photo size with and without it. The cache is used only when it shortens the
  creation of the sessions of a photo size by more than a third; then its folder is
  `~/Library/Caches/<bundle identifier>/ai-denoise-coreml` and its content belongs to the model
  version (the folder name holds the version). The completion record states the result.

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `scripts/cmake/fetch_onnxruntime_macos.cmake` (new) | The NuGet package `Microsoft.ML.OnnxRuntime` 1.24.4: SHA-256, headers, `libonnxruntime.1.24.4.dylib` (its install name); the target `alcedo::onnxruntime_macos`; the install into the bundle |
| `include/decoders/processor/nn/utnet2_onnx_graph.hpp`, `decoders/processor/nn/utnet2_onnx_graph.cpp` (new) | What both platform sessions share: the graph check against `UtNet2Spec`, the tile and crop checks, the float32 to float16 tile input, the float16 output rectangle to float32 times `output_scale` with the check for values that are not finite. Moved out of `utnet2_directml_session.cpp` |
| `include/decoders/processor/nn/utnet2_coreml_session.hpp`, `decoders/processor/nn/utnet2_coreml_session.cpp` (new) | `UtNet2CoreMlSession`, `LoadCoreMlRuntime`, `CoreMlDeviceKey` |
| `include/decoders/processor/nn/utnet2_tile_loop.hpp`, `utnet2_tile_loop.cpp`, `utnet2_specs.hpp` | `UtNet2TileSession::TileMemory`; `UtNet2TileLoopOptions::tile_memory` empty selects the table of the session; `kCoreMlTileMemory` |
| `decoders/processor/nn/utnet2_directml_session.cpp` | Uses `utnet2_onnx_graph`; `TileMemory` |
| `edit/input/ai_denoise_coreml_platform.cpp` (new), `edit/CMakeLists.txt` | The platform of macOS |
| `decoders/CMakeLists.txt`, `CMakeLists.txt` | `UtNet2CoreMl`; the dylib in `Contents/Frameworks` |
| `tests/ai_denoise/utnet2_session_test.cpp` (renamed from `utnet2_directml_session_test.cpp`), `tests/ai_denoise/CMakeLists.txt` | The DN2 and DN3 session tests for the session of the build: `UtNet2DirectMlTest` (Windows), `UtNet2CoreMlTest` (macOS) |
| `tests/ai_denoise/utnet2_coreml_photo_timing_tool.cpp` (new) | The time of full photos, the session creation, the footprint |
| `tests/ai_denoise/ai_denoise_render_test.cpp` | The DN6 render tests on Metal; the RAW photo with the FP16 graph on the platform of the build |

**Primary success call chain**

```text
render of a photo with ai_denoise (MetalRenderer, DecodeRes::FULL) -> PreparedSourceCache::
  AcquireDenoised miss -> DenoiseModelSession::Denoise -> CoreMlAiDenoisePlatform::CreateSession
  (first run) -> DenoiseBayerMosaic(tile size from UtNet2CoreMlSession::DeviceMemory and
  TileMemory) -> for each tile: UtNet2CoreMlSession::Run -> the session of the tile shape (created
  at its first run: free dimension overrides, Core ML on the Neural Engine) -> float16 input ->
  Ort::Session::Run -> float16 output rectangle to float32 times output_scale, finite check ->
  gain match -> FromDenoisedBayer -> Metal Develop passes (DN5) -> frame
```

**Primary failure call chain**

```text
libonnxruntime.dylib absent or without Core ML -> the process does not start (linked library)
  in a broken bundle; the bundle check of the install lists the missing @rpath library
graph load or run error (Core ML compile error, a node outside Core ML) -> std::runtime_error
  -> DenoiseModelSession: RunFailed, session released -> render error card (DN7)
value that is not finite -> RunFailed naming the tile; slow projected time -> DeviceTooSlow
```

**Implementation steps**

1. The dependency script, the session library, the shared graph code (Windows build and tests
   stay green after the move).
2. `UtNet2CoreMlSession`, `TileMemory`, the macOS platform.
3. The session test for both platforms; the Metal render tests.
4. The timing tool; the measurements on the M4 (time, session creation with and without the
   model cache, footprint, tile memory); `kCoreMlTileMemory` from them.
5. The install of the dylib into the bundle and the bundle dependency check.

**Tests**

| Test | Assertion |
| --- | --- |
| The DN2 and DN3 tests of `utnet2_session_test.cpp` on macOS (`UtNet2CoreMlTest`) | The expected outputs within 7e-3 of the largest value; the load errors (architecture, float32 graph, missing file); the size and the finite checks; the tiled output against the one-pass output; `IsReadyFor` only after a run of the shape; `Release` and a later run |
| The same tests on Windows (`UtNet2DirectMlTest`) | No change after the move of the shared code |
| `UtNet2TileLoopTest` | The loop selects with the table of the session when the options give none |
| `GpuDagMetalDevelopTest` denoised tests (DN5) | The first run of the Metal code of the denoised input |
| `AiDenoiseRenderTest` on Metal | The DN6 renderer tests with the test platform; the RAW file with the FP16 graph on the Neural Engine: the same levels as the CFA render and less noise |

**Exit criteria**

- [x] The tests above pass on the M4 (`build/macos-debug`) and on Windows (`build/debug`).
- [x] The time of the 45.4 MP photo is at most 0.69 s (15% above 0.60 s), or the record states
  the cause.
- [x] The footprint after the release of the sessions returns to the level before the run
  (section 8: a release that does not return the memory stops DN10). The sessions return it;
  the large cache of libmalloc keeps freed blocks of the process (completion record).
- [x] The install puts `libonnxruntime.1.24.4.dylib` into `Contents/Frameworks`, and the bundle
  dependency check passes.

**Expected diff**: 800 to 1200 lines.

##### Phase DN10 completion record (2026-11-23)

**Status:** complete. AI Denoise runs on macOS on Apple silicon: the FP16 graph of DN2 on the
Neural Engine through ONNX Runtime 1.24.4 (C++, Core ML execution provider), one session for each
tile shape, with a Core ML model cache for each model version; the DN5 Metal code builds and
renders the denoised input; the switch of the RAW Decode panel appears on macOS. 45.4 MP in
0.543 to 0.577 s on the MacBook Air M4 (`target_time` 0.60 s). Issue #1069.

**Primary success call chain:**

```text
render with ai_denoise on (MetalRenderer, DecodeRes::FULL) -> PreparedSourceCache::AcquireDenoised
  miss -> DenoiseModelSession::Denoise -> (first run) CoreMlAiDenoisePlatform::CreateSession(graph,
  version) -> PrepareCoreMlModelCache(CoreMlModelCacheRoot(), version) -> UtNet2CoreMlSession
  (graph check with a CPU session that ends at once)
  -> DenoiseBayerMosaic: SelectUtNet2TileSize(session.TileMemory() = kCoreMlTileMemory,
     AvailableTileMemory(DeviceMemory(): hw.memsize, kern.memorystatus_level, held))
  -> for each tile: CheckUtNet2TileRun -> session of the tile shape (first run: free dimension
     overrides height, width, output_height, output_width; Core ML MLProgram CPUAndNeuralEngine,
     no CPU fallback, ModelCacheDirectory <cache>/<version>/<w>x<h>) -> ConvertUtNet2TileInput
     (float16 host buffer) -> Ort::Session::Run -> WriteUtNet2OutputCrop (float32 x output_scale,
     finite check)
  -> gain match -> FromDenoisedBayer -> Metal Develop passes (DN5) -> frame
editor close or GPU release -> DenoiseModelSession::Release -> the sessions of all shapes end
```

**Primary failure call chain:**

```text
graph absent or different -> UtNet2CoreMlSession constructor throws (graph path, first
  difference) -> DenoiseModelSession: RunFailed -> render error card with Turn Off AI Denoise (DN7)
Core ML load or run error -> std::runtime_error naming the graph or the tile -> RunFailed, the
  session released (the next run creates it again)
FP16 overflow -> WriteUtNet2OutputCrop throws (tile, channel, row, column) -> RunFailed
slow projected time -> DenoiseDeviceTooSlowError -> DeviceTooSlow, kept for the device
model version that is not a SHA-256 -> PrepareCoreMlModelCache throws, no folder is touched
ONNX Runtime library of another version or without Core ML -> LoadCoreMlRuntime throws ->
  IsPlatformAvailable false -> no switch (D13)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OutputMatchesPyTorchForEachExpectedOutput` on the Neural Engine (1.31e-3, 1.42e-3, 1.13e-3 of the largest value; tolerance 7e-3) | `UtNet2CoreMlTest` (M4) | PASS |
| `LoadFailsOnDifferentArchitectureMetadata`, `LoadFailsOnFloat32Graph`, `LoadFailsOnMissingFile` | `UtNet2CoreMlTest` | PASS |
| `RunRejectsSizeThatIsNotAMultipleOfSixteen`, `RunThrowsOnAnOutputThatIsNotFinite` (the Neural Engine gives inf, not a saturated value) | `UtNet2CoreMlTest` | PASS |
| `TiledOutputEqualsOnePassOnTheExpectedCrop` (tiled and one pass bit-equal on the Neural Engine) | `UtNet2CoreMlTest` | PASS |
| `SessionIsReadyOnlyForATileShapeThatRan`, `ReleaseFreesTheSessionAndALaterRunLoadsAgain` | `UtNet2CoreMlTest` | PASS |
| `ModelCacheHoldsEachShapeAndGivesTheOutputOfACompiledSession` (new) | `UtNet2CoreMlTest` | PASS |
| `UtNet2CoreMlModelCacheTest.PreparedFolderIsTheOnlyEntryOfTheRoot` (new; rejects `..`, `../other`, upper case) | `UtNet2CoreMlTest` | PASS |
| The same session tests on DirectML after the move of the shared code | `UtNet2DirectMlTest` (RTX 3080 Laptop GPU) | PASS |
| `LoopSelectsWithTheTableOfTheSessionWhenTheOptionsGiveNone` (new) and the DN3 loop tests | `UtNet2TileLoopTest` (M4 and Windows) | PASS 10/10 |
| DN6 loader tests | `AiDenoiseLoaderTest` (M4: 32 pass, 2 skip without the lab packing of a RAW file; Windows) | PASS |
| `FullSizeRenderWithTheSwitchOnRendersTheModelOutputOnMetal`, `ModelErrorIsTheRenderErrorWithItsKindOnMetal`, `RenderThatReadsTheRawReportsItsStepsInOrderOnMetal` (new) | `AiDenoiseRenderTest` (M4) | PASS |
| `RawPhotoRendersWithTheFp16GraphOnCoreMlOnMetal` (new): `_DSC0726.ARW` (42.2 MP), means of the denoised crop 1.022, 0.981, 0.987 of the CFA render, Laplacian 0.0313 to 0.0215 | `AiDenoiseRenderTest` (M4) | PASS |
| The DN5 Metal tests `DenoisedPlaneBecomesTheSensorImageWithTheCfaClamp`, `FullSizeRenderWithTheSwitchOnAndACfaInputReportsTheError`, `DenoisedExpectedCropKeepsTheLevelsAndLowersTheNoiseOfTheCfaRender` (ratios 1.006, 1.007, 1.022; Laplacian 0.0136 to 0.0015): the first build and run of the Metal code | `GpuDagMetalDevelopTest` (M4) | PASS. The suite: 26 pass, 3 skip, 3 fail: `MetalGeometryUsesOneResampleForCropRotationViewportAndScale` and `MetalCameraColorConsumesSharedDualIlluminantTransform` (#869), `DcpProfileMatchesScalarReference` (0.0048 against the threshold 0.005, #972, commented) |
| The AI Denoise suites on Windows after the change of `AiDenoisePlatform::CreateSession` | `UtNet2TileLoopTest`, `UtNet2DirectMlTest`, `AiDenoiseLoaderTest`, `AiDenoiseRenderTest` (CUDA, OpenCL, DirectML) | PASS 62/62 |
| The application links `@rpath/libonnxruntime.1.24.4.dylib` and loads it from the build rpath (dyld) | `alcedo_main` (`build/macos-debug`) | PASS |
| The install puts `libonnxruntime.1.24.4.dylib` into `Contents/Frameworks`; "Verified macOS bundle runtime dependencies"; the executable has the rpath `@executable_path/../Frameworks`; `codesign --verify --deep --strict` | `cmake --install build/macos-release` (bundle) | PASS |

Commands:

```text
Windows: cmd /c "scripts\msvc_env.cmd --build build\debug --target UtNet2TileLoopTest UtNet2DirectMlTest AiDenoiseLoaderTest AiDenoiseRenderTest"
         ALCEDO_AI_DENOISE_TEST_ASSETS=build\tmp\dn6\assets ctest --test-dir build/debug -R "^(UtNet2TileLoopTest|UtNet2DirectMlTest|AiDenoiseLoaderTest|AiDenoiseRenderTest)\." -j 1   # 62/62
M4:      cmake --build build/macos-debug --target UtNet2TileLoopTest UtNet2CoreMlTest UtNet2CoreMlPhotoTimingTool AiDenoiseLoaderTest AiDenoiseRenderTest GpuDagMetalDevelopTest alcedo_main
         ALCEDO_AI_DENOISE_TEST_ASSETS=<DN2 graphs and DN0 outputs> build/macos-debug/alcedo_studio/tests/ai_denoise/<test>
         the timing tool built with -O2 from the same sources (the release tree builds no tests)
```

Suite totals: see the table. Full suite: not run (AGENTS.md).

**Measurements (MacBook Air M4, 16 GB, macOS 27.0.1, thermal state nominal, tool with -O2):**

| Photo, tiles of 1536 | Tile loop, ready sessions | First run of the photo size, no model cache (session creation) | First run, model cache filled (session creation) |
| --- | ---: | ---: | ---: |
| 24.3 MP | 0.336 to 0.342 s | 1.80 s (1.44 s) | 0.43 s (0.080 s) |
| 36.2 MP | 0.451 to 0.474 s | 1.62 s (1.15 s) | 0.49 s (0.040 s) |
| 45.4 MP | **0.543 to 0.577 s** | 1.90 s (1.31 s) | 0.59 s (0.035 s) |

- Lab (DN1 addition, 3-crop graph): 0.37, 0.51, 0.60 s. The DN2 graph in Alcedo Studio is faster
  by 7% to 10%; `target_time` (0.60 s) is met without the margin of 15%.
- Tile sizes at 45.4 MP: 1536 0.55 s, 1024 0.68 s, 768 0.79 s; footprint growth of the sessions
  and the runs 402, 146, 108 MiB (`kCoreMlTileMemory` 416, 160, 112 MiB).
- Model cache: 30 MB on disk for each tile shape (297 MB for the 10 shapes of the three sizes).
- The RAW file through the Metal renderer (debug build): first render with the model and a cold
  cache 2.8 s; a model run on ready sessions 0.51 to 1.09 s in total (tile loop 0.47 to 0.55 s;
  the rest is the input preparation and the plane assembly of the debug build).

**Footprint after the release (section 8, stop condition):** the sessions return their memory;
the memory that stays belongs to the allocator.

- Default settings: the footprint after `Release` stays at 1.1 to 1.9 GiB. `vmmap` shows it as
  `Malloc Large (empty)` (954 MiB after a 24.3 MP and a 45.4 MP photo): freed large blocks that
  libmalloc keeps for reuse. No Core ML, IOSurface, or ONNX Runtime region remains. The amount is
  bounded and reused: 9 photos in 3 rounds keep 2076 MiB; with a release after each photo it
  stays between 1.1 and 1.9 GiB.
- `MallocLargeCache=0`: 152 MiB after the release of two photos (11 to 41 MiB after the graph
  check), and the same footprint after each photo of 8 photos with a release after each: no
  growth for each session. About 110 to 160 MiB stay with the Core ML and ONNX Runtime runtime.
- `MallocDeferredReclaim=0` alone, `malloc_zone_pressure_relief`, and a simulated memory pressure
  (`memory_pressure -S -l warn`) do not change it.
- In the timing tool most of the cached blocks are the photo and output images of the tool. The
  application keeps the large cache of libmalloc for all its allocations; DN10 does not change
  the allocator of the process.

**Changes against the plan text and detail choices:**

- **Shared code in two files**: `utnet2_tile_io` (tile checks, float16 input, output rectangle,
  OpenCV only, in `UtNet2TileLoop`) and `utnet2_onnx_graph` (the graph check, ONNX Runtime
  headers, compiled into each platform library). The plan named one file.
- **Model cache used** (session creation 95% shorter): `CoreMlModelCacheRoot()` is
  `alcedo-studio/ai-denoise-coreml` in the cache folder of the user that macOS manages
  (`_CS_DARWIN_USER_CACHE_DIR`; macOS empties it when the disk is low) in place of
  `~/Library/Caches/<bundle identifier>`, so the session library needs no bundle API.
  `PrepareCoreMlModelCache` keeps only the folder of the installed version, and each tile shape
  has its own folder: the Core ML execution provider keys its cache on the graph, so one folder
  for all shapes loaded the model of another shape ("MultiArray shape (1 x 4 x 1536 x 1440) does
  not match the shape (1 x 4 x 1536 x 1536)"). `AiDenoisePlatform::CreateSession` gets the model
  version for it.
- **Memory of the tile selection**: `kern.memorystatus_level` (the free percentage of
  `memory_pressure`: free, inactive, purgeable, and file-backed pages) in place of the free and
  inactive pages of `vm_statistics64`, which leave out the memory that macOS gives back without
  pressure.
- **The graph check at construction** uses a CPU session with graph optimization off that ends
  at once: a Core ML session needs a fixed shape, and a free-shape Core ML session with
  `session.disable_cpu_ep_fallback` fails.
- **The memory error text** is "not enough device memory" (was "video memory"; the Mac has
  unified memory).
- `std::from_chars` of a double needs macOS 26; the graph check reads `output_scale` with a
  stream in the classic locale on both platforms.
- The two DN7 sensor layout tests skip when the sample RAW files (local files of the developer)
  are absent, as the other sample tests do.
- `libonnxruntime.1.24.4.dylib` is built for macOS 14.0; the bundled Qt and OpenCV of Homebrew
  are too, so the application needs macOS 14 already.

**Checklist / exit condition:** all four exit criteria are met: the tests pass on the M4 and on
Windows (the failures of `GpuDagMetalDevelopTest` are #869 and #972); 45.4 MP in at most 0.577 s;
the sessions return their memory (the large cache of libmalloc above); the install and the bundle
dependency check pass.

**LOC note:** new source files below 350 lines (the session test 387); `utnet2_directml_session.cpp`
853 to 688 lines after the move of the shared code. About 1500 added lines with the plan.

**Remaining gaps:** the switch and a render were not checked by eye in the running application
on the Mac; no Mac with 8 GB or another chip was measured; the 3 known failures of
`GpuDagMetalDevelopTest` (#869, #972).

### DN12: feed with more than one package, AI Denoise model package

Written on 2026-11-23, before the implementation (issue #1072). The fixed points are the DN12
bullet of "DN11 to DN13", D3 (the publication is the last product phase), D10 (the model version
is the SHA-256 of the graph), and the AI Models plan
([external_ai_model_download_plan.md](../../ai/external_ai_model_download_plan.md), P1 to P5 and
AM2). The user asked on 2026-11-23 that the download of the released builds stays compatible.

**Objective and deliverables**

- The AI Denoise package `alcedo-ai-denoise`, model set version 1, revision r1, for
  `windows-x86_64` and `macos-arm64`, built from a model set definition with pinned files, as the
  AI Mask package is.
- A feed with more than one package: `prepare_ai_model_packages.py` builds one package and carries
  every other entry of the verified live feed; `publish_ai_model_packages.py` uploads the new
  archives only and checks that the carried archives are on the server.
- `AiModelPackageService` knows the AI Denoise package; Settings > AI Models shows its card.
- The render reads the graph from the installed package. The hand-copied folder `ai-denoise-v1`
  is no longer read.
- The feed published on R2, after the approval of the user (AM2 rule: the agent asks before it
  uploads).

**Inputs and prerequisites**

DN2 (the FP16 graph, 15,552,242 bytes, SHA-256
`0bc98084c9bff16d489945ec357fd008465414813e5330f4a6077f205b668469`, the lab release
`export-utnet2-bayer-dark-loss-10k`), DN7 (the switch: a missing model opens the AI Models page),
the AM1 to AM5 code, and the live feed of sequence `20261028041321` (one entry, AI Mask v1 r1).

**Compatibility with the released builds (user, 2026-11-23)**

Alcedo Studio 0.3.4, 0.3.5, and 0.3.6 read the same feed URL. Their feed parser, installer, and
service are the code of `main` (no change since `v0.3.4`), so the rules are:

- The AI Denoise entry uses only the fields and the limits of feed schema 1 (`schema` stays 1):
  an HTTPS `.7z` archive URL on the feed host, a revision `r<N>`, at least one license, the
  artifact fields. A released build parses the whole feed, so an entry that it rejects would stop
  its AI Mask download too.
- A released build lists only the packages that it knows (`kKnownPackages`), so it ignores the
  entry `alcedo-ai-denoise`.
- The AI Mask entry is carried with the same values (the same archive URLs, sizes, and SHA-256),
  so an installed AI Mask set stays `current` and a new download gets the same archive.
- The sequence of the new feed is above the live sequence; the released builds raise their
  trusted sequence to it.
- Projects keep their model version: the package holds the graph file of DN2, whose SHA-256 is
  the version that the switch records (D10).

**Product rules**

- **Package content.** `model_set.json` (role `denoise_graph`), `package-inventory.json`,
  `NOTICE.txt`, `utnet2_bayer_dark_loss_10k_fp16.onnx` (the same file on both platforms: one
  FP16 graph, D2), `licenses/GPL-3.0.txt` (the root `LICENSE` without the additional permission,
  as in the AI Mask package), and `licenses/CC-BY-4.0.txt`. The feed licenses are `GPL-3.0` and
  `CC BY 4.0`, the licenses of the lab weights (lab `docs/alcedo_integration.md`). `NOTICE.txt`
  names the lab, the checkpoint, the FP16 export, the public RawNIND weights that the fine-tune
  starts from (GPL-3.0 and CC BY 4.0), and the RawNIND dataset (CC BY-SA 4.0; the lab treats the
  weights as adapted material under GPL-3.0).
- **Installation.** `<model folder>/alcedo-ai-denoise-v1/` (AM1 rules: staging, receipt,
  commit point). The build supports model set version 1 only.
- **Graph location.** The render takes the file of the role `denoise_graph` of `model_set.json`
  of a complete supported installation. Not installed, an installation below or above the
  supported versions, an incomplete installation, and a `model_set.json` without the role are
  `ModelNotInstalled` errors (DN7: the card opens the AI Models page), each with its reason.
- **AI Models page.** The first section is named "Editor Models" and holds the AI Mask card and
  the AI Denoise card (two columns), in the order of the service. Each card has its own
  description, project link (`zidage/alcedo-denoise-lab` for AI Denoise), and removal texts; the
  license link opens the file of the installed package `<id>-v<version>`. The download statement
  stays at version 1: its text covers every model of the page ("Each model has its own license").
- **Feed preparation.** A run builds one definition. The base feed is the live feed (or a local
  copy, or none for a first feed with `--no-base-feed`); its signature must verify with the update
  key. Every base entry with another (`id`, `model_set_version`) is carried; the entry of the
  same (`id`, `model_set_version`) is replaced (a new revision). The exact base files are copied
  to `<output>/base/`. The sequence must be above the base sequence.
- **Publication.** An artifact whose archive is in `<output>/packages/` is verified and uploaded;
  any other artifact must belong to an entry equal to an entry of the verified base feed (the
  same JSON values). With `--upload`, a HEAD request of each carried archive must return 200 with
  the signed size before the first upload.

**Modules, files, and APIs**

| File | Responsibility |
| --- | --- |
| `scripts/ai_models/ai_denoise_model_set_v1.json`, `ai_denoise_model_set_v1_NOTICE.txt` (new) | The definition and the notice of the AI Denoise package |
| `third_party_licenses/RawNIND-weights-CC-BY-4.0.txt` (new), `THIRD_PARTY_NOTICE.txt` | The CC BY 4.0 text (legalcode.txt of Creative Commons) |
| `scripts/ai_models/ai_model_packages.py` | `load_base_feed`, `merge_feed_entries` |
| `scripts/ai_models/prepare_ai_model_packages.py` | `--source PLATFORM=DIR`, `--base-feed`, `--no-base-feed` |
| `scripts/ai_models/publish_ai_model_packages.py` | Carried entries: no upload, the HEAD check |
| `include/app/ai_model_package_install.hpp`, `app/ai_model_package_install.cpp` | `kAiDenoiseModelPackageId`, its version range, `ReadAiDenoiseModelInstallation`, `ResolveAiDenoiseGraph` |
| `app/ai_model_package_service.cpp` | The second known package |
| `include/edit/input/ai_denoise_model_session.hpp`, `edit/input/ai_denoise_model_session.cpp` | `SetGraphLocator` in place of `SetModelFolderProvider` |
| `ui/alcedo_main/album_backend/application_module_host.cpp` | The locator from the model folder of the settings |
| `ui/alcedo_main/qml/AiModelsSettingsPanel.qml`, `i18n/alcedo_main_zh_CN.ts` | The section and the AI Denoise card |
| `alcedo_studio/tests/resources/ai_model_packages/` | The fixture feed with the two packages (the AI Denoise entry built over the AI Mask feed) |

**Primary success call chain**

```text
prepare_ai_model_packages.main --definition ai_denoise_model_set_v1.json
  -> load_base_feed (live manifest + .sig, update key) -> build_package per platform
  -> merge_feed_entries (AI Mask v1 carried, AI Denoise v1 added) -> sign -> verify
publish_ai_model_packages.main --upload -> carried archives: HEAD 200 and size
  -> new archives -> archived .sig + manifest -> live .sig -> live manifest
Settings > AI Models -> CheckPackages -> AI Denoise card -> Download -> alcedo-ai-denoise-v1/
render with ai_denoise -> DenoiseModelSession::InstalledModelVersion -> graph locator
  -> ResolveAiDenoiseGraph (complete installation, role denoise_graph) -> SHA-256 -> run
```

**Primary failure call chain**

```text
base feed signature invalid, or sequence not above the base -> prepare stops, no manifest
carried entry differs from the base, or a carried archive is absent on the server
  -> publish stops before the first upload
AI Denoise package absent, incomplete, or of an unsupported version
  -> ResolveAiDenoiseGraph reason -> AiDenoiseError ModelNotInstalled -> card "Open AI Models"
```

**Tests**

| Test | Assertion |
| --- | --- |
| Python: the AI Denoise definition | It pins the DN2 graph (size and SHA-256) on both platforms and the two license texts; the notice names the files, the licenses, RawNIND, and its dataset license |
| Python: a feed over a base feed | The base entry is carried with the same values; a base entry of the same (`id`, `model_set_version`) is replaced; a base feed with an invalid signature, or a sequence that is not above the base, stops the preparation |
| Python: the publication of a feed with a carried entry | The plan uploads only the new archives; a carried entry that differs from the base stops the publication |
| Python: the C++ fixture | The fixture is the current Python output (two packages) |
| `AiModelPackageManifestTest` | The two-package fixture verifies |
| `AiModelPackageServiceTest` | The service lists AI Mask and AI Denoise; a feed with both offers each; the AI Mask comparison and offer are the same with and without the AI Denoise entry (released builds); the Python AI Denoise package installs and its graph resolves on each platform |
| `AiModelPackageServiceTest` (graph location) | Not installed, incomplete, below and above the range, no `denoise_graph` role: no graph, each with its reason; a complete installation: the graph path |
| `AiDenoiseModelSessionTest`, `AiDenoiseRenderTest` | The tests run with the graph locator |
| `AiModelsSettingsQmlTest` | The existing page tests with the two cards of the fixture |

**Exit criteria**

- [x] The tests above pass in `build/debug`; the Python tests pass.
- [x] The prepared feed carries the live AI Mask entry with the same values and verifies with the
  update key.
- [x] The feed is published with the approval of the user; the public manifest and signature are
  the prepared bytes, and the public AI Denoise archives pass `verify_package_archive`.

**Expected diff**: 900 to 1400 lines.

##### Phase DN12 completion record (2026-11-23)

**Status:** complete. The feed of sequence `20261123220131` is live (uploaded with the approval
of the user on 2026-11-23, AM2 rule). The AI model feed lists the AI Denoise
package `alcedo-ai-denoise` v1 r1 next to the AI Mask set; Settings > AI Models shows its card;
the render reads the graph from `<model folder>/alcedo-ai-denoise-v1/`.

**Primary success call chain:**

```text
prepare_ai_model_packages.main --definition ai_denoise_model_set_v1.json --source PLATFORM=DIR
  -> load_base_feed (live manifest + .sig, verify_signature_bytes with the update key)
  -> sequence above the base -> base files to <output>/base/
  -> build_package per platform (pins, licenses, NOTICE, inventory, 7z, verify_package_archive)
  -> merge_feed_entries (AI Mask v1 r1 carried, AI Denoise v1 r1 added) -> manifest_bytes -> sign
publish_ai_model_packages.main -> verify_prepared_output (signature; new archives verified;
  entries equal to a base entry are carried) -> planned_uploads (no carried archive)
  -> --upload: check_carried_archives (HEAD 200 + size) -> publish_object ... live manifest last
AiModelPackageService (kKnownPackages: AI Mask, AI Denoise) -> CheckPackages -> card -> Install
  -> InstallAiModelPackageArchive -> alcedo-ai-denoise-v1/ (installed.json = commit point)
Renderer (ai_denoise on) -> DenoiseModelSession::InstalledModelVersion -> GraphPath
  -> graph locator (application_module_host) -> ResolveAiDenoiseGraph(model folder)
  -> ReadAiDenoiseModelInstallation -> AiModelSetFilePath(role denoise_graph) -> SHA-256 -> run
```

**Primary failure call chain:**

```text
base signature invalid / sequence not above the base -> prepare exit 1, no manifest
an entry without archives in the output that is not a base entry -> publish raises before uploads
a carried archive not on the server or of another size -> publish raises before the first upload
no folder, not installed, incomplete, unsupported version, no denoise_graph role
  -> AiDenoiseGraphLocation.problem -> AiDenoiseError ModelNotInstalled ("AI Denoise: the model is
     not installed: <reason>; see Settings > AI Models") -> card with "Open AI Models" (DN7)
```

**What was proven (executed tests):**

| Required test | Test | Result |
| --- | --- | --- |
| The AI Denoise definition | `AiDenoiseModelSetDefinitionTest` (2 tests): the DN2 graph (15,552,242 bytes, the SHA-256 of this plan) on both platforms, the GPL-3.0 and CC BY 4.0 texts, the notice | PASS |
| A feed over a base feed | `AiModelFeedWithMoreThanOnePackageTest.test_FeedOverBaseCarriesTheBaseEntryUnchanged`, `...test_EntryOfTheSameVersionIsReplaced`, `...test_InvalidBaseSignatureOrSequenceStopsPreparation` | PASS |
| The publication of a feed with a carried entry | `...test_PublicationUploadsOnlyTheNewArchivesAndChecksCarriedEntries` (plan, HEAD check, a changed carried entry) | PASS |
| The C++ fixture is the current Python output | `AiModelPackageCppFixtureTest.test_CppFixtureIsTheCurrentPythonPreparedFeed` (two packages) | PASS |
| Earlier tooling tests | the 10 AM2 tests (`--no-base-feed` added to their preparation) | PASS |
| The two-package fixture verifies in C++ | `AiModelPackageManifestTest.PythonPreparedFeedVerifiesInCpp` | PASS |
| The service lists both packages | `AiModelPackageServiceTest.StartupReadsLocalInstallationWithoutRequestingFeed` | PASS |
| Released builds keep their AI Mask state | `AiModelPackageServiceTest.AiDenoiseEntryKeepsTheAiMaskStateOfReleasedBuilds`: the parser and the AI Mask comparison of the released builds give `current` and the same offer with and without the AI Denoise entry | PASS |
| The Python AI Denoise package installs and its graph resolves (both platforms) | `AiModelPackageServiceTest.PythonPreparedAiDenoisePackageInstallsAndItsGraphResolves` | PASS |
| The graph location | `AiModelPackageServiceTest.AiDenoiseGraphComesOnlyFromACompleteSupportedInstallation` (no folder, not installed, the hand-copied `ai-denoise-v1`, complete, incomplete, no role, v2 only) | PASS |
| The session and render tests with the graph locator | `AiDenoiseLoaderTest`, `AiDenoiseRenderTest` (the 4 tests of the lab assets skip without `ALCEDO_AI_DENOISE_TEST_ASSETS`) | PASS |
| The AI Models page | `AiModelsSettingsQmlTest` (the existing tests with the two cards of the fixture) | PASS |

Commands: `python -m unittest discover -s scripts/ai_models/tests` (16 tests);
`cmd /c "scripts\msvc_env.cmd --build build\debug --target AiModelPackageManifestTest
AiModelPackageServiceTest AiDenoiseLoaderTest AiDenoiseRenderTest AiModelsSettingsQmlTest
SimplifiedChineseCatalogTest alcedo_main"`; `ctest --test-dir build/debug -j 1 -R
"^(AiModelPackageManifestTest|AiModelPackageServiceTest|AiDenoiseLoaderTest|AiDenoiseRenderTest|AiModelsSettingsQmlTest|SimplifiedChineseCatalogTest)\."`
(94 tests: 89 pass, 4 skip, 1 fails: the `CollectionsPanel` count of
`SimplifiedChineseCatalogTest`, issue #1012, before this change).

**The prepared feed (2026-11-23):** sequence `20261123220131`, over the live feed
`20261028041321` (signature verified with `update_public_key.txt`). The AI Mask entry has the
same values as in the live feed (compared as JSON). AI Denoise v1 r1: 5 files (the graph,
`model_set.json`, `NOTICE.txt`, `licenses/GPL-3.0.txt`, `licenses/CC-BY-4.0.txt`); Windows
archive 14,017,034 bytes, SHA-256
`9f6c19c9aadfb497905497e26fbfbc1bf8e6650670c9a2b13c45ac7216c39278`; macOS archive 14,017,779 bytes,
SHA-256 `ff2e54f359414a5806760f3c0fc5383edfe05a57723712ec83896f02084682b5`. The source graph is the
DN2 file (`0bc98084...`), so the projects keep their model version. The publication plan uploads
the two AI Denoise archives, the archived pair, and the live pair; the AI Mask archives are carried.

**Publication (2026-11-23, approved by the user):** `publish_ai_model_packages.py --upload`
uploaded the two AI Denoise archives, the archived pair, and the live pair (live manifest last).
Downloaded again from `static.aoraw.org`: the public `manifest.json` and `.sig` are the prepared
bytes and the signature verifies with the update key; both AI Denoise archives pass
`verify_package_archive`; the carried AI Mask archives answer HEAD with 200 and their signed size.

**Changes against the plan text and detail choices:**

- The AI Denoise entry is checked with the parser of the released builds through the code of
  `main` (the feed code is unchanged since `v0.3.4`), not with a released binary.
- `--windows-source` and `--macos-source` stay as the short forms of `--source PLATFORM=DIR`.
- The AI Models page object names `aiModelsMask*` are now `aiModelsPackage*` (one card per package).
- The model folder description and the "not available" text name AI Denoise; the download statement
  stays at version 1.
- `THIRD_PARTY_NOTICE.txt` item 35 and `third_party_licenses/RawNIND-weights-CC-BY-4.0.txt` (the
  `legalcode.txt` of Creative Commons, downloaded on 2026-11-23).

**Checklist / exit condition:** all exit criteria are met: the tests pass (except #1012); the
feed carries the AI Mask entry unchanged and verifies with the update key; the feed is published
and the public files verify.

**Remaining gaps:** the card and an install from the live feed were not checked in the running
application; the macOS build was not built for this phase (no macOS-only code changed).

### DN13: alcedo-cli commands and user documentation

Written on 2026-11-23, before the implementation. The user asked on 2026-11-23 that `alcedo-cli`
exposes AI Denoise, and that the alcedo agent skill uses it on its own only when the ISO is high
and the noise clearly affects the picture.

**Objective and deliverables**

- `denoise.status` and `denoise.set` of `alcedo-cli` for the photo that the editor has open.
- The AI Denoise error of a frame as structured error data (`data.ai_denoise_error`) in every
  command that reports a render error.
- The user documentation `docs/ai_denoise.md`.
- The command reference `denoise.md`, the rule of the agent skill, a workflow, and the limits in
  [alcedo-skills](https://github.com/zidage/alcedo-skills).

**Inputs and prerequisites**

DN7 (`EditorAiDenoiseControl`, the `raw_decode` key `ai_denoise`, the AI Denoise error of the
editor session), DN8 (several photos through Copy and Paste of the RAW Decode item, the export
item status `ai_denoise`), DN12 (the model package), the automation command registry and
`OperationWait` of AU10.

**Product rules**

- **One photo, one switch.** `denoise.set` is the switch of the RAW Decode panel: one
  `raw_decode` commit with the author `agent` and the `note`, on with the installed model version
  (D10), off with no version. It answers after the frame of the commit. Several photos use
  `adjustments.copy` and `adjustments.paste` of the `raw_decode` item (2.2); the CLI has no batch
  command of its own (user rule of DN8).
- **Refusals before a change** (`-32002`, `data.refusal`): `platform_unavailable` (D13),
  `model_not_installed`, `model_unreadable`, `sensor_not_supported` for a known sensor that is
  not Bayer (D14; the switch of the panel is disabled for it). An unknown sensor is accepted, as
  by the panel; the render reports `sensor_not_supported`.
- **A failed run keeps the commit**, as a failed frame of `editor.set` does: `-32007` with
  `head_commit`, `enabled`, and `ai_denoise_error`. `denoise.set` with `enabled: false` is the
  `Turn Off AI Denoise` of the agent (2.4, 1.4: the CLI renders no photo without the model while
  the switch is on).
- **The agent skill.** The skill turns AI Denoise on when the user asks for noise reduction. On
  its own only when both hold: `iso` of `library.info` is 3200 or more, and a preview of a
  detail at about full resolution shows noise that clearly hurts the picture. It reads
  `denoise.status` first, does not install the model, compares the same region before and after,
  turns the switch off at once after an AI Denoise error, and pastes the switch only onto the
  photos that meet the rule.

**Files**

- `src/include/ui/alcedo_main/automation/automation_denoise_commands.hpp`,
  `src/ui/alcedo_main/automation/commands/denoise_commands.cpp` (new)
- `src/include/ui/alcedo_main/album_backend/editor_ai_denoise_control.hpp`,
  `src/ui/alcedo_main/album_backend/editor_ai_denoise_control.cpp`: `ReadInstalledModel`,
  `PrepareTurnOn`, `TurnOffWrite`; `turnOn` and `turnOff` use them.
- `src/include/ui/alcedo_main/automation/automation_command_support.hpp`, `.cpp`:
  `AutomationOwnerErrorData`.
- `src/ui/alcedo_main/automation/commands/editor_commands.cpp`: the committed failure of
  `editor.set`, `editor.undo`, `editor.redo`, and `editor.batch_set` carries it.
- `automation_host_session.cpp`, `src/ui/alcedo_main/CMakeLists.txt`.
- `tests/automation/automation_denoise_commands_test.cpp` (new), `automation_in_process_session.hpp`,
  `tests/automation/CMakeLists.txt`, `tests/ui/editor_ai_denoise_control_test.cpp`.
- `docs/ai_denoise.md` (new).
- alcedo-skills: `SKILL.md`, `references/reference/denoise.md` (new), `README.md`,
  `commands.json`, `references/workflows.md`, `references/limits.md`, `references/cli.md`,
  `references/reference/editor.md`, `references/reference/render.md`.

**Required tests**

| Test | Assertion |
| --- | --- |
| `EditorAiDenoiseControlTest.PrepareTurnOnGivesWriteOfInstalledVersionWithoutSubmit` | The write of on has the installed version, off has none, nothing is submitted |
| `EditorAiDenoiseControlTest.PrepareTurnOnRefusesPlatformBeforeReadingModel` | No platform: refusal without a model read; the model is still readable |
| `EditorAiDenoiseControlTest.ReadInstalledModelReportsMissingAndUnreadableModels` | The two refusals and their text |
| `AutomationDenoiseCommandsTest.StatusWithoutPhotoReportsPlatformAndMissingModel` | `available`, `model_not_installed`, `photo` null; `denoise.set` is `-32001` |
| `AutomationDenoiseCommandsTest.StatusReportsVersionOfInstalledGraph` | The version is the SHA-256 of the graph file |
| `AutomationDenoiseCommandsTest.SetRejectsUnknownAndMissingParameters` | `-32602` |
| `AutomationDenoiseCommandsTest.TurnOnWithoutModelIsRefusedWithoutCommit` | The refusal of the platform or of the missing model; no commit; the photo of a CI ARW is `bayer` |
| `AutomationDenoiseCommandsTest.FailedModelRunKeepsCommitAndReportsErrorUntilTurnedOff` | A graph that is not ONNX: `-32007` with `run_failed`, one agent commit with the note; `denoise.status` and `render.preview` report the error; off renders and clears it |
| `AutomationDenoiseCommandsTest.TurnOnWithModelRendersDenoisedPhoto` | With the DN2 graph: on with the version, no commit for a second write, `render.preview` writes the file |

**Exit criteria**

The tests pass on Windows (DirectML); the reference pages pass
`generate_cli_reference.py --check` with the `commands.json` of this build; the skill text
states the rule of the user.

##### Phase DN13 completion record (2026-11-23)

**Status:** complete (issue #1074). `alcedo-cli` has `denoise.status` and `denoise.set`; a failed
AI Denoise run reaches an agent as `data.ai_denoise_error`; `docs/ai_denoise.md` is the user
page; the alcedo skill has the command reference and the rule of the user.

**Primary success call chain:**

```text
alcedo-cli call denoise.set {"enabled":true,"note":...}
  -> AutomationCommandRegistry::Dispatch (GUI: agent control) -> denoise.set handler
  -> EditorAiDenoiseControl::sensor_layout (known and not Bayer: refusal)
  -> EditorAiDenoiseControl::PrepareTurnOn -> available() -> ReadInstalledModel
     -> DenoiseModelSession::InstalledModelVersion (SHA-256 of the graph)
     -> DevelopRawDecodeUpdate{ai_denoise = {version}}
  -> FieldOwnerParameterTarget(document, selected node, "raw_decode") (the Develop node)
  -> EditorSessionController::EnqueueTargetedFieldWrite(settled, attribution agent + note)
  -> OperationWait (commit, then a presented frame of the render, which runs the model in the
     loader, DN6) -> AfterQueuedChanges
  -> result {photo: PhotoJson (RecordedModelVersion from the Develop JSON, the error of
     IEditorSessionBackend::ai_denoise_error), head_commit}
denoise.status -> available(), ReadInstalledModel, PhotoJson of the open photo
Several photos: adjustments.copy {"node_id":"develop","item":"raw_decode"} -> adjustments.paste
```

**Primary failure call chain:**

```text
no editor photo -> -32001
platform without a device / no model / unreadable graph / known sensor not Bayer
  -> -32002 data.refusal (platform_unavailable, model_not_installed, model_unreadable,
     sensor_not_supported); nothing is written
model run fails in the frame -> EditorSessionRenderController sets the AI Denoise error
  -> OperationWait committed failure -> SendSetFailure -> -32007 with data.head_commit,
     data.enabled true, data.ai_denoise_error {kind, text}
  -> agent: denoise.set {"enabled":false} -> frame without the model -> error cleared
editor.set / editor.batch_set / render.preview while the run fails
  -> AutomationOwnerErrorData(message) -> ParseAiDenoiseRenderError -> data.ai_denoise_error
```

**What was proven (executed tests):**

| Required test | Target | Result |
| --- | --- | --- |
| `PrepareTurnOnGivesWriteOfInstalledVersionWithoutSubmit`, `PrepareTurnOnRefusesPlatformBeforeReadingModel`, `ReadInstalledModelReportsMissingAndUnreadableModels` (and the 6 DN7 tests) | `EditorAiDenoiseControlTest` | 9/9 PASS |
| `StatusWithoutPhotoReportsPlatformAndMissingModel`, `StatusReportsVersionOfInstalledGraph`, `SetRejectsUnknownAndMissingParameters`, `TurnOnWithoutModelIsRefusedWithoutCommit` | `AutomationDenoiseCommandsTest` | PASS |
| `FailedModelRunKeepsCommitAndReportsErrorUntilTurnedOff` (DirectML, a graph that is not ONNX: ONNX Runtime "Protobuf parsing failed", `run_failed`) | `AutomationDenoiseCommandsTest` | PASS (ran, not skipped) |
| `TurnOnWithModelRendersDenoisedPhoto` (the DN2 graph of the lab, CUDA editor backend, DirectML) | `AutomationDenoiseCommandsTest` | PASS (ran, not skipped) |
| Callers of the changed code: `AutomationAdjustmentCommandsTest`, `AutomationProjectCommandsTest`, `AutomationLibraryCommandsTest`, `AutomationCommandRegistryTest`, `AutomationEditorCommandsTest`, `ExportAiDenoiseControlTest` | | PASS |
| `AutomationReferenceDocsTest` (2 tests) | compares with the checkout of `ALCEDO_SKILLS_DIR` (another alcedo-skills branch without `denoise.*`) | FAIL as expected; its build copy gave the `denoise.*` entries of `commands.json` |
| `generate_cli_reference.py --check` on the alcedo-skills branch | | the `denoise.*` blocks are current; it fails only on `labels.*` without pages, as on alcedo-skills `main` (zidage/alcedo-skills#24) |

Commands: `cmd /c "scripts\msvc_env.cmd --build build\debug --target EditorAiDenoiseControlTest
AutomationDenoiseCommandsTest AutomationReferenceDocsTest ..."`; with
`ALCEDO_AI_DENOISE_TEST_ASSETS=D:\Projects\alcedo-denoise-lab\models\export\dark_area_loss`:
`ctest --test-dir build/debug -j 1 -R "^AutomationDenoiseCommandsTest\."` (6/6) and the caller
suites (65 tests: 63 pass, the 2 reference tests above).

**Changes against the plan text and detail choices:**

- The commands work on the open editor photo only. A batch command was not added: several
  photos use Copy and Paste of the `raw_decode` item (2.2, the user rule of DN8).
- `PhotoJson` reads the AI Denoise error from the session backend, not from the `aiDenoiseError`
  property of the controller: the property follows at the next change notification, after the
  frame that answers `denoise.set` (found by the first run of
  `FailedModelRunKeepsCommitAndReportsErrorUntilTurnedOff`).
- `ai_denoise_error` is added to the owner error data of every command
  (`AutomationOwnerErrorData`), so `render.preview`, `editor.set`, `editor.undo`, `editor.redo`,
  and `editor.batch_set` report it without changes of their own.
- The threshold of the skill rule is ISO 3200 or more, together with noise that the agent sees in
  a detail at about full resolution. The skill does not install the model.
- The `commands.json` of the alcedo-skills branch gets only the `denoise.*` entries; the other
  commands of this build that skills `main` lacks belong to their own skills branches.
- The reference examples show the commands without a recorded output: the release build was not
  built for this phase.

**Checklist / exit condition:** the required tests pass on Windows (DirectML); the reference
blocks of `denoise.*` are current; the skill states the rule of the user.

**Remaining gaps:** no run of the commands from the `alcedo-cli` program against a release
build (the in-process registry path is the path of every socket request); the macOS build was
not built for this phase (no macOS-only code changed).

### DN11 to DN13

Each of these phases gets its detailed section before its implementation starts (DN10 has its section above, 2026-11-23). DN4 and DN9
are removed (2026-11-22, D6). The fixed points that the detailed sections must keep:

- **DN10, DN11**: the same expected outputs, tolerances, FP16 graph, and tile planner as DN2 and
  DN3. The control of 2.1 appears on a platform in the phase that completes its forward code
  (D13).
- **DN10** (U5): FP16 on the Neural Engine, the configuration of the DN1 addition "FP16 on the
  Neural Engine". `UtNet2CoreMlSession` (`decoders/processor/nn`, Apple silicon only) with the
  Core ML execution provider (`ModelFormat` MLProgram, `MLComputeUnits` CPUAndNeuralEngine,
  `session.disable_cpu_ep_fallback` 1) and the FP16 graph of DN2. One session for each tile
  input shape (the shape fixed with the free dimension overrides): a session with free height
  and width puts half of the operations on the CPU. `DenoiseModelSession` keeps the sessions of
  the photo sizes that the editor and the export use until it releases them (2.3). The session
  converts and checks the tiles as `UtNet2DirectMlSession` does, and has the same `Run` and
  `Release` interface, so the tile loop of DN3 runs on both. Tiles of 1536 (peak footprint 2.0
  GiB at 45.4 MP; 1024: 1.7 GiB). The dependency is `libonnxruntime.dylib` of the osx-arm64
  folder of `Microsoft.ML.OnnxRuntime` 1.24.4 (the version of Windows), downloaded with its
  SHA-256 and copied into `Contents/Frameworks` of the bundle.
  - Tests: the tests of DN2 and DN3 on macOS with the FP16 tolerance (7e-3 of the largest
    expected value; measured 1.0e-3 to 1.5e-3 on the Neural Engine); the footprint of the
    process after the release of the sessions.
  - Time of the DN1 addition: 0.37, 0.51, 0.60 s for 24.3, 36.2, 45.4 MP on the MacBook Air M4.
    `target_time` on macOS is 0.60 s at 45.4 MP (U1); the DN10 time may be at most 15% above
    it (0.69 s). The slow-device check of DN3 runs on macOS too.
    Session creation 0.28 to 0.46 s for each of the 4 tile shapes of a photo size; DN10 measures
    `ModelCacheDirectory` for it, because a new photo size in the editor creates new sessions.
- **DN11** (last; the only Linux environment of the user is WSL): before own OpenCL kernels, DN11 measures the WebGPU execution provider of ONNX
  Runtime on Vulkan with the FP16 graph (`Microsoft.ML.OnnxRuntime.EP.WebGpu` has linux-x64; on
  macOS, WebGPU took 2.6 times the Core ML GPU time in FP32). The CUDA rows of section 4 are the
  starting point of own kernels.
- **DN12** (complete, section above): the feed keeps the AI Mask entry when the denoise entry is
  added. The `sequence` of the feed increases. The denoise package holds the FP16 graph.

## 8. Risks and stop conditions

| Risk | Detection | Response |
| --- | --- | --- |
| The measured overlap is near the theoretical 100 packed pixels, so the tile overhead is 1.6 to 2.8 times | DN1 border table | Use the largest tile that the GPU memory permits; give the user the corrected time estimate before DN3 |
| `onnxruntime-directml` 1.24.4 is the last DirectML package of ONNX Runtime | NuGet and PyPI package lists | Keep the pinned version. The graph uses 6 standard operators of opset 17 (Conv, LeakyRelu, MaxPool, ConvTranspose, Concat, DepthToSpace) |
| The DirectML memory arena keeps its memory while the session lives (FP32: up to 3.3 GiB; FP16: 2.7 GiB with tiles of 1536, DN3) | Used GPU memory of the process in the DN3 timing tool | `DenoiseModelSession` releases the session with the editor GPU release and at the editor close (2.3); select a smaller tile from the free video memory |
| A photo outside the calibration overflows FP16 | The output check of each tile (DN2, DN10); the margin of the held-out photos in the calibration record | The render reports the error for that photo. The lab calibrates again with the photo and publishes a new FP16 graph (a new model version, D10) |
| The FP16 output shows a difference against FP32 by eye | The sheets of the DN2 lab work | The user checks the sheets before DN2 completes; a visible difference stops DN2 |
| The model time in the render is too long for editing on a slower device (Intel Iris Xe: 5.2 s at 45.4 MP in FP16) | The slow-device check of each run (2.4) | The render reports the slow-device error with `Turn Off AI Denoise`; the user decided on 2026-11-22 that a slow device gets this error (U1) |
| A fast device crosses `slow_device_limit` under load (thermal limit, another GPU process) and gets the error | DN3 timing tool under load; DN6 render time | `slow_device_limit` is about 3 times `target_time`; record the times under load in the DN3 completion record. A false error on the RTX 3080 Laptop GPU or the M4 stops DN6 until the limit or the measurement changes. DN3: a false error when other processes oversubscribe the video memory, issue #1043. DN6 changed the measurement: the tile size counts the local memory of all processes, and the check uses the fastest of the measured tiles from the third on (DN6 completion record) |
| The denoised entries of the prepared source cache use much host memory (545 MB for 45.4 MP in FP32; 726 MiB in F32 RGBA, DN6) | DN6 memory record | The cache budget of the prepared source cache applies to the entry; DN6 records the budget use |
| The process footprint stays high after the release of the Core ML sessions (GPU: up to 4.9 GiB after 5 photos; Neural Engine: 2.0 GiB in the lab tool) | Footprint test of DN10 | Find the owner of the memory before DN10 completes; a release that does not return the memory stops DN10. DN10: the owner is the large cache of libmalloc (`Malloc Large (empty)`, bounded and reused); with `MallocLargeCache=0` the footprint returns to 150 to 180 MiB after each release (DN10 completion record) |
| A Mac with 8 GB or a GPU with fewer cores takes longer or has too little memory for tiles of 1024 | DN10 run on such a Mac, if the user has one | Record it in the DN10 completion record; the render reports the memory error |
| A GPU of another vendor gives a different output or time | DN2 tests on that GPU; DN3 timing tool | Record it in the DN3 completion record. A test failure stops DN6 for that vendor |
| The Intel Iris Xe removes the DirectML device (887A0005 in 2 of 4 lab processes, one in FP32) | DN2 tests and the DN3 timing tool on the Iris Xe | The slow-device check stops the Iris Xe after its first measured tile, so a render runs few tiles there (DN3: after 0.67 to 0.79 s on a ready session, after 10.1 s on a new session; no removal in 2 processes). A device removal gives the render error with `Turn Off AI Denoise`. Record in the DN3 completion record whether the render backend of the editor continues after the removal; if it does not, find the cause before DN6 completes |
| Clipped areas of the model output are below the clip threshold | DN1 clipped area table | Stop DN5 until its detailed section states the highlight handling |
| A JSON change breaks old root ids | `TryDecodeRootState` rejects an old project in the DN5 tests | Keep the keys absent when the noise reduction is off |

Stop the implementation and update this plan when a phase estimate passes 2000 lines, when a
phase needs a substitute path that section 1.4 excludes, or when the source audit of section 4
no longer agrees with the branch.

## 9. Parameters

| Parameter | Value | Source |
| --- | --- | --- |
| `target_time` | The measured FP16 time at 45.4 MP: 0.72 s on the RTX 3080 Laptop GPU (DirectML), 0.60 s on the MacBook Air M4 (Neural Engine). A phase time more than 15% above it stops the phase (DN3, DN10) | User (U1), DN1 additions |
| `slow_device_limit` | 0.05 s for each megapixel of the mosaic (2.3 s at 45.4 MP), about 3 times `target_time` (RTX 3080 Laptop GPU 0.016 s, M4 0.013 s, Intel Iris Xe 0.114 s for each megapixel). The projection uses the fastest measured tile from the third measured tile on (`UtNet2Spec::kSlowDeviceMeasuredTiles`) | User (slow devices get the error), DN1 additions, DN6 |
| `tile_size` (Windows) | The largest of 768, 1024, and 1536 packed pixels whose DirectML process memory fits in the free local video memory (the smaller of the budget of the process minus its usage and, on an adapter with local memory, the local memory minus the use of all processes; then plus the memory that the session holds, DN6): FP16 736, 768, and 2704 MiB (`UtNet2Spec::kDirectMlTileMemory`; FP32: 0.9, 1.0, 3.3 GiB). 1536 is the fastest on the RTX 3080 Laptop GPU (0.45 s at 45.4 MP; 1024: 0.58 s; 768: 0.70 s) | DN1 additions, DN3 |
| `tile_size` (macOS) | The largest of 768, 1024, and 1536 packed pixels whose memory fits in the available memory of the Mac (`kern.memorystatus_level` of the physical memory, plus the memory that the sessions hold): 112, 160, and 416 MiB (`UtNet2Spec::kCoreMlTileMemory`). 1536 is the fastest on the M4 (0.55 s at 45.4 MP; 1024: 0.68 s; 768: 0.79 s) | DN1 addition, FP16 on the Neural Engine; DN10 |
| `tile_overlap` | 96 packed pixels: the tiled output is the one-pass output | DN1 |
| Session strategy | Windows: one ONNX Runtime session with free height and width, the DirectML memory arena on. macOS: one session for each tile input shape, its compiled Core ML model in the model cache of the model version (`<user cache folder>/alcedo-studio/ai-denoise-coreml/<version>/<width>x<height>`; session creation 0.01 to 0.02 s from the cache, 0.3 to 0.4 s without). Kept by `DenoiseModelSession` between renders, released with the editor GPU release and at the editor close | DN1 additions; 2.3; DN10 |
| Precision | FP16 after the exact rescale of the layers, one graph for Windows and macOS; test tolerance 7e-3 of the largest expected value (DN2: 1.5 times the largest difference of the DN2 graph on DirectML, 2.8e-3 to 4.1e-3; the 3-crop graph of DN1 gave 1.0e-3 to 1.5e-3 on the Neural Engine); FP16 margin of 8 times or more on photos outside the calibration | DN1 additions, FP16 |
| ONNX Runtime | NuGet `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and `Microsoft.AI.DirectML` 1.15.4 on Windows; `libonnxruntime.dylib` of `Microsoft.ML.OnnxRuntime` 1.24.4 (osx-arm64) on macOS | DN1 additions, DirectML and macOS |
| Core ML options | `ModelFormat` MLProgram, `MLComputeUnits` CPUAndNeuralEngine, `session.disable_cpu_ep_fallback` 1, `ModelCacheDirectory` one folder for each tile shape, the free dimensions `height`, `width`, `output_height`, `output_width` fixed to the tile shape | DN1 addition, FP16 on the Neural Engine; DN10 |
| `leaky_relu_slope` | The value in the metadata of the weights file and of the ONNX graph | DN0, DN2 |
| `output_scale` | The scale of the last layer after the rescale, in the metadata of the FP16 graph: 170.34 for the graph of DN2 (127.9 for the 3-crop calibration of DN1) | DN1 addition, FP16 on the Neural Engine; DN2 |
| Model cost | 46.2 GMAC for each megapixel | Layer count, section 5.3 |
| Input exposure curve | Gain = 0.028 / (mean of the R, G, B means), from 1 to 16; linear up to the output 0.5, then a log shoulder that keeps the white level at 1; inverted after the gain match | DN6 addition, input exposure curve |
