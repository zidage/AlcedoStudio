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

// Hard-coded X-Trans student DemosaicNet (`xtrans_p2_s32_d4`). Topology matches
// student_handoff/xtrans_architecture.md and bundled xtrans.safetensors — not a
// runtime graph.
//
// Input:  contiguous NCHW f32 mosaic [N, 3, H, W], H,W >= kMinSpatial, divisible by
//         pack_factor (2).
// Output: contiguous NCHW f32 RGB [N, 3, Oh, Ow] where
//   - exact tile (H=W=kTileInput) → center-cropped export kTileOutput
//   - otherwise → natural size H - kNaturalSpatialLoss
//
// Uses space-to-depth pack (3→12), residual 1×1 → 12, grouped unpack → RGB,
// then mosaick skip + post conv (unlike the full-res teacher trunk).
class XTransDemosaicNet : public NnWeightModule<XTransDemosaicNet> {
 public:
  static constexpr const char* kArchitecture = "xtrans_p2_s32_d4";
  static constexpr int         kDepth        = 4;
  static constexpr int         kWidth        = 32;
  static constexpr int         kPackFactor   = 2;
  static constexpr int         kPackOutCh    = 12;  // space-to-depth 3*2*2
  static constexpr int         kResidualCh   = 12;

  // Tile / CFA contract (product tiling + export goldens).
  static constexpr int kTileInput  = 1048;
  static constexpr int kTileOutput = 1024;
  static constexpr int kTileBorder = 12;
  static constexpr int kTilePad    = 12;
  static constexpr int kTileStep   = 1020;  // 1024 % 6 != 0 → period-safe step
  static constexpr int kCfaPeriod  = 6;

  // Natural valid-conv shrink: H - 2*pack_factor*depth - 2 = H - 18
  static constexpr int kNaturalSpatialLoss = 2 * kPackFactor * kDepth + 2;
  // Export shrink on the fixed training tile (1048 → 1024).
  static constexpr int kSpatialLoss = kTileInput - kTileOutput;  // 24
  static constexpr int kMinSpatial  = kNaturalSpatialLoss + 2;   // 20

  XTransDemosaicNet() = default;

  void LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                       cudaStream_t stream = nullptr);

  void Forward(const cuda::nn::DeviceTensor& input, cuda::nn::DeviceTensor& output,
               cuda::nn::WorkspacePool& workspace, cudaStream_t stream = nullptr) const;

  [[nodiscard]] static auto NaturalOutputHeight(int input_h) -> int {
    return input_h - kNaturalSpatialLoss;
  }
  [[nodiscard]] static auto NaturalOutputWidth(int input_w) -> int {
    return input_w - kNaturalSpatialLoss;
  }
  [[nodiscard]] static auto OutputHeight(int input_h, int input_w = -1) -> int {
    const int w = input_w < 0 ? input_h : input_w;
    if (input_h == kTileInput && w == kTileInput) {
      return kTileOutput;
    }
    return NaturalOutputHeight(input_h);
  }
  [[nodiscard]] static auto OutputWidth(int input_w, int input_h = -1) -> int {
    const int h = input_h < 0 ? input_w : input_h;
    if (h == kTileInput && input_w == kTileInput) {
      return kTileOutput;
    }
    return NaturalOutputWidth(input_w);
  }

  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  [[nodiscard]] auto PackWeightDevicePtr() const -> const float* { return pack_w_.get(); }
  [[nodiscard]] auto OutputWeightDevicePtr() const -> const float* { return output_w_.get(); }

 private:
  cuda::nn::DeviceBufferF32 pack_w_;  // fixed, no bias
  cuda::nn::DeviceBufferF32 trunk_w_[kDepth];
  cuda::nn::DeviceBufferF32 trunk_b_[kDepth];
  cuda::nn::DeviceBufferF32 residual_w_;
  cuda::nn::DeviceBufferF32 residual_b_;
  cuda::nn::DeviceBufferF32 unpack_w_;  // fixed, no bias
  cuda::nn::DeviceBufferF32 post_w_;
  cuda::nn::DeviceBufferF32 post_b_;
  cuda::nn::DeviceBufferF32 output_w_;
  cuda::nn::DeviceBufferF32 output_b_;
};

}  // namespace alcedo
