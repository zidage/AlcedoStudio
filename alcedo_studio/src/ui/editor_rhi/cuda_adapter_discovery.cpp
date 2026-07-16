//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/cuda_adapter_discovery.hpp"

#if defined(_WIN32) && defined(HAVE_CUDA)

#include <cuda_runtime_api.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace alcedo::editor_rhi {
namespace {

using Microsoft::WRL::ComPtr;

}  // namespace

auto GetCudaDeviceLuid(int cuda_device) -> std::optional<LUID> {
  if (cuda_device < 0) {
    return std::nullopt;
  }

  cudaDeviceProp    prop{};
  const cudaError_t prop_err = cudaGetDeviceProperties(&prop, cuda_device);
  if (prop_err != cudaSuccess) {
    return std::nullopt;
  }

  LUID luid{};
  static_assert(sizeof(luid) == sizeof(prop.luid));
  std::memcpy(&luid, prop.luid, sizeof(luid));
  return luid;
}

auto LuidMatches(const LUID& lhs, const LUID& rhs) -> bool {
  return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
}

auto DescribeLuid(const LUID& luid) -> std::string {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08x:%08x", static_cast<unsigned>(luid.HighPart),
                static_cast<unsigned>(luid.LowPart));
  return buf;
}

auto BindCudaDeviceOnCurrentThread(int cuda_device, const char* context) -> bool {
  if (cuda_device < 0) {
    return false;
  }

  const cudaError_t set_err = cudaSetDevice(cuda_device);
  if (set_err != cudaSuccess) {
    std::fprintf(stderr, "cuda_adapter_discovery: cudaSetDevice(%d) failed in %s: %s\n",
                 cuda_device, context ? context : "?", cudaGetErrorString(set_err));
    return false;
  }

  const cudaError_t init_err = cudaFree(nullptr);
  if (init_err != cudaSuccess) {
    std::fprintf(stderr, "cuda_adapter_discovery: cudaFree(0) failed in %s for device %d: %s\n",
                 context ? context : "?", cuda_device, cudaGetErrorString(init_err));
    return false;
  }

  const cudaError_t stale_err = cudaGetLastError();
  if (stale_err != cudaSuccess) {
    std::fprintf(stderr,
                 "cuda_adapter_discovery: cleared stale CUDA error before %s on device %d: %s\n",
                 context ? context : "?", cuda_device, cudaGetErrorString(stale_err));
  }
  return true;
}

auto FindDxgiAdapterForLuid(const LUID& luid) -> std::optional<DxgiAdapterMatch> {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory) {
    return std::nullopt;
  }

  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (!adapter) {
      continue;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      continue;
    }
    if (LuidMatches(desc.AdapterLuid, luid)) {
      DxgiAdapterMatch match;
      match.adapter_index = static_cast<int>(index);
      match.luid          = desc.AdapterLuid;
      // Convert wide description to UTF-8-ish narrow for diagnostics.
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
      match.description = name;
      return match;
    }
  }
  return std::nullopt;
}

auto DiscoverCudaAdapters() -> CudaAdapterDiscoveryResult {
  CudaAdapterDiscoveryResult result;

  int               device_count = 0;
  const cudaError_t count_err    = cudaGetDeviceCount(&device_count);
  if (count_err != cudaSuccess || device_count <= 0) {
    result.error = std::string("cudaGetDeviceCount failed: ") +
                   (count_err == cudaSuccess ? "no devices" : cudaGetErrorString(count_err));
    return result;
  }

  for (int i = 0; i < device_count; ++i) {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) {
      continue;
    }
    CudaAdapterInfo info;
    info.device_index   = i;
    info.name           = prop.name ? prop.name : "";
    info.compute_major  = prop.major;
    info.compute_minor  = prop.minor;
    const auto luid_opt = GetCudaDeviceLuid(i);
    if (!luid_opt) {
      continue;
    }
    info.luid = *luid_opt;
    result.devices.push_back(info);

    if (!result.preferred.has_value()) {
      if (FindDxgiAdapterForLuid(info.luid).has_value()) {
        result.preferred = info;
      }
    }
  }

  if (result.devices.empty()) {
    result.error = "no CUDA devices with a Windows LUID were discovered";
    return result;
  }
  if (!result.preferred.has_value()) {
    result.error =
        "CUDA devices were found but none matched a DXGI adapter LUID for D3D11 interop";
    return result;
  }

  result.ok = true;
  return result;
}

auto D3D11DeviceMatchesCudaDevice(void* d3d11_device, int cuda_device) -> bool {
  if (!d3d11_device || cuda_device < 0) {
    return false;
  }
  auto* device = static_cast<ID3D11Device*>(d3d11_device);
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()))) || !dxgi_device) {
    return false;
  }
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) || !adapter) {
    return false;
  }
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) {
    return false;
  }
  const auto cuda_luid = GetCudaDeviceLuid(cuda_device);
  if (!cuda_luid) {
    return false;
  }
  return LuidMatches(desc.AdapterLuid, *cuda_luid);
}

}  // namespace alcedo::editor_rhi

#endif  // _WIN32 && HAVE_CUDA
