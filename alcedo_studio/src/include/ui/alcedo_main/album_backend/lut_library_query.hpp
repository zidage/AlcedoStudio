//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QHash>
#include <QString>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "utils/lut/lut_library_scan.hpp"
#include "utils/lut/lut_metadata.hpp"

namespace alcedo::ui {

/// Query bounds (docs/roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md 6.3).
inline constexpr int kLutQueryMaxCharacters = 256;
inline constexpr int kLutQueryMaxTokens     = 16;

/// Unicode NFKC normalization followed by case folding. Display text keeps the original.
[[nodiscard]] auto   NormalizeLutSearchText(const QString& text) -> QString;
/// Split normalized text at every code point that is neither a letter, a number, nor a mark.
[[nodiscard]] auto   TokenizeLutSearchText(const QString& normalized) -> std::vector<QString>;

/**
 * @brief Search keys and facet values derived from one library entry.
 *
 * Derived data: LutLibraryModel builds it once per changed entry from a scoped service read
 * and keeps it with its row index. It is not a writable copy of the entry; row display values
 * are read from LutLibraryService when a view asks for them. The labels are the original
 * strings of the facet values, kept only to name filter choices.
 */
struct LutSearchKeys {
  std::string          entry_id;
  std::string          relative_path;
  LutCategory          category = LutCategory::kGeneral;
  /// Declared source ID and name; empty without source metadata.
  QString              source_id;
  QString              source_label;
  /// Normalized film brand and its original text; empty without film metadata.
  QString              brand_key;
  QString              brand_label;
  /// True when the metadata declares a print (film or paper).
  bool                 has_print = false;
  /// Normalized print ID (its name when the ID is empty) and the print's name; empty
  /// without print metadata.
  QString              print_key;
  QString              print_label;
  /// Normalized display name, compared whole with the normalized query.
  QString              name_key;
  /// Tokens of the display name, file stem, and aliases.
  std::vector<QString> name_tokens;
  /// Tokens of the source, brands, film stock, print, and relative path.
  std::vector<QString> field_tokens;
  /// Every normalized field joined with line breaks, for substring matches.
  QString              text;
  /// Case-folded display name for the name order.
  QString              sort_name;
  std::int64_t         modified_time = 0;
};

/// Build the search keys of @p entry, whose stable identity is @p entry_id.
[[nodiscard]] auto BuildLutSearchKeys(const LutLibraryEntry& entry, std::string entry_id)
    -> LutSearchKeys;

/// A normalized query limited to kLutQueryMaxCharacters input characters and
/// kLutQueryMaxTokens tokens; later characters and tokens are ignored.
struct LutSearchQuery {
  QString              normalized;
  std::vector<QString> tokens;

  [[nodiscard]] auto   Empty() const -> bool { return tokens.empty(); }
};

[[nodiscard]] auto ParseLutSearchQuery(const QString& text) -> LutSearchQuery;

/// How one query token matched an entry, best first.
enum class LutTokenMatch : std::uint8_t { kExact = 0, kPrefix = 1, kSubstring = 2, kEdit = 3 };

/**
 * @brief Ordering key of a matching entry; a smaller rank sorts first.
 *
 * A normalized query equal to the whole normalized name ranks first. Then entries whose
 * weakest token match is better, then a smaller sum of token matches, then fewer tokens
 * matched outside the name fields. The caller breaks remaining ties by entry ID.
 */
struct LutSearchRank {
  bool          whole_name_differs = true;
  LutTokenMatch weakest            = LutTokenMatch::kEdit;
  int           match_sum          = 0;
  int           non_name_matches   = 0;

  friend auto   operator<=>(const LutSearchRank&, const LutSearchRank&) = default;
};

/**
 * @brief Edit-distance results of one query's tokens, reused while ranking many entries.
 *
 * Field tokens repeat across a library (brands, sources, folders), so one ranking pass
 * computes each bounded edit distance once per distinct token. Owned by the caller for one
 * pass over one query; it holds no entry data and is not shared between threads.
 */
class LutEditDistanceMemo {
 public:
  explicit LutEditDistanceMemo(const LutSearchQuery& query) : within_limit_(query.tokens.size()) {}

  /// True when field token @p candidate is within the edit limit of query token @p token_index.
  [[nodiscard]] auto WithinLimit(std::size_t token_index, const QString& token,
                                 const QString& candidate, int limit) -> bool;

 private:
  std::vector<QHash<QString, bool>> within_limit_;
};

/**
 * @brief Rank @p keys for @p query, or std::nullopt when a token matches no field.
 *
 * Every query token must match some field token: exactly, as a token prefix, as a substring
 * of any field, or within a bounded edit distance. Tokens shorter than 4 characters match only
 * exactly or as a prefix. Tokens of 4-7 characters allow one edit, longer tokens two.
 * An empty query ranks every entry equally. @p memo, when given, must belong to @p query.
 */
[[nodiscard]] auto RankLutSearchKeys(const LutSearchKeys& keys, const LutSearchQuery& query,
                                     LutEditDistanceMemo* memo = nullptr)
    -> std::optional<LutSearchRank>;

enum class LutCategoryFilter : std::uint8_t { kAll, kGeneral, kFilmSimulation };
enum class LutFacetDimension : std::uint8_t { kCategory, kSource, kBrand, kPrint, kFavorites };

/**
 * @brief The browser's filter predicates; an empty key or kAll removes that predicate.
 *
 * Brand and print are film predicates: they do not apply while the category is General.
 * The print predicate matches one print (print film or photographic paper) by its key.
 */
struct LutFacetFilter {
  LutCategoryFilter category = LutCategoryFilter::kAll;
  QString           source_id;
  QString           brand_key;
  QString           print_key;
  bool              favorites_only = false;
};

/// True when @p keys passes every predicate of @p filter except the @p ignored dimension.
/// @p favorite is the entry's current favorite state.
[[nodiscard]] auto LutKeysPassFilter(const LutSearchKeys& keys, bool favorite,
                                     const LutFacetFilter&            filter,
                                     std::optional<LutFacetDimension> ignored = std::nullopt)
    -> bool;

}  // namespace alcedo::ui
