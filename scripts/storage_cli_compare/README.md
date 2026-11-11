# alcedo-cli read comparison of two builds

`compare_storage_cli_reads.py` checks that a storage change keeps the answers of the library
reads (#807). It runs the same alcedo-cli read commands with the Release build of a baseline
commit and with the Release build of the working tree, on fresh copies of the same projects, and
lists each command whose JSON answer differs. Both builds produce the answers at run time.

This comparison is a separate check: it builds two trees, so it runs on request and stays out of
ctest.

## Run

On Windows, from the repository root, with a working tree whose tracked files are committed:

```
python scripts/storage_cli_compare/compare_storage_cli_reads.py
```

The default run takes the three steps below. `--steps run` repeats only the comparison with the
installed builds and the written projects.

| Step | What it does |
| --- | --- |
| `build` | Builds `alcedo_main` and `alcedo_cli` (`win_release`) of the working tree and installs them to `<out>/install/current`. Builds `StorageCliCompareProjectWriter` (`win_debug`). Checks out the baseline commit in the same working tree, applies `baseline_library_stats.patch`, builds and installs the same targets to `<out>/install/baseline`, and checks out the starting branch again. |
| `projects` | Writes `imported.alcd` (folders of the sample RAW folder, imported with the baseline build), `compat.alcd` (a copy of `tests/resources/compat/raw_project_abdb000c8.alcd`), and `marked.alcd` (written by `StorageCliCompareProjectWriter`). |
| `run` | Generates the command list from the reads of both builds, runs it on a fresh copy of each project with each build, and writes `<out>/report.md` and `<out>/differences.json`. |

Exit code 0 means every answer is the same, 1 means at least one answer differs, and 2 means a
step failed.

## Baseline patch

`library.stats` and the bucket parameters of `library.list` (`camera`, `lens`, `capture_date`,
`rating`, `import_date`, `label`, `time_zone`) arrived with this check. The baseline commit gets
them from `baseline_library_stats.patch`: the same command code, applied to
`library_commands.cpp` of that commit. The `label` parameter uses the label filter of the
Library statistics bar of each build: the label aliases (`SemanticLabelAliases`) in the
baseline, and the semantic label bucket in the current build. Each build reads through its own
storage code, so the comparison covers the query code of both builds.

To compare against another baseline commit, regenerate the patch for that commit.

## Projects

| Project | Content |
| --- | --- |
| `imported` | The photos of the `--raw-folders` folders of `alcedo_studio/tests/resources/sample_images/raw/`, many camera models and lenses. |
| `compat` | The project packed by build abdb000c8, as `ProjectCompatibilityTest` reads it. |
| `marked` | 72 synthetic files (5 deleted again), with ratings, color labels, Delete marks, semantic labels of an active model, three mask assets, one to three commits on about a third of the files, and three albums (one nested). File names hold `_`, `%`, quotes, and Chinese text; camera and lens text comes in two spellings and empty; capture dates are valid, date-only, and invalid; ISO is NULL on some files; import times fall on the New York daylight-saving changes and the Shanghai midnight; semantic labels come in mixed case, with spaces, as Chinese label text, and outside the label taxonomy. A fixed random seed makes the project the same on each run. |

## Commands

The `run` step first reads `library.folders`, `library.stats` (each folder, each time zone), the
full `library.list` of each folder, and the total of each search text with both builds. From the
union of these reads it generates:

- `library.stats` and `library.list` for each bucket of each dimension, for the empty values, and
  for upper-case, lower-case, and space variants of the text labels;
- the import day buckets in `America/New_York`, `Asia/Shanghai`, and `UTC`;
- each color label code and the Delete mark, alone and together, and two buckets together;
- each sort in both directions: the full list and pages of 7 photos;
- `library.info` of every photo, with an unknown id and a repeated id;
- `search.query` for texts with quotes, `%`, `_`, Chinese text, camera, lens, and label text, and
  parts of the file names: every page of the text search, an offset page, and the `auto` mode;
- parameters that the commands reject (an import day without a time zone, an invalid day, an
  unknown time zone, an unknown code, a page above the limit).

## Parameters

| Parameter | Meaning | Default |
| --- | --- | --- |
| `--out` | Folder of the installs, projects, runs, and report. | `build/tmp/storage_cli_compare` |
| `--steps` | Comma-separated steps to take, in the order `build`, `projects`, `run`. | `build,projects,run` |
| `--baseline` | The baseline commit. | `d0f89c738^1` (the commit before the storage refactor of #785) |
| `--projects` | Comma-separated projects of the `run` step. | `imported,compat,marked` |
| `--raw-folders` | Comma-separated folders of the sample RAW folder for `imported.alcd`. | 15 folders, about 270 photos |
| `--jobs` | Parallel compile jobs of the builds. | 6 |
