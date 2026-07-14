# Semantic Generation and Search Integration Plan

Date: 2026-06-12

Primary roadmap owner: `alcedo_studio/src/ai` (with `app`, `storage`, `ui`, and Rust sidecar consumers)

Status: Phase 1 complete; Phase 2 complete; Phase 3 complete; Phase 4a
initial scaffold complete; Phase 4c complete; Phase 4d complete; Phase 5a
complete; Phase 5b complete; Phase 5c complete; Phase 5d complete; Phase 5e
complete; Phase 5f complete; Phase 5g deferred; Phase 6a
complete; Phase 6e Qt wiring complete; Phase 6 planning updated

This document proposes how to integrate `rust/puerh_mind` into Alcedo Studio as
project-level semantic image generation and semantic search services.

## Current State

`rust/puerh_mind` is already a local gRPC inference server. Its active engine is
the ONNX Runtime MobileCLIP path, not the older Candle files:

- `proto/semantic.proto` exposes unary `Ping`, `EmbedText`, and `EmbedImage`.
- `SemanticService` already batches image requests internally before calling the
  engine.
- `OrtClipEngine` loads MobileCLIP text and vision ONNX models, produces
  512-dimensional normalized embeddings, and accepts 256-pixel image inputs.
- Python demos start the server through `cargo run --release`, build text-label
  prototypes, classify images by cosine similarity, and write demo JSON output.

The C++ app already has natural integration points:

- `ImportService` and `ExportService` provide job/progress/cancellation patterns.
- `ThumbnailService` can generate and pin `k256`, `k512`, `k1024`, or `k2048`
  thumbnails and release them after use.
- `SleeveFilterService` already has a `SemanticSearchProvider` interface, but no
  concrete implementation.
- `SearchController` and `GlobalSearchDialog.qml` own the current search-preview
  lifecycle and preview-thumbnail paging.
- Project packaging/checksum code enumerates known database tables explicitly, so
  new semantic tables must be added there.

## Target Architecture

Keep the user-facing services aligned with the planned split:

- `SemanticGenerationService`: bulk image embedding generation and label
  assignment for imported files, selected folders, or the full project.
- `SemanticSearchingService`: text-query embedding, tag search, ranking, and
  integration with `SleeveFilterService::SemanticSearchProvider`.

Add one shared internal service:

- `SemanticRuntimeService`: owns the Rust child process, gRPC client, model
  manifest validation, health checks, startup/shutdown, and runtime diagnostics.

This keeps process management out of both generation and search. Both services
should talk to a narrow async C++ interface rather than spawning the Rust process
themselves.

## Rust Runtime Changes

### Configuration

Replace hardcoded runtime defaults with CLI flags and environment fallbacks:

- `--host`
- `--port`
- `--model-root`
- `--model-id`
- `--revision`
- `--device`
- `--no-download`
- `--batch-cap`
- `--batch-wait-ms`
- `--max-message-bytes`

Default behavior for the packaged app should be validate-only. The Rust service
should not download missing models unless an explicit development flag is set.
The Qt app should own download UX, source selection, progress, and integrity
errors.

### RPC Surface

Keep the existing unary RPCs for debugging and smoke tests, then add explicit
batch/status calls:

- `EmbedTextBatch`
- `EmbedImageBatch`
- `GetModelInfo`
- `GetRuntimeStatus`

`GetModelInfo` should return model id, revision, embedding dimension, expected
image size, provider, model root, and prototype/config hashes. C++ should reject
generation if this does not match the local manifest or database model key.

Batch responses should preserve request IDs and include per-item status. A
partial failure should not fail the whole batch unless the runtime itself is
unusable.

### Execution Providers

Windows should continue to use DirectML, with CPU fallback. DirectML should be
run through a single session worker or otherwise protected from concurrent
`Run` calls on the same session.

macOS should add a Core ML execution-provider option and fall back to CPU when
Core ML is unavailable. Device parsing should distinguish:

- `auto`
- `cpu`
- `directml` / `directml:N`
- `coreml`
- `coreml:all`
- `coreml:cpuandgpu`
- `coreml:cpuonly`

The stale Candle source files should either be removed or clearly quarantined
outside the active build path. The integration should standardize on ONNX
Runtime for this phase.

## Model Identity, Distribution, and Download

The packaged app should not bundle model weights. It should bundle:

- a model manifest JSON
- Hugging Face model id and pinned revision
- required relative files
- expected SHA-256 and byte sizes
- embedding dimension and image input size
- precomputed default text-label prototypes

The active Hugging Face `model_id` is the semantic data compatibility boundary.
Image embeddings, generated labels, and cached label prototypes are valid only
when their stored `model_id` matches the currently selected model. If the user
switches models, old rows must not be read for generation skip logic, ordinary
label search, semantic search, or thumbnail label display. The app should expose
old-model rows as removable semantic data, not silently mix them with active
model results.

The Rust side owns model acquisition and local asset validation. Qt owns only
the settings UX and job orchestration: selected model, endpoint preset/custom
endpoint, optional Hugging Face token, target root, progress display, cancel,
retry, delete, and active-model selection. Qt should send these values to Rust
through explicit model-manager requests instead of implementing Hugging Face
download logic in C++.

Rust downloads into an application data directory resolved by Qt settings, for
example:

- Windows: `%LOCALAPPDATA%/AlcedoStudio/models/...`
- macOS: `~/Library/Application Support/AlcedoStudio/models/...`

Downloader requirements:

- user-configurable endpoint, with a domestic Hugging Face mirror as a preset
- atomic staging directory, then rename on success
- resumable `.part` files where practical
- per-file hash validation before marking the model usable
- clear "missing", "downloading", "ready", and "corrupt" states in settings
- request parameters for endpoint and optional Hugging Face token; tokens should
  not be logged and should not be persisted unless a later credential-store flow
  is deliberately added
- model/profile-driven file manifests instead of MobileCLIP-only hardcoded
  asset paths

The inference runtime should still validate only when loading a model for
generation or search. Downloading should be a separate model-manager job so a
missing model can produce a downloader-ready state instead of preventing the
Rust sidecar from starting.

## C++ Service Design

### SemanticRuntimeService

Responsibilities:

- start and stop the Rust child process through `QProcess`
- choose a free localhost port and pass it to Rust
- pass model root, model id, revision, and device flags
- wait for gRPC health readiness
- expose runtime state to QML/settings
- capture stdout/stderr into the app log
- provide async `EmbedText`, `EmbedImage`, and batch methods
- enforce request timeouts and cancellation
- kill the child tree on app exit or explicit user stop

On Windows, use a Job Object or equivalent child-tree cleanup. On macOS, use a
process group. Stopping should first attempt graceful shutdown, then terminate,
then kill after timeout.

Do not spawn one Rust process per image. Start one local service for an explicit
user action or session, reuse it, and let the Rust side batch requests.

### SemanticGenerationService

Responsibilities:

- enumerate target files after import, selected folder, or full project
- skip images that already have valid embeddings for the same model key unless
  forced
- request a thumbnail from `ThumbnailService`
- pin the thumbnail while encoding and sending it to Rust
- release the thumbnail in every success, failure, and cancellation path
- persist embeddings and label decisions transactionally
- publish progress, failures, and cancellation state to UI

Default thumbnail tier should be `k256` for inference cost. Add a setting to use
`k512` or `k1024` when the user wants semantic generation to also warm preview
cache for search results.

Classification should reuse the Python demo semantics:

- text prototypes are normalized
- image embeddings are normalized
- cosine similarity can be computed as dot product
- label decision keeps best score, second score, margin, and confidence

### SemanticSearchingService

Responsibilities:

- implement `SemanticSearchProvider`
- embed free-text queries through `SemanticRuntimeService`
- search stored image embeddings within the current Sleeve scope
- support direct label search without starting Rust when possible
- return stable result pages for `SearchController`
- expose count or total-result metadata for preview pagination

For folder scoping, reuse the same root/folder semantics as
`ElementController::BuildScopedFileQuery`. Avoid duplicating ad hoc folder SQL.

For applying a semantic search to the album grid, avoid a giant `IN (...)` list
for large result sets. Prefer a temporary or project-local result table keyed by
search token, then let the existing thumbnail model page over that scope.

## Database Schema

Add semantic tables rather than storing vectors inside generic image metadata.

Recommended first schema:

```sql
CREATE TABLE SemanticModel (
  model_key VARCHAR PRIMARY KEY,
  model_id VARCHAR NOT NULL,
  revision VARCHAR NOT NULL,
  embedding_dim INTEGER NOT NULL,
  image_size INTEGER NOT NULL,
  prompt_config_hash VARCHAR,
  asset_manifest_json JSON,
  created_at TIMESTAMP DEFAULT current_timestamp
);

CREATE TABLE SemanticImageEmbedding (
  file_id BIGINT NOT NULL,
  image_id BIGINT NOT NULL,
  model_key VARCHAR NOT NULL,
  embedding BLOB NOT NULL,
  embedding_dim INTEGER NOT NULL,
  thumbnail_resolution INTEGER NOT NULL,
  generated_at TIMESTAMP DEFAULT current_timestamp,
  status VARCHAR NOT NULL,
  error VARCHAR,
  PRIMARY KEY (file_id, model_key)
);

CREATE TABLE SemanticImageLabel (
  file_id BIGINT NOT NULL,
  model_key VARCHAR NOT NULL,
  label VARCHAR NOT NULL,
  score DOUBLE NOT NULL,
  second_label VARCHAR,
  second_score DOUBLE,
  margin DOUBLE,
  confident BOOLEAN NOT NULL,
  top_scores JSON,
  updated_at TIMESTAMP DEFAULT current_timestamp,
  PRIMARY KEY (file_id, model_key)
);

CREATE TABLE SemanticLabelPrototype (
  model_key VARCHAR NOT NULL,
  label VARCHAR NOT NULL,
  prompt_config_hash VARCHAR NOT NULL,
  embedding BLOB NOT NULL,
  synonyms_json JSON,
  PRIMARY KEY (model_key, label, prompt_config_hash)
);
```

Store normalized `float32` vectors in DuckDB-native list/array form rather than
opaque BLOBs. Free-text semantic search must not fetch every stored vector into
C++ for cosine comparison. The storage layer owns a ranked query primitive that:

- validates the query vector against the active model dimension
- reuses `ElementController::BuildScopedFileQuery` for root/folder scope
- joins scoped files to `SemanticImageEmbedding`
- ranks with DuckDB VSS/HNSW using `array_distance` over normalized embeddings
- fetches a bounded VSS candidate pool, applies query-local relevance cutoff,
  then pages over the filtered matches

This keeps the future `SemanticSearchProvider` thin: it embeds text through the
runtime, then asks storage for a ranked page. The application receives only the
page rows, not the full vector corpus.

DuckDB's `vss` extension is required for semantic search. The packaged app must
ship the matching `vss.duckdb_extension`; local DuckDB may recognize the
extension name but still fail `LOAD vss` if the extension file has not been
installed. Storage should therefore:

- require `LOAD vss` at startup or first semantic search
- create and maintain an HNSW index over the fixed-size embedding column
- enable DuckDB's `hnsw_enable_experimental_persistence` setting for on-disk
  project databases before creating the index
- fail with an actionable storage error when the extension or index is missing
- hide the exact-vs-ANN choice behind the storage controller API

The table shape intentionally uses `FLOAT[512]` for the active MobileCLIP model
because DuckDB's HNSW index requires a fixed-size array column. A future
non-512-dimensional model should use a schema migration or a dimension-specific
embedding table rather than degrading back to C++ vector scans.

Required follow-up updates:

- add table creation to `DBController`
- include semantic tables in project data summaries/checksums
- delete semantic rows when files are removed
- include semantic tables in project save/load/package workflows
- add storage tests for create, update, delete cleanup, and package integrity

## Current Database Interaction Notes

This section describes the implementation that exists through Phase 4c, plus
the DB-facing hooks that Phase 4d now calls. It is meant to document behavior,
not necessarily endorse every choice.

### Project open and storage lifetime

- `StorageService` owns one `DBController` plus one long-lived
  `SemanticStorageController`; the semantic controller keeps its own DuckDB
  connection guard for the lifetime of the storage service.
- `DBController::InitializeDB()` runs on both new and existing project
  databases. For existing projects it only runs the semantic schema string,
  then seeds default label query rows.
- The semantic schema uses `CREATE TABLE IF NOT EXISTS` and normal secondary
  indexes for model/file and label lookup. It does not create the HNSW index at
  project open.
- Default label queries are seeded on every project open inside one transaction
  using `INSERT OR REPLACE INTO SemanticLabelQuery`. At the current default
  config this rewrites the small bundled photography label set, not the image
  embedding corpus.
- `SemanticLabelQuery` is project-local seed/config data. It is not passed into
  generation jobs as a per-job input.

### Semantic model registration

- Before a generation workflow starts, the album semantic controller asks the
  runtime for model info, builds a model key from that info, then calls
  `SemanticStorageController::UpsertModel(...)`.
- Model registration is an `INSERT OR REPLACE` into `SemanticModel`.
  Re-running generation for the same model key refreshes that row rather than
  creating a new model row.
- The storage layer currently accepts only 512-dimensional embeddings because
  `SemanticImageEmbedding.embedding` and `SemanticLabelPrototype.embedding` are
  `FLOAT[512]` columns for DuckDB HNSW compatibility.

### Label prototype creation and cache behavior

- At the start of a persistent generation job,
  `EnsureCachedLabelPrototypes(...)` counts label query rows for
  `prompt_config_hash`, then counts existing prototype rows for
  `model_key + prompt_config_hash`.
- If `prototype_count >= query_count`, the job does not call `EmbedText` and
  does not rewrite prototype rows. It proceeds without loading prototype vectors
  into the app process.
- If prototypes are missing, the job reads all label query rows ordered by
  label, embeds each query text via the runtime, validates each returned vector,
  and writes all prototypes through `UpsertLabelPrototypes(...)`.
- `UpsertLabelPrototypes(...)` wraps the group write in one transaction, but
  each row still goes through `UpsertLabelPrototype(...)`, which validates the
  registered model and emits an `INSERT OR REPLACE` for that label.
- After the ensure step, the generation job does not load prototype vectors into
  app memory. Label assignment is deferred to the storage transaction that
  writes each image embedding.

### Per-image generation persistence

- Both the UI controller and `SemanticGenerationService::RunJob()` check
  `HasReadyImageEmbedding(file_id, image_id, model_key, require_label=true)`
  when force-regenerate is false. This causes per-candidate DB reads before
  thumbnail and runtime work.
- Image embedding RPCs are batched, but persistence is per item after each
  batch result is mapped back to its request id.
- For each successful image embedding, the service validates the vector and
  calls `UpsertImageEmbeddingAndAssignLabel(...)`; it does not receive or keep
  prototype vectors.
- `UpsertImageEmbeddingAndAssignLabel(...)` validates the model exists,
  validates the vector dimension and finite/non-zero values, then opens a DuckDB
  transaction.
- The per-image transaction deletes the existing
  `SemanticImageEmbedding(file_id, model_key)` row, deletes the matching
  `SemanticImageLabel(file_id, model_key)` row, inserts the new ready embedding,
  ranks `SemanticLabelPrototype` rows for `model_key + prompt_config_hash` with
  DuckDB's exact `array_inner_product`, writes the top/second/top-N label result
  row, and commits.
- Label assignment intentionally does not use HNSW. The prototype table is tiny,
  and exact DB-side dot product keeps classification deterministic while still
  leaving vector storage and page residency to DuckDB.
- There is no explicit "pending" or "failed" row written for failed/canceled
  generation items. Failed/canceled state is reported through the job result,
  not persisted in the semantic tables.

### Search and vector index behavior

- `SemanticStorageController::SearchImageEmbeddings(...)` validates the query
  vector and calls `EnsureVectorSearchIndex(model_key)` before running the
  ranked query.
- `EnsureVectorSearchIndex(...)` loads DuckDB `vss` lazily, first trying the
  `ALCEDO_DUCKDB_VSS_EXTENSION` path, then packaged executable-adjacent paths,
  then `LOAD vss`.
- On each ensure call it sets
  `hnsw_enable_experimental_persistence = true` and runs
  `CREATE INDEX IF NOT EXISTS idx_semantic_image_embedding_hnsw` on
  `SemanticImageEmbedding USING HNSW (embedding)`.
- The search query passes the user prompt embedding as a fixed `FLOAT[512]`
  vector literal, asks DuckDB VSS/HNSW for the nearest ready image embeddings
  with `array_distance`, then joins those ranked candidates to the folder/root
  scope from `BuildScopedFileQuery(folder_id)`.
- Ranked candidates are filtered in storage before paging. The cutoff is
  query-local: it first looks for a score-curve elbow between strong matches and
  the weak tail, then falls back to a relative span from the top score to the
  sampled candidate background. This avoids a fixed model-specific score floor
  while making weak one-term matches less likely to leak into later pages.
- `ProjectService` registers a concrete `SemanticSearchProvider` for each
  project. The provider starts/acquires the semantic runtime only for submitted
  semantic queries, embeds the user prompt once, validates the returned vector
  against the active model, then calls `SearchImageEmbeddings(...)`.

### Cleanup and packaging participation

- Project data summaries include the semantic tables, so semantic row counts
  participate in the lightweight project summary/check path.
- `DeleteImageEmbeddingsForFiles(...)` exists and deletes embedding and label
  rows for a file-id list, but the current production grep only finds the
  method definition and tests. The normal delete/sync path should be audited
  before assuming semantic rows are always cleaned when files are removed.
- Prototype rows are project/model/config cache data. There is no automatic
  pruning of old `SemanticLabelPrototype` rows when a model key or prompt config
  is superseded.

## Import and UI Flow

Generation should not be silent.

After import finishes and the project has been synced/saved/reloaded, show a
prompt unless the import is part of a repair/reimport path:

- "Start semantic generation now"
- "Always ask"
- "Always start after import"
- "Skip"
- "Never ask"

Remember the choice in `QSettings`. Settings should expose:

- model source URL
- model directory
- model status and download progress
- runtime device
- start/stop runtime buttons
- auto-start preference after import
- semantic generation thumbnail tier
- clear/regenerate semantic data actions

The runtime state should be visible whenever it consumes GPU/CPU memory. The
user must be able to stop it explicitly.

## Search UI Flow

Global search should keep the existing metadata-first behavior as the default.
The user-facing control is a visible toggle named `Semantic Search` / `语义搜索`,
not a required mode picker. When the toggle is off, every query uses the
traditional search path:

- file name, element name, and path matching
- EXIF/document matching, including camera, lens, date, ISO, focal length, and
  aperture
- generated semantic labels/tags as ordinary searchable text once labels exist
- recommendation rows and exact-file application

When the toggle is on, query routing is still conservative:

- Empty text shows normal recommendations and does not start the semantic
  runtime.
- A direct label/tag query is resolved locally through the normal search path
  and does not start the semantic runtime. This includes exact label names,
  normalized/case-insensitive label names, and explicit tag syntax such as
  `#portrait` if that syntax is added.
- A query with clear metadata intent, such as a camera model, lens string, date,
  filename fragment, or EXIF-shaped numeric token, uses normal search first.
- Only a non-label free-text query, with the `Semantic Search` toggle enabled,
  starts or acquires `SemanticRuntimeService`, embeds the text query, and asks
  storage for a VSS/HNSW-ranked page.
- If semantic routing fails because the model is missing, runtime startup fails,
  or the VSS extension/index is unavailable, surface that semantic error
  explicitly. Do not silently replace the semantic branch with a full-vector C++
  scan. The user can turn the toggle off to run ordinary matching.

Phase 5 should not make semantic search run on every keystroke by default.
Traditional preview can keep its current short debounce. Semantic preview should
run only when the user presses Enter or clicks the search button. A development
experiment may measure debounced semantic preview with cancellation and a
single-flight request guard, but the shippable default remains explicit-submit
until text embedding plus HNSW paging is proven interactive with MobileCLIP on
real catalogs.

Existing preview thumbnail lifecycle should be reused. Semantic result pages
should request thumbnails only for visible preview rows and should release them
when rows leave view, matching current `SearchController` behavior.

## Error Handling and Integrity

Reject writes when:

- request ID does not match
- embedding dimension does not match the active model
- vector contains NaN or infinity
- vector norm is zero
- runtime model info does not match the database model key
- database transaction fails

Surface user-actionable states:

- model missing
- model corrupt
- runtime failed to start
- runtime crashed
- device/provider unavailable
- generation cancelled
- partial generation completed with failures

Failed image rows should record enough error text for retry diagnostics, but
large runtime logs should stay in the app log rather than the database.

## Packaging

Add a CMake target that builds the Rust binary:

- Windows: call Cargo through `rust/puerh_mind/script/cargo_msvc.cmd`
- macOS: call Cargo directly from the configured toolchain

Install the binary next to the app executable:

- Windows: install `alcedo_mind.exe` beside `alcedo_main.exe`
- macOS: install inside the `.app` bundle

Also install required ONNX Runtime dynamic libraries. The app package should
contain the runtime binary and small manifests/prototypes, but not model weights.

Packaging smoke tests should verify:

- installed app can find `alcedo_mind`
- runtime can start and answer `Ping`
- `GetModelInfo` reports the expected proto/runtime version
- missing model produces the downloader path instead of a crash

## Phased Rollout

1. Rust runtime hardening - complete
   - CLI config
   - validate-only model assets
   - model/status RPCs
   - batch RPCs
   - Core ML provider option
   - remove or quarantine stale Candle paths

2. C++ runtime client
   - generated C++ gRPC/protobuf stubs
   - `SemanticRuntimeService`
   - start/stop/health/status UI plumbing
   - fake-runtime tests before real model tests

3. Storage foundation
   - semantic tables
   - storage controller or repository wrapper
   - DuckDB VSS/HNSW vector ranking and pagination primitive
   - required DuckDB `vss` extension loading and index creation
   - project checksum/package integration
   - deletion cleanup

4a. Bulk generation request model and thumbnail pipeline - initial scaffold
   complete
   - thumbnail request/pin/release pipeline
   - generation job/progress/cancel/result model
   - batch-shaped image embedding client interface with mock responses
   - real `ThumbnailService::GetThumbnailDetailed` integration
   - thumbnail CPU materialization into RGBA8 request payloads
   - release pinned thumbnail after payload preparation in success, error, and
     cancellation paths
   - tests that import real sample images, request real thumbnails, batch mock
     embeddings, handle thumbnail failures, and cancel during mock embedding

4b. Bulk generation runtime RPC
   - generated C++ gRPC image batch client or equivalent bridge
   - real image embedding request/response waiting and timeout handling
   - request-id matching and per-item partial failure mapping
   - model-info compatibility checks before generation starts
   - decide final image payload contract: raw `rgba8:WxH` or encoded image bytes

4c. Bulk generation persistence and labels - complete
   - persist image embeddings through `SemanticStorageController`
   - seed bundled photography label query rows into new project databases
   - generate label query embeddings once per model/config and cache them in
     `SemanticLabelPrototype`
   - label assignment from cached project-local prototypes
   - persist label decisions transactionally with embeddings
   - reject bad vectors: wrong dimension, NaN/Inf, zero norm, request mismatch

4d. Bulk generation workflow integration
   - retry/force-regenerate rules
   - skip already-valid embeddings for the active model key
   - import-finished prompt
   - UI-facing progress/cancel/failure state

5. Search integration and query routing
    - 5a. Traditional search baseline and label participation
      - keep `SleeveFilterService::BuildFuzzySearchWhere(...)` as the default
        path when semantic search is disabled
      - extend the normal search document to include generated
        `SemanticImageLabel.label` values without requiring the runtime
      - preserve filename, element name, image path, and EXIF matching behavior
        exactly for ordinary queries
      - keep recommendation rows local; label/tag recommendations should be
        backed by stored labels, not by runtime text embeddings
      - add focused tests proving that typing a generated label name returns
        results through the ordinary path
    - 5b. Visible semantic-search toggle and persisted UI state — complete
      - add a compact `Semantic Search` / `语义搜索` toggle to
        `GlobalSearchDialog.qml`
      - persist the preference in `QSettings`, but default it off for existing
        users and new projects
      - expose the toggle through `SearchController` so QML does not decide
        runtime behavior by itself
      - keep ordinary preview debounced while the toggle is off
      - add an explicit submit affordance for semantic search: Enter and a
        search button should both call the same controller method
    - 5c. Query intent classifier — complete
      - centralize routing in C++ near `SearchController` or
        `SleeveFilterService`; QML should only pass query text and toggle state
      - normalize query text for direct label matching against
        `SemanticLabelQuery` and assigned `SemanticImageLabel` values
      - treat exact label names, known synonyms, and explicit tag syntax as
        traditional label/tag search even when the toggle is on
      - treat EXIF-shaped tokens, dates, camera/lens strings, filenames, and
        short structured fragments as ordinary search
      - route only non-label natural-language text to semantic search
      - expose the chosen route in testable data, for example
        `traditional`, `label`, `semantic`, or `empty`
    - 5d. Concrete semantic provider — complete
      - implement the concrete `SemanticSearchProvider` and register it from
        `ProjectService` after storage and runtime services exist
      - acquire `SemanticRuntimeService` only for the semantic route, using the
        same ad hoc lifecycle rule as generation
      - fetch/validate model info, derive the active model key, and reject
        search when the model key is not registered in storage
      - call text embedding once per submitted query, validate the returned
        vector, then call `SemanticStorageController::SearchImageEmbeddings`
      - keep DuckDB VSS/HNSW as the only ranked vector path; missing extension
        or index is an actionable semantic-search error, not a C++ scan fallback
      - return rows in the same lightweight shape as `FuzzySearchMatch` so
        preview thumbnail handling stays shared
    - 5e. Preview pagination, counts, and result lifecycle — complete
      - keep existing preview thumbnail pin/release behavior for semantic rows
      - support paged semantic previews with `offset`/`limit`; do not fetch the
        whole vector corpus or materialize giant result lists in QML
      - decide count semantics explicitly: either return an approximate
        `hasMore` response for semantic pages, or add a storage-owned count/page
        token; do not fake a full count by scanning vectors in C++
      - cancel or ignore stale semantic requests when a newer submitted query
        replaces them
      - show loading, empty, and error states in the existing search dialog
        without closing the dialog before progress is visible
      - schedule preview, submit, and page requests through a pending loading
        state; semantic submit/page requests run off the QML thread, while
        ordinary preview keeps the legacy search path and still paints loading
        feedback before execution
    - 5f. Apply-to-album-grid path — complete
      - ordinary searches continue to apply as SQL `WHERE` filters
      - semantic searches should apply through a storage-owned result token or
        scoped temporary result table, not a giant `IN (...)` list in UI code
      - tie the applied result token to folder scope, query text, model key, and
        a generation/version marker so stale result sets can be invalidated
      - make clearing search release the semantic result scope and restore the
        normal album query path
      - keep root and folder scope aligned with
        `ElementController::BuildScopedFileQuery`
    - 5g. Realtime-search experiment
      - measure a debounced semantic preview behind a development flag or local
        instrumentation only
      - record text embedding latency, runtime startup latency, VSS query
        latency, cancellation behavior, and UI frame impact on realistic albums
      - only promote realtime semantic preview if repeated text embeddings are
        compatible with the active MobileCLIP runtime and stay comfortably
        interactive; otherwise keep Enter/button submission as product behavior
      - cache repeated query embeddings by normalized query + model key only
        after correctness and invalidation rules are defined

6. Model identity, Rust download manager, and packaging
   - 6a. Model identity and storage compatibility
     - complete: keep `model_key` as the compatibility boundary for stored
       semantic rows. In the current implementation the key is derived from
       `model_id@revision`, so the embedding, label, prototype, skip-check,
       label-display, ordinary-search, semantic-search, and cleanup paths are
       already scoped without duplicating `model_id` into every row table.
     - complete: add an explicit active-model flag to `SemanticModel`; ordinary
       search and label stats read generated labels only for the active model.
       If there is no active model, generated labels are unavailable rather than
       mixed into ordinary search.
     - complete: extend `SemanticModel` with model/profile metadata needed for
       multilingual CLIP models, including engine/profile id, supported text
       languages JSON, and manifest JSON. UI can later let the user choose the
       language when downloading/selecting a model.
     - complete: old rows for non-active models stay stored but hidden from
       label display, ordinary label search, and label statistics until that
       model is activated again.
     - complete: keep default label queries simple and reusable, while cached
       label prototypes remain scoped by `model_key + prompt_config_hash`.
     - design decision: generated label storage should use stable canonical
       label ids, not English label text as the semantic source of truth. Each
       model/language profile owns the prompt text used for prototype
       embeddings, such as English prompts for MobileCLIP, Chinese prompts for
       Chinese-CLIP, and a fixed profile prompt language for multilingual
       models. UI display and legacy fuzzy search should read localized alias
       text from the label id, so changing the application language does not
       require regenerating embeddings or labels. Multilingual semantic search
       should accept user query text in any supported language and route through
       embeddings instead of requiring a query-language gate.
   - 6b. Embedding dimension policy
     - complete: this is folded into the model-manager profile contract rather
       than implemented as an independent schema expansion. DuckDB VSS/HNSW
       remains the only semantic vector-ranking path, and the active storage
       shape remains `FLOAT[512]`.
     - complete: Rust model profiles must expose `embedding_dimension = 512`.
       Non-512 profiles are rejected before being marked usable. Jina CLIP v2 is
       tracked as `native_embedding_dimension = 1024` but exposed to Alcedo as a
       512-dimensional Matryoshka profile; the eventual inference engine must
       truncate/normalize to that profile dimension before returning vectors.
     - still required in downstream generation/search wiring: validate
       model-reported embedding dimension before generation, label prototype
       writes, and semantic search.
   - 6c. Rust model-manager RPC surface - complete
     - the Rust sidecar now starts even when the configured inference model is
       missing; embedding RPCs and `GetModelInfo` report model-unavailable
       errors while model-manager RPCs remain available.
     - added fixed public model profiles for English MobileCLIP2 S2, Chinese
       CLIP ViT-B/16 ONNX, and multilingual Jina CLIP v2 INT8 ONNX. The current
       fixed list intentionally does not require `hf_token`.
     - added model-management RPCs for `ListModelProfiles`,
       `ListInstalledModels`, `ValidateModel`, `DownloadModel`,
       `GetModelDownloadStatus`, `CancelModelDownload`, and `DeleteModel`.
     - `DownloadModel` starts a background job and returns `job_id`
       immediately. Qt/C++ must poll `GetModelDownloadStatus(job_id)` rather
       than relying on a long gRPC deadline for the whole download.
     - C++ `SemanticRuntimeService` exposes the same model-manager surface and
       structured profile/manifest metadata; Qt does not implement Hugging Face
       downloading itself.
     - `EmbedTextBatch`, `EmbedImageBatch`, and `GetModelInfo` remain tied to a
       successfully loaded inference model, not to download completion side
       effects.
   - 6d. Rust download implementation
     - complete: move model acquisition out of inference-engine startup and
       into the model manager. Inference startup is validate-only unless
       explicitly told to download for development compatibility.
     - complete: replace MobileCLIP-only download constants with profile-driven
       asset manifests for text model, vision/multimodal model, tokenizer,
       tokenizer config or vocab, preprocessing config, and model config files.
     - complete: validate file size for every asset and SHA-256 for large model
       assets where Hugging Face exposes a stable LFS hash; write a local
       resolved manifest containing model identity, profile id, language,
       512-dimensional compatibility, native dimension, image size, and file
       list.
     - complete: default downloads use the domestic mirror endpoint
       `https://hf-mirror.com`, with a request-level override for custom mirror
       endpoints. The Rust downloader resolves the configured endpoint to the
       actual active source, follows redirects, and reports both the configured
       source and active source in progress messages so UI can warn when a
       mirror/proxy path is really downloading from `huggingface.co`.
     - complete: for fixed manifest assets, avoid generic Hub cache clients in
       the shippable path. Large files download through a controlled HTTP Range
       downloader with up to eight ranged connections by default
       (`ALCEDO_MIND_DOWNLOAD_THREADS`, capped at sixteen), while small files
       and non-Range servers use a resumable single-stream fallback.
     - complete: expose byte-level progress/resume metadata through
       `GetModelDownloadStatus(job_id)`, including phase, current file,
       per-file bytes, total bytes, completed file count, and a short status
       message that C++ polling can show in Settings.
     - complete: use a profile-level staging directory and promote the whole
       profile to the final model root after all files validate. Per-asset
       `.part` files and ranged `.part.N` chunk files are kept in staging so
       interrupted downloads can resume where the server and mirror support HTTP
       range requests.
   - 6e. Qt settings and runtime wiring
     - complete: keep download UX in Settings, but call Rust model-manager RPCs
       for Hugging Face network and file operations.
     - complete: expose selected model, endpoint preset/custom endpoint, model
       directory, status, progress, cancel/retry, delete, and active-model
       selection. The current model-manager RPC has no token field, and the
       fixed public profiles do not require an HF token.
     - complete: expose the three fixed profiles from the plan through a
       Settings combobox. Activation is intentionally limited to MobileCLIP2 S2
       until the non-MobileCLIP engines are wired.
     - complete: pass the active MobileCLIP2 S2 profile root, `model_id`,
       revision, and device to `SemanticRuntimeService`; release builds use
       validate-only inference startup.
     - complete: remove the temporary development `model-root` fallback that
       probes `rust/puerh_mind/models/mobileclip2-s2-openclip`; the default
       model directory is the executable-adjacent `model` folder unless
       explicitly overridden.
     - covered by 6a/current activation gate: old semantic rows are hidden by
       active model key rather than silently mixed into active labels/search.
   - 6f. Model profile engine configuration and compatibility transforms
     - complete: make the model profile contract explicit: `embedding_dimension`
       is Alcedo's stored/output dimension, while `native_embedding_dimension`
       records the upstream model's raw output dimension.
     - complete: configure Jina CLIP v2 as a 512-dimensional Alcedo profile over
       its native 1024-dimensional output using the repository-recommended
       Matryoshka truncation behavior, then L2-normalize before returning
       vectors to C++.
     - add engine-side adapters for every non-MobileCLIP profile so each profile
       consumes its own tokenizer/preprocess/model config instead of sharing the
       MobileCLIP loader by accident.
     - reject a resolved manifest whose `embedding_transform` no longer matches
       the profile, because changing truncation/normalization changes semantic
       compatibility even when `model_id` and revision stay the same.
     - add per-profile smoke tests for reported model info: model id, revision,
       image size, native dimension, output dimension, and transform string.
   - 6g. Generation and label compatibility gates
     - before thumbnail work starts, verify runtime `GetModelInfo.model_id`,
       revision, image size, and embedding dimension against the active model
       record
     - register the active model before generation writes and reject writes when
       response model identity or vector dimension does not match
     - make `HasReadyImageEmbedding` and force-regenerate logic check the active
       `model_id`, not just file/image id
     - make ordinary text search include generated labels only for the active
       `model_id`
     - make semantic data deletion support active-model-only cleanup and
       old-model cleanup
   - 6h. Packaging and smoke tests
     - install the Rust binary next to the app executable or inside the app
       bundle
     - deploy ONNX Runtime dynamic libraries needed by the selected execution
       providers
     - deploy DuckDB `vss.duckdb_extension` when ANN search is enabled
     - package small manifests/profiles and default label-query config, but not
       model weights
     - smoke-test installed runtime startup without models, model validation
       failure, model-manager status, successful validate/load with local model
       assets, `Ping`, and `GetModelInfo`

7. Performance path
   - tune DuckDB VSS/HNSW index parameters for large catalogs
   - cached query embedding/result-token tables for repeated UI paging
   - apply-to-album-grid scope tables instead of giant `IN (...)` lists
   - multi-adapter or CPU worker pool only after single-process batching is
     measured

## Verification Plan

Rust tests:

- CLI config parsing
- missing-model validate-only failure
- model-manager startup without installed model assets
- model-manager download job status, cancellation, corrupt-file reporting, and
  endpoint/token request handling with token redaction
- profile-driven asset manifest validation for MobileCLIP and multilingual CLIP
- provider parsing for DirectML/Core ML/CPU
- batch request ordering and per-item errors
- non-finite embedding rejection

C++ tests:

- semantic table create/update/delete cleanup
- model identity migration/registration keeps old `model_key` rows isolated
  behind the active Hugging Face `model_id`
- project package/checksum includes semantic tables
- fake runtime text/image embedding paths
- generation job progress/cancel/error handling
- thumbnail pin released on success, failure, and cancel
- generation skip logic ignores embeddings generated by a different `model_id`
- label display and ordinary label search ignore labels generated by a different
  `model_id`
- model mismatch states expose delete/switch actions instead of reading stale
  embeddings or labels
- unsupported embedding dimensions fail before storage writes or vector search
- semantic search ranking within root and folder scopes
- label-only search without runtime
- normal search still matches filename, element name, EXIF, camera, lens, date,
  ISO, focal length, and aperture with semantic search disabled
- generated label names participate in ordinary search without starting the
  semantic runtime
- query routing classifies empty, metadata, label/tag, and natural-language
  queries deterministically
- when the semantic-search toggle is enabled, label/tag queries still use the
  ordinary path and natural-language queries use the semantic provider
- semantic provider starts/acquires the runtime only for submitted semantic
  queries and releases it according to the ad hoc lifecycle owner
- semantic provider surfaces missing model, runtime failure, bad embedding, and
  missing VSS extension/index errors without falling back to C++ vector scans
- semantic preview pagination ignores stale results after a newer submitted
  query and keeps thumbnail pin/release behavior intact
- applying a semantic result set to the album grid respects root/folder scope
  and does not build a giant UI-owned `IN (...)` filter

Manual smoke tests:

- download model from default source
- download model from custom mirror source
- download model with a provided HF token and verify the token is not logged
- switch between the default MobileCLIP model and a multilingual CLIP model, then
  verify old embeddings and labels are hidden until switching back
- delete semantic data for an old model without deleting active-model rows
- start/stop runtime repeatedly
- import images, accept semantic generation, cancel mid-run, retry
- with semantic search off, search filename, EXIF strings, dates, generated
  labels, and tag-like text
- with semantic search on, type a generated label name and verify the runtime
  does not start
- with semantic search on, submit a natural-language query with Enter and with
  the search button, then page results
- verify typing alone does not fire semantic requests in the default product
  path
- if the realtime experiment flag is enabled, compare debounce latency and
  cancellation behavior against explicit-submit semantic search
- package/reopen project and verify semantic rows survive

## Open Decisions

- Whether Phase 1 should add full C++ gRPC dependencies, or use a smaller local
  bridge API while keeping Rust gRPC internal. Direct C++ gRPC is cleaner long
  term if dependency size is acceptable.
- Whether default semantic generation should request `k256` only or warm `k1024`
  previews for newly imported images.
- Whether default labels should stay close to the Python demo list or become a
  user-editable taxonomy with regenerated text prototypes.
- Whether semantic embeddings should be project-local only or share a global
  cross-project cache keyed by image fingerprint and model key.

## Review Result
我看了 roadmap 和最近几个 commit，当前 HEAD 的重点确实已经从 MobileCLIP 单模型演进到 MobileCLIP + Jina CLIP 多模型，尤其最近的 c152e86e、1e791baa、9d130935 都在多模型、Jina label cache、active model 隔离这条线上。下面按你 8 个问题梳理“标签生成最短链路”。
1. 图片列表怎么收集
导入流程：列表来自 import 完成后的 snapshot.created_。FinishImport 先保存/打包/刷新当前 folder，然后遍历新创建文件，排除 element_id/image_id == 0 和 unsupported Nikon HE，构造 SemanticGenerationItem{element_id, image_id}，最后丢给 QueueSemanticGenerationPrompt()。见 [import_export.cpp (line 479)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/import_export.cpp:479)。
设置页 Generate：QML 按钮调用 StartAlbumGeneration(false)，C++ 用 AlbumBrowseService::ListFilesInFolderById(0) 收集全库 root scope 文件，再把每个 AlbumFileView.file_id_ / image_id_ 转成 SemanticGenerationItem。见 [SemanticGenerationSettingsPanel.qml (line 206)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/qml/SemanticGenerationSettingsPanel.qml:206) 和 [semantic_generation_controller.cpp (line 892)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:892)。
设置页“Download/Activate model”本身不收集图片。Activate 只注册/激活模型，并在需要时预热 SemanticLabelPrototype。见 [semantic_generation_controller.cpp (line 680)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:680)。
2. 设置页如何排除已有标签，统计谁做
设置页三项统计存在 SemanticGenerationController 成员里：album_total_count_、album_labeled_count_、album_unlabeled_count_，Q_PROPERTY 暴露给 QML。生命周期跟 AlbumBackend.semantic_generation_ 一样，是内存 UI 状态，不是表。见 [semantic_generation_controller.hpp (line 41)](D:/Projects/pu-erh_lab/alcedo_studio/src/include/ui/alcedo_main/album_backend/semantic_generation_controller.hpp:41)。
触发时机：设置页 Component.onCompleted 和 onSemanticControllerChanged 调 RefreshAlbumSummary()；生成进度、生成完成、模型激活后也会刷新。见 [SemanticGenerationSettingsPanel.qml (line 110)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/qml/SemanticGenerationSettingsPanel.qml:110)、[semantic_generation_controller.cpp (line 1316)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:1316)。
统计逻辑：总数走 browse->CountFilesInFolderById(0)，已有标签数走 SemanticStorageController::CountImageLabelsInFolder(0, active_model_key)，未标注数是相减。见 [semantic_generation_controller.cpp (line 847)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:847)。
真正生成前还会过滤一次：ItemsNeedingSemanticGeneration() 对每个候选调用 HasReadyImageEmbedding(file_id, image_id, model_key, require_label=true)。SemanticGenerationService::RunJob() 内部又重复做了一次 skip check，这是双保险，但也是额外 DB 读。见 [semantic_generation_controller.cpp (line 362)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:362) 和 [semantic_generation_service.cpp (line 889)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:889)。
3. 列表进入生成系统后的调用链
入口是 StartGenerationForItems()，它先进入 running 状态，160ms 后继续，让 UI 能先显示“Preparing”。之后确保 runtime 已经按 active profile 启动，如果当前 sidecar 是另一模型/根目录，会 Stop() 后重启。见 [semantic_generation_controller.cpp (line 1016)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:1016)。
模型身份：runtime GetModelInfo 后生成 model_key = model_id@revision，写 SemanticModel 并设 active。MobileCLIP batch size 64，Jina batch size 4。见 [semantic_generation_controller.cpp (line 60)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:60)、[semantic_generation_controller.cpp (line 1124)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:1124)。
缩略图：SemanticGenerationService 按 thumbnail_batch_size=8 请求 k256 缩略图，调用 ThumbnailService::GetThumbnailDetailed(element_id, image_id, pin=true, resolution)，拿到 ThumbnailGuard 后 materialize 成连续 RGBA8，并生成 format_hint = rgba8:WxH。见 [semantic_generation_service.cpp (line 489)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:489)、[semantic_generation_service.cpp (line 37)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:37)。
打包给后端：C++ 的 SemanticImageEmbeddingInput 只有 {item, request_id, rgba8_image, format_hint}，不带源文件路径/EXIF/prompt。随后转成 SemanticImageEmbeddingRequest，通过 gRPC EmbedImageBatch 发给 Rust。proto 里 image payload 是 bytes image_bytes + string image_format_hint。见 [semantic_generation_service.hpp (line 91)](D:/Projects/pu-erh_lab/alcedo_studio/src/include/app/semantic_generation_service.hpp:91)、[semantic_runtime_service.cpp (line 661)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_runtime_service.cpp:661)、[semantic.proto (line 41)](D:/Projects/pu-erh_lab/rust/puerh_mind/proto/semantic.proto:41)。
prompt：每张图片生成不传 prompt。label prompt 是项目表 SemanticLabelQuery 里的固定 label query；缺 prototype 时先 EmbedTextBatch 生成 SemanticLabelPrototype，之后图片标签分配都在 DB 里用 prototype 算。见 [semantic_generation_service.cpp (line 183)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:183)。
4. Batch 拆包和 DB 写入
Rust 侧 EmbedImageBatch 先把每个 RGBA8 decode 成 RgbImage，无效 item 返回 per-item error；有效图片统一 engine.embed_images(&images)，按原 slot 写回 EmbeddingBatchItem，所以 response 保持 request 对应关系。见 [semantic.rs (line 427)](D:/Projects/pu-erh_lab/rust/puerh_mind/src/server/semantic.rs:427)。
C++ 拆包先用 request_id 建 map，再按原 input 顺序重排，检测 missing/duplicate response。见 [semantic_generation_service.cpp (line 417)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:417)。
写入：成功 embedding 调 PersistSemanticResult()，构造 SemanticImageEmbeddingRecord，再进 SemanticStorageController::UpsertImageEmbeddingAndAssignLabel()。一个图片一个 DuckDB transaction：删除旧 SemanticImageEmbedding、删除旧 SemanticImageLabel、插入新 embedding，然后 JOIN SemanticLabelPrototype 用 array_inner_product 排 top labels，插入 SemanticImageLabel，最后 commit。见 [semantic_generation_service.cpp (line 282)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:282)、[semantic_storage_controller.cpp (line 657)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/semantic/semantic_storage_controller.cpp:657)。
写入表：SemanticModel、SemanticImageEmbedding、SemanticImageLabel、必要时 SemanticLabelPrototype；SemanticLabelQuery 是 DB 初始化时 seed。schema 见 [db_controller.hpp (line 48)](D:/Projects/pu-erh_lab/alcedo_studio/src/include/storage/controller/db_controller.hpp:48)。
拷贝情况：不是零拷贝。缩略图 Mat 复制到 std::vector<uint8_t>；generation input 又复制到 runtime request；protobuf set_image_bytes 再复制；embedding 写 DB 前从 response vector 复制进 record，然后再 stringify 成 FLOAT[512] SQL literal。见 [semantic_generation_service.cpp (line 109)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:109)、[semantic_generation_service.cpp (line 711)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_generation_service.cpp:711)、[semantic_runtime_service.cpp (line 669)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/semantic_runtime_service.cpp:669)、[semantic_storage_controller.cpp (line 84)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/semantic/semantic_storage_controller.cpp:84)。
5. 写完后如何刷新相册，三个 panel 怎么读 DB
生成完成后 Finish()：reset job/runtime guard，RefreshAlbumSummary()，backend_.ReloadCurrentFolder()，然后保存/打包项目。runtime guard 析构会 stop sidecar。见 [semantic_generation_controller.cpp (line 1330)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/semantic_generation_controller.cpp:1330)。
中间 grid：ReloadCurrentFolder() 调 stats_.RebuildThumbnailView() 和 stats_.RefreshStats()。列表数据走 AlbumBrowseService::ListFilesInFolderById(current_folder, offset, limit, filter)，底层 SQL 是 Element + FileImage + Image scope query。每个 AlbumItem 再从 ImagePool 读 EXIF，并单独调用 SemanticLabelDisplayText(elementId) 查 SemanticImageLabel。见 [album_backend.cpp (line 1119)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/album_backend.cpp:1119)、[element_controller.cpp (line 391)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/sleeve/element_controller.cpp:391)、[album_backend.cpp (line 1245)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/album_backend.cpp:1245)。
右侧 Inspector：主要读 albumBackend.totalPhotoCount/dateStats/cameraStats/labelStats/ratingStats，这些来自 SleeveFilterService::BuildFolderStats()。label stats 会 join SemanticImageLabel，并只使用 active model key。见 [InspectorPanel.qml (line 92)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/qml/InspectorPanel.qml:92)、[sleeve_filter_service.cpp (line 459)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/sleeve_filter_service.cpp:459)、[element_controller.cpp (line 365)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/sleeve/element_controller.cpp:365)。
左侧 Collection Panel：读 albumBackend.folders，由 FolderController::ReloadTree() / RebuildFolderView() 维护，数据来自 AlbumBrowseService::ListFolders() / sleeve folder tree，不直接读语义表。选择 collection 后触发 ReloadCurrentFolder()。见 [folder_controller.cpp (line 183)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/folder_controller.cpp:183)、[CollectionsPanel.qml (line 407)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/qml/CollectionsPanel.qml:407)。
6. Inspector 状态刷新时机和解包
刷新时机：项目加载完成、导入完成、folder 切换、stats filter/search filter 改变、语义生成进度/完成都会间接走 ReloadCurrentFolder() 或 RefreshStats()。StatsEngine::RefreshStats() 将 storage 返回的 AlbumStatsView 转成 QML QVariantList，每行 {label, count}；语义 label 会本地化 display text。见 [stats_engine.cpp (line 107)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/stats_engine.cpp:107)。
Inspector 不直接拿 DB result；它绑定 albumBackend.*Stats。DB -> StorageStatsBucket -> StatsBucket -> QVariantMap{label,count} -> QML model。见 [sleeve_filter_service.hpp (line 20)](D:/Projects/pu-erh_lab/alcedo_studio/src/include/app/sleeve_filter_service.hpp:20)。
7. Grid 小格子信息从哪来
Grid delegate 的字段来自 AlbumThumbnailModel roles：fileName/cameraModel/extension/iso/aperture/focalLength/captureDate/rating/isHdr/tags/thumbUrl/...。见 [album_thumbnail_model.cpp (line 38)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/album_thumbnail_model.cpp:38)。
这些 role 的来源分三块：基础 file id/name 来自 ElementController::ListFilesInFolderPage()；EXIF/评分/HDR 来自 ImagePoolService::Read(imageId) 的 Image 对象；tags 来自 active model 下的 SemanticImageLabel 查询。缩略图 URL 则由 delegate 可见性触发 SetThumbnailVisible()，经 ThumbnailService 异步回填并发 thumbnailUpdated。见 [ThumbnailGridView.qml (line 730)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/qml/ThumbnailGridView.qml:730)、[thumbnail_manager.cpp (line 102)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/thumbnail_manager.cpp:102)。
8. 每次打开一张图片是否还查 DB
详情弹窗打开时会调用 albumBackend.GetImageDetails(elementId, imageId)。它先从当前 AlbumItem 尝试解析 file id/image id，然后调用 SemanticLabelDisplayText(file_id) 查一次 SemanticImageLabel，再用 ImagePoolService::Read(image_id) 读取本地/缓存 Image 来生成 EXIF rows。见 [Main.qml (line 427)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/qml/Main.qml:427)、[image_controller.cpp (line 734)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/image_controller.cpp:734)。
所以：文件名、image id、部分 grid metadata 可以来自当前 AlbumItem；EXIF 详情、原图尺寸、source directory 来自 ImagePool 中的 Image；semantic tags 仍查 DB。中间 grid 已缓存 tags，但详情弹窗没有复用 item->tags，而是重新查 active model label，这保证模型切换后详情不读旧标签，但会多一次 DB 查询。
总的调用链可以压缩成：
Import snapshot / Settings full album list
→ SemanticGenerationController
→ active profile runtime start + SemanticModel upsert
→ HasReadyImageEmbedding(... require_label=true) 过滤
→ SemanticGenerationService worker
→ ThumbnailService::GetThumbnailDetailed(k256)
→ MaterializeThumbnailRgba8
→ GrpcSemanticRuntimeClient::EmbedImageBatch
→ Rust SemanticServiceImpl::embed_image_batch
→ ORT engine MobileCLIP/Jina
→ response by request_id
→ C++ reorder/map
→ UpsertImageEmbeddingAndAssignLabel
→ SemanticImageEmbedding + SemanticImageLabel
→ RefreshAlbumSummary + ReloadCurrentFolder
→ grid/stats/details 重新按 active model 读标签。

## Review Result - Delete Cleanup
这轮追的是“图片/文件删除时，语义标签和 embedding 是否一起删掉”。结论先说：

当前真正删除 file identity 的路径会删除 per-image semantic rows，包括
`SemanticImageEmbedding`、`SemanticImageEmbedding768` 和 `SemanticImageLabel`。
从 album 文件夹里移除图片、删除 album 文件夹本身不会删 semantic rows；这是刻意的，
因为这些操作只是移除 membership，不删除 root/library 里的图片实体。

1. UI 图片删除入口
中间 grid 或 Nikon HE recovery 走 `AlbumBackend::DeleteImages()`，最终进
`ImageController::DeleteTargets()`。这里先看当前 folder scope：如果当前是 root
folder (`folder_id == 0`)，就是“从 library 删除”；如果当前是 album folder，就是
“从这个 album 移除”。见
[image_controller.cpp (line 511)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/image_controller.cpp:511)
和
[image_controller.cpp (line 605)](D:/Projects/pu-erh_lab/alcedo_studio/src/ui/alcedo_main/album_backend/image_controller.cpp:605)。

`DeleteTargets()` 调的是
`AlbumBrowseService::DeleteFilesInFolderByElementIds(folder_id, ids)`。这个函数在
`folder_id == 0` 时调用 `SleeveServiceImpl::DeleteFilesEverywhere()`；否则调用
`SleeveServiceImpl::DeleteFilesFromFolder()`。见
[album_browse_service.cpp (line 277)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/album_browse_service.cpp:277)
和
[album_browse_service.cpp (line 299)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/album_browse_service.cpp:299)。

2. root/library 删除路径
root 删除的核心链路是：

`ImageController::DeleteTargets()`
→ `AlbumBrowseService::DeleteFilesInFolderByElementIds(0, ids)`
→ `SleeveServiceImpl::DeleteFilesEverywhere(ids)`
→ `FileSystem::DeleteFilesEverywhere(ids)`
→ `SleeveServiceImpl::Sync()`
→ `ElementController::RemoveElements(garbage_elements)`
→ `DeleteSemanticRowsForFiles(file_ids)`.

`FileSystem::DeleteFilesEverywhere()` 会把 file 从所有已加载 folder membership 里移除，
然后把 file 标成 `SyncFlag::DELETED`。真正落库删除发生在后面的 `Sync()`：它取
`fs_->GetDeletedElements()`，调用 `element_ctrl.RemoveElements(garbage_elements)`。
见
[sleeve_filesystem.cpp (line 260)](D:/Projects/pu-erh_lab/alcedo_studio/src/sleeve/sleeve_filesystem.cpp:260)
和
[sleeve_service.cpp (line 128)](D:/Projects/pu-erh_lab/alcedo_studio/src/app/sleeve_service.cpp:128)。

`ElementController::RemoveElements()` 会先收集所有被删 file 的 `element_id`，然后调用
`DeleteSemanticRowsForFiles(file_ids)`。这个 helper 执行三条 SQL：

- `DELETE FROM SemanticImageEmbedding WHERE file_id IN (...)`
- `DELETE FROM SemanticImageEmbedding768 WHERE file_id IN (...)`
- `DELETE FROM SemanticImageLabel WHERE file_id IN (...)`

见
[element_controller.cpp (line 119)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/sleeve/element_controller.cpp:119)
和
[element_controller.cpp (line 301)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/sleeve/element_controller.cpp:301)。

所以，按当前 schema，图片删除时 per-image 的 512/768 embedding 和最终标签都会清掉，
而且是不按 model_key 过滤，所有模型版本下这个 file_id 的 embedding/label 都删。

3. album unlink 和 folder 删除为什么不清 semantic rows
从 album 中删除图片时，走的是 `DeleteFilesFromFolder()` /
`FileSystem::UnlinkFilesFromFolder()`。它只删 `FolderContent` membership，file 仍然在
root/library 里，所以语义数据应当保留。测试也明确覆盖了这个语义：
`AddToAlbumThenDeleteFromAlbum_KeepsRootFile`。见
[sleeve_filesystem.cpp (line 134)](D:/Projects/pu-erh_lab/alcedo_studio/src/sleeve/sleeve_filesystem.cpp:134)
和
[album_backend_image_delete_test.cpp (line 220)](D:/Projects/pu-erh_lab/alcedo_studio/tests/ui/album_backend_image_delete_test.cpp:220)。

删除 album folder 也不是删除里面的图片。`FileSystem::Delete(path)` 删除 folder 时，
代码注释明确写了 album membership 不拥有 file，移除 folder 只影响 folder-tree
identity；file children remain live library files。见
[sleeve_filesystem.cpp (line 364)](D:/Projects/pu-erh_lab/alcedo_studio/src/sleeve/sleeve_filesystem.cpp:364)。

因此：如果用户删除一个 album/folder，semantic rows 不删是正确的；如果用户从 root
删除图片，semantic rows 应该被清掉。

4. 哪些 semantic 表不会随图片删除
不会被 root 图片删除清掉的表是：

- `SemanticModel`
- `SemanticLabelQuery`
- `SemanticLabelPrototype`
- `SemanticLabelPrototype768`

原因是它们不是单张图片的数据。`SemanticModel` 是模型身份；`SemanticLabelQuery` 是固定
label prompt 配置；`SemanticLabelPrototype*` 是 model + prompt 的文本原型缓存，供所有
图片共享。单张图片删除不应该删这些表，否则会影响同一模型下其他图片的生成/搜索。schema
见
[db_controller.hpp (line 48)](D:/Projects/pu-erh_lab/alcedo_studio/src/include/storage/controller/db_controller.hpp:48)。

5. 生成覆盖旧标签时也会先删旧行
除了图片删除，重新生成同一 file/model 的 embedding 时也会先删旧 embedding 和旧 label，
再插入新 row。单张写入走 `UpsertImageEmbeddingAndAssignLabel()`，batch 写入走
`UpsertImageEmbeddingsAndAssignLabels()`；两者都先 delete 再 insert。见
[semantic_storage_controller.cpp (line 1167)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/semantic/semantic_storage_controller.cpp:1167)
和
[semantic_storage_controller.cpp (line 1225)](D:/Projects/pu-erh_lab/alcedo_studio/src/storage/controller/semantic/semantic_storage_controller.cpp:1225)。

6. 测试覆盖
已有一个直接覆盖语义清理的测试：
`SemanticStorageControllerTest.DeletingFileRemovesSemanticRows`。它创建两个文件，给两个
文件都写 semantic embedding，然后 `DeleteFileEverywhere(delete_id)`，最后断言被删文件
的 embedding count 变 0、保留文件仍为 1，并且 semantic search 只返回保留文件。见
[semantic_storage_controller_test.cpp (line 668)](D:/Projects/pu-erh_lab/alcedo_studio/tests/storage/semantic_storage_controller_test.cpp:668)。

UI 层也覆盖了 root 删除会从所有 album 中级联移除 membership：
`DeleteFromRootCascadesOutOfAlbums`。这个测试没有插 semantic rows，但它覆盖了 UI root
删除是否真的到达 `DeleteFilesEverywhere()` 这条 file identity 删除路径。见
[album_backend_image_delete_test.cpp (line 337)](D:/Projects/pu-erh_lab/alcedo_studio/tests/ui/album_backend_image_delete_test.cpp:337)。

7. 仍然值得补的风险点
有两个小风险：

- `ElementController` 里还有一个 `RemoveElement(sl_element_id_t id)` overload，它只删
  `Element` 表，不清 file child rows，也不清 semantic rows。目前我没有看到删除路径调用
  这个 overload；真实路径用的是 `RemoveElement(shared_ptr)` 或 `RemoveElements(span)`。
  但这个 overload 是潜在 footgun，后续最好删除、私有化，或改成完整清理。
- `DeleteSemanticRowsForFiles()` 是同步删除流程里的 helper，但它没有包在同一个显式事务里，
  也没有把 DuckDB error 往外抛。正常情况下没问题；如果将来 schema 迁移失败或表损坏，
  可能出现 file row 删除成功但 semantic row 留下的情况。更稳的做法是把
  `RemoveElements()` 的 semantic/file/history/pipeline/folder/element 删除放进一个
  transaction，并让语义删除失败时阻止 commit。

最后还有一个数据库文件大小层面的点：这些 `DELETE` 会让 row 不再被查询到，也会让空间有
机会被数据库复用；但 DuckDB 文件不一定因为删除几行就立刻变小。如果关心 `.db` 物理文件
回收，需要另看 checkpoint/VACUUM 或项目打包快照是否会压实数据库文件。
