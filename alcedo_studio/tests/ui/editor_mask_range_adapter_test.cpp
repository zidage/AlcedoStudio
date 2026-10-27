//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_mask_range_adapter_test.cpp
/// @brief Mask ranges MR4: the Luminance Range and Hue Range bars of EditorMaskCreationAdapter
///        through the production EditorSessionController and session facade over the real
///        history port.
///
/// A range drag of the target Mask writes live values and settles as one history commit; a
/// reset writes `null`; the range coverage is shown during a drag and while "Show selection" is
/// on, and an adjustment input hides it. During the AI Mask edit of a stored Mask that Mask is
/// the target; a new candidate has none. The Hue bar fill shows the hue that the kernels test.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QString>
#include <QVariantMap>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_mask_coverage_preview_test_support.hpp"
#include "app/editor_mask_creation_controller.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_range_display.hpp"
#include "edit/runtime/oklab_contrast_reference.hpp"
#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {
namespace {

using namespace coverage_preview_test;

auto ToQString(const NodeId& id) -> QString {
  return QString::fromStdString(std::string{id.Value()});
}
auto ToQString(const MaskId& id) -> QString {
  return QString::fromStdString(std::string{id.Value()});
}

class EditorMaskRangeAdapterTest : public EditorMaskCoveragePreviewTest {
 protected:
  void SetUp() override {
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("EditorMaskRangeAdapterTest"));
    EditorMaskCoveragePreviewTest::SetUp();
    controller_ = std::make_unique<EditorSessionController>(service_);
    adapter_    = controller_->mask_creation();
  }

  void TearDown() override {
    controller_.reset();
    EditorMaskCoveragePreviewTest::TearDown();
  }

  /// Run the owner work, the frames, and the queued notifications, then read the owner state.
  void Settle() {
    CompleteFrames();
    QCoreApplication::processEvents();
    adapter_->SyncFromSession();
  }

  void OpenThroughController() {
    controller_->Open(kElementId, kImageId);
    Settle();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
    ASSERT_TRUE(controller_->can_edit());
  }

  static auto Sample(float x, float y) -> MaskCreationSample {
    return MaskCreationSample{{x, y}, {x * 640.0f, y * 480.0f}, true};
  }

  /// Create one Radial Mask with a creation drag through the session; returns its id.
  auto CreateRadialMask() -> MaskId {
    const MaskPointerIdentity pointer{3, 1, 9};
    EditorMaskCreationCommand begin_mode;
    begin_mode.kind        = EditorMaskCreationCommandKind::BeginCreation;
    begin_mode.source_kind = MaskSourceKind::Radial;
    begin_mode.node_id     = GradeId();
    EditorMaskCreationCommand press;
    press.kind        = EditorMaskCreationCommandKind::BeginInput;
    press.source_kind = MaskSourceKind::Radial;
    press.sample      = Sample(0.40f, 0.40f);
    press.identity    = pointer;
    EditorMaskCreationCommand drag;
    drag.kind     = EditorMaskCreationCommandKind::Append;
    drag.sample   = Sample(0.60f, 0.55f);
    drag.identity = pointer;
    EditorMaskCreationCommand finish;
    finish.kind     = EditorMaskCreationCommandKind::Finish;
    finish.identity = pointer;
    EditorMaskCreationCommand end_mode;
    end_mode.kind    = EditorMaskCreationCommandKind::FinishMode;
    end_mode.node_id = GradeId();
    for (const auto& command : {begin_mode, press, drag, finish, end_mode}) {
      EXPECT_EQ(service_->EnqueueMaskCreation(command).kind, EditorSessionResultKind::Accepted);
    }
    Settle();
    const auto masks = Working()->Document().PrimaryGrade()->Masks();
    EXPECT_FALSE(masks.empty());
    return masks.empty() ? MaskId{} : masks[masks.size() - 1].id;
  }

  void Select(const MaskId& mask_id) {
    adapter_->selectMask(ToQString(GradeId()), ToQString(mask_id));
    Settle();
  }

  auto LiveMask(const MaskId& mask_id) -> const MaskModel* {
    working_ = Working();
    return working_->Document().PrimaryGrade()->FindMask(mask_id);
  }

  auto Probe(const MaskId& mask_id) -> MaskCoverageProbe { return {GradeId(), mask_id}; }

  /// Let the key or wheel step settle interval elapse (Qt timers run), then settle the owner.
  void WaitForStepSettle(int interval_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < interval_ms * 3) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    Settle();
  }

  std::unique_ptr<EditorSessionController>     controller_;
  EditorMaskCreationAdapter*                   adapter_ = nullptr;
  std::shared_ptr<const PipelineGraphSnapshot> working_;
};

TEST_F(EditorMaskRangeAdapterTest, RangeDragCommitsOnceAndShowsTheCoverageOnlyDuringTheDrag) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  ASSERT_TRUE(adapter_->mask_range_available());
  EXPECT_DOUBLE_EQ(adapter_->mask_luminance_low(), 0.0);
  EXPECT_DOUBLE_EQ(adapter_->mask_luminance_high(), 1.0);
  const auto commits = Graph()->CommitCount();

  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.2, 0.6);
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.25, 0.7);
  Settle();
  EXPECT_EQ(Preview().range_probe, Probe(mask_id)) << "the coverage is shown during the drag";
  EXPECT_EQ(Graph()->CommitCount(), commits) << "a drag step writes no commit";
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->luminance_range->low, 0.25f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->luminance_range->high, 0.7f);
  EXPECT_DOUBLE_EQ(adapter_->mask_luminance_low(), 0.25);

  adapter_->finishRangeEdit(QStringLiteral("luminance"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
  EXPECT_FALSE(Preview().range_probe.has_value()) << "the drag coverage ends at release";
  EXPECT_FLOAT_EQ(static_cast<float>(adapter_->mask_luminance_low()), 0.25f);
  EXPECT_FLOAT_EQ(static_cast<float>(adapter_->mask_luminance_high()), 0.7f);

  // A Hue Range that wraps through 0 degrees.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->updateRangeEdit(QStringLiteral("hue"), 300.0, 40.0);
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 2);
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 300.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_end, 40.0f);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_start(), 300.0);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_end(), 40.0);

  // A press and release without a move commits nothing.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 2);
}

TEST_F(EditorMaskRangeAdapterTest, HandlesKeepTheMinimumWidthAndBothEndsWriteTheFullRange) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);

  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.5, 0.5);
  adapter_->finishRangeEdit(QStringLiteral("luminance"));
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  const auto& luminance = *LiveMask(mask_id)->luminance_range;
  EXPECT_GE(luminance.high - luminance.low, kLuminanceRangeMinimumWidth);

  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->updateRangeEdit(QStringLiteral("hue"), 359.9, 0.1);
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_GE(ColorRangeWidthDegrees(*LiveMask(mask_id)->color_range),
            kColorRangeMinimumWidthDegrees);

  // Both handles at the ends write the full range: stored as null, one commit.
  const auto commits = Graph()->CommitCount();
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.0, 1.0);
  adapter_->finishRangeEdit(QStringLiteral("luminance"));
  Settle();
  EXPECT_FALSE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
}

TEST_F(EditorMaskRangeAdapterTest, PressAndMoveBackWithoutAChangeCommitsNothing) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.3, 0.6);
  adapter_->finishRangeEdit(QStringLiteral("luminance"));
  Settle();
  const auto commits = Graph()->CommitCount();

  // A press reports no value (the bar moves by the cursor delta), and a drag that ends on the
  // press values writes the stored range again: no commit, no change.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.35, 0.6);
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.3, 0.6);
  adapter_->finishRangeEdit(QStringLiteral("luminance"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits);
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->luminance_range->low, 0.3f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->luminance_range->high, 0.6f);
}

TEST_F(EditorMaskRangeAdapterTest, KeyStepBurstMovesOneHandleAndCommitsOnce) {
  constexpr int kInterval = 60;
  adapter_->SetRangeStepSettleIntervalMs(kInterval);
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  const auto commits = Graph()->CommitCount();

  // Four fine steps of the low handle and one coarse step of the high handle: one burst.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(
        adapter_->stepRangeHandle(QStringLiteral("luminance"), QStringLiteral("first"), 1, false));
  }
  ASSERT_TRUE(
      adapter_->stepRangeHandle(QStringLiteral("luminance"), QStringLiteral("second"), -1, true));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits) << "steps of an open burst write no commit";
  EXPECT_EQ(Preview().range_probe, Probe(mask_id)) << "the coverage is shown during the burst";
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_NEAR(LiveMask(mask_id)->luminance_range->low, 0.02f, 1e-6f);
  EXPECT_NEAR(LiveMask(mask_id)->luminance_range->high, 0.95f, 1e-6f);

  WaitForStepSettle(kInterval);
  EXPECT_EQ(Graph()->CommitCount(), commits + 1) << "one burst, one commit";
  EXPECT_FALSE(Preview().range_probe.has_value()) << "the burst coverage ends when it settles";
  EXPECT_NEAR(adapter_->mask_luminance_low(), 0.02, 1e-6);
  EXPECT_NEAR(adapter_->mask_luminance_high(), 0.95, 1e-6);

  // A wheel step is the same operation. A step of the low handle into the high handle stops at
  // the minimum handle distance; a step at the end of the bar does nothing.
  ASSERT_TRUE(
      adapter_->stepRangeHandle(QStringLiteral("luminance"), QStringLiteral("first"), 500, true));
  EXPECT_FALSE(
      adapter_->stepRangeHandle(QStringLiteral("luminance"), QStringLiteral("first"), 1, false));
  WaitForStepSettle(kInterval);
  EXPECT_EQ(Graph()->CommitCount(), commits + 2);
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_GE(LiveMask(mask_id)->luminance_range->high - LiveMask(mask_id)->luminance_range->low,
            kLuminanceRangeMinimumWidth);

  // One Undo removes the whole burst.
  controller_->Undo();
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_NEAR(LiveMask(mask_id)->luminance_range->low, 0.02f, 1e-6f);
}

TEST_F(EditorMaskRangeAdapterTest, HueStepsKeepHandlesOnTheBarAndADragSettlesTheBurstFirst) {
  constexpr int kInterval = 60;
  adapter_->SetRangeStepSettleIntervalMs(kInterval);
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  const auto commits = Graph()->CommitCount();

  // The start handle at 0 does not wrap to the far end; the end handle steps down by 0.5 degree.
  EXPECT_FALSE(
      adapter_->stepRangeHandle(QStringLiteral("hue"), QStringLiteral("first"), -1, false));
  ASSERT_TRUE(
      adapter_->stepRangeHandle(QStringLiteral("hue"), QStringLiteral("second"), -1, false));
  ASSERT_TRUE(adapter_->stepRangeHandle(QStringLiteral("hue"), QStringLiteral("first"), 2, true));
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 10.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_end, 359.5f);
  EXPECT_EQ(Graph()->CommitCount(), commits);

  // A pointer drag during the burst: the burst commits first, then the drag commits.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
  adapter_->updateRangeEdit(QStringLiteral("hue"), 20.0, 300.0);
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  WaitForStepSettle(kInterval);
  EXPECT_EQ(Graph()->CommitCount(), commits + 2);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 20.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_end, 300.0f);

  // A step of a pointer-dragged bar is refused; a reset during a burst settles the burst first.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  EXPECT_FALSE(adapter_->stepRangeHandle(QStringLiteral("hue"), QStringLiteral("first"), 1, false));
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  ASSERT_TRUE(adapter_->stepRangeHandle(QStringLiteral("hue"), QStringLiteral("second"), 1, false));
  adapter_->resetRange(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 4);
  EXPECT_FALSE(LiveMask(mask_id)->color_range.has_value());
}

TEST_F(EditorMaskRangeAdapterTest, TypedRangeValuesAreClampedAndCommitOnce) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  const auto commits = Graph()->CommitCount();

  // Luminance High typed as 60 (UI L x 100 = 0.6), then Low typed above it: clamped to the
  // minimum handle distance below High. Each typed value is one commit.
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("second"), 0.6));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("first"), 0.9));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 2);
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->luminance_range->high, 0.6f);
  EXPECT_NEAR(LiveMask(mask_id)->luminance_range->low, 0.588f, 1e-5f);
  EXPECT_FALSE(Preview().range_probe.has_value()) << "the typed value settled at once";

  // Hue Start typed above End wraps through 0; Start 360 is 0.
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("second"), 40.0));
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("first"), 300.0));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 3) << "values typed before Enter share a commit";
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 300.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_end, 40.0f);
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("first"), 39.9));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_GE(ColorRangeWidthDegrees(*LiveMask(mask_id)->color_range),
            kColorRangeMinimumWidthDegrees);

  // An unchanged value writes nothing.
  EXPECT_FALSE(
      adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("second"), 0.6));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 4);
}

TEST_F(EditorMaskRangeAdapterTest, SoftnessAndSaturationThresholdBelongToARestrictedRange) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  const auto commits = Graph()->CommitCount();

  // A full range has no Softness or threshold to set.
  EXPECT_FALSE(
      adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("softness"), 0.1));
  EXPECT_FALSE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("minChroma"), 0.1));
  EXPECT_FALSE(
      adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("minChroma"), 0.1));
  EXPECT_DOUBLE_EQ(adapter_->mask_luminance_softness(), LuminanceRangeModel{}.softness);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_min_chroma(), kColorRangeDefaultMinChroma);

  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->updateRangeEdit(QStringLiteral("hue"), 100.0, 200.0);
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  ASSERT_EQ(Graph()->CommitCount(), commits + 1);

  // A Softness slider drag is one range edit: live values, one commit at release, handles kept.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("softness"), 30.0));
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("softness"), 45.0));
  Settle();
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->softness, 45.0f);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_softness(), 45.0);
  EXPECT_EQ(Preview().range_probe, Probe(mask_id)) << "the slider drag shows the coverage";
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 2);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 100.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_end, 200.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->softness, 45.0f);

  // The saturation threshold: clamped to its bounds, stored, and kept by a later handle drag.
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("minChroma"), 0.5));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 3);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->min_chroma, kColorRangeMaximumMinChroma);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_min_chroma(), kColorRangeMaximumMinChroma);
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("minChroma"), 0.08));
  adapter_->commitRangeEdits();
  Settle();
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->min_chroma, 0.08f);
  EXPECT_FLOAT_EQ(MaskModelToJson(*LiveMask(mask_id))["color_range"]["min_chroma"].get<float>(),
                  0.08f);
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->updateRangeEdit(QStringLiteral("hue"), 120.0, 220.0);
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->min_chroma, 0.08f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->softness, 45.0f);

  // Reset to full writes null; the next restriction starts at the default values.
  adapter_->resetRange(QStringLiteral("hue"));
  Settle();
  EXPECT_FALSE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_softness(), ColorRangeModel{}.softness);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_min_chroma(), kColorRangeDefaultMinChroma);
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("hue"), QStringLiteral("first"), 10.0));
  adapter_->commitRangeEdits();
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_EQ(*LiveMask(mask_id)->color_range, (ColorRangeModel{10.0f, 360.0f, 12.0f}));

  // Luminance Softness slider bounds: 0.5 to 50 in L x 100.
  ASSERT_TRUE(adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("second"), 0.8));
  ASSERT_TRUE(
      adapter_->editRangeValue(QStringLiteral("luminance"), QStringLiteral("softness"), 0.0));
  adapter_->commitRangeEdits();
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->luminance_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->luminance_range->softness, 0.005f);
  const auto limits = EditorMaskCreationAdapter::range_parameter_limits();
  EXPECT_FLOAT_EQ(limits.value(QStringLiteral("luminanceSoftnessMaximum")).toFloat(), 0.5f);
  EXPECT_FLOAT_EQ(limits.value(QStringLiteral("hueSoftnessMinimum")).toFloat(), 0.5f);
  EXPECT_FLOAT_EQ(limits.value(QStringLiteral("minChromaMinimum")).toFloat(), 0.015f);
}

TEST_F(EditorMaskRangeAdapterTest, ResetWritesNullWithOneCommit) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->updateRangeEdit(QStringLiteral("hue"), 100.0, 200.0);
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  const auto commits = Graph()->CommitCount();

  adapter_->resetRange(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
  ASSERT_NE(LiveMask(mask_id), nullptr);
  EXPECT_FALSE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_TRUE(MaskModelToJson(*LiveMask(mask_id)).at("color_range").is_null());
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_start(), 0.0);
  EXPECT_DOUBLE_EQ(adapter_->mask_hue_end(), 360.0);

  // A reset of a full range commits nothing.
  adapter_->resetRange(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);

  // Undo restores the range.
  controller_->Undo();
  Settle();
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 100.0f);
}

TEST_F(EditorMaskRangeAdapterTest, ShowSelectionKeepsTheCoverageUntilAnAdjustmentInput) {
  OpenThroughController();
  const auto mask_id = CreateRadialMask();
  Select(mask_id);

  adapter_->setRangeCoverageVisible(true);
  Settle();
  EXPECT_TRUE(adapter_->mask_range_coverage_visible());
  EXPECT_EQ(Preview().range_probe, Probe(mask_id));

  // A drag with the toggle on keeps the coverage after release.
  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  adapter_->updateRangeEdit(QStringLiteral("luminance"), 0.3, 0.8);
  adapter_->finishRangeEdit(QStringLiteral("luminance"));
  Settle();
  EXPECT_EQ(Preview().range_probe, Probe(mask_id));

  // An adjustment input hides the coverage and turns the toggle off (master plan 10.4).
  ASSERT_TRUE(controller_->submitPatch(QStringLiteral("exposure"),
                                       QStringLiteral("{\"exposure\":0.5}"), true));
  Settle();
  EXPECT_FALSE(adapter_->mask_range_coverage_visible());
  EXPECT_FALSE(Preview().range_probe.has_value());

  // The toggle off hides the coverage.
  adapter_->setRangeCoverageVisible(true);
  Settle();
  ASSERT_EQ(Preview().range_probe, Probe(mask_id));
  adapter_->setRangeCoverageVisible(false);
  Settle();
  EXPECT_FALSE(Preview().range_probe.has_value());
}

TEST_F(EditorMaskRangeAdapterTest, AiMaskEditTargetsItsStoredMaskAndANewCandidateHasNone) {
  OpenThroughController();
  const auto mask_id = CommitNewAiMask({{0.2f, 0.2f}, {0.4f, 0.4f}});
  Settle();

  // A new candidate is not a Mask yet (D11): no range target.
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  adapter_->SetAiMaskEditRangeTarget(true, GradeId(), std::nullopt);
  Settle();
  EXPECT_FALSE(adapter_->mask_range_available());
  EXPECT_FALSE(adapter_->beginRangeEdit(QStringLiteral("luminance")));
  (void)service_->CancelAiMaskEdit();
  adapter_->SetAiMaskEditRangeTarget(false, {}, std::nullopt);
  Settle();

  // The AI Mask edit of a stored Mask: that Mask is the target, its drag commits once, and the
  // range coverage covers the candidate during the drag.
  const auto opened = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted);
  adapter_->SetAiMaskEditRangeTarget(true, GradeId(), mask_id);
  Settle();
  ASSERT_FALSE(controller_->can_edit()) << "the AI Mask edit holds the document writes";
  ASSERT_TRUE(adapter_->mask_range_available());
  const auto commits = Graph()->CommitCount();

  ASSERT_TRUE(adapter_->beginRangeEdit(QStringLiteral("hue")));
  adapter_->updateRangeEdit(QStringLiteral("hue"), 100.0, 200.0);
  Settle();
  EXPECT_EQ(Preview().source, EditorMaskCoveragePreviewSource::RangeProbe);
  EXPECT_EQ(Preview().range_probe, Probe(mask_id));
  adapter_->finishRangeEdit(QStringLiteral("hue"));
  Settle();
  EXPECT_EQ(Graph()->CommitCount(), commits + 1);
  ASSERT_TRUE(LiveMask(mask_id)->color_range.has_value());
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_start, 100.0f);
  EXPECT_FLOAT_EQ(LiveMask(mask_id)->color_range->hue_end, 200.0f);
  EXPECT_FALSE(Preview().range_probe.has_value());
  EXPECT_TRUE(service_->ai_mask_edit_open());

  // The end of the edit gives the target back to the selection (none here).
  (void)service_->CancelAiMaskEdit();
  adapter_->SetAiMaskEditRangeTarget(false, {}, std::nullopt);
  Settle();
  EXPECT_FALSE(adapter_->mask_range_available());
}

// The Hue bar shows, at each position, the hue that the MaskEvaluate kernels compute for that
// color: decode the displayed sRGB color, convert it to AP1, and take the OkLCh hue with the
// kernel transform (reference LinearAp1ToOkLab). Every stop is inside the sRGB gamut, so no
// clipping moves its hue.
TEST(EditorMaskRangeBarFill, HueBarStopsShowTheHueThatTheKernelTests) {
  const auto stops = EditorMaskCreationAdapter::hue_gradient_stops();
  ASSERT_EQ(stops.size(), 37);
  const auto decode = [](double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
  };
  for (const auto& entry : stops) {
    const auto   stop     = entry.toMap();
    const double position = stop.value(QStringLiteral("position")).toDouble();
    const auto   color    = stop.value(QStringLiteral("color")).value<QColor>();
    const double r = decode(color.redF()), g = decode(color.greenF()), b = decode(color.blueF());
    // Linear Rec.709 to AP1 (Bradford D65 to D60), the inverse of the bar transform.
    const oklab_contrast_reference::Rgb ap1{
        static_cast<float>(0.6131324224 * r + 0.3395380158 * g + 0.0474166960 * b),
        static_cast<float>(0.0701243808 * r + 0.9163940113 * g + 0.0134515240 * b),
        static_cast<float>(0.0205876575 * r + 0.1095745716 * g + 0.8697854040 * b)};
    const auto lab = oklab_contrast_reference::LinearAp1ToOkLab(ap1);
    double     hue = std::atan2(lab[2], lab[1]) * 180.0 / 3.14159265358979323846;
    if (hue < 0.0) {
      hue += 360.0;
    }
    const double expected = position * 360.0;
    const double distance = std::abs(std::remainder(hue - expected, 360.0));
    EXPECT_LT(distance, 0.5) << "position " << position;
    EXPECT_NEAR(lab[0], kMaskHueBarLightness, 0.01) << "position " << position;
    EXPECT_NEAR(std::hypot(lab[1], lab[2]), kMaskHueBarChroma, 0.01) << "position " << position;
  }
}

TEST(EditorMaskRangeBarFill, LuminanceBarStopsShowTheOkLabLightnessOfTheirPosition) {
  const auto stops = EditorMaskCreationAdapter::luminance_gradient_stops();
  ASSERT_EQ(stops.size(), 17);
  for (const auto& entry : stops) {
    const auto   stop     = entry.toMap();
    const double position = stop.value(QStringLiteral("position")).toDouble();
    const auto   color    = stop.value(QStringLiteral("color")).value<QColor>();
    const double encoded  = color.redF();
    const double linear =
        encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
    EXPECT_NEAR(std::cbrt(linear), position, 0.01) << "position " << position;
    EXPECT_DOUBLE_EQ(color.redF(), color.greenF());
    EXPECT_DOUBLE_EQ(color.redF(), color.blueF());
  }
}

}  // namespace
}  // namespace alcedo::ui
