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

TEST(LutReferenceModel, MissingEncodingKeysReadAsAcesccAndDefaultsAreNotWritten) {
  const nlohmann::json stored = {{"cube_path", ""},
                                 {"name", "Look"},
                                 {"reference", {{"kind", "library"}, {"path", "user/look.cube"}}},
                                 {"strength", 0.5f}};
  LmtModel             model;
  model.LoadJson(stored);
  EXPECT_EQ(model.InputEncoding(), "acescc");
  EXPECT_EQ(model.OutputEncoding(), "acescc");
  EXPECT_EQ(model.ToJson().dump(), stored.dump());

  // Explicit default ids read the same and are still not written.
  LmtModel explicit_default;
  auto     with_defaults           = stored;
  with_defaults["input_encoding"]  = "acescc";
  with_defaults["output_encoding"] = "acescc";
  explicit_default.LoadJson(with_defaults);
  EXPECT_EQ(explicit_default.ToJson().dump(), stored.dump());

  const auto update = LmtUpdateFromModelJson(nlohmann::json{{"cube_path", ""}});
  ASSERT_TRUE(update.input_encoding.has_value());
  ASSERT_TRUE(update.output_encoding.has_value());
  EXPECT_EQ(*update.input_encoding, "acescc");
  EXPECT_EQ(*update.output_encoding, "acescc");
}

TEST(LutReferenceModel, NonDefaultEncodingsRoundTripThroughDocumentJson) {
  auto document = CreateDefaultPipelineDocument();
  PrimaryLmt(document).SetReference(LibraryLutReference{"user/slog3_to_709.cube"}, "S-Log3");
  PrimaryLmt(document).SetEncodings("sony_slog3_sgamut3cine", "rec709_bt1886");
  const auto params = PrimaryLmt(document).ToJson();
  EXPECT_EQ(params.at("input_encoding"), "sony_slog3_sgamut3cine");
  EXPECT_EQ(params.at("output_encoding"), "rec709_bt1886");
  EXPECT_FALSE(PrimaryLmt(document).IsDefault());

  auto reopened = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(PrimaryLmt(reopened).InputEncoding(), "sony_slog3_sgamut3cine");
  EXPECT_EQ(PrimaryLmt(reopened).OutputEncoding(), "rec709_bt1886");
  EXPECT_EQ(reopened.ToJson().dump(), document.ToJson().dump());

  // Only the non-default side is written.
  PrimaryLmt(document).SetEncodings("acescc", "rec2100_pq1000");
  const auto output_only = PrimaryLmt(document).ToJson();
  EXPECT_FALSE(output_only.contains("input_encoding"));
  EXPECT_EQ(output_only.at("output_encoding"), "rec2100_pq1000");

  // Encodings without a LUT are still an adjusted LMT.
  LmtModel no_lut;
  no_lut.SetEncodings("arri_logc4_awg4", "acescc");
  EXPECT_FALSE(no_lut.IsDefault());
}

TEST(LutReferenceModel, EncodingChangeHasOwnRevisionAndClearingReferenceKeepsEncodings) {
  LmtModel model;
  model.SetCubePath("D:/luts/a.cube");
  const auto reference_revision = model.FieldsRevision(DirtyFieldMask{LmtDirty::Reference});
  const auto strength_revision  = model.FieldsRevision(DirtyFieldMask{LmtDirty::Strength});
  const auto encoding_revision  = model.FieldsRevision(DirtyFieldMask{LmtDirty::Encoding});

  model.SetEncodings("sony_slog3_sgamut3cine", "rec709_bt1886");
  EXPECT_GT(model.FieldsRevision(DirtyFieldMask{LmtDirty::Encoding}), encoding_revision);
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{LmtDirty::Reference}), reference_revision);
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{LmtDirty::Strength}), strength_revision);
  EXPECT_EQ(model.CubePath(), "D:/luts/a.cube");

  // An equal encoding pair is not a change.
  const auto revision = model.Revision();
  model.SetEncodings("sony_slog3_sgamut3cine", "rec709_bt1886");
  EXPECT_EQ(model.Revision(), revision);

  // The user may pick another LUT of the same kind after clearing (section 6.1).
  model.SetReference(std::monostate{});
  EXPECT_EQ(model.InputEncoding(), "sony_slog3_sgamut3cine");
  EXPECT_EQ(model.OutputEncoding(), "rec709_bt1886");
  model.SetReference(LibraryLutReference{"user/b.cube"}, "B");
  EXPECT_EQ(model.InputEncoding(), "sony_slog3_sgamut3cine");

  // One update carries a selection and both encodings, and both dirty fields share its revision.
  LmtUpdate update;
  update.reference       = LibraryLutReference{"user/c.cube"};
  update.input_encoding  = "acescct";
  update.output_encoding = "acescc";
  model.ApplyUpdate(update);
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{LmtDirty::Reference}), model.Revision());
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{LmtDirty::Encoding}), model.Revision());
  EXPECT_EQ(model.InputEncoding(), "acescct");
  EXPECT_EQ(model.OutputEncoding(), "acescc");
}

TEST(LutReferenceModel, UnknownEncodingIdThrowsAndLeavesModelUnchanged) {
  LmtModel model;
  model.SetReference(LibraryLutReference{"user/a.cube"}, "A");
  model.SetEncodings("arri_logc3_awg3", "rec709_srgb");
  const auto revision = model.Revision();
  const auto json     = model.ToJson().dump();

  EXPECT_THROW(model.SetEncodings("slog3", "acescc"), std::invalid_argument);
  EXPECT_THROW(model.SetEncodings("acescc", "ACEScc"), std::invalid_argument);
  // A valid part does not apply when another part is invalid.
  LmtUpdate mixed;
  mixed.strength        = 0.2f;
  mixed.input_encoding  = "acescct";
  mixed.output_encoding = "linear_rec709";
  EXPECT_THROW(model.ApplyUpdate(mixed), std::invalid_argument);
  EXPECT_THROW((model.LoadJson({{"cube_path", ""}, {"input_encoding", "unknown"}})),
               std::invalid_argument);
  EXPECT_THROW((model.LoadJson({{"cube_path", ""}, {"output_encoding", 3}})),
               std::invalid_argument);

  EXPECT_EQ(model.Revision(), revision);
  EXPECT_EQ(model.ToJson().dump(), json);
  EXPECT_EQ(model.InputEncoding(), "arri_logc3_awg3");
  EXPECT_FLOAT_EQ(model.Strength(), 1.0f);
  EXPECT_FALSE(ValidateLutEncodingId("unknown").empty());
  EXPECT_TRUE(ValidateLutEncodingId("rec2100_hlg1000").empty());
}

}  // namespace
}  // namespace alcedo
