# Thumbnail loading spinner (archived)

This directory keeps `ThumbnailLoadingSpinner.qml`, the loading mark of a photo before the
staggered grid loader, kept for reference only. Library cards, justified tiles, and Editor
filmstrip cards now show `StaggeredGridMark.qml` while a thumbnail loads (DESIGN.md "Library
loading", [#834](https://github.com/zidage/AlcedoStudio/issues/834)).

- The file is not built, installed, or packaged. No CMake QML file list, resource, or QML file
  refers to this directory.
- The file keeps its original path relative to `alcedo_studio/`:
  `alcedo_studio/src/ui/alcedo_main/qml/ThumbnailLoadingSpinner.qml` is now
  `alcedo_studio/deprecated/thumbnail_loading_spinner/src/ui/alcedo_main/qml/ThumbnailLoadingSpinner.qml`.
  It moved with `git mv` and no content change, so `git log --follow` shows its history.
- Do not add a `CMakeLists.txt` here.

| Archived path | What it held |
| --- | --- |
| `src/ui/alcedo_main/qml/ThumbnailLoadingSpinner.qml` | A 270 degree accent arc that a GUI-thread `RotationAnimation` turned while a thumbnail loaded |
