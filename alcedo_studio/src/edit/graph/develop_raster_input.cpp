//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/develop_raster_input.hpp"

#include <algorithm>
#include <stdexcept>

namespace alcedo {
namespace {

auto MakeSdr(const std::array<float, 8>& primaries, RasterTransferKind kind, float gamma,
             const char* name) -> RasterColorDescription {
  RasterColorDescription description;
  description.referral_     = RasterReferral::DisplayReferred;
  description.primaries_xy_ = primaries;
  RasterTransfer transfer;
  transfer.kind_                   = kind;
  transfer.gamma_                  = gamma;
  description.transfer_            = {transfer, transfer, transfer};
  description.peak_luminance_nits_ = 100.0f;
  description.origin_              = RasterColorOrigin::DefaultSrgb;
  description.profile_description_ = name;
  return description;
}

}  // namespace

auto IsRasterInputProfileOverride(std::string_view value) -> bool {
  return std::find(kRasterInputProfileOverrides.begin(), kRasterInputProfileOverrides.end(),
                   value) != kRasterInputProfileOverrides.end();
}

auto ResolveEffectiveRasterDescription(const DevelopRasterInput& input) -> RasterColorDescription {
  const auto& name = input.profile_override_;
  if (name == "auto") {
    return input.source_color_;
  }
  if (name == "srgb") {
    return MakeSdr(kRasterPrimariesRec709, RasterTransferKind::SrgbPiecewise, 2.2f, "sRGB");
  }
  if (name == "display_p3") {
    return MakeSdr(kRasterPrimariesDisplayP3, RasterTransferKind::SrgbPiecewise, 2.2f,
                   "Display P3");
  }
  if (name == "adobe_rgb") {
    return MakeSdr(kRasterPrimariesAdobeRgb, RasterTransferKind::Gamma, kAdobeRgbGamma,
                   "Adobe RGB (1998)");
  }
  if (name == "rec2020") {
    return MakeSdr(kRasterPrimariesRec2020, RasterTransferKind::Bt1886, 2.4f, "Rec.2020");
  }
  if (name == "prophoto") {
    return MakeSdr(kRasterPrimariesProPhoto, RasterTransferKind::Gamma, 1.8f, "ProPhoto RGB");
  }
  if (name == "linear_rec709") {
    auto description =
        MakeSdr(kRasterPrimariesRec709, RasterTransferKind::Linear, 1.0f, "Linear Rec.709");
    description.referral_ = RasterReferral::SceneLinear;
    return description;
  }
  throw std::invalid_argument("unknown raster input profile override '" + name + "'");
}

auto DevelopRasterInputToJson(const DevelopRasterInput& input) -> nlohmann::json {
  nlohmann::json value;
  value["kind"]             = "raster";
  value["source_color"]     = RasterColorDescriptionToJson(input.source_color_);
  value["profile_override"] = input.profile_override_;
  return value;
}

auto DevelopRasterInputFromJson(const nlohmann::json& value) -> DevelopRasterInput {
  if (!value.is_object() || value.value("kind", std::string{}) != "raster") {
    throw std::invalid_argument("Develop input object must have kind \"raster\"");
  }
  if (!value.contains("source_color")) {
    throw std::invalid_argument("Develop input object lacks source_color");
  }
  DevelopRasterInput input;
  input.source_color_     = RasterColorDescriptionFromJson(value.at("source_color"));
  input.profile_override_ = value.value("profile_override", std::string{"auto"});
  if (!IsRasterInputProfileOverride(input.profile_override_)) {
    throw std::invalid_argument("Develop input object has unknown profile_override '" +
                                input.profile_override_ + "'");
  }
  return input;
}

}  // namespace alcedo
