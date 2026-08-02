# Editor Versioning UI Handoff

## Product model

The product should present two different concepts clearly:

1. **编辑历史**: the linear timeline inside the currently selected version.
2. **版本**: independent visual treatments for the same image, such as “暖调”, “黑白”, or “高对比”.

Versions are **not** Git commits and should not read like branches in a DAG. A new version starts from the original import state, owns its own edit history, and is meant for switching between complete looks rather than accumulating incremental checkpoints.

Internally, snapshots/materialized params remain backend caches only. They exist so the editor can restore state without replaying every transaction, but the UI should not expose “snapshot” as a first-class user concept.

Backend identity (binding): edit history owns HEAD; the live pipeline is a parameter table; chain hash advances once per commit (not per operator write). Full freeze:
`docs/roadmap/alcedo_studio/ui/editor_single_live_pipeline_wal_checkpoint_plan.md` → **Final locked identity model**, and `commit_types.hpp`.

## Confirmed interaction rules

- The primary user goal is to **jump back to an earlier edit quickly**.
- The history tab only shows the current version’s edit history.
- Each visible history row should represent one real edit transaction; do not merge nearby edits yet.
- If the user jumps backward, future history remains available for redo.
- If the user performs a new edit after moving backward, the future tail is discarded.
- Switching versions should be immediate.
- Versions auto-save their latest state; there is no explicit “save to current version”.
- Versions may be renamed by the user.
- Version creation should start from the original imported state, not from the currently selected version.
- Cross-image version copy is intentionally deferred.
- Current undo/redo button treatment can stay as-is for now.

## Target information architecture

```text
┌──────────────────────────────┐
│ 编辑历史                     │
│ [历史] [版本]                │
├──────────────────────────────┤
│ 历史 tab                     │
│  当前 / 已回到某一步         │
│  三条真实记录                │
│  参数变化                    │
├──────────────────────────────┤
│ 版本 tab                     │
│  版本卡片列表                │
│  名称 + 最近修改时间         │
│  当前版本高亮                │
│  新建版本                    │
└──────────────────────────────┘
```

### 历史 tab

- Keep it compact and scannable.
- Show the current state plus recent real edit rows.
- A row should emphasize:
  - operator / action label
  - concise parameter delta
  - time
- Icons are useful, but they should remain text-sized rather than becoming the dominant visual element.
- The reference mockup’s timeline metaphor is directionally right: the user should read it as “where am I in the edit path?”

### 版本 tab

- Show only version cards, not mixed history rows.
- A card needs:
  - editable display name
  - latest modified time
  - active state
- Do not expose backend cache language such as “snapshot”.
- Avoid branch visuals; the mental model is “alternate looks”, not “fork history”.

## Recommended next implementation slice

1. Keep backend naming aligned with the product model:
   - `Version` = user-facing alternate look.
   - materialized params / snapshot = internal persistence detail.
2. Finish service-level support before heavy UI work:
   - rename persistence
   - active/head version switching
   - full transaction timeline persistence
   - cursor persistence for redo-after-jump
3. Only then rebuild the panel:
   - first separate tabs and card structures
   - then interaction polish
   - finally visual tuning

## Data-model notes for the next LLM

- `display_name` belongs in serialized `Version` data.
- A user version should keep a stable identity while its internal timeline evolves.
- Persist both:
  - the full transaction list
  - the cursor indicating how many transactions are currently applied
- A version’s parent should be the root/original import state under the currently agreed UX model.
- `materialized_params` is an optimization cache, not the source of user-facing semantics.

## What changed in this pass

- UI work was intentionally paused before landing the new panel.
- The active engineering focus moved to history semantics and service correctness.
- New tests should describe current semantics rather than the older commit-like model.
- Legacy tests that expect version IDs to mutate with every edit should stay disabled unless the product direction changes again.

## Open questions intentionally left for later

- The exact inline rename affordance for version cards.
- The final density and typography of the history timeline rows.
- Whether a future cross-image “copy version” flow should clone only params, params plus label, or a richer preset-like object.
- Whether history rows need grouping once real-world transaction volume grows.
