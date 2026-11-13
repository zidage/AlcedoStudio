//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/raster_color_description.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include "color/color_encoding_catalog.hpp"
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
      MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Rec709),
                      MakeTransfer(RasterTransferKind::SrgbPiecewise),
                      RasterColorOrigin::DefaultSrgb, RasterReferral::DisplayReferred);
  description.default_reason_ = std::move(reason);
  return description;
}

auto LinearRec709Default(std::string reason) -> RasterColorDescription {
  auto description = MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Rec709),
                                     MakeTransfer(RasterTransferKind::Linear),
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
      primaries = color::GamutPrimariesXy(color::ColorGamutId::Rec709);
      break;
    case 5:
      primaries = color::GamutPrimariesXy(color::ColorGamutId::Bt601_625);
      break;
    case 6:
      primaries = color::GamutPrimariesXy(color::ColorGamutId::Bt601_525);
      break;
    case 9:
      primaries = color::GamutPrimariesXy(color::ColorGamutId::Rec2020);
      break;
    case 11:
      primaries = color::GamutPrimariesXy(color::ColorGamutId::P3Dci);
      break;
    case 12:
      primaries = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
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
        description = MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Rec709),
                                      readout.transfer_[0], RasterColorOrigin::IccMatrixShaper,
                                      RasterReferral::DisplayReferred);
        break;
      case IccProfileLayout::RgbLut:
        // The decoder converts the pixels to linear Rec.2020 with LittleCMS.
        description =
            MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Rec2020),
                            MakeTransfer(RasterTransferKind::Linear),
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
    auto description =
        MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::AdobeRgb),
                        MakeTransfer(RasterTransferKind::Gamma, CE_ADOBE_RGB_GAMMA),
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
    return MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Rec709),
                           MakeTransfer(RasterTransferKind::SrgbPiecewise),
                           RasterColorOrigin::PngSrgbChunk, RasterReferral::DisplayReferred);
  }
  if (info.gama_) {
    if (*info.gama_ == 0) {
      AppendReason(reasons, "gAMA chunk value is 0");
    } else {
      const auto transfer =
          GammaOrLinear(SnapExponent(100000.0 / static_cast<double>(*info.gama_), 1e-3));
      auto description =
          MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Rec709), transfer,
                          RasterColorOrigin::PngGamaChrm, RasterReferral::DisplayReferred);
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
    return MakeDescription(color::GamutPrimariesXy(color::ColorGamutId::Ap0),
                           MakeTransfer(RasterTransferKind::Linear),
                           RasterColorOrigin::ExrAcesContainer, RasterReferral::SceneLinear);
  }
  AppendReason(reasons,
               "the OpenEXR file has no usable chromaticities attribute; Rec.709 primaries "
               "with a D65 white are the OpenEXR default");
  return LinearRec709Default(std::move(reasons));
}

}  // namespace

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

}  // namespace alcedo
