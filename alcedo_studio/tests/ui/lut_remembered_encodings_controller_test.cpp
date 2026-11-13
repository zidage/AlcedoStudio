//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Remembered LUT encodings in the Editor (lut_color_encoding_plan.md, Phase L5): the "remember"
// checkbox writes the library only, a checked combo selection updates its side of the pair, a
// load never writes the library, and applying a LUT with a remembered pair writes the reference
// and both encodings in one settled write.

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "app/lut_library_inventory.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "lut_library_model_test_support.hpp"
#include "lut_target_test_support.hpp"
#include "support/recording_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_lut_encoding_model.hpp"
#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"

namespace alcedo::ui::test {
namespace {

const QString                kTealId = QStringLiteral("library:general/teal.cube");
const LutRememberedEncodings kSlog3ToRec709{"sony_slog3_sgamut3cine", "rec709_bt1886"};

auto                         Files() -> std::vector<std::pair<std::string, std::string>> {
  return {{"general/teal.cube", CubeWithMetadata({})}, {"general/warm.cube", CubeWithMetadata({})}};
}

auto LutTarget(const DocumentTargetSource& source, const NodeId& node) -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = node;
  target.adjustment_instance_id = AdjustmentInstanceId{source.LmtInstance(node)};
  target.field_key              = "lut";
  return target;
}

/// Apply @p write to grade B directly, as the session applies a queued write or as undo and
/// history replay apply a stored LMT state.
auto ApplyToGradeB(DocumentTargetSource& source, const EditorLutWrite& write) -> bool {
  std::string error;
  const bool  applied =
      ApplyEditorParameterWrite(source.Mutable(), LutTarget(source, kGradeB), write, &error);
  EXPECT_TRUE(applied) << error;
  return applied;
}

auto EncodingWrite(std::string input, std::string output) -> EditorLutWrite {
  EditorLutWrite write;
  write.input_encoding  = std::move(input);
  write.output_encoding = std::move(output);
  return write;
}

auto EntryIndex(const EditorLutEncodingModel& model, const QString& id) -> int {
  const QVariantList entries = model.entries();
  for (int index = 0; index < entries.size(); ++index) {
    if (entries[index].toMap().value(QStringLiteral("value")).toString() == id) return index;
  }
  return -1;
}

auto GradeB(const DocumentTargetSource& source) -> const ColorGradeNodeModel& {
  return dynamic_cast<const ColorGradeNodeModel&>(*source.Document()->Graph().FindNode(kGradeB));
}

/// A controller bound to grade B with the teal LUT applied and the encodings @p input / @p output
/// stored in the document.
struct BoundTarget {
  explicit BoundTarget(TemporaryLutLibrary& library, const std::string& input = "acescc",
                       const std::string& output = "acescc") {
    controller.setLibrary(library.Service());
    controller.SetTargetSource(&source);
    EXPECT_TRUE(controller.applyEntry(kTealId));
    EXPECT_EQ(source.ApplyQueued(), 1);
    ApplyToGradeB(source, EncodingWrite(input, output));
    controller.reload();
  }

  DocumentTargetSource source;
  LutLibraryController controller;
};

TEST(LutRememberedEncodingsControllerTest, CheckingStoresShownPairAndUncheckingForgetsIt) {
  TemporaryLutLibrary library(Files());
  LutLibraryService&  service = *library.Service();
  BoundTarget         bound(library, kSlog3ToRec709.input_encoding, kSlog3ToRec709.output_encoding);
  LutLibraryController& controller = bound.controller;
  ASSERT_EQ(controller.associationEntryId(), kTealId);
  EXPECT_TRUE(controller.canRememberEncodings());
  EXPECT_FALSE(controller.rememberEncodings());
  const int  submits = bound.source.submit_count;
  QSignalSpy association(&controller, &LutLibraryController::associationChanged);

  ASSERT_TRUE(controller.setRememberEncodings(true));
  EXPECT_EQ(service.RememberedEncodings(kTealId.toStdString()), kSlog3ToRec709);
  EXPECT_TRUE(controller.rememberEncodings());
  EXPECT_GE(association.count(), 1);

  ASSERT_TRUE(controller.setRememberEncodings(false));
  EXPECT_FALSE(service.RememberedEncodings(kTealId.toStdString()).has_value());
  EXPECT_FALSE(controller.rememberEncodings());

  // The checkbox is not an edit: nothing reaches the editor session.
  EXPECT_EQ(bound.source.submit_count, submits);
  EXPECT_TRUE(bound.source.queued.empty());

  // Without a LUT there is no entry to remember for.
  ASSERT_TRUE(controller.clearAssociation());
  ASSERT_EQ(bound.source.ApplyQueued(), 1);
  controller.reload();
  EXPECT_FALSE(controller.canRememberEncodings());
  EXPECT_FALSE(controller.setRememberEncodings(true));
  EXPECT_FALSE(controller.lastError().isEmpty());
}

TEST(LutRememberedEncodingsControllerTest, CheckedSelectionUpdatesItsSideAndLoadsNeverWrite) {
  TemporaryLutLibrary   library(Files());
  LutLibraryService&    service = *library.Service();
  BoundTarget           bound(library);
  LutLibraryController& controller = bound.controller;
  ASSERT_TRUE(controller.setRememberEncodings(true));
  const std::string teal = kTealId.toStdString();
  ASSERT_EQ(service.RememberedEncodings(teal), (LutRememberedEncodings{"acescc", "acescc"}));

  RecordingSubmitter     submitter;
  EditorLutEncodingModel input;
  EditorLutEncodingModel output;
  output.setSide(QStringLiteral("output"));
  for (EditorLutEncodingModel* model : {&input, &output}) {
    model->setSubmitter(&submitter);
    model->setTarget(&controller);
  }
  QSignalSpy library_writes(&service, &LutLibraryService::RememberedEncodingsChanged);

  // One selection: one settled document write, and only its side of the pair changes.
  output.selectIndex(EntryIndex(output, QStringLiteral("rec709_bt1886")));
  ASSERT_EQ(submitter.calls.size(), 1u);
  EXPECT_EQ(submitter.settledCount(), 1);
  EXPECT_EQ(service.RememberedEncodings(teal), (LutRememberedEncodings{"acescc", "rec709_bt1886"}));
  EXPECT_EQ(library_writes.count(), 1);

  // Publishing the write and loading the target again (node change, reopen) never writes the
  // library.
  ASSERT_TRUE(ApplyToGradeB(bound.source, std::get<EditorLutWrite>(submitter.calls[0].write)));
  controller.reload();
  EditorLutEncodingModel reopened;
  reopened.setSide(QStringLiteral("output"));
  reopened.setSubmitter(&submitter);
  reopened.setTarget(&controller);
  EXPECT_EQ(reopened.currentValue(), QStringLiteral("rec709_bt1886"));
  EXPECT_EQ(library_writes.count(), 1);
  EXPECT_EQ(submitter.calls.size(), 1u);

  // Undo restores the previous document state through a load: the remembered pair stays.
  ASSERT_TRUE(ApplyToGradeB(bound.source, EncodingWrite("acescc", "acescc")));
  controller.reload();
  EXPECT_EQ(output.currentValue(), QStringLiteral("acescc"));
  EXPECT_EQ(service.RememberedEncodings(teal), (LutRememberedEncodings{"acescc", "rec709_bt1886"}));
  EXPECT_EQ(library_writes.count(), 1);

  // A write the session does not accept does not change the pair.
  submitter.canEditState = false;
  input.selectIndex(EntryIndex(input, QStringLiteral("arri_logc4_awg4")));
  EXPECT_EQ(service.RememberedEncodings(teal), (LutRememberedEncodings{"acescc", "rec709_bt1886"}));
  submitter.canEditState = true;

  // Unchecked: a selection edits the document only.
  ASSERT_TRUE(controller.setRememberEncodings(false));
  const int writes_after_forget = library_writes.count();
  input.selectIndex(EntryIndex(input, QStringLiteral("sony_slog3_sgamut3cine")));
  ASSERT_EQ(submitter.calls.size(), 2u);
  EXPECT_FALSE(service.RememberedEncodings(teal).has_value());
  EXPECT_EQ(library_writes.count(), writes_after_forget);
}

TEST(LutRememberedEncodingsControllerTest, ApplyWithRememberedPairWritesReferenceAndEncodingsOnce) {
  TemporaryLutLibrary library(Files());
  LutLibraryService&  service = *library.Service();
  ASSERT_EQ(service.SetRememberedEncodings(kTealId.toStdString(), kSlog3ToRec709),
            LutLibraryService::Status::kOk);
  DocumentTargetSource source;
  LutLibraryController controller;
  controller.setLibrary(library.Service());
  controller.SetTargetSource(&source);

  ASSERT_TRUE(controller.applyEntry(kTealId));
  ASSERT_EQ(source.submit_count, 1) << "the LUT and its encodings are one settled write";
  ASSERT_EQ(source.queued.size(), 1u);
  const EditorLutWrite& write = source.queued.front().second;
  EXPECT_EQ(write.reference, std::optional<LutReference>{LibraryLutReference{"general/teal.cube"}});
  EXPECT_EQ(write.input_encoding, std::optional<std::string>{kSlog3ToRec709.input_encoding});
  EXPECT_EQ(write.output_encoding, std::optional<std::string>{kSlog3ToRec709.output_encoding});
  ASSERT_EQ(source.ApplyQueued(), 1);
  controller.reload();
  EXPECT_TRUE(controller.rememberEncodings());
  const LmtModel* applied = source.Lmt(kGradeB);
  ASSERT_NE(applied, nullptr);
  EXPECT_EQ(applied->InputEncoding(), kSlog3ToRec709.input_encoding);
  EXPECT_EQ(applied->OutputEncoding(), kSlog3ToRec709.output_encoding);

  // The same LUT with the same encodings selected by hand gives the same LMT and the same
  // sampled table.
  DocumentTargetSource by_hand;
  LutLibraryController by_hand_controller;
  by_hand_controller.setLibrary(library.Service());
  by_hand_controller.SetTargetSource(&by_hand);
  ASSERT_EQ(service.SetRememberedEncodings(kTealId.toStdString(), std::nullopt),
            LutLibraryService::Status::kOk);
  ASSERT_TRUE(by_hand_controller.applyEntry(kTealId));
  ASSERT_EQ(by_hand.ApplyQueued(), 1);
  ASSERT_TRUE(ApplyToGradeB(
      by_hand, EncodingWrite(kSlog3ToRec709.input_encoding, kSlog3ToRec709.output_encoding)));
  EXPECT_EQ(applied->ToJson(), by_hand.Lmt(kGradeB)->ToJson());
  const auto resources     = library.Service()->Resources();
  const auto applied_table = TryPackGradeLut(GradeB(source), *resources);
  const auto by_hand_table = TryPackGradeLut(GradeB(by_hand), *resources);
  ASSERT_NE(applied_table, nullptr);
  ASSERT_NE(by_hand_table, nullptr);
  EXPECT_EQ(applied_table->edge, 65u) << "a display output is baked into the composite table";
  EXPECT_EQ(applied_table->key, by_hand_table->key);

  // A LUT without a remembered pair applies ACEScc to ACEScc in the same write, so the
  // encodings of the previous LUT do not stay.
  ASSERT_TRUE(controller.applyEntry(QStringLiteral("library:general/warm.cube")));
  ASSERT_EQ(source.queued.size(), 1u);
  EXPECT_EQ(source.queued.front().second.input_encoding, std::optional<std::string>{"acescc"});
  EXPECT_EQ(source.queued.front().second.output_encoding, std::optional<std::string>{"acescc"});
  ASSERT_EQ(source.ApplyQueued(), 1);
  EXPECT_EQ(source.Lmt(kGradeB)->Reference(),
            LutReference{LibraryLutReference{"general/warm.cube"}});
  EXPECT_EQ(source.Lmt(kGradeB)->InputEncoding(), "acescc");
  EXPECT_EQ(source.Lmt(kGradeB)->OutputEncoding(), "acescc");
  controller.reload();
  EXPECT_FALSE(controller.rememberEncodings()) << "applying the default does not remember it";
  EXPECT_FALSE(service.RememberedEncodings("library:general/warm.cube").has_value());
}

}  // namespace
}  // namespace alcedo::ui::test
