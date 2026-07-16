//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>
#include <memory>

#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "nn/safetensors.hpp"

namespace MTL {
class Buffer;
class Device;
}  // namespace MTL

namespace alcedo {

// Hard-coded Metal MPSGraph Bayer student DemosaicNet (`bayer_s24_d8`).
//
// Fixed product tile graph only:
//   input  NHWC FP32 [1, 1086, 1086, 3]
//   output NHWC FP32 [1, 1024, 1024, 3]
//
// Public headers stay free of MPSGraph / Foundation types. Objective-C++ owns
// the compiled executable and tensor-data wrappers inside the PIMPL.
// Signed gamma decode is intentionally not applied here; that lives in the
// Phase 3 tile-assembly Metal kernel.
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

  // Validate metadata/arrays, build the fixed FP32 graph, compile once, and
  // allocate reusable private input/output tile buffers. Throws on failure and
  // leaves the module not-ready (no partial publication).
  void LoadAndCompile(const nn::SafetensorsTensorMap& tensors, MTL::Device* device = nullptr);

  [[nodiscard]] auto ready() const -> bool;

  // Application-owned resident bytes for weight constants (host copy size of
  // learned + fixed arrays consumed by the graph). Does not include MPSGraph
  // internal activations.
  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Caller-owned fixed tile input + output private buffers.
  [[nodiscard]] auto OwnedBufferBytes() const -> std::size_t;

  [[nodiscard]] auto compile_count() const -> std::uint64_t;
  [[nodiscard]] auto input_output_allocation_count() const -> std::uint64_t;

  // Reusable private FP32 tile buffers (null when not ready).
  [[nodiscard]] auto InputBuffer() const -> MTL::Buffer*;
  [[nodiscard]] auto OutputBuffer() const -> MTL::Buffer*;

  // Encode the compiled executable into an MPSCommandBuffer (ObjC pointer).
  // Does not commit or wait. Input buffer must already hold the fixed NHWC tile.
  // Results land in OutputBuffer via the bound results array.
  void EncodeOnMpsCommandBuffer(void* mps_command_buffer) const;

  // Synchronous host reference path for module tests:
  // NCHW input [1,3,H,W] → NHWC graph → NCHW output [1,3,1024,1024].
  // No gamma decode. H and W must equal kTileInput.
  void ForwardNchwReference(const float* input_nchw, float* output_nchw) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Hard-coded Metal MPSGraph X-Trans student DemosaicNet (`xtrans_p2_s32_d4`).
//
// Fixed product tile graph only:
//   input  NHWC FP32 [1, 1048, 1048, 3]
//   output NHWC FP32 [1, 1024, 1024, 3]
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

  void EncodeOnMpsCommandBuffer(void* mps_command_buffer) const;
  void ForwardNchwReference(const float* input_nchw, float* output_nchw) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo

#endif  // HAVE_METAL
