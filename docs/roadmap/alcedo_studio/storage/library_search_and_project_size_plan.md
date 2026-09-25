# Library Search Performance, Recall, and Project Size Plan

Date: 2026-09-24

Status: Phases S0 and S1 complete (2026-09-24); S2–S6 not started

Primary owner: Alcedo Studio storage (Image schema, import, sleeve filter SQL) and library search.

Affected areas:

- `app/import_service` import sync and image pool cleanup
- `image/metadata_extractor`, `image/image` (`RawRuntimeColorContext` JSON)
- `edit/graph/develop_node_model`, `edit/graph/develop_color_transform` (camera profile binding)
- `edit/history/pipeline_document_checkpoint` (`PipelineRoot` state and `ComputeRootId`)
- `app/pipeline_service` (`PipelineMgmtService` load and root binding)
- `storage/mapper` Image mapper, `storage/store/sleeve/element_store` (search, stats, paging)
- `app/sleeve_filter_service` fuzzy-search WHERE builder
- `ui/alcedo_main/album_backend/search_controller`, `stats_engine`, `library_module`
- `qml/GlobalSearchDialog.qml`

Related roadmaps and notes:

- [duckorm Query Expression and Album Filter SQL Plan](duckorm_query_expression_and_album_filter_sql_plan.md)
- [Semantic Generation and Search Integration Plan](../ai/semantic_generation_search_plan.md)
- [DNG color profiles](../../../dng_color_profiles.md) (design note; Phase S2 replaces its storage section)

Delivery: one feature branch, one commit series per phase. Phases S1 and S2 do not depend on the
search phases and can land first.

Compatibility: this plan does **not** keep compatibility with existing projects. Phases S2 and S3
share one destructive project format cutover. No migration code is written. See
[Decision D1](#d1--one-destructive-format-cutover).

## Problem

A manual end-to-end test on a 927-image library (`demo.alcd`, 329 MB) found three defects.

### P1 — Search is slow and blocks the UI thread

Measured on the extracted `demo.alcd` DuckDB with the DuckDB CLI, using the SQL that
`SleeveFilterService::BuildFuzzySearchWhere` builds today (all four field groups on):

| Query | Rows matched | One `COUNT(*)` run |
| --- | --- | --- |
| `jpg` | 0 | 4.5 s |
| `2026-06-07` | 414 | 4.8 s |
| `P1000123` | 0 | 4.9 s |
| same scan, file name column only | — | 1 ms |

Cost breakdown:

- `FoldedDocumentClause` concatenates `CAST(i.metadata AS VARCHAR)` into one document and runs
  33 nested `REPLACE` calls on it for each row. That part alone takes about 2.8 s.
- About ten `json_extract` calls each parse the full metadata JSON (up to 420 KB per DNG).
- One keystroke runs the predicate twice: `CountSearchResults` + `SearchFolder`.
- `ApplyFuzzySearch` then runs the same predicate again for the thumbnail page and for five or
  six `BuildFolderStats` queries.
- `GlobalSearchDialog.qml` calls `SearchController::SearchPreview` synchronously. Only the
  semantic route runs on a worker thread. The preview therefore runs on the UI thread and
  freezes it.
- `SearchController::BuildResultRows` also does an image pool `Read` for each result row.

### P2 — Search recall and precision are poor

- `6.7` does not mean "June 7". `DateMatchClauses` drops any date group with a year below 1000.
- `6.7` matches **927 of 927** rows. The string `6.7` occurs in the color matrix numbers inside
  `CAST(i.metadata AS VARCHAR)`. The raw metadata dump destroys precision for any short numeric
  query.
- The user reports that file name queries also miss. Phase S0 reproduces this with the local
  Nikon fixture folder (file names with mixed separators such as `Nikon-D810-raw00011.nef`
  and `nikon_d3x_01.nef`).

### P3 — Project files are 20–60× too large

The tables hold about 4 MB of real data. The other ~310 MB is one DNG color profile repeated:

| Location | Total | Mean per row |
| --- | --- | --- |
| `Image.metadata` → `RawRuntimeColorContext.DngColorProfile` | 62 MB | 40 KB (DNG: 421 KB) |
| `PipelineParam.param_json` (Develop node `camera_profile.dng_profile`) | 66 MB | 71 KB |
| `ImageEditState.serialized_pipeline_state` (checkpoint copy) | 66 MB | 71 KB |
| `PipelineRoot.serialized_pipeline_state` (`pipeline_document` + `raw_color_context`) | 127 MB | 137 KB |

- 144 DNG files carry about 2.1 MB each (five copies of a 420 KB profile).
- Across 927 rows there are only **2** distinct profiles.
- RW2 rows are about 5–7 KB for each pipeline column. Those sizes are acceptable.

### P4 — Failed imports leave orphan `Image` rows

`Image` has 1563 rows. Only 927 rows have a `FileImage` reference. The other 636 rows are
`.rrdata` (632), `.xmp` (2), and `.mov` (2) files.

Root cause, in `ImportServiceImpl::SyncImports` (`app/import_service.cpp:261-270`):

1. `ImportToFolder` creates a pinned `UNSYNCED` Image in the pool before metadata extraction
   (`import_service.cpp:146`).
2. `ExtractEXIF_ToImage` rejects the file by content (`UNSUPPORTED_FORMAT`). This is correct.
   Import deliberately does not check the file extension.
3. `SyncImports` calls `fs.DeleteFileEverywhere(entry.element_id_)` for each failed entry. It
   never removes `entry.image_id_` from the image pool.
4. `image_pool_service_->SyncWithStorage()` then writes the failed Image as a normal row.

Nothing ever deletes `Image` rows that have no `FileImage` reference.

### P5 — Non-RAW files import but cannot render

- The user found JPG files in the library. They should not import.
- `MetadataExtractor::ExtractEXIF_ToImage` (`metadata_extractor.cpp`) tries LibRaw first. When
  LibRaw fails, it falls back to Exiv2 and accepts any raster that `IsImportableExiv2Raster`
  allows. This fallback accepts JPEG and TIFF on purpose.
- The only render input is `edit/input/raw_input_loader.cpp`. There is no non-RAW decode path.
  An imported JPEG therefore has metadata but no working render.

The same investigation found two related import risks:

- `FinishImport` queues `snapshot.created_` for semantic generation. Failed entries are still
  in that list (`album_backend/import_export.cpp:672-678`).
- The image pool holds 1024 entries and evicts unpinned entries by LRU, even when they are
  `UNSYNCED` (`image_pool_manager.cpp:107-132`). The pin goes away when each extraction task
  finishes. An import of more than 1024 files can evict a good Image before `SyncImports`
  writes it. The result is a `FileImage` row that points at a missing Image.

## Current behavior: DNG color profile flow

- The profile is read **only at import**:
  `PopulateDngColorMetadataFromExif` → `ReadDngColorProfile` (`metadata_extractor.cpp:936`).
- Render-time decode does **not** read it again. `RawInputLoader::FillColorContext`
  (`raw_input_loader.cpp:491-530`) fills cam_mul, make, model, lens, and warp data only.
- The GPU develop passes read the profile from the Develop node payload
  (`develop_params.camera_profile.dng_profile`) on CUDA, OpenCL, and Metal.
- `MetadataExtractor::ReadRawColorContextForRender` (`metadata_extractor.cpp:1601-1612`) can
  read a profile from the source file. No product code calls it.
- The profile is import-bound data, not a user choice. The command parsers ignore the
  `camera_profile` key (`editor_pipeline_command_service.cpp:367/409/466`).
- `ComputeRootId` hashes the document dump and the raw context dump. The profile goes into the
  hash twice.
- `DngColorProfileFingerprint` is an FNV hash of the profile JSON dump
  (`dng_color_profile.hpp:113-117`). The result cache key includes it.

The persisted copy is therefore the copy that renders. A change that stops the write must also
add a runtime read before the document reaches the renderer.

## Decisions

### D1 — One destructive format cutover

- Phases S2 and S3 change the `Image`, `PipelineParam`, `ImageEditState`, and `PipelineRoot`
  schemas. They share one project format version bump (next after 0.9.0).
- The existing minimum-version check rejects older `.alcd` files. The plan adds no migration.
- The user re-imports test libraries after the cutover.

### D2 — The DNG profile is runtime-only data

- No table keeps the profile tables (HueSatMap, LookTable, ProfileToneCurve, and so on).
- The small `RawRuntimeColorContext` fields stay persisted: color and forward matrices,
  AsShotNeutral, illuminant CCTs, cam_mul, lens fields. Develop binding needs them before decode
  for as-shot CCT and tint. They are a few hundred bytes.
- Persisted data keeps only a **profile reference**: `dng_profile_fingerprint` (the existing FNV
  value) or an absent value.
- A process-wide `DngColorProfileCache` keeps parsed profiles:
  - The key is the source file identity (normalized path + size + mtime).
  - The cache is LRU with a limit of **100 entries**. An evicted entry is parsed again from
    the source file on the next load.
  - Entries with the same fingerprint share one `DngColorProfilePtr`. Many DNG files from one
    camera have the same profile (`demo.alcd` has 2 distinct profiles in 144 DNG files), so
    memory stays small.
- `PipelineMgmtService` binds the profile from the cache when it loads a document and before the
  document goes live. This covers editor, thumbnail, export, and snapshot executors.
- If the source file is missing, the render already fails, because the RAW data is also
  missing. This change adds no new failure mode.
- If the source file changes and the fingerprint does not match, the new profile wins. The
  service logs one warning. The result cache key changes because the fingerprint changes.

### D2a — Import accepts RAW files only

- Import accepts a file only when LibRaw (or the DNG fast path) opens it. The decision stays
  content-based: import does not check the file extension.
- The Exiv2 raster fallback no longer makes a file importable. JPEG, TIFF, PNG, HEIF, sidecars,
  and containers fail with `UNSUPPORTED_FORMAT` and the message "not a supported RAW file".
- This rule stays until a non-RAW render input exists. A later plan can add that input and
  relax this rule.

### D3 — Search reads typed columns, not JSON

- The Image mapper writes typed search columns on every Image insert and update. This is the only
  write path for Image rows, so the columns stay in sync with EXIF rating writes.
- Search, stats, and the thumbnail filter never call `json_extract` or cast `metadata`.
- The folded search text is computed once in C++ at write time. SQL does no separator folding.

### D4 — A C++ parser decides what each query token means

- `SearchQueryParser` turns the query into typed terms: date, file kind, capture parameter, and
  text.
- A date or kind term also keeps a text alternative, so `20260607` still matches a file name
  that contains those digits.
- The parser is pure C++ with table-driven unit tests. It needs no database.

### D5 — The UI thread never runs search SQL

- All search routes go through one worker with latest-wins coalescing. A stale pending request is
  dropped before it runs.
- One SQL statement returns the page rows and the total (`COUNT(*) OVER ()`).
- The result rows come from query columns. There is no image pool read for each row.

## Performance targets

| Measurement | Target |
| --- | --- |
| Preview SQL (page + total), 1k-image library | p95 ≤ 10 ms |
| Preview SQL, synthetic 20k-image library | p95 ≤ 50 ms |
| `ApplyFuzzySearch` (page + stats), 1k library | ≤ 50 ms total, off the UI thread |
| UI thread time for each keystroke in the search dialog | ≤ 2 ms (no SQL) |
| `demo.alcd` library after re-import | ≤ 25 MB |

## Phases

### Phase S0 — Baseline and recall fixtures

Goal: measure before any change, and lock the recall behavior in tests.

1. Add a search benchmark test target. It builds a synthetic library with N images
   (N = 1k and 20k), with realistic file names, EXIF dates, cameras, lenses, and a share of DNG
   rows with a large profile.
2. The benchmark runs the current `BuildFuzzySearchWhere` path and prints p50 and p95 for preview
   (count + page) and for apply (page + stats).
3. The benchmark can also open a real project when the environment variable
   `ALCEDO_SEARCH_BENCH_PROJECT` is set. The test skips when the variable is not set.
4. Add a recall fixture table (query → expected file set) for the synthetic library. Initial cases:
   - `6.7`, `6/7`, `0607`, `6月7日`, `2026.6`, `2026-06-07`, `20260607`, `June 7`
   - `jpg`, `dng`, `raw`, `rw2`
   - `iso800`, `f2.8`, `35mm`
   - file name stems, stem fragments, and stems with separators
5. Add a local real-file test on
   `alcedo_studio/tests/resources/sample_images/raw/camera/nikon`:
   - The folder is local-only (git-ignored, 1.3 GB). It has 38 RAW files (35 NEF, 3 DNG) in
     one subfolder for each camera body, and 7 non-RAW files (5 `.rrdata`, 1 `.pp3`, 1 `.xmp`).
   - The test is **disabled by default** (gtest `DISABLED_` prefix). Run it with
     `--gtest_also_run_disabled_tests`. It calls `GTEST_SKIP` if the folder is missing.
   - It imports the whole folder, then checks import results and search recall.
   - Import checks: 38 imported, 7 failed, `Image` rows = `FileImage` rows = 38.
   - File name recall cases (each must find the named file):
     - `nikon d3x 01`, `d3x_01`, `nikond3x01` → `nikon_d3x_01.nef`
     - `d810 raw 11`, `raw00011`, `00011` → `Nikon-D810-raw00011.nef`
     - `dsc 2230`, `DSC2230`, `2230` → `DSC_2230.NEF`
     - `0431 dng` → `DSC_0431_dng.dng`
     - `z8` → the four RAW files in the z8 folder (path tail and camera model)
     - `dng` → the 3 DNG files only
6. Mark the cases the current code fails as expected failures. Later phases turn them green.

Acceptance: the benchmark reproduces the 4–5 s order of magnitude on the DNG-heavy synthetic
library. The synthetic recall table runs in CI. The disabled Nikon test runs locally and its
result is recorded.

##### Phase S0 completion record (2026-09-24)

**Status:** complete — test-only change; no production code changed.

Branch: `refactor/library-search-s0-baseline` (base of the S1–S6 PR stack).

Files added:

- `alcedo_studio/tests/app/library_search_test_support.hpp` — `SyntheticLibraryBuilder`
  (batched Image + library file writes, 500 per batch, pinned until written),
  `MakeLargeDngColorProfile` (~400 KB JSON, like the 421 KB profile in `demo.alcd`),
  `CountTableRows`, `SearchFileNames`, `LibraryRootFolderId`.
- `alcedo_studio/tests/app/library_search_recall_test.cpp` — target `LibrarySearchRecallTest`
  (label `ci_core_flow`, registered `ci_core`).
- `alcedo_studio/tests/app/library_search_benchmark_test.cpp` — target
  `LibrarySearchBenchmarkTest` (all tests `DISABLED_`).

**Measured path (success call chain):**

```text
LibrarySearchBenchmarkTest / LibrarySearchRecallTest
  -> SleeveFilterService::CountSearchResults / SearchFolder / BuildFolderStats
  -> SleeveFilterService::BuildFuzzySearchWhere (TokenSearchClause, FoldedDocumentClause)
  -> ElementStore::CountFilesInFolder / ListFilesInFolderPage / BuildFolderStats
  -> DuckDB scan of Element ⋈ FileImage ⋈ Image with the compiled predicate
  -> matched file ids / names / stats buckets
```

**Import path in the Nikon test (failure call chain for non-RAW files):**

```text
ImportServiceImpl::ImportToFolder (all 45 regular files, no extension check)
  -> MetadataExtractor::ExtractEXIF_ToImage -> UNSUPPORTED_FORMAT for 7 non-RAW files
  -> ImportLog metadata_failed_ (7) -> ImportResult{requested 45, imported 38, failed 7}
  -> ImportServiceImpl::SyncImports: DeleteFileEverywhere(element) but no image pool removal
  -> ImagePoolService::SyncWithStorage writes 7 orphan Image rows (Image 45, FileImage 38)
```

**Recall table** (`FuzzySearchReturnsExpectedFilesForEachRecallCase`, 9 synthetic files,
28 cases). A known-defect case must still differ from its expected set; when a later phase
fixes it, the test fails until the case is changed to `kPasses`.

| State | Cases |
| --- | --- |
| Passes today (18) | `2026.6`, `2026-06-07`, `20260607`, `jpg`, `rw2`, `raw` (every file is RAW), `P2635860`, `2635860`, `P263 5860`, `p26358`, `d810 raw 11`, `raw00011`, `nikond3x01`, `d3x_01`, `nikon d3x 01`, `shangrila`, `G9M2`, `eos r5` |
| Known defect (10) | `6.7` and `6/7` → all 9 files; `0607` → also `DSC_0432.dng`; `6月7日` → none; `June 7` → none; `dng` → all 9 files; `iso800` → also the ISO 8000 DNG; `f2.8` → none; `35mm` → all 9 files; `5860` → also both DNG files (profile numbers in the metadata dump) |

**Local Nikon test** (`DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames`):

- Import: 45 requested, 38 imported, 7 failed. `FileImage` = 38. `Image` = 45 (P4 orphan
  rows reproduced; the test asserts 45 and tells Phase S1 to change it to 38).
- File name recall: all 10 named-file queries find their file (`nikon d3x 01`, `d3x_01`,
  `nikond3x01`, `d810 raw 11`, `raw00011`, `00011`, `dsc 2230`, `DSC2230`, `2230`, `0431 dng`).
- Precision (recorded as test properties, asserted as supersets until Phase S4):
  - `z8` → the 4 z8 files plus `DSC_0261.NEF` (Z6II), `DSC_1456.NEF` and `DSC_1535.NEF` (Zf).
  - `dng` → all 38 files.
- Deviation from step 5: the plan lists `dng → the 3 DNG files only` as an exact case. The test
  asserts the 3 DNG files are included and records the full set. Phase S4 makes it exact.

**Latency** (debug build, Windows, 3 runs each unless noted; preview = count + 50-row page,
apply = count + 120-row page + `BuildFolderStats`):

| Library | Query | Matches | Preview p50 | Apply p50 |
| --- | --- | --- | --- | --- |
| synthetic 1000 files, 155 DNG | `jpg` | 0 | 10.4 s | 35.2 s |
| | `2026-06-07` | 1 | 10.9 s | 39.5 s |
| | `P1000123` | 0 | 10.5 s | 36.3 s |
| | `6.7` | 1000 | 10.3 s | 36.4 s |
| | `dsc` | 493 | 0.7 s | 2.8 s |
| `demo.alcd` (927 files) | `jpg` | 0 | 9.5 s | 33.6 s |
| | `2026-06-07` | 414 | 10.1 s | 35.4 s |
| | `P1000123` | 0 | 9.7 s | 34.0 s |
| | `6.7` | 927 | 0.57 s | 2.5 s |
| | `dsc` | 0 | 9.6 s | 33.4 s |
| synthetic 20 000 files, 20 DNG per 1000 (1 run) | `jpg` | 0 | 34.4 s | 106.7 s |
| | `2026-06-07` | 22 | 33.8 s | 116.1 s |
| | `P1000123` | 0 | 33.8 s | 112.9 s |
| | `6.7` | 20000 | 34.6 s | 116.6 s |
| | `dsc` | 8240 | 26.3 s | 87.4 s |

The 20 000-file library took 404 s to build through `SyntheticLibraryBuilder` (the 1000-file
library took 4.5 s). Library build time grows faster than linear; S3 measurements can reuse the
same builder but should expect this setup cost.

A query that matches most rows early in the OR chain (`dsc` on synthetic, `6.7` on
`demo.alcd`) is fast because DuckDB stops at the first true clause. A miss evaluates every
clause, including the 33-level `REPLACE` over `CAST(metadata AS VARCHAR)`, on every row.

**What was proven (executed tests):**

| Test | Binary | Result |
| --- | --- | --- |
| `FuzzySearchReturnsExpectedFilesForEachRecallCase` | `LibrarySearchRecallTest` | PASS (3.5 s) |
| `DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames` | `LibrarySearchRecallTest` | PASS (10.2 s, local) |
| `DISABLED_ReportsSearchLatencyForOneThousandFileLibrary` | `LibrarySearchBenchmarkTest` | PASS (628 s) |
| `DISABLED_ReportsSearchLatencyForPackedProject` (`demo.alcd`) | `LibrarySearchBenchmarkTest` | PASS (577 s) |
| `DISABLED_ReportsSearchLatencyForTwentyThousandFileLibrary` (`ALCEDO_SEARCH_BENCH_REPEAT=1`) | `LibrarySearchBenchmarkTest` | PASS (1279 s) |
| `ctest -R LibrarySearchRecallTest` | ctest | 1 passed, Nikon test reported as Disabled |

Each benchmark test also asserts that `CountSearchResults`, the unpaged `SearchFolder` size, and
`BuildFolderStats.total_photo_count_` agree for each query.

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target LibrarySearchRecallTest LibrarySearchBenchmarkTest
LibrarySearchRecallTest.exe
LibrarySearchRecallTest.exe --gtest_also_run_disabled_tests --gtest_filter=*Nikon*
LibrarySearchBenchmarkTest.exe --gtest_also_run_disabled_tests --gtest_filter=*OneThousand*
ALCEDO_SEARCH_BENCH_PROJECT=<demo.alcd> LibrarySearchBenchmarkTest.exe --gtest_also_run_disabled_tests --gtest_filter=*PackedProject*
```

Full `ctest` suite: not run (agent rule). No production code changed.

**Checklist / exit condition:** steps 1–6 done. The benchmark reproduces the slow search
(≥ 9 s preview, ≥ 33 s apply on a miss). The recall table runs in the `ci_core` set. The Nikon
test ran locally and its result is recorded above.

**LOC note:** support header 206, recall test 293, benchmark test 238 lines. No file is near the
1000-line limit.

**Remaining gaps:** none for S0. Phases S1 and S4 must update the Nikon `Image` row assertion and
tighten the `z8` / `dng` supersets to exact sets.

### Phase S1 — Import accepts RAW only and leaves no orphan rows

Goal: only RAW files import. A failed import writes no `Image` row. A large import loses no good
row.

1. Apply [Decision D2a](#d2a--import-accepts-raw-files-only) in `ExtractEXIF_ToImage`: when
   `ExtractRawMetadata_ToImage` returns false, throw `UNSUPPORTED_FORMAT`. Delete the Exiv2
   raster fallback branch and `IsImportableExiv2Raster` if nothing else uses them. Keep Exiv2
   reads that add metadata to a RAW file.
2. In `SyncImports`, collect `entry.image_id_` for each `metadata_failed_` entry. Call
   `image_pool_service_->RemoveBatch(ids)` before `SyncWithStorage()`.
3. Exclude failed entries from the semantic generation queue in `FinishImport`.
4. Keep each imported Image pinned until `SyncImports` has written it, or flush the pool in
   batches during import. Choose the option after a test with more than 1024 files. The test
   asserts that each `FileImage.image_id` has an `Image` row.
5. On project load, run one best-effort delete of `Image` rows with no `FileImage` reference.
   This is a safety net only; step 2 is the fix.
6. Tests:
   - A mixed folder (RAW + JPEG + TIFF + `.xmp` + `.mov` + unknown binary) imports only the RAW
     files and leaves `Image` row count = `FileImage` row count.
   - A JPEG renamed to `.nef` fails. A RAW file renamed to `.bin` imports (content rule).
   - A 1100-file import leaves no `FileImage` row without an `Image` row.
   - The disabled Nikon test from Phase S0 gives 38 imported and 7 failed.

Acceptance: re-import of the `demo.alcd` source folders gives `Image` rows = `FileImage` rows,
and no non-RAW file is in the library.

##### Phase S1 completion record (2026-09-24)

**Status:** complete — import accepts RAW content only, a failed import writes no row, and the
image pool keeps each unwritten Image until it is synced. The `demo.alcd` re-import in the
acceptance line is a manual check and was not run (it belongs to the Phase S6 qualification).

Branch: `feature/library-search-s1-raw-only-import` (on top of
`refactor/library-search-s0-baseline`).

Commits: `style(image-pool)` (CRLF → LF for `image_pool_manager.{hpp,cpp}`, no content change),
then one S1 commit.

**What changed:**

| Step | Change |
| --- | --- |
| 1 | `MetadataExtractor::ExtractEXIF_ToImage` throws `UNSUPPORTED_FORMAT` ("not a supported RAW file") when `ExtractRawMetadata_ToImage` fails. The Exiv2 raster fallback and `IsImportableExiv2Raster` are deleted. Extra defect found and fixed: `ExtractDngMetadataToImageFast` accepted any `.dng`-named file with EXIF even when LibRaw could not open it (a JPEG renamed to `.dng` imported); it now returns false unless LibRaw `open_file` succeeds. Exiv2 still adds DNG metadata inside the RAW path. |
| 2 | `ImportServiceImpl::SyncImports` collects `image_id_` of every `metadata_failed_` entry and calls `ImagePoolService::RemoveBatch` before `SyncWithStorage`. |
| 3 | `ImportExportHandler::FinishImport` queues semantic generation only for `created_` entries with `metadata_ok_`. This also excludes the unsupported Nikon HE entries, so the separate id set is gone. |
| 4 | Option chosen: a pool rule, not per-import pins. `ImagePoolManager::IsEvictable` refuses to evict an Image whose sync state is not `SYNCED` (new, modified, or deleted and not yet written); the pool grows past 1024 until `SyncWithStorage` runs. Reason: the 1100-file test showed that the loss needs one more pool insert after the metadata tasks release their pins (for example the library grid reading a stored Image). Pinning only the import handles would not protect the other unwritten writes (`Write_NoSync` star ratings, `PersistImageHdrFlag`), which had the same eviction loss. Each imported Image is also held by its `SleeveFile`, so the rule adds no memory during import. |
| 5 | `ImageStore::RemoveImagesWithoutFileBinding` (`DELETE … WHERE NOT EXISTS (SELECT 1 FROM FileImage …)`) runs once in `ProjectService::LoadProject`. A failure is logged and the load continues. |

**Primary success call chain (RAW file):**

```text
ImportExportHandler (album backend) -> ImportServiceImpl::ImportToFolder
  -> ImagePoolService::CreateAndReturnPinnedEmpty (UNSYNCED placeholder, pinned)
  -> worker: MetadataExtractor::ExtractEXIF_ToImage -> ExtractRawMetadata_ToImage
     (LibRaw open + unpack, or the DNG fast path with a LibRaw open)
  -> ImportLog::MarkMetadataSuccess; pin released
  -> other pool inserts before the sync: EnsureCapacityForInsert skips UNSYNCED entries
  -> ImportExportHandler::FinishImport -> ImportServiceImpl::SyncImports
  -> ImagePoolService::SyncWithStorage (Image row) -> SleeveServiceImpl::Sync (Element, FileImage)
  -> semantic generation queued for metadata_ok_ entries only
```

**Primary failure call chain (non-RAW file):**

```text
ExtractEXIF_ToImage: ExtractRawMetadata_ToImage == false
  -> MetadataExtractionError(UNSUPPORTED_FORMAT, "not a supported RAW file")
  -> ImportLog::MarkMetadataFailure -> ImportResult.failed_ += 1
  -> SyncImports: FileSystem::DeleteFileEverywhere(element) + ImagePoolService::RemoveBatch(image)
  -> SyncWithStorage erases the DELETED placeholder without writing it
  -> no Element, FileImage, or Image row
  -> FinishImport: metadata_ok_ == false -> not queued for semantic generation
Rows from older imports: ProjectService::LoadProject -> ImageStore::RemoveImagesWithoutFileBinding
```

**What was proven (executed tests):**

| Plan criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Mixed folder (RAW + JPEG + TIFF + `.xmp` + `.mov` + unknown binary) imports only RAW; `Image` = `FileImage`; only the RAW entry has `metadata_ok_` | `MixedFolderImportsOnlyRawFilesAndLeavesNoOrphanImageRows` | `ImportRawOnlyTest` | PASS |
| JPEG renamed to `.nef` fails, RAW renamed to `.bin` imports (also: JPEG renamed to `.dng` fails) | `ImportDecidesRawByContentNotByFileExtension` | `ImportRawOnlyTest` | PASS |
| 1100-file import leaves no `FileImage` row without an `Image` row | `ImportLargerThanImagePoolCapacityWritesAnImageRowForEveryFile` | `ImportRawOnlyTest` | PASS (40 s). Before the pool fix: FAIL, 77 `FileImage` rows without an `Image` |
| Pool keeps unwritten Images past its capacity until the sync | `ImagePoolKeepsUnwrittenImagesPastCapacityUntilSync` | `ImportRawOnlyTest` | PASS. Before the fix: FAIL, 1024 of 1100 written |
| Load-time removal of Image rows without a library file | `ProjectLoadRemovesImageRowsWithoutLibraryFile` | `ImportRawOnlyTest` | PASS |
| Extractor rejects JPEG and TIFF, also when renamed to `.nef` or `.dng` | `NonRawRastersAreRejectedAsUnsupportedRawWhateverTheExtension` | `MetadataExtractorTest` | PASS |
| Nikon S0 test: 38 imported, 7 failed, `Image` = 38 (assertion changed from 45) | `DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames` | `LibrarySearchRecallTest` | PASS (local, 10.2 s) |
| Regression | `MetadataExtractorTest` (12), `ProjectServiceTest` (8), `BatchImportDngMetadataTest` (1), `LibrarySearchRecallTest` (1 + Nikon) | direct run and ctest | PASS |
| Regression | `ImportServiceTest` (no ctest registration; needs local files) | direct run | 12/13. `BatchCancelTest` fails the same way on clean HEAD (`a23c9165a`, stash and rebuild): `ImportToFolder` submits every task before it returns, and the test cancels 100 ms after the return, so nothing is cancelled |

The RAW cases use the CI fixture `ci_rawfiles/…DSC00830.ARW` and skip when it is missing. The
1100-file case uses hard links. NTFS allows 1023 links per file, so the links alternate between
two copies of the fixture.

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target ImportRawOnlyTest MetadataExtractorTest LibrarySearchRecallTest ImportServiceTest ProjectServiceTest BatchImportDngMetadataTest AlbumBackendLib
ctest --test-dir build/debug -R "ImportRawOnlyTest|MetadataExtractorTest|LibrarySearchRecallTest|ProjectServiceTest" -j 1
  -> 26/26 passed (Nikon test listed as Disabled)
LibrarySearchRecallTest.exe --gtest_also_run_disabled_tests -> 2/2 passed
ImportServiceTest.exe -> 12/13 (BatchCancelTest fails on clean HEAD too)
```

Full `ctest` suite: not run (agent rule). `alcedo_main` was not linked; `AlbumBackendLib`, which
compiles `import_export.cpp`, builds.

**Checklist / exit condition:** steps 1–6 done. The acceptance line (`demo.alcd` re-import) was
not run; it needs the user's source folders and is part of Phase S6.

**LOC note:** new `import_raw_only_test.cpp` 319 lines, `tests/support/non_raw_import_files.hpp`
95 lines. `metadata_extractor.cpp` is 1662 lines (1705 before; it was over the 1000-line limit
before this phase). S1 made no split.

**Remaining gaps:**

- `FinishImport` (step 3) has no album-backend test. The import test covers the `metadata_ok_`
  flag that it filters on.
- `LibraryModule::PersistImageHdrFlag` writes with `Write_NoSync` and does not sync. Under the
  new eviction rule such an Image stays in the pool until the next `SyncWithStorage`, where
  before it could be evicted and its write lost.
- `ImportServiceTests.BatchCancelTest` is a failure that exists on clean HEAD (see above).

### Phase S2 — DNG color profile is runtime-only

Goal: no table holds DNG profile tables. DNG renders stay pixel-identical.

1. Add `DngColorProfileCache` (process-wide, file identity key, LRU limit of 100 entries,
   profiles shared by fingerprint). Its loader uses `ReadRawColorContextForRender` or a smaller
   Exiv2 tag read of the same data. Add unit tests for eviction at entry 101, shared pointers
   for equal fingerprints, and a reload after the source file mtime changes.
2. `RawColorContextToJson` writes the small fields and `dng_profile_fingerprint`. It does not
   write the profile tables.
3. `DevelopParamsModel::ToJson` writes the fingerprint reference, not the tables.
   `LoadJson` leaves the profile pointer empty until binding.
4. `PipelineMgmtService` binds the cached profile in `LoadPipeline`, `BindRootCameraProfile`,
   and each other document-load entry point before the document goes live. List each entry
   point in the completion record.
5. `ComputeRootId` hashes the document and the small raw context only. The fingerprint is the
   profile input to the hash.
6. `DngColorProfilesEqual` compares fingerprints first and never compares full JSON dumps.
7. The EXIF details panel (`image_controller.cpp:332`) stops building the full profile JSON.
8. Replace the storage section in `docs/dng_color_profiles.md`.
9. Update the affected tests and expected serialized output files:
   - `tests/edit/runtime/dng_color_profile_test.cpp`
   - `tests/image/metadata_extractor_test.cpp`
   - `tests/app/import_pipeline_document_test.cpp`
   - `tests/app/pipeline_service_test.cpp`
   - `tests/edit/history/pipeline_document_checkpoint_test.cpp` and its
     `expected_serialized/*.json` expected serialized output files
   - `tests/resources/expected_json/imported_camera_profile_*_expected_develop.json`
   - `tests/app/thumbnail_service_test.cpp`, `tests/edit/pipeline/pipeline_document_render_test.cpp`,
     `tests/edit/runtime/dng_profile_test_support.hpp`, `develop_camera_profile_import_test.cpp`,
     `tests/edit/graph/imported_camera_profile_fixtures.hpp`
10. Add a render test: a DNG with HueSatMap and LookTable renders the same pixels before and
    after the change on each available GPU backend.

Acceptance:

- No persisted JSON column contains `HueSatMap`, `LookTable`, or profile table arrays.
- The DNG render test is pixel-identical.
- `demo.alcd` after re-import is ≤ 25 MB.

### Phase S3 — Typed search columns

Goal: search and stats read plain columns.

1. Add columns to `Image`, written by the Image mapper from the `Image` object:
   - `file_stem`, `file_ext` (lowercase, no dot)
   - `capture_at TIMESTAMP`, `capture_date DATE`
   - `camera_make`, `camera_model`, `lens`
   - `iso INTEGER`, `focal_mm DOUBLE`, `aperture DOUBLE`, `rating INTEGER`
   - `search_text VARCHAR`: lowercase, separator-folded concatenation of file name, stem, path
     tail, make, model, lens, and the formatted capture date
2. Add `search_text` to `AiImageUnderstanding`, split as caption text and tags text so the field
   mask still works. The AI store writes it on upsert.
3. Replace the correlated `string_agg` subqueries with one `LEFT JOIN` to the active AI row.
4. Replace `SemanticLabelExpr` (a large `CASE` for each row) with query-side alias expansion
   and `EXISTS (... label IN (...))`.
5. Replace each `json_extract` in `ElementStore::BuildFolderStats` with the typed columns.
6. Make sure the EXIF star-rating write path (`ApplyStarRatingLight`) updates `rating`.

Acceptance: no search or stats SQL contains `json_extract`, `CAST(i.metadata`, or `REPLACE(`.
The Phase S0 benchmark meets the SQL targets.

### Phase S4 — Query parser and new WHERE builder

Goal: correct recall and precision.

1. Add `SearchQueryParser` (`app/`). Term kinds:
   - **Date**: `Y-M-D` in any separator, `M.D` / `M/D` / `M-D` (any year), `M月D日`, `M月`,
     `YYYY年M月`, `YYYY.M`, `YYYYMMDD`, `YYYY`, English month names. Month-day terms match
     `month(capture_date)` and `day(capture_date)` in any year.
   - **Kind**: `jpg`/`jpeg`, `dng`, `raw` (all RAW extensions), each explicit extension.
   - **Capture parameter**: `iso800` / `iso 800`, `f2.8` / `f/2.8`, `35mm`.
   - **Text**: any other token.
2. Each date, kind, or parameter term compiles to `(typed predicate OR text contains)`.
3. A text term compiles to `contains(search_text, folded_token)`, plus the AI text columns when
   their field bits are on.
4. Terms combine with AND. Delete the whole-query fallback clause and `FoldedDocumentClause`.
5. The field mask still controls which columns a term can match.
6. There is no typo tolerance. A text term matches when its folded form is a substring of the
   folded search text.

Acceptance: every Phase S0 recall case passes, including the disabled Nikon cases. `6.7`
matches only June 7 captures and stems that contain `67`.

### Phase S5 — Search runs off the UI thread

Goal: typing never blocks the UI.

1. Replace the synchronous `SearchPreview` call in `GlobalSearchDialog.qml` with
   `RequestSearch`. Delete the synchronous `Q_INVOKABLE` after all callers move.
2. Add one search worker (single thread) with latest-wins coalescing. Each request carries a
   generation value. The worker drops a request whose generation is older than the newest one.
3. `ListFilesInFolderPage` for search returns `COUNT(*) OVER ()` and the display columns
   (`file_name`, `camera_model`, `lens`, `capture_date`, `rating`).
4. `BuildResultRows` reads those columns and does no image pool read.
5. Measure `ApplyFuzzySearch` (thumbnail page + stats) after S3 and S4. If it is above 16 ms on
   the 1k library, move it to the worker and apply the result on the UI thread.
6. Keep the 140 ms preview debounce.
7. Add a test hook that fails if search SQL runs on the UI thread.

Acceptance: the Phase S0 UI-thread target is met. A scripted QML test types 20 characters
quickly and the dialog applies only the last response.

### Phase S6 — Qualification

1. Re-import the `demo.alcd` source folders. Record the file size, row counts, and benchmark
   numbers in the completion record.
2. Run the manual search checklist: date forms, kinds, parameters, file names, AI fields on and
   off, the natural-language route, stats panel counts, and thumbnail grid scroll.
3. Run the full ctest suite and list any failure that is also present on a clean `main`.

## Resolved questions (2026-09-24)

1. **File name recall fixture**: use the local Nikon folder, disabled by default (Phase S0 step 5).
2. **Typo tolerance**: not needed (Phase S4 step 6).
3. **JPG import**: block non-RAW import until a non-RAW render input exists
   ([Decision D2a](#d2a--import-accepts-raw-files-only), Phase S1).
4. **Profile cache size**: LRU, 100 entries ([Decision D2](#d2--the-dng-profile-is-runtime-only-data)).

## Out of scope

- A non-RAW (JPEG, TIFF, HEIF) render input.
- The GPU re-pack and re-upload of DNG tables on each render (`dng_profile_gpu_data.hpp`).
- The duplicate write of the pipeline document to `PipelineParam` (`sleeve_service.cpp:71`)
  beyond the profile removal.
- Natural-language (CLIP) search ranking.

## Completion record template

```markdown
##### Phase SN completion record (YYYY-MM-DD)

- Status:
- Commits:
- What changed:
- Primary call chains:
- Tests added / updated:
- Measurements (before → after):
- Deferred checks:
```
