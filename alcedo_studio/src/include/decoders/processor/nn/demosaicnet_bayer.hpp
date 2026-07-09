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

// Hard-coded Bayer DemosaicNet (depth=15, width=64). Topology matches
// demosaicnet BayerDemosaick / bayer.safetensors — not a runtime graph.
//
// Input:  contiguous NCHW f32 mosaic [N, 3, H, W], H and W even, H,W >= 64.
// Output: contiguous NCHW f32 RGB    [N, 3, H-62, W-62].
//
// Weights are immutable after LoadWeights. Forward is a pure function of
// (weights, input, workspace, stream). Caller owns the WorkspacePool.
class BayerDemosaicNet : public NnWeightModule<BayerDemosaicNet> {
 public:
  static constexpr int kDepth       = 15;
  static constexpr int kWidth       = 64;
  static constexpr int kMinSpatial  = 64;  // even; yields 2×2 RGB
  static constexpr int kSpatialLoss = 62;  // H_out = H_in - kSpatialLoss

  BayerDemosaicNet() = default;

  void LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                       cudaStream_t stream = nullptr);

  // Straight-line §2.1 forward. `output` must be pre-sized to OutputShape.
  void Forward(const cuda::nn::DeviceTensor& input, cuda::nn::DeviceTensor& output,
               cuda::nn::WorkspacePool& workspace, cudaStream_t stream = nullptr) const;

  // Spatial helpers (N and C fixed for demosaicnet).
  [[nodiscard]] static auto OutputHeight(int input_h) -> int { return input_h - kSpatialLoss; }
  [[nodiscard]] static auto OutputWidth(int input_w) -> int { return input_w - kSpatialLoss; }

  // Peak activation workspace for one Forward (does not include weight VRAM).
  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  // Device bytes held by weight slots (0 if not loaded).
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Stable device pointers for no-reload tests (null if empty).
  [[nodiscard]] auto PackWeightDevicePtr() const -> const float* { return pack_w_.get(); }
  [[nodiscard]] auto OutputWeightDevicePtr() const -> const float* { return output_w_.get(); }

 private:
  cuda::nn::DeviceBufferF32 pack_w_;
  cuda::nn::DeviceBufferF32 pack_b_;
  cuda::nn::DeviceBufferF32 conv_w_[kDepth];
  cuda::nn::DeviceBufferF32 conv_b_[kDepth];
  cuda::nn::DeviceBufferF32 residual_w_;
  cuda::nn::DeviceBufferF32 residual_b_;
  cuda::nn::DeviceBufferF32 unpack_w_;
  cuda::nn::DeviceBufferF32 unpack_b_;
  cuda::nn::DeviceBufferF32 post_w_;
  cuda::nn::DeviceBufferF32 post_b_;
  cuda::nn::DeviceBufferF32 output_w_;
  cuda::nn::DeviceBufferF32 output_b_;
};

}  // namespace alcedo
