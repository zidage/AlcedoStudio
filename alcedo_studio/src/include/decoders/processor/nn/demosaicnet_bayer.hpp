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

// Hard-coded Bayer student DemosaicNet (`bayer_s24_d8`). Topology matches
// student_handoff/bayer_architecture.md and bundled bayer.safetensors — not a
// runtime graph.
//
// Input:  contiguous NCHW f32 mosaic [N, 3, H, W], H and W even, H,W >= kMinSpatial.
// Output: contiguous NCHW f32 RGB    [N, 3, Oh, Ow] where
//   - product square tile H=W=owned+2*kTileBorder with owned>=kTileOutput →
//     center-cropped export `owned` (retains 1K export context: 1086→1024, …)
//   - otherwise → natural size H - kNaturalSpatialLoss
//
// Weights are immutable after LoadWeights. Forward is a pure function of
// (weights, input, workspace, stream). Caller owns the WorkspacePool.
class BayerDemosaicNet : public NnWeightModule<BayerDemosaicNet> {
 public:
  static constexpr const char* kArchitecture = "bayer_s24_d8";
  static constexpr int         kDepth        = 8;
  static constexpr int         kWidth        = 24;
  static constexpr int         kPackFactor   = 2;
  static constexpr int         kPackOutCh    = 4;   // collapse-colors 2×2
  static constexpr int         kResidualCh   = 12;  // 3 * pack_factor^2

  // Tile / CFA contract (product tiling + export goldens).
  static constexpr int kTileInput  = 1086;
  static constexpr int kTileOutput = 1024;
  static constexpr int kTileBorder = 31;  // (input - output) / 2
  static constexpr int kTilePad    = 32;  // period-aligned virtual pad (NOT 31)
  static constexpr int kTileStep   = 1024;
  static constexpr int kCfaPeriod  = 2;

  // Natural valid-conv shrink before optional export center-crop:
  //   H - 2*pack_factor*depth - 2  = H - 34
  static constexpr int kNaturalSpatialLoss = 2 * kPackFactor * kDepth + 2;
  // Export shrink on the fixed training tile (1086 → 1024). Product tiling uses
  // kTilePad / kTileBorder / NeuralOutputGeometry instead of this single integer.
  static constexpr int kSpatialLoss = kTileInput - kTileOutput;  // 62
  // Smallest even spatial size that yields a positive natural output.
  static constexpr int kMinSpatial = kNaturalSpatialLoss + 2;  // 36

  BayerDemosaicNet() = default;

  void LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                       cudaStream_t stream = nullptr);

  // Straight-line student forward. `output` must be pre-sized to OutputShape.
  void Forward(const cuda::nn::DeviceTensor& input, cuda::nn::DeviceTensor& output,
               cuda::nn::WorkspacePool& workspace, cudaStream_t stream = nullptr) const;

  // Spatial helpers matching student_models.StudentDemosaicNet.output_size.
  [[nodiscard]] static auto NaturalOutputHeight(int input_h) -> int {
    return input_h - kNaturalSpatialLoss;
  }
  [[nodiscard]] static auto NaturalOutputWidth(int input_w) -> int {
    return input_w - kNaturalSpatialLoss;
  }
  // Product export owned edge when input is a square product tile; else -1.
  // Minimum owned edge matches experimental tiling (256); free-size patches stay natural.
  [[nodiscard]] static auto ProductOwnedOutput(int input_h, int input_w) -> int {
    if (input_h != input_w) {
      return -1;
    }
    const int owned = input_h - 2 * kTileBorder;
    // Exclude free-size patches (e.g. 64→natural) while allowing sub-1K product tiles.
    if (owned < 256 || (owned % kCfaPeriod) != 0) {
      return -1;
    }
    if (input_h != owned + 2 * kTileBorder) {
      return -1;
    }
    return owned;
  }
  [[nodiscard]] static auto OutputHeight(int input_h, int input_w = -1) -> int {
    const int w      = input_w < 0 ? input_h : input_w;
    const int owned  = ProductOwnedOutput(input_h, w);
    if (owned > 0) {
      return owned;
    }
    return NaturalOutputHeight(input_h);
  }
  [[nodiscard]] static auto OutputWidth(int input_w, int input_h = -1) -> int {
    const int h     = input_h < 0 ? input_w : input_h;
    const int owned = ProductOwnedOutput(h, input_w);
    if (owned > 0) {
      return owned;
    }
    return NaturalOutputWidth(input_w);
  }

  // Peak-live activation workspace for one Forward (P1 slot reuse; not sum-of-all
  // intermediates). Does not include weight VRAM.
  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  // Device bytes held by weight slots (0 if not loaded).
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Stable device pointers for no-reload tests (null if empty).
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
