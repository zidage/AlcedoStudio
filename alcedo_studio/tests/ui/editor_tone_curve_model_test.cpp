//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// EditorToneCurveModel: the four curves, the pointer drag through the plot handle layer, the
// point added on the curve, key moves, field edits, removal, reset, the channel change, and the
// number of interactive and settled writes of each (Tone Curve plan TC5, issue #890). No QML or
// GPU.

#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <memory>
#include <variant>
#include <vector>

#include "edit/operators/models/tone_curve_presets.hpp"
#include "support/recording_adjustment_submitter.hpp"
#include "ui/alcedo_main/editor_support/modules/curve.hpp"
#include "ui/alcedo_main/plot_pane/plot_handle_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_view_transform.hpp"

namespace alcedo::ui::test {
namespace {

constexpr int kRgb   = 0;
constexpr int kRed   = 1;
constexpr int kGreen = 2;
constexpr int kBlue  = 3;

auto          MakeCurveModel(RecordingSubmitter& sub) -> std::unique_ptr<EditorToneCurveModel> {
  auto m = std::make_unique<EditorToneCurveModel>();
  m->setSubmitter(&sub);
  return m;
}

auto CurveWrite(const alcedo::EditorParameterWrite* write) -> const alcedo::EditorCurveWrite* {
  return write == nullptr ? nullptr : std::get_if<alcedo::EditorCurveWrite>(write);
}

auto SCurve() -> std::vector<QPointF> {
  return {QPointF(0.0, 0.0), QPointF(0.25, 0.18), QPointF(0.5, 0.5), QPointF(0.75, 0.82),
          QPointF(1.0, 1.0)};
}

auto PointMap(double x, double y) -> QVariantMap {
  return QVariantMap{{QStringLiteral("x"), x}, {QStringLiteral("y"), y}};
}

}  // namespace

TEST(EditorToneCurveModelTest, DefaultIsLinearIdentityAndMatchesOperatorParamsShape) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);

  ASSERT_EQ(model->pointCount(), 2);
  for (int channel = kRgb; channel <= kBlue; ++channel) {
    EXPECT_TRUE(curve::CurveControlPointsEqual(model->channelControlPoints(channel),
                                               curve::DefaultCurveControlPoints()));
  }
  EXPECT_EQ(model->channelModified(), (QVariantList{false, false, false, false}));

  const auto doc = QJsonDocument::fromJson(model->paramsJson().toUtf8());
  ASSERT_TRUE(doc.isObject());
  const auto curve = doc.object().value(QStringLiteral("curve")).toObject();
  EXPECT_EQ(curve.value(QStringLiteral("size")).toInt(), 2);
  EXPECT_EQ(curve.value(QStringLiteral("points")).toArray().size(), 2);
}

TEST(EditorToneCurveModelTest, PointerDragSubmitsInteractiveThenOneSettled) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  QSignalSpy         settled(model.get(), &EditorToneCurveModel::settledCommitted);

  model->beginDrag(0);
  model->updateDrag(0.0, 0.25);
  model->updateDrag(0.0, 0.40);
  model->finishDrag();

  EXPECT_EQ(sub.interactiveCount(), 2);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(settled.count(), 1);
  EXPECT_EQ(sub.calls.back().fieldKey, QStringLiteral("curve"));

  const auto* write = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(write, nullptr);
  ASSERT_TRUE(write->points.has_value());
  EXPECT_FALSE(write->red.has_value()) << "the write leaves the other curves as they are";
  EXPECT_FALSE(write->green.has_value());
  EXPECT_FALSE(write->blue.has_value());
  EXPECT_NEAR(write->points->front().y, 0.40, 1e-6);
}

TEST(EditorToneCurveModelTest, HandleDragToAPanePositionGivesExactlyThatDataPosition) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.0), QPointF(0.4, 0.3), QPointF(1.0, 1.0)});

  // A zoomed pane of 300 x 300 item pixels; one item pixel is about 0.0017 on the curve axis.
  PlotViewTransform view(PlotDataRect{}, PlotViewTransform::kDefaultMaximumZoom);
  view.SetItemRect(QRectF(10.0, 10.0, 300.0, 300.0));
  ASSERT_TRUE(view.ZoomAt(QPointF(0.4, 0.4), 2.0));

  PlotHandleLayer handles;
  handles.set_points(model->points());
  QObject::connect(&handles, &PlotHandleLayer::pressed, model.get(),
                   [&](int index, QPointF, int) { model->beginDrag(index); });
  QObject::connect(&handles, &PlotHandleLayer::moved, model.get(),
                   [&](int, QPointF position, int modifiers) {
                     model->updateDrag(position.x(), position.y(), modifiers);
                   });
  QObject::connect(&handles, &PlotHandleLayer::released, model.get(),
                   [&](int, QPointF, int) { model->finishDrag(); });

  // The press is 3 px left of and 2 px below the center of the handle.
  const QPointF press_offset(-3.0, 2.0);
  // The loaded points are float values, as the document stores them.
  const QPointF start = model->controlPoints()[1];
  const QPointF press = view.ToItem(start) + press_offset;
  ASSERT_TRUE(handles.PointerPress(view, {press, view.ToData(press), Qt::LeftButton, {}}));
  EXPECT_EQ(model->selectedIndex(), 1);

  const QPointF pointer(170.0, 130.0);
  handles.PointerMove(view, {pointer, view.ToData(pointer), Qt::NoButton, {}});

  // The handle center keeps its offset to the pointer: it is at the pointer minus the press
  // offset, and the point is exactly the data position of that pane position.
  const QPointF expected = view.ToData(pointer - press_offset);
  const QPointF point    = model->controlPoints()[1];
  EXPECT_DOUBLE_EQ(point.x(), expected.x());
  EXPECT_DOUBLE_EQ(point.y(), expected.y());
  EXPECT_NEAR(view.ToItem(point).x(), (pointer - press_offset).x(), 1e-9);
  EXPECT_NEAR(view.ToItem(point).y(), (pointer - press_offset).y(), 1e-9);

  handles.PointerRelease(view, {pointer, view.ToData(pointer), Qt::LeftButton, {}});
  EXPECT_EQ(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorToneCurveModelTest, PointAddedOnTheCurveKeepsTheCurveValuesAt33Inputs) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints(SCurve());

  std::vector<double> before;
  for (int i = 0; i <= 32; ++i) {
    before.push_back(model->evaluate(kRgb, i / 32.0));
  }
  const double on_curve = model->evaluate(kRgb, 0.6);

  const int    index    = model->insertPointOnCurve(0.6);
  ASSERT_EQ(index, 3);
  EXPECT_EQ(model->pointCount(), 6);
  EXPECT_EQ(model->selectedIndex(), 3);
  EXPECT_TRUE(model->dragActive());
  EXPECT_DOUBLE_EQ(model->controlPoints()[3].x(), 0.6);
  EXPECT_DOUBLE_EQ(model->controlPoints()[3].y(), on_curve) << "the point is on the curve";
  EXPECT_TRUE(sub.calls.empty()) << "the point on the curve writes at release";

  // The new point solves new slopes, so the curve moves a little between its neighbors. The
  // tolerance is 0.003 on the curve axis (0.05 stop).
  double largest = 0.0;
  for (int i = 0; i <= 32; ++i) {
    largest = std::max(largest, std::abs(model->evaluate(kRgb, i / 32.0) - before[i]));
  }
  EXPECT_LE(largest, 0.003);

  model->finishDrag();
  EXPECT_EQ(sub.settledCount(), 1) << "an added point is one settled entry";
  const auto* write = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(write, nullptr);
  ASSERT_TRUE(write->points.has_value());
  EXPECT_EQ(write->points->size(), 6U);
}

TEST(EditorToneCurveModelTest, PointNearAnotherPointOrAtSixteenPointsIsNotAdded) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints(SCurve());
  EXPECT_EQ(model->insertPointOnCurve(0.2505), -1);
  EXPECT_EQ(model->insertPointOnCurve(0.0005), -1);
  EXPECT_FALSE(model->dragActive());

  std::vector<QPointF> sixteen;
  for (int i = 0; i < 16; ++i) {
    sixteen.emplace_back(i / 15.0, i / 15.0);
  }
  model->setControlPoints(sixteen);
  ASSERT_EQ(model->pointCount(), 16);
  EXPECT_EQ(model->insertPointOnCurve(0.5), -1);
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorToneCurveModelTest, ShiftDragMovesOnlyTheAxisWithTheLargerMotion) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.0), QPointF(0.4, 0.3), QPointF(1.0, 1.0)});

  // The press position: the loaded float values of (0.4, 0.3).
  const QPointF start = model->controlPoints()[1];

  model->beginDrag(1);
  model->updateDrag(0.45, 0.32, Qt::ShiftModifier);
  EXPECT_DOUBLE_EQ(model->controlPoints()[1].x(), 0.45);
  EXPECT_DOUBLE_EQ(model->controlPoints()[1].y(), start.y());
  model->updateDrag(0.42, 0.5, Qt::ShiftModifier);
  EXPECT_DOUBLE_EQ(model->controlPoints()[1].x(), start.x());
  EXPECT_DOUBLE_EQ(model->controlPoints()[1].y(), 0.5);
  model->updateDrag(0.42, 0.5, Qt::NoModifier);
  EXPECT_DOUBLE_EQ(model->controlPoints()[1].x(), 0.42);
  model->finishDrag();
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorToneCurveModelTest, ArrowKeysMoveTheSelectedPointAndSettleOnceAfterTheSequence) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setKeySettleIntervalMs(10);
  model->setControlPoints({QPointF(0.0, 0.0), QPointF(0.4, 0.3), QPointF(1.0, 1.0)});
  QSignalSpy settled(model.get(), &EditorToneCurveModel::settledCommitted);

  EXPECT_FALSE(model->handleKey(Qt::Key_Up, Qt::NoModifier)) << "no selected point";
  model->setSelectedIndex(1);
  EXPECT_TRUE(model->handleKey(Qt::Key_Up, Qt::NoModifier));
  EXPECT_TRUE(model->handleKey(Qt::Key_Up, Qt::NoModifier));
  EXPECT_TRUE(model->handleKey(Qt::Key_Right, Qt::NoModifier));
  EXPECT_TRUE(model->handleKey(Qt::Key_Down, Qt::ShiftModifier));
  EXPECT_EQ(sub.interactiveCount(), 4);
  EXPECT_EQ(sub.settledCount(), 0);
  EXPECT_TRUE(model->hasPendingKeyMoves());

  ASSERT_TRUE(settled.wait(1000));
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_FALSE(model->hasPendingKeyMoves());
  EXPECT_NEAR(model->selectedX(), 0.401, 1e-6);
  EXPECT_NEAR(model->selectedY(), 0.292, 1e-6);
  const auto* write = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(write, nullptr);
  ASSERT_TRUE(write->points.has_value());
  EXPECT_NEAR((*write->points)[1].y, 0.292, 1e-6);
}

TEST(EditorToneCurveModelTest, FieldEditsWriteOneSettledEntryEach) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.0), QPointF(0.4, 0.3), QPointF(1.0, 1.0)});
  EXPECT_FALSE(model->setSelectedXValue(0.45)) << "no selected point";

  model->setSelectedIndex(1);
  EXPECT_TRUE(model->setSelectedXValue(0.45));
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_TRUE(model->setSelectedYValue(0.6));
  EXPECT_EQ(sub.settledCount(), 2);
  EXPECT_FALSE(model->setSelectedYValue(0.6)) << "the same value writes nothing";
  EXPECT_EQ(sub.settledCount(), 2);
  EXPECT_EQ(sub.interactiveCount(), 0);
  EXPECT_DOUBLE_EQ(model->selectedX(), 0.45);
  EXPECT_DOUBLE_EQ(model->selectedY(), 0.6);

  // An x past the next point stops 0.001 before it.
  EXPECT_TRUE(model->setSelectedXValue(1.0));
  EXPECT_NEAR(model->selectedX(), 0.999, 1e-6);
  EXPECT_EQ(sub.settledCount(), 3);
}

TEST(EditorToneCurveModelTest, RemovalAndResetWriteOneSettledEntryForTheSelectedCurve) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setChannel(kBlue);
  model->setControlPoints(SCurve());
  ASSERT_TRUE(sub.calls.empty());

  model->setSelectedIndex(2);
  EXPECT_TRUE(model->removeSelected());
  EXPECT_EQ(model->pointCount(), 4);
  EXPECT_FALSE(model->hasSelection());
  EXPECT_EQ(sub.settledCount(), 1);
  const auto* removal = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(removal, nullptr);
  EXPECT_FALSE(removal->points.has_value());
  ASSERT_TRUE(removal->blue.has_value());
  EXPECT_EQ(removal->blue->size(), 4U);

  EXPECT_FALSE(model->removePoint(0)) << "an end point stays";
  EXPECT_TRUE(model->removePoint(1));
  EXPECT_EQ(sub.settledCount(), 2);

  model->reset();
  EXPECT_EQ(sub.settledCount(), 3);
  EXPECT_TRUE(
      curve::CurveControlPointsEqual(model->controlPoints(), curve::DefaultCurveControlPoints()));
  const auto* reset = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(reset, nullptr);
  ASSERT_TRUE(reset->blue.has_value());
  EXPECT_EQ(reset->blue->size(), 2U);
  model->reset();
  EXPECT_EQ(sub.settledCount(), 3) << "a reset of the identity writes nothing";
  EXPECT_EQ(sub.interactiveCount(), 0);
}

TEST(EditorToneCurveModelTest, ChannelChangeSettlesThePendingKeyMovesAndEditsTheNewCurve) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.0), QPointF(0.4, 0.3), QPointF(1.0, 1.0)});
  model->setSelectedIndex(1);
  model->handleKey(Qt::Key_Up, Qt::NoModifier);
  ASSERT_TRUE(model->hasPendingKeyMoves());

  QSignalSpy channel_changed(model.get(), &EditorToneCurveModel::channelChanged);
  model->setChannel(kRed);
  EXPECT_EQ(channel_changed.count(), 1);
  EXPECT_EQ(sub.settledCount(), 1) << "the key move sequence settles before the channel changes";
  const auto* rgb = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(rgb, nullptr);
  EXPECT_TRUE(rgb->points.has_value());
  EXPECT_FALSE(rgb->red.has_value());
  EXPECT_FALSE(model->hasSelection());
  EXPECT_EQ(model->pointCount(), 2) << "the red curve is the identity";

  model->beginDrag(1);
  model->updateDrag(1.0, 0.8);
  model->finishDrag();
  EXPECT_EQ(sub.settledCount(), 2);
  const auto* red = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(red, nullptr);
  EXPECT_FALSE(red->points.has_value());
  ASSERT_TRUE(red->red.has_value());
  EXPECT_NEAR(red->red->back().y, 0.8, 1e-6);
  EXPECT_EQ(model->channelModified(), (QVariantList{true, true, false, false}));
}

TEST(EditorToneCurveModelTest, PressWithoutMotionWritesNothing) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints(SCurve());

  model->beginDrag(2);
  model->finishDrag();
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_EQ(model->selectedIndex(), 2) << "the press selects the point";

  model->beginDrag(2);
  model->updateDrag(0.5, 0.5);
  model->finishDrag();
  EXPECT_TRUE(sub.calls.empty()) << "a move to the same position writes nothing";
}

TEST(EditorToneCurveModelTest, SnapshotLoadReadsTheFourCurvesWithoutAWrite) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setChannel(kGreen);

  QVariantMap curve;
  curve.insert(QStringLiteral("points"),
               QVariantList{PointMap(0.0, 0.0), PointMap(0.5, 0.6), PointMap(1.0, 1.0)});
  curve.insert(QStringLiteral("red"), QVariantList{PointMap(0.0, 0.1), PointMap(1.0, 1.0)});
  curve.insert(QStringLiteral("blue"),
               QVariantList{PointMap(0.0, 0.0), PointMap(0.3, 0.2), PointMap(1.0, 0.9)});
  model->loadFromSnapshotEntry(QVariantMap{{QStringLiteral("curve"), curve}});

  EXPECT_TRUE(sub.calls.empty());
  EXPECT_EQ(model->channel(), kGreen) << "a load keeps the selected channel";
  EXPECT_EQ(model->channelControlPoints(kRgb).size(), 3U);
  EXPECT_NEAR(model->channelControlPoints(kRed).front().y(), 0.1, 1e-6);
  EXPECT_EQ(model->channelControlPoints(kBlue).size(), 3U);
  EXPECT_TRUE(curve::CurveControlPointsEqual(model->channelControlPoints(kGreen),
                                             curve::DefaultCurveControlPoints()))
      << "a map without the green list loads the identity";
  EXPECT_EQ(model->channelModified(), (QVariantList{true, true, false, true}));
  EXPECT_EQ(model->blueLine().size(), curve::kCurveLineSteps + 1 + 1)
      << "the line passes through the interior point";
}

TEST(EditorToneCurveModelTest, CanEditFalseDropsSubmits) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  sub.canEditState         = false;

  model->beginDrag(1);
  model->updateDrag(1.0, 0.7);
  model->finishDrag();
  EXPECT_TRUE(sub.calls.empty());
}

TEST(EditorToneCurveModelTest, SetControlPointsIsLoadOnly) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.1), QPointF(1.0, 0.9)});
  EXPECT_TRUE(sub.calls.empty());
  EXPECT_NEAR(model->controlPoints().front().y(), 0.1, 1e-6);
}

TEST(EditorToneCurveModelTest, EndpointHorizontalMoveKeepsOrdering) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints(
      {QPointF(0.0, 0.0), QPointF(0.25, 0.25), QPointF(0.75, 0.75), QPointF(1.0, 1.0)});

  model->beginDrag(0);
  model->updateDrag(0.18, 0.22);
  model->updateDrag(0.6, 0.22);
  model->finishDrag();

  EXPECT_NEAR(model->controlPoints().front().x(), 0.249, 1e-9);
  for (size_t i = 1; i < model->controlPoints().size(); ++i) {
    EXPECT_GT(model->controlPoints()[i].x(), model->controlPoints()[i - 1].x());
  }
}

// Tone Curve presets (#891) -----------------------------------------------------------------

TEST(EditorToneCurveModelTest, PresetListIsTheMenuOrderOfTheEditLayer) {
  const auto presets = EditorToneCurveModel::presets();
  const auto data    = alcedo::ToneCurvePresets();
  ASSERT_EQ(static_cast<size_t>(presets.size()), data.size());
  for (qsizetype i = 0; i < presets.size(); ++i) {
    const auto entry = presets.at(i).toMap();
    EXPECT_EQ(entry.value(QStringLiteral("id")).toString().toStdString(), data[i].id);
    EXPECT_EQ(entry.value(QStringLiteral("name")).toString().toStdString(), data[i].display_name);
  }
}

TEST(EditorToneCurveModelTest, ApplyPresetWritesThePointsOfTheSelectedCurveInOneSettledEntry) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  QSignalSpy         settled(model.get(), &EditorToneCurveModel::settledCommitted);
  model->setChannel(kGreen);

  ASSERT_TRUE(model->applyPreset(QStringLiteral("kodak_2383")));
  EXPECT_EQ(sub.interactiveCount(), 0);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(settled.count(), 1);
  const auto* write = CurveWrite(sub.lastSettledWrite());
  ASSERT_NE(write, nullptr);
  EXPECT_FALSE(write->points.has_value()) << "only the selected curve";
  ASSERT_TRUE(write->green.has_value());
  EXPECT_EQ(*write->green, alcedo::FindToneCurvePreset("kodak_2383")->PointList());
  EXPECT_TRUE(curve::CurveControlPointsEqual(model->channelControlPoints(kRgb),
                                             curve::DefaultCurveControlPoints()));

  // The same preset again changes nothing and writes nothing.
  ASSERT_TRUE(model->applyPreset(QStringLiteral("kodak_2383")));
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_FALSE(model->applyPreset(QStringLiteral("no_such_preset")));
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorToneCurveModelTest, MenuNameFollowsThePointsPresetAfterApplyCustomAfterAnEdit) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  QSignalSpy         changed(model.get(), &EditorToneCurveModel::pointsChanged);
  EXPECT_EQ(model->presetId(), QStringLiteral("linear"));

  ASSERT_TRUE(model->applyPreset(QStringLiteral("film_like")));
  EXPECT_EQ(model->presetId(), QStringLiteral("film_like"));
  EXPECT_GE(changed.count(), 1);

  // A point edit: the points are no preset any more.
  model->setSelectedIndex(1);
  ASSERT_TRUE(model->handleKey(Qt::Key_Up, Qt::NoModifier));
  EXPECT_EQ(model->presetId(), QString{});
  model->settlePendingKeyMoves();
  EXPECT_EQ(model->presetId(), QString{});

  // Each channel shows the preset of its own curve.
  model->setChannel(kRed);
  EXPECT_EQ(model->presetId(), QStringLiteral("linear"));
  ASSERT_TRUE(model->applyPreset(QStringLiteral("fuji_velvia_50")));
  EXPECT_EQ(model->presetId(), QStringLiteral("fuji_velvia_50"));
  model->setChannel(kRgb);
  EXPECT_EQ(model->presetId(), QString{});

  // A document load of the preset points (float JSON values) shows the preset.
  QVariantList points;
  for (const auto& point : alcedo::FindToneCurvePreset("filmic")->points) {
    points.push_back(PointMap(static_cast<double>(point.x), static_cast<double>(point.y)));
  }
  model->loadFromSnapshotEntry(QVariantMap{{QStringLiteral("points"), points}});
  EXPECT_EQ(model->presetId(), QStringLiteral("filmic"));
}

}  // namespace alcedo::ui::test
