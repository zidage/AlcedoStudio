//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_grade_white_balance_model.hpp"

#include <algorithm>
#include <cmath>

#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "ui/alcedo_main/editor_support/modules/color_temp.hpp"

namespace alcedo::ui {
namespace {

auto ClampTemperature(double kelvin) -> double {
  return std::clamp(kelvin, static_cast<double>(alcedo::kCat02TemperatureMin),
                    static_cast<double>(alcedo::kCat02TemperatureMax));
}

auto ClampTint(double value) -> double {
  return std::clamp(value, static_cast<double>(alcedo::kCat02TintMin),
                    static_cast<double>(alcedo::kCat02TintMax));
}

}  // namespace

EditorGradeWhiteBalanceModel::EditorGradeWhiteBalanceModel(QObject* parent)
    : EditorAdjustmentModelBase(parent),
      temperature_(alcedo::kCat02DefaultTemperature),
      tint_(alcedo::kCat02DefaultTint) {
  setFieldKey(QStringLiteral("grade_white_balance"));
  setLabel(QStringLiteral("Grade White Balance"));
}

void EditorGradeWhiteBalanceModel::setTemperature(double kelvin) {
  const double next = ClampTemperature(kelvin);
  if (std::abs(next - temperature_) < 1e-6) {
    return;
  }
  temperature_ = next;
  emit temperatureChanged();
}

void EditorGradeWhiteBalanceModel::setTint(double value) {
  const double next = ClampTint(value);
  if (std::abs(next - tint_) < 1e-6) {
    return;
  }
  tint_ = next;
  emit tintChanged();
}

auto EditorGradeWhiteBalanceModel::temperatureSliderPos() const -> int {
  return color_temp::CctToSliderPos(static_cast<float>(temperature_));
}

auto EditorGradeWhiteBalanceModel::defaultTemperature() const -> double {
  return alcedo::kCat02DefaultTemperature;
}

auto EditorGradeWhiteBalanceModel::defaultTint() const -> double {
  return alcedo::kCat02DefaultTint;
}

void EditorGradeWhiteBalanceModel::beginTemperatureDrag() {
  dragMoved_ = false;
  setDragActive(true, DragTarget::Temperature);
}

void EditorGradeWhiteBalanceModel::updateTemperatureSliderDrag(int pos) {
  if (!dragActive_ || dragTarget_ != DragTarget::Temperature) {
    return;
  }
  const double next = ClampTemperature(color_temp::SliderPosToCct(pos));
  if (std::abs(next - temperature_) < 1e-6) {
    return;
  }
  temperature_ = next;
  dragMoved_   = true;
  emit temperatureChanged();
  submitCurrent(false);
}

void EditorGradeWhiteBalanceModel::finishTemperatureDrag() {
  if (!dragActive_ || dragTarget_ != DragTarget::Temperature) {
    return;
  }
  setDragActive(false, DragTarget::None);
  // Empty press/release (including the first half of a double-click) must not settle;
  // resetTemperature() owns the double-click commit.
  if (!dragMoved_) {
    return;
  }
  dragMoved_ = false;
  submitCurrent(true);
  emit settledCommitted();
}

void EditorGradeWhiteBalanceModel::beginTintDrag() {
  dragMoved_ = false;
  setDragActive(true, DragTarget::Tint);
}

void EditorGradeWhiteBalanceModel::updateTintDrag(double value) {
  if (!dragActive_ || dragTarget_ != DragTarget::Tint) {
    return;
  }
  const double next = ClampTint(value);
  if (std::abs(next - tint_) < 1e-6) {
    return;
  }
  tint_      = next;
  dragMoved_ = true;
  emit tintChanged();
  submitCurrent(false);
}

void EditorGradeWhiteBalanceModel::finishTintDrag() {
  if (!dragActive_ || dragTarget_ != DragTarget::Tint) {
    return;
  }
  setDragActive(false, DragTarget::None);
  if (!dragMoved_) {
    return;
  }
  dragMoved_ = false;
  submitCurrent(true);
  emit settledCommitted();
}

void EditorGradeWhiteBalanceModel::resetTemperature() {
  dragMoved_ = false;
  setDragActive(false, DragTarget::None);
  if (temperature_ == static_cast<double>(alcedo::kCat02DefaultTemperature)) {
    return;
  }
  temperature_ = alcedo::kCat02DefaultTemperature;
  emit temperatureChanged();
  submitCurrent(true);
  emit settledCommitted();
}

void EditorGradeWhiteBalanceModel::resetTint() {
  dragMoved_ = false;
  setDragActive(false, DragTarget::None);
  if (tint_ == static_cast<double>(alcedo::kCat02DefaultTint)) {
    return;
  }
  tint_ = alcedo::kCat02DefaultTint;
  emit tintChanged();
  submitCurrent(true);
  emit settledCommitted();
}

void EditorGradeWhiteBalanceModel::loadFromSnapshot(const QVariantMap& snapshot) {
  // Interactive submits echo back while the pointer drag is still open; the drag owns the value.
  if (dragActive_) {
    return;
  }
  // The panel snapshot stores each field under its key, and the field map holds the value under
  // the same key again: snapshot.grade_white_balance.grade_white_balance.{temperature, tint}.
  QVariantMap white_balance = snapshot;
  while (!white_balance.contains(QStringLiteral("temperature"))) {
    const auto nested = white_balance.value(QStringLiteral("grade_white_balance"));
    if (!nested.canConvert<QVariantMap>()) {
      return;
    }
    white_balance = nested.toMap();
  }
  const auto temperature = white_balance.value(QStringLiteral("temperature"));
  const auto tint        = white_balance.value(QStringLiteral("tint"));
  if (temperature.isValid()) {
    setTemperature(temperature.toDouble());
  }
  if (tint.isValid()) {
    setTint(tint.toDouble());
  }
}

void EditorGradeWhiteBalanceModel::submitCurrent(bool settled) {
  alcedo::Cat02WhiteBalanceUpdate update;
  update.temperature = static_cast<float>(temperature_);
  update.tint        = static_cast<float>(tint_);
  submitNow(update, settled);
}

void EditorGradeWhiteBalanceModel::setDragActive(bool active, DragTarget target) {
  if (dragActive_ == active && dragTarget_ == target) {
    return;
  }
  dragActive_ = active;
  dragTarget_ = target;
  emit dragActiveChanged();
}

}  // namespace alcedo::ui
