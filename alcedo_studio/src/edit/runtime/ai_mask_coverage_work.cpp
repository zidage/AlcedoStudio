//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/ai_mask_coverage_work.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "edit/mask/ai_mask_coverage_texture.hpp"

namespace alcedo {
namespace {

[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error("AI Mask coverage: " + message);
}

auto ExtentText(Extent2D extent) -> std::string {
  return std::to_string(extent.width) + " x " + std::to_string(extent.height);
}

auto DescriptorOf(RectI rect, Extent2D image) -> MaskAssetDescriptor {
  MaskAssetDescriptor descriptor;
  descriptor.extent =
      Extent2D{static_cast<std::uint32_t>(rect.width), static_cast<std::uint32_t>(rect.height)};
  descriptor.reference_bounds =
      NormalizedRect{static_cast<float>(rect.x) / static_cast<float>(image.width),
                     static_cast<float>(rect.y) / static_cast<float>(image.height),
                     static_cast<float>(rect.width) / static_cast<float>(image.width),
                     static_cast<float>(rect.height) / static_cast<float>(image.height)};
  return descriptor;
}

}  // namespace

auto AiMaskCoverageWork::OutputDescriptor() const -> MaskAssetDescriptor {
  return HasEdgeAdjustment() ? DescriptorOf(output_rect, analysis_extent) : source;
}

auto MakeAiMaskCoverageWork(const MaskAssetDescriptor& source, Extent2D analysis_extent,
                            const AiMaskEdgeAdjustment& edge) -> AiMaskCoverageWork {
  if (analysis_extent.Empty() || analysis_extent.width > kMaximumRasterMaskAxis ||
      analysis_extent.height > kMaximumRasterMaskAxis) {
    Fail("the analysis image size " + ExtentText(analysis_extent) + " is not in [1, " +
         std::to_string(kMaximumRasterMaskAxis) + "]");
  }
  const auto rect = AiMaskAssetPixelRect(source, analysis_extent);
  if (!rect.has_value()) {
    Fail("the " + ExtentText(source.extent) + " coverage does not lie in the " +
         ExtentText(analysis_extent) + " analysis image");
  }
  std::int32_t margin = 0;
  try {
    margin = AiMaskEdgeMargin(edge);
  } catch (const std::runtime_error& error) {
    Fail(error.what());
  }

  AiMaskCoverageWork work;
  work.source          = source;
  work.analysis_extent = analysis_extent;
  work.source_rect     = *rect;
  work.edge            = edge;
  if (!work.HasEdgeAdjustment()) {
    work.output_rect = *rect;
    return work;
  }
  const auto image_width  = static_cast<std::int32_t>(analysis_extent.width);
  const auto image_height = static_cast<std::int32_t>(analysis_extent.height);
  const auto left         = std::max(rect->x - margin, 0);
  const auto top          = std::max(rect->y - margin, 0);
  const auto right        = std::min(rect->X1() + margin, image_width);
  const auto bottom       = std::min(rect->Y1() + margin, image_height);
  work.output_rect        = RectI{left, top, right - left, bottom - top};
  return work;
}

auto AiMaskCandidateDescriptor(Extent2D analysis_extent) -> MaskAssetDescriptor {
  return MaskAssetDescriptor{analysis_extent, NormalizedRect{0.0f, 0.0f, 1.0f, 1.0f}};
}

}  // namespace alcedo
