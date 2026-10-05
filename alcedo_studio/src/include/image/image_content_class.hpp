//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "image/raster_color_description.hpp"

namespace alcedo {

/// What a file's content is, decided from its bytes, not its extension.
enum class ImageContentClass : uint8_t { Raw, Jpeg, Png, Tiff, OpenExr, Unknown };

/**
 * @brief Classify file content (raster_image_input_plan.md, section 6.1).
 *
 * JPEG, PNG and OpenEXR are recognized by their magic numbers. A TIFF container is RAW when its
 * first directory has a DNGVersion tag, or when LibRaw opens it and reports a camera make and a
 * color matrix; otherwise it is a raster TIFF. Anything else is `Unknown`, which import hands to
 * LibRaw as before (CR3, RAF, RW2 and others).
 */
[[nodiscard]] auto ClassifyImageContent(std::span<const std::byte> bytes) -> ImageContentClass;

/// Raster container of @p content_class, or std::nullopt for RAW and Unknown.
[[nodiscard]] auto RasterFileKindFor(ImageContentClass content_class)
    -> std::optional<RasterFileKind>;

}  // namespace alcedo
