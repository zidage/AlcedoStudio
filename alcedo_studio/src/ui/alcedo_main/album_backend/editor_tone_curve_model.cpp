//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <algorithm>
#include <cmath>
#include <utility>

#include "edit/operators/models/tone_curve_presets.hpp"
#include "ui/alcedo_main/editor_support/modules/curve.hpp"

namespace alcedo::ui {
namespace {

// Two loads of the same document points differ only by the float rounding of the document.
constexpr float kLoadEqualTolerance = 1e-6f;

auto            VariantListToPoints(const QVariantList& list) -> std::vector<QPointF> {
  std::vector<QPointF> points;
  points.reserve(static_cast<size_t>(list.size()));
  for (const QVariant& v : list) {
    if (v.typeId() == QMetaType::QPointF || v.typeId() == QMetaType::QPoint) {
      points.push_back(v.toPointF());
      continue;
    }
    if (v.typeId() == QMetaType::QVariantList) {
      const QVariantList pair = v.toList();
      if (pair.size() >= 2) {
        points.emplace_back(pair.at(0).toDouble(), pair.at(1).toDouble());
      }
      continue;
    }
    const QVariantMap m = v.toMap();
    if (m.contains(QStringLiteral("x")) && m.contains(QStringLiteral("y"))) {
      points.emplace_back(m.value(QStringLiteral("x")).toDouble(),
                          m.value(QStringLiteral("y")).toDouble());
    }
  }
  return points;
}

auto ChannelKey(int channel) -> QString {
  return QString::fromUtf8(
      alcedo::CurveChannelKey(alcedo::kCurveChannels[static_cast<size_t>(channel)]));
}

auto IsIdentity(const std::vector<QPointF>& points) -> bool {
  return curve::CurveControlPointsEqual(points, curve::DefaultCurveControlPoints(),
                                        kLoadEqualTolerance);
}

auto Text(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// The panel points of @p preset: the float points of the preset as doubles.
auto PresetControlPoints(const alcedo::ToneCurvePreset& preset) -> std::vector<QPointF> {
  std::vector<QPointF> points;
  points.reserve(preset.points.size());
  for (const auto& point : preset.points) {
    points.emplace_back(point.x, point.y);
  }
  return points;
}

}  // namespace

EditorToneCurveModel::EditorToneCurveModel(QObject* parent)
    : EditorAdjustmentModelBase(parent), keySettleTimer_(new QTimer(this)) {
  setFieldKey(QStringLiteral("curve"));
  setLabel(QStringLiteral("Tone Curve"));
  for (int channel = 0; channel < kChannelCount; ++channel) {
    curves_[static_cast<size_t>(channel)] = curve::DefaultCurveControlPoints();
    RebuildLine(channel);
  }
  keySettleTimer_->setSingleShot(true);
  keySettleTimer_->setInterval(kDefaultKeySettleIntervalMs);
  connect(keySettleTimer_, &QTimer::timeout, this, &EditorToneCurveModel::settlePendingKeyMoves);
}

auto EditorToneCurveModel::maxControlPoints() -> int { return curve::kCurveMaxControlPoints; }

auto EditorToneCurveModel::axisGrey() -> double { return curve::kCurveAxisGrey; }

auto EditorToneCurveModel::axisStop() -> double { return curve::kCurveAxisStop; }

void EditorToneCurveModel::setChannel(int channel) {
  if (channel < 0 || channel >= kChannelCount || channel == channel_) {
    return;
  }
  settlePendingKeyMoves();
  if (dragActive_) {
    finishDrag();
  }
  SetSelectedIndexInternal(-1);
  channel_ = channel;
  emit channelChanged();
  emit pointsChanged();
  emit selectedPointChanged();
}

auto EditorToneCurveModel::points() const -> QList<QPointF> {
  const auto& selected = Selected();
  return QList<QPointF>(selected.begin(), selected.end());
}

void EditorToneCurveModel::setSelectedIndex(int index) {
  if (index < -1 || index >= pointCount()) {
    return;
  }
  if (index != selectedIndex_) {
    settlePendingKeyMoves();
  }
  SetSelectedIndexInternal(index);
}

auto EditorToneCurveModel::selectedX() const -> double {
  return hasSelection() ? Selected()[static_cast<size_t>(selectedIndex_)].x() : 0.0;
}

auto EditorToneCurveModel::selectedY() const -> double {
  return hasSelection() ? Selected()[static_cast<size_t>(selectedIndex_)].y() : 0.0;
}

auto EditorToneCurveModel::channelModified() const -> QVariantList {
  QVariantList modified;
  for (const auto& points : curves_) {
    modified.push_back(!IsIdentity(points));
  }
  return modified;
}

auto EditorToneCurveModel::presets() -> QVariantList {
  QVariantList list;
  for (const auto& preset : alcedo::ToneCurvePresets()) {
    list.push_back(QVariantMap{{QStringLiteral("id"), Text(preset.id)},
                               {QStringLiteral("name"), Text(preset.display_name)},
                               {QStringLiteral("group"), Text(preset.group)}});
  }
  return list;
}

auto EditorToneCurveModel::presetId() const -> QString {
  // The write converts the panel points to float, so the float values decide the match.
  std::vector<alcedo::CurvePoint> points;
  points.reserve(Selected().size());
  for (const auto& point : Selected()) {
    points.push_back({static_cast<float>(point.x()), static_cast<float>(point.y())});
  }
  const auto* preset = alcedo::MatchToneCurvePreset(points);
  return preset == nullptr ? QString{} : Text(preset->id);
}

auto EditorToneCurveModel::applyPreset(const QString& id) -> bool {
  const auto* preset = alcedo::FindToneCurvePreset(id.toStdString());
  if (preset == nullptr || dragActive_) {
    return false;
  }
  settlePendingKeyMoves();
  SetSelectedIndexInternal(-1);
  if (ReplaceChannelPoints(channel_, PresetControlPoints(*preset))) {
    CommitSettled();
  }
  return true;
}

auto EditorToneCurveModel::channelControlPoints(int channel) const -> const std::vector<QPointF>& {
  return curves_[static_cast<size_t>(std::clamp(channel, 0, kChannelCount - 1))];
}

void EditorToneCurveModel::setPoints(const QVariantList& points) {
  setControlPoints(VariantListToPoints(points));
}

void EditorToneCurveModel::loadFromSnapshotEntry(const QVariantMap& entry) {
  // A drag or a key move sequence owns the selected curve until its settled write.
  if (dragActive_ || keyMovesPending_) {
    return;
  }
  QVariantMap curve = entry;
  if (const QVariant nested = entry.value(QStringLiteral("curve"));
      nested.typeId() == QMetaType::QVariantMap) {
    curve = nested.toMap();
  }
  if (!curve.contains(QStringLiteral("points"))) {
    return;
  }
  for (int channel = 0; channel < kChannelCount; ++channel) {
    auto points = VariantListToPoints(curve.value(ChannelKey(channel)).toList());
    if (points.size() < 2) {
      // The projection omits nothing, but a map without a channel list means the identity.
      points = curve::DefaultCurveControlPoints();
    }
    ReplaceChannelPoints(channel, std::move(points));
  }
}

void EditorToneCurveModel::setControlPoints(const std::vector<QPointF>& points) {
  setChannelControlPoints(channel_, points);
}

void EditorToneCurveModel::setChannelControlPoints(int                         channel,
                                                   const std::vector<QPointF>& points) {
  if (channel < 0 || channel >= kChannelCount) {
    return;
  }
  ReplaceChannelPoints(channel, points);
  if (channel == channel_) {
    SetDragActive(false);
    SetSelectedIndexInternal(-1);
  }
}

auto EditorToneCurveModel::evaluate(int channel, double x) const -> double {
  if (channel < 0 || channel >= kChannelCount || !std::isfinite(x)) {
    return 0.0;
  }
  const auto spline = curve::SolveCurveSpline(curves_[static_cast<size_t>(channel)]);
  return tone_curve::EvaluateToneCurveSpline(spline, static_cast<float>(x));
}

void EditorToneCurveModel::beginDrag(int index) {
  if (index < 0 || index >= pointCount()) {
    return;
  }
  settlePendingKeyMoves();
  SetSelectedIndexInternal(index);
  dragOrigin_  = Selected()[static_cast<size_t>(index)];
  dragChanged_ = false;
  SetDragActive(true);
}

void EditorToneCurveModel::updateDrag(double x, double y, int modifiers) {
  if (!dragActive_ || !hasSelection()) {
    return;
  }
  if ((modifiers & Qt::ShiftModifier) != 0) {
    // One axis: the axis with the larger motion from the press position.
    if (std::abs(x - dragOrigin_.x()) >= std::abs(y - dragOrigin_.y())) {
      y = dragOrigin_.y();
    } else {
      x = dragOrigin_.x();
    }
  }
  if (!MoveSelectedCurvePoint(selectedIndex_, x, y)) {
    return;
  }
  dragChanged_ = true;
  SubmitSelected(false);
}

void EditorToneCurveModel::finishDrag() {
  if (!dragActive_) {
    return;
  }
  SetDragActive(false);
  if (std::exchange(dragChanged_, false)) {
    CommitSettled();
  }
}

auto EditorToneCurveModel::insertPointOnCurve(double x) -> int {
  if (dragActive_ || !std::isfinite(x) || pointCount() >= curve::kCurveMaxControlPoints) {
    return -1;
  }
  const auto& selected = Selected();
  if (x <= selected.front().x() + curve::kCurveMinPointSpacing ||
      x >= selected.back().x() - curve::kCurveMinPointSpacing) {
    return -1;
  }
  const auto next = std::upper_bound(selected.begin(), selected.end(), x,
                                     [](double value, const QPointF& p) { return value < p.x(); });
  if (x - std::prev(next)->x() < curve::kCurveMinPointSpacing ||
      next->x() - x < curve::kCurveMinPointSpacing) {
    return -1;
  }
  const int    index = static_cast<int>(std::distance(selected.begin(), next));
  const double y     = std::clamp(evaluate(channel_, x), 0.0, 1.0);
  settlePendingKeyMoves();
  // The indices after the new point move by one, so the selection moves to the new point.
  SetSelectedIndexInternal(-1);

  auto& points = curves_[static_cast<size_t>(channel_)];
  points.insert(points.begin() + index, QPointF(x, y));
  RebuildLine(channel_);
  emit pointsChanged();
  emit curvesChanged();
  SetSelectedIndexInternal(index);
  dragOrigin_  = QPointF(x, y);
  dragChanged_ = true;
  SetDragActive(true);
  return index;
}

void EditorToneCurveModel::nudgeSelected(double dx, double dy) {
  if (!hasSelection() || dragActive_) {
    return;
  }
  const QPointF point = Selected()[static_cast<size_t>(selectedIndex_)];
  if (!MoveSelectedCurvePoint(selectedIndex_, point.x() + dx, point.y() + dy)) {
    return;
  }
  keyMovesPending_ = true;
  SubmitSelected(false);
  keySettleTimer_->start();
}

auto EditorToneCurveModel::handleKey(int key, int modifiers) -> bool {
  if (!hasSelection()) {
    return false;
  }
  const double step = (modifiers & Qt::ShiftModifier) != 0 ? kKeyStepLarge : kKeyStep;
  switch (key) {
    case Qt::Key_Left:
      nudgeSelected(-step, 0.0);
      return true;
    case Qt::Key_Right:
      nudgeSelected(step, 0.0);
      return true;
    case Qt::Key_Up:
      nudgeSelected(0.0, step);
      return true;
    case Qt::Key_Down:
      nudgeSelected(0.0, -step);
      return true;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      removeSelected();
      return true;
    default:
      return false;
  }
}

auto EditorToneCurveModel::setSelectedXValue(double x) -> bool {
  if (!hasSelection() || dragActive_ || !std::isfinite(x)) {
    return false;
  }
  settlePendingKeyMoves();
  if (!MoveSelectedCurvePoint(selectedIndex_, x, selectedY())) {
    return false;
  }
  CommitSettled();
  return true;
}

auto EditorToneCurveModel::setSelectedYValue(double y) -> bool {
  if (!hasSelection() || dragActive_ || !std::isfinite(y)) {
    return false;
  }
  settlePendingKeyMoves();
  if (!MoveSelectedCurvePoint(selectedIndex_, selectedX(), y)) {
    return false;
  }
  CommitSettled();
  return true;
}

auto EditorToneCurveModel::removePoint(int index) -> bool {
  if (dragActive_ || index <= 0 || index + 1 >= pointCount()) {
    return false;
  }
  settlePendingKeyMoves();
  SetSelectedIndexInternal(-1);
  auto& points = curves_[static_cast<size_t>(channel_)];
  points.erase(points.begin() + index);
  RebuildLine(channel_);
  emit pointsChanged();
  emit curvesChanged();
  CommitSettled();
  return true;
}

auto EditorToneCurveModel::removeSelected() -> bool { return removePoint(selectedIndex_); }

void EditorToneCurveModel::reset() {
  if (dragActive_) {
    return;
  }
  settlePendingKeyMoves();
  SetSelectedIndexInternal(-1);
  if (!ReplaceChannelPoints(channel_, curve::DefaultCurveControlPoints())) {
    return;
  }
  CommitSettled();
}

void EditorToneCurveModel::settlePendingKeyMoves() {
  if (!keyMovesPending_) {
    return;
  }
  keyMovesPending_ = false;
  keySettleTimer_->stop();
  CommitSettled();
}

auto EditorToneCurveModel::paramsJson() const -> QString {
  // Match curve::CurveControlPointsToParams / pipeline ParamsForField(Curve).
  const auto normalized = curve::NormalizeCurveControlPoints(curves_[0]);
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

void EditorToneCurveModel::setKeySettleIntervalMs(int ms) {
  keySettleTimer_->setInterval(std::max(0, ms));
}

auto EditorToneCurveModel::ReplaceChannelPoints(int channel, std::vector<QPointF> points) -> bool {
  auto  normalized = curve::NormalizeCurveControlPoints(points);
  auto& current    = curves_[static_cast<size_t>(channel)];
  if (curve::CurveControlPointsEqual(normalized, current, kLoadEqualTolerance)) {
    return false;
  }
  current = std::move(normalized);
  RebuildLine(channel);
  if (channel == channel_) {
    SetSelectedIndexInternal(-1);
    emit pointsChanged();
  }
  emit curvesChanged();
  return true;
}

auto EditorToneCurveModel::MoveSelectedCurvePoint(int index, double x, double y) -> bool {
  auto& points = curves_[static_cast<size_t>(channel_)];
  if (index < 0 || index >= static_cast<int>(points.size()) || !std::isfinite(x) ||
      !std::isfinite(y)) {
    return false;
  }
  const double spacing = curve::kCurveMinPointSpacing;
  const int    last    = static_cast<int>(points.size()) - 1;
  const double minimum = index == 0 ? 0.0 : points[static_cast<size_t>(index - 1)].x() + spacing;
  const double maximum = index == last ? 1.0 : points[static_cast<size_t>(index + 1)].x() - spacing;
  const QPointF next(std::clamp(x, minimum, maximum), std::clamp(y, 0.0, 1.0));
  QPointF&      point = points[static_cast<size_t>(index)];
  if (next == point) {
    return false;
  }
  point = next;
  RebuildLine(channel_);
  emit pointsChanged();
  emit curvesChanged();
  if (index == selectedIndex_) {
    emit selectedPointChanged();
  }
  return true;
}

void EditorToneCurveModel::RebuildLine(int channel) {
  lines_[static_cast<size_t>(channel)] =
      curve::SampleCurveLine(curve::SolveCurveSpline(curves_[static_cast<size_t>(channel)]));
}

void EditorToneCurveModel::SetSelectedIndexInternal(int index) {
  if (selectedIndex_ == index) {
    return;
  }
  selectedIndex_ = index;
  emit selectedIndexChanged();
  emit selectedPointChanged();
}

void EditorToneCurveModel::SetDragActive(bool active) {
  if (dragActive_ == active) {
    return;
  }
  dragActive_ = active;
  emit dragActiveChanged();
}

void EditorToneCurveModel::SubmitSelected(bool settled) {
  submitNow(SelectedCurveWrite(), settled);
}

void EditorToneCurveModel::CommitSettled() {
  SubmitSelected(true);
  emit settledCommitted();
}

auto EditorToneCurveModel::SelectedCurveWrite() const -> alcedo::EditorCurveWrite {
  std::vector<alcedo::CurvePoint> points;
  points.reserve(Selected().size());
  for (const auto& point : Selected()) {
    points.push_back(
        alcedo::CurvePoint{static_cast<float>(point.x()), static_cast<float>(point.y())});
  }
  // Only the selected curve: the write leaves the other three curves as they are.
  alcedo::EditorCurveWrite write;
  write.Channel(alcedo::kCurveChannels[static_cast<size_t>(channel_)]) = std::move(points);
  return write;
}

}  // namespace alcedo::ui
