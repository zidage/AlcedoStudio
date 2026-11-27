//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string_view>

#include "image/gpu_backend.hpp"

namespace alcedo {

enum class AcceleratorBackendPreference {
  CPU,
  Auto,
  CUDA,
  OpenCL,
  Metal,
  /// In development (#1036). `Auto` never resolves to Vulkan.
  Vulkan,
};

/// Resolves @p preference to the backend of this process. An explicit preference that is not
/// available throws with the reason. For `Vulkan` the call creates the `VulkanContext` of the
/// process when it does not exist (options from the environment, `ALCEDO_VULKAN_DEVICE`) and
/// passes its exception to the caller.
auto ResolveAcceleratorBackend(AcceleratorBackendPreference preference) -> GpuBackendKind;
auto IsCompiledGpuBackend(GpuBackendKind backend) -> bool;
auto IsImplementedMergedPipelineBackend(GpuBackendKind backend) -> bool;
auto IsImplementedGeometryOperatorBackend(GpuBackendKind backend) -> bool;
auto AcceleratorBackendPreferenceToString(AcceleratorBackendPreference preference)
    -> std::string_view;

}  // namespace alcedo
