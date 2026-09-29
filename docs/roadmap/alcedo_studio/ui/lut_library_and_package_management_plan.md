# LUT Library and Package Management Plan

Date: 2026-09-29  
Status: L1 complete (2026-09-29); L2-L6 not started; panel visual design intentionally blank  
Source revision: `dc73591020ef917fed089db7e4f454839d82051f`  
Primary area: Alcedo Studio UI and application services  
Parent: Standalone feature plan, indexed by the [roadmap index](../../README.md)  
Issues: [#213](https://github.com/zidage/AlcedoStudio/issues/213) and
[#215](https://github.com/zidage/AlcedoStudio/issues/215)

This document defines six implementation phases. No phase is implemented by this plan.
The panel visual design is intentionally blank. Live LUT image previews are outside this release.

## 1. Product decisions

### 1.1 Confirmed requirements

1. Store the LUT library outside the application installation.
2. Use `~/.alcedo/luts` by default on both Windows and macOS.
   Resolve `~` from the current user's home directory. Do not store the literal tilde.
3. Let the user choose and migrate the library root. The selected root also holds downloaded packages.
4. Discover `.cube` files recursively. Preserve nested user folders and relative paths.
5. Give one application service responsibility for discovery, the local inventory, and package operations.
6. Persist the inventory in the library root. Refresh it when the user requests a refresh.
7. Read browser entries through the inventory. A missing referenced file requests an inventory refresh.
8. Mark Alcedo-authored files with a structured JSON comment in the CUBE header.
9. Keep Alcedo-authored LUTs distinguishable from user LUTs. Let user tools supply the same classification fields.
10. Support `general` and `film_simulation` categories. Film filters include source and film brand.
11. Derive one print-presence filter from print metadata: All, With print, or No print.
    Combine print film and photographic paper under With print. Do not add separate type or stock-model filters.
12. Each classification filter has an All choice. General LUTs have no film-specific choices other than All.
13. Combine active filters by intersection. A filter changes the candidate set; it does not change a LUT.
14. Support libraries with thousands of files. Use bounded work and virtualized views.
15. Distribute two independent 7z packages: `spectral_film_lut` and `spektrafilm_lut`.
16. Publish packages and a signed manifest on Cloudflare R2. The LUT manifest has no expiration field or age limit.
17. Check the LUT manifest when Settings opens. Do not check it automatically at application startup.
18. Compare each package's file count and content summary with the local inventory.
19. Show a separate download, update, or repair result for each package. Download only after a user action.
20. Existing photos use the newest installed version of their selected official LUT.
21. A missing LUT does not stop rendering. Skip only that LUT operation and show the missing association.
22. Keep the reference when a file is missing. Do not erase the selected LUT from the project.
23. Replace the current browser with a separate LUT workspace, reached from the left application navigation.
24. Follow the currently selected Color Grade node. Show the node identity and name.
25. Without an open photo or a valid selected Color Grade, allow browsing and prohibit application.
26. Retain a small LUT adjustment surface in the Editor for strength and an action that opens the LUT workspace.
27. Add LUT strength as a distinct parameter. Do not change the entire Color Grade's mix amount.
28. Present strength as 0-100%, with a default of 100%.
29. Automatically restore the selected LUT at its configured strength when its missing file becomes available again.
30. A locally edited official LUT remains official while its metadata still declares it as official.
    The package update replaces it. Do not preserve an automatic user copy or ask how to resolve the edit.

The answers in this conversation confirm the shared home-directory default, migration, automatic use
of newer official content, and selection of the current Color Grade as the application target.
They also confirm the combined print-presence filter, the strength range, automatic resource restoration,
and replacement of locally edited files that retain their official metadata.

### 1.2 Scope boundaries

- Do not distribute LUT payloads inside the `.app`, Windows installer, or software-update archive.
- Do not automatically download either package on first launch.
- Do not add current-image LUT thumbnails, a preview worker, or temporary history commits.
- Do not retain old package versions to preserve old photo appearances. The user chose current package content.
- Do not change RAW decoding, rendering quality, or backend selection.
- Do not change film simulation mathematics in either generator.
- Do not infer missing film or print metadata from an ambiguous filename.
- Do not add a second editable pipeline, a second history owner, or a second persistent catalog owner.

Issue #213 also requires a real folder-open action and a visible failure result.
Issue #215 originally suggests a collapsible filesystem tree. This request defines metadata-based
classification instead. Recursive discovery and custom roots are required. Exact tree presentation
remains part of the blank visual design; do not claim that its original tree checklist is complete.

### 1.3 Decisions made during L1 (2026-09-29)

These user decisions refine sections 4.2, 4.3, and 6.3. Later phases apply them.

1. The local inventory is updated only by a user-requested rescan, never at startup or when the
   LUT panel opens. The panel reads the persisted file. It is `<root>/lut-inventory.json`,
   distinct from the signed `package-inventory.json` inside each archive.
2. The scan computes SHA-256 only for files that declare `origin: alcedo`. User LUTs are not hashed.
   Equal file names found by the recursive scan need no special handling.
3. Each entry's name is its file stem. The render pipeline refers to the file path; paths are
   unique, so equal names are separate entries. L4 must reconcile this with the reference forms
   in section 4.2 before changing the model.
4. Official LUTs form one built-in group without further category browsing. Their file names are
   canonical (`<film.id>[__<print.id>][__<variant>].cube`), so a `<film.id>` prefix finds every
   print option of a film. The browser shows only the film name; the print is an option of it.
5. Header parsing and official-file hashing run on a bounded worker set, not one thread.

### 1.4 Related work

| Plan | Relationship |
| --- | --- |
| [QML workspace plan](qml_editor_rhi_unified_workspace_plan.md) | Replace its old LUT browser placement. Keep the existing session and retained workspace lifetimes. |
| [Node-aware adjustment plan](../edit/node_mask_editor/phase_nm6_node_aware_adjustments_plan.md) | Reuse explicit node and adjustment targets, typed writes, and settled history changes. |
| [Background task plan](background_tasks_ui_state_plan.md) | Reuse current task reporting and download admission. The old plan is context, not current implementation evidence. |
| [Shortcut registry plan](configurable_keyboard_shortcut_registry_plan.md) | Preserve scoped LUT navigation and native text input in search. |

Read `AGENTS.md` and the applicable skills again before implementation. Required project skills include
`alcedo-qml-ui`, `alcedo-msvc-cmake`, and `opencl-program-registry` for the affected work.
Use the Qt QML and CMake guidance when writing those files. The user's blank visual-design requirement
takes precedence over visual examples in an older plan or skill.

## 2. Current source audit

The following facts come from source inspection. They are not build or test results.
External generator files were inspected at their supplied paths. Their Git revisions were unavailable
because Git rejected repository ownership. No external repository was changed.

| Area | Existing owner and evidence | Current behavior and required change |
| --- | --- | --- |
| Discovery | [lut_catalog.cpp](../../../../alcedo_studio/src/ui/alcedo_main/editor_support/modules/lut_catalog.cpp) | Uses a nonrecursive directory iterator. Resolves application `LUTs`, then `CONFIG_PATH/LUTs`. Move discovery out of the UI module. |
| Identity | Same catalog, `FindEntryIndexForPath` | Tries a filename match after an exact path misses. This can select the wrong file after recursive discovery. Remove this matching rule. |
| Catalog state | Same catalog, `CachedCatalogState`, `CloneCatalogWithSelection` | Uses a process cache and copies its catalog to add current selection. Replace this with one owner and a separate selection projection. |
| QML data | [editor_lut_catalog_model.cpp](../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_lut_catalog_model.cpp) | Copies entries into QVariant maps. Search uses case-insensitive substring checks. Favorites store absolute paths. |
| Current panel | [LUTPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/LUTPanel.qml) | Owns another JavaScript list, filtering, and sorting. Constructs the folder URL in QML. Replace browser behavior and retain only the small editor adjustment role. |
| Model | [lmt_model.hpp](../../../../alcedo_studio/src/include/edit/operators/models/lmt_model.hpp), [implementation](../../../../alcedo_studio/src/edit/operators/models/lmt_model.cpp) | Persists only `cube_path`. There is no LUT strength or stable official identity. |
| Typed edits | [editor_parameter_write.hpp](../../../../alcedo_studio/src/include/app/editor_parameter_write.hpp), [write owner](../../../../alcedo_studio/src/app/editor_parameter_write.cpp) | `EditorLutWrite` contains only a path. Extend the focused write and native model operation. |
| Panel reads | [editor_panel_projection.cpp](../../../../alcedo_studio/src/app/editor_panel_projection.cpp) | Reads the exact Color Grade and adjustment instance. Extend this projection, rather than reading live models from QML. |
| Runtime | [grade_lut.cpp](../../../../alcedo_studio/src/edit/runtime/grade_lut.cpp) | Parses the path and throws on load failure. Caches up to 16 packed cubes by path, size, and write time. Add explicit missing-file behavior and content-aware resolution. |
| GPU parameters | [adjustment_runtime.cpp](../../../../alcedo_studio/src/edit/runtime/adjustment_runtime.cpp) | Encodes LUT activity as zero or one. Encode strength and effective resource availability. |
| GPU execution | [CUDA](../../../../alcedo_studio/src/edit/runtime/cuda/cuda_primary_grade_pass.cu), [Metal shader](../../../../alcedo_studio/src/edit/runtime/metal/shader/primary_grade.metal), [OpenCL shader](../../../../alcedo_studio/src/edit/runtime/opencl/shader/primary_grade.cl) | The LMT replaces the current color with a sampled LUT color. Implement the same blend in all three paths. |
| Download transport | [download_service.hpp](../../../../alcedo_studio/src/include/app/download_service.hpp), [model download](../../../../alcedo_studio/src/app/model_download_service.cpp) | `DownloadService` already serializes jobs and accepts expected size and SHA-256. Model download performs its own post-download work. |
| Signatures | [update_manifest.cpp](../../../../alcedo_studio/src/app/update_manifest.cpp), [signer](../../../../alcedo_studio/src/app/update_signer_main.cpp) | Uses detached Ed25519 signatures over exact bytes. Software update parsing requires `expiresAt` and platform artifacts. Reuse signature verification, not that schema. |
| Publication | [publish_update.py](../../../../scripts/update/publish_update.py), [update documentation](../../../update-system.md) | Existing R2 tooling uploads immutable artifacts and manifests. Add LUT publication without changing application release rules. |
| Composition | [application_module_host.hpp](../../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/application_module_host.hpp) | Owns the shared download service, updater, editor session, and UI modules. Add the library service here. |
| Navigation | [workspace_router.cpp](../../../../alcedo_studio/src/ui/alcedo_main/album_backend/workspace_router.cpp), [WorkspaceHost.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/WorkspaceHost.qml) | Routes Library and Editor. Opening Library clears route image IDs. A LUT route must retain the editor target explicitly. |
| Settings | [SettingDialog.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/SettingDialog.qml) | Hosts separate settings categories. Add LUT settings and one check on each opening transition. |
| Distribution | [root CMake](../../../../CMakeLists.txt) | `alcedo_collect_packaged_luts` collects Agfa, Fuji, and Kodak cubes. Both platform install rules copy them. Remove this payload installation. |
| Tests | [catalog tests](../../../../alcedo_studio/tests/ui/lut_catalog_test.cpp), [cache tests](../../../../alcedo_studio/tests/edit/runtime/grade_lut_cache_test.cpp) | Catalog tests encode filename matching. Cache tests encode throwing on a missing file. Replace those expectations deliberately. |

The catalog test source exists, but this audit did not find its registration by filename in the
current test CMake files. Do not claim it runs without checking discovery. `grade_lut_cache_test.cpp`
is compiled into `GpuDagRawInputTest`; its suite is `GradeLutCacheTest`.

### 2.1 Generator integration sites

| Repository | Existing files and facts | Required work |
| --- | --- | --- |
| `D:/Projects/spectral_film_lut` | `src/spectral_film_lut/generate_acescc_lmt.py` calls `colour.io.write_LUT`. `film_data.py` has `manufacturer`. The ACEScc entry accepts an optional print film. | Write metadata from actual film objects and export options. Use one structured comment. Cover the ACEScc export entry and its CLI callers. |
| `D:/Projects/spektrafilm_lut` | `src/spektrafilm_lut_creator/aces_lmt.py::write_cube` already writes comments. `scripts/aces_lmt/bake.py` and `bake_print_drt.py` select negative and print paths. `formats/cube.py` supports `header_lines`. | Supply metadata at the export caller where actual film and print choices are known. Reuse the writer's comment facility. |

The spektrafilm print export states that its inverse DRT assumes ACES 2.0 SDR, 100 nit, Rec.709,
and gamma 2.2. Preserve that information in metadata and its existing attribution comments.
This feature must not silently change the display transform or promise compatibility with every DRT.

## 3. Official-file update policy

Editing the contents of an official package file is not an expected customization workflow.
If its metadata still declares an official LUT, continue to classify it as official content.
A changed file hash indicates differing package content. It does not convert the file into a user LUT.

When the user starts that package's update or repair, replace the edited official file with verified package content.
Do not create a user copy, retain the local edits, or add a per-file overwrite confirmation.
Existing photos keep their official LUT reference and use the newly installed content.
Signature and archive validation remain mandatory before activation.

Use the explicit `origin` declaration for classification. Editing numeric rows, a description, or a display name
does not change an `origin: alcedo` file into user content. A user variant must explicitly declare `origin: user`.
Never infer user ownership from a hash mismatch.

All product questions raised during this planning task are resolved. Panel visual design remains intentionally blank.

The public R2 prefix is a deployment value. A proposed prefix is `luts/v1` under the current asset host.
Do not claim that prefix is deployed. A release operator supplies the actual feed URL before publication.
This planning task does not upload packages or access signing secrets.

## 4. Data specification

### 4.1 CUBE comment schema

Use one UTF-8 comment line before the numeric table:

```text
# ALCEDO_LUT {"schema":1,"id":"spectral_film_lut:kodak-5207:negative","origin":"alcedo","category":"film_simulation","source":{"id":"spectral_film_lut","name":"Spectral Film LUT"},"film":{"id":"kodak-5207","name":"Vision3 250D","brand":"Kodak"},"input_space":"ACEScc","output_space":"ACEScc"}
```

The line is a schema example. It is not an approved initial package inventory.

| Field | Meaning and validation |
| --- | --- |
| `schema` | Integer schema version. Reject unsupported versions with a visible metadata error. |
| `id` | Stable generator-namespaced LUT identifier. Different intended looks need different IDs. Rebuilding the same look preserves its ID. |
| `origin` | `alcedo` or `user`. This is the author's declaration, not signature verification. |
| `category` | `general` or `film_simulation`. Missing metadata means a normal user LUT in `general`. |
| `source` | Generator ID and display name. Any producer can define its own source ID. |
| `film` | Film ID, display name, and brand. Required for a classified film simulation. Brand comes from source data. |
| `print` | Optional object with `id`, `name`, `brand`, and `kind`. `kind` is `film` or `paper` for descriptive metadata only. Either kind matches With print. Absence means No print. |
| `variant` | Optional stable name for an intended calibration or look variant. It contributes to the producer's stable ID. |
| `input_space`, `output_space` | Declared encoding. Official packages use the current ACEScc LMT interface. Do not infer or perform a conversion. |
| `description`, `aliases` | Optional display description and search names. Bound their sizes before indexing. |
| `display_requirement` | Optional statement of a required DRT/display configuration. Preserve actual exporter constraints. |

Use a header limit of 64 KiB and a metadata line limit of 16 KiB. Limit string lengths and alias counts.
Stop metadata inspection at the first numeric row. Do not load LUT pixel tables to populate the catalog.
Reject duplicate structured comments and invalid required fields. Keep the file visible with its error.
Unknown optional fields are ignored within schema 1. Unannotated valid CUBE files remain supported.
Do not reinterpret malformed Alcedo metadata as a valid general LUT.

Keep catalog visibility separate from runtime support. The current grade runtime requires a 3D cube.
Show unsupported 1D-only files with a clear reason and disable their application. Do not imply that
this catalog feature adds a 1D processing path. Validate full numeric data when loading for rendering.

A generator may declare `origin: alcedo` only in its explicit Alcedo package export mode.
Normal exports and user-authored tools use `origin: user`. Existing attribution and license comments remain.
The origin declaration controls classification. Signature verification authenticates replacement package bytes.
Changes to numeric LUT data do not alter the declaration or protect an official package file from replacement.

### 4.2 File identities and ownership

Use these reference forms in the LMT model:

- Official: package ID and stable LUT ID. Resolve to the package's current installed content.
- User: a library entry ID assigned by the inventory, plus its root-relative locator.
- Legacy: the old `cube_path`, retained until its identity can be resolved without ambiguity.

Store only the applicable reference form. Do not store a second writable copy of the catalog row in the model.
Keep the last known name as a small presentation value for a missing association.
Metadata IDs are not file hashes. Two different bytes can be revisions of the same official LUT.
Two user files with the same declared metadata ID remain distinct entries with distinct entry IDs.

Track `declared_origin` separately from `managed_package_id` and verified installation status.
A loose file with an Alcedo comment is recognizable as declared Alcedo content, but is not package-owned.
Only a file listed by a verified installed package can be replaced by that package's update.
For such a file, content edits with retained official metadata do not remove package ownership.
Keep its differing hash in the package summary and replace it during the requested update or repair.
Do not create a conflict state that requires a per-file user decision.

A valid `origin: user` declaration creates user content rather than an edited official revision.
If that file still occupies an old package content directory, relocate it through the library owner before cleanup.
Preserve its bytes and index it as a user entry. Do not automatically retarget existing official photo references to it.
This reclassification does not create a user copy of a file that still declares `origin: alcedo`.

For legacy projects, resolve an existing exact path first. For prior library roots, use an explicit
old-root-to-new-root mapping and the same relative path. Map old bundled official files only with
verified content records supplied by the release tooling. Do not identify them by basename alone.
An unresolved legacy reference stays visible and missing. A user selection can replace it through history.
Loading, scanning, and package updates do not create history commits or rewrite old commits.

### 4.3 Root contents and the local inventory

Proposed storage layout:

```text
<root>/
  catalog.json
  user/                         # Files imported by the application
  <existing user subfolders>/   # Recursively discovered loose files
  packages/
    spectral_film_lut/
      installed.json
      content/<inventory-hash>/...
    spektrafilm_lut/
      installed.json
      content/<inventory-hash>/...
  .downloads/                   # Service-owned partial archives and extraction work
```

`catalog.json` is the persisted form of the service's inventory. It stores schema, entry ID,
relative location, metadata, size, SHA-256, availability, and package summaries. It also stores
root-migration aliases and favorites by entry identity. Do not retain absolute-path favorites as the new format.

`installed.json` is a small package activation receipt. It records the active content directory and
the verified manifest descriptor. It does not contain a second copy of all inventory rows.
The receipt lets the service rebuild a damaged catalog without guessing which extracted directory is active.
Store the verified package inventory and signature evidence with that content directory for offline repair.

Scan loose files and only active package content. Exclude `.downloads`, inactive package content,
receipts, and internal metadata. Match the `.cube` extension case-insensitively.
Do not follow directory links or junctions in release one. Report skipped links and unreadable subtrees.
Never publish a partial scan as a complete package integrity result.

For each explicit refresh, stream file bytes for SHA-256 in the worker. Parse only bounded headers.
This is necessary to detect a content change that preserves file size and write time.
Do not read full files again during search, row selection, scrolling, or remote summary comparison.
Show refresh progress. Publish the refreshed inventory only after its persistence succeeds.
A failed refresh keeps the previous inventory available and marks its verification as incomplete.

At startup, load the local inventory without network access. Rebuild a missing or invalid inventory
from local files and verified installation receipts. A catalog lookup checks the resolved file before use.
On absence, report Missing, request one refresh, and avoid a retry loop for the same unresolved reference.
A later manual refresh or successful package operation can resolve it again.

### 4.4 Package manifest and hashes

Use a distinct signed LUT schema:

```json
{
  "schema": 1,
  "kind": "alcedo-lut-packages",
  "sequence": 1,
  "packages": [
    {
      "id": "spectral_film_lut",
      "revision": "release-label",
      "file_count": 42,
      "inventory_sha256": "64 lowercase hexadecimal characters",
      "unpacked_bytes": 123456,
      "artifact": {
        "url": "https://static.aoraw.org/luts/v1/packages/example.7z",
        "size": 12345,
        "sha256": "64 lowercase hexadecimal characters"
      }
    }
  ]
}
```

Values above are illustrative. They are not production URLs, counts, or hashes.
No expiration field is required, synthesized, or enforced. HTTP cache policy does not expire the signature.
Reuse the current Ed25519 public key and exact-byte detached signing mechanism.
Give the LUT feed its own trusted sequence setting. A protocol sequence is not an application race counter.
An older trusted sequence is rejected. The software-update feed keeps its current expiration rule.

Define two different SHA-256 values:

1. `artifact.sha256` covers the exact 7z bytes. `DownloadService` checks it and the archive size.
2. `inventory_sha256` covers the canonical ordered LUT records. It compares installed content across packaging changes.

For each record, encode `id NUL relative-path NUL decimal-byte-size NUL lowercase-sha256 LF`.
Use UTF-8 for the ID and path. Reject NUL and line breaks in both. Normalize separators to `/`.
Sort by relative-path UTF-8 bytes, then ID bytes. Hash the concatenation. Use the same fixture in Python and C++.
Reject duplicate package IDs, duplicate LUT IDs within a package, and platform-colliding paths.
The archive's `package-inventory.json` lists those records. Signed archive bytes authenticate the inventory;
the signed descriptor also fixes its expected count and canonical digest.

Licenses and attribution files are included in an explicit auxiliary-file list with size and hashes.
They are authenticated and verified but do not increase `file_count`, which counts LUTs only.
User files never contribute to an official package digest. Compare each package independently.
An absent optional package shows Not installed. An installed package with differing bytes shows Update available
or Repair required. Do not repeatedly ask users to install a package they did not select.

Entering Settings compares the remote descriptor with the last verified local summary immediately.
Label that comparison with its local verification state. Start a local refresh when verification is incomplete
or a known file change exists. An old summary alone is not proof of current on-disk integrity.

### 4.5 Package extraction and activation

Use an in-process `LibArchive::LibArchive` dependency with 7z/LZMA support on Windows and macOS.
Package the required library through the existing platform dependency flow. Do not require users to install a CLI.
The [libarchive format documentation](https://github.com/libarchive/libarchive/wiki/LibarchiveFormats)
lists 7-Zip support. Verify the exact codecs produced by the publishing tool in both packaged applications.
Do not switch archive formats after an extraction failure.

Download into service-owned partial storage. Validate archive size and SHA-256 before extraction.
Extract into a new directory on the destination volume. Accept only listed regular files and directories.
Reject absolute paths, `..`, links, devices, duplicate destinations, and paths that escape the destination.
Enforce the signed byte count and file count while streaming. Verify every extracted file and the summary.
Do not extract into live package content or user folders.

Write a verified content directory, then atomically replace that package's activation receipt.
This receipt replacement is the persistent commit point. Update the inventory through its owner next.
If catalog publication fails after the receipt changes, report Installed; inventory refresh required.
Rebuild from the receipt on restart. Never report an old receipt as active after the new one committed.
Before the commit point, all failures keep the old active package. Retain other packages and user files.
Replace locally edited official entries under section 3 without a preservation copy or overwrite prompt.
Keep old content only until active resource readers release it and explicitly reclassified user entries are relocated.
This temporary lifetime is not a user-selectable package version history.

If a newer package removes a stable LUT ID, keep the photo's association and report Missing.
Do not substitute another look or retain an old version as an unrequested compatibility path.

User-requested cancel may stop download, hashing, or extraction before activation. Activation completes as one
operation once started. Closing Settings does not silently cancel a download. Application shutdown uses
the existing service shutdown path and does not leave an active receipt pointing at partial content.

### 4.6 Directory migration

Provide two explicit operations: use an existing library root, and migrate the current library to a new root.
Validate the destination and show the operation before starting it. Do not silently merge conflicting libraries.
An existing root may contain user files; index them without renaming or overwriting them.

For migration, enumerate the current library, copy to a staging directory at the destination, and verify bytes.
Preserve relative user paths, entry IDs, favorites, package receipts, and active content.
Reject a destination inside the source, a source inside the destination, conflicting files, and unavailable space.
Use bounded streaming. Cross-volume migration must not assume that directory rename is atomic.
Persist the new root only after the destination is complete. Preserve the source on any earlier failure.
If the process stops after the root switch, the destination remains usable and source cleanup can resume.
Delete only source files verified as copied and unchanged. Leave independently changed source files and report them.

Block conflicting library mutations while a migration commits. Rendering may use already packed immutable cubes.
A resource resolution must retain access until packing completes; it must not observe half-moved content.
Root changes do not rewrite every photo. Resolve official IDs and user relative locators through the new root.
Do not automatically search the application bundle when the selected root becomes unavailable.

## 5. Owners, APIs, and execution

### 5.1 Responsibility map

| Owner | Mutable state | Input and result | Lifetime and error surface |
| --- | --- | --- | --- |
| Proposed `LutLibraryService` in `app/` | Root choice, canonical inventory, package status, ongoing library operation | Focused scan, import, migration, and package requests; row reads and change notices | Application lifetime, owned by `ApplicationModuleHost`. Explicit result/error signals. |
| Proposed library worker owned by that service | Newly discovered records and pending filesystem operation | Filesystem inputs and download completions; finished operation results | One serial operation owner. Disk and hashing work runs outside the GUI thread. |
| Existing `DownloadService` | Transfer process and current request | Verified download request; progress and completion | Existing application lifetime. It can reject a busy request without starting another daemon. |
| Proposed `LutLibraryModel` | Row indices, query, order, and focused browser entry | Scoped service reads and filter changes | UI lifetime retained across workspace changes. No copied QVariant catalog or QML array. |
| Existing `EditorSessionService` and command service | Current target and accepted parameter changes | Explicit `EditorParameterTarget` plus LUT selection or strength | Existing session lifetime. Revalidate target at mutation and use existing history ordering. |
| Existing `LmtModel` | Reference and strength | Focused selection or strength changes | Document lifetime. No file I/O in its setters or JSON parser. |
| Proposed resolver interface in `edit/runtime/` | No separate catalog | Minimal reference; resolved location, content identity, or typed absence/error | Injected resource access. Runtime code must not depend on QML or the application service. |
| Existing packed LUT cache | Immutable GPU upload data | Resolved resource content; packed cube | Bounded to the existing cache policy. Remove dependence on path/mtime alone after verified content changes. |

Use the application service to implement the runtime resolver port. Inject the port through pipeline
construction for the Editor, thumbnails, export, and other existing render consumers.
The resolver reads the same catalog through synchronized owner APIs. Do not copy the entire catalog into each pipeline.
Return minimal resource values. A scoped resource read keeps the selected files valid while the caller packs them.
Release the read before GPU work; `PackedGradeLut` already owns the required immutable upload bytes.

The scan creates new records from the filesystem; it does not clone the old catalog for editing.
The owner retains the published inventory until the replacement is complete. The new records exist only for
validation and atomic publication. Document this purpose and lifetime at their creation site.
The JSON file is serialized persistence for those same records, not another independently writable model.
Model indices and cached normalized search keys are derived data, not editable metadata copies.

### 5.2 Proposed owner operations

Names below are proposed interfaces, not existing APIs.

| Operation | Input | Result and validation |
| --- | --- | --- |
| `RefreshInventory()` | No copied inventory | Async result; publish a complete inventory or keep the prior one with diagnostics. |
| `ReadEntry(entry_id, visitor)` | Entry ID and scoped const visitor | No retained mutable reference. A deleted entry returns a typed missing result. |
| `ImportFiles(paths)` | Selected source paths | Validate, copy into `user/`, then publish entries. Report conflicts; never overwrite silently. |
| `UseRoot(path)` | Existing root | Validate and load/index before persisting the selected root. |
| `MigrateRoot(path)` | Destination root | Return progress and a committed-root result, or the precise failure with the prior root active. |
| `CheckPackages()` | Settings-open action or explicit retry | Fetch and verify the feed; compare per-package summaries. No implicit download. |
| `InstallPackage(package_id)` | Verified descriptor ID | Download, extract, validate, activate, and refresh through the same owner. |
| `CancelOperation()` | Current user-cancelable operation | Reuse the active download request ID where needed; do not create a new task framework. |
| `ResolveLut(reference)` | Minimal model reference | Scoped location and content hash, Missing, Invalid, or I/O error. No filename substitution. |
| `OpenRootDirectory()` | Active root | Use a native local-file URL; report dispatch failure to the caller. |
| `SetLutSelection(reference)` | Explicit target and selection | One typed model mutation and one settled history edit. Preserve independent strength. |
| `SetLutStrength(amount)` | Explicit target and finite amount | Validate the approved range; preview and settle through the existing adjustment path. |

The folder-open adapter calls `QDesktopServices::openUrl(QUrl::fromLocalFile(path))`.
Check the returned result and expose failure. Qt's
[desktop-services documentation](https://doc.qt.io/qt-6/qdesktopservices.html) explains the dispatch behavior.
A successful dispatch is not proof that Finder displayed a window; verify that manually on macOS.

### 5.3 Publication and concurrency rules

Serialize refresh, import, root migration, and package activation through the library owner.
Keep GUI row reads short. Never hold the inventory lock during disk I/O, hashing, or extraction.
Complete persistence before announcing successful changes. Emit minimal affected IDs and status changes.
Coalesce repeated refresh requests while one refresh is active. Do not add per-row epochs or request counters.

Use existing editor command ordering when a user changes selection, strength, or active node.
Capture the full target before submitting the edit. A later selection change cannot redirect that edit.
Do not invent another session identity protocol for this feature.

Existing production boundaries already include GUI edits, the editor render worker, and separate thumbnail
or export work. Before adding any new consistency mechanism, record the actual modified call chain,
the two conflicting operations, and a test that executes the incorrect order. A speculative overlap
does not justify a counter, cancellation scheme, or duplicated state. Package activation receipts and
schema versions describe persistent external data and serve separate purposes.

## 6. Runtime and browser behavior

### 6.1 Resource failure and restoration

Differentiate resource absence from an unreadable file or invalid CUBE data.
The user authorizes continuing without the LUT only when the file is missing.
Malformed cube data, permission failures, signature failures, and unsupported formats retain explicit errors.
Do not catch every error and return an identity transform.

For Missing, preserve the LMT reference and configured strength. Set effective LUT application to zero
for that render. Continue the remaining operations in the same node and all other nodes.
Carry a node-specific diagnostic to the panel through the existing render/session completion path.
Apply the same missing-resource semantics to Editor, thumbnail, and export consumers.
Do not write a synthetic disabled state or create a history commit because a disk file disappeared.

On a verified package change, invalidate dependent runtime results through the current invalidation owner.
Include resolved LUT content identity in the affected resource dependency. A result cache must not return
old pixels merely because the model's stable official ID stayed unchanged. Exercise a warm cache in tests.
Resolution must also inspect availability when a resource dependency is considered reusable.
Metadata refresh alone is insufficient if a cached image skips the LUT loader.

On the next inventory refresh or resource availability check, resolve the same missing association again.
If the file is available and valid, automatically restore its LUT effect at the configured strength.
Invalidate affected render results and refresh the displayed missing status. Do not require a separate enable action
or create a history commit. Invalid or unreadable returned files keep their explicit error behavior.
Preserve explicit user removal of a LUT; a returned file must never re-enable an association that the user cleared.

### 6.2 Strength

Store a finite float in `[0, 1]` and present `0-100%`. The default is 1, displayed as 100%.
Use 1 for legacy files with no strength field. Keep strength when a user selects another LUT.
At zero, retain the selection and skip sampling. At one, match the previous complete LUT result.
At an intermediate value `a`, compute `c + a * (L(c) - c)` at the existing LMT operation location.
Both operands use that operation's ACEScc working encoding. Preserve alpha and the current interpolation rule.
This parameter does not mix the complete Color Grade or its masks.

Use a focused model operation for reference changes and another for strength changes.
History serialization, WAL replay, project reopening, and adjustment transfer must preserve both fields.
Readers accept the old `cube_path` representation. Writers emit the new tagged reference and strength.
Do not create an incompatible top-level project format solely to add these optional model fields.
Validate unknown reference forms before mutating the live model.

### 6.3 Filter and search model

The result set is the intersection of category, source, brand, optional print presence, favorites,
and the text query. All removes that one predicate. Do not turn a choice into a physical folder move.
Use source metadata to populate choices. `spectral_film_lut` and `spektrafilm_lut` are initial values,
not an enumeration of all permitted sources.

When category is General, clear film-specific predicates to All. For film simulations, offer the
print dimension when candidate metadata supplies it. Its choices are All, With print, and No print.
A valid `print` object matches With print, whether it describes print film or photographic paper.
Absence of `print` matches No print. All removes the predicate and includes both sets.
Do not add a print-type or print-stock-model selection level. Print names remain descriptive and searchable metadata.
Recompute facet counts from the intersection of other predicates.
Keep an explicit zero-result state instead of silently changing a valid user's filter.
Clear a dependent choice only when its dimension no longer applies, such as switching to General.

Search names, aliases, source, brand, stock, print name, and relative path. Normalize Unicode for search,
case-fold, and tokenize punctuation. Preserve the original strings for display and filesystem access.
All query tokens must match some indexed field. Rank exact matches, token prefixes, substrings,
and bounded edit-distance matches in that order. Use the entry ID as the final stable tie-breaker.
Do not apply fuzzy matching to resource identity or automatic relinking.

Build normalized keys once per changed entry. Bound query length to 256 characters and 16 tokens.
Allow one edit for tokens of length 4-7 and two for longer tokens. Short tokens require exact or prefix matches.
Use existing aliases for alternate names; do not invent automatic translation or romanization.
Debounce typing by 100 ms. Keep expensive ranking off the GUI thread if the measured budget requires it.
Reuse the library's serial work and pending-query replacement; do not add a generic search scheduler.

Use a `QAbstractListModel` or an index-based proxy over service-owned entries. Request row data as needed.
Keep instantiated QML rows proportional to visible rows. Selecting a row updates selection roles only.
Preserve scroll position on selection and favorite changes. Filtering may change result order deliberately.
Keep the current applied LUT visible in the target summary even when it is excluded by the active filters.

### 6.4 Target and workspace behavior

Opening the LUT workspace preserves the open image, session, primary selected node, and adjustment instance.
Do not call `OpenLibrary()` as an intermediate navigation action because it clears route image IDs.
Read the existing target projection. A primary selected Color Grade in a multi-selection remains the single target.
Never apply a LUT to every selected node implicitly.

Show the target node name, its image context, and the current associated LUT through independent model roles.
When no valid target exists, browsing, search, refresh, and Settings still work. Applying or clearing a LUT is disabled.
When the target changes, load its selection and strength without submitting an edit.
The Editor shortcut opens the same library model and target. It must not create another catalog instance.

The three workspace routes must share one definition of the current image. Audit direct checks for
`workspace == editor` in the shell, shortcuts, filmstrip, and target adapters. A hidden editor surface
does not mean that its image or document target is closed.

Revisit hidden editor presentation handling. Applying a LUT from its workspace must not wait forever for a
hidden `EditorViewportItem` acknowledgement. Use the existing session's hidden-workspace behavior and request
the current frame when Editor becomes visible again. Verify this through the scheduler/service tests.

### 6.5 Panel visual design

<!-- Intentionally blank at the user's request. -->

## 7. File and module map

All paths below are repository-relative unless a drive is specified. Listed new files are proposed.

| Phase | Current files that change | Proposed files or targets |
| --- | --- | --- |
| L1 | Both external export paths in section 2.1; `src/app/update_manifest.cpp`; `src/app/CMakeLists.txt` | `src/include/utils/lut/lut_metadata.hpp`; `src/utils/lut/lut_metadata.cpp`; `src/app/lut_package_manifest.cpp` and matching header; shared detached-signature helper; `scripts/luts/prepare_lut_packages.py`; `scripts/luts/publish_lut_packages.py`; schema fixtures and documentation |
| L2 | `src/ui/alcedo_main/editor_support/modules/lut_catalog.cpp` and its header; applicable app/test CMake | `src/app/lut_library_service.cpp`, `src/app/lut_library_inventory.cpp`, `src/app/lut_library_migration.cpp`, matching headers; `LutLibraryServiceTest` |
| L3 | `src/app/CMakeLists.txt`; root dependency setup; existing download/signature integration | `src/app/lut_package_install.cpp`; `src/app/lut_package_service.cpp`; matching headers; `LutPackageServiceTest`; `LutPackageManifestTest`; platform dependency instructions |
| L4 | `src/edit/operators/models/lmt_model.cpp` and header; `src/app/editor_parameter_write*`; `src/app/editor_pipeline_command_service.cpp`; `src/app/editor_panel_projection.cpp`; pipeline construction; `src/edit/runtime/grade_lut.cpp`; `src/edit/runtime/adjustment_runtime.cpp`; three grade backends and shaders | `src/include/edit/runtime/lut_resource_resolver.hpp`; focused reference serialization helpers if needed; changes to existing model, history, transfer, cache, and GPU tests |
| L5 | `src/ui/alcedo_main/album_backend/editor_lut_catalog_model.cpp` and header; panel presentation; session and application composition | `src/ui/alcedo_main/album_backend/lut_library_model.cpp`, `lut_library_controller.cpp`, `editor_lut_adjustment_model.cpp` and matching headers; `LutLibraryModelTest` |
| L6 | `src/ui/alcedo_main/qml/LUTPanel.qml`, `EditorAdjustmentStack.qml`, `WorkspaceHost.qml`, `SettingDialog.qml`; `workspace_router.cpp` and header; application navigation; UI CMake; root CMake package rules; translations and relevant VI docs | `LutWorkspace.qml`; `LutSettingsPanel.qml`; focused router/service integration tests; artifact inspection script if existing packaging checks cannot express the assertion |

The `src/` prefix in this table means `alcedo_studio/src/`. Matching headers live under its `include/` tree.
New class headers include defining headers. Do not copy existing forward declarations without an allowed reason.
When touching a file, rename prohibited project terms and their callers within that scope.
Count the resulting maintenance changes in the phase estimate. Keep unrelated edits separate.

## 8. Phase summary

| Phase | Deliverable | Depends on | Expected changed lines | Size boundary and status |
| --- | --- | --- | ---: | --- |
| L1 | Metadata, exporter annotations, package schema, signing and publication tools | Confirmed data specification | 1300-1850 | Complete 2026-09-29; actual size exceeded the estimate (see its record). |
| L2 | Service-owned recursive inventory, user import, root selection and migration | L1 metadata | 1450-1950 | Keep query presentation in L5. Not started. |
| L3 | Independent signed package checking, 7z installation, repair and cancellation | L1, L2 | 1500-1950 | Keep Settings QML in L6. Not started. |
| L4 | Stable runtime references, missing-file behavior and LUT strength on all backends | L1, L2 | 1500-1950 | Reuse existing typed writes and grade parameters. Not started. |
| L5 | Indexed classification, fuzzy search, favorites and exact-node application | L2, L4 | 1200-1750 | No visual layout work. Not started. |
| L6 | Independent navigation, Settings, small editor control and payload-free installers | L1-L5; separate visual design input | 1300-1900 | No preview worker. Not started. |

Estimates include production code, tests, build files, resources, and documentation across repositories.
Generated LUT tables and temporary evidence are excluded. These estimates are not implementation evidence.
Count removed lines too. In particular, the existing `LUTPanel.qml` is about 1000 lines; L6 must
include its removal in the estimate. Its range assumes compact views backed by the completed L5 models.
Recount before each phase and after visual design is supplied. Split a phase before implementation
if its upper estimate can exceed 2000 lines. Do not compress tests or omit a backend to meet the limit.
The sixth phase specifies behavior now; its visual composition remains intentionally undefined.

## 9. Detailed implementation phases

### L1. Metadata and signed package publication

**Objective and deliverables.** Produce the interoperable comment schema, two annotated export paths,
a package inventory format, deterministic summaries, and offline signing/publication tooling.

**Inputs and modules.** Use section 4 and the L1 file map. Inspect each external repository's own
instructions before editing it. Preserve unrelated work. Confirm the two export entry points again.

**Data rules.** Preserve generator attribution. Stable IDs describe intended looks, not output filenames.
The signed feed has no expiration. Keep private keys outside the repositories and reuse the existing signer.

**Implementation steps.**

1. Add the bounded CUBE metadata reader and schema fixtures shared with generator tests.
2. Add explicit Alcedo package mode to both exporters. Pass film and print metadata from their actual owners.
3. Add byte-preserving tests that compare numeric LUT rows before and after comment insertion.
4. Generate `package-inventory.json`, auxiliary-file hashes, count, and deterministic digest.
5. Build one 7z per source with a documented codec and safe relative paths.
6. Extract exact-byte signature verification into a narrow helper. Keep update-specific parsing unchanged.
7. Add a separate LUT manifest validator and preparation/publishing scripts with a local-only mode.
8. Upload immutable packages before feed metadata. Publish matching signature and manifest bytes last.
   A transient mismatched pair must fail verification and offer retry; it must never be accepted unsigned.

**Success chain.** Export options -> structured comments -> inventory -> 7z and digest -> exact-byte
manifest signing -> immutable R2 upload -> verified public feed pair.

**Failure and restore chain.** Invalid metadata, duplicate identity, archive error, or failed upload ->
stop publication -> keep the previous live feed -> report the failing package and step.

**Tests and assertions.**

- `CubeCommentPreservesNumericLutRows`: inserting metadata changes no numeric row.
- `MetadataClassifiesUserFilmWithPrint`: a third-party source receives the same classification fields.
- `MetadataRejectsDuplicateAndOversizedComments`: invalid comments produce explicit errors.
- `PythonAndCppInventoryDigestsMatch`: shared records produce identical SHA-256 bytes despite input order.
- `LutManifestAcceptsOldSignedFeedWithoutExpiry`: a valid old publication remains valid without an expiry.
- `LutManifestRejectsBadSignatureAndDuplicatePackage`: invalid trust or identity cannot produce a descriptor.
- Run existing `UpdateManifestTest` to prove software-update expiration behavior is unchanged.

**Build and run.** Follow section 10. Configure and build proposed `LutMetadataTest` and
`LutPackageManifestTest`, plus existing `UpdateManifestTest` and `alcedo_update_signer`.
Run `ctest -R "^(LutMetadataTest|LutPackageManifestTest|UpdateManifestTest)\."` in the selected tree.
Run proposed Python tooling tests with `python -m unittest discover -s scripts/luts/tests`.
In the external repositories, run their existing metadata/export tests and newly registered focused cases.
Record the actual project runner and command before reporting success; do not run full LUT generation tests by accident.

**Exit criteria.**

- [x] Both exporters produce schema-valid metadata without pixel changes.
- [x] The two packages validate independently from local files.
- [x] The existing software-update parser retains its timestamp checks.
- [x] The local publication plan names all output keys without uploading them.

**Expected diff.** 1300-1850 lines. **Completion record:** see below.

##### Phase L1 completion record (2026-09-29)

**Status:** complete. Metadata schema and readers (C++ and Python), both exporters annotated,
package inventory and digest, 7z packaging, shared detached-signature check, LUT feed verifier,
preparation and publication scripts. At the user's request, L1 also adds the parallel local
library scan and the persisted `lut-inventory.json` format (section 1.3). The owning service,
refresh trigger, and root handling remain in L2.

**Source revisions and branches.**

| Repository | Base | Branch | State |
| --- | --- | --- | --- |
| `pu-erh_lab` | `dc7359102` | `feature/lut-metadata-package-publication` | Uncommitted working tree |
| `D:/Projects/spectral_film_lut` | branch HEAD | `feature/acescc-lmt-inversion` | Uncommitted, on top of the user's pending edits |
| `D:/Projects/spektrafilm_lut` | branch HEAD | `aces-adx-lmt` | Uncommitted, on top of the user's pending edits |

**Implemented modules.**

| Module | Responsibility |
| --- | --- |
| `utils/lut/lut_metadata.{hpp,cpp}` (`LutMetadata` library) | Bounded header reader (64 KiB header, 16 KiB metadata line, stops at the first numeric row), schema-1 validation, typed errors, display name and print option, canonical stem |
| `utils/lut/lut_inventory_digest.{hpp,cpp}` | Canonical inventory digest, safe relative paths, streamed file SHA-256, `package-inventory.json` parsing with count, byte, and digest cross-checks |
| `utils/lut/lut_library_scan.{hpp,cpp}` | Serial recursive enumeration (no links or junctions, `.downloads` excluded); header parsing and official-only hashing on up to 8 `std::jthread` workers with an atomic work index; `lut-inventory.json` serialization; atomic write through `QSaveFile`; validated read |
| `app/detached_signature.{hpp,cpp}` | Exact-byte Ed25519 check, now used by `VerifyUpdateManifest` (messages unchanged) and the LUT feed |
| `app/lut_package_manifest.{hpp,cpp}` | `VerifyLutPackageManifest`: signature, `alcedo-lut-packages` schema, trusted sequence, duplicate package IDs, digests, sizes, HTTPS `.7z` URLs on the feed host; no timestamps |
| `scripts/luts/*.py` | Python reference reader and writer, parallel package inventory, 7z (LZMA2 preset 9, `py7zr`), prepare (validate, archive, re-extract and verify, manifest, sign with `alcedo_update_signer`, verify with `cryptography`), publish (plan by default, `--upload` for R2) |
| spectral_film_lut `alcedo_metadata.py`, `generate_acescc_lmt.py`, `cli.py` | Metadata from loaded film objects (brand from `manufacturer`; print kind from `medium`: cine is film, photo is paper); `--alcedo-package` on `acescc-lmt` and `negative-lmt`; `negative-lmt` always uses variant `negative-scan`, so it cannot share an ID with a negative-only `acescc-lmt` export |
| spektrafilm_lut `alcedo_metadata.py`, `aces_lmt.py::write_cube`, `bake.py`, `bake_print_drt.py`, `bake_alcedo_set.sh` | Metadata from profile `info` (name, brand, `support` film or paper); variants from calibration options; print-chain `display_requirement` keeps the ACES 2.0 SDR 100 nit Rec.709 gamma 2.2 statement; `bake_alcedo_set.sh` passes `--alcedo-package` |
| `docs/lut-package-system.md` | Schema, digest, archive, feed, and publication reference |

**Primary success call chain:**

```text
exporter --alcedo-package (film and print objects)
  -> build_lut_metadata -> "# ALCEDO_LUT {...}" as line 1, numeric rows byte-identical
  -> prepare_lut_packages.py --package ID=DIR (each package independent)
  -> build_package_inventory: read_cube_header_file + canonical ID/file-name checks + SHA-256 (thread pool)
  -> package-inventory.json + inventory digest -> py7zr archive -> re-extract and verify every file
  -> manifest.json (no expiry) -> alcedo_update_signer sign -> Ed25519 verification of exact bytes
  -> publish_lut_packages.py: re-verify -> plan (default) | --upload: archives, archived signature
     and manifest, live manifest.json.sig, live manifest.json last
client (L3): VerifyLutPackageManifest -> VerifyDetachedSignature -> package descriptors
library refresh (L2 caller): ScanLutLibrary -> workers: ReadLutHeaderFile, HashLutFile (origin alcedo only)
  -> LutLibraryInventory (sorted, deterministic) -> WriteLutLibraryInventoryFile (QSaveFile commit)
panel read (L5): ReadLutLibraryInventoryFile -> entries; DisplayName() = film name, PrintOptionName() = print
```

**Primary failure call chain:**

```text
invalid, duplicate, or oversized metadata; non-canonical official name; user origin in a package;
unexpected file; colliding path; archive mismatch
  -> LutMetadataError / LutInventoryError for that package
  -> prepare exits 1 before writing manifest.json -> previous live feed unchanged
tampered manifest or wrong key -> signature check fails -> publish refuses; missing .sig -> publish refuses
C++ reader: malformed metadata -> typed LutHeaderError; entry stays visible; never a general LUT
C++ feed: bad signature, duplicate package, older sequence, or foreign URL -> error, no descriptor
unreadable directory or official file during a scan -> diagnostic; Complete() == false
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `CubeCommentPreservesNumericLutRows` | `scripts/luts/tests` (LF and CRLF); `test_cube_comment_preserves_numeric_lut_rows` in both exporter repositories | PASS |
| `MetadataClassifiesUserFilmWithPrint` | `LutMetadataTest`, `scripts/luts/tests` | PASS |
| `MetadataRejectsDuplicateAndOversizedComments` | `LutMetadataTest`, `scripts/luts/tests` | PASS |
| `PythonAndCppInventoryDigestsMatch` | `LutMetadataTest`, `scripts/luts/tests` (shared `inventory_digest_records.json`, three input orders) | PASS |
| `PythonPackageInventoryParsesInCpp` / `CppFixtureMatchesPythonPackageInventory` | `LutMetadataTest`, `scripts/luts/tests` | PASS |
| `LutManifestAcceptsOldSignedFeedWithoutExpiry` | `LutPackageManifestTest` | PASS |
| `LutManifestRejectsBadSignatureAndDuplicatePackage` | `LutPackageManifestTest` | PASS |
| Existing `UpdateManifestTest` (13 cases, including `RejectsExpiredSignedManifest`) | `UpdateManifestTest` | PASS |
| Official film shows film name with print option; unannotated, 1D, and malformed cases | `LutMetadataTest` | PASS |
| Scan hashes only official LUTs; equal names stay separate; `.downloads` excluded; Unicode path and `.CUBE` | `LutMetadataTest.ScanHashesOnlyOfficialLutsAndKeepsEqualNamesSeparate` | PASS |
| 8-worker scan equals 1-worker scan (96 files) | `LutMetadataTest.ParallelScanMatchesSingleWorkerScan` | PASS |
| Persisted inventory reads back without rescanning; damaged file rejected | `LutMetadataTest` | PASS |
| Two packages validate independently; changed archive bytes fail; invalid package stops publication | `scripts/luts/tests` | PASS |
| Signed feed verifies; plan lists the live manifest last; mismatched pair and unsigned feed refused | `scripts/luts/tests` (real `alcedo_update_signer`, temporary key) | PASS |
| Real size-3 exports: spektrafilm 3 LUTs (2383 print, slide) and spectral 2 LUTs -> `prepare_lut_packages.py` | manual command | exit 0, both packages validated |

Commands:

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target LutMetadataTest LutPackageManifestTest UpdateManifestTest UpdateService alcedo_update_signer --parallel 4
ctest --test-dir build/debug -R "^(LutMetadataTest|LutPackageManifestTest|UpdateManifestTest)\." --output-on-failure
python -m unittest discover -s scripts/luts/tests
# spectral_film_lut
uv run --frozen --no-sync --with pytest -- python -m pytest tests/test_alcedo_metadata.py tests/test_acescc_lmt.py -q
# spektrafilm_lut
.venv/Scripts/python.exe -m pytest tests/lut_creator/test_alcedo_metadata.py -q
.venv/Scripts/python.exe -m pytest tests/lut_creator/test_aces_lmt.py tests/lut_creator/test_cube_format.py -q -m unit
```

Suite totals: ctest 34/34 (LutMetadataTest 16, LutPackageManifestTest 5, UpdateManifestTest 13);
`scripts/luts/tests` 15/15; spectral_film_lut 19/19; spektrafilm_lut 13/13 new and 5/5 existing unit tests.
The full suite was not run (repository rule). No macOS build or run.

**Checklist / exit condition:** all four L1 exit criteria are checked above.

**LOC note:** `pu-erh_lab` +3597/-18 across 28 files: production C++ about 1600 lines, C++ tests and
fixtures about 600, Python tooling and tests about 1180, documentation 185. The external repositories
add about 1100 lines including tests. This exceeds the 1850-line estimate. The local scan and
`lut-inventory.json` format requested for L1 account for about 580 lines with tests, and each language
carries its own schema checks. The largest file is `lut_metadata.cpp` (398 lines).

**Remaining gaps:**

- Nothing is committed in any repository. The external changes sit on top of the user's own uncommitted edits.
- The client-side LUT feed sequence setting, download, and extraction belong to L3. `lut-inventory.json` has
  no owning service, refresh trigger, or root selection until L2.
- In normal (user) mode, `negative-lmt` and a negative-only `acescc-lmt` export still write the same file
  name; their IDs differ. Package-mode names differ.
- The Git for Windows OpenSSL on this machine lacks `pkeyutl -rawin`, so the existing
  `scripts/update/verify_release_manifest.py` cannot verify signatures here. The LUT scripts use
  `cryptography`; the update script is unchanged.
- No real R2 upload, production key, or macOS run was performed. The `luts/v1` prefix is not deployed.

### L2. Recursive inventory and root migration

**Objective and deliverables.** Replace UI-owned discovery with one persistent application service.
Support recursive files, imports, custom roots, migration, and correct folder opening.

**Inputs and modules.** L1 metadata reader and L2 file map. The default is the current user's home
directory on both platforms. Existing arbitrary user subfolders remain valid inputs.

**Data rules.** One inventory owner. No basename identity matching. No root change until destination
validation succeeds. An incomplete scan cannot assert package equality.

**Implementation steps.**

1. Add service construction, shutdown, root persistence, and typed operation errors.
2. Load or rebuild `catalog.json`. Add recursive enumeration with skipped-subtree diagnostics.
3. Stream file hashes outside the GUI thread. Publish owner records and serialized inventory atomically.
4. Add import and root-selection operations. Validate all conflicts before changing files or preferences.
5. Implement staged root migration, byte verification, commit, and bounded source cleanup.
6. Convert existing favorites through exact paths or verified legacy identity records.
7. Add native folder URL creation and visible dispatch errors. Remove the QML URL concatenation path.
8. Retire the static catalog cache and copied selection catalog once callers use service APIs.

**Success chain.** Refresh/import/root action -> service worker -> validated inventory persistence ->
owner publication -> model row notices -> browser reads the new entries.

**Failure and restore chain.** Unreadable subtree, disk full, collision, or stopped migration -> explicit
diagnostic -> retain the previous committed root and inventory -> retry through the same owner.
After a committed root switch, recover from the destination and finish only safe source cleanup.

**Tests and assertions.**

- `RecursiveScanFindsUnicodeAndUppercaseCubeFiles`: include nested Chinese names, spaces, and `.CUBE`.
- `EqualBasenamesRemainSeparateEntries`: no selection jumps across folders.
- `HeaderClaimDoesNotGrantPackageOwnership`: loose annotated files cannot be overwritten by package repair.
- `RefreshDetectsChangedBytesWithUnchangedStamp`: SHA-256 changes when size and write time do not.
- `MigrationPreservesEntryIdsAndFavorites`: root changes preserve selections without project rewrites.
- `MigrationFailureKeepsPreviousRoot`: inject copy, inventory-write, and preference-write failures.
- `MigrationDoesNotDeleteChangedSourceFile`: an external change after copying survives source cleanup.
- `OpenRootUsesEncodedLocalFileUrl`: assert the URL for spaces, `#`, `%`, and non-ASCII names.
- `FolderOpenFailureIsVisible`: a rejected OS dispatch surfaces a user-facing error.

**Build and run.** Configure/build proposed `LutLibraryServiceTest`. Run
`ctest -R "^LutLibraryServiceTest\."` in the selected tree. Use temporary roots under the test working
directory. Inspect real Finder/Explorer opening manually with an installed build in L6.

**Exit criteria.**

- [ ] Recursive inventory persists and rebuilds with exact identities.
- [ ] Migration survives injected failures on both sides of the root commit point.
- [ ] Missing file lookups request refresh without repeated retries.
- [ ] Import and migration preserve unrelated user files.

**Expected diff.** 1450-1950 lines. **Completion record:** Not started; fill section 12 for L2.

### L3. Independent package checking and installation

**Objective and deliverables.** Check, download, update, and repair either official package through
the shared download service. Keep the other package and all user-owned entries intact.

**Inputs and modules.** L1 feed, L2 library operations, and the confirmed replacement policy in section 3.
Use the L3 file map, existing `DownloadService`, and existing task reporting.

**Data rules.** No startup network check. No expiry. No automatic package download. No activation
before signature, archive, inventory, and individual extracted files pass verification.
Local edits with retained official metadata remain replaceable package content.

**Implementation steps.**

1. Fetch the feed and detached signature with bounded sizes and the current HTTPS/host restrictions.
2. Compare descriptors independently. Distinguish Not installed, Current, Update available, Repair required,
   Checking, Downloading, Verifying, Installing, and Error.
3. Reuse `DownloadRequest` and its size/hash checks. Surface transport busy without creating another daemon.
4. Add the packaged libarchive dependency and a streaming 7z reader with validated output paths.
5. Verify extracted data. Keep edited entries with official metadata in the package replacement set.
   Relocate explicitly reclassified user entries before old content cleanup. Do not preserve copies of edited official files.
6. Activate one package, refresh the inventory, and publish changed resource IDs to render consumers.
7. Connect progress, cancel, retry, shutdown, and interrupted-install cleanup to existing lifecycle owners.
8. Test unchanged remote comparisons without downloading archives or rehashing files during comparison.

**Success chain.** Settings opens -> `CheckPackages` -> signature validation -> local summary comparison ->
user selects one package -> `DownloadService` -> verified extraction -> receipt commit -> inventory publication.

**Failure and restore chain.** Network/signature/hash/extraction/space/cancel failure before commit ->
leave active receipt unchanged -> clean only owned partial output -> display error and retry.
After receipt commit, a catalog error triggers local rebuild and an explicit incomplete-refresh status.

**Tests and assertions.**

- `SettingsCheckDoesNotStartDownload`: check and install remain separate actions.
- `StartupDoesNotRequestLutFeed`: constructing application modules makes no LUT network request.
- `UpdateOnePackageKeepsOtherPackageAndUserFiles`: verify contents and identities before and after.
- `ArchiveHashMismatchKeepsInstalledPackage`: no extracted bytes become active.
- `ArchiveTraversalAndLinksAreRejected`: files cannot escape staging or replace existing files.
- `InterruptedActivationRecoversCommittedReceipt`: test interruption before and after receipt replacement.
- `CancelBeforeActivationKeepsInstalledPackage`: no receipt changes after a canceled preparation.
- `EditedOfficialLutIsReplacedWithoutPromptOrUserCopy`: change numeric rows without changing metadata;
  update the package; assert the new official bytes, unchanged official identity, and no preserved user entry.
- `OfficialHashMismatchDoesNotChangeOwnership`: refresh an edited official file and verify it remains package-owned.
- `UserDeclaredVariantSurvivesPackageReplacement`: change the origin explicitly to user; preserve those bytes as a user entry
  while the official photo reference continues to resolve to the newly installed official LUT.
- `ApplicationAndLutDownloadsShareAdmission`: a second request reports busy and does not start another transfer.

**Build and run.** Build proposed `LutPackageServiceTest` and `LutPackageManifestTest`, plus existing
`DownloadServiceTest` and `UpdateManifestTest`. Run
`ctest -R "^(LutPackageServiceTest|LutPackageManifestTest|DownloadServiceTest|UpdateManifestTest)\."`.
Use local test data and the existing fake transport pattern. Run a real signed package install on both
platforms in L6. Do not require private keys or the live R2 account for automated tests.

**Exit criteria.**

- [ ] Each package reports and changes independently.
- [ ] Every pre-commit failure preserves current content.
- [ ] Cancel and application shutdown leave a recoverable library.
- [ ] Package checking never runs from application startup.
- [ ] Edited files with retained official metadata are replaced without an extra prompt or preserved user copy.

**Expected diff.** 1500-1950 lines. **Completion record:** Not started; fill section 12 for L3.

### L4. Stable references, missing resources, and strength

**Objective and deliverables.** Make LUT resources survive root migration and official updates.
Continue rendering when a referenced LUT is missing. Apply strength consistently on every supported backend.

**Inputs and modules.** L1 identities, L2 resolution, the confirmed strength/restoration behavior, and the L4 file map.
Use existing typed parameter changes, history, resource packing, and invalidation owners.

**Data rules.** Missing is a typed result, not a catch-all error. Keep user intent separate from effective
availability. Legacy path-only projects load with the approved default strength.

**Implementation steps.**

1. Extend `LmtPayload`, focused setters, dirty fields, and serialization. Validate before mutation.
2. Extend typed write parsing, history replay, panel projection, commit descriptions, and adjustment transfer.
3. Add the runtime resolver port and inject it into every existing pipeline construction path.
4. Resolve official IDs to active package content and legacy references only through proven mappings.
5. Return Missing without loading an invalid replacement. Publish node-specific diagnostics through session results.
6. Connect resource changes and availability to runtime invalidation before cached image reuse.
7. Pack strength into shared grade parameters. Implement the blend at the LMT operation in CUDA, Metal, and OpenCL.
8. Automatically restore a returned valid resource at its configured strength. Invalidate affected results without rewriting user history.

**Success chain.** Explicit node edit -> typed owner mutation -> history commit -> runtime reference
resolution -> packed resource -> GPU blend -> published frame and current association.

**Failure and restore chain.** Resource Missing -> effective LUT disabled -> remaining pipeline renders ->
missing node association shown -> refresh/package repair detects valid resource -> automatic effect restoration at configured strength.
Invalid or unreadable data -> explicit existing render failure -> no substitution or automatic backend switch.

**Tests and assertions.**

- `LegacyLutPathLoadsWithFullStrength`: old serialized input remains readable.
- `LutReferenceAndStrengthRoundTripThroughHistory`: save, reopen, Undo, Redo, and transfer preserve intent.
- `MissingLutKeepsOtherGradeAdjustments`: output matches the same graph without that LUT, including other nodes.
- `InvalidCubeRemainsAnError`: corrupted content does not silently become an identity transform.
- `OfficialUpdateChangesPixelsWithWarmResultCache`: the same stable ID uses new installed bytes.
- `RootMigrationPreservesRenderedLutSelection`: changing the root keeps the exact intended LUT.
- `LutStrengthZeroHalfAndOneMatchExpectedPixels`: use a small nonidentity cube and independent arithmetic;
  check all three backends with absolute channel tolerance `1e-5` for bounded fixture values.
- `LutStrengthDoesNotScaleOtherAdjustments`: independent exposure and masks remain unchanged.
- `MissingLutDoesNotMutateHistory`: no new commit or loss of configured strength.
- `ReturnedLutRestoresConfiguredStrengthWithoutHistoryEdit`: remove and restore the same resource;
  verify the configured strength, restored pixels, cleared missing status, and unchanged history.
- `ReturnedFileDoesNotRestoreClearedLut`: explicitly clear the association while its file is missing;
  restoring the file does not select or apply it again.
- `ReturnedLutAtZeroStrengthRemainsVisuallyInactive`: availability returns without changing the user's zero strength.

**Build and run.** Build existing `GpuDagRawInputTest`, `EditorPipelineCommandServiceTest`, and
`EditorLookModelTest`. Run the discovered LUT/model/history cases, plus their direct caller tests.
Build/run `GpuDagCudaPrimaryGradeTest` and `GpuDagOpenClGradeTest` on Windows, and
`GpuDagMetalGradeTest` on macOS. Add the new cases to these targets where their fixtures fit.
Use `ctest -N -R "(GradeLutCacheTest|LutReference|LutStrength|EditorPipelineCommandServiceTest|EditorLookModelTest)"`
to confirm discovery before execution. GPU tests run with `-j 1`.

**Exit criteria.**

- [ ] Missing LUTs skip only the authorized operation in all render consumers.
- [ ] Strength passes independent pixel assertions on CUDA, OpenCL, and Metal.
- [ ] Warm cached results update after package replacement and file loss.
- [ ] A returned valid resource automatically restores its configured effect without an enable action or history edit.
- [ ] Reopen, migration, Undo/Redo, and transfer preserve the intended reference and strength.

**Expected diff.** 1500-1950 lines. **Completion record:** Not started; fill section 12 for L4.

### L5. Browser queries and target binding

**Objective and deliverables.** Supply the new browser with classification, fuzzy search, favorites,
and one explicit Color Grade target without loading every row into QML arrays.

**Inputs and modules.** L2 inventory, L4 typed edits, the confirmed print-presence filter, and the L5 file map.
Read node selection through the current controller and panel target APIs.

**Data rules.** The service owns entries; models own indices and query state. Browsing does not change
the document. An edit uses the captured target and is validated again by the session owner.

**Implementation steps.**

1. Add indexed row access and filter predicates from metadata. Use All, With print, and No print for the print dimension.
2. Add normalized field keys and deterministic token ranking with the bounds in section 6.3.
3. Persist favorites by entry identity. Preserve selection and scroll state across query updates.
4. Separate the current node's applied LUT summary from the currently focused browser result.
5. Add a controller that reads the session's image, node, adjustment, and editability state.
6. Route application through existing typed submit operations. Never resolve to PrimaryGrade implicitly.
7. Keep a small editor adjustment model for strength and missing-state display. Remove copied catalog ownership
   from `EditorLutCatalogModel` after its callers move to the new model.
8. Measure the query and row access budgets with 1,000 and 10,000 entries before choosing more complex indexing.

**Success chain.** Query/filter change -> indexed intersection/ranking -> changed model indices -> visible
rows -> user applies result -> exact target write -> history -> load-only current association update.

**Failure and restore chain.** No target, missing adjustment, invalid entry, or deleted node -> reject edit ->
keep browser and document unchanged -> publish an actionable reason. Zero matches show an empty result set.

**Tests and assertions.**

- `FiltersUseIntersectionAndAllRemovesOnePredicate`: compare exact entry IDs for every combination.
- `PrintPresenceCombinesPrintFilmAndPaper`: With print includes both kinds; No print excludes both;
  All includes both sets. Brand and source filters still intersect these results.
- `ThirdPartyPrintMetadataAddsPresenceFilter`: a new source receives the same three choices without source-specific code.
- `GeneralCategoryClearsFilmPredicates`: general entries remain reachable with no film metadata.
- `FuzzySearchRanksExactNameBeforeTypo`: deterministic ordering, Unicode names, aliases, and bounded typo matches.
- `TenThousandEntriesKeepVisibleRowsBounded`: increasing the catalog size does not increase instantiated rows beyond the visible range and its fixed buffer.
- `SelectionChangeDoesNotResetRows`: selecting a LUT does not emit a model reset.
- `ApplyLutChangesOnlyCapturedColorGrade`: multi-node fixtures keep unrelated models byte-equivalent.
- `NonGradeSelectionAllowsBrowseButRejectsApply`: RAW, Output, Mask, no-photo, and no-node states are covered.
- `TargetProjectionReloadDoesNotSubmitHistory`: changing nodes and reopening the workspace are read-only.

**Build and run.** Build proposed `LutLibraryModelTest`, existing `EditorLookModelTest`, and existing
`EditorPipelineCommandServiceTest`. Run
`ctest -R "^(LutLibraryModelTest|EditorLookModelTest|EditorPipelineCommandServiceTest)\."`.
Record query p50/p95, GUI-thread time, and process memory for both library sizes in a Release build.

**Exit criteria.**

- [ ] Filter semantics match the approved metadata dimensions.
- [ ] Search ranking is deterministic and bounded.
- [ ] Applying an entry changes only the explicit target.
- [ ] Selection does not rebuild the catalog or move the scroll position.

**Expected diff.** 1200-1750 lines. **Completion record:** Not started; fill section 12 for L5.

### L6. Workspace integration and distribution change

**Objective and deliverables.** Integrate the independent LUT workspace, LUT Settings, and small Editor
control. Ship both platforms without LUT payloads in the application.

**Inputs and modules.** L1-L5 and the L6 file map. Complete the deliberately blank visual design in a
separate user discussion before writing visual composition. This plan does not authorize an invented layout.

**Data rules.** Keep the editor session alive across LUT navigation. One Settings-open transition starts
one remote check. A render request must not wait on a hidden editor presentation acknowledgement.

**Implementation steps.**

1. Add a LUT route that retains the current image and selected Color Grade. Add its left-navigation entry.
2. Add the workspace using the L5 model and controller. Expose loading, empty, missing, invalid, and error states.
3. Reduce the Editor LUT surface to association, strength, missing-state information, and Open LUT library.
4. Add root selection/migration, Open folder, Refresh, two independent package actions, progress, cancel,
   and retry to LUT Settings. Trigger the remote check from Settings opening, not panel construction.
5. Register QML, resources, translations, and scoped keyboard behavior. Apply existing Basic style and AppTheme rules.
6. Remove `alcedo_collect_packaged_luts`, `alcedo_install_packaged_luts`, and their two install call sites.
   Inspect all broad config/resource copies so another install rule cannot reintroduce cube payloads.
7. Preserve existing installed user data across application update and uninstall. Do not delete the home library.
8. Inspect real Windows and macOS artifacts, then verify the signed package workflow from a clean user library.

**Success chain.** Left navigation -> retained LUT workspace -> exact target application -> Editor strength
adjustment -> Settings package install -> current resource invalidation -> correct rendered result.

**Failure and restore chain.** Invalid target or failed folder opening -> visible error with browsing preserved.
Failed package download -> existing library remains usable. Application update -> home library survives.
An empty first-run library offers download/import actions and does not silently choose a default LUT.

**Tests and evidence.**

- `LutRoutePreservesImageAndNode`: owner/router test verifies image and target on round trips.
- `HiddenEditorDoesNotBlockLutApplication`: service/scheduler test completes edits while the viewport is hidden.
- `SettingsOpenChecksOnceAndStartupDoesNotCheck`: lifecycle tests count feed requests.
- `EditorLutControlReloadDoesNotCommit`: owner test verifies strength and missing-state restoration.
- `PackagedApplicationContainsNoLutPayloads`: inspect `.app`, Windows install tree, and update archives for cubes.
- `ApplicationUpdatePreservesHomeLutLibrary`: compare inventory and user bytes across a real update.
- Manual Windows/macOS: open folder with Unicode/spaces; download either package; cancel/retry; migrate roots;
  remove/restore the associated file; select different grades; search a 10,000-entry library; reopen a project.
- Manual UI: keyboard-only operation, accessible names, both themes, narrow windows, no scroll jump, and no hidden target.

**Build and run.** Build `alcedo_main` and the focused model/router/scheduler test targets actually used.
Use the standard release packaging scripts and inspect their install trees. Record package inspection results
separately from compilation. Do not enlarge or rely on `WorkspaceShellTest` for input-delivery evidence.
Focused QML loading checks may supplement C++ owner tests after visual design exists.

**Exit criteria.**

- [ ] Both real platform packages contain no LUT payloads and can install either signed LUT package.
- [ ] The LUT workspace preserves the exact editor target and works without a photo in browse-only mode.
- [ ] The small Editor control loads current state without producing edits.
- [ ] Settings trigger, folder opening, migration, cancellation, and error reporting pass manual checks.
- [ ] Visual design and its manual evidence are recorded; section 6.5 is no longer blank when implementation completes.

**Expected diff.** 1300-1900 lines. **Completion record:** Not started; fill section 12 for L6.

## 10. Build and evidence requirements

This planning task runs no build or product tests. The following commands are implementation instructions.
Proposed test targets become valid only after their phase adds CMake registration.

```powershell
Set-Location D:/Projects/pu-erh_lab
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
$env:PATH = "D:/Projects/pu-erh_lab/build/debug/vcpkg_installed/x64-windows/debug/bin;" + $env:PATH
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target LutLibraryServiceTest --parallel 4
ctest --test-dir build/debug -N -R "^LutLibraryServiceTest\."
ctest --test-dir build/debug -R "^LutLibraryServiceTest\." --output-on-failure
```

Substitute the phase's named targets and explicit test expression. Inspect discovered tests before running them.
`GpuDagRawInputTest` includes multiple suites; use the discovered case names rather than assuming a binary prefix.
Check platform conditions in the test CMake files before naming a GPU target.

```bash
cmake --preset macos_debug -DALCEDO_BUILD_TESTS=ON
cmake --build --preset macos_debug --target LutLibraryServiceTest GpuDagMetalGradeTest
ctest --test-dir build/macos-debug -N -R '(LutLibraryServiceTest|GpuDagMetalGradeTest)'
ctest --test-dir build/macos-debug -R '(LutLibraryServiceTest|GpuDagMetalGradeTest)' -j 1 --output-on-failure
```

Use `win_release` and `macos_release` for performance and installed-product checks. Use the existing
`scripts/package_windows.ps1` and `scripts/package_macos.sh` workflows for artifact verification.
Allow at least 10-20 minutes for a Windows build, with 20 minutes as the initial CUDA allowance.
Poll a healthy build without restarting it. Run one build at a time in each existing preset directory.
Never run tests while linking or copying their DLLs. Refresh stale test-runtime DLLs before diagnosing old behavior.

Run only focused suites and their direct callers. Do not run the full test suite without an explicit user request.
Use C++ owner/service tests for UI behavior. Report unavailable synthetic input coverage honestly.
Verify Metal on a macOS host; a Windows result cannot establish Metal correctness.
Put logs and generated test data in `build/tmp/lut-library/<phase>/`. Record durable conclusions in the
completion record, then remove only that phase's temporary files when finished.

### 10.1 Scale and resource evidence

These are initial engineering acceptance targets, not measured results:

- On a documented Release test machine, 10,000 metadata entries use at most 64 MiB of added catalog/query memory.
- Visible browsing allocates no decoded LUT tables. Packed rendering uses the existing bounded LUT cache.
- A query completes within 100 ms at p95 after the 100 ms typing delay on that machine.
- GUI-thread query publication and selection stay below 16 ms at p95.
- Enumeration, hashing, archive download, and extraction do not run on the GUI thread.
- Record real file sizes and storage type. Large CUBE refresh time is bounded by streamed file I/O, not an invented constant.
- Include a large-file dataset and 10,000 small valid files. Tiny fixtures alone do not measure hashing or memory behavior.

If a target fails, identify the measured owner operation and fix it. Do not reduce image quality or truncate
the visible library. Revisit an estimate before introducing a larger index or worker design.

### 10.2 Cross-phase acceptance matrix

| Behavior | Observable result | Main phase |
| --- | --- | --- |
| Fresh install, no network | Empty local library; rendering without a chosen LUT works; no startup feed request | L2, L6 |
| Nested user folders | Every regular CUBE appears once with its full relative location | L2 |
| Duplicate basenames and metadata IDs | Distinct user entries; no automatic cross-file selection | L2, L5 |
| Signed old feed without expiry | Accepted when its sequence is trusted | L1, L3 |
| New package A, absent B | A updates independently; B remains optional | L3 |
| User edits an official LUT but retains official metadata | Requested update or repair replaces it; no user copy or extra confirmation | L3 |
| User explicitly changes origin to user | Preserve it as a user entry; official photo references still follow the official package | L2, L3 |
| Tampered archive or invalid extraction path | Active content remains unchanged; explicit error | L3 |
| Root move across volumes | All verified entries and favorites remain linked; earlier failures keep old root | L2, L4 |
| Official content update | Existing photos resolve the same ID to new content; warm render caches refresh | L4 |
| Missing LUT | Remaining graph renders; association is visible; history stays unchanged | L4 |
| Missing LUT returns | Valid resource automatically resumes at configured strength; cleared associations remain cleared | L4 |
| Invalid LUT | Precise error, no unauthorized identity substitution | L4 |
| Strength boundaries and invalid numbers | 0/0.5/1 produce expected pixels; NaN and out-of-range writes are rejected atomically | L4 |
| Undo/Redo, checkout, transfer, reopen | Exact reference and strength return without browser-generated commits | L4, L5 |
| New generator with print metadata | All / With print / No print work; print film and paper share With print | L1, L5 |
| No valid Color Grade | Search and browse work; apply is unavailable | L5, L6 |
| Hidden Editor workspace | LUT edit completes and returning Editor shows the current result | L6 |
| Packaged application and update | No LUT payloads inside; external library remains available | L6 |

## 11. Risks and implementation stop conditions

- Stable IDs require an explicit mapping from intended export variants. Stop package publication on duplicate IDs.
- An aggregate digest cannot detect unscanned disk edits. Report its verification age and refresh when required.
- Whole-file hashes can be expensive for large libraries. Stream them in the worker and expose progress.
- A missing-file check only inside the LUT loader can be skipped by warm image caches. Test resource invalidation before reuse.
- Do not infer a standard 7z CLI on macOS or Windows. Test the bundled library against the published codec.
- A root migration or package install is not atomic because its final rename exists. Test the actual persistent commit point.
- The print export's display assumptions are real. Preserve and expose them; do not alter DRT selection as a hidden side effect.
- Existing source files may use CRLF. If a file needs changes, follow the repository's separate LF-conversion commit rule.
- Keep official classification when only content bytes change. Do not reopen the confirmed replacement policy in section 3.
- Keep section 6.5 blank during this planning task. Ask for visual design before implementing visual composition.
- Split a phase if the expected diff can exceed 2000 lines, including terminology cleanup and tests.
- Do not add a consistency protocol without the required production interleaving and executable test.
- If current sources no longer match this audit, update affected owners and call chains before implementation.

## 12. Completion records

L1 is recorded under its phase. L2-L6 are not started. Copy this record into the relevant phase after implementation:

```text
Phase / date / status:
Source revision and branch in each changed repository:
Actual changed modules and changed-line count:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Windows CUDA / Windows OpenCL / macOS Metal evidence:
Manual product and visual verification:
Performance and resource measurements:
Package inventory and signature verification:
Durable evidence location:
Remaining defects or unavailable platforms:
```

Planning validation must remain separate from these completion records. Source inspection, link checks,
and terminology checks do not prove rendering, package extraction, migration, or UI behavior.
