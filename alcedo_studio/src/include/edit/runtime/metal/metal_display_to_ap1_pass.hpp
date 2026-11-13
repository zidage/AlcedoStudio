//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/display_to_ap1_output.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

/**
 * @brief Device buffer of a packed DisplayToAp1 parameter block (display_to_ap1_math.h layout).
 *
 * `Upload` writes the block only when its contents differ from the uploaded block, so a cached
 * ACES 2.0 inverse runtime is transferred once per key. Not thread-safe; the owning render
 * device serializes access.
 */
class MetalDisplayToAp1Parameters {
 public:
  /// @return The buffer that holds the block. @throws std::runtime_error on a Metal error.
  auto               Upload(MetalBackend& backend, MetalCommandContext& command_context,
                            std::span<const float> packed) -> const MetalBackend::Buffer&;
  [[nodiscard]] auto UploadCount() const -> std::uint64_t { return upload_count_; }

 private:
  MetalBackend::Buffer buffer_;
  std::vector<float>   uploaded_;
  std::uint64_t        upload_count_ = 0;
};

/**
 * @brief Encode the DisplayToAp1 kernel from RGBA32F @p src to @p dst on the compute encoder of
 * @p command_context.
 * @throws std::runtime_error when the encoder or the pipeline is missing.
 */
void EncodeMetalDisplayToAp1(MetalBackend& backend, MetalCommandContext& command_context,
                             const MetalBackend::Texture2D& src, MetalBackend::Texture2D& dst,
                             const MetalBackend::Buffer& params, DisplayToAp1Output output_kind,
                             std::uint32_t params_offset_bytes = 0);

/**
 * @brief Encode LinearizeRaster: tightly packed RGBA host-format pixels in @p source to the F32
 * RGBA texture @p dst, with the raster_linearize_math.h parameters in @p params.
 * @throws std::runtime_error for a CFA format or a missing encoder or pipeline.
 */
void EncodeMetalLinearizeRaster(MetalBackend& backend, MetalCommandContext& command_context,
                                const MetalBackend::Buffer& source, HostPixelFormat format,
                                const MetalBackend::Buffer& params, MetalBackend::Texture2D& dst);

}  // namespace alcedo
