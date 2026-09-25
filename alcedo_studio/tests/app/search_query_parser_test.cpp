//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Table-driven tests for SearchQueryParser (library_search_and_project_size_plan.md, Phase S4).
// The parser is pure C++; no database is opened.

#include "app/search_query_parser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto Label(std::wstring_view query) -> std::string { return conv::ToBytes(std::wstring(query)); }

auto ParseOne(std::wstring_view query) -> SearchTerm {
  const auto terms = SearchQueryParser::Parse(query);
  EXPECT_EQ(terms.size(), 1u) << "query `" << Label(query) << "`";
  return terms.empty() ? SearchTerm{} : terms.front();
}

struct DateCase {
  std::wstring_view query_;
  SearchDate        expected_;
};

TEST(SearchQueryParserTest, DateFormsParseToTheNamedCalendarRange) {
  using enum SearchDateMatch;
  const std::vector<DateCase> cases = {
      {L"2026-06-07", {Day, 2026, 6, 7}},
      {L"2026.6.7", {Day, 2026, 6, 7}},
      {L"2026/6/7", {Day, 2026, 6, 7}},
      {L"2026_06_07", {Day, 2026, 6, 7}},
      {L"20260607", {Day, 2026, 6, 7}},
      {L"2026.6", {YearMonth, 2026, 6, 0}},
      {L"2026/05", {YearMonth, 2026, 5, 0}},
      {L"2025.12", {YearMonth, 2025, 12, 0}},
      {L"2026", {Year, 2026, 0, 0}},
      {L"6.7", {MonthDay, 0, 6, 7}},
      {L"6/7", {MonthDay, 0, 6, 7}},
      {L"6-7", {MonthDay, 0, 6, 7}},
      {L"06.07", {MonthDay, 0, 6, 7}},
      {L"2.29", {MonthDay, 0, 2, 29}},          // any year, so February 29 is valid
      {L"6月7日", {MonthDay, 0, 6, 7}},         // 6月7日
      {L"6月7号", {MonthDay, 0, 6, 7}},         // 6月7号
      {L"6月", {Month, 0, 6, 0}},               // 6月
      {L"2026年6月", {YearMonth, 2026, 6, 0}},  // 2026年6月
      {L"2026年6月7日", {Day, 2026, 6, 7}},     // 2026年6月7日
      {L"2026年", {Year, 2026, 0, 0}},          // 2026年
      {L"June 7", {MonthDay, 0, 6, 7}},
      {L"june", {Month, 0, 6, 0}},
      {L"7 June", {MonthDay, 0, 6, 7}},
      {L"June 7, 2026", {Day, 2026, 6, 7}},
      {L"7 June 2026", {Day, 2026, 6, 7}},
      {L"Jun 7", {MonthDay, 0, 6, 7}},
      {L"Sept 2025", {YearMonth, 2025, 9, 0}},
      {L"December 25th", {MonthDay, 0, 12, 25}},
  };
  for (const auto& date_case : cases) {
    const auto term = ParseOne(date_case.query_);
    EXPECT_EQ(term.kind_, SearchTermKind::Date) << Label(date_case.query_);
    EXPECT_EQ(term.date_, date_case.expected_) << Label(date_case.query_);
    EXPECT_EQ(term.text_, date_case.query_);
  }
}

TEST(SearchQueryParserTest, InvalidOrUnlistedDateFormsStayText) {
  for (const auto* query :
       {L"2.30", L"13.1", L"2026-02-30", L"2025.2.29", L"0431", L"0607", L"999", L"00011",
        L"6月32日" /* 6月32日 */, L"日" /* 日 */, L"jun", L"2026-06-", L"6..7", L"2026.6.7.8"}) {
    const auto term = ParseOne(query);
    EXPECT_EQ(term.kind_, SearchTermKind::Text) << Label(query);
  }
}

TEST(SearchQueryParserTest, DateTermKeepsFoldedTextAlternative) {
  const auto dotted = ParseOne(L"6.7");
  EXPECT_EQ(dotted.folded_text_, L"67");
  const auto month_day = ParseOne(L"June 7");
  EXPECT_EQ(month_day.text_, L"June 7");
  EXPECT_EQ(month_day.folded_text_, L"june7");
  EXPECT_FALSE(month_day.literal_);
}

TEST(SearchQueryParserTest, FileKindTermsListTheirExtensions) {
  EXPECT_EQ(ParseOne(L"jpg").extensions_, (std::vector<std::string>{"jpg", "jpeg"}));
  EXPECT_EQ(ParseOne(L"JPEG").extensions_, (std::vector<std::string>{"jpg", "jpeg"}));
  EXPECT_EQ(ParseOne(L"dng").extensions_, (std::vector<std::string>{"dng"}));
  EXPECT_EQ(ParseOne(L".NEF").extensions_, (std::vector<std::string>{"nef"}));
  EXPECT_EQ(ParseOne(L"rw2").extensions_, (std::vector<std::string>{"rw2"}));
  EXPECT_EQ(ParseOne(L"tiff").extensions_, (std::vector<std::string>{"tif", "tiff"}));

  const auto raw = ParseOne(L"RAW");
  EXPECT_EQ(raw.kind_, SearchTermKind::FileKind);
  EXPECT_EQ(raw.extensions_, SearchQueryParser::RawFileExtensions());
  for (const auto* extension : {"nef", "dng", "rw2", "cr3", "arw", "raf", "raw"}) {
    EXPECT_NE(std::ranges::find(raw.extensions_, extension), raw.extensions_.end()) << extension;
  }
  EXPECT_EQ(std::ranges::find(raw.extensions_, "jpg"), raw.extensions_.end());
  EXPECT_EQ(ParseOne(L"dng").kind_, SearchTermKind::FileKind);
  EXPECT_EQ(ParseOne(L"dng").folded_text_, L"dng");
}

struct ParameterCase {
  std::wstring_view     query_;
  CaptureParameterField field_;
  double                value_;
};

TEST(SearchQueryParserTest, CaptureParameterFormsParseToFieldAndValue) {
  using enum CaptureParameterField;
  const std::vector<ParameterCase> cases = {
      {L"iso800", Iso, 800.0},        {L"ISO 800", Iso, 800.0},     {L"ISO-800", Iso, 800.0},
      {L"f2.8", Aperture, 2.8},       {L"f/2.8", Aperture, 2.8},    {L"F8", Aperture, 8.0},
      {L"f/11", Aperture, 11.0},      {L"35mm", FocalLength, 35.0}, {L"35 mm", FocalLength, 35.0},
      {L"24.5mm", FocalLength, 24.5},
  };
  for (const auto& parameter_case : cases) {
    const auto term = ParseOne(parameter_case.query_);
    EXPECT_EQ(term.kind_, SearchTermKind::CaptureParameter) << Label(parameter_case.query_);
    EXPECT_EQ(term.parameter_, parameter_case.field_) << Label(parameter_case.query_);
    EXPECT_DOUBLE_EQ(term.parameter_value_, parameter_case.value_) << Label(parameter_case.query_);
  }
}

TEST(SearchQueryParserTest, WordsThatOnlyLookLikeParametersStayText) {
  for (const auto* query : {L"fujifilm", L"iso", L"mm", L"f", L"f0", L"23mmf14", L"xf23mm",
                            L"isomorphic", L"nikond3x01"}) {
    EXPECT_EQ(ParseOne(query).kind_, SearchTermKind::Text) << Label(query);
  }
}

TEST(SearchQueryParserTest, TextTermsFoldSeparatorsUnlessTypedLiterally) {
  const auto folded = ParseOne(L"D3X_01");
  EXPECT_EQ(folded.kind_, SearchTermKind::Text);
  EXPECT_EQ(folded.folded_text_, L"d3x01");
  EXPECT_FALSE(folded.literal_);

  // Wildcard and quote characters are kept as typed; so is a token that is mostly separators.
  for (const auto* query : {L"100%_", L"a*b", L"what?", L"it's", L"\"x\"", L"-", L"_-_a"}) {
    const auto term = ParseOne(query);
    EXPECT_EQ(term.kind_, SearchTermKind::Text) << Label(query);
    EXPECT_TRUE(term.literal_) << Label(query);
    EXPECT_EQ(term.text_, query);
  }
}

TEST(SearchQueryParserTest, QuerySplitsIntoTermsInOrder) {
  const auto terms = SearchQueryParser::Parse(L"  d810	raw 11 x June 7 iso 800  ");
  ASSERT_EQ(terms.size(), 6u);
  EXPECT_EQ(terms[0].kind_, SearchTermKind::Text);
  EXPECT_EQ(terms[0].text_, L"d810");
  EXPECT_EQ(terms[1].kind_, SearchTermKind::FileKind);
  EXPECT_EQ(terms[2].kind_, SearchTermKind::Text);
  EXPECT_EQ(terms[2].text_, L"11");
  EXPECT_EQ(terms[3].text_, L"x");
  EXPECT_EQ(terms[4].kind_, SearchTermKind::Date);
  EXPECT_EQ(terms[4].text_, L"June 7");
  EXPECT_EQ(terms[5].kind_, SearchTermKind::CaptureParameter);
  EXPECT_EQ(terms[5].text_, L"iso 800");

  // A day number directly before a month name is one date term.
  const auto day_first = SearchQueryParser::Parse(L"raw 11 June");
  ASSERT_EQ(day_first.size(), 2u);
  EXPECT_EQ(day_first[1].date_, (SearchDate{SearchDateMatch::MonthDay, 0, 6, 11}));

  EXPECT_TRUE(SearchQueryParser::Parse(L"").empty());
  EXPECT_TRUE(SearchQueryParser::Parse(L" \t\n ").empty());
}

}  // namespace
}  // namespace alcedo
