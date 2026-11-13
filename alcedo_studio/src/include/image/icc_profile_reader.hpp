//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Read the color-relevant content of an ICC profile with LittleCMS 2: the layout (RGB
// matrix-shaper, RGB LUT, gray, CMYK), the native primaries and white of a matrix-shaper
// profile, its transfer curves in canonical form, the ICC v4.4 `cicp` tag and the 'desc' text.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "image/raster_color_description.hpp"

namespace alcedo {

enum class IccProfileLayout : uint8_t { RgbMatrixShaper, RgbLut, Gray, Cmyk, Unsupported };

struct IccProfileReadout {
  IccProfileLayout                      layout_ = IccProfileLayout::Unsupported;
  /// RgbMatrixShaper: native primaries and white (the D50 adaptation is undone).
  std::array<float, 8>                  primaries_xy_{};
  /// RgbMatrixShaper: one curve per channel. Gray: the gray curve in all three entries.
  std::array<RasterTransfer, 3>         transfer_{};
  /// ICC v4.4 `cicp` tag: colour primaries, transfer characteristics, matrix, full range.
  std::optional<std::array<uint8_t, 4>> cicp_;
  std::string                           description_;
  /// Why the profile cannot be used. Empty when it was read.
  std::string                           defect_;
};

/**
 * @brief Read an ICC profile.
 *
 * Transfer curves are canonicalized: a curve within 1e-4 of the identity, the sRGB piecewise
 * curve or a pure power function becomes `Linear`, `SrgbPiecewise` or `Gamma`. A tabulated
 * curve within 2e-4 of the ST 2084 or the 1000-nit HLG display curve becomes `St2084` or `Hlg`
 * (the Alcedo export profiles store PQ and HLG as 4096-entry tables). Other parametric curves
 * keep their ICC type and parameters, and other tabulated curves are sampled to
 * kRasterSampledTransferEntries entries.
 *
 * Never throws; a profile that cannot be read has `defect_` set.
 */
auto ReadIccProfile(std::span<const std::byte> profile_bytes) -> IccProfileReadout;

}  // namespace alcedo
