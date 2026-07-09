//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/safetensors.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/nn/demosaicnet_module.hpp"

namespace alcedo {

// Hard-coded X-Trans DemosaicNet (depth=11, width=64). Topology matches
// demosaicnet XTransDemosaick / xtrans.safetensors — not a runtime graph.
//
// Input:  contiguous NCHW f32 mosaic [N, 3, H, W], H,W >= 26 (for positive out).
// Output: contiguous NCHW f32 RGB    [N, 3, H-24, W-24].
//
// No pack / residual / transpose branch — full resolution throughout the main
// stack, then crop+concat full-res residual path.
class XTransDemosaicNet : public NnWeightModule<XTransDemosaicNet> {
 public:
  static constexpr int kDepth       = 11;
  static constexpr int kWidth       = 64;
  static constexpr int kMinSpatial  = 26;
  static constexpr int kSpatialLoss = 24;  // H_out = H_in - kSpatialLoss

  XTransDemosaicNet() = default;

  void LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                       cudaStream_t stream = nullptr);

  void Forward(const cuda::nn::DeviceTensor& input, cuda::nn::DeviceTensor& output,
               cuda::nn::WorkspacePool& workspace, cudaStream_t stream = nullptr) const;

  [[nodiscard]] static auto OutputHeight(int input_h) -> int { return input_h - kSpatialLoss; }
  [[nodiscard]] static auto OutputWidth(int input_w) -> int { return input_w - kSpatialLoss; }

  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  [[nodiscard]] auto Conv1WeightDevicePtr() const -> const float* { return conv_w_[0].get(); }
  [[nodiscard]] auto OutputWeightDevicePtr() const -> const float* { return output_w_.get(); }

 private:
  cuda::nn::DeviceBufferF32 conv_w_[kDepth];
  cuda::nn::DeviceBufferF32 conv_b_[kDepth];
  cuda::nn::DeviceBufferF32 post_w_;
  cuda::nn::DeviceBufferF32 post_b_;
  cuda::nn::DeviceBufferF32 output_w_;
  cuda::nn::DeviceBufferF32 output_b_;
};

}  // namespace alcedo
