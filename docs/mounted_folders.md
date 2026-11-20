# Mounted folders

A mounted folder is a collection that follows a directory on a disk. Its subdirectories are its
collections, and its supported image files are its photos. Alcedo Studio reads the directory
and keeps the collection equal to it. The direction is always from the disk to the library:
Alcedo Studio only reads the directory, and the files stay as they are.

The project database is the only store of the edit history, the Versions, the ratings, the
color labels, the Delete mark, and the collection links of each photo.

The product plan is the [Mounted Folder Plan](roadmap/alcedo_studio/storage/mounted_folder_plan.md).
The parameters are in the table at the end.

## Mount a folder

1. In the Collections panel, right-click the library root or a collection and choose
   **Mount Folder...**.
2. Choose the directory in the system folder dialog.
3. A collection with the name of the directory appears below the row. Its first scan runs as a
   background task, and the photos appear while the scan imports them.

Each mount has its own directory tree, separate from the trees of the other mounts. The parent
row of a new mount is an ordinary collection or the library root.

Each mounted row shows a link icon after its name. The count of a mounted row counts its present
and missing photos.

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
1 missing`.

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

## Rules for mounted collections

- **New Collection**, **Rename**, **Delete collection**, and dragging apply to ordinary rows; a
  mounted row follows its directory.
- A file drop on a mounted row or on the grid of a mounted folder shows the "not allowed"
  cursor; the files stay outside the library. Copy them into the directory instead.
- **Add to** and a drag to an ordinary collection link a mounted photo into that collection.
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
| Volume identity | Identity of the volume that holds the directory: the volume GUID path (Windows), the volume UUID (macOS), or the file system UUID (Linux); `\\server\share` for a network share | Read when the mount is created or located | `ReadVolumeIdentity` |
