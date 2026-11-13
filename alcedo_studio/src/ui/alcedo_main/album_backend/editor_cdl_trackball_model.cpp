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

constexpr std::array<EditorCdlTrackballModel::WheelId, 4> kAllWheels = {
    EditorCdlTrackballModel::WheelId::Lift, EditorCdlTrackballModel::WheelId::Gamma,
    EditorCdlTrackballModel::WheelId::Gain, EditorCdlTrackballModel::WheelId::Offset};

auto WheelKey(EditorCdlTrackballModel::WheelId id) -> QString {
  switch (id) {
    case EditorCdlTrackballModel::WheelId::Lift:
      return QStringLiteral("lift");
    case EditorCdlTrackballModel::WheelId::Gamma:
      return QStringLiteral("gamma");
    case EditorCdlTrackballModel::WheelId::Gain:
      return QStringLiteral("gain");
    case EditorCdlTrackballModel::WheelId::Offset:
      return QStringLiteral("offset");
  }
  return QStringLiteral("lift");
}

auto ClampMaster(double master) -> float {
  return std::clamp(static_cast<float>(master), -color_wheel::kMasterMaxStops,
                    color_wheel::kMasterMaxStops);
}

void UpdateDerived(CdlWheelState& wheel) {
  wheel.disc_position_ = color_wheel::ClampDiscPoint(wheel.disc_position_);
  wheel.strength_      = color_wheel::kDiscMaxStops;
  wheel.color_offset_  = color_wheel::DiscToChannelStops(wheel.disc_position_, wheel.strength_);
}

auto WheelWrite(const CdlWheelState& wheel) -> alcedo::ColorWheelControlUpdate {
  alcedo::ColorWheelControlUpdate update;
  update.disc = alcedo::Vec2f{static_cast<float>(wheel.disc_position_.x()),
                              static_cast<float>(wheel.disc_position_.y())};
  update.strength = wheel.strength_;
  update.color_offset =
      alcedo::Vec3f{wheel.color_offset_[0], wheel.color_offset_[1], wheel.color_offset_[2]};
  update.luminance_offset = wheel.master_offset_;
  return update;
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
  out.insert(QStringLiteral("luminance_offset"), wheel.master_offset_);
  return out;
}

}  // namespace

EditorCdlTrackballModel::EditorCdlTrackballModel(QObject* parent)
    : EditorAdjustmentModelBase(parent) {
  setFieldKey(QStringLiteral("color_wheel"));
  setLabel(QStringLiteral("Color Wheels"));
  for (const auto id : kAllWheels) {
    recomputeDerived(id);
  }
}

auto EditorCdlTrackballModel::liftDeltaText() const -> QString {
  return FormatWheelDeltaText(wheel(WheelId::Lift));
}

auto EditorCdlTrackballModel::gammaDeltaText() const -> QString {
  return FormatWheelDeltaText(wheel(WheelId::Gamma));
}

auto EditorCdlTrackballModel::gainDeltaText() const -> QString {
  return FormatWheelDeltaText(wheel(WheelId::Gain));
}

auto EditorCdlTrackballModel::offsetDeltaText() const -> QString {
  return FormatWheelDeltaText(wheel(WheelId::Offset));
}

auto EditorCdlTrackballModel::discPosition(const QString& wheel_key) const -> QPointF {
  const auto id = ParseWheelId(wheel_key);
  return id.has_value() ? wheel(*id).disc_position_ : QPointF(0.0, 0.0);
}

void EditorCdlTrackballModel::setWheelDisc(const QString& wheel_key, double x, double y) {
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value()) {
    return;
  }
  applyDisc(*id, x, y);
}

void EditorCdlTrackballModel::setWheelMaster(const QString& wheel_key, double master) {
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value()) {
    return;
  }
  applyMaster(*id, master);
}

void EditorCdlTrackballModel::beginDiscDrag(const QString& wheel_key) {
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value()) {
    return;
  }
  dragActive_     = true;
  draggingDisc_   = true;
  draggingMaster_ = false;
  dragMoved_      = false;
  dragWheel_      = *id;
  emit dragActiveChanged();
}

void EditorCdlTrackballModel::updateDiscDrag(const QString& wheel_key, double x, double y) {
  if (!dragActive_ || !draggingDisc_) {
    return;
  }
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value() || *id != dragWheel_) {
    return;
  }
  const auto next    = color_wheel::ClampDiscPoint(QPointF(x, y));
  const auto current = wheel(*id).disc_position_;
  if (std::abs(next.x() - current.x()) < 1e-6 && std::abs(next.y() - current.y()) < 1e-6) {
    return;
  }
  applyDisc(*id, x, y);
  dragMoved_ = true;
  submitWheels({*id}, false);
}

void EditorCdlTrackballModel::finishDiscDrag() {
  if (!dragActive_ || !draggingDisc_) {
    return;
  }
  const bool    moved = dragMoved_;
  const WheelId id    = dragWheel_;
  clearDrag();
  // Empty click / double-click press half: no settle. resetWheel owns reset.
  if (!moved) {
    return;
  }
  submitWheels({id}, true);
  emit settledCommitted();
}

void EditorCdlTrackballModel::beginMasterDrag(const QString& wheel_key) {
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value()) {
    return;
  }
  dragActive_     = true;
  draggingMaster_ = true;
  draggingDisc_   = false;
  dragMoved_      = false;
  dragWheel_      = *id;
  emit dragActiveChanged();
}

void EditorCdlTrackballModel::updateMasterDrag(const QString& wheel_key, double master) {
  if (!dragActive_ || !draggingMaster_) {
    return;
  }
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value() || *id != dragWheel_) {
    return;
  }
  if (std::abs(ClampMaster(master) - wheel(*id).master_offset_) < 1e-6f) {
    return;
  }
  applyMaster(*id, master);
  dragMoved_ = true;
  submitWheels({*id}, false);
}

void EditorCdlTrackballModel::finishMasterDrag() {
  if (!dragActive_ || !draggingMaster_) {
    return;
  }
  const bool    moved = dragMoved_;
  const WheelId id    = dragWheel_;
  clearDrag();
  if (!moved) {
    return;
  }
  submitWheels({id}, true);
  emit settledCommitted();
}

void EditorCdlTrackballModel::resetWheel(const QString& wheel_key) {
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value()) {
    return;
  }
  // Double-click arrives while a press/drag may still be open. Clear drag state
  // before submitting so finishDiscDrag cannot re-enter submit afterward.
  clearDrag();
  wheelState(*id) = CdlWheelState{};
  recomputeDerived(*id);
  emitWheel(*id);
  submitWheels({*id}, true);
  emit settledCommitted();
}

void EditorCdlTrackballModel::resetAll() {
  clearDrag();
  for (const auto id : kAllWheels) {
    wheelState(id) = CdlWheelState{};
    recomputeDerived(id);
    emitWheel(id);
  }
  submitWheels({WheelId::Lift, WheelId::Gamma, WheelId::Gain, WheelId::Offset}, true);
  emit settledCommitted();
}

auto EditorCdlTrackballModel::paramsJson() const -> QString {
  QJsonObject color_wheel;
  color_wheel.insert(QStringLiteral("version"), 2);
  for (const auto id : kAllWheels) {
    color_wheel.insert(WheelKey(id), WheelJson(wheel(id)));
  }
  QJsonObject root;
  root.insert(QStringLiteral("color_wheel"), color_wheel);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

auto EditorCdlTrackballModel::wheelDeltaText(const QString& wheel_key) const -> QString {
  const auto id = ParseWheelId(wheel_key);
  if (!id.has_value()) {
    return {};
  }
  return FormatWheelDeltaText(wheel(*id));
}

auto EditorCdlTrackballModel::ParseWheelId(const QString& wheel_key) -> std::optional<WheelId> {
  for (const auto id : kAllWheels) {
    if (wheel_key.compare(WheelKey(id), Qt::CaseInsensitive) == 0) {
      return id;
    }
  }
  return std::nullopt;
}

auto EditorCdlTrackballModel::wheelState(WheelId id) -> CdlWheelState& {
  return wheels_[static_cast<std::size_t>(id)];
}

void EditorCdlTrackballModel::recomputeDerived(WheelId id) { UpdateDerived(wheelState(id)); }

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
    case WheelId::Offset:
      emit offsetChanged();
      break;
  }
}

void EditorCdlTrackballModel::clearDrag() {
  if (!dragActive_) {
    return;
  }
  dragActive_     = false;
  draggingDisc_   = false;
  draggingMaster_ = false;
  dragMoved_      = false;
  emit dragActiveChanged();
}

void EditorCdlTrackballModel::applyDisc(WheelId id, double x, double y) {
  auto&      state = wheelState(id);
  const auto next  = color_wheel::ClampDiscPoint(QPointF(x, y));
  if (std::abs(next.x() - state.disc_position_.x()) < 1e-6 &&
      std::abs(next.y() - state.disc_position_.y()) < 1e-6) {
    return;
  }
  state.disc_position_ = next;
  recomputeDerived(id);
  emitWheel(id);
}

void EditorCdlTrackballModel::applyMaster(WheelId id, double master) {
  auto&       state = wheelState(id);
  const float next  = ClampMaster(master);
  if (std::abs(next - state.master_offset_) < 1e-6f) {
    return;
  }
  state.master_offset_ = next;
  emitWheel(id);
}

void EditorCdlTrackballModel::submitWheels(std::initializer_list<WheelId> ids, bool settled) {
  submitNow(colorWheelWrite(ids), settled);
}

auto EditorCdlTrackballModel::colorWheelWrite(std::initializer_list<WheelId> ids) const
    -> alcedo::ColorWheelUpdate {
  alcedo::ColorWheelUpdate update;
  for (const auto id : ids) {
    const auto control = WheelWrite(wheel(id));
    switch (id) {
      case WheelId::Lift:
        update.lift = control;
        break;
      case WheelId::Gamma:
        update.gamma = control;
        break;
      case WheelId::Gain:
        update.gain = control;
        break;
      case WheelId::Offset:
        update.offset = control;
        break;
    }
  }
  return update;
}

}  // namespace alcedo::ui
