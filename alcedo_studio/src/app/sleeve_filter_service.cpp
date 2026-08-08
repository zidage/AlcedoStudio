//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_filter_service.hpp"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <format>
#include <memory>
#include <optional>
#include <sstream>

#include "storage/controller/semantic/semantic_label_config.hpp"
#include "utils/string/convert.hpp"

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

auto SqlLikeEscape(const std::wstring& value) -> std::wstring {
  std::wstring out;
  out.reserve(value.size() + 4);
  for (const auto ch : value) {
    if (ch == L'~' || ch == L'%' || ch == L'_') {
      out.push_back(L'~');
    }
    out.push_back(ch);
    if (ch == L'\'') {
      out.push_back(L'\'');
    }
  }
  return out;
}

auto SqlStringEscape(const std::wstring& value) -> std::wstring {
  std::wstring out;
  out.reserve(value.size());
  for (const auto ch : value) {
    out.push_back(ch);
    if (ch == L'\'') {
      out.push_back(L'\'');
    }
  }
  return out;
}

auto SqlStringLiteral(const std::string& value) -> std::wstring {
  return L"'" + SqlStringEscape(conv::FromBytes(value)) + L"'";
}

auto SqlStringLiteral(const std::wstring& value) -> std::wstring {
  return L"'" + SqlStringEscape(value) + L"'";
}

auto WStringToUtf8(const std::wstring& value) -> std::optional<std::string> {
  try {
    return conv::ToBytes(value);
  } catch (...) {
    return std::nullopt;
  }
}

auto LikeClause(const std::wstring& expr, const std::wstring& token) -> std::wstring {
  const auto escaped = SqlStringEscape(token);
  const auto value   = std::format(L"COALESCE({}, '')", expr);
  const auto needle  = std::format(L"'{}'", escaped);
  return std::format(L"(contains({}, {}) OR contains(LOWER({}), LOWER({})))", value, needle, value,
                     needle);
}

auto StripSearchSeparators(std::wstring value) -> std::wstring {
  std::wstring out;
  out.reserve(value.size());
  for (const auto ch : value) {
    switch (ch) {
      case L' ':
      case L'\t':
      case L'\n':
      case L'\r':
      case L'_':
      case L'-':
      case L'.':
      case L'/':
      case L'\\':
      case L':':
      case L';':
      case L',':
      case L'\'':
      case L'"':
      case L'(':
      case L')':
      case L'[':
      case L']':
      case L'{':
      case L'}':
      case L'%':
      case L'*':
      case L'?':
      case L'!':
      case L'@':
      case L'#':
      case L'$':
      case L'&':
      case L'+':
      case L'=':
      case L'|':
      case L'`':
      case L'~':
        break;
      default:
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
        break;
    }
  }
  return out;
}

auto FoldSqlSearchSeparators(std::wstring expr) -> std::wstring {
  static constexpr std::wstring_view kSeparators[] = {
      L" ", L"\t", L"\n", L"\r", L"_", L"-", L".", L"/", L"\\", L":", L";",
      L",", L"'",  L"\"", L"(",  L")", L"[", L"]", L"{", L"}",  L"%", L"*",
      L"?", L"!",  L"@",  L"#",  L"$", L"&", L"+", L"=", L"|",  L"`", L"~",
  };

  std::wstring folded = std::format(L"LOWER(COALESCE({}, ''))", expr);
  for (const auto separator : kSeparators) {
    folded =
        std::format(L"REPLACE({}, '{}', '')", folded, SqlStringEscape(std::wstring(separator)));
  }
  return folded;
}

auto SemanticLabelExpr(const std::string& active_model_key) -> std::wstring {
  if (active_model_key.empty()) {
    return L"''";
  }
  std::wstring alias_case = L"CASE";
  for (const auto& label : DefaultSemanticPhotographyLabelDefinitions()) {
    const auto canonical = conv::FromBytes(label.canonical_label);
    const auto en        = conv::FromBytes(label.english_label);
    const auto zh        = conv::FromBytes(label.chinese_label);
    const auto aliases   = SqlStringLiteral(canonical + L" " + en + L" " + zh);
    alias_case +=
        L" WHEN LOWER(sl.label) = LOWER(" + SqlStringLiteral(canonical) + L") THEN " + aliases;
    alias_case += L" WHEN LOWER(sl.label) = LOWER(" + SqlStringLiteral(en) + L") THEN " + aliases;
    alias_case += L" WHEN LOWER(sl.label) = LOWER(" + SqlStringLiteral(zh) + L") THEN " + aliases;
  }
  alias_case += L" ELSE sl.label END";
  return L"(SELECT string_agg(" + alias_case +
         L", ' ') FROM SemanticImageLabel sl WHERE sl.file_id = e.id "
         L"AND sl.model_key = " +
         SqlStringLiteral(active_model_key) + L")";
}

// Phase 5f: active AI image understanding (caption + tags + scene) participates in
// full-text search; the remote LLM rating does NOT (it is a subjective 1..5 score
// exposed for sort/filter only). This is a correlated subquery against the outer
// `Element e` row (e.id is the file id / inode the AI rows bind to). tags_json is a JSON
// array string (e.g. ["sahara","dunes"]); it is concatenated raw and the search
// separator-folding (FoldSqlSearchSeparators) strips the JSON syntax characters so the
// tag words become searchable. string_agg over zero rows is NULL, so COALESCE turns a
// file with no AI understanding into an empty document contribution. Only
// active-for-search rows participate, so a failed/partial remote call that was never
// persisted (or a deactivated row) cannot surface in search.
// JoinWith is defined further down; forward-declared so the masked document
// builder below can compose field parts before the helper is in scope.
auto JoinWith(const std::vector<std::wstring>& parts, const std::wstring& sep) -> std::wstring;

// Phase 5f's AI understanding is split so the search-settings drawer can scope to
// "AI description" (caption + scene — the descriptive prose) independently of
// "AI tags" (the tags_json array). Each is a correlated subquery against the
// outer `Element e` row (e.id is the file id / inode the AI rows bind to).
// string_agg over zero active rows is NULL, so COALESCE at the call site turns
// a file with no AI understanding into an empty contribution. Only
// active-for-search rows participate, so a failed/partial remote call that was
// never persisted (or a deactivated row) cannot surface in search.
auto AiCaptionExpr() -> std::wstring {
  return L"(SELECT string_agg(u.caption || ' ' || u.scene, ' ') "
         L"FROM AiImageUnderstanding u WHERE u.file_id = e.id AND u.active = TRUE)";
}
auto AiTagsExpr() -> std::wstring {
  return L"(SELECT string_agg(u.tags_json, ' ') "
         L"FROM AiImageUnderstanding u WHERE u.file_id = e.id AND u.active = TRUE)";
}

// Concatenates the enabled field groups into one search document. The folded
// separator-match path and the whole-query LIKE both run against this. When no
// field is enabled the result is the empty string; callers guard mask == 0
// before reaching here so the empty-document case never reaches SQL.
auto SearchDocumentExpr(const std::string& active_model_key, SearchFieldMask mask)
    -> std::wstring {
  std::vector<std::wstring> parts;
  if (mask & SearchField::Filename) {
    parts.push_back(L"COALESCE(e.element_name, '')");
    parts.push_back(L"COALESCE(i.file_name, '')");
    parts.push_back(L"COALESCE(i.image_path, '')");
  }
  if (mask & SearchField::Exif) {
    parts.push_back(L"COALESCE(json_extract_string(i.metadata, '$.Make'), '')");
    parts.push_back(L"COALESCE(json_extract_string(i.metadata, '$.Model'), '')");
    parts.push_back(L"COALESCE(json_extract_string(i.metadata, '$.Lens'), '')");
    parts.push_back(L"COALESCE(json_extract_string(i.metadata, '$.LensMake'), '')");
    parts.push_back(L"COALESCE(json_extract_string(i.metadata, '$.DateTimeString'), '')");
    parts.push_back(L"COALESCE(CAST(i.metadata AS VARCHAR), '')");
  }
  if (mask & SearchField::AiDescription) {
    parts.push_back(L"COALESCE(" + AiCaptionExpr() + L", '')");
  }
  if (mask & SearchField::AiTags) {
    parts.push_back(L"COALESCE(" + SemanticLabelExpr(active_model_key) + L", '')");
    parts.push_back(L"COALESCE(" + AiTagsExpr() + L", '')");
  }
  if (parts.empty()) {
    return L"''";
  }
  return L"CONCAT_WS(' ', " + JoinWith(parts, L", ") + L")";
}

auto FoldedDocumentClause(const std::wstring& token, const std::string& active_model_key,
                           SearchFieldMask mask) -> std::optional<std::wstring> {
  if (mask == 0) {
    return std::nullopt;
  }
  if (token.find(L'%') != std::wstring::npos || token.find(L'*') != std::wstring::npos ||
      token.find(L'?') != std::wstring::npos || token.find(L'\'') != std::wstring::npos ||
      token.find(L'"') != std::wstring::npos) {
    return std::nullopt;
  }

  const auto folded_token = StripSearchSeparators(token);
  if (folded_token.size() < 2 || folded_token.size() < token.size() / 2) {
    return std::nullopt;
  }

  const auto folded_doc = FoldSqlSearchSeparators(SearchDocumentExpr(active_model_key, mask));
  const auto pattern    = std::format(L"'%{}%'", SqlLikeEscape(folded_token));
  return std::format(L"({} LIKE {} ESCAPE '~')", folded_doc, pattern);
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

auto DateColumn() -> std::wstring {
  return L"TRY_CAST(json_extract_string(i.metadata, '$.DateTimeString') AS DATE)";
}

auto DateMatchClauses(const std::wstring& token) -> std::vector<std::wstring> {
  std::vector<std::wstring> clauses;
  const auto                digits    = DigitsOnly(token);
  const auto                groups    = DigitGroups(token);
  const auto                col       = DateColumn();

  auto                      add_exact = [&](int year, int month, int day) {
    if (year >= 1000 && IsValidMonth(month) && IsValidDay(day)) {
      clauses.push_back(std::format(L"({} = DATE '{}')", col, DateLiteral(year, month, day)));
    }
  };
  auto add_month = [&](int year, int month) {
    if (year >= 1000 && IsValidMonth(month)) {
      clauses.push_back(std::format(L"({} >= DATE '{}' AND {} < DATE '{}')", col,
                                    DateLiteral(year, month, 1), col, NextMonthStart(year, month)));
    }
  };
  auto add_year = [&](int year) {
    if (year >= 1000) {
      clauses.push_back(std::format(L"({} >= DATE '{}' AND {} < DATE '{}')", col,
                                    DateLiteral(year, 1, 1), col, DateLiteral(year + 1, 1, 1)));
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

  clauses.push_back(LikeClause(L"json_extract_string(i.metadata, '$.DateTimeString')", token));
  return clauses;
}

auto JoinWith(const std::vector<std::wstring>& parts, const std::wstring& sep) -> std::wstring {
  std::wstring out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += sep;
    }
    out += parts[i];
  }
  return out;
}

auto TokenSearchClause(const std::wstring& token, const std::string& active_model_key,
                       SearchFieldMask mask) -> std::wstring {
  std::vector<std::wstring> search_terms{token};
  if (const auto token_u8 = WStringToUtf8(token); token_u8.has_value()) {
    if (const auto canonical = CanonicalSemanticLabel(*token_u8); canonical.has_value()) {
      const auto canonical_w = conv::FromBytes(*canonical);
      if (std::ranges::find(search_terms, canonical_w) == search_terms.end()) {
        search_terms.push_back(canonical_w);
      }
      for (const auto& alias : SemanticLabelAliases(*canonical)) {
        const auto alias_w = conv::FromBytes(alias);
        if (std::ranges::find(search_terms, alias_w) == search_terms.end()) {
          search_terms.push_back(alias_w);
        }
      }
    }
  }

  std::vector<std::wstring> clauses;
  if (mask & SearchField::Filename) {
    clauses.push_back(LikeClause(L"e.element_name", token));
    clauses.push_back(LikeClause(L"i.file_name", token));
    clauses.push_back(LikeClause(L"i.image_path", token));
  }
  if (mask & SearchField::Exif) {
    clauses.push_back(LikeClause(L"json_extract_string(i.metadata, '$.Make')", token));
    clauses.push_back(LikeClause(L"json_extract_string(i.metadata, '$.Model')", token));
    clauses.push_back(LikeClause(L"json_extract_string(i.metadata, '$.Lens')", token));
    clauses.push_back(LikeClause(L"json_extract_string(i.metadata, '$.LensMake')", token));
    clauses.push_back(LikeClause(L"CAST(i.metadata AS VARCHAR)", token));
    clauses.push_back(LikeClause(L"CAST(json_extract(i.metadata, '$.ISO') AS VARCHAR)", token));
    clauses.push_back(
        LikeClause(L"CAST(json_extract(i.metadata, '$.FocalLength') AS VARCHAR)", token));
    clauses.push_back(
        LikeClause(L"CAST(json_extract(i.metadata, '$.Aperture') AS VARCHAR)", token));
    auto date_clauses = DateMatchClauses(token);
    clauses.insert(clauses.end(), date_clauses.begin(), date_clauses.end());
  }
  if (mask & SearchField::AiDescription) {
    clauses.push_back(LikeClause(AiCaptionExpr(), token));
  }
  if (mask & SearchField::AiTags) {
    clauses.push_back(LikeClause(AiTagsExpr(), token));
    if (!active_model_key.empty()) {
      for (const auto& term : search_terms) {
        clauses.push_back(LikeClause(SemanticLabelExpr(active_model_key), term));
      }
    }
  }

  if (auto folded_clause = FoldedDocumentClause(token, active_model_key, mask);
      folded_clause.has_value()) {
    clauses.push_back(*folded_clause);
  }

  return L"(" + JoinWith(clauses, L" OR ") + L")";
}

auto SearchDocumentClause(const std::wstring& query, const std::string& active_model_key,
                           SearchFieldMask mask) -> std::wstring {
  std::vector<std::wstring> clauses;
  if (mask != 0) {
    clauses.push_back(LikeClause(SearchDocumentExpr(active_model_key, mask), query));
  }
  if (auto folded_clause = FoldedDocumentClause(query, active_model_key, mask);
      folded_clause.has_value()) {
    clauses.push_back(*folded_clause);
  }
  if (clauses.empty()) {
    return L"(1=0)";
  }
  return L"(" + JoinWith(clauses, L" OR ") + L")";
}

auto AiUnderstandingFtsClause(const std::wstring& query) -> std::wstring {
  return L"(fts_main_AiImageFtsDocument.match_bm25(e.id, " + SqlStringLiteral(query) +
         L") IS NOT NULL)";
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
      storage_service_->GetElementController().GetElementIdsInFolderByFilter(combo, parent_id);
  // Cache the result for future use.
  filter_result_cache_.RecordAccess(cache_key, result_ids);
  return result_ids;
}

auto SleeveFilterService::BuildFolderStats(sl_element_id_t                  parent_id,
                                           const std::optional<FilterNode>& extra_filter) const
    -> AlbumStatsView {
  std::optional<std::wstring> extra_where;
  if (extra_filter.has_value()) {
    const auto where_frag = FilterSQLCompiler::Compile(*extra_filter);
    if (!where_frag.empty()) {
      extra_where = conv::FromBytes(where_frag.sql_);
    }
  }

  const auto active_model_key = storage_service_->GetSemanticStorageController().ActiveModelKey();
  const auto storage_stats    = storage_service_->GetElementController().BuildFolderStats(
      parent_id, extra_where, active_model_key);

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
    -> std::optional<std::wstring> {
  const auto trimmed = TrimCopy(query);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (mask == 0) {
    // No field selected → nothing can match. Distinct from nullopt (which would
    // mean "no filter", i.e. match everything).
    return L"FALSE";
  }

  auto tokens = SplitTokens(trimmed);
  if (tokens.empty()) {
    return std::nullopt;
  }

  const auto active_model_key =
      storage_service_ ? storage_service_->GetSemanticStorageController().ActiveModelKey()
                       : std::string{};
  const bool has_ai_fts =
      storage_service_ && storage_service_->GetAiStorageController().HasUnderstandingFtsIndex();

  std::vector<std::wstring> token_clauses;
  token_clauses.reserve(tokens.size());
  for (const auto& token : tokens) {
    token_clauses.push_back(TokenSearchClause(token, active_model_key, mask));
  }

  std::wstring where = L"(" + JoinWith(token_clauses, L" AND ") + L")";
  if (tokens.size() > 1) {
    where = L"(" + where + L" OR " + SearchDocumentClause(trimmed, active_model_key, mask) + L")";
  }
  // The AI FTS index body is caption + tags_json + scene concatenated; a BM25
  // hit cannot be attributed to one sub-field, so only run it when both AI
  // field groups are enabled. A single-bit AI scope falls back to the
  // per-field LIKE clauses built above (AiCaptionExpr / AiTagsExpr).
  const bool ai_fts_applicable =
      has_ai_fts && (mask & SearchField::AiDescription) && (mask & SearchField::AiTags);
  if (ai_fts_applicable) {
    where = L"(" + where + L" OR " + AiUnderstandingFtsClause(trimmed) + L")";
  }
  return where;
}

auto SleeveFilterService::BuildExactFileWhere(sl_element_id_t file_id) const -> std::wstring {
  return std::format(L"e.id = {}", file_id);
}

auto SleeveFilterService::SearchFolder(sl_element_id_t parent_id, const std::wstring& query,
                                       size_t offset, size_t limit, SearchFieldMask mask) const
    -> std::vector<FuzzySearchMatch> {
  std::vector<FuzzySearchMatch> out;
  if (!storage_service_) {
    return out;
  }
  const auto where = BuildFuzzySearchWhere(query, mask);
  if (!where.has_value()) {
    return out;
  }

  const auto rows = storage_service_->GetElementController().ListFilesInFolderPage(
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
  if (!storage_service_) {
    return 0;
  }
  const auto where = BuildFuzzySearchWhere(query, mask);
  if (!where.has_value()) {
    return 0;
  }
  return storage_service_->GetElementController().CountFilesInFolder(parent_id, where);
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
