# duckorm Query Expression and Album Filter SQL Plan

Date: 2026-08-08

Status: Phase 1 complete; Phase 2–3 pending

Primary owner: Alcedo Studio storage (`duckorm`, Mapper, Store) and sleeve filter SQL.

Affected areas:

- `storage/mapper/duckorm` SQL fragments and table CRUD
- `storage/mapper` and `storage/service` merge into one Mapper layer
- `storage/controller` rename to Store
- `sleeve/storage_service` facade rename to `Storage`
- `sleeve/sleeve_filter` (`FilterNode`, `FilterSQLCompiler`, `FilterCombo`)
- `app/SleeveFilterService` fuzzy-search WHERE builders
- `ui/.../stats_engine` stats-bar filter WHERE builders
- album thumbnail paging, folder stats, and search filter composition

Related roadmaps and notes:

- [Semantic Generation and Search Integration Plan](../ai/semantic_generation_search_plan.md)
- [Sleeve album membership / scope query notes](../../../refactor/2026-05-25-sleeve-album-membership-filesystem-plan.md)

This plan owns the shared predicate builder and the storage Mapper/Store rename. The
membership notes only record older scope-query work.

Delivery: use one feature branch. Land work as sequential commits. Do not open one
remote PR per phase unless a later review asks for a stack.

## Writing rules for this plan

Write this plan and its completion records in Simplified Technical English style
([ASD-STE100 skill](https://github.com/danyuchn/asd-ste100-skill)).

Follow these rules:

- Use one word for one meaning. Do not rotate synonyms for the same action.
- Use active voice.
- Use simple tense.
- Keep each procedure sentence at or under 20 words.
- Keep each description sentence at or under 25 words when possible.
- Put three or more steps in a numbered or bulleted list.
- Keep one topic in each paragraph.
- Keep required technical names. Define each name once in the glossary.

Locked verbs for this plan:

| Action | Use this verb |
| --- | --- |
| create SQL text or nodes | build |
| turn a tree into SQL | compile |
| keep a value for later | keep |
| execute SQL | run |
| join filter nodes | merge |
| take code out | delete |
| put a new API in place | add |
| change an existing path | replace |
| leave a temporary bridge | keep |

Do not mix `build` / `create` / `construct` for the same SQL action. Use **build**.
Do not mix `compile` / `generate` / `emit` for the FilterNode-to-SQL step. Use
**compile**.

Do not use the verb **store** for “keep a value”. **Store** is the persistence-API
noun only.

## Banned terms

Do not use the banned forms below in:

- this plan
- its completion records
- filenames created for this work
- new first-party identifiers added by this work

| Banned form | Use instead |
| --- | --- |
| `residual` / `residuals` / any casing | an **Open work** item, a later named phase, or a **Deferred checks** item |
| the noun spelled `c` + `ontract` / plural / any casing | interface, API, schema, protocol, invariant, behavior specification, acceptance criterion, compatibility requirement, or performance target |
| `smoke` / `Smoke` / `SMOKE` in test, target, file, or doc names | a concrete behavior, regression, or property name |
| `hydration` / `hydrate` / `gesture` and derived forms | read, load, populate, apply, drag, pinch, input sequence, pointer release, or settled edit |

Rules for unfinished work:

1. After a phase, list unfinished items under **Open work**.
2. List checks that wait for later evidence under **Deferred checks**.
3. Do not name unfinished work with the banned leftover term.
4. Do not name interfaces, APIs, schemas, or acceptance criteria with the banned
   `c` + `ontract` noun.

### Execution discipline (phase completion)

When an agent or developer finishes a named phase from this plan:

1. Finish all work that the phase checklist and exit condition require.
2. Do **not** leave **Open work** for that phase unless an item needs a
   product or engineering **decision from the plan owner**.
3. If the implementer can complete an item without a new decision, complete it
   in the same phase. Do not park implementable scraps under Open work.
4. Put later-phase scope under the next phase heading, not under Open work.
5. Put checks that need later evidence under **Deferred checks**, with the
   exact later phase or suite that will run them.
6. Write the completion record before you stop. Mark checklist boxes only when
   tests or inspection justify them.

Roadmap rule: the banned `c` + `ontract` noun is also forbidden in headings and
link labels in this file.

## Glossary

| Term | Meaning |
| --- | --- |
| **duckorm** | Generic DuckDB SQL helpers under `storage/mapper/duckorm`. Owns `expr` / `SqlFragment` and table CRUD execution. Does not own album field meaning. |
| **SqlFragment** | A duckorm value. It holds SQL text and optional bind values for one expression or clause. |
| **expr** | The duckorm helpers that build `SqlFragment` values (`and_`, `or_`, `eq`, `like`, `exists`, `lit`, `raw`). |
| **Mapper** | One type per table (or per persisted domain object). Owns row params, field descriptors, `ToParams` / `FromParams`, and single-table CRUD through duckorm. |
| **Store** | Connection-scoped persistence API for one domain (`ElementStore`, `ImageStore`, …). Owns locks, transactions, cross-table writes, and scope queries. |
| **Database** | Opens the DuckDB file, owns schema setup, and hands out `ConnectionGuard` values. Today this is `DBController`. |
| **Storage** | Facade that owns `Database` and the domain Stores. Today this is `StorageService`. |
| **TMP** | C++ template metaprogramming used here as CRTP and traits so Mapper and duckorm stay generic and avoid copy-paste. |
| **FilterNode** | The sleeve filter tree (`Logical`, `Condition`, `RawSQL`) in `filter_combo.hpp`. |
| **FilterSQLCompiler** | The sleeve domain compiler. It maps a `FilterNode` onto `duckorm::expr` fragments. It does not own FROM/JOIN scope SQL. |
| **domain predicate** | A sleeve-owned filter meaning (EXIF field, rating, semantic label). It is not a duckorm API. |
| **scope query** | The Root or album file query that joins membership and image rows. Stores own scope. Predicates stay separate. |
| **WHERE predicate** | The boolean SQL fragment that restricts rows. It is not a full SELECT statement. |
| **RawSQL node** | A `FilterNode` that embeds finished SQL. Use it only as a temporary bridge. |
| **legacy class filter** | The unused OOP types under `sleeve_filter/filters/` (`SleeveFilter`, `ExifFilter`, `DatetimeFilter`, and related templates). |

## Storage layer rename

Today `storage/` has three names for one persistence job:

| Today | Real job |
| --- | --- |
| `*Mapper` | Row params, field descriptors, duckorm single-table CRUD |
| `*Service` (under `storage/service`) | `ToParams` / `FromParams` plus a thin wrapper around the mapper |
| `*Controller` | Connection, transactions, cross-table writes, analytical SQL |

The middle storage `*Service` overlaps the mapper. It also collides with app-layer
names such as `SleeveFilterService`. Semantic and AI paths already skip that middle
layer.

Target names:

| New name | Replaces | Job |
| --- | --- | --- |
| **Mapper** | today’s Mapper **merged with** storage Service | serialize domain object ↔ row; run single-table CRUD |
| **Store** | today’s Controller | connection-scoped persistence API |
| **Database** | `DBController` | open DB, schema, connection guards |
| **Storage** | `StorageService` | own Database + Stores |

Examples:

```text
ElementMapper   ← ElementMapper + ElementService
FileMapper      ← FileMapper + FileService
ImageMapper     ← ImageMapper + ImageService
ElementStore    ← ElementController
ImageStore      ← ImageController
SemanticStore   ← SemanticStorageController
AiStore         ← AiStorageController
CommitGraphStore ← CommitGraphService   // orchestration, not an app Service
Database        ← DBController
Storage         ← StorageService
```

App-layer `*Service` types stay as product façades. Do not rename them in this plan.

### TMP rules for Mapper and duckorm

Mapper and duckorm sit in the generic persistence domain. New code here must prefer
templates over hand-copied CRUD.

Required shape after Phase 3:

1. **One CRTP Mapper base.** Merge today’s `MapperInterface` and `ServiceInterface`
   into one template, for example
   `Mapper<Derived, Domain, RowParams, Id>`.
2. **Derived Mapper supplies only table facts and conversion.**
   - table name
   - primary-key clause
   - `DuckFieldDesc` span
   - `ToParams(Domain)` / `FromParams(RowParams&&)`
   - optional `FromRawData` for duckorm row decoding
3. **CRUD lives once in the base.** Insert, batch insert, update, remove, select by
   predicate, and select by query must not be reimplemented per table.
4. **duckorm stays table-agnostic.** It takes table name, field spans, and
   `SqlFragment` values. It must not special-case Element, Image, or AI tables.
5. **Stores call Mappers.** Stores do not reimplement single-table insert/select
   loops when the Mapper base already covers them.
6. **No second parallel ORM.** Do not add a new non-template helper family beside
   the CRTP Mapper for ordinary table CRUD.

Allowed duplication:

- Domain-specific `ToParams` / `FromParams` bodies
- Store-level multi-table transaction order
- Analytical SQL that needs joins beyond one table

Forbidden duplication:

- Per-table copies of insert/update/remove/select wrappers
- Per-controller copies of escape / AND-join / LIKE helpers that belong in
  `duckorm::expr`
- A storage `*Service` layer that only forwards to a Mapper

## Why FilterSQLCompiler still exists

duckorm does **not** assemble the final album SELECT today.

Current split:

1. `FilterSQLCompiler` builds only a WHERE predicate from a `FilterNode`.
2. `ElementController::BuildScopedFileQuery` builds FROM/JOIN and folder scope.
3. The controller formats `SELECT ...` + scope + predicate and runs it.
4. `duckorm::select` only handles simple `SELECT * FROM <table> WHERE <string>` CRUD.

After this plan, duckorm gains `expr`. That does not move album meaning into duckorm.

```text
FilterNode                 // meaning: "camera model equals X"
    ↓ FilterSQLCompiler    // domain map: FilterField → column / subquery shape
duckorm::SqlFragment       // generic SQL: AND / EQ / lit / escape
    ↓ ElementStore         // scope: FROM/JOIN/folder  (today: ElementController)
final SELECT string        // run against DuckDB
```

`FilterSQLCompiler` answers domain questions that duckorm must not answer:

- Which column is `FilterField::ExifCameraModel`?
- Does CONTAINS become `LIKE '%…%'` or `contains(...)`?
- Which table alias does the scoped album query use (`i.metadata`)?
- How does a semantic-label filter become an `EXISTS` subquery?

If you delete `FilterSQLCompiler` and keep only duckorm `expr`, every caller must
repeat those maps. If you put those maps inside duckorm, duckorm learns sleeve
schema and album JSON paths. That breaks the storage/domain split.

So after Phase 1:

- **duckorm `expr`** builds generic SQL fragments.
- **FilterSQLCompiler** compiles domain trees into those fragments.
- **Stores** own scope query assembly for album list/stats.

`FilterSQLCompiler` is not a second place that invents the final query string. It is
the domain-to-fragment step.

## Inventory of sleeve_filter

`sleeve_filter` holds two designs. Only one design is live.

### Live path — FilterNode AST

These types are in the build and have callers:

| Unit | Role | Callers today |
| --- | --- | --- |
| `FilterNode` / `FieldCondition` / `FilterField` / `CompareOp` | Domain filter tree | tests, Qt demos, `CreateFilterCombo`, `BuildFolderStats` extra filter |
| `FilterSQLCompiler` | Compiles a tree to a WHERE clause | `ElementController::GetElement*ByFilter`, `BuildFolderStats`, sleeve and FilterService tests |
| `FilterCombo` | In-memory filter id plus root node | `SleeveFilterService::ApplyFilterOn` and demos |

`FilterSQLCompiler` already is a small AST-to-SQL translator for WHERE clauses. It
supports Logical AND/OR, typed conditions, BETWEEN/LIKE shapes, and RawSQL.

Production UI almost never builds typed `FieldCondition` nodes. Stats and search
bypass the typed path and build WHERE text by hand. Tests and demos still use the
typed path. The compiler is live, but the product underuses it.

### Dead or superseded units

These units are not the live design:

| Unit | Evidence | Action in this plan |
| --- | --- | --- |
| `sleeve_filter/filters/*` (`SleeveFilter`, `ValueFilter`, `RangeFilter`, `ExifFilter`, `DatetimeFilter`) | Not listed in `SleeveFilter` CMake sources. No production or test caller includes them. `exif_filter.cpp` / `datetime_filter.cpp` only sketch `ToJSON`. Predicate methods are incomplete. | Delete in Phase 1. Do not migrate into duckorm. |
| `FilterCombo::GenerateSQLOn` / `GenerateIdSQLOn` | Declared and defined. No caller. Scope SQL now lives in `BuildScopedFileQuery`. | Delete in Phase 1 after grep shows zero callers. |
| Hand-built UI/app WHERE builders | Live product paths that duplicate the compiler | Replace in Phase 2 by building `FilterNode` and compiling |
| storage `*Service` thin wrappers | Duplicate Mapper CRUD with only `ToParams` / `FromParams` added | Merge into Mapper in Phase 3 |

Do not treat the legacy class filter hierarchy as a second SQL AST. It is abandoned
code. Deleting it removes dead code. It does not remove the live FilterNode design.

### What to reuse

Reuse `FilterNode` and `FilterSQLCompiler` as the sleeve domain compile path.

Move only the generic expression mechanics into duckorm:

- AND / OR / NOT grouping
- compare and LIKE shapes
- literal escape and optional binds
- `exists` / `raw` fragment helpers

Keep in sleeve:

- `FilterField` to column mapping
- album table aliases for scoped queries
- semantic-label factories and other domain subqueries

Do not copy `filters/ExifFilter` or `filters/DatetimeFilter` into duckorm. Those
types are not a finished translator.

## Problem

Three layers build album WHERE text today.

1. `duckorm` accepts a raw `where_clause` string. It does not own condition composition.
2. `FilterSQLCompiler` builds domain WHERE text from `FilterNode`. It still joins
   strings by hand. It does not escape string values.
3. UI and app code also build WHERE text by hand:
   - `StatsEngine::BuildStatsFilterWhere`
   - `SleeveFilterService::BuildFuzzySearchWhere` and its private helpers

The UI layer knows DuckDB column paths, JSON extract expressions, and `EXISTS`
subqueries. That knowledge belongs below the UI.

Search already wraps some predicates as `FilterNode::RawSQL`. Stats-bar filters still
build a separate `wstring` WHERE. Thumbnail paging and stats therefore use two
filter paths.

A fourth problem sits beside that stack. The unused `sleeve_filter/filters/` class
hierarchy and unused `FilterCombo::Generate*SQLOn` full-query helpers look like a
parallel SQL layer. Leaving them in tree invites new code to call the wrong API.

A fifth problem sits in `storage/`. Mapper, Service, and Controller names split one
persistence job into three ranks. The Service rank mostly duplicates Mapper CRUD.

## Decision

Add a generic SQL expression layer inside duckorm. Prefer TMP and shared helpers so
new predicates and table CRUD do not copy glue code.

Reuse and harden the live `FilterNode` / `FilterSQLCompiler` path on top of that
layer. The compiler maps domain meaning onto `expr` fragments. It does not replace
scope query assembly.

Collapse storage persistence names to **Mapper + Store**. Delete the storage Service
rank. Rename `DBController` / `StorageService` to **Database** / **Storage**.

Delete the unused legacy class filter hierarchy and unused full-query helpers.

Stop building album filter SQL in the UI.

```text
UI (StatsEngine / SearchController)
  → builds FilterNode only
FilterSQLCompiler
  → maps domain fields to duckorm::expr fragments (WHERE predicate only)
duckorm::expr (SqlFragment)
  → AND / OR / EQ / LIKE / EXISTS / literal / escape / bind
ElementStore scope query
  → FROM / JOIN / folder scope + predicate → final SELECT
Mapper (CRTP)
  → ToParams / FromParams + single-table CRUD through duckorm
```

Invariants:

1. **duckorm owns generic SQL fragments.** It does not know `FilterField`, EXIF paths,
   or semantic-label tables.
2. **Sleeve owns domain predicates.** `FilterSQLCompiler` compiles `FilterNode` into
   `SqlFragment`.
3. **UI owns filter state only.** `StatsEngine` keeps selected labels and ratings. It
   does not build WHERE text.
4. **One WHERE source per request.** Stats-bar filters, search filters, and thumbnail
   paging merge into one `FilterNode` tree before compile.
5. **Bind or escape string literals.** New string predicates must not splice raw user
   text into SQL.
6. **`RawSQL` is a bridge only.** New product filters use typed nodes or sleeve
   factories. Do not add permanent RawSQL builders in UI code.
7. **Storage has two ranks only: Mapper and Store.** No storage `*Service` for table
   CRUD.
8. **Mapper and duckorm stay generic through TMP.** Table-specific code is conversion
   and schema facts only.

This plan does not include:

- a full SQL dialect parser or a third-party query ORM
- moving scope joins (`FolderContent` / Root virtual view) into duckorm
- rewriting every Store call site before the rename phase finishes
- putting Qt or QML types into duckorm or `FilterNode`
- reviving or migrating the legacy class filter hierarchy under `filters/`
- renaming app-layer product `*Service` façades

## Current call chains

Stats-bar filter on the thumbnail grid:

```text
StatsEngine::ToggleStatsFilter
  → BuildStatsFilterWhere()          // hand-built wstring
  → LibraryModule::LoadThumbnailWindow(where)
  → AlbumBrowse / ElementController scoped page query
```

Stats refresh with an active search:

```text
SearchController::ActiveSearchFilterWhere
  → StatsEngine::RefreshStats
  → FilterNode{RawSQL = search where}
  → SleeveFilterService::BuildFolderStats
  → FilterSQLCompiler::Compile
```

Fuzzy search apply:

```text
SearchController::ApplyFuzzySearch
  → SleeveFilterService::BuildFuzzySearchWhere  // hand-built wstring
  → keep optional<wstring>
  → thumbnail / stats consumers paste RawSQL or AND the wstring
```

Mapper CRUD today:

```text
Controller → storage Service → Mapper → duckorm::select|update|remove(string WHERE)
```

## Target call chains

Stats-bar and search on one request:

```text
StatsEngine builds FilterNode children for date/camera/lens/label/rating
SearchController supplies FilterNode for the active search
SleeveFilterService / library merges children under FilterOp::AND
FilterSQLCompiler::Compile → duckorm::SqlFragment
ElementStore scoped page / stats query applies the fragment
```

Mapper WHERE:

```text
Mapper / Store builds duckorm::expr fragment
  → duckorm::select|update|remove accepts SqlFragment
  → keep the string overload only during migration
```

Storage persistence:

```text
app / sleeve
  → Storage.GetElementStore()
    → ElementStore (transactions, joins, stats)
      → ElementMapper / FileMapper / … (serialize + single-table CRUD)
        → duckorm
```

## Phases

### Phase 1 — duckorm expr, live compiler reuse, dead-code removal

**Goal:** add one reusable condition builder in storage. Point the live sleeve
compiler at it. Delete the unused parallel filter layer.

Do this work:

1. Add `SqlFragment` and `duckorm::expr` helpers for column, literal, compare, `and_`,
   `or_`, `not_`, `is_null`, `like`, `exists`, and `raw`.
2. Add string escape and optional bind values in that layer.
3. Prefer free functions and small value types so callers compose fragments without
   copy-pasted string glue.
4. Add overloads so `select`, `update`, and `remove` accept `SqlFragment`.
5. Keep the existing `const char*` WHERE overloads until later consumers finish.
6. Rewrite `FilterSQLCompiler` so it compiles into `SqlFragment`. Reuse the existing
   `FilterNode` AST. Do not invent a second album filter tree.
7. Escape string values in compiled conditions.
8. Align Image column aliases with the scoped album query (`i.metadata` and related
   joins). Document the alias rule next to the compiler.
9. Delete `sleeve_filter/filters/` sources and headers after a zero-reference check.
10. Delete `FilterCombo::GenerateSQLOn` and `GenerateIdSQLOn` after a zero-reference
    check. Keep scope SQL in `BuildScopedFileQuery`.
11. Add unit tests for expr composition, escape, and FilterNode compile output.

Acceptance criteria:

- [x] A caller can build nested AND/OR predicates without string glue in product code.
- [x] `FilterSQLCompiler` returns `SqlFragment`, or an equivalent typed result that owns
      SQL text and binds.
- [x] Quoted string values from filter inputs are escaped or bound.
- [x] Existing FilterService and sleeve filter tests stay green after the compile rewrite.
- [x] New tests cover escape, AND/OR nesting, and at least one RawSQL bridge node.
- [x] duckorm headers and tests do not mention sleeve `FilterField` or UI types.
- [x] `sleeve_filter/filters/` is gone from the tree and from any target source list.
- [x] `GenerateSQLOn` / `GenerateIdSQLOn` are gone. Scoped queries still use
      `BuildScopedFileQuery`.

If Phase 1 leaves unfinished items, list them under **Open work** in the Phase 1
completion record. Do not invent a cleanup phase name for leftover scraps.
Open work is only for plan-owner decisions (see **Execution discipline**).

##### Phase 1 completion record (2026-08-08)

**Status:** complete — duckorm `expr` / `SqlFragment`, FilterSQLCompiler rewrite,
dead filter removal, and unit/integration compile-path proof.

**Primary success call chain:**

```text
FilterNode tree (typed condition / logical / RawSQL)
  -> FilterSQLCompiler::Compile
  -> duckorm::expr (col / lit / and_ / or_ / like / between / raw)
  -> duckorm::SqlFragment { sql_, binds_ }
  -> ElementController / SleeveFilterService convert sql_ to WHERE text
  -> BuildScopedFileQuery (FROM/JOIN + i./e. aliases) + DuckDB run
```

**Primary failure call chain:**

```text
Unescaped user quote in filter string (historical risk)
  -> expr::lit / escape_string doubles '
  -> SqlFragment SQL embeds 'O''Brien' form
  -> scoped query parse/run succeeds (FilterService + compile tests)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Escape / lit / nested AND-OR / like / exists / param binds | `DuckormExprTest` (7) | PASS |
| Alias, escape, RawSQL bridge, BETWEEN, element aliases | `SleeveFilterCompileTest` (6) | PASS |
| Compile + folder filters + search + stats/cache paths | `FilterServiceTest` (36) | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target DuckORM SleeveFilter StorageService SleeveFilterService DuckormExprTest SleeveFilterCompileTest FilterServiceTest
build\debug\alcedo_studio\tests\sleeve\DuckormExprTest_runtime\DuckormExprTest.exe
build\debug\alcedo_studio\tests\sleeve\SleeveFilterCompileTest_runtime\SleeveFilterCompileTest.exe
build\debug\alcedo_studio\tests\app\FilterServiceTest_runtime\FilterServiceTest.exe
```

Suite totals: DuckormExprTest 7/7; SleeveFilterCompileTest 6/6; FilterServiceTest 36/36.

**Checklist / exit condition:** all Phase 1 acceptance boxes checked.

**LOC note (grill-code-review):**

| File | LOC (approx) |
| --- | --- |
| `duckdb_expr.hpp` | 125 |
| `duckdb_expr.cpp` | 189 |
| `duckdb_orm.hpp` | 50 |
| `duckdb_orm.cpp` | 441 |
| `filter_combo.hpp` | 102 |
| `filter_sql.cpp` | 162 |
| `duckorm_expr_test.cpp` | 58 |
| `sleeve_filter_compile_test.cpp` | 83 |

No changed production file is near the 1000-LOC split threshold for this phase.

**Open work:** none (no plan-owner decision required).

**Deferred checks:**

- Album UI stats/search hand-built WHERE removal — Phase 2.
- Mapper/Store rename and CRTP merge — Phase 3.
- Full prepared-statement bind use on album scope queries — later Store work after
  consumers stop pasting WHERE as plain text.

### Phase 2 — Album filter consumers

**Goal:** remove hand-built album filter SQL from the UI. Move fuzzy-search WHERE
helpers onto the same stack.

Do this work:

1. Replace `StatsEngine::BuildStatsFilterWhere` with a builder that returns
   `std::optional<FilterNode>`. Return an empty tree when no stats filter is active.
2. Add sleeve factories for predicates that need subqueries today (semantic label
   `EXISTS`, unknown-date null checks). Keep those factories out of duckorm.
3. Change thumbnail paging and stats refresh so they merge stats nodes and search
   nodes under one AND root before compile.
4. Move shared escape, LIKE, and join helpers from `SleeveFilterService` into duckorm
   expr, or into thin wrappers over it.
5. Rebuild `BuildFuzzySearchWhere` with expr fragments. Keep FTS match text behind
   `expr::raw` only when DuckDB syntax has no typed helper yet.
6. Delete UI-local SQL escape helpers that duplicate duckorm or sleeve helpers.
7. Stop creating new UI-authored `FilterNode::RawSQL` text. Search may keep a
   compiled fragment cache. That cache must hold compiler output only.
8. Extend filter and album backend tests so stats-bar, search, and combined filters
   share one predicate path for thumbnail paging and folder stats.

Acceptance criteria:

- [ ] `stats_engine.cpp` does not contain `json_extract`, `EXISTS`, or SQL keyword string
      literals for filter predicates.
- [ ] Thumbnail paging and `BuildFolderStats` use the same compiled predicate source for
      the same UI filter state.
- [ ] Fuzzy-search WHERE construction uses duckorm expr for ordinary AND/OR/LIKE/literal
      composition.
- [ ] Combined search and stats-bar filters still restrict the thumbnail grid and the
      stats panel together.
- [ ] FilterService, album backend, and related search tests cover the migrated paths.
- [ ] A repository search of first-party album UI and backend sources shows no new
      hand-built stats-bar WHERE builders outside sleeve and app compile entry points.

During Phase 2, existing `*Controller` names may remain. Phase 3 performs the rename.

### Phase 3 — Mapper + Store reshape with TMP

**Goal:** make storage names match the real jobs. Merge the thin storage Service rank
into Mapper. Keep Mapper and duckorm generic through CRTP and shared templates.

Do this work:

1. Design one CRTP Mapper base that replaces `MapperInterface` + `ServiceInterface`.
2. Move `ToParams` / `FromParams` onto each concrete Mapper.
3. Keep single-table CRUD only in the Mapper base and duckorm.
4. Rename `*Controller` to `*Store` (`ElementStore`, `ImageStore`, `SemanticStore`,
   `AiStore`).
5. Rename `CommitGraphService` to `CommitGraphStore`, or fold it into the edit-history
   Store path if that keeps one owner clearer.
6. Rename `DBController` to `Database`.
7. Rename `StorageService` to `Storage`. Update getters such as `GetElementStore()`.
8. Delete the `storage/service` tree after callers move to Mappers and Stores.
9. Update includes, CMake targets, and tests for the new names.
10. Sweep first-party code so new table persistence does not reintroduce a storage
    `*Service` rank.

Acceptance criteria:

- [ ] No `storage/service` sources remain for ordinary table CRUD.
- [ ] Each migrated table has one Mapper type with schema facts plus conversion only.
- [ ] Single-table insert/update/remove/select wrappers are not copy-pasted per table.
- [ ] Domain Stores own transactions, cross-table writes, and scope/analytical queries.
- [ ] `Storage` exposes Stores and `Database`; it does not expose a storage Service rank.
- [ ] Focused storage, sleeve, filter, and album backend tests stay green after rename.
- [ ] A repository search shows no new `storage/service` includes in first-party code.

## Test and verification

Run focused tests after each phase:

```bash
ctest --test-dir build/debug --output-on-failure -R "FilterService|SleeveFilter|duckorm|AlbumBackend|Storage|Element|Image"
```

Add or extend tests that check:

- escape of `'` inside string literals
- AND merge of stats-bar and search predicates
- semantic-label filter with an empty model key yields no rows
- Mapper `SqlFragment` WHERE still round-trips select and remove for one existing table
  path
- one concrete Mapper still round-trips `ToParams` / `FromParams` through the merged
  CRTP base after Phase 3

Name every new test after the behavior it checks. Do not use banned vague test words.

## Completion record template

Copy this block under each phase when that phase finishes:

```markdown
##### Phase N completion record (YYYY-MM-DD)

Branch:
Commits:

Delivered:
- ...

Verification:
- command:
- result:

Open work:
- none

Deferred checks:
- none
```

## Out of scope

- Root virtual-view redesign already covered by the membership refactor notes
- Semantic model download, CLIP runtime, or AI sidecar protocol changes
- QML visual redesign of the stats bar or search drawer
- Replacing DuckDB with another database
- Rewriting the Qt demo filter UI beyond the compile-path changes it already needs
- Keeping or porting the legacy class filter hierarchy as a compatibility shim
- Renaming app-layer product façades such as `ProjectService` or `SleeveFilterService`
