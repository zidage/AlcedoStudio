# Shared Color Grade Synchronization

Date: 2026-11-15

Revised: 2026-11-16

Status: design proposal for discussion; not approved or implemented.

Source reviewed: `feature/batch-paste-version-choice` at `e32a3993`.

## 1. Whole design

A **shared group** is an ordered list of Color Grade nodes stored once as a
**shared source**, S. Each participating photo has a link to S in a tracking
**Version**, an alternative edit of that photo. A **revision** is one saved
state of S, including its node order and settings.

S has one shared history cursor. Each photo Version has its own local history
cursor. Auto Sync links follow S's cursor; they do not copy its edits into their
local histories.

Creating a group creates a tracking Version in the source photo. Sharing it
creates a tracking Version in each target photo. Both operations start from the
photo's **root**, its stored initial editing state. They preserve that photo's
root Develop, geometry, and DRT/Post settings. They do not inherit edits from
its previously active Version or write into that Version.

Previous Versions remain available. Later synchronization changes the shared
state shown by tracking Versions without creating local commits or more named
Versions. Users can add local Grades outside the shared group or change
image-specific settings in a tracking Version.
Those edits stay local.

```text
Shared source S: r1 [X, Y] -> r2 [X exposure +1, Y] -> r3 [Y, X exposure +1]

Image A: Develop A -> [S at r3] -> DRT A
Image B: Develop B -> [S at r3] -> DRT B
Image C: Develop C -> [S at r1] -> DRT C   (Manual Updates; newer revision available)
```

**Auto Sync** allows shared-node editing and follows shared edits, Undo, and
Redo. **Manual Updates** holds a fixed revision until **Update to Latest**.
Its shared nodes are read-only. There are no private drafts, Push operations,
or branch-merging workflows.

## 2. Create and edit a group

1. In A's Nodes panel, select complete Color Grade nodes. Right-click and choose
   **Create Shared Color Grades**. The app keeps their pipeline order.
2. Name S and choose A's mode: Auto Sync or Manual Updates. Auto Sync is the default.
   On confirmation, the app creates S and a tracking Version from A's root,
   then makes that Version active.
3. The app remembers S for the next sharing action, like a copied item for Paste.
4. In the Library view or editor filmstrip, select target photos B and C.
   Right-click and choose **Share Color Grades**.
5. Review S and the targets. Choose Auto Sync or Manual Updates for the new links;
   Auto Sync is the default. Confirm sharing. The app creates and activates a
   tracking Version from each target's root, using S's current shared cursor
   for the batch. Keep the photo selection after completion.

Before either confirmation, explain that unselected edits remain in the previous
Versions. There is no current-Version destination. Disable Share Color Grades
until the app has a source to share. It remembers the most recently created
source and reuses it for later target selections. Sharing links to that source;
it does not create a new source for each target. Each link's mode can later change
independently.

The Nodes panel shows an expandable group boundary. Its header shows the name,
mode, and linked-photo count. While editing, show the number of photos
with Auto Sync links that will change: **Changes affect 2 photos** in the
example above. Manual Updates status shows **Up to date** or **Update available**.
Panels for local Grades and image-specific settings remain editable in either
mode. Shared-node controls are read-only in Manual Updates mode.

Group and node context menus are separate:

| Right-click location | Menu actions |
| --- | --- |
| Empty space inside the group boundary, outside all nodes | Remove from This Photo, Remove from All Linked Photos, Make Independent, and the mutually exclusive mode choices Auto Sync and Manual Updates. Show the current mode as selected. In Manual Updates mode, also offer Update to Latest. |
| A node inside the group | Insert Before Selected Node, Insert After Selected Node, Delete Node, Move Earlier, and Move Later. These actions change the shared contents. |

Right-clicking a node targets that node and never opens the group menu. The
group menu never deletes or reorders individual nodes. Disable node-changing
actions in Manual Updates mode. Keep enough empty space inside the group for
its menu to remain accessible, including when collapsed.
Keyboard context-menu access uses the focused group or node with the same scope.

Node insertion works before the first or after the last node. Move Earlier and
Move Later swap the node with its neighbor; disable unavailable directions.
Deleting the last node keeps an empty group that leaves the image unchanged.
All node edits in Auto Sync mode change the shared contents.

Adding local Grades before or after the group, moving the whole group, and
editing image-specific settings affect only that Version. A node cannot be
dragged across the group boundary. **Make Independent** converts the whole group
to local nodes; those nodes can then move separately.
In a nonempty group, insert, delete, and reorder nodes only through node context
menus; dragging does not reorder them. An empty group is the insertion exception:
right-click its empty space and choose **Insert Node**. Offer this action only
when the group has no nodes. It creates the first Grade and publishes the change
to Auto Sync links.
Groups in Manual Updates mode remain read-only. Choose Auto Sync to edit the
shared source, or Make Independent to edit a local copy.

Use AppTheme controls and keyboard access. Disable affected actions while loading.
Errors state their cause and keep the last valid state.

## 3. Synchronization rules

| Action | Exact behavior |
| --- | --- |
| Edit in Auto Sync mode | Preview input locally. On completion, publish one source revision and advance the shared cursor. All Auto Sync links show it. |
| Select Manual Updates in the group menu | Hold the revision currently shown. Shared controls become read-only. Record this mode change in local history. |
| Update to Latest | Hold the revision at S's current cursor. Stay in Manual Updates mode. Record the change in local history. Disable this action when already up to date. |
| Select Auto Sync in the group menu | Follow S's current cursor and enable shared-node editing. Record the mode change in local history. |
| Edit a group in Manual Updates mode | Choose Auto Sync or Make Independent first. |

In Auto Sync mode, publish each completed edit to the group's contents
automatically. There is no **Publish** button or separate Apply step. Mode
changes and other local operations do not create source revisions. Completion
means:

| Edit type | Completion point |
| --- | --- |
| Slider, color wheel, or supported Mask control drag | Pointer release. Preview intermediate values only in the photo being edited. |
| Typed numeric value or node name | Enter or leaving the field, after validation succeeds. Typing alone does not publish. |
| Checkbox, menu choice, or keyboard value step | Immediately after the control accepts the change. |
| Node insertion, deletion, or reordering | Immediately after the complete operation passes validation. Publish the whole operation, not an intermediate sequence. |

Each completed shared edit that changes the group creates one source revision
and advances its cursor. Invalid input, cancelled edits, and unchanged final
values create no revision. Cancelling an edit restores its committed preview.
Confirming with Enter and then leaving the field must not publish twice.

Starting at r1, B's slider drag previews locally. Release publishes r2. Auto Sync
A and B show r2; C in Manual Updates keeps r1.

Inactive tracking Versions also update when their mode is Auto Sync. Ordinary
independent Versions and old history entries do not change. Deleting A does not
delete S.

"Latest" means the revision at S's current cursor, not the furthest revision
ever created. After shared Undo, Update to Latest can therefore select an older
revision. A Manual Updates link stays fixed unless its user requests an update.

If every link uses Manual Updates, normal shared-node editing is disabled
everywhere. Select Auto Sync on one photo to edit the source. Other photos keep
their revisions until their users choose Update to Latest or Auto Sync.

## 4. Separate cursors and combined history

| History | Edits it owns | Cursor scope |
| --- | --- | --- |
| Shared source S | Shared node values, Masks, names, insertion, deletion, and order | One cursor used by all Auto Sync links, across all photos and Versions. |
| Photo Version | Local Grades, Develop, geometry, DRT/Post, group placement, linking/removal, mode changes, and Update to Latest | One local cursor for that Version. |

Shared Undo moves S's cursor to the previous revision. Shared Redo moves it
forward along its active Redo path. Both update every Auto Sync link, including
inactive Versions. Neither creates a revision or changes a photo's local cursor.
Manual Updates links remain fixed.

Local Undo/Redo moves only the current Version's local cursor. It does not move
S's cursor. Undo of a node edit never changes sync mode. Undo of an explicit
mode change restores that mode change's previous state, as a local operation.

### Combined view and keyboard routing

The History panel combines local entries with references to linked source entries,
including edits from other photos. Rows identify their owner and applied/Redo
state. Showing a shared edit creates no local history entry.

Persist a unique, increasing project-wide edit order with each completed edit.
Do not compare wall-clock times or unrelated revision numbers.

Ctrl+Z and the existing Redo shortcut/menu route automatically:

1. **Undo:** consider the last applied local edit and the last applied edit in
   each currently linked Auto Sync source. Choose the greatest edit order and
   move only that owner's cursor backward.
2. **Redo:** consider the next edit on each eligible history's Redo path.
   Choose the smallest edit order and move only that owner's cursor forward.
3. Recompute candidates after every cursor or link change, including changes
   made from another photo. Count the same source only once in a photo's view.
4. Exclude shared histories reached only through Manual Updates links. Their
   displayed source entries are read-only. Local mode and update actions remain
   eligible. Initial states have no Undo step. Disable actions with no eligible step.

Node selection does not choose the cursor. Menu/tooltip labels name the next
action, such as **Undo Shared Contrast (2 photos)** or **Undo Local Curve**.
Text-field Undo still handles unconfirmed text; routing applies to completed edits.

Example, with A and B in Auto Sync:

| Edit order | Action | History owner |
| --- | --- | --- |
| 1 | Change X's exposure from A | S |
| 2 | Add a local curve in A | A's Version |
| 3 | Change Y's contrast from B | S |

Three Undo actions in A undo shared edit 3 for both photos, then local edit 2
only in A, then shared edit 1 for both photos. Redo applies 1, 2, then 3.
If another photo already moved S's cursor, A uses that new position for its
next shortcut. Undo does not search for the current user's last shared edit.

Cursor moves are not new edit entries. A new edit replaces only its owner's
active Redo path. A local edit cannot erase shared Redo, or vice versa. Keep
older revisions while links, historical states, or exports still need them;
retaining those revisions does not expose a branch-merging workflow.

### Live state and historical state

A live photo uses its local cursor plus each Auto Sync source's current cursor
or each Manual Updates link's fixed revision. One photo commit alone therefore
does not describe its complete live rendering state.

A frozen state records the local commit and exact source revision IDs. Local
entries retain those IDs for historical replay, not to rewind live sources.
A shared-entry preview uses current local state with the selected source revision.
Preview moves no cursor and changes no mode. Comparison and export use fixed
revision references too.

## 5. Local and global removal

Open these actions by right-clicking empty space inside the group, outside its
nodes. They act on the group link, not the node that was last selected.

| Action | Result |
| --- | --- |
| Remove from This Photo | Remove the group from this photo's active Version. Other Versions of the same photo keep their links. |
| Make Independent | Replace the reference with local nodes at the same position, preserving displayed values and order. Assign new local IDs. |
| Remove from All Linked Photos | Show affected photos and Versions, then remove the group from every linked Version, including inactive Versions and links in Manual Updates mode. |

Local removal behaves like unlinking a hard link. Global removal changes linked
Versions, not old history entries. Retain source revisions needed by Versions,
history, or running exports. Source node IDs remain stable.

Removal and Make Independent are local history operations. Undo restores the
link's prior mode: Auto Sync follows S's current cursor; Manual Updates restores
its fixed revision. It never rewinds S. Global removal writes a local removal
step in each affected Version atomically. Undo from one photo restores only its
own link; there is no separate all-photos Undo stack.

## 6. Data ownership and publication

Proposed records:

| Record | Owned data |
| --- | --- |
| `SharedGradeSource` | `source_id`, `name`, `current_revision_id`, and active Redo-path tip. Owns the shared cursor. |
| `SharedGradeRevision` | `revision_id`, `source_id`, `parent_revision_id`, `edit_order`, immutable `grade_chain_json`. Reuse Color Grade serialization. |
| Group link in local document/history | `instance_id`, `source_id`, `mode`, and a fixed revision for Manual Updates. The image graph owns placement. Auto Sync resolves the source cursor. |
| Photo Version history | Local commits, local cursor and active Redo path, edit order, and exact observed source revision IDs for historical replay. |

DuckDB stores each source revision once. Live Auto Sync updates write no
per-photo adoption commits. Runtime identity combines the group instance ID and
source node ID. Names never establish identity. Immutable revisions retain old
results. Historical and export references contain IDs, not editable copies of S.

A proposed `SharedGradeService` owns source operations for one project.
`PipelineMgmtService` owns local commits, cursors, and link modes. The UI calls
these services. `PipelineMgmtService` routes Undo/Redo through the source owner
when the chosen edit is shared. Storage obtains a connection at the operation
boundary. The source service processes shared edits one at a time.

```text
Completed shared edit -> source owner -> validate group
  -> one DB transaction: edit order + new revision + shared cursor/Redo path
  -> publish committed state -> refresh linked history views and previews/thumbnails
  -> render visible images at existing quality
```

Undo/Redo uses the same owner path but persists only cursor movement; it allocates
no edit order or revision. The local path writes only local edits or cursor moves.
Local operations capture required source references through the source owner.
Select routing candidates and move the chosen cursor in the same serialized
project history operation. Allocate edit order in the committing transaction.

Render and checkpoint identity must include the local commit and resolved source
revision IDs. A shared cursor move invalidates affected cached results even when
the photo's local cursor did not move. Independent Versions keep their existing
single-history behavior.

Use the open image's existing editor queue to settle pending input before an
update. Validate before mutation. On persistence failure, leave affected edits,
cursors, modes, and Redo paths unchanged. Restore any initiating preview.
After a crash, reopen reconstructs committed state. Rendering failure reports
the real error without changing backend or quality.

An export batch captures each target's local cursor and exact source revisions
in one consistent read when the batch starts. Keep them alive until completion.
Honor Manual Updates links and historical selections: targets can use different
revisions of S. Do not replace those choices with S's current cursor.

## 7. Masks and compatibility

Share analytic Radial and Linear Gradient Masks in normalized reference
coordinates. Their locations stay identical across participants; they do not
track subjects or register stars. Never union them with unrelated target Masks.

Reject shared groups containing AI segmentation or Brush Masks, and reject
inserting those types. Their image-dependent data needs its own cross-image
specification. Report the unsupported nodes; do not omit Masks or reuse A's
raster on B. Independent Versions retain their existing Mask behavior.

Sharing stays within one project. Packages retain referenced revisions and
dependencies. Increment project and document format versions. Preserve existing
independent documents. Older applications must reject the new format.
Cross-project tracking, private drafts, automatic graph merging, and
registration are excluded.

## 8. Integration and acceptance

[PR #917](https://github.com/zidage/AlcedoStudio/pull/917#issuecomment-6880215213)
proposes explicit tracking for [#764](https://github.com/zidage/AlcedoStudio/issues/764).
Shared groups supply identity and order; a one-node group supports individual sharing.

Ordinary Paste remaps identities. This branch's
`DocumentTransferPlanner::PlanOntoCurrent` matches Grades by identity or name;
shared synchronization must not use that path. Proposed records and
`SharedGradeService` do not exist yet.

Under `alcedo_studio/src/`, extend `edit/graph` for references, `edit/history` for
cursors/routing, `app` for ownership/cache identity, `storage` for transactions,
and `ui/alcedo_main` for controls. Reread `AGENTS.md` and applicable skills before
implementation. Required owner-test assertions:

1. Completed node edits reach all Auto Sync links. Invalid, cancelled, or unchanged
   edits publish nothing. Each completion publishes once without a Publish button.
2. Manual Updates stays fixed through shared edits/Undo/Redo. Update to Latest
   uses S's cursor, even after Undo, without enabling editing.
3. The example routes Undo as 3, 2, 1 and Redo as 1, 2, 3. Shared steps affect
   all Auto Sync links without new revisions/local commits. Local steps stay local.
4. B's shared cursor move changes A's next candidate. Multiple links to S count
   once. Manual Updates links cannot route shared steps.
5. Local removal preserves other Versions. Global removal is atomic and includes
   inactive/Manual Updates links. Local Undo restores the prior link mode only.
6. Write failure changes no edits, cursors, modes, or Redo paths. Reopen preserves
   cursors and Redo. New edits replace only their owner's Redo path.
7. Frozen preview/export states survive later cursor changes. Preview moves no
   cursor. Shared movement refreshes caches without changing local history.
8. Unsupported Masks fail before creation. Retained historical revisions stay readable.
9. Source creation and Library/filmstrip sharing always create Versions from root,
   preserve previous Versions, and keep target selection.
10. Group/node menus keep their separate scopes. Empty Auto Sync groups allow
    Insert Node. Manual Updates disables node-changing actions.
11. With all links in Manual Updates, shared editing stays disabled until one
    switches to Auto Sync. Other fixed links do not change.

Related specifications:

- [Node editor master plan](node_mask_editor_master_plan.md), sections 2.6 and 14.
- [History, Version, and Paste](node_mask_editor/phase_nm4_history_version_paste_plan.md).
- [Node-aware transfer](node_mask_editor/phase_nm10_adjustment_transfer_plan.md).

This proposal does not change those specifications' approval status. It defines
no implementation phases and claims no build or test evidence.
