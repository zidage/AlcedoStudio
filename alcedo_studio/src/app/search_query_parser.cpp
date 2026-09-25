//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/search_query_parser.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/string/search_text.hpp"

namespace alcedo {
namespace {

constexpr wchar_t kYearMark  = L'年';  // 年
constexpr wchar_t kMonthMark = L'月';  // 月
constexpr wchar_t kDayMark   = L'日';  // 日
constexpr wchar_t kDayMark2  = L'号';  // 号

constexpr int     kMinYear   = 1000;
constexpr int     kMaxYear   = 9999;

auto              Lowercase(std::wstring_view text) -> std::wstring {
  std::wstring out(text);
  for (auto& ch : out) {
    ch = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
  }
  return out;
}

auto IsDigit(wchar_t ch) -> bool { return ch >= L'0' && ch <= L'9'; }

auto IsAllDigits(std::wstring_view text) -> bool {
  return !text.empty() && std::ranges::all_of(text, IsDigit);
}

/// Value of a run of 1–9 ASCII digits.
auto ToInt(std::wstring_view digits) -> std::optional<int> {
  if (!IsAllDigits(digits) || digits.size() > 9) {
    return std::nullopt;
  }
  int value = 0;
  for (const auto ch : digits) {
    value = value * 10 + (ch - L'0');
  }
  return value;
}

/// Value of `digits[.digits]` (no sign, no exponent).
auto ToDecimal(std::wstring_view text) -> std::optional<double> {
  const auto dot     = text.find(L'.');
  const auto integer = text.substr(0, dot);
  const auto whole   = ToInt(integer);
  if (!whole.has_value()) {
    return std::nullopt;
  }
  if (dot != std::wstring_view::npos) {
    const auto fraction = text.substr(dot + 1);
    if (!IsAllDigits(fraction) || fraction.size() > 6) {
      return std::nullopt;
    }
  }
  // The text is validated above; std::stod gives the nearest double (2.8, not 2.8000000001).
  return std::stod(std::wstring(text));
}

auto StripTrailingPunctuation(std::wstring_view text) -> std::wstring_view {
  while (!text.empty() && (text.back() == L',' || text.back() == L'.')) {
    text.remove_suffix(1);
  }
  return text;
}

auto IsLeapYear(int year) -> bool { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

auto DaysInMonth(int year, int month) -> int {
  static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && IsLeapYear(year) ? 29 : kDays[static_cast<size_t>(month - 1)];
}

auto IsYear(int year) -> bool { return year >= kMinYear && year <= kMaxYear; }
auto IsMonth(int month) -> bool { return month >= 1 && month <= 12; }

auto MakeDay(int year, int month, int day) -> std::optional<SearchDate> {
  if (!IsYear(year) || !IsMonth(month) || day < 1 || day > DaysInMonth(year, month)) {
    return std::nullopt;
  }
  return SearchDate{SearchDateMatch::Day, year, month, day};
}

auto MakeYearMonth(int year, int month) -> std::optional<SearchDate> {
  if (!IsYear(year) || !IsMonth(month)) {
    return std::nullopt;
  }
  return SearchDate{SearchDateMatch::YearMonth, year, month, 0};
}

auto MakeYear(int year) -> std::optional<SearchDate> {
  if (!IsYear(year)) {
    return std::nullopt;
  }
  return SearchDate{SearchDateMatch::Year, year, 0, 0};
}

/// February 29 is valid: the term matches any year.
auto MakeMonthDay(int month, int day) -> std::optional<SearchDate> {
  if (!IsMonth(month) || day < 1 || day > DaysInMonth(2000, month)) {
    return std::nullopt;
  }
  return SearchDate{SearchDateMatch::MonthDay, 0, month, day};
}

auto MakeMonth(int month) -> std::optional<SearchDate> {
  if (!IsMonth(month)) {
    return std::nullopt;
  }
  return SearchDate{SearchDateMatch::Month, 0, month, 0};
}

/// A month given by its English name.
struct MonthName {
  int  month_     = 0;
  bool full_name_ = false;  ///< `june`, not `jun`.
};

auto ParseMonthName(std::wstring_view token) -> std::optional<MonthName> {
  static constexpr std::array<std::wstring_view, 12> kNames = {
      L"january", L"february", L"march",     L"april",   L"may",      L"june",
      L"july",    L"august",   L"september", L"october", L"november", L"december"};
  const auto lower = Lowercase(StripTrailingPunctuation(token));
  for (size_t i = 0; i < kNames.size(); ++i) {
    const int month = static_cast<int>(i) + 1;
    if (lower == kNames[i]) {
      return MonthName{month, true};
    }
    if (lower == kNames[i].substr(0, 3) || (month == 9 && lower == L"sept")) {
      return MonthName{month, false};
    }
  }
  return std::nullopt;
}

/// `7`, `07`, `7,`, `7th`, `1st`, `2nd`, `3rd`.
auto ParseDayNumber(std::wstring_view token) -> std::optional<int> {
  auto lower = Lowercase(StripTrailingPunctuation(token));
  for (const std::wstring_view suffix : {L"st", L"nd", L"rd", L"th"}) {
    if (lower.size() > suffix.size() && lower.ends_with(suffix)) {
      lower.resize(lower.size() - suffix.size());
      break;
    }
  }
  if (lower.size() > 2) {
    return std::nullopt;
  }
  const auto day = ToInt(lower);
  return day.has_value() && *day >= 1 && *day <= 31 ? day : std::nullopt;
}

auto ParseYearNumber(std::wstring_view token) -> std::optional<int> {
  const auto text = StripTrailingPunctuation(token);
  if (text.size() != 4) {
    return std::nullopt;
  }
  const auto year = ToInt(text);
  return year.has_value() && IsYear(*year) ? year : std::nullopt;
}

auto IsNumericDateSeparator(wchar_t ch) -> bool {
  return ch == L'-' || ch == L'.' || ch == L'/' || ch == L'_';
}

/// `Y-M-D` (any separator), `YYYY.M`, `M.D`, `YYYYMMDD`, `YYYY`.
auto ParseNumericDate(std::wstring_view token) -> std::optional<SearchDate> {
  std::vector<std::wstring_view> groups;
  size_t                         start = 0;
  for (size_t i = 0; i <= token.size(); ++i) {
    if (i < token.size() && IsDigit(token[i])) {
      continue;
    }
    if (i < token.size() && !IsNumericDateSeparator(token[i])) {
      return std::nullopt;
    }
    if (i == start) {
      return std::nullopt;  // Empty group: leading, trailing, or doubled separator.
    }
    groups.push_back(token.substr(start, i - start));
    start = i + 1;
  }

  if (groups.size() == 1) {
    const auto digits = groups[0];
    if (digits.size() == 8) {
      return MakeDay(*ToInt(digits.substr(0, 4)), *ToInt(digits.substr(4, 2)),
                     *ToInt(digits.substr(6, 2)));
    }
    if (digits.size() == 4) {
      return MakeYear(*ToInt(digits));
    }
    return std::nullopt;
  }
  if (std::ranges::any_of(groups, [](std::wstring_view group) { return group.size() > 4; })) {
    return std::nullopt;
  }
  if (groups.size() == 3 && groups[0].size() == 4 && groups[1].size() <= 2 &&
      groups[2].size() <= 2) {
    return MakeDay(*ToInt(groups[0]), *ToInt(groups[1]), *ToInt(groups[2]));
  }
  if (groups.size() == 2 && groups[0].size() == 4 && groups[1].size() <= 2) {
    return MakeYearMonth(*ToInt(groups[0]), *ToInt(groups[1]));
  }
  if (groups.size() == 2 && groups[0].size() <= 2 && groups[1].size() <= 2) {
    return MakeMonthDay(*ToInt(groups[0]), *ToInt(groups[1]));
  }
  return std::nullopt;
}

/// `YYYY年`, `YYYY年M月`, `YYYY年M月D日`, `M月`, `M月D日` (`号` is accepted for `日`).
auto ParseCjkDate(std::wstring_view token) -> std::optional<SearchDate> {
  std::optional<int> year;
  std::optional<int> month;
  std::optional<int> day;
  size_t             pos = 0;
  while (pos < token.size()) {
    size_t end = pos;
    while (end < token.size() && IsDigit(token[end])) {
      ++end;
    }
    if (end == pos || end == token.size()) {
      return std::nullopt;  // A unit mark must follow each number.
    }
    const auto value = ToInt(token.substr(pos, end - pos));
    if (!value.has_value()) {
      return std::nullopt;
    }
    const auto mark = token[end];
    if (mark == kYearMark && !year && !month && !day) {
      year = value;
    } else if (mark == kMonthMark && !month && !day) {
      month = value;
    } else if ((mark == kDayMark || mark == kDayMark2) && month && !day) {
      day = value;
    } else {
      return std::nullopt;
    }
    pos = end + 1;
  }
  if (!year && !month) {
    return std::nullopt;
  }
  if (year && month && day) {
    return MakeDay(*year, *month, *day);
  }
  if (year && month) {
    return MakeYearMonth(*year, *month);
  }
  if (year) {
    return MakeYear(*year);
  }
  if (day) {
    return MakeMonthDay(*month, *day);
  }
  return MakeMonth(*month);
}

struct CaptureParameterValue {
  CaptureParameterField field_ = CaptureParameterField::Iso;
  double                value_ = 0.0;
};

/// `iso800`, `ISO-800`, `f2.8`, `f/2.8`, `35mm`, `24.5mm`.
auto ParseCaptureParameter(std::wstring_view token) -> std::optional<CaptureParameterValue> {
  const auto lower = Lowercase(token);
  const auto view  = std::wstring_view(lower);
  if (view.starts_with(L"iso")) {
    auto rest = view.substr(3);
    while (!rest.empty() &&
           (rest.front() == L'-' || rest.front() == L':' || rest.front() == L'_')) {
      rest.remove_prefix(1);
    }
    const auto iso = ToInt(rest);
    if (iso.has_value() && *iso > 0) {
      return CaptureParameterValue{CaptureParameterField::Iso, static_cast<double>(*iso)};
    }
    return std::nullopt;
  }
  if (view.starts_with(L"f")) {
    auto rest = view.substr(1);
    if (rest.starts_with(L"/")) {
      rest.remove_prefix(1);
    }
    const auto aperture = ToDecimal(rest);
    if (aperture.has_value() && *aperture > 0.0 && *aperture <= 128.0) {
      return CaptureParameterValue{CaptureParameterField::Aperture, *aperture};
    }
    return std::nullopt;
  }
  if (view.size() > 2 && view.ends_with(L"mm")) {
    const auto focal = ToDecimal(view.substr(0, view.size() - 2));
    if (focal.has_value() && *focal > 0.0 && *focal <= 5000.0) {
      return CaptureParameterValue{CaptureParameterField::FocalLength, *focal};
    }
  }
  return std::nullopt;
}

auto FileKindExtensions(std::wstring_view token) -> std::optional<std::vector<std::string>> {
  auto lower = Lowercase(token);
  if (lower.starts_with(L".")) {
    lower.erase(lower.begin());
  }
  if (lower == L"raw") {
    return SearchQueryParser::RawFileExtensions();
  }
  if (lower == L"jpg" || lower == L"jpeg") {
    return std::vector<std::string>{"jpg", "jpeg"};
  }
  if (lower == L"tif" || lower == L"tiff") {
    return std::vector<std::string>{"tif", "tiff"};
  }
  if (lower == L"heic" || lower == L"heif") {
    return std::vector<std::string>{"heic", "heif"};
  }
  if (lower == L"png") {
    return std::vector<std::string>{"png"};
  }
  if (lower == L"webp") {
    return std::vector<std::string>{"webp"};
  }
  const auto& raw = SearchQueryParser::RawFileExtensions();
  const auto  it  = std::ranges::find_if(raw, [&lower](const std::string& extension) {
    return std::equal(extension.begin(), extension.end(), lower.begin(), lower.end());
  });
  if (it != raw.end()) {
    return std::vector<std::string>{*it};
  }
  return std::nullopt;
}

auto SplitAtWhiteSpace(std::wstring_view query) -> std::vector<std::wstring_view> {
  std::vector<std::wstring_view> tokens;
  size_t                         pos = 0;
  while (pos < query.size()) {
    while (pos < query.size() && std::iswspace(static_cast<std::wint_t>(query[pos])) != 0) {
      ++pos;
    }
    const size_t start = pos;
    while (pos < query.size() && std::iswspace(static_cast<std::wint_t>(query[pos])) == 0) {
      ++pos;
    }
    if (pos > start) {
      tokens.push_back(query.substr(start, pos - start));
    }
  }
  return tokens;
}

auto JoinTokens(std::span<const std::wstring_view> tokens) -> std::wstring {
  std::wstring out;
  for (const auto token : tokens) {
    if (!out.empty()) {
      out.push_back(L' ');
    }
    out.append(token);
  }
  return out;
}

auto MakeTerm(SearchTermKind kind, std::span<const std::wstring_view> tokens) -> SearchTerm {
  SearchTerm term;
  term.kind_        = kind;
  term.text_        = JoinTokens(tokens);
  term.folded_text_ = FoldSearchText(term.text_);
  return term;
}

auto MakeDateTerm(const SearchDate& date, std::span<const std::wstring_view> tokens) -> SearchTerm {
  auto term  = MakeTerm(SearchTermKind::Date, tokens);
  term.date_ = date;
  return term;
}

auto MakeTextTerm(std::wstring_view token) -> SearchTerm {
  auto term     = MakeTerm(SearchTermKind::Text, std::span(&token, 1));
  term.literal_ = token.find_first_of(L"%*?'\"") != std::wstring_view::npos ||
                  term.folded_text_.empty() || term.folded_text_.size() < token.size() / 2;
  return term;
}

/// A term that starts at tokens[index] and covers `token_count_` tokens.
struct ParsedTerm {
  SearchTerm term_{};
  size_t     token_count_ = 1;
};

/// Forms that span more than one token: `June 7 [2026]`, `7 June [2026]`, `June 2026`,
/// `June` (full name only), `iso 800`, `35 mm`.
auto ParseMultiTokenTerm(std::span<const std::wstring_view> tokens) -> std::optional<ParsedTerm> {
  const auto first     = tokens[0];
  const auto has_token = [&tokens](size_t index) { return index < tokens.size(); };
  const auto day_at    = [&](size_t index) -> std::optional<int> {
    return has_token(index) ? ParseDayNumber(tokens[index]) : std::nullopt;
  };
  const auto year_at = [&](size_t index) -> std::optional<int> {
    return has_token(index) ? ParseYearNumber(tokens[index]) : std::nullopt;
  };
  const auto month_at = [&](size_t index) -> std::optional<MonthName> {
    return has_token(index) ? ParseMonthName(tokens[index]) : std::nullopt;
  };
  const auto date_term = [&tokens](const std::optional<SearchDate>& date,
                                   size_t count) -> std::optional<ParsedTerm> {
    if (!date.has_value()) {
      return std::nullopt;
    }
    return ParsedTerm{MakeDateTerm(*date, tokens.first(count)), count};
  };

  if (const auto month = ParseMonthName(first); month.has_value()) {
    const auto day = day_at(1);
    if (day.has_value()) {
      if (const auto year = year_at(2); year.has_value()) {
        if (auto parsed = date_term(MakeDay(*year, month->month_, *day), 3)) {
          return parsed;
        }
      }
      if (auto parsed = date_term(MakeMonthDay(month->month_, *day), 2)) {
        return parsed;
      }
    }
    if (const auto year = year_at(1); year.has_value()) {
      return date_term(MakeYearMonth(*year, month->month_), 2);
    }
    if (month->full_name_) {
      return date_term(MakeMonth(month->month_), 1);
    }
  }

  if (const auto day = ParseDayNumber(first); day.has_value()) {
    if (const auto month = month_at(1); month.has_value()) {
      if (const auto year = year_at(2); year.has_value()) {
        if (auto parsed = date_term(MakeDay(*year, month->month_, *day), 3)) {
          return parsed;
        }
      }
      if (auto parsed = date_term(MakeMonthDay(month->month_, *day), 2)) {
        return parsed;
      }
    }
  }

  if (!has_token(1)) {
    return std::nullopt;
  }
  const auto first_lower  = Lowercase(first);
  const auto second_lower = Lowercase(tokens[1]);
  if (first_lower == L"iso" || second_lower == L"mm") {
    const auto joined = first_lower + second_lower;
    if (const auto parameter = ParseCaptureParameter(joined); parameter.has_value()) {
      auto term             = MakeTerm(SearchTermKind::CaptureParameter, tokens.first(2));
      term.parameter_       = parameter->field_;
      term.parameter_value_ = parameter->value_;
      return ParsedTerm{std::move(term), 2};
    }
  }
  return std::nullopt;
}

auto ParseSingleTokenTerm(std::wstring_view token) -> SearchTerm {
  const auto tokens = std::span(&token, 1);
  if (auto date = ParseCjkDate(token); date.has_value()) {
    return MakeDateTerm(*date, tokens);
  }
  if (auto date = ParseNumericDate(token); date.has_value()) {
    return MakeDateTerm(*date, tokens);
  }
  if (const auto parameter = ParseCaptureParameter(token); parameter.has_value()) {
    auto term             = MakeTerm(SearchTermKind::CaptureParameter, tokens);
    term.parameter_       = parameter->field_;
    term.parameter_value_ = parameter->value_;
    return term;
  }
  if (auto extensions = FileKindExtensions(token); extensions.has_value()) {
    auto term        = MakeTerm(SearchTermKind::FileKind, tokens);
    term.extensions_ = std::move(*extensions);
    return term;
  }
  return MakeTextTerm(token);
}

}  // namespace

auto SearchQueryParser::Parse(std::wstring_view query) -> std::vector<SearchTerm> {
  const auto              tokens = SplitAtWhiteSpace(query);
  std::vector<SearchTerm> terms;
  size_t                  index = 0;
  while (index < tokens.size()) {
    const auto rest = std::span(tokens).subspan(index);
    if (auto parsed = ParseMultiTokenTerm(rest); parsed.has_value()) {
      terms.push_back(std::move(parsed->term_));
      index += parsed->token_count_;
      continue;
    }
    terms.push_back(ParseSingleTokenTerm(tokens[index]));
    ++index;
  }
  return terms;
}

auto SearchQueryParser::RawFileExtensions() -> const std::vector<std::string>& {
  // Extensions of the camera RAW formats LibRaw opens. Import decides by content (Decision
  // D2a), so this list only affects the `raw` search term.
  static const std::vector<std::string> kExtensions = {
      "3fr", "arw", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "gpr",
      "iiq", "k25", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw", "orf",
      "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "x3f"};
  return kExtensions;
}

}  // namespace alcedo
