# AI Sidecar Backend — Phase 0 Contract Inventory and Gates

Date: 2026-06-25
Primary roadmap owner: `alcedo_studio/src/ai` (cross-module sidecar/app integration)
Status: Phase 0 complete. This document freezes the compatibility boundary that
Phase 1 (`proto/ai_common.proto`, `proto/ai_runtime.proto`, multi-proto generation)
must not cross.

It is the authoritative answer to every Phase 0 deliverable in
[`ai_sidecar_backend_plan.md`](ai_sidecar_backend_plan.md) (lines 120-141). All
file/line references below were verified against the current `feature/alcedo_brain`
HEAD on 2026-06-25.

---

## 0. Terminology frozen here

- **Cargo package / binary name** is `alcedo_mind` (`rust/puerh_mind/Cargo.toml:2`);
  the source directory is `rust/puerh_mind/`. Env-var prefix is `ALCEDO_MIND_*`
  (`src/config.rs:69-83`). The resolved-manifest filename is
  `alcedo_model_manifest.json` (`src/service/model_assets.rs:17`). Phase 0 does not
  rename anything.
- **`request_id`** = per-call correlation id (UUID-ish), echoed in the response.
- **`task_id`** = stable task *type* identifier (e.g. `"semantic.embed_text"`,
  `"image_understanding.describe"`). It names a capability, not a call.
- **Control surface** = `AiRequestHeader` / `AiResponseHeader` / `AiCapability` and
  the status-code enums. Nothing else.
- **Legacy-compatible** = the RPC signature, request/response field numbers, and
  observable behavior are frozen for the entire migration. A v2 may be added
  *alongside*; the v1 is not edited.

---

## 1. Shared header fields and status-code mapping (Deliverable 1)

### 1.1 Package decision

Both new proto files share **one** package, `alcedo.ai`:

- `proto/ai_common.proto` — `package alcedo.ai;` — headers, status enums, capability
  descriptor. No service.
- `proto/ai_runtime.proto` — `package alcedo.ai;` — `import "ai_common.proto";` —
  the `AiRuntimeService` (sidecar capability/runtime service).

Rationale (frozen here, resolves the plan's Open Decision on package naming):

- A single package means `ai_runtime.proto` references `AiRequestHeader`
  unqualified (same package). This avoids `prost` cross-package `extern_path`
  configuration and avoids C++ fully-qualified churn — both of which are fragile on
  MSVC. `tonic_prost_build` emits one Rust module per package, so a single
  `tonic::include_proto!("alcedo.ai")` brings in every type from both files.
- The `alcedo.` prefix avoids a bare top-level `ai` namespace colliding with future
  third-party protos. Existing project protos keep their bare packages (`common`,
  `semantic`); they are frozen legacy and are not renamed.
- C++ namespace becomes `alcedo::ai`. The app already uses `alcedo::` for DTO
  structs (e.g. `alcedo::SemanticRuntimeModelInfo`); `alcedo::ai::AiRequestHeader`
  nests cleanly with no collision.

`google/protobuf/*.proto` well-known types are **not** imported. The existing
`build.rs` (`rust/puerh_mind/build.rs:9-14`) uses `tonic_prost_build` defaults with
no `compile_well_known_types` and only `proto/` on the proto path; introducing WKT
imports would require a new include path and `prost` WKT config. Timeouts are
therefore `int64 ..._ms` scalars, not `google.protobuf.Duration`.

### 1.2 `AiRequestHeader` (`ai_common.proto`)

```proto
message AiRequestHeader {
  string request_id      = 1;  // per-call correlation; echoed in response
  string task_id         = 2;  // stable task type id, e.g. "semantic.embed_text"
  int64  timeout_ms      = 3;  // relative deadline from receipt; 0 = server default
  AiPriority priority    = 4;  // defaults to NORMAL when UNSPECIFIED
  string trace_id        = 5;  // optional cross-cutting trace; empty allowed
  string credential_ref  = 6;  // opaque handle from the Rust credential vault;
                               //   empty = no credential (local task)
  repeated string client_capabilities = 7;  // tokens the client understands,
                               //   e.g. "cancel-by-request_id"
}
```

**`timeout_ms` (relative), not `deadline_ms` (absolute).** Decision: the C++ client
already works entirely in relative timeouts — `GrpcSemanticRuntimeClient` sets
`context.set_deadline(DeadlineFromNow(timeout))` at every call
(`alcedo_studio/src/app/semantic_runtime_service.cpp:122-124`, 12 call sites), and
`SemanticRuntimeOptions` carries relative `startup_timeout` / `graceful_stop_timeout`
(`semantic_runtime_service.hpp:161-164`). An absolute deadline would additionally
require clock sync between host and sidecar. Relative wins and matches existing
behavior.

**`credential_ref` is a string handle, never key material.** Phase 0 defines only
the field; the vault that mints handles is Phase 3. A dedicated `CredentialHandle`
message is deliberately *not* introduced now (Phase 0 self-review: do not over-design
the control plane). Empty `credential_ref` means "no credential required" (local
tasks such as CLIP embedding).

### 1.3 `AiResponseHeader` (`ai_common.proto`)

```proto
message AiResponseHeader {
  string request_id    = 1;
  string task_id       = 2;
  AiResponseStatus status        = 3;
  AiErrorCode error_code         = 4;  // set only when status != OK
  string error_message           = 5;  // human-readable, redacted of secrets
  string provider                = 6;  // provider_id that actually served
  string model_id                = 7;  // model_id that actually served
  int64  elapsed_ms              = 8;
}
```

`error_message` must never contain credential material, full request payloads, or
remote-provider raw headers — only a redacted summary. This is the field the Phase 3
self-review ("keys never appear in ... crash messages") will grep for.

### 1.4 Enums (`ai_common.proto`)

```proto
enum AiPriority {
  AI_PRIORITY_UNSPECIFIED  = 0;
  AI_PRIORITY_LOW          = 1;
  AI_PRIORITY_NORMAL       = 2;
  AI_PRIORITY_HIGH         = 3;
  AI_PRIORITY_USER_INITIATED = 4;
}

enum AiResponseStatus {
  AI_STATUS_UNSPECIFIED        = 0;
  AI_STATUS_OK                 = 1;
  AI_STATUS_CANCELLED          = 2;
  AI_STATUS_DEADLINE_EXCEEDED  = 3;
  AI_STATUS_INVALID_ARGUMENT   = 4;
  AI_STATUS_PAYLOAD_TOO_LARGE  = 5;
  AI_STATUS_UNAUTHENTICATED    = 6;  // credential missing
  AI_STATUS_PERMISSION_DENIED  = 7;  // credential invalid / expired / revoked
  AI_STATUS_PROVIDER_UNAVAILABLE = 8;
  AI_STATUS_PROVIDER_ERROR     = 9;
  AI_STATUS_UNSUPPORTED_TASK   = 10;
  AI_STATUS_UNIMPLEMENTED      = 11;
  AI_STATUS_INTERNAL           = 12;
}

enum AiErrorCode {
  AI_ERROR_NONE                = 0;
  AI_ERROR_MISSING_HEADER      = 1;
  AI_ERROR_MISSING_CREDENTIAL  = 2;
  AI_ERROR_CREDENTIAL_REVOKED  = 3;
  AI_ERROR_CREDENTIAL_EXPIRED  = 4;
  AI_ERROR_PROVIDER_TIMEOUT    = 5;
  AI_ERROR_PROVIDER_RATE_LIMITED = 6;
  AI_ERROR_PROVIDER_5XX        = 7;
  AI_ERROR_MODEL_NOT_READY     = 8;
  AI_ERROR_TASK_UNKNOWN        = 9;
  AI_ERROR_PAYLOAD_DECODE      = 10;
  AI_ERROR_CANCELLED_BY_CLIENT = 11;
  AI_ERROR_INTERNAL            = 12;
}
```

### 1.5 Status → tonic mapping (Rust impl contract)

The Rust server maps `AiResponseStatus` to `tonic::Status` for transport-level
behavior, while the typed `AiResponseHeader` carries the precise outcome inside the
body. This dual representation lets legacy v1 callers (no header) keep getting plain
tonic codes while v2 callers get the structured header.

| `AiResponseStatus`        | `tonic::Code`         | Retryable? | Notes |
|---------------------------|-----------------------|------------|-------|
| `OK`                      | `Ok`                  | n/a        | success |
| `CANCELLED`               | `Cancelled`           | no         | client or server cancel |
| `DEADLINE_EXCEEDED`       | `DeadlineExceeded`    | yes (backoff) | `timeout_ms` exceeded |
| `INVALID_ARGUMENT`        | `InvalidArgument`     | no         | bad payload/shape |
| `PAYLOAD_TOO_LARGE`       | `InvalidArgument`     | no         | exceeds `max_payload_bytes` |
| `UNAUTHENTICATED`         | `Unauthenticated`     | no         | `credential_ref` empty but required |
| `PERMISSION_DENIED`       | `PermissionDenied`    | no         | handle revoked/expired/unknown |
| `PROVIDER_UNAVAILABLE`    | `Unavailable`         | yes        | remote provider offline / DNS / connect |
| `PROVIDER_ERROR`          | `Internal`            | maybe      | provider returned non-retryable error |
| `UNSUPPORTED_TASK`        | `Unimplemented`       | no         | `task_id` not in registry |
| `UNIMPLEMENTED`           | `Unimplemented`       | no         | task known but not built on this sidecar |
| `INTERNAL`                | `Internal`            | no         | unexpected server fault |

### 1.6 `AiCapability` (`ai_common.proto`)

```proto
enum AiInputKind {
  AI_INPUT_UNSPECIFIED = 0;
  AI_INPUT_TEXT        = 1;
  AI_INPUT_IMAGE       = 2;   // full-res bytes
  AI_INPUT_THUMBNAIL   = 3;   // host-rendered thumbnail
  AI_INPUT_PREVIEW     = 4;   // host-rendered preview
  AI_INPUT_METADATA    = 5;
  AI_INPUT_PIPELINE_PARAMS = 6;
}
enum AiOutputKind {
  AI_OUTPUT_UNSPECIFIED = 0;
  AI_OUTPUT_EMBEDDING   = 1;
  AI_OUTPUT_CAPTION     = 2;
  AI_OUTPUT_TAGS        = 3;
  AI_OUTPUT_SCORE       = 4;
  AI_OUTPUT_RECIPE      = 5;  // future edit-assistant
}
message AiCapability {
  string task_id              = 1;
  string provider_id          = 2;  // "local", "mock", "openai", ...
  string model_id             = 3;
  repeated AiInputKind input_kinds  = 4;
  repeated AiOutputKind output_kinds = 5;
  bool supports_batch         = 6;
  bool supports_cancel        = 7;
  bool requires_credential    = 8;
  int64 max_payload_bytes     = 9;
}
```

This is a *descriptor*, not a universal `Invoke(task_name, json)` surface. Each task
still owns its own typed request/response proto (plan lines 52-53, 56-57). The
Phase 0 self-review explicitly confirms this distinction (§7.1).

---

## 2. Legacy-compatible semantic RPCs (Deliverable 2)

Current proto surface (`rust/puerh_mind/proto/`): exactly two project-owned protos,
`common.proto` (package `common`, service `HealthService`) and `semantic.proto`
(package `semantic`, services `SemanticService` and `ModelManagerService`). No
`option`, no `import`, no enums, no nested messages.

### 2.1 Compatibility table

| Proto | Service | RPC | Request → Response | Phase 0 status |
|-------|---------|-----|--------------------|----------------|
| `common`  | `HealthService`    | `Ping`        | `PingRequest → PingResponse`                 | **Frozen legacy.** Infrastructure health; not on the AI control plane. |
| `common`  | `HealthService`    | `GetVersion`  | `GetVersionRequest → GetVersionResponse`     | **Frozen legacy.** |
| `semantic`| `SemanticService`  | `Ping`        | `PingRequest → PingResponse`                 | **Frozen legacy.** Used by readiness polling. |
| `semantic`| `SemanticService`  | `GetModelInfo`| `GetModelInfoRequest → GetModelInfoResponse` | **Frozen legacy.** Used by readiness + `require_model_info=false` mode. |
| `semantic`| `SemanticService`  | `GetRuntimeStatus` | `…Request → …Response`                | **Frozen legacy.** |
| `semantic`| `SemanticService`  | `EmbedText`        | `EmbedTextRequest → EmbeddingResponse`     | **Frozen v1.** v2 added in Phase 4; v1 untouched until v2 stable. |
| `semantic`| `SemanticService`  | `EmbedImage`       | `EmbedImageRequest → EmbeddingResponse`    | **Frozen v1.** v2 in Phase 4. |
| `semantic`| `SemanticService`  | `EmbedTextBatch`   | `EmbedTextBatchRequest → EmbeddingBatchResponse` | **Frozen v1.** v2 in Phase 4. |
| `semantic`| `SemanticService`  | `EmbedImageBatch`  | `EmbedImageBatchRequest → EmbeddingBatchResponse` | **Frozen v1.** v2 in Phase 4. |
| `semantic`| `ModelManagerService` | `ListModelProfiles`  | `…Request → …Response` | **Frozen legacy.** C++ owns download (`ModelDownloadService`); Rust only validates. |
| `semantic`| `ModelManagerService` | `ListInstalledModels`| `…Request → …Response` | **Frozen legacy.** |
| `semantic`| `ModelManagerService` | `ValidateModel`      | `ValidateModelRequest → ModelManagerResponse` | **Frozen legacy.** |
| `semantic`| `ModelManagerService` | `DeleteModel`        | `DeleteModelRequest → ModelManagerResponse`   | **Frozen legacy.** |

### 2.2 Compatibility path for each existing user flow

- **Readiness polling** (`SemanticRuntimeService::WaitForReadiness`,
  `semantic_runtime_service.cpp:1045-1088`): uses `Ping` then `GetModelInfo` on the
  `SemanticService` stub. Both are frozen. The `require_model_info=false` model-manager
  mode (`1062-1065`) keeps working unchanged.
- **Album semantic generation** (`semantic_generation_controller.cpp:391,774` →
  `StartAndWait` → `SemanticRuntimeImageEmbeddingClient` → `EmbedImage`/`EmbedImageBatch`):
  v1 RPCs frozen; Phase 4 migrates this path to v2 *call-site by call-site* with a
  legacy fallback until the new fake-runtime and real-runtime tests are stable.
- **Semantic search** (`ProjectSemanticSearchProvider`, `project_service.cpp:183-293`
  → `EmbedText` → `SemanticStorageController::SearchImageEmbeddings`): v1 frozen.
  Search-document construction and HNSW ranking are untouched in Phase 1.
- **Model download / install / settings** (`ModelDownloadService` + aria2 +
  `ModelDownloadController` + `model_asset_catalog`): entirely C++-owned, talks to
  the Rust sidecar only through `ModelManagerService` validation RPCs. Frozen.
- **Project load / lifetime** (`project_service.cpp` lazy `GetSemanticRuntimeService`
  `637-643`, `MakeSemanticRuntimeService` `344-358`, destructor `479-495`): unchanged.
  `StopForProjectClose` and the caller-thread affinity test
  (`ProjectServiceCreatesRuntimeOnCallerThreadAfterBackgroundLoad`) must keep passing.

### 2.3 What "frozen" forbids

- No renumbering, renaming, or removal of any existing field, RPC, or message in
  `common.proto` or `semantic.proto`.
- No new field added *to* a legacy message that changes wire behavior observed by the
  current C++ client (`GrpcSemanticRuntimeClient` hand-maps every field; surprises
  break the anonymous-namespace converters at `semantic_runtime_service.cpp:162-279`).
- v2 embedding RPCs are added as **new methods** (e.g. `EmbedTextV2`), not by editing
  v1. The C++ client migrates one call site at a time; v1 stays the fallback.

---

## 3. Generated protobuf targets after Phase 1 (Deliverable 3)

### 3.1 Proto files that must exist after Phase 1

| File | Package | Service | Status |
|------|---------|---------|--------|
| `proto/common.proto`     | `common`   | `HealthService`        | existing, unchanged |
| `proto/semantic.proto`   | `semantic` | `SemanticService`, `ModelManagerService` | existing, unchanged |
| `proto/ai_common.proto`  | `alcedo.ai`| none (messages + enums) | **new in Phase 1** |
| `proto/ai_runtime.proto` | `alcedo.ai`| `AiRuntimeService`      | **new in Phase 1** |

`ai_runtime.proto` (Phase 1 shape — credential/cancel RPCs are staged as comments so
the file boundary is stable, but only `ListCapabilities` is implemented in Phase 1):

```proto
syntax = "proto3";
package alcedo.ai;
import "ai_common.proto";

service AiRuntimeService {
  rpc ListCapabilities(ListCapabilitiesRequest) returns (ListCapabilitiesResponse);
  // Phase 3 (staged, not implemented in Phase 1):
  // rpc RegisterCredential(RegisterCredentialRequest) returns (RegisterCredentialResponse);
  // rpc CancelTask(CancelTaskRequest) returns (CancelTaskResponse);
}

message ListCapabilitiesRequest  { AiRequestHeader header = 1; }
message ListCapabilitiesResponse {
  AiResponseHeader header = 1;
  repeated AiCapability capabilities = 2;
}
```

`ListCapabilities` carries a header for uniform request correlation, but
`task_id`/`credential_ref`/`timeout_ms` are documented as ignored for this RPC.

### 3.2 Rust generated targets (`rust/puerh_mind`)

- `build.rs` (`build.rs:11`) `compile_protos` list becomes:
  `&["proto/common.proto", "proto/semantic.proto", "proto/ai_common.proto", "proto/ai_runtime.proto"]`.
- `build.rs` adds `cargo:rerun-if-changed=proto/ai_common.proto` and
  `cargo:rerun-if-changed=proto/ai_runtime.proto`.
- The file-descriptor-set path (`build.rs:7`, `semantic_descriptor.bin`) already
  captures *all* compiled protos in one set; it now also covers the two `ai` files.
  The embedded symbol name (`semantic_descriptor`, `src/service/registry.rs:14`) is
  left as-is in Phase 1 to minimize churn; it is misnamed but correct. Renaming to
  `alcedo_mind_descriptor` is optional cleanup, not required.
- `src/proto.rs` gains:
  ```rust
  pub mod alcedo {
      pub mod ai {
          tonic::include_proto!("alcedo.ai");  // brings in ai_common + ai_runtime types
      }
  }
  ```
  Generated server trait reachable as
  `crate::proto::alcedo::ai::ai_runtime_service_server::AiRuntimeService`.
- Reflection (`registry.rs:29-32`) registers the same (now superset) descriptor set;
  v1alpha reflection API unchanged.

### 3.3 C++ generated targets (`alcedo_studio/src`)

Current generation is strictly single-proto: `alcedo_studio/src/CMakeLists.txt:936-964`
hardcodes `semantic.proto` and enumerates exactly four outputs into
`${CMAKE_CURRENT_BINARY_DIR}/generated/semantic`, linked as `SemanticProto`.

After Phase 1:

- **Keep `SemanticProto` frozen** — still generates only `semantic.pb.{h,cc}` and
  `semantic.grpc.pb.{h,cc}` into `generated/semantic`. Existing consumers
  (`SemanticRuntimeService`) are not touched.
- **Add a sibling `AiProto` library** generating the two `ai` protos into
  `generated/ai`:
  - `ai_common.pb.h` / `ai_common.pb.cc` — messages + enums only (no service → no
    `grpc.pb`).
  - `ai_runtime.pb.h` / `ai_runtime.pb.cc` — messages.
  - `ai_runtime.grpc.pb.h` / `ai_runtime.grpc.pb.cc` — `AiRuntimeService` stub.
- **Shared `--proto_path`**: both new protos live in `${ALCEDO_SEMANTIC_PROTO_DIR}`
  (`rust/puerh_mind/proto`), already on the proto path. `ai_runtime.proto`'s
  `import "ai_common.proto";` resolves from that path at generation time; the custom
  command for `ai_runtime.proto` must `DEPENDS` the `ai_common.proto` *source* file
  (not a generated artifact).
- `AiProto`: `target_include_directories(... PUBLIC ${gen_dir}/ai)`,
  `target_link_libraries(... PUBLIC puerhlab::grpc++ puerhlab::protobuf)`.
- `protoc`/`grpc_cpp_plugin` executables are already exported as
  `PUERHLAB_PROTOC_EXECUTABLE` / `PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE`
  (`src/third_party/CMakeLists.txt:526-529`); reuse, do not re-resolve.
- Phase 1 only needs C++ to *compile* the generated headers/stubs; no C++ service
  implementation is required yet (Phase 2 wires the runtime). The build target is
  "configure + build `AiProto` cleanly on MSVC."

`common.proto` remains **Rust-only** in Phase 1 — the C++ client reaches health via
`semantic::SemanticService::Ping`, never `common::HealthService`.

---

## 4. Minimum fake-runtime behavior for C++ tests (Deliverable 4)

Today the fake runtime is split: an OS-process binary
(`alcedo_studio/tests/app/semantic_fake_runtime_main.cpp`, target `SemanticFakeRuntime`,
`tests/CMakeLists.txt:656-661`) that simulates *only* process lifecycle, and an
in-process `FakeSemanticRuntimeClient` (`semantic_runtime_service_test.cpp:24-216`)
that returns *canned* gRPC responses. The binary does not serve gRPC; the live gRPC
path is exercised only by `SemanticRuntimeServiceLiveTest` against an external Rust
runtime.

Phase 0 freezes the **minimum fake-runtime contract** the test suite must satisfy for
Phases 1-6. Items marked **(existing)** are already provided; **(Phase N)** are added
in that phase.

### 4.1 Process lifecycle (binary) — existing, must stay

- `--record-args <path>`: writes every arg newline-separated (arg-carriage tests).
- `--exit-now [--exit-code N]`: exits immediately (crash-before-ready).
- `--exit-after-ms N [--exit-code N]`: runs, then self-exits, printing
  `semantic fake runtime self-exit` to stderr (post-ready crash).
- `--ignore-terminate`: `SIG_IGN` SIGTERM/SIGINT (hung process → must be killed).
- `--sleep-ms N`: stays alive N ms then exits 0 (default 30000).
- On start: prints `semantic fake runtime started` to stdout.
- Signal handler otherwise sets a clean-shutdown flag for SIGTERM/SIGINT.

### 4.2 Readiness — existing, must stay

- `FakeSemanticRuntimeClient::SetReady` / `SetModelInfoReady` / `PingCount` control
  the readiness ladder the service polls in `WaitForReadiness`.
- The `require_model_info=false` model-manager path must remain testable: Ping
  succeeds, `GetModelInfo` fails, service still reaches `kReady`
  (`semantic_runtime_service.cpp:1062-1065`).

### 4.3 Semantic responses — existing, must stay

- Canned `GetModelInfoResponse` (`profile_id="mobileclip2-s2-en"`,
  `model_id="test/mobileclip"`, dim 512, image_size 256, transform `l2_normalize`).
- Two canned profiles (`mobileclip2-s2-en`, `jina-clip-v2-int8-multilingual`).
- Constant embeddings for text and image, batch and single, preserving request order.
- **No download API** — model download was moved off the gRPC runtime into the C++
  aria2 layer (`semantic_runtime_service_test.cpp:140-142`); the fake must not
  re-introduce one.

### 4.4 Capability responses — added in Phase 2

The in-process fake client must gain a canned `ListCapabilities` response (the
`AiRuntimeService` RPC from §3.1). Minimum set:

- One local semantic-embedding capability: `task_id="semantic.embed_*"`,
  `provider_id="local"`, `model_id` from the canned model info, `requires_credential=false`,
  `supports_cancel=true`.
- (Phase 5) one image-understanding capability: `task_id="image_understanding.describe"`,
  `provider_id="mock"`, `requires_credential=false` (mock) / `true` (remote behind a
  handle), `supports_batch=false`.

### 4.5 Credential behavior — added in Phase 3

- The fake must serve local tasks with an **empty `credential_ref`** and never require
  key material.
- For remote-provider tests, the fake must accept an opaque `credential_ref` handle
  and must **not** echo the handle or any key material in args, stdout, stderr, or
  error messages (this is what the Phase 3 redaction test greps for).

### 4.6 Cancellation behavior — added in Phase 3/4

- A delayed fake operation that completes only on cancel: the fake accepts a request
  with a known `request_id`, holds it without responding, and returns
  `AI_STATUS_CANCELLED` when `CancelTask` arrives with that `request_id`. This is the
  minimum needed for the Phase 3 cancellation test and the Phase 4 timeout test.

---

## 5. AI annotation storage (Deliverable 5)

### 5.1 Current storage state (verified)

Schema is inline `constexpr` strings in
`alcedo_studio/src/include/storage/controller/db_controller.hpp`, run by
`DBController::InitializeDB()` (`db_controller.cpp:74-126`). There are **no `.sql`
files** in the repo. Relevant existing tables:

- `SemanticModel` (`db_controller.hpp:52-64`): `model_key` PK (= `model_id@revision`),
  `model_id`, `revision`, `embedding_dim` (512 or 768 only), `image_size`,
  `engine_id`, `profile_id`, `supported_text_languages_json`, `prompt_config_hash`,
  `asset_manifest_json`, **`active BOOLEAN`** (model-level, single-active,
  `db_controller.cpp:1003-1013,1170-1182`), `created_at`.
- `SemanticImageEmbedding` / `SemanticImageEmbedding768` (`65-90`): dimension-sharded
  (FLOAT[512] / FLOAT[768]); `file_id`, `image_id`, `model_key`, `embedding`,
  `embedding_dim`, `thumbnail_resolution`, `generated_at`, `status`, `error`;
  PK `(file_id, model_key)`; HNSW index created lazily by
  `SemanticStorageController::EnsureVectorSearchIndex` (`.cpp:1902-1928`).
- `SemanticImageLabel` (`91-104`): CLIP label assignment, one row per
  `(file_id, model_key)` — `label`, `score`, `second_label`, `second_score`, `margin`,
  `confident`, `top_scores JSON`. **Not** an LLM caption/tag store.
- `SemanticLabelPrototype` / `…768`, `SemanticLabelQuery`.

**Gaps confirmed by grep:** no `provider_id` column anywhere; no caption / free-form
tag / AI-score / per-annotation-active columns exist. The only `active` flag is
model-level on `SemanticModel`. Every `caption|annotation|provider_id` hit is UI
(`CaptionButton`, font roles) or unrelated.

Search has **no DuckDB FTS index**. `SleeveFilterService::SearchDocumentExpr`
(`alcedo_studio/src/app/sleeve_filter_service.cpp:173-187`) is a `CONCAT_WS(' ', …)`
over `element_name`, `file_name`, `image_path`, selected EXIF JSON fields, the
semantic-label subquery `SemanticLabelExpr` (`.cpp:149-171`), and a whole-metadata
fallback. Matching is `LIKE … ESCAPE '~'` / `contains()` assembled in
`BuildFuzzySearchWhere` (`.cpp:505-532`).

### 5.2 New table: `AiImageAnnotation`

Single table (no vector → no dimension sharding). Column names chosen to read
alongside the existing `SemanticImageEmbedding` columns.

```sql
CREATE TABLE IF NOT EXISTS AiImageAnnotation (
  file_id          BIGINT  NOT NULL,
  image_id         BIGINT  NOT NULL,
  task_id          VARCHAR NOT NULL,   -- e.g. "image_understanding.describe"
  provider_id      VARCHAR NOT NULL,   -- "mock" | "local" | "openai" | ...
  model_id         VARCHAR NOT NULL,
  model_revision   VARCHAR,            -- nullable; remote LLMs may not carry one
  prompt_profile_id VARCHAR,           -- nullable; the prompt/profile used
  caption          VARCHAR,
  tags             JSON,               -- array of strings, optionally with scores
  score            DOUBLE,
  active_for_search BOOLEAN NOT NULL DEFAULT FALSE,
  created_at       TIMESTAMP DEFAULT current_timestamp,
  PRIMARY KEY (file_id, task_id, provider_id, model_id)
);
CREATE INDEX IF NOT EXISTS idx_ai_annotation_file_active
  ON AiImageAnnotation (file_id, task_id, active_for_search);
```

Add this DDL as a new `constexpr` string in `db_controller.hpp` (beside
`semantic_table_query`) and execute it in `InitializeDB()` alongside the semantic
tables (run on every open, `CREATE … IF NOT EXISTS`, idempotent — same pattern as the
semantic tables). A migration block is **not** needed because the table is new; a
future `active_for_search` policy change would use the `ALTER TABLE … ADD COLUMN IF
NOT EXISTS` pattern already used by `semantic_migration_query`
(`db_controller.hpp:124-133`).

### 5.3 Identity columns

- `provider_id` — who served (matches `AiResponseHeader.provider`).
- `model_id` + `model_revision` — what served. Stored **separately** from
  `SemanticModel.model_key` (`model_id@revision`) because image-understanding models
  are remote LLMs, not local CLIP models registered in `SemanticModel`. Decoupling
  avoids forcing every remote model into the CLIP model registry. The `model_id`
  value matches `AiResponseHeader.model_id` and `AiCapability.model_id`.
- `prompt_profile_id` — the prompt/profile used (Open Decision in the plan; Phase 0
  freezes the column as nullable so Phase 5 can store it without a schema change).

### 5.4 Active-for-search policy (frozen here)

**Decision:** `active_for_search` is **per `(file_id, task_id)`**, with at most one
active row per `(file_id, task_id)` enforced by the writer — before inserting a new
active annotation, set `active_for_search = FALSE` for all existing rows with the same
`(file_id, task_id)`, then insert with `active_for_search = TRUE`. This mirrors the
existing single-active pattern on `SemanticModel` (`db_controller.cpp:1003-1013`).

This resolves the plan's Open Decision ("Whether captions/tags are per
provider/profile, per active semantic model, or globally active") in favor of:
**multiple providers/models may coexist as history; exactly one per `(file, task)` is
active for search.** Rationale:

- Lets a user re-run captioning with a different provider/model without losing the
  prior result (audit/compare), while keeping search deterministic.
- Failed remote calls produce no row (or a row left `active_for_search = FALSE`),
  so they cannot create a partial active search document — directly satisfying the
  Phase 5 self-review ("failed remote calls do not create partial active search
  documents").

### 5.5 Search integration

Add an `AiAnnotationExpr` subquery to `SearchDocumentExpr`, exactly analogous to the
existing `SemanticLabelExpr` (`sleeve_filter_service.cpp:149-171`):

```sql
(SELECT string_agg(coalesce(caption,'') || ' ' || coalesce(
     (SELECT string_agg(value::VARCHAR, ' ') FROM unnest(tags::VARCHAR[])), ''), ' ')
 FROM AiImageAnnotation
 WHERE file_id = e.id
   AND active_for_search = TRUE
   AND task_id IN (<active task set>))
```

- The `<active task set>` comes from a small host-side config (which `task_id`s
  participate in full-text search). Initially `{"image_understanding.describe"}`.
- The subquery is emitted only when that set is non-empty (mirrors how
  `SemanticLabelExpr` is gated on a non-empty `active_model_key`,
  `sleeve_filter_service.cpp:470,517-519`).
- **No DuckDB FTS index is introduced.** The existing `LIKE`/`contains` document
  approach is reused for consistency; captions/tags are short strings, so the
  `CONCAT_WS` document stays effective. (A future FTS index is a separate, later
  optimization and is out of scope for this plan.)
- A successful annotation write must refresh the search index path for the affected
  file(s); because matching is expression-based (not a materialized FTS index), the
  "refresh" is simply committing the row with `active_for_search = TRUE` before the
  next search query. Phase 6 wires the post-persistence refresh call.

### 5.6 Connection ownership and cleanup wiring

- **Connection ownership:** the new `AiAnnotationStorageController` follows the
  `SemanticStorageController` pattern — store a `DBController&`, open a transient
  `ConnectionGuard` per operation (`db_ctrl_.GetConnectionGuard()` then
  `guard.Lock()`), and hold **no** long-lived connection. This is the DuckDB ownership
  rule in `CLAUDE.md` and matches `SemanticStorageController` exactly
  (`.cpp:980-981`). It must **not** follow the `ElementController`/`ImageController`
  long-lived-guard pattern.
- **Delete cleanup:** `ElementController::DeleteSemanticRowsForFiles`
  (`element_controller.cpp:119-138,270-271,312`) currently deletes
  `SemanticImageEmbedding` / `SemanticImageEmbedding768` / `SemanticImageLabel` rows
  for the deleted file ids. It must be extended to also
  `DELETE FROM AiImageAnnotation WHERE file_id IN (…)`. This is a required wiring
  point in Phase 5; without it, deleting a file leaves orphan annotations.
- **No sidecar DB writes:** the sidecar never writes to DuckDB. Only the C++ host
  (the `ImageUnderstandingService` in Phase 5) writes `AiImageAnnotation` rows, after
  validating the structured response. This satisfies the Phase 5 self-review
  ("sidecar does not write directly to the database").

---

## 6. Known test state (carried into Phase 1, not a Phase 0 blocker)

Phase 0 produces no code and no tests. For Phase 1's `cargo test` gate, four Rust
tests are currently expected to fail on this branch because the macOS model list was
changed and the C++/Rust catalog drift has not been reconciled:

- `server::model_manager::tests::lists_fixed_model_profiles_with_dimension_policy`
- `server::model_manager::tests::validate_missing_model_returns_structured_error`
- `service::model_assets::tests::validate_model_profile_missing_root_reports_path`
- `service::model_assets::tests::validate_only_missing_model_fails_without_creating_root`

These are pre-existing failures unrelated to the AI sidecar work and are **accepted as
ignored** for Phase 1, per the task brief. Phase 1's `cargo test` gate is "all green
except these four known failures." Reconciling the model list is its own work item,
not part of this plan.

---

## 7. Phase 0 self-review

### 7.1 Control plane vs. universal provider abstraction

Confirmed: Phase 0 introduces **only** the control surface — headers, status enums,
and a capability *descriptor*. There is no `Invoke(task_name, json_payload)` universal
RPC, no generic provider trait, and no credential vault in the Phase 1 scope. Provider
abstractions are task-scoped and deferred to Phase 5 (image-understanding provider
traits) and Phase 3 (credential vault), each behind its own typed contract. The
`AiCapability` message describes capabilities; it does not dispatch them.

### 7.2 Compatibility paths

Confirmed (§2.2): every existing semantic user flow — readiness polling, album
semantic generation, semantic search, model download/install/settings, project load
and lifetime — has a frozen v1 path that Phase 1 does not touch. v2 embedding RPCs
are added alongside, not in place of, v1.

### 7.3 Review conclusion

```
Review conclusion: none; accepted decisions (package=alcedo.ai single-package;
timeout_ms relative not deadline_ms absolute; active_for_search per (file,task);
model identity decoupled from SemanticModel.model_key; no WKT imports); none.
```

No high-priority correctness, credential-handling, persistence, or compatibility
issue is left unresolved. Phase 0 is documentation only — no code, no tests, no files
moved.

---

## 8. Phase 1 acceptance checklist (gate before any Phase 1 code)

Phase 1 code may start only when **all** of the following are explicit and agreed
(this section is that explicit list):

1. `proto/ai_common.proto` and `proto/ai_runtime.proto` are authored exactly as
   specified in §1 and §3.1 (package `alcedo.ai`; no WKT imports; `ai_runtime`
   implements only `ListCapabilities`, with credential/cancel RPCs staged as
   comments).
2. `rust/puerh_mind/build.rs` compiles all four protos and emits `rerun-if-changed`
   for both new files; `src/proto.rs` exposes `crate::proto::alcedo::ai`.
3. A Rust `AiRuntimeService` impl is registered in `service/registry.rs` (beside the
   existing `HealthService` / `ModelManagerService` / `SemanticService`
   registrations) without altering them.
4. CMake gains an `AiProto` library (§3.3) generating `ai_common.*` and
   `ai_runtime.*` into `generated/ai`; `SemanticProto` is left frozen; MSVC configure
   + build of `AiProto` succeeds.
5. Existing `HealthService`, `ModelManagerService`, `SemanticService` behavior is
   byte-for-byte unchanged (diff the three `server/*.rs` files — only additive
   registration, no method edits).
6. `cargo test` in `rust/puerh_mind` is green except the four known failures listed
   in §6.
7. `SemanticRuntimeServiceTest` (fake runtime) still passes unchanged — Phase 1 adds
   no new fake-runtime behavior (that is Phase 2).
8. The Phase 1 self-review conclusion is written before advancing to Phase 2, in the
   shape required by the plan (line 115).
