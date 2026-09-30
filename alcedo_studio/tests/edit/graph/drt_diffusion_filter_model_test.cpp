//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

TEST(DrtDiffusionFilterModel, DefaultDocumentHasZeroStrengthAndWritesNoDiffusionKey) {
  const auto document = CreateDefaultPipelineDocument();
  EXPECT_EQ(document.Drt()->Params().DiffusionStrength(), 0.0f);
  EXPECT_FALSE(document.Drt()->Params().ToJson().contains("diffusion"));
  EXPECT_TRUE(document.Drt()->Params().IsDefault());
}

TEST(DrtDiffusionFilterModel, StrengthRoundTripsThroughDocumentJsonAsTheOnlyStoredKey) {
  auto document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(0.35f);
  const auto json = document.Drt()->Params().ToJson();
  ASSERT_TRUE(json.contains("diffusion"));
  EXPECT_EQ(json.at("diffusion"), (nlohmann::json{{"strength", 0.35f}}));

  const auto restored = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(restored.Drt()->Params().DiffusionStrength(), 0.35f);
  EXPECT_FALSE(restored.Drt()->Params().IsDefault());
}

TEST(DrtDiffusionFilterModel, OutputTransformJsonExcludesDiffusionStrength) {
  auto document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(0.5f);
  auto without = document.Drt()->Params().ToJson();
  without.erase("diffusion");
  EXPECT_EQ(document.Drt()->Params().OutputTransformJson(), without);
}

TEST(DrtDiffusionFilterModel, StrengthChangeMarksOnlyDiffusionField) {
  auto       document = CreateDefaultPipelineDocument();
  auto&      params   = document.Drt()->Params();
  const auto output_fields =
      DirtyFieldMask{static_cast<std::uint64_t>(DrtDirty::Method) |
                     static_cast<std::uint64_t>(DrtDirty::Encoding) |
                     static_cast<std::uint64_t>(DrtDirty::OpenDrt)};
  const auto output_before    = params.FieldsRevision(output_fields);
  const auto diffusion_before = params.FieldsRevision(DirtyFieldMask{DrtDirty::Diffusion});
  params.ApplyDiffusionStrength(0.2f);
  EXPECT_GT(params.FieldsRevision(DirtyFieldMask{DrtDirty::Diffusion}), diffusion_before);
  EXPECT_EQ(params.FieldsRevision(output_fields), output_before);
}

TEST(DrtDiffusionFilterModel, EqualStrengthKeepsRevision) {
  auto document = CreateDefaultPipelineDocument();
  auto& params  = document.Drt()->Params();
  params.ApplyDiffusionStrength(0.4f);
  const auto revision = params.Revision();
  params.ApplyDiffusionStrength(0.4f);
  EXPECT_EQ(params.Revision(), revision);
}

TEST(DrtDiffusionFilterModel, EditorWriteClampsStrengthAndRejectsNonFiniteValue) {
  auto  document = CreateDefaultPipelineDocument();
  auto& params   = document.Drt()->Params();
  params.ApplyDiffusionStrength(3.0f);
  EXPECT_EQ(params.DiffusionStrength(), kDiffusionStrengthMax);
  params.ApplyDiffusionStrength(-1.0f);
  EXPECT_EQ(params.DiffusionStrength(), kDiffusionStrengthMin);
  params.ApplyDiffusionStrength(0.25f);
  EXPECT_THROW(params.ApplyDiffusionStrength(std::numeric_limits<float>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_EQ(params.DiffusionStrength(), 0.25f);
}

TEST(DrtDiffusionFilterModel, LoadRejectsOutOfRangeStoredStrengthAndNamesTheKey) {
  auto json                  = CreateDefaultPipelineDocument().Drt()->Params().ToJson();
  json["diffusion"]          = {{"strength", 1.5}};
  DrtParamsModel params;
  try {
    params.LoadJson(json);
    FAIL() << "out-of-range diffusion.strength was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string{error.what()}.find("diffusion.strength"), std::string::npos);
  }
}

TEST(DrtDiffusionFilterModel, LoadWithoutDiffusionObjectReadsZeroStrength) {
  DrtParamsModel params;
  params.ApplyDiffusionStrength(0.7f);
  params.LoadJson(CreateDefaultPipelineDocument().Drt()->Params().ToJson());
  EXPECT_EQ(params.DiffusionStrength(), 0.0f);
}

TEST(DrtDiffusionFilterModel, ShapeScalesOnlyScatterFractionWithStrength) {
  const auto off  = ResolveDiffusionFilterShape(0.0f);
  const auto half = ResolveDiffusionFilterShape(0.5f);
  const auto full = ResolveDiffusionFilterShape(1.0f);
  EXPECT_EQ(off.scatter_fraction, 0.0f);
  EXPECT_FLOAT_EQ(half.scatter_fraction, 0.5f * kDiffusionMaxScatterFraction);
  EXPECT_FLOAT_EQ(full.scatter_fraction, kDiffusionMaxScatterFraction);
  EXPECT_EQ(half.glow_radius, full.glow_radius);
  EXPECT_EQ(half.black_mist, full.black_mist);
  EXPECT_EQ(half.highlight_glow, full.highlight_glow);
  EXPECT_FALSE(IsDiffusionFilterActive(0.0f));
  EXPECT_TRUE(IsDiffusionFilterActive(0.01f));
}

}  // namespace
}  // namespace alcedo
