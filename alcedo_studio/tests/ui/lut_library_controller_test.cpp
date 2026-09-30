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
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "lut_library_model_test_support.hpp"
#include "support/recording_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_adjustment_model.hpp"
#include "ui/alcedo_main/album_backend/lut_library_model.hpp"

namespace alcedo::ui::test {
namespace {

const NodeId kPrimary{"grade.primary"};
const NodeId kGradeB{"grade.b"};
const NodeId kGradeC{"grade.c"};

/**
 * Target source over a real PipelineDocument. Writes are queued like the session's pending
 * input and applied later with ApplyEditorParameterWrite, the owner operation that validates
 * the target again, so a selection change between submit and apply is observable.
 */
class DocumentTargetSource final : public LutTargetSource {
 public:
  DocumentTargetSource() : document_(std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument())) {
    EXPECT_TRUE(AddCleanColorGrade(*document_, document_->Drt()->Id(), kGradeB).empty());
    EXPECT_TRUE(AddCleanColorGrade(*document_, document_->Drt()->Id(), kGradeC).empty());
  }

  [[nodiscard]] auto ImageId() const -> std::uint64_t override { return image_id; }
  [[nodiscard]] auto Document() const -> std::shared_ptr<const PipelineDocument> override {
    return image_id == 0 ? nullptr : document_;
  }
  [[nodiscard]] auto SelectedNodeId() const -> NodeId override { return selected; }
  [[nodiscard]] auto SelectedMaskId() const -> std::string override { return mask; }
  [[nodiscard]] auto CanEdit() const -> bool override { return can_edit; }
  auto SubmitLutWrite(const EditorParameterTarget& target, EditorLutWrite write) -> bool override {
    ++submit_count;
    queued.emplace_back(target, std::move(write));
    return true;
  }

  /// Apply queued writes in order; returns the number the owner accepted.
  auto ApplyQueued() -> int {
    int accepted = 0;
    while (!queued.empty()) {
      auto [target, write] = std::move(queued.front());
      queued.pop_front();
      std::string error;
      if (ApplyEditorParameterWrite(*document_, target, write, &error)) ++accepted;
    }
    return accepted;
  }

  [[nodiscard]] auto Mutable() -> PipelineDocument& { return *document_; }
  [[nodiscard]] auto Lmt(const NodeId& node) const -> const LmtModel* {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document_->Graph().FindNode(node));
    if (grade == nullptr) return nullptr;
    const auto* id = grade->FindAdjustmentIdByType(type_ids::Lmt());
    return id == nullptr ? nullptr : dynamic_cast<const LmtModel*>(grade->FindAdjustment(*id));
  }
  [[nodiscard]] auto LmtInstance(const NodeId& node) const -> std::string {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document_->Graph().FindNode(node));
    const auto* id    = grade->FindAdjustmentIdByType(type_ids::Lmt());
    return std::string(id->Value());
  }

  std::uint64_t image_id = 7;
  NodeId        selected = kGradeB;
  std::string   mask;
  bool          can_edit     = true;
  int           submit_count = 0;
  std::deque<std::pair<EditorParameterTarget, EditorLutWrite>> queued;

 private:
  std::shared_ptr<PipelineDocument> document_;
};

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

auto SetStrength(DocumentTargetSource& source, const NodeId& node, float strength) -> bool {
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = node;
  target.adjustment_instance_id = AdjustmentInstanceId{source.LmtInstance(node)};
  target.field_key              = "lut";
  EditorLutWrite write;
  write.strength = strength;
  std::string error;
  return ApplyEditorParameterWrite(source.Mutable(), target, write, &error);
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

}  // namespace alcedo::ui::test
