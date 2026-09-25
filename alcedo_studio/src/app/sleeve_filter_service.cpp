//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_filter_service.hpp"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/search_query_parser.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
#include "utils/string/convert.hpp"
#include "utils/string/search_text.hpp"

namespace alcedo {
namespace {
auto FilterScopeCacheKey(filter_id_t filter_id, sl_element_id_t parent_id) -> std::uint64_t {
  return (static_cast<std::uint64_t>(parent_id) << 32U) | static_cast<std::uint64_t>(filter_id);
}

auto TrimCopy(std::wstring value) -> std::wstring {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](wchar_t ch) { return std::iswspace(ch) != 0; });
  const auto last  = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
                      return std::iswspace(ch) != 0;
                    }).base();
  if (first >= last) {
    return {};
  }
  return std::wstring(first, last);
}

// Fuzzy-search clauses are composed from duckorm expr fragments. Search reads the typed and
// folded search columns that the Image mapper and the AI store write (Decision D3 of
// library_search_and_project_size_plan.md): no json_extract, no metadata cast, and no
// separator folding in SQL. Every user text value goes through expr::param (prepared binds).

auto LitW(const std::wstring& value) -> duckorm::SqlFragment {
  return duckorm::expr::param(conv::ToBytes(value));
}

/// `(contains(COALESCE(field, ''), token) OR
///   contains(LOWER(COALESCE(field, '')), LOWER(token)))`
auto ContainsClause(duckorm::SqlFragment field, const std::wstring& token)
    -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;

  auto coalesced = expr::raw("COALESCE(");
  coalesced.append(std::move(field));
  coalesced.append(expr::raw(", '')"));
  const auto needle = LitW(token);

  auto lower_value = expr::raw("LOWER(");
  lower_value.append(coalesced);
  lower_value.append(expr::raw(")"));
  auto lower_needle = expr::raw("LOWER(");
  lower_needle.append(needle);
  lower_needle.append(expr::raw(")"));

  auto plain = expr::raw("contains(");
  plain.append(coalesced);
  plain.append(expr::raw(", "));
  plain.append(needle);
  plain.append(expr::raw(")"));

  auto folded = expr::raw("contains(");
  folded.append(std::move(lower_value));
  folded.append(expr::raw(", "));
  folded.append(std::move(lower_needle));
  folded.append(expr::raw(")"));

  return expr::or_({std::move(plain), std::move(folded)});
}

/// `contains(COALESCE(column, ''), folded_token)` for a folded search text column. The
/// column is already folded at write time, so the token is folded here with the same
/// FoldSearchText and compared as is.
auto FoldedColumnContains(std::string_view column, const std::wstring& folded_token)
    -> duckorm::SqlFragment {
  auto clause = duckorm::expr::raw("contains(COALESCE(" + std::string(column) + ", ''), ");
  clause.append(LitW(folded_token));
  clause.append(duckorm::expr::raw(")"));
  return clause;
}

/// One folded search text column and, for the EXIF text, its word-folded form.
struct SearchTextColumn {
  std::string_view text_;
  std::string_view words_;  ///< Empty when the column has no word-folded form.
};

/// Folded search text columns that the enabled field groups can match. `i.` columns are
/// written by ImageMapper; `u.` columns come from the AI understanding join in
/// BuildScopedFileQuery.
auto SearchTextColumns(SearchFieldMask mask) -> std::vector<SearchTextColumn> {
  std::vector<SearchTextColumn> columns;
  if (mask & SearchField::Filename) {
    columns.push_back({"i.file_search_text", {}});
  }
  if (mask & SearchField::Exif) {
    columns.push_back({"i.exif_search_text", "i.exif_search_words"});
  }
  if (mask & SearchField::AiDescription) {
    columns.push_back({"u.caption_search_text", {}});
  }
  if (mask & SearchField::AiTags) {
    columns.push_back({"u.tags_search_text", {}});
  }
  return columns;
}

auto IsAsciiDigit(wchar_t ch) -> bool { return ch >= L'0' && ch <= L'9'; }

/// RE2 pattern that matches @p folded_token in a word-folded column with at most one space
/// between two characters, and that is not followed by a digit:
/// `z8` gives `z ?8(?:[^0-9]|$)`. ASCII punctuation is escaped; other characters are literal.
auto CrossWordNumberEndPattern(const std::wstring& folded_token) -> std::string {
  std::wstring pattern;
  for (size_t i = 0; i < folded_token.size(); ++i) {
    const auto ch = folded_token[i];
    if (i > 0) {
      pattern += L" ?";
    }
    if (ch < 0x80 && std::iswalnum(static_cast<std::wint_t>(ch)) == 0) {
      pattern.push_back(L'\\');
    }
    pattern.push_back(ch);
  }
  pattern += L"(?:[^0-9]|$)";
  return conv::ToBytes(pattern);
}

/**
 * @brief Match a folded token in one search text column.
 *
 * `contains(text, token)` is the whole rule for a column without a word-folded form and for a
 * token that does not end with a digit. In the EXIF text, a match of a token that ends with a
 * digit must not cross a word boundary and then end inside a number: `z8` matches the model
 * `NIKON Z 8` but not the lens `NIKKOR Z 85mm`, whose folded text `nikkorz85mm` also contains
 * `z8`. Inside one word the token may end anywhere (`8` finds `85mm`). File names keep the
 * plain rule, so a counter prefix typed without its separator (`dsc223` for `DSC_2230`) still
 * matches. The `contains` term runs first and limits the regular expression to rows that
 * already contain the token.
 */
auto FoldedTextClause(const SearchTextColumn& column, const std::wstring& folded_token)
    -> duckorm::SqlFragment {
  namespace expr     = duckorm::expr;
  auto contains_text = FoldedColumnContains(column.text_, folded_token);
  if (column.words_.empty() || folded_token.empty() || !IsAsciiDigit(folded_token.back())) {
    return contains_text;
  }
  auto regexp = expr::raw("regexp_matches(" + std::string(column.words_) + ", ");
  regexp.append(expr::param(CrossWordNumberEndPattern(folded_token)));
  regexp.append(expr::raw(")"));
  return expr::and_(
      {std::move(contains_text),
       expr::or_({FoldedColumnContains(column.words_, folded_token), std::move(regexp)})});
}

/**
 * @brief Match the CLIP semantic labels of a file against one query term.
 *
 * Query-side alias expansion: a label definition matches when the folded term is part of one
 * of its folded aliases (canonical, English, or Chinese label). The clause is
 * `EXISTS (SELECT 1 FROM SemanticImageLabel sl WHERE sl.file_id = e.id AND
 * sl.model_key = ? AND (LOWER(sl.label) IN (LOWER(?), ...) OR contains(LOWER(sl.label),
 * LOWER(?))))`. The IN list holds every alias of every matched definition, because a stored
 * label can be any alias. The `contains` term matches a stored label that no definition
 * lists. Returns std::nullopt when no semantic model is active.
 */
auto SemanticLabelClause(const std::wstring& term, const std::string& active_model_key)
    -> std::optional<duckorm::SqlFragment> {
  namespace expr = duckorm::expr;

  if (active_model_key.empty()) {
    return std::nullopt;
  }
  const auto                        folded_term = FoldSearchText(term);

  std::vector<duckorm::SqlFragment> label_terms;
  if (!folded_term.empty()) {
    std::vector<duckorm::SqlFragment> alias_values;
    for (const auto& label : DefaultSemanticPhotographyLabelDefinitions()) {
      const auto canonical = conv::FromBytes(label.canonical_label);
      const auto en        = conv::FromBytes(label.english_label);
      const auto zh        = conv::FromBytes(label.chinese_label);
      const bool matched   = std::ranges::any_of(
          std::initializer_list<std::wstring>{canonical, en, zh},
          [&folded_term](const std::wstring& alias) {
            return FoldSearchText(alias).find(folded_term) != std::wstring::npos;
          });
      if (!matched) {
        continue;
      }
      for (const auto& alias : {canonical, en, zh}) {
        auto lower_alias = expr::raw("LOWER(");
        lower_alias.append(LitW(alias));
        lower_alias.append(expr::raw(")"));
        alias_values.push_back(std::move(lower_alias));
      }
    }
    if (!alias_values.empty()) {
      auto in_list = expr::raw("(LOWER(sl.label) IN (");
      for (size_t i = 0; i < alias_values.size(); ++i) {
        if (i > 0) {
          in_list.append(expr::raw(", "));
        }
        in_list.append(std::move(alias_values[i]));
      }
      in_list.append(expr::raw("))"));
      label_terms.push_back(std::move(in_list));
    }
  }
  auto lower_term = expr::raw("contains(LOWER(sl.label), LOWER(");
  lower_term.append(LitW(term));
  lower_term.append(expr::raw("))"));
  label_terms.push_back(std::move(lower_term));

  auto subquery = expr::raw("SELECT 1 FROM SemanticImageLabel sl WHERE ");
  subquery.append(expr::and_({expr::eq(expr::col("sl.file_id"), expr::col("e.id")),
                              expr::eq(expr::col("sl.model_key"), expr::param(active_model_key)),
                              expr::or_(label_terms)}));
  return expr::exists(std::move(subquery));
}

auto DateLiteral(int year, int month, int day) -> std::string {
  return std::format("{:04}-{:02}-{:02}", year, month, day);
}

auto DateValue(int year, int month, int day) -> duckorm::SqlFragment {
  return duckorm::expr::raw("DATE " + duckorm::expr::lit(DateLiteral(year, month, day)).sql_);
}

/// `i.capture_date >= DATE 'from' AND i.capture_date < DATE 'to'`.
auto CaptureDateRange(duckorm::SqlFragment from, duckorm::SqlFragment to) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;
  return expr::and_({expr::ge(expr::col("i.capture_date"), std::move(from)),
                     expr::lt(expr::col("i.capture_date"), std::move(to))});
}

/// `<part>(i.capture_date) = value`, for `month` and `day`.
auto CaptureDatePart(std::string_view part, int value) -> duckorm::SqlFragment {
  return duckorm::expr::eq(duckorm::expr::raw(std::string(part) + "(i.capture_date)"),
                           duckorm::expr::lit(static_cast<int64_t>(value)));
}

/// Typed predicate of a date term on `Image.capture_date`.
auto CaptureDateClause(const SearchDate& date) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;
  switch (date.match_) {
    case SearchDateMatch::Day:
      return expr::eq(expr::col("i.capture_date"), DateValue(date.year_, date.month_, date.day_));
    case SearchDateMatch::YearMonth:
      return CaptureDateRange(DateValue(date.year_, date.month_, 1),
                              date.month_ == 12 ? DateValue(date.year_ + 1, 1, 1)
                                                : DateValue(date.year_, date.month_ + 1, 1));
    case SearchDateMatch::Year:
      return CaptureDateRange(DateValue(date.year_, 1, 1), DateValue(date.year_ + 1, 1, 1));
    case SearchDateMatch::MonthDay:
      return expr::and_({CaptureDatePart("month", date.month_), CaptureDatePart("day", date.day_)});
    case SearchDateMatch::Month:
      return CaptureDatePart("month", date.month_);
  }
  return expr::raw("1=0");
}

/// `i.file_ext IN ('ext', ...)`. The extensions come from the parser's fixed table.
auto FileKindClause(const std::vector<std::string>& extensions) -> duckorm::SqlFragment {
  auto clause = duckorm::expr::raw("i.file_ext IN (");
  for (size_t i = 0; i < extensions.size(); ++i) {
    if (i > 0) {
      clause.append(duckorm::expr::raw(", "));
    }
    clause.append(duckorm::expr::lit(extensions[i]));
  }
  clause.append(duckorm::expr::raw(")"));
  return clause;
}

/// Typed predicate of a capture parameter term. ISO is an integer column. Focal length and
/// aperture are stored rounded to two decimals (see FillImageSearchColumns), so they match
/// within half of that step.
auto CaptureParameterClause(const SearchTerm& term) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;
  if (term.parameter_ == CaptureParameterField::Iso) {
    return expr::eq(expr::col("i.iso"), expr::param(static_cast<int64_t>(term.parameter_value_)));
  }
  const auto* column =
      term.parameter_ == CaptureParameterField::Aperture ? "i.aperture" : "i.focal_mm";
  auto clause = expr::raw(std::string("abs(") + column + " - ");
  clause.append(expr::param(term.parameter_value_));
  clause.append(expr::raw(") < 0.005"));
  return clause;
}

/**
 * @brief Compile one parsed search term (Phase S4 of library_search_and_project_size_plan.md).
 *
 * A date, file kind, or capture parameter term is `(typed predicate OR text alternative)`;
 * the typed predicate needs the field bit of its column (Exif for dates and parameters,
 * Filename for file kinds). A text term matches the folded search text columns that @p mask
 * enables, a literal text term matches the name, camera, and lens columns as typed, and with
 * the AiTags bit a text term also matches the CLIP semantic labels.
 */
auto TermClause(const SearchTerm& term, const std::string& active_model_key, SearchFieldMask mask)
    -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;

  std::vector<duckorm::SqlFragment> clauses;
  switch (term.kind_) {
    case SearchTermKind::Date:
      if (mask & SearchField::Exif) {
        clauses.push_back(CaptureDateClause(term.date_));
      }
      break;
    case SearchTermKind::FileKind:
      if (mask & SearchField::Filename) {
        clauses.push_back(FileKindClause(term.extensions_));
      }
      break;
    case SearchTermKind::CaptureParameter:
      if (mask & SearchField::Exif) {
        clauses.push_back(CaptureParameterClause(term));
      }
      break;
    case SearchTermKind::Text:
      break;
  }

  if (term.literal_) {
    if (mask & SearchField::Filename) {
      clauses.push_back(ContainsClause(expr::raw("e.element_name"), term.text_));
      clauses.push_back(ContainsClause(expr::raw("i.file_name"), term.text_));
    }
    if (mask & SearchField::Exif) {
      clauses.push_back(ContainsClause(expr::raw("i.camera_make"), term.text_));
      clauses.push_back(ContainsClause(expr::raw("i.camera_model"), term.text_));
      clauses.push_back(ContainsClause(expr::raw("i.lens"), term.text_));
    }
  } else if (!term.folded_text_.empty()) {
    for (const auto& column : SearchTextColumns(mask)) {
      clauses.push_back(FoldedTextClause(column, term.folded_text_));
    }
  }

  if (term.kind_ == SearchTermKind::Text && (mask & SearchField::AiTags)) {
    if (auto label_clause = SemanticLabelClause(term.text_, active_model_key);
        label_clause.has_value()) {
      clauses.push_back(std::move(*label_clause));
    }
  }
  if (clauses.empty()) {
    return expr::raw("1=0");
  }
  return expr::or_(clauses);
}

auto AiUnderstandingFtsClause(const std::wstring& query) -> duckorm::SqlFragment {
  auto fragment = duckorm::expr::raw("(fts_main_AiImageFtsDocument.match_bm25(e.id, ");
  fragment.append(LitW(query));
  fragment.append(duckorm::expr::raw(") IS NOT NULL)"));
  return fragment;
}

}  // namespace

auto SleeveFilterService::CreateFilterCombo(const FilterNode& root) -> filter_id_t {
  filter_id_t new_id = filter_id_generator_.GenerateID();
  filter_storage_.RecordAccess(new_id, std::make_shared<FilterCombo>(new_id, root));
  return new_id;
}

auto SleeveFilterService::GetFilterCombo(filter_id_t filter_id)
    -> std::optional<std::shared_ptr<FilterCombo>> {
  auto combo_opt = filter_storage_.AccessElement(filter_id);
  if (combo_opt.has_value()) {
    return combo_opt.value();
  } else {
    return std::nullopt;
  }
}

void SleeveFilterService::RemoveFilterCombo(filter_id_t filter_id) {
  // If there is no record, this is a no-op.
  filter_storage_.RemoveRecord(filter_id);
  // Result cache keys include folder scope; flushing keeps removal simple and stable.
  filter_result_cache_.Flush();
}

auto SleeveFilterService::ApplyFilterOn(filter_id_t filter_id, sl_element_id_t parent_id)
    -> std::optional<std::vector<sl_element_id_t>> {
  // First, check if the filter combo exists.
  auto combo_opt = filter_storage_.AccessElement(filter_id);
  if (!combo_opt.has_value()) {
    return std::nullopt;
  }
  auto       combo      = combo_opt.value();

  // Next, check if we have a cached result for this filter in this folder scope.
  const auto cache_key  = FilterScopeCacheKey(filter_id, parent_id);
  auto       result_opt = filter_result_cache_.AccessElement(cache_key);
  if (result_opt.has_value()) {
    return result_opt;
  }

  // No cached result, we need to execute the filter.
  auto result_ids =
      storage_->GetElementStore().GetElementIdsInFolderByFilter(combo, parent_id);
  // Cache the result for future use.
  filter_result_cache_.RecordAccess(cache_key, result_ids);
  return result_ids;
}

auto SleeveFilterService::BuildFolderStats(sl_element_id_t                  parent_id,
                                           const std::optional<FilterNode>& extra_filter) const
    -> AlbumStatsView {
  const auto extra_predicate = CompileFilterPredicate(extra_filter);

  const auto active_model_key = storage_->GetSemanticStore().ActiveModelKey();
  const auto storage_stats    = storage_->GetElementStore().BuildFolderStats(
      parent_id, extra_predicate, active_model_key);

  AlbumStatsView out;
  out.total_photo_count_ = storage_stats.total_photo_count_;

  out.date_stats_.reserve(storage_stats.date_stats_.size());
  for (const auto& bucket : storage_stats.date_stats_) {
    out.date_stats_.push_back({bucket.label_, bucket.count_});
  }

  out.camera_stats_.reserve(storage_stats.camera_stats_.size());
  for (const auto& bucket : storage_stats.camera_stats_) {
    out.camera_stats_.push_back({bucket.label_, bucket.count_});
  }

  out.lens_stats_.reserve(storage_stats.lens_stats_.size());
  for (const auto& bucket : storage_stats.lens_stats_) {
    out.lens_stats_.push_back({bucket.label_, bucket.count_});
  }

  out.label_stats_.reserve(storage_stats.label_stats_.size());
  for (const auto& bucket : storage_stats.label_stats_) {
    out.label_stats_.push_back({bucket.label_, bucket.count_});
  }

  out.rating_stats_.reserve(storage_stats.rating_stats_.size());
  for (const auto& bucket : storage_stats.rating_stats_) {
    out.rating_stats_.push_back({bucket.label_, bucket.count_});
  }

  return out;
}

auto SleeveFilterService::BuildFuzzySearchWhere(const std::wstring& query,
                                                SearchFieldMask      mask) const
    -> std::optional<FilterNode> {
  const auto trimmed = TrimCopy(query);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (mask == 0) {
    // No field selected → nothing can match. Distinct from nullopt (which would
    // mean "no filter", i.e. match everything).
    return FilterNode{FilterNode::Type::RawSQL, FilterOp::AND, {}, std::nullopt, L"FALSE"};
  }

  const auto terms = SearchQueryParser::Parse(trimmed);
  if (terms.empty()) {
    return std::nullopt;
  }

  const auto active_model_key =
      storage_ ? storage_->GetSemanticStore().ActiveModelKey()
                       : std::string{};
  const bool has_ai_fts =
      storage_ && storage_->GetAiStore().HasUnderstandingFtsIndex();

  // Every term must match (Decision D4). There is no whole-query alternative: a name split
  // at a separator (`P263 5860`) matches because each part is in the folded file name.
  std::vector<duckorm::SqlFragment> term_clauses;
  term_clauses.reserve(terms.size());
  for (const auto& term : terms) {
    term_clauses.push_back(TermClause(term, active_model_key, mask));
  }

  auto       where = duckorm::expr::and_(term_clauses);
  // The AI FTS index body is caption + tags_json + scene concatenated; a BM25
  // hit cannot be attributed to one sub-field, so only run it when both AI
  // field groups are enabled. A single-bit AI scope uses only the per-field
  // search text clauses built above (u.caption_search_text / u.tags_search_text).
  const bool ai_fts_applicable =
      has_ai_fts && (mask & SearchField::AiDescription) && (mask & SearchField::AiTags);
  if (ai_fts_applicable) {
    where = duckorm::expr::or_({std::move(where), AiUnderstandingFtsClause(trimmed)});
  }
  // The node owns compiler output (SQL + binds). Prefer typed nodes; this bridge
  // keeps factory/search fragments ready for prepared album scope queries.
  FilterNode node{FilterNode::Type::RawSQL, FilterOp::AND, {}, std::nullopt,
                  conv::FromBytes(where.sql_)};
  node.raw_binds_ = std::move(where.binds_);
  return node;
}

auto SleeveFilterService::BuildExactFileWhere(sl_element_id_t file_id) const -> FilterNode {
  const auto fragment = duckorm::expr::eq(
      duckorm::expr::col("e.id"), duckorm::expr::param(static_cast<int64_t>(file_id)));
  FilterNode node{FilterNode::Type::RawSQL, FilterOp::AND, {}, std::nullopt,
                  conv::FromBytes(fragment.sql_)};
  node.raw_binds_ = fragment.binds_;
  return node;
}

auto SleeveFilterService::SearchFolder(sl_element_id_t parent_id, const std::wstring& query,
                                       size_t offset, size_t limit, SearchFieldMask mask) const
    -> std::vector<FuzzySearchMatch> {
  std::vector<FuzzySearchMatch> out;
  if (!storage_) {
    return out;
  }
  const auto filter_node = BuildFuzzySearchWhere(query, mask);
  if (!filter_node.has_value()) {
    return out;
  }
  const auto where = CompileFilterPredicate(filter_node);
  if (!where.has_value()) {
    return out;
  }

  const auto rows = storage_->GetElementStore().ListFilesInFolderPage(
      parent_id, offset, limit, where);
  out.reserve(rows.size());
  for (const auto& row : rows) {
    out.push_back({row.file_id_, row.image_id_, row.file_name_});
  }
  return out;
}

void SleeveFilterService::SetSemanticSearchProvider(
    std::shared_ptr<SemanticSearchProvider> provider) {
  semantic_search_provider_ = std::move(provider);
}

auto SleeveFilterService::HasSemanticSearchProvider() const -> bool {
  return semantic_search_provider_ != nullptr;
}

auto SleeveFilterService::SearchFolderSemantic(sl_element_id_t parent_id, const std::wstring& query,
                                               size_t offset, size_t limit) const
    -> std::vector<FuzzySearchMatch> {
  if (!semantic_search_provider_) {
    return {};
  }
  return semantic_search_provider_->Search(parent_id, query, offset, limit);
}

auto SleeveFilterService::CountSearchResults(sl_element_id_t     parent_id,
                                             const std::wstring& query,
                                             SearchFieldMask      mask) const -> size_t {
  if (!storage_) {
    return 0;
  }
  const auto filter_node = BuildFuzzySearchWhere(query, mask);
  if (!filter_node.has_value()) {
    return 0;
  }
  const auto where = CompileFilterPredicate(filter_node);
  if (!where.has_value()) {
    return 0;
  }
  return storage_->GetElementStore().CountFilesInFolder(parent_id, where);
}

void SleeveFilterService::InvalidateResultCache(sl_element_id_t folder_id) {
  const auto keys = filter_result_cache_.GetLRUKeys();
  for (const auto& key : keys) {
    const auto key_folder_id = static_cast<sl_element_id_t>(key >> 32U);
    if (key_folder_id == folder_id) {
      filter_result_cache_.RemoveRecord(key);
    }
  }
}

void SleeveFilterService::InvalidateResultCache() { filter_result_cache_.Flush(); }
}  // namespace alcedo
