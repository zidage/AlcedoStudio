# Shared Color Grade Synchronization

Date: 2026-11-15

Status: design proposal for discussion; not approved or implemented.

Source reviewed: `feature/batch-paste-version-choice` at `e32a3993`.

## 1. Whole design

A **shared source** owns an ordered group of Color Grade nodes. Images use
references to its published revisions. Each image keeps its own Develop,
geometry, DRT/Post, and local Grades outside the group.

Sharing creates one new tracking Version in each participating image, including
the source image. Previous Versions remain unchanged. Later updates add commits
to tracking Versions, not more named Versions.

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

1. In A, select complete Color Grade nodes in their pipeline order.
2. Choose **Share Color Grades**, select B and C, name the source, and choose
   Automatic or Manual. Automatic is the default.
3. Review the sequence and affected images, then confirm **Create Links**.
4. Create S and make a new tracking Version active in A, B, and C. Keep the
   library selection.

Creation starts from each image's root Develop, geometry, and DRT/Post. Only
selected Grades enter the shared group. Other current edits remain in the
previous Versions. State this in the dialog before confirmation. Do not match
or combine the group with existing target Grades.

The Nodes panel shows an expandable group boundary. Its header shows the name,
mode, affected-image count, and actions. Automatic editing displays **Editing
shared group: 3 images**. Manual status shows **Up to date** or **Update
available**. Local adjustment panels remain editable in either mode.

| Action inside an Automatic group | Shared result |
| --- | --- |
| Edit a Grade's values, enabled state, mix, name, or supported Masks | Publish the changed group. |
| Reorder X and Y | Publish the new order. |
| Add Color Grade | Insert after the selected child, or at the end if none is selected. |
| Delete X | Remove X from the group. |
| Delete every child | Keep an empty group that passes its input through. |

Adding local Grades before or after the group, moving the whole group, and
editing image-specific settings affect only that Version. A child cannot be
dragged across the group boundary. Use **Make Independent** to separate it.

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

For example, B's slider drag previews only in B. Release publishes r2. Automatic
A and B adopt r2. Manual C keeps r1. This behavior requires no branch comparison,
unpublished draft storage, or conflict dialog.

The source owner serializes publications. A settled edit means one completed
input sequence, such as slider release. Inactive tracking Versions also receive
updates when their policy is Automatic. Ordinary Versions and historical commits
do not change. A is not a permanent owner: deleting A does not delete S.

## 4. History and Undo

Keep rendering state separate from tracking policy:

- An image commit stores an exact source revision. Rendering, replay, and history
  preview resolve that revision; they never substitute the current source head.
- Each Automatic adoption creates a small image commit that changes the revision
  reference. This keeps existing image history authoritative.
- Automatic/Manual is mutable tracking policy for a Version's group instance.
  The image history owner stores it alongside the Version reference. It is not
  part of the immutable rendering state.

**Image Undo/Redo** uses the existing image history. Before moving its head,
pause that Version's tracking policies in the same transaction. Show
**Manual: history navigation paused synchronization**. Redo stays paused.
References restored by Undo are also Manual. **Resume Automatic** explicitly
returns them to the latest source revision.

Thus Undo in B can restore B to r1 without changing S or A. Merely previewing an
old history entry changes neither the head nor the tracking policy.

**Restore Shared Revision** is a separate source-history action. Selecting r1
publishes its contents as a new source revision. Show the affected Automatic
followers before confirmation. Manual references stay pinned. This action
restores a complete known group; it does not selectively reverse old edits or
maintain a second Undo/Redo stack.

## 5. Local and global removal

| Action | Result |
| --- | --- |
| Remove from This Version | Remove only the current reference. |
| Make Independent | Replace the reference with local nodes at the same position, preserving displayed values and order. Assign new local IDs. |
| Remove from All Linked Versions | After showing affected images and Versions, remove all current references in one operation, including Manual references. |

Local removal behaves like unlinking a hard link. Global removal is an explicit
batch operation. Retain source revisions while any live or historical reference
needs them. Source node IDs remain stable across published revisions.

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
3. Image Undo/Redo pauses tracking and preserves replay after reopen.
4. Restoring shared r1 publishes a new revision and leaves Manual pins unchanged.
5. Local removal leaves other links intact. Global removal is atomic; historical
   references remain readable and local Undo restores a paused link.
6. A failed DB transaction changes no heads or policies.
7. Export honors different target revisions throughout a concurrent source edit.
8. Unsupported Masks fail before creating any source or tracking Version.

Related specifications:

- [Node editor master plan](node_mask_editor_master_plan.md), sections 2.6 and 14.
- [History, Version, and Paste](node_mask_editor/phase_nm4_history_version_paste_plan.md).
- [Node-aware transfer](node_mask_editor/phase_nm10_adjustment_transfer_plan.md).

This proposal does not change those specifications' approval status. It defines
no implementation phases and claims no build or test evidence.
