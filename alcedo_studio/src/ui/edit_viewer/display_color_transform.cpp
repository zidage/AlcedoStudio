//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/display_color_transform.hpp"

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "color/color_encoding_catalog.hpp"
#include "color/color_encoding_math.h"

namespace alcedo {
namespace {

/// Entries of the tabulated source curve. Linear interpolation of a gamma 2.2 curve between 4096
/// entries has an error below 1e-7.
constexpr int   kSourceCurveEntries  = 4096;
/// Table input exponent of a linear encoding.
constexpr float kLinearInputExponent = 1.0f / 2.2f;
/// Intent and flags of every display conversion (plan decision D7).
constexpr cmsUInt32Number kDisplayIntent = INTENT_RELATIVE_COLORIMETRIC;
constexpr cmsUInt32Number kDisplayFlags  = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE;

struct ProfileCloser {
  void operator()(void* profile) const { cmsCloseProfile(profile); }
};
using ProfileHandle = std::unique_ptr<void, ProfileCloser>;

struct CurveFreer {
  void operator()(cmsToneCurve* curve) const { cmsFreeToneCurve(curve); }
};
using CurveHandle = std::unique_ptr<cmsToneCurve, CurveFreer>;

struct TransformDeleter {
  void operator()(void* transform) const { cmsDeleteTransform(transform); }
};
using TransformHandle = std::unique_ptr<void, TransformDeleter>;

auto ReadDescription(cmsHPROFILE profile) -> std::string {
  const cmsUInt32Number size =
      cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", nullptr, 0);
  if (size == 0) {
    return {};
  }
  std::string text(size, '\0');
  cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", text.data(), size);
  text.resize(std::strlen(text.c_str()));
  return text;
}

/// The decode curve of @p eotf: code value to linear light, 1 at code value 1.
auto BuildSourceCurve(ColorUtils::EOTF eotf) -> CurveHandle {
  if (eotf == ColorUtils::EOTF::LINEAR) {
    return CurveHandle(cmsBuildGamma(nullptr, 1.0));
  }
  std::vector<cmsFloat32Number> entries(kSourceCurveEntries);
  for (int i = 0; i < kSourceCurveEntries; ++i) {
    const float code = static_cast<float>(i) / static_cast<float>(kSourceCurveEntries - 1);
    entries[i]       = CeDecode(static_cast<int>(eotf), code);
  }
  return CurveHandle(cmsBuildTabulatedToneCurveFloat(nullptr, kSourceCurveEntries, entries.data()));
}

/// An ICC profile of the encoding of @p source, from the gamut catalog and CeDecode only.
auto BuildSourceProfile(const ViewerDisplayConfig& source) -> ProfileHandle {
  if (!HasDisplayTransform(source)) {
    throw std::invalid_argument("display transform: " +
                                ColorUtils::EOTFToString(source.encoding_eotf) +
                                " is an HDR encoding");
  }
  const auto& xy = color::GamutPrimariesXy(ColorUtils::ColorSpaceToGamutId(source.encoding_space));
  const cmsCIExyY       white{xy[6], xy[7], 1.0};
  const cmsCIExyYTRIPLE primaries{{xy[0], xy[1], 1.0}, {xy[2], xy[3], 1.0}, {xy[4], xy[5], 1.0}};
  const CurveHandle     curve = BuildSourceCurve(source.encoding_eotf);
  if (!curve) {
    throw std::runtime_error("display transform: LittleCMS could not build the curve of " +
                             ColorUtils::EOTFToString(source.encoding_eotf));
  }
  cmsToneCurve* curves[3] = {curve.get(), curve.get(), curve.get()};
  ProfileHandle profile(cmsCreateRGBProfile(&white, &primaries, curves));
  if (!profile) {
    throw std::runtime_error("display transform: LittleCMS could not build a profile for " +
                             ColorUtils::ColorSpaceToString(source.encoding_space));
  }
  return profile;
}

/// The monitor profile, when it is an RGB profile with a conversion to the device.
auto OpenDisplayProfile(std::span<const std::byte> display_profile) -> ProfileHandle {
  if (display_profile.empty()) {
    throw std::runtime_error("display transform: the monitor ICC profile is empty");
  }
  ProfileHandle profile(cmsOpenProfileFromMem(
      display_profile.data(), static_cast<cmsUInt32Number>(display_profile.size())));
  if (!profile) {
    throw std::runtime_error("display transform: the monitor ICC profile cannot be parsed");
  }
  if (cmsGetColorSpace(profile.get()) != cmsSigRgbData) {
    throw std::runtime_error("display transform: the monitor ICC profile '" +
                             ReadDescription(profile.get()) + "' is not an RGB profile");
  }
  if (!cmsIsIntentSupported(profile.get(), kDisplayIntent, LCMS_USED_AS_OUTPUT)) {
    throw std::runtime_error("display transform: the monitor ICC profile '" +
                             ReadDescription(profile.get()) + "' has no conversion to the device");
  }
  return profile;
}

/// The conversion of both public functions, and the 'desc' text of the monitor profile.
struct DisplayConversion {
  TransformHandle transform;
  std::string     description;
};

auto CreateDisplayConversion(const ViewerDisplayConfig& source,
                             std::span<const std::byte> display_profile,
                             cmsUInt32Number pixel_format, cmsUInt32Number flags)
    -> DisplayConversion {
  const ProfileHandle encoding = BuildSourceProfile(source);
  const ProfileHandle display  = OpenDisplayProfile(display_profile);
  DisplayConversion   conversion{
      TransformHandle(cmsCreateTransform(encoding.get(), pixel_format, display.get(), pixel_format,
                                           kDisplayIntent, flags)),
      ReadDescription(display.get())};
  if (!conversion.transform) {
    throw std::runtime_error("display transform: LittleCMS could not convert to '" +
                             conversion.description + "'");
  }
  return conversion;
}

}  // namespace

auto HasDisplayTransform(const ViewerDisplayConfig& config) -> bool {
  return config.encoding_eotf != ColorUtils::EOTF::ST2084 &&
         config.encoding_eotf != ColorUtils::EOTF::HLG;
}

auto SrgbDisplayProfile() -> std::vector<std::byte> {
  const ProfileHandle profile(cmsCreate_sRGBProfile());
  cmsUInt32Number     size = 0;
  if (!profile || !cmsSaveProfileToMem(profile.get(), nullptr, &size) || size == 0) {
    throw std::runtime_error("display transform: LittleCMS could not build the sRGB profile");
  }
  std::vector<std::byte> bytes(size);
  if (!cmsSaveProfileToMem(profile.get(), bytes.data(), &size)) {
    throw std::runtime_error("display transform: LittleCMS could not write the sRGB profile");
  }
  bytes.resize(size);
  return bytes;
}

auto BuildDisplayTransformLut(const ViewerDisplayConfig& source,
                              std::span<const std::byte> display_profile, int edge)
    -> DisplayTransformLut {
  if (edge < 2) {
    throw std::invalid_argument("display transform: the table needs at least 2 nodes per axis");
  }
  const DisplayConversion conversion =
      CreateDisplayConversion(source, display_profile, TYPE_RGB_FLT, kDisplayFlags);

  DisplayTransformLut lut;
  lut.edge                = edge;
  lut.display_description = conversion.description;
  lut.input_exponent =
      source.encoding_eotf == ColorUtils::EOTF::LINEAR ? kLinearInputExponent : 1.0f;

  // The source code value of each node: the inverse of the table input coordinate.
  std::vector<float> axis(static_cast<std::size_t>(edge));
  for (int i = 0; i < edge; ++i) {
    const float coordinate = static_cast<float>(i) / static_cast<float>(edge - 1);
    axis[i] =
        lut.input_exponent == 1.0f ? coordinate : std::pow(coordinate, 1.0f / lut.input_exponent);
  }

  const auto         row_values = static_cast<std::size_t>(edge) * 3;
  std::vector<float> row_in(row_values);
  std::vector<float> row_out(row_values);
  lut.rgba.resize(static_cast<std::size_t>(edge) * edge * edge * 4);
  float* node = lut.rgba.data();
  for (int b = 0; b < edge; ++b) {
    for (int g = 0; g < edge; ++g) {
      for (int r = 0; r < edge; ++r) {
        row_in[static_cast<std::size_t>(r) * 3]     = axis[r];
        row_in[static_cast<std::size_t>(r) * 3 + 1] = axis[g];
        row_in[static_cast<std::size_t>(r) * 3 + 2] = axis[b];
      }
      cmsDoTransform(conversion.transform.get(), row_in.data(), row_out.data(),
                     static_cast<cmsUInt32Number>(edge));
      for (int r = 0; r < edge; ++r) {
        for (int channel = 0; channel < 3; ++channel) {
          const float value = row_out[static_cast<std::size_t>(r) * 3 + channel];
          // A color outside the monitor gamut clips to the gamut surface per channel.
          *node++           = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        }
        *node++ = 1.0f;
      }
    }
  }
  return lut;
}

void ConvertImageToDisplay(QImage& image, const ViewerDisplayConfig& source,
                           std::span<const std::byte> display_profile) {
  if (image.format() != QImage::Format_RGBA8888) {
    throw std::invalid_argument("display transform: the image format is not RGBA8888");
  }
  DisplayImageTransform(source, display_profile).Apply(image);
}

DisplayImageTransform::DisplayImageTransform(const ViewerDisplayConfig& source,
                                             std::span<const std::byte> display_profile) {
  DisplayConversion conversion = CreateDisplayConversion(source, display_profile, TYPE_RGBA_8,
                                                         kDisplayFlags | cmsFLAGS_COPY_ALPHA);
  transform_ = std::shared_ptr<void>(conversion.transform.release(), TransformDeleter{});
}

void DisplayImageTransform::Apply(QImage& image) const {
  if (image.format() != QImage::Format_RGBA8888) {
    throw std::invalid_argument("display transform: the image format is not RGBA8888");
  }
  const auto width = static_cast<cmsUInt32Number>(image.width());
  for (int y = 0; y < image.height(); ++y) {
    uchar* row = image.scanLine(y);
    cmsDoTransform(transform_.get(), row, row, width);
  }
}

}  // namespace alcedo
