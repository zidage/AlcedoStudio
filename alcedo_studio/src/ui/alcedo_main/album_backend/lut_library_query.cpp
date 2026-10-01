//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/lut_library_query.hpp"

#include <QChar>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>
#include <vector>

namespace alcedo::ui {
namespace {

auto ToQString(const std::string& text) -> QString { return QString::fromStdString(text); }

auto IsTokenCodePoint(char32_t code_point) -> bool {
  return QChar::isLetterOrNumber(code_point) || QChar::isMark(code_point);
}

auto FileStem(const std::string& relative_path) -> QString {
  const QString path  = ToQString(relative_path);
  const int     slash = static_cast<int>(path.lastIndexOf(QLatin1Char('/')));
  QString       name  = path.mid(slash + 1);
  const int     dot   = static_cast<int>(name.lastIndexOf(QLatin1Char('.')));
  return dot > 0 ? name.left(dot) : name;
}

void AppendTokens(const QString& normalized, std::vector<QString>* tokens) {
  for (QString& token : TokenizeLutSearchText(normalized)) tokens->push_back(std::move(token));
}

/// Levenshtein distance between @p left and @p right, or `limit + 1` once it exceeds @p limit.
/// Tokens up to kStackTokenLength characters use stack rows; longer tokens use heap rows.
auto BoundedEditDistance(const QString& left, const QString& right, int limit) -> int {
  constexpr int kStackTokenLength = 63;
  const int     left_size         = static_cast<int>(left.size());
  const int     right_size        = static_cast<int>(right.size());
  if (std::abs(left_size - right_size) > limit) return limit + 1;
  std::array<int, kStackTokenLength + 1> stack_previous{};
  std::array<int, kStackTokenLength + 1> stack_current{};
  std::vector<int>                       heap_previous;
  std::vector<int>                       heap_current;
  int*                                   previous = stack_previous.data();
  int*                                   current  = stack_current.data();
  if (right_size > kStackTokenLength) {
    heap_previous.resize(static_cast<std::size_t>(right_size) + 1);
    heap_current.resize(static_cast<std::size_t>(right_size) + 1);
    previous = heap_previous.data();
    current  = heap_current.data();
  }
  const QChar* left_chars  = left.constData();
  const QChar* right_chars = right.constData();
  for (int j = 0; j <= right_size; ++j) previous[j] = j;
  for (int i = 1; i <= left_size; ++i) {
    current[0]    = i;
    int row_least = i;
    for (int j = 1; j <= right_size; ++j) {
      const int substitution =
          previous[j - 1] + (left_chars[i - 1] == right_chars[j - 1] ? 0 : 1);
      const int value = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
      current[j]      = value;
      row_least       = std::min(row_least, value);
    }
    if (row_least > limit) return limit + 1;
    std::swap(previous, current);
  }
  return previous[right_size];
}

auto EditLimitFor(const QString& token) -> int {
  const auto size = token.size();
  if (size >= 8) return 2;
  if (size >= 4) return 1;
  return 0;
}

struct TokenResult {
  LutTokenMatch match   = LutTokenMatch::kEdit;
  bool          in_name = false;
};

/// Exact or prefix match of @p token among @p tokens.
auto MatchExactOrPrefix(const QString& token, const std::vector<QString>& tokens)
    -> std::optional<LutTokenMatch> {
  std::optional<LutTokenMatch> best;
  for (const QString& candidate : tokens) {
    if (candidate == token) return LutTokenMatch::kExact;
    if (candidate.startsWith(token)) best = LutTokenMatch::kPrefix;
  }
  return best;
}

auto AnyWithinEditLimit(std::size_t token_index, const QString& token,
                        const std::vector<QString>& tokens, int limit, LutEditDistanceMemo* memo)
    -> bool {
  for (const QString& candidate : tokens) {
    const bool within = memo != nullptr
                            ? memo->WithinLimit(token_index, token, candidate, limit)
                            : BoundedEditDistance(token, candidate, limit) <= limit;
    if (within) return true;
  }
  return false;
}

auto MatchToken(std::size_t token_index, const QString& token, const LutSearchKeys& keys,
                LutEditDistanceMemo* memo) -> std::optional<TokenResult> {
  const auto in_name   = MatchExactOrPrefix(token, keys.name_tokens);
  const auto in_fields = MatchExactOrPrefix(token, keys.field_tokens);
  if (in_name && (!in_fields || *in_name <= *in_fields)) return TokenResult{*in_name, true};
  if (in_fields) return TokenResult{*in_fields, false};
  // Short tokens match only exactly or as a prefix.
  const int edit_limit = EditLimitFor(token);
  if (edit_limit == 0) return std::nullopt;
  if (keys.name_key.contains(token)) return TokenResult{LutTokenMatch::kSubstring, true};
  if (keys.text.contains(token)) return TokenResult{LutTokenMatch::kSubstring, false};
  if (AnyWithinEditLimit(token_index, token, keys.name_tokens, edit_limit, memo)) {
    return TokenResult{LutTokenMatch::kEdit, true};
  }
  if (AnyWithinEditLimit(token_index, token, keys.field_tokens, edit_limit, memo)) {
    return TokenResult{LutTokenMatch::kEdit, false};
  }
  return std::nullopt;
}

}  // namespace

auto NormalizeLutSearchText(const QString& text) -> QString {
  return text.normalized(QString::NormalizationForm_KC).toCaseFolded();
}

auto TokenizeLutSearchText(const QString& normalized) -> std::vector<QString> {
  std::vector<QString> tokens;
  QString              current;
  const QList<uint>    code_points = normalized.toUcs4();
  for (const uint value : code_points) {
    const auto code_point = static_cast<char32_t>(value);
    if (IsTokenCodePoint(code_point)) {
      current.append(QString::fromUcs4(&code_point, 1));
    } else if (!current.isEmpty()) {
      tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.isEmpty()) tokens.push_back(std::move(current));
  return tokens;
}

auto BuildLutSearchKeys(const LutLibraryEntry& entry, std::string entry_id) -> LutSearchKeys {
  LutSearchKeys keys;
  keys.entry_id      = std::move(entry_id);
  keys.relative_path = entry.relative_path;
  keys.modified_time = entry.modified_time;

  const QString display_name = ToQString(entry.DisplayName());
  keys.name_key              = NormalizeLutSearchText(display_name);
  keys.sort_name             = keys.name_key;
  QStringList texts{keys.name_key};
  AppendTokens(keys.name_key, &keys.name_tokens);
  const QString stem = NormalizeLutSearchText(FileStem(entry.relative_path));
  AppendTokens(stem, &keys.name_tokens);
  texts.push_back(stem);

  const std::optional<LutMetadata>& metadata = entry.header.metadata;
  if (entry.header_error == LutHeaderError::kNone && metadata) {
    keys.category = metadata->category;
    for (const std::string& alias : metadata->aliases) {
      const QString normalized = NormalizeLutSearchText(ToQString(alias));
      AppendTokens(normalized, &keys.name_tokens);
      texts.push_back(normalized);
    }
    auto add_field = [&](const std::string& value) {
      if (value.empty()) return;
      const QString normalized = NormalizeLutSearchText(ToQString(value));
      AppendTokens(normalized, &keys.field_tokens);
      texts.push_back(normalized);
    };
    if (metadata->source) {
      keys.source_id    = ToQString(metadata->source->id);
      keys.source_label = ToQString(metadata->source->name.empty() ? metadata->source->id
                                                                   : metadata->source->name);
      add_field(metadata->source->name);
      add_field(metadata->source->id);
    }
    if (metadata->film) {
      keys.brand_label = ToQString(metadata->film->brand);
      keys.brand_key   = NormalizeLutSearchText(keys.brand_label);
      add_field(metadata->film->brand);
      add_field(metadata->film->name);
      add_field(metadata->film->id);
    }
    if (metadata->print) {
      keys.has_print   = true;
      keys.print_label = ToQString(metadata->print->name.empty() ? metadata->print->id
                                                                 : metadata->print->name);
      keys.print_key   = NormalizeLutSearchText(
          metadata->print->id.empty() ? keys.print_label : ToQString(metadata->print->id));
      add_field(metadata->print->brand);
      add_field(metadata->print->name);
      add_field(metadata->print->id);
    }
  }
  const QString path = NormalizeLutSearchText(ToQString(entry.relative_path));
  AppendTokens(path, &keys.field_tokens);
  texts.push_back(path);
  keys.text = texts.join(QLatin1Char('\n'));
  return keys;
}

auto ParseLutSearchQuery(const QString& text) -> LutSearchQuery {
  LutSearchQuery query;
  query.normalized = NormalizeLutSearchText(text.left(kLutQueryMaxCharacters)).trimmed();
  query.tokens     = TokenizeLutSearchText(query.normalized);
  if (query.tokens.size() > static_cast<std::size_t>(kLutQueryMaxTokens)) {
    query.tokens.resize(static_cast<std::size_t>(kLutQueryMaxTokens));
  }
  return query;
}

auto LutEditDistanceMemo::WithinLimit(std::size_t token_index, const QString& token,
                                      const QString& candidate, int limit) -> bool {
  QHash<QString, bool>& results = within_limit_[token_index];
  const auto            found   = results.constFind(candidate);
  if (found != results.constEnd()) return found.value();
  const bool within = BoundedEditDistance(token, candidate, limit) <= limit;
  results.insert(candidate, within);
  return within;
}

auto RankLutSearchKeys(const LutSearchKeys& keys, const LutSearchQuery& query,
                       LutEditDistanceMemo* memo) -> std::optional<LutSearchRank> {
  LutSearchRank rank;
  if (query.Empty()) return rank;
  rank.whole_name_differs = query.normalized != keys.name_key;
  rank.weakest            = LutTokenMatch::kExact;
  for (std::size_t index = 0; index < query.tokens.size(); ++index) {
    const std::optional<TokenResult> result = MatchToken(index, query.tokens[index], keys, memo);
    if (!result) return std::nullopt;
    rank.weakest = std::max(rank.weakest, result->match);
    rank.match_sum += static_cast<int>(result->match);
    if (!result->in_name) ++rank.non_name_matches;
  }
  return rank;
}

auto LutKeysPassFilter(const LutSearchKeys& keys, bool favorite, const LutFacetFilter& filter,
                       std::optional<LutFacetDimension> ignored) -> bool {
  auto applies = [&](LutFacetDimension dimension) { return ignored != dimension; };
  if (applies(LutFacetDimension::kCategory)) {
    if (filter.category == LutCategoryFilter::kGeneral && keys.category != LutCategory::kGeneral) {
      return false;
    }
    if (filter.category == LutCategoryFilter::kFilmSimulation &&
        keys.category != LutCategory::kFilmSimulation) {
      return false;
    }
  }
  if (applies(LutFacetDimension::kSource) && !filter.source_id.isEmpty() &&
      keys.source_id != filter.source_id) {
    return false;
  }
  const bool film_predicates = filter.category != LutCategoryFilter::kGeneral;
  if (film_predicates && applies(LutFacetDimension::kBrand) && !filter.brand_key.isEmpty() &&
      keys.brand_key != filter.brand_key) {
    return false;
  }
  if (film_predicates && applies(LutFacetDimension::kPrint) && !filter.print_key.isEmpty() &&
      keys.print_key != filter.print_key) {
    return false;
  }
  if (applies(LutFacetDimension::kFavorites) && filter.favorites_only && !favorite) return false;
  return true;
}

}  // namespace alcedo::ui
