//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace alcedo {

/// A capture time parsed from the EXIF display date text: the value of the `capture_at` and
/// `capture_date` columns of an Image row.
struct CaptureDateTime {
  std::string date;       ///< `YYYY-MM-DD`
  std::string date_time;  ///< `YYYY-MM-DD HH:MM:SS` (`00:00:00` when the text has no time)
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

}  // namespace alcedo
