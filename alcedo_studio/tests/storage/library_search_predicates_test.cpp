//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Storage S5 (#790): storage builds the library search predicate from the parsed request and
// the project capabilities. The application gives domain values only; user text is a bound
// value of the statement.

#include "storage/query/library_search_predicates.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "app/project_service.hpp"
#include "app/search_query_parser.hpp"
#include "app/sleeve_filter_service.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/sleeve_filter/library_search_request.hpp"
#include "utils/clock/time_provider.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

using library_search_test::LibraryRootFolderId;
using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

/// The request that SleeveFilterService builds for @p query and @p fields.
auto RequestOf(const std::wstring& query, SearchFieldMask fields = kAllSearchFields)
    -> LibrarySearchRequest {
  return LibrarySearchRequest{
      .query_text = query, .terms = SearchQueryParser::Parse(query), .fields = fields};
}

auto StringBinds(const duckorm::SqlFragment& fragment) -> std::vector<std::string> {
  std::vector<std::string> values;
  for (const auto& bind : fragment.binds_) {
    if (const auto* value = std::get_if<std::string>(&bind)) {
      values.push_back(*value);
    }
  }
  return values;
}

auto Contains(const std::vector<std::string>& values, const std::string& value) -> bool {
  return std::find(values.begin(), values.end(), value) != values.end();
}

class LibrarySearchPredicatesTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    TimeProvider::Refresh();
    db_path_   = temp_dir_ / "project.db";
    meta_path_ = temp_dir_ / "project.json";
  }

  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
};

TEST_F(LibrarySearchPredicatesTest, QuoteInSearchTextIsBound) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              file_ids = builder.AddFiles({
      SyntheticImageSpec{.file_name_   = L"O'Brien_portrait.JPG",
                                      .image_path_  = L"D:/people/O'Brien_portrait.JPG",
                                      .is_raw_file_ = false},
      SyntheticImageSpec{.file_name_   = L"OBrien_street.JPG",
                                      .image_path_  = L"D:/people/OBrien_street.JPG",
                                      .is_raw_file_ = false},
      SyntheticImageSpec{.file_name_   = L"lighthouse.JPG",
                                      .image_path_  = L"D:/coast/lighthouse.JPG",
                                      .is_raw_file_ = false},
  });
  ASSERT_EQ(file_ids.size(), 3u);

  // The quote makes the term literal: it matches the names as typed, and the text is a bind.
  const auto predicate = BuildLibrarySearchPredicate(RequestOf(L"O'Brien"), SearchCapabilities{});
  EXPECT_EQ(predicate.sql_.find("O'Brien"), std::string::npos);
  EXPECT_TRUE(Contains(StringBinds(predicate), "O'Brien"));

  SleeveFilterService filter_service(project.GetStorage());
  const auto          page =
      filter_service.SearchFolderPage(LibraryRootFolderId(project), L"O'Brien", 0, 10);
  ASSERT_EQ(page.total_, 1u);
  ASSERT_EQ(page.rows_.size(), 1u);
  EXPECT_EQ(page.rows_[0].file_id_, file_ids[0]);
  EXPECT_EQ(page.rows_[0].file_name_, "O'Brien_portrait.JPG");

  // A quote that would end a literal and append a clause still selects by its text only.
  EXPECT_EQ(
      filter_service.SearchFolderPage(LibraryRootFolderId(project), L"x'OR'1'='1", 0, 10).total_,
      0u);
}

TEST_F(LibrarySearchPredicatesTest, Bm25AlternativeNeedsTheIndexAndBothAiFieldGroups) {
  const SearchCapabilities with_index{.has_ai_fts_index = true};
  const auto               bm25_of = [&](SearchFieldMask fields, const SearchCapabilities& caps) {
    return BuildLibrarySearchPredicate(RequestOf(L"lighthouse coast", fields), caps)
               .sql_.find("match_bm25") != std::string::npos;
  };
  EXPECT_TRUE(bm25_of(kAllSearchFields, with_index));
  EXPECT_TRUE(bm25_of(
      SearchField::AiDescription | static_cast<SearchFieldMask>(SearchField::AiTags), with_index));
  EXPECT_FALSE(bm25_of(static_cast<SearchFieldMask>(SearchField::AiDescription), with_index));
  EXPECT_FALSE(bm25_of(static_cast<SearchFieldMask>(SearchField::AiTags), with_index));
  EXPECT_FALSE(bm25_of(kAllSearchFields, SearchCapabilities{}));

  // The BM25 alternative matches the whole query text, bound once.
  const auto predicate = BuildLibrarySearchPredicate(RequestOf(L"lighthouse coast"), with_index);
  EXPECT_TRUE(Contains(StringBinds(predicate), "lighthouse coast"));
}

TEST_F(LibrarySearchPredicatesTest, SemanticLabelAlternativeNeedsTheActiveModelAndTheTagsField) {
  const SearchCapabilities with_model{.semantic_model_key = "model-a"};
  const auto with_label = BuildLibrarySearchPredicate(RequestOf(L"portrait"), with_model);
  EXPECT_NE(with_label.sql_.find("SemanticImageLabel"), std::string::npos);
  EXPECT_TRUE(Contains(StringBinds(with_label), "model-a"));

  EXPECT_EQ(BuildLibrarySearchPredicate(RequestOf(L"portrait"), SearchCapabilities{})
                .sql_.find("SemanticImageLabel"),
            std::string::npos);
  EXPECT_EQ(BuildLibrarySearchPredicate(
                RequestOf(L"portrait", static_cast<SearchFieldMask>(SearchField::Filename) |
                                           SearchField::AiDescription),
                with_model)
                .sql_.find("SemanticImageLabel"),
            std::string::npos);
}

TEST_F(LibrarySearchPredicatesTest, RequestWithoutFieldsMatchesNoFile) {
  const auto predicate =
      BuildLibrarySearchPredicate(RequestOf(L"Nikon", SearchFieldMask{0}), SearchCapabilities{});
  EXPECT_EQ(predicate.sql_, "FALSE");
  EXPECT_TRUE(predicate.binds_.empty());
}

TEST_F(LibrarySearchPredicatesTest, RequestWithFieldsAndNoTermIsRejected) {
  EXPECT_THROW((void)BuildLibrarySearchPredicate(
                   LibrarySearchRequest{.query_text = L"", .terms = {}, .fields = kAllSearchFields},
                   SearchCapabilities{}),
               std::invalid_argument);
}

}  // namespace
}  // namespace alcedo
