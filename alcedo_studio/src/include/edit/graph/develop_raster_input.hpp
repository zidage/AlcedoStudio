//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Develop `input` object of a raster (JPEG, PNG, TIFF, OpenEXR) document
// (docs/roadmap/alcedo_studio/edit/raster_image_input_plan.md, section 7.3). Its presence makes
// the document raster; a RAW document never has it.

#pragma once

#include <array>
#include <json.hpp>
#include <string>
#include <string_view>

#include "image/raster_color_description.hpp"

namespace alcedo {

struct DevelopRasterInput {
  /// Description resolved from the file at import. Never edited.
  RasterColorDescription source_color_{};
  /// One of kRasterInputProfileOverrides. The only editable field.
  std::string            profile_override_                                   = "auto";

  auto                   operator==(const DevelopRasterInput&) const -> bool = default;
};

/// Values of `profile_override`, in menu order (decision D5).
inline constexpr std::array<std::string_view, 7> kRasterInputProfileOverrides = {
    "auto", "srgb", "display_p3", "adobe_rgb", "rec2020", "prophoto", "linear_rec709"};

[[nodiscard]] auto IsRasterInputProfileOverride(std::string_view value) -> bool;

/**
 * @brief Description that rendering uses: the file's description for `auto`, else the named
 * color space (display-referred SDR, or scene-linear Rec.709 for `linear_rec709`).
 * @throws std::invalid_argument for an unknown override.
 */
[[nodiscard]] auto ResolveEffectiveRasterDescription(const DevelopRasterInput& input)
    -> RasterColorDescription;

/// `{"kind":"raster","source_color":{...},"profile_override":"..."}`.
[[nodiscard]] auto DevelopRasterInputToJson(const DevelopRasterInput& input) -> nlohmann::json;

/// @throws std::invalid_argument when the object is not a valid raster input.
[[nodiscard]] auto DevelopRasterInputFromJson(const nlohmann::json& value) -> DevelopRasterInput;

}  // namespace alcedo
