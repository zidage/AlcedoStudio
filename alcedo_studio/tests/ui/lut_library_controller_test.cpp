//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LUT target binding (plan L5): application reaches exactly the captured Color Grade, other
// selections browse without applying, target reads never submit, and the Editor strength
// control writes only the strength.

#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "color/color_encoding_catalog.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "lut_library_model_test_support.hpp"
#include "lut_target_test_support.hpp"
#include "support/recording_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_adjustment_model.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_encoding_model.hpp"
#include "ui/alcedo_main/album_backend/lut_library_model.hpp"

namespace alcedo::ui::test {
namespace {

auto NodeJson(const PipelineDocument& document, const NodeId& node) -> nlohmann::json {
  return document.Graph().FindNode(node)->ToJson();
}

/// Node JSON without the adjustment instance @p excluded.
auto NodeJsonWithout(const PipelineDocument& document, const NodeId& node,
                     const std::string& excluded) -> nlohmann::json {
  nlohmann::json json        = NodeJson(document, node);
  auto&          adjustments = json["adjustments"];
  for (auto it = adjustments.begin(); it != adjustments.end(); ++it) {
    if ((*it)["id"] == excluded) {
      adjustments.erase(it);
      break;
    }
  }
  return json;
}

auto LibraryFiles() -> std::vector<std::pair<std::string, std::string>> {
  return {{"kodak/portra_400.cube",
           CubeWithMetadata(FilmMetadata("user:portra", "spectral_film_lut", "Spectral Film LUT",
                                         "portra-400", "Portra 400", "Kodak"))},
          {"general/teal.cube", CubeWithMetadata({})},
          {"general/strip.cube", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"}};
}

auto LutTarget(const DocumentTargetSource& source, const NodeId& node) -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = node;
  target.adjustment_instance_id = AdjustmentInstanceId{source.LmtInstance(node)};
  target.field_key              = "lut";
  return target;
}

auto SetStrength(DocumentTargetSource& source, const NodeId& node, float strength) -> bool {
  EditorLutWrite write;
  write.strength = strength;
  std::string error;
  return ApplyEditorParameterWrite(source.Mutable(), LutTarget(source, node), write, &error);
}

/// Index of encoding @p id in the combo entries, or -1.
auto EntryIndex(const EditorLutEncodingModel& model, const QString& id) -> int {
  const QVariantList entries = model.entries();
  for (int index = 0; index < entries.size(); ++index) {
    if (entries[index].toMap().value(QStringLiteral("value")).toString() == id) return index;
  }
  return -1;
}

/// An official (`origin: alcedo`) film simulation with a print, stored as a loose library file.
auto OfficialFilmWithPrint() -> std::string {
  return CubeWithMetadata(
      R"({"schema":1,"id":"spectral_film_lut:kodak_vision3_250d:kodak_vision_2383","origin":"alcedo",)"
      R"("category":"film_simulation","source":{"id":"spectral_film_lut","name":"Spectral Film LUT"},)"
      R"("film":{"id":"kodak_vision3_250d","name":"Vision3 250D","brand":"Kodak"},)"
      R"("print":{"id":"kodak_vision_2383","name":"Vision 2383","brand":"Kodak","kind":"film"},)"
      R"("input_space":"ACEScc","output_space":"ACEScc"})");
}

}  // namespace

TEST(LutLibraryControllerTest, ApplyLutChangesOnlyCapturedColorGrade) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  ASSERT_TRUE(SetStrength(source, kGradeB, 0.4f));
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  ASSERT_TRUE(controller.canApply());
  EXPECT_EQ(controller.targetNodeId(), QStringLiteral("grade.b"));

  const PipelineDocument& document = source.Mutable();
  const std::string       lmt_b    = source.LmtInstance(kGradeB);
  const nlohmann::json    primary  = NodeJson(document, kPrimary);
  const nlohmann::json    grade_c  = NodeJson(document, kGradeC);
  const nlohmann::json    develop  = NodeJson(document, document.Develop()->Id());
  const nlohmann::json    drt      = NodeJson(document, document.Drt()->Id());
  const nlohmann::json    b_others = NodeJsonWithout(document, kGradeB, lmt_b);

  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:kodak/portra_400.cube")));
  // The selection moves before the session applies the queued write.
  source.selected = kGradeC;
  controller.reload();
  EXPECT_EQ(controller.targetNodeId(), QStringLiteral("grade.c"));
  ASSERT_EQ(source.ApplyQueued(), 1);

  const LmtModel* applied = source.Lmt(kGradeB);
  ASSERT_NE(applied, nullptr);
  EXPECT_EQ(applied->Reference(), LutReference{LibraryLutReference{"kodak/portra_400.cube"}});
  EXPECT_FLOAT_EQ(applied->Strength(), 0.4f);
  EXPECT_TRUE(IsEmptyLutReference(source.Lmt(kGradeC)->Reference()));
  EXPECT_TRUE(IsEmptyLutReference(source.Lmt(kPrimary)->Reference()));
  EXPECT_EQ(NodeJson(document, kPrimary), primary);
  EXPECT_EQ(NodeJson(document, kGradeC), grade_c);
  EXPECT_EQ(NodeJson(document, document.Develop()->Id()), develop);
  EXPECT_EQ(NodeJson(document, document.Drt()->Id()), drt);
  EXPECT_EQ(NodeJsonWithout(document, kGradeB, lmt_b), b_others);

  // A node deleted after the target was captured rejects the write; nothing changes.
  source.selected = kGradeC;
  controller.reload();
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:general/teal.cube")));
  ASSERT_TRUE(RemoveColorGradeAndBridge(source.Mutable(), kGradeC).empty());
  const nlohmann::json before_apply = document.ToJson();
  EXPECT_EQ(source.ApplyQueued(), 0);
  EXPECT_EQ(document.ToJson(), before_apply);

  // Entries that cannot be applied are rejected before a write.
  source.selected = kGradeB;
  controller.reload();
  QSignalSpy rejected(&controller, &LutLibraryController::applyRejected);
  const int  submits = source.submit_count;
  EXPECT_FALSE(controller.applyEntry(QStringLiteral("library:general/strip.cube")));
  EXPECT_FALSE(controller.applyEntry(QStringLiteral("library:general/absent.cube")));
  EXPECT_EQ(rejected.count(), 2);
  EXPECT_EQ(source.submit_count, submits);
  EXPECT_FALSE(controller.lastError().isEmpty());

  // Clearing removes only the reference; the strength stays configured.
  ASSERT_TRUE(controller.clearAssociation());
  ASSERT_EQ(source.ApplyQueued(), 1);
  EXPECT_TRUE(IsEmptyLutReference(source.Lmt(kGradeB)->Reference()));
  EXPECT_FLOAT_EQ(source.Lmt(kGradeB)->Strength(), 0.4f);
}

TEST(LutLibraryControllerTest, NonGradeSelectionAllowsBrowseButRejectsApply) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  LutLibraryModel browser;
  browser.setLibrary(library.Service());

  struct Case {
    const char*           name;
    std::function<void()> arrange;
    LutTargetState        expected;
  };
  const std::vector<Case> cases = {
      {"no photo", [&] { source.image_id = 0; }, LutTargetState::kNoImage},
      {"no node", [&] { source.selected = NodeId{}; }, LutTargetState::kNoNode},
      {"deleted node", [&] { source.selected = NodeId{"grade.deleted"}; }, LutTargetState::kNoNode},
      {"RAW", [&] { source.selected = source.Mutable().Develop()->Id(); },
       LutTargetState::kNotColorGrade},
      {"Output", [&] { source.selected = source.Mutable().Drt()->Id(); },
       LutTargetState::kNotColorGrade},
      {"Mask", [&] { source.mask = "mask.1"; }, LutTargetState::kMaskSelected},
      {"not editable", [&] { source.can_edit = false; }, LutTargetState::kNotEditable},
  };
  for (const Case& test_case : cases) {
    source.image_id = 7;
    source.selected = kGradeB;
    source.mask.clear();
    source.can_edit = true;
    test_case.arrange();
    controller.reload();
    SCOPED_TRACE(test_case.name);
    EXPECT_EQ(controller.state(), test_case.expected);
    EXPECT_FALSE(controller.canApply());
    EXPECT_FALSE(controller.targetMessage().isEmpty());
    QSignalSpy rejected(&controller, &LutLibraryController::applyRejected);
    EXPECT_FALSE(controller.applyEntry(QStringLiteral("library:general/teal.cube")));
    EXPECT_FALSE(controller.clearAssociation());
    EXPECT_EQ(rejected.count(), 2);
    EXPECT_EQ(rejected.front().front().toString(), controller.targetMessage());
    EXPECT_EQ(source.submit_count, 0);
    // Browsing and search still work.
    browser.setQueryText(QStringLiteral("teal"));
    browser.applyQueryNow();
    EXPECT_EQ(RowEntryIds(browser), QStringList{"library:general/teal.cube"});
    browser.clearFilters();
    EXPECT_EQ(browser.count(), 3);
  }
}

TEST(LutLibraryControllerTest, DevelopAndDrtPanelsApplyToLutPanelColorGrade) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);

  const PipelineDocument& document = source.Mutable();
  const NodeId            develop  = document.Develop()->Id();
  const NodeId            drt      = document.Drt()->Id();
  source.lut_panel_node            = kGradeC;

  struct Case {
    const char* panel;
    NodeId      selected;
  };
  const std::vector<Case> cases = {{"display", drt}, {"post", drt}, {"raw", develop}};
  const char*             entries[] = {"library:kodak/portra_400.cube",
                                       "library:general/teal.cube",
                                       "library:kodak/portra_400.cube"};
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const Case& test_case = cases[index];
    SCOPED_TRACE(test_case.panel);
    source.panel    = QString::fromLatin1(test_case.panel);
    source.selected = test_case.selected;
    controller.reload();
    ASSERT_EQ(controller.state(), LutTargetState::kReady);
    EXPECT_EQ(controller.targetNodeId(), QStringLiteral("grade.c"));

    const nlohmann::json develop_json = NodeJson(document, develop);
    const nlohmann::json drt_json     = NodeJson(document, drt);
    ASSERT_TRUE(controller.applyEntry(QString::fromLatin1(entries[index])));
    ASSERT_EQ(source.ApplyQueued(), 1);
    const std::string expected_path = index == 1 ? "general/teal.cube" : "kodak/portra_400.cube";
    EXPECT_EQ(source.Lmt(kGradeC)->Reference(),
              LutReference{LibraryLutReference{expected_path}});
    EXPECT_TRUE(IsEmptyLutReference(source.Lmt(kGradeB)->Reference()));
    EXPECT_EQ(NodeJson(document, develop), develop_json);
    EXPECT_EQ(NodeJson(document, drt), drt_json);
  }

  // A selected Color Grade stays the target on those panels.
  source.panel    = QStringLiteral("display");
  source.selected = kGradeB;
  controller.reload();
  EXPECT_EQ(controller.targetNodeId(), QStringLiteral("grade.b"));

  // No Color Grade for the LUT panel: the apply is rejected.
  source.selected       = drt;
  source.lut_panel_node = NodeId{};
  controller.reload();
  EXPECT_EQ(controller.state(), LutTargetState::kNoNode);
  EXPECT_FALSE(controller.canApply());

  // Geometry keeps the Develop node as the target and rejects LUT changes.
  source.panel          = QStringLiteral("geometry");
  source.selected       = develop;
  source.lut_panel_node = kGradeC;
  controller.reload();
  EXPECT_EQ(controller.state(), LutTargetState::kNotColorGrade);
  EXPECT_FALSE(controller.canApply());
  const int submits = source.submit_count;
  EXPECT_FALSE(controller.applyEntry(QStringLiteral("library:general/teal.cube")));
  EXPECT_EQ(source.submit_count, submits);
}

TEST(LutLibraryControllerTest, TargetProjectionReloadDoesNotSubmitHistory) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  {
    LutLibraryController writer;
    writer.setLibrary(library.Service());
    writer.SetTargetSource(&source);
    ASSERT_TRUE(writer.applyEntry(QStringLiteral("library:kodak/portra_400.cube")));
    ASSERT_EQ(source.ApplyQueued(), 1);
    ASSERT_TRUE(SetStrength(source, kGradeB, 0.25f));
  }
  const int            submits  = source.submit_count;
  const nlohmann::json document = source.Mutable().ToJson();

  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  RecordingSubmitter       submitter;
  EditorLutAdjustmentModel strength;
  strength.setSubmitter(&submitter);
  strength.setTarget(&controller);
  LutLibraryModel browser;
  browser.setLibrary(library.Service());
  QObject::connect(&controller, &LutLibraryController::associationChanged, &browser,
                   [&] { browser.setAppliedEntryId(controller.associationEntryId()); });
  browser.setAppliedEntryId(controller.associationEntryId());

  EXPECT_EQ(controller.associationEntryId(), QStringLiteral("library:kodak/portra_400.cube"));
  EXPECT_EQ(controller.associationName(), QStringLiteral("portra_400"));
  EXPECT_DOUBLE_EQ(controller.associationStrength(), 0.25);
  EXPECT_DOUBLE_EQ(strength.value(), 25.0);
  EXPECT_EQ(strength.statusText(), QStringLiteral("portra_400"));
  EXPECT_TRUE(browser.data(browser.index(browser.rowOfEntry(controller.associationEntryId())),
                           LutLibraryModel::AppliedRole)
                  .toBool());

  // Changing nodes and reopening read the target without writing.
  for (const NodeId& node : {kPrimary, kGradeC, source.Mutable().Develop()->Id(), kGradeB}) {
    source.selected = node;
    controller.reload();
    strength.setTarget(nullptr);
    strength.setTarget(&controller);
  }
  source.selected = kPrimary;
  controller.reload();
  EXPECT_FALSE(controller.hasAssociation());
  EXPECT_DOUBLE_EQ(strength.value(), 100.0);
  EXPECT_EQ(browser.appliedEntryId(), QString());
  source.selected = kGradeB;
  controller.reload();
  EXPECT_DOUBLE_EQ(strength.value(), 25.0);

  LutLibraryController reopened;
  reopened.setLibrary(library.Service());
  reopened.SetTargetSource(&source);
  EXPECT_EQ(reopened.associationEntryId(), controller.associationEntryId());

  EXPECT_EQ(source.submit_count, submits);
  EXPECT_TRUE(source.queued.empty());
  EXPECT_TRUE(submitter.calls.empty());
  EXPECT_EQ(source.Mutable().ToJson(), document);
}

TEST(LutLibraryControllerTest, MissingAssociationKeepsReferenceNameAndStrength) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:kodak/portra_400.cube")));
  ASSERT_EQ(source.ApplyQueued(), 1);
  ASSERT_TRUE(SetStrength(source, kGradeB, 0.6f));
  controller.reload();
  ASSERT_FALSE(controller.associationMissing());
  EditorLutAdjustmentModel strength;
  strength.setTarget(&controller);

  std::filesystem::remove(library.Root() / "kodak" / "portra_400.cube");
  ASSERT_EQ(library.Service()->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(library.WaitUntilIdle());
  controller.reload();
  EXPECT_TRUE(controller.associationMissing());
  EXPECT_TRUE(controller.hasAssociation());
  EXPECT_EQ(controller.associationName(), QStringLiteral("portra_400"));
  EXPECT_DOUBLE_EQ(controller.associationStrength(), static_cast<double>(0.6f));
  EXPECT_TRUE(strength.missing());
  EXPECT_EQ(strength.statusText(), QStringLiteral("Missing: portra_400"));
  EXPECT_EQ(source.Lmt(kGradeB)->Reference(),
            LutReference{LibraryLutReference{"kodak/portra_400.cube"}});

  // The returned file restores the association on the next inventory publication.
  library.Write("kodak/portra_400.cube",
                CubeWithMetadata(FilmMetadata("user:portra", "spectral_film_lut",
                                              "Spectral Film LUT", "portra-400", "Portra 400",
                                              "Kodak")));
  ASSERT_EQ(library.Service()->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(library.WaitUntilIdle());
  EXPECT_FALSE(controller.associationMissing());
  EXPECT_EQ(controller.associationEntryId(), QStringLiteral("library:kodak/portra_400.cube"));
}

TEST(LutLibraryControllerTest, StrengthControlWritesOnlyStrength) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  RecordingSubmitter       submitter;
  EditorLutAdjustmentModel strength;
  strength.setSubmitter(&submitter);
  strength.setTarget(&controller);
  EXPECT_EQ(strength.fieldKey(), QStringLiteral("lut"));
  EXPECT_DOUBLE_EQ(strength.value(), 100.0);
  EXPECT_EQ(strength.statusText(), QStringLiteral("No LUT"));

  strength.beginDrag();
  strength.updateDrag(50.0);
  strength.updateDrag(35.0);
  strength.finishDrag();
  ASSERT_EQ(submitter.calls.size(), 3u);
  EXPECT_EQ(submitter.interactiveCount(), 2);
  EXPECT_EQ(submitter.settledCount(), 1);
  for (const RecordedAdjustmentCall& call : submitter.calls) {
    EXPECT_EQ(call.fieldKey, QStringLiteral("lut"));
    const auto* write = std::get_if<EditorLutWrite>(&call.write);
    ASSERT_NE(write, nullptr);
    EXPECT_FALSE(write->reference.has_value());
    ASSERT_TRUE(write->strength.has_value());
  }
  EXPECT_FLOAT_EQ(*std::get<EditorLutWrite>(submitter.calls.back().write).strength, 0.35f);

  // Applied to the document, the strength write keeps the selected LUT.
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:general/teal.cube")));
  ASSERT_EQ(source.ApplyQueued(), 1);
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = kGradeB;
  target.adjustment_instance_id = AdjustmentInstanceId{source.LmtInstance(kGradeB)};
  target.field_key              = "lut";
  std::string error;
  ASSERT_TRUE(ApplyEditorParameterWrite(source.Mutable(), target, submitter.calls.back().write,
                                        &error))
      << error;
  controller.reload();
  EXPECT_EQ(source.Lmt(kGradeB)->Reference(), LutReference{LibraryLutReference{"general/teal.cube"}});
  EXPECT_NEAR(strength.value(), 35.0, 1e-4);
}

// Plan 1.4 (L6A): the browser row, the stored association name, and the indicator show the film
// brand and stock as the title and the print on a separate line.
TEST(LutLibraryControllerTest, AssociationShowsPrintAsSeparateLine) {
  TemporaryLutLibrary library(
      {{"films/kodak_vision3_250d__kodak_vision_2383.cube", OfficialFilmWithPrint()}});
  const QString entry_id =
      QStringLiteral("library:films/kodak_vision3_250d__kodak_vision_2383.cube");
  LutLibraryModel browser;
  browser.setLibrary(library.Service());
  ASSERT_EQ(browser.count(), 1);
  const QModelIndex row = browser.index(0);
  EXPECT_EQ(browser.entryIdAt(0), entry_id);
  EXPECT_EQ(browser.data(row, LutLibraryModel::DisplayNameRole).toString(),
            QStringLiteral("Kodak Vision3 250D"));
  EXPECT_EQ(browser.data(row, LutLibraryModel::PrintNameRole).toString(),
            QStringLiteral("Vision 2383"));

  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  EXPECT_TRUE(controller.associationPrintName().isEmpty());
  QSignalSpy association_changes(&controller, &LutLibraryController::associationChanged);
  ASSERT_TRUE(controller.applyEntry(entry_id));
  ASSERT_EQ(source.ApplyQueued(), 1);
  controller.reload();
  EXPECT_GE(association_changes.count(), 1);
  EXPECT_EQ(source.Lmt(kGradeB)->DisplayName(), "Kodak Vision3 250D");
  EXPECT_EQ(controller.associationName(), QStringLiteral("Kodak Vision3 250D"));
  EXPECT_EQ(controller.associationPrintName(), QStringLiteral("Vision 2383"));
  EXPECT_EQ(controller.associationEntryId(), entry_id);

  // Clearing the association clears the print line with the name.
  ASSERT_TRUE(controller.clearAssociation());
  ASSERT_EQ(source.ApplyQueued(), 1);
  controller.reload();
  EXPECT_FALSE(controller.hasAssociation());
  EXPECT_TRUE(controller.associationPrintName().isEmpty());
}

// Plan L6A: the small Editor control only loads. Target reloads, node changes, and a missing file
// that returns restore its strength and Missing state without a single submit.
TEST(LutLibraryControllerTest, EditorLutControlReloadDoesNotCommit) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:general/teal.cube")));
  source.selected = kGradeC;
  controller.reload();
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:kodak/portra_400.cube")));
  ASSERT_EQ(source.ApplyQueued(), 2);
  ASSERT_TRUE(SetStrength(source, kGradeB, 0.4f));
  ASSERT_TRUE(SetStrength(source, kGradeC, 0.8f));
  const int                submits_after_setup = source.submit_count;

  RecordingSubmitter       submitter;
  EditorLutAdjustmentModel control;
  control.setSubmitter(&submitter);
  control.setTarget(&controller);

  source.selected = kGradeB;
  controller.reload();
  EXPECT_NEAR(control.value(), 40.0, 1e-4);
  EXPECT_EQ(control.statusText(), QStringLiteral("teal"));
  EXPECT_FALSE(control.missing());

  source.selected = kGradeC;
  controller.reload();
  controller.reload();
  EXPECT_NEAR(control.value(), 80.0, 1e-4);
  EXPECT_EQ(control.associationName(), QStringLiteral("portra_400"));

  std::filesystem::remove(library.Root() / "kodak" / "portra_400.cube");
  ASSERT_EQ(library.Service()->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(library.WaitUntilIdle());
  controller.reload();
  EXPECT_TRUE(control.missing());
  EXPECT_EQ(control.statusText(), QStringLiteral("Missing: portra_400"));
  EXPECT_NEAR(control.value(), 80.0, 1e-4);

  library.Write(
      "kodak/portra_400.cube",
      CubeWithMetadata(FilmMetadata("user:portra", "spectral_film_lut", "Spectral Film LUT",
                                    "portra-400", "Portra 400", "Kodak")));
  ASSERT_EQ(library.Service()->RefreshInventory(), LutLibraryService::Status::kOk);
  ASSERT_TRUE(library.WaitUntilIdle());
  EXPECT_FALSE(control.missing());
  EXPECT_NEAR(control.value(), 80.0, 1e-4);

  EXPECT_TRUE(submitter.calls.empty());
  EXPECT_EQ(source.submit_count, submits_after_setup);
  EXPECT_TRUE(source.queued.empty());
  EXPECT_FLOAT_EQ(source.Lmt(kGradeB)->Strength(), 0.4f);
  EXPECT_FLOAT_EQ(source.Lmt(kGradeC)->Strength(), 0.8f);
}

// A LUT from an installed package is applied by its official reference, so the photo follows the
// package across content updates; a loose file is applied by its library path. (Moved from the
// removed EditorLutCatalogModel tests in L6A.)
TEST(LutLibraryControllerTest, ApplyingAPackageEntrySubmitsItsOfficialReference) {
  TemporaryLutLibrary library(
      {{"packages/spectral_film_lut/content/a/kodak-5207.cube",
        CubeWithMetadata(R"({"schema":1,"id":"kodak-5207","origin":"alcedo","category":"general",)"
                         R"("input_space":"ACEScc","output_space":"ACEScc"})")},
       {"kodak/look.cube", CubeWithMetadata({})}},
      nullptr, [](const std::filesystem::path& root) {
        // Active content of an installed package: its receipt names the content directory.
        LutPackageReceipt receipt;
        receipt.package_id        = "spectral_film_lut";
        receipt.content_directory = "packages/spectral_film_lut/content/a";
        EXPECT_TRUE(WriteLutPackageReceiptFile(root, receipt).empty());
      });
  LutLibraryModel browser;
  browser.setLibrary(library.Service());
  ASSERT_TRUE(
      RowEntryIds(browser).contains(QStringLiteral("official:spectral_film_lut/kodak-5207")))
      << RowEntryIds(browser).join(QLatin1Char(' ')).toStdString();

  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("official:spectral_film_lut/kodak-5207")))
      << controller.lastError().toStdString();
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:kodak/look.cube")));
  ASSERT_EQ(source.queued.size(), 2u);
  const EditorLutWrite& official = source.queued[0].second;
  ASSERT_TRUE(official.reference.has_value());
  EXPECT_EQ(*official.reference,
            (LutReference{OfficialLutReference{"spectral_film_lut", "kodak-5207"}}));
  EXPECT_FALSE(official.display_name.empty());
  EXPECT_FALSE(official.strength.has_value());
  const EditorLutWrite& loose = source.queued[1].second;
  EXPECT_EQ(*loose.reference, LutReference{LibraryLutReference{"kodak/look.cube"}});
}

// L4: the encoding combos list every catalog encoding, scene-referred first, and each selection
// is one settled `lut` write of its own side only, so the reference, the strength and the other
// side stay as stored.
TEST(LutLibraryControllerTest, EncodingSelectionSubmitsOneSettledWriteOfThatSideOnly) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:general/teal.cube")));
  ASSERT_EQ(source.ApplyQueued(), 1);
  ASSERT_TRUE(SetStrength(source, kGradeB, 0.4f));
  controller.reload();

  RecordingSubmitter     submitter;
  EditorLutEncodingModel input;
  EditorLutEncodingModel output;
  output.setSide(QStringLiteral("output"));
  for (EditorLutEncodingModel* model : {&input, &output}) {
    model->setSubmitter(&submitter);
    model->setTarget(&controller);
  }
  const auto encodings = color::ColorEncodings();
  ASSERT_EQ(input.entries().size(), static_cast<qsizetype>(encodings.size()));
  bool seen_display = false;
  for (const QVariant& entry : input.entries()) {
    const bool display =
        entry.toMap().value(QStringLiteral("group")).toString() == QStringLiteral("display");
    EXPECT_FALSE(seen_display && !display) << "scene encodings are listed first";
    seen_display = seen_display || display;
  }
  EXPECT_TRUE(seen_display);
  EXPECT_EQ(input.currentValue(), QStringLiteral("acescc"));
  EXPECT_EQ(input.defaultIndex(), EntryIndex(input, QStringLiteral("acescc")));
  EXPECT_FALSE(output.displayReferred());

  output.selectIndex(EntryIndex(output, QStringLiteral("rec709_bt1886")));
  input.selectIndex(EntryIndex(input, QStringLiteral("sony_slog3_sgamut3cine")));
  ASSERT_EQ(submitter.calls.size(), 2u);
  EXPECT_EQ(submitter.settledCount(), 2);
  const auto& output_write = std::get<EditorLutWrite>(submitter.calls[0].write);
  EXPECT_EQ(output_write.output_encoding, std::optional<std::string>{"rec709_bt1886"});
  EXPECT_FALSE(output_write.input_encoding.has_value());
  EXPECT_FALSE(output_write.reference.has_value());
  EXPECT_FALSE(output_write.strength.has_value());
  const auto& input_write = std::get<EditorLutWrite>(submitter.calls[1].write);
  EXPECT_EQ(input_write.input_encoding, std::optional<std::string>{"sony_slog3_sgamut3cine"});
  EXPECT_FALSE(input_write.output_encoding.has_value());

  // Both writes are applied after both selections, as a busy session applies them: neither
  // write resets the other side.
  std::string error;
  for (const RecordedAdjustmentCall& call : submitter.calls) {
    ASSERT_TRUE(
        ApplyEditorParameterWrite(source.Mutable(), LutTarget(source, kGradeB), call.write, &error))
        << error;
  }
  controller.reload();
  const LmtModel* lmt = source.Lmt(kGradeB);
  ASSERT_NE(lmt, nullptr);
  EXPECT_EQ(lmt->InputEncoding(), "sony_slog3_sgamut3cine");
  EXPECT_EQ(lmt->OutputEncoding(), "rec709_bt1886");
  EXPECT_EQ(lmt->Reference(), LutReference{LibraryLutReference{"general/teal.cube"}});
  EXPECT_FLOAT_EQ(lmt->Strength(), 0.4f);
  EXPECT_EQ(controller.inputEncoding(), QStringLiteral("sony_slog3_sgamut3cine"));
  EXPECT_EQ(output.currentValue(), QStringLiteral("rec709_bt1886"));
  EXPECT_TRUE(output.displayReferred());
  EXPECT_FALSE(input.displayReferred());

  // Reset writes the default of its own side only.
  output.reset();
  ASSERT_EQ(submitter.calls.size(), 3u);
  const auto& reset_write = std::get<EditorLutWrite>(submitter.calls[2].write);
  EXPECT_EQ(reset_write.output_encoding, std::optional<std::string>{"acescc"});
  EXPECT_FALSE(reset_write.input_encoding.has_value());

  // Removing the LUT keeps the encodings.
  ASSERT_TRUE(controller.clearAssociation());
  ASSERT_EQ(source.ApplyQueued(), 1);
  controller.reload();
  EXPECT_FALSE(controller.hasAssociation());
  EXPECT_EQ(controller.inputEncoding(), QStringLiteral("sony_slog3_sgamut3cine"));
  EXPECT_EQ(controller.outputEncoding(), QStringLiteral("rec709_bt1886"));
}

// L4: the panel shows the stored encodings after the document is written and read again in
// the project document format, as reopening the project does. Loading never submits.
TEST(LutLibraryControllerTest, StoredEncodingsAreShownAfterReopeningTheDocument) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  EditorLutWrite       write;
  write.reference       = LibraryLutReference{"general/teal.cube"};
  write.input_encoding  = "arri_logc4_awg4";
  write.output_encoding = "rec2100_pq1000";
  std::string error;
  ASSERT_TRUE(
      ApplyEditorParameterWrite(source.Mutable(), LutTarget(source, kGradeB), write, &error))
      << error;
  const std::string    stored = source.Mutable().ToJson().dump();

  DocumentTargetSource reopened;
  reopened.ReplaceDocument(PipelineDocument::FromJson(nlohmann::json::parse(stored)));
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&reopened);
  RecordingSubmitter     submitter;
  EditorLutEncodingModel input;
  EditorLutEncodingModel output;
  output.setSide(QStringLiteral("output"));
  for (EditorLutEncodingModel* model : {&input, &output}) {
    model->setSubmitter(&submitter);
    model->setTarget(&controller);
  }

  EXPECT_TRUE(controller.hasAssociation());
  EXPECT_EQ(controller.inputEncoding(), QStringLiteral("arri_logc4_awg4"));
  EXPECT_EQ(controller.outputEncoding(), QStringLiteral("rec2100_pq1000"));
  EXPECT_EQ(input.currentValue(), QStringLiteral("arri_logc4_awg4"));
  EXPECT_EQ(output.currentValue(), QStringLiteral("rec2100_pq1000"));
  EXPECT_TRUE(output.displayReferred());
  EXPECT_TRUE(submitter.calls.empty()) << "loading the stored encodings never submits";
  EXPECT_EQ(reopened.submit_count, 0);
}

// L4: a 3D LUT with a 1D shaper is listed, marked unsupported with its own reason, and cannot be
// applied; a 1D-only LUT keeps its reason.
TEST(LutLibraryControllerTest, ShaperLutIsUnsupportedWithShaperReasonAndIsNotApplied) {
  std::string shaper = "LUT_1D_SIZE 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n";
  for (int i = 0; i < 8; ++i) shaper += "0.5 0.5 0.5\n";
  auto files = LibraryFiles();
  files.emplace_back("general/shaper.cube", shaper);
  TemporaryLutLibrary  library(files);
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);
  LutLibraryModel model;
  model.setLibrary(library.Service());

  const int shaper_row = model.rowOfEntry(QStringLiteral("library:general/shaper.cube"));
  ASSERT_GE(shaper_row, 0);
  const QModelIndex shaper_index = model.index(shaper_row);
  EXPECT_FALSE(model.data(shaper_index, LutLibraryModel::SelectableRole).toBool());
  EXPECT_EQ(model.data(shaper_index, LutLibraryModel::StatusTextRole).toString(),
            QStringLiteral("Unsupported"));
  EXPECT_EQ(model.data(shaper_index, LutLibraryModel::DetailTextRole).toString(),
            QStringLiteral("1D shaper LUTs are not supported."));
  const int strip_row = model.rowOfEntry(QStringLiteral("library:general/strip.cube"));
  ASSERT_GE(strip_row, 0);
  EXPECT_EQ(model.data(model.index(strip_row), LutLibraryModel::DetailTextRole).toString(),
            QStringLiteral("1D LUTs cannot be applied by the grade stage."));

  EXPECT_FALSE(controller.applyEntry(QStringLiteral("library:general/shaper.cube")));
  EXPECT_EQ(controller.lastError(), QStringLiteral("1D shaper LUTs are not supported."));
  EXPECT_EQ(source.submit_count, 0);
}

}  // namespace alcedo::ui::test
