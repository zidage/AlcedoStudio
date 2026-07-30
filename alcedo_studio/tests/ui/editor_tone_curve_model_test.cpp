//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6B unit tests for EditorToneCurveModel: point ordering, insert/remove,
// pointer drag (interactive per move + one settled on release), reset, and the
// operator-shaped params JSON that matches curve::CurveControlPointsToParams /
// pipeline ParamsForField(Curve). No QML or GPU.

#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"
#include "ui/alcedo_main/editor_dialog/modules/curve.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace alcedo::ui::test {
namespace {

class RecordingSubmitter : public QObject, public IEditorAdjustmentSubmitter {
 public:
  struct Call {
    QString fieldKey;
    QString params;
    bool    settled;
  };
  std::vector<Call> calls;
  bool              canEditState = true;

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    if (!canEditState) {
      return false;
    }
    calls.push_back({fieldKey, paramsJson, settled});
    return true;
  }
  auto canEdit() const -> bool override { return canEditState; }

  auto settledCount() const -> int {
    return static_cast<int>(
        std::count_if(calls.begin(), calls.end(), [](const Call& c) { return c.settled; }));
  }
  auto interactiveCount() const -> int {
    return static_cast<int>(
        std::count_if(calls.begin(), calls.end(), [](const Call& c) { return !c.settled; }));
  }
  auto lastSettledParams() const -> QString {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      if (it->settled) {
        return it->params;
      }
    }
    return {};
  }
};

auto MakeCurveModel(RecordingSubmitter& sub) -> std::unique_ptr<EditorToneCurveModel> {
  auto m = std::make_unique<EditorToneCurveModel>();
  m->setSubmitter(&sub);
  return m;
}

auto ParsePoints(const QString& params) -> std::vector<QPointF> {
  const auto  doc  = QJsonDocument::fromJson(params.toUtf8());
  const auto  root = doc.object();
  const auto  curve = root.value(QStringLiteral("curve")).toObject();
  const auto  arr  = curve.value(QStringLiteral("points")).toArray();
  std::vector<QPointF> points;
  for (const auto& v : arr) {
    const auto o = v.toObject();
    points.emplace_back(o.value(QStringLiteral("x")).toDouble(),
                        o.value(QStringLiteral("y")).toDouble());
  }
  return points;
}

}  // namespace

TEST(EditorToneCurveModelTest, DefaultIsLinearIdentityAndMatchesOperatorParamsShape) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);

  ASSERT_EQ(model->pointCount(), 2);
  EXPECT_NEAR(model->controlPoints().front().x(), 0.0, 1e-6);
  EXPECT_NEAR(model->controlPoints().front().y(), 0.0, 1e-6);
  EXPECT_NEAR(model->controlPoints().back().x(), 1.0, 1e-6);
  EXPECT_NEAR(model->controlPoints().back().y(), 1.0, 1e-6);

  const QString json = model->paramsJson();
  const auto    doc  = QJsonDocument::fromJson(json.toUtf8());
  ASSERT_TRUE(doc.isObject());
  ASSERT_TRUE(doc.object().contains(QStringLiteral("curve")));
  const auto curve = doc.object().value(QStringLiteral("curve")).toObject();
  EXPECT_EQ(curve.value(QStringLiteral("size")).toInt(), 2);
  EXPECT_EQ(curve.value(QStringLiteral("points")).toArray().size(), 2);

  // Matches the nlohmann path used by the pipeline adapter.
  const auto expected = curve::CurveControlPointsToParams(model->controlPoints());
  EXPECT_EQ(expected["curve"]["size"].get<size_t>(), 2u);
}

TEST(EditorToneCurveModelTest, PointerDragSubmitsInteractiveThenOneSettled) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  QSignalSpy         settled(model.get(), &EditorToneCurveModel::settledCommitted);

  model->beginDrag(0);
  model->updateDrag(0.0, 0.25);
  model->updateDrag(0.0, 0.40);
  model->finishDrag();

  EXPECT_GE(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(settled.count(), 1);
  ASSERT_FALSE(sub.calls.empty());
  EXPECT_EQ(sub.calls.back().fieldKey, QStringLiteral("curve"));
  EXPECT_TRUE(sub.calls.back().settled);

  const auto points = ParsePoints(sub.lastSettledParams());
  ASSERT_GE(points.size(), 2u);
  EXPECT_NEAR(points.front().y(), 0.40, 1e-3);
}

TEST(EditorToneCurveModelTest, InsertInteriorPointThenDragCommitsOnce) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);

  const int idx = model->insertPoint(0.5, 0.35);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(model->pointCount(), 3);
  EXPECT_TRUE(model->dragActive());
  EXPECT_EQ(sub.interactiveCount(), 1);
  EXPECT_EQ(sub.settledCount(), 0);

  model->updateDrag(0.5, 0.55);
  model->finishDrag();
  EXPECT_EQ(sub.settledCount(), 1);

  const auto points = ParsePoints(sub.lastSettledParams());
  ASSERT_EQ(points.size(), 3u);
  EXPECT_NEAR(points[1].x(), 0.5, 1e-2);
  EXPECT_NEAR(points[1].y(), 0.55, 1e-2);
}

TEST(EditorToneCurveModelTest, RemoveInteriorPointCommitsSettled) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.0), QPointF(0.4, 0.5), QPointF(1.0, 1.0)});
  sub.calls.clear();

  EXPECT_TRUE(model->removePoint(1));
  EXPECT_EQ(model->pointCount(), 2);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_FALSE(model->removePoint(0));  // endpoints pinned
  EXPECT_EQ(sub.settledCount(), 1);
}

TEST(EditorToneCurveModelTest, ResetRestoresDefaultAndCommitsOnce) {
  RecordingSubmitter sub;
  auto               model = MakeCurveModel(sub);
  model->setControlPoints({QPointF(0.0, 0.2), QPointF(0.5, 0.5), QPointF(1.0, 0.9)});
  sub.calls.clear();

  model->reset();
  EXPECT_EQ(model->pointCount(), 2);
  EXPECT_TRUE(curve::CurveControlPointsEqual(model->controlPoints(),
                                              curve::DefaultCurveControlPoints()));
  EXPECT_EQ(sub.settledCount(), 1);
  // Second reset is a no-op (already default).
  model->reset();
  EXPECT_EQ(sub.settledCount(), 1);
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
  sub.calls.clear();

  model->beginDrag(0);
  model->updateDrag(0.18, 0.22);
  model->finishDrag();

  ASSERT_GE(model->controlPoints().size(), 2u);
  EXPECT_NEAR(model->controlPoints().front().x(), 0.18, 1e-3);
  EXPECT_NEAR(model->controlPoints().front().y(), 0.22, 1e-3);
  // Still strictly ordered by x.
  for (size_t i = 1; i < model->controlPoints().size(); ++i) {
    EXPECT_GT(model->controlPoints()[i].x(), model->controlPoints()[i - 1].x());
  }
}

}  // namespace alcedo::ui::test
