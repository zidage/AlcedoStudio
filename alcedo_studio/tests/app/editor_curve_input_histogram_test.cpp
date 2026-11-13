//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_curve_input_histogram_test.cpp
/// @brief Tone Curve TC3 (issue #888): the curve input histogram request through the production
///        session facade.
///
/// ShowCurveInputHistogram makes every editor render request carry a CurveInputHistogramRequest
/// (checked on the requests that the recording scheduler receives) and routes one Quality render;
/// Hide stops it without a render. Before Show, no request carries one, so no render counts a
/// histogram.

#include <memory>
#include <optional>
#include <vector>

#include "editor_mask_coverage_preview_test_support.hpp"

namespace alcedo::ui {
namespace {

using namespace coverage_preview_test;

/// Sink that the requests carry; this test never renders, so it receives nothing.
class UnusedHistogramSink final : public ICurveInputHistogramSink {
 public:
  void PublishCurveInputHistogram(CurveInputHistogram /*histogram*/) override {}
};

class EditorCurveInputHistogramTest : public EditorMaskCoveragePreviewTest {
 protected:
  auto Show(const NodeId& grade_id, std::shared_ptr<ICurveInputHistogramSink> sink)
      -> EditorSessionResult {
    auto result = service_->ShowCurveInputHistogram(grade_id, std::move(sink));
    Drain();
    return result;
  }
  void Hide() {
    (void)service_->HideCurveInputHistogram();
    Drain();
  }
  /// Route one Interactive frame and return the histogram request of the newest scheduled request.
  auto RequestOfNextFrame() -> std::optional<CurveInputHistogramRequest> {
    const auto before = scheduler_->scheduled.size();
    (void)service_->RequestViewChange(EditorRenderReason::ScopeRefresh, std::nullopt);
    CompleteFrames();
    EXPECT_GT(scheduler_->scheduled.size(), before) << "no frame was routed";
    return scheduler_->scheduled.empty()
               ? std::nullopt
               : scheduler_->scheduled.back().intent.curve_input_histogram;
  }

  std::shared_ptr<ICurveInputHistogramSink> sink_ = std::make_shared<UnusedHistogramSink>();
};

TEST_F(EditorCurveInputHistogramTest, NoFrameCarriesAHistogramRequestBeforeShow) {
  OpenInteractive();
  ASSERT_FALSE(scheduler_->scheduled.empty());
  for (std::size_t i = 0; i < scheduler_->scheduled.size(); ++i) {
    EXPECT_FALSE(scheduler_->scheduled[i].intent.curve_input_histogram.has_value())
        << "frame " << i;
  }
  EXPECT_FALSE(RequestOfNextFrame().has_value());
}

TEST_F(EditorCurveInputHistogramTest, ShowRoutesOneQualityRenderAndEveryFrameCarriesItUntilHidden) {
  OpenInteractive();
  const auto frames       = scheduler_->scheduled.size();
  const auto request      = CurveInputHistogramRequest{GradeId(), sink_};

  const auto shown_result = Show(request.grade_id, request.sink);
  EXPECT_EQ(shown_result.kind, EditorSessionResultKind::Accepted);
  // One Quality render of the whole frame counts the new request at once.
  ASSERT_EQ(scheduler_->scheduled.size(), frames + 1);
  const auto& shown = scheduler_->scheduled.back().intent;
  EXPECT_EQ(shown.reason, EditorRenderReason::CurveInputHistogramRequested);
  EXPECT_EQ(shown.quality, EditorRenderQuality::Quality);
  EXPECT_EQ(shown.curve_input_histogram, request);
  EXPECT_FALSE(ScopeUpdateAllowedForReason(shown.reason));
  CompleteFrames();

  // Later frames carry it too, so an Exposure drag updates the histogram.
  EXPECT_EQ(RequestOfNextFrame(), request);

  // Hide routes no render, and later frames carry no request.
  const auto before_hide = scheduler_->scheduled.size();
  Hide();
  EXPECT_EQ(scheduler_->scheduled.size(), before_hide);
  EXPECT_FALSE(RequestOfNextFrame().has_value());
}

TEST_F(EditorCurveInputHistogramTest, ShowOfTheSameRequestRoutesNoSecondRender) {
  OpenInteractive();
  (void)Show(GradeId(), sink_);
  CompleteFrames();
  const auto frames = scheduler_->scheduled.size();

  (void)Show(GradeId(), sink_);
  EXPECT_EQ(scheduler_->scheduled.size(), frames);

  // Another receiver is another request: one Quality render counts it.
  (void)Show(GradeId(), std::make_shared<UnusedHistogramSink>());
  EXPECT_EQ(scheduler_->scheduled.size(), frames + 1);
}

TEST_F(EditorCurveInputHistogramTest, ShowWithoutAGradeOrAReceiverIsRefused) {
  OpenInteractive();
  const auto frames = scheduler_->scheduled.size();
  EXPECT_EQ(Show(NodeId{}, sink_).kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(Show(GradeId(), nullptr).kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(scheduler_->scheduled.size(), frames);
  EXPECT_FALSE(RequestOfNextFrame().has_value());
}

TEST_F(EditorCurveInputHistogramTest, ShowBeforeAnImageIsOpenAppliesToTheFramesOfTheImage) {
  // No image is open yet: the id of the primary Grade of the default document.
  const auto request = CurveInputHistogramRequest{NodeId{"grade.primary"}, sink_};
  EXPECT_EQ(Show(request.grade_id, request.sink).kind, EditorSessionResultKind::Accepted);
  EXPECT_TRUE(scheduler_->scheduled.empty());

  OpenInteractive();
  ASSERT_EQ(GradeId(), request.grade_id);
  ASSERT_FALSE(scheduler_->scheduled.empty());
  EXPECT_EQ(scheduler_->scheduled.back().intent.curve_input_histogram, request);
}

}  // namespace
}  // namespace alcedo::ui
