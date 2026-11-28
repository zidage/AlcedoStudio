# Mounted folders

A mounted folder is a collection that follows a directory on a disk. Its subdirectories are its
collections, and its supported image files are its photos. Alcedo Studio reads the directory
and keeps the collection equal to it. The direction is always from the disk to the library:
Alcedo Studio only reads the directory, and the files stay as they are.

The project database is the only store of the edit history, the Versions, the ratings, the
color labels, the Delete mark, and the collection links of each photo.

The product plan is the [Mounted Folder Plan](roadmap/alcedo_studio/storage/mounted_folder_plan.md).
The parameters are in the table at the end.

## The Collections panel

Below **All Photos**, the Collections panel has two sections:

- **Collections**: the **Library** row and the ordinary collections below it. The Library row is
  the library root. You arrange these rows.
- **Disk**: the mounts, grouped by volume. Each volume has a header row with its name and its
  mount point (`D:`), or **Offline**; its mounts follow below it. These rows follow the disk.

The parent of a mount is always the library root, so an ordinary collection never holds a mount.
**All Photos** shows every photo of the project. The **Library** row shows the photos directly in
the library root, with its collections as tiles above them.

## Mount a folder

1. In the Collections panel, click **+** on the **Disk** header. You can also click **Import** at
   the foot of the panel, choose **Import Folder...**, and choose **Mount Folder** in the dialog.
2. Choose the directory in the system folder dialog.
3. The **Import Folder** dialog opens with **Mount Folder** selected. It shows the photo count,
   the subfolder count, and the volume of the directory. Click **Mount N photos**.
4. The mount appears in the Disk section, below the row of its volume. Its first scan runs as a
   background task, and the photos appear while the scan imports them.

## Import or mount

The **Import** menu at the foot of the panel has two items:

- **Import Images...** imports the files that you choose into the selected collection.
- **Import Folder...** opens the **Import Folder** dialog after the system folder dialog.

Both choices of the dialog keep the files where they are. The difference is who owns the
structure:

| Choice | Arrange freely | Sync folder structure |
| --- | --- | --- |
| **Import into Collection** | Yes | No |
| **Mount Folder** | No | Yes |

- **Import into Collection** puts the photos into the collection that you pick in the tree once.
  The photos are then yours to arrange, and later changes on the disk do not reach the library.
  The type chips of the count line choose the file types. **Search collections** filters the
  tree, and **New collection** adds a collection inside the picked one.
- **Mount Folder** mounts the directory: its subdirectories are collections, and the collection
  follows the disk. The dialog shows the volume of the mount.

**Remember my choice** stores the choice as the default action of Import Folder (Settings,
Default Behavior, Import). With a stored choice, **Import Folder...** runs it after the system
folder dialog:

- Import imports into the selected collection, with the file types of the last dialog. While **All Photos** or a
  mounted row is selected, the photos go into **Library**.
- Mount mounts the directory.

The **+** of the Disk header always mounts. **Ask every time** in Settings shows the dialog again.

A folder that you drop on the grid or on a collection row opens the dialog with that collection
as the destination. While a mounted row is selected, **Import Images...** is disabled: a mount
cannot hold imported photos.

When a volume without a mount arrives, for example a camera card, a card at the top of the Disk
section shows its name, its mount point, and `DCIM` when the volume holds one. **Import...** and
**Mount...** open the Import Folder dialog for the `DCIM` directory, or the system folder dialog
at the root of the volume.

Each mount has its own directory tree, separate from the trees of the other mounts.
Alcedo Studio records the display name of the volume of each mount when you mount the folder and
at each scan that reaches the volume. An offline mount keeps the last recorded name.

The root row of a mount shows a folder icon with an arrow; the folders below it show the folder
icon. The count of a mounted row counts its present and missing photos, in the folder and in all folders below it, as for every collection. The
Library view of a folder shows its child folders as tiles above its photos; a click on a tile
shows that folder.

## Scans

A scan compares the directory with the collection and applies the changes:

| Change on the disk | Result in the library |
| --- | --- |
| New file | A new photo |
| File renamed or moved inside the mounted directory | The same photo, with its new name and collection; the edits stay |
| Directory renamed | Each photo keeps its edits; the collection takes the new name |
| File copied inside the mounted directory | A second photo |
| File changed in place | The same photo; the thumbnail renders again |
| File deleted | The photo is missing; its edits stay |
| File returns | The photo is present again |

A scan runs at these times:

- when the project opens;
- when Alcedo Studio becomes active after the activation interval or more in the background;
- the watcher delay after the last change in a mounted directory;
- when you choose **Synchronize** in the menu of the mount root.

A scan that changes the library shows a task with its counts, for example `12 new, 3 moved,
1 missing`. At its end, the scan saves the project file once, as an import does. A scan that
changes nothing leaves the project file as it is. When the save fails, the task shows
`The project could not be saved.`; the changes stay in the library, and the next save writes them.

Imports and deletes take turns with the scans. While an import or a delete runs, the scans wait.
A scan that runs when an import or a delete starts completes the batch in work first; its task
shows `Paused`, and the scan runs again when the import or the delete has ended. An import of a
file inside a mounted directory leaves that file to the scan of the mount, so the file stays one
photo, in its mounted collection; a message gives the count of these files.

Alcedo Studio recognizes a moved file by its source fingerprint: a hash of the first and the last
block of the file, with its byte size. A file that another application rewrites at its start or
end and also moves gets a new fingerprint: the scan shows the old photo missing and the file as a
new photo. Use **Locate Photo...** on the missing photo to join them.

On a FAT or exFAT volume, the modification time has a resolution of two seconds. A file that
changes in place within that time and keeps its byte size counts as unchanged until its next
change.

## Offline mounts

When the directory of a mount is unreachable, for example on an unplugged drive, the mount is
offline. Its rows are dimmed, and the root row shows **Offline** in the count position. An
offline mount keeps every photo present: the library stays as it was until the directory
returns.

When the project opens and the directory of a mount is unreachable, Alcedo Studio looks for the
volume of the mount by its volume identity. A drive that has a new drive letter or mount point
is found, and the mount uses the same directory on it.

To bind a mount to a directory that moved, choose **Locate Folder...** in the menu of the offline
mount root and choose the new location. Each photo keeps its edits, and the next scan compares
the new location.

## Missing photos

A photo whose file is gone at the last scan shows a crossed photo icon on its tile. Its rating,
color labels, and Delete mark stay editable, and its edits stay.

- **Locate Photo...** in the menu of a missing photo takes a file you choose as its source. The
  file is inside the mounted directory, is a supported image file, and is free: another photo
  of the mount uses a different file.
  The photo moves to the collection of the file's directory and keeps its edit history.
- **Remove Missing Photos** in the menu of the mount root removes the missing photos of the mount
  from the project, with their edit history and Versions, after you confirm the count. This is
  the only way that a mounted photo leaves the project.

## Photos of the library root

The **Library** row lists the photos that no collection holds. Its count counts those photos and
the photos of the ordinary collections. A mounted photo counts there only when you added it to an
ordinary collection. **All Photos** lists and counts every photo of the project, the mounted
photos included.

## Rules for mounted collections

- **New Collection**, **Rename**, **Delete collection**, and dragging apply to ordinary rows; a
  mounted row follows its directory.
- A file drop on a mounted row or on the grid of a mounted folder shows the "not allowed"
  cursor; the files stay outside the library. Copy them into the directory instead.
- **Add to** lists the library root and the ordinary collections; a mounted folder is not in
  the list. **Add to** and a drag to an ordinary collection link a mounted photo into that
  collection.
- **Delete** at the library root applies to ordinary photos. Use the Delete mark to mark mounted
  photos for review.
- **Show in File Manager** opens the directory of a mounted row.
- **Unmount** in the menu of the mount root keeps every photo as an ordinary photo, with its
  edits, marks, and collection links, and removes the mounted collections.

## Automation

The `alcedo-cli` commands `library.mounts`, `library.mount`, `library.sync`, and
`library.unmount` run the same operations as the Collections panel. The
[alcedo-skills command reference](https://github.com/zidage/alcedo-skills/blob/main/skills/alcedo/references/reference/library.md)
describes them.

## Parameters

| Parameter | Meaning | Value | Source |
| --- | --- | --- | --- |
| Activation interval | Time in the background before an activation of the application scans each mount | 60 s | Plan, section 14 |
| Watcher delay | Time after the last change notification of a mount before its scan starts | 5 s | Plan, section 14 |
| Fingerprint block | Bytes that the fingerprint reads at the start and at the end of a file | 128 KiB each | Plan, section 14 |
| New file batch | New files that one stored batch of a scan imports | 500 | Plan, section 14 |
| Import Folder action | Default action of Import Folder: `ask`, `import`, or `mount` | `ask` | Preference `library/importFolderAction` |
| Volume identity | Identity of the volume that holds the directory: the volume GUID path (Windows), the volume UUID (macOS), or the file system UUID (Linux); `\\server\share` for a network share | Read when the mount is created or located | `ReadVolumeIdentity` |
