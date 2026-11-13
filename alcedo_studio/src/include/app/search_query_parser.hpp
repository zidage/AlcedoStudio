//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sleeve/sleeve_filter/library_search_request.hpp"

namespace alcedo {

/**
 * @brief Turns a library search query into typed terms.
 *
 * The query is split at white space. Recognized forms:
 *
 * - Date: `Y-M-D` with any of `- . / _` as separator, `YYYYMMDD`, `YYYY.M`, `YYYY`
 *   (1000–9999), `M.D` / `M/D` / `M-D` (any year), `M月D日`, `M月`, `YYYY年M月`,
 *   `YYYY年M月D日`, `YYYY年`, and English month names (`June`, `Jun`, `June 7`, `7 June`,
 *   `June 7 2026`, `June 2026`). Invalid days (`2.30`) are text.
 * - FileKind: `jpg`/`jpeg`, `tif`/`tiff`, `raw` (every RAW extension), and each explicit
 *   extension, with or without a leading dot.
 * - CaptureParameter: `iso800`, `iso 800`, `ISO-800`, `f2.8`, `f/2.8`, `35mm`, `35 mm`.
 * - Text: any other token.
 *
 * Terms are combined with AND by the caller. The parser is pure: it reads no database and
 * keeps no state.
 */
class SearchQueryParser {
 public:
  /// Parse @p query. Returns an empty list for a query that is empty or only white space.
  [[nodiscard]] static auto Parse(std::wstring_view query) -> std::vector<SearchTerm>;

  /// Lowercase extensions (no dot) that the `raw` file kind term matches.
  [[nodiscard]] static auto RawFileExtensions() -> const std::vector<std::string>&;
};

}  // namespace alcedo
