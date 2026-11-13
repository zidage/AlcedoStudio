# SM3.A Segmentation Service Architecture — Phase Plan

Date: 2026-10-22.

Status: complete on 2026-10-22 (`feature/ai-segmentation-sm3a-service-architecture`, #462). The macOS build and port check ran later with SM3.B (section 11.3).

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), section
9.2.1.

Prerequisites: SM1 complete (master plan section 8.2). No other phase.

Issues: SM3.A.1 #460, SM3.A.2 #461.

Source revision of the audit: `d1e0d8c7` on `feature/ai-segmentation-sm1-quality-review`. The
implementation is on `main` (`3c378d9c`); the SM1 measurement program of that branch is not on
`main`.

---

## 1. Decision record

### 1.1 User request (2026-10-22)

- SM1 is complete with the evidence of `alcedo-ai-mask-lab`. SAM3 is not used.
- SM3 starts now. SM2 moves after SM9.
- SM3 splits into SM3.A (service architecture) and SM3.B (image embedding cache and first
  optimizations). The split is by function. It is not a split into a Rust phase and a C++ phase.
- The macOS adapter is part of SM3.A, although the Mac is not available. The model load and the
  inference follow the CLIP path: Windows and macOS have different execution paths.
- The code has low coupling.

### 1.2 Inherited decisions

- D12: the sidecar runs the models. Windows: ONNX Runtime with DirectML. macOS: compiled native
  Core ML models through the `coreml` crate.
- D13: no model download before SM9. The host gives the sidecar a local model directory.
- D15: U3 (no edge post-processing), U6 (memory budget, unload when the user goes back to
  adjusting), U7 (no decoding while the pointer moves), U8 (float16 acceptable).
- Master plan section 8.2.3: ONNX Runtime 1.28 or later, the MVP session settings, the decoder
  input rules, and the mask selection rules.

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| A1 | The load request carries the model directory and the device. The sidecar process arguments do not change. | One sidecar process serves CLIP, image analysis, and segmentation. Four code paths build `AiSidecarRuntimeOptions`. A process argument would need all four to agree, and a change would restart the process. A per-request directory keeps the segmentation path independent. |
| A2 | Models load and unload only on explicit requests. There is no idle unload timer. | U6: the host unloads when the user goes back to adjusting. The CLIP idle timer does not apply. |
| A3 | The sidecar keeps only the image embedding of a session. Each decode request carries all points, the box, and the previous selected logits. | The selection belongs to the tool (host). The sidecar does not keep a copy of it. The extra upload is 256 KB on loopback. |
| A4 | The decode response carries float32 logits (256 x 256) as `repeated float`. A click returns three levels; other prompts return one mask. | The host needs the logits for `mask_input`, for presentation, and for the level change without a model run (section 8.2.3). |
| A5 | The C++ segmentation client is a separate object with its own persistent channel. It is not a member of `sidecar_client::Client`. | The existing client makes a new channel per call and has a 4 MB receive limit. Segmentation needs one channel for the tool lifetime and a 64 MB limit. A separate object keeps the existing clients unchanged. |
| A6 | No cancellation protocol. | Decode takes 12 to 35 ms and encode 70 to 200 ms (section 8.2.4). The host ends a session instead of cancelling a call. |
| A7 | ONNX Runtime sessions of segmentation register DirectML with `error_on_failure`: a DirectML registration failure is a load error, never a CPU session. The CPU entry of the provider list only switches off the CPU memory arena (`DisableCpuMemArena`), as the MVP session option does; ONNX Runtime always keeps the CPU provider for node placement. | AGENTS.md (no fallback) and #451 (the CLIP list runs on the CPU without an error). |
| A8 | SM3.A raises `ort` to 2.0.0-rc.13 (ONNX Runtime 1.28). DirectML and Core ML become target-specific features. | microsoft/onnxruntime#29071. `ort` 2.0.0-rc.14 and later need Rust 1.92; the toolchain is 1.91. The pyke binaries of rc.13 exist per execution provider set only. |
| A9 | SM3.A has two parts, SM3.A.1 and SM3.A.2. Each part changes Rust and C++ code. | The whole phase is estimated at 2,700 lines, more than the 2,000-line limit. The split is by function (model execution and sessions, then prompt decoding), so each part is a complete vertical slice. |

### 1.4 Rejected options

- A CPU entry after DirectML in the segmentation sessions (A7).
- The ONNX Runtime Core ML execution provider on macOS (lab U15).
- Padding the decoder input to a fixed point count (E3: it breaks the masks).
- A new channel per segmentation call (P7).
- A sidecar process argument for the model directory (A1).

---

## 2. Product and data specification

SM3.A has no user-visible change. Its users are SM3.B, SM3.C, SM7, and SM9.

### 2.1 Model directory

The host gives an absolute directory path in the load request. The directory contains:

| Platform | Encoder | Decoder |
| --- | --- | --- |
| Windows (ONNX Runtime) | `point_encoder.onnx` | `point_decoder.onnx` |
| macOS (native Core ML) | `point_encoder.mlmodelc/` | `point_decoder.mlmodelc/` |

The files are the files of master plan section 8.2.1. SM3.C adds the head and S3OD files to the
same directory. The model set identity is `alcedo-ai-mask-v1`. SM9 resolves the directory as
`<model download directory>/alcedo-ai-mask-v1`. SM2 installs the files there.

Developer setup until SM2 (Windows): copy `point_encoder.onnx` and `point_decoder.onnx` from the
lab evidence `build/tmp/explore/e4/onnx_float32_batch16/tiny/` into one directory. macOS: copy
`point_encoder.mlmodelc` and `point_decoder.mlmodelc` from the lab evidence
`build/tmp/explore/coreml/float32/`.

### 2.2 Devices

| Platform | Device value | Result |
| --- | --- | --- |
| Windows | `directml` (default when empty), `directml:N` | DirectML on adapter 0 or N. One execution provider. |
| Windows, macOS | `cpu` | ONNX Runtime CPU on Windows; Core ML `CpuOnly` for all models on macOS. Only for development and tests. Never selected automatically. |
| macOS | `coreml` (default when empty) | Encoder `ComputeUnits::All`, decoder `ComputeUnits::CpuOnly` (U15). |
| any | other values | The load request fails with `AI_STATUS_INVALID_ARGUMENT`. |

ONNX Runtime session settings (section 8.2.3): graph optimization level 3, 4 intra-op threads, 1
inter-op thread, CPU memory arena off, memory patterns off, sequential execution.

### 2.3 Operations

| Operation | Input | Result | Failure |
| --- | --- | --- | --- |
| Load models | model directory, device | Encoder and decoder are loaded now. The same directory and device again: no reload. Another directory or device: all sessions end, the old models unload, the new models load. | The sidecar unloads everything and returns the real load error (path, backend message). |
| Unload models | none | All sessions end. The models unload. Idempotent. | none |
| Status | none | loaded, model set identity, directory, device, backend name, per-model residency (role, device label, load time), session count. | none |
| Start session | RGB8 pixels, width, height | Session identity, encoder time. The encoder runs once. | Models not loaded; width or height 0; long edge over 4096; byte count not `width * height * 3`; more than 4 sessions; encoder error. No session is kept after a failure. |
| Decode prompt (SM3.A.2) | session identity, points, optional box, optional mask input | Mask levels with logits, score, token, area; selected index. | Models not loaded; unknown session; invalid prompt (section 2.4); decoder error. |
| End session | session identity | `ended` true, or false for an unknown session. | none |

### 2.4 Prompt rules (SM3.A.2)

- A point has normalized ReferenceSpace coordinates in [0, 1] and a label: add (1) or subtract (0).
- A box has normalized corners. The sidecar orders the corners (minimum first). A box with zero
  width or zero height is invalid.
- The decoder point count is the number of points plus 2 for a box. It must be 1 to 64 (the Core ML
  decoder range).
- The mask input is empty or exactly 65,536 finite values (256 x 256).
- Decoder input: the box corners first with labels 2 and 3, then the points; coordinates x 1024.
- Selection: one point without a box returns three levels: tokens 1 to 3 sorted by area (logits
  > 0), then token; the selected index is the level with the best score. Other prompts return one
  mask: token 0 when its stability is at least 0.98, else the token 1 to 3 with the best score.
  Stability is the area of logits > 0.05 divided by the area of logits > -0.05 (1 when the second
  area is 0). Logits are clipped to +-32.

### 2.5 Image preprocessing

The sidecar converts the RGB8 image to float in [0, 1], resizes it to 1024 x 1024 with the
antialiased bilinear filter of PyTorch `interpolate(mode="bilinear", antialias=True,
align_corners=False)` (the filter of torchvision `Resize` on a tensor), normalizes with the ImageNet
mean (0.485, 0.456, 0.406) and standard deviation (0.229, 0.224, 0.225), and writes CHW order. The
resize works in float, not in 8 bits.

### 2.6 Errors

Each response has an `AiResponseHeader`. gRPC status is OK unless the transport fails.

| Condition | `status` | `error_code` |
| --- | --- | --- |
| Models not loaded | `AI_STATUS_PROVIDER_UNAVAILABLE` | `AI_ERROR_MODEL_NOT_READY` |
| Model file missing, load error | `AI_STATUS_PROVIDER_UNAVAILABLE` | `AI_ERROR_MODEL_NOT_READY` |
| Invalid device, image, or prompt | `AI_STATUS_INVALID_ARGUMENT` | `AI_ERROR_INVALID_INPUT` (new) |
| Unknown session | `AI_STATUS_INVALID_ARGUMENT` | `AI_ERROR_SESSION_NOT_FOUND` (new) |
| Session limit | `AI_STATUS_INVALID_ARGUMENT` | `AI_ERROR_INVALID_INPUT` |
| Encoder or decoder error | `AI_STATUS_INTERNAL` | `AI_ERROR_INTERNAL` |

The message is the real error text. No operation substitutes another model, device, or size.

### 2.7 Threads and ordering

- The tonic handlers run model work on the blocking thread pool.
- The engine keeps the loaded model set behind a read-write lock. Start and decode hold the read
  lock for the whole model run. Load and unload take the write lock, so they wait for the running
  model calls and then clear the sessions. This is ordinary lock scope, not a version or token
  mechanism.
- Each loaded model has its own mutex: one call per model at a time. A decode of one session can
  run while another session encodes.
- Core ML: each model is called under its own mutex, so its calls never overlap.
- The C++ app service is not thread-safe. Its owner calls it from one thread (SM9 decides which).

---

## 3. Scope

### 3.1 Included

| Module | Responsibility |
| --- | --- |
| `rust/puerh_mind/Cargo.toml`, `src/service/ort_runtime.rs` | `ort` 2.0.0-rc.13, target-specific execution provider features, execution provider code compiled only for its target. No behavior change for CLIP. |
| `rust/puerh_mind/proto/segmentation.proto`, `proto/ai_common.proto` | Segmentation service and messages. Two new error codes. |
| `rust/puerh_mind/src/service/segmentation/` | Backend interface, ONNX Runtime adapter, Core ML adapter, model directory rules, preprocessing, prompt rules, engine. |
| `rust/puerh_mind/src/server/segmentation.rs` | tonic service: request validation, header mapping, blocking pool. |
| `rust/puerh_mind/src/main.rs`, `bootstrap.rs`, `service/registry.rs`, `build.rs` | Construct the engine and register the service. |
| `alcedo_studio/src/app/CMakeLists.txt` | Generate the C++ code of `segmentation.proto` into `AiProto`. |
| `alcedo_studio/src/sidecar_client/`, `include/sidecar_client/` | Segmentation DTOs, client interface, gRPC client with a persistent channel. |
| `alcedo_studio/src/app/`, `include/app/` | `AiMaskSegmentationService`: one tool activation; holds the sidecar lease and the client. |
| `scripts/ai_models/sam2/` | Reference generator for the Rust port check (SM3.A.2). |

### 3.2 Excluded

| Item | Owner |
| --- | --- |
| Embedding cache, native tensor residency, DirectML session per point count | SM3.B |
| Head, S3OD, task masks | SM3.C |
| Analysis render that produces the RGB8 image | SM4 |
| Tool controller, threads, UI, model directory resolution from the settings | SM9 |
| Model download | SM2 |
| A capability descriptor in `ListCapabilities` | not needed: the host knows the service from its version |

---

## 4. Current source audit

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| ONNX Runtime version | `rust/puerh_mind/Cargo.toml` | `ort` 2.0.0-rc.12, `api-24`, features `directml` and `coreml` on all targets. | rc.13, `api-28`; `directml` on Windows, `coreml` on macOS (A8). |
| Execution provider list | `src/service/ort_runtime.rs` | `execution_providers_for_device_request` uses `cfg!` at run time and names `ep::DirectML` and `ep::CoreML` on all targets. rc.13 defines these types only with their feature. | Move each provider into a `#[cfg]` function. Same lists as before. |
| CLIP engines | `src/service/inference.rs`, `ort_clip.rs`, `coreml_clip.rs` | Windows: `OrtClipEngine`; macOS: `CoreMlClipEngine`. Both use `EncoderResidency` (lazy load, idle unload). | No change. The segmentation engine follows the same platform split but uses explicit load and unload (A2). |
| Service registration | `src/service/registry.rs` | Registers health, model manager, semantic, AI runtime, image analysis. Message limits only on the semantic service. | Register the segmentation service with a 64 MB limit. |
| Proto build | `rust/puerh_mind/build.rs`; `alcedo_studio/src/app/CMakeLists.txt` | Rust compiles five protos into one descriptor set. C++ `AiProto` has `ai_common`, `ai_runtime`, `image_analysis`. | Add `segmentation.proto` to both. |
| C++ transport | `alcedo_studio/src/sidecar_client/client.cpp` | `MakeChannel` per call, default 4 MB receive limit. `Client` aggregates five sub-clients with one fake in `tests/app/ai_sidecar_runtime_service_test.cpp`. | New separate segmentation client (A5). `Client` does not change. |
| Sidecar lifetime | `app/ai_sidecar_runtime_service.cpp` | `AcquireLease()` keeps the process running; `Endpoint()` gives the address. | No change. The tool owner passes a lease and the endpoint to `AiMaskSegmentationService`. |

---

## 5. Target architecture

### 5.1 Rust owners

```text
server::segmentation::SegmentationServiceImpl        (tonic, header mapping, spawn_blocking)
  -> service::segmentation::engine::SegmentationEngine
       owns: RwLock<Option<LoadedModelSet>>, Mutex<HashMap<session id, ImageEmbedding>>
       uses: Arc<dyn SegmentationBackendFactory>     (platform adapter, chosen at startup)
  -> service::segmentation::backend
       trait SegmentationBackendFactory: create(device) -> Box<dyn SegmentationBackend>
       trait SegmentationBackend: load(directory, role) -> Box<dyn SegmentationModel>
       trait SegmentationModel: run(inputs) -> outputs in model specification order
  -> ort_backend::OrtSegmentationBackend             (all targets; DirectML on Windows)
  -> coreml_backend::CoreMlSegmentationBackend       (macOS only)
  -> model_set: file names, input names, output names per backend
  -> sam2: preprocessing (SM3.A.1), prompt rules and mask selection (SM3.A.2)
```

- The engine knows models only through the backend traits and the model specification. It does
  not name ONNX Runtime or Core ML types.
- The adapters know nothing about prompts, sessions, or selection rules.
- `sam2` contains pure functions without model calls; unit tests cover them without model files.

### 5.2 C++ owners

```text
SM9 tool controller (future)
  -> AiSidecarRuntimeService::AcquireLease() + Endpoint()
  -> AiMaskSegmentationService(lease, MakeGrpcSegmentationClient(endpoint))
       LoadModels(directory, device) / StartImage(image) / Decode(prompt) / EndImage() / Close()
  -> sidecar_client::SegmentationClient (gRPC, one channel, 64 MB limit)
```

- `AiMaskSegmentationService` owns one current session identity. `StartImage` ends the previous
  session first. `Close` ends the session, unloads the models, and releases the lease, in this
  order. The destructor calls `Close`.
- The service does not keep the selection; its caller passes the full prompt (A3).

### 5.3 Primary success call chain

```text
AiMaskSegmentationService::LoadModels(dir, "")
  -> GrpcSegmentationClient::LoadModels -> SegmentationService.LoadSegmentationModels
  -> SegmentationServiceImpl::load_segmentation_models -> spawn_blocking
  -> SegmentationEngine::load -> backend factory create(device) -> backend.load(dir, Encoder/Decoder)
  -> LoadedModelSet published under the write lock -> header OK
AiMaskSegmentationService::StartImage(rgb8)
  -> StartSegmentationSession -> SegmentationEngine::start_session
  -> sam2::preprocess_image -> encoder.run -> ImageEmbedding stored -> session id
AiMaskSegmentationService::Decode(prompt)                                   (SM3.A.2)
  -> DecodeSegmentationPrompt -> sam2::validate_prompt -> sam2::model_prompt
  -> decoder.run(embedding, coords, labels, mask_input) -> sam2::select_masks -> masks
AiMaskSegmentationService::Close()
  -> EndSegmentationSession -> UnloadSegmentationModels -> lease released
```

### 5.4 Primary failure call chain

```text
LoadModels(dir without point_encoder.onnx)
  -> SegmentationEngine::load -> backend.load fails "point_encoder.onnx: file not found"
  -> engine keeps no models and no sessions -> header PROVIDER_UNAVAILABLE / MODEL_NOT_READY
  -> SegmentationCallStatus{ok=false, error=<real message>}
StartImage before LoadModels
  -> engine: no LoadedModelSet -> PROVIDER_UNAVAILABLE / MODEL_NOT_READY -> no session id
DirectML registration failure
  -> ort session builder with DirectML error_on_failure -> load error -> same as above
```

No operation changes document or project state, so no restore chain exists.

---

## 6. File and API map

### 6.1 Changed files

| Path | Change |
| --- | --- |
| `rust/puerh_mind/Cargo.toml`, `Cargo.lock` | `ort` rc.13 (A8). |
| `rust/puerh_mind/src/service/ort_runtime.rs` | Provider functions compiled only for their target. |
| `rust/puerh_mind/proto/ai_common.proto` | `AI_ERROR_INVALID_INPUT = 13`, `AI_ERROR_SESSION_NOT_FOUND = 14`. |
| `rust/puerh_mind/build.rs` | Compile `segmentation.proto`. |
| `rust/puerh_mind/src/service/mod.rs`, `src/server/mod.rs` | New modules. |
| `rust/puerh_mind/src/service/registry.rs`, `src/bootstrap.rs`, `src/main.rs` | Engine construction and registration. |
| `alcedo_studio/src/app/CMakeLists.txt` | `segmentation.proto` in `AiProto`; `AiMaskSegmentationService` library. |
| `alcedo_studio/src/sidecar_client/CMakeLists.txt` | New sources. |
| `alcedo_studio/tests/app/CMakeLists.txt` | New test targets. |

### 6.2 Proposed files

| Path | Content |
| --- | --- |
| `rust/puerh_mind/proto/segmentation.proto` | `SegmentationService`: `LoadSegmentationModels`, `UnloadSegmentationModels`, `GetSegmentationStatus`, `StartSegmentationSession`, `DecodeSegmentationPrompt` (SM3.A.2), `EndSegmentationSession`. |
| `src/service/segmentation/mod.rs` | Module documentation and exports. |
| `src/service/segmentation/tensor.rs` | `TensorF32 { shape, data }` with a length check. |
| `src/service/segmentation/backend.rs` | `SegmentationModelRole`, `SegmentationModel`, `SegmentationBackend`, `SegmentationBackendFactory`. |
| `src/service/segmentation/model_set.rs` | `MODEL_SET_ID`, file names per backend, input and output names, expected shapes. |
| `src/service/segmentation/ort_backend.rs` | `OrtSegmentationBackend`, device parsing, session settings, I/O validation, `run`. |
| `src/service/segmentation/coreml_backend.rs` | `CoreMlSegmentationBackend` (macOS), compute units per role, multi-array conversion. |
| `src/service/segmentation/sam2.rs` | `preprocess_image`, `resize_bilinear_antialias`; SM3.A.2: `validate_prompt`, `model_prompt`, `select_masks`. |
| `src/service/segmentation/engine.rs` | `SegmentationEngine`, `LoadedModelSet`, `ImageEmbedding`, `SegmentationError`. |
| `src/server/segmentation.rs` | `SegmentationServiceImpl`. |
| `rust/puerh_mind/tests/segmentation_port_check.rs` | SM3.A.2 port check with model files and reference logits (skips without the environment variables). |
| `scripts/ai_models/sam2/make_segmentation_port_reference.py` | SM3.A.2 reference logits from PyTorch preprocessing and ONNX Runtime 1.31 CPU. |
| `alcedo_studio/src/include/sidecar_client/dto/segmentation.hpp` | C++ DTOs. |
| `alcedo_studio/src/include/sidecar_client/segmentation_client.hpp` | `SegmentationClient`, `MakeGrpcSegmentationClient`. |
| `alcedo_studio/src/sidecar_client/segmentation_client.cpp` | `GrpcSegmentationClient`. |
| `alcedo_studio/src/include/app/ai_mask_segmentation_service.hpp`, `src/app/ai_mask_segmentation_service.cpp` | `AiMaskSegmentationService`. |
| `alcedo_studio/tests/app/segmentation_client_test.cpp` | Client against an in-process gRPC server. |
| `alcedo_studio/tests/app/ai_mask_segmentation_service_test.cpp` | App service with a fake client. |

### 6.3 Main APIs

```rust
pub trait SegmentationModel: Send {
    /// Runs the model once. `inputs` follow the input names of the model specification;
    /// the result follows its output order.
    fn run(&mut self, inputs: &[&TensorF32]) -> anyhow::Result<Vec<TensorF32>>;
}
pub trait SegmentationBackend: Send + Sync {
    fn backend_name(&self) -> &str;                      // "onnxruntime-directml", "coreml-native"
    fn device_label(&self, role: SegmentationModelRole) -> String;
    fn load(&self, directory: &Path, role: SegmentationModelRole)
        -> anyhow::Result<Box<dyn SegmentationModel>>;
}
pub trait SegmentationBackendFactory: Send + Sync {
    fn create(&self, device: &str) -> Result<Box<dyn SegmentationBackend>, SegmentationError>;
}
impl SegmentationEngine {
    pub fn new(factory: Arc<dyn SegmentationBackendFactory>) -> Self;
    pub fn load(&self, directory: &Path, device: &str) -> Result<(), SegmentationError>;
    pub fn unload(&self);
    pub fn status(&self) -> SegmentationStatus;
    pub fn start_session(&self, image: &RgbImageView) -> Result<StartedSession, SegmentationError>;
    pub fn decode(&self, session_id: &str, prompt: &SegmentationPrompt)
        -> Result<DecodedMasks, SegmentationError>;                    // SM3.A.2
    pub fn end_session(&self, session_id: &str) -> bool;
}
```

```cpp
class SegmentationClient {
 public:
  virtual auto LoadModels(const std::string& model_directory, const std::string& device,
                          std::chrono::milliseconds timeout) -> SegmentationCallStatus = 0;
  virtual auto UnloadModels(std::chrono::milliseconds timeout) -> SegmentationCallStatus = 0;
  virtual auto GetStatus(std::chrono::milliseconds timeout) -> SegmentationStatus = 0;
  virtual auto StartSession(const SegmentationRgb8Image& image, std::chrono::milliseconds timeout)
      -> SegmentationSessionResult = 0;
  virtual auto Decode(const std::string& session_id, const SegmentationPrompt& prompt,
                      std::chrono::milliseconds timeout) -> SegmentationDecodeResult = 0;
  virtual auto EndSession(const std::string& session_id, std::chrono::milliseconds timeout)
      -> SegmentationCallStatus = 0;
};
auto MakeGrpcSegmentationClient(std::string endpoint) -> std::unique_ptr<SegmentationClient>;
```

---

## 7. Phase SM3.A.1 — Model execution and session lifecycle

Issue: #460.

### 7.1 Objective and deliverables

Load, run, and unload the SAM 2.1 tiny encoder and decoder on Windows DirectML and macOS Core ML
through one backend interface. Start and end segmentation sessions. Give the C++ host a client with
a persistent channel and an app service with the tool lifecycle.

### 7.2 Inputs and prerequisites

- Read `AGENTS.md` and the skills `alcedo-msvc-cmake` and `execute-phase-plan` again.
- Model files of section 2.1 for the manual Windows check.

### 7.3 Implementation steps

1. Raise `ort` to 2.0.0-rc.13 (A8). Move the provider lists of `ort_runtime.rs` into `#[cfg]`
   functions. Run `cargo test`.
2. Add `AI_ERROR_INVALID_INPUT` and `AI_ERROR_SESSION_NOT_FOUND` to `ai_common.proto`.
3. Write `segmentation.proto` with the load, unload, status, start, and end messages.
4. Write `tensor.rs`, `backend.rs`, and `model_set.rs`.
5. Write `ort_backend.rs`: parse `""`, `directml`, `directml:N`, `cpu`; build the session with the
   settings of section 2.2 and `error_on_failure` on DirectML; check that the session has the input
   and output names of the model specification; `run` binds the inputs as tensor views and copies
   the outputs in specification order.
6. Write `coreml_backend.rs` (macOS): parse `""`, `coreml`, `cpu`; load `<role>.mlmodelc` with the
   compute units of section 2.2; `run` copies each input into a Float32 `MultiArray`, predicts, and
   reads each output by name. It reads Float32 and Float16 storage and follows the strides.
7. Write `sam2::preprocess_image` and `sam2::resize_bilinear_antialias` (section 2.5).
8. Write `engine.rs` with load, unload, status, start session, and end session (section 2.3, 2.7).
9. Write `server/segmentation.rs`; register the service with a 64 MB message limit.
10. C++: add the proto to `AiProto`; write the DTOs, the client interface, and the gRPC client with
    one channel and 64 MB send and receive limits; write `AiMaskSegmentationService`.
11. Write the tests of section 7.5. Run them.
12. Windows manual check: load the model files on DirectML, start a session on a 4096 px photo, and
    record the load time and the encoder time.
13. macOS: type-check `coreml_backend.rs` from Windows through a shim crate in `build/tmp/` (the
    full crate does not build for macOS on Windows). Record that the Mac build and run are not done.

### 7.4 Data rules

- A failed load leaves no loaded model and no session.
- A session keeps the three encoder outputs only. It does not keep the image.
- `start_session` validates the image before it runs the encoder.
- The engine never selects another device after an error.

### 7.5 Tests

| Name | Observable assertion |
| --- | --- |
| Rust `resize_matches_pytorch_antialias_reference` | The resize of a fixed 7 x 5 three-channel image to 3 x 4 and to 9 x 6 matches values computed with PyTorch `interpolate(antialias=True)` within 1e-5. |
| Rust `preprocess_normalizes_constant_gray_image` | A constant RGB (128, 128, 128) image gives `(128/255 - mean_c) / std_c` in every element of channel c, shape [1, 3, 1024, 1024]. |
| Rust `start_session_before_load_reports_model_not_ready` | `SegmentationError::ModelNotReady`; the session count stays 0. |
| Rust `load_failure_leaves_no_models_and_reports_backend_error` | A fake backend that fails on the decoder: status not loaded, error text contains the backend message. |
| Rust `start_session_runs_encoder_once_and_keeps_its_outputs` | The fake encoder runs once; the stored embedding equals the fake output. |
| Rust `start_session_rejects_invalid_image_without_running_encoder` | Byte count mismatch, zero width, long edge 4097: `InvalidInput`; encoder call count 0. |
| Rust `session_limit_rejects_fifth_session` | Four sessions succeed, the fifth fails with `InvalidInput`. |
| Rust `same_load_request_keeps_models_and_sessions` | The second identical load does not call the backend; the session stays. |
| Rust `load_with_other_directory_ends_sessions` | The session count becomes 0; the backend loads again. |
| Rust `unload_ends_sessions_and_is_idempotent` | After two unloads: not loaded, 0 sessions. |
| Rust `end_unknown_session_returns_false` | `false`; known session: `true` once, then `false`. |
| Rust `ort_device_values_parse_to_one_provider` | `""` and `directml` give DirectML on Windows; `cpu` gives CPU; `coreml`, `cuda`, `directml:-1` are rejected. |
| Rust `ort_backend_reports_missing_model_file_path` | Load from an empty directory: the error names `point_encoder.onnx`. |
| Rust `server_maps_model_not_ready_to_provider_unavailable` | The tonic handler returns `AI_STATUS_PROVIDER_UNAVAILABLE` and `AI_ERROR_MODEL_NOT_READY`. |
| Rust `server_maps_invalid_image_to_invalid_argument` | `AI_STATUS_INVALID_ARGUMENT`, `AI_ERROR_INVALID_INPUT`. |
| C++ `SegmentationClientTest.StartSessionSendsImageLargerThanDefaultLimit` | A 4096 x 2731 RGB8 image (33.5 MB) reaches the in-process server unchanged in size. |
| C++ `SegmentationClientTest.SequentialCallsUseOneConnection` | 20 calls: the server sees one peer address. |
| C++ `SegmentationClientTest.FailureHeaderMapsToFailedStatus` | Server header `INVALID_ARGUMENT` with a message: `ok == false`, the message is kept. |
| C++ `SegmentationClientTest.UnreachableEndpointReportsTransportError` | `ok == false`, non-empty error. |
| C++ `AiMaskSegmentationServiceTest.StartImageEndsPreviousSession` | Call order: start(A), end(A), start(B). |
| C++ `AiMaskSegmentationServiceTest.FailedStartKeepsNoSession` | `HasSession() == false`; a later `EndImage` makes no call. |
| C++ `AiMaskSegmentationServiceTest.CloseEndsSessionUnloadsAndReleasesLease` | Order: end, unload, lease release. A second `Close` makes no call. |
| C++ `AiMaskSegmentationServiceTest.DestructorClosesService` | The destructor makes the same calls as `Close`. |

### 7.6 Exit criteria

- [x] `cargo test --release -- --skip live_` passes with the new tests; the count is recorded.
- [x] The C++ test targets pass.
- [x] The Windows manual check loads the model files on DirectML and starts a session; load and
      encoder times are recorded.
- [x] The Core ML adapter type-checks through the shim crate; the Mac build and run are recorded as
      not done.

### 7.7 Expected diff

1,350-1,650 lines: Rust 950-1,100 (with tests), C++ 400-550 (with tests). Lockfile changes are not
counted.

---

## 8. Phase SM3.A.2 — Prompt decoding and port check

Issue: #461.

### 8.1 Objective and deliverables

Decode stroke, click, and box prompts with the previous mask as `mask_input`, with the selection
rules of section 2.4. Prove the port against PyTorch preprocessing and ONNX Runtime 1.31 CPU on the
quality photos.

### 8.2 Implementation steps

1. Add the decode messages to `segmentation.proto`.
2. Write `sam2::validate_prompt`, `sam2::model_prompt`, and `sam2::select_masks`.
3. Add `SegmentationEngine::decode` and the tonic handler.
4. C++: add the prompt and result DTOs, `SegmentationClient::Decode`, and
   `AiMaskSegmentationService::Decode` (fails without a session and makes no call).
5. Write `make_segmentation_port_reference.py`: for each photo of the lab input set
   (`build/tmp/explore/e4/input_4096/photos`, long edge 4096) and a fixed prompt list (one click,
   one box, one 4-point stroke, and one subtract click with the box mask as `mask_input`), write the
   selected logits of the MVP rules with torchvision preprocessing and ONNX Runtime 1.31 CPU.
6. Write `tests/segmentation_port_check.rs`: it runs the engine on the same photos and prompts on
   the device of `ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE` and compares the masks.
7. Record the decoder latency on Windows DirectML for 1, 4, 6, and 10 points.

### 8.3 Tests

| Name | Observable assertion |
| --- | --- |
| Rust `model_prompt_puts_box_corners_first_and_scales_to_1024` | Coordinates and labels of a box with two points. |
| Rust `validate_prompt_rejects_invalid_values` | Coordinate 1.01, NaN, label 2, zero-area box, 65 decoder points, mask input of 100 values, empty prompt: each `InvalidInput`. |
| Rust `click_returns_three_levels_sorted_by_area` | Fake logits of known areas: level order, selected index of the best score, token values. |
| Rust `box_selects_token_zero_when_stable` and `box_selects_best_scored_token_when_unstable` | Selected token 0 or the best of 1 to 3. |
| Rust `selected_logits_are_clipped_to_32` | No value outside +-32. |
| Rust `decode_passes_embedding_and_mask_input_to_decoder` | The fake decoder receives the session embedding, `has_mask_input` 1, and the given mask input. |
| Rust `decode_unknown_session_reports_session_not_found` | `SessionNotFound`. |
| Rust `segmentation_port_check_matches_reference_masks` | For each photo and prompt: IoU of the thresholded logits at 256 x 256 and at the photo size against the reference; the test fails when the photo-size IoU is below 0.999. Skips without `ALCEDO_SEGMENTATION_MODEL_DIR` and `ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR`. |
| C++ `SegmentationClientTest.DecodeRequestCarriesPromptAndMaskInput` | The server sees the points, labels, box, and 65,536 mask values; the client maps three levels and the selected index. |
| C++ `AiMaskSegmentationServiceTest.DecodeWithoutSessionFailsWithoutCall` | `ok == false`; the fake client records no decode. |

### 8.4 Exit criteria

- [x] All tests of section 8.3 pass; the port check passes on Windows DirectML and on the ONNX
      Runtime CPU device.
- [x] The decoder latency record exists.
- [x] The macOS port check is recorded as not run (the Mac is not available).

### 8.5 Expected diff

1,050-1,300 lines: Rust 600-750 (with tests and the port check), C++ 250-300, Python 150-200.

---

## 9. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab\rust\puerh_mind
cmd /c "script\cargo_msvc.cmd test --release -- --skip live_"
cmd /c "script\cargo_msvc.cmd build --release"
```

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SegmentationClientTest AiMaskSegmentationServiceTest --parallel 4
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;" + $env:PATH
ctest --test-dir build/debug -R "SegmentationClientTest|AiMaskSegmentationServiceTest" --output-on-failure
```

```powershell
# SM3.A.2 port check (Windows)
$env:ALCEDO_SEGMENTATION_MODEL_DIR = "<directory of section 2.1>"
$env:ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3a\port_reference"
$env:ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE = "directml"
cmd /c "script\cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture"
```

```bash
# macOS (when the Mac is available)
cd rust/puerh_mind && cargo test --release -- --skip live_
```

## 10. Repository rules that apply

- No fallback: no CPU entry, no other device after an error (A7).
- Run MSVC builds through PowerShell with `scripts\msvc_env.cmd`; run `cargo` through
  `script\cargo_msvc.cmd`.
- Temporary files under `build/tmp/sm3a/`.
- Only targeted test runs; no full `ctest` run.
- Include the defining header; no new forward declarations.
- Prohibited terms: check the new code and documents before completion.

## 11. Completion records

### 11.1 SM3.A.1 completion record

**Phase / date / status:** SM3.A.1, 2026-10-22. Complete on Windows. macOS: the Core ML adapter
type-checks for `aarch64-apple-darwin`; the Mac build and run are not done (the Mac is not
available).

**Source revision and branch:** `3c378d9c` (`main`),
`feature/ai-segmentation-sm3a-service-architecture`.

**Actual changed modules:**

- `rust/puerh_mind/Cargo.toml`, `Cargo.lock`: `ort` `=2.0.0-rc.13`, `api-28` (ONNX Runtime
  1.28.0); `directml` only on Windows, `coreml` only on macOS (the rc.13 binaries exist per
  execution provider set: `coreml` + `directml` on Windows fails at link time with an unresolved
  `OrtGetApiBase`).
- `src/service/ort_runtime.rs`: the provider lists in `#[cfg]` functions, same lists as before.
- `proto/ai_common.proto`: `AI_ERROR_INVALID_INPUT`, `AI_ERROR_SESSION_NOT_FOUND`.
  `proto/segmentation.proto`: the service of section 6.2 (with the SM3.A.2 decode messages).
- `src/service/segmentation/`: `backend.rs`, `tensor.rs`, `model_set.rs`, `ort_backend.rs`,
  `coreml_backend.rs` (macOS), `sam2.rs`, `engine.rs`, `mod.rs` (`platform_backend_factory`).
- `src/server/segmentation.rs`; `registry.rs` (64 MB limit), `bootstrap.rs`, `main.rs`, `build.rs`.
- C++: `AiProto` generates `segmentation.proto`; `sidecar_client/dto/segmentation.hpp`,
  `segmentation_client.{hpp,cpp}` (`MakeGrpcSegmentationClient`: one channel, 64 MB send and
  receive limits); `app/ai_mask_segmentation_service.{hpp,cpp}` (library
  `AiMaskSegmentationService`); tests `SegmentationClientTest`, `AiMaskSegmentationServiceTest`.

**Implemented behavior:** sections 2.1 to 2.3, 2.5 to 2.7. The CPU entry of the DirectML session
only applies `DisableCpuMemArena` (decision A7 is worded to match): `ort`'s CPU provider
registration toggles that session option and nothing else, and DirectML is registered with
`error_on_failure`.

**Primary success call chain:**

```text
AiMaskSegmentationService::LoadModels -> GrpcSegmentationClient::LoadModels (one channel)
  -> SegmentationServiceImpl::load_segmentation_models -> spawn_blocking
  -> SegmentationEngine::load -> platform_backend_factory().create(device)
  -> OrtSegmentationBackend::load (Windows) | CoreMlSegmentationBackend::load (macOS) x2
  -> LoadedModelSet under the write lock -> header OK
AiMaskSegmentationService::StartImage -> EndImage (previous) -> StartSegmentationSession
  -> SegmentationEngine::start_session -> sam2::preprocess_image -> encoder.run
  -> ImageEmbedding stored -> session id
AiMaskSegmentationService::Close -> EndSegmentationSession -> UnloadSegmentationModels
  -> lease released
```

**Primary failure call chain:** a missing model file:
`OrtSegmentationBackend::load` -> "segmentation encoder model <path> does not exist" ->
`SegmentationError::ModelNotReady` -> engine keeps no model -> header
`AI_STATUS_PROVIDER_UNAVAILABLE` / `AI_ERROR_MODEL_NOT_READY` -> `SegmentationCallStatus{ok=false}`.
An unsupported device: `UnsupportedDevice` -> `InvalidInput` -> `AI_STATUS_INVALID_ARGUMENT` /
`AI_ERROR_INVALID_INPUT`, verified against the real sidecar (`"cuda"`).

**Build and test commands with exit codes:**

- `cargo_msvc.cmd build --release --all-targets`: 0.
- `cargo_msvc.cmd test --release -- --skip live_`: 0; 312 + 4 + 5 + 1 passed, 0 failed, 8 filtered
  (`live_`). Before the change: 293 (284 + 4 + 5). The 1 is the port check without its environment.
- `cargo_msvc.cmd test --release --lib segmentation`: 28 passed.
- `cargo fmt --check`: clean. `cargo_msvc.cmd clippy --release --all-targets`: no warning in the
  new files; `register_services` keeps its existing `too_many_arguments` warning (9 arguments,
  8 before); `start_server` has `#[allow(clippy::too_many_arguments)]` (8 arguments, 7 before).
  The clippy run exits 101 on the existing `absurd_extreme_comparisons` error in
  `src/server/semantic.rs:913`, unrelated to this phase.
- macOS type check: `cargo check --target aarch64-apple-darwin --lib --tests` of a shim crate under
  `build/tmp/sm3a/coreml_shim` that includes `backend.rs`, `tensor.rs`, `model_set.rs`, and
  `coreml_backend.rs` by path, with `DOCS_RS=1` (the `coreml` build script then skips the Swift
  bridge): 0, no warning. A planted type error in the shim fails the check, so the check is
  effective. The full crate cannot be checked for macOS on Windows (`onig_sys` needs a macOS libc).
- `msvc_env.cmd --build --preset win_debug --target SegmentationClientTest
  AiMaskSegmentationServiceTest`: 0, no warning in the new files.
- `ctest -R "SegmentationClientTest|AiMaskSegmentationServiceTest"`: 11 of 11 passed.
- Direct callers of the changed `AiProto` and `SidecarClient`: `ctest -R
  "^(AiSidecarRuntimeServiceTest|ImageAnalysisServiceTest)\."`: 64 of 64 passed, 2 environment
  skips (existing live tests).

**Windows manual check (DirectML, RTX 3080 Laptop):** release `alcedo_mind.exe` with a Python gRPC
client (`build/tmp/sm3a/e2e_client.py`): start before load -> `PROVIDER_UNAVAILABLE` /
`MODEL_NOT_READY`; load with `"cuda"` -> `INVALID_ARGUMENT` / `INVALID_INPUT`; load -> OK in
3,123 ms (encoder 2,701 ms, decoder 419 ms); start session with a 4096 x 2736 image (33.6 MB
message) -> OK, cold encoder 322 ms, round trip 387 ms; end -> `ended` true, then false; unload ->
status not loaded.

**Unavailable or not done:**

- macOS build, run, and port check (the Mac is not available).
- The C++ client was not run against the real sidecar; the Python client covered the real
  sidecar, and the C++ client was tested against an in-process C++ server of the same proto.

**Diff size:** about 3,200 lines for SM3.A.1, more than the 1,350-1,650 estimate and more than the
2,000-line limit. Causes: tests (engine 400, server 230, C++ 430 lines) and the formatter layout
of the Rust code. The phase was implemented together with SM3.A.2.

### 11.2 SM3.A.2 completion record

**Phase / date / status:** SM3.A.2, 2026-10-22. Complete on Windows. macOS port check not run.

**Actual changed modules:** `segmentation.proto` decode messages; `sam2::validate_prompt`,
`model_prompt`, `select_masks`; `SegmentationEngine::decode`; the decode handler; C++
`SegmentationClient::Decode`, `AiMaskSegmentationService::Decode`;
`tests/segmentation_port_check.rs`; `scripts/ai_models/sam2/make_segmentation_port_reference.py`.

**Primary success call chain:**

```text
AiMaskSegmentationService::Decode(prompt) -> GrpcSegmentationClient::Decode
  -> SegmentationServiceImpl::decode_segmentation_prompt -> prompt_from_request
  -> SegmentationEngine::decode -> sam2::validate_prompt -> session embedding
  -> sam2::model_prompt -> decoder.run -> sam2::select_masks -> masks, selected index
```

**Primary failure call chain:** an unknown session -> `SegmentationError::SessionNotFound` ->
`AI_STATUS_INVALID_ARGUMENT` / `AI_ERROR_SESSION_NOT_FOUND`; an unknown point label or an invalid
prompt -> `AI_ERROR_INVALID_INPUT`; a decode without a session in the app service fails with no
sidecar call.

**Port check** (`segmentation_port_check_matches_reference_masks`; reference: PyTorch 2.14.1
preprocessing and ONNX Runtime 1.31.0 CPU on the 14 lab photos at a 4096 px long edge, 76 prompts:
clicks, several points, boxes, a 4-point stroke, and a subtract click with the box logits as
`mask_input`):

| Device | Compared masks | Minimum IoU at the photo size | Largest logit difference | Result |
| --- | ---: | ---: | ---: | --- |
| DirectML (`directml:0`) | 128 | 0.999973 | 0.00046 | passed |
| ONNX Runtime CPU | 128 | 0.999964 | not recorded | passed |

Masks, tokens, and selected indices are equal on all prompts. The real sidecar check (Python gRPC)
gave IoU 1.0 at 256 x 256 on the five prompts of the first photo.

**Latency record (DirectML, warm, from the port check; milliseconds, p50):**

| Item | Value |
| --- | ---: |
| Session start (preprocessing of the 4096 px image and encoder) | 149 (first 271) |
| Decoder, 1 point (the point count of the first call) | 8 |
| Decoder, 2, 3, and 4 decoder points | 20 |
| First decoder call after load (real sidecar) | 441 |
| CPU device: session start / decoder | 1,781 / 60 |

The decoder times include the host tensor copies. The 8 ms against 20 ms difference is the DirectML
first-point-count effect of E3; the first-call cost and the per-count sessions belong to SM3.B.

**Build and test commands with exit codes:**

- Reference: `build/tmp/explore/venv/Scripts/python -I
  scripts/ai_models/sam2/make_segmentation_port_reference.py --onnx-dir <lab>/e4/onnx_float32_batch16/tiny
  --photos <lab>/e4/input_4096/photos --prompts <lab>/sm1/quality/prompts.json --out
  build/tmp/sm3a/port_reference`: 0 (lab environment of `alcedo-ai-mask-lab`).
- `cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture` with the
  environment of section 9, device `directml`: 0; device `cpu`: 0.

**Diff size:** about 1,300 lines, inside the 1,050-1,300 estimate.

**Remaining defects or unavailable platforms:** macOS port check not run.

### 11.3 macOS record (2026-10-22, with SM3.B)

The SM3.A port check at `c016dea1` passed on an Apple Silicon Mac (macOS 27.0.1) with the native
Core ML models (`point_encoder.mlmodelc`, `point_decoder.mlmodelc`): 128 masks, minimum photo IoU
0.999964; encoder 241 ms p50, decoder 33 to 34 ms p50 for 1 to 4 decoder points. The SM3.B branch
passed `cargo test` on the same Mac. Details: SM3.B plan, section 13.2.
