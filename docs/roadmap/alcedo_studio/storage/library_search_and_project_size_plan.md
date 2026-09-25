# Library Search Performance, Recall, and Project Size Plan

Date: 2026-09-24

Status: Phases S0, S1, S2, and S3 complete (2026-09-24); S4 and S5 complete (2026-09-25); S6 not started

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
- [DNG color profiles](../../../dng_color_profiles.md) (design note; Phase S2 replaced its storage section)

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

##### Phase S2 completion record (2026-09-24)

**Status:** complete — project tables store a DNG profile fingerprint only; the pipeline
service binds the profile tables from the source file before a loaded document goes live.
The `demo.alcd` size check (≤ 25 MB after re-import) was not run: it needs the user's source
folders and belongs to the Phase S6 qualification.

Branch: `feature/library-search-s2-runtime-dng-profile` (on top of
`feature/library-search-s1-raw-only-import`, PR 194).

Commits: `style(image)` and `style(tests)` (CRLF → LF for `image.cpp` and the three
`imported_camera_profile_*_expected_develop.json` files, no content change). The S2 content
change is not committed yet.

**What changed:**

| Step | Change |
| --- | --- |
| 1 | New `DngColorProfileCache` (`image/dng_color_profile_cache.{hpp,cpp}`): key = normalized path + file size + last write time, LRU at 100 files, profiles shared by fingerprint through a weak-pointer map, loader runs outside the lock. `DngColorProfileCache::Shared()` uses the new `MetadataExtractor::ReadDngColorProfileFromSource`, which reads the same IFD0 tags as import (`PopulateDngColorMetadataFromExif`), so an unchanged file gives the import fingerprint. It replaces `ReadRawColorContextForRender`, which had no product caller. |
| 2 | New `DngColorProfileRef` (`dng_color_profile.hpp`): empty, bound (fingerprint + profile), or unbound (fingerprint only). `RawRuntimeColorContext::dng_profile_` is a `DngColorProfileRef`. `RawColorContextToJson` writes `DngProfileFingerprint` (16 hex digits or null) and no tables; `RawColorContextFromJson` reads an unbound reference. |
| 3 | `DevelopCameraProfile::dng_profile` is a `DngColorProfileRef`. `DevelopParamsModel::ToJson` writes `camera_profile.dng_profile_fingerprint`. `LoadJson` keeps a bound profile when the fingerprint is the same and leaves any other fingerprint unbound. New `DngProfile()` and `BindDngColorProfile()`. `ReplaceParams` and `BindImportedCameraProfile` also install a bound profile when the payloads compare equal (equality compares fingerprints; the first run of `EditorLoadCheckpointAndRebuildBindSourceProfile` found that the checkpoint path skipped the bind). `ClonePipelineDocument` keeps the source's bound profile (the paste, history-detail, and adjustment-transfer paths clone `root_document_`). |
| 4 | `app/source_dng_profile_binding.{hpp,cpp}` (in `PipelineMgmtService`): `SourceImagePath` (Element → FileImage → Image row), `LoadSourceDngColorProfile` (cache load; another fingerprint in the source file wins with one `std::cerr` warning for each bind), `BindSourceDngColorProfile` for a document and for a RAW context. Entry points in `pipeline_service.cpp`: `LoadPipeline` (thumbnail, export, analysis, and the base of every editor load), `InitializeImageRoot` for an existing root (after the connection lock is released), `LoadEditorPipeline` (root state; the checkpoint and replay documents then bind from the bound root context), `CheckoutVersion`, and `RebuildActiveEditorPipeline` (bind failure is returned in `error` and leaves the prior document bound). A new root (import) binds from the in-memory import context, which is already bound. |
| 5 | `ComputeRootId` is unchanged in code: it hashes the document and RAW context dumps, which now carry the fingerprint text and no tables. `PipelineDocumentCheckpointTest` expected root id and chain hash changed because of this (document JSON is otherwise identical). |
| 6 | `DngColorProfilesEqual` compares fingerprints only. `DngColorProfileFromJson` and the `DngHueSatMap` `from_json` are deleted (no persisted table data is read any more). |
| 7 | New `Image::ExifDisplayToJson()`; `image_controller.cpp` `ParseExifDisplayJson` uses it and no longer serializes the RAW context. |
| 8 | `docs/dng_color_profiles.md`: storage section replaced (fingerprint columns, cache, bind entry points, failure rules). |
| 9 | Tests and expected files updated (list below). |
| 10 | `VerifyCanonDngProfile` (CUDA/OpenCL/Metal) now also reads the document back from JSON, binds the profile from the source file through a `DngColorProfileCache`, renders on a new device, and requires the same pixels (NORM_INF < 1e-6). The helper had only a Metal caller; CUDA and OpenCL now call it too. |
| D1 | Project format 0.9.0 → 0.10.0 (`kProjectFileVersion`, min and max). 0.9.0 projects are rejected. No migration. S3 lands in the same format version. |

Rendering fails closed: an unbound reference gives `ColorTransformError::UnboundDngProfile`
from `ResolveDevelopColorTransform`, and `PackDngProfileGpuData` throws. It never renders as
"no profile".

Renamed in touched files to follow the `AGENTS.md` terminology rules: the label-query setup
method is now `Database::PopulateSemanticLabelQueries`, the checkpoint test camera value is now
`RootStateCamera`, and one comment in `tests/app/CMakeLists.txt` was reworded.

**Primary success call chain (load after reopen):**

```text
ThumbnailService / ExportService / editor -> PipelineMgmtService::LoadPipeline(element)
  -> LoadPipelineDocument: PipelineParam JSON -> Develop dng_profile = unbound ref (fingerprint)
  -> BindSourceDngColorProfile(storage, element, document)
     -> SourceImagePath: ElementStore::GetElementById -> SleeveFile.image_id_
        -> ImageStore::GetImageById -> image_path_
     -> DngColorProfileCache::Shared().Load(path)   [hit: path + size + mtime match]
        miss -> MetadataExtractor::ReadDngColorProfileFromSource -> ReadDngColorProfile (Exiv2)
        -> shared by fingerprint
     -> DevelopParamsModel::BindDngColorProfile(profile)
  -> PipelineExecutor::SetPipelineDocument (document goes live)
  -> render: ResolveDevelopColorTransform + PackDngProfileGpuData use the bound tables
Editor: LoadEditorPipeline -> DecodePipelineRootState -> BindSourceDngProfiles(root ctx + root doc)
  -> checkpoint doc: BindRootCameraProfile(doc, bound ctx) / replay: clone of bound root
```

**Primary failure call chains:**

```text
Source file missing -> DngColorProfileCache::Load throws "source file is unavailable"
  -> LoadPipeline records load_error_ and rethrows -> thumbnail/export report the error
  (CheckoutVersion / RebuildActiveEditorPipeline return false with the error; the prior
  document stays bound)
Source file has another profile -> the source profile binds, one warning per bind,
  fingerprint and result cache key change
Document read from JSON and never bound -> ResolveDevelopColorTransform: UnboundDngProfile,
  PackDngProfileGpuData throws; BindDevelopCameraProfile rejects an unbound RAW context
```

**What was proven (executed tests):**

| Plan criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Step 1: eviction at entry 101 | `EvictsLeastRecentlyUsedFileWhenTheHundredAndFirstFileLoads` | `DngColorProfileCacheTest` | PASS |
| Step 1: shared pointer for equal fingerprints | `FilesWithEqualProfileContentShareOneProfile` | `DngColorProfileCacheTest` | PASS |
| Step 1: reload after the source file changes (content + mtime, and mtime only) | `ChangedSourceFileIsReadAgainAndGivesTheNewProfile` | `DngColorProfileCacheTest` | PASS |
| Step 1: missing file / failed read leave no entry; file without profile | `MissingFileOrFailedReadThrowsAndLeavesNoEntry`, `FileWithoutDngProfileIsCachedAsNoProfile` | `DngColorProfileCacheTest` | PASS |
| Step 1: runtime read gives the import fingerprint (CI DNG) | `RealDngLoadsTheProfileFingerprintBoundAtImport` | `DngColorProfileCacheTest` | PASS |
| Steps 2, 6: fingerprint text, unbound reference, equality by fingerprint | `FingerprintIdentifiesContentAndIsPersistedAsSixteenHexDigits` | `DngColorProfileTest` | PASS |
| Unbound reference fails closed | `UnboundReferenceFailsColorTransformAndGpuPackingInsteadOfDroppingProfile` | `DngColorProfileTest` | PASS |
| Step 3: Develop JSON fingerprint, LoadJson keeps a matching bound profile | `DevelopJsonStoresFingerprintAndLoadKeepsOnlyAMatchingBoundProfile` | `DngColorProfileTest` | PASS |
| Clone keeps the bound profile | `ClonedDocumentSharesTheSourceBoundProfile` | `DngColorProfileTest` | PASS |
| Equal payload still binds (checkpoint path regression) | `ImportedContextBindsProfileOnDocumentReadFromJson` | `DngColorProfileTest` | PASS |
| Step 2: Image metadata holds only the fingerprint; source read gives the same profile (Sony DNG, local) | `ProjectMetadataStoresFingerprintOnlyAndSourceReadGivesSameProfile` | `DngColorProfileTest` | PASS |
| Acceptance: no persisted JSON column holds `HueSatMap`/`LookTable`/table arrays (`Image.metadata`, `PipelineParam`, `PipelineRoot`) | `ImportedDngStoresProfileFingerprintAndNoProfileTables` | `PipelineDngProfileBindingTest` | PASS — stored bytes for the CI DNG: `Image.metadata` 1,404, `PipelineParam` 5,486, `PipelineRoot` 6,693 |
| Step 4: `LoadPipeline` binds before the document goes live | `LoadPipelineBindsSourceProfileBeforeDocumentGoesLive` | `PipelineDngProfileBindingTest` | PASS |
| Step 4: `LoadEditorPipeline` (root, `root_document_`), `RebuildActiveEditorPipeline`, checkpoint reopen; `ImageEditState` checkpoint holds no tables | `EditorLoadCheckpointAndRebuildBindSourceProfile` | `PipelineDngProfileBindingTest` | PASS (FAILED on the first run: found the equal-payload bind bug) |
| D2: missing source file fails the load | `MissingSourceFileFailsPipelineLoad` | `PipelineDngProfileBindingTest` | PASS |
| D2: another profile in the source file wins | `SourceFileWithAnotherProfileWinsOnLoad` | `PipelineDngProfileBindingTest` | PASS |
| Step 5: root id / checkpoint expected output | `CheckpointExpectedSerializedCarriesRootHeadChainAndDocument` and the other expected-serialized cases | `PipelineDocumentCheckpointTest` | PASS (root id and chain hash updated; document JSON unchanged apart from the key) |
| Step 9: expected Develop JSON (`imported_camera_profile_*`) | `BindImportedCameraProfileMatchesPreviousDevelopParameters` | `GpuDagModelGraphTest` | PASS (DNG fingerprint `fe283656e9b1715b`) |
| D1: format 0.10.0, 0.9.0 rejected | `CurrentProjectFileVersionIsSupported`, `ProjectVersion080FailsBeforeHistoryLoad`, `NewProjectWritesCurrentVersionAndHasNoEditHistoryTable` | `CommitGraphTest`, `ProjectServiceTest` | PASS |
| S0 recall table | `FuzzySearchReturnsExpectedFilesForEachRecallCase` | `LibrarySearchRecallTest` | PASS — `0607` and `5860` are now correct (profile numbers left the metadata) and were changed to `kPasses` |
| Step 10: a DNG with HueSatMap and LookTable (Canon R6 III) renders the same pixels after JSON reload + source bind, on a new device (NORM_INF < 1e-6) | `CanonDngProfileRendersAtFullResolutionAndInvalidatesOnlyColorCache` | `GpuDagCudaDevelopTest`, `GpuDagOpenClDevelopTest` | PASS on CUDA (40.7 s) and OpenCL (6.2 s). Metal: not available on this Windows machine. The first run FAILED on both backends at the helper's older scalar-versus-GPU check (after the new reload check had passed): the scalar reference omitted `AcesReferenceGamutCompress`, which `fd048bbd7` added to all three camera passes; the helper had only a Metal caller, so this never ran on Windows. The reference now applies it. |
| Import binds and reloads the profile; stored Develop JSON holds the fingerprint and no tables; first background render | `ImportCreatesRenderableDocumentWithoutStageMirror`, `ImportBindsCameraProfileOnDocumentOnly` | `ImportPipelineDocumentTest` | PASS |
| GPU regression | all cases | `PipelineDocumentRenderTest`, `ImportPipelineDocumentTest`, `CiRawWorkflowTest`, `GpuDagCudaDevelopTest`, `GpuDagOpenClDevelopTest`, `GpuDagCudaDrtProductTest` | 135/138 on the first run (the 2 Canon cases above; 1 skipped: the 100-megapixel OpenCL fixture is missing); the 2 Canon cases re-run after the reference fix: 2/2 |
| S0/S1 local Nikon import and file name recall | `DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames` | `LibrarySearchRecallTest` | PASS (local, 9.2 s) |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DngColorProfileCacheTest DngColorProfileTest MetadataExtractorTest BatchImportDngMetadataTest PipelineDngProfileBindingTest PipelineMapperTest PipelineDocumentCheckpointTest ImportPipelineDocumentTest CommitGraphTest ProjectServiceTest SleeveServiceTest LibrarySearchRecallTest ImportRawOnlyTest GpuDagModelGraphTest GpuDagRawInputTest EditorMiniGitMaterializerTest PipelineEditBatchTest CiRawWorkflowTest GpuDagCudaDevelopTest GpuDagOpenClDevelopTest GpuDagCudaDrtProductTest PipelineDocumentRenderTest AlbumBackendLib
ctest --test-dir build/debug -R "DngColorProfileCacheTest|DngColorProfileTest|MetadataExtractorTest|BatchImportDngMetadataTest|PipelineDngProfileBindingTest|PipelineMapperTest|PipelineDocumentCheckpointTest|CommitGraphTest|ProjectServiceTest|SleeveServiceTest|LibrarySearchRecallTest|ImportRawOnlyTest|GpuDagModelGraphTest|GpuDagRawInputTest|EditorMiniGitMaterializerTest|PipelineEditBatchTest" -j 1
  -> 403/403 passed (3 disabled by design: the local Nikon test, PipelineMapper Fuzz/ThreadSafe)
ctest --test-dir build/debug -R "GpuDagCudaDevelopTest|GpuDagOpenClDevelopTest|GpuDagCudaDrtProductTest|PipelineDocumentRenderTest|ImportPipelineDocumentTest|CiRawWorkflowTest" -j 1
  -> 135/138 (2 Canon reference failures, 1 skip); after the reference fix:
ctest --test-dir build/debug -R "CanonDngProfileRendersAtFullResolution" -j 1 -> 2/2 passed
LibrarySearchRecallTest.exe --gtest_also_run_disabled_tests --gtest_filter=*Nikon* -> 1/1 passed
```

Full `ctest` suite: not run (agent rule). `alcedo_main` was not linked; `AlbumBackendLib`
(which compiles `image_controller.cpp`) builds. `BrushSourceFormatBoundaryTest` exists only
with `ALCEDO_ENABLE_BRUSH_MASK` (off here); its version literals were updated but not built.

**Checklist / exit condition:** steps 1–10 done. Acceptance: no persisted JSON column holds
profile tables (proven on the CI DNG import); the DNG render check is pixel-identical within 1e-6 on CUDA and OpenCL; the
`demo.alcd` ≤ 25 MB check was not run (Phase S6, needs the source folders).

**LOC note:** `pipeline_service.cpp` 1092 → 1122 (it was over the 1000-line limit before this
phase; the source-profile lookup went to the new `source_dng_profile_binding.cpp`, 71 lines,
instead of into it). New files: `dng_color_profile_cache.{hpp,cpp}` 93 + 123,
`source_dng_profile_binding.hpp` 55, `pipeline_dng_profile_binding_test.cpp` 250,
`dng_color_profile_cache_test.cpp` 163. `metadata_extractor.cpp` 1662 → 1661 (over the limit
before S2; not split).

**Remaining gaps:**

- `demo.alcd` re-import size (≤ 25 MB) is not measured; it needs the user's source folders
  (Phase S6). The per-row sizes above are for one CI DNG.
- The source-changed warning is written for each bind while the stored fingerprint differs; the
  stored fingerprint changes only when the document is saved dirty. It is not rate-limited.
- `SourceImagePath` reads the Element and Image rows (the Image row parse includes the metadata
  JSON) on each bind of a referenced, unbound document. This is one lookup per pipeline cache
  miss of a DNG; the plan's performance targets are for search and do not cover it.
- `ThumbnailServiceTest` and `PipelineSharedUseTest` were not built or run. They bind camera
  profiles from images that the test extracts in memory (bound references), so the S2 change
  does not change what they bind. `ThumbnailServiceTest` has no ctest registration.
- Two pre-existing test target names in `tests/app/CMakeLists.txt` (the semantic gRPC
  dependency test and the live image analysis test) use a term that `AGENTS.md` prohibits. They
  were not renamed; renaming them changes test file names outside this phase.
- The S0 synthetic benchmark library still attaches a large profile to DNG rows, but the rows
  now persist only the fingerprint, so S0 latency numbers are not comparable with a new run.

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

##### Phase S3 completion record (2026-09-24)

**Status:** complete — the Image mapper and the AI store write typed and folded search
columns, and search, stats, and the thumbnail filter read only those columns. The SQL
targets are met in a release DuckDB (measured with the DuckDB CLI on the S3 schema); the
debug-build S0 benchmark is about 200 times faster but still above the targets (see below).

Branch: `refactor/library-search-s3-typed-search-columns` (on top of
`feature/library-search-s2-runtime-dng-profile`, PR 195). Project format stays
0.10.0 (Decision D1: S2 and S3 share one cutover).

**What changed:**

| Step | Change |
| --- | --- |
| 1 | `Image` DDL gains `file_stem`, `file_ext`, `capture_at TIMESTAMP`, `capture_date DATE`, `camera_make`, `camera_model`, `lens`, `iso INTEGER`, `focal_mm DOUBLE`, `aperture DOUBLE`, `rating INTEGER`, `pixel_count BIGINT`, `file_search_text`, `exif_search_text`. `ImageMapper::ToParams` fills them through the new `FillImageSearchColumns` (`storage/mapper/image/image_search_columns.{hpp,cpp}`) on every insert and update; `FromRawData` reads the 19-column row and ignores the derived columns. `ParseCaptureDateTime` accepts `YYYY-MM-DD HH:MM:SS` and `YYYY:MM:DD ...` and gives NULL for invalid days (for example `0000:00:00`). A metadata value of 0 (unknown ISO, focal length, aperture, size) is stored as NULL. Focal length and aperture are rounded to two decimals (float `2.8f` is stored as `2.8`). The shared fold is `FoldSearchText` (`utils/string/search_text.{hpp,cpp}`, in `StrConv`): the separator set of the old SQL `REPLACE` chain, then `towlower`. The query uses the same fold. duckorm gained `NULLABLE_INT64`, NULL-aware reads for text and nullable types (a NULL text cell was undefined behavior before), and frees the strings it reads. |
| 1 (deviation) | The plan's single `search_text` is two columns: `file_search_text` (file name + parent folder name, the "path tail") and `exif_search_text` (make, model, lens, lens make, date text). One column cannot keep the Filename and EXIF field-mask bits apart. Parts are joined with one space; a folded query has no space, so a match never crosses two parts. `pixel_count` is not in the plan list: it replaces `json_extract(... '$.ImageSize')` for `FilterField::ImageSize`. |
| 2 | `AiImageUnderstanding` gains `caption_search_text` (folded caption + scene) and `tags_search_text` (folded tags, one space between tags, no JSON syntax). `AiStore::UpsertUnderstandings` sets them with a bound `UPDATE` in the upsert transaction. The first attempt used a second duckorm upsert (`INSERT ... ON CONFLICT DO UPDATE`). On a row already written in the same transaction, DuckDB reset the unlisted columns (the caption read back empty). `AiUnderstandingUpsertWritesFoldedCaptionAndTagsSearchText` found this. |
| 3 | `BuildScopedFileQuery` adds one `LEFT JOIN` (alias `u`) when a predicate is present: the active understandings grouped by `file_id`. The key allows several active rows per file (one per `task_id`), so grouping keeps one row per file and the join never adds result rows. The correlated `string_agg` subqueries are deleted. |
| 4 | `SemanticLabelExpr` (a `CASE` over all label aliases for each row) is deleted. `SemanticLabelClause` expands aliases on the query side: `EXISTS (SELECT 1 FROM SemanticImageLabel sl WHERE sl.file_id = e.id AND sl.model_key = ? AND (LOWER(sl.label) IN (...) OR contains(LOWER(sl.label), LOWER(?))))`. A definition matches when the folded term is part of one of its folded aliases. |
| 5 | `ElementStore::BuildFolderStats` groups by `capture_date`, `camera_model`, `lens`, and `rating`. The thumbnail filter compiler (`FilterSQLCompiler::FieldToColumn`) and the bucket factories use the typed columns. `BuildCaptureDateUnknownFilter` is `i.capture_date IS NULL`, so it now equals the NULL date stats bucket. Before, a non-empty date text that did not parse was in the NULL bucket but not in the unknown filter. |
| 6 | `ApplyStarRatingLight` → `FlushPendingStarRatings` writes through `ImagePoolService::SyncWithStorage` → `ImageStore::UpdateImages` → `ImageMapper::ToParams`, so `rating` is rewritten with the metadata. `StarRatingWriteUpdatesRatingColumnStatsAndRatingFilter` proves this on the same pool calls. |
| Search builder | `TokenSearchClause` and `SearchDocumentClause` keep the S0 structure (per-token OR, AND of tokens, whole-query alternative, AI BM25) for S4 to replace. They use folded `contains` on the enabled search text columns; literal `contains` on `e.element_name`, `i.file_name`, make, model, lens, and the ISO / focal / aperture text; and date ranges on `i.capture_date`. `FoldedDocumentClause`, `FoldSqlSearchSeparators`, `SearchDocumentExpr`, and the AI string subqueries are deleted. As before, a token with `%`, `*`, `?`, `'`, or `"`, or a token that is mostly separators, matches only literally. `FuzzySearchEscapesSqlLikeWildcardsAndQuotesInWideInput` found this on the first run: `100%_` folded to `100` and matched a decoy file. |

**Primary success call chain (write):**

```text
Import / star rating / HDR flag -> Image in the image pool (MODIFIED)
  -> ImagePoolService::SyncWithStorage -> ImageStore::AddImages / UpdateImages
  -> ImageMapper::ToParams -> Image::ExifToJson + FillImageSearchColumns(name, path, display)
  -> duckorm insert / upsert of the 19 Image columns (one row write)
AI describe -> AlbumImageAnalysisSink -> AiStore::UpsertUnderstandings
  -> insert_or_replace(AiImageUnderstanding) + WriteUnderstandingSearchText (same transaction)
```

**Primary success call chain (read):**

```text
SearchController / StatsEngine -> SleeveFilterService::BuildFuzzySearchWhere
  -> TokenSearchClause: FoldSearchText(token) -> contains(i.file_search_text | i.exif_search_text
     | u.caption_search_text | u.tags_search_text, ?) OR literal columns OR i.capture_date range
     OR SemanticLabelClause EXISTS
  -> ElementStore::CountFilesInFolder / ListFilesInFolderPage / BuildFolderStats
  -> BuildScopedFileQuery: Element ⋈ FileImage ⋈ Image ⟕ grouped active AiImageUnderstanding
  -> GROUP BY capture_date / camera_model / lens / rating (no metadata JSON read)
```

**Primary failure call chains:**

```text
Date text missing or invalid -> ParseCaptureDateTime nullopt -> capture_at/capture_date NULL
  -> NULL date stats bucket == BuildCaptureDateUnknownFilter rows; date search ranges skip the row
Metadata value 0 (unknown) -> iso / focal_mm / aperture / pixel_count NULL -> typed filters skip it
AI description invalid or orphan -> no row, no search text (unchanged guard)
Search text UPDATE fails -> runtime_error -> transaction rolled back, no partial AI row
Folder without AI rows -> u columns NULL -> COALESCE(..., '') -> no match, no row loss
```

**What was proven (executed tests):**

| Plan criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Fold rule and substring property | `FoldRemovesSeparatorsAndLowercasesAndKeepsSubstrings` | `LibrarySearchColumnsTest` (new) | PASS |
| Capture date parse and rejection | `ParsesExifDateFormsAndRejectsInvalidDates` | `LibrarySearchColumnsTest` | PASS |
| Step 1: column derivation (NULL for 0, rounding, rating clamp, search texts) | `DerivesTypedValuesAndFoldedTextFromMetadata` | `LibrarySearchColumnsTest` | PASS |
| Step 1: stored row values; an update rewrites the columns; 19-column read back | `ImageRowStoresSearchColumnsAndRewritesThemOnUpdate` | `LibrarySearchColumnsTest` | PASS |
| Step 6: the star-rating path updates `rating`, stats, and the rating bucket filter | `StarRatingWriteUpdatesRatingColumnStatsAndRatingFilter` | `LibrarySearchColumnsTest` | PASS |
| Acceptance: search, stats, bucket filters, and typed conditions give the same results with `Image.metadata` set to `{}`; the compiled search SQL has no `json_extract`, `metadata`, or `REPLACE(` | `SearchStatsAndFiltersGiveSameResultsWithMetadataJsonCleared` | `LibrarySearchColumnsTest` | PASS |
| Step 2: AI search text written and replaced on a re-run; caption kept | `AiUnderstandingUpsertWritesFoldedCaptionAndTagsSearchText` | `LibrarySearchColumnsTest` | PASS (FAILED on the first run: see step 2) |
| Step 3: two active understandings count one file; caption and tag masks stay apart | `AiSearchJoinsEachFileOnceAndKeepsCaptionAndTagMasksApart` | `LibrarySearchColumnsTest` | PASS |
| Step 4: semantic labels (aliases, no active model, stats EXISTS filter) | `FuzzySearchMatchesGeneratedSemanticLabelsAsOrdinaryText`, `FuzzySearchIgnoresSemanticLabelsWhenNoModelIsActive`, `StatsSemanticLabelExistsFilterRestrictsFolderStats`, `LabelQueryUsesOrdinaryPathNotSemanticProvider` | `FilterServiceTest` | PASS |
| Search and stats regression (field mask, AI persistence, wildcards, buckets, album scope) | all cases | `FilterServiceTest` | PASS (SQL text expectations changed to typed columns) |
| S0 recall table | `FuzzySearchReturnsExpectedFilesForEachRecallCase` | `LibrarySearchRecallTest` | PASS. `dng` → `kPasses` (only the two DNG files). `raw` → `kKnownDefect`: it matched every file only through the `RawRuntimeColorContext` key in the metadata dump and now matches only `raw00011`; the S4 file kind term restores it. Other cases are unchanged; `6.7` now returns only `IMG_0067.CR3` (still a defect until the S4 date terms). |
| S0/S1 local Nikon import and file name recall | `DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames` | `LibrarySearchRecallTest` | PASS (local, 8.1 s) |
| Filter compiler and bucket factories | all cases | `SleeveFilterCompileTest`, `SleeveFilterFactoryTest` | PASS (expected SQL text changed) |
| Regression | `ProjectServiceTest`, `ImportRawOnlyTest`, `SleeveServiceTest`, `MetadataExtractorTest`, `PipelineDngProfileBindingTest`, `PipelineMapperTest`, `MapperCrtpRoundtripTest`, `SleeveFSTest`, `DuckormExprTest`, `CommitGraphTest`, `BatchImportDngMetadataTest`, `SleeveFilesystemCiTest`, `SearchQueryClassifierTest`, `ImportPipelineDocumentTest`, `SemanticGenerationServiceTest`, `ExportServiceTest`, `AlbumBackendRatingTest`, `AlbumBackendStatsFilterTest`, `AlbumBackendImageDetailsTest`, `AlbumBackendFolderTest`, `AlbumBackendImageDeleteTest` | ctest | PASS |

**Measurements (before → after):**

| Measurement | S0 (debug) | S3 debug `LibrarySearchBenchmarkTest` | S3 release DuckDB CLI | Target |
| --- | --- | --- | --- | --- |
| 1000 files, miss (`jpg`), preview (count + page) | 10.4 s | p50 53 ms, p95 58 ms | 5 ms for each `COUNT(*)` → about 10 ms | p95 ≤ 10 ms |
| 1000 files, miss, apply (count + page + stats) | 35.2 s | p50 126 ms, p95 128 ms | not measured | ≤ 50 ms |
| 20 000 files, miss, preview | 34.4 s | not run | 23 ms for each `COUNT(*)` → about 46 ms | p95 ≤ 50 ms |

All five 1000-file S0 queries (`jpg`, `2026-06-07`, `P1000123`, `6.7`, `dsc`) take 52–61 ms
preview and 126–135 ms apply in the debug build. A miss is no longer slower than a hit. The
release numbers come from the DuckDB CLI on the S3 schema with 1000 and 20 000 synthetic
rows and the compiled `jpg` predicate (all columns and the AI join). They measure the SQL
cost, not the app path. The app path cannot run with release test targets (`build/release`
has `ALCEDO_BUILD_TESTS=OFF`), so the debug numbers are the only measurement of the app path.
`demo.alcd` is a 0.9.0 project and does not open after the S2 cutover, so the packed-project
benchmark was not run (Phase S6 re-import).

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target LibrarySearchColumnsTest FilterServiceTest LibrarySearchRecallTest ProjectServiceTest ImportRawOnlyTest SleeveServiceTest MetadataExtractorTest PipelineDngProfileBindingTest PipelineMapperTest MapperCrtpRoundtripTest SleeveFSTest SleeveFilterCompileTest SleeveFilterFactoryTest DuckormExprTest CommitGraphTest BatchImportDngMetadataTest SleeveFilesystemCiTest SearchQueryClassifierTest ImportPipelineDocumentTest SemanticGenerationServiceTest ExportServiceTest LibrarySearchBenchmarkTest AlbumBackendRatingTest AlbumBackendStatsFilterTest AlbumBackendImageDetailsTest AlbumBackendFolderTest AlbumBackendImageDeleteTest
ctest --test-dir build/debug -R "<the targets above>" -j 1   -> 266/266 passed (6 disabled by design)
LibrarySearchRecallTest.exe --gtest_also_run_disabled_tests --gtest_filter=*Nikon*   -> 1/1 passed
LibrarySearchBenchmarkTest.exe --gtest_also_run_disabled_tests --gtest_filter=*OneThousand*
```

Full `ctest` suite: not run (agent rule). `alcedo_main` was not linked; `AlbumBackendLib`
builds (the album backend tests link it).

**Checklist / exit condition:** steps 1–6 done. Acceptance: no search, stats, or thumbnail
filter SQL contains `json_extract`, `CAST(i.metadata`, or `REPLACE(` (source search of
`app/`, `sleeve/`, and `storage/`, plus the cleared-metadata test). SQL targets: met in a
release DuckDB by measurement of the SQL; the debug app path is 53–58 ms preview and
126–135 ms apply for 1000 files.

**LOC note:** `sleeve_filter_service.cpp` 750 → 622. New files: `image_search_columns.{hpp,cpp}`
52 + 154, `search_text.{hpp,cpp}` 33 + 78, `library_search_columns_test.cpp` 450.
`ai_store.cpp` 565, `element_store.cpp` 571, `duckdb_orm.cpp` 541. No file is near the
1000-line limit.

**Remaining gaps:**

- The apply path (page + stats) takes 126 ms in the debug build and was not measured in
  release. Phase S5 step 5 decides whether it moves to the worker.
- `raw` recall is a known defect until the S4 file kind term. The date forms (`6.7`,
  `6月7日`, `June 7`) and the parameters (`iso800`, `f2.8`, `35mm`) stay known defects for S4.
- The literal clauses (`e.element_name`, `i.file_name`, make, model, lens, and the numeric
  text) and the whole-query alternative stay until S4 replaces the builder.
- The AI BM25 clause (`AiImageFtsDocument`) is unchanged.
- `tests/storage/ai_storage_controller_test.cpp` has no build target (it was not registered
  before this phase). `LibrarySearchColumnsTest` tests the AI search text instead.

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

##### Phase S4 completion record (2026-09-25)

**Status:** complete — `SearchQueryParser` turns the query into typed terms and the WHERE
builder compiles one clause for each term, combined with AND. Every Phase S0 recall case
passes, including the local Nikon cases with exact `z8` and `dng` sets.

Branch: `refactor/library-search-s4-query-parser` (on top of
`refactor/library-search-s3-typed-search-columns`). Project format stays 0.10.0 (the S4
column below joins the unreleased S2/S3 cutover of Decision D1).

**What changed:**

| Step | Change |
| --- | --- |
| 1 | New `app/search_query_parser.{hpp,cpp}` (in the `SleeveFilterService` library): `SearchQueryParser::Parse(query) -> std::vector<SearchTerm>`. A `SearchTerm` has a kind (`Date`, `FileKind`, `CaptureParameter`, `Text`), the query text, its folded text, and the typed value (`SearchDate` with `Day` / `YearMonth` / `Year` / `MonthDay` / `Month`, an extension list, or an ISO / aperture / focal length value). Date forms: `Y-M-D` with `- . / _`, `YYYYMMDD`, `YYYY.M`, `YYYY` (1000–9999), `M.D` / `M/D` / `M-D`, `M月D日` (also `号`), `M月`, `YYYY年M月`, `YYYY年M月D日`, `YYYY年`, and English month names (`June`, `June 7`, `7 June`, `June 7, 2026`, `Sept 2025`; a three-letter abbreviation only with a day or year, so `mar` and `jun` stay text). Invalid days (`2.30`, `2025.2.29`) are text. Kinds: `jpg`/`jpeg`, `tif`/`tiff`, `heic`/`heif`, `png`, `webp`, `raw` (29 LibRaw RAW extensions), and each RAW extension, with or without a dot. Parameters: `iso800`, `iso 800`, `ISO-800`, `f2.8`, `f/2.8`, `35mm`, `35 mm`. |
| 2 | `TermClause`: a date term is `(capture_date predicate OR text)`: `= DATE`, a `[from, to)` range for a month or year, or `month(i.capture_date) = M [AND day(i.capture_date) = D]` for any year. A file kind term is `(i.file_ext IN (...) OR text)`. A parameter term is `(i.iso = ? OR text)` or `(abs(i.aperture - ?) < 0.005 OR text)` (same for `i.focal_mm`). The S3 month range for `YYYY.12` built `DATE 'YYYY-13-01'` (found by reading the code); the range now ends at January 1 of the next year. |
| 3 | A text term is `contains(<folded search text column>, folded token)` for each column that the mask enables (file, EXIF, caption, tags), plus the CLIP label `EXISTS` clause under AiTags. The S3 literal clauses on `e.element_name`, `i.file_name`, make, model, lens, and `CAST(i.iso / focal_mm / aperture AS VARCHAR)` are deleted for folded tokens (no feature renames a library file, so the Element name equals the Image file name). A bare number (`800`) no longer matches the ISO column; the user writes `iso800`. |
| 3 (kept) | A token with `%`, `*`, `?`, `'`, or `"`, or a token that folding shrinks to less than half, is a literal text term: it matches the name, camera, and lens columns as typed. `FuzzySearchEscapesSqlLikeWildcardsAndQuotesInWideInput` needs this: `100%_` must not fold to `100` and match `1000A...`. |
| 4 | Terms combine with AND. The whole-query alternative (`SearchDocumentClause`) is deleted; `P263 5860` matches through two text terms. `FoldedDocumentClause` was already deleted in S3. The AI BM25 clause (`AiImageFtsDocument`, both AI bits on) stays OR-ed with the whole predicate. |
| 5 | The typed part of a term needs the field bit of its column: Exif for dates and parameters, Filename for file kinds. The text part follows the enabled text columns. |
| 6 (deviation) | No typo tolerance. The plain substring rule of step 6 cannot give the exact `z8` set of the acceptance: the lens `NIKKOR Z 85mm f/1.8 S` of the Z6II and Zf files folds to `nikkorz85mmf18s`, which contains `z8`. New rule for the EXIF text only: when a token ends with a digit, a match that crosses a word boundary must not end inside a number. `Image` gains `exif_search_words` (the `exif_search_text` parts folded with the new `FoldSearchWords`: one space between words, `\|` between parts). The clause is `contains(i.exif_search_text, ?) AND (contains(i.exif_search_words, ?) OR regexp_matches(i.exif_search_words, ?))` with the pattern `z ?8(?:[^0-9]\|$)` for `z8`; the regular expression runs only on rows that pass the first `contains`. File names keep the plain rule because users type counter prefixes (`dsc223` while typing `DSC_2230`), which the same rule would reject. AI text keeps the plain rule. |

**Primary success call chain:**

```text
SearchController / StatsEngine / tests -> SleeveFilterService::CountSearchResults / SearchFolder
  / BuildFolderStats(extra) -> BuildFuzzySearchWhere(query, mask)
  -> SearchQueryParser::Parse(query) -> [Date | FileKind | CaptureParameter | Text] terms
  -> TermClause(term, active_model_key, mask) for each term
     -> CaptureDateClause / FileKindClause / CaptureParameterClause (field bit on)
     -> FoldedTextClause for each enabled text column (EXIF: number-end check)
     -> SemanticLabelClause (Text term, AiTags on)
  -> AND of term clauses [OR AI BM25] -> RawSQL FilterNode with binds
  -> ElementStore::CountFilesInFolder / ListFilesInFolderPage / BuildFolderStats
```

**Write path addition:**

```text
ImageMapper::ToParams -> FillImageSearchColumns
  -> exif_search_text  = JoinFoldedParts(make, model, lens, lens make, date text)
  -> exif_search_words = JoinWordFoldedParts(same parts)   (FoldSearchWords, `|` between parts)
```

**Primary failure call chains:**

```text
Empty or white-space query -> Parse returns [] -> std::nullopt (no filter)
Mask 0 -> RawSQL FALSE (unchanged)
Date that does not exist (2.30, 2026-02-30, 6月32日) -> Text term -> folded text only
Typed column NULL (unknown date / ISO) -> typed predicate NULL -> only the text part can match
Typed column's field bit off -> typed part omitted; no enabled column -> term is 1=0 -> no rows
Token with an SQL wildcard or quote -> literal Text term -> bound contains on name/camera/lens
```

**What was proven (executed tests):**

| Plan criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Step 1: date forms and their calendar ranges (28 forms) | `DateFormsParseToTheNamedCalendarRange` | `SearchQueryParserTest` (new, `ci_core`) | PASS |
| Step 1: invalid or unlisted date forms stay text | `InvalidOrUnlistedDateFormsStayText` | `SearchQueryParserTest` | PASS |
| Step 2: date terms keep the folded text alternative | `DateTermKeepsFoldedTextAlternative` | `SearchQueryParserTest` | PASS |
| Step 1: file kinds and the RAW extension list | `FileKindTermsListTheirExtensions` | `SearchQueryParserTest` | PASS |
| Step 1: capture parameters | `CaptureParameterFormsParseToFieldAndValue`, `WordsThatOnlyLookLikeParametersStayText` | `SearchQueryParserTest` | PASS |
| Step 3: folded and literal text terms | `TextTermsFoldSeparatorsUnlessTypedLiterally` | `SearchQueryParserTest` | PASS |
| Step 4: term order, multi-token terms, empty query | `QuerySplitsIntoTermsInOrder` | `SearchQueryParserTest` | PASS |
| Acceptance: every S0 recall case (28; the 10 known defects are now `kPasses`) plus 13 S4 cases (`2026年6月`, `June`, `7 June`, `June 7 2026`, `3月`, `March 2024`, `iso 800`, `f/2.8`, `35 mm`, `.nef`, `dng 2026`, `raw 6.7`, `rw2 iso200`). `6.7` returns the three June 7 files and `IMG_0067` only. | `FuzzySearchReturnsExpectedFilesForEachRecallCase` | `LibrarySearchRecallTest` | PASS |
| Step 5: the typed part needs its field bit | `TypedTermsMatchOnlyWhenTheirFieldBitIsEnabled` | `LibrarySearchRecallTest` | PASS |
| Step 2: December month range, month and year terms | `YearMonthAndMonthTermsCoverDecember` | `LibrarySearchRecallTest` | PASS |
| Step 6 deviation: EXIF number-end rule; file name prefixes kept | `CrossWordMatchDoesNotEndInsideANumber` | `LibrarySearchRecallTest` | PASS |
| Acceptance: local Nikon folder, exact `z8` (4 files) and `dng` (3 files) | `DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames` | `LibrarySearchRecallTest` | PASS (local, 7.9 s). FAILED before the step 6 rule: `z8` also returned `DSC_0261.NEF` (Z6II) and `DSC_1456.NEF`, `DSC_1535.NEF` (Zf) through the lens `NIKKOR Z 85mm`. |
| `FoldSearchWords` form | `FoldWordsKeepsOneSpaceBetweenWordsAndMatchesTheFoldWithoutSpaces` | `LibrarySearchColumnsTest` | PASS |
| Stored `exif_search_words`; 20-column Image read back | `DerivesTypedValuesAndFoldedTextFromMetadata`, `ImageRowStoresSearchColumnsAndRewritesThemOnUpdate` | `LibrarySearchColumnsTest` | PASS |
| Parameters with the metadata JSON cleared; bare `800` no longer matches ISO | `SearchStatsAndFiltersGiveSameResultsWithMetadataJsonCleared` | `LibrarySearchColumnsTest` | PASS (S3 had `800` → Nikon; now `iso800`, `f2.8`, `35mm` → Nikon and `800` → none) |
| Search and stats regression | all cases | `FilterServiceTest` | PASS. `StatsBarAndSearchMergeUnderOneCompiledPredicate`: `50mm` is now a focal length term, so the fixture's `NIKKOR 24mm` file gets `focal_ = 24` (it had the default 50). |
| Regression | `SearchQueryClassifierTest`, `ProjectServiceTest`, `ImportRawOnlyTest`, `SleeveServiceTest`, `MetadataExtractorTest`, `PipelineDngProfileBindingTest`, `PipelineMapperTest`, `MapperCrtpRoundtripTest`, `SleeveFSTest`, `SleeveFilterCompileTest`, `SleeveFilterFactoryTest`, `DuckormExprTest`, `CommitGraphTest`, `BatchImportDngMetadataTest`, `SleeveFilesystemCiTest`, `ImportPipelineDocumentTest`, `SemanticGenerationServiceTest`, `ExportServiceTest`, `AlbumBackendRatingTest`, `AlbumBackendStatsFilterTest`, `AlbumBackendImageDetailsTest`, `AlbumBackendFolderTest`, `AlbumBackendImageDeleteTest` | ctest | PASS |

**Measurements** (debug build, `LibrarySearchBenchmarkTest`, 1000 files, p50 / p95):

| Query | Matches S3 → S4 | Preview S4 | Apply S4 |
| --- | --- | --- | --- |
| `jpg` | 0 → 0 | 58.7 / 59.5 ms | 130.0 / 135.0 ms |
| `2026-06-07` | 1 → 1 | 56.3 / 62.6 ms | 124.0 / 128.6 ms |
| `P1000123` | 0 → 0 | 54.3 / 54.6 ms | 116.0 / 117.6 ms |
| `6.7` | 1000 → 23 | 53.8 / 57.1 ms | 118.7 / 122.3 ms |
| `dsc` | 493 → 493 | 52.4 / 58.9 ms | 113.9 / 116.8 ms |

The S4 builder costs the same as S3 in the debug build (S3: 52–61 ms preview, 126–135 ms
apply). The release measurement was not repeated. The new clauses are the same `contains`
scans plus typed column compares, and the regular expression runs only for tokens that end
with a digit, on rows that already contain the token.

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target SearchQueryParserTest LibrarySearchRecallTest FilterServiceTest LibrarySearchColumnsTest SearchQueryClassifierTest
ctest --test-dir build/debug -R "SearchQueryParserTest|LibrarySearchRecallTest|FilterServiceTest|LibrarySearchColumnsTest|SearchQueryClassifierTest" -j 1   -> 74/74 passed (Nikon test disabled by design)
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target <the 22 regression targets above> LibrarySearchBenchmarkTest
ctest --test-dir build/debug -R "^(<the 22 regression targets>)\." -j 1   -> 204/204 passed (5 disabled by design)
LibrarySearchRecallTest.exe --gtest_also_run_disabled_tests --gtest_filter=*Nikon*   -> 1/1 passed
LibrarySearchBenchmarkTest.exe --gtest_also_run_disabled_tests --gtest_filter=*OneThousand*
```

Full `ctest` suite: not run (agent rule). `alcedo_main` was not linked; `AlbumBackendLib`
builds (the album backend tests link it).

**Checklist / exit condition:** steps 1–5 done; step 6 done with the EXIF number-end
deviation above. Acceptance: every Phase S0 recall case passes, including the disabled Nikon
cases with exact sets; `6.7` matches only June 7 captures and the stem `IMG_0067`.

**LOC note:** `sleeve_filter_service.cpp` 622 → 592. New files: `search_query_parser.hpp` 99,
`search_query_parser.cpp` 526, `search_query_parser_test.cpp` 180.
`library_search_recall_test.cpp` 344 → 478. No file is near the 1000-line limit.

**Remaining gaps:**

- The debug app path is still 52–59 ms preview and 114–135 ms apply for 1000 files. Phase S5
  moves search off the UI thread and measures apply.
- `SearchQueryClassifier` (route choice) and `SearchQueryParser` (term meaning) both scan the
  query; the classifier is unchanged.
- The AI BM25 clause is unchanged.

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

##### Phase S5 completion record (2026-09-25)

**Status:** complete — every search route of `SearchController` (typing preview, paging,
explicit submit, applied fuzzy search, exact search) runs its SQL on one search worker with
latest-wins coalescing. The UI thread only classifies the query, builds result rows from
query columns, and commits worker results. The synchronous `SearchPreview` and `SubmitSearch`
`Q_INVOKABLE`s are deleted.

Branch: `refactor/library-search-s5-search-worker` (on top of
`refactor/library-search-s4-query-parser`). No schema change.

**What changed:**

| Step | Change |
| --- | --- |
| 1 | `GlobalSearchDialog.qml` `executePendingSearch` calls `RequestSearch` (preview) or `RequestSubmitSearch` (submit) and stores the returned id; the response arrives through `onSearchResponseReady`. `applySearchResponse` now requires `requestId == activeSearchRequestId` and a non-zero id: an older response that arrived between `beginSearchRequest` (id reset to 0) and `executePendingSearch` was accepted before. `SearchPreview` and `SubmitSearch` are deleted; the test callers (`AlbumBackendImportTest`, `GlobalSearchDialogQmlTest`, `MainQmlWorkflowTest`) use `RequestSearch` / `RequestSubmitSearch`. |
| 2 | New `SearchRequestWorker` (`album_backend/search_request_worker.{hpp,cpp}`, no Qt): one `std::thread`, a queue in submit order, at most one pending request for each `SearchRequestKind` (`kPreview`: dialog page and submit; `kApply`: grid page and stats), so a preview never removes a pending apply. `Submit` returns a generation (unique across kinds) and removes the pending request of the same kind; `IsCurrent(kind, generation)` tells the owner whether a result that finished on the worker is still the newest; `Invalidate(kind)` drops the pending request and marks the running one stale. The destructor drops pending requests and joins after the running job. Executable interleaving (AGENTS.md rule): request N runs on the worker while request N+1 is submitted from the UI thread; N posts its result to the UI thread queue before N+1 finishes — `RapidPreviewRequestsDeliverOnlyTheNewestResponse` fails with 2 responses when the `IsCurrent` check is removed. The detached `std::thread` per semantic submit is gone. |
| 3 | New `ElementStore::ListSearchResultPage`: `SELECT e.id, fi.image_id, i.file_name, i.camera_model, i.lens, CAST(i.capture_date AS VARCHAR), i.rating, COUNT(*) OVER () ... ORDER BY e.id LIMIT/OFFSET` — page, display columns, and total in one statement. A page past the last row has no window value; then one `COUNT(*)` supplies the total. `ListSearchResultRows(ids)` reads the display columns of the semantic provider's ranked ids in their order. `SleeveFilterService::SearchFolderPage` (query → WHERE → page), `ListSearchResultPage` (compiled filter → page), and `SearchFolderSemanticRows` wrap them. Deviation from the step wording: `ListFilesInFolderPage` keeps its three columns, because the album grid (`AlbumBrowseService`) also uses it and needs no display columns; search uses the new method. |
| 4 | `SearchController::BuildResultRows(const std::vector<SearchResultRow>&)` reads `fileName`, `cameraModel`, `lens`, `captureDate`, `rating` from the row and the thumbnail state from `LibraryModule::FindAlbumItem`. The image pool `Read` for each row is deleted. |
| 5 | Measured after S3/S4 (debug, 1000 files): apply 61–78 ms p50 (below). It is above 16 ms, so apply moved to the worker: `RunSearchApplyRequest` builds the WHERE (it reads the active semantic model and the AI index state, both SQL), `ListSearchResultPage(folder, filter, 0, 120)`, and `BuildFolderStats(folder, filter)`. `CommitAppliedSearch` on the UI thread installs the filter, clears the stats filters, and calls the new `LibraryModule::ApplySearchWindow` (the reset part of `LoadThumbnailWindow`, split into `ResetThumbnailWindow` / `PublishThumbnailWindowPage`) and `StatsEngine::ApplyFolderStats` (the property part of `RefreshStats`). `ApplyExactSearch` uses the same apply request with a prepared `e.id = ?` filter. `ClearFuzzySearch` and `ClearSearchState` (folder change) call `Invalidate(kApply)`: before this, an apply still on the worker would install its search after the user cleared it (`ClearFuzzySearchDropsAnApplyStillOnTheWorker` fails without it). A failed apply keeps the previous grid, stats, and query and logs the error. |
| 6 | The 140 ms `previewTimer` debounce and the 24 ms `searchExecutionTimer` are unchanged. |
| 7 | `SleeveFilterService::SetQueryThreadObserver`: an observer that receives the operation name at the start of `BuildFuzzySearchWhere`, `SearchFolderPage`, `ListSearchResultPage`, `SearchFolderSemanticRows`, `SearchFolder`, `SearchFolderSemantic`, `CountSearchResults`, and `BuildFolderStats`, on the query's thread (mutex-protected, empty by default). The tests' `SearchQueryThreadRecorder` fails on any call from the UI thread. |
| Shutdown | `ApplicationModuleHost::ShutdownModules` calls the new `SearchController::CancelSearchRequests` (invalidates both kinds); the controller destructor joins the worker before anything else, so a running job can post to `this` (a result posted during teardown is removed with the object's posted events). |

**Primary success call chain (typing preview):**

```text
GlobalSearchDialog onTextChanged -> previewTimer (140 ms) -> refreshPreview -> beginSearchRequest
  -> searchExecutionTimer (24 ms) -> executePendingSearch
  -> SearchController::RequestSearch -> RequestSearchPage (UI: ClassifySearchQuery, field mask,
     folder id, filter service shared_ptr)
  -> SearchRequestWorker::Submit(kPreview) [pending preview removed]
  -> worker: RunSearchPageRequest -> SleeveFilterService::SearchFolderPage
     -> BuildFuzzySearchWhere -> ElementStore::ListSearchResultPage (one statement: rows + total)
  -> QMetaObject::invokeMethod(UI) -> IsCurrent(kPreview, generation)
  -> BuildResultRows (query columns + LibraryModule thumbnail state) -> SearchResponseReady
  -> QML applySearchResponse (requestId == activeSearchRequestId) -> readPreviewResponse
```

**Applied search chain:**

```text
Enter / recommendation / field toggle -> SearchController::ApplyFuzzySearch (or ApplyExactSearch)
  -> SubmitApplyRequest -> SearchRequestWorker::Submit(kApply)
  -> worker: RunSearchApplyRequest -> BuildFuzzySearchWhere -> ListSearchResultPage(0, 120)
     -> BuildFolderStats
  -> UI: IsCurrent(kApply) -> CommitAppliedSearch -> StatsEngine::ClearFilters
     -> LibraryModule::ApplySearchWindow -> StatsEngine::ApplyFolderStats
     -> StatsFilterChanged + SearchStateChanged
```

**Primary failure call chains:**

```text
Newer request of the same kind while one waits -> pending request removed, never runs
Newer request while one runs -> result posted, IsCurrent false on the UI thread -> dropped
Clear search / folder change while an apply runs -> Invalidate(kApply) -> result dropped
Apply query parses to no terms -> CommitAppliedSearch -> ClearFuzzySearch
Apply SQL throws -> error_text -> previous grid, stats, and query kept; qWarning
Preview SQL throws -> response searchErrorText, no rows
Semantic submit without provider / too long / no folder -> semanticUnavailable / tooLong
Controller destroyed with a job running -> worker joined first; queued result removed with the object
```

**What was proven (executed tests):**

| Plan criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Step 2: a pending request is replaced by the newer one of its kind (20 submits → 2 runs) | `PendingRequestIsReplacedByTheNewerRequestOfItsKind` | `SearchRequestWorkerTest` (new) | PASS |
| Step 2: kinds do not replace each other; submit order kept | `RequestOfAnotherKindKeepsItsPendingRequest` | `SearchRequestWorkerTest` | PASS |
| Step 2: the running request becomes stale | `RunningRequestBecomesStaleWhenANewerRequestArrives` | `SearchRequestWorkerTest` | PASS |
| Clear: invalidate drops pending, marks running stale | `InvalidateDropsThePendingRequestAndMarksTheRunningOneStale` | `SearchRequestWorkerTest` | PASS |
| Shutdown: destructor waits for the running job, drops pending | `DestructorDropsPendingRequestsAndWaitsForTheRunningJob` | `SearchRequestWorkerTest` | PASS |
| Step 3: page + display columns + total in one statement; past-end total; rows by id in order; no semantic substitute | `SearchResultPageReadsDisplayColumnsAndTotalInOneStatement` | `LibrarySearchColumnsTest` | PASS |
| Steps 1, 3, 4: `RequestSearch` returns before the result; rows carry display columns from the query; paging total | `PreviewRowsCarryDisplayColumnsAndTotalFromTheQuery` | `AlbumBackendSearchWorkerTest` (new) | PASS |
| Step 2 + UI-thread target: 20 requests in one burst deliver one response (the newest); UI thread time 0.12 ms per request (< 2 ms asserted) | `RapidPreviewRequestsDeliverOnlyTheNewestResponse` | `AlbumBackendSearchWorkerTest` | PASS; FAILED (2 responses) with the `IsCurrent` check removed |
| Step 5: apply queries on the worker, commits grid, stats, cleared stats filters; nothing installed before the result | `ApplyFuzzySearchQueriesOnTheWorkerAndCommitsGridAndStats` | `AlbumBackendSearchWorkerTest` | PASS |
| Step 5: clear and folder change drop an apply still on the worker | `ClearFuzzySearchDropsAnApplyStillOnTheWorker` | `AlbumBackendSearchWorkerTest` | PASS; FAILED (search installed, 3 of 12 shown) without `Invalidate` in `ClearFuzzySearch` |
| Step 7: no search SQL on the UI thread (preview, paging, submit, apply, exact, field toggle re-apply) | `SearchSqlNeverRunsOnTheUiThread` | `AlbumBackendSearchWorkerTest` | PASS |
| Acceptance: scripted QML test types 20 characters quickly (each sends a request) and the dialog applies only the last response | `TypingTwentyCharactersQuicklyAppliesOnlyTheLastResponse` | `GlobalSearchDialogQmlTest` | PASS; FAILED (2 responses) with the `IsCurrent` check removed |
| Step 1 regression: dialog paging, thumbnails, reopen; semantic typing / submit; routes | the other 5 cases | `GlobalSearchDialogQmlTest` | PASS |
| Step 1 regression: paged preview and preview thumbnails (real RAW import) | `SearchPreview_ReturnsPagedResultsAndTotalCount`, `SearchPreviewThumbnail_LoadsForPagedVisibleResult` | `AlbumBackendImportTest` | PASS |
| Regression | all cases | `AlbumBackendStatsFilterTest`, `MainQmlWorkflowTest`, `FilterServiceTest`, `LibrarySearchRecallTest`, `LibrarySearchColumnsTest` | PASS |
| Regression | all cases | `SearchQueryParserTest`, `SearchQueryClassifierTest`, `ProjectServiceTest`, `SleeveServiceTest`, `SleeveFilterCompileTest`, `SleeveFilterFactoryTest`, `SleeveFSTest`, `SleeveFilesystemCiTest`, `SemanticGenerationServiceTest`, `AlbumBackendRatingTest`, `AlbumBackendImageDetailsTest`, `AlbumBackendFolderTest`, `AlbumBackendImageDeleteTest`, `ApplicationModuleHostLifecycleTest` | PASS (153/153, Nikon test disabled by design) |

**Measurements** (debug build, `LibrarySearchBenchmarkTest`, 1000 files, 10 runs, p50 / p95; the
benchmark now measures the S5 paths: preview = `SearchFolderPage`, apply = WHERE +
`ListSearchResultPage` + `BuildFolderStats`):

| Query | Matches | Preview S4 → S5 | Apply S4 → S5 |
| --- | --- | --- | --- |
| `jpg` | 0 | 58.7 / 59.5 → 24.2 / 29.7 ms | 130.0 / 135.0 → 61.1 / 69.9 ms |
| `2026-06-07` | 1 | 56.3 / 62.6 → 23.3 / 27.8 ms | 124.0 / 128.6 → 65.4 / 70.8 ms |
| `P1000123` | 0 | 54.3 / 54.6 → 27.1 / 31.7 ms | 116.0 / 117.6 → 71.8 / 87.0 ms |
| `6.7` | 23 | 53.8 / 57.1 → 26.3 / 35.4 ms | 118.7 / 122.3 → 78.0 / 81.0 ms |
| `dsc` | 493 | 52.4 / 58.9 → 24.8 / 27.5 ms | 113.9 / 116.8 → 71.5 / 76.7 ms |

The preview is one statement instead of two. Both paths now run off the UI thread; the UI
thread spends 0.12 ms for each preview request (`RapidPreviewRequestsDeliverOnlyTheNewestResponse`,
debug). The release numbers were not measured (release test targets are not built in
`build/release`).

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target SearchRequestWorkerTest AlbumBackendSearchWorkerTest LibrarySearchColumnsTest GlobalSearchDialogQmlTest AlbumBackendStatsFilterTest AlbumBackendImportTest MainQmlWorkflowTest FilterServiceTest LibrarySearchRecallTest LibrarySearchBenchmarkTest
ctest --test-dir build/debug -R "^(SearchRequestWorkerTest|AlbumBackendSearchWorkerTest|LibrarySearchColumnsTest|GlobalSearchDialogQmlTest|AlbumBackendStatsFilterTest|MainQmlWorkflowTest|FilterServiceTest|LibrarySearchRecallTest)\.|^AlbumBackendImportTest\..*Search" -j 1   -> 83/83 passed (Nikon test disabled by design)
ctest --test-dir build/debug -R "^(<the 16 regression targets above>)\." -j 1   -> 153/153 passed
LibrarySearchBenchmarkTest.exe --gtest_also_run_disabled_tests --gtest_filter=*OneThousand*   (ALCEDO_SEARCH_BENCH_REPEAT=10)
```

Full `ctest` suite: not run (agent rule). `alcedo_main` was not linked; `AlbumBackendLib`
builds (the album backend and QML tests link it).

**Checklist / exit condition:** steps 1–7 done (step 3 through a new search page method, see
the table). Acceptance: the UI thread runs no search SQL (observer test) and spends 0.12 ms for
each keystroke request; the scripted QML test types 20 characters and the dialog applies only
the last response.

**LOC note:** `search_controller.cpp` 857 → 823 (the detached-thread path and the duplicate
sync preview/submit code are deleted). New: `search_request_worker.hpp` 80,
`search_request_worker.cpp` 82, `search_request_worker_test.cpp` 182,
`album_backend_search_worker_test.cpp` 294. `element_store.cpp` 571 → 681,
`sleeve_filter_service.cpp` 592 → 657, `global_search_dialog_qml_test.cpp` 858 → 957.
`GlobalSearchDialog.qml` is 1434 lines (pre-existing size; this phase changed 27 lines).

**Remaining gaps:**

- `ClearFuzzySearch`, stats bar toggles, star rating refresh, and grid scrolling
  (`LoadMoreThumbnailView`) still run their page and stats SQL on the UI thread with the
  active search filter. They are library browsing actions, not search routes; Phase S5 did not
  cover them.
- A project switch while an apply runs is not checked: the apply commits into the new
  project's controllers. No short interleaving reaches it (opening a project takes longer than
  the 60–80 ms apply); no guard was added.
- `ApplySearchFieldEnabled` re-applies `active_search_query_`; after `ApplyExactSearch` that
  text is the display label (`Image 12`), which parses as a fuzzy query. This was already the
  behavior before S5.
- The UI-thread commit (row maps for 24 rows, QML apply) was not timed separately.

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
