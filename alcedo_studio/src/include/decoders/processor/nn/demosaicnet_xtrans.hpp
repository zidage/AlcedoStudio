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
//   - product square tile H=W=owned+2*kTileBorder with owned>=kTileOutput →
//     center-cropped export `owned` (retains 1K export context: 1048→1024, …)
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
               cuda::nn::WorkspacePool& workspace, cudaStream_t stream = nullptr,
               bool force_ordinary_tail = false) const;

  // Product HWC entry (P4-A): fused post/output/(optional gamma) into pitched HWC.
  void ForwardHwc(const cuda::nn::DeviceTensor& input, float* rgb_hwc, std::size_t rgb_step_bytes,
                  cuda::nn::WorkspacePool& workspace, cudaStream_t stream = nullptr,
                  bool apply_gamma_decode = true) const;

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
    if (owned < 256) {
      return -1;
    }
    if (input_h != owned + 2 * kTileBorder) {
      return -1;
    }
    return owned;
  }
  [[nodiscard]] static auto OutputHeight(int input_h, int input_w = -1) -> int {
    const int w     = input_w < 0 ? input_h : input_w;
    const int owned = ProductOwnedOutput(input_h, w);
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
  // intermediates). Does not include weight VRAM. Default matches product fused tail.
  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1,
                                                   bool fuse_post_output = true) -> std::size_t;

  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  [[nodiscard]] auto PackWeightDevicePtr() const -> const float* { return pack_w_.get(); }
  [[nodiscard]] auto OutputWeightDevicePtr() const -> const float* { return output_w_.get(); }
  [[nodiscard]] auto OutputWeightCioDevicePtr() const -> const float* {
    return output_w_cio_.get();
  }

 private:
  cuda::nn::DeviceBufferF32 pack_w_;  // fixed, no bias
  cuda::nn::DeviceBufferF32 trunk_w_[kDepth];
  cuda::nn::DeviceBufferF32 trunk_b_[kDepth];
  cuda::nn::DeviceBufferF32 residual_w_;
  cuda::nn::DeviceBufferF32 residual_b_;
  cuda::nn::DeviceBufferF32 unpack_w_;  // fixed, no bias
  cuda::nn::DeviceBufferF32 post_w_;
  cuda::nn::DeviceBufferF32 post_b_;
  cuda::nn::DeviceBufferF32 output_w_;      // OIHW [3,width,1,1] for ordinary path
  cuda::nn::DeviceBufferF32 output_w_cio_;  // prepacked [width,3] for fused tail
  cuda::nn::DeviceBufferF32 output_b_;
};

}  // namespace alcedo
