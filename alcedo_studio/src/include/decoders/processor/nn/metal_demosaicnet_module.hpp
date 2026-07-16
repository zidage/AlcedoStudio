//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "nn/safetensors.hpp"

namespace MTL {
class Buffer;
class Device;
class Texture;
}  // namespace MTL

namespace alcedo {

// Host-side layout for demosaicnet_io.metal fused-tail params. Must match the
// shader struct byte-for-byte.
struct DemosaicNetFusedTailParams {
  int batch_index = 0;
  int cat_h       = 0;
  int cat_w       = 0;
  int export_h    = 0;
  int export_w    = 0;
  int final_crop  = 0;
  int src_x0      = 0;
  int src_y0      = 0;
  int owned_w     = 0;
  int owned_h     = 0;
  int dst_x       = 0;
  int dst_y       = 0;
  int crop_x      = 0;
  int crop_y      = 0;
  int crop_w      = 0;
  int crop_h      = 0;
  int apply_gamma = 0;
};

// Hard-coded Metal MPSGraph Bayer student DemosaicNet (`bayer_s24_d8`).
//
// Fixed product tile graph (batch N=2):
//   input  NHWC FP32 [2, 1086, 1086, 3]
//   graph  output NHWC FP32 [2, unpacked_h, unpacked_h, 6]
//          (channels 0..2 sparse input skip; channels 3..5 residual)
//   fused  Metal tail: post 3×3 + bias + ReLU + output 1×1 + bias
//          (+ optional signed γ=2.2) without materializing the width-channel
//          post activation. Product path writes crop-sized RGBA; reference
//          path writes NHWC RGB.
//
// Public headers stay free of MPSGraph / Foundation types. Objective-C++ owns
// the compiled executable, tensor-data wrappers, and tail weight buffers.
class MetalBayerDemosaicNet {
 public:
  using Spec = DemosaicNetBayerSpec;

  static constexpr const char* kArchitecture       = Spec::kArchitecture;
  static constexpr int         kDepth              = Spec::kDepth;
  static constexpr int         kWidth              = Spec::kWidth;
  static constexpr int         kPackFactor         = Spec::kPackFactor;
  static constexpr int         kPackOutCh          = Spec::kPackOutCh;
  static constexpr int         kResidualCh         = Spec::kResidualCh;
  static constexpr int         kTileInput          = Spec::kTileInput;
  static constexpr int         kTileOutput         = Spec::kTileOutput;
  static constexpr int         kBatchSize          = 2;
  static constexpr int         kTileBorder         = Spec::kTileBorder;
  static constexpr int         kTilePad            = Spec::kTilePad;
  static constexpr int         kTileStep           = Spec::kTileStep;
  static constexpr int         kCfaPeriod          = Spec::kCfaPeriod;
  static constexpr int         kNaturalSpatialLoss = Spec::kNaturalSpatialLoss;
  static constexpr int         kSpatialLoss        = Spec::kSpatialLoss;
  static constexpr int         kMinSpatial         = Spec::kMinSpatial;
  static constexpr int         kMinProductOwned    = Spec::kMinProductOwned;

  MetalBayerDemosaicNet();
  ~MetalBayerDemosaicNet();

  MetalBayerDemosaicNet(const MetalBayerDemosaicNet&)            = delete;
  MetalBayerDemosaicNet& operator=(const MetalBayerDemosaicNet&) = delete;
  MetalBayerDemosaicNet(MetalBayerDemosaicNet&&) noexcept;
  MetalBayerDemosaicNet& operator=(MetalBayerDemosaicNet&&) noexcept;

  // Validate metadata/arrays, build the fixed FP32 graph through concat, compile
  // once, upload fused-tail weights, and allocate reusable private input/cat
  // tile buffers. Throws on failure and leaves the module not-ready.
  void LoadAndCompile(const nn::SafetensorsTensorMap& tensors, MTL::Device* device = nullptr);

  [[nodiscard]] auto ready() const -> bool;

  // Application-owned resident bytes for learned + fixed arrays consumed by the
  // graph and the fused-tail weight buffers.
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Caller-owned fixed tile input + concat output private buffers.
  [[nodiscard]] auto OwnedBufferBytes() const -> std::size_t;

  [[nodiscard]] auto compile_count() const -> std::uint64_t;
  [[nodiscard]] auto input_output_allocation_count() const -> std::uint64_t;

  // Reusable private FP32 tile buffers (null when not ready).
  // Input:  [2, tile_input, tile_input, 3]
  // Output: graph concat [2, unpacked_h, unpacked_h, 6]
  [[nodiscard]] auto InputBuffer() const -> MTL::Buffer*;
  [[nodiscard]] auto OutputBuffer() const -> MTL::Buffer*;

  // Geometry exposed for fused-tail encoding.
  [[nodiscard]] auto CatHeight() const -> int;
  [[nodiscard]] auto FinalCrop() const -> int;

  // Encode the compiled executable into an MPSCommandBuffer (ObjC pointer).
  // Does not commit or wait. Input buffer must already hold the fixed NHWC tile.
  // Results land in OutputBuffer (concat) via the bound results array.
  void EncodeOnMpsCommandBuffer(void* mps_command_buffer) const;

  // Product fused tail: concat → post/output/gamma → owned ROI ∩ crop RGBA.
  // Encodes into a Metal command buffer (ObjC id as void*). Does not commit.
  void EncodeFusedTailRgba(void* mtl_command_buffer, MTL::Texture* output_rgba,
                           const DemosaicNetFusedTailParams& params) const;

  // Multi-tile encode helpers: clear before the first tile; inspect after the final wait.
  void ClearLastEncodeError() const;
  [[nodiscard]] auto HasLastEncodeError() const -> bool;
  [[nodiscard]] auto LastEncodeErrorMessage() const -> std::string;

  // Synchronous host reference path for module tests:
  // NCHW input [1,3,H,W] → NHWC graph to concat → fused tail (no gamma) →
  // NCHW output [1,3,1024,1024]. H and W must equal kTileInput.
  void ForwardNchwReference(const float* input_nchw, float* output_nchw) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Hard-coded Metal MPSGraph X-Trans student DemosaicNet (`xtrans_p2_s32_d4`).
//
// Fixed product tile graph (batch N=2):
//   input  NHWC FP32 [2, 1048, 1048, 3]
//   graph  output NHWC FP32 [2, unpacked_h, unpacked_h, 6]
//          (channels 0..2 sparse input skip; channels 3..5 residual)
//   fused  Metal tail identical to Bayer with width=32.
class MetalXTransDemosaicNet {
 public:
  using Spec = DemosaicNetXTransSpec;

  static constexpr const char* kArchitecture       = Spec::kArchitecture;
  static constexpr int         kDepth              = Spec::kDepth;
  static constexpr int         kWidth              = Spec::kWidth;
  static constexpr int         kPackFactor         = Spec::kPackFactor;
  static constexpr int         kPackOutCh          = Spec::kPackOutCh;
  static constexpr int         kResidualCh         = Spec::kResidualCh;
  static constexpr int         kTileInput          = Spec::kTileInput;
  static constexpr int         kTileOutput         = Spec::kTileOutput;
  static constexpr int         kBatchSize          = 2;
  static constexpr int         kTileBorder         = Spec::kTileBorder;
  static constexpr int         kTilePad            = Spec::kTilePad;
  static constexpr int         kTileStep           = Spec::kTileStep;
  static constexpr int         kCfaPeriod          = Spec::kCfaPeriod;
  static constexpr int         kNaturalSpatialLoss = Spec::kNaturalSpatialLoss;
  static constexpr int         kSpatialLoss        = Spec::kSpatialLoss;
  static constexpr int         kMinSpatial         = Spec::kMinSpatial;
  static constexpr int         kMinProductOwned    = Spec::kMinProductOwned;

  MetalXTransDemosaicNet();
  ~MetalXTransDemosaicNet();

  MetalXTransDemosaicNet(const MetalXTransDemosaicNet&)            = delete;
  MetalXTransDemosaicNet& operator=(const MetalXTransDemosaicNet&) = delete;
  MetalXTransDemosaicNet(MetalXTransDemosaicNet&&) noexcept;
  MetalXTransDemosaicNet& operator=(MetalXTransDemosaicNet&&) noexcept;

  void LoadAndCompile(const nn::SafetensorsTensorMap& tensors, MTL::Device* device = nullptr);

  [[nodiscard]] auto ready() const -> bool;
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;
  [[nodiscard]] auto OwnedBufferBytes() const -> std::size_t;
  [[nodiscard]] auto compile_count() const -> std::uint64_t;
  [[nodiscard]] auto input_output_allocation_count() const -> std::uint64_t;
  [[nodiscard]] auto InputBuffer() const -> MTL::Buffer*;
  [[nodiscard]] auto OutputBuffer() const -> MTL::Buffer*;
  [[nodiscard]] auto CatHeight() const -> int;
  [[nodiscard]] auto FinalCrop() const -> int;

  void EncodeOnMpsCommandBuffer(void* mps_command_buffer) const;
  void EncodeFusedTailRgba(void* mtl_command_buffer, MTL::Texture* output_rgba,
                           const DemosaicNetFusedTailParams& params) const;

  void ClearLastEncodeError() const;
  [[nodiscard]] auto HasLastEncodeError() const -> bool;
  [[nodiscard]] auto LastEncodeErrorMessage() const -> std::string;

  void ForwardNchwReference(const float* input_nchw, float* output_nchw) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo

#endif  // HAVE_METAL
