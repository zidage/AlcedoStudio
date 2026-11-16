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

Creating a group creates a tracking Version in the source photo. Sharing it
creates a tracking Version in each target photo. Both operations start from the
photo's **root**, its stored initial editing state. They preserve that photo's
root Develop, geometry, and DRT/Post settings. They do not inherit edits from
its previously active Version or write into that Version.

Previous Versions remain available. Later synchronization updates the tracking
Versions without creating more named Versions. Users can add local Grades
outside the shared group or change image-specific settings in a tracking Version.
Those edits stay local.

```text
Shared source S: r1 [X, Y] -> r2 [X exposure +1, Y] -> r3 [Y, X exposure +1]

Image A: Develop A -> [S at r3] -> DRT A
Image B: Develop B -> [S at r3] -> DRT B
Image C: Develop C -> [S at r1] -> DRT C   (Manual: Pull available)
```

**Automatic** follows published revisions. **Manual** holds a published
revision until Pull. Manual group contents are read-only. There are no private
drafts, Push operations, or source branches to merge.

## 2. Create and edit a group

1. In A's Nodes panel, select complete Color Grade nodes. Right-click and choose
   **Create Shared Color Grades**. The app keeps their pipeline order.
2. Name S and choose A's mode: Automatic or Manual. Automatic is the default.
   On confirmation, the app creates S and a tracking Version from A's root,
   then makes that Version active.
3. The app remembers S for the next sharing action, like a copied item for Paste.
4. In the Library view or editor filmstrip, select target photos B and C.
   Right-click and choose **Share Color Grades**.
5. Review S and the targets. Choose Automatic or Manual for the new target links;
   Automatic is the default. Confirm sharing. The app creates and activates a
   tracking Version from each target's root, using the same latest published
   revision of S for the batch. Keep the photo selection after completion.

Before either confirmation, explain that unselected edits remain in the previous
Versions. There is no current-Version destination. Disable Share Color Grades
until the app has a source to share. It remembers the most recently created
source and reuses it for later target selections. Sharing links to that source;
it does not create a new source for each target. Each link's mode can later change
independently.

The Nodes panel shows an expandable group boundary. Its header shows the name,
mode, linked-photo count, and actions. While editing, show the number of photos
with Automatic links that will change: **Changes affect 2 photos** in the
example above. Manual status shows **Up to date** or **Update available**.
Panels for local Grades and image-specific settings remain editable in either
mode. Shared-node controls are read-only in Manual mode.

| Action inside an Automatic group | Shared result |
| --- | --- |
| Edit a Grade's values, enabled state, mix, name, or supported Masks | Publish the changed group. |
| Reorder X and Y | Publish the new order. |
| Right-click a child and choose Insert Before Selected Node | Insert a shared Grade immediately before that child, including before the first child. |
| Right-click a child and choose Insert After Selected Node | Insert a shared Grade immediately after that child, including after the last child. |
| Right-click an empty group and choose Insert Node | Insert the first shared Grade. |
| Delete X | Remove X from the group. |
| Delete every child | Keep an empty linked group that leaves the image unchanged at that point in the pipeline. |

Adding local Grades before or after the group, moving the whole group, and
editing image-specific settings affect only that Version. A node cannot be
dragged across the group boundary. **Make Independent** converts the whole group
to local nodes; those nodes can then move separately.
In a nonempty group, insert shared Grades only through the child context-menu
actions. In an empty group, right-click the group and choose **Insert Node**.
This creates its first Grade and publishes the change to Automatic followers.
Manual groups remain read-only until Resume Automatic or Make Independent.

Use existing AppTheme controls and keyboard access. Loading disables the affected
actions. An error states its cause and keeps the last valid state.

## 3. Synchronization rules

| Action | Exact behavior |
| --- | --- |
| Edit in Automatic mode | Preview input locally. On completion, publish one source revision and update all Automatic references. |
| Switch to Manual | Keep the currently adopted revision. Shared controls become read-only. |
| Pull in Manual mode | Adopt the latest published revision. Stay Manual. |
| Resume Automatic | Adopt the latest revision and enable future automatic updates. |
| Edit a Manual group | Choose Resume Automatic or Make Independent first. |

Starting with A, B, and C at r1, B's slider drag previews only in B. Release
publishes r2. Automatic A and B update to r2. Manual C keeps r1. No branch
comparison, unpublished draft storage, or conflict dialog is needed.

Each completed shared edit saves a new revision. Inactive tracking Versions also
update when their mode is Automatic. Ordinary independent Versions and old
history entries do not change. Deleting A does not delete S.

## 4. History and Undo

The app records each image edit as a **commit**, a history entry that can be
replayed. Keep the recorded image state separate from its tracking policy:

- Each linked group in the recorded image state refers to an exact source
  revision. Rendering, replay, and history preview use that revision, not the
  source's latest revision.
- Each Automatic adoption creates a small image commit that changes the revision
  reference. This keeps existing image history authoritative.
- Automatic/Manual is mutable tracking policy for a Version's group instance.
  The image history owner stores it alongside the Version reference. It is not
  part of the immutable rendering state.

**Image Undo/Redo** uses the existing image history. When it moves to a different
history entry, switch that Version's group links to Manual in the same
transaction. This includes Undo of a local edit. Show **Manual: history
navigation paused synchronization**. Redo stays Manual. A link restored by Undo
also starts in Manual mode. **Resume Automatic** updates the selected link to
the latest revision and enables future updates.

Thus Undo immediately after a shared edit restores B's previous image state
without reversing the edit in S or A. Merely previewing an old history entry
changes neither the current history position nor the tracking policy.

**Restore Shared Revision** is a separate source-history action. Selecting r1
publishes its contents as a new source revision. Show the affected Automatic
followers before confirmation. Manual references stay pinned. This action
restores a complete known group; it does not selectively reverse old edits or
maintain a second Undo/Redo stack.

## 5. Local and global removal

| Action | Result |
| --- | --- |
| Remove from This Photo | Remove the group from this photo's active Version. Other Versions of the same photo keep their links. |
| Make Independent | Replace the reference with local nodes at the same position, preserving displayed values and order. Assign new local IDs. |
| Remove from All Linked Photos | Show affected photos and Versions, then remove the group from every linked Version, including inactive Versions and Manual links. |

Local removal behaves like unlinking a hard link. Global removal changes each
linked Version's current state in one operation; it does not erase old history
entries. Retain source revisions while any Version, history entry, or running
export needs them. Source node IDs remain stable across published revisions.

Removal and Make Independent are recorded in image history. Image Undo can
restore that Version's reference in Manual mode. Global removal does not add a
separate all-images Undo system. Users can restore individual references through
image history. Old commits continue to render their original contents.

## 6. Data ownership and publication

Proposed records:

| Record | Owned data |
| --- | --- |
| `SharedGradeSource` | `source_id`, `name`, `head_revision_id`. |
| `SharedGradeRevision` | `revision_id`, `source_id`, `parent_revision_id`, immutable `grade_chain_json`. Reuse Color Grade serialization. |
| Group reference in image document/history | `instance_id`, `source_id`, exact `revision_id`; the image graph owns placement. |
| Tracking policy owned with Version refs | `(version_id, instance_id, mode)`. Controls future adoption, not historical rendering. |

DuckDB stores each revision's contents once. Image commits store reference
changes. Runtime node identity combines the group instance ID and source node
ID. Names never establish identity. Immutable revisions are necessary to retain
old results; readers use their models without creating another editable source.

A proposed `SharedGradeService` owns source operations for one project.
`PipelineMgmtService` owns image commits and tracking policies. The UI calls
these services. Storage obtains a connection at the operation boundary.
The source service processes shared edits one at a time.

```text
Completed shared edit -> source owner -> validate affected Versions
  -> one DB transaction: new revision + source head + Automatic adoption commits
  -> publish committed state -> invalidate affected previews/thumbnails
  -> render visible images at existing quality
```

Use the open image's existing editor queue to settle pending input before an
update. Validate creation, adoption, restoration, and removal before mutation.
On persistence failure, keep all affected heads and policies unchanged.
Restore the initiating preview to its committed revision.
After a crash, reopen reconstructs committed state. Rendering failure reports
the real error without changing backend or quality.

An export batch captures each target's exact resolved revision references when
the batch starts. Keep those immutable revisions alive until completion. Honor
Manual pins and historical selections: targets can intentionally use different
revisions of S. Never replace them with one source-head revision for the batch.

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
Cross-project tracking, independent manual drafts, automatic graph merging, and
registration are excluded.

## 8. Integration and acceptance

[PR #917's maintainer response](https://github.com/zidage/AlcedoStudio/pull/917#issuecomment-6880215213)
proposes explicit tracking for [issue #764](https://github.com/zidage/AlcedoStudio/issues/764).
This proposal supplies identity and ordering without matching unrelated chains.
A one-node group also supports individual-node sharing.

Ordinary Paste remaps identities. This branch's
`DocumentTransferPlanner::PlanOntoCurrent` matches Grades by identity or name;
shared synchronization must not use that path. Proposed records and
`SharedGradeService` do not exist yet.

Extend `alcedo_studio/src/edit/graph` for references and traversal, `edit/history`
for reference changes, `app` for source ownership, `storage` for transactions,
and `ui/alcedo_main` for group controls. Before implementation, reread
`AGENTS.md` and applicable skills. Verify these behaviors through owner tests:

1. Parameter, order, insertion, and removal edits reach all Automatic followers.
2. Manual C keeps r1 after A publishes r2. Pull adopts r2 without enabling editing.
3. Image Undo/Redo pauses that Version's links, including when undoing a local
   edit. Reopen preserves the chosen history entry and Manual mode.
4. Restoring shared r1 publishes a new revision and leaves Manual pins unchanged.
5. Local removal leaves other Versions intact. Global removal includes inactive
   and Manual links and is atomic. Historical references remain readable; image
   Undo restores a Manual link.
6. A failed DB transaction changes no heads or policies.
7. Export honors different target revisions throughout a concurrent source edit.
8. Unsupported Masks fail before creating any source or tracking Version.
9. Create Shared Color Grades in A, then Share Color Grades to selected Library
   or filmstrip targets. Each operation creates root-based Versions and preserves
   previous Versions. Target selection remains unchanged. No current-Version
   destination is available.
10. Child context menus insert before the first or after the last node.
    Right-clicking an empty Automatic group offers Insert Node and publishes
    its first Grade to Automatic followers.

Related specifications:

- [Node editor master plan](node_mask_editor_master_plan.md), sections 2.6 and 14.
- [History, Version, and Paste](node_mask_editor/phase_nm4_history_version_paste_plan.md).
- [Node-aware transfer](node_mask_editor/phase_nm10_adjustment_transfer_plan.md).

This proposal does not change those specifications' approval status. It defines
no implementation phases and claims no build or test evidence.
