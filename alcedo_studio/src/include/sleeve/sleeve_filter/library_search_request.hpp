//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
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

/// Field-scope mask of the library search. Each bit lets one field group match, so the
/// search-settings drawer can scope results. Bits are combinable; `kAllSearchFields` lets every
/// field group match (the default).
///   - `Filename`:      Image `file_search_text` (file name and parent folder
///                      name) + file kind terms on `file_ext`.
///   - `Exif`:          Image `exif_search_text` (make, model, lens, lens make,
///                      date text) + capture parameter terms on ISO / focal
///                      length / aperture + date terms on `capture_date`.
///   - `AiDescription`: `AiImageSearchText.caption_search_text` (caption + scene).
///   - `AiTags`:        `AiImageSearchText.tags_search_text` + the local CLIP
///                      `SemanticImageLabel` taxonomy (AI-derived labels).
///
/// Declared `enum class` (scoped) deliberately: an unscoped enum would leak the
/// enumerator names (notably `AiDescription`, which collides with the
/// `alcedo::AiDescription` struct in ai/ai_description.hpp) into the `alcedo`
/// namespace. The bitmask operators below let callers write
/// `mask |= SearchField::Filename` / `mask & SearchField::Exif` directly.
enum class SearchField : std::uint8_t {
  Filename      = 1u << 0u,
  Exif          = 1u << 1u,
  AiDescription = 1u << 2u,
  AiTags        = 1u << 3u,
};
using SearchFieldMask = std::uint8_t;
inline constexpr SearchFieldMask kAllSearchFields =
    static_cast<SearchFieldMask>(SearchField::Filename) |
    static_cast<SearchFieldMask>(SearchField::Exif) |
    static_cast<SearchFieldMask>(SearchField::AiDescription) |
    static_cast<SearchFieldMask>(SearchField::AiTags);

inline constexpr SearchFieldMask operator|(SearchFieldMask m, SearchField f) {
  return m | static_cast<SearchFieldMask>(f);
}
inline constexpr SearchFieldMask operator|(SearchField f, SearchFieldMask m) {
  return static_cast<SearchFieldMask>(f) | m;
}
inline constexpr SearchFieldMask& operator|=(SearchFieldMask& m, SearchField f) {
  m = m | static_cast<SearchFieldMask>(f);
  return m;
}
inline constexpr SearchFieldMask operator&(SearchFieldMask m, SearchField f) {
  return m & static_cast<SearchFieldMask>(f);
}

/**
 * @brief One parsed library search query: what the user asked for.
 *
 * SleeveFilterService builds it with SearchQueryParser. Storage turns it into the search
 * predicate (storage/query/library_search_predicates.hpp).
 */
struct LibrarySearchRequest {
  /// The trimmed query text. The full-text (BM25) alternative matches it as a whole.
  std::wstring            query_text{};
  /// The parsed terms; every term must match. Not empty when `fields` is not 0.
  std::vector<SearchTerm> terms{};
  /// The field groups that can match. 0 matches no file.
  SearchFieldMask         fields = kAllSearchFields;
};

/**
 * @brief The project indexes that a library search can use, read once when the search is
 *        built.
 */
struct SearchCapabilities {
  /// The active semantic model. Empty: the CLIP label alternative is not built.
  std::string semantic_model_key{};
  /// True when the BM25 index of the AI understanding documents exists.
  bool        has_ai_fts_index = false;
};

/// The files that match one library search: a FilterNode condition.
struct LibrarySearchCondition {
  LibrarySearchRequest request{};
  SearchCapabilities   capabilities{};
};

}  // namespace alcedo
