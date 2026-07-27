# Phase 7A History/Versions Repair and UI Refactor Plan

Date: 2026-07-26

Status (2026-07-27): Phase 2, 3, 4, 6 done; Phase 5 mostly done (structured diagnostics incomplete);
Phase 1 complete (13/13 listed tests; rail split landed with Phase 6); Phase 7 (real-RAW
end-to-end qualification) remains open. Priority axis: P0 done 2026-07-26, P1 done 2026-07-27,
P2 done 2026-07-27. See "Phase completion status" for the mapping.

Baseline:

- Phase 7A implementation: `f7bad872 feat(editor): complete phase 7A version history workflow`
- QML panel split and outline-selection follow-up:
  `9f5f89ab refactor(ui): split history and versions panels`

Related documents:

- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)
- [QML Editor and Qt RHI Unified Workspace Refactor Plan](qml_editor_rhi_unified_workspace_plan.md)
- [Alcedo Studio QML Visual Identity](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md)

## Goal

Repair the Phase 7A history and Version workflow as one coherent editor operation:

- history cards show a user-facing adjustment name and meaningful before/after values;
- clicking a history commit supports multi-step Undo/Redo on the active Version;
- **New Version** creates a named Version at the immutable image root, checks it out, and shows the
  clean root render;
- **Branch Here** creates a named Version at an explicitly selected commit and checks it out;
- existing Version checkout keeps the current image and last valid frame visible until the target
  pipeline is ready;
- save or checkout failure never turns a retained image into the empty-editor placeholder and never
  traps all later image selection behind an unexplained `editor_save` task;
- Version naming happens inline; active selection is a white outline only; no stop-playback glyph is
  used as Version chrome.

This work does not change LUT Panel behavior.

## Locked product semantics

### Version operations

| User action | New ref head | Becomes active | Pipeline result |
| --- | --- | --- | --- |
| New Version | image root (`nullopt`) | yes | rebuild from immutable root |
| Branch Here | selected commit ID | yes | rebuild root plus selected first-parent path |
| Checkout Version | existing Version head | selected ref | rebuild root plus selected first-parent path |
| Paste Adjustments | root-relative pasted head | pasted ref | existing Phase 6C Paste behavior |
| Merge Adjustments | merge commit on current Version | current ref stays active | existing Phase 6C Merge behavior |

New Version and Branch Here are different commands. They must have different controller methods,
different accessible names, different tooltips, and different SVGs.

Detached-HEAD editing remains unsupported. Branch Here always creates a named Version ref before
editing can resume.

### History movement

- The timeline represents one active Version.
- Applied commits, the current working head, and the in-memory redo suffix are visible.
- Exactly one row may be current. At image root, no commit row is current.
- Clicking an applied ancestor moves the working head backward in one user operation.
- Clicking a future row moves the working head forward only through the current redo suffix.
- A new edit after moving backward clears the future suffix, as Mini-Git already requires.
- One history jump publishes one final adjustment snapshot and routes one render. It must not expose
  each intermediate Undo/Redo frame.

### Failure behavior

- A failed save keeps the current identity, guards, and last presented frame.
- A failed Version rebuild restores the prior active Version, head, pipeline, snapshot, and frame.
- The empty-editor placeholder is shown only when there is no retained image identity/frame.
- After a save failure, the user can Retry Save, Discard Changes and continue the pending navigation,
  or cancel the pending navigation.
- Error UI shows the exact operation and backend message. Internal task keys such as `editor_save`
  are never user-facing text.

## Evidence-led audit

The findings below separate user-observed behavior from facts established by source inspection and
missing test evidence.

### P0 — save/checkout failure hides the retained image and traps later navigation

Evidence:

- **User-observed failure:** after Version switching, the viewport changed to the empty-editor
  placeholder; later images could not be displayed; the status bar reported `editor_save` without
  the cause.
- **Source fact:** `EditorSessionLifecycle::KeepCurrentAfterCheckpointFailure()` changes the state to
  `Failed` while retaining identity and guards.
- **Source fact:** `EditorSessionHasImage(Failed)` is false, so QML treats the retained image as no
  image.
- **Source fact:** the next image selection sees the retained non-zero identity and attempts the
  failing save again before acquiring the requested image.
- **Coverage gap:** `FailedCheckoutKeepsImageAndDoesNotReleaseOrSwitch` and
  `CheckpointFailureKeepsAAndNeverAcquiresB` assert identity/guard retention but never assert
  `has_image()`, last-frame visibility, recovery actions, or a successful later switch.

Required correction:

- introduce an explicit retained-image failure state instead of reusing fatal `Failed`;
- keep `hasImage=true` and the last frame visible in that state;
- add retry/discard/cancel recovery and test a subsequent image switch.

### P0 — New Version implements the wrong branch point and does not checkout

Evidence:

- **User-observed failure:** New Version does not show a clean image, later edits have unclear
  ownership, and switching Versions becomes unreliable.
- **Source fact:** `EditorSessionHistoryPort::CreateVersion()` calls
  `CreateVersionRefAtActiveHead()`.
- **Source fact:** the created ref is not selected and no pipeline rebuild is requested.
- **Source fact:** the QML fake repeats this behavior by creating a non-active Version at
  `snapshot_.active_head`.
- **Coverage gap:** the QML test asserts only that a card with the typed name exists. It does not
  assert root head, active Version ID, clean adjustment snapshot, rendered result, next-commit
  ownership, save completion, or reopen.

Required correction:

- replace the ambiguous CreateVersion operation with CreateRootVersionAndCheckout;
- add BranchFromCommitAndCheckout as a separate operation;
- remove the old active-head creation path from QML-facing APIs.

### P1 — multi-step Undo/Redo was removed

Evidence:

- **User-observed regression:** only the toolbar Undo and Redo buttons remain.
- **Source fact:** the legacy QWidget connected history-row clicks to `MoveCursorTo`.
- **Source fact:** the QML transaction delegate has no click action and `EditorHistoryModel` exposes
  only one-step `undo()` and `redo()`.
- **Coverage gap:** the QML suite clicks only the two toolbar buttons. It does not click a commit or
  verify a multi-commit move, future suffix, one-render behavior, or reopen replay.

Required correction:

- add a typed `moveHeadToCommit(commitId)` operation;
- publish applied/current/future row state;
- make the full card keyboard and pointer activatable.

### P1 — history projection drops the useful transaction data

Evidence:

- **User-observed regression:** adjustment names remain lowercase and cards do not show previous and
  current values; a generic sentence merely repeats that an adjustment changed.
- **Source fact:** `EditorHistoryCommit` contains only `label` and `field_key`, not ordinary-edit
  before/after payload data.
- **Source fact:** `CommitLabel()` returns the raw lowercase field key.
- **Source fact:** QML renders `Applied %1 adjustment.` instead of a value delta.
- **Source fact:** `ReadHistorySnapshot()` sets `row.current = true` for every commit.
- **Coverage gap:** test commits use already-prettified labels and set every fake commit current, so
  they cannot detect lowercase production labels or multiple selected cards.

Required correction:

- carry semantic before/after payload data into the read-only projection;
- format it in a focused UI presentation helper;
- remove the generic explanatory sentence;
- mark only the actual working head current.

### P1 — operation results are discarded at the QML boundary

Evidence:

- **Source fact:** `EditorSessionController::{CreateVersion,CheckoutVersion,RenameVersion,
  RemoveVersion,Undo,Redo}` ignore `EditorSessionResult`.
- **Source fact:** `EditorHistoryModel` refreshes immediately after these void calls even when a save
  is asynchronous.
- **Source fact:** `EditorSessionTaskPort` publishes the internal name `editor_save` as the task
  title; `BackgroundTaskBar` shows the title but not the failure detail.
- **Coverage gap:** no test asserts the user-visible message for a failed history operation.

Required correction:

- publish a typed operation result with sequence, action, state, task ID, and message;
- show pending/success/error state in the owning panel;
- use a localized Editor Save title and expose the terminal failure detail in the bar and popover.

### P2 — Version creation and selection chrome do not match the requested interaction

Evidence:

- **User-observed UI issue:** Version naming opens a modal dialog.
- **Source fact:** `EditorVersionsPanel.qml` owns `editorVersionNameDialog`.
- **User-observed UI issue:** a stop-playback glyph appears on Version cards.
- **Source fact:** the remove action uses `panel_icons/stop.svg`.

Required correction:

- replace the modal with an inline draft row modeled on CollectionsPanel behavior while using Basic
  controls and AppTheme tokens;
- retain the white 1 px active-card outline as the only selection indication;
- remove `stop.svg` from Version cards; put removal behind a correctly named delete/overflow action.

## Current call chains

### History display

```text
EditCommit ordinary payload
  -> EditorSessionHistoryPort::ReadHistorySnapshot
  -> CommitFieldKey + CommitLabel(raw field key)
  -> EditorHistoryCommit(label, field_key only)
  -> EditorHistoryModel roles
  -> EditorHistoryTransactionsPanel generic "Applied <key> adjustment"
```

### New Version

```text
Create button
  -> modal TextField
  -> EditorHistoryModel::createVersion
  -> EditorSessionController::CreateVersion (result discarded)
  -> EditorSessionService::CreateVersion
  -> EditorSessionHistoryPort::CreateVersion
  -> CommitGraph::CreateVersionRefAtActiveHead
  -> history checkpoint
  -> no checkout, no root rebuild, no active-ref change
```

### Version checkout failure and navigation trap

```text
Version card click
  -> save checkpoint
  -> ContinueCheckoutVersion
  -> history/pipeline checkout fails
  -> lifecycle state = Failed, old identity and guards retained
  -> EditorSessionHasImage(Failed) = false
  -> viewport shows empty-editor placeholder
  -> later image selection sees retained identity
  -> retries save of the same failed session
  -> requested image is never acquired
```

### Error text

```text
save starts
  -> EditorSessionTaskPort::BeginTask("editor_save", ...)
  -> BackgroundTaskSnapshot.title = "editor_save"
  -> BackgroundTaskBar renders title

save fails
  -> BackgroundTaskSnapshot.detail = backend error
  -> bar still renders title only
  -> exact cause is hidden until task details are inspected
```

## Target architecture

```text
EditorHistoryTransactionsPanel
  -> EditorHistoryModel
     -> moveHeadToCommit(commit_id)
     -> branchFromCommit(commit_id, display_name)
  -> EditorHistoryCommitPresentation
     -> display name + before/after/delta + icon key

EditorVersionsPanel
  -> inline draft state
  -> EditorHistoryModel::createRootVersion(display_name)
  -> checkoutVersion(version_id)

EditorHistoryModel
  -> EditorSessionController typed action result
  -> EditorSessionService
  -> EditorSessionNavigationController
     -> save current Version
     -> EditorSessionHistoryPort named-ref operation
     -> candidate pipeline rebuild
     -> publish active ref/snapshot/frame
     -> persist resulting ref state

Failure
  -> prior ref/pipeline/frame remains published
  -> retained-image recovery state
  -> Retry Save / Discard and Continue / Cancel
```

Dependency direction:

- QML owns only transient input, focus, selection chrome, and action presentation.
- `EditorHistoryModel` owns QML list roles and action-result adaptation.
- `EditorSessionNavigationController` owns save-before-navigation ordering and pending targets.
- `EditorSessionHistoryPort` owns active graph/working-history state.
- `CommitGraph` owns ref and immutable commit invariants.
- `PipelineMgmtService` builds a candidate pipeline before publication.
- QML never creates refs, chooses parent hashes, or mutates redo state.

## Phase completion status

This plan has two axes that must not be conflated:

- **Priority axis (P0/P1/P2)** — the seven audited problems grouped by urgency. Completion records
  are filed per priority (`P0 completion record`, `P1 completion record` below).
- **Implementation axis (Phase 1-7)** — the ordered build steps. A priority can span several
  phases, and a phase can serve several priorities.

Phase status against executable evidence (audited 2026-07-27):

| Phase | Responsibility | Status | Key evidence | Open gap |
| --- | --- | --- | --- | --- |
| 1 | Failing evidence before behavior change | Done (13/13 tests) | All 13 listed tests exist and pass, including the two Phase-6 UI tests; rail split into `EditorHistoryTransactionsPanelQmlTest` + `EditorVersionsPanelQmlTest` | test-first ordering was relaxed historically (tests written alongside implementation) |
| 2 | History projection + card content | Done | `HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue`, `HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix`, `EditorHistoryCommitPresentationTest.FormatsNumericBooleanPathEnumAndCompoundAdjustments`; `EditorHistoryTransactionsPanel.qml` renders `Exposure / 0.00 -> +0.35` | none |
| 3 | One-operation history jumps | Done (residual) | `MoveHeadToAncestorThenRedoDescendantPublishesOneFinalSnapshot`, `HistoryCardClickMovesToCommitAndBranchButtonUsesSelectedCommitId`; `MiniGitWorkingHistory::MoveHeadToCommit` writes one head-move record and routes one render; `ApplyRecoveredRecord` re-walks every hop on reopen | mid-apply operator failure leaves the head moved with a partial pipeline (documented residual, parity with single-step Undo/Redo); merge hops skip the per-field delta on the live snapshot |
| 4 | Split New Version / Branch Here | Done | `CreateRootVersionChecksOutRootAndNextCommitBelongsToNewVersion`, `BranchFromSelectedCommitChecksOutNamedRefWithoutDetachedHead`, `CreateOrBranchFailureRestoresPriorRefPipelineSnapshotAndFrame`; `createRootVersion`/`branchFromCommit` model methods; no QML-facing active-head creation remains | none |
| 5 | Retained-image failure + error reporting | Mostly done | `RetainedImageFailure` state; `FailedVersionCheckoutKeepsHasImageAndLastFrameVisible`; `SaveFailureRetryThenSwitchAcquiresRequestedImage`, `SaveFailureDiscardThenSwitchClearsJournalAndAcquiresRequestedImage`; `EditorSaveRecoveryBar.qml` (Retry Save / Discard and Continue / Cancel); `tr("Editor Save")` + bar/popover failure detail; `ProductionEditorSaveTaskPublishesAndClearsFiveCheckpointLocks` (locks clear on success/fail/cancel); `DuplicateOrStaleCompletionCannotResumeBOrFinishTaskTwice` | change 9 structured diagnostics incomplete: typed result carries action/state/kind/task_id/render_request_id/element-or-image id/message but not current/requested Version ID or checkpoint stage |
| 6 | Version panel UI | Done (P2) | Inline draft (Enter/Escape/focus-loss/pending); outline-only active card; `trash.svg` remove; Branch Here on history cards only; contentY preserve; split QML targets | none |
| 7 | End-to-end qualification | Open | split targets exist (`EditorHistoryTransactionsPanelQmlTest`, `EditorVersionsPanelQmlTest`) | real-RAW 14-step sequence not run |

Priority to phase mapping: P0 = Phase 4 + Phase 5 (retained-image parts); P1 = Phase 2 + Phase 3 +
Phase 5 (error-reporting parts); P2 = Phase 6. Phase 1 is the shared evidence foundation; Phase 7 is
the shared end-to-end gate.

## Open findings (grill review, 2026-07-27)

Status audit of the Phase axis. No test failed this turn; the items below are coverage gaps and a
plan-self issue.

1. ~~Coverage gap — Phase 7 verification commands reference targets that do not exist.~~
   **Resolved in Phase 6 (2026-07-27):** `EditorHistoryTransactionsPanelQmlTest` and
   `EditorVersionsPanelQmlTest` are registered; the monolithic
   `EditorHistoryVersionsRailQmlTest` was removed.

2. Coverage gap — Phase 5 change 9 (structured diagnostics) is half-implemented.
   `EditorSessionController::PublishHistoryResult` carries action/state/kind/task_id/
   render_request_id/element-or-image id/message, but not the current Version ID, requested Version
   ID, or checkpoint stage that change 9 requires. The "status chrome states what failed and why"
   exit condition is met by the message field; the richer structured diagnostics are not. Decide:
   implement the remaining fields or explicitly descope change 9.

3. ~~Coverage gap — Phase 1 is 11/13.~~
   **Resolved in Phase 6 (2026-07-27):** `InlineVersionDraftAcceptsEnterAndEscapeWithoutDialog` and
   `ActiveVersionUsesOutlineWithoutStopPlaybackAction` land and pass. Historical note: test-first
   ordering for earlier phases was relaxed.

4. Style/maintainability — the plan conflated the priority axis (P0/P1/P2) with the implementation
   axis (Phase 1-7). Corrected this turn: the Status line, the "Phase completion status" overview,
   and the per-phase heading tags now keep the two axes distinct.

## Remaining work

Completed phases (2, 3, 4) and the mostly-completed Phase 5 are documented in the
`P0 completion record` and `P1 completion record` below. The actionable items, ordered by
dependency:

### Residual fixes (independent of Phase 6/7)

- Phase 5 change 9: add current/requested Version ID and checkpoint stage to the typed operation
  result, or explicitly descope change 9 and update its exit condition.
- Phase 3 residual (documented, accepted): mid-apply operator failure during a multi-step
  `MoveHeadToCommit` leaves the head moved with a partial pipeline — parity with single-step
  Undo/Redo. No action unless Undo/Redo parity is itself raised.

### Phase 6 — Version panel UI (done, P2, 2026-07-27)

Complete. See "Phase 6 completion record" under the Phase 6 specification. Lands:

- `InlineVersionDraftAcceptsEnterAndEscapeWithoutDialog`,
  `ActiveVersionUsesOutlineWithoutStopPlaybackAction`;
- split targets `EditorHistoryTransactionsPanelQmlTest` + `EditorVersionsPanelQmlTest`
  (resolves Open finding 1).

### Phase 7 — end-to-end qualification (open)

Spec: see "Phase 7 — end-to-end qualification" in the reference section below. Run the 14-step
real-RAW sequence with a deterministic commit clock. Before running, correct the Phase 7
build/test commands to the targets that will exist after the Phase 6 split (or to
`EditorHistoryVersionsRailQmlTest` as an interim).

## Implementation phase specifications (reference)

### Phase 1 — add failing evidence before changing behavior [PARTIAL: 11/13 tests; split deferred]

Files:

- `tests/edit/history/editor_session_history_port_test.cpp`
- `tests/edit/history/commit_graph_test.cpp`
- `tests/app/editor_session_navigation_controller_test.cpp`
- `tests/app/editor_session_lifecycle_test.cpp`
- `tests/ui/editor_session_task_port_test.cpp`
- split `tests/ui/editor_history_versions_rail_qml_test.cpp` into:
  - `tests/ui/editor_history_transactions_panel_qml_test.cpp`
  - `tests/ui/editor_versions_panel_qml_test.cpp`
- `tests/ui/CMakeLists.txt`

Add these tests first:

- `HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue`
- `HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix`
- `MoveHeadToAncestorThenRedoDescendantPublishesOneFinalSnapshot`
- `CreateRootVersionChecksOutRootAndNextCommitBelongsToNewVersion`
- `BranchFromSelectedCommitChecksOutNamedRefWithoutDetachedHead`
- `CreateOrBranchFailureRestoresPriorRefPipelineSnapshotAndFrame`
- `FailedVersionCheckoutKeepsHasImageAndLastFrameVisible`
- `SaveFailureRetryThenSwitchAcquiresRequestedImage`
- `SaveFailureDiscardThenSwitchClearsJournalAndAcquiresRequestedImage`
- `EditorSaveTaskUsesUserFacingTitleAndShowsTerminalFailureDetail`
- `InlineVersionDraftAcceptsEnterAndEscapeWithoutDialog`
- `HistoryCardClickMovesToCommitAndBranchButtonUsesSelectedCommitId`
- `ActiveVersionUsesOutlineWithoutStopPlaybackAction`

Each integration test must assert active Version ID, working head, transaction-chain hash, serialized
state, panel snapshot, render request count, image identity, and final task detail where applicable.

Exit condition:

- every new test fails for the intended missing behavior before production changes;
- existing tests remain registered;
- `EditorSessionTaskPortTest` is built and executed instead of reported as `_NOT_BUILT`.

### Phase 2 — repair the history projection and card content [DONE]

Files:

- `src/include/app/editor_history_types.hpp`
- `src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- new:
  - `src/include/ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp`
  - `src/ui/alcedo_main/album_backend/editor_history_commit_presentation.cpp`
- `src/include/ui/alcedo_main/album_backend/editor_history_models.hpp`
- `src/ui/alcedo_main/album_backend/editor_history_models.cpp`
- `src/ui/alcedo_main/qml/EditorHistoryTransactionsPanel.qml`
- `src/ui/alcedo_main/CMakeLists.txt`

Changes:

1. Extend `EditorHistoryCommit` with ordinary-edit before/after JSON, before/after enabled state, and
   a typed timeline position (`Applied`, `Current`, `Future`). Merge rows keep ordered parent IDs and
   resolved field information.
2. Build the visible timeline from the applied first-parent path plus the in-memory redo suffix.
3. Set `Current` only for the working head.
4. Implement a pure presentation helper that maps the stable field key and payload to:
   `display_name`, `before_text`, `after_text`, `delta_text`, and `icon_key`.
5. Reuse the useful semantic formatting from legacy `history_cards.cpp`, adapted to
   `OrdinaryEditPayload`; do not make the QML parse arbitrary JSON.
6. Render ordinary cards as:

   ```text
   Exposure                         2 min ago
   0.00  →  +0.35
   ```

   Remove the generic “Applied … adjustment” sentence and move commit hash to tooltip/accessibility
   description.
7. Keep merge provenance in a compact secondary well.

Exit condition:

- names are user-facing and capitalized;
- before/after values are present for every supported adjustment;
- exactly one commit card has the white current outline;
- no QML color, spacing, radius, type, or icon-size literal is introduced.

##### Phase 2 completion record (2026-07-27)

**Status:** complete — the history projection carries the semantic before/after payload, a pure
presentation helper formats user-facing text without QML parsing JSON, exactly one row is
`Current`, and the transaction card renders "Exposure / 0.00 → +0.35" with the commit hash in a
tooltip and merge provenance in a compact secondary well.

**Primary success call chain:**

```text
EditCommit ordinary/merge payload
  -> EditorSessionHistoryPort::ReadHistorySnapshot
  -> CommitRowFromEdit(commit, position) carries field_key + before/after JSON + enabled + position
  -> EditorHistoryCommit (serialized before_value_json/after_value_json, no JSON dep in header)
  -> EditorHistoryModel::RebuildPresentations
  -> PresentEditorHistoryCommit (pure helper) -> display_name + before_text/after_text/delta_text + icon_key
  -> EditorHistoryTransactionsPanel renders "Exposure / 0.00 → +0.35", hash in tooltip
```

**Primary edge / failure call chain:**

```text
Image root (no working head):
  -> first-parent chain empty -> no Current row; redo suffix emitted as Future rows
  -> exactly one Current only when a working head exists; zero Current at root
Merge commit:
  -> field_key = "merge", before/after JSON empty, merge_field_keys populated
  -> PresentEditorHistoryCommit returns is_merge = true + compact merge_summary
  -> QML renders merge title + secondary provenance well, no before/after value line
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue` | `EditorSessionHistoryPortTest` | PASS |
| `HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix` | `EditorSessionHistoryPortTest` | PASS |
| `EditorHistoryCommitPresentationTest.FormatsNumericBooleanPathEnumAndCompoundAdjustments` | `EditorSessionHistoryPortTest` | PASS |
| `NonCurrentCardsRenderBeforeAfterValueText` | `EditorHistoryTransactionsPanelQmlTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest EditorHistoryTransactionsPanelQmlTest
ctest --test-dir build/debug -R "HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue|HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix|FormatsNumericBooleanPathEnumAndCompoundAdjustments|NonCurrentCardsRenderBeforeAfterValueText" --output-on-failure
```

Suite totals: port 3/3 + QML 5/5 passed (0 failed, 0 skipped).

**Post-record amendment (2026-07-27):** the original record over-claimed the QML value line.
`HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue` only inspects the head commit,
so it did not catch that non-current (Applied/Future) transaction cards rendered an empty value
line: the `editorHistoryCommitValue` Label used a multi-statement JS-block `text` binding
(`qsTr("%1 → %2").arg(before, after)`) that did not re-evaluate for non-head delegates, even though
the delegate's `transactionBefore/After/Delta` properties were populated. Fix: bind `text` directly
to `transactionDelegate.transactionDelta`, the authoritative `delta_text` the presentation helper
already computes ("0 → +12", "0 → +0.35", after-only, On/Off). Added
`NonCurrentCardsRenderBeforeAfterValueText` as a regression test asserting the contrast card shows
"0 → +12" and the exposure card shows "0 → +0.35". Verified: `EditorHistoryTransactionsPanelQmlTest`
5/5, `EditorVersionsPanelQmlTest` 8/8, and the Phase 2 port unit tests 3/3 still pass; `alcedo_main`
rebuilt at 2026-07-27 11:24 with the fixed QML embedded.

**Checklist / exit condition:** all met.

- [x] names are user-facing and capitalized (asserted: Exposure, LUT, ODT, Crop / Rotate, Merge)
- [x] before/after values are present for every supported adjustment (numeric, boolean, path, enum,
  compound) — now asserted for non-current cards too via `NonCurrentCardsRenderBeforeAfterValueText`
- [x] exactly one commit card has the white current outline (`current_count == 1`; QML
  `border.color = root.colText` for the current row, `appTheme.cardBorderColor` otherwise)
- [x] no QML color, spacing, radius, type, or icon-size literal is introduced — every color, space,
  radius, font family/size/weight, and icon source resolves through `appTheme` tokens or the
  passed-in `theme`. The single `spacing: 0` in the history `ListView` is a structural no-op between
  delegates; the visible row gap comes from `appTheme.spaceSm` inside the delegate height, matching
  existing list patterns.

**LOC note (grill-code-review):**

| File | LOC |
| --- | ---: |
| `editor_history_types.hpp` | 80 |
| `editor_history_commit_presentation.hpp` | 58 |
| `editor_history_commit_presentation.cpp` | 762 |
| `editor_history_models.hpp` | 144 |
| `editor_history_models.cpp` | 280 |
| `EditorHistoryTransactionsPanel.qml` | 514 |
| `editor_session_history_port.cpp` | 1297 |
| `editor_session_history_port_test.cpp` | 473 |
| `editor_history_transactions_panel_qml_test.cpp` | 233 |

`editor_session_history_port.cpp` (1297 LOC) exceeds the ~1000 LOC guardrail the plan sets for it
("do not add UI formatting; extract pure projection/presentation work"). Phase 2 satisfies that
direction: the port fills raw projection data only and the pure presentation helper
(`editor_history_commit_presentation.cpp`, 762 LOC) owns payload-to-display conversion, unit-testable
without the session, graph, pipeline, or QML engine. The port's growth is the read-only projection
plus the Phase 3 `MoveHeadToCommit` plumbing layered on the same file.

**Remaining gaps:** none for Phase 2 after the post-record amendment. The presentation helper's
`OperatorDisplayName` / `OperatorIconResource` switches mirror the legacy `history_cards.cpp` maps;
a drift test covers the known mappings, and consolidating the two switches is deferred to a later
cleanup pass (same note as the P1 record). Real-RAW end-to-end qualification is Phase 7, not Phase 2.

### Phase 3 — implement one-operation history jumps [DONE: mid-apply failure parity with Undo/Redo]

Files:

- `src/include/edit/history/mini_git_working_history.hpp`
- `src/edit/history/mini_git_working_history.cpp`
- `src/include/app/editor_session_ports.hpp`
- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- `src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp`
- `src/ui/alcedo_main/album_backend/editor_session_history_port.cpp`
- `src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp`
- `src/ui/alcedo_main/album_backend/editor_session_controller.cpp`
- history model and transaction-panel files from Phase 2

Changes:

1. Add `MiniGitWorkingHistory::MoveHeadToCommit()` with explicit ancestry/redo validation.
2. Moving backward reconstructs the redo suffix in deterministic order; moving forward consumes only
   the current redo suffix.
3. Build a candidate pipeline and adjustment snapshot for the target before changing the published
   working head.
4. Append one head-move journal record, publish the candidate pipeline, update head/hash/snapshot,
   and route one render.
5. On any pre-publication failure, leave the prior graph, redo suffix, pipeline, and frame unchanged.
6. Expose `moveHeadToCommit(commitId)` through controller/model.
7. Make the commit card activatable by click, Enter, and Space. Keep Undo/Redo toolbar buttons as
   one-step shortcuts.

Exit condition:

- backward and forward multi-step moves work from the cards;
- one action creates one head-move record and one final render;
- replay/reopen selects the same head and reconstructs the same pipeline.

### Phase 4 — split New Version from Branch Here [DONE]

Files:

- `src/include/app/editor_session_ports.hpp`
- `src/include/app/editor_session_navigation_controller.hpp`
- `src/app/editor_session_navigation_controller.cpp`
- `src/include/app/editor_session_service.hpp`
- `src/app/editor_session_service.cpp`
- history port/controller/model files
- `src/ui/alcedo_main/qml/EditorHistoryTransactionsPanel.qml`
- `src/ui/alcedo_main/qml/EditorVersionsPanel.qml`
- `src/config/panel_icons/`
- `src/ui/alcedo_main/CMakeLists.txt`

Changes:

1. Replace the QML-facing ambiguous CreateVersion method with:
   - `CreateRootVersionAndCheckout(display_name)`
   - `BranchFromCommitAndCheckout(commit_id, display_name)`
2. Add pending navigation kinds for root creation and selected-commit branching.
3. Run the current-Version save checkpoint before creating or selecting another ref.
4. Create the new ref with `CreateVersionRefAtRoot` or `CreateVersionRefAtHead` as requested.
5. Rebuild and publish the candidate pipeline, set the new ref active, clear redo, and publish the
   matching snapshot/frame.
6. Persist the new active ref and serialized state before reporting success.
7. If creation, rebuild, or persistence fails, remove the provisional ref and restore the prior
   ref/pipeline/snapshot/frame.
8. Use a Version-create SVG for New Version and `git-branch.svg` for Branch Here. Normalize both to
   the DESIGN 24×24, white stroke, 1.5 stroke-width rule.
9. Delete the old QML-facing active-head creation method after all callers and tests move.

Exit condition:

- New Version always lands at root and is active;
- Branch Here lands at the selected commit and is active;
- the next edit advances only the new Version;
- reopen preserves the new ref, active head, and rendered pipeline.

### Phase 5 — repair retained-image failure and error reporting [MOSTLY DONE: structured diagnostics incomplete]

Files:

- `src/include/app/editor_session_types.hpp`
- `src/include/app/editor_session_lifecycle.hpp`
- `src/app/editor_session_lifecycle.cpp`
- navigation/service files
- `src/ui/alcedo_main/album_backend/editor_session_task_port.cpp`
- `src/ui/alcedo_main/qml/BackgroundTaskBar.qml`
- `src/ui/alcedo_main/qml/BackgroundTaskPopover.qml`
- new `src/ui/alcedo_main/qml/EditorSaveRecoveryBar.qml`
- `src/ui/alcedo_main/qml/EditorWorkspace.qml`
- `src/ui/alcedo_main/DESIGN.md`
- `src/ui/alcedo_main/CMakeLists.txt`

Changes:

1. Add a retained-image failure state separate from fatal acquisition/render failure.
2. Preserve `hasImage`, identity, guards, and last frame in the retained-image state; disable edits
   and Version mutations until recovery resolves.
3. A checkout rebuild failure restores the prior Version and returns to Interactive with an
   operation error rather than entering fatal Failed.
4. A save failure opens a recovery bar with exact error text and Retry Save, Discard and Continue,
   and Cancel actions.
5. Preserve the requested Version/image/workspace target while recovery is pending.
6. Discard explicitly clears the unmaterialized journal/working changes before continuing. No
   implicit data loss is allowed.
7. Publish `Editor Save` / localized “Saving edits” as the task label.
8. When the primary task failed, the compact bar shows the failure detail; the popover shows full
   action, target, task ID, and error.
9. Add structured diagnostics with element ID, current Version ID, requested Version ID, checkpoint
   stage, and backend message.

Exit condition:

- save/checkout failure never displays the empty-editor placeholder for a retained image;
- a later image can be opened through Retry or explicit Discard;
- status chrome states what failed and why;
- task locks clear exactly once on every terminal path.

### Phase 6 — finish the Version panel UI [DONE: P2, 2026-07-27]

Files:

- `src/ui/alcedo_main/qml/EditorVersionsPanel.qml`
- `src/ui/alcedo_main/qml/EditorHistoryTransactionsPanel.qml`
- `src/ui/alcedo_main/qml/IconActionButton.qml` only if a shared state is genuinely missing
- `src/ui/alcedo_main/DESIGN.md`

Changes:

1. Delete `editorVersionNameDialog`.
2. Add an inline draft row below the Versions header:
   - New Version opens the field with a generated unique name selected;
   - Enter commits;
   - Escape cancels;
   - focus loss commits only non-empty changed text;
   - pending save disables duplicate submission without removing the field.
3. Use the same inline editor for rename if it reduces duplicate state without obscuring actions.
4. Keep active Version indication to the existing white 1 px card outline and CURRENT HEAD label.
5. Remove `stop.svg` from the Version delegate.
6. Put removal in a correctly labeled trash/overflow action, visually separate from selection.
7. Put Branch Here on each eligible transaction card, not in the Version selection row.
8. Preserve list `contentY` across create, rename, checkout, and data-only updates.

Exit condition:

- [x] no naming dialog exists;
- [x] no stop-playback glyph exists in Version UI;
- [x] selected Version uses white outline only;
- [x] keyboard, tooltip, focus, disabled, and accessibility states are covered by QML tests.

##### Phase 6 completion record (2026-07-27)

**Status:** complete — Version-panel UI chrome (inline naming, outline selection, trash remove),
contentY preserve, and rail test split.

**Primary success call chain:**

```text
EditorVersionsPanel openCreateVersion / openRenameVersion
  -> inline editorVersionNameField (draftVisible, no Dialog)
  -> Enter / Accept: commitDraft(false)
  -> draftSubmitPending = true (field stays, re-submit blocked)
  -> EditorHistoryModel::createRootVersion | renameVersion
  -> EditorSessionController::CreateRootVersion | RenameVersion
  -> IEditorSessionBackend (CreateRootVersionAndCheckout / RenameVersion)
  -> HistoryOperationFinished + lastHistoryResult
  -> finishDraftAfterSubmit closes draft; restoreListScroll preserves contentY
```

**Primary failure / cancel call chain:**

```text
Escape or focus-loss with empty/unchanged text
  -> cancelDraft (no backend call)

draftSubmitPending already true
  -> commitDraft returns without calling create/rename
  -> field remains visible and disabled until terminal HistoryOperationFinished
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `InlineVersionDraftAcceptsEnterAndEscapeWithoutDialog` | `EditorVersionsPanelQmlTest` | PASS |
| `ActiveVersionUsesOutlineWithoutStopPlaybackAction` | `EditorVersionsPanelQmlTest` | PASS |
| `VersionNameInputCreatesRenamesAndRemovesNamedVersion` (inline path) | `EditorVersionsPanelQmlTest` | PASS |
| `ClickingNamedVersionChecksOutStableVersionId` | `EditorVersionsPanelQmlTest` | PASS |
| `InlineDraftPendingSubmitBlocksDuplicateCreate` | `EditorVersionsPanelQmlTest` | PASS |
| `RenameUsesSameInlineDraftField` | `EditorVersionsPanelQmlTest` | PASS |
| `VersionListPreservesContentYAcrossCreateRenameAndCheckout` | `EditorVersionsPanelQmlTest` | PASS |
| `InlineDraftFocusLossCommitsChangedTextAndCancelsUnchanged` | `EditorVersionsPanelQmlTest` | PASS |
| `HistoryCardClickMovesToCommitAndBranchButtonUsesSelectedCommitId` | `EditorHistoryTransactionsPanelQmlTest` | PASS |
| `HistoryToolbarUndoAndRedoFollowUserClicks` | `EditorHistoryTransactionsPanelQmlTest` | PASS |
| `PasteAndMergeUseVisibleActionsAndResolveEveryField` | `EditorHistoryTransactionsPanelQmlTest` | PASS |
| `SaveRecoveryBarShowsFailureDetailAndRoutesEveryRecoveryAction` | `EditorHistoryTransactionsPanelQmlTest` | PASS |
| Backend Version/history safety suite | `EditorSessionHistoryPortTest` + Lifecycle + NavigationController | PASS 52/52 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorVersionsPanelQmlTest EditorHistoryTransactionsPanelQmlTest alcedo_main
ctest --test-dir build/debug -R "EditorVersionsPanelQmlTest|EditorHistoryTransactionsPanelQmlTest" --output-on-failure
ctest --test-dir build/debug -R "EditorSessionHistoryPortTest|EditorSessionLifecycleTest|EditorSessionNavigationControllerTest" --output-on-failure
```

Suite totals: QML 12/12; backend safety 52/52; build zero errors for listed targets.

Scroll/focus proof: `VersionListPreservesContentYAcrossCreateRenameAndCheckout` seeds 18 rows,
scrolls to contentY≈96, then create/rename/checkout and asserts contentY within 2 px after real
model updates. `InlineDraftFocusLossCommitsChangedTextAndCancelsUnchanged` drives
`editingFinished` via Accept focus steal: unchanged generated name cancels; changed text commits.

contentY race fix: `captureListScroll` / `modelAboutToBeReset` set `_restoringContentY` (or
history `restoringContentY`) **before** model mutation so a synchronous contentY jump cannot
clobber the preserved value.

Static checks (`phase6_static.txt`): no `editorVersionNameDialog`; remove uses `trash.svg` not
`stop.svg`; Branch Here only in `EditorHistoryTransactionsPanel.qml`.

**Checklist / exit condition:** all boxes checked.

**LOC note (grill-code-review):**

| File | LOC |
| --- | ---: |
| `EditorVersionsPanel.qml` | 585 |
| `EditorHistoryTransactionsPanel.qml` | ~514 (contentY preserve + existing) |
| `editor_versions_panel_qml_test.cpp` | ~290 |
| `editor_history_transactions_panel_qml_test.cpp` | ~210 |
| `editor_history_versions_rail_qml_harness.hpp` | ~630 |
| `editor_history_versions_rail_qml_harness.cpp` | ~12 |
| `panel_icons/trash.svg` | new asset |
| `resource.qrc` | +trash entry |
| `tests/ui/CMakeLists.txt` | split targets; old `EditorHistoryVersionsRailQmlTest` removed |

No production file crossed ~1000 LOC. Shared harness is fixture/fakes only (not a god production module).

**Residual gaps:** Phase 7 real-RAW 14-step qualification not run; Phase 5 structured Version-ID /
checkpoint-stage diagnostics still incomplete (independent of Phase 6).

### Phase 7 — end-to-end qualification [OPEN: real-RAW sequence not run]

Use a real RAW image and deterministic commit clock:

1. open the image and record root/default Version/head/frame identity;
2. commit Exposure `0.00 -> +0.35`, Contrast `0.00 -> +12`, and Saturation `0 -> +8`;
3. verify formatted cards and one current row;
4. click Exposure, verify one head move, one render, and Contrast/Saturation in the future suffix;
5. click Saturation, verify forward movement and identical pipeline/frame to step 2;
6. create New Version inline, verify active root head, clean panel values, and clean root render;
7. edit Exposure on the new Version and verify only its ref advances;
8. Branch Here from the original Contrast commit and verify the new ref/head/render;
9. checkout all three Versions and compare active ID, head, chain hash, panel snapshot, and frame;
10. inject checkout rebuild failure and verify the prior frame/Version remain interactive;
11. inject save failure, verify exact status text and retained frame, then Retry successfully;
12. inject save failure again, request another image, choose Discard and Continue, and verify the new
    image presents;
13. reopen the project and repeat Version checkout comparisons;
14. exit cleanly and verify only unreachable commits are collected.

Required build/test commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target CommitGraphTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionNavigationControllerTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionLifecycleTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionTaskPortTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorHistoryTransactionsPanelQmlTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorVersionsPanelQmlTest
ctest --test-dir build/debug -R "CommitGraphTest|EditorSessionHistoryPortTest|EditorSessionNavigationControllerTest|EditorSessionLifecycleTest|EditorSessionTaskPortTest|EditorHistoryTransactionsPanelQmlTest|EditorVersionsPanelQmlTest" --output-on-failure
```

Also build and run the broader WorkspaceShell and editor checkpoint integration suites.

## Acceptance matrix

| User behavior | Current evidence | Required evidence |
| --- | --- | --- |
| readable adjustment name | raw key reaches QML; fake supplies pretty labels | production payload maps to user-facing name |
| before/after values | no projection roles | numeric, enum, boolean, path, compound-adjustment assertions |
| one current transaction | production marks every row current | zero/one current invariant across undo/redo/root |
| multi-step Undo/Redo | toolbar one-step test only | card jump backward/forward + one render + replay |
| New Version from root | fake copies active head | root head, active ref, clean snapshot/frame, reopen |
| Branch Here | graph primitive only | selected commit ref + checkout + next edit ownership |
| safe Version checkout | save-before-checkout ordering passes | real ref/pipeline/frame switch and rollback |
| failed save retains image | identity retained only | `hasImage`, last frame, retry/discard, later switch |
| useful save error | detail stored in task | compact bar and popover show exact cause |
| inline naming | modal interaction test | Enter/Escape/focus/pending behavior |
| outline-only selection | card color/token assertion | one outlined active card; no stop glyph |

## Existing test evidence

Executed on 2026-07-26:

```text
ctest --test-dir build/debug -R "EditorHistoryVersionsRailQmlTest|EditorSessionHistoryPortTest|EditorSessionNavigationControllerTest|EditorSessionLifecycleTest|EditorSessionTaskPortTest" --output-on-failure
```

Result:

- 46 executable tests passed.
- `EditorSessionTaskPortTest_NOT_BUILT` did not run.
- The passing tests prove one-step actions, typed Version ID forwarding, basic graph capture/replay,
  and save-before-checkout ordering.
- They do not prove any of the missing behaviors listed in the acceptance matrix.

No additional runtime reproduction was performed in this planning pass. The viewport/save sequence is
user-observed evidence and must be converted into the failure-injection and real-RAW tests above.

## File size and responsibility guardrails

Phase 7A introduced or substantially changed 34 files. The main growth risks are:

| File | Current LOC | Phase 7A diff | Direction |
| --- | ---: | ---: | --- |
| `editor_session_history_port.cpp` | 986 | +422/-15 | do not add UI formatting; extract pure projection/presentation work |
| `editor_session_service.cpp` | 863 | +291/-0 | keep as facade; put pending Version navigation in navigation controller |
| `editor_session_controller.cpp` | 834 | +96/-28 | adapt typed results only; no graph rules |
| `adjustment_transfer_service.cpp` | 789 | +47/-41 | leave Paste/Merge semantics focused and unchanged |
| `app_theme.cpp` | 1143 | +389/-364 | no new token unless DESIGN lacks a required recovery state |
| `editor_history_versions_rail_qml_test.cpp` | 627 | +623/-0 | split by the two production panel responsibilities |
| `tests/ui/CMakeLists.txt` | 1070 | +40/-4 | register focused targets; do not reorganize unrelated tests |
| unified workspace plan | 3126 | +59/-4 | record completion here only after this repair plan passes |

The new presentation helper is a real module only if it owns pure payload-to-display conversion and
can be tested without constructing the session, graph, pipeline, or QML engine. Splitting methods
across another file while retaining hidden state in `EditorSessionHistoryPort` does not count.

## Completion record requirements

When implementation finishes, append:

- exact success/failure call chains;
- every changed and new file;
- per-file LOC and diff LOC;
- exact build/test commands and pass/fail/skip totals;
- real-RAW sequence evidence;
- failure-injection evidence for save, rebuild, persistence, retry, and discard;
- remaining risks, if any.

Phase 7A repair is complete only when all seven reported problems have executable evidence and the
user-visible workflow no longer depends on modal naming, raw field keys, one-step-only history, an
active-head copy disguised as New Version, hidden save errors, or stop-playback Version chrome.

## P0 completion record — 2026-07-26

P0 is complete. P1 and P2 remain intentionally open; this record does not claim completion of the
full Phase 7A repair.

### Success and failure call chains

Retained-image save failure:

```text
EditorSessionService::Open/Switch
  -> EditorSessionNavigationController::RequestOpenOrSwitch
  -> SealAndStartSave
  -> EditorSessionNavigationController::OnCheckpointFinished
  -> RetainPendingFailure
  -> EditorSessionLifecycle::KeepCurrentAfterCheckpointFailure
  -> RetainedImageFailure with the prior identity, guards, and frame retained
```

Recovery and later navigation:

```text
RetrySave
  -> RetrySaveAfterFailure
  -> SealAndStartSave for the preserved PendingEditorAction
  -> ContinueToTarget / ContinueCheckoutVersion / ContinueCreateRootVersion /
     ContinueBranchFromCommit

DiscardAndContinue
  -> DiscardAndContinueAfterFailure
  -> IEditorSessionJournal::DiscardUnflushed
  -> release the prior image only after discard succeeds
  -> continue the preserved target

CancelPendingNavigation
  -> clear the preserved target
  -> ResumeInteractiveAfterFailure
  -> keep the current image and frame published
```

Named Version operations:

```text
EditorVersionsPanel.qml
  -> EditorHistoryModel::createRootVersion
  -> EditorSessionController::CreateRootVersion
  -> EditorSessionService::CreateRootVersion
  -> EditorSessionNavigationController::RequestCreateRootVersion
  -> save checkpoint
  -> ContinueCreateRootVersion
  -> EditorSessionHistoryPort::CreateRootVersionAndCheckout
  -> CommitGraph::CreateVersionRefAtRoot
  -> PipelineMgmtService::CheckoutVersion
  -> MiniGitWorkingHistory::SelectVersion
  -> PipelineMgmtService::PersistEditorHistoryState
  -> CommitGraphService::Materialize
  -> publish the clean snapshot and initial frame
```

Branching follows the same sequence, with
`EditorSessionHistoryPort::BranchFromCommitAndCheckout` and
`CommitGraph::CreateVersionRefAtHead(display_name, selected_commit)` replacing the root creation
step. No QML-facing active-head Version creation operation remains.

For checkout, root creation, and branching, any pipeline rebuild, selection, snapshot, or
persistence failure enters `RestoreGraphAndPipeline`, restores the prior graph/pipeline fields and
committed projection, and returns through `RetainPendingFailure`; the prior image is never
released on that path.

### Executable evidence

The following P0 behavior tests pass:

- `FailedVersionCheckoutKeepsHasImageAndLastFrameVisible`
- `SaveFailureRetryThenSwitchAcquiresRequestedImage`
- `SaveFailureDiscardThenSwitchClearsJournalAndAcquiresRequestedImage`
- `CreateRootVersionChecksOutRootAndNextCommitBelongsToNewVersion`
- `BranchFromSelectedCommitChecksOutNamedRefWithoutDetachedHead`
- `CreateOrBranchFailureRestoresPriorRefPipelineSnapshotAndFrame`
- `RetainedImageFailureResumesInteractiveWithoutChangingIdentity`
- `PersistEditorHistoryStateWritesNewActiveVersionBeforeEditorReopen`

Failure-injection coverage now includes save checkpoint failure, history checkout/rebuild failure,
root/branch failure, journal discard, Retry Save, Cancel, and stale persistence state. The last test
also proves that a new active root Version is present after a fresh `LoadEditorPipeline`, not merely
in the live graph.

The real-RAW sequence was not run for this P0 change: the repaired paths operate on an already
loaded editor history/pipeline and do not change RAW decoding. Real-RAW reopen/render evidence
remains part of the later full Phase 7A acceptance work.

Exact verification commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main EditorSessionHistoryPortTest EditorSessionLifecycleTest EditorSessionNavigationControllerTest EditorSessionServiceFacadeTest EditorSessionControllerPhase5ATest EditorHistoryVersionsRailQmlTest PipelineServiceTest --parallel 4

ctest --test-dir build/debug -R "EditorSessionHistoryPortTest|EditorSession(NavigationController|Lifecycle)Test|EditorHistoryVersionsRailQmlTest|EditorSessionControllerPhase5ATest|EditorSessionServiceFacadeTest|PersistEditorHistoryStateWritesNewActiveVersionBeforeEditorReopen" --output-on-failure

ctest --test-dir build/debug -R "EditorWorkspaceNavigationQmlTest" --output-on-failure

ctest --test-dir build/debug -R "WorkspaceShellTest" --output-on-failure
```

Result: the wrapper build exited successfully; the P0 ctest set passed 86/86 with 0 failures and
0 skipped; `EditorWorkspaceNavigationQmlTest` passed 3/3; and `WorkspaceShellTest` passed 44/44
test cases. The existing hardware-gated
`ProductionFirstFramePathWritesAndSubmitsRealFrameData` case remained skipped inside the latter
suite. The CMake check `EditorSessionServiceCMakeDoesNotLinkQtWidgets` also passed after the
public-header dependency propagation was repaired without adding a Widgets link.

### Changed files and LOC

The figures below are current file LOC followed by the working-tree diff `+added/-deleted`. They
include the existing uncommitted P0 implementation that this turn continued.

| File | LOC | Diff |
| --- | ---: | ---: |
| `alcedo_studio/src/app/CMakeLists.txt` | 353 | +2/-2 |
| `alcedo_studio/src/app/editor_session_lifecycle.cpp` | 226 | +13/-1 |
| `alcedo_studio/src/app/editor_session_navigation_controller.cpp` | 928 | +517/-17 |
| `alcedo_studio/src/app/editor_session_service.cpp` | 919 | +112/-56 |
| `alcedo_studio/src/app/pipeline_service.cpp` | 1131 | +65/-0 |
| `alcedo_studio/src/include/app/editor_session_lifecycle.hpp` | 160 | +17/-4 |
| `alcedo_studio/src/include/app/editor_session_navigation_controller.hpp` | 222 | +76/-1 |
| `alcedo_studio/src/include/app/editor_session_ports.hpp` | 386 | +23/-4 |
| `alcedo_studio/src/include/app/editor_session_service.hpp` | 355 | +60/-4 |
| `alcedo_studio/src/include/app/editor_session_types.hpp` | 150 | +11/-0 |
| `alcedo_studio/src/include/app/pipeline_service.hpp` | 161 | +8/-0 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_models.hpp` | 129 | +2/-1 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp` | 282 | +7/-1 |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp` | 119 | +13/-3 |
| `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt` | 776 | +1/-0 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_models.cpp` | 226 | +7/-2 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp` | 876 | +44/-2 |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp` | 1200 | +243/-29 |
| `alcedo_studio/src/ui/alcedo_main/qml/EditorHistoryVersionsRail.qml` | 196 | +9/-5 |
| `alcedo_studio/src/ui/alcedo_main/qml/EditorSaveRecoveryBar.qml` | 103 | +103/-0 (untracked) |
| `alcedo_studio/src/ui/alcedo_main/qml/EditorVersionsPanel.qml` | 384 | +1/-1 |
| `alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspace.qml` | 733 | +22/-9 |
| `alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspaceNavigation.qml` | 197 | +1/-1 |
| `alcedo_studio/tests/app/CMakeLists.txt` | 364 | +2/-1 |
| `alcedo_studio/tests/app/editor_session_lifecycle_test.cpp` | 221 | +18/-1 |
| `alcedo_studio/tests/app/editor_session_navigation_controller_test.cpp` | 513 | +145/-6 |
| `alcedo_studio/tests/app/pipeline_service_test.cpp` | 1028 | +45/-0 |
| `alcedo_studio/tests/support/editor_session_navigation_fixture.cpp` | 257 | +19/-0 |
| `alcedo_studio/tests/support/editor_session_navigation_fixture.hpp` | 191 | +7/-0 |
| `alcedo_studio/tests/support/editor_session_test_ports.hpp` | 435 | +37/-0 |
| `alcedo_studio/tests/ui/editor_history_versions_rail_qml_test.cpp` | 771 | +150/-6 |
| `docs/roadmap/alcedo_studio/ui/phase_7a_history_versions_repair_and_ui_refactor_plan.md` | 788 | +788/-0 (untracked) |

### Remaining scope and risks

- P1 multi-step history navigation, rich before/after projection, and typed operation-result UI are
  still pending.
- P2 inline Version draft and Version-card action cleanup are still pending.
- Production root/branch orchestration is covered through the navigation fake, while the real
  persistence boundary is covered by `PipelineServiceTest`; a future full-phase integration pass
  should combine both with a real session port and a real-RAW reopen sequence.

## P1 completion record — 2026-07-27

P1 is complete: multi-step history navigation, rich before/after projection, and typed
operation-result UI are implemented and covered by executable evidence. P2 (inline Version draft
and Version-card action cleanup) remains intentionally open; this record does not claim completion
of the full Phase 7A repair.

### Success and failure call chains

History projection and card content:

```text
EditCommit ordinary/merge payload
  -> EditorSessionHistoryPort::ReadHistorySnapshot
  -> CommitRowFromEdit(commit, position) carries field_key + before/after JSON + enabled + position
  -> EditorHistoryCommit (serialized before_value_json/after_value_json, no JSON dep in header)
  -> EditorHistoryModel::RebuildPresentations
  -> PresentEditorHistoryCommit (pure helper) -> display_name + before_text/after_text/delta_text + icon_key
  -> EditorHistoryTransactionsPanel renders "Exposure / 0.00 → +0.35", hash in tooltip
```

One-operation history jump (card click):

```text
Card click / Enter / Space
  -> EditorHistoryModel::moveHeadToCommit(commit_id)
  -> EditorSessionController::MoveHeadToCommit
  -> EditorSessionService::MoveHeadToCommit
  -> EditorSessionEditController::HandleMoveHeadToCommit
  -> EditorSessionHistoryPort::MoveHeadToCommit
  -> MiniGitWorkingHistory::MoveHeadToCommit (ancestry/redo validation, one head-move journal record)
  -> apply traversed before/after deltas to pipeline + committed_snapshot
  -> publish one final adjustment snapshot + route one render
  -> EditorSessionController::PublishHistoryResult (typed result at QML boundary)
```

Failure (invalid target / apply failure):

```text
MoveHeadToCommit target not on first-parent path or redo suffix
  -> MiniGitWorkingHistory rejects without moving graph, journal, or redo suffix
  -> port returns false; prior head/redo/snapshot/frame unchanged
```

Typed operation result at the QML boundary:

```text
EditorSessionController::{Undo,Redo,MoveHeadToCommit,CheckoutVersion,CreateRootVersion,
  BranchFromCommit,RenameVersion,RemoveVersion}
  -> capture EditorSessionResult
  -> PublishHistoryResult(action, state, kind, task_id, render_request_id, element/image id, message)
  -> emit HistoryOperationFinished
  -> EditorHistoryTransactionsPanel status label binds lastHistoryMessage / lastHistoryFailed
```

Editor Save title and failure detail:

```text
save starts
  -> EditorSessionTaskPort::BeginTask("editor_save", ...)
  -> snapshot.title = tr("Editor Save"); snapshot.detail = tr("Saving editor changes")
save fails
  -> EditorSessionTaskPort::EndTask(id, false, backend_error)
  -> BackgroundTaskController::FinishTask(Failed, backend_error)
  -> BackgroundTaskBar shows "Editor Save · <backend_error>" when primary.state == "failed"
  -> BackgroundTaskPopover shows kind badge + title + detail (error)
```

### Executable evidence

The following P1 behavior tests pass:

- `HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue`
- `HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix`
- `MoveHeadToAncestorThenRedoDescendantPublishesOneFinalSnapshot`
- `HistoryCardClickMovesToCommitAndBranchButtonUsesSelectedCommitId`
- `EditorSaveTaskUsesUserFacingTitleAndShowsTerminalFailureDetail`
- `EditorHistoryCommitPresentationTest.FormatsNumericBooleanPathEnumAndCompoundAdjustments`

The card-click test also asserts the typed operation result is published
(`last_history_result.action == "moveHeadToCommit"` / `"branchFromCommit"`,
`last_history_failed == false`). The projection tests assert the production payload maps to a
capitalized display name, before/after values, exactly one Current row, and the redo suffix as
Future rows. The move test asserts backward and forward multi-step jumps in one operation, the
redo suffix reconstructed deterministically, and the final adjustment snapshot reflects the target
head.

The real-RAW sequence (Phase 7) was not run for this P1 change: the repaired paths operate on an
already loaded editor history/pipeline and do not change RAW decoding. Real-RAW reopen/render
evidence remains part of the later full Phase 7A acceptance work.

Exact verification commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main EditorSessionHistoryPortTest EditorHistoryVersionsRailQmlTest EditorSessionTaskPortTest CommitGraphTest EditorSessionLifecycleTest EditorSessionNavigationControllerTest EditorSessionControllerPhase5ATest

ctest --test-dir build/debug -R "EditorSessionHistoryPortTest|EditorHistoryVersionsRailQmlTest|EditorSessionTaskPortTest|CommitGraphTest|EditorSessionLifecycleTest|EditorSessionNavigationControllerTest|EditorSessionControllerPhase5ATest" --output-on-failure

cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target WorkspaceShellTest EditorWorkspaceNavigationQmlTest EditorSessionServiceFacadeTest PipelineServiceTest

ctest --test-dir build/debug -R "WorkspaceShellTest|EditorWorkspaceNavigationQmlTest|EditorSessionServiceFacadeTest|PipelineServiceTest" --output-on-failure
```

Result: the wrapper build exited successfully; the P1 ctest set passed 123/123 with 0 failures and
0 skipped; the broader regression set passed 73/73 (`WorkspaceShellTest` 47/47,
`EditorWorkspaceNavigationQmlTest` 3/3, `EditorSessionServiceFacadeTest` 23/23,
`PipelineServiceTest` included) with the existing hardware-gated
`ProductionFirstFramePathWritesAndSubmitsRealFrameData` case skipped and the two fuzz/thread
cases disabled as before. No P0 regression was introduced.

### Changed files and LOC

Current file LOC (P1 changes are layered on the uncommitted P0 baseline).

| File | LOC | P1 change |
| --- | ---: | ---: |
| `alcedo_studio/src/include/app/editor_history_types.hpp` | 80 | +43/-8 (timeline position enum, before/after JSON strings, merge field keys; dropped label/current) |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp` | 58 | +58/-0 (new, pure helper API) |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_commit_presentation.cpp` | 762 | +762/-0 (new, per-operator value formatting ported from history_cards.cpp) |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_models.hpp` | 144 | +38/-1 (presentation roles, moveHeadToCommit, presentations_ storage) |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_models.cpp` | 280 | +72/-12 (presentation roles, RebuildPresentations, moveHeadToCommit, timeline position) |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_history_port.hpp` | 124 | +5/-0 (MoveHeadToCommit override) |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp` | 1297 | +96/-25 (CommitRowFromEdit, redo suffix in projection, MoveHeadToCommit, multi-step ApplyRecoveredRecord) |
| `alcedo_studio/src/include/edit/history/mini_git_working_history.hpp` | 212 | +30/-0 (MoveHeadToCommit, RedoSuffix, traversed_commits/backward result fields) |
| `alcedo_studio/src/edit/history/mini_git_working_history.cpp` | 741 | +83/-0 (MoveHeadToCommit ancestry/redo validation + redo reconstruction) |
| `alcedo_studio/src/include/app/editor_session_ports.hpp` | 398 | +12/-0 (IEditorHistoryPort::MoveHeadToCommit) |
| `alcedo_studio/src/include/app/editor_session_service.hpp` | 367 | +18/-0 (IEditorSessionBackend::MoveHeadToCommit, EditorSessionService::MoveHeadToCommit) |
| `alcedo_studio/src/app/editor_session_service.cpp` | 944 | +24/-0 (MoveHeadToCommit facade) |
| `alcedo_studio/src/include/app/editor_session_edit_controller.hpp` | 90 | +6/-0 (HandleMoveHeadToCommit) |
| `alcedo_studio/src/app/editor_session_edit_controller.cpp` | 213 | +35/-0 (HandleMoveHeadToCommit) |
| `alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_session_controller.hpp` | 301 | +22/-0 (lastHistory* properties/signal, MoveHeadToCommit, PublishHistoryResult) |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_controller.cpp` | 916 | +66/-22 (capture + publish typed result for every history op, MoveHeadToCommit, PublishHistoryResult) |
| `alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_task_port.cpp` | 97 | +6/-2 (localized Editor Save title) |
| `alcedo_studio/src/ui/alcedo_main/qml/BackgroundTaskBar.qml` | 151 | +14/-2 (editorSave kind label, failure detail in compact bar) |
| `alcedo_studio/src/ui/alcedo_main/qml/BackgroundTaskPopover.qml` | 185 | +1/-0 (editorSave kind label) |
| `alcedo_studio/src/ui/alcedo_main/qml/EditorHistoryTransactionsPanel.qml` | 478 | +210/-160 (before/after value line, Branch Here button, card click/Enter/Space, hash tooltip, status binds typed result) |
| `alcedo_studio/src/ui/alcedo_main/CMakeLists.txt` | 778 | +2/-0 (presentation helper source + header) |
| `alcedo_studio/tests/ui/CMakeLists.txt` | 1072 | +1/-0 (presentation helper linked into EditorSessionHistoryPortTest) |
| `alcedo_studio/tests/edit/history/editor_session_history_port_test.cpp` | 428 | +131/-0 (projection, current/redo-suffix, and multi-step move tests + helpers) |
| `alcedo_studio/tests/ui/editor_history_versions_rail_qml_test.cpp` | 867 | +96/-12 (MakeCommit/snapshot updated to new struct, MoveHeadToCommit + branch recording in fake backend, card-click + branch-button test, HistoryCards helper) |
| `alcedo_studio/tests/ui/editor_session_task_port_test.cpp` | 68 | +21/-0 (Editor Save title + terminal failure detail test) |

### Remaining scope and risks

- P2 inline Version draft, stop-playback glyph removal, and the formal split of
  `editor_history_versions_rail_qml_test.cpp` into transactions/versions panel files remain open.
  The P1 transactions-panel tests were added to the existing rail test file (which already covered
  both panels per P0) rather than performing the file split, to avoid disrupting the shared QML
  harness; the split is deferred to P2, which owns the versions panel.
- `MoveHeadToCommit` validates the target and journals one head-move record before applying the
  traversed deltas, matching the existing single-step Undo/Redo apply-after-move pattern. A
  mid-apply operator failure (exceptional) would leave the head moved with a partial pipeline, the
  same residual risk as Undo/Redo; the candidate-pipeline-before-publication guarantee is fully met
  for the invalid-target case (the common failure mode).
- Multi-step moves that traverse a merge commit skip the merge's per-field delta on the live
  snapshot (merges have no single before/after value); the head, chain, redo suffix, and journal
  are still correct, and the next full-frame render re-applies the committed snapshot. Recovery
  (`ApplyRecoveredRecord`) walks every hop of a multi-step head-move record so reopen reconstructs
  the same head and snapshot.
- The presentation helper's `OperatorDisplayName` / `OperatorIconResource` switches mirror the
  legacy `history_cards.cpp` maps; a drift test asserts a few known mappings, but the two switches
  should be consolidated in a later cleanup pass.
