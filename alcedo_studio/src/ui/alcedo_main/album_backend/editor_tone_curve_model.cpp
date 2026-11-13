//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

#include "ui/alcedo_main/editor_support/modules/curve.hpp"

namespace alcedo::ui {
namespace {

auto PointsToVariantList(const std::vector<QPointF>& points) -> QVariantList {
  QVariantList out;
  out.reserve(static_cast<int>(points.size()));
  for (const auto& p : points) {
    QVariantMap m;
    m.insert(QStringLiteral("x"), p.x());
    m.insert(QStringLiteral("y"), p.y());
    out.push_back(m);
  }
  return out;
}

auto VariantListToPoints(const QVariantList& list) -> std::vector<QPointF> {
  std::vector<QPointF> points;
  points.reserve(static_cast<size_t>(list.size()));
  for (const QVariant& v : list) {
    if (v.canConvert<QPointF>()) {
      points.push_back(v.toPointF());
      continue;
    }
    if (v.typeId() == QMetaType::QVariantList || v.canConvert<QVariantList>()) {
      const QVariantList pair = v.toList();
      if (pair.size() >= 2) {
        points.emplace_back(pair.at(0).toDouble(), pair.at(1).toDouble());
        continue;
      }
    }
    const QVariantMap m = v.toMap();
    if (m.contains(QStringLiteral("x")) && m.contains(QStringLiteral("y"))) {
      points.emplace_back(m.value(QStringLiteral("x")).toDouble(),
                          m.value(QStringLiteral("y")).toDouble());
    }
  }
  return points;
}

auto PointsFromSnapshotEntry(const QVariantMap& entry) -> std::vector<QPointF> {
  // Snapshot key "curve" stores the full operator params:
  //   {"curve":{"size":N,"points":[{"x":…,"y":…},…]}}
  // Also accept the inner object directly.
  QVariantMap curve = entry;
  if (entry.contains(QStringLiteral("curve"))) {
    const QVariant nested = entry.value(QStringLiteral("curve"));
    if (nested.canConvert<QVariantMap>()) {
      curve = nested.toMap();
    }
  }
  const QVariant points_var = curve.value(QStringLiteral("points"));
  if (!points_var.isValid()) {
    return {};
  }
  return VariantListToPoints(points_var.toList());
}

auto FindClosestPointIndex(const std::vector<QPointF>& points, const QPointF& target) -> int {
  if (points.empty()) {
    return -1;
  }
  int   best_idx  = 0;
  qreal best_dist = std::abs(points[0].x() - target.x()) + std::abs(points[0].y() - target.y());
  for (int i = 1; i < static_cast<int>(points.size()); ++i) {
    const qreal dist = std::abs(points[static_cast<size_t>(i)].x() - target.x()) +
                       std::abs(points[static_cast<size_t>(i)].y() - target.y());
    if (dist < best_dist) {
      best_dist = dist;
      best_idx  = i;
    }
  }
  return best_idx;
}

}  // namespace

EditorToneCurveModel::EditorToneCurveModel(QObject* parent)
    : EditorAdjustmentModelBase(parent), points_(curve::DefaultCurveControlPoints()) {
  setFieldKey(QStringLiteral("curve"));
  setLabel(QStringLiteral("Tone Curve"));
}

auto EditorToneCurveModel::maxControlPoints() -> int { return curve::kCurveMaxControlPoints; }

auto EditorToneCurveModel::points() const -> QVariantList { return PointsToVariantList(points_); }

void EditorToneCurveModel::setPoints(const QVariantList& points) {
  setControlPoints(VariantListToPoints(points));
}

void EditorToneCurveModel::loadFromSnapshotEntry(const QVariantMap& entry) {
  if (dragActive_) {
    return;
  }
  const auto points = PointsFromSnapshotEntry(entry);
  if (points.size() < 2) {
    return;
  }
  setControlPoints(points);
}

void EditorToneCurveModel::setControlPoints(const std::vector<QPointF>& points) {
  applyNormalized(points);
  if (dragActive_) {
    dragActive_ = false;
    emit dragActiveChanged();
  }
  setActiveIndex(-1);
}

void EditorToneCurveModel::beginDrag(int index) {
  if (index < 0 || index >= static_cast<int>(points_.size())) {
    return;
  }
  dragActive_ = true;
  setActiveIndex(index);
  emit dragActiveChanged();
}

void EditorToneCurveModel::updateDrag(double x, double y) {
  if (!dragActive_ || activeIndex_ < 0) {
    return;
  }
  if (!moveActivePoint(x, y)) {
    return;
  }
  submitInteractive();
}

void EditorToneCurveModel::finishDrag() {
  if (!dragActive_) {
    return;
  }
  dragActive_ = false;
  emit dragActiveChanged();
  submitSettled();
  emit settledCommitted();
}

auto EditorToneCurveModel::insertPoint(double x, double y) -> int {
  if (static_cast<int>(points_.size()) >= curve::kCurveMaxControlPoints) {
    return -1;
  }
  if (points_.size() < 2) {
    return -1;
  }

  const float min_insert_x = static_cast<float>(points_.front().x()) + curve::kCurveMinPointSpacing;
  const float max_insert_x = static_cast<float>(points_.back().x()) - curve::kCurveMinPointSpacing;
  if (x <= min_insert_x || x >= max_insert_x) {
    return -1;
  }

  std::vector<QPointF> next = points_;
  next.emplace_back(curve::Clamp01(static_cast<float>(x)), curve::Clamp01(static_cast<float>(y)));
  const auto normalized = curve::NormalizeCurveControlPoints(next);
  if (normalized.size() <= points_.size()) {
    return -1;
  }

  const QPointF target(x, y);
  const int     idx = FindClosestPointIndex(normalized, target);
  points_           = normalized;
  dragActive_       = true;
  setActiveIndex(idx);
  emit pointsChanged();
  emit dragActiveChanged();
  submitInteractive();
  return idx;
}

auto EditorToneCurveModel::removePoint(int index) -> bool {
  if (index <= 0 || index + 1 >= static_cast<int>(points_.size())) {
    return false;
  }
  std::vector<QPointF> next = points_;
  next.erase(next.begin() + index);
  const auto normalized = curve::NormalizeCurveControlPoints(next);
  if (curve::CurveControlPointsEqual(normalized, points_)) {
    return false;
  }
  points_     = normalized;
  dragActive_ = false;
  setActiveIndex(-1);
  emit pointsChanged();
  emit dragActiveChanged();
  submitSettled();
  emit settledCommitted();
  return true;
}

void EditorToneCurveModel::reset() {
  const auto defaults = curve::DefaultCurveControlPoints();
  if (curve::CurveControlPointsEqual(defaults, points_) && !dragActive_) {
    return;
  }
  points_     = defaults;
  dragActive_ = false;
  setActiveIndex(-1);
  emit pointsChanged();
  emit dragActiveChanged();
  submitSettled();
  emit settledCommitted();
}

auto EditorToneCurveModel::paramsJson() const -> QString { return buildParamsJson(); }

void EditorToneCurveModel::applyNormalized(std::vector<QPointF> points) {
  const auto normalized = curve::NormalizeCurveControlPoints(std::move(points));
  if (curve::CurveControlPointsEqual(normalized, points_)) {
    return;
  }
  points_ = normalized;
  emit pointsChanged();
}

void EditorToneCurveModel::setActiveIndex(int index) {
  if (activeIndex_ == index) {
    return;
  }
  activeIndex_ = index;
  emit activeIndexChanged();
}

void EditorToneCurveModel::submitInteractive() {
  submitNow(currentCurveWrite(), false);
}

void EditorToneCurveModel::submitSettled() { submitNow(currentCurveWrite(), true); }

auto EditorToneCurveModel::currentCurveWrite() const -> alcedo::EditorCurveWrite {
  const auto              normalized = curve::NormalizeCurveControlPoints(points_);
  alcedo::EditorCurveWrite write;
  write.points.reserve(normalized.size());
  for (const auto& point : normalized) {
    write.points.push_back(
        alcedo::CurvePoint{static_cast<float>(point.x()), static_cast<float>(point.y())});
  }
  return write;
}

auto EditorToneCurveModel::buildParamsJson() const -> QString {
  // Match curve::CurveControlPointsToParams / pipeline ParamsForField(Curve).
  const auto normalized = curve::NormalizeCurveControlPoints(points_);
  QJsonArray pts;
  for (const auto& p : normalized) {
    QJsonObject pt;
    pt.insert(QStringLiteral("x"), p.x());
    pt.insert(QStringLiteral("y"), p.y());
    pts.append(pt);
  }
  QJsonObject curve_obj;
  curve_obj.insert(QStringLiteral("size"), static_cast<int>(normalized.size()));
  curve_obj.insert(QStringLiteral("points"), pts);
  QJsonObject root;
  root.insert(QStringLiteral("curve"), curve_obj);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

auto EditorToneCurveModel::moveActivePoint(double x, double y) -> bool {
  if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(points_.size())) {
    return false;
  }

  const int   last_idx = static_cast<int>(points_.size()) - 1;
  const float nx       = static_cast<float>(x);
  const float ny       = curve::Clamp01(static_cast<float>(y));
  QPointF     next     = points_[static_cast<size_t>(activeIndex_)];

  if (activeIndex_ == 0) {
    const float max_x = static_cast<float>(points_[1].x()) - curve::kCurveMinPointSpacing;
    next              = QPointF(std::clamp(nx, 0.0f, max_x), ny);
  } else if (activeIndex_ == last_idx) {
    const float min_x = static_cast<float>(points_[static_cast<size_t>(last_idx - 1)].x()) +
                        curve::kCurveMinPointSpacing;
    next = QPointF(std::clamp(nx, min_x, 1.0f), ny);
  } else {
    const float min_x = static_cast<float>(points_[static_cast<size_t>(activeIndex_ - 1)].x()) +
                        curve::kCurveMinPointSpacing;
    const float max_x = static_cast<float>(points_[static_cast<size_t>(activeIndex_ + 1)].x()) -
                        curve::kCurveMinPointSpacing;
    next = QPointF(std::clamp(nx, min_x, max_x), ny);
  }

  if (std::abs(next.x() - points_[static_cast<size_t>(activeIndex_)].x()) < 1e-9 &&
      std::abs(next.y() - points_[static_cast<size_t>(activeIndex_)].y()) < 1e-9) {
    return false;
  }

  points_[static_cast<size_t>(activeIndex_)] = next;
  emit pointsChanged();
  return true;
}

}  // namespace alcedo::ui
