# Advanced Content Analysis (archived)

This directory keeps the C++ and QML sources of Advanced Content Analysis, kept for reference
only. Alcedo Studio 0.3.4 is the last release with the feature. The release after 0.3.4 removes it
([#710](https://github.com/zidage/AlcedoStudio/issues/710)). The Rust sidecar and proto sources of
the feature are archived in `rust/puerh_mind/deprecated/advanced_content_analysis/`.

- The files are not built, formatted, tidied, installed, or packaged. No CMake target, source
  list, include directory, compile definition, install rule, `format` target, or `tidy` target
  refers to this directory.
- They do not compile against the current tree. Nothing in `alcedo_studio/src` or
  `alcedo_studio/tests` includes a file from here.
- Each file keeps its original path relative to `alcedo_studio/`. For example,
  `alcedo_studio/src/app/image_analysis_service.cpp` is now
  `alcedo_studio/deprecated/advanced_content_analysis/src/app/image_analysis_service.cpp`. The
  files moved with `git mv` and no content change, so `git log --follow` shows their history.
- Do not add a `CMakeLists.txt` here.

| Archived path | What it held |
| --- | --- |
| `src/app/image_analysis_service.cpp`, `src/include/app/image_analysis_service.hpp` | `ImageAnalysisService` and the sidecar client adapter of the analysis runs |
| `src/app/image_analysis_encoder.cpp`, `src/include/app/image_analysis_encoder.hpp` | The encoded image rendition sent to a provider |
| `src/include/sidecar_client/image_analysis_client.hpp`, `src/include/sidecar_client/dto/image_analysis.hpp` | The sidecar client interface and data types of `ImageAnalysisService` |
| `src/include/sidecar_client/credential_client.hpp`, `src/include/sidecar_client/runtime_control_client.hpp` | The sidecar client interfaces of `AiRuntimeService` (credential registration, capabilities, task cancel) |
| `src/ui/alcedo_main/album_backend/image_analysis_controller.cpp` and its header | `ImageAnalysisController` (`appModules.imageAnalysis`) |
| `src/ui/alcedo_main/album_backend/image_analysis_sink.cpp` and its header | The writer of analysis results into the project |
| `src/ui/alcedo_main/album_backend/image_analysis_environment.cpp` | The album environment of the analysis runs |
| `src/ui/alcedo_main/album_backend/project_db_write_barrier.cpp` and its header | The export barrier that deferred analysis result writes |
| `src/ui/alcedo_main/qml/AdvancedContentAnalysisDialog.qml` | The analysis dialog |
| `tests/app/image_analysis_*_test.cpp`, `tests/ui/album_backend_db_write_barrier_test.cpp` | The tests of the files above |

The static test `NoCMakeFileNamesArchivedAdvancedContentAnalysisFile`
(`alcedo_studio/tests/ci/advanced_content_analysis_removal_checks.cmake`) fails when a CMake file
under `alcedo_studio/src` or `alcedo_studio/tests` names a file from here.

`AiProviderProfileController`, `IAiCredentialStore`, and the Saved AI Keys settings page stay in
the build for one release, so users can delete the API keys and the Codex login that they saved
for the feature. The next release removes them.
