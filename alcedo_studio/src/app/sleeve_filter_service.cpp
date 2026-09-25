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
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

/// Folded form of a query token for the search text columns, or std::nullopt when the token
/// must match literally. A token with `%`, `*`, `?`, `'`, or `"` is taken as typed (these are
/// not name separators the user wants to skip), and a token that is mostly separators would
/// fold to too little text to be selective. Such tokens still match the literal clauses.
auto FoldedTokenForColumns(const std::wstring& token) -> std::optional<std::wstring> {
  if (token.find_first_of(L"%*?'\"") != std::wstring::npos) {
    return std::nullopt;
  }
  auto folded = FoldSearchText(token);
  if (folded.empty() || folded.size() < token.size() / 2) {
    return std::nullopt;
  }
  return folded;
}

/// Folded search text columns that the enabled field groups can match. `i.` columns are
/// written by ImageMapper; `u.` columns come from the AI understanding join in
/// BuildScopedFileQuery.
auto SearchTextColumns(SearchFieldMask mask) -> std::vector<std::string_view> {
  std::vector<std::string_view> columns;
  if (mask & SearchField::Filename) {
    columns.push_back("i.file_search_text");
  }
  if (mask & SearchField::Exif) {
    columns.push_back("i.exif_search_text");
  }
  if (mask & SearchField::AiDescription) {
    columns.push_back("u.caption_search_text");
  }
  if (mask & SearchField::AiTags) {
    columns.push_back("u.tags_search_text");
  }
  return columns;
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

auto SplitTokens(const std::wstring& query) -> std::vector<std::wstring> {
  std::wistringstream       stream(query);
  std::vector<std::wstring> tokens;
  std::wstring              token;
  while (stream >> token) {
    token = TrimCopy(token);
    if (!token.empty()) {
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

auto DigitsOnly(const std::wstring& value) -> std::wstring {
  std::wstring digits;
  for (const auto ch : value) {
    if (std::iswdigit(ch) != 0) {
      digits.push_back(ch);
    }
  }
  return digits;
}

auto SafeToInt(const std::wstring& value) -> std::optional<int> {
  if (value.empty() || value.size() > 9) {
    return std::nullopt;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return std::nullopt;
  }
}

auto DigitGroups(const std::wstring& value) -> std::vector<int> {
  std::vector<int> groups;
  std::wstring     current;
  for (const auto ch : value) {
    if (std::iswdigit(ch) != 0) {
      if (current.size() < 9) {
        current.push_back(ch);
      }
      continue;
    }
    if (!current.empty()) {
      if (const auto parsed = SafeToInt(current); parsed.has_value()) {
        groups.push_back(*parsed);
      }
      current.clear();
    }
  }
  if (!current.empty()) {
    if (const auto parsed = SafeToInt(current); parsed.has_value()) {
      groups.push_back(*parsed);
    }
  }
  return groups;
}

auto IsValidMonth(int month) -> bool { return month >= 1 && month <= 12; }

auto IsValidDay(int day) -> bool { return day >= 1 && day <= 31; }

auto DateLiteral(int year, int month, int day) -> std::wstring {
  return std::format(L"{:04}-{:02}-{:02}", year, month, day);
}

auto NextMonthStart(int year, int month) -> std::wstring {
  if (month >= 12) {
    return DateLiteral(year + 1, 1, 1);
  }
  return DateLiteral(year, month + 1, 1);
}

auto DateColumn() -> duckorm::SqlFragment { return duckorm::expr::raw("i.capture_date"); }

auto DateValue(int year, int month, int day) -> duckorm::SqlFragment {
  return duckorm::expr::raw(
      "DATE " + duckorm::expr::lit(conv::ToBytes(DateLiteral(year, month, day))).sql_);
}

auto DateMatchClauses(const std::wstring& token) -> std::vector<duckorm::SqlFragment> {
  namespace expr = duckorm::expr;

  std::vector<duckorm::SqlFragment> clauses;
  const auto                        digits = DigitsOnly(token);
  const auto                        groups = DigitGroups(token);
  const auto                        col    = DateColumn();

  auto add_exact = [&](int year, int month, int day) {
    if (year >= 1000 && IsValidMonth(month) && IsValidDay(day)) {
      clauses.push_back(expr::eq(col, DateValue(year, month, day)));
    }
  };
  auto add_month = [&](int year, int month) {
    if (year >= 1000 && IsValidMonth(month)) {
      clauses.push_back(expr::and_(
          {expr::ge(col, DateValue(year, month, 1)),
           expr::lt(col, DateValue(year, month + 1, 1))}));
    }
  };
  auto add_year = [&](int year) {
    if (year >= 1000) {
      clauses.push_back(expr::and_(
          {expr::ge(col, DateValue(year, 1, 1)), expr::lt(col, DateValue(year + 1, 1, 1))}));
    }
  };

  if (digits.size() == 8) {
    const auto year  = SafeToInt(digits.substr(0, 4));
    const auto month = SafeToInt(digits.substr(4, 2));
    const auto day   = SafeToInt(digits.substr(6, 2));
    if (year.has_value() && month.has_value() && day.has_value()) {
      add_exact(*year, *month, *day);
    }
  } else if (digits.size() == 6) {
    const auto yy    = SafeToInt(digits.substr(0, 2));
    const auto month = SafeToInt(digits.substr(2, 2));
    const auto day   = SafeToInt(digits.substr(4, 2));
    if (yy.has_value() && month.has_value() && day.has_value()) {
      add_exact(*yy >= 70 ? 1900 + *yy : 2000 + *yy, *month, *day);
    }
  } else if (digits.size() == 4 && token.size() == 4) {
    if (const auto year = SafeToInt(digits); year.has_value()) {
      add_year(*year);
    }
  }

  if (groups.size() >= 3) {
    add_exact(groups[0], groups[1], groups[2]);
  } else if (groups.size() == 2) {
    add_month(groups[0], groups[1]);
  } else if (groups.size() == 1 && digits.size() == 4 && groups[0] >= 1000) {
    add_year(groups[0]);
  }

  // The date text itself is matched through the folded `i.exif_search_text` column.
  return clauses;
}

auto TokenSearchClause(const std::wstring& token, const std::string& active_model_key,
                       SearchFieldMask mask) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;

  std::vector<duckorm::SqlFragment> clauses;
  if (const auto folded_token = FoldedTokenForColumns(token); folded_token.has_value()) {
    for (const auto column : SearchTextColumns(mask)) {
      clauses.push_back(FoldedColumnContains(column, *folded_token));
    }
  }
  if (mask & SearchField::Filename) {
    // Literal matches for a token that is not folded (for example `100%_`). The Element name
    // is not an Image column; a renamed library file keeps its own name.
    clauses.push_back(ContainsClause(expr::raw("e.element_name"), token));
    clauses.push_back(ContainsClause(expr::raw("i.file_name"), token));
  }
  if (mask & SearchField::Exif) {
    clauses.push_back(ContainsClause(expr::raw("i.camera_make"), token));
    clauses.push_back(ContainsClause(expr::raw("i.camera_model"), token));
    clauses.push_back(ContainsClause(expr::raw("i.lens"), token));
    clauses.push_back(ContainsClause(expr::raw("CAST(i.iso AS VARCHAR)"), token));
    clauses.push_back(ContainsClause(expr::raw("CAST(i.focal_mm AS VARCHAR)"), token));
    clauses.push_back(ContainsClause(expr::raw("CAST(i.aperture AS VARCHAR)"), token));
    auto date_clauses = DateMatchClauses(token);
    clauses.insert(clauses.end(), std::make_move_iterator(date_clauses.begin()),
                   std::make_move_iterator(date_clauses.end()));
  }
  if (mask & SearchField::AiTags) {
    if (auto label_clause = SemanticLabelClause(token, active_model_key);
        label_clause.has_value()) {
      clauses.push_back(std::move(*label_clause));
    }
  }
  if (clauses.empty()) {
    return expr::raw("1=0");
  }
  return expr::or_(clauses);
}

// Whole-query alternative for a multi-token query: the folded query (separators and spaces
// removed) is part of one enabled search text column, so `P263 5860` matches `P2635860.RW2`.
auto SearchDocumentClause(const std::wstring& query, SearchFieldMask mask) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;

  std::vector<duckorm::SqlFragment> clauses;
  if (const auto folded_query = FoldedTokenForColumns(query); folded_query.has_value()) {
    for (const auto column : SearchTextColumns(mask)) {
      clauses.push_back(FoldedColumnContains(column, *folded_query));
    }
  }
  if (mask & SearchField::Filename) {
    clauses.push_back(ContainsClause(expr::raw("e.element_name"), query));
    clauses.push_back(ContainsClause(expr::raw("i.file_name"), query));
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

  auto tokens = SplitTokens(trimmed);
  if (tokens.empty()) {
    return std::nullopt;
  }

  const auto active_model_key =
      storage_ ? storage_->GetSemanticStore().ActiveModelKey()
                       : std::string{};
  const bool has_ai_fts =
      storage_ && storage_->GetAiStore().HasUnderstandingFtsIndex();

  std::vector<duckorm::SqlFragment> token_clauses;
  token_clauses.reserve(tokens.size());
  for (const auto& token : tokens) {
    token_clauses.push_back(TokenSearchClause(token, active_model_key, mask));
  }

  auto where = duckorm::expr::and_(token_clauses);
  if (tokens.size() > 1) {
    where = duckorm::expr::or_({std::move(where), SearchDocumentClause(trimmed, mask)});
  }
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
