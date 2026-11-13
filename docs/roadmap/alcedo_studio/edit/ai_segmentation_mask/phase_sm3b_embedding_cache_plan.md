# SM3.B Image Embedding Cache and First Optimizations — Phase Plan

Date: 2026-10-22.

Status: complete on 2026-10-22 (`feature/ai-segmentation-sm3b`, #463, issue #464). Windows and
macOS are verified (section 13); the macOS build and run of decision B5 are done.

Parent plan: [AI Segmentation Mask Master Plan](../ai_segmentation_mask_master_plan.md), section
9.2.2.

Prerequisites: SM3.A ([phase plan](phase_sm3a_segmentation_service_plan.md)), merged to `main` in
`c016dea1` (#462). The SM3.A macOS build and run are still open; this phase closes them (B5).

---

## 1. Decision record

### 1.1 User request (2026-10-22)

- Complete SM3.B: the image embedding cache, the native residency of the encoder outputs, and the
  DirectML decoder sessions per point count.
- The three open decisions of the master plan outline (cache key, cache size, session limit) are
  closed by the user in this conversation (B1 to B3).
- The phase is complete only after the macOS build and run (B5).

### 1.2 Inherited decisions

- D12, D13, D15 (U6 memory budget: 3 GB on Windows, 3.5 GB on macOS; models unload when the user
  goes back to adjusting).
- SM3.A decisions A1 to A9. In particular A3 (the host keeps the selection; the sidecar keeps only
  image embeddings), A6 (no cancellation protocol), and A7 (no CPU entry after DirectML).
- Master plan section 8.2.3: one decoder session is fast only for the point count of its first
  run on DirectML; padding the point count breaks the masks.

### 1.3 Decisions of this plan

| ID | Decision | Reason |
| --- | --- | --- |
| B1 | User decision. The cache key is an opaque image key that the host supplies. A start request with a key and without pixels uses the cache only: a cached key starts a session without the encoder; an unknown key fails with `AI_ERROR_IMAGE_NOT_CACHED`, and the host then sends the pixels with the same key. SM4 defines the content of the key (the analysis render identity and the model set identity). | A hit then skips the analysis render, the 33.6 MB transfer, the preprocessing, and the encoder. The host owns the image content, so it owns the key. |
| B2 | User decision. The cache keeps at most 4 image embeddings (about 16 MB each), with least-recently-used eviction. Unload and a model change clear the cache. | U6: the memory returns when the user goes back to adjusting. A model change makes the stored features invalid. |
| B3 | User decision. On DirectML, the decoder loads one session for each of the point counts 1 (click), 2 (box), and 4 (stroke), and one session for all other counts. Each session runs once at load with its point count. No session is created on demand. | Within one selection the point count grows with each operation, so counts other than the first operation seldom repeat. An on-demand session costs about 178 ms of load, more than it saves. |
| B4 | The encoder outputs stay in the native form of the backend. ONNX Runtime: the encoder output values, without a copy to a host tensor; each decoder session binds the features once through an `IoBinding` and binds them again only when the features change. Core ML: the encoder output arrays, converted once to Float32 when the model gives Float16; the decoder adds the same arrays to its input for each call. | The decoder call then does not copy the 16 MB of features (master plan section 9.2.2). |
| B5 | User decision. SM3.B is complete only after the macOS build, the macOS unit tests, and the macOS port check with the real Core ML models pass on the Mac. | Master plan section 12: the SM3.A macOS run is open. |
| B6 | The cache shares the image embedding with the sessions. A session and a cache entry of one image hold one embedding; ending the session keeps the cache entry; evicting the entry keeps the session. | No copy of the 16 MB of features. The session limit (4) and the cache limit (4) bound the memory to 8 embeddings. |

### 1.4 Rejected options

- A content digest that the sidecar computes from the pixels as the cache key (B1: it does not
  skip the render and the transfer).
- Decoder sessions created on demand for new point counts (B3).
- A cache that survives an unload (B2).
- Feature tensors on the DirectML device shared across sessions: each ONNX Runtime session with
  DirectML has its own Direct3D 12 device, so a device allocation of the encoder session is not
  valid in a decoder session. The `IoBinding` device copy per decoder session is used instead (B4).

---

## 2. Product and data specification

SM3.B has no user-visible change. Its users are SM4 (the key), SM9 (the start sequence), and
SM3.C (the features in native form).

### 2.1 Start request

| Request | Cache state | Result |
| --- | --- | --- |
| pixels, no key | any | The encoder runs. The session is not cached. `from_cache` false. (SM3.A behavior.) |
| pixels and key | key cached | The session uses the cached embedding. The encoder does not run. The pixels are not read. `from_cache` true. The entry becomes the most recent. |
| pixels and key | key not cached | The encoder runs. The embedding is stored under the key; the least recent entry leaves the cache when 4 entries exist. `from_cache` false. |
| key, no pixels | key cached | As "pixels and key, key cached". |
| key, no pixels | key not cached | `AI_STATUS_INVALID_ARGUMENT` / `AI_ERROR_IMAGE_NOT_CACHED`. No session. The encoder does not run. |
| no key, no pixels | any | `AI_ERROR_INVALID_INPUT`. |

- "No pixels" means width 0, height 0, and an empty byte string. A request with only some of
  these values is invalid.
- The key has 1 to 256 bytes. A longer key is `AI_ERROR_INVALID_INPUT`.
- The session limit of SM3.A (4 sessions) applies to every start, also a cache hit.
- The models must be loaded for every start (`AI_ERROR_MODEL_NOT_READY` otherwise).

### 2.2 Cache rules

- Capacity: 4 entries (B2). Recency changes on an insert and on a hit.
- Unload, and a load with another directory or device, clear the cache with the sessions.
- Ending a session does not remove its cache entry. Evicting an entry does not end its session.
- `GetSegmentationStatus` reports `cached_image_count`.

### 2.3 DirectML decoder sessions

| Decoder point count (points plus 2 for a box) | Session |
| --- | --- |
| 1 | per-count session 1 |
| 2 | per-count session 2 |
| 4 | per-count session 4 |
| 3, 5 to 64 | the general session |

- The load creates the four sessions and runs each twice with zero features and its point count
  (the general session with 3): once without and once with a mask input. The first decode with a
  mask input otherwise takes 21 ms instead of 9 ms (section 13). The load fails with the real error
  when one session fails; no other device is used (A7).
- The `cpu` device has only the general session. Core ML is not changed: its decoder runs on
  `CpuOnly` with no cost for a new point count (U15).
- Each decoder session keeps the features that it bound last. A decode with other features binds
  the new features first.

### 2.4 Errors

| Condition | `status` | `error_code` |
| --- | --- | --- |
| Key without pixels, key not cached | `AI_STATUS_INVALID_ARGUMENT` | `AI_ERROR_IMAGE_NOT_CACHED` (new, 15) |
| No key and no pixels; partial "no pixels" values; key longer than 256 bytes | `AI_STATUS_INVALID_ARGUMENT` | `AI_ERROR_INVALID_INPUT` |

All other errors are as in SM3.A section 2.6.

### 2.5 Threads and ordering

- The SM3.A lock order stays: the model set lock first, then the session map or the cache.
  The engine never holds the session map and the cache lock at the same time.
- Two starts with the same unknown key can run the encoder at the same time. Both insert; the
  second insert replaces the first entry. Both embeddings are correct for the key, so no
  consistency mechanism is necessary.
- The decoder keeps its sessions behind the decoder mutex of SM3.A. The `IoBinding` of a session
  is used only under that mutex.

---

## 3. Scope

### 3.1 Included

| Module | Responsibility |
| --- | --- |
| `rust/puerh_mind/proto/ai_common.proto`, `segmentation.proto` | `AI_ERROR_IMAGE_NOT_CACHED`; `image_key` in the start request; `from_cache` in the start response; `cached_image_count` in the status. |
| `src/service/segmentation/backend.rs` | Role interfaces: `SegmentationEncoder` (pixels to native features), `SegmentationDecoder` (native features and prompt tensors to logits and scores), `ImageFeatures`. |
| `src/service/segmentation/image_embedding_cache.rs` | `ImageEmbeddingCache`: least-recently-used map from key to the shared embedding. |
| `src/service/segmentation/engine.rs` | Start with a key, cache hit and miss, clear on unload and model change, status count. |
| `src/service/segmentation/test_support.rs` | The fake backend, moved from `engine.rs` and changed to the role interfaces. |
| `src/service/segmentation/ort_backend.rs` | Encoder that keeps its output values. |
| `src/service/segmentation/ort_decoder.rs` | Decoder sessions per point count, warm-up runs, `IoBinding` with the bound features. |
| `src/service/segmentation/coreml_backend.rs` | Encoder that keeps its output arrays (Float32); decoder that adds them to its input. |
| `src/server/segmentation.rs` | Key and "no pixels" mapping, the new error code, the new response fields. |
| `tests/segmentation_port_check.rs` | Cache hits and misses with the real models; latency of the hit. |
| C++ `sidecar_client` | `image_key` in `StartSession`; `from_cache`, `image_not_cached`, `cached_image_count` in the DTOs. |
| C++ `AiMaskSegmentationService` | `StartImage(image, key)`, `StartCachedImage(key)`. |

### 3.2 Excluded

| Item | Owner |
| --- | --- |
| The content of the image key | SM4 |
| The start sequence of the tool (cached start, render on a miss) | SM9 |
| Head and S3OD on the native features | SM3.C |

---

## 4. Current source audit

Source revision: `c016dea1` (`main`).

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Model interface | `segmentation/backend.rs` `SegmentationModel::run(&[&TensorF32]) -> Vec<TensorF32>` | Every model input and output is a host tensor. | Role interfaces with native features (B4). |
| Encoder outputs | `engine.rs` `ImageEmbedding` | Three `TensorF32` values (16 MB) per session; `ort_backend.rs` copies each output with `to_vec()`. | Native features shared by sessions and cache (B4, B6). |
| Decoder inputs, ONNX Runtime | `ort_backend.rs` `OrtSegmentationModel::run` | `session.run` with tensor views; DirectML copies the 16 MB of features to the device for each call. | `IoBinding` per decoder session (B4). |
| Decoder inputs, Core ML | `coreml_backend.rs` `CoreMlSegmentationModel::run` | Allocates and fills a new `MultiArray` for each input of each call. | Add the stored arrays (B4). |
| Decoder sessions | `ort_backend.rs` | One session; fast only for its first point count (8 ms, others 20 ms); first call after load 441 ms. | Sessions per point count with warm-up runs (B3). |
| Sessions | `engine.rs` `sessions: Mutex<HashMap<String, Arc<ImageEmbedding>>>`, `MAX_SESSIONS = 4` | Each start runs the encoder. | Cache (B1, B2, B6). |
| C++ start | `segmentation_client.cpp` `StartSession(image, timeout)`; `AiMaskSegmentationService::StartImage(image)` | Always sends the pixels. | Key and key-only start. |

---

## 5. Target architecture

### 5.1 Rust owners

```text
SegmentationEngine
  owns: RwLock<Option<LoadedModelSet>>
        Mutex<HashMap<session id, Arc<ImageEmbedding>>>
        Mutex<ImageEmbeddingCache>                       (key -> Arc<ImageEmbedding>, 4 entries)
  LoadedModelSet { encoder: Mutex<Box<dyn SegmentationEncoder>>,
                   decoder: Mutex<Box<dyn SegmentationDecoder>> }
  ImageEmbedding { features: Arc<dyn ImageFeatures> }   (native form of the backend)
backend traits
  SegmentationEncoder::encode(pixel_values) -> Arc<dyn ImageFeatures>
  SegmentationDecoder::decode(&Arc<dyn ImageFeatures>, DecoderPrompt) -> DecoderOutput
ort_backend::OrtEncoder          -> OrtImageFeatures { image_embed, high_res_feats_0, high_res_feats_1: DynValue }
ort_decoder::OrtDecoder          -> Vec<OrtDecoderSession { point_count, session, binding, bound features }>
coreml_backend::CoreMlEncoder    -> CoreMlImageFeatures { FeatureProvider with Float32 arrays }
coreml_backend::CoreMlDecoder    -> one model, adds the stored arrays to each input
```

- The engine does not name ONNX Runtime or Core ML types. An adapter reads its own feature type
  through `ImageFeatures::as_any`. Features of another backend are an inference error.
- A decoder session holds a strong reference to the features that it bound, so the features stay
  valid while they are bound, and the identity comparison (`Arc::ptr_eq`) cannot match a new
  allocation at a reused address.

### 5.2 C++ owners

```text
SM9 tool controller (future)
  -> AiMaskSegmentationService::StartCachedImage(key)
       image_not_cached -> SM4 analysis render -> StartImage(image, key)
  -> GrpcSegmentationClient::StartSession(image, key)
```

### 5.3 Primary success call chain

```text
AiMaskSegmentationService::StartCachedImage(key)
  -> EndImage (previous) -> GrpcSegmentationClient::StartSession(empty image, key)
  -> SegmentationServiceImpl::start_segmentation_session -> spawn_blocking
  -> SegmentationEngine::start_session -> ImageEmbeddingCache::get(key) hit
  -> session id with the shared embedding, from_cache true
AiMaskSegmentationService::Decode(prompt)
  -> SegmentationEngine::decode -> SegmentationDecoder::decode(features, prompt)
  -> OrtDecoder: session for the point count -> bind features if they changed -> run_binding
  -> sam2::select_masks -> masks
```

### 5.4 Primary failure call chain

```text
StartCachedImage(key not cached)
  -> engine: cache miss and no pixels -> SegmentationError::ImageNotCached
  -> header INVALID_ARGUMENT / IMAGE_NOT_CACHED -> SegmentationSessionResult{image_not_cached=true}
  -> no session; the host renders and calls StartImage(image, key)
DirectML per-count session load failure
  -> OrtDecoder::load returns the error -> engine keeps no models -> MODEL_NOT_READY
```

---

## 6. File and API map

### 6.1 Main APIs

```rust
pub trait ImageFeatures: Send + Sync {
    fn as_any(&self) -> &dyn Any;
}
pub trait SegmentationEncoder: Send {
    fn encode(&mut self, pixel_values: &TensorF32) -> Result<Arc<dyn ImageFeatures>>;
}
pub struct DecoderPrompt { pub point_coords: TensorF32, pub point_labels: TensorF32,
                           pub mask_input: TensorF32, pub has_mask_input: TensorF32 }
pub struct DecoderOutput { pub mask_logits: TensorF32, pub iou_scores: TensorF32 }
pub trait SegmentationDecoder: Send {
    fn decode(&mut self, features: &Arc<dyn ImageFeatures>, prompt: &DecoderPrompt)
        -> Result<DecoderOutput>;
}
pub trait SegmentationBackend: Send + Sync {
    fn backend_name(&self) -> &str;
    fn device_label(&self, role: SegmentationModelRole) -> String;
    fn load_encoder(&self, directory: &Path) -> Result<Box<dyn SegmentationEncoder>>;
    fn load_decoder(&self, directory: &Path) -> Result<Box<dyn SegmentationDecoder>>;
}
pub struct StartSessionRequest<'a> { pub image: Option<RgbImageView<'a>>, pub image_key: &'a str }
impl SegmentationEngine {
    pub fn start_session(&self, request: StartSessionRequest<'_>)
        -> Result<StartedSession, SegmentationError>;   // StartedSession.from_cache
}
```

```cpp
// SegmentationClient
virtual auto StartSession(const SegmentationRgb8Image& image, const std::string& image_key,
                          std::chrono::milliseconds timeout) -> SegmentationSessionResult = 0;
// AiMaskSegmentationService
auto StartImage(const SegmentationRgb8Image& image, const std::string& image_key = {})
    -> SegmentationSessionResult;
auto StartCachedImage(const std::string& image_key) -> SegmentationSessionResult;
```

---

## 7. Implementation steps

1. Read `AGENTS.md`, `alcedo-msvc-cmake`, and `execute-phase-plan` again.
2. Proto: `AI_ERROR_IMAGE_NOT_CACHED = 15`; `StartSegmentationSessionRequest.image_key = 5`;
   `StartSegmentationSessionResponse.from_cache = 4`; `GetSegmentationStatusResponse.cached_image_count = 9`.
3. `backend.rs`: the role interfaces of section 6.1. Move the fake backend to `test_support.rs`.
4. `image_embedding_cache.rs`: `ImageEmbeddingCache::{new(capacity), get, insert, clear, len}`.
5. `engine.rs`: `start_session(StartSessionRequest)` with the rules of section 2.1;
   `SegmentationError::ImageNotCached`; clear the cache in `load` (model change) and `unload`;
   `cached_image_count` in the status.
6. `ort_backend.rs`: `OrtEncoder` keeps the three output values; checks their shapes.
7. `ort_decoder.rs`: `decoder_session_counts(device)`; `OrtDecoder::load` creates the sessions and
   runs the warm-up; `decode` selects the session, binds the features when they changed (input
   values of the features and a strong reference), binds the prompt tensors, and runs the
   binding with outputs on the CPU.
8. `coreml_backend.rs`: `CoreMlEncoder` keeps a `FeatureProvider` with Float32 arrays;
   `CoreMlDecoder` adds the arrays with `get_multi_array` (a retained reference, no copy).
9. Server: key and "no pixels" mapping (section 2.1), `ImageNotCached` mapping, new fields.
10. Port check: start each photo with its name as the key; after all photos, start the last 4
    photos with the key only and decode their first prompt again (compare with the reference);
    start the first photo with the key only and expect `ImageNotCached`. Print the hit time.
11. C++: DTO fields, client key and flags, service `StartImage(image, key)` and
    `StartCachedImage(key)`.
12. Measure on Windows DirectML against the SM3.A record (section 9).
13. macOS: build, `cargo test`, and the port check with the Core ML models on the Mac (B5).

---

## 8. Tests

| Name | Observable assertion |
| --- | --- |
| Rust `cache_evicts_least_recently_used_entry` | Capacity 2: insert a, b; get a; insert c: b is gone, a and c stay. |
| Rust `cache_insert_of_existing_key_replaces_entry` | The second value is returned; the length stays 1. |
| Rust `start_with_cached_key_skips_encoder_and_shares_embedding` | Second start with the same key and pixels: encoder runs 1, `from_cache` true; the decoder sees the same features object for both sessions. |
| Rust `key_only_start_reports_image_not_cached_without_encoder` | `ImageNotCached`; encoder runs 0; session count 0. |
| Rust `key_only_start_hits_after_session_end` | Start with key and pixels, end the session, key-only start: `from_cache` true, encoder runs 1. |
| Rust `cache_evicts_fifth_key` | Keys k1 to k5 (ending each session): key-only k1 is `ImageNotCached`; k2 to k5 hit; `cached_image_count` 4. |
| Rust `unload_and_model_change_clear_cached_images` | After unload and after a load with another directory: count 0 and key-only start `ImageNotCached` (after a new load). |
| Rust `start_rejects_invalid_key_and_partial_empty_image` | Key of 257 bytes; width 0 with bytes; no key and no pixels: each `InvalidInput`, encoder runs 0. |
| Rust `decoder_session_counts_follow_device` | DirectML: [1, 2, 4] plus the general session; CPU: the general session only. |
| Rust `decoder_session_for_point_count_routes_counts` | DirectML: 1, 2, 4 to their sessions; 3, 5, 64 to the general session. |
| Rust `server_maps_image_not_cached_to_invalid_argument` | Header `INVALID_ARGUMENT` / `IMAGE_NOT_CACHED`; empty session id. |
| Rust `server_reports_from_cache_and_cached_image_count` | Second start with the key: `from_cache` true; status `cached_image_count` 1. |
| Rust `segmentation_port_check_matches_reference_masks` (extended) | The SM3.A checks, and: key-only starts of the last 4 photos hit and give masks with IoU at least 0.999 against the reference; the key-only start of the first photo is `ImageNotCached`. |
| C++ `SegmentationClientTest.StartSessionSendsImageKeyWithoutPixels` | The server sees the key and 0 bytes; `from_cache` maps to true. |
| C++ `SegmentationClientTest.ImageNotCachedHeaderSetsImageNotCached` | `ok` false, `image_not_cached` true. |
| C++ `SegmentationClientTest.SequentialCallsUseOneConnection` (extended) | The `cached_image_count` of the server reaches the DTO. |
| C++ `AiMaskSegmentationServiceTest.StartCachedImageEndsPreviousSessionAndSendsKeyOnly` | Call order: end(A), start(key, 0 bytes); the new session is current. |
| C++ `AiMaskSegmentationServiceTest.StartCachedImageMissKeepsNoSession` | `image_not_cached` true; `HasSession()` false. |
| C++ `AiMaskSegmentationServiceTest.StartImageSendsImageKey` | The fake client receives the pixels and the key. |
| C++ `AiMaskSegmentationServiceTest.StartCachedImageWithEmptyKeyFailsWithoutCall` | `ok` false; the fake client records no call. |
| Rust `coreml_features_keep_float32_array_without_copy` (macOS) | Two retained references of one stored array read the same storage. |

---

## 9. Measurements

Windows, RTX 3080 Laptop, DirectML, release build. Compare with the SM3.A record (SM3.A plan
section 11.2).

| Item | Method |
| --- | --- |
| Decoder time for 1, 2, 3, 4, and more decoder points | Port check output (p50). |
| First decoder call after load | Real sidecar, Python gRPC client. |
| Model load time | Port check and real sidecar. |
| Dedicated GPU memory of the sidecar after load | `Get-Counter "\GPU Process Memory(pid_<pid>*)\Dedicated Usage"` before and after the change. |
| Start: miss with pixels; hit with the key only | Real sidecar round trip; port check `encoder_ms`. |

macOS (B5): the port check with the Core ML models; decoder time; start hit time.

---

## 10. Build and run commands

```powershell
Set-Location D:\Projects\pu-erh_lab\rust\puerh_mind
cmd /c "script\cargo_msvc.cmd test --release -- --skip live_"
$env:ALCEDO_SEGMENTATION_MODEL_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3a\models\alcedo-ai-mask-v1"
$env:ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR = "D:\Projects\pu-erh_lab\build\tmp\sm3a\port_reference"
$env:ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE = "directml"
cmd /c "script\cargo_msvc.cmd test --release --test segmentation_port_check -- --nocapture"
```

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SegmentationClientTest AiMaskSegmentationServiceTest --parallel 4
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;" + $env:PATH
ctest --test-dir build/debug -R "SegmentationClientTest|AiMaskSegmentationServiceTest" --output-on-failure
```

```bash
# macOS (B5)
cd rust/puerh_mind && cargo test --release -- --skip live_
ALCEDO_SEGMENTATION_MODEL_DIR=<directory with the .mlmodelc models> \
ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR=<copy of the port reference> \
cargo test --release --test segmentation_port_check -- --nocapture
```

## 11. Repository rules that apply

- No fallback: a failed per-count session is a load error; no other device (A7).
- Run MSVC builds through PowerShell with `scripts\msvc_env.cmd`; run `cargo` through
  `script\cargo_msvc.cmd`.
- Temporary files under `build/tmp/sm3b/`.
- Only targeted test runs; no full `ctest` run.
- The identity of the bound features is a strong reference, not a version or token (AGENTS.md:
  the address reuse after an eviction is a sequential, executable case).
- Prohibited terms: check the new code and documents before completion.

## 12. Expected diff

900-1,300 lines: Rust 650-900 (with tests), C++ 200-300 (with tests), documents 100.

## 13. Completion record

### 13.1 SM3.B completion record (2026-10-22)

**Status:** complete. Windows: implemented and verified. macOS: built and verified on the Mac
(section 13.2), so the requirement of decision B5 is met.

**Source revision and branch:** uncommitted work on `c016dea1` (`main` after #462),
`feature/ai-segmentation-sm3b`.

**Actual changed modules:**

- Proto: `AI_ERROR_IMAGE_NOT_CACHED = 15`; `image_key`, `from_cache`, `cached_image_count`.
  `ai_common.proto`: one existing comment reworded for the terminology rules.
- `segmentation/backend.rs`: `ImageFeatures`, `SegmentationEncoder`, `SegmentationDecoder`,
  `DecoderPrompt`, `DecoderOutput`, `features_of`; `SegmentationModel` is removed.
- `segmentation/image_embedding_cache.rs` (new), `segmentation/test_support.rs` (new, the fake
  backend moved out of `engine.rs`), `segmentation/ort_decoder.rs` (new).
- `segmentation/engine.rs`: `StartSessionRequest`, `StartedSession.from_cache`,
  `SegmentationError::ImageNotCached`, the cache, `cached_image_count`.
- `segmentation/ort_backend.rs`: `OrtEncoder` keeps the output values (`OrtImageFeatures`);
  `load_session` is shared with the decoder.
- `segmentation/coreml_backend.rs`: `CoreMlImageFeatures`, `CoreMlEncoder`, `CoreMlDecoder`.
- `server/segmentation.rs`: key and "no pixels" mapping; new error mapping and fields.
- `tests/segmentation_port_check.rs`: cache hit and miss checks; times in microseconds; a steady
  decode time (20 decodes of one prompt on one session).
- C++: DTO fields `from_cache`, `image_not_cached`, `cached_image_count`;
  `SegmentationClient::StartSession(image, image_key, timeout)`;
  `AiMaskSegmentationService::StartImage(image, image_key)` and `StartCachedImage(image_key)`.

**Primary success call chain:**

```text
AiMaskSegmentationService::StartCachedImage(key) -> EndImage (previous)
  -> GrpcSegmentationClient::StartSession(empty image, key)
  -> SegmentationServiceImpl::start_segmentation_session -> spawn_blocking
  -> SegmentationEngine::start_session -> ImageEmbeddingCache::get(key) hit
  -> session with the shared Arc<dyn ImageFeatures> -> from_cache true
AiMaskSegmentationService::Decode(prompt) -> SegmentationEngine::decode
  -> OrtDecoder::decode -> session_index_for(point count)
  -> DecoderSession: bind features only when they changed -> bind prompt -> run_binding
  -> sam2::select_masks -> masks
```

**Primary failure call chain:**

```text
StartCachedImage(uncached key) -> engine: no cache entry and no pixels
  -> SegmentationError::ImageNotCached -> AI_STATUS_INVALID_ARGUMENT / AI_ERROR_IMAGE_NOT_CACHED
  -> SegmentationSessionResult{ok=false, image_not_cached=true}, no session
  -> caller: analysis render -> StartImage(image, key) -> encoder -> cache insert
OrtDecoder::load: a session or its warm-up run fails
  -> load error "failed to run the decoder session for N points: ..." -> MODEL_NOT_READY,
     no models loaded
```

**What was proven (executed tests):**

| Criterion | Target | Result |
| --- | --- | --- |
| Rust tests of section 8 (cache, engine, server, decoder routing) | `cargo test --release -- --skip live_` | 324 + 4 + 5 + 1 passed, 0 failed, 8 filtered (`live_`); before: 312 + 4 + 5 + 1 |
| `segmentation_port_check_matches_reference_masks`, DirectML | real models, 14 photos | passed: 140 masks (128 + 12 from cached starts), minimum photo IoU 0.999973; first photo key-only start `ImageNotCached` |
| Same, ONNX Runtime CPU | real models | passed: 140 masks, minimum photo IoU 0.999964 |
| C++ section 8 tests | `SegmentationClientTest`, `AiMaskSegmentationServiceTest` | 17 of 17 passed |
| Direct callers of `AiProto` and `SidecarClient` | `AiSidecarRuntimeServiceTest`, `ImageAnalysisServiceTest` with the two above | 79 passed, 2 environment skips (existing live tests), 0 failed |
| Core ML adapter type check | shim crate `build/tmp/sm3b/coreml_shim`, `cargo check --target aarch64-apple-darwin --lib --tests` | 0, no warning |
| `coreml_features_keep_float32_array_without_copy` | macOS | passed (section 13.2) |

Commands: section 10; `cargo fmt --check` clean; `cargo_msvc.cmd clippy --release --all-targets`:
no finding in the changed files; the run exits 101 on the existing `semantic.rs:913`
`absurd_extreme_comparisons` error (also in SM3.A). `git clang-format` on the changed C++ lines.

**Measurements (Windows, RTX 3080 Laptop, DirectML, release):**

Port check, same machine and day, SM3.A code (`main`) against SM3.B; decode times include the
host tensor copies and the mask selection:

| Item | SM3.A | SM3.B |
| --- | ---: | ---: |
| Decoder load (port check) | 197 ms | 1,057 ms |
| Decode p50, 1 decoder point | 8 ms | 8.0 ms |
| Decode p50, 2 / 3 / 4 decoder points | 20 / 19 / 20 ms | 8.1 / 7.7 / 8.0 ms |
| First decode after load | not printed | 9.3 ms |
| Same prompt on the same session (20 calls) | not measured | 7.0 ms |
| Same, with a forced bind of the features on each call (temporary build) | | 7.9 ms (bound once: 6.5 ms in the same series) |
| Start from the cache (engine) | none | 4 to 9 us |

Real sidecar with a Python gRPC client (`build/tmp/sm3b/e2e_client.py`), photo 1, two runs each:

| Item | SM3.A | SM3.B |
| --- | ---: | ---: |
| Model load request | 1,782 / 2,804 ms | 2,637 / 2,994 ms |
| Dedicated GPU memory after load | 232 MB | 578 MB |
| Dedicated GPU memory after start and 5 decodes | 1,354.5 MB | 1,634.5 MB |
| First decode (1 point) | 95 / 405 ms | 9 / 9 ms |
| Decodes with 2, 3, 4 points | 20 to 38 ms | 7 to 10 ms |
| First decode with a mask input (3 points) | 20 / 22 ms | 9 / 9 ms (21 ms with one warm-up run) |
| Start with pixels, cold encoder | 500 / 555 ms round trip | 399 / 556 ms |
| Start with the key only, cached | not available | 1.8 / 1.9 ms round trip |
| Start with pixels and a cached key | not available | 55 / 62 ms (transfer of 33.6 MB) |
| Key-only start of an unknown key | not available | `INVALID_ARGUMENT` / 15, no session |

GPU memory: the per-count sessions and their bound features add 280 MB after decodes. The lab
figure for all models (SM1, 2,666 MB in the Python process) plus 280 MB is about 2,950 MB, inside
the U6 budget of 3 GB but with little margin. SM3.C must measure the sidecar with all models.

macOS: section 13.2.

**Checklist / exit condition:** all steps of section 7 and all tests of section 8 are done on
Windows and macOS.

**LOC note:** about 2,200 inserted and 650 removed lines, of which 410 are documents. The code is
above the 900-1,300 estimate: the engine tests were rewritten for the role interfaces (the fake
backend moved to `test_support.rs`, 169 lines) and the port check was restructured. The largest
file is `engine.rs` with 896 lines, of which 480 are tests.

**Remaining gaps:**

- The C++ client was not run against the real sidecar; the Python client covered the real sidecar.
- The image key content is open until SM4.

### 13.2 macOS record (2026-10-22)

Mac: Apple Silicon (arm64), macOS 27.0.1, rustc 1.90.0, branch `feature/ai-segmentation-sm3b` at
`6a52abfc`. Core ML models: `point_encoder.mlmodelc` and `point_decoder.mlmodelc` of the lab
evidence `build/tmp/explore/coreml/float32/`. Port reference: a copy of the Windows
`build/tmp/sm3a/port_reference` (14 photos, the same prompts).

| Check | Result |
| --- | --- |
| `cargo test --release -- --skip live_` | 327 + 4 + 5 + 1 passed, 0 failed, 0 warnings; includes `coreml_features_keep_float32_array_without_copy` and all SM3.B engine and cache tests |
| Port check, device `""` (encoder `ComputeUnits::All`, decoder `CpuOnly`) | passed: 140 masks, minimum photo IoU 0.999964; key-only start of the first photo `ImageNotCached` |
| Port check, device `cpu` (all models `CpuOnly`) | passed: 140 masks, minimum photo IoU 0.999973 |
| SM3.A port check at `c016dea1` on the same Mac | passed: 128 masks, minimum photo IoU 0.999964 (the open SM3.A macOS port check) |
| C++ `SegmentationClientTest`, `AiMaskSegmentationServiceTest` | 17 of 17 passed in the `macOS arm64 Metal` CI job of #463 |

Core ML timing, port check, same Mac and day (milliseconds, p50):

| Item | SM3.A | SM3.B |
| --- | ---: | ---: |
| Model load | 67 + 67 (warm) | 54 to 439 + 64 to 67 |
| Encoder (preprocessing and encoder) | 241 | 230 to 232 |
| Decode, 1 / 2 / 3 / 4 decoder points | 33 / 33 / 34 / 34 | 33.5 / 33.3 / 33.0 / 33.5 |
| Same prompt on the same session (20 calls) | not measured | 29.1 |
| Same, with a strided read and copy of the features on each call (temporary build) | | 38.6 |
| Start from the cache (engine) | none | 6 to 7 us |

The Core ML decode time does not change: the decoder runs on the CPU (U15) and its run dominates.
SM3.A copied the host tensors into new arrays with one memory copy for each call; SM3.B adds
retained references to the stored arrays and does not copy. The temporary build, which reads
the arrays through their strides and copies them on each call, is 9.5 ms slower. The cache saves
the encoder (230 ms) for each cached start. The first encoder run after a load took 3,039 ms in the
first run on this Mac (Core ML preparation) and 292 ms in later processes.
