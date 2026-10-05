//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/icc_profile_reader.hpp"

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace alcedo {
namespace {

using Mat3                               = std::array<double, 9>;  // Row major.
using Vec3                               = std::array<double, 3>;

constexpr Vec3   kD50White               = {0.9642, 1.0, 0.8249};

/// Maximum absolute difference that canonicalizes a curve to an analytic form.
constexpr double kAnalyticCurveTolerance = 1e-4;
/// Tolerance for PQ and HLG tables, which the 16-bit `curv` encoding quantizes.
constexpr double kHdrCurveTolerance      = 2e-4;

struct ProfileCloser {
  void operator()(void* profile) const { cmsCloseProfile(profile); }
};
using ProfileHandle = std::unique_ptr<void, ProfileCloser>;

auto Multiply(const Mat3& a, const Mat3& b) -> Mat3 {
  Mat3 out{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) {
        sum += a[r * 3 + k] * b[k * 3 + c];
      }
      out[r * 3 + c] = sum;
    }
  }
  return out;
}

auto Multiply(const Mat3& a, const Vec3& v) -> Vec3 {
  return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
          a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}

auto Invert(const Mat3& m, Mat3& out) -> bool {
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (!std::isfinite(det) || std::abs(det) < 1e-12) {
    return false;
  }
  const double inv = 1.0 / det;
  out              = {(m[4] * m[8] - m[5] * m[7]) * inv, (m[2] * m[7] - m[1] * m[8]) * inv,
                      (m[1] * m[5] - m[2] * m[4]) * inv, (m[5] * m[6] - m[3] * m[8]) * inv,
                      (m[0] * m[8] - m[2] * m[6]) * inv, (m[2] * m[3] - m[0] * m[5]) * inv,
                      (m[3] * m[7] - m[4] * m[6]) * inv, (m[1] * m[6] - m[0] * m[7]) * inv,
                      (m[0] * m[4] - m[1] * m[3]) * inv};
  return true;
}

/// Bradford adaptation that maps @p source_white to @p target_white.
auto BradfordAdaptation(const Vec3& source_white, const Vec3& target_white) -> Mat3 {
  constexpr Mat3 kBradford = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135,
                              0.0367, 0.0389, -0.0685, 1.0296};
  Mat3           bradford_inverse{};
  Invert(kBradford, bradford_inverse);
  const auto source = Multiply(kBradford, source_white);
  const auto target = Multiply(kBradford, target_white);
  const Mat3 scale  = {target[0] / source[0], 0.0, 0.0, 0.0, target[1] / source[1], 0.0, 0.0, 0.0,
                       target[2] / source[2]};
  return Multiply(bradford_inverse, Multiply(scale, kBradford));
}

auto ReadXyzTag(cmsHPROFILE profile, cmsTagSignature signature, Vec3& out) -> bool {
  const auto* xyz = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile, signature));
  if (xyz == nullptr) {
    return false;
  }
  out = {xyz->X, xyz->Y, xyz->Z};
  return true;
}

auto ReadRawTag(cmsHPROFILE profile, cmsTagSignature signature) -> std::vector<uint8_t> {
  const auto size = cmsReadRawTag(profile, signature, nullptr, 0);
  if (size == 0) {
    return {};
  }
  std::vector<uint8_t> bytes(size);
  if (cmsReadRawTag(profile, signature, bytes.data(), size) != size) {
    return {};
  }
  return bytes;
}

auto ReadBeS15Fixed16(const uint8_t* bytes) -> float {
  const int32_t raw = static_cast<int32_t>(
      (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
      (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]));
  return static_cast<float>(static_cast<double>(raw) / 65536.0);
}

/// Parse a raw `para` tag. Returns false for other tag types.
auto ParseParametricTag(const std::vector<uint8_t>& raw, RasterTransfer& transfer) -> bool {
  if (raw.size() < 12 || std::memcmp(raw.data(), "para", 4) != 0) {
    return false;
  }
  const int                            function_type   = (raw[8] << 8) | raw[9];
  constexpr std::array<std::size_t, 5> kParameterCount = {1, 3, 4, 5, 7};
  if (function_type < 0 || function_type > 4) {
    return false;
  }
  const auto count = kParameterCount[static_cast<std::size_t>(function_type)];
  if (raw.size() < 12 + count * 4) {
    return false;
  }
  transfer.kind_                = RasterTransferKind::IccParametric;
  transfer.icc_parametric_type_ = static_cast<uint8_t>(function_type);
  transfer.icc_params_.fill(0.0f);
  for (std::size_t i = 0; i < count; ++i) {
    transfer.icc_params_[i] = ReadBeS15Fixed16(raw.data() + 12 + i * 4);
  }
  return true;
}

auto MaxDifference(const std::vector<float>& samples, const RasterTransfer& reference) -> double {
  double worst = 0.0;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float x = static_cast<float>(i) / static_cast<float>(samples.size() - 1);
    worst         = std::max(worst, std::abs(static_cast<double>(samples[i]) -
                                             static_cast<double>(EvaluateRasterTransfer(reference, x))));
  }
  return worst;
}

auto MakeTransfer(RasterTransferKind kind, float gamma = 2.2f) -> RasterTransfer {
  RasterTransfer transfer;
  transfer.kind_  = kind;
  transfer.gamma_ = gamma;
  return transfer;
}

/// Exponent of a pure power curve fitted at mid-tones, or a non-positive value.
auto EstimatePowerExponent(const std::vector<float>& samples) -> double {
  double sum   = 0.0;
  int    count = 0;
  for (std::size_t i = samples.size() / 8; i < samples.size() * 7 / 8; i += samples.size() / 64) {
    const double x = static_cast<double>(i) / static_cast<double>(samples.size() - 1);
    const double y = samples[i];
    if (y <= 0.0 || y >= 1.0) {
      continue;
    }
    sum += std::log(y) / std::log(x);
    ++count;
  }
  return count > 0 ? sum / count : -1.0;
}

/// Canonical transfer of one ICC TRC tag.
auto ReadTransfer(cmsHPROFILE profile, cmsTagSignature signature, std::string& defect)
    -> RasterTransfer {
  const auto* curve = static_cast<const cmsToneCurve*>(cmsReadTag(profile, signature));
  if (curve == nullptr) {
    defect = "ICC TRC tag cannot be read";
    return {};
  }
  std::vector<float> samples(kRasterSampledTransferEntries);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = cmsEvalToneCurveFloat(
        curve, static_cast<float>(i) / static_cast<float>(samples.size() - 1));
  }

  if (MaxDifference(samples, MakeTransfer(RasterTransferKind::Linear)) < kAnalyticCurveTolerance) {
    return MakeTransfer(RasterTransferKind::Linear);
  }
  if (MaxDifference(samples, MakeTransfer(RasterTransferKind::SrgbPiecewise)) <
      kAnalyticCurveTolerance) {
    return MakeTransfer(RasterTransferKind::SrgbPiecewise);
  }

  RasterTransfer parametric;
  const bool     is_parametric = ParseParametricTag(ReadRawTag(profile, signature), parametric);
  double         exponent      = is_parametric && parametric.icc_parametric_type_ == 0
                                     ? static_cast<double>(parametric.icc_params_[0])
                                     : EstimatePowerExponent(samples);
  if (exponent > 0.0) {
    // Snap the s15Fixed16 rounding of a nominal exponent such as 2.199997 to 2.2.
    const double snapped = std::round(exponent * 100.0) / 100.0;
    if (std::abs(snapped - exponent) < 1e-4) {
      exponent = snapped;
    }
    const auto power = MakeTransfer(RasterTransferKind::Gamma, static_cast<float>(exponent));
    if (MaxDifference(samples, power) < kAnalyticCurveTolerance) {
      return power;
    }
  }
  if (MaxDifference(samples, MakeTransfer(RasterTransferKind::St2084)) < kHdrCurveTolerance) {
    return MakeTransfer(RasterTransferKind::St2084);
  }
  if (MaxDifference(samples, MakeTransfer(RasterTransferKind::Hlg)) < kHdrCurveTolerance) {
    return MakeTransfer(RasterTransferKind::Hlg);
  }
  if (is_parametric) {
    return parametric;
  }
  RasterTransfer sampled;
  sampled.kind_    = RasterTransferKind::IccSampled;
  sampled.sampled_ = std::move(samples);
  return sampled;
}

auto Chromaticity(const Vec3& xyz, float& x, float& y) -> bool {
  const double sum = xyz[0] + xyz[1] + xyz[2];
  if (!std::isfinite(sum) || std::abs(sum) < 1e-9) {
    return false;
  }
  x = static_cast<float>(xyz[0] / sum);
  y = static_cast<float>(xyz[1] / sum);
  return true;
}

void ReadMatrixShaper(cmsHPROFILE profile, IccProfileReadout& readout) {
  Vec3 red{}, green{}, blue{};
  if (!ReadXyzTag(profile, cmsSigRedColorantTag, red) ||
      !ReadXyzTag(profile, cmsSigGreenColorantTag, green) ||
      !ReadXyzTag(profile, cmsSigBlueColorantTag, blue)) {
    readout.defect_ = "ICC colorant tags cannot be read";
    return;
  }
  const Mat3  pcs_colorants = {red[0],  green[0], blue[0],  red[1], green[1],
                               blue[1], red[2],   green[2], blue[2]};

  // The colorants are adapted to the D50 PCS. Undo that with the inverse of `chad`; a profile
  // without `chad` (ICC v2) states its native white in `wtpt` and uses Bradford.
  Mat3        pcs_to_native{};
  const auto* chad =
      static_cast<const cmsFloat64Number*>(cmsReadTag(profile, cmsSigChromaticAdaptationTag));
  if (chad != nullptr) {
    Mat3 adaptation{};
    std::copy(chad, chad + 9, adaptation.begin());
    if (!Invert(adaptation, pcs_to_native)) {
      readout.defect_ = "ICC chad matrix is singular";
      return;
    }
  } else {
    Vec3 media_white = kD50White;
    ReadXyzTag(profile, cmsSigMediaWhitePointTag, media_white);
    if (media_white[1] <= 0.0) {
      readout.defect_ = "ICC media white point is invalid";
      return;
    }
    media_white   = {media_white[0] / media_white[1], 1.0, media_white[2] / media_white[1]};
    pcs_to_native = BradfordAdaptation(kD50White, media_white);
  }
  const Mat3 native       = Multiply(pcs_to_native, pcs_colorants);
  const Vec3 native_white = {native[0] + native[1] + native[2], native[3] + native[4] + native[5],
                             native[6] + native[7] + native[8]};
  for (int column = 0; column < 3; ++column) {
    const Vec3 xyz = {native[column], native[3 + column], native[6 + column]};
    if (!Chromaticity(xyz, readout.primaries_xy_[column * 2],
                      readout.primaries_xy_[column * 2 + 1])) {
      readout.defect_ = "ICC colorant has no chromaticity (degenerate primaries)";
      return;
    }
  }
  if (!Chromaticity(native_white, readout.primaries_xy_[6], readout.primaries_xy_[7])) {
    readout.defect_ = "ICC white has no chromaticity (degenerate primaries)";
    return;
  }
  readout.transfer_[0] = ReadTransfer(profile, cmsSigRedTRCTag, readout.defect_);
  readout.transfer_[1] = ReadTransfer(profile, cmsSigGreenTRCTag, readout.defect_);
  readout.transfer_[2] = ReadTransfer(profile, cmsSigBlueTRCTag, readout.defect_);
}

auto ReadDescription(cmsHPROFILE profile) -> std::string {
  const auto size = cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", nullptr, 0);
  if (size == 0) {
    return {};
  }
  std::string text(size, '\0');
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", text.data(), size);
  text.resize(std::strlen(text.c_str()));
  return text;
}

}  // namespace

auto ReadIccProfile(std::span<const std::byte> profile_bytes) -> IccProfileReadout {
  IccProfileReadout readout;
  if (profile_bytes.empty()) {
    readout.defect_ = "ICC profile is empty";
    return readout;
  }
  ProfileHandle profile(cmsOpenProfileFromMem(profile_bytes.data(),
                                              static_cast<cmsUInt32Number>(profile_bytes.size())));
  if (!profile) {
    readout.defect_ = "ICC profile cannot be parsed";
    return readout;
  }
  readout.description_ = ReadDescription(profile.get());
  if (cmsGetDeviceClass(profile.get()) == cmsSigLinkClass ||
      cmsGetDeviceClass(profile.get()) == cmsSigAbstractClass) {
    readout.defect_ = "ICC device link and abstract profiles do not describe a source";
    return readout;
  }
  // The `cicp` tag is read from its raw bytes: 'cicp', 4 reserved bytes, then the colour
  // primaries, transfer characteristics, matrix coefficients and full range flag.
  if (cmsIsTag(profile.get(), cmsSigcicpTag)) {
    const auto raw = ReadRawTag(profile.get(), cmsSigcicpTag);
    if (raw.size() >= 12 && std::memcmp(raw.data(), "cicp", 4) == 0) {
      readout.cicp_ = std::array<uint8_t, 4>{raw[8], raw[9], raw[10], raw[11]};
    }
  }

  switch (cmsGetColorSpace(profile.get())) {
    case cmsSigCmykData:
      readout.layout_ = IccProfileLayout::Cmyk;
      return readout;
    case cmsSigGrayData:
      readout.layout_      = IccProfileLayout::Gray;
      readout.transfer_[0] = ReadTransfer(profile.get(), cmsSigGrayTRCTag, readout.defect_);
      readout.transfer_[1] = readout.transfer_[0];
      readout.transfer_[2] = readout.transfer_[0];
      return readout;
    case cmsSigRgbData:
      if (cmsIsMatrixShaper(profile.get())) {
        readout.layout_ = IccProfileLayout::RgbMatrixShaper;
        ReadMatrixShaper(profile.get(), readout);
      } else if (cmsIsTag(profile.get(), cmsSigAToB0Tag)) {
        readout.layout_ = IccProfileLayout::RgbLut;
      } else {
        readout.defect_ = "RGB ICC profile has neither matrix-shaper tags nor an A2B0 table";
      }
      return readout;
    default:
      readout.defect_ = "ICC data color space is not RGB, gray or CMYK";
      return readout;
  }
}

}  // namespace alcedo
