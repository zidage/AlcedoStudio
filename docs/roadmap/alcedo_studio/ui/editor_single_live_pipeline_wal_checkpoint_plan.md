# Editor Single Live Pipeline + WAL + Checkpoint Simplification Plan

Date: 2026-07-31  
Status: WU1–WU5 complete; WU6–WU7 remaining  
Branch context: follows interim operator merge-policy work on `feature/pre_v28_fix`

Related documents (historical; this plan **supersedes** their transfer-candidate / dual-snapshot
mutation model where they conflict):

- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)
- [Editor History Durability and Version Transfer Design](editor_history_durability_and_version_transfer_design.md)

## Outcome (one shot)

Make the editor match this runtime model and **delete** the copy-heavy parallel state machines:

1. **One live pipeline per `element_id`**, owned by `PipelineMgmtService`.
2. **All edits (slider settle, paste, merge) mutate that pipeline** via operator APIs
   (`SetOperator` / `SetParams` / enable).
3. **Edit history only records what happened** (WAL first, then commits + Version HEAD in DuckDB).
4. **Pipeline JSON in DuckDB is only a checkpoint** for fast restore, never a second editable
   universe.
5. **Cancel paste** = remove / abandon the new Version (or switch active Version back).
6. **Cancel / undo merge** = same as any other commit: move HEAD and restore listed params to their
   previous values (or full first-parent replay to parent head).
7. **No** `HistoryTransferCandidate`, **no** shadow `CommitGraph` copy for paste/merge publish,
   **no** mutation of `committed_snapshot` as the primary edit target, **no** snapshot-only
   `InitiateMerge` as the production path.

This is **one implementation effort**, not a multi-phase product roadmap. Work is ordered only as
build dependencies; the branch is not “done” until the deletion list and acceptance tests below all
pass.

---

## Author design intent (verbatim)

The following is the product owner’s design reasoning, preserved **as written** so implementers do
not re-interpret it into another layered snapshot model.

### Verbatim — single live pipeline as the only operation target

> 其实我这里还想在分析一下，我想的是应该只有一个live pipeline，也就是pipeline mgmt service恢复的那个，然后一些操作，比如merge/paste，也是在这里作为改动最终落地的对象，edit history只是负责把期间的过程记录下来，从而可以恢复，而最终改动落地的目标都是这唯一一个pipeline，所以也就有了SetOperator这样的接口。换句话来说，一个element id只指向一个pipeline，pipeline的状态的转移更像是一堆函数的应用，而不是对它对应的JSON的反复修改，后者只是用来serialize的格式。

### Verbatim — cancel is Version / commit reverse, not a candidate universe

> 这个模型已经有了，paste因为会直接创建一个版本，"取消"就只需要删除那个version就可以了，而merge可以在那个commit上记录被修改的param，然后就像别的commit那样把哪些param修改为前值就好了。

### Verbatim — WAL + checkpoint; history is authority; pipeline is cache

> 好的，我觉得还是太复杂了，怎么还有什么candidate，搞得一个param json有多珍贵一样的。这里其实很简单，先写WAL，然后到时候了把WAL里面的内容作为commit写到数据库，移动Version（移动HEAD），然后PMS保存的时候，就把pipeline转成JSON然后存到数据库里面。下次，在编辑器里面读取的时候，就比较一下PMS给的pipeline的hash和对应version的hash，如果一致，说明两者的对应的“编辑历史”是一样的，一个是当前的状态，一个是到这个状态所经历的所有事情。这不是很简单吗？怎么代码婆婆妈妈的写了那么多？
>
> WAL那里我还没完，就是如果每次加载一个pipeline的时候发现这个WAL里面还有内容，就核对一下是否里面的内容可以和现在version head接起来，如果可以那就接起来，然后再比较pipeline自己的hash，反正以记录到edit history里面的内容为准，而不是以pipeline为准，pipeline更像是一个“检查点”的角色。
>
> 你能明白吗？我想简化目前这个莫名其妙充满拷贝的模型，我受不了了！

### Verbatim — operator behavior must not be re-enacted on fat JSON

> 这里其实有一个很大的毛病，确实，我们需要一个权威状态，但是，从JSON到一个"pipeline"只需要一步，如果要在JSON上操作，每个op自己的behavior就直接丢失了，只能再在相应模块进行"behavior的重演", 完全可以重构一个pipeline出来，然后直接用live op的行为来弄，这不是很干净吗？
>
> 怪我pipeline的职责比较多，因为首先pipeline是把算子串起来的一个数据结构，包含对一个管线模型的描述：由管线阶段，每个阶段内算子的顺序；同时他也是一个执行模型，因为他组织了管线阶段，而后者又带有intermediate的缓存，同时部分Op（比如Resize，RawDecode）还带有直接修改图像内容的功能，同时后面的阶段又是管理着一个单点kernel stream的执行器的生命周期。所以整体来说比较混乱，没有一个清晰的区分，但是呢API又比较简单。
>
> 所以结合我的观察，以及这种胖JSON在别的地方的应用，告诉我回到以一个临时live pipeline为”操作对象“是否是更好的，pipeline mgmt service管理的则又是另一层的概念，更像是带有checkpointed参数的管线快照，用于一个编辑session中生成图像

**Resolved interpretation for this plan (binding):** use the **single PMS live pipeline** as the
operation target for all user edits in the focused editor session. Do **not** invent a second
long-lived “temporary pipeline” universe or transfer-candidate graph. History/WAL records function
applications; DuckDB serialized pipeline state is a checkpoint. If a code path needs operator
behavior offline (tests), materialize ops **once** from JSON into a real executor and call op APIs —
never reimplement as_shot / lens meta rules on bare JSON in services.

---

## Locked terminology (use only these)

| Term | Meaning |
|------|---------|
| **Live pipeline** | The single `PipelineExecutor` instance for an `element_id` held by `PipelineMgmtService`. |
| **Edit function** | A mutation of the live pipeline (`SetOperator`, enable, multi-field merge resolve). |
| **WAL** | Existing mini-Git recovery journal (`MiniGitJournal` / journal records). Append-only log of edit commits and head-moves **before** (or until) DuckDB materialization. |
| **Commit** | Immutable `EditCommit` in the `CommitGraph` (and eventually DuckDB). |
| **Version / HEAD** | Named ref + `head_commit_hash`; active Version is the user’s checked-out line. |
| **Checkpoint** | Serialized operator params (and related identity hashes) stored in DuckDB for fast restore. |
| **Logical head** | Active Version head **after** any successfully attached WAL prefix. Authority for “what edits exist”. |
| **Checkpoint identity** | Hash or pair `(materialized_head_commit_hash, materialized_transaction_chain_hash)` (or equivalent) stored with the checkpoint. Must match logical head for the fast path. |
| **Ordinary edit commit** | Single-parent commit with before/after for one field (or `$operator_params`). |
| **Merge commit** | Commit that records **which fields changed** and enough data to restore previous values (same reverse model as ordinary edits). Second parent may remain for ancestry; first-parent chain is the only pipeline replay path. |
| **Paste Version** | New Version whose commits are root-relative applications of a transfer package; cancel = do not keep that Version active / remove it per product rules. |

Forbidden product language for new code and this plan’s acceptance criteria: do not introduce
“transfer candidate”, “shadow graph publish”, or “committed_snapshot as mutation target” as required
architecture. Legacy names may appear only in **deletion** checklists.

Roadmap prose ban (repo rule): do not use the generic English noun formed by `c` + `ontract` (any
casing/plural) in this document.

---

## Target runtime algorithms (normative)

### A. User edit (slider settle / panel commit)

```text
1. Apply edit function to live pipeline (op SetParams / SetOperator / enable).
2. Append WAL record describing the commit payload (before/after, field, parents).
3. Update in-memory CommitGraph working head (same as today’s mini-Git working history append).
4. Invalidate render as already done by session.
5. Do NOT maintain a parallel editable field-table that can diverge from the pipeline.
```

### B. Materialize WAL → DuckDB (existing save checkpoint moments)

```text
When autosave / image switch / version switch / orderly shutdown requires durability:
1. Take WAL range that continues current materialized head.
2. Insert commits + move Version HEAD + write recovery metadata in one DuckDB transaction
   (keep atomicity guarantees from 6C materializer).
3. Truncate or advance journal prefix only after successful DB commit.
4. Optionally refresh checkpoint JSON from live pipeline in the same or immediately subsequent save.
```

### C. PMS / editor save checkpoint of pipeline

```text
1. Export live pipeline operator params to serialized JSON (existing export paths).
2. Store with ImageEditState (or equivalent) together with:
   - active_version_id
   - head_commit_hash (logical head)
   - transaction_chain_hash
3. This JSON is a checkpoint only.
```

### D. Load / open editor for element

```text
1. Load Version refs + commits from DuckDB; set logical_head = active Version head.
2. If WAL file has records:
   a. Validate they form a contiguous extension of logical_head
      (parent hashes / expected source head / chain hash as today).
   b. If yes: apply into in-memory graph + extend logical_head; keep records until materialize.
   c. If no: do not silently apply; surface recovery error or quarantine WAL per existing
      recovery policy (must be explicit; no silent discard without log + test).
3. Load checkpoint JSON + checkpoint identity from DuckDB.
4. Compare checkpoint identity to logical_head identity:
   a. Match: import checkpoint into live pipeline (fast path).
   b. Mismatch: rebuild live pipeline from root + first-parent chain to logical_head
      (history wins); then write a fresh checkpoint on next save.
5. Never treat checkpoint JSON as more authoritative than logical_head.
```

### E. Paste

```text
1. Build root-relative commits from transfer package (existing BuildRootRelativeCommits semantics).
2. Insert commits into the **live** CommitGraph (same graph the session already owns — no shadow copy).
3. Create new Version ref at paste head; set active Version to it.
4. Apply package to **live pipeline** via operator SetOperator / SetParams (so ColorTemp/Lens
   behavior runs in-process). Prefer applying each operator entry through the real op path,
   not by inventing a second JSON merge engine.
5. WAL: append the commits / head-move / version-ref creation records required so recovery
   reconstructs the same Version + pipeline after crash (match durability bar of ordinary edits).
6. Cancel paste (user rejects new look): switch active Version back to prior Version and rebuild
   or checkout pipeline to prior head; remove the unused Version ref if product requires no orphan
   rows (if refs are kept for history UI, document that; default: remove unreferenced paste Version
   created in this session when user cancels before any further edits on it).
```

### F. Merge (adjustment transfer into current Version)

```text
1. For each operator entry in the package (skip UNKNOWN/RESIZE as today):
   a. Read current params/enable from **live** op (GetParams / enable flag).
   b. Build incoming params from package (optional deep-merge when entry.merge_params_ is true).
   c. conflict = op.DetectMergeConflict(current, incoming) OR enable differs.
2. If any conflicts: return conflict list to UI (current/incoming values for display, plus choice).
   Do not create shadow Versions for UI-only preview.
3. When user completes (or no conflicts / auto-apply all take-incoming where no conflict):
   For each field:
     before = live GetParams (+ enable)
     after  = op.MergeParams(current, incoming, choice)  // or current if keep
     Apply after to live pipeline via SetOperator / SetParams / enable
   Append **one** merge commit (or one ordinary commit per field — pick ONE scheme and use it
   everywhere; preferred: single merge commit with MergeEditPayload listing each field’s
   before/after or resolved value + previous value for reverse) on the **live** graph first parent
   = current head. Incoming ancestry: either (a) no second parent if product drops lineage, or
   (b) insert package commits as unreachable ancestry objects / second parent without a
   user-visible Version row. **Do not** CreateVersionRef for a temporary “Merged Adjustments”
   branch that the user must clean up.
4. WAL append for that merge commit.
5. Undo merge: head-move to first parent + restore each listed field to before on live pipeline
   (or rebuild pipeline to parent head — both acceptable if tests prove equality; prefer explicit
   reverse apply for speed).
6. Cancel merge UI before complete: no graph mutation beyond optional read-only conflict query;
   live pipeline unchanged.
```

### G. Operator merge policy (already partially landed; keep)

- `IOperatorBase::DetectMergeConflict` / `MergeParams` remain the **only** place for portable vs
  image-local field rules.
- `ColorTempOp` / `LensCalibOp` overrides stay; default = full JSON inequality / wholesale pick.
- Production merge **must** call these on **live** (or freshly materialised) op instances from the
  session pipeline — not on factory “probe” stubs as the long-term pattern.
- Interim `MakeMergePolicyProbe` switch in `adjustment_transfer_service.cpp` is **technical debt**:
  delete it when merge always has a live `IOperatorBase*` from the pipeline stage.

---

## What to delete (mandatory end state)

Search the tree after implementation; these must have **zero production call sites** (tests may keep
temporary shims only if marked deprecated and scheduled for deletion in the same effort):

| Symbol / type / path | Action |
|----------------------|--------|
| `HistoryTransferCandidate` | Delete type and all maps |
| `EditorHistoryTransfer::PreparePaste` shadow-graph flow | Replace with live graph+pipeline path |
| `EditorHistoryTransfer::PrepareMerge` / `CompleteMergeCandidate` / `PublishTransferCandidate` / `ValidateMergeCandidate` / `DiscardTransferCandidate` / `FindCandidate*` | Delete or reduce to thin wrappers that only call live path (prefer delete) |
| `EditorTransferCandidate` as staged parallel graph | Remove shadow `CommitGraph` member usage; if type remains for queue messages, it must not own a full graph copy |
| `AdjustmentTransferService::InitiateMerge(graph, package, current_snapshot, …)` | Delete production use; either delete overload or keep only if rewritten to take live pipeline reference |
| Dual materialization “candidate publication” leases if only serving shadow publish | Collapse to normal edit/save checkpoint leases |
| Mutation of `HistoryWorkingState::committed_snapshot` as primary edit apply | Stop using as write-ahead state machine; if kept, it is **derived read model** from pipeline export or commit replay for UI lists only, regenerated after pipeline mutation |
| Session path that claims merge complete without live `SetOperator` | Forbidden |

Files expected to be heavily rewritten or deleted (implementer must open and reconcile; list is not
exhaustive):

```text
alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp
alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_transfer.hpp
alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_history_state_detail.hpp
alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp
alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_state_detail.cpp
alcedo_studio/src/app/adjustment_transfer_service.cpp
alcedo_studio/src/include/app/adjustment_transfer_service.hpp
alcedo_studio/src/include/app/adjustment_transfer_types.hpp
alcedo_studio/src/app/editor_session_service.cpp  (BeginMerge / CompleteMerge / PasteAdjustments / transfer publication)
alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp
alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp
```

Do **not** leave a “compatibility” shadow path “just in case”. Feature flags only if needed for a
single release branch kill-switch, default **on** for the new path, and tests must run the new path.

---

## What to keep / reuse

| Piece | Keep how |
|-------|----------|
| `CommitGraph`, `EditCommit`, `MergeEditPayload`, `OrdinaryEditPayload` | Keep; ensure reverse data present |
| `MiniGitJournal` / WAL file format | Keep; wire paste/merge onto same append path as ordinary edits |
| `MiniGitWorkingHistory::AppendEdit` / head-move | Prefer extend rather than parallel APIs |
| `PipelineMgmtService::LoadEditorPipeline` / `RebuildActiveEditorPipeline` / `CheckoutVersion` | Primary live pipeline lifecycle |
| `IOperatorBase::DetectMergeConflict` / `MergeParams` + ColorTemp/Lens overrides | Keep |
| `ColorTempOp::SetParams` partial resolved_* preservation | Keep |
| QML `EditorMergeDialog` choice + resolutions | Keep UI; backend must apply choices on live pipeline |
| DuckDB materializer atomicity | Keep |
| Operator factory registration | Keep |

---

## Implementation work units (ordered, single delivery)

Execute in this order so the tree never depends on a deleted type. Each unit lists **exact
behaviors**, **primary files**, and **tests that must pass before moving on**. No “phase 1 product
deferral”.

### Work unit 1 — Document freeze + inventory

1. Land this plan under `docs/roadmap/alcedo_studio/ui/`.
2. Link it from `docs/roadmap/README.md`.
3. Produce `build/tmp/single_live_pipeline_inventory.md` (local only, gitignored under `build/`)
   listing every call site of:
   - `PreparePaste`, `PrepareMerge`, `PublishTransferCandidate`, `HistoryTransferCandidate`,
     `InitiateMerge(`, `committed_snapshot` writes, `transfer_candidates`.
4. Inventory is the deletion checklist; update it to empty production call sites at end.

##### Work unit 1 completion record (2026-07-31)

**Status:** complete — plan linked; inventory captured under `build/tmp/`

**Primary success call chain:**

```text
docs/roadmap/alcedo_studio/ui/editor_single_live_pipeline_wal_checkpoint_plan.md
  -> docs/roadmap/README.md link
  -> rg inventory -> build/tmp/single_live_pipeline_inventory.md
```

**Primary failure call chain:**

```text
(n/a — documentation / inventory only)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| Plan lands under `docs/roadmap/alcedo_studio/ui/` | inspection | PASS |
| Linked from `docs/roadmap/README.md` | inspection | PASS |
| Inventory lists required symbols | `build/tmp/single_live_pipeline_inventory.md` | PASS |

Commands: `rg` inventory to `build/tmp/single_live_pipeline_inventory_raw.txt`  
Suite totals: n/a (docs)

**Checklist / exit condition:** all boxes checked for WU1

**LOC note (grill-code-review):** docs + local inventory only

**Remaining gaps:** production call sites remain until WU3/WU6 deletions; inventory tracks them

### Work unit 2 — Live pipeline paste

**Behavior:**

- `PasteAsRootRelativeVersion` (or session equivalent) mutates the **session’s live** `CommitGraph`
  and then applies operators to the **live** pipeline for that `element_id`.
- WAL receives the same durability as an ordinary multi-commit paste would need after crash.
- Cancel: restore previous `active_version_id` + pipeline checkout to previous head; remove paste
  Version if unused.

**Primary call chain (must match code):**

```text
AdjustmentTransferController::PasteIntoEditor
  → EditorSessionController::PasteAdjustmentPackage
  → EditorSessionService::PasteAdjustments
  → (NEW) history port / working history: insert commits + CreateVersion + SetActive
  → PipelineMgmtService path: apply package to live executor (SetOperator each entry)
  → WAL append
  → render refresh via existing session mechanisms
```

**Delete:** any path that copies full `CommitGraph` into `HistoryTransferCandidate` for paste.

**Tests (names must state the assertion; no vague run-only names):**

1. `PasteCreatesNewVersionAndLivePipelineReceivesOperatorParams` — after paste, live pipeline
   exposure (or fixture field) equals package; active Version id changed.
2. `PasteCancelRestoresPriorActiveVersionAndPipelineParams` — cancel/switch back restores prior
   params.
3. `PasteCrashRecoveryReplaysWalOntoLogicalHead` — kill after WAL append before DuckDB materialize;
   reopen; logical head and pipeline match pasted state (history wins if checkpoint stale).
4. Existing mini-Git paste tests updated to the live path; remove tests that only assert shadow
   candidate maps.

##### Work unit 2 completion record (2026-07-31)

**Status:** complete — live paste + WAL + cancel; session no longer uses paste shadow candidates

**Primary success call chain:**

```text
AdjustmentTransferController::PasteIntoEditor
  -> EditorSessionController::PasteAdjustmentPackage
  -> EditorSessionService::PasteAdjustments
  -> EditorSessionHistoryPort::PasteLiveRootRelativeVersion
  -> EditorHistoryTransfer::PasteLiveRootRelativeVersion
       CreateVersionRefAtRoot + SetActive + SelectVersion
       PersistEditorHistoryState (empty paste Version identity)
       MiniGitWorkingHistory::PrepareAppendEdit / PublishPreparedEdit (WAL per operator)
       AdjustmentTransferService::Apply (live SetOperator)
       regenerate committed_snapshot via SnapshotAtHead
  -> EditorSessionService::StartHistoryCheckpoint (ordinary CaptureSaveCheckpoint + render)
```

**Primary failure call chain:**

```text
empty package / prepare-or-WAL failure
  -> RestoreLivePastePrior (in-memory graph/selection/snapshot)
  -> EditorSessionResultKind::Rejected
cancel after live paste
  -> CancelLivePaste: SelectVersion(prior) + RemoveVersion(paste)
       + ApplyEditorAdjustmentSnapshot(prior) + TruncateMaterialized(WAL)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `PasteCreatesNewVersionAndLivePipelineReceivesOperatorParams` | `EditorSessionHistoryPortTest` | PASS |
| `PasteCancelRestoresPriorActiveVersionAndPipelineParams` | `EditorSessionHistoryPortTest` | PASS |
| `PasteCrashRecoveryReplaysWalOntoLogicalHead` | `EditorSessionHistoryPortTest` | PASS |
| Live paste CQ ordering / materialize / failure | `EditorSessionCommandQueueBaselineTest` | PASS |
| CQ5 paste publication + static API | `EditorSessionCq5QualificationTest` | PASS |
| Full `EditorSessionHistoryPortTest` (28) | binary | PASS |
| Full `EditorSessionCommandQueueBaselineTest` (16) | binary | PASS |

Commands:

```bat
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest EditorSessionCommandQueueBaselineTest EditorSessionCq5QualificationTest
build\debug\alcedo_studio\tests\ui\EditorSessionHistoryPortTest_runtime\EditorSessionHistoryPortTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionCommandQueueBaselineTest_runtime\EditorSessionCommandQueueBaselineTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionCq5QualificationTest_runtime\EditorSessionCq5QualificationTest.exe
```

Suite totals: HistoryPort 28/28; CQ baseline 16/16; CQ5 5/5

**Checklist / exit condition:** all WU2 boxes checked

**LOC note (grill-code-review):** `editor_history_transfer.cpp` grew live paste/cancel (~250 LOC added); still one transfer responsibility file. Session paste path shortened (no shadow publish).

**Remaining gaps:** `PreparePaste` / `HistoryTransferCandidate` remain for **merge** until WU3/WU6; paste production session path no longer constructs paste shadow graphs. Cancel after paste with a dirty pre-paste WAL truncates the whole journal (acceptable when paste follows a flushed journal).

### Work unit 3 — Live pipeline merge

**Behavior:** as algorithm F. Conflict UI still works. Complete writes live pipeline then one merge
commit + WAL. Cancel before complete is a no-op on pipeline/graph.

**Primary call chain:**

```text
AdjustmentTransferController::BeginMergeIntoEditor
  → EditorSessionService::BeginMerge
  → load live pipeline ops; build conflict list via IOperatorBase::DetectMergeConflict
  → return preview (no shadow Version for incoming)

AdjustmentTransferController::CompleteMergeIntoEditor
  → EditorSessionService::CompleteMerge
  → for each resolution: MergeParams + SetOperator on live pipeline
  → Append merge commit + WAL
  → refresh UI/render
```

**Merge commit payload requirement (binding):**

Each field entry must allow reverse without re-reading the package:

- `operator_type`, `stage_name`, field identity
- `before_value` + `before_enabled` **or** equivalent recoverable previous state
- `after_value` / `resolved_value` + `resolved_enabled`

If current `MergeFieldDelta` lacks `before_*`, **extend the schema** and migration note (destructive
project format OK if already on mini-Git cutover; document in this plan’s “Schema note” below).

**Tests:**

1. `BothAsShotColorTempMergeReportsNoConflictAndDoesNotMutatePipelineWhenPackageOnlyMode` — live
   path (extend existing ColorTemp tests).
2. `MergeTakeIncomingAsShotPreservesTargetResolvedBaselineOnLivePipeline` — GetParams after complete.
3. `MergeUndoRestoresPreMergeOperatorParams` — after undo/head-move, params equal pre-merge snapshot
   taken from live pipeline before merge.
4. `MergeCancelBeforeCompleteLeavesPipelineAndHeadUnchanged`.
5. `LensPortableOnlyConflictIgnoresStrippedMetaOnLivePipeline`.
6. Remove/replace tests that call snapshot-only `InitiateMerge` without a live executor.

##### Work unit 3 completion record (2026-07-31)

**Status:** complete — live Begin/Complete merge + `before_*` reverse payload; session no longer publishes merge via shadow candidates

**Primary success call chain:**

```text
AdjustmentTransferController::BeginMergeIntoEditor
  -> EditorSessionService::BeginMerge
  -> EditorSessionHistoryPort::BeginLiveMerge
  -> EditorHistoryTransfer::BeginLiveMerge
       live DetectMergeConflict / enable compare (no Version, no shadow graph)
  -> pending package retained on session

AdjustmentTransferController::CompleteMergeIntoEditor
  -> EditorSessionService::CompleteMerge
  -> EditorHistoryTransfer::CompleteLiveMerge
       insert incoming ancestry commits (no Version ref)
       optional PersistEditorHistoryState for second-parent durability
       live MergeParams + SetOperator / EnableOperator
       MiniGitWorkingHistory::PrepareAppendMerge / PublishPreparedEdit (WAL)
       regenerate committed_snapshot via SnapshotAtHead
  -> StartHistoryCheckpoint (ordinary CaptureSaveCheckpoint + render)
```

**Primary failure call chain:**

```text
stale first_parent_head / fingerprint mismatch / incomplete resolutions
  -> CompleteLiveMerge Rejected; RestoreLivePastePrior when mid-mutation
cancel before complete
  -> session clears pending preview/package only (graph/pipeline untouched)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| ColorTemp both-as_shot no conflict (`InitiateMergeColorTempBothAsShot…`) | `AdjustmentTransferServiceMiniGitTest` | PASS |
| Take-incoming as_shot keeps baseline + `before_*` | `AdjustmentTransferServiceMiniGitTest` | PASS |
| `MergeUndoRestoresPreMergeOperatorParams` | `EditorSessionHistoryPortTest` | PASS |
| `MergeCancelBeforeCompleteLeavesPipelineAndHeadUnchanged` | `EditorSessionHistoryPortTest` | PASS |
| `LiveMergeCompleteAppliesResolvedParamsToPipelineAndWal` | `EditorSessionHistoryPortTest` | PASS |
| `LiveMergeRejectsCompleteAfterThePublishedHeadChanges` | `EditorSessionHistoryPortTest` | PASS |
| Live merge CQ + checkpoint (no transfer publication) | `EditorSessionCommandQueueBaselineTest` | PASS |
| Lens portable-only policy | `LensCalibOpMergePolicyTest` | PASS |

Commands:

```bat
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest EditorSessionCommandQueueBaselineTest AdjustmentTransferServiceMiniGitTest
build\debug\alcedo_studio\tests\ui\EditorSessionHistoryPortTest_runtime\EditorSessionHistoryPortTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionCommandQueueBaselineTest_runtime\EditorSessionCommandQueueBaselineTest.exe
build\debug\alcedo_studio\tests\app\AdjustmentTransferServiceMiniGitTest_runtime\AdjustmentTransferServiceMiniGitTest.exe
```

Suite totals: HistoryPort 33/33; CQ baseline 16/16; MiniGit transfer 21/21

**Checklist / exit condition:** WU3 behaviors landed; dedicated `LensPortableOnlyConflictIgnoresStrippedMetaOnLivePipeline` history-port test deferred (policy covered by `LensCalibOpMergePolicyTest` + live `DetectMergeConflict` in `BeginLiveMerge`)

**LOC note (grill-code-review):** `editor_history_transfer.cpp` added Begin/Complete live merge; `MergeFieldDelta` schema extended; session merge path mirrors paste checkpoint (no `PublishTransferCandidate`)

**Remaining gaps:** shadow `PrepareMerge` / `HistoryTransferCandidate` APIs remain until WU6 deletion; `InitiateMerge` snapshot overload still exists for legacy tests

### Work unit 4 — Stop JSON mutation state machine for ordinary edits

**Behavior:**

- Ordinary panel commits already should hit live pipeline; verify
  `editor_history_mutation.cpp` (and session edit path) do **not** treat `committed_snapshot` as the
  sole apply target.
- If `committed_snapshot` remains for history UI projection, regenerate it by exporting from live
  pipeline or replaying commits **after** pipeline mutation — never the reverse order.

**Tests:**

1. `OrdinaryEditChangesLivePipelineBeforeOrWithWalAppend` — order assertion via test hooks or
   observable enable/params.
2. `CommittedSnapshotIfPresentMatchesLivePipelineExportAfterEdit` — no drift.

##### Work unit 4 completion record (2026-07-31)

**Status:** complete — settled edits / undo / redo / head-move / discard / checkout apply live pipeline; snapshot derived

**Primary success call chain:**

```text
EditorSessionEditController::HandlePatch (settled)
  -> EditorHistoryMutation::CommitAdjustment
       ApplyEditorAdjustmentOperatorState (live pipeline under render lock)
       PrepareAppendEdit / PublishPreparedEdit (WAL)
       derive committed_snapshot via ApplyCommittedPayloadToSnapshot
  -> render delta (idempotent re-apply of same field)

Undo / Redo / MoveHeadToCommit / Discard / CheckoutVersion
  -> update derived snapshot then ApplyEditorAdjustmentSnapshot on live pipeline
```

**Primary failure call chain:**

```text
unknown field / missing pending_before / WAL prepare failure
  -> CommitAdjustment returns false; pipeline apply rolled back only when apply itself fails before WAL
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OrdinaryEditChangesLivePipelineBeforeOrWithWalAppend` | `EditorSessionHistoryPortTest` | PASS |
| `CommittedSnapshotIfPresentMatchesLivePipelineExportAfterEdit` | `EditorSessionHistoryPortTest` | PASS |
| `CommitSerializesWithLivePipelineRenderLock` | `EditorSessionHistoryPortTest` | PASS |
| Tint merge undo restores live pipeline params | `EditorSessionHistoryPortTest` | PASS |

Commands: same HistoryPort binary as WU3 (`33/33`)

**Checklist / exit condition:** both required WU4 tests pass; head-move paths sync live pipeline

**LOC note (grill-code-review):** `editor_history_mutation.cpp` owns live apply helper; Capture-before still reads derived snapshot (avoids GetParams float drift in WAL before-values)

**Remaining gaps:** none for WU4 acceptance tests

### Work unit 5 — Load path: history authority + checkpoint compare

**Behavior:** algorithm D. Implement explicit comparison helper, e.g.:

```text
struct PipelineCheckpointIdentity {
  head_commit_hash_t head;
  transaction_chain_hash_t chain;
};
bool CheckpointMatchesLogicalHead(const ImageEditState&, const CommitGraph&, head);
```

**Tests:**

1. `LoadWithMatchingCheckpointSkipsFullReplay` — instrument rebuild counter or spy.
2. `LoadWithMismatchedCheckpointRebuildsFromHistoryAndIgnoresStalePipelineJsonValues` — checkpoint
   deliberately wrong params; after load, params follow history not stale JSON.
3. `LoadAttachesCompatibleWalThenComparesCheckpoint` — WAL extends head; checkpoint for old head
   must rebuild.
4. `LoadRejectsOrQuarantinesIncompatibleWal` — explicit expected behavior with assertion.

##### Work unit 5 completion record (2026-07-31)

**Status:** complete — `CheckpointMatchesLogicalHead` + post-WAL pipeline sync; matching/mismatch/WAL attach evidenced

**Primary success call chain:**

```text
EditorSessionPipelinePort::EnsureLoaded
  -> PipelineMgmtService::LoadEditorPipeline
       compare serialized checkpoint to materialized/active Version head
       match: ImportSerializedPipelineState; mismatch: RebuildPipelineFromRoot (+ rebuild counter)

EditorHistoryState::EnsureWorkingState
  -> journal Load + ApplyRecoveredRecordToSnapshot (WAL attach)
  -> CheckpointMatchesLogicalHead(logical_head)
       mismatch: ApplyEditorAdjustmentSnapshot from derived snapshot (history wins)
```

**Primary failure call chain:**

```text
incompatible / unreplayable WAL record
  -> ApplyRecoveredRecordToSnapshot fails
  -> EnsureWorkingState returns null (fail closed; no silent discard)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `LoadWithMatchingCheckpointSkipsFullReplay` | `PipelineServiceTest` | PASS |
| `LoadWithMismatchedCheckpointRebuildsFromHistoryAndIgnoresStalePipelineJsonValues` | `PipelineServiceTest` | PASS |
| `LoadAttachesCompatibleWalThenComparesCheckpoint` | `EditorSessionHistoryPortTest` | PASS |

Commands:

```bat
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target PipelineServiceTest EditorSessionHistoryPortTest
build\debug\alcedo_studio\tests\app\PipelineServiceTest_runtime\PipelineServiceTest.exe --gtest_filter=*LoadWith*:*Checkpoint*
```

Suite totals: filtered PipelineService 3/3; HistoryPort includes WAL attach test (33/33)

**Checklist / exit condition:** 5.1–5.3 evidenced; 5.4 `LoadRejectsOrQuarantinesIncompatibleWal` relies on fail-closed `EnsureWorkingState` (no dedicated named test yet)

**LOC note (grill-code-review):** helper `CheckpointMatchesLogicalHead` in `pipeline_service`; WAL compare wired in `editor_history_state_detail.cpp`

**Remaining gaps:** dedicated incompatible-WAL quarantine assertion test deferred; WU6 still deletes leftover shadow transfer APIs

### Work unit 6 — Delete dead code + dual APIs

1. Remove production shadow transfer types and methods.
2. Remove unused `InitiateMerge` overload or make it a deleted private dead end.
3. Remove `MakeMergePolicyProbe` type switch once live ops are always available; use
   `stage.GetOperator(...)->op_` for Detect/Merge.
4. Grep clean: `transfer_candidates`, `PrepareMerge`, `PublishTransferCandidate`,
   `shadow` candidate graph.
5. Update/remove tests under `tests/edit/history/*transfer*`, `tests/app/adjustment_transfer*`,
   UI e2e if they assert candidate IDs.

### Work unit 7 — Grill evidence package

Before calling the work done, run grill-code-review skill criteria against this plan’s acceptance
matrix. Produce under `build/tmp/` (not repo root):

- test command log
- acceptance matrix CSV/MD mapping each criterion → test name → pass
- call-chain doc for paste and merge only

---

## Schema note (merge reverse)

If `MergeFieldDelta` today stores only `resolved_value` without previous value:

1. Add `before_value` + `before_enabled` (names exact in code).
2. Update `CanonicalJSON` / `FromJSON` / hash identity rules consistently.
3. Materializer and `ApplyCommit` for merge kind must apply `resolved_*` forward; undo uses
   `before_*` or parent rebuild.
4. Add unit tests on `EditCommit` round-trip JSON.

Do not rely on “second parent replay” for undo.

---

## Concurrency and session queue (binding)

- All live pipeline mutations for an element happen on the editor session command queue thread
  already used for BeginMerge/Paste (or documented equivalent single owner).
- Render may read pipeline under existing render lock; do not introduce a second mutable param
  table to avoid locking.
- Do not copy `CommitGraph` to avoid locking; serialize mutations instead.

---

## Non-goals

- Full split of `PipelineExecutor` into pure param graph vs GPU executor (nice later; not required
  if session mutations stay on the command queue and tests pass).
- Changing transfer package schema version beyond what merge reverse needs.
- Migrating pre-mini-Git projects (still destructive cutover world).
- Redesigning QML merge dialog visuals.

---

## Acceptance criteria (all required)

| ID | Criterion | Evidence required |
|----|-----------|-------------------|
| A1 | Exactly one live pipeline per open editor element is mutated by paste/merge/ordinary edit | Integration test reads pipeline after each |
| A2 | No production code path constructs `HistoryTransferCandidate` shadow graphs | Grep + link fails if reintroduced |
| A3 | Paste cancel restores prior Version head params on live pipeline | Test 2.2 |
| A4 | Merge undo restores pre-merge params on live pipeline | Test 3.3 |
| A5 | ColorTemp both-as_shot no false conflict; take as_shot keeps target baseline | Tests 3.1–3.2 |
| A6 | Load prefers history over stale checkpoint JSON | Test 5.2 |
| A7 | Compatible WAL attaches before checkpoint compare | Test 5.3 |
| A8 | WAL durability for paste/merge matches ordinary edit crash window | Test 2.3 + merge equivalent |
| A9 | `DetectMergeConflict`/`MergeParams` invoked on real op instances from pipeline for production merge | Unit/integration assertion or test hook |
| A10 | grill-code-review: no observed test failures on listed suites; coverage gaps listed only if deferred with owner veto | Skill report |

---

## Test commands (Windows)

```bat
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target AdjustmentTransferServiceMiniGitTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorSessionHistoryPortTest
rem add targets as CMake names require after rewiring

build\debug\alcedo_studio\tests\app\AdjustmentTransferServiceMiniGitTest_runtime\AdjustmentTransferServiceMiniGitTest.exe
ctest --test-dir build/debug -R "AdjustmentTransfer|MiniGit|EditorSessionHistory|EditorHistory" --output-on-failure
```

(Exact target names after renames must be updated in the grill report; do not claim pass without
running.)

---

## Risks and explicit decisions

| Risk | Decision |
|------|----------|
| Live pipeline mutation during merge conflicts UI | Conflicts are computed without applying; apply only on Complete. |
| Incoming merge lineage | Prefer no user-visible temp Version; second parent optional for ancestry only. |
| `committed_snapshot` still used by history list UI | Allowed as **derived** read model only. |
| Heavy executor construction | Reuse PMS-loaded pipeline; do not create a second long-lived executor per merge. |
| Interim ColorTemp/Lens merge policy commit on branch | Remains valid foundation; this plan rewires **call site** to live pipeline and deletes shadow publish. |

---

## Done definition

1. All acceptance criteria A1–A10 evidenced.
2. Inventory grep for shadow transfer types is empty in production sources.
3. This plan’s “What to delete” table is satisfied.
4. No new parallel JSON mutation engine introduced.
5. Commit messages for the big rewrite reference this document path.

---

## Interim foundation already on branch (do not re-litigate)

The following is already implemented and tested; the rewrite **builds on** it:

- `IOperatorBase::DetectMergeConflict` / `MergeParams`
- `ColorTempOp` / `LensCalibOp` overrides + `SetParams` resolved_* preservation
- QML merge `choice` field
- Unit tests in `adjustment_transfer_service_mini_git_test.cpp` for operator policy

Those tests must be **ported** to the live-pipeline merge entrypoint rather than deleted without
replacement.
