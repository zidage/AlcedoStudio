# AI Denoise Integration Master Plan

- Date: 2026-11-21
- Status: DN0 complete (2026-11-21, lab PR zidage/alcedo-denoise-lab#2, issue #1032); DN1 to
  DN13 planned
- Source revision of the audit: `main` at `61d59bc98`
- Model source: [alcedo-denoise-lab](https://github.com/zidage/alcedo-denoise-lab), `docs/plan.md`
  steps 7 and 8
- Related plans:
  - [CUDA CNN Forward Framework + DemosaicNet Plan](../demosaicnet/cuda_nn_forward_demosaicnet_plan.md)
    and [its performance follow-up](../demosaicnet/cuda_demosaicnet_performance_next.md): this
    plan extends the CUDA layer library of those plans.
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
| D2 | The model runs in FP32 in the forward code of Alcedo Studio: fixed topology in C++, weights from a safetensors file. ONNX Runtime, TensorRT, and cuDNN are not used. | 2026-11-21 |
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
| D13 | Backend order: CUDA, then Metal, then OpenCL. On a backend without its forward phase, the application does not show the noise reduction control. | 2026-11-21 |
| D14 | The first version supports Bayer sensors only. For an X-Trans or a monochrome photo, the application reports that the photo is not supported. In a batch, it reports the photo and continues with the other photos. | 2026-11-21 |

### 1.3 Decisions that wait for a measurement

| Id | Decision | Measurement that it needs | Owner |
| --- | --- | --- | --- |
| U1 | `target_time`: the largest time for one 45 megapixel photo on the RTX 3080 Laptop GPU. It has no value now. The user sets it after the measured time of the full model (the baseline of DN3) exists | The time of DN3 | User |
| U2 | The pixel format and the compression of the stored result file | DN1, stored format table | User |
| U3 | The tile size and the tile overlap | DN1, border difference table | Implementer, recorded in DN3 |
| U4 | The highlight handling of the stored result | DN1, clipped area table | Implementer, recorded in DN5 |

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
- The switch is visible when the active backend has a forward phase (D13) and the photo is a Bayer
  RAW file. For an X-Trans or a monochrome photo, the switch is disabled and its tooltip states
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
| Folder not writable or disk full | The run stops with the file system error text; the temporary file is removed |
| Stored result does not agree with the RAW identity | The file is not used; the reason is shown; the result is computed again on the next editor entry or export |

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility in this plan |
| --- | --- |
| alcedo-denoise-lab | Weights export, expected outputs, measurements, `docs/alcedo_integration.md` |
| `cuda/nn` | The new FP32 layers of UtNet2 |
| `decoders/processor/nn` | The UtNet2 module, its weights cache, the overlap tile planner |
| `app` (new denoise service) | The denoise run of one photo and of a batch, the stored result reader and writer |
| `edit/input`, `edit/graph`, `edit/runtime` | The Develop parameter, the denoised input kind, the pass list, the content keys |
| `ui/alcedo_main` | The switch, the blocking progress display, the library action, the settings section |
| `metal`, `opencl` | The forward code of the other two backends |
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
| CUDA conv layers | `cuda/nn/conv2d.cu:671-753`, `include/cuda/nn/conv2d.hpp` | Fast kernels for convolutions without padding: 3x3 with output blocks of 8, 16, 24, 32; 1x1 for 24 or 32 inputs. A padded convolution uses the slow generic kernel | Padded 3x3 kernels for 4 to 512 channels |
| CUTLASS | `cuda/nn/cutlass_conv2d.cu:31-105` | SIMT FP32 implicit GEMM for 32 to 32 channels without padding, ReLU epilogue | A wrapper with padding, variable channels, and the LeakyReLU epilogue (`linear_combination_leaky_relu.h` is in the tree) |
| Other CUDA layers | `cuda/nn/*.cu` | ReLU, concat (NCHW), slice, crop, generic transposed convolution. Max pooling, LeakyReLU, and PixelShuffle are absent | New layers |
| Tile planner | `include/decoders/processor/neural_tile_jobs.hpp:190-331` | Planned for a network that shrinks its input: 1086 in, 1024 out, no overlap between outputs | A planner for an output of the input size with an overlap |
| Weights | `decoders/processor/nn/demosaicnet_cache.cpp:83-147`, `nn/safetensors.cpp:96` | The DemosaicNet files are part of the installer. The loader checks metadata strings | The denoise weights come from the model folder of the AI Models page |
| Layer tests | `alcedo_studio/tests/cuda/CMakeLists.txt:14`, target `MlOpsTest` | Layer tests and module tests with stored expected outputs at an absolute tolerance of 1e-4 | New tests in the same target |
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
| `UtNet2BayerModule` (proposed, one for each backend) | One packed tile, weights | One RGB tile | Its workspace | Exception with the backend error |
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
    -> input preparation of the lab specification -> packed mosaic on the GPU
    -> for each tile: UtNet2BayerModule::Forward -> crop the overlap -> copy to host
       -> accumulate the sums of the gain match -> progress event -> check the cancel request
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
  measured overlap of DN1 replaces this estimate.

## 6. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| DN0 | Weights file, expected outputs, input and output specification | lab | none | 600 to 900 | complete (#1032) |
| DN1 | Measurements: tile overlap, PyTorch time, clipped areas, stored formats | lab | DN0 | 700 to 1100 | planned |
| DN2 | CUDA layers of UtNet2 with tests and a time for each layer shape | `cuda/nn` | DN0 | 1400 to 1900 | planned |
| DN3 | CUDA UtNet2 module, overlap tile planner, full photo run, time against `target_time` | `decoders/processor/nn` | DN1, DN2 | 1200 to 1700 | planned |
| DN4 | Stored result reader and writer, folder setting, statistics | `app` | U2 | 900 to 1400 | planned |
| DN5 | Develop parameter, denoised input kind, pass list, content keys (CUDA, Metal, OpenCL upload) | `edit` | DN4, U4 | 1200 to 1800 | planned |
| DN6 | Denoise run service: preparation, tiles, gain match, cancel, GPU release, errors | `app` | DN3, DN4 | 1000 to 1500 | planned |
| DN7 | Editor switch, blocking progress display, missing model path | `ui/alcedo_main` | DN5, DN6 | 800 to 1300 | planned |
| DN8 | Batch action, paste, export, deletion with the RAW file | `ui/alcedo_main`, `app` | DN7 | 1000 to 1600 | planned |
| DN9 | Settings section for the stored results | `ui/alcedo_main` | DN4 | 400 to 700 | planned |
| DN10 | Metal forward code | `metal` | DN3 | 1000 to 1600 | planned |
| DN11 | OpenCL forward code | `opencl` | DN3 | 1400 to 1900 | planned |
| DN12 | Feed with more than one package, denoise model package, AI Models card | `app`, `scripts/ai_models` | DN7 | 900 to 1400 | planned |
| DN13 | `alcedo-cli` denoise commands, user documentation | automation, `docs` | DN8 | 500 to 900 | planned |

DN0 to DN3 have detailed sections below. DN4 to DN13 depend on U1 to U4. Each gets its detailed
section in this file before its implementation starts, with the format of DN0 to DN3. DN4 and
DN5 can start before DN3 completes: a script of the lab writes stored result files from PyTorch,
so the Develop integration can be checked by eye before the C++ forward code exists.

Until DN12, a developer copies the weights file by hand into
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

- [ ] Each of the four tables is in the findings files with its command and its source file.
- [ ] `tile_size` and `tile_overlap` have values in the parameter table of the lab plan.
- [ ] The user has the stored format table and the sheets for U2.

**Expected diff**: 700 to 1100 lines.

**Completion record**: empty until the phase is complete.

### DN2: CUDA layers of UtNet2

**Objective and deliverables**

FP32 CUDA layers for each operation of UtNet2 in the NHWC layout, each with a test against a CPU
reference, and a time table for each layer shape of the model.

**Inputs and prerequisites**

The expected outputs of DN0. `MlOpsTest` builds in `build/debug`.

**Modules, files, and APIs**

Current files that change: `cuda/CMakeLists.txt`, `alcedo_studio/tests/cuda/CMakeLists.txt`.

Proposed files (headers under `include/cuda/nn/`, sources under `cuda/nn/`):

| API (proposed) | Operation | Notes |
| --- | --- | --- |
| `Conv2d3x3SamePadNhwcBiasLeakyRelu` | 3x3, zero padding 1, bias, LeakyReLU with a slope argument | Input and output channel counts are arguments. Candidate kernels: CUTLASS implicit GEMM with the LeakyReLU epilogue for channel counts that are multiples of 32; a direct kernel for the 4 to 32 layer |
| `MaxPool2x2Nhwc` | 2x2 max, stride 2 | Fails for an odd height or width |
| `ConvTranspose2x2Stride2NhwcBias` | 2x2, stride 2, bias | Each input pixel writes one 2x2 output block: 4 products of the 1x1 form |
| `ConcatChannelsNhwc` | Concatenation of two tensors on the channel axis | Used for the 4 skip connections |
| `Conv1x1ToPixelShuffle2NhwcBias` | 1x1 from 32 to 12 channels and PixelShuffle(2) in one kernel | Output: 3 channels at 2 times the size |
| `TransformConv2d3x3WeightsKrsc` | OIHW weights to the CUTLASS KRSC layout | Variable channels |

**Data rules**

- FP32 only. No TF32, FP16, or BF16.
- Each layer validates its pointers and sizes and throws `std::runtime_error` with the layer name.
- No layer allocates device memory. The caller gives the output buffer.
- No new dependency. The layers link `cudart` only, as the current layers do.

**Implementation steps**

1. Add the CPU reference functions of the six operations to a test support file. They follow the
   PyTorch definitions.
2. Implement `MaxPool2x2Nhwc`, `ConcatChannelsNhwc`, `ConvTranspose2x2Stride2NhwcBias`, and
   `Conv1x1ToPixelShuffle2NhwcBias`. Add their tests.
3. Implement the padded 3x3 layer with CUTLASS for the 17 layer shapes whose channel counts are
   multiples of 32. Select the threadblock and warp tile shape for each shape from the time table
   of step 5.
4. Implement the direct kernel for the 4 to 32 layer.
5. Add the timing tool `UtNet2LayerTimingTool` (proposed target, `alcedo_studio/tests/perf/`,
   built only with the proposed CMake option `ALCEDO_BUILD_PERF_TOOLS`). For each of the 23 layers at the tensor sizes of a
   512 and a 1024 packed pixel tile, it reports the median time of 20 runs after 5 warm-up runs,
   the multiply-accumulate count, and the throughput in TFLOP/s. It writes a CSV file to
   `build/tmp/ai_denoise/`.
6. For each 3x3 layer shape, compare the CUTLASS kernel with an in-tree direct kernel of the
   existing multi-output form extended to padding. Keep the faster one for each shape. Record the
   table in the completion record.

**Primary success call chain**

```text
test -> upload input and weights -> Conv2d3x3SamePadNhwcBiasLeakyRelu(stream)
     -> download -> compare with the CPU reference
```

**Primary failure call chain**

```text
caller gives channels that the selected kernel does not support
  -> the layer throws std::runtime_error("<layer name>: unsupported shape ...")
  -> no output is written
```

**Tests** (in `MlOpsTest`)

| Test | Assertion |
| --- | --- |
| `SamePadConv3x3MatchesCpuReferenceForEachUtNet2Shape` | Largest absolute difference below the tolerance of DN0 for the 18 shapes on random input |
| `SamePadConv3x3ZeroPadsTheImageBorder` | The border rows and columns equal the CPU reference with zero padding |
| `LeakyReluSlopeIsAppliedToNegativeValues` | A negative pre-activation value gives value times slope |
| `MaxPool2x2SelectsTheLargestOfEachBlock` | Equal to the CPU reference; an odd size throws |
| `ConvTranspose2x2Stride2MatchesCpuReference` | Equal within the tolerance for the 4 shapes of the model |
| `ConcatChannelsNhwcKeepsBothInputsInOrder` | Output channels 0 to A-1 are input 1; A to A+B-1 are input 2 |
| `Conv1x1ToPixelShuffle2MatchesPyTorchChannelOrder` | Equal to the expected output of the DN0 layer record |
| `EachUtNet2LayerMatchesThePyTorchLayerOutput` | With the DN0 weights and the 64 x 64 input, each of the 23 layer outputs is within the tolerance |

**Build and run commands**

```text
Set-Location <repository root>
cmd /c "scripts\msvc_env.cmd --build build\debug --target MlOpsTest"
$env:PATH = "build\debug\vcpkg_installed\x64-windows\debug\bin;" + $env:PATH
ctest --test-dir build/debug -R "MlOpsTest\.(SamePad|LeakyRelu|MaxPool|ConvTranspose2x2|ConcatChannelsNhwc|Conv1x1ToPixelShuffle|EachUtNet2Layer)" -j 1 --output-on-failure
```

The timing tool runs from the release build (`win_release`).

**Exit criteria**

- [ ] The eight tests pass in `build/debug`.
- [ ] The time table of the 23 layers at both tile sizes is in the completion record, with the
      GPU temperature and clock.
- [ ] The sum of the layer times for one tile, times the tile count of a 45 megapixel photo at the
      overlap of DN1, is in the completion record as the first estimate against `target_time`.

**Expected diff**: 1400 to 1900 lines. If the direct kernel comparison of step 6 passes 1900
lines, it moves to its own phase DN2b.

**Completion record**: empty until the phase is complete.

### DN3: CUDA UtNet2 module and full photo run

**Objective and deliverables**

A CUDA module that denoises one full Bayer mosaic in tiles, a test against the expected outputs,
and the measured time for 24, 36, and 45 megapixel photos. This phase closes U1 and D4.

**Inputs and prerequisites**

DN2. The `tile_size` and `tile_overlap` values of DN1.

**Modules, files, and APIs** (proposed)

| File | Responsibility |
| --- | --- |
| `include/decoders/processor/nn/utnet2_specs.hpp` | Channel table, tensor names, alignment (16), tile limits |
| `include/decoders/processor/nn/overlap_tile_jobs.hpp` | `PlanOverlapTiles(width, height, tile, overlap, alignment)`: tile origins, the input rectangle and the kept output rectangle of each tile |
| `decoders/processor/nn/utnet2_bayer.cu` | `UtNet2BayerModule`: weight load with name, shape, and metadata checks; `Forward(tile)` with a workspace that the caller owns |
| `decoders/processor/nn/utnet2_cache.cpp` | One loaded module for each device; model folder resolution |
| `decoders/processor/operators/gpu/cuda_utnet2.cu` | `DenoiseBayerMosaicCuda(packed mosaic, tile callback, cancel check)`: the tile loop |
| `alcedo_studio/tests/perf/utnet2_photo_timing_tool.cpp` | Full photo time and peak GPU memory |

**Data rules**

- The kept output rectangles of all tiles cover the image exactly one time. The planner proves
  this in its tests, as `neural_tile_jobs.hpp` does.
- A tile at the image border is filled by reflection to the alignment of 16, as the lab
  specification states. The filled area is not part of the output.
- The module selects the largest tile size of the DN1 table whose workspace fits in the free GPU
  memory. When the smallest size does not fit, it throws the memory error. It does not use a
  different backend.
- Each tile is one short GPU submission. The cancel check runs between tiles.
- The loader fails when a tensor name, a shape, `architecture`, or `leaky_relu_slope` is absent or
  different.

**Implementation steps**

1. Write the planner and its tests.
2. Write the module: the load, the weight layout change, the workspace size function, the forward
   pass of section 5.3.
3. Write the tile loop: input tile with reflection, forward, crop to the kept rectangle, callback
   with the RGB tile.
4. Write the module test against the expected outputs.
5. Write the photo timing tool. For each photo it reports the tile size, the tile count, the time
   of the forward loop, the total time with the host copies, the peak GPU memory, and the GPU
   temperature and clock.
6. Run the tool on the three photos in the release build. Record the table. Give the 45 megapixel
   time to the user for U1.

**Primary success call chain**

```text
DenoiseBayerMosaicCuda
  -> UtNet2 cache: load the module one time
  -> PlanOverlapTiles
  -> for each tile: gather input -> UtNet2BayerModule::Forward -> crop -> callback(tile RGB)
```

**Primary failure call chain**

```text
weights file absent or with a different tensor table
  -> the cache throws std::runtime_error with the file path and the first difference
  -> no tile runs; the caller reports the error
```

**Tests**

| Test | Assertion |
| --- | --- |
| `OverlapTilesCoverTheImageExactlyOnce` | For sizes that are and are not multiples of the tile, each output pixel is in one kept rectangle |
| `OverlapTileInputsAreMultiplesOfSixteen` | Each input rectangle has a width and a height that are multiples of 16 |
| `UtNet2BayerOutputMatchesPyTorchWithinTolerance` | The three expected outputs of DN0 are matched within the DN0 tolerance |
| `UtNet2TiledOutputMatchesOnePassWithinBorderTolerance` | On the 512 crop, the tiled output at the DN1 overlap differs from one pass by less than the DN1 border level |
| `UtNet2LoadFailsOnMissingTensor` | A file without one tensor gives an error that names the tensor |
| `UtNet2TileLoopStopsAtCancelRequest` | After the cancel request, no more tile callback runs |

**Exit criteria**

- [ ] The six tests pass in `build/debug`.
- [ ] The time table of the three photos is in the completion record.
- [ ] U1 is closed: the user gives `target_time`, and the record states whether D4 starts the
      smaller model.

**Expected diff**: 1200 to 1700 lines.

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
  unpacks the next RAW file while the GPU runs the current photo.
- **DN7**: QML rules of the `alcedo-qml-ui` skill. No test for pure UI; the tests are on the C++
  owner of the switch state.
- **DN8**: the deletion path of `LibraryMutationOperations::StartDeleteTargets` calls the store
  for each RAW file that it deletes (D8).
- **DN10, DN11**: the same expected outputs and tolerances as DN3. The control of 2.1 appears on
  a backend in the phase that completes its forward code (D13).
- **DN12**: the feed keeps the AI Mask entry when the denoise entry is added. The `sequence` of
  the feed increases.

## 8. Risks and stop conditions

| Risk | Detection | Response |
| --- | --- | --- |
| The measured overlap is near the theoretical 100 packed pixels, so the tile overhead is 1.6 to 2.8 times | DN1 border table | Use the largest tile that the GPU memory permits; give the user the corrected time estimate before DN3 |
| The CUDA 3x3 kernels at 64 to 512 channels are far below the 5 TFLOP/s of the 24 channel kernel | DN2 time table | Record the table; continue to DN3 for the full time; the user decides U1 with it |
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
| `tile_size` | 256 to 1024 packed pixels, multiple of 16; selected from the free GPU memory | DN1 |
| `tile_overlap` | Smallest overlap with a border difference below the TF32 difference level | DN1 |
| `leaky_relu_slope` | The value in the metadata of the weights file | DN0 |
| Comparison tolerance of the forward tests | From the CPU against GPU difference of PyTorch | DN0 |
| Model cost | 46.2 GMAC for each megapixel | Layer count, section 5.3 |
| Stored result folder default | A `denoise` folder beside the thumbnail cache folder of the application | This plan |
