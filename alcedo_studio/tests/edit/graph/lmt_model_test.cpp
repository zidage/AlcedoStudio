//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/lmt_model.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

auto PrimaryLmt(PipelineDocument& document) -> LmtModel& {
  auto* lmt =
      dynamic_cast<LmtModel*>(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
  if (lmt == nullptr) {
    throw std::runtime_error("lmt_model_test: default Color Grade has no LMT adjustment");
  }
  return *lmt;
}

TEST(LutReferenceModel, LegacyLutPathLoadsWithFullStrength) {
  const nlohmann::json legacy = {{"cube_path", "C:/looks/teal.cube"}};
  LmtModel             model;
  model.LoadJson(legacy);

  const auto reference = model.Reference();
  ASSERT_TRUE(std::holds_alternative<FileLutReference>(reference));
  EXPECT_EQ(std::get<FileLutReference>(reference).path, "C:/looks/teal.cube");
  EXPECT_FLOAT_EQ(model.Strength(), 1.0f);
  EXPECT_TRUE(model.DisplayName().empty());
  // Writing back must keep the stored bytes, so pipeline root IDs of old projects still verify.
  EXPECT_EQ(model.ToJson().dump(), legacy.dump());

  LmtModel empty;
  empty.LoadJson(nlohmann::json{{"cube_path", ""}});
  EXPECT_TRUE(empty.IsDefault());
  EXPECT_EQ(empty.ToJson().dump(), R"({"cube_path":""})");
}

TEST(LutReferenceModel, DefaultDocumentLmtSerializesInLegacyForm) {
  auto document = CreateDefaultPipelineDocument();
  EXPECT_EQ(PrimaryLmt(document).ToJson().dump(), R"({"cube_path":""})");
}

TEST(LutReferenceModel, OfficialAndLibraryReferencesRoundTripThroughDocumentJson) {
  auto document = CreateDefaultPipelineDocument();
  PrimaryLmt(document).SetReference(OfficialLutReference{"spectral_film_lut", "kodak-5207"},
                                    "Vision3 250D");
  PrimaryLmt(document).SetStrength(0.35f);
  auto reopened = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(PrimaryLmt(reopened).Reference(),
            (LutReference{OfficialLutReference{"spectral_film_lut", "kodak-5207"}}));
  EXPECT_EQ(PrimaryLmt(reopened).DisplayName(), "Vision3 250D");
  EXPECT_FLOAT_EQ(PrimaryLmt(reopened).Strength(), 0.35f);
  EXPECT_EQ(reopened.ToJson().dump(), document.ToJson().dump());

  PrimaryLmt(document).SetReference(LibraryLutReference{"user/films/中文 look.cube"}, "Look");
  auto library = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(PrimaryLmt(library).Reference(),
            LutReference{LibraryLutReference{"user/films/中文 look.cube"}});
  EXPECT_FLOAT_EQ(PrimaryLmt(library).Strength(), 0.35f);
  EXPECT_TRUE(PrimaryLmt(library).CubePath().empty());
}

TEST(LutReferenceModel, SelectionKeepsStrengthAndStrengthKeepsSelection) {
  LmtModel model;
  model.SetCubePath("D:/luts/a.cube");
  model.SetStrength(0.4f);
  const auto strength_revision = model.FieldsRevision(DirtyFieldMask{LmtDirty::Strength});

  model.SetReference(OfficialLutReference{"spektrafilm_lut", "portra-400"}, "Portra 400");
  EXPECT_FLOAT_EQ(model.Strength(), 0.4f);
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{LmtDirty::Strength}), strength_revision);

  const auto reference_revision = model.FieldsRevision(DirtyFieldMask{LmtDirty::Reference});
  model.SetStrength(0.9f);
  EXPECT_EQ(model.Reference(),
            (LutReference{OfficialLutReference{"spektrafilm_lut", "portra-400"}}));
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{LmtDirty::Reference}), reference_revision);

  // Clearing the association drops the name and keeps the strength.
  model.SetReference(std::monostate{}, "ignored");
  EXPECT_TRUE(model.DisplayName().empty());
  EXPECT_FLOAT_EQ(model.Strength(), 0.9f);

  // An equivalent update is not a change.
  const auto revision = model.Revision();
  model.SetStrength(0.9f);
  EXPECT_EQ(model.Revision(), revision);
}

TEST(LutReferenceModel, InvalidLutUpdateIsRejectedWithoutMutation) {
  LmtModel model;
  model.SetReference(LibraryLutReference{"user/a.cube"}, "A");
  model.SetStrength(0.5f);
  const auto revision = model.Revision();
  const auto json     = model.ToJson().dump();

  for (const float strength : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
    EXPECT_THROW((model.SetStrength(strength)), std::invalid_argument) << strength;
  }
  LmtUpdate mixed;
  mixed.reference = OfficialLutReference{"spectral_film_lut", "kodak-5207"};
  mixed.strength  = 2.0f;
  EXPECT_THROW((model.ApplyUpdate(mixed)), std::invalid_argument);
  EXPECT_THROW((model.SetReference(LibraryLutReference{"../outside.cube"})), std::invalid_argument);
  EXPECT_THROW((model.SetReference(LibraryLutReference{"/abs.cube"})), std::invalid_argument);
  EXPECT_THROW((model.SetReference(OfficialLutReference{"", "id"})), std::invalid_argument);
  EXPECT_THROW((model.LoadJson({{"reference", {{"kind", "catalog"}, {"path", "a.cube"}}}})),
               std::invalid_argument);
  EXPECT_THROW((model.LoadJson({{"cube_path", "C:/a.cube"},
                                {"reference", {{"kind", "library"}, {"path", "a.cube"}}}})),
               std::invalid_argument);
  EXPECT_THROW((model.LoadJson({{"cube_path", ""}, {"strength", "full"}})), std::invalid_argument);
  EXPECT_THROW((model.LoadJson({{"cube_path", ""}, {"unknown", 1}})), std::invalid_argument);

  EXPECT_EQ(model.Revision(), revision);
  EXPECT_EQ(model.ToJson().dump(), json);
}

}  // namespace
}  // namespace alcedo
