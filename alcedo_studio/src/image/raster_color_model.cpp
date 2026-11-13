//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Value operations of RasterColorDescription that need no file parsing: transfer evaluation,
// the usability check, and the JSON form. Kept apart from the file resolver so that the
// pipeline document can use them without the image decoding libraries.

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include "color/color_encoding_math.h"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

using json = nlohmann::json;
// -------------------------------------------------------------------------------------------
// JSON names
// -------------------------------------------------------------------------------------------

template <typename Enum>
struct EnumName {
  Enum        value_;
  const char* name_;
};

constexpr std::array<EnumName<RasterReferral>, 2>     kReferralNames = {{
    {RasterReferral::DisplayReferred, "display_referred"},
    {RasterReferral::SceneLinear, "scene_linear"},
}};

constexpr std::array<EnumName<RasterTransferKind>, 8> kTransferNames = {{
    {RasterTransferKind::Linear, "linear"},
    {RasterTransferKind::SrgbPiecewise, "srgb_piecewise"},
    {RasterTransferKind::Gamma, "gamma"},
    {RasterTransferKind::Bt1886, "bt1886"},
    {RasterTransferKind::IccParametric, "icc_parametric"},
    {RasterTransferKind::IccSampled, "icc_sampled"},
    {RasterTransferKind::St2084, "st2084"},
    {RasterTransferKind::Hlg, "hlg"},
}};

constexpr std::array<EnumName<RasterColorOrigin>, 10> kOriginNames   = {{
    {RasterColorOrigin::IccMatrixShaper, "icc_matrix_shaper"},
    {RasterColorOrigin::IccLutConverted, "icc_lut_converted"},
    {RasterColorOrigin::IccCicp, "icc_cicp"},
    {RasterColorOrigin::PngCicp, "png_cicp"},
    {RasterColorOrigin::PngSrgbChunk, "png_srgb_chunk"},
    {RasterColorOrigin::PngGamaChrm, "png_gama_chrm"},
    {RasterColorOrigin::ExrChromaticities, "exr_chromaticities"},
    {RasterColorOrigin::ExrAcesContainer, "exr_aces_container"},
    {RasterColorOrigin::ExifInteropAdobeRgb, "exif_interop_adobe_rgb"},
    {RasterColorOrigin::DefaultSrgb, "default_srgb"},
}};

template <typename Enum, std::size_t N>
auto NameOf(const std::array<EnumName<Enum>, N>& names, Enum value) -> const char* {
  for (const auto& entry : names) {
    if (entry.value_ == value) {
      return entry.name_;
    }
  }
  throw std::invalid_argument("raster color enum value has no JSON name");
}

template <typename Enum, std::size_t N>
auto ValueOf(const std::array<EnumName<Enum>, N>& names, const json& value, const char* key)
    -> Enum {
  if (!value.is_string()) {
    throw std::invalid_argument(std::string("raster color key '") + key + "' is not a string");
  }
  const auto text = value.get<std::string>();
  for (const auto& entry : names) {
    if (text == entry.name_) {
      return entry.value_;
    }
  }
  throw std::invalid_argument(std::string("raster color key '") + key + "' has unknown value '" +
                              text + "'");
}

auto Require(const json& object, const char* key) -> const json& {
  if (!object.is_object() || !object.contains(key)) {
    throw std::invalid_argument(std::string("raster color JSON lacks key '") + key + "'");
  }
  return object.at(key);
}

auto RequireFloat(const json& value, const char* key) -> float {
  if (!value.is_number()) {
    throw std::invalid_argument(std::string("raster color key '") + key + "' is not a number");
  }
  return value.get<float>();
}

auto ParametricParameterCount(uint8_t function_type) -> std::size_t {
  constexpr std::array<std::size_t, 5> kCount = {1, 3, 4, 5, 7};
  return function_type < kCount.size() ? kCount[function_type] : 0;
}

auto TransferToJson(const RasterTransfer& transfer) -> json {
  json value;
  value["kind"] = NameOf(kTransferNames, transfer.kind_);
  if (transfer.kind_ == RasterTransferKind::Gamma) {
    value["gamma"] = transfer.gamma_;
  } else if (transfer.kind_ == RasterTransferKind::IccParametric) {
    value["function_type"] = transfer.icc_parametric_type_;
    json params            = json::array();
    for (std::size_t i = 0; i < ParametricParameterCount(transfer.icc_parametric_type_); ++i) {
      params.push_back(transfer.icc_params_[i]);
    }
    value["params"] = std::move(params);
  } else if (transfer.kind_ == RasterTransferKind::IccSampled) {
    value["samples"] = transfer.sampled_;
  }
  return value;
}

auto TransferFromJson(const json& value) -> RasterTransfer {
  RasterTransfer transfer;
  transfer.kind_ = ValueOf(kTransferNames, Require(value, "kind"), "kind");
  if (transfer.kind_ == RasterTransferKind::Gamma) {
    transfer.gamma_ = RequireFloat(Require(value, "gamma"), "gamma");
  } else if (transfer.kind_ == RasterTransferKind::IccParametric) {
    const auto& type = Require(value, "function_type");
    if (!type.is_number_unsigned() || type.get<unsigned>() > 4) {
      throw std::invalid_argument("raster color key 'function_type' must be 0..4");
    }
    transfer.icc_parametric_type_ = static_cast<uint8_t>(type.get<unsigned>());
    const auto& params            = Require(value, "params");
    if (!params.is_array() ||
        params.size() != ParametricParameterCount(transfer.icc_parametric_type_)) {
      throw std::invalid_argument("raster color key 'params' has the wrong size");
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
      transfer.icc_params_[i] = RequireFloat(params[i], "params");
    }
  } else if (transfer.kind_ == RasterTransferKind::IccSampled) {
    const auto& samples = Require(value, "samples");
    if (!samples.is_array() || samples.size() != kRasterSampledTransferEntries) {
      throw std::invalid_argument("raster color key 'samples' must have 4096 entries");
    }
    transfer.sampled_.reserve(samples.size());
    for (const auto& sample : samples) {
      transfer.sampled_.push_back(RequireFloat(sample, "samples"));
    }
  }
  return transfer;
}

// -------------------------------------------------------------------------------------------
// Curves
// -------------------------------------------------------------------------------------------

auto SafePow(double base, double exponent) -> double {
  return base <= 0.0 ? 0.0 : std::pow(base, exponent);
}

auto EvaluateIccParametric(const RasterTransfer& transfer, double x) -> double {
  const auto&  p = transfer.icc_params_;
  const double g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
  switch (transfer.icc_parametric_type_) {
    case 0:
      return SafePow(x, g);
    case 1:
      return (a != 0.0 && x >= -b / a) ? SafePow(a * x + b, g) : 0.0;
    case 2:
      return (a != 0.0 && x >= -b / a) ? SafePow(a * x + b, g) + c : c;
    case 3:
      return x >= d ? SafePow(a * x + b, g) : c * x;
    case 4:
      return x >= d ? SafePow(a * x + b, g) + e : c * x + f;
    default:
      return x;
  }
}

}  // namespace

auto EvaluateRasterTransfer(const RasterTransfer& transfer, float encoded) -> float {
  const double x = encoded;
  switch (transfer.kind_) {
    case RasterTransferKind::Linear:
      return encoded;
    case RasterTransferKind::SrgbPiecewise:
      return CeSrgbDecode(encoded);
    case RasterTransferKind::Gamma:
      return CeGammaDecode(encoded, transfer.gamma_);
    case RasterTransferKind::Bt1886:
      return CeGammaDecode(encoded, CE_BT1886_GAMMA);
    case RasterTransferKind::IccParametric:
      return static_cast<float>(EvaluateIccParametric(transfer, x));
    case RasterTransferKind::IccSampled: {
      if (transfer.sampled_.empty()) {
        return 0.0f;
      }
      const double position =
          std::clamp(x, 0.0, 1.0) * static_cast<double>(transfer.sampled_.size() - 1);
      const auto   lower = static_cast<std::size_t>(position);
      const auto   upper = std::min(lower + 1, transfer.sampled_.size() - 1);
      const double t     = position - static_cast<double>(lower);
      return static_cast<float>(transfer.sampled_[lower] * (1.0 - t) +
                                transfer.sampled_[upper] * t);
    }
    case RasterTransferKind::St2084:
      return CePqDecode(std::min(encoded, 1.0f));
    case RasterTransferKind::Hlg:
      // Gray axis of the 1000-nit reference display: OOTF gain of a neutral signal.
      return CeGammaDecode(CeHlgDecode(std::min(encoded, 1.0f)), CE_HLG_OOTF_GAMMA);
  }
  return encoded;
}

auto FindRasterColorDescriptionDefect(const RasterColorDescription& description)
    -> std::optional<std::string> {
  const auto& p = description.primaries_xy_;
  for (const float value : p) {
    if (!std::isfinite(value)) {
      return "primaries contain a non-finite value";
    }
  }
  const double area2 = (static_cast<double>(p[2]) - p[0]) * (static_cast<double>(p[5]) - p[1]) -
                       (static_cast<double>(p[4]) - p[0]) * (static_cast<double>(p[3]) - p[1]);
  if (std::abs(area2) < 1e-5) {
    return "primaries are degenerate (their triangle has no area)";
  }
  if (p[7] <= 0.0f) {
    return "white point has a non-positive y";
  }
  // Barycentric coordinates of the white point (x, y) in the triangle R, G, B.
  const double xr = p[0], yr = p[1], xg = p[2], yg = p[3], xb = p[4], yb = p[5];
  const double xw = p[6], yw = p[7];
  const double denominator = (yg - yb) * (xr - xb) + (xb - xg) * (yr - yb);
  const double l1          = ((yg - yb) * (xw - xb) + (xb - xg) * (yw - yb)) / denominator;
  const double l2          = ((yb - yr) * (xw - xb) + (xr - xb) * (yw - yb)) / denominator;
  const double l3          = 1.0 - l1 - l2;
  if (l1 < -1e-4 || l2 < -1e-4 || l3 < -1e-4) {
    return "white point lies outside the primaries triangle";
  }
  if (!std::isfinite(description.peak_luminance_nits_) ||
      description.peak_luminance_nits_ <= 0.0f) {
    return "peak luminance is not positive";
  }
  constexpr std::array<const char*, 3> kChannelNames = {"red", "green", "blue"};
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const auto& transfer = description.transfer_[channel];
    if (transfer.kind_ == RasterTransferKind::Gamma &&
        (!std::isfinite(transfer.gamma_) || transfer.gamma_ <= 0.0f)) {
      return std::string("transfer exponent of the ") + kChannelNames[channel] +
             " channel is not positive";
    }
    if (transfer.kind_ == RasterTransferKind::IccSampled &&
        transfer.sampled_.size() != kRasterSampledTransferEntries) {
      return std::string("sampled transfer of the ") + kChannelNames[channel] +
             " channel does not have 4096 entries";
    }
    constexpr int kSteps    = 1024;
    float         previous  = EvaluateRasterTransfer(transfer, 0.0f);
    const float   first     = previous;
    bool          monotonic = std::isfinite(previous);
    for (int i = 1; i <= kSteps && monotonic; ++i) {
      const float value = EvaluateRasterTransfer(transfer, static_cast<float>(i) / kSteps);
      monotonic         = std::isfinite(value) && value >= previous - 1e-6f;
      previous          = value;
    }
    if (!monotonic || !(previous > first)) {
      return std::string("transfer curve of the ") + kChannelNames[channel] +
             " channel is not monotonic increasing";
    }
  }
  return std::nullopt;
}

auto RasterColorDescriptionToJson(const RasterColorDescription& description) -> json {
  json value;
  value["referral"]     = NameOf(kReferralNames, description.referral_);
  value["primaries_xy"] = description.primaries_xy_;
  json       transfer   = json::array();
  const bool uniform    = description.transfer_[0] == description.transfer_[1] &&
                       description.transfer_[0] == description.transfer_[2];
  for (std::size_t channel = 0; channel < (uniform ? 1u : 3u); ++channel) {
    transfer.push_back(TransferToJson(description.transfer_[channel]));
  }
  value["transfer"]            = std::move(transfer);
  value["peak_luminance_nits"] = description.peak_luminance_nits_;
  value["origin"]              = NameOf(kOriginNames, description.origin_);
  if (!description.profile_description_.empty()) {
    value["profile_description"] = description.profile_description_;
  }
  if (!description.icc_sha256_.empty()) {
    value["icc_sha256"] = description.icc_sha256_;
  }
  if (!description.default_reason_.empty()) {
    value["default_reason"] = description.default_reason_;
  }
  return value;
}

auto RasterColorDescriptionFromJson(const json& value) -> RasterColorDescription {
  RasterColorDescription description;
  description.referral_ = ValueOf(kReferralNames, Require(value, "referral"), "referral");
  const auto& primaries = Require(value, "primaries_xy");
  if (!primaries.is_array() || primaries.size() != 8) {
    throw std::invalid_argument("raster color key 'primaries_xy' must have 8 numbers");
  }
  for (std::size_t i = 0; i < 8; ++i) {
    description.primaries_xy_[i] = RequireFloat(primaries[i], "primaries_xy");
  }
  const auto& transfer = Require(value, "transfer");
  if (!transfer.is_array() || (transfer.size() != 1 && transfer.size() != 3)) {
    throw std::invalid_argument("raster color key 'transfer' must have 1 or 3 entries");
  }
  for (std::size_t channel = 0; channel < 3; ++channel) {
    description.transfer_[channel] = TransferFromJson(transfer[transfer.size() == 1 ? 0 : channel]);
  }
  description.peak_luminance_nits_ =
      RequireFloat(Require(value, "peak_luminance_nits"), "peak_luminance_nits");
  description.origin_              = ValueOf(kOriginNames, Require(value, "origin"), "origin");
  description.profile_description_ = value.value("profile_description", std::string{});
  description.icc_sha256_          = value.value("icc_sha256", std::string{});
  description.default_reason_      = value.value("default_reason", std::string{});
  return description;
}

}  // namespace alcedo
