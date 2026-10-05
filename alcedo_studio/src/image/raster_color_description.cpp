//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/raster_color_description.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include "image/icc_profile_reader.hpp"
#include "image/raster_container_reader.hpp"
#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

using json = nlohmann::json;

// -------------------------------------------------------------------------------------------
// Description builders
// -------------------------------------------------------------------------------------------

auto MakeTransfer(RasterTransferKind kind, float gamma = 2.2f) -> RasterTransfer {
  RasterTransfer transfer;
  transfer.kind_  = kind;
  transfer.gamma_ = gamma;
  return transfer;
}

auto MakeDescription(const std::array<float, 8>& primaries, const RasterTransfer& transfer,
                     RasterColorOrigin origin, RasterReferral referral) -> RasterColorDescription {
  RasterColorDescription description;
  description.referral_     = referral;
  description.primaries_xy_ = primaries;
  description.transfer_     = {transfer, transfer, transfer};
  description.origin_       = origin;
  return description;
}

void AppendReason(std::string& reasons, std::string_view reason) {
  if (!reasons.empty()) {
    reasons += "; ";
  }
  reasons += reason;
}

auto SrgbDefault(std::string reason) -> RasterColorDescription {
  auto description =
      MakeDescription(kRasterPrimariesRec709, MakeTransfer(RasterTransferKind::SrgbPiecewise),
                      RasterColorOrigin::DefaultSrgb, RasterReferral::DisplayReferred);
  description.default_reason_ = std::move(reason);
  return description;
}

auto LinearRec709Default(std::string reason) -> RasterColorDescription {
  auto description =
      MakeDescription(kRasterPrimariesRec709, MakeTransfer(RasterTransferKind::Linear),
                      RasterColorOrigin::DefaultSrgb, RasterReferral::SceneLinear);
  description.default_reason_ = std::move(reason);
  return description;
}

/// Exponent with the rounding of a fixed-point encoding removed (2.19998 -> 2.2).
auto SnapExponent(double exponent, double tolerance) -> float {
  const double snapped = std::round(exponent * 100.0) / 100.0;
  return static_cast<float>(std::abs(snapped - exponent) < tolerance ? snapped : exponent);
}

auto GammaOrLinear(float exponent) -> RasterTransfer {
  return std::abs(exponent - 1.0f) < 1e-4f ? MakeTransfer(RasterTransferKind::Linear)
                                           : MakeTransfer(RasterTransferKind::Gamma, exponent);
}

/// Peak luminance of a display-referred signal (plan section 4.2).
auto PeakLuminanceFor(RasterTransferKind kind, std::optional<float> mastering_max_nits,
                      std::optional<float> max_content_light_nits) -> float {
  if (kind == RasterTransferKind::St2084) {
    float peak = 1000.0f;
    if (mastering_max_nits && *mastering_max_nits > 0.0f) {
      peak = *mastering_max_nits;
    } else if (max_content_light_nits && *max_content_light_nits > 0.0f) {
      peak = *max_content_light_nits;
    }
    return std::clamp(peak, 100.0f, 10000.0f);
  }
  if (kind == RasterTransferKind::Hlg) {
    return 1000.0f;
  }
  return 100.0f;
}

/// Description from ITU-T H.273 code points (PNG cICP or the ICC v4.4 `cicp` tag).
auto DescribeCicp(const std::array<uint8_t, 4>& cicp, RasterColorOrigin origin,
                  std::optional<float> mastering_max_nits,
                  std::optional<float> max_content_light_nits, std::string& reasons)
    -> std::optional<RasterColorDescription> {
  const auto [primaries_code, transfer_code, matrix_code, full_range] = cicp;
  if (matrix_code != 0) {
    AppendReason(reasons, "CICP matrix coefficients are not 0 (RGB)");
    return std::nullopt;
  }
  if (full_range != 1) {
    AppendReason(reasons, "CICP signal is narrow range, which raster input does not support");
    return std::nullopt;
  }
  std::array<float, 8> primaries{};
  switch (primaries_code) {
    case 1:
      primaries = kRasterPrimariesRec709;
      break;
    case 5:
      primaries = {0.64f, 0.33f, 0.29f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f};
      break;
    case 6:
      primaries = {0.630f, 0.340f, 0.310f, 0.595f, 0.155f, 0.070f, 0.3127f, 0.3290f};
      break;
    case 9:
      primaries = kRasterPrimariesRec2020;
      break;
    case 11:
      primaries = kRasterPrimariesDciP3;
      break;
    case 12:
      primaries = kRasterPrimariesDisplayP3;
      break;
    default:
      AppendReason(reasons,
                   "CICP colour primaries " + std::to_string(primaries_code) + " are unknown");
      return std::nullopt;
  }
  RasterTransfer transfer;
  switch (transfer_code) {
    case 1:
    case 6:
    case 14:
    case 15:
      transfer = MakeTransfer(RasterTransferKind::Bt1886);
      break;
    case 4:
      transfer = MakeTransfer(RasterTransferKind::Gamma, 2.2f);
      break;
    case 5:
      transfer = MakeTransfer(RasterTransferKind::Gamma, 2.8f);
      break;
    case 8:
      transfer = MakeTransfer(RasterTransferKind::Linear);
      break;
    case 13:
      transfer = MakeTransfer(RasterTransferKind::SrgbPiecewise);
      break;
    case 16:
      transfer = MakeTransfer(RasterTransferKind::St2084);
      break;
    case 18:
      transfer = MakeTransfer(RasterTransferKind::Hlg);
      break;
    default:
      AppendReason(reasons, "CICP transfer characteristics " + std::to_string(transfer_code) +
                                " are unknown");
      return std::nullopt;
  }
  auto description = MakeDescription(primaries, transfer, origin, RasterReferral::DisplayReferred);
  description.peak_luminance_nits_ =
      PeakLuminanceFor(transfer.kind_, mastering_max_nits, max_content_light_nits);
  return description;
}

/// Description from an embedded ICC profile.
/// @throws RasterColorDescriptionError for a CMYK profile.
auto DescribeIcc(std::span<const std::byte> profile_bytes, bool gray_image, std::string& reasons)
    -> std::optional<RasterColorDescription> {
  const auto readout = ReadIccProfile(profile_bytes);
  if (readout.layout_ == IccProfileLayout::Cmyk) {
    throw RasterColorDescriptionError(RasterColorDescriptionError::Reason::UnsupportedCmyk,
                                      "the embedded ICC profile is CMYK");
  }
  if (!readout.defect_.empty()) {
    AppendReason(reasons, "embedded ICC profile: " + readout.defect_);
    return std::nullopt;
  }
  if (gray_image != (readout.layout_ == IccProfileLayout::Gray)) {
    AppendReason(reasons, gray_image ? "embedded ICC profile is not gray but the image is"
                                     : "embedded ICC profile is gray but the image is not");
    return std::nullopt;
  }

  std::optional<RasterColorDescription> description;
  if (readout.cicp_) {
    std::string cicp_reasons;
    description = DescribeCicp(*readout.cicp_, RasterColorOrigin::IccCicp, std::nullopt,
                               std::nullopt, cicp_reasons);
    if (!description) {
      AppendReason(reasons, "embedded ICC cicp tag: " + cicp_reasons);
    }
  }
  if (!description) {
    switch (readout.layout_) {
      case IccProfileLayout::RgbMatrixShaper:
        description =
            MakeDescription(readout.primaries_xy_, readout.transfer_[0],
                            RasterColorOrigin::IccMatrixShaper, RasterReferral::DisplayReferred);
        description->transfer_ = readout.transfer_;
        break;
      case IccProfileLayout::Gray:
        description =
            MakeDescription(kRasterPrimariesRec709, readout.transfer_[0],
                            RasterColorOrigin::IccMatrixShaper, RasterReferral::DisplayReferred);
        break;
      case IccProfileLayout::RgbLut:
        // The decoder converts the pixels to linear Rec.2020 with LittleCMS.
        description =
            MakeDescription(kRasterPrimariesRec2020, MakeTransfer(RasterTransferKind::Linear),
                            RasterColorOrigin::IccLutConverted, RasterReferral::DisplayReferred);
        break;
      default:
        AppendReason(reasons, "embedded ICC profile layout is not supported");
        return std::nullopt;
    }
    description->peak_luminance_nits_ =
        PeakLuminanceFor(description->transfer_[0].kind_, std::nullopt, std::nullopt);
  }
  if (const auto defect = FindRasterColorDescriptionDefect(*description)) {
    AppendReason(reasons, "embedded ICC profile is unusable: " + *defect);
    return std::nullopt;
  }
  description->profile_description_ = readout.description_;
  description->icc_sha256_          = ComputeSha256Hex(profile_bytes);
  return description;
}

/// Run @p read and report a malformed container as RasterColorDescriptionError.
template <typename Read>
auto ReadContainer(Read&& read) {
  try {
    return read();
  } catch (const RasterContainerError& error) {
    throw RasterColorDescriptionError(RasterColorDescriptionError::Reason::MalformedContainer,
                                      error.what());
  }
}

// -------------------------------------------------------------------------------------------
// Per-format rules (plan section 4.2)
// -------------------------------------------------------------------------------------------

auto ResolveJpeg(std::span<const std::byte> bytes) -> RasterColorDescription {
  const auto info = ReadContainer([&] { return ReadJpegContainer(bytes); });
  if (info.component_count_ == 4) {
    throw RasterColorDescriptionError(RasterColorDescriptionError::Reason::UnsupportedCmyk,
                                      "the JPEG has four components (CMYK or YCCK)");
  }
  const bool  gray = info.component_count_ == 1;
  std::string reasons;
  if (!info.icc_profile_.empty()) {
    if (auto description = DescribeIcc(info.icc_profile_, gray, reasons)) {
      return *description;
    }
  }
  const auto exif = ReadExifColorTags(info.exif_tiff_);
  if (exif.color_space_ == 0xFFFF && exif.interop_index_ == "R03") {
    auto description = MakeDescription(
        kRasterPrimariesAdobeRgb, MakeTransfer(RasterTransferKind::Gamma, kAdobeRgbGamma),
        RasterColorOrigin::ExifInteropAdobeRgb, RasterReferral::DisplayReferred);
    return description;
  }
  if (exif.color_space_ == 1) {
    AppendReason(reasons, "EXIF ColorSpace states sRGB");
  }
  if (reasons.empty()) {
    reasons = "the JPEG has no ICC profile and no Adobe RGB EXIF interoperability index";
  }
  return SrgbDefault(std::move(reasons));
}

auto ResolvePng(std::span<const std::byte> bytes) -> RasterColorDescription {
  const auto  info = ReadContainer([&] { return ReadPngContainer(bytes); });
  const bool  gray = info.color_type_ == 0 || info.color_type_ == 4;
  std::string reasons;

  if (info.cicp_) {
    std::string cicp_reasons;
    if (auto description = DescribeCicp(*info.cicp_, RasterColorOrigin::PngCicp,
                                        info.mastering_max_luminance_nits_,
                                        info.max_content_light_level_nits_, cicp_reasons)) {
      return *description;
    }
    AppendReason(reasons, "cICP chunk: " + cicp_reasons);
  }
  if (info.has_iccp_) {
    if (info.icc_profile_.empty()) {
      AppendReason(reasons, "iCCP chunk cannot be decompressed");
    } else if (auto description = DescribeIcc(info.icc_profile_, gray, reasons)) {
      return *description;
    }
  }
  if (info.has_srgb_) {
    return MakeDescription(kRasterPrimariesRec709, MakeTransfer(RasterTransferKind::SrgbPiecewise),
                           RasterColorOrigin::PngSrgbChunk, RasterReferral::DisplayReferred);
  }
  if (info.gama_) {
    if (*info.gama_ == 0) {
      AppendReason(reasons, "gAMA chunk value is 0");
    } else {
      const auto transfer =
          GammaOrLinear(SnapExponent(100000.0 / static_cast<double>(*info.gama_), 1e-3));
      auto description =
          MakeDescription(kRasterPrimariesRec709, transfer, RasterColorOrigin::PngGamaChrm,
                          RasterReferral::DisplayReferred);
      if (info.chrm_) {
        const auto& chrm        = *info.chrm_;
        auto        with_chrm   = description;
        // cHRM order is Wx Wy Rx Ry Gx Gy Bx By.
        with_chrm.primaries_xy_ = {
            static_cast<float>(chrm[2] / 100000.0), static_cast<float>(chrm[3] / 100000.0),
            static_cast<float>(chrm[4] / 100000.0), static_cast<float>(chrm[5] / 100000.0),
            static_cast<float>(chrm[6] / 100000.0), static_cast<float>(chrm[7] / 100000.0),
            static_cast<float>(chrm[0] / 100000.0), static_cast<float>(chrm[1] / 100000.0)};
        if (const auto defect = FindRasterColorDescriptionDefect(with_chrm)) {
          AppendReason(reasons, "cHRM chunk is unusable: " + *defect);
        } else {
          return with_chrm;
        }
      }
      if (const auto defect = FindRasterColorDescriptionDefect(description)) {
        AppendReason(reasons, "gAMA chunk is unusable: " + *defect);
      } else {
        description.default_reason_.clear();
        return description;
      }
    }
  } else if (info.chrm_) {
    AppendReason(reasons, "cHRM chunk without gAMA does not define a transfer function");
  }
  if (reasons.empty()) {
    reasons = "the PNG has no cICP, iCCP, sRGB or gAMA chunk";
  }
  return SrgbDefault(std::move(reasons));
}

auto ResolveTiff(std::span<const std::byte> bytes) -> RasterColorDescription {
  const auto info = ReadContainer([&] { return ReadTiffContainer(bytes); });
  if (info.photometric_ == 5) {
    throw RasterColorDescriptionError(RasterColorDescriptionError::Reason::UnsupportedCmyk,
                                      "the TIFF photometric interpretation is separated (CMYK)");
  }
  const bool  floating_point = info.sample_format_ == 3;
  const bool  gray           = info.photometric_ == 0 || info.photometric_ == 1;
  std::string reasons;
  if (!info.icc_profile_.empty()) {
    if (auto description = DescribeIcc(info.icc_profile_, gray, reasons)) {
      const bool linear = std::all_of(description->transfer_.begin(), description->transfer_.end(),
                                      [](const RasterTransfer& transfer) {
                                        return transfer.kind_ == RasterTransferKind::Linear;
                                      });
      // Float TIFF with a linear ICC curve is scene-linear (decision D3). A LUT profile
      // converts display-referred pixels, so it stays display-referred.
      if (floating_point && linear && description->origin_ != RasterColorOrigin::IccLutConverted) {
        description->referral_ = RasterReferral::SceneLinear;
      }
      return *description;
    }
  }
  if (floating_point) {
    AppendReason(reasons,
                 "floating-point TIFF without a usable ICC profile is scene-linear Rec.709");
    return LinearRec709Default(std::move(reasons));
  }
  if (reasons.empty()) {
    reasons = "the TIFF has no ICC profile";
  }
  return SrgbDefault(std::move(reasons));
}

auto ResolveOpenExr(std::span<const std::byte> bytes) -> RasterColorDescription {
  const auto  header = ReadContainer([&] { return ReadExrHeader(bytes); });
  std::string reasons;
  if (header.chromaticities_) {
    auto description =
        MakeDescription(*header.chromaticities_, MakeTransfer(RasterTransferKind::Linear),
                        RasterColorOrigin::ExrChromaticities, RasterReferral::SceneLinear);
    if (const auto defect = FindRasterColorDescriptionDefect(description)) {
      AppendReason(reasons, "chromaticities attribute is unusable: " + *defect);
    } else {
      return description;
    }
  }
  if (header.aces_container_) {
    return MakeDescription(kRasterPrimariesAp0, MakeTransfer(RasterTransferKind::Linear),
                           RasterColorOrigin::ExrAcesContainer, RasterReferral::SceneLinear);
  }
  AppendReason(reasons,
               "the OpenEXR file has no usable chromaticities attribute; Rec.709 primaries "
               "with a D65 white are the OpenEXR default");
  return LinearRec709Default(std::move(reasons));
}

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

auto EvaluatePq(double encoded) -> double {
  constexpr double m1 = 0.1593017578125, m2 = 78.84375;
  constexpr double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const double     p   = SafePow(encoded, 1.0 / m2);
  const double     num = std::max(p - c1, 0.0);
  return SafePow(num / (c2 - c3 * p), 1.0 / m1);
}

auto EvaluateHlgGray(double encoded) -> double {
  constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
  const double     scene =
      encoded <= 0.5 ? encoded * encoded / 3.0 : (std::exp((encoded - c) / a) + b) / 12.0;
  return SafePow(scene, 1.2);
}

}  // namespace

auto EvaluateRasterTransfer(const RasterTransfer& transfer, float encoded) -> float {
  const double x = encoded;
  switch (transfer.kind_) {
    case RasterTransferKind::Linear:
      return encoded;
    case RasterTransferKind::SrgbPiecewise:
      return static_cast<float>(x <= 0.04045 ? x / 12.92 : SafePow((x + 0.055) / 1.055, 2.4));
    case RasterTransferKind::Gamma:
      return static_cast<float>(SafePow(x, transfer.gamma_));
    case RasterTransferKind::Bt1886:
      return static_cast<float>(SafePow(x, 2.4));
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
      return static_cast<float>(EvaluatePq(std::clamp(x, 0.0, 1.0)));
    case RasterTransferKind::Hlg:
      return static_cast<float>(EvaluateHlgGray(std::clamp(x, 0.0, 1.0)));
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

auto ResolveRasterColorDescription(std::span<const std::byte> file_bytes, RasterFileKind kind)
    -> RasterColorDescription {
  switch (kind) {
    case RasterFileKind::Jpeg:
      return ResolveJpeg(file_bytes);
    case RasterFileKind::Png:
      return ResolvePng(file_bytes);
    case RasterFileKind::Tiff:
      return ResolveTiff(file_bytes);
    case RasterFileKind::OpenExr:
      return ResolveOpenExr(file_bytes);
  }
  throw std::invalid_argument("unknown raster file kind");
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
