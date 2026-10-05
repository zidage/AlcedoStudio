//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <span>

#include "edit/input/prepared_raw_input.hpp"
#include "image/raster_color_description.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Decode JPEG, PNG, TIFF and OpenEXR pixels into a RasterRgb PreparedRawInput
 * (raster_image_input_plan.md, section 6.1).
 *
 * Integer files keep their native depth (U8Rgba / U16Rgba); float TIFF and OpenEXR become
 * F32Rgba. Gray is copied to RGB and alpha is set to the maximum code value (decision D6). The
 * EXIF orientation becomes the RawSensorGeometry flip; mirrored orientations are applied to the
 * host plane. A LUT-based RGB ICC profile is applied on the host by LittleCMS (relative
 * colorimetric) to linear Rec.2020, F32. The color description itself comes from the document,
 * not from this loader.
 */
class RasterInputLoader {
 public:
  /**
   * @brief Decode @p encoded. JPEG uses TurboJPEG DCT scaling for HALF, QUARTER and EIGHTH;
   * PNG, TIFF and OpenEXR decode at full size.
   * @throws std::runtime_error on a corrupt or unsupported (CMYK) pixel stream.
   */
  [[nodiscard]] static auto LoadEncoded(std::span<const std::byte> encoded, DecodeRes decode_res,
                                        RasterFileKind kind) -> PreparedRawInput;

  /**
   * @brief Wrap a full-resolution RGBA host plane (U8Rgba, U16Rgba or F32Rgba) as raster input
   * with orientation flip @p orientation_flip (0, 3, 5 or 6). Used by tests and benchmarks.
   */
  [[nodiscard]] static auto FromHostPlane(HostImagePlane plane, int orientation_flip = 0)
      -> PreparedRawInput;
};

/**
 * @brief Default unpack: classify @p encoded once and call exactly one loader. Raster content
 * goes to RasterInputLoader, everything else to RawInputLoader. A failure reports that loader's
 * error; there is no retry with the other loader.
 */
[[nodiscard]] auto LoadEncodedImage(std::span<const std::byte> encoded, DecodeRes decode_res)
    -> PreparedRawInput;

}  // namespace alcedo
