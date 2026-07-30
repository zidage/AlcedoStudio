//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#if defined(_WIN32) && defined(HAVE_CUDA)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <optional>
#include <string>
#include <vector>

namespace alcedo::editor_rhi {

struct CudaAdapterInfo {
  int         device_index = -1;
  LUID        luid{};
  std::string name;
  int         compute_major = 0;
  int         compute_minor = 0;
};

struct CudaAdapterDiscoveryResult {
  bool                        ok = false;
  std::string                 error;
  std::vector<CudaAdapterInfo> devices;
  // Preferred device for Alcedo (first CUDA device with a valid LUID that has a
  // matching DXGI adapter). Empty when none.
  std::optional<CudaAdapterInfo> preferred;
};

// Enumerates CUDA devices and their Windows LUIDs. Does not create a QQuickWindow
// or Qt RHI device. Safe to call before QGuiApplication if CUDA runtime allows it;
// callers typically run after QGuiApplication construction but before any QQuickWindow.
[[nodiscard]] auto DiscoverCudaAdapters() -> CudaAdapterDiscoveryResult;

// Resolve LUID for one CUDA device index.
[[nodiscard]] auto GetCudaDeviceLuid(int cuda_device) -> std::optional<LUID>;

[[nodiscard]] auto LuidMatches(const LUID& lhs, const LUID& rhs) -> bool;
[[nodiscard]] auto DescribeLuid(const LUID& luid) -> std::string;

// Enumerate DXGI adapters and find the index / LUID match for a CUDA device.
struct DxgiAdapterMatch {
  int         adapter_index = -1;
  LUID        luid{};
  std::string description;
};

[[nodiscard]] auto FindDxgiAdapterForLuid(const LUID& luid) -> std::optional<DxgiAdapterMatch>;

// Bind CUDA device on the calling thread (cudaSetDevice + context init).
[[nodiscard]] auto BindCudaDeviceOnCurrentThread(int cuda_device, const char* context) -> bool;

// Validate that a live ID3D11Device's adapter LUID matches the given CUDA device.
// device must be a non-null ID3D11Device*. Returns false on mismatch.
[[nodiscard]] auto D3D11DeviceMatchesCudaDevice(void* d3d11_device, int cuda_device) -> bool;

}  // namespace alcedo::editor_rhi

#endif  // _WIN32 && HAVE_CUDA
