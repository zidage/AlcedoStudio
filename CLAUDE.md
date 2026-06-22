# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Alcedo Studio** is a RAW photo editor and digital asset management (DAM) system written in C++20. It features CUDA-accelerated (Windows) and Metal-accelerated (macOS) image processing, a DuckDB-backed asset management system ("Sleeve"), and a Qt 6 UI combining QML (album browser) and Qt Widgets (editor).

## Build Commands

### Windows (MSVC + CUDA)

```bash
# Configure (debug)
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"

# Build (debug)
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4

# Configure + build (release)
cmd /c scripts\msvc_env.cmd --preset win_release -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4

# Install + package
cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install
cpack --config build/release/CPackConfig.cmake
```

### macOS (Metal)

```bash
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_main

cmake --preset macos_release
cmake --build --preset macos_release
cmake --install build/macos-release
cd build/macos-release && cpack -G DragNDrop
```

### Formatting & Static Analysis

```bash
cmake --build build/debug --target format   # clang-format (Google style, 100-col)
cmake --build build/debug --target tidy     # clang-tidy with auto-fixes
```

### Tests

```bash
# Run all tests
ctest --test-dir build/debug --output-on-failure

# Run a single test binary (example)
./build/debug/tests/test_exposure_op
```

## Architecture

The codebase follows a strict layered architecture. Higher layers depend only on the layer directly below them.

### Layer 1 — Core Data Structures (`image/`, `include/image/`)
- **Image / ImageBuffer**: Core image representation with embedded metadata
- **ImagePool**: 3-tier LRU cache (metadata → thumbnail → full-res) coordinating memory across the app

### Layer 2 — Image Processing Pipeline (`edit/`)
- **EditPipeline**: Orchestrates ~30 edit operators with CPU / CUDA / Metal execution paths
- **Operators** (`edit/operators/`): One file per operation (exposure, contrast, curves, color temp, LUT, lens calibration, crop/rotate, ACES output, etc.)
- **GPU kernels**: CUDA sources in `edit/operators/GPU_kernels/cuda/`; Metal shaders in `edit/operators/GPU_kernels/metal_shader/` compiled to `.metallib` at build time
- **EditHistory / Version**: Git-like version tree with unlimited undo/redo and branching

### Layer 3 — Application Services (`app/`, `include/app/`)
These façade services are the **only** API surface the UI layer may call. They insulate the UI from all infrastructure changes:
- `ProjectService`, `ImportService`, `ThumbnailService`, `ExportService`
- `EditHistoryMgmtService`, `PipelineMgmtService`
- `SleeveFilterService`, `FSService`, `SleeveManager`

### Layer 4 — Asset Management / "Sleeve" (`sleeve/`, `storage/`)
- **SleeveFS**: DuckDB-backed virtual filesystem with inode-like abstraction
- **SleeveFile / SleeveFolder**: Hierarchy nodes with metadata bindings
- **Storage**: DuckDB ORM layer with mappers and controllers (`storage/`)

### Layer 5 — UI (`ui/`)
- **AlbumBackendLib**: Reusable QML/C++ backend module for the album browser
- **EditViewer**: Real-time editor viewport using Qt RHI (D3D11 / Metal / OpenGL fallback)
- **editor_dialog**: Editor UI panels (tone, color, geometry, versioning, scope/histogram)
- **alcedo_main**: Application entry point (QML + C++ shell)

## Key Technical Notes

- **Qt path is hardcoded** in `CMakeLists.txt` (~line 142) to `D:/misc/Qt/6.9.3/msvc2022_64`. Override with `-DCMAKE_PREFIX_PATH`.
- **Submodules** (`third_party/lensfun`, `third_party/libultrahdr`) must be initialized before configuring: `git submodule update --init --recursive`.
- **Windows packages** are resolved via vcpkg; macOS via Homebrew.
- **CUDA** requires Toolkit 12.8 and compute capability ≥ 6.0. CUDA files have their own compile database entry.
- **C++ standard**: C++20 with AVX/AVX2 SIMD flags.
- **Naming convention** (clang-tidy enforced): private members use a trailing `_` suffix; public/protected members do not.
- **32-bit float pipeline**: All internal image processing operates in 32-bit float; output rendering uses ACES 2.0 with optional CUBE LUT.
- **DuckDB ownership rule**: DuckDB connections must be requested from `DBController` at the operation boundary. Storage controllers must not own long-lived `duckdb_connection` / `ConnectionGuard` instances, including `SemanticStorageController`; this keeps connection lifetime, transaction scope, and cross-controller serialization explicit.
