//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise parameter of the Develop node: JSON, validation, and dirty group
// (ai_denoise_integration_plan.md, DN5).

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>

#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/models/dirty_field_mask.hpp"

namespace alcedo {
namespace {

const std::string kVersionA(64, 'a');
const std::string kVersionB = std::string(63, '0') + "f";

auto DenoiseFields() -> DirtyFieldMask { return DirtyFieldMask{DevelopDirty::Denoise}; }

auto OtherFields() -> DirtyFieldMask {
  return DirtyFieldMask{DevelopDirty::Demosaic} | DirtyFieldMask{DevelopDirty::Highlights} |
         DirtyFieldMask{DevelopDirty::WhiteBalance} | DirtyFieldMask{DevelopDirty::Lens} |
         DirtyFieldMask{DevelopDirty::Input} | DirtyFieldMask{DevelopDirty::ProfileTone};
}

TEST(DevelopAiDenoiseTest, AiDenoiseJsonRoundTripsTheModelVersion) {
  DevelopParamsModel model;
  const auto         off_json = model.ToJson();
  EXPECT_FALSE(off_json.contains("ai_denoise"));
  EXPECT_FALSE(model.AiDenoise().has_value());
  EXPECT_TRUE(model.IsDefault());

  model.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersionA});
  const auto on_json = model.ToJson();
  ASSERT_TRUE(on_json.contains("ai_denoise"));
  EXPECT_EQ(on_json["ai_denoise"], (nlohmann::json{{"model_version", kVersionA}}));
  EXPECT_FALSE(model.IsDefault());

  DevelopParamsModel loaded;
  loaded.LoadJson(on_json);
  ASSERT_TRUE(loaded.AiDenoise().has_value());
  EXPECT_EQ(loaded.AiDenoise()->model_version, kVersionA);
  EXPECT_EQ(loaded.ToJson().dump(), on_json.dump());

  // Off again: the key is gone and the JSON is the JSON before the switch.
  model.ApplyAiDenoiseUpdate(std::nullopt);
  EXPECT_EQ(model.ToJson().dump(), off_json.dump());
  loaded.LoadJson(off_json);
  EXPECT_FALSE(loaded.AiDenoise().has_value());
}

TEST(DevelopAiDenoiseTest, AiDenoiseJsonRejectsAMalformedValueAndKeepsTheModel) {
  DevelopParamsModel model;
  model.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersionA});
  auto edited                   = model.Params();
  edited.highlights_reconstruct = false;
  model.ReplaceParams(edited);
  const auto before     = model.ToJson().dump();

  const auto with_value = [&](nlohmann::json value) {
    auto json                      = model.ToJson();
    json["ai_denoise"]             = std::move(value);
    // Another field changes too, so a partial load would be visible.
    json["highlights_reconstruct"] = true;
    return json;
  };
  const nlohmann::json malformed[] = {
      with_value({{"model_version", std::string(63, 'a')}}),
      with_value({{"model_version", std::string(65, 'a')}}),
      with_value({{"model_version", std::string(64, 'A')}}),
      with_value({{"model_version", std::string(63, 'a') + "g"}}),
      with_value({{"model_version", 7}}),
      with_value(kVersionA),
      with_value(nlohmann::json::object()),
  };
  for (const auto& json : malformed) {
    SCOPED_TRACE(json["ai_denoise"].dump());
    EXPECT_THROW(model.LoadJson(json), std::runtime_error);
    EXPECT_EQ(model.ToJson().dump(), before);
  }

  // A raster document has no AI Denoise.
  auto raster_json     = model.ToJson();
  raster_json["input"] = nlohmann::json::object();
  EXPECT_THROW(model.LoadJson(raster_json), std::runtime_error);
  EXPECT_EQ(model.ToJson().dump(), before);
}

TEST(DevelopAiDenoiseTest, AiDenoiseChangeMarksTheSensorGroupDirty) {
  DevelopParamsModel model;
  auto               denoise       = model.FieldsRevision(DenoiseFields());
  const auto         other         = model.FieldsRevision(OtherFields());

  const auto         expect_change = [&](std::optional<DevelopAiDenoise> value, bool changes) {
    model.ApplyAiDenoiseUpdate(std::move(value));
    const auto after = model.FieldsRevision(DenoiseFields());
    EXPECT_EQ(after != denoise, changes);
    EXPECT_EQ(model.FieldsRevision(OtherFields()), other);
    denoise = after;
  };
  expect_change(DevelopAiDenoise{kVersionA}, true);
  expect_change(DevelopAiDenoise{kVersionA}, false);
  expect_change(DevelopAiDenoise{kVersionB}, true);
  expect_change(std::nullopt, true);
  expect_change(std::nullopt, false);

  // ReplaceParams marks the same group.
  auto payload       = model.Params();
  payload.ai_denoise = DevelopAiDenoise{kVersionB};
  model.ReplaceParams(payload);
  EXPECT_NE(model.FieldsRevision(DenoiseFields()), denoise);
  EXPECT_EQ(model.FieldsRevision(OtherFields()), other);

  // A version that is not a SHA-256 is rejected; the payload is unchanged.
  EXPECT_THROW(model.ApplyAiDenoiseUpdate(DevelopAiDenoise{"abc"}), std::invalid_argument);
  EXPECT_EQ(model.AiDenoise()->model_version, kVersionB);
}

TEST(DevelopAiDenoiseTest, AiDenoiseIsRejectedForARasterDocument) {
  DevelopParamsModel model;
  auto               payload = model.Params();
  payload.input              = DevelopRasterInput{};
  model.ReplaceParams(payload);
  const auto revision = model.Revision();
  EXPECT_THROW(model.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersionA}), std::logic_error);
  EXPECT_FALSE(model.AiDenoise().has_value());
  EXPECT_EQ(model.Revision(), revision);
}

}  // namespace
}  // namespace alcedo
