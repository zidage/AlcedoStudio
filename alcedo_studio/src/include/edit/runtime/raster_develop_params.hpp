//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host parameter blocks of the two raster develop steps: LinearizeRaster (in UploadRgb) and
// DisplayToAp1. Every backend packs through these functions, so the three backends read the same
// values (raster_image_input_plan.md, sections 5 and 6.2).

#pragma once

#include <memory>
#include <span>
#include <vector>

#include "edit/graph/develop_raster_input.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/drt/aces2_reference_runtime.hpp"

namespace alcedo {

/**
 * @brief LinearizeRaster parameters (raster_linearize_math.h layout) for @p description and the
 * host plane @p format.
 * @throws std::invalid_argument for a CFA format.
 */
[[nodiscard]] auto PackRasterLinearize(const RasterColorDescription& description,
                                       HostPixelFormat               format) -> std::vector<float>;

/// DisplayToAp1 parameters of one render: the cached ACES 2.0 inverse for display-referred input,
/// or the packed scene-linear matrix.
struct DisplayToAp1Block {
  std::shared_ptr<const Aces2ReferenceRuntime>    inverse_;
  std::array<float, ALCEDO_D2A_SCENE_PACKED_SIZE> scene_{};

  [[nodiscard]] auto                              Packed() const -> std::span<const float> {
    return inverse_ ? std::span<const float>(inverse_->packed_) : std::span<const float>(scene_);
  }
};

[[nodiscard]] auto ResolveDisplayToAp1Block(const RasterColorDescription& description)
    -> DisplayToAp1Block;

/// Parameter-arena slot of the DisplayToAp1 block on backends that bind it through the workspace
/// parameter arena (OpenCL, Metal).
inline const AdjustmentInstanceId kDevelopDisplayToAp1Slot{"display_to_ap1"};

/// Fixed-size arena copy of the DisplayToAp1 block (the scene-linear block is zero-padded).
struct DisplayToAp1ArenaBlock {
  float values[ALCEDO_D2A_PACKED_SIZE];
};

[[nodiscard]] auto MakeDisplayToAp1ArenaBlock(const DisplayToAp1Block& block)
    -> DisplayToAp1ArenaBlock;

/**
 * @brief Raster input of @p document, or throw.
 * @throws std::runtime_error naming @p caller when the document has no Develop node or no raster
 *         input object.
 */
[[nodiscard]] auto RequireRasterInput(const PipelineDocument& document, const char* caller)
    -> DevelopRasterInput;

/**
 * @brief Check that the prepared pixels match the document's description: a LUT-based ICC
 * description needs pixels converted by that profile (same SHA-256), and converted pixels need
 * that description.
 * @throws std::runtime_error naming @p caller on a mismatch.
 */
void RequireRasterPixelsMatchDescription(const PreparedRawInput&       input,
                                         const RasterColorDescription& description,
                                         const char*                   caller);

}  // namespace alcedo
