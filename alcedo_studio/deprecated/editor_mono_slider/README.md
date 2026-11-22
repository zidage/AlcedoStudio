# Editor mono slider (archived)

This directory keeps `EditorMonoSlider.qml`, the relative-drag slider of the White Balance rows
and the Mask fields, kept for reference only. Every numeric slider now uses `AlcedoSlider.qml`:
`EditorDragSlider.qml` binds it to owners that take a drag as begin, update, and finish calls
([#922](https://github.com/zidage/AlcedoStudio/issues/922)).

- The file is not built, installed, or packaged. No CMake QML file list, resource, or QML file
  refers to this directory.
- The file keeps its original path relative to `alcedo_studio/`:
  `alcedo_studio/src/ui/alcedo_main/qml/EditorMonoSlider.qml` is now
  `alcedo_studio/deprecated/editor_mono_slider/src/ui/alcedo_main/qml/EditorMonoSlider.qml`.
  It moved with `git mv` and no content change, so `git log --follow` shows its history.
- Do not add a `CMakeLists.txt` here.

| Archived path | What it held |
| --- | --- |
| `src/ui/alcedo_main/qml/EditorMonoSlider.qml` | A Controls `Slider` with a 6 px track, a round 22 px handle, and a `MouseArea` that dragged the handle relative to the press point and reset on a double-click |
