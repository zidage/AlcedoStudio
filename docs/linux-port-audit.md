# Linux OpenCL port verification

This update applies the [review of PR #87](https://github.com/zidage/AlcedoStudio/pull/87#pullrequestreview-4967460386)
to the current GPU graph renderer. The upstream base is `63b2963f` (updated on 2026-11-15).

## Apple standard-library compatibility (2026-11-18)

The [macOS arm64 CI build](https://github.com/zidage/AlcedoStudio/actions/runs/43701960570/job/134231508167)
failed because Xcode 16.4's standard library has no `std::jthread` declaration.
The project module now owns a `std::thread` and joins it explicitly during destruction,
before its fields and QObject are destroyed. A failed preparation attempt also joins the
previous worker before starting another attempt. Kernel preparation did not use a stop token,
so the worker still runs to completion before its owner is destroyed.

Local Clang/libc++ checks with `jthread` declarations disabled reproduce the old header's
failure and compile both the updated header and project-module implementation. These checks
do not replace a build with the CI runner's Apple SDK.

WSL E2E validation remains incomplete. The recorded GPU presentation checks below ran on
native Linux and Wayland; they are not WSL interoperation results.

## Processing and presentation

- Linux selects the OpenCL GPU backend. There is no CPU editor backend or CPU preset.
- OpenCL CPU devices are rejected, including a CPU selected by name. Missing GPU support
  produces an initialization error; the application does not start a CPU image pipeline.
- Startup tries OpenCL/OpenGL sharing with the current GLX or EGL context. When sharing is
  unavailable, OpenCL still processes the image on the GPU.
- The viewport then supplies a host RGBA32F target. The GPU presenter reads the completed
  texture into this target, and Qt Quick uploads it on the scene-graph thread.
- Image identity, request id, frame role, ROI geometry, and display encoding travel with the
  completed frame. Frames from a previous image session are discarded.
- Mask coverage uses its own presentation channel. It can also transfer the completed GPU
  texture through host memory, with the same image-session and clear rules.
- The scope analyzer continues to receive the final GPU image and its completion event.
- Frame transfer has no CLI option or saved user preference.
- Linux destroys the hidden sharing context during Qt application teardown. Kernel preparation
  belongs to the project module and finishes before that module and the OpenCL library are destroyed.

## Dependencies

Lensfun uses the current upstream git submodule and modern C API. The old system Lensfun
search and compatibility implementation were removed. The current Lensfun processing code
has no Linux-specific API variant.

Linux dependency discovery accepts the CMake targets, pkg-config data, or development
library/header pairs supplied by distributions for xxHash and LittleCMS2. The libjpeg
library is resolved before libultrahdr narrows its library search. Missing Apple-only
metal-cpp sources do not block a Linux build.

## Recorded checks

Validation uses GCC 16.2.1, the installed Qt 6 and OpenCV 5 development packages, and an
Intel Arc B390 GPU. All CMake build commands use four parallel jobs.

- The full `alcedo_main` target and all focused test targets built successfully.
- Native Wayland/EGL application startup selected the OpenCL GPU backend with GL sharing.
  Two isolated launches were closed through the local automation API while kernel preparation
  was active; both completed normal shutdown with exit status 0.
- `--verify-qml-imports`: passed for the bundled QuickQanava QML module.
- Application startup with `--editor-backend=cpu` exits with status 1 and an invalid-backend error.
  Startup with an empty ICD directory exits with status 1 and reports that no OpenCL platform exists.
- `MaskCoveragePresentationTest`: 7 passed in a real X11/OpenGL session on the B390. The tests
  compare coverage texels within one 8-bit code value and check resource lifetime, image changes,
  and clear ordering. The same seven tests also pass on native Wayland/EGL.
- `MaskCoverageHostUploadTest`: 7 passed on native Wayland/EGL with a GPU context created without
  GL sharing. This separately verifies the real GPU readback and QRhi texture upload path.
- `EditorViewportSuspensionTest`: 7 passed. This includes backend selection, host pixel and
  metadata lifetime, old-image rejection, minimized viewport behavior, and coverage clear order.
- Existing backend parsing, fixture data, and presentation resource checks: 10 passed.
- `OpenClGpuSelectionTest`: 3 passed. A CPU-only ICD, an explicitly preferred CPU device, and
  a missing ICD all fail before an OpenCL processing context is created.
- Four selected `OpenClRendererFixture` tests passed on the B390. They compare host-presentation
  pixels with GPU output within `1e-5`, require zero readback bytes on direct submission, retain
  the GPU scope image, and reject an incompatible presentation target.

The full test suite was not run. Windows and macOS builds were not run in this Linux environment.

## Upstream merge validation (2026-11-15)

Merged upstream `63b2963f` and resolved the viewport scheduling conflict. Frame publication
uses the upstream GUI-thread update path. Linux host uploads still request an update after
publication; the obsolete render-thread update connection stays removed. The upstream display
color transform integration is retained.

- Full `alcedo_main` and the focused test targets built with four jobs.
- 48 focused tests passed: viewport suspension and host-frame state (7), GPU-only device
  selection (3), backend/lease fixtures (10), display color transforms (10), native Wayland
  shared coverage (7), native Wayland host-upload coverage (7), and GPU photo presentation (4).
- Two isolated Wayland launches closed normally during OpenCL kernel preparation, with exit
  status 0.
- The full test suite and Windows/macOS builds were not run for this merge.
