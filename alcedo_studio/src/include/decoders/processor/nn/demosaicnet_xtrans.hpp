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
#include "decoders/processor/nn/demosaicnet_specs.hpp"

namespace alcedo {

// Hard-coded X-Trans student DemosaicNet (`xtrans_p2_s32_d4`). Topology matches
// student_handoff/xtrans_architecture.md and bundled xtrans.safetensors — not a
// runtime graph.
//
// Input:  contiguous NCHW f32 mosaic [N, 3, H, W], H,W >= kMinSpatial, divisible by
//         pack_factor (2).
// Output: pitched HWC RGB via ForwardHwcChannelsLast, where
//   - product tile H=owned_h+2*kTileBorder, W=owned_w+2*kTileBorder with each
//     owned axis >= kMinProductOwned → center-cropped export (owned_h, owned_w)
//     (retains 1K export context: 1048→1024, …; rectangular/strip tiles use the
//     same border per axis)
//   - otherwise → natural size H - kNaturalSpatialLoss
//
// Uses space-to-depth pack (3→12), residual 1×1 → 12, grouped unpack → RGB,
// then mosaick skip + post conv (unlike the full-res teacher trunk).
class XTransDemosaicNet : public NnWeightModule<XTransDemosaicNet> {
 public:
  using Spec = DemosaicNetXTransSpec;

  static constexpr const char* kArchitecture = Spec::kArchitecture;
  static constexpr int         kDepth        = Spec::kDepth;
  static constexpr int         kWidth        = Spec::kWidth;
  static constexpr int         kPackFactor   = Spec::kPackFactor;
  static constexpr int         kPackOutCh    = Spec::kPackOutCh;
  static constexpr int         kResidualCh   = Spec::kResidualCh;

  // Tile / CFA contract (product tiling + export goldens).
  static constexpr int kTileInput  = Spec::kTileInput;
  static constexpr int kTileOutput = Spec::kTileOutput;
  static constexpr int kTileBorder = Spec::kTileBorder;
  static constexpr int kTilePad    = Spec::kTilePad;
  static constexpr int kTileStep   = Spec::kTileStep;
  static constexpr int kCfaPeriod  = Spec::kCfaPeriod;

  static constexpr int kNaturalSpatialLoss = Spec::kNaturalSpatialLoss;
  static constexpr int kSpatialLoss        = Spec::kSpatialLoss;
  static constexpr int kMinSpatial         = Spec::kMinSpatial;
  static constexpr int kMinProductOwned    = Spec::kMinProductOwned;

  XTransDemosaicNet() = default;

  void LoadWeightsImpl(const cuda::nn::SafetensorsTensorMap& tensors,
                       cudaStream_t stream = nullptr);

  // Product path: pack → trunk0 NCHW → persistent NHWC CUTLASS trunk → residual/
  // structural → fused post/output/(optional gamma) into pitched HWC RGB.
  void ForwardHwcChannelsLast(const cuda::nn::DeviceTensor& input, float* rgb_hwc,
                              std::size_t rgb_step_bytes, cuda::nn::WorkspacePool& workspace,
                              cudaStream_t stream = nullptr,
                              bool apply_gamma_decode = true) const;

  [[nodiscard]] static auto NaturalOutputHeight(int input_h) -> int {
    return Spec::NaturalOutputHeight(input_h);
  }
  [[nodiscard]] static auto NaturalOutputWidth(int input_w) -> int {
    return Spec::NaturalOutputWidth(input_w);
  }
  [[nodiscard]] static auto IsProductExportInput(int input_h, int input_w) -> bool {
    return Spec::IsProductExportInput(input_h, input_w);
  }
  [[nodiscard]] static auto ProductOwnedOutput(int input_h, int input_w) -> int {
    return Spec::ProductOwnedOutput(input_h, input_w);
  }
  [[nodiscard]] static auto OutputHeight(int input_h, int input_w = -1) -> int {
    return Spec::OutputHeight(input_h, input_w);
  }
  [[nodiscard]] static auto OutputWidth(int input_w, int input_h = -1) -> int {
    return Spec::OutputWidth(input_w, input_h);
  }

  // Peak-live activation workspace for one ForwardHwcChannelsLast (fused tail).
  // Does not include weight VRAM.
  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  [[nodiscard]] auto PackWeightDevicePtr() const -> const float* { return pack_w_.get(); }
  [[nodiscard]] auto OutputWeightCioDevicePtr() const -> const float* {
    return output_w_cio_.get();
  }

 private:
  cuda::nn::DeviceBufferF32 pack_w_;  // fixed, no bias
  // First unequal trunk layer stays NCHW (12→32); later square layers are CUTLASS KRSC.
  cuda::nn::DeviceBufferF32 trunk0_w_;
  cuda::nn::DeviceBufferF32 trunk_w_nhwc_[kDepth];
  cuda::nn::DeviceBufferF32 trunk_b_[kDepth];
  cuda::nn::DeviceBufferF32 residual_w_nhwc_;  // prepacked [width,12]
  cuda::nn::DeviceBufferF32 residual_b_;
  cuda::nn::DeviceBufferF32 post_w_;
  cuda::nn::DeviceBufferF32 post_b_;
  cuda::nn::DeviceBufferF32 output_w_cio_;  // prepacked [width,3] for fused tail
  cuda::nn::DeviceBufferF32 output_b_;
};

}  // namespace alcedo
