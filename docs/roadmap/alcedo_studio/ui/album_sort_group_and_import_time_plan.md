# Album Sorting, Grouping, and Import Time Plan

Date: 2026-10-02

Last revised: 2026-10-02 after the user selected direct Inspector controls and allowed the same field for grouping and photo sorting.

Status: Phases 1, 2, and 3 implemented and tested (2026-10-02); the Phase 3 manual visual review in the real application is open.

Source audit revision: `a03728184`. The worktree was clean before this plan was added.

Primary owners: DuckORM, ElementStore, AlbumBrowseService, and LibraryModule.

Parent plan: none. This document is the feature master plan and the detailed phase plan.

## 1. Product decisions

Add sorting and grouping to the library. Make these controls a supplement to Album Inspector.
Use one backend query path for Inspector filters, search, sorting, grouping, and photo pages.
Add import time to filtering, sorting, and date grouping.
Implement the capability from DuckORM through storage and application services to QML.

The user replaced the reference-image menu design with direct Inspector field controls.
Do not add the former field-selection menus or a duplicate toolbar menu.
The image's status, environment, PR, and custom-group controls are not product requirements.

Confirmed interaction requirements:

- Put ascending and descending sort actions beside each Inspector field title.
- Put a grouping checkbox beside those actions.
- Keep at most one active grouping field.
- Keep at most one active photo sort field. It can be the same field as the active grouping field.
- Keep every Inspector filter category available while grouping and sorting are active.
- Use the same backend interfaces for these operations and the existing filters.

The checkbox means GROUP BY. The ascending and descending actions mean ORDER BY.
Use Group as the checkbox label. Do not label a grouping action Order by.

The following details are design proposals selected for this plan. They are not prior user approvals:

- Start with no explicit grouping or sorting. Preserve the existing ascending File ID order in that state.
- Treat an active sort action as a toggle. Activating it again clears the explicit sort.
- Keep grouping and photo sorting independent. Selecting a group preserves the selected sort.
- Group dates by calendar day. Use local calendar days for import time.
- Within date groups, use the full grouping timestamp to resolve equal photo-sort values, then use File ID.
- Preserve active Inspector filters when the user applies or clears a search.
- Combine active filter categories with AND. Preserve the existing toggle behavior within each category.
- Show a multi-label photo in every applicable label group.
- Show only groups that contain matching photos. Keep an explicit unknown group when data is missing.
- Keep presentation settings for each project and album during the current application session.

Do not implement product code as part of plan creation.

## 2. Product design specification

### 2.1 Filter, group, and sort semantics

Let `F` be the unique live files in the current Root or album scope.
Let `P` be the merged search and Inspector predicate.
The matching photo set is `R = {file in F | P(file)}`.
Compute Inspector statistics from `R`, as the current product does.
Do not change these statistics to self-excluding category counts in this feature.

Grouping partitions `R` for single-value fields. Labels create a membership relation instead.
Selecting a grouping field does not add a predicate that chooses one field value.
For example, Camera Model grouping shows every camera present in `R`.
An active camera filter can legitimately leave one camera group.
Clearing a grouping checkbox does not clear that camera filter.
Allow grouping without an explicit photo sort, and sorting without grouping.
When both are active, allow the same field or different fields.
For a date group, sort by the full timestamp inside each calendar-day group.
The day key and full timestamp have different precision even though they belong to one Inspector field.
All filter categories remain independent of those two selections.

Apply the operations in this order:

```text
Root or album scope
  -> search AND Inspector filters
  -> matching unique files
  -> group membership and group counts
  -> group order, then photo order within each group
  -> bounded photo page
```

Use the same field meaning for filtering and grouping.
An unknown camera or lens uses a null key after empty-string normalization.
Do not use a translated label, such as Unknown, as a database key.
Rating 0 means unrated. It does not mean unknown.

### 2.2 Field choices

| Field | Inspector filter | Grouping checkbox | Ascending / descending actions | Database source |
| --- | --- | --- | --- | --- |
| File name | Existing search behavior | No new Inspector section | No new Inspector section | `e.element_name` |
| Capture time | Existing capture-day filter | Capture day | Oldest or newest | `i.capture_at`, `i.capture_date` |
| Import time | New day selection | Import day | Oldest or newest | `e.added_time` |
| Camera model | Existing camera filter | Camera model | Ascending or descending | `i.camera_model` |
| Lens | Existing lens filter | Lens | Ascending or descending | `i.lens` |
| Rating | Existing rating filter | Rating | Lowest or highest | `i.rating` |
| Labels | Existing active-model label filter | All assigned labels | Ascending or descending | `SemanticImageLabel` for the active model |

Labels sorting uses the minimum canonical assigned label key for each file.
It does not discard other labels or change label-group membership.
Keep this same per-file sort key when Labels grouping is active.
Files in one label group can have different minimum assigned keys because they have multiple labels.
The File ID remains the deterministic final order term.
Reuse the existing taxonomy alias rules when normalizing label keys.

Group order follows a fixed field rule. It is independent of the selected photo sort field:

- Capture and import days: newest first.
- Ratings: highest first.
- Cameras, lenses, and labels: ascending by canonical key.
- Unknown remains last.

Do not add a separate group-direction control in this design.
Do not interpret the photo-sort arrows as group-direction controls.

The selected photo sort applies within every group.
For flat mode, it applies across all matching photos.
Always use explicit null placement and a final ascending file ID comparison.
The comparison sequence is group key, selected photo-sort value if present, a more precise grouping value if needed, then File ID.
For capture-day grouping with Capture time sorting, use capture_date for the group and capture_at for the photo order.
For import-day grouping with Import time sorting, use the converted local day for the group and the original UTC added_time for the photo order.
Do not truncate the timestamp used for photo sorting to a day.
When another field supplies the photo sort, equal sort values within a date group use that group's full timestamp, newest first.
For example, equal ratings within a capture-day group use capture_at DESC, then File ID ASC.
When the selected photo sort already uses that timestamp, emit the timestamp comparison only once, with the selected direction.
This secondary comparison is derived from the date group. It does not create a second user-selected sort field or direction setting.
For exact-value scalar groups, such as camera, lens, or rating, the same-field sort value is constant within a group.
Allow that selection. The final File ID comparison then determines the order within those tied values.
Do not add another user-selected sort field to resolve ties.
When no explicit photo sort is active, date groups use their full timestamp newest first, then File ID.
Other groups and flat mode use ascending File ID when there is no explicit photo sort.
These are defined presentation orders. They are not additional user-selected sort fields.
String order uses DuckDB's deterministic binary comparison in this release.
String-sort tooltips explain ascending or descending key order.
They must not promise locale-aware or natural-number ordering.

### 2.2.1 Exclusive choices and owner updates

The header checkboxes are an optional exclusive choice.
Unlike a required radio selection, all grouping checkboxes can be unchecked.
The sort arrows form another optional exclusive choice across all field headers.
Do not keep a separate checked or selected owner for each section.
LibraryModule validates the prospective choices before starting SQL.

| User action | Required result |
| --- | --- |
| Check a field's Group checkbox | Select that grouping field and uncheck the previous grouping field. |
| Uncheck the current grouping field | Return to flat mode and preserve any active photo sort. |
| Check the currently sorted field for grouping | Select the new group and preserve that field's active sort and direction. |
| Activate Ascending or Descending on another field | Select that one sort field and direction; clear the previous sort indication. |
| Activate the already active sort action | Clear the explicit photo sort and use the defined presentation order for the current grouping mode. |
| Activate the other direction on the active sort field | Change its direction without changing the group or filters. |
| Activate Ascending or Descending on the active grouping field | Select that photo sort and direction. Keep the group selected and keep its fixed section order. |
| Change any filter, including the grouped or sorted field | Apply that condition while preserving valid group and sort choices. |

Validate against accepted choices plus pending focused changes.
Rapid actions retain pending group and sort choices, including choices on the same field.
Group replacement forms one logically indivisible owner operation and does not clear sorting.
Show both accepted indications after success. Do not silently choose another sort field.
Return validation errors through the same explicit error surface as other invalid query inputs.

### 2.3 Import time

Import time means the creation time of the logical File in this project.
Use `SleeveElement::added_time_` and the existing `Element.added_time` column.
Adding an existing file to another album does not change import time.
Metadata refresh, rating changes, project reopen, and thumbnail loading do not change it.
A new successful import creates a new File and therefore a new import time.
Failed imports retain the current cleanup behavior and do not appear in query results.

The existing mapper writes UTC calendar text into a plain DuckDB TIMESTAMP.
Treat that column as UTC by application convention.
Fix its read conversion so UTC text returns the same instant.
Use thread-safe time conversion. Do not use shared `std::gmtime` storage on concurrent paths.
Do not rewrite existing timestamp rows merely because this feature reads them.
Record any evidence of previously rewritten times before proposing a data repair.

Import-day filtering uses a half-open interval:

```sql
e.added_time >= CAST(? AS TIMESTAMP)
AND e.added_time < CAST(? AS TIMESTAMP)
```

Compute both UTC boundaries from the chosen local day and the same query time zone.
Compute the next local midnight independently. A daylight-saving day is not always 24 hours.
Do not use an inclusive BETWEEN expression for day boundaries.
Do not wrap `e.added_time` in a date function inside the range predicate.

Import-day grouping converts UTC to the selected local calendar day before grouping.
Use one explicit IANA time zone for the read operation.
Verify the timezone SQL and ICU availability on the linked DuckDB version.
A missing required timezone capability reports an error. It does not silently use UTC days.
Do not change connection-global timezone settings shared with other operations.
Capture dates keep their current EXIF calendar meaning. Do not reinterpret them as UTC instants.

Read import time through the SQL page projection.
Populate the existing `AlbumItem.import_date` and `AlbumThumbnailModel::ImportDate` role from that result.
Remove `QDate::currentDate()` as the source of a photo's import date.
Missing import time remains missing and appears in the unknown group.

### 2.4 Direct Inspector controls

```text
Album Inspector
LIBRARY OVERVIEW
Total Photos                                              1,240

By Capture Date                    [Ascending] [Descending] [ ] Group
[existing capture-date filter]

By Import Time                     [Ascending] [Descending*] [x] Group
[new import-date filter]

By Camera Model                    [Ascending] [Descending] [ ] Group
[existing camera filter]

By Labels                          [Ascending] [Descending] [ ] Group
[existing label filter]

Rating                             [Ascending] [Descending] [ ] Group
[existing rating filter]

By Lens                            [Ascending] [Descending] [ ] Group
[existing lens filter]

* The marked sort action is the only active photo sort.

Library photo area
2026-10-02                                            420 photos
[photo] [photo] [photo] [photo] [photo] [photo]
[photo] [photo] [photo] [photo] [photo] [photo]

2026-10-01                                            790 photos
[photo] [photo] [photo] [photo] [photo] [photo]

Unknown                                                30 photos
[photo] [photo] [photo] [photo] [photo] [photo]
```

Each field header invokes the same focused LibraryModule operations.
Bind arrow selection and checkbox state to its accepted settings.
QML does not keep another copy of those settings.
Use compact up/down text actions with full accessible names and tooltips.
The layout above spells out their meaning, not their final pixel width.
Use ThemeCheckBox for Group and existing Basic button behavior for sort actions.
Keep section expansion separate from sorting and grouping.
Activating a header action must not toggle section expansion or a filter bucket.
Keep the actions available when their filter section is collapsed.
Keep the grouped field's sort actions and filter choices enabled.
Date-sort tooltips explain that the full timestamp orders photos within each day when that field is grouped.
When date grouping and another photo sort are active, explain that equal sort values use the grouping timestamp newest first.
For exact-value scalar groups, explain that equal values use File ID order.
When Inspector is closed, use the existing Inspector toggle to access these controls.
Do not add a second sort/group control surface to the toolbar.
Use explicit text actions for Expand all and Collapse all in grouped mode.
Collapsing a group changes presentation only. It does not add a filter or change photo counts.

Group headers contain separate title, count, and disclosure roles.
Do not add new pills, status dots, colored selection frames, or compound title/count strings.
Use the existing Basic style and AppTheme tokens.
Use neutral selected rows and a visible neutral keyboard focus treatment.
Use text glyphs for the requested sort arrows. This design requires no new SVG asset.
Do not invent or assign a new SVG during implementation without the approval required by DESIGN.md.

The import filter reuses DateFilterSection and its calendar/activity choices.
Give it independent day-selection and view-style state from the capture filter.
Share the renderer, not the selected value.
New strings use `qsTr` or the existing C++ localization helpers.
Update the existing translation catalogs and `docs/VI/README.md` in the UI phase.

### 2.5 Selection, focus, and navigation

Keep photo selection keyed by File ID across sorting, grouping, and section collapse.
Remove selected IDs only when filtering, deletion, or an album change removes them from the result.
Do not select section headers as photos.
Arrow keys move through photo cells. Header controls use normal Tab focus.
Shift selection follows the visible expanded photo order and skips headers.
Ctrl+A selects unique files in the current filtered result, including collapsed sections.
Run ID-only reads for unloaded ranges. Do not load thumbnail metadata to select a range.

Duplicate label occurrences share selection, rating, thumbnail state, and export identity.
Deletion, export, drag operations, and edit opening deduplicate by File ID.
Thumbnail visibility uses an occurrence count per photo and resolution tier.
Release the underlying pin only when the last visible occurrence releases it.
Use the stable `(group_key, file_id)` occurrence identity for idempotent visibility registration.
Repeated visibility notifications must not increment a count twice.

Sorting and grouping preserve the focused File ID when it remains in the result.
Restore its section and position with the same SQL ordering used by page reads.
Do not load every earlier page to find its position.
If the focused photo no longer matches, focus the first remaining photo.
Keep the current selection behavior for editor entry and return.
The editor filmstrip reads the same ordered unique photo result without section headers.
For label groups, its order uses each file's first occurrence in the ordered group stream.
Do not add a separate filmstrip query interpretation.

### 2.6 Loading, empty, error, and narrow-window states

Keep the last accepted view visible during a query and mark it as updating.
Do not publish new group counts beside pages from a different accepted read.
On first load, show the existing skeleton treatment until the first result arrives.
Publish a successful zero result as No matching photos.
Show a database failure as an explicit error with Retry. Do not publish an empty success result.
Retain the accepted filter and presentation settings after failure.
Keep pending changes in the existing request path until success or failure.
Do not write a copied old AlbumViewState over live state during recovery.

During an album or project transition, clear content from the previous scope before showing the new scope.
Report transition failure in the new scope. Do not show another album's photos as its result.
At narrow widths, wrap Inspector titles and move their complete action row below the title when necessary.
Keep sort actions and the Group label readable. Do not clip checkbox hit areas.
Clamp the photo column count to at least one.
Use fixed, documented photo-row and header geometry for reliable scroll calculations.
Give long group keys a single-line elided title and an accessible full name.

## 3. Scope and related work

| Module | Responsibility in this feature |
| --- | --- |
| DuckORM | Generic SELECT clauses, grouping, ordering, pagination, and bind order |
| Sleeve filter compiler | Import-day predicates and domain field meanings |
| ElementStore | Root/album scope, matching files, memberships, SQL results, and operation-scoped reads |
| AlbumBrowseService | One application API for flat and grouped library reads |
| SleeveFilterService | Existing search predicate construction and compatibility callers |
| LibraryModule | Accepted query settings, async refresh, page publication, and section presentation |
| StatsEngine | Existing filter values and Inspector statistics presentation |
| SearchController | Existing search UI and preview; applied searches enter the unified library path |
| AlbumThumbnailModel and AlbumViewState | Existing photo data and update APIs |
| QML library views | Inspector field actions, headers, rows, keyboard navigation, and viewport visibility |

This plan extends these verified implementation areas:

- [DuckORM expressions and album SQL](../storage/duckorm_query_expression_and_album_filter_sql_plan.md): reuse SqlFragment and prepared execution.
- [Library search and project size](../storage/library_search_and_project_size_plan.md): preserve typed search columns and the single-evaluation read result.
- [Unified workspace](qml_editor_rhi_unified_workspace_plan.md): preserve LibraryModule and application-service boundaries.
- [Semantic generation and search](../ai/semantic_generation_search_plan.md): reuse active-model labels and existing alias rules.
- [Album membership notes](../../../refactor/2026-05-25-sleeve-album-membership-filesystem-plan.md): preserve one File identity across album links.

Do not introduce a new ORM, a second filter compiler, custom groups, nested grouping, or persistent user-defined views.
Folder grouping is not required by the supplied image or the revised field-control design.
Do not reinterpret album membership as a file's unique parent folder.
Do not change RAW quality, render backends, search ranking, or import success policy.
No persistent schema column is needed for import time.
Do not add indexes or change project format as an unmeasured default.

## 4. Current source audit

Paths in this section are relative to the repository root.

| Area | Current source and owner | Verified behavior | Required change |
| --- | --- | --- | --- |
| Generic SQL | `alcedo_studio/src/include/storage/mapper/duckorm/duckdb_expr.hpp`; `duckdb_orm.hpp` | SqlFragment carries SQL and binds. Expressions and prepared execution exist. Full scoped SELECT clauses use manual strings. | Add generic clause composition without album field knowledge. |
| Scope | `alcedo_studio/src/storage/store/sleeve/element_store.cpp`, `BuildScopedFileQuery` | Root reads all File rows. Albums join FolderContent. Shared aliases are `e`, `fi`, and `i`. | Reuse this exact scope for every read variant. |
| Paging | Same file, `ListFilesInFolderPage`, `ListSearchResultPage` | Fixed `ORDER BY e.id`; LIMIT/OFFSET. Several failure paths return empty or zero. | Add explicit ordering and preserve SQL errors. |
| Statistics | Same file, `ReadMatchSet`, `ReadMatchSetBuckets` | One temporary SearchMatchSet evaluates the filter. GROUPING SETS reads scalar fields. Labels use COUNT DISTINCT for the active model. The table is dropped after the operation. | Reuse this representation. Add import data and group results. |
| Import persistence | `alcedo_studio/src/include/storage/store/database.hpp`; `storage/mapper/sleeve/element/element_mapper.cpp`; `sleeve/sleeve_element/sleeve_element.cpp` | Element already contains added_time TIMESTAMP. Construction sets time. ToParams writes UTC; FromParams uses local mktime. | Fix UTC round-trip and read the existing column. |
| Typed fields | `alcedo_studio/src/include/storage/mapper/image/image_mapper.hpp`; `storage/mapper/image/image_search_columns.cpp` | Capture timestamps, dates, camera, lens, and rating are typed columns. | Reuse them. Avoid metadata JSON reads. |
| Filter fields | `alcedo_studio/src/sleeve/sleeve_filter/filter_sql.cpp` | ImportDate already maps to `e.added_time`. No Inspector import bucket factory exists. | Add validated local-day boundaries and unknown handling. |
| App listing | `alcedo_studio/src/app/album_browse_service.cpp` | Paged listing and count forward to ElementStore. Both hide errors. | Add one throwing query API for the new path. |
| Library query | `alcedo_studio/src/ui/alcedo_main/album_backend/library_module.cpp` | Merges stats and search filters. Reads count and page synchronously. Pages are 1000 for albums or 120 for search. Focus lookup reads all IDs. | Route refresh and paging through the shared async query owner. |
| Import display | Same file, `AddOrUpdateAlbumItem` | Missing import_date becomes QDate::currentDate(). Photo metadata reads ImagePool per row. | Read import time and display fields from bounded SQL results. |
| Inspector | `qml/AlbumInspectorPanel.qml`; `album_backend/stats_engine.cpp` under `alcedo_main` | Capture, camera, label, rating, and lens filters exist. Stats and grid share a merged predicate. Stats query errors retain old values silently. | Add import filtering and direct field actions. Report failures. |
| Field headers | `qml/StatsCard.qml`, `DateFilterSection.qml`, and `StarRatingFilter.qml` under `alcedo_main` | Each component paints its own title. Some also own section expansion. They have no shared header-action slot. | Add an optional header-action slot without coupling filter selection or expansion to sort/group actions. |
| Applied search | `album_backend/search_controller.cpp` | Search apply clears Inspector filters. It publishes page and stats from an async read. | Preserve filters and enter the same library query path. |
| Worker | `album_backend/search_request_worker.cpp` | One worker supports preview and apply requests. It already removes superseded pending work and checks requests before UI publication. | Reuse its ordering and publication mechanism. Do not add another counter. |
| Photo model | `include/ui/alcedo_main/album_backend/album_thumbnail_model.hpp`; `album_types.hpp` | ImportDate role exists. Photo rows appear in both AlbumViewState and AlbumThumbnailModel. ID lookup assumes one photo row per file. | Add a section projection of IDs and positions. Do not add another photo buffer. |
| Photo view | `qml/ThumbnailGridView.qml` | Flat GridView, zoom, selection, visibility pins, and scrolling assume flat indices. | Add grouped row layout and preserve existing actions. |
| Store lifetime | `include/storage/store/sleeve/element_store.hpp`; `sleeve/storage.cpp` | ElementStore currently owns ConnectionGuard for its lifetime. Database supplies it during Storage construction. | Bring the touched owner into the current operation-boundary connection rule. |

Existing test targets include DuckormExprTest, DuckormStatementTest, SleeveFilterCompileTest,
SleeveFilterFactoryTest, FilterServiceTest, LibrarySearchColumnsTest, AlbumBackendStatsFilterTest,
AlbumBackendSearchWorkerTest, AlbumBackendThumbnailTest, AlbumBackendRatingTest, and AlbumBackendImportTest.

DuckORM tests are registered in `alcedo_studio/tests/sleeve/CMakeLists.txt`.
Application tests are registered in `alcedo_studio/tests/app/CMakeLists.txt`.
UI owner tests are registered in `alcedo_studio/tests/ui/CMakeLists.txt`.
The existing synthetic library support can create persisted photos with typed metadata without RAW decoding.
Extend that support instead of constructing an isolated SQL schema that differs from the product.
Use the production import path separately for the import-time persistence regression.

## 5. Target owners and APIs

All API names below are proposed unless the source audit identifies an existing name.

### 5.1 DuckORM SELECT composition

Add `duckdb_select.hpp` and `duckdb_select.cpp` beside the existing expression and ORM files.
Register the source on the existing DuckORM target in `src/storage/CMakeLists.txt`.

Use SqlFragment for each expression and retain declaration-order binds.
Support projection, FROM/JOIN fragments, WHERE, GROUP BY, GROUPING SETS, ORDER BY, and LIMIT/OFFSET.
Support fixed aliases, DISTINCT projections, and COUNT expressions needed by membership queries.
Use small templates or shared clause functions. Keep the existing Mapper CRTP path for table CRUD.
The builder is a statement description, not a copy of database or UI state.

Proposed generic operations:

| API | Input and output | Validation and failure |
| --- | --- | --- |
| `select_query` | Projection and trusted FROM/JOIN fragments -> SqlFragment | Require a nonempty projection and source. |
| `group_by` / `grouping_sets` | Expression spans -> clause fragment | Require valid nonempty sets, except the explicit total set. |
| `order_by` | Expression, direction enum, null-placement enum -> clause | Reject unknown enums. Never accept a QML SQL string. |
| `limit_offset` | Positive bounded limit and nonnegative offset -> bound clause | Reject overflow and a zero UI page limit. |
| Existing `execute_query` / `select_by_query` | Complete SqlFragment -> result | Preserve bind and execution errors. |

Do not add a general SQL parser or a query AST for unused syntax.
Use trusted fixed SQL fragments for domain-specific timezone or label subqueries.
Keep their values bound. Keep those fragments below the application API.

### 5.2 Domain query options

Add a proposed `sleeve/album_query.hpp` for minimal enums and scalar options.
Define AlbumSortField, AlbumGroupField, SortDirection, and AlbumQueryOptions there.
Options contain sort field, sort direction, group field, and import-day time zone.
Both field enums support None. Sort direction has no effect when the sort field is None.
Derive group order from the field rules in Section 2.2. Do not keep a second group-direction setting.
Accept equal group and sort fields. Validate field enums and directions independently.
Map the grouping key and photo-sort expression separately, including their different precision for dates.
Build one shared comparison definition with the derived full-timestamp tie comparison for date groups.
Do not store this derived comparison as another mutable sort setting.
Do not put a second FilterNode, folder model, photo vector, or selection map inside options.
Pass the existing filter by const reference during synchronous service calls.
The UI owner holds one accepted set of presentation options.
Apply option changes through focused setters or a minimal validated change description.

Proposed LibraryModule operations are `ToggleInspectorSort(field, direction)` and `SetInspectorGrouping(field, enabled)`.
The sort operation implements direction selection, field replacement, and clearing the active action.
The grouping operation implements checkbox replacement atomically and preserves the photo sort.
Neither operation changes filters.
Both operations combine pending changes before validating and submitting the next read.

Map domain enums to SQL expressions in proposed `storage/store/sleeve/album_query.cpp`.
ElementStore owns that read implementation. DuckORM must not know album enums or table meaning.
Reuse FilterSQLCompiler for the merged predicate.
Reuse canonical bucket rules for camera, lens, rating, dates, and labels.

### 5.3 Unified application read

Extend AlbumBrowseService with these proposed operations:

```text
ReadAlbumQuery(folder_id, filter, options, active_model_key, offset, limit, read_statistics)
  -> bounded existing photo-row result, unique photo total,
     group descriptors when requested, and existing AlbumStatsView when requested

ReadAlbumFilePosition(folder_id, filter, options, active_model_key, file_id, preferred_group_key)
  -> optional photo occurrence position and group key

ReadAlbumFileIds(folder_id, filter, options, active_model_key, occurrence_range)
  -> ordered unique file IDs for selection or caller navigation
```

Extend SearchResultRow with only required import, sort, and display values.
Reuse existing page and statistics result types rather than adding another full photo representation.
Add minimal group descriptors and membership positions to the query result where needed.
A descriptor contains a typed key, photo count, and starting occurrence offset.
It does not contain every photo in the group.
Use signed 64-bit database counts. Check conversions to Qt model integers.
Read the active model key from its existing owner when building the request.
Pass that same immutable key to predicates, statistics, label memberships, and label sorting.
Do not read a different active model independently in each helper.

The initial read returns the first page, all group descriptors, unique count, and Inspector statistics.
Paging reads return bounded photos and occurrence keys with the same query interpretation.
Do not recalculate every Inspector statistic during each scroll page.
Position reads use SQL comparison/ranking and return scalar positions.
They must not return all IDs merely to find one file.

The facade validates options and page limits before execution.
It acquires the storage owner through the existing service boundary.
It reports invalid input and DuckDB errors explicitly.
It does not return a successful empty page on failure.
Retain unrelated compatibility callers where necessary. Route changed library callers to this one API.

### 5.4 Connection, transaction, and temporary-result lifetime

Replace ElementStore's long-lived ConnectionGuard with access to its Database owner.
Request a connection at each public storage operation boundary.
Pass that operation's connection to private helpers.
Preserve cross-controller serialization through the existing database lock.
Update Storage construction and the direct ElementStore test callers together.
Do not extend this phase into an unrelated rewrite of every Store.

Reuse the existing SearchMatchSet representation for the initial query operation.
Add only the columns required by its page, statistics, and selected ordering.
Include the full capture timestamp for capture sorting and added_time for import behavior.
Avoid reading or copying Image.metadata, pixel buffers, or full SleeveElement objects.

This operation needs independent SQL rows because pages, counts, and buckets require one predicate evaluation.
A const view of live tables cannot satisfy that requirement across several statements.
The captured fields are read-only query outputs owned by the operation's ElementStore read.
Hold the connection lock and a read transaction through matching, label reads, and result construction.
Drop the existing temporary result before releasing the connection.
Reuse the existing RAII cleanup behavior after both success and failure.
Do not retain the table across user interaction or create another long-lived result cache.

An open library is a live view. It is not a frozen point-in-time library export.
After import sync, membership changes, rating changes, deletion, or active-model changes, request a full query refresh.
Use existing owner notifications and the query worker publication path.
Replace group descriptors and the first page together.
Pending page work for the old accepted result must not append after that refresh.
Do not add a database version or revision counter without a real failing interleaving.

### 5.5 Label memberships and identity

For scalar grouping, produce one membership per matching File ID.
For label grouping, produce one membership per canonical `(label_key, file_id)` pair.
Restrict labels to the current active semantic model.
Deduplicate alias-equivalent assignments before counting or paging.
Use an explicit unlabelled group for files with no qualifying label.
Do not let inactive-model assignments exclude a file from the unlabelled group.

The unique photo total comes from matching files before label expansion.
The sum of label-group counts can exceed that total.
Name each group count Photos. Do not present the sum as the number of unique library photos.
Reuse the same canonical label relation for Inspector buckets and grouped pages.
Keep translated display labels in the UI presentation layer.

Illustrative SQL for a scalar field is below. The real builder supplies field expressions and binds.

```sql
-- SearchMatchSet already contains the scoped, filtered typed rows.
SELECT group_key, COUNT(*) AS photo_count
FROM (
    SELECT file_id, NULLIF(camera_model, '') AS group_key
    FROM SearchMatchSet
)
GROUP BY group_key
ORDER BY group_key ASC NULLS LAST;

SELECT file_id, image_id, file_name, added_time,
       NULLIF(camera_model, '') AS group_key
FROM SearchMatchSet
ORDER BY group_key ASC NULLS LAST,
         added_time DESC NULLS LAST, file_id ASC
LIMIT ? OFFSET ?;
```

This second statement returns photos from every group in one ordered stream.
It is not `WHERE camera_model = selected_group`.
Page boundaries can split a group. The group descriptor supplies its full count and identity.
The section model must not create another header merely because a page begins inside that group.

For Capture day grouping and ascending Capture time sorting, the photo stream uses:

```sql
SELECT file_id, image_id, file_name, capture_date AS group_key, capture_at
FROM SearchMatchSet
ORDER BY group_key DESC NULLS LAST,
         capture_at ASC NULLS LAST, file_id ASC
LIMIT ? OFFSET ?;
```

The first order term keeps each day contiguous and keeps sections newest first.
The second term orders photos by their full time within that day.
The final term resolves equal timestamps, including timestamps with insufficient source precision.
Use the same comparison terms for page reads, focus-position reads, and ordered selection IDs.

For Capture day grouping and descending Rating sorting, use:

```sql
ORDER BY capture_date DESC NULLS LAST,
         rating DESC NULLS LAST,
         capture_at DESC NULLS LAST, file_id ASC
```

The full capture time resolves equal ratings within a day.
For import-day grouping, use added_time for that secondary comparison.
Do not repeat the full timestamp term when it is already the selected photo-sort expression.

### 5.6 Async owner and publication

LibraryModule owns accepted query settings and model publication on the GUI thread.
StatsEngine owns Inspector filter values. SearchController owns search text and preview state.
Both controllers request a LibraryModule refresh instead of publishing an independent album page.
The refresh composes their existing filters once.
Search apply no longer calls ClearFilters.
Clear search removes only the search term. Clear filters removes only Inspector terms.
Compose replacement requests from accepted values plus all pending focused changes.
For example, checking a camera group before a pending import-time sort finishes must retain both changes.
Checking an import-time group instead must also retain the pending import-time sort and its direction.
Keep only the pending change description until publication. Do not create another complete mutable query owner.
Remove accepted pending changes after success. Retain a failed change description only for explicit Retry.

Reuse SearchRequestWorker's existing execution and latest-request check.
Move its applied-library scheduling responsibility to LibraryModule, or extract that responsibility from SearchController.
Keep search-preview behavior on the same established worker implementation.
Do not create a second query executor and a second publication counter for this feature.
Page reads share the applied-library ordering domain and are coalesced by required viewport range.

A worker request needs immutable inputs after the GUI call returns.
Capture only the filter expression, scope ID, option scalars, requested range, and retained service lifetime.
This is required message input across the thread boundary, not a copied AlbumViewState.
Do not capture photo vectors, QObject state, or mutable references to GUI owners.
Move query outputs once into the queued GUI completion.
Validate the existing request identity before publishing.
Publish photo rows, section descriptors, settings, and statistics before their notifications become observable.
Begin required model reset operations while every owner still exposes the old accepted result.
Install all new owner data before ending those reset operations or emitting property notifications.
Keep required QAbstractItemModel structural signals. Do not suppress them with QSignalBlocker.
Do not rely only on the GUI thread being single-threaded to prevent partial publication.

The production reordering boundary already exists:

```text
GUI submits applied read A
  -> worker starts SQL A
GUI changes filter or sort to B while A runs
  -> the same worker accepts B and marks A superseded
worker posts completion A with QMetaObject::invokeMethod(..., QueuedConnection)
  -> GUI rejects A through the existing IsCurrent check
worker completes B
  -> GUI publishes B
```

Extend the existing delayed-worker tests to drive this exact path with sort, grouping, and album changes.
Also test a page completion queued before a new initial refresh.
Do not invent additional consistency mechanisms for hypothetical worker parallelism.

### 5.7 Section presentation and virtualization

Add proposed AlbumSectionModel as a projection owned by LibraryModule.
Keep photo data in the existing catalog/model owner. The projection stores IDs and occurrence positions only.
Do not add `vector<AlbumItem>` per group or another map of copied photo metadata.
When touching the current duplicate model/cache path, use owner operations and scoped reads.
Do not add another synchronization copy between AlbumViewState and AlbumThumbnailModel.

Use one vertical ListView for grouped mode.
Each logical row is a group header or one horizontal row of photo cells.
A row creates at most the current column count of photo cells.
Use `reuseItems` and a viewport-based cacheBuffer.
Handle `ListView.pooled` and `ListView.reused` to release and rebind visible photo occurrences.
Keep selection and collapse state in the owner, not in recycled QML items.

Store one descriptor per group and prefix row counts.
For `n` photos and `c` columns, an expanded group contributes `1 + ceil(n / c)` logical rows.
A collapsed group contributes one row.
Derive the logical row at a requested index through prefix lookup.
Do not allocate an object for every unloaded photo row.
Derive pixel positions from the documented header height and photo-row height.
Check scrollbar jumps against that exact geometry. Do not assume ListView's estimated average row height is exact.
Translate visible row ranges to contiguous occurrence ranges and coalesce adjacent requests.
Use the existing metadata page limits, with a maximum of 1000 rows per database read.
Retain bounded page ranges near the viewport instead of loading preceding pages for a distant section.
Record loaded-row counts while scrolling through a large library.

The grouped view shares photo-cell actions and rendering with ThumbnailGridView.
Extract the current cell into a reusable component only where the section view needs it.
Keep flat mode on the existing flat view with the same backend ordering.
Do not run one full GridView or one full Repeater per group.
Do not use GridView cells as full-width section headers. GridView uses uniform cell geometry.

Zoom recalculates row geometry from counts. It does not query metadata again solely for column changes.
Keep the current thumbnail resolution tiers and quality policy.
Restore the viewport from `(file_id, preferred_group_key, vertical_offset)` scalar values.
For a group removed by filtering, select the next available scroll position deterministically.

## 6. DuckDB efficiency requirements

These are design requirements, not measurements of the unimplemented feature.

1. Read typed columns. Never extract EXIF JSON to sort, count, or group photos.
2. Push scope and filter predicates into the matching-file read before membership expansion.
3. Keep page projections narrow. Do not use SELECT * for multi-table album queries.
4. Retain GROUPING SETS for scalar Inspector buckets and add import-day buckets there.
5. Keep label expansion separate from scalar buckets so labels cannot multiply camera or date counts.
6. Use one ordered membership stream for pages. Do not run a photo-page query for every group header.
7. Keep explicit ordering on every page and position read. GROUP BY does not establish display order.
8. Use LIMIT with the bounded page read. Check whether the deployed version selects TOP_N.
9. Treat deep OFFSET as work, not constant-time access. Measure beginning, middle, and final pages.
10. Do not claim that an ART index makes ORDER BY or GROUP BY efficient.
11. Preserve selective timestamp predicates that can benefit from existing column statistics and zonemaps.
12. Run blocking SQL on the worker. Do not raise global DuckDB thread or memory settings to hide a poor query.
13. Do not build LIST or STRING_AGG values containing every photo in each group.
14. Measure the initial temporary-result cost. Sorting and grouping can spill and consume substantial memory.

LIMIT/OFFSET is the explicit random-access page protocol for this design.
It preserves the current storage protocol and supports direct jumps to section positions.
It does not promise constant-time deep pages.
If deep-page latency fails the measured budget, revise the SQL access design before completing Phase 2.
Do not silently reduce page quality, skip groups, or move sorting into QML.
A retained ordered ID result or a seek-page API needs its own measured justification and updated lifetime specification.

Use deterministic synthetic libraries with 10,000 and 100,000 files.
Include many repeated timestamps, unknown fields, uneven groups, and overlapping labels.
Include a large group and many small groups.
Run the same query cases on Root and a small album subset.
Use metadata-only data for scale checks. Do not decode RAW pixels in SQL benchmarks.

Record the linked `SELECT version()`, CPU, RAM, build mode, thread settings, and result sizes.
Record cold first reads separately from repeated warm reads.
Record p50 and p95 for initial reads, section changes, shallow pages, deep pages, and focus lookup.
Record database lock wait, query duration, GUI publication duration, retained metadata rows, and visible cells.
Use EXPLAIN ANALYZE to record operators, scan cardinality, and filter placement.
Do not add parallel operator times together as if they equal wall time.

Proposed engineering budgets on the development machine:

| Scenario | Initial qualification target |
| --- | --- |
| 100,000 files, scalar grouping, warm initial result | p95 at or below 500 ms |
| 100,000 files, scalar page of at most 1000 rows | p95 at or below 150 ms, including deep pages |
| 100,000 files, overlapping labels, warm initial result | p95 at or below 750 ms |
| SQL execution on the GUI thread | None |
| GUI publication of one bounded page | p95 at or below 16 ms |
| QML cells and metadata retained during distant scroll | Bounded by documented viewport and page policy |

These budgets are proposals. They are not observed performance or user-approved hardware promises.
Record a failed budget honestly. Diagnose the plan or implementation before changing a budget.

## 7. File and build map

Existing files that change, relative to `alcedo_studio/src/`:

- `include/storage/mapper/duckorm/duckdb_expr.hpp` and `storage/mapper/duckorm/duckdb_expr.cpp`, only for missing generic expression support.
- `include/storage/store/sleeve/element_store.hpp` and `storage/store/sleeve/element_store.cpp` for read ownership and shared entrypoints.
- `include/storage/mapper/sleeve/element/element_mapper.hpp` and `storage/mapper/sleeve/element/element_mapper.cpp` for explicit UTC conversion.
- `include/sleeve/sleeve_filter/filter_factory.hpp`, `sleeve/sleeve_filter/filter_factory.cpp`, and `sleeve/sleeve_filter/filter_sql.cpp` for import-time predicates.
- `include/sleeve/storage.hpp` and `sleeve/storage.cpp` for ElementStore construction.
- `include/app/album_browse_service.hpp` and `app/album_browse_service.cpp` for the unified facade.
- `include/app/sleeve_filter_service.hpp` and `app/sleeve_filter_service.cpp` for shared stats results and compatibility routing.
- `include/ui/alcedo_main/album_backend/library_module.hpp` and its `.cpp` for query ownership and publication.
- `include/ui/alcedo_main/album_backend/stats_engine.hpp` and its `.cpp` for import filter state and accepted stats.
- `include/ui/alcedo_main/album_backend/search_controller.hpp` and its `.cpp` for search composition and scheduling responsibility.
- `include/ui/alcedo_main/album_backend/search_request_worker.hpp` and its `.cpp`, only for the shared applied-read scheduling changes.
- `include/ui/alcedo_main/album_backend/album_types.hpp`, `album_thumbnail_model.hpp`, and `album_catalog.hpp`, plus their callers as required.
- `ui/alcedo_main/album_backend/album_thumbnail_model.cpp` and `thumbnail_manager.cpp` for owner reads and duplicate-occurrence visibility.
- `ui/alcedo_main/qml/AlbumInspectorPanel.qml`, `DateFilterSection.qml`, `StatsCard.qml`, `StarRatingFilter.qml`, `LibraryWorkspace.qml`, and `ThumbnailGridView.qml`.
- `storage/CMakeLists.txt`, `ui/alcedo_main/CMakeLists.txt`, and the affected test CMake files.

Proposed files:

- `include/storage/mapper/duckorm/duckdb_select.hpp` and `storage/mapper/duckorm/duckdb_select.cpp`.
- `include/sleeve/album_query.hpp` and `storage/store/sleeve/album_query.cpp`.
- `include/ui/alcedo_main/album_backend/album_section_model.hpp` and `ui/alcedo_main/album_backend/album_section_model.cpp`.
- `ui/alcedo_main/qml/InspectorFieldActions.qml`, `AlbumSectionView.qml`, and a shared photo-cell component if extraction is necessary.
- `alcedo_studio/tests/app/album_query_test.cpp` and `alcedo_studio/tests/ui/album_section_model_test.cpp`, relative to the repository root.

Use existing test targets for DuckORM and filter changes where their responsibility already matches.
Proposed new targets are AlbumQueryTest and AlbumSectionModelTest.
Register new QML in ALCEDO_MAIN_QML_FILES and new C++ in AlbumBackendLib.
Register `storage/store/sleeve/album_query.cpp` in STORAGE_SRCS on the existing Storage target.
Update AppTheme, DESIGN.md, and the per-file VI entry together if new tokens are required.
Do not reformat entire existing source files as part of a feature edit.

## 8. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |
| 1 | DuckORM SELECT support and storage query semantics, including real import time | DuckORM, filter factories, ElementStore, mapper, storage tests | Current source audit | 1400-1850 lines | Complete |
| 2 | One application query path and tested section projection | App services, LibraryModule, existing worker, models, owner tests | Phase 1 | 1500-1900 lines | Complete |
| 3 | Direct Inspector field actions, import filtering, and virtualized album sections | QML, theme documentation, translations, integration checks | Phase 2 | 1200-1800 lines | Implemented; manual review open |

The estimates include code, tests, build registration, resources, and documentation changed by each phase.
They exclude generated files and temporary evidence.
Phase 2 prepares section geometry before Phase 3 renders it. This keeps the UI phase reviewable.
If source changes make an estimate exceed 2000 lines, split that phase before implementation.
Keep the three product stages. Add a named subphase only when the measured diff requires it.

Before each phase, read AGENTS.md and the applicable skills again.
Use alcedo-msvc-cmake for Windows commands. Use alcedo-qml-ui and qt-qml for the UI work.
Use execute-phase-plan when the user requests phase execution.

## 9. Detailed phases

### Phase 1 - SQL operations and storage query semantics

**Objective and deliverables**

Deliver reusable SELECT clauses and real SQL ordering and grouping.
Deliver import-day filtering and correct persisted import-time reads.
Deliver a storage query result with unique total, group descriptors, and bounded photos.

**Inputs and prerequisites**

Use the current typed Image columns, SqlFragment binds, BuildScopedFileQuery, and SearchMatchSet.
Check all direct ElementStore construction sites before changing its connection ownership.
Check the actual linked DuckDB version and timezone capability.

**Modules, files, and APIs**

Change the DuckORM, ElementStore, filter factory, ElementMapper, and Storage files from Section 7.
Add the proposed SQL builder and album-query domain files.
Add AlbumQueryTest to the application test registration, using the existing persisted synthetic library support.

**Data rules and invariants**

- Root and album scope remain identical to BuildScopedFileQuery.
- Import time remains File-owned and uses the existing column.
- Query options contain at most one group and one explicit sort field; both can name the same field.
- Every sort has deterministic null placement and a unique final comparison.
- Date groups use full timestamps for tied photo-sort values without adding a second explicit sort choice.
- Scalar group counts sum to the unique photo total.
- Label memberships contain no duplicate canonical `(key, file_id)` pair.
- Every statement preserves placeholder and bind order.
- Connections belong to one operation. Results release before that operation ends.

**Implementation steps**

1. Add generic SELECT clause functions and execute their output against DuckDB in DuckormStatementTest.
2. Replace the touched ElementStore connection lifetime with operation-boundary acquisition from Database.
3. Fix ElementMapper UTC write/read conversion without rewriting existing timestamp rows.
4. Add import-day and import-unknown filter factories. Validate local dates and independently converted UTC boundaries.
5. Add the domain enum-to-expression map. Reuse current scope and typed field definitions.
6. Extend the existing initial match-set read with import time and required sort values.
7. Add scalar and canonical-label group descriptors. Keep unique counts separate from label occurrence counts.
8. Add ordered page, scalar position, and ordered ID-only reads. Apply one shared comparison definition.
9. Propagate SQL errors through the new path. Remove empty-success handling from the touched query helpers.
10. Record physical plans and initial scale timings. Check timestamp predicate placement and projection width.

**Primary success call chain**

```text
Album query test or service caller
  -> ElementStore operation -> Database connection and lock
  -> FilterSQLCompiler + domain field expressions -> DuckORM SELECT clauses
  -> scoped matching rows -> counts, groups, and ordered page
  -> typed result -> temporary rows released -> caller
```

**Primary failure and restore call chain**

```text
Invalid option / timezone conversion / prepare / execute / decode failure
  -> reject input or throw the original error
  -> transaction and temporary-result cleanup
  -> release operation connection
  -> caller receives failure; persistent rows remain unchanged
```

**Tests and evidence**

| Test | Required observable assertion |
| --- | --- |
| `SelectClausesKeepBindsInSqlOrder` | Projection, source, predicate, and pagination binds execute with their intended values. |
| `GroupingSetsDistinguishUnknownFromTotal` | A real null-key group is distinct from the total row. |
| `SortedPagesCoverAllMatchingFilesWithoutDuplicates` | Equal timestamps and null values produce the same IDs across page boundaries in both directions. |
| `ScalarGroupsCountEveryFilteredFileOnce` | Every group and total matches independently computed fixture results. |
| `LabelGroupsUseAllCanonicalAssignmentsFromActiveModel` | Overlapping labels repeat across groups, aliases do not duplicate within a group, and inactive-model labels do not count. |
| `ImportDayFilterUsesLocalMidnightBoundaries` | Both ordinary and daylight-saving days include their start and exclude the next local midnight. |
| `ElementTimesRoundTripAsUtcAcrossLocalTimeZones` | A persisted instant remains the same after a mapper read and update. |
| `LinkingFileToAlbumKeepsImportTime` | Linking and unlinking membership preserve the File's original added_time. |
| `AlbumQueryFailureReportsDuckDbError` | A real prepare or execute failure cannot become an empty successful result. |
| `DateGroupsSortPhotosByFullTimestamp` | Capture and import day groups remain contiguous; both photo-sort directions retain full timestamp precision within a day. |
| `DateGroupTimestampResolvesEqualPhotoSortValues` | Equal ratings within capture and import day groups follow full time newest first, then File ID; page and position reads agree. |
| `DateGroupWithoutExplicitSortUsesFullTimestamp` | With no selected arrow, date groups use full time newest first; flat mode retains File ID order. |
| `SameScalarGroupAndSortUsesFileIdForTies` | Camera, lens, and rating grouping accept the same sort field and keep deterministic File ID order within exact-value groups. |

Also test empty scopes, unknown import time, invalid enum values, limit overflow, and invalid dates.
Test import time through a successful import and project reopen.
Test a failed import through the existing cleanup path.

**Build and run commands**

Use the Phase 1 commands in Section 11 after target registration.
Run DuckORM, filter compiler/factory, AlbumQueryTest, LibrarySearchColumnsTest, and relevant import-owner cases.
Do not run RAW decode or the full test suite for SQL scale data.

**Exit criteria**

- [ ] Generic builder output executes with correct bind order.
- [ ] All supported group and sort fields have storage-level evidence.
- [ ] Import time round-trip, day boundaries, and membership preservation pass.
- [ ] Failures remain failures and resources release after error injection.
- [ ] Linked DuckDB version, timezone capability, and first query plans are recorded.

**Expected diff**: 1400-1850 lines.

**Completion record**:

##### Phase 1 completion record (2026-10-02)

```text
Phase / date / status: Phase 1 / 2026-10-02 / complete.
Source revision and branch: base deeb89018 (main); branch feature/album-query-storage.
Actual changed modules and diff size: DuckORM clause builder, album query domain options and
  SQL map, ElementStore, ElementMapper, import-day filter factories, Storage construction,
  synthetic library test support, DuckormStatementTest, AlbumQueryTest. 2576 added and 274
  removed code lines (16 files), above the 1400-1850 estimate. The connection-per-operation
  rewrite changes every ElementStore operation (749 lines), and AlbumQueryTest is 1013 lines.
Implemented behavior:
  - duckorm::clause: select_query, as, count_all, count_distinct, where, group_by,
    grouping_sets, order_by (explicit direction and NULL placement), limit_offset (bound).
  - AlbumQueryOptions (sort field, direction, group field, import-day IANA zone) with
    ValidateAlbumQueryOptions; one comparison definition (OccurrenceOrderTerms) for page,
    position, and id reads; fixed group orders; date-group timestamp tie term.
  - ElementStore::ReadAlbumQuery / ReadAlbumFilePosition / ReadAlbumFileIds over one
    temporary match set inside one read transaction; scalar and canonical-label groups;
    unique and occurrence counts; Inspector statistics with local import-day buckets.
  - ElementStore requests a connection from Database at every public operation; the
    long-lived ConnectionGuard is removed. Touched list/count helpers throw DuckDB errors
    instead of returning an empty success.
  - ElementMapper writes and reads Element times as UTC with calendar arithmetic (no
    std::gmtime, no mktime). SearchResultRow carries the import time (Unix seconds).
  - BuildImportDateBucketFilter (half-open UTC range from two independently converted local
    midnights) and BuildImportDateUnknownFilter.
  - Inspector label buckets read the same canonical label relation as the label group.
Explicitly unimplemented items: application facade, UI owner, worker routing, and section
  projection (Phase 2); QML (Phase 3). 100,000-file timings (Phase 2 step 11).
Primary success call chain:
  AlbumQueryTest / future AlbumBrowseService caller
    -> ElementStore::ReadAlbumQuery -> Database::GetConnectionGuard + database lock
    -> duckorm::Transaction -> MatchSetTable (BuildScopedFileQuery + FilterSQLCompiler output)
    -> album_query_sql (OccurrenceSource, GroupKeyExpression, OccurrenceOrderTerms,
       LabelRelations) -> duckorm::clause statements
    -> counts, group descriptors, statistics, bounded page
    -> match set dropped -> COMMIT -> connection released -> AlbumQueryResult
Primary failure and restore call chain:
  invalid option or page bound -> std::invalid_argument before any statement
  prepare / execute failure (bad predicate, unknown IANA zone) -> std::runtime_error with the
    DuckDB message -> MatchSetTable drop -> Transaction rollback -> connection and lock
    released -> caller receives the error; persistent rows unchanged
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DuckormExprTest
    DuckormStatementTest SleeveFilterCompileTest SleeveFilterFactoryTest AlbumQueryTest
    LibrarySearchColumnsTest FilterServiceTest LibrarySearchRecallTest ProjectServiceTest
    ImportRawOnlyTest SemanticStorageControllerTest AiStorageControllerTest SleeveFSTest
    SleeveServiceTest AlbumBackendImportTest AlbumBackendStatsFilterTest --parallel 4 -> 0
  ctest --test-dir build/debug --output-on-failure -j 1 -R '^(<the same targets>)\.' -> 0
  ALCEDO_ALBUM_QUERY_PROFILE=1 ctest ... -R 'AlbumQueryTest\.AlbumQueryTest\.TenThousand' -> 0
Discovered / passed / failed / skipped counts: 209 / 208 / 0 / 1. The skipped case is the
  opt-in 10,000-file measurement; it passed in its own run with the variable set.
Import-time and timezone evidence: ElementTimesRoundTripAsUtcAcrossLocalTimeZones (process
  TZ JST-9, EST5EDT, UTC0), ImportDayFilterUsesLocalMidnightBoundaries (America/New_York
  2026-06-10 and the 23-hour 2026-03-08), LinkingFileToAlbumKeepsImportTime,
  ImportTimePersistsThroughImportAndProjectReopen (production ImportService with the RAW
  fixture and one rejected JPEG), UnknownImportTimeFormsTheLastImportDayGroup.
  LinkedDuckDbConvertsIanaTimeZonesWithoutTheSessionSetting: ICU time zones are linked.
  Rewritten-time evidence: before this phase, FromParams read the UTC text with mktime, so
  each read and update of an Element row in a non-UTC process moved added_time and
  modified_time by the UTC offset. No row repair is proposed; the affected rows cannot be
  told apart from correct rows without the original import log.
SQL plans, linked DuckDB version, and latency measurements: SELECT version() = v1.2.1
  (bundled libduckdb-windows). EXPLAIN ANALYZE of the deep page (LIMIT 1000 OFFSET 9000,
  camera group, rating DESC): TOP_N (Top 1000, Offset 9000) over a PROJECTION over a
  sequential TABLE_SCAN of SearchMatchSet that reads only file_id, image_id, file_name,
  camera_model, rating. Debug build, 10,000 files, 202 camera groups, 5 warm runs:
  initial read (groups + statistics + page) p50 40.0 ms / p95 41.4 ms (cold 41.8 ms);
  shallow page p50 16.9 / p95 17.6 ms; deep page p50 15.6 / p95 16.5 ms; focus position
  p50 10.9 / p95 13.7 ms. Release and 100,000-file timings belong to Phase 2.
Loaded metadata and visible-cell measurements: not applicable (no UI in this phase).
Manual verification in both themes: not applicable.
Unavailable platforms or UI coverage: macOS not built or run (no macOS host).
Durable evidence record and temporary evidence path: this record;
  build/tmp/album_sort_group/phase1/ (removed after the phase).
Remaining defects: the SemanticImageLabel primary key (file_id, model_key) holds one label
  per file and model, so the active model cannot give a file two labels today; the label
  relation is a generic DISTINCT (file_id, label_key) relation and the tests prove alias
  merging and active-model isolation. Pre-existing, outside this phase: updating a FILE
  element through ElementStore::UpdateElement fails on the FileImage upsert (no key).
```

### Phase 2 - Unified application query and section model

**Objective and deliverables**

Deliver one async application read path for filters, search, sorting, grouping, and paging.
Deliver tested section geometry and minimal group occurrence references.
Preserve photo identity and remove the current fabricated import-date display.

**Inputs and prerequisites**

Require Phase 1 storage and timezone evidence.
Read the existing worker interleaving tests and every direct caller of the changed catalog APIs.
Audit editor filmstrip order, rating updates, selection ranges, and thumbnail visibility before changing index meaning.

**Modules, files, and APIs**

Change AlbumBrowseService, SleeveFilterService, LibraryModule, StatsEngine, and SearchController.
Reuse the existing worker and its applied-request publication check.
Add AlbumSectionModel and AlbumSectionModelTest.
Extend the existing thumbnail/caller APIs only where grouped occurrences require it.

**Data rules and invariants**

- LibraryModule owns one accepted option set. Controls read that set.
- Existing controllers own their filter values. A request captures only the inputs needed on the worker.
- One accepted completion publishes groups, first page, statistics, and settings together.
- Section projection stores group keys, counts, IDs, and positions. It does not copy AlbumItem rows.
- Photos keep one File ID across groups and album links.
- A stale page completion cannot append to a later accepted query.

**Implementation steps**

1. Add the facade reads from Section 5.3. Move library storage access behind those APIs, including focus lookup.
2. Add focused LibraryModule operations for exclusive sort actions and grouping checkboxes. Keep each choice exclusive within its role and allow both roles on one field.
3. Route StatsEngine changes and applied searches to the same read path. Preserve Inspector filters during search apply and clear.
4. Reuse the existing worker for first reads and bounded pages. Keep preview ordering independent from applied-library ordering.
5. Convert SQL page rows directly to existing photo display fields. Remove current-date import values and per-row reads where SQL supplies the data.
6. Add section counts, prefix row lookup, collapse state, occurrence-range mapping, and scalar scroll anchors.
7. Add identity-based range selection and focus-position reads. Update filmstrip callers to use the same ordered unique IDs.
8. Count visible label occurrences before releasing thumbnail pins. Publish rating and thumbnail changes to every occurrence.
9. Handle import, membership, rating, deletion, and active-model notifications through a full accepted refresh.
10. Add explicit query error state and retry. Preserve valid accepted content without overwriting owners from old copies.
11. Complete the 10,000/100,000-file measurements and diagnose any failed budgets from Section 6.

**Primary success call chain**

```text
Inspector / search / presentation change
  -> LibraryModule focused operation -> existing worker
  -> AlbumBrowseService -> ElementStore -> DuckORM -> DuckDB
  -> moved result -> queued GUI completion -> existing request check
  -> photo owner and section projection -> StatsEngine presentation
  -> accepted settings and model notifications
```

**Primary failure and restore call chain**

```text
Service or SQL failure
  -> worker returns explicit error
  -> GUI rejects superseded completion or reports current failure
  -> accepted settings, groups, photos, and stats remain valid
  -> Retry resubmits the requested operation
```

**Tests and evidence**

| Test | Required observable assertion |
| --- | --- |
| `GroupingPreservesSearchAndInspectorPredicates` | Grouped and flat results contain the same unique filtered files. |
| `SearchApplyKeepsInspectorFiltersAndPresentationOptions` | Search adds its predicate without clearing import, rating, camera, group, or sort choices. |
| `ReopenDisplaysPersistedImportDate` | Displayed import date comes from persisted time rather than the reopen day. |
| `SectionHeadersRemainUniqueAcrossPageBoundaries` | A group split across pages has one header and its complete count. |
| `SectionRowsSkipCollapsedPhotoRanges` | Row lookup and selection navigation omit collapsed cells without changing unique total. |
| `EqualSortKeysKeepDeterministicFocusPosition` | Position lookup agrees with page order for ties and null fields. |
| `EarlierReadCannotReplaceLaterSortSelection` | A delayed real worker completion cannot publish after a newer sort request. |
| `PendingSortAndGroupChangesApplyTogether` | Rapid sort, group, and filter changes retain every still-requested field in the accepted result. |
| `SelectingNewGroupReplacesPreviousGroupWithoutChangingFilters` | Exactly one checkbox is checked after success; all active filters remain. |
| `GroupingSortedFieldPreservesSortAndFilters` | One accepted update selects the group and keeps the same field's sort, direction, and all active filters. |
| `PendingGroupChangePreservesSameFieldPendingSort` | Rapid requests preserve both same-field choices and their direction after publication. |
| `SortActionsReplaceDirectionOrClearExplicitSort` | Only one arrow is active; selecting its opposite reverses order; activating it again clears sorting. |
| `QueuedPageCannotAppendAfterFilterRefresh` | A page queued before an accepted refresh cannot add old rows to the new result. |
| `DuplicateLabelOccurrencesSharePhotoUpdatesAndExportIdentity` | Rating and thumbnail updates reach all occurrences; export and delete receive each ID once. |
| `LastVisibleOccurrenceReleasesThumbnailPin` | Releasing one of two visible occurrences keeps the photo pinned; releasing both releases it. |
| `RepeatedOccurrenceVisibilityIsIdempotent` | Repeated true/false notifications neither leak a pin nor release a still-visible occurrence. |
| `AlbumQuerySqlRunsOnWorkerThread` | Observer evidence records no album SQL on the GUI thread. |
| `DistantSectionReadKeepsMetadataPagesBounded` | Repeated distant jumps do not load all earlier photos or retain an unbounded page cache. |

Use persisted multi-photo fixtures. The current one-photo Inspector fixture cannot prove sorting or multiple sections.
Drive worker timing through the existing production observer/callback path.
Do not inject impossible generation values to manufacture evidence.

**Build and run commands**

Use Phase 2 commands in Section 11.
Run AlbumQueryTest, AlbumSectionModelTest, and the listed direct UI owner/caller targets.
Use a single running build or test operation in the build directory.

**Exit criteria**

- [ ] Inspector, search, sort, group, and paging use the same facade read.
- [ ] No fake import dates remain in the changed display path.
- [ ] Section geometry, identity handling, and thumbnail occurrence ownership pass.
- [ ] Real worker interleavings prove correct publication and error behavior.
- [ ] Deep-page, focus, memory, and GUI-thread measurements are recorded and assessed.

**Expected diff**: 1500-1900 lines.

**Completion record**:

##### Phase 2 completion record (2026-10-02)

```text
Phase / date / status: Phase 2 / 2026-10-02 / complete.
Source revision and branch: stacked on feature/album-query-storage (Phase 1);
  branch feature/album-query-library-module.
Actual changed modules and diff size: AlbumBrowseService facade, SleeveFilterService stats
  conversion, LibraryModule query owner, AlbumSectionModel (new), AlbumThumbnailModel split
  reset, SearchRequestWorker kinds and idle wait, SearchController pending search, StatsEngine
  import filter and refresh routing, ThumbnailManager occurrence visibility, seeded-project
  fixture, AlbumSectionModelTest (new), AlbumBackendLibraryQueryTest (new), search worker test,
  AlbumQueryTest measurement case. About 2800 added and 430 removed lines (25 files), above the
  1500-1900 estimate: the LibraryModule query rework replaces its window loads (806 changed
  lines) and the two new test files are 860 lines.
Implemented behavior:
  - AlbumBrowseService::ReadAlbumQuery / ReadAlbumFilePosition / ReadAlbumFileIds compile the
    merged filter tree once, throw on every failure, and report each read to a query observer.
  - LibraryModule owns one accepted AlbumQueryOptions value and one optional pending change.
    ToggleInspectorSort and SetInspectorGrouping implement the exclusive choices of Section
    2.2.1; both keep every filter; group and sort can name the same field.
  - Every library refresh (Inspector filter, search apply and clear, sort, group, folder
    change, import, membership, rating, deletion, active model, language) is one coalesced
    request on the one SearchRequestWorker, now owned by LibraryModule. A refresh reads the
    first page, the groups, and the statistics in one read. Later pages, positions, and id
    reads use kinds kLibraryPage and kLibraryIds and read the accepted query interpretation.
  - Publication begins the thumbnail-model and section-model resets, installs the photos, the
    sections, the options, the statistics, and the search state, then ends the resets and
    emits the notifications. A newer pending change survives the publication of an older read.
  - Search apply keeps the Inspector filters and the presentation options; clear search
    removes only the search term. The applied search becomes active only with its result.
  - Album items take file name, camera, lens, rating, capture date, and import date from the
    SQL row; QDate::currentDate() is no longer an import date.
  - AlbumSectionModel: groups with prefix row starts, 1 + ceil(n / c) rows per expanded group,
    collapse state per group key, column count >= 1, occurrence ranges for rows, visible
    occurrence ranges without collapsed groups, documented row geometry (RowOffset,
    RowAtOffset), and at most 3 retained occurrence pages (file ids only).
  - Grouped mode keeps the unique files of the retained pages in the thumbnail model in
    occurrence order, so the editor filmstrip shows the same order without headers.
  - ThumbnailManager counts visible occurrences by (group key, file id, tier); repeated
    notifications are idempotent and the pin is released with the last occurrence.
  - Query failure: the error is published, the accepted content and options stay, the
    pending change stays, and RetryLibraryQuery resubmits it.
  - Import-day Inspector filter category "import" with local-day buckets (importDateStats).
Explicitly unimplemented items: QML controls, grouped view, and translations (Phase 3).
Primary success call chain:
  Inspector / search / presentation change -> StatsEngine / SearchController /
  LibraryModule focused operation -> LibraryModule::RequestLibraryRefresh (one per turn)
  -> SubmitPendingRefresh: LibraryQueryInput (scope, filter tree, search part, options,
     model key, services) -> SearchRequestWorker kApply
  -> RunLibraryQuery: BuildFuzzySearchWhere (pending text) -> AlbumBrowseService::ReadAlbumQuery
     -> ElementStore -> DuckDB
  -> QMetaObject::invokeMethod(QueuedConnection) -> IsCurrent check -> PublishRefresh
  -> thumbnail model, section model, StatsEngine, SearchController -> notifications
Primary failure and restore call chain:
  ReadAlbumQuery throws -> LibraryQueryOutput.error_ -> PublishRefresh keeps accepted state,
  publishes queryError -> RetryLibraryQuery -> same pending change resubmitted
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumQueryTest
    AlbumSectionModelTest AlbumBackendLibraryQueryTest FilterServiceTest
    AlbumBackendStatsFilterTest AlbumBackendSearchWorkerTest AlbumBackendThumbnailTest
    AlbumBackendRatingTest AlbumBackendImageDeleteTest --parallel 4 -> 0
  ctest --test-dir build/debug --output-on-failure -j 1 -R '^(<the same targets>)\.' -> 0
  Direct callers: AlbumBackendImportTest AlbumBackendProjectTest AlbumBackendFolderTest
    AlbumBackendImageDetailsTest AlbumBackendI18nTest AlbumBackendCiWorkflowTest
    AlbumBackendBackgroundTaskTest AlbumBackendInteractionPolicyTest
    AlbumBackendDbWriteBarrierTest AdjustmentTransferControllerTest GlobalSearchDialogQmlTest
    -> build 0, ctest 0 after the fix below.
Discovered / passed / failed / skipped counts: Phase 2 list 116 / 114 / 0 / 2 (a Metal-only
  thumbnail case on Windows, and the opt-in measurement); direct callers 113 / 113 / 0 / 0
  (first run: 1 failure, ProjectSwitchRemovesPreviousWorkspace, caused by this phase: the
  accepted query input held the closed project's services and its database file. Fixed: the
  accepted input keeps no service; each read takes the open project's services.)
Import-time and timezone evidence: ReopenDisplaysPersistedImportDate (the displayed import
  date is the local date of the persisted UTC time for every photo).
SQL plans, linked DuckDB version, and latency measurements: Linked DuckDB v1.2.1. Release build (win_release, tests enabled for this
  run only, then set back to OFF), AlbumQueryTest.LargeLibraryPagesStayBoundedAndRecordTimings,
  10 warm runs after one cold run, page limit 1000. Camera groups (202) with rating DESC;
  label groups (12 canonical labels plus unlabelled, one active-model label per file) with
  capture time DESC.
  100,000 files: scalar initial (groups + statistics + page) p50 130.0 / p95 146.2 ms
  (cold 150.2); shallow page p50 73.5 / p95 91.2; middle page p50 76.2 / p95 84.4; deep page
  (offset 99000) p50 74.7 / p95 76.4; focus position p50 67.6 / p95 72.7; label initial
  p50 174.7 / p95 199.1 (cold 199.8); label deep page p50 146.4 / p95 159.9.
  10,000 files: scalar initial p95 42.4; pages p95 11.6-17.0; focus p95 10.7; label initial
  p95 71.9; label deep page p95 31.7.
  Budgets of Section 6: scalar initial 146 <= 500 ms, scalar page 91 <= 150 ms (deep
  included), label initial 199 <= 750 ms: all met. The label deep page (160 ms) has no
  separate budget. EXPLAIN ANALYZE of the 100,000-file deep page: TOP_N (Top 1000, Offset
  99000) over a sequential scan of the match set; deep OFFSET costs the same as a shallow
  page because each read re-evaluates the match set (about 70 ms), not because of the offset.
  Measured GUI-thread publication was not instrumented separately; AlbumQuerySqlRunsOnWorkerThread
  proves that no album SQL of the refresh, page, position, and id paths runs on the UI thread.
Loaded metadata and visible-cell measurements: DistantSectionReadKeepsMetadataPagesBounded:
  600 photos, pages of 120, six distant jumps: at most 3 loaded pages and at most 360 photo
  rows in the model (never all 600). Visible-cell counts belong to the Phase 3 view.
Manual verification in both themes: not applicable (no QML change).
Unavailable platforms or UI coverage: macOS not built or run.
Durable evidence record and temporary evidence path: this record;
  build/tmp/album_sort_group/phase2/ (removed after the phase).
Remaining defects: IndexOfElementInCurrentView (editor restore, synchronous QML call) runs one
  scalar ReadAlbumFilePosition on the UI thread when the file is not loaded; its page then
  loads on the worker. Multi-label occurrences of one file cannot occur with the current
  SemanticImageLabel key (one label per file and model); occurrence pins and unique ids are
  tested through the owner API.
```

### Phase 3 - Inspector field actions and virtualized album sections

**Objective and deliverables**

Deliver the direct sort actions and grouping checkboxes, the import-day filter, and grouped photo sections.
Preserve selection, zoom, editor navigation, keyboard operation, and existing thumbnail quality.

**Inputs and prerequisites**

Require the accepted query APIs and tested section projection from Phase 2.
Read DESIGN.md, docs/VI/README.md, alcedo-qml-ui, and qt-qml before editing QML.
Inspect existing photo-cell and visibility behavior before extracting shared rendering.

**Modules, files, and APIs**

Change AlbumInspectorPanel, StatsCard, DateFilterSection, StarRatingFilter, LibraryWorkspace, and ThumbnailGridView.
Add InspectorFieldActions and AlbumSectionView.
Add a proposed optional `headerActions` component slot to each existing filter-header component.
Load that slot separately from filter content so section collapse does not hide the actions.
Leave the slot empty in other callers. Do not add album controls to unrelated StatsCard uses.
Reuse DateFilterSection with independent capture/import state.
Register QML, update translations, and document the affected VI entries.

**Data rules and invariants**

- Every field header invokes the same focused LibraryModule operations.
- At most one Group checkbox and one sort arrow are active across the Inspector.
- The active grouping field keeps its sort actions and filters usable. Both roles can be selected together.
- QML does not construct SQL or sort the photo dataset.
- Headers are full-width rows. Photo cells retain the existing resolution tiers.
- Recycled items do not own selection, collapse state, or retained photo references.
- Section count text and labels have separate layout roles.

**Implementation steps**

1. Add InspectorFieldActions with two sort actions and a Group checkbox. Insert it into each field's optional header-action slot.
2. Bind every header to accepted LibraryModule values. Implement the optional exclusive choices from Section 2.2.1 and the same-field sort explanations.
3. Add the import-date section and independent filter binding. Keep capture-date selection unchanged.
4. Render section-model header and photo rows in one reusable ListView.
5. Share the existing photo actions and rendering. Avoid a second copy of selection or context-menu logic.
6. Map visible rows to coalesced metadata requests. Release occurrence pins on pooling, collapse, and workspace teardown.
7. Preserve zoom anchors and column reflow. Do not lower thumbnail quality when grouping is enabled.
8. Add keyboard and accessible names for sort arrows, Group checkboxes, headers, disclosure actions, photos, errors, and Retry.
9. Register new QML and update AppTheme/VI documentation only for required new tokens.
10. Verify both themes, narrow layout, multiple pages, multi-label groups, search composition, and editor return in the real app.

**Primary success call chain**

```text
Inspector field action -> focused LibraryModule operation -> shared async read
  -> accepted section and photo model notifications
  -> AlbumSectionView visible rows -> existing thumbnail visibility API
  -> photo selection / edit / export through existing owners
```

**Primary failure and restore call chain**

```text
Current query fails -> LibraryModule error state -> visible error and Retry
  -> last accepted settings and content remain identifiable
Missing QML registration or component error -> fix module registration
  -> rebuild and reload; do not substitute a weaker grouped view
```

**Tests and evidence**

- Extend AlbumSectionModelTest for column changes, accessible row meaning, and collapse/focus behavior.
- Extend Inspector owner tests for optional exclusive grouping, one sort direction, same-field choices, and preserved filters.
- Check that sort and grouping actions never change filter selection or section expansion.
- Check that an active group permits its own sort actions and can be unchecked without clearing the sort or filters.
- Run the direct C++ owner tests from Phase 2 after any API changes.
- Use a production-QML loading check for component creation and property availability when a reliable harness exists.
- Verify real pointer, wheel, focus, keyboard, and zoom input manually in the application.
- Record actual visible-cell and loaded-row counts while traversing large sections.
- Do not freeze colors or screenshots with automated tests. Record visual review in both themes.
- Do not spend this phase repairing WorkspaceShellTest or similar unreliable offscreen input delivery.
- Mark skipped offscreen UI coverage as skipped. Do not weaken assertions to obtain a pass.

**Build and run commands**

Use Phase 3 commands in Section 11.
Launch alcedo_main for the manual cases. Record the project, query choices, expected result, and observed result.

**Exit criteria**

- [ ] Every Inspector header reflects the same accepted exclusive choices.
- [ ] Group and sort can use the same field; header actions preserve every filter and section expansion state.
- [ ] Import-day filtering works with existing Inspector filters and search.
- [ ] Every nonempty group is reachable through correctly counted virtualized sections.
- [ ] Selection, zoom, thumbnail pin ownership, export deduplication, and editor return work in the real app.
- [ ] Both themes, narrow layout, loading, empty, and error states receive manual review.
- [ ] Build registration, translations, and VI documentation are complete.

**Expected diff**: 1200-1800 lines.

**Completion record**:

##### Phase 3 completion record (2026-10-02)

```text
Phase / date / status: Phase 3 / 2026-10-02 / implemented and tested; the manual visual
  review in the real application (both themes, narrow layout, pointer and wheel input) is
  not done by the agent and stays open for the user (no display access in this session).
Source revision and branch: stacked on feature/album-query-library-module (Phase 2);
  branch feature/album-inspector-sections.
Actual changed modules and diff size: Inspector field actions (new InspectorFieldActions.qml),
  header-action slot of StatsCard / DateFilterSection / StarRatingFilter, AlbumInspectorPanel
  (import-time section, actions on six headers), grouped photo view (new AlbumSectionView.qml),
  LibraryWorkspace (view switch, Updating / error and Retry / No Matching Photos states),
  AppTheme tokens (librarySectionHeaderHeight, inspectorHeaderActionSize), DESIGN.md,
  docs/VI/README.md, zh_CN and en catalogs, AlbumBrowseService row read for selections,
  LibraryModule selection items and lowercase QML signal twins, AlbumSectionModel RowInfo,
  AlbumSectionQmlTest (new), AlbumSectionModelTest. About 1970 added lines (24 files) including
  374 catalog lines, inside the 1200-1800 code estimate when the catalogs are excluded.
Implemented behavior:
  - Each Inspector field header (capture date, import time, camera model, labels, rating,
    lens) shows ascending and descending text actions and a Group ThemeCheckBox bound to the
    accepted LibraryModule options. One arrow and one checkbox at most; both can name the same
    field; the actions never change a filter or the section expansion; they stay available
    when the section is collapsed. Tooltips explain full-time order inside a day, the
    newest-first tie order inside date groups, the file id tie order, and code-point text order.
  - Import-day filter section with its own day selection and style; "import" filter category.
  - AlbumSectionView: one ListView with recycled header and photo rows, at most columnCount
    cells per row, fixed header height and zoom-derived photo row height (scroll offsets from
    AlbumSectionModel.RowOffset), visible rows request their occurrence pages, Expand all /
    Collapse all, separate disclosure / title / count roles, arrow-key focus over photo cells
    (headers skipped), Shift range and Ctrl+A through ordered id reads of the worker (collapsed
    groups included for Ctrl+A, skipped for Shift), Enter and double click open the editor,
    context menu and selection through the same LibraryWorkspace handlers as the grid.
  - Occurrence pins: cells register (group, file, tier) visibility; pooling, collapse, and
    teardown release them.
  - Error state with Retry, Updating caption, and No Matching Photos empty state.
Explicitly unimplemented items: restore of the scroll position from (file id, group key,
  offset) across a mode switch uses the focus position read only; a separate group-direction
  control is out of the product design.
Primary success call chain:
  InspectorFieldActions sort action / Group checkbox -> LibraryModule::ToggleInspectorSort /
  SetInspectorGrouping -> shared async read -> PublishRefresh -> sortField / groupField /
  sectionModel notifications -> AlbumSectionView rows -> RequestSectionRows and
  SetOccurrenceThumbnailVisible -> selection / editor / export through LibraryWorkspace handlers
Primary failure and restore call chain:
  read fails -> LibraryModule.queryError -> LibraryWorkspace error caption and Retry ->
  RetryLibraryQuery; the accepted rows and options stay visible
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main
    AlbumSectionModelTest AlbumSectionQmlTest AlbumBackendLibraryQueryTest
    AlbumBackendStatsFilterTest AlbumBackendThumbnailTest AlbumBackendSearchWorkerTest
    AlbumBackendI18nTest AlbumBackendProjectTest AlbumBackendImageDeleteTest
    AlbumBackendRatingTest AlbumQueryTest --parallel 4 -> 0
  ctest --test-dir build/debug --output-on-failure -j 1 -R '^(<the same test targets>)\.' -> 0
  MainQmlWorkflowTest: ProductionWindowLoadsAndRoutesCoreWorkspaceActions fails on a Qt
    Material/Dialog.qml warning; the same failure occurs on the Phase 2 head without this
    phase (stash, rebuild, rerun), so it is pre-existing.
Discovered / passed / failed / skipped counts: 117 / 112 / 0 / 5 (three env-gated external
  project cases, one Metal-only case on Windows, the opt-in measurement).
Import-time and timezone evidence: import-day section binds importDateStats and the "import"
  filter category; storage and owner evidence in the Phase 1 and Phase 2 records.
SQL plans, linked DuckDB version, and latency measurements: see Phase 2 record.
Loaded metadata and visible-cell measurements: LargeSectionTraversalKeepsCellsAndLoadedRowsBounded,
  400 photos, 1000 x 828 view, six columns, five scroll positions: at most 132 photo cells
  (viewport plus one viewport of cache buffer above and below), at most 2 loaded occurrence
  pages, at most 240 photo rows in the model.
Manual verification in both themes: not performed by the agent (no access to the desktop
  display). Required by the user: Alcedo and Classic themes, narrow Inspector, Inspector
  closed, collapsed groups, failed query, empty filtered result, real pointer, wheel, focus,
  keyboard, zoom, editor return, export of a range selection.
Unavailable platforms or UI coverage: macOS not built or run. WorkspaceShellTest not used
  (unreliable offscreen input; AGENTS.md). AlbumSectionQmlTest calls component functions and
  does not deliver synthetic pointer input.
Durable evidence record and temporary evidence path: this record;
  build/tmp/album_sort_group/phase3/ (removed after the phase).
Remaining defects: the manual review items above are open.
```

## 10. Cross-phase acceptance matrix

| Behavior | Required result | Evidence |
| --- | --- | --- |
| No grouping | All matching unique photos follow the selected SQL order. | Storage and owner tests |
| No explicit grouping or sorting | Flat photos preserve the existing File ID order, with no checked group or selected arrow. | Storage and owner tests |
| Exclusive group and sort choices | At most one group checkbox and one arrow are active. They can be on the same field; neither selection changes filters. | Owner tests and manual Inspector review |
| Grouping the currently sorted field | The group changes and the active photo sort and direction remain. | Owner and pending-request tests |
| Same time field for group and sort | Calendar days determine sections; full timestamps determine order within each day; File ID resolves equal times. | Storage page/position tests and manual Inspector review |
| Date grouping and equal values in another photo sort | Full grouping timestamps resolve ties newest first, then File ID. Only the selected photo-sort arrow is active. | Storage page/position tests and owner tests |
| Scalar grouping | All matching values create sections; each file appears once. | SQL group tests and manual view |
| Active group-field filter | Only groups with matching photos remain. Grouping does not change the filter. | Owner integration |
| Multiple Inspector categories and search | All conditions apply together in either interaction order. | Persisted multi-photo fixture |
| Import after existing photos | Persisted new import time sorts correctly; album links retain original time. | Import and reopen tests |
| Midnight and daylight-saving changes | Filter, header, calendar, and photo date agree on the same local day. | Timezone boundary tests |
| Unknown camera/lens/date | One explicit unknown section appears last. | Storage and manual view |
| Rating 0 | Unrated remains a known rating group. | Rating owner tests |
| Multi-label photo | All qualifying groups contain it; total, selection, and export remain unique by ID. | Membership and caller tests |
| Equal sort values across pages | Stable final File ID ordering produces no skipped or duplicate membership. | Ordered page tests |
| Repeated settings changes | Superseded worker results cannot publish. | Real delayed-worker path |
| Import/rating/delete during browsing | Owner notification replaces groups, counts, and page together. | Mutation and query integration |
| Query failure | Error is visible; accepted content remains valid; Retry repeats the requested operation. | Failure injection and manual check |
| Project or album switch | No rows from the previous scope publish into the new scope. | Worker and owner tests |
| Library/editor transition | Presentation options survive; focused photo identity and unique filmstrip order agree. | Manual integration and owner tests |
| Large library | SQL leaves the GUI thread; deep pages are measured; visible objects and metadata remain bounded. | Query profile and runtime counts |

Presentation changes do not enter edit history. Undo and Redo remain owned by existing edit services.
Test rating/metadata changes that affect an active sort or group through those owners.
The subsequent library refresh must reflect the resulting persisted values.

## 11. Build and verification instructions

These commands are instructions for implementation. No build or test runs during plan creation.
Run Windows commands through PowerShell from the repository root.
Allow 10-20 minutes per build invocation. Poll the same process in short intervals.
Do not terminate a healthy build merely because one poll has no output.

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
```

Confirm target registration and `ctest -N -R` discovery before running tests.
AlbumQueryTest and AlbumSectionModelTest are proposed targets. They do not exist before these phases.

```powershell
# Phase 1, after the proposed target is registered.
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target DuckormExprTest DuckormStatementTest SleeveFilterCompileTest SleeveFilterFactoryTest AlbumQueryTest LibrarySearchColumnsTest AlbumBackendImportTest --parallel 4
ctest --test-dir build/debug -N -R '^(DuckormExprTest|DuckormStatementTest|SleeveFilterCompileTest|SleeveFilterFactoryTest|AlbumQueryTest|LibrarySearchColumnsTest|AlbumBackendImportTest)\.'
ctest --test-dir build/debug --output-on-failure -j 1 -R '^(DuckormExprTest|DuckormStatementTest|SleeveFilterCompileTest|SleeveFilterFactoryTest|AlbumQueryTest|LibrarySearchColumnsTest|AlbumBackendImportTest)\.'

# Phase 2, after the section model target is registered.
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumQueryTest AlbumSectionModelTest FilterServiceTest AlbumBackendStatsFilterTest AlbumBackendSearchWorkerTest AlbumBackendThumbnailTest AlbumBackendRatingTest AlbumBackendImageDeleteTest --parallel 4
ctest --test-dir build/debug --output-on-failure -j 1 -R '^(AlbumQueryTest|AlbumSectionModelTest|FilterServiceTest|AlbumBackendStatsFilterTest|AlbumBackendSearchWorkerTest|AlbumBackendThumbnailTest|AlbumBackendRatingTest|AlbumBackendImageDeleteTest)\.'

# Phase 3: application build and the owner checks affected by the final UI work.
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main AlbumSectionModelTest AlbumBackendStatsFilterTest AlbumBackendThumbnailTest AlbumBackendSearchWorkerTest --parallel 4
ctest --test-dir build/debug --output-on-failure -j 1 -R '^(AlbumSectionModelTest|AlbumBackendStatsFilterTest|AlbumBackendThumbnailTest|AlbumBackendSearchWorkerTest)\.'
```

For latency qualification, build only the relevant query benchmark/test targets with win_release.
Use the existing build/release directory. Do not create a second build tree for this feature.
Record debug correctness and release timings separately.
Use the linked C++ DuckDB runtime for final evidence, not a different Python package version.

On macOS, use macos_debug in build/macos-debug.
Configure it with ALCEDO_BUILD_TESTS=ON for the targeted owner and SQL tests.
Report macOS timezone and UI coverage as unavailable if no macOS host is available.
SQL behavior does not require CUDA or Metal image processing.
Run any GPU-dependent direct caller cases with `ctest -j 1`.

Only the user can request a full test suite run. Record it as not run unless explicitly requested.
Check copied DLLs in each test runtime folder when results show old code behavior.
Do not run tests while a build links executables or copies DLLs.
Put temporary logs and query profiles under `build/tmp/album_sort_group/<phase>/`.
Write durable measured conclusions into the completion record before removing temporary files.

Existing touched files can use CRLF. Detect line endings from bytes.
Convert each such file to LF in a separate normalization commit before its content edit, as AGENTS.md requires.
Do not mix whole-file line-ending changes with the feature diff.
Check changed-line formatting, defining includes, naming, and added data copies before phase completion.
Search first-party source, tests, and docs with third_party excluded.
Check the full roadmap tree and filenames against the current terminology rules.

## 12. Risks and required responses

| Risk | Detection | Required response |
| --- | --- | --- |
| UTC text was read as local time | Non-UTC mapper round-trip changes an instant. | Correct conversion and test read/update/reopen. Do not guess at historical data repair. |
| Local-day grouping capability differs by DuckDB build | Required timezone expression fails or ICU is unavailable. | Report the exact capability error and update dependency wiring within the planned path. |
| Label expansion multiplies scalar statistics | Camera/date totals exceed unique matching files. | Read scalar buckets before label membership expansion. |
| Group header/page order diverges | Position lookup or page-boundary test disagrees. | Use one domain expression and comparison definition for every read. |
| Deep OFFSET is too expensive | Middle/final pages fail the measured budget. | Diagnose physical plans and revise the documented page access design before Phase 2 completes. |
| A temporary result becomes a lasting data mirror | Tables or copied photo vectors survive without a required independent lifetime. | Remove the mirror or document a concrete requirement before adding independent state. |
| A new publication protocol lacks evidence | No production interleaving can reach the claimed incorrect state. | Use existing worker ordering. Do not add the proposed mechanism. |
| UI extraction exceeds the phase estimate | Expected diff can exceed 2000 lines. | Add a named subphase before implementation. Preserve the full requested capability. |
| Synthetic QML input is unreliable | Offscreen pointer/focus delivery misses handlers. | Use owner tests and real-app verification. Record skipped input coverage. |
| Touched QML exposes existing style drift | New controls inherit Material or hardcoded selection colors. | Use Basic and documented tokens in the changed surface. Keep unrelated redesign outside this feature. |

## 13. Completion record template

Complete one record per phase. Keep failed, skipped, and unavailable evidence visible.
Do not mark a phase complete until its required behavior and evidence exist.

```text
Phase / date / status:
Source revision and branch:
Actual changed modules and diff size:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Import-time and timezone evidence:
SQL plans, linked DuckDB version, and latency measurements:
Loaded metadata and visible-cell measurements:
Manual verification in both themes:
Unavailable platforms or UI coverage:
Durable evidence record and temporary evidence path:
Remaining defects:
```

Plan creation record: source and official documentation review only. Product code is unchanged.
No build, test, runtime performance measurement, or manual application verification occurred during planning.

## 14. Primary technical references

- [DuckDB indexing](https://duckdb.org/docs/current/guides/performance/indexing): zonemap selectivity and explicit index limits.
- [DuckDB workload tuning](https://duckdb.org/docs/current/guides/performance/how_to_tune_workloads): blocking operations, memory, spill, and query-plan review.
- [DuckDB GROUPING SETS](https://duckdb.org/docs/current/sql/query_syntax/grouping_sets): multiple bucket sets and null-versus-total identification.
- [DuckDB ORDER BY](https://duckdb.org/docs/current/sql/query_syntax/orderby): explicit direction, null placement, and string comparison.
- [DuckDB prepared statements](https://duckdb.org/docs/current/clients/c/prepared): parameter binding through the C API.
- [DuckDB timestamp issues](https://duckdb.org/docs/stable/guides/sql_features/timestamps): timezone-sensitive casts and conversion costs.
- [DuckDB EXPLAIN ANALYZE](https://duckdb.org/docs/current/guides/meta/explain_analyze): runtime plan and cardinality evidence.
- [Qt GridView](https://doc.qt.io/qt-6/qml-qtquick-gridview.html): uniform cell geometry and item lifetime.
- [Qt ListView](https://doc.qt.io/qt-6/qml-qtquick-listview.html): reusable items, section behavior, and scroll positioning.

The web references describe the current public documentation at plan creation.
Check behavior against the repository's actual linked DuckDB and Qt 6.9.3 before implementation.
Do not use APIs introduced only in later Qt versions.
