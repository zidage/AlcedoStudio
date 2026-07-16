//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>
#include <memory>

#include <opencv2/core/types.hpp>

#include "decoders/processor/nn/metal_demosaicnet_module.hpp"
#include "image/metal_image.hpp"

namespace MTL {
class Buffer;
class Texture;
}  // namespace MTL

namespace alcedo {

// Product-path tile orchestration for Metal MPSGraph DemosaicNet.
//
// Reads the original R32F CFA texture directly (no full-frame HWC3 staging),
// encodes fixed student tiles into the module's reusable private input buffer,
// runs the compiled graph through residual/skip concat, then the fused Metal
// post/output/gamma tail writes owned ROIs into a crop-sized RGBA texture.
// One MPSCommandBuffer carries every tile; the tile loop never waits.

struct MetalDemosaicNetTiledDispatch {
  // Full-frame linear CFA (R32FLOAT). Phase crop is applied in the input kernel.
  const metal::MetalImage* cfa_image = nullptr;

  // Caller-owned crop-sized RGBA32FLOAT result. Must already exist with size
  // product_crop.width × product_crop.height.
  metal::MetalImage* output_rgba = nullptr;

  // Phase-align geometry in original CFA coordinates / aligned lattice size.
  int shift_sx       = 0;
  int shift_sy       = 0;
  int aligned_width  = 0;
  int aligned_height = 0;

  // Product crop in aligned-lattice coordinates. Output texture pixels map 1:1
  // to this rectangle (origin → texture (0,0)).
  cv::Rect product_crop;

  // When true (default), commit and wait once after all tiles. Tests may set
  // false only when they intentionally keep the command buffer live.
  bool commit_and_wait = true;
};

struct MetalDemosaicNetTiledResult {
  std::size_t   tile_count             = 0;
  std::size_t   graph_invocation_count = 0;
  std::size_t   padded_tile_count      = 0;
  std::uint64_t host_wait_count        = 0;
  std::uint64_t tile_encode_count      = 0;
};

// Shared parameter layouts for demosaicnet_io.metal (must match shader structs).
struct DemosaicNetTileInputParams {
  int batch_index = 0;
  int origin_x   = 0;
  int origin_y   = 0;
  int tile_w     = 0;
  int tile_h     = 0;
  int aligned_w  = 0;
  int aligned_h  = 0;
  int shift_sx   = 0;
  int shift_sy   = 0;
  int full_w     = 0;
  int full_h     = 0;
  int period     = 0;
  int rgb_fc[36] = {};
};

struct DemosaicNetTileOutputParams {
  int batch_index = 0;
  int tile_w  = 0;
  int tile_h  = 0;
  int src_x0  = 0;
  int src_y0  = 0;
  int owned_w = 0;
  int owned_h = 0;
  int dst_x   = 0;
  int dst_y   = 0;
  int crop_x  = 0;
  int crop_y  = 0;
  int crop_w  = 0;
  int crop_h  = 0;
};

// Fill training-origin rgb_fc table for Bayer (period 2) or X-Trans (period 6).
void FillDemosaicNetTrainingRgbFc(bool is_xtrans, DemosaicNetTileInputParams& params);

// Encode only the tile input kernel into a Metal command buffer (ObjC id as void*).
// Used by the tiled executor and by synthetic IO tests.
void EncodeDemosaicNetTileInput(void* mtl_command_buffer, MTL::Texture* cfa_texture,
                                MTL::Buffer* tile_nhwc, const DemosaicNetTileInputParams& params);

// Encode only the tile output assembly kernel.
void EncodeDemosaicNetTileOutput(void* mtl_command_buffer, MTL::Buffer* tile_nhwc,
                                 MTL::Texture* output_rgba,
                                 const DemosaicNetTileOutputParams& params);

// Test instrumentation: host waits performed by Enqueue* when commit_and_wait is true.
void ResetMetalDemosaicNetHostWaitCountForTest();
[[nodiscard]] auto MetalDemosaicNetHostWaitCountForTest() noexcept -> std::uint64_t;

class MetalDemosaicNetTiledExecutor {
 public:
  MetalDemosaicNetTiledExecutor();
  ~MetalDemosaicNetTiledExecutor();

  MetalDemosaicNetTiledExecutor(const MetalDemosaicNetTiledExecutor&)            = delete;
  MetalDemosaicNetTiledExecutor& operator=(const MetalDemosaicNetTiledExecutor&) = delete;
  MetalDemosaicNetTiledExecutor(MetalDemosaicNetTiledExecutor&&) noexcept;
  MetalDemosaicNetTiledExecutor& operator=(MetalDemosaicNetTiledExecutor&&) noexcept;

  // Throws std::runtime_error with stage=prepare|tile_input|graph_encode|graph_execute|tile_output.
  // Does not allocate full-frame HWC3 or full-frame RGB staging.
  [[nodiscard]] auto EnqueueBayer(const MetalBayerDemosaicNet&         module,
                                  const MetalDemosaicNetTiledDispatch& dispatch)
      -> MetalDemosaicNetTiledResult;

  [[nodiscard]] auto EnqueueXTrans(const MetalXTransDemosaicNet&        module,
                                   const MetalDemosaicNetTiledDispatch& dispatch)
      -> MetalDemosaicNetTiledResult;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo

#endif  // HAVE_METAL
