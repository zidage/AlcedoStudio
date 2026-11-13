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
#include "storage/query/capture_date_time.hpp"

namespace alcedo {

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
 * - `exif_search_words`: the `exif_search_text` parts folded with FoldSearchWords (one space
 *   between the words of a part) and joined with `|`.
 *
 * Each search text joins its folded parts with one space. A folded query never contains a
 * space or `|`, so a query can match inside one part but never across two parts. Search
 * reads the words form to reject an EXIF match that crosses a word boundary and ends inside
 * a number (see SleeveFilterService::BuildFuzzySearchWhere).
 */
void FillImageSearchColumns(const std::wstring& file_name, const std::filesystem::path& image_path,
                            const ExifDisplayMetaData& metadata, ImageMapperParams& row);

}  // namespace alcedo
