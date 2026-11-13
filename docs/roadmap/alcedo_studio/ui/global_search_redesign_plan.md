# Global Search Redesign Plan

Date: 2026-10-17

Status: Phases GS1 to GS5 complete (2026-10-17, completion records in each phase). The manual
check of GS2 and GS4 in `alcedo_main` is open (see the GS4 completion record).

Source revision of the audit: `main` at `ada6f002c`.

Primary owner: Alcedo Studio library search (search palette, Library search results, and
natural-language ranking).

Tracking issue: [#423](https://github.com/zidage/AlcedoStudio/issues/423). Sub-issues: [#420](https://github.com/zidage/AlcedoStudio/issues/420) (GS1), [#421](https://github.com/zidage/AlcedoStudio/issues/421) (GS2), [#422](https://github.com/zidage/AlcedoStudio/issues/422) (GS3 to GS5). Related: [#424](https://github.com/zidage/AlcedoStudio/issues/424) (sidecar residency).

Affected areas:

- `qml/SearchPalette.qml` (search palette; replaced `qml/GlobalSearchDialog.qml` in GS4 and GS5)
- `ui/alcedo_main/album_backend/search_controller`, `search_session`, `library_module`,
  `stats_engine`
- `ui/alcedo_main/automation/commands/search_commands` (`search.query`)
- `app/sleeve_filter_service`, `app/project_service` (`ProjectSemanticSearchProvider`),
  `app/album_browse_service`, `app/project_package_backend`
- `storage/store/semantic/semantic_vector_search`, `semantic_score_elbow`
- `storage/store/sleeve/element_store`, `storage/store/sleeve/album_query`
- `qml/AlbumInspectorPanel.qml` (search filter card)

Related plans:

- [Semantic Generation and Search Integration Plan](../ai/semantic_generation_search_plan.md).
  This plan replaces its sections "Search and vector index behavior" and "Search UI Flow" for
  search ranking and the search palette. Its Phase 5f records the semantic apply to the album grid
  as complete, but the current source has no semantic apply path. Phase GS2 adds that path.
- [Library Search Performance, Recall, and Project Size Plan](../storage/library_search_and_project_size_plan.md).
  This plan depends on its match set (Phase S8) and its search worker (Phase S5). That plan
  excludes natural-language ranking. This plan adds it.
- [CLI, MCP, and Shared-Session Automation Plan](../automation/cli_mcp_automation_plan.md).
  This plan changes `SearchSession`, which `search.query` uses. The `search.query` response
  fields stay the same.
- [AI Sidecar Runtime Residency Plan](../ai/ai_sidecar_runtime_residency_plan.md). That plan
  keeps the sidecar and its text encoder loaded between searches. This plan does not depend on
  it. Without it, the first natural-language search after an idle period starts the sidecar.

## 1. Decision record

### 1.1 Product goal

The global search palette becomes a launcher, not a result browser. It shows a small number of
commands, structured filters, and photo previews. The complete result set opens in the Library.
Natural-language (CLIP) search returns only photos that match the query clearly. When no photo
matches clearly, the result is empty.

### 1.2 Approved decisions

1. **The palette shows previews, not a list.** The palette shows up to three photo previews in
   one row. Each preview has a thumbnail from the thumbnail service and a file name label below
   it. The palette has no result list, no paging, and no "Load more" control.
2. **Enter opens the Library.** Enter and the "View all" action apply the search to the
   Library and close the palette. This is the same operation for both search routes.
3. **Natural-language search runs only on Enter.** Typing does not start the sidecar. For a
   natural-language query, the palette shows a hint in place of the photo previews. Enter applies
   the natural-language search to the Library. The Library shows the results in relevance order.
   The palette does not show natural-language previews.
4. **The palette shows no result count.** Only the filter rows show a count. That count is the
   existing statistics bucket count.
5. **Mouse first.** Hover highlights a row. A click runs the row. The keyboard supports only the
   text field, Up and Down to move the row highlight, Enter, and Escape.
6. **No action panel.** The palette has no secondary action menu for a row or a photo.
7. **Search settings are a collapsible drawer.** The drawer is inside the palette, below the
   input row. It holds the search field check boxes and the natural-language switch. It does not
   change the palette width. Its open state persists.
8. **Natural-language search and field search stay mutually exclusive.** The natural-language
   switch disables the field check boxes, as today. Rank fusion (for example reciprocal rank
   fusion) is not part of this plan. Reason: the field search is a Boolean filter with no rank,
   and fusion needs a CLIP query for each typed query.
9. **Scope is the current folder, direct children only.** The search does not include
   subfolders. This is the current `BuildScopedFileQuery` scope.
10. **An empty result is correct.** When no photo passes the relevance rule, the Library shows
    its "No Matching Photos" state. The search does not add the nearest photos to fill the
    result.
11. **The relevance rule replaces the elbow cutoff for search.** Search uses an exact scan of the
    folder scope with a calibrated relevance score (Section 2.5). Search does not use the HNSW
    index. The label assignment keeps `semantic_score_elbow`.
12. **Sidecar residency is a separate plan.** Idle time, encoder loading, and prewarm are in the
    [AI Sidecar Runtime Residency Plan](../ai/ai_sidecar_runtime_residency_plan.md).

### 1.3 Rejected options

Do not add these items again:

- an action panel (Tab or Ctrl+K menu) for rows or photos;
- a result count or "x–y of z" text in the palette;
- debounced natural-language preview while the user types;
- rank fusion of field search and natural-language search;
- recursive subfolder scope;
- a minimum result count for natural-language search;
- a pill, chip, or badge for the active route or the active search (see the `alcedo-qml-ui`
  skill).

## 2. Product design specification

### 2.1 Palette layout

The palette is a modal panel at the center of the window, with the current blurred backdrop.

```text
+--------------------------------------------------------------------+
| [search icon]  mountain sunset      Natural language  [gear]  [x]  |
+--------------------------------------------------------------------+
| SEARCH IN                                                          |  <- settings drawer
| [x] File name   [x] EXIF info   [x] AI description   [x] AI tags   |     (open)
| ------------------------------------------------------------------ |
| Natural language search  [on]                                      |
| Search by meaning with the semantic model. Press Enter to run it.  |
+--------------------------------------------------------------------+
| COMMANDS                                                           |
|   [icon] Search photos for "mountain sunset"                 Enter |
|                                                                    |
| FILTERS                                                            |
|   [camera icon] Camera   Canon EOS R5                          128 |
|   [lens icon]   Lens     RF 24-70mm F2.8                        42 |
|                                                                    |
| PHOTOS                                                  View all > |
|   +-------------+  +-------------+  +-------------+                |
|   |  thumbnail  |  |  thumbnail  |  |  thumbnail  |                |
|   +-------------+  +-------------+  +-------------+                |
|   IMG_0412.CR3     IMG_0415.CR3     IMG_0533.CR3                    |
+--------------------------------------------------------------------+
| Enter  Search                                                      |
+--------------------------------------------------------------------+
```

- The palette width is `min(window width - 56, 960)`. The palette is horizontally centered. The
  palette height fits its content, up to `window height - 72`.
- The settings drawer opens and closes with the settings button in the input row. The drawer
  opens inside the palette, below the input row. The palette keeps its width and position.
- The close button `[x]` at the right end of the input row is an `IconActionButton` with
  `qrc:/panel_icons/close.svg`.
- The footer shows only the Enter / Search hint.
- All colors, spacing, radii, type sizes, and icon sizes come from `appTheme` and `DESIGN.md`.
  The palette uses the Basic style. It does not import `QtQuick.Controls.Material`.

### 2.2 Sections and rows

The palette shows these sections in this order. A section with no rows is hidden.

**Empty query**

| Section | Rows | Click result |
| --- | --- | --- |
| Recent searches | Up to 5 recent applied queries, newest first | Puts the query in the field and applies it to the Library |
| Filters | Up to 8 rows from `SearchRecommendations` | `StatsEngine::ToggleStatsFilter(category, label)`, then close |

**Query with text**

| Section | Rows | Click result |
| --- | --- | --- |
| Commands | One row: `Search photos for "<query>"`. For the natural-language route: `Search photos by meaning for "<query>"`. | Same as Enter |
| Filters | Up to 4 recommendation rows whose label contains the query (case-insensitive) | `ToggleStatsFilter(category, label)`, then close |
| Photos | Field route: up to 3 previews from the search preview request. Natural-language route: the text "Press Enter to search by meaning." | Preview click: `ApplyExactSearch(elementId)`, then close |

Row rules:

- A row shows an icon, a title, and an optional trailing value. A filter row shows the category
  name, the value, and the count as three separate labels. Do not join them with a separator
  character.
- The Commands row shows the text `Enter` at its right end.
- The Photos section header shows "View all" at its right end. "View all" does the same as
  Enter.
- A photo preview has a 4:3 thumbnail with `PreserveAspectCrop` and the file name below it. The
  file name elides in the middle. The thumbnail uses the `k512` tier.

Recent searches:

- `SearchController` owns the list. It stores at most 5 entries in `QSettings` key
  `search/recentQueries`.
- A query enters the list when the Library accepts it (`AcceptLibrarySearch`). Exact-file
  searches (photo click) do not enter the list.
- An entry stores the query text and the route (`fuzzy` or `semantic`). Applying an entry uses
  its stored route. The natural-language switch does not change a stored entry.
- A repeated query moves to the top. The list has no duplicates.

### 2.3 Routes, Enter, and focus

- The route of a query comes from `SearchSession::RouteFor`: `empty`, `fuzzy`, or `semantic`.
  The rule stays the same: `semantic` needs the natural-language switch and the classifier route
  `semantic`.
- The input row shows the text "Natural language" at its right side when the route is
  `semantic`. It is plain muted text. It is not a pill.
- Typing restarts a 140 ms debounce. After the debounce, a `fuzzy` query requests the preview
  rows. A `semantic` query requests nothing.
- Enter with an empty query does nothing.
- Enter with a `fuzzy` query calls `SearchController::ApplyFuzzySearch(query)` and closes the
  palette.
- Enter with a `semantic` query calls `SearchController::ApplySemanticSearch(query)`.
  - If the call returns an error text, the palette stays open and shows the text in the Photos
    section. The errors are: the query is too long, no semantic model is active, and no semantic
    search provider exists.
  - If the call returns an empty text, the palette closes. The Library runs the search.
- Up and Down move the row highlight across the Commands and Filters rows. The mouse hover moves
  the same highlight. Enter runs the highlighted row. The Commands row is highlighted when the
  query changes.
- Escape, a click outside the palette, and the close button in the input row close it.
- The text field has focus when the palette opens.

### 2.4 Settings drawer

- The drawer is below the input row, above the sections. It folds with the shared fold motion
  (`CollapsibleSection` and DESIGN.md Motion: clip, height, and opacity; `reduceMotion` snaps).
- Group "Search in": check boxes "File name", "EXIF info", "AI description", "AI tags", in one
  row. They call the existing `SetSearchField*Enabled` operations.
- A divider, then the switch "Natural language search". It calls
  `SetNaturalLanguageSearchEnabled` and updates `InteractionPolicyController`.
- When the switch is on, the check boxes are disabled. They show the `ThemeCheckBox` disabled
  state: a muted box, a muted check mark, and a muted label. The drawer shows no reason text.
- A change re-runs the preview of the current query.
- The open state persists in `QSettings` key `search/settingsPanelOpen`. Default: closed.
- Controls use `ThemeCheckBox.qml` and `ThemeSwitch.qml`. The drawer does not use Material
  controls.

### 2.5 Natural-language relevance rule

Definitions:

- **Model**: the active semantic model of the project, with key `model_key`.
- **Query embedding** `q`: the text embedding of the trimmed query text for the model.
- **Neutral embedding** `n`: the text embedding of the fixed text `a photo` for the model.
- **Scored file**: a direct child file of the scope folder that has a `ready` image embedding
  `x` for the model. A file without such an embedding is never a natural-language result.
- **Query similarity** `c = array_cosine_similarity(x, q)`.
- **Neutral similarity** `b = array_cosine_similarity(x, n)`.
- **Relevance** `r = c - b`.
- **Scope statistics**: `N` is the number of scored files. `mu` and `sigma` are the mean and the
  population standard deviation of `r` over the scored files.

Rule:

1. Compute `c`, `b`, and `r` for each scored file in one exact scan of the scope.
2. Keep a file when `c >= kMinQuerySimilarity` and one of these is true:
   - `N < kMinScopeStatisticsCount`;
   - `sigma > 0` and `(r - mu) / sigma >= kMinRelevanceZ`.
3. Order the kept files by `r` descending, then by file id ascending.

Constants (owned by `SemanticVectorSearch`):

| Constant | Initial value | Value after the Phase GS1 calibration |
| --- | --- | --- |
| `kMinRelevanceZ` | 2.0 | 1.0 |
| `kMinQuerySimilarity` | none | 0.0855 (SigLIP2 embeddings) |
| `kMinScopeStatisticsCount` | 30 | 30 (fixed) |
| neutral text | `a photo` | `a photo` (fixed) |

The SQL computes `c` and `b` as cosine similarities from inner products: the text vectors are
scaled to unit length, `|x| = sqrt(x . x)`, `c = x . q / |x|`, and `r = x . (q - n) / |x|`.
The result does not depend on the length of the stored vectors.

The rule is deterministic for one scope, one model, and one query. Each page and each Library
read gives the same order. A different page size does not change the kept set.

### 2.6 Library result view

- After `ApplySemanticSearch`, the Library shows the kept files of the current folder.
- Without a selected photo sort, the photo order is relevance descending, then file id
  ascending. This applies in flat mode and inside each group. The group order does not change.
- A selected photo sort replaces the relevance order. The kept set does not change.
- Inspector statistics, select all, the file position read, and the page reads use the same kept
  set.
- The Inspector search filter card shows the query text. For a natural-language search it also
  shows the line "Natural language" below the query, as a separate label. The existing clear
  button clears the search.
- Selecting a folder clears the active search and the Inspector filters, for field search and
  for natural-language search alike (`FolderController::SelectFolder`, current behavior). A
  folder change that keeps the search (Delete Folder moves the selection to the parent folder)
  evaluates it for the new folder from the accepted embedding, with no new embedding. Decision
  of 2026-10-17: the source audit of this plan had the folder rule wrong, and the current clear
  stays.
- If the embedding or the ranking fails, the Library keeps its accepted content. It shows the
  existing error row "Library query failed: <reason>" with Retry. The search does not become
  active.

### 2.7 Palette states

| State | Display |
| --- | --- |
| Empty query, no recent searches, no recommendations | Text "Type to search photos" |
| Field query, preview loading | Photos section shows three placeholder frames |
| Field query, no preview rows | Photos section shows "No matching photos in this folder" |
| Field query, preview error | Photos section shows the error text from `SearchSession` |
| Semantic query | Photos section shows "Press Enter to search by meaning." |
| Semantic Enter rejected | Photos section shows the returned error text |
| Thumbnail missing source | The preview frame shows the current "!" mark with the tooltip |

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility |
| --- | --- |
| `SemanticVectorSearch` | Owns the relevance rule and its SQL. Exact scan of the scope. No HNSW index. |
| `SemanticQueryEmbeddingCache` (proposed) | Least-recently-used cache of text embeddings per model and text. |
| `ProjectSemanticSearchProvider` | Embeds the query and the neutral text through the cache and the sidecar. |
| `SleeveFilterService` | Exposes the query embedding and the relevance select to the Library path. |
| `ElementStore` and `AlbumBrowseService` | Join the relevance select into the match set and order by relevance. |
| `SearchController` and `LibraryModule` | Apply a natural-language search to the Library; keep the accepted embedding; keep recent searches. |
| `SearchSession` | Field preview of up to three rows; `Replace` for `search.query`. No sliding window. |
| `SearchPalette.qml` (proposed) | The new palette. It replaces `GlobalSearchDialog.qml`. |
| `AlbumInspectorPanel.qml` | The "Natural language" line in the search filter card. |
| `project_package_backend` | Drops an HNSW index of an older project when it opens. |

### 3.2 Exclusions

- Sidecar idle time, lazy encoder loading, and prewarm. The
  [AI Sidecar Runtime Residency Plan](../ai/ai_sidecar_runtime_residency_plan.md) owns them.
- Ranking of field search results. Field preview rows keep the file id order of
  `ListSearchResultPage`.
- A keyboard shortcut to open the palette. The
  [Configurable Keyboard Shortcut Registry Plan](configurable_keyboard_shortcut_registry_plan.md)
  owns shortcuts.
- An automation command that applies a search to the Library.
- Removal of the packaged DuckDB `vss` extension. Older projects need it to drop their HNSW
  index.

## 4. Current source audit

All paths are under `alcedo_studio/src` unless stated otherwise.

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Vector ranking | `SemanticVectorSearch::SearchImageEmbeddings`, `storage/store/semantic/semantic_vector_search.cpp:64-147` | HNSW top-K (`max(512, offset + limit)`) over the whole table with `array_distance`, then a join to the folder scope, then `semantic_score_elbow::CutoffScore`, then paging in C++. | Exact scan of the scope with the relevance rule. Paging in SQL. |
| Score cutoff | `semantic_score_elbow::CutoffScore`, `storage/store/semantic/semantic_score_elbow.cpp:21-58` | A flat span keeps the whole sample. Without a clear elbow it keeps the top 35 % of the span. No absolute limit. | Search stops using it. Label assignment keeps it. |
| Folder recall | Same file, `nearest` CTE then `JOIN scoped` | The folder filter runs after the global top-K. A small folder can lose matches that rank below 512 in the whole project. | The scope filter runs before the score. |
| Page stability | Same file, `RequestedCandidateLimit` and `FilterAndPageCandidates` | The cutoff is recomputed on each page from a candidate pool whose size depends on the offset. Pages after 512 rows can repeat or skip rows. | One rule for all pages. |
| HNSW index | `SemanticVectorSearch::EnsureVectorSearchIndex` (same file, :149-182); `project_pack::EnsureVssExtensionForExistingHnswIndexes`, `app/project_package_backend.cpp:482`, called at `app/project_service.cpp:607` | Search creates the index. Project open loads `vss` when an index exists. | Search does not create it. Project open drops an existing index. |
| Query embedding | `ProjectSemanticSearchProvider::Search`, `app/project_service.cpp:167-275` | Embeds the text on every call, including every page. No cache. | Cache by model key and text. Embed the neutral text once per model. |
| Semantic page total | `RunSearchPageRequest`, `ui/alcedo_main/album_backend/search_controller.cpp:164-180` | `total = offset + rows + 1` when a page is full. | The relevance query returns the real total for `search.query`. |
| Library apply | `RunLibraryQuery`, `ui/alcedo_main/album_backend/library_module.cpp:447-468`; `SearchController::ApplyFuzzySearch`, `search_controller.cpp:407-418` | Only `BuildFuzzySearchWhere` (SQL) and the exact file filter. No natural-language apply. | Add the natural-language apply path. |
| Library order | `AlbumQueryOptions`, `include/sleeve/album_query.hpp:66-74`; `storage/store/sleeve/album_query.cpp` | No relevance order. Without a sort the order is file id. | Relevance order when a natural-language search is active and no sort is selected. |
| Match set | `MatchSetTable`, `storage/store/sleeve/element_store.cpp:141-169`; used by `ReadAlbumQuery` (:804), `ReadAlbumFilePosition` (:838), `ReadAlbumFileIds` (:903), `BuildFolderStats` (:782) | One TEMP table per operation from the scope and the filter. No relevance column. | Optional join to the relevance select and a `relevance` column. |
| Palette window | `SearchSession`, `include/ui/alcedo_main/album_backend/search_session.hpp:48-139` | Page size 24, window 48, `LoadMore`, `LoadPrevious`, prepend and append. | Preview of 3 rows. `Replace` stays for `search.query`. |
| Palette preview thumbnails | `SearchController::SetSearchPreviewThumbnailVisible`, `search_controller.cpp:501-713` | `image://alcedo-thumb/...` URLs in the shared `ThumbnailImageStore`. Pins through `ThumbnailService`. Tier from `maxEdge`. The names `dataUrl` and `thumb_data_url` are from an older design. | Keep the path. Use `maxEdge` 512. Rename `dataUrl` to `thumbUrl` in touched signatures. |
| Recommendations | `StatsEngine::BuildSearchRecommendations`, `ui/alcedo_main/album_backend/stats_engine.cpp:189-200` | Rows `{category, categoryLabel, label, query, count}` for camera, date, lens, label. The palette applies a row as fuzzy text. | Apply a row with `ToggleStatsFilter(category, label)`. |
| Palette | `qml/GlobalSearchDialog.qml` (1240 lines) | Full list with paging, settings popup, Material controls, Search button. | Replace (Section 2). |
| Active search display | `qml/AlbumInspectorPanel.qml:244-330` | "SEARCH FILTER" card with the query and a clear button. | Add the "Natural language" line. |
| `search.query` | `ui/alcedo_main/automation/commands/search_commands.cpp:60-150` | Calls `SearchSession::Replace`; limit maximum `kWindowCapacity`; result has `total` and `has_more`. | Same fields. The limit maximum becomes `SearchSession::kMaxPageRows` (48). |

Current tests (target names in `alcedo_studio/tests`):

- `SemanticStorageControllerTest` (`storage/semantic_storage_controller_test.cpp`):
  `VssSearchRanksWithinRootAndFolderScope`, `PackedSnapshotSucceedsAfterVectorSearchIndex`,
  `VssSearchCutsOffWeakTailBeforePaging`, `Supports768DimensionalModelStorageAndSearch`,
  `AssignsElbowTruncatedLabelsByScoreDistribution`.
- `FilterServiceTest`: `SemanticSearchProviderDelegatesWhenRegistered`,
  `LabelQueryUsesOrdinaryPathNotSemanticProvider`, `StatsBarAndSearchMergeUnderOneCompiledPredicate`.
- `SearchSessionTest`: `ResponseWithOldRequestIdIsIgnored`, `AppendBeyondCapacityDropsLeadingRows`,
  `SemanticQueryWaitsForSubmitWhenNaturalLanguageSearchIsOn`.
- `AlbumBackendSearchWorkerTest`: five tests, including
  `ApplyFuzzySearchQueriesOnTheWorkerAndKeepsInspectorFilters` and `SearchSqlNeverRunsOnTheUiThread`.
- `GlobalSearchDialogQmlTest`: six tests, including
  `ScrolledSearchResultsRenderPreviewThumbnailsAndReopenStillWorks`.
- `LibrarySearchColumnsTest`, `LibrarySearchRecallTest`, `LibrarySearchBenchmarkTest` (disabled
  benchmarks).
- `AutomationSearchE2ETest`: `FuzzySearchFindsImportedFileName`.

## 5. Target architecture

### 5.1 Owners

| Owner | Input | Output | Changes | Reads only | Lifetime | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| `SemanticVectorSearch` | scope folder id, `SemanticQueryEmbedding` | relevance select (`SqlFragment`), ranked page | nothing | embedding tables | per `Storage` | `std::runtime_error` with the DuckDB message; model not registered; dimension mismatch |
| `SemanticQueryEmbeddingCache` (proposed) | model key, text, encoder callback | embedding vector | its entries | nothing | per `ProjectSemanticSearchProvider` | the encoder error, passed through; no entry on failure |
| `ProjectSemanticSearchProvider` | query text | `SemanticQueryEmbedding` (query and neutral vectors, model key, dimension) | cache entries; sidecar lease while it embeds | active model, runtime status | per project | runtime start failure, model mismatch, embedding validation failure |
| `SleeveFilterService` | query text, folder | `SemanticQueryEmbedding`; relevance select; result rows | nothing | provider, storage | per project | provider missing; provider error |
| `ElementStore` | scope, filter, relevance select, options | album page, groups, statistics, ids, position | its TEMP match set | element, image, embedding tables | per operation | DuckDB error |
| `SearchController` | palette and automation calls | pending and accepted `LibrarySearchInput`; recent searches | accepted search state; `QSettings` keys | classifier, provider presence | per application module host | synchronous rejection text from `ApplySemanticSearch` |
| `LibraryModule` | `LibraryQueryInput` | published library state | accepted content | search input | per application module host | `queryError` with Retry |

### 5.2 New value type

`SemanticQueryEmbedding` holds `model_key_`, `dimension_`, `query_` (`std::vector<float>`), and
`neutral_` (`std::vector<float>`). It is the output of an embedding operation, not a copy of
an owner's state. The Library keeps the accepted value in the accepted search state, so later
page reads, statistics, and select all do not call the sidecar again. The accepted search state
releases it when the search is cleared or replaced. No code writes it back to an owner.

### 5.3 Primary success call chain (natural-language apply)

```text
Search palette Enter
  -> SearchController::ApplySemanticSearch(query)        [GUI thread]
       classify, too-long check, provider check -> "" (accepted)
       pending_search_ = {display_query_, semantic_text_}
       LibraryModule::RequestLibraryRefresh()
  -> SearchRequestWorker kApply -> RunLibraryQuery       [query worker]
       SleeveFilterService::EmbedSemanticQuery(text)
         -> ProjectSemanticSearchProvider::EmbedQuery
              cache hit, or lease + StartAndWait + EmbedText (query, then neutral once)
       SemanticVectorSearch::BuildRelevanceSelect(folder, embedding)
       AlbumBrowseService::ReadAlbumQuery(folder, stats filter, options, model, read, relevance)
         -> ElementStore::ReadAlbumQuery
              MatchSetTable(scope JOIN relevance select) -> page ordered by relevance
  -> LibraryModule::PublishRefresh                        [GUI thread]
       SearchController::AcceptLibrarySearch(input, output) stores the embedding
       recent searches updated; SearchStateChanged; grid and Inspector update
```

### 5.4 Primary failure chain (natural-language apply)

```text
RunLibraryQuery: EmbedQuery throws (runtime start failed / model mismatch / bad vector)
  -> output.error_ = message
  -> LibraryModule::PublishRefresh: SetQueryError(message); accepted content stays;
     the pending search stays for Retry; AcceptLibrarySearch is not called
  -> LibraryWorkspace shows "Library query failed: <message>" with Retry
```

No other path runs. The search does not change to field search, and it does not use a stored
older embedding of another model.

## 6. File and API map

### 6.1 Current files that change

| File | Phase | Change |
| --- | --- | --- |
| `include/storage/store/semantic/semantic_vector_search.hpp`, `storage/store/semantic/semantic_vector_search.cpp` | GS1 | Relevance rule; exact scan; remove `EnsureVectorSearchIndex` |
| `include/storage/store/semantic/semantic_records.hpp` | GS1 | `SemanticQueryEmbedding` |
| `storage/store/semantic/semantic_tables.hpp` and `.cpp` | GS1 | Remove `ImageEmbeddingIndex` when no caller is left |
| `app/project_service.cpp` | GS1 | Provider uses the cache and `EmbedQuery`; HNSW drop at open |
| `include/app/project_package_backend.hpp`, `app/project_package_backend.cpp` | GS1 | `DropHnswIndexesOfOlderProject` replaces `EnsureVssExtensionForExistingHnswIndexes` |
| `include/app/sleeve_filter_service.hpp`, `app/sleeve_filter_service.cpp` | GS1, GS2 | `SemanticSearchProvider::EmbedQuery`; `EmbedSemanticQuery`; `BuildSemanticRelevanceSelect` |
| `include/storage/store/sleeve/element_store.hpp`, `storage/store/sleeve/element_store.cpp`, `storage/store/sleeve/album_query.cpp`, `include/storage/store/sleeve/album_query_sql.hpp` | GS2 | Relevance join and order |
| `include/sleeve/album_query.hpp` | GS2 | Order rule documentation for relevance |
| `include/app/album_browse_service.hpp`, `app/album_browse_service.cpp` | GS2 | Pass the relevance select |
| `include/ui/alcedo_main/album_backend/search_controller.hpp`, `.../search_controller.cpp` | GS2, GS3 | `ApplySemanticSearch`; accepted embedding; recent searches; recommendation apply |
| `include/ui/alcedo_main/album_backend/search_session.hpp`, `.../search_session.cpp` | GS2 | Remove the window; preview of 3 rows |
| `include/ui/alcedo_main/album_backend/library_module.hpp`, `.../library_module.cpp` | GS2 | Semantic branch in `RunLibraryQuery` and page reads |
| `ui/alcedo_main/album_backend/stats_engine.cpp` | GS2 | Statistics with the relevance select |
| `ui/alcedo_main/automation/commands/search_commands.cpp` | GS2 | New limit constant; new `ResponseApplied` signature |
| `ui/alcedo_main/qml/GlobalSearchDialog.qml` | GS2, GS3, GS5 | Minimal compile change (GS2); recommendation apply (GS3); deleted (GS5) |
| `ui/alcedo_main/qml/AppDialogs.qml`, `ui/alcedo_main/CMakeLists.txt` | GS4, GS5 | Instantiate and register `SearchPalette`; remove the old file entry |
| `ui/alcedo_main/qml/AlbumInspectorPanel.qml` | GS2 | "Natural language" line |
| `ui/alcedo_main/DESIGN.md`, AppTheme sources | GS4 | New palette tokens, if a value is missing |

### 6.2 Proposed files

| File | Phase | Responsibility |
| --- | --- | --- |
| `include/app/semantic_query_embedding_cache.hpp`, `app/semantic_query_embedding_cache.cpp` | GS1 | LRU cache of text embeddings |
| `alcedo_studio/tests/app/semantic_query_embedding_cache_test.cpp` | GS1 | Cache tests, target `SemanticQueryEmbeddingCacheTest` registered in `alcedo_studio/tests/app/CMakeLists.txt` |
| `ui/alcedo_main/qml/SearchPalette.qml` | GS4 | The new search palette |

### 6.3 Proposed APIs

| API | Input | Output | Owner and lifetime | Validation | Error | Changes state |
| --- | --- | --- | --- | --- | --- | --- |
| `SemanticVectorSearch::BuildRelevanceSelect(folder_id, const SemanticQueryEmbedding&)` | folder id, embedding | `duckorm::SqlFragment` selecting `file_id, image_id, relevance` of kept files | `SemanticVectorSearch`; the fragment is valid for one statement | model registered; vector dimension equals model dimension; finite values | `std::runtime_error` | No |
| `SemanticVectorSearch::SearchImageEmbeddings(folder_id, const SemanticQueryEmbedding&, offset, limit, error)` | as above, page | `SemanticRankedPage {rows_, total_}` | `SemanticVectorSearch` | as above | `error` text, empty page | No |
| `SemanticQueryEmbeddingCache::GetOrEmbed(model_key, text, encode)` | model key, trimmed text, `std::function<std::vector<float>()>` | `std::vector<float>` | owned by the provider; capacity 64 | none (the provider validates the vector) | exception of `encode`; no entry is stored | Yes (entries) |
| `SemanticSearchProvider::EmbedQuery(query)` | query text | `SemanticQueryEmbedding` | provider | provider validates both vectors | `std::runtime_error` | Cache only |
| `SleeveFilterService::EmbedSemanticQuery(query)` | query text | `SemanticQueryEmbedding` | filter service | provider present | `std::runtime_error` | No |
| `SleeveFilterService::BuildSemanticRelevanceSelect(folder_id, embedding)` | folder, embedding | `duckorm::SqlFragment` | filter service | delegates to storage | `std::runtime_error` | No |
| `SearchController::ApplySemanticSearch(query)` (`Q_INVOKABLE`) | query | `QString` error, empty on acceptance | GUI thread | route, length, provider | returned text | Yes (pending search) |
| `SearchController::RecentSearches()` (`Q_PROPERTY QVariantList recentSearches`) | none | list of `{query, route}` | `SearchController` | none | none | No |
| `SearchController::ApplyRecentSearch(index)` (`Q_INVOKABLE`) | list index | `QString` error | GUI thread | index range | returned text | Yes |

Each new header includes the headers that define its types. Do not add forward declarations.

## 7. Repository rules that affect this work

Read `AGENTS.md` and the applicable skills again before each phase. These rules apply directly:

- No fallback without authorization. A natural-language failure reports the real error. It does
  not run field search in its place.
- Update data through its owner. `SemanticQueryEmbedding` is an operation result (Section 5.2).
  Do not copy the match set or the search session state for convenience.
- Do not add a generation, token, or similar mechanism without an executable interleaving. The
  existing `SearchRequestWorker` latest-wins rule and `SearchSession` request id stay. This plan
  adds no new consistency mechanism.
- DuckDB connections come from `DBController` at the operation boundary.
- QML: Basic style only; `appTheme` tokens; no compound `xx · xx` labels; no new pills, badges,
  or status dots; register new QML files in `ALCEDO_MAIN_QML_FILES` (`alcedo-qml-ui` skill).
- Terminology: do not use the prohibited terms of `AGENTS.md` in code, tests, or text.
- Tests: use the debug build; do not run the full suite; do not run tests in parallel. Offscreen
  QML tests are not reliable. Verify behavior through the C++ owner tests and report skipped QML
  coverage.
- MSVC builds run through the PowerShell tool.
- LF line endings only.

## 8. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| GS1 | Calibrated relevance over the folder scope; query embedding cache; no HNSW | `SemanticVectorSearch`, provider, cache, package backend | none | 900–1400 | complete (2026-10-17) |
| GS2 | Natural-language results in the Library in relevance order; simpler `SearchSession` | `ElementStore`, `LibraryModule`, `SearchController`, `SearchSession`, Inspector card | GS1 | 1100–1700 | complete (2026-10-17) |
| GS3 | Recent searches, recommendation apply, and `thumbUrl` in `SearchController` | `SearchController` | GS2 | 400–700 | complete (2026-10-17) |
| GS4 | New search palette with previews and the settings side panel | `SearchPalette.qml`, `AppDialogs.qml` | GS3 | 900–1300 | complete (2026-10-17), manual check open |
| GS5 | Remove the old search dialog | `GlobalSearchDialog.qml`, CMake | GS4 | about 1250 removed lines (file deletion) | complete (2026-10-17) |

## 9. Detailed phases

### Phase GS1 — Calibrated relevance over the folder scope

Issue: [#420](https://github.com/zidage/AlcedoStudio/issues/420).

**Objective and deliverables**

- Natural-language search uses the relevance rule of Section 2.5 with an exact scan of the
  folder scope.
- A query that matches no photo clearly returns no rows.
- The query and the neutral text are embedded once per model and text.
- Search does not create an HNSW index. An existing HNSW index is dropped at project open.
- `search.query` returns the real total for a natural-language query.

**Inputs and prerequisites**

- Source at or after `ada6f002c`.
- A local project with generated embeddings for the calibration step (Step 9).

**Modules, files, and APIs**

- `SemanticVectorSearch`: `BuildRelevanceSelect`, new `SearchImageEmbeddings` signature,
  constants of Section 2.5. Remove `EnsureVectorSearchIndex`, `RequestedCandidateLimit`,
  `FilterAndPageCandidates`, and `kMinCandidatePool`.
- `SemanticQueryEmbeddingCache` (proposed).
- `SemanticSearchProvider`: add `EmbedQuery`. `Search` returns `SemanticRankedPage`-based rows.
- `ProjectSemanticSearchProvider` in `app/project_service.cpp`.
- `project_pack::DropHnswIndexesOfOlderProject` in `app/project_package_backend.cpp`.
- `RunSearchPageRequest` in `search_controller.cpp`: real total for the semantic route.

**Data rules and invariants**

- Embeddings in storage and from the sidecar are L2-normalized. The relevance SQL uses
  `array_cosine_similarity`, so the rule does not depend on that normalization.
- The relevance select reads only `ready` rows of the active model with the model dimension.
- The scan computes the statistics over the scored files of the scope only.
- The order key is `(relevance DESC, file_id ASC)`. The total is the kept count.
- The cache key is `(model_key, trimmed query text)`. The text is not lowercased. The capacity
  is 64 entries. A hit moves the entry to the front. An insert at capacity removes the least
  recently used entry. A failed encode stores nothing.
- The neutral text is a normal cache entry.
- The provider holds the sidecar lease only while it embeds. A cache hit takes no lease.

**Implementation steps**

1. Add `SemanticQueryEmbedding` to `semantic_records.hpp`. Document its purpose and lifetime at
   the type (Section 5.2).
2. Add `SemanticQueryEmbeddingCache` with `GetOrEmbed`. Protect the entries with one
   `std::mutex`. Reason: the query worker calls it, and the residency plan adds a prewarm call
   from another thread. Hold the mutex only for the lookup and the insert, not during `encode`.
3. Add `SemanticSearchProvider::EmbedQuery`. In `ProjectSemanticSearchProvider`, move the
   runtime start, model check, and `EmbedText` into one private `EncodeText(text)`. `EmbedQuery`
   calls the cache for the query and for `a photo`. It validates both vectors with
   `ValidateSearchEmbedding`.
4. Write `BuildRelevanceSelect`. Use one statement with these CTEs: `scoped` (from
   `BuildScopedFileQuery(folder_id)`), `scored` (join of `scoped` and the embedding table; `c`,
   `b`, `r`), `stats` (`count`, `avg`, `stddev_pop` of `r`), and the final select with the keep
   rule. Pass both vectors as fixed-size array literals, as the current code does.
5. Rewrite `SearchImageEmbeddings` to run `BuildRelevanceSelect` with
   `ORDER BY relevance DESC, file_id ASC LIMIT ? OFFSET ?` and `COUNT(*) OVER ()` for the total.
   Return `SemanticRankedPage`.
6. Update `ProjectSemanticSearchProvider::Search` and `SleeveFilterService::SearchFolderSemanticRows`
   to the new page type. Set `response["total"]` and `response["hasMore"]` from the real total
   in `RunSearchPageRequest`.
7. Remove `EnsureVectorSearchIndex` and its callers. Remove `ImageEmbeddingIndex` when it has no
   caller left. Keep `semantic_score_elbow` for label assignment. Update the header comment of
   `semantic_score_elbow.hpp` so it names only label assignment.
8. Replace `EnsureVssExtensionForExistingHnswIndexes` with `DropHnswIndexesOfOlderProject`. It
   reads `duckdb_indexes()` for an HNSW index. If one exists, it loads the packaged `vss`
   extension and runs `DROP INDEX`. If the load or the drop fails, project open fails with that
   error. Call it from `LoadProject` at the current call site (`project_service.cpp:607`).
9. Calibration. Add the test `LiveRelevanceKeepsListedPhotosAndRejectsNonsenseQueries`.
   It reads `ALCEDO_SEMANTIC_CALIBRATION_PROJECT` (an `.alcd` file) and
   `ALCEDO_SEMANTIC_CALIBRATION_QUERIES` (a JSON file with positive queries and their expected
   file names, and nonsense queries). The maintainer provides both files. The test needs the
   release `alcedo_mind.exe` and the model files, as the current live runtime tests do. It prints `c`,
   `r`, and the z value of each expected file and of the top 20 files of each query. Choose
   `kMinQuerySimilarity` and `kMinRelevanceZ` so that every nonsense query keeps zero files and
   each positive query keeps all its expected files. Record the values and the printed table in
   the completion record. If no pair of values meets both conditions, stop and update this plan.
10. Add the benchmark `DISABLED_RelevanceSelectOver20000Embeddings` to
    `LibrarySearchBenchmarkTest`. It uses 20 000 synthetic 512-dimensional embeddings in one
    folder.

**Primary success call chain**

```text
SearchSession::Replace (search.query, semantic) -> RunSearchPageRequest [worker]
  -> SleeveFilterService::SearchFolderSemanticRows
  -> ProjectSemanticSearchProvider::Search
       EmbedQuery -> cache (query), cache (a photo)
       SemanticVectorSearch::SearchImageEmbeddings -> BuildRelevanceSelect -> page + total
  -> ElementStore::ListSearchResultRows(file ids in rank order)
  -> response {rows, total, hasMore}
```

**Primary failure and restore call chain**

```text
EncodeText: runtime start fails -> std::runtime_error -> no cache entry
  -> RunSearchPageRequest sets semanticUnavailable + semanticErrorText
  -> search.query returns semantic_unavailable = true and semantic_error = message
```

**Tests and evidence**

| Test (target) | Assertion |
| --- | --- |
| `RelevanceKeepsOnlyFilesAboveTheZLimit` (`SemanticStorageControllerTest`) | 100 synthetic files, 5 aligned with `q`: the result is exactly those 5 in descending relevance. |
| `UnrelatedQueryKeepsNoFiles` (`SemanticStorageControllerTest`) | A query vector orthogonal to all files keeps zero files and total 0. |
| `NeutralSimilarityDemotesFilesThatMatchEveryText` (`SemanticStorageControllerTest`) | A file with high `c` and equally high `b` ranks below a file with lower `c` and low `b`. |
| `FolderScopeKeepsChildrenThatRankLowInTheProject` (`SemanticStorageControllerTest`) | 600 strongly aligned files outside the folder and 3 aligned files inside: a folder search returns the 3. This is the regression of the HNSW post-filter. |
| `SmallScopeUsesOnlyTheSimilarityLimit` (`SemanticStorageControllerTest`) | With 10 scored files, the kept set equals the files with `c >= kMinQuerySimilarity`. |
| `PagesOfOneQueryJoinToTheFullOrderedResult` (`SemanticStorageControllerTest`) | Pages of size 7 from offset 0 to the end, joined, equal the single page of size 1000. No repeat and no gap. |
| `FilesWithoutReadyEmbeddingAreNotResults` (`SemanticStorageControllerTest`) | A file with a `pending` row is never returned. |
| `Supports768DimensionalModelStorageAndSearch` (updated) | The relevance select works for a 768-dimensional model. |
| `OlderProjectHnswIndexIsDroppedOnOpen` (`SemanticStorageControllerTest` or project service test) | A project file with an HNSW index opens, the index is gone, and a search returns the expected rows. |
| `CachedTextIsNotEncodedAgain` (`SemanticQueryEmbeddingCacheTest`) | The second `GetOrEmbed` of the same key does not call `encode`. |
| `CacheKeyIncludesTheModelKey` (`SemanticQueryEmbeddingCacheTest`) | Same text, two model keys: `encode` runs twice. |
| `LeastRecentlyUsedEntryIsRemovedAtCapacity` (`SemanticQueryEmbeddingCacheTest`) | After 65 inserts and one hit on entry 1, entry 2 is encoded again and entry 1 is not. |
| `FailedEncodeStoresNoEntry` (`SemanticQueryEmbeddingCacheTest`) | After a throwing `encode`, the next call calls `encode` again. |
| `SemanticSearchProviderDelegatesWhenRegistered` (`FilterServiceTest`, updated) | The fake provider receives `EmbedQuery` and the rows keep the rank order. |
| `LiveRelevanceKeepsListedPhotosAndRejectsNonsenseQueries` (runs only when the two environment variables are set) | Step 9. Reported as skipped when the variables are absent. |
| `DISABLED_RelevanceSelectOver20000Embeddings` (`LibrarySearchBenchmarkTest`, release) | p50 at or below 80 ms. |

Remove or rename: `VssSearchRanksWithinRootAndFolderScope` (becomes
`RelevanceRanksWithinRootAndFolderScope`), `VssSearchCutsOffWeakTailBeforePaging` (replaced by
`RelevanceKeepsOnlyFilesAboveTheZLimit`), `PackedSnapshotSucceedsAfterVectorSearchIndex`
(becomes `PackedSnapshotSucceedsAfterRelevanceSearch`).

**Build and run commands**

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target SemanticStorageControllerTest SemanticQueryEmbeddingCacheTest FilterServiceTest AlbumBackendSearchWorkerTest
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "SemanticStorageControllerTest|SemanticQueryEmbeddingCacheTest|FilterServiceTest|AlbumBackendSearchWorkerTest|SearchSessionTest" --output-on-failure
```

Release benchmark: reconfigure `build/release` with `-DALCEDO_BUILD_TESTS=ON`, build only
`LibrarySearchBenchmarkTest`, run it with `--gtest_also_run_disabled_tests
--gtest_filter=*RelevanceSelect*`, then set the option back to `OFF`. Put logs under
`build/tmp/global_search_gs1/`.

**Exit criteria**

- [x] All named tests pass, except tests that need environment variables. Report each of them as run or skipped.
- [x] The calibration record has the chosen constants and the printed table.
- [x] The release benchmark p50 is at or below 80 ms.
- [x] No source file calls `EnsureVectorSearchIndex` or `CREATE INDEX ... USING HNSW`.
- [x] `search.query` with a nonsense query returns `matches: []` and `total: 0`.

**Expected diff**

900–1400 lines (storage and provider 400–600, cache 150, tests 350–600).

**Completion record**

```text
Phase / date / status: GS1 / 2026-10-17 / complete.
Source revision and branch: main at f3dc848b7; branch fix/semantic-search-calibrated-relevance.
Actual changed modules: SemanticVectorSearch (+ semantic_records, semantic_tables,
  semantic_score_elbow comment); SemanticQueryEmbeddingCache (new library);
  SemanticSearchProvider / ProjectSemanticSearchProvider; SleeveFilterService;
  project_pack::DropHnswIndexesOfOlderProject; SearchController RunSearchPageRequest.
Implemented behavior:
  - Relevance rule of Section 2.5 in one SQL statement; the order is (relevance DESC,
    file_id ASC); paging and the total come from that statement (COUNT(*) OVER ()).
  - Query and neutral text embedded once per (model key, trimmed text); LRU of 64 entries.
  - No HNSW index is created; LoadProject drops an existing one and fails when it cannot.
  - search.query reports the real total and has_more for natural-language queries.
API differences from Sections 5 and 6 (recorded, not hidden):
  - SemanticSearchProvider has only EmbedQuery. Search was removed from the provider:
    SleeveFilterService::SearchFolderSemantic calls EmbedSemanticQuery and then
    SemanticVectorSearch::SearchImageEmbeddings. GS2 uses the same split (embedding from
    the provider, relevance select from storage).
  - SearchFolderSemanticRows returns SearchResultPage {total_, rows_}.
  - SemanticVectorSearch::ReadRelevanceScores (new) returns c, b, r, z, and kept for every
    scored file. The calibration test uses it.
  - SleeveFilterService::BuildSemanticRelevanceSelect is not added; GS2 adds it with its
    first caller.
Explicitly unimplemented items: none of GS1. GS2 to GS5 are not started.
Primary success call chain:
  search.query (semantic) -> SearchSession::Replace -> RunSearchPageRequest [worker]
    -> SleeveFilterService::SearchFolderSemanticRows -> SearchFolderSemantic
    -> EmbedSemanticQuery (trim) -> ProjectSemanticSearchProvider::EmbedQuery
         cache(model, query) / cache(model, "a photo"); miss -> EncodeText:
         lease + StartAndWait when needed + model check + EmbedText + ValidateSearchEmbedding
    -> SemanticVectorSearch::SearchImageEmbeddings -> BuildRelevanceSelect (one statement)
    -> ElementStore::ListSearchResultRows(file ids in rank order)
    -> response {rows, total, hasMore}
Primary failure and restore call chain:
  EncodeText: runtime start fails / model mismatch / invalid vector -> std::runtime_error
    -> SemanticQueryEmbeddingCache stores no entry
    -> RunSearchPageRequest sets semanticUnavailable + semanticErrorText
    -> search.query returns semantic_unavailable = true and semantic_error = message.
  LoadProject: HNSW read, vss load, or DROP INDEX fails -> std::runtime_error -> the project
    does not open (no partially opened project).
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
    SemanticStorageControllerTest SemanticQueryEmbeddingCacheTest FilterServiceTest
    SemanticRelevanceCalibrationTest AlbumBackendSearchWorkerTest SearchSessionTest
    LibrarySearchColumnsTest LibrarySearchBenchmarkTest AiSidecarRuntimeServiceTest
    alcedo_main                                                              -> 0
  ctest --test-dir build/debug -R "SemanticStorageControllerTest|SemanticQueryEmbeddingCacheTest|
    FilterServiceTest|SemanticRelevanceCalibrationTest|AlbumBackendSearchWorkerTest|
    SearchSessionTest|LibrarySearchColumnsTest|LibrarySearchBenchmarkTest|
    AiSidecarRuntimeServiceTest" --output-on-failure                         -> 0
Discovered / passed / failed / skipped counts: 120 discovered; 116 passed; 0 failed;
  2 skipped (LiveRelevanceKeepsListedPhotosAndRejectsNonsenseQueries and the live runtime
  test, no environment in ctest); 4 disabled benchmarks not run by ctest.
  Named tests, all passed: RelevanceKeepsOnlyFilesAboveTheZLimit, UnrelatedQueryKeepsNoFiles,
  NeutralSimilarityDemotesFilesThatMatchEveryText, FolderScopeKeepsChildrenThatRankLowInTheProject,
  SmallScopeUsesOnlyTheSimilarityLimit, PagesOfOneQueryJoinToTheFullOrderedResult,
  FilesWithoutReadyEmbeddingAreNotResults, Supports768DimensionalModelStorageAndSearch,
  OlderProjectHnswIndexIsDroppedOnOpen, RelevanceRanksWithinRootAndFolderScope,
  PackedSnapshotSucceedsAfterRelevanceSearch (SemanticStorageControllerTest);
  CachedTextIsNotEncodedAgain, CacheKeyIncludesTheModelKey, TextIsNotLowercased,
  LeastRecentlyUsedEntryIsRemovedAtCapacity, FailedEncodeStoresNoEntry
  (SemanticQueryEmbeddingCacheTest); SemanticSearchProviderDelegatesWhenRegistered
  (FilterServiceTest). VssSearchCutsOffWeakTailBeforePaging is removed.
  LiveRelevanceKeepsListedPhotosAndRejectsNonsenseQueries: run directly with the
  environment below, final constants -> PASSED (240 s).
Manual verification (with the first constant 0.0793, before the time-test calibration):
  alcedo-cli session start --headless on a copy of bug_reproduce.alcd
  (649 SigLIP2 embeddings, older HNSW index) with a settings directory that enables
  natural-language search. The project opened (index dropped).
  search.query "a plate of sushi" and "quarterly tax invoice spreadsheet" -> matches [],
  total 0. "a Chinese pavilion with curved roofs" limit 5 -> total 15, has_more true, the
  top rows are the pavilion photos; offset 5 -> 5 rows, total 15, and no new sidecar
  embedding request in the host log (4 requests for 3 queries + the neutral text).
Performance or resource evidence:
  LibrarySearchBenchmarkTest.DISABLED_RelevanceSelectOver20000Embeddings, win_release with
  ALCEDO_BUILD_TESTS=ON (set back to OFF): 20 000 synthetic 512-dimensional embeddings in
  one folder, page of 120, final constants: p50 55.5 ms / 56.9 ms (two runs), p95 64.6 ms /
  79.1 ms, 419 kept. Target p50 <= 80 ms: met. (With the first constant 0.0793: p50 52.9 ms /
  53.7 ms, 552 kept.)
  Measured during the work: array_cosine_similarity twice with an inner join of the vectors
  161 ms; with a materialized CTE 100.6 ms. DuckDB 1.2.1 runs 20 000 rows (one row group)
  on one thread, one 512-dimensional product costs about 18 ms, and an inner join that
  carries the vectors builds its hash table on the embedding rows (about 50 ms). The final
  statement computes three inner products in a projection over the embedding scan and joins
  only scalars to the scope. Consequence: the scan cost follows the number of ready
  embeddings of the model in the project, not the scope size.
Calibration constants and table (GS1 only):
  Environment: ALCEDO_SEMANTIC_CALIBRATION_PROJECT = time-test.alcd (195 SigLIP2-256
  embeddings of 768 values, a varied set: city streets and skylines, animals, vehicles,
  portraits, concerts, landscapes, snow, night scenes), chosen by the maintainer;
  ALCEDO_SEMANTIC_CALIBRATION_QUERIES = 25 positive queries (2 in Chinese) with 39 listed
  files and 8 nonsense queries; ALCEDO_SEMANTIC_CALIBRATION_MODEL_DIR = local model directory;
  ALCEDO_MIND_BINARY = release alcedo_mind.exe. The query file was written by the implementing
  agent from contact sheets of the embedded previews, not by the maintainer. The listed files
  are clear examples, not every match.
  Chosen: kMinQuerySimilarity 0.0855, kMinRelevanceZ 1.0 (kMinScopeStatisticsCount 30).
  Feasible region over all 195 x 33 scores: kMinQuerySimilarity in [0.0817, 0.0894] with
  kMinRelevanceZ in [0, 1.8]. The highest c of a nonsense-query file is 0.0816 ("qwzx vplk
  mrrt", z 2.19); the lowest c of a listed file is 0.0894 ("樱花"). 0.0855 is the midpoint
  (margin 0.0039 on each side). The lowest z of a listed file is 1.80 (broad query "a snowy
  park by a lake in winter"); kMinRelevanceZ 1.0 also keeps the broad query of the first
  calibration (z 1.12).
  | query                                           | kept | listed min c | listed min z |
  | a squirrel                                      |    2 |       0.1437 |        6.066 |
  | an owl perched on a branch                      |    1 |       0.1341 |        5.423 |
  | a red race car on a track                       |    1 |       0.1347 |        4.693 |
  | a cowboy riding a horse                         |    2 |       0.1212 |        5.034 |
  | cherry blossom trees                            |   10 |       0.1169 |        2.900 |
  | wind turbines                                   |    2 |       0.1448 |        5.314 |
  | a city skyline at night                         |    2 |       0.1151 |        3.145 |
  | a man standing next to an elephant              |    1 |       0.1531 |        5.752 |
  | a woman playing electric guitar on stage        |    1 |       0.1340 |        5.255 |
  | a city bus with passengers wearing face masks   |    9 |       0.1387 |        3.438 |
  | sand dunes in front of mountains                |    5 |       0.1642 |        4.224 |
  | a flower shop                                   |    2 |       0.1388 |        4.282 |
  | a vintage streetcar                             |    9 |       0.1461 |        4.096 |
  | a snowy park by a lake in winter                |   19 |       0.1001 |        1.801 |
  | a hot air balloon over red rock canyons         |    1 |       0.1795 |        6.539 |
  | a protest march escorted by police motorcycles  |    1 |       0.1635 |        5.985 |
  | an old airplane in a museum hangar              |    1 |       0.1293 |        4.541 |
  | a portrait of a woman with tattoos              |    1 |       0.1363 |        6.443 |
  | a couple posing in front of a waterfall         |    2 |       0.1651 |        5.546 |
  | a vintage Mercedes car parked on the street     |    7 |       0.1213 |        2.890 |
  | a red boat in a harbor at dusk                  |    2 |       0.1582 |        4.214 |
  | a woman on a rope bridge in the forest          |    1 |       0.1509 |        4.832 |
  | a man taking a photo with a camera              |    3 |       0.1558 |        4.572 |
  | 一辆公交车 (a bus)                               |   10 |       0.1134 |        3.315 |
  | 樱花 (cherry blossoms)                           |    6 |       0.0894 |        3.071 |
  | nonsense query                                  | kept | top-1 c | top-1 r | top-1 z |
  | quarterly tax invoice spreadsheet               |    0 |  0.0344 | -0.0304 |   3.108 |
  | an astronaut walking on the moon                |    0 |  0.0753 |  0.0118 |   3.339 |
  | a plate of sushi                                |    0 |  0.0434 | -0.0289 |   3.047 |
  | a cat sleeping on a laptop keyboard             |    0 |  0.0350 | -0.0234 |   3.541 |
  | qwzx vplk mrrt                                  |    0 |  0.0816 |  0.0164 |   2.185 |
  | 一碗牛肉面 (a bowl of beef noodles)              |    0 |  0.0584 | -0.0059 |   3.601 |
  | an underwater coral reef with tropical fish     |    0 |  0.0305 | -0.0335 |   2.742 |
  | a newborn baby in a hospital crib               |    0 |  0.0495 | -0.0089 |   4.087 |
  Ranking by r against ranking by c (mean rank of the listed files): equal for 21 queries,
  r better for 2, c better for 2; mean 1.8 (r) against 1.6 (c). The neutral text does not
  change the ranking on this set either. Stop condition 3 of Section 12 ("r ranks worse than
  c") is not met clearly; the result is recorded for a later review.
  First calibration (superseded): bug_reproduce.alcd (649 SigLIP2-256 embeddings of one trip,
  most photos waterfalls and forest) gave the feasible region kMinQuerySimilarity in
  [0.07875, 0.08] and the first constant 0.0793. On time-test.alcd that constant keeps 2 files
  for "qwzx vplk mrrt". The two feasible regions do not overlap: with 0.0855, listed files of
  3 of the 9 bug_reproduce queries ("two people taking a selfie" 0.0807, "rocky mountain peaks
  under a cloudy sky" 0.0825, "a Chinese pavilion with curved roofs" 0.0844) fall below the
  limit. The maintainer chose the varied project as the reference because bug_reproduce
  is not representative.
Evidence path: build/tmp/global_search_gs1/ and build/tmp/global_search_gs1b/ (logs removed
  after the phase).
Remaining defects or unavailable platforms:
  - The constants are calibrated on SigLIP2 only, with a 0.0039 margin on c, and the c
    limit depends on the photo set (the two calibration projects have no common feasible
    value). Other model profiles (for example jina-clip-v2) are not calibrated. Follow-up
    issue.
  - Scan cost follows the project embedding count, not the folder size. Follow-up issue
    #427, fixed (record below).
  - macOS not built or tested in this phase.
  - Cold sidecar start on Enter still blocks the worker (Section 12, known behavior; the
    residency plan owns it). Since residency Phase SR1 the boot no longer blocks the GUI
    thread, and the sidecar stays running for the idle time after a search.
```

**Follow-up record: issue #427 (relevance products only for the folder scope)**

```text
Date / status: 2026-10-17 / complete.
Change: SemanticVectorSearch::BuildRatedCte computes the three inner products only for the
  embedding rows of scope files. A join of scalar columns (`scope_row` JOIN `scoped`) returns
  the row ids of the ready embedding rows of the scope; the outer scan of the embedding table
  reads those rows through a semi join on the row id, and a projection over it computes the
  products. The semi join builds its hash table on the row ids (the outer scan has no filter,
  so its estimate is the full table), so no vector is copied into a hash table, and the row id
  range of the scope rows is pushed into the outer scan.
Rejected shapes, measured on the production statement (20 000 embeddings, one folder):
  vectors joined with the scope (either text order) 109-128 ms, because DuckDB 1.2.1 builds the
  hash table on the embedding rows; IN semi join on (file_id, image_id) 105-115 ms; LATERAL
  68 ms; MATERIALIZED scope 63-81 ms; row id semi join 62-64 ms in that run, 53 ms in the
  benchmark. A constant IN list of file ids or row ids, an OR of file id ranges, and an extra
  single-column index on file_id did not reduce the vector reads (DuckDB 1.2.1 reads the vector
  column of every row that its pushed range filter admits).
Tests (release, ALCEDO_BUILD_TESTS=ON for the run, then OFF again; two runs each):
  DISABLED_RelevanceSelectOver20000Embeddings: before p50 57.3 / 55.2 ms; after 53.5 / 52.9 ms
    (target <= 80 ms: met; 419 kept in both).
  DISABLED_RelevanceSelectOfFolderDoesNotGrowWithProjectSize (new): folder of 100 files with
    contiguous ids, other files in another folder, half of their rows before and half after
    the folder rows:
      1 000 project embeddings:   before 28.6 / 28.3 ms, after 27.6 / 26.9 ms
      100 000 project embeddings: before 24.4 / 23.7 ms, after 28.0 / 31.0 ms
    Check large p50 <= 2 x small p50: passes after the change. It also passes before it,
    because DuckDB's join filter already limited the scan to the folder id range.
    Reported, not checked: one more folder file written after every other file, so the folder
    rows span the table (100 001 embeddings): before 227.6 / 241.7 ms (products of every row),
    after 69.6 / 67.6 ms (products of the 101 folder rows only; the scan still reads the
    vectors in the row id range of the folder rows).
  Debug: SemanticStorageControllerTest, AlbumQueryTest, FilterServiceTest,
    SemanticRelevanceCalibrationTest: 97 tests, 95 passed, 2 skipped (live calibration and
    LargeLibraryPagesStayBoundedAndRecordTimings, environment variables), including
    FolderScopeKeepsChildrenThatRankLowInTheProject and
    PagesOfOneQueryJoinToTheFullOrderedResult.
Remaining defect: when the rows of a folder span the embedding table, the scan reads the
  vectors of every row in that range. DuckDB 1.2.1 has no plan that fetches only the listed
  rows of an array column.
```

### Phase GS2 — Natural-language results in the Library

Issue: [#421](https://github.com/zidage/AlcedoStudio/issues/421).

**Objective and deliverables**

- `SearchController::ApplySemanticSearch` applies a natural-language search to the Library.
- The Library shows the kept files in relevance order when no photo sort is selected.
- Statistics, select all, and file position use the same kept set.
- The Inspector search filter card shows "Natural language" for a natural-language search.
- `SearchSession` has no sliding window. It serves a preview of up to 3 rows and `Replace` for
  `search.query`.

**Inputs and prerequisites**

- Phase GS1 complete: `EmbedQuery`, `BuildRelevanceSelect`, `SemanticQueryEmbedding`.

**Modules, files, and APIs**

- `LibrarySearchInput` (`search_controller.hpp:38-44`): add `semantic_text_`
  (`std::optional<std::wstring>`). `semantic_text_`, `pending_text_`, and `filter_` are
  mutually exclusive.
- Accepted search state in `SearchController`: add `accepted_semantic_query_`
  (`std::optional<SemanticQueryEmbedding>`).
- `LibraryQueryOutput`: add `semantic_query_` (`std::optional<SemanticQueryEmbedding>`).
- `ElementStore`: `BuildScopedFileQuery`, `MatchSetTable`, `ReadAlbumQuery`,
  `ReadAlbumFilePosition`, `ReadAlbumFileIds`, `BuildFolderStats` take
  `const std::optional<duckorm::SqlFragment>& relevance_select`.
- `AlbumBrowseService::ReadAlbumQuery` and the other forwarding functions take the same
  parameter.
- `album_query.cpp`: the order rule for relevance.
- `SearchSession`: remove `LoadMore`, `LoadPrevious`, `HasPrevious`, `WindowStart`,
  `kWindowCapacity`, `SubmitSemantic`, and the window logic of `ApplyResponse`. Add
  `kPreviewRows = 3` and `kMaxPageRows = 48`. `ResponseApplied(requestId, mode)` loses the two
  count arguments. Keep `Total()` and `HasMore()` as C++ accessors for `search.query`. Remove
  their `Q_PROPERTY` entries.
- `search_commands.cpp`: use `kMaxPageRows`; update the `ResponseApplied` connection.
- `AlbumInspectorPanel.qml`: the "Natural language" line, bound to a new
  `SearchController` property `activeSearchIsSemantic`.

**Data rules and invariants**

- The match set is built from `scope JOIN relevance_select ON file_id` when the relevance select
  is present. It has the column `relevance DOUBLE`. Without it, the column is `NULL`.
- Order: with `sort_field_ == kNone` and a relevance column, photos order by
  `relevance DESC, file_id ASC` in flat mode and in each group. Group order is unchanged. With a
  selected sort, the current order rule applies.
- The stats filter and the relevance select combine with AND (the join plus the WHERE).
- The kept set is computed once for each `ElementStore` operation, in the match set. No read
  computes it a second time.
- Page reads after the apply use `accepted_semantic_query_`. They do not call the provider.
- A folder change keeps the active search. The next read builds the relevance select for the new
  folder from the accepted embedding.
- Clearing the search removes `accepted_semantic_query_`.

**Implementation steps**

1. Add the relevance parameter to `BuildScopedFileQuery`, `MatchSetTable`, and the four
   `ElementStore` reads. Add the `relevance` column to the match set projection.
2. Add the relevance order to `album_query.cpp` and document it in `album_query.hpp`.
3. Forward the parameter through `AlbumBrowseService`.
4. In `RunLibraryQuery`: when `semantic_text_` is set, call `EmbedSemanticQuery`, store the
   result in `output.semantic_query_`, build the select with `BuildSemanticRelevanceSelect`, and
   pass it. When the accepted search has an embedding (page reads, select all, position), build
   the select from it.
5. Add `ApplySemanticSearch`. Check, in this order: empty text (return "" and do nothing), route
   not `semantic` (return the text "This query uses field search."), too long, no provider. Then
   set the pending search and request a refresh.
6. In `AcceptLibrarySearch`, store `output.semantic_query_` and set `activeSearchIsSemantic`.
7. Make `StatsEngine::RefreshStats` pass the relevance select of the accepted search.
8. Simplify `SearchSession`. `Preview(query)` requests `kPreviewRows` rows for the `fuzzy`
   route and sets `awaitingSubmit` for the `semantic` route.
9. Update `search_commands.cpp`.
10. Add the "Natural language" line to `AlbumInspectorPanel.qml`.
11. Make the current dialog compile against the new `SearchSession` with the smallest change:
    remove its Load more and Load previous controls. Phase GS4 replaces the dialog.

**Primary success call chain**

Section 5.3.

**Primary failure and restore call chain**

Section 5.4. A relevance SQL failure follows the same chain with the DuckDB message.

**Tests and evidence**

| Test (target) | Assertion |
| --- | --- |
| `SemanticApplyShowsKeptFilesInRelevanceOrder` (`AlbumBackendSearchWorkerTest`, fake provider) | After the apply, the published grid ids equal the relevance order of the synthetic embeddings. |
| `SemanticApplyKeepsInspectorFilters` (`AlbumBackendSearchWorkerTest`) | With a camera stats filter, the grid shows only kept files of that camera. |
| `SelectedSortReplacesRelevanceOrder` (`LibrarySearchColumnsTest` or `ElementStore` test) | With capture-time sort, the same set appears in capture-time order. |
| `GroupedSemanticResultKeepsGroupOrderAndRanksInsideGroups` (`ElementStore` test) | Groups keep their order; photos in each group are in relevance order. |
| `SemanticStatsCountOnlyKeptFiles` (`ElementStore` test) | Bucket counts equal the counts of the kept set. |
| `SemanticSelectAllReturnsTheKeptSet` (`ElementStore` test) | `ReadAlbumFileIds` returns exactly the kept ids. |
| `SemanticPageReadsDoNotCallTheProvider` (`AlbumBackendSearchWorkerTest`) | After the apply, scrolling pages calls `EmbedQuery` zero more times. |
| `SemanticApplyFailureKeepsAcceptedContentAndReportsError` (`AlbumBackendSearchWorkerTest`) | A throwing provider leaves the earlier grid, sets `queryError`, and `activeSearchQuery` is unchanged. |
| `ApplySemanticSearchRejectsTooLongQueryWithoutRefresh` (`AlbumBackendSearchWorkerTest`) | The returned text is not empty and no library refresh starts. |
| `FolderChangeReevaluatesActiveSemanticSearch` (`AlbumBackendSearchWorkerTest`) | After a folder change, the grid shows kept files of the new folder; the provider is not called. |
| `ClearSearchRemovesSemanticOrder` (`AlbumBackendSearchWorkerTest`) | After clear, the order is file id. |
| `PreviewRequestsThreeRowsForFieldQuery` (`SearchSessionTest`) | `Preview` requests limit 3. |
| `SemanticQueryWaitsForSubmitWhenNaturalLanguageSearchIsOn` (`SearchSessionTest`, updated) | No request for a semantic preview. |
| `AppendBeyondCapacityDropsLeadingRows` (`SearchSessionTest`) | Removed with the window. |
| `FuzzySearchFindsImportedFileName` (`AutomationSearchE2ETest`) | Still passes with the same response fields. |

**Build and run commands**

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlbumBackendSearchWorkerTest SearchSessionTest LibrarySearchColumnsTest LibrarySearchRecallTest SemanticStorageControllerTest FilterServiceTest alcedo_main
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "AlbumBackendSearchWorkerTest|SearchSessionTest|LibrarySearchColumnsTest|LibrarySearchRecallTest|SemanticStorageControllerTest|FilterServiceTest" --output-on-failure
```

Run `AutomationSearchE2ETest` when the automation targets are built. Record it as not run when
they are not built.

**Exit criteria**

- [x] All named tests pass.
- [ ] Manual check in `alcedo_main`: a natural-language Enter shows the kept photos in
      relevance order, the Inspector card shows "Natural language", and clear restores the
      folder. Not done in this phase (see the completion record).
- [x] `SearchSession` has no window code and no `LoadMore` or `LoadPrevious`.
- [x] `search.query` response fields are unchanged.

**Expected diff**

1100–1700 lines (storage 300–450, Library and search controller 300–450, session 150–250 net,
tests 350–550).

**Completion record**

```text
Phase / date / status: GS2 / 2026-10-17 / complete, except the manual check in alcedo_main.
Source revision and branch: fix/semantic-search-calibrated-relevance at 7b5b652f0 (GS1, not
  yet merged); branch feature/semantic-search-library-results, stacked on it.
Actual changed modules: ElementStore (BuildScopedFileQuery, MatchSetTable, ReadMatchSet,
  ReadAlbumQuery, ReadAlbumFilePosition, ReadAlbumFileIds, BuildFolderStats); album_query
  (OccurrenceOrderTerms, AlbumQueryOptions documentation); AlbumBrowseService (three reads);
  SleeveFilterService::BuildSemanticRelevanceSelect (new); LibraryModule (RunLibraryQuery,
  RelevanceSelectOf, select all, ids, position, accepted input); SearchController
  (ApplySemanticSearch, activeSearchIsSemantic, accepted_semantic_query_, AcceptLibrarySearch);
  SearchSession (no window); search_commands.cpp; GlobalSearchDialog.qml (minimal);
  AlbumInspectorPanel.qml; alcedo_main_zh_CN.ts (new strings, edited by hand, no lupdate).
Implemented behavior:
  - Enter on a natural-language query calls SearchController::ApplySemanticSearch. It checks,
    in this order: empty text (returns "" and does nothing), route not semantic, too long, no
    semantic search provider, no active semantic model; a rejection returns the reason and
    changes nothing. Else the search becomes the pending search and the library refreshes.
  - The query worker embeds the text once (EmbedSemanticQuery), builds the relevance select
    for the current folder, and passes it to the library read. The match set joins it
    (`JOIN (<select>) rel ON rel.file_id = e.id AND rel.image_id = fi.image_id`) and stores
    `relevance DOUBLE` (NULL without a select). The Inspector filters combine with AND. The
    select runs once for each ElementStore operation.
  - Without a selected sort the photos order by relevance DESC, file id ASC in the flat mode
    and inside each group (it replaces the date-group timestamp order); the group order does
    not change. A selected sort replaces the relevance order; the kept set stays.
  - The accepted embedding is kept in SearchController and in the accepted library input.
    Pages, select all, ids, positions, and later refreshes (Inspector filters, sort, group,
    RefreshStats) build the select from it and call no provider. Clear, a new field or exact
    search, and a folder selection release it.
  - The Inspector search card shows "Natural language" below the query (activeSearchIsSemantic).
  - SearchSession: no window; Preview requests kPreviewRows (3) rows for a field query and
    nothing for a semantic query; Replace serves search.query with at most kMaxPageRows (48)
    rows; Offset/Total/HasMore are C++ accessors; ResponseApplied(requestId, mode).
  - GlobalSearchDialog.qml: Load more / Load previous, the window scroll code, and the result
    count text are removed; Enter on a semantic query calls ApplySemanticSearch and closes the
    dialog, or shows the returned reason.
Differences from the phase text (recorded, not hidden):
  - Folder change: Section 2.6 is corrected. SelectFolder clears the search (current behavior,
    decision of 2026-10-17). FolderChangeReevaluatesActiveSemanticSearch tests the folder
    change that keeps the search (Delete Folder of the current folder) and then the clear.
  - Step 7 needs no StatsEngine change: StatsEngine::RefreshStats only requests a library
    refresh, and that read already passes the relevance select of the accepted search.
  - ApplySemanticSearch also rejects a project without an active semantic model (Section 2.3
    lists this error; step 5 did not).
  - The ElementStore tests are in AlbumQueryTest (new fixture AlbumQueryRelevanceTest), the
    test target of ElementStore::ReadAlbumQuery.
  - Tests added beyond the table: SemanticResultShowsKeptFilesInRelevanceOrderOnEveryPage,
    NonsenseSemanticQueryShowsNoPhotos, ReplacePageIsLimitedToMaxPageRowsAndKeepsItsOffset.
  - GlobalSearchDialogQmlTest: ScrolledSearchResultsRenderPreviewThumbnailsAndReopenStillWorks
    becomes PreviewThumbnailsRenderAfterReopen (3 preview rows, no second page);
    SemanticSubmitSurvivesPendingTypingDebounceAndLeavesAwaitingSubmit becomes
    SemanticEnterSurvivesPendingTypingDebounceAndShowsRejection; the test of the switch sync on
    dialog open is renamed NaturalLanguageSwitchIsSyncedOnDialogOpen (AGENTS.md terminology
    rule for touched files).
  - search.query keeps its semantic route through SearchFolderSemanticRows (GS1).
Explicitly unimplemented items: GS3 to GS5. The synchronous position read of
  LibraryModule::IndexOfElementInCurrentView (editor restore, current behavior) builds the
  relevance select on the GUI thread; it calls no provider.
Primary success call chain:
  GlobalSearchDialog Enter -> SearchController::ApplySemanticSearch(query)        [GUI thread]
       route / length / provider / active model checks -> "" ; pending_search_ =
       {display_query_, semantic_text_} ; LibraryModule::RequestLibraryRefresh
    -> SubmitPendingRefresh -> SearchRequestWorker kApply -> RunLibraryQuery     [query worker]
       SleeveFilterService::EmbedSemanticQuery -> SemanticSearchProvider::EmbedQuery (cache)
       RelevanceSelectOf -> SleeveFilterService::BuildSemanticRelevanceSelect
         -> SemanticVectorSearch::BuildRelevanceSelect(folder, embedding)
       AlbumBrowseService::ReadAlbumQuery(..., relevance) -> ElementStore::ReadAlbumQuery
         -> BuildScopedFileQuery(folder, filter, relevance) -> MatchSetTable (relevance column)
         -> ReadAlbumQueryFromMatchSet(has_relevance) -> OccurrenceOrderTerms (relevance DESC)
    -> LibraryModule::PublishRefresh                                              [GUI thread]
       accepted_input_.search_.semantic_query_ = output.semantic_query_
       SearchController::AcceptLibrarySearch(used, filter, semantic_query) stores the embedding
       -> SearchStateChanged (activeSearchQuery, activeSearchIsSemantic); grid, sections,
          Inspector statistics from the same match set.
  Later reads (page, select all, ids, position, refresh) -> RelevanceSelectOf(accepted
    embedding) -> the same ElementStore reads; no provider call.
Primary failure and restore call chain:
  RunLibraryQuery: EmbedQuery throws (runtime start, model mismatch, invalid vector) or the
    relevance SQL fails -> output.error_ = message
    -> PublishRefresh: SetQueryError(message); accepted content, accepted search, and the
       Inspector state stay; pending_search_ stays for Retry; AcceptLibrarySearch is not called
    -> LibraryWorkspace shows "Library query failed: <message>" with Retry
    -> RetryLibraryQuery runs the pending natural-language search again.
  ApplySemanticSearch rejection -> returned reason; no pending search, no refresh; the dialog
    stays open and shows the reason.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlbumQueryTest
    SearchSessionTest AlbumBackendSearchWorkerTest GlobalSearchDialogQmlTest
    LibrarySearchColumnsTest LibrarySearchRecallTest SemanticStorageControllerTest
    FilterServiceTest AlbumBackendLibraryQueryTest alcedo_main alcedo_cli
    AutomationSearchE2ETest                                                   -> 0
  ctest --test-dir build/debug -R "^(AlbumQueryTest|SearchSessionTest|AlbumBackendSearchWorkerTest|
    GlobalSearchDialogQmlTest|LibrarySearchColumnsTest|LibrarySearchRecallTest|
    SemanticStorageControllerTest|FilterServiceTest|AlbumBackendLibraryQueryTest|
    AutomationSearchE2ETest)\." --output-on-failure                          -> 0
Discovered / passed / failed / skipped counts: 167 discovered; 165 passed; 0 failed;
  1 skipped (AlbumQueryTest.LargeLibraryPagesStayBoundedAndRecordTimings, a timing record
  that skips in ctest); 1 disabled (LibrarySearchRecallTest.NikonFolderImportsRawOnlyAndSearch
  FindsFileNames). The full suite was not run (AGENTS.md).
  Named tests, all passed: SemanticApplyShowsKeptFilesInRelevanceOrder,
  SemanticApplyKeepsInspectorFilters, SemanticPageReadsDoNotCallTheProvider (321 files, 121
  kept: two pages, select all, a position, and a refresh with one EmbedQuery call),
  SemanticApplyFailureKeepsAcceptedContentAndReportsError (also Retry),
  ApplySemanticSearchRejectsTooLongQueryWithoutRefresh, FolderChangeReevaluatesActiveSemanticSearch,
  ClearSearchRemovesSemanticOrder, NonsenseSemanticQueryShowsNoPhotos
  (AlbumBackendSearchWorkerTest, fake provider); SemanticResultShowsKeptFilesInRelevanceOrder
  OnEveryPage, SelectedSortReplacesRelevanceOrder, GroupedSemanticResultKeepsGroupOrderAndRanks
  InsideGroups, SemanticStatsCountOnlyKeptFiles, SemanticSelectAllReturnsTheKeptSet
  (AlbumQueryTest); PreviewRequestsThreeRowsForFieldQuery, ReplacePageIsLimitedToMaxPageRows
  AndKeepsItsOffset, SemanticQueryWaitsForSubmitWhenNaturalLanguageSearchIsOn (updated)
  (SearchSessionTest); FuzzySearchFindsImportedFileName (AutomationSearchE2ETest). Removed:
  AppendBeyondCapacityDropsLeadingRows.
  GlobalSearchDialogQmlTest (offscreen QML): 6 of 6 passed.
Manual verification: not done. The check needs alcedo_main with a project that has semantic
  embeddings and a running sidecar; no automation command applies a search to the Library
  (Section 3.2). The Library behavior is covered by the AlbumBackendSearchWorkerTest cases
  above through ApplicationModuleHost with a fake provider.
Performance or resource evidence: no new benchmark. The relevance join runs only when the
  select is present; without it BuildScopedFileQuery and the match set projection add only
  `CAST(NULL AS DOUBLE) AS relevance` (Section 12, field-search cost).
Evidence path: build/tmp/global_search_gs2/ (removed after the phase).
Changed lines: about 1750 (production about 700, tests about 1000), below the 2000-line limit.
Remaining defects or unavailable platforms:
  - Manual check in alcedo_main not done.
  - macOS not built or tested in this phase.
  - Cold sidecar start on Enter blocks the query worker, not the GUI thread (Section 12; the
    residency plan owns it).
```

### Phase GS3 — Palette data in SearchController

Issue: [#422](https://github.com/zidage/AlcedoStudio/issues/422).

**Objective and deliverables**

- `SearchController` keeps the recent searches (Section 2.2).
- `SearchController` exposes the palette operations: `ApplyRecentSearch`,
  `ApplyRecommendation(category, label)`, and `MatchingRecommendations(query, limit)`.
- The preview thumbnail signal names the URL `thumbUrl`.
- The current dialog uses the new operations, so the behavior is testable before the new palette.

**Inputs and prerequisites**

- Phase GS2 complete.

**Modules, files, and APIs**

- `search_controller.hpp` and `.cpp`:
  - `Q_PROPERTY(QVariantList recentSearches READ RecentSearches NOTIFY RecentSearchesChanged)`;
  - `Q_INVOKABLE QString ApplyRecentSearch(int index)`;
  - `Q_INVOKABLE void ApplyRecommendation(const QString& category, const QString& label)`, which
    calls `StatsEngine::ToggleStatsFilter(category, label)`;
  - `Q_INVOKABLE QVariantList MatchingRecommendations(const QString& query, int limit)`, which
    returns the `SearchRecommendations` rows whose label contains the query, case-insensitive;
  - recent list update in `AcceptLibrarySearch`;
  - rename the argument `dataUrl` to `thumbUrl` in `SearchPreviewThumbnailUpdated` and
    `searchPreviewThumbnailUpdated`, and `thumb_data_url` in touched search code.
- `GlobalSearchDialog.qml`: `applyRecommendation` calls `ApplyRecommendation`. No other change.

**Data rules and invariants**

- Recent searches: Section 2.2. The `QSettings` value is a list of maps `{query, route}`.
- `SearchController` reads the list once at construction and writes it after each change.
- A stored entry with an unknown route is skipped on read.

**Implementation steps**

1. Add the recent list, its read, its write, and the update in `AcceptLibrarySearch`.
2. Add `ApplyRecentSearch`. For route `semantic` it calls `ApplySemanticSearch` and returns its
   text. For route `fuzzy` it calls `ApplyFuzzySearch` and returns an empty text.
3. Add `ApplyRecommendation` and `MatchingRecommendations`.
4. Rename the thumbnail URL arguments.
5. Change `applyRecommendation` in the current dialog.

**Primary success call chain**

```text
ApplyFuzzySearch("R5") -> Library refresh -> PublishRefresh -> AcceptLibrarySearch
  -> recent list {R5, fuzzy} at index 0 -> QSettings write -> RecentSearchesChanged
```

**Primary failure and restore call chain**

```text
ApplyRecentSearch(index out of range) -> returns "This recent search does not exist."
  -> no state change
```

**Tests and evidence**

| Test (target `AlbumBackendSearchWorkerTest`) | Assertion |
| --- | --- |
| `RecentSearchAddsAcceptedQueryAtTheTop` | After two applies, the list is newest first, with routes. |
| `RecentSearchMovesARepeatedQueryWithoutDuplicate` | A repeated query has one entry, at the top. |
| `RecentSearchKeepsAtMostFiveEntries` | After six applies, the oldest entry is gone. |
| `ExactFileSearchIsNotARecentSearch` | `ApplyExactSearch` does not change the list. |
| `FailedSearchIsNotARecentSearch` | A semantic apply with a throwing provider does not change the list. |
| `RecentSearchesPersistInSettings` | A new controller reads the same list from `search/recentQueries`. |
| `RecentSemanticSearchUsesItsStoredRoute` | With the natural-language switch off, a stored semantic entry still applies a natural-language search. |
| `RecommendationAppliesStatsFilter` | `ApplyRecommendation("camera", X)` sets the camera stats filter `X` and starts one refresh. |
| `MatchingRecommendationsFilterByLabel` | The query `r5` returns the camera row `Canon EOS R5` and no unrelated row. |

**Build and run commands**

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AlbumBackendSearchWorkerTest alcedo_main
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "AlbumBackendSearchWorkerTest" --output-on-failure
```

**Exit criteria**

- [x] All named tests pass.
- [x] No search source file uses the names `dataUrl` or `thumb_data_url` for the preview URL.

**Expected diff**

400–700 lines (controller 200–300, tests 200–350, dialog 10).

**Completion record**

```text
Phase / date / status: GS3 / 2026-10-17 / complete.
Source revision and branch: feature/semantic-search-library-results at 54d4c95a2 (GS2, not yet
  merged); branch feature/search-palette, stacked on it. Commit 1c21de722.
Actual changed modules: SearchController (recent searches, ApplyRecentSearch,
  ApplyRecommendation, MatchingRecommendations, searchSettingsPanelOpen, preview pins);
  AlbumItem / AlbumThumbnailModel / ThumbnailManager / LibraryModule / StatsEngine (thumb_url
  and thumbUrl names); GlobalSearchDialog.qml (applyRecommendation); alcedo_main_zh_CN.ts.
Implemented behavior:
  - SearchController keeps at most 5 recent searches {query, route}, newest first, no
    duplicate query text, in QSettings search/recentQueries; read once at construction, written
    after each change; an entry with an unknown route is skipped on read.
  - AcceptLibrarySearch records an accepted field query (pending_text_ with a filter) and an
    accepted natural-language query. An exact-file search, a query with no terms, and a failed
    refresh record nothing. AcceptLibrarySearch emits RecentSearchesChanged itself (the list
    does not depend on the library result); SearchStateChanged stays with the caller.
  - ApplyRecentSearch: out of range -> "This recent search does not exist."; fuzzy ->
    ApplyFuzzySearch; semantic -> ApplySemanticText, the checks of ApplySemanticSearch after
    the route check (too long, provider, active model), so the switch does not change a stored
    entry.
  - ApplyRecommendation(category, label) calls StatsEngine::ToggleStatsFilter only when that
    category filter does not already have the label (a row click sets the filter; it never
    clears it). MatchingRecommendations returns the SearchRecommendations rows whose label
    contains the trimmed query (case-insensitive), at most limit rows.
  - The preview thumbnail URL is named thumbUrl in the signals and thumb_url in AlbumItem.
Differences from the phase text (recorded, not hidden):
  - Added SearchController::searchSettingsPanelOpen (QSettings search/settingsPanelOpen). GS4
    step 5 puts this state in the palette; the palette cannot persist it without a C++ owner
    (the QtCore Settings QML type is not linked in this static Qt build), so SearchController
    owns it and the palette binds to it.
  - ApplyRecommendation does not toggle a filter off (see above).
  - Preview pins: SearchController now releases only the ThumbnailService pins that its own
    preview requests took (search_preview_pinned_thumbnails_). Interleaving that made this
    necessary: the palette requests the k512 tier, which the Library grid also pins; a preview
    shown from the Library item URL took no pin, but its release (frame destroyed) called
    ReleaseThumbnail(k512) and decremented the grid's pin. A preview whose conversion produced
    no image now releases its pin (before, the pin leaked).
  - The thumb_data_url rename covers AlbumItem and its readers, not only the search files,
    because the search code reads the Library item field.
Explicitly unimplemented items: none of GS3.
Primary success call chain:
  ApplyFuzzySearch("R5") -> LibraryModule::RequestLibraryRefresh -> worker -> PublishRefresh
    -> SearchController::AcceptLibrarySearch -> RecordRecentSearch(R5, fuzzy) -> QSettings
    write -> RecentSearchesChanged
Primary failure and restore call chain:
  ApplyRecentSearch(index out of range) -> "This recent search does not exist." -> no change.
  Semantic apply whose embedding fails -> PublishRefresh sets queryError; AcceptLibrarySearch
    is not called -> the recent list does not change.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target
    AlbumBackendSearchWorkerTest alcedo_main (and the GS4 targets)          -> 0
  ctest --test-dir build/debug -R "^AlbumBackendSearchWorkerTest\." --output-on-failure -> 0
Discovered / passed / failed / skipped counts: AlbumBackendSearchWorkerTest 24 / 24 / 0 / 0.
  Named tests, all passed: RecentSearchAddsAcceptedQueryAtTheTop,
  RecentSearchMovesARepeatedQueryWithoutDuplicate, RecentSearchKeepsAtMostFiveEntries,
  ExactFileSearchIsNotARecentSearch, FailedSearchIsNotARecentSearch,
  RecentSearchesPersistInSettings, RecentSemanticSearchUsesItsStoredRoute,
  RecommendationAppliesStatsFilter, MatchingRecommendationsFilterByLabel; added
  SearchSettingsPanelOpenStatePersistsInSettings. The 13 earlier tests of the target pass.
  Direct callers: SearchSessionTest 4/4, AlbumBackendLibraryQueryTest 24/24,
  AutomationSearchE2ETest 1/1. The full suite was not run (AGENTS.md).
Manual verification: see GS4.
Evidence path: build/tmp/global_search_gs3/ (removed after the phase).
Remaining defects or unavailable platforms: macOS not built or tested.
```

### Phase GS4 — New search palette

Issue: [#422](https://github.com/zidage/AlcedoStudio/issues/422).

**Objective and deliverables**

- The proposed file `qml/SearchPalette.qml` implements Section 2.1 to Section 2.4 and
  Section 2.7.
- `AppDialogs.qml` instantiates `SearchPalette` in place of `GlobalSearchDialog`. It keeps
  `id: globalSearchDialogObj`, `objectName: "globalSearchDialog"`, the alias, and
  `openFromCollection()`, so `Main.qml` does not change.
- The text field keeps `objectName: "globalSearchField"`.

**Inputs and prerequisites**

- Phase GS3 complete.
- Read `ui/alcedo_main/DESIGN.md` and the `alcedo-qml-ui` skill.

**Modules, files, and APIs**

- `qml/SearchPalette.qml` (proposed). Register it in `ALCEDO_MAIN_QML_FILES`.
- `qml/AppDialogs.qml`: instantiate `SearchPalette`.
- AppTheme and `DESIGN.md`: add a token only if no current token fits. Add each new token to
  both in the same change.
- `GlobalSearchDialogQmlTest`: point the tests to the new structure.

**Data rules and invariants**

- The palette requests preview thumbnails only for visible preview frames, with `maxEdge` 512.
  It releases each thumbnail when the frame is destroyed or the query changes. It uses the
  current `SetSearchPreviewThumbnailVisible` and `CancelSearchPreviewThumbnails` operations.
- The palette holds no search state except the text, the highlight index, and the side panel
  open state.
- The palette reads routes and preview rows only from `SearchSession`. It does not classify a
  query.

**Implementation steps**

1. Write the palette shell: input row, sections column, side panel, footer. Use the Basic style
   and `appTheme` tokens.
2. Write the Recent searches, Commands, Filters, and Photos sections.
3. Write the preview frame as an inline component nested in the root object (an inline
   component at file level does not compile with `qmlcachegen`). Bind the thumbnail pin to the
   frame lifetime.
4. Write the highlight rule (Up, Down, hover) and Enter (Section 2.3).
5. Write the side panel with `ThemeCheckBox` and the switch. Persist its state in
   `search/settingsPanelOpen`.
6. Write the states of Section 2.7.
7. Change `AppDialogs.qml` to instantiate `SearchPalette`.
8. Update `GlobalSearchDialogQmlTest`.

**Primary success call chain**

```text
type "R5" -> debounce 140 ms -> SearchSession::Preview -> worker -> 3 rows
  -> Photos section shows 3 frames -> SetSearchPreviewThumbnailVisible(id, image, true, 512)
  -> SearchPreviewThumbnailUpdated(id, thumbUrl) -> frame shows the image
Enter -> ApplyFuzzySearch("R5") -> close -> Library refresh -> recent searches updated
```

**Primary failure and restore call chain**

```text
semantic Enter -> ApplySemanticSearch returns "Natural language search is not available yet"
  -> palette stays open -> Photos section shows the text -> no Library refresh
```

**Tests and evidence**

| Test (target `GlobalSearchDialogQmlTest`) | Assertion |
| --- | --- |
| `FieldQueryShowsThreePreviewThumbnails` | Three frames show `image://alcedo-thumb/` sources. |
| `SemanticQueryShowsEnterHintAndNoPreviews` | The hint text is visible and no preview request is sent. |
| `RecommendationRowAppliesStatsFilterAndCloses` | A click on a camera row sets the camera stats filter and closes the palette. |
| `RejectedSemanticEnterKeepsPaletteOpen` | With no provider, Enter shows the error text and the palette stays open. |
| `SettingsPanelStatePersists` | The drawer is open on reopen after it was open; it does not change the palette width. |
| `PreviewThumbnailsRenderAfterReopen` (renamed in GS2 from `ScrolledSearchResultsRenderPreviewThumbnailsAndReopenStillWorks`; GS2 tests the old dialog with it) | Thumbnails show after close and reopen. |
| `NaturalLanguageSwitchStateIsReadOnOpen` (renamed from the current test of the switch sync on open) | The check boxes are disabled on open when the persisted switch is on. |

QML tests can fail for offscreen input reasons. Record such a failure with its cause. Do not
weaken the assertion. The C++ tests of GS2 and GS3 are the required evidence for behavior.

**Build and run commands**

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GlobalSearchDialogQmlTest alcedo_main
$env:PATH = "$PWD\build\debug\vcpkg_installed\x64-windows\debug\bin;$env:PATH"
ctest --test-dir build/debug -R "GlobalSearchDialogQmlTest" --output-on-failure
```

**Exit criteria**

- [x] QML test results are recorded, with the cause of each failure.
- [x] `SearchPalette.qml` has no `QtQuick.Controls.Material` import and no literal color, size,
      or spacing value that has an `appTheme` token.
- [ ] Manual check in `alcedo_main`: side panel open and closed, a narrow window, a field query,
      a natural-language query, a recommendation row, a recent search, and a photo click.
      Not done (see the completion record).
- [x] Screenshots of the states of Section 2.7 are in the completion record (offscreen renders;
      photo pixels are missing, see the record).

**Expected diff**

900–1300 lines (palette 700–950, tests 150–300, theme and design text 0–50).

**Completion record**

```text
Phase / date / status: GS4 / 2026-10-17 / complete, except the manual check in alcedo_main.
Source revision and branch: feature/search-palette (stacked on GS2), commit 6ee9fab2f.
Actual changed modules: SearchPalette.qml (new); ThemeSwitch.qml (new shared component);
  AppDialogs.qml; ALCEDO_MAIN_QML_FILES; AppTheme (searchPaletteMaxWidth 960,
  searchSettingsPanelWidth 260, searchPaletteWindowMarginX 28, searchPaletteWindowMarginY 36)
  with DESIGN.md "Search palette" and the shared-component table; docs/VI/README.md entries
  for SearchPalette.qml and ThemeSwitch.qml; alcedo_main_zh_CN.ts (SearchPalette context,
  edited by hand, no lupdate); GlobalSearchDialogQmlTest.
Implemented behavior: Sections 2.1 to 2.4 and 2.7.
  - Layout: main column min(window - 56, 960); the open side panel adds 260 px at the right and
    keeps the left edge, and in a narrow window the main column becomes narrower first; height
    follows the content up to window - 72; the shell stays vertically centered. Blurred
    backdrop as before. A click outside the shell closes the palette.
  - Sections: Recent searches and Filters (8 rows) for the empty query; Commands, Filters
    (MatchingRecommendations, 4 rows), and Photos for a typed query. Hidden when empty.
  - Rows: icon, title, trailing value; filter rows show category, value, and count as three
    labels; the command row shows "Enter"; Photos shows "View all" for field previews.
  - Photos: up to 3 frames (4:3, PreserveAspectCrop, k512, file name elided in the middle);
    loading shows 3 SkeletonBlock frames; "No matching photos in this folder"; the
    SearchSession error and a rejected natural-language Enter in an AlertBadge; the semantic
    route shows "Press Enter to search by meaning." and requests nothing.
  - Each frame requests its thumbnail when it is created and releases it when it is destroyed
    (new rows, a new query, loading, or close).
  - Highlight: one index moved by hover and by Up / Down across the recent, command, and filter
    rows; the command row is highlighted when the query changes; Enter runs the highlighted
    row, else applies the query; Enter with an empty query and no highlight does nothing.
  - Settings panel: title, "Search in", 4 ThemeCheckBox rows (disabled with the
    searchFieldFiltersReason text while natural-language search is on), divider, ThemeSwitch.
    The panel folds with motionFoldOpenMs / motionFoldCloseMs. The open state is
    SearchController::searchSettingsPanelOpen.
  - The palette keeps id globalSearchDialogObj, objectName "globalSearchDialog",
    openFromCollection(), and objectName "globalSearchField"; Main.qml is unchanged.
Differences from the phase text (recorded, not hidden):
  - The settings panel open state is owned by SearchController (GS3 record), not the palette.
  - The root id is searchPalette: an id named `palette` resolves to Item.palette
    (QQuickPalette) inside child items, so every `palette.<function>` call failed.
  - GS4 changed about 2786 lines (2099 added, 687 removed), above the 2000-line split rule of
    Section 12: SearchPalette.qml 1295, the rewrite of global_search_dialog_qml_test.cpp
    1174 changed lines (the old tests targeted the removed list and paging), the translations
    115. The phase was not split because the test rewrite replaces tests of code that GS5
    deletes; production QML is within the planned range plus ThemeSwitch.qml.
Explicitly unimplemented items: the manual check in alcedo_main.
Primary success call chain:
  type "R5" -> previewTimer 140 ms -> refreshPreview -> SearchSession::Preview -> worker ->
    3 rows -> PreviewFrame x3 -> SetSearchPreviewThumbnailVisible(id, image, true, 512) ->
    SearchPreviewThumbnailUpdated(id, thumbUrl) -> frame shows the image
  Enter -> handleSearchSubmit -> applyQuery -> ApplyFuzzySearch("R5") -> close -> Library
    refresh -> AcceptLibrarySearch -> recent searches updated
Primary failure and restore call chain:
  semantic Enter -> ApplySemanticSearch returns "No semantic model is active." -> the palette
    stays open -> the Photos section shows the AlertBadge -> no Library refresh
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
    AlbumBackendSearchWorkerTest GlobalSearchDialogQmlTest SearchSessionTest
    MainQmlWorkflowTest UiFuzzAutomationTest EditorFilmstripQmlTest
    AlbumBackendLibraryQueryTest AutomationSearchE2ETest                     -> 0
  ctest --test-dir build/debug -R "^GlobalSearchDialogQmlTest\." --output-on-failure -> 0
Discovered / passed / failed / skipped counts: GlobalSearchDialogQmlTest 9 / 9 / 0 / 0:
  FieldQueryShowsThreePreviewThumbnails, PreviewThumbnailsRenderAfterReopen,
  SemanticQueryShowsEnterHintAndNoPreviews, RecommendationRowAppliesStatsFilterAndCloses,
  RejectedSemanticEnterKeepsPaletteOpen, SettingsPanelStatePersists,
  NaturalLanguageSwitchStateIsReadOnOpen, SearchControllerClassifiesAndRoutesBySemanticToggle,
  TypingTwentyCharactersQuicklyAppliesOnlyTheLastResponse. The first run had 3 failures, all
  from the `palette` id (above); they pass after the rename.
  Not related to this phase, the same with the old dialog in AppDialogs.qml:
  MainQmlWorkflowTest 3 passed / 2 failed (ProductionWindowLoadsAndRoutesCoreWorkspaceActions:
  BackgroundTaskBar.toggleByUser not invokable; ConnectionsHandlersReachOwnerSignals...:
  ImageActionsController.qml:388 onDeleteTargetsFinished matches no signal);
  EditorFilmstripQmlTest 0 / 12 (EditorFilmstrip.qml:280 reads selection.revision, which
  the test fake does not have). UiFuzzAutomationTest built, not run.
Manual verification: not done. The palette was not opened in alcedo_main by the implementing
  agent; the user must check the cases of the exit criteria.
Screenshots: offscreen grabs of the real SearchPalette.qml with ApplicationModuleHost
  (ALCEDO_SEARCH_PALETTE_SCREENSHOT_DIR, GlobalSearchDialogQmlTest): empty query, field query
  loading, field query previews, field query with no match, filter row, semantic hint,
  rejected semantic Enter, settings panel open, narrow window (900 px) with the panel open.
  The offscreen tests use the software scene graph, which does not run MultiEffect, so the
  thumbnail pixels are not drawn (the frames are empty; the test asserts Image.Ready and
  that each URL resolves in the thumbnail store). A D3D11 run was refused (swap chain access
  denied). The missing-source "!" state shows in the filter-row grab (synthetic DNGs).
  The preview error state was not captured. Files: build/tmp/global_search_gs4/screens/
  (local evidence, not committed).
Evidence path: build/tmp/global_search_gs4/.
Remaining defects or unavailable platforms: manual check in alcedo_main; macOS not built or
  tested.
```

**GS4 follow-up record ([#433](https://github.com/zidage/AlcedoStudio/issues/433),
[#434](https://github.com/zidage/AlcedoStudio/issues/434), 2026-10-17)**

```text
Changes after GS4 (Sections 2.1, 2.3, and 2.4 are updated):
  - The settings side panel is now a drawer inside the palette, below the input row. The palette
    keeps its width (min(window - 56, 960)) and stays horizontally centered. The drawer folds
    with motionFoldOpenMs / motionFoldCloseMs (clip, height, opacity; reduceMotion snaps). The
    open state stays SearchController::searchSettingsPanelOpen. AppTheme.searchSettingsPanelWidth
    is removed.
  - ThemeCheckBox has a disabled state: hairline box, muted check mark, muted label, no parent
    opacity. The drawer shows no searchFieldFiltersReason text.
  - The "Esc" text action is replaced by the close IconActionButton (close.svg). The footer shows
    only the Enter / Search hint. The Escape key still closes the palette.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target GlobalSearchDialogQmlTest alcedo_main                               -> 0
  ctest --test-dir build/debug -R "GlobalSearchDialogQmlTest" --output-on-failure -> 0
Discovered / passed / failed / skipped counts: GlobalSearchDialogQmlTest 10 / 10 / 0 / 0
  (new: CloseButtonClosesPalette).
Screenshots (offscreen, not committed): disabled_field_check_boxes, settings_drawer_open,
  narrow_window_settings_drawer in build/debug/search_palette_shots/.
Manual verification: the GS4 manual check in alcedo_main is still open.
```

### Phase GS5 — Remove the old search dialog

Issue: [#422](https://github.com/zidage/AlcedoStudio/issues/422).

**Objective and deliverables**

- `qml/GlobalSearchDialog.qml` is deleted and removed from `ALCEDO_MAIN_QML_FILES`.
- No source, test, or document refers to the old dialog file, except history records in plans.

**Inputs and prerequisites**

- Phase GS4 complete, with its manual check done.

**Implementation steps**

1. Delete the file and its CMake entry.
2. Search for `GlobalSearchDialog` in `alcedo_studio/src`, `alcedo_studio/tests`, and `docs`.
   Change each reference to the palette file to `SearchPalette`, except completion records. The
   test target name `GlobalSearchDialogQmlTest` can stay.
3. Build `alcedo_main` and `GlobalSearchDialogQmlTest`.

**Tests and evidence**

- `alcedo_main` builds, and the palette opens from the Collections panel (manual check).
- `GlobalSearchDialogQmlTest` results are the same as at the end of GS4.

**Exit criteria**

- [x] A search for `GlobalSearchDialog.qml` has no match outside history records.
- [x] The build passes.

**Expected diff**

About 1250 removed lines and fewer than 30 other changed lines. The phase is a file deletion.
It is separate from GS4 so that GS4 stays below 2000 changed lines.

**Completion record**

```text
Phase / date / status: GS5 / 2026-10-17 / complete. The prerequisite "GS4 manual check done"
  is not met: the user asked for GS3 to GS5 in one pull request, so the manual check of the
  palette stays open for review.
Source revision and branch: feature/search-palette.
Actual changed modules: GlobalSearchDialog.qml deleted; ALCEDO_MAIN_QML_FILES; the
  GlobalSearchDialog contexts of alcedo_main_zh_CN.ts and alcedo_main_en.ts; comments that
  named the old dialog (search_controller.hpp, search_session.hpp,
  automation_search_commands.hpp, the search.query description); the residency plan file
  table (SR3 row now names SearchPalette.qml).
Implemented behavior: no behavior change; the old dialog no longer exists.
Explicitly unimplemented items: none. References in completion records and audits of other
  plans stay as history. The test target name GlobalSearchDialogQmlTest stays.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
    GlobalSearchDialogQmlTest AutomationSearchE2ETest                        -> 0
  ctest --test-dir build/debug -R "^(GlobalSearchDialogQmlTest|AutomationSearchE2ETest)\."
    --output-on-failure                                                      -> 0
Discovered / passed / failed / skipped counts: GlobalSearchDialogQmlTest 9 / 9 / 0 / 0
  (same as GS4); AutomationSearchE2ETest 1 / 1 / 0 / 0.
Manual verification: not done (opening the palette from the Collections panel; see GS4).
Remaining defects or unavailable platforms: macOS not built or tested.
```

## 10. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Nonsense natural-language query | Library shows "No Matching Photos". | GS1, GS2 |
| Clear natural-language query in a large folder | Library shows the matching photos first. | GS1, GS2 |
| Natural-language query in a small subfolder | Matching photos of that folder appear even when other folders have stronger matches. | GS1 |
| Scroll through a long natural-language result | No repeated and no missing photo. | GS1, GS2 |
| Repeat the same natural-language query | No sidecar call for the embedding. | GS1 |
| Runtime start failure on Enter | Library shows the error row with Retry; earlier content stays. | GS2 |
| Too-long query on Enter | Palette shows the error; Library does not change. | GS2, GS4 |
| Select a folder with an active natural-language search | The search is cleared, as for field search. | GS2 |
| Delete the current folder with an active natural-language search | The parent folder shows its kept photos in relevance order; no new embedding. | GS2 |
| Select a sort with an active natural-language search | Same photos in the selected order. | GS2 |
| Field query in the palette | Up to 3 previews; Enter applies the field search. | GS4 |
| Recent search with the switch off | A stored natural-language entry still runs a natural-language search. | GS3 |
| Recommendation row | The stats filter is set; the palette closes. | GS3, GS4 |
| Older project with an HNSW index | Opens; the index is dropped; search works. | GS1 |
| `search.query` | Same fields; real total for natural-language queries. | GS1, GS2 |

## 11. Build and evidence rules

- Presets: `win_debug` for tests; `win_release` with `-DALCEDO_BUILD_TESTS=ON` only for the
  benchmark, then set back to `OFF`. macOS: `macos_debug` with `-DALCEDO_BUILD_TESTS=ON` when a
  macOS check is required.
- Evidence and logs: `build/tmp/global_search_<phase>/`. Remove them when the phase is complete.
- Record each test as passed, failed, skipped, or not run. A skipped test is not a pass. An
  unavailable benchmark is not a zero.
- Do not run the full test suite. Run the named targets and their direct caller suites.

## 12. Risks and stop conditions

| Risk | Signal | Response |
| --- | --- | --- |
| Exact scan is too slow on large folders | GS1 benchmark p50 above 80 ms at 20 000 embeddings | Stop. Update the plan with the measurement. Do not keep HNSW as a silent second path. |
| No constant pair separates nonsense queries from positive queries | GS1 Step 9 | Stop. Report the table and update the relevance rule. |
| The neutral text does not help the chosen model | Calibration shows `r` ranks worse than `c` | Stop. Report the table. Change the rule in the plan before code. |
| Match set join changes the cost of field search | `LibrarySearchBenchmarkTest` field apply regresses | The relevance join must not run when the select is absent. Fix before completion. |
| Cold sidecar start on Enter blocks the GUI thread | Manual check | Known current behavior of `StartAndWait` from the worker. The residency plan fixes it. Record it in the completion record. |
| `search.query` behavior change | `AutomationSearchE2ETest` | Keep the fields. Fix before completion. |

Stop implementation when a required owner API does not exist, when the source audit no longer
matches the branch, when a phase can pass 2000 changed lines, or when an operation needs a
fallback that the user did not authorize. Update this plan before work continues.

## 13. Completion record template

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
Performance or resource evidence, if required:
Calibration constants and table (GS1 only):
Evidence path:
Remaining defects or unavailable platforms:
```
