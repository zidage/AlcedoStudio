# AI Sidecar Backend Integration Plan

Date: 2026-06-25

Primary roadmap owner: `alcedo_studio/src/ai` (cross-module sidecar/app integration)

Status: Planning. SAM and smart mask work are explicitly deferred until the editor pipeline
has first-class mask capability.

## Background

`rust/puerh_mind` is currently a local semantic sidecar. Its gRPC surface is centered on
health checks, model management, CLIP image embeddings, and semantic search generation. That
shape is correct for the first semantic-search milestone, but the next AI features will not all
look like CLIP inference.

Near-term candidates include:

- Image understanding through a multimodal LLM API, producing a score, tags, and a one-line
  caption that can be written into the database search document.
- Future local or remote AI tasks that use very different request payloads, provider settings,
  and timeout behavior.
- A later edit-assistant workflow that proposes non-destructive adjustment recipes for the C++
  editor pipeline.

The exchange format should therefore be unified only at the control plane. Each task still owns
its task-specific protobuf payload, because embeddings, captioning, scoring, and editor recipes
do not share a meaningful request schema. What should be common is the envelope around those
payloads: request identity, task name, timeout, cancellation, priority, trace metadata, capability
description, and credential handles.

## Current C++ Integration Points

- `ProjectService` lazily owns `AiSidecarRuntimeService`; this is the C++ entry point that starts
  the sidecar process and hands runtime access to album/semantic flows.
- `AiSidecarRuntimeService` currently owns `QProcess`, readiness polling, command-line arguments,
  runtime status, the gRPC client interface, and most proto-to-DTO mapping. This is now too broad:
  lifecycle and protocol work must be split before persistence/search wiring adds more call sites.
- `GrpcAiSidecarRuntimeClient` currently creates semantic, model-manager, AI-runtime, and
  image-analysis stubs directly. It also hand-maps protobuf messages for capabilities,
  credentials, model profiles, embeddings, image understanding, rating, and model discovery in one
  file. That makes every new RPC grow the same monolithic client.
- Model download and model-profile settings are currently C++-owned through
  `ModelDownloadService` and `ModelDownloadController`, using the local aria2-based download
  path. The AI sidecar plan should not assume Rust owns semantic asset acquisition today.
- Search indexing currently flows through Sleeve storage and `SleeveFilterService` search
  document construction. Captions and AI tags need explicit storage fields or tables before they
  can enter full-text search.
- C++ CMake generation currently targets `rust/puerh_mind/proto/semantic.proto`. A general AI
  sidecar will need multi-proto generation on both Rust and C++ sides.
- The editor pipeline does not yet have a product-level persistent mask/local-adjustment model.
  SAM integration is therefore not part of this plan.

## Design Principles

- Keep execution APIs task-specific. Do not introduce a universal
  `Invoke(task_name, json_payload)` interface.
- Add a small shared protobuf control surface for common fields, then let each task define its
  own typed payload and response.
- Move direct sidecar protocol code into a real `sidecar_client` module under
  `alcedo_studio/src/sidecar_client` and `alcedo_studio/src/include/sidecar_client`. The runtime
  service owns the process; the client owns gRPC, request envelopes, stubs, and protobuf mapping.
- Make DTO/protobuf conversion explicit on the DTO types through a shared CRTP-style mapper helper.
  Call sites should read as `Dto::FromProto(proto)` or `dto.ToProto(&proto)`, not as anonymous
  helper functions hidden in `ai_sidecar_runtime_service.cpp`.
- Keep host ownership clear. C++ owns project state, persistence, UI policy, model download
  settings, and database writes. The sidecar computes results and reports structured outcomes.
- Treat credentials as short-lived capabilities. Do not pass long-lived API keys through command
  line arguments, persistent logs, or sidecar startup environment by default.
- Cut over cleanly when refactoring C++ protocol ownership. Do not keep `AiSidecarRuntimeService`
  as a method-by-method forwarding facade for every sidecar API, and do not add fallback paths that
  silently try an older provider, protocol, model, schema mode, or C++ RPC implementation.
- Keep the binary name `alcedo_mind.exe` initially. Renaming the sidecar can be a later packaging
  cleanup once the API shape is stable.

## Remote LLM Technical Stack And Security Decisions

The first remote provider path should be implemented in Rust inside `alcedo_mind`, not in QML or
direct UI code. The C++ host remains responsible for settings, project state, persistence, and
database writes; Rust owns only the outbound provider call and runtime-only secret handling.

Recommended Rust stack:

- Use `reqwest` on the existing Tokio runtime for HTTPS JSON calls. Keep the provider layer thin
  and task-specific instead of adopting a broad multi-provider LLM SDK in the first iteration.
- Pin `reqwest` with `default-features = false` and an explicit Rustls TLS feature when added to
  `Cargo.toml`. Do not enable invalid certificate acceptance or plaintext HTTP endpoints for
  production providers.
- Build a small `RemoteVisionProvider` / `OpenAiVisionProvider` adapter around the provider REST
  API. It should map provider errors, rate limits, request ids, and usage/cost metadata into the
  typed AI response, not leak provider JSON directly through C++.
- Use provider structured-output support for caption/tag/rating responses when available. The Rust
  service must still validate and normalize the response before returning protobuf fields.
- Do not stream in the MVP. Use non-streaming calls for deterministic timeout, cancellation, and
  schema-validation behavior. Streaming can be added later for assistant-style workflows.
- Keep retries conservative. Retrying a provider call can duplicate cost, so only retry transient
  transport / 429 / 5xx failures under a small, bounded policy and surface the provider request id
  when available.
- Log correlation ids, task ids, provider id, model id, latency, status, and provider request id.
  Never log prompt payloads, image bytes/base64, API keys, credential handles, or raw provider error
  bodies before redaction.

## Provider Driver And Config Strategy

Large-model providers do not share a stable request JSON shape. A pure "JSON template per provider"
system would look flexible, but it would push protocol semantics, credential handling, error mapping,
retry policy, and response validation into data files where they are hard to test. The safer pattern
used by LLM gateway projects is:

1. Keep Alcedo task schemas stable and code-owned.
2. Implement a small set of provider drivers for real protocol families.
3. Let provider config files describe endpoints, model defaults, capability flags, schema-injection
   mode, auth slot, limits, and response extraction.

Provider config files are therefore deployment/configuration data, not executable adapters. They
should not contain JavaScript, shell commands, arbitrary eval expressions, or raw API keys.

Phase 6 correction (2026-06-26): the product-facing provider setup should be
protocol-family first, not brand first. The user's paid subscription is Opencode,
not OpenRouter. Opencode exposes API-key-authenticated compatible endpoints:
OpenAI-style `/chat/completions` and Anthropic-style `/messages` (for example
under `https://opencode.ai/zen/go/v1` for Opencode Go). That means Phase 6 should
let users choose or create protocol presets containing base URL, endpoint, model
id, auth slot, and structured-output mode. OpenRouter and Volcengine remain useful
built-in sample presets / live-smoke fixtures, but they are not the abstraction the
UI should organize around.

Initial driver families:

- `openai_chat_compatible`: generic OpenAI-compatible chat-completions servers.
- `anthropic_messages`: Anthropic Messages API.
- `openai_responses`: OpenAI-compatible Responses API, added only when a target endpoint needs the
  Responses shape instead of Chat Completions.
- `openrouter_chat`: OpenRouter's OpenAI Chat Completions-compatible endpoint, with OpenRouter
  routing preferences and metadata handling. Treat this as a specialized preset/driver, not the
  Phase 6 product default.
- `volcengine_ark_responses`: Volcengine Ark / 火山方舟 Responses API, used by the built-in
  Doubao multimodal provider config.
- `volcengine_ark_chat`: Volcengine Ark / 火山方舟 Chat API. Phase 6 should remove this from
  product-facing paths unless live testing proves a concrete endpoint cannot be represented by
  `openai_chat_compatible`.
- `gemini_generate_content`: Google Gemini `generateContent`.
- `generic_json_http`: reserved historical idea only. Do not ship it as a Phase 6 product fallback;
  compatible-protocol presets must use code-owned OpenAI-compatible or Anthropic-compatible drivers.

Provider config file shape:

```json
{
  "schema_version": 1,
  "provider_id": "openrouter",
  "display_name": "OpenRouter",
  "driver": "openrouter_chat",
  "base_url": "https://openrouter.ai/api/v1",
  "endpoint": "/chat/completions",
  "auth": {
    "type": "bearer",
    "credential_slot": "openrouter_api_key"
  },
  "attribution_headers": {
    "HTTP-Referer": "https://alcedo.studio",
    "X-OpenRouter-Title": "Alcedo Studio"
  },
  "defaults": {
    "model": "qwen/qwen3.7-plus",
    "stream": false,
    "temperature": 0.2
  },
  "structured_output": {
    "mode": "response_format_json_schema",
    "strict": true,
    "provider_require_parameters": true
  },
  "response": {
    "content_json_pointer": "/choices/0/message/content",
    "usage_json_pointer": "/usage",
    "provider_request_id_json_pointer": "/id",
    "provider_request_id_header": null
  },
  "limits": {
    "timeout_ms": 60000,
    "max_image_bytes": 4194304,
    "max_output_tokens": 1200
  }
}
```

Phase 5 should use JSON config first because `rust/puerh_mind` already depends on `serde_json`.
Built-in provider configs can be embedded into the sidecar binary with `include_str!` to avoid
packaging drift. User-added provider configs should live in a user config directory and be loaded
after built-ins, with validation errors surfaced in settings. User configs may override model lists
and endpoints, but they may not override secret storage policy or bypass schema validation.

The Alcedo task schema remains code-owned:

- `image_understanding.describe` always returns Alcedo's caption/tags/scene/confidence shape.
- `image_rating.score` always returns Alcedo's rating/rubric shape.
- Provider configs only describe how that schema is requested from a provider and where to extract
  the provider's response.

Second built-in provider config example:

```json
{
  "schema_version": 1,
  "provider_id": "volcengine_ark",
  "display_name": "Volcengine Ark / 火山方舟",
  "driver": "volcengine_ark_responses",
  "base_url": "https://ark.cn-beijing.volces.com/api/v3",
  "endpoint": "/responses",
  "auth": {
    "type": "bearer",
    "credential_slot": "volcengine_ark_api_key"
  },
  "defaults": {
    "model": "doubao-seed-2-0-lite-260428",
    "stream": false,
    "temperature": 0.2
  },
  "structured_output": {
    "mode": "responses_json_schema",
    "strict": true
  },
  "response": {
    "content_json_pointer": null,
    "usage_json_pointer": "/usage",
    "provider_request_id_json_pointer": "/id",
    "provider_request_id_header": null
  },
  "limits": {
    "timeout_ms": 60000,
    "max_image_bytes": 4194304,
    "max_output_tokens": 1200
  }
}
```

## OpenRouter Implementation Strategy

Historical Phase 5 note: OpenRouter was the first implemented OpenAI-compatible
test path, but it is not the user's current subscription. Phase 6 should not
require or prioritize an OpenRouter credential. Reuse the request/response work
by generalizing it into `openai_chat_compatible`, then add Opencode-compatible
presets on top of that generic driver.

OpenRouter was chosen for Phase 5 exploration because it gives one OpenAI-compatible endpoint for
many model vendors while still supporting structured outputs for compatible models. The bundled
OpenRouter preset default is Qwen3.7 Plus: show it to users as `qwen3.7-plus`, but send the
canonical OpenRouter model slug `qwen/qwen3.7-plus` on the wire.

OpenRouter driver behavior:

- Use `POST https://openrouter.ai/api/v1/chat/completions`.
- Authenticate with `Authorization: Bearer <runtime secret>`, where the secret comes from the
  runtime credential vault by `credential_ref`; never from config files.
- Send `Content-Type: application/json`.
- Optionally send `HTTP-Referer` and `X-OpenRouter-Title` attribution headers from config.
- Build OpenAI Chat-compatible `messages` with an Alcedo-owned system prompt and user content that
  contains the selected image rendition plus task instructions.
- Keep the request body compatible with OpenRouter's official Go SDK chat-completion shape. Rust
  still sends direct HTTPS in Phase 5, but the request/response fixtures should be reusable by an
  OpenRouter Go client without changing model slug, structured-output fields, provider routing, or
  attribution-header semantics.
- Inject Alcedo's JSON Schema through `response_format: { "type": "json_schema", ... }` and set
  strict mode when the selected model supports it.
- Include `provider: { "require_parameters": true }` when structured output is required, so
  OpenRouter does not route to a provider that silently ignores `response_format`.
- Keep `stream: false` in Phase 5.
- Parse `choices[0].message.content` as JSON, validate it against the Alcedo task schema, normalize
  strings/tags/scores, and return typed protobuf fields.
- Capture `usage` and provider request id when available; expose them as metadata for UI/cost
  summaries and diagnostics.
- If schema validation fails, return a typed provider/schema error and do not persist an active
  annotation. A future retry may use response healing, but Phase 5 should not silently repair and
  persist ambiguous results.

OpenRouter config should ship with a small curated model list instead of defaulting to arbitrary
router aliases. Phase 5 starts with `qwen/qwen3.7-plus` as the bundled default, because it is the
canonical OpenRouter slug for the user-facing `qwen3.7-plus` model. Each bundled model entry should
declare:

- model slug
- whether vision input is supported
- whether JSON Schema structured output is supported
- max image bytes / recommended image rendition
- approximate cost metadata if available
- whether to request data-collection restrictions such as `provider.data_collection = "deny"` when
  the user enables a privacy-first mode

## Volcengine Ark Implementation Strategy

Volcengine Ark / 火山方舟 should ship as the second built-in remote provider. The default model is
`doubao-seed-2-0-lite-260428`, matching the current multimodal/Responses API plan and keeping a
China-friendly provider available without requiring users to configure an arbitrary custom endpoint.

Volcengine driver behavior:

- Use the Ark data-plane base URL `https://ark.cn-beijing.volces.com/api/v3`.
- Prefer the Responses API driver (`volcengine_ark_responses`) for multimodal understanding because
  the linked 火山方舟 docs place multimodal understanding under Responses API. Keep
  `volcengine_ark_chat` available as a compatibility driver for deployments or models that are
  easier to call through Chat API.
- Authenticate with `Authorization: Bearer <runtime secret>`, where the secret comes from the
  runtime credential vault by `credential_ref`; never from config files.
- Send `Content-Type: application/json`.
- Build a Responses API request from Alcedo-owned task schema, selected image rendition, and prompt
  profile. The driver, not the provider config, owns the exact Responses API field mapping.
- Inject Alcedo's JSON Schema through the Ark structured-output mechanism when supported. If a
  selected model does not support structured output, fail closed for Phase 5 rather than relying on
  best-effort free-form JSON.
- Keep `stream: false` in Phase 5.
- Parse the provider response content as JSON, validate it against the Alcedo task schema, normalize
  values, and return typed protobuf fields.
- Capture response id, model id, usage metadata, and provider error codes when available.
- Map Ark/transport errors into `AiResponseStatus` / `AiErrorCode`, with redacted messages.

Bundled Volcengine config should declare `doubao-seed-2-0-lite-260428` as the default model and mark
it as supporting text generation, multimodal understanding, and structured output only after a live
provider smoke confirms the exact request shape. `content_json_pointer: null` means the
`volcengine_ark_responses` driver owns response-content extraction with a typed parser; it should
only become a static JSON Pointer if the live response shape is stable enough to make that safer than
driver-owned parsing.

Credential ownership is split deliberately:

- Long-lived user API keys are persisted by the C++/Qt host, not by Rust.
- The persisted store must be an OS credential store: Windows Credential Manager, macOS Keychain,
  and Linux Secret Service / KWallet where supported. `QSettings` may store only non-secret metadata
  such as provider id, selected model, masked key label, and "remember key" preference.
- A practical implementation path is to add a small host `AiCredentialStore` abstraction backed by
  QtKeychain or a minimal platform-native wrapper. QtKeychain is attractive because it already maps
  to Windows Credential Store, macOS Keychain, and Linux desktop keyrings and has vcpkg/Homebrew
  availability; using it should still be gated by a focused dependency/build review.
- When starting a remote task, C++ reads the secret from the OS credential store, calls
  `AiRuntimeService.RegisterCredential`, receives an opaque handle, and passes only that handle in
  task headers. Rust keeps the secret only in memory with TTL/revoke/redaction.
- On sidecar stop, project close, provider logout, or settings deletion, C++ must revoke runtime
  handles and delete persisted credentials when requested.
- Developer override via `OPENAI_API_KEY` or equivalent is allowed for tests/manual smoke, but it is
  not the normal product persistence path and must not be copied into QSettings.

## Rating vs Understanding Boundary

Rating and understanding should be separate task semantics even if a provider can answer both in one
HTTP call.

- `image_understanding.describe`: objective-ish image content. Outputs caption, searchable tags,
  scene/category hints, and optional confidence. These fields can participate in the search document
  when marked active.
- `image_rating.score`: subjective or product-policy scoring. Outputs one or more numeric scores
  such as keeper score, aesthetic score, technical quality, or curation priority, plus short reasons.
  Rating is not full-text search content by default; it should drive sort/filter/recommendation
  workflows only after the product contract is clear.
- Shared plumbing is fine: both tasks may use the same credential handle, HTTP client, provider
  adapter, image rendition selection, timeout, cancellation, and redaction path.
- Storage should keep `task_id` / `prompt_profile_id` / provider / model identity with each result
  so a future rating rubric change does not silently overwrite or reinterpret earlier understanding
  annotations.

## Shared Control Surface

Add protobuf messages similar to:

- `AiRequestHeader`
  - `request_id`
  - `task_id`
  - `deadline_ms` or `timeout_ms`
  - `priority`
  - `trace_id`
  - `credential_ref`
  - `client_capabilities`
- `AiResponseHeader`
  - `request_id`
  - `task_id`
  - `status`
  - `error_code`
  - `error_message`
  - `provider`
  - `model_id`
  - `elapsed_ms`
- `AiCapability`
  - `task_id`
  - `provider_id`
  - `model_id`
  - `input_kinds`
  - `output_kinds`
  - `supports_batch`
  - `supports_cancel`
  - `requires_credential`
  - `max_payload_bytes`

This layer should live beside, not inside, task-specific protobuf files. For example:

- `proto/ai_common.proto` for headers, status, capability descriptors, and credential refs.
- `proto/ai_runtime.proto` for sidecar-level capabilities, credential registration, and
  cancellation.
- `proto/semantic.proto` remains the typed semantic embedding service during migration.
- `proto/image_analysis.proto` or `proto/image_understanding.proto` adds typed image
  understanding and rating task messages; `task_id` distinguishes searchable understanding from
  subjective rating.
- Future task protobufs can be added without changing existing task contracts.

## Mandatory Phase Rule

Every phase must end with both:

- Focused tests for the changed Rust, C++, storage, UI, or packaging surface.
- A self code-review conclusion before moving to the next phase.

The self-review conclusion must be written in the phase handoff or PR notes using this shape:

`Review conclusion: <bugs found or none>; <risk accepted or none>; <missing tests or none>.`

Do not advance to the next phase if phase tests are failing or if the review conclusion contains
an unresolved high-priority correctness, credential-handling, persistence, or compatibility issue.

## Phase 0 - Contract Inventory And Gates

Goal: freeze the compatibility boundary before adding new services.

Deliverables:

- Write the exact shared header fields and status-code mapping.
- Record which existing semantic RPCs stay legacy-compatible during migration.
- List the generated C++ and Rust protobuf targets that must exist after Phase 1.
- Define the minimum fake-runtime behavior required for C++ tests.
- Define where AI annotations will be stored, including model/provider identity and whether the
  result is active for search.

Tests:

- Documentation review only unless files are moved.
- No code phase starts until the acceptance checklist is explicit.

Self-review focus:

- Check that this phase does not mix control-plane design with a universal provider abstraction.
- Check that all existing semantic user flows have a compatibility path.

## Phase 1 - Proto And Runtime Control Plane

Goal: add the general AI sidecar protobuf foundation without changing existing semantic behavior.

Deliverables:

- Add `ai_common.proto` and `ai_runtime.proto`.
- Update `rust/puerh_mind/build.rs` and `src/proto.rs` to compile and expose the new protos.
- Update C++ CMake protobuf generation so C++ stubs can be generated for multiple proto files.
- Add Rust service registration for a sidecar capability/runtime service.
- Keep existing `HealthService`, `ModelManagerService`, and `SemanticService` behavior intact.

Tests:

- `cargo test` in `rust/puerh_mind`.
- C++ configure/build enough to prove generated headers and stubs compile.
- Existing `SemanticRuntimeServiceTest` should still pass with the fake runtime.

Self-review focus:

- Check generated file paths and include names for Windows/MSVC stability.
- Check that old semantic clients and fake runtime fixtures were not broken.

## Phase 2 - C++ Runtime Neutralization

Goal: let the C++ host treat the process as an AI sidecar while preserving semantic entry points.

Deliverables:

- Introduce neutral runtime DTOs for sidecar endpoint, process state, capability status, and
  startup options.
- Either rename `SemanticRuntimeService` carefully or add a small `AiSidecarRuntimeService`
  wrapper around the existing process owner. Keep the public semantic facade stable until
  semantic migration is complete.
- Preserve existing process lifetime behavior, logs, timeouts, readiness polling, and
  `require_model_info=false` model-manager startup mode.
- Extend the fake runtime so tests can expose capability responses as well as semantic responses.

Tests:

- `AiSidecarRuntimeServiceTest` (renamed from `SemanticRuntimeServiceTest` in Phase 2).
- Fake runtime tests for startup args, readiness, crash handling, hung stop, and capability query.
- Manual smoke if the sidecar command-line contract changes.

Self-review focus:

- Check that `QProcess` is only touched on its owning thread.
- Check that model-manager-only startup still works without local model assets.
- Check that ordinary album/search startup does not launch extra AI work.

## Phase 3 - Capability Registry, Credential Handles, And Host Credential Store

Goal: support remote providers without making users re-enter keys every session and without leaking
long-lived secrets into sidecar process launch, QSettings, persistent logs, or crash messages.

Deliverables:

- Add a Rust in-memory credential vault with registration, TTL, revoke, and no-log redaction.
- Add C++ APIs for creating credential handles and passing only the handle in task headers.
- Add capability descriptors for local semantic embedding and remote image understanding.
- Add cancellation by `request_id` or task operation id.
- Add a C++ `AiCredentialStore` interface for long-lived user API keys. Back it with the platform
  secure credential store, preferably through QtKeychain or a narrow native wrapper after dependency
  review.
- Store only non-secret metadata in QSettings: provider id, selected model/profile, masked key label,
  and whether the user enabled persistence.
- Define the task flow from persisted key -> C++ loads secret -> `RegisterCredential` -> runtime
  handle -> task header -> Rust resolves handle for the provider call.
- Keep actual secure credential persistence out of Rust. Rust may keep secrets only in memory for the
  lifetime/TTL of the registered handle.

Tests:

- Rust tests for register, resolve, TTL expiry, revoke, and redaction.
- C++ fake-runtime tests proving API key material is not present in process args or routine logs.
- Cancellation tests with a delayed fake operation.
- C++ credential-store tests with a fake backend for save/read/delete, metadata-only QSettings, and
  "remember key" off/on behavior.
- Manual smoke on Windows Credential Manager and macOS Keychain before treating persisted keys as
  shippable.

Self-review focus:

- Check that keys never appear in command-line arguments, persistent logs, or crash messages.
- Check that credential handles cannot silently outlive their intended session.
- Check that QSettings and project files never contain raw key material.
- Check that deleting or replacing a key revokes the in-memory sidecar handle.

## Phase 4 - Semantic Embedding V2 Migration

Goal: move semantic embedding onto the shared AI control surface while keeping current search
generation stable.

Deliverables:

- Add request/response headers to the semantic embedding path, either in a v2 RPC or compatible
  wrapper messages.
- Update C++ semantic runtime client code to fill request ids, timeout, task id, and trace fields.
- Preserve existing batching, request-id to file-id mapping, model-info validation, and embedding
  persistence behavior.
- Keep a legacy fallback path until the new semantic fake runtime and real runtime tests are
  stable.

Tests:

- Rust semantic batch tests.
- `SemanticGenerationServiceTest`.
- `SemanticStorageControllerTest`.
- `FilterServiceTest` for existing semantic labels/search behavior.
- Environment-gated live runtime smoke when model assets are available.

Self-review focus:

- Check that embedding vector dimensions, model keys, and persistence compatibility are unchanged.
- Check that timeout/cancellation semantics match the old C++ expectations.

## Phase 5 - Remote Image Analysis MVP

Goal: add the first non-CLIP remote AI task over Rust HTTPS, with OpenRouter and Volcengine Ark /
火山方舟 as the two built-in remote providers. Image understanding and image rating are separate task
contracts even if a provider request can return both.

### Phase 5a - Provider Config Loader And Registry

Goal: make provider selection data-driven without turning provider JSON files into executable code.

Deliverables:

- Add a Rust `provider_config` module that loads built-in JSON provider configs and optional
  user-provider configs from a configured directory.
- Embed built-in configs with `include_str!` for Phase 5 so Windows packaging cannot omit them.
- Add a validated `ProviderConfig` schema with fields for `provider_id`, `driver`, `base_url`,
  `endpoint`, auth `credential_slot`, attribution headers, structured-output mode, response
  extraction pointers or driver-owned parser mode, model capabilities, and limits.
- Add config validation: HTTPS-only except localhost dev, no raw secrets, known driver id, known
  schema version, allowed header names, bounded timeout/payload limits, and JSON Pointer syntax
  checks.
- Add capability descriptors from loaded provider configs so C++ can display remote-provider
  availability before a task starts.
- Add user-config precedence rules: user configs can add providers or override model defaults, but
  cannot override credential policy, disable schema validation, or enable arbitrary code execution.

Tests:

- Rust config-loader tests for built-in load, user override, duplicate provider id, unknown driver,
  invalid HTTPS policy, raw-secret rejection, invalid JSON Pointer, and schema-version mismatch.
- Capability-registry tests showing OpenRouter and Volcengine Ark model capabilities become
  `image_understanding.describe` / `image_rating.score` descriptors.

Review focus:

- Check that provider configs are data only: no eval, no scripts, no shell, no secrets.
- Check that invalid user configs fail closed and produce actionable diagnostics.

### Phase 5b - Image Analysis Protobuf And Alcedo Task Schemas

Goal: freeze Alcedo's provider-independent result contracts before writing HTTP provider code.

Deliverables:

- Add `image_analysis.proto` (or equivalent) with typed request/response messages for:
  - `image_understanding.describe`
  - `image_rating.score`
- Include `AiRequestHeader` / `AiResponseHeader` in every request/response.
- Define code-owned JSON Schemas for the two task outputs. Provider configs select injection mode;
  they do not define business fields.
- Include provider/model/prompt profile identity, selected rendition metadata, usage metadata, and
  provider request id in the response.
- Add a mock Rust provider that returns valid typed results without HTTP.

Tests:

- Rust proto/service tests for valid understanding, valid rating, missing credential, timeout, and
  schema-validation failure.
- C++ generated-proto build coverage after adding the new proto.

Review focus:

- Check that rating and understanding cannot overwrite each other because they carry distinct
  `task_id`s and result identities.
- Check that provider-specific raw JSON is not exposed as the public task contract.

### Phase 5c - OpenRouter And Volcengine Ark Drivers

Goal: implement the first real remote providers through OpenRouter's Chat Completions-compatible API
and Volcengine Ark's Responses API.

Deliverables:

- Add `reqwest` with explicit Rustls TLS features and no plaintext/invalid-cert production mode.
- Add `OpenRouterChatProvider` behind the `openrouter_chat` driver id.
- Build `POST /api/v1/chat/completions` requests from the loaded OpenRouter config.
- Keep OpenRouter request and response fixtures compatible with OpenRouter Go SDK chat-completion
  types, even though the production Rust implementation uses `reqwest`.
- Add `VolcengineArkResponsesProvider` behind the `volcengine_ark_responses` driver id, with the
  bundled config defaulting to `doubao-seed-2-0-lite-260428`.
- Build `POST /responses` requests from the loaded Volcengine config and Ark data-plane base URL.
- Keep `volcengine_ark_chat` as a reserved compatibility driver unless live provider testing proves
  the default Doubao path needs Chat API instead of Responses API.
- Resolve the OpenRouter or Volcengine API key from `credential_ref` through the Rust credential
  vault and send it only as an `Authorization: Bearer ...` header.
- Send optional `HTTP-Referer` and `X-OpenRouter-Title` attribution headers from config.
- Send non-streaming requests with `response_format.type = "json_schema"` and strict JSON Schema
  when the selected model supports it.
- Send `provider.require_parameters = true` whenever structured output is required.
- Support optional privacy routing knobs from config/user settings, such as
  `provider.data_collection = "deny"` or `provider.zdr = true` when available.
- For Volcengine, construct the Responses API request from the Alcedo task schema, selected image
  rendition, and prompt profile; the typed driver owns the exact request-field mapping.
- Extract `choices[0].message.content`, parse JSON, validate against the Alcedo task schema,
  normalize values, and return typed protobuf fields for OpenRouter.
- Extract Ark Responses output content with a typed parser, parse JSON, validate against the Alcedo
  task schema, normalize values, and return typed protobuf fields for Volcengine.
- Capture response `id`, `model`, `usage`, and any available request-id header for diagnostics and
  usage/cost display.
- Map OpenRouter, Ark, and transport errors into `AiResponseStatus` / `AiErrorCode`, with redacted
  messages.

Tests:

- Rust HTTP tests with a local mock server for auth header placement, attribution headers,
  structured-output request body, `provider.require_parameters`, response parsing, usage capture,
  rate-limit mapping, 5xx retry policy, cancellation, and timeout.
- Rust HTTP tests with a local mock server for Volcengine Ark auth header placement, Responses API
  request body, structured-output request body, typed output extraction, usage capture, provider
  error-code mapping, cancellation, and timeout.
- Negative tests proving API keys, image payloads/base64, prompts, and raw provider bodies are not
  emitted in routine logs or error strings.
- Manual OpenRouter smoke behind an environment-gated test that is skipped without credentials.
- Manual Volcengine Ark smoke behind an environment-gated test that is skipped unless
  `ALCEDO_VOLCENGINE_ARK_API_KEY` or `ALCEDO_ARK_API_KEY` is set.

Review focus:

- Check that OpenRouter and Volcengine compatibility are implemented by drivers, not by arbitrary
  JSON templates.
- Check that a provider schema failure cannot create an active annotation.
- Check that the Volcengine response parser is backed by a live smoke fixture before Phase 5 is
  marked complete.
- Check that retries are bounded and do not retry non-idempotent or paid calls too aggressively.

### Phase 5d - C++ Runtime Client And Host Image Analysis Service

Goal: expose remote image analysis to the host while keeping C++ ownership of image rendition,
project state, and persistence.

Pre-execution decisions (2026-06-25):

- Do not make LibRaw embedded thumbnails the Phase 5d source path. They are attractive because they
  are cheap and already compressed in many RAW files, but using them as the first implementation would
  bypass the current thumbnail/render cache semantics, vary by camera/file, and force `ThumbnailService`
  ownership changes before the remote-analysis contract is proven. Keep embedded thumbnails as a later
  optimization behind an explicit rendition source such as `embedded_preview`, not as the MVP path.
- Phase 5d uses the existing `ThumbnailService`/thumbnail provider boundary with
  `ThumbnailResolution::k1024` as the default remote-analysis rendition. The service should request a
  1024 max-edge thumbnail/preview, materialize it on CPU, and record the selected max edge and source in
  `RenditionMetadata`.
- Remote image analysis must send encoded image bytes, not the CLIP path's raw `rgba8:WxH` payload.
  Raw RGBA8 is appropriate for local CLIP inference because it avoids decode/encode overhead inside the
  same process family. For remote multimodal APIs, encoded transfer is the right boundary: smaller wire
  payloads, provider-native image inputs, and no accidental multi-megabyte raw uploads.
- Use JPEG as the default host-side upload encoding for photographic RGB thumbnails, with a fixed
  quality setting in the service (for example 90). Fall back to PNG only when preserving alpha or a
  non-photographic/diagnostic fixture matters. The Rust provider drivers already detect PNG/JPEG/WebP/GIF
  and pass encoded bytes through to data-URI or raw-base64 provider shapes, so Phase 5d should avoid
  sending undecodable raw RGBA8 to `ImageAnalysisService`.
- Use OpenImageIO for the host-side JPEG/PNG encoding path, not OpenCV `imencode` / `imwrite`.
  OpenCV image-codec failures have already shown up several times in this repository, while OIIO is an
  existing required dependency and is already used by the thumbnail disk cache and export writer. The
  Phase 5d encoder should expose a simple in-memory result (`bytes`, `mime_type`, `max_edge`, quality,
  dimensions); internally it may use an OIIO memory sink if the linked OIIO version supports it, or a
  scoped temporary file + readback fallback if that is the stable Windows/MSVC path. That fallback must
  stay hidden inside the encoder helper and must not leak temp files on cancellation/failure.
- Update the image-analysis wire contract/comment so `image_format_hint` covers encoded hints such as
  `image/jpeg;max_edge=1024` or `image/png;max_edge=1024`. The old `rgba8:WxH` wording belongs to the
  semantic embedding RPC only.
- Keep remote API concurrency at 1 for Phase 5d. Providers differ on paid-call concurrency/rate limits,
  and description/rating calls are non-idempotent from a billing perspective. The C++ host
  `ImageAnalysisService` should serialize remote requests through one worker/queue and expose progress
  as queued/running/cancelled. Provider-specific concurrency can become a later configuration only after
  the product UI and retry policy are clear.

Deliverables:

- Extend `IAiSidecarRuntimeClient` / `GrpcAiSidecarRuntimeClient` with typed image-analysis RPCs.
- Add a C++ `ImageAnalysisService` (or narrow `ImageUnderstandingService` plus rating companion)
  that prepares thumbnails/previews via existing host services, registers credentials with the
  sidecar, calls the typed RPC, and returns structured DTOs.
- Add a small host-side remote-analysis rendition encoder: request `ThumbnailResolution::k1024`,
  convert/sync to CPU, encode JPEG by default, produce encoded bytes plus an encoded
  `image_format_hint`, and keep the raw RGBA8 conversion path isolated to semantic CLIP generation.
  Prefer OpenImageIO for the encoder implementation; do not use OpenCV imgcodecs as the primary JPEG
  path.
- Keep all database writes in C++; the sidecar returns results only.
- Add cancellation propagation from C++ job id/request id to `AiRuntimeService.CancelTask`.
- Add a Phase 5d-local serial dispatch limit of one in-flight remote analysis request.

Tests:

- C++ fake-sidecar tests for success, missing credential, invalid provider config, timeout,
  cancellation, and schema-error propagation.
- C++ encoder tests proving a 1024 max-edge thumbnail is encoded as JPEG by default, reports encoded
  format metadata, and does not send `rgba8:WxH` to the image-analysis RPC.
- C++ encoder tests covering OIIO failure cleanup: no leftover temporary files if the implementation
  falls back to temp-file readback, and no OpenCV imgcodecs dependency in the primary encode path.
- C++ queue tests proving two remote analysis jobs run serially when the in-flight limit is one, and
  that cancelling a queued or running job does not start an extra provider call.
- Tests proving raw API key material never enters `AiSidecarRuntimeOptions`, process args, QSettings,
  project files, or captured logs.

Review focus:

- Check that the host controls which image rendition is sent and records that rendition in result
  metadata.
- Check that Phase 5d did not refactor `ThumbnailService` or introduce a LibRaw embedded-thumbnail fast
  path before the encoded-rendition contract is proven.
- Check that encoded remote-analysis payloads and raw CLIP embedding payloads remain separate code
  paths.
- Check that the JPEG/PNG upload encoder uses OpenImageIO as the primary codec path and does not
  reintroduce fragile OpenCV image-codec behavior.
- Check that remote calls are serialized at the host boundary and that retries cannot multiply
  concurrency.
- Check that sidecar startup remains on demand and normal browsing/search do not require API keys.

### Phase 5e - Local Prefill Queue Before Persistence

Goal: overlap local rendition preparation with the single in-flight remote LLM request, without
writing any database rows yet.

Rationale: Phase 5d correctly keeps paid/non-idempotent remote calls serialized through
`ImageAnalysisInFlightGate`, but its per-item loop prepares the next thumbnail/JPEG only after the
previous remote call returns. The intended product behavior is a small host-side pipeline: while image
N is flying to the remote provider, C++ should prepare image N+1 locally and place the encoded
rendition in a bounded ready queue. The gate still limits remote calls to one; the prefill queue only
keeps local CPU/cache work ahead of the provider.

Deliverables:

- Refactor `ImageAnalysisService::RunJob` into a small producer/consumer pipeline:
  - producer: `ThumbnailService` request -> CPU materialization -> `EncodeThumbnailForRemoteAnalysis`
    -> push an encoded item into a bounded ready queue.
  - consumer: pop encoded item -> `ImageAnalysisInFlightGate::Acquire` -> `DescribeImage` /
    `ScoreImage` -> `Release` -> append structured DTO result.
- Keep the ready queue bounded, initially `prefetch=1` or `prefetch=2`. Do not let a large album
  accumulate unbounded JPEG byte buffers in memory.
- Release each `ThumbnailGuard` immediately after encoding. The queue must contain only encoded bytes,
  rendition metadata, item id, request id, and task/provider options; it must not hold thumbnail pins
  while waiting for the remote provider.
- Keep `ImageAnalysisInFlightGate` as the remote-call boundary. This phase must not increase remote
  provider concurrency; it only overlaps local preparation with the active remote request.
- Preserve cancellation semantics:
  - cancel stops the producer from requesting/encoding more thumbnails,
  - wakes a producer or consumer blocked on the queue,
  - wakes any wait on `ImageAnalysisInFlightGate`,
  - best-effort cancels only this job's in-flight remote request,
  - discards post-RPC results if cancellation happened during the provider call.
- Keep credential handling unchanged: register once at job start, clear the local secret copy, and
  thread only the opaque `credential_ref` through queued encoded items.
- Keep persistence out of this phase. The output is still `ImageAnalysisItemResult` DTOs only.

Tests:

- C++ pipeline test proving image 2 is thumbnail-requested/encoded while image 1 is blocked in the
  fake remote `DescribeImage` / `ScoreImage` call.
- Bounded-queue test proving prefetch does not exceed the configured queue depth and does not request
  the whole album at once.
- Cancellation tests for:
  - cancel while producer is waiting for queue capacity,
  - cancel while consumer is waiting for an encoded item,
  - cancel while a remote request is in flight,
  - cancel after some encoded-but-not-sent items exist.
- Pin-lifetime test proving `ReleaseThumbnail` is called after encode and before the encoded item waits
  behind the remote gate.
- Regression test proving two jobs sharing one `ImageAnalysisInFlightGate` still serialize remote RPCs,
  even if both jobs locally prefill their queues.

Review focus:

- Check that the queue stores encoded payloads, not `ThumbnailGuard` / `ImageBuffer` pins.
- Check that remote provider concurrency remains one across all services sharing the gate.
- Check that cancellation cannot cancel another job's in-flight request and cannot leave the gate or
  queue permanently blocked.
- Check memory behavior for large albums: bounded JPEG queue, no unbounded thumbnail pins, no database
  writes.

### Phase 5f - Storage And Search Integration

Goal: persist remote analysis results without mixing searchable understanding with subjective rating.

Deliverables:

- Add storage for AI image annotations with file id, task id, provider id, model id, prompt/profile
  id, selected rendition, caption, tags, scene/category hints, confidence, created time, and
  active-for-search state.
- Add rating storage with file id, task id, provider id, model id, prompt/profile id, score fields,
  rubric id/version, reasons, created time, and active-for-rating state.
- Enforce at most one active understanding result per `(file_id, task_id)` for search.
- Keep rating out of full-text search by default; expose it later as sort/filter/recommendation
  data only when a product rubric is approved.
- Extend search document construction so active captions/tags can participate in full-text search.
- Extend delete cleanup so deleting files removes both understanding and rating rows.

Tests:

- Storage controller tests for insert, replace, active selection, provider/model/prompt identity,
  delete cleanup, and rating-vs-understanding isolation.
- `FilterServiceTest` or equivalent coverage showing captions/tags are searchable only after
  successful active persistence, while rating scores do not enter full-text search.

Review focus:

- Check that failed remote calls do not create partial active search documents.
- Check that prompt/profile/rubric changes do not reinterpret old scores as new scores.

### Phase 5g - Developer Smoke And Handoff

Goal: prove the MVP path end to end before product UI wiring in Phase 6.

Deliverables:

- Add CLI/dev smoke paths or environment-gated tests that run one real OpenRouter request and one
  real Volcengine Ark request against a small fixture image when the matching API key env var is set.
- Record required environment variables, skipped-test behavior, and expected output shape.
- Write the Phase 5 self-review conclusion in the required plan format.

Tests:

- Full Rust focused tests for provider config, OpenRouter driver, Volcengine Ark driver, schema
  validation, and mock provider.
- Targeted C++ tests for runtime client, host service, storage, and search.
- Environment-gated real OpenRouter and Volcengine Ark smokes, skipped by default.

Review focus:

- Check that no raw API key appears in diagnostics, logs, screenshots, settings, process args, or
  packed projects.
- Check that OpenRouter and Volcengine model/provider metadata and usage are captured enough for
  Phase 6 UI.

## Phase 6 - Protocol-First Product Wiring For Credentials, Caption, And Rating

### Phase 5f handoff — live data-carrier payload shapes (response header + content stripped)

The two JSON bodies below are the SHAPE of the data carriers the Phase 5f
env-gated live run received from the remote provider for one image in
`install_test.alcd` (provider `volcengine_ark_coding`, model
`doubao-seed-2.0-lite`). The gRPC response wraps each in an `AiResponseHeader`
(request_id, provider, model_id, usage, provider_request_id, error, elapsed_ms,
rendition) — that header metadata is stripped here. The field VALUES for the
photo-describing fields (`caption`, `tags`, `scene`, `reasons`) are ALSO stripped
and replaced with `<redacted ...>` placeholders: the live image's actual content
is private and is NOT recorded in this plan doc (the live-smoke test already
states "the caption is printed to stdout only (NOT recorded in the plan doc)").
What remains is the exact field set, types, and structure a Phase 6 consumer
gets after `ImageAnalysisService` -> `AiSidecarRuntimeService` ->
`GrpcAiSidecarRuntimeClient::DescribeImage`/`ScoreImage` -> sidecar HTTP driver —
enough for handoff without leaking photo contents. Each body is an instance of
the code-owned JSON Schema the driver validates + normalizes before returning
(`IMAGE_UNDERSTANDING_SCHEMA` / `IMAGE_RATING_SCHEMA` in
`rust/puerh_mind/src/service/image_analysis.rs`): `caption` + `tags` required
(understanding), `rating` + `rubric_id` required (rating), `rating` an integer in
1..5 with NO `confidence` (Phase 5f rating-contract change; understanding still
carries `confidence` in 0..1).

`image_understanding.describe` body (`AlcedoImageUnderstanding`):

```json
{
  "caption": "<redacted: photo-describing caption>",
  "tags": [
    "<redacted tag>",
    "<redacted tag>"
  ],
  "scene": "<redacted: photo-describing scene>",
  "confidence": 0.95
}
```

`image_rating.score` body (`AlcedoImageRating`):

```json
{
  "rating": 5,
  "rubric_id": "general",
  "rubric_version": "1.0",
  "reasons": "<redacted: photo-describing reasons>"
}
```

Goal: make remote image analysis usable from the album workflow without disturbing ordinary search or
requiring users to re-enter API keys every session. Product setup is protocol-first: users pick a
compatible protocol preset (`openai_chat_compatible` or `anthropic_messages`) and fill endpoint,
model id, and API key metadata. Brand/provider names such as Opencode, Volcengine, OpenRouter, or a
local compatible server are presets over those protocol families, not separate product concepts.
When an endpoint supports model listing, the backend should discover visible model ids first and let
the selected model be written back into provider config / user preset state instead of relying on a
user to type opaque model ids from memory.

Primary Phase 6 assumption: the user's available paid path is Opencode, not OpenRouter. Add Opencode
as the first product-facing compatible preset family, but do not mark any Opencode model as
image-analysis capable until a live smoke confirms both image input and structured JSON output for
the selected model. The already-live Volcengine Ark Coding Plan remains a useful
Anthropic-compatible smoke target. The existing OpenRouter built-in remains an optional legacy
OpenAI-compatible preset/test fixture; Phase 6 should not block on OpenRouter access.

Structured-output rule:

- For OpenAI-compatible chat, request Alcedo's schema through `response_format: { "type":
  "json_schema", ... }` when the endpoint supports it.
- For Anthropic-compatible Messages, request Alcedo's schema through tool use (`tools` +
  `tool_choice`) and extract the tool input.
- When a host analysis run asks for multiple outputs for the same image (for example description,
  rating, and rating reason), the provider request is still a single per-image request. The selected
  outputs adjust the JSON Schema / Anthropic tool `input_schema` and prompt instructions; they must
  not fan out into separate `describe`, `score`, and `reason` provider round-trips for the same image.
  Persistence can still split the combined result into distinct understanding and rating rows.
- If an endpoint cannot enforce structured output through one of the supported protocol mechanisms,
  fail closed and do not expose it as a product preset. Do not add a "prompt it to return JSON" path
  as a fallback; that would hide provider incompatibility and make persistence ambiguous.
- A model-list response only proves the account/endpoint can see a model id. It does not prove image
  input or schema/tool-use support. Discovered models therefore start unverified
  (`supports_vision=false`, `supports_structured_output=false`, `live_confirmed=false`) until a
  validation smoke proves the selected model accepts the exact Phase 6 image+structured-output
  contract.

### Phase 6a - Compatible Provider Presets And Config Contract

Deliverables:

- Rename the Phase 6 product mental model from "provider brand" to "compatible protocol preset" in
  controller DTOs and UI copy where practical. Keep existing Rust `provider_id` fields for wire
  compatibility, but make their meaning "configured endpoint id".
- Define the editable preset fields: display name, protocol family, base URL, endpoint, auth type,
  credential slot label, model id, optional model display name, optional model-list endpoint,
  structured-output mode, timeout, max image bytes, and recommended rendition.
- Add Opencode preset templates:
  - `opencode_go_anthropic`: `anthropic_messages`, base URL
    `https://opencode.ai/zen/go/v1`, endpoint `/messages`.
  - `opencode_go_openai`: `openai_chat_compatible`, base URL
    `https://opencode.ai/zen/go/v1`, endpoint `/chat/completions`.
  - If the user is on Opencode Zen rather than Go, add parallel `https://opencode.ai/zen/v1`
    presets after checking the account's model/endpoint access.
- Keep Volcengine Ark Coding Plan as a built-in Anthropic-compatible sample preset because the
  Phase 5f live smoke already proved that path.
- Move OpenRouter copy into "optional compatible preset / legacy Phase 5 smoke" wording. Do not show
  it as the primary recommendation unless the user explicitly chooses it.
- Add a config validation rule that a preset is advertised for image analysis only when the selected
  model has `supports_vision && supports_structured_output` or a live smoke has explicitly pinned
  that capability.

Tests:

- Rust provider-config tests for Opencode preset parsing, HTTPS policy, no raw secret in JSON,
  duplicate endpoint-id handling, and capability advertisement gated on vision + structured output.
- C++ DTO/controller tests proving the selected preset survives settings round-trip without storing
  a raw API key.

### Phase 6b - Generic OpenAI-Compatible And Anthropic-Compatible Drivers

Deliverables:

- Refactor `OpenRouterChatProvider` shared code into a generic `openai_chat_compatible` driver, with
  OpenRouter-specific routing/attribution knobs left optional and disabled for Opencode by default.
- Keep `AnthropicMessagesProvider` generic and ensure it is not coupled to Volcengine Ark Coding
  Plan names. It should accept Opencode-compatible base URLs/endpoints from config.
- Build request bodies from the code-owned Alcedo task schemas for both compatible protocols. Config
  selects the protocol and endpoint; it does not own prompt text, response schema, or business
  result fields.
- Capture usage metadata and provider request ids when the compatible endpoint reports them, but make
  those fields optional because compatible providers do not all report identical usage shapes.
- Add an explicit "unsupported structured output" failure path so a compatible endpoint that ignores
  JSON Schema/tool-use does not create active annotations.

Tests:

- Rust mock-server tests for `openai_chat_compatible`: bearer auth, image content placement, JSON
  Schema `response_format`, JSON extraction, usage extraction, provider 4xx/429/5xx mapping, and
  redaction.
- Rust mock-server tests for `anthropic_messages` with an Opencode-style base URL: tool schema,
  image block, tool-use extraction, missing/wrong tool-use schema failure, and redaction.
- Existing OpenRouter and Volcengine tests stay green after the generic-driver refactor.

### Phase 6c - Credential Store, Settings, And Validation Flow

Deliverables:

- Add a host-side `AiCredentialStore` backed by an OS credential store (QtKeychain or a small native
  wrapper after dependency review). `QSettings` may store only non-secret metadata: selected preset
  id, protocol family, endpoint, model id, masked key label, and remember/delete preference.
- On connection validation or job start, C++ reads the secret from the OS store, calls
  `AiSidecarRuntimeService.RegisterCredential`, receives an opaque handle, and passes only that
  handle to image-analysis RPCs.
- Add "validate connection" as a dry run against the selected compatible endpoint. Prefer a tiny
  schema-capability smoke if the endpoint/model supports it; otherwise validate credential and model
  listing without persisting annotations.
- Add backend model discovery for compatible presets:
  - OpenAI-compatible: request the endpoint's model list using the same credential/auth path. The
    default discovery endpoint is `{base_url}/models`, with an optional config override for providers
    that expose model listing elsewhere.
  - Anthropic-compatible: request the endpoint's model list using the configured auth convention
    (`Authorization: Bearer`, `x-api-key` + `anthropic-version`, or `none`). The default discovery
    endpoint is `{base_url}/models`, with an optional config override for Opencode/Ark-compatible
    variants if live testing proves a different path.
  - Parse the provider's list response into provider-independent `DiscoveredModel` DTOs containing
    model id, display name when reported, source preset/provider id, and raw capability flags only
    when the provider reports trustworthy capability metadata.
  - Merge discovered models into the user-provider config/preset state as candidates, but keep them
    unadvertised until the image+structured-output validation smoke succeeds.
- Tighten model selection after discovery lands: a non-empty request `model_id` must resolve to a
  known model entry from built-in config or discovered/persisted user config. Unknown explicit model
  ids fail before any provider HTTP call. This closes the Phase 6b review gap where an unlisted
  model slug could bypass `supports_structured_output` checks.
- Revoke runtime handles on provider logout, settings deletion, sidecar stop, and project close.
- Developer env overrides (`ALCEDO_OPENCODE_API_KEY`, existing provider-specific keys) are allowed
  for tests/smoke only and must never be copied into `QSettings` or packed projects.

Tests:

- Controller tests for missing credential, saved credential, masked display, delete credential,
  validation success, validation auth failure, validation network failure, and runtime handle revoke.
- Rust/C++ tests for model discovery: OpenAI-compatible `/models`, Anthropic-compatible `/models`,
  bearer vs `x-api-key` auth, provider 4xx/429/5xx mapping, optional pagination when exposed by the
  provider, redaction, merge into user config without marking capabilities verified, and unknown
  explicit `model_id` failing before any paid image-analysis HTTP request.
- Redaction tests covering settings dumps, QML-exposed properties, runtime args, diagnostics logs,
  and packed project files.

### Phase 6d - Album Job Wiring

Deliverables:

- Add album actions for:
  - generate/refresh captions and tags (`image_understanding.describe`);
  - generate/refresh rating (`image_rating.score`);
  - combined multi-output analysis that sends one provider request per image and still writes
    separate understanding/rating task results.
- Construct `ImageAnalysisService` with one shared `ImageAnalysisInFlightGate` owned by the album
  backend so jobs serialize across the whole album flow, not per service instance.
- Start the sidecar on demand with `require_model_info=false` when only remote image analysis is
  needed, so ordinary album browsing/search does not require a running sidecar or an API key.
- Add progress, cancellation, retry, and clear error states. A cancelled or failed remote call must
  not upsert an active understanding/rating row.
- Reuse the Phase 5d encoded-rendition path and cap prefetch/image bytes from the selected preset.

Tests:

- Controller tests for empty selection, one-image success, multi-image success, cancel, retry,
  provider error, schema error, and no partial active annotation after failure.
- QML smoke tests for visible states if UI is added in this phase.

### Phase 6e - Sidecar Client Module Refactor

Goal: stop growing `app/ai_sidecar_runtime_service.cpp` as the place where every sidecar concern
lands. Phase 6d proved the product flow can call the sidecar from album code; the next phase is a
structural cutover before persistence/search work adds more permanent call sites. The runtime
service should manage the sidecar process lifecycle. A new `sidecar_client` module should own every
direct gRPC API, protobuf DTO conversion, request envelope, and task-specific client.

Reflection on the current shape:

- `ai_sidecar_runtime_service.cpp` is doing at least four unrelated jobs: runtime binary discovery
  and `QProcess` lifecycle, readiness/status state, gRPC transport/stub creation, and hand-written
  protobuf mapping for semantic, model-manager, credential, capability, and image-analysis DTOs.
- `IAiSidecarRuntimeClient` is a single broad interface for runtime control, model management,
  embedding, credential vault, cancellation, image description, rating, and model discovery. Adding
  one task means editing one god interface, one god implementation, and one god test fake.
- DTO conversion is hidden in anonymous `ToXxx(...)` functions instead of being attached to the DTO
  contract. That makes mapper coverage hard to target and makes call sites depend on whatever file
  happens to include the generated protobuf headers.
- `AiSidecarRuntimeService` currently preserves old call shapes by offering ready-guarded forwarding
  methods for every sidecar API. That keeps the old boundary alive and makes the runtime service a
  second client surface.

Target module layout:

```text
alcedo_studio/src/include/sidecar_client/
  proto_dto.hpp
  client.hpp
  runtime_control_client.hpp
  credential_client.hpp
  model_manager_client.hpp
  semantic_embedding_client.hpp
  image_analysis_client.hpp
  dto/runtime.hpp
  dto/credentials.hpp
  dto/model_manager.hpp
  dto/semantic_embedding.hpp
  dto/image_analysis.hpp

alcedo_studio/src/sidecar_client/
  client.cpp
  runtime_control_client.cpp
  credential_client.cpp
  model_manager_client.cpp
  semantic_embedding_client.cpp
  image_analysis_client.cpp
  dto/runtime.cpp
  dto/credentials.cpp
  dto/model_manager.cpp
  dto/semantic_embedding.cpp
  dto/image_analysis.cpp
```

The CMake target should be `SidecarClient`, with `SemanticProto` and `AiProto` as private protocol
dependencies. App services may include `sidecar_client/*.hpp`; only files under
`src/sidecar_client` should include generated `*.pb.h` / `*.grpc.pb.h` headers after the cutover.

DTO/protobuf conversion contract:

- Introduce a small CRTP-style helper in `sidecar_client/proto_dto.hpp`. DTOs declare their protobuf
  counterpart and expose static/self conversion, for example:

  ```cpp
  struct ImageAnalysisRatingResult
      : ProtoDto<ImageAnalysisRatingResult, alcedo::ai::ScoreImageResponse> {
    static auto FromProto(const alcedo::ai::ScoreImageResponse& proto,
                          std::string fallback_request_id = {}) -> ImageAnalysisRatingResult;
    void ToProto(alcedo::ai::ScoreImageResponse* proto) const;
  };
  ```

- Request DTOs own `ToProto(...)`; response DTOs own `FromProto(...)`; bidirectional DTOs own both.
  The implementation lives in `src/sidecar_client/dto/*.cpp` beside generated protobuf includes.
- Shared envelope fields (`AiRequestHeader`, `AiResponseHeader`, deadlines, credential refs,
  request ids, task ids) move into `sidecar_client`, not `AiSidecarRuntimeService`.
- No anonymous `ToRuntimeModelInfo`, `ToEmbeddingResult`, `ToImageRatingResult`, or equivalent mapper
  helpers remain in `ai_sidecar_runtime_service.cpp`.

Client surface after cutover:

- `sidecar_client::Client` owns one channel factory / channel and exposes typed member modules:
  `runtime()`, `credentials()`, `models()`, `semantic()`, and `image_analysis()`.
- `RuntimeControlClient` owns `Ping`, `GetRuntimeStatus`, `ListCapabilities`, and `CancelTask`.
- `CredentialClient` owns `RegisterCredential` and `RevokeCredential`.
- `ModelManagerClient` owns model profile/install/validate/delete/download/status/cancel RPCs that
  are actually exposed by the sidecar contract.
- `SemanticEmbeddingClient` owns current semantic embedding RPCs and `GetModelInfo`. The production
  app path should use the current versioned/batch protocol directly. Remove the runtime-service
  v2-to-v1 fallback path; an old sidecar returning `UNIMPLEMENTED` should fail with a clear protocol
  error instead of silently switching wire contracts.
- `ImageAnalysisClient` owns `DescribeImage`, `ScoreImage`, and `ListModels`.
- Tests that need fakes fake the narrow module interface they use, not the entire sidecar runtime.

Runtime-service boundary after cutover:

- `AiSidecarRuntimeService` keeps only lifecycle concerns: default binary/model-root resolution,
  option-to-argument construction, port selection, `QProcess` start/stop, stdout/stderr tail,
  child-tree cleanup, readiness polling, and runtime status snapshots.
- `AiSidecarRuntimeService` constructs or receives a `std::shared_ptr<sidecar_client::Client>` and
  exposes a ready session/client reference after `StartAndWait`. It should not keep
  method-by-method wrappers such as `DescribeImage`, `ScoreImage`, `EmbedImageBatch`,
  `RegisterCredential`, or `ListModels`.
- Readiness checks may call `client->runtime().Ping(...)` and, when `require_model_info=true`,
  `client->semantic().GetModelInfo(...)`. Those are lifecycle checks, not a general client facade.
- `app/ai_sidecar_runtime_service.hpp` should no longer declare semantic embedding DTOs,
  image-analysis DTOs, model-manager DTOs, `IAiSidecarRuntimeClient`, or
  `GrpcAiSidecarRuntimeClient`.

Consumer cutover:

- `SemanticGenerationService` and semantic search provider code receive/use
  `sidecar_client::SemanticEmbeddingClient` (or a `sidecar_client::Client` session) for embeddings
  and model info. They keep their generation/search business logic in `app/` and storage.
- `ImageAnalysisService` receives/use `sidecar_client::CredentialClient`,
  `sidecar_client::RuntimeControlClient`, and `sidecar_client::ImageAnalysisClient` through a
  narrow bundle or `sidecar_client::Client` session. Credential registration/revoke stays explicit;
  no raw key reaches runtime options, process args, or logs.
- `ImageAnalysisController` and `AlbumImageAnalysisEnvironment` ask the runtime service only to
  ensure the sidecar process is ready, then pass the live sidecar client/session into
  `ImageAnalysisService`.
- `ProjectService` remains the owner that lazily creates the runtime service, but it no longer
  makes `AiSidecarRuntimeService` the application-wide RPC facade.

Execution steps:

1. Add `SidecarClient` target, DTO headers, CRTP mapper helper, and module client skeletons.
2. Move all protobuf mapping out of `ai_sidecar_runtime_service.cpp` into `sidecar_client/dto/*.cpp`
   and add focused mapper tests before changing app call sites.
3. Implement the five module clients and replace `GrpcAiSidecarRuntimeClient` with
   `sidecar_client::Client`.
4. Strip `AiSidecarRuntimeService` down to lifecycle/session ownership and delete
   `IAiSidecarRuntimeClient` / `GrpcAiSidecarRuntimeClient` from `app/`.
5. Update `SemanticGenerationService`, semantic search provider wiring, `ImageAnalysisService`,
   `ImageAnalysisController`, and tests to use the new sidecar client modules directly.
6. Delete the old forwarding methods and v1 embedding fallback code. Do not add compatibility
   aliases or old-header forwarding shims.
7. Re-run the focused C++ test group and then the existing semantic/image-analysis regression group.

Tests:

- New `SidecarClientDtoMappingTest`: round-trip or one-way mapper coverage for runtime status/model
  info, capabilities, credential responses, model manager profile/manifest/result, semantic
  embeddings, image understanding, rating, rendition/usage metadata, and discovered models.
- New `SidecarClientModuleTest` or split per module: fake/stubbed gRPC calls prove each module fills
  deadlines, request ids, task ids, credential refs, and maps transport/protocol errors exactly once.
- `AiSidecarRuntimeServiceTest` becomes lifecycle-only: binary missing, start failure, readiness
  timeout, ready status, stop/kill behavior, log tails, and child cleanup. It should not assert
  image-analysis or embedding protobuf mapping.
- Existing `ImageAnalysisServiceTest`, `ImageAnalysisControllerTest`,
  `SemanticGenerationServiceTest`, semantic search tests, and live-smoke skips stay green after the
  include/API cutover.

Acceptance checks:

- `rg "#include \".*(pb|grpc)\\.h\"" alcedo_studio/src/app alcedo_studio/src/include/app` finds no
  sidecar protobuf includes.
- `ai_sidecar_runtime_service.cpp` contains no generated protobuf includes and no task-specific DTO
  mapper functions.
- `AiSidecarRuntimeService` has no public sidecar task API beyond lifecycle/session access.
- There is no production v2-to-v1 semantic embedding fallback and no remote-provider fallback across
  protocol family, model id, structured-output mode, or provider id.
- The refactor lands as a cutover, not a parallel permanent API. Old app-layer client classes,
  forwarding wrappers, and broad fakes are deleted in the same phase.

## Phase 6e - Completion & Self-Review

Status: complete for the backend refactor slice. The sidecar protocol boundary now lives under
`SidecarClient`: generated sidecar protobuf includes are gone from `app/`, `AiSidecarRuntimeService`
has been reduced to process lifecycle/readiness/session ownership, and app consumers call the
live `sidecar_client::Client` session's narrow modules (`runtime`, `credentials`, `models`,
`semantic`, `image_analysis`) instead of runtime-service forwarding methods.

What changed:

- Added the `SidecarClient` CMake target and `sidecar_client` public interfaces/DTO headers.
- Moved gRPC stub creation, request-envelope filling, protobuf-to-DTO mapping, credential calls,
  model-manager calls, semantic v2 embedding calls, runtime control, and image-analysis calls out of
  `app/ai_sidecar_runtime_service.cpp`.
- Deleted the app-layer `IAiSidecarRuntimeClient` / `GrpcAiSidecarRuntimeClient` boundary and the
  method-by-method `AiSidecarRuntimeService` wrappers.
- Removed production semantic v2-to-v1 embedding fallback. The production client now calls the v2
  semantic RPCs directly and reports transport/protocol failure as failed results.
- Updated semantic generation, semantic search, and image analysis to use the runtime service only
  for process readiness/session access.
- Reworked `AiSidecarRuntimeServiceTest` around lifecycle and session injection instead of the old
  broad fake runtime client.

Acceptance verification:

- `rg '#include "(ai_common|ai_runtime|image_analysis|semantic)\.(pb|grpc\.pb)\.h"' alcedo_studio/src
  alcedo_studio/tests -g"*.cpp" -g"*.hpp"` now finds sidecar generated includes only in
  `alcedo_studio/src/sidecar_client/client.cpp`.
- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AiSidecarRuntimeServiceTest
  SemanticGenerationServiceTest ImageAnalysisServiceTest --parallel 4`: succeeded.
- `ctest --test-dir build/debug --output-on-failure -R
  "AiSidecarRuntimeServiceTest|SemanticGenerationServiceTest|ImageAnalysisServiceTest"`: 54/54
  passed, with 2 environment-gated live smoke tests skipped.

## Phase 7 - Persistence, Search, Live Smoke, And Product Cutover

This phase starts only after the `sidecar_client` boundary is in place. Persistence and album search
refresh touch storage/search behavior, so they should not be mixed into the Phase 6 code-shape
refactor.

### Phase 7a - Persistence, Search Refresh, Rating Surface, And Usage Summary

Goal: wire the remote image-analysis results produced by Phase 6d's
`ImageAnalysisController` into persistence, search, the existing EXIF star-rating
surface, and a per-job usage summary — without letting a failed or cancelled call
create an active annotation.

Phase 7a constraint (product decision, 2026-06-27): the numeric AI rating is NOT
persisted through `AiStorageController.UpsertRating`. A photo's star rating is
already the EXIF/metadata `Rating` value surfaced everywhere (star UI, stats
filter, thumbnail cards — read via `json_extract(i.metadata, '$.Rating')`). The AI
score must therefore be applied directly through the existing
`AlbumBackend::SetImageRating` / `ImageController::SetImageRating` path that owns
that metadata column. `AiStorageController` is used in 7a only to persist the
rating *reasons* (the rationale text) plus provider/model/prompt-profile/rubric
identity. `UpsertRating` from Phase 5f is kept but not called in 7a.

#### Decisions (locked via design review, 2026-06-27)

1. **Reasons storage** — reuse the existing `AiImageRating` table. Add a new
   `AiStorageController.UpsertRatingReasons` that writes a row with `rating = 0`
   as a sentinel (NOT the truth), plus `reasons`, `provider_id`, `model_id`,
   `prompt_profile_id`, `rendition_kind`, `rubric_id`, `rubric_version`, `active`.
   Add `AiRating::IsValidReasonsOnly()` (file_id/task/provider/model non-empty +
   reasons non-empty; rating ignored) as the gate. `AiRating::IsValid()` stays
   strict (rating 1..5) so Phase 5f tests are untouched. No DDL change — the
   `rating` column already has `NOT NULL DEFAULT 0`.
2. **Star overwrite policy** — overwrite always. Every successful AI score calls
   the star-rating path with the model's 1..5 value, replacing any existing manual
   star. Accepted risk: a batch AI run silently overwrites manual curation;
   mitigated only by re-running manual rating (UI copy deferred).
3. **Batch write cost** — light per-image, sync once at job end, no
   `SaveProject`/`Package` in 7a. Per image: `Write_NoSync` (in-memory
   `exif_display_.rating_` + MODIFIED flag) + view-state patch +
   `thumbnail_model_.updateRating`. At job end: one `SyncWithStorage()` (flushes
   all MODIFIED image rows in a single transaction) + `stats_.RefreshStats()`. The
   `.alcd` packaged snapshot is left stale until the next normal save/close; the
   live DB is authoritative.
4. **Usage summary surface** — new aggregate `lastUsage` Q_PROPERTY on
   `ImageAnalysisController`: `inputTokens`, `outputTokens`, `totalTokens`,
   `providerRequestIds` (list), `itemsWithUsage`, `itemsWithoutUsage`. No
   per-item usage in `lastResults`; the aggregate is the 7a summary.
5. **"Rating surface" scope** — persist reasons + add a new
   `Q_INVOKABLE GetImageRatingReasons(uint elementId)` read API on `AlbumBackend`
   returning `{hasReasons, reasons, provider, modelId, rubricId, rubricVersion}`.
   No QML UI in 7a (consistent with 6d's "controller is QML-callable, no menu/dialog
   yet").
6. **Search refresh** — after a successful describe job persists rows, emit a
   search-document-changed notification so `stats_.RebuildThumbnailView()` re-runs
   the active search. `AiUnderstandingExpr` is a live correlated subquery against
   `AiImageUnderstanding`, so newly-persisted active rows match immediately; there
   is no materialized index to rebuild.

#### Research conclusion driving decision 3

`SyncWithStorage()` is NOT redundant — it is the only path that writes the rating
into the DuckDB `Image.metadata` JSON column. `Write_NoSync` mutates in-memory
state only; `ProjectService::SaveProject` (`project_service.cpp:502`) writes only
the project-meta JSON (`db_path`, `project_uuid`, `start_id`, `data_summary`) and
touches no image rows; `PackageCurrentProjectFiles` (`project_handler.cpp:311`)
snapshots the DB as-is. Every canonical save path calls `SyncWithStorage()`
immediately before `SaveProject()` — `PersistCurrentProjectState`
(`project_handler.cpp:300-302`), close/switch (`113-114`, `159-160`),
`AlbumBackend::SaveProject` (`1002`), import/export (`434-435`), editor
(`121`, `580`). Removing sync from `SetImageRating` would lose the rating on
reload (DB has the old value) and leave `stats_.RefreshStats()` (a DB
`GROUP BY json_extract(i.metadata,'$.Rating')`) stale. The genuine waste in a
batch is `SaveProject`+`Package` per image; 7a therefore defers those to the next
normal save and does only the cheap DB flush once at job end.

#### Design

**Persistence seam — `IImageAnalysisSink`.** The controller stays decoupled from
`AlbumBackend` (6d invariant) and `ImageAnalysisService` stays storage-agnostic
(5d/6d tests unchanged). A new narrow interface, injected into
`ImageAnalysisController`'s constructor, owns all host-state mutation:

```cpp
class IImageAnalysisSink {
 public:
  virtual ~IImageAnalysisSink() = default;
  // Describe: persist understanding (caption/tags/scene/confidence + identity).
  virtual bool PersistUnderstanding(const alcedo::ImageAnalysisItemResult& r) = 0;
  // Score: persist reasons-only (rating=0 sentinel) + identity. Does NOT write the star.
  virtual bool PersistRatingReasons(const alcedo::ImageAnalysisItemResult& r) = 0;
  // Score: write the 1..5 star into EXIF/metadata in-memory (Write_NoSync) + view/model patch.
  virtual bool ApplyStarRating(uint elementId, uint imageId, int rating) = 0;
  // Score job end: one SyncWithStorage + RefreshStats (durability + star-filter stats).
  virtual void FlushPendingStarRatings() = 0;
  // Describe job end: re-run active search so new captions/tags match.
  virtual void NotifySearchDocumentChanged() = 0;
};
```

The production implementation `AlbumImageAnalysisSink` lives in `album_backend.cpp`
(declared a friend of `AlbumBackend`) and delegates to
`StorageService::GetAiStorageController()`, `ImageController`, and `stats_`. The
test fake records calls so "no upsert on failure" is a one-liner assertion. The
controller constructor becomes `(env, preset, sink, parent)`.

**When persistence fires:** in the service's finished callback, before `Finish()`,
iterate `results`: for each `kAnalyzed` item call `PersistUnderstanding` (describe)
or `PersistRatingReasons` + `ApplyStarRating` (score); skip `kError`/`kCanceled`.
Then: score job → `FlushPendingStarRatings()`; describe job →
`NotifySearchDocumentChanged()`. Persistence happens at job end (not per item) —
simpler, matching the single finished-callback delivery. Accepted risk: a crash
mid-batch loses unsynced reasons/stars for already-completed items; re-running the
job regenerates them (reasons/stars are cheap to recompute).

**Storage — reasons-only path.** `AiRating` gains `IsValidReasonsOnly()`:
`file_id_ != 0 && !task_id_/provider_id_/model_id_.empty() && !reasons_.empty()`
(rating ignored). `IsValid()` stays strict (rating 1..5) so 5f tests are
untouched. `AiStorageController::UpsertRatingReasons(const AiRating&)` validates
via `IsValidReasonsOnly()`, runs the `FileExists` guard, and
`duckorm::insert_or_replace` on `AiImageUnderstanding`'s sibling `AiImageRating`
reusing `kInsertRatingFields` (caller sets `rating_ = 0`). PK `(file_id, task_id)`
guarantees at most one active reasons row per pair. `GetActiveRating` returns it
(rating = 0, reasons filled) — acceptable, since the star truth lives in EXIF.
Identity columns (`provider_id_`, `model_id_`, `prompt_profile_id_`,
`rendition_kind_`, `rubric_id_`, `rubric_version_`) are filled from the result DTO
in both the understanding and reasons paths.

**Rating star path — light writes.** Extract from
`ImageController::SetImageRating` (`image_controller.cpp:856-967`) two pieces:

- `ImageController::ApplyStarRatingLight(elementId, imageId, rating)`: the
  `Write_NoSync` block (897-909) + view-state patch (924-929) +
  `thumbnail_model_.updateRating` (930). NO `SyncWithStorage`/`SaveProject`/
  `Package`/`RefreshStats`.
- `ImageController::FlushPendingStarRatings()`:
  `image_pool->SyncWithStorage()` + `stats_.RefreshStats()`.

`AlbumBackend` exposes both; the sink delegates. The existing `SetImageRating`
Q_INVOKABLE (used for manual single star clicks) is unchanged — it still does a
full sync+save per single user action, which is correct for a one-off click.

**Usage summary.** In `Finish()`, accumulate from each item's
`understanding.usage`/`rating.usage` + `provider_request_id` into a new
`last_usage_` struct; expose as `Q_PROPERTY(QVariantMap lastUsage ...)`. Items
where `usage.total_tokens == 0 && provider_request_id` empty count as
`itemsWithoutUsage`. Also add `promptProfileId` and `providerRequestId` to each
`lastResults` map (provider/modelId already present) — cheap, and satisfies "show
identity on job results."

**`GetImageRatingReasons` read API.** New
`Q_INVOKABLE QVariantMap AlbumBackend::GetImageRatingReasons(uint elementId)` →
`AiStorageController::GetActiveRating(elementId)` → `{hasReasons, reasons,
provider, modelId, rubricId, rubricVersion}`. No QML wiring in 7a.

**Search refresh hook.** `NotifySearchDocumentChanged()` →
`stats_.RebuildThumbnailView()` (mirrors `search_controller.cpp:451`). The live
`AiUnderstandingExpr` correlated subquery picks up the newly-persisted
`AiImageUnderstanding` rows, so an active fuzzy search re-matches immediately.

#### Deliverables

- Add `AiRating::IsValidReasonsOnly()` and `AiStorageController::UpsertRatingReasons`
  (reuses `kInsertRatingFields`; no DDL change).
- Add `IImageAnalysisSink` + production `AlbumImageAnalysisSink`; inject into
  `ImageAnalysisController`. Wire persistence (understanding / reasons + star) +
  job-end flush/notify into the finished callback.
- Extract `ImageController::ApplyStarRatingLight` + `FlushPendingStarRatings` from
  `SetImageRating`.
- Add `lastUsage` aggregate Q_PROPERTY; add `promptProfileId`/`providerRequestId`
  to `lastResults`.
- Add `Q_INVOKABLE GetImageRatingReasons` on `AlbumBackend`.
- Show provider/model/prompt-profile/rubric identity on job results and stored rows
  so prompt or model changes do not reinterpret old annotations.
- Persist successful describe results through `AiStorageController.UpsertUnderstanding`;
  refresh the search path so new captions/tags become searchable only after
  persistence.
- Keep rating out of full-text search (rating reasons never enter the FTS document;
  `AiUnderstandingExpr` reads understanding only).

#### Tests

- `AiStorageControllerTest`: `UpsertRatingReasons` insert + replace (PK), reasons-only
  row has `rating = 0`, `GetActiveRating` returns reasons, `IsValidReasonsOnly`
  rejects empty reasons, `IsValid` (5f) still rejects `rating = 0` (no regression),
  delete cascade still drops reasons rows.
- `ImageAnalysisControllerTest`: extend the fake env with a fake
  `IImageAnalysisSink`; assert (a) describe success → `PersistUnderstanding` per
  `kAnalyzed` + `NotifySearchDocumentChanged` at end; (b) score success →
  `PersistRatingReasons` + `ApplyStarRating` per `kAnalyzed` +
  `FlushPendingStarRatings` at end; (c) provider/schema error + cancel → ZERO sink
  calls (no active annotation); (d) `lastUsage` aggregates tokens +
  provider_request_ids and counts itemsWith/WithoutUsage; (e) `lastResults` carries
  `promptProfileId`/`providerRequestId`.
- `ImageAnalysisServiceTest`: unchanged (service stays storage-agnostic — proves the
  sink boundary is clean).
- `FilterServiceTest` (or controller-level via fake sink + a real
  `SleeveFilterService`): caption searchable only after `PersistUnderstanding`;
  rating reasons never enter FTS.
- AlbumBackend-level (offline if feasible): `GetImageRatingReasons` returns stored
  reasons; `ApplyStarRatingLight` + `FlushPendingStarRatings` leave the DB
  `metadata.Rating` updated and star-filter stats correct.
- Usage summary tests for present/absent usage metadata.

#### Review focus / accepted risks

- No upsert on failure/cancel: the sink receives zero calls for non-`kAnalyzed`
  items (test-enforced); the storage-layer `IsValidReasonsOnly`/`IsValid` backstop
  remains.
- Rating truth is EXIF, not the AI table: `AiImageRating.rating` is a 0-sentinel for
  7a rows; documented; `GetActiveRating` consumers must not treat it as the real
  score. 5f's `UpsertRating` rows with real ratings can coexist under different
  `task_id`s, but 7a never writes them.
- Overwrite-always star policy: a batch AI run silently replaces manual stars —
  accepted per decision 2; mitigated only by re-running manual curation.
- Job-end persistence: a crash mid-batch loses unsynced reasons/stars for completed
  items — accepted (re-run). Reasons persist only at job end, not per item.
- No `SaveProject`/`Package` in 7a: the `.alcd` packaged snapshot is stale until the
  next normal save/close — same as any DB change between manual saves; the live DB
  is authoritative.
- Combined describe+rating was not part of this historical 7a persistence slice.
  Future multi-output analysis follows the 2026-06-28 correction: one provider
  request per image, with separate persisted task results.
- QML UI: still deferred; `GetImageRatingReasons` read API lands ready for later.

Review conclusion (handoff, 2026-06-27): bugs found — none; risk accepted —
(1) overwrite-always star policy, (2) job-end persistence crash window,
(3) `rating = 0` sentinel semantics on `AiImageRating` reasons rows, (4) no
`.alcd` repackage in 7a; missing tests — none (storage reasons-only, controller
sink wiring incl. no-upsert-on-failure, usage aggregate, search refresh, read API
all covered).

#### Phase 7a - Completion & Self-Review

Status: complete. Remote image-analysis results from Phase 6d's
`ImageAnalysisController` now flow into persistence (understanding / rating reasons),
the existing EXIF star-rating surface, the album search view, and a per-job usage
summary — without letting a failed or cancelled call create an active annotation.

What changed:

- `AiRating` gained `IsValidReasonsOnly()` (rating ignored; file key + provider/model
  identity + non-empty reasons). `IsValid()` stays strict (1..5) so Phase 5f tests are
  untouched. `AiStorageController::UpsertRatingReasons` reuses `kInsertRatingFields`
  (caller sets `rating_ = 0` sentinel) with the same `(file_id, task_id)` PK +
  `FileExists` guard — no DDL change.
- New `IImageAnalysisSink` seam (`image_analysis_sink.hpp`) injected into
  `ImageAnalysisController`'s constructor `(env, preset, sink, parent)`. `Finish()`
  iterates results at job end: per `kAnalyzed` item it calls `PersistUnderstanding`
  (describe) or `PersistRatingReasons` + `ApplyStarRating` (score); `kError`/`kCanceled`
  are skipped. The trailing `FlushPendingStarRatings` (score) / `NotifySearchDocumentChanged`
  (describe) fires only when `analyzed_ > 0`, so a fully failed/cancelled job produces
  ZERO sink calls. The service stays storage-agnostic.
- `ImageController::ApplyStarRatingLight` (the `Write_NoSync` + view-state patch +
  `thumbnail_model_.updateRating` half) and `FlushPendingStarRatings`
  (`SyncWithStorage` + `RefreshStats`) were extracted from `SetImageRating`.
  `SetImageRating` (manual single click) is unchanged.
- Production `AlbumImageAnalysisSink` in `album_backend.cpp` (friend of `AlbumBackend`)
  delegates to `AiStorageController`, `ImageController`, and `stats_.RebuildThumbnailView()`.
  task_id slots `"describe"` / `"rate"` match the Phase 5g live-smoke convention.
- New `Q_INVOKABLE AlbumBackend::GetImageRatingReasons(elementId)` returns
  `{hasReasons, reasons, provider, modelId, rubricId, rubricVersion}` from
  `GetActiveRating`. No QML UI in 7a.
- New `lastUsage` Q_PROPERTY (`inputTokens`, `outputTokens`, `totalTokens`,
  `providerRequestIds`, `itemsWithUsage`, `itemsWithoutUsage`) aggregated in `Finish()`;
  `promptProfileId` + `providerRequestId` added to each `lastResults` map.

Acceptance verification (MSVC debug, PowerShell tool per project memory):

- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumBackendLib
  AiStorageControllerTest ImageAnalysisControllerTest FilterServiceTest --parallel 4`:
  succeeded.
- `ctest --test-dir build/debug -R "AiStorageControllerTest|ImageAnalysisControllerTest|
  FilterServiceTest"`: 72/72 passed.
- Regression `ctest --test-dir build/debug -R "ImageAnalysisServiceTest|
  AiSidecarRuntimeServiceTest|SemanticGenerationServiceTest|SemanticStorageControllerTest"`:
  66/66 passed (2 environment-gated live smokes skipped).
- `ImageAnalysisServiceTest` unchanged and green — proves the sink boundary keeps the
  service storage-agnostic.

Accepted risks (unchanged from the locked decisions): overwrite-always star policy;
job-end persistence crash window (re-run regenerates); `rating = 0` sentinel semantics
on `AiImageRating` reasons rows (truth is EXIF `Rating`); no `.alcd` repackage in 7a
(live DB authoritative). AlbumBackend-level offline coverage of `GetImageRatingReasons`
/ `ApplyStarRatingLight` + `FlushPendingStarRatings` is provided through the storage
`GetActiveRating`/`UpsertRatingReasons` round-trip tests plus the controller sink-wiring
tests rather than a heavy AlbumBackend project harness; the read API is a thin delegate
over the tested `GetActiveRating`.

### Phase 7b - Live Smoke Matrix And Handoff

Deliverables:

- Always-run fake-provider smoke remains the deterministic product path.
- Env-gated Opencode smokes:
  - `ALCEDO_OPENCODE_API_KEY` plus `ALCEDO_IA_LIVE_PROVIDER_ID=opencode_go_anthropic` or
    `opencode_go_openai`;
  - skip cleanly when the key/model is absent;
  - list visible models first when the compatible endpoint supports model listing, choose the
    requested `ALCEDO_IA_LIVE_MODEL_ID` if set, otherwise print discovered candidates and skip
    rather than guessing a model;
  - fail closed if the selected model does not accept image input or does not return validated
    structured JSON.
- When a live smoke succeeds, record enough information to update `config.models[]` or a user config
  override: model id, display name, protocol family, endpoint, validation timestamp, tested task(s),
  and the fact that image input + structured output were both verified. Do not flip
  `live_confirmed` from model listing alone.
- Keep the known-good Volcengine Ark Coding Plan smoke as the Anthropic-compatible reference until
  Opencode image+schema capability is confirmed.
- Record the final provider/protocol matrix in the phase handoff with exact endpoint, protocol
  family, model id, image support, structured-output support, and expected skip/fail/pass behavior.

### Phase 7c - Legacy Cleanup And No-Fallback Cutover

Goal: after the compatible-protocol path is live, remove Phase 5-era brand-specific and legacy
provider surfaces instead of carrying them as permanent product complexity.

Deliverables:

- Delete or demote OpenRouter-specific product UI/copy, default recommendations, and required live
  smoke wiring. OpenRouter may remain only as a user-created compatible config or a clearly isolated
  developer fixture if a test still needs the request shape.
- Remove `openrouter_chat` as a product-facing driver id once `openai_chat_compatible` covers the
  same request/response contract. Keep OpenRouter-only routing knobs behind optional config fields
  consumed by the generic driver only when explicitly set.
- Remove unused reserved provider families from product code paths (`volcengine_ark_chat`,
  `generic_json_http`, or any other unimplemented placeholder) unless a concrete live endpoint and
  test require them. Reserved strings may stay documented as design history, but should not appear in
  settings, capability descriptors, or product presets.
- Remove legacy live-smoke defaults that point at OpenRouter or other old providers. The default
  env-gated smoke matrix should target Opencode compatible presets plus the known-good
  Anthropic-compatible reference until Opencode is confirmed.
- Do not fallback across protocol families. If a selected preset is `anthropic_messages`, a failed
  call must not retry as `openai_chat_compatible`; if a selected preset is OpenAI-compatible, it must
  not retry as Anthropic Messages. The selected preset is the contract.
- Do not fallback from schema-enforced structured output to free-form JSON prompting, response
  healing, provider auto-routing that ignores schema parameters, or a different model id. Surface a
  clear capability/configuration error instead.

Tests:

- Rust/provider tests proving an unsupported structured-output mode, missing tool-use, ignored
  `response_format`, or wrong protocol shape returns a typed failure without retrying another
  protocol or model.
- C++ controller tests proving a selected preset id is passed through unchanged and no hidden
  alternate provider is attempted after auth, schema, or provider errors.
- Config/capability tests proving removed legacy provider ids are not advertised in product
  settings or capability descriptors.
- Live-smoke documentation test/handoff check proving OpenRouter is no longer a required or default
  smoke path.

Self-review focus:

- Check user-visible error copy for credential, model capability, schema, cancellation, and network
  failures.
- Check that normal search and browsing remain usable without API keys.
- Check that cancellation and retry do not leave stale progress or half-active annotations.
- Check that no raw API key appears in QML state dumps, settings files, diagnostics logs, process
  arguments, live-smoke stdout, or packed projects.
- Check that the implementation is truly protocol-first: adding another OpenAI-compatible or
  Anthropic-compatible endpoint should require a preset/config, not a new brand-specific product
  branch.
- Check that no remote image-analysis path performs an implicit protocol, provider, model, or
  free-form-JSON fallback after the user selected a preset.

## Future Candidate - Edit Assistant Recipes

This is a speculative AI scene that does not require editor mask support.

Goal: let a model propose non-destructive adjustment recipes, while C++ remains the only owner of
the edit graph.

Possible shape:

- C++ sends a low-resolution preview, selected metadata, current pipeline parameters, and a user
  intent such as "make this feel warmer but keep highlights safe".
- The sidecar returns an `AdjustmentRecipe` containing allowed operator names, parameter deltas,
  confidence, and a short explanation.
- C++ validates every operator name and parameter range before creating a candidate edit version.
- The sidecar never mutates `EditHistory`, project files, or pipeline state directly.

Required tests before product use:

- Validator rejects unknown operators and out-of-range values.
- Fake recipe creates a reversible candidate version.
- Existing editor undo/redo and version branching remain unchanged.

## Deferred - SAM And Smart Masks

SAM should not start in the current AI sidecar plan.

Prerequisites before reopening this work:

- The editor pipeline has a persistent mask/local-adjustment model.
- Mask coordinate spaces are defined across thumbnail, preview, full-resolution image, crop,
  rotate, and lens-correction stages.
- The UI has a clear overlay, refine, apply, undo, and versioning lifecycle.
- Storage has a decision for whether masks are project assets, edit-version assets, or derived
  cache artifacts.

Only after those prerequisites land should SAM get its own task-specific protobuf and product
roadmap phase.

## Validation Targets

Use targeted validation per phase rather than one giant test sweep every time.

Rust:

```powershell
cd rust/puerh_mind
cargo test
```

C++ build after generated-code or runtime-service changes:

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
```

Targeted CTest group after semantic, storage, or search changes:

```powershell
ctest --test-dir build/debug --output-on-failure -R "AiSidecarRuntimeServiceTest|SemanticGenerationServiceTest|SemanticStorageControllerTest|FilterServiceTest|GlobalSearchDialogQmlTest|SearchQueryClassifierTest"
```

Live runtime tests remain environment-gated and should be run when the required model assets and
provider credentials are available.

## Open Decisions

- Whether image understanding should use thumbnails, previews, or caller-selected image renditions.
- Whether Opencode Go or Opencode Zen is the user's actual account/API surface for Phase 6 live
  validation. The first product-facing presets should target Opencode-compatible endpoints, while
  OpenRouter routing knobs stay optional/legacy.
- For Opencode, which selected model is live-confirmed to accept image input and structured output
  through either the OpenAI-compatible or Anthropic-compatible endpoint. Do not advertise an
  Opencode preset as image-analysis capable until this is proven.
- Where discovered model candidates should be persisted long term: C++ preset settings, Rust
  user-provider config JSON, or a small generated user-provider config file owned by the settings
  flow. Model discovery should feed `config.models[]`, but should not make a model advertised until
  validation succeeds.
- Volcengine Ark's bundled default is `doubao-seed-2-0-lite-260428`.
- Exact live-verified Volcengine Responses output extraction shape and whether the reserved Chat API
  compatibility driver is needed for any target deployment.
- Exact rating rubric: whether score means aesthetic quality, technical quality, keeper priority, or
  a weighted combination.
- Exact first-UI control grouping for description/rating/rating-reason toggles. The backend/provider
  request remains combined per image whenever multiple outputs are selected, while persistence stores
  separate task results.
- Exact user-provider config directory and whether C++ passes it to Rust or Rust resolves it from an
  app-specific config path.
- When to rename C++ classes and the sidecar executable from semantic-oriented names to AI-sidecar
  names.

## Research References

- OpenAI API docs: official SDKs are available for several languages, and direct HTTP clients are a
  supported path when no official SDK fits. Rust should therefore use direct HTTPS for the first
  provider instead of relying on an unofficial broad SDK.
- OpenAI API docs: API keys are bearer secrets and should be loaded from environment variables or a
  key-management service, not hard-coded or exposed client-side.
- OpenAI API docs: Responses API supports image input and structured outputs, which match the
  caption/tag/rating schema requirement.
- OpenAI API docs: the Models API exposes a list endpoint for available model ids; Phase 6c should
  use it for OpenAI-compatible model discovery when the selected endpoint implements that compatible
  surface.
- Anthropic API docs: the Models API exposes `GET /v1/models`; Phase 6c should use the analogous
  compatible endpoint for Anthropic-compatible discovery, but still validate image+tool-use support
  separately.
- Opencode Go docs (`https://opencode.ai/docs/go/`): Opencode Go uses Bearer-authenticated API
  access and exposes compatible model endpoints under `https://opencode.ai/zen/go/v1`, including
  OpenAI-compatible `/chat/completions` and Anthropic-compatible `/messages` paths depending on the
  model.
- Opencode Zen docs (`https://opencode.ai/docs/zen/`): Opencode Zen exposes API endpoints under
  `https://opencode.ai/zen/v1`, including OpenAI-compatible, Responses-compatible, and
  Anthropic-compatible entry points. Treat these as presets over Alcedo's compatible protocol
  drivers, not as a brand-specific driver unless a live smoke proves a protocol deviation.
- OpenRouter docs (`https://openrouter.ai/docs/quickstart`,
  `https://openrouter.ai/docs/guides/features/structured-outputs`): the direct API uses
  `POST /api/v1/chat/completions` with Bearer authentication, OpenAI-compatible request/response
  shapes, optional attribution headers, structured outputs via `response_format: json_schema`, and
  provider routing options such as `require_parameters`.
- OpenRouter model docs (`https://openrouter.ai/qwen/qwen3.7-plus`): Qwen3.7 Plus uses the canonical
  model slug `qwen/qwen3.7-plus`, supports text and image input with text output, and is therefore
  the bundled OpenRouter default for Phase 5 image understanding.
- OpenRouter Go SDK docs (`https://openrouter.ai/docs/client-sdks/go/overview`,
  `https://openrouter.ai/docs/sdks/go/api-reference/chat`): the Go SDK is a type-safe client over
  OpenRouter's chat-completion API, so Phase 5 request/response fixtures should stay compatible with
  the SDK's chat send shape even though Rust calls the REST API directly.
- OpenRouter docs: usage metadata is available on non-streaming responses, making it appropriate for
  Phase 6 job-level usage/cost summaries.
- Volcengine Ark docs (`https://www.volcengine.com/docs/82379/1958521?lang=zh`): the linked
  multimodal-understanding guide is under the Responses API path, supporting
  `volcengine_ark_responses` as the preferred built-in driver for Doubao multimodal calls.
- Volcengine Ark model-list docs (`https://www.volcengine.com/docs/82379/1330310`):
  `doubao-seed-2-0-lite-260428` is a current Doubao Seed 2.0 Lite model with text generation,
  multimodal understanding, and tool-call capabilities, making it the built-in default for the
  China-friendly provider path.
- BytePlus/ModelArk docs (`https://docs.byteplus.com/en/docs/ModelArk/Responses_API`,
  `https://docs.byteplus.com/en/docs/ModelArk/1958523`): Responses API and Structured output
  (Responses API) are documented as first-class APIs, so the Volcengine driver should use
  schema-based output when the selected model supports it rather than best-effort prompt-only JSON.
- LiteLLM docs: mature LLM gateway systems use configuration for model aliases, `api_base`,
  provider-specific params, routing, and secret-manager references while still routing through
  provider-aware code paths. This supports Alcedo's driver-plus-config design instead of a pure
  arbitrary-template design.
- Rust `reqwest` docs: async HTTP client with JSON support, reusable clients, timeout/TLS
  configuration, and explicit TLS backend selection.
- QtKeychain docs: platform-independent Qt secret storage backed by Windows Credential Store, macOS
  Keychain, and Linux desktop keyrings.
- Rust `secrecy` / `zeroize` docs: explicit secret exposure and zeroing help prevent accidental
  logging and reduce in-memory lifetime risk; the current custom `SecretString` can stay if it keeps
  the same audit properties.

## Phase 4 - Completion & Self-Review

Status: complete. Semantic embedding now runs over the shared AI control surface
(`alcedo.ai.AiRequestHeader` / `AiResponseHeader`) via additive v2 RPCs, with v1 kept as an automatic
fallback. v1 RPCs, batching, request-id-to-file-id mapping, model-info validation, embedding
dimensions, model keys, and persistence are all unchanged (Phase 0 contract section 2.3 honored: v2 is
added as new methods, v1 is frozen).

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/proto/semantic.proto` - `import "ai_common.proto";`, 4 v2 RPCs on `SemanticService`
  (`EmbedTextV2` / `EmbedImageV2` / `EmbedTextBatchV2` / `EmbedImageBatchV2`), and 9 v2 messages that
  reference `alcedo.ai.*` fully-qualified. v1 is untouched.
- `rust/puerh_mind/src/server/semantic.rs` - a `build_response_header` helper plus 4 v2 trait methods
  that delegate to the frozen v1 methods and wrap the result with an `AiResponseHeader` (single ->
  `header.model_id`; batch -> per-item `request_id` preserved). v1 methods and v1 tests are unedited. 5
  new v2 tests were added. The cross-package `semantic` -> `alcedo.ai` prost reference resolves with no
  `build.rs` / `proto.rs` change (the existing sibling-module layout emits `super::alcedo::ai::*`).
- `alcedo_studio/src/CMakeLists.txt` - the SemanticProto custom-command `DEPENDS` now includes
  `ai_common.proto`; the SemanticProto `add_library` is ordered after AiProto;
  `target_include_directories` adds the AI generated dir; `target_link_libraries` adds `AiProto`. The
  DAG stays clean (AiProto does not depend on SemanticProto).
- `alcedo_studio/src/include/app/ai_sidecar_runtime_service.hpp` - 4 v2 virtuals on
  `IAiSidecarRuntimeClient` (with out-of-line default impls) and 4 `override` declarations on
  `GrpcAiSidecarRuntimeClient`.
- `alcedo_studio/src/app/ai_sidecar_runtime_service.cpp` - `FillAiRequestHeader` gained a
  backward-compatible `trace_id` parameter (the 3 existing AI-runtime call sites are unchanged);
  `MakeBatchRequestId()`; 2 v2 `ToEmbeddingResult` overloads (single reads `header().request_id()`,
  batch item keeps the per-item `request_id`); 4 `GrpcAiSidecarRuntimeClient::Embed*V2` overrides; and 4
  service wrappers that try-v2-then-fallback. `SemanticEmbeddingResult` is unchanged, so storage and
  search are unchanged.
- `alcedo_studio/tests/app/ai_sidecar_runtime_service_test.cpp` - `FakeAiSidecarRuntimeClient` was
  extended with `v2_supported_` / `SetV2Supported`, 4 v2 overrides canned bit-identical to v1, and call
  counters; 5 new tests were added. Existing tests are unedited.

Fallback policy: only `grpc::UNIMPLEMENTED` triggers v1 fallback (`*v2_available = false`, the service
then calls v1). All other grpc codes (including `DEADLINE_EXCEEDED`) keep v2 (`*v2_available = true`)
with synthesized per-input failures and no v1 retry - the server has v2 but the call failed. Single-call
v2 correlation is `header.request_id`; batch correlation is a fresh `MakeBatchRequestId()` with the
per-item `request_id` preserved end to end.

Credential handling (Phase 3 invariant preserved): Phase 4 embedding has no credentials, so
`FillAiRequestHeader` receives an empty `credential_ref`. Secrets still travel only over gRPC loopback
to the Rust vault, never through process args, `AiSidecarRuntimeOptions`, or logs; `FillAiRequestHeader`
never echoes a secret. v2 calls set `trace_id = request_id` (local correlation; no distributed trace
exists yet). `priority` stays at default and `client_capabilities` is left empty (no `cancel-by-request_id`
advertised).

Deferred to Phase 5 (per Phase 0 section 1.5 and the plan design):

- Cancellation wiring for embedding - not a Phase 4 deliverable; the old C++ embedding has no gRPC-level
  cancel (job-level only, unchanged). `supports_cancel: true` remains a forward promise with no regression.
- Full status-to-tonic structured-error-in-body mapping - v2 hard failures still propagate the v1
  `tonic::Status` (`?`); the `AiResponseHeader` travels on success only, matching the existing
  `ok_header` success-only pattern.

Test results:

- Rust - `cargo test` in `rust/puerh_mind`: 88 passed; 0 failed; 0 ignored. Includes the 5 v2 tests
  (`text_batch_v2_preserves_request_order_and_item_errors`,
  `image_batch_v2_preserves_request_order_and_item_errors`,
  `rejects_non_finite_batch_embedding_as_item_error_v2`,
  `embed_image_v2_routes_through_micro_batch_worker`, `embed_text_v2_echoes_header_request_id`).
- C++ MSVC build (run through the PowerShell tool, per project memory): succeeded after regenerating
  `semantic.pb.h` / `ai_common.pb.h`. This resolved all prior clangd "No type named EmbeddingResponseV2"
  and "redefinition" diagnostics, which were stale generated headers, not real errors.
- ctest targeted group (the validation regex in this plan): 87 tests; 100% passed; 0 failed (1 live
  smoke Skipped - environment-gated, `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` was not set). The 5 new
  `AiSidecarRuntimeServiceTest` tests pass: `EmbedTextV2ReturnsCannedViaV2Path`,
  `EmbedTextV2FallsBackToV1WhenV2Unsupported`, `EmbedImageBatchV2ReturnsCannedViaV2Path`,
  `EmbedImageBatchFallsBackToV1WhenV2Unsupported`, `EmbedImageBatchV2EchoesRequestIds`. The 15 existing
  `AiSidecarRuntimeServiceTest` tests still pass - now exercising the v2 path (the fake v2 overrides
  with `v2_supported_ = true`), which proves v2 canned is bit-identical to v1.
  `SemanticGenerationServiceTest` / `SemanticStorageControllerTest` / `FilterServiceTest` /
  `SearchQueryClassifierTest` / `GlobalSearchDialogQmlTest` are unchanged and green (they use
  `ISemanticImageEmbeddingClient` fakes / storage / search, not the v2 gRPC path).

Build note for the next handoff: the default `all` MSVC build (`--build --preset win_debug`) did not
rebuild `AiSidecarRuntimeServiceTest` after a test-source-only edit - ninja treated the target as
up-to-date, and deleting the executable confirmed `all` does not own it. After editing test sources,
build the target explicitly before ctest:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AiSidecarRuntimeServiceTest --parallel 4`
(or the affected test targets). The plan's verification step 2 should be augmented with this explicit
test-target build.

Review conclusion: none; risk accepted: v2 hard-failure responses propagate the v1 `tonic::Status` (the
`AiResponseHeader` travels on success only) and embedding cancellation is not wired into the
micro-batch worker - both deferred to Phase 5 per Phase 0 section 1.5, and `supports_cancel` stays a
forward promise with no regression; missing tests: none.

## Phase 5a - Completion & Self-Review

Status: complete. Provider selection is data-driven via validated JSON provider configs (built-in
plus an optional user-config directory), with built-ins embedded through `include_str!` so Windows
packaging cannot omit them. Provider configs are DATA ONLY: the validator scans the raw JSON for
secrets before deserializing, rejects raw secret values (`sk-...`, `Bearer ...`, `AKIA...`) and
secret-named keys, enforces HTTPS-only (except localhost dev), known driver / schema version, valid
JSON Pointers, bounded timeout / payload / output-token limits, `stream = false`, and reserved
attribution-header rejection. Invalid user configs fail closed (skipped with a warning, never
offered); an invalid built-in is a hard error. Capability descriptors are derived from loaded configs
so C++ can display remote-provider availability before a task starts. No HTTP is issued in 5a - only
config load, validation, and descriptor advertisement.

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/configs/providers/openrouter.json` - built-in OpenRouter config: `openrouter_chat`
  driver, `https://openrouter.ai/api/v1` + `/chat/completions`, bearer auth slot `openrouter_api_key`,
  `HTTP-Referer` / `X-OpenRouter-Title` attribution headers, `response_format_json_schema` strict,
  JSON Pointer extraction (`/choices/0/message/content`, `/usage`, `/id`), qwen vision+structured
  model, `data_collection = "deny"`.
- `rust/puerh_mind/configs/providers/volcengine_ark.json` - built-in Volcengine Ark config:
  `volcengine_ark_responses` driver, `https://ark.cn-beijing.volces.com/api/v3` + `/responses`, bearer
  auth slot, `responses_json_schema` mode, driver-owned parser (`content_json_pointer = null`),
  doubao-seed-2-0-lite-260428 vision+structured model.
- `rust/puerh_mind/src/service/provider_config.rs` - `ProviderConfig` schema + validation,
  `ProviderRegistry` (upsert-by-provider_id / get / iter), `BUILTIN_PROVIDER_CONFIGS` via
  `include_str!`, `load_provider_configs(user_dir)` (built-ins hard-error, user configs skip+warn, user
  overrides built-in by provider_id, user-on-user duplicates skipped), `scan_for_secrets` (runs before
  deserialize; rejects secret-named keys and leaked-looking values; a `credential_slot` VALUE like
  `openrouter_api_key` is not treated as a secret because it is a value, not a key, and the slot regex
  `^[a-z0-9_]+$` cannot carry a real key), `build_provider_capability_descriptors` (emits one
  `image_understanding.describe` + one `image_rating.score` descriptor per model with
  `supports_vision && supports_structured_output`; `requires_credential` from auth type). 18 module
  tests.
- `rust/puerh_mind/src/service/capabilities.rs` - rewritten
  `build_capability_descriptors(engine, max_payload_bytes, registry, extra)` returns local-semantic +
  config-derived + extra. The old placeholder `provider_id = "remote"` / `model_id = "unconfigured"`
  descriptor is removed. 3 tests.
- `rust/puerh_mind/src/service/mod.rs` - `pub mod provider_config;` (and `pub mod image_analysis;`
  for 5b).
- `rust/puerh_mind/src/config.rs` - `provider_config_dir: Option<String>` field,
  `--provider-config-dir` CLI flag, `ALCEDO_MIND_PROVIDER_CONFIG_DIR` env. 2 tests.
- `rust/puerh_mind/src/main.rs` - loads the provider registry, constructs the mock image-analysis
  provider + its capability (5b), and passes the registry + extra capability + image providers through
  to `start_server`.
- `rust/puerh_mind/src/server/ai_runtime.rs` - `test_impl` loads the registry; two `ai_runtime` tests
  updated to assert config-derived descriptors (count >= 5, an openrouter understanding descriptor with
  `requires_credential` and `model_id != "unconfigured"`).

Data-only / fail-closed invariants (Phase 5a review focus):

- `scan_for_secrets` runs before deserialize on both built-in and user configs, so a secret-shaped
  value or secret-named key is rejected even if the surrounding JSON would otherwise parse and even
  if the field is unknown to `ProviderConfig` (serde silently drops unknown fields). The
  `credential_slot` value is intentionally not treated as a secret: it names a vault slot, never
  holds key material, and the slot regex `^[a-z0-9_]+$` cannot carry a real key (`sk-`, `Bearer `,
  `AKIA...` all fail it).
- Invalid user configs fail closed (skip + warn, not offered) and produce an actionable `ConfigError`
  diagnostic naming the origin file and reason; an invalid built-in is a hard error (the binary is
  broken).
- User configs can add providers or override model defaults, but cannot override credential policy,
  disable schema validation, or enable arbitrary code execution: those are not user-overridable fields,
  and validation always runs.

Deferred to Phase 5c:

- The real HTTP drivers (`OpenRouterChatProvider`, `VolcengineArkResponsesProvider`) - 5a only loads,
  validates, and advertises; no HTTPS call is made.
- Secret persistence across sidecar restarts - the 5a vault is in-memory only (Phase 3 invariant).

Test results:

- Rust - `cargo test` in `rust/puerh_mind`: 129 passed; 0 failed; 0 ignored; 0 warnings. The 18
  `provider_config` tests cover built-in load, user override of a built-in model default, user adds a
  new provider, duplicate user provider id rejected, unknown driver rejected, invalid HTTPS policy
  rejected, http-localhost allowed for dev, raw secret in an `api_key`-named field rejected, leaked
  `Bearer` value rejected, `credential_slot` value not treated as a secret, invalid JSON Pointer
  rejected, invalid JSON Pointer escape rejected, schema-version mismatch rejected, `stream = true`
  rejected, reserved attribution header rejected, out-of-range timeout rejected, built-ins advertise
  understanding + rating descriptors, non-vision / non-structured models not advertised. The 3
  `capabilities` tests + 2 `ai_runtime` tests cover config-derived remote descriptors appended, the
  local semantic descriptor, extra local providers appended, and `ListCapabilities` with / without a
  request header. The 2 `config` tests cover the `--provider-config-dir` override and the None default.
- C++ - no C++ surface changed in 5a (AiProto / CMake are untouched in 5a; `image_analysis.proto` CMake
  generation is 5b). No ctest required for 5a.

Review conclusion: none at phase close; superseded by the 2026-06-25 follow-up (two bugs found and
fixed, see "Phase 5a - Follow-up Review & Fixes" below); risk accepted: the openrouter and volcengine
capability descriptors advertised by 5a are not backed by a registered provider until the 5c drivers
land, so a `DescribeImage` / `ScoreImage` for those provider_ids returns `UNSUPPORTED_TASK` /
`TASK_UNKNOWN` in 5a/5b (only the mock is registered) - this is the intended phase boundary, not a
regression; missing tests: none after the follow-up added 3 regression tests.

### Phase 5a - Follow-up Review & Fixes (2026-06-25)

A follow-up review of the 5a surface found two correctness gaps in `provider_config.rs`; both are
fixed with regression tests.

- P1 (raw-secret scan not applied to user configs): `load_user_configs` deserialized each user config
  with `serde_json::from_value::<ProviderConfig>` without first calling `scan_for_secrets`. Because
  `ProviderConfig` has no `#[serde(deny_unknown_fields)]`, serde silently drops unknown fields, so a
  user JSON carrying `"api_key": "sk-..."` or `"note": "Bearer ..."` was silently accepted -
  contradicting the 5a "raw JSON before deserialize" / "configs are data only" guarantee, which only
  the built-in `parse_and_validate` path actually enforced. Fixed: `load_user_configs` now scans the
  parsed `Value` before `from_value`, skipping + warning on a secret hit (fail closed, not a hard
  error, matching the existing user-config failure policy).
- P2 (user-on-user duplicate of a built-in override): the duplicate guard was
  `registry.get(&id).is_some() && !is_builtin(&id)`. `is_builtin` checks the static built-in id list,
  so for a built-in id like `openrouter` the second clause is always false, and a second user file
  overriding `openrouter` silently clobbered the first (the guard only caught user-on-user dupes for
  non-builtin ids). The existing `duplicate_user_provider_id_is_rejected` test passed only because it
  used the non-builtin id `"dupe"`. Fixed: `load_user_configs` now tracks a `seen_user_provider_ids`
  `HashSet`; the first user config wins and any later user config sharing the id is skipped + warned,
  regardless of whether the id is also a built-in. The now-dead `is_builtin` helper was removed.

Regression tests added (`provider_config`): `user_config_with_raw_secret_in_unknown_field_is_skipped`,
`user_config_with_leaked_bearer_in_unknown_field_is_skipped`,
`duplicate_user_override_of_builtin_does_not_silently_clobber`. `cargo test` in `rust/puerh_mind`:
133 passed; 0 failed; 0 warnings (was 129; +3 here, +1 in 5b). The `is_builtin` removal introduced no
dead-code warning (its only call site was the old guard).

## Phase 5b - Completion & Self-Review

Status: complete. Alcedo's provider-independent result contracts for `image_understanding.describe`
and `image_rating.score` are frozen as typed proto messages plus code-owned JSON Schemas. The two
tasks are distinct contracts - distinct `task_id`s, distinct result message types
(`ImageUnderstandingResult` vs `ImageRatingResult`), distinct RPCs (`DescribeImage` vs `ScoreImage`) -
so a rating result can never overwrite or be reinterpreted as an understanding result (Phase 5b review
focus). Provider-specific raw JSON is never the public contract: the (5c) driver validates and
normalizes provider output against the code-owned schemas and returns these typed fields. The mock
provider returns valid typed results without HTTP. The service owns the control-plane concerns the
provider should not - credential resolution against the vault, request timeout, cooperative
cancellation via the `CancellationRegistry`, and schema validation of the provider's typed result - and
carries outcomes inside the `AiResponseHeader` (status / error_code / redacted error_message). This is
the Phase 5 structured-error-in-body mapping deferred from Phase 4: image analysis has no legacy v1
caller, so the RPC returns `Ok(Response{ header, ... })` with the header carrying the precise status
rather than a plain `tonic::Status`. A genuinely malformed request (empty `image_bytes`) is still a
transport-level `tonic::Status::invalid_argument`, since there is no provider outcome to report.

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/proto/image_analysis.proto` - package `alcedo.ai` (Phase 0 single-package decision),
  `import "ai_common.proto";`, `ImageAnalysisService { DescribeImage, ScoreImage }`; messages
  `RenditionMetadata`, `UsageMetadata`, `ImageUnderstandingResult { caption, tags, scene, confidence }`,
  `ScoredDimension { name, score }`, `ImageRatingResult { scores, rubric_id, rubric_version, reasons,
  confidence }`, `DescribeImageRequest` / `Response`, `ScoreImageRequest` / `Response`. Every
  request / response carries `AiRequestHeader` / `AiResponseHeader`; the response echoes the selected
  rendition, usage, provider request id, and prompt profile id.
- `rust/puerh_mind/build.rs` - `image_analysis.proto` added to `compile_protos` and
  `cargo:rerun-if-changed`.
- `alcedo_studio/src/CMakeLists.txt` - `ALCEDO_AI_IMAGE_ANALYSIS_PROTO` var; `image_analysis.pb.{cc,h}`
  and `image_analysis.grpc.pb.{cc,h}` added to `ALCEDO_AI_PROTO_SRCS` / `_HDRS`; a new custom command
  mirroring `ai_runtime.proto` (imports `ai_common.proto`, emits `.pb` + `.grpc.pb`, `DEPENDS` the
  `ai_common.proto` source). `AiProto.lib` now includes `image_analysis`.
- `rust/puerh_mind/src/service/image_analysis.rs` - domain types (`Usage`, `DescribeOutcome`,
  `ScoreOutcome`, `ScoreDimension`, `ProviderError`); code-owned `IMAGE_UNDERSTANDING_SCHEMA` and
  `IMAGE_RATING_SCHEMA` (JSON Schema draft 2020-12); `validate_understanding` / `validate_rating`
  (reject empty caption, out-of-range confidence, empty tags / scores, empty rubric_id);
  `ImageAnalysisProvider` trait (`#[tonic::async_trait]`: `provider_id`, `requires_credential`,
  `capability`, `describe_image`, `score_image`); `MockImageAnalysisProvider` with canned valid results
  and `MockFailure { None, InvalidOutput, Slow, Error, Transient }`. 7 schema tests.
- `rust/puerh_mind/src/server/image_analysis.rs` - `ImageAnalysisServiceImpl { providers,
  default_provider_id, vault, cancel_registry }`; `resolve_credential` (missing ->
  `UNAUTHENTICATED` / `MISSING_CREDENTIAL`; `NotFound` / `Revoked` -> `PERMISSION_DENIED` /
  `CREDENTIAL_REVOKED`; `Expired` -> `PERMISSION_DENIED` / `CREDENTIAL_EXPIRED`); `timeout_duration`;
  `failure_header` / `success_header` (return `Option<AiResponseHeader>`, redact via
  `vault.redact_error_message`); `provider_error_to_header` (`SchemaValidation` -> `PROVIDER_ERROR` /
  `PAYLOAD_DECODE`; `Transient` -> `PROVIDER_UNAVAILABLE` / `PROVIDER_5XX`; `Provider` ->
  `PROVIDER_ERROR` / `INTERNAL`, dropping the inner provider string); `describe_image` / `score_image`
  via `tokio::select! { biased; cancel_rx; timeout(provider_fut) }`. Empty `image_bytes` ->
  `tonic::Status::invalid_argument`. 12 service tests (all 5 plan-required plus cancellation,
  valid-credential, unknown-provider, empty-bytes, provider-error, transient-error, mock-capability).
- `rust/puerh_mind/src/server/mod.rs` - `pub mod image_analysis;`.
- `rust/puerh_mind/src/service/registry.rs` - `register_services` takes `image_providers` +
  `default_image_provider_id`; `ImageAnalysisServiceImpl` shares `vault.clone()` /
  `cancel_registry.clone()` with the AI runtime service; `ImageAnalysisServiceServer` added to the
  router.
- `rust/puerh_mind/src/bootstrap.rs` - `start_server` passes through `image_providers` +
  `default_image_provider_id`.
- `rust/puerh_mind/src/main.rs` - the mock image-analysis provider is registered as the default and its
  no-credential capability is passed as `extra` to `build_capability_descriptors`.

Credential handling (Phase 3 invariant preserved): secrets travel only over gRPC loopback to the Rust
vault; the service resolves `credential_ref` against the vault and never sees the secret material.
`error_message` is redacted via `vault.redact_error_message` before placement; the `Provider(String)`
inner text is dropped by `provider_error_to_header` (only a fixed string is placed), so provider text is
not leaked in the header. No image bytes, base64, or prompt payloads are placed in headers. The service
never writes to DuckDB - C++ owns DB writes (relevant to 5f, not 5b).

Distinct-contract / fail-closed invariants (Phase 5b review focus):

- `DescribeImage` returns `ImageUnderstandingResult` (caption / tags / scene); `ScoreImage` returns
  `ImageRatingResult` (scores / rubric_id / rubric_version / reasons). Distinct `task_id`s on the
  request header; the response header echoes the same `task_id`. A rating result cannot overwrite an
  understanding result and vice versa.
- Provider-specific raw JSON is never the public contract: the code-owned JSON Schemas and the proto
  typed fields are the contract; the (5c) driver validates + normalizes provider output against them.
- A schema-validation failure returns a typed error header with `result = None` (no active annotation) -
  `describe_image_schema_validation_failure_returns_provider_error` proves this.

Deferred to Phase 5c:

- The real HTTP drivers (`OpenRouterChatProvider`, `VolcengineArkResponsesProvider`) - 5b ships only the
  mock. The openrouter / volcengine descriptors advertised by 5a are not backed by a registered
  provider in 5b; a request for those `provider_id`s returns `UNSUPPORTED_TASK` / `TASK_UNKNOWN` until
  5c.
- Real 5xx / rate-limit detection - the `Transient` path is exercised by a mock in 5b; real detection
  lives in the 5c HTTP drivers.
- Secret persistence across sidecar restarts (5c / 5d).

Test results:

- Rust - `cargo test` in `rust/puerh_mind`: 129 passed; 0 failed; 0 ignored; 0 warnings. Includes the 5
  plan-required 5b service tests (`describe_image_returns_valid_understanding`,
  `score_image_returns_valid_rating`, `describe_image_missing_credential_returns_unauthenticated`,
  `describe_image_timeout_returns_deadline_exceeded`,
  `describe_image_schema_validation_failure_returns_provider_error`) plus
  `describe_image_cancellation_returns_cancelled`, `describe_image_with_valid_credential_succeeds`,
  `describe_image_unknown_provider_returns_unsupported_task`,
  `describe_image_empty_bytes_is_transport_error`,
  `describe_image_provider_error_returns_provider_error`,
  `describe_image_transient_error_returns_provider_unavailable`,
  `mock_capability_advertises_no_credential_understanding`, and the 7 schema tests. All three
  `provider_error_to_header` arms (SchemaValidation, Provider, Transient) are exercised, so the service
  has no dead error-mapping code.
- C++ MSVC build (run through the PowerShell tool, per project memory): the `AiProto` target built -
  `image_analysis.pb.{cc,h}` and `image_analysis.grpc.pb.{cc,h}` were generated into `generated/ai/` and
  `AiProto.lib` (10 MB) linked. `AiSidecarRuntimeServiceTest` built (exit code 0), confirming the AiProto
  change (adding `image_analysis` to the library) is regression-free downstream - this target
  transitively depends on AiProto via `AppDiagnostics` -> `SemanticProto` -> `AiProto`. The C++
  generated-proto build coverage requirement is met.

Build note for the next handoff: `--target AiProto` regenerates the `image_analysis` stubs and links
`AiProto.lib` but does not rebuild downstream C++ test targets whose own sources are unchanged - after
any 5c change that touches `AiProto`, build a downstream target explicitly (e.g.
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AiSidecarRuntimeServiceTest --parallel 4`)
before ctest, mirroring the Phase 4 note. `image_analysis.grpc.pb.h` includes `ai_common.pb.h`, so
AiProto's existing PUBLIC include dir is sufficient for future 5d C++ consumers - no new include dirs.

Review conclusion: none at phase close; superseded by the 2026-06-25 follow-up (one bug found and
fixed, see "Phase 5b - Follow-up Review & Fixes" below); risk accepted: the openrouter / volcengine
capability descriptors advertised by 5a are not backed by a registered provider in 5b (only the mock
is registered), so a `DescribeImage` / `ScoreImage` for those `provider_id`s returns
`UNSUPPORTED_TASK` / `TASK_UNKNOWN` until the 5c drivers land - this is the intended phase boundary,
not a regression; missing tests: none after the follow-up added 1 regression test and renamed 1 for
honesty.

### Phase 5b - Follow-up Review & Fixes (2026-06-25)

A follow-up review of the 5b surface found one correctness gap in `image_analysis.rs`; fixed with a
regression test.

- P2 (empty `tags` list not rejected): `validate_understanding` only checked
  `out.tags.iter().any(|t| t.trim().is_empty())`, which is false for an empty list, so `tags: []`
  slipped through - contradicting the 5b "reject empty tags / scores" promise. `validate_rating`
  already rejected `scores: []` (it checks `out.scores.is_empty()` and `IMAGE_RATING_SCHEMA` has
  `minItems: 1`), so the gap was understanding-only. The existing
  `validator_rejects_empty_tags_or_scores` test only exercised `vec![""]` (a blank-string tag) for
  understanding and `vec![]` for rating, so the empty-tags-list case for understanding was uncovered.
  Fixed: `validate_understanding` now rejects `out.tags.is_empty()`, and `IMAGE_UNDERSTANDING_SCHEMA`
  gained `minItems: 1` on `tags` plus `minLength: 1` on tag items, matching the rating schema's
  `scores` handling.

Regression tests: added `validator_rejects_empty_tags_list`; renamed
`validator_rejects_empty_tags_or_scores` -> `validator_rejects_blank_tag_string_and_empty_scores_list`
so the name describes what it actually covers (a blank-string tag for understanding, an empty scores
list for rating) rather than implying the empty-tags-list case it did not cover. The schema test now
asserts `tags.minItems == 1` and `tags.items.minLength == 1`. `cargo test` in `rust/puerh_mind`:
133 passed; 0 failed; 0 warnings (was 129; +1 here, +3 in 5a).

## Phase 5c - Completion & Self-Review

Status: complete. The first real remote image-analysis providers are wired behind the two shipped
driver ids. `OpenRouterChatProvider` (`openrouter_chat`) builds OpenAI Chat-compatible
`POST /api/v1/chat/completions` requests; `VolcengineArkResponsesProvider` (`volcengine_ark_responses`)
builds OpenAI Responses-compatible `POST /api/v3/responses` requests against the Ark data-plane base
URL. Both reuse a shared rustls HTTPS client, image->data-URI encoding, bounded retry, strict-schema
injection, and redaction discipline (`http_util`), so the driver files own only the per-family
request/response shape and the typed parser - not the cross-cutting policy. The credential secret is
resolved per request from the Rust vault by the service and passed to the provider as
`Option<&SecretString>`; the provider calls `expose()` exactly once, at the `Authorization: Bearer`
header call site. No secret, image base64, prompt, or raw provider body travels through args, options,
logs, or error strings. The mock provider remains the default; the real providers are merged into the
provider map at startup and only selected when a request names their `provider_id`.

Contract change (threading the credential): `ImageAnalysisProvider::describe_image` / `score_image`
now take `credential: Option<&SecretString>`. The service (`server/image_analysis.rs`) gained
`resolve_credential_secret` - `Ok(None)` when the provider does not require a credential,
`Ok(Some(secret))` when the handle resolves, or a failure-header triple on `MISSING_CREDENTIAL` /
`CREDENTIAL_REVOKED` / `CREDENTIAL_EXPIRED` - and passes `credential.as_ref()` into the provider call.
`MockImageAnalysisProvider` was updated to the new signature. This keeps the secret out of
`AiSidecarRuntimeOptions` / `BuildArguments` (Phase 3 invariant preserved) and localizes `expose()`
to the driver.

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/Cargo.toml` - `reqwest = { 0.12, default-features = false, features =
  ["rustls-tls-webpki-roots", "json"] }` (no native-tls, no invalid-cert acceptance, no plaintext-for-
  production), `rustls = { 0.23, default-features = false, features = ["ring", "std", "tls12",
  "logging"] }`, `base64 = "0.22"`; dev-deps `wiremock = "0.6"` (mock HTTP server) and `dotenvy = "0.15"`
  (loads `.env.test` for the live smokes). The `ring` crypto provider is installed at client
  construction, reusing ort's existing tls-rustls native build rather than introducing aws-lc-rs.
- `rust/puerh_mind/src/service/providers/mod.rs` - new module; `build_real_image_providers(&
  ProviderRegistry) -> HashMap<String, Arc<dyn ImageAnalysisProvider>>` constructs only the shipped
  driver families from the loaded registry and skips a config naming a reserved-but-unimplemented driver
  (`openai_responses`, `anthropic_messages`, `volcengine_ark_chat`) with a `warn!` - fail closed, so a
  request for that `provider_id` returns `UNSUPPORTED_TASK` in-header. `build_one` maps a construction
  failure to a skipped provider (also fail-closed).
- `rust/puerh_mind/src/service/providers/http_util.rs` - shared helpers: `build_rustls_client`
  (installs `ring`, `use_rustls_tls().build()`); `build_image_data_uri` (magic-byte detect
  PNG/JPEG/WebP/GIF pass-through, else image-crate re-encode to PNG); `strict_schema_value` +
  `sanitize_strict` (drops unsupported keys $schema/title/minLength/minItems/minimum/maximum/pattern/
  format, forces `required` = all properties recursively, keeps additionalProperties:false - strict-
  mode compatible; the code-owned validator still enforces the dropped constraints, so fail-closed is
  preserved); `send_with_retry` (bounded retry: retryable = 429 | 5xx | transport error; max 1 retry /
  2 attempts; 4xx non-429 never retried; Retry-After respected and capped at 5s; body drained with
  `let _ = resp.text().await` and never logged); `parse_content_json`, `extract_usage` (tolerant
  input_tokens/prompt_tokens + output_tokens/completion_tokens), `json_pointer_str`,
  `transport_error_category`, `retry_after`. Constants `MAX_TRANSIENT_RETRIES=1`, `RETRY_BACKOFF=100ms`,
  `MAX_RETRY_AFTER=5s`.
- `rust/puerh_mind/src/service/providers/openrouter.rs` - `OpenRouterChatProvider` (`new`,
  `with_client`, `url`, `resolve_model`, `ensure_structured_output`, `bearer` (expose() only here),
  `attribution_headers` (HTTP-Referer, X-OpenRouter-Title), `provider_knobs` (require_parameters +
  data_collection=deny), `build_chat_body`, `parse_describe`, `parse_score`); schema names
  `alcedo_image_understanding` / `alcedo_image_rating`; `build_chat_body` emits `{model, messages:[
  {role:system},{role:user,content:[{type:text},{type:image_url,image_url:{url:data_uri}}]}],
  stream:false, temperature, max_tokens, response_format:{type:json_schema,json_schema:{name,strict,
  schema}}}` plus a conditional `provider` object (omitted when empty). 13 mock-server tests.
- `rust/puerh_mind/src/service/providers/volcengine_ark.rs` - `VolcengineArkResponsesProvider`,
  structurally parallel but with the Responses shape; `build_responses_body` emits `{model, input:[
  {role:system,content:[{type:input_text}]},{role:user,content:[{type:input_image,image_url:data_uri},
  {type:input_text}]}], stream:false, temperature, max_output_tokens, text:{format:{type:json_schema,
  name,strict,schema}}}` (no `provider` object, flat `image_url` string); `extract_output_text` is the
  driver-owned typed parser walking `output[].content[]` for the first `output_text` item (config
  `content_json_pointer` is null for this driver - the parser owns the shape). 13 mock-server tests.
- `rust/puerh_mind/src/service/providers/live_smoke.rs` - env-gated live smokes
  `live_openrouter_smoke_describe_and_score` (keys `ALCEDO_OPENROUTER_API_KEY` / `OPENROUTER_API_KEY`)
  and `live_volcengine_ark_smoke_describe_and_score` (keys `ALCEDO_VOLCENGINE_ARK_API_KEY` /
  `ALCEDO_ARK_API_KEY`); `env_or_skip` returns the first non-empty env var or skips with an eprintln;
  `smoke_image_png` builds a 32x32 RGB gradient PNG; `dotenvy::from_filename(".env.test")` loads the
  gitignored file (existing process env takes precedence); each smoke constructs the provider from the
  real built-in config (no base_url override), calls describe + score, asserts `validate_understanding`
  / `validate_rating`, and eprintlns usage + provider request id.
- `rust/puerh_mind/src/service/image_analysis.rs` - the `ImageAnalysisProvider` trait and
  `MockImageAnalysisProvider` updated to the `Option<&SecretString>` credential signature.
- `rust/puerh_mind/src/server/image_analysis.rs` - `resolve_credential_secret` resolves the vault
  handle to a `SecretString` (or a failure-header triple) and passes `credential.as_ref()` into
  `describe_image` / `score_image`.
- `rust/puerh_mind/src/service/mod.rs` - `pub mod providers;`.
- `rust/puerh_mind/src/main.rs` - `build_real_image_providers(&provider_registry)` merged into the
  image provider map; the mock stays the default.
- `rust/puerh_mind/.gitignore` - `.env.test` ignored (the `.env.test.example` template stays tracked).
- `rust/puerh_mind/.env.test.example` - committed template documenting the four expected key names with
  empty values.
- `rust/puerh_mind/.env.test` - gitignored, empty values, for the user to fill and hand back; loaded by
  `dotenvy` only for the live smokes.

Driver-shape / fail-closed / bounded-retry invariants (Phase 5c review focus):

- Compatibility is implemented by typed drivers, not arbitrary JSON templates. Each provider is a
  struct with typed methods that build the request from the loaded `ProviderConfig` and parse the
  response with a driver-owned typed parser (`parse_describe` / `parse_score` for OpenRouter;
  `extract_output_text` + `parse_describe` / `parse_score` for Volcengine).
  `request_body_has_structured_output_and_require_parameters` and
  `request_body_uses_responses_shape_with_structured_output` pin the exact wire shape (Chat
  `choices[0].message.content` + nested `image_url.url`; Responses `output[].content[].output_text` +
  flat `image_url` string) so a template drift is caught.
- A provider schema failure cannot create an active annotation. `schema_failure_does_not_produce_active
  _result` (both drivers) sends a malformed-but-200 response; the driver returns
  `ProviderError::SchemaValidation`, the service maps it to `PROVIDER_ERROR` / `PAYLOAD_DECODE` with
  `result = None`. `non_json_content_maps_to_schema_validation` (OpenRouter) and
  `missing_output_text_maps_to_schema_validation` (Volcengine) cover the non-JSON / missing-output-text
  paths to the same fail-closed outcome.
- The Volcengine response parser is backed by a live smoke fixture -
  `live_volcengine_ark_smoke_describe_and_score` asserts `validate_understanding` / `validate_rating` on
  the real Ark response and is the ground-truth check that the documented Responses shape matches the
  live provider. The fixture is wired and skip-gated; see "Test results" for its execution status.
- Retries are bounded and not aggressive on paid non-idempotent calls. `send_with_retry` retries at
  most once (2 attempts) on 429 | 5xx | transport error, never on 4xx non-429, with a 100ms backoff and
  Retry-After respected and capped at 5s. `rate_limit_maps_to_transient` and
  `server_500_is_retried_then_succeeds` (both drivers) pin the policy;
  `client_4xx_is_not_retried_and_maps_to_provider_error` (OpenRouter) pins the no-retry-on-4xx side.

Negative / redaction invariants:

- `no_secret_image_prompt_or_body_in_logs_or_error_strings` (both drivers) mounts a 500-then-400-
  with-sentinel sequence, captures tracing output on a `current_thread` runtime under a thread-local
  capturing subscriber, and asserts the captured logs and the returned error string contain neither the
  API key, the `data:image/png;base64,` prefix, the prompt text, nor the raw provider body sentinel. A
  positive `captured.contains("retrying")` assertion proves the capture is real (not an empty-buffer
  false pass); the test was rewritten from a `multi_thread` + `block_in_place` form that could poll the
  future's continuation on a worker thread where the thread-local subscriber is not set, leaving
  `captured` empty and the no-leak assertions passing trivially. `bearer` calls `SecretString::expose()`
  exactly once, at the `Authorization` header call site; error strings use fixed messages
  (`"provider returned HTTP {code}"`), never the body.

Test results:

- Rust - `cargo test` in `rust/puerh_mind`: 171 passed; 0 failed; 0 ignored (was 133 after the 5b
  follow-up; +38 here: 10 `http_util`, 13 `openrouter`, 13 `volcengine_ark`, 2 `live_smoke`). The plan-
  required OpenRouter mock-server tests (`sends_bearer_authorization_and_attribution_headers`,
  `request_body_has_structured_output_and_require_parameters`,
  `parses_understanding_response_and_captures_usage`, `parses_rating_response_and_captures_usage`,
  `rate_limit_maps_to_transient`, `server_500_is_retried_then_succeeds`,
  `cancellation_drops_in_flight_request`, `timeout_returns_deadline_exceeded`,
  `schema_failure_does_not_produce_active_result`, `non_json_content_maps_to_schema_validation`,
  `bearer_required_without_credential_errors`, `no_secret_image_prompt_or_body_in_logs_or_error_strings`,
  `client_4xx_is_not_retried_and_maps_to_provider_error`) and the Volcengine equivalents
  (`sends_bearer_authorization`, `request_body_uses_responses_shape_with_structured_output`,
  `extracts_output_text_from_responses_envelope`, `parses_rating_response_and_captures_usage`,
  `ark_error_body_maps_to_provider_error_without_leaking_text`,
  `missing_output_text_maps_to_schema_validation`, `schema_failure_does_not_produce_active_result`,
  `rate_limit_maps_to_transient`, `server_500_is_retried_then_succeeds`,
  `cancellation_drops_in_flight_request`, `timeout_returns_deadline_exceeded`,
  `bearer_required_without_credential_errors`,
  `no_secret_image_prompt_or_body_in_logs_or_error_strings`) are all green.
  `cargo check --all-targets` exits 0 with only the 5 pre-existing test-API warnings carried from
  earlier phases (`ProviderRegistry::get`, `MockFailure` variants, `with_requires_credential` /
  `with_failure`, `EmbedImageItemV2` / `EmbedTextItemV2`) - no new warnings from 5c.
- Live smokes - both `live_openrouter_smoke_describe_and_score` and
  `live_volcengine_ark_smoke_describe_and_score` SKIP cleanly (printed skip line, counted as `ok`)
  because `.env.test` ships with empty values. They have NOT been executed against the real provider APIs
  yet - pending the user-supplied credentials. This is the explicit handoff: the user fills `.env.test`
  and hands it back; the smokes then run against the real endpoints and assert the parsed outcome
  validates against the code-owned contract.

Deferred to Phase 5d / 5e / 5f / 5g:

- Secret persistence across sidecar restarts - the vault is in-memory; a registered credential does not
  survive a sidecar restart. Persisting user credentials encrypted at rest remains a Phase 6 product
  wiring concern.
- The C++ host image-analysis service and runtime client (5d), local prefill queue before persistence
  (5e), storage/search integration (5f), and developer smoke (5g).
- `volcengine_ark_chat` remains a reserved compatibility driver (not wired) unless live testing proves
  the default Doubao path needs the Chat API instead of the Responses API; the bundled config defaults to
  `doubao-seed-2-0-lite-260428` on the Responses path.

Review conclusion: none (no shipped-code bugs found in review - the compile errors fixed during
implementation, including a `resp` use-after-move in `send_with_retry`, an `Option<&str>` comparison in
`sanitize_strict`, and `is_body_decode` -> `is_decode`, were caught at compile time before any test ran;
the one review finding was a test-honesty gap, not a shipped bug: the no-leak log-capture tests were
rewritten from a `multi_thread` + `block_in_place` form that could false-pass on an empty captured
buffer to a `current_thread` runtime with a positive `captured.contains("retrying")` assertion proving
capture is real); risk accepted: (1) a single bounded retry (max 1, 100ms backoff, Retry-After
respected and capped at 5s, only on 429 / 5xx / transport, never 4xx) on paid non-idempotent `POST
describe/score` calls - a retried POST could double-charge if the first request succeeded server-side
but its response was lost, and the bound is the minimum useful retry, not aggressive; (2) the
advertised-but-unregistered-provider risk carried from 5a/5b - a user-supplied config naming a reserved
driver id (`openai_responses`, `anthropic_messages`, `volcengine_ark_chat`) is skipped with `warn!`
(fail closed), so a request for that `provider_id` returns `UNSUPPORTED_TASK`; the shipped built-ins
(`openrouter`, `volcengine_ark`) are wired, so advertisement and registration align; missing tests: the
env-gated live smokes (OpenRouter + Volcengine) are implemented and skip cleanly without credentials but
have not yet been executed against the real provider APIs - pending the user-supplied credentials in the
gitignored `.env.test`; the mock-server suite (38 tests, no network, no cost) is the CI gate and is
green. Per the Phase 5 review focus, the Volcengine parser's live-smoke backing is wired but unexecuted;
running it is the explicit next step once `.env.test` is handed back.

### Phase 5c - Follow-up Review & Fixes (2026-06-25): Anthropic Messages driver for the Volcengine Ark Coding Plan

Triggered by the user's request to talk to the Volcengine Ark **Coding Plan** endpoint, which exposes an
Anthropic-compatible API (`https://ark.cn-beijing.volces.com/api/coding` -> `POST /v1/messages`). The user
chose the **Anthropic Messages** protocol, **vision `DescribeImage`/`ScoreImage`** when a vision-capable
model is selected, default model **`doubao-seed-2.0-lite`**. This fills the reserved `anthropic_messages`
driver slot declared in `KNOWN_DRIVER_IDS` since 5a; no config-schema changes were required
(`driver: "anthropic_messages"`, `auth.type: "bearer"`/`"api_key_header"`, `structured_output.mode: "tool"`,
`content_json_pointer: null` were all already in the known-ID closed sets).

What shipped:

- New driver `rust/puerh_mind/src/service/providers/anthropic_messages.rs` (`AnthropicMessagesProvider`),
  mirroring `volcengine_ark.rs` with these wire-shape deltas: the request body is the Anthropic Messages
  shape (`model`, mandatory `max_tokens`, top-level `system`, `messages[user]` with an `image` block
  `{type:"image", source:{type:"base64", media_type, data}}` carrying RAW base64 — not a data URI — plus a
  `text` instruction, `temperature`, `stream:false`), structured output via **tool-use**
  (`tools[{name, description, input_schema}]` + `tool_choice:{type:"tool", name}`) with the code-owned
  Alcedo schema run through `strict_schema_value` as `input_schema`, and a driver-owned `tool_use` walker
  (`extract_tool_use_input` finds `content[].tool_use` by expected name and returns its `input` object;
  missing or wrong-name -> `SchemaValidation`, fail-closed). Auth is selected by `config.auth.auth_type`:
  `bearer` -> `Authorization: Bearer <secret>` (Claude-Code-style, the Coding Plan default), `api_key_header`
  -> `x-api-key: <secret>` (real Anthropic API convention), `none` -> no credential; `anthropic-version:
  2023-06-01` is always sent. `SecretString::expose()` is called only at the header-build site; the
  `api_key_header` value is a short-lived `String` clone dropped after the call. The `describe_prompt` /
  `score_prompt` free fns are copied verbatim from `volcengine_ark.rs` (per-driver, not shared). The module
  doc records the Coding Plan ToS + vision caveats.
- New built-in config `rust/puerh_mind/configs/providers/volcengine_ark_coding.json` (embedded via
  `include_str!` in `BUILTIN_PROVIDER_CONFIGS`): `provider_id volcengine_ark_coding`, `driver
  anthropic_messages`, `base_url https://ark.cn-beijing.volces.com/api/coding`, `endpoint /v1/messages`,
  `auth.type bearer` reusing the `volcengine_ark_api_key` slot (the slot is a label; the vault resolves by
  opaque handle, so one registered Ark key serves both `volcengine_ark` and `volcengine_ark_coding`),
  `defaults.model doubao-seed-2.0-lite`, `structured_output.mode tool` strict, `content_json_pointer null`,
  one model `doubao-seed-2.0-lite` (`supports_vision true`, `supports_structured_output true`). Passes
  `scan_for_secrets` (same shape as `volcengine_ark.json` which loads today; `credential_slot` is exempt
  from the `_key`-suffix rejection because it is a label, not a secret).
- `http_util.rs`: extracted the image-encode core into private `detect_image_base64` and added
  `pub fn build_image_base64(bytes) -> (media_type, base64)` so the Anthropic driver can emit raw base64 +
  media type; `build_image_data_uri` now delegates to the same core (DRY, existing behavior preserved). The
  old `data_uri_uses_standard_base64` test was replaced 1:1 by `build_image_base64_returns_mime_and_base64`
  (still asserts STANDARD base64), so `http_util` stays at 10 tests.
- `providers/mod.rs`: `pub mod anthropic_messages;` + an `"anthropic_messages"` arm in `build_one`; the doc
  comment now lists `anthropic_messages` as wired (removed from the reserved list) and `volcengine_ark_coding`
  among the shipped built-ins.
- `live_smoke.rs`: `live_volcengine_ark_coding_smoke_describe_and_score` mirrors the Ark smoke and reuses
  the same `ALCEDO_VOLCENGINE_ARK_API_KEY` / `ALCEDO_ARK_API_KEY` keys (no new env var); the module doc +
  `.env.test.example` note that the Coding Plan smoke reuses the Ark key.

Two existing `provider_config` count-assertion tests were updated for the 3rd built-in (these were the only
test breakages from adding the config — both are count tests, not logic):
`loads_built_in_configs` 2 -> 3 built-ins (and now explicitly asserts the coding config's driver / base_url /
endpoint / auth.type / credential_slot / default model / `structured_output.mode tool` / strict /
`content_json_pointer null`); `built_ins_advertise_understanding_and_rating_descriptors` 4 -> 6 capability
descriptors (3 providers x 1 model x 2), understanding 2 -> 3, rating 2 -> 3.

Test results:

- Rust - `cargo check --all-targets` exits 0 with only the 5 pre-existing test-API warnings carried from
  earlier phases (`ProviderRegistry::get`, `MockFailure` variants, `with_requires_credential` /
  `with_failure`, `CredentialVault::revoke`, `EmbedImageItemV2` / `EmbedTextItemV2`) - no new warnings.
- Rust - `cargo test -- --skip live_` (mock suite, no network, no cost): 185 passed; 0 failed; 0 ignored; 3
  filtered (the 3 live smokes). vs 169 mock tests at 5c close (+16 `anthropic_messages` driver tests; the
  `http_util` image-encode test was a 1:1 replacement, not a net add). The 16 new driver tests:
  `sends_authorization_and_anthropic_version_headers`, `request_body_uses_messages_shape_with_tool_use`,
  `extracts_tool_use_input_from_messages_envelope`, `parses_understanding_response_and_captures_usage`,
  `parses_rating_response_and_captures_usage`, `rate_limit_maps_to_transient`, `server_500_is_retried_then_succeeds`,
  `client_4xx_is_not_retried_and_maps_to_provider_error`, `schema_failure_does_not_produce_active_result`,
  `missing_tool_use_maps_to_schema_validation`, `wrong_tool_name_maps_to_schema_validation`,
  `bearer_required_without_credential_errors`, `api_key_header_mode_sends_x_api_key`,
  `no_secret_image_prompt_or_body_in_logs_or_error_strings`, `cancellation_drops_in_flight_request`,
  `timeout_returns_deadline_exceeded`. Full binary total 188 (was 171; +16 driver + 1 coding live smoke + 0
  net http_util + the 2 count-test fixes). The no-leak test uses the same `current_thread` runtime +
  positive `captured.contains("retrying")` pattern proven in 5c (a `multi_thread` runtime false-passes on an
  empty buffer); it computes the image base64 via `build_image_base64` and asserts it is absent from logs.
- Live smokes - `live_volcengine_ark_coding_smoke_describe_and_score` SKIPs cleanly without a key (printed
  skip line, counted `ok`). It has NOT been executed against the real Coding Plan endpoint yet - see the
  handoff / blocker note below.

Live Coding Plan smoke result (executed 2026-06-25, PASSED):

- After the env-file fix below, `cargo test live_volcengine_ark_coding -- --nocapture` ran against the real
  Coding Plan endpoint and PASSED - both `describe_image` and `score_image` succeeded and the parsed
  outcomes validated against the code-owned Alcedo contract. Sample describe output: caption "A smooth
  diagonal gradient transitioning from dark green in the bottom-left to bright pink in the top-right.",
  tags ["gradient","abstract","green","pink","background","smooth","color transition"], scene "abstract
  gradient background", confidence 0.98, usage {input_tokens 1901, output_tokens 130}; sample score: 2
  dims, rubric alcedo-default-v1, usage {input 1978, output 169}. This confirms three things that were
  previously "unverified" accepted risks: (a) `auth.type: bearer` is accepted by the Coding Plan (no 401 -
  no flip to `api_key_header` needed); (b) the `doubao-seed-2.0-lite` slug is valid on the Coding Plan and
  IS vision-capable (it accepted and described the image - no 404, no image rejection); (c) the documented
  Anthropic Messages wire shape (tool-use structured output, raw-base64 image block, `anthropic-version`
  header) matches the Coding Plan proxy. Only the Coding Plan usage-policy caveat remains
  (operator-policy, not technical): using the Coding Plan as an OpenClaw-style coding-tool backend is in
  the intended usage class, while routing non-coding production image analysis through it remains an
  operator decision. Production Alcedo image analysis stays on `volcengine_ark`.
- Env-file fix: the user moved the real keys into the gitignored `.env.test` (where dotenvy reads them)
  and emptied `.env.test.example` back to a true template, resolving the hygiene finding below. (The
  earlier attempt to copy the real-key file into `.env.test` was blocked by the Claude Code auto-classifier;
  the user did the copy themselves.)

Finding (env-file hygiene, not shipped code; RESOLVED 2026-06-25 by the user): `rust/puerh_mind/.env.test.example`
is UNTRACKED and NOT gitignored (only `.env.test` is gitignored, per `rust/puerh_mind/.gitignore:35`), and the
`.gitignore` comment (lines 31-33) describes it as the tracked *empty-value* template. It briefly held
real-looking API keys (a `git add .` would have committed them); the user has since emptied it back to a
true template and moved the real keys into the gitignored `.env.test`. No leak occurred (the file was
untracked throughout). Optional follow-up: `git add` the empty `.env.test.example` so it is a tracked
template matching the `.gitignore` comment's intent (it is currently untracked, so other developers do not
get it automatically).

Review conclusion: none (no shipped-code bugs found - the two test failures from adding the built-in
(`loads_built_in_configs` 2->3, `built_ins_advertise_understanding_and_rating_descriptors` 4->6) were
expected count-assertion updates, not logic bugs, and are fixed; the driver compiles clean with no new
warnings and all 16 mock tests pass on the first run; the env-gated live Coding Plan smoke was executed
against the real Coding Plan endpoint on 2026-06-25 and PASSED - both describe and score returned valid
outcomes that validate against the code-owned contract); risk accepted: (1) the Coding Plan usage-policy
caveat - `/api/coding/*` is intended for AI coding tools, so using this driver as an OpenClaw-style
coding-tool backend is low-risk and aligned with the plan's purpose; the remaining caution is routing
non-coding PRODUCTION Alcedo image analysis through `volcengine_ark_coding`, which stays an operator
decision. Mitigation: production Alcedo image analysis stays on `volcengine_ark` (`/api/v3/responses`);
the coding endpoint remains available for coding-tool validation and explicit operator-approved use,
documented in the driver module doc + here; (2) the env-file
hygiene finding (`.env.test.example` untracked + not gitignored) - resolved by the user (real keys moved to
gitignored `.env.test`, `.env.test.example` emptied); optional follow-up to `git add` the empty template;
risks previously listed as "unverified" (auth-header style, model/vision) are now CONFIRMED by the live
smoke - `auth.type: bearer` is accepted (no 401), `doubao-seed-2.0-lite` is a valid Coding Plan slug and is
vision-capable (it described the image); missing tests: none - the live Coding Plan smoke is implemented,
executed, and passing; the mock-server suite (16 driver tests + the replaced `http_util` test, no network,
no cost) is the CI gate and is green.

## Phase 5d - Completion & Self-Review

Status: complete. The C++ host can now drive the Rust image-analysis sidecar end to end
over typed RPCs while keeping C++ ownership of image rendition, project state, and
persistence. `IAiSidecarRuntimeClient` / `GrpcAiSidecarRuntimeClient` gained typed
`DescribeImage` / `ScoreImage` RPCs (proto `alcedo.ai.ImageAnalysisService`) with inline
`AiRequestHeader` / `AiResponseHeader`; `AiSidecarRuntimeService` exposes ready-guarded
host wrappers. A new host `ImageAnalysisService` owns the k1024 thumbnail materialization
→ OIIO JPEG encode → credential registration → serialized typed RPC → structured-DTO
return flow, with an injectable service-wide in-flight gate (max one remote analysis at a
time) and cooperative + server-side cancellation. A new `image_analysis_encoder` provides
the encoded-rendition path (OpenImageIO primary codec, NOT OpenCV imgcodecs), kept
strictly separate from the raw RGBA8 CLIP embedding path. No database writes occur in 5d
(the local prefill queue is 5e; database writes are 5f); no product UI / controller wiring (6).
All pre-execution decisions (k1024 rendition,
encoded JPEG bytes q90, OIIO primary, concurrency=1, encoded `image_format_hint`) are
honored.

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/proto/image_analysis.proto` - comment-only edit: `DescribeImageRequest.
  image_format_hint` now documents encoded hints (`image/jpeg;max_edge=<N>`,
  `image/png;max_edge=<N>`) and states the `rgba8:WxH` shape belongs to the semantic
  embedding RPC only; `ScoreImageRequest.image_format_hint` gained the same comment. Field
  numbers unchanged. Regenerates Rust (prost, byte-identical) + C++ stubs (no semantic
  change); `grep -rn "rgba8:WxH" rust/puerh_mind` confirmed no 5c test greps the proto
  source.
- `alcedo_studio/src/include/app/ai_sidecar_runtime_service.hpp` - new proto-free DTOs
  (`ImageAnalysisRendition`, `ImageAnalysisUsage`, `ImageAnalysisScoredDimension`,
  `ImageAnalysisRequest`, `ImageAnalysisUnderstandingResult`, `ImageAnalysisRatingResult`;
  `status` / `error_code` as `int` raw enum values, mirroring `AiSidecarCapability.
  input_kinds`); pure-virtual `DescribeImage` / `ScoreImage` on `IAiSidecarRuntimeClient`
  (no v1/v2 split - image analysis is new); `override` decls on `GrpcAiSidecarRuntimeClient`;
  ready-guarded `AiSidecarRuntimeService::DescribeImage` / `ScoreImage` wrappers.
- `alcedo_studio/src/app/ai_sidecar_runtime_service.cpp` - `#include "image_analysis.pb.h"`
  + `"image_analysis.grpc.pb.h"`; anon-namespace mappers `ToImageAnalysisRendition`,
  `ToImageAnalysisUsage`, `ToImageUnderstandingResult`, `ToImageRatingResult` (ok follows
  `AiResponseHeader.status`: `AI_STATUS_OK` => ok + mapped body; anything else => ok=false
  with the redacted header error and the body left empty - fail-closed, matching the Rust
  service), and `FillImageAnalysisRequestProto`; `GrpcAiSidecarRuntimeClient::DescribeImage`
  / `ScoreImage` (build the proto request, `FillAiRequestHeader(..., request_id,
  "image_understanding.describe"|"image_rating.score", timeout, credential_ref, request_id)`,
  `set_image_bytes` / `set_image_format_hint` / fill `mutable_rendition()` / provider/model/
  prompt(/rubric), call the stub; `!status.ok()` => failed result with `GrpcErrorMessage`,
  UNIMPLEMENTED mapped to `AI_STATUS_UNIMPLEMENTED`); `AiSidecarRuntimeService::DescribeImage`
  / `ScoreImage` wrappers (not-ready => failed result `"ai sidecar runtime is not ready"`).
- `alcedo_studio/src/include/app/image_analysis_encoder.hpp` +
  `alcedo_studio/src/app/image_analysis_encoder.cpp` - new. `EncodedRendition { bytes,
  mime_type, format_hint, rendition_kind, width, height, max_edge, quality, ok, error }` and
  `EncodeThumbnailForRemoteAnalysis(guard, quality, max_edge_hint, temp_dir, error)`.
  Includes only `opencv2/core.hpp` + `opencv2/imgproc.hpp` (NO `opencv2/imgcodecs`) +
  `<OpenImageIO/imageio.h>`. Replicates `PrepareForOiioEncoding` (anon-namespace in
  `thumbnail_disk_cache_service.cpp`, unreachable) -> CV_8UC3 RGB; syncs via
  `ImageBuffer::SyncToCPU()` when `gpu_data_valid_ && !cpu_data_valid_` (mirrors
  `MaterializeThumbnailRgba8`). `max_edge = max(width,height)`; `format_hint =
  "image/jpeg;max_edge=<N>"`. OIIO temp-file + readback (the in-memory sink is unproven on
  this MSVC/DLL build): `ImageOutput::create(dst_string)` (single-arg, not the two-arg
  MSVC-breaking overload) -> `ImageSpec(w,h,3,UINT8)` -> `CompressionQuality` -> `open` ->
  `write_image` -> `close`; read back via `ifstream(binary)+istreambuf_iterator`; a
  `TempFileGuard` RAII removes the temp file on every exit path (success, OIIO failure,
  readback failure).
- `alcedo_studio/src/include/app/image_analysis_service.hpp` +
  `alcedo_studio/src/app/image_analysis_service.cpp` - new. Types: `ImageAnalysisItem`,
  `ImageAnalysisTask {kDescribe, kScore}`, `ImageAnalysisItemStatus`, `ImageAnalysisCredential`,
  `ImageAnalysisOptions` (task, `thumbnail_resolution=k1024`, `jpeg_quality=90`, timeout,
  provider/model/prompt/rubric, credential, `temp_dir`, `credential_ttl_ms`),
  `ImageAnalysisProgress`, `ImageAnalysisItemResult` (carries understanding OR rating + the
  recorded rendition). `ImageAnalysisInFlightGate` (`Acquire(is_canceled)` cv-wait on
  `!in_flight_ || is_canceled()`, `Release`, `PublishRequestId`/`ClearRequestId`/
  `CurrentRequestId`, `NotifyAll`) - injectable so the album backend (Phase 6) can share one
  gate app-wide; the service creates a private one if none is passed. New
  `IImageAnalysisThumbnailProvider` + `ThumbnailServiceImageAnalysisProvider` (wraps
  `ThumbnailService::GetThumbnailDetailed`; `ThumbnailService` is NOT refactored). New
  `IImageAnalysisClient` (`Ready`, `RegisterCredential`, `DescribeImage`, `ScoreImage`,
  `CancelTask`) + `AiSidecarRuntimeImageAnalysisClient` (wraps `AiSidecarRuntimeService`;
  `Ready`->`IsRunning`). `ImageAnalysisJob` (`Cancel`/`IsCanceled`/`Wait`/`SnapshotProgress`/
  `Results`, dtor joins - mirrors `SemanticGenerationJob`). `ImageAnalysisService::StartAnalysis`
  spawns a `std::thread` per job (mirrors `StartGeneration`). `RunJob`: (1) if the credential
  secret is non-empty, `client->RegisterCredential` once, then zeroize+clear the secret from
  the local options copy and thread only the handle into every request; (2) per item:
  `WaitForOneThumbnail(k1024)` -> `EncodeThumbnailForRemoteAnalysis` -> build the typed
  request (`request_id = "image-analysis-<task>-<el>-<img>"`, `credential_ref = handle`,
  `format_hint`/rendition from the encoder's actuals); (3) `gate->Acquire(IsCanceled)` - if
  canceled while queued, exit without ever calling the provider; re-check `IsCanceled()` after
  acquiring; publish `request_id`; (4) `DescribeImage`/`ScoreImage`; clear id; release slot;
  (5) post-RPC `IsCanceled()` discard (the correctness guarantee, not CancelTask); (6) append
  result + dispatch progress. `ImageAnalysisJob::Cancel()` sets the flag, `gate_->NotifyAll()`,
  and best-effort `client->CancelTask(gate_->CurrentRequestId())` only while `am_in_flight_`
  (so a queued job's cancel never cancels another job's in-flight RPC).
- `alcedo_studio/tests/app/ai_sidecar_runtime_service_test.cpp` - `FakeAiSidecarRuntimeClient`
  extended with canned `DescribeImage`/`ScoreImage` + `DescribeImageCalls()`/`ScoreImageCalls()`
  counters; 3 new `AiSidecarRuntimeServiceTest` cases: `DescribeImageDelegatesToClient`,
  `DescribeImageRespectsReadyGuard`, `ScoreImageRespectsReadyGuard` (ready-guard + delegation;
  proto->DTO mapping is exercised only by a live sidecar per the embedding-mapper convention).
- `alcedo_studio/tests/app/image_analysis_encoder_test.cpp` - new. `EncodesThumbnailAsJpegByDefault`
  (mime `image/jpeg`, `format_hint == "image/jpeg;max_edge=64"`, JPEG SOI magic, NOT `rgba8:`,
  width/height/max_edge correct, no leftover temp file), `EncodesFromRgbaAndFloatInputs`
  (CV_8UC4 / CV_32FC3 / CV_8UC1 all normalize to RGB8 and encode), `NullBufferFailsCleanlyWithout
  LeakingTempFiles` (failure path leaves no temp file).
- `alcedo_studio/tests/app/image_analysis_service_test.cpp` - new, with a fake
  `IImageAnalysisClient` (configurable outcome, block/release latch, request recording, counters)
  + fake `IImageAnalysisThumbnailProvider`. Cases: `DescribeSuccessReturnsAnalyzedResult` (also
  asserts the rendition is recorded in result metadata), `MissingCredentialPropagatesAsError`
  (`AI_STATUS_UNAUTHENTICATED`/`MISSING_CREDENTIAL`), `InvalidProviderConfigPropagatesAsError`
  (`UNSUPPORTED_TASK`/`TASK_UNKNOWN`), `TimeoutPropagatesAsError` (`DEADLINE_EXCEEDED`),
  `SchemaErrorPropagatesAsErrorWithoutActiveResult` (`PROVIDER_ERROR`/`PAYLOAD_DECODE`, ok=false,
  caption empty), `CancelRunningJobCallsCancelTaskAndDiscardsResult` (CancelTask called once with
  the in-flight id, no extra provider call, result canceled), `TwoJobsSharingGateRunSerially`
  (two service instances sharing one gate - the production scenario - second provider call does
  not start until the first releases), `CancelQueuedJobDoesNotStartProviderCall` (queued job
  canceled -> DescribeImage never called), `SecretReachesOnlyRegisterCredentialNotDescribeImage`
  (sentinel secret reaches RegisterCredential but only the opaque handle reaches DescribeImage;
  result/error carry no secret).
- `alcedo_studio/src/CMakeLists.txt` - `def_library(ImageAnalysisEncoder ...)` (PUBLIC_DEPS
  ThumbnailService ImageBuffer OpenImageIO::OpenImageIO opencv_core opencv_imgproc - NO
  opencv_imgcodecs) and `def_library(ImageAnalysisService ...)` (PUBLIC_DEPS AiSidecarRuntimeService
  ThumbnailService ImageAnalysisEncoder ImageBuffer, PRIVATE_DEPS AppDiagnostics).
- `alcedo_studio/tests/CMakeLists.txt` - `ImageAnalysisEncoderTest` + `ImageAnalysisServiceTest`
  targets; both registered in the `app` category, `ImageAnalysisServiceTest` also in `ci_raw`.

Invariants (Phase 5d review focus):

- The host controls which rendition is sent and records it in result metadata: the service
  requests `ThumbnailResolution::k1024`, the encoder reports the ACTUAL width/height/max_edge,
  the request carries `RenditionMetadata`, and the result echoes it
  (`DescribeSuccessReturnsAnalyzedResult` asserts `rendition.max_edge == 16` on the test fixture).
- No `ThumbnailService` refactor and no LibRaw embedded-thumbnail fast path: 5d adds a new
  `ThumbnailServiceImageAnalysisProvider` adapter; `ThumbnailService` is untouched.
- Encoded remote-analysis payloads and raw CLIP embedding payloads remain separate code paths:
  the encoder produces `image/jpeg;max_edge=<N>` bytes; `MaterializeThumbnailRgba8`
  (`semantic_generation_service.cpp`) still owns the `rgba8:WxH` CLIP path. The two never share a
  producer.
- The JPEG upload encoder uses OpenImageIO as the primary codec path and does not reintroduce
  fragile OpenCV image-codec behavior: the encoder includes only `opencv2/core.hpp` +
  `opencv2/imgproc.hpp` (channel/depth conversion) and `OpenImageIO/imageio.h` for the actual
  encode; `ImageAnalysisEncoder` does not link `opencv_imgcodecs`.
- Remote calls are serialized at the host boundary and retries cannot multiply concurrency: the
  injectable `ImageAnalysisInFlightGate` caps in-flight remote analyses at one;
  `TwoJobsSharingGateRunSerially` proves two service instances sharing one gate serialize. There
  are no host-side retries in 5d (the bounded retry lives in the Rust `http_util` driver, which
  is behind the single gate slot).
- Sidecar startup remains on demand and normal browsing/search do not require API keys: the
  service never auto-starts the runtime; `AiSidecarRuntimeImageAnalysisClient::Ready()` ->
  `IsRunning()`, and a not-ready runtime fails fast without an API key
  (`DescribeImageRespectsReadyGuard` / `ScoreImageRespectsReadyGuard`).

Credential handling (Phase 3 invariant preserved): the secret travels only over gRPC loopback
to `RegisterCredential`; `RunJob` zeroizes + clears it from the local options copy immediately
after registration and threads only the opaque handle into `ImageAnalysisRequest.credential_ref`.
The secret never enters `ImageAnalysisRequest`, result DTOs, `AiSidecarRuntimeOptions`, process
args, or logs. `SecretReachesOnlyRegisterCredentialNotDescribeImage` proves the sentinel reaches
`RegisterCredential` but only the handle reaches `DescribeImage`; the existing
`AiSidecarRuntimeServiceTest.RegisterCredentialReturnsHandleWithoutLeakingSecretIntoProcessArgs`
(Phase 3) still covers the process-args surface. The sidecar returns results only - C++ owns all
DB writes (none occur in 5d).

Cancellation: the post-RPC `IsCanceled()` discard is the correctness guarantee - a canceled
running job's provider result is dropped and the item marked canceled even if the provider call
completed (`CancelRunningJobCallsCancelTaskAndDiscardsResult`). `Cancel()` additionally
best-effort calls `CancelTask` on this job's in-flight `request_id` (only while `am_in_flight_`,
so a queued job's cancel never touches another job's RPC). A canceled queued job exits without
ever calling the provider (`CancelQueuedJobDoesNotStartProviderCall`).

Distinct contracts: `DescribeImage` -> `ImageUnderstandingResult`, `ScoreImage` ->
`ImageRatingResult`, with distinct task_ids (`"image_understanding.describe"` vs
`"image_rating.score"`); a rating result can never overwrite an understanding result (the
`ImageAnalysisItemResult` carries both but only the task-matching one is filled).

Deferred to Phase 5e / 5f / 5g / 6:

- Local prefill queue before persistence (5e): while one encoded image is in the remote LLM call, the
  host should prepare the next encoded rendition into a bounded queue. 5d returns structured DTOs only.
- Database writes / persistence / search integration (5f).
- Product UI / controller wiring / the OS credential store (QtKeychain) and a shared gate owned
  by the album backend (6). 5d's `ImageAnalysisService` is standalone and constructed directly by
  tests; Phase 6 must construct it with ONE shared `ImageAnalysisInFlightGate` (and a secure
  secret source) so the host-boundary serialization holds app-wide - per-use construction without
  a shared gate would NOT serialize across instances (the 5d test proves serialization only when
  the gate is shared).
- PNG fallback (JPEG-only in 5d). No PNG OIIO encode exists in-repo (unproven on this MSVC/DLL
  build); JPEG covers the photographic-thumbnail MVP. PNG is a fast-follow once a PNG OIIO encode
  is validated.
- Live sidecar proto->DTO mapper coverage (5g). `ToImageUnderstandingResult` / `ToImageRatingResult`
  are not directly unit-tested, matching the embedding-mapper convention (`ToEmbeddingResult` is
  also untested directly); they are exercised by a live sidecar in 5g.

Test results:

- Rust - `cargo test -- --skip live_` in `rust/puerh_mind`: 185 passed; 0 failed; 0 ignored; 3
  filtered (the 3 live smokes). Confirms the comment-only `image_analysis.proto` edit did not
  break 5a-5c.
- C++ MSVC build (run through the PowerShell tool, per project memory): `--target AiProto
  ImageAnalysisEncoder ImageAnalysisService ImageAnalysisEncoderTest ImageAnalysisServiceTest
  AiSidecarRuntimeServiceTest` built clean. `AiProto` regenerated `image_analysis.pb.{cc,h}` /
  `image_analysis.grpc.pb.{cc,h}` from the comment edit; `AiSidecarRuntimeService.lib`,
  `ImageAnalysisEncoder.lib`, `ImageAnalysisService.lib`, and the three test executables linked.
  (One compile bug fixed during implementation: `image_analysis_service.cpp` initially omitted
  `#include "app/image_analysis_encoder.hpp"`, producing an `EncodeThumbnailForRemoteAnalysis`
  identifier-not-found cascade - fixed before any test ran; no shipped-code bug.)
- ctest Phase 5d group (`-R "ImageAnalysisEncoderTest|ImageAnalysisServiceTest|AiSidecarRuntimeServiceTest"`):
  36/36 passed (3 `ImageAnalysisEncoderTest` + 9 `ImageAnalysisServiceTest` + 24
  `AiSidecarRuntimeServiceTest` including the 3 new wire tests; 1 pre-existing live-runtime test
  Skipped - `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` not set).
- ctest regression regex (`-R "SemanticGenerationServiceTest|SemanticStorageControllerTest|FilterServiceTest|GlobalSearchDialogQmlTest|SearchQueryClassifierTest"`):
  66/66 passed - no regression in semantic/storage/search from the shared-header changes.

Build note for the next handoff: after any edit to `image_analysis.proto` or to test-only
sources, build the affected targets explicitly before ctest (mirroring the Phase 4 / 5b notes) -
the default `all` build does not always rebuild test executables whose own sources changed:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ImageAnalysisServiceTest
ImageAnalysisEncoderTest --parallel 4` (run through PowerShell, not Bash).

Review conclusion: none (no shipped-code bugs found in review - the one compile error during
implementation, the missing `image_analysis_encoder.hpp` include, was caught at compile time
before any test ran); risk accepted: (1) JPEG-only encoder - PNG fallback deferred because no PNG
OIIO encode exists in-repo and is unproven on this MSVC/DLL build (photographic thumbnails are
JPEG; PNG is a fast-follow); (2) proto->DTO mappers (`ToImageUnderstandingResult` /
`ToImageRatingResult`) are not directly unit-tested, matching the embedding-mapper convention -
exercised by the 5g live sidecar; (3) the mid-`write_image` OIIO failure cleanup is covered by
the `TempFileGuard` RAII design + review, not by a forced-failure test (the deterministic proxies
- success-cleanup and empty-guard early-return - are tested); (4) production cross-instance
serialization depends on Phase 6 wiring a single shared `ImageAnalysisInFlightGate` - the 5d test
proves two instances sharing one gate serialize, but a per-use construction without a shared gate
would NOT serialize across instances (flagged for Phase 6); missing tests: none - all plan-required
5d tests (success, missing credential, invalid provider config, timeout, cancellation, schema-error
propagation; encoder JPEG-default + format metadata + no `rgba8:WxH` + no leftover temp files +
no OpenCV imgcodecs dep; two-jobs-serial + cancel queued/running + no extra provider call; secret
not in request/options/args/logs) are implemented and green, and the 5d-adjacent regression suite
is green.

## Phase 5e - Completion & Self-Review

Status: complete. `ImageAnalysisService::RunJob` is now a producer/consumer
prefill pipeline that overlaps local thumbnail/JPEG preparation with the single
in-flight remote LLM call, without writing any database rows. A bounded
`PrefillQueue` (depth = `prefetch`, clamped to `[1, kMaxImageAnalysisPrefetch]` (= 4))
holds only encoded JPEG bytes
plus rendition/item/request/task/provider metadata - never `ThumbnailGuard` or
`ImageBuffer` pins. A producer thread runs `ThumbnailService` request -> CPU
materialization -> `EncodeThumbnailForRemoteAnalysis` -> release the guard -> push
the encoded item; the consumer worker pops FIFO -> `ImageAnalysisInFlightGate::
Acquire` -> `DescribeImage`/`ScoreImage` -> `Release` -> append the DTO. The gate
still caps remote concurrency at one; this phase only overlaps local prep with the
active provider call. All Phase 5d invariants (credential-vault registration +
secret zeroization, post-RPC cancel discard, best-effort `CancelTask` on this job's
in-flight id only, OIIO JPEG encoder, no DB writes) are preserved unchanged. No
product UI / controller wiring (Phase 6).

Implemented (file-by-file, per the plan):

- `alcedo_studio/src/include/app/image_analysis_service.hpp` - `ImageAnalysisOptions`
  gains `int prefetch = 1;` (Phase 5e prefill queue depth, clamped to
  `[1, kMaxImageAnalysisPrefetch]` in RunJob; the gate still caps remote at one).
  New `inline constexpr int kMaxImageAnalysisPrefetch = 4;` bounds the prefill depth
  so a caller cannot request an unbounded depth (Phase 5e review follow-up). The gate
  replaces the separate `Acquire` + `PublishRequestId` with `AcquireAndPublish`
  (acquire + publish request_id atomically under one lock) to close the post-acquire
  cancel race. `ImageAnalysisJob` gains a `std::thread producer_;` member (joined in
  the dtor alongside `worker_`).
- `alcedo_studio/src/app/image_analysis_service.cpp` - `RunJob` refactored into a
  producer/consumer pipeline. New `EncodedAnalysisItem` (encoded bytes + mime /
  format_hint + rendition metadata + item ids + request_id + task/provider options
  + the opaque `credential_ref` - NO `ThumbnailGuard`/`ImageBuffer` pin). New
  `PrefillQueue` (bounded `std::deque`, 25ms timed-wait `Push`/`Pop` so cancel is
  observed without an explicit notify, `MarkProducerDone`). The producer thread
  (`producer_body` lambda, stored on `producer_`) prepares items in order:
  `WaitForOneThumbnail` -> `EncodeThumbnailForRemoteAnalysis` -> release the
  `ThumbnailGuard` -> `Push` (drops + exits on cancel). The consumer (the existing
  `worker_` thread) pops FIFO -> on `PrepFailed`/`canceled`/`Encoded`:
  `gate->AcquireAndPublish(request_id, IsCanceled)` (acquires the slot AND publishes
  the request_id atomically; exits without a provider call if canceled while queued)
  -> `am_in_flight_.store(true)` -> re-check `IsCanceled()` (PRE-RPC: if cancel
  landed between AcquireAndPublish's internal check and the am_in_flight_ store, it
  saw no in-flight job and sent no CancelTask; this re-check tears down the slot and
  discards as canceled WITHOUT issuing the paid provider RPC, closing the narrow
  post-acquire race) -> `DescribeImage`/`ScoreImage` (a `ScopeExit` slot guard
  releases the gate on every exit path) -> post-RPC `IsCanceled()` discard (the
  correctness guarantee) -> `Release` -> append result + `++consumed`.
  `MarkProducerDone` + `producer_.join()` run before the finalizer, which emits
  `canceled` for `items[consumed..N-1]`. `prefetch` is clamped via
  `std::clamp(options.prefetch, 1, kMaxImageAnalysisPrefetch)` (logged when clamped).
- `alcedo_studio/tests/app/image_analysis_service_test.cpp` - 9 new
  `ImageAnalysisServiceTest` cases (the 8 plan-required 5e tests + 1 review-driven
  prefetch-clamp regression):
  `PrefillPipelinePreparesImage2WhileImage1BlockedInRpc` (prefetch=1; image 2 is
  thumbnail-requested/encoded while image 1 is blocked in the fake `Describe`),
  `PrefetchBoundedQueueDoesNotRequestWholeAlbum` (prefetch=2 over 6 items; requests
  == prefetch+2 < 6, never the whole album), `OversizedPrefetchClampedToUpperBound`
  (prefetch=1000 over 10 items; clamped to `kMaxImageAnalysisPrefetch`, requests
  == max+2 < 10, never the whole album - Phase 5e review follow-up),
  `PinReleasedAfterEncodeBefore
  WaitingBehindGate` (`ReleaseThumbnail` called for all 3 before the first
  `Describe`), `CancelWhileConsumerWaitsForEncodedItem` (blockable provider; all
  items canceled, zero `Describe` calls), `CancelWhileProducerWaitsForQueueCapacity`
  (prefetch=1 over 4 items; cancel unblocks the producer), `CancelWhileRemote
  RequestInFlightDiscardsResult` (post-RPC discard + best-effort `CancelTask` on
  this job's id only), `CancelAfterPrefilledNotSentItemsDropsQueuedRenditions`
  (queued renditions dropped after cancel), `TwoJobsSharingGateSerializeRpcsWith
  Prefill` (two jobs sharing one gate; B prefills locally with 0 remote calls while
  A holds the slot). Plus a `SpinWaitFor` polling helper and a refactor of
  `FakeThumbnailProvider` to a blockable mode (pending-request queue +
  `SetBlockMode`/`WaitForPending`) for the consumer-waits-for-encoded-item case.
  New includes `<condition_variable>`, `<deque>`, `<vector>`.
- `alcedo_studio/tests/app/image_analysis_live_smoke_test.cpp` - NEW env-gated
  live smoke (Phase 5g live-smoke territory, built early here as the bonus
  real-image end-to-end). Skips unless `ALCEDO_IA_LIVE_RUNTIME_PATH`,
  `ALCEDO_TEST_PACKED_PROJECT_PATH`, and `ALCEDO_IA_LIVE_ENV_TEST_PATH` are set
  (optional `ALCEDO_IA_LIVE_PROVIDER_ID`, default `openrouter`). Opens a packed
  `.alcd` via `ProjectPackageService`, materializes one k1024 thumbnail through the
  REAL `ThumbnailService`+`PipelineMgmtService`, starts the real sidecar
  (`require_model_info=false`, `allow_download=false`, empty model root - describe
  is served over the HTTP provider path), registers a real credential read from
  `.env.test`, runs `ImageAnalysisService` describe, and prints caption/tags/scene/
  confidence (NEVER the image or the key) with redact-checks that the key is absent
  from every result field. Calls `RegisterAllOperators()` at start (see harness
  note below). The key var is `ALCEDO_VOLCENGINE_ARK_API_KEY` for any
  `volcengine*` provider, else `ALCEDO_OPENROUTER_API_KEY`.
- `alcedo_studio/tests/CMakeLists.txt` - new `ImageAnalysisLiveSmokeTest` target
  (links `ImageAnalysisService ProjectService GTest::gtest_main`), registered in
  the `ci_raw_flow` label.

Invariants (Phase 5e review focus):

- The queue stores encoded payloads, not `ThumbnailGuard`/`ImageBuffer` pins:
  `EncodedAnalysisItem` carries bytes + metadata + the opaque `credential_ref`
  only; the producer releases each `ThumbnailGuard` immediately after encoding,
  before pushing. `PinReleasedAfterEncodeBeforeWaitingBehindGate` asserts
  `ReleaseCount == 3` while only 1 `Describe` has fired.
- Remote provider concurrency remains one across all services sharing the gate:
  the prefill queue only overlaps LOCAL prep; `gate->AcquireAndPublish` still gates
  the remote call. `TwoJobsSharingGateSerializeRpcsWithPrefill` proves two jobs
  sharing one gate serialize (B makes 0 remote calls while A holds the slot); the
  gate's contract changed only in that acquire + request_id publish are now atomic
  (5e review follow-up), which does not affect serialization.
- Cancellation cannot cancel another job's in-flight request, cannot issue a paid
  provider RPC after a cancel that sent no CancelTask, and cannot leave the gate or
  queue permanently blocked: the 25ms timed-wait on both `Push` and `Pop` observes
  the cancel flag without an explicit notify; `Cancel()` calls `gate_->NotifyAll()`;
  `AcquireAndPublish` publishes this job's request_id atomically with the slot, so
  while `am_in_flight_` is true `gate_->CurrentRequestId()` is this job's non-empty
  id (a queued job's cancel never touches another job's RPC); the post-acquire
  PRE-RPC `IsCanceled()` re-check tears down the slot without a provider call if
  cancel landed between AcquireAndPublish and the `am_in_flight_` store (closing the
  narrow race where Cancel saw no in-flight job and sent no CancelTask); the
  post-RPC `IsCanceled()` discard drops this job's own result if cancel landed
  during the call (CancelTask best-effort + discard). The four cancel tests cover
  producer-waits-for-capacity, consumer-waits-for-item, remote-in-flight, and
  encoded-but-not-sent.
- Memory behavior for large albums: bounded JPEG queue
  (`PrefetchBoundedQueueDoesNotRequestWholeAlbum`: with prefetch=2 over 6 items,
  only prefetch+2 thumbnails are requested, never the whole album), no unbounded
  thumbnail pins (guards released after encode), no database writes (RunJob emits
  DTOs only).

Credential handling (Phase 3 invariant preserved, unchanged from 5d): the secret
travels only to `RegisterCredential` once at job start; `RunJob` zeroizes + clears
the local options copy immediately after and threads only the opaque
`credential_ref` through the `EncodedAnalysisItem` and the request. The prefill
queue never holds the secret - only the handle.

Cancellation (guarantee strengthened, now spanning the prefill pipeline): the
post-RPC `IsCanceled()` discard is the correctness guarantee for a cancel that
lands during the RPC (CancelTask best-effort + discard); the 5e review follow-up
adds a post-acquire PRE-RPC `IsCanceled()` re-check so a cancel that lands in the
narrow window between `AcquireAndPublish` and the `am_in_flight_` store (where
`Cancel()` saw no in-flight job and sent no CancelTask) tears down the slot and
discards as canceled WITHOUT issuing the paid provider RPC. `AcquireAndPublish`
publishing the request_id atomically with the slot means `Cancel()`'s
`am_in_flight_` + `CurrentRequestId()` checks always agree. The 25ms timed-wait
polling on the bounded queue means a producer blocked on capacity and a consumer
blocked on an empty queue both observe cancel promptly without an explicit notify
(the gate's `NotifyAll` covers the gate wait). A canceled queued job exits without
ever calling the provider.

Bonus live run (Phase 5g-adjacent): the env-gated live smoke now runs a real
describe end-to-end against a packed `.alcd` project. Two test-harness /
environment issues were found and fixed during the run (neither is a Phase 5e
shipped-code bug):

- (Harness) The live smoke initially crashed with 0xC0000005 inside
  `CPUPipelineExecutor::InitDefaultPipeline` -> `PipelineStage::SetOperator` (null
  `op_` deref). Root cause: the smoke never called `alcedo::RegisterAllOperators()`,
  so the global `OperatorFactory` was empty and `OperatorFactory::Create` returned
  nullptr for every default operator; the 3-arg `SetOperator` then dereferenced the
  null op in `SetGlobalParams`. Fixed by calling `RegisterAllOperators()` at the
  start of the smoke (exactly as `main.cpp:142` and every pipeline-using test
  fixture do). This is a harness requirement for any test exercising the real
  `ThumbnailService` -> CPU-pipeline render path, not a Phase 5e defect.
- (Environment) `DescribeImage` returned gRPC UNIMPLEMENTED (code 12). The on-disk
  release `alcedo_mind.exe` was a Phase-4-era build that registered
  `AiRuntimeService` (so `RegisterCredential` worked) but NOT `ImageAnalysisService`
  (`registry.rs:66` is a 5a addition). The C++ client even documents this case
  (`grpc::UNIMPLEMENTED => the sidecar predates Phase 5d`). Fixed by rebuilding the
  release sidecar binary (`cargo build --release --bin alcedo_mind` in
  `rust/puerh_mind`).

Provider-credential state in `.env.test` (environment, not code): the legacy
OpenRouter env value returns HTTP 401 and should not be treated as the user's paid
provider path (the subscription is Opencode). The Volcengine Ark Responses default
model returns HTTP 404 (the default `doubao-seed-2.0-lite-260428` endpoint/model
is not found for this account - this is Open Decision #4, the live-verified
Volcengine Responses shape). The Volcengine Ark Coding Plan
(Anthropic-compatible, via `AnthropicMessagesProvider`) WORKS with the same Ark
key. The live smoke PASSED
with `ALCEDO_IA_LIVE_PROVIDER_ID=volcengine_ark_coding` against the real `.alcd`
image: the LLM (Doubao seed 2.0 lite, model id `doubao-seed-2.0-lite`) returned a
coherent caption, 10 tags, a scene hint, and confidence 0.95 in ~2.8s, and the
redact-checks confirmed the key is absent from every result field. (The actual
caption is printed only to the env-gated test stdout, not recorded here, per the
"you don't need to look at what the image is" instruction.)

Test results:

- C++ MSVC build (PowerShell tool): `--target ImageAnalysisServiceTest
  ImageAnalysisLiveSmokeTest` built clean. The two live-smoke edits this phase
  (`RegisterAllOperators()` + the `volcengine*` key-var fix) rebuilt in one pass.
- ctest Phase 5d/5e group (`-R "ImageAnalysisServiceTest|ImageAnalysisEncoderTest
  |AiSidecarRuntimeServiceTest"`): 46/46 passed (19 `ImageAnalysisServiceTest`
  incl. the 9 new 5e cases, 3 `ImageAnalysisEncoderTest`, 24
  `AiSidecarRuntimeServiceTest`; 1 pre-existing live-runtime test Skipped -
  `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` not set). The 9 new 5e tests rely only on
  bounded 25ms timed-wait polling, not wall-clock timing. Re-run after the 5e
  review follow-up (cancel-race fix + prefetch upper cap): 46/46, incl. the new
  `OversizedPrefetchClampedToUpperBound` regression.
- Rust (no 5e source change - the sidecar was only REBUILT, not modified): the 5d
  suite (185 passed) is unchanged. The 3 env-gated Rust live smokes were run this
  phase: `live_openrouter_smoke_*` -> HTTP 401, `live_volcengine_ark_smoke_*` ->
  HTTP 404, `live_volcengine_ark_coding_smoke_*` -> PASSED (real caption of the
  32x32 fixture PNG), confirming the driver / endpoint / key matrix above.
- C++ env-gated live smoke `ImageAnalysisLiveSmokeTest.DescribesOneImageFrom
  PackedProject`: PASSED with `volcengine_ark_coding` (real describe of a real
  `.alcd` image through the full C++ -> sidecar -> HTTP-provider path); SKIPPED
  cleanly without the env vars.

Deferred to Phase 5f / 5g / 6:

- Database writes / persistence / search integration (5f) - 5e still emits DTOs
  only.
- The env-gated live smokes are formally a 5g deliverable; the C++ one was built
  early here as the bonus real-image run. Phase 5g should also record the
  provider-credential matrix (OpenRouter 401 / Volcengine Ark Responses 404 /
  Volcengine Ark Coding Plan ok) and the harness requirement to call
  `RegisterAllOperators()` in any test that drives the real CPU pipeline.
- Product UI / controller wiring / a shared `ImageAnalysisInFlightGate` owned by
  the album backend, and setting the app-wide `prefetch` (6).
- Opencode is the Phase 6 credential path to validate. Do not block 5g/6 on
  OpenRouter access; keep OpenRouter as an optional historical smoke.
  A confirmed Volcengine Ark Responses model/endpoint (Open Decision #4) is still
  useful for full coverage, but not required for the Opencode-compatible product
  path.

Build note for the next handoff: rebuild the release sidecar binary after any Rust
image-analysis change (`cargo build --release --bin alcedo_mind` in
`rust/puerh_mind`) - a stale binary returns UNIMPLEMENTED for `DescribeImage`. Run
MSVC builds through the PowerShell tool (not Bash), and build affected test targets
explicitly before ctest.

Review conclusion: two findings from the 5e review were addressed in a follow-up
(both shipped-code, both fixed and re-tested green):

- (P2 cancel race) `ImageAnalysisJob::Cancel` had a narrow window after
  `Acquire()` succeeded: if Cancel landed between the post-Acquire `IsCanceled()`
  check and the `am_in_flight_` / `PublishRequestId` stores, Cancel saw no
  in-flight request, sent no CancelTask, and the worker could still issue the paid
  provider RPC. Fixed by replacing `Acquire` + `PublishRequestId` with a single
  atomic `AcquireAndPublish(request_id, is_canceled)` (acquire + publish under one
  lock) and adding a post-publish / PRE-RPC `IsCanceled()` re-check after the
  `am_in_flight_` store: if Cancel landed in that window, the re-check tears down
  the slot and discards as canceled WITHOUT issuing the paid RPC. The seq_cst
  atomics + gate mutex make the store happen-after the atomic publish and the
  re-check observe a cancel that preceded it. The residual accepted behavior
  (unchanged) is that a cancel landing AFTER the re-check may still issue the RPC,
  but then Cancel DOES send CancelTask (am_in_flight_ true + published id) and the
  post-RPC discard drops the result. The four existing cancel tests stay green; the
  new pre-RPC discard branch is not deterministically reachable without
  production-side instrumentation, so it is covered by the memory-model argument +
  the regression suite, not a dedicated flaky test.
- (P2/P3 prefetch upper bound) `prefetch` was clamped only to `>= 1`; a caller
  could set an unbounded depth and let the producer encode most of a large album
  while one RPC was blocked. Fixed by adding `inline constexpr int
  kMaxImageAnalysisPrefetch = 4;` and clamping via
  `std::clamp(options.prefetch, 1, kMaxImageAnalysisPrefetch)` (logged when
  clamped). New regression `OversizedPrefetchClampedToUpperBound` (prefetch=1000
  over 10 items; requests == max+2 < 10, never the whole album).

Re-run after the follow-up: ctest 5d/5e group 46/46 (19 `ImageAnalysisServiceTest`
incl. the 9 new 5e cases + the new prefetch-clamp regression, 3
`ImageAnalysisEncoderTest`, 24 `AiSidecarRuntimeServiceTest`; 1 pre-existing
live-runtime Skip). The earlier-accepted risk (1) (prefetch lower-bounded only) is
now resolved; the remaining accepted risks are unchanged: (2) the deterministic
suite uses `FakeThumbnailProvider`, so the prefill pipeline's interaction with the
real `ThumbnailService` async / `PipelineScheduler` ThreadPool render path is
proven only by the env-gated live smoke (5g territory), not the always-run ctest
group; (3) the optional legacy `.env.test` OpenRouter smoke returns HTTP 401 and
the Volcengine Ark Responses default model 404s (Open Decision #4) - the live
describe succeeds via the Volcengine Ark Coding Plan provider, and this credential
/ config state is accepted as an environment constraint, not a code defect. The
two bonus-live-run
issues (live smoke omitted `RegisterAllOperators()` -> null-deref; stale Phase-4
sidecar binary -> UNIMPLEMENTED for `DescribeImage`) were test-harness /
environment, both fixed. Missing tests: none - all 8 plan-required 5e tests
(pipeline overlap, bounded queue, the four cancel scenarios, pin lifetime,
two-jobs-sharing-gate) plus the review-driven prefetch-clamp regression are
implemented and green, and the bonus env-gated live smoke runs end-to-end against
a real provider.

## Phase 5f - Completion & Self-Review

Status: complete. Remote image-analysis results now persist and feed search
without mixing the searchable understanding with the subjective rating. Two new
DuckDB tables (`AiImageUnderstanding`, `AiImageRating`) hold the annotations;
both bind to `file_id` (the Sleeve element id / inode) — the same key the CLIP
embeddings bind to, not the image id — so deleting a file cascades cleanly and a
re-import under a new image id recovers prior annotations. The
`(file_id, task_id)` primary key makes `insert_or_replace` enforce "at most one
row per pair", hence at most one active-for-search understanding per
`(file_id, task_id)`. The rating is kept out of full-text search by default: the
search document folds in active captions/tags/scene via a `string_agg` subquery
over `AiImageUnderstanding` only, and the `AiImageRating` columns never enter it.
All serialization/deserialization goes through the duckorm layer
(`insert_or_replace`, `select`, `remove`) — no raw INSERT/SELECT is written for
the AI tables. Deleting files removes both understanding and rating rows via the
existing element-deletion cascade. The remote rating contract (user-requested
Task #1) is changed to a 1..5 integer with no confidence. A bonus env-gated
live run proves both the describe (with search attribution) and the rating (1..5
integer, no confidence) paths end-to-end against a real LLM. No product UI /
controller wiring (Phase 6).

Implemented (file-by-file, per the plan):

- `alcedo_studio/src/ai/ai_description.cpp` +
  `alcedo_studio/src/include/ai/ai_description.hpp` - NEW standalone domain
  object (NOT a member of `image.hpp` or `sleeve_file.hpp`, per the Phase 5f
  code-quality requirement). Holds `file_id` + `task_id` +
  provider/model/prompt-profile/rendition identity + `caption` + `tags_json` +
  `scene` + `confidence` + `active` + `updated_at`. `Tags()` parses `tags_json`
  via `nlohmann::json` (catch returns `{}` — a malformed store surfaces "no
  tags" rather than throwing through the app/search path); `SetTags()` serializes.
  `IsValid()` requires `file_id != 0` and non-empty task/provider/model (the
  storage-layer backstop that a partial/failed remote call leaves no active
  search document).
- `alcedo_studio/src/ai/ai_rating.cpp` +
  `alcedo_studio/src/include/ai/ai_rating.hpp` - NEW standalone domain object,
  same placement rule. Holds `file_id` + `task_id` +
  provider/model/prompt-profile/rendition identity + `rating` (integer 1..5;
  `kMinRating = 1`, `kMaxRating = 5`, `NormalizeRating` clamps) +
  `rubric_id`/`rubric_version` + `reasons` + `active` + `updated_at`. `IsValid()`
  requires `file_id != 0`, non-empty task/provider/model, and `rating in [1,5]` —
  a rating of 0 ("unset") is rejected so a scored image is never confused with an
  unrated one (`UnsetRatingNotPersisted`). The EXIF-standard `Rating` (0..5,
  0 = unrated) in `metadata.hpp` is a separate contract and is untouched.
- `alcedo_studio/src/storage/controller/ai/ai_storage_controller.cpp` +
  `alcedo_studio/src/include/storage/controller/ai/ai_storage_controller.hpp` -
  NEW ORM controller. Field arrays via `FIELD_AS`
  (`kInsertUnderstandingFields` = 11 fields, `kSelectUnderstandingFields` = 12 in
  DDL order incl. `updated_at`; same shape for rating with `rating` INT32).
  `UpsertUnderstanding`/`UpsertRating` return false and write nothing when
  `!IsValid()`, else `duckorm::insert_or_replace` (PRIMARY KEY
  `(file_id, task_id)` replaces the prior row in place). `Get*` do a
  `SELECT * WHERE file_id = <int>` and filter `task_id` in C++; `GetActive*` add
  `AND active = TRUE`. `file_id` is read as INT64 (cast to `uint32`) and inserted
  as UINT32 — the element-id convention. `DeleteForFiles` takes a named guard +
  lock then calls `DeleteAiAnnotationRowsForFiles(conn, file_ids)`, which uses
  `duckorm::remove(conn, table, "file_id IN (...)")` for both tables — no raw
  DELETE for the AI tables, only the `file_id IN (...)` predicate. Constants
  `kUnderstandingTable = "AiImageUnderstanding"`,
  `kRatingTable = "AiImageRating"`.
- `alcedo_studio/src/include/storage/controller/db_controller.hpp` - NEW
  `ai_annotation_table_query` DDL (static `const char*`, executed once on both
  init paths, the same pattern as the semantic tables): `CREATE TABLE IF NOT
  EXISTS AiImageUnderstanding` (`file_id BIGINT`; `task_id`/`provider_id`/
  `model_id`/`prompt_profile_id`/`rendition_kind`/`caption`/`tags_json`/`scene`
  `VARCHAR NOT NULL DEFAULT ''`; `confidence DOUBLE NOT NULL DEFAULT 0.0`;
  `active BOOLEAN NOT NULL DEFAULT TRUE`; `updated_at TIMESTAMP DEFAULT
  current_timestamp`; `PRIMARY KEY (file_id, task_id)`) +
  `idx_ai_understanding_file_active (file_id, active)`; `AiImageRating`
  (`rating INTEGER NOT NULL DEFAULT 0`; `rubric_id`/`rubric_version`/`reasons`
  `VARCHAR NOT NULL DEFAULT ''`) + `idx_ai_rating_file_active`. Text columns are
  `NOT NULL DEFAULT ''` so `duckorm` never sees a `string(nullptr)` null-crash.
- `alcedo_studio/src/app/sleeve_filter_service.cpp` - NEW `AiUnderstandingExpr()`
  returns `string_agg(u.caption || ' ' || u.tags_json || ' ' || u.scene, ' ') FROM
  AiImageUnderstanding u WHERE u.file_id = e.id AND u.active = TRUE` (`e.id` is
  the element/file-id alias). Folded into `SearchDocumentExpr`'s `CONCAT_WS` as
  `COALESCE(AiUnderstandingExpr(), '')` after `SemanticLabelExpr`, so active
  captions/tags/scene participate in full-text search. The `AiImageRating`
  columns are deliberately NOT added (rating stays out of full-text search). Both
  the LIKE and the folded search paths consume the same `SearchDocumentExpr`.
- `alcedo_studio/src/storage/controller/sleeve/element_controller.cpp` - the
  existing anonymous-namespace `DeleteSemanticAndAiRowsForFiles(conn, file_ids)`
  (called from `RemoveElement(shared_ptr)` with a single-element span and from
  `RemoveElements` with the bulk `file_ids`) now also calls
  `DeleteAiAnnotationRowsForFiles(conn, file_ids)` after the pre-existing raw
  DELETEs for the three semantic tables. The AI cleanup runs on the
  ElementController's own connection so it is atomic with element deletion;
  rating rows are dropped here too even though they are not part of full-text
  search, so a re-import cannot resurrect an old AI rating under a new image id.
  The single-id `RemoveElement(sl_element_id_t)` overload is unchanged (it never
  cascaded semantic rows either — see accepted risk).
- `rust/puerh_mind/src/service/image_analysis.rs` (Task #1, user-requested) -
  `ScoreOutcome` drops `confidence` (the remote rating no longer outputs it);
  `IMAGE_RATING_SCHEMA` constrains `"rating": { "type": "integer", "minimum": 1,
  "maximum": 5 }` with `required = ["rating", "rubric_id"]`; `validate_rating`
  rejects `!(1..=5).contains(&out.rating)`. The understanding schema/validator
  are unchanged.
- `alcedo_studio/src/include/app/ai_sidecar_runtime_service.hpp` +
  `alcedo_studio/src/app/ai_sidecar_runtime_service.cpp` (Task #2) - the C++
  rating DTO `ImageAnalysisRatingResult` carries int `rating`, `rubric_id`,
  `rubric_version`, `reasons` and NO `confidence` (the header comment documents
  the Phase 5f contract change and contrasts it with
  `ImageAnalysisUnderstandingResult`, which still reports the describe-task
  `confidence`). `ToImageRatingResult` maps `ScoreImageResponse` -> DTO via
  `result.rating = body.rating()` (comment: "1..=5 integer star rating (Phase 5f
  contract); 0 = unset"), `rubric_id`, `rubric_version`; it maps no confidence,
  unlike the describe mapper which reads `body.confidence()`. The regenerated
  AiProto `ScoreImageResponse` (int32 `rating`, `rubric_id`, `rubric_version`,
  `reasons`; no confidence) mirrors this.
- `alcedo_studio/src/app/image_analysis_service.cpp` - threads `rubric_id` into
  the `ScoreImage` request and assembles the rating result (the score task's
  `ok`/`rendition`/`error` are taken from the rating result, never the
  understanding result, so the two contracts stay distinct).
- `alcedo_studio/tests/storage/ai_storage_controller_test.cpp` - NEW 11
  `AiStorageControllerTest` cases (the plan-required storage tests):
  `UpsertAndRetrieveUnderstanding` (full identity + content + confidence +
  tags-json round-trip), `ReplaceUnderstandingForSamePairKeepsOneRow`
  (`insert_or_replace` does not append a second row for the same
  `(file_id, task_id)` — the "at most one per pair" invariant),
  `DifferentTaskIdsForSameFileCoexist` (the invariant constrains each pair, not
  the file), `InvalidUnderstandingNotPersisted` (`!IsValid()` writes nothing),
  `GetActiveUnderstandingReturnsPersistedRow`, `UpsertAndRetrieveRating` (1..5
  integer + rubric identity round-trip), `UnsetRatingNotPersisted` (rating 0
  rejected), `RatingAndUnderstandingAreIsolated` (the rating's reasons never
  appear on the understanding object and vice versa),
  `ProviderModelPromptIdentityPreserved` (a different
  provider/model/prompt-profile/rubric on a later run is read back as that row's
  own identity), `DeleteForFilesRemovesBothKinds`, `ElementDeletionCascadesAiRows`
  (`ElementController::RemoveElements` drops AI rows on the same connection). The
  fixture uses a per-test temp DB, calls `RegisterAllOperators()`, and
  `CountUnderstandingRows` runs a raw `COUNT(*)` on the project's own connection
  (test verification, not ser/deser).
- `alcedo_studio/tests/app/filter_service_test.cpp` - 2 NEW `FilterServiceTest`
  cases (the plan-required search tests):
  `AiUnderstandingCaptionAndTagsSearchableOnlyAfterActivePersistence` (a
  caption/tag token is NOT searchable before `UpsertUnderstanding`, IS searchable
  after — so the hit is attributable to the AI row, not pre-existing
  filename/metadata) and `AiRatingReasonsAreNotInFullScreenSearch` (a token from
  persisted rating reasons is NOT searchable — rating stays out of full-text
  search). These are always-run deterministic coverage, not env-gated.
- `alcedo_studio/tests/app/image_analysis_live_smoke_test.cpp` - the env-gated
  live smoke is extended with a NEW `RatesOneImageFromPackedProject` (validates
  Task #1's 1..5 integer contract end-to-end) and the existing
  `DescribesOneImageFromPackedProject` is strengthened: after the live describe,
  the result is persisted via `AiStorageController.UpsertUnderstanding` and read
  back with `GetActiveUnderstanding` (round-trip assertions on caption/scene/
  provider/model/confidence/active/tags), then a token drawn from the live caption
  is shown searchable via a fresh `SleeveFilterService` ONLY after AI persistence
  (attribution proof). The rating test persists via `UpsertRating`, reads back
  with `GetActiveRating`, asserts `rating in [1,5]` and non-empty reasons and NO
  confidence field, and asserts a rating-reasons token is NOT searchable (rating
  excluded from full-text search). Both tests redact-check the key absent from
  every result/persisted field. (The actual caption and reasons are printed only
  to the env-gated test stdout, not recorded here, per the "you don't need to
  look at what the image is" instruction.)
- `alcedo_studio/tests/CMakeLists.txt` - NEW `AiStorageControllerTest` target
  (links `ProjectService GTest::gtest_main`, `gtest_discover_tests` with the
  WIN32 dll-copy block for lensfun/duckdb, label `ci_core_flow`), added to the
  `ci_core` category after `SemanticStorageControllerTest`.

Invariants (Phase 5f review focus):

- Failed remote calls do not create partial active search documents: the primary
  guard is that the host only reaches `UpsertUnderstanding`/`UpsertRating` on a
  complete successful describe/score (the live tests assert `kAnalyzed` +
  `understanding.ok`/`rating.ok` before persisting, and skip on a provider
  limitation rather than persist); the storage-layer backstop is `IsValid()` —
  `Upsert*` returns false and writes nothing when `file_id == 0` or
  task/provider/model is empty (or `rating not in [1,5]`).
  `InvalidUnderstandingNotPersisted` and `UnsetRatingNotPersisted` assert zero
  rows after a rejected upsert.
- Prompt/profile/rubric changes do not reinterpret old scores as new scores:
  identity (`provider_id`, `model_id`, `prompt_profile_id`, `rubric_id`,
  `rubric_version`) is stored PER ROW and round-trips.
  `ProviderModelPromptIdentityPreserved` upserts a row with a different
  provider/model/prompt-profile/rubric and reads it back as that row's own
  identity. `insert_or_replace` on `PRIMARY KEY (file_id, task_id)` replaces the
  prior row for that pair (latest wins), but each row carries its own identity, so
  a stale row cannot be misread as a new prompt's score; distinct `task_id`s
  coexist (`DifferentTaskIdsForSameFileCoexist`) so history is preserved across
  prompt-profile changes.
- At most one active understanding per `(file_id, task_id)`: the
  `PRIMARY KEY (file_id, task_id)` + `insert_or_replace` keep one row per pair;
  `ReplaceUnderstandingForSamePairKeepsOneRow` asserts the row count stays 1
  across two upserts for the same pair; `GetActiveUnderstanding` filters
  `active = TRUE`.
- Rating stays out of full-text search: the `AiImageRating` columns are not in
  `SearchDocumentExpr`; `AiRatingReasonsAreNotInFullScreenSearch` (offline) and
  the live rating test both assert a rating-reasons token is NOT searchable after
  persistence.

ORM discipline (user-requested): no raw SQL is written for AI
serialization/deserialization — `AiStorageController` uses
`duckorm::insert_or_replace`, `duckorm::select`, and `duckorm::remove`
exclusively, with `FIELD_AS` field descriptors and the `file_id IN (...)`
predicate as the only hand-written fragment (a predicate, not a statement). The
two raw `DELETE FROM SemanticImageEmbedding...` statements in
`DeleteSemanticAndAiRowsForFiles` are pre-existing (the semantic tables predate
5f and were already cleaned up with raw DELETEs there); refactoring them to an
ORM path is out of scope for 5f and is not a 5f change. The AI row cleanup is the
new code and it is ORM-faithful. No scattered patches: the cascade is one call
site in the existing `DeleteSemanticAndAiRowsForFiles`, and the search extension
is one `AiUnderstandingExpr` folded into the existing `SearchDocumentExpr`.

Foreign key = file_id (element id): both `AiImageUnderstanding` and
`AiImageRating` bind to `file_id` (`sl_element_id_t`, the Sleeve element id /
inode) — the same key the CLIP embeddings (`SemanticImageEmbedding*`) bind to,
not the image id. `file_id` is read as INT64 and cast to `uint32` for select, and
inserted as UINT32, matching the element-id convention. Because the cleanup is by
`file_id` and atomic with element deletion
(`ElementDeletionCascadesAiRows`), a re-import under a new image id cannot
resurrect an old AI annotation — the old rows are gone with the old element.

Rating contract (user-requested Task #1): the software already has an
EXIF-standard `Rating` (0..5 stars, 0 = unrated, integer) in `metadata.hpp`,
which is unchanged. The REMOTE LLM rating is a separate contract: the prompt/
response schema now requires a 1..5 integer and confidence is NOT output. Rust
`ScoreOutcome` has no `confidence`; `IMAGE_RATING_SCHEMA` constrains `rating` to
`integer` `minimum 1 maximum 5` with `required = ["rating", "rubric_id"]`;
`validate_rating` rejects `!(1..=5)`. The C++ `ImageAnalysisRatingResult` DTO and
the regenerated AiProto `ScoreImageResponse` carry the integer rating with no
confidence field; the `AiRating` domain object's `NormalizeRating` clamps to
`[1,5]` and `kMinRating = 1` (0 = unset = rejected). Validated live: the
Volcengine Ark Coding provider returned an integer rating in `[1,5]` with
non-empty reasons and no confidence field.

Bonus live run (Phase 5g-adjacent): the env-gated live smoke now runs BOTH a real
describe and a real score end-to-end against the packed `.alcd` project, through
the full C++ -> sidecar -> HTTP-provider path, with the rebuilt release
`alcedo_mind.exe` (Task #1's contract change). The key is read from `.env.test`
and registered into the sidecar vault; the secret is never in process args,
`AiSidecarRuntimeOptions`, logs, or the persisted rows (redact-checked absent
from every result/persisted field).

- Describe (re-run this phase with attribution): PASSED. The LLM returned a
  coherent caption, a non-empty tags list, a scene hint, and a confidence in
  range; the result was persisted via `AiStorageController.UpsertUnderstanding`
  and read back with `GetActiveUnderstanding` (round-trip on caption/scene/
  provider/model/confidence/active/tags); a token drawn from the live caption is
  searchable via a fresh `SleeveFilterService` ONLY after AI persistence (not
  before, not from pre-existing filename/metadata), proving the search hit is
  attributable to the AI row. Redact-checks pass. (The actual caption is printed
  only to the env-gated test stdout, not recorded here.)
- Rating (NEW this phase): PASSED. The Volcengine Ark Coding provider returned an
  integer rating in `[1,5]` (no confidence field), rubric `general/1.0`,
  non-empty reasons; the result was persisted via `UpsertRating` and read back
  with `GetActiveRating`; a rating-reasons token is NOT searchable after
  persistence (rating stays out of full-text search). Redact-checks pass. (The
  actual reasons are printed only to the env-gated test stdout, not recorded
  here.)

Provider-credential state in `.env.test` (environment, not code; unchanged from
5e): the legacy OpenRouter env value returns HTTP 401 and should not be treated as
the user's paid provider path (the subscription is Opencode); the Volcengine Ark
Responses default model returns HTTP 404 (Open Decision #4); the Volcengine Ark
Coding Plan (Anthropic-compatible, via `AnthropicMessagesProvider`) WORKS with the
same Ark key. Both live tests this phase used
`ALCEDO_IA_LIVE_PROVIDER_ID=volcengine_ark_coding`.

Test results:

- C++ MSVC build (PowerShell tool): the new `AiStorageControllerTest` target and
  the edited `FilterServiceTest` / `ImageAnalysisLiveSmokeTest` targets built
  clean.
- ctest Phase 5f group (`-R "AiStorageControllerTest|FilterServiceTest|
  ImageAnalysisServiceTest|ImageAnalysisLiveSmokeTest|AiSidecarRuntimeServiceTest"`):
  88/88 — 85 passed, 3 Skipped, 0 failed. Composition: 32 `FilterServiceTest`
  (incl. the 2 new AI cases `AiUnderstandingCaptionAndTagsSearchableOnlyAfter
  ActivePersistence`, `AiRatingReasonsAreNotInFullScreenSearch`), 24
  `AiSidecarRuntimeServiceTest` (23 passed + 1 pre-existing live-runtime Skip —
  `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` not set), 11 `AiStorageControllerTest` (all
  new, all passed), 19 `ImageAnalysisServiceTest`, 2 `ImageAnalysisLiveSmokeTest`
  (both Skipped — the live env vars are not set in the ctest shell; run
  individually below).
- Rust (Task #1 contract change): `cargo test --release -- --skip live_smoke`
  (the offline suite): 187/187 green (`0 failed`; 3 filtered out = the env-gated
  live smokes). The rating-contract tests pass
  (`validator_rejects_blank_tag_string_and_out_of_range_rating`,
  `out_of_range_rating_maps_to_schema_validation`,
  `parses_rating_accepts_float_rating_as_integer`,
  `strict_schema_forces_required_on_rating_properties`). The 3 env-gated Rust
  live smokes (run separately, not in the offline count) give the expected
  environment outcomes: `live_openrouter_smoke` -> HTTP 401,
  `live_volcengine_ark_smoke` -> HTTP 404 (Open Decision #4),
  `live_volcengine_ark_coding_smoke` -> PASSED. (A plain `cargo test --release`
  with `.env.test` present reports 188 passed / 2 failed — the 2 "failures" are
  exactly the OpenRouter 401 and Volcengine Ark 404 live smokes above; they are
  environment outcomes, not code defects, so the offline count excludes them via
  `--skip live_smoke`.)
- C++ env-gated live smoke: `ImageAnalysisLiveSmokeTest.DescribesOneImageFrom
  PackedProject` PASSED (with search attribution) and
  `RatesOneImageFromPackedProject` PASSED (1..5 integer, no confidence), both with
  `ALCEDO_IA_LIVE_PROVIDER_ID=volcengine_ark_coding` against the real `.alcd`
  image; both SKIP cleanly without the env vars (as in the ctest run above).

Deferred to Phase 5g / 6:

- The env-gated live smokes are formally a 5g deliverable; the C++ describe was
  built early in 5e and the C++ rating is built here. Phase 5g should record the
  provider-credential matrix and the harness requirements
  (`RegisterAllOperators()` for any test driving the real CPU pipeline; rebuild
  release `alcedo_mind.exe` after any Rust image-analysis change).
- Product UI / controller wiring: persisting results from the album backend,
  surfacing the rating as sort/filter/recommendation data only when a product
  rubric is approved, and constructing `ImageAnalysisService` with a single
  shared `ImageAnalysisInFlightGate` (6).
- Opencode is the Phase 6 credential path to validate. Do not block 5g/6 on
  OpenRouter access; keep OpenRouter as an optional historical smoke.
  A confirmed Volcengine Ark Responses model/endpoint (Open Decision #4) is still
  useful for full coverage, but not required for the Opencode-compatible product
  path.

Build note for the next handoff: rebuild the release sidecar binary after any
Rust image-analysis change (`cargo build --release --bin alcedo_mind` in
`rust/puerh_mind`) — a stale binary returns UNIMPLEMENTED for
`DescribeImage`/`ScoreImage` and would not carry the Task #1 rating-contract
change. Run MSVC builds through the PowerShell tool (not Bash), and build
affected test targets explicitly before ctest. For an offline cargo run with
`.env.test` present, use `cargo test -- --skip live_smoke` so the env-gated live
smokes (including the optional legacy OpenRouter smoke and known Volcengine
Responses 404) do not turn the suite red.

Review conclusion: bugs found — none; risk accepted — (1) the single-id
`ElementController::RemoveElement(sl_element_id_t)` overload does not cascade AI
(or semantic) rows, which is pre-existing behavior unchanged by 5f and out of
scope, and (2) the `.env.test` credential matrix (optional legacy OpenRouter 401,
Volcengine Ark Responses 404 = Open Decision #4) is an environment constraint,
not a code defect; missing tests — none.

## Phase 6a - Completion & Self-Review

Status: complete. The Phase 6 product mental model is now "compatible protocol
preset", not "provider brand": two Opencode Go compatible presets are shipped as
built-ins (`opencode_go_anthropic` over `anthropic_messages`, `opencode_go_openai`
over `openai_chat_compatible`), the editable preset field set is frozen as a C++
DTO + settings controller, and OpenRouter copy is demoted to legacy/optional. A
preset is advertised for image analysis only when a model has
`supports_vision && supports_structured_output` OR a live smoke has explicitly
pinned that capability (`live_confirmed`); the Opencode models ship unverified
(both flags false, `live_confirmed` false) so they are NOT advertised until a
live smoke confirms image input + structured JSON for the selected model — the
Phase 6 primary assumption. The Volcengine Ark Coding Plan built-in stays
unchanged (the proven Anthropic-compatible reference). No driver code changed
(the generic `openai_chat_compatible` driver is Phase 6b; the
`opencode_go_openai` preset is config-only until 6b wires it). No C++ host
job/credential wiring (6c/6d); no persistence changes (6e).

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/configs/providers/opencode_go_anthropic.json` - NEW built-in.
  `anthropic_messages`, `https://opencode.ai/zen/go/v1` + `/messages`, bearer auth
  slot `opencode_api_key`, `structured_output.mode = tool` strict, driver-owned
  parser (`content_json_pointer: null`), `provider_request_id_header: "request-id"`,
  one model `claude-sonnet-4-5` with `supports_vision=false`,
  `supports_structured_output=false`, `live_confirmed=false` (unverified → not
  advertised).
- `rust/puerh_mind/configs/providers/opencode_go_openai.json` - NEW built-in.
  `openai_chat_compatible`, `https://opencode.ai/zen/go/v1` + `/chat/completions`,
  bearer slot `opencode_api_key`, `structured_output.mode =
  response_format_json_schema` strict, `content_json_pointer:
  /choices/0/message/content`, one model `gpt-4o` unverified (all three flags
  false → not advertised). Driver reserved (not wired) until 6b; config-only here.
- `rust/puerh_mind/configs/providers/openrouter.json` - `display_name` changed to
  `"OpenRouter (legacy / optional Phase 5 smoke)"` (the Phase 6a "move OpenRouter
  copy to legacy/optional wording" deliverable). No wire-shape change.
- `rust/puerh_mind/src/service/provider_config.rs` -
  (a) `ModelConfig` gained `#[serde(default)] live_confirmed: bool` (the
  "live-smoke-pinned capability" mechanism; `#[allow(dead_code)]` until 6b/6f
  consume it from a driver path).
  (b) `build_provider_capability_descriptors` advertisement gate changed from
  `supports_vision && supports_structured_output` to
  `(supports_vision && supports_structured_output) || live_confirmed` — the Phase
  6a advertisement rule.
  (c) `BUILTIN_PROVIDER_CONFIGS` extended with the two Opencode `include_str!`
  entries (3 → 5 built-ins).
  (d) Module doc rewritten to describe the protocol-first preset model, the
  legacy/optional status of OpenRouter, and the advertisement rule.
- `alcedo_studio/src/include/app/ai_provider_preset.hpp` + `app/ai_provider_preset.cpp`
  - NEW. `struct AiProviderPreset` (the editable preset fields: display name,
  protocol family, base URL, endpoint, auth type, credential slot label, model id,
  optional model display name, structured-output mode, timeout, max image bytes,
  recommended rendition, masked key label, remember-key flag) with intentionally
  NO raw-API-key field. `class AiProviderPresetController : public QObject` with a
  `Q_PROPERTY` per field, `Q_INVOKABLE` setters, `CurrentPreset()` / `SetFromPreset()`
  / `Clear()`, shared `PresetChanged` NOTIFY, and `Normalized{ProtocolFamily,
  AuthType,StructuredOutputMode,Rendition}` clampers (mirror
  `NormalizedEndpointPreset` in `project_service.cpp`). Round-trips through
  `QSettings` under `ai/preset/*` (the same default-scope pattern as the semantic
  model-endpoint settings). Defaults mirror the Opencode Anthropic preset. No
  `SetApiKey` / `SetSecret` exists — the secret stays in the OS credential store
  (6c) and reaches the sidecar only as a vault handle.
- `alcedo_studio/src/CMakeLists.txt` - NEW `def_library(AiProviderPreset ...)`
  (PUBLIC `Qt6::Core`), placed beside `AiSidecarRuntimeService`.
- `alcedo_studio/tests/app/ai_provider_preset_test.cpp` + `tests/CMakeLists.txt` -
  NEW `AiProviderPresetTest` target (links `AiProviderPreset GTest::gtest_main`,
  LABELS `ci_core_flow`). 5 cases: `RoundTripsEveryEditableField`,
  `IndividualSettersPersistAndReload`, `ClearRemovesAllPresetKeys`,
  `GarbageValuesClampToKnownClosedSets`, `NoRawApiKeyIsPersisted`.

Invariants (Phase 6a review focus):

- Protocol-first, not brand-first: adding another OpenAI- or Anthropic-compatible
  endpoint requires a preset/config, not a new brand-specific product branch. The
  C++ DTO's primary axis is `protocol_family`; `credential_slot` is a slot label,
  not a brand. The two Opencode presets differ only in protocol family + endpoint
  over the same base URL.
- No raw API key in the DTO/controller/QSettings: `AiProviderPreset` has no
  secret field; the controller has no `SetApiKey`/`SetSecret`. `NoRawApiKeyIsPersisted`
  asserts the `ai/preset` group has no key-named setting and no full-`sk-`-key
  value; `maskedKeyLabel` is a short display mask (contains an ellipsis, does not
  match a full-key regex), not key material.
- Advertisement gated on verified capability: the Opencode presets are NOT
  advertised (both model flags false, `live_confirmed` false). A user override
  flipping `live_confirmed=true` advertises the model even with both flags false
  (the live-pin disjunct) — `opencode_presets_not_advertised_until_live_confirmed`
  and `live_confirmed_alone_advertises_without_vision_or_structured_flags` pin both
  sides. The capability-descriptor count stays 6 (3 advertised built-ins × 2); the
  2 Opencode presets add no descriptors.
- OpenRouter demoted: `loads_built_in_configs` asserts the OpenRouter
  `display_name` now contains "legacy".
- Duplicate endpoint-id handling: two user configs sharing an Opencode
  `provider_id` keep the first and skip the second (fail closed, not a silent
  clobber) — `opencode_duplicate_provider_id_is_rejected`.
- Configs are data only: an Opencode-shaped preset carrying a raw `api_key` in an
  unknown field is rejected by the pre-deserialize secret scan —
  `opencode_preset_with_injected_raw_secret_is_rejected`.

Test results:

- Rust - `cargo test -- --skip live_` in `rust/puerh_mind`: 196 passed; 0 failed;
  0 ignored; 5 filtered out (the env-gated live smokes). The 5 new
  `provider_config` tests (`opencode_presets_are_https_with_no_raw_secret`,
  `opencode_preset_with_injected_raw_secret_is_rejected`,
  `opencode_duplicate_provider_id_is_rejected`,
  `opencode_presets_not_advertised_until_live_confirmed`,
  `live_confirmed_alone_advertises_without_vision_or_structured_flags`) are green.
  The two updated count tests (`loads_built_in_configs` 3→5 built-ins with Opencode
  field assertions; `built_ins_advertise_understanding_and_rating_descriptors`
  count stays 6 + Opencode-not-advertised assertions) are green. The
  `capabilities.rs` / `ai_runtime.rs` `assert!(caps.len() >= 5)` lower bounds still
  hold (count is 6).
- C++ MSVC build (PowerShell tool): `AiProviderPreset` lib + `AiProviderPresetTest`
  built clean (AUTOMOC ran; no warnings). ctest `-R "AiSidecarRuntimeServiceTest|
  AiProviderPresetTest|AiStorageControllerTest"`: 42/42 passed (1 live-runtime
  Skip — `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` not set). The 5 `AiProviderPresetTest`
  cases pass. No existing C++ source changed, so no downstream rebuild was
  required beyond the new target.

Deferred to Phase 6b / 6c / 6d / 6f:

- The generic `openai_chat_compatible` driver (6b): the `opencode_go_openai`
  preset is config-only until 6b wires it. `build_real_image_providers` skips it
  with `warn!` at sidecar startup (reserved driver, fail closed) — expected, not a
  regression. The `opencode_go_anthropic` preset IS usable today (the
  `anthropic_messages` driver is wired from the 5c follow-up) but is not
  advertised as image-analysis capable until live-confirmed.
- Opencode Zen (`https://opencode.ai/zen/v1`) parallel presets: deferred until the
  user's account/model/endpoint access is confirmed (Open Decision: Go vs Zen). The
  Go presets are the documented primary path.
- `AiCredentialStore` (OS credential store), "validate connection" dry run, and
  runtime-handle revoke (6c) — the controller persists only non-secret metadata;
  the secret flow is 6c.
- AlbumBackend/QML member wiring of `AiProviderPresetController` (6d) — 6a ships
  the controller as a standalone, testable QObject in its own lib; it is not yet a
  member of `AlbumBackend` and has no QML exposure. The constructor takes only
  `QObject* parent` so 6d can adopt it without rework.
- Live smoke matrix against Opencode (6f) — the `live_confirmed` flag on the
  Opencode models stays false until a 6f live smoke pins it; only then do the
  Opencode presets advertise as image-analysis capable.

Build note for the next handoff: rebuild the release sidecar binary
(`cargo build --release --bin alcedo_mind` in `rust/puerh_mind`) after picking up
these Rust config changes — the two Opencode presets are embedded via
`include_str!`, so a stale binary will not list them in `BUILTIN_PROVIDER_CONFIGS`
and a 6f Opencode live smoke would not find its preset. Run MSVC builds through
the PowerShell tool (not Bash), and build affected test targets explicitly before
ctest. For an offline cargo run with `.env.test` present, use
`cargo test -- --skip live_` so the env-gated live smokes do not turn the suite
red.

Review conclusion: bugs found — none (the one test failure during implementation
was a test-honesty gap in `NoRawApiKeyIsPersisted`, which asserted no persisted
value starts with `sk-` and was tripped by the legitimate `maskedKeyLabel`
display mask `sk-…4f2a`; fixed by asserting against a full-key regex
`sk-[A-Za-z0-9_-]{20,}` that a short ellipsis-containing mask does not match);
risk accepted — (1) the `opencode_go_openai` built-in names the reserved
`openai_chat_compatible` driver, so `build_real_image_providers` skips it with
`warn!` at sidecar startup until 6b wires the generic driver — this is the
intended 6a/6b phase split, and the preset is NOT advertised (model unverified),
so there is no advertisement/registration mismatch; (2) the Opencode default
model slugs (`claude-sonnet-4-5`, `gpt-4o`) are unverified placeholders the user
must edit — the presets are not advertised until `live_confirmed` flips, so a
wrong default cannot drive an image-analysis job; missing tests — none.

## Phase 6b - Completion & Self-Review

Status: complete. The two compatible-protocol drivers are now generic over the
configured base URL/endpoint, not coupled to any provider brand. The OpenRouter
Chat implementation is refactored into a generic `openai_chat_compatible` driver
(`OpenAiChatCompatibleProvider`); `openrouter_chat` and `openai_chat_compatible`
both dispatch to it, and `OpenRouterChatProvider` is now a type alias so all
existing call sites (`build_one`, `live_smoke`, the OpenRouter test suite) compile
unchanged. OpenRouter's routing/attribution knobs (`attribution_headers`,
`structured_output.provider_require_parameters`, per-model `data_collection`) are
read from the provider config and only emitted when set, so they are on for the
OpenRouter built-in and off by default for the Opencode / plain OpenAI-compatible
preset — "disabled for Opencode by default" is automatic from the config, not a
code branch. `AnthropicMessagesProvider` was already URL-agnostic; Phase 6b
decoupled its module doc from the Volcengine Ark Coding Plan name, made
provider-request-id capture work from EITHER a response header (Opencode echoes
`request-id`) OR a body JSON pointer (Coding Plan embeds `id`), and added
Opencode-style mock-server tests proving the driver accepts the Opencode base URL
(`/messages`) from config. Both compatible drivers build request bodies from the
code-owned Alcedo task schemas (config selects protocol/endpoint only — it owns no
prompt text, response schema, or business fields). An endpoint that ignores
`response_format` / tool-use and returns non-JSON prose, or returns JSON that
violates the code-owned contract, maps to `ProviderError::SchemaValidation` so the
service creates no active annotation (the explicit "unsupported structured output"
fail-closed path).

Implemented (file-by-file, per the plan):

- `rust/puerh_mind/src/service/providers/openai_chat_compatible.rs` - NEW. The
  generic OpenAI Chat-compatible driver, moved from `openrouter.rs` and renamed
  `OpenAiChatCompatibleProvider`. All OpenRouter-specific knobs are config-gated
  (`provider_knobs` emits `require_parameters` / `data_collection` only when the
  config sets them; `attribution_headers` forwards the config map, empty for
  Opencode; the `provider` object is omitted entirely when empty). Uses the shared
  `read_response` + `extract_provider_request_id` so the provider request id is
  captured from a header OR a body pointer. 12 new mock-server tests cover the
  Opencode preset shape: bearer auth with NO attribution/routing for Opencode,
  `response_format: json_schema` + image data URI, understanding/rating parse +
  usage capture, 429 transient / 500-retry / 4xx-no-retry mapping, the explicit
  ignored-`response_format` fail-closed path, contract-violation fail-closed,
  missing-credential, header-based provider-request-id capture, and the
  current_thread-runtime no-leak redaction test.
- `rust/puerh_mind/src/service/providers/openrouter.rs` - reduced to a module doc
  + `pub type OpenRouterChatProvider = super::openai_chat_compatible::OpenAiChatCompatibleProvider;`.
  The full OpenRouter test suite is retained unchanged (it constructs
  `OpenRouterChatProvider` via the alias and asserts the OpenRouter-specific knobs
  are still emitted for the OpenRouter built-in config), with its test-module
  imports made explicit (`serde_json`, `ImageAnalysisProvider`, `ProviderError`,
  `validate_*`) since the top-level re-exports moved with the implementation.
- `rust/puerh_mind/src/service/providers/anthropic_messages.rs` - module doc
  rewritten to state the driver is generic and NOT coupled to the Coding Plan
  (Opencode / Anthropic / Ark Coding selected by `base_url`/`endpoint`/
  `credential_slot`); `parse_describe`/`parse_score` now take the
  already-extracted `header_req_id` and the trait impl uses `read_response` +
  `extract_provider_request_id` so Opencode's `request-id` response header is
  captured (previously only the body `id` pointer was read, which would have
  returned empty for the Opencode preset). 4 new Opencode-style tests:
  `/messages` request shape with tool-use + image block (no `provider`/
  `response_format` leak, no `x-api-key` in bearer mode), tool-use extraction +
  header-based provider-request-id, missing-tool-use fail-closed, and 4xx raw-body
  redaction.
- `rust/puerh_mind/src/service/providers/http_util.rs` - two new shared helpers:
  `extract_provider_request_id(headers, body, header_name, json_pointer)` (header
  takes precedence, then pointer, then empty — the field stays optional end to
  end) and `read_response(resp) -> (HeaderMap, Value)` (clones headers before
  consuming the body so the id can come from either source; non-JSON body →
  `SchemaValidation`). 1 new unit test (`extract_provider_request_id_prefers_header_then_pointer`).
- `rust/puerh_mind/src/service/providers/mod.rs` - `pub mod openai_chat_compatible`
  added; `build_one` now has an `"openai_chat_compatible" | "openrouter_chat"` arm
  dispatching to `OpenAiChatCompatibleProvider::new` (so the `opencode_go_openai`
  preset is now registered, not skipped-with-warn as in 6a). Module doc updated
  to describe the three protocol-family drivers.
- `rust/puerh_mind/src/service/image_analysis.rs` - trait doc updated to mention
  the Phase 6b generic `OpenAiChatCompatibleProvider` and that
  `OpenRouterChatProvider` is now a type alias.

Invariants (Phase 6b review focus):

- Protocol-first, not brand-first: adding another OpenAI- or Anthropic-compatible
  endpoint requires a preset/config, not a new brand-specific driver branch. Both
  compatible drivers select behavior purely from `ProviderConfig` fields
  (`base_url`, `endpoint`, `auth_type`, `attribution_headers`,
  `provider_require_parameters`, `provider_request_id_header`/`_json_pointer`).
  The Opencode and OpenRouter OpenAI-compatible presets differ only in config over
  the same `openai_chat_compatible` driver; the Opencode and Coding Plan
  Anthropic presets differ only in config over the same `anthropic_messages`
  driver.
- OpenRouter knobs optional and off for Opencode by default:
  `sends_bearer_authorization_without_attribution_or_routing_for_opencode` asserts
  no `HTTP-Referer`/`X-OpenRouter-Title` headers and no `provider` routing object
  are sent for the Opencode preset, while
  `request_body_has_structured_output_and_require_parameters` (OpenRouter suite)
  still asserts both are present for the OpenRouter config.
- No active annotation on unsupported structured output: the explicit fail-closed
  path is pinned by `ignored_response_format_produces_no_active_annotation`
  (OpenAI-compatible: 200 with prose content → `SchemaValidation`),
  `json_violating_contract_maps_to_schema_validation` (valid JSON, empty caption),
  `opencode_missing_tool_use_maps_to_schema_validation` (Anthropic: 200 with only
  a text block), and the existing `missing_tool_use`/`wrong_tool_name` Coding Plan
  tests. `ensure_structured_output` still fails closed before any HTTP call when
  `mode = "none"` or the model lacks `supports_structured_output`.
- Provider request id is optional and source-agnostic:
  `provider_request_id_captured_from_response_header_when_configured` (OpenAI
  driver, header override),
  `opencode_extracts_tool_use_and_captures_request_id_header` (Anthropic driver,
  Opencode `request-id` header), and the OpenRouter/Coding-Plan body-`id` tests
  (unchanged) cover both sources; `extract_provider_request_id_prefers_header_then_pointer`
  pins the precedence. An unreported id yields an empty string, not an error.
- Redaction preserved: the current_thread-runtime no-leak test is duplicated in
  the new `openai_chat_compatible` module (asserts no secret / image data-URI /
  prompt / raw-body in captured logs or the error string), and
  `opencode_client_4xx_redacts_raw_body` covers the Opencode Anthropic 4xx path.
  The shared `send_with_retry` still drains and never logs the body; the secret is
  still `expose()`d only at the single header-build site per driver.
- Existing OpenRouter and Volcengine tests stay green: the OpenRouter suite runs
  unchanged against the alias; the Volcengine Ark Responses driver was not touched
  (its `provider_request_id` still comes from the `/id` body pointer via the same
  shared `extract_provider_request_id` helper — behavior preserved because its
  config sets `provider_request_id_json_pointer: "/id"`, `provider_request_id_header: null`).

Test results:

- Rust - `cargo test -- --skip live_` in `rust/puerh_mind`: 213 passed; 0 failed;
  0 ignored; 5 filtered out (the env-gated live smokes). 196 (Phase 6a baseline)
  + 17 new tests: 12 in `openai_chat_compatible`, 4 Opencode tests in
  `anthropic_messages`, 1 `extract_provider_request_id` unit test in `http_util`.
  The existing OpenRouter tests, the existing Anthropic Coding-Plan tests, and the
  Volcengine Ark tests all stayed green. `cargo build --tests` is warning-free.
- No C++ changes in Phase 6b (the deliverables are Rust driver code only); no C++
  rebuild or ctest run was required for this phase.

Deferred to Phase 6c / 6d / 6f / 6g:

- `AiCredentialStore` (OS credential store), "validate connection" dry run, and
  runtime-handle revoke (6c) — the secret flow is still env/vault-only; the
  compatible drivers resolve the credential from the vault per request and never
  persist it.
- AlbumBackend/QML job wiring of the Opencode presets (6d) — the presets are
  registered (callable by `provider_id`) but not advertised (models unverified),
  so no album job can pick them as a default capability yet.
- Live smoke matrix against Opencode (6f) — the `live_confirmed` flag on the
  Opencode models stays false until a 6f live smoke pins it; only then do the
  Opencode presets advertise as image-analysis capable.
- Retire `openrouter_chat` as a product-facing driver id (6g) — `openrouter_chat`
  and `openai_chat_compatible` currently dispatch to the same implementation; 6g
  may collapse to the single generic id once OpenRouter is demoted to an optional
  compatible preset.

Build note for the next handoff: rebuild the release sidecar binary
(`cargo build --release --bin alcedo_mind` in `rust/puerh_mind`) after picking up
these Rust driver changes — the `openai_chat_compatible` driver is new code, so a
stale binary will not register the `opencode_go_openai` preset (it would fall to
the reserved-driver `warn!` skip path) and a 6f Opencode live smoke would fail to
find a usable provider. Run MSVC builds through the PowerShell tool (not Bash) if
a later phase touches C++. For an offline cargo run with `.env.test` present, use
`cargo test -- --skip live_` so the env-gated live smokes do not turn the suite
red.

Accepted risk (Phase 6a → 6b state change): in Phase 6a the Opencode presets were
"advertised-but-unregistered" (config named a reserved driver, `build_one` skipped
it with `warn!`). Phase 6b wires the generic drivers, so the Opencode presets are
now "registered-but-not-advertised" (callable by an explicit `provider_id`, but no
capability descriptor because the bundled models ship with `supports_vision=false`
/ `supports_structured_output=false` / `live_confirmed=false`). `ListCapabilities`
does not surface them. Follow-up review correction: a request using the bundled
default model still fails closed at `ensure_structured_output`, but a non-empty
explicit `model_id` that is not present in `config.models[]` currently resolves to
`model = None` and bypasses the model capability check before the provider call.
Phase 6c must close this by adding model discovery/listing and requiring explicit
model ids to resolve to a known built-in or discovered/persisted model entry before
any image-analysis HTTP request.

Review conclusion update: the original self-review said "bugs found — none", but a
post-review found one Phase 6c-bound gap: unknown explicit `model_id` values are
not required to exist in `config.models[]`, so they can bypass the current
`supports_structured_output` check. Treat this as a follow-up correctness/safety
item, not as a Phase 6b driver-shape blocker. The `openrouter_chat` and
`openai_chat_compatible` driver ids still intentionally dispatch to the same
implementation in 6b; the deduplication to a single id is deferred to 6g. The
"unsupported structured output" failure path still reuses
`ProviderError::SchemaValidation`; the required outcome is no active annotation.

## Phase 6c - Completion & Self-Review

Status: complete for the backend slice. Phase 6c now has the credential-store
boundary, runtime-handle revoke, dry-run model discovery, and the Phase 6b
unknown-explicit-model gap closed before provider HTTP calls. Album/QML wiring
remains Phase 6d, and live capability pinning remains Phase 6f.

Implemented:

- `rust/puerh_mind/proto/ai_runtime.proto` +
  `rust/puerh_mind/src/server/ai_runtime.rs`: added
  `AiRuntimeService.RevokeCredential`. Revoke is idempotent, reports whether a
  live handle was removed, and never logs/echoes the handle or secret material.
- `rust/puerh_mind/proto/image_analysis.proto` +
  `rust/puerh_mind/src/server/image_analysis.rs`: added
  `ImageAnalysisService.ListModels`, returning provider-independent
  `DiscoveredModel` candidates as an unverified dry run. Credential resolution,
  timeout handling, provider errors, and missing/unknown provider mapping reuse
  the typed AI response header path.
- Rust provider layer: `openai_chat_compatible` and `anthropic_messages` now
  implement `/models` discovery using the configured auth convention and optional
  `models_endpoint` override; Anthropic-compatible discovery handles bounded
  cursor pagination. `http_util::send_get_with_retry` applies the same
  retry/redaction policy as task calls. `provider_config` validates
  `models_endpoint`.
- Rust model safety: OpenAI-compatible, Anthropic-compatible, and Volcengine
  drivers now reject a non-empty explicit `model_id` unless it resolves to a
  known `config.models[]` entry, before any image-analysis HTTP request. Empty
  `model_id` continues to mean the config-authored default model.
- C++ runtime bridge: `AiSidecarRuntimeService` / `GrpcAiSidecarRuntimeClient`
  expose `RevokeCredential` and `ListModels`; ready guards map unavailable
  sidecar state into failed typed results instead of falling back.
- C++ credential store: added `AiCredentialStore` with a Windows Credential
  Manager implementation (`AlcedoStudio/AiCredential/<slot>`) and an in-memory
  fallback for tests/non-Windows. Slot labels are constrained to `[a-z0-9_]+`;
  error strings never include secret material. The new target links `Advapi32`
  on Windows.
- C++ validation flow: `ImageAnalysisService::ValidateConnection` loads the
  secret from `IAiCredentialStore`, registers a short-lived sidecar vault handle,
  runs `ListModels`, and revokes the handle even on model-list failure or thrown
  RPC. Normal image-analysis jobs also revoke their registered handle on job
  completion/cancel/error.

Invariants (Phase 6c review focus):

- No raw API key in QSettings, process args, request DTOs, logs, result DTOs, or
  packed project paths changed in this phase. The raw key enters C++ only through
  `IAiCredentialStore::LoadCredential`, then through `RegisterCredential`; task
  RPCs and discovery use only the opaque vault handle.
- Model discovery is not capability advertisement. `DiscoveredModel` carries id,
  display name, and source provider only; image input / structured output remain
  unverified until Phase 6f live smoke pins them.
- No paid image-analysis call can bypass structured-output checks with an
  unknown explicit model id. The provider drivers fail locally with
  `ProviderError::UnknownModel`, mapped to `AI_STATUS_INVALID_ARGUMENT`.
- Runtime handles are revoked at the two host-managed lifetimes available in
  this backend slice: validate-connection dry runs and `ImageAnalysisService`
  jobs. Revoke is idempotent and benign when the sidecar is already stopped.

Test results:

- Rust - `cargo test -- --skip live_` in `rust/puerh_mind`: 230 passed; 0
  failed; 0 ignored; 7 filtered out. This includes the new RevokeCredential,
  ListModels, `/models` discovery, redaction, pagination, retry, and
  unknown-explicit-model tests.
- C++ - built with `cmd /c scripts\msvc_env.cmd --build --preset win_debug
  --target AiCredentialStoreTest ImageAnalysisServiceTest
  AiSidecarRuntimeServiceTest --parallel 4`: succeeded.
- C++ - `ctest --test-dir build/debug --output-on-failure -R
  "AiCredentialStoreTest|AiSidecarRuntimeServiceTest|ImageAnalysisServiceTest"`:
  53/53 passed, with 1 live-runtime test skipped because
  `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` was not set.

Accepted risk / deferred:

- Non-Windows still uses the in-memory fallback from `MakeDefaultAiCredentialStore`;
  native macOS Keychain and Linux Secret Service/KWallet stores are deferred. The
  Windows production path is backed by Credential Manager.
- The host currently returns discovered model candidates to callers but does not
  yet persist/merge them into a generated user provider config. That merge point
  is intentionally left for the Phase 6d/6f settings and live-validation flow.
- AlbumBackend/QML exposure of provider settings, credential save/delete buttons,
  and validate-connection UI remain Phase 6d. The backend contract is ready for
  that wiring.

## Phase 6d - Completion & Self-Review

Status: complete for the backend slice. Remote image analysis is now drivable
from the album workflow through a **standalone** QML-exposed module
(`ImageAnalysisController`), NOT inlined into `album_backend.cpp` — it mirrors
the cleanly-factored QObject sub-controller pattern
(`SemanticGenerationController` / `ModelDownloadController`): own hpp/cpp under
`album_backend/`, held as an `AlbumBackend` member, surfaced to QML via
`Q_PROPERTY(QObject* imageAnalysisController ... CONSTANT)`. The controller is
unit-testable with fakes through an `IImageAnalysisEnvironment` seam, so it has
no direct `AlbumBackend` dependency. The `AiProviderPresetController` (Phase 6a,
shipped standalone) is now also an `AlbumBackend` member exposed to QML, and
gained a `provider_id` field so a job can select a registered sidecar provider
config. The sidecar starts on demand with `require_model_info=false`; remote
calls serialize through one shared `ImageAnalysisInFlightGate` owned by
`AlbumBackend`; the Phase 5d encoded-rendition path is reused with a new
`max_image_bytes` cap sourced from the preset. **No QML UI this phase** (the
controller is QML-callable; menu actions / progress dialog deferred). **No
combined describe+rating run was implemented in this historical 6d slice**; the
2026-06-28 protocol correction above supersedes that as a future direction:
multi-output analysis must be one provider request per image. **No database
writes** (persistence + search refresh now moves to Phase 7a, after the
sidecar-client refactor), so a cancelled or failed run leaves no active
annotation trivially — failed/canceled items are never counted as `analyzed`.

Implemented (file-by-file, per the plan):

- `alcedo_studio/src/include/ui/alcedo_main/album_backend/image_analysis_controller.hpp`
  + `.../image_analysis_controller.cpp` - NEW standalone module. `IImageAnalysisEnvironment`
  (ThumbnailProvider / AnalysisClient / CredentialStore / Gate / EnsureSidecarReady)
  is the testability seam. `ImageAnalysisController : public QObject` with
  `Q_PROPERTY`s (`running`, `total`/`analyzed`/`failed`/`canceled`, `statusText`,
  `lastError`, `canRetry`, `providerConfigured`, `credentialAvailable`,
  `lastResults`) and `Q_INVOKABLE` `StartDescribeForTargets` /
  `StartScoreForTargets` / `CancelAnalysis` / `RetryLast` / `ValidateConnection` /
  `RefreshCredentialState`, one shared `StateChanged()` NOTIFY. Selection is a
  `QVariantList` of `{elementId, imageId}` maps (the
  `ImportExportHandler::CollectExportTargets` convention); **empty selection is a
  no-op with a clear error** (paid-call safety; never falls back to "whole view").
  Job flow: parse → read preset (`provider_id`/`model_id`/`credential_slot`/
  `timeout_ms`/`max_image_bytes`) → `LoadCredential` from the OS store →
  `EnsureSidecarReady` (`require_model_info=false`) → build `ImageAnalysisService`
  with the SHARED gate → `StartAnalysis` with `QPointer`+`Qt::QueuedConnection`
  progress/finished marshalling (mirrors `semantic_generation_controller.cpp:897–925`).
  `Finish` tallies counts + builds `lastResults` for QML; failed/canceled items
  never count as `analyzed`. `ValidateConnection` reuses the 6c dry-run
  (`ImageAnalysisService::ValidateConnection`) off the QML thread. `RetryLast`
  re-runs the last `(targets, task)` (controller-level retry; no new host-side
  HTTP retry — the bounded retry lives in the Rust `http_util` driver behind the
  gate).
- `alcedo_studio/src/ui/alcedo_main/album_backend/album_backend.cpp` - the
  production `AlbumImageAnalysisEnvironment` (lazy `ThumbnailServiceImageAnalysisProvider`
  / `AiSidecarRuntimeImageAnalysisClient` / `MakeDefaultAiCredentialStore` /
  shared `image_analysis_gate_`; `EnsureSidecarReady` starts the sidecar
  `require_model_info=false`, 60s startup timeout, checks only `state==kReady`)
  and the `MakeAlbumImageAnalysisEnvironment` factory. Declared a friend of
  `AlbumBackend`. The controller class itself has no `AlbumBackend` coupling.
- `alcedo_studio/src/include/ui/alcedo_main/album_backend/album_backend.hpp` -
  new members (`AiProviderPresetController ai_provider_preset_`, the shared
  `std::shared_ptr<ImageAnalysisInFlightGate> image_analysis_gate_`,
  `ImageAnalysisController image_analysis_`), two new `Q_PROPERTY(QObject* ...)`
  + inline getters, `friend class ImageAnalysisController` +
  `friend class AlbumImageAnalysisEnvironment`. Constructor init-list order:
  `ai_provider_preset_(this)` → `image_analysis_gate_(make_shared<...>())` →
  `image_analysis_(MakeAlbumImageAnalysisEnvironment(*this), &ai_provider_preset_)`.
- `alcedo_studio/src/include/app/ai_provider_preset.hpp` + `.../ai_provider_preset.cpp` -
  `AiProviderPreset` gained `QString provider_id` (the configured endpoint id the
  sidecar RPC selects, e.g. `opencode_go_anthropic`); `Q_PROPERTY providerId` /
  `ProviderId()` / `Q_INVOKABLE SetProviderId`; settings key `ai/preset/providerId`;
  default `opencode_go_anthropic` (matches the 6a Opencode Anthropic default
  protocol/base_url); sanitized as a non-secret string. `provider_id` is NOT a
  secret and is exempt from the raw-key scan.
- `alcedo_studio/src/include/app/image_analysis_service.hpp` +
  `.../image_analysis_service.cpp` - `ImageAnalysisOptions` gained
  `int64_t max_image_bytes = 0` (0 = no cap). In `RunJob`'s producer, after a
  successful encode, if `max_image_bytes > 0 && encoded.bytes.size() > max_image_bytes`,
  the item is marked `kPrepFailed` with a redacted size error — no provider call,
  no pin held (the encode already released the guard). This is the "cap image
  bytes from the selected preset" deliverable; `prefetch` stays at the 5e-bounded
  default (1).
- `alcedo_studio/src/CMakeLists.txt` - new `def_library(ImageAnalysisController ...)`
  (PUBLIC_DEPS `ImageAnalysisService AiProviderPreset AiCredentialStore
  UiLocalization Qt6::Core`); `AlbumBackendLib` links it (plus the direct
  `ImageAnalysisService`/`AiProviderPreset`/`AiCredentialStore` it uses in
  `album_backend.cpp`); the controller source is NOT in `AlbumBackendLib`'s source
  list so the test can link the controller without pulling all of AlbumBackend.
- `alcedo_studio/tests/CMakeLists.txt` - new `ImageAnalysisControllerTest` target
  (links `ImageAnalysisController AiProviderPreset AiCredentialStore
  GTest::gtest_main`), label `ci_core_flow`; added to the `ci_core` category.
- `alcedo_studio/tests/app/image_analysis_controller_test.cpp` - NEW. A fake
  `IImageAnalysisEnvironment` (synchronous `FakeThumbProvider` +
  `FakeImageAnalysisClient` with configurable outcome / block mode +
  `InMemoryAiCredentialStore` + a shared gate + a `SidecarEnsured()` flag) and a
  real `AiProviderPresetController` on temp `QSettings`. A `QCoreApplication` is
  created in `SetUpTestSuite` so the controller's `Qt::QueuedConnection`
  progress/finished marshalling delivers. 10 cases (the 6d-required set):
  `EmptySelectionSetsErrorAndDoesNotStart`, `OneImageDescribeSucceeds`,
  `MultiImageDescribeSucceeds`, `CancelRunningAnalysisDiscardsResult`,
  `RetryLastReRunsTargets`, `ProviderErrorPropagatesAndNoActiveAnnotation`,
  `SchemaErrorPropagatesAndNoActiveAnnotation`,
  `MissingCredentialSetsErrorAndDoesNotStart`, `ScoreTaskReturnsRating`,
  `SharedGateSerializesTwoConcurrentRuns` (two controllers over one shared env
  prove the gate serializes app-wide — the 6d mandate). `WaitForFinished` /
  `SpinWaitFor` pump the event loop with a timeout so a leaked gate cannot hang
  the suite.
- `alcedo_studio/tests/app/image_analysis_service_test.cpp` - NEW
  `OversizedImageBytesRejectedBeforeProviderCall` (preset cap = 1 byte < the
  fixture JPEG → item `kError` with "exceeds preset limit", `DescribeCalls() == 0`).
- `alcedo_studio/tests/app/ai_provider_preset_test.cpp` - `provider_id` added to
  `RoundTripsEveryEditableField` / `IndividualSettersPersistAndReload` /
  `ClearRemovesAllPresetKeys` (after Clear it falls back to the
  `opencode_go_anthropic` default, not empty — it is the endpoint selector).

Invariants (Phase 6d review focus):

- Standalone module, not inlined: `ImageAnalysisController` is its own hpp/cpp
  + lib; `AlbumBackend` only holds it as a member and exposes it via
  `Q_PROPERTY`. The production env (`AlbumImageAnalysisEnvironment`) lives in
  `album_backend.cpp`; the controller class has no `AlbumBackend` dependency, so
  the test links only `ImageAnalysisController` + fakes.
- Shared gate app-wide: `AlbumBackend` owns one `image_analysis_gate_`; the env
  returns it; the controller passes it to every `ImageAnalysisService` it builds
  (never `nullptr`, never a private gate). `SharedGateSerializesTwoConcurrentRuns`
  proves two concurrent controller runs serialize.
- Sidecar on demand, `require_model_info=false`: `EnsureSidecarReady` starts the
  sidecar only when a job is launched and only checks `state==kReady` (not
  `model_info`, which is unpopulated). Ordinary browsing/search never starts the
  sidecar. `MissingCredentialSetsErrorAndDoesNotStart` proves the sidecar is NOT
  touched when the credential is absent.
- No DB writes / no partial active annotation: the controller performs NO
  persistence (now Phase 7a); `Finish` only tallies counts and builds `lastResults`.
  Failed/canceled items are counted `failed`/`canceled`, never `analyzed`.
  `ProviderErrorPropagatesAndNoActiveAnnotation` +
  `SchemaErrorPropagatesAndNoActiveAnnotation` +
  `CancelRunningAnalysisDiscardsResult` assert `analyzed==0` on failure/cancel.
- Credential handling (Phase 3/6c invariant preserved): the secret is loaded via
  `IAiCredentialStore::LoadCredential` (OS store on Windows), placed into
  `ImageAnalysisOptions::credential.secret`, and the 5d `RunJob` registers it
  once, zeroizes the local copy, threads only the opaque handle, and revokes on
  job end. The controller never logs/stores the secret; `provider_id` is a
  non-secret endpoint id.
- Encoded-rendition path reused + capped: k1024 / JPEG q90 / OIIO encoder (5d)
  unchanged; `max_image_bytes` from the preset rejects oversized payloads before
  the provider call (`OversizedImageBytesRejectedBeforeProviderCall`).
- Sidecar startup remains on demand and normal browsing/search do not require
  API keys: confirmed — the controller only reaches `EnsureSidecarReady` after a
  non-empty selection + valid preset + present credential.

Test results:

- C++ MSVC build (PowerShell tool, per project memory): `--target
  ImageAnalysisControllerTest ImageAnalysisServiceTest AiProviderPresetTest
  AlbumBackendLib alcedo_main` built clean (one typo fixed during implementation:
  `<Q_OBJECT>` → `<QObject>` in the new header, caught at compile time before any
  test ran; and a link-layer refactor — the controller was moved from the
  `AlbumBackendLib` source list into its own `ImageAnalysisController` lib so the
  test can link it without pulling all of AlbumBackend).
- ctest 6d group (`-R "ImageAnalysisControllerTest|ImageAnalysisServiceTest|
  AiProviderPresetTest"`): 43/43 — 42 passed, 1 Skipped (the pre-existing
  `ImageAnalysisServiceLiveTest.ValidateConnectionDiscoversOpencodeModels` live
  smoke, env-gated). The 10 `ImageAnalysisControllerTest` cases pass; the new
  `OversizedImageBytesRejectedBeforeProviderCall` passes; the `AiProviderPresetTest`
  `provider_id` additions pass.
- ctest regression (`-R "SemanticGenerationServiceTest|FilterServiceTest|
  AiStorageControllerTest|AiSidecarRuntimeServiceTest"`): all green (1 pre-existing
  live-runtime Skip — `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` not set). No regression
  in semantic/storage/search.

Next phase / renumbering note:

- Phase 6e is now the `sidecar_client` module refactor. It should run before any
  storage/search persistence work, because `AiSidecarRuntimeService` is currently
  carrying the gRPC client, DTO mapping, credential vault calls, embedding RPCs,
  and image-analysis RPCs in the same file as `QProcess` lifecycle management.
- Persistence, search refresh, rating surface, and usage summary are renumbered
  to Phase 7a. The controller emits `lastResults` only; `AiStorageController`
  wiring and the storage-layer `IsValid()` backstop for "no upsert on failure"
  land after the sidecar client boundary is clean.
- QML UI (menu actions + progress dialog) remains deferred. The controller is
  QML-callable, but has no menu/dialog yet.
- Combined describe+rating was not implemented in this historical 6d slice. The
  2026-06-28 protocol correction above makes multi-output analysis a required
  single-request-per-image shape for future UI/provider work.
- Live Opencode smoke and capability pinning move to Phase 7b. `live_confirmed`
  stays false on the Opencode models; the controller's `ValidateConnection`
  dry-run is the closest 6d gets to live provider contact.
- Merging discovered model candidates into preset state moves to Phase 7. It was
  not done in 6d; `ValidateConnection` surfaces candidates in `lastError` text
  only.

Build note for the next handoff: after any Rust image-analysis change, rebuild
the release sidecar (`cargo build --release --bin alcedo_mind`) before a live run
(not required for the 6d offline tests). Run MSVC builds through the PowerShell
tool (not Bash). For an offline cargo run with `.env.test` present, use
`cargo test -- --skip live_`. The new `ImageAnalysisController` lib must be
linked by any future consumer; `AlbumBackendLib` already does.

Review conclusion: bugs found — none (the `<Q_OBJECT>` typo and the link-layer
lib split were caught at compile/link time before any test ran); risk accepted —
(1) no QML UI this phase (controller is QML-callable, no menu/dialog — deferred),
(2) no persistence (Phase 7a); the "no upsert on failure" guarantee is
controller-level only ("nothing persisted" + failed/canceled never `analyzed`),
with the storage-layer `IsValid()` backstop landing after the sidecar-client
cutover, (3) combined describe+rating not implemented in this historical 6d
slice but superseded by the 2026-06-28 single-request-per-image correction,
(4) `ValidateConnection`
spawns a detached `std::thread` per call (acceptable for a dry-run; not
exercised by UI this phase), (5) `max_image_bytes`
rejects oversized items as `kPrepFailed` (error, not retried) — matches the
fail-closed posture; missing tests — none: all 6d-required controller cases
(empty selection, one-image success, multi-image success, cancel, retry, provider
error, schema error, no partial active annotation after failure) plus the
shared-gate serialization and missing-credential cases are implemented and green,
and the 6d-adjacent regression suite is green.
