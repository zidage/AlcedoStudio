//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Conversion of display-encoded photo pixels to the ICC profile of a monitor
// (docs/roadmap/alcedo_studio/ui/windows_display_icc_color_management_plan.md). Windows has no
// window color space that the compositor converts, so the application does the conversion that
// ColorSync does on macOS: as a 3D lookup table that the viewport composite samples, and for an
// 8-bit image.

#pragma once

#include <QImage>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

/// Nodes per axis of a display transform table.
inline constexpr int kDisplayTransformLutEdge = 65;

/**
 * @brief Lookup table from the code values of a viewer encoding to the code values of a monitor.
 *
 * The table input coordinate of a channel is `clamp(code, 0, 1) ^ input_exponent`. Node
 * `(r, g, b)` is at `rgba[((b * edge + g) * edge + r) * 4]`: red changes fastest, and one blue
 * index is one slice of a 3D texture. Each value is in [0, 1], and each alpha is 1.
 */
struct DisplayTransformLut {
  int                edge           = 0;
  /// 1 for a non-linear encoding. A linear encoding uses 1 / 2.2, so that the nodes are not
  /// equally spaced in linear light.
  float              input_exponent = 1.0f;
  std::vector<float> rgba;
  /// The 'desc' text of the monitor profile.
  std::string        display_description;
};

/// True when @p config is an SDR encoding. ST 2084 and HLG have no display transform.
[[nodiscard]] auto HasDisplayTransform(const ViewerDisplayConfig& config) -> bool;

/// An ICC profile with the Rec.709 primaries and the IEC 61966-2-1 curve. The destination for a
/// monitor without a profile and for a monitor that Windows converts from sRGB itself.
/// @throws std::runtime_error when LittleCMS cannot build or write the profile.
[[nodiscard]] auto SrgbDisplayProfile() -> std::vector<std::byte>;

/**
 * @brief Build the table from the encoding of @p source to the monitor profile @p display_profile.
 *
 * The source profile has the primaries and the white of `source.encoding_space` and the curve of
 * `source.encoding_eotf`. LittleCMS 2 converts with the relative colorimetric intent and black
 * point compensation, the default of display color management. A color outside the gamut of the
 * monitor clips per channel. The video card gamma table of the monitor profile (`vcgt`) is not
 * applied: the operating system loads it.
 *
 * @param edge Nodes per axis, at least 2.
 * @throws std::invalid_argument when @ref HasDisplayTransform is false for @p source or @p edge
 *         is less than 2.
 * @throws std::runtime_error when @p display_profile is not a readable RGB profile that can be a
 *         destination, or when LittleCMS cannot build the source profile or the conversion.
 * @thread Any thread. The function holds no state.
 */
[[nodiscard]] auto BuildDisplayTransformLut(const ViewerDisplayConfig& source,
                                            std::span<const std::byte> display_profile,
                                            int edge = kDisplayTransformLutEdge)
    -> DisplayTransformLut;

/**
 * @brief Convert the pixels of @p image from the encoding of @p source to @p display_profile, in
 *        place.
 *
 * The conversion has the profiles, the intent, and the clipping of @ref BuildDisplayTransformLut.
 * The alpha values stay as they are.
 *
 * @param image An image of `QImage::Format_RGBA8888`.
 * @throws std::invalid_argument when @p image has a different format, or as
 *         @ref BuildDisplayTransformLut for @p source.
 * @throws std::runtime_error as @ref BuildDisplayTransformLut for @p display_profile.
 * @thread Any thread. The function changes only @p image.
 */
void ConvertImageToDisplay(QImage& image, const ViewerDisplayConfig& source,
                           std::span<const std::byte> display_profile);

/**
 * @brief The conversion of @ref ConvertImageToDisplay, prepared once for many images.
 *
 * Building the LittleCMS conversion costs about 2 to 3 ms (release build), more than converting a
 * 512-pixel thumbnail. An image store keeps one object for each encoding of its destination
 * (DisplayImageConverter). The pixels equal those of @ref ConvertImageToDisplay.
 *
 * Thread: @ref Apply runs on any thread, also on several threads at once (the LittleCMS
 * conversion has no pixel cache).
 */
class DisplayImageTransform {
 public:
  /// @throws as @ref ConvertImageToDisplay for @p source and @p display_profile.
  DisplayImageTransform(const ViewerDisplayConfig& source,
                        std::span<const std::byte> display_profile);

  /**
   * @brief Convert the pixels of @p image in place; the alpha values stay.
   * @param image An image of `QImage::Format_RGBA8888`.
   * @throws std::invalid_argument when @p image has a different format.
   */
  void Apply(QImage& image) const;

 private:
  /// The LittleCMS transform (TYPE_RGBA_8 to TYPE_RGBA_8); the deleter frees it.
  std::shared_ptr<void> transform_;
};

}  // namespace alcedo
