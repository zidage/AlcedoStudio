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
};

auto ResolveAcceleratorBackend(AcceleratorBackendPreference preference) -> GpuBackendKind;
auto IsCompiledGpuBackend(GpuBackendKind backend) -> bool;
auto IsImplementedMergedPipelineBackend(GpuBackendKind backend) -> bool;
auto IsImplementedGeometryOperatorBackend(GpuBackendKind backend) -> bool;
auto AcceleratorBackendPreferenceToString(AcceleratorBackendPreference preference)
    -> std::string_view;

}  // namespace alcedo
