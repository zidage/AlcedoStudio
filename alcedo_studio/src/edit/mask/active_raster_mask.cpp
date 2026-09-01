//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/active_raster_mask.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace alcedo {
namespace {

[[noreturn]] void FailActiveRaster(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

}  // namespace

auto ClipRasterDirtyRectangle(RectI rectangle, Extent2D extent) -> RectI {
  if (extent.Empty() || rectangle.width <= 0 || rectangle.height <= 0) {
    return {};
  }
  const auto x0 = std::max(rectangle.x, 0);
  const auto y0 = std::max(rectangle.y, 0);
  const auto x1 = std::min(rectangle.X1(), static_cast<std::int32_t>(extent.width));
  const auto y1 = std::min(rectangle.Y1(), static_cast<std::int32_t>(extent.height));
  return x1 > x0 && y1 > y0 ? RectI{x0, y0, x1 - x0, y1 - y0} : RectI{};
}

auto CopyPackedR8Rectangle(std::span<const std::uint8_t> pixels, Extent2D extent, RectI rectangle)
    -> std::vector<std::byte> {
  std::vector<std::byte> bytes(static_cast<std::size_t>(rectangle.width) *
                               static_cast<std::size_t>(rectangle.height));
  for (std::int32_t row = 0; row < rectangle.height; ++row) {
    const auto source = static_cast<std::size_t>(rectangle.y + row) * extent.width +
                        static_cast<std::size_t>(rectangle.x);
    std::copy_n(reinterpret_cast<const std::byte*>(pixels.data() + source), rectangle.width,
                bytes.data() + static_cast<std::size_t>(row) * rectangle.width);
  }
  return bytes;
}

auto FindActiveRasterMaskInput(std::span<const ActiveRasterMaskInput> inputs,
                               const NodeId& owner_node_id, const MaskId& mask_id)
    -> const ActiveRasterMaskInput* {
  for (const auto& input : inputs) {
    if (input.owner_node_id == owner_node_id && input.mask_id == mask_id) {
      return &input;
    }
  }
  return nullptr;
}

void ValidateActiveRasterMaskFields(std::span<const ActiveRasterMaskInput> inputs) {
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& input : inputs) {
    if (input.owner_node_id.Empty() || input.mask_id.Empty()) {
      FailActiveRaster("Active raster input requires NodeId and MaskId");
    }
    const auto identity =
        std::make_pair(std::string{input.owner_node_id.Value()}, std::string{input.mask_id.Value()});
    if (!seen.insert(identity).second) {
      FailActiveRaster("Active raster input duplicates NodeId and MaskId");
    }
    if (!input.pixels) {
      FailActiveRaster("Active raster input requires immutable R8 pixels");
    }
    ValidateMaskAssetPixels(input.descriptor, *input.pixels);
    const auto clipped = ClipRasterDirtyRectangle(input.dirty_rectangle, input.descriptor.extent);
    if (clipped.width <= 0 || clipped.height <= 0) {
      FailActiveRaster("Active raster input requires a non-empty dirty rectangle");
    }
  }
}

}  // namespace alcedo
