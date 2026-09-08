//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/brush_mask_commands.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace alcedo {
namespace {

auto MakeSample(float local_x, float local_y, float radius = 8.0f, float strength = 1.0f,
                float hardness = 1.0f) -> BrushCanonicalSample {
  return {local_x, local_y, radius, strength, hardness};
}

auto MakePaintStroke(std::string_view id, std::vector<BrushCanonicalSample> samples)
    -> BrushStroke {
  return MakeBrushStroke(StrokeId{std::string{id}}, BrushStrokeMode::Paint, std::move(samples));
}

auto MakeEraseStroke(std::string_view id, std::vector<BrushCanonicalSample> samples)
    -> BrushStroke {
  return MakeBrushStroke(StrokeId{std::string{id}}, BrushStrokeMode::Erase, std::move(samples));
}

void AddEmptyBrush(ColorGradeNodeModel& grade, MaskId id) {
  MaskModel mask;
  mask.id     = std::move(id);
  mask.source = BrushMaskSource{};
  grade.AddMask(std::move(mask), grade.MaskCount());
}

auto PrimaryGradeJson(nlohmann::json& document_json) -> nlohmann::json& {
  for (auto& node : document_json.at("nodes")) {
    if (node.at("id") == "grade.primary") {
      return node;
    }
  }
  throw std::runtime_error("document JSON is missing grade.primary");
}

TEST(BrushParameterizedSource, BrushSourceRoundTripsWithoutRasterFiles) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  AddEmptyBrush(*grade, MaskId{"mask.brush"});
  const auto first = MakePaintStroke(
      "stroke.paint", {MakeSample(12.0f, 8.0f, 6.0f, 1.0f, 1.0f), MakeSample(16.0f, 9.0f, 6.0f)});
  const auto second =
      MakeEraseStroke("stroke.erase", {MakeSample(14.0f, 8.5f, 4.0f, 0.5f, 0.25f)});
  const auto mask_id = MaskId{"mask.brush"};
  grade->AppendBrushStroke(
      {grade->Id(), mask_id, first, grade->MaskContentRevision(mask_id)});
  grade->AppendBrushStroke(
      {grade->Id(), mask_id, second, grade->MaskContentRevision(mask_id)});
  SetBrushTranslationCommand move;
  move.node_id            = grade->Id();
  move.mask_id            = MaskId{"mask.brush"};
  move.before             = {};
  move.after              = {3.5f, -2.0f};
  move.expected_revision  = grade->MaskContentRevision(MaskId{"mask.brush"});
  grade->SetBrushTranslation(move);

  auto json = document.ToJson();
  const auto source = PrimaryGradeJson(json).at("masks").at(0).at("source");
  ASSERT_TRUE(source.is_object());
  EXPECT_EQ(source.at("kind"), "brush");
  EXPECT_EQ(source.at("source_format_version"), kBrushSourceFormatVersion);
  EXPECT_EQ(source.at("raster_algorithm_version"), kBrushRasterAlgorithmVersion);
  EXPECT_FALSE(source.contains("asset_key"));
  EXPECT_FALSE(source.contains("width"));
  ASSERT_EQ(source.at("strokes").size(), 2u);
  EXPECT_EQ(source.at("strokes").at(0).at("id"), "stroke.paint");
  EXPECT_EQ(source.at("strokes").at(1).at("mode"), 1);

  const auto restored = PipelineDocument::FromJson(json);
  const auto* mask    = restored.PrimaryGrade()->FindMask(MaskId{"mask.brush"});
  ASSERT_NE(mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  ASSERT_NE(brush, nullptr);
  EXPECT_FALSE(brush->asset_key.has_value());
  EXPECT_EQ(brush->source_format_version, kBrushSourceFormatVersion);
  EXPECT_EQ(brush->raster_algorithm_version, kBrushRasterAlgorithmVersion);
  EXPECT_EQ(brush->placement_translation, (Vector2{3.5f, -2.0f}));
  ASSERT_EQ(brush->strokes.size(), 2u);
  EXPECT_EQ(brush->strokes[0].id, StrokeId{"stroke.paint"});
  EXPECT_EQ(brush->strokes[1].id, StrokeId{"stroke.erase"});
  EXPECT_EQ(brush->strokes[1].mode, BrushStrokeMode::Erase);
  ASSERT_EQ(BrushStrokeSamples(brush->strokes[0]).size(), 2u);
  EXPECT_EQ(BrushStrokeSamples(brush->strokes[0])[0], MakeSample(12.0f, 8.0f, 6.0f));
  EXPECT_EQ(MaskModelToJson(*mask).at("source").dump(), source.dump());
  EXPECT_EQ(*mask, restored.PrimaryGrade()->MaskAt(0));
}

TEST(BrushParameterizedSource, BrushAppendKeepsExistingStrokeIds) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  AddEmptyBrush(*grade, MaskId{"mask.a"});
  AddEmptyBrush(*grade, MaskId{"mask.b"});
  const auto first = MakePaintStroke("stroke.keep", {MakeSample(1.0f, 2.0f)});
  grade->AppendBrushStroke(
      {grade->Id(), MaskId{"mask.a"}, first, grade->MaskContentRevision(MaskId{"mask.a"})});
  const auto kept_body = grade->BrushStrokes(MaskId{"mask.a"})[0].samples;
  const auto first_revision = grade->MaskContentRevision(MaskId{"mask.a"});
  const auto other_revision = grade->MaskContentRevision(MaskId{"mask.b"});

  const auto second = MakePaintStroke("stroke.next", {MakeSample(4.0f, 5.0f, 10.0f, 0.8f, 0.5f)});
  grade->AppendBrushStroke({grade->Id(), MaskId{"mask.a"}, second, first_revision});

  const auto strokes = grade->BrushStrokes(MaskId{"mask.a"});
  ASSERT_EQ(strokes.size(), 2u);
  EXPECT_EQ(strokes[0].id, StrokeId{"stroke.keep"});
  EXPECT_EQ(strokes[1].id, StrokeId{"stroke.next"});
  EXPECT_EQ(kept_body.get(), strokes[0].samples.get());
  EXPECT_EQ(BrushStrokeSamples(strokes[0])[0], MakeSample(1.0f, 2.0f));
  EXPECT_NE(grade->MaskContentRevision(MaskId{"mask.a"}), first_revision);
  EXPECT_EQ(grade->MaskContentRevision(MaskId{"mask.b"}), other_revision);
  EXPECT_TRUE(grade->BrushStrokes(MaskId{"mask.b"}).empty());
}

TEST(BrushParameterizedSource, InvalidStrokeDoesNotPartiallyMutateSource) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  AddEmptyBrush(*grade, MaskId{"mask.brush"});
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}), 1);
  const auto valid = MakePaintStroke("stroke.1", {MakeSample(0.0f, 0.0f)});
  grade->AppendBrushStroke(
      {grade->Id(), MaskId{"mask.brush"}, valid, grade->MaskContentRevision(MaskId{"mask.brush"})});
  const auto before        = *grade->FindMask(MaskId{"mask.brush"});
  const auto before_revision = grade->MaskContentRevision(MaskId{"mask.brush"});
  const auto before_radial = *grade->FindMask(MaskId{"mask.radial"});
  const auto original_body = std::get<BrushMaskSource>(before.source).strokes[0].samples;

  BrushCanonicalSample nan_sample = MakeSample(1.0f, 1.0f);
  nan_sample.local_x              = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)MakeBrushStroke(StrokeId{"stroke.bad"}, BrushStrokeMode::Paint, {nan_sample}),
               std::runtime_error);

  auto duplicate = MakePaintStroke("stroke.1", {MakeSample(9.0f, 9.0f)});
  EXPECT_THROW(grade->AppendBrushStroke({grade->Id(), MaskId{"mask.brush"}, duplicate,
                                        before_revision}),
               std::runtime_error);

  auto empty_id = MakePaintStroke("stroke.2", {MakeSample(3.0f, 3.0f)});
  empty_id.id   = StrokeId{};
  EXPECT_THROW(
      grade->AppendBrushStroke({grade->Id(), MaskId{"mask.brush"}, empty_id, before_revision}),
      std::runtime_error);

  auto next = MakePaintStroke("stroke.2", {MakeSample(3.0f, 3.0f)});
  EXPECT_THROW(grade->AppendBrushStroke(
                   {NodeId{"grade.other"}, MaskId{"mask.brush"}, next, before_revision}),
               std::runtime_error);
  EXPECT_THROW(
      grade->AppendBrushStroke({grade->Id(), MaskId{"mask.missing"}, next, before_revision}),
      std::runtime_error);
  EXPECT_THROW(
      grade->AppendBrushStroke({grade->Id(), MaskId{"mask.brush"}, next, before_revision - 1}),
      std::runtime_error);
  EXPECT_THROW(grade->AppendBrushStroke({grade->Id(), MaskId{"mask.radial"}, next,
                                        grade->MaskContentRevision(MaskId{"mask.radial"})}),
               std::runtime_error);

  BrushCanonicalSample zero_radius = MakeSample(1.0f, 1.0f);
  zero_radius.radius              = 0.0f;
  EXPECT_THROW(
      (void)MakeBrushStroke(StrokeId{"stroke.zero"}, BrushStrokeMode::Paint, {zero_radius}),
      std::runtime_error);

  EXPECT_EQ(*grade->FindMask(MaskId{"mask.brush"}), before);
  EXPECT_EQ(grade->MaskContentRevision(MaskId{"mask.brush"}), before_revision);
  EXPECT_EQ(std::get<BrushMaskSource>(grade->FindMask(MaskId{"mask.brush"})->source)
                .strokes[0]
                .samples.get(),
            original_body.get());
  EXPECT_EQ(*grade->FindMask(MaskId{"mask.radial"}), before_radial);
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"}).size(), 1u);
}

TEST(BrushParameterizedSource, BrushTranslationDoesNotCopyOrRewriteSamples) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  AddEmptyBrush(*grade, MaskId{"mask.brush"});
  const auto stroke = MakePaintStroke("stroke.1", {MakeSample(10.0f, 20.0f, 5.0f, 0.75f, 0.5f)});
  const auto mask_id = MaskId{"mask.brush"};
  grade->AppendBrushStroke(
      {grade->Id(), mask_id, stroke, grade->MaskContentRevision(mask_id)});
  const auto* body_before =
      grade->BrushStrokes(MaskId{"mask.brush"})[0].samples.get();
  const auto sample_before = BrushStrokeSamples(grade->BrushStrokes(MaskId{"mask.brush"})[0])[0];
  const auto revision      = grade->MaskContentRevision(MaskId{"mask.brush"});

  SetBrushTranslationCommand move;
  move.node_id           = grade->Id();
  move.mask_id           = MaskId{"mask.brush"};
  move.before            = {};
  move.after             = {40.0f, -15.0f};
  move.expected_revision = revision;
  grade->SetBrushTranslation(move);

  const auto strokes = grade->BrushStrokes(MaskId{"mask.brush"});
  ASSERT_EQ(strokes.size(), 1u);
  EXPECT_EQ(strokes[0].samples.get(), body_before);
  EXPECT_EQ(BrushStrokeSamples(strokes[0])[0], sample_before);
  EXPECT_EQ(BrushStrokeSamples(strokes[0])[0].local_x, 10.0f);
  EXPECT_EQ(BrushStrokeSamples(strokes[0])[0].local_y, 20.0f);
  EXPECT_EQ(grade->BrushPlacementTranslation(MaskId{"mask.brush"}), (Vector2{40.0f, -15.0f}));
  EXPECT_NE(grade->MaskContentRevision(MaskId{"mask.brush"}), revision);

  move.before            = {40.0f, -15.0f};
  move.after             = {40.0f, -15.0f};
  move.expected_revision = grade->MaskContentRevision(MaskId{"mask.brush"});
  const auto no_op_revision = move.expected_revision;
  grade->SetBrushTranslation(move);
  EXPECT_EQ(grade->MaskContentRevision(MaskId{"mask.brush"}), no_op_revision);
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[0].samples.get(), body_before);

  move.after = {1.0f, 1.0f};
  move.before = {};
  EXPECT_THROW(grade->SetBrushTranslation(move), std::runtime_error);
  EXPECT_EQ(grade->BrushPlacementTranslation(MaskId{"mask.brush"}), (Vector2{40.0f, -15.0f}));
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[0].samples.get(), body_before);
}

TEST(BrushParameterizedSource, InsertAndRemoveKeepRemainingSampleBodies) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  AddEmptyBrush(*grade, MaskId{"mask.brush"});
  grade->AppendBrushStroke({grade->Id(), MaskId{"mask.brush"},
                            MakePaintStroke("stroke.a", {MakeSample(1.0f, 1.0f)}),
                            grade->MaskContentRevision(MaskId{"mask.brush"})});
  grade->AppendBrushStroke({grade->Id(), MaskId{"mask.brush"},
                            MakePaintStroke("stroke.c", {MakeSample(3.0f, 3.0f)}),
                            grade->MaskContentRevision(MaskId{"mask.brush"})});
  const auto first_body  = grade->BrushStrokes(MaskId{"mask.brush"})[0].samples;
  const auto second_body = grade->BrushStrokes(MaskId{"mask.brush"})[1].samples;

  InsertBrushStrokeCommand insert;
  insert.node_id           = grade->Id();
  insert.mask_id           = MaskId{"mask.brush"};
  insert.index             = 1;
  insert.stroke            = MakePaintStroke("stroke.b", {MakeSample(2.0f, 2.0f)});
  insert.expected_revision = grade->MaskContentRevision(MaskId{"mask.brush"});
  grade->InsertBrushStroke(std::move(insert));
  ASSERT_EQ(grade->BrushStrokes(MaskId{"mask.brush"}).size(), 3u);
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[0].id, StrokeId{"stroke.a"});
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[1].id, StrokeId{"stroke.b"});
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[2].id, StrokeId{"stroke.c"});
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[0].samples.get(), first_body.get());
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[2].samples.get(), second_body.get());

  RemoveBrushStrokeCommand remove;
  remove.node_id           = grade->Id();
  remove.mask_id           = MaskId{"mask.brush"};
  remove.stroke_id         = StrokeId{"stroke.b"};
  remove.expected_revision = grade->MaskContentRevision(MaskId{"mask.brush"});
  grade->RemoveBrushStroke(remove);
  ASSERT_EQ(grade->BrushStrokes(MaskId{"mask.brush"}).size(), 2u);
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[0].samples.get(), first_body.get());
  EXPECT_EQ(grade->BrushStrokes(MaskId{"mask.brush"})[1].samples.get(), second_body.get());
}

TEST(BrushParameterizedSource, ParameterizedBrushJsonRejectsUnsupportedAlgorithmVersion) {
  auto document = CreateDefaultPipelineDocument();
  AddEmptyBrush(*document.PrimaryGrade(), MaskId{"mask.brush"});
  document.PrimaryGrade()->AppendBrushStroke(
      {document.PrimaryGrade()->Id(), MaskId{"mask.brush"},
       MakePaintStroke("stroke.1", {MakeSample(1.0f, 1.0f)}),
       document.PrimaryGrade()->MaskContentRevision(MaskId{"mask.brush"})});
  auto json = document.ToJson();
  PrimaryGradeJson(json).at("masks").at(0).at("source")["raster_algorithm_version"] = 2;
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);

  json = document.ToJson();
  PrimaryGradeJson(json).at("masks").at(0).at("source")["source_format_version"] = 0;
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);

  json = document.ToJson();
  PrimaryGradeJson(json).at("masks").at(0).at("source").erase("placement_translation");
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);

  json = document.ToJson();
  PrimaryGradeJson(json).at("masks").at(0).at("source")["strokes"].push_back(
      nlohmann::json{{"id", "stroke.1"},
                     {"mode", 0},
                     {"samples", nlohmann::json::array({nlohmann::json{{"local_x", 2.0},
                                                                     {"local_y", 2.0},
                                                                     {"radius", 1.0},
                                                                     {"strength", 1.0},
                                                                     {"hardness", 1.0}}})}});
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
