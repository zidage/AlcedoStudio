//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <initializer_list>
#include <string_view>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/safetensors.hpp"

namespace alcedo {

// CRTP base that standardizes weight ingestion for hard-coded RAW NN modules.
//
// Derived types own fixed DeviceBuffer slots and implement LoadWeightsImpl by
// mapping known safetensors keys into those slots. Topology is never read from
// the file — only tensor values are.
//
// Design intent (see docs/roadmap/cuda_nn_forward_demosaicnet_plan.md §3.2.1):
// - No runtime layer list / graph IR
// - Zero-overhead LoadWeights dispatch (static_cast, not virtual)
// - Shared Require + H2D helpers for every hard-coded module
template <typename Derived>
class NnWeightModule {
 public:
  void LoadWeights(const cuda::nn::SafetensorsTensorMap& tensors,
                   cudaStream_t stream = nullptr) {
    static_cast<Derived*>(this)->LoadWeightsImpl(tensors, stream);
    loaded_ = true;
  }

  [[nodiscard]] auto weights_loaded() const -> bool { return loaded_; }

 protected:
  NnWeightModule() = default;

  // Lookup F32 + exact shape, then replace `dst` with a fresh device upload.
  static void RequireUpload(const cuda::nn::SafetensorsTensorMap& map, std::string_view key,
                            std::initializer_list<std::int64_t> expected_shape,
                            cuda::nn::DeviceBufferF32& dst, cudaStream_t stream) {
    const auto& host = cuda::nn::RequireF32Tensor(map, key, expected_shape);
    dst              = cuda::nn::UploadToDevice(host, stream);
  }

  bool loaded_ = false;
};

}  // namespace alcedo
