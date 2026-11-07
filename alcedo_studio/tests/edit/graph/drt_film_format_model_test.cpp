//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The film format of the DRT node (plan 'film_grain_format_and_stock_plan.md', phase G2). The key
// `film_format` is written only when the format is not 35mm, so a document made before the field
// keeps its bytes.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

TEST(DrtFilmFormatModel, DefaultDocumentReads35mmAndWritesNoFilmFormatKey) {
  const auto document = CreateDefaultPipelineDocument();
  EXPECT_EQ(document.Drt()->Params().FilmFormat(), FilmFormat::Film35mm);
  EXPECT_FALSE(document.Drt()->Params().ToJson().contains("film_format"));
  EXPECT_TRUE(document.Drt()->Params().IsDefault());
}

TEST(DrtFilmFormatModel, EveryFormatRoundTripsThroughDocumentJson) {
  for (const auto format : {FilmFormat::Film16mm, FilmFormat::Film35mm, FilmFormat::Film70mm}) {
    SCOPED_TRACE(std::string{FilmFormatId(format)});
    auto document = CreateDefaultPipelineDocument();
    document.Drt()->Params().ApplyFilmFormat(format);
    const auto json = document.Drt()->Params().ToJson();
    if (format == FilmFormat::Film35mm) {
      EXPECT_FALSE(json.contains("film_format"));
    } else {
      ASSERT_TRUE(json.contains("film_format"));
      EXPECT_EQ(json.at("film_format"), std::string{FilmFormatId(format)});
      EXPECT_FALSE(document.Drt()->Params().IsDefault());
    }
    const auto restored = PipelineDocument::FromJson(document.ToJson());
    EXPECT_EQ(restored.Drt()->Params().FilmFormat(), format);
    EXPECT_EQ(restored.ToJson().dump(), document.ToJson().dump());
  }
}

TEST(DrtFilmFormatModel, Return35mmRemovesTheKeyAndGivesTheDefaultBytes) {
  auto       document      = CreateDefaultPipelineDocument();
  const auto default_bytes = document.ToJson().dump();
  document.Drt()->Params().ApplyFilmFormat(FilmFormat::Film16mm);
  EXPECT_NE(document.ToJson().dump(), default_bytes);
  document.Drt()->Params().ApplyFilmFormat(FilmFormat::Film35mm);
  EXPECT_EQ(document.ToJson().dump(), default_bytes);
}

TEST(DrtFilmFormatModel, OutputTransformJsonExcludesFilmFormat) {
  auto document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyFilmFormat(FilmFormat::Film70mm);
  auto without = document.Drt()->Params().ToJson();
  without.erase("film_format");
  EXPECT_EQ(document.Drt()->Params().OutputTransformJson(), without);
}

TEST(DrtFilmFormatModel, FormatChangeMarksOnlyTheFilmFormatField) {
  auto       document      = CreateDefaultPipelineDocument();
  auto&      params        = document.Drt()->Params();
  const auto other_fields  = DirtyFieldMask{static_cast<std::uint64_t>(DrtDirty::All) &
                                           ~static_cast<std::uint64_t>(DrtDirty::FilmFormat)};
  const auto other_before  = params.FieldsRevision(other_fields);
  const auto format_before = params.FieldsRevision(DirtyFieldMask{DrtDirty::FilmFormat});
  params.ApplyFilmFormat(FilmFormat::Film16mm);
  EXPECT_GT(params.FieldsRevision(DirtyFieldMask{DrtDirty::FilmFormat}), format_before);
  EXPECT_EQ(params.FieldsRevision(other_fields), other_before);

  const auto revision = params.Revision();
  params.ApplyFilmFormat(FilmFormat::Film16mm);
  EXPECT_EQ(params.Revision(), revision);
}

TEST(DrtFilmFormatModel, ValueOutsideTheEnumThrowsWithTheFieldNameAndChangesNothing) {
  DrtParamsModel params;
  params.ApplyFilmFormat(FilmFormat::Film70mm);
  const auto revision = params.Revision();
  try {
    params.ApplyFilmFormat(static_cast<FilmFormat>(7));
    FAIL() << "a film format outside the enum was accepted";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string{error.what()}.find("film_format"), std::string::npos);
  }
  EXPECT_EQ(params.FilmFormat(), FilmFormat::Film70mm);
  EXPECT_EQ(params.Revision(), revision);
}

TEST(DrtFilmFormatModel, LoadRejectsAnInvalidStoredFormatAndNamesTheKey) {
  for (const auto& value : {nlohmann::json("8mm"), nlohmann::json(16), nlohmann::json("35MM")}) {
    SCOPED_TRACE(value.dump());
    auto json           = CreateDefaultPipelineDocument().Drt()->Params().ToJson();
    json["film_format"] = value;
    DrtParamsModel params;
    params.ApplyFilmFormat(FilmFormat::Film70mm);
    params.ApplyDiffusionStrength(0.4f);
    try {
      params.LoadJson(json);
      FAIL() << "an invalid film_format was accepted";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string{error.what()}.find("film_format"), std::string::npos);
    }
    EXPECT_EQ(params.FilmFormat(), FilmFormat::Film70mm);
    EXPECT_EQ(params.DiffusionStrength(), 0.4f);
  }
}

TEST(DrtFilmFormatModel, LoadWithoutTheKeyReads35mm) {
  DrtParamsModel params;
  params.ApplyFilmFormat(FilmFormat::Film16mm);
  params.LoadJson(CreateDefaultPipelineDocument().Drt()->Params().ToJson());
  EXPECT_EQ(params.FilmFormat(), FilmFormat::Film35mm);
}

}  // namespace
}  // namespace alcedo
