# Why Another `third_party` Folder

Because lensfun has not updated their Release for a long time, and for the newest version, we have to build it from source every time, so we decided to include the source code of lensfun in our project. We will update the source code of lensfun when there is a new release, and we will also update the source code of lensfun if there are some critical bugs that need to be fixed.

## Why We Need A 'CMakeLists.txt' File in `third_party`

Because we need to build the source code of lensfun, and on Windows machines, we need to reconfigure the CMakeLists.txt file from the original one to make it compatible with the Windows environment. We will also update the CMakeLists.txt file when there is a new release of lensfun, and we will also update the CMakeLists.txt file if there are some critical bugs that need to be fixed.

## How about the `metal-cpp` Folder?

Since Pu-erh Lab v0.2.0, a new Metal support has been added, and we need to include the source code of metal-cpp in our project. We will update the source code of metal-cpp when there is a new release, and we will also update the source code of metal-cpp if there are some critical bugs that need to be fixed.

## How about the `libultrahdr` Folder?

`libultrahdr` is managed as an upstream git submodule in `alcedo_studio/src/third_party/libultrahdr` so Pu-erh Lab does not vendor a private copy of the library. The build expects a complete submodule checkout there; if it is missing, initialize it with:

```powershell
git submodule update --init --recursive alcedo_studio/src/third_party/libultrahdr
```

## How about `LibRaw`?

Alcedo uses the pinned custom LibRaw submodule in `alcedo_studio/src/third_party/libraw` so Nikon HE/HE* compressed RAW support is available on both macOS and Windows. The Windows presets require this bundled checkout and intentionally do not fall back to the vcpkg LibRaw package.

```powershell
git submodule update --init --recursive alcedo_studio/src/third_party/libraw
```

## How about `clblast_min`?

`clblast_min` is **not** a full CLBlast checkout. It is a minimal Apache-2.0 extract of
the FP32 single-kernel direct-convolution path used by OpenCL DemosaicNet
(`xconvgemm_direct_f32_nhwc4.cl`), plus `LICENSE` and `UPSTREAM.md` provenance.
Do not vendor the CLBlast build system, C++ API, tuner, or unrelated BLAS levels.

## How about `protobuf` and `grpc`?

The semantic runtime client builds gRPC and protobuf from pinned source submodules next to `libultrahdr`. Do not rely on vcpkg or Homebrew for these two packages. The top-level CMake build consumes the local source targets through `puerhlab::grpc++` and `puerhlab::protobuf`.

The gRPC checkout must include its recursive third-party dependencies:

```powershell
git submodule update --init --recursive alcedo_studio/src/third_party/protobuf alcedo_studio/src/third_party/grpc
```

## How about `QuickQanava`?

The node editor baseline uses the pinned QuickQanava submodule in
`alcedo_studio/src/third_party/QuickQanava`. Do not FetchContent this library. Pin is
tag `2.50` (`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`). See
`docs/roadmap/alcedo_studio/edit/node_mask_editor/phase_nm0_quickqanava_integration_plan.md`.

```powershell
git submodule update --init alcedo_studio/src/third_party/QuickQanava
```
