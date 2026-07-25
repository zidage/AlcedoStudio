//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace alcedo::ui {
namespace {

void UpdateDerived(CdlWheelState& wheel, bool add_unity, bool invert_delta) {
  wheel.disc_position_ = color_wheel::ClampDiscPoint(wheel.disc_position_);
  wheel.strength_      = std::clamp(wheel.strength_, 0.0f, color_wheel::kStrengthDefault);
  const auto  delta    = color_wheel::DiscToCdlDelta(wheel.disc_position_, wheel.strength_);
  const float base     = add_unity ? 1.0f : 0.0f;
  const float sign     = invert_delta ? -1.0f : 1.0f;
  wheel.color_offset_  = {base + sign * delta[0], base + sign * delta[1], base + sign * delta[2]};
}

auto WheelJson(const CdlWheelState& wheel) -> QJsonObject {
  QJsonObject disc;
  disc.insert(QStringLiteral("x"), static_cast<double>(wheel.disc_position_.x()));
  disc.insert(QStringLiteral("y"), static_cast<double>(wheel.disc_position_.y()));
  QJsonObject color_offset;
  color_offset.insert(QStringLiteral("x"), wheel.color_offset_[0]);
  color_offset.insert(QStringLiteral("y"), wheel.color_offset_[1]);
  color_offset.insert(QStringLiteral("z"), wheel.color_offset_[2]);
  QJsonObject out;
  out.insert(QStringLiteral("disc"), disc);
  out.insert(QStringLiteral("strength"), wheel.strength_);
  out.insert(QStringLiteral("color_offset"), color_offset);
  out.insert(QStringLiteral("luminance_offset"),
             std::clamp(wheel.master_offset_, -1.0f, 1.0f));
  return out;
}

}  // namespace

EditorCdlTrackballModel::EditorCdlTrackballModel(QObject* parent)
    : EditorAdjustmentModelBase(parent) {
  setFieldKey(QStringLiteral("color_wheel"));
  setLabel(QStringLiteral("Color Wheels"));
  recomputeDerived(WheelId::Lift);
  recomputeDerived(WheelId::Gamma);
  recomputeDerived(WheelId::Gain);
}

auto EditorCdlTrackballModel::liftMasterUi() const -> int {
  return color_wheel::CdlMasterToSliderUi(lift_.master_offset_);
}

auto EditorCdlTrackballModel::liftDeltaText() const -> QString {
  return FormatWheelDeltaText(lift_, false);
}

auto EditorCdlTrackballModel::gammaMasterUi() const -> int {
  // Gamma master slider is inverted in the legacy panel for ergonomic direction.
  return color_wheel::CdlMasterToSliderUi(-gamma_.master_offset_);
}

auto EditorCdlTrackballModel::gammaDeltaText() const -> QString {
  return FormatWheelDeltaText(gamma_, true);
}

auto EditorCdlTrackballModel::gainMasterUi() const -> int {
  return color_wheel::CdlMasterToSliderUi(gain_.master_offset_);
}

auto EditorCdlTrackballModel::gainDeltaText() const -> QString {
  return FormatWheelDeltaText(gain_, true);
}

void EditorCdlTrackballModel::setWheelDisc(const QString& wheel, double x, double y) {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return;
  }
  applyDisc(*id, x, y);
}

void EditorCdlTrackballModel::setWheelMaster(const QString& wheel, double master) {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return;
  }
  auto& state = wheelState(*id);
  const float next = std::clamp(static_cast<float>(master), -1.0f, 1.0f);
  if (std::abs(next - state.master_offset_) < 1e-6f) {
    return;
  }
  state.master_offset_ = next;
  recomputeDerived(*id);
  emitWheel(*id);
}

void EditorCdlTrackballModel::setWheelMasterUi(const QString& wheel, int ui_value) {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return;
  }
  applyMasterUi(*id, ui_value);
}

void EditorCdlTrackballModel::beginDiscDrag(const QString& wheel) {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return;
  }
  dragActive_     = true;
  draggingDisc_   = true;
  draggingMaster_ = false;
  dragWheel_      = *id;
  emit dragActiveChanged();
}

void EditorCdlTrackballModel::updateDiscDrag(const QString& wheel, double x, double y) {
  if (!dragActive_ || !draggingDisc_) {
    return;
  }
  const auto id = ParseWheelId(wheel);
  if (!id.has_value() || *id != dragWheel_) {
    return;
  }
  applyDisc(*id, x, y);
  submitInteractive();
}

void EditorCdlTrackballModel::finishDiscDrag() {
  if (!dragActive_ || !draggingDisc_) {
    return;
  }
  dragActive_     = false;
  draggingDisc_   = false;
  emit dragActiveChanged();
  submitSettled();
  emit settledCommitted();
}

void EditorCdlTrackballModel::beginMasterDrag(const QString& wheel) {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return;
  }
  dragActive_     = true;
  draggingMaster_ = true;
  draggingDisc_   = false;
  dragWheel_      = *id;
  emit dragActiveChanged();
}

void EditorCdlTrackballModel::updateMasterDragUi(const QString& wheel, int ui_value) {
  if (!dragActive_ || !draggingMaster_) {
    return;
  }
  const auto id = ParseWheelId(wheel);
  if (!id.has_value() || *id != dragWheel_) {
    return;
  }
  applyMasterUi(*id, ui_value);
  submitInteractive();
}

void EditorCdlTrackballModel::finishMasterDrag() {
  if (!dragActive_ || !draggingMaster_) {
    return;
  }
  dragActive_     = false;
  draggingMaster_ = false;
  emit dragActiveChanged();
  submitSettled();
  emit settledCommitted();
}

void EditorCdlTrackballModel::resetWheel(const QString& wheel) {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return;
  }
  CdlWheelState defaults =
      (*id == WheelId::Lift) ? DefaultLiftWheelState() : DefaultGammaGainWheelState();
  auto& state = wheelState(*id);
  state       = defaults;
  recomputeDerived(*id);
  emitWheel(*id);
  submitSettled();
  emit settledCommitted();
}

void EditorCdlTrackballModel::resetAll() {
  lift_  = DefaultLiftWheelState();
  gamma_ = DefaultGammaGainWheelState();
  gain_  = DefaultGammaGainWheelState();
  recomputeDerived(WheelId::Lift);
  recomputeDerived(WheelId::Gamma);
  recomputeDerived(WheelId::Gain);
  emit liftChanged();
  emit gammaChanged();
  emit gainChanged();
  submitSettled();
  emit settledCommitted();
}

auto EditorCdlTrackballModel::paramsJson() const -> QString { return buildParamsJson(); }

auto EditorCdlTrackballModel::wheelDeltaText(const QString& wheel) const -> QString {
  const auto id = ParseWheelId(wheel);
  if (!id.has_value()) {
    return {};
  }
  switch (*id) {
    case WheelId::Lift:
      return FormatWheelDeltaText(lift_, false);
    case WheelId::Gamma:
      return FormatWheelDeltaText(gamma_, true);
    case WheelId::Gain:
      return FormatWheelDeltaText(gain_, true);
  }
  return {};
}

void EditorCdlTrackballModel::setWheels(const CdlWheelState& lift, const CdlWheelState& gamma,
                                        const CdlWheelState& gain) {
  lift_  = lift;
  gamma_ = gamma;
  gain_  = gain;
  recomputeDerived(WheelId::Lift);
  recomputeDerived(WheelId::Gamma);
  recomputeDerived(WheelId::Gain);
  emit liftChanged();
  emit gammaChanged();
  emit gainChanged();
}

auto EditorCdlTrackballModel::ParseWheelId(const QString& wheel) -> std::optional<WheelId> {
  if (wheel.compare(QStringLiteral("lift"), Qt::CaseInsensitive) == 0) {
    return WheelId::Lift;
  }
  if (wheel.compare(QStringLiteral("gamma"), Qt::CaseInsensitive) == 0) {
    return WheelId::Gamma;
  }
  if (wheel.compare(QStringLiteral("gain"), Qt::CaseInsensitive) == 0) {
    return WheelId::Gain;
  }
  return std::nullopt;
}

auto EditorCdlTrackballModel::wheelState(WheelId id) -> CdlWheelState& {
  switch (id) {
    case WheelId::Lift:
      return lift_;
    case WheelId::Gamma:
      return gamma_;
    case WheelId::Gain:
      return gain_;
  }
  return lift_;
}

auto EditorCdlTrackballModel::wheelState(WheelId id) const -> const CdlWheelState& {
  switch (id) {
    case WheelId::Lift:
      return lift_;
    case WheelId::Gamma:
      return gamma_;
    case WheelId::Gain:
      return gain_;
  }
  return lift_;
}

void EditorCdlTrackballModel::recomputeDerived(WheelId id) {
  UpdateDerived(wheelState(id), addUnity(id), invertDelta(id));
}

void EditorCdlTrackballModel::emitWheel(WheelId id) {
  switch (id) {
    case WheelId::Lift:
      emit liftChanged();
      break;
    case WheelId::Gamma:
      emit gammaChanged();
      break;
    case WheelId::Gain:
      emit gainChanged();
      break;
  }
}

void EditorCdlTrackballModel::applyDisc(WheelId id, double x, double y) {
  auto&       state = wheelState(id);
  const auto  next  = color_wheel::ClampDiscPoint(QPointF(x, y));
  if (std::abs(next.x() - state.disc_position_.x()) < 1e-6 &&
      std::abs(next.y() - state.disc_position_.y()) < 1e-6) {
    return;
  }
  state.disc_position_ = next;
  recomputeDerived(id);
  emitWheel(id);
}

void EditorCdlTrackballModel::applyMasterUi(WheelId id, int ui_value) {
  auto&       state = wheelState(id);
  const float sign  = invertDelta(id) ? -1.0f : 1.0f;
  const float next  = color_wheel::CdlSliderUiToMaster(ui_value) * sign;
  if (std::abs(next - state.master_offset_) < 1e-6f) {
    return;
  }
  state.master_offset_ = next;
  recomputeDerived(id);
  emitWheel(id);
}

void EditorCdlTrackballModel::submitInteractive() { submitNow(buildParamsJson(), false); }

void EditorCdlTrackballModel::submitSettled() { submitNow(buildParamsJson(), true); }

auto EditorCdlTrackballModel::buildParamsJson() const -> QString {
  CdlWheelState lift  = lift_;
  CdlWheelState gamma = gamma_;
  CdlWheelState gain  = gain_;
  UpdateDerived(lift, false, false);
  UpdateDerived(gamma, true, true);
  UpdateDerived(gain, true, false);

  QJsonObject color_wheel;
  color_wheel.insert(QStringLiteral("lift"), WheelJson(lift));
  color_wheel.insert(QStringLiteral("gamma"), WheelJson(gamma));
  color_wheel.insert(QStringLiteral("gain"), WheelJson(gain));
  QJsonObject root;
  root.insert(QStringLiteral("color_wheel"), color_wheel);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

auto EditorCdlTrackballModel::addUnity(WheelId id) -> bool {
  return id != WheelId::Lift;
}

auto EditorCdlTrackballModel::invertDelta(WheelId id) -> bool {
  return id == WheelId::Gamma;
}

}  // namespace alcedo::ui
