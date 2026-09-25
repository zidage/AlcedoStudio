//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace alcedo {

/// What a library search term means (Decision D4 of library_search_and_project_size_plan.md).
enum class SearchTermKind {
  Date,              ///< A capture date, month, year, or month-day.
  FileKind,          ///< A file kind: `jpg`, `dng`, `raw`, or one explicit extension.
  CaptureParameter,  ///< `iso800`, `f2.8`, or `35mm`.
  Text,              ///< Any other token.
};

/// Which part of the capture date a SearchDate fixes.
enum class SearchDateMatch {
  Day,        ///< year_, month_, day_: one calendar day.
  YearMonth,  ///< year_, month_: one month of one year.
  Year,       ///< year_: one year.
  MonthDay,   ///< month_, day_: that day in any year.
  Month,      ///< month_: that month in any year.
};

/// One reading of a date term. The fields that SearchDateMatch does not name are 0.
struct SearchDate {
  SearchDateMatch match_                                      = SearchDateMatch::Day;
  int             year_                                       = 0;
  int             month_                                      = 0;
  int             day_                                        = 0;

  auto            operator==(const SearchDate&) const -> bool = default;
};

enum class CaptureParameterField {
  Iso,          ///< Compared with `Image.iso`.
  Aperture,     ///< Compared with `Image.aperture` (f-number).
  FocalLength,  ///< Compared with `Image.focal_mm`.
};

/**
 * @brief One typed term of a library search query.
 *
 * A Date, FileKind, or CaptureParameter term matches its typed column **or** its text
 * alternative (`folded_text_` inside the folded search text columns), so `20260607` still
 * finds a file name that contains those digits. A Text term matches only the text columns.
 */
struct SearchTerm {
  SearchTermKind           kind_ = SearchTermKind::Text;
  /// Query text of the term. A term built from several tokens (`June 7`, `iso 800`) joins
  /// them with one space.
  std::wstring             text_{};
  /// FoldSearchText(text_). Empty when the text folds to nothing.
  std::wstring             folded_text_{};
  /// Text term only: match `text_` as typed against the file name, camera, and lens columns
  /// instead of `folded_text_`. Set when the token has `%`, `*`, `?`, `'`, or `"` (the user
  /// typed these on purpose), or when folding removes more than half of it.
  bool                     literal_ = false;
  /// Date term: the date the term names.
  SearchDate               date_{};
  /// FileKind term: lowercase extensions without a dot.
  std::vector<std::string> extensions_{};
  /// CaptureParameter term.
  CaptureParameterField    parameter_       = CaptureParameterField::Iso;
  double                   parameter_value_ = 0.0;
};

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
