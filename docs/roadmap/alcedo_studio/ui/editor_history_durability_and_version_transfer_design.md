# Editor History Durability and Version Transfer Design

Date: 2026-07-20

Status: historical design for the completed Phase 5F-5I implementation.

Phase 6C supersedes this document's array-owned Version timeline, `WorkingVersion`, cursor,
`RewriteTimeline`, image-scoped overlapping save, journal compaction, and Paste/Merge behavior. The
authoritative replacement is
[Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md).
This document remains as a record of the earlier recovery implementation and its failure analysis;
it must not be used to define new history APIs or product behavior.

Related roadmap:

- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)

## Purpose

This document separates three concerns that were previously all described as journaling:

1. `Version` and `EditTransaction` are the user-visible edit-history model.
2. `EditorTransactionJournal` is the application recovery log for edits that have not yet been
   materialized into DuckDB.
3. DuckDB owns database-page recovery and its own physical WAL.

The application recovery log is not a second user history. It durably records changes to the
active `WorkingVersion` so that the stored `Version` and pipeline projection can be reconstructed
after process termination. User Undo/Redo remains part of edit history and must not be confused
with recovery UNDO.

The design uses application-level no-steal materialization and REDO recovery. It does not implement
ARIES UNDO in Alcedo. DuckDB remains responsible for physical database recovery.

The ARIES comparison is precise:

| ARIES concept | Alcedo application layer |
| --- | --- |
| Log sequence number | Journal record sequence within one journal generation |
| Transaction commit record | `JournalBatchCommit` for one or more independent operation records |
| Dirty database page written before commit | Forbidden; unmaterialized edits remain outside DuckDB |
| REDO | Replay journal-committed records after the materialized journal head |
| UNDO | Not required because uncommitted records never modify DuckDB |
| Checkpoint | DuckDB recovery metadata plus an optional committed `MaterializedHead` record |
| Physical page recovery | DuckDB's own WAL and recovery implementation |

Calling this a "log of a log" obscures the ownership boundary. `EditTransaction` is domain data
that explains how a Version changes. The recovery log records durable changes to that domain data.
DuckDB then stores a materialized projection and protects its pages with its own WAL.

## Required terminology

Use the following terms consistently in code, tests, and later roadmap work:

- **Edit command**: one editor action that produces at most one `EditTransaction`. A slider drag is
  provisional while values are changing; releasing the slider or reaching the defined idle
  coalescing boundary finalizes the edit command.
- **Open edit command**: provisional editor state that has not produced an `EditTransaction`.
- **Transaction chain**: the ordered `EditTransaction` sequence owned by one `Version`.
- **Applied transaction chain**: transactions from the chain start through, but not including, the
  transaction at `cursor`.
- **Redo chain**: transactions at and after `cursor`.
- **Journal record chain**: checksum-valid records connected by increasing record sequence and the
  expected chain hash.
- **Written record**: all bytes reached the operating-system file API, but no durability claim has
  been made.
- **Journal-committed record**: the record is covered by a `JournalBatchCommit` record. Recovery may
  apply it.
- **Durable journal head**: the greatest operation-record sequence covered by a
  `JournalBatchCommit` whose file flush completed successfully in the running process.
- **Materialized journal head**: the greatest operation-record sequence included in an atomically
  committed DuckDB history/pipeline update.
- **Journal generation**: the recovery-log epoch for one image and one active Version.
- **Session generation**: an in-memory ownership fence used to reject stale asynchronous results.
  It is not a database transaction identifier.

Name edit-history and durability boundaries with edit commands and transactions. Input events such
as pinch do not define persistence semantics. Always describe the exact transaction chain, record
chain, cursor range, or durable/materialized head.

## State model

An edit command moves through these states:

```text
Open edit command
  -> Finalized EditTransaction
  -> Queued journal record
  -> Written journal record
  -> Journal-committed record
  -> Materialized in DuckDB
```

The editor may render provisional and finalized state before it is durable. The UI must expose
pending-save or save-error state until the journal writer reports success. In this historical
Phase 5 design, image switch, workspace exit, application shutdown, Paste, and Merge first complete
a save checkpoint: they finalize the open edit command and wait asynchronously for the journal batch
commit before releasing the image write lock or starting an operation that depends on the saved
state. The GUI thread never waits on file I/O.

Maintain separate monotonically increasing values per image/journal generation:

```text
next_record_sequence
written_record_sequence
durable_batch_commit_sequence
durable_operation_sequence
materialized_operation_sequence
```

Advancing one value must never imply that a later value also advanced.

Every journal record, including `JournalBatchCommit`, consumes a record sequence.
`durable_batch_commit_sequence` identifies the batch commit record itself.
`durable_operation_sequence` identifies the last edit-history operation covered by that commit.
`materialized_operation_sequence` identifies the last edit-history operation included in DuckDB.
Control-record sequences must never be mistaken for materialized edit-history operations.

## Current implementation differences

Phase 5F completed checksummed in-memory record framing, atomic `RewriteTimeline`, record-chain
validation, and the independent simulator. Phase 5G completed `EditorJournalWriter`,
`JournalBatchCommit`, file flush, and durable sequence publication. Phase 5H completed
`EditorHistoryMaterializer`, atomic DuckDB materialization with `EditorRecoveryMetadata`, journal
recovery REDO, create-new compaction, injectable file faults, and diagnostic bundles.

The following behaviors remain for later shared adjustment-transfer integration:

- `AdjustmentTransferService::Apply(kPaste)` currently uses the live target pipeline as the new
  Version base. The required base is the target image's import/default pipeline.
- `AdjustmentTransferService::Apply(kMerge)` currently creates a transaction-free materialized
  Version. It must instead copy the active applied transaction chain and append incoming
  transactions.
- Paste/Merge Version publication should call the same atomic history/pipeline/recovery path as
  editor materialization rather than separate `SaveHistory()` / `SavePipeline()` service calls.

These differences do not reopen the completed Phase 5F–5H recovery journal algorithm.

## Exact journal flush procedure

Every finalized `EditTransaction`, `CursorMove`, or `RewriteTimeline` is immediately submitted to
the image-scoped journal writer. There is no periodic timer between transaction finalization and
journal submission. The writer may group records that are already queued when it starts one write,
but it must not delay a record merely to grow the group.

For one group, the writer performs the following operations in order:

1. Encode every operation as a complete checksummed record with consecutive sequence values.
2. Append the operation records with a short-write-safe write loop.
3. Append one `JournalBatchCommit` control record containing:
   - the previous journal batch commit sequence;
   - the first and last covered record sequences;
   - the last covered edit-history operation sequence;
   - the cumulative record-chain hash through the last covered record.
4. Call `FlushFileBuffers` on Windows or `fsync` on POSIX for the journal file handle.
5. Only after the flush succeeds, publish `durable_batch_commit_sequence` and
   `durable_operation_sequence`, complete the associated futures, and clear the UI pending-save
   state for those operations.

File creation and journal replacement additionally require a flush of the containing directory on
platforms where directory durability is explicit. Compaction continues to use create-new, flush,
verify, atomic replace, and directory flush. The active journal file is never rewritten in place.

`JournalBatchCommit` is a recovery control record, not an `EditTransaction`, Version row, or user
history entry. It is added in Phase 5G; Phase 5F's completed record framing and checksums remain the
foundation.

Recovery applies operation records only when they are covered by the last valid
`JournalBatchCommit` in the journal record chain. Complete operation records after that point are
ignored. This rule distinguishes an OS-level write from an application journal commit without
guessing from file length.

If the process terminates after the commit record was written but before the flush result was
reported to the application, reopening may observe either the preceding batch or the new batch.
Both are valid atomic outcomes. It must never observe part of one operation record or only one half
of `RewriteTimeline`.

## Autosave and DuckDB materialization

Autosave has two separate jobs:

1. Immediate journal durability for each finalized edit command, as defined above.
2. Background materialization of journal-committed records into DuckDB.

Materialization may be coalesced for throughput. It is also forced by image switch, workspace exit,
orderly shutdown, Paste, Merge, and any operation that changes the active Version. Its scheduling
frequency does not change which journal records recovery may apply.

For a selected durable edit-history operation sequence `N`, materialization must use one DuckDB
connection and one DuckDB transaction. The database transaction performs all of the following:

1. Validate the image ID, active Version ID, journal generation, expected transaction-chain hash,
   and previously materialized record sequence.
2. Update the active Version transaction chain and cursor through journal record `N`.
3. Update the Version's cached final pipeline parameters.
4. Update the image's active pipeline parameters.
5. Upsert recovery metadata containing image ID, active Version ID, journal generation,
   `materialized_operation_sequence = N`, transaction-chain hash, and pipeline-parameter hash.
6. Commit the DuckDB transaction.

History, pipeline, and recovery metadata must not be saved by separate service calls. Add a narrow
storage operation that performs these writes on the same connection. The current sequence of
`SaveHistory()` followed by `SavePipeline()` is not atomic and is insufficient for this design.

After the DuckDB commit succeeds, the journal writer may append a `MaterializedHead` record in a
later journal batch. That marker accelerates diagnostics and compaction, but database recovery
metadata is authoritative. Correctness must not depend on writing the marker after the database
commit.

## Recovery algorithm

For each image with an active journal generation:

1. Let DuckDB finish its own recovery before application recovery begins.
2. Read the stored active Version, pipeline parameters, and recovery metadata in one consistent
   database snapshot.
3. Decode the journal record chain. Stop at the first malformed frame, checksum failure, sequence
   gap, identity mismatch, or chain-hash mismatch and preserve the original file for diagnostics.
4. Locate the last valid `JournalBatchCommit` and ignore later operation records.
5. Ignore edit-history operations at or before `materialized_operation_sequence` after validating
   the recorded chain hash at that point.
6. Replay later journal-committed records into the independent `JournalTimelineSimulator` and a
   temporary pipeline instance.
7. Materialize the recovered state with the single DuckDB transaction described above.
8. Append a `RecoveryMarker` in a later committed journal batch and schedule compaction.

Replaying the same committed record chain is idempotent because the database records the journal
generation, materialized operation sequence, and chain hash.

No application-level UNDO is required. Before materialization, uncommitted or incomplete records
have not modified DuckDB. During materialization, DuckDB atomically selects the old or new database
state. `CursorMove` and `RewriteTimeline` are edit-history operations replayed by REDO; they are not
recovery rollback records.

## Paste and Merge semantics

Library and Editor invoke the same `AdjustmentTransferService` application operation. UI adapters
may select targets and display progress, but they must not implement Version creation themselves.

An adjustment package is converted into ordered `EditTransaction` objects against the selected
target base. Package entries do not keep source-image transaction IDs. The target Version allocates
transaction IDs in order and regenerates transaction hashes.

### Common preparation

For each target image independently:

1. Acquire the image-scoped write lock.
2. If that image is open in Editor, finalize its open edit command.
3. Submit any resulting `EditTransaction` to the journal and complete a journal batch commit.
4. Materialize every journal-committed record for the current active Version into DuckDB.
5. Read the now-durable active Version and import pipeline parameters.

Failure in any preparation step leaves Paste/Merge unapplied. Batch transfer across multiple images
is atomic per image, matching the current partial-success result model; it is not one transaction
across all selected images.

### Paste

Paste creates a new Version whose transaction chain is initially empty and whose base is the target
image's import/default pipeline parameters. It does not inherit adjustments from the active Version.

The service then:

1. Imports the target image's import/default pipeline parameters into a temporary pipeline.
2. Converts each incoming package entry into an `EditTransaction` using the temporary pipeline's
   current operator state as the before value and the incoming state as the after value.
3. Appends those transactions to the new Version in package order and applies them to the temporary
   pipeline.
4. Stores the resulting pipeline parameters as the Version's materialized cache.

### Merge

Merge creates a new Version by copying the active Version's applied transaction chain. Transactions
in the active Version's redo chain are not copied. The source Version remains unchanged.

The service then:

1. Starts from the active Version's materialized pipeline parameters.
2. Copies the applied transactions into a new Version with a new stable Version ID.
3. Converts each incoming package entry into a new `EditTransaction` using the current merged
   pipeline state as its before value.
4. Appends and applies the incoming transactions in package order.
5. Stores the resulting pipeline parameters as the new Version's materialized cache.

This is copy semantics, not a persisted parent/child branch relation. A future version graph may add
parent metadata, but Paste/Merge must not depend on it.

### Atomic Version publication

Version creation remains outside the editor recovery journal only after common preparation has
materialized the current active Version. Publish the new Version with one DuckDB transaction that:

1. Inserts the complete new Version, including all copied and incoming transactions.
2. Sets it as the active Version in edit history.
3. Writes its final pipeline parameters as the image's active pipeline.
4. Starts a new journal generation whose base Version is the new Version and whose materialized
   operation sequence is zero.
5. Commits.

The application reports success and updates the editor only after this database commit. A process
termination before the commit leaves the old active Version and pipeline; a termination after the
commit leaves the complete new Version and matching pipeline. If the new journal file does not yet
exist, recovery creates an empty file from the committed generation metadata.

## Required service boundaries

Phase 5G and adjustment-transfer integration require these application/storage responsibilities:

- `EditorJournalWriter`: owns append, `JournalBatchCommit`, file flush, durable sequence, file
  rotation, and injected file failures.
- `EditorHistoryMaterializer`: owns replay through a selected durable record sequence and the single
  DuckDB history/pipeline/recovery update.
- `AdjustmentTransferService`: owns shared Paste/Merge preparation, transaction generation, and
  atomic Version publication for both Library and Editor.
- Image-scoped operation coordinator: serializes journal commit, materialization, Paste/Merge,
  checkout, and deletion for one image while allowing unrelated images to proceed concurrently.

`IEditorJournalPort::AppendBarrier` must be replaced with typed operations. A generic barrier cannot
state whether it finalizes an edit command, commits queued records, materializes DuckDB, or performs
all three.

## Crash-point outcomes

| Process termination point | Required reopened state |
| --- | --- |
| Before an edit command produces `EditTransaction` | Last journal-committed edit state |
| During operation-record write | Last `JournalBatchCommit` state |
| After operation-record write but before batch commit record | Last `JournalBatchCommit` state |
| During batch commit record or file flush | Previous or new complete journal batch |
| After journal flush but before DuckDB materialization | New edit state reconstructed by REDO |
| During DuckDB materialization | Old or new complete database state, selected by DuckDB |
| After DuckDB commit but before `MaterializedHead` | New database state; no duplicate replay |
| During Paste/Merge preparation | Current active Version including all journal-committed edits |
| During Paste/Merge DuckDB transaction | Old or complete new Version, never a mixed history/pipeline |
| After Paste/Merge commit but before UI notification | Complete new Version on reopen |

## Required tests

- `RecordsAfterLastJournalBatchCommitAreNotReplayed`
- `JournalFlushAdvancesDurableSequenceOnlyAfterSuccessfulFileFlush`
- `FailedJournalFlushKeepsEditPendingAndBlocksVersionTransfer`
- `MaterializationCommitsHistoryPipelineAndRecoveryMetadataTogether`
- `CrashAfterJournalCommitBeforeMaterializationRedoesEditExactlyOnce`
- `CrashAfterDatabaseCommitBeforeMaterializedMarkerDoesNotReplayTwice`
- `PasteStartsFromImportPipelineAndDoesNotInheritActiveAdjustments`
- `MergeCopiesAppliedTransactionChainAndExcludesRedoChain`
- `MergeCommitsOpenEditorTransactionBeforeAppendingIncomingTransactions`
- `LibraryAndEditorPasteUseTheSameApplicationServicePath`
- `PasteMergeFailureBeforeDatabaseCommitLeavesPriorVersionActive`
- `PasteMergeCompletionPublishesMatchingVersionAndPipeline`

The forced-termination harness must cover every row in the crash-point table. Assertions compare
Version ID, journal generation, transaction chain, cursor, transaction IDs, chain hash, pipeline
parameters, durable batch commit sequence, durable operation sequence, and materialized operation
sequence.
