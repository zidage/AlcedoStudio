//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_accelerator.hpp"

#include <string>
#include <stdexcept>

#ifdef HAVE_CUDA
#include "utils/cuda/cuda_driver_requirements.hpp"

#include <opencv2/core/cuda.hpp>
#endif

#ifdef HAVE_OPENCL
#include "opencl/opencl_runtime.hpp"
#endif

namespace alcedo {
namespace {

auto IsCudaRuntimeAvailable() -> bool {
#ifdef HAVE_CUDA
  try {
#if defined(_WIN32)
    if (!cuda::CheckDriverSupport().IsSupported()) {
      return false;
    }
#endif
    return cv::cuda::getCudaEnabledDeviceCount() > 0;
  } catch (...) {
    return false;
  }
#else
  return false;
#endif
}

auto TryOpenClRuntime() -> bool {
#ifdef HAVE_OPENCL
  return TryInitializeOpenClRuntime();
#else
  return false;
#endif
}

void PrepareExplicitOpenClRuntime() {
#ifdef HAVE_OPENCL
  InitializeOpenClRuntime();
#else
  throw std::runtime_error("OpenCL accelerator backend is not compiled.");
#endif
}

[[noreturn]] void ThrowUnavailableBackend(std::string_view backend) {
  throw std::runtime_error("Requested accelerator backend is unavailable: " + std::string(backend) +
                           ".");
}

}  // namespace

auto ResolveAcceleratorBackend(AcceleratorBackendPreference preference) -> GpuBackendKind {
#if defined(__APPLE__)
  if (preference == AcceleratorBackendPreference::CUDA ||
      preference == AcceleratorBackendPreference::OpenCL) {
    ThrowUnavailableBackend(AcceleratorBackendPreferenceToString(preference));
  }
#endif

  switch (preference) {
    case AcceleratorBackendPreference::CPU:
      return GpuBackendKind::None;

    case AcceleratorBackendPreference::CUDA:
      if (IsCudaRuntimeAvailable()) {
        return GpuBackendKind::CUDA;
      }
      ThrowUnavailableBackend("cuda");

    case AcceleratorBackendPreference::OpenCL:
      PrepareExplicitOpenClRuntime();
      return GpuBackendKind::OpenCL;

    case AcceleratorBackendPreference::Metal:
#ifdef HAVE_METAL
      return GpuBackendKind::Metal;
#else
      ThrowUnavailableBackend("metal");
#endif

    case AcceleratorBackendPreference::Auto:
#if defined(__APPLE__)
#ifdef HAVE_METAL
      return GpuBackendKind::Metal;
#else
      return GpuBackendKind::None;
#endif
#else
      if (TryOpenClRuntime()) {
        return GpuBackendKind::OpenCL;
      }
      if (IsCudaRuntimeAvailable()) {
        return GpuBackendKind::CUDA;
      }
#ifdef HAVE_METAL
      return GpuBackendKind::Metal;
#else
      return GpuBackendKind::None;
#endif
#endif
  }

  return GpuBackendKind::None;
}

auto IsCompiledGpuBackend(GpuBackendKind backend) -> bool {
  switch (backend) {
    case GpuBackendKind::None:
      return true;
    case GpuBackendKind::CUDA:
#ifdef HAVE_CUDA
      return true;
#else
      return false;
#endif
    case GpuBackendKind::OpenCL:
#ifdef HAVE_OPENCL
      return true;
#else
      return false;
#endif
    case GpuBackendKind::Metal:
#ifdef HAVE_METAL
      return true;
#else
      return false;
#endif
  }
  return false;
}

auto IsImplementedMergedPipelineBackend(GpuBackendKind backend) -> bool {
  switch (backend) {
    case GpuBackendKind::CUDA:
#ifdef HAVE_CUDA
      return true;
#else
      return false;
#endif
    case GpuBackendKind::Metal:
#ifdef HAVE_METAL
      return true;
#else
      return false;
#endif
    case GpuBackendKind::OpenCL:
#ifdef HAVE_OPENCL
      return true;
#else
      return false;
#endif
    case GpuBackendKind::None:
      return false;
  }
  return false;
}

auto IsImplementedGeometryOperatorBackend(GpuBackendKind backend) -> bool {
  switch (backend) {
    case GpuBackendKind::CUDA:
#ifdef HAVE_CUDA
      return true;
#else
      return false;
#endif
    case GpuBackendKind::Metal:
#ifdef HAVE_METAL
      return true;
#else
      return false;
#endif
    case GpuBackendKind::OpenCL:
#ifdef HAVE_OPENCL
      return true;
#else
      return false;
#endif
    case GpuBackendKind::None:
      return false;
  }
  return false;
}

auto AcceleratorBackendPreferenceToString(AcceleratorBackendPreference preference)
    -> std::string_view {
  switch (preference) {
    case AcceleratorBackendPreference::CPU:
      return "cpu";
    case AcceleratorBackendPreference::Auto:
      return "auto";
    case AcceleratorBackendPreference::CUDA:
      return "cuda";
    case AcceleratorBackendPreference::OpenCL:
      return "opencl";
    case AcceleratorBackendPreference::Metal:
      return "metal";
  }
  return "unknown";
}

}  // namespace alcedo
