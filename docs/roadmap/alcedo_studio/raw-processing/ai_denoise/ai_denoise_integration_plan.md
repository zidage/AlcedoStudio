# AI Denoise Integration Master Plan

- Date: 2026-11-21
- Status: DN0 complete (2026-11-21, lab PR zidage/alcedo-denoise-lab#2, issue #1032); DN1
  complete (2026-11-21, lab PR zidage/alcedo-denoise-lab#3, issue #1033; U3 closed, U2 with
  the user); DN2 to DN13 planned. 2026-11-22: D2 and D13 changed to ONNX Runtime DirectML in
  C++ on Windows; DN2, DN3, DN10, and DN11 rewritten for it. 2026-11-22: macOS measured (DN1
  addition, ONNX Runtime on macOS); D2 extended to Core ML on macOS; DN10 rewritten for it
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

Alcedo Studio gets AI noise reduction in the Develop stage. The user turns it on for a photo. The
application runs a neural network on the RAW mosaic one time, stores the result on disk, and the
Develop stage reads the stored result in place of its own linearization and demosaic.

### 1.2 Approved decisions

| Id | Decision | Date |
| --- | --- | --- |
| D1 | The model is the full UtNet2 Bayer model of the lab, `models/dark_area_loss/checkpoint_10000.pt`. It has no strength control. | 2026-11-21 |
| D2 | The model runs in FP32 in ONNX Runtime, in the process of Alcedo Studio through the C++ API of ONNX Runtime: on Windows with the DirectML execution provider, on macOS with the Core ML execution provider on the GPU. The Rust sidecar does not run it. The model package holds the ONNX graph that the lab makes from the safetensors weights. TensorRT and cuDNN are not used. Changed on 2026-11-22 from own CUDA forward code, after the DirectML measurement of DN1; macOS added on 2026-11-22 (the user: ONNX Runtime in C++ on macOS too), with the execution provider from the macOS measurement of DN1. | 2026-11-22 |
| D3 | The work starts with the weights export and the measurements. The publication of the model package on R2 is the last product phase. | 2026-11-21 |
| D4 | Distillation to a smaller model runs only if the measured time of DN3 is above `target_time`. | 2026-11-21 |
| D5 | The denoise run blocks the application with a progress display and a Cancel control. It can process a batch of photos. It does not run in the background during editing. | 2026-11-21 |
| D6 | The result is a stored result file of the RAW file, as the current Camera Raw stores its enhanced data. The RAW file stays the photo. A separate DNG file is not made. | 2026-11-21 |
| D7 | The stored result files are in one folder that the user selects in the settings. The settings show the folder and the space in use, and let the user delete the files. | 2026-11-21 |
| D8 | When the user deletes a photo from the library with its RAW file, the application deletes the stored result file of that RAW file too. The application deletes a stored result file in no other automatic case. | 2026-11-21 |
| D9 | A project package (`.alcd`) does not contain stored result files. | 2026-11-21 |
| D10 | A stored result file keeps the result of the model version that made it. A model update does not change it. | 2026-11-21 |
| D11 | The Develop node keeps its structure: one root node, no input port. The noise reduction is a parameter of the Develop node. The loader gives the stored result to the Develop passes as a second prepared input of the same RAW file. | 2026-11-21 |
| D12 | Noise reduction on, stored result absent: the editor and the export compute the result first, with the blocking progress display. A thumbnail and a semantic input image render from the reduced mosaic and do not read the stored result. | 2026-11-21 |
| D13 | Platform order: Windows (DirectML, for each render backend and each GPU with Direct3D 12), then macOS (Core ML on Apple silicon), then Linux. On a platform without its forward phase, the application does not show the noise reduction control. Changed on 2026-11-22 from the backend order CUDA, Metal, OpenCL. | 2026-11-22 |
| D14 | The first version supports Bayer sensors only. For an X-Trans or a monochrome photo, the application reports that the photo is not supported. In a batch, it reports the photo and continues with the other photos. | 2026-11-21 |

### 1.3 Decisions that wait for a measurement

| Id | Decision | Measurement that it needs | Owner |
| --- | --- | --- | --- |
| U1 | `target_time`: the largest time for one 45 megapixel photo on the RTX 3080 Laptop GPU. It has no value now. The user sets it after the measured time of the full model (the baseline of DN3) exists | The time of DN3 | User |
| U2 | The pixel format and the compression of the stored result file | DN1, stored format table | User |
| U3 | The tile size and the tile overlap. Closed 2026-11-21: overlap 96 packed px, tile size the largest of 512 to 1536 that fits (DN1 completion record) | DN1, border difference table | Implementer, recorded in DN3 |
| U4 | The highlight handling of the stored result. The user decided on 2026-11-22 that no clip mask is made: high-noise photos have a low dynamic range, and many scenes that need the highlight recovery are not at a high ISO | DN1, clipped area table | Implementer, recorded in DN5 |

### 1.4 Options that must not return

- A separate DNG file for the denoised image.
- A background denoise job that runs while the user edits.
- A stored result file beside the RAW file.
- A render without noise reduction when the noise reduction is on and the stored result is absent,
  in the editor or in the export.
- A different model, a lower resolution, or a different backend after a failure of the denoise
  run. The run reports the error.

## 2. Product design specification

### 2.1 Editor

- The RAW Decode panel gets one switch, `AI Denoise`, above the demosaic method.
- The switch is visible when the platform has a forward phase (D13) and the photo is a Bayer
  RAW file. On Windows, the switch does not depend on the render backend (CUDA or OpenCL): the
  model runs on DirectML. For an X-Trans or a monochrome photo, the switch is disabled and its tooltip states
  that the sensor type is not supported.
- When the switch is on, the demosaic method control is disabled, because the model also does the
  demosaic.
- When the user turns the switch on and the model is not installed, the editor opens the AI Models
  page of the settings, as the AI Mask tool does. The switch stays off.
- When the user turns the switch on and the stored result is absent, the application shows the
  blocking progress display (2.3). After the run, the edit history gets one `SetParameter` change
  and the viewport shows the result. When the user cancels, the switch stays off and the history
  does not change.
- When the user turns the switch on and a valid stored result exists, the history gets the change
  immediately.
- When the user opens a photo whose noise reduction is on and whose stored result is absent, the
  editor shows the blocking progress display before the first frame. When the user cancels, the
  editor closes the photo and returns to the library.

### 2.2 Library and batch

- The library inspector actions get `AI Denoise`. It uses the targets of
  `LibraryMutationOperations::ResolveTargets`.
- A batch run processes one photo at a time on the GPU. It turns the noise reduction on in a new
  Version of each photo that completes.
- The result list of a batch gives one status for each photo: complete, not supported, RAW file
  missing, model version different, failed with the error text, canceled.
- A paste of the RAW Decode item with the noise reduction on starts the same batch run for the
  targets that have no stored result.
- The export computes an absent stored result as part of the export task of that photo.

### 2.3 Blocking progress display

- It uses the shell of `ImportProgressOverlay.qml`: `BlurredOverlay`, a progress ring, a text line,
  and a Cancel control.
- One photo: the progress is the fraction of completed tiles.
- A batch: the progress is the fraction of completed photos plus the tile fraction of the current
  photo. The text gives the file name, the index, and the remaining time. The remaining time comes
  from the measured time of the completed photos.
- Cancel takes effect between two tiles. A canceled photo leaves no file in the folder.

### 2.4 Stored result file

- Identity: the `SourceFile` fingerprint (XXH3-128 of the size and of the first and last 128 KB)
  and the byte size of the RAW file. The file name is made from these two values. The file does not
  depend on a project, so two projects that contain the same RAW file use one stored result.
- Header fields: format version, RAW fingerprint and byte size, SHA-256 of the weights, input
  preparation version, image width and height, active area, CFA phase, value domain, gain match
  factor.
- A file whose RAW identity, format version, or input preparation version does not agree is not
  used. The application states the reason. A file of a different weights SHA-256 is valid (D10).
- The writer writes to a temporary name in the same folder and then renames the file.
- The pixel format and the compression are decision U2.

### 2.5 Settings

- A new section in the Cache page: the folder of the stored results (with a folder picker), the
  file count, the total size, and `Delete all`.
- A change of the folder moves no file. The settings text states the old folder so that the user
  can move the files.
- Before a deletion, when the installed model version differs from the version of a file, the
  dialog states that a new result can differ from the deleted one.

### 2.6 Failure behavior

| Failure | Result |
| --- | --- |
| Model not installed | The AI Models page opens; no run starts |
| Sensor type not supported | Error for that photo; no stored result |
| RAW file missing or its mount offline | Error for that photo; the export keeps its existing "missing" status |
| GPU error or not enough GPU memory for the smallest tile | The run stops with the GPU error text; no stored result |
| No Direct3D 12 GPU, or ONNX Runtime or DirectML.dll fails to load | The run stops with the error text of ONNX Runtime; no stored result. The application does not run the model on the CPU |
| Folder not writable or disk full | The run stops with the file system error text; the temporary file is removed |
| Stored result does not agree with the RAW identity | The file is not used; the reason is shown; the result is computed again on the next editor entry or export |

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility in this plan |
| --- | --- |
| alcedo-denoise-lab | Weights export, ONNX graph export, expected outputs, measurements, `docs/alcedo_integration.md` |
| `cmake`, `ui/alcedo_main/CMakeLists.txt` | The ONNX Runtime packages as a C++ dependency (DirectML on Windows, the osx-arm64 library on macOS), and their install rules |
| `decoders/processor/nn` | The ONNX Runtime session of UtNet2, the overlap tile planner |
| `app` (new denoise service) | The denoise run of one photo and of a batch, the stored result reader and writer |
| `edit/input`, `edit/graph`, `edit/runtime` | The Develop parameter, the denoised input kind, the pass list, the content keys |
| `ui/alcedo_main` | The switch, the blocking progress display, the library action, the settings section |
| `decoders/processor/nn` (macOS) | The Core ML session of UtNet2 (DN10) |
| `opencl` | The forward code of Linux (DN11) |
| `app/ai_model_package_*`, `scripts/ai_models` | The feed with more than one package, the denoise model package |
| `ui/alcedo_main/automation` | The `denoise` commands of `alcedo-cli` |

### 3.2 Exclusions

- The linear UtNet2 model for X-Trans. The lab has no fine-tuned linear model.
- A strength control and a blend with the original image.
- A smaller distilled model, unless D4 starts it. That work has its own plan in the lab.
- The WebGPU and CPU render paths. They are deprecated.

## 4. Current source audit

Paths are relative to `alcedo_studio/src` unless they start with `docs/`, `scripts/`, or
`alcedo_studio/`.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Develop passes | `CompileDevelopPasses`, `edit/runtime/graph_compiler.cpp:427` | CFA input: UploadRaw, Linearize, CfaClamp, Demosaic, HighlightRecover, InverseCamMulPack, Lens, GeometryResample, CameraToAp1. RGB input (`RawInputKind::DebayeredRgb`): UploadRgb, Lens, GeometryResample, CameraToAp1 | A pass list for the denoised input kind that keeps HighlightRecover |
| Linearization | `edit/runtime/cuda/cuda_develop_pass.cpp:102`, `decoders/processor/operators/gpu/cuda_white_balance.cu:114` | Black level, white level, and the relative as-shot white balance are applied before the demosaic. `InverseCamMulPack` removes the white balance after the highlight recovery | The stored result enters after the demosaic in the same value domain |
| Highlight recovery | `decoders/processor/operators/gpu/cuda_highlight_reconstruct.cu` | Works on demosaiced RGB with white balance, normalized to the white level. The clip levels come from `cam_mul`. It reads no mosaic and no clip mask | None, if U4 shows that clipped areas of the model output stay at the clip level |
| RGB input without linearization | `RawInputLoader::FromDirectRgb`, `edit/input/raw_input_loader.cpp:628`; `cuda_sensor_demosaic.cpp:279` | `rgb_linearization` is empty, so the highlight recovery is off | The denoised input sets `rgb_linearization` from the RAW metadata |
| Decode resolution | `renderer/pipeline_scheduler.cpp:154`, `include/edit/runtime/develop_demosaic.hpp:26` | Editor and export decode at `DecodeRes::FULL`. Thumbnails and semantic input images reduce the mosaic on the host and force the Legacy demosaic | None for D12 |
| Develop node | `edit/graph/develop_node_model.cpp:591-598`, `include/edit/graph/develop_node_model.hpp:171` | Root node, no input port. `DevelopPayload::demosaic_method` holds `default`, `legacy`, or `neural_engine` | One new field; no port |
| Root id | `ComputeRootId`, `edit/history/pipeline_document_checkpoint.cpp:105` | The id is a hash of the document JSON. A new JSON key in every document changes the id of old roots | Write the new key only when the noise reduction is on |
| Content keys | `BuildFrameResultContentKeys`, `edit/runtime/result_content_key.cpp:548` | `develop.sensor_linear` is keyed on the prepared source, the linearization, and the sensor Develop parameters | Add the identity of the stored result |
| Prepared source cache | `include/edit/input/prepared_source_cache.hpp` | Key: FNV hash of the encoded bytes, byte count, downsample passes, `kRawInputPreparationVersion` | Add the denoised input as its own entry kind |
| GPU release | `include/ui/alcedo_main/album_backend/editor_gpu_resource_release.hpp`, `PipelineExecutor::ReleaseBinding` | Export and semantic generation release the GPU resources of the editor before they run | A holder for the denoise run |
| ONNX Runtime | `rust/puerh_mind/Cargo.toml:25-33` (`ort` 2.0.0-rc.13, ONNX Runtime 1.28); `ui/alcedo_main/CMakeLists.txt:1237` | Only the Rust sidecar uses ONNX Runtime. The pyke binaries link ONNX Runtime statically into `alcedo_mind.exe` (`onnxruntime.lib`, no DLL, no headers). The sidecar build copies DirectML.dll 1.15.4 beside `alcedo_main`. The vcpkg port of the tree (`vcpkg/ports/onnxruntime`, 1.23.2) has no DirectML feature, and the manifest does not use it | A C++ dependency on the NuGet packages `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 (onnxruntime.dll and the C and C++ headers) and `Microsoft.AI.DirectML` 1.15.4 (DN2). The sidecar keeps its static ONNX Runtime; one DirectML.dll of one version is in the install folder. macOS (DN10): `libonnxruntime.dylib` of the osx-arm64 folder of `Microsoft.ML.OnnxRuntime` 1.24.4 (CPU, Core ML, and WebGPU execution providers), the same headers as on Windows. The macOS sidecar `alcedo_mind` also links ONNX Runtime 1.28 statically (no dylib in the bundle), in its own process |
| CUDA conv layers | `cuda/nn/conv2d.cu:671-753`, `cuda/nn/cutlass_conv2d.cu:31-105` | Fast kernels for convolutions without padding only. Max pooling, LeakyReLU, and PixelShuffle are absent | None on Windows (D2). Input for an own forward code of DN11 |
| Tile planner | `include/decoders/processor/neural_tile_jobs.hpp:190-331` | Planned for a network that shrinks its input: 1086 in, 1024 out, no overlap between outputs | A planner for an output of the input size with an overlap |
| Weights | `decoders/processor/nn/demosaicnet_cache.cpp:83-147`, `nn/safetensors.cpp:96` | The DemosaicNet files are part of the installer. The loader checks metadata strings | The denoise ONNX graph comes from the model folder of the AI Models page |
| Tests of model code | `alcedo_studio/tests/cuda/CMakeLists.txt:14`, target `MlOpsTest` | CUDA layer and module tests with stored expected outputs | A new test target for the ONNX Runtime session (Windows only) |
| Timing tools | `alcedo_studio/deprecated/legacy_pipeline/tests/perf/` | No CMake target builds them | A new timing tool target for UtNet2 |
| Model packages | `app/ai_model_package_service.cpp:40`, `scripts/ai_models/prepare_ai_model_packages.py:90` | One known package. The script makes a feed with one entry, so a second package replaces the first | A feed with all packages |
| Blocking UI | `ui/alcedo_main/qml/ImportProgressOverlay.qml`, `BlurredOverlay.qml` | Blocking overlay with progress and Cancel for the import | A denoise overlay on the same shell |
| Paste | `ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.cpp:73` | A paste writes a Version for each target and starts no computation | Start the batch run after the paste |
| Files beside RAW files | `decoders/metadata_decoder.cpp:41` | The application reads XMP files and writes no file into a photo folder | None (D7) |
| Noise reduction | none | The editor has no noise reduction operator and no Detail panel | New |

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Changes | Error surface |
| --- | --- | --- | --- | --- |
| `UtNet2DirectMlSession` (proposed, Windows) | One packed tile in host memory, the ONNX graph | One RGB tile in host memory | Its ONNX Runtime session and the DirectML memory arena of the session | Exception with the ONNX Runtime status text |
| `UtNet2CoreMlSession` (proposed, macOS, DN10) | One packed tile in host memory, the ONNX graph without DepthToSpace | One RGB tile in host memory (PixelShuffle in the crop copy) | Its ONNX Runtime session and the Core ML model that the session compiles | Exception with the ONNX Runtime status text |
| `DenoiseRunService` (proposed, `app`) | RAW path or encoded bytes, cancel request | A stored result file, progress events | The folder of the stored results | Result value with a status and an error text for each photo |
| `DenoiseResultStore` (proposed, `app`) | RAW identity | Read handle, write handle, folder statistics | Files in the folder | File system error; header validation reason |
| `RawInputLoader` (current) | RAW bytes, the Develop parameter, the store | `PreparedRawInput` of the denoised kind | Nothing outside its result | Exception when the parameter is on and the stored result is absent or invalid |
| Develop passes (current) | `PreparedRawInput` | `develop.sensor_linear` | GPU textures of the frame | Existing render error path |

The render path never starts a denoise run. The editor entry, the switch, the batch action, and
the export start it before they request a frame. This keeps the rule of D12 in one place: a
render with the noise reduction on and no valid stored result is an error.

### 5.2 Data flow

```text
Denoise run (one photo):
  UI or export -> DenoiseRunService::Run(photo)
    -> EditorGpuReleaseHolder (denoise) releases the editor GPU resources
    -> LibRaw unpack -> ClassifyRawInput -> reject a sensor that is not Bayer
    -> input preparation of the lab specification -> packed mosaic in host memory
    -> UtNet2DirectMlSession: one session with free height and width (DN1 addition, DirectML)
    -> for each tile: host input tile (reflection fill to 16) -> Ort::Session::Run on DirectML
       -> crop the overlap into the host output
       -> accumulate the sums of the gain match -> progress event -> check the cancel request
    -> release the session (its memory arena holds up to 3.3 GiB)
    -> apply the gain match factor -> DenoiseResultStore::Write (temporary file, rename)

Render with the noise reduction on:
  RawInputLoader::LoadEncoded
    -> LibRaw open -> linearization metadata and cam_mul
    -> DenoiseResultStore::Read(identity) -> validate the header
    -> PreparedRawInput { kind = denoised RGB, rgb_linearization = set }
  CompileDevelopPasses
    -> UploadRgb (to the value domain of the highlight recovery) -> HighlightRecover
    -> InverseCamMulPack -> Lens -> GeometryResample -> CameraToAp1
```

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
| DN1 | Measurements: tile overlap, PyTorch time, clipped areas, stored formats | lab | DN0 | 700 to 1100 | complete (#1033) |
| DN2 | ONNX graph in the lab release; ONNX Runtime DirectML as a C++ dependency; the session of UtNet2 with its tests | lab, `cmake`, `decoders/processor/nn` | DN0 | 700 to 1100 | planned |
| DN3 | Overlap tile planner, full photo run on DirectML, time against `target_time` | `decoders/processor/nn` | DN1, DN2 | 800 to 1200 | planned |
| DN4 | Stored result reader and writer, folder setting, statistics | `app` | U2 | 900 to 1400 | planned |
| DN5 | Develop parameter, denoised input kind, pass list, content keys (upload on CUDA, Metal, OpenCL) | `edit` | DN4, U4 | 1200 to 1800 | planned |
| DN6 | Denoise run service: preparation, tiles, gain match, cancel, GPU release, errors | `app` | DN3, DN4 | 1000 to 1500 | planned |
| DN7 | Editor switch, blocking progress display, missing model path | `ui/alcedo_main` | DN5, DN6 | 800 to 1300 | planned |
| DN8 | Batch action, paste, export, deletion with the RAW file | `ui/alcedo_main`, `app` | DN7 | 1000 to 1600 | planned |
| DN9 | Settings section for the stored results | `ui/alcedo_main` | DN4 | 400 to 700 | planned |
| DN10 | macOS forward code: the Core ML session of UtNet2 in ONNX Runtime (C++), the osx-arm64 library, the macOS tile size selection | `cmake`, `decoders/processor/nn` | DN3 | 600 to 1000 | planned |
| DN11 | Linux forward code (OpenCL) | `opencl` | DN3 | 1400 to 1900 | planned |
| DN12 | Feed with more than one package, denoise model package, AI Models card | `app`, `scripts/ai_models` | DN7 | 900 to 1400 | planned |
| DN13 | `alcedo-cli` denoise commands, user documentation | automation, `docs` | DN8 | 500 to 900 | planned |

DN0 to DN3 have detailed sections below. DN4 to DN13 depend on U1 to U4. Each gets its detailed
section in this file before its implementation starts, with the format of DN0 to DN3. DN4 and
DN5 can start before DN3 completes: a script of the lab writes stored result files from PyTorch,
so the Develop integration can be checked by eye before the DirectML tile loop of DN3 exists.

Until DN12, a developer copies the ONNX graph file by hand into
`<model folder>/ai-denoise-v1/`. The model folder is the QSettings value
`semantic/modelDirectory`.

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

**Remaining gaps:** step 5 (`write_stored_result.py`) after U2. Not measured: the forward code of
Alcedo Studio (DN3), the highlight recovery on the stored result by eye (DN5), stored formats in
C++.

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

- **Selected for DN10:** Core ML, `ModelFormat` MLProgram, `MLComputeUnits` CPUAndGPU,
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
  times (Windows: 1.9 times). A stored result (D6) is necessary on macOS.
- **Session start:** Core ML session creation 0.09 to 0.50 s, first run of a tile of 1536 0.49 to
  0.79 s. WebGPU compiles its shaders in the first run (1.35 to 1.73 s).
- **Memory after the release:** in one process, the footprint after the release of the `free`
  session grows from photo to photo (0.97 to 4.9 GiB, of which 0.75 GiB are host arrays). The lab
  did not find the owner of this memory. DN10 measures the footprint of Alcedo Studio after a run
  and after a batch.
- Not measured: M4 Pro, M4 Max, and other Apple GPUs; Intel Macs; Macs with more or less than
  16 GB; Core ML input and output in `IOSurface` memory.

### DN2: ONNX graph, ONNX Runtime DirectML dependency, UtNet2 session

Rewritten on 2026-11-22 for D2 (ONNX Runtime DirectML in C++). The plan of own CUDA layers of
UtNet2 is replaced; the DN1 addition of the last tile states the cuDNN level that those layers
needed.

**Objective and deliverables**

- In the lab: `scripts/export/export_onnx.py` and the ONNX graph as a release asset beside the
  safetensors weights, with its SHA-256 and the metadata of the weights file. The same release
  gets the copy without the last DepthToSpace node for macOS (lab
  `scripts/export/macos/write_graph_without_pixel_shuffle.py`, DN10), with its SHA-256.
- In Alcedo Studio: ONNX Runtime with DirectML as a C++ dependency on Windows, one session of
  UtNet2 that runs one tile, and its tests against the DN0 expected outputs.

**Inputs and prerequisites**

DN0. The DN1 addition of 2026-11-22 (ONNX Runtime DirectML). `build/debug` configured.

**Modules, files, and APIs**

Lab:

| File | Responsibility |
| --- | --- |
| `scripts/export/export_onnx.py` (proposed) | The export function of `measure_onnx_directml.py` as its own script: opset 17, TorchScript exporter, input `input` (1, 4, height, width), output `output` (1, 3, 2 height, 2 width), height and width free. The ONNX metadata (`metadata_props`) gets `architecture`, `leaky_relu_slope`, `input_preparation_version`, and the SHA-256 of the safetensors weights. Check: the CPU execution provider gives the PyTorch output of the three DN0 inputs within 1e-5 of the largest value |
| `measure_onnx_directml.py` | Imports the export function of `export_onnx.py` |

Alcedo Studio (headers under `include/decoders/processor/nn/`, sources under
`decoders/processor/nn/`):

| File (proposed) | Responsibility |
| --- | --- |
| `cmake/OnnxRuntimeDirectMl.cmake` | Downloads the NuGet packages `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and `Microsoft.AI.DirectML` 1.15.4 at configure time with `file(DOWNLOAD ... EXPECTED_HASH SHA256=...)` into the build tree, extracts them, and defines the imported target `alcedo::onnxruntime_directml` (headers, `onnxruntime.lib`, `onnxruntime.dll`). Windows x64 only |
| `ui/alcedo_main/CMakeLists.txt` | Copies `onnxruntime.dll` and `onnxruntime_providers_shared.dll` beside `alcedo_main` in the build and the install. DirectML.dll comes from the DirectML package; the build fails when the DirectML.dll that the sidecar build copies has a different file version |
| `utnet2_specs.hpp` | Input and output names, the alignment of 16, the tile sizes of DN1, the metadata keys and values that the session checks |
| `utnet2_directml_session.hpp`, `utnet2_directml_session.cpp` | `UtNet2DirectMlSession`: one `Ort::Env` for the process, one session with free height and width on the DirectML adapter of high performance, memory arena on, CPU fallback refused (`session.disable_cpu_ep_fallback`), graph optimization level all, `ORT_SEQUENTIAL`, memory pattern off. `Run(tile)`: host input (1, 4, h, w) to host output (1, 3, 2h, 2w). `Release()` ends the session and frees its memory arena |

**Data rules**

- FP32 only.
- The session fails at load when the graph file is absent, when a metadata value differs from
  `utnet2_specs.hpp`, or when a node cannot run on DirectML. The error text names the file and
  the first difference.
- `Run` throws `std::runtime_error` with the ONNX Runtime status text and the tile shape. It
  throws for a height or a width that is not a multiple of 16.
- The session is not used on the CPU execution provider in the application (2.6). The CPU
  execution provider is used only in the export check of the lab.
- Only `decoders/processor/nn` includes ONNX Runtime headers. The ONNX Runtime types do not appear
  in a public header of another module.
- The Rust sidecar keeps its own static ONNX Runtime 1.28. The application has one
  `onnxruntime.dll` (1.24.4) and one DirectML.dll (1.15.4).

**Implementation steps**

1. Lab: write `export_onnx.py`, add the ONNX graph and its SHA-256 to the release asset of DN0
   and to the README of `scripts/export/`.
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
     -> Ort::Session (graph metadata checked)
     -> Run(host tile) -> Ort::Session::Run on DirectML -> host output
     -> compare with the DN0 expected output
```

**Primary failure call chain**

```text
graph with another architecture or a node that DirectML cannot run
  -> the session constructor throws std::runtime_error with the file path and the reason
  -> no tile runs; the caller reports the error
```

**Tests** (`UtNet2DirectMlTest`)

| Test | Assertion |
| --- | --- |
| `OutputMatchesPyTorchForEachExpectedOutput` | `random_64`, `random_256`, and `rawnind_crop_512` within 1e-5 of the largest expected value |
| `LoadFailsOnDifferentArchitectureMetadata` | A copy of the graph with a different `architecture` value gives an error that names the key |
| `LoadFailsOnMissingFile` | The error names the path |
| `RunRejectsSizeThatIsNotAMultipleOfSixteen` | A 4 x 72 x 64 input throws; no output |
| `ReleaseFreesTheSessionAndALaterRunLoadsAgain` | After `Release`, `Run` loads the session again and gives the same output |

**Build and run commands**

```text
Set-Location <repository root>
cmd /c "scripts\msvc_env.cmd --build build\debug --target UtNet2DirectMlTest"
$env:ALCEDO_AI_DENOISE_TEST_ASSETS = "<folder with the graph and the expected outputs>"
ctest --test-dir build/debug -R "UtNet2DirectMlTest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] The ONNX graph is in the lab release with its SHA-256.
- [ ] The five tests pass in `build/debug`.
- [ ] The install folder has one `onnxruntime.dll` and one DirectML.dll, and the AI Mask sidecar
      runs its DirectML session.

**Expected diff**: 700 to 1100 lines (lab included).

**Completion record**: empty until the phase is complete.

### DN3: overlap tile planner and full photo run on DirectML

Rewritten on 2026-11-22 for D2.

**Objective and deliverables**

The tile loop that denoises one full Bayer mosaic with `UtNet2DirectMlSession`, a test against
one pass, and the measured time for 24, 36, and 45 megapixel photos. This phase closes U1 and
D4.

**Inputs and prerequisites**

DN2. The `tile_size` and `tile_overlap` values of DN1 and the session strategy of the DN1
addition (one session with free height and width).

**Modules, files, and APIs** (proposed)

| File | Responsibility |
| --- | --- |
| `include/decoders/processor/nn/overlap_tile_jobs.hpp` | `PlanOverlapTiles(width, height, tile, overlap, alignment)`: the input rectangle and the kept output rectangle of each tile (lab `measurement_common.plan_axis` with the shrunk last tile) |
| `decoders/processor/nn/utnet2_tile_loop.cpp` | `DenoiseBayerMosaicDirectMl(packed mosaic, tile callback, cancel check)`: the tile size selection, the input tile with reflection, `Run`, the crop to the kept rectangle |
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
- Tile size: the largest of 1536, 1024, and 768 whose measured process memory (3.3, 1.0, and
  0.9 GiB) fits in the free local video memory of the adapter (`IDXGIAdapter3::QueryVideoMemoryInfo`,
  budget minus current usage). When 768 does not fit, the run throws the memory error. It does
  not use a different backend.
- The cancel check runs between tiles.
- The session is released after the last photo of a run (single photo or batch), so that the
  editor gets its GPU memory back.

**Implementation steps**

1. Write the planner and its tests.
2. Write the tile loop: tile size selection, input tile with reflection, `Run`, crop, callback
   with the RGB tile.
3. Write the tile loop tests.
4. Write the photo timing tool. For each photo it reports the tile size, the tile count, the time
   of the tile loop, the sum of the `Run` times, the used GPU memory of the process, and the GPU
   temperature and clock (NVML when the adapter is NVIDIA).
5. Run the tool on the three photos in the release build. Record the table beside the DN1
   DirectML and PyTorch times. Give the 45 megapixel time to the user for U1.

**Primary success call chain**

```text
DenoiseBayerMosaicDirectMl
  -> UtNet2DirectMlSession (load one time for the run)
  -> select the tile size from the free video memory
  -> PlanOverlapTiles
  -> for each tile: input with reflection -> Run -> crop -> callback(tile RGB) -> cancel check
```

**Primary failure call chain**

```text
DirectML device removed or out of memory during Run
  -> Run throws std::runtime_error with the ONNX Runtime status text
  -> no more tile runs; the caller reports the error and writes no stored result
```

**Tests**

| Test | Assertion |
| --- | --- |
| `OverlapTilesCoverTheImageExactlyOnce` | For sizes that are and are not multiples of the tile, each output pixel is in one kept rectangle |
| `OverlapTileInputsAreMultiplesOfSixteen` | Each input rectangle starts at a multiple of 16 and has a width and a height that are multiples of 16, also for image sizes that are not multiples of 16 |
| `TiledOutputEqualsOnePassOnTheExpectedCrop` | On `rawnind_crop_512` with tiles of 256 and overlap 96, the tiled output equals the one-pass output within 1e-5 of the largest value |
| `TileSizeSelectionUsesTheLargestThatFits` | For free memory values below and above the three thresholds, the selected size is the largest that fits; below the smallest, the error |
| `TileLoopStopsAtCancelRequest` | After the cancel request, no more tile callback runs |

**Exit criteria**

- [ ] The five tests pass in `build/debug`.
- [ ] The time table of the three photos is in the completion record.
- [ ] U1 is closed: the user gives `target_time`, and the record states whether D4 starts the
      smaller model.

**Expected diff**: 800 to 1200 lines.

**Completion record**: empty until the phase is complete.

### DN4 to DN13

Each of these phases gets its detailed section before its implementation starts. The fixed points
that the detailed sections must keep:

- **DN4**: the store is the only reader and writer of the folder. The header rules of 2.4. A write
  failure leaves no file with the final name.
- **DN5**: the new `DevelopPayload` field is written to JSON only when it is on. The Develop
  dirty group of the change is the sensor group, so `develop.sensor_linear` and all later results
  are computed again. The loader throws when the field is on and the store has no valid file.
- **DN6**: the gain match factor is one value for the whole photo. The tile loop accumulates its
  sums, and the writer applies the factor before the file gets its final name. In a batch, the CPU
  unpacks the next RAW file while the GPU runs the current photo. The editor GPU release holder
  is held during the run on every render backend: DirectML uses the same GPU memory as the CUDA
  or OpenCL resources of the editor.
- **DN7**: QML rules of the `alcedo-qml-ui` skill. No test for pure UI; the tests are on the C++
  owner of the switch state.
- **DN8**: the deletion path of `LibraryMutationOperations::StartDeleteTargets` calls the store
  for each RAW file that it deletes (D8).
- **DN10, DN11**: the same expected outputs, tolerances, and tile planner as DN3. The control of
  2.1 appears on a platform in the phase that completes its forward code (D13).
- **DN10**: the configuration of the DN1 addition "ONNX Runtime on macOS": `UtNet2CoreMlSession`
  (`decoders/processor/nn`, Apple silicon only) with the Core ML execution provider
  (`ModelFormat` MLProgram, `MLComputeUnits` CPUAndGPU, `AllowLowPrecisionAccumulationOnGPU` 0,
  `session.disable_cpu_ep_fallback` 1), one session with free height and width, and the graph
  file without DepthToSpace; the tile loop does the PixelShuffle (mode CRD) in its crop copy.
  FP32 only: the activations reach 1.3e6, above the FP16 range (lab `inference_cost.md`,
  2026-11-18), and the Neural Engine is not used. `UtNet2DirectMlSession` and
  `UtNet2CoreMlSession` have the same `Run` and `Release` interface, so the tile loop of DN3
  runs on both. The macOS tile size is the largest of 1536 and 1024 whose measured footprint
  (5.5 and 3.2 GiB at 45.4 MP) fits in the memory that the system reports as available; below
  1024, the run throws the memory error. The dependency is `libonnxruntime.dylib` of the
  osx-arm64 folder of `Microsoft.ML.OnnxRuntime` 1.24.4 (the version of Windows), downloaded with
  its SHA-256 and copied into `Contents/Frameworks` of the bundle. Tests: the five tests of DN2
  and the five tests of DN3 on macOS, and the footprint of the process after a run and after a
  batch of 5 photos (DN1 addition, memory after the release).
- **DN11**: own OpenCL kernels; the CUDA rows of section 4 are its starting point. Before it
  writes kernels, DN11 measures the WebGPU execution provider of ONNX Runtime on Vulkan
  (`Microsoft.ML.OnnxRuntime.EP.WebGpu` has linux-x64); on macOS, WebGPU took 2.6 times the Core
  ML time.
- **DN12**: the feed keeps the AI Mask entry when the denoise entry is added. The `sequence` of
  the feed increases.

## 8. Risks and stop conditions

| Risk | Detection | Response |
| --- | --- | --- |
| The measured overlap is near the theoretical 100 packed pixels, so the tile overhead is 1.6 to 2.8 times | DN1 border table | Use the largest tile that the GPU memory permits; give the user the corrected time estimate before DN3 |
| `onnxruntime-directml` 1.24.4 is the last DirectML package of ONNX Runtime | NuGet and PyPI package lists | Keep the pinned version. The graph uses 6 standard operators of opset 17 (Conv, LeakyRelu, MaxPool, ConvTranspose, Concat, DepthToSpace) |
| The DirectML memory arena keeps up to 3.3 GiB after a run | Used GPU memory of the process in the DN3 timing tool | Release the session after each run (DN3 data rules); select a smaller tile from the free video memory |
| The process footprint stays high after the release of the Core ML session (up to 4.9 GiB after 5 photos in the lab tool) | Footprint test of DN10 after a run and after a batch | Find the owner of the memory before DN10 completes; a release that does not return the memory stops DN10 |
| A Mac with 8 GB or a GPU with fewer cores takes longer or has too little memory for tiles of 1024 | DN10 run on such a Mac, if the user has one | Record it in the DN10 completion record; the run reports the memory error |
| A GPU of another vendor gives a different output or time | DN2 tests on that GPU; DN3 timing tool | Record it in the DN3 completion record. A test failure stops DN6 for that vendor |
| Clipped areas of the model output are below the clip threshold | DN1 clipped area table | Stop DN5 until its detailed section states the highlight handling |
| The lossy formats show an error after +5 EV | DN1 stored format table and sheets | The user selects the format (U2) |
| A JSON change breaks old root ids | `TryDecodeRootState` rejects an old project in the DN5 tests | Keep the key absent when the noise reduction is off |

Stop the implementation and update this plan when a phase estimate passes 2000 lines, when a
phase needs a substitute path that section 1.4 excludes, or when the source audit of section 4
no longer agrees with the branch.

## 9. Parameters

| Parameter | Value | Source |
| --- | --- | --- |
| `target_time` | No value until the baseline time of DN3 exists; then from the user (U1) | User |
| `tile_size` | The largest of 768, 1024, and 1536 packed pixels whose DirectML process memory (0.9, 1.0, 3.3 GiB) fits in the free local video memory; 1536 is the fastest on the RTX 3080 Laptop GPU (0.82 s at 45.4 MP), 1024 takes 1.06 s | DN1 addition, DirectML |
| `tile_overlap` | 96 packed pixels: the tiled output is the one-pass output | DN1 |
| `tile_size` (macOS) | The largest of 1024 and 1536 packed pixels whose measured footprint (3.2, 5.5 GiB at 45.4 MP) fits in the available memory; 1536 takes 3.55 s at 45.4 MP on the M4, 1024 takes 4.40 s | DN1 addition, macOS |
| Session strategy | One ONNX Runtime session with free height and width, released after the run; on Windows the DirectML memory arena on, on macOS the graph without DepthToSpace | DN1 additions, DirectML and macOS |
| ONNX Runtime | NuGet `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 and `Microsoft.AI.DirectML` 1.15.4 on Windows; `libonnxruntime.dylib` of `Microsoft.ML.OnnxRuntime` 1.24.4 (osx-arm64) on macOS | DN1 additions, DirectML and macOS |
| Core ML options | `ModelFormat` MLProgram, `MLComputeUnits` CPUAndGPU, `AllowLowPrecisionAccumulationOnGPU` 0 | DN1 addition, macOS |
| `leaky_relu_slope` | The value in the metadata of the weights file and of the ONNX graph | DN0, DN2 |
| Comparison tolerance of the forward tests | 1e-5 of the largest value (from the CPU against GPU difference of PyTorch); DirectML gives at most 1.7e-6 | DN0, DN1 addition |
| Model cost | 46.2 GMAC for each megapixel | Layer count, section 5.3 |
| Stored result folder default | A `denoise` folder beside the thumbnail cache folder of the application | This plan |
