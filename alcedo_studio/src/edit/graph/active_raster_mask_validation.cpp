//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/active_raster_mask_validation.hpp"

#include <stdexcept>
#include <string>
#include <variant>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

void ValidateActiveRasterMaskBindings(const PipelineDocument& document,
                                      std::span<const ActiveRasterMaskInput> inputs) {
  ValidateActiveRasterMaskFields(inputs);
  for (const auto& input : inputs) {
    const auto* grade =
        dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(input.owner_node_id));
    if (grade == nullptr) {
      throw std::runtime_error("Active raster input target Grade '" +
                               std::string{input.owner_node_id.Value()} + "' does not exist");
    }
    const auto* mask = grade->FindMask(input.mask_id);
    if (mask == nullptr) {
      throw std::runtime_error("Active raster input target Mask '" +
                               std::string{input.mask_id.Value()} + "' does not exist");
    }
    const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
    if (brush == nullptr) {
      throw std::runtime_error("Active raster input requires a Brush Mask source");
    }
    if (brush->asset_key.has_value() && !brush->asset_key->Empty()) {
      if (brush->descriptor != input.descriptor) {
        throw std::runtime_error("Active raster input descriptor does not match the Brush Mask");
      }
    } else if (!brush->descriptor.extent.Empty() && brush->descriptor != input.descriptor) {
      throw std::runtime_error("Active raster input descriptor does not match the Brush Mask");
    }
  }
}

}  // namespace alcedo
