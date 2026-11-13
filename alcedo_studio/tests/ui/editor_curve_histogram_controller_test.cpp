//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_curve_histogram_controller_test.cpp
/// @brief Tone Curve TC3 (issue #888): the QML-facing owner of the curve input histogram.
///
/// The owner gives a request only while active with a Grade, accepts render results of the
/// current image, session epoch, and Grade only, publishes them as ScopeHistogramRenderData, and
/// takes results from the render thread through its sink.

#include "ui/alcedo_main/album_backend/editor_curve_histogram_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace alcedo::ui::test {
namespace {

const NodeId kGrade{"grade.primary"};

/// Counts with a different count in each bin of each channel.
auto         MakeCounts() -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> counts(kCurveInputHistogramValueCount, 0U);
  for (std::uint32_t i = 0; i < counts.size(); ++i) {
    counts[i] = (i * 37U) % 101U;
  }
  return counts;
}

auto MakeResult(std::uint64_t image_identity, std::uint64_t session_epoch,
                const NodeId& grade_id = kGrade, std::vector<std::uint32_t> counts = MakeCounts())
    -> CurveInputHistogram {
  CurveInputHistogram result;
  result.grade_id       = grade_id;
  result.image_identity = image_identity;
  result.session_epoch  = session_epoch;
  result.sample_count   = 1000;
  result.counts         = std::move(counts);
  return result;
}

void DeliverQueued() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }

class EditorCurveHistogramControllerTest : public ::testing::Test {
 protected:
  /// Active owner of image 7, session epoch 3, Grade kGrade.
  void MakeActive() {
    controller_.SetImageIdentity(7, 3);
    controller_.set_grade_node_id(QStringLiteral("grade.primary"));
    controller_.set_active(true);
  }
  void Publish(CurveInputHistogram result) {
    controller_.sink()->PublishCurveInputHistogram(std::move(result));
    DeliverQueued();
  }

  EditorCurveHistogramController controller_;
};

TEST_F(EditorCurveHistogramControllerTest, RequestExistsOnlyWhileActiveWithAGrade) {
  QSignalSpy changed(&controller_, &EditorCurveHistogramController::RequestChanged);
  EXPECT_FALSE(controller_.request().has_value());

  controller_.set_grade_node_id(QStringLiteral("grade.primary"));
  EXPECT_FALSE(controller_.request().has_value()) << "a Grade without active";
  controller_.set_active(true);
  const auto request = controller_.request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->grade_id, kGrade);
  EXPECT_EQ(request->sink, controller_.sink());

  controller_.set_active(false);
  EXPECT_FALSE(controller_.request().has_value());
  controller_.set_active(true);
  controller_.set_grade_node_id(QString{});
  EXPECT_FALSE(controller_.request().has_value()) << "active without a Grade";
  EXPECT_EQ(changed.count(), 5);
}

TEST_F(EditorCurveHistogramControllerTest, ResultOfTheCurrentImageIsPublishedAsScopeRenderData) {
  MakeActive();
  QSignalSpy changed(&controller_, &EditorCurveHistogramController::HistogramChanged);
  const auto counts = MakeCounts();
  Publish(MakeResult(7, 3, kGrade, counts));

  ASSERT_EQ(changed.count(), 1);
  EXPECT_TRUE(controller_.has_histogram());
  EXPECT_EQ(controller_.bins(), 256);
  const auto expected = BuildScopeHistogramRenderData(counts, 256);
  EXPECT_EQ(controller_.histogram().rgb, expected.rgb);
  EXPECT_EQ(controller_.histogram().peak_cut, expected.peak_cut);
  EXPECT_EQ(controller_.histogram().highlight_clip_warning, expected.highlight_clip_warning);
}

TEST_F(EditorCurveHistogramControllerTest, ResultOfAnotherImageEpochOrGradeIsDropped) {
  MakeActive();
  QSignalSpy changed(&controller_, &EditorCurveHistogramController::HistogramChanged);
  Publish(MakeResult(8, 3));
  Publish(MakeResult(7, 4));
  Publish(MakeResult(7, 3, NodeId{"grade.other"}));
  EXPECT_EQ(changed.count(), 0);
  EXPECT_FALSE(controller_.has_histogram());

  // An inactive owner drops the result of the current image too.
  controller_.set_active(false);
  Publish(MakeResult(7, 3));
  EXPECT_FALSE(controller_.has_histogram());
}

TEST_F(EditorCurveHistogramControllerTest, ImageChangeAndAResultWithoutCountsClearTheHistogram) {
  MakeActive();
  Publish(MakeResult(7, 3));
  ASSERT_TRUE(controller_.has_histogram());

  // The render did not evaluate the Grade: no older histogram stays shown.
  Publish(MakeResult(7, 3, kGrade, {}));
  EXPECT_FALSE(controller_.has_histogram());

  Publish(MakeResult(7, 3));
  ASSERT_TRUE(controller_.has_histogram());
  const auto generation = controller_.histogram_generation();
  controller_.SetImageIdentity(9, 3);
  EXPECT_FALSE(controller_.has_histogram());
  EXPECT_GT(controller_.histogram_generation(), generation);
  // A late result of the earlier image does not come back.
  Publish(MakeResult(7, 3));
  EXPECT_FALSE(controller_.has_histogram());
}

TEST_F(EditorCurveHistogramControllerTest, ResultFromTheRenderThreadIsAcceptedOnTheOwnerThread) {
  MakeActive();
  auto        sink = controller_.sink();
  std::thread render_thread([sink] { sink->PublishCurveInputHistogram(MakeResult(7, 3)); });
  render_thread.join();
  EXPECT_FALSE(controller_.has_histogram()) << "accepted before the owner thread ran";
  DeliverQueued();
  EXPECT_TRUE(controller_.has_histogram());
}

TEST(EditorCurveHistogramControllerLifetimeTest, SinkDropsResultsAfterTheOwnerIsDestroyed) {
  std::shared_ptr<ICurveInputHistogramSink> sink;
  {
    EditorCurveHistogramController controller;
    controller.SetImageIdentity(7, 3);
    controller.set_grade_node_id(QStringLiteral("grade.primary"));
    controller.set_active(true);
    sink = controller.sink();
    // Queued before the destruction: Qt drops it with the owner.
    sink->PublishCurveInputHistogram(MakeResult(7, 3));
  }
  sink->PublishCurveInputHistogram(MakeResult(7, 3));
  DeliverQueued();
  SUCCEED();
}

}  // namespace
}  // namespace alcedo::ui::test
