//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "image/metadata.hpp"
#include "storage/mapper/image/image_mapper.hpp"

namespace alcedo {

/// A capture time parsed from the EXIF display date text.
struct CaptureDateTime {
  std::string date_;       ///< `YYYY-MM-DD`
  std::string date_time_;  ///< `YYYY-MM-DD HH:MM:SS` (`00:00:00` when the text has no time)
};

/**
 * @brief Parse the EXIF display date text (`YYYY-MM-DD HH:MM:SS`, or `YYYY:MM:DD ...`).
 *
 * The date part must be four digits, a separator, two digits, a separator, and two digits,
 * and must name a real calendar day. A time part, when present, must be `HH:MM:SS` in range.
 * Returns std::nullopt for empty, partial, or out-of-range text (for example
 * `0000:00:00 00:00:00`), so the capture columns are NULL instead of a wrong date.
 */
auto ParseCaptureDateTime(std::string_view text) -> std::optional<CaptureDateTime>;

/**
 * @brief Write the search columns of @p row from the Image file name, path, and metadata.
 *
 * - `file_stem`, `file_ext`: from @p file_name (the extension is lowercase, without the dot).
 * - `capture_at`, `capture_date`: ParseCaptureDateTime of the display date; empty (NULL) when
 *   the date does not parse.
 * - `camera_make`, `camera_model`, `lens`, `rating`: copied from @p metadata.
 * - `iso`, `focal_mm`, `aperture`, `pixel_count`: std::nullopt (NULL) when the metadata value
 *   is 0, which the metadata reader uses for "unknown". Focal length and aperture are rounded
 *   to two decimals (the float 2.8f is stored as 2.8).
 * - `file_search_text`: folded file name and folded parent folder name ("path tail").
 * - `exif_search_text`: folded make, model, lens, lens make, and display date text.
 *
 * Each search text joins its folded parts with one space. A folded query never contains a
 * space, so a query can match inside one part but never across two parts.
 */
void FillImageSearchColumns(const std::wstring& file_name, const std::filesystem::path& image_path,
                            const ExifDisplayMetaData& metadata, ImageMapperParams& row);

}  // namespace alcedo
