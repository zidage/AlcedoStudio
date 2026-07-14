//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <memory>

#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "nn/safetensors.hpp"
#include "opencl/nn/activation_slots.hpp"
#include "opencl/nn/device_buffer.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo {

// Hard-coded OpenCL Bayer student DemosaicNet (`bayer_s24_d8`).
// Topology matches CUDA BayerDemosaicNet and bundled bayer.safetensors.
//
// Input:  contiguous NCHW f32 mosaic [N, 3, H, W] on device (batch N==1).
// Output: contiguous HWC RGB f32 on device, size OutputHeight × OutputWidth.
// Weights immutable after LoadWeights. Caller owns ActivationSlots (two grow-only
// ping-pong buffers); the hot path never creates OpenCL sub-buffers.
class OpenClBayerDemosaicNet {
 public:
  using Spec                                       = DemosaicNetBayerSpec;

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

  OpenClBayerDemosaicNet();
  ~OpenClBayerDemosaicNet();

  OpenClBayerDemosaicNet(const OpenClBayerDemosaicNet&)            = delete;
  OpenClBayerDemosaicNet& operator=(const OpenClBayerDemosaicNet&) = delete;
  OpenClBayerDemosaicNet(OpenClBayerDemosaicNet&&) noexcept;
  OpenClBayerDemosaicNet& operator=(OpenClBayerDemosaicNet&&) noexcept;

  // Validates metadata/topology, packs OHWI4o4i weights, uploads once, creates kernels once.
  void LoadWeights(const nn::SafetensorsTensorMap& tensors, cl_command_queue queue = nullptr);

  [[nodiscard]] auto weights_loaded() const -> bool;

  // Enqueues full fixed-network forward on the in-order queue. Does not clFinish.
  // `output_rgb_hwc` must hold OutputHeight(H,W) * OutputWidth(W,H) * 3 floats.
  void ForwardNchwToHwc(cl_mem input_nchw, int batch, int height, int width, cl_mem output_rgb_hwc,
                        opencl::nn::ActivationSlots& activation_slots,
                        cl_command_queue queue = nullptr, bool apply_gamma_decode = true) const;

  // Product tile forward: reflect-101 and signed-gamma encode the phase-aligned
  // HWC3 frame directly into the first NHWC4 activation, then enqueue the same
  // fixed network. The input frame and output tile remain device resident.
  void ForwardReflectHwc3ToHwc(cl_mem input_frame_hwc3, int frame_height, int frame_width,
                               int origin_y, int origin_x, int tile_height, int tile_width,
                               cl_mem output_rgb_hwc, opencl::nn::ActivationSlots& activation_slots,
                               cl_command_queue queue              = nullptr,
                               bool             apply_gamma_decode = true) const;

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

  // Bytes required for each of the two ping-pong activation slots.
  [[nodiscard]] static auto EstimateActivationSlotBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  // Total capacity for both slots (2 × EstimateActivationSlotBytes).
  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t {
    return 2 * EstimateActivationSlotBytes(input_h, input_w, batch);
  }

  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  // Stable device handles for no-reload tests (null if empty).
  [[nodiscard]] auto Trunk0WeightBuffer() const -> cl_mem;
  [[nodiscard]] auto OutputWeightBuffer() const -> cl_mem;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Hard-coded OpenCL X-Trans student DemosaicNet (`xtrans_p2_s32_d4`).
class OpenClXTransDemosaicNet {
 public:
  using Spec                                       = DemosaicNetXTransSpec;

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

  OpenClXTransDemosaicNet();
  ~OpenClXTransDemosaicNet();

  OpenClXTransDemosaicNet(const OpenClXTransDemosaicNet&)            = delete;
  OpenClXTransDemosaicNet& operator=(const OpenClXTransDemosaicNet&) = delete;
  OpenClXTransDemosaicNet(OpenClXTransDemosaicNet&&) noexcept;
  OpenClXTransDemosaicNet& operator=(OpenClXTransDemosaicNet&&) noexcept;

  void LoadWeights(const nn::SafetensorsTensorMap& tensors, cl_command_queue queue = nullptr);

  [[nodiscard]] auto weights_loaded() const -> bool;

  void ForwardNchwToHwc(cl_mem input_nchw, int batch, int height, int width, cl_mem output_rgb_hwc,
                        opencl::nn::ActivationSlots& activation_slots,
                        cl_command_queue queue = nullptr, bool apply_gamma_decode = true) const;

  void ForwardReflectHwc3ToHwc(cl_mem input_frame_hwc3, int frame_height, int frame_width,
                               int origin_y, int origin_x, int tile_height, int tile_width,
                               cl_mem output_rgb_hwc, opencl::nn::ActivationSlots& activation_slots,
                               cl_command_queue queue              = nullptr,
                               bool             apply_gamma_decode = true) const;

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

  [[nodiscard]] static auto EstimateActivationSlotBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t;

  [[nodiscard]] static auto EstimateWorkspaceBytes(int input_h, int input_w, int batch = 1)
      -> std::size_t {
    return 2 * EstimateActivationSlotBytes(input_h, input_w, batch);
  }

  [[nodiscard]] auto ResidentWeightBytes() const -> std::size_t;

  [[nodiscard]] auto Trunk0WeightBuffer() const -> cl_mem;
  [[nodiscard]] auto OutputWeightBuffer() const -> cl_mem;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
