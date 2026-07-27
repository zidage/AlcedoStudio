# Phase 7A History/Versions Repair and UI Refactor Plan

Date: 2026-07-26

Status: P0 completed on 2026-07-26; P1/P2 remain

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

## Implementation phases

### Phase 1 — add failing evidence before changing behavior

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

### Phase 2 — repair the history projection and card content

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

### Phase 3 — implement one-operation history jumps

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

### Phase 4 — split New Version from Branch Here

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

### Phase 5 — repair retained-image failure and error reporting

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

### Phase 6 — finish the Version panel UI

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

- no naming dialog exists;
- no stop-playback glyph exists in Version UI;
- selected Version uses white outline only;
- keyboard, tooltip, focus, disabled, and accessibility states are covered by QML tests.

### Phase 7 — end-to-end qualification

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
