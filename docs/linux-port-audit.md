# Linux + Intel Port Blocker Audit

Date: 2026-08-19
Source revision: `efdcc9a4` (`main`)
Build policy: Linux verification uses Ninja `-j4`
Scope: Linux x86_64 CPU fallback, Intel OpenCL, and Qt Quick presentation.
Behavior boundary: shared file enumeration, supported-format policy, and the
existing RAW-only test-host behavior are intentionally unchanged; this audit
only addresses Linux behavior and Linux platform plumbing.

## Current status

```text
CMake configure: PASS (CPU and OpenCL presets; WebGPU explicitly OFF)
CPU application build: PASS (full default target with all Linux app/RHI entry points)
OpenCL application/RHI build: PASS (full default target with OpenCL compute and RHI)
CPU UI startup: PASS in offscreen smoke test (event loop reached; 12 s timeout)
CPU-only RHI host-upload composition: NOT VERIFIED (offscreen Qt did not run a scene-graph frame)
OpenCL RHI host-upload composition: PASS (real xcb desktop run with Intel Arc B390)
Linux default OpenCL→CPU fallback: PASS (OpenCL build survives zero-platform runtime)
CPU image writer: PASS (ImageWriterTest 13/13)
CPU sample-image/export run: PARTIAL (a real DNG was decoded/displayed; export was not shown)
Export service tests: BLOCKED (both cases resolve to 133-byte Git LFS pointers)
OpenCL runtime/device: PASS on the user's host (clinfo lists Intel Arc B390)
Direct OpenCL/GL sharing: NOT USED (the verified path uses device-to-host readback)
Native install and Linux CPack ZIP: PASS
WebGPU: DEFERRED; no matching implementation is enabled by the Linux path
```

The full builds produced `alcedo_main`, `alcedo_studio_test_host`, `EditorRhiHarness`,
the update tools, and the Rust `alcedo_mind` sidecar. `ldd` on the build-tree
executables found no missing shared libraries. The local verification used
temporary DuckDB/Lensfun prefixes
and a partial Khronos/ANGLE header tree only for the OpenCL compile probe; that
header tree is not an Intel runtime or device validation. `cmake --install`
completed successfully, and CPack generated
`/tmp/alcedo-linux-package-final/alcedo-0.2.9-Linux-x86_64.zip`. The current
Linux ZIP follows the repository's existing install rules, which also install
some third-party development payload; it is a native bring-up package, not yet
an AppImage, Flatpak, or distribution-native Arch package.
It is not self-contained: the installed executable still expects
system/packaging-provided Qt, OpenCV, OpenImageIO, DuckDB, Lensfun, and OpenCL
loader libraries. In this container, `ldd` on the install-prefix copy
specifically reports the temporary `libduckdb.so` and `liblensfun.so.1` prefixes
as unresolved; the build-tree binary resolves them through its configured
development-prefix RUNPATH.

## Audit classification

### A. CPU path now works on Linux

- CUDA and Metal are disabled for Linux builds; the CPU decoder/edit pipeline is
  independent of both APIs.
- The Linux CPU backend is selected only by the Linux platform fallback. The
  existing Windows/macOS default backend selection remains unchanged.
- When Linux is compiled with OpenCL but the runtime has no usable device or
  sharing context, the normal default/settings path falls back to CPU host
  upload. An explicit `--editor-backend=opencl` remains a diagnostic error.
- On the user's real X11 desktop, explicit `--editor-backend=opencl` selected the
  Intel Arc B390 device and completed OpenCL compute plus Qt Quick presentation.
- Qt Quick CPU presentation is a real host-upload path: the CPU pipeline copies
  the final `CV_32FC4` output into `DirectFrameSink::SubmitHostFrame`, and the
  scene-graph renderer uploads it to a QRhi `RGBA32F` texture.
- The CPU editor reached the Qt/QML event loop under `QT_QPA_PLATFORM=offscreen`
  without a startup crash. QML emitted existing binding warnings.

### B. CMake and dependency blockers fixed

- Linux OpenCL detection is no longer guarded by `WIN32`; it can use the system
  OpenCL loader and headers.
- Linux now has `linux_cpu_debug` and `linux_opencl_debug` configure/build
  presets, both fixed at four build jobs.
- Qt `ShaderTools` and `GuiPrivate` are found for desktop RHI targets on Linux.
- xxHash and LittleCMS2 have Linux package-config/direct-library fallbacks.
- Linux can use system Lensfun when the stale bundled tree is absent, and the
  lens calibration operator handles Lensfun 0.3.4's current API.
- libultrahdr accepts the shared system JPEG library on Linux.
- CMake 4.4 policy compatibility, conditional Metal-cpp setup, offline ed25519,
  and missing standard-header includes were fixed for the CPU/OpenCL builds.

### C. OpenCL and display portability work completed

- OpenCL compute sources and the RHI targets compile on Linux when
  `ALCEDO_ENABLE_OPENCL=ON` and the loader/header paths are supplied.
- OpenCL/GL context properties now support WGL on Windows, GLX on X11, and EGL
  on Linux EGL sessions.
- Qt startup resolves the native GLX/EGL handles after creating a hidden sharing
  context, then initializes the OpenCL runtime with those properties.
- Linux OpenCL selects Qt Quick OpenGL. CPU selection also selects Qt Quick
  OpenGL but does not require OpenCL or native interop.
- Host-frame queueing and QRhi upload cover CPU presentation. The OpenCL fused
  pipeline now uses the same queue for an actual device-to-host readback when
  GL sharing is unavailable.
- No shared decoder, file-enumeration, supported-format, or RAW scanning rule
  was changed as part of the Linux port.

### D. Verified commands and results

CPU configure/build:

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_PREFIX_PATH=/tmp/alcedo-duckdb-root/usr;/tmp/alcedo-lensfun-root/usr' \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DFETCHCONTENT_SOURCE_DIR_ALCEDO_ED25519_SOURCE=/tmp/alcedo-ed25519-root \
  -DALCEDO_ENABLE_CUDA=OFF -DALCEDO_ENABLE_METAL=OFF \
  -DALCEDO_ENABLE_OPENCL=OFF -DALCEDO_BUILD_TESTS=OFF
cmake --build build/linux --target alcedo_main EditorRhiHarness -j4
```

OpenCL configure/build:

```bash
cmake -S . -B build/linux-opencl -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_PREFIX_PATH=/tmp/alcedo-duckdb-root/usr;/tmp/alcedo-lensfun-root/usr' \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DFETCHCONTENT_SOURCE_DIR_ALCEDO_ED25519_SOURCE=/tmp/alcedo-ed25519-root \
  -DALCEDO_ENABLE_CUDA=OFF -DALCEDO_ENABLE_METAL=OFF \
  -DALCEDO_ENABLE_OPENCL=ON -DALCEDO_BUILD_TESTS=OFF \
  -DALCEDO_BUILD_ARIA2C_FETCH=OFF \
  -DOpenCL_INCLUDE_DIR=/home/steveyang137/Documents/ChatGPT/Alcdeo\ Studio/AlcedoStudio/alcedo_studio/third_party/dawn/third_party/angle/include \
  -DOpenCL_LIBRARY=/usr/lib/libOpenCL.so
cmake --build build/linux-opencl --target EditorRhiHarness alcedo_main -j4
```

For the Codex execution container, the OpenCL include path was a local
compile-only probe. Its Intel ICD file points to
`/usr/lib/intel-opencl/libigdrcl.so`, but `clinfo` reported zero platforms and
`/dev/dri` was absent. The CPU smoke test reached the event loop and was
stopped by the 12-second timeout. The OpenCL offscreen smoke test with an
explicit OpenCL request stopped at the expected native-context/device
requirement: `failed to create hidden OpenGL context for OpenCL sharing`; the
subsequent plain-OpenCL fallback also found no platform. The same OpenCL build
without an explicit backend request survived the failure and logged
`source=runtime-cpu-fallback`, then reached the Qt/QML event loop.

The user then ran the same OpenCL build on the real X11 host. `clinfo -l`
reported `Intel(R) Arc(TM) B390 GPU`, and the application logged
`editor.backend=opencl`, `pipeline.accelerator backend=opencl`, and repeated
`[OpenCL Pipeline]` frames with `present=host_upload`. The log also contains
`RENDER_E2E` records with non-zero scene-graph import/presentation times, so
Intel OpenCL compute and the Linux Qt Quick host-upload display path are
validated on the actual desktop. Direct OpenCL/GL sharing was not used; the
application correctly used the readback path instead.

The CPU `EditorRhiHarness --editor-backend=cpu --case=direct-presentation` was
also attempted. Its host frame was accepted by the production sink, but the
`offscreen` platform did not advance a QRhi scene-graph render pass, so the
composition counter stayed at zero. This is an environment limitation; a real
X11/Wayland session is still required to validate the final Qt Quick texture
upload and on-screen composition.

The focused `PipelineFrameSinkTest` build passed. Its filtered run had 28
passing tests and three CUDA skips; the excluded full run also reported one
allocator-address-reuse assertion failure in
`ReattachingFrameSinkPreservesMergedStage`; excluding that brittle assertion
left 28 passing/three skipped tests. No image fixture was processed because the
repository's RAW files are Git LFS pointer files and this environment has no
Git LFS payload checkout.

Additional focused checks passed:

```text
RawDecodeOpParamsTest: 3/3 passed
AiCredentialStoreTest: 4/4 passed (Linux in-memory fallback)
PipelineFrameSinkTest filtered run: 28 passed, 3 CUDA skips
ImageWriterTest: 13/13 passed
WorkspaceShellTest target: built successfully with `-j4`; its selected offscreen
frame-binding case was blocked in the shared `CreateTestProject()` fixture before
the frame assertion (temporary project service did not become ready).
```

`ExportServiceTest` ran but both cases failed before exercising the Linux writer
with a real image. `CollectSupportedBatchImportImages()` selected files under
`tests/resources/sample_images/ci_rawfiles`; each checked-in file is a 133-byte
Git LFS pointer beginning with `version https://git-lfs.github.com/spec/v1`,
not the RAW payload. This is a missing-fixture checkout condition. The Linux
port deliberately does not change that existing collection rule or add a new
regular-image scanning/decoding policy.

The native install check was:

```bash
cmake --install build/linux --prefix /tmp/alcedo-linux-install-final
cpack --config build/linux/CPackConfig.cmake -B /tmp/alcedo-linux-package-final
```

Both commands completed successfully. The installed tree contains the native
Linux executable and its existing assets. The build-tree dependency check
reported no missing shared libraries for the platform-specific pattern; the
install-prefix check separately exposed the temporary DuckDB/Lensfun RUNPATH
dependency caveat described above.

The post-audit full-build checks were:

```bash
cmake --build build/linux -j4
cmake --build build/linux-opencl -j4
```

Both completed successfully. The OpenCL build emitted warnings from the bundled
gRPC/Abseil/protobuf sources, but no Linux application or OpenCL target failed.

### E. Remaining blockers

- The actual Intel OpenCL device and X11 host-upload presentation are now
  validated on the user's host. Direct OpenCL/GL sharing remains intentionally
  unused in that run; the readback/host-upload path is the verified Linux path.
- Fetch Git LFS fixture payloads or provide a local JPEG/PNG/RAW sample, then
  verify CPU decode → edit → display → export and compare the OpenCL path. This
  is verification input, not a request to change the existing file-enumeration
  or supported-format behavior.
- A controlled CPU-versus-Intel-OpenCL timing comparison remains follow-up work;
  the provided log proves functionality, not a benchmark.
- Linux credential bring-up is covered by the existing in-memory fallback and
  its focused test; libsecret/Secret Service integration remains follow-up
  work.
- Linux update support remains explicitly `unsupported` for bring-up, as
  allowed by the port instructions.
- The native CPack ZIP is verified. Arch/AppImage/Flatpak packaging and a Linux
  CI job remain follow-up work.

## Next step

The Linux Intel OpenCL bring-up is complete on the user's X11 host. Remaining
follow-up is limited to export validation with a real fixture, controlled
performance comparison, and optional direct OpenCL/GL sharing experiments.
