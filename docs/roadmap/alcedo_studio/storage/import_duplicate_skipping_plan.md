# Import Duplicate Skipping Plan

Updated: 2026-11-10

Status: Path-and-size imports and replacement rendering qualification are complete on Windows/CUDA.
Section 12 preserves combined-reference evidence and platform limits before this branch split.

Coordination parent: [Duplicate Import and Project Maintenance Plans](duplicate_image_cleanup_and_import_plan.md).

Source audit: `a195dd21`. This revision replaces the hash-based design from 2026-11-09.
Sections 7 and 11 preserve historical completion evidence for that earlier implementation.
Their removed APIs and hash requirements are not current requirements.

## 1. Approved product decisions

Use the normalized full source path and file size in bytes to identify repeated imports.
Keep every folder component. Different paths remain separate images, even when their bytes match.
Do not read image contents to compute a duplicate key. Do not store source-content hashes.

When the size changes at an existing path, keep the Image ID, project entry IDs, ratings,
color labels, deletion marks, edit history, Versions, masks, memberships, and import time.
Record the new size. Existing edits now apply to the current file at that path.
Tell the user that the source changed. Refresh affected previews and an open editor.
The user accepts that a same-size content replacement can remain undetected.

The initial hash prototype was not merged upstream. The owner confirmed that the only existing
project was already converted to size-only storage. Do not retain automatic hash migration,
hash-table detection, or hash-record deletion on project opening.

Keep the default-on **Skip duplicate imports** preference at
**Settings > Default Behavior > Import**. Settings keeps its existing pending/apply/dismiss behavior.
File selection, folder import, dropped files, and automation use the same request-local option.
An explicit automation option overrides the saved preference for that request only.
Disabling skipping permits independent entries and independent edit histories.

Import skipping and Project Maintenance are separate features. Import does not remove project
entries, merge different paths, add a duplicate-review dialog, or compare filenames alone.

## 2. Product behavior

### 2.1 Matching rules

`NormalizeSourceImagePath` requires an absolute path. Normalize dot components and separators.
Windows uses invariant case folding. macOS preserves path case. Do not resolve symlinks or
merge distinct visible hard-link paths. File identity checks protect a read; they are not keys.

| Existing binding versus incoming file | Result with skipping enabled |
| --- | --- |
| Different normalized full paths | Import a separate entry |
| Same path and recorded size | Skip the occurrence; preserve user state |
| Same path and different recorded size | Update source size; preserve IDs and user state; refresh affected views |
| Same path without a recorded size | Record the current readable size and skip; make no claim about previous bytes |
| Same path and size but changed contents | Skip; this limitation is accepted |
| Missing unrelated source | Continue the incoming path without opening the unrelated file |
| Unreadable incoming source | Report its read failure |
| Source changes while metadata/persistence work runs | Report the real source-change error; restore provisional import state |

If several existing entries share a path, compare all recorded sizes. A mismatch in any entry
counts as one updated incoming occurrence. Record the current size for all affected Images.
Refresh all bound entries. A later equal-size occurrence counts as a duplicate.

### 2.2 Storage and ownership

Image owns the original full source path. Store only `image_id` and `byte_size` in
`ImageSourceSize`. The table does not duplicate metadata, paths, ratings, or history.
Create the size table for fresh and existing projects. Do not create a hash table or inspect one.

`SourceFileStore::FindBindings` reads current Image/FileImage/PipelineRoot bindings once per
request. It normalizes paths in memory and retains matching query results.
`RecordSizes` uses a scoped locked connection and one transaction. It validates distinct live
Image bindings before changing any size. A storage failure preserves previous sizes and user data.
ImageStore invalidates the recorded size when its owner changes the source path.
Removing the final Image reference also removes its size row through the Image storage owner.

A size update changes only size rows. It must not write an old Image copy over current ratings,
marks, metadata, or edits. `ImportSourceChange` carries only the path and affected entry/Image IDs.
The import result owns this change description until the UI completion callback consumes it.

### 2.3 Progress, completion and failure

Count each requested occurrence exactly once as imported, updated, duplicate, or failed.
Keep RAW subsets and cancellation/unsupported/excluded subsets accurate.
UI completed counts include imports and updates. Duplicate counts remain separate from failures.
Automation returns `updated` and `duplicates` in task counts.

Report a size change with the existing localized notice that edits and ratings now apply to the
current file. Invalidate affected thumbnails. Notify the editor for each affected entry.
If the affected image is loading, wait until it is Interactive. If navigation leaves the image,
clear its pending refresh. Reuse the existing checkpoint save and navigation owners.
Do not replace a pending switch/close target with the image being left.

New import completion follows checked Image, filesystem, pipeline-root, and size persistence.
A failed representative cannot cause a later occurrence to be reported as a successful skip.
Remove provisional IDs through their owners after a write failure. Stop admission if restoration
fails, and return that error. Cancellation finalizes accepted work and leaves unread files unread.
Reject a second overlapping import with the existing explicit BUSY result.

## 3. Current source audit and file map

Paths below are relative to `alcedo_studio/`.

| Responsibility | Owner and current files | Observable result |
| --- | --- | --- |
| Path comparison | `src/utils/file/source_image_path.cpp` | Full lexical path; platform case rule; no content read |
| Protected source size | `src/utils/file/source_file_reader.cpp` | Handle metadata only; Windows denies writers/replacement; POSIX validates identity, size, mtime and ctime |
| Size persistence | `src/storage/store/image/source_file_store.cpp`, `src/storage/store/database.cpp` | Size-only table and atomic owner updates; no hash migration |
| Image path/deletion updates | `src/storage/store/image/image_store.cpp` | Size invalidation/removal in the owner's transaction |
| Import orchestration | `src/app/import_service.cpp` | One submission executor; bounded 64-entry metadata/root batches; checked persistence |
| Preference | `src/ui/alcedo_main/album_backend/import_behavior_preferences.cpp` | Persisted default-on preference, request-local override |
| Completion adapters | `src/ui/alcedo_main/album_backend/import_export.cpp`, `automation/commands/library_commands.cpp` | Counts, source-change notice, thumbnail refresh and editor notification |
| Editor refresh | `editor_session_controller.cpp`, `src/app/editor_session_service.cpp`, `editor_session_navigation_controller.cpp` | Save/reacquire same Image through the normal command queue and load current bytes |
| Rendering integration tests | `tests/automation/import_source_replacement_rendering_test.cpp` | Real TIFF/Bayer DNG replacement, preview/export pixel comparisons, package reopen |

The import source reader does not decode or hash the file. Metadata extraction still reads the
source through the existing image metadata owner. New images still require project writes.
Path lookup uses ordered containers, rather than an incoming-file by existing-entry nested scan.
For P stored entries and M incoming paths, comparison work is approximately
O(P log M + M log M), excluding path lengths and database join costs.
Metadata preparation stays bounded to 64 entries. The 6,000-file HDD workload is not benchmarked.

## 4. Primary call chains

```text
File/folder/drop/automation request -> saved preference or explicit request option
  -> ImportService submission executor -> normalized requested paths -> FindBindings
  -> same path: protected handle size -> skip or atomic size update
  -> new path: bounded metadata worker -> private default document -> root encoding
  -> checked Image/filesystem/root/size persistence -> progress -> completion
  -> adapter notice -> thumbnail invalidation -> SourceFileChanged
  -> editor command queue -> save checkpoint -> reacquire history -> load current source
```

```text
Source read/change failure or owner write failure -> occurrence error
  -> provisional IDs removed through storage/history/filesystem/pool owners
  -> checked restoration -> terminal result
Restoration failure -> explicit restoration error -> stop further admission
```

The Windows reader holds its sharing restriction through metadata extraction and persistence.
POSIX rechecks the existing identity/size/timestamps at validation boundaries.
These observations protect one import operation. They do not detect accepted same-size changes
between imports. Do not add a content read to remove that accepted limitation.

## 5. Implementation scope and qualification work

The earlier I1-I5 implementation remains the base. This revision replaces its source identity
policy and adds the missing rendering evidence. Existing owner boundaries remain in use.

| Work | Modules | Required assertions | Expected changed lines | Status |
| --- | --- | --- | ---: | --- |
| Size-only import and refresh | ImportService, SourceFileReader/Store, editor lifecycle | Equal-size skips; changed size keeps user data; cancellation and rollback remain explicit | 700-1,400 | implemented |
| Remove prototype migration | database initialization, obsolete migration test | No prototype hash detection, copying, or deletion on open | 30-60 | implemented |
| Replacement-image qualification | Real application host, automation, GPU sink, preview/export | New pixels with preserved exposure; reference B comparison; saved rating/history and pixels after reopen | 300-650 | complete on Windows/CUDA |
| Current specification | This file, parent, roadmap and maintenance references | Current size policy; historical evidence clearly identified | 350-800 | complete |

Read current AGENTS.md and applicable skills before further implementation. Use focused owner
operations and minimal change descriptions. Keep temporary outputs in repository-root build/tmp.
Do not create another build directory, introduce a weaker rendering backend, or run the full suite.

The rendering test writes A and B as separate valid image files with different pixels and sizes.
It imports A, assigns four stars, opens A, applies +0.5 EV, and captures the initial outputs.
It replaces A's source while A is open, then reimports through `library.import` and the normal
ImportExportHandler. It requires an updated occurrence and a newly presented editor frame.
It compares refreshed preview/export pixels with a fresh import of B using the same edit.
It also requires a visible difference from A and from unedited B. RGB8 reference comparisons allow
at most one channel value of rounding error. Project reopen must retain the Image ID, commit head,
rating, exposure value, and replacement pixels. TIFF and Bayer DNG use the real GPU render path.

## 6. Current acceptance matrix and commands

| Behavior | Executable evidence |
| --- | --- |
| Paths and folder boundaries | SourceImagePathTest; SameJobEquivalentPathSpellingsCreateOnePersistedEntry; IdenticalBytesInDifferentFoldersCreateSeparateEntries |
| Size-only comparisons and source I/O | ReportsFileSizeWithoutReadingSourceContents; SamePathAndSizeSkipsWithoutInspectingChangedContents; ChangedSizeUpdatesWithoutReadingSourceContents |
| Existing user data and shared paths | ChangedSizeUpdatesTheExistingSourceAndPreservesItsRating; ChangedSizeRefreshesEveryExistingSamePathEntry; FailedSourceSizeUpdateKeepsThePreviousSizeAndAllUserState |
| Persistence and independent entries | SavedDuplicateImportReopensWithOneSamePathEntry; SkipDisabledCreatesIndependentEntriesAndRecordsEverySize |
| Cancellation and write errors | PreCanceledJobReportsEveryOccurrenceExactlyOnce; CanceledJobFinalizesCompletedEntriesExactlyOnce; ImageWriteFailureCannotBeReportedAsImported; RootWriteFailureRemovesProvisionalBinding; RestorationFailureIsReturnedAndStopsLaterAdmission |
| Request/UI/automation policy | ImportBehaviorPreferencesTest; ImportExportHandlerTest; AutomationLibraryCommandsTest |
| Editor save/navigation ordering | SourceReloadSavesAndReacquiresTheSameImage; SourceRefreshWaitsForLoadingAndRunsOnceOnTheSameImage; NavigationAwayClearsThePendingSourceRefresh |
| Actual replacement pixels and preserved edits after reopen | RasterReplacementRefreshesPreviewAndExportAndPreservesEditsAfterReopen; BayerRawReplacementRefreshesPreviewAndExportAndPreservesEditsAfterReopen |

Run Windows builds through PowerShell from the repository root. Reuse win_release/build/release.
Allow at least 10–20 minutes per build, and poll the same healthy process while it works.

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4 --target ImportSourceReplacementRenderingTest ImportDuplicateSkippingTest
$env:PATH = "$PWD/vcpkg/installed/x64-windows/bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
ctest --test-dir build/release --output-on-failure -j 1 -R '^(ImportSourceReplacementRenderingTest|ImportDuplicateSkippingTest|SourceFileReaderTest|SourceImagePathTest)\.'
```

Run the direct caller suites when their code changes. Do not run the full suite without an
explicit user request. Record selected, passed, failed, skipped, backend and platform limits.
Do not claim the physical-HDD throughput or macOS path is verified from Windows tests.

## 7. Detailed phases: historical implementation records

The following completion records describe the hash prototype on 2026-11-09. Keep them as
historical evidence only. Sections 1–6 above define the current behavior and APIs.

#### I1 completion record (2026-11-09)

**Status:** complete on Windows. Utilities and fingerprint storage use 613 changed lines across
13 files, including utility tests and build registration. Integration assertions are counted under I5.

`NormalizeSourceImagePath` owns absolute lexical normalization and Windows invariant case folding.
`SourceFingerprintReader` streams SHA-256 with a 64 KiB buffer and holds the protected read handle
through metadata and persistence. `SourceFingerprintStore` owns scoped candidate reads and
validated identity transactions through `Database::GetConnectionGuard`. The existing Database
accessor is sufficient; no additional Storage forwarding API or Image copy was needed.
`ImageStore` invalidates identity when the source binding changes, in the same transaction, and
removes identity with its Image owner. The positional Image schema and project version are unchanged.

**Success chain:** absolute source -> normalizer -> scoped bound-ID lookup -> protected source read
-> validated fingerprint transaction -> persisted identity with explicit provenance.

**Failure chain:** invalid digest/provenance, absent binding, or source read error -> exception
-> no identity row; a failing batch rolls back its earlier inserts. Existing identity is never replaced.

**Executed evidence:** SourceImagePathTest, SourceFingerprintTest, and the I5 storage/reopen tests.
`LegacyCurrentSourceInitializationSurvivesReopenWithoutHistoricalClaim` drops the optional table,
reopens the existing database, compares Image/root/Version rows, and verifies explicit legacy basis.
`InvalidFingerprintOrMissingBindingCannotPublishPartialRows` checks validation, batch rollback,
and preservation of accepted identity. The selected ProjectPackageBackendTest checks the existing
package byte layout. Commands and totals are in section 11. Symlink creation and macOS execution
are recorded there as unavailable, not as passed.

#### I2 completion record (2026-11-09)

**Status:** complete. I2 and I3 share one ImportService rewrite and its declarations/log changes:
894 changed lines across eight core files. Count this shared change once. The UI removal of the
second sync is included in I4's adapter count. Final integration tests are included in I5.

The submission executor owns bounded batches of 64 entries. Workers return source-read and root
encoding outputs while Image data remains in its pool owner. New focused
`ImagePoolService::PersistImportedImages` and `DiscardImportedImages` operations affect only the
admitted IDs. The existing root writer and filesystem owner remain responsible for their writes.
`SyncImports` remains a checked compatibility API; UI completion no longer calls it.

**Success chain:** metadata workers -> submission flush -> Image insert -> filesystem sync
-> root write -> fingerprint batch -> source validation -> success counts -> one terminal callback.

**Failure chain:** failed worker or persistence operation -> actual occurrence error -> remove
provisional fingerprint/history/files/Image through their owners -> checked completion. A restoration
error is returned explicitly and prevents subsequent admission on the affected service.

**Executed evidence:** callback reads all four persisted owners; image/library/root/fingerprint
failure tests assert restored rows; an overlapping request receives BUSY while finalization is
active. `CanceledJobFinalizesCompletedEntriesExactlyOnce` cancels after one confirmed entry,
asserts its complete persistence and exactly two canceled remaining occurrences, and compares
submitted/finished task counts. The earlier pre-canceled test is now named
`PreCanceledJobReportsEveryOccurrenceExactlyOnce` to state its narrower assertion.

Owner testing exposed the existing ImagePoolService Write -> SyncWithStorage recursive lock error.
Write now calls the private already-locked helper; public sync still acquires its lock. The rating
and source-binding update tests pass through that owner path. Issue tracking is unavailable as
recorded in section 11.

#### I3 completion record (2026-11-09)

**Status:** complete on Windows. Shared core LOC is reported under I2.

Admission compares full normalized paths before creating a placeholder. A matching stored byte
size is required before hashing. Every recorded digest at that path must agree. Only the required
legacy IDs receive `legacy_current_source`, with an explicit import-log record. Every successful
new entry records import-time identity even when skipping is disabled. Progress, result, log,
RAW subsets, and automation processed counts include confirmed duplicate occurrences.

A pending-path set plus checked batch flush replaces the planned follower-index queue. On reaching
a pending repeat, submission waits for that representative's real outcome before deciding the next
occurrence. This uses the existing single submission owner, preserves order, and needs no added
version/cancellation mechanism. A database failure stops admission; it is not retried through followers.

**Success chain:** normalized same path -> byte size -> SHA-256 -> persisted representative
-> duplicate log and counters; distinct path -> independent Image/root/fingerprint.

**Failure chain:** size/hash mismatch -> SOURCE_CHANGED with no new entry or skip; failed
representative -> actual failure and no follower success; read/comparison error -> explicit failure.

**Executed evidence:** repeated same paths, equivalent spellings, equal bytes in different folders,
same names with different contents, conflicting same-path identities, disabled skipping, all-duplicate
completion, RAW subset accounting, and unavailable unrelated sources. Retained rows, inactive edited
Versions, undone active history, mask assets, ratings, and memberships are compared across reopen.
The existing 1,100-distinct-hard-link capacity test passes with skipping enabled. Windows source
replacement evidence comes from `OpenReadProtectsTheMetadataReopenBoundaryFromReplacement`: a held
production reader prevents delete and write/truncate after hashing. This tests the actual reader
boundary; it does not claim a service-level injected metadata pause or macOS execution.

#### I4 completion record (2026-11-09)

**Status:** complete. Preference, adapters, UI, translations, caller tests, registration, and VI
documentation use 606 changed lines across 24 files, excluding this completion record.

`ImportBehaviorPreferences` owns the default-on QSettings value on the GUI thread. ApplicationModuleHost
owns and exposes it. Settings loads a pending scalar and applies through that owner; workers receive
only `ImportOptions::skip_duplicates_`. Automation validates an optional boolean override for file,
nonrecursive folder, and recursive folder requests. ImportExportHandler retains requested repeated
occurrences, snapshots the option once, and publishes separate duplicate/RAW duplicate counts.
Progress rings, task results, completion text, and Nikon reimport progress include duplicates.

**Success chain:** Settings Done or explicit library.import option -> resolved request bool
-> ImportService -> confirmed import/skip results -> task counts, progress, and package save.

**Failure chain:** invalid override -> InvalidParams before admission; Escape dismissal -> saved
preference unchanged; source/storage error -> existing service error presentation.

**Executed evidence:** three isolated preference tests; real Settings QML Apply/dismiss/reopen methods;
handler tests; automation file/folder overrides and package reopen; common file-picker/drop adapter
test changes the preference while a job runs and verifies that only the next job changes policy.
An empty native-picker result starts no job. Native file and folder selectors and the folder scan/type
dialog remain in their existing call chains.

Manual Windows checks used the built application: Welcome -> Settings before entering a project;
default enabled checkbox and help text; Tab focus and Space toggle; Escape discard and reopen;
Done persistence and reopen; both Alcedo and Classic themes; and the 1,214 by 793 window layout.
No clipping was observed in the changed Import section. Original Alcedo theme and enabled preference
were restored. The built test instance was closed. The Welcome Settings text action and popup order
were necessary to make this application-wide setting reachable before entering a project.

#### I5 completion record (2026-11-09)

**Status:** complete for Windows qualification. The focused production-owner integration fixture is
533 lines. It uses real ProjectService, ImportService, storage, filesystem, and history owners.
Caller and utility test LOC is counted under I1/I4. New tests have no production-only test switches.

**Success chain:** real service or GUI/automation adapter -> checked import/skip -> project save
-> reopen -> complete retained-row, independent-path, provenance, and count assertions.

**Failure chain:** source/hash cancellation or real owner write error -> explicit final result
-> restored provisional IDs or explicit restoration error -> unchanged accepted entries.

Section 11 gives the commands, final totals, resource measurements, and platform limits. Earlier
failed iterations were resolved before the final run: owner recursive locking, a test-held history
service keeping the database open during reopen, and an old missing-file test expecting no log.
The hard-link test requires native filesystem access outside the sandbox; its workload was retained.

## 11. Execution evidence (2026-11-09)

### 11.1 Environment and build results

All product and test builds completed with exit code 0. The existing `build/release` directory
was reused; no debug build existed. Its cached Qt installation is `C:/Qt/6.12.0/msvc2022_64`.
Tests were enabled with `ALCEDO_BUILD_TESTS=ON` through the supported win_release preset.
The cached compiler is MSVC 14.44. Set `VCVARS_USER_VERSION=14.44.35207` so the wrapper selects
its matching headers rather than the machine's newer default 14.51 headers.

Commands ran in PowerShell from the repository root. Configure used:

```powershell
Set-Location C:/Users/Haidong/Data/Photography/AlcedoStudio
$env:VCVARS_USER_VERSION = '14.44.35207'
cmd /c scripts\msvc_env.cmd --preset win_release -DCMAKE_PREFIX_PATH="C:/Qt/6.12.0/msvc2022_64/lib/cmake" -DALCEDO_BUILD_TESTS=ON
```

Targets successfully built in incremental invocations: `SourceImagePathTest`,
`SourceFingerprintTest`, `ImportDuplicateSkippingTest`, `ImportServiceTest`,
`ImportContentClassificationTest`, `ImportBehaviorPreferencesTest`, `ImportExportHandlerTest`,
`ShortcutSettingsQmlTest`, `AutomationLibraryCommandsTest`, `ProjectPackageBackendTest`,
`alcedo_main`, and `alcedo_cli`. The final focused build commands were:

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_release --target ImportDuplicateSkippingTest SourceFingerprintTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_release --target ImportDuplicateSkippingTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_release --target ProjectPackageBackendTest --parallel 4
```

The application was deployed in its build directory with the matching Qt `windeployqt`, then
launched for the native Settings checks. No installed application files were replaced.

### 11.2 Final targeted results

Final command, exit code 0, 45.81 seconds: **74 selected, 73 passed, 0 failed, 1 skipped**.
CTest names contain both the target prefix and the GoogleTest suite name.

```powershell
$env:PATH = "C:/Qt/6.12.0/msvc2022_64/bin;$PWD/vcpkg/installed/x64-windows/bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
ctest --test-dir build/release --output-on-failure -j 1 -R '^(SourceImagePathTest|SourceFingerprintTest|ImportDuplicateSkippingTest|ImportContentClassificationTest|ImportBehaviorPreferencesTest|ImportExportHandlerTest|AutomationLibraryCommandsTest)\.|^ShortcutSettingsQmlTest\..*ImportPreferenceApplyPersistsAndCancelDiscardsPendingChanges|^ImportServiceTest\..*(InitTest|ImportEmptyTest|MissingFileReportsFailureWithoutCreatingAPlaceholder|ImportToNonExistentDestination)$|^ProjectPackageBackendTest\..*PackageInTheExistingByteLayoutOpensAndWriterMatchesIt$'
```

| Target / selected cases | Passed | Skipped | Assertion coverage |
| --- | ---: | ---: | --- |
| SourceImagePathTest | 3 | 1 | Absolute lexical paths, Windows slash/case equivalence, distinct folders; symlink fixture unavailable |
| SourceFingerprintTest | 6 | 0 | Known digest, all chunks, real missing-file error, cancellation after a read chunk, source handle protection, memory bound |
| ImportDuplicateSkippingTest | 31 | 0 | Comparison table, all-owner completion, validation/rollback, owner failures, partial cancellation, BUSY, reopen, user-state preservation, resources |
| ImportBehaviorPreferencesTest | 3 | 0 | Missing/invalid default, persistent update, notification count |
| ImportServiceTest, selected cases | 4 | 0 | Initialization, empty request, missing-file occurrence log, invalid destination |
| ImportContentClassificationTest | 11 | 0 | Existing RAW/raster content behavior, category exclusions, 1,100 distinct hard-link capacity |
| ImportExportHandlerTest | 2 | 0 | Existing adapter behavior |
| ShortcutSettingsQmlTest, import preference case | 1 | 0 | Real production Settings pending/apply/dismiss/reopen methods and checkbox value |
| AutomationLibraryCommandsTest | 11 | 0 | File/folder policy, request override, invalid type, file/drop common adapter, task counts, package reopen, existing library commands |
| ProjectPackageBackendTest, existing-layout case | 1 | 0 | Existing package byte layout reads and writes identically |

The focused cancellation rerun also passed all three selected tests in 0.79 seconds:

```powershell
ctest --test-dir build/release --output-on-failure -j 1 -R '(PreCanceledJobReportsEveryOccurrenceExactlyOnce|CanceledJobFinalizesCompletedEntriesExactlyOnce|CancellationStopsTheReadWithItsActualError)$'
```

These are subsets of the final result, not additional unique cases. The previous 71-case run
had 70 passes and the same one skip. The final run adds partial cancellation, storage validation,
and package compatibility coverage. No unresolved failure remained in the selected tests.

The plan's descriptive test names map to concrete evidence as follows:

| Planned criterion | Executed evidence |
| --- | --- |
| Full path and platform spelling | EquivalentSeparatorsAndDotSegmentsPreserveTheFullPath; PathCaseFollowsThePlatformRule; SameJobEquivalentPathSpellingsCreateOnePersistedEntry |
| Existing optional schema and explicit legacy basis | LegacyCurrentSourceInitializationSurvivesReopenWithoutHistoricalClaim |
| Complete stored-user-state preservation | DuplicateSkippingPreservesCompleteStoredRowsAcrossReopen; RepeatedImportSkipsAndPreservesExistingRatingAndRoot; DuplicateIntoAnotherAlbumPreservesExistingMemberships |
| Size mismatch avoids hashing | SamePathSizeMismatchDoesNotReadTheChangedSourceContents |
| Equal-size replacement differs from accepted bytes | SamePathEqualSizeHashMismatchReportsSourceChanged; MultipleSamePathEntriesMustAgreeWithEveryRecordedDigest |
| Source reopening stability | OpenReadProtectsTheMetadataReopenBoundaryFromReplacement, at the production reader boundary |
| Persist before success / failed representative | CompletionRunsAfterImageLibraryRootAndFingerprintPersistence; FailedRepresentativeCannotProduceSuccessfulFollowerSkips |
| Explicit storage and restoration errors | ImageWriteFailureCannotBeReportedAsImported; LibraryWriteFailureRestoresProvisionalImages; RootWriteFailureRemovesProvisionalBinding; FingerprintWriteFailureRemovesProvisionalBindingAndRoot; RestorationFailureIsReturnedAndStopsLaterAdmission |
| Running request retains its policy / empty picker | NativePickerCancellationAndFileDropUseTheCommonPreference |
| File/folder request override and package reopen | AutomationOmittedOptionInheritsPreferenceAndExplicitOptionOverridesIt; FolderRequestsKeepThePreferenceAndPerRequestOverride |

### 11.3 Source I/O and memory measurements

Measured cases ran directly with GoogleTest XML output, using the same PATH and serial runtime:

```powershell
& build/release/alcedo_studio/tests/app/ImportDuplicateSkippingTest_runtime/ImportDuplicateSkippingTest.exe --gtest_filter=ImportDuplicateSkippingTest.DistinctPathThroughputWithBothPoliciesPreservesEveryEntry:ImportDuplicateSkippingTest.SamePathSizeMismatchDoesNotReadTheChangedSourceContents --gtest_output=xml:build/tmp/import_duplicate_skipping/io-measurements.xml
& build/release/alcedo_studio/tests/utils/SourceFingerprintTest_runtime/SourceFingerprintTest.exe --gtest_filter=SourceFingerprintTest.StreamingFingerprintMemoryDoesNotGrowWithSourceFileSize --gtest_output=xml:build/tmp/import_duplicate_skipping/memory-measurement.xml
```

All three passed. The local 128-distinct-hard-link workload took **430 ms with skipping enabled**
and **413 ms with skipping disabled**. It asserted 256 independent saved bindings, roots, and
fingerprints across both requests. This is one local measurement, not a general performance target.
Both policies perform the required initial complete hash for each new entry.

For a recorded same-path source enlarged to 32 MiB, the failed comparison recorded **0 bytes**
of additional process ReadTransferCount and returned SOURCE_CHANGED. An unrelated missing legacy
source was not opened during distinct-path admission. Candidate lookup reads minimal project
binding paths once, normalizes them once, and retains only requested matches; it does not read
other source files.

Hashing a 64 MiB fixture recorded **0 bytes above the process's prior peak working set**, with
an asserted increase below 8 MiB. This does not mean zero total memory use. Each reader uses a
fixed 64 KiB buffer; at most 64 admitted entries occupy one batch. On macOS, source validation
also rereads the digest; that platform cost was not measured here.

### 11.4 Review, limits, and tracking

The feature changes before these roadmap records total **2,646 changed lines**: I1 613,
shared I2/I3 894, I4 606, and I5 533. This includes production code, tests, build files,
translations, and the VI update. The prerequisite LF-only commit adds 34 changed lines in
`import_log.hpp`; it is separate from content edits. Each implemented module slice is below
2,000 changed lines. Section 11.5 records per-file size and diff counts.

New data types contain source identity or necessary algorithm outputs. Pending state contains
provisional IDs, paths, a pool pin, and the worker future. It does not duplicate Image metadata
or editing state. Source readers own their native handles until batch publication. New stores
hold Database references, not long-lived connections. No new first-party forward declaration
or OpenCV umbrella include was introduced. Changed and new files use LF. Changed C++ lines and
new C++ files were formatted, and `git diff --check` passed. Repository terminology scans included
the whole roadmap tree and filenames; unrelated existing matches were left outside this scope.

Concrete limitations:

- macOS build, path case behavior, and POSIX source-change validation were not executed on this
  Windows machine. Their source paths exist, but no macOS result is claimed.
- `DifferentUserVisibleSymlinkPathsRemainSeparate` skipped because Windows does not grant symlink
  creation privilege. An attempt outside the sandbox confirmed the same restriction. No system
  security setting was changed. Distinct hard-link paths were exercised successfully.
- Native hard-link fixtures require the test command to run outside the sandbox. Sandbox denial
  was not a product regression and did not justify replacing the 1,100-path workload.
- ImportPipelineDocumentTest was not run: the configured CUDA condition is enabled, but its required
  `sample_images/raw/linear_dng/mfzoty.dng` fixture is absent. Existing RAW/raster classification
  tests and the new real root-persistence assertions passed.
- The full test suite and unreliable WorkspaceShell synthetic-input tests were not run. The
  Settings QML test invokes actual pending/apply/reopen methods; native checks verify input.
- Native picker cancellation was verified through its empty-result adapter. No import or deletion
  was performed in the user's Photos project during manual verification.

GitHub CLI authentication is valid for ClearSights, and `origin` is the user's administrator-owned
fork `ClearSights/AlcedoStudio`. GitHub reports `hasIssuesEnabled=false`. Issue search/creation and
project-status updates therefore cannot complete there. No issue, comment, remote push, or upstream
change was made. In addition to the planned persistence defects, execution observed and fixed the
ImagePoolService recursive lock failure described under I2; it could not receive a tracking issue.
The Welcome Settings access gap found during manual verification was fixed in I4.

Temporary logs and measurement XML were kept under `build/tmp/import_duplicate_skipping/` during
execution. Their durable commands, counts, and measurements are recorded above; task-specific
temporary files are removed at completion. This plan is the retained evidence record.


### 11.5 Changed-file size review

Counts below compare feature content with `6f4857f6` and include untracked new files. Roadmap
record counts are shown separately from the 2,646-line feature total. The LF-only prerequisite
is not counted a second time. Added/deleted counts are line counts, not executable LOC.

The six existing production files above 1,000 lines retain these boundaries:

- `ImportExportHandler` still owns import/export UI lifecycle and presentation. Its import job,
  counters, finalization thread, and signals are the concrete state that an import-task controller
  would need to own in a broader separation. This change removes persistence decisions from that
  adapter and puts them in ImportService. It adds no storage policy to the large adapter.
- `library_commands.cpp` is the library command registry. The added code validates one option and
  routes it to existing application owners. Source comparison does not move into the registry.
- `SettingDialog.qml` composes category pages and owns pending settings values. A separate Default
  Behavior page would need to own pending import/mask controls with explicit apply/reset inputs.
  The 36-line addition uses the existing composition and a separate C++ preference owner; no
  application/import work is placed in QML.
- The UI CMake list and the two translation catalogs are declarative registries. Their two-line
  registration and localized strings do not create new mutable-state owners.

No new production file exceeds 392 lines; the focused integration test is 533 lines. Splitting
methods into additional files without moving their state was not used as a size workaround.

| File (repository relative) | Total lines | Added | Deleted |
| --- | ---: | ---: | ---: |
| `alcedo_studio/src/app/CMakeLists.txt` | 759 | 1 | 1 |
| `alcedo_studio/src/app/image_pool_service.cpp` | 207 | 25 | 0 |
| `alcedo_studio/src/app/import_service.cpp` | 392 | 341 | 417 |
| `alcedo_studio/src/include/app/image_pool_service.hpp` | 153 | 10 | 2 |
| `alcedo_studio/src/include/app/import_service.hpp` | 216 | 31 | 22 |
| `alcedo_studio/src/include/storage/store/image/source_fingerprint_store.hpp` | 44 | 44 | 0 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/application_module_host.hpp` | 276 | 6 | 0 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/import_behavior_preferences.hpp` | 26 | 26 | 0 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/import_export.hpp` | 343 | 17 | 2 |
| `alcedo_studio/src/include/utils/file/source_fingerprint.hpp` | 57 | 57 | 0 |
| `alcedo_studio/src/include/utils/file/source_image_path.hpp` | 14 | 14 | 0 |
| `alcedo_studio/src/include/utils/import/import_error_code.hpp` | 29 | 4 | 0 |
| `alcedo_studio/src/include/utils/import/import_log.hpp` | 125 | 26 | 0 |
| `alcedo_studio/src/storage/CMakeLists.txt` | 65 | 2 | 1 |
| `alcedo_studio/src/storage/store/database.cpp` | 130 | 8 | 0 |
| `alcedo_studio/src/storage/store/image/image_store.cpp` | 207 | 44 | 6 |
| `alcedo_studio/src/storage/store/image/source_fingerprint_store.cpp` | 106 | 106 | 0 |
| `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt` | 1266 | 2 | 0 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/application_module_host.cpp` | 843 | 4 | 0 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/import_behavior_preferences.cpp` | 22 | 22 | 0 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/import_export.cpp` | 1151 | 43 | 51 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/nikon_he_recovery_controller.cpp` | 504 | 4 | 2 |
| `alcedo_studio/src/ui/alcedo_main/application_module_qml_types.cpp` | 74 | 3 | 0 |
| `alcedo_studio/src/ui/alcedo_main/automation/automation_task_tracker.cpp` | 285 | 14 | 10 |
| `alcedo_studio/src/ui/alcedo_main/automation/commands/library_commands.cpp` | 1035 | 27 | 8 |
| `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_en.ts` | 6860 | 40 | 0 |
| `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts` | 9156 | 40 | 0 |
| `alcedo_studio/src/ui/alcedo_main/qml/AppDialogs.qml` | 407 | 3 | 2 |
| `alcedo_studio/src/ui/alcedo_main/qml/ImportProgressOverlay.qml` | 209 | 15 | 3 |
| `alcedo_studio/src/ui/alcedo_main/qml/NikonHeRecoveryDialog.qml` | 508 | 7 | 3 |
| `alcedo_studio/src/ui/alcedo_main/qml/SettingDialog.qml` | 1163 | 36 | 0 |
| `alcedo_studio/src/ui/alcedo_main/qml/ShellSignals.qml` | 120 | 1 | 1 |
| `alcedo_studio/src/ui/alcedo_main/qml/WelcomeDialog.qml` | 685 | 12 | 3 |
| `alcedo_studio/src/utils/CMakeLists.txt` | 78 | 7 | 0 |
| `alcedo_studio/src/utils/file/source_fingerprint.cpp` | 114 | 114 | 0 |
| `alcedo_studio/src/utils/file/source_image_path.cpp` | 34 | 34 | 0 |
| `alcedo_studio/tests/app/CMakeLists.txt` | 981 | 21 | 0 |
| `alcedo_studio/tests/app/import_duplicate_skipping_test.cpp` | 533 | 533 | 0 |
| `alcedo_studio/tests/app/import_service_test.cpp` | 771 | 9 | 5 |
| `alcedo_studio/tests/automation/automation_library_commands_test.cpp` | 290 | 96 | 0 |
| `alcedo_studio/tests/ui/import_behavior_preferences_test.cpp` | 42 | 42 | 0 |
| `alcedo_studio/tests/ui/shortcut_settings_qml_test.cpp` | 930 | 22 | 0 |
| `alcedo_studio/tests/utils/CMakeLists.txt` | 53 | 13 | 0 |
| `alcedo_studio/tests/utils/source_fingerprint_test.cpp` | 119 | 119 | 0 |
| `alcedo_studio/tests/utils/source_image_path_test.cpp` | 44 | 44 | 0 |
| `docs/VI/README.md` | 684 | 16 | 2 |
| `docs/roadmap/alcedo_studio/storage/duplicate_image_cleanup_and_import_plan.md` | 311 | 67 | 20 |
| `docs/roadmap/alcedo_studio/storage/import_duplicate_skipping_plan.md` | 994 | 407 | 56 |

## 12. Combined-reference qualification (2026-11-10)

This record describes the combined reference branch before the split. Maintenance targets below
are absent from this import-only branch. Isolated import verification is recorded in its commit
and the branch split report. No PR or push was created.

The Release app and focused test targets built successfully in the existing `build/release` tree.
Commands ran through PowerShell and `scripts/msvc_env.cmd`. The cached toolchain uses MSVC 14.44.
Set `VCToolsVersion=14.44.35207` and `VSCMD_ARG_VCVARS_VER=14.44` for the matching headers.
TEMP/TMP pointed to `build/tmp/source_replacement_fix/compiler_temp`. PROTOC pointed to the
existing `build/release/third_party/grpc/third_party/protobuf/protoc.exe` for the sidecar build.

Built targets: `ImportSourceReplacementRenderingTest`, `ImportDuplicateSkippingTest`,
`ProjectMaintenanceServiceTest`, `ProjectMaintenanceStorageTest`, `ProjectMaintenanceApplyTest`,
`ProjectMaintenanceModelTest`, `ProjectMaintenanceControllerTest`, and `alcedo_main`.

Final command, exit code 0, 49.73 seconds: **96 selected, 95 passed, 0 failed, 1 skipped**.

```powershell
ctest --test-dir build/release -j 1 --output-on-failure -R '^(ImportSourceReplacementRenderingTest|ImportDuplicateSkippingTest|SourceFileReaderTest|SourceImagePathTest|ProjectMaintenanceServiceTest|ProjectMaintenanceStorageTest|ProjectMaintenanceApplyTest|ProjectMaintenanceModelTest|ProjectMaintenanceControllerTest)\.'
```

| Target | Passed | Skipped | Evidence |
| --- | ---: | ---: | --- |
| SourceImagePathTest | 3 | 1 | Full paths, separator/case rules, distinct folders; symlink creation unavailable |
| SourceFileReaderTest | 4 | 0 | Handle protection and size without source-content reads |
| ImportDuplicateSkippingTest | 33 | 0 | Size-only admission, changed-source user data, saved reopen, owner failures and cancellation |
| ProjectMaintenanceServiceTest | 13 | 0 | Marks protect duplicate and missing-source entries |
| ProjectMaintenanceStorageTest | 12 | 0 | Marks added after review reject the entire batch without partial removal |
| ProjectMaintenanceApplyTest | 11 | 0 | Revalidation and retained color/deletion marks after package reopen |
| ProjectMaintenanceModelTest | 5 | 0 | Protected rows, pagination and explicit selection |
| ProjectMaintenanceControllerTest | 12 | 0 | Task drain, staged application, reopening and recovery |
| ImportSourceReplacementRenderingTest | 2 | 0 | Real TIFF/Bayer DNG replacement pixels, preserved exposure, rating, Image ID and commit after reopen |

The new rendering tests use the real application host, normal import handler, editor service,
CUDA frame sink, preview renderer, export queue, and project persistence. Both edited preview and
edited export must differ from unedited B. Refreshed and reopened outputs must match independently
imported edited B within one RGB8 channel value. No mock renderer supplies expected pixels.
The Bayer fixture is an uncompressed RGGB DNG with the same camera/color metadata in A and B.
The TIFF case also changes image dimensions. These are regression fixtures, not HDD benchmarks.

Database initialization now creates only the source-size table for this feature. The obsolete
prototype migration and its migration test were removed. Production source contains no
ImageSourceFingerprint/SourceFingerprint implementation. The existing fresh-storage assertion
still verifies that imports do not create a fingerprint table.

No full test suite or macOS/Metal execution was run. Windows symlink creation was unavailable.
Physical 6,000-file HDD throughput was not measured. Earlier hash tests and measurements in
sections 7 and 11 describe the historical prototype only.
