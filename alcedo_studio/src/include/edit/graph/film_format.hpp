//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alcedo {

/**
 * @brief The film format of the frame: it sets the frame size, and with the image size the pixel
 * pitch on the film.
 *
 * Plan 'film_grain_format_and_stock_plan.md', decision U1. The document identifier of each value is
 * @ref FilmFormatId. The default of a document is @ref FilmFormat::Film35mm.
 */
enum class FilmFormat : std::uint8_t {
  Film16mm = 0,  ///< Super 16 camera aperture
  Film35mm = 1,  ///< The 135 frame, the frame of the film print
  Film70mm = 2,  ///< The 6x7 frame of 120 film and the 15/70 frame
};

/// The identifier of @p format in a document: `16mm`, `35mm`, or `70mm`.
/// @throws std::invalid_argument that names the field `film_format` for a value outside the enum.
[[nodiscard]] inline auto FilmFormatId(FilmFormat format) -> std::string_view {
  switch (format) {
    case FilmFormat::Film16mm:
      return "16mm";
    case FilmFormat::Film35mm:
      return "35mm";
    case FilmFormat::Film70mm:
      return "70mm";
  }
  throw std::invalid_argument("film_format: the value " + std::to_string(static_cast<int>(format)) +
                              " is not a film format");
}

/// The format of the identifier @p id, or none for an unknown identifier.
[[nodiscard]] inline auto TryParseFilmFormat(std::string_view id) -> std::optional<FilmFormat> {
  for (const auto format : {FilmFormat::Film16mm, FilmFormat::Film35mm, FilmFormat::Film70mm}) {
    if (FilmFormatId(format) == id) {
      return format;
    }
  }
  return std::nullopt;
}

/**
 * @brief The long side of the frame of @p format, in millimetres (decision U1).
 *
 * 16mm: 12.52 (Super 16 camera aperture). 35mm: 36 (the 135 frame, the same as `common/frame_mm` of
 * the film print). 70mm: 70 (the 6x7 frame of 120 film, 69.5 mm, and the 15/70 frame, 70.4 mm).
 * @throws std::invalid_argument that names the field `film_format` for a value outside the enum.
 */
[[nodiscard]] inline auto FilmFrameLongSideMm(FilmFormat format) -> double {
  switch (format) {
    case FilmFormat::Film16mm:
      return 12.52;
    case FilmFormat::Film35mm:
      return 36.0;
    case FilmFormat::Film70mm:
      return 70.0;
  }
  throw std::invalid_argument("film_format: the value " + std::to_string(static_cast<int>(format)) +
                              " is not a film format");
}

}  // namespace alcedo
