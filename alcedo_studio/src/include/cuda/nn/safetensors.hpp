//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// CUDA compatibility façade over backend-neutral host safetensors DTOs.
// Host parsing lives in alcedo::nn; this header re-exports those types under
// alcedo::cuda::nn and adds CUDA H2D upload helpers.

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "nn/safetensors.hpp"

namespace alcedo::cuda::nn {

using SafetensorsTensor    = ::alcedo::nn::SafetensorsTensor;
using SafetensorsTensorMap = ::alcedo::nn::SafetensorsTensorMap;

using ::alcedo::nn::LoadSafetensors;
using ::alcedo::nn::RequireF32Tensor;
using ::alcedo::nn::ShapesEqual;

// Thin H2D helpers for LoadWeightsImpl (stream-aware).
[[nodiscard]] auto UploadToDevice(const SafetensorsTensor& tensor, cudaStream_t stream = nullptr)
    -> DeviceBufferF32;

void UploadTo(DeviceBufferF32& dst, const SafetensorsTensor& tensor,
              cudaStream_t stream = nullptr);

}  // namespace alcedo::cuda::nn
