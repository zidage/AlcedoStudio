//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

auto FilmPrintUpdate() -> DrtParameterUpdate {
  DrtParameterUpdate update;
  update.method              = DrtMethod::FilmPrint;
  update.film_negative       = "kodak_portra_400";
  update.film_print_material = "fuji_ca_dpII";
  update.film_scene_light    = "tungsten";
  update.film_light_spread   = 2.5f;
  update.film_halation       = 4.0f;
  update.film_enlarger_flare = 0.05f;
  return update;
}

void ExpectDefaultFilmPrint(const FilmPrintParams& film) {
  EXPECT_EQ(film.negative, "kodak_5207");
  EXPECT_EQ(film.print_material, "kodak_endura_premier");
  // The lists of the editor put Lab Scan, Cine Look, and the most common negative first (issue
  // #661). The defaults stay.
  EXPECT_EQ(kFilmPrintMaterialIds[0], kFilmLabScanId);
  EXPECT_EQ(kFilmPrintMaterialIds[1], "cine_look");
  EXPECT_EQ(kFilmPrintMaterialNames[1], "Cine Look (2383)");
  EXPECT_EQ(kFilmNegativeIds.front(), "kodak_5207");
  EXPECT_TRUE(IsFilmIdentifier(kFilmPrintMaterialIds, "kodak_2383"));
  EXPECT_EQ(film.scene_light, "reference");
  EXPECT_EQ(film.light_spread, 1.0f);
  EXPECT_EQ(film.halation, 1.0f);
  EXPECT_EQ(film.enlarger_flare, 0.01f);
}

TEST(DrtFilmPrintModelTest, DefaultDocumentKeepsOpenDrtAndTheDefaultFieldsOfThePlan) {
  const auto document = CreateDefaultPipelineDocument();
  EXPECT_EQ(document.Drt()->Params().Method(), DrtMethod::OpenDrt);
  ExpectDefaultFilmPrint(document.Drt()->Params().FilmPrint());
}

TEST(DrtFilmPrintModelTest, FieldsRoundTripThroughDocumentJson) {
  auto document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyUpdate(FilmPrintUpdate());

  const auto json = document.Drt()->Params().ToJson();
  EXPECT_EQ(json.at("method"), "film_print");
  EXPECT_EQ(json.at("film_print"), (nlohmann::json{{"negative", "kodak_portra_400"},
                                                   {"print_material", "fuji_ca_dpII"},
                                                   {"scene_light", "tungsten"},
                                                   {"light_spread", 2.5f},
                                                   {"halation", 4.0f},
                                                   {"enlarger_flare", 0.05f}}));

  const auto restored = PipelineDocument::FromJson(document.ToJson());
  const auto film     = restored.Drt()->Params().FilmPrint();
  EXPECT_EQ(restored.Drt()->Params().Method(), DrtMethod::FilmPrint);
  EXPECT_EQ(film.negative, "kodak_portra_400");
  EXPECT_EQ(film.print_material, "fuji_ca_dpII");
  EXPECT_EQ(film.scene_light, "tungsten");
  EXPECT_EQ(film.light_spread, 2.5f);
  EXPECT_EQ(film.halation, 4.0f);
  EXPECT_EQ(film.enlarger_flare, 0.05f);
  EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), json.dump());
  EXPECT_FALSE(restored.Drt()->Params().IsDefault());
}

TEST(DrtFilmPrintModelTest, ReversalFilmIsAValueOfTheNegativeKeyAndKeepsThePrintMaterial) {
  // The lab list order (U86), and no identifier in both lists.
  EXPECT_EQ(kFilmReversalIds.front(), "fuji_velvia_50");
  EXPECT_EQ(kFilmReversalIds.back(), "kodak_ektachrome_e100");
  for (const auto id : kFilmReversalIds) {
    EXPECT_TRUE(IsFilmReversalIdentifier(id)) << id;
    EXPECT_TRUE(IsFilmStockIdentifier(id)) << id;
    EXPECT_FALSE(IsFilmIdentifier(kFilmNegativeIds, id)) << id;
  }
  EXPECT_FALSE(IsFilmReversalIdentifier("kodak_5207"));
  EXPECT_TRUE(IsFilmStockIdentifier("kodak_5207"));

  for (const auto id : kFilmReversalIds) {
    SCOPED_TRACE(std::string{id});
    auto document        = CreateDefaultPipelineDocument();
    auto update          = FilmPrintUpdate();
    update.film_negative = std::string{id};
    document.Drt()->Params().ApplyUpdate(update);
    const auto json = document.Drt()->Params().ToJson();
    // The same key holds the reversal film. There is no new key, and the print material stays.
    EXPECT_EQ(json.at("film_print").at("negative"), std::string{id});
    EXPECT_EQ(json.at("film_print").at("print_material"), "fuji_ca_dpII");
    EXPECT_EQ(json.at("film_print").size(), 6u);

    const auto restored = PipelineDocument::FromJson(document.ToJson());
    EXPECT_EQ(restored.Drt()->Params().FilmPrint().negative, id);
    EXPECT_EQ(restored.Drt()->Params().FilmPrint().print_material, "fuji_ca_dpII");
    EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), json.dump());
  }
}

TEST(DrtFilmPrintModelTest, LabScanIsAValueOfThePrintMaterialKeyAndKeepsTheFlare) {
  // Lab Scan is first in the list, before Cine Look, and it is no film.
  EXPECT_EQ(kFilmPrintMaterialIds.front(), kFilmLabScanId);
  EXPECT_EQ(kFilmPrintMaterialNames.front(), "Lab Scan");
  EXPECT_EQ(kFilmPrintMaterialIds[1], "cine_look");
  EXPECT_FALSE(IsFilmStockIdentifier(kFilmLabScanId));
  EXPECT_TRUE(IsFilmLabScan("kodak_5207", kFilmLabScanId));
  EXPECT_FALSE(IsFilmLabScan("kodak_5207", "kodak_2383"));
  // A colour reversal film reads no print material.
  EXPECT_FALSE(IsFilmLabScan("fuji_velvia_50", kFilmLabScanId));

  auto document               = CreateDefaultPipelineDocument();
  auto update                 = FilmPrintUpdate();
  update.film_print_material  = std::string{kFilmLabScanId};
  document.Drt()->Params().ApplyUpdate(update);
  const auto json = document.Drt()->Params().ToJson();
  // The same key holds Lab Scan. There is no new key, and the stored flare stays.
  EXPECT_EQ(json.at("film_print").at("print_material"), "lab_scan");
  EXPECT_EQ(json.at("film_print").size(), 6u);
  EXPECT_FLOAT_EQ(json.at("film_print").at("enlarger_flare").get<float>(), 0.05f);
  const auto restored = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(restored.Drt()->Params().FilmPrint().print_material, kFilmLabScanId);
  EXPECT_FLOAT_EQ(restored.Drt()->Params().FilmPrint().enlarger_flare, 0.05f);
  EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), json.dump());

  // A reversal film with a stored Lab Scan keeps it, as it keeps any print material.
  DrtParameterUpdate slide;
  slide.film_negative = "fuji_velvia_50";
  document.Drt()->Params().ApplyUpdate(slide);
  EXPECT_EQ(document.Drt()->Params().FilmPrint().print_material, kFilmLabScanId);
}

TEST(DrtFilmPrintModelTest, UpperRangeValuesRoundTrip) {
  DrtParamsModel     params;
  DrtParameterUpdate update;
  update.method              = DrtMethod::FilmPrint;
  update.film_light_spread   = kFilmLightSpreadMax;
  update.film_halation       = kFilmHalationMax;
  update.film_enlarger_flare = kFilmEnlargerFlareMax;
  params.ApplyUpdate(update);

  DrtParamsModel restored;
  restored.LoadJson(nlohmann::json::parse(params.ToJson().dump()));
  EXPECT_EQ(restored.FilmPrint().light_spread, kFilmLightSpreadMax);
  EXPECT_EQ(restored.FilmPrint().halation, kFilmHalationMax);
  EXPECT_EQ(restored.FilmPrint().enlarger_flare, kFilmEnlargerFlareMax);
}

TEST(DrtFilmPrintModelTest, OtherMethodsOmitTheObjectAndKeepTheirBytes) {
  for (const auto method : {DrtMethod::OpenDrt, DrtMethod::Aces20}) {
    DrtParamsModel     reference;
    DrtParameterUpdate method_update;
    method_update.method = method;
    reference.ApplyUpdate(method_update);

    DrtParamsModel params;
    auto           update = FilmPrintUpdate();
    update.method         = method;
    params.ApplyUpdate(update);

    EXPECT_FALSE(params.ToJson().contains("film_print"));
    EXPECT_FALSE(params.OutputTransformJson().contains("film_print"));
    EXPECT_EQ(params.ToJson().dump(), reference.ToJson().dump());
  }
}

TEST(DrtFilmPrintModelTest, OutputTransformJsonHoldsTheObjectAndExcludesDiffusion) {
  DrtParamsModel params;
  params.ApplyUpdate(FilmPrintUpdate());
  params.ApplyDiffusionStrength(0.5f);
  auto without = params.ToJson();
  without.erase("diffusion");
  EXPECT_EQ(params.OutputTransformJson(), without);
  EXPECT_TRUE(params.OutputTransformJson().contains("film_print"));
}

TEST(DrtFilmPrintModelTest, FieldsPersistInMemoryWhenTheUserLeavesTheMethod) {
  DrtParamsModel params;
  params.ApplyUpdate(FilmPrintUpdate());
  DrtParameterUpdate other;
  other.method = DrtMethod::OpenDrt;
  params.ApplyUpdate(other);
  EXPECT_EQ(params.FilmPrint().negative, "kodak_portra_400");
  other.method = DrtMethod::FilmPrint;
  params.ApplyUpdate(other);
  EXPECT_EQ(params.ToJson().at("film_print").at("negative"), "kodak_portra_400");
}

TEST(DrtFilmPrintModelTest, LoadWithoutTheObjectReadsTheDefaultFields) {
  DrtParamsModel params;
  params.ApplyUpdate(FilmPrintUpdate());
  params.LoadJson(CreateDefaultPipelineDocument().Drt()->Params().ToJson());
  EXPECT_EQ(params.Method(), DrtMethod::OpenDrt);
  ExpectDefaultFilmPrint(params.FilmPrint());
}

TEST(DrtFilmPrintModelTest, LoadWithMissingKeysReadsTheirDefaults) {
  auto json          = CreateDefaultPipelineDocument().Drt()->Params().ToJson();
  json["method"]     = "film_print";
  json["film_print"] = {{"halation", 3.0}};
  DrtParamsModel params;
  params.LoadJson(json);
  const auto film = params.FilmPrint();
  EXPECT_EQ(film.halation, 3.0f);
  EXPECT_EQ(film.negative, "kodak_5207");
  EXPECT_EQ(film.enlarger_flare, 0.01f);
}

struct RejectedJson {
  const char*    key;
  nlohmann::json value;
};

TEST(DrtFilmPrintModelTest, LoadRejectsInvalidValuesNamesTheKeyAndChangesNoField) {
  const RejectedJson cases[] = {
      {"light_spread", 3.5},
      {"light_spread", -0.1},
      {"light_spread", std::numeric_limits<double>::quiet_NaN()},
      {"halation", 8.5},
      {"halation", std::numeric_limits<double>::infinity()},
      {"halation", "strong"},
      {"enlarger_flare", 0.11},
      {"enlarger_flare", -std::numeric_limits<double>::infinity()},
      {"negative", "kodak_9999"},
      {"negative", 5207},
      {"print_material", "kodak_9999"},
      {"scene_light", "moonlight"},
  };
  for (const auto& rejected : cases) {
    SCOPED_TRACE(std::string{rejected.key} + " = " + rejected.value.dump());
    DrtParamsModel params;
    params.ApplyUpdate(FilmPrintUpdate());
    params.ApplyDiffusionStrength(0.25f);
    const auto before                = params.ToJson().dump();
    const auto revision              = params.Revision();

    auto       json                  = CreateDefaultPipelineDocument().Drt()->Params().ToJson();
    json["film_print"][rejected.key] = rejected.value;
    try {
      params.LoadJson(json);
      FAIL() << "the invalid value was accepted";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string{error.what()}.find(std::string{"film_print."} + rejected.key),
                std::string::npos)
          << error.what();
    }
    EXPECT_EQ(params.ToJson().dump(), before);
    EXPECT_EQ(params.Revision(), revision);
  }
}

TEST(DrtFilmPrintModelTest, LoadRejectsAnObjectOfTheWrongType) {
  auto json          = CreateDefaultPipelineDocument().Drt()->Params().ToJson();
  json["film_print"] = 1;
  DrtParamsModel params;
  EXPECT_THROW(params.LoadJson(json), std::runtime_error);
}

TEST(DrtFilmPrintModelTest, UpdateRejectsInvalidValuesAndChangesNoField) {
  const auto nan     = std::numeric_limits<float>::quiet_NaN();
  const auto inf     = std::numeric_limits<float>::infinity();
  auto       invalid = [] {
    // The valid fields come first, so a partial write would show.
    DrtParameterUpdate update;
    update.method         = DrtMethod::Aces20;
    update.peak_luminance = 400.0f;
    update.film_negative  = "kodak_portra_400";
    return update;
  };
  std::vector<DrtParameterUpdate> cases;
  auto                            add = [&](auto&& change) {
    auto update = invalid();
    change(update);
    cases.push_back(std::move(update));
  };
  add([](DrtParameterUpdate& update) { update.film_negative = "kodak_9999"; });
  add([](DrtParameterUpdate& update) { update.film_print_material = "kodak_9999"; });
  add([](DrtParameterUpdate& update) { update.film_scene_light = "moonlight"; });
  add([&](DrtParameterUpdate& update) { update.film_light_spread = nan; });
  add([](DrtParameterUpdate& update) { update.film_light_spread = 3.5f; });
  add([](DrtParameterUpdate& update) { update.film_light_spread = -0.5f; });
  add([&](DrtParameterUpdate& update) { update.film_halation = inf; });
  add([](DrtParameterUpdate& update) { update.film_halation = 8.5f; });
  add([](DrtParameterUpdate& update) { update.film_enlarger_flare = 0.11f; });
  add([](DrtParameterUpdate& update) { update.film_enlarger_flare = -0.01f; });

  for (auto& update : cases) {
    DrtParamsModel params;
    const auto     before   = params.ToJson().dump();
    const auto     film     = params.FilmPrint();
    const auto     revision = params.Revision();
    EXPECT_THROW(params.ApplyUpdate(update), std::invalid_argument);
    EXPECT_EQ(params.ToJson().dump(), before);
    EXPECT_EQ(params.FilmPrint().negative, film.negative);
    EXPECT_EQ(params.Revision(), revision);
  }
}

TEST(DrtFilmPrintModelTest, FieldChangeMarksOnlyItsFilmPrintField) {
  DrtParamsModel params;
  const auto     other_fields = DirtyFieldMask{static_cast<std::uint64_t>(DrtDirty::Method) |
                                           static_cast<std::uint64_t>(DrtDirty::Encoding) |
                                           static_cast<std::uint64_t>(DrtDirty::OpenDrt) |
                                           static_cast<std::uint64_t>(DrtDirty::Diffusion)};
  const auto     other_before = params.FieldsRevision(other_fields);
  const std::array<DrtDirty, 3> film_fields{DrtDirty::FilmExposure, DrtDirty::FilmHalation,
                                            DrtDirty::FilmFlare};

  // Each change marks its own Film Print field and no other field.
  const auto                    expect_marks = [&](DrtDirty marked, auto&& change) {
    std::array<ParameterRevision, 3> before{};
    for (std::size_t index = 0; index < film_fields.size(); ++index) {
      before[index] = params.FieldsRevision(DirtyFieldMask{film_fields[index]});
    }
    DrtParameterUpdate update;
    change(update);
    params.ApplyUpdate(update);
    for (std::size_t index = 0; index < film_fields.size(); ++index) {
      const auto after = params.FieldsRevision(DirtyFieldMask{film_fields[index]});
      if (film_fields[index] == marked) {
        EXPECT_GT(after, before[index]);
      } else {
        EXPECT_EQ(after, before[index]);
      }
    }
    EXPECT_EQ(params.FieldsRevision(other_fields), other_before);
  };
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_light_spread = 2.0f; });
  expect_marks(DrtDirty::FilmHalation,
               [](DrtParameterUpdate& update) { update.film_halation = 2.0f; });
  expect_marks(DrtDirty::FilmFlare,
               [](DrtParameterUpdate& update) { update.film_enlarger_flare = 0.02f; });
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_negative = "fuji_c200"; });
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_print_material = "kodak_2393"; });
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_scene_light = "led"; });
  // A reversal film is a new film, so its exposure and all its results change.
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_negative = "fuji_velvia_50"; });
  // The Loupe flare of a reversal film is the stored Enlarger flare.
  expect_marks(DrtDirty::FilmFlare,
               [](DrtParameterUpdate& update) { update.film_enlarger_flare = 0.04f; });
  // Lab Scan has no entry gamut compression, so the exposure and all its results change.
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_print_material = "lab_scan"; });
  expect_marks(DrtDirty::FilmExposure, [](DrtParameterUpdate& update) {
    update.film_scene_light = std::string{kFilmAsShotLightId};
  });

  // The LAD aim changes only the code of the scan: its own field, and no field of the canvases.
  const auto scan_before = params.FieldsRevision(DirtyFieldMask{DrtDirty::FilmScan});
  std::array<ParameterRevision, 3> canvas_before{};
  for (std::size_t index = 0; index < film_fields.size(); ++index) {
    canvas_before[index] = params.FieldsRevision(DirtyFieldMask{film_fields[index]});
  }
  DrtParameterUpdate lad;
  lad.film_lad_aim = true;
  params.ApplyUpdate(lad);
  EXPECT_GT(params.FieldsRevision(DirtyFieldMask{DrtDirty::FilmScan}), scan_before);
  for (std::size_t index = 0; index < film_fields.size(); ++index) {
    EXPECT_EQ(params.FieldsRevision(DirtyFieldMask{film_fields[index]}), canvas_before[index]);
  }
  EXPECT_EQ(params.FieldsRevision(other_fields), other_before);
}

TEST(DrtFilmPrintModelTest, AsShotIsASceneLightAndTheLadAimIsWrittenOnlyWhenOn) {
  // As Shot is second in the list, after the reference light (issue 661).
  EXPECT_EQ(kFilmSceneLightIds[0], "reference");
  EXPECT_EQ(kFilmSceneLightIds[1], kFilmAsShotLightId);
  EXPECT_TRUE(IsFilmAsShotLight(kFilmAsShotLightId));
  EXPECT_FALSE(IsFilmAsShotLight("daylight"));
  EXPECT_FALSE(FilmPrintParams{}.lad_aim);

  // A document with the LAD aim off has the bytes and the root id of a document of before.
  auto document              = CreateDefaultPipelineDocument();
  auto update                = FilmPrintUpdate();
  update.film_print_material = std::string{kFilmLabScanId};
  update.film_scene_light    = std::string{kFilmAsShotLightId};
  document.Drt()->Params().ApplyUpdate(update);
  const auto off = document.Drt()->Params().ToJson();
  EXPECT_FALSE(off.at("film_print").contains("lad_aim"));
  EXPECT_EQ(off.at("film_print").size(), 6u);
  EXPECT_EQ(off.at("film_print").at("scene_light"), "as_shot");
  auto explicit_off                     = off;
  explicit_off["film_print"]["lad_aim"] = false;
  DrtParamsModel read_off;
  read_off.LoadJson(explicit_off);
  EXPECT_EQ(read_off.ToJson().dump(), off.dump());

  DrtParameterUpdate lad;
  lad.film_lad_aim = true;
  document.Drt()->Params().ApplyUpdate(lad);
  const auto on = document.Drt()->Params().ToJson();
  EXPECT_EQ(on.at("film_print").at("lad_aim"), true);
  const auto restored = PipelineDocument::FromJson(document.ToJson());
  EXPECT_TRUE(restored.Drt()->Params().FilmPrint().lad_aim);
  EXPECT_EQ(restored.Drt()->Params().FilmPrint().scene_light, kFilmAsShotLightId);
  EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), on.dump());

  // The LAD aim must be a boolean.
  auto wrong                     = on;
  wrong["film_print"]["lad_aim"] = 1;
  DrtParamsModel params;
  const auto     before = params.ToJson().dump();
  EXPECT_THROW(params.LoadJson(wrong), std::runtime_error);
  EXPECT_EQ(params.ToJson().dump(), before);
}

TEST(DrtFilmPrintModelTest, EqualValuesKeepTheRevision) {
  DrtParamsModel params;
  params.ApplyUpdate(FilmPrintUpdate());
  const auto revision = params.Revision();
  params.ApplyUpdate(FilmPrintUpdate());
  EXPECT_EQ(params.Revision(), revision);
}

TEST(DrtFilmPrintModelTest, HalationAdjustmentValueStaysInTheDocument) {
  auto  document = CreateDefaultPipelineDocument();
  auto* halation = document.Drt()->FindAdjustmentByType(type_ids::Halation());
  ASSERT_NE(halation, nullptr);
  const auto before = halation->ToJson().dump();
  document.Drt()->Params().ApplyUpdate(FilmPrintUpdate());

  const auto  restored = PipelineDocument::FromJson(document.ToJson());
  const auto* stored   = restored.Drt()->FindAdjustmentByType(type_ids::Halation());
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->ToJson().dump(), before);
}

}  // namespace
}  // namespace alcedo
