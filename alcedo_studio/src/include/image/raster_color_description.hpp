//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Source color description of a raster (JPEG, PNG, TIFF, OpenEXR) image: primaries, white
// point, transfer function, peak luminance and where each value came from
// (docs/roadmap/alcedo_studio/edit/raster_image_input_plan.md, sections 4 and 7.3).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace alcedo {

/// Container format of a raster file. The caller classifies the file content.
enum class RasterFileKind : uint8_t { Jpeg, Png, Tiff, OpenExr };

/// Whether code values encode light after a display rendering or scene light.
enum class RasterReferral : uint8_t { DisplayReferred, SceneLinear };

enum class RasterTransferKind : uint8_t {
  Linear,
  SrgbPiecewise,
  Gamma,
  Bt1886,
  IccParametric,
  IccSampled,
  St2084,
  Hlg,
};

/// Where the color description came from. `DefaultSrgb` is the documented default for files
/// that carry no usable description (sRGB, or Rec.709 linear for scene-linear files).
enum class RasterColorOrigin : uint8_t {
  IccMatrixShaper,
  IccLutConverted,
  IccCicp,
  PngCicp,
  PngSrgbChunk,
  PngGamaChrm,
  ExrChromaticities,
  ExrAcesContainer,
  ExifInteropAdobeRgb,
  DefaultSrgb,
};

/// Number of entries of a sampled ICC curve, evenly spaced over the encoded range [0, 1].
inline constexpr std::size_t kRasterSampledTransferEntries = 4096;

/// Transfer function from code value to linear light for one channel.
struct RasterTransfer {
  RasterTransferKind   kind_                = RasterTransferKind::SrgbPiecewise;
  /// Exponent for `Gamma`.
  float                gamma_               = 2.2f;
  /// ICC parametricCurveType function type (0..4) for `IccParametric`.
  uint8_t              icc_parametric_type_ = 0;
  /// ICC parametric parameters g, a, b, c, d, e, f for `IccParametric`. Unused entries are 0.
  std::array<float, 7> icc_params_{};
  /// `IccSampled`: kRasterSampledTransferEntries linear values for code values 0..1.
  std::vector<float>   sampled_;

  auto                 operator==(const RasterTransfer&) const -> bool = default;
};

/// Source color description that rendering uses for a raster image.
struct RasterColorDescription {
  RasterReferral                referral_ = RasterReferral::DisplayReferred;
  /// CIE xy chromaticities Rx Ry Gx Gy Bx By Wx Wy with the native (not adapted) white.
  std::array<float, 8>          primaries_xy_{};
  /// One transfer per channel R, G, B. Equal in the common case.
  std::array<RasterTransfer, 3> transfer_{};
  /// Peak luminance of a display-referred signal. 100 nits for SDR transfer functions.
  float                         peak_luminance_nits_ = 100.0f;
  RasterColorOrigin             origin_              = RasterColorOrigin::DefaultSrgb;
  /// ICC 'desc' text. Empty when no ICC profile was used. For the UI only.
  std::string                   profile_description_;
  /// Lower-case hex SHA-256 of the ICC profile bytes. Empty when no ICC profile was used.
  std::string                   icc_sha256_;
  /// Why a source description was not used, when the documented default applies.
  std::string                   default_reason_;

  auto                          operator==(const RasterColorDescription&) const -> bool = default;
};

/// Nominal chromaticities (Rx Ry Gx Gy Bx By Wx Wy).
inline constexpr std::array<float, 8> kRasterPrimariesRec709    = {0.64f, 0.33f, 0.30f,   0.60f,
                                                                   0.15f, 0.06f, 0.3127f, 0.3290f};
inline constexpr std::array<float, 8> kRasterPrimariesDisplayP3 = {
    0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f};
inline constexpr std::array<float, 8> kRasterPrimariesDciP3    = {0.680f, 0.320f, 0.265f, 0.690f,
                                                                  0.150f, 0.060f, 0.314f, 0.351f};
inline constexpr std::array<float, 8> kRasterPrimariesRec2020  = {0.708f, 0.292f, 0.170f,  0.797f,
                                                                  0.131f, 0.046f, 0.3127f, 0.3290f};
inline constexpr std::array<float, 8> kRasterPrimariesAdobeRgb = {0.64f, 0.33f, 0.21f,   0.71f,
                                                                  0.15f, 0.06f, 0.3127f, 0.3290f};
inline constexpr std::array<float, 8> kRasterPrimariesProPhoto = {
    0.7347f, 0.2653f, 0.1596f, 0.8404f, 0.0366f, 0.0001f, 0.3457f, 0.3585f};
inline constexpr std::array<float, 8> kRasterPrimariesAp0 = {0.7347f, 0.2653f,  0.0f,     1.0f,
                                                             0.0001f, -0.0770f, 0.32168f, 0.33767f};

/// Adobe RGB (1998) transfer exponent, 2 + 51/256.
inline constexpr float                kAdobeRgbGamma      = 563.0f / 256.0f;

/// The import step cannot use the file: its content is CMYK, or its container structure is
/// malformed.
class RasterColorDescriptionError : public std::runtime_error {
 public:
  enum class Reason : uint8_t { UnsupportedCmyk, MalformedContainer };

  RasterColorDescriptionError(Reason reason, const std::string& message)
      : std::runtime_error(message), reason_(reason) {}

  [[nodiscard]] auto reason() const -> Reason { return reason_; }

 private:
  Reason reason_;
};

/**
 * @brief Resolve the source color description of a raster file (plan section 4.2).
 *
 * The rules of each format are applied in order. The first rule that gives a usable
 * description wins. When no rule does, the documented default applies and `default_reason_`
 * states why.
 *
 * @param file_bytes Complete file content.
 * @param kind Container format, as classified from the content.
 * @throws RasterColorDescriptionError for CMYK content or a malformed container.
 */
auto ResolveRasterColorDescription(std::span<const std::byte> file_bytes, RasterFileKind kind)
    -> RasterColorDescription;

/// Return why @p description is not usable (degenerate primaries, white outside the
/// primaries, or a non-monotonic transfer), or std::nullopt when it is usable.
auto FindRasterColorDescriptionDefect(const RasterColorDescription& description)
    -> std::optional<std::string>;

/**
 * @brief Evaluate @p transfer at code value @p encoded in [0, 1].
 *
 * The result is linear light relative to the curve's own encoded maximum: 1.0 at code value
 * 1.0 for SDR curves, Y / 10000 nits for `St2084`, and the HLG display signal of the
 * 1000-nit reference display (gray axis) for `Hlg`.
 */
auto EvaluateRasterTransfer(const RasterTransfer& transfer, float encoded) -> float;

/// JSON form of plan section 7.3 (`source_color`). Keys that do not apply are omitted.
auto RasterColorDescriptionToJson(const RasterColorDescription& description) -> nlohmann::json;

/// Parse the JSON written by RasterColorDescriptionToJson.
/// @throws std::invalid_argument when a required key is missing or a value is invalid.
auto RasterColorDescriptionFromJson(const nlohmann::json& value) -> RasterColorDescription;

}  // namespace alcedo
