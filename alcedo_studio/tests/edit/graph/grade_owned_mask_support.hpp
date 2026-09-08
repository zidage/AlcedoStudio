//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo::grade_mask_test {

inline auto MakePaintStroke(std::string_view id, float local_x = 8.0f, float local_y = 12.0f,
                            float radius = 4.0f) -> BrushStroke {
  return MakeBrushStroke(StrokeId{std::string{id}}, BrushStrokeMode::Paint,
                         {{local_x, local_y, radius, 1.0f, 1.0f}});
}

inline auto MakeParameterizedBrushMask(MaskId id, std::vector<BrushStroke> strokes = {},
                                       Vector2 translation = {}) -> MaskModel {
  MaskModel mask;
  mask.id = std::move(id);
  BrushMaskSource brush;
  brush.strokes               = std::move(strokes);
  brush.placement_translation = translation;
  mask.source                 = std::move(brush);
  return mask;
}

inline auto MakeBrushMask(MaskId id, MaskAssetKey key, MaskAssetDescriptor descriptor = {},
                          float feather = 0.0f, bool invert = false) -> MaskModel {
  MaskModel mask;
  mask.id     = std::move(id);
  mask.invert = invert;
  BrushMaskSource brush;
  if (!key.Empty() && descriptor.extent.Empty()) {
    descriptor.extent = {1, 1};
  }
  brush.asset_key      = std::move(key);
  brush.descriptor     = descriptor;
  brush.feather_radius = feather;
  mask.source          = std::move(brush);
  return mask;
}

inline auto MakeBrushMask(MaskId id, const MaskAsset& asset, float feather = 0.0f,
                          bool invert = false) -> MaskModel {
  return MakeBrushMask(std::move(id), asset.key, asset.descriptor, feather, invert);
}

inline auto MakeRadialMask(MaskId id, RadialMaskSource source = {}, bool invert = false)
    -> MaskModel {
  MaskModel mask;
  mask.id     = std::move(id);
  mask.invert = invert;
  mask.source = std::move(source);
  return mask;
}

inline auto MakeLinearGradientMask(MaskId id, LinearGradientMaskSource source = {},
                                   bool invert = false) -> MaskModel {
  MaskModel mask;
  mask.id     = std::move(id);
  mask.invert = invert;
  mask.source = std::move(source);
  return mask;
}

inline auto AddMask(ColorGradeNodeModel& grade, MaskModel mask) -> MaskModel& {
  const auto id = mask.id;
  grade.AddMask(std::move(mask), grade.MaskCount());
  auto* found = grade.FindMask(id);
  return *found;
}

inline auto AddParameterizedBrushMask(PipelineDocument& document, MaskId id,
                                      std::vector<BrushStroke> strokes = {},
                                      Vector2 translation = {}) -> MaskModel& {
  auto& mask = AddMask(*document.PrimaryGrade(), MakeParameterizedBrushMask(
                                                     std::move(id), std::move(strokes), translation));
  document.MarkTopologyDirty();
  return mask;
}

inline auto AddBrushMask(PipelineDocument& document, MaskId id, MaskAssetKey key,
                         MaskAssetDescriptor descriptor = {}, float feather = 0.0f,
                         bool invert = false) -> MaskModel& {
  auto& mask = AddMask(*document.PrimaryGrade(),
                       MakeBrushMask(std::move(id), std::move(key), descriptor, feather, invert));
  document.MarkTopologyDirty();
  return mask;
}

inline auto AddRadialMask(PipelineDocument& document, MaskId id, RadialMaskSource source = {},
                          bool invert = false) -> MaskModel& {
  auto& mask =
      AddMask(*document.PrimaryGrade(), MakeRadialMask(std::move(id), std::move(source), invert));
  document.MarkTopologyDirty();
  return mask;
}

inline auto AddLinearGradientMask(PipelineDocument& document, MaskId id,
                                  LinearGradientMaskSource source = {}, bool invert = false)
    -> MaskModel& {
  auto& mask = AddMask(*document.PrimaryGrade(),
                       MakeLinearGradientMask(std::move(id), std::move(source), invert));
  document.MarkTopologyDirty();
  return mask;
}

}  // namespace alcedo::grade_mask_test
