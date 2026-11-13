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
  // The lists of the editor put Negative Scan, Cine Look, and the most common negative first
  // (issue #661). The defaults stay.
  EXPECT_EQ(kFilmPrintMaterialIds[0], kFilmNegativeScanId);
  EXPECT_EQ(kFilmPrintMaterialIds[1], "cine_look");
  EXPECT_EQ(kFilmPrintMaterialNames[1], "Cine Look (2383)");
  EXPECT_EQ(kFilmNegativeIds.front(), "kodak_5207");
  EXPECT_TRUE(IsFilmIdentifier(kFilmPrintMaterialIds, "kodak_2383"));
  // The default scene light is As Shot for every form (issue #672).
  EXPECT_EQ(film.scene_light, kFilmAsShotLightId);
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
                                                   {"enlarger_flare", 0.05f},
                                                   // New settings write the auto colour: it is
                                                   // not kFilmScanAutoStoredDefault.
                                                   {"auto", "neutral"},
                                                   {"auto_strength", 0.5f}}));

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
    // Six fields and the auto colour of new settings.
    EXPECT_EQ(json.at("film_print").size(), 8u);

    const auto restored = PipelineDocument::FromJson(document.ToJson());
    EXPECT_EQ(restored.Drt()->Params().FilmPrint().negative, id);
    EXPECT_EQ(restored.Drt()->Params().FilmPrint().print_material, "fuji_ca_dpII");
    EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), json.dump());
  }
}

TEST(DrtFilmPrintModelTest, NegativeScanIsAValueOfThePrintMaterialKeyAndKeepsTheFlare) {
  // Negative Scan is first in the list, before Cine Look, and it is no film.
  EXPECT_EQ(kFilmPrintMaterialIds.front(), kFilmNegativeScanId);
  EXPECT_EQ(kFilmNegativeScanId, "negative_scan");
  EXPECT_EQ(kFilmPrintMaterialNames.front(), "Negative Scan");
  EXPECT_EQ(kFilmPrintMaterialIds[1], "cine_look");
  EXPECT_FALSE(IsFilmStockIdentifier(kFilmNegativeScanId));
  EXPECT_FALSE(IsFilmIdentifier(kFilmPrintMaterialIds, "lab_scan"));
  EXPECT_TRUE(IsFilmNegativeScan("kodak_5207", kFilmNegativeScanId));
  EXPECT_FALSE(IsFilmNegativeScan("kodak_5207", "kodak_2383"));
  // A colour reversal film reads no print material.
  EXPECT_FALSE(IsFilmNegativeScan("fuji_velvia_50", kFilmNegativeScanId));

  auto document               = CreateDefaultPipelineDocument();
  auto update                 = FilmPrintUpdate();
  update.film_print_material  = std::string{kFilmNegativeScanId};
  document.Drt()->Params().ApplyUpdate(update);
  const auto json = document.Drt()->Params().ToJson();
  // The same key holds Negative Scan. Its settings at their defaults write no key except the auto
  // colour of new settings, and the stored flare stays.
  EXPECT_EQ(json.at("film_print").at("print_material"), "negative_scan");
  EXPECT_EQ(json.at("film_print").size(), 8u);
  EXPECT_FLOAT_EQ(json.at("film_print").at("enlarger_flare").get<float>(), 0.05f);
  const auto restored = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(restored.Drt()->Params().FilmPrint().print_material, kFilmNegativeScanId);
  EXPECT_FLOAT_EQ(restored.Drt()->Params().FilmPrint().enlarger_flare, 0.05f);
  EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), json.dump());

  // A reversal film with a stored Negative Scan keeps it, as it keeps any print material.
  DrtParameterUpdate slide;
  slide.film_negative = "fuji_velvia_50";
  document.Drt()->Params().ApplyUpdate(slide);
  EXPECT_EQ(document.Drt()->Params().FilmPrint().print_material, kFilmNegativeScanId);
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
  // Negative Scan has no entry gamut compression, so the exposure and all its results change.
  expect_marks(DrtDirty::FilmExposure,
               [](DrtParameterUpdate& update) { update.film_print_material = "negative_scan"; });
  expect_marks(DrtDirty::FilmExposure, [](DrtParameterUpdate& update) {
    update.film_scene_light = std::string{kFilmAsShotLightId};
  });

  // Each setting of Negative Scan changes only the scan: its own field, and no field of the
  // canvases. The scanner settings (changes 9 to 13) mark FilmScanner: they build the scanner table
  // again, and the prescan of the auto colour reads them. The others mark FilmScan and act after
  // the scan.
  std::vector<DrtParameterUpdate> scan_changes(14);
  scan_changes[0].film_scan_auto           = std::string{"warm"};
  scan_changes[1].film_scan_auto_strength  = 0.75f;
  scan_changes[2].film_scan_auto_density   = 0.25f;
  scan_changes[3].film_scan_contrast       = 1.2f;
  scan_changes[4].film_scan_exposure_stops = -1.0f;
  scan_changes[5].film_scan_cyan           = 0.05f;
  scan_changes[6].film_scan_magenta        = -0.05f;
  scan_changes[7].film_scan_yellow         = 0.1f;
  scan_changes[8].film_scan_black          = 0.5f;
  scan_changes[9].film_scanner_red_nm      = 630.0f;
  scan_changes[10].film_scanner_green_nm   = 530.0f;
  scan_changes[11].film_scanner_blue_nm    = 460.0f;
  scan_changes[12].film_scanner_width_nm   = 60.0f;
  scan_changes[13].film_scanner_light      = std::string{"equal_energy"};
  for (std::size_t change = 0; change < scan_changes.size(); ++change) {
    SCOPED_TRACE(change);
    const bool scanner     = change >= 9;
    const auto own         = scanner ? DrtDirty::FilmScanner : DrtDirty::FilmScan;
    const auto other       = scanner ? DrtDirty::FilmScan : DrtDirty::FilmScanner;
    const auto scan_before = params.FieldsRevision(DirtyFieldMask{own});
    const auto other_scan  = params.FieldsRevision(DirtyFieldMask{other});
    std::array<ParameterRevision, 3> canvas_before{};
    for (std::size_t index = 0; index < film_fields.size(); ++index) {
      canvas_before[index] = params.FieldsRevision(DirtyFieldMask{film_fields[index]});
    }
    params.ApplyUpdate(scan_changes[change]);
    EXPECT_GT(params.FieldsRevision(DirtyFieldMask{own}), scan_before);
    EXPECT_EQ(params.FieldsRevision(DirtyFieldMask{other}), other_scan);
    for (std::size_t index = 0; index < film_fields.size(); ++index) {
      EXPECT_EQ(params.FieldsRevision(DirtyFieldMask{film_fields[index]}), canvas_before[index]);
    }
    EXPECT_EQ(params.FieldsRevision(other_fields), other_before);
    // The same value again changes nothing.
    const auto revision = params.Revision();
    params.ApplyUpdate(scan_changes[change]);
    EXPECT_EQ(params.Revision(), revision);
  }
}

TEST(DrtFilmPrintModelTest, AsShotIsASceneLightAndNegativeScanWritesOnlyKeysAwayFromTheDefault) {
  // As Shot is second in the list, after the reference light (issue 661).
  EXPECT_EQ(kFilmSceneLightIds[0], "reference");
  EXPECT_EQ(kFilmSceneLightIds[1], kFilmAsShotLightId);
  EXPECT_TRUE(IsFilmAsShotLight(kFilmAsShotLightId));
  EXPECT_FALSE(IsFilmAsShotLight("daylight"));
  // The defaults of new settings: auto colour neutral at strength 0.5 (user decision of
  // 2026-11-06, G8 study), and the defaults of the lab (P14 report, section 6): the scanner
  // 650/545/450 nm, 40 nm, halogen, and black 1.
  const FilmNegativeScanParams defaults;
  EXPECT_EQ(defaults.auto_colour, "neutral");
  EXPECT_EQ(defaults.auto_strength, 0.5f);
  EXPECT_EQ(kFilmScanAutoStoredDefault, "off");
  EXPECT_EQ(kFilmScanAutoStrengthStoredDefault, 0.8f);
  EXPECT_EQ(defaults.auto_density, 0.0f);
  EXPECT_EQ(defaults.contrast, 1.0f);
  EXPECT_EQ(defaults.exposure_stops, 0.0f);
  EXPECT_EQ(defaults.cyan, 0.0f);
  EXPECT_EQ(defaults.black, 1.0f);
  EXPECT_EQ(defaults.scanner_red_nm, 650.0f);
  EXPECT_EQ(defaults.scanner_green_nm, 545.0f);
  EXPECT_EQ(defaults.scanner_blue_nm, 450.0f);
  EXPECT_EQ(defaults.scanner_width_nm, 40.0f);
  EXPECT_EQ(defaults.scanner_light, "halogen");
  EXPECT_EQ(FilmPrintParams{}.scan, defaults);
  EXPECT_EQ(kFilmScanAutoIds.front(), "off");
  EXPECT_EQ(kFilmScannerLightIds.front(), "halogen");

  // New settings write the auto colour: a missing `auto` or `auto_strength` reads as the stored
  // default (off, 0.8), the value of a document of a build before G8.
  auto document              = CreateDefaultPipelineDocument();
  auto update                = FilmPrintUpdate();
  update.film_print_material = std::string{kFilmNegativeScanId};
  update.film_scene_light    = std::string{kFilmAsShotLightId};
  document.Drt()->Params().ApplyUpdate(update);
  const auto plain = document.Drt()->Params().ToJson();
  EXPECT_EQ(plain.at("film_print").size(), 8u);
  EXPECT_EQ(plain.at("film_print").at("scene_light"), "as_shot");
  EXPECT_EQ(plain.at("film_print").at("auto"), "neutral");
  EXPECT_EQ(plain.at("film_print").at("auto_strength").get<float>(), 0.5f);

  // A stored object of an old build, with no Negative Scan keys, is Neutral Scan (auto colour off,
  // strength 0.8), and it keeps its bytes, so its root id.
  auto old_json = plain;
  old_json["film_print"].erase("auto");
  old_json["film_print"].erase("auto_strength");
  DrtParamsModel old_document;
  old_document.LoadJson(old_json);
  EXPECT_EQ(old_document.FilmPrint().scan.auto_colour, "off");
  EXPECT_EQ(old_document.FilmPrint().scan.auto_strength, 0.8f);
  EXPECT_EQ(old_document.ToJson().dump(), old_json.dump());

  // A document with the settings at their defaults written out has the bytes of the document that
  // omits them.
  auto  explicit_defaults     = plain;
  auto& keys                  = explicit_defaults["film_print"];
  keys["auto"]                = "neutral";
  keys["auto_strength"]       = 0.5;
  keys["auto_density"]        = 0.0;
  keys["scan_contrast"]       = 1.0;
  keys["scan_exposure_stops"] = 0.0;
  keys["cyan"]                = 0.0;
  keys["magenta"]             = 0.0;
  keys["yellow"]              = 0.0;
  keys["black"]               = 1.0;
  keys["scanner_red_nm"]      = 650.0;
  keys["scanner_green_nm"]    = 545.0;
  keys["scanner_blue_nm"]     = 450.0;
  keys["scanner_width_nm"]    = 40.0;
  keys["scanner_light"]       = "halogen";
  DrtParamsModel read_defaults;
  read_defaults.LoadJson(explicit_defaults);
  EXPECT_EQ(read_defaults.ToJson().dump(), plain.dump());

  // Each setting away from its default writes its key, and the document reads it back.
  DrtParameterUpdate scan;
  scan.film_scan_auto           = std::string{"warm"};
  scan.film_scan_auto_strength  = 0.5f;
  scan.film_scan_auto_density   = 0.25f;
  scan.film_scan_contrast       = 1.25f;
  scan.film_scan_exposure_stops = -0.75f;
  scan.film_scan_cyan           = 0.05f;
  scan.film_scan_magenta        = -0.03f;
  scan.film_scan_yellow         = 0.125f;
  scan.film_scan_black          = 0.5f;
  scan.film_scanner_red_nm      = 630.0f;
  scan.film_scanner_green_nm    = 530.0f;
  scan.film_scanner_blue_nm     = 460.0f;
  scan.film_scanner_width_nm    = 60.0f;
  scan.film_scanner_light       = std::string{"equal_energy"};
  document.Drt()->Params().ApplyUpdate(scan);
  const auto set = document.Drt()->Params().ToJson().at("film_print");
  EXPECT_EQ(set.size(), 6u + 14u);
  EXPECT_EQ(set.at("auto"), "warm");
  EXPECT_FLOAT_EQ(set.at("scan_exposure_stops").get<float>(), -0.75f);
  EXPECT_FLOAT_EQ(set.at("scanner_width_nm").get<float>(), 60.0f);
  EXPECT_EQ(set.at("scanner_light"), "equal_energy");
  const auto restored = PipelineDocument::FromJson(document.ToJson());
  const auto film     = restored.Drt()->Params().FilmPrint();
  EXPECT_EQ(film.scan.auto_colour, "warm");
  EXPECT_EQ(film.scan.yellow, 0.125f);
  EXPECT_EQ(film.scan.scanner_red_nm, 630.0f);
  EXPECT_EQ(film.scan.scanner_light, "equal_energy");
  EXPECT_EQ(film.scene_light, kFilmAsShotLightId);
  EXPECT_EQ(restored.Drt()->Params().ToJson().dump(), document.Drt()->Params().ToJson().dump());
  // One setting back at its default removes its key again.
  DrtParameterUpdate back;
  back.film_scan_auto = std::string{"off"};
  document.Drt()->Params().ApplyUpdate(back);
  EXPECT_FALSE(document.Drt()->Params().ToJson().at("film_print").contains("auto"));
  // Another method keeps the settings in memory and writes no film print object.
  DrtParameterUpdate open;
  open.method = DrtMethod::OpenDrt;
  document.Drt()->Params().ApplyUpdate(open);
  EXPECT_FALSE(document.Drt()->Params().ToJson().contains("film_print"));
  EXPECT_EQ(document.Drt()->Params().FilmPrint().scan.yellow, 0.125f);
}

TEST(DrtFilmPrintModelTest, NegativeScanRejectsInvalidValuesAndNamesTheKey) {
  const RejectedJson cases[] = {
      {"auto", "vivid"},
      {"auto", 1},
      {"auto_strength", 1.1},
      {"auto_density", -0.1},
      {"scan_contrast", 0.4},
      {"scan_contrast", std::numeric_limits<double>::quiet_NaN()},
      {"scan_exposure_stops", 3.5},
      {"cyan", 0.31},
      {"magenta", -0.31},
      {"yellow", "warm"},
      {"black", 1.6},
      {"scanner_red_nm", 590.0},
      {"scanner_green_nm", 600.0},
      {"scanner_blue_nm", 395.0},
      {"scanner_width_nm", 5.0},
      {"scanner_width_nm", 125.0},
      {"scanner_light", "led"},
  };
  for (const auto& rejected : cases) {
    SCOPED_TRACE(std::string{rejected.key} + " = " + rejected.value.dump());
    DrtParamsModel params;
    params.ApplyUpdate(FilmPrintUpdate());
    const auto before                = params.ToJson().dump();
    auto       json                  = params.ToJson();
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
  }
  // An update out of range changes no field.
  DrtParamsModel     params;
  const auto         before = params.ToJson().dump();
  DrtParameterUpdate update;
  update.film_scan_contrast  = 1.2f;
  update.film_scanner_red_nm = 721.0f;
  EXPECT_THROW(params.ApplyUpdate(update), std::invalid_argument);
  EXPECT_EQ(params.ToJson().dump(), before);
  EXPECT_EQ(params.FilmPrint().scan.contrast, 1.0f);
  DrtParameterUpdate light;
  light.film_scanner_light = std::string{"led"};
  EXPECT_THROW(params.ApplyUpdate(light), std::invalid_argument);
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
