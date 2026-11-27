# File management fuzz

`file_fuzz.py` runs random operation sequences on the file management layer of Alcedo Studio: the
sleeve folder tree, the photos of each folder, and above all the mounted folders (#1188, #1192).
It drives a headless session through `alcedo-cli`, changes a disk directory between the steps,
and compares each answer and each read with a reference model. A seed selects the run; a failed
run shrinks to a short operation log that replays on its own.

The scheduled C++ CI runs it on macOS (#1193). Pull requests and pushes do not run it. A defect
that it finds gets an issue, and its fix adds an ordinary regression test in the suite of the
owner.

## Commands

From the repository root, with a Release build of `alcedo_main` and `alcedo_cli` and a build of
`StorageInvariantCheck` (a test target of `tests/app`):

```
python scripts/file_fuzz/file_fuzz.py run --seed 7 --steps 300 --mode watcher-off --out build/tmp/file_fuzz/run7
python scripts/file_fuzz/file_fuzz.py replay build/tmp/file_fuzz/run7/ops.jsonl
python scripts/file_fuzz/file_fuzz.py shrink build/tmp/file_fuzz/run7/ops.jsonl --out build/tmp/file_fuzz/run7/shrink
python3 -m unittest discover -s scripts/file_fuzz/tests
```

| Command | What it does |
| --- | --- |
| `run` | Starts a headless session with a new project, generates each operation from the seed and the model, runs it, and checks the step. It writes `ops.jsonl` (a header line, then one operation per line), `steps.jsonl` (the answers of each step), `summary.json`, `failure.json` after a failure, the host logs in `logs/`, the disk directory in `disk/`, and the project in `project/`. |
| `replay FILE` | Runs the operations of a log in a new session, with the options of its header. |
| `shrink FILE` | Removes operations that the failure does not need (delta debugging, one replay in a new session for each try) and writes `shrunk.jsonl`: the shortest log that still fails the same check with the same operation kind. `run --shrink` shrinks after a failure. |

Exit code 0: every step passed. 1: a step failed. 2: a usage or setup error (no build, no log).

## Modes

| Mode | Directory watcher | Check of a scan |
| --- | --- | --- |
| `watcher-off` | `--mount-watcher off`: a disk change starts no scan | Each `library.sync` result (`new`, `moved`, `changed`, `missing`, `returned`, `not_imported`) equals the prediction of the model. |
| `watcher-on` | On | The driver makes one to four disk changes, waits one second, and calls `library.mounts.wait_idle`. The final state equals the model with one scan of each changed mount, however many scans the watcher ran. |
| `concurrent` | Off | Two connections work on disjoint subtrees: one creates, renames, and imports into ordinary folders below its own top folder; the other adds files to one mounted directory and syncs it. The seed gives the operations; the interleaving is free. The model applies the answers of each side in order after the round, which is exact for each interleaving. |

## The model

`fuzz_model.py` holds the library folder tree, the photos with their folder links, the mounts,
and the scan prediction. It names each object by a handle that the operation log uses
(`fg3`, `mg1`, `f12.2`, `p40.1`): the generator writes `<kind>g<n>`, and an operation that
creates objects names them `<kind><operation id>.<n>`. A shortened log keeps the handles of the
operations that it keeps. The run binds a handle to its database id when the session answers.

The rules of the model come from the source:

| Rule | Source |
| --- | --- |
| A folder name is trimmed like `QString::trimmed`; empty, `.`, `..`, `/`, `\`, and control characters are invalid; a name compares exactly with the names of the siblings (case and Unicode form count). | `AlbumBrowseService::IsValidFolderName`, `FolderController` |
| A new mount root takes the directory name, with `@` added while the library root has the name. | `FileSystem::Create` |
| `library.list` of the root lists every photo (All Photos); of a folder, the photos linked into it. | `AlbumScope` |
| Each import of a file makes a new photo; a named file becomes a photo when its bytes are an image. A file inside a mounted directory stays with the scan of its mount. | `ImportServiceImpl`, `ImportExportHandler::StartImportResolvedPaths` |
| A listing holds the regular files with a supported extension, with each name in NFC; it skips names that start with a period and does not follow links or junctions. | `ListMountDirectory` |
| The fingerprint is a hash of the 8-byte size, the first 128 KiB, and the last 128 KiB (all bytes once for a file of 256 KiB or less). The model hashes the same bytes with SHA-256, so two files have one model key exactly when they have one fingerprint. | `ComputeSourceFileFingerprint` |
| A scan joins the listing with the records by relative path; a different size or time is a change in place; a lost record and a new path with one fingerprint make a move (the same file name first, then the sorted order); an ordinary photo at a new path is adopted; a record below an unreadable directory stays. A directory gets a folder when it holds a listed file. | `BuildMountScanChangeList`, `MountScanRun` |

Where an operation has several applicable refusals, the model accepts each of them, because the
order of the checks differs between the commands.

## Checks

1. Model equality after each step: `library.folders` (paths, ids, parents), `library.list` of the
   root and of each folder (element ids and `total`), and `library.mounts` (state, path, present
   and missing ids, and the volume name of a disk image mount).
2. A refused operation changes nothing: the reads before and after it are equal.
3. Each answer is in the set that the model accepts: -32602 with `data.pointer`, -32002 with its
   `data.error`, or Busy where the operation allows it. -32603, -32007, a timeout, and a crash of
   the host fail the run.
4. `save_check` (every `--save-every` steps and at the end): `project.save`, then
   `StorageInvariantCheck` on the database of the saved project file.
5. `reopen` (every `--reopen-every` steps): `project.close`, `project.open`, the scan of each
   mount at the open, then check 1.

## Operation kinds

The weight table `WEIGHTS` in `fuzz_ops.py` holds every operation kind with its weight and its
area of the test list of #1188. `--areas M2,M3` keeps the kinds of those areas (and `sync`), and
`--weight KIND=W` sets one weight, so a run can stress one area.

| Area | Kinds |
| --- | --- |
| A. Folder names | `folder_create`, `folder_rename` with names from `NAME_SAMPLES` and variants of the existing names |
| B. Folder structure | `folder_move`, `folder_delete`, `folder_bad_id`, `deep_tree` (200 levels), `wide_folder` (2,000 children) |
| C. Photo membership | `import`, `import_bad_files`, `link`, `delete_album`, `delete_project`, `delete_unknown_ids` |
| M1. Mount creation and refusals | `mount`, `mount_refused` (nested, a link to a mount, another case, unreachable, a file), `mounted_structure_edit` |
| M2. Disk changes | `disk_add`, `disk_delete`, `disk_rename`, `disk_case_rename`, `disk_move`, `disk_rename_dir`, `disk_move_dir`, `disk_rewrite`, `disk_file_to_dir`, `disk_dir_to_file`, `disk_unicode_name`, `disk_unlisted`, `disk_unreadable_dir`, `disk_readable_dir`, `disk_lock`, `disk_unlock` (Windows), `disk_many` (499 to 1,001 files), `sync` |
| M3. Fingerprint pairs | `disk_copy`, `disk_swap_pair`, `disk_move_and_change`, `disk_middle_change`, `disk_touch`, `disk_hardlink` |
| M4. Missing photos | `locate_photo`, `remove_missing`, `delete_mounted` |
| M5. Root and volume | `locate_root`, `root_away`, `root_back`, and on macOS `volume_create`, `volume_detach`, `volume_attach` (also at another mount point, then an open), `volume_rename` with `hdiutil` disk images |
| M6. Interleaving | `scan_interleave` (an import, a delete, or a save during a scan), `scan_cancel`, `scan_twice` |
| M7. Unmount | `unmount`, `remount` |
| D. Persistence | `save_check`, `reopen`, `kill_reopen` (the host is killed after a save, also during a scan or an import) |
| F. Parameters | `param_types`: a wrong type, an extra key, or a missing key gives -32602 with the parameter path |
| G. Concurrency | `concurrent_round` (mode `concurrent`) |

The test images are copies of the smallest CI RAW fixture and JPEG files made from
`tests/resources/raster/untagged_8bit.jpg`: seeded comment segments after the start marker and
seeded bytes after the end marker give each file its size and its first and last blocks. A run
needs no other data.

## Behavior that the model records

The first runs recorded these behaviors; the model follows them:

- A change of only the modification time of a mounted file counts as `changed` (a change in
  place), and the photo keeps its element and fingerprint. Test 34 of #1188 expected
  "unchanged"; `BuildMountScanChangeList` defines a different time as a change in place.
- An empty directory in a mount gets no folder.
- A new mount of a directory whose name the library root has gets the name with `@`.

## Known issues

`KNOWN_ISSUES` in `fuzz_ops.py` lists the open issues that a run found. The generator writes no
operation that an entry covers, so each run can find the next defect; `--include-known-issues`
writes them again. The header of `ops.jsonl` lists the entries of the run. Remove an entry with
the fix of its issue.

| Issue | Operations that the generator leaves out |
| --- | --- |
| #1214 | Folder names with `:` on Windows |
| #1215 | Mounted file names that are not in NFC on Windows |
| #1218 | Hard links on Windows (NTFS keeps an old size and time in the entry of the other link) |

Two answers are accepted until their issue is fixed: a `library.sync` during an import of the
other connection answers -32002 without `data.error` (#1216), and an import of only mounted files
answers -32002 without `data.error` (#1217).

## Parameters

| Parameter | Meaning | Default |
| --- | --- | --- |
| `--seed` | Seed of the run: the operations and the contents of the files | A random 64-bit value |
| `--steps` | Operations of a run | 200 |
| `--mode` | `watcher-off`, `watcher-on`, or `concurrent` | `watcher-off` |
| `--out` | Output directory | `build/tmp/file_fuzz/run_<seed>` |
| `--cli`, `--build-dir` | The `alcedo-cli` executable, or the build tree that holds it and `alcedo_main` | The first of `build/release`, `build/macos-arm-metal-ci`, `build/macos-release`, `build/debug`, `build/macos-debug` |
| `--invariant-check` | The `StorageInvariantCheck` executable; `none` saves and reads the database without the check | The test target in the build trees |
| `--areas` | Areas of the operation kinds | All |
| `--weight KIND=W` | Weight of one kind | `WEIGHTS` |
| `--check-every` | Steps between two reads of check 1 (refusals, syncs, mounts, and opens are always checked) | 1 |
| `--save-every` | Steps between two `save_check` operations | 25 |
| `--reopen-every` | Steps between two `reopen` operations | 60 |
| `--clean-file-limit` | `ALCEDO_TEST_SLEEVE_CLEAN_FILE_LIMIT` of the host, so the eviction of the node cache runs at each step (test 57) | 2 for one seed in four |
| `--scale` | Factor of the large cases (tests 11 and 29) | 1.0 |
| `--time-limit` | Seconds after which no new step starts | None |
| `--include-known-issues` | Also write the operations of `KNOWN_ISSUES` | Off |
| `--inject-failure KIND` | Test only: the first operation of the kind fails with the check `injected` | Off |
| `--shrink`, `--shrink-time-limit` | Shrink the log after a failure, for at most this many seconds | Off, 900 |
| `shrink --time-limit` | Seconds of the shrink command | 1800 |
| Watcher delay | Wait before `wait_idle` in `watcher-on` mode, so the watcher has reported the change | 1 s |
| Volume arrival delay | Wait after an `hdiutil` attach or detach | 3 s |
