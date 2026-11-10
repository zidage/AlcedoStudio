# Storage Query Boundary Plan

Date: 2026-11-09

Status: in progress. S1 and S2 complete (2026-11-09); S3 to S6 planned.

Primary owner: Alcedo Studio storage (`duckorm`, Mapper, Store) and the callers of storage in
`app/` and `sleeve/`.

Source revision of the audit: `main` at `307d68737`.

Tracking issue: #785

Related plans:

- [duckorm Query Expression and Album Filter SQL Plan](duckorm_query_expression_and_album_filter_sql_plan.md):
  this plan extends it. That plan added `SqlFragment`, `expr::`, `clause::`, and the
  Mapper and Store names. This plan completes the read path, the bind path, and the layer
  boundary.
- [Library Search and Project Size Plan](library_search_and_project_size_plan.md): this plan
  depends on it. The match set table, the search columns, and the stats buckets come from that
  plan. This plan moves their SQL into storage and keeps their behavior.

Before implementation of each phase, read `AGENTS.md` and every applicable skill again. The
repository rules can change after this plan.

## 1. Decision record

### 1.1 Goal

The user reviewed the storage code and found a severe leak of the storage abstraction. Some
stores use duckorm. Other stores call the DuckDB C API directly (`MaskAssetStore`,
`CommitGraphStore`). Code outside storage also builds SQL and opens connections. The user wants:

1. A storage layer where every SQL execution, bind, and result read goes through duckorm.
2. A clear method for complex queries (aggregates, joins, window functions, grouping sets,
   vector and full-text search) in C++.
3. Library management and statistics features that reach the database only through storage.

### 1.2 Approved design

- **Write side and read side.** Aggregate rows (Image, Element, EditCommit, VersionRef,
  ImageEditState, MaskAsset) use a table Mapper with typed keys. Lists, pages, statistics, and
  search use named read queries: SQL text in the store, with typed binds and typed result rows.
  duckorm generates SQL only for single-table CRUD and for composable predicates.
- **Typed result rows.** A read query decodes into a C++ row struct. The row struct lists its
  columns as a list of member pointers. duckorm checks the column count and the column types once
  for each result. This replaces positional `VarTypes` reads and direct `duckdb_value_*` calls.
- **One statement owner.** `duckorm::Statement` is the single RAII owner of a prepared
  statement and its result.
- **Transactions belong to the caller.** Mapper methods run inside the transaction of the
  caller. Store methods open transactions with `duckorm::Transaction`.
- **One definition for each stats dimension.** The GROUP BY expression of a stats bucket and the
  filter of a clicked bucket come from one storage definition.
- **SQL text lives only in storage.** `app/`, `sleeve/`, and `ui/` give domain values to
  storage. They receive domain values and row DTOs.
- **A source check enforces the boundary.** A script in CI fails when code outside storage
  calls the DuckDB C API, includes duckorm or mapper headers, or reads the connection guard.

### 1.3 Decisions that prevent rework

- **SQL DSL.** A compile-time SQL DSL (sqlpp11, sqlite_orm) is rejected. DuckDB features in use
  (`GROUPING SETS`, window functions, `vss`, FTS `match_bm25`, temporary match set tables) exceed
  what such a DSL expresses.
- **Boost.PFR.** The project has no Boost.PFR dependency. Member-pointer column lists give the
  same decoding with C++20 and add no package.
- **UI calls to stores.** UI code can keep calls to store methods through the `Storage` facade
  (for example `AiStore::GetActiveRating`). These calls stay inside the storage layer API.
  A move of those calls into app services is excluded from this plan.
- **Persisted format.** The plan keeps every table, column, and stored value. Old projects open
  unchanged.

## 2. Current source audit

### 2.1 Direct DuckDB C API use inside storage

Counts of `duckdb_*(` calls and SQL built with `std::format` outside `storage/mapper/duckorm`:

| File | `duckdb_*` calls | Formatted SQL |
| --- | ---: | ---: |
| `alcedo_studio/src/storage/store/edit_history/commit_graph_store.cpp` | 73 | 13 |
| `alcedo_studio/src/storage/store/sleeve/element_store.cpp` | 42 | 8 |
| `alcedo_studio/src/storage/store/mask_asset/mask_asset_store.cpp` | 26 | 0 |
| `alcedo_studio/src/storage/mapper/sleeve/element/folder_mapper.cpp` | 2 | 4 |
| `alcedo_studio/src/storage/store/ai/ai_store.cpp` | 1 | 7 |
| `alcedo_studio/src/storage/store/database.cpp` | 3 | 0 |

### 2.2 Causes in duckorm and Mapper

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Result read | `duckorm::select`, `select_by_query` in `storage/mapper/duckorm/duckdb_orm.hpp` | Returns `std::vector<std::vector<VarTypes>>`. Reads by position. Needs a `DuckFieldDesc` table built with `offsetof`. Types: INT32, INT64, UINT32, UINT64, DOUBLE, text kinds, nullable DOUBLE and INT64, FLOAT_ARRAY. | Typed row decoding for any SELECT shape. Add BLOB, FLOAT, UTINYINT, DATE. |
| Bind | `BindValue` in `duckdb_expr.hpp` | Holds monostate, int64, double, bool, string. | Add BLOB. Add typed bind overloads on `Statement`. |
| Statement owner | `duckorm::PreparedStatement` in `duckdb_types.hpp`; a second `PreparedStatement` class in `mask_asset_store.cpp` | Two RAII classes. The duckorm class has public members. | One `duckorm::Statement`. |
| WHERE clause | `Mapper::Get(const char*)`, `GetParams(const char*)`, `RemoveByClause(const std::string&)`, `PrimeKeyClause()` format strings in `storage/mapper/mapper.hpp` | The API takes text WHERE clauses. Callers fill them with `std::format`, for example `commit_hash='{}'` in `commit_graph_store.cpp:181`. | Typed key methods. WHERE input is `SqlFragment` only. |
| Transactions | `begin_transaction` and `commit_transaction` with `try`/`catch` (9 sites in `commit_graph_store.cpp`, `element_store.cpp`, `mask_asset_store.cpp`, `mapper.hpp`); `duckorm::Transaction` in five other stores | Three styles. `Mapper::InsertParamsBatch` and `UpdateParamsBatch` open their own transaction, so a store cannot call them inside its own transaction. | `duckorm::Transaction` only. Mapper methods open no transaction. |

### 2.3 Defect found during the audit

`ImageStore::RemoveImageByPath` in `alcedo_studio/src/storage/store/image/image_store.cpp:101`
builds `image_path=<path>` with the path unquoted. Each call fails in DuckDB, and a path with a
quote changes the statement. The method has no caller. Phase S2 deletes it.

### 2.4 SQL and connections outside storage

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Stats bucket filters | `FilterSQLCompiler` in `alcedo_studio/src/sleeve/sleeve_filter/filter_sql.cpp:64`; bucket builders in `filter_factory.cpp` | Builds `COALESCE(NULLIF(i.camera_model, ''), '(unknown)')` and similar bucket expressions. `filter_combo.hpp:37` states in a comment that each expression must equal the GROUP BY in `ElementStore` (`element_store.cpp:218`). Two copies of each expression exist in two layers. | One stats dimension definition in storage. A bucket filter is a domain value. |
| Search predicates | `alcedo_studio/src/app/sleeve_filter_service.cpp` | 42 `expr::raw` calls: `contains`, `regexp_matches`, FTS `match_bm25`, `SemanticImageLabel` and `AiImageSearchText` subqueries, table and column names. | Move the predicate builders into storage. |
| Service signatures | `AlbumBrowseService` (`alcedo_studio/src/include/app/album_browse_service.hpp:84-132`), `SleeveFilterService::BuildSemanticRelevanceSelect`, `library_module.cpp:50-60` | `std::optional<duckorm::SqlFragment>` crosses the app and UI boundary. | Domain value for the semantic relevance request. |
| Project diagnostics | `ComputeProjectDataSummary` in `alcedo_studio/src/app/project_service.cpp:341-403` | Calls `duckdb_query` with `SELECT COUNT(*)`, `MIN`, `MAX`. Table and key names are literal text in app code. | Storage maintenance API. |
| Package backend | `alcedo_studio/src/app/project_package_backend.cpp:815-1054`, header `project_package_backend.hpp:118-131` | Runs `CHECKPOINT`, `ATTACH`, `COPY FROM DATABASE`, `DETACH`, HNSW index queries through the C API. The public header takes `duckdb_connection`. | Storage maintenance API. |
| Store construction | `pipeline_service.cpp`, `committed_snapshot_cache.cpp`, `editor_mini_git_commit_writer.cpp`, `editor_mini_git_journal_recovery.cpp`, `editor_mini_git_materializer.cpp`, `project_mask_asset_reader.cpp` | Read `GetDatabase().GetConnectionGuard().conn_` and build `CommitGraphStore`, `MaskAssetStore`, or `PipelineMapper` on it. | `Storage` gives a store session that holds the lock and the stores. |
| Build boundary | `alcedo_studio/src/storage/CMakeLists.txt:10,32,59` | `DuckDB` and `DuckORM` are `PUBLIC_DEPS` of the storage targets, so every dependent target can include `duckdb.h`. | A source check in CI. |

The UI layer (`alcedo_studio/src/ui`) has no `duckdb_*` call and no SQL text. The statistics
read path is correct and stays:

```text
LibraryModule (worker job)
  -> AlbumBrowseService::ReadAlbumQuery
  -> ElementStore::ReadAlbumQuery          (one match set table, one GROUPING SETS read)
  -> AlbumQueryResult.statistics_ (FolderStatsView)
  -> ToAlbumStatsView -> StatsEngine::ApplyFolderStats -> QML
```

The list, the total, the groups, and the buckets come from one match set. The plan keeps this
property in every phase.

### 2.5 Current tests

| Target | Source | Covers |
| --- | --- | --- |
| `DuckormExprTest` | `tests/storage/duckorm_expr_test.cpp` | `expr::` and `clause::` text and binds |
| `DuckormStatementTest` | `tests/storage/duckorm_statement_test.cpp` | `execute`, `select_int64`, `Transaction`, `Appender` |
| `MapperCrtpRoundtripTest` | `tests/storage/mapper_crtp_roundtrip_test.cpp` | Mapper CRUD |
| `MaskAssetStoreTest` | `tests/storage/mask_asset_store_test.cpp` | MaskAsset put, read, corruption, delete |
| `CommitGraphTest` | `tests/edit/history/commit_graph_test.cpp` | Commit graph persistence |
| `SleeveFilterCompileTest`, `SleeveFilterFactoryTest` | `tests/sleeve/` | Filter compile text and bucket filters |
| `FilterServiceTest`, `AlbumQueryTest`, `LibrarySearchRecallTest`, `LibrarySearchColumnsTest` | `tests/app/` | Search, album pages, stats |
| `AiStorageControllerTest`, `SemanticStorageControllerTest` | `tests/storage/` | AI and semantic stores |
| `PipelineMapperTest`, `ProjectPackageBackendTest`, `ProjectCompatibilityTest` | `tests/app/` | Pipeline rows, package snapshot, old projects |

## 3. Target architecture

### 3.1 duckorm layers

| Layer | Owner | Responsibility |
| --- | --- | --- |
| L0 | `duckorm::Statement`, `duckorm::Result` (proposed `storage/mapper/duckorm/duckdb_statement.hpp`) | Prepare, bind, run, and read cells. Throw `std::runtime_error` with the DuckDB message and the first 120 characters of the SQL text. |
| L1 | `duckorm::query<Row>`, `query_one<Row>`, `scalar<T>`, `execute`, `execute_count`, `Transaction`, `Appender` | Run one statement and decode typed rows. |
| L2 | `Mapper<Derived, Domain, RowParams, Id>` | Single-table CRUD by typed key or by `SqlFragment`. |
| L3 | `expr::`, `clause::` | Composable predicates and clauses for scope, filter, sort, and page. |

### 3.2 Store rule

A store file contains SQL text, row structs, and domain conversion. It calls only duckorm L1 to
L3. It contains zero `duckdb_` symbols. `database.cpp` and the duckorm sources are the only
files in storage that call the DuckDB C API.

### 3.3 Read query example

```cpp
struct LabelBucketRow {
  std::string label_key;
  int64_t     count = 0;
  using Columns = duckorm::RowColumns<&LabelBucketRow::label_key, &LabelBucketRow::count>;
};

auto rows = duckorm::query<LabelBucketRow>(conn, label_bucket_query);
```

`query<Row>` checks `duckdb_column_count` against the member count and each
`duckdb_column_type` against the member type before it reads row 0. A mismatch throws
`std::runtime_error` that names the column index, the DuckDB type, and the member type.

### 3.4 Layer boundary

```text
ui/       -> app services, Storage facade store methods, row DTOs
app/      -> Storage::OpenSession, store methods, StorageMaintenance, domain values
sleeve/   -> FilterNode (domain AST), sleeve elements
storage/  -> duckorm, Mapper, Store, StatsDimension, LibrarySearchPredicates, SQL text
```

## 4. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| S1 | Typed statements and rows in duckorm; `MaskAssetStore` on duckorm | duckorm, mask asset store | none | 1000-1400 | complete (#786) |
| S2 | Typed-key Mapper and one transaction style; `CommitGraphStore`, `FolderMapper`, `ImageStore` on duckorm | Mapper, edit history store, sleeve mappers | S1 | 1300-1800 | complete (#787) |
| S3 | `ElementStore` read paths on typed rows | element store, album query | S1 | 1000-1500 | planned (#788) |
| S4 | One stats dimension definition; filter compiler in storage | storage query, sleeve filter, element store | S3 | 1200-1800 | planned (#789) |
| S5 | Search predicates in storage; domain values in service signatures | storage query, app filter and browse services, library module | S4 | 1200-1800 | planned (#790) |
| S6 | Storage owns connections; maintenance API; boundary check in CI | Storage facade, app services, package backend, CI | S2, S5 | 1300-1900 | planned (#791) |

Each phase keeps its upper estimate under 2000 lines. S3 and S4 are separate phases because
S4 moves files between layers, and the move adds removed and added lines to one diff.

## 5. Detailed phases

### Phase S1: Typed statements and rows in duckorm

**Problem that this phase solves.** duckorm reads only single-table rows with a limited type
set and binds only five value kinds. `MaskAssetStore` therefore owns a second statement class
and reads BLOB and FLOAT cells with the C API.

**Objective and deliverables**

- `duckorm::Statement` and `duckorm::Result` replace `duckorm::PreparedStatement`.
- `query<Row>`, `query_one<Row>`, `scalar<T>`, and `execute_count` decode and count typed
  results.
- `BindValue` holds BLOB values.
- `MaskAssetStore` uses only duckorm.

**Inputs and prerequisites**

- `main` at or after `307d68737`.
- `DuckormStatementTest` and `MaskAssetStoreTest` pass before the change.

**Modules, files, and APIs**

- Proposed `alcedo_studio/src/include/storage/mapper/duckorm/duckdb_statement.hpp` and
  `alcedo_studio/src/storage/mapper/duckorm/duckdb_statement.cpp`:
  - `Statement(duckdb_connection& conn, std::string_view sql)`: prepares. Throws on a prepare
    error.
  - `Bind(idx_t index, T value)` for `int32_t`, `int64_t`, `uint8_t`, `uint32_t`, `uint64_t`,
    `float`, `double`, `bool`, `std::string_view`, `std::span<const std::uint8_t>` (BLOB),
    `std::nullopt_t`, and `std::optional<T>` of each scalar type.
  - `BindFragment(idx_t start_index, const SqlFragment& fragment)`.
  - `Execute() -> Result&`: the result stays valid until the next `Execute` or destruction.
  - `Result::RowCount()`, `ColumnCount()`, `RowsChanged()`, `Get<T>(idx_t column, idx_t row)`.
    `Get<T>` throws on a NULL cell unless `T` is `std::optional<U>`. BLOB reads as
    `std::vector<std::uint8_t>`.
- Proposed `duckorm::RowColumns<auto... Members>` in `duckdb_statement.hpp`.
- `duckdb_orm.hpp`: add `query<Row>`, `query_one<Row>`, `scalar<T>`, `execute_count`. Keep
  `select_int64` and `select_string` as calls of `scalar<T>` until S3 replaces their callers.
- `duckdb_expr.hpp`: add `std::vector<std::uint8_t>` to `BindValue`. Add `expr::param` for a
  BLOB span.
- `duckdb_types.hpp`: delete `duckorm::PreparedStatement`. Its users
  (`folder_mapper.cpp:100`, `duckdb_orm.cpp`) move to `Statement`.
- `mask_asset_store.cpp`: delete the local `PreparedStatement`, `ReadText`, and direct cell
  reads.

**Data rules and invariants**

- The `MaskAsset` table, its column types, `kMaskAssetFormatVersion`, and the zlib codec stay
  unchanged.
- `Put` writes once for each key. A put of a stored key returns `false` and writes nothing.
- `Read` keeps every corruption check and every error message text that `MaskAssetStoreTest`
  asserts.
- `DeleteKeysNotIn` runs in one `duckorm::Transaction`.

**Implementation steps**

1. Add `Statement` and `Result` with the bind and read overloads. Each failed DuckDB call
   throws `std::runtime_error` with the DuckDB message and the SQL head.
2. Add `RowColumns` and the type check of `query<Row>`. Accept these member types: integer
   types, `float`, `double`, `bool`, `std::string`, `std::vector<std::uint8_t>`, and
   `std::optional` of each.
3. Add `query<Row>`, `query_one<Row>`, `scalar<T>`, and `execute_count` on `Statement`.
4. Move the internals of `execute_query`, `select_by_query`, and `bind_fragment_values` onto
   `Statement`. Keep their public signatures for callers that S2 and S3 change.
5. Delete `duckorm::PreparedStatement` and update its two users.
6. Rewrite `MaskAssetStore::Put` with `execute_count` and a BLOB bind, `Read` with
   `query_one<MaskAssetRow>`, `ListKeys` with `query<MaskAssetKeyRow>`, and `DeleteKeysNotIn`
   with `Transaction` and one reused `Statement`.

**Primary success call chain**

```text
MaskAssetStore::Read(key)
  -> duckorm::query_one<MaskAssetRow>(conn, SqlFragment{select text, {key}})
  -> Statement::Execute -> RowColumns type check -> decode row 0
  -> FailCorrupt checks -> zlib inflate -> MakeMaskAssetKey check -> MaskAsset
```

**Primary failure and restore call chain**

```text
MaskAssetStore::DeleteKeysNotIn(keep)
  -> Transaction tx(conn) -> Statement DELETE ... Execute fails on row k
  -> std::runtime_error -> ~Transaction runs ROLLBACK
  -> the caller receives the DuckDB message; the table keeps every row
```

**Tests and evidence**

- `DuckormStatementTest`, proposed cases:
  - `QueryDecodesRowsIntoMemberPointerColumns`: two rows of (VARCHAR, BIGINT) decode in order.
  - `QueryRejectsColumnCountMismatch`: a three-column SELECT into a two-member row throws and
    names the counts.
  - `QueryRejectsColumnTypeMismatch`: a VARCHAR column into an `int64_t` member throws and names
    column 0.
  - `NullCellReadsAsNulloptForOptionalMember` and `NullCellThrowsForPlainMember`.
  - `BlobBindAndReadKeepsBytes`: bytes 0x00 to 0xFF round-trip.
  - `FloatAndUtinyintColumnsDecode`.
  - `ExecuteCountReportsChangedRows`: an `INSERT ... ON CONFLICT DO NOTHING` of a stored key
    reports 0.
- `MaskAssetStoreTest`: every current case passes unchanged.
- Source check: `grep -c "duckdb_" alcedo_studio/src/storage/store/mask_asset/mask_asset_store.cpp`
  reports 0.

**Build and run commands**

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DuckORM Storage DuckormStatementTest DuckormExprTest MaskAssetStoreTest MapperCrtpRoundtripTest
ctest --test-dir build/debug --output-on-failure -R "DuckormStatementTest|DuckormExprTest|MaskAssetStoreTest|MapperCrtpRoundtripTest"
```

**Exit criteria**

- [x] `duckorm::Statement` is the only prepared-statement owner in the repository.
- [x] `mask_asset_store.cpp` has zero `duckdb_` symbols.
- [x] The new `DuckormStatementTest` cases and all `MaskAssetStoreTest` cases pass.

**Expected diff:** 1000-1400 lines.

**Completion record:** see section 8.

##### Phase S1 completion record (2026-11-09)

**Status:** complete. Branch `refactor/duckorm-typed-statements-786` from `main` at `2843077e3`.

**Changed modules:**

| Area | Files | Change |
| --- | --- | --- |
| duckorm L0 | `include/storage/mapper/duckorm/duckdb_statement.hpp`, `storage/mapper/duckorm/duckdb_statement.cpp` (new) | `Statement`, `Result`, `CellScalar` / `CellValue`, `MemberKind`, `ColumnTypeFitsMember`, `RowColumns`, `TypedRow`, `RequireRowColumns` / `DecodeRow` / `DecodeRows` |
| duckorm L1 | `duckdb_orm.hpp`, `duckdb_orm.cpp` | `query<Row>`, `query_one<Row>`, `scalar<T>`, `execute_count`; every statement of `insert`, `update`, `upsert_rows`, `remove`, `select`, `select_by_query`, `execute`, `execute_query` runs on `Statement` |
| duckorm values | `duckdb_expr.hpp`, `duckdb_expr.cpp` | `BindValue` holds `std::vector<std::uint8_t>` (BLOB); `expr::param(std::span<const std::uint8_t>)` |
| duckorm internal | `storage/mapper/duckorm/duckdb_value.hpp` (new) | `DuckValueGuard`, `MakeFloatArrayValue`, shared by `Statement::BindFloatArray` and `Appender` |
| Deleted | `duckdb_types.hpp` (class only), `duckdb_types.cpp` (file) | `duckorm::PreparedStatement` |
| Users | `folder_mapper.cpp`, `mask_asset_store.cpp` | `Statement`; `MaskAssetStore` on `execute_count`, `query_one<MaskAssetRow>`, `query<MaskAssetKeyRow>`, `Transaction` |

**Detail decisions made during the work** (the plan left them open):

- **Column type rule.** A member type takes a column only without loss: an integer member takes
  an integer column of the same signedness and a range that fits, and a signed member also takes
  a narrower unsigned column (UINTEGER into `int64_t`). `double` takes FLOAT and DOUBLE. Every
  other member takes only its own type. So `COUNT(*)` (BIGINT) and `SELECT 1` (INTEGER) read as
  `int64_t`, and `SUM` (HUGEINT) needs a cast. The rule is `ColumnTypeFitsMember`.
- **DATE.** Section 2.2 lists DATE. `std::chrono::sys_days` is a member type and a bind type, so
  S3 can read the import-day buckets as dates.
- **`select_int64` and `select_string` are `scalar<int64_t>` and `scalar<std::string>`.** They
  now reject a column that does not fit. Two existing test reads changed with them:
  `AppenderWritesEveryValueTypeInsideTheTransaction` reads `vec[3]` (FLOAT) as `scalar<float>`,
  and `DuckDbExtensionTest.MissingExtensionReportsEachFailedAttemptWithoutDownloading` reads
  `current_setting('autoinstall_known_extensions')` (BOOLEAN) as `scalar<bool>`. Every production
  caller reads INTEGER, BIGINT, VARCHAR, or JSON (the C API reports JSON as VARCHAR;
  `SemanticStorageControllerTest` reads `supported_text_languages_json`).
- **Untyped NULL.** The C API reports `SELECT NULL` as INTEGER, so a NULL literal needs a cast
  (`NULL::VARCHAR`).
- **Error text.** Every `Statement` failure throws `"<DuckDB message or duckorm message>
  [SQL: <first 120 bytes>]"`; the cut ends at a whole UTF-8 sequence. Type errors name the column
  index, the column name, the DuckDB type, and the member type.
- **`bind_fragment_values` is deleted.** It had no caller outside duckorm;
  `Statement::BindFragment` replaces it. `execute_query` keeps its signature through
  `Statement::ExecuteDetached`, which gives the `duckdb_result` to the caller (S3 removes it).
- **DuckFieldDesc reads.** `select` and `select_by_query` keep the positional `VarTypes` decoding
  through `Result::ReadFieldCell`, which keeps DuckDB's value casts, so the Mapper rows read as
  before until S2 and S3.
- **MaskAssetStore.** The float bounds bind as DOUBLE in the `SqlFragment` (`BindValue` holds
  floating values as `double`); DuckDB casts them back to the same FLOAT values, and the key check after a read proves
  it. The store's own messages keep their text. SQL failures now carry the duckorm text instead of
  the prefix `MaskAssetStore: `, and a NULL cell (the columns are NOT NULL) fails with the duckorm
  NULL message instead of `is corrupt: a column is null`.
- **Source check wording.** `grep -c "duckdb_"` also counts the include lines
  `storage/mapper/duckorm/duckdb_*.hpp`. The check is
  `grep -n "duckdb_" mask_asset_store.cpp | grep -v "#include"`, which prints 0 lines.
- **Prepared-statement owner.** `Statement` is the only prepared-statement class.
  `commit_graph_store.cpp` still calls `duckdb_prepare` on raw handles; S2 moves it to duckorm
  (section 2.1).

**Primary success call chain:**

```text
MaskAssetStore::Read(key)
  -> duckorm::query_one<MaskAssetRow>(conn, SqlFragment{kReadSql, {key}})
  -> Statement(conn, sql) -> BindFragment(1) -> Execute -> Result&
  -> RequireRowColumns<MaskAssetRow>: column count 10, each column type once
  -> RequireAtMostOneRow -> DecodeRow (ReadChecked per member, BLOB as std::vector<uint8_t>)
  -> format version, codec, dimension checks -> zlib uncompress -> MakeMaskAssetKey check
  -> MaskAsset
```

**Primary failure and restore call chain:**

```text
MaskAssetStore::DeleteKeysNotIn(keep)
  -> ListKeys (query<MaskAssetKeyRow>)
  -> Transaction tx(conn) -> Statement DELETE ... Bind(1, key) -> Execute fails
  -> std::runtime_error("<DuckDB message> [SQL: DELETE FROM MaskAsset ...]")
  -> ~Statement releases the result and the prepared statement -> ~Transaction runs ROLLBACK
  -> the caller receives the message; the table keeps every row
```

**What was proven (executed tests, debug build):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `QueryDecodesRowsIntoMemberPointerColumns` | `DuckormStatementTest` | PASS |
| `QueryRejectsColumnCountMismatch` (names 3 and 2; empty result fails the same way) | `DuckormStatementTest` | PASS |
| `QueryRejectsColumnTypeMismatch` (column 0 BIGINT into `std::string`, column 1 VARCHAR into `int64_t`) | `DuckormStatementTest` | PASS |
| `NullCellReadsAsNulloptForOptionalMember` | `DuckormStatementTest` | PASS |
| `NullCellThrowsForPlainMember` (column, row) | `DuckormStatementTest` | PASS |
| `BlobBindAndReadKeepsBytes` (0x00 to 0xFF, empty BLOB, `expr::param` BLOB) | `DuckormStatementTest` | PASS |
| `FloatAndUtinyintColumnsDecode` (+ lossless widening accepted, narrowing rejected) | `DuckormStatementTest` | PASS |
| `ExecuteCountReportsChangedRows` (`ON CONFLICT DO NOTHING` of a stored key reports 0) | `DuckormStatementTest` | PASS |
| `QueryOneReturnsNulloptForNoRowAndThrowsForTwoRows` | `DuckormStatementTest` | PASS |
| `ScalarRejectsColumnTypeAndColumnCountMismatch` (DOUBLE into text, two columns, HUGEINT) | `DuckormStatementTest` | PASS |
| `DateColumnDecodesAndBindsAsSysDays` | `DuckormStatementTest` | PASS |
| `StatementRebindsAndReportsPrepareAndBindErrors` (reuse, C string as VARCHAR, 120-byte SQL head) | `DuckormStatementTest` | PASS |
| Every current `MaskAssetStoreTest` case, unchanged | `MaskAssetStoreTest` | 5/5 PASS |
| `mask_asset_store.cpp` without `duckdb_` symbols | source search | 0 lines outside `#include` |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DuckORM Storage DuckormStatementTest DuckormExprTest MaskAssetStoreTest MapperCrtpRoundtripTest DuckDbExtensionTest   (exit 0)
ctest --test-dir build/debug --output-on-failure -R "DuckormStatementTest|DuckormExprTest|MaskAssetStoreTest|MapperCrtpRoundtripTest|DuckDbExtensionTest"   (exit 0)
ctest --test-dir build/debug --output-on-failure -R "^(SemanticStorageControllerTest|AiStorageControllerTest|SleeveFilterCompileTest|SleeveFilterFactoryTest|FilterServiceTest|AlbumQueryTest|LibrarySearchRecallTest|LibrarySearchColumnsTest|PipelineMapperTest|ProjectPackageBackendTest|ProjectCompatibilityTest|CommitGraphTest)\."   (exit 8, one failure from main, below)
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
```

Suite totals: `DuckormStatementTest` 24/24, `DuckormExprTest` 10/10, `MaskAssetStoreTest` 5/5,
`MapperCrtpRoundtripTest` 4/4, `DuckDbExtensionTest` 2/2. Caller suites: 260 passed, 1 skipped
(`AlbumQueryTest.LargeLibraryPagesStayBoundedAndRecordTimings`), 1 disabled, 1 failed:
`SemanticStoreTest.ExistingSemanticModelWithoutActiveColumnPromotesLatestModel` fails on `main`
in the library marks migration before any duckorm read (#793). The full suite is for the user to start;
this record lists the targeted suites.

**Checklist / exit condition:** all three boxes checked.

**LOC note:** `duckdb_statement.hpp` 434, `duckdb_statement.cpp` 576, `duckdb_orm.cpp` 471
(was 768), `mask_asset_store.cpp` 186 (was 249), `duckorm_statement_test.cpp` 582. The diff is
1697 added and 634 removed lines in 15 files (the plan document included). Every changed file
is under 1000 lines.

**Remaining gaps:** `CommitGraphStore`, `FolderMapper` text WHERE clauses, and the Mapper
transactions are S2 (#787). `ElementStore` reads and the callers of `execute_query`,
`select_by_query`, `select_int64`, and `select_string` are S3 (#788).

### Phase S2: Typed-key Mapper and one transaction style

**Problem that this phase solves.** The Mapper API takes text WHERE clauses and key format
strings, so stores interpolate values into SQL text. Three transaction styles exist, and Mapper
batch methods cannot run inside a store transaction. `CommitGraphStore` has 73 direct C API
calls.

**Objective and deliverables**

- Mapper methods take a typed key or a `SqlFragment`.
- Mapper methods run inside the caller transaction.
- `CommitGraphStore`, `FolderMapper`, `ElementMapper`, `FileMapper`, `ImageMapper`, and
  `ImageStore` use only duckorm with bound values.
- Every storage transaction uses `duckorm::Transaction`.

**Inputs and prerequisites**

- S1 complete.

**Modules, files, and APIs**

- `alcedo_studio/src/include/storage/mapper/mapper.hpp`:
  - Replace `PrimeKeyClause()` (format string) with `PrimaryKeyColumn()` (column name).
    `RemoveById`, `UpdateParams`, and `Get(Id)` build `expr::column_eq(PrimaryKeyColumn(), id)`
    with a bound value.
  - Delete `Get(const char*)`, `GetParams(const char*)`, `GetParamsByQuery(const char*)`,
    `GetByQuery(const char*)`, `GetByPredicate(std::string&&)`, and
    `RemoveByClause(const std::string&)`.
  - Add `GetById(Id) -> std::optional<RowParams>` and `GetWhere(const SqlFragment&)`.
  - Rename `InsertParamsBatch` and `UpdateParamsBatch` behavior: they run inside the caller
    transaction and open none.
- `duckdb_orm.hpp`: delete the `const char* where_clause` overloads of `select`, `update`, and
  `remove`.
- Each Derived mapper (`edit_commit_mapper`, `version_ref_mapper`, `image_edit_state_mapper`,
  `element_mapper`, `file_mapper`, `folder_mapper`, `image_mapper`, `pipeline_mapper`,
  `recovery_metadata_mapper` callers): supply `PrimaryKeyColumn()`.
- `commit_graph_store.cpp`: delete `ExecuteOrThrow`, `QueryUint64`, and `SqlQuote`. Every
  statement uses `Statement`, `query<Row>`, `scalar<T>`, or Mapper typed-key methods.
- `folder_mapper.cpp`: composite-key deletes use `expr::and_` of two `column_eq` values and
  `expr::in_list`.
- `image_store.cpp`: `RemoveImageByType` uses `column_eq`. Delete `RemoveImageByPath` (section
  2.3).

**Data rules and invariants**

- All persisted rows and SQL results stay equal to the current results.
- `CommitGraphStore::Materialize` stays one transaction. A failure leaves every prior row.
- Hash, root id, and version id values are always bound parameters.

**Implementation steps**

1. Change the Mapper key API and the Derived mappers.
2. Update each caller of a deleted overload to a typed-key or `SqlFragment` call.
3. Replace each `begin_transaction`/`commit_transaction`/`rollback_transaction` triplet in
   storage with `duckorm::Transaction`.
4. Rewrite `CommitGraphStore` reads with row structs: `MaterializedHistoryLabelRow`,
   `RootPipelineStateRow`, `AttributionRow`.
5. Rewrite `Materialize` and `DeleteUnreachableCommits` deletes with bound `in_list` or a
   reused `Statement`.
6. Delete `ImageStore::RemoveImageByPath` and its declaration.

**Primary success call chain**

```text
CommitGraphStore::Materialize(m)
  -> m.Validate() -> Transaction tx
  -> InsertCommitIfAbsent / InsertAttributionIfAbsent (bound Statement)
  -> DELETE VersionRef WHERE element_id = ? AND version_id NOT IN (bound list)
  -> version_ref_mapper_.UpsertParamsRows -> image_edit_state_mapper_ upsert -> tx.commit()
```

**Primary failure and restore call chain**

```text
Materialize -> UpsertVersionRef fails -> std::runtime_error
  -> ~Transaction ROLLBACK -> EditCommit, VersionRef, ImageEditState keep prior rows
  -> EditorMiniGitMaterializer receives the error
```

**Tests and evidence**

- `MapperCrtpRoundtripTest`: add `GetByIdReturnsNulloptForMissingKey`,
  `RemoveByIdBindsTextKeyWithQuote` (a key `a'b` removes exactly one row), and
  `BatchInsertJoinsCallerTransaction` (a rollback after `InsertParamsRows` leaves zero rows).
- `CommitGraphTest`: all current cases pass. Add
  `MaterializeFailureRollsBackVersionRefDelete`: inject a failing upsert and assert the prior
  VersionRef rows remain.
- `ProjectCompatibilityTest`: old project fixtures open and load their graphs.
- `PipelineMapperTest` passes.
- Source check: zero `duckdb_` symbols and zero `std::format` SQL in
  `commit_graph_store.cpp`, `folder_mapper.cpp`, `image_store.cpp`.

**Build and run commands**

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target Storage SleeveMapper MapperCrtpRoundtripTest CommitGraphTest PipelineMapperTest ProjectCompatibilityTest MaskAssetStoreTest
ctest --test-dir build/debug --output-on-failure -R "MapperCrtpRoundtripTest|CommitGraphTest|PipelineMapperTest|ProjectCompatibilityTest|MaskAssetStoreTest"
```

**Exit criteria**

- [x] `mapper.hpp` and `duckdb_orm.hpp` accept WHERE input only as `SqlFragment`.
- [x] Storage has one transaction style.
- [x] `commit_graph_store.cpp` has zero `duckdb_` symbols.
- [x] `RemoveImageByPath` is deleted.

**Expected diff:** 1300-1800 lines.

**Completion record:** see section 8.

##### Phase S2 completion record (2026-11-09)

**Status:** complete. Branch `refactor/storage-typed-key-mapper-787`, stacked on the S1 branch
`refactor/duckorm-typed-statements-786` (PR #794).

**Changed modules:**

| Area | Files | Change |
| --- | --- | --- |
| Mapper L2 | `include/storage/mapper/mapper.hpp` | `MapperKey` concept; `KeyEquals(id)` builds `(PrimaryKeyColumn() = ?)`; `GetById`, `GetWhere`, `GetRowsByQuery`, `RemoveById`, `RemoveByIds` (integer keys, `in_list`), `RemoveWhere`, `UpdateParams`, `UpdateParamsRows`; `FieldReflectable::PrimaryKeyColumn` |
| Mapper keys | 13 Derived mapper headers | `prime_key_clause_` format strings become `primary_key_column_` column names |
| duckorm L1 | `duckdb_orm.hpp`, `duckdb_orm.cpp` | the `const char* where_clause` overloads of `select`, `update`, `remove` and the public `begin_transaction` / `commit_transaction` / `rollback_transaction` are deleted; `Transaction` runs BEGIN and COMMIT itself |
| duckorm L3 | `duckdb_expr.hpp`, `duckdb_expr.cpp` | `expr::in_list(operand, std::span<const std::string>)` binds each text value |
| duckorm types | `duckdb_types.hpp` | `duckorm::Connection` names the connection handle for stores |
| Edit history | `commit_graph_store.hpp`, `commit_graph_store.cpp` | every statement on `Statement`, `execute`, `scalar<T>`, `query<Row>`, `query_one<Row>`, or Mapper key methods; row structs `MaterializedHistoryLabelRow`, `RootPipelineStateRow`, `AttributionRow`, `ImageEditStateIdRow`, `StoredHistoryJsonRow`; `ExecuteOrThrow`, `QueryUint64`, `SqlQuote` deleted |
| Sleeve mappers | `folder_mapper.cpp`, `file_mapper.cpp`, `element_mapper.*`, `element_id_mapper.*` | bound keys and `in_list`; `InsertFolderContents` binds `(?, ?)` rows |
| Image | `image_mapper.cpp`, `image_store.*` | bound name, path, and type; `AddImages` and `UpdateImages` open a `Transaction`; `RemoveImageByPath` deleted |
| Pipeline | `pipeline_mapper.cpp` | `GetById` |
| Element store | `element_store.cpp` | `AddElements` and `UpdateElements` on `Transaction`; `GetFolderChildren` binds the folder id |

**Detail decisions made during the work** (the plan left them open):

- **Method names.** Row methods: `GetById`, `GetWhere`, `GetRowsByQuery`, `RemoveById`,
  `RemoveByIds`, `RemoveWhere`, `UpdateParams`, `UpdateParamsRows`. Domain methods keep
  `GetByPredicate(SqlFragment)` and `GetByQuery(SqlFragment)`. `RemoveByClause(SqlFragment)`
  became `RemoveWhere`, and `UpdateParamsBatch` became `UpdateParamsRows`, which states that it
  joins the caller transaction. `InsertParamsBatch` is deleted: `InsertParamsRows` is the same
  operation inside the caller transaction. The aliases `Get(const char*)`,
  `GetByQuery(const char*)`, `Remove(Id)`, and `Update(Id, RowParams)` are deleted.
- **Key types.** `MapperKey` accepts integers that fit `int64_t` (bound as BIGINT) and
  `std::string` (bound as VARCHAR). `GetById` throws when a key matches several rows, which
  only a table without a key constraint (`FileImage`, `FolderContent` by `folder_id`) can hold.
- **Transactions.** The public BEGIN, COMMIT, and ROLLBACK functions are deleted, so
  `duckorm::Transaction` is the only way to open a transaction. `ImageStore::AddImages` and
  `UpdateImages` open the transaction that the Mapper batch opened before.
- **Integer id lists.** `RemoveByIds`, `RemoveContentByIds`, `RemoveFolderContents`, and the
  root existence check use `expr::in_list` with integer literals, as section 2.2 decided for id
  lists. Text values (version ids, commit hashes, root ids, names, paths) are binds.
- **Connection type in stores.** `commit_graph_store.cpp` names the connection
  `duckorm::Connection`, an alias of the C API handle in `duckdb_types.hpp`, so the source
  check reports 0 lines. S6 uses the same alias for the store session.
- **`ListImageElementIds`.** It reads `SELECT element_id` into `ImageEditStateIdRow`; the old
  read loaded every ImageEditState row with its checkpoint JSON to return the ids.
- **`DeleteUnreachableCommits`.** One DELETE for the attribution rows and one for the commit
  rows, each with a bound `in_list` of the hashes, replace one pair of statements per hash.
- **Raw-SQL mapper reads.** `ElementMapper::GetElementsInFolderByFilter(std::wstring)` became
  `GetElementsByQuery(SqlFragment)`, and `ElementIdMapper::GetElementIdsByQuery` takes a
  `SqlFragment`. `GetElementByName`, `GetImageByName`, and `GetImageByPath` bind their text;
  before, each pasted the text unquoted into the WHERE clause, so each call failed in DuckDB.
- **`FileMapper::Update` on `FileImage`.** The statement keeps its text with the key now bound.
  DuckDB rejects it because `FileImage` has no key constraint; #795 records the defect.

**Primary success call chain:**

```text
EditorMiniGitMaterializer -> CommitGraphStore::Materialize(m)
  -> m.Validate() -> duckorm::Transaction tx(conn)
  -> InsertCommitIfAbsent: commit_mapper_.GetById(hash) -> Mapper::GetWhere((commit_hash = ?))
     -> commit_mapper_.Insert (prepared INSERT, field binds)
  -> InsertAttributionIfAbsent: duckorm::execute(INSERT OR IGNORE ... VALUES (?, ?, ?))
  -> version_ref_mapper_.RemoveWhere(((element_id = ?) AND (NOT (version_id IN (?, ...)))))
  -> version_ref_mapper_.UpdateParams(id, row) -> duckorm::update(... WHERE (version_id = ?))
  -> image_edit_state_mapper_.UpdateParams -> tx.commit()
```

**Primary failure and restore call chain:**

```text
Materialize -> VersionRef delete and upserts ran -> ImageEditState upsert fails
  -> std::runtime_error("<DuckDB message> [SQL: INSERT INTO ImageEditState ...]")
  -> ~Statement -> ~Transaction runs ROLLBACK
  -> EditCommit, EditCommitAttribution, VersionRef, ImageEditState keep the prior rows
  -> EditorMiniGitMaterializer receives the error
```

**What was proven (executed tests, debug build):**

| Required name / criterion | Target | Result |
| --- | --- | --- |
| `GetByIdReturnsNulloptForMissingKey` | `MapperCrtpRoundtripTest` | PASS |
| `RemoveByIdBindsTextKeyWithQuote` (key `a'b` removes one of three VersionRef rows) | `MapperCrtpRoundtripTest` | PASS |
| `BatchInsertJoinsCallerTransaction` (rows visible inside, 0 rows after rollback, 2 after commit) | `MapperCrtpRoundtripTest` | PASS |
| `GetImageByPathBindsPathWithQuote` (path `D:/O'Brien/frame.dng`, name read) | `MapperCrtpRoundtripTest` | PASS |
| `MaterializeFailureRollsBackVersionRefDelete` (failing ImageEditState upsert after the VersionRef delete; the deleted Version row and the prior state remain; the same capture then commits) | `CommitGraphTest` | PASS |
| Every current `CommitGraphTest` and `MapperCrtpRoundtripTest` case | both | PASS |
| `ProjectCompatibilityTest`, `PipelineMapperTest` | `tests/app` | PASS |
| Zero `duckdb_` symbols outside `#include` and zero `std::format` in `commit_graph_store.cpp`, `folder_mapper.cpp`, `image_store.cpp` | source search | 0 lines each |
| One transaction style: zero `begin_transaction` / `"BEGIN` / `"COMMIT` / `"ROLLBACK` outside duckorm in storage | source search | 0 lines |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DuckORM Storage SleeveMapper MapperCrtpRoundtripTest CommitGraphTest DuckormStatementTest DuckormExprTest MaskAssetStoreTest   (exit 0)
ctest --test-dir build/debug --output-on-failure -R "^(MapperCrtpRoundtripTest|CommitGraphTest|DuckormStatementTest|DuckormExprTest|MaskAssetStoreTest)\."   (exit 0)
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineMapperTest ProjectCompatibilityTest LibraryHistoryAndExportTest AlbumQueryTest FilterServiceTest SemanticStorageControllerTest AiStorageControllerTest ProjectPackageBackendTest LibrarySearchColumnsTest LibrarySearchRecallTest SleeveFilterCompileTest SleeveFilterFactoryTest   (exit 0)
ctest --test-dir build/debug --output-on-failure -R "^(PipelineMapperTest|ProjectCompatibilityTest|LibraryHistoryAndExportTest|AlbumQueryTest|FilterServiceTest|SemanticStorageControllerTest|AiStorageControllerTest|ProjectPackageBackendTest|LibrarySearchColumnsTest|LibrarySearchRecallTest|SleeveFilterCompileTest|SleeveFilterFactoryTest)\."   (exit 8, one failure from main, below)
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
```

Suite totals: `MapperCrtpRoundtripTest` 8/8, `CommitGraphTest` 49/49, `DuckormStatementTest` 24/24,
`DuckormExprTest` 10/10, `MaskAssetStoreTest` 5/5 (96/96). Caller suites: 224 passed, 1 skipped
(`AlbumQueryTest.LargeLibraryPagesStayBoundedAndRecordTimings`), 1 disabled, 1 failed:
`SemanticStoreTest.ExistingSemanticModelWithoutActiveColumnPromotesLatestModel`, the failure on
`main` recorded in #793. The full suite is for the user to start; this record lists the targeted
suites.

**Checklist / exit condition:** all four boxes checked.

**LOC note:** `mapper.hpp` 288 (was 371), `commit_graph_store.cpp` 563 (was 670),
`folder_mapper.cpp` 143, `mapper_crtp_roundtrip_test.cpp` 222. Two touched files were above
1000 lines before this phase: `element_store.cpp` 1273 (S3 moves its read paths onto row
structs) and `commit_graph_test.cpp` 1421 (one test added). The code diff is 637 added and 705
removed lines in 30 files, below the 1300-1800 estimate: the Mapper key change made most caller
edits one line.

**Remaining gaps:** `ElementStore` reads (formatted SQL and C API cells) are S3 (#788). #795
(`FileMapper::Update` on `FileImage`) needs a table key and a migration.

### Phase S3: ElementStore read paths on typed rows

**Problem that this phase solves.** The album, search, and stats reads in `ElementStore` read
42 cells with the C API (`ReadSearchResultRow`, `ReadMatchSetBuckets`,
`ReadMatchSetMarkCounts`, `ReadMatchSetLabelBuckets`, `ReadAlbumQueryFromMatchSet`). A column
order change in the SQL text shifts every read silently.

**Objective and deliverables**

- Each `ElementStore` read uses `query<Row>` or `scalar<T>`.
- `MatchSetTable` drops its table with `duckorm::execute`.
- `ExecuteQueryOrThrow`, `ReadVarchar`, and `RunScalarInt64` are deleted.

**Inputs and prerequisites**

- S1 complete. S3 can run in parallel with S2.

**Modules, files, and APIs**

- `alcedo_studio/src/storage/store/sleeve/element_store.cpp`: row structs
  `SearchResultDbRow`, `StatsBucketRow` (grouping id, label, count), `MarkCountRow`,
  `LabelBucketRow`, `AlbumGroupRow`, `AlbumPageRow`.
- `alcedo_studio/src/storage/store/sleeve/album_query.cpp`: the same change for its reads.
- `alcedo_studio/src/storage/store/database.cpp`, `store_types.cpp`: keep their C API calls
  (connection open and close). They are the permitted owners.
- `alcedo_studio/src/storage/store/ai/ai_store.cpp`: replace its one direct C API call.

**Data rules and invariants**

- Every page, total, group, and bucket comes from one match set table, as today.
- Bucket order rules stay as `element_store.cpp:205` states.
- `MatchSetTable` drops the temporary table at scope exit, also after a failed statement.

**Implementation steps**

1. Add a row struct for each read. Use `std::optional` members for nullable columns
   (`added_time`, `edited_at`, group keys).
2. Replace each read loop with `query<Row>`.
3. Replace the `DROP TABLE` C API call in `~MatchSetTable` with `duckorm::execute` inside a
   `noexcept` destructor that logs a failure.
4. Delete the local helpers.

**Primary success call chain**

```text
ElementStore::ReadAlbumQuery(scope, options, read)
  -> MatchSetTable (CREATE TEMP TABLE ... AS SELECT)
  -> query<AlbumGroupRow> / query<AlbumPageRow> / query<StatsBucketRow>
  -> AlbumQueryResult -> ~MatchSetTable DROP
```

**Primary failure and restore call chain**

```text
query<StatsBucketRow> throws (type mismatch or DuckDB error)
  -> ~MatchSetTable drops the temp table -> std::runtime_error to AlbumBrowseService
  -> LibraryModule worker reports the read error; the shown page stays
```

**Tests and evidence**

- `AlbumQueryTest`, `FilterServiceTest`, `LibrarySearchRecallTest`,
  `LibrarySearchColumnsTest`, `AiStorageControllerTest`: all current cases pass.
- `AlbumQueryTest`, proposed `StatsBucketsMatchPageTotalAfterTypedRead`: the sum of camera
  buckets equals the total for a 12-file fixture.
- Source check: zero `duckdb_` symbols in `element_store.cpp`, `album_query.cpp`,
  `ai_store.cpp`.

**Build and run commands**

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target Storage SleeveFilterService AlbumQueryTest FilterServiceTest LibrarySearchRecallTest LibrarySearchColumnsTest AiStorageControllerTest
ctest --test-dir build/debug --output-on-failure -R "AlbumQueryTest|FilterServiceTest|LibrarySearchRecallTest|LibrarySearchColumnsTest|AiStorageControllerTest"
```

**Exit criteria**

- [ ] Inside storage, only `database.cpp`, `store_types.cpp`, and duckorm call the C API.
- [ ] All listed tests pass.

**Expected diff:** 1000-1500 lines.

**Completion record:** see section 8.

### Phase S4: One stats dimension definition and the filter compiler in storage

**Problem that this phase solves.** Each stats bucket expression exists twice: in the
`ElementStore` GROUP BY and in `FilterSQLCompiler`. Only a comment links them. A change on one
side makes a clicked bucket select a different photo count than the bucket shows, and no test or
error reports it.

**Objective and deliverables**

- Proposed `StatsDimension` definitions in storage: one entry for each dimension (capture date,
  camera model, lens, rating, import day, color label, delete mark, semantic label). Each entry
  owns its group expression, its unknown label, and its bucket predicate.
- The `ElementStore` GROUP BY and the bucket filters both read the `StatsDimension` entry.
- `FilterSQLCompiler` and the bucket SQL of `filter_factory.cpp` move into storage.
- `sleeve/` keeps `FilterNode`, `FilterField`, `CompareOp`, and the bucket filter builders as
  domain values.

**Inputs and prerequisites**

- S3 complete.

**Modules, files, and APIs**

- Proposed `alcedo_studio/src/include/storage/query/stats_dimension.hpp` and
  `alcedo_studio/src/storage/query/stats_dimension.cpp`:
  `enum class StatsDimensionId`, `auto GroupExpression(StatsDimensionId, ColumnSource) ->
  SqlFragment`, `auto BucketPredicate(StatsDimensionId, const StatsBucketKey&, ColumnSource)
  -> SqlFragment`. `ColumnSource` selects the live tables (`i.`) or the match set table.
- Proposed `alcedo_studio/src/storage/query/filter_compiler.cpp` (moved from
  `sleeve/sleeve_filter/filter_sql.cpp`).
- `alcedo_studio/src/sleeve/sleeve_filter/filter_factory.cpp`: bucket builders return a
  `FilterNode` with a `StatsBucketCondition{dimension, key}`. `MakeRawSQLNode` callers for
  buckets, color labels, and the delete mark move to this condition.
- `alcedo_studio/src/include/sleeve/sleeve_filter/filter_combo.hpp`: delete the comment-only
  link and the `CameraModelLabel`, `LensLabel`, `CaptureDateLabel`, `RatingLabel` columns.
- `element_store.cpp`: `ReadMatchSetBuckets` builds its projection from `GroupExpression`.

**Data rules and invariants**

- Bucket labels, the `(unknown)` label, bucket order, and counts stay equal to the current
  results.
- For each dimension and each bucket key, the bucket predicate selects exactly the rows that
  the group expression puts in that bucket.

**Implementation steps**

1. Add `StatsDimension` with the current expressions copied from `element_store.cpp:214-222`
   and `filter_sql.cpp:64-70`. Confirm that the two copies agree before the move.
2. Change `ReadMatchSetBuckets` to use `GroupExpression`.
3. Add `StatsBucketCondition` to `FilterNode`. Change the bucket builders.
4. Move `FilterSQLCompiler` into storage. It compiles `StatsBucketCondition` through
   `BucketPredicate`.
5. Delete the label filter fields and their compiler cases.

**Primary success call chain**

```text
StatsEngine bucket click -> sleeve_filter::BuildCameraModelBucketFilter(label)
  -> FilterNode{StatsBucketCondition{kCameraModel, label}}
  -> AlbumBrowseService::ReadAlbumQuery -> storage FilterCompiler
  -> BucketPredicate(kCameraModel, label, kLiveTables) -> match set -> page and buckets
```

**Primary failure and restore call chain**

```text
FilterCompiler receives an unknown StatsDimensionId
  -> std::invalid_argument("Stats dimension: unknown id") -> ReadAlbumQuery fails
  -> LibraryModule keeps the prior filter and page
```

**Tests and evidence**

- Proposed `StatsDimensionTest` (`tests/storage/stats_dimension_test.cpp`):
  `EachBucketPredicateSelectsItsBucketCount`. For each dimension, the fixture database (24
  files with empty, NULL, and repeated camera, lens, date, rating, label, and mark values) runs
  the stats read. Then for each returned bucket the test runs the bucket filter and asserts the
  page total equals the bucket count.
- `SleeveFilterCompileTest`, `SleeveFilterFactoryTest`: update the expected SQL of moved cases.
- `AlbumQueryTest`, `FilterServiceTest`, `AlbumBackendStatsFilterTest`: all pass.

**Build and run commands**

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target Storage SleeveFilter SleeveFilterService StatsDimensionTest SleeveFilterCompileTest SleeveFilterFactoryTest AlbumQueryTest FilterServiceTest AlbumBackendStatsFilterTest
ctest --test-dir build/debug --output-on-failure -R "StatsDimensionTest|SleeveFilterCompileTest|SleeveFilterFactoryTest|AlbumQueryTest|FilterServiceTest|AlbumBackendStatsFilterTest"
```

**Exit criteria**

- [ ] Each bucket expression exists once, in `stats_dimension.cpp`.
- [ ] `StatsDimensionTest` passes for every dimension.
- [ ] `alcedo_studio/src/sleeve` has zero SQL text.

**Expected diff:** 1200-1800 lines.

**Completion record:** see section 8.

### Phase S5: Search predicates in storage and domain values in service signatures

**Problem that this phase solves.** `SleeveFilterService` in `app/` writes SQL with table and
column names (42 `expr::raw` calls). `duckorm::SqlFragment` crosses into app service signatures
and into `LibraryModule` in the UI.

**Objective and deliverables**

- Proposed `LibrarySearchPredicates` in storage builds the fuzzy text, EXIF word, date part,
  file extension, capture parameter, AI text, FTS, and semantic label predicates.
- `SleeveFilterService` parses the query text into a proposed domain value
  `LibrarySearchRequest` (terms with their kinds and the field mask). It returns a `FilterNode`
  with a proposed `LibrarySearchCondition` that holds the request. This replaces the
  `FilterNode::Type::RawSQL` node that `BuildFuzzySearchWhere` and `BuildExactFileWhere` return
  today (`sleeve_filter_service.cpp:546-556`). The storage filter compiler (S4) compiles the
  condition.
- Proposed domain value `SemanticRelevanceRequest{model_key, query_text_embedding}` replaces
  `std::optional<duckorm::SqlFragment> relevance_select` in `AlbumBrowseService` and
  `SleeveFilterService`.
- `library_module.cpp` stops including `duckdb_expr.hpp`.

**Inputs and prerequisites**

- S4 complete.

**Modules, files, and APIs**

- Proposed `alcedo_studio/src/include/storage/query/library_search_predicates.hpp` and
  `alcedo_studio/src/storage/query/library_search_predicates.cpp`:
  `auto BuildLibrarySearchPredicate(const LibrarySearchRequest&, const SearchCapabilities&)
  -> SqlFragment`. `SearchCapabilities` reports the FTS index and the active semantic model.
- `alcedo_studio/src/app/sleeve_filter_service.cpp`: keep parsing and paging. Delete SQL text.
- `alcedo_studio/src/include/app/album_browse_service.hpp`,
  `alcedo_studio/src/include/app/sleeve_filter_service.hpp`: replace `SqlFragment` parameters.
- `alcedo_studio/src/storage/store/semantic/semantic_vector_search.cpp`: build the relevance
  select from `SemanticRelevanceRequest` inside `ElementStore::ReadAlbumQuery`.
- `alcedo_studio/src/ui/alcedo_main/album_backend/library_module.cpp`: pass
  `SemanticRelevanceRequest`.

**Data rules and invariants**

- Search recall and precision stay equal to the current results.
- The semantic relevance select runs once for each read, as today.
- User text is always a bound value.

**Implementation steps**

1. Add `LibrarySearchRequest` and `SemanticRelevanceRequest` in `app/` domain headers that
   storage can include without app dependencies (proposed `storage/query/library_search_types.hpp`).
2. Move each SQL builder from `sleeve_filter_service.cpp:46-405` into
   `library_search_predicates.cpp`.
3. Change the service signatures and their callers.
4. Delete the `duckdb_expr.hpp` includes from `app/` and `ui/` files.

**Primary success call chain**

```text
SearchController -> SleeveFilterService::SearchFolderPage(text)
  -> ParseLibrarySearch(text) -> LibrarySearchRequest
  -> ElementStore::ListSearchResultPage(request, capabilities)
  -> BuildLibrarySearchPredicate -> match set -> rows -> SearchController
```

**Primary failure and restore call chain**

```text
ElementStore::ListSearchResultPage -> DuckDB error in the compiled search predicate
  -> std::runtime_error -> SleeveFilterService -> SearchController shows the search error
  -> the prior result rows stay
```

The FTS rule stays as today: the BM25 clause joins the predicate only when
`AiStore::HasUnderstandingFtsIndex` is true and both AI field groups are in the mask
(`sleeve_filter_service.cpp:536-543`). The per-field AI text clauses apply in every case.

**Tests and evidence**

- `LibrarySearchRecallTest`, `LibrarySearchColumnsTest`, `FilterServiceTest`: all current
  cases pass with unchanged expected file sets.
- Proposed `LibrarySearchPredicatesTest`: `QuoteInSearchTextIsBound` (text `O'Brien` returns
  the matching file and leaves the statement valid).
- `AlbumBackendLibraryQueryTest` passes.

**Build and run commands**

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target Storage SleeveFilterService LibrarySearchPredicatesTest LibrarySearchRecallTest LibrarySearchColumnsTest FilterServiceTest AlbumBackendLibraryQueryTest
ctest --test-dir build/debug --output-on-failure -R "LibrarySearchPredicatesTest|LibrarySearchRecallTest|LibrarySearchColumnsTest|FilterServiceTest|AlbumBackendLibraryQueryTest"
```

**Exit criteria**

- [ ] `alcedo_studio/src/app` and `alcedo_studio/src/ui` have zero `duckorm::` references.
- [ ] `FilterNode::Type::RawSQL` and `raw_binds_` are deleted.
- [ ] All listed tests pass.

**Expected diff:** 1200-1800 lines.

**Completion record:** see section 8.

### Phase S6: Storage owns connections, maintenance API, and boundary check

**Problem that this phase solves.** Any code that holds `Storage` can read the connection and
build stores or run SQL. `ProjectService` and `project_package_backend` run SQL through the C
API in `app/`. The storage rule depends on review alone, so `CommitGraphStore` stayed
unmigrated after the rule existed.

**Objective and deliverables**

- Proposed `Storage::OpenSession() -> StorageSession`: holds the database lock and gives
  `CommitGraphStore&`, `MaskAssetStore&`, and `PipelineMapper&` for the session lifetime.
- Proposed `StorageMaintenance` in storage: `ReadDataSummary()`, `WriteSnapshot(path)`,
  `DropHnswIndexesOfOlderProject()`, `HasHnswIndex()`, `CurrentCatalog()`.
- `Database::GetConnectionGuard` and `Storage::GetDatabase` leave the public API of
  `Storage`.
- Proposed `scripts/check_storage_boundary.py` runs in CI.

**Inputs and prerequisites**

- S2 and S5 complete.

**Modules, files, and APIs**

- `alcedo_studio/src/include/sleeve/storage.hpp`, `alcedo_studio/src/sleeve/storage.cpp`:
  add `OpenSession`, `Maintenance`. Remove `GetDatabase`.
- Proposed `alcedo_studio/src/include/storage/store/storage_maintenance.hpp` and
  `alcedo_studio/src/storage/store/storage_maintenance.cpp`: receive the code from
  `project_service.cpp:341-403` and `project_package_backend.cpp:815-1054`.
- `alcedo_studio/src/include/app/project_package_backend.hpp`: remove `duckdb_connection`
  parameters.
- `pipeline_service.cpp`, `committed_snapshot_cache.cpp`, `editor_mini_git_commit_writer.cpp`,
  `editor_mini_git_journal_recovery.cpp`, `editor_mini_git_materializer.cpp`,
  `project_mask_asset_reader.cpp`: use `StorageSession`.
- `library_commands.cpp:40`: replace the include of `image_search_columns.hpp` with a storage
  query header that declares `ParseCaptureDateTime`.
- Proposed `scripts/check_storage_boundary.py` and a CI step in `.github/workflows/`. The script
  scans `alcedo_studio/src` outside `storage/` and `include/storage/` and fails on:
  `#include <duckdb.h>`, `#include "storage/mapper/`, `duckdb_[a-z_]+\(`,
  `GetConnectionGuard`, and `duckorm::`.
- Tests that use the C API for fixture setup keep it. The script scans `alcedo_studio/src` only.

**Data rules and invariants**

- The package snapshot bytes and the diagnostics summary fields stay equal to the current
  results.
- A `StorageSession` holds the database lock for its whole lifetime. Stores from a session are
  valid only during that lifetime.

**Implementation steps**

1. Add `StorageMaintenance` and move the maintenance SQL into it.
2. Add `StorageSession` and change the six app callers.
3. Remove `GetDatabase` from `Storage` and fix each compile error with a session or a store
   method.
4. Add the check script and its CI step. Run it on the branch and record zero findings.

**Primary success call chain**

```text
EditorMiniGitMaterializer::Materialize
  -> storage.OpenSession() (lock) -> session.CommitGraph().Materialize(m)
  -> session ends (unlock)
```

**Primary failure and restore call chain**

```text
StorageMaintenance::WriteSnapshot fails at COPY FROM DATABASE
  -> DETACH runs in the cleanup path -> std::runtime_error to ProjectService
  -> the package write reports the error; the open project database stays unchanged
```

**Tests and evidence**

- `ProjectPackageBackendTest`, `ProjectCompatibilityTest`, `PipelineMapperTest`,
  `CommitGraphTest`, `MaskAssetStoreTest`: all pass.
- Proposed `StorageMaintenanceTest`: `DataSummaryCountsRowsOfEveryTable` and
  `SnapshotFailureKeepsSourceDatabase`.
- Proposed CI check: `python scripts/check_storage_boundary.py` exits 0 on the branch. A test
  input file with a `duckdb_query(` call in `alcedo_studio/src/app` makes it exit 1.

**Build and run commands**

```bash
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target Storage StorageMaintenanceTest ProjectPackageBackendTest ProjectCompatibilityTest PipelineMapperTest CommitGraphTest MaskAssetStoreTest
ctest --test-dir build/debug --output-on-failure -R "StorageMaintenanceTest|ProjectPackageBackendTest|ProjectCompatibilityTest|PipelineMapperTest|CommitGraphTest|MaskAssetStoreTest"
python scripts/check_storage_boundary.py
```

**Exit criteria**

- [ ] Code outside storage has zero `duckdb_` calls, zero `duckorm::` references, and zero
      connection guard reads.
- [ ] The boundary check runs in CI and reports zero findings.

**Expected diff:** 1300-1900 lines.

**Completion record:** see section 8.

## 6. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Old project open | Graphs, masks, labels, stats, and search results equal the current results | S2, S6 |
| Text value with a quote (`a'b`, `O'Brien`) | One bound value; the statement stays valid | S2, S5 |
| Column order change in a read query | `query<Row>` throws and names the column | S1, S3 |
| Clicked stats bucket | Page total equals the bucket count for every dimension | S4 |
| Failed write in a store transaction | All rows of the transaction roll back | S1, S2 |
| Failed package snapshot | Source database stays unchanged | S6 |
| New raw SQL outside storage | CI boundary check fails | S6 |

## 7. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| A phase diff passes 2000 lines | Diff count during implementation | Stop. Split the phase in this plan first. |
| A DuckDB column type maps to two C++ types | `query<Row>` type check rejects a valid query | Add the explicit accepted pair to the type table with a test. |
| Bucket expressions in the two copies differ before S4 | Step 1 of S4 | Stop. Open a `[BUG]` issue with the differing dimension and its counts. |
| Package snapshot bytes change | `ProjectPackageBackendTest` | Stop. Restore the statement order of the current backend. |

## 8. Completion record template

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
Evidence path:
Remaining defects or unavailable platforms:
```

## 9. Terms

| Term | Meaning |
| --- | --- |
| duckorm | The DuckDB access library in `alcedo_studio/src/storage/mapper/duckorm`. |
| Mapper | CRTP base for single-table CRUD in `storage/mapper/mapper.hpp`. |
| Store | Storage owner of one data area, for example `ElementStore`, `CommitGraphStore`. |
| Read query | A named SELECT in a store, with SQL text, a `SqlFragment` of binds, and a row struct. |
| Row struct | C++ struct that lists its result columns with `duckorm::RowColumns`. |
| Match set | Temporary table with the files that match one filter, read for page, total, groups, and buckets. |
| Stats dimension | One stats bar category (camera, lens, date, rating, import day, color label, delete mark, semantic label). |
| Storage session | Proposed object that holds the database lock and the stores for one operation. |
| Boundary check | Proposed CI script that fails on DuckDB or duckorm use outside storage. |
