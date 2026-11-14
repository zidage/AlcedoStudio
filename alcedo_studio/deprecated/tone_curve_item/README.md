# Tone curve item (archived)

This directory keeps `EditorToneCurveItem`, the scene-graph item that drew the Tone Curve of the
Tone panel before the plot pane, and its geometry test, kept for reference only. The Tone Curve
section now draws on `PlotPane` (`EditorToneCurveSection.qml`, Tone Curve plan TC5,
[#890](https://github.com/zidage/AlcedoStudio/issues/890)).

- The files are not built, installed, or packaged. No CMake target, source list, test
  registration, QML type registration, or QML file refers to this directory.
- The files keep their original paths relative to `alcedo_studio/`, for example
  `alcedo_studio/src/ui/alcedo_main/album_backend/editor_tone_curve_item.cpp` is now
  `alcedo_studio/deprecated/tone_curve_item/src/ui/alcedo_main/album_backend/editor_tone_curve_item.cpp`.
  They moved with `git mv` and no content change, so `git log --follow` shows their history.
- Do not add a `CMakeLists.txt` here.

| Archived path | What it held |
| --- | --- |
| `src/include/ui/alcedo_main/album_backend/editor_tone_curve_item.hpp` | `EditorToneCurveItem` declaration: a 3.5 px ribbon, 11 px to 13 px handle discs, and the pointer gain `kPointerGain = 0.32` |
| `src/ui/alcedo_main/album_backend/editor_tone_curve_item.cpp` | Scene-graph geometry and the pointer input of the item (a click added a point at the pointer, a double click reset the curve) |
| `tests/ui/editor_tone_curve_geometry_test.cpp` | Geometry checks of the item (`EditorToneCurveGeometryTest`) |
